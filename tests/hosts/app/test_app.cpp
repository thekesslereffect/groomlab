// SPDX-License-Identifier: Apache-2.0
// The standalone app's logic without a window: the document over a session (open, evaluate,
// edit params with undo and redo, save, export), the OBJ reader, the camera, and one frame of
// the sample groom rendered through QRhi into a texture and read back (the viewport's renderer
// on the platform's API, the Null backend when there is no device).

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <tuple>
#include <vector>

#include <QApplication>
#include <QColor>
#include <QDir>
#include <QImage>
#include <QSettings>
#include <QVector4D>
#include <rhi/qrhi.h>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "groomlab/brush_catalog.h"
#include "groomlab/display.h"
#include "brush_session.h"
#include "camera.h"
#include "card_preview.h"
#include "gpu_preview.h"
#include "graph_presets.h"
#include "groom_document.h"
#include "prman_render.h"
#include "vray_render.h"
#include "groomlab/alembic_io.h"
#include "groomlab/card_presets.h"
#include "groomlab/correctives.h"
#include "groomlab/export_view.h"
#include "groomlab/hair_color.h"
#include "groomlab/graph_io.h"
#include "groomlab/groom_file.h"
#include "groomlab/parts.h"
#include "groomlab/sculpt.h"
#include "groomlab/subdivision.h"
#include "groomlab/usd_writer.h"
#include "look.h"
#include "lookdev_renderer.h"
#include "mesh_io.h"
#include "offscreen.h"
#include "playback.h"
#include "renderer.h"
#include "surface.h"
#include "theme.h"
#include "tool_view.h"
#include "update_check.h"

#include "groomlab/alembic_operators.h"
#include "groomlab/graph.h"

namespace groomlab::app {
namespace {

using nlohmann::json;

const std::string kSampleGroom = std::string(GROOMLAB_TEST_DATA) + "/strokes/strokes.groom";

std::string tempPath(const std::string& name) {
    return (std::filesystem::temp_directory_path() / name).string();
}

TEST(AppUpdates, ComparesVersionsAndReadsTheFeed) {
    EXPECT_LT(compareVersions(QStringLiteral("0.1.1"), QStringLiteral("0.1.2")), 0);
    EXPECT_GT(compareVersions(QStringLiteral("0.10.0"), QStringLiteral("0.9.9")), 0);
    EXPECT_EQ(compareVersions(QStringLiteral("1.0"), QStringLiteral("1.0.0")), 0);
    EXPECT_LT(compareVersions(QStringLiteral("1.0.0-beta"), QStringLiteral("1.0.0")), 0);  // a pre-release first
    EXPECT_GT(compareVersions(QStringLiteral("1.0.0-beta.10"), QStringLiteral("1.0.0-beta.2")), 0);  // numerically
    EXPECT_EQ(compareVersions(QStringLiteral("v1.2.0"), QStringLiteral("1.2.0+build.5")), 0);
    EXPECT_EQ(compareVersions(QStringLiteral("latest"), QStringLiteral("1.0.0")), 0);  // not a version

    // tools/publish_app.py's latest.json.
    const QByteArray feed = R"({"version": "0.2.0", "url": "https://downloads.getgroomlab.com/app/GroomLab-App-0.2.0-beta-windows-x64.zip",
                                "notes": "- Faster brushes", "channel": "beta", "platform": "windows-x64"})";
    const UpdateInfo newer = parseLatest(feed, QStringLiteral("0.1.1"));
    EXPECT_TRUE(newer.ok);
    EXPECT_TRUE(newer.newer);
    EXPECT_EQ(newer.latest, QStringLiteral("0.2.0"));
    EXPECT_TRUE(newer.url.endsWith(QStringLiteral("-beta-windows-x64.zip")));
    EXPECT_EQ(newer.notes, QStringLiteral("- Faster brushes"));
    const UpdateInfo same = parseLatest(feed, QStringLiteral("0.2.0"));
    EXPECT_TRUE(same.ok);
    EXPECT_FALSE(same.newer);
    EXPECT_FALSE(parseLatest("not json", QStringLiteral("0.1.1")).ok);
}

const GroomDocument::NodeInfo& nodeOfType(const std::vector<GroomDocument::NodeInfo>& nodes, const std::string& type) {
    for (const auto& n : nodes) {
        if (n.type == type) return n;
    }
    throw std::runtime_error("no node of type " + type);
}

TEST(AppMeshIO, ReadsObjWithUVsAndNegativeIndices) {
    const std::string obj =
        "# a quad and a triangle\n"
        "v 0 0 0\nv 1 0 0\nv 1 0 1\nv 0 0 1\nv 2 0 0\n"
        "vt 0 0\nvt 1 0\nvt 1 1\nvt 0 1\nvt 2 0\n"
        "f 1/1 2/2 3/3 4/4\n"
        "f -4/2 -1/5 -3/3\n";
    const auto mesh = parseObjMesh(obj);
    ASSERT_TRUE(mesh);
    EXPECT_EQ(mesh->pointCount(), 5u);
    EXPECT_EQ(mesh->triangleCount(), 3u);
    EXPECT_TRUE(mesh->hasUVs());
    EXPECT_EQ(mesh->cornerUVs().size(), 9u);
    EXPECT_THROW(parseObjMesh("v 0 0 0\nf 1 2 3\n"), MeshFileError);
    EXPECT_THROW(parseObjMesh("v 0 0 0\n"), MeshFileError);
    // Corners without texture coordinates leave the mesh without UVs.
    const auto plain = parseObjMesh("v 0 0 0\nv 1 0 0\nv 0 0 1\nf 1 2 3\n");
    EXPECT_FALSE(plain->hasUVs());
    EXPECT_THROW(readMeshFile("nothing.ply"), MeshFileError);
}

TEST(AppMeshIO, ReadsObjSurfaceWithNormalsAndMaterial) {
    const std::string obj =
        "mtllib head.mtl\no head\nusemtl skin\n"
        "v 0 0 0\nv 1 0 0\nv 1 1 0\nv 0 1 0\n"
        "vt 0 0\nvt 1 0\nvt 1 1\nvt 0 1\n"
        "vn 0 0 1\nvn 0 0 1\n"
        "f 1/1/1 2/2/1 3/3/2 4/4/2\n";
    const std::string mtl =
        "newmtl other\nmap_Kd other.png\n"
        "newmtl skin\nKd 0.5 0.25 0.125\nmap_Kd textures/albedo.jpg\nmap_Bump -bm 0.8 normal.png\nmap_Ks spec.png\n"
        "map_Pr rough.png\nmap_Pm C:/abs/metal.png\nmap_Ka ao.png\n";
    const Surface s = parseObjSurface(obj, mtl, "test.obj", "scans/", surfacePreset("skin"));
    EXPECT_EQ(s.name, "head");
    EXPECT_EQ(s.material.kind, SurfaceKind::Skin) << "the preset is the base";
    ASSERT_TRUE(s.mesh);
    EXPECT_EQ(s.mesh->triangleCount(), 2u);
    EXPECT_TRUE(s.mesh->hasUVs());
    ASSERT_EQ(s.cornerNormals.size(), 6u);
    EXPECT_FLOAT_EQ(s.cornerNormals[0].z, 1.0f);
    // The named material, its maps against the folder, the option skipped, absolute kept.
    EXPECT_FLOAT_EQ(s.material.baseColor[1], 0.25f);
    EXPECT_EQ(s.material.albedoMap, "scans/textures/albedo.jpg");
    EXPECT_EQ(s.material.normalMap, "scans/normal.png");
    EXPECT_EQ(s.material.specularMap, "scans/spec.png");
    EXPECT_EQ(s.material.roughnessMap, "scans/rough.png");
    EXPECT_EQ(s.material.metallicMap, "C:/abs/metal.png");
    EXPECT_EQ(s.material.occlusionMap, "scans/ao.png");
    // A library without Kd keeps the preset's color (and its roughness stays the preset's).
    const Surface mapsOnly = parseObjSurface(obj, "newmtl skin\nmap_Kd a.png\n", "test.obj", "", surfacePreset("skin"));
    EXPECT_FLOAT_EQ(mapsOnly.material.baseColor[0], surfacePreset("skin").baseColor[0]);
    EXPECT_EQ(mapsOnly.material.albedoMap, "a.png");
    // Corners without normals leave the surface to smooth normals; no library, no maps.
    const Surface plain = parseObjSurface("v 0 0 0\nv 1 0 0\nv 0 0 1\nf 1 2 3\n", "", "plain.obj");
    EXPECT_TRUE(plain.cornerNormals.empty());
    EXPECT_TRUE(plain.material.albedoMap.empty());
    EXPECT_EQ(plain.name, "plain.obj");
}

TEST(AppSurface, VerticesCarryTangentsOcclusionAndCurvature) {
    // A quad in the xy plane with u along x: the tangent is +x, the bitangent +y (sign +1).
    Surface quad;
    quad.name = "quad";
    quad.mesh = parseObjMesh("v 0 0 0\nv 1 0 0\nv 1 1 0\nv 0 1 0\nvt 0 0\nvt 1 0\nvt 1 1\nvt 0 1\nf 1/1 2/2 3/3 4/4\n");
    quad.vertexOcclusion = {0.25f, 0.5f, 0.75f, 1.0f};
    const SurfaceVertices v = buildSurfaceVertices(quad);
    EXPECT_EQ(v.vertexCount(), 4u);  // shared where the corners agree
    EXPECT_EQ(v.indices.size(), 6u);
    const float* first = v.data.data();
    EXPECT_NEAR(first[3 + 2], 1.0f, 1e-5f);  // the normal +z
    EXPECT_NEAR(first[6], 1.0f, 1e-4f);      // the tangent +x
    EXPECT_NEAR(first[7], 0.0f, 1e-4f);
    EXPECT_NEAR(first[9], 1.0f, 1e-5f);      // the handedness
    EXPECT_NEAR(first[12], 0.25f, 1e-6f);    // the occlusion
    EXPECT_NEAR(first[13], 0.0f, 1e-6f);     // flat: no curvature
    // A sphere's curvature is 1 / its radius, whichever the radius.
    for (float radius : {1.0f, 2.5f}) {
        const Surface eye = makeEyeSurface({0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 1.0f}, {0.0f, 1.0f, 0.0f}, radius, surfacePreset("eye brown"));
        const SurfaceVertices e = buildSurfaceVertices(eye);
        double sum = 0.0;
        int count = 0;
        for (std::size_t i = 0; i < e.vertexCount(); ++i) {
            const float* p = e.data.data() + i * SurfaceVertices::kStride;
            if (p[2] < -0.3f * radius) {  // the back: the sclera's sphere, away from the cornea's crease
                sum += p[13];
                ++count;
            }
        }
        ASSERT_GT(count, 100);
        EXPECT_NEAR(sum / count, 1.0 / radius, 0.08 / radius);
    }
    // Normals per corner split the vertices where they disagree.
    Surface faceted = quad;
    faceted.cornerNormals = {{0, 0, 1}, {0, 0, 1}, {0, 0, 1}, {1, 0, 0}, {1, 0, 0}, {1, 0, 0}};
    EXPECT_EQ(buildSurfaceVertices(faceted).vertexCount(), 6u);
}

TEST(AppSurface, EyeballShapeAndBakedOcclusion) {
    const Vec3f center{1.0f, 2.0f, 3.0f}, forward{0.0f, 0.0f, 1.0f};
    const float radius = 1.2f;
    const Surface eye = makeEyeSurface(center, forward, {0.0f, 1.0f, 0.0f}, radius, surfacePreset("eye blue"));
    ASSERT_TRUE(eye.mesh);
    EXPECT_TRUE(eye.isEye);
    EXPECT_EQ(eye.material.kind, SurfaceKind::Eye);
    EXPECT_TRUE(eye.mesh->hasUVs());
    EXPECT_EQ(eye.cornerNormals.size(), eye.mesh->triangleCount() * 3);
    float apex = 0.0f;
    int sclera = 0;
    for (const Vec3f& p : eye.mesh->points()) {
        const Vec3f rel = (p - center) / radius;
        apex = std::max(apex, rel.z);
        const float r = rel.length();
        if (rel.z < kEyeLimbusCos - 0.01f) {
            // Behind the limbus: on the sclera's sphere.
            EXPECT_NEAR(r, 1.0f, 1e-4f);
            ++sclera;
        } else {
            // The cornea bulges out, never past its apex.
            EXPECT_GE(r, 1.0f - 1e-4f);
            EXPECT_LE(rel.z, kEyeCorneaApex + 1e-3f);
        }
    }
    EXPECT_GT(sclera, 100);
    EXPECT_NEAR(apex, kEyeCorneaApex, 2e-3f);
    // Occlusion baked against a lid: a plane just above the eye's top shadows the top, not the bottom.
    const auto lid = parseObjMesh("v -5 1.3 -5\nv 5 1.3 -5\nv 5 1.3 5\nv -5 1.3 5\nf 1 2 3 4\n");
    Surface eyeAtOrigin = makeEyeSurface({0.0f, 0.0f, 0.0f}, forward, {0.0f, 1.0f, 0.0f}, 1.0f, surfacePreset("eye brown"));
    const std::vector<float> occlusion = bakeVertexOcclusion(*eyeAtOrigin.mesh, {lid}, 32, 10.0f);
    ASSERT_EQ(occlusion.size(), eyeAtOrigin.mesh->pointCount());
    double top = 0.0, bottom = 0.0;
    int tops = 0, bottoms = 0;
    for (std::size_t i = 0; i < occlusion.size(); ++i) {
        const Vec3f& p = eyeAtOrigin.mesh->points()[i];
        if (p.y > 0.9f) {
            top += occlusion[i];
            ++tops;
        } else if (p.y < -0.9f) {
            bottom += occlusion[i];
            ++bottoms;
        }
    }
    ASSERT_GT(tops, 0);
    ASSERT_GT(bottoms, 0);
    EXPECT_LT(top / tops, 0.3);
    EXPECT_GT(bottom / bottoms, 0.95);
    // No occluders, no rays: everything open.
    for (float o : bakeVertexOcclusion(*eyeAtOrigin.mesh, {}, 8, 10.0f)) EXPECT_FLOAT_EQ(o, 1.0f);
}

TEST(AppLook, SurfacePresetsAndParams) {
    for (const std::string& name : surfacePresetNames()) {
        const SurfaceMaterial m = surfacePreset(name);
        EXPECT_EQ(m.presetName, name);
        EXPECT_EQ(m.kind == SurfaceKind::Skin, name.rfind("skin", 0) == 0);
        EXPECT_EQ(m.kind == SurfaceKind::Eye, name.rfind("eye", 0) == 0);
    }
    EXPECT_TRUE(surfacePreset("no such").presetName.empty());
    EXPECT_FLOAT_EQ(surfacePreset("metal").metallic, 1.0f);
    SurfaceMaterial m = surfacePreset("clay");
    EXPECT_TRUE(setSurfaceParam(m, "roughness", "0.25"));
    EXPECT_FLOAT_EQ(m.roughness, 0.25f);
    EXPECT_TRUE(m.presetName.empty());
    EXPECT_TRUE(setSurfaceParam(m, "baseColor.g", "0.75"));
    EXPECT_FLOAT_EQ(m.baseColor[1], 0.75f);
    EXPECT_TRUE(setSurfaceParam(m, "kind", "skin"));
    EXPECT_EQ(m.kind, SurfaceKind::Skin);
    EXPECT_TRUE(setSurfaceParam(m, "albedoMap", "a.png"));
    EXPECT_EQ(m.albedoMap, "a.png");
    EXPECT_FALSE(setSurfaceParam(m, "roughness", "soft"));
    EXPECT_FALSE(setSurfaceParam(m, "no such", "1"));
    EXPECT_FALSE(setSurfaceParam(m, "baseColor.x", "1"));
    EXPECT_FALSE(setSurfaceParam(m, "kind", "glass"));
}

TEST(AppLook, SurfaceTablesHaveTheirShapes) {
    // The environment BRDF: a smooth surface seen head-on reflects all of its F0 (scale near
    // 1, bias near 0); a rough one loses energy to multiple scattering (scale + bias < 1);
    // the bias (what a black F0 still reflects) grows toward grazing.
    const int size = 32;
    const std::vector<float> brdf = environmentBrdfLUT(size, 64);
    ASSERT_EQ(brdf.size(), static_cast<std::size_t>(size * size * 2));
    const auto smoothHeadOn = environmentBrdfAt(brdf, size, 0.05f, 1.0f);
    EXPECT_NEAR(smoothHeadOn[0] + smoothHeadOn[1], 1.0f, 0.05f);
    EXPECT_LT(smoothHeadOn[1], 0.05f);
    const auto rough = environmentBrdfAt(brdf, size, 0.9f, 0.7f);
    EXPECT_LT(rough[0] + rough[1], 0.9f);
    EXPECT_GT(rough[0] + rough[1], 0.3f);
    EXPECT_GT(environmentBrdfAt(brdf, size, 0.3f, 0.1f)[1], environmentBrdfAt(brdf, size, 0.3f, 0.9f)[1]);
    for (float v : brdf) {
        EXPECT_GE(v, 0.0f);
        EXPECT_LE(v, 1.05f);
    }

    // The skin profile (Burley's, normalized) integrates to 1 per channel over the plane,
    // and the red reaches furthest (a centimeter out it is far above the others, whose mass
    // sits closer in).
    double integral[3] = {0.0, 0.0, 0.0};
    for (int i = 0; i < 200000; ++i) {
        const float r = (i + 0.5f) * 0.001f;
        const RGB p = skinDiffusionProfile(r);
        for (int k = 0; k < 3; ++k) integral[k] += 2.0 * 3.14159265 * r * p[k] * 0.001;
    }
    for (int k = 0; k < 3; ++k) EXPECT_NEAR(integral[k], 1.0, 0.03);
    const RGB far = skinDiffusionProfile(10.0f);
    EXPECT_GT(far[0], far[1] * 1.5f);
    EXPECT_GT(far[1], far[2] * 1.5f);
    EXPECT_LT(skinDiffusionProfile(0.5f)[0], skinDiffusionProfile(0.5f)[2]);  // near the entry the blue is densest
    // Through a slab: everything at no thickness, red the most at 1 mm, falling with the
    // thickness (red still passes some at 5 mm, as an ear lit from behind shows; blue little).
    const RGB none = skinTransmittance(0.0f), mm = skinTransmittance(1.0f), thick = skinTransmittance(5.0f);
    for (int k = 0; k < 3; ++k) EXPECT_NEAR(none[k], 1.0f, 0.01f);
    EXPECT_GT(mm[0], mm[1]);
    EXPECT_GT(mm[1], mm[2]);
    EXPECT_LT(thick[0], mm[0] * 0.8f);
    EXPECT_LT(thick[2], 0.1f);
    EXPECT_GT(thick[0], 0.2f);

    // The strip table: the whole plane's mass is 1 per channel; a narrow strip holds less
    // (and the red the least: it spreads furthest).
    const ProfileStripTable strip = profileStripTable(kSkinMeanFreePathMm, 100.0f, 512);
    const RGB whole = strip.at(100.0f), narrow = strip.at(1.0f);
    for (int k = 0; k < 3; ++k) {
        EXPECT_NEAR(whole[k], 1.0f, 0.02f);
        EXPECT_GT(narrow[k], 0.05f);
        EXPECT_LT(narrow[k], 0.9f);
    }
    EXPECT_LT(narrow[0], narrow[1]);
    EXPECT_LT(narrow[1], narrow[2]);

    // The separable kernel: an odd number of taps from -radius to radius crowded toward the
    // center, every weight positive, the weights summing to 1 per channel, symmetric, the
    // middle three taps (within a quarter millimeter) holding more than any other, red the
    // widest.
    const std::vector<SssKernelSample> kernel = separableSssKernel(25, kSkinMeanFreePathMm, 22.0f);
    ASSERT_EQ(kernel.size(), 25u);
    EXPECT_FLOAT_EQ(kernel.front().offset, -22.0f);
    EXPECT_FLOAT_EQ(kernel.back().offset, 22.0f);
    EXPECT_FLOAT_EQ(kernel[12].offset, 0.0f);
    EXPECT_LT(std::abs(kernel[13].offset), 0.25f);
    double sum[3] = {0.0, 0.0, 0.0}, tails[3] = {0.0, 0.0, 0.0};
    for (std::size_t i = 0; i < kernel.size(); ++i) {
        for (int k = 0; k < 3; ++k) {
            EXPECT_GT(kernel[i].weight[k], 0.0f);
            if (i < 11 || i > 13) EXPECT_LT(kernel[i].weight[k], kernel[11].weight[k] + kernel[12].weight[k] + kernel[13].weight[k]);
            sum[k] += kernel[i].weight[k];
            if (std::abs(kernel[i].offset) > 5.0f) tails[k] += kernel[i].weight[k];
        }
        EXPECT_NEAR(kernel[i].weight[0], kernel[kernel.size() - 1 - i].weight[0], 1e-4f);
    }
    for (int k = 0; k < 3; ++k) EXPECT_NEAR(sum[k], 1.0, 1e-3);
    EXPECT_GT(tails[0], tails[1]);
    EXPECT_GT(tails[1], tails[2]);
    EXPECT_GT(tails[0], 0.1);  // red keeps a good share beyond 5 mm
    EXPECT_LT(tails[2], 0.1);  // blue little

    // The pre-integrated diffuse: flat skin is Lambert's cosine; curved skin wraps past the
    // terminator, red furthest, and never exceeds 1.
    const int w = 64, h = 16;
    const std::vector<float> lut = preintegratedSkinLUT(w, h, 128);
    ASSERT_EQ(lut.size(), static_cast<std::size_t>(w * h * 3));
    for (float nDotL : {-0.5f, 0.0f, 0.5f, 1.0f}) {
        const RGB flat = preintegratedSkinAt(lut, w, h, nDotL, 0.0f);
        for (int k = 0; k < 3; ++k) EXPECT_NEAR(flat[k], std::max(nDotL, 0.0f), 0.04f);
    }
    const RGB curved = preintegratedSkinAt(lut, w, h, -0.2f, 1.0f);  // a 1 mm radius: an ear's edge
    EXPECT_GT(curved[0], 0.02f);
    EXPECT_GT(curved[0], curved[2]);
    for (float v : lut) {
        EXPECT_GE(v, 0.0f);
        EXPECT_LE(v, 1.0f + 1e-4f);
    }
    // Curved skin facing the light gets a little less than flat (light leaves toward the sides).
    EXPECT_LT(preintegratedSkinAt(lut, w, h, 1.0f, 1.0f)[2], preintegratedSkinAt(lut, w, h, 1.0f, 0.0f)[2] + 1e-4f);

    // The detail normal: unit-ish, pointing out, tileable (the first and last columns alike).
    const int d = 64;
    const std::vector<float> detail = microNormalTexture(d);
    ASSERT_EQ(detail.size(), static_cast<std::size_t>(d * d * 3));
    double meanZ = 0.0;
    for (int i = 0; i < d * d; ++i) {
        const float x = detail[i * 3] * 2.0f - 1.0f, y = detail[i * 3 + 1] * 2.0f - 1.0f, z = detail[i * 3 + 2] * 2.0f - 1.0f;
        EXPECT_NEAR(x * x + y * y + z * z, 1.0f, 0.02f);
        EXPECT_GT(z, 0.0f);
        meanZ += z;
    }
    EXPECT_GT(meanZ / (d * d), 0.8);
    double seam = 0.0;
    for (int y = 0; y < d; ++y) seam += std::abs(detail[(y * d) * 3] - detail[(y * d + d - 1) * 3]);
    EXPECT_LT(seam / d, 0.25);  // neighbors across the seam differ as any neighbors do

    // The prefiltered environment: roughness 0 is the map; roughness 1 is far smoother (the
    // studio's softboxes spread over the sphere) and keeps about the mean.
    const EnvironmentMap studio = makeEnvironment(Environment::Studio, 128, 64);
    const EnvironmentMap mirror = prefilterEnvironment(studio, 0.0f, 128, 64);
    double diff = 0.0;
    for (std::size_t i = 0; i < studio.rgb.size(); ++i) diff += std::abs(studio.rgb[i] - mirror.rgb[i]);
    EXPECT_LT(diff / studio.rgb.size(), 1e-3);
    const EnvironmentMap rough1 = prefilterEnvironment(studio, 1.0f, 32, 16, 256);
    auto spread = [](const EnvironmentMap& m) {
        double mean = 0.0, var = 0.0;
        for (int y = 0; y < m.height; ++y) for (int x = 0; x < m.width; ++x) mean += m.at(x, y)[1];
        mean /= m.width * m.height;
        for (int y = 0; y < m.height; ++y) for (int x = 0; x < m.width; ++x) var += std::pow(m.at(x, y)[1] - mean, 2.0);
        return std::make_pair(mean, std::sqrt(var / (m.width * m.height)));
    };
    const auto sharp = spread(studio), soft = spread(rough1);
    EXPECT_LT(soft.second, sharp.second * 0.5);
    EXPECT_GT(soft.first, sharp.first * 0.5);
    EXPECT_LT(soft.first, sharp.first * 2.0);
    // Toward the big softbox the mirror level is bright, the rough one far less so.
    const auto softbox = lightDirection(-50.0f, 45.0f);
    EXPECT_GT(environmentRadiance(mirror, softbox)[0], environmentRadiance(rough1, softbox)[0] * 2.0f);
}

TEST(AppDocument, OpensEvaluatesEditsAndUndoes) {
    auto doc = GroomDocument::open(kSampleGroom);
    ASSERT_TRUE(doc);
    EXPECT_EQ(doc->path(), kSampleGroom);
    EXPECT_FALSE(doc->dirty());
    const auto nodes = doc->nodes();
    ASSERT_FALSE(nodes.empty());
    const std::string scatter = nodeOfType(nodes, "Scatter").name;
    ASSERT_TRUE(doc->nodeSpec(scatter));
    const json density = doc->paramOrDefault(scatter, "density");
    ASSERT_TRUE(density.is_number());

    ViewRequest request;
    request.density = 0.5f;
    const ViewResult& view = doc->evaluate(request);
    ASSERT_TRUE(view.strands);
    EXPECT_TRUE(view.errors.empty()) << view.errors.front();
    const std::size_t before = view.strands->strandCount();
    EXPECT_GT(before, 0u);
    const std::uint64_t strandsVersion = view.versions.strands;

    // Half the density: fewer strands, one undo step, the versions move.
    EXPECT_EQ(doc->setParam(scatter, "density", density.get<double>() * 0.5), "");
    EXPECT_TRUE(doc->dirty());
    EXPECT_TRUE(doc->canUndo());
    EXPECT_FALSE(doc->canRedo());
    const ViewResult& half = doc->evaluate(request);  // the same object as `view`: the session's last
    ASSERT_TRUE(half.strands);
    const std::size_t halfCount = half.strands->strandCount();
    EXPECT_LT(halfCount, before);
    EXPECT_NE(half.versions.strands, strandsVersion);
    // The same value again records nothing.
    EXPECT_EQ(doc->setParam(scatter, "density", density.get<double>() * 0.5), "");
    EXPECT_EQ(doc->undoText(), "Undo " + scatter + ".density");

    EXPECT_EQ(doc->undo(), "");
    EXPECT_TRUE(doc->canRedo());
    EXPECT_FALSE(doc->canUndo());
    EXPECT_EQ(doc->paramOrDefault(scatter, "density"), density);
    EXPECT_EQ(doc->evaluate(request).strands->strandCount(), before);
    EXPECT_EQ(doc->redo(), "");
    EXPECT_EQ(doc->evaluate(request).strands->strandCount(), halfCount);

    // A slider's drag: steps unrecorded, one edit from where it started.
    const json dragStart = doc->paramOrDefault(scatter, "density");
    EXPECT_EQ(doc->setParam(scatter, "density", density.get<double>() * 0.3, false), "");
    EXPECT_EQ(doc->setParam(scatter, "density", density.get<double>() * 0.25, false), "");
    doc->recordEdit(scatter, "density", dragStart, doc->paramOrDefault(scatter, "density"));
    EXPECT_EQ(doc->undo(), "");
    EXPECT_EQ(doc->paramOrDefault(scatter, "density"), dragStart);

    // A bad value is refused and the graph stays as it was.
    EXPECT_NE(doc->setParam(scatter, "density", "not a number"), "");
    EXPECT_EQ(doc->paramOrDefault(scatter, "density"), dragStart);
    EXPECT_NE(doc->setParam("no_such_node", "density", 1.0), "");

    // Bypass is an edit too.
    const std::string length = nodeOfType(nodes, "Length").name;
    EXPECT_EQ(doc->setBypass(length, true), "");
    EXPECT_TRUE(nodeOfType(doc->nodes(), "Length").bypass);
    ViewRequest shownView = request;
    shownView.viewport = true;
    const std::uint64_t bypassed = doc->evaluate(shownView).strands->contentHash();
    EXPECT_EQ(doc->undo(), "");
    EXPECT_FALSE(nodeOfType(doc->nodes(), "Length").bypass);

    // So is when it runs: render only, the viewport skips it (as bypassed), exports do not.
    const std::uint64_t shown = doc->evaluate(shownView).strands->contentHash();
    const std::uint64_t exported = doc->evaluate(request).strands->contentHash();
    EXPECT_EQ(doc->setEvaluation(length, "renderOnly"), "");
    EXPECT_EQ(nodeOfType(doc->nodes(), "Length").evaluation, "renderOnly");
    EXPECT_EQ(doc->evaluate(shownView).strands->contentHash(), bypassed);
    EXPECT_EQ(doc->evaluate(request).strands->contentHash(), exported);
    EXPECT_NE(doc->setEvaluation(length, "sometimes"), "");
    EXPECT_EQ(doc->undo(), "");
    EXPECT_EQ(nodeOfType(doc->nodes(), "Length").evaluation, "always");
    EXPECT_EQ(doc->evaluate(shownView).strands->contentHash(), shown);
}

TEST(AppDocument, GraphEditsAddConnectRemoveAndUndo) {
    // The node graph's edits: each one undo step holding the whole graph, the session following.
    auto doc = GroomDocument::open(kSampleGroom);
    ASSERT_TRUE(doc);
    const auto nodes = doc->nodes();
    const std::size_t count = nodes.size();
    const std::string scatter = nodeOfType(nodes, "Scatter").name;
    const std::string output = nodeOfType(nodes, "Output").name;
    EXPECT_FALSE(doc->operatorTypes().empty());
    ViewRequest request;
    request.density = 0.5f;
    ASSERT_TRUE(doc->evaluate(request).errors.empty());

    // The default graph has a Noise named noise, so the added ones are noise1 and noise2.
    ASSERT_EQ(nodeOfType(nodes, "Noise").name, "noise");
    std::string noise;
    EXPECT_EQ(doc->addNode("Noise", 10.0f, 20.0f, &noise), "");
    EXPECT_EQ(noise, "noise1");
    EXPECT_EQ(doc->nodes().size(), count + 1);
    EXPECT_EQ(doc->nodes().back().name, noise);
    EXPECT_EQ(doc->nodes().back().type, "Noise");
    float x = 0.0f, y = 0.0f;
    ASSERT_TRUE(doc->nodePosition(noise, &x, &y));
    EXPECT_FLOAT_EQ(x, 10.0f);
    EXPECT_FLOAT_EQ(y, 20.0f);
    EXPECT_EQ(doc->undoText(), "Undo add " + noise);
    EXPECT_NE(doc->addNode("NoSuchOperator", 0.0f, 0.0f, nullptr), "");

    // Fed by the scatter: the session takes the graph; a self-feed is refused.
    const std::size_t connections = doc->connections().size();
    EXPECT_EQ(doc->connectNodes(scatter, "", noise, "strands"), "");
    EXPECT_EQ(doc->connections().size(), connections + 1);
    EXPECT_NE(doc->connectNodes(noise, "", noise, "strands"), "");
    // The core's checks and names (graph_edit.h), as Maya's Graph Editor makes them: a mesh into
    // strands refused before the session sees it, the next Noise named noise2 (not noise11), a second
    // Scatter seeded apart.
    const std::string scalp = nodeOfType(nodes, "Scalp").name;
    EXPECT_NE(doc->connectNodes(scalp, "", noise, "strands").find("cannot connect"), std::string::npos);
    std::string noise2, scatter2;
    EXPECT_EQ(doc->addNode("Noise", 0.0f, 0.0f, &noise2), "");
    EXPECT_EQ(noise2, "noise2");
    EXPECT_EQ(doc->addNode("Scatter", 0.0f, 0.0f, &scatter2), "");
    EXPECT_NE(doc->paramOrDefault(scatter2, "seed"), doc->paramOrDefault(scatter, "seed"));
    EXPECT_EQ(doc->undo(), "");
    EXPECT_EQ(doc->undo(), "");
    EXPECT_TRUE(doc->evaluate(request).errors.empty());
    EXPECT_EQ(doc->disconnectInput(noise, "strands"), "");
    EXPECT_EQ(doc->connections().size(), connections);
    EXPECT_NE(doc->disconnectInput(noise, "strands"), "");  // nothing feeds it now
    EXPECT_EQ(doc->undo(), "");
    EXPECT_EQ(doc->connections().size(), connections + 1);

    // A move is not a step; removing the node takes its connections with it; the output stays.
    doc->setNodePosition(noise, 30.0f, 40.0f);
    ASSERT_TRUE(doc->nodePosition(noise, &x, &y));
    EXPECT_FLOAT_EQ(x, 30.0f);
    EXPECT_EQ(doc->undoText(), "Undo connect " + scatter + " to " + noise);
    EXPECT_NE(doc->removeNode(output), "");
    EXPECT_EQ(doc->removeNode(noise), "");
    EXPECT_EQ(doc->nodes().size(), count);
    EXPECT_EQ(doc->connections().size(), connections);
    EXPECT_EQ(doc->undo(), "");
    EXPECT_EQ(doc->nodes().size(), count + 1);
    EXPECT_EQ(doc->redo(), "");
    EXPECT_EQ(doc->nodes().size(), count);
    EXPECT_TRUE(doc->evaluate(request).errors.empty());

    // Positions ride along in the file.
    EXPECT_EQ(doc->undo(), "");
    doc->setNodePosition(scatter, -5.0f, 7.5f);
    const std::string path = tempPath("groomlab_app_graph_edits.groom");
    doc->save(path);
    auto again = GroomDocument::open(path);
    ASSERT_TRUE(again);
    ASSERT_TRUE(again->nodePosition(scatter, &x, &y));
    EXPECT_FLOAT_EQ(x, -5.0f);
    EXPECT_FLOAT_EQ(y, 7.5f);
    EXPECT_EQ(again->nodes().size(), count + 1);
    std::filesystem::remove(path);
}

