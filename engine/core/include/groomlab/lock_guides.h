// SPDX-License-Identifier: Apache-2.0
#pragma once

// Lock guides, as an artist blocks a haircut in: a few key guides where the locks are, drawn as
// strokes on the lined-up photos, and the rest of the scalp combed and filled in from them. Round
// a head (HeadFrame) and the hair's shell over it (hair_shell.h):
//
// - strokesFromImage: strokes drawn in one color on an image of a photo (a screenshot, an overlay)
//   as lines in the photo's pixels, root first (the end with a dot, more stroke round it).
// - liftStroke: a stroke on a photo to a 3D guide: each point's ray through the photo's camera to
//   the hair's shell (the first point in it; a ray that misses it takes the point it passes
//   nearest it, no deeper than the head's middle), the depth smoothed along the stroke (rays that
//   graze the shell would zig-zag), the root on the scalp under its first point.
// - combDirection / combGuide: a combing field and a guide grown root out along it: away from a
//   part line and back over the top, the sides back over the ears, the back falling; the guide
//   rides the shell (rising off the scalp first, the forehead's hairline lifting up and back),
//   an S-wave across the flow (in step with its neighbours, so locks band together) and its tip
//   flipping out. Everything is scaled by the head's radius.
// - fillGuide: a guide at a root from its nearest key guides (their shapes from their roots,
//   blended by distance), mixed into the combed guide where no key is near, kept off the head.
// - clippedToPhotos: a guide cut where the photos show the hair end.

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "groomlab/hair_shell.h"

