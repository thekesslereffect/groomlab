// SPDX-License-Identifier: Apache-2.0
// Golden-output tests: every operator on a fixed scene, compared with
// tests/data/golden/<case>.json. Strand counts and IDs must match exactly;
// positions, widths and channels within a tolerance, so the same files hold
// on every compiler and platform. Evaluation with one thread and with many
// must be bit-identical.
//
// After an intended change in output, regenerate with GROOMLAB_UPDATE_GOLDEN=1
// and review the diff of the golden files.

#ifdef _MSC_VER
#define _CRT_SECURE_NO_WARNINGS  // std::getenv, read-only
#endif

#include <gtest/gtest.h>

#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include <nlohmann/json.hpp>

#if GROOMLAB_HAS_TBB
#include <tbb/task_arena.h>
#endif

#include "groomlab/cards.h"
#include "groomlab/graph_io.h"
#include "groomlab/groom.h"
#include "groomlab/guide_hierarchy.h"
#include "groomlab/random.h"
#include "groomlab/sculpt.h"
#include "groomlab/simulation.h"
#include "test_helpers.h"

namespace groomlab {
namespace {

using json = nlohmann::json;
namespace fs = std::filesystem;

const fs::path kData = GROOMLAB_TEST_DATA;
constexpr float kPositionTolerance = 1e-4f;

// Sphere with UVs (spherical mapping) and three color sets: "mask" (by height), "comb" (a
// painted direction, +X) and "regions" (three sectors around it, red, green and blue).
MeshPtr goldenScalp() {
    auto mesh = std::make_shared<ScalpMesh>(*test::makeSphere(24, 48, 1.0f));
    std::vector<ScalpMesh::UV> uvs;
    std::vector<ScalpMesh::Color> mask, comb, regions;
    for (const ScalpMesh::Triangle& t : mesh->triangles()) {
        for (const std::int32_t v : t) {
            const Vec3f& p = mesh->points()[static_cast<std::size_t>(v)];
            const float u = 0.5f + std::atan2(p.z, p.x) / 6.2831853f;
            uvs.push_back({u, 0.5f + 0.5f * p.y});
            const float m = std::clamp(0.5f + p.y, 0.0f, 1.0f);
            mask.push_back({m, m, m, 1.0f});
            comb.push_back({1.0f, 0.5f, 0.5f, 1.0f});
            regions.push_back(u < 1.0f / 3.0f ? ScalpMesh::Color{1, 0, 0, 1} : u < 2.0f / 3.0f ? ScalpMesh::Color{0, 1, 0, 1} : ScalpMesh::Color{0, 0, 1, 1});
        }
    }
    mesh->setCornerUVs(std::move(uvs));
    mesh->setCornerColors("mask", std::move(mask));
    mesh->setCornerColors("comb", std::move(comb));
    mesh->setCornerColors("regions", std::move(regions));
    return mesh;
}

// A ball resting in the hair above the top of the scalp: the "collider" mesh
// external (ColliderMesh) for Collide's collider input.
MeshPtr goldenCollider() {
    const auto unit = test::makeSphere(12, 24, 0.6f);
    std::vector<Vec3f> points = unit->points();
    for (Vec3f& p : points) p += Vec3f{0.0f, 1.7f, 0.0f};
    return std::make_shared<const ScalpMesh>(std::move(points), unit->triangles());
}

// A thin flap standing on a floor (an ear on a head, a fold of a shell): Interpolate's paths along
// the skin go round it. Guides on its two faces (one combed out of the face, the other along
// it) and on the floor (standing up, leaning toward +x).
MeshPtr flapScalp() { return test::makeFin(1.2f, 0.08f, 2.0f, 0.05f, true, 1.0f); }
std::string flapHierarchy(const ScalpMesh& scalp) {
    GuideHierarchy h;
    const GroupId primary = h.addGroup("primary");
    for (int i = 0; i < 6; ++i) {
        const float x = 0.17f + 0.33f * static_cast<float>(i);
        for (int j = 0; j < 4; ++j) {
            const float y = 0.15f + 0.3f * static_cast<float>(j);
            h.addGuide(primary, test::straightCurve({x, y, -0.04f}, {0.0f, -0.1f, -0.7f}, 6));
            h.addGuide(primary, test::straightCurve({x, y, 0.04f}, {0.7f, -0.2f, 0.1f}, 6));
        }
        for (const float z : {-0.75f, -0.35f, 0.35f, 0.75f}) h.addGuide(primary, test::straightCurve({x, 0.0f, z}, {0.2f, 0.7f, 0.0f}, 6));
    }
    h.evaluate(&scalp);
    return h.toJson();
}

// Eight primary guides over the sphere with generated children.
std::string goldenHierarchy(const ScalpMesh& scalp) {
    GuideHierarchy h;
    const GroupId primary = h.addGroup("primary");
    TubeProfile tube;
    tube.width = {0.2f, 0.12f};
    Rng rng(11);
    for (int i = 0; i < 8; ++i) {
        const Vec3f n = Vec3f{rng.uniform(-1, 1), rng.uniform(-0.2f, 1), rng.uniform(-1, 1)}.normalized();
        const Vec3f bend{rng.uniform(-0.3f, 0.3f), -0.2f, rng.uniform(-0.3f, 0.3f)};
        std::vector<Vec3f> pts;
        for (int k = 0; k <= 8; ++k) {
            const float t = static_cast<float>(k) / 8.0f;
            pts.push_back(n + n * (0.9f * t) + bend * (t * t));
        }
        h.addGuide(primary, pts, tube);
    }
    ChildGenerationParams params;
    params.count = 4;
    params.seed = 3;
    h.generateChildren(primary, "children", params);
    h.evaluate(&scalp);
    return h.toJson();
}

struct Case {
    const char* name;
    const char* output;  // node whose value is compared
    std::string nodes;   // extra nodes (JSON array items)
    std::string connections;
    bool parts = false;  // run with a part along a meridian of the scalp
    double time = 0.0;   // the evaluation time (Wind)
    bool motion = false;  // with a history of the scalp moving over the last frames (Jiggle)
    int simulate = 0;     // frames the Simulate node "op" is stepped (from rest on the guides) before the evaluation
    bool flap = false;    // on the flap scalp (a thin flap standing on a floor) and its guides
    bool sculpt = false;  // the Sculpt node "op" given edits of the guides it is fed (sculptEdits)
};

// Every case shares scalp -> scatter -> interpolate (guides, scalp); the strands grow with
// kSegments segments (the context's, every Scatter alike).
constexpr int kSegments = 7;

std::string graphFor(const Case& c) {
    std::string graph = R"({"version": 1, "nodes": [
      {"name": "scalp", "type": "Scalp"},
      {"name": "guides", "type": "Guides"},
      {"name": "primaries", "type": "Guides", "params": {"level": 0}},
      {"name": "scatter", "type": "Scatter", "params": {"density": 300.0}},
      {"name": "interp", "type": "Interpolate"})";
    if (!c.nodes.empty()) graph += ",\n" + c.nodes;
    graph += R"(], "connections": [
      {"from": "scalp", "to": "scatter", "input": "scalp"},
      {"from": "scatter", "to": "interp", "input": "strands"},
      {"from": "guides", "to": "interp", "input": "guides"},
      {"from": "scalp", "to": "interp", "input": "scalp"})";
    if (!c.connections.empty()) graph += ",\n" + c.connections;
    graph += std::string(R"(], "output": ")") + c.output + "\"}";
    const std::string texture = (kData / "textures" / "density.pgm").generic_string();
    for (std::size_t at; (at = graph.find("$TEXTURE")) != std::string::npos;) graph.replace(at, 8, texture);
    return graph;
}

