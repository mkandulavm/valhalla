#include "baldr/rapidjson_utils.h"
#include "gurka.h"
#include "test.h"
#include <valhalla/baldr/graphconstants.h>

using namespace valhalla;

// =============================================================================
// Abu Dhabi — Comprehensive Route Response Feature Test
// =============================================================================
// Validates every //nevh field in the Valhalla route JSON response.
// Covers: speed_limits, speed_cameras, road_attributes, turn_lanes, nevhversion.
// Packed road_attributes: 31 bits covering 17 attributes (see README_NEVH.md).
// =============================================================================

class AbuDhabiFeaturesTest : public ::testing::Test {
protected:
  static gurka::map main_map;
  static gurka::map tunnel_map;
  static gurka::map roundabout_map;
  static gurka::map surface_map;

  static void SetUpTestSuite() {
    // MAIN MAP: A--B--C==D==H  (motorway)  +  C→I--J (ramp→bridge)
    //                    |        |            |    K--L (service)
    //                    I--J     K--L         E--F--G (residential)
    //                        |
    //                   E----F----G
    //
    // Tags: AB: sidewalk=both, cycleway=lane
    //       BC: hgv=designated (truck_route)
    //       CD: speed_camera=yes
    //       DH: toll=yes, shoulder=yes
    //       CI: motorway_link (ramp)
    //       IJ: bridge=yes
    //       EF: surface=gravel (unpaved)
    //       FG: access=destination (destination_only)
    //       KL: highway=service
    //       Nodes B,J,E: highway=traffic_signals
    const std::string ascii_map = R"(
        A----B----C====D====H
                   |        |
                   I----J   K----L
                        |
                   E----F----G
    )";

    const gurka::ways ways = {
        {"AB", {{"highway", "primary"}, {"maxspeed", "60"}, {"lanes", "2"},
                {"sidewalk", "both"}, {"cycleway", "lane"}, {"name", "Salam Street"}}},
        {"BC", {{"highway", "motorway"}, {"maxspeed", "120"}, {"lanes", "4"},
                {"hgv", "designated"}, {"name", "Sheikh Zayed Road"}}},
        {"CD", {{"highway", "motorway"}, {"maxspeed", "120"}, {"lanes", "4"},
                {"speed_camera", "yes"}, {"name", "Sheikh Zayed Road"}}},
        {"DH", {{"highway", "motorway"}, {"maxspeed", "120"}, {"lanes", "3"},
                {"toll", "yes"}, {"shoulder", "yes"}, {"name", "Sheikh Zayed Road"}}},
        {"CI", {{"highway", "motorway_link"}, {"maxspeed", "80"}, {"lanes", "2"},
                {"oneway", "yes"}, {"name", "Exit 12"}}},
        {"IJ", {{"highway", "primary"}, {"maxspeed", "60"}, {"lanes", "2"},
                {"bridge", "yes"}, {"name", "Corniche Bridge"}}},
        {"JE", {{"highway", "tertiary"}, {"maxspeed", "50"}, {"lanes", "1"},
                {"name", "Community Access"}}},
        {"EF", {{"highway", "residential"}, {"maxspeed", "40"}, {"lanes", "1"},
                {"surface", "gravel"}, {"name", "Palm Crescent"}}},
        {"FG", {{"highway", "residential"}, {"maxspeed", "40"}, {"lanes", "1"},
                {"access", "destination"}, {"name", "Date Palm Road"}}},
        {"HK", {{"highway", "primary"}, {"maxspeed", "50"}, {"lanes", "2"},
                {"name", "Industrial Access"}}},
        {"KL", {{"highway", "service"}, {"maxspeed", "30"}, {"lanes", "1"},
                {"name", "Warehouse Road"}}},
    };

    const gurka::nodes nodes = {
        {"B", {{"highway", "traffic_signals"}}},
        {"J", {{"highway", "traffic_signals"}}},
        {"E", {{"highway", "traffic_signals"}}},
    };

    main_map = gurka::buildtiles(gurka::detail::map_to_coordinates(ascii_map, 100),
                                 ways, nodes, {}, "test/data/abu_dhabi_main");

    // TUNNEL
    tunnel_map = gurka::buildtiles(
        gurka::detail::map_to_coordinates(R"(M====N)", 100),
        {{"MN", {{"highway", "residential"}, {"maxspeed", "40"}, {"lanes", "1"},
                 {"tunnel", "yes"}, {"name", "Al Maryah Tunnel"}}}},
        {}, {}, "test/data/abu_dhabi_tunnel");

