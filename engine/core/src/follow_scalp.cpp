// SPDX-License-Identifier: Apache-2.0
#include "groomlab/follow_scalp.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <unordered_map>

#include "groomlab/cards.h"
#include "groomlab/display.h"
#include "groomlab/feathers.h"
#include "groomlab/frames.h"
#include "groomlab/guide_hierarchy.h"
#include "groomlab/guide_set.h"
#include "groomlab/instances.h"
#include "groomlab/parallel.h"
#include "groomlab/random.h"
#include "groomlab/strand_set.h"

namespace groomlab {

namespace {

bool finite(const Vec3f& v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }

// A frame from an origin, a normal and an edge direction (made perpendicular to the normal).
RootFrame frameFrom(const Vec3f& origin, const Vec3f& normal, const Vec3f& edgeIn) {
    RootFrame f;
    const Vec3f n = normal;
    Vec3f edge = edgeIn - n * edgeIn.dot(n);
    const Vec3f x = edge.length() > 1e-9f ? edge.normalized() : anyPerpendicular(n);
    f.origin = origin;
    f.x = x;
    f.y = n;
    f.z = x.cross(n);
    f.valid = finite(origin) && finite(x) && finite(n) && finite(f.z) && n.length() > 0.5f;
    return f;
}

}  // namespace

int longestEdge(const ScalpMesh& scalp, const StrandRoot& root) {
    if (!scalp.contains(root)) return 0;
    const auto& tri = scalp.triangles()[static_cast<std::size_t>(root.face)];
    int best = 0;
    float length = -1.0f;
    for (int e = 0; e < 3; ++e) {
        const float l = (scalp.points()[static_cast<std::size_t>(tri[(e + 1) % 3])] - scalp.points()[static_cast<std::size_t>(tri[e])]).length();
        if (l > length) {
            length = l;
            best = e;
        }
    }
    return best;
}

RootFrame rootFrameOn(const ScalpMesh& scalp, const StrandRoot& root, int edge) {
    if (!scalp.contains(root)) return RootFrame{};
    const auto& tri = scalp.triangles()[static_cast<std::size_t>(root.face)];
    if (edge < 0) edge = longestEdge(scalp, root);
    return frameFrom(scalp.position(root), scalp.normal(root),
                     scalp.points()[static_cast<std::size_t>(tri[(edge + 1) % 3])] - scalp.points()[static_cast<std::size_t>(tri[edge])]);
}

Vec3f RootMotion::point(const Vec3f& p) const {
    if (still()) return p;
    const Vec3f d = p - from.origin;
    const float lx = d.dot(from.x), ly = d.dot(from.y), lz = d.dot(from.z);
    return to.origin + to.x * lx + to.y * ly + to.z * lz;
}

Vec3f RootMotion::direction(const Vec3f& v) const {
    if (still()) return v;
    const float lx = v.dot(from.x), ly = v.dot(from.y), lz = v.dot(from.z);
    return to.x * lx + to.y * ly + to.z * lz;
}

RootMotion rootMotion(const ScalpMesh& from, const ScalpMesh& to, const StrandRoot& root) {
    const int edge = longestEdge(from, root);  // one edge on both: the face's longest on `from`
    return {rootFrameOn(from, root, edge), rootFrameOn(to, root, edge)};
}

namespace {

// The strands' (ID, index) pairs sorted (an ID's first strand first), for finding an instance's
// strand: kept for the last strands asked, as a playback carries the same strands at rest every
// frame (an index of 180k strands took ~40 ms a frame to make).
using StrandIndex = std::vector<std::pair<StrandId, std::uint32_t>>;
std::shared_ptr<const StrandIndex> strandsById(const StrandsPtr& strands) {
    static std::mutex mutex;
    static std::weak_ptr<const StrandSet> keptFor;
    static std::shared_ptr<const StrandIndex> kept;
    {
        const std::lock_guard<std::mutex> lock(mutex);
        if (kept && keptFor.lock() == strands) return kept;
    }
    auto index = std::make_shared<StrandIndex>(strands->strandCount());
    const std::vector<StrandId>& ids = strands->ids();
    for (std::size_t s = 0; s < ids.size(); ++s) (*index)[s] = {ids[s], static_cast<std::uint32_t>(s)};
    std::sort(index->begin(), index->end());
    const std::lock_guard<std::mutex> lock(mutex);
    keptFor = strands;
    kept = index;
    return index;
}

// Each strand's root motion; false when every one is still. With the strands' boxes (made from
// them on `from`), each root's frame and edge on `from` read from them: rootMotion's own.
bool strandMotions(const StrandSet& strands, const ScalpMesh& from, const ScalpMesh& to, const StrandBoxes* boxes,
                   std::vector<RootMotion>& out) {
    const std::size_t n = strands.strandCount();
    out.resize(n);
    std::vector<char> moved(n, 0);
    const std::vector<StrandRoot>& roots = strands.roots();
    if (boxes && (boxes->frames.size() != n || boxes->edges.size() != n)) boxes = nullptr;  // (other strands' boxes)
    parallelFor(0, n, [&](std::size_t s) {
        out[s] = boxes ? RootMotion{boxes->frames[s], rootFrameOn(to, roots[s], boxes->edges[s])} : rootMotion(from, to, roots[s]);
        moved[s] = out[s].still() ? 0 : 1;
    }, 1024);
    for (const char m : moved) {
        if (m) return true;
    }
    return false;
}

CardMesh::Card carryCard(const CardMesh::Card& card, const RootMotion& m) {
    CardMesh::Card out = card;
    for (Vec3f& p : out.center) p = m.point(p);
    for (Vec3f& p : out.refCenter) p = m.point(p);
    for (Vec3f& p : out.base) p = m.point(p);
    for (auto* list : {&out.side, &out.normal, &out.shading, &out.refSide, &out.refNormal, &out.refShading}) {
        for (Vec3f& v : *list) v = m.direction(v);
    }
    return out;
}

}  // namespace

StrandsPtr carryStrands(const StrandsPtr& strands, const ScalpMesh& from, const ScalpMesh& to, const StrandBoxes* boxes) {
    if (!strands) return strands;
    std::vector<RootMotion> motions;
    const bool strandsMove = strandMotions(*strands, from, to, boxes, motions);

    // Card layers: each card by its first strand's root (its source's, for baking, carried too).
    std::map<const StrandSet*, StrandsPtr> carriedSources;
    bool cardsMove = false;
    std::vector<std::shared_ptr<const CardMesh>> layers;
    for (const auto& layer : strands->cardLayers()) {
        if (!layer) {
            layers.push_back(layer);
            continue;
        }
        auto carried = std::make_shared<CardMesh>(*layer);
        bool moved = false;
        for (CardMesh::Card& card : carried->cards) {
            const StrandSet* source = card.source ? card.source.get() : strands.get();
            if (card.strands.empty() || card.strands.front() >= source->strandCount()) continue;
            const RootMotion m = rootMotion(from, to, source->roots()[card.strands.front()]);
            if (card.source) {
                auto it = carriedSources.find(card.source.get());
                if (it == carriedSources.end()) it = carriedSources.emplace(card.source.get(), carryStrands(card.source, from, to)).first;
                if (it->second != card.source) moved = true;
                card.source = it->second;
            }
            if (m.still()) continue;
            card = carryCard(card, m);
            moved = true;
        }
        cardsMove = cardsMove || moved;
        layers.push_back(moved ? std::shared_ptr<const CardMesh>(carried) : layer);
    }

    // Rachis rings: each shaft by the scalp point nearest its base on `from`.
    bool feathersMove = false;
    std::vector<std::shared_ptr<const FeatherLayer>> feathers;
    for (const auto& layer : strands->featherLayers()) {
        if (!layer || layer->rings.empty()) {
            feathers.push_back(layer);
            continue;
        }
        auto carried = std::make_shared<FeatherLayer>(*layer);
        bool moved = false;
        for (std::size_t k = 0; k + 1 < carried->offsets.size(); ++k) {
            const std::size_t begin = carried->offsets[k], end = carried->offsets[k + 1];
            if (begin >= end || end > carried->rings.size()) continue;
            const SurfacePoint base = from.closestPoint(carried->rings[begin].center);
            if (base.root.face < 0) continue;
            const RootMotion m = rootMotion(from, to, base.root);
            if (m.still()) continue;
            moved = true;
            for (std::size_t r = begin; r < end; ++r) {
                RachisRing& ring = carried->rings[r];
                ring.center = m.point(ring.center);
                ring.tangent = m.direction(ring.tangent);
                ring.normal = m.direction(ring.normal);
                ring.side = m.direction(ring.side);
            }
        }
        feathersMove = feathersMove || moved;
        feathers.push_back(moved ? std::shared_ptr<const FeatherLayer>(carried) : layer);
    }

    // Instances: a fixed direction (Random's +Y) turns with its strand's root.
    std::shared_ptr<const InstanceSet> instances = strands->instances();
    if (strandsMove && instances && !instances->empty()) {
        const auto index = strandsById(strands);
        auto turned = std::make_shared<InstanceSet>(*instances);
        parallelFor(0, turned->instances.size(), [&](std::size_t k) {
            InstanceSet::Instance& i = turned->instances[k];
            const auto it = std::lower_bound(index->begin(), index->end(), std::make_pair(i.strand, std::uint32_t{0}));
            if (it != index->end() && it->first == i.strand) i.direction = motions[it->second].direction(i.direction);
        }, 1024);
        instances = turned;
    }

    if (!strandsMove && !cardsMove && !feathersMove) return strands;
    auto out = std::make_shared<StrandSet>(*strands);
    if (strandsMove) {
        // Every point written into an array of its own (not the input's cloned first, then moved),
        // a strand at a time, in parallel.
        const std::vector<Vec3f>& from = strands->positions();
        Vec3f* to = out->overwritePositions().data();
        const std::vector<std::size_t>& offsets = strands->offsets();
        parallelFor(0, out->strandCount(), [&](std::size_t s) {
            const RootMotion& m = motions[s];
            const std::size_t begin = offsets[s], end = offsets[s + 1];
            if (m.still()) {
                std::copy(from.begin() + static_cast<std::ptrdiff_t>(begin), from.begin() + static_cast<std::ptrdiff_t>(end), to + begin);
                return;
            }
            for (std::size_t k = begin; k < end; ++k) to[k] = m.point(from[k]);
        }, 256);
        out->setInstances(instances);
    }
    if (cardsMove) out->setCardLayers(std::move(layers));
    if (feathersMove) out->setFeatherLayers(std::move(feathers));
    return out;
}

bool carriesPointsAndInstances(const StrandSet& strands) {
    for (const auto& layer : strands.cardLayers()) {
        if (layer && !layer->cards.empty()) return false;
    }
    for (const auto& layer : strands.featherLayers()) {
        if (layer && !layer->rings.empty()) return false;
    }
    return !strands.instances() || !strands.instances()->hasMeshes();
}

std::shared_ptr<const StrandBoxes> strandBoxes(const StrandSet& strands, const ScalpMesh& from) {
    auto out = std::make_shared<StrandBoxes>();
    const std::size_t n = strands.strandCount();
    out->centers.resize(n);
    out->halves.resize(n);
    out->frames.resize(n);
    out->edges.resize(n);
    std::vector<char> pointsFinite(n, 1);
    parallelFor(0, n, [&](std::size_t s) {
        constexpr float kBig = std::numeric_limits<float>::max();
        const int edge = longestEdge(from, strands.roots()[s]);
        out->edges[s] = static_cast<std::int8_t>(edge);
        out->frames[s] = rootFrameOn(from, strands.roots()[s], edge);
        Vec3f lo(kBig, kBig, kBig), hi(-kBig, -kBig, -kBig);
        const Vec3f* pts = strands.strandPoints(s);
        for (std::size_t k = 0, count = strands.strandPointCount(s); k < count; ++k) {
            const Vec3f& p = pts[k];
            if (!finite(p)) {
                pointsFinite[s] = 0;
                continue;
            }
            lo = minPerAxis(lo, p);
            hi = maxPerAxis(hi, p);
        }
        if (lo.x > hi.x) {
            out->centers[s] = Vec3f();
            out->halves[s] = Vec3f(-1.0f, -1.0f, -1.0f);
            return;
        }
        out->centers[s] = (lo + hi) * 0.5f;
        out->halves[s] = (hi - lo) * 0.5f;
    }, 1024);
    for (const char f : pointsFinite) out->finite = out->finite && f;
    out->longestStrand = strands.extent().longestStrand;
    return out;
}

StrandSet::Extent carriedBounds(const StrandSet& strands, const StrandBoxes& boxes, const ScalpMesh& to) {
    constexpr float kBig = std::numeric_limits<float>::max();
    StrandSet::Extent out;
    out.lo = Vec3f(kBig, kBig, kBig);
    out.hi = Vec3f(-kBig, -kBig, -kBig);
    out.finite = boxes.finite;
    out.longestStrand = boxes.longestStrand;
    const std::size_t n = std::min(strands.strandCount(), boxes.centers.size());
    constexpr std::size_t kBlock = 8192;
    const std::size_t blocks = (n + kBlock - 1) / kBlock;
    std::vector<StrandSet::Extent> parts(blocks);
    const std::vector<StrandRoot>& roots = strands.roots();
    parallelFor(0, blocks, [&](std::size_t b) {
        Vec3f lo(kBig, kBig, kBig), hi(-kBig, -kBig, -kBig);
        for (std::size_t s = b * kBlock; s < std::min(n, (b + 1) * kBlock); ++s) {
            const Vec3f& h = boxes.halves[s];
            if (h.x < 0.0f) continue;
            const RootMotion m{boxes.frames[s], rootFrameOn(to, roots[s], boxes.edges[s])};  // (rootMotion's)
            Vec3f c = boxes.centers[s], e = h;
            if (!m.still()) {
                // The box turned: its center carried, its half extents through |M|, M the turn
                // (to's axes by from's: M = sum over a of to.a from.a^T).
                c = m.point(c);
                const Vec3f f[3] = {m.from.x, m.from.y, m.from.z}, t[3] = {m.to.x, m.to.y, m.to.z};
                float row[3][3];
                for (int j = 0; j < 3; ++j) {
                    for (int i = 0; i < 3; ++i) {
                        float v = 0.0f;
                        for (int a = 0; a < 3; ++a) v += (&t[a].x)[j] * (&f[a].x)[i];
                        row[j][i] = std::abs(v);
                    }
                }
                e = Vec3f(row[0][0] * h.x + row[0][1] * h.y + row[0][2] * h.z, row[1][0] * h.x + row[1][1] * h.y + row[1][2] * h.z,
                          row[2][0] * h.x + row[2][1] * h.y + row[2][2] * h.z);
                // (Rounding: a carried point may sit a hair outside the turned box.)
                const float pad = 1e-5f * (std::abs(c.x) + std::abs(c.y) + std::abs(c.z) + e.x + e.y + e.z) + 1e-6f;
                e = e + Vec3f(pad, pad, pad);
            }
            if (!finite(c) || !finite(e)) continue;
            lo = Vec3f(std::min(lo.x, c.x - e.x), std::min(lo.y, c.y - e.y), std::min(lo.z, c.z - e.z));
            hi = Vec3f(std::max(hi.x, c.x + e.x), std::max(hi.y, c.y + e.y), std::max(hi.z, c.z + e.z));
        }
        parts[b].lo = lo;
        parts[b].hi = hi;
    }, 1);
    for (const StrandSet::Extent& e : parts) {
        if (e.empty()) continue;
        out.lo = minPerAxis(out.lo, e.lo);
        out.hi = maxPerAxis(out.hi, e.hi);
    }
    return out;
}

FollowCarry::FollowCarry(StrandsPtr rest, MeshPtr from, MeshPtr to) : rest_(std::move(rest)), from_(std::move(from)), to_(std::move(to)) {}

StrandsPtr FollowCarry::carried() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!carried_) carried_ = rest_ && from_ && to_ ? carryStrands(rest_, *from_, *to_) : rest_;
    return carried_;
}

