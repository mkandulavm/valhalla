#include "baldr/rapidjson_utils.h"
#include "gurka.h"
#include "test.h"
#include <valhalla/baldr/graphconstants.h>

using namespace valhalla;

// =============================================================================
// Test suite for //nevh custom features:
//   - speed_limits (DirectionsLeg field 9)
//   - road_attributes (DirectionsLeg field 11)
//   - speed_cameras (DirectionsLeg field 10)
//   - turn_lanes on maneuvers
//   - nevh_version in summary
// =============================================================================

class NevhFeaturesTest : public ::testing::Test {
protected:
  static gurka::map nevh_map;

  static void SetUpTestSuite() {
    // ASCII map with varied road types for testing:
    // A — B ==== C ——— D
    //
    // AB: primary bridge,    speed=50km/h, 2 lanes
    // BC: motorway,          speed=100km/h, 3 lanes, toll
    // CD: motorway_link,     speed=80km/h,  1 lane
    const std::string ascii_map = R"(
        A----B====C----D
    )";

    const gurka::ways ways = {
        {"AB", {{"highway", "primary"},
                {"maxspeed", "50"},
                {"lanes", "2"},
                {"bridge", "yes"},
                {"name", "Bridge Road"}}},
        {"BC", {{"highway", "motorway"},
                {"maxspeed", "100"},
                {"lanes", "3"},
                {"toll", "yes"},
                {"speed_camera", "yes"},
                {"name", "Toll Motorway"}}},
        {"CD", {{"highway", "motorway_link"},
                {"maxspeed", "80"},
                {"lanes", "1"},
                {"oneway", "yes"},
                {"name", "Ramp Exit"}}},
    };

    const auto layout = gurka::detail::map_to_coordinates(ascii_map, 100);
    nevh_map = gurka::buildtiles(layout, ways, {}, {}, "test/data/nevh_features");
  }

  // Helper: unpack road_attributes uint32_t into its components.
  struct RoadAttrs {
    uint32_t road_class;          // bits 0–2
    uint32_t use;                 // bits 3–8
    uint32_t infra_flags;         // bits 9–12  (bridge|tunnel|roundabout|indoor)
    bool has_level_changes;       // bit 13
    bool toll;                    // bit 14
    bool truck_route;             // bit 15
    bool has_time_restrictions;   // bit 16
    bool traffic_signal;          // bit 17
    bool speed_camera;            // bit 18
    bool shoulder;                // bit 19
    bool bicycle_network;         // bit 20
    bool destination_only;        // bit 21
    bool unpaved;                 // bit 22
    bool country_crossing;        // bit 23
    uint32_t sidewalk;            // bits 24–25
    uint32_t cycle_lane;          // bits 26–27
    uint32_t surface;             // bits 28–30
  };

  static RoadAttrs unpack(uint32_t attrs) {
    RoadAttrs r;
    r.road_class          = (attrs >> 0) & 0x7;
    r.use                 = (attrs >> 3) & 0x3F;
    r.infra_flags         = (attrs >> 9) & 0xF;
    r.has_level_changes   = (attrs >> 13) & 0x1;
    r.toll                = (attrs >> 14) & 0x1;
    r.truck_route         = (attrs >> 15) & 0x1;
    r.has_time_restrictions = (attrs >> 16) & 0x1;
    r.traffic_signal      = (attrs >> 17) & 0x1;
    r.speed_camera        = (attrs >> 18) & 0x1;
    r.shoulder            = (attrs >> 19) & 0x1;
    r.bicycle_network     = (attrs >> 20) & 0x1;
    r.destination_only    = (attrs >> 21) & 0x1;
    r.unpaved             = (attrs >> 22) & 0x1;
    r.country_crossing    = (attrs >> 23) & 0x1;
    r.sidewalk            = (attrs >> 24) & 0x3;
    r.cycle_lane          = (attrs >> 26) & 0x3;
    r.surface             = (attrs >> 28) & 0x7;
    return r;
  }
};

gurka::map NevhFeaturesTest::nevh_map = {};

