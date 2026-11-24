// SPDX-License-Identifier: Apache-2.0
#pragma once

// Sculpt nodes: brush strokes on guides the graph made (Houdini's guide levels: sparse hand guides
// -> Scatter + Interpolate -> ToGuides -> Sculpt -> the next Interpolate, Clump centers...). The
// strokes are kept inside the groom, keyed by the node's name ("sculpts", GroomData::sculptsJson),
// and per guide by its ID, so they follow the guides when the graph above makes them again.
//
// An edit is kept as a relative form, not as positions: for each segment of the guide as the node
// was fed it, the turn that takes the segment's direction to the sculpted one, in the segment's
// own rotation-minimizing frame, and the segment's length factor, both at the segment's middle by
// normalized arc length t (0 root, 1 tip). Reconstruction walks the guide fed now from its root:
// each segment turned by the edit's turn at its t (read between the recorded samples) in its own
// frame, and scaled. So when what feeds the node changes (the sparse guides combed, a Noise added,
// the guides longer or with other point counts), the sculpt adds the same bends on top of the new
// shapes: a 30 degree hook near the tip stays a 30 degree hook, lengths stay what they were but
// where the sculpt scaled them, and nothing snaps back to old positions.
//
// The frames: the root frame is the scalp's surface frame at the guide's root (its normal and the
// root face's first edge, as guide_hierarchy.cpp's rootFrame) turned by the minimal rotation that
// takes the normal onto the first segment (defined for any direction but straight into the
// scalp), then carried segment to segment by the minimal rotation between their directions
// (discrete parallel transport: rotation-minimizing). Without a scalp (or a root off it) world +Y
// and +X stand in for the normal and the edge.
//
// Only guides a stroke touched hold an edit. IDs the graph no longer makes keep theirs, unused,
// and use them again if the ID comes back.

#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "groomlab/guide_hierarchy.h"
#include "groomlab/guide_set.h"
#include "groomlab/math.h"

namespace groomlab {

class ScalpMesh;

// One sample of a guide's edit: at normalized arc length t of the guide fed the node, the turn
// (axis-angle in the segment's frame: x along the frame's normal, y its binormal; the axis lies
// across the segment, so z is 0 and left out) and the length factor.
struct SculptSample {
    float t = 0.0f;
    float ax = 0.0f, ay = 0.0f;
    float scale = 1.0f;

    bool operator==(const SculptSample& o) const { return t == o.t && ax == o.ax && ay == o.ay && scale == o.scale; }
};

// One guide's edit: samples by increasing t (one per segment of the guide it was recorded on).
struct GuideSculpt {
    std::vector<SculptSample> samples;

    // Whether it changes nothing (every turn and length factor within 1e-6 of none).
    bool identity() const;
    // The sample at t: linear between the two around it, the end ones held past them.
    SculptSample at(float t) const;
    bool operator==(const GuideSculpt& o) const { return samples == o.samples; }
};

// One strand's edit in a SculptStrands node (sculpt_strands.h): the guides' relative form, plus
// where the strand grew and how far apart the strands were there when it was recorded, so strands
// with no edit of their own (density raised, the render's strands past a thinned preview's) take
// their neighbours' (sculpt_strands.h's fill). An empty edit: brushed over but left as it was (its
// neighbours with none read it as none, not as missing).
struct StrandSculpt {
    StrandRoot root;       // the strand's root when recorded (its rest-pose place: the fill's)
    float spacing = 0.0f;  // the distance to the nearest other strand's root then, on the rest pose
    GuideSculpt edit;

    bool operator==(const StrandSculpt& o) const {
        return root.face == o.root.face && root.u == o.root.u && root.v == o.root.v && spacing == o.spacing && edit == o.edit;
    }
};

// One Sculpt node's edits, by guide ID; a SculptStrands node's, by strand ID.
struct SculptEdits {
    std::map<GuideId, GuideSculpt> guides;
    std::map<std::uint64_t, StrandSculpt> strands;
    // The node's guides (or strands) frozen for its brushes, by ID: their freeze value (0-1; none
    // left out). The freeze brush, Unfreeze All and Invert Frozen set it while the node is targeted.
    std::map<std::uint64_t, float> frozen;

    bool empty() const { return guides.empty() && strands.empty() && frozen.empty(); }
    std::uint64_t contentHash() const;
    bool operator==(const SculptEdits& o) const { return guides == o.guides && strands == o.strands && frozen == o.frozen; }
};
using SculptPtr = std::shared_ptr<const SculptEdits>;

// The groom's sculpts by Sculpt node name ("compound/inner" inside a compound):
// {"<node>": {"guides": {"<guide id>": [t, ax, ay, scale, t, ax, ay, scale, ...]}}}; a SculptStrands
// node's under "strands": {"<strand id>": [face, u, v, spacing, t, ax, ay, scale, ...]}; what is
// frozen for its brushes under "frozen": {"<id>": value} (builds before read the guides alone and
// leave the others out). Empty text: none. A node with no
// edits is left out of the text.
class SculptSet {
public:
    std::map<std::string, SculptEdits> nodes;

