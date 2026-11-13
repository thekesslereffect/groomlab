// SPDX-License-Identifier: Apache-2.0
#pragma once

// A Card Atlas: hair card textures authored block by block. Each block is a groom of its own,
// grown on a narrow root band at the top of the card (the card's width across, the root area's
// height down it, facing the viewer) and hanging down the card from it, edited as any groom is (the graph, guides, brushes, maps); all
// the blocks bake into one texture set (cards.h bakeCardMaps: one card per block, its cell the
// block's rectangle of the atlas), written by the map export's presets (card_maps.h), and each
// block's card exports as a UV-ready mesh to place by hand.
//
// Every host keeps a block on its groom as text ("cardBlock": cardBlockToJson), the atlas'
// settings with it, so a groom file of blocks is the atlas: its grooms are the blocks, in order.
// A block's space: its root band's top edge centered at `origin`, across x (width) and down -y
// (thickness), facing +z as the card does (its depth map's front: drawn on face to face from the
// front); its hair hangs down -y for `length`. Tilting the band back (rootRotation x) spreads the
// roots in depth.

#include <array>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "groomlab/card_maps.h"
#include "groomlab/cards.h"
#include "groomlab/fbx_writer.h"
#include "groomlab/math.h"
#include "groomlab/scalp_mesh.h"
#include "groomlab/strand_set.h"

