// SPDX-License-Identifier: Apache-2.0
#include "groomlab/card_blocks.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <nlohmann/json.hpp>
#include <stdexcept>

#include "groomlab/guide_hierarchy.h"

#include "cards_internal.h"

namespace groomlab {

void buildCardVertices(CardMesh& mesh, bool holdInCell);  // card_atlas.cpp

namespace {

using json = nlohmann::json;

constexpr float kDegrees = 3.14159265358979f / 180.0f;

float clampPositive(float value, float fallback, float lo, float hi) {
    return std::isfinite(value) && value > 0.0f ? std::clamp(value, lo, hi) : fallback;
}

// v turned by the block's root rotation (degrees: about x, then y, then z).
Vec3f rotated(const Vec3f& v, const Vec3f& degrees) {
    Vec3f out = v;
    if (degrees.x != 0.0f) out = rotateAxisAngle(out, {1.0f, 0.0f, 0.0f}, degrees.x * kDegrees);
    if (degrees.y != 0.0f) out = rotateAxisAngle(out, {0.0f, 1.0f, 0.0f}, degrees.y * kDegrees);
    if (degrees.z != 0.0f) out = rotateAxisAngle(out, {0.0f, 0.0f, 1.0f}, degrees.z * kDegrees);
    return out;
}

// The inverse: v turned back (about z, then y, then x).
Vec3f unrotated(const Vec3f& v, const Vec3f& degrees) {
    Vec3f out = v;
    if (degrees.z != 0.0f) out = rotateAxisAngle(out, {0.0f, 0.0f, 1.0f}, -degrees.z * kDegrees);
    if (degrees.y != 0.0f) out = rotateAxisAngle(out, {0.0f, 1.0f, 0.0f}, -degrees.y * kDegrees);
    if (degrees.x != 0.0f) out = rotateAxisAngle(out, {1.0f, 0.0f, 0.0f}, -degrees.x * kDegrees);
    return out;
}

float padOf(const CardBlockAtlas& atlas) {
    return static_cast<float>(std::max(atlas.settings.padding, 0)) / static_cast<float>(std::max(atlas.settings.size, 1));
}

std::array<float, 2> blockSize(const CardBlock& b) {
    const CardFrame f = cardBlockFrame(b);
    return {std::max(f.width(), 1e-6f), std::max(f.length, 1e-6f)};
}

bool inside(const std::array<float, 4>& c) {
    return c[0] >= -1e-6f && c[1] >= -1e-6f && c[2] <= 1.0f + 1e-6f && c[3] <= 1.0f + 1e-6f && c[2] > c[0] && c[3] > c[1];
}

// The free spot nearest the bottom left for a w x h cell, `pad` from the others and the edges;
// false when none.
bool freeSpot(float w, float h, float pad, const std::vector<std::array<float, 4>>& taken, std::array<float, 4>* out) {
    if (w + 2.0f * pad > 1.0f || h + 2.0f * pad > 1.0f) return false;
    // Candidate corners: the atlas' own and each taken cell's right and top edges.
    std::vector<float> xs{pad}, ys{pad};
    for (const auto& c : taken) {
        xs.push_back(c[2] + pad);
        ys.push_back(c[3] + pad);
    }
    std::sort(xs.begin(), xs.end());
    std::sort(ys.begin(), ys.end());
    for (const float y : ys) {
        for (const float x : xs) {
            const std::array<float, 4> cell{x, y, x + w, y + h};
            if (cell[2] > 1.0f - pad + 1e-6f || cell[3] > 1.0f - pad + 1e-6f) continue;
            bool clear = true;
            for (const auto& c : taken) {
                const std::array<float, 4> grown{c[0] - pad + 1e-6f, c[1] - pad + 1e-6f, c[2] + pad - 1e-6f, c[3] + pad - 1e-6f};
                if (cardCellsOverlap(cell, grown)) {
                    clear = false;
                    break;
                }
            }
            if (clear) {
                *out = cell;
                return true;
            }
        }
    }
    return false;
}

}  // namespace

CardFrame cardBlockFrame(const CardBlock& b) {
    if (b.frameLength > 0.0f && b.frameRight > b.frameLeft) return {b.frameLeft, b.frameRight, b.frameLength};
    return {-0.5f * b.width, 0.5f * b.width, b.length};
}

// ---- Text -------------------------------------------------------------------------------------

std::string cardBlockToJson(const CardBlock& b, const CardAtlasSettings& s) {
    json out = {{"width", b.width},
                {"length", b.length},
                {"thickness", b.thickness},
                {"origin", {b.origin.x, b.origin.y, b.origin.z}},
                {"cell", {b.cell[0], b.cell[1], b.cell[2], b.cell[3]}},
                {"placed", b.placed},
                {"atlas", {{"size", s.size}, {"padding", s.padding}, {"preset", s.preset}, {"name", s.name}, {"rootAtTop", s.rootAtTop}}}};
    if (b.rootRotation.x != 0.0f || b.rootRotation.y != 0.0f || b.rootRotation.z != 0.0f) {
        out["rootRotation"] = {b.rootRotation.x, b.rootRotation.y, b.rootRotation.z};
    }
    if (b.frameLength > 0.0f) out["frame"] = {b.frameLeft, b.frameRight, b.frameLength};
    return out.dump();
}

bool cardBlockFromJson(const std::string& text, CardBlock* block, CardAtlasSettings* settings) {
    if (text.find_first_not_of(" \t\r\n") == std::string::npos) return false;
    json in;
    try {
        in = json::parse(text);
    } catch (const std::exception& e) {
        throw std::runtime_error(std::string("cardBlock: not JSON: ") + e.what());
    }
    if (!in.is_object()) throw std::runtime_error("cardBlock: not an object");
    auto vec3 = [&](const char* key) {
        Vec3f v;
        if (in.contains(key) && in[key].is_array() && in[key].size() == 3) v = {in[key][0].get<float>(), in[key][1].get<float>(), in[key][2].get<float>()};
        return v;
    };
    try {
        CardBlock b;
        b.width = clampPositive(in.value("width", b.width), b.width, 1e-4f, 1e6f);
        b.length = clampPositive(in.value("length", b.length), b.length, 1e-4f, 1e6f);
        b.thickness = clampPositive(in.value("thickness", b.thickness), b.thickness, 1e-5f, 1e6f);
        b.origin = vec3("origin");
        b.rootRotation = vec3("rootRotation");
        if (in.contains("cell") && in["cell"].is_array() && in["cell"].size() == 4) {
            for (int i = 0; i < 4; ++i) b.cell[static_cast<std::size_t>(i)] = std::clamp(in["cell"][static_cast<std::size_t>(i)].get<float>(), 0.0f, 1.0f);
            if (!(b.cell[2] > b.cell[0] && b.cell[3] > b.cell[1])) b.cell = {0.0f, 0.0f, 1.0f, 1.0f};
        }
        b.placed = in.value("placed", false);
        if (in.contains("frame") && in["frame"].is_array() && in["frame"].size() == 3) {
            const float l = in["frame"][0].get<float>(), r = in["frame"][1].get<float>(), len = in["frame"][2].get<float>();
            if (std::isfinite(l) && std::isfinite(r) && std::isfinite(len) && r > l && len > 0.0f) {
                b.frameLeft = std::max(l, -1e6f);
                b.frameRight = std::min(r, 1e6f);
                b.frameLength = std::min(len, 1e6f);
            }
        }
        CardAtlasSettings s;
        if (in.contains("atlas") && in["atlas"].is_object()) {
            const json& a = in["atlas"];
            s.size = std::clamp(a.value("size", s.size), 16, 8192);
            s.padding = std::clamp(a.value("padding", s.padding), 0, 512);
            s.preset = a.value("preset", s.preset);
            s.name = a.value("name", s.name);
            s.rootAtTop = a.value("rootAtTop", s.rootAtTop);
        }
        if (block) {
            b.name = block->name;  // the groom's own
            *block = b;
        }
        if (settings) *settings = s;
    } catch (const json::exception& e) {
        throw std::runtime_error(std::string("cardBlock: ") + e.what());
    }
    return true;
}

std::string cardBlockAtlasToJson(const CardBlockAtlas& atlas) {
    json blocks = json::array();
    for (const CardBlock& b : atlas.blocks) {
        json block = json::parse(cardBlockToJson(b, atlas.settings));
        block.erase("atlas");
        block["name"] = b.name;
        blocks.push_back(std::move(block));
    }
    const CardAtlasSettings& s = atlas.settings;
    return json{{"settings", {{"size", s.size}, {"padding", s.padding}, {"preset", s.preset}, {"name", s.name}, {"rootAtTop", s.rootAtTop}}},
                {"blocks", blocks}}
        .dump();
}

CardBlockAtlas cardBlockAtlasFromJson(const std::string& text) {
    json in = json::parse(text, nullptr, false);
    if (!in.is_object()) throw std::runtime_error("card atlas: not a JSON object");
    std::vector<std::pair<std::string, std::string>> named;
    const json settings = in.value("settings", json::object());
    for (const json& b : in.value("blocks", json::array())) {
        if (!b.is_object()) throw std::runtime_error("card atlas: a block is not an object");
        json block = b;
        block["atlas"] = settings;
        named.emplace_back(b.value("name", std::string()), block.dump());
    }
    CardBlockAtlas atlas = cardBlockAtlasOf(named);
    if (atlas.blocks.empty()) {
        CardBlock none;
        json probe = {{"atlas", settings}};
        cardBlockFromJson(probe.dump(), &none, &atlas.settings);
    }
    return atlas;
}

CardBlockAtlas cardBlockAtlasOf(const std::vector<std::pair<std::string, std::string>>& namedBlocks) {
    CardBlockAtlas atlas;
    for (const auto& [name, text] : namedBlocks) {
        CardBlock block;
        block.name = name;
        CardAtlasSettings settings;
        try {
            if (!cardBlockFromJson(text, &block, &settings)) continue;
        } catch (const std::exception& e) {
            throw std::runtime_error(name + ": " + e.what());
        }
        if (atlas.blocks.empty()) atlas.settings = settings;
        atlas.blocks.push_back(block);
    }
    return atlas;
}

// ---- Layout -------------------------------------------------------------------------------------

bool cardCellsOverlap(const std::array<float, 4>& a, const std::array<float, 4>& b) {
    return a[0] < b[2] && b[0] < a[2] && a[1] < b[3] && b[1] < a[3];
}

void packCardBlocks(CardBlockAtlas& atlas) {
    std::vector<std::array<float, 2>> sizes;
    for (const CardBlock& b : atlas.blocks) sizes.push_back(blockSize(b));
    const auto cells = detail::packRects(sizes, padOf(atlas));
    for (std::size_t i = 0; i < atlas.blocks.size(); ++i) {
        atlas.blocks[i].cell = cells[i];
        atlas.blocks[i].placed = false;
    }
}

void updateCardBlockLayout(CardBlockAtlas& atlas) {
    const bool anyPlaced = std::any_of(atlas.blocks.begin(), atlas.blocks.end(), [](const CardBlock& b) { return b.placed; });
    if (!anyPlaced) {
        packCardBlocks(atlas);
        return;
    }
    const float pad = padOf(atlas);
    // The density the placed blocks show (texture units per scene unit, their median).
    std::vector<float> densities;
    for (const CardBlock& b : atlas.blocks) {
        if (!b.placed) continue;
        const auto size = blockSize(b);
        densities.push_back(0.5f * ((b.cell[2] - b.cell[0]) / size[0] + (b.cell[3] - b.cell[1]) / size[1]));
    }
    std::sort(densities.begin(), densities.end());
    const float density = densities[densities.size() / 2];
    std::vector<std::array<float, 4>> taken;
    for (const CardBlock& b : atlas.blocks) {
        if (b.placed) taken.push_back(b.cell);
    }
    for (CardBlock& b : atlas.blocks) {
        if (b.placed) continue;
        const auto size = blockSize(b);
        // Kept while it has its shape and lies clear of the rest.
        const float w = b.cell[2] - b.cell[0], h = b.cell[3] - b.cell[1];
        const bool shaped = std::abs(w * size[1] - h * size[0]) <= 0.01f * std::max(w * size[1], h * size[0]);
        bool clear = inside(b.cell) && shaped;
        for (const auto& c : taken) clear = clear && !cardCellsOverlap(b.cell, c);
        if (!clear) {
            float d = density;
            std::array<float, 4> cell{};
            bool found = false;
            for (int attempt = 0; attempt < 40 && !found; ++attempt, d *= 0.85f) {
                found = freeSpot(size[0] * d, size[1] * d, pad, taken, &cell);
            }
            if (!found) cell = {0.0f, 0.0f, std::min(1.0f, size[0] * d), std::min(1.0f, size[1] * d)};  // nowhere: over the rest
            b.cell = cell;
        }
        taken.push_back(b.cell);
    }
}

bool fitCardBlocks(CardBlockAtlas& atlas, const std::vector<StrandsPtr>& strands) {
    bool changed = false;
    for (std::size_t i = 0; i < atlas.blocks.size(); ++i) {
        CardBlock& b = atlas.blocks[i];
        // The hair's reach in the block's space (the card's: unturned, x across, down -y).
        CardFrame want{-0.5f * b.width, 0.5f * b.width, b.length};
        const StrandSet* s = i < strands.size() ? strands[i].get() : nullptr;
        if (s && s->pointCount() > 0) {
            const std::vector<Vec3f>& p = s->positions();
            const std::vector<float>& w = s->widths();
            const bool widths = w.size() == p.size();
            float lo = std::numeric_limits<float>::max(), hi = -lo, down = 0.0f, halfWidth = 0.0f;
            for (std::size_t k = 0; k < p.size(); ++k) {
                const Vec3f d = p[k] - b.origin;
                lo = std::min(lo, d.x);
                hi = std::max(hi, d.x);
                down = std::max(down, -d.y);
                if (widths) halfWidth = std::max(halfWidth, 0.5f * w[k]);
            }
            // A margin: a strand's width and a little air (the bake's soft edges and padding).
            const float across = std::max(hi - lo, 1e-4f);
            const float side = halfWidth + 0.02f * across, end = halfWidth + 0.01f * std::max(down, 1e-4f);
            want = {lo - side, hi + side, std::max(down + end, 1e-3f)};
        }
        const CardFrame had = cardBlockFrame(b);
        const float tolerance = 0.002f * std::max(want.width(), want.length);
        const bool fitted = b.frameLength > 0.0f;
        if (fitted && std::abs(want.left - had.left) <= tolerance && std::abs(want.right - had.right) <= tolerance &&
            std::abs(want.length - had.length) <= tolerance) {
            continue;
        }
        if (b.placed) {
            // Its density kept, its root corner where it was (the top left with roots at the top).
            const float du = (b.cell[2] - b.cell[0]) / std::max(had.width(), 1e-6f);
            const float dv = (b.cell[3] - b.cell[1]) / std::max(had.length, 1e-6f);
            float w = du * want.width(), h = dv * want.length;
            const float scale = std::min({1.0f, 1.0f / std::max(w, 1e-6f), 1.0f / std::max(h, 1e-6f)});
            w *= scale;
            h *= scale;
            const float u0 = std::clamp(b.cell[0], 0.0f, 1.0f - w);
            if (atlas.settings.rootAtTop) {
                const float v1 = std::clamp(b.cell[3], h, 1.0f);
                b.cell = {u0, v1 - h, u0 + w, v1};
            } else {
                const float v0 = std::clamp(b.cell[1], 0.0f, 1.0f - h);
                b.cell = {u0, v0, u0 + w, v0 + h};
            }
        }
        b.frameLeft = want.left;
        b.frameRight = want.right;
        b.frameLength = want.length;
        changed = true;
    }
    if (changed) updateCardBlockLayout(atlas);
    return changed;
}

Vec3f nextCardBlockOrigin(const CardBlockAtlas& atlas, float width) {
    if (atlas.blocks.empty()) return {};
    float right = -std::numeric_limits<float>::max(), widest = width;
    for (const CardBlock& b : atlas.blocks) {
        const CardFrame f = cardBlockFrame(b);
        right = std::max(right, b.origin.x + std::max(0.5f * b.width, f.right));
        widest = std::max(widest, std::max(b.width, f.width()));
    }
    return {right + 0.5f * widest + 0.5f * width, 0.0f, 0.0f};
}

// ---- Geometry -------------------------------------------------------------------------------------

CardBlockMesh cardBlockMesh(const CardBlock& block) {
    CardBlockMesh m;
    const float w = std::max(block.width, 1e-6f), t = std::max(block.thickness, 1e-6f);
    const int across = std::clamp(static_cast<int>(std::lround(w / t)), 4, 64);
    for (int j = 0; j <= 1; ++j) {
        for (int i = 0; i <= across; ++i) {
            const float u = static_cast<float>(i) / static_cast<float>(across), v = static_cast<float>(j);
            const Vec3f local{(u - 0.5f) * w, -v * t, 0.0f};  // its top edge at the origin, down the card
            m.points.push_back(block.origin + rotated(local, block.rootRotation));
            m.uvs.push_back(u);
            m.uvs.push_back(v);
        }
    }
    const int row = across + 1;
    for (int i = 0; i < across; ++i) {
        // (i, 0), (i, 1), (i + 1, 1), (i + 1, 0): facing +z before the rotation.
        m.faceCounts.push_back(4);
        m.faceIndices.insert(m.faceIndices.end(), {i, row + i, row + i + 1, i + 1});
    }
    return m;
}

std::shared_ptr<ScalpMesh> cardBlockScalp(const CardBlock& block) {
    const CardBlockMesh m = cardBlockMesh(block);
    std::vector<ScalpMesh::Triangle> triangles;
    std::vector<ScalpMesh::UV> uvs;
    std::vector<std::int32_t> polygons;
    std::size_t corner = 0;
    for (const int count : m.faceCounts) {
        const int* f = m.faceIndices.data() + corner;
        for (int k = 1; k + 1 < count; ++k) {
            triangles.push_back({f[0], f[k], f[k + 1]});
            for (const int p : {f[0], f[k], f[k + 1]}) {
                uvs.push_back({m.uvs[2 * static_cast<std::size_t>(p)], m.uvs[2 * static_cast<std::size_t>(p) + 1]});
            }
        }
        polygons.push_back(count - 2);
        corner += static_cast<std::size_t>(count);
    }
    auto scalp = std::make_shared<ScalpMesh>(m.points, std::move(triangles));
    scalp->setCornerUVs(std::move(uvs));
    scalp->setPolygons(std::move(polygons));
    return scalp;
}

std::string cardBlockGuidesJson(const CardBlock& block, int count) {
    const float w = std::max(block.width, 1e-6f), length = std::max(block.length, 1e-6f);
    if (count <= 0) count = std::clamp(static_cast<int>(std::lround(w / 0.6f)), 3, 9);
    GuideHierarchy h;
    const GroupId group = h.addGroup("card");
    const float t = std::max(block.thickness, 1e-6f);
    constexpr int kPoints = 12;
    for (int k = 0; k < count; ++k) {
        const float u = (static_cast<float>(k) + 0.5f) / static_cast<float>(count);
        const Vec3f root = block.origin + rotated({(u - 0.5f) * w * 0.9f, -0.5f * t, 0.0f}, block.rootRotation);
        // Straight down the card to its length below the band's top.
        const float down = std::max(root.y - (block.origin.y - length), 0.05f * length);
        std::vector<Vec3f> points;
        for (int i = 0; i < kPoints; ++i) {
            const float f = static_cast<float>(i) / static_cast<float>(kPoints - 1);
            points.push_back(root + Vec3f{0.0f, -down * f, 0.0f});
        }
        h.addGuide(group, std::move(points));
    }
    return h.toJson();
}

std::string moveCardBlockGuides(const std::string& hierarchyJson, const CardBlock& from, const CardBlock& to) {
    if (hierarchyJson.find_first_not_of(" \t\r\n") == std::string::npos) return hierarchyJson;
    GuideHierarchy h = GuideHierarchy::fromJson(hierarchyJson);
    const std::vector<HierarchyGuide> guides = h.guides();  // (setPoints changes the list's guides)
    for (const HierarchyGuide& g : guides) {
        if (g.followsParent() || g.points.empty()) continue;
        // Its root on the old band, in the band's own frame, as shares of its width and height.
        const Vec3f local = unrotated(g.points.front() - from.origin, from.rootRotation);
        const float u = local.x / std::max(from.width, 1e-6f), v = local.y / std::max(from.thickness, 1e-6f);
        const Vec3f root = to.origin + rotated({u * to.width, v * to.thickness, local.z}, to.rootRotation);
        const Vec3f shift = root - g.points.front();
        std::vector<Vec3f> points = g.points;
        for (Vec3f& p : points) p = p + shift;
        h.setPoints(g.id, std::move(points));
    }
    return h.toJson();
}

const std::string& cardBlockGraphJson() {
    // Densities per square unit of the root band (2 x 0.3 by default: about 400 strands).
    static const std::string text = R"({
  "version": 1,
  "nodes": [
    {"name": "scalp", "type": "Scalp"},
    {"name": "guides", "type": "Guides"},
    {"name": "scatter", "type": "Scatter", "params": {"density": 700.0, "randomness": 0.8}},
    {"name": "interpolate", "type": "Interpolate"},
    {"name": "clump", "type": "Clump", "params": {"count": 14}},
    {"name": "length", "type": "Length", "params": {"fraction": 1.0, "randomness": 0.25}},
    {"name": "noise", "type": "Noise", "params": {"amplitude": 0.12, "frequency": 0.6}},
    {"name": "color", "type": "HairColor"},
    {"name": "width", "type": "Width", "params": {"width": 0.006}},
    {"name": "resample", "type": "Resample", "params": {"segments": 24}},
    {"name": "output", "type": "Output"}
  ],
  "connections": [
    {"from": "scalp", "to": "scatter", "input": "scalp"},
    {"from": "scatter", "to": "interpolate", "input": "strands"},
    {"from": "guides", "to": "interpolate", "input": "guides"},
    {"from": "scalp", "to": "interpolate", "input": "scalp"},
    {"from": "interpolate", "to": "clump", "input": "strands"},
    {"from": "clump", "to": "length", "input": "strands"},
    {"from": "length", "to": "noise", "input": "strands"},
    {"from": "noise", "to": "color", "input": "strands"},
    {"from": "color", "to": "width", "input": "strands"},
    {"from": "width", "to": "resample", "input": "strands"},
    {"from": "resample", "to": "output", "input": "strands"}
  ],
  "output": "output"
})";
    return text;
}

