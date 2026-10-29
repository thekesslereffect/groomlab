// SPDX-License-Identifier: Apache-2.0
#include "groomlab/brush_alpha.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <stdexcept>

#include "groomlab/paths.h"
#include "groomlab/texture.h"

namespace groomlab {

namespace fs = std::filesystem;

namespace {

std::vector<std::string> splitPathList(const std::string& list) {
#ifdef _WIN32
    const char separator = ';';
#else
    const char separator = ':';
#endif
    std::vector<std::string> out;
    std::stringstream in(list);
    std::string dir;
    while (std::getline(in, dir, separator)) {
        if (dir.find_first_not_of(" \t") != std::string::npos) out.push_back(dir);
    }
    return out;
}

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::mutex& shippedMutex() {
    static std::mutex m;
    return m;
}

std::string& shippedDir() {
    static std::string dir;
    return dir;
}

// Whether two folders are one (as the file system sees them, when both exist).
bool sameFolder(const fs::path& a, const fs::path& b) {
    std::error_code ec;
    const bool same = fs::equivalent(a, b, ec);
    return !ec && same;
}

}  // namespace

BrushAlpha::BrushAlpha(const Texture& texture, std::string path) : path_(std::move(path)) {
    const Texture::Image* image = texture.image();
    if (!image || image->width < 1 || image->height < 1) throw std::invalid_argument("a brush alpha needs one image (not a tile set)");
    const std::size_t n = static_cast<std::size_t>(image->width) * static_cast<std::size_t>(image->height);
    bool transparent = false;
    for (std::size_t i = 0; i < n && !transparent; ++i) transparent = image->rgba[4 * i + 3] < 0.999f;
    Level level;
    level.width = image->width;
    level.height = image->height;
    level.values.resize(n);
    for (std::size_t i = 0; i < n; ++i) {
        const float* p = &image->rgba[4 * i];
        const float v = transparent ? p[3] : 0.2126f * p[0] + 0.7152f * p[1] + 0.0722f * p[2];
        level.values[i] = std::clamp(v, 0.0f, 1.0f);
    }
    levels_.push_back(std::move(level));
    buildLevels();
}

BrushAlpha::BrushAlpha(int width, int height, std::vector<float> values, std::string path) : path_(std::move(path)) {
    if (width < 1 || height < 1 || values.size() != static_cast<std::size_t>(width) * static_cast<std::size_t>(height)) {
        throw std::invalid_argument("a brush alpha needs width x height values");
    }
    for (float& v : values) v = std::clamp(v, 0.0f, 1.0f);
    levels_.push_back({width, height, std::move(values)});
    buildLevels();
}

void BrushAlpha::buildLevels() {
    // Halved (2 x 2 boxes, an odd edge clamped) down to a texel or two.
    while (levels_.back().width > 1 || levels_.back().height > 1) {
        const Level& from = levels_.back();
        Level next;
        next.width = std::max(1, from.width / 2);
        next.height = std::max(1, from.height / 2);
        next.values.resize(static_cast<std::size_t>(next.width) * static_cast<std::size_t>(next.height));
        for (int y = 0; y < next.height; ++y) {
            for (int x = 0; x < next.width; ++x) {
                float sum = 0.0f;
                for (int dy = 0; dy < 2; ++dy) {
                    for (int dx = 0; dx < 2; ++dx) {
                        const int sx = std::min(2 * x + dx, from.width - 1), sy = std::min(2 * y + dy, from.height - 1);
                        sum += from.values[static_cast<std::size_t>(sy) * from.width + sx];
                    }
                }
                next.values[static_cast<std::size_t>(y) * next.width + x] = 0.25f * sum;
            }
        }
        levels_.push_back(std::move(next));
    }
}

int BrushAlpha::levelFor(float radiusPixels) const {
    // Texels per screen pixel across the circle's square (2 x radius pixels).
    const float perPixel = static_cast<float>(std::max(width(), height())) / std::max(2.0f * radiusPixels, 1.0f);
    if (perPixel <= 1.0f) return 0;
    return std::min(static_cast<int>(std::floor(std::log2(perPixel))), levelCount() - 1);
}

float BrushAlpha::sampleLevel(const Level& level, float u, float v) const {
    // The square's edges are the image's outer texel edges; samples clamp to the edge texels.
    const float fx = (u + 1.0f) * 0.5f * static_cast<float>(level.width) - 0.5f;
    const float fy = (1.0f - v) * 0.5f * static_cast<float>(level.height) - 0.5f;
    const float x0f = std::floor(fx), y0f = std::floor(fy);
    const float tx = fx - x0f, ty = fy - y0f;
    const auto at = [&](int x, int y) {
        x = std::clamp(x, 0, level.width - 1);
        y = std::clamp(y, 0, level.height - 1);
        return level.values[static_cast<std::size_t>(y) * level.width + x];
    };
    const int x0 = static_cast<int>(x0f), y0 = static_cast<int>(y0f);
    const float top = at(x0, y0) + (at(x0 + 1, y0) - at(x0, y0)) * tx;
    const float bottom = at(x0, y0 + 1) + (at(x0 + 1, y0 + 1) - at(x0, y0 + 1)) * tx;
    return top + (bottom - top) * ty;
}

float BrushAlpha::sample(float u, float v, float radiusPixels) const {
    if (!(u >= -1.0f && u <= 1.0f && v >= -1.0f && v <= 1.0f)) return 0.0f;
    return sampleLevel(levels_[static_cast<std::size_t>(levelFor(radiusPixels))], u, v);
}

std::vector<std::uint8_t> BrushAlpha::thumbnail(int size) const {
    size = std::max(size, 1);
    // The level a little larger than the thumbnail, sampled at each pixel's center.
    const float radius = 0.5f * static_cast<float>(size);
    std::vector<std::uint8_t> out(static_cast<std::size_t>(size) * static_cast<std::size_t>(size));
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            const float u = (static_cast<float>(x) + 0.5f) / radius - 1.0f;
            const float v = 1.0f - (static_cast<float>(y) + 0.5f) / radius;
            const float a = sample(u, v, radius);
            out[static_cast<std::size_t>(y) * size + x] = static_cast<std::uint8_t>(std::lround(std::clamp(a, 0.0f, 1.0f) * 255.0f));
        }
    }
    return out;
}

