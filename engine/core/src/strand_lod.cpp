// SPDX-License-Identifier: Apache-2.0
// Screen-space level of detail for strand lines (strand_lod.h, docs/research/clump_lod.md).

#include "groomlab/portable_math.h"
#include "groomlab/strand_lod.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <limits>
#include <unordered_map>

#include "groomlab/display.h"
#include "groomlab/parallel.h"
#include "groomlab/random.h"

namespace groomlab {

namespace {

// Members a group's shape is measured on (its first by rank: a fair sample, the same ones each time).
constexpr std::size_t kMeasured = 64;
// Members a group's impostor color is the mean of.
constexpr std::size_t kColored = 256;
// Strands a cell of roots holds on average, for strands without a clump channel.
constexpr double kCellMembers = 256.0;

// A digest of a large array, in chunks side by side, combined in order.
std::uint64_t digestOf(const void* data, std::size_t bytes) {
    constexpr std::size_t kChunk = std::size_t{1} << 20;
    const std::size_t chunks = (bytes + kChunk - 1) / kChunk;
    if (chunks <= 1) return bufferDigest(data, bytes);
    std::vector<std::uint64_t> parts(chunks);
    const auto* p = static_cast<const unsigned char*>(data);
    parallelFor(0, chunks, [&](std::size_t c) {
        const std::size_t begin = c * kChunk, end = std::min(bytes, begin + kChunk);
        parts[c] = bufferDigest(p + begin, end - begin);
    }, 1);
    return hashCombine(bufferDigest(parts.data(), parts.size() * sizeof(std::uint64_t)), bytes);
}

// Point k of an axis of `axisPoints` points on a strand of n points: the nearest by share of length.
std::size_t pointAt(std::size_t k, std::size_t axisPoints, std::size_t n) {
    if (axisPoints <= 1 || n <= 1) return 0;
    return static_cast<std::size_t>(std::lround(static_cast<double>(k) * static_cast<double>(n - 1) / static_cast<double>(axisPoints - 1)));
}

// A view's StrandLodView::pixelsAt and pixelsAtFarthest with the levels' exponentials taken once
// (the same arithmetic, so the same values: select asks them for every point).
struct ViewScales {
    float scale = 0.0f, nearDepth = 0.0f, farDepth = 0.0f, flat = 0.0f;
    bool fixed = false;  // orthographic or no level: `flat` everywhere
    explicit ViewScales(const StrandLodView& view) : scale(view.pixelScale) {
        const float center = lodLevelPixels(view.level);
        fixed = view.orthographic || !std::isfinite(center);
        flat = center;
        if (fixed) return;
        nearDepth = scale / center;
        farDepth = scale / lodLevelPixels(view.farLevel != kNoLodLevel ? view.farLevel : view.level);
    }
    float at(float distance) const {
        if (fixed) return flat;
        const float depth = nearDepth - std::max(distance, 0.0f);
        if (!(depth > 0.0f)) return std::numeric_limits<float>::infinity();
        return scale / depth;
    }
    float atFarthest(float distance) const { return fixed ? flat : scale / (farDepth + std::max(distance, 0.0f)); }
};

}  // namespace

float lodClassExponent(int lodClass) {
    return pm::exp2(static_cast<float>(std::clamp(lodClass, 0, kLodClassCount - 1)) / static_cast<float>(kLodClassesPerOctave));
}

float StrandLodView::pixelsAt(float distance) const {
    const float center = lodLevelPixels(level);
    if (orthographic || !std::isfinite(center)) return center;
    const float depth = pixelScale / center - std::max(distance, 0.0f);
    if (!(depth > 0.0f)) return std::numeric_limits<float>::infinity();
    return pixelScale / depth;
}

float StrandLodView::pixelsAtFarthest(float distance) const {
    const float center = lodLevelPixels(farLevel != kNoLodLevel && level != kNoLodLevel ? farLevel : level);
    if (orthographic || !std::isfinite(center)) return center;
    return pixelScale / (pixelScale / center + std::max(distance, 0.0f));
}

StrandLodView lodView(const Vec3f& eye, const Vec3f& forward, float pixelScale, bool orthographic, const Vec3f& center,
                      const Vec3f& pivot, const StrandLodView& previous) {
    StrandLodView view;
    view.orthographic = orthographic;
    view.pixelScale = pixelScale;
    if (!(pixelScale > 0.0f) || !std::isfinite(pixelScale)) return view;
    if (orthographic) {
        view.level = view.farLevel = lodLevel(pixelScale, previous.level);
        return view;
    }
    // The pivot's depth stays as the camera orbits it; the center, e from it, comes as near as that
    // less e, goes as far as that plus e (the pivot the center itself: the center's depth).
    const float pivotDepth = (pivot - eye).dot(forward.normalized());
    const float offset = (center - pivot).length();
    const float nearest = pivotDepth - offset, farthest = pivotDepth + offset;
    if (!(nearest > 0.0f) || !std::isfinite(farthest)) return view;
    view.level = lodLevel(pixelScale / nearest, previous.level);
    view.farLevel = lodLevel(pixelScale / farthest, previous.farLevel);
    if (view.level == kNoLodLevel || view.farLevel == kNoLodLevel) view.level = view.farLevel = kNoLodLevel;
    return view;
}

StrandLodView lodView(const Vec3f& eye, const Vec3f& forward, float pixelScale, bool orthographic, const Vec3f& center,
                      const StrandLodView& previous) {
    return lodView(eye, forward, pixelScale, orthographic, center, center, previous);
}

int lodLevel(float pixelsPerUnit, int previous) {
    if (!(pixelsPerUnit > 0.0f) || !std::isfinite(pixelsPerUnit)) return kNoLodLevel;
    const double x = static_cast<double>(kLodLevelsPerOctave) * pm::log2(static_cast<double>(pixelsPerUnit));
    if (previous != kNoLodLevel && std::fabs(x - static_cast<double>(previous)) < 0.75) return previous;
    return static_cast<int>(std::lround(x));
}

float lodLevelPixels(int level) {
    if (level == kNoLodLevel) return std::numeric_limits<float>::infinity();
    return static_cast<float>(pm::exp2(static_cast<double>(level) / static_cast<double>(kLodLevelsPerOctave)));
}

float lodBandInk(float a) {
    if (!(a > 0.0f)) return 0.0f;
    if (a < 1e-3f) return a;  // thin: every fiber its own ink
    // Midpoints over [-L, L], L past where a phi(y) falls below 1e-4 of a pixel's share.
    const double reach = 4.5 + std::sqrt(2.0 * std::max(0.0, pm::log(static_cast<double>(a))));
    constexpr int kSteps = 96;
    const double dy = 2.0 * reach / kSteps;
    double sum = 0.0;
    for (int i = 0; i < kSteps; ++i) {
        const double y = -reach + (i + 0.5) * dy;
        const double phi = pm::exp(-0.5 * y * y) * 0.3989422804014327;
        sum += 1.0 - pm::exp(-static_cast<double>(a) * phi);
    }
    return static_cast<float>(sum * dy);
}

// The groups: their members by rank, and their shapes (measured on their first members).
struct StrandLod::Groups {
    std::uint64_t key = 0;               // the strands' IDs, counts and clump channel they were made for
    std::vector<std::uint32_t> first;    // group g's members: order[first[g], first[g + 1])
    std::vector<std::uint32_t> order;    // strand indices, by group, then rank
    std::vector<std::uint64_t> ids;      // a group's own ID (impostors')
    // Shapes: group g's axis points [axisFirst[g], axisFirst[g + 1]).
    std::vector<std::uint32_t> axisFirst;
    std::vector<Vec3f> axis;
    std::vector<float> sigma;  // the members' spread about the axis point, RMS across one direction
    std::vector<float> width;  // the members' mean width there
    std::vector<float> reach;  // how far the members reach from the bounding sphere's center there
    // Hysteresis: each group's last class (-1 none) and impostor flag.
    std::vector<std::int8_t> lastClass;
    std::vector<std::uint8_t> lastImpostor;
    // The selection's per-group choice: class (-1: an impostor) and members drawn.
    std::vector<std::int8_t> chosenClass;
    std::vector<std::uint32_t> chosenCount;

