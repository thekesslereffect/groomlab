// SPDX-License-Identifier: Apache-2.0
#pragma once

// Maps painted on the scalp (PG-2, GT-1): a scalar (gray: 1 channel) or a color (RGBA: 4
// channels) over the scalp surface stored
//   - per vertex of the scalp mesh (a vertex map: channels floats per mesh point, interleaved), or
//   - per texel of an image at the scalp's UVs (a texture map: a gray or RGBA PNG).
// Painted guide layers (guide_hierarchy.h) are the third kind. All three take
// the same brush stamp as guide layers do (a sphere with the brush's falloff, or
// a custom weight), and convert into each other by resampling: a map is a
// function on the surface, each kind samples it at its own points.

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "groomlab/brush.h"
#include "groomlab/guide_set.h"
#include "groomlab/scalp_maps.h"
#include "groomlab/scalp_mesh.h"
#include "groomlab/texture.h"
#include "groomlab/transfer.h"

namespace groomlab {

// ---- Channels ---------------------------------------------------------------

// A painted map is gray (kGrayMap: one value per vertex or texel) or color (kColorMap: r g b a,
// interleaved). A channel mask picks which of a color map's channels a stamp, smooth or fill
// changes (bit k: channel k, r g b a); a gray map ignores it. Gray and color convert into each
// other: gray -> color replicates the value to r g b with a = 1; color -> gray is the luminance
// (Rec. 709, as texture maps are read), alpha dropped.
constexpr int kGrayMap = 1;
constexpr int kColorMap = 4;
constexpr unsigned kAllChannels = 0xFu;
using PaintColor = std::array<float, 4>;
float compositePaintValue(float current, float value, PaintMode mode);
// Fill interleaved gray or color map values in the selected channels, compositing each value in
// `mode`. A gray map takes color[0].
void fillMapValues(std::vector<float>& values, int channels, unsigned mask, const PaintColor& color,
                   PaintMode mode = PaintMode::Replace);

// "rgba" or any non-empty subset of those letters <-> a mask. Throws std::invalid_argument.
unsigned channelMask(const std::string& letters);
std::string channelLetters(unsigned mask);
// A color's luminance (r g b; alpha ignored).
float paintLuminance(const PaintColor& c);
// A map's values with `to` channels from `from` (1 or 4 each; the same values when equal).
std::vector<float> convertMapChannels(const std::vector<float>& values, int from, int to);

// ---- The surface a brush walks ----------------------------------------------

// Every map paints the surface as if it had no seams: a dab reaches the surface within its sphere
// that is connected to where it lands (it crosses UV seams, never a gap to another part of the
// scalp), and smoothing blends a texel with its neighbors on the surface, across a seam too.

// A scalp's triangles as painting walks over them: triangles sharing an edge are neighbors whether
// or not their UVs meet there (a UV seam), and points at one position count as one (a mesh split
// at its seams: welded within a millionth of its size). Triangles joined across edges whose UVs
// agree make a UV island. The same for every map of one surface (ScalpMesh::surfaceHash):
// surfaceTopology() shares one while something holds it (and keeps the last one built).
struct SurfaceTopology {
    std::vector<Vec3f> points;                        // the scalp's points
    std::vector<ScalpMesh::Triangle> triangles;
    std::vector<std::uint32_t> weld;                  // per point: the point standing for its position
    std::vector<std::array<std::int32_t, 3>> across;  // per triangle, edge k (corner k to k + 1): the triangle across it, -1 none
    std::vector<std::uint32_t> island;                // per triangle: its UV island (all 0 without UVs)
    std::size_t bytes() const;
};
std::shared_ptr<const SurfaceTopology> surfaceTopology(const ScalpMesh& scalp);

// The triangles a dab reaches: those connected to triangle `seed` through triangles meeting the
// sphere of `radius` around `center` (the seed counts whatever its distance). `reached` gets a
// flag per triangle (resized, cleared first); returns the triangles reached, in the order found.
std::vector<std::int32_t> reachedFaces(const SurfaceTopology& surface, std::int32_t seed, const Vec3f& center, float radius,
                                       std::vector<std::uint8_t>& reached);

// ---- Faces (the host's polygons) --------------------------------------------

// A map filled on whole faces: the host's polygons (ScalpMesh::polygons(): a quad is one face,
// not its two triangles; without them every triangle is a face of its own). What a face selection
// fills (Create Groom from faces, a new map with faces selected, Fill Selected) and what the
// Paint brush's Whole Faces fills under it.
struct ScalpPolygons {
    std::vector<std::uint32_t> polygonOf;    // per triangle: its polygon
    std::vector<std::uint32_t> firstTriangle;  // per polygon + 1: its triangles' range
    std::size_t count() const { return firstTriangle.empty() ? 0 : firstTriangle.size() - 1; }
    static ScalpPolygons of(const ScalpMesh& scalp);
};
// Per triangle: 1 when it belongs to one of the polygons `faces` (indices past the mesh ignored).
std::vector<std::uint8_t> polygonTriangles(const ScalpMesh& scalp, const std::vector<std::int32_t>& faces);
// The vertices of the triangles flagged in `triangles` (ascending, each once).
std::vector<std::uint32_t> trianglesVertices(const ScalpMesh& scalp, const std::vector<std::uint8_t>& triangles);
// The polygons a Whole Faces dab fills, as triangle flags: those on the surface the dab reaches
// (reachedFaces from the triangle nearest `center`) whose center lies within `radius`, and the
// polygon under the center whatever its size (a brush smaller than a face still fills it). Empty
// (all 0) when the dab misses the scalp.
std::vector<std::uint8_t> polygonsUnderDab(const ScalpMesh& scalp, const Vec3f& center, float radius);
// A vertex map (values per vertex: `channels` 1 or 4) filled on the vertices of polygons `faces`
// only, compositing in `mode` (fillMapValues on those). Returns the vertices changed.
std::size_t fillVertexMapFaces(std::vector<float>& values, int channels, unsigned mask, const PaintColor& color,
                               PaintMode mode, const ScalpMesh& scalp, const std::vector<std::int32_t>& faces);

// ---- Vertex maps ----------------------------------------------------------

// One brush stamp on a vertex map (values.size() == scalp.pointCount()): each
// vertex within `radius` of `center` (on the surface reached: reachedFaces from the triangle
// closest to the center) moves toward `value` by strength x the
// brush's weight there (falloff, or `weight` when given, as brush.h). A custom
// weight weighs only inside the sphere, as texture maps do, so both kinds of
// map take the same footprint (and a screen-space weight does not reach
// through the head to its far side). Returns the number of vertices changed.
std::size_t stampVertexMap(std::vector<float>& values, const ScalpMesh& scalp, const Vec3f& center, float radius,
                           float strength, float value, const BrushWeightFn& weight = {});
// The same on a map of `channels` (1 or 4) values per vertex: each channel in `mask` moves
// toward its own `target` (a gray map takes target[0]). Returns the vertices changed.
std::size_t stampVertexMap(std::vector<float>& values, int channels, unsigned mask, const ScalpMesh& scalp,
                           const Vec3f& center, float radius, float strength, const PaintColor& target,
                           const BrushWeightFn& weight = {});

// Each vertex's neighbors over the scalp's triangles (list[start[v] .. start[v + 1])), for
// smoothing. Points at one position are one vertex (SurfaceTopology::weld): every copy has the
// neighbors of all of them, each neighbor listed once by the point standing for its position, so a
// mesh split at its seams smooths as the same mesh welded does.
struct VertexNeighbors {
    std::vector<std::uint32_t> start, list;
    static VertexNeighbors of(const ScalpMesh& scalp);
};

// A dab's reach on a vertex map: the vertices within `radius` of `center` on the surface reached
// (the corners of reachedFaces from the triangle closest to the center), ascending, with the
// brush's weight at each (falloff, or `weight`; none of weight 0), before the strength. Found once per dab, it serves every pass of
// the dab (stampVertexReach, smoothVertexReach give what stampVertexMap and smoothVertexMap give).
using VertexReach = std::vector<std::pair<std::size_t, float>>;
VertexReach vertexReach(const ScalpMesh& scalp, const Vec3f& center, float radius, const BrushWeightFn& weight = {});
std::size_t stampVertexReach(std::vector<float>& values, int channels, unsigned mask, const ScalpMesh& scalp,
                             const VertexReach& reach, float strength, const PaintColor& target,
                             PaintMode mode = PaintMode::Replace, const std::vector<float>* strokeStart = nullptr);
std::size_t smoothVertexReach(std::vector<float>& values, int channels, unsigned mask, const ScalpMesh& scalp,
                              const VertexNeighbors& neighbors, const VertexReach& reach, float strength);

// One smoothing stamp on a vertex map: each vertex within `radius` of `center` moves toward
// the mean of its neighbors by strength x the brush's weight there (inside the sphere, as
// stampVertexMap). Returns the vertices changed.
std::size_t smoothVertexMap(std::vector<float>& values, const ScalpMesh& scalp, const VertexNeighbors& neighbors,
                            const Vec3f& center, float radius, float strength, const BrushWeightFn& weight = {});
// The same on a map of `channels` values per vertex, smoothing the channels in `mask`.
std::size_t smoothVertexMap(std::vector<float>& values, int channels, unsigned mask, const ScalpMesh& scalp,
                            const VertexNeighbors& neighbors, const Vec3f& center, float radius, float strength,
                            const BrushWeightFn& weight = {});

// The vertex map as per-corner colors for ScalpMesh::setCornerColors, so the graph reads it as a
// color set: a gray map r = g = b = value, a = 1; a color map its r g b a.
std::vector<ScalpMesh::Color> cornerColorsFromVertexMap(const std::vector<float>& values, const ScalpMesh& scalp,
                                                        int channels = kGrayMap);

// A vertex map's value at a root (barycentric over the triangle's vertices).
float vertexMapAt(const std::vector<float>& values, const ScalpMesh& scalp, const StrandRoot& root);
// A vertex map's color at a root (a gray map: r = g = b = value, a = 1).
PaintColor vertexColorAt(const std::vector<float>& values, int channels, const ScalpMesh& scalp, const StrandRoot& root);

// ---- Texture maps ---------------------------------------------------------

// A rectangle of texels: columns [x0, x1), rows [y0, y1) (rows top-down, as the image's). Empty
// when x1 <= x0 or y1 <= y0.
struct TexelRect {
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    bool empty() const { return x1 <= x0 || y1 <= y0; }
    // The smallest rectangle holding both.
    void unite(const TexelRect& other);
    void add(int x, int y) { unite({x, y, x + 1, y + 1}); }
    static TexelRect all(int width, int height) { return {0, 0, width, height}; }
    bool operator==(const TexelRect& o) const { return x0 == o.x0 && y0 == o.y0 && x1 == o.x1 && y1 == o.y1; }
};

// Where each texel of a width x height image at a scalp's UVs lies on the scalp (its root and
// position; face -1 off it), and a grid over the covered texels' positions for sphere queries.
// The same for every map of that scalp surface and size: texelTable() shares one.
//
// Seams: a covered texel's neighbors (the 8 around it) are the texels beside it in the image when
// they are covered by its own UV island. A texel with one that is not (beside an island's edge,
// another island, or the image's border) is a seam texel: its neighbor that way is the texel
// showing the surface one texel step away across that edge (the step taken on the surface,
// unfolded over the edges it crosses, into whatever island lies there), or none past the mesh's
// border. And the uncovered texels within kPadding of the covered ones (4-neighbor steps) copy
// the nearest covered texel (the viewport's preview: bilinear samples and mips at an island's
// edge do not pull in the background).
struct TexelTable {
    static constexpr std::uint32_t kNoTexel = 0xFFFFFFFFu;
    static constexpr int kPadding = 8;
    int width = 0, height = 0;
    std::vector<StrandRoot> roots;
    std::vector<std::uint8_t> covered;  // 0 off the scalp, 1 on it (roots[i].face >= 0), 2 a seam texel
    std::vector<Vec3f> positions;
    std::size_t coveredCount = 0;
    Vec3f gridLo;
    float cell = 1.0f;
    int nx = 0, ny = 0, nz = 0;
    std::vector<std::uint32_t> cellStart;  // nx*ny*nz + 1 offsets into cellTexels
    std::vector<std::uint32_t> cellTexels;
    std::shared_ptr<const SurfaceTopology> surface;
    // The seam texels, ascending, and their 8 neighbors each (rows y - 1, y, y + 1, columns x - 1,
    // x, x + 1 within each, the texel itself skipped; kNoTexel none).
    std::vector<std::uint32_t> seamTexels, seamNeighbors;
    // The padded texels, ascending, and the covered texel each copies.
    std::vector<std::uint32_t> padTexels, padSources;
    // A covered texel's 8 neighbors (kNoTexel none), in seamNeighbors' order.
    std::array<std::uint32_t, 8> neighbors(std::size_t texel) const;
    std::size_t bytes() const;
};

// The table of `scalp` (which needs UVs; throws std::invalid_argument without them) at that size:
// built (rows in parallel, the same table as one row at a time) or shared with the maps that
// asked for the same surface (ScalpMesh::surfaceHash) and size before, while one still holds it
// (the last one built is kept too).
std::shared_ptr<const TexelTable> texelTable(const ScalpMesh& scalp, int width, int height);

// What a stroke changed on a texture map, tile by tile (TexturePaint::kEditTile square, cut at
// the image's right and bottom edges): each changed tile's values before and after it
// (channels per texel, the tile's rows top-down). For undo and redo: a few tiles instead of the
// whole image.
struct TextureEdit {
    int width = 0, height = 0, channels = 0;
    std::vector<std::uint32_t> tiles;  // tile indices (row-major over the tile grid), ascending
    std::vector<float> before, after;  // the tiles' values, one tile after another
    bool empty() const { return tiles.empty(); }
    std::size_t bytes() const {
        return (before.size() + after.size()) * sizeof(float) + tiles.size() * sizeof(std::uint32_t);
    }
};

// A gray or RGBA image at the scalp's UVs, painted in 3D: every texel whose center
// lies in a UV triangle knows its point on the scalp, so a brush sphere in
// world space finds its texels through a grid over them. Texel (0, 0) is the
// image's top-left, as files store it (v = 1 at the top, as Texture samples).
class TexturePaint {
public:
    static constexpr int kEditTile = 32;

