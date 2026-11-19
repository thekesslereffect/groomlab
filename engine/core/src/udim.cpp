// SPDX-License-Identifier: Apache-2.0
#include "groomlab/udim.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>

#include "groomlab/texture.h"

namespace groomlab {

int udimTileAt(float u, float v) {
    if (!(u >= 0.0f) || !(v >= 0.0f) || !(u < 10.0f) || !(v < 10.0f)) return -1;
    return kFirstUdimTile + static_cast<int>(std::floor(u)) + 10 * static_cast<int>(std::floor(v));
}

std::array<int, 2> udimTileOrigin(int tile) {
    const int k = tile - kFirstUdimTile;
    return {k % 10, k / 10};
}

void UdimLayout::arrange() {
    const int n = std::max(static_cast<int>(tiles.size()), 1);
    columns = static_cast<int>(std::ceil(std::sqrt(static_cast<double>(n))));
    rows = (n + columns - 1) / columns;
}

int UdimLayout::slotOf(int tile) const {
    const auto it = std::lower_bound(tiles.begin(), tiles.end(), tile);
    return it != tiles.end() && *it == tile ? static_cast<int>(it - tiles.begin()) : -1;
}

std::array<int, 2> UdimLayout::mosaicSize(int width, int height) const {
    if (!udim_) return {width, height};
    return {columns * (width + 2 * kGutter), rows * (height + 2 * kGutter)};
}

std::array<int, 2> UdimLayout::tileOrigin(int tile, int width, int height) const {
    if (!udim_) return {0, 0};
    const int slot = std::max(slotOf(tile), 0);
    return {(slot % columns) * (width + 2 * kGutter) + kGutter, (slot / columns) * (height + 2 * kGutter) + kGutter};
}

ScalpMesh::UV UdimLayout::toMosaic(const ScalpMesh::UV& uv, int tile, int width, int height) const {
    if (!udim_) return uv;
    const std::array<int, 2> at = udimTileOrigin(tile), origin = tileOrigin(tile, width, height), size = mosaicSize(width, height);
    const float x = static_cast<float>(origin[0]) + (uv[0] - static_cast<float>(at[0])) * static_cast<float>(width);
    const float top = static_cast<float>(origin[1]) + (1.0f - (uv[1] - static_cast<float>(at[1]))) * static_cast<float>(height);
    return {x / static_cast<float>(size[0]), 1.0f - top / static_cast<float>(size[1])};
}

int UdimLayout::fromMosaic(float x, float y, int width, int height, ScalpMesh::UV* uv) const {
    if (!udim_) {
        if (uv) *uv = {x / static_cast<float>(width), 1.0f - y / static_cast<float>(height)};
        return kFirstUdimTile;
    }
    const int cellW = width + 2 * kGutter, cellH = height + 2 * kGutter;
    const int column = static_cast<int>(std::floor(x / static_cast<float>(cellW)));
    const int row = static_cast<int>(std::floor(y / static_cast<float>(cellH)));
    if (column < 0 || row < 0 || column >= columns || row >= rows) return -1;
    const int slot = row * columns + column;
    if (slot >= static_cast<int>(tiles.size())) return -1;
    const float fx = (x - static_cast<float>(column * cellW + kGutter)) / static_cast<float>(width);
    const float fy = (y - static_cast<float>(row * cellH + kGutter)) / static_cast<float>(height);
    if (fx < 0.0f || fx > 1.0f || fy < 0.0f || fy > 1.0f) return -1;
    const int tile = tiles[static_cast<std::size_t>(slot)];
    const std::array<int, 2> at = udimTileOrigin(tile);
    if (uv) *uv = {static_cast<float>(at[0]) + fx, static_cast<float>(at[1]) + (1.0f - fy)};
    return tile;
}

UdimLayout UdimLayout::ofTiles(std::vector<int> tiles) {
    UdimLayout out;
    std::sort(tiles.begin(), tiles.end());
    tiles.erase(std::unique(tiles.begin(), tiles.end()), tiles.end());
    tiles.erase(std::remove_if(tiles.begin(), tiles.end(), [](int t) { return t < kFirstUdimTile || t >= kFirstUdimTile + kUdimTiles; }),
                tiles.end());
    if (tiles.empty()) tiles.push_back(kFirstUdimTile);
    out.tiles = std::move(tiles);
    out.udim_ = !(out.tiles.size() == 1 && out.tiles[0] == kFirstUdimTile);
    out.arrange();
    return out;
}

UdimLayout udimLayoutOf(const ScalpMesh& scalp) {
    UdimLayout out;
    if (!scalp.hasUVs()) {
        out.tiles = {kFirstUdimTile};
        out.triangleTile.assign(scalp.triangleCount(), kFirstUdimTile);
        return out;
    }
    const auto& uvs = scalp.cornerUVs();
    out.triangleTile.resize(scalp.triangleCount());
    std::vector<char> used(kUdimTiles, 0);
    bool off = false;
    for (std::size_t t = 0; t < scalp.triangleCount(); ++t) {
        const ScalpMesh::UV& a = uvs[t * 3], &b = uvs[t * 3 + 1], &c = uvs[t * 3 + 2];
        const int tile = udimTileAt((a[0] + b[0] + c[0]) / 3.0f, (a[1] + b[1] + c[1]) / 3.0f);
        out.triangleTile[t] = tile;
        if (tile < 0) off = true;
        else used[static_cast<std::size_t>(tile - kFirstUdimTile)] = 1;
    }
    for (int k = 0; k < kUdimTiles; ++k) {
        if (used[static_cast<std::size_t>(k)]) out.tiles.push_back(kFirstUdimTile + k);
    }
    if (out.tiles.empty()) out.tiles.push_back(kFirstUdimTile);
    // (Off the grid: drawn from tile 1001's slot when there is one, else the first; as a single
    // image repeats such UVs, the identity layout keeps them.)
    const int fallback = out.tiles.front();
    for (int& tile : out.triangleTile) {
        if (tile < 0) tile = fallback;
    }
    out.udim_ = !(out.tiles.size() == 1 && out.tiles[0] == kFirstUdimTile);
    (void)off;
    out.arrange();
    return out;
}

std::shared_ptr<const ScalpMesh> mosaicScalp(const ScalpMesh& scalp, const UdimLayout& layout, int width, int height) {
    if (!layout.udim() || !scalp.hasUVs()) return std::make_shared<const ScalpMesh>(scalp);
    auto out = std::make_shared<ScalpMesh>(scalp);
    std::vector<ScalpMesh::UV> uvs = scalp.cornerUVs();
    for (std::size_t t = 0; t < scalp.triangleCount(); ++t) {
        const int tile = t < layout.triangleTile.size() ? layout.triangleTile[t] : kFirstUdimTile;
        for (std::size_t k = 0; k < 3; ++k) uvs[t * 3 + k] = layout.toMosaic(uvs[t * 3 + k], tile, width, height);
    }
    out->setCornerUVs(std::move(uvs));
    return out;
}

std::string udimTilePath(const std::string& pattern, int tile) {
    std::string path = pattern;
    for (const char* token : {"<UDIM>", "<udim>", "%(UDIM)d"}) {
        const std::size_t at = path.find(token);
        if (at != std::string::npos) {
            path.replace(at, std::strlen(token), std::to_string(tile));
            return path;
        }
    }
    return path;
}

bool isUdimPath(const std::string& path) { return !udimPatternOf(path).empty(); }

const char* udimTokenIn(const std::string& path) {
    const std::size_t slash = path.find_last_of("/\\");
    const std::string name = slash == std::string::npos ? path : path.substr(slash + 1);
    for (const char* token : kUdimTokens) {
        if (name.find(token) != std::string::npos) return token;
    }
    return nullptr;
}

std::vector<std::pair<std::string, std::string>> mapImageMoves(const std::string& from, const std::string& to) {
    if (!isUdimPath(from)) return {{from, to}};
    const std::string fromSet = udimPathFor(from), toSet = udimPathFor(to);
    std::vector<std::pair<std::string, std::string>> out;
    for (const int tile : Texture(fromSet).udimTiles()) out.emplace_back(udimTilePath(fromSet, tile), udimTilePath(toSet, tile));
    return out;
}

namespace {
std::string fileText(const std::string& path) {
    std::ifstream in(std::filesystem::u8path(path), std::ios::binary);
    return in ? std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()) : std::string();
}
}  // namespace

