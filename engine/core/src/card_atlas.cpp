// SPDX-License-Identifier: Apache-2.0
// Cards' textures: alike cards sharing one (variations), the layers of several Cards nodes
// combined, the atlas the textures are laid out in and the cells a bake keeps (CardAtlas).

#include "groomlab/cards.h"
#include "groomlab/portable_math.h"

#include <algorithm>
#include <cmath>
#include <nlohmann/json.hpp>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <unordered_map>
#include <utility>

#include "groomlab/curve.h"
#include "groomlab/parallel.h"
#include "groomlab/random.h"
#include "groomlab/strand_set.h"

#include "cards_internal.h"

namespace groomlab {

// What the card files share (cards_internal.h).
using detail::CardShape;
using detail::textureWidthOf;
using detail::CardPoint;
using detail::projectOntoCard;
using detail::StrandColors;
using detail::ColorCache;
using detail::distance2;
using detail::kMeans;
using detail::packRects;

namespace {

// What a card's texture looks like, for grouping alike cards: its size, where
// its strands lie on it, their root-to-tip flow, and its color.
std::vector<float> describeCard(const CardMesh::Card& card, const StrandSet& s, const StrandColors& colors, bool mirrored = false) {
    constexpr int kAlong = 6, kAcross = 4, kSamples = 12;
    std::vector<float> f;
    const CardShape shape(card);
    const float length = std::max(arcLength(shape.center), 1e-6f);
    // The texture's scale across: its width (the widest station), which every station
    // maps into at the same scale.
    const float width = std::max(textureWidthOf(card), 1e-6f);
    f.push_back(2.0f * pm::log(length));
    f.push_back(2.0f * pm::log(width));
    f.push_back(20.0f * card.tubeArc);
    std::vector<float> hist(kAlong * kAcross, 0.0f);
    std::array<float, kAlong> sum{}, sum2{}, count{}, slope{}, slopeCount{};
    const std::size_t stride = std::max<std::size_t>(1, card.strands.size() / 48);
    float total = 0.0f;
    std::array<float, 3> color{0, 0, 0};
    float colored = 0.0f;
    for (std::size_t m = 0; m < card.strands.size(); m += stride) {
        const std::size_t si = card.strands[m];
        const auto pts = resampleByArcLength(s.strandPoints(si), s.strandPointCount(si), kSamples);
        int segment = -1;
        float previousAcross = 0.0f, previousAlong = 0.0f;
        bool previousValid = false;
        for (const Vec3f& p : pts) {
            const CardPoint cp = projectOntoCard(card, p, segment);
            if (cp.along < 0.0f || cp.along > 1.0f || cp.across < 0.0f || cp.across > 1.0f) continue;
            const float across = mirrored ? 1.0f - cp.across : cp.across;
            const int a = std::min(kAlong - 1, static_cast<int>(cp.along * kAlong));
            const int b = std::min(kAcross - 1, static_cast<int>(across * kAcross));
            hist[static_cast<std::size_t>(a * kAcross + b)] += 1.0f;
            sum[static_cast<std::size_t>(a)] += across;
            sum2[static_cast<std::size_t>(a)] += across * across;
            count[static_cast<std::size_t>(a)] += 1.0f;
            if (previousValid && cp.along > previousAlong + 1e-6f) {
                slope[static_cast<std::size_t>(a)] += std::clamp((across - previousAcross) / (cp.along - previousAlong), -4.0f, 4.0f);
                slopeCount[static_cast<std::size_t>(a)] += 1.0f;
            }
            previousAcross = across;
            previousAlong = cp.along;
            previousValid = true;
            total += 1.0f;
        }
        const auto c = colors(si);
        for (int k = 0; k < 3; ++k) color[static_cast<std::size_t>(k)] += c[static_cast<std::size_t>(k)];
        colored += 1.0f;
    }
    for (const float h : hist) f.push_back(3.0f * (total > 0 ? h / total : 0.0f) * std::sqrt(static_cast<float>(kAlong * kAcross)));
    // Mean position, spread and direction in each root-to-tip band. These keep a straight card
    // from sharing a visibly zigzagging or side-drifting bake merely because its coarse occupancy
    // histogram and dimensions happen to match.
    for (int a = 0; a < kAlong; ++a) {
        const std::size_t i = static_cast<std::size_t>(a);
        const float mean = count[i] > 0.0f ? sum[i] / count[i] : 0.5f;
        const float variance = count[i] > 0.0f ? std::max(0.0f, sum2[i] / count[i] - mean * mean) : 0.0f;
        f.push_back(5.0f * (mean - 0.5f));
        f.push_back(4.0f * std::sqrt(variance));
        f.push_back(2.0f * (slopeCount[i] > 0.0f ? slope[i] / slopeCount[i] : 0.0f));
    }
    for (const float c : color) f.push_back(4.0f * (colored > 0 ? c / colored : 0.0f));
    return f;
}


}  // namespace

namespace detail {

// Rectangles (w, h) packed into the unit square as large as they fit, with
// `pad` around each: shelves of the tallest first. Returns x0, y0, x1, y1 per rect.
std::vector<std::array<float, 4>> packRects(const std::vector<std::array<float, 2>>& sizes, float pad) {
    const std::size_t n = sizes.size();
    std::vector<std::array<float, 4>> cells(n);
    if (n == 0) return cells;
    std::vector<std::size_t> order(n);
    for (std::size_t i = 0; i < n; ++i) order[i] = i;
    std::sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
        return sizes[a][1] != sizes[b][1] ? sizes[a][1] > sizes[b][1] : a < b;
    });
    // Skyline, bottom-left: each rect (tallest first) goes where it sits lowest, so short ones
    // fill in beside tall ones instead of wasting whole rows.
    struct Span {
        float x, width, top;
    };
    auto fits = [&](float scale, bool place) {
        std::vector<Span> skyline{{0.0f, 1.0f, 0.0f}};
        for (const std::size_t i : order) {
            const float w = sizes[i][0] * scale + pad, h = sizes[i][1] * scale + pad;  // with the gap after it
            if (w + pad > 1.0f || h + pad > 1.0f) return false;
            float bestY = std::numeric_limits<float>::max(), bestX = 0.0f;
            for (std::size_t s = 0; s < skyline.size(); ++s) {
                const float x = skyline[s].x;
                if (x + w > 1.0f - pad + 1e-7f) break;
                float y = 0.0f;
                for (std::size_t t = s; t < skyline.size() && skyline[t].x < x + w - 1e-7f; ++t) y = std::max(y, skyline[t].top);
                if (y < bestY - 1e-7f) bestY = y, bestX = x;
            }
            if (bestY + h + pad > 1.0f) return false;
            if (place) {
                const float x0 = pad + bestX, y0 = pad + bestY;
                cells[i] = {x0, y0, x0 + sizes[i][0] * scale, y0 + sizes[i][1] * scale};
            }
            // The skyline under [bestX, bestX + w) rises to the rect's top.
            std::vector<Span> next;
            const float x1 = bestX + w;
            for (const Span& span : skyline) {
                const float s0 = span.x, s1 = span.x + span.width;
                if (s1 <= bestX + 1e-7f || s0 >= x1 - 1e-7f) {
                    next.push_back(span);
                    continue;
                }
                if (s0 < bestX) next.push_back({s0, bestX - s0, span.top});
                if (s1 > x1) next.push_back({x1, s1 - x1, span.top});
            }
            next.push_back({bestX, w, bestY + h});
            std::sort(next.begin(), next.end(), [](const Span& a, const Span& b) { return a.x < b.x; });
            skyline.clear();
            for (const Span& span : next) {  // neighbours at the same height become one
                if (!skyline.empty() && std::abs(skyline.back().top - span.top) < 1e-7f) {
                    skyline.back().width += span.width;
                } else {
                    skyline.push_back(span);
                }
            }
        }
        return true;
    };
    float lo = 0.0f, hi = 1.0f;
    while (fits(hi, false) && hi < 1e12f) hi *= 2.0f;
    for (int it = 0; it < 48; ++it) {
        const float mid = 0.5f * (lo + hi);
        (fits(mid, false) ? lo : hi) = mid;
    }
    if (!fits(lo, true)) {  // too many for the padding: squeeze them in a grid
        const int columns = static_cast<int>(std::ceil(std::sqrt(static_cast<float>(n))));
        for (std::size_t i = 0; i < n; ++i) {
            const float c = static_cast<float>(i % static_cast<std::size_t>(columns)), r = static_cast<float>(i / static_cast<std::size_t>(columns));
            cells[i] = {c / columns, r / columns, (c + 1.0f) / columns, (r + 1.0f) / columns};
        }
    }
    return cells;
}

}  // namespace detail