// ---- Bake and export ------------------------------------------------------------------------------

CardMesh cardBlockCards(const CardBlockAtlas& atlas, const std::vector<StrandsPtr>& strands, int segments) {
    CardMesh mesh;
    mesh.padding = padOf(atlas);
    mesh.atlasSize = atlas.settings.size;
    mesh.dilate = true;
    for (std::size_t i = 0; i < atlas.blocks.size(); ++i) {
        const CardBlock& b = atlas.blocks[i];
        const CardFrame frame = cardBlockFrame(b);
        const float middle = 0.5f * (frame.left + frame.right);
        CardMesh::Card card;
        const std::size_t n = detail::kRefStations;
        for (std::size_t k = 0; k < n; ++k) {
            const float f = static_cast<float>(k) / static_cast<float>(n - 1);
            card.refCenter.push_back(b.origin + Vec3f{middle, -frame.length * f, 0.0f});
            card.refSide.push_back({1.0f, 0.0f, 0.0f});
            card.refNormal.push_back({0.0f, 0.0f, 1.0f});
            card.refShading.push_back({0.0f, 0.0f, 1.0f});
            card.refWidth.push_back(frame.width());
        }
        card.textureWidth = frame.width();
        card.textureRefWidth = card.refWidth;
        card.stations = std::clamp(segments, 1, 256) + 1;
        card.across = 1;
        std::copy(b.cell.begin(), b.cell.end(), card.cell);
        if (i < strands.size() && strands[i]) {
            card.source = strands[i];
            card.strands.resize(strands[i]->strandCount());
            for (std::size_t s = 0; s < card.strands.size(); ++s) card.strands[s] = s;
        }
        card.clump = static_cast<int>(i);  // the ID map: one value per block
        card.flipAlong = atlas.settings.rootAtTop;
        card.clip = true;  // its frame is the hair's (fitted): what still strays past it is cut, not squeezed in
        detail::sampleStations(card);
        mesh.cards.push_back(std::move(card));
    }
    buildCardVertices(mesh, true);
    return mesh;
}