float AlphaStamp::at(float dx, float dy, float radius) const {
    if (!alpha) return 1.0f;
    // The point turned back by the angle: the image turns by it.
    const float c = std::cos(angle), s = std::sin(angle), r = std::max(radius, 1e-6f);
    const float a = alpha->sample((c * dx + s * dy) / r, (c * dy - s * dx) / r, radius);
    return invert ? 1.0f - a : a;
}

BrushAlphaPtr loadBrushAlpha(const std::string& path) {
    // Kept per path, while the texture loadTexture gives is the same one (a rewritten file is not).
    struct Entry {
        TexturePtr texture;
        BrushAlphaPtr alpha;
    };
    static std::mutex mutex;
    static std::map<std::string, Entry> cache;
    TexturePtr texture = loadTexture(path);
    std::lock_guard<std::mutex> lock(mutex);
    Entry& entry = cache[path];
    if (entry.texture != texture || !entry.alpha) {
        entry.alpha = std::make_shared<const BrushAlpha>(*texture, path);
        entry.texture = std::move(texture);
    }
    return entry.alpha;
}

// ---- The library ------------------------------------------------------------------------------

std::string brushAlphaUserDir() {
    if (const std::optional<std::string> own = environmentVariable("GROOMLAB_USER_ALPHAS"); own && !own->empty()) return *own;
#ifdef _WIN32
    const std::optional<std::string> home = environmentVariable("USERPROFILE");
#else
    const std::optional<std::string> home = environmentVariable("HOME");
#endif
    if (!home || home->empty()) return {};
    return (fs::u8path(*home) / "Documents" / "GroomLab" / "alphas").generic_u8string();
}