void shareCardTextures(std::vector<CardMesh::Card>& cards, const StrandSet* strands, int variations, bool mirror) {
    const std::size_t n = cards.size();
    for (CardMesh::Card& c : cards) c.variationOf = -1, c.mirrored = false;
    if (variations <= 0 || n <= static_cast<std::size_t>(variations)) return;
    std::vector<std::vector<float>> features(n), mirroredFeatures(n);
    const ColorCache colors(cards, strands, {0, 0, 0});
    parallelFor(0, n, [&](std::size_t i) {
        const StrandSet* s = cards[i].source ? cards[i].source.get() : strands;
        features[i] = s ? describeCard(cards[i], *s, colors.of(*s)) : std::vector<float>{0.0f};
        mirroredFeatures[i] = s ? describeCard(cards[i], *s, colors.of(*s), true) : features[i];
    });
    const std::size_t dims = features[0].size();
    const std::vector<int> labels = kMeans(features, static_cast<std::size_t>(variations), 30);
    const std::size_t k = static_cast<std::size_t>(*std::max_element(labels.begin(), labels.end())) + 1;
    std::vector<std::size_t> group(labels.begin(), labels.end());
    // Each group's mean feature.
    std::vector<std::vector<float>> centers(k, std::vector<float>(dims, 0.0f));
    {
        std::vector<float> counts(k, 0.0f);
        for (std::size_t i = 0; i < n; ++i) {
            for (std::size_t d = 0; d < dims; ++d) centers[group[i]][d] += features[i][d];
            counts[group[i]] += 1.0f;
        }
        for (std::size_t c = 0; c < k; ++c) {
            for (std::size_t d = 0; d < dims; ++d) centers[c][d] /= counts[c];  // every group has a member
        }
    }
    // Each group shows its most typical card's texture; the others alternate mirrored.
    std::vector<int> representative(k, -1);
    for (std::size_t i = 0; i < n; ++i) {
        const std::size_t g = group[i];
        if (representative[g] < 0 ||
            distance2(features[i], centers[g]) < distance2(features[static_cast<std::size_t>(representative[g])], centers[g])) {
            representative[g] = static_cast<int>(i);
        }
    }
    for (std::size_t i = 0; i < n; ++i) {
        const int rep = representative[group[i]];
        if (rep == static_cast<int>(i)) continue;
        // A tube's texture is its braid unrolled, a flat card's its strands across it: neither
        // shows the other's (the features below keep them apart; this never lets one through).
        if ((cards[i].tubeArc > 0.0f) != (cards[static_cast<std::size_t>(rep)].tubeArc > 0.0f) ||
            std::abs(cards[i].tubeArc - cards[static_cast<std::size_t>(rep)].tubeArc) > 1e-3f) {
            continue;
        }
        const float direct = distance2(features[i], features[static_cast<std::size_t>(rep)]);
        const float reversed = distance2(features[i], mirroredFeatures[static_cast<std::size_t>(rep)]);
        cards[i].variationOf = rep;
        // Mirror only when the owner's mirrored strand flow is genuinely a closer match. Blindly
        // alternating receivers turns a compatible curved bake into the opposite curve.
        cards[i].mirrored = mirror && reversed < direct;
    }
}

