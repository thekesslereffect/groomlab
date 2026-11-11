// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

namespace groomlab {

class TextureError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// An image as float RGBA, sampled with UVs for map parameters (PG-2).
// 8-bit files keep their stored values (0-255 -> 0-1, no color conversion:
// maps are data); 16-bit PNGs and PNMs keep their precision (0-65535 -> 0-1);
// .hdr files load as linear floats. Paths are UTF-8 on every platform.
//
// UDIM: a path naming a tile set ("<UDIM>", "<udim>" or "%(UDIM)d" where the
// tile number goes, or a "1001"-style tile number in the file name when other
// tiles sit beside it) samples tile 1001 + floor(u) + 10 * floor(v) from its
// own file, read the first time it is sampled and kept. UVs off the 10 x 10
// tile grid, or on a tile with no file, sample transparent black; a tile
// that fails to read counts as missing (see error()). Without UDIM, UVs repeat.
class Texture {
public:
    struct Image {
        int width = 0;
        int height = 0;
        std::vector<float> rgba;
        // The file's bits per channel: 8 or 16, 32 for floats (HDR); 0 for an image made in memory.
        int bits = 0;
        std::array<float, 4> texel(int x, int y) const;  // repeat wrapping
        // Bilinear sample with repeat wrapping. v = 0 is the bottom row,
        // matching Maya's UV space (image rows are stored top-down).
        std::array<float, 4> sample(float u, float v) const;
    };

    Texture(int width, int height, std::vector<float> rgba, int bits = 0);  // one image (bits: Image::bits)
    explicit Texture(std::string udimPattern);                // tiles on demand; the pattern holds "<UDIM>"
    ~Texture();
    Texture(const Texture&) = delete;
    Texture& operator=(const Texture&) = delete;

    static constexpr int kTiles = 100;  // a tile set's tiles: 1001-1100, u and v in [0, 10)

    bool isUdim() const { return !udimPattern_.empty(); }
    // The image (null for a tile set).
    const Image* image() const { return image_.get(); }
    // The image's size; a tile set's is tile 1001's once read (0 before).
    int width() const;
    int height() const;

    std::array<float, 4> sample(float u, float v) const;

    // Memory held now: the image, or the tiles read so far.
    std::size_t bytes() const;
    // Why the last tile failed to read (empty when none did).
    std::string error() const;

private:
    const Image* tile(int index) const;

    std::shared_ptr<const Image> image_;
    std::string udimPattern_;
    mutable std::mutex tileLock_;
    mutable std::array<std::atomic<const Image*>, kTiles> tileCache_{};  // read without the lock once set
    mutable std::vector<std::unique_ptr<const Image>> tileStorage_;
    mutable std::atomic<std::size_t> tileBytes_{0};
    mutable std::string error_;
};

using TexturePtr = std::shared_ptr<const Texture>;

// Loads a texture (or names a UDIM tile set) through a process-wide cache
// keyed by path and file stamp, so a rewritten file is re-read. The cache
// keeps the most recently used textures within a byte budget (1 GiB, or
// GROOMLAB_TEXTURE_CACHE_MB). Formats: PNG (8 and 16 bit), JPEG, TGA, BMP,
// PSD, GIF, HDR, PIC, PNM (8 and 16 bit). Throws TextureError. Thread-safe.
TexturePtr loadTexture(const std::string& path);

// The texture loadTexture gives for an 8-bit image file of these pixels (`channels` 1 gray,
// 2 gray + alpha, 3 RGB, 4 RGBA per texel, rows top-down): each value / 255, gray in r g b,
// alpha 1 where there is none. Throws TextureError on a bad size.
TexturePtr textureFromPixels(int width, int height, int channels, const std::vector<std::uint8_t>& pixels);

// Puts `texture` in loadTexture's cache as the file `path` with the stamp `stamp` (fileStamp of
// the file as written), for a host that just wrote the file and knows what reading it gives
// (textureFromPixels of the pixels it wrote): the next load does not read the file again. A
// UDIM path is refused (false).
bool primeTextureCache(const std::string& path, std::uint64_t stamp, TexturePtr texture);
// The cached texture of `path` when the cache holds it at the file's stamp now; null otherwise
// (nothing is read).
TexturePtr cachedTexture(const std::string& path);

// Whether a name is an image file's (rather than a map's or a color set's): it ends in an image
// extension loadTexture reads (.png, .jpg, .jpeg, .tga, .bmp, .psd, .gif, .hdr, .pic, .pnm, .ppm,
// .pgm, any case), or names a UDIM tile set (<UDIM>). Nothing is read.
bool isImagePath(const std::string& name);
// Whether the file name ends in .png (any case). Painting a texture map writes PNG bytes to its
// file, so a map is painted only into a .png: a map linked to a JPEG, TGA, PSD... paints a PNG
// copy, never the source.
bool isPngPath(const std::string& path);
// Whether every texel's r, g and b agree (a gray map's image); false for a UDIM set.
bool isGrayImage(const Texture& image);

// A cheap hash of the file's path, size and modification time (0 if missing), for
// cache keys of anything that reads the file. For a UDIM path, of every tile
// that exists (0 when none does).
std::uint64_t fileStamp(const std::string& path);

// While one is alive on a thread, fileStamp there asks the file system once per path and then
// answers from memory: one graph evaluation (Graph::evaluateOutputs holds one) reads each
// file's stamp once for all its cache keys and texture loads. Nested scopes share the
// outermost's answers; files rewritten during the scope are seen by the next one.
class ScopedFileStamps {
public:
    ScopedFileStamps();
    ~ScopedFileStamps();
    ScopedFileStamps(const ScopedFileStamps&) = delete;
    ScopedFileStamps& operator=(const ScopedFileStamps&) = delete;

private:
    bool owner_ = false;
};

}  // namespace groomlab