TEST(AppTheme, FontsBrushesAndColors) {
    // The bundled fonts register; the rail's brushes are grouped by category in order.
    EXPECT_TRUE(theme::loadFonts());
    EXPECT_EQ(theme::uiFont(14, 800).family(), theme::uiFamily());
    EXPECT_EQ(theme::monoFont(13).family(), theme::monoFamily());
    // The rail's brushes are the core's catalog (brush_catalog.h) without Select (the rail's own
    // tool), in its order: Create first, then Shape, Trim, Paint, and Freeze among the guide tools.
    const std::vector<theme::BrushEntry>& brushes = theme::brushes();
    EXPECT_EQ(brushes.size(), brushCatalog().size() - 1);
    EXPECT_EQ(brushes.front().name, "place");
    int groups = 1;
    for (std::size_t i = 1; i < brushes.size(); ++i) {
        if (brushes[i].category != brushes[i - 1].category) ++groups;
        EXPECT_GE(static_cast<int>(brushes[i].category), static_cast<int>(brushes[i - 1].category)) << "grouped in order";
    }
    EXPECT_EQ(groups, 5);
    ASSERT_TRUE(theme::brush("freeze"));
    EXPECT_EQ(theme::brush("freeze")->category, theme::BrushCategory::Guides);
    EXPECT_EQ(brushes.back().name, "freeze");
    ASSERT_TRUE(theme::brush("cut"));
    EXPECT_EQ(theme::brush("cut")->category, theme::BrushCategory::Trim);
    EXPECT_EQ(theme::brush("shape")->category, theme::BrushCategory::Shape);
    EXPECT_EQ(theme::brush("grab"), nullptr) << "Comb does what Grab did";
    EXPECT_EQ(theme::brush("clump"), nullptr) << "the Paint brush paints the clump attribute";
    EXPECT_EQ(theme::brush("no such brush"), nullptr);
    EXPECT_EQ(theme::brush("select"), nullptr) << "the rail's own tool";
    // Playful's colors (the categories in the primaries; Charcoal, the default, tints its icons alike).
    ASSERT_TRUE(theme::setTheme(QStringLiteral("playful")));
    struct Restore {
        ~Restore() { theme::setTheme(theme::defaultTheme()); }
    } restore;
    EXPECT_EQ(theme::categoryColor(theme::BrushCategory::Shape), theme::blue());
    EXPECT_EQ(theme::categoryColor(theme::BrushCategory::Create), theme::emerald());
    EXPECT_EQ(theme::categoryColor(theme::BrushCategory::Guides), theme::white());
    EXPECT_EQ(theme::brushTitle("comb"), QStringLiteral("Comb"));
    EXPECT_EQ(theme::brushTitle("straighten"), QStringLiteral("Straighten"));
    EXPECT_EQ(theme::brush("comb")->tip.rfind("Comb: ", 0), 0u) << "the catalog's label and hint";
    EXPECT_EQ(theme::nodeCategoryColor("shape"), theme::blue());
    EXPECT_EQ(theme::nodeCategoryColor("look"), theme::yellow());
    EXPECT_EQ(theme::portColor("strands"), theme::yellow());
    EXPECT_EQ(theme::glass().alpha(), 184);  // rgba(31,32,37,0.72)

    // The colors are the Maya host's theme file's (embedded at build time), not a copy of it.
    std::ifstream in(std::string(GROOMLAB_TEST_DATA) + "/../../hosts/maya/themes/playful.json");
    ASSERT_TRUE(in);
    const json playful = json::parse(in);
    auto hex = [&](const std::string& value) {
        const std::string named = playful["palette"].value(value, value);
        return QColor(QString::fromStdString(named));
    };
    EXPECT_EQ(theme::red(), hex("red"));
    EXPECT_EQ(theme::blue(), hex("blue"));
    EXPECT_EQ(theme::purple(), hex("purple"));
    EXPECT_EQ(theme::white(), hex("white"));
    EXPECT_EQ(theme::nodeCategoryColor("grow"), hex(playful["graph"]["categories"]["Grow"]));
    EXPECT_EQ(theme::nodeCategoryColor("feathers"), hex(playful["graph"]["categories"]["Feathers"]));
    EXPECT_EQ(theme::nodeCategoryColor("utility"), hex(playful["graph"]["categories"]["Utility"]));
    EXPECT_EQ(theme::portColor("curves"), hex(playful["graph"]["ports"]["Curves"]));
    EXPECT_EQ(theme::nodeBody(), hex(playful["graph"]["node"]));
    EXPECT_EQ(theme::categoryColor(theme::BrushCategory::Trim), hex(playful["icons"]["trim"]));
    EXPECT_EQ(theme::uiColor("panel"), hex(playful["ui"]["panel"]));
    EXPECT_EQ(theme::uiColor("accent"), theme::blue());  // a ui key naming a palette color
    EXPECT_TRUE(theme::styleSheet().contains(hex(playful["ui"]["hover"]).name()));
    EXPECT_FALSE(theme::styleSheet().contains(QStringLiteral("@")));
}

TEST(AppTheme, EveryShippedThemeIsEmbeddedAndCharcoalIsSolid) {
    // Every theme file the Maya host ships is a resource; Charcoal is the app's default, solid:
    // opaque panels with a thin edge, its own text tones, an accent pill for the chosen segment and
    // the Render button, tinted node headers; Playful (glass, round filled headers) is still the
    // theme the others extend.
    const std::string folder = std::string(GROOMLAB_TEST_DATA) + "/../../hosts/maya/themes";
    QStringList files;
    for (const auto& entry : std::filesystem::directory_iterator(folder)) {
        if (entry.path().extension() == ".json") files << QString::fromStdString(entry.path().stem().string());
    }
    files.sort();
    EXPECT_EQ(theme::themeNames(), files);
    EXPECT_TRUE(theme::themeNames().contains(QStringLiteral("charcoal")));
    EXPECT_EQ(theme::themeLabel(QStringLiteral("charcoal")), QStringLiteral("Charcoal"));
    EXPECT_EQ(theme::themeLabel(QStringLiteral("my_theme")), QStringLiteral("My Theme"));
    EXPECT_EQ(theme::defaultTheme(), QStringLiteral("charcoal"));
    ASSERT_EQ(theme::currentTheme(), theme::defaultTheme());
    EXPECT_FALSE(theme::glassy());
    ASSERT_TRUE(theme::setTheme(QStringLiteral("playful")));
    EXPECT_TRUE(theme::glassy());
    EXPECT_FALSE(theme::glassEdge().isValid());
    EXPECT_EQ(theme::nodeRadius(), 14.0);
    EXPECT_FALSE(theme::tintedHeaders());
    EXPECT_EQ(theme::segmentFill(theme::red()), theme::red());
    EXPECT_EQ(theme::sliderFill(theme::purple()), theme::purple()) << "Playful's slider cycle: each slider its own color";
    EXPECT_EQ(theme::actionFill(theme::yellow()), theme::yellow());

    std::ifstream in(folder + "/charcoal.json");
    ASSERT_TRUE(in);
    const json charcoal = json::parse(in);
    auto hex = [&](const json& value) { return QColor(QString::fromStdString(value.get<std::string>())); };
    ASSERT_TRUE(theme::setTheme(QStringLiteral("charcoal")));
    struct Restore {
        ~Restore() { theme::setTheme(theme::defaultTheme()); }
    } restore;
    EXPECT_FALSE(theme::glassy());
    EXPECT_EQ(theme::blue(), hex(charcoal["palette"]["blue"]));
    EXPECT_EQ(theme::glass(), hex(charcoal["ui"]["panel"]));
    EXPECT_EQ(theme::glass().alpha(), 255) << "solid panels, no glass";
    EXPECT_EQ(theme::glassPanel(), hex(charcoal["ui"]["panel"]));
    EXPECT_EQ(theme::glassEdge(), hex(charcoal["ui"]["outline"]));
    EXPECT_EQ(theme::text(), hex(charcoal["ui"]["text"]));
    EXPECT_EQ(theme::textMuted(), hex(charcoal["ui"]["textDim"]));
    EXPECT_EQ(theme::backdrop(), hex(charcoal["ui"]["background"]));
    EXPECT_EQ(theme::renderRed(), hex(charcoal["ui"]["accent"])) << "the primary action: the off-white pill";
    EXPECT_EQ(theme::readableOn(theme::renderRed()), theme::ink());
    EXPECT_EQ(theme::segmentFill(theme::red()), hex(charcoal["ui"]["accent"]));
    EXPECT_EQ(theme::segmentText(Qt::white), hex(charcoal["ui"]["accentText"]));
    // Every slider takes the theme's one slider color (ui.sliders ["silver"]), as in Maya; filled
    // actions (Export, Bake, Paint) are the accent pill.
    const QColor silver = hex(charcoal["palette"][charcoal["ui"]["sliders"][0].get<std::string>()]);
    EXPECT_EQ(theme::sliderFill(theme::yellow()), silver);
    EXPECT_EQ(theme::sliderFill(theme::purple()), silver);
    EXPECT_EQ(theme::actionFill(theme::emerald()), hex(charcoal["ui"]["accent"]));
    EXPECT_EQ(theme::actionText(theme::ink()), hex(charcoal["ui"]["accentText"]));
    EXPECT_EQ(theme::nodeRadius(), charcoal["graph"]["cornerRadius"].get<double>());
    EXPECT_TRUE(theme::tintedHeaders());
    EXPECT_EQ(theme::nodeBorder(), hex(charcoal["graph"]["nodeBorder"]));
    EXPECT_EQ(theme::nodeCategoryColor("shape"), hex(charcoal["graph"]["categories"]["Shape"]));
    EXPECT_EQ(theme::portColor("guides"), hex(charcoal["graph"]["ports"]["Guides"]));
    const QString sheet = theme::styleSheet();
    EXPECT_TRUE(sheet.contains(hex(charcoal["ui"]["panel"]).name()));
    EXPECT_TRUE(sheet.contains(hex(charcoal["ui"]["selection"]).name()));
    EXPECT_FALSE(sheet.contains(QStringLiteral("@")));
    EXPECT_FALSE(sheet.contains(QStringLiteral("#F3F1EC"), Qt::CaseInsensitive)) << "the glass text tone";
    // The window's chrome: solid docks, the menu bar, the tool bar, tabs and separators in the
    // theme's tones; no white washes on a solid theme (its raised tone instead).
    for (const char* selector : {"QDockWidget::title", "QMenuBar", "QToolBar", "QTabBar::tab", "QMainWindow::separator", "QSplitter::handle", "#dockBody"}) {
        EXPECT_TRUE(sheet.contains(QLatin1String(selector))) << selector;
    }
    EXPECT_TRUE(sheet.contains(hex(charcoal["ui"]["background"]).name()));
    EXPECT_TRUE(sheet.contains(hex(charcoal["ui"]["raised"]).name()));
    EXPECT_TRUE(sheet.contains(hex(charcoal["ui"]["outline"]).name()));
    EXPECT_FALSE(sheet.contains(QStringLiteral("rgba(255,255,255"))) << "a glass wash on a solid theme";
    EXPECT_EQ(theme::chip(), hex(charcoal["ui"]["raised"]));
    EXPECT_EQ(theme::graphCanvas(), hex(charcoal["graph"]["canvas"]));
    EXPECT_EQ(theme::graphGrid(), hex(charcoal["graph"]["grid"]));
    // The viewport's overlays in the theme's viewport colors, linear as the Maya host sends them
    // (groomLabDisplayTheme).
    auto linear = [](const QColor& c) {
        auto f = [](double v) { return v <= 0.04045 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4); };
        return std::array<double, 3>{f(c.redF()), f(c.greenF()), f(c.blueF())};
    };
    const DisplayTheme display = theme::displayTheme();
    const auto frozen = linear(hex(charcoal["viewport"]["frozen"]));
    for (int k = 0; k < 3; ++k) EXPECT_NEAR(display.frozen[k], frozen[k], 1e-4);
    const auto parts = linear(hex(charcoal["viewport"]["parts"]));
    for (int k = 0; k < 3; ++k) EXPECT_NEAR(display.parts[k], parts[k], 1e-4);
    EXPECT_FLOAT_EQ(display.pointSize, charcoal["viewport"]["pointSize"].get<float>());
    EXPECT_NE(display.frozen, DisplayTheme().frozen) << "not the core's defaults";
    // An unknown theme changes nothing; a theme without "glass" (Polar) takes Playful's (it extends it).
    EXPECT_FALSE(theme::setTheme(QStringLiteral("no such theme")));
    EXPECT_EQ(theme::currentTheme(), QStringLiteral("charcoal"));
    ASSERT_TRUE(theme::setTheme(QStringLiteral("polar")));
    EXPECT_TRUE(theme::glassy());
    EXPECT_EQ(theme::nodeRadius(), 12.0);
    EXPECT_EQ(theme::sliderFill(theme::yellow()), theme::uiColor("accent")) << "Polar's sliders: [\"accent\"]";
}

TEST(AppTheme, ViewportOverlayFollowsTheCorners) {
    // What stays over the viewport once the window has docks: the tool's name top-left, Undo and
    // Redo top-right, the stats bottom-left, the refine line
    // bottom-right, the toast centered above them, the prompt a little above the middle, the
    // empty card in the middle.
    theme::ViewportOverlayRequest request;
    request.view = QSize(1000, 700);
    request.toolLabel = QSize(90, 26);
    request.history = QSize(60, 34);
    request.stats = QSize(300, 18);
    request.refine = QSize(160, 18);
    request.toast = QSize(380, 36);
    request.prompt = QSize(440, 160);
    request.emptyCard = QSize(376, 300);
    const theme::ViewportOverlayLayout l = theme::layoutViewportOverlay(request);
    EXPECT_EQ(l.toolLabel.topLeft(), QPoint(12, 12));
    EXPECT_EQ(l.history.topRight(), QPoint(1000 - 12 - 1, 12));
    EXPECT_EQ(l.stats.left(), 12);
    EXPECT_EQ(l.stats.bottom(), 700 - 12 - 1);
    EXPECT_EQ(l.refine.right(), 1000 - 12 - 1);
    EXPECT_EQ(l.refine.bottom(), 700 - 12 - 1);
    EXPECT_NEAR(l.toast.center().x(), 500, 1);
    EXPECT_LT(l.toast.bottom(), l.stats.top());
    EXPECT_NEAR(l.prompt.center().x(), 500, 1);
    EXPECT_LT(l.prompt.center().y(), 350) << "a little above the middle";
    EXPECT_NEAR(l.emptyCard.center().y(), 350, 1);
    // Nothing asked, nothing placed.
    const theme::ViewportOverlayLayout none = theme::layoutViewportOverlay({QSize(1000, 700)});
    EXPECT_TRUE(none.toolLabel.isNull() && none.history.isNull() && none.stats.isNull() && none.toast.isNull());
}

TEST(AppTheme, ViewportOverlayHasNoOverlapsAtSmallSizes) {
    // A viewport squeezed between docks (or a small screen at 150%): the pieces stay inside it and
    // never meet; the refine line goes above the stats when both cannot share the bottom line.
    for (const QSize& size : {QSize(1280, 720), QSize(700, 420), QSize(480, 320), QSize(360, 260)}) {
        SCOPED_TRACE(std::to_string(size.width()) + "x" + std::to_string(size.height()));
        theme::ViewportOverlayRequest request;
        request.view = size;
        request.toolLabel = QSize(120, 26);
        request.history = QSize(60, 34);
        request.stats = QSize(300, 18);
        request.refine = QSize(160, 18);
        request.toast = QSize(std::min(560, size.width() - 48), 36);
        const theme::ViewportOverlayLayout l = theme::layoutViewportOverlay(request);
        const QRect view(QPoint(0, 0), size);
        for (const QRect& r : {l.toolLabel, l.history, l.stats, l.refine, l.toast}) {
            EXPECT_TRUE(view.contains(r)) << r.x() << "," << r.y() << " " << r.width() << "x" << r.height();
        }
        for (const QRect& r : {l.toolLabel, l.stats, l.refine, l.toast}) {
            EXPECT_FALSE(l.history.intersects(r)) << "Undo and Redo over another piece at " << r.x() << "," << r.y();
        }
        EXPECT_FALSE(l.stats.intersects(l.refine)) << "the refine line over the stats";
        EXPECT_FALSE(l.toast.intersects(l.stats)) << "the toast over the stats";
        EXPECT_FALSE(l.toast.intersects(l.refine)) << "the toast over the refine line";
    }
}

TEST(AppPresets, ShippedPresetsApplySaveCopyAndPaste) {
    // The shipped presets are the Maya host's files, read through the core; applying one
    // replaces the graph as one undo step, fitted to the scalp, the maps it reads made.
    const std::vector<PresetInfo> presets = availablePresets();
    ASSERT_GT(presets.size(), 10u);
    const std::string dir = shippedPresetDir();
    EXPECT_NE(dir.find("presets"), std::string::npos) << dir;
    auto doc = GroomDocument::open(kSampleGroom);
    ASSERT_TRUE(doc);
    const std::size_t nodesBefore = doc->nodes().size();
    PresetApplied applied;
    EXPECT_EQ(applyPresetFile(*doc, presetPath("painted_maps", presetDirectories()), &applied), "");
    EXPECT_EQ(applied.title, "Painted maps");
    EXPECT_EQ(applied.nodes, doc->nodes().size());
    ASSERT_EQ(applied.mapsMade.size(), 1u);
    EXPECT_EQ(applied.mapsMade[0], "groomMasks");
    ASSERT_TRUE(doc->map("groomMasks"));
    EXPECT_EQ(doc->map("groomMasks")->channels, kColorMap);
    EXPECT_EQ(doc->undoText(), "Undo preset Painted maps");
    EXPECT_NE(presetMessage(applied).find("Painted maps"), std::string::npos);
    EXPECT_EQ(doc->undo(), "");
    EXPECT_EQ(doc->nodes().size(), nodesBefore);
    EXPECT_EQ(doc->redo(), "");
    // Fitted: the sample's scalp is not 20 units, so a length scaled by the ratio.
    EXPECT_TRUE(applied.fitted);
    EXPECT_NE(applied.fitFactor, 1.0);
    // flow_with_guides' Blend reads the guideAreas map: made black (a gray vertex map), so the
    // groom evaluates at once, all fur until it is painted.
    PresetApplied flow;
    EXPECT_EQ(applyPresetFile(*doc, presetPath("flow_with_guides", presetDirectories()), &flow), "");
    ASSERT_EQ(flow.mapsMade, std::vector<std::string>{"guideAreas"});
    ASSERT_TRUE(doc->map("guideAreas"));
    EXPECT_EQ(doc->map("guideAreas")->kind, "vertices");
    EXPECT_EQ(doc->map("guideAreas")->channels, kGrayMap);
    {
        ViewRequest request;
        request.density = 0.05f;
        const ViewResult& view = doc->evaluate(request);
        EXPECT_TRUE(view.errors.empty()) << view.errors.front();
        ASSERT_TRUE(view.strands);
        EXPECT_GT(view.strands->strandCount(), 0u);
    }
    // Every shipped preset applies to this groom (the session takes each graph).
    for (const PresetInfo& p : presets) EXPECT_EQ(applyPresetFile(*doc, p.path), "") << p.name;
    EXPECT_NE(applyPresetFile(*doc, "C:/no/such/preset.json"), "");
    EXPECT_NE(applyPresetText(*doc, "not json", false), "");
    EXPECT_NE(applyPresetText(*doc, R"({"nodes": [{"name": "x", "type": "NoSuchOperator"}]})", false), "")
        << "an unknown operator is refused, the graph kept";

    // Saved as a user preset (the folder from the environment), listed with the shipped ones.
    const std::filesystem::path user = std::filesystem::temp_directory_path() / "groomlab_app_user_presets";
    std::filesystem::remove_all(user);
#ifdef _WIN32
    _putenv_s("GROOMLAB_USER_PRESETS", user.u8string().c_str());
#else
    setenv("GROOMLAB_USER_PRESETS", user.u8string().c_str(), 1);
#endif
    EXPECT_EQ(applyPresetFile(*doc, presetPath("hair_basics", presetDirectories())), "");
    std::string saved;
    EXPECT_EQ(savePresetFile(*doc, "my look", false, &saved), "");
    EXPECT_TRUE(std::filesystem::exists(std::filesystem::u8path(saved)));
    EXPECT_NE(savePresetFile(*doc, "my look", false), "") << "a file in the way";
    EXPECT_EQ(savePresetFile(*doc, "my look", true), "");
    EXPECT_NE(savePresetFile(*doc, "bad/name", true), "");
    bool listed = false;
    for (const PresetInfo& p : availablePresets()) listed = listed || (p.name == "my look" && p.title == "My look" && p.category.empty());
    EXPECT_TRUE(listed);
    {
        std::ifstream savedFile(std::filesystem::u8path(saved));
        const json file = json::parse(savedFile);
        EXPECT_TRUE(file.contains("size"));
        EXPECT_FALSE(file.contains("about"));
    }

    // Copy Graph gives a preset titled after the groom; Paste Graph takes it back, one undo step.
    const std::string text = graphAsText(*doc);
    const json copied = json::parse(text);
    EXPECT_EQ(copied["about"]["title"], "strokes");
    EXPECT_EQ(copied["about"]["category"], "Shared");
    const std::string scatter = nodeOfType(doc->nodes(), "Scatter").name;
    EXPECT_EQ(doc->setParam(scatter, "density", 7.0), "");
    PresetApplied pasted;
    EXPECT_EQ(pasteGraphText(*doc, "```json\n" + text + "\n```", &pasted), "");
    EXPECT_EQ(pasted.title, "strokes");
    EXPECT_FALSE(pasted.fitted) << "copied from this scalp: the same size";
    EXPECT_NE(doc->paramOrDefault(scatter, "density"), json(7.0));
    EXPECT_EQ(doc->undoText(), "Undo preset strokes");
    EXPECT_NE(pasteGraphText(*doc, "   "), "");
    EXPECT_NE(pasteGraphText(*doc, "{\"version\": 1}"), "");
#ifdef _WIN32
    _putenv_s("GROOMLAB_USER_PRESETS", "");
#else
    unsetenv("GROOMLAB_USER_PRESETS");
#endif
    std::filesystem::remove_all(user);
}

TEST(AppPresets, InsertsAPresetAsACompoundAfterTheSelectedNode) {
    // Insert after selected node: the preset's look as one Compound after the node (before the
    // Output with none), fitted, one undo step; every add-on inserts.
    auto doc = GroomDocument::open(kSampleGroom);
    ASSERT_TRUE(doc);
    ASSERT_EQ(applyPresetFile(*doc, presetPath("hair_basics", presetDirectories())), "");
    const std::size_t before = doc->nodes().size();
    const std::string clump = nodeOfType(doc->nodes(), "Clump").name;
    PresetApplied applied;
    EXPECT_EQ(insertPresetFile(*doc, presetPath("frizz", presetDirectories()), clump, &applied), "");
    EXPECT_EQ(applied.nodeMade, "frizz");
    EXPECT_EQ(applied.after, clump);
    EXPECT_EQ(applied.title, "Frizz");
    EXPECT_TRUE(applied.fitted);
    EXPECT_EQ(doc->nodes().size(), before + 1);
    EXPECT_EQ(nodeOfType(doc->nodes(), "Compound").name, "frizz");
    const json graph = json::parse(doc->graphJson());
    std::string fedByClump, feedsFrizz;
    for (const json& c : graph["connections"]) {
        if (c["from"] == clump) fedByClump = c["to"];
        if (c["to"] == "frizz") feedsFrizz = c["from"];
    }
    EXPECT_EQ(fedByClump, "frizz");
    EXPECT_EQ(feedsFrizz, clump);
    EXPECT_EQ(doc->undoText(), "Undo insert Frizz");
    EXPECT_EQ(doc->undo(), "");
    EXPECT_EQ(doc->nodes().size(), before);
    EXPECT_EQ(doc->redo(), "");
    EXPECT_EQ(doc->nodes().size(), before + 1);
    // Nothing selected: before the Output.
    EXPECT_EQ(insertPresetFile(*doc, presetPath("wet", presetDirectories()), "", &applied), "");
    EXPECT_EQ(applied.after, "");
    const json after = json::parse(doc->graphJson());
    std::string feedsOutput;
    for (const json& c : after["connections"]) {
        if (c["to"] == after["output"]) feedsOutput = c["from"];
    }
    EXPECT_EQ(feedsOutput, "wet");
    for (const char* name : {"flyaways", "cards_mid", "gravity"}) EXPECT_EQ(insertPresetFile(*doc, presetPath(name, presetDirectories()), ""), "") << name;
    EXPECT_NE(insertPresetFile(*doc, presetPath("wet", presetDirectories()), "noSuchNode"), "");
    EXPECT_NE(insertPresetFile(*doc, "C:/no/such/preset.json", ""), "");
}

TEST(AppDocument, SavesAndExports) {
    auto doc = GroomDocument::open(kSampleGroom);
    const std::string scatter = nodeOfType(doc->nodes(), "Scatter").name;
    const double density = doc->paramOrDefault(scatter, "density").get<double>();
    ASSERT_EQ(doc->setParam(scatter, "density", density * 0.5), "");
    ViewRequest request;
    const std::size_t count = doc->evaluate(request).strands->strandCount();

    const std::string saved = tempPath("groomlab_app_saved.groom");
    doc->save(saved);
    EXPECT_EQ(doc->path(), saved);
    EXPECT_FALSE(doc->dirty());
    auto again = GroomDocument::open(saved);
    EXPECT_DOUBLE_EQ(again->paramOrDefault(scatter, "density").get<double>(), density * 0.5);
    EXPECT_EQ(again->evaluate(request).strands->strandCount(), count);
    EXPECT_EQ(again->file().data.scalp->pointCount(), doc->file().data.scalp->pointCount());
    std::remove(saved.c_str());

    const std::string abc = tempPath("groomlab_app_export.abc");
    const std::size_t written = doc->exportAlembic(abc);
    EXPECT_EQ(written, count);  // the view was at full density
    const auto frames = readAlembicCurves(abc);
    ASSERT_EQ(frames.size(), 1u);
    EXPECT_EQ(frames[0].strands.strandCount(), count);
    EXPECT_FALSE(frames[0].uvs.empty());  // the scalp has UVs
    std::remove(abc.c_str());
    // The export left the view's document alone: the next view is a cache hit of the same strands.
    EXPECT_EQ(doc->evaluate(request).strands->strandCount(), count);
}

TEST(AppDocument, ScaleWithScalpIsSavedAndATransferKeepsTheScale) {
    auto doc = GroomDocument::open(kSampleGroom);  // a file without a reference: its first size is 1x
    ViewRequest request;
    const std::size_t count = doc->evaluate(request).strands->strandCount();
    EXPECT_EQ(doc->session().groomScale(), 1.0);
    const double area = doc->session().scaleArea();
    ASSERT_GT(area, 0.0);
    EXPECT_DOUBLE_EQ(doc->currentFile().data.scaleReference, area) << "saved: the reference in use";
    // Moved onto a scalp twice the size, the groom keeps its scale (1x: its sizes as they were, the
    // same density on four times the skin, as a transfer always did).
    const ScalpMesh& was = *doc->file().data.scalp;
    std::vector<Vec3f> points = was.points();
    for (Vec3f& p : points) p = p * 2.0f;
    auto big = std::make_shared<ScalpMesh>(points, was.triangles());
    if (was.hasUVs()) big->setCornerUVs(was.cornerUVs());
    ASSERT_EQ(doc->transferToScalp(big, false, false), "");
    EXPECT_NEAR(doc->session().groomScale(), 1.0, 1e-4);
    EXPECT_NEAR(doc->currentFile().data.scaleReference / (4.0 * area), 1.0, 1e-4);
    EXPECT_GT(doc->evaluate(request).strands->strandCount(), 3 * count);
    // Off and Transform Only: saved as such. Set Scale Reference: the size now is 1x.
    doc->setScaleSource(ScaleSource::Off);
    EXPECT_EQ(doc->currentFile().data.scaleSource, ScaleSource::Off);
    doc->setScaleSource(ScaleSource::Transform);
    EXPECT_EQ(doc->currentFile().data.scaleSource, ScaleSource::Transform);
    doc->setScaleSource(ScaleSource::Area);
    doc->setScaleReference();
    EXPECT_EQ(doc->session().groomScale(), 1.0);
    // Fit to the new scalp's size: the scalp twice the size is the old one resized, so the groom is
    // scaled 2x (as many strands as at 1x).
    auto fitted = GroomDocument::open(kSampleGroom);
    fitted->evaluate(request);
    TransferOptions fit;
    fit.fitSize = true;
    ASSERT_EQ(fitted->transferToScalp(big, fit, false), "");
    EXPECT_NEAR(fitted->session().groomScale(), 2.0, 1e-4);
    EXPECT_EQ(fitted->evaluate(request).strands->strandCount(), count);
}

TEST(AppDocument, SeveralScalpsAndAReferenceOpenSaveAndExport) {
    // A groom file with another scalp (its own Scalp and Scatter) and a Reference node bringing in
    // the sample groom: the app evaluates it through the core, saves it with its scalps, and
    // exports it with root UVs on the joined scalps.
    GroomFile file = readGroomFile(kSampleGroom);
    std::vector<Vec3f> points;
    std::vector<ScalpMesh::Triangle> tris;
    for (int z = 0; z <= 4; ++z) {
        for (int x = 0; x <= 4; ++x) points.emplace_back(40.0f + 2.0f * x, 0.0f, 2.0f * z);
    }
    for (int z = 0; z < 4; ++z) {
        for (int x = 0; x < 4; ++x) {
            const int a = z * 5 + x;
            tris.push_back({a, a + 5, a + 1});
            tris.push_back({a + 5, a + 6, a + 1});
        }
    }
    auto ear = std::make_shared<ScalpMesh>(std::move(points), std::move(tris));
    std::vector<ScalpMesh::UV> uvs(ear->triangleCount() * 3, ScalpMesh::UV{0.5f, 0.5f});
    ear->setCornerUVs(std::move(uvs));
    file.data.scalps = {{"ear", ear}};
    std::string library = kSampleGroom;
    std::replace(library.begin(), library.end(), '\\', '/');
    file.data.graphJson = R"({"version": 1, "nodes": [
    {"name": "scalp", "type": "Scalp"},
    {"name": "scatter", "type": "Scatter", "params": {"density": 0.5}},
    {"name": "earScalp", "type": "Scalp", "params": {"input": "ear"}},
    {"name": "earScatter", "type": "Scatter", "params": {"density": 4.0, "seed": 3}},
    {"name": "merge", "type": "Merge"},
    {"name": "library", "type": "Reference", "params": {"path": ")" + library + R"(", "bind": "world"}},
    {"name": "merge2", "type": "Merge"},
    {"name": "output", "type": "Output"}],
    "connections": [{"from": "scalp", "to": "scatter", "input": "scalp"},
                    {"from": "earScalp", "to": "earScatter", "input": "scalp"},
                    {"from": "scatter", "to": "merge", "input": "a"},
                    {"from": "earScatter", "to": "merge", "input": "b"},
                    {"from": "merge", "to": "merge2", "input": "a"},
                    {"from": "library", "to": "merge2", "input": "b"},
                    {"from": "merge2", "to": "output", "input": "strands"}], "output": "output"})";
    const std::string path = tempPath("groomlab_app_scalps.groom");
    writeGroomFile(path, file);

    auto doc = GroomDocument::open(path);
    ViewRequest request;
    const ViewResult& view = doc->evaluate(request);
    ASSERT_TRUE(view.strands) << (view.errors.empty() ? "" : view.errors.front());
    const std::size_t count = view.strands->strandCount();
    const std::size_t headTriangles = file.data.scalp->triangleCount();
    std::size_t onEar = 0, unrooted = 0;
    for (const StrandRoot& r : view.strands->roots()) {
        onEar += r.face >= 0 && static_cast<std::size_t>(r.face) >= headTriangles;
        unrooted += r.face < 0;
    }
    EXPECT_GT(onEar, 100u);
    EXPECT_GT(unrooted, 100u);  // the library's, where the file has them

    const std::string saved = tempPath("groomlab_app_scalps_saved.groom");
    doc->save(saved);
    auto again = GroomDocument::open(saved);
    ASSERT_EQ(again->file().data.scalps.size(), 1u);
    EXPECT_EQ(again->file().data.scalps[0].first, "ear");
    EXPECT_EQ(again->evaluate(request).strands->strandCount(), count);
    const std::string abc = tempPath("groomlab_app_scalps.abc");
    EXPECT_EQ(doc->exportAlembic(abc), count);
    const auto frames = readAlembicCurves(abc);
    ASSERT_EQ(frames.size(), 1u);
    EXPECT_EQ(frames[0].strands.strandCount(), count);
    for (const std::string& p : {path, saved, abc}) std::remove(p.c_str());
}