CardMesh combineCardLayers(const std::vector<std::shared_ptr<const CardMesh>>& layers) {
    CardMesh out;
    bool first = true;
    for (std::size_t l = 0; l < layers.size(); ++l) {
        const auto& layer = layers[l];
        if (!layer) continue;
        const int offset = static_cast<int>(out.cards.size());
        for (CardMesh::Card card : layer->cards) {
            if (card.variationOf >= 0) card.variationOf += offset;
            // Its node by name in the combined list (a layer of an unnamed node: by the layer).
            std::string name = cardNodeOf(*layer, card);
            if (name.empty()) name = "#" + std::to_string(l);
            auto it = std::find(out.nodes.begin(), out.nodes.end(), name);
            if (it == out.nodes.end()) it = out.nodes.insert(out.nodes.end(), std::move(name));
            card.node = static_cast<int>(it - out.nodes.begin());
            out.cards.push_back(std::move(card));
        }
        out.padding = first ? layer->padding : std::min(out.padding, layer->padding);
        out.atlasSize = std::max(out.atlasSize, layer->atlasSize);
        out.thinnestStrand = first ? layer->thinnestStrand : std::max(out.thinnestStrand, layer->thinnestStrand);
        out.dilate = first ? layer->dilate : (out.dilate && layer->dilate);
        first = false;
    }
    return out;
}

const std::string& cardNodeOf(const CardMesh& mesh, const CardMesh::Card& card) {
    static const std::string none;
    return card.node >= 0 && static_cast<std::size_t>(card.node) < mesh.nodes.size()
               ? mesh.nodes[static_cast<std::size_t>(card.node)]
               : none;
}