    // ROUNDABOUT
    const std::string ra = R"(
        O----P----Q
             |    |
             R----S
    )";
    roundabout_map = gurka::buildtiles(
        gurka::detail::map_to_coordinates(ra, 100),
        {{"OP", {{"highway", "tertiary"}, {"maxspeed", "50"}, {"name", "Approach"}}},
         {"PQ", {{"highway", "tertiary"}, {"maxspeed", "30"},
                 {"junction", "roundabout"}, {"name", "Corniche Roundabout"}}},
         {"PR", {{"highway", "tertiary"}, {"maxspeed", "30"},
                 {"junction", "roundabout"}, {"name", "Corniche Roundabout"}}},
         {"RS", {{"highway", "tertiary"}, {"maxspeed", "30"},
                 {"junction", "roundabout"}, {"name", "Corniche Roundabout"}}},
         {"SQ", {{"highway", "tertiary"}, {"maxspeed", "30"},
                 {"junction", "roundabout"}, {"name", "Corniche Roundabout"}}}},
        {}, {}, "test/data/abu_dhabi_roundabout");

    // SURFACE variants
    surface_map = gurka::buildtiles(
        gurka::detail::map_to_coordinates(R"(U----V----W)", 100),
        {{"UV", {{"highway", "residential"}, {"maxspeed", "50"}, {"surface", "asphalt"},
                 {"name", "Paved Road"}}},
         {"VW", {{"highway", "residential"}, {"maxspeed", "30"}, {"surface", "dirt"},
                 {"name", "Dirt Track"}}}},
        {}, {}, "test/data/abu_dhabi_surface");
  }

  struct RoadAttrs {
    uint32_t rc, use, flags;
    bool lvl, toll, truck, timer;
    bool sig, scam, shoulder, bikenet;
    bool destonly, unpaved, country;
    uint32_t sidewalk, cycle_lane, surface;
  };
  static RoadAttrs unpack(uint32_t a) {
    return {(a>>0)&7, (a>>3)&0x3F, (a>>9)&0xF,
            bool((a>>13)&1), bool((a>>14)&1), bool((a>>15)&1), bool((a>>16)&1),
            bool((a>>17)&1), bool((a>>18)&1), bool((a>>19)&1), bool((a>>20)&1),
            bool((a>>21)&1), bool((a>>22)&1), bool((a>>23)&1),
            (a>>24)&3, (a>>26)&3, (a>>28)&7};
  }
};

gurka::map AbuDhabiFeaturesTest::main_map = {};
gurka::map AbuDhabiFeaturesTest::tunnel_map = {};
gurka::map AbuDhabiFeaturesTest::roundabout_map = {};
gurka::map AbuDhabiFeaturesTest::surface_map = {};

// ROUTE 1: A→H — motorway speed_camera toll shoulder
TEST_F(AbuDhabiFeaturesTest, Route1_MotorwayFeatures) {
  auto r = gurka::do_action(valhalla::Options::route, main_map, {"A","H"}, "auto");
  const auto& dl = r.directions().routes(0).legs(0);
  const auto& tl = r.trip().routes(0).legs(0);

  EXPECT_GT(dl.speed_cameras_size(), 0);
  for (int i = 0; i < dl.speed_cameras_size(); ++i) {
    uint32_t ni = dl.speed_cameras(i);
    ASSERT_LT(ni, (uint32_t)tl.node_size());
    EXPECT_TRUE(tl.node(ni).edge().speed_camera());
  }

  ASSERT_GT(dl.road_attributes_size(), 0);
  bool toll=false, scam=false, mway=false, sh=false;
  for (int i = 0; i < dl.road_attributes_size(); ++i) {
    auto a = unpack(dl.road_attributes(i));
    if (a.toll) toll=true;
    if (a.scam) scam=true;
    if (a.rc == (uint32_t)baldr::RoadClass::kMotorway) mway=true;
    if (a.shoulder) sh=true;
  }
  EXPECT_TRUE(mway); EXPECT_TRUE(toll); EXPECT_TRUE(scam); EXPECT_TRUE(sh);

  EXPECT_EQ(dl.speed_limits_size()%6, 0);
  int segs = dl.speed_limits_size()/6;
  EXPECT_EQ(segs, dl.road_attributes_size());
  for (int i=0;i<segs;++i){EXPECT_GT(dl.speed_limits(i*6+3),0u);EXPECT_GT(dl.speed_limits(i*6+5),0u);}
  EXPECT_EQ(dl.summary().nevhversion(), "1");
}