bool sameMapImage(const std::string& from, const std::string& to) {
    if (fileStamp(from) == 0) return false;
    for (const auto& [a, b] : mapImageMoves(from, to)) {
        std::error_code ec;
        if (!std::filesystem::is_regular_file(std::filesystem::u8path(b), ec)) return false;
        if (std::filesystem::file_size(std::filesystem::u8path(a), ec) != std::filesystem::file_size(std::filesystem::u8path(b), ec)) {
            return false;
        }
        if (fileText(a) != fileText(b)) return false;
    }
    return true;
}

std::vector<std::string> copyMapImage(const std::string& from, const std::string& to) {
    std::vector<std::string> written;
    if (fileStamp(from) == 0) return written;
    for (const auto& [a, b] : mapImageMoves(from, to)) {
        std::error_code ec;
        const std::filesystem::path target = std::filesystem::u8path(b);
        if (std::filesystem::exists(target, ec)) continue;
        if (target.has_parent_path()) std::filesystem::create_directories(target.parent_path(), ec);
        std::filesystem::copy_file(std::filesystem::u8path(a), target, std::filesystem::copy_options::none, ec);
        if (!ec) written.push_back(b);
    }
    return written;
}

bool isPlainImagePath(const std::string& path) {
    const std::filesystem::path p = std::filesystem::u8path(path);
    const std::string name = p.filename().u8string(), stem = p.stem().u8string();
    for (const char* token : {"<UDIM>", "<udim>", "%(UDIM)d"}) {
        if (name.find(token) != std::string::npos) return false;
    }
    if (stem.size() >= 5) {
        const std::string digits = stem.substr(stem.size() - 4);
        const char sep = stem[stem.size() - 5];
        const bool number = std::all_of(digits.begin(), digits.end(), [](unsigned char c) { return std::isdigit(c) != 0; });
        if (number && (sep == '.' || sep == '_')) {
            const int tile = std::stoi(digits);
            if (tile >= kFirstUdimTile && tile < kFirstUdimTile + kUdimTiles) return false;
        }
    }
    return !p.extension().empty();
}