std::string link(const char* from, const char* to, const char* input) {
    return std::string(R"({"from": ")") + from + R"(", "to": ")" + to + R"(", "input": ")" + input + R"("})";
}

std::vector<Case> cases() {
    return {
        {"Scatter", "scatter", "", ""},
        {"ScatterDensityTexture", "tex",
         R"({"name": "tex", "type": "Scatter", "params": {"density": {"texture": "$TEXTURE", "component": "luminance", "max": 600.0}}})",
         link("scalp", "tex", "scalp")},
        {"Guides", "guides", "", ""},
        {"Interpolate", "interp", "", ""},
        {"InterpolateFlap", "interp", "", "", false, 0.0, false, 0, true},
        // Regions painted in three sectors: each strand from its sector's guides; Clump within them.
        {"InterpolateRegions", "op", R"({"name": "op", "type": "Interpolate", "params": {"regionMap": "regions"}})",
         link("scatter", "op", "strands") + "," + link("guides", "op", "guides") + "," + link("scalp", "op", "scalp")},
        {"ClumpRegions", "op",
         R"({"name": "regioned", "type": "Interpolate", "params": {"regionMap": "regions"}},
            {"name": "op", "type": "Clump", "params": {"count": 30, "strength": 0.7}})",
         link("scatter", "regioned", "strands") + "," + link("guides", "regioned", "guides") + "," + link("scalp", "regioned", "scalp") +
             "," + link("regioned", "op", "strands")},
        {"InterpolateFlapStraight", "op", R"({"name": "op", "type": "Interpolate", "params": {"followSurface": false}})",
         link("scatter", "op", "strands") + "," + link("guides", "op", "guides") + "," + link("scalp", "op", "scalp"), false, 0.0,
         false, 0, true},
        {"Length", "op", R"({"name": "op", "type": "Length", "params": {"keep": 0.6, "randomness": 0.3}})",
         link("interp", "op", "strands")},
        {"LengthCutColorSet", "op",
         R"({"name": "op", "type": "Length", "params": {"keep": {"colorSet": "mask", "min": 0.3, "max": 1.0}}})",
         link("interp", "op", "strands")},
        {"Width", "op", R"({"name": "op", "type": "Width", "params": {"width": 0.02, "profile": [[0, 1], [1, 0.2]]}})",
         link("interp", "op", "strands")},
        {"FieldDrivenLength", "op",
         R"({"name": "ramp", "type": "Expression", "params": {"expression": "u", "min": 0.3, "max": 1.0}},
            {"name": "noise", "type": "FieldNoise", "params": {"frequency": 4.0, "seed": 5}},
            {"name": "soft", "type": "FieldRemap", "params": {"curve": [[0, 0.5], [1, 1]]}},
            {"name": "both", "type": "FieldCombine", "params": {"mode": "multiply"}},
            {"name": "op", "type": "Length", "params": {}, "paramInputs": ["keep"]})",
         link("noise", "soft", "in") + "," + link("ramp", "both", "a") + "," + link("soft", "both", "b") + "," +
             link("interp", "op", "strands") + "," + link("both", "op", "keep")},
        // Tapered strands: instances scale with the width where they sit (scaleByWidth, on by default).
        {"Instance", "op",
         R"({"name": "taper", "type": "Width", "params": {"width": 0.01, "profile": [[0, 1], [1, 0.25]]}},
            {"name": "op", "type": "Instance", "params": {"probability": 0.2, "count": 2, "seed": 4}})",
         link("interp", "taper", "strands") + "," + link("taper", "op", "strands")},
        // A scale at each root, lying on the skin along its strand, as wide as the strand's root.
        {"Scales", "op",
         R"({"name": "wide", "type": "Width", "params": {"width": 0.05}},
            {"name": "op", "type": "Scales", "params": {"lift": 10.0, "scaleRandom": 0.2, "spinRandom": 0.1, "seed": 2}})",
         link("interp", "wide", "strands") + "," + link("wide", "op", "strands")},
        // Fitted to the strands (sizeMode fitStrand): a fraction of each strand's length along +Y,
        // aspect of that across, narrowed by the taper where they sit.
        {"InstanceFitStrand", "op",
         R"({"name": "taper", "type": "Width", "params": {"width": 0.01, "profile": [[0, 1], [1, 0.25]]}},
            {"name": "op", "type": "Instance", "params": {"probability": 0.2, "seed": 5, "tMin": 0.0, "tMax": 0.5,
             "sizeMode": "fitStrand", "fitScale": 0.4, "aspect": 0.3}})",
         link("interp", "taper", "strands") + "," + link("taper", "op", "strands")},
        // Bent along their strands (deform alongStrand): placed as before, facing the skin.
        {"InstanceDeform", "op",
         R"({"name": "op", "type": "Instance", "params": {"probability": 0.2, "seed": 5, "tMin": 0.1, "tMax": 0.3,
             "sizeMode": "fitStrand", "fitScale": 0.5, "aspect": 0.3, "spinRandom": 0.0, "deform": "alongStrand"}})",
         link("interp", "op", "strands")},
        {"Braid", "op", R"({"name": "op", "type": "Braid", "params": {"region": 0.6, "width": 0.3, "seed": 1}})",
         link("interp", "op", "strands") + "," + link("guides", "op", "braids")},
        {"BraidFed", "op",
         R"({"name": "op", "type": "Braid", "params": {"style": "cornrow", "feed": true, "region": 0.4, "width": 0.2, "seed": 1}})",
         link("interp", "op", "strands") + "," + link("guides", "op", "braids")},
        {"InterpolateRadius", "op",
         R"({"name": "comb", "type": "Bend", "params": {"angle": 90.0, "along": "direction", "direction": [1, 0, 0]}},
            {"name": "op", "type": "Interpolate", "params": {"limitDistance": 0.4, "falloff": [[0, 1], [0.6, 0.8], [1, 0]]}})",
         link("scatter", "comb", "strands") + "," + link("comb", "op", "strands") + "," + link("guides", "op", "guides")},
        {"MapLength", "op",
         R"x({"name": "mask", "type": "Map", "params": {"name": "mask", "min": 1.0, "max": 0.3}},
            {"name": "op", "type": "Length", "params": {}, "paramInputs": ["keep"]})x",
         link("interp", "op", "strands") + "," + link("mask", "op", "keep")},
        {"ExpressionClump", "op",
         R"x({"name": "len", "type": "Length", "params": {"randomness": 1.0}},
            {"name": "long", "type": "Expression", "params": {"expression": "smoothstep(0.5, 0.8, length / longest)"}},
            {"name": "op", "type": "Clump", "params": {"count": 20, "centerWeight": {"expression": "length / longest > 0.6 ? 1 : 0"},
             "strength": 0.9}, "paramInputs": ["strength"]})x",
         link("interp", "len", "strands") + "," + link("len", "op", "strands") +
             "," + link("long", "op", "strength")},
        {"ToStrands", "op", R"({"name": "op", "type": "ToStrands", "params": {"segments": 6}})",
         link("guides", "op", "guides")},
        {"TwistAxes", "op", R"({"name": "op", "type": "Twist", "params": {"turns": 1.5}})",
         link("interp", "op", "strands") + "," + link("guides", "op", "axes")},
        {"TwistClumps", "op",
         R"({"name": "clump", "type": "Clump", "params": {"count": 20, "strength": 0.7}},
            {"name": "op", "type": "Twist", "params": {"turns": 2.0, "profile": [[0, 0], [1, 1]]}})",
         link("interp", "clump", "strands") + "," + link("clump", "op", "strands")},
        {"Smooth", "op",
         R"({"name": "frizz", "type": "Noise", "params": {"amplitude": 0.05, "frequency": 8.0, "octaves": 1}},
            {"name": "op", "type": "Smooth", "params": {"strength": 0.8, "scale": 0.2}})",
         link("interp", "frizz", "strands") + "," + link("frizz", "op", "strands")},
        {"SmoothStraightens", "op",  // scale 1: the Straighten node it replaced, the same output
         R"({"name": "frizz", "type": "Noise", "params": {"amplitude": 0.05, "frequency": 4.0, "octaves": 1}},
            {"name": "op", "type": "Smooth", "params": {"scale": 1.0, "strength": 0.7, "profile": [[0, 0.2], [1, 1]]}})",
         link("interp", "frizz", "strands") + "," + link("frizz", "op", "strands")},
        {"BendAtTheRoot", "op", R"({"name": "op", "type": "Bend", "params": {"angle": 25.0, "end": 0.0}})",
         link("interp", "op", "strands")},
        {"Transform", "op",
         R"({"name": "op", "type": "Transform", "params": {"translate": [0.01, 0.02, 0.0], "rotate": [10.0, 30.0, -5.0], "scale": 0.8,
             "translateRandom": [0.01, 0.0, 0.01], "rotateRandom": [5.0, 20.0, 0.0], "scaleRandom": 0.2, "seed": 3}})",
         link("interp", "op", "strands")},
        {"TransformWorld", "op",
         R"({"name": "op", "type": "Transform", "params": {"space": "world", "translate": [0.1, 0.0, 0.0], "rotate": [0.0, 25.0, 0.0],
             "scale": {"colorSet": "mask", "min": 0.8, "max": 1.2}, "pivot": [0.0, 1.0, 0.0]}})",
         link("interp", "op", "strands")},
        {"Bend", "op", R"({"name": "op", "type": "Bend", "params": {"angle": -40.0, "start": 0.2, "end": 0.8, "randomness": 0.3}})",
         link("interp", "op", "strands")},
        {"BendVector", "op",
         R"({"name": "op", "type": "Bend", "params": {"angle": 50.0, "along": "direction", "direction": [1.0, -1.0, 0.0], "end": 0.6}})",
         link("interp", "op", "strands")},
        {"Direction", "op",
         R"({"name": "op", "type": "Direction", "params": {"inclination": 60.0, "roll": 30.0, "rollRandom": 20.0, "inclinationRandom": 5.0, "seed": 2}})",
         link("interp", "op", "strands")},
        {"MeshCut", "op",
         R"({"name": "ball", "type": "ColliderMesh", "params": {"name": "collider"}},
            {"name": "op", "type": "MeshCut", "params": {"offset": 0.02}})",
         link("interp", "op", "strands") + "," + link("ball", "op", "mesh")},
        {"RenderData", "op",
         R"({"name": "op", "type": "RenderData", "params": {"opacityProfile": [[0, 1], [1, 0.3]], "opacityRandom": 0.2, "seed": 4}})",
         link("interp", "op", "strands")},
        // HairColor's highlight: a strength varied per strand, a spread by a map, the cuticle tilt.
        {"HairColorHighlight", "op",
         R"({"name": "op", "type": "HairColor", "params": {"specular": 0.45, "specularRandom": 0.3,
             "roughness": {"colorSet": "mask", "min": 0.25, "max": 0.75}, "roughnessRandom": 0.2, "tilt": 5.0, "seed": 4}})",
         link("interp", "op", "strands")},
        {"ClumpVariation", "op",
         R"({"name": "op", "type": "Clump", "params": {"count": 30, "strength": 0.8, "lockRadius": 0.03, "volumize": 0.5, "strengthRandom": 0.5,
             "lockRadiusRandom": 0.5, "copyAmount": 0.6, "copyNeighbors": 3}})",
         link("interp", "op", "strands")},
        {"ScatterGrid", "op", R"({"name": "op", "type": "Scatter", "params": {"layout": "uvGrid", "gridRows": 30, "gridColumns": 60, "gridRandom": 0.5}})",
         link("scalp", "op", "scalp")},
        {"Cull", "op", R"({"name": "op", "type": "Cull", "params": {"keep": {"expression": "u", "min": 0.2, "max": 1.0}}})",
         link("interp", "op", "strands")},
        {"AttributeMap", "len",
         R"({"name": "op", "type": "Attribute", "params": {"name": "mask", "value": {"expression": "v"}}},
            {"name": "len", "type": "Length", "params": {"keep": {"attribute": "mask", "min": 0.4}}})",
         link("interp", "op", "strands") + "," + link("op", "len", "strands")},
        {"ClumpLocks", "op",
         R"({"name": "op", "type": "Clump", "params": {"count": 20, "strength": 0.9, "lockRadius": 0.05}})",
         link("interp", "op", "strands")},
        {"BlendControl", "op",
         R"({"name": "wet", "type": "Curl", "params": {"radius": 0.05}},
            {"name": "amount", "type": "Control", "params": {"name": "wetness", "default": 0.6}},
            {"name": "op", "type": "Blend", "params": {"profile": [[0, 0], [1, 1]]}, "paramInputs": ["mix"]})",
         link("interp", "wet", "strands") + "," + link("interp", "op", "a") + "," + link("wet", "op", "b") + "," +
             link("amount", "op", "mix")},
        {"CurveProfile", "op",
         R"({"name": "bell", "type": "Curve", "params": {"curve": [[0, 0], [0.5, 1], [1, 0.3]], "interpolation": "ease"}},
            {"name": "op", "type": "Clump", "params": {"count": 30, "strength": 0.8}, "paramInputs": ["profile"]})",
         link("interp", "op", "strands") + "," + link("bell", "op", "profile")},
        // A Value node setting a count (a Value port: one value for the whole node).
        {"ValueCount", "op",
         R"({"name": "count", "type": "Value", "params": {"kind": "integer", "integer": 24}},
            {"name": "op", "type": "Clump", "params": {"strength": 0.8}, "paramInputs": ["count"]})",
         link("interp", "op", "strands") + "," + link("count", "op", "count")},
        {"HairColor", "op",
         R"({"name": "clump", "type": "Clump", "params": {"count": 30, "strength": 0.5}},
            {"name": "op", "type": "HairColor", "params": {"melanin": 0.4, "melaninRandom": 0.2, "greys": 0.1, "seed": 3}})",
         link("interp", "clump", "strands") + "," + link("clump", "op", "strands")},
        {"HairColorRgb", "op",
         R"({"name": "clump", "type": "Clump", "params": {"count": 30, "strength": 0.5}},
            {"name": "op", "type": "HairColor", "params": {"melanin": 0.2, "dye": [1.0, 0.8, 0.6], "map": "mask",
             "hueRandom": 0.1, "saturationRandom": 0.2, "valueRandom": 0.3, "tipColor": [0.8, 0.7, 0.5], "tipStrength": 0.5, "seed": 5}})",
         link("interp", "clump", "strands") + "," + link("clump", "op", "strands")},
        // Bands over HairColor's colors: two to a strand, guard hairs at random and a tinted undercoat.
        {"Bands", "op",
         R"({"name": "color", "type": "HairColor", "params": {"melanin": 0.6, "tipStrength": 0.4, "seed": 2}},
            {"name": "op", "type": "Bands", "params": {"count": 2, "guards": 0.7, "undercoatDye": [0.8, 0.75, 0.7], "seed": 2}})",
         link("interp", "color", "strands") + "," + link("color", "op", "strands")},
        {"Flyaway", "op", R"({"name": "op", "type": "Flyaway", "params": {"fraction": 0.2, "stray": 0.2, "seed": 2}})",
         link("interp", "op", "strands")},
        {"Cards", "op", R"({"name": "op", "type": "Cards", "params": {"segments": 4}})",  // strands pass through
         link("interp", "op", "strands")},
        {"Flow", "op", R"({"name": "op", "type": "Flow", "params": {"lift": 20.0, "bend": 0.5, "seed": 3}})",
         link("interp", "op", "strands")},
        {"Tangle", "op", R"({"name": "op", "type": "Tangle", "params": {"radius": 0.3, "strength": 1.0, "seed": 6}})",
         link("interp", "op", "strands")},
        {"Wet", "op", R"({"name": "op", "type": "Wet", "params": {"wetness": 0.8, "clumpSize": 0.6, "seed": 2}})",
         link("interp", "op", "strands")},
        {"SelfCollide", "op",
         R"({"name": "lay", "type": "Gravity", "params": {"strength": 1.0}},
            {"name": "op", "type": "SelfCollide", "params": {"packing": 0.02}})",
         link("interp", "lay", "strands") + "," + link("lay", "op", "strands")},
        {"Static", "op",
         R"({"name": "clump", "type": "Clump", "params": {"count": 30, "strength": 0.7}},
            {"name": "op", "type": "Static", "params": {"strength": 0.8, "randomness": 0.6}})",
         link("interp", "clump", "strands") + "," + link("clump", "op", "strands")},
        {"ToGuidesClump", "op",
         R"({"name": "centers", "type": "ToGuides", "params": {"fraction": 0.05, "seed": 2}},
            {"name": "op", "type": "Clump", "params": {"strength": 0.6, "centersFrom": "inputPoints"}})",
         link("interp", "centers", "strands") + "," + link("interp", "op", "strands") + "," +
             link("centers", "op", "centers")},
        {"Clump", "op",
         R"({"name": "op", "type": "Clump", "params": {"count": 30, "strength": 0.7}})",
         link("interp", "op", "strands")},
        {"ClumpGuides", "op", R"({"name": "op", "type": "Clump", "params": {"strength": {"colorSet": "mask"}, "centersFrom": "inputCurves"}})",
         link("interp", "op", "strands") + "," + link("primaries", "op", "centers")},
        // A sparse Scatter's spikes as the centers: they place the clumps, the strands shape them.
        {"ClumpScatterCenters", "op",
         R"({"name": "spots", "type": "Scatter", "params": {"density": 15.0, "seed": 3}},
            {"name": "op", "type": "Clump", "params": {"strength": 0.8, "pastTip": "extend", "copyAmount": 0.3,
             "centersFrom": "inputPoints"}})",
         link("scalp", "spots", "scalp") + "," + link("interp", "op", "strands") + "," + link("spots", "op", "centers")},
        {"ClumpOwnCenters", "op",  // centersFrom pick: centers of its own, by size (darts on the scalp)
         R"({"name": "op", "type": "Clump", "params": {"strength": 0.7, "clumpBy": "size", "size": 0.35}})",
         link("interp", "op", "strands")},
        {"NoiseWorld", "op", R"({"name": "op", "type": "Noise", "params": {"amplitude": 0.05, "frequency": 3.0, "space": "world"}})",
         link("interp", "op", "strands")},
        {"NoiseStrand", "op",
         R"({"name": "op", "type": "Noise", "params": {"amplitude": 0.05, "space": "strand", "octaves": 3}})",
         link("interp", "op", "strands")},
        {"Curl", "op", R"({"name": "op", "type": "Curl", "params": {"radius": 0.03, "randomness": 0.5}})",
         link("interp", "op", "strands")},
        {"NoiseFrizz", "op",
         R"({"name": "op", "type": "Noise", "params": {"fraction": 0.3, "amplitude": 0.04, "frequency": 8.0, "octaves": 1}})",
         link("interp", "op", "strands")},
        {"Attribute", "op",
         R"({"name": "op", "type": "Attribute", "params": {"name": "melanin", "value": {"expression": "rand", "min": 0.2, "max": 0.8}}})",
         link("interp", "op", "strands")},
        {"NoiseAttributeMap", "op",
         R"({"name": "attr", "type": "Attribute", "params": {"name": "w", "value": {"expression": "rand"}}},
            {"name": "op", "type": "Noise", "params": {"amplitude": {"attribute": "w", "max": 0.1}}})",
         link("interp", "attr", "strands") + "," + link("attr", "op", "strands")},
        {"Merge", "op",
         R"({"name": "more", "type": "Scatter", "params": {"density": 100.0, "seed": 7}},
            {"name": "op", "type": "Merge"})",
         link("scalp", "more", "scalp") + "," + link("interp", "op", "a") + "," + link("more", "op", "b")},
        {"Switch", "op",
         R"({"name": "more", "type": "Scatter", "params": {"density": 100.0, "seed": 7}},
            {"name": "op", "type": "Switch", "params": {"index": 1}})",
         link("scalp", "more", "scalp") + "," + link("interp", "op", "a") + "," + link("more", "op", "b")},
        {"BendComb", "op",
         R"({"name": "op", "type": "Bend", "params": {"angle": 180.0, "end": 0.0, "along": "direction", "direction": [0.0, -1.0, 0.3]}})",
         link("interp", "op", "strands")},
        {"Collide", "op",
         R"({"name": "down", "type": "Gravity", "params": {"strength": 3.0, "stiffness": 0.0}},
            {"name": "op", "type": "Collide", "params": {"offset": 0.02}})",
         link("interp", "down", "strands") + "," + link("down", "op", "strands") + "," + link("scalp", "op", "scalp")},
        {"CollideExact", "op",
         R"({"name": "down", "type": "Gravity", "params": {"strength": 3.0, "stiffness": 0.0}},
            {"name": "op", "type": "Collide", "params": {"offset": 0.02, "method": "exact"}})",
         link("interp", "down", "strands") + "," + link("down", "op", "strands") + "," + link("scalp", "op", "scalp")},
        {"Gravity", "op",
         R"({"name": "op", "type": "Gravity", "params": {"strength": 1.5, "direction": [0.3, -1.0, 0.0]}})",
         link("interp", "op", "strands")},
        {"GravityDirectionExpression", "op",
         R"j({"name": "op", "type": "Gravity", "params": {"strength": 1.5, "direction": {"expression": "normalize([x, -2, 0])"}}})j",
         link("interp", "op", "strands")},
        // A group by region, and a node restricted to a group (its affect mapped to the group's channel).
        {"Group", "op",
         R"({"name": "op", "type": "Group", "params": {"name": "crown", "center": [0.0, 1.0, 0.0], "radius": 0.8, "falloff": 0.3}})",
         link("interp", "op", "strands")},
        {"GroupAffect", "op",
         R"j({"name": "front", "type": "Group", "params": {"name": "front", "by": "map", "value": {"expression": "z + 0.3 * snoise(P * 2)"}, "threshold": 0.0, "softness": 0.4}},
            {"name": "op", "type": "Noise", "params": {"amplitude": 0.08, "affect": {"attribute": "front"}}})j",
         link("interp", "front", "strands") + "," + link("front", "op", "strands")},
        {"GravityCollide", "op",  // hung, then kept off the scalp
         R"({"name": "hang", "type": "Gravity", "params": {"strength": 3.0, "stiffness": 0.2}},
            {"name": "op", "type": "Collide", "params": {"offset": 0.02}})",
         link("interp", "hang", "strands") + "," + link("hang", "op", "strands") + "," + link("scalp", "op", "scalp")},
        {"Displace", "op",
         R"({"name": "op", "type": "Displace", "params": {"distance": {"colorSet": "mask", "min": 0.0, "max": 0.2}}})",
         link("interp", "op", "strands")},
        {"Mirror", "op", R"({"name": "op", "type": "Mirror", "params": {"axis": "x", "offset": 0.1}})", link("interp", "op", "strands")},
        {"MirrorGuides", "op", R"({"name": "op", "type": "Mirror", "params": {"mode": "add", "axis": "z"}})",
         link("primaries", "op", "strands")},
        {"NoiseKinky", "op", R"({"name": "op", "type": "Noise", "params": {"mode": "kinky", "amplitude": 0.04, "frequency": 6.0}})",
         link("interp", "op", "strands")},
        {"NoiseKinkyWorld", "op",
         R"({"name": "op", "type": "Noise", "params": {"mode": "kinky", "space": "world", "amplitude": 0.04, "frequency": 4.0}})",
         link("interp", "op", "strands")},
        {"WidthRandom", "op", R"({"name": "op", "type": "Width", "params": {"width": 0.01, "randomness": 0.5, "seed": 2}})",
         link("interp", "op", "strands")},
        {"CurlWave", "op", R"({"name": "op", "type": "Curl", "params": {"shape": "wave", "radius": 0.03, "phase": 0.3, "phaseRandom": 0.5}})",
         link("interp", "op", "strands")},
        {"CurlZigzag", "op", R"({"name": "op", "type": "Curl", "params": {"shape": "zigzag", "radius": 0.03, "frequency": 6.0}})",
         link("interp", "op", "strands")},
        {"CurlCoil", "op",
         R"({"name": "width", "type": "Width", "params": {"width": 0.01}},
            {"name": "op", "type": "Curl", "params": {"shape": "coil", "coilWidths": 2.0, "frequency": 12.0}})",
         link("interp", "width", "strands") + "," + link("width", "op", "strands")},
        {"CurlClumps", "op",
         R"({"name": "clump", "type": "Clump", "params": {"count": 30, "strength": 0.7}},
            {"name": "op", "type": "Curl", "params": {"coherence": "clump", "radius": 0.04}})",
         link("interp", "clump", "strands") + "," + link("clump", "op", "strands")},
        {"ClumpTwist", "op",  // round locks turned into ropes by a Twist after the Clump
         R"({"name": "clump", "type": "Clump", "params": {"count": 30, "strength": 0.8, "lockRadius": 0.04}},
            {"name": "op", "type": "Twist", "params": {"turns": 1.5}})",
         link("interp", "clump", "strands") + "," + link("clump", "op", "strands")},
        {"ClumpFlat", "op",
         R"({"name": "op", "type": "Clump", "params": {"count": 30, "strength": 0.8, "flatness": 0.8}})",
         link("interp", "op", "strands")},
        {"Output", "op", R"({"name": "op", "type": "Output"})", link("interp", "op", "strands")},
        {"Wind", "op",
         R"({"name": "op", "type": "Wind", "params": {"direction": [1.0, -0.2, 0.3], "strength": 0.5, "gust": 0.8, "frequency": 0.7, "speed": 3.0, "turbulence": 0.4, "seed": 2}})",
         link("interp", "op", "strands"), false, 1.75},
        {"Jiggle", "op",
         R"({"name": "op", "type": "Jiggle", "params": {"strength": 1.2, "frequency": 2.5, "damping": 0.25, "history": 10}})",
         link("interp", "op", "strands"), false, 0.0, true},
        // Twelve frames of the solver on the guides (the session's frame as the external "sim:op"),
        // the strands grown from them.
        {"Simulate", "grown",
         R"({"name": "op", "type": "Simulate", "params": {"gravity": 9.8, "bend": 0.3, "collisionOffset": 0.02, "windSpeed": 1.5, "seed": 3}},
            {"name": "grown", "type": "Interpolate"})",
         link("guides", "op", "guides") + "," + link("scatter", "grown", "strands") + "," + link("op", "grown", "guides") + "," +
             link("scalp", "grown", "scalp"),
         false, 0.0, false, 12},
        // Edits on the guides (the session's as the external "sculpt:op": every other guide hooked
        // near its tip, the rest shortened), the strands grown from them.
        {"Sculpt", "grown",
         R"({"name": "op", "type": "Sculpt", "params": {"strength": 0.8}},
            {"name": "grown", "type": "Interpolate"})",
         link("guides", "op", "guides") + "," + link("scalp", "op", "scalp") + "," + link("scatter", "grown", "strands") + "," +
             link("op", "grown", "guides") + "," + link("scalp", "grown", "scalp"),
         false, 0.0, false, 0, false, true},
        {"Resample", "op", R"({"name": "op", "type": "Resample", "params": {"segments": 7, "mode": "length", "minSegments": 2}})",
         link("interp", "op", "strands")},
        {"PartedClump", "part",
         R"({"name": "clump", "type": "Clump", "params": {"count": 30, "strength": 0.7}},
            {"name": "part", "type": "Part", "params": {"width": 0.25}})",
         link("interp", "clump", "strands") + "," + link("clump", "part", "strands"),
         true},
        {"MapColorSet", "len",
         R"({"name": "op", "type": "Attribute", "params": {"name": "mask", "value": {"colorSet": "mask"}}},
            {"name": "len", "type": "Length", "params": {"keep": {"attribute": "mask", "min": 0.4}}})",
         link("interp", "op", "strands") + "," + link("op", "len", "strands")},
        {"Compound", "op",
         R"({"name": "op", "type": "Compound", "params": {"amount": 0.08, "definition": {
              "graph": {"version": 1, "output": "curl", "nodes": [
                  {"name": "in", "type": "Input", "params": {"name": "strands"}},
                  {"name": "noise", "type": "Noise", "params": {"amplitude": 0.02}},
                  {"name": "curl", "type": "Curl"}],
                "connections": [{"from": "in", "to": "noise", "input": "strands"},
                                {"from": "noise", "to": "curl", "input": "strands"}]},
              "inputs": [{"name": "strands", "type": "Strands"}],
              "expose": {"amount": "noise.amplitude"}}}})",
         link("interp", "op", "strands")},
        {"Script", "op",
         R"({"name": "op", "type": "Script", "params": {"amount": 0.05, "definition": {
              "glnode": 1, "id": "test.golden", "version": 1, "label": "Golden",
              "inputs": [{"name": "strands", "type": "Strands"}, {"name": "guides", "type": "Guides", "optional": true}],
              "params": [{"name": "amount", "type": "length", "default": 0.02, "mappable": true}],
              "writes": ["P", "width", "channel.kinked"],
              "script": "per strand { $k = 0; for $i in 0 .. 3 { if (hash(id, $i) < 0.3) { $k += 1; } } channel.kinked = $k; } per point { P = mix(P, guidePoint(t), 0.2 * t) + vnoise(P * 4) * $amount * t; width = 0.01 * (1 - 0.8 * t); }"}}})",
         link("interp", "op", "strands") + "," + link("guides", "op", "guides")},
        {"BlendNeighbors", "op",
         R"({"name": "noise", "type": "Noise", "params": {"amplitude": 0.05, "frequency": 3.0}},
            {"name": "op", "type": "BlendNeighbors", "params": {"strength": 0.8, "neighbors": 6}})",
         link("interp", "noise", "strands") + "," + link("noise", "op", "strands")},
        {"RestoreLength", "op",
         R"({"name": "noise", "type": "Noise", "params": {"amplitude": 0.05, "frequency": 3.0}},
            {"name": "op", "type": "RestoreLength", "params": {"affect": 0.9}})",
         link("interp", "noise", "strands") + "," + link("noise", "op", "strands") + "," + link("interp", "op", "reference")},
        {"ColliderMeshCollide", "op",
         R"({"name": "ball", "type": "ColliderMesh", "params": {"name": "collider"}},
            {"name": "op", "type": "Collide", "params": {"offset": 0.02, "iterations": 4}})",
         link("interp", "op", "strands") + "," + link("scalp", "op", "scalp") + "," + link("ball", "op", "collider")},
        {"ColliderMeshCollideExact", "op",
         R"({"name": "ball", "type": "ColliderMesh", "params": {"name": "collider"}},
            {"name": "op", "type": "Collide", "params": {"offset": 0.02, "iterations": 4, "method": "exact"}})",
         link("interp", "op", "strands") + "," + link("scalp", "op", "scalp") + "," + link("ball", "op", "collider")},
        {"CompoundRepeat", "op",
         R"({"name": "op", "type": "Compound", "params": {"repeat": 3, "definition": {
              "graph": {"version": 1, "output": "noise", "nodes": [
                  {"name": "in", "type": "Input", "params": {"name": "strands"}},
                  {"name": "pass", "type": "Input", "params": {"name": "iteration", "type": "Field"}},
                  {"name": "remap", "type": "FieldRemap", "params": {"inMin": 0.0, "inMax": 2.0, "outMin": 0.01, "outMax": 0.03}},
                  {"name": "noise", "type": "Noise", "params": {"frequency": 2.0}, "paramInputs": ["amplitude"]}],
                "connections": [{"from": "in", "to": "noise", "input": "strands"},
                                {"from": "pass", "to": "remap", "input": "in"},
                                {"from": "remap", "to": "noise", "input": "amplitude"}]},
              "inputs": [{"name": "strands", "type": "Strands"}]}}})",
         link("interp", "op", "strands")},
        {"Propagate", "op",
         R"({"name": "op", "type": "Propagate", "params": {"count": 6, "segments": 4, "splits": 3, "asymmetry": 0.3, "lift": 5.0, "seed": 2}})",
         link("interp", "op", "shafts")},
        {"PropagateGuidesAround", "op",
         R"({"name": "op", "type": "Propagate", "params": {"sides": "around", "count": 5, "segments": 3, "length": {"colorSet": "mask", "min": 0.1, "max": 0.3}}})",
         link("primaries", "op", "shafts")},
        {"PropagateClump", "op",
         R"({"name": "feathers", "type": "Propagate", "params": {"count": 8, "segments": 4}},
            {"name": "op", "type": "Clump", "params": {"count": 200, "strength": 0.8}})",
         link("interp", "feathers", "shafts") + "," + link("feathers", "op", "strands")},
        // Bars on feathers, a down color and iridescence on the right vanes.
        {"FeatherColor", "op",
         R"({"name": "feathers", "type": "Propagate", "params": {"count": 8, "segments": 4, "down": 0.3, "seed": 2}},
            {"name": "color", "type": "HairColor", "params": {"melanin": 0.5}},
            {"name": "op", "type": "FeatherColor", "params": {"count": 4.0, "vanes": "left", "downStrength": 0.7,
             "iridescence": 0.6, "iridescenceOn": "rightVane", "seed": 3}})",
         link("interp", "feathers", "shafts") + "," + link("feathers", "color", "strands") + "," + link("color", "op", "strands")},
        // Feathers laid over one another (rigid, by root distance) and off the body.
        {"Plume", "op",
         R"({"name": "feathers", "type": "Propagate", "params": {"count": 6, "segments": 4, "droop": 20.0, "seed": 2}},
            {"name": "op", "type": "Plume", "params": {"offset": 0.005, "bodyOffset": 0.002}})",
         link("interp", "feathers", "shafts") + "," + link("feathers", "op", "strands")},
        // Bending (stiffness), shorter feathers on top, a collider for the body too.
        {"PlumeBend", "op",
         R"({"name": "feathers", "type": "Propagate", "params": {"count": 6, "segments": 4, "droop": 20.0, "seed": 2}},
            {"name": "ball", "type": "ColliderMesh", "params": {"name": "collider"}},
            {"name": "op", "type": "Plume", "params": {"offset": 0.005, "stiffness": 0.3, "rankBy": "length", "maxAngle": 40.0}})",
         link("interp", "feathers", "shafts") + "," + link("feathers", "op", "strands") + "," + link("ball", "op", "collider")},
        {"Split", "op", R"({"name": "op", "type": "Split", "params": {"weightA": 2.0, "weightB": 1.0, "seed": 3}})",
         link("interp", "op", "strands")},
        {"SplitMapFeathers", "op",
         R"({"name": "split", "type": "Split", "params": {"mode": "map", "select": {"colorSet": "mask"}}},
            {"name": "op", "type": "Propagate", "params": {"count": 4, "segments": 3, "keepShaft": false}})",
         R"({"from": "interp", "to": "split", "input": "strands"},
            {"from": "split", "to": "op", "input": "shafts", "output": "b"})"},
        // Subdivide: a render's one level (each segment halved along the smooth curve).
        {"Subdivide", "op", R"({"name": "op", "type": "Subdivide", "params": {"levels": 1}})", link("interp", "op", "strands")},
    };
}

