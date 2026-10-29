// SPDX-License-Identifier: Apache-2.0
#pragma once

// Brush alphas: a grayscale image stamped inside the brush circle while painting a scalp map
// (Paint on a vertex or texture map), so a dab leaves the image's pattern (speckles, noise, streaks)
// rather than a round spot. The dab's weight is the falloff times the alpha where a point lands
// in the circle: white paints fully, black not at all. The image fills the square around the
// circle, turned by the dab's angle (stroke.h: fixed, along the stroke, or random per dab).
//
// The library: every folder on GROOMLAB_ALPHA_PATH (the OS path separator between them), the
// user's folder (brushAlphaUserDir: Documents/GroomLab/alphas, shared by every host), then the
// folder shipped with the host (setShippedBrushAlphaDir). A file in an earlier folder hides one of
// the same name in a later one. Any image loadTexture reads is an alpha (no UDIM sets).

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace groomlab {

class Texture;

class BrushAlpha {
public:
    // The image's alpha channel when it has one that is not opaque everywhere (a shape on a
    // transparent background), else its luminance (Rec. 709). Throws std::invalid_argument for a
    // tile set or an empty image.
    BrushAlpha(const Texture& texture, std::string path = {});
    // `values` width x height, rows top-down, each 0..1.
    BrushAlpha(int width, int height, std::vector<float> values, std::string path = {});

    int width() const { return levels_.front().width; }
    int height() const { return levels_.front().height; }
    const std::string& path() const { return path_; }

    // The alpha at (u, v) in the square around the brush circle: -1..1 each, u to the right, v up
    // (the image's top row at v = 1); 0 outside the square. Bilinear, from the mip level whose
    // texels are about a screen pixel when the circle is `radiusPixels` (no shimmering speckle at
    // a small brush).
    float sample(float u, float v, float radiusPixels) const;
    // Every value of `level` (0 the image), rows top-down.
    int levelCount() const { return static_cast<int>(levels_.size()); }
    int levelFor(float radiusPixels) const;

    // The image as a square thumbnail of `size` pixels (gray, 8 bits, rows top-down): white full
    // weight, black none. What the hosts' alpha pickers show.
    std::vector<std::uint8_t> thumbnail(int size) const;

private:
    struct Level {
        int width = 0, height = 0;
        std::vector<float> values;
    };
    void buildLevels();
    float sampleLevel(const Level& level, float u, float v) const;

    std::vector<Level> levels_;
    std::string path_;
};

using BrushAlphaPtr = std::shared_ptr<const BrushAlpha>;

// An alpha as one dab stamps it: turned by `angle` (radians, counterclockwise on screen), and
// inverted (1 - the alpha) or not. Empty (no alpha): every point weighs 1.
struct AlphaStamp {
    BrushAlphaPtr alpha;
    float angle = 0.0f;
    bool invert = false;
    explicit operator bool() const { return alpha != nullptr; }
    // The weight at (dx, dy) pixels from the dab's center (y up) in a circle of `radius` pixels.
    float at(float dx, float dy, float radius) const;
};

// The alpha of an image file, through loadTexture's cache (a rewritten file is read again).
// Throws TextureError (a file it cannot read) or std::invalid_argument.
BrushAlphaPtr loadBrushAlpha(const std::string& path);

// ---- The library ------------------------------------------------------------------------------

struct BrushAlphaFile {
    std::string name;      // the file's stem: "Speckle"
    std::string path;      // forward slashes
    bool shipped = false;  // from the host's folder (not the user's own)
};

// The user's alpha folder: $GROOMLAB_USER_ALPHAS when set, else Documents/GroomLab/alphas (Windows:
// the profile's; elsewhere $HOME's). "" without a home. Made by the first addBrushAlpha.
std::string brushAlphaUserDir();
// The folder of alphas shipped with this host ("" none).
void setShippedBrushAlphaDir(const std::string& dir);
std::string shippedBrushAlphaDir();
// GROOMLAB_ALPHA_PATH's folders, brushAlphaUserDir(), shippedBrushAlphaDir(): where alphas are found.
std::vector<std::string> brushAlphaDirs();
// Every alpha in brushAlphaDirs() (image files; a name in an earlier folder hides a later one's),
// the user's before the shipped ones, each part sorted by name.
std::vector<BrushAlphaFile> listBrushAlphas();
// Copies an image into the user's folder (made when missing) under its own name, " 2", " 3"...
// when an alpha has that name already (a shipped one too: the copy would hide it); an image in
// one of the library's folders is not copied again.
// Returns the alpha's path in the library. Throws std::runtime_error (not an image it can read,
// no user folder, the copy failed).
std::string addBrushAlpha(const std::string& file);

}  // namespace groomlab