namespace groomlab {

using StrandsPtr = std::shared_ptr<const StrandSet>;

// What the whole atlas shares: one texture of `size` texels square, `padding` texels between the
// blocks, the map export preset its maps are written with (card_maps.h) and the name its files
// take ({name} of the preset's template).
struct CardAtlasSettings {
    int size = 2048;
    int padding = 8;
    std::string preset = "Unreal Hair Cards";
    std::string name = "hairCards";
    // Each block's root at the top of its rectangle and its tip at the bottom, as hand-made card
    // atlases lay hair out (the maps and the card shapes' UVs alike); false: the root at the bottom
    // (as the Cards node bakes).
    bool rootAtTop = true;
    bool operator==(const CardAtlasSettings& o) const {
        return size == o.size && padding == o.padding && preset == o.preset && name == o.name && rootAtTop == o.rootAtTop;
    }
};

struct CardBlock {
    std::string name;          // the block's groom's name (the host's; not in its JSON)
    float width = 2.0f;        // across the card at its root, scene units
    float length = 15.0f;      // root to tip
    float thickness = 0.5f;    // the root band's height down the card: where the roots spread
    Vec3f origin;              // the middle of its root band's top edge (blocks side by side in a scene)
    // The root band turned about its top edge's middle, degrees: x tilts it back (its lower edge
    // away from the viewer: the roots spread in depth, as a scalp seen at an angle: the texture
    // reads with depth; 90 lies it flat, facing down the card), y turns it (one side of the card
    // nearer), z rolls it in the card's plane (the hair leaving at a slant). The card stays flat
    // facing +z; the hair hangs down it from wherever its roots are.
    Vec3f rootRotation;
    // Its rectangle of the atlas: u0, v0, u1, v1 (0..1; the root at v1 with the atlas' rootAtTop, else at v0). `placed`: set by
    // hand (moved or resized on the atlas), kept as it is; else the layout gives it one.
    std::array<float, 4> cell{0.0f, 0.0f, 1.0f, 1.0f};
    bool placed = false;
    // The card (its rectangle of the atlas, its baked texture, its exported shape) is as wide and
    // as long as the hair actually reaches (combed or noised past the width, shorter or longer
    // than `length`): from `frameLeft` to `frameRight` across (about the origin) and
    // `frameLength` down, kept up to date by fitCardBlocks. Not fitted yet (frameLength 0): the
    // card is `width` by `length`.
    float frameLeft = 0.0f, frameRight = 0.0f, frameLength = 0.0f;
};

// A block's card in its space: x from `left` to `right`, down -y from the band's top for
// `length` (the frame fitted to its hair; its width and length until it is fitted).
struct CardFrame {
    float left = -1.0f, right = 1.0f, length = 15.0f;
    float width() const { return right - left; }
};
CardFrame cardBlockFrame(const CardBlock& block);

struct CardBlockAtlas {
    CardAtlasSettings settings;
    std::vector<CardBlock> blocks;
};

// A block as a groom stores it ({"width", "length", "thickness", "origin", "rootRotation", "cell", "placed", "frame",
// "atlas": {"size", "padding", "preset", "name", "rootAtTop"}}; its name is the groom's).
std::string cardBlockToJson(const CardBlock& block, const CardAtlasSettings& settings);
// The block and atlas settings a groom's text holds; false for blank text (not a block). Throws
// std::runtime_error for text that is not a block. Values out of range are clamped.
bool cardBlockFromJson(const std::string& text, CardBlock* block, CardAtlasSettings* settings);

// The whole atlas as one text ({"settings": {...}, "blocks": [{"name", ...the block's}]}), for a
// host's tools (the Maya window's Python, a script), and back (throws std::runtime_error).
std::string cardBlockAtlasToJson(const CardBlockAtlas& atlas);
CardBlockAtlas cardBlockAtlasFromJson(const std::string& text);
// The atlas a host's grooms make: each (name, its cardBlock text) that holds a block is one, in
// order (blank text: not a block, skipped); the settings are the first block's. Throws
// std::runtime_error naming the groom whose text is not a block.
CardBlockAtlas cardBlockAtlasOf(const std::vector<std::pair<std::string, std::string>>& namedBlocks);

// ---- Layout ---------------------------------------------------------------------------------

// Every block packed afresh into the atlas at one texel density (as large as they all fit, each
// as wide as its card and as tall as its length, `padding` texels apart): no block stays placed.
void packCardBlocks(CardBlockAtlas& atlas);
// The layout after a change (a block added, removed or resized): with no block placed by hand,
// all packed afresh; else each block not placed by hand whose cell no longer fits (the wrong
// shape, over another's) is given the largest free spot of its shape the others leave, at the
// density of the rest (smaller until one is found).
void updateCardBlockLayout(CardBlockAtlas& atlas);
// The frame each block's hair needs (its `strands`, by block: every
// point's reach across and down from the band's top, a strand's width and a small margin beyond;
// none: its width and length), set where it changed by more than a hair. A block placed by hand
// grows or shrinks about its root corner at the density it had (smaller if it no longer fits the
// atlas); the layout follows (updateCardBlockLayout). True when a frame changed.
bool fitCardBlocks(CardBlockAtlas& atlas, const std::vector<StrandsPtr>& strands);
// Whether two cells overlap (their padding apart counts as touching, not overlapping).
bool cardCellsOverlap(const std::array<float, 4>& a, const std::array<float, 4>& b);
// Where a new block of `width` goes in a scene: to the right of the others (its root band's
// center), a gap of half the widest card between them.
Vec3f nextCardBlockOrigin(const CardBlockAtlas& atlas, float width);

// ---- What a host makes of a block -------------------------------------------------------------

// The block's root band as polygons: quads across its width (about square, at least 4) by one
// down its height, facing +z before its rootRotation, per point a uv (u across the width, v down
// the band).
struct CardBlockMesh {
    std::vector<Vec3f> points;
    std::vector<int> faceCounts, faceIndices;
    std::vector<float> uvs;  // 2 per point
};
CardBlockMesh cardBlockMesh(const CardBlock& block);
// The same as a scalp (its triangles, per-corner uvs, its polygons).
std::shared_ptr<ScalpMesh> cardBlockScalp(const CardBlock& block);
// A new block's guides (a GuideHierarchy's JSON, group "card"): `count` guides (0: one per 0.6 of
// its width, 3 to 9) evenly across its root band's middle (turned with it), each straight down the
// card to `length` below the band's top.
// Unrooted: the host's evaluation roots them on the band.
std::string cardBlockGuidesJson(const CardBlock& block, int count = 0);
// The guides (a GuideHierarchy's JSON) of a block whose root band changed from `from` to `to`
// (its rotation, width or height): each primary and locked guide moved, not turned, so its root
// keeps its place on the band (across it and down it, as shares of its width and height) and its
// shape hangs as it did: tilting the band spreads the roots, the hair still falls the way it was
// combed. Following children follow their parents. The guides re-root on the band at the host's
// next evaluation. Blank text stays blank.
std::string moveCardBlockGuides(const std::string& hierarchyJson, const CardBlock& from, const CardBlock& to);
// A new block's graph (graph_io.h JSON): the band scattered densely, interpolated from the guides,
// clumped, lengths varied, a little noise, tapered strands, a hair color: what a card's texture
// usually holds, to start from.
const std::string& cardBlockGraphJson();

// ---- Bake and export ----------------------------------------------------------------------------

// One card per block (a flat card over its frame (cardBlockFrame), `segments` quads along, facing
// +z, its root at the band's top, its cell the block's), baking that block's `strands` (by block; null: none, the card
// left empty). Laid out (vertices and UVs in its cell).
CardMesh cardBlockCards(const CardBlockAtlas& atlas, const std::vector<StrandsPtr>& strands, int segments = 8);
// The atlas' maps for a map export (card_maps.h): every block baked into its cell at the export's
// resolution (0: the atlas' size), padding and supersampling, with the occlusion strength and the
// colors' conversion (CardBakeSettings::colorToRec709). Throws std::invalid_argument when
// `strands` does not have one entry per block, std::runtime_error when no block has strands.
CardMaps bakeCardBlockMaps(const CardBlockAtlas& atlas, const std::vector<StrandsPtr>& strands, const CardMapExport& settings,
                           float occlusion = 1.0f, const ColorMatrix& colorToRec709 = kIdentityColorMatrix);

// The blocks' cards as meshes to place by hand: one per block (<block>_card, its material <atlas>_mat:
// apart from the blocks' grooms and the atlas, so the FBX imports beside them; its frame's size), `segments` quads
// along and `across` across, UVs in its cell (v0 at the root), its root at the origin hanging down
// -y and facing +z; bent `bend` degrees over its length (toward +z: a card curving away from the
// head) and twisted `twist` degrees root to tip; laid side by side along +x (`spread`) or each at
// the origin. Y up, in the blocks' units.
struct CardShapeOptions {
    int segments = 8;
    int across = 1;
    float bend = 0.0f;
    float twist = 0.0f;
    bool spread = true;
};
std::vector<FbxMesh> cardBlockShapes(const CardBlockAtlas& atlas, const CardShapeOptions& options);
// Writes them to `path` (binary FBX; meters for Unity). Throws std::runtime_error.
void writeCardBlockShapes(const std::string& path, const CardBlockAtlas& atlas, const CardShapeOptions& options, bool meters = false);

}  // namespace groomlab