// Strands from a strand or guide output.
StrandSet asStrands(const Value& v) {
    if (const StrandsPtr s = valueAs<StrandsPtr>(v)) return *s;
    StrandSet out;
    if (const GuidesPtr g = valueAs<GuidesPtr>(v)) {
        for (const Guide& guide : g->guides()) out.addStrand(guide.id, guide.points, {}, guide.root);
    }
    return out;
}

struct Scene {
    MeshPtr scalp = goldenScalp();
    HierarchyPtr hierarchy;
    ExternalInputs ext;
    ExternalInputs flapExt;  // the flap's scalp and guides
    Scene() {
        auto h = std::make_shared<GuideHierarchy>(GuideHierarchy::fromJson(goldenHierarchy(*scalp)));
        h->evaluate(scalp.get());
        hierarchy = h;
        setGroomExternals(ext, scalp, nullptr, hierarchy);
        setMeshExternals(ext, {{"collider", goldenCollider()}});
        const MeshPtr flap = flapScalp();
        auto f = std::make_shared<GuideHierarchy>(GuideHierarchy::fromJson(flapHierarchy(*flap)));
        f->evaluate(flap.get());
        setGroomExternals(flapExt, flap, nullptr, f);
    }
    StrandSet run(const Case& c) {
        GraphDocument doc = loadGraphJson(graphFor(c));
        EvalContext ctx;
        ctx.strandSegments = kSegments;
        ctx.externals = c.flap ? &flapExt : &ext;
        ExternalInputs withParts;
        if (c.parts) {
            PartSet set;
            std::vector<Vec3f> meridian;
            for (int i = 0; i <= 10; ++i) {
                const float a = 0.15f + 1.2f * static_cast<float>(i) / 10.0f;  // from near the top, down the +z side
                meridian.push_back({0.0f, std::cos(a), std::sin(a)});
            }
            set.parts.push_back(PartSet::fromPoints("meridian", meridian, *scalp));
            const auto lines = std::make_shared<const PartLines>(set, *scalp);
            withParts.setParent(&ext);
            withParts.set("parts", PartsPtr(lines), lines->contentHash());
            ctx.externals = &withParts;
        }
        ctx.time = c.time;
        ExternalInputs withHistory;
        if (c.motion) {
            // The scalp swung along +x and tipped over the last frames, stopping two frames ago.
            std::vector<std::vector<Vec3f>> past;
            for (int k = 1; k <= 12; ++k) {
                const float moved = static_cast<float>(std::clamp(k - 2, 0, 6));
                std::vector<Vec3f> points = scalp->points();
                for (Vec3f& p : points) {
                    p = rotateAxisAngle(p, {0.0f, 0.0f, 1.0f}, 0.02f * moved);
                    p.x -= 0.05f * moved;
                }
                past.push_back(std::move(points));
            }
            const ScalpHistoryPtr history = ScalpHistory::make(1.0 / 24.0, std::move(past));
            withHistory.setParent(ctx.externals);
            withHistory.set("scalpHistory", history, history->hash);
            ctx.externals = &withHistory;
        }
        ExternalInputs withFrame;
        if (c.simulate > 0) {
            // As the session steps it: from rest on the guides it is fed, then frame by frame.
            const NodeId sim = doc.node("op");
            NodeId source = 0;
            doc.graph.inputSource(sim, 0, &source);
            const SimInputs in{valueAs<GuidesPtr>(doc.graph.evaluate(source, ctx)), scalp, {}};
            SimCache cache;
            cache.configure(SimSettings::fromParams(doc.graph.node(sim).params()), 24.0);
            cache.start(in, 0);
            for (int f = 0; f < c.simulate; ++f) cache.step(in, 0);
            const GuidesPtr frame = cache.frame(cache.lastFrame());
            withFrame.setParent(ctx.externals);
            withFrame.set(simulationExternal("op"), frame, frame->contentHash());
            ctx.externals = &withFrame;
        }
        ExternalInputs withSculpts;
        if (c.sculpt) {
            // Recorded on the guides the node is fed: every other one hooked about the axis across
            // its tip segment, the rest shortened by a fifth.
            const NodeId op = doc.node("op");
            NodeId source = 0;
            doc.graph.inputSource(op, 0, &source);
            const GuidesPtr fed = valueAs<GuidesPtr>(doc.graph.evaluate(source, ctx));
            auto edits = std::make_shared<SculptEdits>();
            for (std::size_t i = 0; i < fed->size(); ++i) {
                const Guide& g = fed->guides()[i];
                const std::size_t n = g.points.size();
                if (n < 4) continue;
                std::vector<Vec3f> sculpted = g.points;
                if (i % 2 == 0) {
                    const Vec3f pivot = g.points[n - 4];
                    const Vec3f along = (g.points[n - 1] - pivot).normalized();
                    Vec3f axis = along.cross(Vec3f(0.0f, 1.0f, 0.0f));
                    axis = axis.length() > 1e-3f ? axis.normalized() : Vec3f(1.0f, 0.0f, 0.0f);
                    for (std::size_t k = n - 3; k < n; ++k) sculpted[k] = pivot + rotateAxisAngle(g.points[k] - pivot, axis, 0.6f);
                } else {
                    for (std::size_t k = 1; k < n; ++k) sculpted[k] = g.points[0] + (g.points[k] - g.points[0]) * 0.8f;
                }
                edits->guides[g.id] = recordSculpt(g, sculpted, 1.0f, scalp.get());
            }
            withSculpts.setParent(ctx.externals);
            withSculpts.set(sculptExternal("op"), SculptPtr(edits), edits->contentHash());
            ctx.externals = &withSculpts;
        }
        return asStrands(doc.graph.evaluate(doc.node(c.output), ctx));
    }
    // The case's output evaluated for cards (the Cards nodes build theirs): the strands with
    // their card layers.
    StrandsPtr runCards(const Case& c) {
        GraphDocument doc = loadGraphJson(graphFor(c));
        EvalContext ctx;
        ctx.strandSegments = kSegments;
        ctx.externals = &ext;
        ctx.buildCards = true;
        return doc.graph.evaluateStrands(doc.node(c.output), ctx);
    }
};