    bool empty() const;
    const SculptEdits* node(const std::string& name) const;
    std::string toJson() const;
    // Throws std::invalid_argument ("sculptData: <why>").
    static SculptSet fromJson(const std::string& text);
};

// The texts hosts edit without parsing the guides (a node renamed, duplicated, grouped, deleted,
// its edits cleared). Each returns the new text (the same text when nothing changes); throw as
// fromJson. Renamed: the edits move to `to` (replacing any there), and a compound's inner nodes'
// with it ("from/x" to "to/x"). Copied: `to` gets a copy (a compound's inner nodes' too). Moved:
// each rename of `moves` in order (GraphEdit::movedNodes: a rename, Group, Ungroup). Removed or
// cleared: `name`'s edits go.
std::string renameSculptNode(const std::string& json, const std::string& from, const std::string& to);
std::string copySculptNode(const std::string& json, const std::string& from, const std::string& to);
std::string moveSculptNodes(const std::string& json, const std::vector<std::pair<std::string, std::string>>& moves);
std::string clearSculptNode(const std::string& json, const std::string& name);
// The nodes holding edits in a text.
std::vector<std::string> sculptedNodes(const std::string& json);

// The external a Sculpt node reads its edits from: "sculpt:<node path>".
std::string sculptExternal(const std::string& nodePath);

// A guide fed with a path frame per point (a Braid's sections: point layers kPathFrameLayers, the
// path's arc length there, its point and its frame's normal and tangent) is edited in that path's
// straightened space: each point as its offset in the frame there (normal, binormal, and along the
// tangent from the arc length), the relative form recorded and applied there with world axes, the
// result put back on the path as it is now. So a section's strokes ride along with its braid's path
// when the path is moved or bent (its weave in the path's frame unchanged: the same strokes).
extern const char* const kPathFrameLayers[10];  // braidPathAt, braidPathX..Z, braidNormalX..Z, braidTangentX..Z

// A guide fed the node (its points, its root for the frame) with an edit at `strength` (0: the
// guide as it is; 1: the edit; between, every turn and length factor that share of it).
std::vector<Vec3f> applySculpt(const Guide& input, const GuideSculpt& edit, float strength, const ScalpMesh* scalp);
// The edit taking `input` to `sculpted` (the same point count) at `strength`: applySculpt(input,
// edit, strength) gives `sculpted` back (its root at the input's). Throws std::invalid_argument
// when strength is about 0 (nothing could be recorded) or the point counts differ.
GuideSculpt recordSculpt(const Guide& input, const std::vector<Vec3f>& sculpted, float strength, const ScalpMesh* scalp);
// The same relative form with the root frame given (a unit normal, and a direction across the
// surface there, laid flat on it) in place of the scalp's at the root's triangle: SculptStrands'
// (sculpt_strands.h), one frame shared by neighbouring strands (ScalpMesh::acrossAt) so their edits
// mean the same turns and blend.
std::vector<Vec3f> applySculptFramed(const std::vector<Vec3f>& points, const GuideSculpt& edit, float strength, const Vec3f& normal,
                                     const Vec3f& across);
GuideSculpt recordSculptFramed(const std::vector<Vec3f>& points, const std::vector<Vec3f>& sculpted, float strength, const Vec3f& normal,
                               const Vec3f& across);
// Every guide of `input` with its edit applied (by ID), the others as they are; `input` itself
// when no edit reaches it.
std::shared_ptr<const GuideSet> applySculpts(const std::shared_ptr<const GuideSet>& input, const SculptEdits& edits,
                                             float strength, const ScalpMesh* scalp);

// A stroke's result recorded into `edits`: each guide of `sculpted` (by ID) whose points differ
// from the node's output now (`input` with `edits` at `strength`) by more than a ten-thousandth
// of its length gets its edit recorded again (an edit back to none goes); the others keep theirs.
// Returns the IDs recorded. Throws std::invalid_argument when strength is about 0.
std::vector<GuideId> recordSculptStroke(SculptEdits& edits, const GuideSet& input, const GuideHierarchy& sculpted,
                                        float strength, const ScalpMesh* scalp);

// The same record kept through a live stroke, dab after dab: `edits` left as recordSculptStroke
// leaves a copy of `stored` (the node's edits when the stroke began), at the cost of the guides
// whose sculpted points changed since the dab before (the first dab records them all). `stored`,
// `input`, `strength` and `scalp` are the stroke's: the same at every dab; `edits` is the record's
// own between dabs (clear() before another stroke, or after changing it).
class SculptStrokeRecord {
public:
    void clear();
    // Throws as recordSculptStroke (the record is then cleared).
    void record(SculptEdits& edits, const SculptEdits& stored, const GuideSet& input, const GuideHierarchy& sculpted,
                float strength, const ScalpMesh* scalp);

private:
    std::vector<std::vector<Vec3f>> seen_;  // per guide of the input, the sculpted points last recorded (none: not there)
    bool started_ = false;
};

// What a stroke on a Sculpt node reads (GroomSession::sculptInput): the guides it is fed, the
// scalp it takes (null: none connected), its strength and its stored edits.
struct SculptInput {
    std::string node;
    std::shared_ptr<const GuideSet> guides;
    std::shared_ptr<const ScalpMesh> scalp;
    float strength = 1.0f;
    SculptEdits edits;
    // A SculptStrands node's (sculpt_strands.h): the strands it is fed (`guides` then null), and its fill.
    std::shared_ptr<const StrandSet> strands;
    bool fill = true;
};

// A node's frozen guides (SculptEdits::frozen) laid on `h` as their `freeze` attribute, and read
// back from it (the guides with some freeze, by ID).
void layFrozen(GuideHierarchy& h, const SculptEdits& edits, const std::string& freeze = "freeze");
std::map<std::uint64_t, float> frozenOf(const GuideHierarchy& h, const std::string& freeze = "freeze");

// The guides of `guides` as an editable hierarchy (one root group named `group`), rooted on
// `scalp` when given: what brushes shape for a Sculpt node (its output).
std::shared_ptr<GuideHierarchy> sculptHierarchy(const GuideSet& guides, const std::string& group, const ScalpMesh* scalp);

}  // namespace groomlab