std::vector<CardNodeSummary> summarizeCardNodes(const CardMesh& mesh, const StrandSet* strands) {
    std::vector<CardNodeSummary> out(std::max<std::size_t>(mesh.nodes.size(), 1));
    for (std::size_t n = 0; n < mesh.nodes.size(); ++n) out[n].node = mesh.nodes[n];
    std::vector<std::vector<const CardMesh::Card*>> members(out.size());
    for (const CardMesh::Card& card : mesh.cards) {
        const std::size_t n = card.node >= 0 && static_cast<std::size_t>(card.node) < mesh.nodes.size()
                                  ? static_cast<std::size_t>(card.node)
                                  : 0;
        members[n].push_back(&card);
    }
    std::map<const StrandSet*, std::uint64_t> sourceHashes;  // each source hashed once
    for (std::size_t n = 0; n < out.size(); ++n) {
        CardNodeSummary& summary = out[n];
        std::uint64_t h = hashString(hash64(0xBA4Eu), summary.node);
        std::vector<const StrandSet*> sources;  // the strands its cards are made of, in order
        for (const CardMesh::Card* card : members[n]) {
            const StrandSet* source = card->source ? card->source.get() : strands;
            if (source && std::find(sources.begin(), sources.end(), source) == sources.end()) sources.push_back(source);
        }
        for (const StrandSet* source : sources) {
            auto it = sourceHashes.find(source);
            if (it == sourceHashes.end()) it = sourceHashes.emplace(source, source->contentHash()).first;
            h = hashCombine(h, it->second);
        }
        h = hashCombine(h, members[n].size());
        std::set<int> feathers;
        for (const CardMesh::Card* card : members[n]) {
            if (card->feather >= 0) feathers.insert(card->feather);
            const auto columns = static_cast<std::size_t>(std::max(card->across, 1) + 1);
            const std::size_t first = card->firstVertex;
            const std::size_t end = std::min(mesh.positions.size(), first + columns * static_cast<std::size_t>(card->stations));
            for (std::size_t v = first; v < end; ++v) {
                const Vec3f& p = mesh.positions[v];
                h = hashFloat(hashFloat(hashFloat(h, p.x), p.y), p.z);
            }
            summary.triangles += 2 * (columns - 1) * static_cast<std::size_t>(std::max(card->stations - 1, 0));
            if (card->variationOf < 0) ++summary.textures;
        }
        summary.cards = members[n].size();
        summary.feathers = feathers.size();
        summary.bakeHash = h ? h : 1;
    }
    return out;
}

void buildCardVertices(CardMesh& mesh, bool holdInCell);

void layoutCards(CardMesh& mesh, float padding) {
    mesh.positions.clear();
    mesh.normals.clear();
    mesh.u.clear();
    mesh.v.clear();
    mesh.quads.clear();
    mesh.padding = padding;
    const std::size_t n = mesh.cards.size();
    // Textures: cards with their own, sized by the card in the world (even texel density).
    std::vector<std::size_t> owners;
    std::vector<std::array<float, 2>> sizes;
    for (std::size_t c = 0; c < n; ++c) {
        CardMesh::Card& card = mesh.cards[c];
        if (card.variationOf >= static_cast<int>(n) || card.variationOf == static_cast<int>(c) ||
            (card.variationOf >= 0 && mesh.cards[static_cast<std::size_t>(card.variationOf)].variationOf >= 0)) {
            card.variationOf = -1;  // not a texture owner: its own after all
        }
        if (card.variationOf >= 0) continue;
        const CardShape shape(card);  // not the stations: the layout doesn't change with the segments
        owners.push_back(c);
        sizes.push_back({std::max(textureWidthOf(card), 1e-6f), std::max(arcLength(shape.center), 1e-6f)});
    }
    const auto cells = packRects(sizes, std::max(padding, 0.0f));
    for (std::size_t o = 0; o < owners.size(); ++o) {
        std::copy(cells[o].begin(), cells[o].end(), mesh.cards[owners[o]].cell);
    }
    for (std::size_t c = 0; c < n; ++c) {
        CardMesh::Card& card = mesh.cards[c];
        // Its texture is baked from its own strands, or from the card's it shares: their colors.
        const CardMesh::Card& owner = card.variationOf < 0 ? card : mesh.cards[static_cast<std::size_t>(card.variationOf)];
        card.textureColors = owner.colors;
        card.textureRefWidth = owner.refWidth;
        card.textureWidth = textureWidthOf(owner);
        if (card.variationOf < 0) continue;
        std::copy(std::begin(owner.cell), std::end(owner.cell), card.cell);
    }
    buildCardVertices(mesh, false);
}

void layoutCardsUnique(CardMesh& mesh, float padding) {
    for (CardMesh::Card& card : mesh.cards) {
        card.variationOf = -1;
        card.mirrored = false;
    }
    layoutCards(mesh, padding);
}