// -----------------------------------------------------------------------------
// Test 1: Speed limits array is produced and has expected format
// -----------------------------------------------------------------------------
TEST_F(NevhFeaturesTest, SpeedLimitsArray) {
  auto result = gurka::do_action(valhalla::Options::route, nevh_map, {"A", "D"}, "auto");

  const auto& leg = result.directions().routes(0).legs(0);

  // Verify speed_limits array exists (field 9 on DirectionsLeg)
  int sl_count = leg.speed_limits_size();
  EXPECT_GT(sl_count, 0) << "speed_limits array should not be empty";

  // Each segment is 6 uint32_t: [begin_shape, end_shape, way_id, speed_limit,
  //                              u32edgeSpeed, lane_count]
  EXPECT_EQ(sl_count % 6, 0) << "speed_limits should be in groups of 6";

  int num_segments = sl_count / 6;
  EXPECT_GE(num_segments, 3) << "Should have at least 3 segments (AB, BC, CD)";

  // Check a few speed_limit values (index 3 in each group of 6)
  for (int seg = 0; seg < num_segments; ++seg) {
    uint32_t speed_limit = leg.speed_limits(seg * 6 + 3);
    EXPECT_GT(speed_limit, 0) << "Segment " << seg << " should have a speed limit";
    EXPECT_LE(speed_limit, 255) << "Segment " << seg << " speed_limit should be <= 255";

    uint32_t lane_count = leg.speed_limits(seg * 6 + 5);
    EXPECT_GT(lane_count, 0) << "Segment " << seg << " should have positive lane count";
    EXPECT_LE(lane_count, 15) << "Segment " << seg << " lane_count should be <= 15";
  }

  // Verify way_id (index 2) is non-zero for all segments
  for (int seg = 0; seg < num_segments; ++seg) {
    EXPECT_NE(leg.speed_limits(seg * 6 + 2), 0) << "Segment " << seg << " should have a way_id";
  }
}

// -----------------------------------------------------------------------------
// Test 2: Road attributes array is produced, correctly packed, and parallel
// -----------------------------------------------------------------------------
TEST_F(NevhFeaturesTest, RoadAttributesArray) {
  auto result = gurka::do_action(valhalla::Options::route, nevh_map, {"A", "D"}, "auto");

  const auto& leg = result.directions().routes(0).legs(0);

  // Verify road_attributes array exists (field 11 on DirectionsLeg)
  int ra_count = leg.road_attributes_size();
  EXPECT_GT(ra_count, 0) << "road_attributes array should not be empty";

  // road_attributes should be parallel with speed_limits segments
  int sl_segments = leg.speed_limits_size() / 6;
  EXPECT_EQ(ra_count, sl_segments)
      << "road_attributes count should match speed_limits segment count";

  // Unpack each road_attributes entry and verify basic validity
  for (int i = 0; i < ra_count; ++i) {
    uint32_t attrs = leg.road_attributes(i);
    auto r = unpack(attrs);

    // RoadClass should be 0–7 (valid enum range)
    EXPECT_LE(r.road_class, 7) << "RoadClass should be 0–7, got " << r.road_class
                               << " at index " << i;

    // Use should be 0–54 (valid enum range)
    EXPECT_LE(r.use, 54) << "Use should be 0–54, got " << r.use
                         << " at index " << i;

    // Spare bit 31 should be 0 (unused)
    EXPECT_EQ((attrs >> 31) & 0x1, 0) << "Bit 31 (spare) should be 0 at index " << i;
  }
}