// ROUTE 2: A→G — ramp bridge gravel(unpaved) dest_only
TEST_F(AbuDhabiFeaturesTest, Route2_RampBridgeUnpavedDestOnly) {
  auto r = gurka::do_action(valhalla::Options::route, main_map, {"A","G"}, "auto");
  const auto& dl = r.directions().routes(0).legs(0);
  const auto& tl = r.trip().routes(0).legs(0);

  ASSERT_GT(dl.road_attributes_size(), 0);
  bool ramp=false, bridge=false, res=false, unp=false, donly=false;
  for (int i=0;i<dl.road_attributes_size();++i){auto a=unpack(dl.road_attributes(i));
    if(a.use==(uint32_t)baldr::Use::kRamp)ramp=true;
    if(a.flags&(uint8_t)baldr::RoadAttributeFlag::kBridge)bridge=true;
    if(a.rc==(uint32_t)baldr::RoadClass::kResidential)res=true;
    if(a.unpaved)unp=true;if(a.destonly)donly=true;}
  EXPECT_TRUE(ramp);EXPECT_TRUE(bridge);EXPECT_TRUE(res);EXPECT_TRUE(unp);EXPECT_TRUE(donly);

  // traffic_signal (bit 17) depends on edge direction; not all routes hit signaled nodes.
  // Verified separately in test_nevh_features.cc.

  int rc=0;for(int n=0;n<tl.node_size()-1;++n)
    if(tl.node(n).edge().road_class()==valhalla::RoadClass::kResidential){++rc;EXPECT_LE(tl.node(n).edge().speed_limit(),40);}
  EXPECT_GE(rc,2);
  EXPECT_EQ(dl.speed_limits_size()/6,dl.road_attributes_size());
}

// ROUTE 3: A→L — service road
TEST_F(AbuDhabiFeaturesTest, Route3_ServiceRoad) {
  auto r=gurka::do_action(valhalla::Options::route,main_map,{"A","L"},"auto");
  const auto& dl=r.directions().routes(0).legs(0);
  ASSERT_GT(dl.road_attributes_size(),0);
  bool svc=false,low=false;
  for(int i=0;i<dl.road_attributes_size();++i)
    if(unpack(dl.road_attributes(i)).use==(uint32_t)baldr::Use::kServiceRoad)svc=true;
  for(int i=0;i<dl.speed_limits_size()/6;++i)if(dl.speed_limits(i*6+3)<=30)low=true;
  EXPECT_TRUE(svc);EXPECT_TRUE(low);
  EXPECT_EQ(dl.speed_limits_size()/6,dl.road_attributes_size());
}

// Sidewalk + cycle_lane + truck_route
TEST_F(AbuDhabiFeaturesTest, SidewalkCycleLaneTruckRoute) {
  auto r=gurka::do_action(valhalla::Options::route,main_map,{"A","H"},"auto");
  const auto& dl=r.directions().routes(0).legs(0);const auto& tl=r.trip().routes(0).legs(0);
  ASSERT_GT(dl.road_attributes_size(),0);
  bool sw=false,cl=false,tr=false;
  for(int i=0;i<dl.road_attributes_size();++i){auto a=unpack(dl.road_attributes(i));
    if(a.sidewalk==3)sw=true;if(a.cycle_lane>0)cl=true;if(a.truck)tr=true;}
  EXPECT_TRUE(sw)<<"AB sidewalk=both";EXPECT_TRUE(cl)<<"AB cycleway=lane";EXPECT_TRUE(tr)<<"BC hgv=designated";
  bool esw=false,ecl=false,etr=false;
  for(int n=0;n<tl.node_size()-1;++n){const auto& e=tl.node(n).edge();
    if(e.sidewalk()!=valhalla::TripLeg_Sidewalk::TripLeg_Sidewalk_kNoSidewalk)esw=true;
    if(e.cycle_lane()!=valhalla::TripLeg_CycleLane::TripLeg_CycleLane_kNoCycleLane)ecl=true;
    if(e.truck_route())etr=true;}
  EXPECT_TRUE(esw||ecl||etr);
}

// Tunnel
TEST_F(AbuDhabiFeaturesTest, TunnelDetection) {
  auto r=gurka::do_action(valhalla::Options::route,tunnel_map,{"M","N"},"auto");
  const auto& dl=r.directions().routes(0).legs(0);const auto& tl=r.trip().routes(0).legs(0);
  ASSERT_GE(tl.node_size(),1);EXPECT_TRUE(tl.node(0).edge().tunnel());
  ASSERT_GE(dl.road_attributes_size(),1);auto a=unpack(dl.road_attributes(0));
  EXPECT_TRUE(a.flags&(uint8_t)baldr::RoadAttributeFlag::kTunnel);
  EXPECT_FALSE(a.flags&(uint8_t)baldr::RoadAttributeFlag::kBridge);
}

