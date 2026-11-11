// SPDX-License-Identifier: Apache-2.0
#include "groomlab/texture.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <list>
#include <map>
#include <mutex>
#include <system_error>
#include <unordered_map>
#include <utility>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>  // CompareStringOrdinal: file names as NTFS compares them
#endif

#include "groomlab/parallel.h"
#include "groomlab/random.h"
#include "groomlab/process_caches.h"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_FAILURE_USERMSG
#define STBI_WINDOWS_UTF8  // stb opens UTF-8 paths with _wfopen on Windows
#if defined(_MSC_VER)
#pragma warning(push, 0)
#endif
#include "stb_image.h"
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

namespace groomlab {

namespace fs = std::filesystem;

namespace {

constexpr int kTileBase = 1001;
constexpr int kTileEnd = kTileBase + Texture::kTiles;  // one past the last tile
constexpr const char* kUdimToken = "<UDIM>";

// One tile whose file is missing or unreadable (never dereferenced).
const Texture::Image kMissingTile{};

fs::path toPath(const std::string& utf8) { return fs::u8path(utf8); }

std::string tilePath(const std::string& pattern, int tile) {
    std::string path = pattern;
    const std::size_t at = path.find(kUdimToken);
    if (at != std::string::npos) path.replace(at, std::strlen(kUdimToken), std::to_string(tile));
    return path;
}

using Tiles = std::array<bool, Texture::kTiles>;  // which tiles exist, 1001 first

// Names as the file system compares them: regardless of case on Windows.
bool sameName(const fs::path::string_type& a, const fs::path::string_type& b) {
#ifdef _WIN32
    return a.size() == b.size() &&
           (a.empty() || CompareStringOrdinal(a.c_str(), static_cast<int>(a.size()), b.c_str(), static_cast<int>(b.size()),
                                              TRUE) == CSTR_EQUAL);
#else
    return a == b;
#endif
}

// Which tiles of a UDIM pattern exist, from one listing of its folder rather than a question per
// tile. Asks tile by tile when the folder cannot be listed (or the token is not in the name).
Tiles tilesOf(const std::string& pattern) {
    Tiles tiles{};
    const std::size_t slash = pattern.find_last_of("/\\");
    const std::size_t nameStart = slash == std::string::npos ? 0 : slash + 1;
    const std::size_t at = pattern.find(kUdimToken);
    bool listed = false;
    if (at != std::string::npos && at >= nameStart) {
        const fs::path::string_type prefix = toPath(pattern.substr(nameStart, at - nameStart)).native();
        const fs::path::string_type suffix = toPath(pattern.substr(at + std::strlen(kUdimToken))).native();
        const fs::path folder = nameStart == 0 ? fs::path(".") : toPath(pattern.substr(0, nameStart));
        std::error_code ec;
        fs::directory_iterator it(folder, ec), end;
        listed = !ec;
        for (; listed && it != end; it.increment(ec)) {
            const fs::path::string_type name = it->path().filename().native();
            if (name.size() != prefix.size() + 4 + suffix.size()) continue;
            if (!sameName(name.substr(0, prefix.size()), prefix) || !sameName(name.substr(prefix.size() + 4), suffix)) continue;
            int tile = 0;
            bool digits = true;
            for (std::size_t k = prefix.size(); k < prefix.size() + 4; ++k) {
                digits = digits && name[k] >= '0' && name[k] <= '9';
                tile = tile * 10 + static_cast<int>(name[k] - '0');
            }
            if (digits && tile >= kTileBase && tile < kTileEnd) tiles[static_cast<std::size_t>(tile - kTileBase)] = true;
        }
        listed = listed && !ec;
    }
    if (!listed) {
        tiles = Tiles{};
        for (int t = kTileBase; t < kTileEnd; ++t) {
            std::error_code ec;
            tiles[static_cast<std::size_t>(t - kTileBase)] = fs::exists(toPath(tilePath(pattern, t)), ec);
        }
    }
    return tiles;
}

// The UDIM pattern of a path ("<UDIM>" where the tile number goes), or empty
// for a plain image. A "1001"-style number in the file name only counts when
// another tile of the set exists beside it: a lone 1001 is just a name.
std::string udimPattern(const std::string& path) {
    const std::size_t slash = path.find_last_of("/\\");
    const std::size_t nameStart = slash == std::string::npos ? 0 : slash + 1;
    std::string name = path.substr(nameStart);
    for (const char* token : {"<UDIM>", "<udim>", "%(UDIM)d"}) {
        const std::size_t at = name.find(token);
        if (at == std::string::npos) continue;
        name.replace(at, std::strlen(token), kUdimToken);
        return path.substr(0, nameStart) + name;
    }
    auto separator = [](char c) { return c == '.' || c == '_' || c == '-'; };
    for (std::size_t i = 0; i + 4 <= name.size(); ++i) {
        const std::size_t end = i + 4;
        if (i > 0 && !separator(name[i - 1])) continue;
        if (end < name.size() && !separator(name[end])) continue;
        bool digits = true;
        for (std::size_t k = i; k < end; ++k) digits = digits && name[k] >= '0' && name[k] <= '9';
        if (!digits) continue;
        const int tile = std::atoi(name.substr(i, 4).c_str());
        if (tile < kTileBase || tile >= kTileEnd) continue;
        const std::string pattern = path.substr(0, nameStart) + name.substr(0, i) + kUdimToken + name.substr(end);
        const Tiles tiles = tilesOf(pattern);
        for (int t = kTileBase; t < kTileEnd; ++t) {
            if (t != tile && tiles[static_cast<std::size_t>(t - kTileBase)]) return pattern;
        }
        return {};
    }
    return {};
}

std::uint64_t stampOf(const fs::path& path) {
    std::error_code ec;
    // One look at the file: the entry keeps the size and time it read (Windows), else asks again.
    const fs::directory_entry entry(path, ec);
    if (ec) return 0;
    const auto size = entry.file_size(ec);
    if (ec) return 0;
    const auto time = entry.last_write_time(ec).time_since_epoch().count();
    if (ec) return 0;
    return hashCombine(hash64(size), static_cast<std::uint64_t>(time));
}

// The file's stamp with its path in it: a cache keyed by it never takes another file of the same
// size and time (a search path changed, a copy keeping its time) for this one.
std::uint64_t stampFromDisk(const std::string& path);
std::uint64_t stampWithPath(const std::string& path) {
    const std::uint64_t stamp = stampFromDisk(path);
    if (!stamp) return 0;
    const std::string pattern = udimPattern(path);  // a tile's: its set's (the set is what it stamps)
    const std::uint64_t h = hashString(stamp, pattern.empty() ? path : pattern);
    return h ? h : 1;
}

std::uint64_t stampFromDisk(const std::string& path) {
    const std::string pattern = udimPattern(path);
    if (pattern.empty()) return stampOf(toPath(path));
    const Tiles tiles = tilesOf(pattern);
    std::uint64_t h = 0;
    for (int t = kTileBase; t < kTileEnd; ++t) {
        if (!tiles[static_cast<std::size_t>(t - kTileBase)]) continue;
        const std::uint64_t s = stampOf(toPath(tilePath(pattern, t)));
        if (s) h = hashCombine(hashCombine(h, static_cast<std::uint64_t>(t)), s);
    }
    return h;
}

// The stamps of the innermost ScopedFileStamps' owner on this thread (null: none).
thread_local std::unordered_map<std::string, std::uint64_t>* tStamps = nullptr;

bool isPnm(const std::string& path) {
    std::ifstream in(toPath(path), std::ios::binary);
    char magic[2] = {0, 0};
    in.read(magic, 2);
    return magic[0] == 'P' && (magic[1] == '5' || magic[1] == '6');
}

std::unique_ptr<Texture::Image> decode(const std::string& path) {
    auto image = std::make_unique<Texture::Image>();
    int w = 0, h = 0, n = 0;
    if (stbi_is_hdr(path.c_str())) {
        float* data = stbi_loadf(path.c_str(), &w, &h, &n, 4);
        if (!data) throw TextureError("cannot read '" + path + "': " + stbi_failure_reason());
        image->rgba.assign(data, data + static_cast<std::size_t>(w) * h * 4);
        image->bits = 32;
        stbi_image_free(data);
    } else if (stbi_is_16_bit(path.c_str())) {
        stbi_us* data = stbi_load_16(path.c_str(), &w, &h, &n, 4);
        if (!data) throw TextureError("cannot read '" + path + "': " + stbi_failure_reason());
        image->rgba.resize(static_cast<std::size_t>(w) * h * 4);
        // A 16-bit PNM stores samples big-endian, which stb_image reads as they lie.
        const bool swap = isPnm(path);
        for (std::size_t i = 0; i < image->rgba.size(); ++i) {
            const stbi_us v = swap ? static_cast<stbi_us>((data[i] >> 8) | (data[i] << 8)) : data[i];
            image->rgba[i] = v / 65535.0f;
        }
        image->bits = 16;
        stbi_image_free(data);
    } else {
        unsigned char* data = stbi_load(path.c_str(), &w, &h, &n, 4);
        if (!data) throw TextureError("cannot read '" + path + "': " + stbi_failure_reason());
        image->rgba.resize(static_cast<std::size_t>(w) * h * 4);
        for (std::size_t i = 0; i < image->rgba.size(); ++i) image->rgba[i] = data[i] / 255.0f;
        image->bits = 8;
        stbi_image_free(data);
    }
    if (w <= 0 || h <= 0) throw TextureError("cannot read '" + path + "': empty image");
    image->width = w;
    image->height = h;
    return image;
}

}  // namespace

std::array<float, 4> Texture::Image::texel(int x, int y) const {
    x = ((x % width) + width) % width;
    y = ((y % height) + height) % height;
    const float* p = rgba.data() + (static_cast<std::size_t>(y) * width + x) * 4;
    return {p[0], p[1], p[2], p[3]};
}

std::array<float, 4> Texture::Image::sample(float u, float v) const {
    // Texel centers at (i + 0.5) / size; row 0 is the top of the image.
    const float fx = u * width - 0.5f;
    const float fy = (1.0f - v) * height - 0.5f;
    const float x0f = std::floor(fx), y0f = std::floor(fy);
    const float tx = fx - x0f, ty = fy - y0f;
    const int x0 = static_cast<int>(x0f), y0 = static_cast<int>(y0f);
    const auto a = texel(x0, y0), b = texel(x0 + 1, y0), c = texel(x0, y0 + 1), d = texel(x0 + 1, y0 + 1);
    std::array<float, 4> out{};
    for (int k = 0; k < 4; ++k) {
        const float top = a[k] + (b[k] - a[k]) * tx;
        const float bottom = c[k] + (d[k] - c[k]) * tx;
        out[k] = top + (bottom - top) * ty;
    }
    return out;
}

Texture::Texture(int width, int height, std::vector<float> rgba, int bits) {
    if (width <= 0 || height <= 0 || rgba.size() != static_cast<std::size_t>(width) * height * 4) {
        throw TextureError("Texture: size does not match its data");
    }
    auto image = std::make_shared<Image>();
    image->width = width;
    image->height = height;
    image->rgba = std::move(rgba);
    image->bits = bits;
    image_ = std::move(image);
}

Texture::Texture(std::string udimPattern) : udimPattern_(std::move(udimPattern)) {
    if (udimPattern_.find(kUdimToken) == std::string::npos) throw TextureError("Texture: a UDIM pattern needs <UDIM>");
}

Texture::~Texture() = default;

int Texture::width() const {
    if (image_) return image_->width;
    const Image* first = tileCache_[0].load(std::memory_order_acquire);
    return first && first != &kMissingTile ? first->width : 0;
}

int Texture::height() const {
    if (image_) return image_->height;
    const Image* first = tileCache_[0].load(std::memory_order_acquire);
    return first && first != &kMissingTile ? first->height : 0;
}

std::size_t Texture::bytes() const {
    if (image_) return image_->rgba.size() * sizeof(float);
    return tileBytes_.load(std::memory_order_relaxed);
}

std::string Texture::error() const {
    std::lock_guard<std::mutex> lock(tileLock_);
    return error_;
}

const Texture::Image* Texture::tile(int index) const {
    if (const Image* cached = tileCache_[static_cast<std::size_t>(index)].load(std::memory_order_acquire)) return cached;
    std::lock_guard<std::mutex> lock(tileLock_);
    if (const Image* cached = tileCache_[static_cast<std::size_t>(index)].load(std::memory_order_acquire)) return cached;
    const std::string path = tilePath(udimPattern_, kTileBase + index);
    const Image* result = &kMissingTile;
    std::error_code ec;
    if (fs::exists(toPath(path), ec)) {
        try {
            std::unique_ptr<Image> image = decode(path);
            tileBytes_.fetch_add(image->rgba.size() * sizeof(float), std::memory_order_relaxed);
            result = image.get();
            tileStorage_.push_back(std::move(image));
        } catch (const TextureError& e) {
            error_ = e.what();  // a bad tile samples black rather than failing the whole groom mid-evaluation
        }
    }
    tileCache_[static_cast<std::size_t>(index)].store(result, std::memory_order_release);
    return result;
}

std::array<float, 4> Texture::sample(float u, float v) const {
    if (image_) return image_->sample(u, v);
    const float fu = std::floor(u), fv = std::floor(v);
    if (!(fu >= 0.0f && fu < 10.0f && fv >= 0.0f && fv < 10.0f)) return {0.0f, 0.0f, 0.0f, 0.0f};
    const Image* image = tile(static_cast<int>(fu) + 10 * static_cast<int>(fv));
    if (image == &kMissingTile) return {0.0f, 0.0f, 0.0f, 0.0f};
    return image->sample(u - fu, v - fv);
}

bool isImagePath(const std::string& name) {
    if (name.find("<UDIM>") != std::string::npos) return true;
    const std::size_t dot = name.find_last_of('.');
    if (dot == std::string::npos || name.find_first_of("/\\", dot) != std::string::npos) return false;
    std::string ext = name.substr(dot + 1);
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    static const char* const kImages[] = {"png", "jpg", "jpeg", "tga", "bmp", "psd", "gif", "hdr", "pic", "pnm", "ppm", "pgm"};
    return std::any_of(std::begin(kImages), std::end(kImages), [&](const char* e) { return ext == e; });
}

bool isPngPath(const std::string& path) {
    std::string ext = std::filesystem::u8path(path).extension().u8string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext == ".png";
}

bool isGrayImage(const Texture& image) {
    const Texture::Image* im = image.image();
    if (!im) return false;
    for (std::size_t i = 0; i + 2 < im->rgba.size(); i += 4) {
        if (im->rgba[i] != im->rgba[i + 1] || im->rgba[i + 1] != im->rgba[i + 2]) return false;
    }
    return true;
}

std::uint64_t fileStamp(const std::string& path) {
    if (!tStamps) return stampWithPath(path);
    auto it = tStamps->find(path);
    if (it == tStamps->end()) it = tStamps->emplace(path, stampWithPath(path)).first;
    return it->second;
}

ScopedFileStamps::ScopedFileStamps() {
    if (tStamps) return;  // nested: the outer scope's answers
    tStamps = new std::unordered_map<std::string, std::uint64_t>();
    owner_ = true;
}

ScopedFileStamps::~ScopedFileStamps() {
    if (!owner_) return;
    delete tStamps;
    tStamps = nullptr;
}

namespace {

// Most recently used first, within a byte budget: many maps in turn drop the
// ones not touched for longest instead of growing without bound.
class TextureCache {
public:
    TexturePtr find(const std::string& key, std::uint64_t stamp) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = entries_.find(key);
        if (it == entries_.end()) return nullptr;
        if (it->second.stamp != stamp) {
            order_.erase(it->second.order);  // rewritten (painting re-saves the map): the stale decode goes
            entries_.erase(it);
            return nullptr;
        }
        order_.splice(order_.begin(), order_, it->second.order);
        return it->second.texture;
    }