CardMaps bakeCardBlockMaps(const CardBlockAtlas& atlas, const std::vector<StrandsPtr>& strands, const CardMapExport& settings,
                           float occlusion, const ColorMatrix& colorToRec709) {
    if (strands.size() != atlas.blocks.size()) throw std::invalid_argument("bakeCardBlockMaps: one strand set per block");
    if (std::none_of(strands.begin(), strands.end(), [](const StrandsPtr& s) { return s && s->strandCount() > 0; })) {
        throw std::runtime_error("the atlas has no hair to bake: every block is empty");
    }
    const CardMesh cards = cardBlockCards(atlas, strands);
    const int resolution = std::clamp(settings.resolution > 0 ? settings.resolution : atlas.settings.size, 16, 8192);
    CardBakeSettings base;
    base.occlusion = occlusion;
    base.colorToRec709 = colorToRec709;
    base.dilate = -1;
    const CardBakeSettings bakeSettings = cardMapBakeSettings(base, settings, resolution);
    const StrandSet none;
    CardMaps maps = bakeCardMaps(none, cards, bakeSettings);
    if (maps.size > resolution) maps = downsampleCardMaps(maps, maps.size / resolution);
    maps.aoWorld = bakeStrandWorldAo(none, cards, resolution, occlusion, bakeSettings.dilate);
    return maps;
}