    // A blank gray map (every texel `value`) of width x height on `scalp` (which needs UVs;
    // throws std::invalid_argument without them, or on a non-positive size).
    TexturePaint(const ScalpMesh& scalp, int width, int height, float value = 0.0f);
    // A blank color map, every texel `color`.
    TexturePaint(const ScalpMesh& scalp, int width, int height, const PaintColor& color);
    // The image as the map (any size; the table is built at that size): its luminance for a
    // gray map (channels 1), its r g b a for a color map (4). Throws std::invalid_argument for
    // a UDIM tile set (as the conversions below do).
    TexturePaint(const ScalpMesh& scalp, const Texture& image, int channels = kGrayMap);

    int width() const { return width_; }
    int height() const { return height_; }
    int channels() const { return channels_; }
    // channels() values per texel, interleaved, rows top-down. The writable one counts as a
    // change of every texel (dirty()).
    const std::vector<float>& values() const { return values_; }
    std::vector<float>& values() {
        dirty_ = TexelRect::all(width_, height_);
        return values_;
    }
    // A texel's color (a gray map: r = g = b = value, a = 1).
    PaintColor colorAt(int x, int y) const;
    // Turns the map gray (1) or color (4) in place (convertMapChannels).
    void setChannels(int channels);
    // Whether the texel lies on the scalp (in some UV triangle).
    bool covered(int x, int y) const { return table_->covered[index(x, y)] != 0; }
    // The covered texel's point on the scalp (face -1 elsewhere), and its position there.
    const StrandRoot& root(int x, int y) const { return table_->roots[index(x, y)]; }
    const Vec3f& position(int x, int y) const { return table_->positions[index(x, y)]; }
    std::size_t coveredCount() const { return table_->coveredCount; }
    const std::shared_ptr<const TexelTable>& table() const { return table_; }

