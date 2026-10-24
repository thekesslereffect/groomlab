// SPDX-License-Identifier: Apache-2.0
#pragma once

// Groom to cards (game hair): strands grouped into clumps, each clump into
// `cardsPerClump` x `layers` groups of strands lying together (k-means over
// where they run in the clump's cross-section, in depth bands), each group a
// ribbon card following its own strands' mean curve, turned the way they lie
// (their principal direction across, per station), as wide and as long as
// they reach, with a place in a UV atlas. Built from the evaluated strands,
// so cards follow every grooming change live.
//
// Texture space: cards share a few texture variations (`variations`): cards
// alike in length, width, the way their strands lie and their color use one
// texture, baked from the most typical of them, every other one mirrored so
// the repeat doesn't show. Variations are packed by their size in the world,
// so texel density is even over the atlas. `variations` 0 gives every card
// its own texture.
//
// bakeCardTextures renders the strands into the atlas in Unreal's hair card
// layout: depth, coverage, tangent, attribute (root UV, seed, coordU) and
// color, plus ambient occlusion and the strands' normal.
//
// Feathers (a Propagate node's barbs: the featherId, shaftU, side and isShaft channels) are
// carded the way games do them: one alpha-tested strip per feather along its shaft, bending
// with it, lying in the vane plane and as wide as both vanes reach (the texture shows the
// barbs and the rachis), sharing a few baked feathers (`variations`) mirrored left to right as
// their vanes need; with `featherSpine` a vertex column runs down the shaft and each half folds
// to its vane's shape. Down feathers (barbs all around the shaft) get crossed cards.

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "groomlab/math.h"
#include "groomlab/ramp.h"