// The vertices, normals, UVs and quads of laid-out cards (their cells set). With `holdInCell`
// the UVs never leave the card's cell (a card grown wider than the texture it was baked with
// shows the texture's edge rather than its neighbour's).
void buildCardVertices(CardMesh& mesh, bool holdInCell) {
    mesh.positions.clear();
    mesh.normals.clear();
    mesh.u.clear();
    mesh.v.clear();
    mesh.quads.clear();
    mesh.tints.clear();
    mesh.iridescence.clear();
    const bool iridescent = std::any_of(mesh.cards.begin(), mesh.cards.end(), [](const CardMesh::Card& c) {
        return c.iridescence[0] > 0.0f || c.iridescence[1] > 0.0f;
    });
    // The iridescence at `x` across a card (-1 its left edge .. 1 its right).
    auto iridescenceAcross = [](const CardMesh::Card& card, float x) {
        const float w = std::clamp(0.5f * (x + 1.0f), 0.0f, 1.0f);
        return card.iridescence[0] + (card.iridescence[1] - card.iridescence[0]) * w;
    };
    for (CardMesh::Card& card : mesh.cards) {
        // Mirrored: u runs the other way across the cell.
        const float u0 = card.mirrored ? card.cell[2] : card.cell[0], u1 = card.mirrored ? card.cell[0] : card.cell[2];
        card.firstVertex = static_cast<std::uint32_t>(mesh.positions.size());
        const int across = std::max(card.across, 1);
        const auto columnsAcross = static_cast<std::uint32_t>(across + 1);
        // UVs keep a regular row/column grid. Their U span follows the final physical width, so
        // Width Profile samples the matching part of the bake instead of stretching it across
        // every tapered row.
        const float textureWidth = textureWidthOf(card);
        const float hairGridWidth = card.width.empty() ? textureWidth
                                                       : std::max(*std::max_element(card.width.begin(), card.width.end()), 1e-9f);
        // A shared feather is made from its owner's outline scaled to this feather's length.
        // Normalize by that final scaled envelope, never by the owner's unscaled scene width;
        // otherwise long receivers widen past their atlas cell and sample neighbouring feathers.
        const float featherGridWidth = card.width.empty() ? textureWidth
                                                          : std::max(*std::max_element(card.width.begin(), card.width.end()), 1e-9f);
        const bool spine = card.spine && across == 2;
        // Seated (Card::base): the rows near the root bend from where the stations put them to
        // the base, all the way at the root row, none from a tenth of the card on.
        const std::vector<Vec3f> root = detail::cardRow(card, 0);
        std::vector<std::vector<Vec3f>> rows(static_cast<std::size_t>(card.stations));
        for (int station = 0; station < card.stations; ++station) {
            rows[static_cast<std::size_t>(station)] = detail::cardVertexRow(card, static_cast<std::size_t>(station), root);
        }
        auto stationSample = [&](float at) {
            const float wanted = std::clamp(at, 0.0f, 1.0f);
            if (card.stationAlong.size() == static_cast<std::size_t>(card.stations)) {
                const auto upper = std::upper_bound(card.stationAlong.begin(), card.stationAlong.end(), wanted);
                const std::size_t b = std::clamp(static_cast<std::size_t>(upper - card.stationAlong.begin()), std::size_t{1},
                                                 card.stationAlong.size() - 1);
                const std::size_t a = b - 1;
                const float span = card.stationAlong[b] - card.stationAlong[a];
                const float t = span > 1e-9f ? (wanted - card.stationAlong[a]) / span : 0.0f;
                return std::make_pair(a, std::clamp(t, 0.0f, 1.0f));
            }
            const float station = wanted * static_cast<float>(card.stations - 1);
            const std::size_t a = std::min(static_cast<std::size_t>(station), static_cast<std::size_t>(card.stations - 2));
            return std::make_pair(a, station - static_cast<float>(a));
        };
        auto sampleEdge = [&](float at, int edge) {
            const auto [a, t] = stationSample(at);
            return rows[a][static_cast<std::size_t>(edge)] * (1.0f - t) + rows[a + 1][static_cast<std::size_t>(edge)] * t;
        };
        auto sampleWidth = [&](float at) {
            const auto [a, t] = stationSample(at);
            return card.width[a] * (1.0f - t) + card.width[a + 1] * t;
        };
        // Feather outer-edge stations follow the measured barb direction, but remain ordered.
        // The previous independent clamp let several rows land on the tip, collapsing the last
        // quads. Root and tip are fixed, the correction eases out at both, and every edge segment
        // retains at least a quarter of its ordinary station spacing.
        auto featherEdgeAlong = [&](const std::vector<float>& shifts) {
            std::vector<float> edge(static_cast<std::size_t>(card.stations));
            for (int station = 0; station < card.stations; ++station) {
                const float at = card.stationAlong.size() == static_cast<std::size_t>(card.stations)
                                     ? card.stationAlong[static_cast<std::size_t>(station)]
                                     : static_cast<float>(station) / static_cast<float>(card.stations - 1);
                const float shift = static_cast<std::size_t>(station) < shifts.size() ? shifts[static_cast<std::size_t>(station)] : 0.0f;
                edge[static_cast<std::size_t>(station)] = std::clamp(at + shift, 0.0f, 1.0f);
            }
            edge.front() = 0.0f;
            edge.back() = 1.0f;
            const float absoluteMinStep = 0.3f / static_cast<float>(card.stations - 1);
            for (std::size_t station = 1; station + 1 < edge.size(); ++station) {
                const float baseStep = card.stationAlong.size() == edge.size()
                                           ? card.stationAlong[station] - card.stationAlong[station - 1]
                                           : 1.0f / static_cast<float>(card.stations - 1);
                edge[station] = std::max(edge[station], edge[station - 1] + std::max(0.25f * baseStep, absoluteMinStep));
            }
            for (std::size_t station = edge.size() - 1; station-- > 1;) {
                const float baseStep = card.stationAlong.size() == edge.size()
                                           ? card.stationAlong[station + 1] - card.stationAlong[station]
                                           : 1.0f / static_cast<float>(card.stations - 1);
                edge[station] = std::min(edge[station], edge[station + 1] - std::max(0.25f * baseStep, absoluteMinStep));
            }
            return edge;
        };
        const std::vector<float> featherEdgeAlongLeft =
            card.featherEdgeAlongLeft.size() == static_cast<std::size_t>(card.stations)
                ? card.featherEdgeAlongLeft
                : featherEdgeAlong(card.featherBarbShiftLeft);
        const std::vector<float> featherEdgeAlongRight =
            card.featherEdgeAlongRight.size() == static_cast<std::size_t>(card.stations)
                ? card.featherEdgeAlongRight
                : featherEdgeAlong(card.featherBarbShiftRight);
        for (int k = 0; k < card.stations; ++k) {
            const std::size_t i = static_cast<std::size_t>(k);
            const float along = card.stationAlong.size() == static_cast<std::size_t>(card.stations)
                                    ? card.stationAlong[i]
                                    : static_cast<float>(k) / static_cast<float>(card.stations - 1);
            std::vector<Vec3f> points = rows[i];
            const std::array<float, 3> tint = detail::cardTint(card, along);
            if (spine) {
                // Left edge, the spine on the shaft, right edge; each edge lifted along the normal
                // by its half's fold (Card::spine), at the flat card's texture coordinates.
                const float halfW = 0.5f * card.width[i];
                const float at = std::clamp(card.spineOffset, -halfW, halfW);
                const float foldR = i < card.foldRight.size() ? card.foldRight[i] : 0.0f;
                const float foldL = i < card.foldLeft.size() ? card.foldLeft[i] : 0.0f;
                const Vec3f& side = card.side[i];
                const Vec3f& normal = card.normal[i];
                const Vec3f normalR = (normal - side * foldR).normalized(), normalL = (normal + side * foldL).normalized();
                const float offsets[3] = {-halfW, at, halfW};
                const Vec3f normals[3] = {normalL, (normalL + normalR).normalized(), normalR};
                // A feather's barbs leave the shaft diagonally. Move each outer vertex to the
                // matching place along the existing edge and give it that V; the center column
                // remains on the shaft station. Topology and texture flow then follow the barbs.
                const float edgeAlong[3] = {featherEdgeAlongLeft[i], along, featherEdgeAlongRight[i]};
                points[0] = sampleEdge(edgeAlong[0], 0);
                points[2] = sampleEdge(edgeAlong[2], 2);
                for (int e = 0; e < 3; ++e) {
                    mesh.positions.push_back(points[static_cast<std::size_t>(e)]);
                    mesh.normals.push_back(normals[e]);
                    const float edgeHalfW = 0.5f * sampleWidth(edgeAlong[e]);
                    const float textureOffset = e == 0 ? -edgeHalfW : e == 2 ? edgeHalfW : offsets[e];
                    float acrossTexture = 0.5f + textureOffset / featherGridWidth;
                    if (holdInCell) acrossTexture = std::clamp(acrossTexture, 0.0f, 1.0f);
                    mesh.u.push_back(u0 + (u1 - u0) * acrossTexture);
                    mesh.v.push_back(card.flipAlong ? card.cell[3] - (card.cell[3] - card.cell[1]) * edgeAlong[e]
                                                    : card.cell[1] + (card.cell[3] - card.cell[1]) * edgeAlong[e]);
                    mesh.tints.push_back(tint);
                    if (iridescent) mesh.iridescence.push_back(iridescenceAcross(card, static_cast<float>(e - 1)));
                }
            }
            for (int e = 0; e <= across && !spine && card.tubeArc > 0.0f; ++e) {
                // A tube: around its center line, the texture's u the way around (a closed tube's
                // last column on its first, at the cell's other edge: the seam), each vertex's
                // normal straight out of it.
                const float around = static_cast<float>(e) / static_cast<float>(across);
                const float t = card.tubeArc * (around - 0.5f);
                mesh.positions.push_back(points[static_cast<std::size_t>(e)]);
                mesh.normals.push_back(detail::tubePoint(card.normal[i], card.side[i], 0.5f * card.width[i], card.tubeAspect, t).outward);
                // A closed tube's seam held a texel and a half inside its cell's edges: the bake
                // leaves the cell's last column to the gap and nothing covers past its edge, so
                // filtering there would cut a slit down the tube.
                const float texel = 1.0f / static_cast<float>(mesh.atlasSize > 0 ? mesh.atlasSize : 2048);
                const float inset = card.tubeArc >= 6.2831f ? std::min(1.5f * texel, 0.1f * std::abs(u1 - u0)) : 0.0f;
                const float a0 = u0 + (u1 > u0 ? inset : -inset), a1 = u1 - (u1 > u0 ? inset : -inset);
                mesh.u.push_back(a0 + (a1 - a0) * around);
                mesh.v.push_back(card.flipAlong ? card.cell[3] - (card.cell[3] - card.cell[1]) * along
                                             : card.cell[1] + (card.cell[3] - card.cell[1]) * along);
                mesh.tints.push_back(tint);
                if (iridescent) mesh.iridescence.push_back(iridescenceAcross(card, 2.0f * around - 1.0f));
            }
            for (int e = 0; e <= across && !spine && card.tubeArc <= 0.0f; ++e) {
                const float x = 2.0f * static_cast<float>(e) / static_cast<float>(across) - 1.0f;
                mesh.positions.push_back(points[static_cast<std::size_t>(e)]);
                // The bowed surface's normal: tilt by its slope across.
                const float slope = -2.0f * card.curvature * x;
                mesh.normals.push_back(card.shading.size() == card.center.size()
                                           ? card.shading[i]
                                           : (card.normal[i] - card.side[i] * slope).normalized());
                // Normalize hair by this card's own widest final row, not by a shared texture
                // owner's scene-space width. Otherwise a wider receiver expands past its atlas
                // cell even when its root profile was never changed.
                const float uvFrameWidth = card.feather < 0 ? hairGridWidth : featherGridWidth;
                float acrossTexture = 0.5f + x * (0.5f * card.width[i]) / uvFrameWidth;
                if (holdInCell) acrossTexture = std::clamp(acrossTexture, 0.0f, 1.0f);
                mesh.u.push_back(u0 + (u1 - u0) * acrossTexture);
                mesh.v.push_back(card.flipAlong ? card.cell[3] - (card.cell[3] - card.cell[1]) * along
                                             : card.cell[1] + (card.cell[3] - card.cell[1]) * along);
                mesh.tints.push_back(tint);
                if (iridescent) mesh.iridescence.push_back(iridescenceAcross(card, x));
            }
            if (k > 0) {
                const std::uint32_t row0 = card.firstVertex + static_cast<std::uint32_t>(k - 1) * columnsAcross;
                const std::uint32_t row1 = row0 + columnsAcross;
                for (std::uint32_t e = 0; e < static_cast<std::uint32_t>(across); ++e) {
                    mesh.quads.insert(mesh.quads.end(), {row0 + e, row0 + e + 1, row1 + e + 1, row1 + e});
                }
            }
        }
    }
}