void setShippedBrushAlphaDir(const std::string& dir) {
    std::lock_guard<std::mutex> lock(shippedMutex());
    shippedDir() = dir;
}

std::string shippedBrushAlphaDir() {
    std::lock_guard<std::mutex> lock(shippedMutex());
    return shippedDir();
}

std::vector<std::string> brushAlphaDirs() {
    std::vector<std::string> dirs;
    if (const std::optional<std::string> path = environmentVariable("GROOMLAB_ALPHA_PATH")) dirs = splitPathList(*path);
    if (const std::string user = brushAlphaUserDir(); !user.empty()) dirs.push_back(user);
    if (const std::string shipped = shippedBrushAlphaDir(); !shipped.empty()) dirs.push_back(shipped);
    return dirs;
}

std::vector<BrushAlphaFile> listBrushAlphas() {
    const std::string shipped = shippedBrushAlphaDir();
    std::vector<BrushAlphaFile> own, ours;
    std::set<std::string> seen;
    for (const std::string& dir : brushAlphaDirs()) {
        const bool isShipped = !shipped.empty() && (dir == shipped || sameFolder(fs::u8path(dir), fs::u8path(shipped)));
        std::vector<fs::path> files;
        std::error_code ec;
        for (fs::directory_iterator it(fs::u8path(dir), ec), end; !ec && it != end; it.increment(ec)) {
            const std::string name = it->path().filename().u8string();
            if (it->is_regular_file(ec) && isImagePath(name) && name.find("<UDIM>") == std::string::npos) files.push_back(it->path());
        }
        for (const fs::path& path : files) {
            const std::string name = path.stem().u8string();
            if (!seen.insert(lower(name)).second) continue;  // an earlier folder's
            (isShipped ? ours : own).push_back({name, path.generic_u8string(), isShipped});
        }
    }
    const auto byName = [](const BrushAlphaFile& a, const BrushAlphaFile& b) { return lower(a.name) < lower(b.name); };
    std::sort(own.begin(), own.end(), byName);
    std::sort(ours.begin(), ours.end(), byName);
    own.insert(own.end(), ours.begin(), ours.end());
    return own;
}

std::string addBrushAlpha(const std::string& file) {
    const fs::path from = fs::u8path(file);
    if (!isImagePath(from.filename().u8string())) throw std::runtime_error("not an image GroomLab reads: " + file);
    try {
        loadBrushAlpha(file);  // readable as an alpha
    } catch (const std::exception& e) {
        throw std::runtime_error(e.what());
    }
    for (const std::string& dir : brushAlphaDirs()) {
        if (sameFolder(from.parent_path(), fs::u8path(dir))) return from.generic_u8string();  // in the library already
    }
    const std::string user = brushAlphaUserDir();
    if (user.empty()) throw std::runtime_error("no folder for alphas (set GROOMLAB_USER_ALPHAS)");
    std::error_code ec;
    fs::create_directories(fs::u8path(user), ec);
    // A name no alpha has yet (in any folder: it would hide a shipped one), nor any file there.
    std::set<std::string> taken;
    for (const BrushAlphaFile& a : listBrushAlphas()) taken.insert(lower(a.name));
    const std::string stem = from.stem().u8string(), extension = from.extension().u8string();
    std::string name = stem;
    fs::path to = fs::u8path(user) / fs::u8path(name + extension);
    for (int k = 2; taken.count(lower(name)) || fs::exists(to, ec); ++k) {
        name = stem + " " + std::to_string(k);
        to = fs::u8path(user) / fs::u8path(name + extension);
    }
    if (!fs::copy_file(from, to, ec) || ec) throw std::runtime_error("cannot copy " + file + " to " + user + ": " + ec.message());
    return to.generic_u8string();
}

}  // namespace groomlab
