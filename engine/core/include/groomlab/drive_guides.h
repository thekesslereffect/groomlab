// SPDX-License-Identifier: Apache-2.0
#pragma once

// Driver curves: a few curves an animator rigs (joints, spline IK, bones: the host's own rigging)
// that deform the guides, as Pixar's and Disney's hair rigs pose hero hair before a simulation
// follows. The groom holds the drivers twice:
//
//   - as they are now (GroomData::drivers, world space: the host's curves at the evaluation time),
//   - as they were bound (DriverBind, GroomData::driverBindJson): the drivers at the bind frame
//     (frame 0 by default), each carried to the scalp's rest pose by its anchor (the scalp point
//     nearest its first point then), so the bind holds whatever the character did at that frame.
//
// A DriveGuides node binds each guide (or strand) to its nearest drivers as bound, point by point
// (the closest point of each driver and the offset from it in the driver's rotation-minimizing
// frame there), and carries the offsets along the drivers as they are now. Drivers as bound and as
// they are now go into the space the graph evaluates in (driversInGraphSpace): the scalp's rest
// pose while following the scalp, else the scalp as it is; each driver is matched by ID.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "groomlab/graph.h"
#include "groomlab/guide_set.h"
#include "groomlab/math.h"
#include "groomlab/scalp_mesh.h"
#include "groomlab/strand_set.h"

namespace groomlab {

class Ramp;

// The externals a DriveGuides node reads (GuidesPtr each, in the graph's space).
inline constexpr const char* kDriversExternal = "drivers";        // the drivers now
inline constexpr const char* kDriverBindExternal = "driverBind";  // the drivers as bound

// The drivers as bound: each with its ID, its points on the scalp's rest pose and its anchor
// there (Guide::root: the face the driver rides when the scalp moves; face -1: none, the driver
// stays where it is with the scalp).
struct DriverBind {
    int frame = 0;  // the frame bound at (shown to the user; nothing reads it back)
    GuideSet drivers;

    bool empty() const { return drivers.empty(); }
    std::string toJson() const;
    // Throws std::invalid_argument ("driverBind: <why>") on malformed text; empty text: no bind.
    static DriverBind fromJson(const std::string& text);
};

// The bind of `drivers` (world space, as they are on `scalp` at `frame`): each anchored at the
// scalp point nearest its first point and carried by that anchor's motion to the scalp's rest
// pose (the scalp itself when it has none). Drivers of fewer than 2 points are left out. Without
// a scalp (null or empty), the drivers as they are, anchored nowhere.
DriverBind bindDrivers(const GuideSet& drivers, const ScalpMesh* scalp, int frame);

// The drivers as bound and as they are now, both in the space the graph evaluates in: `graph` is
// the scalp the graph evaluates on (the rest pose while following the scalp, else the scalp now),
// `rest` the scalp's rest pose, `now` the scalp now (any may be null: no scalp, nothing carried).
// The bound ones are carried from `rest` to `graph` by their anchors, the current ones from `now`
// to `graph` by their bound driver's anchor (a driver with no bound one of its ID stays as it is).
struct DriversInGraph {
    GuidesPtr bound, now;
};
DriversInGraph driversInGraphSpace(const DriverBind& bind, const GuidesPtr& drivers, const ScalpMesh* rest,
                                   const ScalpMesh* now, const ScalpMesh* graph);

// How a curve follows its drivers.
struct DriveSettings {
    int drivers = 2;     // the nearest drivers each curve follows, blended by distance (1..8)
    float reach = 0.0f;  // > 0: curves past half this from every driver follow less, none past it; 0: all follow
};

// Each curve's drivers (as bound) and each point's place on them: what driveCurves carries.
struct DriveBinding {
    struct Hold {
        std::uint32_t driver = 0;   // its index in the bound drivers
        std::uint32_t segment = 0;  // the point's closest segment of it
        float t = 0.0f;             // and where along it (0..1)
        Vec3f local0, local1;       // the offset from that point in the frames at the segment's ends
    };
    std::size_t perPoint = 0;              // holds per point (the drivers each curve follows)
    std::vector<std::size_t> pointOffset;  // per curve, its first point's index into holds / perPoint
    std::vector<Hold> holds;               // per point, perPoint of them (weight 0: unused)
    std::vector<float> weights;            // per curve, perPoint driver weights (summing to its influence)
    std::uint64_t key = 0;                 // what it was made from (bindingKey)
};

// What a binding depends on: the curves' points, the bound drivers and the settings.
std::uint64_t driveBindingKey(const StrandSet& curves, const GuideSet& bound, const DriveSettings& settings);
DriveBinding bindToDrivers(const StrandSet& curves, const GuideSet& bound, const DriveSettings& settings);

// The curves carried along their drivers from `bound` to `now` (matched by ID; a driver missing now,
// or of another point count, stays as bound): each point but the root moves by the carry times
// amount(curve) x profile along the length. `curves` must be what `binding` was made for.
void driveCurves(StrandSet& curves, const DriveBinding& binding, const GuideSet& bound, const GuideSet& now,
                 const std::vector<float>& amount, const Ramp& profile);

}  // namespace groomlab