std::uint64_t writeFollowRoots(const StrandSet& strands, const ScalpMesh& from, FollowRoot* out) {
    const std::size_t n = strands.strandCount();
    const std::vector<StrandRoot>& roots = strands.roots();
    constexpr std::size_t kChunk = std::size_t{1} << 14;
    const std::size_t chunks = (n + kChunk - 1) / kChunk;
    std::vector<std::uint64_t> digests(chunks);
    parallelFor(0, chunks, [&](std::size_t c) {
        const std::size_t begin = c * kChunk, end = std::min(n, begin + kChunk);
        for (std::size_t s = begin; s < end; ++s) {
            const StrandRoot& root = roots[s];
            FollowRoot r;
            if (from.contains(root)) {
                const auto& tri = from.triangles()[static_cast<std::size_t>(root.face)];
                for (int k = 0; k < 3; ++k) r.corners[k] = static_cast<std::uint32_t>(tri[k]);
                r.edge = static_cast<std::uint32_t>(longestEdge(from, root));
                r.u = root.u;
                r.v = root.v;
            } else {
                r.edge = kFollowUnrooted;
            }
            out[s] = r;
        }
        digests[c] = bufferDigest(out + begin, (end - begin) * sizeof(FollowRoot));
    }, 1);
    return hashCombine(bufferDigest(digests.data(), digests.size() * sizeof(std::uint64_t)), n) | 1u;
}