std::uint64_t idsHash(const StrandSet& s) {
    std::uint64_t h = hash64(s.strandCount());
    for (const StrandId id : s.ids()) h = hashCombine(h, id);
    return h;
}

// About 64 strands in full, plus counts, IDs and channels.
json summarize(const StrandSet& s) {
    const std::size_t stride = std::max<std::size_t>(1, s.strandCount() / 64);
    json j;
    j["strands"] = s.strandCount();
    j["points"] = s.pointCount();
    j["idsHash"] = std::to_string(idsHash(s));
    json samples = json::array();
    for (std::size_t i = 0; i < s.strandCount(); i += stride) {
        json pts = json::array();
        for (std::size_t k = 0; k < s.strandPointCount(i); ++k) {
            const Vec3f& p = s.strandPoints(i)[k];
            pts.push_back({p.x, p.y, p.z});
        }
        json widths = json::array();
        for (std::size_t k = s.strandBegin(i); k < s.strandBegin(i) + s.strandPointCount(i) && k < s.widths().size(); ++k) {
            widths.push_back(s.widths()[k]);
        }
        samples.push_back({{"strand", i}, {"points", pts}, {"widths", widths}});
    }
    j["samples"] = samples;
    json channels = json::object();
    for (const std::string& name : s.strandChannelNames()) {
        json values = json::array();
        const std::vector<float>& v = *s.strandChannel(name);
        for (std::size_t i = 0; i < v.size(); i += stride) values.push_back(v[i]);
        channels[name] = values;
    }
    j["channels"] = channels;
    // Instances (the Instance node), placed on these strands: about 64 of them.
    if (s.instances() && !s.instances()->empty()) {
        const auto placed = s.instances()->resolve(s);
        json inst = json::array();
        const std::size_t every = std::max<std::size_t>(1, placed.size() / 64);
        for (std::size_t i = 0; i < placed.size(); i += every) {
            const auto& p = placed[i];
            inst.push_back({{"id", std::to_string(p.id)},
                            {"origin", {p.origin.x, p.origin.y, p.origin.z}},
                            {"y", {p.y.x, p.y.y, p.y.z}},
                            {"scale", p.x.length()}});
        }
        j["instances"] = {{"count", placed.size()}, {"samples", inst}};
    }
    return j;
}