    // One brush stamp: covered texels within `radius` of `center` on the surface reached
    // (reachedFaces from the nearest texel's triangle: both sides of a UV seam) move toward
    // `value` by strength x weight (as stampVertexMap; every channel of a color map). Returns
    // the number of texels changed.
    std::size_t stamp(const Vec3f& center, float radius, float strength, float value, const BrushWeightFn& weight = {});
    // The same toward a color, in the channels of `mask` (a gray map takes target[0]).
    std::size_t stamp(const Vec3f& center, float radius, float strength, const PaintColor& target, unsigned mask,
                      const BrushWeightFn& weight = {});
    // One smoothing stamp: covered texels within `radius` of `center` move toward the mean of
    // themselves and their neighbors on the surface (TexelTable::neighbors: across UV seams) by
    // strength x weight, in the channels of `mask`. Returns the texels changed.
    std::size_t smooth(const Vec3f& center, float radius, float strength, const BrushWeightFn& weight = {},
                       unsigned mask = kAllChannels);
    // A dab's reach: the covered texels within `radius` of `center` with the brush's weight at each
    // (falloff, or `weight`; none of weight 0), before the strength. Found once per dab, it serves every pass of the
    // dab: stampReach and smoothReach give what stamp and smooth give.
    using Reach = std::vector<std::pair<std::size_t, float>>;
    // `threadSafeWeight`: the weight may be called from several threads at once (the screen
    // brush's is), so a big dab weighs its texels in parallel (the same reach, in the same order).
    Reach reachOf(const Vec3f& center, float radius, const BrushWeightFn& weight = {}, bool threadSafeWeight = false) const;
    std::size_t stampReach(const Reach& reach, float strength, const PaintColor& target, unsigned mask,
                           PaintMode mode = PaintMode::Replace, bool fromEditStart = false);
    std::size_t smoothReach(const Reach& reach, float strength, unsigned mask = kAllChannels);
    // A Whole Faces dab's reach: every covered texel of the triangles flagged in `triangles`
    // (polygonsUnderDab), weight 1, found within the sphere of `radius` around `center` holding
    // them (a dab's polygons: near it).
    Reach reachOfTriangles(const std::vector<std::uint8_t>& triangles, const Vec3f& center, float radius) const;
    // The covered texels of the triangles flagged in `triangles` set to `color` in the channels of
    // `mask` (a gray map: color[0]), composited in `mode`: a face selection filled. Returns the
    // texels changed.
    std::size_t fillTriangles(const std::vector<std::uint8_t>& triangles, const PaintColor& color, unsigned mask = kAllChannels,
                              PaintMode mode = PaintMode::Replace);
    // Every texel `value`: a color map's r g b (its alpha kept).
    void fill(float value, PaintMode mode = PaintMode::Replace);
    // The channels of `mask` set to the color's (a gray map: value = color[0]).
    void fill(const PaintColor& color, unsigned mask = kAllChannels, PaintMode mode = PaintMode::Replace);