    std::size_t count() const { return first.empty() ? 0 : first.size() - 1; }
};

StrandLod::StrandLod() : groups_(std::make_unique<Groups>()) {}
StrandLod::~StrandLod() = default;

std::size_t StrandLod::groupCount() const { return groups_->count(); }

void StrandLod::setStrands(StrandsPtr strands, const std::string& clumpChannel) {
    if (strands == strands_ && strands) return;
    strands_ = std::move(strands);
    selectionCurrent_ = false;
    impostors_.reset();
    impostorsKey_ = 0;
    Groups& g = *groups_;
    if (!strands_ || strands_->strandCount() == 0) {
        *groups_ = Groups{};
        selection_ = Selection{};
        hasWidths_ = false;
        ++groupingSerial_;
        return;
    }
    const StrandSet& s = *strands_;
    const std::size_t n = s.strandCount();
    hasWidths_ = displayWidths(s) != nullptr;
    const std::vector<float>* channel = clumpChannel.empty() ? nullptr : s.strandChannel(clumpChannel);
    if (channel && channel->size() != n) channel = nullptr;
    // What the groups depend on: the IDs (ranks), the point counts, the clump channel.
    std::uint64_t key = hashCombine(digestOf(s.ids().data(), n * sizeof(StrandId)), digestOf(s.offsets().data(), s.offsets().size() * sizeof(std::size_t)));
    key = hashCombine(key, channel ? digestOf(channel->data(), n * sizeof(float)) : 0x5EEDull);
    const StrandSet::Extent extent = s.extent();
    if (!extent.empty()) {
        center_ = (extent.lo + extent.hi) * 0.5f;
        radius_ = 0.5f * (extent.hi - extent.lo).length();
    } else {
        center_ = Vec3f{};
        radius_ = 0.0f;
    }
    if (key != g.key || g.count() == 0) {
        // Group keys: the clump channel's values, or cells of the roots.
        std::vector<std::int64_t> keys(n);
        clumped_ = channel != nullptr;
        if (channel) {
            parallelFor(0, n, [&](std::size_t i) {
                const float v = (*channel)[i];
                keys[i] = std::isfinite(v) ? static_cast<std::int64_t>(std::llround(static_cast<double>(v))) : std::numeric_limits<std::int64_t>::min();
            }, 4096);
        } else {
            Vec3f lo{1e30f, 1e30f, 1e30f}, hi{-1e30f, -1e30f, -1e30f};
            for (std::size_t i = 0; i < n; ++i) {
                if (s.strandPointCount(i) == 0) continue;
                const Vec3f& p = s.strandPoints(i)[0];
                lo = minPerAxis(lo, p);
                hi = maxPerAxis(hi, p);
            }
            const float diagonal = lo.x <= hi.x ? (hi - lo).length() : 1.0f;
            const float cell = std::max(diagonal * static_cast<float>(std::sqrt(kCellMembers / 4.0 / static_cast<double>(n)) * 2.0), 1e-6f);
            parallelFor(0, n, [&](std::size_t i) {
                if (s.strandPointCount(i) == 0) {
                    keys[i] = std::numeric_limits<std::int64_t>::min();
                    return;
                }
                const Vec3f& p = s.strandPoints(i)[0];
                const auto q = [&](float v, float o) { return static_cast<std::int64_t>(std::floor((v - o) / cell)) & 0x1FFFFF; };
                keys[i] = (q(p.x, lo.x) << 42) | (q(p.y, lo.y) << 21) | q(p.z, lo.z);
            }, 4096);
        }
        std::unordered_map<std::int64_t, std::uint32_t> index;
        index.reserve(1024);
        std::vector<std::uint32_t> groupOf(n);
        std::vector<std::uint32_t> counts;
        std::vector<std::int64_t> groupKeys;
        for (std::size_t i = 0; i < n; ++i) {
            const auto [it, added] = index.emplace(keys[i], static_cast<std::uint32_t>(counts.size()));
            if (added) {
                counts.push_back(0);
                groupKeys.push_back(keys[i]);
            }
            groupOf[i] = it->second;
            ++counts[it->second];
        }
        const std::size_t groups = counts.size();
        g = Groups{};
        g.key = key;
        g.first.assign(groups + 1, 0);
        for (std::size_t k = 0; k < groups; ++k) g.first[k + 1] = g.first[k] + counts[k];
        g.order.resize(n);
        {
            std::vector<std::uint32_t> at(g.first.begin(), g.first.end() - 1);
            for (std::size_t i = 0; i < n; ++i) g.order[at[groupOf[i]]++] = static_cast<std::uint32_t>(i);
        }
        // Ranks: the preview's order (keepAtDensity: the hash of the ID), ties by index.
        const std::vector<StrandId>& ids = s.ids();
        parallelFor(0, groups, [&](std::size_t k) {
            auto* begin = g.order.data() + g.first[k];
            auto* end = g.order.data() + g.first[k + 1];
            std::sort(begin, end, [&](std::uint32_t a, std::uint32_t b) {
                const std::uint64_t ha = hash64(ids[a]), hb = hash64(ids[b]);
                return ha != hb ? ha < hb : a < b;
            });
        }, 1);
        g.ids.resize(groups);
        for (std::size_t k = 0; k < groups; ++k) {
            g.ids[k] = hashCombine(hash64(static_cast<std::uint64_t>(groupKeys[k])) ^ (clumped_ ? 0xC1u : 0xCEu), 0x1A90570Bull);
        }
        g.lastClass.assign(groups, -1);
        g.lastImpostor.assign(groups, 0);
        g.chosenClass.assign(groups, 0);
        g.chosenCount.assign(groups, 0);
        ++groupingSerial_;
        selection_ = Selection{};
    }
    measure();
}

void StrandLod::measure() {
    Groups& g = *groups_;
    const StrandSet& s = *strands_;
    const std::size_t groups = g.count();
    const float* widths = hasWidths_ ? s.widths().data() : nullptr;
    // Each group's axis points: as many as its first members' most.
    std::vector<std::uint32_t> points(groups, 0);
    parallelFor(0, groups, [&](std::size_t k) {
        std::size_t most = 0;
        const std::size_t members = std::min<std::size_t>(g.first[k + 1] - g.first[k], kMeasured);
        for (std::size_t r = 0; r < members; ++r) most = std::max(most, s.strandPointCount(g.order[g.first[k] + r]));
        points[k] = static_cast<std::uint32_t>(most);
    }, 64);
    g.axisFirst.assign(groups + 1, 0);
    for (std::size_t k = 0; k < groups; ++k) g.axisFirst[k + 1] = g.axisFirst[k] + points[k];
    g.axis.assign(g.axisFirst.back(), Vec3f{});
    g.sigma.assign(g.axisFirst.back(), 0.0f);
    g.width.assign(g.axisFirst.back(), 0.0f);
    g.reach.assign(g.axisFirst.back(), 0.0f);
    parallelFor(0, groups, [&](std::size_t k) {
        const std::size_t members = std::min<std::size_t>(g.first[k + 1] - g.first[k], kMeasured);
        const std::size_t axisPoints = points[k], at = g.axisFirst[k];
        if (axisPoints == 0 || members == 0) return;
        for (std::size_t r = 0; r < members; ++r) {
            const std::uint32_t i = g.order[g.first[k] + r];
            const std::size_t count = s.strandPointCount(i), begin = s.strandBegin(i);
            if (count == 0) continue;
            for (std::size_t p = 0; p < axisPoints; ++p) {
                const std::size_t q = pointAt(p, axisPoints, count);
                g.axis[at + p] += s.positions()[begin + q];
                if (widths) g.width[at + p] += widths[begin + q];
            }
        }
        const float share = 1.0f / static_cast<float>(members);
        for (std::size_t p = 0; p < axisPoints; ++p) {
            g.axis[at + p] = g.axis[at + p] * share;
            g.width[at + p] *= share;
        }
        for (std::size_t r = 0; r < members; ++r) {
            const std::uint32_t i = g.order[g.first[k] + r];
            const std::size_t count = s.strandPointCount(i), begin = s.strandBegin(i);
            if (count == 0) continue;
            for (std::size_t p = 0; p < axisPoints; ++p) {
                const Vec3f d = s.positions()[begin + pointAt(p, axisPoints, count)] - g.axis[at + p];
                g.sigma[at + p] += d.dot(d);
            }
        }
        // RMS across one direction: of the offsets' squares (across the strand, two directions) half.
        for (std::size_t p = 0; p < axisPoints; ++p) {
            g.sigma[at + p] = std::sqrt(0.5f * g.sigma[at + p] * share);
            g.reach[at + p] = (g.axis[at + p] - center_).length() + 2.0f * g.sigma[at + p];
        }
    }, 16);
    ++measuredSerial_;
    impostors_.reset();
    impostorsKey_ = 0;
}

const StrandLod::Selection& StrandLod::select(const StrandLodView& view, const StrandLodSettings& settings) {
    // The same strands, view and settings: the same selection (every rule below, its hysteresis
    // too, gives what it gave).
    if (selectionCurrent_ && selection_.view == view && selectedWith_.clumpChannel == settings.clumpChannel &&
        selectedWith_.error == settings.error && selectedWith_.widthScale == settings.widthScale &&
        selectedWith_.density == settings.density && selectedWith_.impostors == settings.impostors &&
        selectedWith_.splitBlended == settings.splitBlended) {
        return selection_;
    }
    selectionCurrent_ = true;
    Groups& g = *groups_;
    const std::size_t groups = g.count();
    const std::size_t total = strands_ ? strands_->strandCount() : 0;
    const float density = settings.density > 0.0f && settings.density < 1.0f ? settings.density : 1.0f;
    const float widthScale = settings.widthScale > 0.0f ? settings.widthScale : 1.0f;
    const float cStar = std::clamp(2.0f * settings.error, 0.02f, 0.9f);
    const double lnStar = pm::log(1.0 - static_cast<double>(cStar));
    Selection next;
    next.view = view;
    next.totalStrands = total;
    if (!strands_ || groups == 0 || !hasWidths_ || view.level == kNoLodLevel || !std::isfinite(view.pixelsAt(0.0f))) {
        // Nothing thinned: the item as it is (the groups forget their classes).
        std::fill(g.lastClass.begin(), g.lastClass.end(), static_cast<std::int8_t>(-1));
        std::fill(g.lastImpostor.begin(), g.lastImpostor.end(), static_cast<std::uint8_t>(0));
        next.drawnStrands = total;
        selection_ = std::move(next);
        selectedWith_ = settings;
        return selection_;
    }
    // Each group's class (rules in strand_lod.h), and whether it is drawn as one line: at the view
    // drawn last (other strands, a dab or a tweak) with a quarter class of hysteresis, so nothing
    // comes or goes for a small change; at another view as the rules say (the same view draws the
    // same strands whichever views came before).
    const ViewScales scales(view);
    const bool sameView = selection_.view == view && selectedWith_.error == settings.error &&
                          selectedWith_.density == settings.density && selectedWith_.widthScale == settings.widthScale;
    parallelFor(0, groups, [&](std::size_t k) {
        const std::size_t n = g.first[k + 1] - g.first[k];
        const std::size_t at = g.axisFirst[k], axisPoints = g.axisFirst[k + 1] - at;
        std::int8_t cls = 0;
        bool impostor = false;
        bool nearEye = false;
        for (std::size_t p = 0; p < axisPoints && !nearEye; ++p) nearEye = !std::isfinite(scales.at(g.reach[at + p]));
        if (n >= kLodMinGroup && axisPoints > 0 && !nearEye) {
            double coverage = 0.0;
            float spread = 0.0f, spreadPast = 0.0f;  // (past the root: the clump's cell spreads there)
            for (std::size_t p = 0; p < axisPoints; ++p) {
                // The point's scale where it comes closest as the camera orbits (a group that may
                // come to the eye is drawn whole).
                const float pixels = scales.at(g.reach[at + p]);
                coverage += std::clamp(static_cast<double>(g.width[at + p]) * widthScale * pixels, 0.0, 0.999);
                spread = std::max(spread, g.sigma[at + p] * pixels);
                if (p > 0 || axisPoints == 1) spreadPast = std::max(spreadPast, g.sigma[at + p] * pixels);
            }
            coverage /= static_cast<double>(axisPoints);
            double needed = 1.0;
            if (coverage > 0.0) needed = std::ceil(static_cast<double>(n) * pm::log(1.0 - coverage) / (static_cast<double>(density) * lnStar));
            needed = std::max(needed, std::ceil(4.0 * static_cast<double>(spread)));
            needed = std::clamp(needed, 1.0, static_cast<double>(n));
            const double x = static_cast<double>(kLodClassesPerOctave) * pm::log2(static_cast<double>(n) / needed);
            int c = static_cast<int>(std::floor(x + 1e-9));
            const int last = sameView ? g.lastClass[k] : -1;
            if (last >= 0 && x >= last - 0.25 && x < last + 1.25) c = last;
            cls = static_cast<std::int8_t>(std::clamp(c, 0, kLodClassCount - 1));
            if (settings.impostors) {
                const float limit = sameView && g.lastImpostor[k] ? kLodImpostorSpread * 1.15f : kLodImpostorSpread;
                impostor = spreadPast < limit;
            }
        }
        g.lastClass[k] = cls;
        g.lastImpostor[k] = impostor ? 1 : 0;
        g.chosenClass[k] = impostor ? static_cast<std::int8_t>(-1) : cls;
        const double e = lodClassExponent(cls);
        g.chosenCount[k] = impostor ? 0
                           : cls == 0 ? static_cast<std::uint32_t>(n)
                                      : static_cast<std::uint32_t>(std::clamp<long long>(std::llround(static_cast<double>(n) / e), 1, static_cast<long long>(n)));
    }, 16);
    // Class 0's segments that may blend at this view (lineKeepShare: their own coverage somewhere
    // within (kLineHashedBelow, 1)), from the nearest and farthest each point comes as the camera
    // orbits: a list of their own, which hosts draw after the rest without writing depth.
    const StrandSet& s = *strands_;
    const bool anyWhole = std::find(g.chosenClass.begin(), g.chosenClass.end(), std::int8_t{0}) != g.chosenClass.end();
    blendSegment_.assign(settings.splitBlended && anyWhole ? s.pointCount() : 0, 0);
    std::atomic<std::size_t> blendable{0};
    if (settings.splitBlended && anyWhole) {
        const float* widths = s.widths().data();
        const std::vector<Vec3f>& positions = s.positions();
        parallelFor(0, groups, [&](std::size_t k) {
            if (g.chosenClass[k] != 0) return;
            std::size_t here = 0;
            for (std::uint32_t r = 0; r < g.chosenCount[k]; ++r) {
                const std::uint32_t i = g.order[g.first[k] + r];
                const std::size_t begin = s.strandBegin(i), points = s.strandPointCount(i);
                float lastMin = 0.0f, lastMax = 0.0f;
                for (std::size_t p = 0; p < points; ++p) {
                    const float reach = (positions[begin + p] - center_).length();
                    const float width = widths[begin + p] * widthScale;
                    const float most = width * scales.at(reach), least = width * scales.atFarthest(reach);
                    if (p > 0) {
                        const float hi = std::max(most, lastMax), lo = std::min(least, lastMin);
                        if (hi > kLineHashedBelow && lo < 1.0f) {
                            blendSegment_[begin + p - 1] = 1;
                            ++here;
                        }
                    }
                    lastMin = least;
                    lastMax = most;
                }
            }
            blendable += here;
        }, 4);
    }
    next.blendSegments = blendable.load();
    // What is drawn, as a key: the groups' choices, the grouping, the blendable segments, the
    // impostors' widths' settings.
    std::uint64_t key = hashCombine(groupingSerial_, 0x10Dull);
    const std::uint64_t blendDigest = next.blendSegments > 0 ? digestOf(blendSegment_.data(), blendSegment_.size()) : 0;
    if (next.blendSegments > 0) key = hashCombine(key, blendDigest);
    std::uint64_t impostorKey = hashCombine(measuredSerial_, hashCombine(floatBits(widthScale), floatBits(density)));
    for (std::size_t k = 0; k < groups; ++k) {
        key = hashCombine(key, (static_cast<std::uint64_t>(static_cast<std::uint8_t>(g.chosenClass[k])) << 32) | g.chosenCount[k]);
        if (g.chosenClass[k] < 0) {
            ++next.thinnedGroups;
            next.impostorGroups.push_back(static_cast<std::uint32_t>(k));
            impostorKey = hashCombine(impostorKey, k);
        } else if (g.chosenClass[k] > 0) {
            ++next.thinnedGroups;
        }
    }
    next.active = next.thinnedGroups > 0 || next.blendSegments > 0;
    next.key = next.active ? key | 1u : 0;
    next.impostorKey = next.active && !next.impostorGroups.empty() ? hashCombine(key, impostorKey) | 1u : 0;
    if (next.active && next.key == selection_.key && selection_.active) {
        // The same strands drawn: the lists stand (the impostors may have moved).
        selection_.view = view;
        selection_.impostorKey = next.impostorKey;
        selectedWith_ = settings;
        return selection_;
    }
    if (!next.active) {
        next.drawnStrands = total;
        selection_ = std::move(next);
        selectedWith_ = settings;
        return selection_;
    }
    // Each strand's class (0xFF: not drawn), then the classes' strands in index order, in chunks
    // side by side.
    std::vector<std::uint8_t> classOf(total, 0xFF);
    parallelFor(0, groups, [&](std::size_t k) {
        if (g.chosenClass[k] < 0) return;
        const auto cls = static_cast<std::uint8_t>(g.chosenClass[k]);
        for (std::uint32_t r = 0; r < g.chosenCount[k]; ++r) classOf[g.order[g.first[k] + r]] = cls;
    }, 16);
    constexpr std::size_t kChunk = 1 << 16;
    const std::size_t chunks = (total + kChunk - 1) / kChunk;
    std::vector<std::array<std::uint32_t, kLodClassCount>> counts(chunks);
    parallelFor(0, chunks, [&](std::size_t c) {
        counts[c].fill(0);
        const std::size_t end = std::min(total, (c + 1) * kChunk);
        for (std::size_t i = c * kChunk; i < end; ++i) {
            if (classOf[i] != 0xFF) ++counts[c][classOf[i]];
        }
    }, 1);
    std::array<std::size_t, kLodClassCount> sizes{};
    for (std::size_t c = 0; c < chunks; ++c) {
        for (int k = 0; k < kLodClassCount; ++k) {
            const std::uint32_t here = counts[c][k];
            counts[c][k] = static_cast<std::uint32_t>(sizes[k]);  // (now the chunk's first)
            sizes[k] += here;
        }
    }
    for (int k = 0; k < kLodClassCount; ++k) {
        next.strands[k].resize(sizes[k]);
        next.drawnStrands += sizes[k];
    }
    // ... and each slot's segments (class 0's split by the blendable ones), chunk by chunk.
    std::vector<std::array<std::size_t, kLodSlotCount>> segments(chunks);
    parallelFor(0, chunks, [&](std::size_t c) {
        std::array<std::uint32_t, kLodClassCount> at = counts[c];
        std::array<std::size_t, kLodSlotCount>& here = segments[c];
        here.fill(0);
        const std::size_t end = std::min(total, (c + 1) * kChunk);
        for (std::size_t i = c * kChunk; i < end; ++i) {
            const std::uint8_t cls = classOf[i];
            if (cls == 0xFF) continue;
            next.strands[cls][at[cls]++] = static_cast<std::uint32_t>(i);
            const std::size_t points = s.strandPointCount(i);
            if (points < 2) continue;
            std::size_t blended = 0;
            if (cls == 0 && !blendSegment_.empty()) {
                const std::uint8_t* flags = blendSegment_.data() + s.strandBegin(i);
                for (std::size_t p = 0; p + 1 < points; ++p) blended += flags[p];
            }
            here[cls] += points - 1 - blended;
            here[kLodClassCount] += blended;
        }
    }, 1);
    for (std::size_t c = 0; c < chunks; ++c) {
        for (int slot = 0; slot < kLodSlotCount; ++slot) next.slotSegments[slot] += segments[c][slot];
    }
    for (int slot = 0; slot < kLodSlotCount; ++slot) next.drawnSegments += next.slotSegments[slot];
    // Each slot's key: its strands, its segments (class 0's: the blendable ones' flags too).
    for (int slot = 0; slot < kLodSlotCount; ++slot) {
        if (next.slotSegments[slot] == 0) continue;
        const std::vector<std::uint32_t>& list = next.strands[slot < kLodClassCount ? slot : 0];
        std::uint64_t k = hashCombine(hashCombine(groupingSerial_, static_cast<std::uint64_t>(slot)), next.slotSegments[slot]);
        k = hashCombine(k, digestOf(list.data(), list.size() * sizeof(std::uint32_t)));
        if (slot == 0 || slot == kLodClassCount) k = hashCombine(k, blendDigest);
        next.slotKeys[slot] = k | 1u;
    }
    selection_ = std::move(next);
    selectedWith_ = settings;
    impostors_.reset();
    impostorsKey_ = 0;
    return selection_;
}

namespace {

// Strand i's segments in `part` (kLodOpaque: those that do not blend, kLodBlended: those that may),
// `blend` the blendable segments by first point (empty: none).
std::size_t segmentsOf(const StrandSet& s, std::uint32_t i, int part, const std::vector<std::uint8_t>& blend) {
    const std::size_t points = s.strandPointCount(i);
    if (points < 2) return 0;
    if (blend.empty()) return part == kLodOpaque ? points - 1 : 0;
    std::size_t n = 0;
    const std::size_t begin = s.strandBegin(i);
    for (std::size_t p = 0; p + 1 < points; ++p) n += (blend[begin + p] != 0) == (part == kLodBlended) ? 1 : 0;
    return n;
}

}  // namespace

std::size_t StrandLod::segmentCount(int lodClass, int part) const {
    if (!strands_ || lodClass < 0 || lodClass >= kLodClassCount) return 0;
    if (part == kLodBlended && lodClass != 0) return 0;
    return selection_.slotSegments[part == kLodBlended ? kLodClassCount : lodClass];
}

void StrandLod::writeSegments(int lodClass, int part, std::uint32_t* pairs) const {
    if (!strands_ || lodClass < 0 || lodClass >= kLodClassCount) return;
    if (part == kLodBlended && lodClass != 0) return;
    const StrandSet& s = *strands_;
    const std::vector<std::uint8_t> none;
    const std::vector<std::uint8_t>& blend = lodClass == 0 ? blendSegment_ : none;
    const std::vector<std::uint32_t>& list = selection_.strands[lodClass];
    // In chunks side by side: each chunk's segments counted, then written from its first.
    constexpr std::size_t kChunk = 2048;
    const std::size_t chunks = (list.size() + kChunk - 1) / kChunk;
    std::vector<std::size_t> at(chunks + 1, 0);
    parallelFor(0, chunks, [&](std::size_t c) {
        std::size_t n = 0;
        const std::size_t end = std::min(list.size(), (c + 1) * kChunk);
        for (std::size_t j = c * kChunk; j < end; ++j) n += segmentsOf(s, list[j], part, blend);
        at[c + 1] = n;
    }, 1);
    for (std::size_t c = 0; c < chunks; ++c) at[c + 1] += at[c];
    parallelFor(0, chunks, [&](std::size_t c) {
        std::uint32_t* out = pairs + 2 * at[c];
        const std::size_t end = std::min(list.size(), (c + 1) * kChunk);
        for (std::size_t j = c * kChunk; j < end; ++j) {
            const std::size_t begin = s.strandBegin(list[j]);
            const auto base = static_cast<std::uint32_t>(begin);
            const std::size_t points = s.strandPointCount(list[j]);
            for (std::size_t p = 0; p + 1 < points; ++p) {
                const bool blendable = !blend.empty() && blend[begin + p] != 0;
                if (blendable != (part == kLodBlended)) continue;
                *out++ = base + static_cast<std::uint32_t>(p);
                *out++ = base + static_cast<std::uint32_t>(p + 1);
            }
        }
    }, 1);
}

std::vector<std::uint32_t> StrandLod::classSegments(std::array<std::size_t, kLodClassCount + 2>& firsts) const {
    firsts.fill(0);
    for (int k = 0; k < kLodClassCount; ++k) firsts[k + 1] = firsts[k] + 2 * segmentCount(k, kLodOpaque);
    firsts[kLodClassCount + 1] = firsts[kLodClassCount] + 2 * segmentCount(0, kLodBlended);
    std::vector<std::uint32_t> out(firsts[kLodClassCount + 1]);
    for (int k = 0; k < kLodClassCount; ++k) {
        if (firsts[k + 1] > firsts[k]) writeSegments(k, kLodOpaque, out.data() + firsts[k]);
    }
    if (firsts[kLodClassCount + 1] > firsts[kLodClassCount]) writeSegments(0, kLodBlended, out.data() + firsts[kLodClassCount]);
    return out;
}

StrandsPtr StrandLod::impostors() const {
    if (!selection_.active || selection_.impostorGroups.empty() || !strands_) return nullptr;
    if (impostors_ && impostorsKey_ == selection_.impostorKey) return impostors_;
    const Groups& g = *groups_;
    const StrandSet& s = *strands_;
    const float density = selectedWith_.density > 0.0f && selectedWith_.density < 1.0f ? selectedWith_.density : 1.0f;
    const float widthScale = selectedWith_.widthScale > 0.0f ? selectedWith_.widthScale : 1.0f;
    const std::vector<std::uint32_t>& list = selection_.impostorGroups;
    std::vector<std::size_t> counts(list.size());
    for (std::size_t j = 0; j < list.size(); ++j) counts[j] = g.axisFirst[list[j] + 1] - g.axisFirst[list[j]];
    auto out = std::make_shared<StrandSet>();
    out->resize(counts);
    std::vector<Vec3f>& positions = out->mutablePositions();
    std::vector<float>& widths = out->mutableWidths();
    std::vector<StrandId>& ids = out->mutableIds();
    std::vector<StrandRoot>& roots = out->mutableRoots();
    parallelFor(0, list.size(), [&](std::size_t j) {
        const std::uint32_t k = list[j];
        const std::size_t members = g.first[k + 1] - g.first[k];
        const double stands = static_cast<double>(members) / density;
        const std::size_t from = g.axisFirst[k], to = out->strandBegin(j);
        for (std::size_t p = 0; p < counts[j]; ++p) {
            positions[to + p] = g.axis[from + p];
            // The band's ink: the members' total width (each standing for 1 / density) spread as a
            // Gaussian of the members' spread, widened by one fiber's own (a box's variance).
            const double fiber = static_cast<double>(g.width[from + p]) * widthScale;
            const double sigma = std::sqrt(static_cast<double>(g.sigma[from + p]) * g.sigma[from + p] + fiber * fiber / 12.0);
            const double total = stands * fiber;
            const double ink = sigma > 0.0 ? sigma * lodBandInk(static_cast<float>(total / sigma)) : 0.0;
            widths[to + p] = static_cast<float>(ink / widthScale);
        }
        ids[j] = g.ids[k];
        roots[j] = s.roots()[g.order[g.first[k]]];
    }, 16);
    impostors_ = out;
    impostorsKey_ = selection_.impostorKey;
    return impostors_;
}

std::shared_ptr<const StrandColors> StrandLod::impostorColors(const StrandColors& members) const {
    const StrandsPtr axes = impostors();
    if (!axes || !strands_ || !members.strands || members.strands->strandCount() != strands_->strandCount()) return nullptr;
    // Kept while the impostors and the members' colors (by their key) are the same.
    const std::uint64_t colorsFor = members.key != 0 ? hashCombine(impostorsKey_, members.key) | 1u : 0;
    if (colorsFor != 0 && impostorColors_ && impostorColorsFor_ == colorsFor) return impostorColors_;
    const Groups& g = *groups_;
    const StrandSet& s = *strands_;
    const std::vector<std::uint32_t>& list = selection_.impostorGroups;
    auto set = std::make_shared<StrandSet>(*axes);  // (its arrays shared: the colors' channels its own)
    std::vector<float>& r = set->addPointChannel("lodColorR");
    std::vector<float>& gr = set->addPointChannel("lodColorG");
    std::vector<float>& b = set->addPointChannel("lodColorB");
    parallelFor(0, list.size(), [&](std::size_t j) {
        const std::uint32_t k = list[j];
        const std::size_t axisPoints = set->strandPointCount(j), to = set->strandBegin(j);
        const std::size_t colored = std::min<std::size_t>(g.first[k + 1] - g.first[k], kColored);
        std::vector<double> sum(axisPoints * 3, 0.0);
        std::size_t used = 0;
        for (std::size_t m = 0; m < colored; ++m) {
            const std::uint32_t i = g.order[g.first[k] + m];
            const std::size_t count = s.strandPointCount(i), begin = s.strandBegin(i);
            if (count == 0) continue;
            for (std::size_t p = 0; p < axisPoints; ++p) {
                const std::array<float, 3> c = members.at(i, begin + pointAt(p, axisPoints, count));
                sum[p * 3] += c[0];
                sum[p * 3 + 1] += c[1];
                sum[p * 3 + 2] += c[2];
            }
            ++used;
        }
        const double share = used ? 1.0 / static_cast<double>(used) : 0.0;
        for (std::size_t p = 0; p < axisPoints; ++p) {
            r[to + p] = static_cast<float>(sum[p * 3] * share);
            gr[to + p] = static_cast<float>(sum[p * 3 + 1] * share);
            b[to + p] = static_cast<float>(sum[p * 3 + 2] * share);
        }
    }, 16);
    auto colors = std::make_shared<StrandColors>();
    colors->strands = set;
    colors->channels = {set->pointChannel("lodColorR"), set->pointChannel("lodColorG"), set->pointChannel("lodColorB")};
    colors->perPoint = true;
    std::uint64_t key = hashCombine(selection_.impostorKey, members.key);
    key = hashCombine(key, digestOf(r.data(), r.size() * sizeof(float)));
    key = hashCombine(key, digestOf(gr.data(), gr.size() * sizeof(float)));
    colors->key = hashCombine(key, digestOf(b.data(), b.size() * sizeof(float))) | 1u;
    impostorColors_ = colors;
    impostorColorsFor_ = colorsFor;
    return colors;
}

}  // namespace groomlab