void expectMatches(const json& expected, const json& actual, const std::string& name) {
    ASSERT_EQ(expected.at("strands"), actual.at("strands")) << name;
    ASSERT_EQ(expected.at("points"), actual.at("points")) << name;
    EXPECT_EQ(expected.at("idsHash"), actual.at("idsHash")) << name << ": strand IDs differ";
    const json& es = expected.at("samples");
    const json& as = actual.at("samples");
    ASSERT_EQ(es.size(), as.size()) << name;
    float worst = 0.0f;
    for (std::size_t i = 0; i < es.size(); ++i) {
        const json& ep = es[i].at("points");
        const json& ap = as[i].at("points");
        ASSERT_EQ(ep.size(), ap.size()) << name << " sample " << i;
        for (std::size_t k = 0; k < ep.size(); ++k) {
            for (int c = 0; c < 3; ++c) worst = std::max(worst, std::abs(ep[k][c].get<float>() - ap[k][c].get<float>()));
        }
        const json& ew = es[i].at("widths");
        const json& aw = as[i].at("widths");
        ASSERT_EQ(ew.size(), aw.size()) << name;
        for (std::size_t k = 0; k < ew.size(); ++k) worst = std::max(worst, std::abs(ew[k].get<float>() - aw[k].get<float>()));
    }
    EXPECT_LE(worst, kPositionTolerance) << name << ": sampled points differ by up to " << worst;
    const json& ec = expected.at("channels");
    const json& ac = actual.at("channels");
    ASSERT_EQ(ec.size(), ac.size()) << name;
    for (const auto& [channel, values] : ec.items()) {
        ASSERT_TRUE(ac.contains(channel)) << name << ": channel " << channel;
        for (std::size_t i = 0; i < values.size(); ++i) {
            EXPECT_NEAR(values[i].get<float>(), ac.at(channel)[i].get<float>(), kPositionTolerance) << name << " " << channel;
        }
    }
    ASSERT_EQ(expected.contains("instances"), actual.contains("instances")) << name << ": instances";
    if (!expected.contains("instances")) return;
    const json& ei = expected.at("instances");
    const json& ai = actual.at("instances");
    ASSERT_EQ(ei.at("count"), ai.at("count")) << name << ": instance count";
    ASSERT_EQ(ei.at("samples").size(), ai.at("samples").size()) << name;
    for (std::size_t i = 0; i < ei.at("samples").size(); ++i) {
        const json& e = ei.at("samples")[i];
        const json& a = ai.at("samples")[i];
        EXPECT_EQ(e.at("id"), a.at("id")) << name << " instance " << i;
        for (const char* key : {"origin", "y"}) {
            for (int c = 0; c < 3; ++c) {
                EXPECT_NEAR(e.at(key)[c].get<float>(), a.at(key)[c].get<float>(), kPositionTolerance) << name << " instance " << i << " " << key;
            }
        }
        EXPECT_NEAR(e.at("scale").get<float>(), a.at("scale").get<float>(), kPositionTolerance) << name << " instance " << i;
    }
}