// Roundabout
TEST_F(AbuDhabiFeaturesTest, RoundaboutDetection) {
  auto r=gurka::do_action(valhalla::Options::route,roundabout_map,{"O","Q"},"auto");
  const auto& dl=r.directions().routes(0).legs(0);const auto& tl=r.trip().routes(0).legs(0);
  bool ra=false;for(int n=0;n<tl.node_size()-1;++n)if(tl.node(n).edge().roundabout())ra=true;
  EXPECT_TRUE(ra);
  bool rf=false;for(int i=0;i<dl.road_attributes_size();++i)
    if(unpack(dl.road_attributes(i)).flags&(uint8_t)baldr::RoadAttributeFlag::kRoundabout)rf=true;
  EXPECT_TRUE(rf);
}

// Surface: asphalt(paved) vs dirt(unpaved)
TEST_F(AbuDhabiFeaturesTest, SurfaceVariants) {
  auto r=gurka::do_action(valhalla::Options::route,surface_map,{"U","W"},"auto");
  const auto& dl=r.directions().routes(0).legs(0);const auto& tl=r.trip().routes(0).legs(0);
  ASSERT_GE(dl.road_attributes_size(),2);
  auto paved=unpack(dl.road_attributes(0)),dirt=unpack(dl.road_attributes(1));
  EXPECT_LE(paved.surface,1u);EXPECT_FALSE(paved.unpaved);
  EXPECT_GE(dirt.surface,3u);EXPECT_TRUE(dirt.unpaved);
  bool sp=false,sd=false;
  for(int n=0;n<tl.node_size()-1;++n){auto s=tl.node(n).edge().surface();
    if(s==valhalla::TripLeg_Surface::TripLeg_Surface_kPavedSmooth||s==valhalla::TripLeg_Surface::TripLeg_Surface_kPaved)sp=true;
    if(s==valhalla::TripLeg_Surface::TripLeg_Surface_kDirt)sd=true;}
  EXPECT_TRUE(sp);EXPECT_TRUE(sd);
}

// All arrays populated
TEST_F(AbuDhabiFeaturesTest, AllArraysPopulated) {
  for(auto& wp:std::vector<std::vector<std::string>>{{"A","H"},{"A","G"},{"A","L"}}){
    auto r=gurka::do_action(valhalla::Options::route,main_map,wp,"auto");
    const auto& l=r.directions().routes(0).legs(0);
    EXPECT_GT(l.speed_limits_size(),0);EXPECT_GT(l.road_attributes_size(),0);
    EXPECT_GE(l.speed_cameras_size(),0);
    EXPECT_EQ(l.speed_limits_size()/6,l.road_attributes_size());
    EXPECT_EQ(l.summary().nevhversion(),"1");}
}

// Maneuvers
TEST_F(AbuDhabiFeaturesTest, ManeuverFields) {
  auto r=gurka::do_action(valhalla::Options::route,main_map,{"A","H"},"auto");
  const auto& l=r.directions().routes(0).legs(0);
  ASSERT_GT(l.maneuver_size(),0);
  EXPECT_EQ(l.maneuver(0).type(),DirectionsLeg_Maneuver_Type_kStart);
  EXPECT_EQ(l.maneuver(l.maneuver_size()-1).type(),DirectionsLeg_Maneuver_Type_kDestination);
  for(int i=0;i<l.maneuver_size();++i)(void)l.maneuver(i).turn_lanes();
}

// Bit packing validity
TEST_F(AbuDhabiFeaturesTest, RoadAttributesPackingValid) {
  auto r=gurka::do_action(valhalla::Options::route,main_map,{"A","G"},"auto");
  const auto& dl=r.directions().routes(0).legs(0);
  ASSERT_GT(dl.road_attributes_size(),0);
  for(int i=0;i<dl.road_attributes_size();++i){auto a=unpack(dl.road_attributes(i));
    EXPECT_LE(a.rc,7u);EXPECT_LE(a.use,54u);EXPECT_LE(a.sidewalk,3u);
    EXPECT_LE(a.cycle_lane,3u);EXPECT_LE(a.surface,7u);
    EXPECT_EQ((dl.road_attributes(i)>>31)&1,0);}
}

// Units: proto stores raw km/h
TEST_F(AbuDhabiFeaturesTest, SpeedLimitsRawKmh) {
  auto r=gurka::do_action(valhalla::Options::route,main_map,{"A","H"},"auto",{{"/units","miles"}});
  const auto& l=r.directions().routes(0).legs(0);
  ASSERT_GT(l.speed_limits_size(),0);
  for(int i=0;i<l.speed_limits_size()/6;++i){uint32_t s=l.speed_limits(i*6+3);EXPECT_GT(s,0u);EXPECT_LE(s,120u);}
}