namespace {

std::vector<std::uint64_t> cardStrandIds(const CardMesh::Card& card, const StrandSet* strands) {
    const StrandSet* s = card.source ? card.source.get() : strands;
    std::vector<std::uint64_t> ids;
    if (!s) return ids;
    ids.reserve(card.strands.size());
    for (const std::size_t si : card.strands) {
        if (si < s->strandCount()) ids.push_back(s->ids()[si]);
    }
    return ids;
}

}  // namespace

CardAtlas captureCardAtlas(const CardMesh& mesh, const StrandSet* strands) {
    CardAtlas atlas;
    atlas.entries.reserve(mesh.cards.size());
    for (const CardMesh::Card& card : mesh.cards) {
        CardAtlas::Entry entry;
        entry.node = cardNodeOf(mesh, card);
        entry.strands = cardStrandIds(card, strands);
        std::copy(std::begin(card.cell), std::end(card.cell), entry.cell);
        entry.mirrored = card.mirrored;
        entry.textureWidth = textureWidthOf(card);
        entry.textureRefWidth = card.textureRefWidth.empty() ? card.refWidth : card.textureRefWidth;
        entry.flyaway = card.flyaway;
        entry.feather = card.feather >= 0;
        entry.colors = card.textureColors.empty() ? card.colors : card.textureColors;  // what the cell shows
        atlas.entries.push_back(std::move(entry));
    }
    return atlas;
}

