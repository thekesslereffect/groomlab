// SPDX-License-Identifier: Apache-2.0
#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <thread>
#include <tuple>

#include "groomlab/scalp_paint.h"
#include "groomlab/stroke.h"
#include "groomlab/texture.h"
#include "test_helpers.h"

using namespace groomlab;

namespace {

// The unit plane with UVs (u = x, v = z) and a rest pose equal to itself.
std::shared_ptr<ScalpMesh> uvPlane(int cells = 16) {
    const auto plane = test::makePlane(cells);
    auto mesh = std::make_shared<ScalpMesh>(plane->points(), plane->triangles());
    std::vector<ScalpMesh::UV> uvs;
    for (const auto& tri : mesh->triangles()) {
        for (const std::int32_t v : tri) {
            const Vec3f& p = mesh->points()[static_cast<std::size_t>(v)];
            uvs.push_back({p.x, p.z});
        }
    }
    mesh->setCornerUVs(std::move(uvs));
    return mesh;
}

std::string tempPath(const char* name) { return (std::filesystem::temp_directory_path() / name).string(); }

// A UV sphere (test::makeSphere's layout) of `radius` around `center`, with UVs: layout 0 the
// usual (u around, v up), layout 1 the same turned a quarter and shrunk into [0.1, 0.9] (another
// UV layout of the same surface).
std::shared_ptr<ScalpMesh> uvSphere(int rings, int segments, float radius = 1.0f, Vec3f center = {0, 0, 0},
                                    int layout = 0) {
    const auto base = test::makeSphere(rings, segments, radius);
    std::vector<Vec3f> points = base->points();
    for (Vec3f& p : points) p = p + center;
    auto mesh = std::make_shared<ScalpMesh>(std::move(points), base->triangles());
    std::vector<ScalpMesh::UV> uvs;
    for (const auto& tri : mesh->triangles()) {
        for (const std::int32_t v : tri) {
            const float u0 = static_cast<float>(v % (segments + 1)) / static_cast<float>(segments);
            const float v0 = 1.0f - static_cast<float>(v / (segments + 1)) / static_cast<float>(rings);
            uvs.push_back(layout == 0 ? ScalpMesh::UV{u0, v0} : ScalpMesh::UV{0.1f + 0.8f * v0, 0.1f + 0.8f * (1.0f - u0)});
        }
    }
    mesh->setCornerUVs(std::move(uvs));
    return mesh;
}

// A smooth color over the unit sphere's directions (a map painted by position): r the gray
// value, g its inverse, b and a other gradients.
PaintColor sphereColor(const Vec3f& direction) {
    const Vec3f n = direction.normalized();
    const float f = 0.5f + 0.35f * n.y + 0.1f * n.x;
    return {f, 1.0f - f, 0.5f + 0.4f * n.z, 0.6f + 0.3f * n.x};
}

// The color above as an image at layout 0's UVs of a unit sphere (each texel: the direction its
// UV names), w x h, rows top-down.
std::shared_ptr<Texture> sphereImage(int w, int h, bool gray) {
    std::vector<float> rgba;
    for (int y = 0; y < h; ++y) {
        const float v = 1.0f - (static_cast<float>(y) + 0.5f) / static_cast<float>(h);
        for (int x = 0; x < w; ++x) {
            const float u = (static_cast<float>(x) + 0.5f) / static_cast<float>(w);
            const float theta = 3.14159265f * (1.0f - v), phi = 6.2831853f * u;
            const PaintColor c = sphereColor({std::sin(theta) * std::cos(phi), std::cos(theta), std::sin(theta) * std::sin(phi)});
            if (gray) {
                rgba.insert(rgba.end(), {c[0], c[0], c[0], 1.0f});
            } else {
                rgba.insert(rgba.end(), c.begin(), c.end());
            }
        }
    }
    return std::make_shared<Texture>(w, h, std::move(rgba));
}

// Per vertex of `mesh`, the color of its direction from `center` (channels 1: r; 4: r g b a).
std::vector<float> sphereMap(const ScalpMesh& mesh, int channels, Vec3f center = {0, 0, 0}) {
    std::vector<float> out;
    for (const Vec3f& p : mesh.points()) {
        const PaintColor c = sphereColor(p - center);
        if (channels == kGrayMap) {
            out.push_back(c[0]);
        } else {
            out.insert(out.end(), c.begin(), c.end());
        }
    }
    return out;
}

}  // namespace

TEST(ScalpPaint, TextureMapsOpenAndKeepAsEveryHostDoes) {
    // A texture map's paint as the Maya brush, Maya's map command and the app open it, and keep it
    // between strokes while its file is what they wrote (scalp_paint.h).
    const auto plane = uvPlane(8);
    const std::string path = (std::filesystem::temp_directory_path() / "groomlab_kept_paint.png").u8string();
    std::remove(path.c_str());
    const std::shared_ptr<TexturePaint> blank = openTexturePaint(*plane, path, kColorMap, 2);  // not there yet: blank
    EXPECT_EQ(blank->width(), 4);  // at least 4 texels
    EXPECT_EQ(blank->channels(), kColorMap);
    EXPECT_FLOAT_EQ(blank->colorAt(0, 0)[3], 1.0f);  // black, opaque
    EXPECT_EQ(openTexturePaint(*plane, path, kGrayMap, 1 << 20)->width(), 16384);
    EXPECT_THROW(openTexturePaint(ScalpMesh(plane->points(), plane->triangles()), path, kGrayMap, 16), std::runtime_error);
    EXPECT_THROW(requirePaintablePng("masks/a.tif"), std::runtime_error);
    EXPECT_NO_THROW(requirePaintablePng("masks/a.PNG"));

    KeptTexturePaint kept;
    const std::shared_ptr<TexturePaint> first = kept.take(*plane, path, kGrayMap, 32);
    EXPECT_EQ(first->width(), 32);
    first->fill(0.5f);
    first->save(path);
    kept.written();
    EXPECT_TRUE(kept.holds(path, *plane, kGrayMap));
    EXPECT_EQ(kept.take(*plane, path, kGrayMap, 32), first) << "the file is what it wrote: painted on as it is";
    EXPECT_FALSE(kept.holds(path, *plane, kColorMap)) << "other channels: opened again";
    auto moved = std::make_shared<ScalpMesh>(*plane);
    moved->setCornerColors("x", std::vector<ScalpMesh::Color>(plane->triangleCount() * 3, {1.0f, 1.0f, 1.0f, 1.0f}));
    EXPECT_FALSE(kept.holds(path, *moved, kGrayMap)) << "another scalp: opened again";
    // The file written by something else: opened again, as it reads now.
    TexturePaint other(*plane, 8, 8, 1.0f);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    other.save(path);
    EXPECT_FALSE(kept.holds(path, *plane, kGrayMap));
    const std::shared_ptr<TexturePaint> again = kept.take(*plane, path, kGrayMap, 32);
    EXPECT_NE(again, first);
    EXPECT_EQ(again->width(), 8);
    std::remove(path.c_str());
}

TEST(ScalpPaint, VertexStampFallsOffAndClamps) {
    const auto plane = uvPlane(8);
    std::vector<float> values(plane->pointCount(), 0.0f);
    // A stamp at the center: the vertex there goes to the value, vertices near the radius barely move.
    const Vec3f center{0.5f, 0.0f, 0.5f};
    EXPECT_GT(stampVertexMap(values, *plane, center, 0.3f, 1.0f, 1.0f), 0u);
    float atCenter = 0.0f, atEdge = 0.0f, far = 0.0f;
    for (std::size_t i = 0; i < plane->pointCount(); ++i) {
        const float d = (plane->points()[i] - center).length();
        if (d < 1e-4f) atCenter = values[i];
        if (std::abs(d - 0.25f) < 0.02f) atEdge = std::max(atEdge, values[i]);
        if (d > 0.4f) far = std::max(far, values[i]);
    }
    EXPECT_NEAR(atCenter, 1.0f, 1e-5f);
    EXPECT_GT(atEdge, 0.0f);
    EXPECT_LT(atEdge, 0.3f);
    EXPECT_EQ(far, 0.0f);
    // Stamps accumulate toward the value and stop there; a custom weight replaces the sphere.
    for (int k = 0; k < 20; ++k) stampVertexMap(values, *plane, center, 0.3f, 0.5f, 1.0f);
    EXPECT_NEAR(values[0], 0.0f, 1e-6f);  // a corner, out of reach
    EXPECT_NEAR(atCenter, 1.0f, 1e-5f);
    // A custom weight weighs inside the sphere only, for vertex and texture maps alike (a
    // screen-space weight must not reach through the head): here, the sphere's left half.
    const auto left = [](const Vec3f& p) { return p.x < 0.5f ? 1.0f : 0.0f; };
    const float r2 = 0.3f * 0.3f;
    auto hit = [&](const Vec3f& p) { return (p - center).dot(p - center) <= r2 && p.x < 0.5f; };
    std::vector<float> weighed(plane->pointCount(), 0.5f);
    stampVertexMap(weighed, *plane, center, 0.3f, 1.0f, 0.0f, left);
    std::size_t vertexHits = 0;
    for (std::size_t i = 0; i < plane->pointCount(); ++i) {
        vertexHits += hit(plane->points()[i]) ? 1 : 0;
        EXPECT_NEAR(weighed[i], hit(plane->points()[i]) ? 0.0f : 0.5f, 1e-6f);
    }
    EXPECT_GT(vertexHits, 0u);
    EXPECT_LT(vertexHits, plane->pointCount() / 2);
    TexturePaint texels(*plane, 32, 32, 0.5f);
    texels.stamp(center, 0.3f, 1.0f, 0.0f, left);
    for (int y = 0; y < 32; ++y) {
        for (int x = 0; x < 32; ++x) {
            EXPECT_NEAR(texels.values()[static_cast<std::size_t>(y) * 32 + static_cast<std::size_t>(x)],
                        hit(texels.position(x, y)) ? 0.0f : 0.5f, 1e-6f);
        }
    }
    // As a color set: r = g = b = the value at each corner's vertex.
    const auto colors = cornerColorsFromVertexMap(values, *plane);
    ASSERT_EQ(colors.size(), plane->triangleCount() * 3);
    plane->setCornerColors("m", colors);
    StrandRoot root;
    root.face = 0;
    root.u = 0.25f;
    root.v = 0.25f;
    EXPECT_NEAR(plane->colorAt("m", root)[0], vertexMapAt(values, *plane, root), 1e-5f);
}

TEST(ScalpPaint, TextureTableCoversTheScalpAndStampsTexels) {
    const auto plane = uvPlane(8);
    TexturePaint paint(*plane, 64, 64, 0.0f);
    // Every texel of a plane filling the whole UV square lies on it; its position is its UV.
    EXPECT_EQ(paint.coveredCount(), 64u * 64u);
    EXPECT_NEAR(paint.position(0, 0).x, 0.5f / 64.0f, 1e-4f);
    EXPECT_NEAR(paint.position(0, 0).z, 1.0f - 0.5f / 64.0f, 1e-4f);  // row 0 is the top: v = 1
    EXPECT_NEAR(paint.position(63, 63).x, 1.0f - 0.5f / 64.0f, 1e-4f);
    const Vec3f center{0.25f, 0.0f, 0.75f};  // u = 0.25, v = 0.75: the upper-left quarter
    EXPECT_GT(paint.stamp(center, 0.1f, 1.0f, 1.0f), 0u);
    EXPECT_NEAR(paint.values()[static_cast<std::size_t>(16) * 64 + 16], 1.0f, 0.05f);  // the texel center is a bit off the stamp's
    EXPECT_EQ(paint.values()[static_cast<std::size_t>(48) * 64 + 48], 0.0f);
    EXPECT_EQ(paint.values()[static_cast<std::size_t>(16) * 64 + 40], 0.0f);  // 0.375 away, past the radius
    // Saved as a gray PNG and read back as a texture: the stamp is where it was painted.
    const std::string path = tempPath("groomlab_paint.png");
    paint.save(path);
    const TexturePtr back = loadTexture(path);
    ASSERT_EQ(back->width(), 64);
    EXPECT_NEAR(back->sample(0.25f, 0.75f)[0], 1.0f, 0.05f);
    EXPECT_NEAR(back->sample(0.75f, 0.25f)[0], 0.0f, 0.01f);
    // The same image reloaded into a paint keeps the values.
    TexturePaint again(*plane, *back);
    EXPECT_NEAR(again.values()[static_cast<std::size_t>(16) * 64 + 16], 1.0f, 0.05f);
    std::remove(path.c_str());
    // A scalp using only part of UV space: uncovered texels are not on it, and padding fills
    // the border so bilinear samples at the edge stay clean.
    auto half = uvPlane(4);
    std::vector<ScalpMesh::UV> uvs = half->cornerUVs();
    for (auto& uv : uvs) uv[0] *= 0.5f;
    half->setCornerUVs(uvs);
    TexturePaint partial(*half, 32, 32, 1.0f);
    EXPECT_NEAR(static_cast<double>(partial.coveredCount()), 32.0 * 16.0, 40.0);
    EXPECT_FALSE(partial.covered(31, 16));
    const auto gray = partial.toGray8(4);
    EXPECT_EQ(gray[16 * 32 + 8], 255);
    EXPECT_EQ(gray[16 * 32 + 18], 255);  // padded from the edge
    EXPECT_EQ(gray[16 * 32 + 30], 0);    // beyond the padding
}

