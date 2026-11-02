// SPDX-License-Identifier: Apache-2.0
// The bakes: cards' maps from the strands they hold (bakeCardMaps, and bakeCardTextures: them in
// 8 bits), the groom's color on the scalp (bakeScalpTextures), and the TGA files they are written as.

#include "groomlab/cards.h"
#include "groomlab/card_maps.h"
#include "groomlab/portable_math.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <map>
#include <set>
#include <stdexcept>
#include <utility>

#include "groomlab/curve.h"
#include "groomlab/parallel.h"
#include "groomlab/random.h"
#include "groomlab/scalp_mesh.h"
#include "groomlab/strand_set.h"

#include "cards_internal.h"

namespace groomlab {

// What the card files share (cards_internal.h).
using detail::kIsShaftChannel;
using detail::CardShape;
using detail::CardPoint;
using detail::projectOntoCard;
using detail::StrandColors;
using detail::colorsOf;
using detail::ColorCache;

namespace {

// Each RGB triple of `rgb` by `m` (CardBakeSettings::colorToRec709).
void applyColorMatrix(std::vector<float>& rgb, const ColorMatrix& m) {
    parallelFor(0, rgb.size() / 3, [&](std::size_t p) {
        const float r = rgb[p * 3], g = rgb[p * 3 + 1], b = rgb[p * 3 + 2];
        for (std::size_t i = 0; i < 3; ++i) rgb[p * 3 + i] = m[i * 3] * r + m[i * 3 + 1] * g + m[i * 3 + 2] * b;
    }, 4096);
}

float linearToSrgb(float x) {
    x = std::clamp(x, 0.0f, 1.0f);
    return x <= 0.0031308f ? 12.92f * x : 1.055f * pm::pow(x, 1.0f / 2.4f) - 0.055f;
}

// Strand points per voxel, for ambient occlusion: how much hair lies further out.
class DensityGrid {
public:
    explicit DensityGrid(const std::vector<Vec3f>& points) {
        if (points.empty()) return;
        lo_ = hi_ = points.front();
        for (const Vec3f& p : points) {
            lo_ = {std::min(lo_.x, p.x), std::min(lo_.y, p.y), std::min(lo_.z, p.z)};
            hi_ = {std::max(hi_.x, p.x), std::max(hi_.y, p.y), std::max(hi_.z, p.z)};
        }
        const Vec3f extent = hi_ - lo_;
        voxel_ = std::max({extent.x, extent.y, extent.z, 1e-6f}) / 48.0f;
        for (int a = 0; a < 3; ++a) dims_[a] = std::max(1, static_cast<int>(std::ceil((a == 0 ? extent.x : a == 1 ? extent.y : extent.z) / voxel_)) + 1);
        counts_.assign(static_cast<std::size_t>(dims_[0]) * static_cast<std::size_t>(dims_[1]) * static_cast<std::size_t>(dims_[2]), 0.0f);
        for (const Vec3f& p : points) {
            const long long i = index(p);
            if (i >= 0) counts_[static_cast<std::size_t>(i)] += 1.0f;
        }
        float sum = 0.0f, occupied = 0.0f;
        for (const float c : counts_) {
            if (c > 0.0f) sum += c, occupied += 1.0f;
        }
        mean_ = occupied > 0.0f ? sum / occupied : 1.0f;
    }
    float voxel() const { return voxel_; }
    // Occlusion over a few voxels outward from p along n (0 open .. more: covered).
    float cover(const Vec3f& p, const Vec3f& n) const {
        if (counts_.empty()) return 0.0f;
        float sum = 0.0f;
        for (int step = 1; step <= 4; ++step) sum += density(p + n * (voxel_ * static_cast<float>(step)));
        return sum / (4.0f * mean_);
    }

private:
    // Trilinear, so occlusion changes smoothly from voxel to voxel.
    float density(const Vec3f& p) const {
        const float fx = (p.x - lo_.x) / voxel_ - 0.5f, fy = (p.y - lo_.y) / voxel_ - 0.5f, fz = (p.z - lo_.z) / voxel_ - 0.5f;
        const int x = static_cast<int>(std::floor(fx)), y = static_cast<int>(std::floor(fy)), z = static_cast<int>(std::floor(fz));
        const float ax = fx - static_cast<float>(x), ay = fy - static_cast<float>(y), az = fz - static_cast<float>(z);
        float sum = 0.0f;
        for (int k = 0; k < 8; ++k) {
            const int cx = x + (k & 1), cy = y + ((k >> 1) & 1), cz = z + ((k >> 2) & 1);
            if (cx < 0 || cy < 0 || cz < 0 || cx >= dims_[0] || cy >= dims_[1] || cz >= dims_[2]) continue;
            const float w = ((k & 1) ? ax : 1.0f - ax) * (((k >> 1) & 1) ? ay : 1.0f - ay) * (((k >> 2) & 1) ? az : 1.0f - az);
            sum += w * counts_[(static_cast<std::size_t>(cz) * static_cast<std::size_t>(dims_[1]) + static_cast<std::size_t>(cy)) *
                                   static_cast<std::size_t>(dims_[0]) + static_cast<std::size_t>(cx)];
        }
        return sum;
    }