std::size_t applyCardAtlas(CardMesh& mesh, const CardAtlas& atlas, const StrandSet* strands) {
    if (atlas.empty() || mesh.cards.empty()) return 0;
    // Per Cards node (the entries of the card's own node; a node the bake never saw: all of
    // them): which entries hold each strand, and the entries of each kind.
    struct Pool {
        std::unordered_map<std::uint64_t, std::vector<std::uint32_t>> byStrand;
        std::vector<std::uint32_t> flyawayEntries, cardEntries, featherEntries;
        void add(const CardAtlas::Entry& entry, std::uint32_t e) {
            for (const std::uint64_t id : entry.strands) byStrand[id].push_back(e);
            (entry.feather ? featherEntries : entry.flyaway ? flyawayEntries : cardEntries).push_back(e);
        }
    };
    std::map<std::string, Pool> byNode;
    Pool everything;
    for (std::size_t e = 0; e < atlas.entries.size(); ++e) {
        byNode[atlas.entries[e].node].add(atlas.entries[e], static_cast<std::uint32_t>(e));
        everything.add(atlas.entries[e], static_cast<std::uint32_t>(e));
    }
    std::size_t matched = 0;
    std::vector<char> taken(atlas.entries.size(), 0);  // entries a card matched by its strands
    std::unordered_map<std::uint32_t, std::uint32_t> shared;
    for (CardMesh::Card& card : mesh.cards) {
        const auto own = byNode.find(cardNodeOf(mesh, card));
        const Pool& pool = own != byNode.end() ? own->second : everything;
        const auto& byStrand = pool.byStrand;
        const std::vector<std::uint32_t>& flyawayEntries = pool.flyawayEntries;
        const std::vector<std::uint32_t>& cardEntries = pool.cardEntries;
        const std::vector<std::uint64_t> ids = cardStrandIds(card, strands);
        shared.clear();
        std::uint64_t hash = hash64(0x47A5u);
        for (const std::uint64_t id : ids) {
            hash = hashCombine(hash, id);
            auto it = byStrand.find(id);
            if (it == byStrand.end()) continue;
            for (const std::uint32_t e : it->second) ++shared[e];
        }
        std::size_t best = SIZE_MAX;
        std::uint32_t most = 0;
        bool bestTaken = false;
        for (const auto& [e, count] : shared) {
            // Ties (the cards of a cross hold the same strands): an entry no card took yet, then
            // the lower one, so the pick doesn't depend on the map's order.
            const bool isTaken = taken[e] != 0;
            if (count > most || (count == most && (bestTaken > isTaken || (bestTaken == isTaken && e < best)))) {
                most = count, best = e, bestTaken = isTaken;
            }
        }
        if (best != SIZE_MAX) {
            ++matched;
            taken[best] = 1;
        } else {
            // Nothing in common with any baked card (new strands, a new flyaway, a new
            // feather): a baked card of the same kind, the same one every time for these strands.
            const std::vector<std::uint32_t>* kind = &pool.featherEntries;
            if (card.feather < 0 || kind->empty()) {
                kind = card.flyaway && !flyawayEntries.empty() ? &flyawayEntries
                       : !cardEntries.empty()                   ? &cardEntries
                       : !flyawayEntries.empty()                ? &flyawayEntries
                                                                : &pool.featherEntries;
            }
            best = (*kind)[static_cast<std::size_t>(hash % kind->size())];
        }
        const CardAtlas::Entry& entry = atlas.entries[best];
        std::copy(std::begin(entry.cell), std::end(entry.cell), card.cell);
        card.mirrored = entry.mirrored;
        if (entry.textureWidth > 0.0f) card.textureWidth = entry.textureWidth;
        card.textureRefWidth = entry.textureRefWidth;
        // Tinted from the colors the cell was baked with (none recorded: its own, no tint).
        card.textureColors = entry.colors.size() == card.colors.size() ? entry.colors : card.colors;
        card.variationOf = -1;  // the cell is its own now (from the bake), not another card's
    }
    buildCardVertices(mesh, true);
    return matched;
}

