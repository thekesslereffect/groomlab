// SPDX-License-Identifier: Apache-2.0
// Whole-groom deformers: Collide (push strands out of the scalp and any other
// mesh), Part (open the parts), Gravity (bend strands under their weight) and
// Displace (offset them along the scalp's normal).

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <mutex>
#include <vector>

#include "groomlab/distance_field.h"
#include "groomlab/field.h"
#include "groomlab/graph.h"
#include "groomlab/look/look_cpu.h"
#include "groomlab/operator_state.h"
#include "groomlab/operators.h"
#include "groomlab/parallel.h"
#include "groomlab/portable_math.h"
#include "groomlab/preview_core.h"
#include "incremental.h"
#include "internal.h"

namespace groomlab {

namespace {

// Tests: setCollideStrandBounds, collideSkippedSurfaces.
std::atomic<bool> collideStrandBounds{true};
std::atomic<std::size_t> collideSkipped{0};

// Turns each segment of a strand toward `dir` by amount x the profile there,
// keeping segment lengths (Part's opening); the root stays. Reads each original point before
// overwriting it, so no copy of the strand is needed.
void turnToward(Vec3f* pts, std::size_t n, const Vec3f& dir, float amount, const RampAlong& profile) {
    Vec3f prevOriginal = pts[0];
    for (std::size_t i = 1; i < n; ++i) {
        const Vec3f original = pts[i];
        const Vec3f seg = original - prevOriginal;
        prevOriginal = original;
        const float len = seg.length();
        if (len <= 0.0f) {
            pts[i] = pts[i - 1];
            continue;
        }
        const float w = std::clamp(amount * profile(i, n), 0.0f, 1.0f);
        Vec3f turned = seg / len * (1.0f - w) + dir * w;
        if (turned.length() < 1e-6f) turned = seg / len;  // opposite directions: leave it
        pts[i] = pts[i - 1] + turned.normalized() * len;
    }
}

// The parts found in `strands`' flow with a Part node's params (findAutoParts, parts.h); null
// when none.
PartsPtr autoPartLines(const ParamSet& params, const StrandSet& strands, const ScalpMesh& scalp) {
    const PartSet found = findAutoParts(strands, scalp, static_cast<float>(params.getDouble("splitAngle")),
                                        static_cast<float>(params.getDouble("minLength")));
    return found.empty() ? nullptr : std::make_shared<const PartLines>(found, scalp);
}

// Collide's softness: a push of `delta` up to the offset plus the softness `s` (|delta| = u: how far
// below that the point is), eased: a point within the band of 2s below that rises u^2 / 4s (at its
// top it rises nothing, the rise growing smoothly), one deeper rises to the offset itself (u - s).
// Its new height is a smooth maximum of its height and the offset, never below either (look.slang
// softPush, the same arithmetic).
Vec3f softPush(const Vec3f& delta, float s) {
    if (!(s > 0.0f)) return delta;
    const float u = delta.length();
    if (!(u > 0.0f)) return delta;
    const float f = u < 2.0f * s ? u / (4.0f * s) : (u - s) / u;
    return delta * f;
}

// Collide's smoothing: each point's correction `c` (what the walk moved it by) spread toward the
// root over `length` of the strand (its segments' `lengths`, [0] unused): point i takes the mean
// of its own and each later point's within that length, each weighed by smoothstep of how far
// along that point is (1 - 3x^2 + 2x^3 of the share of `length`; its own 1). In place (a point
// reads only those after it). (look.slang smoothStrand, the same arithmetic.)
void smoothCorrections(Vec3f* c, const float* lengths, std::size_t n, float length) {
    for (std::size_t i = 1; i < n; ++i) {
        Vec3f sum = c[i];
        float weight = 1.0f;
        float apart = 0.0f;
        for (std::size_t j = i + 1; j < n; ++j) {
            apart += lengths[j];
            if (!(apart < length)) break;
            const float x = apart / length;
            const float w = 1.0f - x * x * (3.0f - 2.0f * x);
            sum += c[j] * w;
            weight += w;
        }
        c[i] = Vec3f(sum.x / weight, sum.y / weight, sum.z / weight);
    }
}

// Walks each strand from the root: keeps the segment length from the
// (already fixed) previous point, pushes the point out along the surface
// normal when it is less than `offset` above the surface, then re-keeps the
// length toward the pushed spot. Staying outside wins over exact length.
// Points deeper than maxDepth are left alone: on an open scalp (a head cap)
// they are on its far side, not inside it. The scalp and the optional
// `collider` (any mesh: a ColliderMesh node) are pushed against in turn at
// every point of the walk. With `method` field (the default) each mesh's question at a point (how
// far above the surface along the closest point's normal) is a lookup in its signed distance
// field (distance_field.h: made once per mesh and kept across evaluations), answered by the field
// alone (MeshDistanceField::Probe::fieldOnly): where its blend cannot tell (deep under the surface,
// across a crease, the search's edge) the closest point among the triangles around the closest
// faces it holds there, the mesh's own almost everywhere. Points land within a few percent of the
// offset of where the mesh's closest point puts them, and the GPU's Collide (look.slang, from the
// field's tables) walks the same. `exact` asks the mesh's closest-point tree at every point, the
// reference. A mesh deformed away from its rest pose (an animated scalp; a collider given one) is
// asked through its rest pose's field, made once for every frame: each point taken to rest space by
// its nearest vertex's frame (RestPoseCollision), the deformed mesh asked where that field cannot
// tell (the GPU leaves such a Collide to the CPU). A deforming collider without a rest pose gets a
// field per frame. `softness` eases the push in over a band above the offset (softPush) and
// `smoothing` spreads the pushes toward the root (smoothCorrections); with either the walk carries
// each segment along its own heading once a point moved (a strand meeting a surface slides along
// it), both 0 the walk above, as it always was.
class CollideOp : public Operator {
public:
    std::string typeName() const override { return op::kCollide; }
    std::string category() const override { return "shape"; }
    std::vector<PortSpec> inputs() const override {
        return {{"strands", PortType::Curves}, {"scalp", PortType::Mesh, true}, {"collider", PortType::Mesh, true}};
    }
    PortSpec output() const override { return {"out", PortType::Curves}; }  // strands or guides, as fed
    std::vector<ParamSpec> paramSpecs() const override {
        return {
            ParamSpec{"offset", 0.05, "Distance kept between strands and the surfaces", "length"}.soft(0, 0.5),
            ParamSpec{"maxDepth", 3.0, "Points deeper under a surface than this are not pushed", "length"}.soft(0, 10),
            ParamSpec{"softness", 0.0, "Height above the offset where the collider's push starts to ease in: strands curve away "
                                       "before they touch instead of kinking there (0: a hard push; the scalp's always is)", "length"}
                .atLeast(0)
                .soft(0, 1),
            ParamSpec{"smoothing", 0.0, "Length along the strand, toward the root, over which the collider's push fades out: "
                                        "the strand curves onto it and slides along it, its segments' lengths kept (0: none)",
                      "length"}
                .atLeast(0)
                .soft(0, 5),
            ParamSpec{"iterations", std::int64_t{2}, "Root-to-tip passes"}.range(1, 16).soft(1, 8).adv(),
            ParamSpec{"method", std::string("field"), "field: each surface's signed distance field alone, made once and kept "
                                                      "(fast, within a small fraction of the offset of exact; what the GPU "
                                                      "preview runs); exact: the closest point on the mesh at every point "
                                                      "(the CPU's alone)", "", {"field", "exact"}}
                .adv(),
        };
    }

