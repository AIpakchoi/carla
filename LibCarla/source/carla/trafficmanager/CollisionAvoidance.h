
/// This file has functionality to detect potential collision with a nearby actor.

#include <memory>

#include "carla/Memory.h"

#include "boost/geometry.hpp"
#include "boost/geometry/geometries/geometries.hpp"
#include "boost/geometry/geometries/point_xy.hpp"
#include "boost/geometry/geometries/polygon.hpp"
#include "carla/geom/Math.h"

#include "carla/trafficmanager/Constants.h"
#include "carla/trafficmanager/DataStructures.h"
#include "carla/trafficmanager/LocalizationUtils.h"
#include "carla/trafficmanager/Parameters.h"
#include "carla/trafficmanager/SimpleWaypoint.h"
#include "carla/trafficmanager/VehicleStateAndAttributeQuery.h"

namespace carla
{
namespace traffic_manager
{

struct GeometryComparison
{
  double reference_vehicle_to_other_geodesic;
  double other_vehicle_to_reference_geodesic;
  double inter_geodesic_distance;
  double inter_bbox_distance;
};

namespace bg = boost::geometry;

using Buffer = std::deque<std::shared_ptr<SimpleWaypoint>>;
using BufferMap = std::unordered_map<carla::ActorId, Buffer>;
using BufferMapPtr = std::shared_ptr<BufferMap>;
using LocationList = std::vector<cg::Location>;
using GeodesicBoundaryMap = std::unordered_map<ActorId, LocationList>;
using GeometryComparisonMap = std::unordered_map<std::string, GeometryComparison>;
using Point2D = bg::model::point<double, 2, bg::cs::cartesian>;
using Polygon = bg::model::polygon<bg::model::d2::point_xy<double>>;
using TLS = carla::rpc::TrafficLightState;

using namespace constants::Collision;
using constants::WaypointSelection::JUNCTION_LOOK_AHEAD;

/// Method to calculate the speed dependent bounding box extention for a vehicle.
float GetBoundingBoxExtention(const ActorId actor_id,
                              const KinematicState &kinematic_state,
                              const CollisionLockMap &collision_lock_map);

/// Returns the bounding box corners of the vehicle passed to the method.
LocationList GetBoundary(const KinematicState &kinematic_state, const StaticAttributes &attributes);

/// Returns the extrapolated bounding box of the vehicle along its trajectory.
LocationList GetGeodesicBoundary(const ActorId actor_id,
                                 GeodesicBoundaryMap &geodesic_boundary_map,
                                 const KinematicState &kinematic_state,
                                 const StaticAttributes &attributes,
                                 const Buffer &waypoint_buffer,
                                 const float specific_lead_distance,
                                 const CollisionLockMap &collision_lock_map);

/// Method to construct a boost polygon object.
Polygon GetPolygon(const LocationList &boundary);

/// Method to compute Geometry result between two vehicles
GeometryComparison GetGeometryBetweenActors(GeometryComparisonMap &geometry_cache,
                                            GeodesicBoundaryMap &geodesic_boundary_map,
                                            const ActorId reference_vehicle_id,
                                            const ActorId other_actor_id,
                                            const KinematicState &reference_vehicle_state,
                                            const KinematicState &other_vehicle_state,
                                            const StaticAttributes &reference_vehicle_attributes,
                                            const StaticAttributes &other_vehicle_attributes,
                                            const Buffer &reference_vehicle_buffer,
                                            const Buffer &other_vehicle_buffer,
                                            const CollisionLockMap &collision_lock_map,
                                            const float reference_lead_distance,
                                            const float other_lead_distance);

/// The method returns true if ego_vehicle should stop and wait for
/// other_vehicle to pass along with available distance margin.
std::pair<bool, float> NegotiateCollision(const ActorId reference_vehicle_id,
                                          const ActorId other_actor_id,
                                          GeometryComparisonMap &geometry_cache,
                                          GeodesicBoundaryMap &geodesic_boundary_map,
                                          CollisionLockMap &collision_locks,
                                          const KinematicState &reference_vehicle_state,
                                          const KinematicState &other_vehicle_state,
                                          const StaticAttributes &reference_vehicle_attributes,
                                          const StaticAttributes &other_vehicle_attributes,
                                          const TrafficLightState &reference_tl_state,
                                          const Buffer &reference_vehicle_buffer,
                                          const Buffer &other_vehicle_buffer,
                                          const uint64_t reference_junction_look_ahead_index,
                                          const float reference_lead_distance,
                                          const float other_lead_distance);

void CollisionAvoidance(const unsigned long index,
                        const std::vector<ActorId> &vehicle_id_list,
                        const KinematicStateMap &state_map,
                        const StaticAttributeMap &attribute_map,
                        const TrafficLightStateMap &tl_state_map,
                        const BufferMapPtr &buffer_map,
                        const TrackTraffic &track_traffic,
                        const Parameters &parameters,
                        CollisionLockMap &collision_locks,
                        CollisionFramePtr &output_array)
{
  GeodesicBoundaryMap geodesic_boundary_map;
  GeometryComparisonMap geometry_cache;

  ActorId obstacle_id = 0u;
  bool collision_hazard = false;
  float available_distance_margin = std::numeric_limits<float>::infinity();

  const ActorId ego_actor_id = vehicle_id_list.at(index);
  if (state_map.find(ego_actor_id) != state_map.end()
      && attribute_map.find(ego_actor_id) != attribute_map.end())
  {
    const KinematicState &ego_kinematic_state = state_map.at(ego_actor_id);
    const StaticAttributes &ego_attributes = attribute_map.at(ego_actor_id);
    const cg::Location ego_location = ego_kinematic_state.location;
    const Buffer &ego_buffer = buffer_map->at(ego_actor_id);
    const uint64_t look_ahead_index = GetTargetWaypoint(ego_buffer, JUNCTION_LOOK_AHEAD).second;

    ActorIdSet overlapping_actors = track_traffic.GetOverlappingVehicles(ego_actor_id);
    std::vector<ActorId> collision_candidate_ids;

    // Run through vehicles with overlapping paths and filter them;
    float collision_radius_square = SQUARE(MAX_COLLISION_RADIUS);
    for (ActorId overlapping_actor_id : overlapping_actors)
    {
      // If actor is within maximum collision avoidance and vertical overlap range.
      const cg::Location &overlapping_actor_location = GetLocation(state_map, overlapping_actor_id);
      if (overlapping_actor_id != ego_actor_id
          && cg::Math::DistanceSquared(overlapping_actor_location, ego_location) < collision_radius_square
          && std::abs(ego_location.z - overlapping_actor_location.z) < VERTICAL_OVERLAP_THRESHOLD)
      {
        collision_candidate_ids.push_back(overlapping_actor_id);
      }
    }

    // Sorting collision candidates in accending order of distance to current vehicle.
    std::sort(collision_candidate_ids.begin(), collision_candidate_ids.end(),
              [&state_map, &ego_location](const ActorId &a_id_1, const ActorId &a_id_2) {
                const cg::Location &e_loc = ego_location;
                const cg::Location &loc_1 = GetLocation(state_map, a_id_1);
                const cg::Location &loc_2 = GetLocation(state_map, a_id_2);
                return (cg::Math::DistanceSquared(e_loc, loc_1) < cg::Math::DistanceSquared(e_loc, loc_2));
              });

    const float reference_lead_distance = parameters.GetDistanceToLeadingVehicle(ego_actor_id);

    // Check every actor in the vicinity if it poses a collision hazard.
    for (auto iter = collision_candidate_ids.begin();
        iter != collision_candidate_ids.end() && !collision_hazard;
        ++iter)
    {
      const ActorId other_actor_id = *iter;
      const ActorType other_actor_type = GetType(attribute_map, other_actor_id);
      const KinematicState &other_kinematic_state = state_map.at(other_actor_id);
      const StaticAttributes &other_attributes = attribute_map.at(other_actor_id);

      if (parameters.GetCollisionDetection(ego_actor_id, other_actor_id)
          && tl_state_map.find(ego_actor_id) != tl_state_map.end()
          && buffer_map->find(ego_actor_id) != buffer_map->end()
          && buffer_map->find(other_actor_id) != buffer_map->end())
      {
        const float other_lead_distance = parameters.GetDistanceToLeadingVehicle(other_actor_id);
        std::pair<bool, float> negotiation_result = NegotiateCollision(ego_actor_id,
                                                                      other_actor_id,
                                                                      geometry_cache,
                                                                      geodesic_boundary_map,
                                                                      collision_locks,
                                                                      ego_kinematic_state,
                                                                      other_kinematic_state,
                                                                      ego_attributes,
                                                                      other_attributes,
                                                                      tl_state_map.at(ego_actor_id),
                                                                      buffer_map->at(ego_actor_id),
                                                                      buffer_map->at(other_actor_id),
                                                                      look_ahead_index,
                                                                      reference_lead_distance,
                                                                      other_lead_distance);
        if (negotiation_result.first)
        {
          if ((other_actor_type == ActorType::Vehicle
              && parameters.GetPercentageIgnoreVehicles(ego_actor_id) <= (rand() % 101))
              || (other_actor_type == ActorType::Pedestrian
                  && parameters.GetPercentageIgnoreWalkers(ego_actor_id) <= (rand() % 101)))
          {
            collision_hazard = true;
            obstacle_id = other_actor_id;
            available_distance_margin = negotiation_result.second;
          }
        }
      }
    }
  }

  CollisionHazardData &output_element = output_array->at(index);
  output_element.hazard_actor_id = obstacle_id;
  output_element.hazard = collision_hazard;
  output_element.available_distance_margin = available_distance_margin;
}

/// Method to calculate the speed dependent bounding box extention for a vehicle.
float GetBoundingBoxExtention(const ActorId actor_id,
                              const KinematicState &kinematic_state,
                              const CollisionLockMap &collision_lock_map)
{

  const float velocity = cg::Math::Dot(kinematic_state.velocity, kinematic_state.rotation.GetForwardVector());
  float bbox_extension;
  // Using a linear function to calculate boundary length.
  bbox_extension = BOUNDARY_EXTENSION_RATE * velocity + BOUNDARY_EXTENSION_MINIMUM;
  // If a valid collision lock present, change boundary length to maintain lock.
  if (collision_lock_map.find(actor_id) != collision_lock_map.end())
  {
    const CollisionLock &lock = collision_lock_map.at(actor_id);
    float lock_boundary_length = static_cast<float>(lock.distance_to_lead_vehicle
                                                    + LOCKING_DISTANCE_PADDING);
    // Only extend boundary track vehicle if the leading vehicle
    // if it is not further than velocity dependent extension by MAX_LOCKING_EXTENSION.
    if ((lock_boundary_length - lock.initial_lock_distance) < MAX_LOCKING_EXTENSION)
    {
      bbox_extension = lock_boundary_length;
    }
  }

  return bbox_extension;
}

LocationList GetBoundary(const KinematicState &kinematic_state, const StaticAttributes &attributes)
{
  const ActorType actor_type = attributes.actor_type;
  const cg::Vector3D heading_vector = kinematic_state.rotation.GetForwardVector();

  float forward_extension = 0.0f;
  if (actor_type == ActorType::Pedestrian) {
    // Extend the pedestrians bbox to "predict" where they'll be and avoid collisions.
    forward_extension = kinematic_state.velocity.Length() * WALKER_TIME_EXTENSION;
  }

  float bbox_x = attributes.half_length;
  float bbox_y = attributes.half_width;

  const cg::Vector3D x_boundary_vector = heading_vector * (bbox_x + forward_extension);
  const auto perpendicular_vector = cg::Vector3D(-heading_vector.y, heading_vector.x, 0.0f).MakeUnitVector();
  const cg::Vector3D y_boundary_vector = perpendicular_vector * (bbox_y + forward_extension);

  // Four corners of the vehicle in top view clockwise order (left-handed system).
  const cg::Location location = kinematic_state.location;
  LocationList bbox_boundary = {
    location + cg::Location(x_boundary_vector - y_boundary_vector),
    location + cg::Location(-1.0f * x_boundary_vector - y_boundary_vector),
    location + cg::Location(-1.0f * x_boundary_vector + y_boundary_vector),
    location + cg::Location(x_boundary_vector + y_boundary_vector),
  };

  return bbox_boundary;
}

LocationList GetGeodesicBoundary(const ActorId actor_id,
                                 GeodesicBoundaryMap &geodesic_boundary_map,
                                 const KinematicState &kinematic_state,
                                 const StaticAttributes &attributes,
                                 const Buffer &waypoint_buffer,
                                 const float specific_lead_distance,
                                 const CollisionLockMap &collision_lock_map)
{
  LocationList geodesic_boundary;

  if (geodesic_boundary_map.find(actor_id) != geodesic_boundary_map.end()) {

    geodesic_boundary = geodesic_boundary_map.at(actor_id);

  } else {

    const LocationList bbox = GetBoundary(kinematic_state, attributes);

    if (attributes.actor_type == ActorType::Vehicle) {

      float bbox_extension = GetBoundingBoxExtention(actor_id, kinematic_state, collision_lock_map);
      bbox_extension = MAX(specific_lead_distance, bbox_extension);
      const float bbox_extension_square = SQUARE(bbox_extension);

      LocationList left_boundary;
      LocationList right_boundary;
      const float width = attributes.half_width;
      const float length = attributes.half_length;

      const TargetWPInfo target_wp_info = GetTargetWaypoint(waypoint_buffer, length);
      const SimpleWaypointPtr boundary_start = target_wp_info.first;
      const uint64_t boundary_start_index = target_wp_info.second;

      // At non-signalized junctions, we extend the boundary across the junction
      // and in all other situations, boundary length is velocity-dependent.
      SimpleWaypointPtr boundary_end = nullptr;
      SimpleWaypointPtr current_point = waypoint_buffer.at(boundary_start_index);
      bool reached_distance = false;
      for (uint64_t j = boundary_start_index; !reached_distance && (j < waypoint_buffer.size()); ++j) {

        if (boundary_start->DistanceSquared(current_point) > bbox_extension_square
            || j == waypoint_buffer.size() - 1) {
          reached_distance = true;
        }

        if (boundary_end == nullptr
            || cg::Math::Dot(boundary_end->GetForwardVector(), current_point->GetForwardVector()) < COS_10_DEGREES
            || reached_distance) {

          const cg::Vector3D heading_vector = current_point->GetForwardVector();
          const cg::Location location = current_point->GetLocation();
          cg::Vector3D perpendicular_vector = cg::Vector3D(-heading_vector.y, heading_vector.x, 0.0f);
          perpendicular_vector = perpendicular_vector.MakeUnitVector();
          // Direction determined for the left-handed system.
          const cg::Vector3D scaled_perpendicular = perpendicular_vector * width;
          left_boundary.push_back(location + cg::Location(scaled_perpendicular));
          right_boundary.push_back(location + cg::Location(-1.0f * scaled_perpendicular));

          boundary_end = current_point;
        }

        current_point = waypoint_buffer.at(j);
      }

      // Reversing right boundary to construct clockwise (left-hand system)
      // boundary. This is so because both left and right boundary vectors have
      // the closest point to the vehicle at their starting index for the right
      // boundary,
      // we want to begin at the farthest point to have a clockwise trace.
      std::reverse(right_boundary.begin(), right_boundary.end());
      geodesic_boundary.insert(geodesic_boundary.end(), right_boundary.begin(), right_boundary.end());
      geodesic_boundary.insert(geodesic_boundary.end(), bbox.begin(), bbox.end());
      geodesic_boundary.insert(geodesic_boundary.end(), left_boundary.begin(), left_boundary.end());

    } else {

      geodesic_boundary = bbox;
    }

    geodesic_boundary_map.insert({actor_id, geodesic_boundary});
  }

  return geodesic_boundary;
}

Polygon GetPolygon(const LocationList &boundary) {

  traffic_manager::Polygon boundary_polygon;
  for (const cg::Location &location: boundary) {
    bg::append(boundary_polygon.outer(), Point2D(location.x, location.y));
  }
  bg::append(boundary_polygon.outer(), Point2D(boundary.front().x, boundary.front().y));

  return boundary_polygon;
}

GeometryComparison GetGeometryBetweenActors(GeometryComparisonMap &geometry_cache,
                                            GeodesicBoundaryMap &geodesic_boundary_map,
                                            const ActorId reference_vehicle_id,
                                            const ActorId other_actor_id,
                                            const KinematicState &reference_vehicle_state,
                                            const KinematicState &other_vehicle_state,
                                            const StaticAttributes &reference_vehicle_attributes,
                                            const StaticAttributes &other_vehicle_attributes,
                                            const Buffer &reference_vehicle_buffer,
                                            const Buffer &other_vehicle_buffer,
                                            const CollisionLockMap &collision_lock_map,
                                            const float reference_lead_distance,
                                            const float other_lead_distance)
{

  std::string actor_id_key = reference_vehicle_id < other_actor_id
                             ? std::to_string(reference_vehicle_id) + "|" + std::to_string(other_actor_id)
                             : std::to_string(other_actor_id) + "|" + std::to_string(other_actor_id);

  GeometryComparison mCache{-1,-1,-1,-1};

  if (geometry_cache.find(actor_id_key) != geometry_cache.end()) {

    mCache = geometry_cache.at(actor_id_key);
    double mref_veh_other = mCache.reference_vehicle_to_other_geodesic;
    mCache.reference_vehicle_to_other_geodesic = mCache.other_vehicle_to_reference_geodesic;
    mCache.other_vehicle_to_reference_geodesic = mref_veh_other;

  } else {

    const Polygon reference_polygon = GetPolygon(GetBoundary(reference_vehicle_state, reference_vehicle_attributes));
    const Polygon other_polygon = GetPolygon(GetBoundary(other_vehicle_state, other_vehicle_attributes));

    const Polygon reference_geodesic_polygon = GetPolygon(GetGeodesicBoundary(reference_vehicle_id,
                                                                              geodesic_boundary_map,
                                                                              reference_vehicle_state,
                                                                              reference_vehicle_attributes,
                                                                              reference_vehicle_buffer,
                                                                              reference_lead_distance,
                                                                              collision_lock_map));

    const Polygon other_geodesic_polygon = GetPolygon(GetGeodesicBoundary(other_actor_id,
                                                                          geodesic_boundary_map,
                                                                          other_vehicle_state,
                                                                          other_vehicle_attributes,
                                                                          other_vehicle_buffer,
                                                                          other_lead_distance,
                                                                          collision_lock_map));

    const double reference_vehicle_to_other_geodesic = bg::distance(reference_polygon, other_geodesic_polygon);
    const double other_vehicle_to_reference_geodesic = bg::distance(other_polygon, reference_geodesic_polygon);
    const auto inter_geodesic_distance = bg::distance(reference_geodesic_polygon, other_geodesic_polygon);
    const auto inter_bbox_distance = bg::distance(reference_polygon, other_polygon);

    mCache = {reference_vehicle_to_other_geodesic,
              other_vehicle_to_reference_geodesic,
              inter_geodesic_distance,
              inter_bbox_distance};

    geometry_cache.insert({actor_id_key, mCache});

  }

  return mCache;
}

std::pair<bool, float> NegotiateCollision(const ActorId reference_vehicle_id,
                                          const ActorId other_actor_id,
                                          GeometryComparisonMap &geometry_cache,
                                          GeodesicBoundaryMap &geodesic_boundary_map,
                                          CollisionLockMap &collision_locks,
                                          const KinematicState &reference_vehicle_state,
                                          const KinematicState &other_vehicle_state,
                                          const StaticAttributes &reference_vehicle_attributes,
                                          const StaticAttributes &other_vehicle_attributes,
                                          const TrafficLightState &reference_tl_state,
                                          const Buffer &reference_vehicle_buffer,
                                          const Buffer &other_vehicle_buffer,
                                          const uint64_t reference_junction_look_ahead_index,
                                          const float reference_lead_distance,
                                          const float other_lead_distance)
{

  // Output variables for the method.
  bool hazard = false;
  float available_distance_margin = std::numeric_limits<float>::infinity();

  const cg::Location reference_location = reference_vehicle_state.location;
  const cg::Location other_location = other_vehicle_state.location;

  // Ego and other vehicle heading.
  const cg::Vector3D reference_heading = reference_vehicle_state.rotation.GetForwardVector();
  // Vector from ego position to position of the other vehicle.
  cg::Vector3D reference_to_other = other_location - reference_location;
  float reference_to_other_magnitude = reference_to_other.Length();
  if (reference_to_other_magnitude > 2.0f * std::numeric_limits<float>::epsilon()) {
    reference_to_other /= reference_to_other_magnitude;
  }

  // Other vehicle heading.
  const cg::Vector3D other_heading = reference_vehicle_state.rotation.GetForwardVector();
  // Vector from other vehicle position to ego position.
  cg::Vector3D other_to_reference = reference_location - other_location;
  float other_to_reference_magnitude = other_to_reference.Length();
  if (other_to_reference_magnitude > 2.0f * std::numeric_limits<float>::epsilon()) {
    other_to_reference /= other_to_reference_magnitude;
  }

  float reference_vehicle_length = reference_vehicle_attributes.half_length * SQUARE_ROOT_OF_TWO;
  float other_vehicle_length = other_vehicle_attributes.half_length * SQUARE_ROOT_OF_TWO;

  float inter_vehicle_distance = cg::Math::DistanceSquared(reference_location, other_location);
  float ego_bounding_box_extension = GetBoundingBoxExtention(reference_vehicle_id,
                                                             reference_vehicle_state, collision_locks);
  float other_bounding_box_extension = GetBoundingBoxExtention(other_actor_id, other_vehicle_state, collision_locks);
  // Calculate minimum distance between vehicle to consider collision negotiation.
  float inter_vehicle_length = reference_vehicle_length + other_vehicle_length;
  float ego_detection_range = SQUARE(ego_bounding_box_extension + inter_vehicle_length);
  float cross_detection_range = SQUARE(ego_bounding_box_extension
                                       + inter_vehicle_length
                                       + other_bounding_box_extension);

  // Conditions to consider collision negotiation.
  bool other_vehicle_in_ego_range = inter_vehicle_distance < ego_detection_range;
  bool other_vehicles_in_cross_detection_range = inter_vehicle_distance < cross_detection_range;
  bool other_vehicle_in_front = cg::Math::Dot(reference_heading, reference_to_other) > 0;
  SimpleWaypointPtr closest_point = reference_vehicle_buffer.front();
  bool ego_inside_junction = closest_point->CheckJunction();
  bool ego_at_traffic_light = reference_tl_state.at_traffic_light;
  bool ego_stopped_by_light = reference_tl_state.tl_state != TLS::Green;
  SimpleWaypointPtr look_ahead_point = reference_vehicle_buffer.at(reference_junction_look_ahead_index);
  bool ego_at_junction_entrance = !closest_point->CheckJunction() && look_ahead_point->CheckJunction();

  // Conditions to consider collision negotiation.
  if (!(ego_at_junction_entrance && ego_at_traffic_light && ego_stopped_by_light)
      && ((ego_inside_junction && other_vehicles_in_cross_detection_range)
          || (!ego_inside_junction && other_vehicle_in_front && other_vehicle_in_ego_range))) {

    GeometryComparison geometry_comparison = GetGeometryBetweenActors(geometry_cache,
                                                                      geodesic_boundary_map,
                                                                      reference_vehicle_id,
                                                                      other_actor_id,
                                                                      reference_vehicle_state,
                                                                      other_vehicle_state,
                                                                      reference_vehicle_attributes,
                                                                      other_vehicle_attributes,
                                                                      reference_vehicle_buffer,
                                                                      other_vehicle_buffer,
                                                                      collision_locks,
                                                                      reference_lead_distance,
                                                                      other_lead_distance);

    // Conditions for collision negotiation.
    bool geodesic_path_bbox_touching = geometry_comparison.inter_geodesic_distance < 0.1;
    bool vehicle_bbox_touching = geometry_comparison.inter_bbox_distance < 0.1;
    bool ego_path_clear = geometry_comparison.other_vehicle_to_reference_geodesic > 0.1;
    bool other_path_clear = geometry_comparison.reference_vehicle_to_other_geodesic > 0.1;
    bool ego_path_priority = geometry_comparison.reference_vehicle_to_other_geodesic
                             < geometry_comparison.other_vehicle_to_reference_geodesic;
    bool ego_angular_priority = cg::Math::Dot(reference_heading, reference_to_other)
                                < cg::Math::Dot(other_heading, other_to_reference);

    // Whichever vehicle's path is farthest away from the other vehicle gets priority to move.
    if (geodesic_path_bbox_touching
        && ((!vehicle_bbox_touching
          && (!ego_path_clear || (ego_path_clear && other_path_clear && !ego_angular_priority && !ego_path_priority)))
            || (vehicle_bbox_touching && !ego_angular_priority && !ego_path_priority))) {

      hazard = true;

      const float specific_distance_margin = MAX(reference_lead_distance, BOUNDARY_EXTENSION_MINIMUM);
      available_distance_margin = static_cast<float>(MAX(geometry_comparison.reference_vehicle_to_other_geodesic
                                                         - specific_distance_margin, 0.0));

      ///////////////////////////////////// Collision locking mechanism /////////////////////////////////
      // The idea is, when encountering a possible collision,
      // we should ensure that the bounding box extension doesn't decrease too fast and loose collision tracking.
      // This enables us to smoothly approach the lead vehicle.

      // When possible collision found, check if an entry for collision lock present.
      if (collision_locks.find(reference_vehicle_id) != collision_locks.end()) {
        CollisionLock &lock = collision_locks.at(reference_vehicle_id);
        // Check if the same vehicle is under lock.
        if (other_actor_id == lock.lead_vehicle_id) {
          // If the body of the lead vehicle is touching the reference vehicle bounding box.
          if (geometry_comparison.other_vehicle_to_reference_geodesic < 0.1) {
            // Distance between the bodies of the vehicles.
            lock.distance_to_lead_vehicle = geometry_comparison.inter_bbox_distance;
          } else {
            // Distance from reference vehicle body to other vehicle path polygon.
            lock.distance_to_lead_vehicle = geometry_comparison.reference_vehicle_to_other_geodesic;
          }
        } else {
          // If possible collision with a new vehicle, re-initialize with new lock entry.
          lock = {other_actor_id, geometry_comparison.inter_bbox_distance, geometry_comparison.inter_bbox_distance};
        }
      } else {
        // Insert and initialize lock entry if not present.
        collision_locks.insert({reference_vehicle_id,
                                {other_actor_id,
                                 geometry_comparison.inter_bbox_distance,
                                 geometry_comparison.inter_bbox_distance}});
      }
    }
  }

  // If no collision hazard detected, then flush collision lock held by the vehicle.
  if (!hazard && collision_locks.find(reference_vehicle_id) != collision_locks.end()) {
    collision_locks.erase(reference_vehicle_id);
  }

  return {hazard, available_distance_margin};
}

} // namespace traffic_manager
} // namespace carla
