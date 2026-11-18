// SPDX-License-Identifier: Apache-2.0
#pragma once

// A Braid node's sections as geometry (operators/braid_op.cpp), for hosts' displays, tools and tests.
//
// A braid's hair runs in sections: each a tube along its own course through the weave, every strand
// at a spot of its own in its section. Two views of them, both read off the node's own stations (the
// ones its strands are braided with), so they lie where the hair does:
//
//     braidSections(settings)        // one cycle of the weave in the braid's cross-section, in braid
//                                    // widths: each section's center, half axes and outline, sample
//                                    // by sample
//     braidTubes(graph, node, ctx)   // a Braid node's sections along its paths, in the groom's space:
//                                    // each a center line, its cross-sections and a quad tube mesh
//
// A version 1 Braid node's sections are plain elliptical tubes that pass through each other where
// they cross. A version 2 node presses them against each other (`separate`, `inflate`): each
// section has an outline of its own in the braid's cross-section, flat where it lies against
// another, round where it lies free, never inside another. BraidSections::overlap and gap measure
// it; the tubes' rings follow the outlines.
//
// Show Tubes (every host's button on a Braid node) draws them in the viewport: the names of the
// nodes shown are a display setting (DisplaySettings::braidTubes; the rules here and in
// graph_edit.h), and GroomSession::braidTubeMesh gives what a host draws for them (BraidTubeMesh).

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "groomlab/graph.h"
#include "groomlab/vec3.h"

namespace groomlab {

// The Braid params that shape its weave's cross-section (the node's own names and defaults), and
// the node's version (1: plain ellipses, whatever separate and inflate say).
struct BraidWeaveSettings {
    std::string style = "three";
    int sections = 0;
    bool dutch = false;
    float flatness = 0.3f, pancake = 0.0f, tightness = 1.0f, separate = 1.0f, inflate = 0.5f;
    int version = 2;
};

// One cycle of a weave in the braid's cross-section, in braid widths: [0] side to side, [1] depth
// (out of the scalp). `perStep` samples a weave step, `cycle` steps (after which every section is
// back where it began: the samples wrap round).
struct BraidSections {
    int sections = 0, cycle = 0, perStep = 0;
    int around = 0;                          // points round an outline
    bool round = false;                      // a rope's plies: each section's thickness along the radius from the braid's axis
    bool pressed = false;                    // a version 2 node's: the sections pressed against each other
    float halfWidth = 0.0f, halfThick = 0.0f;  // a section's half sizes as the weave has them (pancake's loosening in)
    // [section][sample]: its center, and the two half axes (across its band, through it) of its
    // ellipse there. Pressed: the ellipse the braid's cross-section cuts of the section (longer the
    // way its course tilts), before the press. Else the section's own cross-section, square to its
    // course, seen along the braid.
    std::vector<std::vector<std::array<float, 2>>> centers, across, thick;
    // [section][sample * around + k]: the section's outline, which its hair lies within: point k
    // along the ray center + across cos + thick sin of turn k / around. Pressed: as far along it as
    // the press leaves the section; else on the ellipse.
    std::vector<std::vector<std::array<float, 2>>> outlines;
    // Pressed: the scale, side to side and in depth, the whole cross-section took to keep the
    // braid's reach (in the centers and axes above), and what the sections were grown by first.
    std::array<float, 2> keep{1.0f, 1.0f};
    float grown = 1.0f;