    Value evaluate(const EvalContext& ctx, const std::vector<Value>& inputs) const override {
        Value out;
        run(ctx, inputs, nullptr, &out, nullptr);
        return out;
    }
    // Each strand is pushed against the meshes alone, never against other strands: with the same
    // meshes in (the scalp's and the collider's inputs unchanged: the same surfaces, as the Scalp
    // node's key is its surface), only the strands the input changed in are walked again.
    bool evaluateIncremental(const EvalContext& ctx, const std::vector<Value>& inputs, const IncrementalInputs& incremental,
                             Value* out, StrandChanges* changed) const override {
        return run(ctx, inputs, &incremental, out, changed);
    }

private:
    // In full, or (with `incremental`) only the strands the input changed in, into the last
    // result's copy; false when that cannot be done.
    bool run(const EvalContext&, const std::vector<Value>& inputs, const IncrementalInputs* incremental, Value* result,
             StrandChanges* changed) const {
        const StrandsPtr in = requireStrands(inputs, op::kCollide);
        // The surfaces to stay out of: the scalp, then the collider.
        std::vector<MeshPtr> meshes;
        std::vector<char> colliders;  // per mesh: the collider input's (softness and smoothing act on it)
        for (std::size_t slot = 1; slot < 3 && slot < inputs.size(); ++slot) {
            const MeshPtr mesh = valueAs<MeshPtr>(inputs[slot]);
            if (mesh && mesh->triangleCount() > 0) {
                meshes.push_back(mesh);
                colliders.push_back(slot == 2 ? 1 : 0);
            }
        }
        if (meshes.empty()) {
            if (incremental) return false;  // the input handed on: the graph passes its changes on
            *result = in;
            return true;
        }
        // Only widths or channels changed (a Width or HairColor edit before it): each strand is
        // walked by its points alone, never its widths or channels, so its last points stand.
        if (incremental && incremental->inputs.size() > 0 && incremental->inputs[0] && incremental->inputs[0]->pointsKept &&
            inputUnchanged(*incremental, 1) && inputUnchanged(*incremental, 2)) {
            if (const StrandsPtr last = previousStrandsFor(*incremental, *in)) {
                *result = withPointsOf(*in, *last);
                *changed = *incremental->inputs[0];
                return true;
            }
        }
        const auto offset = static_cast<float>(params().getDouble("offset"));
        const auto maxDepth = static_cast<float>(params().getDouble("maxDepth"));
        const auto iterations = static_cast<int>(params().getInt("iterations"));
        const auto softness = static_cast<float>(params().getDouble("softness"));
        const auto smoothing = static_cast<float>(params().getDouble("smoothing"));
        // A soft push asks how far below `reach` (the offset plus the softness) a point is.
        const float reach = offset + softness;
        // Only points this close to the surface can need a push; the bound keeps far points cheap.
        const float searchRadius = std::max(reach, maxDepth) * 1.001f + 1e-6f;
        // The meshes' fields (shared across evaluations, by surface): the cell size from the mesh
        // and the offset, reaching past the search radius. A mesh deformed away from its rest
        // pose (an animated scalp, a collider with a rest pose) is a new mesh on every frame,
        // whose own field would be made for one evaluation: it is asked through its rest pose's
        // field, kept across frames (RestPoseCollision; the tree alone for a maxDepth too shallow
        // for that field's cells, where it would only ask). A deforming collider without a rest
        // pose has nothing to keep: each frame's mesh gets a field of its own.
        std::vector<DistanceFieldPtr> fields(meshes.size());
        std::vector<std::shared_ptr<const RestPoseCollision>> rests(meshes.size());
        if (params().getString("method") != "exact") {
            for (std::size_t m = 0; m < meshes.size(); ++m) {
                fields[m] = collisionField(meshes[m], offset, searchRadius);
                if (!fields[m]) rests[m] = RestPoseCollision::of(meshes[m], offset, maxDepth, searchRadius);
            }
        }

        if (!incremental && gpu::cpuEngineOn() && meshes.size() <= 2) {
            // look.slang's CollideCS, the GPU preview's own (look_cpu.h), on its surfaces' fields.
            std::uint32_t colliderBits = 0;
            for (std::size_t m = 0; m < colliders.size(); ++m) colliderBits |= colliders[m] ? 1u << m : 0u;
            std::string whyNot;
            gpu::StageInputs surfaces;
            surfaces.collide = gpu::collideInputs(meshes, colliderBits, fields, offset, softness, &whyNot);
            if (surfaces.collide) {
                if (StrandsPtr made = gpu::runStage(gpu::StageKind::Collide, params(), in, nullptr, std::move(surfaces))) {
                    *result = made;
                    return true;
                }
            }
        }
        const StrandChanges* changes = nullptr;
        StrandsPtr last;  // the last result, to make again only the strands the input changed in
        if (incremental) {
            changes = someStrandsChanged(*incremental, 0);
            last = changes ? previousStrandsFor(*incremental, *in) : nullptr;
            if (!last || !inputUnchanged(*incremental, 1) || !inputUnchanged(*incremental, 2)) return false;
            if (changes->none()) {  // the same strands in: the same result
                *result = last;
                *changed = *changes;
                return true;
            }
        }
        auto out = std::make_shared<StrandSet>(*in);
        if (last) {
            positionsOfLast(*out, last, *incremental);  // the strands left alone, as they were made
        } else {
            out->overwritePositions();  // made its own here, each strand copied in by the thread walking it
        }
        // Per field: the margin its blend may answer below a point's height by (two of its cells'
        // diagonals), for the per-strand test below.
        std::vector<float> fieldMargins(meshes.size(), 0.0f);
        for (std::size_t m = 0; m < meshes.size(); ++m) {
            if (fields[m]) fieldMargins[m] = 2.0f * 1.7320508f * fields[m]->cellSize();
        }
        // Each still mesh's field asked once for a whole strand first (MeshDistanceField::bounds at
        // its root, out to its farthest point and the push's reach): a mesh those bounds settle every
        // push for (the strand well above it, or beyond the search: its field would leave each of
        // those points alone) is not asked at the strand's points while they stay that near the root
        // (one a push of the other mesh moved farther is asked). look.slang CollideCS does the same.
        auto collide = [&](std::size_t s) {
            const std::size_t n = out->strandPointCount(s);
            if (n < 2) return;
            Vec3f* pts = out->strandPoints(s);
            std::array<float, 2> skipWithin{-1.0f, -1.0f};  // per mesh: skipped while points are this near the root
            if (collideStrandBounds.load(std::memory_order_relaxed)) {
                float farthest = 0.0f;
                for (std::size_t i = 1; i < n; ++i) farthest = std::max(farthest, (pts[i] - pts[0]).length());
                const float within = farthest * 1.001f + reach + 1e-6f;
                for (std::size_t m = 0; m < meshes.size(); ++m) {
                    float heightLow = 0.0f, distanceLow = 0.0f;
                    if (fields[m] && fields[m]->bounds(pts[0], within, &heightLow, &distanceLow) &&
                        (heightLow >= reach + fieldMargins[m] || distanceLow > searchRadius + fieldMargins[m])) {
                        skipWithin[m] = within;
                        collideSkipped.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            }
            thread_local std::vector<float> lengths;  // scratch, reused across strands
            lengths.assign(n, 0.0f);
            for (std::size_t i = 1; i < n; ++i) lengths[i] = (pts[i] - pts[i - 1]).length();

            auto keepLength = [&](std::size_t i) {
                const Vec3f d = pts[i] - pts[i - 1];
                const float len = d.length();
                if (len > 0.0f) pts[i] = pts[i - 1] + d * (lengths[i] / len);
            };
            // Per mesh: a point queried on this strand and how near the mesh can be to it. The
            // distance to a mesh changes no faster than the point moves, so a point farther from
            // that one's bound than the search radius (with room for rounding) cannot be found
            // near the mesh: its query is skipped, the answer (a miss) the same.
            std::array<Vec3f, 2> anchor{};
            std::array<float, 2> bound{-1.0f, -1.0f};
            // Per deformed mesh asked through its rest pose: where the strand's walk is.
            std::array<RestPoseCollision::Cursor, 2> cursors{};
            auto beyondBound = [&](std::size_t m, const Vec3f& p) {
                if (!(bound[m] > searchRadius)) return false;
                const float scale = std::max({std::fabs(p.x), std::fabs(p.y), std::fabs(p.z)});
                const float room = 1e-4f * (searchRadius + bound[m] + scale) + 1e-6f;
                return bound[m] - (p - anchor[m]).length() > searchRadius + room;
            };
            // `soft`: the push eased by the softness (softPush), asked to `reach`; else to the offset.
            auto pushOut = [&](std::size_t m, std::size_t i, bool soft) {
                if (skipWithin[m] >= 0.0f && (pts[i] - pts[0]).length() <= skipWithin[m]) return false;  // settled for the strand
                const float to = soft ? reach : offset;
                // Through a rest pose, the mesh's bound first: it costs less than the walk to the
                // point's vertex, and the answer (a miss) is the mesh's own.
                if (rests[m] && beyondBound(m, pts[i])) return false;
                if (fields[m] || rests[m]) {
                    Vec3f delta;
                    // A still mesh's field answers alone (never Ask, but for a field without a grid:
                    // the mesh's), as the GPU's Collide does from its tables. Through a rest pose the
                    // deformed mesh answers where the rest field cannot tell (its own answer, which
                    // the rest field's alone would miss by how the mesh bends; the GPU leaves that
                    // Collide to the CPU).
                    MeshDistanceField::Probe alone;
                    alone.fieldOnly = true;
                    const MeshDistanceField::Push push =
                        fields[m] ? fields[m]->push(pts[i], to, maxDepth, searchRadius, &delta, &alone)
                                  : rests[m]->push(pts[i], to, maxDepth, searchRadius, &delta, cursors[m]);
                    switch (push) {
                        case MeshDistanceField::Push::Kept: return false;
                        case MeshDistanceField::Push::Moved:
                            if (soft) delta = softPush(delta, softness);
                            pts[i] += delta;
                            return true;
                        case MeshDistanceField::Push::Ask: break;  // the mesh's answer, below
                    }
                }
                const ScalpMesh& mesh = *meshes[m];
                if (beyondBound(m, pts[i])) return false;
                float lowerBound = 0.0f;
                const SurfacePoint sp = mesh.closestPointBounded(pts[i], searchRadius, &lowerBound);
                anchor[m] = pts[i];
                bound[m] = lowerBound;
                if (sp.root.face < 0) return false;
                const float height = (pts[i] - sp.position).dot(sp.normal);
                if (height >= to || height <= -maxDepth) return false;
                Vec3f delta = sp.normal * (to - height);
                if (soft) delta = softPush(delta, softness);
                pts[i] += delta;
                return true;
            };
            const StrandRoot root = s < out->roots().size() ? out->roots()[s] : StrandRoot{-1, 0.0f, 0.0f};
            if (!(softness > 0.0f) && !(smoothing > 0.0f)) {
                // Root-to-tip passes: each point follows the one before (its length kept), then is
                // pushed out of each mesh in turn (if it moved, its length kept and pushed again).
                for (int pass = 0; pass < iterations; ++pass) {
                    bool moved = false;
                    for (std::size_t m = 0; m < meshes.size(); ++m) {  // each pass walks from the root
                        if (rests[m]) cursors[m] = rests[m]->cursor(root, pts[0]);
                    }
                    for (std::size_t i = 1; i < n; ++i) {
                        keepLength(i);  // follow the leader
                        for (std::size_t m = 0; m < meshes.size(); ++m) {
                            if (pushOut(m, i, false)) {
                                moved = true;
                                keepLength(i);
                                pushOut(m, i, false);
                            }
                        }
                    }
                    if (!moved) break;
                }
                return;
            }
            // Softness and smoothing act on the collider (a floor, a shoulder): the scalp, which the
            // strands grow from (each root within any band above it), keeps its hard push. A strand
            // the collider never moves walks as above. Once the collider moved a point, the walk
            // carries each later segment as the strand it walks has it (its heading kept) from the
            // point before, so a strand meeting the collider slides along it rather than folding
            // back on itself; its first push of a point in the first pass is eased by the softness
            // (softPush). Passes after the first walk the last one's strand, to the offset alone
            // (an ease asked again of the point it made would lift it again). `from`: the strand
            // walked first; true when the collider moved a point.
            thread_local std::vector<Vec3f> came, corrections;
            came.assign(pts, pts + n);
            auto carry = [&](const Vec3f* from, bool ease) {
                bool touched = false;
                for (int pass = 0; pass < iterations; ++pass) {
                    const Vec3f* walked = pass == 0 ? from : pts;
                    bool moved = false, shifted = false;
                    for (std::size_t m = 0; m < meshes.size(); ++m) {
                        if (rests[m]) cursors[m] = rests[m]->cursor(root, pts[0]);
                    }
                    Vec3f last = walked[0];  // the walked strand's point before (as it was)
                    for (std::size_t i = 1; i < n; ++i) {
                        const Vec3f here = walked[i];
                        if (shifted) {
                            pts[i] = pts[i - 1] + (here - last);
                        } else {
                            pts[i] = here;
                            keepLength(i);  // follow the leader
                        }
                        last = here;
                        for (std::size_t m = 0; m < meshes.size(); ++m) {
                            if (pushOut(m, i, ease && pass == 0 && colliders[m])) {
                                moved = true;
                                if (colliders[m]) shifted = touched = true;
                                keepLength(i);
                                pushOut(m, i, false);
                            }
                        }
                    }
                    if (!moved) break;
                }
                return touched;
            };
            if (!carry(came.data(), softness > 0.0f) || !(smoothing > 0.0f)) return;
            // Smoothing: each point's correction (from the strand as it came) spread toward the
            // root (smoothCorrections), each segment turned to the smoothed strand's (came plus
            // the spread corrections), its length kept, from the root; then walked again (to the
            // offset: no point is left inside).
            corrections.resize(n);
            for (std::size_t i = 0; i < n; ++i) corrections[i] = pts[i] - came[i];
            smoothCorrections(corrections.data(), lengths.data(), n, smoothing);
            Vec3f lastSpot = came[0];
            for (std::size_t i = 1; i < n; ++i) {
                const Vec3f spot = came[i] + corrections[i];
                const Vec3f d = spot - lastSpot;
                const float len = d.length();
                pts[i] = pts[i - 1] + (len > 0.0f ? d * (lengths[i] / len) : came[i] - came[i - 1]);
                lastSpot = spot;
            }
            thread_local std::vector<Vec3f> bent;
            bent.assign(pts, pts + n);
            carry(bent.data(), false);
        };
        if (last) {
            forEachListed(changes->strands, [&](std::size_t s) {
                copyStrandPoints(*out, *in, s);
                collide(s);
            });
            *changed = *changes;
        } else {
            parallelFor(0, out->strandCount(), [&](std::size_t s) {
                copyStrandPoints(*out, *in, s);
                collide(s);
            });
        }
        *result = StrandsPtr(out);
        return true;
    }
};

// Opens the parts: strands near a part lean away from it, on their own side,
// fading out over `width` (rest-pose distance from the line), keeping lengths.
// The parts are the groom's painted ones (the "parts" external), or with partsFrom auto the ones
// found where the strands it takes split on the scalp (findAutoParts, parts.h), opened the same
// way. Auto parts are found anew from the whole input on every evaluation (no incremental run): a
// brushed strand can move them. Interpolate and Clump upstream never see auto parts (they are
// found after them): their respectParts keeps to painted parts only. The lines found are kept with
// the result (operator_state.h autoPartState): the GPU preview opens those while a drag or a
// stroke upstream moves the strands, until the release evaluates here and finds them again.
class PartOp : public Operator {
public:
    std::string typeName() const override { return op::kPart; }
    std::string category() const override { return "shape"; }
    std::vector<PortSpec> inputs() const override { return {{"strands", PortType::Strands}}; }
    std::vector<ParamSpec> paramSpecs() const override {
        return {
            ParamSpec{"partsFrom", std::string("painted"),
                      "Where the parts come from: painted: the lines drawn with the Part tool; auto: found where the "
                      "hair it takes splits on the scalp, flowing away to both sides of a line (a parting combed into "
                      "the guides)",
                      "", {"painted", "auto"}},
            ParamSpec{"width", 1.5, "Distance from the part over which strands lean away (scene units)", "length"}
                .atLeast(1e-6)
                .soft(0.1, 5),
            ParamSpec{"strength", 1.0, "How far strands at the part turn away from it, 0-1 (mappable)", "fraction"}.range(0, 1),
            {"profile", Ramp::linear(0.0f, 1.0f), "Turn along the length, root to tip"},
            ParamSpec{"part", std::string(), "Only this part (empty: every part)"}.when("partsFrom", {"painted"}),
            ParamSpec{"splitAngle", 90.0, "How sharply the hair must split to make a part: the angle between its flow "
                                          "on the two sides of the line, degrees (lower finds softer partings)", "angle"}
                .range(1, 180)
                .when("partsFrom", {"auto"}),
            ParamSpec{"minLength", 0.15, "The shortest part kept, as a share of the scalp's size (the square root of its "
                                         "area): shorter splits, like a whorl's center, are left alone", "fraction"}
                .range(0, 1)
                .when("partsFrom", {"auto"}),
        };
    }
    std::vector<std::string> externalInputs() const override {
        if (params().getString("partsFrom") == "auto") return {"scalp"};
        return {"scalp", "parts"};
    }

    Value evaluate(const EvalContext& ctx, const std::vector<Value>& inputs) const override {
        Value out;
        run(ctx, inputs, nullptr, &out, nullptr);
        return out;
    }
    bool evaluateIncremental(const EvalContext& ctx, const std::vector<Value>& inputs, const IncrementalInputs& incremental,
                             Value* out, StrandChanges* changed) const override {
        return run(ctx, inputs, &incremental, out, changed);
    }

    // operator_state.h autoPartState.
    bool autoLines(const StrandsPtr& output, PartsPtr* lines) const {
        const std::lock_guard<std::mutex> lock(keptMutex_);
        if (!output || !kept_.found || !sameOrShown(kept_.output.lock(), output)) return false;
        *lines = kept_.lines;
        return true;
    }
    // Masked: its lines answer for the mixed result the graph keeps (Operator::resultMixed).
    void resultMixed(const StrandsPtr& made, const StrandsPtr& mixed) const override {
        const std::lock_guard<std::mutex> lock(keptMutex_);
        if (made && kept_.output.lock() == made) kept_.output = mixed;
    }

private:
    // The last result (by identity) and, made with partsFrom auto (`found`), the lines it found
    // (null: none), for a host opening them elsewhere (the GPU preview).
    struct Kept {
        std::weak_ptr<const StrandSet> output;
        PartsPtr lines;
        bool found = false;
    };
    void remember(const Value& out, bool found, const PartsPtr& lines) const {
        const std::lock_guard<std::mutex> lock(keptMutex_);
        kept_.output = valueAs<StrandsPtr>(out);
        kept_.lines = found ? lines : nullptr;
        kept_.found = found;
    }
    mutable std::mutex keptMutex_;
    mutable Kept kept_;

    // In full, or (with `incremental`) only the strands the input changed in, into the last
    // result's copy; false when that cannot be done. (The parts and the scalp are externals: the
    // same ones as the last result's, or the node's key would differ.)
    bool run(const EvalContext& ctx, const std::vector<Value>& inputs, const IncrementalInputs* incremental, Value* result,
             StrandChanges* changed) const {
        const StrandsPtr in = requireStrands(inputs, op::kPart);
        const ScalpMesh* scalp = scalpOf(ctx);
        const bool flow = params().getString("partsFrom") == "auto";
        if (flow && incremental) return false;  // found from every strand: made anew
        const PartsPtr parts = flow ? (scalp ? autoPartLines(params(), *in, *scalp) : nullptr)
                                    : valueAs<PartsPtr>(ctx.external("parts"));
        if (!parts || parts->empty() || !scalp) {
            if (incremental) return false;  // the input handed on: the graph passes its changes on
            if (flow && scalp) {
                ctx.note("no parts found: the hair it takes splits nowhere by splitAngle over minLength; the strands pass through");
            } else if (!flow && (!parts || parts->empty())) {
                ctx.note("no parts drawn: draw some with the Part tool; the strands pass through");
            }
            *result = in;
            remember(*result, flow, nullptr);
            return true;
        }
        const float width = static_cast<float>(params().getDouble("width"));
        const ScalarField strength(params(), "strength", scalp);
        strength.validate(*in);
        const Ramp profile = params().getRamp("profile");
        const std::string only = flow ? std::string() : params().getString("part");

        const StrandChanges* changes = nullptr;
        StrandsPtr last;  // the last result, to make again only the strands the input changed in
        if (incremental) {
            changes = someStrandsChanged(*incremental, 0);
            last = changes ? previousStrandsFor(*incremental, *in) : nullptr;
            if (!last || !strength.strandLocal()) return false;
            if (changes->none()) {  // the same strands in: the same result
                *result = last;
                *changed = *changes;
                remember(*result, false, nullptr);  // (incremental runs are never auto)
                return true;
            }
        }
        auto out = std::make_shared<StrandSet>(*in);
        if (last) {
            positionsOfLast(*out, last, *incremental);  // the strands left alone, as they were made
        } else {
            out->mutablePositions();  // cloned once here: the loop writes points while fields read the set
        }
        const RampAlong along(profile, *out);
        auto part =[&](std::size_t s) {
            const std::size_t n = out->strandPointCount(s);
            const StrandRoot& root = out->roots()[s];
            if (n < 2 || !(scalp && scalp->contains(root))) return;
            Vec3f* pts = out->strandPoints(s);
            const PartLines::Side side = parts->sideOf(scalp->restPositionOr(root, pts[0]), only);
            if (side.side == 0 || side.distance >= width) return;
            const float falloff = (1.0f - side.distance / width) * (1.0f - side.distance / width);
            const float amount = std::clamp(strength.at(*out, s), 0.0f, 1.0f) * falloff;
            const Vec3f away = parts->awayFrom(side, scalp->normal(root));
            if (amount <= 0.0f || away.length() == 0.0f) return;
            turnToward(pts, n, away, amount, along);
        };
        if (last) {
            forEachListed(changes->strands, [&](std::size_t s) {
                copyStrandPoints(*out, *in, s);
                part(s);
            });
            *changed = *changes;
        } else {
            parallelFor(0, out->strandCount(), part);
        }
        *result = StrandsPtr(out);
        remember(*result, flow, parts);
        return true;
    }
};


// How fast strands bend toward gravity where they bend most (strength 1, no stiffness, all the
// strand still to hang): over a whole strand at that rate, its direction comes within about 2%
// of gravity's (exp(-8 x the mean load, 0.5)).
constexpr float kGravityBend = 8.0f;

// Strands bend from the root under their weight, toward `direction` (world -Y, or painted per
// root in a color set). Walking from the root, each segment keeps its own shape relative to the
// one before and turns further toward gravity by a share that grows with the hair hanging beyond
// it (the load: the share of the length past the segment) and with how soft the strand is there
// (1 - stiffness x stiffnessProfile, by arc length); the turns made above carry down the strand,
// so a curl or a wave hangs as a whole. The rate is per unit of the strand's length, so the shape
// does not depend on the point count. Fully stiff at the root (the profile's 1 there), strands
// leave the scalp in their own direction. Strands bent into the scalp: a Collide after it.
// Implemented from the behaviour artists know from Yeti's and Ornatrix's gravity nodes;
// nothing of theirs is used.
class GravityOp : public Operator {
public:
    std::string typeName() const override { return op::kGravity; }
    std::string category() const override { return "shape"; }
    std::vector<PortSpec> inputs() const override { return {{"strands", PortType::Curves}}; }
    PortSpec output() const override { return {"out", PortType::Curves}; }  // strands or guides, as fed
    std::vector<ParamSpec> paramSpecs() const override {
        return {
            ParamSpec{"strength", 1.0, "How far strands bend under their weight (mappable): 0 none, 1 hanging, more heavier"}
                .soft(0, 2),
            ParamSpec{"direction", Vec3f{0.0f, -1.0f, 0.0f}, "Gravity's direction (world space; or an expression "
                                                             "giving a vector per strand)"}
                .world()
                .vectorExpr(),
            {"directionColorSet", std::string(), "Scalp color set holding a painted direction per root (RGB 0-1 = XYZ "
                                                 "-1..1; empty: none)"},
            ParamSpec{"stiffness", 1.0, "Bend stiffness: how much strands resist bending, 0-1, times stiffnessProfile (1 "
                                        "there: not at all)", "fraction"}
                .range(0, 1),
            {"stiffnessProfile", Ramp{{0.0f, 1.0f}, {0.25f, 0.5f}, {1.0f, 0.5f}},
             "Stiffness along the length, root to tip (1 at the root: strands leave the scalp in their own direction)"},
        };
    }
    std::vector<std::string> externalInputs() const override { return {"scalp"}; }
    // The painted direction's color set (the scalp's key leaves its color sets out).
    std::uint64_t contextSalt(const EvalContext& ctx) const override {
        return colorSetSalt(ctx, params().getString("directionColorSet"));
    }

    Value evaluate(const EvalContext& ctx, const std::vector<Value>& inputs) const override {
        Value out;
        run(ctx, inputs, nullptr, &out, nullptr);
        return out;
    }
    // Each strand bends alone (against the scalp alone): only the strands the input changed in.
    bool evaluateIncremental(const EvalContext& ctx, const std::vector<Value>& inputs, const IncrementalInputs& incremental,
                             Value* out, StrandChanges* changed) const override {
        return run(ctx, inputs, &incremental, out, changed);
    }

private:
    // In full, or (with `incremental`) only the strands the input changed in, into the last
    // result's copy; false when that cannot be done.
    bool run(const EvalContext& ctx, const std::vector<Value>& inputs, const IncrementalInputs* incremental, Value* result,
             StrandChanges* changed) const {
        const StrandsPtr in = requireStrands(inputs, op::kGravity);
        const ScalpMesh* scalp = scalpOf(ctx);
        const ScalarField strength(params(), "strength", scalp);
        strength.validate(*in);
        const VectorField worldDirection(params(), "direction", scalp);
        worldDirection.validate(*in);
        const std::string colorSet = params().getString("directionColorSet");
        const bool painted = !colorSet.empty() && scalp && scalp->hasColorSet(colorSet);
        const float stiffness = static_cast<float>(params().getDouble("stiffness"));
        const Ramp stiffnessProfile = params().getRamp("stiffnessProfile");

        const StrandChanges* changes = nullptr;
        StrandsPtr last;  // the last result, to make again only the strands the input changed in
        if (incremental) {
            changes = someStrandsChanged(*incremental, 0);
            last = changes ? previousStrandsFor(*incremental, *in) : nullptr;
            if (!last || !strength.strandLocal() || !worldDirection.strandLocal()) return false;
            if (changes->none()) {  // the same strands in: the same result
                *result = last;
                *changed = *changes;
                return true;
            }
        }
        if (!last && gpu::cpuEngineOn()) {  // look.slang's, the GPU preview's own (look_cpu.h)
            if (StrandsPtr made = gpu::runStage(gpu::StageKind::Gravity, params(), in, scalp)) {
                *result = made;
                return true;
            }
        }
        auto out = std::make_shared<StrandSet>(*in);
        if (last) {
            positionsOfLast(*out, last, *incremental);  // the strands left alone, as they were made
        } else {
            out->mutablePositions();  // cloned once here: the loop writes points while fields read the set
        }
        auto hang = [&](std::size_t s) {
            const std::size_t n = out->strandPointCount(s);
            if (n < 2) return;
            const float amount = strength.at(*out, s);
            if (amount <= 0.0f) return;
            const StrandRoot& root = out->roots()[s];
            Vec3f down = worldDirection.at(*in, s);
            if (painted && scalp->contains(root)) {
                const ScalpMesh::Color c = scalp->colorAt(colorSet, root);
                const Vec3f p{c[0] * 2.0f - 1.0f, c[1] * 2.0f - 1.0f, c[2] * 2.0f - 1.0f};
                if (p.length() > 1e-3f) down = p;
            }
            if (down.length() < 1e-6f) return;
            down = down.normalized();
            Vec3f* pts = out->strandPoints(s);
            // The strand's own segments (read before they move) and its length.
            thread_local std::vector<Vec3f> segments;
            segments.resize(n);
            float total = 0.0f;
            for (std::size_t i = 1; i < n; ++i) {
                segments[i] = pts[i] - pts[i - 1];
                total += segments[i].length();
            }
            if (total <= 0.0f) return;
            const float strandLength = total;
            Turn carried;  // the turns of the segments above
            float arc = 0.0f;
            for (std::size_t i = 1; i < n; ++i) {
                const float len = segments[i].length();
                if (len <= 0.0f) {
                    pts[i] = pts[i - 1];
                    continue;
                }
                const Vec3f dir = carried(segments[i] / len);
                const float load = std::max(0.0f, strandLength - (arc + 0.5f * len)) / strandLength;
                const float stiff = std::clamp(stiffness * stiffnessProfile(arc / strandLength), 0.0f, 1.0f);
                arc += len;
                const float share = 1.0f - pm::exp(-amount * kGravityBend * (1.0f - stiff) * load * len / strandLength);
                // The turn toward gravity (straight against it: about any way across).
                const float c = std::clamp(dir.dot(down), -1.0f, 1.0f);
                Vec3f axis = dir.cross(down);
                const float sine = axis.length();
                axis = sine > 1e-6f ? axis / sine : anyPerpendicular(dir);
                const float angle = pm::atan2(sine, c) * share;
                const Turn turn = angle > 0.0f ? Turn::about(axis, angle) : Turn{};
                pts[i] = pts[i - 1] + turn(dir) * len;
                carried = carried.then(turn);
            }
        };
        if (last) {
            forEachListed(changes->strands, [&](std::size_t s) {
                copyStrandPoints(*out, *in, s);
                hang(s);
            });
            *changed = *changes;
        } else {
            parallelFor(0, out->strandCount(), hang);
        }
        *result = StrandsPtr(out);
        return true;
    }
};

// Offsets strands along the scalp's normal at their root (or a world direction) by `distance`
// (mappable, per point) times `profile` along the length; the root stays. Lifts hair off the
// scalp for volume, or pushes a region in.
class DisplaceOp : public Operator {
public:
    std::string typeName() const override { return op::kDisplace; }
    std::string category() const override { return "shape"; }
    std::vector<PortSpec> inputs() const override { return {{"strands", PortType::Curves}}; }
    PortSpec output() const override { return {"out", PortType::Curves}; }  // strands or guides, as fed
    std::vector<ParamSpec> paramSpecs() const override {
        return {
            ParamSpec{"distance", 0.1, "How far each point moves, scene units (mappable, per point; negative: the other way)",
                      "length"}
                .soft(-1, 1),
            {"profile", Ramp{{0.0f, 0.0f}, {0.2f, 1.0f}, {1.0f, 1.0f}}, "Distance along the length, root to tip"},
            {"along", std::string("normal"), "normal: along the scalp's normal at the root; direction: along `direction`", "",
             {"normal", "direction"}},
            ParamSpec{"direction", Vec3f{0.0f, 1.0f, 0.0f}, "The way strands move (world space; unrooted strands too; or an "
                                                            "expression giving a vector per strand)"}
                .world()
                .vectorExpr(),
        };
    }
    std::vector<std::string> externalInputs() const override { return {"scalp"}; }

    Value evaluate(const EvalContext& ctx, const std::vector<Value>& inputs) const override {
        Value out;
        run(ctx, inputs, nullptr, &out, nullptr);
        return out;
    }
    bool evaluateIncremental(const EvalContext& ctx, const std::vector<Value>& inputs, const IncrementalInputs& incremental,
                             Value* out, StrandChanges* changed) const override {
        return run(ctx, inputs, &incremental, out, changed);
    }

private:
    // In full, or (with `incremental`) only the strands the input changed in, into the last
    // result's copy; false when that cannot be done.
    bool run(const EvalContext& ctx, const std::vector<Value>& inputs, const IncrementalInputs* incremental, Value* result,
             StrandChanges* changed) const {
        const StrandsPtr in = requireStrands(inputs, op::kDisplace);
        const ScalpMesh* scalp = scalpOf(ctx);
        const ScalarField amount(params(), "distance", scalp);
        amount.validate(*in);
        const bool perPoint = amount.perPoint();
        const Ramp profile = params().getRamp("profile");
        const bool alongNormal = params().getString("along") != "direction";
        const VectorField directionField(params(), "direction", scalp);
        directionField.validate(*in);
        auto unit = [](Vec3f d) { return d.length() > 1e-6f ? d.normalized() : Vec3f{}; };
        const Vec3f direction = unit(directionField.constant());

        const StrandChanges* changes = nullptr;
        StrandsPtr last;  // the last result, to make again only the strands the input changed in
        if (incremental) {
            changes = someStrandsChanged(*incremental, 0);
            last = changes ? previousStrandsFor(*incremental, *in) : nullptr;
            if (!last || !amount.strandLocal() || !directionField.strandLocal()) return false;
            if (changes->none()) {  // the same strands in: the same result
                *result = last;
                *changed = *changes;
                return true;
            }
        }
        if (!last && gpu::cpuEngineOn()) {  // look.slang's, the GPU preview's own (look_cpu.h)
            if (StrandsPtr made = gpu::runStage(gpu::StageKind::Displace, params(), in, scalp)) {
                *result = made;
                return true;
            }
        }
        auto out = std::make_shared<StrandSet>(*in);
        if (last) {
            positionsOfLast(*out, last, *incremental);  // the strands left alone, as they were made
        } else {
            out->mutablePositions();  // cloned once here: the loop writes points while fields read the set
        }
        const RampAlong along(profile, *out);
        auto displace = [&](std::size_t s) {
            const std::size_t n = out->strandPointCount(s);
            const StrandRoot& root = out->roots()[s];
            const Vec3f way = alongNormal && scalp && scalp->contains(root)
                                  ? scalp->normal(root)
                                  : (directionField.isConstant() ? direction : unit(directionField.at(*in, s)));
            const float strandAmount = perPoint ? 0.0f : amount.at(*out, s);
            Vec3f* pts = out->strandPoints(s);
            for (std::size_t i = 1; i < n; ++i) {
                // Per point: read before the point moves (the field sees the incoming shape).
                pts[i] += way * ((perPoint ? amount.atPoint(*out, s, i) : strandAmount) * along(i, n));
            }
        };
        if (last) {
            forEachListed(changes->strands, [&](std::size_t s) {
                copyStrandPoints(*out, *in, s);
                displace(s);
            });
            *changed = *changes;
        } else {
            parallelFor(0, out->strandCount(), displace);
        }
        *result = StrandsPtr(out);
        return true;
    }
};

}  // namespace

PartsPtr partLinesFor(const ParamSet& part, const StrandSet& strands, const ScalpMesh& scalp, const PartsPtr& painted) {
    if (part.getString("partsFrom") == "auto") return autoPartLines(part, strands, scalp);
    return painted && !painted->empty() ? painted : nullptr;
}

bool autoPartState(const Operator& op, const StrandsPtr& output, PartsPtr* lines) {
    const auto* part = dynamic_cast<const PartOp*>(&op);
    return part && part->autoLines(output, lines);
}

void registerPartOperator(OperatorRegistry& registry) {
    registry.registerType(op::kPart,
                          {"shape", "Opens a gap along the parts drawn on the scalp.", "parting gap line"},
                          [] { return std::make_unique<PartOp>(); });
}

void registerCollideOperator(OperatorRegistry& registry) {
    registry.registerType(op::kCollide,
                          {"collide",
                           "Pushes strands out of the scalp and any collider. Best last.",
                           "collision penetration body scalp"},
                          [] { return std::make_unique<CollideOp>(); });
}

void registerGravityOperator(OperatorRegistry& registry) {
    registry.registerType(op::kGravity,
                          {"deform",
                           "Bends strands from the root under their weight; stiff at the root (a Collide after it keeps them off the scalp).",
                           "weight droop sag fall"},
                          [] { return std::make_unique<GravityOp>(); });
}

void registerDisplaceOperator(OperatorRegistry& registry) {
    registry.registerType(op::kDisplace,
                          {"deform",
                           "Moves strands along the scalp's normal (or a direction): volume off the scalp.",
                           "volume push offset normal"},
                          [] { return std::make_unique<DisplaceOp>(); });
}

void setCollideStrandBounds(bool on) { collideStrandBounds = on; }
std::size_t collideSkippedSurfaces() { return collideSkipped.load(); }

}  // namespace groomlab