TEST(ScalpPaint, FacesFillWholePolygons) {
    // A 4 x 4 plane of quads: polygon z * 4 + x is cell (x, z), its two triangles 2p and 2p + 1.
    auto plane = uvPlane(4);
    plane->setPolygons(std::vector<std::int32_t>(16, 2));
    const auto at = [](int x, int z) { return static_cast<std::size_t>(z * 5 + x); };
    // A face selection on a vertex map: the quad's four corners, nothing else.
    std::vector<float> values(plane->pointCount(), 0.0f);
    EXPECT_EQ(fillVertexMapFaces(values, kGrayMap, kAllChannels, {1.0f, 1.0f, 1.0f, 1.0f}, PaintMode::Replace, *plane, {5}), 4u);
    for (std::size_t v = 0; v < values.size(); ++v) {
        const bool corner = v == at(1, 1) || v == at(2, 1) || v == at(1, 2) || v == at(2, 2);
        EXPECT_EQ(values[v], corner ? 1.0f : 0.0f) << v;
    }
    EXPECT_EQ(fillVertexMapFaces(values, kGrayMap, kAllChannels, {0.5f, 0.5f, 0.5f, 0.5f}, PaintMode::Replace, *plane, {99}), 0u);
    // Without polygons every triangle is a face.
    auto triangles = uvPlane(4);
    std::vector<float> one(triangles->pointCount(), 0.0f);
    EXPECT_EQ(fillVertexMapFaces(one, kGrayMap, kAllChannels, {1.0f, 1.0f, 1.0f, 1.0f}, PaintMode::Replace, *triangles, {10}), 3u);
    // A texture map: the quad's texels (u and v 0.25 to 0.5), nothing else.
    TexturePaint paint(*plane, 32, 32, 0.0f);
    EXPECT_GT(paint.fillTriangles(polygonTriangles(*plane, {5}), {1.0f, 1.0f, 1.0f, 1.0f}), 0u);
    const auto texel = [&](float u, float v) {
        const int x = static_cast<int>(u * 32.0f), y = static_cast<int>((1.0f - v) * 32.0f);
        return paint.values()[static_cast<std::size_t>(y) * 32 + static_cast<std::size_t>(x)];
    };
    EXPECT_EQ(texel(0.375f, 0.375f), 1.0f);
    EXPECT_EQ(texel(0.8f, 0.375f), 0.0f);
    EXPECT_EQ(texel(0.375f, 0.8f), 0.0f);
    // A dab's faces: the one under its center however small the dab, plus those whose center it
    // covers (the four beside it, 0.25 away; not the diagonal ones, 0.35 away).
    const Vec3f middle{0.375f, 0.0f, 0.375f};
    const auto count = [](const std::vector<std::uint8_t>& flags) { return std::count(flags.begin(), flags.end(), 1); };
    const std::vector<std::uint8_t> small = polygonsUnderDab(*plane, middle, 0.05f);
    EXPECT_EQ(count(small), 2);
    EXPECT_TRUE(small[10] && small[11]);
    EXPECT_EQ(count(polygonsUnderDab(*plane, middle, 0.3f)), 10);
    // The Paint brush's Whole Faces: a small dab fills its quad's corners all the way.
    BrushToolSettings settings;
    settings.paintWholeFaces = true;
    BrushSettings brush;
    brush.mode = BrushMode::Attribute;
    brush.value = 1.0f;
    brush.strength = 1.0f;
    std::vector<float> painted(plane->pointCount(), 0.0f);
    MapCanvas canvas;
    canvas.vertices = &painted;
    const MapDab dab = MapDab::of(settings, brush, kGrayMap, false, 1.0f);
    EXPECT_TRUE(dab.wholeFaces);
    EXPECT_EQ(paintMapDab(canvas, *plane, dab, middle, 0.05f, sphereWeight(middle, 0.05f), true), 4u);
    EXPECT_EQ(painted, values);
    // ...and on a texture, the quad's texels.
    TexturePaint brushed(*plane, 32, 32, 0.0f);
    MapCanvas textureCanvas;
    textureCanvas.texture = &brushed;
    EXPECT_GT(paintMapDab(textureCanvas, *plane, dab, middle, 0.05f, sphereWeight(middle, 0.05f), true), 0u);
    EXPECT_EQ(brushed.values(), paint.values());
    // Off, the same dab paints the falloff's few vertices inside it: none of the corners, 0.177 away.
    settings.paintWholeFaces = false;
    std::vector<float> plain(plane->pointCount(), 0.0f);
    canvas.vertices = &plain;
    EXPECT_EQ(paintMapDab(canvas, *plane, MapDab::of(settings, brush, kGrayMap, false, 1.0f), middle, 0.05f,
                          sphereWeight(middle, 0.05f), true),
              0u);
}

TEST(ScalpPaint, SmoothingBlendsNeighbors) {
    // A vertex map with a hard step at x = 0.5: smoothing at the step spreads it, and only
    // within the brush; the sum barely moves (the blur takes from one side, gives to the other).
    const auto plane = uvPlane(16);
    std::vector<float> values(plane->pointCount());
    for (std::size_t i = 0; i < values.size(); ++i) values[i] = plane->points()[i].x > 0.5f ? 1.0f : 0.0f;
    const VertexNeighbors neighbors = VertexNeighbors::of(*plane);
    EXPECT_EQ(neighbors.start.size(), plane->pointCount() + 1);
    EXPECT_GT(neighbors.list.size(), plane->pointCount() * 4);
    const Vec3f center{0.5f, 0.0f, 0.5f};
    std::vector<float> smoothed = values;
    EXPECT_GT(smoothVertexMap(smoothed, *plane, neighbors, center, 0.2f, 1.0f), 0u);
    float belowStep = 0.0f, aboveStep = 1.0f, corner = 0.0f;
    for (std::size_t i = 0; i < smoothed.size(); ++i) {
        const Vec3f& p = plane->points()[i];
        if (std::abs(p.x - 0.5f) < 1e-4f && std::abs(p.z - 0.5f) < 1e-4f) belowStep = smoothed[i];  // 0, next to the first 1
        if (std::abs(p.x - 0.5625f) < 1e-4f && std::abs(p.z - 0.5f) < 1e-4f) aboveStep = smoothed[i];
        if (p.x < 0.1f || p.x > 0.9f) corner = std::max(corner, std::abs(smoothed[i] - values[i]));
    }
    EXPECT_GT(belowStep, 0.05f);
    EXPECT_LT(aboveStep, 0.95f);
    EXPECT_EQ(corner, 0.0f);  // out of reach
    for (int k = 0; k < 30; ++k) smoothVertexMap(smoothed, *plane, neighbors, center, 0.3f, 1.0f);
    EXPECT_LT(*std::max_element(smoothed.begin(), smoothed.end()), 1.0f + 1e-6f);
    // The same on a texture: a hard black/white split at column 32 blurs at the split, untouched far away.
    TexturePaint paint(*plane, 64, 64, 0.0f);
    for (int y = 0; y < 64; ++y) {
        for (int x = 32; x < 64; ++x) paint.values()[static_cast<std::size_t>(y) * 64 + static_cast<std::size_t>(x)] = 1.0f;
    }
    EXPECT_GT(paint.smooth(center, 0.15f, 1.0f), 0u);
    EXPECT_GT(paint.values()[static_cast<std::size_t>(32) * 64 + 31], 0.0f);
    EXPECT_LT(paint.values()[static_cast<std::size_t>(32) * 64 + 32], 1.0f);
    EXPECT_EQ(paint.values()[static_cast<std::size_t>(32) * 64 + 2], 0.0f);
    EXPECT_EQ(paint.values()[static_cast<std::size_t>(32) * 64 + 61], 1.0f);
}

TEST(ScalpPaint, ConversionsResampleTheSameFunction) {
    const auto plane = uvPlane(16);
    // A vertex map: 1 on x > 0.5. Through a texture and back it stays that, and at roots too.
    std::vector<float> vertices(plane->pointCount());
    for (std::size_t i = 0; i < vertices.size(); ++i) vertices[i] = plane->points()[i].x > 0.5f ? 1.0f : 0.0f;
    const TexturePaint texture = textureFromVertexMap(vertices, *plane, 64, 64);
    EXPECT_NEAR(texture.values()[static_cast<std::size_t>(32) * 64 + 60], 1.0f, 1e-4f);
    EXPECT_NEAR(texture.values()[static_cast<std::size_t>(32) * 64 + 4], 0.0f, 1e-4f);
    const std::string path = tempPath("groomlab_convert.png");
    texture.save(path);
    const TexturePtr image = loadTexture(path);
    const std::vector<float> back = vertexMapFromTexture(*image, *plane);
    ASSERT_EQ(back.size(), vertices.size());
    for (std::size_t i = 0; i < back.size(); ++i) {
        if (std::abs(plane->points()[i].x - 0.5f) < 0.07f) continue;  // the step spans a cell: texels between blend
        EXPECT_NEAR(back[i], vertices[i], 0.02f) << i;
    }
    std::vector<StrandRoot> roots(2);
    roots[0].face = 0;
    roots[0].u = roots[0].v = 0.2f;
    roots[1].face = static_cast<std::int32_t>(plane->triangleCount() - 1);
    roots[1].u = roots[1].v = 0.2f;
    const std::vector<float> fromVertices = rootValuesFromVertexMap(vertices, *plane, roots);
    const std::vector<float> fromTexture = rootValuesFromTexture(*image, *plane, roots);
    EXPECT_NEAR(fromVertices[0], 0.0f, 1e-5f);
    EXPECT_NEAR(fromVertices[1], 1.0f, 1e-5f);
    EXPECT_NEAR(fromTexture[0], 0.0f, 0.02f);
    EXPECT_NEAR(fromTexture[1], 1.0f, 0.02f);
    std::remove(path.c_str());

    // A guide layer: two guides, "tint" 1 at the right, 0 at the left: the vertices and texels
    // between blend them by distance to the roots.
    GuideSet guides;
    guides.add(1, test::straightCurve({0.1f, 0.0f, 0.5f}, {0.0f, 1.0f, 0.0f})).attributes["tint"] = 0.0f;
    guides.add(2, test::straightCurve({0.9f, 0.0f, 0.5f}, {0.0f, 1.0f, 0.0f})).attributes["tint"] = 1.0f;
    guides.rootOnMesh(*plane);
    const std::vector<float> onVertices = vertexMapFromGuides(guides, "tint", *plane);
    float left = 1.0f, right = 0.0f, middle = 0.0f;
    for (std::size_t i = 0; i < onVertices.size(); ++i) {
        const float x = plane->points()[i].x;
        if (x < 0.15f) left = std::min(left, onVertices[i]);
        if (x > 0.85f) right = std::max(right, onVertices[i]);
        if (std::abs(x - 0.5f) < 1e-4f && std::abs(plane->points()[i].z - 0.5f) < 1e-4f) middle = onVertices[i];
    }
    EXPECT_LT(left, 0.1f);
    EXPECT_GT(right, 0.9f);
    EXPECT_NEAR(middle, 0.5f, 0.05f);
    const TexturePaint onTexels = textureFromGuides(guides, "tint", *plane, 32, 32);
    EXPECT_LT(onTexels.values()[static_cast<std::size_t>(16) * 32 + 2], 0.1f);
    EXPECT_GT(onTexels.values()[static_cast<std::size_t>(16) * 32 + 29], 0.9f);
    EXPECT_NEAR(onTexels.values()[static_cast<std::size_t>(16) * 32 + 16], 0.5f, 0.06f);
    // And back onto guide roots from the vertex map.
    std::vector<StrandRoot> guideRoots = {guides.guides()[0].root, guides.guides()[1].root};
    const std::vector<float> atRoots = rootValuesFromVertexMap(onVertices, *plane, guideRoots);
    EXPECT_LT(atRoots[0], 0.15f);
    EXPECT_GT(atRoots[1], 0.85f);
}

