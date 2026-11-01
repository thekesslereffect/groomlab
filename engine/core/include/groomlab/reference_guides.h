// SPDX-License-Identifier: Apache-2.0
#pragma once

// Guides from reference photos (a prototype): 1-3 photos of a groom, each with the camera that
// took it (camera_solve.h's pinhole: a reference camera lined up with the mesh) and a hair mask,
// give guides on the scalp that follow the hair the photos show. No learning, only image
// processing and geometry:
//
// 1. Each photo's 2D orientation field: a bank of Gabor filters (Paris et al. 2004, Chai et al.
//    2012) at a few scales gives every pixel the direction strands run in there and a confidence
//    (relative to the photo's own usual response). Outside the mask the photo is filled with the
//    hair's local mean first, so the outline is not read as strands; weak pixels inside the mask
//    take their strong neighbours' direction.
// 2. A shell around the scalp that the hair stays in: the visual hull of the masks (a voxel grid;
//    what the scalp and occluders hide counts as "could be hair behind"), and with one photo a
//    depth guessed from the silhouette (the scalp inflated: hair that reaches d past the scalp in
//    the photo is taken to reach about d in depth too).
// 3. Guides grown root to tip in steps: at each point every photo that sees it (not behind the
//    scalp or an occluder) says the strand lies in the plane through its camera and the 2D
//    direction there; the planes of all photos (weighted by confidence) and weak priors (keep going
//    the way it went; lie along the surface near it; fall with gravity where the photos say little)
//    give the 3D direction by least squares. The first direction: the axis on the scalp the
//    evidence along a short line through the root agrees with best. The 180 degree ambiguity: both
//    ways are grown; the one with more evidence, leaning toward gravity, wins, then neighbours vote.
//    A guide stops where the photos that see it say it left the hair, at the shell, or its length.
// 4. Symmetry (world X or the scalp's topology, scalp_mirror.h): every photo also judges each
//    point's mirror image, so one three-quarter view covers both sides; the guides come in
//    mirrored pairs (one grown, the other its mirror, as Mirror Group makes them).
// 5. The look: the masked pixels' color (white-balanced by the hair's highlights) as a hair
//    appearance color (hair_color.h), its base color and the nearest melanin and redness, the
//    color at the roots and at the tips.
// 6. The measured look (reference_look.cpp): clumps (the brightness's period and swing across the
//    flow), curls (the fine direction's swing along it), flyaways (thin ridges past the outline) and
//    frizz (ridges crossing the flow), tips, and where the skin shows (thinning), each turned into
//    the graph value it suggests (calibrated on a synthetic look sweep). Rough: a starting point.
//
// Validation, pictures and the numbers: tools/research/reference_guides (build/probe/fromref).

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "groomlab/camera_solve.h"
#include "groomlab/math.h"
#include "groomlab/scalp_mesh.h"

namespace groomlab {

// An image as floats, rows top to bottom, `channels` values a pixel (1 gray, 3 RGB).
struct RefImage {
    int width = 0, height = 0, channels = 0;
    std::vector<float> pixels;

    bool empty() const { return width <= 0 || height <= 0 || pixels.empty(); }
    float at(int x, int y, int c = 0) const {
        return pixels[(static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x)) *
                          static_cast<std::size_t>(channels) + static_cast<std::size_t>(c)];
    }
    // Bilinear, pixel (x, y)'s center at (x + 0.5, y + 0.5), clamped at the borders.
    float sample(float u, float v, int c = 0) const;
    // The image scaled to `width` x `height` (area average down, bilinear up).
    RefImage resized(int width, int height) const;
    // Gray (the channels' mean) as one channel.
    RefImage gray() const;
};

// An image file (as loadTexture reads it: PNG, JPEG, TGA, ...) as `channels` (1, 3 or 4: RGBA)
// floats 0-1 as stored (8-bit sRGB stays encoded). Throws TextureError.
RefImage loadRefImage(const std::string& path, int channels);

// A pinhole camera (camera_solve.h): the pose (camera axes to world, the eye), the focal length
// in pixels of an image `width` x `height`; pixels from the top-left, v down.
struct RefCamera {
    CameraPose pose;
    double focalPx = 0.0;
    double width = 0.0, height = 0.0;
    double offsetU = 0.0, offsetV = 0.0;  // the principal point's offset from the image's center, pixels (a crop's)

