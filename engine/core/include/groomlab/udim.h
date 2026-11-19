// SPDX-License-Identifier: Apache-2.0
#pragma once

// UDIM tile sets for the texture maps painted on a scalp (issue #87).
//
// A scalp's UVs may leave the 0-1 square: each UDIM tile is a unit square of UV space, tile
// 1001 + floor(u) + 10 floor(v) (u and v in [0, 10)), and a map over such a scalp is a tile set,
// one image file per tile: "<name>.1001.png", "<name>_1002.png"... (texture.h reads both, and
// "<UDIM>", "<udim>" or "%(UDIM)d" where the tile number goes).
//
// A texture map is painted, converted and projected as ONE image, the tile set's mosaic: the
// tiles the scalp's triangles lie on, side by side in a grid of slots, each slot the tile's
// texels with a gutter of kGutter texels round it (so a bilinear sample or a padded edge in one
// tile never reaches another's texels). The scalp is seen through the mosaic's UVs
// (mosaicScalp: each triangle's corners moved from its tile into its slot), so everything a
// TexturePaint does over one image (brush stamps, smoothing across seams, fills, undo tiles,
// conversions, the viewport's preview) does the same over the tile set. A tile's texels in the
// mosaic are the file's texels exactly (tileRect), so loading and saving a tile set is a copy.
//
// A scalp whose triangles all lie on tile 1001 has the identity layout (udim() false): no
// mosaic, its maps one image over UVs 0-1, as they always were.
//
// A triangle belongs to the tile its UV centroid lies on; one crossing a tile's border is drawn
// from its own tile's slot (and reaches past it into the gutter), so a mesh made for UDIMs, whose
// triangles keep to their tiles, paints exactly.

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "groomlab/scalp_mesh.h"

namespace groomlab {

constexpr int kFirstUdimTile = 1001;
constexpr int kUdimTiles = 100;  // 1001-1100: u and v in [0, 10)
// The tokens a tile set's path names its tiles with (Mari's and most renderers' <UDIM>, <udim>,
// and Arnold/RenderMan's %(UDIM)d): every host's list.
constexpr const char* kUdimTokens[] = {"<UDIM>", "<udim>", "%(UDIM)d"};
// The token in `path`'s file name, or nullptr.
const char* udimTokenIn(const std::string& path);

// The tile of a UV (1001 + floor(u) + 10 floor(v)); -1 off the 10 x 10 grid.
int udimTileAt(float u, float v);
// A tile's lower-left UV corner (its u and v offsets).
std::array<int, 2> udimTileOrigin(int tile);

struct UdimLayout {
    static constexpr int kGutter = 8;

    // The tiles the scalp's triangles lie on, ascending, and each triangle's tile (-1: off the
    // grid, kept in tile 1001's slot when there is one, else the first).
    std::vector<int> tiles;
    std::vector<int> triangleTile;
    int columns = 1, rows = 1;  // the slots' grid; slot k at column k % columns, row k / columns (row 0 at the top)

    // Whether the scalp needs a tile set (any triangle off tile 1001); false: one image, no mosaic.
    bool udim() const { return udim_; }
    // The slot of `tile` (-1: not in the layout).
    int slotOf(int tile) const;
    // The mosaic's size for tiles of width x height texels (udim() false: width x height).
    std::array<int, 2> mosaicSize(int width, int height) const;
    // A tile's texels in the mosaic: columns [x, x + width), rows [y, y + height), rows top-down.
    std::array<int, 2> tileOrigin(int tile, int width, int height) const;
    // A UV on `tile` as the mosaic's UV (tiles of width x height).
    ScalpMesh::UV toMosaic(const ScalpMesh::UV& uv, int tile, int width, int height) const;
    // The UV on its tile of a point of the mosaic (texel coordinates, rows top-down), and the
    // tile (-1 in a gutter or an empty slot).
    int fromMosaic(float x, float y, int width, int height, ScalpMesh::UV* uv) const;

    // A layout of these tiles (a tile set read with no scalp to lay it out: transfers, tests).
    static UdimLayout ofTiles(std::vector<int> tiles);

private:
    friend UdimLayout udimLayoutOf(const ScalpMesh& scalp);
    void arrange();
    bool udim_ = false;
};

// The scalp's layout (the identity one without UVs or with every triangle on tile 1001).
UdimLayout udimLayoutOf(const ScalpMesh& scalp);

// The scalp seen through the mosaic of tiles width x height (its corner UVs moved into their
// slots; points, triangles, rest pose and color sets as they are). The scalp itself for the
// identity layout.
std::shared_ptr<const ScalpMesh> mosaicScalp(const ScalpMesh& scalp, const UdimLayout& layout, int width, int height);

// The path a tile set is written to for a map stored at `path`: the path itself when it names
// a tile set already (a token, or "<name>.1001.png" / "<name>_1001.png" with other tiles beside
// it: their separator kept), else "<UDIM>" put before the extension after a dot
// ("masks/head/density.png" -> "masks/head/density.<UDIM>.png").
std::string udimPathFor(const std::string& path);
// A tile's file of a tile set's path ("<UDIM>", "<udim>" or "%(UDIM)d" replaced by the tile).
std::string udimTilePath(const std::string& pattern, int tile);
// Whether a path names a tile set (texture.h's rule: a token, or a tile number with tiles beside it).
bool isUdimPath(const std::string& path);
// Whether `path` is an image's own name ("density.png"), not a tile set's pattern or a tile's
// ("density.<UDIM>.png", "density.1001.png", "head_1002.png"): a file there is read before the
// tiles beside it (texture.h), so a map that becomes a tile set (its scalp's UVs reach another
// tile) removes it when it writes its tiles (TexturePaint::save).
bool isPlainImagePath(const std::string& path);
// Every file a texture map's path stands for, for a host that copies, deletes or keeps the bytes
// of a map's image (undo): `path` itself for one image; for a tile set (a path naming one, or
// `tiles` given: a paint's, TexturePaint::udimLayout) each tile's file of udimPathFor(path), the
// tiles there now and `tiles`, ascending, after `path` itself when it is a plain name
// (isPlainImagePath: the one image the set replaces). Files that are not there are listed too (a
// write makes them).
std::vector<std::string> mapImageFiles(const std::string& path, const std::vector<int>& tiles = {});
// A texture map's image moved or copied from `from` to `to` (a rename): the files to move, from and
// to: `from` and `to` for one image; for a tile set each tile there now, from its file of
// udimPathFor(from) to its file of udimPathFor(to).
std::vector<std::pair<std::string, std::string>> mapImageMoves(const std::string& from, const std::string& to);
// Whether the image at `to` is the one at `from` (one image: the same bytes; a tile set: each of
// `from`'s tiles at its file of `to`, the same bytes). False when `from` has no file.
bool sameMapImage(const std::string& from, const std::string& to);
// The image at `from` copied to `to` (a tile set: each tile, mapImageMoves), no file overwritten,
// folders made. Returns the files written.
std::vector<std::string> copyMapImage(const std::string& from, const std::string& to);

}  // namespace groomlab