TEST(ScalpPaint, ChannelMasksAndConversions) {
    EXPECT_EQ(channelMask("rgba"), kAllChannels);
    EXPECT_EQ(channelMask("gA"), 0xAu);
    EXPECT_EQ(channelLetters(0x5u), "rb");
    EXPECT_THROW(channelMask(""), std::invalid_argument);
    EXPECT_THROW(channelMask("rx"), std::invalid_argument);
    // Gray -> color replicates with alpha 1; color -> gray is the luminance.
    const std::vector<float> gray = {0.0f, 0.5f, 1.0f};
    const std::vector<float> color = convertMapChannels(gray, kGrayMap, kColorMap);
    ASSERT_EQ(color.size(), 12u);
    EXPECT_EQ((std::vector<float>(color.begin() + 4, color.begin() + 8)), (std::vector<float>{0.5f, 0.5f, 0.5f, 1.0f}));
    const std::vector<float> back = convertMapChannels(color, kColorMap, kGrayMap);
    for (std::size_t i = 0; i < gray.size(); ++i) EXPECT_NEAR(back[i], gray[i], 1e-6f);
    const std::vector<float> green = convertMapChannels({0.0f, 1.0f, 0.0f, 0.3f}, kColorMap, kGrayMap);
    EXPECT_NEAR(green[0], 0.7152f, 1e-6f);
    EXPECT_EQ(convertMapChannels(gray, kGrayMap, kGrayMap), gray);
}

TEST(ScalpPaint, ColorVertexMapsStampSmoothAndMaskChannels) {
    const auto plane = uvPlane(8);
    const std::size_t n = plane->pointCount();
    const Vec3f center{0.5f, 0.0f, 0.5f};
    // A gray stamp through the multi-channel function is the gray stamp, bit for bit.
    std::vector<float> a(n, 0.25f), b(n, 0.25f);
    EXPECT_EQ(stampVertexMap(a, *plane, center, 0.3f, 0.7f, 0.9f),
              stampVertexMap(b, kGrayMap, 0x2u, *plane, center, 0.3f, 0.7f, {0.9f, 0.1f, 0.1f, 0.1f}));
    EXPECT_EQ(a, b);  // a gray map ignores the mask and takes target[0]
    // A color map painted red in r and b only: g and a keep their values, far vertices nothing.
    std::vector<float> color(n * 4);
    for (std::size_t i = 0; i < n; ++i) {
        color[i * 4] = 0.0f;
        color[i * 4 + 1] = 0.5f;
        color[i * 4 + 2] = 0.0f;
        color[i * 4 + 3] = 0.75f;
    }
    const std::vector<float> before = color;
    EXPECT_GT(stampVertexMap(color, kColorMap, channelMask("rb"), *plane, center, 0.3f, 1.0f, {1.0f, 1.0f, 1.0f, 0.0f}), 0u);
    std::size_t centerVertex = 0;
    for (std::size_t i = 0; i < n; ++i) {
        const float d = (plane->points()[i] - center).length();
        if (d < 1e-4f) centerVertex = i;
        EXPECT_EQ(color[i * 4 + 1], 0.5f);
        EXPECT_EQ(color[i * 4 + 3], 0.75f);
        if (d > 0.4f) EXPECT_EQ(color[i * 4], 0.0f);
    }
    EXPECT_NEAR(color[centerVertex * 4], 1.0f, 1e-5f);
    EXPECT_NEAR(color[centerVertex * 4 + 2], 1.0f, 1e-5f);
    // Smoothing only the channels asked for.
    const VertexNeighbors neighbors = VertexNeighbors::of(*plane);
    std::vector<float> smoothed = color;
    EXPECT_GT(smoothVertexMap(smoothed, kColorMap, channelMask("r"), *plane, neighbors, center, 0.3f, 1.0f), 0u);
    EXPECT_LT(smoothed[centerVertex * 4], color[centerVertex * 4]);
    EXPECT_EQ(smoothed[centerVertex * 4 + 2], color[centerVertex * 4 + 2]);
    // Gray smoothing through the general function matches the gray one.
    std::vector<float> step(n), step2;
    for (std::size_t i = 0; i < n; ++i) step[i] = plane->points()[i].x > 0.5f ? 1.0f : 0.0f;
    step2 = step;
    EXPECT_EQ(smoothVertexMap(step, *plane, neighbors, center, 0.3f, 1.0f),
              smoothVertexMap(step2, kGrayMap, kAllChannels, *plane, neighbors, center, 0.3f, 1.0f));
    EXPECT_EQ(step, step2);
    // As a color set: the real r g b a at each corner, and at roots.
    const auto corners = cornerColorsFromVertexMap(color, *plane, kColorMap);
    ASSERT_EQ(corners.size(), plane->triangleCount() * 3);
    plane->setCornerColors("c", corners);
    StrandRoot root;
    root.face = 3;
    root.u = 0.3f;
    root.v = 0.2f;
    const PaintColor at = vertexColorAt(color, kColorMap, *plane, root);
    for (std::size_t k = 0; k < 4; ++k) EXPECT_NEAR(plane->colorAt("c", root)[k], at[k], 1e-5f);
    EXPECT_NEAR(at[3], 0.75f, 1e-6f);
    const PaintColor grayAt = vertexColorAt(before, kColorMap, *plane, root);
    EXPECT_NEAR(grayAt[1], 0.5f, 1e-6f);
    const std::vector<float> g(n, 0.4f);
    EXPECT_EQ(vertexColorAt(g, kGrayMap, *plane, root)[3], 1.0f);
    EXPECT_NEAR(vertexColorAt(g, kGrayMap, *plane, root)[2], 0.4f, 1e-6f);
    std::vector<float> short4 = g;  // one value per vertex is not a color map
    EXPECT_THROW(stampVertexMap(short4, kColorMap, kAllChannels, *plane, center, 0.3f, 1.0f, {}), std::invalid_argument);
}

TEST(ScalpPaint, CompositeModesPreserveAndClampValues) {
    EXPECT_FLOAT_EQ(compositePaintValue(0.3f, 0.2f, PaintMode::Replace), 0.2f);
    EXPECT_FLOAT_EQ(compositePaintValue(0.3f, 0.2f, PaintMode::Add), 0.5f);
    EXPECT_FLOAT_EQ(compositePaintValue(0.9f, 0.2f, PaintMode::Add), 1.0f);
    EXPECT_FLOAT_EQ(compositePaintValue(0.3f, 0.2f, PaintMode::Subtract), 0.1f);
    EXPECT_FLOAT_EQ(compositePaintValue(0.1f, 0.2f, PaintMode::Subtract), 0.0f);
    EXPECT_FLOAT_EQ(compositePaintValue(0.8f, 0.6f, PaintMode::ClampMaximum), 0.6f);
    EXPECT_FLOAT_EQ(compositePaintValue(0.4f, 0.6f, PaintMode::ClampMaximum), 0.4f);
    EXPECT_EQ(paintModeFromString("add"), PaintMode::Add);
    EXPECT_EQ(std::string(paintModeName(PaintMode::ClampMaximum)), "clampMaximum");
    EXPECT_THROW(paintModeFromString("multiply"), std::invalid_argument);
}

TEST(ScalpPaint, CompositeModesFillAndStampRealMapsOncePerStroke) {
    const auto plane = uvPlane(8);
    const Vec3f center{0.5f, 0.0f, 0.5f};
    const std::vector<Vec3f> one{center}, overlaps{center, center, center};
    const std::array<PaintMode, 4> modes{
        PaintMode::Replace, PaintMode::Add, PaintMode::Subtract, PaintMode::ClampMaximum};
    const std::array<float, 4> filled{0.2f, 0.6f, 0.2f, 0.2f};
    for (std::size_t m = 0; m < modes.size(); ++m) {
        std::vector<float> vertices(plane->pointCount(), 0.4f);
        fillMapValues(vertices, kGrayMap, kAllChannels, {0.2f, 0.2f, 0.2f, 0.2f}, modes[m]);
        for (float x : vertices) EXPECT_FLOAT_EQ(x, filled[m]);

        TexturePaint filledTexture(*plane, 32, 32, 0.4f);
        filledTexture.fill(0.2f, modes[m]);
        for (float x : filledTexture.values()) EXPECT_FLOAT_EQ(x, filled[m]);

        MapDab dab;
        dab.target = {0.2f, 0.2f, 0.2f, 0.2f};
        dab.mode = modes[m];
        dab.fromStrokeStart = modes[m] != PaintMode::Replace;

        std::vector<float> vertexOnce(plane->pointCount(), 0.4f), vertexOverlaps = vertexOnce;
        MapCanvas onceCanvas, overlapsCanvas;
        onceCanvas.vertices = &vertexOnce;
        overlapsCanvas.vertices = &vertexOverlaps;
        paintMapStroke(onceCanvas, *plane, dab, one, 0.3f);
        paintMapStroke(overlapsCanvas, *plane, dab, overlaps, 0.3f);
        if (modes[m] != PaintMode::Replace) {
            const float limit = compositePaintValue(0.4f, 0.2f, modes[m]);
            for (float x : vertexOverlaps) {
                EXPECT_LE(std::min(0.4f, limit), x) << paintModeName(modes[m]);
                EXPECT_GE(std::max(0.4f, limit), x) << paintModeName(modes[m]);
            }
        }

        TexturePaint textureOnce(*plane, 32, 32, 0.4f), textureOverlaps = textureOnce;
        MapCanvas textureOnceCanvas, textureOverlapsCanvas;
        textureOnceCanvas.texture = &textureOnce;
        textureOverlapsCanvas.texture = &textureOverlaps;
        paintMapStroke(textureOnceCanvas, *plane, dab, one, 0.3f);
        paintMapStroke(textureOverlapsCanvas, *plane, dab, overlaps, 0.3f);
        if (modes[m] != PaintMode::Replace) {
            const float limit = compositePaintValue(0.4f, 0.2f, modes[m]);
            for (float x : textureOverlaps.values()) {
                EXPECT_LE(std::min(0.4f, limit), x) << paintModeName(modes[m]);
                EXPECT_GE(std::max(0.4f, limit), x) << paintModeName(modes[m]);
            }
        }

        if (modes[m] != PaintMode::Replace) {
            const Vec3f offset{0.58f, 0.0f, 0.5f};
            const std::vector<float> vertexStart(plane->pointCount(), 0.4f);
            std::vector<float> crossedVertices = vertexStart;
            stampVertexReach(crossedVertices, kGrayMap, kAllChannels, *plane, vertexReach(*plane, center, 0.3f), 1.0f,
                             dab.target, modes[m], &vertexStart);
            const std::vector<float> afterFirstVertex = crossedVertices;
            stampVertexReach(crossedVertices, kGrayMap, kAllChannels, *plane, vertexReach(*plane, offset, 0.3f), 1.0f,
                             dab.target, modes[m], &vertexStart);
            for (std::size_t i = 0; i < crossedVertices.size(); ++i) {
                EXPECT_GE(std::abs(crossedVertices[i] - vertexStart[i]),
                          std::abs(afterFirstVertex[i] - vertexStart[i]))
                    << paintModeName(modes[m]);
            }

            TexturePaint crossedTexture(*plane, 32, 32, 0.4f);
            crossedTexture.beginEdit();
            crossedTexture.stampReach(crossedTexture.reachOf(center, 0.3f), 1.0f, dab.target, kAllChannels, modes[m], true);
            const std::vector<float> afterFirstTexture = crossedTexture.values();
            crossedTexture.stampReach(crossedTexture.reachOf(offset, 0.3f), 1.0f, dab.target, kAllChannels, modes[m], true);
            for (std::size_t i = 0; i < crossedTexture.values().size(); ++i) {
                EXPECT_GE(std::abs(crossedTexture.values()[i] - 0.4f), std::abs(afterFirstTexture[i] - 0.4f))
                    << paintModeName(modes[m]);
            }
            crossedTexture.endEdit();
        }
    }
}

