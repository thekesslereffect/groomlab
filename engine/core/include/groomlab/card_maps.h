// SPDX-License-Identifier: Apache-2.0
#pragma once

// Card map exports: every map a card bake can give (the sources), packed into image files the
// way an export preset says, at any resolution, format and bit depth.
//
// The bake (bakeCardMaps) renders the strands into the atlas at full precision: coverage, depth,
// occlusion, the strands' direction and their normal as round fibers, color, root UV, root to
// tip, a random value per strand and per clump, and how many strands lie over each texel. The
// sources are those, plus a few derived from them (flow, specular, translucency): see
// cardMapSources() for each one's meaning and channels. bakeCardTextures (cards.h) is this bake
// quantized to 8 bits in Unreal's hair card layout, so the old maps and the "Unreal Hair Cards"
// preset written as 8-bit TGA are the same bytes.
//
// A preset (CardMapPreset) is a list of output files, each with one to four channels (R, G, B, A),
// each channel a source's channel ("depth", "tangent.x", "color.r"), a constant ("0", "1",
// "0.5"), optionally inverted ("1-ao"), and optionally only there when the groom has it
// ("iridescence?": dropped otherwise, a file of only such channels left out). The built-in presets
// are Unreal's hair card layout, a packed one for an Unreal material of your own, Unity HDRP
// hair, and every source its own file. A user's presets are JSON files in
// Documents/GroomLab/card_presets (every host's), or in a folder on GROOMLAB_CARD_PRESET_PATH.
//
// Values as written: every data channel is 0..1 as an integer image would hold it, in every format
// (a direction's component x as 0.5 + 0.5 x, depth 0.5 on the card plane), so a material
// decodes all formats alike; only color differs by the file's color space (sRGB or linear).

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "groomlab/cards.h"