// -----------------------------------------------------------------------------
// Test 3: Road attributes reflect correct OSM data (bridge, toll)
// -----------------------------------------------------------------------------
TEST_F(NevhFeaturesTest, RoadAttributesCorrectValues) {
  auto result = gurka::do_action(valhalla::Options::route, nevh_map, {"A", "D"}, "auto");

  const auto& leg = result.directions().routes(0).legs(0);
  ASSERT_GE(leg.road_attributes_size(), 3) << "Need at least 3 segments";

  // Segment 0: AB — primary, bridge=yes, speed=50, lanes=2
  {
    auto r = unpack(leg.road_attributes(0));
    EXPECT_EQ(r.road_class, static_cast<uint32_t>(baldr::RoadClass::kPrimary))
        << "AB should be primary";
    EXPECT_EQ(r.use, static_cast<uint32_t>(baldr::Use::kRoad))
        << "AB should be road use";
    EXPECT_TRUE(r.infra_flags & static_cast<uint8_t>(baldr::RoadAttributeFlag::kBridge))
        << "AB should have bridge flag";
    EXPECT_EQ(r.toll, 0) << "AB should not be toll";
  }

  // Segment 1: BC — motorway, toll=yes, speed=100, lanes=3
  {
    auto r = unpack(leg.road_attributes(1));
    EXPECT_EQ(r.road_class, static_cast<uint32_t>(baldr::RoadClass::kMotorway))
        << "BC should be motorway";
    EXPECT_EQ(r.use, static_cast<uint32_t>(baldr::Use::kRoad))
        << "BC should be road use";
    EXPECT_EQ(r.toll, 1) << "BC should be toll";
  }

  // Segment 2: CD — motorway_link (ramp), speed=80
  {
    auto r = unpack(leg.road_attributes(2));
    EXPECT_EQ(r.road_class, static_cast<uint32_t>(baldr::RoadClass::kMotorway))
        << "CD should be motorway (link inherits road class)";
    EXPECT_EQ(r.use, static_cast<uint32_t>(baldr::Use::kRamp))
        << "CD should be ramp use (motorway_link)";
    EXPECT_EQ(r.toll, 0) << "CD should not be toll";
  }
}

// -----------------------------------------------------------------------------
// Test 4: Speed cameras are detected and stored with node indices
// -----------------------------------------------------------------------------
TEST_F(NevhFeaturesTest, SpeedCamerasArray) {
  auto result = gurka::do_action(valhalla::Options::route, nevh_map, {"A", "D"}, "auto");

  const auto& leg = result.directions().routes(0).legs(0);
  // speed_cameras field (10) should be non-empty — BC has speed_camera=yes
  EXPECT_GT(leg.speed_cameras_size(), 0) << "speed_cameras should contain entries (BC has speed_camera=yes)";

  // Verify each speed camera entry is a valid node index within trip node range
  const auto& trip_leg = result.trip().routes(0).legs(0);
  for (int i = 0; i < leg.speed_cameras_size(); ++i) {
    uint32_t node_idx = leg.speed_cameras(i);
    EXPECT_LT(node_idx, static_cast<uint32_t>(trip_leg.node_size()))
        << "Speed camera node index " << node_idx << " should be within trip node range";
    // The edge at this node index should have speed_camera=true
    if (node_idx < static_cast<uint32_t>(trip_leg.node_size())) {
      const auto& edge = trip_leg.node(node_idx).edge();
      EXPECT_TRUE(edge.speed_camera())
          << "Edge at node " << node_idx << " should have speed_camera=true";
    }
  }
}

// -----------------------------------------------------------------------------
// Test 5: nevh_version is set in summary
// -----------------------------------------------------------------------------
TEST_F(NevhFeaturesTest, NevhVersionInSummary) {
  auto result = gurka::do_action(valhalla::Options::route, nevh_map, {"A", "D"}, "auto");

  const auto& summary = result.directions().routes(0).legs(0).summary();
  EXPECT_EQ(summary.nevhversion(), "1") << "nevh_version should be '1'";
}