TEST(AppDocument, NewGroomOnAMesh) {
    const auto mesh = parseObjMesh("v -1 0 -1\nv 1 0 -1\nv 1 0 1\nv -1 0 1\nvt 0 0\nvt 1 0\nvt 1 1\nvt 0 1\nf 1/1 2/2 3/3 4/4\n");
    auto doc = GroomDocument::newOnMesh(mesh, "plane.obj");
    EXPECT_TRUE(doc->path().empty());
    EXPECT_FALSE(doc->nodes().empty());  // the default graph
    ViewRequest request;
    const ViewResult& view = doc->evaluate(request);
    EXPECT_TRUE(view.scalp);
    EXPECT_TRUE(view.strands || !view.errors.empty());
    const Bounds b = doc->sceneBounds();
    EXPECT_TRUE(b.valid);
    EXPECT_LE(b.lo.x, -1.0f);
    EXPECT_GE(b.hi.x, 1.0f);
    EXPECT_THROW(GroomDocument::newOnMesh(nullptr, "none"), std::runtime_error);
}

TEST(AppCamera, FramesBoundsAndMoves) {
    OrbitCamera camera;
    Bounds b;
    b.valid = true;
    b.lo = {-1.0f, 2.0f, -1.0f};
    b.hi = {1.0f, 4.0f, 1.0f};
    camera.frame(b);
    EXPECT_FLOAT_EQ(camera.target().y(), 3.0f);
    // The view puts the target straight ahead at the camera's distance.
    const QVector3D ahead = camera.view().map(camera.target());
    EXPECT_NEAR(ahead.x(), 0.0f, 1e-4f);
    EXPECT_NEAR(ahead.y(), 0.0f, 1e-4f);
    EXPECT_NEAR(ahead.z(), -camera.distance(), 1e-3f);
    // The bounds' corners project inside the clip volume.
    const QMatrix4x4 vp = camera.viewProjection(1.5f);
    for (int k = 0; k < 8; ++k) {
        const QVector3D corner((k & 1) ? b.hi.x : b.lo.x, (k & 2) ? b.hi.y : b.lo.y, (k & 4) ? b.hi.z : b.lo.z);
        const QVector4D clip = vp * QVector4D(corner, 1.0f);
        EXPECT_LE(std::abs(clip.x()), clip.w());
        EXPECT_LE(std::abs(clip.y()), clip.w());
    }
    const float distance = camera.distance();
    camera.dolly(0.5f);
    EXPECT_FLOAT_EQ(camera.distance(), distance * 0.5f);
    camera.orbit(90.0f, 200.0f);
    EXPECT_FLOAT_EQ(camera.pitch(), 89.0f);  // clamped short of the pole
    const QVector3D target = camera.target();
    camera.pan(100.0f, 0.0f, 500);
    EXPECT_NE(camera.target(), target);
    Bounds none;
    camera.frame(none);
    EXPECT_FLOAT_EQ(camera.distance(), 10.0f);
    // A close-up: the target and the distance set outright, the eye that far from it.
    camera.setTarget(QVector3D(1.0f, 2.0f, 3.0f));
    camera.setDistance(4.0f);
    EXPECT_NEAR((camera.eye() - camera.target()).length(), 4.0f, 1e-4f);
    EXPECT_FLOAT_EQ(camera.target().y(), 2.0f);
}

// ---- The tools --------------------------------------------------------------------------------

TEST(AppToolView, ProjectsAndCastsRaysAsTheCamera) {
    OrbitCamera camera;
    Bounds b;
    b.valid = true;
    b.lo = {-2.0f, 0.0f, -2.0f};
    b.hi = {2.0f, 3.0f, 2.0f};
    camera.frame(b);
    const int w = 640, h = 480;
    const ToolView view = toolViewFor(camera, w, h);
    ASSERT_TRUE(view.projection.valid());
    ASSERT_TRUE(view.hasCamera);
    EXPECT_FALSE(view.orthographic);
    // The core's eye is the camera's.
    const QVector3D eye = camera.eye();
    EXPECT_NEAR((view.eye - Vec3f{eye.x(), eye.y(), eye.z()}).length(), 0.0f, 1e-3f * camera.distance());
    // A point projects where Qt's matrix puts it, counted from the bottom left.
    const Vec3f p{0.7f, 1.3f, -0.4f};
    const QVector4D clip = camera.viewProjection(static_cast<float>(w) / h) * QVector4D(p.x, p.y, p.z, 1.0f);
    const float ex = (clip.x() / clip.w() + 1.0f) * 0.5f * w, ey = (clip.y() / clip.w() + 1.0f) * 0.5f * h;
    float x = 0.0f, y = 0.0f;
    ASSERT_TRUE(view.project(p, x, y));
    EXPECT_NEAR(x, ex, 0.01f);
    EXPECT_NEAR(y, ey, 0.01f);
    // A widget position (y down) is that pixel flipped.
    const std::array<float, 2> port = toPort(QPointF(ex, h - ey), h);
    EXPECT_NEAR(port[0], ex, 1e-4f);
    EXPECT_NEAR(port[1], ey, 1e-4f);
    // The ray under the pixel passes through the point.
    Vec3f origin, direction;
    ASSERT_TRUE(view.ray(x, y, origin, direction));
    const Vec3f toP = p - origin;
    EXPECT_NEAR((origin + direction * toP.dot(direction) - p).length(), 0.0f, 1e-3f);
    // A circle of r pixels at the target's depth spans r x the units per pixel there.
    const QVector3D t = camera.target();
    float tx = 0.0f, ty = 0.0f;
    ASSERT_TRUE(view.project({t.x(), t.y(), t.z()}, tx, ty));
    const float unitsPerPixel = 2.0f * camera.distance() * std::tan(camera.fovDegrees() * 3.14159265f / 360.0f) / h;
    EXPECT_NEAR(worldRadiusAt(view, {t.x(), t.y(), t.z()}, tx, ty, 60.0f), 60.0f * unitsPerPixel, 0.02f * 60.0f * unitsPerPixel);
    // The ray under a pixel meets the scalp where the brush starts: a plane at y = 0 under the target.
    const auto plane = parseObjMesh("v -5 0 -5\nv 5 0 -5\nv 5 0 5\nv -5 0 5\nf 1 2 3 4\n");
    SurfacePoint hit;
    ASSERT_TRUE(plane->intersectRay(origin, direction, &hit));
    EXPECT_NEAR(hit.position.y, 0.0f, 1e-4f);
    // No view without a size.
    EXPECT_FALSE(toolViewFor(camera, 0, 0).projection.valid());
}

// The sample groom with a brush session over it, the camera framing it in a view of w x h.
struct BrushFixture {
    std::unique_ptr<GroomDocument> doc;
    BrushSession session;
    OrbitCamera camera;
    const int w = 800, h = 600;
    explicit BrushFixture(const std::string& path = kSampleGroom) : doc(GroomDocument::open(path)) {
        ViewRequest request;
        request.density = 0.2f;
        doc->evaluate(request);
        camera.frame(doc->sceneBounds());
        session.setDocument(doc.get());
        session.setView(camera, w, h);
    }
    std::string guides() const { return doc->session().data().hierarchyJson; }
    // A guide point in widget pixels (y down).
    QPointF widgetPoint(const Vec3f& p) const {
        float x = 0.0f, y = 0.0f;
        EXPECT_TRUE(session.view().project(p, x, y));
        return QPointF(x, h - y);
    }
    const HierarchyGuide& guide(std::size_t index) const { return doc->session().hierarchy()->guides().at(index); }
};

TEST(AppBrush, StrokesMoveGuidesWithOneUndoStepEach) {
    BrushFixture f;
    const std::string before = f.guides();
    ASSERT_FALSE(before.empty());
    const std::size_t count = f.doc->session().hierarchy()->guides().size();
    ASSERT_GT(count, 0u);
    // A comb stroke across the first guide's middle, to the right.
    const HierarchyGuide& g = f.guide(0);
    const QPointF at = f.widgetPoint(g.points[g.points.size() / 2]);
    f.session.settings().setMode("comb");
    f.session.settings().screenRadius = 40.0f;
    std::vector<QPointF> path;
    for (int k = 0; k <= 8; ++k) path.emplace_back(at.x() + 10.0 * k, at.y());
    ASSERT_TRUE(f.session.press(path[0], 1.0f, 0)) << f.session.lastError();
    EXPECT_TRUE(f.session.stroking());
    EXPECT_TRUE(f.doc->session().hasPreview()) << "the dab previews through the session";
    for (std::size_t i = 1; i < path.size(); ++i) f.session.move(path[i], 1.0f);
    EXPECT_EQ(f.session.release(), StrokeRelease::Stroke);
    EXPECT_FALSE(f.session.stroking());
    EXPECT_FALSE(f.doc->session().hasPreview()) << "the release clears the preview";
    EXPECT_TRUE(f.session.lastError().empty()) << f.session.lastError();
    const std::string after = f.guides();
    EXPECT_NE(after, before) << "the guides moved";
    EXPECT_EQ(f.doc->session().hierarchy()->guides().size(), count);
    EXPECT_TRUE(f.doc->dirty());
    ASSERT_TRUE(f.doc->canUndo());
    EXPECT_EQ(f.doc->undoText(), "Undo comb stroke");
    // One undo step: back to the text as it was, bit for bit; redo gives the stroke's again.
    EXPECT_EQ(f.doc->undo(), "");
    EXPECT_FALSE(f.doc->canUndo()) << "the stroke was one step";
    EXPECT_EQ(f.guides(), before);
    EXPECT_EQ(f.doc->redo(), "");
    EXPECT_EQ(f.guides(), after);
    // A second stroke through the convenience, with Ctrl (lift lays guides down): another step.
    f.session.settings().setMode("lift");
    EXPECT_TRUE(f.session.stroke(path, 1.0f, kStrokeCtrl));
    EXPECT_NE(f.guides(), after);
    EXPECT_EQ(f.doc->undoText(), "Undo lift stroke");
    EXPECT_EQ(f.doc->undo(), "");
    EXPECT_EQ(f.guides(), after);
    EXPECT_EQ(f.doc->undo(), "");
    EXPECT_EQ(f.guides(), before);
    // A stroke off the scalp and the guides starts nothing and records nothing.
    EXPECT_FALSE(f.session.stroke({QPointF(2.0, 2.0), QPointF(30.0, 2.0)}));
    EXPECT_FALSE(f.doc->canUndo());
    // The parameter edits and the strokes share the history.
    const std::string length = nodeOfType(f.doc->nodes(), "Length").name;
    EXPECT_EQ(f.doc->setBypass(length, true), "");
    EXPECT_TRUE(f.session.stroke(path));
    EXPECT_EQ(f.doc->undoText(), "Undo lift stroke");
    EXPECT_EQ(f.doc->undo(), "");
    EXPECT_EQ(f.doc->undoText(), "Undo " + length + ".bypass");
    // Saved, the stroked guides come back.
    EXPECT_EQ(f.doc->redo(), "");
    const std::string stroked = f.guides();
    const std::string saved = tempPath("groomlab_app_stroked.groom");
    f.doc->save(saved);
    auto again = GroomDocument::open(saved);
    EXPECT_EQ(again->session().data().hierarchyJson, stroked);
    std::remove(saved.c_str());
}

TEST(AppBrush, HoverRingAndPressure) {
    BrushFixture f;
    const HierarchyGuide& g = f.guide(0);
    // Over the root (on the scalp; the guide's tip sticks out past the sphere's silhouette).
    const QPointF at = f.widgetPoint(g.points[0]);
    f.session.settings().screenRadius = 50.0f;
    f.session.hover(at);
    EXPECT_TRUE(f.session.ringValid());
    EXPECT_GT(f.session.ringRadius(), 0.0f);
    const std::uint64_t version = f.session.overlayVersion();
    // The ring (49 points) and the guides under the brush, colored lines over the view.
    const DisplayGeometry& overlay = f.session.overlay();
    EXPECT_TRUE(overlay.overlay);
    EXPECT_GT(overlay.positions.size() / 3, 49u);
    EXPECT_EQ(overlay.colors.size() / 4, overlay.positions.size() / 3);
    EXPECT_FALSE(overlay.indices.empty());
    // The ring sits on the scalp: its center is on the surface under the cursor, and its radius
    // is the screen radius in the world there.
    const SurfacePoint closest = f.doc->session().scalp()->closestPoint(f.session.ringCenter());
    EXPECT_LT((closest.position - f.session.ringCenter()).length(), 1e-3f * f.camera.distance());
    float x = 0.0f, y = 0.0f;
    f.session.view().project(f.session.ringCenter(), x, y);
    EXPECT_NEAR(f.session.ringRadius(), worldRadiusAt(f.session.view(), f.session.ringCenter(), x, y, 50.0f), 1e-5f);
    f.session.clearHover();
    EXPECT_NE(f.session.overlayVersion(), version);
    EXPECT_TRUE(f.session.overlay().empty());
    EXPECT_FALSE(f.session.ringValid());
    // Symmetry on: the scalp's line of symmetry joins the overlay in red (as Substance Painter's
    // center line), every point on the plane x = 0; off again, it goes.
    f.session.hover(at);
    const std::size_t plain = f.session.overlay().positions.size();
    f.session.settings().setSymmetry("x");
    f.session.hover(at);
    const DisplayGeometry& mirrored = f.session.overlay();
    ASSERT_GT(mirrored.positions.size(), plain + 3 * 20);
    std::size_t red = 0;
    for (std::size_t i = 0; i < mirrored.positions.size() / 3; ++i) {
        if (mirrored.colors[4 * i] != 1.0f || mirrored.colors[4 * i + 1] > 0.2f) continue;
        ++red;
        EXPECT_NEAR(mirrored.positions[3 * i], 0.0f, 0.02f * f.camera.distance());
    }
    EXPECT_GT(red, 20u);
    f.session.settings().setSymmetry("none");
    f.session.hover(at);
    EXPECT_EQ(f.session.overlay().positions.size(), plain);
    f.session.clearHover();
    // Off the scalp the ring sits at the target's depth, so it is always there to see.
    f.session.hover(QPointF(4.0, 4.0));
    EXPECT_TRUE(f.session.ringValid());
    // Half the pressure moves the guides less than the full one.
    const std::string before = f.guides();
    std::vector<QPointF> path;
    for (int k = 0; k <= 6; ++k) path.emplace_back(at.x() + 12.0 * k, at.y());
    f.session.settings().setMode("comb");
    ASSERT_TRUE(f.session.stroke(path, 1.0f));
    const std::vector<Vec3f> full = f.guide(0).points;
    f.doc->undo();
    ASSERT_TRUE(f.session.stroke(path, 0.3f));
    const std::vector<Vec3f> light = f.guide(0).points;
    f.doc->undo();
    EXPECT_EQ(f.guides(), before);
    const HierarchyGuide& g0 = f.guide(0);
    double movedFull = 0.0, movedLight = 0.0;
    for (std::size_t k = 0; k < g0.points.size(); ++k) {
        movedFull += (full[k] - g0.points[k]).length();
        movedLight += (light[k] - g0.points[k]).length();
    }
    EXPECT_GT(movedFull, 0.0);
    EXPECT_LT(movedLight, movedFull);
    EXPECT_GT(movedLight, 0.0);
    // The symmetric stroke moves guides on both sides (the sample's sphere is symmetric in x).
    f.session.settings().brush.mirrorAxis = 0;
    ASSERT_TRUE(f.session.stroke(path, 1.0f));
    const auto shown = f.doc->session().hierarchy();
    const HierarchyPtr original = GroomDocument::open(kSampleGroom)->session().hierarchy();
    std::size_t movedLeft = 0, movedRight = 0;
    for (std::size_t i = 0; i < shown->guides().size(); ++i) {
        const HierarchyGuide& a = shown->guides()[i];
        const HierarchyGuide* b = original->guide(a.id);
        ASSERT_TRUE(b);
        bool moved = false;
        for (std::size_t k = 0; k < a.points.size() && k < b->points.size(); ++k) moved |= (a.points[k] - b->points[k]).length() > 1e-5f;
        if (!moved) continue;
        if (a.points[0].x > 0.0f) ++movedRight;
        else ++movedLeft;
    }
    EXPECT_GT(movedLeft, 0u);
    EXPECT_GT(movedRight, 0u);
}

TEST(AppBrush, PlacesErasesSelectsAndDeletesGuides) {
    BrushFixture f;
    const std::size_t count = f.doc->session().hierarchy()->guides().size();
    // Place: a click on the scalp adds a guide (its group the brush's level's).
    const Vec3f onScalp = f.doc->session().scalp()->position(f.guide(0).root);
    const QPointF at = f.widgetPoint(onScalp + (onScalp - Vec3f{0.0f, 0.0f, 0.0f}).normalized() * 0.01f);
    f.session.settings().setMode("place");
    // The click: on the scalp, a little away from the guides (another root would be too close for Place Many only).
    ASSERT_TRUE(f.session.stroke({at}));
    EXPECT_EQ(f.doc->session().hierarchy()->guides().size(), count + 1);
    EXPECT_EQ(f.doc->undoText(), "Undo place stroke");
    // Draw: a guide shaped like the path.
    f.session.settings().setMode("draw");
    std::vector<QPointF> drawn;
    for (int k = 0; k <= 10; ++k) drawn.emplace_back(at.x() + 3.0 * k, at.y() - 12.0 * k);
    ASSERT_TRUE(f.session.stroke(drawn));
    EXPECT_EQ(f.doc->session().hierarchy()->guides().size(), count + 2);
    // Erase: the guides under the brush go.
    f.session.settings().setMode("erase");
    f.session.settings().screenRadius = 30.0f;
    ASSERT_TRUE(f.session.stroke({at, QPointF(at.x() + 2.0, at.y())}));
    EXPECT_LT(f.doc->session().hierarchy()->guides().size(), count + 2);
    const std::size_t afterErase = f.doc->session().hierarchy()->guides().size();
    EXPECT_EQ(f.doc->undo(), "");
    EXPECT_EQ(f.doc->session().hierarchy()->guides().size(), count + 2);
    EXPECT_EQ(f.doc->redo(), "");
    EXPECT_EQ(f.doc->session().hierarchy()->guides().size(), afterErase);

    // Select: a click near a guide picks it, a box picks what is inside, modes combine.
    f.session.settings().brush.level = -1;
    // The click at a guide's root no other guide passes near on screen (the default groom's
    // framing crosses guides within a pixel of some roots, and the pick measures whole pixels:
    // the nearest may then be another's).
    const auto& guides = f.doc->session().hierarchy()->guides();
    auto clear = [&](const HierarchyGuide& c) {
        const QPointF at = f.widgetPoint(c.points[0]);
        for (const HierarchyGuide& o : guides) {
            if (o.id == c.id) continue;
            for (const Vec3f& p : o.points) {
                const QPointF q = f.widgetPoint(p);
                if (std::hypot(q.x() - at.x(), q.y() - at.y()) < 4.0) return false;
            }
        }
        return true;
    };
    const auto clearGuide = std::find_if(guides.begin(), guides.end(), clear);
    ASSERT_NE(clearGuide, guides.end());
    const HierarchyGuide& g = *clearGuide;
    const QPointF root = f.widgetPoint(g.points[0]);
    EXPECT_EQ(f.session.select({root}, SelectMode::Replace), 1u);
    ASSERT_EQ(f.doc->selection().size(), 1u);
    EXPECT_EQ(f.doc->selection()[0], g.id);
    EXPECT_EQ(f.session.select({QPointF(0.0, 0.0), QPointF(f.w, f.h)}, SelectMode::Replace), afterErase);
    EXPECT_EQ(f.doc->selection().size(), afterErase);
    EXPECT_EQ(f.session.select({root}, SelectMode::Deselect), 1u);
    EXPECT_EQ(f.doc->selection().size(), afterErase - 1);
    EXPECT_EQ(f.session.select({root}, SelectMode::Toggle), 1u);
    EXPECT_EQ(f.doc->selection().size(), afterErase);
    // A guide brush's Shift+click: the guides inside its ring (none: the selection cleared).
    // Painting (Freeze), Shift smooths.
    f.session.settings().setMode("comb");
    EXPECT_TRUE(f.session.settings().shiftSelects());
    f.session.settings().screenRadius = 2000.0f;
    EXPECT_EQ(f.session.paintSelection(root, SelectMode::Replace), afterErase);
    f.session.settings().screenRadius = 5.0f;
    EXPECT_EQ(f.session.paintSelection(QPointF(1.0, 1.0), SelectMode::Replace), 0u);
    EXPECT_TRUE(f.doc->selection().empty());
    // The Select tool's lasso: the guides inside a polygon round the whole view.
    EXPECT_EQ(f.session.select({QPointF(0.0, 0.0), QPointF(f.w, 0.0), QPointF(f.w, f.h), QPointF(0.0, f.h)}, SelectMode::Add), afterErase);
    EXPECT_EQ(f.doc->selection().size(), afterErase);
    f.session.setTool("freeze");
    EXPECT_FALSE(f.session.settings().shiftSelects());
    f.session.setTool("comb");
    // The selection shows as the display's selected item.
    DisplaySettings settings;
    const DisplayFrame& frame = f.doc->updateDisplay(settings);
    EXPECT_TRUE(frame.item(DisplayItemId::Selected).shown);
    EXPECT_FALSE(frame.dots.points.empty());
    // Delete the selected guides: one step; undo brings them and the selection back... the
    // guides at least (the selection is not in the history).
    const std::vector<GuideId> selected = f.doc->selection();
    EXPECT_EQ(f.doc->editGuides("delete guides", [&](GuideHierarchy& h) { h.removeGuides(selected); }), "");
    EXPECT_EQ(f.doc->session().hierarchy()->guides().size(), 0u);
    EXPECT_EQ(f.doc->undoText(), "Undo delete guides");
    EXPECT_TRUE(f.doc->selection().empty()) << "deleted guides leave the selection";
    EXPECT_EQ(f.doc->undo(), "");
    EXPECT_EQ(f.doc->session().hierarchy()->guides().size(), afterErase);
    // A refused edit records nothing: the erase stroke stays on top, the delete still redoable.
    EXPECT_NE(f.doc->editGuides("bad", [](GuideHierarchy& h) { h.renameGroup(kNoGroup, ""); }), "");
    EXPECT_EQ(f.doc->undoText(), "Undo erase stroke");
    EXPECT_EQ(f.doc->redoText(), "Redo delete guides");
}

TEST(AppBrush, FreezeHoldsGuidesBackAndCtrlUnfreezes) {
    // The rail's Freeze (the catalog's "freeze", Maya's freeze_tool): the Paint brush on the
    // guides' freeze mask toward 1, Ctrl toward 0. A comb leaves the frozen guides as they are
    // and moves them once unfrozen; the display draws the frozen ones in the theme's color.
    BrushFixture f;
    BrushToolSettings& s = f.session.settings();
    s.pressure = false;
    s.brush.strength = 1.0f;
    s.brush.hardness = 1.0f;  // full strength to the brush's edge
    s.setMode("paint");
    s.brush.attribute = "density";  // what Paint paints: back once Freeze is left
    f.session.setTool("freeze");
    EXPECT_EQ(f.session.toolId(), "freeze");
    EXPECT_EQ(s.modeName(), "paint");
    EXPECT_EQ(s.brush.attribute, "freeze");
    EXPECT_EQ(s.brush.value, 1.0f);
    EXPECT_EQ(s.paintKind, "guides");
    EXPECT_THROW(f.session.setTool("no such tool"), std::invalid_argument);
    EXPECT_EQ(f.session.toolId(), "freeze") << "an unknown tool changes nothing";

    auto frozenValues = [&] {
        std::vector<float> out;
        const auto h = f.doc->session().hierarchy();
        for (const HierarchyGuide& g : h->guides()) {
            float v = 0.0f;
            h->effectiveAttribute(g, "freeze", &v);
            out.push_back(v);
        }
        return out;
    };
    auto points = [&] {
        std::vector<std::vector<Vec3f>> out;
        for (const HierarchyGuide& g : f.doc->session().hierarchy()->guides()) out.push_back(g.points);
        return out;
    };
    auto moved = [](const std::vector<Vec3f>& a, const std::vector<Vec3f>& b) {
        float most = 0.0f;
        for (std::size_t k = 0; k < std::min(a.size(), b.size()); ++k) most = std::max(most, (a[k] - b[k]).length());
        return a.size() != b.size() ? 1e9f : most;
    };
    // A short stroke over the first guide's middle (widget pixels).
    const HierarchyGuide& g0 = f.guide(0);
    const QPointF at = f.widgetPoint(g0.points[g0.points.size() / 2]);
    auto strokeAt = [&](float radius, double reach, unsigned modifiers) {
        s.screenRadius = radius;
        std::vector<QPointF> path;
        for (int k = 0; k <= 6; ++k) path.emplace_back(at.x() + reach * k / 6.0, at.y());
        return f.session.stroke(path, 1.0f, modifiers);
    };

    // Freeze the guides around it: some, not all.
    ASSERT_TRUE(strokeAt(50.0f, 6.0, 0)) << f.session.lastError();
    const std::vector<float> frozen = frozenValues();
    const std::size_t count = frozen.size();
    std::vector<std::size_t> held;
    for (std::size_t i = 0; i < count; ++i) {
        if (frozen[i] >= 0.999f) held.push_back(i);
    }
    ASSERT_FALSE(held.empty()) << "the stroke froze the guides under it";
    ASSERT_LT(held.size(), count) << "and only those";
    // Drawn in the theme's frozen color.
    ViewRequest request;
    request.density = 0.2f;
    f.doc->evaluate(request);
    DisplaySettings settings;
    const DisplayFrame& frame = f.doc->updateDisplay(settings);
    const auto& guideLines = frame.item(DisplayItemId::Guides).geometry;
    ASSERT_TRUE(guideLines);
    std::size_t blue = 0;
    for (std::size_t k = 0; k + 3 < guideLines->colors.size(); k += 4) {
        const float* c = guideLines->colors.data() + k;
        if (std::abs(c[0] - settings.theme.frozen[0]) < 1e-4f && std::abs(c[1] - settings.theme.frozen[1]) < 1e-4f &&
            std::abs(c[2] - settings.theme.frozen[2]) < 1e-4f) {
            ++blue;
        }
    }
    EXPECT_GT(blue, 0u) << "frozen guides draw in the theme's frozen color";

    // Comb over them and the guides around: the frozen ones stay, others move.
    f.session.setTool("comb");
    EXPECT_EQ(f.session.toolId(), "comb");
    EXPECT_EQ(s.brush.attribute, "density") << "Paint's map came back";
    const auto beforeComb = points();
    ASSERT_TRUE(strokeAt(250.0f, 120.0, 0)) << f.session.lastError();
    const auto combed = points();
    ASSERT_EQ(combed.size(), count);
    std::size_t movedCount = 0;
    for (std::size_t i = 0; i < count; ++i) {
        if (moved(beforeComb[i], combed[i]) > 1e-4f) ++movedCount;
    }
    for (std::size_t i : held) EXPECT_LT(moved(beforeComb[i], combed[i]), 1e-5f) << "frozen guide " << i << " moved";
    EXPECT_GT(movedCount, 0u) << "the comb moved the guides that are not frozen";
    EXPECT_EQ(f.doc->undo(), "");

    // Ctrl unfreezes (toward 0); the comb then moves the guides that were frozen.
    f.session.setTool("freeze");
    ASSERT_TRUE(strokeAt(50.0f, 6.0, kStrokeCtrl)) << f.session.lastError();
    const std::vector<float> thawed = frozenValues();
    for (std::size_t i : held) EXPECT_LT(thawed[i], 0.5f) << "guide " << i << " still frozen";
    f.session.setTool("comb");
    const auto beforeThawed = points();
    ASSERT_TRUE(strokeAt(250.0f, 120.0, 0)) << f.session.lastError();
    const auto combedThawed = points();
    std::size_t thawedMoved = 0;
    for (std::size_t i : held) {
        if (moved(beforeThawed[i], combedThawed[i]) > 1e-4f) ++thawedMoved;
    }
    EXPECT_GT(thawedMoved, 0u) << "unfrozen, they comb";
}

TEST(AppDocument, NodeRenameDuplicateOutputAndTidy) {
    // The node editors' other edits through the core's GraphEdit, one undo step each.
    auto doc = GroomDocument::open(kSampleGroom);
    ASSERT_TRUE(doc);
    const std::string length = nodeOfType(doc->nodes(), "Length").name;
    EXPECT_EQ(doc->renameNode(length, "trim"), "");
    bool found = false, fed = false;
    for (const GroomDocument::NodeInfo& n : doc->nodes()) found = found || n.name == "trim";
    for (const GroomDocument::ConnectionInfo& c : doc->connections()) fed = fed || c.to == "trim" || c.from == "trim";
    EXPECT_TRUE(found);
    EXPECT_TRUE(fed) << "the connections follow the name";
    EXPECT_EQ(doc->undoText(), "Undo rename " + length);
    EXPECT_NE(doc->renameNode("trim", nodeOfType(doc->nodes(), "Scatter").name), "") << "a name taken";
    EXPECT_NE(doc->renameNode("nope", "x"), "");
    std::string copy;
    EXPECT_EQ(doc->duplicateNode("trim", &copy), "");
    EXPECT_EQ(copy, "length");  // the type's name, free again
    float x = 0.0f, y = 0.0f, cx = 0.0f, cy = 0.0f;
    if (doc->nodePosition("trim", &x, &y)) {
        ASSERT_TRUE(doc->nodePosition(copy, &cx, &cy));
        EXPECT_GT(cx, x);
    }
    EXPECT_EQ(doc->paramOrDefault(copy, "keep"), doc->paramOrDefault("trim", "keep")) << "the params copied";
    EXPECT_NE(doc->duplicateNode(nodeOfType(doc->nodes(), "Output").name, &copy), "");
    const std::string output = doc->graph().value("output", std::string());
    EXPECT_EQ(doc->setOutputNode("trim"), "");
    EXPECT_EQ(doc->graph().value("output", std::string()), "trim");
    EXPECT_EQ(doc->undo(), "");
    EXPECT_EQ(doc->graph().value("output", std::string()), output);
    EXPECT_EQ(doc->tidyLayout(), "");
    EXPECT_FALSE(doc->nodePosition("trim", &x, &y)) << "every position dropped";
    EXPECT_EQ(doc->undo(), "");
}

TEST(AppDocument, SculptEditsFollowTheirNodeIntoAndOutOfACompound) {
    // A Sculpt node's edits are kept by node path outside the graph ("compound/sculpt" inside a
    // compound): Group, a rename of the compound, a copy of it and Ungroup take them along in the
    // same undo step (Group dropped them).
    auto doc = GroomDocument::open(kSampleGroom);
    ASSERT_TRUE(doc);
    std::string sculpt;
    ASSERT_EQ(doc->addNode("Sculpt", 0.0f, 0.0f, &sculpt), "");
    SculptSet set;
    set.nodes[sculpt].guides[7].samples = {{0.0f, 0.1f, 0.0f, 1.0f}, {1.0f, 0.2f, 0.0f, 1.0f}};
    ASSERT_EQ(doc->editStoredText("sculpt edits", StrokeField::Sculpts, [&](std::string& text) { text = set.toJson(); }), "");
    ASSERT_EQ(doc->sculptedGuides(sculpt), 1u);
    std::string compound;
    ASSERT_EQ(doc->collapseNodes({sculpt}, &compound), "");
    EXPECT_EQ(doc->sculptedGuides(compound + "/" + sculpt), 1u);
    EXPECT_EQ(doc->sculptedGuides(sculpt), 0u);
    EXPECT_EQ(doc->undo(), "");
    EXPECT_EQ(doc->sculptedGuides(sculpt), 1u) << "one undo step: the graph and the edits";
    EXPECT_EQ(doc->redo(), "");
    EXPECT_EQ(doc->sculptedGuides(compound + "/" + sculpt), 1u);
    ASSERT_EQ(doc->renameNode(compound, "shaping"), "");
    EXPECT_EQ(doc->sculptedGuides("shaping/" + sculpt), 1u);
    std::string copy;
    ASSERT_EQ(doc->duplicateNode("shaping", &copy), "");
    EXPECT_EQ(doc->sculptedGuides(copy + "/" + sculpt), 1u) << "a compound's copy takes its inner edits";
    std::vector<std::string> made;
    ASSERT_EQ(doc->expandNodes({"shaping"}, &made), "");
    ASSERT_EQ(made, std::vector<std::string>{sculpt});
    EXPECT_EQ(doc->sculptedGuides(sculpt), 1u);
    EXPECT_EQ(doc->sculptedGuides("shaping/" + sculpt), 0u);
}