    Vec3f eye() const;
    Vec3f axis(int i) const;  // the camera's X (right), Y (up), Z (back: it looks down -Z) in world space
    // The point in pixels and its depth along the view; false behind the camera.
    bool project(const Vec3f& p, float& u, float& v, float* depth = nullptr) const;
    // The same camera for an image `scale` times the size.
    RefCamera scaled(double scale) const;
    // The same camera for the part of its image from (x0, y0), `w` x `h` pixels.
    RefCamera cropped(double x0, double y0, double w, double h) const;
};

// The part of an image from (x0, y0), w x h pixels (inside it).
RefImage cropImage(const RefImage& image, int x0, int y0, int w, int h);

// ---- 1. Orientation ---------------------------------------------------------------------------

struct OrientationOptions {
    int orientations = 32;        // filters over 180°
    float wavelength = 4.0f;      // pixels across a strand (the carrier's period) at the finest scale
    int scales = 3;               // octaves: the image and its halves, each filtered at `wavelength`
    float confidenceFloor = 0.2f; // pixels below this confidence take their neighbours' direction
    bool fillWeak = true;         // weak pixels take their strong neighbours' direction
    float edgeWidth = 1.0f;       // wavelengths from the mask's edge over which the confidence fades in (the outline is not strands)
    float smoothSigma = 1.5f;     // pixels: the final blur of the direction field inside the mask
};

// The strands' 2D direction at each pixel: an angle in [0, pi) from +u toward +v (v down), a
// confidence 0-1, and the smoothed field as doubled-angle vectors (c cos 2a, c sin 2a) that
// sample() reads.
struct OrientationField {
    int width = 0, height = 0;
    std::vector<float> angle;
    std::vector<float> confidence;
    std::vector<float> tensor;  // 2 a pixel, smoothed

    // The direction (unit, in pixels: u right, v down) and its confidence at (u, v); false outside.
    bool sample(float u, float v, float& du, float& dv, float& conf) const;
    // RGB8: hue the direction, brightness the confidence (a picture to check it by).
    std::vector<std::uint8_t> colors(bool smoothed = true) const;
};

// `gray`: one channel; `mask` (one channel 0-1, may be empty: everywhere) bounds the work and the
// diffusion.
OrientationField orientationField(const RefImage& gray, const RefImage& mask, const OrientationOptions& options = {});

// The field smoothed over `sigma` pixels inside the mask (a lock's flow, past its curls).
OrientationField coarseField(const OrientationField& field, const RefImage& mask, float sigma);

// Curls in a photo: along the coarse flow (streamlines), the fine direction's swing to either side of
// it, its period (autocorrelation's first peak) and its amplitude.
struct CurlEstimate {
    int streamlines = 0;         // traced
    float periodicShare = 0.0f;  // of those, the share with a clear period
    float wavelengthPx = 0.0f;   // the period along the flow (median), pixels
    float amplitude = 0.0f;      // the swing's amplitude (median), radians
};
CurlEstimate measureCurl(const OrientationField& fine, const OrientationField& coarse, const RefImage& mask);

// ---- 6-10. The measured look (reference_look.cpp) ----------------------------------------------

// Clumps and color variation: profiles of the log brightness across the flow (perpendicular to the
// coarse field, inside the mask). Clumps make it rise and fall: the period is their spacing, the
// swing how tight they are (dark gaps between locks); finer than ~2 pixels, the strands' own spread.
struct ClumpEstimate {
    int profiles = 0;            // traced
    float periodicShare = 0.0f;  // of those, the share with a clear period
    float spacingPx = 0.0f;      // the period across the flow (median), pixels
    float contrast = 0.0f;       // the clump-scale swing: std of the smoothed, detrended log brightness (median)
    float strandSpread = 0.0f;   // the strand-scale swing: std of the log brightness finer than ~2 pixels (median)
    float clumpPeak = 0.0f;      // the autocorrelation at the period (median): how regular the clumps are
    float coherentSpread = 0.0f; // the strand-scale swing that runs along the flow (a strand's own color: the same a
                                 // few pixels further along), sqrt of the fine profiles' covariance (median)
};
// `luminance`: linear, one channel, at the fields' size.
ClumpEstimate measureClumps(const RefImage& luminance, const OrientationField& coarse, const RefImage& mask);