namespace groomlab {

// ---- Strokes drawn on an image --------------------------------------------------------------------

struct StrokeImageOptions {
    std::array<float, 3> color{1.0f, 0.0f, 0.0f};  // the strokes' color, 0-1 as stored
    float tolerance = 0.34f;                       // each channel this close to the color
    float minSaturation = 0.43f;                   // and the pixel this colorful (its largest channel less its smallest)
    int minPixels = 0;                             // smaller pieces are left out (0: 1/20000 of the image, at least 12)
    float smooth = 3.0f;                           // the traced path smoothed over this many image pixels
    float spacing = 4.0f;                          // image pixels between the points kept
    std::string root = "dot";                      // the root end: "dot" (more stroke round it), "top", "bottom"
    // Where the photo is in the image: photo pixel = (image pixel - offset) / scale. Stroke pixels
    // outside the photo (past a few pixels) are left out. photoWidth / Height 0: the image's / scale.
    float scale = 1.0f;
    std::array<float, 2> offset{0.0f, 0.0f};
    int photoWidth = 0, photoHeight = 0;
};

struct ImageStroke {
    std::vector<std::array<float, 2>> points;  // photo pixels, root first
    int pixels = 0;                            // its pixels in the image
    int rootBlob = 0, tipBlob = 0;             // stroke pixels round each end
};

// The strokes of `options.color` in `image` (RGB or RGBA, 0-1), each an ordered line.
std::vector<ImageStroke> strokesFromImage(const RefImage& image, const StrokeImageOptions& options = {});

// ---- Strokes lifted onto the shell ------------------------------------------------------------------

struct StrokeLiftOptions {
    float spacingPx = 0.0f;     // the stroke resampled every this many photo pixels (0: 0.6% of its larger side)
    float smoothPx = 0.0f;      // the depth smoothed along it over this many pixels (0: 2% of the larger side)
    float pointSpacing = 0.0f;  // the guide's points this far apart (0: a 20th of the head's radius)
};

struct LiftedStroke {
    std::vector<Vec3f> points;  // root first (on the scalp)
    int hits = 0, misses = 0;   // stroke samples whose ray met the shell, and those that passed it
    float length = 0.0f;
};

// `pixels`: the stroke in the photo's pixels, root first (2 or more); `camera` the photo's (at its
// size); `scalp` where the root goes (the hair's region).
LiftedStroke liftStroke(const RefCamera& camera, const std::vector<std::array<float, 2>>& pixels, const HairShell& shell,
                        const ScalpMesh& scalp, const StrokeLiftOptions& options = {});

// A guide's mirror image across the plane through `planePoint` square to `normal`, moved so its
// root is on `scalp` (when given).
std::vector<Vec3f> mirroredGuide(const std::vector<Vec3f>& points, const Vec3f& planePoint, const Vec3f& normal, const ScalpMesh* scalp);

// The guide cut where the shell's photos show the hair end: before the first two points in a row
// they show bare (a point counts as bare when it, and the point a tenth of the head's radius
// further in, are both seen off the hair), past its first 0.15 head radii. At least `keep` points
// stay. Unchanged without photos.
std::vector<Vec3f> clippedToPhotos(const std::vector<Vec3f>& points, const HairShell& shell, std::size_t keep = 3);

float guideLength(const std::vector<Vec3f>& points);
std::vector<Vec3f> resampledGuide(const std::vector<Vec3f>& points, std::size_t count);    // evenly along it
std::vector<Vec3f> resampledBySpacing(const std::vector<Vec3f>& points, float spacing);   // at least 2 points

// ---- Combing ------------------------------------------------------------------------------------------

// The part: a plane (through `point`, square to `normal`); hair sweeps away from it to the side its
// normal points to and the other way on the other side. Off: no part (the front sweeps back).
struct PartLine {
    bool on = false;
    Vec3f point, normal;
};

enum class FrontSweep { Back, Out, Forward };  // the front: back over the top, out to the sides, forward (a fringe)
enum class SideSweep { Back, Down };           // the sides: back over the ears, or down

// Lengths by part of the head (headRegion), scene units; 0: the default share of the head's radius
// (top 1.6, front 1.7, sides 1.1, back 2.3, nape 0.7).
struct RegionLengths {
    float top = 0.0f, front = 0.0f, sides = 0.0f, back = 0.0f, nape = 0.0f;
    float of(const std::string& region, float headRadius) const;
};

struct CombOptions {
    PartLine part;
    FrontSweep front = FrontSweep::Back;
    SideSweep sides = SideSweep::Back;
    RegionLengths lengths;
    float lengthRandom = 0.2f;   // each guide's length times 1 +- this
    float ride = 0.8f;           // the guides ride at this share of the shell's thickness off the head
    float amplitude = -1.0f;     // the S-wave across the flow, scene units (below 0: 0.08 head radii)
    float wavelength = 0.0f;     // its period along the guide (0: half a head radius)
    float coherence = 0.8f;      // 1: every guide's wave in step (locks band together), 0: random phases
    float flip = -1.0f;          // the tip's flip out and up, scene units (below 0: 0.12 head radii)
    float rise = 1.0f;           // how far roots high on the head rise off it before lying down (0-1)
    float step = 0.0f;           // the guide's points this far apart (0: a 20th of the head's radius)
    bool clip = true;            // cut where the shell's photos show the hair end
    std::uint64_t seed = 1;
};

// The combing direction at `p` (unit, along the head's surface there).
Vec3f combDirection(const HeadFrame& frame, const Vec3f& p, const CombOptions& options);
// A root's length: its region's (options.lengths), times 1 +- lengthRandom by `seed`.
float combLength(const HeadFrame& frame, const Vec3f& root, const CombOptions& options, std::uint64_t seed);
// A guide grown from `root` (on the scalp) about `length` long, root first.
std::vector<Vec3f> combGuide(const Vec3f& root, float length, const HairShell& shell, const CombOptions& options, std::uint64_t seed);

// ---- Filling in from key guides -------------------------------------------------------------------------

struct FillOptions {
    float reach = 0.0f;    // how far a key's shape carries (exp(-(d / reach)^2)); 0: 0.4 head radii
    int points = 40;       // the keys and the combed guide resampled to this many points
    float keepOut = 0.0f;  // kept this far off the head (0: 0.04 head radii)
    int nearest = 3;       // keys blended
};

struct FilledGuide {
    std::vector<Vec3f> points;
    float keyShare = 0.0f;  // how much the keys decided it (0: the combed guide, 1: the keys)
};

// `keys`: key guides each resampled to options.points points; `combed`: this root's combed guide
// (resampled likewise), what it is where no key is near.
FilledGuide fillGuide(const Vec3f& root, const std::vector<std::vector<Vec3f>>& keys, const std::vector<Vec3f>& combed, const HairShell& shell,
                      const FillOptions& options = {});

}  // namespace groomlab