    void insert(const std::string& key, std::uint64_t stamp, TexturePtr texture) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = entries_.find(key);
        if (it != entries_.end()) {
            order_.erase(it->second.order);
            entries_.erase(it);
        }
        order_.push_front(key);
        entries_[key] = {stamp, std::move(texture), order_.begin()};
        while (entries_.size() > 1 && total() > budget()) {
            entries_.erase(order_.back());
            order_.pop_back();
        }
    }

    // Every decode let go (releaseProcessCaches), freed outside the lock.
    void clear() {
        std::map<std::string, Entry> drop;
        std::lock_guard<std::mutex> lock(mutex_);
        drop.swap(entries_);
        order_.clear();
    }
    std::size_t bytes() {
        std::lock_guard<std::mutex> lock(mutex_);
        return total();
    }

private:
    struct Entry {
        std::uint64_t stamp = 0;
        TexturePtr texture;
        std::list<std::string>::iterator order;
    };

    std::size_t total() const {
        std::size_t bytes = 0;
        for (const auto& [key, entry] : entries_) bytes += entry.texture->bytes();
        return bytes;
    }

    static std::size_t budget() {
        static const std::size_t value = [] {
            const std::size_t defaultMb = 1024;
            std::size_t mb = defaultMb;
#if defined(_MSC_VER)
            char* text = nullptr;
            std::size_t length = 0;
            if (_dupenv_s(&text, &length, "GROOMLAB_TEXTURE_CACHE_MB") == 0 && text) {
                mb = static_cast<std::size_t>(std::max(std::atoll(text), 1LL));
                std::free(text);
            }
#else
            if (const char* text = std::getenv("GROOMLAB_TEXTURE_CACHE_MB")) {
                mb = static_cast<std::size_t>(std::max(std::atoll(text), 1LL));
            }
#endif
            return mb * 1024 * 1024;
        }();
        return value;
    }