namespace groomlab {

// A card bake's maps at full precision. Images row 0 at v = 1 (top), `size` square; every texel
// holds a value (with a limited padding, past the strands and it: the empty card's: depth 0.5,
// occlusion 1, the tangent along the card, the normal out of it, the fallback color).
struct CardMaps {
    int size = 0;
    bool iridescent = false;           // a strand had iridescence (else that map is all 0)
    std::vector<float> coverage;       // 0..1 opacity
    std::vector<float> depth;          // 0..1: 0.5 on the card plane, higher in front
    std::vector<float> ao;             // 0..1 (1: open)
    std::vector<float> rootU, rootV;   // the strands' root UV on the scalp (0 without one)
    std::vector<float> seed;           // a random value per strand (0..1)
    std::vector<float> root;           // 0 at the root .. 1 at the tip, along each strand
    std::vector<float> id;             // a random value per clump (0..1)
    std::vector<float> iridescence;    // 0..1
    std::vector<float> layers;         // strands over the texel (their coverage summed)
    std::vector<float> tangent;        // 3 a texel: the strands' unit direction in card space
    std::vector<float> normal;         // 3: the strands' unit normal as round fibers, card space
    std::vector<float> color;          // 3: linear RGB
};

// Renders the strands into the cards' atlas (see bakeCardTextures, cards.h, for the settings).
CardMaps bakeCardMaps(const StrandSet& strands, const CardMesh& cards, const CardBakeSettings& settings);
// The maps a `factor` times smaller (a box filter: coverage averaged, the other values weighted
// by it, directions renormalized): a supersampled bake brought down to the export's size.
CardMaps downsampleCardMaps(const CardMaps& maps, int factor);
// The maps quantized to 8 bits in Unreal's hair card layout (bakeCardTextures' result).
CardTextures cardTexturesFromMaps(const CardMaps& maps);

// A source the exports pack from.
struct CardMapSource {
    std::string name;         // "depth"
    std::string label;        // "Depth"
    int channels = 1;
    std::string channelNames;  // one letter a channel: "x", "xyz", "rgb", "uv"
    bool color = false;       // color data (sRGB in an sRGB file); else data, always linear
    std::string description;
};
// depth, coverage, tangent, flow, normal, color, ao, root, seed, id, rootuv, specular,
// translucency, iridescence.
const std::vector<CardMapSource>& cardMapSources();

// One channel of an output file, parsed: a source's channel or a constant, inverted or not.
struct CardMapChannel {
    int source = -1;          // index into cardMapSources(); -1: the constant
    int component = 0;
    float constant = 0.0f;
    bool invert = false;
    bool optional = false;    // "?": left out when the groom has none of it
};
// Parses "depth", "tangent.y", "color.g", "rootuv.v", "0", "1", "0.5", "1-ao", "iridescence?".
// Channel letters: r g b a, x y z w or u v, by position. Throws std::invalid_argument saying why.
CardMapChannel parseCardMapChannel(const std::string& text);
// Every channel name a packing table offers: each one-channel source, each channel of the others
// ("tangent.x"...), then "0" and "1".
std::vector<std::string> cardMapChannelNames();

struct CardMapFile {
    std::string map;                    // the {map} of the file name: "Depth", "Color", "Packed"
    std::vector<std::string> channels;  // R, G, B, A (1: gray, 2 or 3: RGB, the rest 0; 4: RGBA)
    // "auto" (sRGB, but linear in EXR), "srgb" or "linear": how color channels are written.
    // Data channels are always linear.
    std::string colorSpace = "auto";
};

struct CardMapPreset {
    std::string name;
    std::string description;
    std::string format = "tga";     // what the preset suggests: tga, png, tiff, exr
    int bitDepth = 8;
    // The normal map's green channel: "opengl" (Y up the texture: Unity, Blender, Maya) or
    // "directx" (Y down: Unreal, 3ds Max).
    std::string normal = "opengl";
    std::vector<CardMapFile> files;
    bool builtin = false;           // one of builtinCardMapPresets (not saved, not deleted)
    std::string path;               // the file a user's preset came from
};

std::string cardMapPresetToJson(const CardMapPreset& preset);
// Throws std::runtime_error (not JSON, or not a preset: what is wrong).
CardMapPreset cardMapPresetFromJson(const std::string& json);
// "" or what is wrong with it: no name, no files, a file with no channels or more than four, a
// map name used twice or unfit for a file name, a channel that does not parse, a format or bit
// depth that does not exist.
std::string checkCardMapPreset(const CardMapPreset& preset);

// "Unreal Hair Cards", "Unreal (packed)", "Unity HDRP Hair", "Separate maps (all)".
const std::vector<CardMapPreset>& builtinCardMapPresets();
// The user's card preset folder: $GROOMLAB_USER_CARD_PRESETS when set, else
// Documents/GroomLab/card_presets (Windows: the profile's; elsewhere $HOME's). "" without a home.
std::string cardMapPresetUserDir();
// GROOMLAB_CARD_PRESET_PATH's folders, then cardMapPresetUserDir(): where presets are looked up.
std::vector<std::string> cardMapPresetDirs();
// The built-in presets, then every *.json preset in cardMapPresetDirs() (a name an earlier one
// has hides a later one; files that are not presets are skipped), sorted by name after the
// built-in ones.
std::vector<CardMapPreset> cardMapPresets();
std::optional<CardMapPreset> findCardMapPreset(const std::string& name);
// Saves a preset to the user's folder as <name, file safe>.json, replacing one of that name.
// Returns the file. Throws std::runtime_error (a built-in's name, a bad preset, can't write).
std::string saveCardMapPreset(const CardMapPreset& preset);
// Deletes the user's preset of that name (its file in the user's folder). Whether one was.
bool deleteCardMapPreset(const std::string& name);

// The export's settings (the preset says what goes in each file).
struct CardMapExport {
    int resolution = 0;        // texels square (the atlas is square); 0: the Cards nodes' atlasSize, else 2048; up to 8192
    std::string format;        // tga, png, tiff, exr; "": the preset's
    int bitDepth = 0;          // 8 or 16 (PNG, TIFF), 16 or 32 (EXR: half or float; TIFF 32 float); 0: the preset's, else the format's
    int padding = -1;          // texels the values spread past the strands (mip padding); -1: Full (to the neighbouring cards)
    int supersample = 1;       // 1, 2 or 4 samples a texel each way (the bake no bigger than 8192)
    std::string nameTemplate = "{name}_{map}";  // tokens {name}, {map}, {res}; the extension added
};
// The supersampling a bake at `resolution` gets: `requested` (1, 2 or 4), halved until the bake is
// no bigger than 8192.
int cardMapSupersample(int resolution, int requested);
// "tga", "png", "tiff", "exr" (".tif" is "tiff"); "" for none of them.
std::string cardMapFormat(const std::string& text);
// The bit depths a format writes, its default first: tga {8}, png {8, 16}, tiff {8, 16, 32}, exr {16, 32}.
std::vector<int> cardMapBitDepths(const std::string& format);
// The bake settings an export asks for (its size, padding and the thinnest strand scaled by the
// supersampling), from `base` (the scalp, occlusion, color).
CardBakeSettings cardMapBakeSettings(const CardBakeSettings& base, const CardMapExport& settings, int resolution);
// The file name the template makes (no folder, no extension): "{name}_{map}" -> "hair_Depth".
std::string cardMapFileName(const std::string& nameTemplate, const std::string& name, const std::string& map, int resolution);

// Writes the preset's files from the maps into `folder` (made when missing), each
// <cardMapFileName>.<format's extension>, side by side. Returns the paths written, in the
// preset's order. Throws std::runtime_error (a bad preset or setting, a file that can't be written).
std::vector<std::string> writeCardMaps(const CardMaps& maps, const CardMapPreset& preset, const CardMapExport& settings,
                                       const std::string& folder, const std::string& name);
// One packed file's pixels, before it is written: `channels` per texel (1, 3 or 4; a two-channel
// file gets a third, 0), values as the file holds them (0..1; color in the color space asked
// for). What writeCardMaps writes; tests read it.
std::vector<float> packCardMapFile(const CardMaps& maps, const CardMapFile& file, const std::string& normal,
                                   const std::string& format, int* channels);

// The presets and sources as JSON, for hosts:
//   presets: [{name, description, format, bitDepth, normal, builtin, path, files: [{map, channels, colorSpace}]}]
//   sources: [{name, label, channels, channelNames, color, description}], plus "channels" (cardMapChannelNames)
std::string cardMapPresetsJson();
std::string cardMapSourcesJson();

// Image files at any bit depth, from floats (0..1 for integer formats: rounded to the nearest
// step; EXR as they are). `channels` 1, 3 or 4, rows top to bottom. TGA 8; PNG 8 or 16; TIFF 8,
// 16 or 32 (float); EXR 16 (half) or 32 (float), ZIP compressed. Throws std::runtime_error.
void writeImageFile(const std::string& path, const std::string& format, int bitDepth, int width, int height, int channels,
                    const std::vector<float>& pixels);

}  // namespace groomlab