TEST(ScalpPaint, ColorTexturesPaintFillRoundTripAndConvert) {
    const auto plane = uvPlane(8);
    TexturePaint paint(*plane, 32, 32, PaintColor{0.0f, 0.0f, 0.0f, 1.0f});
    EXPECT_EQ(paint.channels(), kColorMap);
    ASSERT_EQ(paint.values().size(), 32u * 32u * 4u);
    // Paint green into g only at the upper-left quarter; alpha stays 1.
    const Vec3f center{0.25f, 0.0f, 0.75f};
    EXPECT_GT(paint.stamp(center, 0.1f, 1.0f, {1.0f, 1.0f, 1.0f, 0.0f}, channelMask("g")), 0u);
    EXPECT_GT(paint.colorAt(8, 8)[1], 0.85f);  // the texel center is a bit off the stamp's
    EXPECT_EQ(paint.colorAt(8, 8)[0], 0.0f);
    EXPECT_EQ(paint.colorAt(8, 8)[3], 1.0f);
    EXPECT_EQ(paint.colorAt(24, 24)[1], 0.0f);
    // Smoothing g blurs the stamp's edge, r stays untouched.
    TexturePaint blurred = paint;
    EXPECT_GT(blurred.smooth(center, 0.15f, 1.0f, {}, channelMask("g")), 0u);
    for (std::size_t i = 0; i < 32u * 32u; ++i) EXPECT_EQ(blurred.values()[i * 4], 0.0f);
    // Saved as RGBA and read back as a color map: the same colors (8 bits).
    const std::string path = tempPath("groomlab_color_paint.png");
    paint.save(path);
    const TexturePtr image = loadTexture(path);
    TexturePaint again(*plane, *image, kColorMap);
    for (int y = 0; y < 32; y += 5) {
        for (int x = 0; x < 32; x += 5) {
            for (std::size_t c = 0; c < 4; ++c) EXPECT_NEAR(again.colorAt(x, y)[c], paint.colorAt(x, y)[c], 1.0f / 255.0f + 1e-5f);
        }
    }
    // A color image read as a gray map (a legacy map) is its luminance.
    TexturePaint asGray(*plane, *image);
    EXPECT_EQ(asGray.channels(), kGrayMap);
    EXPECT_NEAR(asGray.values()[static_cast<std::size_t>(8) * 32 + 8], 0.7152f * paint.colorAt(8, 8)[1], 0.01f);
    EXPECT_NEAR(static_cast<float>(asGray.toGray8(0)[static_cast<std::size_t>(8) * 32 + 8]) / 255.0f,
                asGray.values()[static_cast<std::size_t>(8) * 32 + 8], 1.0f / 255.0f);
    // Fill: a value sets r g b and keeps alpha; a color fills only the masked channels.
    paint.fill(PaintColor{0.0f, 0.0f, 0.0f, 0.4f}, channelMask("a"));
    paint.fill(0.6f);
    EXPECT_NEAR(paint.colorAt(3, 3)[0], 0.6f, 1e-6f);
    EXPECT_NEAR(paint.colorAt(3, 3)[3], 0.4f, 1e-6f);
    paint.fill({0.1f, 0.2f, 0.3f, 0.9f});
    EXPECT_NEAR(paint.colorAt(30, 1)[2], 0.3f, 1e-6f);
    // Gray <-> color in place.
    paint.setChannels(kGrayMap);
    EXPECT_EQ(paint.channels(), kGrayMap);
    EXPECT_NEAR(paint.values()[0], paintLuminance({0.1f, 0.2f, 0.3f, 1.0f}), 1e-6f);
    paint.setChannels(kColorMap);
    EXPECT_EQ(paint.colorAt(0, 0)[3], 1.0f);
    // Vertices <-> texture keep the color.
    std::vector<float> vertices;
    for (const Vec3f& p : plane->points()) vertices.insert(vertices.end(), {p.x > 0.5f ? 1.0f : 0.0f, 0.0f, 0.5f, 1.0f});
    const TexturePaint texture = textureFromVertexMap(vertices, kColorMap, *plane, 64, 64);
    EXPECT_EQ(texture.channels(), kColorMap);
    EXPECT_NEAR(texture.colorAt(60, 32)[0], 1.0f, 1e-4f);
    EXPECT_NEAR(texture.colorAt(4, 32)[0], 0.0f, 1e-4f);
    EXPECT_NEAR(texture.colorAt(4, 32)[2], 0.5f, 1e-4f);
    texture.save(path);
    const std::vector<float> back = vertexMapFromTexture(*loadTexture(path), *plane, kColorMap);
    ASSERT_EQ(back.size(), vertices.size());
    for (std::size_t i = 0; i < plane->pointCount(); ++i) {
        if (std::abs(plane->points()[i].x - 0.5f) < 0.15f) continue;
        for (std::size_t c = 0; c < 4; ++c) EXPECT_NEAR(back[i * 4 + c], vertices[i * 4 + c], 0.02f) << i;
    }
    // To guides: the luminance at the roots (a gray vertex map made first).
    std::vector<StrandRoot> roots(1);
    roots[0].face = static_cast<std::int32_t>(plane->triangleCount() - 1);
    roots[0].u = roots[0].v = 0.2f;
    const std::vector<float> atRoot = rootValuesFromVertexMap(convertMapChannels(vertices, kColorMap, kGrayMap), *plane, roots);
    EXPECT_NEAR(atRoot[0], paintLuminance({1.0f, 0.0f, 0.5f, 1.0f}), 1e-4f);
    std::remove(path.c_str());
}

TEST(ScalpPaint, UdimTileSetsAreRefused) {
    // A scalp map is one image over UVs 0-1: a tile set (whose size is not even known before a
    // tile is read) is refused with a clear error, not read as one blurred texel.
    const auto plane = uvPlane(4);
    const Texture tiles((std::filesystem::temp_directory_path() / "groomlab_none.<UDIM>.png").u8string());
    ASSERT_TRUE(tiles.isUdim());
    try {
        TexturePaint paint(*plane, tiles);
        FAIL() << "a UDIM set was accepted";
    } catch (const std::invalid_argument& e) {
        EXPECT_NE(std::string(e.what()).find("UDIM"), std::string::npos);
    }
    EXPECT_THROW(vertexMapFromTexture(tiles, *plane), std::invalid_argument);
    EXPECT_THROW(rootValuesFromTexture(tiles, *plane, {StrandRoot{}}), std::invalid_argument);
    EXPECT_THROW(projectTexture(tiles, kGrayMap, *plane, *plane), std::invalid_argument);
}

TEST(ScalpPaint, ImagesOfAnyFormatCopyAsPngTexelForTexel) {
    // A texture map linked to a TGA (or JPEG, PSD...) paints into a PNG copy: the copy holds the
    // same texels, RGBA for a color image and gray for a gray one.
    const int w = 5, h = 3;
    const std::string tga = tempPath("groomlab_relink_source.tga");
    {
        std::vector<std::uint8_t> file = {0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                                          static_cast<std::uint8_t>(w), 0, static_cast<std::uint8_t>(h), 0, 24, 0x20};
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {  // stored B G R, rows from the top (descriptor bit 5)
                file.push_back(static_cast<std::uint8_t>(10 * y));
                file.push_back(static_cast<std::uint8_t>(200 - 7 * x));
                file.push_back(static_cast<std::uint8_t>(40 * x + 3));
            }
        }
        std::ofstream out(std::filesystem::u8path(tga), std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(file.data()), static_cast<std::streamsize>(file.size()));
    }
    const std::string png = tempPath("groomlab_relink_copy.png");
    writeImagePng(*loadTexture(tga), png);
    std::ifstream in(std::filesystem::u8path(png), std::ios::binary);
    std::vector<char> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    in.close();
    ASSERT_GT(bytes.size(), 26u);
    EXPECT_EQ(static_cast<unsigned char>(bytes[1]), 'P');
    EXPECT_EQ(static_cast<int>(bytes[25]), 6);  // RGBA
    const TexturePtr copy = loadTexture(png);
    ASSERT_EQ(copy->width(), w);
    ASSERT_EQ(copy->height(), h);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const auto c = copy->sample((x + 0.5f) / w, 1.0f - (y + 0.5f) / h);
            EXPECT_NEAR(c[0] * 255.0f, 40.0f * x + 3.0f, 0.01f) << x << "," << y;
            EXPECT_NEAR(c[1] * 255.0f, 200.0f - 7.0f * x, 0.01f);
            EXPECT_NEAR(c[2] * 255.0f, 10.0f * y, 0.01f);
            EXPECT_NEAR(c[3], 1.0f, 1e-6f);
        }
    }
    // A gray image: a gray PNG.
    std::vector<float> rgba;
    for (int i = 0; i < 4 * 2; ++i) rgba.insert(rgba.end(), {i / 7.0f, i / 7.0f, i / 7.0f, 1.0f});
    writeImagePng(Texture(4, 2, rgba), png);
    std::ifstream again(std::filesystem::u8path(png), std::ios::binary);
    std::vector<char> grayBytes((std::istreambuf_iterator<char>(again)), std::istreambuf_iterator<char>());
    again.close();
    ASSERT_GT(grayBytes.size(), 26u);
    EXPECT_EQ(static_cast<int>(grayBytes[25]), 0);  // gray
    EXPECT_NEAR(loadTexture(png)->sample(0.125f, 0.25f)[0], std::round(4 / 7.0f * 255.0f) / 255.0f, 1e-5f);  // bottom row's first
    EXPECT_THROW(writeImagePng(Texture((std::filesystem::temp_directory_path() / "groomlab_none.<UDIM>.png").u8string()), png),
                 std::invalid_argument);
    std::remove(tga.c_str());
    std::remove(png.c_str());
}