    std::size_t samples() const { return centers.empty() ? 0 : centers.front().size(); }
    const std::array<float, 2>* outline(int section, std::size_t sample) const {
        return outlines[static_cast<std::size_t>(section)].data() + sample * static_cast<std::size_t>(around);
    }
    // The area of a section's outline at a sample (braid widths squared).
    float area(int section, std::size_t sample) const;
    // How far sections a and b reach into each other at a sample (braid widths): the deepest a point
    // of one's outline lies inside the other's; 0 when they are clear.
    float overlap(std::size_t sample, int a, int b) const;
    // The most any two sections overlap over the cycle.
    float overlap() const;
    // The least distance between the outlines of sections a and b at a sample (0: touching, or
    // overlapping).
    float gap(std::size_t sample, int a, int b) const;
    // The farthest any outline reaches from the braid's path over the cycle, side to side and in depth.
    std::array<float, 2> reach() const;
};

// A weave's sections over one cycle as a Braid node of those settings lays them (its stations' own
// values on a straight braid one wide), `perStep` samples a step, `around` points round an outline.
BraidSections braidSections(const BraidWeaveSettings& settings, int perStep = 16, int around = 48);

// A Braid node's sections along its paths: per path and section a tube over the braid's woven
// stretch (from where the hair has gathered into it, or with feed from the path's start, to where
// the weave ends), with pancake, the press (separate, inflate), the taper, ties and a fed braid's
// growth and lift as the hair has them. Loops are left out (hair let out of its section), and so is
// fuzz.
struct BraidTubes {
    struct Tube {
        int path = 0;          // which of the node's paths (the paths output's order)
        float braidId = 0.0f;  // the braid's id, as its strands' braidId
        int section = 0;       // the section, as its strands' braidSection
        StrandRoot root;       // its path's root on the scalp (the path's guide's; face -1: not on it)
        // A ring per station along the path: how far along it is (scene units), the section's
        // center there and its ellipse's half axes (across the band, through it; pressed sections:
        // the ellipse before the press, BraidSections::across).
        std::vector<float> at;
        std::vector<Vec3f> centers, across, thick;
        std::size_t firstPoint = 0;  // its points in `points`: `around` a ring, ring after ring
        std::size_t firstQuad = 0;   // its quads in `quads`: `around` between two rings
    };
    int around = 0;        // points round a ring
    float perStep = 8.0f;  // rings a weave step (BraidTubeDetail: fewer when its point budget asked)
    bool pressed = false;  // a version 2 node's: the rings follow the sections' pressed outlines
    std::vector<Tube> tubes;
    // All the tubes as one quad mesh, open at the ends (a quad grid rolled into a tube each), facing
    // out. A ring's point k lies along center + across * cos + thick * sin of turn k / around: on
    // the ellipse, or for pressed sections on the section's outline (as far along that ray as the
    // press leaves it), held by the braid's ties as the hair is.
    std::vector<Vec3f> points;
    std::vector<std::array<std::uint32_t, 4>> quads;
    std::vector<std::uint32_t> quadTube;  // each quad's tube
};

// How fine the tubes' mesh is: `around` points round a ring (3 at least), `perStep` rings a weave
// step. `maxPoints` (0: no limit): the most points the mesh should take; a node of many braids past
// it gets fewer rings a step, then fewer points round, not under kLeastPerStep and kLeastAround (a
// weave still reads; so hundreds of long braids take more) until that comes to kMostOverBudget
// times the budget (thousands of braids): then fewer rings still, down to each tube's two ends.
struct BraidTubeDetail {
    static constexpr float kLeastPerStep = 4.0f;
    static constexpr int kLeastAround = 8;
    static constexpr std::size_t kMostOverBudget = 4;
    // The points round a ring a display draws with (braidTubesAround): an ellipse reads with 12; a
    // pressed outline has flat faces and corners between them, and its hair lies closer under it.
    static constexpr int kAround = 12, kPressedAround = 24;
    int around = 12;
    float perStep = 8.0f;
    std::size_t maxPoints = 0;
};

// The tubes of a Braid operator evaluated with these inputs (its strands, its braids): nothing is
// braided, only the paths are laid as an evaluation lays them. Throws GraphError as an evaluation
// would, and for an operator that is not a Braid.
BraidTubes braidTubes(const Operator& braid, const EvalContext& ctx, const std::vector<Value>& inputs, int around = 12);
BraidTubes braidTubes(const Operator& braid, const EvalContext& ctx, const std::vector<Value>& inputs, const BraidTubeDetail& detail);
// The same for a Braid node of a graph, its inputs evaluated through the graph (cached results are
// reused; the Braid itself is not evaluated).
BraidTubes braidTubes(Graph& graph, NodeId braid, const EvalContext& ctx = {}, int around = 12);
BraidTubes braidTubes(Graph& graph, NodeId braid, const EvalContext& ctx, const BraidTubeDetail& detail);
// The points round a ring a display draws a Braid node's tubes with (BraidTubeDetail::kAround, or
// kPressedAround for a version 2 node: its sections are pressed).
int braidTubesAround(const Operator& braid);

// ---- Show Tubes ---------------------------------------------------------------------------------

// The Braid nodes whose tubes the viewport shows (DisplaySettings::braidTubes, a groom file's
// "braidTubes"): their names, one a line (a node's name may hold spaces and commas). Display only:
// no evaluation reads it. braidTubeNames: the names in order, each once, blank lines left out;
// braidTubesText: its reverse.
std::vector<std::string> braidTubeNames(const std::string& shown);
std::string braidTubesText(const std::vector<std::string>& names);
// The names once node `from` is renamed `to`: the same at `to` when it was shown, else as they are
// (every host's Rename keeps Show Tubes on).
std::string renamedBraidTubes(const std::string& shown, const std::string& from, const std::string& to);
// A section's color (linear RGB): Clump Colors' saturation and value, the hues a golden-ratio step
// apart from section to section, so a braid's sections read apart whatever their count (Clump
// Colors' own hues, hashed from the number, put sections 0 and 2 both in pink).
std::array<float, 3> braidSectionColor(int section);

// The tubes of the Braid nodes shown as a host draws them: one quad mesh (open tubes, facing out)
// with a normal and its section's color at each point, in the space the strands are drawn in. A
// host triangulates at the draw call (triangles: each quad's two). An elliptical section's ring is
// drawn round its outline, not inside it (its sides touch the outline, its corners lie 1 / cos(pi /
// around) out: 3.5% at 12 a ring), so the hair on a section's rim stays under the drawn surface. A
// pressed section's ring is drawn on its outline (BraidTubes::pressed: moved out, two sections
// lying against each other would be drawn through one another; its finer ring keeps the hair
// under).
struct BraidTubeMesh {
    std::vector<Vec3f> points, normals;
    std::vector<std::array<float, 3>> colors;
    // Per point: how far along its tube (0 its first ring, 1 its last) and round its ring (0 to 1).
    std::vector<std::array<float, 2>> uvs;
    std::vector<std::array<std::uint32_t, 4>> quads;
    std::size_t tubes = 0;
    // A hash of what it was built from (the nodes' params, their inputs, the scalp): equal keys,
    // equal meshes. Never 0 for a mesh GroomSession::braidTubeMesh made.
    std::uint64_t key = 0;

    bool empty() const { return quads.empty(); }
    std::vector<std::uint32_t> triangles() const;
};
// A node's tubes added to a mesh: their points (an elliptical section's rings moved out round its
// outline, see above) and quads, smooth normals and each tube's section color.
void appendBraidTubes(BraidTubeMesh& mesh, BraidTubes tubes);

}  // namespace groomlab
