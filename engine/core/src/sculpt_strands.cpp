// SPDX-License-Identifier: Apache-2.0
// SculptStrands nodes' edits: record and apply, with the fill (sculpt_strands.h).

#include "groomlab/sculpt_strands.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <stdexcept>

#include "groomlab/parallel.h"
#include "groomlab/point_index.h"
#include "groomlab/preview_core.h"
#include "groomlab/scalp_mesh.h"

namespace groomlab {

namespace {

constexpr float kMinStrength = 1e-3f;
constexpr std::size_t kFillNeighbours = 8;  // the recorded strands looked at around a strand to fill

std::vector<Vec3f> pointsOf(const StrandSet& set, std::size_t s) {
    const Vec3f* pts = set.strandPoints(s);
    return {pts, pts + set.strandPointCount(s)};
}

// A strand's root frame: the scalp's normal and its across direction there (ScalpMesh::acrossAt:
// smooth over the scalp, so neighbouring strands' edits mean the same turns); world +Y and +X off it.
void rootFrame(const StrandSet& set, std::size_t s, const ScalpMesh* scalp, Vec3f* normal, Vec3f* across) {
    const StrandRoot& root = set.roots()[s];
    if (scalp && scalp->contains(root)) {
        *normal = scalp->normal(root);
        *across = scalp->acrossAt(root);
    } else {
        *normal = {0.0f, 1.0f, 0.0f};
        *across = {1.0f, 0.0f, 0.0f};
    }
}

// A root's place on the rest pose (the fill's distances: the same however the scalp deforms); false
// off the scalp.
bool restRoot(const ScalpMesh* scalp, const StrandRoot& root, Vec3f* at) {
    if (!scalp || !scalp->contains(root)) return false;
    *at = scalp->restPositionOr(root, scalp->position(root));
    return true;
}

// The recorded strands by their roots' places, for the fill.
class FillIndex {
public:
    FillIndex(const SculptEdits& edits, const ScalpMesh* scalp) {
        std::vector<Vec3f> places;
        for (const auto& [id, s] : edits.strands) {
            Vec3f at;
            if (!(s.spacing > 0.0f) || !restRoot(scalp, s.root, &at)) continue;
            places.push_back(at);
            entries_.push_back(&s);
            maxReach_ = std::max(maxReach_, s.spacing * kFillReach);
        }
        if (!places.empty()) index_ = PointIndex(std::move(places));
    }

    bool empty() const { return index_.empty(); }