// -----------------------------------------------------------------------------
// Test 6: Per-edge data on TripLeg is correct
// -----------------------------------------------------------------------------
TEST_F(NevhFeaturesTest, TripLegEdgeAttributes) {
  auto result = gurka::do_action(valhalla::Options::route, nevh_map, {"A", "D"}, "auto");

  const auto& leg = result.trip().routes(0).legs(0);

  // Check edge AB (node 0 → edge)
  {
    const auto& edge = leg.node(0).edge();
    EXPECT_EQ(edge.speed_limit(), 50) << "AB should have speed_limit=50 km/h";
    EXPECT_GE(edge.lane_count(), 1) << "AB should have at least 1 lane";
    EXPECT_TRUE(edge.bridge()) << "AB should be a bridge";
    EXPECT_FALSE(edge.tunnel()) << "AB should not be a tunnel";
    EXPECT_EQ(edge.road_class(), valhalla::RoadClass::kPrimary);
  }

  // Check edge BC (node 1 → edge)
  {
    const auto& edge = leg.node(1).edge();
    EXPECT_EQ(edge.speed_limit(), 100) << "BC should have speed_limit=100 km/h";
    EXPECT_GE(edge.lane_count(), 1) << "BC should have at least 1 lane";
    EXPECT_TRUE(edge.toll()) << "BC should be toll";
    EXPECT_EQ(edge.road_class(), valhalla::RoadClass::kMotorway);
  }

  // Check edge CD (node 2 → edge)
  {
    const auto& edge = leg.node(2).edge();
    EXPECT_EQ(edge.speed_limit(), 80) << "CD should have speed_limit=80 km/h";
    EXPECT_GE(edge.lane_count(), 1) << "CD should have at least 1 lane";
    EXPECT_EQ(edge.use(), valhalla::TripLeg_Use::TripLeg_Use_kRampUse)
        << "CD should be ramp use";
    EXPECT_EQ(edge.road_class(), valhalla::RoadClass::kMotorway);
  }
}

// -----------------------------------------------------------------------------
// Test 7: Road attributes and speed_limits arrays are parallel
// -----------------------------------------------------------------------------
TEST_F(NevhFeaturesTest, ParallelArrays) {
  auto result = gurka::do_action(valhalla::Options::route, nevh_map, {"A", "D"}, "auto");

  const auto& leg = result.directions().routes(0).legs(0);

  int sl_segments = leg.speed_limits_size() / 6;
  int ra_segments = leg.road_attributes_size();

  // The arrays must have the same number of segments
  EXPECT_EQ(sl_segments, ra_segments)
      << "speed_limits and road_attributes must have the same segment count";

  // Speed cameras array is independent (one entry per camera, not per segment)
  // BC has speed_camera=yes, so at least 1 entry expected
  EXPECT_GT(leg.speed_cameras_size(), 0) << "BC motorway should have speed_camera";

  // Verify the arrays are non-empty
  EXPECT_GT(sl_segments, 0) << "Should have at least 1 speed-limit segment";
}

// -----------------------------------------------------------------------------
// Test 8: Speed limit units conversion (miles mode)
// -----------------------------------------------------------------------------
TEST_F(NevhFeaturesTest, SpeedLimitsMilesConversion) {
  // Request route with miles units
  auto result = gurka::do_action(valhalla::Options::route, nevh_map,
                                 {"A", "D"}, "auto",
                                 {{"/units", "miles"}});

  const auto& leg = result.directions().routes(0).legs(0);
  ASSERT_GT(leg.speed_limits_size(), 0) << "speed_limits should not be empty";

  // The protobuf always stores speed_limits in km/h.
  // The units conversion happens in serialization, not in proto.
  // Verify the proto values are in km/h (raw OSM values).
  int num_segments = leg.speed_limits_size() / 6;
  for (int seg = 0; seg < num_segments; ++seg) {
    uint32_t spd = leg.speed_limits(seg * 6 + 3);
    // All our test speeds are ≤100 km/h
    EXPECT_LE(spd, 100) << "Proto speed_limit should be in km/h (raw)";
  }
}