void PrintTo(const Case& c, std::ostream* os) { *os << c.name; }

bool updating() {
    const char* v = std::getenv("GROOMLAB_UPDATE_GOLDEN");
    return v && *v && std::string(v) != "0";
}

class Golden : public ::testing::TestWithParam<Case> {};

TEST_P(Golden, MatchesStoredOutput) {
    static Scene scene;
    const Case& c = GetParam();
    const StrandSet out = scene.run(c);
    EXPECT_GT(out.strandCount(), 0u) << c.name;
    const json actual = summarize(out);
    const fs::path file = kData / "golden" / (std::string(c.name) + ".json");

    if (updating()) {
        std::ofstream(file, std::ios::binary) << actual.dump() << "\n";
        GTEST_SKIP() << "wrote " << file.generic_string();
    }
    std::ifstream in(file, std::ios::binary);
    ASSERT_TRUE(in) << "no golden file " << file.generic_string() << " (run with GROOMLAB_UPDATE_GOLDEN=1)";
    std::stringstream text;
    text << in.rdbuf();
    expectMatches(json::parse(text.str()), actual, c.name);

#if GROOMLAB_HAS_TBB
    // One thread and many must agree to the bit.
    StrandSet serial;
    tbb::task_arena single(1);
    single.execute([&] { serial = scene.run(c); });
    EXPECT_EQ(serial.ids(), out.ids()) << c.name;
    EXPECT_EQ(serial.positions(), out.positions()) << c.name << ": thread count changes the output";
    EXPECT_EQ(serial.widths(), out.widths()) << c.name;
    EXPECT_EQ(serial.instances() ? serial.instances()->contentHash() : 0, out.instances() ? out.instances()->contentHash() : 0)
        << c.name << ": thread count changes the instances";
#endif
}

