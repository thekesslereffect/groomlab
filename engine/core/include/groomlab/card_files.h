// SPDX-License-Identifier: Apache-2.0
#pragma once

// A card bake as every host makes, names, writes and records it.
//
// In memory (bakeCardsInMemory): the cards every Cards node of the strands built (their card
// layers), laid out in one atlas, and their maps baked. On disk, at a prefix <folder>/<name>_:
// <prefix><map>.tga for the seven maps in Unreal's hair card layout (cardMapNames: Depth,
// Coverage, Tangent, Attribute, Color, AO, Normal), <prefix>Atlas.json (the cells rebuilt cards keep) and
// <prefix>Preview.png (the viewport's map: the color darkened by half the occlusion, the coverage
// in alpha; display.h writeCardBakePreview makes one from the maps). A groom file keeps its bake
// beside it at <stem>_cards/<stem>_ (groom_file.h's "cardBake" names it relative to the file).
//
// A groom records its last bake in one text, the cardMaps attribute (Maya) or the session's
// card maps (GroomSession::setCardMaps): "Color=<path>;Coverage=<path>;AO=<path>;Atlas=<path>;
// Strands=<stamp>;Bake=<n>", every path the bake's prefix as the host stores it (relative to
// the scene, the groom file, or absolute) plus its file, Strands what each Cards node's cards
// were made from (cardsBakeStamp, groom_session.h; left out when unknown), Bake a number the host
// changes when the files are rewritten in place (so a viewport reads them again).

#include <memory>
#include <string>
#include <vector>

#include "groomlab/card_maps.h"
#include "groomlab/cards.h"
#include "groomlab/graph.h"

namespace groomlab {

// Depth, Coverage, Tangent, Attribute, Color, AO, Normal: the maps a bake writes, in that order.
const std::vector<std::string>& cardMapNames();

// Where a bake's files are: `prefix` plus each file's suffix.
struct CardBakePaths {
    std::string prefix;  // <folder>/<name>_

    CardBakePaths() = default;
    explicit CardBakePaths(std::string at) : prefix(std::move(at)) {}
    // <folder>/<name>_ (forward slashes).
    static CardBakePaths in(const std::string& folder, const std::string& name);
    // The bake beside a groom file: <its folder>/<stem>_cards/<stem>_.
    static CardBakePaths besideGroomFile(const std::string& groomPath);

    std::string map(const std::string& name) const { return prefix + name + ".tga"; }
    std::string atlas() const { return prefix + "Atlas.json"; }
    std::string preview() const { return prefix + "Preview.png"; }
    // The seven maps, cardMapNames' order.
    std::vector<std::string> maps() const;
    // The seven maps then the atlas: the bake as a host copies or owns it (the preview is made
    // again from the maps).
    std::vector<std::string> files() const;
    // Whether the bake is there: its atlas and color map exist.
    bool exists() const;
};

// The file suffixes of a bake (Depth.tga ... AO.tga, Atlas.json): CardBakePaths::files without
// the prefix.
const std::vector<std::string>& cardBakeSuffixes();
// "<stem>_cards/<stem>_": where a groom file's bake sits, relative to the file's folder.
std::string groomFileCardBakePrefix(const std::string& stem);

// The cardMaps text of the bake at `prefix` (as the host stores it), with its stamp ("": none
// written) and bake number.
std::string formatCardMaps(const std::string& prefix, const std::string& stamp, int bake);
// The prefix a cardMaps text names (its Color map's path without "Color.tga"); "" for none.
std::string cardMapsPrefix(const std::string& cardMaps);
// The text with every path of the bake (Color, Coverage, AO, Atlas) moved from its prefix to
// `prefix`; the other entries as they were.
std::string withCardMapsPrefix(const std::string& cardMaps, const std::string& prefix);

// A card bake in memory: the cards as the viewport and the exports make them, laid out, and
// their maps.
struct CardBake {
    StrandsPtr strands;               // what the cards were made from (they hold the card layers)
    std::shared_ptr<CardMesh> cards;  // laid out
    CardTextures textures;
    MeshPtr scalp;                    // root UVs of the attribute map
    // The maps at full precision, for an export (bakeCardMapsInMemory; then `textures` is empty).
    std::shared_ptr<const CardMaps> maps;
};
// Combines the card layers of `strands` (combineCardLayers), lays them out and bakes them at
// `resolution` (0: the Cards nodes' atlasSize, else 2048) with the occlusion strength. Throws
// std::runtime_error without cards.
CardBake bakeCardsInMemory(StrandsPtr strands, MeshPtr scalp, int resolution = 0, float occlusion = 1.0f);
// The same for a map export (card_maps.h): the maps at full precision in `maps`, at the export's
// resolution (0: the Cards nodes' atlasSize, else 2048; at most 8192), padding and supersampling
// (baked that many times bigger, then brought down).
CardBake bakeCardMapsInMemory(StrandsPtr strands, MeshPtr scalp, const CardMapExport& settings, float occlusion = 1.0f);
// The resolution an export bakes at (settings.resolution, else the cards' atlasSize, else 2048;
// 16 to 8192).
int cardMapResolution(const CardMesh& cards, const CardMapExport& settings);
// Writes the bake's files at `at` (folders made): the seven maps (side by side), the atlas and,
// with `preview`, the preview. Returns the stamp of what the cards were made from
// (cardsBakeStamp). Throws std::runtime_error.
std::string writeCardBake(const CardBake& bake, const CardBakePaths& at, bool preview = true);

}  // namespace groomlab
