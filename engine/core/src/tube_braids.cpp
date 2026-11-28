// SPDX-License-Identifier: Apache-2.0
#include "groomlab/tube_braids.h"

#include "groomlab/guide_hierarchy.h"
#include "operators/internal.h"

namespace groomlab {

TubeBraids tubeBraids(const GuideSet& guides) {
    TubeBraids out;
    // Every tube, by ID (a guide's own first: a levelTube of the same ID is the same tube).
    std::vector<const Guide*> all;
    std::unordered_map<std::uint64_t, const Guide*> byId;
    bool marked = false;
    auto take = [&](const Guide& g) {
        if (g.id == 0 || !byId.emplace(g.id, &g).second) return;
        all.push_back(&g);
        marked = marked || g.braid;
    };
    for (const Guide& g : guides.guides()) take(g);
    if (guides.levelTubes()) {
        for (const Guide& g : *guides.levelTubes()) take(g);
    }
    if (!marked) return out;
    // Each tube's outermost marked ancestor (itself counting), walking its parents up (at most as
    // many steps as there are tubes: a broken loop ends).
    auto outermost = [&](const Guide* g) -> const Guide* {
        const Guide* found = nullptr;
        for (std::size_t steps = 0; g && steps <= all.size(); ++steps) {
            if (g->braid) found = g;
            if (g->parentId == 0) break;
            const auto up = byId.find(g->parentId);
            g = up == byId.end() ? nullptr : up->second;
        }
        return found;
    };
    std::unordered_map<const Guide*, int> index;
    std::vector<const Guide*> ofTube(all.size());
    for (std::size_t k = 0; k < all.size(); ++k) {
        ofTube[k] = outermost(all[k]);
        if (ofTube[k] == all[k]) {
            index.emplace(all[k], static_cast<int>(out.braids.size()));
            out.braids.push_back(all[k]);
        }
    }
    for (std::size_t k = 0; k < all.size(); ++k) {
        if (ofTube[k]) out.braidOf[all[k]->id] = index.at(ofTube[k]);
    }
    std::vector<std::uint64_t> ids;
    ids.reserve(out.braids.size());
    for (const Guide* g : out.braids) ids.push_back(g->id);
    out.numbers = stableGroupNumbers(ids);
    return out;
}

std::uint64_t braidTubeOf(const GuideHierarchy& hierarchy, std::uint64_t id) {
    std::uint64_t found = kNoGuide;
    const HierarchyGuide* g = hierarchy.guide(id);
    for (std::size_t steps = 0; g && steps <= hierarchy.guides().size(); ++steps) {
        if (g->braid) found = g->id;
        if (g->parent == kNoGuide) break;
        g = hierarchy.guide(g->parent);
    }
    return found;
}

}  // namespace groomlab