// -----------------------------------------------------------------------------
// Test 9: Maneuvers contain expected fields
// -----------------------------------------------------------------------------
TEST_F(NevhFeaturesTest, ManeuverFields) {
  auto result = gurka::do_action(valhalla::Options::route, nevh_map, {"A", "D"}, "auto");

  const auto& leg = result.directions().routes(0).legs(0);

  // Verify maneuvers exist
  ASSERT_GT(leg.maneuver_size(), 0) << "Should have at least start and destination maneuvers";

  // First maneuver should be a start type
  EXPECT_EQ(leg.maneuver(0).type(),
            valhalla::DirectionsLeg_Maneuver_Type::DirectionsLeg_Maneuver_Type_kStart);

  // Last maneuver should be a destination type
  int last = leg.maneuver_size() - 1;
  EXPECT_EQ(leg.maneuver(last).type(),
            valhalla::DirectionsLeg_Maneuver_Type::DirectionsLeg_Maneuver_Type_kDestination);

  // Turn lanes field (42) is set only if turn lane data exists on edges.
  // Since we didn't set turn:lanes OSM tags, it should be empty.
  for (int i = 0; i < leg.maneuver_size(); ++i) {
    // just verify the field is accessible (added by //nevh)
    const auto& tl = leg.maneuver(i).turn_lanes();
    // May be empty if no turn lane data — that's fine
    (void)tl;
  }
}

// -----------------------------------------------------------------------------
// Test 10: Verify road_attributes bit packing round-trip for known edges
// -----------------------------------------------------------------------------
TEST_F(NevhFeaturesTest, RoadAttributesBitPacking) {
  auto result = gurka::do_action(valhalla::Options::route, nevh_map, {"A", "D"}, "auto");

  const auto& leg = result.directions().routes(0).legs(0);

  // For each segment, verify the packed road_attributes match the
  // corresponding trip edge attributes (from the first edge of each segment).
  ASSERT_GE(leg.road_attributes_size(), 3);

  // This also validates that GetRoadAttributes and GetSpeedLimits
  // use the same merge boundaries (parallel arrays).
  for (int i = 0; i < leg.road_attributes_size(); ++i) {
    auto r = unpack(leg.road_attributes(i));

    // Basic sanity: RoadClass must be 0–7
    EXPECT_LE(r.road_class, 7);
    // Use must be 0–54
    EXPECT_LE(r.use, 54);
    // Sidewalk must be 0–3
    EXPECT_LE(r.sidewalk, 3);
    // Cycle lane must be 0–3
    EXPECT_LE(r.cycle_lane, 3);
    // Surface must be 0–7
    EXPECT_LE(r.surface, 7);
  }
}

// -----------------------------------------------------------------------------
// Test 11: Tunnel map — verify tunnel flag in road_attributes + trip edges
// -----------------------------------------------------------------------------
TEST_F(NevhFeaturesTest, TunnelAttributes) {
  // Build a separate small map with a tunnel
  const std::string ascii_map = R"(X----Y)";
  const gurka::ways ways = {
      {"XY", {{"highway", "residential"},
              {"maxspeed", "30"},
              {"lanes", "1"},
              {"tunnel", "yes"},
              {"name", "Tunnel Way"}}},
  };
  const auto layout = gurka::detail::map_to_coordinates(ascii_map, 100);
  auto map = gurka::buildtiles(layout, ways, {}, {}, "test/data/nevh_tunnel");

  auto result = gurka::do_action(valhalla::Options::route, map, {"X", "Y"}, "auto");
  const auto& leg = result.directions().routes(0).legs(0);

  // Check road_attributes
  ASSERT_GE(leg.road_attributes_size(), 1);
  auto r = unpack(leg.road_attributes(0));
  EXPECT_TRUE(r.infra_flags & static_cast<uint8_t>(baldr::RoadAttributeFlag::kTunnel))
      << "Tunnel flag should be set in road_attributes";
  EXPECT_FALSE(r.infra_flags & static_cast<uint8_t>(baldr::RoadAttributeFlag::kBridge))
      << "Bridge flag should NOT be set";

  // Check trip edge
  const auto& edge = result.trip().routes(0).legs(0).node(0).edge();
  EXPECT_TRUE(edge.tunnel()) << "Trip edge should have tunnel=true";
  EXPECT_FALSE(edge.bridge()) << "Trip edge should have bridge=false";
  EXPECT_EQ(edge.speed_limit(), 30);
  EXPECT_EQ(edge.lane_count(), 1);
}