TEST(AppDocument, StoredTextsEditAsOneStepAndTransferMovesTheGroom) {
    auto doc = GroomDocument::open(kSampleGroom);
    ASSERT_TRUE(doc);
    ViewRequest request;
    request.density = 0.2f;
    doc->evaluate(request);
    // A part and some flow laid as the brushes store them, then removed through the stored
    // texts: one undo step each, the session following.
    const MeshPtr scalp = doc->file().data.scalp;
    ASSERT_TRUE(scalp);
    EXPECT_EQ(doc->storedText(StrokeField::Parts), "");
    EXPECT_EQ(doc->editStoredText("add part", StrokeField::Parts, [&](std::string& text) {
        PartSet parts = PartSet::fromJson(text);
        std::vector<Vec3f> line;
        const Bounds b = doc->sceneBounds();
        for (int k = 0; k <= 8; ++k) line.push_back({b.lo.x + (b.hi.x - b.lo.x) * k / 8.0f, b.hi.y, (b.lo.z + b.hi.z) * 0.5f});
        parts.add("middle", line, *scalp);
        text = parts.toJson();
    }), "");
    EXPECT_EQ(doc->undoText(), "Undo add part stroke");
    EXPECT_EQ(PartSet::fromJson(doc->storedText(StrokeField::Parts)).parts.size(), 1u);
    EXPECT_EQ(doc->editStoredText("clear parts", StrokeField::Parts, [](std::string& text) { text.clear(); }), "");
    EXPECT_EQ(doc->storedText(StrokeField::Parts), "");
    EXPECT_EQ(doc->undo(), "");
    EXPECT_EQ(PartSet::fromJson(doc->storedText(StrokeField::Parts)).parts.size(), 1u);
    EXPECT_NE(doc->editStoredText("bad", StrokeField::Flow, [](std::string&) { throw std::invalid_argument("no"); }), "");
    EXPECT_EQ(doc->editStoredText("same", StrokeField::Flow, [](std::string&) {}), "") << "unchanged: nothing recorded";
    // A corrective layer through the same door.
    EXPECT_EQ(doc->editStoredText("add corrective", StrokeField::Correctives, [](std::string& text) {
        Correctives c = Correctives::fromJson(text);
        CorrectiveLayer layer;
        layer.name = "fix";
        layer.holdBefore = layer.holdAfter = true;
        c.layers.push_back(layer);
        text = c.toJson();
    }), "");
    EXPECT_EQ(Correctives::fromJson(doc->storedText(StrokeField::Correctives)).layers.size(), 1u);

    // The groom onto a copy of its scalp elsewhere and twice as big (the bounding boxes fitted):
    // every guide re-rooted there, the part with them, a vertex map projected; the history gone.
    const HierarchyPtr before = doc->session().authoredHierarchy();
    ASSERT_TRUE(before);
    const std::size_t guides = before->guides().size();
    EXPECT_EQ(doc->createMap("mask", "vertices", false, 0.25f), "");
    std::vector<Vec3f> points = scalp->points();
    for (Vec3f& p : points) p = p * 2.0f + Vec3f{100.0f, 0.0f, 0.0f};
    auto bigger = std::make_shared<ScalpMesh>(points, scalp->triangles());
    if (scalp->hasUVs()) bigger->setCornerUVs(scalp->cornerUVs());
    std::vector<std::string> warnings;
    EXPECT_EQ(doc->transferToScalp(bigger, true, true, &warnings), "");
    EXPECT_FALSE(doc->canUndo());
    EXPECT_TRUE(doc->dirty());
    EXPECT_EQ(doc->file().data.scalp->pointCount(), bigger->pointCount());
    const HierarchyPtr after = doc->session().authoredHierarchy();
    ASSERT_TRUE(after);
    EXPECT_EQ(after->guides().size(), guides);
    Bounds roots;
    for (const HierarchyGuide& g : after->guides()) {
        if (g.points.empty()) continue;
        const Vec3f& p = g.points.front();
        if (!roots.valid) {
            roots.lo = roots.hi = p;
            roots.valid = true;
        } else {
            roots.lo = {std::min(roots.lo.x, p.x), std::min(roots.lo.y, p.y), std::min(roots.lo.z, p.z)};
            roots.hi = {std::max(roots.hi.x, p.x), std::max(roots.hi.y, p.y), std::max(roots.hi.z, p.z)};
        }
    }
    ASSERT_TRUE(roots.valid);
    EXPECT_GT(roots.lo.x, 50.0f) << "the roots moved with the scalp";
    EXPECT_EQ(PartSet::fromJson(doc->storedText(StrokeField::Parts)).parts.size(), 1u);
    ASSERT_TRUE(doc->map("mask"));
    EXPECT_EQ(doc->session().scalpMaps().maps.at("mask").values.size(), bigger->pointCount());
    EXPECT_NEAR(doc->session().scalpMaps().maps.at("mask").values[0], 0.25f, 1e-3f);
    doc->evaluate(request);
    EXPECT_TRUE(doc->lastView().errors.empty());
    EXPECT_NE(doc->transferToScalp(nullptr, true, true), "");
}

TEST(AppDocument, GuideGroupsAndChildren) {
    auto doc = GroomDocument::open(kSampleGroom);
    ViewRequest request;
    request.density = 0.2f;
    doc->evaluate(request);
    const HierarchyPtr h0 = doc->session().authoredHierarchy();
    ASSERT_TRUE(h0);
    const std::size_t groups = h0->groups().size(), guides = h0->guides().size();
    ASSERT_GT(groups, 0u);
    const std::string primary = h0->groups()[0].name;
    // A new root group, renamed, hidden (not drawn), then children generated into a group under
    // the primaries; each edit one step.
    EXPECT_EQ(doc->editGuides("add group", [](GuideHierarchy& h) { h.addGroup("extra"); }), "");
    EXPECT_EQ(doc->session().authoredHierarchy()->groups().size(), groups + 1);
    EXPECT_EQ(doc->editGuides("rename group", [](GuideHierarchy& h) { h.renameGroup(h.findGroup("extra"), "spare"); }), "");
    EXPECT_NE(doc->session().authoredHierarchy()->findGroup("spare"), kNoGroup);
    doc->setGroupHidden("spare", true);
    doc->setGroupHidden(primary, true);
    EXPECT_EQ(doc->hiddenGroupsText(), primary + ",spare");
    DisplaySettings settings;
    settings.hiddenGuideGroups = doc->hiddenGroupsText();
    doc->setGroupHidden(primary, false);
    EXPECT_EQ(doc->hiddenGroupsText(), "spare");
    EXPECT_EQ(doc->editGuides("generate children", [&](GuideHierarchy& h) {
        ChildGenerationParams p;
        p.count = 3;
        h.generateChildren(h.findGroup(primary), "kids", p);
    }), "");
    const HierarchyPtr h1 = doc->session().authoredHierarchy();
    EXPECT_EQ(h1->guides().size(), guides + 3 * h0->guidesInGroup(h0->findGroup(primary)).size());
    for (const HierarchyGuide& g : h1->guides()) {
        if (g.group == h1->findGroup("kids")) EXPECT_GE(g.points.size(), 2u) << "children took their shapes";
    }
    EXPECT_EQ(doc->undoText(), "Undo generate children");
    EXPECT_EQ(doc->undo(), "");
    EXPECT_EQ(doc->session().authoredHierarchy()->guides().size(), guides);
    EXPECT_EQ(doc->undo(), "");
    EXPECT_EQ(doc->undo(), "");
    EXPECT_FALSE(doc->canUndo());
    EXPECT_EQ(doc->session().authoredHierarchy()->groups().size(), groups);
    EXPECT_EQ(doc->redo(), "");
    EXPECT_EQ(doc->redo(), "");
    EXPECT_EQ(doc->redo(), "");
    EXPECT_EQ(doc->session().authoredHierarchy()->guides().size(), h1->guides().size());
    // The evaluation and the display follow (the children interpolate too).
    const ViewResult& view = doc->evaluate(request);
    EXPECT_TRUE(view.errors.empty());
    EXPECT_EQ(view.hierarchy->guides().size(), h1->guides().size());
}

// Pixels that differ from `background` by more than a little.
int pixelsNot(const QImage& image, const QColor& background, int tolerance = 12) {
    int drawn = 0;
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            const QColor c = image.pixelColor(x, y);
            if (std::abs(c.red() - background.red()) + std::abs(c.green() - background.green()) +
                    std::abs(c.blue() - background.blue()) > tolerance) {
                ++drawn;
            }
        }
    }
    return drawn;
}

// The mean absolute difference per channel between two images of one size.
double meanDifference(const QImage& a, const QImage& b) {
    double sum = 0.0;
    for (int y = 0; y < a.height(); ++y) {
        for (int x = 0; x < a.width(); ++x) {
            const QColor ca = a.pixelColor(x, y), cb = b.pixelColor(x, y);
            sum += std::abs(ca.red() - cb.red()) + std::abs(ca.green() - cb.green()) + std::abs(ca.blue() - cb.blue());
        }
    }
    return sum / (3.0 * a.width() * a.height());
}

TEST(AppLook, MaterialNumbersFollowArnold) {
    // Melanin absorbs more the more of it, the blue most; redness shifts toward pheomelanin.
    const RGB dark = hairMelaninAbsorption(1.0f, 0.0f), light = hairMelaninAbsorption(0.1f, 0.0f);
    EXPECT_GT(dark[2], dark[1]);
    EXPECT_GT(dark[1], dark[0]);
    for (int k = 0; k < 3; ++k) EXPECT_GT(dark[k], light[k]);
    const RGB none = hairMelaninAbsorption(0.0f, 0.5f);
    for (float v : none) EXPECT_FLOAT_EQ(v, 0.0f);
    // Rougher: wider lobes.
    EXPECT_LT(hairLongitudinalWidth(0.1f), hairLongitudinalWidth(0.5f));
    EXPECT_LT(hairAzimuthalSpread(0.1f), hairAzimuthalSpread(0.6f));
    // The presets by name, "groom" keeping the groom's pigment.
    for (const std::string& name : hairPresetNames()) {
        const HairMaterial m = hairPreset(name);
        EXPECT_EQ(m.presetName, name);
        EXPECT_EQ(m.pigment == HairMaterial::Pigment::GroomMelanin, name == "groom");
    }
    EXPECT_GT(hairPreset("black").melanin, hairPreset("blonde").melanin);
    EXPECT_TRUE(hairPreset("no such").presetName.empty());
    // The groom's HairColor look as create_hair_shader maps it.
    HairMaterial m;
    applyGroomLook(m, 0.15f, 0.5f);
    EXPECT_FLOAT_EQ(m.specularTint[0], 0.5f);
    EXPECT_FLOAT_EQ(m.roughness, 0.3f);
    EXPECT_FLOAT_EQ(m.shift, kHairShiftDegrees) << "the default tilt: Arnold's shift";
    applyGroomLook(m, 0.3f, 0.5f, 6.0f);
    EXPECT_FLOAT_EQ(m.shift, 6.0f) << "HairColor's tilt is the lobes' shift";
    for (const std::string& name : lightingPresetNames()) EXPECT_EQ(lightingPreset(name).presetName, name);
    EXPECT_FALSE(lightingPreset("studio").lights.empty());
    // A light's direction: azimuth 0 is +z, elevation 90 straight up.
    const auto up = lightDirection(0.0f, 90.0f);
    EXPECT_NEAR(up[1], 1.0f, 1e-5f);
    const auto front = lightDirection(0.0f, 0.0f);
    EXPECT_NEAR(front[2], 1.0f, 1e-5f);
    const auto turned = rotateY(front, 90.0f);
    EXPECT_NEAR(turned[0], 1.0f, 1e-5f);
}

TEST(AppLook, EnvironmentIsPrefilteredConsistently) {
    // An overcast sky: the irradiance from above exceeds the one from below, and the SH
    // irradiance straight up is close to the map's cosine-weighted integral.
    const EnvironmentMap map = makeEnvironment(Environment::Overcast, 128, 64);
    ASSERT_EQ(map.rgb.size(), static_cast<std::size_t>(128 * 64 * 3));
    const auto sh = environmentSH(map);
    const RGB above = irradianceSH(sh, {0.0f, 1.0f, 0.0f}), below = irradianceSH(sh, {0.0f, -1.0f, 0.0f});
    EXPECT_GT(above[1], below[1]);
    double integral = 0.0;
    const float pi = 3.14159265f;
    for (int y = 0; y < map.height; ++y) {
        const float theta = (y + 0.5f) / map.height * pi;
        const float solidAngle = (2.0f * pi / map.width) * (pi / map.height) * std::sin(theta);
        for (int x = 0; x < map.width; ++x) {
            const auto d = EnvironmentMap::direction((x + 0.5f) / map.width, (y + 0.5f) / map.height);
            integral += map.at(x, y)[1] * std::max(0.0f, d[1]) * solidAngle;
        }
    }
    EXPECT_NEAR(above[1], integral, 0.08 * integral);
    // The lights standing in for the map: as many as asked, unit directions, their summed
    // intensity near the map's total.
    const auto lights = environmentLights(map, 8);
    EXPECT_EQ(lights.size(), 8u);
    double total = 0.0, mapTotal = 0.0;
    for (const DirectionalLight& l : lights) {
        const float n = std::sqrt(l.direction[0] * l.direction[0] + l.direction[1] * l.direction[1] + l.direction[2] * l.direction[2]);
        EXPECT_NEAR(n, 1.0f, 1e-4f);
        total += l.intensity;
    }
    for (int y = 0; y < map.height; ++y) {
        const float theta = (y + 0.5f) / map.height * pi;
        const float solidAngle = (2.0f * pi / map.width) * (pi / map.height) * std::sin(theta);
        for (int x = 0; x < map.width; ++x) {
            const RGB c = map.at(x, y);
            mapTotal += (0.2126f * c[0] + 0.7152f * c[1] + 0.0722f * c[2]) * solidAngle;
        }
    }
    EXPECT_NEAR(total, mapTotal, 0.05 * mapTotal);
    // A sky's sun gets a light of its own, close to the sun's direction.
    const EnvironmentMap sky = makeEnvironment(Environment::Sky, 128, 64);
    const auto skyLights = environmentLights(sky, 8);
    const auto sun = lightDirection(-40.0f, 50.0f);
    float best = -1.0f;
    for (const DirectionalLight& l : skyLights) {
        best = std::max(best, l.direction[0] * sun[0] + l.direction[1] * sun[1] + l.direction[2] * sun[2]);
    }
    EXPECT_GT(best, 0.9f);
}

TEST(AppLook, HairBuffersFollowTheStrands) {
    StrandSet strands;
    strands.addStrand(7, {{0.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, {0.0f, 2.0f, 0.0f}}, {0.02f, 0.02f, 0.01f});
    strands.addStrand(8, {{1.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 1.0f}});
    strands.addStrand(9, {{2.0f, 0.0f, 0.0f}});  // one point: nothing to draw
    strands.addStrandChannel("melanin", 0.7f);
    const HairBuffers b = buildHairBuffers(strands, nullptr);
    EXPECT_EQ(b.pointCount, 6u);
    EXPECT_EQ(b.strandCount, 3u);
    // Padded to a whole row of the 4096-wide textures (two texels a point, three a strand).
    ASSERT_EQ(b.points.size(), 4096u * 4);
    ASSERT_EQ(b.strands.size(), 4096u * 4);
    EXPECT_EQ(b.points[6 * 8], 0.0f) << "the padding is zero";
    // The first strand: up the y axis, its points strand 0, its widths; the second strand's
    // points say strand 1, and its texels hold its first point and count, and (no scalp) its
    // first segment as the root normal.
    EXPECT_FLOAT_EQ(b.points[4 + 1], 1.0f);
    EXPECT_FLOAT_EQ(b.points[3], 0.0f);
    EXPECT_FLOAT_EQ(b.points[16 + 7], 0.01f);
    EXPECT_FLOAT_EQ(b.points[4 * 8 + 3], 1.0f);
    EXPECT_FLOAT_EQ(b.strands[0], 0.7f);
    EXPECT_FLOAT_EQ(b.strands[3], 0.0f);
    EXPECT_FLOAT_EQ(b.strands[7], 3.0f);
    EXPECT_FLOAT_EQ(b.strands[9], 1.0f);
    EXPECT_FLOAT_EQ(b.strands[12 + 1], kDefaultMelaninRedness);
    EXPECT_FLOAT_EQ(b.strands[12 + 3], 3.0f);
    EXPECT_FLOAT_EQ(b.strands[12 + 7], 2.0f);
    EXPECT_FLOAT_EQ(b.strands[12 + 10], 1.0f);
    // The strip: 2n + 1 indices a strand of n points (the restart), none for the lone point.
    ASSERT_EQ(b.strip.size(), 7u + 5u);
    EXPECT_EQ(b.strip[6], 0xFFFFFFFFu);
    EXPECT_EQ(b.strip[7], 6u);  // the second strand's first vertex (point 3, side 0)
    EXPECT_EQ(b.list.size(), 12u + 6u);
    EXPECT_TRUE(b.bounds.valid);
    EXPECT_FLOAT_EQ(b.bounds.hi.y, 2.0f);
}

TEST(AppLook, PointColorsRideAlongWhereTheStrandsHaveThem) {
    // HairColor's per-point colors (bands, a feather pattern, a tip) and its iridescence: a texel
    // a point (r g b iridescence); a groom without either has none (each strand's color then).
    StrandSet strands;
    strands.addStrand(7, {{0.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, {0.0f, 2.0f, 0.0f}});
    strands.addStrand(8, {{1.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 1.0f}});
    for (const char* name : kColorChannels) strands.addStrandChannel(name, 0.25f);
    const HairBuffers plain = buildHairBuffers(strands, nullptr);
    EXPECT_FALSE(plain.pointColored);
    EXPECT_TRUE(plain.pointColors.empty());
    EXPECT_FALSE(hairPointColored(strands));
    std::vector<float>& iridescence = strands.addPointChannel(kIridescenceChannel, 0.0f);
    iridescence[1] = 0.6f;
    HairBuffers b = buildHairBuffers(strands, nullptr);
    ASSERT_TRUE(b.pointColored);
    ASSERT_EQ(b.pointColors.size(), 4096u * 4);
    EXPECT_FLOAT_EQ(b.pointColors[0], 0.25f);       // the strand's color (none per point)
    EXPECT_FLOAT_EQ(b.pointColors[4 + 3], 0.6f);    // the second point's iridescence
    std::vector<float>& red = strands.addPointChannel(kColorChannels[0], 0.0f);
    strands.addPointChannel(kColorChannels[1], 0.0f);
    strands.addPointChannel(kColorChannels[2], 0.0f);
    red[4] = 0.9f;
    b = buildHairBuffers(strands, nullptr);
    EXPECT_FLOAT_EQ(b.pointColors[4 * 4], 0.9f);    // the last point's own red
    EXPECT_FLOAT_EQ(b.pointColors[4 * 4 + 1], 0.0f);
    // A range (a partial upload) writes the same.
    std::vector<float> points, strandData, colors;
    buildHairRange(strands, nullptr, 1, 2, points, strandData, nullptr, &colors);
    ASSERT_EQ(colors.size(), 2u * 4);
    EXPECT_FLOAT_EQ(colors[1 * 4], 0.9f);
}

TEST(AppRender, IridescenceTurnsTheHairToItsStructuralColor) {
    // The sample groom with every point iridescent toward blue, on both shading paths (the
    // interactive frame's per-point shading, the refined frames' per fragment): bluer than without.
    std::unique_ptr<QRhi> rhi = createOffscreenRhi();
    ASSERT_TRUE(rhi);
    auto doc = GroomDocument::open(kSampleGroom);
    ViewRequest request;
    request.density = 0.5f;
    const ViewResult& view = doc->evaluate(request);
    ASSERT_TRUE(view.strands);
    DisplaySettings settings;
    auto iridescent = std::make_shared<StrandSet>(*view.strands);
    iridescent->addPointChannel(kIridescenceChannel, 1.0f);
    OrbitCamera camera;
    camera.frame(doc->sceneBounds());
    const QSize size(320, 240);
    auto render = [&](const StrandsPtr& strands, Quality quality, std::uint64_t version) {
        GroomRenderer renderer;
        renderer.setFrame(doc->updateDisplay(settings));
        renderer.setScalp(doc->file().data.scalp);
        renderer.setHairLook(doc->session().hairLook());
        renderer.setHairStrands(strands, version);
        renderer.setLookdev(true);
        HairMaterial material = hairPreset("brown");
        material.iridescenceColor = {0.05f, 0.1f, 0.9f};
        material.iridescenceShift = 0.0f;
        renderer.setMaterial(material);
        LightingSetup lighting = lightingPreset("studio");
        lighting.quality = quality;
        renderer.setLighting(lighting);
        const OffscreenResult r = renderOffscreen(renderer, camera, size, quality == Quality::Refine ? 4 : 1, rhi.get());
        renderer.releaseResources();
        return r.image;
    };
    if (rhi->backend() == QRhi::Null) GTEST_SKIP() << "no GPU device: the pixels are not checked";
    // The share of blue in the lit pixels.
    auto blueness = [](const QImage& image) {
        double blue = 0.0, all = 0.0;
        for (int y = 0; y < image.height(); ++y) {
            for (int x = 0; x < image.width(); ++x) {
                const QColor c = image.pixelColor(x, y);
                if (c.value() < 40) continue;
                blue += c.blueF();
                all += c.redF() + c.greenF() + c.blueF();
            }
        }
        return all > 0.0 ? blue / all : 0.0;
    };
    for (const Quality quality : {Quality::Interactive, Quality::Refine}) {
        const QImage plainImage = render(view.strands, quality, view.versions.strands);
        const QImage blueImage = render(iridescent, quality, view.versions.strands + 1000);
        ASSERT_FALSE(plainImage.isNull());
        ASSERT_FALSE(blueImage.isNull());
        EXPECT_GT(blueness(blueImage), blueness(plainImage) + 0.1) << (quality == Quality::Refine ? "refined" : "interactive");
        EXPECT_TRUE(blueImage.save(QDir::temp().filePath(quality == Quality::Refine ? QStringLiteral("groomlab_app_iridescent_refined.png")
                                                                                   : QStringLiteral("groomlab_app_iridescent.png"))));
    }
}

TEST(AppRender, PickedColorsRenderAsArnoldsBaseColor) {
    // A color picked as RGB (HairColor's mode color: the groom's RGB, Pigment::GroomColor) shades as
    // Arnold's aiStandardHair renders its base color (hair_bsdf.glsl hairColorSigma: Chiang et al.'s
    // color to absorption), not as one pass's transmittance (which the lobes square and more: a mid
    // grey drew a tenth as bright as Arnold, a skin tone dark red). Against the melanin 0.35 brown:
    // Arnold renders the head groom's picked 0.5 grey 7.0 times as bright, in the same pixels under
    // the key light (the app 11.6 in that view, about 22 in this one; 0.7 before), and a skin tone
    // with white tips 1.06 times the grey's red over blue (the app 1.05, 2.7 before; here, without
    // tips, 1.29, 7.9 before).
    std::unique_ptr<QRhi> rhi = createOffscreenRhi();
    ASSERT_TRUE(rhi);
    // The head groom of the Arnold comparisons (tests/data/app: head_scalp.obj, head_graph.json).
    const std::string data = std::string(GROOMLAB_TEST_DATA) + "/app/";
    auto doc = GroomDocument::newOnMesh(readObjMesh(data + "head_scalp.obj"), "head_scalp.obj");
    std::ifstream graphFile(data + "head_graph.json");
    const std::string graph((std::istreambuf_iterator<char>(graphFile)), std::istreambuf_iterator<char>());
    ASSERT_FALSE(graph.empty());
    doc->replaceGraph(graph);
    ViewRequest request;
    request.density = 0.5f;
    const ViewResult& view = doc->evaluate(request);
    ASSERT_TRUE(view.strands);
    DisplaySettings settings;
    OrbitCamera camera;
    camera.frame(doc->sceneBounds());
    const QSize size(320, 240);
    auto colored = [&](std::array<float, 3> rgb) {
        auto strands = std::make_shared<StrandSet>(*view.strands);
        for (int k = 0; k < 3; ++k) {
            std::vector<float>& channel = strands->addStrandChannel(kColorChannels[k], 0.0f);
            std::fill(channel.begin(), channel.end(), rgb[static_cast<std::size_t>(k)]);
        }
        return strands;
    };
    auto melanin = std::make_shared<StrandSet>(*view.strands);
    for (const auto& [name, value] : {std::pair<const char*, float>{kMelaninChannel, 0.35f}, {kMelaninRednessChannel, 0.2f}}) {
        std::vector<float>& channel = melanin->addStrandChannel(name, 0.0f);
        std::fill(channel.begin(), channel.end(), value);
    }
    std::uint64_t version = view.versions.strands + 2000;
    auto render = [&](const StrandsPtr& strands, HairMaterial::Pigment pigment) {
        GroomRenderer renderer;
        renderer.setFrame(doc->updateDisplay(settings));
        renderer.setScalp(doc->file().data.scalp);
        renderer.setHairLook(doc->session().hairLook());
        renderer.setHairStrands(strands, ++version);
        renderer.setLookdev(true);
        HairMaterial material = hairPreset("groom");
        material.pigment = pigment;
        renderer.setMaterial(material);
        LightingSetup lighting = lightingPreset("key");
        lighting.quality = Quality::Refine;
        renderer.setLighting(lighting);
        const OffscreenResult r = renderOffscreen(renderer, camera, size, 4, rhi.get());
        renderer.releaseResources();
        return r.image;
    };
    if (rhi->backend() == QRhi::Null) GTEST_SKIP() << "no GPU device: the pixels are not checked";
    // Where the hair is: the pixels a picked grey's hair changes from a picked black's.
    const QImage background = render(colored({0.0f, 0.0f, 0.0f}), HairMaterial::Pigment::GroomColor);
    const QImage greyImage = render(colored({0.5f, 0.5f, 0.5f}), HairMaterial::Pigment::GroomColor);
    std::vector<char> hair(static_cast<std::size_t>(size.width() * size.height()), 0);
    for (int y = 0; y < size.height(); ++y) {
        for (int x = 0; x < size.width(); ++x) {
            const QColor c = greyImage.pixelColor(x, y), b = background.pixelColor(x, y);
            hair[static_cast<std::size_t>(y * size.width() + x)] =
                std::abs(c.red() - b.red()) + std::abs(c.green() - b.green()) + std::abs(c.blue() - b.blue()) > 24;
        }
    }
    // The mean linear color there.
    auto mean = [&](const QImage& image) {
        auto lin = [](double c) { return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4); };
        std::array<double, 3> sum{};
        int n = 0;
        for (int y = 0; y < image.height(); ++y) {
            for (int x = 0; x < image.width(); ++x) {
                if (!hair[static_cast<std::size_t>(y * size.width() + x)]) continue;
                const QColor c = image.pixelColor(x, y);
                sum[0] += lin(c.redF());
                sum[1] += lin(c.greenF());
                sum[2] += lin(c.blueF());
                ++n;
            }
        }
        for (double& v : sum) v /= std::max(n, 1);
        return sum;
    };
    auto luminance = [](const std::array<double, 3>& c) { return 0.2126 * c[0] + 0.7152 * c[1] + 0.0722 * c[2]; };
    const auto brown = mean(render(melanin, HairMaterial::Pigment::GroomMelanin));
    const auto grey = mean(greyImage), skin = mean(render(colored({0.8f, 0.56f, 0.45f}), HairMaterial::Pigment::GroomColor));
    EXPECT_TRUE(greyImage.save(QDir::temp().filePath(QStringLiteral("groomlab_app_picked_grey.png"))));
    const double bright = luminance(grey) / std::max(luminance(brown), 1e-6);
    const double warm = (skin[0] / std::max(skin[2], 1e-6)) / std::max(grey[0] / std::max(grey[2], 1e-6), 1e-6);
    std::printf("picked colors, mean linear: brown %.4f %.4f %.4f, grey %.4f %.4f %.4f, skin %.4f %.4f %.4f\n", brown[0], brown[1],
                brown[2], grey[0], grey[1], grey[2], skin[0], skin[1], skin[2]);
    EXPECT_GT(bright, 3.0) << "a picked 0.5 grey against the melanin brown (Arnold: 7.0 on the head, its key-lit pixels)";
    EXPECT_LT(bright, 40.0);
    EXPECT_GT(warm, 1.03) << "the skin tone reads warmer than the grey";
    EXPECT_LT(warm, 1.9) << "but not the dark red of one pass's transmittance squared";
}

TEST(AppRender, DrawsTheSampleGroomOffscreen) {
    std::unique_ptr<QRhi> rhi = createOffscreenRhi();
    ASSERT_TRUE(rhi);

    auto doc = GroomDocument::open(kSampleGroom);
    ViewRequest request;
    request.density = 0.5f;
    const ViewResult& view = doc->evaluate(request);
    ASSERT_TRUE(view.strands);
    DisplaySettings settings;
    const DisplayFrame& frame = doc->updateDisplay(settings);
    EXPECT_TRUE(frame.item(DisplayItemId::Strands).shown);
    EXPECT_TRUE(frame.item(DisplayItemId::Guides).shown);

    GroomRenderer renderer;
    renderer.setFrame(frame);
    renderer.setScalp(doc->file().data.scalp);
    renderer.setHairLook(doc->session().hairLook());
    renderer.setHairStrands(view.strands, view.versions.strands);
    OrbitCamera camera;
    camera.frame(doc->sceneBounds());
    const QSize size(320, 240);

    // The line path first: the scalp, the strands, the guides.
    renderer.setLookdev(false);
    const OffscreenResult lines = renderOffscreen(renderer, camera, size, 2, rhi.get());
    EXPECT_EQ(lines.frames, 2);
    EXPECT_GE(renderer.lastDrawCalls(), 3);
    EXPECT_FALSE(renderer.lookdevActive());

    // The lookdev path: one interactive frame, then refined ones accumulating.
    renderer.setLookdev(true);
    LightingSetup lighting = lightingPreset("studio");
    lighting.quality = Quality::Interactive;
    lighting.background = Background::Solid;  // the pixels below read a plain backdrop (the app's default is a gradient)
    renderer.setLighting(lighting);
    const OffscreenResult interactive = renderOffscreen(renderer, camera, size, 1, rhi.get());
    const bool lookdev = renderer.lookdevActive();
    EXPECT_EQ(lookdev, LookdevRenderer::supported(rhi.get()));
    if (lookdev) {
        EXPECT_GE(renderer.lookdevRenderer().lastHairDraws(), 3);  // two maps, the scene
        EXPECT_EQ(renderer.lookdevRenderer().pointCount(), view.strands->pointCount());
    }
    lighting.quality = Quality::Refine;
    renderer.setLighting(lighting);
    const OffscreenResult refined = renderOffscreen(renderer, camera, size, 6, rhi.get());
    if (lookdev) EXPECT_EQ(renderer.accumulatedFrames(), 6);

    if (rhi->backend() == QRhi::Null) {
        renderer.releaseResources();
        GTEST_SKIP() << "no GPU device: the pixels are not checked";
    }
    ASSERT_FALSE(lines.image.isNull());
    EXPECT_EQ(lines.image.size(), size);
    EXPECT_GT(pixelsNot(lines.image, QColor(23, 24, 27)), size.width() * size.height() / 20) << "the groom covers some of the view";
    EXPECT_TRUE(lines.image.save(QDir::temp().filePath(QStringLiteral("groomlab_app_smoke_lines.png"))));
    ASSERT_TRUE(lookdev) << "a Direct3D 11 device runs the lookdev path";
    ASSERT_FALSE(interactive.image.isNull());
    ASSERT_FALSE(refined.image.isNull());
    // A solid backdrop is a deep charcoal, the same top and bottom; the groom is lit
    // (not black) and covers some of the view; the refined frames stay close to the interactive
    // one (the same scene, converging), yet differ (jitter, stochastic coverage, more lights).
    const QColor bottom = interactive.image.pixelColor(2, size.height() - 2), top = interactive.image.pixelColor(2, 2);
    EXPECT_EQ(bottom.value(), top.value());
    EXPECT_LT(top.value(), 40) << "a deep charcoal backdrop";
    int lit = 0;
    for (int y = 0; y < size.height(); ++y) {
        for (int x = 0; x < size.width(); ++x) {
            const QColor c = interactive.image.pixelColor(x, y);
            if (c.value() > 40 && std::abs(c.value() - top.value()) > 20 && std::abs(c.value() - bottom.value()) > 20) ++lit;
        }
    }
    EXPECT_GT(lit, size.width() * size.height() / 100);  // the sample's sphere of strands is small in the view
    const double difference = meanDifference(interactive.image, refined.image);
    EXPECT_LT(difference, 12.0);
    EXPECT_GT(difference, 0.0);
    EXPECT_TRUE(interactive.image.save(QDir::temp().filePath(QStringLiteral("groomlab_app_smoke.png"))));
    EXPECT_TRUE(refined.image.save(QDir::temp().filePath(QStringLiteral("groomlab_app_smoke_refined.png"))));
    // Another material and lighting change the picture.
    renderer.setMaterial(hairPreset("blonde"));
    renderer.setLighting(lightingPreset("rim"));
    const OffscreenResult blonde = renderOffscreen(renderer, camera, size, 1, rhi.get());
    ASSERT_FALSE(blonde.image.isNull());
    EXPECT_GT(meanDifference(interactive.image, blonde.image), 2.0);
    renderer.releaseResources();
}

// A 1 x 1 plane in xz (normals up) and straight strands out of it, 3 long, `width` wide at the
// root, Width's profile 1 to `tip` along them, `density` a unit square, evaluated at preview
// density `preview`: what the width as coverage tests look at side on.
StrandsPtr coverageStrands(float width, float tip, MeshPtr* scalpOut, float density = 300.0f, float preview = 1.0f) {
    std::vector<Vec3f> points;
    std::vector<ScalpMesh::Triangle> triangles;
    constexpr int kCells = 4;
    for (int i = 0; i <= kCells; ++i) {
        for (int k = 0; k <= kCells; ++k) points.emplace_back(-0.5f + float(i) / kCells, 0.0f, -0.5f + float(k) / kCells);
    }
    for (int i = 0; i < kCells; ++i) {
        for (int k = 0; k < kCells; ++k) {
            const int a = i * (kCells + 1) + k, b = a + 1, c = a + kCells + 1, d = c + 1;
            triangles.push_back({a, b, c});
            triangles.push_back({b, d, c});
        }
    }
    auto scalp = std::make_shared<ScalpMesh>(std::move(points), std::move(triangles));
    GroomData data;
    data.scalp = scalp;
    data.graphJson = R"({"version": 1, "output": "output", "nodes": [{"name": "scalp", "type": "Scalp"},
        {"name": "scatter", "type": "Scatter", "params": {"density": )" + std::to_string(density) + R"(, "length": 3.0}},
        {"name": "width", "type": "Width", "params": {"width": )" + std::to_string(width) +
                     R"(, "profile": {"keys": [[0.0, 1.0], [1.0, )" + std::to_string(tip) + R"(]]}}},
        {"name": "output", "type": "Output"}],
      "connections": [{"from": "scalp", "to": "scatter", "input": "scalp"}, {"from": "scatter", "to": "width", "input": "strands"},
        {"from": "width", "to": "output", "input": "strands"}]})";
    GroomSession session(data);
    ViewRequest request;
    request.density = preview;
    const StrandsPtr strands = session.evaluate(request).strands;
    *scalpOut = scalp;
    return strands;
}