namespace groomlab {

class ScalpMesh;
class StrandSet;

struct CardSettings {
    // Strands are grouped by this per-strand channel (Clump writes "clumpId");
    // without it, spread-out strands are picked as centers and every strand
    // joins the nearest: as many as maxCards allows (over the cards each takes,
    // cardsPerClump x layers; 0: 60).
    std::string channel = "clumpId";
    // At most this many cards (the one budget; sub-cards, crossed and flyaway cards count) (0: no
    // limit): neighbouring clumps merge to keep within it.
    int maxCards = 0;
    // Cards per clump: each clump's strands split into this many groups of strands lying
    // together (k-means over their offsets from the clump's mean curve, at several heights),
    // each group its own card along its own mean curve. 1: one card per clump (the old
    // single-card look); a hero groom wants 4 to 8. Within `maxCards`, clumps with the fewest
    // strands per card give up sub-cards first.
    int cardsPerClump = 1;
    // Depth layers per clump (1 to 3): the clump's strands are banded by their depth (along the
    // clump's normal, out of the scalp) before they are split into sub-cards, so a clump gets
    // inner and outer shells of cards, each showing only its own strands, alternately turned
    // (`ruffle`), for depth. Cards per clump in all: cardsPerClump x layers.
    int layers = 1;
    // Fit: card width per station covers every member strand across it (plus `margin` of the
    // width kept clear each side, so the bake never touches the cell's edges) and the length
    // covers the longest member. `fitTwist` (0..1) turns each station's plane to the strands'
    // principal direction across (a PCA of their offsets), so the card lies the way they do;
    // 0 keeps the rotation-minimizing frame facing out of the scalp (the old look).
    float margin = 0.1f;
    float fitTwist = 1.0f;
    // Ruffle (0..1): alternate cards of a clump push in and out along their normal (a share of
    // their width) and turn a little the other way, so layered cards never lie in one plane
    // (no z-fighting, more volume).
    float ruffle = 0.3f;
    // Strands no card holds (clumps and groups under `minStrands`, strands without a clump):
    // the farthest from any card get thin flyaway cards of their own, up to this many (within
    // `maxCards`); the rest join the nearest card within reach (widening it) or are dropped.
    int flyawayCards = 0;
    int segments = 8;          // quads along each card
    float widthScale = 1.0f;   // times the strands' spread
    Ramp widthProfile = Ramp::constant(1.0f);  // width along the card, root to tip (taper)
    float twist = 0.0f;        // degrees the card turns around its length, root to tip (on top of the fit)
    float curvature = 0.0f;    // 0 flat, 1 a half-tube bowed out of the scalp (less flat-looking cards)
    int widthSegments = 1;     // quads across (curvature needs 2 or more)
    float minWidth = 0.002f;   // scene units
    int minStrands = 3;        // smaller groups make no card (their strands are flyaways)
    // Cards per group of strands: a single ribbon, or `crossCount` crossed around
    // its axis (2 an X, 3 a star...). Auto crosses round tufts standing off the
    // scalp (spikes, fur tufts), which a single ribbon shows as a flat blade, and
    // keeps one ribbon for hair lying along the head.
    enum class CrossStyle { Auto, Single, Cross };
    CrossStyle crossStyle = CrossStyle::Auto;
    int crossCount = 2;
    // Braids: strands a Braid node wove (its per-strand "braidId" channel, -1 for the
    // rest) leave the clumps and make one card set per braid, whatever crossStyle says:
    // a cross of crossCount (at least 2) cards along the braid's axis, or, for a braid
    // lying on the scalp (a cornrow, "braidFlat" 1), one ribbon along it facing out of
    // the skin. No sub-cards, layers or ruffle; they take their cards from `maxCards`
    // first. Off: braided strands are carded by their clumps like any other.
    bool braids = true;
    // Segments by length: each card gets segments in proportion to its length
    // (`segments` for the longest, at least `minSegments`); the UVs don't change.
    bool segmentsByLength = false;
    int minSegments = 2;
    // Vertex normals: true bends them to the hair volume (out from the scalp
    // under each station), so cards shade like the strands around them instead
    // of like flat planes; false keeps each card's own (geometric) normals.
    bool volumeNormals = true;
    int variations = 0;        // textures the cards share (0: each its own)
    bool mirror = true;        // cards sharing a texture alternate mirrored
    float padding = 0.004f;    // atlas units between textures
    // Feathers: strands with a featherId (Propagate's barbs and kept shafts, told apart by the
    // shaft's featherId and root) are carded per feather: a strip along the shaft of
    // `featherSegments` quads, spanning both vanes in their plane (the shaft off the middle
    // when the vanes differ), its texture the feather's barbs and rachis; a down feather
    // (barbs all around, side 0) a cross of `crossCount` cards along its shaft. Feathers take
    // their cards from `maxCards` first, the shortest going first past it; feathers shorter
    // than `featherMinLength` x the longest make no card. `variations` alike feathers (length,
    // vane outline, asymmetry, barb angle and curl, color) share a texture, each scaled to its
    // own length and mirrored when its longer vane is on the other side (down feathers
    // cluster apart). Auto: feathers per feather, the rest as hair; PerFeather: only the
    // feathers (hair in the input makes no card); PerVane: everything as hair (a card per
    // clumpId, i.e. per vane or split); Off: only the hair (feathers make no card, e.g. for
    // a hair Cards node after the feathers were carded by their own node).
    enum class FeatherMode { Auto, PerFeather, PerVane, Off };
    FeatherMode featherMode = FeatherMode::Auto;
    int featherSegments = 6;
    float featherMinLength = 0.0f;
    // A spine down each vaned feather's card: a column of vertices on the shaft (each segment two
    // quads, 4 x featherSegments triangles a card), each half folding out of the vane plane to
    // follow its own vane (lift, droop, a cupped vane, one vane up and the other down), so the
    // card bends along the rachis as the groom's feather does (Card::spine). Off: a flat strip of
    // one quad per segment. Down feathers' crossed cards stay flat either way.
    bool featherSpine = true;
};

// Stations along a card its color is kept at (Card::colors), root to tip.
constexpr int kCardColorStations = 8;
// The most a tint lightens a texture (and the scale of its vertex color: 0.5 x the tint, so 0.5
// is none and 1 is kCardTintMax).
constexpr float kCardTintMax = 2.0f;

struct CardMesh {
    std::vector<Vec3f> positions;        // (widthSegments + 1) per station, across the card
    std::vector<Vec3f> normals;
    std::vector<float> u, v;             // atlas UVs per vertex
    // Per vertex, linear RGB: the color the card's own strands have there over its texture's
    // (Card::colors), 0..kCardTintMax, 1 none. A host multiplies the texture's color by it; the
    // exports carry it as a vertex color of half its value (1 = twice the texture).
    std::vector<std::array<float, 3>> tints;
    // Per vertex, the card's iridescence (Card::iridescence: how much structural color the hair
    // shading mixes in; across a feather's card from its left vane's to its right's); empty when
    // no card has any.
    std::vector<float> iridescence;
    std::vector<std::uint32_t> quads;    // 4 vertex indices per quad