    // Sets every covered texel from `f(root, position)`, e.g. a guide layer sampled there (a
    // color map: r = g = b = that, alpha 1).
    template <class F>
    void assign(F&& f) {
        assignColor([&](const StrandRoot& root, const Vec3f& p) {
            const float v = f(root, p);
            return PaintColor{v, v, v, 1.0f};
        });
    }
    // Sets every covered texel from `f(root, position)`, a PaintColor (a gray map takes its r).
    template <class F>
    void assignColor(F&& f) {
        requireNotEditing("assign");
        const auto n = static_cast<std::size_t>(channels_);
        const TexelTable& t = *table_;
        for (std::size_t i = 0; i < t.roots.size(); ++i) {
            if (t.roots[i].face < 0) continue;
            const PaintColor color = f(t.roots[i], t.positions[i]);
            for (std::size_t c = 0; c < n; ++c) values_[i * n + c] = color[c];
        }
        dirty_ = TexelRect::all(width_, height_);
    }

    // The texels changed since clearDirty() (or since the map was made): stamps, smoothing,
    // fills, conversions and applied edits add theirs. A viewport preview updates only those.
    const TexelRect& dirty() const { return dirty_; }
    void clearDirty() { dirty_ = {}; }

    // Undo by tiles: between beginEdit() and endEdit(), the first change to each tile keeps the
    // tile as it was; endEdit() returns the tiles that changed, before and after (tiles whose
    // values came back to what they were are left out). Only stamps and smoothing may run
    // during an edit (a fill, conversion or assign throws std::logic_error).
    void beginEdit();
    bool editing() const { return editing_; }
    TextureEdit endEdit();
    // Writes an edit's values before (undo) or after (redo) it into its tiles. Throws
    // std::invalid_argument when the edit is of another size or channel count.
    void applyEdit(const TextureEdit& edit, bool after);