TEST(AppRender, StrandLinesDrawTheirWidthsAsCoverage) {
    // Width as coverage: straight strands tapering to nothing (about half a pixel wide at the root),
    // seen side on, draw their tips lighter than their roots, lit, flat and in the color modes'
    // colors; off, as opaque lines (the picture of strands a pixel wide or more with it on).
    std::unique_ptr<QRhi> rhi = createOffscreenRhi();
    ASSERT_TRUE(rhi);
    if (rhi->backend() == QRhi::Null) GTEST_SKIP() << "no GPU device: the pixels are not checked";
    MeshPtr scalp;
    const StrandsPtr tapered = coverageStrands(0.012f, 0.0f, &scalp), wide = coverageStrands(1.0f, 1.0f, &scalp);
    ASSERT_TRUE(tapered && wide);
    ASSERT_GT(tapered->strandCount(), 100u);
    OrbitCamera camera;
    camera.setTarget(QVector3D(0.0f, 1.5f, 0.0f));
    camera.setDistance(8.0f);
    camera.orbit(-camera.yaw(), -camera.pitch());  // level, side on
    const QSize size(320, 240);
    const QColor background(23, 24, 27);
    auto render = [&](const StrandsPtr& strands, DisplaySettings settings, bool coverage) {
        DisplayInputs in;
        in.strands = strands;
        in.scalp = scalp;
        in.strandsVersion = strands == wide ? 2 : 1;
        settings.guides = false;
        DisplayBuilder builder;
        builder.setWriteLines(true);
        GroomRenderer renderer;
        renderer.setFrame(builder.update(in, settings));
        renderer.setLookdev(false);
        renderer.setShowScalp(false);
        renderer.setWidthCoverage(coverage);
        const QImage image = renderOffscreen(renderer, camera, size, 1, rhi.get()).image;
        renderer.releaseResources();
        return image;
    };
    // The ink of the rows of the strands' upper (tips) and lower (roots) fifth, from the background.
    auto ink = [&](const QImage& image) {
        std::vector<double> rows(static_cast<std::size_t>(image.height()), 0.0);
        for (int y = 0; y < image.height(); ++y) {
            for (int x = 0; x < image.width(); ++x) {
                const QColor c = image.pixelColor(x, y);
                rows[static_cast<std::size_t>(y)] += std::abs(c.red() - background.red()) + std::abs(c.green() - background.green()) +
                                                     std::abs(c.blue() - background.blue());
            }
        }
        int top = 0, bottom = image.height() - 1;
        while (top < bottom && rows[static_cast<std::size_t>(top)] < 1.0) ++top;
        while (bottom > top && rows[static_cast<std::size_t>(bottom)] < 1.0) --bottom;
        auto band = [&](double from, double to) {
            const int a = top + static_cast<int>(from * (bottom - top)), b = top + static_cast<int>(to * (bottom - top));
            double sum = 0.0;
            for (int y = a; y < b; ++y) sum += rows[static_cast<std::size_t>(y)];
            return sum / std::max(1, b - a);
        };
        return std::pair<double, double>(band(0.1, 0.3), band(0.7, 0.9));  // tips, roots
    };
    for (const int mode : {0, 1, 2}) {  // lit, flat, colored
        SCOPED_TRACE(mode == 0 ? "lit" : mode == 1 ? "flat" : "colored");
        DisplaySettings settings;
        settings.lighting = mode == 0;
        settings.colorMode = mode == 2 ? kRandomColors : kSolidColors;
        const QImage on = render(tapered, settings, true), off = render(tapered, settings, false);
        ASSERT_FALSE(on.isNull());
        const auto [onTips, onRoots] = ink(on);
        const auto [offTips, offRoots] = ink(off);
        std::printf("[ coverage ] %s: tips %.0f, roots %.0f; off: %.0f, %.0f\n", mode == 0 ? "lit" : mode == 1 ? "flat" : "colored",
                    onTips, onRoots, offTips, offRoots);
        // Against the opaque lines' (the lit tips are lighter there: the root-to-tip ramp), the tips
        // keep far less of their ink than the roots (where strands overlapping in depth add up).
        EXPECT_LT(onTips / offTips, 0.6 * onRoots / offRoots);
        EXPECT_LT(onRoots, 0.95 * offRoots);
        EXPECT_GT(offTips, 0.6 * offRoots);  // off: opaque to the tips
        // Off: the opaque lines whatever the widths, as strands a pixel wide or more draw with it on.
        EXPECT_EQ(meanDifference(off, render(wide, settings, false)), 0.0);
        EXPECT_LT(meanDifference(off, render(wide, settings, true)), 0.05);
        if (mode == 0) {
            EXPECT_TRUE(on.save(QDir::temp().filePath(QStringLiteral("groomlab_app_coverage_on.png"))));
            EXPECT_TRUE(off.save(QDir::temp().filePath(QStringLiteral("groomlab_app_coverage_off.png"))));
        }
    }
}

TEST(AppRender, DenseStrandLinesKeepTheirCoverage) {
    // Width as coverage on dense hair seen from afar (a few percent of a pixel a strand, a hundred
    // strands behind each pixel): the strands each cover their share of a pixel (hashed), so the hair
    // keeps about the ink opaque lines give it instead of fading to its front strand's coverage, on
    // one sample a pixel and on the viewport's four; the same frame draws the same picture and a
    // small orbit changes few pixels; a thinned preview (density 0.25) reads as dense as the full one
    // with its density given, lighter without.
    std::unique_ptr<QRhi> rhi = createOffscreenRhi();
    ASSERT_TRUE(rhi);
    if (rhi->backend() == QRhi::Null) GTEST_SKIP() << "no GPU device: the pixels are not checked";
    MeshPtr scalp;
    const StrandsPtr dense = coverageStrands(0.003f, 1.0f, &scalp, 2000.0f);
    const StrandsPtr moderate = coverageStrands(0.003f, 1.0f, &scalp, 400.0f);
    const StrandsPtr thinned = coverageStrands(0.003f, 1.0f, &scalp, 400.0f, 0.25f);
    ASSERT_TRUE(dense && moderate && thinned);
    ASSERT_LT(thinned->strandCount(), moderate->strandCount() / 2);
    OrbitCamera camera;
    camera.setTarget(QVector3D(0.0f, 1.5f, 0.0f));
    camera.setDistance(20.0f);
    camera.orbit(-camera.yaw(), -camera.pitch());  // level, side on
    const QSize size(320, 240);
    const QColor background(23, 24, 27);
    auto render = [&](const StrandsPtr& strands, bool lit, bool coverage, int samples, float density = 1.0f) {
        DisplayInputs in;
        in.strands = strands;
        in.scalp = scalp;
        in.strandsVersion = 1;
        DisplaySettings settings;
        settings.guides = false;
        settings.lighting = lit;
        DisplayBuilder builder;
        builder.setWriteLines(true);
        GroomRenderer renderer;
        renderer.setBackground(background);
        renderer.setFrame(builder.update(in, settings));
        renderer.setLookdev(false);
        renderer.setShowScalp(false);
        renderer.setWidthCoverage(coverage, 1.0f, density);
        const QImage image = renderOffscreen(renderer, camera, size, 1, rhi.get(), samples).image;
        renderer.releaseResources();
        return image;
    };
    auto ink = [&](const QImage& image) {
        double sum = 0.0;
        for (int y = 0; y < image.height(); ++y) {
            for (int x = 0; x < image.width(); ++x) {
                const QColor c = image.pixelColor(x, y);
                sum += std::abs(c.red() - background.red()) + std::abs(c.green() - background.green()) +
                       std::abs(c.blue() - background.blue());
            }
        }
        return sum / (image.width() * image.height());
    };
    auto changed = [](const QImage& a, const QImage& b) {
        int count = 0;
        for (int y = 0; y < a.height(); ++y) {
            for (int x = 0; x < a.width(); ++x) count += a.pixel(x, y) != b.pixel(x, y) ? 1 : 0;
        }
        return static_cast<double>(count) / (a.width() * a.height());
    };
    for (const int samples : {1, 4}) {
        for (const bool lit : {false, true}) {
            SCOPED_TRACE(std::string(lit ? "lit" : "flat") + (samples > 1 ? ", 4 samples" : ", 1 sample"));
            const QImage on = render(dense, lit, true, samples), off = render(dense, lit, false, samples);
            ASSERT_FALSE(on.isNull());
            const double inkOn = ink(on), inkOff = ink(off);
            std::printf("[ coverage ] dense, %s, %d sample(s): ink %.2f, opaque %.2f (%.0f%%)\n", lit ? "lit" : "flat", samples, inkOn,
                        inkOff, 100.0 * inkOn / inkOff);
            EXPECT_GT(inkOn, 0.85 * inkOff);
            if (!lit) {
                EXPECT_TRUE(on.save(QDir::temp().filePath(QStringLiteral("groomlab_app_dense_coverage_%1.png").arg(samples))));
            }
        }
    }
    // The pattern holds: the same frame twice; a 0.2 degree orbit.
    const QImage first = render(moderate, false, true, 1), second = render(moderate, false, true, 1);
    EXPECT_EQ(changed(first, second), 0.0);
    camera.orbit(0.2f, 0.0f);
    const QImage orbited = render(moderate, false, true, 1);
    camera.orbit(-0.2f, 0.0f);
    std::printf("[ coverage ] a 0.2 degree orbit changes %.4f of the pixels\n", changed(first, orbited));
    EXPECT_LT(changed(first, orbited), 0.03);
    // A thinned preview with its density given covers as the full one does; without, less.
    const double full = ink(render(moderate, false, true, 1)), compensated = ink(render(thinned, false, true, 1, 0.25f)),
                 plain = ink(render(thinned, false, true, 1));
    std::printf("[ coverage ] density 0.25: %.2f compensated, %.2f plain; full %.2f\n", compensated, plain, full);
    EXPECT_NEAR(compensated, full, 0.1 * full);
    EXPECT_LT(plain, 0.8 * compensated);
}

TEST(AppRender, ClumpLodDrawsAShareOfTheStrandsWithTheirInk) {
    // The screen-space level of detail (strand_lod.h; on by default) on dense thin strands seen
    // from afar (the strands grouped by cells of their roots: no clump channel): a share of them,
    // each widened to stand for the rest, draws the ink every strand does, flat and lit; a still
    // view draws the same picture, a small orbit keeps the strands drawn (no indices written again)
    // and changes few pixels; off, every strand.
    std::unique_ptr<QRhi> rhi = createOffscreenRhi();
    ASSERT_TRUE(rhi);
    if (rhi->backend() == QRhi::Null) GTEST_SKIP() << "no GPU device: the pixels are not checked";
    MeshPtr scalp;
    const StrandsPtr dense = coverageStrands(0.003f, 0.3f, &scalp, 2000.0f);
    ASSERT_TRUE(dense);
    OrbitCamera camera;
    camera.setTarget(QVector3D(0.0f, 1.5f, 0.0f));
    camera.setDistance(30.0f);
    camera.orbit(-camera.yaw(), 20.0f - camera.pitch());
    const QSize size(320, 240);
    const QColor background(23, 24, 27);
    auto ink = [&](const QImage& image) {
        double sum = 0.0;
        for (int y = 0; y < image.height(); ++y) {
            for (int x = 0; x < image.width(); ++x) {
                const QColor c = image.pixelColor(x, y);
                sum += std::abs(c.red() - background.red()) + std::abs(c.green() - background.green()) + std::abs(c.blue() - background.blue());
            }
        }
        return sum / (image.width() * image.height());
    };
    auto changed = [](const QImage& a, const QImage& b) {
        int count = 0;
        for (int y = 0; y < a.height(); ++y) {
            for (int x = 0; x < a.width(); ++x) count += a.pixel(x, y) != b.pixel(x, y) ? 1 : 0;
        }
        return static_cast<double>(count) / (a.width() * a.height());
    };
    for (const bool lit : {false, true}) {
        SCOPED_TRACE(lit ? "lit" : "flat");
        DisplayInputs in;
        in.strands = dense;
        in.scalp = scalp;
        in.strandsVersion = 1;
        DisplaySettings settings;
        settings.guides = false;
        settings.lighting = lit;
        DisplayBuilder builder;
        builder.setWriteLines(true);
        GroomRenderer renderer;
        renderer.setBackground(background);
        renderer.setFrame(builder.update(in, settings));
        renderer.setLookdev(false);
        renderer.setShowScalp(false);
        renderer.setWidthCoverage(true);
        renderer.setClumpLod(false);
        const QImage all = renderOffscreen(renderer, camera, size, 1, rhi.get()).image;
        EXPECT_FALSE(renderer.lodStats().on);
        renderer.setClumpLod(true);
        const QImage first = renderOffscreen(renderer, camera, size, 1, rhi.get()).image;
        const GroomRenderer::LodStats stats = renderer.lodStats();
        const QImage second = renderOffscreen(renderer, camera, size, 1, rhi.get()).image;
        camera.orbit(1.0f, 0.0f);
        const QImage orbited = renderOffscreen(renderer, camera, size, 1, rhi.get()).image;
        camera.orbit(-1.0f, 0.0f);
        const GroomRenderer::LodStats after = renderer.lodStats();
        std::printf("[ lod ] %s: %zu of %zu strands drawn (%zu impostors, %zu draws), ink %.2f against every strand's %.2f; "
                    "a still view changes %.4f of the pixels, a 1 degree orbit %.4f\n",
                    lit ? "lit" : "flat", stats.drawnStrands, stats.totalStrands, stats.impostors, stats.draws, ink(first), ink(all),
                    changed(first, second), changed(first, orbited));
        ASSERT_TRUE(stats.on);
        EXPECT_LT(stats.drawnStrands, stats.totalStrands / 2);
        EXPECT_GT(stats.draws, 0u);
        EXPECT_NEAR(ink(first), ink(all), 0.1 * ink(all));
        EXPECT_EQ(changed(first, second), 0.0);
        EXPECT_EQ(after.rebuilds, stats.rebuilds);  // (the orbit wrote no indices)
        EXPECT_EQ(after.level, stats.level);
        EXPECT_LT(changed(first, orbited), 0.05);
        renderer.releaseResources();
    }
}

// The visibility path's coverage, measured as ink: black hair (no lobes, no lights, no
// environment) over a white backdrop, so each pixel is 1 - alpha. Straight vertical strands at
// x = half a pixel right of the view's axis, the camera 10 away looking down -z, 256 px square,
// fov 40: a strand of width w pixels at depth d is w x the pixel's world size there wide.
namespace vis {

constexpr int kSize = 256;
constexpr float kDistance = 10.0f;

float pixelWorld(float depth) { return 2.0f * std::tan(20.0f * 3.14159265f / 180.0f) * depth / static_cast<float>(kSize); }

// Strands at view depths `depths`, each `pixels` wide where it stands, in the order given.
StrandsPtr strandsAt(const std::vector<float>& depths, float pixels) {
    auto set = std::make_shared<StrandSet>();
    StrandId id = 1;
    for (float depth : depths) {
        const float z = kDistance - depth, x = 0.5f * pixelWorld(depth), w = pixels * pixelWorld(depth);
        set->addStrand(id++, {{x, -2.0f, z}, {x, -1.0f, z}, {x, 0.0f, z}, {x, 1.0f, z}, {x, 2.0f, z}}, std::vector<float>(5, w));
    }
    return set;
}

// The rows' mean ink (1 - the backdrop's share, linear) over rows 96..159, summed across each row.
double rowInk(const QImage& image) {
    double total = 0.0;
    for (int y = 96; y < 160; ++y) {
        for (int x = 0; x < image.width(); ++x) {
            const double v = image.pixelColor(x, y).redF();
            const double linear = v <= 0.04045 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4);
            total += 1.0 - linear;
        }
    }
    return total / 64.0;
}

QImage render(QRhi* rhi, const StrandsPtr& strands, LookdevRenderer::HairPath path, std::uint64_t version) {
    GroomRenderer renderer;
    renderer.setLookdev(true);
    renderer.setShowScalp(false);
    renderer.setClumpLod(false);
    LightingSetup l = lightingPreset("key");
    l.lights.clear();
    l.environment = Environment::None;
    l.background = Background::Solid;
    l.backgroundColor = {1.0f, 1.0f, 1.0f};
    l.toneMap = ToneMap::None;
    l.exposure = 0.0f;
    l.ambientOcclusion = false;
    l.quality = Quality::Interactive;
    l.interactiveQuality = InteractiveQuality::Full;
    renderer.setLighting(l);
    HairMaterial m = hairPreset("black");
    m.pigment = HairMaterial::Pigment::Material;
    m.specular = m.specular2 = m.transmission = m.diffuse = m.multipleScattering = 0.0f;
    renderer.setMaterial(m);
    renderer.lookdevRenderer().setHairPath(path);
    renderer.setHairStrands(strands, version);
    OrbitCamera camera;
    camera.orbit(-camera.yaw(), -camera.pitch());
    camera.setTarget(QVector3D(0.0f, 0.0f, 0.0f));
    camera.setDistance(kDistance);
    const OffscreenResult r = renderOffscreen(renderer, camera, QSize(kSize, kSize), 1, rhi);
    EXPECT_EQ(renderer.lookdevRenderer().lastHairPath(), path);
    renderer.releaseResources();
    return r.image;
}

}  // namespace vis

TEST(AppRender, VisibilityBufferCoverIsTheStrandsWidth) {
    // One strand a fraction of a pixel wide, or a few: a row holds its width in ink, whatever it
    // straddles (the analytic box-filtered coverage, no MSAA steps, no coverage floor).
    std::unique_ptr<QRhi> rhi = createOffscreenRhi();
    ASSERT_TRUE(rhi);
    if (rhi->backend() == QRhi::Null || !rhi->isFeatureSupported(QRhi::Compute)) GTEST_SKIP() << "no compute device";
    for (float pixels : {0.3f, 2.5f}) {
        const QImage image = vis::render(rhi.get(), vis::strandsAt({vis::kDistance}, pixels), LookdevRenderer::HairPath::Visibility, 1);
        ASSERT_FALSE(image.isNull());
        EXPECT_NEAR(vis::rowInk(image), pixels, 0.04 * pixels + 0.02) << pixels << " px wide";
        EXPECT_TRUE(image.save(QDir::temp().filePath(QStringLiteral("groomlab_app_vis_cover_%1.png").arg(pixels))));
    }
}

TEST(AppRender, VisibilityBufferTransparencyIsOrderIndependent) {
    // Four strands behind one another over the same pixel column, each 0.3 of it: together they
    // cover 1 - 0.7^4 of it (Beer-Lambert, as their ribbons would), in either order.
    std::unique_ptr<QRhi> rhi = createOffscreenRhi();
    ASSERT_TRUE(rhi);
    if (rhi->backend() == QRhi::Null || !rhi->isFeatureSupported(QRhi::Compute)) GTEST_SKIP() << "no compute device";
    const double expected = 1.0 - std::pow(0.7, 4.0);
    const QImage front = vis::render(rhi.get(), vis::strandsAt({7.0f, 8.0f, 9.0f, 10.0f}, 0.3f), LookdevRenderer::HairPath::Visibility, 1);
    const QImage back = vis::render(rhi.get(), vis::strandsAt({10.0f, 9.0f, 8.0f, 7.0f}, 0.3f), LookdevRenderer::HairPath::Visibility, 2);
    ASSERT_FALSE(front.isNull());
    ASSERT_FALSE(back.isNull());
    EXPECT_NEAR(vis::rowInk(front), expected, 0.03);
    EXPECT_NEAR(vis::rowInk(back), expected, 0.03);
    EXPECT_LT(meanDifference(front, back), 0.05);
    // The legacy path draws the same strands too (its thin strands' coverage is approximate).
    const QImage legacy = vis::render(rhi.get(), vis::strandsAt({7.0f, 8.0f, 9.0f, 10.0f}, 0.3f), LookdevRenderer::HairPath::Legacy, 3);
    ASSERT_FALSE(legacy.isNull());
    EXPECT_GT(vis::rowInk(legacy), 0.3);
}

TEST(AppRender, DrawsGuideDotsAndToolOverlay) {
    std::unique_ptr<QRhi> rhi = createOffscreenRhi();
    ASSERT_TRUE(rhi);
    BrushFixture f;
    DisplaySettings settings;
    settings.guidePoints = 2;
    const DisplayFrame& frame = f.doc->updateDisplay(settings);
    ASSERT_FALSE(frame.dots.points.empty());
    GroomRenderer renderer;
    renderer.setFrame(frame);
    renderer.setScalp(f.doc->file().data.scalp);
    renderer.setLookdev(false);
    const QSize size(320, 240);
    const OffscreenResult plain = renderOffscreen(renderer, f.camera, size, 1, rhi.get());
    const int calls = renderer.lastDrawCalls();
    EXPECT_GE(calls, 4) << "the scalp, the strands, the guides, the dots";
    // The brush ring and the hovered guides: one more draw while the overlay is set, none once
    // it is cleared.
    const HierarchyGuide& g = f.guide(0);
    f.session.settings().screenRadius = 60.0f;
    f.session.hover(f.widgetPoint(g.points[g.points.size() / 2]));
    renderer.setToolOverlay(f.session.overlay(), f.session.overlayVersion());
    const OffscreenResult hovered = renderOffscreen(renderer, f.camera, size, 1, rhi.get());
    EXPECT_EQ(renderer.lastDrawCalls(), calls + 1);
    f.session.clearHover();
    renderer.setToolOverlay(f.session.overlay(), f.session.overlayVersion());
    renderOffscreen(renderer, f.camera, size, 1, rhi.get());
    EXPECT_EQ(renderer.lastDrawCalls(), calls);
    if (rhi->backend() == QRhi::Null) {
        renderer.releaseResources();
        GTEST_SKIP() << "no GPU device: the pixels are not checked";
    }
    ASSERT_FALSE(plain.image.isNull());
    ASSERT_FALSE(hovered.image.isNull());
    EXPECT_GT(meanDifference(plain.image, hovered.image), 0.0) << "the ring is in the picture";
    EXPECT_TRUE(hovered.image.save(QDir::temp().filePath(QStringLiteral("groomlab_app_brush_ring.png"))));
    renderer.releaseResources();
}

TEST(AppRender, HairEyeHidesTheStrandsAloneAtOnce) {
    // The Groom Tools' hair eye (GroomRenderer::setHairShown): the hair's items and the lookdev's
    // strands go at once (no new frame, no strands handed again), the guides and their dots stay;
    // shown again, the strands held come back. The guides' eye (DisplaySettings::guides) is the
    // other way round.
    EXPECT_TRUE(isHairItem(DisplayItemId::Strands));
    EXPECT_TRUE(isHairItem(DisplayItemId::Cards));
    EXPECT_FALSE(isHairItem(DisplayItemId::Guides));
    EXPECT_FALSE(isHairItem(DisplayItemId::Parts));
    std::unique_ptr<QRhi> rhi = createOffscreenRhi();
    ASSERT_TRUE(rhi);
    auto doc = GroomDocument::open(kSampleGroom);
    ViewRequest request;
    request.density = 0.5f;
    const ViewResult& view = doc->evaluate(request);
    ASSERT_TRUE(view.strands);
    DisplaySettings settings;
    settings.guidePoints = 2;
    const DisplayFrame& frame = doc->updateDisplay(settings);
    ASSERT_TRUE(frame.item(DisplayItemId::Strands).shown);
    ASSERT_TRUE(frame.item(DisplayItemId::Guides).shown);
    ASSERT_FALSE(frame.dots.points.empty());

    GroomRenderer renderer;
    renderer.setFrame(frame);
    renderer.setScalp(doc->file().data.scalp);
    renderer.setHairLook(doc->session().hairLook());
    renderer.setHairStrands(view.strands, view.versions.strands);
    renderer.setClumpLod(false);  // (one draw for the strands)
    OrbitCamera camera;
    camera.frame(doc->sceneBounds());
    const QSize size(320, 240);
    const QString dir = qEnvironmentVariable("GROOMLAB_EYES_DIR", QDir::temp().path());
    auto save = [&](const QImage& image, const char* name) {
        if (!image.isNull()) image.save(QDir(dir).filePath(QStringLiteral("eyes_%1.png").arg(QLatin1String(name))));
    };

    // The line path: the scalp, the strands, the guides, the dots; the hair hidden, one draw fewer.
    renderer.setLookdev(false);
    const OffscreenResult all = renderOffscreen(renderer, camera, size, 1, rhi.get());
    const int calls = renderer.lastDrawCalls();
    EXPECT_GE(calls, 4);
    renderer.setHairShown(false);
    EXPECT_FALSE(renderer.hairShown());
    const OffscreenResult hairHidden = renderOffscreen(renderer, camera, size, 1, rhi.get());
    EXPECT_EQ(renderer.lastDrawCalls(), calls - 1) << "the strands alone go: the scalp, the guides and their dots stay";
    // The guides hidden too (their eye: the builder's frame without them): fewer still.
    settings.guides = false;
    renderer.setFrame(doc->updateDisplay(settings));
    const OffscreenResult bothHidden = renderOffscreen(renderer, camera, size, 1, rhi.get());
    const int scalpOnly = renderer.lastDrawCalls();
    EXPECT_LT(scalpOnly, calls - 1) << "the guides drew while the hair was hidden";
    EXPECT_GE(scalpOnly, 1);
    // The guides hidden, the hair shown: the strands draw again (their buffers kept).
    renderer.setHairShown(true);
    const OffscreenResult guidesHidden = renderOffscreen(renderer, camera, size, 1, rhi.get());
    EXPECT_EQ(renderer.lastDrawCalls(), scalpOnly + 1);
    settings.guides = true;
    renderer.setFrame(doc->updateDisplay(settings));

    // The lookdev path: its strands go without being handed again, and come back from the ones held.
    renderer.setLookdev(true);
    LightingSetup lighting = lightingPreset("studio");
    lighting.quality = Quality::Interactive;
    lighting.background = Background::Solid;
    renderer.setLighting(lighting);
    const OffscreenResult lookAll = renderOffscreen(renderer, camera, size, 1, rhi.get());
    const bool lookdev = renderer.lookdevActive();
    if (lookdev) {
        EXPECT_EQ(renderer.lookdevRenderer().pointCount(), view.strands->pointCount());
        EXPECT_GE(renderer.lookdevRenderer().lastHairDraws(), 1);
    }
    renderer.setHairShown(false);
    const OffscreenResult lookHairHidden = renderOffscreen(renderer, camera, size, 1, rhi.get());
    if (lookdev) {
        EXPECT_EQ(renderer.lookdevRenderer().pointCount(), 0u) << "no strands in the lookdev while the hair is hidden";
        EXPECT_EQ(renderer.lookdevRenderer().lastHairDraws(), 0);
    }
    // Strands handed meanwhile (an evaluation while hidden) are held, not drawn.
    renderer.setHairStrands(view.strands, view.versions.strands);
    renderOffscreen(renderer, camera, size, 1, rhi.get());
    if (lookdev) EXPECT_EQ(renderer.lookdevRenderer().pointCount(), 0u);
    renderer.setHairShown(true);
    renderOffscreen(renderer, camera, size, 1, rhi.get());
    if (lookdev) EXPECT_EQ(renderer.lookdevRenderer().pointCount(), view.strands->pointCount()) << "back without an evaluation";

    if (rhi->backend() == QRhi::Null) {
        renderer.releaseResources();
        GTEST_SKIP() << "no GPU device: the pixels are not checked";
    }
    save(all.image, "lines_all");
    save(hairHidden.image, "lines_hair_hidden");
    save(guidesHidden.image, "lines_guides_hidden");
    save(bothHidden.image, "lines_both_hidden");
    save(lookAll.image, "lookdev_all");
    save(lookHairHidden.image, "lookdev_hair_hidden");
    const QColor backdrop(23, 24, 27);
    EXPECT_GT(meanDifference(all.image, hairHidden.image), 0.0) << "the strands went";
    EXPECT_GT(meanDifference(hairHidden.image, bothHidden.image), 0.0) << "the guides drew with the hair hidden";
    EXPECT_GT(meanDifference(all.image, guidesHidden.image), 0.0);
    EXPECT_GT(pixelsNot(guidesHidden.image, backdrop), pixelsNot(bothHidden.image, backdrop)) << "the strands drew with the guides hidden";
    if (lookdev) EXPECT_GT(meanDifference(lookAll.image, lookHairHidden.image), 0.5) << "the lookdev's strands went";
    renderer.releaseResources();
}

// A closed sphere of `radius` about the origin (a scalp all round), its triangles beyond -0.75 of
// the radius along z (the back cap) first; `capTriangles` says how many those are.
MeshPtr closedSphere(float radius, std::size_t* capTriangles) {
    constexpr int kRings = 32, kSegments = 48;
    std::vector<Vec3f> points;
    for (int r = 0; r <= kRings; ++r) {
        const float theta = 3.14159265f * static_cast<float>(r) / kRings;  // from +z to -z
        for (int s = 0; s < kSegments; ++s) {
            const float phi = 2.0f * 3.14159265f * static_cast<float>(s) / kSegments;
            points.emplace_back(radius * std::sin(theta) * std::cos(phi), radius * std::sin(theta) * std::sin(phi), radius * std::cos(theta));
        }
    }
    std::vector<ScalpMesh::Triangle> cap, rest;
    for (int r = 0; r < kRings; ++r) {
        for (int s = 0; s < kSegments; ++s) {
            const int a = r * kSegments + s, b = r * kSegments + (s + 1) % kSegments, c = a + kSegments, d = b + kSegments;
            // Outward: (b - a) x (c - a) points away from the center.
            const bool back = points[static_cast<std::size_t>(c)].z < -0.75f * radius;
            auto& to = back ? cap : rest;
            if (r > 0) to.push_back({a, c, b});
            if (r + 1 < kRings) to.push_back({b, c, d});
        }
    }
    *capTriangles = cap.size();
    cap.insert(cap.end(), rest.begin(), rest.end());
    return std::make_shared<ScalpMesh>(std::move(points), std::move(cap));
}

