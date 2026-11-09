// SPDX-License-Identifier: Apache-2.0
#pragma once

// The follicles baked into the skin's textures, for skin shaders (a renderer's or a game
// engine's): a radial gradient round each strand's root, 1 at the root falling to 0 at `radius`
// (on the skin, in scene units), so a shader can darken, tint or bump the skin where hair leaves
// it (follicle dots, stubble shadow, a hairline's density) without the hair's geometry. A texel
// takes the gradient of the root nearest it (the gradients combined by their maximum): where
// roots crowd (a follicular unit, a dense crown) their gradients merge.
//
// Each texel of the skin's uv layout is the point on the skin under it; as the scale maps
// (scale_maps.h), a scalp whose uvs leave the 0-1 square bakes a UDIM tile set, one image per
// tile, and texels past the uv shells' edges take their neighbors' values (`padding`).

#include <cstdint>
#include <string>
#include <vector>

namespace groomlab {

class ScalpMesh;
class StrandSet;

struct FollicleMapOptions {
    int resolution = 2048;  // texels across each tile (16 to 8192)
    // How far each follicle's gradient reaches on the skin (scene units). 0: from the roots'
    // spacing, 0.4 of the median distance from a root to its nearest neighbour (the gradients
    // round neighbouring roots just short of touching).
    float radius = 0.0f;
    // How much of the radius stays at full value before the gradient falls (0: it falls from the
    // root; 1: a hard-edged dot). The fall is a smoothstep.
    float hardness = 0.0f;
    int padding = 8;  // texels each uv shell's edges grow by
};

struct FollicleMap {
    struct Tile {
        int udim = 1001;
        std::vector<std::uint16_t> value;  // rows top to bottom (v = 1 first): 65535 at a root, 0 bare skin
    };
    int resolution = 0;
    float radius = 0.0f;  // the radius baked with (measured when the options gave 0)
    bool udim = false;    // a tile set (the uvs leave the 0-1 square)
    std::vector<Tile> tiles;
    std::size_t texels = 0, touched = 0;  // the skin's texels, and those within a follicle's radius
    std::size_t follicles = 0;            // the roots baked
};

// The follicles of `strands` (their roots on `skin`, the scalp they grew on) on `skin`'s uvs.
// Throws std::invalid_argument when the skin has no uvs, the resolution is out of range or no
// strand is rooted on the skin.
FollicleMap bakeFollicleMap(const ScalpMesh& skin, const StrandSet& strands, const FollicleMapOptions& options = {});

// The radius `radius` 0 stands for: 0.4 of the median nearest-neighbour distance between the roots
// (0 when fewer than two roots are on the skin).
float follicleMapAutoRadius(const ScalpMesh& skin, const StrandSet& strands);

// The map's file name: <prefix>FollicleMap.png (16-bit gray), or <prefix>FollicleMap.<tile>.png
// for each tile of a tile set; followed by <UDIM> where a texture names the tile set.
inline const char* const kFollicleMapName = "FollicleMap";
std::string follicleMapPath(const std::string& prefix, bool udim);
// Writes every tile; returns the files written. Throws std::runtime_error when one cannot be.
std::vector<std::string> writeFollicleMap(const FollicleMap& map, const std::string& prefix);

}  // namespace groomlab