    // The map as 8-bit values, channels() per texel, uncovered texels taking the nearest
    // covered value up to `padding` texels away (so bilinear samples at UV seams stay clean),
    // 0 beyond.
    std::vector<std::uint8_t> toBytes(int padding = 8) const;
    // `rect` of toBytes(0) (the covered texels as they are, 0 elsewhere) into `bytes`, which
    // holds the whole image (channels() per texel, rows top-down).
    void toBytesUnpadded(const TexelRect& rect, std::uint8_t* bytes) const;
    // The map as the viewport's preview shows it (TexturePreview), 8-bit, channels() per texel:
    // the covered texels as they are, the padded ones (TexelTable::padTexels) their source's
    // value, 0 beyond.
    std::vector<std::uint8_t> toPreviewBytes() const;
    // The map as 8-bit gray (toBytes; a color map's luminance).
    std::vector<std::uint8_t> toGray8(int padding = 8) const;
    // Writes the map as a PNG of its channels, gray or RGBA (toBytes).
    void save(const std::string& path, int padding = 8) const;
    // The PNG save() writes, as bytes (written to a file they are that file).
    std::vector<std::uint8_t> pngBytes(int padding = 8) const;

private:
    std::size_t index(int x, int y) const { return static_cast<std::size_t>(y) * static_cast<std::size_t>(width_) + static_cast<std::size_t>(x); }
    std::size_t texelCount() const { return static_cast<std::size_t>(width_) * static_cast<std::size_t>(height_); }
    int tilesAcross() const { return (width_ + kEditTile - 1) / kEditTile; }
    // While editing: keeps the tiles of `reach`'s texels as they are, those not kept yet.
    void keepTiles(const Reach& reach);
    float editStartValue(std::size_t texel, std::size_t channel) const;
    void requireNotEditing(const char* what) const;