GuidesPtr carryGuidesBack(const GuidesPtr& guides, const ScalpMesh& from, const ScalpMesh& to) {
    if (!guides) return guides;
    std::shared_ptr<GuideSet> out;
    for (std::size_t g = 0; g < guides->guides().size(); ++g) {
        const Guide& guide = guides->guides()[g];
        const RootMotion back = rootMotion(from, to, guide.root).inverse();
        if (back.still()) continue;
        if (!out) out = std::make_shared<GuideSet>(*guides);
        for (Vec3f& p : out->guides()[g].points) p = back.point(p);
    }
    return out ? GuidesPtr(out) : guides;
}

GuidesPtr carryPosesBack(const GuidesPtr& poses, const GuideHierarchy& rooted, const ScalpMesh& from, const ScalpMesh& to) {
    if (!poses) return poses;
    std::shared_ptr<GuideSet> out;
    for (std::size_t g = 0; g < poses->guides().size(); ++g) {
        const HierarchyGuide* guide = rooted.guide(poses->guides()[g].id);
        if (!guide) continue;
        const RootMotion back = rootMotion(from, to, guide->root).inverse();
        if (back.still()) continue;
        if (!out) out = std::make_shared<GuideSet>(*poses);
        for (Vec3f& p : out->guides()[g].points) p = back.point(p);
    }
    return out ? GuidesPtr(out) : poses;
}