    // The blend of the recorded edits reaching `place` (sculpt_strands.h); empty for none.
    GuideSculpt blendAt(const Vec3f& place) const {
        if (index_.empty()) return {};
        float d2 = 0.0f;
        if (index_.nearestWithin(place, maxReach_ * maxReach_, &d2) == SIZE_MAX) return {};  // (most strands: far from every stroke)
        std::size_t found[kFillNeighbours];
        float dist2[kFillNeighbours];
        const std::size_t n = index_.kNearest(place, kFillNeighbours, found, dist2);
        std::pair<const GuideSculpt*, float> parts[kFillNeighbours];
        std::size_t count = 0, samples = 0;
        float total = 0.0f, cover = 0.0f;
        for (std::size_t k = 0; k < n; ++k) {
            const StrandSculpt& s = *entries_[found[k]];
            const float reach = s.spacing * kFillReach;
            const float d = std::sqrt(dist2[k]);
            if (!(d < reach)) continue;
            const float x = d / reach;
            const float w = (1.0f - x * x) * (1.0f - x * x);  // smooth: 1 at the recorded root, 0 at its reach
            parts[count++] = {&s.edit, w};
            total += w;
            samples = std::max(samples, s.edit.samples.size());
            // How covered this place is: fully within a spacing of a recorded root, fading out to its reach.
            const float c = d <= s.spacing ? 1.0f : 1.0f - (d - s.spacing) / (reach - s.spacing);
            cover = std::max(cover, c * c * (3.0f - 2.0f * c));
        }
        if (count == 0 || samples == 0 || !(total > 0.0f)) return {};  // (none reaches, or only strands left as they were)
        // The edits' shares (a strand left as it was: a share of none), all of it where covered.
        const float norm = cover / total;
        GuideSculpt out;
        out.samples.resize(samples);
        for (std::size_t j = 0; j < samples; ++j) {
            SculptSample& o = out.samples[j];
            o.t = (static_cast<float>(j) + 0.5f) / static_cast<float>(samples);
            for (std::size_t k = 0; k < count; ++k) {
                if (parts[k].first->samples.empty()) continue;  // (left as it was: none)
                const SculptSample x = parts[k].first->at(o.t);
                const float w = parts[k].second * norm;
                o.ax += w * x.ax;
                o.ay += w * x.ay;
                o.scale += w * (x.scale - 1.0f);
            }
        }
        if (out.identity()) return {};
        return out;
    }

private:
    PointIndex index_;
    std::vector<const StrandSculpt*> entries_;
    float maxReach_ = 0.0f;
};

const GuideSculpt* ownEdit(const SculptEdits& edits, StrandId id) {
    const auto it = edits.strands.find(id);
    return it == edits.strands.end() ? nullptr : &it->second.edit;
}

GuideSculpt editOf(const StrandSet& input, std::size_t s, const SculptEdits& edits, const ScalpMesh* scalp, const FillIndex* fill) {
    if (const GuideSculpt* own = ownEdit(edits, input.ids()[s])) return *own;  // (an empty one: left as it was, no fill)
    Vec3f at;
    if (!fill || fill->empty() || !restRoot(scalp, input.roots()[s], &at)) return {};
    return fill->blendAt(at);
}

}  // namespace

GuideSculpt strandSculptAt(const StrandSet& input, std::size_t s, const SculptEdits& edits, const ScalpMesh* scalp, bool fill) {
    if (s >= input.strandCount()) return {};
    if (!fill) return editOf(input, s, edits, scalp, nullptr);
    const FillIndex index(edits, scalp);
    return editOf(input, s, edits, scalp, &index);
}

ResolvedStrandSculpts resolveStrandSculpts(const StrandSet& in, const SculptEdits& edits, const ScalpMesh* scalp, bool fill) {
    ResolvedStrandSculpts r;
    const std::size_t count = in.strandCount();
    r.ranges.assign(2 * count, 0);
    r.frames.assign(8 * count, 0.0f);
    if (edits.strands.empty()) return r;
    const std::unique_ptr<const FillIndex> index = fill && scalp ? std::make_unique<const FillIndex>(edits, scalp) : nullptr;
    std::vector<GuideSculpt> per(count);
    parallelFor(0, count, [&](std::size_t s) {
        Vec3f normal, across;
        rootFrame(in, s, scalp, &normal, &across);
        float* f = r.frames.data() + 8 * s;
        f[0] = normal.x, f[1] = normal.y, f[2] = normal.z;
        f[4] = across.x, f[5] = across.y, f[6] = across.z;
        if (in.strandPointCount(s) >= 2) per[s] = editOf(in, s, edits, scalp, index.get());
    });
    std::size_t total = 0;
    for (std::size_t s = 0; s < count; ++s) {
        r.ranges[2 * s] = static_cast<std::uint32_t>(total);
        r.ranges[2 * s + 1] = static_cast<std::uint32_t>(per[s].samples.size());
        total += per[s].samples.size();
    }
    r.samples.resize(total);
    parallelFor(0, count, [&](std::size_t s) { std::copy(per[s].samples.begin(), per[s].samples.end(), r.samples.begin() + r.ranges[2 * s]); });
    r.any = total > 0;
    return r;
}

StrandsPtr applyResolvedStrandSculpts(const StrandsPtr& input, const ResolvedStrandSculpts& resolved, float strength) {
    if (!input || !resolved.any || strength == 0.0f || resolved.strandCount() != input->strandCount()) return input;
    const StrandSet& in = *input;
    auto out = std::make_shared<StrandSet>(in);
    out->mutablePositions();  // (cloned once, before the threads write)
    std::atomic<bool> changed{false};
    parallelFor(0, in.strandCount(), [&](std::size_t s) {
        const std::uint32_t first = resolved.ranges[2 * s], n = resolved.ranges[2 * s + 1];
        if (in.strandPointCount(s) < 2 || n == 0) return;
        GuideSculpt edit;
        edit.samples.assign(resolved.samples.begin() + first, resolved.samples.begin() + first + n);
        const float* f = resolved.frames.data() + 8 * s;
        const std::vector<Vec3f> pts = applySculptFramed(pointsOf(in, s), edit, strength, {f[0], f[1], f[2]}, {f[4], f[5], f[6]});
        std::copy(pts.begin(), pts.end(), out->strandPoints(s));
        changed.store(true, std::memory_order_relaxed);
    });
    return changed.load() ? StrandsPtr(out) : input;
}

StrandsPtr applyStrandSculpts(const StrandsPtr& input, const SculptEdits& edits, float strength, const ScalpMesh* scalp, bool fill) {
    if (!input || edits.strands.empty() || strength == 0.0f) return input;
    return applyResolvedStrandSculpts(input, resolveStrandSculpts(*input, edits, scalp, fill), strength);
}

namespace {

// The input's roots' places (rest pose; without a scalp, the first points) in a tree: the spacing
// a recorded strand keeps, the fill's reach.
struct StrandPlaces {
    std::vector<Vec3f> places;
    PointIndex index;

