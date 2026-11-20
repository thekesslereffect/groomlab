// SPDX-License-Identifier: Apache-2.0
#pragma once

// Screen-space level of detail for strand lines (docs/research/clump_lod.md): a viewport draws, per
// clump, m of its n strands, each standing for n / m of them, and a clump thinner than a pixel as
// one line along its axis. Viewport only: renders and exports never see it.
//
// The strands are grouped by their clump channel (StrandLodSettings::clumpChannel, Clump's
// clumpId), or, without one, by cells of their roots. Within a group they are ranked by the hash
// of their ID, the preview's own order (keepAtDensity): a group drawing m strands draws its first
// m, so fewer strands are always a subset of more (nothing swaps as the view zooms).
//
// A group's m comes from two rules (the research's section 1.3), each point of its axis taken at
// the pixels a scene unit spans where that point comes closest to the camera as the camera orbits
// the strands' bounding sphere at its distance (StrandLodView):
//   - coverage: the m strands, each widened to 1 - (1 - c)^(n/m) from its own coverage c (what the
//     shaders' lineCoverageAtDensity does for a thinned preview), stay below c* = 2 x the error
//     allowed (StrandLodSettings::error), where widening's bias (about half of c* relative) stays
//     within it: m >= n ln(1 - c) / ln(1 - c*), c the members' mean coverage;
//   - footprint: a group spread D pixels across needs about D lines a pixel wide to fill it.
// The share n / m is rounded down to a class, a quarter octave apart (lodClassExponent): a host
// draws each class's strands with one exponent (its shaders' density, lineDensity, divided by the
// class's exponent), so the classes are its draw calls, and a group takes round(n / exponent)
// strands, so the strands stand for the group's n exactly (to a strand).
//
// A group whose members spread less than kLodImpostorSpread pixels about its axis is drawn as one
// strand along the axis (impostors()): the members' mean point by point, as wide as the ink of
// the members as a Gaussian band of Beer-Lambert fibers (lodBandInk), in their mean color.
//
// The view is the pixels a scene unit spans at the nearest depth the strands' bounding sphere's
// center can come to as the camera orbits its pivot (the point it tumbles about: Maya's center of
// interest or tumble pivot, the app's orbit target; the sphere's center when none is given),
// quantized to eighth octaves with hysteresis (lodLevel), and the pixels it spans at clip w 1 (the
// projection and the viewport's height). An orbit keeps the camera's distance to its pivot and the
// pivot's place in the view, so the pivot's depth d stays, and the center, e from the pivot, comes
// as near as d - e and goes as far as d + e whichever way it turns; a point r from the center comes
// as near as d - e - r. So nothing comes or goes as the camera orbits, about whatever pivot, only
// as it comes nearer or goes farther, in steps. At the view drawn last a group's class keeps a
// quarter class of hysteresis, so a dab's or a tweak's moved strands change nothing; at another
// view the rules alone choose. Everything is deterministic: the same strands, settings and view
// give the same selection, whichever views came before (but the one just before, the same).

#include <array>
#include <climits>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "groomlab/math.h"
#include "groomlab/strand_set.h"