std::vector<FbxMesh> cardBlockShapes(const CardBlockAtlas& atlas, const CardShapeOptions& options) {
    std::vector<FbxMesh> out;
    const int along = std::clamp(options.segments, 1, 256), across = std::clamp(options.across, 1, 64);
    float x = 0.0f, widest = 0.0f;
    for (const CardBlock& b : atlas.blocks) widest = std::max(widest, cardBlockFrame(b).width());
    for (const CardBlock& b : atlas.blocks) {
        FbxMesh m;
        // Named apart from the block's groom and the atlas (an FBX dropped into the scene that made
        // them would merge into those of the same names): <block>_card, <atlas>_mat.
        m.name = (b.name.empty() ? std::string("block") : b.name) + "_card";
        m.material = atlas.settings.name + "_mat";
        const CardFrame frame = cardBlockFrame(b);
        const float length = std::max(frame.length, 1e-6f), width = std::max(frame.width(), 1e-6f);
        const Vec3f at = options.spread ? Vec3f{x + 0.5f * width, 0.0f, 0.0f} : Vec3f{};
        // Down the card: bent toward +z by `bend` degrees in all (a circular arc), each row
        // twisted about the card's axis by its share of `twist`.
        const float bend = options.bend * kDegrees, twist = options.twist * kDegrees;
        std::vector<Vec3f> center(static_cast<std::size_t>(along) + 1), forward(center.size()), normal(center.size());
        for (int k = 0; k <= along; ++k) {
            const float f = static_cast<float>(k) / static_cast<float>(along);
            const float a = bend * f;
            if (std::abs(bend) > 1e-6f) {
                const float r = length / bend;
                center[static_cast<std::size_t>(k)] = {0.0f, -r * std::sin(a), r * (1.0f - std::cos(a))};
            } else {
                center[static_cast<std::size_t>(k)] = {0.0f, -length * f, 0.0f};
            }
            normal[static_cast<std::size_t>(k)] = {0.0f, std::sin(a), std::cos(a)};
        }
        std::vector<Vec3f> normals;
        for (int k = 0; k <= along; ++k) {
            const float f = static_cast<float>(k) / static_cast<float>(along);
            const Vec3f axis = (center[static_cast<std::size_t>(std::min(k + 1, along))] - center[static_cast<std::size_t>(std::max(k - 1, 0))]).normalized();
            const Vec3f side = rotateAxisAngle({1.0f, 0.0f, 0.0f}, axis, twist * f);
            const Vec3f n = rotateAxisAngle(normal[static_cast<std::size_t>(k)], axis, twist * f);
            for (int j = 0; j <= across; ++j) {
                const float s = static_cast<float>(j) / static_cast<float>(across);
                m.points.push_back(at + center[static_cast<std::size_t>(k)] + side * ((s - 0.5f) * width));
                normals.push_back(n);
            }
        }
        const int row = across + 1;
        auto uvOf = [&](int k, int j) {
            const float s = static_cast<float>(j) / static_cast<float>(across), f = static_cast<float>(k) / static_cast<float>(along);
            const float v = atlas.settings.rootAtTop ? b.cell[3] - (b.cell[3] - b.cell[1]) * f : b.cell[1] + (b.cell[3] - b.cell[1]) * f;
            return std::array<float, 2>{b.cell[0] + (b.cell[2] - b.cell[0]) * s, v};
        };
        for (int k = 0; k < along; ++k) {
            for (int j = 0; j < across; ++j) {
                // Facing +z: root row left to right, then the next row back.
                const int corners[4][2] = {{k, j}, {k + 1, j}, {k + 1, j + 1}, {k, j + 1}};
                m.faceCounts.push_back(4);
                for (const auto& c : corners) {
                    const int p = c[0] * row + c[1];
                    m.faceIndices.push_back(p);
                    const auto uv = uvOf(c[0], c[1]);
                    m.uvs.insert(m.uvs.end(), uv.begin(), uv.end());
                    m.normals.push_back(normals[static_cast<std::size_t>(p)]);
                }
            }
        }
        out.push_back(std::move(m));
        x += width + 0.5f * widest;
    }
    return out;
}

void writeCardBlockShapes(const std::string& path, const CardBlockAtlas& atlas, const CardShapeOptions& options, bool meters) {
    if (atlas.blocks.empty()) throw std::runtime_error("the atlas has no blocks");
    FbxSettings settings;
    settings.meters = meters;
    writeFbx(path, cardBlockShapes(atlas, options), settings);
}

}  // namespace groomlab