    long long index(const Vec3f& p) const {
        const int x = static_cast<int>((p.x - lo_.x) / voxel_), y = static_cast<int>((p.y - lo_.y) / voxel_),
                  z = static_cast<int>((p.z - lo_.z) / voxel_);
        if (x < 0 || y < 0 || z < 0 || x >= dims_[0] || y >= dims_[1] || z >= dims_[2]) return -1;
        return (static_cast<long long>(z) * dims_[1] + y) * dims_[0] + x;
    }
    Vec3f lo_, hi_;
    float voxel_ = 1.0f, mean_ = 1.0f;
    int dims_[3] = {1, 1, 1};
    std::vector<float> counts_;
};

// Per-pixel sums while baking: the values are averaged by weight, the coverage composited.
struct BakeBuffers {
    explicit BakeBuffers(std::size_t pixels)
        : open(pixels, 1.0f), weight(pixels, 0.0f), depth(pixels, 0.0f), coordU(pixels, 0.0f), seed(pixels, 0.0f),
          ao(pixels, 0.0f), rootU(pixels, 0.0f), rootV(pixels, 0.0f), tangent(pixels * 3, 0.0f), color(pixels * 3, 0.0f),
          iridescence(pixels, 0.0f), id(pixels, 0.0f), layers(pixels, 0.0f), normal(pixels * 3, 0.0f) {}
    std::vector<float> open;  // product of (1 - coverage): light getting through
    std::vector<float> weight, depth, coordU, seed, ao, rootU, rootV, tangent, color, iridescence, id;
    std::vector<float> layers;  // the strands' coverage summed (how many lie over the texel)
    std::vector<float> normal;
};

// One strand's pixels before they are composited (a pixel it crosses twice counts once).
struct StrandStamp {
    float coverage = 0.0f, depth = 0.0f, coordU = 0.0f, ao = 0.0f, iridescence = 0.0f;
    Vec3f tangent, color;  // color: linear, the strand's there (HairColor's tip blend)
    Vec3f normal;          // the strand as a round fiber there (card space)
};

// A round fiber's normal in card space, `offset` (-1..1) of its radius across it toward `side`
// (the texture's direction across the strand): square to the strand, out of the card at its
// middle, turning to face `side` at its edge.
Vec3f fiberNormal(const Vec3f& tangent, float offset, const Vec3f& side) {
    const Vec3f out{0.0f, 0.0f, 1.0f};
    Vec3f w = out, across = side;
    const float length = tangent.length();
    if (length > 1e-6f) {
        const Vec3f t = tangent * (1.0f / length);
        const Vec3f square = out - t * out.dot(t);
        if (square.length() > 1e-4f) {
            w = square.normalized();
            across = t.cross(w).normalized();
            if (across.dot(side) < 0.0f) across = across * -1.0f;
        }
    }
    const float a = std::clamp(offset, -1.0f, 1.0f);
    return across * a + w * std::sqrt(std::max(0.0f, 1.0f - a * a));
}

}  // namespace

CardTextures bakeCardTextures(const StrandSet& strands, const CardMesh& cards, const CardBakeSettings& settings) {
    return cardTexturesFromMaps(bakeCardMaps(strands, cards, settings));
}

CardMaps bakeCardMaps(const StrandSet& strands, const CardMesh& cards, const CardBakeSettings& settings) {
    CardMaps t;
    t.size = std::max(settings.size, 8);
    const int size = t.size;
    const std::size_t pixels = static_cast<std::size_t>(size) * static_cast<std::size_t>(size);
    BakeBuffers b(pixels);

    // The textures to bake: cards with their own.
    std::vector<std::size_t> owners;
    for (std::size_t c = 0; c < cards.cards.size(); ++c) {
        if (cards.cards[c].variationOf < 0) owners.push_back(c);
    }
    auto sourceOf = [&](const CardMesh::Card& card) -> const StrandSet& { return card.source ? *card.source : strands; };

    // Ambient occlusion from all the hair the textures are baked from.
    std::vector<Vec3f> cloud;
    for (const std::size_t c : owners) {
        const StrandSet& s = sourceOf(cards.cards[c]);
        for (const std::size_t si : cards.cards[c].strands) {
            const Vec3f* pts = s.strandPoints(si);
            cloud.insert(cloud.end(), pts, pts + s.strandPointCount(si));
        }
    }
    const DensityGrid grid(settings.occlusion > 0.0f ? cloud : std::vector<Vec3f>{});
    cloud.clear();
    cloud.shrink_to_fit();
    const ColorCache colorCache(cards.cards, &strands, settings.color);
    // Whether each source set has real widths (any not 1), once per set, not per texture.
    std::map<const StrandSet*, bool> hasRealWidths;
    for (const std::size_t c : owners) {
        const StrandSet& s = sourceOf(cards.cards[c]);
        if (hasRealWidths.count(&s)) continue;
        hasRealWidths[&s] = !s.widths().empty() && std::any_of(s.widths().begin(), s.widths().end(), [](float x) { return x != 1.0f; });
    }
    // The biggest textures first (strands times samples along them), so the last ones running
    // are small: a few textures (variations) can hold most of the work. Each writes only its
    // own rectangle, so the order changes nothing in the result.
    // Cards sharing a cell (laid out from a baked atlas: applyCardAtlas) bake it once, from the
    // first of them: no two bakes side by side write the same pixels (the last to finish winning).
    std::vector<std::size_t> baking;
    std::set<std::array<float, 4>> cells;
    for (const std::size_t c : owners) {
        const float* cell = cards.cards[c].cell;
        if (cells.insert({cell[0], cell[1], cell[2], cell[3]}).second) baking.push_back(c);
    }
    std::vector<double> work(cards.cards.size(), 0.0);
    for (const std::size_t c : baking) {
        const CardMesh::Card& card = cards.cards[c];
        const float cellH = (card.cell[3] - card.cell[1]) * static_cast<float>(size);
        work[c] = static_cast<double>(card.strands.size()) * static_cast<double>(std::clamp(static_cast<int>(cellH / 2.0f), 12, 400));
    }
    std::stable_sort(baking.begin(), baking.end(), [&](std::size_t a, std::size_t c) { return work[a] > work[c]; });

    parallelFor(0, baking.size(), [&](std::size_t o) {
        const CardMesh::Card& card = cards.cards[baking[o]];
        const StrandSet& s = sourceOf(card);
        // The cell in pixels (row 0 at the top: v = 1). The cards bake in
        // parallel, each writing only inside its own rectangle, so rectangles
        // must never share a pixel: edges are exclusive and each cell gives up
        // its last column and row to the gap, which keeps cells that touch
        // (padding 0, or less than a pixel at this size) apart. A cell narrower
        // than a pixel bakes nothing.
        const int x0 = std::clamp(static_cast<int>(std::lround(card.cell[0] * size)), 0, size);
        const int x1 = std::clamp(static_cast<int>(std::lround(card.cell[2] * size)) - 1, 0, size);
        const int y0 = std::clamp(static_cast<int>(std::lround((1.0f - card.cell[3]) * size)), 0, size);
        const int y1 = std::clamp(static_cast<int>(std::lround((1.0f - card.cell[1]) * size)) - 1, 0, size);
        if (x1 <= x0 || y1 <= y0) return;
        const float cellW = (card.cell[2] - card.cell[0]) * static_cast<float>(size);
        const float cellH = (card.cell[3] - card.cell[1]) * static_cast<float>(size);
        const int w = x1 - x0, h = y1 - y0;
        std::vector<StrandStamp> stamp(static_cast<std::size_t>(w) * static_cast<std::size_t>(h));
        std::vector<int> stampOwner(stamp.size(), -1);
        std::vector<std::size_t> touched;
        const StrandColors& colors = colorCache.of(s);
        const bool realWidths = hasRealWidths.at(&s);
        // The ID map: a random value per clump (the strands' clumpId), else per card.
        const std::vector<float>* clumpIds = s.strandChannel("clumpId");
        const std::uint64_t cardKey = card.clump >= 0 ? (static_cast<std::uint64_t>(card.node) << 32) + static_cast<std::uint64_t>(card.clump)
                                                      : (card.strands.empty() ? 0 : s.ids()[card.strands.front()]);
        const float cardId = unitFloat(hashCombine(0x1D5EEDull, cardKey));
        float meanWidth = 0.0f;
        const CardShape shape(card);
        for (const float x : shape.width) meanWidth += x / static_cast<float>(shape.width.size());
        const float fallbackWidth = settings.strandWidth > 0.0f ? settings.strandWidth : meanWidth / 40.0f;
        const auto samples = static_cast<std::size_t>(std::clamp(static_cast<int>(cellH / 2.0f), 12, 400));
        // A feather's rachis (a kept shaft) reads thicker than its barbs: at least 2.5 times their
        // mean width (3 times the fallback without real widths).
        const std::vector<float>* shafts = card.feather >= 0 ? s.strandChannel(kIsShaftChannel) : nullptr;
        float barbWidth = 0.0f;
        if (shafts && realWidths) {
            float sum = 0.0f, count = 0.0f;
            for (const std::size_t si : card.strands) {
                if ((*shafts)[si] > 0.5f) continue;
                for (std::size_t p = 0; p < s.strandPointCount(si); ++p) sum += s.widths()[s.strandBegin(si) + p], count += 1.0f;
            }
            barbWidth = count > 0.0f ? sum / count : 0.0f;
        }

        for (std::size_t m = 0; m < card.strands.size(); ++m) {
            const std::size_t si = card.strands[m];
            const std::size_t n = s.strandPointCount(si);
            if (n < 2 || arcLength(s.strandPoints(si), n) <= 0.0f) continue;
            const bool rachis = shafts && (*shafts)[si] > 0.5f;
            const float strandFallback = rachis ? 3.0f * fallbackWidth : fallbackWidth;
            const std::vector<Vec3f> pts = resampleByArcLength(s.strandPoints(si), n, samples);
            const std::size_t begin = s.strandBegin(si);
            // Per sample: pixel position, radius, and the values it carries.
            struct Sample {
                float x, y, radius, depth, coordU, ao, fade, iridescence;
                Vec3f tangent, color;
                bool in;
            };
            std::vector<Sample> line(samples);
            int segment = -1;
            for (std::size_t k = 0; k < samples; ++k) {
                const float coordU = static_cast<float>(k) / static_cast<float>(samples - 1);
                const CardPoint cp = projectOntoCard(card, pts[k], segment);
                const Vec3f dir = (pts[std::min(k + 1, samples - 1)] - pts[k > 0 ? k - 1 : 0]).normalized();
                float width = strandFallback;
                if (realWidths) {
                    const float f = coordU * static_cast<float>(n - 1);
                    const std::size_t i = std::min(static_cast<std::size_t>(f), n - 2);
                    const float a = f - static_cast<float>(i);
                    width = s.widths()[begin + i] * (1.0f - a) + s.widths()[begin + i + 1] * a;
                    if (rachis) width = std::max(width, 2.5f * barbWidth);
                }
                Sample& sm = line[k];
                // Across in the texture's one frame (cp.across: offset / textureWidth), so a
                // straight strand bakes straight whatever the card's width does along it.
                sm.x = card.cell[0] * size + cp.across * cellW;
                sm.y = (1.0f - (card.cell[1] + (card.cell[3] - card.cell[1]) * cp.along)) * size;
                sm.radius = 0.5f * width / cp.textureWidth * cellW;
                sm.depth = std::clamp(cp.depth / cp.width, -0.5f, 0.5f);
                sm.coordU = coordU;
                const std::array<float, 3> rgb = colors.at(s, si, coordU);
                sm.color = {rgb[0], rgb[1], rgb[2]};
                sm.iridescence = colors.iridescenceAt(s, si, coordU);
                sm.tangent = {dir.dot(cp.side), dir.dot(cp.forward), dir.dot(cp.normal)};
                sm.ao = settings.occlusion > 0.0f ? pm::exp(-settings.occlusion * grid.cover(pts[k], cp.normal)) : 1.0f;
                sm.in = cp.along >= -0.1f && cp.along <= 1.1f;
                sm.fade = settings.rootFade > 0.0f ? smoothstep(0.0f, settings.rootFade, cp.along) : 1.0f;
            }
            // Capsules between samples, into this strand's stamp. Each stays inside its cell,
            // clear of the cell's edge rows and columns: a strand past the card's edge or ends
            // (a card narrower than its hair: widthScale under 1) is drawn along that edge
            // rather than cut off, and nothing bleeds into the neighbouring texture.
            const int self = static_cast<int>(m);
            for (std::size_t k = 0; k + 1 < samples; ++k) {
                Sample a = line[k];
                Sample c = line[k + 1];
                if (!a.in && !c.in) continue;
                // No wider than the cell lets it stay clear of the edge rows and columns from the
                // middle (a cell narrower than its hair: the strand draws thinner there).
                const float inside = 0.5f * static_cast<float>(std::min(x1 - x0, y1 - y0)) - 1.5f;
                const float r = std::min(std::max(0.5f * (a.radius + c.radius), 0.5f * settings.minStrandTexels), std::max(inside, 0.0f));
                const float keep = std::max(r, 0.5f) + 1.5f;
                for (Sample* sample : {&a, &c}) {
                    sample->x = static_cast<float>(x1 - x0) > 2.0f * keep ? std::clamp(sample->x, static_cast<float>(x0) + keep, static_cast<float>(x1) - keep)
                                                                          : 0.5f * static_cast<float>(x0 + x1);
                    sample->y = static_cast<float>(y1 - y0) > 2.0f * keep ? std::clamp(sample->y, static_cast<float>(y0) + keep, static_cast<float>(y1) - keep)
                                                                          : 0.5f * static_cast<float>(y0 + y1);
                }
                const float reach = std::max(r, 0.5f) + 1.0f;
                const int bx0 = std::max(x0, static_cast<int>(std::floor(std::min(a.x, c.x) - reach)));
                const int bx1 = std::min(x1 - 1, static_cast<int>(std::ceil(std::max(a.x, c.x) + reach)));
                const int by0 = std::max(y0, static_cast<int>(std::floor(std::min(a.y, c.y) - reach)));
                const int by1 = std::min(y1 - 1, static_cast<int>(std::ceil(std::max(a.y, c.y) + reach)));
                const float ex = c.x - a.x, ey = c.y - a.y, len2 = std::max(ex * ex + ey * ey, 1e-12f);
                // Thinner than a pixel: fainter, not thinner (the coverage a real strand would give).
                const float thin = std::min(1.0f, 2.0f * r);
                for (int y = by0; y <= by1; ++y) {
                    for (int x = bx0; x <= bx1; ++x) {
                        const float px = static_cast<float>(x) + 0.5f - a.x, py = static_cast<float>(y) + 0.5f - a.y;
                        const float tt = std::clamp((px * ex + py * ey) / len2, 0.0f, 1.0f);
                        const float dx = px - ex * tt, dy = py - ey * tt;
                        const float cover = std::clamp(std::max(r, 0.5f) + 0.5f - std::sqrt(dx * dx + dy * dy), 0.0f, 1.0f) * thin *
                                            (a.fade + (c.fade - a.fade) * tt);
                        if (cover <= 0.0f) continue;
                        const std::size_t local = static_cast<std::size_t>(y - y0) * static_cast<std::size_t>(w) + static_cast<std::size_t>(x - x0);
                        StrandStamp& st = stamp[local];
                        if (stampOwner[local] != self) {
                            stampOwner[local] = self;
                            st.coverage = 0.0f;
                            touched.push_back(local);
                        }
                        if (cover <= st.coverage) continue;
                        st.coverage = cover;
                        st.depth = a.depth + (c.depth - a.depth) * tt;
                        st.coordU = a.coordU + (c.coordU - a.coordU) * tt;
                        st.ao = a.ao + (c.ao - a.ao) * tt;
                        st.tangent = a.tangent + (c.tangent - a.tangent) * tt;
                        st.color = a.color + (c.color - a.color) * tt;
                        st.iridescence = a.iridescence + (c.iridescence - a.iridescence) * tt;
                        // Across the strand (the texture's direction square to it, and how far
                        // over, of its radius): its normal as a round fiber.
                        const float axis = std::sqrt(ex * ex + ey * ey);
                        const float offset = axis > 1e-6f ? (ex * dy - ey * dx) / (axis * std::max(r, 0.5f)) : 0.0f;
                        const Vec3f side = axis > 1e-6f ? Vec3f{-ey / axis, -ex / axis, 0.0f} : Vec3f{1.0f, 0.0f, 0.0f};
                        st.normal = fiberNormal(st.tangent, offset, side);
                    }
                }
            }
            // Composite: coverage over what's there; values weighted toward the front.
            const float seed = unitFloat(hash64(s.ids()[si]));
            const float id = clumpIds ? unitFloat(hashCombine(0xC1D5ull, static_cast<std::uint64_t>(std::llround((*clumpIds)[si])))) : cardId;
            ScalpMesh::UV root{0.0f, 0.0f};
            if (settings.scalp && settings.scalp->contains(s.roots()[si])) root = settings.scalp->uvAt(s.roots()[si]);
            for (const std::size_t local : touched) {
                const StrandStamp& st = stamp[local];
                const std::size_t p = static_cast<std::size_t>(y0 + static_cast<int>(local / static_cast<std::size_t>(w))) * static_cast<std::size_t>(size) +
                                      static_cast<std::size_t>(x0 + static_cast<int>(local % static_cast<std::size_t>(w)));
                b.open[p] *= 1.0f - st.coverage;
                const float wgt = st.coverage * pm::exp(6.0f * st.depth);
                b.weight[p] += wgt;
                b.depth[p] += st.depth * wgt;
                b.coordU[p] += st.coordU * wgt;
                b.seed[p] += seed * wgt;
                b.ao[p] += st.ao * wgt;
                b.rootU[p] += root[0] * wgt;
                b.rootV[p] += root[1] * wgt;
                b.iridescence[p] += st.iridescence * wgt;
                b.id[p] += id * wgt;
                b.layers[p] += st.coverage;
                for (int k = 0; k < 3; ++k) {
                    b.tangent[p * 3 + static_cast<std::size_t>(k)] += (k == 0 ? st.tangent.x : k == 1 ? st.tangent.y : st.tangent.z) * wgt;
                    b.color[p * 3 + static_cast<std::size_t>(k)] += (k == 0 ? st.color.x : k == 1 ? st.color.y : st.color.z) * wgt;
                    b.normal[p * 3 + static_cast<std::size_t>(k)] += (k == 0 ? st.normal.x : k == 1 ? st.normal.y : st.normal.z) * wgt;
                }
            }
            touched.clear();
        }
    }, 1);

    // Pixel loops run by rows, a few rows per task (every pixel its own: the same in any order).
    constexpr std::size_t kRowGrain = 16;
    const std::size_t rowPixels = static_cast<std::size_t>(size);
    // Averages; then colors spread a few pixels past the strands, so mip maps and
    // filtering at the alpha's edges don't pull in black.
    std::vector<char> filled(pixels, 0);
    parallelFor(0, rowPixels, [&](std::size_t y) {
        for (std::size_t p = y * rowPixels; p < (y + 1) * rowPixels; ++p) {
            const float wsum = b.weight[p];
            if (wsum <= 0.0f) continue;
            filled[p] = 1;
            for (std::vector<float>* v : {&b.depth, &b.coordU, &b.seed, &b.ao, &b.rootU, &b.rootV, &b.iridescence, &b.id}) (*v)[p] /= wsum;
            for (int k = 0; k < 3; ++k) {
                b.tangent[p * 3 + static_cast<std::size_t>(k)] /= wsum;
                b.color[p * 3 + static_cast<std::size_t>(k)] /= wsum;
                b.normal[p * 3 + static_cast<std::size_t>(k)] /= wsum;
            }
        }
    }, kRowGrain);
    const int rings = settings.dilate < 0 ? 2 : settings.dilate;
    for (int pass = 0; pass < rings; ++pass) {
        std::vector<char> next = filled;
        parallelFor(0, static_cast<std::size_t>(size), [&](std::size_t yy) {
            const int y = static_cast<int>(yy);
            for (int x = 0; x < size; ++x) {
                const std::size_t p = static_cast<std::size_t>(y) * static_cast<std::size_t>(size) + static_cast<std::size_t>(x);
                if (filled[p]) continue;
                float count = 0.0f;
                float acc[17] = {};
                for (int dy = -1; dy <= 1; ++dy) {
                    for (int dx = -1; dx <= 1; ++dx) {
                        const int nx = x + dx, ny = y + dy;
                        if ((dx == 0 && dy == 0) || nx < 0 || ny < 0 || nx >= size || ny >= size) continue;
                        const std::size_t q = static_cast<std::size_t>(ny) * static_cast<std::size_t>(size) + static_cast<std::size_t>(nx);
                        if (!filled[q]) continue;
                        const float vals[17] = {b.depth[q], b.coordU[q], b.seed[q], b.ao[q], b.rootU[q], b.rootV[q],
                                                b.tangent[q * 3], b.tangent[q * 3 + 1], b.tangent[q * 3 + 2],
                                                b.color[q * 3], b.color[q * 3 + 1], b.color[q * 3 + 2], b.iridescence[q],
                                                b.id[q], b.normal[q * 3], b.normal[q * 3 + 1], b.normal[q * 3 + 2]};
                        for (int k = 0; k < 17; ++k) acc[k] += vals[k];
                        count += 1.0f;
                    }
                }
                if (count == 0.0f) continue;
                for (float& a : acc) a /= count;
                b.depth[p] = acc[0], b.coordU[p] = acc[1], b.seed[p] = acc[2], b.ao[p] = acc[3];
                b.rootU[p] = acc[4], b.rootV[p] = acc[5], b.iridescence[p] = acc[12], b.id[p] = acc[13];
                for (int k = 0; k < 3; ++k) {
                    b.tangent[p * 3 + static_cast<std::size_t>(k)] = acc[6 + k];
                    b.color[p * 3 + static_cast<std::size_t>(k)] = acc[9 + k];
                    b.normal[p * 3 + static_cast<std::size_t>(k)] = acc[14 + k];
                }
                next[p] = 1;
            }
        }, kRowGrain);
        filled.swap(next);
    }
    if (settings.dilate < 0) {
        // All the way: every empty texel the values of its nearest filled one (a flood from all of
        // them at once, 8-connected: each card's region grows until it meets its neighbours').
        std::vector<std::int32_t> from(pixels, -1);
        std::vector<std::int32_t> front;
        for (std::size_t p = 0; p < pixels; ++p)
            if (filled[p]) {
                from[p] = static_cast<std::int32_t>(p);
                front.push_back(static_cast<std::int32_t>(p));
            }
        std::vector<std::int32_t> next;
        while (!front.empty() && front.size() < pixels) {
            next.clear();
            for (const std::int32_t p : front) {
                const int x = p % size, y = p / size;
                for (int dy = -1; dy <= 1; ++dy)
                    for (int dx = -1; dx <= 1; ++dx) {
                        const int nx = x + dx, ny = y + dy;
                        if ((dx == 0 && dy == 0) || nx < 0 || ny < 0 || nx >= size || ny >= size) continue;
                        const std::int32_t q = ny * size + nx;
                        if (from[static_cast<std::size_t>(q)] >= 0) continue;
                        from[static_cast<std::size_t>(q)] = from[static_cast<std::size_t>(p)];
                        next.push_back(q);
                    }
            }
            front.swap(next);
        }
        parallelFor(0, rowPixels, [&](std::size_t y) {
            for (std::size_t p = y * rowPixels; p < (y + 1) * rowPixels; ++p) {
                if (filled[p] || from[p] < 0) continue;
                const std::size_t q = static_cast<std::size_t>(from[p]);
                for (std::vector<float>* v : {&b.depth, &b.coordU, &b.seed, &b.ao, &b.rootU, &b.rootV, &b.iridescence, &b.id})
                    (*v)[p] = (*v)[q];
                for (int k = 0; k < 3; ++k) {
                    b.tangent[p * 3 + static_cast<std::size_t>(k)] = b.tangent[q * 3 + static_cast<std::size_t>(k)];
                    b.color[p * 3 + static_cast<std::size_t>(k)] = b.color[q * 3 + static_cast<std::size_t>(k)];
                    b.normal[p * 3 + static_cast<std::size_t>(k)] = b.normal[q * 3 + static_cast<std::size_t>(k)];
                }
            }
        }, kRowGrain);
        for (std::size_t p = 0; p < pixels; ++p)
            if (from[p] >= 0) filled[p] = 1;
    }

    // The iridescence only when a strand has any.
    t.iridescent = std::any_of(owners.begin(), owners.end(), [&](std::size_t c) {
        return !colorCache.of(sourceOf(cards.cards[c])).iridescence.empty();
    });
    // The final values, in place (each texel its own: any order), then the buffers handed over.
    parallelFor(0, rowPixels, [&](std::size_t y) {
        for (std::size_t p = y * rowPixels; p < (y + 1) * rowPixels; ++p) {
            const bool any = filled[p] != 0;
            b.open[p] = 1.0f - b.open[p];  // the coverage
            b.depth[p] = any ? 0.5f + b.depth[p] : 0.5f;
            b.ao[p] = any ? b.ao[p] : 1.0f;
            Vec3f tan = any ? Vec3f{b.tangent[p * 3], b.tangent[p * 3 + 1], b.tangent[p * 3 + 2]} : Vec3f{0, 1, 0};
            tan = tan.length() > 1e-6f ? tan.normalized() : Vec3f{0, 1, 0};
            b.tangent[p * 3 + 0] = tan.x, b.tangent[p * 3 + 1] = tan.y, b.tangent[p * 3 + 2] = tan.z;
            Vec3f n = any ? Vec3f{b.normal[p * 3], b.normal[p * 3 + 1], b.normal[p * 3 + 2]} : Vec3f{0, 0, 1};
            n = n.length() > 1e-6f ? n.normalized() : Vec3f{0, 0, 1};
            b.normal[p * 3 + 0] = n.x, b.normal[p * 3 + 1] = n.y, b.normal[p * 3 + 2] = n.z;
            if (!any) {
                for (int k = 0; k < 3; ++k) b.color[p * 3 + static_cast<std::size_t>(k)] = settings.color[static_cast<std::size_t>(k)];
                b.rootU[p] = 0.0f, b.rootV[p] = 0.0f, b.seed[p] = 0.0f, b.coordU[p] = 0.0f, b.id[p] = 0.0f, b.iridescence[p] = 0.0f;
            }
            if (!t.iridescent) b.iridescence[p] = 0.0f;
        }
    }, kRowGrain);
    t.coverage = std::move(b.open);
    t.depth = std::move(b.depth);
    t.ao = std::move(b.ao);
    t.rootU = std::move(b.rootU);
    t.rootV = std::move(b.rootV);
    t.seed = std::move(b.seed);
    t.root = std::move(b.coordU);
    t.id = std::move(b.id);
    t.iridescence = std::move(b.iridescence);
    t.layers = std::move(b.layers);
    t.tangent = std::move(b.tangent);
    t.normal = std::move(b.normal);
    t.color = std::move(b.color);
    if (settings.colorToRec709 != kIdentityColorMatrix) applyColorMatrix(t.color, settings.colorToRec709);
    return t;
}

namespace {

// Fills pixels without weight from coarser averages of the ones with it
// (push-pull), so a color spreads smoothly over every gap and seam.
void pushPullFill(std::vector<float>& rgb, std::vector<float>& weight, int size) {
    struct Level {
        int size;
        std::vector<float> rgb, weight;
    };
    std::vector<Level> levels{{size, rgb, weight}};
    while (levels.back().size > 1) {
        const Level& fine = levels.back();
        Level coarse{(fine.size + 1) / 2, {}, {}};
        coarse.rgb.assign(static_cast<std::size_t>(coarse.size) * coarse.size * 3, 0.0f);
        coarse.weight.assign(static_cast<std::size_t>(coarse.size) * coarse.size, 0.0f);
        for (int y = 0; y < fine.size; ++y) {
            for (int x = 0; x < fine.size; ++x) {
                const std::size_t f = static_cast<std::size_t>(y) * fine.size + x;
                const float w = std::min(fine.weight[f], 1.0f);
                if (w <= 0.0f) continue;
                const std::size_t c = static_cast<std::size_t>(y / 2) * coarse.size + x / 2;
                for (int k = 0; k < 3; ++k) coarse.rgb[c * 3 + k] += fine.rgb[f * 3 + k] * w;
                coarse.weight[c] += w;
            }
        }
        for (std::size_t c = 0; c < coarse.weight.size(); ++c) {
            if (coarse.weight[c] > 0.0f) {
                for (int k = 0; k < 3; ++k) coarse.rgb[c * 3 + k] /= coarse.weight[c];
            }
        }
        levels.push_back(std::move(coarse));
    }
    // Pull: from the coarsest down, empty pixels take their parent's color.
    for (std::size_t l = levels.size() - 1; l-- > 0;) {
        Level& fine = levels[l];
        const Level& coarse = levels[l + 1];
        for (int y = 0; y < fine.size; ++y) {
            for (int x = 0; x < fine.size; ++x) {
                const std::size_t f = static_cast<std::size_t>(y) * fine.size + x;
                if (fine.weight[f] > 0.0f) continue;
                const std::size_t c = static_cast<std::size_t>(y / 2) * coarse.size + x / 2;
                if (coarse.weight[c] <= 0.0f) continue;
                for (int k = 0; k < 3; ++k) fine.rgb[f * 3 + k] = coarse.rgb[c * 3 + k];
                fine.weight[f] = 1e-6f;  // filled (not covered)
            }
        }
    }
    rgb = std::move(levels.front().rgb);
    weight = std::move(levels.front().weight);
}

}  // namespace

ScalpTextures bakeScalpTextures(const StrandSet& strands, const ScalpMesh& scalp, const ScalpBakeSettings& settings) {
    if (!scalp.hasUVs()) throw std::invalid_argument("the scalp has no UVs to bake the groom into");
    ScalpTextures t;
    t.size = std::max(settings.size, 8);
    const int size = t.size;
    const std::size_t pixels = static_cast<std::size_t>(size) * static_cast<std::size_t>(size);
    Vec3f lo = scalp.points().empty() ? Vec3f{} : scalp.points().front(), hi = lo;
    for (const Vec3f& p : scalp.points()) {
        lo = {std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z)};
        hi = {std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z)};
    }
    const float extent = std::max((hi - lo).length(), 1e-6f);
    const float reach = settings.reach > 0.0f ? settings.reach : 0.03f * extent;
    // Pixels per scene unit on each triangle (UV area against world area).
    const auto& uvs = scalp.cornerUVs();
    std::vector<float> pxPerUnit(scalp.triangleCount(), 0.0f);
    for (std::size_t tri = 0; tri < scalp.triangleCount(); ++tri) {
        const auto& a = uvs[tri * 3], &b = uvs[tri * 3 + 1], &c = uvs[tri * 3 + 2];
        const float uvArea = 0.5f * std::abs((b[0] - a[0]) * (c[1] - a[1]) - (c[0] - a[0]) * (b[1] - a[1]));
        const auto& tv = scalp.triangles()[tri];
        const Vec3f e1 = scalp.points()[static_cast<std::size_t>(tv[1])] - scalp.points()[static_cast<std::size_t>(tv[0])];
        const Vec3f e2 = scalp.points()[static_cast<std::size_t>(tv[2])] - scalp.points()[static_cast<std::size_t>(tv[0])];
        const float area = 0.5f * e1.cross(e2).length();
        pxPerUnit[tri] = area > 0.0f ? std::sqrt(uvArea / area) * static_cast<float>(size) : 0.0f;
    }
    const bool realWidths = !strands.widths().empty() &&
                            std::any_of(strands.widths().begin(), strands.widths().end(), [](float x) { return x != 1.0f; });
    const float fallbackWidth = settings.strandWidth > 0.0f ? settings.strandWidth : extent / 3000.0f;
    const StrandColors colors = colorsOf(strands, settings.color);  // base color: the scalp map's color/albedo contract

    // Each strand's stretch near the skin, in UV pixels: (x, y, radius, height 0 skin .. 1 reach).
    struct Dot {
        float x, y, radius, height;
    };
    std::vector<std::vector<Dot>> near(strands.strandCount());
    // Hair standing up covers the skin by its side, not its footprint: each strand also
    // marks a disc at its root as big as the side it shows within reach (width x length).
    std::vector<float> rootDisc(strands.strandCount(), 0.0f);
    parallelFor(0, strands.strandCount(), [&](std::size_t s) {
        const std::size_t n = strands.strandPointCount(s);
        if (n < 2) return;
        const Vec3f* pts = strands.strandPoints(s);
        const float total = arcLength(pts, n);
        if (!(total > 0.0f)) return;
        // Samples a few pixels apart along the first stretch of the strand.
        const std::size_t samples = std::clamp<std::size_t>(static_cast<std::size_t>(total / (0.25f * reach)) + 2, 8, 400);
        const std::vector<Vec3f> dense = resampleByArcLength(pts, n, samples);
        const std::size_t begin = strands.strandBegin(s);
        for (std::size_t k = 0; k < samples; ++k) {
            const SurfacePoint sp = scalp.closestPoint(dense[k], 2.0f * reach);
            if (sp.root.face < 0) break;
            const float height = (dense[k] - sp.position).length() / reach;
            if (height > 1.0f) break;  // off the skin: the rest of the strand is the cards' job
            const ScalpMesh::UV uv = scalp.uvAt(sp.root);
            float width = fallbackWidth;
            if (realWidths) {
                const float f = static_cast<float>(k) / static_cast<float>(samples - 1) * static_cast<float>(n - 1);
                const std::size_t i = std::min(static_cast<std::size_t>(f), n - 2);
                const float a = f - static_cast<float>(i);
                width = strands.widths()[begin + i] * (1.0f - a) + strands.widths()[begin + i + 1] * a;
            }
            near[s].push_back({uv[0] * size, (1.0f - uv[1]) * size,
                               0.5f * width * pxPerUnit[static_cast<std::size_t>(sp.root.face)], height});
            if (k > 0) {
                const float step = (dense[k] - dense[k - 1]).length();
                rootDisc[s] += width * step * pxPerUnit[static_cast<std::size_t>(sp.root.face)] *
                               pxPerUnit[static_cast<std::size_t>(sp.root.face)];
            }
        }
        rootDisc[s] = std::sqrt(0.5f * rootDisc[s] / 3.14159265f);  // radius, pixels, of half that area
    });

    std::vector<float> open(pixels, 1.0f), weight(pixels, 0.0f), rgb(pixels * 3, 0.0f);
    std::vector<float> stamp(pixels, 0.0f), stampHeight(pixels, 0.0f);
    std::vector<int> owner(pixels, -1);
    std::vector<std::size_t> touched;
    for (std::size_t s = 0; s < near.size(); ++s) {
        const auto& dots = near[s];
        // The root disc: a segment of no length at the root.
        const std::size_t segments = dots.empty() ? 0 : dots.size();
        for (std::size_t k = 0; k < segments; ++k) {
            const bool disc = k + 1 == segments;
            if (disc && !(rootDisc[s] > dots[0].radius)) continue;
            const Dot& a = disc ? dots[0] : dots[k];
            const Dot& c = disc ? dots[0] : dots[k + 1];
            if (std::abs(a.x - c.x) + std::abs(a.y - c.y) > 0.05f * size) continue;  // across a UV seam
            const float r = disc ? rootDisc[s] : 0.5f * (a.radius + c.radius);
            const float reachPx = std::max(r, 0.5f) + 1.0f;
            const int bx0 = std::max(0, static_cast<int>(std::floor(std::min(a.x, c.x) - reachPx)));
            const int bx1 = std::min(size - 1, static_cast<int>(std::ceil(std::max(a.x, c.x) + reachPx)));
            const int by0 = std::max(0, static_cast<int>(std::floor(std::min(a.y, c.y) - reachPx)));
            const int by1 = std::min(size - 1, static_cast<int>(std::ceil(std::max(a.y, c.y) + reachPx)));
            const float ex = c.x - a.x, ey = c.y - a.y, len2 = std::max(ex * ex + ey * ey, 1e-12f);
            const float thin = std::min(1.0f, 2.0f * r);
            for (int y = by0; y <= by1; ++y) {
                for (int x = bx0; x <= bx1; ++x) {
                    const float px = static_cast<float>(x) + 0.5f - a.x, py = static_cast<float>(y) + 0.5f - a.y;
                    const float tt = std::clamp((px * ex + py * ey) / len2, 0.0f, 1.0f);
                    const float dx = px - ex * tt, dy = py - ey * tt;
                    const float cover = std::clamp(std::max(r, 0.5f) + 0.5f - std::sqrt(dx * dx + dy * dy), 0.0f, 1.0f) * thin;
                    if (cover <= 0.0f) continue;
                    const std::size_t p = static_cast<std::size_t>(y) * static_cast<std::size_t>(size) + static_cast<std::size_t>(x);
                    if (owner[p] != static_cast<int>(s)) {
                        owner[p] = static_cast<int>(s);
                        stamp[p] = 0.0f;
                        touched.push_back(p);
                    }
                    if (cover > stamp[p]) {
                        stamp[p] = cover;
                        stampHeight[p] = a.height + (c.height - a.height) * tt;
                    }
                }
            }
        }
        const auto color = colors.at(strands, s, 0.0f);  // at the root: what lies on the skin
        for (const std::size_t p : touched) {
            open[p] *= 1.0f - stamp[p];
            const float w = stamp[p] * pm::exp(3.0f * stampHeight[p]);  // hair further out shows on top
            weight[p] += w;
            for (int k = 0; k < 3; ++k) rgb[p * 3 + static_cast<std::size_t>(k)] += color[static_cast<std::size_t>(k)] * w;
        }
        touched.clear();
    }
    for (std::size_t p = 0; p < pixels; ++p) {
        if (weight[p] > 0.0f) {
            for (int k = 0; k < 3; ++k) rgb[p * 3 + static_cast<std::size_t>(k)] /= weight[p];
        }
    }
    if (settings.fill) pushPullFill(rgb, weight, size);
    // Coverage softened over about the spacing between roots: a base of hair, not a dot per root.
    float uvArea = 0.0f;
    for (std::size_t tri = 0; tri < scalp.triangleCount(); ++tri) {
        const auto& a = uvs[tri * 3], &b = uvs[tri * 3 + 1], &c = uvs[tri * 3 + 2];
        uvArea += 0.5f * std::abs((b[0] - a[0]) * (c[1] - a[1]) - (c[0] - a[0]) * (b[1] - a[1]));
    }
    const int soften = std::clamp(static_cast<int>(0.5f * static_cast<float>(size) *
                                                   std::sqrt(uvArea / static_cast<float>(std::max<std::size_t>(strands.strandCount(), 1)))),
                                  0, size / 16);
    if (soften > 0) {
        std::vector<float> cover(pixels), tmp(pixels);
        for (std::size_t p = 0; p < pixels; ++p) cover[p] = 1.0f - open[p];
        for (int pass = 0; pass < 2; ++pass) {  // two box blurs, each way: close to a Gaussian
            parallelFor(0, static_cast<std::size_t>(size), [&](std::size_t y) {
                float sum = 0.0f;
                const std::size_t row = y * static_cast<std::size_t>(size);
                for (int x = -soften; x < size + soften; ++x) {
                    if (x + soften < size) sum += cover[row + static_cast<std::size_t>(std::max(0, x + soften))] * (x + soften >= 0 ? 1.0f : 0.0f);
                    if (x - soften - 1 >= 0) sum -= cover[row + static_cast<std::size_t>(x - soften - 1)];
                    if (x >= 0 && x < size) tmp[row + static_cast<std::size_t>(x)] = sum / static_cast<float>(2 * soften + 1);
                }
            }, 16);
            parallelFor(0, static_cast<std::size_t>(size), [&](std::size_t x) {
                float sum = 0.0f;
                for (int y = -soften; y < size + soften; ++y) {
                    if (y + soften < size && y + soften >= 0) sum += tmp[static_cast<std::size_t>(y + soften) * static_cast<std::size_t>(size) + x];
                    if (y - soften - 1 >= 0) sum -= tmp[static_cast<std::size_t>(y - soften - 1) * static_cast<std::size_t>(size) + x];
                    if (y >= 0 && y < size) cover[static_cast<std::size_t>(y) * static_cast<std::size_t>(size) + x] = sum / static_cast<float>(2 * soften + 1);
                }
            }, 16);
        }
        for (std::size_t p = 0; p < pixels; ++p) open[p] = 1.0f - cover[p];
    }

    auto to8 = [](float x) { return static_cast<std::uint8_t>(std::lround(std::clamp(x, 0.0f, 1.0f) * 255.0f)); };
    std::array<float, 3> fallback = settings.color;
    if (settings.colorToRec709 != kIdentityColorMatrix) {
        applyColorMatrix(rgb, settings.colorToRec709);
        std::vector<float> f(fallback.begin(), fallback.end());
        applyColorMatrix(f, settings.colorToRec709);
        fallback = {f[0], f[1], f[2]};
    }
    t.color.resize(pixels * 3);
    t.coverage.resize(pixels);
    const std::size_t rowPixels = static_cast<std::size_t>(size);
    parallelFor(0, rowPixels, [&](std::size_t y) {
        for (std::size_t p = y * rowPixels; p < (y + 1) * rowPixels; ++p) {
            t.coverage[p] = to8(1.0f - open[p]);
            for (int k = 0; k < 3; ++k) {
                const float c = weight[p] > 0.0f ? rgb[p * 3 + static_cast<std::size_t>(k)] : fallback[static_cast<std::size_t>(k)];
                t.color[p * 3 + static_cast<std::size_t>(k)] = to8(linearToSrgb(c));
            }
        }
    }, 16);
    return t;
}