namespace groomlab {

struct StrandColors;
using StrandsPtr = std::shared_ptr<const StrandSet>;

// Classes a quarter octave apart: class k's strands each stand for 2^(k/4) strands.
constexpr int kLodClassesPerOctave = 4;
constexpr int kLodClassCount = 32;
float lodClassExponent(int lodClass);
// Levels of the view's scale an eighth of an octave apart.
constexpr int kLodLevelsPerOctave = 8;
constexpr int kNoLodLevel = INT_MIN;
// Members spread less than this (pixels, RMS across the screen) about their axis: one line.
constexpr float kLodImpostorSpread = 0.75f;
// The fewest members a group thins or draws as one line (smaller ones are drawn whole).
constexpr std::size_t kLodMinGroup = 4;

// The level of a scale (eighth octaves: round(8 log2 pixelsPerUnit)), `previous` kept while the
// scale stays within three quarters of a level of it; kNoLodLevel for an infinite or unusable scale.
int lodLevel(float pixelsPerUnit, int previous = kNoLodLevel);
float lodLevelPixels(int level);

// A view as the level of detail takes it.
struct StrandLodView {
    // lodLevel of the pixels a unit spans at the nearest depth the bounding sphere's center comes to
    // as the camera orbits its pivot (none: no LOD), and at the farthest it goes.
    int level = kNoLodLevel;
    int farLevel = kNoLodLevel;
    float pixelScale = 0.0f;  // pixels a unit spans at clip w 1 (display.h linePixelScale); orthographic: at any depth
    bool orthographic = false;
    // The pixels a unit spans at the closest a point `distance` from the center comes to the eye:
    // the center's nearest depth (pixelScale over the level's pixels) less the distance; infinite at
    // or past the eye.
    float pixelsAt(float distance) const;
    // ... and at the farthest it goes (the center's farthest depth plus the distance).
    float pixelsAtFarthest(float distance) const;
    bool operator==(const StrandLodView& o) const {
        return level == o.level && farLevel == o.farLevel && pixelScale == o.pixelScale && orthographic == o.orthographic;
    }
    bool operator!=(const StrandLodView& o) const { return !(*this == o); }
};
// The view of a camera at `eye` looking along `forward` with `pixelScale` (display.h
// linePixelScale) on strands bounded by a sphere about `center`, the camera orbiting `pivot` (the
// point it tumbles about; the center without one); `previous` the view last given for this camera
// (its levels' hysteresis). No level when the center can come to the eye's plane or behind it as
// the camera orbits (the pivot's depth no more than its distance to the center).
StrandLodView lodView(const Vec3f& eye, const Vec3f& forward, float pixelScale, bool orthographic, const Vec3f& center,
                      const Vec3f& pivot, const StrandLodView& previous = StrandLodView{});
StrandLodView lodView(const Vec3f& eye, const Vec3f& forward, float pixelScale, bool orthographic, const Vec3f& center,
                      const StrandLodView& previous = StrandLodView{});

// The ink across a Gaussian band of Beer-Lambert fibers, over its spread: the integral over y of
// 1 - exp(-a phi(y)), phi the standard normal density, a the fibers' total width over the band's
// spread (sigma). A band's ink (pixels) is sigma x lodBandInk(total width / sigma); about a for a
// thin band, about 2 sigma sqrt(2 ln a) for a dense one.
float lodBandInk(float a);

struct StrandLodSettings {
    std::string clumpChannel = "clumpId";
    // The coverage error allowed, relative (c* = 2 x error: widened strands cover at most that).
    float error = 0.175f;
    float widthScale = 1.0f;  // the lines' width scale (DisplaySettings::ribbonWidthScale)
    // The density a thinned preview's coverage is taken to the render's from (its shaders'
    // lineDensity; 1: none): each strand already stands for 1 / density.
    float density = 1.0f;
    bool impostors = true;
    // Class 0's segments that may blend at this view (their own coverage, somewhere an orbit at
    // this distance takes them, within lineKeepShare's blended range) in a list of their own
    // (kLodBlended), which a host draws twice: its opaque fragments alone with the rest, depth
    // written, then after everything its blended fragments alone, depth not written. A blended
    // fragment then no longer hides the strands drawn after it behind it (the light streaks of a
    // blended fragment writing depth). The class's other segments draw once, as ever: a segment
    // the list should have held (a coarser view drawing the same lists, a playblast smaller than
    // the panel) draws as it did before, never loses a fragment. Thinned classes' strands are
    // hashed alone (they stand for several: no blended range), and so are impostors. Off (a
    // multisampled target whose lines write coverage masks, never blending): no list.
    bool splitBlended = true;
};

// The parts of a class's segments: those that do not blend (drawn once, every fragment), or class
// 0's that may (drawn twice: opaque fragments with the rest, blended ones after everything).
constexpr int kLodOpaque = 0;
constexpr int kLodBlended = 1;
// A host's index lists ("slots"): slot k < kLodClassCount class k's segments that do not blend,
// slot kLodClassCount class 0's blendable ones.
constexpr int kLodSlotCount = kLodClassCount + 1;

class StrandLod {
public:
    StrandLod();
    ~StrandLod();
    StrandLod(const StrandLod&) = delete;
    StrandLod& operator=(const StrandLod&) = delete;

