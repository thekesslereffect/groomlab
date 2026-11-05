// SPDX-License-Identifier: Apache-2.0
#pragma once

// The cards' levels of detail (hero, mid, low) every host offers: one table, so a Cards node set
// to "mid" in Maya (groomlab.cards_preset) and in the standalone app's Cards panel holds the
// same params.
//
// A hair preset gives a triangle budget, the cards per clump x depth layers (hero 6 x 2 with
// flyaway cards, mid 2 x 1, low one flat card a clump), how each card is built (segments, width
// segments, curvature, ruffle, width scale), the texture variations and the atlas. A Cards node
// only feathers feed takes the feather preset instead (a card per feather: a strip of
// featherSegments segments along the shaft, with featherSpine two quads each, a column on the
// rachis and each half folded to its vane; feathers shorter than featherMinLength x the longest
// left out). A card costs 2 x segments x widthSegments triangles (hair), 2 x featherSegments, 4 x
// with the spine (a feather): hero 24, mid 12, low 4 (a V). The card count, maxCards, follows from
// the budget.

#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "groomlab/graph.h"

namespace groomlab {

struct CardLodPreset {
    std::string lod;  // "hero", "mid", "low"
    int triangles = 0;
    int cardsPerClump = 1, layers = 1, flyawayCards = 0;
    double ruffle = 0.0;
    int segments = 8, widthSegments = 2;
    double curvature = 0.0;
    int variations = 0, atlasSize = 2048;
    double widthScale = 1.0;
};

struct FeatherCardLodPreset {
    std::string lod;
    int triangles = 0;
    int featherSegments = 6;
    bool featherSpine = true;
    double featherMinLength = 0.0;
    int variations = 0, atlasSize = 2048;
};

// hero, mid, low, in that order.
const std::vector<CardLodPreset>& cardLodPresets();
const std::vector<FeatherCardLodPreset>& featherCardLodPresets();
// The preset of that name, or null.
const CardLodPreset* findCardLodPreset(const std::string& lod);
const FeatherCardLodPreset* findFeatherCardLodPreset(const std::string& lod);

// Triangles one vaned feather's card costs: 2 a segment, 4 with the spine.
int featherCardTriangles(int segments, bool spine = true);

// What Cards node `node` of a graph (its JSON, graph_io.h) cards: "feathers" (only feathers reach
// it: a node on the way grows them, a Propagate or a compound holding one; or its featherMode is
// perFeather), "hair" (no feathers reach it, or its featherMode is off or perVane) or "both" (hair
// and feathers, featherMode auto). Reads the JSON as it is (a graph being edited need not load);
// throws std::invalid_argument for text that is not a graph or a node it does not have.
std::string cardNodeKind(const std::string& graphJson, const std::string& node);
// Whether the strands reaching `node` (itself included) hold feathers (a node on the way grows
// them) and hair (a strand source reached without passing one). Throws as cardNodeKind.
struct StrandsReaching {
    bool feathers = false, hair = false;
};
StrandsReaching strandsReaching(const std::string& graphJson, const std::string& node);

// The params a preset sets on a Cards node of that kind, in a fixed order, maxCards from the
// preset's triangle budget (`triangles` overrides it): the hair preset for hair, the feather
// preset for feathers, and for both the hair preset plus the feather preset's featherSegments,
// featherSpine and featherMinLength with both budgets' cards. Throws std::invalid_argument for an
// unknown lod or kind.
std::vector<std::pair<std::string, ParamValue>> cardPresetParams(const std::string& lod, const std::string& kind,
                                                                 std::optional<int> triangles = std::nullopt);

}  // namespace groomlab