TEST(AppRender, TheScalpHidesTheStrandsBehindIt) {
    // A closed scalp with hair on its back cap alone, seen from the front: the scalp hides every
    // strand behind it in the line path (lit, flat, colored; width as coverage with its level of
    // detail) and in the lookdev (the default), the scalp's subdivided surface too. The hair shown
    // or hidden draws the same frame inside the scalp's outline.
    std::unique_ptr<QRhi> rhi = createOffscreenRhi();
    ASSERT_TRUE(rhi);
    if (rhi->backend() == QRhi::Null) GTEST_SKIP() << "no GPU device: the pixels are not checked";
    std::size_t capCount = 0;
    const MeshPtr sphere = closedSphere(10.0f, &capCount);
    ASSERT_GT(capCount, 10u);
    // The hair's roots: the cap's triangles (the sphere's first ones, the same points).
    std::vector<ScalpMesh::Triangle> capTriangles(sphere->triangles().begin(), sphere->triangles().begin() + static_cast<std::ptrdiff_t>(capCount));
    auto cap = std::make_shared<ScalpMesh>(sphere->points(), std::move(capTriangles));
    GroomData data;
    data.scalp = cap;
    data.graphJson = R"({"version": 1, "output": "output", "nodes": [{"name": "scalp", "type": "Scalp"},
        {"name": "scatter", "type": "Scatter", "params": {"density": 30.0, "length": 2.5}},
        {"name": "output", "type": "Output"}],
      "connections": [{"from": "scalp", "to": "scatter", "input": "scalp"}, {"from": "scatter", "to": "output", "input": "strands"}]})";
    GroomSession session(data);
    ViewRequest request;
    const StrandsPtr strands = session.evaluate(request).strands;
    ASSERT_TRUE(strands);
    ASSERT_GT(strands->strandCount(), 500u);
    OrbitCamera camera;
    Bounds b;
    b.valid = true;
    b.lo = {-13.0f, -13.0f, -13.0f};
    b.hi = {13.0f, 13.0f, 13.0f};
    camera.frame(b);
    camera.orbit(-camera.yaw(), -camera.pitch());  // straight down the z axis: the cap behind the sphere
    const QSize size(240, 240);
    const QString dir = qEnvironmentVariable("GROOMLAB_OCCLUSION_DIR");
    // The pixels inside the scalp's outline (a centered box well inside it) the hair changed.
    auto changed = [&](const QImage& with, const QImage& without) {
        int n = 0;
        for (int y = size.height() / 4; y < size.height() * 3 / 4; ++y) {
            for (int x = size.width() / 4; x < size.width() * 3 / 4; ++x) {
                const QColor a = with.pixelColor(x, y), c = without.pixelColor(x, y);
                if (std::abs(a.red() - c.red()) + std::abs(a.green() - c.green()) + std::abs(a.blue() - c.blue()) > 24) ++n;
            }
        }
        return n;
    };
    LightingSetup lighting = lightingPreset("studio");
    lighting.quality = Quality::Interactive;
    lighting.background = Background::Solid;
    for (const int levels : {0, 2}) {
        const MeshPtr shown = levels > 0 ? subdivideScalp(sphere, levels) : sphere;
        for (const int mode : {0, 1, 2, 3}) {  // the lookdev; lines lit, flat, colored
            const char* name = mode == 0 ? "lookdev" : mode == 1 ? "lit" : mode == 2 ? "flat" : "colored";
            SCOPED_TRACE(std::string(name) + " subdivision " + std::to_string(levels));
            DisplaySettings settings;
            settings.guides = false;
            settings.lighting = mode != 2;
            settings.colorMode = mode == 3 ? kRandomColors : kSolidColors;
            DisplayInputs in;
            in.strands = strands;
            in.scalp = shown;
            in.strandsVersion = 1;
            DisplayBuilder builder;
            builder.setWriteLines(true);
            GroomRenderer renderer;
            renderer.setFrame(builder.update(in, settings));
            renderer.setScalp(shown);
            renderer.setHairStrands(strands, 1);
            renderer.setLighting(lighting);
            renderer.setLookdev(mode == 0);
            const OffscreenResult with = renderOffscreen(renderer, camera, size, 1, rhi.get());
            if (mode == 0) EXPECT_EQ(renderer.lookdevActive(), LookdevRenderer::supported(rhi.get()));
            // The scalp hidden: the hair behind it is what the box shows (the check sees strands there).
            renderer.setShowScalp(false);
            const OffscreenResult bare = renderOffscreen(renderer, camera, size, 1, rhi.get());
            renderer.setShowScalp(true);
            renderer.setHairShown(false);
            const OffscreenResult without = renderOffscreen(renderer, camera, size, 1, rhi.get());
            renderer.releaseResources();
            ASSERT_FALSE(with.image.isNull());
            EXPECT_GT(changed(bare.image, without.image), size.width() * size.height() / 4 / 4) << "the strands behind drew nothing";
            if (!dir.isEmpty()) {
                with.image.save(QDir(dir).filePath(QStringLiteral("occlusion_%1_sub%2.png").arg(QLatin1String(name)).arg(levels)));
                without.image.save(QDir(dir).filePath(QStringLiteral("occlusion_%1_sub%2_nohair.png").arg(QLatin1String(name)).arg(levels)));
            }
            const int n = changed(with.image, without.image);
            std::printf("[ occlusion ] %s, subdivision %d: %d pixels inside the scalp changed by the hair behind it\n", name, levels, n);
            EXPECT_LE(n, size.width() * size.height() / 4 / 200) << "strands behind the scalp show through it";
        }
    }
}

// The mean color of the pixels inside a centered box of the image (a fraction of its size).
QColor meanColorInside(const QImage& image, double fraction) {
    double r = 0.0, g = 0.0, b = 0.0;
    int n = 0;
    const int x0 = static_cast<int>(image.width() * (0.5 - fraction / 2)), x1 = static_cast<int>(image.width() * (0.5 + fraction / 2));
    const int y0 = static_cast<int>(image.height() * (0.5 - fraction / 2)), y1 = static_cast<int>(image.height() * (0.5 + fraction / 2));
    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) {
            const QColor c = image.pixelColor(x, y);
            r += c.red();
            g += c.green();
            b += c.blue();
            ++n;
        }
    }
    return n > 0 ? QColor(static_cast<int>(r / n), static_cast<int>(g / n), static_cast<int>(b / n)) : QColor();
}

TEST(AppRender, DrawsSurfacesInEveryMaterial) {
    std::unique_ptr<QRhi> rhi = createOffscreenRhi();
    ASSERT_TRUE(rhi);
    // A sphere (an eyeball's mesh with any material) as the one prop, filling the view.
    auto sphere = std::make_shared<Surface>(makeEyeSurface({0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 1.0f}, {0.0f, 1.0f, 0.0f}, 1.2f, surfacePreset("clay")));
    sphere->name = "sphere";
    GroomRenderer renderer;
    renderer.setProps({sphere});
    LightingSetup lighting = lightingPreset("studio");
    lighting.quality = Quality::Interactive;
    lighting.background = Background::Solid;  // the pixels below read a plain backdrop (the app's default is a gradient)
    renderer.setLighting(lighting);
    OrbitCamera camera;
    Bounds b;
    b.valid = true;
    b.lo = {-1.2f, -1.2f, -1.2f};
    b.hi = {1.2f, 1.2f, 1.2f};
    camera.frame(b);
    camera.orbit(-camera.yaw(), -camera.pitch());  // straight down the z axis, at the cornea
    const QSize size(240, 240);

    // The line path draws the prop too (Lambert).
    renderer.setLookdev(false);
    const OffscreenResult lines = renderOffscreen(renderer, camera, size, 1, rhi.get());
    EXPECT_GE(renderer.lastDrawCalls(), 1);

    renderer.setLookdev(true);
    auto renderAs = [&](const std::string& preset) {
        sphere->material = surfacePreset(preset);
        renderer.propsChanged();
        return renderOffscreen(renderer, camera, size, 1, rhi.get());
    };
    const OffscreenResult clay = renderAs("clay");
    const bool lookdev = renderer.lookdevActive();
    EXPECT_EQ(lookdev, LookdevRenderer::supported(rhi.get()));
    if (lookdev) {
        EXPECT_GE(renderer.lookdevRenderer().lastSurfaceDraws(), 3);  // the maps and the scene
        EXPECT_GE(renderer.lookdevRenderer().textureCount(), 3);     // the tables
    }
    const OffscreenResult metal = renderAs("metal"), skin = renderAs("skin"), eye = renderAs("eye blue");
    // Skin with the pre-integrated profile against plain Lambert, on a small sphere (1.2 mm:
    // an ear's rim) where the profile wraps the terminator the most.
    auto small = std::make_shared<Surface>(makeEyeSurface({0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 1.0f}, {0.0f, 1.0f, 0.0f}, 0.12f, surfacePreset("skin")));
    ASSERT_TRUE(setSurfaceParam(small->material, "sssMode", "preintegrated"));
    renderer.setProps({small});
    OrbitCamera close = camera;
    close.dolly(0.1f);
    const OffscreenResult scattered = renderOffscreen(renderer, close, size, 1, rhi.get());
    ASSERT_TRUE(setSurfaceParam(small->material, "sssStrength", "0"));
    renderer.propsChanged();
    const OffscreenResult lambert = renderOffscreen(renderer, close, size, 1, rhi.get());
    renderer.setProps({sphere});
    if (rhi->backend() == QRhi::Null) {
        renderer.releaseResources();
        GTEST_SKIP() << "no GPU device: the pixels are not checked";
    }
    ASSERT_TRUE(lookdev);
    ASSERT_FALSE(lines.image.isNull());
    EXPECT_GT(pixelsNot(lines.image, QColor(23, 24, 27)), size.width() * size.height() / 6) << "the sphere covers the middle";
    for (const OffscreenResult* r : {&clay, &metal, &skin, &eye, &lambert, &scattered}) ASSERT_FALSE(r->image.isNull());
    // Each material is its own picture: the metal mirrors the studio (brighter highlights,
    // darker elsewhere), the skin is warmer than the clay, the eye has a dark pupil in the
    // middle and a pale sclera around it.
    EXPECT_GT(meanDifference(clay.image, metal.image), 4.0);
    EXPECT_GT(meanDifference(clay.image, skin.image), 2.0);
    EXPECT_GT(meanDifference(skin.image, eye.image), 4.0);
    const QColor pupil = meanColorInside(eye.image, 0.06), skinMid = meanColorInside(skin.image, 0.3), clayMid = meanColorInside(clay.image, 0.3);
    EXPECT_LT(pupil.value(), 60) << "the pupil is black";
    EXPECT_GT(skinMid.red(), skinMid.blue() + 20) << "skin is warm";
    EXPECT_GT(skinMid.red() - skinMid.blue(), clayMid.red() - clayMid.blue());
    int sclera = 0;
    for (int y = 0; y < size.height(); ++y) {
        for (int x = 0; x < size.width(); ++x) {
            const QColor c = eye.image.pixelColor(x, y);
            if (c.value() > 150 && c.saturation() < 60) ++sclera;
        }
    }
    EXPECT_GT(sclera, size.width() * size.height() / 20) << "the sclera is pale";
    // The pre-integrated profile wraps the terminator: the two differ (and the scattered one
    // lights more of the sphere, redder), but not wildly.
    const double sss = meanDifference(scattered.image, lambert.image);
    EXPECT_GT(sss, 0.2);
    EXPECT_LT(sss, 40.0);
    auto litPixels = [&](const QImage& image) {
        int lit = 0;
        for (int y = 0; y < image.height(); ++y) {
            for (int x = 0; x < image.width(); ++x) {
                if (image.pixelColor(x, y).red() > 165) ++lit;  // the backdrop is darker than that
            }
        }
        return lit;
    };
    EXPECT_GT(litPixels(scattered.image), litPixels(lambert.image));
    const QColor wrapped = meanColorInside(scattered.image, 0.6), plain = meanColorInside(lambert.image, 0.6);
    EXPECT_GE(wrapped.red() - wrapped.blue(), plain.red() - plain.blue() - 3);
    EXPECT_TRUE(scattered.image.save(QDir::temp().filePath(QStringLiteral("groomlab_app_surface_sss.png"))));
    EXPECT_TRUE(lambert.image.save(QDir::temp().filePath(QStringLiteral("groomlab_app_surface_lambert.png"))));
    EXPECT_TRUE(clay.image.save(QDir::temp().filePath(QStringLiteral("groomlab_app_surface_clay.png"))));
    EXPECT_TRUE(skin.image.save(QDir::temp().filePath(QStringLiteral("groomlab_app_surface_skin.png"))));
    EXPECT_TRUE(eye.image.save(QDir::temp().filePath(QStringLiteral("groomlab_app_surface_eye.png"))));
    EXPECT_TRUE(metal.image.save(QDir::temp().filePath(QStringLiteral("groomlab_app_surface_metal.png"))));

    // With the sample groom over it: the scalp is a surface too, and the hair draws over the sphere.
    auto doc = GroomDocument::open(kSampleGroom);
    ViewRequest request;
    request.density = 0.3f;
    const ViewResult& view = doc->evaluate(request);
    ASSERT_TRUE(view.strands);
    DisplaySettings settings;
    renderer.setFrame(doc->updateDisplay(settings));
    renderer.setScalp(doc->file().data.scalp);
    renderer.setHairStrands(view.strands, view.versions.strands);
    sphere->material = surfacePreset("skin");
    renderer.propsChanged();
    OrbitCamera all;
    Bounds both = doc->sceneBounds();
    both.lo = {std::min(both.lo.x, b.lo.x), std::min(both.lo.y, b.lo.y), std::min(both.lo.z, b.lo.z)};
    both.hi = {std::max(both.hi.x, b.hi.x), std::max(both.hi.y, b.hi.y), std::max(both.hi.z, b.hi.z)};
    all.frame(both);
    const OffscreenResult together = renderOffscreen(renderer, all, size, 2, rhi.get());
    ASSERT_FALSE(together.image.isNull());
    EXPECT_GE(renderer.lookdevRenderer().lastSurfaceDraws(), 2);
    EXPECT_GE(renderer.lookdevRenderer().lastHairDraws(), 3);
    EXPECT_TRUE(together.image.save(QDir::temp().filePath(QStringLiteral("groomlab_app_surface_groom.png"))));
    renderer.releaseResources();
}

// The luminance (0..1, of the sRGB values) of a pixel.
double luminanceAt(const QImage& image, int x, int y) {
    const QColor c = image.pixelColor(x, y);
    return 0.2126 * c.redF() + 0.7152 * c.greenF() + 0.0722 * c.blueF();
}

// Along the row `y`, the first and last column whose luminance is above `threshold` (the
// object against a darker backdrop); -1 when none.
std::pair<int, int> litSpan(const QImage& image, int y, double threshold) {
    int first = -1, last = -1;
    for (int x = 0; x < image.width(); ++x) {
        if (luminanceAt(image, x, y) <= threshold) continue;
        if (first < 0) first = x;
        last = x;
    }
    return {first, last};
}

// A plain sphere (latitude-longitude, smooth normals from the mesh) of `radius` at the origin.
Surface makeSphereSurface(float radius, const SurfaceMaterial& material, int rings = 48, int segments = 64) {
    std::vector<Vec3f> points;
    std::vector<ScalpMesh::Triangle> triangles;
    for (int r = 0; r <= rings; ++r) {
        const float theta = 3.14159265f * r / rings;
        for (int q = 0; q <= segments; ++q) {
            const float phi = 2.0f * 3.14159265f * q / segments;
            points.push_back(Vec3f{std::sin(theta) * std::cos(phi), std::cos(theta), std::sin(theta) * std::sin(phi)} * radius);
        }
    }
    const int cols = segments + 1;
    for (int r = 0; r < rings; ++r) {
        for (int q = 0; q < segments; ++q) {
            const std::int32_t a = r * cols + q, b = a + 1, c = (r + 1) * cols + q, d = c + 1;
            if (r > 0) triangles.push_back({a, c, b});
            if (r + 1 < rings) triangles.push_back({b, c, d});
        }
    }
    Surface s;
    s.name = "sphere";
    s.mesh = std::make_shared<ScalpMesh>(std::move(points), std::move(triangles));
    s.material = material;
    return s;
}

TEST(AppRender, SkinStaysBrightToTheSilhouetteAndScatters) {
    std::unique_ptr<QRhi> rhi = createOffscreenRhi();
    ASSERT_TRUE(rhi);
    // A skin sphere the size of a nose (1 cm) under the studio lights, seen head-on, over a
    // black backdrop so the silhouette is plain.
    auto sphere = std::make_shared<Surface>(makeSphereSurface(1.0f, surfacePreset("skin")));
    GroomRenderer renderer;
    renderer.setProps({sphere});
    LightingSetup lighting = lightingPreset("studio");
    lighting.quality = Quality::Interactive;
    lighting.background = Background::Solid;
    lighting.backgroundColor = {0.0f, 0.0f, 0.0f};
    renderer.setLighting(lighting);
    renderer.setLookdev(true);
    OrbitCamera camera;
    Bounds b;
    b.valid = true;
    b.lo = {-1.0f, -1.0f, -1.0f};
    b.hi = {1.0f, 1.0f, 1.0f};
    camera.frame(b);
    camera.orbit(-camera.yaw(), -camera.pitch());
    const QSize size(240, 240);
    const OffscreenResult screen = renderOffscreen(renderer, camera, size, 1, rhi.get());
    const bool lookdev = renderer.lookdevActive();
    EXPECT_EQ(lookdev, LookdevRenderer::supported(rhi.get()));
    // The same under the key alone (a real terminator, nothing lighting the far side) with
    // and without any scattering (plain Lambert), refined frames of each (the blur's taps
    // jittered, the shadow filter rotated) for the terminator's profile.
    LightingSetup key = lightingPreset("key");
    key.quality = Quality::Refine;
    renderer.setLighting(key);
    const OffscreenResult screenRefined = renderOffscreen(renderer, camera, size, 8, rhi.get());
    ASSERT_TRUE(setSurfaceParam(sphere->material, "sssStrength", "0"));
    renderer.propsChanged();
    const OffscreenResult lambertRefined = renderOffscreen(renderer, camera, size, 8, rhi.get());
    if (rhi->backend() == QRhi::Null) {
        renderer.releaseResources();
        GTEST_SKIP() << "no GPU device: the pixels are not checked";
    }
    ASSERT_TRUE(lookdev);
    for (const OffscreenResult* r : {&screen, &screenRefined, &lambertRefined}) ASSERT_FALSE(r->image.isNull());
    EXPECT_TRUE(screen.image.save(QDir::temp().filePath(QStringLiteral("groomlab_app_skin_sphere.png"))));
    EXPECT_TRUE(screenRefined.image.save(QDir::temp().filePath(QStringLiteral("groomlab_app_skin_sphere_key.png"))));
    EXPECT_TRUE(lambertRefined.image.save(QDir::temp().filePath(QStringLiteral("groomlab_app_skin_sphere_key_lambert.png"))));

    // No dark rim: a few pixels inside the silhouette the skin keeps a good share of the
    // center's luminance on every side (the key lights it from the upper left, the fill and
    // the environment the rest), where a diffuse weighed by Schlick's Fresnel of the macro
    // normal, or a normal flipped at the silhouette, went black.
    const int cy = size.height() / 2, cx = size.width() / 2;
    const double center = luminanceAt(screen.image, cx, cy);
    EXPECT_GT(center, 0.3);
    const auto row = litSpan(screen.image, cy, 0.02);
    ASSERT_GT(row.first, 0);
    ASSERT_LT(row.second, size.width() - 1);
    EXPECT_GT(row.second - row.first, size.width() / 2) << "the sphere fills the middle";
    const double left = luminanceAt(screen.image, row.first + 3, cy), right = luminanceAt(screen.image, row.second - 3, cy);
    EXPECT_GT(left, 0.45 * center) << "the lit side's rim";
    EXPECT_GT(right, 0.2 * center) << "the far side's rim";
    int top = -1, bottom = -1;
    for (int y = 0; y < size.height(); ++y) {
        if (luminanceAt(screen.image, cx, y) <= 0.02) continue;
        if (top < 0) top = y;
        bottom = y;
    }
    ASSERT_GT(top, 0);
    EXPECT_GT(luminanceAt(screen.image, cx, top + 3), 0.45 * center) << "the top's rim";
    EXPECT_GT(luminanceAt(screen.image, cx, bottom - 3), 0.2 * center) << "the bottom's rim";

    // Scattering widens the terminator: along the row through the center (the key lights
    // the sphere from the upper left), Lambert's luminance is gone (below 5 percent of its
    // peak) well before the far rim, the scattered one's reaches further, with more light on
    // the far side, and warm (the red travels furthest).
    auto darkFrom = [&](const QImage& image) {
        double peak = 0.0;
        int peakAt = row.first;
        for (int x = row.first; x <= row.second; ++x) {
            const double l = luminanceAt(image, x, cy);
            if (l > peak) {
                peak = l;
                peakAt = x;
            }
        }
        for (int x = peakAt; x <= row.second; ++x) {
            if (luminanceAt(image, x, cy) <= 0.05 * peak) return x;
        }
        return row.second + 1;
    };
    const int scattered = darkFrom(screenRefined.image), plain = darkFrom(lambertRefined.image);
    EXPECT_LT(plain, row.second - 5) << "Lambert's far side is dark";
    EXPECT_GT(scattered, plain + 5) << "the scattered light reaches further";
    const int farX = row.second - 10;
    const QColor farScattered = screenRefined.image.pixelColor(farX, cy), farPlain = lambertRefined.image.pixelColor(farX, cy);
    EXPECT_GT(farScattered.red(), farPlain.red() + 5) << "light reaches the far side";
    EXPECT_GT(farScattered.red(), farScattered.blue() + 5) << "and it is warm";
    EXPECT_GT(meanDifference(screenRefined.image, lambertRefined.image), 1.0);
    renderer.releaseResources();
}

// ---- Timeline, cards and exports --------------------------------------------------------------

// The sample groom with an animated scalp cache beside it: the scalp slides along x by
// 0.1 a frame over `frames` frames at 24 fps from frame 1 (as Maya's AbcExport numbers them),
// with a rest pose (its first frame) so followScalp has something to follow. Returns the
// groom file's path; the cache sits beside it.
std::string animatedSampleGroom(const std::string& stem, int frames) {
    const std::string cache = tempPath(stem + ".scalp.abc"), groom = tempPath(stem + ".groom");
    GroomFile file = readGroomFile(kSampleGroom);
    const MeshPtr scalp = file.data.scalp;
    {
        AlembicMeshWriter writer(cache, "head", 1.0 / 24.0, 1.0 / 24.0);
        for (int f = 0; f < frames; ++f) {
            std::vector<Vec3f> points = scalp->points();
            for (Vec3f& p : points) p.x += 0.1f * static_cast<float>(f);
            writer.writeFrame(scalp->withPoints(std::move(points)));
        }
        writer.close();
    }
    auto rest = std::make_shared<ScalpMesh>(*scalp);
    rest->setRestPoints(scalp->points());
    file.data.scalp = rest;
    file.scalpCache.file = cache;
    file.scalpCache.object = "head";
    writeGroomFile(groom, file);
    return groom;
}

TEST(AppTimeline, PlaybackClockSkipsFramesAndLoops) {
    PlaybackClock clock;
    PlaybackRange range;
    range.first = 1.0;
    range.last = 10.0;
    range.fps = 24.0;
    clock.start(range, 3.0, 100.0);
    EXPECT_TRUE(clock.playing());
    EXPECT_DOUBLE_EQ(clock.frameAt(100.0), 3.0);
    EXPECT_DOUBLE_EQ(clock.frameAt(100.0 + 2.0 / 24.0 + 1e-4), 5.0);
    // A slow groom: the clock has moved on, whatever was shown meanwhile (frames dropped).
    EXPECT_DOUBLE_EQ(clock.frameAt(100.0 + 7.0 / 24.0 + 1e-4), 10.0);
    bool finished = false;
    EXPECT_DOUBLE_EQ(clock.frameAt(100.0 + 8.0 / 24.0 + 1e-4, &finished), 1.0);  // around
    EXPECT_FALSE(finished);
    EXPECT_DOUBLE_EQ(clock.frameAt(100.0 + 20.0 / 24.0 + 1e-4), 3.0);
    range.loop = false;
    clock.start(range, 9.0, 0.0);
    EXPECT_DOUBLE_EQ(clock.frameAt(1.0, &finished), 10.0);
    EXPECT_TRUE(finished);
    clock.start(range, 50.0, 0.0);  // clamped into the range
    EXPECT_DOUBLE_EQ(clock.frameAt(0.0), 10.0);
}

TEST(AppTimeline, AnimatedScalpCacheEvaluatesAsTheSceneDoes) {
    const std::string path = animatedSampleGroom("groomlab_app_timeline", 24);
    auto doc = GroomDocument::open(path);
    ASSERT_TRUE(doc);
    EXPECT_TRUE(doc->animated());
    double first = 0.0, last = 0.0;
    ASSERT_TRUE(doc->cacheFrameRange(&first, &last));
    EXPECT_DOUBLE_EQ(first, 1.0);
    EXPECT_DOUBLE_EQ(last, 24.0);
    EXPECT_NEAR(doc->frame(), 1.0, 1e-9) << "opens at the range's start";
    EXPECT_DOUBLE_EQ(doc->framesPerSecond(), 24.0);

    // The document at a frame gives what a direct evaluation of the scene at that time gives.
    GroomScene direct(path);
    ViewRequest request;
    for (const double frame : {1.0, 12.0, 24.0}) {
        doc->setFrame(frame);
        const ViewResult& view = doc->evaluate(request);
        ASSERT_TRUE(view.strands) << (view.errors.empty() ? "" : view.errors.front());
        const StrandsPtr expected = direct.evaluate(frame / 24.0);
        ASSERT_TRUE(expected);
        ASSERT_EQ(view.strands->strandCount(), expected->strandCount());
        ASSERT_EQ(view.strands->pointCount(), expected->pointCount());
        for (std::size_t i = 0; i < expected->pointCount(); i += 97) {
            EXPECT_NEAR(view.strands->positions()[i].x, expected->positions()[i].x, 1e-5f) << "frame " << frame;
            EXPECT_NEAR(view.strands->positions()[i].y, expected->positions()[i].y, 1e-5f);
        }
        // The scalp of that frame: slid by 0.1 a frame from the file's.
        ASSERT_TRUE(view.scalp);
        EXPECT_NEAR(view.scalp->points()[0].x, doc->file().data.scalp->points()[0].x + 0.1f * static_cast<float>(frame - 1.0), 1e-4f);
    }
    // Frames apart move the strands (the roots ride the scalp).
    doc->setFrame(1.0);
    const float x1 = doc->evaluate(request).strands->positions()[0].x;
    doc->setFrame(24.0);
    const float x24 = doc->evaluate(request).strands->positions()[0].x;
    EXPECT_NEAR(x24 - x1, 2.3f, 0.05f);

    // Following the scalp: on, kept in the file, the strands still ride along.
    EXPECT_FALSE(doc->followScalp());
    doc->setFollowScalp(true);
    EXPECT_TRUE(doc->dirty());
    const ViewResult& followed = doc->evaluate(request);
    ASSERT_TRUE(followed.strands) << (followed.errors.empty() ? "" : followed.errors.front());
    EXPECT_TRUE(doc->session().following());
    EXPECT_NEAR(followed.strands->positions()[0].x, x24, 0.05f);
    const std::string saved = tempPath("groomlab_app_timeline_saved.groom");
    doc->save(saved);
    EXPECT_TRUE(GroomDocument::open(saved)->followScalp());
    std::remove(saved.c_str());

    // A new groom on the animated cache alone: the first frame's mesh as the scalp, the cache
    // as the scalp cache, the timeline spanning it.
    auto fresh = GroomDocument::newOnAlembic(tempPath("groomlab_app_timeline.scalp.abc"));
    ASSERT_TRUE(fresh);
    EXPECT_TRUE(fresh->animated());
    EXPECT_EQ(fresh->file().scalpCache.object, "headShape");  // the mesh's own name in the cache
    ASSERT_TRUE(fresh->cacheFrameRange(&first, &last));
    EXPECT_DOUBLE_EQ(last, 24.0);
    fresh->setFrame(5.0);
    const ViewResult& freshView = fresh->evaluate(request);
    EXPECT_TRUE(freshView.strands || !freshView.errors.empty());
    EXPECT_NEAR(freshView.scalp->points()[0].x, doc->file().data.scalp->points()[0].x + 0.4f, 1e-4f);
    const std::string freshPath = tempPath("groomlab_app_timeline_fresh.groom");
    fresh->save(freshPath);
    EXPECT_TRUE(GroomDocument::open(freshPath)->animated());
    std::remove(freshPath.c_str());
    doc.reset();
    fresh.reset();
    std::remove(path.c_str());
    std::remove(tempPath("groomlab_app_timeline.scalp.abc").c_str());
}

TEST(AppTimeline, ASimulateNodeStepsLiveFillsBakesAndSavesItsBake) {
    const std::string path = animatedSampleGroom("groomlab_app_sim", 24);
    auto doc = GroomDocument::open(path);
    ASSERT_TRUE(doc);
    EXPECT_TRUE(doc->simulateNodes().empty());
    // A Simulate node between the guides and Interpolate (as the node graph wires one).
    const std::string guides = nodeOfType(doc->nodes(), "Guides").name, interpolate = nodeOfType(doc->nodes(), "Interpolate").name;
    std::string sim;
    ASSERT_EQ(doc->addNode("Simulate", 0.0f, 0.0f, &sim), "");
    ASSERT_EQ(doc->connectNodes(guides, "", sim, "guides"), "");
    ASSERT_EQ(doc->connectNodes(sim, "", interpolate, "guides"), "");
    ASSERT_EQ(doc->simulateNodes(), std::vector<std::string>{sim});
    EXPECT_TRUE(doc->animated());

    // Playing steps the frames it shows (live), scrubbing back reads them.
    ViewRequest request;
    std::vector<std::vector<Vec3f>> played;
    for (int f = 1; f <= 8; ++f) {
        doc->setFrame(f);
        const ViewResult& view = doc->evaluate(request);
        ASSERT_TRUE(view.strands) << (view.errors.empty() ? "" : view.errors.front());
        played.push_back(view.strands->positions());
    }
    ASSERT_EQ(doc->simulationStatus().size(), 1u);
    EXPECT_EQ(doc->simulationStatus()[0].ranges, (std::vector<std::pair<int, int>>{{1, 8}}));
    doc->setFrame(3);
    EXPECT_EQ(doc->evaluate(request).strands->positions(), played[2]) << "scrubbing back";

    // Simulate Range fills the cache to the end, frame by frame.
    int reported = 0;
    const SimRun run = doc->simulateTo(24, [&](int done, int count) {
        reported = done;
        EXPECT_EQ(count, 16);
        return true;
    });
    EXPECT_TRUE(run.errors.empty());
    EXPECT_EQ(reported, 16);
    EXPECT_EQ(doc->simulationStatus()[0].ranges, (std::vector<std::pair<int, int>>{{1, 24}}));

    // Bake: the frames beside the groom, read from there, named by the file once saved.
    const std::vector<std::string> files = doc->bakeSimulation(24);
    ASSERT_EQ(files.size(), 1u);
    EXPECT_TRUE(std::filesystem::exists(std::filesystem::u8path(files[0])));
    EXPECT_EQ(files[0], doc->simulationCachePath(sim));
    EXPECT_TRUE(doc->simulationStatus()[0].baked);
    EXPECT_TRUE(doc->dirty());
    doc->setFrame(12);
    const std::vector<Vec3f> atTwelve = doc->evaluate(request).strands->positions();
    const std::string saved = tempPath("groomlab_app_sim_saved.groom");
    doc->save(saved);
    {
        auto reopened = GroomDocument::open(saved);
        ASSERT_EQ(reopened->simulationBakes().count(sim), 1u);
        reopened->setFrame(12);
        const std::vector<Vec3f> read = reopened->evaluate(request).strands->positions();
        ASSERT_EQ(read.size(), atTwelve.size());
        float worst = 0.0f;
        for (std::size_t i = 0; i < read.size(); ++i) worst = std::max(worst, (read[i] - atTwelve[i]).length());
        EXPECT_LT(worst, 1e-4f);
        const SimStatus status = reopened->simulationStatus()[0];
        EXPECT_TRUE(status.baked);
        EXPECT_EQ(status.frames, 0u) << "the bake was simulated again";
    }
    // Clear: the frames and the bake dropped, the next frame simulates again.
    doc->invalidateSimulation();
    EXPECT_TRUE(doc->simulationBakes().empty());
    EXPECT_FALSE(doc->simulationStatus()[0].baked);
    EXPECT_EQ(doc->simulationStatus()[0].frames, 0u);
    doc.reset();
    std::error_code ec;
    std::filesystem::remove_all(std::filesystem::u8path(files[0]).parent_path(), ec);
    std::remove(saved.c_str());
    std::remove(path.c_str());
    std::remove(tempPath("groomlab_app_sim.scalp.abc").c_str());
}

// A .glb's JSON chunk (the header checked), for the cards export test.
json glbJson(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    std::vector<char> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (bytes.size() < 20 || std::string(bytes.data(), 4) != "glTF") throw std::runtime_error("not a glb");
    std::uint32_t chunkLength = 0;
    std::memcpy(&chunkLength, bytes.data() + 12, 4);
    if (std::string(bytes.data() + 16, 4) != "JSON") throw std::runtime_error("no JSON chunk");
    return json::parse(std::string(bytes.data() + 20, chunkLength));
}