    std::mutex mutex_;
    std::map<std::string, Entry> entries_;
    std::list<std::string> order_;  // front: most recently used
};

TextureCache& textureCache() {
    static TextureCache cache;
    static const bool registered =
        (registerProcessCache("textures", [] { cache.clear(); }, [] { return cache.bytes(); }), true);
    (void)registered;
    return cache;
}

}  // namespace

TexturePtr textureFromPixels(int width, int height, int channels, const std::vector<std::uint8_t>& pixels) {
    if (width <= 0 || height <= 0 || channels < 1 || channels > 4 ||
        pixels.size() != static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * static_cast<std::size_t>(channels)) {
        throw TextureError("textureFromPixels: size does not match its pixels");
    }
    // As decode() reads an 8-bit file: stb expands to RGBA (gray to r g b, a missing alpha 255),
    // then each value / 255.
    const std::size_t count = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    std::vector<float> rgba(count * 4);
    const auto n = static_cast<std::size_t>(channels);
    parallelFor(0, count, [&](std::size_t i) {
        const std::uint8_t* p = pixels.data() + i * n;
        std::uint8_t c[4];
        switch (channels) {
            case 1: c[0] = c[1] = c[2] = p[0], c[3] = 255; break;
            case 2: c[0] = c[1] = c[2] = p[0], c[3] = p[1]; break;
            case 3: c[0] = p[0], c[1] = p[1], c[2] = p[2], c[3] = 255; break;
            default: c[0] = p[0], c[1] = p[1], c[2] = p[2], c[3] = p[3]; break;
        }
        for (std::size_t k = 0; k < 4; ++k) rgba[i * 4 + k] = c[k] / 255.0f;
    }, 1 << 14);
    return std::make_shared<const Texture>(width, height, std::move(rgba), 8);
}

