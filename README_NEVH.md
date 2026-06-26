# Valhalla NEVH Route Response — Complete Feature Guide

> **Audience:** Client-side developers consuming the Valhalla `/route` JSON response.
> **Version:** `nevhversion = "1"` in `summary`.
> **Endpoints:** `POST /route` with JSON body (see [Valhalla API docs](https://valhalla.github.io/valhalla/api/turn-by-turn/api-reference/)).
> **Test maps:** `test/gurka/test_abu_dhabi_features.cc` + `test/gurka/test_nevh_features.cc`

---

## Table of Contents

1. [Full JSON Response Structure](#1-full-json-response-structure)
2. [Summary](#2-summary)
3. [Shape (Route Geometry)](#3-shape-route-geometry)
4. [Maneuvers](#4-maneuvers)
5. [speed_limits Array [NEVH]](#5-speed_limits-array-nevh)
6. [road_attributes Array [NEVH]](#6-road_attributes-array-nevh)
7. [speed_cameras Array [NEVH]](#7-speed_cameras-array-nevh)
8. [Trip Path (Per-Edge Data)](#8-trip-path-per-edge-data)
9. [NEVH Data Pipeline](#9-nevh-data-pipeline)
10. [Client Implementation Guide](#10-client-implementation-guide)
11. [Building & Running Tests](#11-building--running-tests)

---

## 1. Full JSON Response Structure

The `/route` endpoint returns both the raw trip path (`trip`) and turn-by-turn directions (`directions`). NEVH additions are marked with **[NEVH]**.

```
{
  "trip": {
    "routes": [{
      "legs": [{
        "nodes": [{ "edge": { ... } }]        // Per-edge detail (see §8)
      }]
    }]
  },
  "directions": {
    "routes": [{
      "legs": [{
        "summary": { ... },                   // Route aggregates (§2)
        "shape": "polyline6_string",          // Encoded geometry (§3)
        "maneuvers": [ ... ],                 // Turn instructions (§4)
        "speed_limits": [ ... ],              // [NEVH] §5
        "road_attributes": [ ... ],           // [NEVH] §6
        "speed_cameras": [ ... ],             // [NEVH] §7
        "level_changes": [ ... ]
      }]
    }]
  }
}
```

---

## 2. Summary

Route-level aggregates. Access: `directions.routes[0].legs[0].summary`

| Field | Type | Description |
|-------|------|-------------|
| `length` | float | Total route length (km or miles per `units` request param) |
| `time` | float | Estimated travel time in seconds |
| `bbox` | object | `{min_ll:{lat,lng}, max_ll:{lat,lng}}` |
| `has_toll` | bool | Route includes toll roads |
| `has_highway` | bool | Route includes motorway segments |
| `has_ferry` | bool | Route includes ferry crossings |
| `has_time_restrictions` | bool | Time-conditional access present |
| **`nevhversion`** | **string** | **[NEVH]** Always `"1"` |

---

## 3. Shape (Route Geometry)

`leg.shape` — **Polyline6** encoded string (6 decimal digits).

> ⚠️ Using polyline5 (Google default) places points in the ocean. Always use **precision=6**.

```javascript
import polyline from '@mapbox/polyline';
const coords = polyline.decode(leg.shape, 6); // [[lat, lng], ...]
```

`begin_shape_index` / `end_shape_index` in speed_limits and maneuvers index into this array.

---

## 4. Maneuvers

Turn-by-turn instructions. Access: `leg.maneuvers[]`.

### 4.1 Core Fields

| Field | Type | Description |
|-------|------|-------------|
| `type` | int | 1=start, 4=dest, 8=continue, 10=right, 15=left, 26=enter_roundabout, etc. |
| `length` | float | Distance (km or miles) |
| `time` | float | Seconds |
| `begin_shape_index` | int | Start index into shape polyline |
| `end_shape_index` | int | End index into shape polyline |
| `begin_path_index` | int | Start index into trip.nodes[] |
| `end_path_index` | int | End index into trip.nodes[] |
| `travel_mode` | int | 0=drive, 1=pedestrian, 2=bicycle, 3=transit |
| `turn_degree` | int | 0–359 (0=straight, 90=right, 180=U-turn, 270=left) |
| `begin_heading` | int | 0–359 |
| `begin_cardinal_direction` | int | 0=N, 1=NE, 2=E, 3=SE, 4=S, 5=SW, 6=W, 7=NW |
| `instruction` | string | Human-readable (localized) |
| `verbal_pre_transition_instruction` | string | Verbal alert before |
| `verbal_post_transition_instruction` | string | Verbal alert after |
| `street_name` | string[] | Street names |
| `begin_street_name` | string[] | Names at maneuver start (if different) |
| `portions_toll` | bool | Has toll sections |
| `portions_unpaved` | bool | Has unpaved sections |
| `portions_highway` | bool | Has motorway sections |
| `portions_ferry` | bool | Has ferry |
| `has_time_restrictions` | bool | Time-conditional access |
| `to_stay_on` | bool | Continue on same-named road |
| `sign` | object | Exit/guide signs (numbers, branch names, toward) |
| **`turn_lanes`** | **string** | **[NEVH]** Active lane guidance (§4.2) |

### 4.2 `turn_lanes` [NEVH]

Format: `"<end_shape_index>:<directions>[#<next_segment>]"`

Directions: `left|slight_left|through|slight_right|right|sharp_left|sharp_right|reverse`
`ACTIVE` marks the recommended lane(s).

**OSM tag:** [`turn:lanes=*`](https://wiki.openstreetmap.org/wiki/Key:turn:lanes)

```javascript
function parseTurnLanes(str) {
  if (!str) return [];
  return str.split('#').map(s => {
    const [idx, lanes] = s.split(':');
    return {
      endShapeIndex: +idx,
      lanes: lanes.split('|').map(l => ({
        direction: l.replace(';ACTIVE','').replace('ACTIVE',''),
        active: l.includes('ACTIVE')
      }))
    };
  });
}
```

### 4.3 Maneuver Type Enum

| Type | Name | Type | Name |
|------|------|------|------|
| 1 | kStart | 15 | kLeft |
| 4 | kDestination | 18 | kRampRight |
| 8 | kContinue | 19 | kRampLeft |
| 9 | kSlightRight | 20 | kExitRight |
| 10 | kRight | 21 | kExitLeft |
| 11 | kSharpRight | 22 | kStayStraight |
| 12 | kUturnRight | 23 | kStayRight |
| 13 | kUturnLeft | 24 | kStayLeft |
| 14 | kSharpLeft | 25 | kMerge |
| 16 | kSlightLeft | 26 | kRoundaboutEnter |
| 17 | kRampStraight | 27 | kRoundaboutExit |

---

## 5. `speed_limits` Array [NEVH]

**Proto:** `repeated uint32 speed_limits = 9`

Six `uint32_t` per road segment. Consecutive edges with identical attributes are coalesced.

| Offset | Name | Description |
|--------|------|-------------|
| 0 | `begin_shape_index` | Inclusive index into `shape` polyline |
| 1 | `end_shape_index` | Inclusive index into `shape` polyline |
| 2 | `way_id` | OSM way ID |
| 3 | `speed_limit` | **km/h** (0=unknown, 255=unlimited) |
| 4 | `u32edgeSpeed` | `(edge_speed & 0xFFFF) \| ((default_speed & 0xFFFF) << 16)` |
| 5 | `lane_count` | 1–15 |

**OSM tags:** [`maxspeed=*`](https://wiki.openstreetmap.org/wiki/Key:maxspeed), [`lanes=*`](https://wiki.openstreetmap.org/wiki/Key:lanes)

```javascript
function parseSpeedLimits(arr) {
  const segs = [];
  for (let i = 0; i < arr.length; i += 6) {
    segs.push({
      beginShapeIndex: arr[i], endShapeIndex: arr[i+1], wayId: arr[i+2],
      speedLimitKmh: arr[i+3],
      edgeSpeedKmh: arr[i+4] & 0xFFFF,
      defaultSpeedKmh: (arr[i+4] >> 16) & 0xFFFF,
      laneCount: arr[i+5]
    });
  }
  return segs;
}
```

---

## 6. `road_attributes` Array [NEVH]

**Proto:** `repeated uint32 road_attributes = 11`

One `uint32_t` per segment, **parallel** to `speed_limits`. 17 attributes packed in 31 bits.

### 6.1 Bit Layout

| Bits | Field | Values | OSM Tag | Wiki |
|------|-------|--------|---------|------|
| 0–2 | `road_class` | 0=motorway..7=service | `highway=*` | [🔗](https://wiki.openstreetmap.org/wiki/Key:highway) |
| 3–8 | `use` | 0=road,1=ramp,11=service,20=cycleway,25=footway,41=ferry | `highway=*` | [🔗](https://wiki.openstreetmap.org/wiki/Key:highway) |
| 9 | `bridge` | 0/1 | `bridge=yes` | [🔗](https://wiki.openstreetmap.org/wiki/Key:bridge) |
| 10 | `tunnel` | 0/1 | `tunnel=yes` | [🔗](https://wiki.openstreetmap.org/wiki/Key:tunnel) |
| 11 | `roundabout` | 0/1 | `junction=roundabout` | [🔗](https://wiki.openstreetmap.org/wiki/Key:junction) |
| 12 | `indoor` | 0/1 | `indoor=yes` | [🔗](https://wiki.openstreetmap.org/wiki/Key:indoor) |
| 13 | `has_level_changes` | 0/1 | `level=*` | [🔗](https://wiki.openstreetmap.org/wiki/Key:level) |
| 14 | `toll` | 0/1 | `toll=yes` | [🔗](https://wiki.openstreetmap.org/wiki/Key:toll) |
| 15 | `truck_route` | 0/1 | `hgv=designated` | [🔗](https://wiki.openstreetmap.org/wiki/Key:hgv) |
| 16 | `has_time_restrictions` | 0/1 | `*:conditional` | [🔗](https://wiki.openstreetmap.org/wiki/Conditional_restrictions) |
| 17 | `traffic_signal` | 0/1 | `highway=traffic_signals` (node) | [🔗](https://wiki.openstreetmap.org/wiki/Tag:highway%3Dtraffic_signals) |
| 18 | `speed_camera` | 0/1 | `highway=speed_camera` | [🔗](https://wiki.openstreetmap.org/wiki/DE:Tag:highway%3Dspeed_camera) |
| 19 | `shoulder` | 0/1 | `shoulder=yes` | [🔗](https://wiki.openstreetmap.org/wiki/Key:shoulder) |
| 20 | `bicycle_network` | 0/1 | `ncn/rcn/lcn/mtb=yes` | [🔗](https://wiki.openstreetmap.org/wiki/Cycle_routes) |
| 21 | `destination_only` | 0/1 | `access=private/destination` | [🔗](https://wiki.openstreetmap.org/wiki/Key:access) |
| 22 | `unpaved` | 0/1 | derived: `surface >= compacted` | [🔗](https://wiki.openstreetmap.org/wiki/Key:surface) |
| 23 | `country_crossing` | 0/1 | computed (admin boundary) | — |
| 24–25 | `sidewalk` | 0=none,1=left,2=right,3=both | `sidewalk=*` | [🔗](https://wiki.openstreetmap.org/wiki/Key:sidewalk) |
| 26–27 | `cycle_lane` | 0=none,1=shared,2=ded,3=sep | `cycleway=*` | [🔗](https://wiki.openstreetmap.org/wiki/Key:cycleway) |
| 28–30 | `surface` | 0=paved_smooth..7=impassable | `surface=*` | [🔗](https://wiki.openstreetmap.org/wiki/Key:surface) |
| 31 | `spare` | always 0 | — | — |

### 6.2 Consumer Code

```javascript
function unpackRoadAttributes(attrs) {
  return {
    roadClass: attrs & 0x7, use: (attrs>>3) & 0x3F,
    bridge: !!((attrs>>9)&1), tunnel: !!((attrs>>10)&1),
    roundabout: !!((attrs>>11)&1), indoor: !!((attrs>>12)&1),
    hasLevelChanges: !!((attrs>>13)&1),
    toll: !!((attrs>>14)&1), truckRoute: !!((attrs>>15)&1),
    hasTimeRestrictions: !!((attrs>>16)&1),
    trafficSignal: !!((attrs>>17)&1), speedCamera: !!((attrs>>18)&1),
    shoulder: !!((attrs>>19)&1), bicycleNetwork: !!((attrs>>20)&1),
    destinationOnly: !!((attrs>>21)&1), unpaved: !!((attrs>>22)&1),
    countryCrossing: !!((attrs>>23)&1),
    sidewalk: (attrs>>24)&3, cycleLane: (attrs>>26)&3, surface: (attrs>>28)&7,
  };
}

// Merge with speed_limits for complete per-segment view:
const segments = parseSpeedLimits(leg.speed_limits);
leg.road_attributes.forEach((a,i) => { if(i<segments.length) segments[i].attrs = unpackRoadAttributes(a); });
```

---

## 7. `speed_cameras` Array [NEVH]

**Proto:** `repeated uint32 speed_cameras = 10`

Each entry is a **node index** into `trip.legs[leg].nodes[]`.

**OSM:** [`highway=speed_camera`](https://wiki.openstreetmap.org/wiki/DE:Tag:highway%3Dspeed_camera) on way.

```javascript
const tripNodes = trip.routes[0].legs[0].nodes;
leg.speed_cameras.forEach(ni => {
  const e = tripNodes[ni]?.edge;
  console.log(`Camera at node ${ni}, limit=${e?.speed_limit} km/h`);
});
```

---

## 8. Trip Path (Per-Edge Data)

`trip.routes[].legs[].nodes[].edge` — detailed edge attributes.

### 8.1 Original Valhalla Edge Fields

| Field | Type | # | OSM Tag |
|-------|------|---|---------|
| `length_km` | float | 2 | — |
| `speed` | float | 3 | — |
| `road_class` | enum | 4 | `highway=*` |
| `begin_heading` | uint32 | 5 | — |
| `end_heading` | uint32 | 6 | — |
| `begin_shape_index` | uint32 | 7 | — |
| `end_shape_index` | uint32 | 8 | — |
| `use` | enum | 10 | `highway=*` |
| `toll` | bool | 11 | `toll=yes` |
| `unpaved` | bool | 12 | derived |
| `tunnel` | bool | 13 | `tunnel=yes` |
| `bridge` | bool | 14 | `bridge=yes` |
| `roundabout` | bool | 15 | `junction=roundabout` |
| `surface` | enum | 18 | `surface=*` |
| `way_id` | uint64 | 27 | — |
| `lane_count` | uint32 | 31 | `lanes=*` |
| `cycle_lane` | enum | 32 | `cycleway=*` |
| `bicycle_network` | bool | 33 | `ncn/rcn/lcn=yes` |
| `sidewalk` | enum | 34 | `sidewalk=*` |
| `speed_limit` | uint32 | 36 | `maxspeed=*` |
| `truck_route` | bool | 38 | `hgv=designated` |
| `has_time_restrictions` | bool | 43 | `*:conditional` |
| `default_speed` | float | 44 | — |
| `destination_only` | bool | 46 | `access=private` |
| `shoulder` | bool | 52 | `shoulder=yes` |
| `indoor` | bool | 53 | `indoor=yes` |

### 8.2 NEVH Edge Fields

| Field | Type | # | OSM Tag |
|-------|------|---|---------|
| `traffic_signal` | bool | 66 | `highway=traffic_signals` (node) |
| `speed_camera` | bool | 68 | `highway=speed_camera` |
| `levels` | repeated | 61 | `level=*` |

---

## 9. NEVH Data Pipeline

```
OSM Tags → lua/graph.lua → pbfgraphparser.cc → graphbuilder.cc (DirectedEdge)
  → triplegbuilder.cc (TripLeg_Edge)
  → maneuversbuilder.cc:
      GetSpeedLimits()    → DirectionsLeg.speed_limits [9]
      GetRoadAttributes() → DirectionsLeg.road_attributes [11]
      GetSpeedCams()      → DirectionsLeg.speed_cameras [10]
      GetTurnLanes()      → Maneuver.turn_lanes [42]
  → directionsbuilder.cc → route_serializer_valhalla.cc → JSON
```

---

## 10. Client Implementation Guide

### Quick Start

```javascript
async function getRoute(origin, dest) {
  const resp = await fetch('/route', { method:'POST',
    body: JSON.stringify({ locations:[origin,dest], costing:'auto',
      directions_type:'instructions', units:'kilometers' }) });
  const data = await resp.json();
  const leg = data.directions.routes[0].legs[0];

  if (leg.summary?.nevhversion !== '1') {
    console.warn('NEVH extensions not available');
    return data;
  }

  const coords = polyline.decode(leg.shape, 6);
  const segs = parseSpeedLimits(leg.speed_limits);
  leg.road_attributes.forEach((a,i) => { segs[i].attrs = unpackRoadAttributes(a); });
  const cameras = leg.speed_cameras.map(n => ({
    nodeIndex: n, edge: data.trip.routes[0].legs[0].nodes[n]?.edge
  }));

  return { summary: leg.summary, coords, segments: segs, cameras,
           maneuvers: leg.maneuvers, tripNodes: data.trip.routes[0].legs[0].nodes };
}
```

### Display Use Cases

**Speed limits on map:**
```javascript
segs.forEach(s => {
  const [start, end] = [coords[s.beginShapeIndex], coords[s.endShapeIndex]];
  // Draw colored polyline; show badge `${s.speedLimitKmh} km/h`
});
```

**Speed camera alerts:** Iterate `cameras`, show alerts at camera locations.

**Toll detection:** `segs.some(s => s.attrs?.toll)` → show toll warning.

**Lane guidance:** Parse `maneuver.turn_lanes` → show lane diagram with highlighted ACTIVE lanes.

**Surface warnings:** `segs.filter(s => s.attrs?.unpaved)` → warn about unpaved sections.

---

## 11. Building & Running Tests

```bash
cd build
cmake .. -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DPROTOBUF_INCLUDE_DIR=$(pwd)/../third_party/protobuf-3.12.3/src \
  -Dprotobuf_MODULE_COMPATIBLE=ON -DminizIncludePath=$(pwd)/../tmp/stub_include \
  -DENABLE_TESTS=ON -DENABLE_DATA_TOOLS=ON
cmake --build . -j$(nproc) --target gurka_nevh_features
cmake --build . -j$(nproc) --target gurka_abu_dhabi_features
./test/gurka/gurka_nevh_features   # 14 tests
./test/gurka/gurka_abu_dhabi_features  # 11 tests
```

**25 tests, all passing** ✅

---

## Appendix: OSM Tag Quick Reference

| Feature | OSM Tag | Wiki |
|---------|---------|------|
| Speed limit | `maxspeed=*` | [Key:maxspeed](https://wiki.openstreetmap.org/wiki/Key:maxspeed) |
| Lane count | `lanes=*` | [Key:lanes](https://wiki.openstreetmap.org/wiki/Key:lanes) |
| Road class | `highway=*` | [Key:highway](https://wiki.openstreetmap.org/wiki/Key:highway) |
| Bridge | `bridge=yes` | [Key:bridge](https://wiki.openstreetmap.org/wiki/Key:bridge) |
| Tunnel | `tunnel=yes` | [Key:tunnel](https://wiki.openstreetmap.org/wiki/Key:tunnel) |
| Roundabout | `junction=roundabout` | [Key:junction](https://wiki.openstreetmap.org/wiki/Key:junction) |
| Toll road | `toll=yes` | [Key:toll](https://wiki.openstreetmap.org/wiki/Key:toll) |
| Speed camera | `highway=speed_camera` | [DE:Tag](https://wiki.openstreetmap.org/wiki/DE:Tag:highway%3Dspeed_camera) |
| Traffic signal | `highway=traffic_signals` | [Tag](https://wiki.openstreetmap.org/wiki/Tag:highway%3Dtraffic_signals) |
| Surface | `surface=*` | [Key:surface](https://wiki.openstreetmap.org/wiki/Key:surface) |
| Sidewalk | `sidewalk=*` | [Key:sidewalk](https://wiki.openstreetmap.org/wiki/Key:sidewalk) |
| Cycle lane | `cycleway=*` | [Key:cycleway](https://wiki.openstreetmap.org/wiki/Key:cycleway) |
| Shoulder | `shoulder=yes` | [Key:shoulder](https://wiki.openstreetmap.org/wiki/Key:shoulder) |
| Truck route | `hgv=designated` | [Key:hgv](https://wiki.openstreetmap.org/wiki/Key:hgv) |
| Destination only | `access=private/destination` | [Key:access](https://wiki.openstreetmap.org/wiki/Key:access) |
| Building levels | `level=*` | [Key:level](https://wiki.openstreetmap.org/wiki/Key:level) |
| Indoor | `indoor=yes` | [Key:indoor](https://wiki.openstreetmap.org/wiki/Key:indoor) |
| Time restrictions | `*:conditional` | [Conditional](https://wiki.openstreetmap.org/wiki/Conditional_restrictions) |
| Turn lanes | `turn:lanes=*` | [Key:turn:lanes](https://wiki.openstreetmap.org/wiki/Key:turn:lanes) |