TEST(ScalpPaint, VertexMapsProjectOntoAnotherScalp) {
    const auto from = uvSphere(24, 48);
    // Onto the same mesh: the same values.
    for (const int channels : {kGrayMap, kColorMap}) {
        const std::vector<float> values = sphereMap(*from, channels);
        const std::vector<float> same = projectVertexMap(values, channels, *from, *from);
        ASSERT_EQ(same.size(), values.size());
        for (std::size_t i = 0; i < values.size(); ++i) EXPECT_NEAR(same[i], values[i], 1e-4f) << i;
    }
    // Onto a remeshed head (another resolution): the gradient by position, within the old
    // mesh's facets.
    const auto remeshed = uvSphere(15, 28);
    const std::vector<float> gray = projectVertexMap(sphereMap(*from, kGrayMap), kGrayMap, *from, *remeshed);
    ASSERT_EQ(gray.size(), remeshed->pointCount());
    const std::vector<float> expected = sphereMap(*remeshed, kGrayMap);
    for (std::size_t i = 0; i < gray.size(); ++i) EXPECT_NEAR(gray[i], expected[i], 0.02f) << i;
    // A color map onto a head elsewhere and twice the size (the bounding boxes fitted): each
    // channel follows the position on the new head.
    const Vec3f center{10.0f, 0.0f, 0.0f};
    const auto big = uvSphere(16, 32, 2.0f, center);
    const ScalpFit fit = ScalpFit::boundingBoxes(*from, *big);
    EXPECT_NEAR(fit.scale, 2.0f, 1e-4f);
    const std::vector<float> color = projectVertexMap(sphereMap(*from, kColorMap), kColorMap, *from, *big, fit);
    ASSERT_EQ(color.size(), big->pointCount() * 4);
    const std::vector<float> want = sphereMap(*big, kColorMap, center);
    for (std::size_t i = 0; i < color.size(); ++i) EXPECT_NEAR(color[i], want[i], 0.02f) << i;
    // Without the fit, the far head's points find the old head's nearest side: not the map.
    const std::vector<float> unfitted = projectVertexMap(sphereMap(*from, kColorMap), kColorMap, *from, *big);
    float worst = 0.0f;
    for (std::size_t i = 0; i < color.size(); ++i) worst = std::max(worst, std::abs(unfitted[i] - want[i]));
    EXPECT_GT(worst, 0.2f);
    // A map that does not fit the old scalp (another vertex count) is refused.
    EXPECT_THROW(projectVertexMap(std::vector<float>(7, 0.5f), kGrayMap, *from, *remeshed), std::invalid_argument);
    EXPECT_THROW(projectVertexMap(sphereMap(*from, kGrayMap), 3, *from, *remeshed), std::invalid_argument);
}

TEST(ScalpPaint, ProjectionReadsTheRestPosesWhenBothHaveOne) {
    // The new scalp is posed elsewhere, both rest poses agree: the map lands by the rest poses.
    const auto from = uvSphere(12, 24);
    from->setRestPoints(from->points());
    std::vector<Vec3f> moved = from->points();
    for (Vec3f& p : moved) p = p + Vec3f{5.0f, 0.0f, 0.0f};
    ScalpMesh to = from->withPoints(moved);
    ASSERT_TRUE(to.hasRest());
    const std::vector<float> values = sphereMap(*from, kGrayMap);
    const std::vector<float> out = projectVertexMap(values, kGrayMap, *from, to);
    for (std::size_t i = 0; i < values.size(); ++i) EXPECT_NEAR(out[i], values[i], 1e-4f) << i;
}

TEST(ScalpPaint, TexturesProjectOntoAnotherScalpByPosition) {
    const auto from = uvSphere(24, 48);
    for (const bool gray : {true, false}) {
        const int channels = gray ? kGrayMap : kColorMap;
        const auto image = sphereImage(128, 64, gray);
        // The same mesh and UVs: the source's texels back (the image read as a map on it).
        const TexturePaint source(*from, *image, channels);
        const TexturePaint same = projectTexture(*image, channels, *from, *from);
        EXPECT_EQ(same.width(), 128);  // the source's size by default
        EXPECT_EQ(same.height(), 64);
        EXPECT_EQ(same.channels(), channels);
        double sum = 0.0;
        std::size_t count = 0, off = 0;
        for (int y = 0; y < 64; ++y) {
            for (int x = 0; x < 128; ++x) {
                if (!same.covered(x, y)) continue;
                for (std::size_t c = 0; c < static_cast<std::size_t>(channels); ++c) {
                    const float d = std::abs(same.colorAt(x, y)[c] - source.colorAt(x, y)[c]);
                    sum += d;
                    off += d > 0.02f ? 1 : 0;
                    ++count;
                }
            }
        }
        ASSERT_GT(count, 0u);
        EXPECT_LT(sum / static_cast<double>(count), 0.002);
        EXPECT_LT(static_cast<double>(off) / static_cast<double>(count), 0.02);

        // Another mesh with another UV layout, elsewhere and twice the size (fitted): each texel
        // takes the color painted where it lies on the head, not where its UV was on the old one.
        const Vec3f center{0.0f, 3.0f, -4.0f};
        const auto other = uvSphere(20, 40, 2.0f, center, 1);
        const TexturePaint moved = projectTexture(*image, channels, *from, *other, ScalpFit::boundingBoxes(*from, *other), 96, 96);
        EXPECT_EQ(moved.width(), 96);
        sum = 0.0;
        count = 0;
        float worst = 0.0f;
        for (int y = 0; y < 96; ++y) {
            for (int x = 0; x < 96; ++x) {
                if (!moved.covered(x, y)) continue;
                const PaintColor want = sphereColor(moved.position(x, y) - center);
                for (std::size_t c = 0; c < static_cast<std::size_t>(channels); ++c) {
                    const float d = std::abs(moved.colorAt(x, y)[c] - want[c]);
                    sum += d;
                    worst = std::max(worst, d);
                    ++count;
                }
            }
        }
        ASSERT_GT(count, 1000u);
        EXPECT_LT(sum / static_cast<double>(count), 0.01) << (gray ? "gray" : "color");
        EXPECT_LT(worst, 0.06f) << (gray ? "gray" : "color");
    }
    // Without UVs on either side there is nothing to read or paint at.
    const auto bare = std::make_shared<ScalpMesh>(from->points(), from->triangles());
    const auto image = sphereImage(8, 8, true);
    EXPECT_THROW(projectTexture(*image, kGrayMap, *bare, *from), std::invalid_argument);
    EXPECT_THROW(projectTexture(*image, kGrayMap, *from, *bare), std::invalid_argument);
}

// ---- Tables, previews, edits and cached decodes -------------------------------------

namespace {

// The texel table as one pass over the triangles builds it (the reference for the parallel build).
struct SerialTable {
    std::vector<StrandRoot> roots;
    std::vector<std::uint8_t> covered;
    std::vector<Vec3f> positions;
    std::size_t count = 0;
};

SerialTable serialTable(const ScalpMesh& scalp, int width, int height) {
    SerialTable t;
    const std::size_t texels = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    t.roots.assign(texels, StrandRoot{});
    t.covered.assign(texels, 0);
    t.positions.assign(texels, Vec3f{});
    const auto& uvs = scalp.cornerUVs();
    const float w = static_cast<float>(width), h = static_cast<float>(height);
    for (std::size_t tri = 0; tri < scalp.triangleCount(); ++tri) {
        const float ax = uvs[tri * 3][0] * w, ay = (1.0f - uvs[tri * 3][1]) * h;
        const float bx = uvs[tri * 3 + 1][0] * w, by = (1.0f - uvs[tri * 3 + 1][1]) * h;
        const float cx = uvs[tri * 3 + 2][0] * w, cy = (1.0f - uvs[tri * 3 + 2][1]) * h;
        const float det = (bx - ax) * (cy - ay) - (cx - ax) * (by - ay);
        if (std::abs(det) < 1e-12f) continue;
        const int x0 = std::max(0, static_cast<int>(std::floor(std::min({ax, bx, cx}))));
        const int x1 = std::min(width - 1, static_cast<int>(std::ceil(std::max({ax, bx, cx}))));
        const int y0 = std::max(0, static_cast<int>(std::floor(std::min({ay, by, cy}))));
        const int y1 = std::min(height - 1, static_cast<int>(std::ceil(std::max({ay, by, cy}))));
        const float inv = 1.0f / det;
        const float eps = -1e-3f;
        for (int y = y0; y <= y1; ++y) {
            const float py = static_cast<float>(y) + 0.5f;
            for (int x = x0; x <= x1; ++x) {
                const float px = static_cast<float>(x) + 0.5f;
                const float u = ((px - ax) * (cy - ay) - (cx - ax) * (py - ay)) * inv;
                const float v = ((bx - ax) * (py - ay) - (px - ax) * (by - ay)) * inv;
                if (u < eps || v < eps || u + v > 1.0f - eps) continue;
                StrandRoot root;
                root.face = static_cast<std::int32_t>(tri);
                root.u = std::clamp(u, 0.0f, 1.0f);
                root.v = std::clamp(v, 0.0f, 1.0f - root.u);
                const std::size_t i = static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x);
                if (t.roots[i].face < 0) ++t.count;
                t.roots[i] = root;
                t.covered[i] = 1;
                t.positions[i] = scalp.position(root);
            }
        }
    }
    return t;
}

// The image sampled at every texel center, as a map reads an image (the reference for the
// direct copy of texels at exact centers).
std::vector<float> sampledValues(const Texture& image, int width, int height, int channels) {
    std::vector<float> out;
    const float hu = 0.5f / static_cast<float>(std::max(image.width(), 1));
    const float hv = 0.5f / static_cast<float>(std::max(image.height(), 1));
    for (int y = 0; y < height; ++y) {
        const float v = 1.0f - (static_cast<float>(y) + 0.5f) / static_cast<float>(height);
        for (int x = 0; x < width; ++x) {
            const float u = (static_cast<float>(x) + 0.5f) / static_cast<float>(width);
            const auto c = image.sample(std::clamp(u, hu, 1.0f - hu), std::clamp(v, hv, 1.0f - hv));
            if (channels == kGrayMap) {
                out.push_back(std::clamp(0.2126f * c[0] + 0.7152f * c[1] + 0.0722f * c[2], 0.0f, 1.0f));
            } else {
                for (std::size_t k = 0; k < 4; ++k) out.push_back(std::clamp(c[k], 0.0f, 1.0f));
            }
        }
    }
    return out;
}

// The viewport's image of a map as the draw override made it from the whole map (toPreviewBytes):
// rows turned over, gray to r g b with alpha 255, a color map's alpha kept unless the map shows
// as a color map (`mapChannels` 4: alpha 255, `pick` a channel as gray).
std::vector<std::uint8_t> wholePreview(const TexturePaint& paint, int mapChannels, int pick) {
    const std::vector<std::uint8_t> bytes = paint.toPreviewBytes();
    const auto w = static_cast<std::size_t>(paint.width()), h = static_cast<std::size_t>(paint.height());
    const std::size_t per = paint.channels() == 4 ? 4 : 1;
    std::vector<std::uint8_t> rgba(w * h * 4);
    for (std::size_t y = 0; y < h; ++y) {
        const std::uint8_t* row = bytes.data() + (h - 1 - y) * w * per;
        for (std::size_t x = 0; x < w; ++x) {
            const std::size_t i = y * w + x;
            if (per == 1) {
                rgba[i * 4] = rgba[i * 4 + 1] = rgba[i * 4 + 2] = row[x];
            } else {
                for (std::size_t c = 0; c < 4; ++c) rgba[i * 4 + c] = row[x * 4 + c];
            }
            rgba[i * 4 + 3] = per == 1 ? 255 : rgba[i * 4 + 3];
        }
    }
    if (mapChannels == 4) {
        for (std::size_t i = 0; i < w * h; ++i) {
            if (pick >= 0) rgba[i * 4] = rgba[i * 4 + 1] = rgba[i * 4 + 2] = rgba[i * 4 + static_cast<std::size_t>(pick)];
            rgba[i * 4 + 3] = 255;
        }
    }
    return rgba;
}

}  // namespace