// Flyaways and frizz: thin lines (the Hessian's ridges at a strand's scale, Frangi-like) that leave
// the hair's body (flyaways: past its outline) or cross its flow inside (frizz).
struct StrayEstimate {
    float outlinePx = 0.0f;     // the body's outline (the mask opened: thin strays off, closed)
    float strayPx = 0.0f;       // ridge pixels outside the body, within reach of it
    float strayDensity = 0.0f;  // strayPx / outlinePx
    float ridgeShare = 0.0f;    // inside the body: the share of pixels on a ridge
    float crossShare = 0.0f;    // of those, the share crossing the flow by more than 30 degrees
    float contrast = 0.0f;      // the hair's gray against the backdrop's (|median difference|): what a stray shows as
};
// `gray`: as stored (sRGB) 0-1, one channel; `near` (may be empty): where strays may be (the hair's
// reach from the scalp), one channel.
// `picture` (diagnostics): RGB8 at the field's size: the photo dimmed, the body's outline white, strays red,
// ridges inside green (crossing the flow: yellow).
StrayEstimate measureStrays(const RefImage& gray, const OrientationField& coarse, const RefImage& mask, const RefImage& near = {},
                            std::vector<std::uint8_t>* picture = nullptr);

// A Scatter density factor (0-1) from the share of the skin hair covers (scalpCoverage): hair a
// little thinner barely shows the skin, so the factor falls faster than the cover (calibrated).
float densityOfCoverage(float coverage);

// ---- 2-4. Shell and guides --------------------------------------------------------------------

// An artist's flow hint on a photo (optional): a line drawn along the hair, in the photo's pixels,
// root end first when `directed` (else only its line counts and the usual rules pick the way).
struct RefHintStroke {
    std::vector<std::array<float, 2>> points;
    bool directed = true;
};

// A touch-up of the hair mask painted on a photo (optional): a brush line in the photo's pixels,
// `radius` pixels wide on either side; `include` marks hair (sure hair for the refinement, and hair
// in the mask whatever the refinement says), else backdrop (sure backdrop, and never hair).
struct RefMaskStroke {
    std::vector<std::array<float, 2>> points;
    float radius = 8.0f;
    bool include = true;
};

struct ReferenceView {
    RefCamera camera;      // for the image's own size (the view's image may be given at any size: scaled)
    RefImage image;        // RGB (or gray) 0-1, as stored
    RefImage mask;         // one channel 0-1; empty: everything not the image's corner color (a plain backdrop)
    std::vector<RefMaskStroke> maskStrokes;  // touch-ups over the mask (given or automatic), photo pixels
    std::shared_ptr<const OrientationField> field;  // given (tests): used as it is, at the mask's size
    // Flow hints (optional, never required): strokes, and/or an overlay painted in any image editor
    // (RGBA at the photo's size: the strokes opaque on a transparent layer; each stroke's way read
    // from a brightness ramp along it, dark at the root end and light at the tip, where it has one,
    // else only its line). Near a hint its line replaces the photo's directions (blended out over
    // ReferenceGuideOptions::hintLineRadius), and its way decides which way guides leave their roots
    // (over hintRadius).
    std::vector<RefHintStroke> hints;
    RefImage hintOverlay;  // 4 channels
};

enum class ReferenceSymmetry { None, WorldX, Topology, Auto };

// How the look fit takes the light's color out (ReferenceLookFit).
enum class LookBalance { None, Gray, Highlights, Auto };