    StrandPlaces(const StrandSet& input, const ScalpMesh* scalp) : places(input.strandCount()) {
        parallelFor(0, places.size(), [&](std::size_t s) {
            if (!restRoot(scalp, input.roots()[s], &places[s])) places[s] = input.strandPoints(s)[0];
        });
        index = PointIndex(places);
    }

    // The mean distance to the few nearest other roots: the local spacing (less noisy than the nearest alone).
    float spacing(std::size_t s) const {
        constexpr std::size_t kSpacingNeighbours = 4;
        std::size_t found[kSpacingNeighbours + 1];
        float d2[kSpacingNeighbours + 1];
        const std::size_t n = index.kNearest(places[s], kSpacingNeighbours + 1, found, d2);
        float sum = 0.0f;
        std::size_t others = 0;
        for (std::size_t k = 0; k < n; ++k) {
            if (found[k] == s) continue;
            sum += std::sqrt(d2[k]);
            if (++others == kSpacingNeighbours) break;
        }
        return others > 0 ? sum / static_cast<float>(others) : 0.0f;
    }
};

// Strand `s` of `input` taken to `sculpted` (its points) at `strength`, as recorded.
StrandSculpt recordStrand(const StrandSet& input, std::size_t s, const std::vector<Vec3f>& sculpted, float strength,
                          const ScalpMesh* scalp, const StrandPlaces& places) {
    StrandSculpt r;
    r.root = input.roots()[s];
    r.spacing = places.spacing(s);
    Vec3f normal, across;
    rootFrame(input, s, scalp, &normal, &across);
    GuideSculpt edit = recordSculptFramed(pointsOf(input, s), sculpted, strength, normal, across);
    if (!edit.identity()) r.edit = std::move(edit);
    return r;
}

void checkStrength(float strength) {
    if (std::abs(strength) < kMinStrength) throw std::invalid_argument("the SculptStrands node's strength is 0: raise it to sculpt");
}

}  // namespace

std::vector<StrandId> recordStrandStroke(SculptEdits& edits, const StrandSet& input,
                                         const std::vector<std::pair<std::size_t, std::vector<Vec3f>>>& sculpted, float strength,
                                         const ScalpMesh* scalp) {
    checkStrength(strength);
    for (const auto& [s, points] : sculpted) {
        if (s >= input.strandCount()) throw std::invalid_argument("a sculpted strand is not one of the node's input");
        if (points.size() != input.strandPointCount(s)) throw std::invalid_argument("a sculpted strand has another point count than its input");
    }
    if (sculpted.empty()) return {};
    const StrandPlaces places(input, scalp);
    std::vector<StrandSculpt> made(sculpted.size());
    parallelFor(0, sculpted.size(), [&](std::size_t i) {
        made[i] = recordStrand(input, sculpted[i].first, sculpted[i].second, strength, scalp, places);
    });
    std::vector<StrandId> ids;
    ids.reserve(made.size());
    for (std::size_t i = 0; i < made.size(); ++i) {
        const StrandId id = input.ids()[sculpted[i].first];
        edits.strands[id] = std::move(made[i]);
        ids.push_back(id);
    }
    return ids;
}

std::shared_ptr<const GuideSet> strandsAsGuides(const StrandSet& strands, const Vec3f& color) {
    auto out = std::make_shared<GuideSet>();
    out->setDisplayColor(color);
    out->guides().reserve(strands.strandCount());
    const PreviewCore core(strands);
    for (std::size_t s = 0; s < strands.strandCount(); ++s) {
        if (core.hidden(s) || strands.strandPointCount(s) < 2) continue;
        Guide& g = out->add(strands.ids()[s], pointsOf(strands, s));
        g.root = strands.roots()[s];
    }
    return out;
}

struct StrandStrokeRecord::State {
    StrandsPtr input;
    const ScalpMesh* scalp = nullptr;
    std::unique_ptr<StrandPlaces> places;  // (made at the first dab that records)
    std::vector<Vec3f> seen;               // each strand's points as last recorded (the output at the start)
    bool first = true;
};

StrandStrokeRecord::StrandStrokeRecord() = default;
StrandStrokeRecord::~StrandStrokeRecord() = default;

void StrandStrokeRecord::clear() { state_.reset(); }

bool StrandStrokeRecord::started() const { return state_ != nullptr; }

void StrandStrokeRecord::begin(const StrandsPtr& input, const StrandsPtr& output, const ScalpMesh* scalp) {
    if (!input || !output || output->pointCount() != input->pointCount() || output->strandCount() != input->strandCount()) {
        throw std::invalid_argument("the SculptStrands node's output is not its input's strands");
    }
    state_ = std::make_unique<State>();
    state_->input = input;
    state_->scalp = scalp;
    state_->seen = output->positions();
}

std::size_t StrandStrokeRecord::record(SculptEdits& edits, const SculptEdits& stored, const GuideHierarchy& sculpted, float strength) {
    if (!state_) throw std::invalid_argument("a SculptStrands stroke recorded before it began");
    checkStrength(strength);
    State& st = *state_;
    if (st.first) {
        edits = stored;
        st.first = false;
    }
    const StrandSet& in = *st.input;
    // The strands whose shaped points changed since last recorded.
    std::vector<std::uint8_t> moved(in.strandCount(), 0);
    parallelFor(0, in.strandCount(), [&](std::size_t s) {
        const HierarchyGuide* g = sculpted.guide(in.ids()[s]);
        const std::size_t n = in.strandPointCount(s);
        if (!g || g->points.size() != n) return;
        Vec3f* seen = st.seen.data() + in.strandBegin(s);
        if (std::equal(g->points.begin(), g->points.end(), seen, [](const Vec3f& a, const Vec3f& b) { return a.x == b.x && a.y == b.y && a.z == b.z; })) {
            return;
        }
        std::copy(g->points.begin(), g->points.end(), seen);
        moved[s] = 1;
    });
    std::vector<std::size_t> changed;
    for (std::size_t s = 0; s < moved.size(); ++s) {
        if (moved[s]) changed.push_back(s);
    }
    if (changed.empty()) return 0;
    if (!st.places) st.places = std::make_unique<StrandPlaces>(in, st.scalp);
    std::vector<StrandSculpt> made(changed.size());
    parallelFor(0, changed.size(), [&](std::size_t i) {
        const std::size_t s = changed[i];
        const Vec3f* pts = st.seen.data() + in.strandBegin(s);
        made[i] = recordStrand(in, s, std::vector<Vec3f>(pts, pts + in.strandPointCount(s)), strength, st.scalp, *st.places);
    });
    for (std::size_t i = 0; i < changed.size(); ++i) edits.strands[in.ids()[changed[i]]] = std::move(made[i]);
    return changed.size();
}

}  // namespace groomlab
