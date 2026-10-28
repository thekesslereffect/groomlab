// SPDX-License-Identifier: Apache-2.0
#pragma once

// What the card files share: cards.cpp makes cards from strands, card_feathers.cpp from
// feathers, card_atlas.cpp shares and lays out their textures, card_bake.cpp bakes them. A
// card's fixed-resolution shape and the points on it, the strands' colors, k-means.

#include <algorithm>
#include <array>
#include <cstddef>
#include <map>
#include <vector>

#include "groomlab/cards.h"
#include "groomlab/frames.h"
#include "groomlab/math.h"
#include "groomlab/scalp_mesh.h"
#include "groomlab/strand_set.h"

namespace groomlab::detail {

constexpr std::size_t kRefStations = 33;  // the card's fixed resolution (see Card::refCenter)

// The channels a Propagate node writes (feather_ops.cpp) that cards read.
constexpr const char* kFeatherIdChannel = "featherId";  // the shaft's number in its Propagate (from its ID; -1: none)
constexpr const char* kFeatherSideChannel = "side";     // -1 left, +1 right, 0 around
constexpr const char* kShaftUChannel = "shaftU";        // where a barb grows along its shaft
constexpr const char* kIsShaftChannel = "isShaft";      // 1 for the kept shafts

// The card's stations (mesh edge loops) sampled from its fixed-resolution shape.
void sampleStations(CardMesh::Card& card);

// A row of the card's vertices at station i, left edge to right, as the stations put it (its
// base aside): across + 1 points along `side`, the middle bowed out along `normal` by the
// curvature; a spine card's left edge, spine and right edge, each edge lifted by its fold.
std::vector<Vec3f> cardRow(const CardMesh::Card& card, std::size_t i);

// The share of a card's length over which its mesh bends from its stations' rows to its base.
constexpr float kBaseBlend = 0.1f;
// A station's vertices as the mesh has them: its row (cardRow), bent toward the base near the
// root (`root`: cardRow(card, 0)).
std::vector<Vec3f> cardVertexRow(const CardMesh::Card& card, std::size_t i, const std::vector<Vec3f>& root);

// Seats a card on the scalp its strands grow from (Card::base): each vertex of its root row
// (stations sampled) moved along the scalp's normal under it to `clearance` above the skin (a
// row standing on edge moved as a whole, its lowest vertex there), then `lift` along the card's
// normal (the node's offset). Only when `root`, where the strands grow, lies on the scalp
// (within a few `clearance` of it): a card of strands rooted elsewhere (a feather held off the
// body, curves not grown from this scalp) keeps the row its stations give. `rigid` (0..1): the row
// toward moving as a whole (its lowest vertex onto the skin), so it stays parallel to the rows
// above and the first quads flat (a flattened feather card: CardSettings::featherFlatten).
void seatCard(CardMesh::Card& card, const ScalpMesh& scalp, const Vec3f& root, float clearance, float rigid = 0.0f);

// The fixed-resolution shape when there is one (cards made here), else the stations.
struct CardShape {
    const std::vector<Vec3f>& center;
    const std::vector<Vec3f>& side;
    const std::vector<Vec3f>& normal;
    const std::vector<float>& width;
    const std::vector<float>& foldRight;  // a spine card's folds (Card::spine); may be empty
    const std::vector<float>& foldLeft;
    explicit CardShape(const CardMesh::Card& c)
        : center(c.refCenter.size() >= 2 ? c.refCenter : c.center), side(c.refCenter.size() >= 2 ? c.refSide : c.side),
          normal(c.refCenter.size() >= 2 ? c.refNormal : c.normal), width(c.refCenter.size() >= 2 ? c.refWidth : c.width),
          foldRight(c.refCenter.size() >= 2 ? c.refFoldRight : c.foldRight),
          foldLeft(c.refCenter.size() >= 2 ? c.refFoldLeft : c.foldLeft) {}
};

// The card's texture-space width (Card::textureWidth); a card built by hand without one
// gets its widest station.
float textureWidthOf(const CardMesh::Card& card);

// A point in a card's space: across (0..1 edge to edge of the texture, one scale along the
// whole card: 0.5 + offset / textureWidth), along (0 root .. 1 tip; outside past the
// ends), depth off the card plane, its offset along the side in scene units, and the
// card's frame and width there. `segment` is a search hint, updated.
struct CardPoint {
    float across = 0.0f, along = 0.0f, depth = 0.0f, offset = 0.0f, width = 1.0f, textureWidth = 1.0f;
    Vec3f side, normal, forward;
};

CardPoint projectOntoCard(const CardMesh::Card& card, const Vec3f& p, int& segment);

// Linear RGB per strand, as every exporter has it (strandColors: HairColor's RGB channels,
// else melanin channels), and per point where HairColor blends toward a tip color (pointColors,
// as the strands' lit drawing shows them); the fallback for a set with neither.
struct StrandColors {
    std::vector<std::array<float, 3>> colors;
    std::vector<std::array<float, 3>> points;  // per point of the set; empty: one color a strand
    std::vector<float> iridescence;            // per point of the set (FeatherColor's channel); empty: none
    std::array<float, 3> fallback{0.3f, 0.2f, 0.12f};
    std::array<float, 3> operator()(std::size_t s) const { return s < colors.size() ? colors[s] : fallback; }
    // Strand s's color at t along it (0 root .. 1 tip).
    std::array<float, 3> at(const StrandSet& set, std::size_t s, float t) const {
        const std::size_t n = s < set.strandCount() ? set.strandPointCount(s) : 0;
        if (points.size() != set.pointCount() || n == 0) return (*this)(s);
        if (n == 1) return points[set.strandBegin(s)];
        const float f = std::clamp(t, 0.0f, 1.0f) * static_cast<float>(n - 1);
        const std::size_t i = std::min(static_cast<std::size_t>(f), n - 2);
        const float w = f - static_cast<float>(i);
        const std::array<float, 3>& a = points[set.strandBegin(s) + i];
        const std::array<float, 3>& b = points[set.strandBegin(s) + i + 1];
        return {a[0] + (b[0] - a[0]) * w, a[1] + (b[1] - a[1]) * w, a[2] + (b[2] - a[2]) * w};
    }
    // Strand s's iridescence at t along it (0 without the channel).
    float iridescenceAt(const StrandSet& set, std::size_t s, float t) const {
        const std::size_t n = s < set.strandCount() ? set.strandPointCount(s) : 0;
        if (iridescence.size() != set.pointCount() || n == 0) return 0.0f;
        if (n == 1) return iridescence[set.strandBegin(s)];
        const float f = std::clamp(t, 0.0f, 1.0f) * static_cast<float>(n - 1);
        const std::size_t i = std::min(static_cast<std::size_t>(f), n - 2);
        const float w = f - static_cast<float>(i);
        const float a = iridescence[set.strandBegin(s) + i], b = iridescence[set.strandBegin(s) + i + 1];
        return a + (b - a) * w;
    }
};

// A card's strands' mean color (linear) at kCardColorStations along them, root to tip
// (Card::colors).
std::vector<std::array<float, 3>> cardColors(const CardMesh::Card& card, const StrandSet& s, const StrandColors& colors);
// A card's strands' mean iridescence (Card::iridescence): a feather card's per vane (its barbs'
// `side`, its shaft's to both), else the same both sides; 0 without the channel.
std::array<float, 2> cardIridescence(const CardMesh::Card& card, const StrandSet& s, const StrandColors& colors);

// The tint of a vertex `along` (0 root .. 1 tip) a card: its strands' color over its texture's
// (Card::colors over Card::textureColors), per channel, within 0..kCardTintMax; 1 without them.
std::array<float, 3> cardTint(const CardMesh::Card& card, float along);

StrandColors colorsOf(const StrandSet& s, const std::array<float, 3>& fallback);

// The colors of every set the cards were made from, computed once each.
class ColorCache {
public:
    ColorCache(const std::vector<CardMesh::Card>& cards, const StrandSet* strands, const std::array<float, 3>& fallback) {
        if (strands) colors_.emplace(strands, colorsOf(*strands, fallback));
        for (const CardMesh::Card& c : cards) {
            if (c.source && !colors_.count(c.source.get())) colors_.emplace(c.source.get(), colorsOf(*c.source, fallback));
        }
    }
    const StrandColors& of(const StrandSet& s) const {
        const auto it = colors_.find(&s);
        return it != colors_.end() ? it->second : none_;
    }

private:
    std::map<const StrandSet*, StrandColors> colors_;
    StrandColors none_;
};

// The squared distance between two feature vectors.
float distance2(const std::vector<float>& a, const std::vector<float>& b);

// Frames along a card's curve, the first facing out of the scalp.
std::vector<Frame> cardFrames(const std::vector<Vec3f>& center, const ScalpMesh* scalp);

// k-means over feature vectors: the group of each (deterministic: seeded by farthest
// points from the one nearest the mean). Empty groups are renumbered away.
std::vector<int> kMeans(const std::vector<std::vector<float>>& f, std::size_t k, int iterations);

// A feather: its strands (barbs and the kept shaft) and which one is the shaft (-1: none kept).
struct Feather {
    std::vector<std::size_t> strands;
    long long shaft = -1;
};

// Strands grouped into feathers by featherId (the shaft's number in its Propagate) and
// root (the shaft's, which every barb keeps), so the feathers of several Propagate nodes merged
// together stay apart; in the order of each feather's first strand. Marks every strand with a
// featherId in `member`.
std::vector<Feather> groupFeathers(const StrandSet& s, std::vector<char>& member);

// The cards of every feather in `s` (see CardSettings::featherMode), marking the strands they
// took (every strand with a featherId, carded or not) in `member`.
std::vector<CardMesh::Card> makeFeatherCards(const StrandSet& s, const ScalpMesh* scalp, const CardSettings& settings,
                                             std::vector<char>& member);

}  // namespace groomlab::detail