TEST(AppExports, EveryTargetWritesAReadableFile) {
    const std::string path = animatedSampleGroom("groomlab_app_exports", 3);
    auto doc = GroomDocument::open(path);
    ASSERT_TRUE(doc);
    ExportOptions options;
    options.frames.first = 1.0;
    options.frames.last = 3.0;
    EXPECT_EQ(options.frames.count(), 3u);
    std::vector<std::pair<std::size_t, std::size_t>> progress;
    auto onProgress = [&](std::size_t done, std::size_t count) {
        progress.emplace_back(done, count);
        return true;
    };

    // Alembic curves: three frames, the roots sliding with the scalp, UVs from it.
    const std::string abc = tempPath("groomlab_app_export_anim.abc");
    const ExportResult alembic = doc->exportAlembic(abc, options, onProgress);
    EXPECT_EQ(alembic.frames, 3u);
    EXPECT_FALSE(alembic.cancelled);
    EXPECT_EQ(alembic.files, std::vector<std::string>{abc});
    EXPECT_EQ(progress.size(), 3u);
    EXPECT_EQ(progress.back(), std::make_pair(std::size_t{3}, std::size_t{3}));
    const auto frames = readAlembicCurves(abc);
    ASSERT_EQ(frames.size(), 3u);
    EXPECT_EQ(frames[0].strands.strandCount(), alembic.strands);
    EXPECT_NEAR(frames[0].seconds, 1.0 / 24.0, 1e-9);
    EXPECT_NEAR(frames[2].strands.positions()[0].x - frames[0].strands.positions()[0].x, 0.2f, 0.02f);
    EXPECT_FALSE(frames[0].uvs.empty());
    const Vec3f firstRoot = frames[0].strands.positions()[0];  // frame 1, the app's space (Y up, cm)
    std::remove(abc.c_str());

    // Cancelled after the first frame: a one-frame file.
    const std::string shortAbc = tempPath("groomlab_app_export_short.abc");
    const ExportResult cancelled = doc->exportAlembic(shortAbc, options, [](std::size_t, std::size_t) { return false; });
    EXPECT_TRUE(cancelled.cancelled);
    EXPECT_EQ(cancelled.frames, 1u);
    EXPECT_EQ(readAlembicCurves(shortAbc).size(), 1u);
    std::remove(shortAbc.c_str());

    // USD: a text layer with BasisCurves and the three frames' samples.
    const std::string usda = tempPath("groomlab_app_export.usda");
    const ExportResult usd = doc->exportUsd(usda, options);
    EXPECT_EQ(usd.frames, 3u);
    {
        std::ifstream in(usda);
        const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        EXPECT_NE(text.find("def BasisCurves"), std::string::npos);
        EXPECT_NE(text.find("timeSamples"), std::string::npos);
        EXPECT_NE(text.find("primvars:st"), std::string::npos);
        EXPECT_NE(text.find("3:"), std::string::npos) << "the third frame's sample";
        // The versioned layout (web/content/docs/usd-layout.mdx), in the groom file's units.
        EXPECT_NE(text.find("int layoutVersion = " + std::to_string(kUsdLayoutVersion)), std::string::npos);
        EXPECT_NE(text.find("metersPerUnit = 0.01\n"), std::string::npos);
        EXPECT_NE(text.find("upAxis = \"Y\"\n"), std::string::npos);
    }
    std::remove(usda.c_str());

    // Unreal's groom: the strands with the primary guides as its own object.
    const std::string unreal = tempPath("groomlab_app_export_unreal.abc");
    options.frames.last = 1.0;
    const ExportResult ue = doc->exportUnrealGroom(unreal, options);
    EXPECT_EQ(ue.frames, 1u);
    EXPECT_GT(ue.strands, 0u);
    const StrandSet ueStrands = AlembicCurvesReader(unreal, "strands").at(1.0 / 24.0);
    EXPECT_EQ(ueStrands.strandCount(), ue.strands);
    const StrandSet ueGuides = AlembicCurvesReader(unreal, "guides").at(1.0 / 24.0);
    EXPECT_GT(ueGuides.strandCount(), 0u) << "the sample's primary guides";
    EXPECT_LT(ueGuides.strandCount(), ueStrands.strandCount());
    // In Unreal's space (Z up, left-handed, cm), as its FBX importer brings in the app's Y-up
    // scalp: (x, y, z) -> (x, z, y), so the importer's defaults line the groom up with it.
    EXPECT_NEAR(ueStrands.positions()[0].x, firstRoot.x, 1e-5f);
    EXPECT_NEAR(ueStrands.positions()[0].y, firstRoot.z, 1e-5f);
    EXPECT_NEAR(ueStrands.positions()[0].z, firstRoot.y, 1e-5f);
    std::remove(unreal.c_str());

    // Unity's strands: root UVs per point, every root width positive, resampled to 8 points.
    const std::string unity = tempPath("groomlab_app_export_unity.abc");
    options.pointsPerStrand = 8;
    const ExportResult un = doc->exportUnityStrands(unity, options);
    EXPECT_EQ(un.frames, 1u);
    const auto unityFrames = readAlembicCurves(unity, "groomlab_app_export_unity");
    ASSERT_EQ(unityFrames.size(), 1u);
    const StrandSet& us = unityFrames[0].strands;
    EXPECT_EQ(us.strandCount(), un.strands);
    EXPECT_EQ(us.pointCount(), us.strandCount() * 8);
    EXPECT_EQ(unityFrames[0].uvs.size(), us.pointCount()) << "UVs per point";
    for (std::size_t s = 0; s < us.strandCount(); ++s) EXPECT_GT(us.widths()[us.offsets()[s]], 0.0f);
    std::remove(unity.c_str());
    // The Maya host's options (engine_export.h) reach the app's exports: the writer refuses what
    // it refuses there (a barb density of 0), and the feather options write.
    options.barbDensity = 0.0f;
    EXPECT_THROW(doc->exportUnityStrands(unity, options), std::runtime_error);
    options.barbDensity = 0.5f;
    options.barbWidthScale = 2.0f;
    options.barbPoints = 4;
    options.featherGuides = true;
    options.shafts = false;
    EXPECT_EQ(doc->exportUnityStrands(unity, options).frames, 1u);
    EXPECT_EQ(doc->exportUnrealGroom(unreal, options).frames, 1u);
    std::remove(unity.c_str());
    std::remove(unreal.c_str());
    doc.reset();
    std::remove(path.c_str());
    std::remove(tempPath("groomlab_app_exports.scalp.abc").c_str());
}

TEST(AppCards, BuildBakeAndExportCards) {
    auto doc = GroomDocument::open(kSampleGroom);
    ASSERT_TRUE(doc);
    EXPECT_TRUE(doc->cardsNodes().empty());
    EXPECT_THROW(doc->bakeCardsInMemory(64), std::runtime_error);
    std::string cards;
    ASSERT_EQ(doc->addCardsNode(&cards), "");
    EXPECT_EQ(doc->cardsNodes(), std::vector<std::string>{cards});
    EXPECT_EQ(doc->undoText(), "Undo add " + cards);
    // Wired between what fed the output and the output.
    const std::string output = nodeOfType(doc->nodes(), "Output").name;
    bool feedsOutput = false, fed = false;
    for (const GroomDocument::ConnectionInfo& c : doc->connections()) {
        if (c.from == cards && c.to == output) feedsOutput = true;
        if (c.to == cards) fed = true;
    }
    EXPECT_TRUE(feedsOutput);
    EXPECT_TRUE(fed);

    // The view builds the cards (laid out in one atlas) beside the strands.
    ViewRequest request;
    request.wantCards = true;
    const ViewResult& view = doc->evaluate(request);
    ASSERT_TRUE(view.strands) << (view.errors.empty() ? "" : view.errors.front());
    ASSERT_TRUE(view.cards);
    EXPECT_GT(view.cards->cards.size(), 0u);
    EXPECT_GT(view.cards->triangleCount(), 0u);
    ASSERT_EQ(view.cardNodes.size(), 1u);
    EXPECT_EQ(view.cardNodes[0].node, cards);
    DisplaySettings settings;
    settings.cardsView = true;
    settings.drawCards = true;
    const DisplayFrame& frame = doc->updateDisplay(settings);
    EXPECT_TRUE(frame.item(DisplayItemId::Cards).shown);
    EXPECT_FALSE(frame.item(DisplayItemId::Strands).shown) << "the cards stand in for the strands";

    // The bake: the seven maps as TGA (Unreal's layout, the Maya host's names), the atlas and
    // the viewport's preview, 64 px square; the session takes it.
    const std::string dir = tempPath("groomlab_app_cards_bake");
    const CardBakeFiles bake = doc->bakeCards(dir, "sample", 64, 1.0f);
    ASSERT_EQ(bake.paths.size(), 9u);
    EXPECT_EQ(bake.size, 64);
    EXPECT_EQ(bake.cards, view.cards->cards.size());
    for (const char* map : {"Depth", "Coverage", "Tangent", "Attribute", "Color", "AO", "Normal"}) {
        const std::string path = dir + "/sample_" + map + ".tga";
        EXPECT_TRUE(std::filesystem::exists(path)) << path;
        std::ifstream in(path, std::ios::binary);
        unsigned char header[18] = {};
        in.read(reinterpret_cast<char*>(header), 18);
        EXPECT_EQ(header[12] | (header[13] << 8), 64) << map << " width";
        EXPECT_EQ(header[14] | (header[15] << 8), 64) << map << " height";
    }
    EXPECT_TRUE(std::filesystem::exists(bake.atlas));
    EXPECT_NO_THROW(CardAtlas::fromJson([&] {
        std::ifstream in(bake.atlas);
        return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }()));
    const QImage preview(QString::fromStdString(bake.preview));
    ASSERT_FALSE(preview.isNull());
    EXPECT_EQ(preview.size(), QSize(64, 64));
    EXPECT_TRUE(preview.hasAlphaChannel());
    ASSERT_TRUE(doc->lastBake());
    EXPECT_EQ(doc->lastBake()->stamp, bake.stamp);
    EXPECT_FALSE(bake.stamp.empty());
    // Not stale right after; stale once the cards change.
    doc->evaluate(request);
    EXPECT_FALSE(doc->bakeStale());
    EXPECT_EQ(doc->setParam(cards, "widthScale", 1.7), "");
    doc->evaluate(request);
    EXPECT_TRUE(doc->bakeStale());
    EXPECT_EQ(doc->undo(), "");

    // The viewport's textured cards: a surface of the card mesh with the preview as albedo.
    const SurfacePtr surface = makeCardSurface(*view.cards, bake.preview, 0.4f);
    ASSERT_TRUE(surface && surface->mesh);
    EXPECT_EQ(surface->mesh->triangleCount(), view.cards->triangleCount());
    EXPECT_TRUE(surface->mesh->hasUVs());
    EXPECT_EQ(surface->cornerNormals.size(), surface->mesh->triangleCount() * 3);
    EXPECT_EQ(surface->material.albedoMap, bake.preview);
    // Its UVs are the cards' as a mesh's are (v up): the surface shader turns them to the
    // image's rows itself, so a flipped v showed every card another place in the atlas.
    const auto& corners = surface->mesh->cornerUVs();
    ASSERT_GE(corners.size(), 3u);
    for (std::size_t k = 0; k < 3; ++k) {
        const std::uint32_t vertex = view.cards->quads[k];
        EXPECT_FLOAT_EQ(corners[k][0], view.cards->u[vertex]);
        EXPECT_FLOAT_EQ(corners[k][1], view.cards->v[vertex]);
    }

    // glTF: the cards with their maps embedded, the hair cap with its, under one root.
    const std::string glb = tempPath("groomlab_app_cards.glb");
    ExportOptions options;
    options.resolution = 64;
    const ExportResult exported = doc->exportCards(glb, options);
    EXPECT_EQ(exported.files, std::vector<std::string>{glb});
    const json g = glbJson(glb);
    ASSERT_EQ(g["meshes"].size(), 2u);
    EXPECT_EQ(g["meshes"][0]["name"], "groomlab_app_cards_cards");
    EXPECT_EQ(g["meshes"][1]["name"], "groomlab_app_cards_scalpCap");
    EXPECT_EQ(g["images"].size(), 4u);  // BaseColor, ORM and Normal for the cards, BaseColor for the cap
    EXPECT_EQ(g["materials"][0]["alphaMode"], "MASK");
    EXPECT_EQ(g["buffers"].size(), 1u);
    std::remove(glb.c_str());
    // The text form writes the .bin and the PNGs beside it.
    const std::string gltf = tempPath("groomlab_app_cards.gltf");
    options.binary = false;
    const ExportResult text = doc->exportCards(gltf, options);
    EXPECT_GE(text.files.size(), 5u);
    EXPECT_EQ(text.files.front(), gltf);
    for (const std::string& f : text.files) {
        EXPECT_TRUE(std::filesystem::exists(f)) << f;
        std::remove(f.c_str());
    }
    std::filesystem::remove_all(dir);
}

TEST(AppCards, BakedCardsAreCutOutByTheirCoverage) {
    // The textured cards (makeCardSurface: the bake's preview as the albedo, the coverage in its
    // alpha) are cut away where the coverage is below the cutoff, as Maya's card shader cuts
    // them: the gaps between a card's strands show the backdrop, not the card's color, which the
    // same cards drawn opaque (cutoff 0) cover.
    std::unique_ptr<QRhi> rhi = createOffscreenRhi();
    ASSERT_TRUE(rhi);
    auto doc = GroomDocument::open(kSampleGroom);
    ASSERT_TRUE(doc);
    std::string cards;
    ASSERT_EQ(doc->addCardsNode(&cards), "");
    ASSERT_EQ(doc->applyCardPreset(cards, "hero"), "");
    ViewRequest request;
    request.wantCards = true;
    const ViewResult& view = doc->evaluate(request);
    ASSERT_TRUE(view.cards && !view.cards->cards.empty()) << (view.errors.empty() ? "" : view.errors.front());
    const std::shared_ptr<const CardMesh> shownCards = view.cards;
    const std::string dir = tempPath("groomlab_app_cards_cutout");
    std::filesystem::remove_all(dir);
    const CardBakeFiles bake = doc->bakeCards(dir, "cutout", 512, 1.0f);
    ASSERT_FALSE(bake.preview.empty());
    const SurfacePtr cut = makeCardSurface(*shownCards, bake.preview, 0.4f);
    EXPECT_EQ(cut->material.alphaCutoff, kCardAlphaCutoff);
    auto opaque = std::make_shared<Surface>(*cut);
    opaque->material.alphaCutoff = 0.0f;

    GroomRenderer renderer;
    renderer.setLookdev(true);
    LightingSetup lighting = lightingPreset("studio");
    lighting.quality = Quality::Interactive;
    lighting.background = Background::Solid;  // the pixels below read a plain backdrop (the app's default is a gradient)
    renderer.setLighting(lighting);
    Bounds b;
    for (const Vec3f& p : shownCards->positions) {
        if (!b.valid) {
            b.lo = b.hi = p;
            b.valid = true;
        }
        b.lo = {std::min(b.lo.x, p.x), std::min(b.lo.y, p.y), std::min(b.lo.z, p.z)};
        b.hi = {std::max(b.hi.x, p.x), std::max(b.hi.y, p.y), std::max(b.hi.z, p.z)};
    }
    OrbitCamera camera;
    camera.frame(b);
    const QSize size(400, 400);
    renderer.setProps({opaque});
    const OffscreenResult whole = renderOffscreen(renderer, camera, size, 1, rhi.get());
    renderer.setProps({cut});
    const OffscreenResult holed = renderOffscreen(renderer, camera, size, 1, rhi.get());
    const bool lookdev = renderer.lookdevActive();
    EXPECT_EQ(lookdev, LookdevRenderer::supported(rhi.get()));
    if (rhi->backend() == QRhi::Null) {
        renderer.releaseResources();
        std::filesystem::remove_all(dir);
        GTEST_SKIP() << "no GPU device: the pixels are not checked";
    }
    ASSERT_TRUE(lookdev);
    ASSERT_FALSE(whole.image.isNull());
    ASSERT_FALSE(holed.image.isNull());
    EXPECT_TRUE(whole.image.save(QDir::temp().filePath(QStringLiteral("groomlab_app_cards_opaque.png"))));
    EXPECT_TRUE(holed.image.save(QDir::temp().filePath(QStringLiteral("groomlab_app_cards_cutout.png"))));
    // The backdrop: a corner (the cards are framed in the middle), solid.
    const QColor background = whole.image.pixelColor(1, 1);
    EXPECT_EQ(holed.image.pixelColor(1, 1), background);
    auto isBackground = [&](const QImage& image, int x, int y) {
        const QColor c = image.pixelColor(x, y);
        return std::abs(c.red() - background.red()) <= 3 && std::abs(c.green() - background.green()) <= 3 &&
               std::abs(c.blue() - background.blue()) <= 3;
    };
    int covered = 0, gaps = 0, strands = 0, outside = 0;
    for (int y = 0; y < size.height(); ++y) {
        for (int x = 0; x < size.width(); ++x) {
            const bool card = !isBackground(whole.image, x, y), shown = !isBackground(holed.image, x, y);
            covered += card ? 1 : 0;
            gaps += card && !shown ? 1 : 0;
            strands += shown ? 1 : 0;
            outside += shown && !card ? 1 : 0;
        }
    }
    std::printf("cards cutout: %d px covered opaque, %d of them cut to the backdrop, %d px of strands, %d outside\n", covered,
                gaps, strands, outside);
    EXPECT_GT(covered, size.width() * size.height() / 50) << "the cards fill some of the view";
    EXPECT_GT(strands, covered / 20) << "the strands on the cards still show";
    EXPECT_GT(gaps, covered / 10) << "the gaps between the strands show the backdrop";
    EXPECT_LT(outside, std::max(strands / 50, 20)) << "nothing shows where the opaque cards are not";
    renderer.releaseResources();
    std::filesystem::remove_all(dir);
}

TEST(AppCards, LevelsOfDetailAreTheCoresPresets) {
    // The Cards panel's Hero, Mid and Low set what Maya's cards_preset sets (the core's table,
    // card_presets.h): every param of the preset, maxCards from its budget, one undo step.
    auto doc = GroomDocument::open(kSampleGroom);
    ASSERT_TRUE(doc);
    std::string cards;
    ASSERT_EQ(doc->addCardsNode(&cards), "");
    EXPECT_EQ(cardNodeKind(doc->graphJson(), cards), "hair");
    for (const CardLodPreset& preset : cardLodPresets()) {
        ASSERT_EQ(doc->applyCardPreset(cards, preset.lod), "");
        EXPECT_EQ(doc->undoText(), "Undo " + preset.lod + " cards on " + cards);
        for (const auto& [name, value] : cardPresetParams(preset.lod, "hair")) {
            const json stored = doc->param(cards, name);
            ASSERT_FALSE(stored.is_null()) << preset.lod << " " << name;
            if (const auto* i = std::get_if<std::int64_t>(&value)) EXPECT_EQ(stored.get<std::int64_t>(), *i) << preset.lod << " " << name;
            if (const auto* d = std::get_if<double>(&value)) EXPECT_DOUBLE_EQ(stored.get<double>(), *d) << preset.lod << " " << name;
        }
        EXPECT_EQ(doc->param(cards, "segments").get<int>(), preset.segments);
        EXPECT_EQ(doc->param(cards, "maxCards").get<int>(), preset.triangles / (2 * preset.segments * preset.widthSegments));
    }
    EXPECT_EQ(doc->param(cards, "segments").get<int>(), 5);  // low, as Maya's
    EXPECT_NE(doc->applyCardPreset(cards, "ultra"), "");
}

TEST(AppCards, SavedGroomNamesItsBakeForAnotherHost) {
    // Saving names the last bake (the file's cardBake: the prefix relative to the groom, the
    // stamp), so Maya's import takes it; opening adopts a named bake with its stamp (not stale),
    // and a bake another host wrote has no preview: one is made from its maps, the app's own.
    auto doc = GroomDocument::open(kSampleGroom);
    ASSERT_TRUE(doc);
    std::string cards;
    ASSERT_EQ(doc->addCardsNode(&cards), "");
    const std::string dir = tempPath("groomlab_app_named_bake");
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    const std::string groom = dir + "/named.groom";
    doc->save(groom);
    const CardBakeFiles bake = doc->bakeCards(doc->defaultBakeDirectory(), doc->defaultBakeName(), 64, 1.0f);
    doc->save(groom);
    const GroomFile file = readGroomFile(groom);
    EXPECT_EQ(std::filesystem::path(file.cardBake.prefix), std::filesystem::path(dir + "/named_cards/named_").lexically_normal());
    EXPECT_EQ(file.cardBake.strands, bake.stamp);
    EXPECT_EQ(file.upAxis, "Y");
    {
        std::ifstream in(groom, std::ios::binary);
        const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        EXPECT_NE(text.find("\"prefix\":\"named_cards/named_\""), std::string::npos);  // relative: it moves with the groom
    }

    const QImage own(QString::fromStdString(bake.preview));
    ASSERT_FALSE(own.isNull());
    std::filesystem::remove(bake.preview);  // as Maya's bake: no preview
    auto reopened = GroomDocument::open(groom);
    ASSERT_TRUE(reopened->lastBake());
    EXPECT_EQ(reopened->lastBake()->stamp, bake.stamp);
    EXPECT_EQ(reopened->lastBake()->size, 64);
    ASSERT_TRUE(std::filesystem::exists(bake.preview));
    EXPECT_EQ(QImage(QString::fromStdString(bake.preview)).convertToFormat(QImage::Format_RGBA8888),
              own.convertToFormat(QImage::Format_RGBA8888));
    ViewRequest request;
    request.wantCards = true;
    reopened->evaluate(request);
    EXPECT_FALSE(reopened->bakeStale());  // the stamp came with the file

    // A bake in a folder beside the groom's (a shared one): stored relative with "..", as Maya's
    // export stores such a path (paths.h portablePath), and found again.
    const std::string shared = dir + "_shared";
    std::filesystem::remove_all(shared);
    doc->bakeCards(shared, "named", 64, 1.0f);
    doc->save(groom);
    {
        std::ifstream in(groom, std::ios::binary);
        const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        EXPECT_NE(text.find("\"prefix\":\"../groomlab_app_named_bake_shared/named_\""), std::string::npos);
    }
    EXPECT_TRUE(std::filesystem::equivalent(std::filesystem::u8path(readGroomFile(groom).cardBake.prefix + "Atlas.json"),
                                            std::filesystem::u8path(shared + "/named_Atlas.json")));
    reopened.reset();
    doc.reset();
    std::filesystem::remove_all(dir);
    std::filesystem::remove_all(shared);
}

TEST(AppDocument, OpensAZUpGroomTurnedYUp) {
    // A Z-up groom file (Blender's) opens turned a quarter about X: the app is Y-up, as Maya.
    GroomFile file = readGroomFile(kSampleGroom);
    convertUpAxis(file, "Z");
    const std::string path = tempPath("groomlab_app_zup.groom");
    writeGroomFile(path, file);
    auto zup = GroomDocument::open(path);
    auto yup = GroomDocument::open(kSampleGroom);
    ASSERT_TRUE(zup && yup);
    const StrandsPtr a = yup->evaluate(ViewRequest{}).strands;
    const StrandsPtr b = zup->evaluate(ViewRequest{}).strands;
    ASSERT_TRUE(a && b);
    ASSERT_GT(a->strandCount(), 100u);
    EXPECT_EQ(a->ids(), b->ids());
    EXPECT_EQ(a->positions(), b->positions());  // turned there and back: exactly
    const std::string saved = tempPath("groomlab_app_zup_saved.groom");
    zup->save(saved);
    EXPECT_EQ(readGroomFile(saved).upAxis, "Y");
    zup.reset();
    std::remove(path.c_str());
    std::remove(saved.c_str());
}

}  // namespace
}  // namespace groomlab::app

// ---- Partial uploads and the GPU look preview ------------------------------------------------------