    int width_ = 0, height_ = 0;
    int channels_ = kGrayMap;
    std::vector<float> values_;  // channels_ per texel
    std::shared_ptr<const TexelTable> table_;
    TexelRect dirty_;
    // The edit being recorded: per tile, where its values before are kept (slot + 1; 0: not yet).
    bool editing_ = false;
    std::vector<std::uint32_t> editSlot_;
    std::vector<std::size_t> editOffset_;  // per tile: offset in editBefore_ + 1; 0 not kept
    std::vector<std::uint32_t> editTiles_;
    std::vector<float> editBefore_;
};

// A texture map's paint as every host opens it for a stroke or a fill: the file's image (any
// size) when the file is there, else a blank map `resolution` square (4 to 16384): gray 0, or
// black with alpha 1 for a color map (`channels` 4). Throws std::runtime_error when the scalp has
// no UVs to paint on.
std::shared_ptr<TexturePaint> openTexturePaint(const ScalpMesh& scalp, const std::string& path, int channels, int resolution);
// Throws std::runtime_error when `path` is not a PNG: a stroke writes PNG bytes into its file
// (link the map to its image again, which paints a PNG copy).
void requirePaintablePng(const std::string& path);

// The paint a host keeps for a texture map between strokes, so the next stroke goes on painting
// the values the last one left (nothing decoded) while the file is what it wrote, on the same
// scalp and in the same channels; otherwise the file is opened again as it reads now.
struct KeptTexturePaint {
    std::shared_ptr<TexturePaint> paint;
    std::string path;
    std::uint64_t scalp = 0;  // the contentHash of the scalp it was opened on
    std::uint64_t stamp = 0;  // the file's stamp (fileStamp) when opened or last written