std::shared_ptr<GuideHierarchy> carryHierarchyBack(const GuideHierarchy& hierarchy, const ScalpMesh& from, const ScalpMesh& to) {
    auto out = std::make_shared<GuideHierarchy>(hierarchy);
    for (const HierarchyGuide& g : hierarchy.guides()) {
        const RootMotion back = rootMotion(from, to, g.root).inverse();
        if (back.still() || g.points.size() < 2) continue;
        std::vector<Vec3f> points = g.points;
        for (Vec3f& p : points) p = back.point(p);
        out->pose(g.id, std::move(points), back.direction(g.frameUp));
    }
    return out;
}

ScalpHistoryPtr historyAtRest(const ScalpHistory& history, const ScalpMesh& rest, const ScalpMesh& now) {
    const std::size_t n = now.pointCount();
    // Each vertex's frame: its normal (the smooth normal at its corner of a face) and its longest
    // edge at rest (a short one, such as between a pole's nearly coincident vertices, points
    // nowhere in particular and would not turn with the head).
    std::vector<StrandRoot> corners(n, StrandRoot{});
    std::vector<std::int32_t> next(n, -1);
    std::vector<float> longest(n, 0.0f);
    const auto& tris = now.triangles();
    const std::vector<Vec3f>& at = rest.points();
    for (std::size_t t = 0; t < tris.size(); ++t) {
        for (int c = 0; c < 3; ++c) {
            const auto v = static_cast<std::size_t>(tris[t][c]);
            for (const int o : {(c + 1) % 3, (c + 2) % 3}) {
                const auto w = static_cast<std::size_t>(tris[t][o]);
                const float length = (at[w] - at[v]).length();
                if (!(length > longest[v])) continue;
                longest[v] = length;
                corners[v] = {static_cast<std::int32_t>(t), c == 1 ? 1.0f : 0.0f, c == 2 ? 1.0f : 0.0f};
                next[v] = tris[t][o];
            }
        }
    }
    auto vertexFrame = [&](const ScalpMesh& mesh, std::size_t v) {
        if (next[v] < 0) return RootFrame{};
        return frameFrom(mesh.points()[v], mesh.normal(corners[v]), mesh.points()[static_cast<std::size_t>(next[v])] - mesh.points()[v]);
    };
    std::vector<RootMotion> toRest(n);
    parallelFor(0, n, [&](std::size_t v) { toRest[v] = {vertexFrame(now, v), vertexFrame(rest, v)}; }, 1024);
    std::vector<std::vector<Vec3f>> frames(history.frames.size());
    parallelFor(0, history.frames.size(), [&](std::size_t k) {
        const std::vector<Vec3f>& was = history.frames[k];
        if (was.size() != n) {
            frames[k] = was;  // another topology then: Jiggle reads it as still
            return;
        }
        std::vector<Vec3f>& out = frames[k];
        out.resize(n);
        for (std::size_t v = 0; v < n; ++v) {
            // Where the vertex was, relative to where it is now, turned into rest space.
            out[v] = was[v] == now.points()[v] ? rest.points()[v]
                                                : rest.points()[v] + toRest[v].direction(was[v] - now.points()[v]);
        }
    }, 1);
    return ScalpHistory::make(history.step, std::move(frames), history.loopSeconds);
}

}  // namespace groomlab