    struct Card {
        std::uint32_t firstVertex = 0;
        int stations = 0;
        float cell[4] = {0, 0, 1, 1};    // atlas cell: u0, v0, u1, v1 (v0 at the root)
        std::vector<Vec3f> center, side, normal;  // per station (side: unit, toward u1)
        std::vector<Vec3f> shading;      // per station: the vertex normal (empty: the card's own)
        // The card at a fixed resolution, whatever `stations` is: the stations above are
        // sampled from it, and its texture (layout, bake) is made from it, so changing the
        // segments only adds or removes edge loops and the UVs stay as they are.
        std::vector<Vec3f> refCenter, refSide, refNormal, refShading;
        std::vector<float> refWidth;
        // The card's texture-space width: the widest station (refWidth's max). The texture
        // keeps this one scale along the whole card: a point at offset x across a station
        // maps to u = 0.5 + x / textureWidth of the cell, so narrower stations (a taper)
        // show the middle band of the texture and a straight strand bakes straight.
        float textureWidth = 0.0f;
        std::vector<float> width;        // per station
        std::vector<std::size_t> strands;  // member strand indices (into `source`)
        int clump = -1;                  // the clump (cluster) the card was made from (-1: a flyaway or braid card)
        int layer = 0;                   // its depth layer in the clump (0 innermost)
        bool flyaway = false;            // a thin card for strands no clump card held
        int braid = -1;                  // the braid it stands for (a Braid node's braidId; -1: none)
        // The feather it stands for: its index among the node's feathers (the cards of a down
        // feather's cross share it); -1: hair.
        int feather = -1;
        // A feather card's spine (CardSettings::featherSpine; across 2, the columns left edge,
        // spine, right edge): the middle column lies on the shaft, `spineOffset` along `side`
        // from `center` (scene units, one place all along, so the rachis bakes straight at
        // u = 0.5 + spineOffset / textureWidth of the unmirrored cell). Each half folds out of the
        // card's plane to follow its vane: its outer edge sits where the flat card has it, lifted
        // along `normal` by its distance from the spine x its fold (the tangent of the half's
        // angle out of the plane; negative: down), so every vertex keeps the flat card's texture
        // coordinate and a barb lying in its folded half shows exactly where it is. The bake
        // projects onto the flat card as ever (across and along), with depth and tangent in each
        // half's own frame, which is what the half's vertex normals shade with. Per station
        // (fold*) and at the fixed resolution (refFold*); right is the half toward +side.
        bool spine = false;
        float spineOffset = 0.0f;
        std::vector<float> foldRight, foldLeft, refFoldRight, refFoldLeft;
        int across = 1;                  // quads across
        float curvature = 0.0f;
        // Its base: where the root row's vertices sit (left edge to right, as across the
        // stations), on the scalp its strands grow from, a strand's width above the skin (plus
        // the node's offset), following the scalp under the card (one standing on edge: its
        // lowest vertex there): the stations' row is straight across, bowed and turned with the
        // card, and floated over a curved head or cut into it. The mesh bends from the stations'
        // rows to it over the first tenth of the card; the UVs stay. Empty: the stations' row
        // (no scalp, or strands rooted elsewhere).
        std::vector<Vec3f> base;
        // The card whose texture this one shows (an index into the same list;
        // -1: its own), and whether it shows it mirrored across.
        int variationOf = -1;
        bool mirrored = false;
        // Its strands' mean color (linear RGB) at kCardColorStations along them, root to tip
        // (a tip blend, bands, feather patterns included), and the same of the strands its texture was baked
        // from (its own, the card's it shares, or the baked atlas entry's: CardAtlas::Entry::colors).
        // Their ratio is the card's tint (CardMesh::tints): textures stay shared, each card
        // shows its own strands' color. Empty: no tint.
        std::vector<std::array<float, 3>> colors, textureColors;
        // Its strands' mean iridescence (FeatherColor's channel, 0-1) on its left and right edges: a
        // feather's left and right vanes' (one vane iridescent, say), else the same both sides.
        // The card's own, not its texture's.
        std::array<float, 2> iridescence{0.0f, 0.0f};
        int node = 0;                    // the Cards node it came from: an index into CardMesh::nodes
        // The strands the card was made from, for baking; null: the strands
        // given to bakeCardTextures.
        std::shared_ptr<const StrandSet> source;
    };
    std::vector<Card> cards;
    // The Cards nodes the cards came from, by name (a node inside a compound: "compound/inner";
    // an unnamed one's layer: "#<its index>", see combineCardLayers). The baked atlas and the
    // bake's stale check go by it, so each node keeps its own cells and its own stamp. Empty:
    // one unnamed node (cardNodeOf gives "").
    std::vector<std::string> nodes;
    float padding = 0.004f;              // atlas units between textures (layoutCards)
    int atlasSize = 0;                   // suggested bake resolution (0: none)
    float thinnestStrand = 1.0f;         // the narrowest a strand bakes, in texels (CardBakeSettings::minStrandTexels)
    std::size_t vertexCount() const { return positions.size(); }
    std::size_t triangleCount() const { return quads.size() / 2; }
    std::size_t textureCount() const;    // cards with a texture of their own
};

// The cards of a strand set, without atlas cells or vertices (layoutCards adds
// them). With settings.variations, cards are grouped into that many shared textures.
std::vector<CardMesh::Card> makeCards(const StrandSet& strands, const ScalpMesh* scalp, const CardSettings& settings);
// Groups `cards` into `variations` shared textures (see CardSettings):
// variationOf and mirrored. Strands come from each card's source, else `strands`.
void shareCardTextures(std::vector<CardMesh::Card>& cards, const StrandSet* strands, int variations, bool mirror);
// Several layers' cards in one list (variation indices follow), each card keeping the Cards
// node it came from (a layer's nodes; an unnamed layer is "#<its index>"). All the layers
// share one atlas and one bake: the atlas size is the largest any layer asks for, the padding
// the smallest.
CardMesh combineCardLayers(const std::vector<std::shared_ptr<const CardMesh>>& layers);
// The name of the Cards node `card` came from ("" when the mesh names none).
const std::string& cardNodeOf(const CardMesh& mesh, const CardMesh::Card& card);

// Each Cards node's share of a laid-out mesh, in the order of mesh.nodes (one unnamed entry
// when it names none): its cards, triangles, textures (cards with a texture of their own) and
// what a bake of them is made from (`bakeHash`: the strands its cards are made of, their
// source, and its cards' vertices), so a bake can tell which nodes changed since.
struct CardNodeSummary {
    std::string node;
    std::size_t cards = 0, triangles = 0, textures = 0;
    std::size_t feathers = 0;  // feathers carded (a down feather's crossed cards count once)
    std::uint64_t bakeHash = 0;
};
std::vector<CardNodeSummary> summarizeCardNodes(const CardMesh& mesh, const StrandSet* strands);
// Packs the cards' textures into the atlas (by their size in the world) and
// builds the vertices, normals, UVs and quads. Rebuilds from scratch.
void layoutCards(CardMesh& mesh, float padding);

// makeCards + layoutCards.
CardMesh buildCards(const StrandSet& strands, const ScalpMesh* scalp, const CardSettings& settings);

// A baked atlas: where each card's texture sits, remembered by the strands the
// card was made of. Cards rebuilt later (guides brushed, the groom re-evaluated)
// take the cell they were baked with, so the maps keep fitting until the next
// bake instead of every rebuild shuffling the atlas. Saved next to the maps as
// JSON.
struct CardAtlas {
    struct Entry {
        std::string node;                    // the Cards node the card came from (CardMesh::nodes)
        std::vector<std::uint64_t> strands;  // IDs of the card's strands
        float cell[4] = {0, 0, 1, 1};        // u0, v0, u1, v1
        bool mirrored = false;
        float textureWidth = 0.0f;           // the card's texture-space width at the bake
        bool flyaway = false;
        bool feather = false;                // a feather's card (a card with no strands in common takes one of its kind)
        // The mean color of the strands the cell was baked from, root to tip (Card::colors):
        // what a card taking the cell tints from. Empty: its own (no tint).
        std::vector<std::array<float, 3>> colors;
    };
    std::vector<Entry> entries;
    bool empty() const { return entries.empty(); }
    std::string toJson() const;
    static CardAtlas fromJson(const std::string& json);  // throws std::runtime_error
};
// The atlas of a laid-out mesh (each card's strands from its source, else `strands`).
CardAtlas captureCardAtlas(const CardMesh& mesh, const StrandSet* strands);
// Gives each card of a laid-out mesh the cell, mirroring and texture width of the entry
// of its own Cards node sharing the most of its strands (none shared: an entry of the same
// kind, by a hash of the card's strands, so it still shows hair) and rebuilds the vertices,
// UVs held inside the cell. Layered Cards nodes card the same strands, so entries are only
// matched within the card's node; a node the bake never saw (added or renamed since) matches
// against every entry. Ties (the cards of a cross hold the same strands) go to an entry no
// card took yet. Returns how many cards matched an entry by shared strands.
std::size_t applyCardAtlas(CardMesh& mesh, const CardAtlas& atlas, const StrandSet* strands);

// How far the strands lie from the cards (how much the cards look like the
// groom): `samples` points along each strand of every card (or of `strands`
// when `allStrands`), each measured to the nearest card surface (past a
// card's edge or ends: to that edge). Sorted ascending, in scene units.
std::vector<float> cardFitDistances(const StrandSet& strands, const CardMesh& cards, int samples = 12,
                                    bool allStrands = false);

struct CardBakeSettings {
    int size = 2048;
    const ScalpMesh* scalp = nullptr;             // root UVs (attribute map); null: zeros
    std::array<float, 3> color = {0.3f, 0.2f, 0.12f};  // linear, for strands without melanin channels
    float strandWidth = 0.0f;                     // scene units; 0: the strands' own (or a fallback)
    // The narrowest a strand bakes, in texels, at full coverage: a viewport draws every strand
    // at least a pixel wide and opaque, and a real hair's width is a fraction of a texel, so
    // strands baked to scale came out faint, and cut out (coverage under the cards' cutoff)
    // where the strands' own drawing is solid. 0: to scale (thinner ones fainter).
    float minStrandTexels = 1.0f;
    float occlusion = 1.0f;                       // ambient occlusion strength (0: none)
    int dilate = 8;                               // pixels colors spread past the strands (mip padding)
    // Share of the card, from its root, over which coverage fades in, so the
    // cards' root ends blend into each other and the scalp instead of reading
    // as edges (0: none).
    float rootFade = 0.06f;
};

// Images row 0 at v = 1 (top), `size` square, in Unreal's hair card layout.
struct CardTextures {
    int size = 0;
    std::vector<std::uint8_t> coverage;   // 1 channel: opacity
    std::vector<std::uint8_t> depth;      // 1: distance off the card plane (128: on it, higher: in front)
    // 3: each strand's own direction in card space (x across, y along the card root to tip, z out):
    // a feather's barbs run diagonal to its shaft, and so does their tangent.
    std::vector<std::uint8_t> tangent;
    // 3: the strands' normal as round fibers in card tangent space (x across, y along the card
    // root to tip, z out), with DirectX's green as Unreal reads it (y down: 255 - an OpenGL map's;
    // cardNormalOpenGl turns it for glTF and Unity).
    std::vector<std::uint8_t> normal;
    // 4: root U, root V, seed, coordU (0 root .. 1 tip along each strand: along each barb, from
    // the shaft out, on a feather's card; the gradient along the shaft is the card's own V).
    std::vector<std::uint8_t> attribute;
    std::vector<std::uint8_t> color;      // 3: base color (sRGB)
    std::vector<std::uint8_t> occlusion;  // 1: ambient occlusion (255: open)
    // 1: the strands' iridescence (FeatherColor's channel: 255 full); empty when they have none.
    // Written as the Color map's alpha (Unity's: its Mask map's red, the metallic it leaves at 0).
    std::vector<std::uint8_t> iridescence;
};
CardTextures bakeCardTextures(const StrandSet& strands, const CardMesh& cards, const CardBakeSettings& settings);
// The normal map with OpenGL's green (y up), as glTF and Unity read it.
std::vector<std::uint8_t> cardNormalOpenGl(const CardTextures& textures);

// The groom baked onto its scalp (a hair cap): the hair near the skin, seen
// from outside, in the scalp's UVs. Low card LODs leave the skin showing
// between cards; the cap (the scalp, or a copy just above it) textured with
// this shows hair color there instead.
struct ScalpBakeSettings {
    int size = 2048;
    float reach = 0.0f;                           // how far off the skin hair counts; 0: 3% of the scalp's size
    std::array<float, 3> color = {0.3f, 0.2f, 0.12f};  // linear, for strands without melanin channels
    float strandWidth = 0.0f;                     // scene units; 0: the strands' own (or a fallback)
    bool fill = true;                             // spread the color over the whole map (skin between roots, seams)
};
struct ScalpTextures {
    int size = 0;
    std::vector<std::uint8_t> color;     // 3: the hair's color there (sRGB), filled everywhere with `fill`
    std::vector<std::uint8_t> coverage;  // 1: how much hair covers the skin (255: none shows)
};
// Throws std::invalid_argument when the scalp has no UVs.
ScalpTextures bakeScalpTextures(const StrandSet& strands, const ScalpMesh& scalp, const ScalpBakeSettings& settings);

// Writes an uncompressed TGA (1 channel: grayscale; 3: RGB; 4: RGBA). Throws std::runtime_error.
void writeTga(const std::string& path, int width, int height, int channels, const std::vector<std::uint8_t>& pixels);

}  // namespace groomlab