std::vector<std::string> mapImageFiles(const std::string& path, const std::vector<int>& tiles) {
    const bool set = !tiles.empty() || isUdimPath(path);
    if (!set) return {path};
    const std::string pattern = udimPathFor(path);
    std::vector<int> all = Texture(pattern).udimTiles();
    all.insert(all.end(), tiles.begin(), tiles.end());
    std::sort(all.begin(), all.end());
    all.erase(std::unique(all.begin(), all.end()), all.end());
    std::vector<std::string> out;
    if (isPlainImagePath(path) && pattern != path) out.push_back(path);  // (the one image the set replaces)
    for (const int tile : all) out.push_back(udimTilePath(pattern, tile));
    return out;
}

std::string udimPathFor(const std::string& path) {
    const std::string pattern = udimPatternOf(path);
    if (!pattern.empty()) return pattern;
    // A tile's own name ("<name>.1001.png", "<name>_1002.png") with no tiles beside it yet: that
    // naming's set.
    const std::size_t slash = path.find_last_of("/\\");
    const std::size_t nameStart = slash == std::string::npos ? 0 : slash + 1;
    const std::size_t dot = path.find_last_of('.');
    const std::size_t end = dot == std::string::npos || dot < nameStart ? path.size() : dot;
    if (end >= nameStart + 5) {
        const std::size_t at = end - 4;
        const char separator = path[at - 1];
        bool digits = separator == '.' || separator == '_';
        for (std::size_t k = at; k < end && digits; ++k) digits = path[k] >= '0' && path[k] <= '9';
        if (digits) {
            const int tile = std::stoi(path.substr(at, 4));
            if (tile >= kFirstUdimTile && tile < kFirstUdimTile + kUdimTiles) return path.substr(0, at) + "<UDIM>" + path.substr(end);
        }
    }
    return path.substr(0, end) + ".<UDIM>" + path.substr(end);
}

}  // namespace groomlab