TEST(ScalpPaint, TexelTablesBuildInParallelAsInOnePassAndAreShared) {
    // A sphere's UVs (seams, poles, texels on shared edges) and a plane covering part of UV space.
    auto half = uvPlane(6);
    std::vector<ScalpMesh::UV> uvs = half->cornerUVs();
    for (auto& uv : uvs) uv = {0.1f + 0.6f * uv[0], 0.2f + 0.7f * uv[1]};
    half->setCornerUVs(uvs);
    for (const auto& [scalp, w, h] : {std::tuple{uvSphere(24, 48), 257, 131}, std::tuple{uvSphere(10, 20), 64, 64},
                                      std::tuple{std::shared_ptr<ScalpMesh>(half), 100, 37}}) {
        const SerialTable want = serialTable(*scalp, w, h);
        const std::shared_ptr<const TexelTable> table = texelTable(*scalp, w, h);
        ASSERT_EQ(table->roots.size(), want.roots.size());
        EXPECT_EQ(table->coveredCount, want.count);
        std::vector<std::uint8_t> covered(table->covered.size());
        for (std::size_t i = 0; i < covered.size(); ++i) covered[i] = table->covered[i] ? 1 : 0;  // 2: a seam texel
        EXPECT_EQ(covered, want.covered);
        for (std::size_t i = 0; i < want.roots.size(); ++i) {
            ASSERT_EQ(table->roots[i].face, want.roots[i].face) << i;
            ASSERT_EQ(table->roots[i].u, want.roots[i].u) << i;
            ASSERT_EQ(table->roots[i].v, want.roots[i].v) << i;
            ASSERT_EQ(table->positions[i].x, want.positions[i].x) << i;
            ASSERT_EQ(table->positions[i].y, want.positions[i].y) << i;
            ASSERT_EQ(table->positions[i].z, want.positions[i].z) << i;
        }
        // Maps of the same surface and size share the table; another size has its own.
        const TexturePaint a(*scalp, w, h, 0.0f), b(*scalp, w, h, PaintColor{0.0f, 0.0f, 0.0f, 1.0f});
        EXPECT_EQ(a.table().get(), table.get());
        EXPECT_EQ(b.table().get(), table.get());
        EXPECT_NE(TexturePaint(*scalp, w + 1, h, 0.0f).table().get(), table.get());
    }
}

TEST(ScalpPaint, MapsReadTheirImageAsTheSampleGivesIt) {
    // Same-size images read texel for texel (exact centers), other sizes sampled: both exactly
    // what sampling every texel center gives, gray and color, for sizes whose centers are exact
    // and for sizes whose are not.
    const auto scalp = uvSphere(12, 24);
    for (const auto& [iw, ih, w, h] : {std::tuple{64, 32, 64, 32}, std::tuple{100, 37, 100, 37}, std::tuple{96, 96, 96, 96},
                                       std::tuple{64, 32, 50, 70}, std::tuple{7, 5, 7, 5}}) {
        for (const bool gray : {true, false}) {
            const auto image = sphereImage(iw, ih, gray);
            for (const int channels : {kGrayMap, kColorMap}) {
                if (iw != w || ih != h) continue;  // a map is the image's size
                const TexturePaint paint(*scalp, *image, channels);
                const std::vector<float> want = sampledValues(*image, w, h, channels);
                ASSERT_EQ(paint.values().size(), want.size());
                for (std::size_t i = 0; i < want.size(); ++i) {
                    ASSERT_EQ(paint.values()[i], want[i]) << iw << "x" << ih << " texel " << i;
                }
            }
        }
    }
}

TEST(ScalpPaint, PaddedBytesAreTheSameInParallel) {
    // toBytes grows the covered area in parallel: the bytes one pass at a time give (computed
    // here the slow way, the reference loop of the earlier implementation).
    const auto scalp = uvSphere(16, 32);
    TexturePaint paint(*scalp, 200, 120, PaintColor{0.2f, 0.4f, 0.6f, 1.0f});
    for (int k = 0; k < 6; ++k) paint.stamp({std::cos(k * 1.0f), 0.3f, std::sin(k * 1.0f)}, 0.4f, 0.7f, {1.0f, 0.0f, 0.5f, 0.3f}, kAllChannels);
    auto half = uvPlane(6);
    std::vector<ScalpMesh::UV> uvs = half->cornerUVs();
    for (auto& uv : uvs) uv = {0.1f + 0.6f * uv[0], 0.2f + 0.7f * uv[1]};
    half->setCornerUVs(uvs);
    TexturePaint partial(*half, 90, 70, 0.25f);
    partial.stamp({0.3f, 0.0f, 0.5f}, 0.3f, 1.0f, 0.9f);
    for (const TexturePaint* p : {&paint, &partial}) {
        const int w = p->width(), h = p->height();
        const auto n = static_cast<std::size_t>(p->channels());
        for (const int padding : {0, 1, 3, 8}) {
            // The reference: the frontier grown one pass at a time.
            std::vector<std::uint8_t> want(static_cast<std::size_t>(w * h) * n, 0);
            std::vector<std::uint8_t> have(static_cast<std::size_t>(w * h), 0);
            std::vector<float> value(static_cast<std::size_t>(w * h) * n, 0.0f);
            for (int y = 0; y < h; ++y) {
                for (int x = 0; x < w; ++x) {
                    const std::size_t i = static_cast<std::size_t>(y * w + x);
                    if (!p->covered(x, y)) continue;
                    have[i] = 1;
                    for (std::size_t c = 0; c < n; ++c) value[i * n + c] = p->values()[i * n + c];
                }
            }
            for (int pass = 0; pass < padding; ++pass) {
                std::vector<std::pair<std::size_t, std::array<float, 4>>> grown;
                for (int y = 0; y < h; ++y) {
                    for (int x = 0; x < w; ++x) {
                        const std::size_t i = static_cast<std::size_t>(y * w + x);
                        if (have[i]) continue;
                        std::array<float, 4> sum{0, 0, 0, 0};
                        int count = 0;
                        const int dx[4] = {-1, 1, 0, 0}, dy[4] = {0, 0, -1, 1};
                        for (int k = 0; k < 4; ++k) {
                            const int nx = x + dx[k], ny = y + dy[k];
                            if (nx < 0 || ny < 0 || nx >= w || ny >= h) continue;
                            const std::size_t j = static_cast<std::size_t>(ny * w + nx);
                            if (!have[j]) continue;
                            for (std::size_t c = 0; c < n; ++c) sum[c] += value[j * n + c];
                            ++count;
                        }
                        if (count == 0) continue;
                        for (std::size_t c = 0; c < n; ++c) sum[c] /= static_cast<float>(count);
                        grown.emplace_back(i, sum);
                    }
                }
                for (const auto& [i, v] : grown) {
                    have[i] = 1;
                    for (std::size_t c = 0; c < n; ++c) value[i * n + c] = v[c];
                }
            }
            for (std::size_t i = 0; i < have.size(); ++i) {
                if (!have[i]) continue;
                for (std::size_t c = 0; c < n; ++c) {
                    want[i * n + c] = static_cast<std::uint8_t>(std::lround(std::clamp(value[i * n + c], 0.0f, 1.0f) * 255.0f));
                }
            }
            EXPECT_EQ(p->toBytes(padding), want) << "padding " << padding;
        }
    }
}

TEST(ScalpPaint, PreviewsUpdateWhatTheMapChangedAsTheWholeImageShowsIt) {
    const auto scalp = uvSphere(16, 32);
    for (const int channels : {kGrayMap, kColorMap}) {
        for (const auto& [mapChannels, pick, keepAlpha] : {std::tuple{channels, -1, true}, std::tuple{4, 1, true},
                                                          std::tuple{4, 3, true}, std::tuple{1, -1, true}}) {
            TexturePaint paint = channels == kGrayMap ? TexturePaint(*scalp, 128, 64, 0.1f)
                                                      : TexturePaint(*scalp, 128, 64, PaintColor{0.1f, 0.2f, 0.3f, 0.4f});
            const MapDisplay display{mapChannels, pick, keepAlpha};
            TexturePreview preview;
            const TexelRect first = preview.update(paint, display);
            EXPECT_EQ(first, TexelRect::all(128, 64));
            EXPECT_EQ(preview.rgba(), wholePreview(paint, mapChannels, pick));
            const std::uint64_t layout = preview.layout();
            std::uint64_t seen = preview.revision();
            for (int k = 0; k < 8; ++k) {
                const Vec3f c{std::cos(k * 0.7f), 0.5f - 0.1f * static_cast<float>(k), std::sin(k * 0.7f)};
                paint.stamp(c, 0.25f, 0.6f, {0.9f, 0.1f, 0.7f, 0.2f}, kAllChannels);
                if (k % 3 == 2) paint.smooth(c, 0.3f, 1.0f);
                const TexelRect changed = paint.dirty();
                EXPECT_FALSE(changed.empty());
                EXPECT_LT(static_cast<std::size_t>((changed.x1 - changed.x0) * (changed.y1 - changed.y0)), 128u * 64u);
                if (k % 2 == 0) continue;  // two stamps per update, as a throttled preview
                const TexelRect updated = preview.update(paint, display);
                EXPECT_TRUE(paint.dirty().empty());
                EXPECT_EQ(preview.rgba(), wholePreview(paint, mapChannels, pick)) << "stamp " << k;
                // A host that has the revision before last uploads both updates' texels.
                const TexelRect since = preview.changedSince(seen, layout);
                TexelRect both = updated;
                both.unite(since);
                EXPECT_EQ(both, since);
                seen = preview.revision();
                EXPECT_TRUE(preview.changedSince(seen, layout).empty());
            }
            EXPECT_EQ(preview.changedSince(seen, layout + 1), TexelRect::all(128, 64));
            // Another display makes the image anew.
            preview.update(paint, MapDisplay{4, 0, false});
            EXPECT_NE(preview.layout(), layout);
            EXPECT_EQ(preview.rgba(), [&] {
                std::vector<std::uint8_t> out(128u * 64u * 4u);
                const std::vector<std::uint8_t> bytes = paint.toPreviewBytes();
                displayPixels(bytes.data(), 128, 64, channels, MapDisplay{4, 0, false}, TexelRect::all(128, 64), out.data());
                return out;
            }());
        }
    }
}

TEST(ScalpPaint, EditsKeepChangedTilesAndUndoExactly) {
    const auto scalp = uvSphere(16, 32);
    for (const int channels : {kGrayMap, kColorMap}) {
        TexturePaint paint = channels == kGrayMap ? TexturePaint(*scalp, 200, 100, 0.3f)
                                                  : TexturePaint(*scalp, 200, 100, PaintColor{0.3f, 0.2f, 0.1f, 1.0f});
        paint.stamp({0.0f, 1.0f, 0.0f}, 0.5f, 0.3f, {0.8f, 0.8f, 0.8f, 0.8f}, kAllChannels);  // before any edit
        const std::vector<float> start = paint.values();
        const std::vector<std::uint8_t> startPng = paint.pngBytes();
        paint.beginEdit();
        EXPECT_THROW(paint.fill(0.5f), std::logic_error);
        for (int k = 0; k < 5; ++k) {
            const Vec3f c{0.8f * std::cos(k * 0.3f), 0.6f, 0.8f * std::sin(k * 0.3f)};
            paint.stamp(c, 0.2f, 0.8f, {1.0f, 0.0f, 0.5f, 0.6f}, kAllChannels);
            paint.smooth(c, 0.25f, 1.0f);
        }
        const TextureEdit edit = paint.endEdit();
        EXPECT_FALSE(paint.editing());
        const std::vector<float> end = paint.values();
        const std::vector<std::uint8_t> endPng = paint.pngBytes();
        ASSERT_FALSE(edit.empty());
        // Only the tiles the stroke changed, far less than the image.
        const std::size_t tiles = static_cast<std::size_t>((200 + 31) / 32) * static_cast<std::size_t>((100 + 31) / 32);
        EXPECT_LT(edit.tiles.size(), tiles);
        EXPECT_LT(edit.bytes(), 2 * start.size() * sizeof(float));
        EXPECT_TRUE(std::is_sorted(edit.tiles.begin(), edit.tiles.end()));
        // Undo and redo, again and again: the values and the PNG exactly.
        for (int round = 0; round < 3; ++round) {
            paint.clearDirty();
            paint.applyEdit(edit, false);
            EXPECT_FALSE(paint.dirty().empty());
            EXPECT_EQ(paint.values(), start);
            EXPECT_EQ(paint.pngBytes(), startPng);
            paint.applyEdit(edit, true);
            EXPECT_EQ(paint.values(), end);
            EXPECT_EQ(paint.pngBytes(), endPng);
        }
        // A stroke that changes nothing keeps no tile; an edit of another image is refused.
        paint.beginEdit();
        paint.stamp({0.0f, -5.0f, 0.0f}, 0.1f, 1.0f, 1.0f);
        EXPECT_TRUE(paint.endEdit().empty());
        TexturePaint other(*scalp, 100, 100, 0.0f);
        EXPECT_THROW(other.applyEdit(edit, false), std::invalid_argument);
    }
}