std::string CardAtlas::toJson() const {
    nlohmann::json out;
    out["version"] = 1;
    nlohmann::json cards = nlohmann::json::array();
    for (const Entry& entry : entries) {
        nlohmann::json e;
        e["node"] = entry.node;
        e["strands"] = entry.strands;
        e["cell"] = {entry.cell[0], entry.cell[1], entry.cell[2], entry.cell[3]};
        e["mirrored"] = entry.mirrored;
        e["textureWidth"] = entry.textureWidth;
        if (!entry.textureRefWidth.empty()) e["textureRefWidth"] = entry.textureRefWidth;
        e["flyaway"] = entry.flyaway;
        e["feather"] = entry.feather;
        if (!entry.colors.empty()) e["colors"] = entry.colors;
        cards.push_back(std::move(e));
    }
    out["cards"] = std::move(cards);
    return out.dump();
}

CardAtlas CardAtlas::fromJson(const std::string& json) {
    CardAtlas atlas;
    nlohmann::json in;
    try {
        in = nlohmann::json::parse(json);
    } catch (const std::exception& e) {
        throw std::runtime_error(std::string("card atlas: ") + e.what());
    }
    if (!in.is_object() || !in.contains("cards") || !in["cards"].is_array()) {
        throw std::runtime_error("card atlas: no cards");
    }
    for (const nlohmann::json& e : in["cards"]) {
        Entry entry;
        entry.node = e.value("node", std::string());
        if (e.contains("strands")) entry.strands = e["strands"].get<std::vector<std::uint64_t>>();
        if (e.contains("cell") && e["cell"].is_array() && e["cell"].size() == 4) {
            for (std::size_t k = 0; k < 4; ++k) entry.cell[k] = e["cell"][k].get<float>();
        }
        entry.mirrored = e.value("mirrored", false);
        entry.textureWidth = e.value("textureWidth", 0.0f);
        if (e.contains("textureRefWidth") && e["textureRefWidth"].is_array()) {
            entry.textureRefWidth = e["textureRefWidth"].get<std::vector<float>>();
        }
        entry.flyaway = e.value("flyaway", false);
        entry.feather = e.value("feather", false);
        if (e.contains("colors") && e["colors"].is_array()) entry.colors = e["colors"].get<std::vector<std::array<float, 3>>>();
        atlas.entries.push_back(std::move(entry));
    }
    return atlas;
}

}  // namespace groomlab
