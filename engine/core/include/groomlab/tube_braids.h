// SPDX-License-Identifier: Apache-2.0
#pragma once

// Root tubes made braids (issue 129): some of a groom's tubes are plain hair, some are braided.
//
// A tube of any level of the tube hierarchy can be marked a braid (HierarchyGuide::braid, the Tube
// tool's Make Braid; Guide::braid on the guides a Guides node gives, and on their levelTubes). A
// marked tube and every tube under it make one braid:
//
//   - its path is the marked tube's guide, its hair every strand Interpolate's fillTubes gives to
//     that tube or to one under it (channel tubeBraid: the braid's number);
//   - above it nothing changes: shaping a tube carries the tubes inside it, a braid among them, and
//     the rest of that tube's hair stays as it is;
//   - the outermost marked tube of a lineage wins (hair is braided once): a mark under another
//     marked tube is covered, and makes a braid of its own once the outer one is unmarked;
//   - each braid is numbered from its marked tube's ID (stableGroupNumbers), so adding, removing or
//     marking other tubes renumbers none of the others (a Sculpt Sections' strokes stay on theirs).
//
// tubeBraids works this out from the guides alone, the same way for Interpolate (which writes
// tubeBraid) and for a Braid node joining by tube (which braids each path's strands by it): fed the
// same guides, both number the braids alike.

#include <cstdint>
#include <unordered_map>
#include <vector>

#include "groomlab/guide_set.h"

namespace groomlab {

class GuideHierarchy;

struct TubeBraids {
    // The braids' marked tubes, outermost marks only, in the guides' order (the set's guides, then
    // its levelTubes), each once; they point into the GuideSet given.
    std::vector<const Guide*> braids;
    // Per braid: its number (from its tube's ID, as braidId and tubeBraid carry it).
    std::vector<float> numbers;
    // Every tube in a braid (its marked tube and every tube under it), by ID: the braid's index.
    std::unordered_map<std::uint64_t, int> braidOf;

    bool empty() const { return braids.empty(); }
    // The braid tube `id` lies in (an index into braids), -1: none.
    int of(std::uint64_t id) const {
        const auto it = braidOf.find(id);
        return it == braidOf.end() ? -1 : it->second;
    }
};

// The braids among the tubes these guides carry (their guides' and their levelTubes').
TubeBraids tubeBraids(const GuideSet& guides);

// In a hierarchy: the marked tube whose braid guide `id` lies in (the outermost marked one of its
// lineage, itself when it is), kNoGuide (0) when none is. A marked guide whose braid is another's
// has its mark covered (the Tube tool shows it so).
std::uint64_t braidTubeOf(const GuideHierarchy& hierarchy, std::uint64_t id);

}  // namespace groomlab
