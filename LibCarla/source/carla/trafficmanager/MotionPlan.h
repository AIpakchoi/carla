
/// This file has functionality for motion planning based on information
/// from localization, collision avoidance and traffic light response.

#include "carla/client/DebugHelper.h"
#include "carla/client/detail/EpisodeProxy.h"
#include "carla/rpc/VehicleControl.h"
#include "carla/rpc/Command.h"

#include "carla/trafficmanager/Constants.h"
#include "carla/trafficmanager/DataStructures.h"
#include "carla/trafficmanager/LocalizationUtils.h"
#include "carla/trafficmanager/Parameters.h"
#include "carla/trafficmanager/PIDController.h"

namespace carla
{
namespace traffic_manager
{

using namespace constants::MotionPlan;
using namespace constants::WaypointSelection;

using constants::SpeedThreshold::HIGHWAY_SPEED;
using constants::HybridMode::HYBRID_MODE_DT;

void MotionPlan(const unsigned long index,
                const std::vector<ActorId> &vehicle_id_list,
                const KinematicStateMap &state_map,
                const StaticAttributeMap &attribute_map,
                const Parameters &parameters,
                const BufferMapPtr &buffer_map,
                const std::vector<float> &urban_longitudinal_parameters,
                const std::vector<float> &highway_longitudinal_parameters,
                const std::vector<float> &urban_lateral_parameters,
                const std::vector<float> &highway_lateral_parameters,
                const CollisionFramePtr &collision_frame,
                const TLFramePtr &tl_frame,
                std::unordered_map<ActorId, StateEntry> &pid_state_map,
                std::unordered_map<ActorId, TimeInstance> &teleportation_instance,
                ControlFramePtr &output_array)
{
  const ActorId actor_id = vehicle_id_list.at(index);
  const KinematicState kinematic_state = state_map.at(actor_id);
  const cg::Location ego_location = kinematic_state.location;
  const cg::Vector3D ego_velocity = kinematic_state.velocity;
  const float ego_speed = ego_velocity.Length();
  const cg::Vector3D ego_heading = kinematic_state.rotation.GetForwardVector();
  const bool ego_physics_enabled = kinematic_state.physics_enabled;
  const Buffer &waypoint_buffer = buffer_map->at(actor_id);
  const CollisionHazardData &collision_hazard = collision_frame->at(index);
  const bool &tl_hazard = tl_frame->at(index);

  const float target_point_distance = std::max(ego_speed * TARGET_WAYPOINT_TIME_HORIZON,
                                               TARGET_WAYPOINT_HORIZON_LENGTH);
  const SimpleWaypointPtr &target_waypoint = GetTargetWaypoint(waypoint_buffer, target_point_distance).first;
  const cg::Location target_location = target_waypoint->GetLocation();
  float dot_product = DeviationDotProduct(ego_location, ego_heading, target_location);
  float cross_product = DeviationCrossProduct(ego_location, ego_heading, target_location);
  dot_product = 1.0f - dot_product;
  if (cross_product < 0.0f)
  {
    dot_product *= -1.0f;
  }
  const float current_deviation = dot_product;

  // If previous state for vehicle not found, initialize state entry.
  if (pid_state_map.find(actor_id) == pid_state_map.end())
  {
    const auto initial_state = StateEntry{0.0f, 0.0f, chr::system_clock::now(), 0.0f, 0.0f};
    pid_state_map.insert({actor_id, initial_state});
  }

  // Retrieving the previous state.
  traffic_manager::StateEntry previous_state;
  previous_state = pid_state_map.at(actor_id);

  // Select PID parameters.
  std::vector<float> longitudinal_parameters;
  std::vector<float> lateral_parameters;
  if (ego_speed > HIGHWAY_SPEED)
  {
    longitudinal_parameters = highway_longitudinal_parameters;
    lateral_parameters = highway_lateral_parameters;
  }
  else
  {
    longitudinal_parameters = urban_longitudinal_parameters;
    lateral_parameters = urban_lateral_parameters;
  }

  // Target velocity for vehicle.
  const float ego_speed_limit = attribute_map.at(actor_id).speed_limit;
  float max_target_velocity = parameters.GetVehicleTargetVelocity(actor_id, ego_speed_limit) / 3.6f;
  float dynamic_target_velocity = max_target_velocity;
  //////////////////////// Collision related data handling ///////////////////////////
  bool collision_emergency_stop = false;
  if (collision_hazard.hazard)
  {
    const ActorId other_actor_id = collision_hazard.hazard_actor_id;
    const KinematicState &other_kinematic_state = state_map.at(other_actor_id);
    const cg::Vector3D other_velocity = other_kinematic_state.velocity;
    const float ego_relative_speed = (ego_velocity - other_velocity).Length();
    const float available_distance_margin = collision_hazard.available_distance_margin;

    const float other_speed_along_heading = cg::Math::Dot(other_velocity, ego_heading);

    // Consider collision avoidance decisions only if there is positive relative velocity
    // of the ego vehicle (meaning, ego vehicle is closing the gap to the lead vehicle).
    if (ego_relative_speed > EPSILON_RELATIVE_SPEED)
    {
      // If other vehicle is approaching lead vehicle and lead vehicle is further
      // than follow_lead_distance 0 kmph -> 5m, 100 kmph -> 10m.
      float follow_lead_distance = ego_relative_speed * FOLLOW_DISTANCE_RATE + MIN_FOLLOW_LEAD_DISTANCE;
      if (available_distance_margin > follow_lead_distance)
      {
        // Then reduce the gap between the vehicles till FOLLOW_LEAD_DISTANCE
        // by maintaining a relative speed of RELATIVE_APPROACH_SPEED
        dynamic_target_velocity = other_speed_along_heading + RELATIVE_APPROACH_SPEED;
      }
      // If vehicle is approaching a lead vehicle and the lead vehicle is further
      // than CRITICAL_BRAKING_MARGIN but closer than FOLLOW_LEAD_DISTANCE.
      else if (available_distance_margin > CRITICAL_BRAKING_MARGIN)
      {
        // Then follow the lead vehicle by acquiring it's speed along current heading.
        dynamic_target_velocity = std::max(other_speed_along_heading, RELATIVE_APPROACH_SPEED);
      }
      else
      {
        // If lead vehicle closer than CRITICAL_BRAKING_MARGIN, initiate emergency stop.
        collision_emergency_stop = true;
      }
    }
    if (available_distance_margin < CRITICAL_BRAKING_MARGIN)
    {
      collision_emergency_stop = true;
    }
  }
  ///////////////////////////////////////////////////////////////////////////////////

  // Clip dynamic target velocity to maximum allowed speed for the vehicle.
  dynamic_target_velocity = std::min(max_target_velocity, dynamic_target_velocity);

  // In case of collision or traffic light hazard.
  bool emergency_stop = (tl_hazard || collision_emergency_stop);

  ActuationSignal actuation_signal{0.0f, 0.0f, 0.0f};
  cg::Transform teleportation_transform;

  // If physics is enabled for the vehicle, use PID controller.
  const auto current_time = chr::system_clock::now();
  StateEntry current_state;
  if (ego_physics_enabled)
  {

    // State update for vehicle.
    current_state = PID::StateUpdate(previous_state, ego_speed, dynamic_target_velocity,
                                     current_deviation, current_time);

    // Controller actuation.
    actuation_signal = PID::RunStep(current_state, previous_state,
                                    longitudinal_parameters, lateral_parameters);

    if (emergency_stop)
    {

      current_state.deviation_integral = 0.0f;
      current_state.velocity_integral = 0.0f;
      actuation_signal.throttle = 0.0f;
      actuation_signal.brake = 1.0f;
    }
  }
  // For physics-less vehicles, determine position and orientation for teleportation.
  else
  {
    // Flushing controller state for vehicle.
    current_state = {0.0f, 0.0f,
                     chr::system_clock::now(),
                     0.0f, 0.0f};

    // Add entry to teleportation duration clock table if not present.
    if (teleportation_instance.find(actor_id) == teleportation_instance.end())
    {
      teleportation_instance.insert({actor_id, chr::system_clock::now()});
    }

    // Measuring time elapsed since last teleportation for the vehicle.
    chr::duration<float> elapsed_time = current_time - teleportation_instance.at(actor_id);

    // Find a location ahead of the vehicle for teleportation to achieve intended velocity.
    if (!emergency_stop && (parameters.GetSynchronousMode() || elapsed_time.count() > HYBRID_MODE_DT))
    {

      // Target displacement magnitude to achieve target velocity.
      const float target_displacement = dynamic_target_velocity * HYBRID_MODE_DT;
      const SimpleWaypointPtr teleport_target_waypoint = GetTargetWaypoint(waypoint_buffer, target_displacement).first; 

      // Construct target transform to accurately achieve desired velocity.
      float missing_displacement = 0.0f;
      const float base_displacement = teleport_target_waypoint->Distance(ego_location);
      if (base_displacement < target_displacement)
      {
        missing_displacement = target_displacement - base_displacement;
      }
      cg::Transform target_base_transform = teleport_target_waypoint->GetTransform();
      cg::Location target_base_location = target_base_transform.location;
      cg::Vector3D target_heading = target_base_transform.GetForwardVector();
      cg::Location teleportation_location = target_base_location + cg::Location(target_heading * missing_displacement);
      teleportation_transform = cg::Transform(teleportation_location, target_base_transform.rotation);
    }
    // In case of an emergency stop, stay in the same location.
    // Also, teleport only once every dt in asynchronous mode.
    else
    {
      teleportation_transform = cg::Transform(ego_location, kinematic_state.rotation);
    }
  }

  // Updating PID state.
  StateEntry &state = pid_state_map.at(actor_id);
  state = current_state;

  // Constructing the actuation signal.
  if (ego_physics_enabled)
  {
    carla::rpc::VehicleControl vehicle_control;
    vehicle_control.throttle = actuation_signal.throttle;
    vehicle_control.brake = actuation_signal.brake;
    vehicle_control.steer = actuation_signal.steer;

    output_array->at(index) = carla::rpc::Command::ApplyVehicleControl(actor_id, vehicle_control);
  }
  else
  {
    output_array->at(index) = carla::rpc::Command::ApplyTransform(actor_id, teleportation_transform);
  }
}

} // namespace traffic_manager
} // namespace carla