INSTANTIATE_TEST_SUITE_P(Operators, Golden, ::testing::ValuesIn(cases()),
                         [](const ::testing::TestParamInfo<Case>& info) { return std::string(info.param.name); });

// ---------------------------------------------------------------------------
// Card goldens: the cards the Cards nodes of a case build (laid out in one atlas as the host
// does), compared with tests/data/golden/Cards_<case>.json: counts, variations and mirroring
// exactly, cells, station widths and sampled vertices within the tolerance.

std::vector<Case> cardCases() {
    const std::string clumped =
        R"({"name": "clump", "type": "Clump", "params": {"count": 30, "strength": 0.7}})";
    const std::string clumpLinks = link("interp", "clump", "strands");
    return {
        {"Hair", "op",
         clumped + R"(, {"name": "op", "type": "Cards", "params": {"segments": 4, "cardsPerClump": 2, "variations": 6}})",
         clumpLinks + "," + link("clump", "op", "strands")},
        {"HairCross", "op",
         clumped + R"(, {"name": "op", "type": "Cards", "params": {"segments": 5, "crossStyle": "cross", "maxCards": 40, "flyawayCards": 4}})",
         clumpLinks + "," + link("clump", "op", "strands")},
        {"Feathers", "op",
         R"({"name": "feathers", "type": "Propagate", "params": {"count": 6, "segments": 4, "asymmetry": 0.3, "seed": 2}},
            {"name": "op", "type": "Cards", "params": {"segments": 4, "variations": 4}})",
         link("guides", "feathers", "shafts") + "," + link("feathers", "op", "strands")},
        // The same without the spine: the flat strips as they were before featherSpine.
        {"FeathersFlat", "op",
         R"({"name": "feathers", "type": "Propagate", "params": {"count": 6, "segments": 4, "asymmetry": 0.3, "seed": 2}},
            {"name": "op", "type": "Cards", "params": {"segments": 4, "variations": 4, "featherSpine": false}})",
         link("guides", "feathers", "shafts") + "," + link("feathers", "op", "strands")},
        {"FeathersDown", "op",
         R"({"name": "feathers", "type": "Propagate", "params": {"sides": "around", "count": 5, "segments": 3, "length": 0.2}},
            {"name": "op", "type": "Cards", "params": {"variations": 2}})",
         link("primaries", "feathers", "shafts") + "," + link("feathers", "op", "strands")},
        {"FeathersPerVane", "op",
         R"({"name": "feathers", "type": "Propagate", "params": {"count": 6, "segments": 4, "splits": 2, "seed": 2}},
            {"name": "op", "type": "Cards", "params": {"segments": 4, "featherMode": "perVane"}})",
         link("guides", "feathers", "shafts") + "," + link("feathers", "op", "strands")},
    };
}

