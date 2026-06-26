#include "route_serializer_valhalla.h"
#include "baldr/rapidjson_utils.h"
#include "baldr/timedomain.h"
#include "midgard/aabb2.h"
#include "midgard/logging.h"
#include "odin/enhancedtrippath.h"
#include "proto_conversions.h"
#include "tyr/serializers.h"

#include <algorithm>
#include <array>
#include <map>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

using namespace valhalla;
using namespace valhalla::midgard;
using namespace valhalla::odin;
using namespace valhalla::baldr;

namespace {

struct ManeuverTruckPermitDetails {
  bool has_permit = false;
  bool has_timed = false;
  std::vector<std::string> timed_windows;
};

struct LegTruckPermitIndex {
  std::vector<uint32_t> permit_prefix;
  std::vector<uint32_t> timed_prefix;
  std::unordered_map<uint64_t, std::vector<uint32_t>> timed_value_positions;
};

std::string format_two_digits(uint32_t value) {
  return value < 10 ? "0" + std::to_string(value) : std::to_string(value);
}

std::string format_ampm_time(uint8_t hours, uint8_t mins) {
  uint32_t normalized_hours = hours % 24;
  const char* am_pm = normalized_hours >= 12 ? "PM" : "AM";
  uint32_t hour12 = normalized_hours % 12;
  if (hour12 == 0) {
    hour12 = 12;
  }

  return std::to_string(hour12) + ":" + format_two_digits(mins) + am_pm;
}

std::string format_ampm_range(const baldr::TimeDomain& td) {
  return format_ampm_time(td.begin_hrs(), td.begin_mins()) + "-" +
         format_ampm_time(td.end_hrs(), td.end_mins());
}

std::string dow_label(uint8_t dow_mask) {
  static const std::array<const char*, 7> day_names = {
      "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
  static const std::array<const char*, 7> day_short = {"Su", "Mo", "Tu", "We", "Th", "Fr", "Sa"};

  if (dow_mask == 0 || dow_mask == 0b1111111) {
    return "Time Ban";
  }

  int bit_count = 0;
  int first_bit = -1;
  int last_bit = -1;
  for (int i = 0; i < 7; ++i) {
    if (dow_mask & (1 << i)) {
      ++bit_count;
      if (first_bit == -1) {
        first_bit = i;
      }
      last_bit = i;
    }
  }

  if (bit_count == 1) {
    return std::string(day_names[first_bit]) + " Ban";
  }

  const uint8_t contiguous_mask = static_cast<uint8_t>(((1 << (last_bit - first_bit + 1)) - 1) << first_bit);
  if (contiguous_mask == dow_mask) {
    return std::string(day_short[first_bit]) + "-" + day_short[last_bit] + " Ban";
  }

  std::stringstream ss;
  bool first = true;
  for (int i = 0; i < 7; ++i) {
    if (dow_mask & (1 << i)) {
      if (!first) {
        ss << ",";
      }
      ss << day_short[i];
      first = false;
    }
  }
  ss << " Ban";
  return ss.str();
}

std::vector<std::string> format_timed_windows(const std::vector<uint64_t>& timed_values) {
  std::map<uint8_t, std::vector<std::pair<uint32_t, std::string>>> grouped_ranges;

  for (const auto value : timed_values) {
    const baldr::TimeDomain td(value);
    uint32_t start_minutes = static_cast<uint32_t>(td.begin_hrs()) * 60 + td.begin_mins();
    grouped_ranges[td.dow()].emplace_back(start_minutes, format_ampm_range(td));
  }

  std::vector<std::string> formatted;
  for (auto& group : grouped_ranges) {
    auto& ranges = group.second;
    std::sort(ranges.begin(), ranges.end(), [](const auto& lhs, const auto& rhs) {
      if (lhs.first != rhs.first) {
        return lhs.first < rhs.first;
      }
      return lhs.second < rhs.second;
    });

    std::vector<std::string> unique_ranges;
    unique_ranges.reserve(ranges.size());
    for (const auto& entry : ranges) {
      if (unique_ranges.empty() || unique_ranges.back() != entry.second) {
        unique_ranges.emplace_back(entry.second);
      }
    }

    std::stringstream ss;
    ss << dow_label(group.first) << ": ";
    for (size_t i = 0; i < unique_ranges.size(); ++i) {
      if (i > 0) {
        ss << " & ";
      }
      ss << unique_ranges[i];
    }
    formatted.emplace_back(ss.str());
  }

  return formatted;
}

LegTruckPermitIndex build_truck_permit_index(const TripLeg& leg) {
  LegTruckPermitIndex index;
  const size_t node_count = leg.node_size();
  index.permit_prefix.resize(node_count + 1, 0);
  index.timed_prefix.resize(node_count + 1, 0);

  for (uint32_t path_index = 0; path_index < node_count; ++path_index) {
    const auto& edge = leg.node(path_index).edge();
    bool is_permit = false;
    bool is_timed = false;

    auto process_restriction = [&](uint32_t restriction_type, uint64_t restriction_value) {
      if (restriction_type == static_cast<uint32_t>(AccessType::kPermitRequired)) {
        is_permit = true;
        return;
      }

      if (restriction_type == static_cast<uint32_t>(AccessType::kTimedDenied) ||
          restriction_type == static_cast<uint32_t>(AccessType::kTimedAllowed) ||
          restriction_type == static_cast<uint32_t>(AccessType::kDestinationAllowed)) {
        is_timed = true;
        if (restriction_value != 0) {
          index.timed_value_positions[restriction_value].push_back(path_index);
        }
      }
    };

    if (edge.restrictions_size() > 0) {
      for (const auto& restriction : edge.restrictions()) {
        process_restriction(restriction.type(), restriction.value());
      }
    } else {
      process_restriction(edge.restriction().type(), edge.restriction().value());
    }

    index.permit_prefix[path_index + 1] = index.permit_prefix[path_index] + (is_permit ? 1 : 0);
    index.timed_prefix[path_index + 1] = index.timed_prefix[path_index] + (is_timed ? 1 : 0);
  }

  return index;
}

ManeuverTruckPermitDetails collect_truck_permit_details(const LegTruckPermitIndex& index,
                                                        const DirectionsLeg_Maneuver& maneuver,
                                                        const size_t node_count) {
  ManeuverTruckPermitDetails details;
  const auto begin_path = maneuver.begin_path_index();
  const auto end_path = maneuver.end_path_index();

  if (begin_path >= node_count || end_path > node_count || begin_path >= end_path) {
    return details;
  }

  details.has_permit = index.permit_prefix[end_path] > index.permit_prefix[begin_path];
  details.has_timed = index.timed_prefix[end_path] > index.timed_prefix[begin_path];

  if (!details.has_timed) {
    return details;
  }

  std::vector<uint64_t> timed_values;
  timed_values.reserve(index.timed_value_positions.size());
  for (const auto& timed_entry : index.timed_value_positions) {
    const auto& positions = timed_entry.second;
    const auto it = std::lower_bound(positions.begin(), positions.end(), begin_path);
    if (it != positions.end() && *it < end_path) {
      timed_values.push_back(timed_entry.first);
    }
  }

  details.timed_windows = format_timed_windows(timed_values);
  return details;
}

const char* restriction_category(const ManeuverTruckPermitDetails& details) {
  if (details.has_permit && details.has_timed) {
    return "permit_with_timed_ban";
  }
  if (details.has_permit) {
    return "permit_only";
  }
  if (details.has_timed) {
    return "timed_ban";
  }
  return "no_time_ban";
}

/*
valhalla output looks like this:
{
    "trip":
{
    "status": 0,
    "locations": [
       {
        "longitude": -76.4791,
        "latitude": 40.4136,
         "stopType": 0
       },
       {
        "longitude": -76.5352,
        "latitude": 40.4029,
        "stopType": 0
       }
     ],
    "units": "kilometers"
    "summary":
{
    "distance": 4973,
    "time": 325,
    "cost": 304
},
"legs":
[
  {
      "summary":
  {
      "distance": 4973,
      "time": 325,
      "cost": 304
  },
  "maneuvers":
  [
    {
        "beginShapeIndex": 0,
        "distance": 633,
        "writtenInstruction": "Start out going west on West Market Street.",
        "streetNames":
        [
            "West Market Street"
        ],
        "type": 1,
        "time": 41,
        "cost": 23
    },
    {
        "beginShapeIndex": 7,
        "distance": 4340,
        "writtenInstruction": "Continue onto Jonestown Road.",
        "streetNames":
        [
            "Jonestown Road"
        ],
        "type": 8,
        "time": 284,
        "cost": 281
    },
    {
        "beginShapeIndex": 40,
        "distance": 0,
        "writtenInstruction": "You have arrived at your destination.",
        "type": 4,
        "time": 0,
        "cost": 0
    }
],
"shape":
"gysalAlg|zpC~Clt@tDtx@hHfaBdKl{BrKbnApGro@tJrz@jBbQj@zVt@lTjFnnCrBz}BmFnoB]pHwCvm@eJxtATvXTnfAk@|^z@rGxGre@nTpnBhBbQvXduCrUr`Edd@naEja@~gAhk@nzBxf@byAfm@tuCvDtOvNzi@|jCvkKngAl`HlI|}@`N`{Adx@pjE??xB|J"
}
],
"status_message": "Found route between points"
},
"id": "work route"
}
*/

void summary(const valhalla::Api& api, int route_index, rapidjson::writer_wrapper_t& writer) {
  double route_time = 0;
  double route_length = 0;
  double route_cost = 0;
  bool has_time_restrictions = false;
  bool has_toll = false;
  bool has_highway = false;
  bool has_ferry = false;
  AABB2<PointLL> bbox(10000.0f, 10000.0f, -10000.0f, -10000.0f);
  std::vector<double> recost_times(api.options().recostings_size(), 0);
  for (int leg_index = 0; leg_index < api.directions().routes(route_index).legs_size(); ++leg_index) {
    const auto& leg = api.directions().routes(route_index).legs(leg_index);
    const auto& trip_leg = api.trip().routes(route_index).legs(leg_index);
    route_time += leg.summary().time();
    route_length += leg.summary().length();
    route_cost += trip_leg.node().rbegin()->cost().elapsed_cost().cost();

    // recostings
    const auto& recosts = trip_leg.node().rbegin()->recosts();
    auto recost_time_itr = recost_times.begin();
    for (const auto& recost : recosts) {
      if (!recost.has_elapsed_cost() || (*recost_time_itr) < 0)
        (*recost_time_itr) = -1;
      else
        (*recost_time_itr) += recost.elapsed_cost().seconds();
      ++recost_time_itr;
    }

    AABB2<PointLL> leg_bbox(leg.summary().bbox().min_ll().lng(), leg.summary().bbox().min_ll().lat(),
                            leg.summary().bbox().max_ll().lng(), leg.summary().bbox().max_ll().lat());
    bbox.Expand(leg_bbox);
    has_time_restrictions = has_time_restrictions || leg.summary().has_time_restrictions();
    has_toll = has_toll || leg.summary().has_toll();
    has_highway = has_highway || leg.summary().has_highway();
    has_ferry = has_ferry || leg.summary().has_ferry();
  }

  //get nevh_version from options
  auto nevh_version = (uint32_t)api.options().nevh_version();

  writer.start_object("summary");
  writer("has_time_restrictions", has_time_restrictions);
  writer("has_toll", has_toll);
  writer("has_highway", has_highway);
  writer("has_ferry", has_ferry);
  writer.set_precision(tyr::kCoordinatePrecision);
  writer("min_lat", bbox.miny());
  writer("min_lon", bbox.minx());
  writer("max_lat", bbox.maxy());
  writer("max_lon", bbox.maxx());
  writer.set_precision(tyr::kDefaultPrecision);
  writer("time", route_time);
  writer.set_precision(api.options().units() == Options::miles ? 4 : 3);
  writer("length", route_length);
  writer.set_precision(tyr::kDefaultPrecision);
  writer("cost", route_cost);
  writer("nevh_version", nevh_version);

  auto recost_itr = api.options().recostings().begin();
  for (auto recost : recost_times) {
    if (recost < 0)
      writer("time_" + recost_itr->name(), std::nullptr_t());
    else
      writer("time_" + recost_itr->name(), recost);
    ++recost_itr;
  }
  writer.end_object();

  writer("status_message", "Found route between points");
  writer("status", 0); // 0 success
  writer("units", valhalla::Options_Units_Enum_Name(api.options().units()));
  writer("language", api.options().language());

  LOG_DEBUG("trip_time::" + std::to_string(route_time) + "s");
}

void locations(const valhalla::Api& api, int route_index, rapidjson::writer_wrapper_t& writer) {

  int index = 0;
  writer.set_precision(tyr::kCoordinatePrecision);
  writer.start_array("locations");
  for (const auto& leg : api.directions().routes(route_index).legs()) {
    for (auto location = leg.location().begin() + index; location != leg.location().end();
         ++location) {
      index = 1;
      writer.start_object();

      writer("type", Location_Type_Enum_Name(location->type()));
      writer("lat", location->ll().lat());
      writer("lon", location->ll().lng());
      if (!location->name().empty()) {
        writer("name", location->name());
      }

      if (!location->street().empty()) {
        writer("street", location->street());
      }

      if (location->has_heading_case()) {
        writer("heading", static_cast<uint64_t>(location->heading()));
      }

      if (!location->date_time().empty()) {
        writer("date_time", location->date_time());
      }

      if (!location->time_zone_offset().empty()) {
        writer("time_zone_offset", location->time_zone_offset());
      }

      if (!location->time_zone_name().empty()) {
        writer("time_zone_name", location->time_zone_name());
      }

      if (location->waiting_secs()) {
        writer("waiting", static_cast<uint64_t>(location->waiting_secs()));
      }

      if (location->side_of_street() != valhalla::Location::kNone) {
        writer("side_of_street", Location_SideOfStreet_Enum_Name(location->side_of_street()));
      }

      writer("original_index", location->correlation().original_index());

      writer.end_object();
    }
  }

  writer.end_array();
}

// Serialize turn lane information
void turn_lanes(const TripLeg& leg,
                const DirectionsLeg_Maneuver& maneuver,
                rapidjson::writer_wrapper_t& writer) {

  // Read edge from a trip leg
  if (maneuver.begin_path_index() == 0 ||
      maneuver.begin_path_index() >= static_cast<uint32_t>(leg.node_size()))
    return;

  auto prev_index = maneuver.begin_path_index() - 1;
  const auto& prev_edge = leg.node(prev_index).edge();

  if (prev_edge.turn_lanes_size() > 1) {
    writer.start_array("lanes");

    for (const auto& turn_lane : prev_edge.turn_lanes()) {
      writer.start_object();

      // Directions as a bit mask
      writer("directions", turn_lane.directions_mask());

      if (turn_lane.state() == TurnLane::kActive) {
        writer("active", turn_lane.active_direction());
      } else if (turn_lane.state() == TurnLane::kValid) {
        writer("valid", turn_lane.active_direction());
      }

      writer.end_object();
    }

    writer.end_array();
  }
}

void legs(valhalla::Api& api, int route_index, rapidjson::writer_wrapper_t& writer) {
  writer.start_array("legs");
  const auto& directions_legs = api.directions().routes(route_index).legs();
  const bool is_truck_permit = api.options().costing_type() == Costing::truck_permit;
  unsigned int length_prec = api.options().units() == Options::miles ? 4 : 3;
  auto trip_leg_itr = api.mutable_trip()->mutable_routes(route_index)->mutable_legs()->begin();
  for (const auto& directions_leg : directions_legs) {
    valhalla::odin::EnhancedTripLeg etp(*trip_leg_itr);
    const auto truck_permit_index = is_truck_permit ? build_truck_permit_index(*trip_leg_itr)
                                                    : LegTruckPermitIndex{};
    writer.start_object(); // leg
    bool has_time_restrictions = false;
    bool has_toll = false;
    bool has_highway = false;
    bool has_ferry = false;

    if (directions_leg.maneuver_size())
      writer.start_array("maneuvers");

    int maneuver_index = 0;
    for (const auto& maneuver : directions_leg.maneuver()) {
      writer.start_object();

      // Maneuver type
      writer("type", static_cast<uint64_t>(maneuver.type()));

      // Instruction and verbal instructions
      writer("instruction", maneuver.text_instruction());
      if (!maneuver.verbal_transition_alert_instruction().empty()) {
        writer("verbal_transition_alert_instruction", maneuver.verbal_transition_alert_instruction());
      }
      if (!maneuver.verbal_succinct_transition_instruction().empty()) {
        writer("verbal_succinct_transition_instruction",
               maneuver.verbal_succinct_transition_instruction());
      }
      if (!maneuver.verbal_pre_transition_instruction().empty()) {
        writer("verbal_pre_transition_instruction", maneuver.verbal_pre_transition_instruction());
      }
      if (!maneuver.verbal_post_transition_instruction().empty()) {
        writer("verbal_post_transition_instruction", maneuver.verbal_post_transition_instruction());
      }

      // Set street names
      if (maneuver.street_name_size() > 0) {
        writer.start_array("street_names");
        for (int i = 0; i < maneuver.street_name_size(); i++) {
          writer(maneuver.street_name(i).value());
        }
        writer.end_array();
      }

      // Set begin street names
      if (maneuver.begin_street_name_size() > 0) {
        writer.start_array("begin_street_names");
        for (int i = 0; i < maneuver.begin_street_name_size(); i++) {
          writer(maneuver.begin_street_name(i).value());
        }
        writer.end_array();
      }

      // Set bearings
      // absolute bearing (degrees from north, clockwise) before and after the maneuver.
      bool depart_maneuver = (maneuver_index == 0);
      bool arrive_maneuver = (maneuver_index == directions_leg.maneuver_size() - 1);
      if (!depart_maneuver) {
        uint32_t node_index = maneuver.begin_path_index();
        uint32_t in_brg = etp.GetPrevEdge(node_index)->end_heading();
        writer("bearing_before", in_brg);
      }
      if (!arrive_maneuver) {
        uint32_t out_brg = maneuver.begin_heading();
        writer("bearing_after", out_brg);
      }

      // Time, length, cost, and shape indexes
      const auto& end_node = trip_leg_itr->node(maneuver.end_path_index());
      const auto& begin_node = trip_leg_itr->node(maneuver.begin_path_index());
      auto cost = end_node.cost().elapsed_cost().cost() - begin_node.cost().elapsed_cost().cost();

      writer.set_precision(tyr::kDefaultPrecision);
      writer("time", maneuver.time());
      writer.set_precision(length_prec);
      writer("length", maneuver.length());
      writer.set_precision(tyr::kDefaultPrecision);
      writer("cost", cost);
      writer("begin_shape_index", maneuver.begin_shape_index());
      writer("end_shape_index", maneuver.end_shape_index());
      auto recost_itr = api.options().recostings().begin();
      auto begin_recost_itr = begin_node.recosts().begin();
      for (const auto& end_recost : end_node.recosts()) {
        if (end_recost.has_elapsed_cost())
          writer("time_" + recost_itr->name(),
                 end_recost.elapsed_cost().seconds() - begin_recost_itr->elapsed_cost().seconds());
        else
          writer("time_" + recost_itr->name(), std::nullptr_t());
        ++recost_itr;
      }

      // Portions toll, highway, ferry and rough
      if (maneuver.portions_toll()) {
        writer("toll", maneuver.portions_toll());
        has_toll = true;
      }
      if (maneuver.portions_highway()) {
        writer("highway", maneuver.portions_highway());
        has_highway = true;
      }
      if (maneuver.portions_ferry()) {
        writer("ferry", maneuver.portions_ferry());
        has_ferry = true;
      }
      if (maneuver.portions_unpaved()) {
        writer("rough", maneuver.portions_unpaved());
      }
      if (maneuver.has_time_restrictions()) {
        writer("has_time_restrictions", maneuver.has_time_restrictions());
        has_time_restrictions = true;
      }

      // Process sign
      if (maneuver.has_sign()) {
        writer.start_object("sign");

        // Process exit number
        if (maneuver.sign().exit_numbers_size() > 0) {
          writer.start_array("exit_number_elements");
          for (int i = 0; i < maneuver.sign().exit_numbers_size(); ++i) {
            writer.start_object();
            // Add the exit number text
            writer("text", maneuver.sign().exit_numbers(i).text());
            // Add the exit number consecutive count only if greater than zero
            if (maneuver.sign().exit_numbers(i).consecutive_count() > 0) {
              writer("consecutive_count", maneuver.sign().exit_numbers(i).consecutive_count());
            }
            writer.end_object();
          }
          writer.end_array();
        }

        // Process exit branch
        if (maneuver.sign().exit_onto_streets_size() > 0) {
          writer.start_array("exit_branch_elements");
          for (int i = 0; i < maneuver.sign().exit_onto_streets_size(); ++i) {
            writer.start_object();
            // Add the exit branch text
            writer("text", maneuver.sign().exit_onto_streets(i).text());
            // Add the exit branch consecutive count only if greater than zero
            if (maneuver.sign().exit_onto_streets(i).consecutive_count() > 0) {
              writer("consecutive_count", maneuver.sign().exit_onto_streets(i).consecutive_count());
            }
            writer.end_object();
          }
          writer.end_array();
        }

        // Process exit toward
        if (maneuver.sign().exit_toward_locations_size() > 0) {
          writer.start_array("exit_toward_elements");
          for (int i = 0; i < maneuver.sign().exit_toward_locations_size(); ++i) {
            writer.start_object();
            // Add the exit toward text
            writer("text", maneuver.sign().exit_toward_locations(i).text());
            // Add the exit toward consecutive count only if greater than zero
            if (maneuver.sign().exit_toward_locations(i).consecutive_count() > 0) {
              writer("consecutive_count",
                     maneuver.sign().exit_toward_locations(i).consecutive_count());
            }
            writer.end_object();
          }
          writer.end_array();
        }

        // Process exit name
        if (maneuver.sign().exit_names_size() > 0) {
          writer.start_array("exit_name_elements");
          for (int i = 0; i < maneuver.sign().exit_names_size(); ++i) {
            writer.start_object();
            // Add the exit name text
            writer("text", maneuver.sign().exit_names(i).text());
            // Add the exit name consecutive count only if greater than zero
            if (maneuver.sign().exit_names(i).consecutive_count() > 0) {
              writer("consecutive_count", maneuver.sign().exit_names(i).consecutive_count());
            }
            writer.end_object();
          }
          writer.end_array();
        }

        writer.end_object(); // sign
      }

      // Roundabout count
      if (maneuver.roundabout_exit_count() > 0) {
        writer("roundabout_exit_count", maneuver.roundabout_exit_count());
      }

      // Depart and arrive instructions
      if (!maneuver.depart_instruction().empty()) {
        writer("depart_instruction", maneuver.depart_instruction());
      }
      if (!maneuver.verbal_depart_instruction().empty()) {
        writer("verbal_depart_instruction", maneuver.verbal_depart_instruction());
      }
      if (!maneuver.arrive_instruction().empty()) {
        writer("arrive_instruction", maneuver.arrive_instruction());
      }
      if (!maneuver.verbal_arrive_instruction().empty()) {
        writer("verbal_arrive_instruction", maneuver.verbal_arrive_instruction());
      }

      // Process transit route
      if (maneuver.has_transit_info()) {
        const auto& transit_info = maneuver.transit_info();
        writer.start_object("transit_info");

        if (!transit_info.onestop_id().empty()) {
          writer("onestop_id", transit_info.onestop_id());
        }
        if (!transit_info.short_name().empty()) {
          writer("short_name", transit_info.short_name());
        }
        if (!transit_info.long_name().empty()) {
          writer("long_name", transit_info.long_name());
        }
        if (!transit_info.headsign().empty()) {
          writer("headsign", transit_info.headsign());
        }
        writer("color", transit_info.color());
        writer("text_color", transit_info.text_color());
        if (!transit_info.description().empty()) {
          writer("description", transit_info.description());
        }
        if (!transit_info.operator_onestop_id().empty()) {
          writer("operator_onestop_id", transit_info.operator_onestop_id());
        }
        if (!transit_info.operator_name().empty()) {
          writer("operator_name", transit_info.operator_name());
        }
        if (!transit_info.operator_url().empty()) {
          writer("operator_url", transit_info.operator_url());
        }

        // Add transit stops
        if (transit_info.transit_stops().size() > 0) {
          writer.start_array("transit_stops");
          for (const auto& transit_stop : transit_info.transit_stops()) {
            writer.start_object();

            // type
            if (transit_stop.type() == TransitPlatformInfo_Type_kStation) {
              writer("type", "station");
            } else {
              writer("type", "stop");
            }

            // onestop_id - using the station onestop_id
            if (!transit_stop.station_onestop_id().empty()) {
              writer("onestop_id", transit_stop.station_onestop_id());
            }

            // name - using the station name
            if (!transit_stop.station_name().empty()) {
              writer("name", transit_stop.station_name());
            }

            // arrival_date_time
            if (!transit_stop.arrival_date_time().empty()) {
              writer("arrival_date_time", transit_stop.arrival_date_time());
            }

            // departure_date_time
            if (!transit_stop.departure_date_time().empty()) {
              writer("departure_date_time", transit_stop.departure_date_time());
            }

            // assumed_schedule
            writer("assumed_schedule", transit_stop.assumed_schedule());

            // latitude and longitude
            if (transit_stop.has_ll()) {
              writer.set_precision(tyr::kCoordinatePrecision);
              writer("lat", transit_stop.ll().lat());
              writer("lon", transit_stop.ll().lng());
            }

            writer.end_object(); // transit_stop
          }
          writer.end_array(); // transit_stops
        }
        writer.end_object(); // transit_info
      }

      if (maneuver.verbal_multi_cue()) {
        writer("verbal_multi_cue", maneuver.verbal_multi_cue());
      }

      //nevh//////      
      writer("turn_lanes", maneuver.turn_lanes());              
      //nevh//////

      // Travel mode
      auto mode_type = travel_mode_type(maneuver);
      writer("travel_mode", mode_type.first);

      // Travel type
      writer("travel_type", mode_type.second);

      if (is_truck_permit) {
        const auto details =
            collect_truck_permit_details(truck_permit_index, maneuver, trip_leg_itr->node_size());
        writer("truck_permit_restriction_type", restriction_category(details));
        writer.start_array("truck_permit_ban_timings");
        for (const auto& window : details.timed_windows) {
          writer(window);
        }
        writer.end_array();
      }

      //  man->emplace("hasGate", maneuver.);
      //  man->emplace("hasFerry", maneuver.);
      // “portionsTollNote” : “<portionsTollNote>”,
      // “portionsUnpavedNote” : “<portionsUnpavedNote>”,
      // “gateAccessRequiredNote” : “<gateAccessRequiredNote>”,
      // “checkFerryInfoNote” : “<checkFerryInfoNote>”

      // Add Line info if enabled
      if (api.options().turn_lanes()) {
        turn_lanes(*trip_leg_itr, maneuver, writer);
      }

      writer.end_object(); // maneuver
      maneuver_index++;
    }
    if (directions_leg.maneuver_size()) {
      writer.end_array(); // maneuvers
    }

    // Store elevation for the leg
    if (api.options().elevation_interval() > 0.0f) {
      writer.set_precision(1);
      float unit_factor = api.options().units() == Options::miles ? kFeetPerMeter : 1.0f;
      float interval = api.options().elevation_interval();
      writer("elevation_interval", interval * unit_factor);
      auto elevation = tyr::get_elevation(*trip_leg_itr, interval);

      writer.start_array("elevation");
      for (const auto& h : elevation) {
        writer(h * unit_factor);
      }
      writer.end_array(); // elevation
    }

    writer.start_object("summary");

    // Does the user want admin info?
    if (api.options().admin_crossings()) {
      // write the admin array
      writer.start_array("admins");
      for (const auto& admin : trip_leg_itr->admin()) {
        writer.start_object();
        writer("country_code", admin.country_code());
        writer("country_text", admin.country_text());
        writer("state_code", admin.state_code());
        writer("state_text", admin.state_text());
        writer.end_object();
      }
      writer.end_array();

      if (trip_leg_itr->admin_size() > 1) {
        // write the admin crossings
        auto node_itr = trip_leg_itr->node().begin();
        auto next_node_itr = trip_leg_itr->node().begin();
        next_node_itr++;
        writer.start_array("admin_crossings");

        while (next_node_itr != trip_leg_itr->node().end()) {
          if (next_node_itr->admin_index() != node_itr->admin_index()) {
            writer.start_object();
            writer("from_admin_index", node_itr->admin_index());
            writer("to_admin_index", next_node_itr->admin_index());
            writer("begin_shape_index", node_itr->edge().begin_shape_index());
            writer("end_shape_index", node_itr->edge().end_shape_index());
            writer.end_object();
          }
          ++node_itr;
          ++next_node_itr;
        }
        writer.end_array();
      }
    }

    // are there any level changes along the leg
    if (directions_leg.level_changes().size() > 0) {
      writer.start_array("level_changes");
      for (auto& level_change : directions_leg.level_changes()) {
        writer.start_array();
        writer(level_change.shape_index());
        writer.set_precision(std::max(level_change.precision(), static_cast<uint32_t>(1)));
        writer(level_change.level());
        writer.set_precision(tyr::kDefaultPrecision);
        writer.end_array();
      }
      writer.end_array();
    }

    writer("has_time_restrictions", has_time_restrictions);
    writer("has_toll", has_toll);
    writer("has_highway", has_highway);
    writer("has_ferry", has_ferry);
    writer.set_precision(tyr::kCoordinatePrecision);
    writer("min_lat", directions_leg.summary().bbox().min_ll().lat());
    writer("min_lon", directions_leg.summary().bbox().min_ll().lng());
    writer("max_lat", directions_leg.summary().bbox().max_ll().lat());
    writer("max_lon", directions_leg.summary().bbox().max_ll().lng());
    writer.set_precision(tyr::kDefaultPrecision);
    writer("time", directions_leg.summary().time());
    writer.set_precision(length_prec);
    writer("length", directions_leg.summary().length());
    writer.set_precision(tyr::kDefaultPrecision);
    writer("cost", trip_leg_itr->node().rbegin()->cost().elapsed_cost().cost());
    //get nevh_version from options
    auto nevh_version = (uint32_t)api.options().nevh_version();
    //writer("nevh_version", nevh_version); //we write to trip summary..not here, this is for each leg

    
    auto recost_itr = api.options().recostings().begin();
    for (const auto& recost : trip_leg_itr->node().rbegin()->recosts()) {
      if (recost.has_elapsed_cost())
        writer("time_" + recost_itr->name(), recost.elapsed_cost().seconds());
      else
        writer("time_" + recost_itr->name(), std::nullptr_t());
      ++recost_itr;
    }
    ++trip_leg_itr;
    writer.end_object();

    writer("shape", directions_leg.shape());
    
    //nevh//////
    //write speed_limits as a int array

    // for(int s = 0; s < directions_leg.speed_limits_size(); ++s){
    //   std::cout << s%6 << ":" << directions_leg.speed_limits(s) << " ";
    // }
    // std::cout << std::endl;
    if(nevh_version == 0) {
      if(directions_leg.speed_limits_size() > 0) {
        writer.start_array("speed_limits_lanes");
        int writeSpeedC = 0;
        for(int s = 0; s < directions_leg.speed_limits_size(); ++s){
          if(writeSpeedC == 2 || writeSpeedC == 4) {
            writeSpeedC++;
            continue;
          }          
          else if(writeSpeedC == 3) {
            if(valhalla::Options_Units_Enum_Name(api.options().units()) == "miles") {
              // convert from km/h to mph
              //round number to nearest integer multiple of 5
              auto speed_limit = static_cast<uint64_t>(directions_leg.speed_limits(s) * 0.621371);
              speed_limit = (speed_limit + 2) / 5 * 5; // round to nearest 5
              writer(speed_limit);
            } else {
              // keep as km/h
              writer(static_cast<uint64_t>(directions_leg.speed_limits(s)));
            }
            writeSpeedC++;
          }          
          else if(writeSpeedC == 5) {            
            writer(static_cast<uint64_t>(directions_leg.speed_limits(s)));
            writeSpeedC = 0; // reset counter
          }
          else {
            writer(static_cast<uint64_t>(directions_leg.speed_limits(s)));
            writeSpeedC++;
          }
        }
        writer.end_array();
      }
    }
    else if(nevh_version == 1) {    
      if(directions_leg.speed_limits_size() > 0) {
        writer.start_array("speed_limits_lanes");
        int writeSpeedC = 0;
        
        for(int s = 0; s < directions_leg.speed_limits_size(); ++s){
          //int v = directions_leg.speed_limits(s);
          if(writeSpeedC == 3) {
            if(valhalla::Options_Units_Enum_Name(api.options().units()) == "miles") {
              // convert from km/h to mph
              //round number to nearest integer multiple of 5
              auto speed_limit = static_cast<uint64_t>(directions_leg.speed_limits(s) * 0.621371);
              speed_limit = (speed_limit + 2) / 5 * 5; // round to nearest 5
              writer(speed_limit);
            } else {
              // keep as km/h
              writer(static_cast<uint64_t>(directions_leg.speed_limits(s)));
            }
            writeSpeedC++;
          }
          else if(writeSpeedC == 5) {            
            writer(static_cast<uint64_t>(directions_leg.speed_limits(s)));
            writeSpeedC = 0; // reset counter
          }
          else {
            writer(static_cast<uint64_t>(directions_leg.speed_limits(s)));
            writeSpeedC++;
          }
        }
        writer.end_array();
      }    
    }
    if(directions_leg.speed_cameras_size() > 0) {
      writer.start_array("speed_cameras");
      for(int s = 0; s < directions_leg.speed_cameras_size(); ++s){
        writer(static_cast<uint64_t>(directions_leg.speed_cameras(s)));
      }
      writer.end_array();
    }
    if(directions_leg.road_attributes_size() > 0) {
      writer.start_array("road_attributes");
      for(int s = 0; s < directions_leg.road_attributes_size(); ++s){
        writer(static_cast<uint64_t>(directions_leg.road_attributes(s)));
      }
      writer.end_array();
    }
    //nevh//////

    writer.end_object(); // leg
  }
  writer.end_array(); // legs
}
} // namespace

namespace valhalla_serializers {
std::string serialize(Api& api) {
  // build up the json object, reserve 4k bytes
  rapidjson::writer_wrapper_t writer(4096);

  // for each route
  for (int i = 0; i < api.directions().routes_size(); ++i) {
    if (i == 1) {
      writer.start_array("alternates");
    }

    // the route itself
    writer.start_object();
    writer.start_object("trip");

    // the locations in the trip
    locations(api, i, writer);

    // the actual meat of the route
    legs(api, i, writer);

    // openlr references of the edges in the route
    valhalla::tyr::openlr(api, i, writer);

    // summary time/distance and other stats
    summary(api, i, writer);

    // get serialized warnings
    if (api.info().warnings_size() >= 1) {
      valhalla::tyr::serializeWarnings(api, writer);
    }

    writer.end_object(); // trip

    // leave space for alternates by closing this one outside the loop
    if (i > 0) {
      writer.end_object();
    }
  }

  if (api.directions().routes_size() > 1) {
    writer.end_array(); // alternates
  }

  if (api.options().has_id_case()) {
    writer("id", api.options().id());
  }

  writer.end_object(); // outer object

  return writer.get_buffer();
}
} // namespace valhalla_serializers