struct ReferenceGuideOptions {
    int guideCount = 300;               // roots spread over the scalp (blue noise); fewer survive (bald, unseen)
    std::vector<StrandRoot> roots;      // these roots instead
    float maxLength = 0.0f;             // 0: automatic, how far the photos show the hair reaching past the scalp (1.2
                                        // times its 90th percentile; the scalp's size without one); below 0:
                                        // -maxLength times the rim the silhouettes show past the scalp
    float minLength = 0.0f;             // shorter guides are dropped; 0: three steps
    float lengthTrim = 0.86f;           // a guide that reaches a given maxLength (its tip never seen) is cut to this share
    float step = 0.0f;                  // 0: maxLength / 80, at least ~1.5 pixels in the views
    int points = 16;                    // points a guide
    float scalpOffset = 0.0f;           // kept above the scalp; 0: 0.4% of the scalp's size
    ReferenceSymmetry symmetry = ReferenceSymmetry::Auto;
    float symmetryCenter = 0.0f;        // the world X plane (WorldX)
    int maxImageSize = 768;             // images are worked on at most this many pixels across (their subject's part)
    bool cropToSubject = true;          // work on the part of each photo the scalp and its hair can reach (finer detail)
    int subjectSize = 512;              // a subject photographed at twice this size or more (its strands resolved) is
                                        // worked at this size (0: never): the orientation filters work in pixels and
                                        // read the strands' texture there, not the hair's flow (the wolf photo, 1253
                                        // pixels: IoU 0.62 -> 0.65); from 1.5x to 2x it blends in, smaller is kept
                                        // (the synthetic heads, ~650 pixels, lose 1-4 degrees at 512)
    int refineMask = -1;                // refine the mask by color (GrabCut-like): -1 automatic masks only, 0 never, 1 always
    OrientationOptions orientation;
    std::vector<std::shared_ptr<const ScalpMesh>> occluders;  // more surfaces hiding hair (head, body)
    int shellResolution = 96;           // voxels along the longest side
    int inflate = -1;                   // single-view depth bound: -1 when there is one view, 0 off, 1 on
    float inflateFactor = 1.0f;         // depth allowed per unit the silhouette reaches past the scalp
    float surfacePrior = 0.15f;         // weight of "lie along the surface" (one view decides depth by it)
    float continuityPrior = 0.1f;     // weight of "keep going the same way"
    float gravityPrior = 0.15f;        // weight of "flow down along the surface" (where the views say little)
    float orientationWeight = 1.0f;     // how much the photos' strand directions count (0: priors and silhouettes only)
    float downBias = 1.0f;              // the way a guide leaves its root: toward gravity (hair hangs, fur lies back and down)
    float hintRadius = 0.08f;           // the reach of a hint stroke's way (which way guides leave their roots), a share
                                        // of the photo's larger side
    float hintLineRadius = 0.025f;      // the reach of its line over the photo's own directions (narrower: a stroke is
                                        // coarser than the strands the photo shows)
    float hintWeight = 3.0f;            // how strongly directed hints choose the way guides leave their roots
    int curlFlow = -1;                  // guides follow the locks' coarse flow, not each curl: -1 when curls are seen, 0, 1
    Vec3f up{0.0f, 1.0f, 0.0f};         // gravity is -up
    std::uint64_t seed = 1;
    bool keepFields = false;            // return the orientation fields (diagnostics)
    LookBalance lookBalance = LookBalance::Auto;  // the look: how the light's color is taken out
    float lookQuantile = 0.65f;         // the look: which share of the hair's pixels is darker than its color (the lit side above 0.5)
    bool lookExposure = true;           // the look: the photo's white point taken as white (a dim photo brightened)
};

struct ReferenceGuide {
    std::vector<Vec3f> points;   // root first
    StrandRoot root;
    int mirrorOf = -1;           // the guide this one mirrors (symmetry), else -1
    float confidence = 0.0f;     // the evidence's mean confidence along it
    std::string stop;            // why it ended: "length", "mask", "shell", "unseen", "scalp"
};

struct ReferenceLookFit {
    bool ok = false;
    int samples = 0;
    std::array<float, 3> appearance{0, 0, 0};      // linear RGB: the hair's median color
    std::array<float, 3> baseColor{0, 0, 0};       // hairBaseOfAppearance(appearance)
    float melanin = 0.0f, redness = 0.0f;          // the melanin color nearest it
    float melaninError = 0.0f;                     // the fit's RMS (log appearance)
    float valueRandomness = 0.0f;                  // the spread of brightness (std / mean of luminance)
    std::array<float, 3> rootAppearance{0, 0, 0};  // along the guides: their first third
    std::array<float, 3> tipAppearance{0, 0, 0};   // their last third
    std::array<float, 3> whiteBalance{1, 1, 1};    // what the colors above were multiplied by (light and exposure out)
    float exposure = 1.0f;                         // the exposure part of it
    // Curls (measureCurl over the views): curly when a clear share of the flow swings with a period;
    // then a Curl node's frequency (turns per unit of length) and radius, in scene units.
    bool curly = false;
    float curlShare = 0.0f, curlAmplitudeDeg = 0.0f;
    float curlFrequency = 0.0f, curlRadius = 0.0f;
    // The measured look (6-10), each with the graph value it suggests (calibrated on the synthetic
    // look sweep, tools/research/reference_guides/look_sweep.py). Clumps (measureClumps): their spacing
    // across the flow (scene units) and the profile's swing -> a Clump's size (clumpBy size) and strength.
    bool clumped = false;
    float clumpSpacing = 0.0f, clumpContrast = 0.0f, clumpRegularity = 0.0f;
    float clumpSize = 0.0f, clumpStrength = 0.0f;
    // Flyaways and frizz (measureStrays) -> a Flyaway's fraction, a frizz Noise's fraction (0: none).
    float strayDensity = 0.0f, crossShare = 0.0f;
    float flyawayFraction = 0.0f, frizzFraction = 0.0f;
    // Color variation: the strands' and the clumps' spread of brightness -> HairColor's melaninRandom
    // and clumpShare; the tips (lighter or darker than the roots) -> tipColor (base color) and tipStrength.
    float strandSpread = 0.0f, coherentSpread = 0.0f;
    float melaninRandom = 0.0f, clumpShare = 0.5f;
    float tipStrength = 0.0f;
    std::array<float, 3> tipColor{0.55f, 0.42f, 0.28f};
    // Density: the share of the scalp the views see that hair covers (ReferenceGuidesResult::scalpCoverage).
    float coverage = 0.0f;
};