bool primeTextureCache(const std::string& path, std::uint64_t stamp, TexturePtr texture) {
    if (!texture || stamp == 0 || texture->isUdim() || !udimPattern(path).empty()) return false;
    textureCache().insert(path, stamp, std::move(texture));
    return true;
}

TexturePtr cachedTexture(const std::string& path) {
    if (!udimPattern(path).empty()) return nullptr;
    const std::uint64_t stamp = fileStamp(path);
    if (stamp == 0) return nullptr;
    return textureCache().find(path, stamp);
}

TexturePtr loadTexture(const std::string& path) {
    const std::string pattern = udimPattern(path);
    const std::string key = pattern.empty() ? path : pattern;
    const std::uint64_t stamp = fileStamp(path);
    if (stamp == 0) throw TextureError("cannot read '" + path + "': file not found");
    if (TexturePtr cached = textureCache().find(key, stamp)) return cached;
    TexturePtr texture;  // decoded outside the lock: decoding can be slow
    if (pattern.empty()) {
        std::unique_ptr<Texture::Image> image = decode(path);
        texture = std::make_shared<const Texture>(image->width, image->height, std::move(image->rgba), image->bits);
    } else {
        texture = std::make_shared<const Texture>(pattern);
    }
    textureCache().insert(key, stamp, texture);
    return texture;
}

}  // namespace groomlab