json summarizeCards(const StrandSet& s) {
    CardMesh cards = combineCardLayers(s.cardLayers());
    layoutCards(cards, cards.padding);
    json j;
    j["cards"] = cards.cards.size();
    j["vertices"] = cards.vertexCount();
    j["triangles"] = cards.triangleCount();
    j["textures"] = cards.textureCount();
    json list = json::array();
    const std::size_t every = std::max<std::size_t>(1, cards.cards.size() / 64);
    for (std::size_t i = 0; i < cards.cards.size(); i += every) {
        const CardMesh::Card& c = cards.cards[i];
        json card;
        card["strands"] = c.strands.size();
        card["variationOf"] = c.variationOf;
        card["mirrored"] = c.mirrored;
        card["stations"] = c.stations;
        card["across"] = c.across;
        card["cell"] = {c.cell[0], c.cell[1], c.cell[2], c.cell[3]};
        card["width"] = {c.width.front(), c.width[c.width.size() / 2], c.width.back()};
        list.push_back(std::move(card));
    }
    j["list"] = std::move(list);
    const std::size_t stride = std::max<std::size_t>(1, cards.vertexCount() / 256);
    json vertices = json::array();
    for (std::size_t v = 0; v < cards.vertexCount(); v += stride) {
        const Vec3f& p = cards.positions[v];
        vertices.push_back({p.x, p.y, p.z, cards.u[v], cards.v[v]});
    }
    j["sampled"] = std::move(vertices);
    return j;
}

void expectCardsMatch(const json& expected, const json& actual, const std::string& name) {
    for (const char* key : {"cards", "vertices", "triangles", "textures"}) {
        ASSERT_EQ(expected.at(key), actual.at(key)) << name << ": " << key;
    }
    float worst = 0.0f;
    auto near = [&](const json& a, const json& b) {
        ASSERT_EQ(a.size(), b.size()) << name;
        for (std::size_t k = 0; k < a.size(); ++k) worst = std::max(worst, std::abs(a[k].get<float>() - b[k].get<float>()));
    };
    const json& el = expected.at("list");
    const json& al = actual.at("list");
    if (!test::kRecordedMath) {
        // Which card's texture a card shares is a clustering of float features (kMeans): with
        // another C runtime's math (test::kRecordedMath) a last bit can pick another card, and the
        // atlas cells and mirroring follow. The counts above hold; the rest only when it picked alike.
        for (std::size_t c = 0; c < el.size(); ++c) {
            if (el[c].at("variationOf") != al[c].at("variationOf")) {
                std::printf("[ cards ] %s: card %zu shares card %s's texture, recorded %s (another C runtime's math)\n",
                            name.c_str(), c, al[c].at("variationOf").dump().c_str(), el[c].at("variationOf").dump().c_str());
                return;
            }
        }
    }
    for (std::size_t c = 0; c < el.size(); ++c) {
        for (const char* key : {"strands", "variationOf", "mirrored", "stations", "across"}) {
            EXPECT_EQ(el[c].at(key), al[c].at(key)) << name << " card " << c << ": " << key;
        }
        near(el[c].at("cell"), al[c].at("cell"));
        near(el[c].at("width"), al[c].at("width"));
    }
    const json& es = expected.at("sampled");
    const json& as = actual.at("sampled");
    ASSERT_EQ(es.size(), as.size()) << name;
    for (std::size_t v = 0; v < es.size(); ++v) near(es[v], as[v]);
    EXPECT_LE(worst, kPositionTolerance) << name << ": cards differ by up to " << worst;
}

class CardGolden : public ::testing::TestWithParam<Case> {};

TEST_P(CardGolden, MatchesStoredCards) {
    static Scene scene;
    const Case& c = GetParam();
    const StrandsPtr out = scene.runCards(c);
    ASSERT_TRUE(out) << c.name;
    ASSERT_FALSE(out->cardLayers().empty()) << c.name;
    const json actual = summarizeCards(*out);
    EXPECT_GT(actual.at("cards").get<std::size_t>(), 0u) << c.name;
    const fs::path file = kData / "golden" / ("Cards_" + std::string(c.name) + ".json");
    if (updating()) {
        std::ofstream(file, std::ios::binary) << actual.dump() << "\n";
        GTEST_SKIP() << "wrote " << file.generic_string();
    }
    std::ifstream in(file, std::ios::binary);
    ASSERT_TRUE(in) << "no golden file " << file.generic_string() << " (run with GROOMLAB_UPDATE_GOLDEN=1)";
    std::stringstream text;
    text << in.rdbuf();
    expectCardsMatch(json::parse(text.str()), actual, c.name);

#if GROOMLAB_HAS_TBB
    // One thread and many must agree to the bit.
    StrandsPtr serial;
    tbb::task_arena single(1);
    single.execute([&] { serial = scene.runCards(c); });
    CardMesh a = combineCardLayers(serial->cardLayers()), b = combineCardLayers(out->cardLayers());
    layoutCards(a, a.padding);
    layoutCards(b, b.padding);
    EXPECT_EQ(a.positions, b.positions) << c.name << ": thread count changes the cards";
    EXPECT_EQ(a.u, b.u) << c.name;
#endif
}

INSTANTIATE_TEST_SUITE_P(Cards, CardGolden, ::testing::ValuesIn(cardCases()),
                         [](const ::testing::TestParamInfo<Case>& info) { return std::string(info.param.name); });

TEST(Golden, EveryOperatorIsCovered) {
    // Only nodes that feed a case's output count: the scaffold every case shares (and a
    // dangling node) would otherwise cover a type whose own case was gone.
    std::set<std::string> covered;
    for (const Case& c : cases()) {
        const json graph = json::parse(graphFor(c));
        std::map<std::string, std::string> types;
        for (const json& n : graph["nodes"]) types[n["name"].get<std::string>()] = n["type"].get<std::string>();
        std::vector<std::string> stack{c.output};
        std::set<std::string> seen;
        while (!stack.empty()) {
            const std::string name = stack.back();
            stack.pop_back();
            if (!seen.insert(name).second) continue;
            covered.insert(types.at(name));
            for (const json& link : graph["connections"]) {
                if (link["to"] == name) stack.push_back(link["from"].get<std::string>());
            }
        }
    }
    for (const std::string& type : OperatorRegistry::instance().typeNames()) {
        if (type == "ImportCurves") continue;  // Alembic operator: unit-tested in tests/engine/io
        if (type == "Input") continue;         // only inside compounds: covered by the Compound case
        if (type == "Reference") continue;     // another groom file's result: test_reference.cpp, against that groom's
        if (type == "StrandMesh") continue;    // hands its strands on as they came: its mesh, test_strand_geometry.cpp and test_follow.cpp
        EXPECT_TRUE(covered.count(type)) << type << " has no golden case";
    }
}

}  // namespace
}  // namespace groomlab
