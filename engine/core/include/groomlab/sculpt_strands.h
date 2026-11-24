// SPDX-License-Identifier: Apache-2.0
#pragma once

// SculptStrands nodes: brush strokes on the strands themselves (XGen Interactive's way), late in the
// chain, after the look nodes, so what is fixed is what shows. The edits are kept in the groom with
// the Sculpt nodes' (sculpt.h: SculptEdits::strands, by strand ID), in the same relative form: per
// strand, the turn and length factor of each segment of the strand as the node was fed it. So when
// what feeds the node changes (the guides combed, a Clump's or a Noise's params), the sculpted bends
// come back on top of the new shapes, and nothing snaps back to old positions.
//
// Strand IDs are stable (Scatter: raising the density only adds strands; a preview thins by ID), so
// an edit stays on its strand. Strands with no edit of their own fill from their neighbours': the
// strands a raised density adds, and the render's strands past those a thinned preview showed when
// the strokes were made. Each recorded strand keeps its root's place and how far apart the strands
// were there (its spacing: the mean distance to the four nearest other roots, on the rest pose). A
// strand without an edit blends the edits of the recorded strands whose roots are within kFillReach
// of their spacing of its own (shares by a smooth falloff of the distance, adding up to 1), all of
// it within a spacing of the nearest, fading to none at its reach: inside a brushed region new
// strands take the stroke whole, past its edge it fades out within half a spacing. A strand brushed
// over and left as it was keeps an empty edit: its share is none, so it holds the fill back.

#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

#include "groomlab/guide_hierarchy.h"
#include "groomlab/guide_set.h"
#include "groomlab/math.h"
#include "groomlab/sculpt.h"
#include "groomlab/strand_set.h"

namespace groomlab {

class ScalpMesh;
using StrandsPtr = std::shared_ptr<const StrandSet>;

// How far a recorded strand's edit reaches strands with none, in its spacings.
constexpr float kFillReach = 1.75f;

// The strands of `input` with `edits.strands` at `strength`: each strand with an edit of its own (by
// ID) has it; with `fill`, each without takes its recorded neighbours' (the header). `input` itself
// when nothing reaches a strand. Without a scalp (or a root off it) a strand's frame starts from
// world axes and nothing fills (the roots' places are the scalp's).
StrandsPtr applyStrandSculpts(const StrandsPtr& input, const SculptEdits& edits, float strength, const ScalpMesh* scalp,
                              bool fill = true);

// What each strand of a set gets, flat: the GPU's tables (look_chain.cpp's SculptStrands stage) and
// what applyStrandSculpts lays on the CPU. Per strand: its edit's samples (the first index into
// `samples` and how many; 0: no edit) and its root frame (the normal and the direction across the
// scalp the segments' frames start from). It depends on the edits, the strands' IDs and roots and
// the scalp, not on the strands' shapes: a drag above the node reuses it.
// Laid out as the GPU reads them (three buffers): `ranges` two words a strand (first, count),
// `samples` four floats each (t, ax, ay, scale), `frames` eight floats a strand (normal xyz, 0,
// across xyz, 0).
struct ResolvedStrandSculpts {
    std::vector<std::uint32_t> ranges;
    std::vector<SculptSample> samples;
    std::vector<float> frames;
    bool any = false;  // whether some strand has an edit

    std::size_t strandCount() const { return ranges.size() / 2; }
};
static_assert(sizeof(SculptSample) == 16, "SculptSample is four floats: the GPU reads them as such");
ResolvedStrandSculpts resolveStrandSculpts(const StrandSet& input, const SculptEdits& edits, const ScalpMesh* scalp, bool fill = true);
// The strands of `input` with `resolved` (made for them) laid on at `strength`; `input` itself when
// no strand has an edit.
StrandsPtr applyResolvedStrandSculpts(const StrandsPtr& input, const ResolvedStrandSculpts& resolved, float strength);

// The edit strand `s` of `input` gets now (its own, or with `fill` its neighbours' blend); empty when
// none reaches it. What applyStrandSculpts lays on it.
GuideSculpt strandSculptAt(const StrandSet& input, std::size_t s, const SculptEdits& edits, const ScalpMesh* scalp, bool fill = true);

// A stroke's result recorded into `edits`: `sculpted` holds every strand the stroke reached (by its
// index in `input`, the node's input), changed or not, with its points after the stroke (the same
// point count). Each gets an edit of its own taking its input to those points at `strength` (none
// left: an empty edit, still kept), its root and its spacing among `input`'s strands. Strands the
// stroke did not reach keep what they had. Returns the IDs recorded. Throws std::invalid_argument
// when strength is about 0 or a point count differs.
std::vector<StrandId> recordStrandStroke(SculptEdits& edits, const StrandSet& input,
                                         const std::vector<std::pair<std::size_t, std::vector<Vec3f>>>& sculpted, float strength,
                                         const ScalpMesh* scalp);

// The strands of `strands` as guides, each with its ID, root and points (a preview's hidden strands
// left out), drawn in `color`: what the brushes shape for a SculptStrands node (sculptHierarchy of
// its output), one guide a strand.
std::shared_ptr<const GuideSet> strandsAsGuides(const StrandSet& strands, const Vec3f& color);

// A brush stroke on a SculptStrands node kept dab after dab (stroke.h): each dab records only the
// strands whose shaped points changed since the dab before (the first dab: since the stroke began,
// the node's output then), each anew from its input (recordStrandStroke), the strand spacing index
// made once a stroke. `edits` is left as the stroke leaves the node's: `stored` with those strands'
// edits. clear() before another stroke.
class StrandStrokeRecord {
public:
    StrandStrokeRecord();
    ~StrandStrokeRecord();
    void clear();
    // The stroke's start: the node's input and its output then (the same strands, by index), its scalp.
    void begin(const StrandsPtr& input, const StrandsPtr& output, const ScalpMesh* scalp);
    bool started() const;
    // Throws as recordStrandStroke. Returns how many strands this dab recorded.
    std::size_t record(SculptEdits& edits, const SculptEdits& stored, const GuideHierarchy& sculpted, float strength);

private:
    struct State;
    std::unique_ptr<State> state_;
};

}  // namespace groomlab