void writeTga(const std::string& path, int width, int height, int channels, const std::vector<std::uint8_t>& pixels) {
    if (channels != 1 && channels != 3 && channels != 4) throw std::runtime_error("writeTga: 1, 3 or 4 channels");
    if (pixels.size() != static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * static_cast<std::size_t>(channels)) {
        throw std::runtime_error("writeTga: pixel count does not match the size");
    }
    std::ofstream out(path, std::ios::binary);
    if (!out) throw std::runtime_error("cannot write '" + path + "'");
    std::uint8_t header[18] = {};
    header[2] = channels == 1 ? 3 : 2;  // uncompressed grayscale / true color
    header[12] = static_cast<std::uint8_t>(width & 0xFF);
    header[13] = static_cast<std::uint8_t>(width >> 8);
    header[14] = static_cast<std::uint8_t>(height & 0xFF);
    header[15] = static_cast<std::uint8_t>(height >> 8);
    header[16] = static_cast<std::uint8_t>(8 * channels);
    header[17] = static_cast<std::uint8_t>(0x20 | (channels == 4 ? 8 : 0));  // rows top to bottom; alpha bits
    out.write(reinterpret_cast<const char*>(header), sizeof header);
    if (channels == 1) {
        out.write(reinterpret_cast<const char*>(pixels.data()), static_cast<std::streamsize>(pixels.size()));
    } else {
        std::vector<std::uint8_t> bgr(pixels.size());
        const auto step = static_cast<std::size_t>(channels);
        for (std::size_t i = 0; i < pixels.size(); i += step) {
            bgr[i] = pixels[i + 2];
            bgr[i + 1] = pixels[i + 1];
            bgr[i + 2] = pixels[i];
            if (channels == 4) bgr[i + 3] = pixels[i + 3];
        }
        out.write(reinterpret_cast<const char*>(bgr.data()), static_cast<std::streamsize>(bgr.size()));
    }
    if (!out) throw std::runtime_error("cannot write '" + path + "'");
}

}  // namespace groomlab