struct ReferenceGuidesResult {
    std::vector<ReferenceGuide> guides;
    ReferenceSymmetry symmetry = ReferenceSymmetry::None;  // the one used (Auto resolved)
    int rootsTried = 0;
    int rootsUnseen = 0;   // no view (or mirror) sees them
    int rootsBald = 0;     // seen, but no hair over them
    int rootsShort = 0;    // grown shorter than minLength
    float step = 0.0f, maxLength = 0.0f, scalpOffset = 0.0f;
    float shellFill = 0.0f;  // the shell's share of its grid
    float rimLength = 0.0f;  // how far the hair reaches past the scalp's outline in the views (scene units, its rim)
    float reachLength = 0.0f;  // the same, its 90th percentile (what an automatic length starts from)
    ReferenceLookFit look;
    // Each scalp vertex: the share of it hair covers in the views that see it face on (0-1; -1 unseen):
    // a Scatter density map (thinning) once mapped (densityOfCoverage).
    std::vector<float> scalpCoverage;
    std::vector<OrientationField> fields;  // keepFields: one a view, at the working size
    std::vector<RefImage> masks;           // keepFields: the masks used, at the working size
    std::vector<std::array<int, 4>> crops; // keepFields: the part of each photo they cover (x0, y0, w, h; photo pixels)
    double seconds = 0.0;
};

// Whether the scalp is symmetric: in topology (its mirror pairs most vertices: ScalpMirror), or in
// shape (nearly all its points mirrored across its middle land on its surface: a sculpted or morphed
// head, the mirror's partners then its nearest spots).
bool scalpLooksSymmetric(const ScalpMesh& scalp);

ReferenceGuidesResult guidesFromReference(const ScalpMesh& scalp, const std::vector<ReferenceView>& views,
                                          const ReferenceGuideOptions& options = {});

// What a host's Reference window shows over one photo, as guidesFromReference would work on it
// (the same crop, working size, mask and orientation field): the hair mask (automatic and refined,
// or the given one, the touch-up strokes applied) at the working size, the part of the photo it
// covers, and the orientation field as short line segments in the photo's pixels (one every
// `lineSpacing` photo pixels where the field is confident inside the mask: x0, y0, x1, y1).
struct ReferenceViewPreview {
    RefImage mask;                    // one channel 0-1, `crop`'s part of the photo at the working size
    std::array<int, 4> crop{0, 0, 0, 0};  // x0, y0, w, h in photo pixels
    std::vector<std::array<float, 4>> flowLines;
};
ReferenceViewPreview previewReferenceView(const ScalpMesh& scalp, const ReferenceView& view,
                                          const ReferenceGuideOptions& options = {}, float lineSpacing = 0.0f);

// The shell alone (tests, diagnostics): whether points lie in it, for these views at their sizes
// (masks given), the scalp and occluders.
class ReferenceShell {
public:
    ReferenceShell(const ScalpMesh& scalp, const std::vector<ReferenceView>& views, const ReferenceGuideOptions& options,
                   float maxLength);
    bool inside(const Vec3f& p) const;
    float fill() const;  // inside voxels / all
    const Vec3f& lo() const { return lo_; }
    float cell() const { return cell_; }

private:
    friend struct ReferenceShellBuilder;
    ReferenceShell() = default;
    Vec3f lo_;
    float cell_ = 1.0f;
    int nx_ = 0, ny_ = 0, nz_ = 0;
    std::vector<std::uint8_t> voxels_;
};

const char* referenceSymmetryName(ReferenceSymmetry s);
ReferenceSymmetry referenceSymmetryFromName(const std::string& name);  // none, x, topology, auto; throws

}  // namespace groomlab