TEST(ScalpPaint, WrittenMapsPrimeTheTextureCacheWithWhatReadingThemGives) {
    const auto scalp = uvSphere(12, 24);
    for (const int channels : {kGrayMap, kColorMap}) {
        TexturePaint paint = channels == kGrayMap ? TexturePaint(*scalp, 64, 48, 0.2f)
                                                  : TexturePaint(*scalp, 64, 48, PaintColor{0.2f, 0.5f, 0.7f, 0.9f});
        paint.stamp({0.0f, 1.0f, 0.0f}, 0.6f, 0.7f, {1.0f, 0.3f, 0.0f, 0.5f}, kAllChannels);
        const std::string path = tempPath(channels == kGrayMap ? "groomlab_prime_gray.png" : "groomlab_prime_color.png");
        const std::vector<std::uint8_t> pixels = paint.toBytes();
        paint.save(path);
        // What decoding the file gives: the same texels as the pixels made into a texture.
        const TexturePtr decoded = loadTexture(path);
        const TexturePtr made = textureFromPixels(64, 48, channels, pixels);
        ASSERT_EQ(made->width(), decoded->width());
        ASSERT_EQ(made->height(), decoded->height());
        EXPECT_EQ(made->image()->rgba, decoded->image()->rgba);
        // Primed under the file's stamp, it is what the next load returns; a stale stamp is not.
        ASSERT_TRUE(primeTextureCache(path, fileStamp(path), made));
        EXPECT_EQ(loadTexture(path).get(), made.get());
        EXPECT_EQ(cachedTexture(path).get(), made.get());
        // The map read back from it is the map read back from the file.
        const TexturePaint fromCache(*scalp, *made, channels), fromFile(*scalp, *decoded, channels);
        EXPECT_EQ(fromCache.values(), fromFile.values());
        std::remove(path.c_str());
        EXPECT_EQ(cachedTexture(path), nullptr);
    }
    EXPECT_FALSE(primeTextureCache(tempPath("groomlab_prime.<UDIM>.png"), 1, textureFromPixels(1, 1, 1, {0})));
    EXPECT_THROW(textureFromPixels(2, 2, 1, {0, 0, 0}), TextureError);
}

TEST(ScalpPaint, EveryHostProjectsAGroomsMapsAlike) {
    // projectScalpMaps: what Maya's groomLabTransfer and the app project a groom's maps with. A
    // vertex map lands by position, a texture map through its image into a file the host
    // writes, a map that cannot be projected says why and keeps what it had.
    const auto from = uvSphere(24, 48);
    const auto to = uvSphere(15, 28);
    ScalpMaps maps;
    maps.maps["mask"] = {"vertices", sphereMap(*from, kGrayMap), {}, kGrayMap};
    maps.maps["tint"] = {"vertices", sphereMap(*from, kColorMap), {}, kColorMap};
    maps.maps["detail"] = {"texture", {}, "detail.png", kGrayMap};
    maps.maps["lost"] = {"texture", {}, "missing.png", kGrayMap};
    const std::string png = tempPath("groomlab_project_detail.png");
    writeImagePng(*sphereImage(64, 32, true), png);
    MapProjectionFiles files;
    files.resolve = [&](const std::string& stored) { return stored == "detail.png" ? png : std::string(); };
    std::string written;
    files.write = [&](const std::string& name, const TexturePaint& paint) {
        written = tempPath(("groomlab_project_" + name + "_out.png").c_str());
        paint.save(written);
        return "projected/" + name + ".png";
    };
    std::vector<std::string> warnings;
    const std::vector<std::string> done = projectScalpMaps(maps, {}, *from, *to, ScalpFit::identity(), files, &warnings);
    EXPECT_EQ(done, (std::vector<std::string>{"detail", "mask", "tint"}));
    ASSERT_EQ(warnings.size(), 1u);
    EXPECT_NE(warnings[0].find("lost"), std::string::npos);
    EXPECT_EQ(maps.maps["mask"].values.size(), to->pointCount());
    EXPECT_EQ(maps.maps["tint"].values.size(), to->pointCount() * 4);
    const std::vector<float> expected = sphereMap(*to, kGrayMap);
    for (std::size_t i = 0; i < expected.size(); ++i) EXPECT_NEAR(maps.maps["mask"].values[i], expected[i], 0.02f) << i;
    EXPECT_EQ(maps.maps["detail"].texture, "projected/detail.png");
    EXPECT_TRUE(std::filesystem::exists(std::filesystem::u8path(written)));
    EXPECT_EQ(maps.maps["lost"].texture, "missing.png") << "left as it was";
    // Only some: the rest untouched.
    ScalpMaps some = maps;
    EXPECT_EQ(projectScalpMaps(some, {"mask"}, *to, *from, ScalpFit::identity(), files), std::vector<std::string>{"mask"});
    EXPECT_EQ(some.maps["tint"].values.size(), to->pointCount() * 4);
    std::remove(png.c_str());
    std::remove(written.c_str());
}

// ---- Seams: painting the surface as if it had none -------------------------------------------

namespace {

// A 2 x 1 strip (x in [0, 2], z in [0, 1], normal +y) of 32 x 16 cells, with UVs: seamless (u = x / 2,
// v = z / 2), or cut at x = 1 into two islands far apart in UV space (the left u = x / 2, v = 0.5 +
// z / 2; the right turned half around, u = 1 - (x - 1) / 2, v = 0.5 - z / 2), the same texel size
// on both. `split`: the points at x = 1 doubled, the right half's triangles on their own copies (a
// mesh split at its seam).
std::shared_ptr<ScalpMesh> seamStrip(bool cut, bool split = false) {
    constexpr int cx = 32, cz = 16;
    std::vector<Vec3f> points;
    for (int z = 0; z <= cz; ++z) {
        for (int x = 0; x <= cx; ++x) points.emplace_back(2.0f * static_cast<float>(x) / cx, 0.0f, static_cast<float>(z) / cz);
    }
    auto at = [&](int x, int z) { return z * (cx + 1) + x; };
    std::vector<std::int32_t> copy(points.size(), -1);
    if (split) {
        for (int z = 0; z <= cz; ++z) {
            copy[static_cast<std::size_t>(at(cx / 2, z))] = static_cast<std::int32_t>(points.size());
            points.push_back(points[static_cast<std::size_t>(at(cx / 2, z))]);
        }
    }
    std::vector<ScalpMesh::Triangle> tris;
    std::vector<bool> right;
    for (int z = 0; z < cz; ++z) {
        for (int x = 0; x < cx; ++x) {
            const bool r = x >= cx / 2;
            auto v = [&](int px, int pz) {
                const std::int32_t i = at(px, pz);
                return r && copy[static_cast<std::size_t>(i)] >= 0 ? copy[static_cast<std::size_t>(i)] : i;
            };
            tris.push_back({v(x, z), v(x, z + 1), v(x + 1, z)});
            tris.push_back({v(x, z + 1), v(x + 1, z + 1), v(x + 1, z)});
            right.push_back(r);
            right.push_back(r);
        }
    }
    auto mesh = std::make_shared<ScalpMesh>(points, tris);
    std::vector<ScalpMesh::UV> uvs;
    for (std::size_t t = 0; t < tris.size(); ++t) {
        for (const std::int32_t i : tris[t]) {
            const Vec3f& p = points[static_cast<std::size_t>(i)];
            if (!cut) {
                uvs.push_back({p.x / 2.0f, p.z / 2.0f});
            } else if (!right[t]) {
                uvs.push_back({p.x / 2.0f, 0.5f + p.z / 2.0f});
            } else {
                uvs.push_back({1.0f - (p.x - 1.0f) / 2.0f, 0.5f - p.z / 2.0f});
            }
        }
    }
    mesh->setCornerUVs(std::move(uvs));
    return mesh;
}

// The texel of a seamless strip's 128 x 128 map at a point of the strip.
std::size_t seamlessTexel(const Vec3f& p) {
    const int col = std::clamp(static_cast<int>(std::floor(p.x * 64.0f)), 0, 127);
    const int row = std::clamp(static_cast<int>(std::floor((1.0f - p.z / 2.0f) * 128.0f)), 0, 127);
    return static_cast<std::size_t>(row) * 128 + static_cast<std::size_t>(col);
}

// The largest difference between a cut strip's map and a seamless one's, texel by texel at the
// same points of the surface (every covered texel of the cut one).
float largestDifference(const TexturePaint& cut, const TexturePaint& seamless) {
    const auto n = static_cast<std::size_t>(cut.channels());
    float worst = 0.0f;
    for (int y = 0; y < cut.height(); ++y) {
        for (int x = 0; x < cut.width(); ++x) {
            if (!cut.covered(x, y)) continue;
            const std::size_t i = static_cast<std::size_t>(y) * 128 + static_cast<std::size_t>(x);
            const std::size_t j = seamlessTexel(cut.position(x, y));
            for (std::size_t c = 0; c < n; ++c) worst = std::max(worst, std::abs(cut.values()[i * n + c] - seamless.values()[j * n + c]));
        }
    }
    return worst;
}

}  // namespace

TEST(ScalpPaint, SeamsPaintAndSmoothAsIfThereWereNone) {
    const auto cut = seamStrip(true), seamless = seamStrip(false);
    for (const int channels : {kGrayMap, kColorMap}) {
        TexturePaint a = channels == kGrayMap ? TexturePaint(*cut, 128, 128, 0.0f) : TexturePaint(*cut, 128, 128, PaintColor{0, 0, 0, 1});
        TexturePaint b = channels == kGrayMap ? TexturePaint(*seamless, 128, 128, 0.0f)
                                              : TexturePaint(*seamless, 128, 128, PaintColor{0, 0, 0, 1});
        // Both cover the strip texel for texel (the islands are as big as the seamless layout).
        ASSERT_EQ(a.coveredCount(), 128u * 64u);
        ASSERT_EQ(b.coveredCount(), 128u * 64u);
        // A hard step at the seam: 1 left of it, 0 right.
        auto step = [](const StrandRoot&, const Vec3f& p) {
            const float v = p.x < 1.0f ? 1.0f : 0.0f;
            return PaintColor{v, 1.0f - v, 0.5f * v, 1.0f};
        };
        a.assignColor(step);
        b.assignColor(step);
        a.beginEdit();
        // A stroke across the seam paints both sides alike.
        for (int k = 0; k <= 16; ++k) {
            const Vec3f c{0.6f + 0.05f * static_cast<float>(k), 0.0f, 0.3f};
            for (TexturePaint* p : {&a, &b}) p->stamp(c, 0.15f, 0.5f, PaintColor{0.5f, 0.2f, 0.9f, 0.6f}, kAllChannels);
        }
        EXPECT_LT(largestDifference(a, b), 1e-5f) << "the stroke";
        // Smoothing across it blurs the step on the surface: the cut layout as the seamless one.
        for (int k = 0; k < 12; ++k) {
            const Vec3f c{1.0f, 0.0f, 0.5f + 0.05f * static_cast<float>(k % 3 - 1)};
            for (TexturePaint* p : {&a, &b}) p->smooth(c, 0.3f, 1.0f);
        }
        EXPECT_LT(largestDifference(a, b), 1e-4f) << "the smoothing";
        // ...and it did blur across: the texels either side of the seam at z = 0.6 moved.
        const auto n = static_cast<std::size_t>(channels);
        const std::size_t left = seamlessTexel({1.0f - 0.5f / 64.0f, 0.0f, 0.6f}), right = seamlessTexel({1.0f + 0.5f / 64.0f, 0.0f, 0.6f});
        EXPECT_LT(b.values()[left * n], 0.85f);
        EXPECT_GT(b.values()[right * n], 0.15f);
        // Undo restores the step exactly, redo the stroke and the smoothing.
        const std::vector<float> done = a.values();
        const TextureEdit edit = a.endEdit();
        a.applyEdit(edit, false);
        for (int y = 0; y < 128; ++y) {
            for (int x = 0; x < 128; ++x) {
                if (!a.covered(x, y)) continue;
                const float want = a.position(x, y).x < 1.0f ? 1.0f : 0.0f;
                ASSERT_EQ(a.values()[(static_cast<std::size_t>(y) * 128 + static_cast<std::size_t>(x)) * n], want);
            }
        }
        a.applyEdit(edit, true);
        EXPECT_EQ(a.values(), done);
    }
}