    // Whether the kept paint is what `path` holds for this scalp and these channels.
    bool holds(const std::string& file, const ScalpMesh& on, int channels) const;
    // The file's paint opened afresh (openTexturePaint), kept with its stamp.
    static KeptTexturePaint open(const ScalpMesh& on, const std::string& file, int channels, int resolution);
    // The kept paint when it holds, else the file opened again (and kept).
    std::shared_ptr<TexturePaint> take(const ScalpMesh& on, const std::string& file, int channels, int resolution);
    // After the host wrote the paint to its file: the stamp it has now.
    void written();
};

// How the viewport shows a map's 8-bit pixels on the scalp, as an RGBA texture made from raw
// pixels (rows bottom-up, as Maya's textures from pixels sample): a gray map's value in r g b;
// a color map (mapChannels 4) its r g b, or one channel (`pick` 0-3) as gray; alpha opaque,
// except that pixels of 4 channels shown for a map of 1 keep theirs when `keepAlpha`.
struct MapDisplay {
    int mapChannels = kGrayMap;  // the stored map's: 1 or 4
    int pick = -1;               // a color map's channel shown as gray, -1: its color
    bool keepAlpha = true;       // the brush's preview keeps it; a file's image does not
    bool operator==(const MapDisplay& o) const {
        return mapChannels == o.mapChannels && pick == o.pick && keepAlpha == o.keepAlpha;
    }
    bool operator!=(const MapDisplay& o) const { return !(*this == o); }
};

// Converts `rect` (rows top-down) of 8-bit pixels (`channels` 1 or 4 per texel, width x height,
// rows top-down) into `rgba` (width x height x 4, rows bottom-up) as `display` shows them.
void displayPixels(const std::uint8_t* pixels, int width, int height, int channels, const MapDisplay& display,
                   const TexelRect& rect, std::uint8_t* rgba);

// A texture map's live image for a viewport: 8-bit RGBA, rows bottom-up (displayPixels of the
// map's toPreviewBytes(): padded, so the islands' edges filter clean), kept between updates.
// update() converts only the texels the map changed since the last update (TexturePaint::dirty,
// which it clears) and the padding around them, the whole image when the map's
// size or the display changed. Each update has a revision; a host uploading the image asks
// which texels changed since the revision it has (changedSince), so it uploads only those.
class TexturePreview {
public:
    // Returns the texels updated (rows top-down; empty when nothing changed).
    TexelRect update(TexturePaint& paint, const MapDisplay& display);
    int width() const { return width_; }
    int height() const { return height_; }
    const MapDisplay& display() const { return display_; }
    const std::vector<std::uint8_t>& rgba() const { return rgba_; }
    std::uint64_t revision() const { return revision_; }
    // Changes when the image was made anew (another size or display): a host remakes its texture.
    // Unique in the process (no two previews, or two layouts of one, share one).
    std::uint64_t layout() const { return layout_; }
    // The texels (rows top-down) changed after `revision` of `layout`: the whole image for
    // another layout or a revision older than the kept history; empty when it is current.
    TexelRect changedSince(std::uint64_t revision, std::uint64_t layout) const;

private:
    int width_ = 0, height_ = 0;
    MapDisplay display_;
    std::vector<std::uint8_t> rgba_;
    std::uint64_t revision_ = 0, layout_ = 0;
    std::vector<std::pair<std::uint64_t, TexelRect>> history_;  // (revision, rect), the last few
};

// Writes an 8-bit PNG: `channels` 1 (gray), 2 (gray + alpha), 3 (RGB) or 4 (RGBA),
// rows top to bottom. Throws std::runtime_error when it cannot write.
void writePng(const std::string& path, int width, int height, int channels, const std::vector<std::uint8_t>& pixels);
// The PNG writePng writes, as bytes. Throws std::runtime_error on a bad size or pixel count.
std::vector<std::uint8_t> storedPngBytes(int width, int height, int channels, const std::vector<std::uint8_t>& pixels);

// An image (any format loadTexture reads) written as an 8-bit compressed PNG of the same size,
// texel for texel: gray when every texel is gray and opaque, else RGBA. A texture map linked to
// a JPEG, TGA, PSD... paints into such a copy, never into the source (painting writes PNG).
// Throws std::invalid_argument for a UDIM tile set, std::runtime_error when it cannot write.
void writeImagePng(const Texture& image, const std::string& path);

// ---- Conversions ------------------------------------------------------------

// A guide layer `name` sampled at `points`: the nearest 4 guide roots (rest
// pose) blended by inverse squared distance, as Interpolate blends it into
// strands and Scatter reads it before them. Guides without the layer count as 0.
std::vector<float> sampleGuideLayer(const GuideSet& guides, const std::string& name, const ScalpMesh* scalp,
                                    const std::vector<Vec3f>& points);
// The same from the roots (as sampleGuideLayer places them) and their values.
std::vector<float> sampleRootValues(const std::vector<Vec3f>& roots, const std::vector<float>& values,
                                    const std::vector<Vec3f>& points);

// The scalp's vertex positions where maps are sampled: the rest pose when it has one.
std::vector<Vec3f> vertexSamplePoints(const ScalpMesh& scalp);

// A texture's luminance at each vertex (its corners' UVs averaged; 0 for a vertex in no triangle).
// Texture maps are one image over UVs 0-1: the functions reading one throw
// std::invalid_argument for a UDIM tile set.
std::vector<float> vertexMapFromTexture(const Texture& image, const ScalpMesh& scalp);
// The same with `channels` per vertex: 1 the luminance, 4 the r g b a.
std::vector<float> vertexMapFromTexture(const Texture& image, const ScalpMesh& scalp, int channels);
// A vertex map rasterized into a width x height texture map (of the map's channels).
TexturePaint textureFromVertexMap(const std::vector<float>& values, const ScalpMesh& scalp, int width, int height);
TexturePaint textureFromVertexMap(const std::vector<float>& values, int channels, const ScalpMesh& scalp, int width,
                                  int height);
// A guide layer sampled at the vertices / the texels.
std::vector<float> vertexMapFromGuides(const GuideSet& guides, const std::string& name, const ScalpMesh& scalp);
TexturePaint textureFromGuides(const GuideSet& guides, const std::string& name, const ScalpMesh& scalp, int width, int height);
// A gray vertex map / a texture's luminance at each root (for a guide layer; a color vertex map
// goes through convertMapChannels to gray first).
std::vector<float> rootValuesFromVertexMap(const std::vector<float>& values, const ScalpMesh& scalp,
                                           const std::vector<StrandRoot>& roots);
std::vector<float> rootValuesFromTexture(const Texture& image, const ScalpMesh& scalp, const std::vector<StrandRoot>& roots);

// ---- Projection onto another scalp (Transfer Groom to Scalp) ----------------------

// A map painted on `from` carried onto `to` (another mesh: a new sculpt, a retopologized head,
// another character): each point of `to` is taken back onto `from` by the mapping's inverse
// (ScalpMapping::back: the fit's or the markers' inverse then the closest point of `from`, or the
// same UV on `from`), and the map's value at that point becomes its value. Each function takes
// a ScalpMapping (transfer.h: any method, built on `from` and `to`) or a ScalpFit (the bounds
// method, as before). Both meshes are read in one pose, their rest poses
// when both have one and their points otherwise (the pose the guides' transfer reads), so the
// map lands where the guides do.
//
// A vertex map (`channels` values per vertex of `from`) -> values per vertex of `to`, the old
// triangle's vertex values blended by the closest point's barycentrics. Throws
// std::invalid_argument when the values do not fit `from` (another vertex count) or `channels`
// is not 1 or 4.
std::vector<float> projectVertexMap(const std::vector<float>& values, int channels, const ScalpMesh& from,
                                    const ScalpMesh& to, const ScalpFit& fit = {});
std::vector<float> projectVertexMap(const std::vector<float>& values, int channels, const ScalpMapping& mapping);
// A texture map (the image at `from`'s UVs) -> a texture map at `to`'s UVs, `width` x `height`
// (0: the image's own size): every covered texel of `to` samples the image (bilinear; its
// luminance for a gray map, r g b a for a color one) at the UV of its closest point on `from`.
// `to` may share `from`'s UVs or have its own. Uncovered texels are left to save()'s padding.
// Throws std::invalid_argument when either mesh has no UVs, for a UDIM tile set, or for
// channels other than 1 and 4.
TexturePaint projectTexture(const Texture& image, int channels, const ScalpMesh& from, const ScalpMesh& to,
                            const ScalpFit& fit = {}, int width = 0, int height = 0);
TexturePaint projectTexture(const Texture& image, int channels, const ScalpMapping& mapping, int width = 0, int height = 0);

// A groom's painted maps projected onto another scalp (a transfer, GH-8), as every host does it
// (Maya's groomLabTransfer, the app): each vertex map by position (projectVertexMap), each
// texture map through its image (projectTexture) into a new file the host writes. `files`
// says where a stored texture path resolves and where a projected image goes:
struct MapProjectionFiles {
    std::function<std::string(const std::string& stored)> resolve;  // the full path ("" unknown)
    // Writes the projected image for the map and returns the path to store for it.
    std::function<std::string(const std::string& map, const TexturePaint& paint)> write;
};
// `only` names the maps to project (empty: every one); guide layers and other kinds are left
// alone. A map that cannot be projected (its image missing, the meshes without UVs...) keeps
// its values, with why in `warnings`. Returns the names projected.
std::vector<std::string> projectScalpMaps(ScalpMaps& maps, const std::set<std::string>& only, const ScalpMesh& from,
                                          const ScalpMesh& to, const ScalpFit& fit, const MapProjectionFiles& files,
                                          std::vector<std::string>* warnings = nullptr);
std::vector<std::string> projectScalpMaps(ScalpMaps& maps, const std::set<std::string>& only, const ScalpMapping& mapping,
                                          const MapProjectionFiles& files, std::vector<std::string>* warnings = nullptr);

}  // namespace groomlab