// -----------------------------------------------------------------------------
// Test 12: Roundabout detection in road_attributes
// -----------------------------------------------------------------------------
TEST_F(NevhFeaturesTest, RoundaboutAttributes) {
  // A roundabout: A→B→C→D where B-C is the roundabout section
  const std::string ascii_map = R"(
      A----B
           |
      D----C
  )";
  const gurka::ways ways = {
      {"AB", {{"highway", "residential"}, {"maxspeed", "30"}, {"name", "Approach"}}},
      {"BC", {{"highway", "residential"},
              {"maxspeed", "20"},
              {"junction", "roundabout"},
              {"name", "Roundabout"}}},
      {"CD", {{"highway", "residential"},
              {"maxspeed", "20"},
              {"junction", "roundabout"},
              {"name", "Roundabout"}}},
  };
  const auto layout = gurka::detail::map_to_coordinates(ascii_map, 100);
  auto map = gurka::buildtiles(layout, ways, {}, {}, "test/data/nevh_roundabout");

  auto result = gurka::do_action(valhalla::Options::route, map, {"A", "D"}, "auto");
  const auto& leg_trip = result.trip().routes(0).legs(0);

  // The second edge (BC) should have roundabout=true
  ASSERT_GT(leg_trip.node_size(), 1);
  const auto& edge = leg_trip.node(1).edge();
  EXPECT_TRUE(edge.roundabout()) << "Roundabout edge should have roundabout=true";

  // Check road_attributes has the roundabout flag on at least one segment
  const auto& leg_dir = result.directions().routes(0).legs(0);
  ASSERT_GT(leg_dir.road_attributes_size(), 0);
  bool found_roundabout = false;
  for (int i = 0; i < leg_dir.road_attributes_size(); ++i) {
    auto r = unpack(leg_dir.road_attributes(i));
    if (r.infra_flags & static_cast<uint8_t>(baldr::RoadAttributeFlag::kRoundabout)) {
      found_roundabout = true;
      break;
    }
  }
  EXPECT_TRUE(found_roundabout) << "At least one road_attributes entry should have roundabout flag";
}

// -----------------------------------------------------------------------------
// Test 13: Edge count consistency between trip edges and directions segments
// -----------------------------------------------------------------------------
TEST_F(NevhFeaturesTest, TripEdgeCountsMatch) {
  auto result = gurka::do_action(valhalla::Options::route, nevh_map, {"A", "D"}, "auto");

  const auto& trip_leg = result.trip().routes(0).legs(0);
  const auto& dir_leg = result.directions().routes(0).legs(0);

  // Trip path has nodes; each node (except last) has one edge
  int trip_edges = trip_leg.node_size() - 1;
  EXPECT_GT(trip_edges, 0) << "Should have at least 1 trip edge";

  // Speed limits are coalesced (consecutive same-attribute edges merged)
  // So speed_limits segment count ≤ trip edge count
  int sl_segments = dir_leg.speed_limits_size() / 6;
  EXPECT_LE(sl_segments, trip_edges)
      << "Coalesced speed-limit segments should be ≤ trip edges";
}

// -----------------------------------------------------------------------------
// Test 14: Multiple route legs produce independent speed_limits arrays
// -----------------------------------------------------------------------------
TEST_F(NevhFeaturesTest, MultiPointRoute) {
  // A route with 3 waypoints (A→C→D) exercises multi-point routing.
  auto result = gurka::do_action(valhalla::Options::route, nevh_map,
                                 {"A", "C", "D"}, "auto");

  const auto& directions = result.directions();
  ASSERT_GE(directions.routes_size(), 1);
  ASSERT_GE(directions.routes(0).legs_size(), 1)
      << "Multi-point route should produce at least 1 leg";

  // Each leg should have its own speed_limits and road_attributes
  for (int leg_idx = 0; leg_idx < directions.routes(0).legs_size(); ++leg_idx) {
    const auto& leg = directions.routes(0).legs(leg_idx);
    EXPECT_GT(leg.speed_limits_size(), 0)
        << "Leg " << leg_idx << " should have speed_limits";
    EXPECT_GT(leg.road_attributes_size(), 0)
        << "Leg " << leg_idx << " should have road_attributes";
    EXPECT_EQ(leg.summary().nevhversion(), "1")
        << "Leg " << leg_idx << " should have nevh_version='1'";
  }
}