TEST(ScalpPaint, SeamTexelsFindTheirNeighborsAcrossTheSeam) {
    const auto cut = seamStrip(true);
    const std::shared_ptr<const TexelTable> table = texelTable(*cut, 128, 128);
    // The islands' outlines (the seam and the strip's border) are the seam texels, nothing else.
    std::size_t seams = 0;
    for (std::size_t i = 0; i < table->covered.size(); ++i) seams += table->covered[i] == 2;
    EXPECT_EQ(seams, table->seamTexels.size());
    EXPECT_EQ(seams, 2u * (4u * 64u - 4u));  // each island 64 x 64 texels
    // A texel just left of the seam: its neighbor to the right (in the image: +x) is the texel just
    // right of the seam on the surface, in the other island; the others beside it in its own.
    auto texelAt = [&](const Vec3f& p) -> std::uint32_t {
        for (std::size_t i = 0; i < table->positions.size(); ++i) {
            if (table->covered[i] && (table->positions[i] - p).length() < 1e-4f) return static_cast<std::uint32_t>(i);
        }
        return TexelTable::kNoTexel;
    };
    const float h = 0.5f / 64.0f;
    const std::uint32_t left = texelAt({1.0f - h, 0.0f, 0.5f + h});
    ASSERT_NE(left, TexelTable::kNoTexel);
    const std::array<std::uint32_t, 8> around = table->neighbors(left);
    // The left island's image +x is the surface's +x, its image up (-y) the surface's +z.
    EXPECT_EQ(around[4], texelAt({1.0f + h, 0.0f, 0.5f + h}));
    EXPECT_EQ(around[2], texelAt({1.0f + h, 0.0f, 0.5f + 3.0f * h}));
    EXPECT_EQ(around[7], texelAt({1.0f + h, 0.0f, 0.5f - h}));
    EXPECT_EQ(around[3], texelAt({1.0f - 3.0f * h, 0.0f, 0.5f + h}));
    // At the strip's border there is nothing across.
    const std::uint32_t corner = texelAt({h, 0.0f, h});
    ASSERT_NE(corner, TexelTable::kNoTexel);
    const std::array<std::uint32_t, 8> edge = table->neighbors(corner);
    EXPECT_EQ(edge[3], TexelTable::kNoTexel);
    EXPECT_EQ(edge[4], texelAt({3.0f * h, 0.0f, h}));
}

TEST(ScalpPaint, StampsReachTheSurfaceConnectedToTheBrush) {
    // A thin flap (an ear): a brush on one face, wider than the flap is thick, leaves the other
    // face alone (reached only round the top, outside the brush)...
    const auto fin = test::makeFin(1.0f, 0.04f, 1.0f, 0.04f);
    std::vector<float> values(fin->pointCount(), 0.0f);
    EXPECT_GT(stampVertexMap(values, *fin, {0.5f, 0.3f, 0.02f}, 0.15f, 1.0f, 1.0f), 0u);
    float front = 0.0f, back = 0.0f;
    for (std::size_t i = 0; i < values.size(); ++i) {
        float& side = fin->points()[i].z > 0.0f ? front : back;
        side = std::max(side, values[i]);
    }
    EXPECT_GT(front, 0.5f);
    EXPECT_EQ(back, 0.0f);
    // ...near the top it wraps round to the other side.
    std::fill(values.begin(), values.end(), 0.0f);
    stampVertexMap(values, *fin, {0.5f, 1.0f, 0.02f}, 0.15f, 1.0f, 1.0f);
    back = 0.0f;
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (fin->points()[i].z < 0.0f) back = std::max(back, values[i]);
    }
    EXPECT_GT(back, 0.0f);
    // A sphere's UV seam (u = 0 | 1, its points a rounding apart): a texture stamp on it paints both sides.
    const auto sphere = uvSphere(16, 32);
    TexturePaint paint(*sphere, 128, 64, 0.0f);
    EXPECT_GT(paint.stamp({1.0f, 0.0f, 0.0f}, 0.3f, 1.0f, 1.0f), 0u);
    EXPECT_GT(paint.values()[32 * 128 + 1], 0.5f);
    EXPECT_GT(paint.values()[32 * 128 + 126], 0.5f);
}

TEST(ScalpPaint, VertexMapsSplitAtASeamPaintAsWelded) {
    const auto welded = seamStrip(true), split = seamStrip(true, true);
    ASSERT_EQ(split->pointCount(), welded->pointCount() + 17);
    for (const int channels : {kGrayMap, kColorMap}) {
        const auto n = static_cast<std::size_t>(channels);
        auto stepMap = [&](const ScalpMesh& mesh) {
            std::vector<float> values;
            for (const Vec3f& p : mesh.points()) {
                const float v = p.x < 1.0f ? 1.0f : 0.0f;
                if (channels == kGrayMap) {
                    values.push_back(v);
                } else {
                    values.insert(values.end(), {v, 1.0f - v, 0.5f, 1.0f});
                }
            }
            return values;
        };
        std::vector<float> a = stepMap(*welded), b = stepMap(*split);
        const VertexNeighbors na = VertexNeighbors::of(*welded), nb = VertexNeighbors::of(*split);
        for (int k = 0; k <= 8; ++k) {
            const Vec3f c{0.8f + 0.05f * static_cast<float>(k), 0.0f, 0.3f};
            stampVertexMap(a, channels, kAllChannels, *welded, c, 0.2f, 0.5f, {0.3f, 0.6f, 0.9f, 0.5f});
            stampVertexMap(b, channels, kAllChannels, *split, c, 0.2f, 0.5f, {0.3f, 0.6f, 0.9f, 0.5f});
        }
        for (int k = 0; k < 10; ++k) {
            const Vec3f c{1.0f, 0.0f, 0.6f};
            smoothVertexMap(a, channels, kAllChannels, *welded, na, c, 0.3f, 1.0f);
            smoothVertexMap(b, channels, kAllChannels, *split, nb, c, 0.3f, 1.0f);
        }
        // Every point of the split mesh (both copies at the seam) holds what the welded one does there.
        float worst = 0.0f;
        for (std::size_t i = 0; i < split->pointCount(); ++i) {
            const std::size_t j = i < welded->pointCount() ? i : (i - welded->pointCount()) * 33 + 16;
            ASSERT_LT((split->points()[i] - welded->points()[j]).length(), 1e-6f);
            for (std::size_t c = 0; c < n; ++c) worst = std::max(worst, std::abs(b[i * n + c] - a[j * n + c]));
        }
        EXPECT_LT(worst, 1e-5f);
        // And the smoothing crossed the seam.
        const std::size_t past = 10 * 33 + 17;  // x = 1.0625, z = 0.625
        EXPECT_GT(a[past * n], 0.05f);
        // A point not finite (first, where the bounds once began) joins nothing and keeps the seam
        // welded: the same paint (it once made the bounds NaN, so no seam welded).
        std::vector<Vec3f> points = split->points();
        points.insert(points.begin(), Vec3f{std::nanf(""), 0.0f, 0.0f});
        std::vector<ScalpMesh::Triangle> triangles = split->triangles();
        for (auto& t : triangles) {
            for (auto& v : t) ++v;
        }
        const ScalpMesh withNan(points, triangles);
        std::vector<float> c = stepMap(withNan);
        const VertexNeighbors nc = VertexNeighbors::of(withNan);
        for (int k = 0; k <= 8; ++k) {
            const Vec3f at{0.8f + 0.05f * static_cast<float>(k), 0.0f, 0.3f};
            stampVertexMap(c, channels, kAllChannels, withNan, at, 0.2f, 0.5f, {0.3f, 0.6f, 0.9f, 0.5f});
        }
        for (int k = 0; k < 10; ++k) smoothVertexMap(c, channels, kAllChannels, withNan, nc, {1.0f, 0.0f, 0.6f}, 0.3f, 1.0f);
        float worstNan = 0.0f;
        for (std::size_t i = 0; i < split->pointCount(); ++i) {
            for (std::size_t ch = 0; ch < n; ++ch) worstNan = std::max(worstNan, std::abs(c[(i + 1) * n + ch] - b[i * n + ch]));
        }
        EXPECT_LT(worstNan, 1e-5f);
    }
}

TEST(ScalpPaint, PreviewsPadTheIslandsEdges) {
    // The cut strip's two islands in a 128 x 128 map: the preview's texels next to an island copy
    // the nearest covered texel, out to TexelTable::kPadding steps; the rest stay black.
    const auto cut = seamStrip(true);
    TexturePaint paint(*cut, 128, 128, 0.0f);
    paint.assign([](const StrandRoot&, const Vec3f& p) { return 0.2f + 0.3f * p.x + 0.1f * p.z; });
    const std::vector<std::uint8_t> bytes = paint.toPreviewBytes();
    auto byteOf = [](float v) { return static_cast<std::uint8_t>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f)); };
    std::size_t padded = 0;
    for (int y = 0; y < 128; ++y) {
        for (int x = 0; x < 128; ++x) {
            const std::size_t i = static_cast<std::size_t>(y) * 128 + static_cast<std::size_t>(x);
            if (paint.covered(x, y)) {
                ASSERT_EQ(bytes[i], byteOf(paint.values()[i]));
                continue;
            }
            // Steps to the nearest covered texel (4-neighbor: |dx| + |dy|).
            int steps = 1000;
            for (int yy = std::max(0, y - 9); yy < std::min(128, y + 10); ++yy) {
                for (int xx = std::max(0, x - 9); xx < std::min(128, x + 10); ++xx) {
                    if (paint.covered(xx, yy)) steps = std::min(steps, std::abs(xx - x) + std::abs(yy - y));
                }
            }
            if (steps > TexelTable::kPadding) {
                ASSERT_EQ(bytes[i], 0) << x << " " << y;
                continue;
            }
            ++padded;
            ASSERT_GT(bytes[i], 0) << x << " " << y;
            // Straight out from an island's side: that side's texel, so a bilinear sample at the
            // island's edge reads the edge.
            for (const auto& [dx, dy] : {std::pair{1, 0}, std::pair{-1, 0}, std::pair{0, 1}, std::pair{0, -1}}) {
                const int ex = x + dx, ey = y + dy;
                if (steps != 1 || ex < 0 || ey < 0 || ex >= 128 || ey >= 128 || !paint.covered(ex, ey)) continue;
                const int ox = x - dy, oy = y + dx;  // along the edge, both ways: still a straight side
                const int px = x + dy, py = y - dx;
                const bool straight = ox >= 0 && oy >= 0 && ox < 128 && oy < 128 && px >= 0 && py >= 0 && px < 128 && py < 128 &&
                                      paint.covered(ox + dx, oy + dy) && paint.covered(px + dx, py + dy);
                if (straight) ASSERT_EQ(bytes[i], byteOf(paint.values()[static_cast<std::size_t>(ey) * 128 + static_cast<std::size_t>(ex)])) << x << " " << y;
            }
        }
    }
    EXPECT_EQ(padded, paint.table()->padTexels.size());
    EXPECT_GT(padded, 1800u);  // a band 8 wide round both islands, inside the image
}