    // The strands the lines draw. Grouped again when their IDs, point counts or clump channel
    // changed; else the groups are kept and only their shapes (axis, spread, widths, measured on
    // up to 64 members each) measured again. The same set (pointer) does nothing.
    void setStrands(StrandsPtr strands, const std::string& clumpChannel);
    const StrandsPtr& strands() const { return strands_; }
    std::size_t groupCount() const;
    // Grouped by the clump channel (else by cells of the roots).
    bool clumped() const { return clumped_; }
    // The strands carry widths (displayWidths: one a point, some set): without them lines draw
    // opaque, and nothing is thinned.
    bool hasWidths() const { return hasWidths_; }
    // The strands' bounding sphere (lodView: the center's depth).
    Vec3f center() const { return center_; }
    float radius() const { return radius_; }
    // Moves when the groups were made again (another strand set's IDs or counts).
    std::uint64_t groupingSerial() const { return groupingSerial_; }

    struct Selection {
        // Some group is thinned or drawn as one line: draw the classes and the impostors instead of
        // every strand. Off: draw the item as it is.
        bool active = false;
        // Moves when the classes' strands change: a host keeps its index buffers while it stays.
        std::uint64_t key = 0;
        // Moves when the impostors change (which groups, their shapes as measured, the settings
        // their widths take): a host keeps its impostor item while it stays (0: none).
        std::uint64_t impostorKey = 0;
        StrandLodView view;
        // Each class's strands (indices into the set, ascending): drawn with lodClassExponent(k).
        std::array<std::vector<std::uint32_t>, kLodClassCount> strands;
        std::vector<std::uint32_t> impostorGroups;
        std::size_t drawnStrands = 0;  // the classes' strands together (impostors not counted)
        std::size_t totalStrands = 0;
        std::size_t drawnSegments = 0;
        std::size_t thinnedGroups = 0;  // groups past class 0
        std::size_t blendSegments = 0;  // class 0's segments in kLodBlended
        // Each slot's segments, and a key that moves when they change (0: none): a host writes
        // again only the slots whose key moved.
        std::array<std::size_t, kLodSlotCount> slotSegments{};
        std::array<std::uint64_t, kLodSlotCount> slotKeys{};
    };
    // The selection for a view (lodView; no level: none, inactive) with these settings.
    const Selection& select(const StrandLodView& view, const StrandLodSettings& settings);
    const Selection& selection() const { return selection_; }

    // A class's segments of a part (kLodOpaque, kLodBlended: class 0's alone) as index pairs into
    // the set's points (two a segment), in strand order (written in parallel chunks).
    std::size_t segmentCount(int lodClass, int part = kLodOpaque) const;
    void writeSegments(int lodClass, int part, std::uint32_t* pairs) const;
    // Every class's segments that do not blend, class by class, then class 0's blendable ones (the ranges:
    // `firsts`, offsets into the indices, two a segment: class k's [firsts[k], firsts[k + 1]), the
    // blended [firsts[kLodClassCount], firsts[kLodClassCount + 1])).
    std::vector<std::uint32_t> classSegments(std::array<std::size_t, kLodClassCount + 2>& firsts) const;

    // The selection's impostors as strands: one per group, its points the members' mean point by
    // point, its widths each point's band ink (lodBandInk) for the lines' width scale and density
    // as selected, its root and ID from the group (null: none). Built once per selection.
    StrandsPtr impostors() const;
    // Their colors from the members' (`members`: colors of the set's strands): each point the
    // members' mean (null: none).
    std::shared_ptr<const StrandColors> impostorColors(const StrandColors& members) const;

private:
    struct Groups;
    void measure();
    StrandsPtr strands_;
    std::unique_ptr<Groups> groups_;
    Selection selection_;
    StrandLodSettings selectedWith_;
    // selection_ is select's for the strands set now (cleared when they change): the same view and
    // settings again give it as it is (an orbit holds the view's level).
    bool selectionCurrent_ = false;
    std::vector<std::uint8_t> blendSegment_;  // per point: the segment from it is class 0's and blendable
    mutable StrandsPtr impostors_;
    mutable std::uint64_t impostorsKey_ = 0;
    mutable std::shared_ptr<const StrandColors> impostorColors_;
    mutable std::uint64_t impostorColorsFor_ = 0;
    Vec3f center_;
    float radius_ = 0.0f;
    bool clumped_ = false;
    bool hasWidths_ = false;
    std::uint64_t groupingSerial_ = 0;
    std::uint64_t measuredSerial_ = 0;
};

}  // namespace groomlab