namespace groomlab::app {
namespace {

using nlohmann::json;

// Frames drawn until the GPU preview's kernels are ready (OpenGL compiles them in the background
// from the first frame, a drag or stroke the CPU's meanwhile; DirectX 11's are at once).
bool framesUntilKernelsReady(GpuLookPreview& gpu, GroomRenderer& renderer, OffscreenView& view, const OrbitCamera& camera) {
    const auto start = std::chrono::steady_clock::now();
    while (!gpu.kernelsReady()) {
        if (std::chrono::steady_clock::now() - start > std::chrono::minutes(5)) return false;
        view.frame(renderer, camera, false);
    }
    return true;
}

// A graph the GPU runs from Interpolate to the output (look_chain.h), on the sample groom's
// scalp and guides: Scatter -> Interpolate (keeping its strands clear of the scalp) -> Noise ->
// Width -> Output.
std::string gpuGraphJson(double density) {
    json graph = {
        {"nodes",
         {{{"name", "scalp"}, {"type", "Scalp"}},
          {{"name", "guides"}, {"type", "Guides"}},
          {{"name", "scatter"}, {"type", "Scatter"}, {"params", {{"density", density}, {"length", 0.8}}}},
          {{"name", "interpolate"}, {"type", "Interpolate"}},
          {{"name", "noise"}, {"type", "Noise"}, {"params", {{"amplitude", 0.15}, {"frequency", 2.0}}}},
          {{"name", "width"}, {"type", "Width"}, {"params", {{"width", 0.01}}}},
          {{"name", "output"}, {"type", "Output"}}}},
        {"connections",
         {{{"from", "scalp"}, {"to", "scatter"}, {"input", "scalp"}},
          {{"from", "scatter"}, {"to", "interpolate"}, {"input", "strands"}},
          {{"from", "guides"}, {"to", "interpolate"}, {"input", "guides"}},
          {{"from", "scalp"}, {"to", "interpolate"}, {"input", "scalp"}},
          {{"from", "interpolate"}, {"to", "noise"}, {"input", "strands"}},
          {{"from", "noise"}, {"to", "width"}, {"input", "strands"}},
          {{"from", "width"}, {"to", "output"}, {"input", "strands"}}}},
        {"output", "output"}};
    return graph.dump();
}

// The view's strands and frame handed to a renderer as the window hands them (reevaluate and
// redisplay): the changes beside the strands, so the renderer may write only what moved.
void feed(GroomRenderer& renderer, GroomDocument& doc, const DisplayFrame& frame) {
    const ViewResult& view = doc.lastView();
    renderer.setScalp(view.scalp ? view.scalp : doc.file().data.scalp);
    renderer.setHairStrands(view.strands, view.versions.strands, HairChanges{view.strandsSerial, view.changesBase, view.strandsChanges});
    renderer.setHairLook(doc.session().hairLook());
    renderer.setFrame(frame);
}

// The share of pixels that differ between two images by more than `tolerance` in a channel.
double pixelsDiffering(const QImage& a, const QImage& b, int tolerance = 8) {
    if (a.size() != b.size() || a.isNull()) return 1.0;
    std::size_t differing = 0;
    for (int y = 0; y < a.height(); ++y) {
        for (int x = 0; x < a.width(); ++x) {
            const QColor p = a.pixelColor(x, y), q = b.pixelColor(x, y);
            if (std::abs(p.red() - q.red()) > tolerance || std::abs(p.green() - q.green()) > tolerance || std::abs(p.blue() - q.blue()) > tolerance) ++differing;
        }
    }
    return static_cast<double>(differing) / (static_cast<double>(a.width()) * a.height());
}

}  // namespace

TEST(AppRender, PartialUploadsDrawTheSameFrameAsWholeOnes) {
    // A brush stroke's dabs change a few strands: one renderer writes only their ranges (the
    // line path's buffers, the lookdev textures), another the whole buffers; both frames are
    // the same pixels, on both paths.
    std::unique_ptr<QRhi> rhi = createOffscreenRhi();
    ASSERT_TRUE(rhi);
    if (rhi->backend() == QRhi::Null) GTEST_SKIP() << "no GPU device: the pixels are not checked";
    for (const bool lookdev : {false, true}) {
        SCOPED_TRACE(lookdev ? "lookdev" : "lines");
        BrushFixture f;
        const QSize size(320, 240);
        OffscreenView view(size, rhi.get());
        ASSERT_TRUE(view.ready());
        GroomRenderer partial, whole;
        whole.setPartialUploads(false);
        for (GroomRenderer* r : {&partial, &whole}) {
            r->setLookdev(lookdev);
            LightingSetup lighting = lightingPreset("studio");
            lighting.quality = Quality::Interactive;
            r->setLighting(lighting);
        }
        DisplaySettings settings;
        auto frames = [&] {
            const DisplayFrame& frame = f.doc->updateDisplay(settings);
            feed(partial, *f.doc, frame);
            feed(whole, *f.doc, frame);
            const QImage a = view.frame(partial, f.camera).image, b = view.frame(whole, f.camera).image;
            EXPECT_FALSE(a.isNull());
            EXPECT_EQ(meanDifference(a, b), 0.0);
            EXPECT_EQ(pixelsDiffering(a, b, 0), 0.0);
            return a;
        };
        const QImage before = frames();
        EXPECT_EQ(partial.lookdevActive(), lookdev && LookdevRenderer::supported(rhi.get()));
        // A comb stroke across the first guide's middle: the press steps the strands back (another
        // layout: whole), the dabs after it move a few strands (partial).
        const HierarchyGuide& g = f.guide(0);
        const QPointF at = f.widgetPoint(g.points[g.points.size() / 2]);
        f.session.settings().setMode("comb");
        f.session.settings().screenRadius = 40.0f;
        ViewRequest request;
        request.density = 0.2f;
        request.sculptDensity = f.session.sculptDensity;
        ASSERT_TRUE(f.session.press(at, 1.0f, 0)) << f.session.lastError();
        f.doc->evaluate(request);
        frames();
        bool partialSeen = false;
        for (int k = 1; k <= 3; ++k) {
            f.session.move(QPointF(at.x() + 10.0 * k, at.y()), 1.0f);
            f.doc->evaluate(request);
            const QImage dab = frames();
            const bool wrotePartial = lookdev && partial.lookdevActive() ? partial.lookdevRenderer().lastUploadPartial() : partial.lastUploadPartial();
            partialSeen = partialSeen || wrotePartial;
            if (k == 3) EXPECT_GT(meanDifference(before, dab), 0.0) << "the dabs moved strands";
        }
        EXPECT_TRUE(partialSeen) << "a dab after the press wrote only the changed strands";
        EXPECT_EQ(f.session.release(), StrokeRelease::Stroke);
        f.doc->evaluate(request);
        frames();
        partial.releaseResources();
        whole.releaseResources();
    }
}

TEST(AppRender, WholeWritesBuiltAheadDrawTheSameFrame) {
    // The window's renderer builds a whole write's buffers off the UI thread (a commit landing:
    // setBuildAhead): until they are built its frames keep the hair held and ask for another;
    // once written, the same pixels as a renderer that built them in the frame.
    std::unique_ptr<QRhi> rhi = createOffscreenRhi();
    ASSERT_TRUE(rhi);
    if (rhi->backend() == QRhi::Null || !LookdevRenderer::supported(rhi.get())) GTEST_SKIP() << "no device for the lookdev path";
    BrushFixture f;
    const QSize size(320, 240);
    OffscreenView view(size, rhi.get());
    ASSERT_TRUE(view.ready());
    GroomRenderer ahead, here;
    ahead.setBuildAhead(true);
    for (GroomRenderer* r : {&ahead, &here}) {
        r->setLookdev(true);
        LightingSetup lighting = lightingPreset("studio");
        lighting.quality = Quality::Interactive;
        r->setLighting(lighting);
    }
    DisplaySettings settings;
    auto frames = [&](bool built) {
        const DisplayFrame& frame = f.doc->updateDisplay(settings);
        feed(ahead, *f.doc, frame);
        feed(here, *f.doc, frame);
        EXPECT_EQ(ahead.lookdevRenderer().hairBuilding(), built) << "a whole write building ahead";
        QImage a = view.frame(ahead, f.camera).image;
        QElapsedTimer clock;
        clock.start();
        while (ahead.lookdevRenderer().hairBuilding() && clock.elapsed() < 20000) {
            EXPECT_TRUE(ahead.wantsAnotherFrame());
            a = view.frame(ahead, f.camera).image;
        }
        EXPECT_FALSE(ahead.lookdevRenderer().hairBuilding());
        const QImage b = view.frame(here, f.camera).image;
        EXPECT_FALSE(a.isNull());
        EXPECT_EQ(meanDifference(a, b), 0.0);
        EXPECT_EQ(pixelsDiffering(a, b, 0), 0.0);
        return a;
    };
    const QImage before = frames(false);  // (the first write: nothing held to draw meanwhile)
    // Another density: another layout, written whole, built ahead.
    ViewRequest request;
    request.density = 0.5f;
    f.doc->evaluate(request);
    const QImage denser = frames(true);
    EXPECT_GT(meanDifference(before, denser), 0.0) << "the hair changed";
    ahead.releaseResources();
    here.releaseResources();
}

TEST(AppGpuPreview, DragFrameMatchesTheCpuCommit) {
    // A Noise drag previewed by the GPU (the kernels writing the point texture, or the line
    // path's buffers) against the same value committed and evaluated on the CPU: nearly the
    // same pixels (float rounding), and both away from the value before. A number (amplitude),
    // a profile's ramp and a seed.
    struct Drag {
        const char* param;
        json before, value;
    };
    const json ramp = json::parse("[[0, 0.15], [0.5, 1.0], [1, 0.3]]");
    const Drag drags[] = {{"amplitude", 0.15, 0.5}, {"profile", json::parse("[[0, 1.0], [1, 1.0]]"), ramp}, {"seed", 0, 9}};
    std::unique_ptr<QRhi> rhi = createOffscreenRhi();
    ASSERT_TRUE(rhi);
    if (rhi->backend() == QRhi::Null || !GpuLookPreview::supported(rhi.get())) GTEST_SKIP() << "no device with a GPU preview engine";
    auto doc = GroomDocument::open(kSampleGroom);
    ASSERT_EQ(doc->replaceGraph(gpuGraphJson(1500.0)), "");
    ViewRequest request;
    request.density = 1.0f;
    ASSERT_TRUE(doc->evaluate(request).strands);
    OrbitCamera camera;
    camera.frame(doc->sceneBounds());
    const QSize size(320, 240);
    OffscreenView view(size, rhi.get());
    ASSERT_TRUE(view.ready());
    DisplaySettings settings;
    for (const bool lookdev : {false, true}) {
        SCOPED_TRACE(lookdev ? "lookdev" : "lines");
        GpuLookPreview gpu;
        gpu.setDocument(doc.get());
        GroomRenderer renderer;
        renderer.setGpuPreview(&gpu);
        renderer.setLookdev(lookdev);
        LightingSetup lighting = lightingPreset("studio");
        lighting.quality = Quality::Interactive;
        // Every frame at full resolution: the adaptive quality draws a stream of changes (frames
        // back to back, each after a change: here the frame before the drag and the drag's) at a
        // scale it picks from the frames' wall-clock times, so on a busy machine the drag's frame
        // came out smaller than the commit's and the two differed by the scale, not the strands.
        lighting.interactiveQuality = InteractiveQuality::Full;
        renderer.setLighting(lighting);
        for (const Drag& d : drags) {
            SCOPED_TRACE(d.param);
            ASSERT_EQ(doc->setParam("noise", d.param, d.before), "");
            doc->evaluate(request);
            doc->display().invalidateAll();
            feed(renderer, *doc, doc->updateDisplay(settings));
            const QImage before = view.frame(renderer, camera).image;
            ASSERT_FALSE(before.isNull());
            ASSERT_TRUE(framesUntilKernelsReady(gpu, renderer, view, camera));
            // The drag (a ramp as the panel's ramp editor sends its keys): the first frame after
            // an upload puts the run off to the next one.
            std::string why;
            const gpu::OverrideValue value =
                d.value.is_number() ? gpu::OverrideValue(d.value.get<double>()) : gpu::OverrideValue(rampFromJson(d.value.dump()));
            ASSERT_TRUE(gpu.drag("noise", d.param, value, &why)) << why;
            EXPECT_TRUE(gpu.pending());
            QImage dragged = view.frame(renderer, camera).image;
            if (!renderer.gpuRanLastFrame()) dragged = view.frame(renderer, camera).image;
            ASSERT_TRUE(renderer.gpuRanLastFrame()) << renderer.gpuWhy();
            EXPECT_FALSE(gpu.pending());
            EXPECT_GE(gpu.runs(), 1u);
            const gpu::GpuTimes times = gpu.gpuTimes(true);
            EXPECT_TRUE(times.valid);
            std::printf("[ %-8s ] GPU drag of %s on %s: %.3f ms on the GPU (noise %.3f, width %.3f, lines %.3f, texture %.3f), %.2f ms "
                        "on the CPU\n",
                        lookdev ? "lookdev" : "lines", d.param, qPrintable(view.frame(renderer, camera, false).api), times.totalMs,
                        times.noiseMs, times.widthMs, times.linesMs, times.textureMs, gpu.lastRunMs());
            // The release: the value committed on the CPU, evaluated, uploaded whole.
            gpu.endDrag();
            ASSERT_EQ(doc->setParam("noise", d.param, d.value), "");
            doc->evaluate(request);
            feed(renderer, *doc, doc->updateDisplay(settings));
            const QImage committed = view.frame(renderer, camera).image;
            ASSERT_FALSE(committed.isNull());
            EXPECT_FALSE(renderer.gpuRanLastFrame());
            const double changed = pixelsDiffering(before, committed), error = pixelsDiffering(dragged, committed);
            const double mean = meanDifference(dragged, committed);
            std::printf("[ %-8s ] %s: pixels differing: the drag's value vs before %.2f %%, the GPU's frame vs the CPU's %.3f %% "
                        "(mean %.3f)\n",
                        lookdev ? "lookdev" : "lines", d.param, changed * 100.0, error * 100.0, mean);
            EXPECT_GT(changed, 0.01) << "the change shows";
            // The lines: the same positions and frames to the bit, the widths to a millionth (the
            // GPU's float arithmetic); width as coverage hashes each fragment against a threshold,
            // so where a fragment's coverage sits at its threshold that millionth flips it whole,
            // in a dense pixel's dozens of fragments now and then: about a percent of the pixels
            // (with width as coverage off, none). The lookdev path: the GPU's frame fits its deep
            // opacity maps to the box the run can reach (lookBounds), the CPU's to the strands'
            // own, so the shadows' texels differ a little in a few percent of the pixels.
            EXPECT_LT(error, lookdev ? 0.03 : 0.02) << "the GPU's strands draw as the CPU's";
            EXPECT_LT(mean, lookdev ? 1.0 : 0.5);
            const QString tag = QStringLiteral("%1_%2").arg(QLatin1String(d.param), QLatin1String(lookdev ? "lookdev" : "lines"));
            EXPECT_TRUE(dragged.save(QDir::temp().filePath(QStringLiteral("groomlab_app_gpu_drag_%1.png").arg(tag))));
            EXPECT_TRUE(committed.save(QDir::temp().filePath(QStringLiteral("groomlab_app_gpu_commit_%1.png").arg(tag))));
            ASSERT_EQ(doc->setParam("noise", d.param, d.before), "");
        }
        renderer.releaseResources();
    }
}

TEST(AppGpuPreview, StrokeDabsRunOnTheGpu) {
    // A stroke's press is the CPU's; the dabs after it evaluate the guides alone (stopAt) and
    // the GPU blends them into the strands it holds: the frame matches the CPU's evaluation of
    // the same dab.
    std::unique_ptr<QRhi> rhi = createOffscreenRhi();
    ASSERT_TRUE(rhi);
    if (rhi->backend() == QRhi::Null || !GpuLookPreview::supported(rhi.get())) GTEST_SKIP() << "no device with a GPU preview engine";
    BrushFixture f;
    ASSERT_EQ(f.doc->replaceGraph(gpuGraphJson(1500.0)), "");
    ViewRequest request;
    request.density = 1.0f;
    request.sculptDensity = f.session.sculptDensity;
    ASSERT_TRUE(f.doc->evaluate(request).strands);
    f.camera.frame(f.doc->sceneBounds());
    f.session.setView(f.camera, f.w, f.h);
    const QSize size(320, 240);
    OffscreenView view(size, rhi.get());
    ASSERT_TRUE(view.ready());
    GpuLookPreview gpu;
    gpu.setDocument(f.doc.get());
    GroomRenderer renderer;
    renderer.setGpuPreview(&gpu);
    renderer.setLookdev(false);
    DisplaySettings settings;
    f.doc->display().invalidateAll();
    feed(renderer, *f.doc, f.doc->updateDisplay(settings));
    view.frame(renderer, f.camera, false);
    ASSERT_TRUE(framesUntilKernelsReady(gpu, renderer, view, f.camera));
    const HierarchyGuide& g = f.guide(0);
    const QPointF at = f.widgetPoint(g.points[g.points.size() / 2]);
    f.session.settings().setMode("comb");
    f.session.settings().screenRadius = 60.0f;
    ASSERT_TRUE(f.session.press(at, 1.0f, 0)) << f.session.lastError();
    f.doc->evaluate(request);  // the press: the CPU's, at the stroke's density
    feed(renderer, *f.doc, f.doc->updateDisplay(settings));
    view.frame(renderer, f.camera, false);
    std::string why;
    ASSERT_TRUE(gpu.armStroke(&why)) << why;
    EXPECT_FALSE(gpu.stopAt().empty());
    int gpuDabs = 0;
    QImage gpuFrame, cpuFrame;
    for (int k = 1; k <= 3; ++k) {
        f.session.move(QPointF(at.x() + 12.0 * k, at.y()), 1.0f);
        request.stopAt = gpu.stopAt();
        f.doc->evaluate(request);
        ASSERT_TRUE(gpu.strokeHolds()) << gpu.why();
        feed(renderer, *f.doc, f.doc->updateDisplay(settings));
        gpuFrame = view.frame(renderer, f.camera).image;
        if (!renderer.gpuRanLastFrame()) gpuFrame = view.frame(renderer, f.camera).image;
        if (renderer.gpuRanLastFrame()) ++gpuDabs;
        else ADD_FAILURE() << "the dab did not run on the GPU: " << renderer.gpuWhy();
    }
    EXPECT_EQ(gpuDabs, 3);
    // The same dab evaluated whole on the CPU (the view's strands made again), drawn: the
    // GPU's frame is nearly it.
    request.stopAt.clear();
    f.doc->evaluate(request);
    feed(renderer, *f.doc, f.doc->updateDisplay(settings));
    cpuFrame = view.frame(renderer, f.camera).image;
    if (renderer.gpuRanLastFrame()) cpuFrame = view.frame(renderer, f.camera).image;
    ASSERT_FALSE(gpuFrame.isNull());
    ASSERT_FALSE(cpuFrame.isNull());
    const double error = pixelsDiffering(gpuFrame, cpuFrame);
    std::printf("[ stroke   ] %d dabs drawn by the GPU; pixels differing from the CPU's dab %.3f %%; the GPU's base upload: %zu strands%s\n",
                gpuDabs, error * 100.0, gpu.lastUploadStrands(), gpu.lastUploadPartial() ? " (partial)" : "");
    EXPECT_LT(error, 0.005);
    EXPECT_EQ(f.session.release(), StrokeRelease::Stroke);
    gpu.endStroke();
    f.doc->evaluate(request);
    feed(renderer, *f.doc, f.doc->updateDisplay(settings));
    view.frame(renderer, f.camera, false);
    renderer.releaseResources();
}

TEST(AppGpuPreview, SculptStrokeDabsRunOnTheGpu) {
    // The same on a Sculpt node's guides (Sculpt <- ToGuides <- an even Scatter's strands shaped by
    // the hand guides): the dabs after the press evaluate the Sculpt node alone (stopAt) and the GPU
    // runs the look's Interpolate and what follows from its guides; the frame matches the CPU's
    // evaluation of the same dab, and the release is one undo step of the node's edits.
    std::unique_ptr<QRhi> rhi = createOffscreenRhi();
    ASSERT_TRUE(rhi);
    if (rhi->backend() == QRhi::Null || !GpuLookPreview::supported(rhi.get())) GTEST_SKIP() << "no device with a GPU preview engine";
    BrushFixture f;
    json graph = json::parse(gpuGraphJson(1500.0));
    graph["nodes"].push_back({{"name", "even"}, {"type", "Scatter"}, {"params", {{"density", 150.0}, {"randomness", 0.0}, {"length", 0.8}, {"previewAll", true}}}});
    graph["nodes"].push_back({{"name", "dense"}, {"type", "Interpolate"}});
    graph["nodes"].push_back({{"name", "toGuides"}, {"type", "ToGuides"}});
    graph["nodes"].push_back({{"name", "sculpt"}, {"type", "Sculpt"}});
    for (json& c : graph["connections"]) {
        if (c["from"] == "guides" && c["to"] == "interpolate") c["from"] = "sculpt";
    }
    for (const auto& [from, to, input] : std::vector<std::tuple<const char*, const char*, const char*>>{
             {"scalp", "even", "scalp"}, {"even", "dense", "strands"}, {"guides", "dense", "guides"}, {"scalp", "dense", "scalp"},
             {"dense", "toGuides", "strands"}, {"toGuides", "sculpt", "guides"}, {"scalp", "sculpt", "scalp"}}) {
        graph["connections"].push_back({{"from", from}, {"to", to}, {"input", input}});
    }
    ASSERT_EQ(f.doc->replaceGraph(graph.dump()), "");
    f.doc->setSculptTarget("sculpt");
    ViewRequest request;
    request.density = 1.0f;
    request.sculptDensity = f.session.sculptDensity;
    ASSERT_TRUE(f.doc->evaluate(request).strands);
    f.camera.frame(f.doc->sceneBounds());
    f.session.setView(f.camera, f.w, f.h);
    const QSize size(320, 240);
    OffscreenView view(size, rhi.get());
    ASSERT_TRUE(view.ready());
    GpuLookPreview gpu;
    gpu.setDocument(f.doc.get());
    GroomRenderer renderer;
    renderer.setGpuPreview(&gpu);
    renderer.setLookdev(false);
    DisplaySettings settings;
    f.doc->display().invalidateAll();
    feed(renderer, *f.doc, f.doc->updateDisplay(settings));
    view.frame(renderer, f.camera, false);
    ASSERT_TRUE(framesUntilKernelsReady(gpu, renderer, view, f.camera));
    const HierarchyPtr sculpted = f.doc->session().sculptHierarchy();
    ASSERT_TRUE(sculpted && sculpted->guides().size() > 20u);
    // The guide nearest the camera (its middle), combed across.
    const QVector3D eye = f.camera.eye();
    const Vec3f camera(eye.x(), eye.y(), eye.z());
    const HierarchyGuide* g = &sculpted->guides()[0];
    for (const HierarchyGuide& c : sculpted->guides()) {
        if ((c.points[c.points.size() / 2] - camera).length() < (g->points[g->points.size() / 2] - camera).length()) g = &c;
    }
    const QPointF at = f.widgetPoint(g->points[g->points.size() / 2]);
    f.session.settings().setMode("comb");
    f.session.settings().screenRadius = 80.0f;
    ASSERT_TRUE(f.session.press(at, 1.0f, 0)) << f.session.lastError();
    EXPECT_TRUE(f.doc->session().hasPreviewSculpt());
    f.doc->evaluate(request);  // the press: the CPU's, at the stroke's density
    feed(renderer, *f.doc, f.doc->updateDisplay(settings));
    const QImage pressFrame = view.frame(renderer, f.camera).image;
    std::string why;
    ASSERT_TRUE(gpu.armStroke(&why)) << why;
    EXPECT_EQ(gpu.stopAt(), "interpolate");
    int gpuDabs = 0;
    QImage gpuFrame, cpuFrame;
    for (int k = 1; k <= 3; ++k) {
        f.session.move(QPointF(at.x() + 30.0 * k, at.y()), 1.0f);
        request.stopAt = gpu.stopAt();
        const std::size_t evaluations = f.doc->session().evaluationCount();
        f.doc->evaluate(request);
        EXPECT_LE(f.doc->session().evaluationCount() - evaluations, 1u) << "the Sculpt node alone on the CPU";
        ASSERT_TRUE(gpu.strokeHolds()) << gpu.why();
        feed(renderer, *f.doc, f.doc->updateDisplay(settings));
        gpuFrame = view.frame(renderer, f.camera).image;
        if (!renderer.gpuRanLastFrame()) gpuFrame = view.frame(renderer, f.camera).image;
        if (renderer.gpuRanLastFrame()) ++gpuDabs;
        else ADD_FAILURE() << "the dab did not run on the GPU: " << renderer.gpuWhy();
    }
    EXPECT_EQ(gpuDabs, 3);
    request.stopAt.clear();
    f.doc->evaluate(request);
    feed(renderer, *f.doc, f.doc->updateDisplay(settings));
    cpuFrame = view.frame(renderer, f.camera).image;
    if (renderer.gpuRanLastFrame()) cpuFrame = view.frame(renderer, f.camera).image;
    ASSERT_FALSE(gpuFrame.isNull());
    ASSERT_FALSE(cpuFrame.isNull());
    const double error = pixelsDiffering(gpuFrame, cpuFrame), moved = pixelsDiffering(pressFrame, cpuFrame);
    std::printf("[ sculpt   ] %d dabs drawn by the GPU; pixels differing from the CPU's dab %.3f %% (from the press's frame %.2f %%)\n",
                gpuDabs, error * 100.0, moved * 100.0);
    EXPECT_LT(error, 0.005);
    EXPECT_GT(moved, 0.001) << "the dabs reshaped the hair";
    EXPECT_EQ(f.session.release(), StrokeRelease::Stroke);
    gpu.endStroke();
    EXPECT_FALSE(f.doc->session().hasPreviewSculpt());
    EXPECT_GT(f.doc->sculptedGuides("sculpt"), 0u);
    ASSERT_TRUE(f.doc->canUndo());
    EXPECT_EQ(f.doc->undo(), "");
    EXPECT_EQ(f.doc->sculptedGuides("sculpt"), 0u) << "one undo step takes the stroke back";
    f.doc->evaluate(request);
    feed(renderer, *f.doc, f.doc->updateDisplay(settings));
    view.frame(renderer, f.camera, false);
    renderer.releaseResources();
}

TEST(AppDocument, ALiveDragHeldOutsideTheGraphCommitsAtItsEnd) {
    // A slider's drag the host previews elsewhere: the document holds the value (param shows
    // it), the graph is untouched until the drag's end records it, then it is one undo step.
    auto doc = GroomDocument::open(kSampleGroom);
    const std::string length = nodeOfType(doc->nodes(), "Length").name;
    const json before = doc->paramOrDefault(length, "keep");
    int previews = 0, ended = 0;
    doc->livePreview = [&](const std::string& node, const std::string& name, const json&) {
        ++previews;
        return node == length && name == "keep";
    };
    doc->livePreviewEnded = [&] { ++ended; };
    ViewRequest request;
    request.density = 0.2f;
    doc->evaluate(request);
    const std::size_t evaluations = doc->session().evaluationCount();
    EXPECT_EQ(doc->setParam(length, "keep", 0.7, false), "");
    EXPECT_TRUE(doc->previewing());
    EXPECT_EQ(doc->param(length, "keep").get<double>(), 0.7);
    EXPECT_EQ(previews, 1);
    doc->evaluate(request);
    EXPECT_EQ(doc->session().evaluationCount(), evaluations) << "nothing evaluated while the value is held";
    EXPECT_FALSE(doc->canUndo());
    EXPECT_EQ(doc->setParam(length, "keep", 0.9, false), "");
    doc->recordEdit(length, "keep", before, doc->paramOrDefault(length, "keep"));
    EXPECT_FALSE(doc->previewing());
    EXPECT_EQ(ended, 1);
    EXPECT_EQ(doc->graph()["nodes"].size() > 0, true);
    EXPECT_EQ(doc->param(length, "keep").get<double>(), 0.9);
    EXPECT_TRUE(doc->canUndo());
    EXPECT_EQ(doc->undoText(), "Undo " + length + ".keep");
    EXPECT_EQ(doc->undo(), "");
    EXPECT_EQ(doc->paramOrDefault(length, "keep"), before);
    // Refused by the host: the step applies on the CPU as ever.
    doc->livePreview = [](const std::string&, const std::string&, const json&) { return false; };
    EXPECT_EQ(doc->setParam(length, "keep", 0.3, false), "");
    EXPECT_FALSE(doc->previewing());
    EXPECT_EQ(doc->param(length, "keep").get<double>(), 0.3);
}

TEST(AppGpuPreview, AVectorParamDragsAsAVector) {
    // The window's livePreview hands a drag's JSON to the GPU preview as gpuOverrideFromJson reads
    // it: a number, three numbers as a vec3 (not a profile's keys), a ramp; nothing else.
    gpu::OverrideValue v;
    ASSERT_TRUE(gpuOverrideFromJson(json(0.25), &v));
    EXPECT_EQ(std::get<double>(v), 0.25);
    ASSERT_TRUE(gpuOverrideFromJson(json::parse("[1, -1, 0.5]"), &v));
    ASSERT_TRUE(std::holds_alternative<Vec3f>(v));
    EXPECT_EQ(std::get<Vec3f>(v), Vec3f(1.0f, -1.0f, 0.5f));
    ASSERT_TRUE(gpuOverrideFromJson(json::parse("[[0, 1.0], [0.5, 0.2], [1, 0.3]]"), &v));
    EXPECT_TRUE(std::holds_alternative<Ramp>(v));
    EXPECT_FALSE(gpuOverrideFromJson(json("text"), &v));
    EXPECT_FALSE(gpuOverrideFromJson(json::parse("[1, 2]"), &v));

    // Curl's weightDirection dragged over a weighted Curl: the GPU preview takes the vector (and
    // refuses it sent as anything else).
    auto doc = GroomDocument::open(kSampleGroom);
    json graph = json::parse(gpuGraphJson(800.0));
    for (json& n : graph["nodes"]) {
        if (n["name"] == "noise") {
            n = {{"name", "noise"}, {"type", "Curl"}, {"params", {{"radius", 0.2}, {"frequency", 1.0}, {"weight", 0.8}}}};
        }
    }
    ASSERT_EQ(doc->replaceGraph(graph.dump()), "");
    ViewRequest request;
    request.density = 1.0f;
    ASSERT_TRUE(doc->evaluate(request).strands);
    GpuLookPreview gpu;
    gpu.setDocument(doc.get());
    std::string why;
    ASSERT_TRUE(gpuOverrideFromJson(json::parse("[1, -1, 0]"), &v));
    EXPECT_TRUE(gpu.drag("noise", "weightDirection", v, &why)) << why;
    EXPECT_TRUE(gpu.dragging());
    gpu.endDrag();
    EXPECT_FALSE(gpu.drag("noise", "weightDirection", gpu::OverrideValue(0.5), &why));
}


// ---- V-Ray renders (vray_render.h) --------------------------------------------------------------

TEST(AppVray, TheStageScene) {
    const std::string dir = tempPath("groomlab_app_vray");
    std::filesystem::create_directories(dir);
    const std::string scene = dir + "/stage_scene.vrscene";
    VrayStage stage;
    stage.eye = QVector3D(0.0f, 10.0f, 50.0f);
    stage.target = QVector3D(0.0f, 10.0f, 0.0f);
    stage.lighting = lightingPreset("studio");
    stage.lighting.lights[1].enabled = false;
    stage.lighting.exposure = 1.0f;
    stage.scalp = std::make_shared<ScalpMesh>(std::vector<Vec3f>{{0, 0, 0}, {1, 0, 0}, {0, 1, 0}},
                                              std::vector<ScalpMesh::Triangle>{{0, 1, 2}});
    stage.width = 400;
    stage.height = 200;
    const auto read = [&scene] {
        std::ifstream in(scene);
        return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    };
    writeVrayScene(scene, dir + "/groom.vrscene", stage);
    const std::string text = read();
    EXPECT_NE(text.find("#include \""), std::string::npos);
    EXPECT_NE(text.find("groom.vrscene\""), std::string::npos);
    EXPECT_NE(text.find("img_width=400;"), std::string::npos);
    // The key and the rim (the fill is off), intensity / pi.
    EXPECT_NE(text.find("LightDirect light_0"), std::string::npos);
    EXPECT_NE(text.find("LightDirect light_1"), std::string::npos);
    EXPECT_EQ(text.find("LightDirect light_2"), std::string::npos);
    EXPECT_NE(text.find("intensity=0.636619"), std::string::npos);  // the key's 2 / pi
    EXPECT_NE(text.find("LightDome environment"), std::string::npos);
    EXPECT_NE(text.find("intensity=0.4;"), std::string::npos);  // the studio environment
    EXPECT_TRUE(std::filesystem::exists(dir + "/stage_scene_env.hdr"));
    EXPECT_NE(text.find("exposure=Color(2, 2, 2);"), std::string::npos);
    EXPECT_NE(text.find("faces=ListInt(0, 1, 2);"), std::string::npos);
    // The camera looks down -Z from +Z: its Z axis is +Z, at the eye; the horizontal fov of a 40
    // degree vertical one at 2:1.
    EXPECT_NE(text.find("transform=Transform(Matrix(Vector(1, 0, 0), Vector(0, 1, 0), Vector(0, 0, 1)), Vector(0, 10, 50));"),
              std::string::npos);
    const double fov = 2.0 * std::atan(std::tan(20.0 * 3.14159265358979 / 180.0) * 2.0);
    char expected[64];
    std::snprintf(expected, sizeof expected, "fov=%.7g;", fov);
    EXPECT_NE(text.find(expected), std::string::npos) << expected;

    // A turntable: the camera keyed per frame, around the target.
    stage.turntableFrames = 4;
    stage.lighting = lightingPreset("key");  // no environment: no dome
    writeVrayScene(scene, dir + "/groom.vrscene", stage);
    const std::string turn = read();
    EXPECT_NE(turn.find("transform=interpolate((1, Transform("), std::string::npos);
    EXPECT_NE(turn.find("(4, Transform("), std::string::npos);
    EXPECT_EQ(turn.find("(5, Transform("), std::string::npos);
    EXPECT_EQ(turn.find("LightDome"), std::string::npos);
    EXPECT_NE(turn.find("bg_color=Color(0, 0, 0);"), std::string::npos);
    std::error_code ignored;
    std::filesystem::remove_all(dir, ignored);
}

TEST(AppVray, ReadsVraysProgress) {
    VrayProgress p;
    EXPECT_FALSE(p.feed(QStringLiteral("[2026/Oct/20|10:09:00] [224 MB] Scene created successfully.")));
    EXPECT_TRUE(p.feed(QStringLiteral("[2026/Oct/20|10:09:00] [232 MB] Starting frame 2.")));
    EXPECT_EQ(p.frame, 2);
    EXPECT_TRUE(p.feed(QStringLiteral("[2026/Oct/20|10:09:00] [447 MB] Rendering image...: 45.50% [ 0h  0m  0.1s]")));
    EXPECT_DOUBLE_EQ(p.percent, 45.5);
    EXPECT_TRUE(p.feed(QStringLiteral("[2026/Oct/20|10:09:00] [283 MB] Successfully written image file \"out.0002.png\"")));
    EXPECT_EQ(p.written, QStringLiteral("out.0002.png"));
    EXPECT_EQ(p.imagesWritten, 1);
    p.feed(QStringLiteral("[2026/Oct/20|10:09:00] [20 MB] error: Cannot open file \"x.vrscene\""));
    EXPECT_EQ(p.error, QStringLiteral("error: Cannot open file \"x.vrscene\""));
    EXPECT_EQ(renderFrameImage(QStringLiteral("C:/r/turn.png"), 3, 36), QStringLiteral("C:/r/turn.0003.png"));
    EXPECT_EQ(renderFrameImage(QStringLiteral("C:/r/one.png"), 1, 0), QStringLiteral("C:/r/one.png"));
    EXPECT_TRUE(vrayArguments(QStringLiteral("s.vrscene"), QStringLiteral("o.png"), 0).contains(QStringLiteral("-noFrameNumbers=1")));
    EXPECT_TRUE(vrayArguments(QStringLiteral("s.vrscene"), QStringLiteral("o.png"), 36).contains(QStringLiteral("-frames=1-36")));
}

// ---- RenderMan renders (prman_render.h) -----------------------------------------------------------

TEST(AppPrman, TheStageScene) {
    const std::string dir = tempPath("groomlab_app_prman");
    std::filesystem::create_directories(dir);
    const std::string scene = dir + "/stage_scene.rib";
    RenderStage stage;
    stage.eye = QVector3D(0.0f, 10.0f, 50.0f);
    stage.target = QVector3D(0.0f, 10.0f, 0.0f);
    stage.lighting = lightingPreset("studio");
    stage.lighting.lights[1].enabled = false;
    stage.lighting.exposure = 1.0f;
    stage.scalp = std::make_shared<ScalpMesh>(std::vector<Vec3f>{{0, 0, 0}, {1, 0, 0}, {0, 1, 0}},
                                              std::vector<ScalpMesh::Triangle>{{0, 1, 2}});
    stage.width = 400;
    stage.height = 200;
    PrmanScene rib;
    rib.groomPath = dir + "/groom.groom";
    rib.frame = 12.0;
    rib.hairTilt = 2.5f;
    rib.procedurals = dir;
    rib.imagePath = dir + "/frame.png";
    rib.environmentTexture = dir + "/stage_scene_env.tex";
    const auto read = [&scene] {
        std::ifstream in(scene);
        return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    };
    const auto count = [](const std::string& text, const std::string& what) {
        std::size_t n = 0;
        for (std::size_t at = text.find(what); at != std::string::npos; at = text.find(what, at + 1)) ++n;
        return n;
    };
    writePrmanScene(scene, rib, stage);
    const std::string text = read();
    EXPECT_EQ(text.rfind("##RenderMan RIB", 0), 0u);
    EXPECT_EQ(count(text, "FrameBegin"), 1u);
    EXPECT_NE(text.find("Format 400 200 1"), std::string::npos);
    EXPECT_NE(text.find("frame.png\" \"png\" \"rgba\""), std::string::npos);
    EXPECT_NE(text.find(":@\"]"), std::string::npos);  // the procedural's folder searched
    // The viewport's vertical 40 degrees: PxrPerspective's (the shorter side's) at 2:1.
    EXPECT_NE(text.find("\"PxrPerspective\" \"float fov\" [40]"), std::string::npos);
    // The camera: Z flipped, then the world into the camera at (0, 10, 50) looking down -Z.
    EXPECT_NE(text.find("Scale 1 1 -1\nConcatTransform [1 0 0 0 0 1 0 0 0 0 1 0 0 -10 -50 1]"), std::string::npos);
    // The key and the rim (the fill is off): the irradiance x 2^EV over the disk's solid angle.
    EXPECT_EQ(count(text, "Light \"PxrDistantLight\""), 2u);
    const double solidAngle = 2.0 * 3.14159265358979 * (1.0 - std::cos(kPrmanDistantAngle * 3.14159265358979 / 360.0));
    char key[64];
    std::snprintf(key, sizeof key, "\"float intensity\" [%.7g]", 2.0 * 2.0 / solidAngle);
    EXPECT_NE(text.find(key), std::string::npos) << key;
    // The environment: a dome unseen (the background is the solid color, by the display filter).
    EXPECT_NE(text.find("Light \"PxrDomeLight\" \"environment\" \"float intensity\" [0.8]"), std::string::npos);
    EXPECT_NE(text.find("stage_scene_env.tex\""), std::string::npos);
    EXPECT_NE(text.find("\"int camera\" [0]"), std::string::npos);
    EXPECT_NE(text.find("PxrBackgroundDisplayFilter"), std::string::npos);
    EXPECT_NE(text.find("PointsPolygons [3]\n  [0 1 2]"), std::string::npos);
    // The groom through the procedural at the document's frame, the hair network with its tilt.
    EXPECT_NE(text.find("Procedural2 \"DynamicLoad\" \"DSOBound\" \"string dsoname\" [\"groomlab_renderman\"]"), std::string::npos);
    EXPECT_NE(text.find("groom.groom\"] \"float frame\" [12]"), std::string::npos);
    EXPECT_NE(text.find("\"float specularOffset\" [-2.5]"), std::string::npos);
    EXPECT_NE(text.find("\"string varname\" [\"rman_rgbShare\"]"), std::string::npos);

    // A turntable: a frame a frame, each its own image and camera; no environment, no dome.
    stage.turntableFrames = 4;
    stage.lighting = lightingPreset("key");
    rib.environmentTexture.clear();
    writePrmanScene(scene, rib, stage);
    const std::string turn = read();
    EXPECT_EQ(count(turn, "FrameBegin"), 4u);
    EXPECT_NE(turn.find("frame.0004.png\""), std::string::npos);
    EXPECT_EQ(turn.find("PxrDomeLight"), std::string::npos);
    std::error_code ignored;
    std::filesystem::remove_all(dir, ignored);
}

TEST(AppPrman, ReadsPrmansProgress) {
    PrmanProgress p;
    EXPECT_FALSE(p.feed(QStringLiteral("X00004 [groomlab] hair.groom: 50320 strands, 855440 points")));
    EXPECT_TRUE(p.feed(QStringLiteral("R90000    0%")));
    EXPECT_EQ(p.frame, 1);
    EXPECT_TRUE(p.feed(QStringLiteral("R90000   45%")));
    EXPECT_DOUBLE_EQ(p.percent, 45.0);
    EXPECT_TRUE(p.feed(QStringLiteral("R90000  100%")));
    EXPECT_EQ(p.imagesWritten, 1);
    EXPECT_TRUE(p.feed(QStringLiteral("R90000  100%")));  // said twice: one image
    EXPECT_EQ(p.imagesWritten, 1);
    EXPECT_TRUE(p.feed(QStringLiteral("R90000    3%")));  // the next frame
    EXPECT_EQ(p.frame, 2);
    p.feed(QStringLiteral("R56001 {ERROR} cannot open \"x.groom\""));
    EXPECT_EQ(p.error, QStringLiteral("R56001 {ERROR} cannot open \"x.groom\""));
    EXPECT_EQ(prmanArguments(QStringLiteral("s.rib")), QStringList({QStringLiteral("-Progress"), QStringLiteral("s.rib")}));
    // prman found in its file, its folder, or RenderMan Pro Server's folder.
    const std::filesystem::path tree = std::filesystem::path(tempPath("groomlab_app_prman_tree")) / "RenderManProServer-27.3";
    std::filesystem::create_directories(tree / "bin");
#ifdef _WIN32
    const std::filesystem::path exe = tree / "bin" / "prman.exe";
#else
    const std::filesystem::path exe = tree / "bin" / "prman";
#endif
    std::ofstream(exe).put('x');
    const QString want = QFileInfo(QString::fromStdString(exe.u8string())).absoluteFilePath();
    for (const auto& given : {exe, tree / "bin", tree}) EXPECT_EQ(prmanIn(QString::fromStdString(given.u8string())), want);
    EXPECT_EQ(prmanEnvironment(want).value(QStringLiteral("RMANTREE")),
              QDir::toNativeSeparators(QFileInfo(QString::fromStdString(tree.u8string())).absoluteFilePath()));
    EXPECT_TRUE(prmanIn(QString::fromStdString((tree / "nothing").u8string())).isEmpty());
    std::error_code ignored;
    std::filesystem::remove_all(tree.parent_path(), ignored);
}

// The suite's settings folder (main sets QSettings there; a test with its own goes back to it).
QString testSettingsFolder() {
    const QString given = qEnvironmentVariable("GROOMLAB_SETTINGS_DIR");
    return given.isEmpty() ? QDir::tempPath() + QStringLiteral("/groomlab_app_tests_settings") : given;
}

}  // namespace groomlab::app

int main(int argc, char** argv) {
    // No window: the offscreen platform (ctest sets it too; a run by hand gets it here). A
    // QApplication, not a QGuiApplication: the panel tests make widgets.
    if (qgetenv("QT_QPA_PLATFORM").isEmpty()) qputenv("QT_QPA_PLATFORM", "offscreen");
    std::setvbuf(stderr, nullptr, _IONBF, 0);  // a crash loses nothing said before it (ctest pipes stderr)
    QApplication app(argc, argv);
    // The window remembers recent files, the theme, autosaves and units in QSettings: an ini of this
    // run's own (GROOMLAB_SETTINGS_DIR, as ctest sets it, else under %TEMP%), never the user's
    // %APPDATA%\GroomLab\GroomLab.ini (another application name, and another folder).
    QApplication::setApplicationName(QStringLiteral("GroomLabTests"));
    QApplication::setOrganizationName(QStringLiteral("GroomLab"));
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, groomlab::app::testSettingsFolder());
    // The ImportCurves node (Alembic guides), as the app's main() registers it.
    groomlab::registerAlembicOperators(groomlab::OperatorRegistry::instance());
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
