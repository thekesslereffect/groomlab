// SPDX-License-Identifier: Apache-2.0
// Scales (scales.h): the Instance node's mode scales, the built-in plates, the bend onto the skin,
// the roof-tile lift, the per-scale data and the timings at 50k scales.

#include <gtest/gtest.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <map>
#include <set>

#include "groomlab/curve.h"
#include "groomlab/export_view.h"
#include "groomlab/flow.h"
#include "groomlab/frames.h"
#include "groomlab/graph.h"
#include "groomlab/groom.h"
#include "groomlab/groom_session.h"
#include "groomlab/guide_hierarchy.h"
#include "groomlab/operators.h"
#include "groomlab/scales.h"
#include "test_helpers.h"

namespace groomlab {
namespace {

// A sphere with uvs (longitude, latitude), as a body's skin.
std::shared_ptr<const ScalpMesh> uvSphere(float radius, int rings = 48, int segments = 96) {
    auto mesh = std::make_shared<ScalpMesh>(*test::makeSphere(rings, segments, radius));
    std::vector<ScalpMesh::UV> uvs;
    for (const auto& tri : mesh->triangles()) {
        for (const std::int32_t v : tri) {
            const Vec3f& p = mesh->points()[static_cast<std::size_t>(v)];
            uvs.push_back({0.5f + std::atan2(p.z, p.x) / 6.2831853f, std::acos(std::clamp(p.y / radius, -1.0f, 1.0f)) / 3.14159265f});
        }
    }
    mesh->setCornerUVs(std::move(uvs));
    return mesh;
}

// Scatter on `skin`, combed down along it, a Width, then Scales.
struct ScaleGraph {
    Graph g;
    ExternalInputs ext;
    MeshPtr skin;
    NodeId scalp = 0, scatter = 0, comb = 0, width = 0, color = 0, scales = 0;

    explicit ScaleGraph(MeshPtr mesh, double density, double length = 1.0, double strandWidth = 0.6) : skin(std::move(mesh)) {
        ext.set("scalp", skin, skin->surfaceHash());
        scalp = g.addNode(op::kScalp);
        scatter = g.addNode(op::kScatter);
        g.node(scatter).params().set("density", density);
        g.node(scatter).params().set("length", length);
        comb = g.addNode(op::kBend);  // laid down the body (-y on the skin)
        g.node(comb).params().set("angle", 180.0);
        g.node(comb).params().set("end", 0.0);
        g.node(comb).params().set("along", std::string("direction"));
        g.node(comb).params().set("direction", Vec3f{0, -1, 0});
        width = g.addNode(op::kWidth);
        g.node(width).params().set("width", strandWidth);
        color = g.addNode(op::kHairColor);
        g.node(color).params().set("melaninRandom", 0.3);
        scales = g.addNode(op::kScales);
        auto& p = g.node(scales).params();
        p.set("prototype", std::string("groomlab:scale"));
        p.set("scaleRandom", 0.0);
        p.set("spinRandom", 0.0);
        g.connect(scalp, scatter, 0);
        g.connect(scatter, comb, 0);
        g.connect(comb, width, 0);
        g.connect(width, color, 0);
        g.connect(color, scales, 0);
    }
    ParamSet& params() { return g.node(scales).params(); }
    StrandsPtr eval(NodeId id) {
        EvalContext ctx;
        ctx.externals = &ext;
        return g.evaluateStrands(id, ctx);
    }
    StrandsPtr eval() { return eval(scales); }
};

std::map<std::uint64_t, std::size_t> strandIndex(const StrandSet& s) {
    std::map<std::uint64_t, std::size_t> out;
    for (std::size_t i = 0; i < s.strandCount(); ++i) out[s.ids()[i]] = i;
    return out;
}

TEST(Scales, OneAtEachRootLyingOnTheSkinAlongTheStrand) {
    ScaleGraph sg(uvSphere(5.0f), 2.0);
    const StrandsPtr s = sg.eval();
    ASSERT_TRUE(s->instances());
    ASSERT_GT(s->strandCount(), 300u);
    EXPECT_EQ(s->instances()->size(), s->strandCount());  // one each, whatever the probability
    EXPECT_TRUE(s->instances()->hasScales());
    EXPECT_TRUE(s->instances()->needsRestPose());  // sized from the rest length, as fitted instances
    EXPECT_TRUE(s->instances()->hidesStrands);     // the strands only lay them out
    const auto placed = s->instances()->resolve(*s, sg.skin.get());
    ASSERT_EQ(placed.size(), s->strandCount());
    const auto index = strandIndex(*s);
    std::size_t checked = 0;
    for (const auto& p : placed) {
        ASSERT_TRUE(p.onSkin);
        const std::size_t i = index.at(s->instances()->instances[p.index].strand);
        const Vec3f* pts = s->strandPoints(i);
        const std::size_t n = s->strandPointCount(i);
        const Vec3f normal = sg.skin->normal(s->roots()[i]);
        EXPECT_LT((p.origin - pts[0]).length(), 1e-5f);                // at the root
        EXPECT_GT(p.z.normalized().dot(normal), 0.9999f);              // +Z the skin's normal
        EXPECT_NEAR(p.y.normalized().dot(normal), 0.0f, 1e-4f);        // +Y on the skin
        EXPECT_NEAR(p.x.normalized().dot(p.y.normalized()), 0.0f, 1e-4f);
        EXPECT_GT(p.x.cross(p.y).dot(p.z), 0.0f);                      // right-handed
        Vec3f chord = pts[n - 1] - pts[0];
        chord = chord - normal * chord.dot(normal);
        if (chord.length() > 0.1f) {
            EXPECT_GT(p.y.normalized().dot(chord.normalized()), 0.999f);  // along the strand
            ++checked;
        }
        EXPECT_NEAR(p.y.length(), arcLength(pts, n), 1e-3f);          // the strand's length (no rest pose here)
        EXPECT_NEAR(p.x.length(), s->widths()[s->strandBegin(i)], 1e-4f);  // its root width across
        EXPECT_NEAR(p.z.length(), p.x.length(), 1e-6f);
    }
    EXPECT_GT(checked, placed.size() / 2);

    // size scales both; scaleRandom varies them per scale, from the ID.
    sg.params().set("size", 0.5);
    sg.params().set("scaleRandom", 0.3);
    const StrandsPtr jittered = sg.eval();
    const auto smaller = jittered->instances()->resolve(*jittered, sg.skin.get());
    float lo = 1e9f, hi = 0.0f;
    for (std::size_t k = 0; k < smaller.size(); ++k) {
        const float r = smaller[k].y.length() / placed[k].y.length();
        lo = std::min(lo, r);
        hi = std::max(hi, r);
        EXPECT_NEAR(smaller[k].x.length() / placed[k].x.length(), r, 1e-3f);
    }
    EXPECT_GT(lo, 0.5f * 0.7f - 1e-3f);
    EXPECT_LT(hi, 0.5f * 1.3f + 1e-3f);
    EXPECT_GT(hi - lo, 0.15f);

    // showStrands: the strands draw and render too.
    sg.params().set("showStrands", true);
    EXPECT_FALSE(sg.eval()->instances()->hidesStrands);
    sg.params().set("showStrands", false);

    // Without the scalp, the normal the node saw.
    const auto unrooted = s->instances()->resolve(*s);
    for (std::size_t k = 0; k < placed.size(); ++k) EXPECT_GT(unrooted[k].z.normalized().dot(placed[k].z.normalized()), 0.9999f);
}

TEST(Scales, VariantsByIdAndByMap) {
    ScaleGraph sg(uvSphere(5.0f), 2.0);
    sg.params().set("prototype", std::string("groomlab:scale groomlab:scaleRound, groomlab:scaleShield"));
    const StrandsPtr s = sg.eval();
    const InstanceSet& set = *s->instances();
    EXPECT_EQ(set.prototypes, (std::vector<std::string>{"groomlab:scale", "groomlab:scaleRound", "groomlab:scaleShield"}));
    std::map<int, std::size_t> counts;
    for (const auto& i : set.instances) {
        EXPECT_EQ(i.prototype, i.variant);  // the first node: its list is the set's
        ++counts[i.variant];
    }
    ASSERT_EQ(counts.size(), 3u);
    for (const auto& [variant, n] : counts) EXPECT_GT(n, set.size() / 5) << variant;
    // The same seed, the same picks; another seed, others.
    EXPECT_EQ(sg.eval()->instances()->contentHash(), set.contentHash());
    sg.params().set("seed", std::int64_t{7});
    const StrandsPtr reseeded = sg.eval();
    std::size_t moved = 0;
    for (std::size_t k = 0; k < set.size(); ++k) moved += reseeded->instances()->instances[k].variant != set.instances[k].variant;
    EXPECT_GT(moved, set.size() / 3);

    // By a map: the top half of the body the first plate, the bottom half the last.
    sg.params().set("seed", std::int64_t{0});
    sg.params().set("variantBy", std::string("map"));
    MapRef byHeight;
    byHeight.source = MapRef::Source::Expression;
    byHeight.name = "$y > 0 ? 0 : 1";
    sg.params().set("variant", byHeight);
    const StrandsPtr mapped = sg.eval();
    const auto index = strandIndex(*mapped);
    for (const auto& i : mapped->instances()->instances) {
        const float y = mapped->strandPoints(index.at(i.strand))[0].y;
        if (std::abs(y) > 1e-3f) EXPECT_EQ(i.variant, y > 0.0f ? 0 : 2) << y;
    }

    // By a channel (an attribute map), on strands made by hand.
    auto strands = std::make_shared<StrandSet>();
    const MeshPtr plane = test::makePlane(4, 4.0f);
    for (int k = 0; k < 40; ++k) {
        const Vec3f root{0.1f * static_cast<float>(k), 0.0f, 1.0f};
        strands->addStrand(static_cast<StrandId>(k + 1), {root, root + Vec3f{0, 0, 0.5f}}, {0.2f, 0.2f}, plane->closestPoint(root).root);
    }
    auto& zone = strands->addStrandChannel("zone");
    for (int k = 0; k < 40; ++k) zone[static_cast<std::size_t>(k)] = k < 20 ? 0.2f : 0.9f;
    auto op = OperatorRegistry::instance().create(op::kScales);
    op->params().set("prototype", std::string("small big"));
    op->params().set("variantBy", std::string("map"));
    MapRef channel;
    channel.source = MapRef::Source::Attribute;
    channel.name = "zone";
    op->params().set("variant", channel);
    ExternalInputs ext;
    ext.set("scalp", plane, plane->surfaceHash());
    EvalContext ctx;
    ctx.externals = &ext;
    const StrandsPtr byChannel = valueAs<StrandsPtr>(op->evaluate(ctx, {Value(StrandsPtr(strands))}));
    for (const auto& i : byChannel->instances()->instances) EXPECT_EQ(i.variant, i.strand <= 20 ? 0 : 1);
}

// Each vertex's height above the skin: along the closest point's normal.
float heightAbove(const ScalpMesh& skin, const Vec3f& p) {
    const SurfacePoint sp = skin.closestPoint(p);
    return (p - sp.position).dot(sp.normal);
}

TEST(Scales, ConformKeepsEveryVertexAboveTheSkin) {
    // Big plates on a small sphere (each about a third of its radius long): flat, their tips
    // would stand well off it.
    ScaleGraph sg(uvSphere(3.0f), 0.5, 1.2, 0.8);
    sg.params().set("offset", 0.02);
    sg.params().set("tile", 0.0);
    sg.params().set("stack", 0.0);
    const StrandsPtr s = sg.eval();
    const auto bent = buildScaleMesh(*s, sg.skin);
    ASSERT_TRUE(bent);
    ASSERT_GT(bent->scales.size(), 20u);
    EXPECT_EQ(bent->vertexCount(), bent->scales.size() * builtinPrototype("groomlab:scale")->points.size());
    float lowest = 1e9f, highestBent = 0.0f;
    for (const Vec3f& p : bent->points) {
        const float h = heightAbove(*sg.skin, p);
        lowest = std::min(lowest, h);
        highestBent = std::max(highestBent, h);
    }
    EXPECT_GT(lowest, 0.02f - 2e-3f);  // never inside, at least the offset off it
    // Rigid (conform off): the same plates stand off the curve.
    sg.params().set("conform", false);
    const auto rigid = buildScaleMesh(*sg.eval(), sg.skin);
    float highestRigid = 0.0f;
    for (const Vec3f& p : rigid->points) highestRigid = std::max(highestRigid, heightAbove(*sg.skin, p));
    const float plate = 0.8f * 0.14f;  // the plate's height at its root width (the dome and keel)
    EXPECT_LT(highestBent, 0.02f + 1.5f * plate);
    EXPECT_GT(highestRigid, highestBent + 0.05f);
    // Normals still face out.
    std::size_t outward = 0;
    for (std::size_t v = 0; v < bent->vertexCount(); ++v) {
        outward += bent->normals[v].dot(sg.skin->closestPoint(bent->points[v]).normal) > -0.2f ? 1 : 0;
    }
    EXPECT_GT(outward, bent->vertexCount() * 55 / 100);  // (the undersides and their edge, two fifths of the plate, face in)
}

TEST(Scales, TileAndLiftRaiseTheTips) {
    // One strand on a plane, along +X: the tip vertex is the one farthest along.
    const MeshPtr plane = test::makePlane(4, 4.0f);
    auto strands = std::make_shared<StrandSet>();
    const Vec3f root{1.0f, 0.0f, 2.0f};
    strands->addStrand(1, {root, root + Vec3f{0.5f, 0, 0}, root + Vec3f{1.0f, 0, 0}}, {0.5f, 0.5f, 0.5f}, plane->closestPoint(root).root);
    auto tipHeight = [&](double tile, double lift) {
        auto op = OperatorRegistry::instance().create(op::kScales);
        op->params().set("prototype", std::string("groomlab:scale"));
        op->params().set("scaleRandom", 0.0);
        op->params().set("spinRandom", 0.0);
        op->params().set("stack", 0.0);
        op->params().set("tile", tile);
        op->params().set("lift", lift);
        ExternalInputs ext;
        ext.set("scalp", plane, plane->surfaceHash());
        EvalContext ctx;
        ctx.externals = &ext;
        const StrandsPtr out = valueAs<StrandsPtr>(op->evaluate(ctx, {Value(StrandsPtr(strands))}));
        const auto mesh = buildScaleMesh(*out, plane);
        float farthest = -1e9f, height = 0.0f;
        for (const Vec3f& p : mesh->points) {
            if (p.x > farthest + 1e-6f) {
                farthest = p.x;
                height = p.y;
            }
        }
        return std::make_pair(height, mesh->scales[0].y.normalized().y);
    };
    const auto flat = tipHeight(0.0, 0.0);
    const auto tiled = tipHeight(1.0, 0.0);
    const auto lifted = tipHeight(1.0, 10.0);
    EXPECT_NEAR(flat.second, 0.0f, 1e-5f);  // flat on the plane
    EXPECT_GT(tiled.second, 0.0f);
    // Tile 1 raises the tip by about the plate's height (0.1 of the plate's 0.5 breadth... times
    // its root width 0.5): the dome's top, 0.1 + the keel, at Z's scale 0.5.
    const float plateHeight = 0.5f * 0.14f;
    EXPECT_NEAR(tiled.first - flat.first, plateHeight, 0.4f * plateHeight);
    EXPECT_GT(lifted.first, tiled.first + 0.1f);  // 10 degrees more over a length of 1
}

TEST(Scales, PerScaleShaderData) {
    ScaleGraph sg(uvSphere(5.0f), 1.0);
    sg.g.node(sg.color).params().set("tipStrength", 1.0);
    sg.g.node(sg.color).params().set("tipColor", Vec3f{0.0f, 0.0f, 0.0f});
    sg.params().set("prototype", std::string("groomlab:scale groomlab:scaleRound"));
    const StrandsPtr s = sg.eval();
    const auto mesh = buildScaleMesh(*s, sg.skin);
    ASSERT_TRUE(mesh);
    EXPECT_EQ(mesh->colors.size(), mesh->vertexCount());
    EXPECT_EQ(mesh->uvs.size(), mesh->vertexCount());
    const auto index = strandIndex(*s);
    const std::vector<float>* red = s->pointChannel(kColorChannels[0]);
    ASSERT_TRUE(red);
    std::set<float> randoms;
    std::set<int> variants;
    for (const auto& sc : mesh->scales) {
        const std::size_t i = index.at(sc.strand);
        EXPECT_NEAR(sc.color[0], (*red)[s->strandBegin(i)], 1e-5f);  // HairColor's at the root
        const auto uv = sg.skin->uvAt(s->roots()[i]);
        EXPECT_FLOAT_EQ(sc.rootUv[0], uv[0]);
        EXPECT_FLOAT_EQ(sc.rootUv[1], uv[1]);
        EXPECT_GE(sc.random, 0.0f);
        EXPECT_LT(sc.random, 1.0f);
        randoms.insert(sc.random);
        variants.insert(sc.variant);
        EXPECT_EQ(sc.variant, s->instances()->instances[sc.index].variant);
        // The vertices' colors follow the strand base to tip: the tip's toward the dark tip color.
        float base = 1e9f, tip = 1e9f, lowY = 1e9f, highY = -1e9f;
        const InstancePrototype& proto = *builtinPrototype(s->instances()->prototypes[sc.prototype]);
        for (std::uint32_t v = 0; v < sc.vertexCount; ++v) {
            const float y = proto.points[v].y;
            if (y < lowY) { lowY = y; base = mesh->colors[sc.firstVertex + v][0]; }
            if (y > highY) { highY = y; tip = mesh->colors[sc.firstVertex + v][0]; }
        }
        EXPECT_LT(tip, base);
    }
    EXPECT_GT(randoms.size(), mesh->scales.size() * 9 / 10);
    EXPECT_EQ(variants, (std::set<int>{0, 1}));
}

TEST(Scales, KeptPerScaleAndQuadsForThePreview) {
    ScaleGraph sg(uvSphere(5.0f), 2.0);
    const StrandsPtr s = sg.eval();
    ScaleMeshBuilder builder;
    const auto first = builder.build(*s, sg.skin);
    EXPECT_EQ(builder.stats().bent, first->scales.size());
    EXPECT_EQ(builder.stats().kept, 0u);
    const auto again = builder.build(*s, sg.skin);
    EXPECT_EQ(builder.stats().kept, first->scales.size());  // nothing changed: nothing bent again
    EXPECT_EQ(again->points, first->points);
    EXPECT_EQ(again->normals, first->normals);
    // A changed offset bends them all again, to what a fresh build makes.
    sg.params().set("offset", 0.05);
    const StrandsPtr raised = sg.eval();
    const auto moved = builder.build(*raised, sg.skin);
    EXPECT_EQ(builder.stats().bent, first->scales.size());
    EXPECT_EQ(moved->points, buildScaleMesh(*raised, sg.skin)->points);

    // The viewport's preview: quads past the budget (auto), or always (quads); a render, meshes.
    ScaleMeshOptions preview;
    preview.preview = true;
    preview.triangleBudget = 10;
    const auto quads = buildScaleMesh(*s, sg.skin, {}, preview);
    EXPECT_EQ(quads->vertexCount(), 4 * quads->scales.size());
    for (const auto& sc : quads->scales) EXPECT_TRUE(sc.quad);
    preview.triangleBudget = 0;
    EXPECT_EQ(buildScaleMesh(*s, sg.skin, {}, preview)->vertexCount(), first->vertexCount());
    sg.params().set("offset", 0.0);
    sg.params().set("preview", std::string("quads"));
    const StrandsPtr quadded = sg.eval();
    EXPECT_EQ(buildScaleMesh(*quadded, sg.skin, {}, preview)->vertexCount(), 4 * first->scales.size());
    EXPECT_EQ(buildScaleMesh(*quadded, sg.skin)->vertexCount(), first->vertexCount());  // a render
    // A prototype the host has (or none: a quad).
    sg.params().set("prototype", std::string("myPlate"));
    const StrandsPtr own = sg.eval();
    EXPECT_EQ(buildScaleMesh(*own, sg.skin)->vertexCount(), 4 * first->scales.size());
    InstancePrototype triangle;
    triangle.points = {{-0.5f, 0, 0}, {0.5f, 0, 0}, {0, 1, 0.05f}};
    triangle.normals = {{0, 0, 1}, {0, 0, 1}, {0, 0, 1}};
    triangle.triangles = {0, 1, 2};
    triangle.found = true;
    const auto hosted = buildScaleMesh(*own, sg.skin, [&](const std::string& name) { return name == "myPlate" ? &triangle : nullptr; });
    EXPECT_EQ(hosted->vertexCount(), 3 * first->scales.size());
}

TEST(Scales, BuiltinPlates) {
    for (const std::string& name : builtinPrototypeNames()) {
        const InstancePrototype* plate = builtinPrototype(name);
        ASSERT_TRUE(plate) << name;
        EXPECT_TRUE(plate->found);
        EXPECT_EQ(plate->normals.size(), plate->points.size());
        EXPECT_EQ(plate->uvs.size(), plate->points.size());
        float lowY = 1e9f, highY = -1e9f, lowZ = 1e9f, highZ = -1e9f, wide = 0.0f;
        for (const Vec3f& p : plate->points) {
            lowY = std::min(lowY, p.y);
            highY = std::max(highY, p.y);
            lowZ = std::min(lowZ, p.z);
            highZ = std::max(highZ, p.z);
            wide = std::max(wide, std::abs(p.x));
        }
        EXPECT_NEAR(lowY, 0.0f, 1e-5f) << name;  // the base at the origin, the tip at +Y 1
        EXPECT_NEAR(highY, 1.0f, 1e-5f) << name;
        EXPECT_NEAR(lowZ, 0.0f, 1e-5f) << name;  // flat underneath, domed above
        EXPECT_GT(highZ, 0.05f) << name;
        EXPECT_NEAR(wide, 0.5f, 0.02f) << name;  // a unit wide: as wide as its strand
        // One closed piece by its vertices (for subdivision: no split edges), every edge in two
        // triangles; and by position, every edge met the other way round by a neighbouring one.
        std::map<std::pair<std::uint32_t, std::uint32_t>, int> byIndex;
        for (std::size_t t = 0; t + 2 < plate->triangles.size(); t += 3) {
            for (std::size_t c = 0; c < 3; ++c) {
                const std::uint32_t a = plate->triangles[t + c], b = plate->triangles[t + (c + 1) % 3];
                ++byIndex[{std::min(a, b), std::max(a, b)}];
            }
        }
        for (const auto& [e, count] : byIndex) EXPECT_EQ(count, 2) << name << " edge " << e.first << "-" << e.second;
        EXPECT_EQ(plate->triangles.size() % 6, 0u) << name;  // quads: two triangles each
        using Key = std::array<long, 3>;
        auto key = [](const Vec3f& v) { return Key{std::lround(v.x * 1e4f), std::lround(v.y * 1e4f), std::lround(v.z * 1e4f)}; };
        std::set<std::pair<Key, Key>> edges;
        for (std::size_t t = 0; t + 2 < plate->triangles.size(); t += 3) {
            for (std::size_t c = 0; c < 3; ++c) {
                const Key a = key(plate->points[plate->triangles[t + c]]), b = key(plate->points[plate->triangles[t + (c + 1) % 3]]);
                if (a != b) edges.emplace(a, b);  // (the rounded tip's few degenerate ones aside)
            }
        }
        std::size_t open = 0;
        for (const auto& edge : edges) open += edges.count(std::make_pair(edge.second, edge.first)) == 0 ? 1 : 0;
        EXPECT_EQ(open, 0u) << name;
    }
    EXPECT_EQ(builtinPrototype("leaf"), nullptr);
}

TEST(Scales, AnInstanceNodeInstancesAlongTheStrands) {
    // Instance (not Scales): instances along the strands, none on the skin, no materials; a single
    // prototype name as ever.
    ScaleGraph sg(uvSphere(5.0f), 2.0);
    const NodeId instance = sg.g.addNode(op::kInstance);
    sg.g.connect(sg.color, instance, 0);
    sg.g.node(instance).params().set("prototype", std::string("snowflake"));
    sg.g.node(instance).params().set("probability", 1.0);
    const StrandsPtr s = sg.eval(instance);
    EXPECT_FALSE(s->instances()->hasScales());
    EXPECT_FALSE(s->instances()->hidesStrands);
    EXPECT_TRUE(s->instances()->materials.empty());
    EXPECT_EQ(s->instances()->prototypes, std::vector<std::string>{"snowflake"});
    for (const auto& i : s->instances()->instances) {
        EXPECT_FALSE(i.onSkin);
        EXPECT_EQ(i.variant, 0);
    }
    EXPECT_EQ(buildScaleMesh(*s, sg.skin), nullptr);
}

// A groom's graph: Scalp, Scatter, `shaping` (nodes JSON, the last named "shaped"), a Width and
// Scales.
std::string scaleGroomGraph(const std::string& shaping, const std::string& connections, const std::string& instanceParams = "") {
    return R"({"version": 1, "nodes": [
        {"name": "scalp", "type": "Scalp"},
        {"name": "scatter", "type": "Scatter", "params": {"density": 30.0, "length": 0.4}},
        )" + shaping + R"(,
        {"name": "width", "type": "Width", "params": {"width": 0.12, "profile": [[0, 1], [1, 1]]}},
        {"name": "scales", "type": "Scales", "params": {"prototype": "groomlab:scale",
         "scaleRandom": 0.0, "spinRandom": 0.0)" + instanceParams + R"(}},
        {"name": "output", "type": "Output"}],
      "connections": [
        {"from": "scalp", "to": "scatter", "input": "scalp"},
        )" + connections + R"(,
        {"from": "shaped", "to": "width", "input": "strands"},
        {"from": "width", "to": "scales", "input": "strands"},
        {"from": "scales", "to": "output", "input": "strands"}],
      "output": "output"})";
}

// Guides on a unit sphere's upper half, each lying along the skin toward `toward` (laid onto it).
std::shared_ptr<GuideHierarchy> guidesToward(const ScalpMesh& skin, Vec3f toward) {
    auto h = std::make_shared<GuideHierarchy>();
    const GroupId primary = h->addGroup("primary");
    for (int ring = 0; ring < 3; ++ring) {
        for (int i = 0; i < 8; ++i) {
            const float a = 6.2831853f * (static_cast<float>(i) + 0.5f * static_cast<float>(ring)) / 8.0f;
            const float polar = 0.3f + 0.35f * static_cast<float>(ring);
            const Vec3f n{std::sin(polar) * std::cos(a), std::cos(polar), std::sin(polar) * std::sin(a)};
            Vec3f along = toward - n * toward.dot(n);
            along = along.length() > 1e-3f ? along.normalized() : anyPerpendicular(n).normalized();
            h->addGuide(primary, test::straightCurve(n, along * 0.4f + n * 0.04f, 6));
        }
    }
    h->evaluate(&skin);
    return h;
}

// Every scale of `mesh` points (laid on the skin) along its strand's root-to-tip chord; the
// scales' mean direction on the skin, for comparing one groom with another.
Vec3f expectScalesAlongTheirStrands(const ScaleMesh& mesh, const StrandSet& strands, const ScalpMesh& skin) {
    const auto index = strandIndex(strands);
    std::size_t checked = 0;
    Vec3f mean;
    for (const auto& sc : mesh.scales) {
        const std::size_t s = index.at(sc.strand);
        const Vec3f* pts = strands.strandPoints(s);
        const std::size_t n = strands.strandPointCount(s);
        const Vec3f normal = skin.normal(strands.roots()[s]).normalized();
        Vec3f chord = pts[n - 1] - pts[0];
        chord = chord - normal * chord.dot(normal);
        const Vec3f dir = sc.y - normal * sc.y.dot(normal);  // (tilted up by the tile: laid back onto the skin)
        if (chord.length() < 0.05f) continue;
        EXPECT_GT(dir.normalized().dot(chord.normalized()), 0.995f) << "scale " << sc.id;
        mean = mean + dir.normalized();
        ++checked;
    }
    EXPECT_GT(checked, mesh.scales.size() * 3 / 4);
    return mean / static_cast<float>(std::max<std::size_t>(checked, 1));
}

TEST(Scales, TurnWithTheGuidesTheirBrushStrokesAndTheFlow) {
    // Through the session (what every host evaluates, the rest pose included): guides driving the
    // strands through Interpolate, then a stroke's working copy of them turned a quarter turn, then
    // committed; the viewport's builder keeps nothing of the old directions. Then Flow's arrows.
    const auto skin = test::makeSphere(32, 64);
    GroomData d;
    d.graphJson = scaleGroomGraph(
        R"({"name": "guides", "type": "Guides"}, {"name": "shaped", "type": "Interpolate"})",
        R"({"from": "scatter", "to": "shaped", "input": "strands"}, {"from": "guides", "to": "shaped", "input": "guides"},
           {"from": "scalp", "to": "shaped", "input": "scalp"})");
    d.hierarchyJson = guidesToward(*skin, {0, 0, 1})->toJson();
    d.scalp = skin;
    GroomSession session(d);
    ScaleMeshBuilder viewport;
    const ViewResult& combed = session.evaluate(ViewRequest{});
    ASSERT_TRUE(combed.strands) << (combed.errors.empty() ? "" : combed.errors[0]);
    const StrandsPtr before = combed.strands;
    const auto first = viewport.build(*before, skin);
    ASSERT_TRUE(first);
    EXPECT_GT(expectScalesAlongTheirStrands(*first, *before, *skin).z, 0.5f);

    // A stroke: the working copy of the guides, turned toward +X.
    const auto stroke = guidesToward(*skin, {1, 0, 0});
    const auto authored = session.authoredHierarchy();
    ASSERT_EQ(authored->guides().size(), stroke->guides().size());
    auto preview = std::make_shared<GuideHierarchy>(*authored);
    for (std::size_t g = 0; g < authored->guides().size(); ++g) preview->setPoints(authored->guides()[g].id, stroke->guides()[g].points);
    session.setPreviewHierarchy(preview);
    const StrandsPtr during = session.evaluate(ViewRequest{}).strands;
    const auto dab = viewport.build(*during, skin);
    EXPECT_GT(expectScalesAlongTheirStrands(*dab, *during, *skin).x, 0.5f);
    EXPECT_LT(viewport.stats().kept, dab->scales.size() / 10);  // turned: bent again, not kept
    EXPECT_EQ(dab->points, buildScaleMesh(*during, skin)->points);
    // Released: the guides as brushed.
    session.setPreviewHierarchy(nullptr);
    session.setHierarchy(preview);
    const StrandsPtr after = session.evaluate(ViewRequest{}).strands;
    const auto released = viewport.build(*after, skin);
    EXPECT_GT(expectScalesAlongTheirStrands(*released, *after, *skin).x, 0.5f);
    EXPECT_EQ(released->points, buildScaleMesh(*after, skin)->points);

    // Flow: the arrows turn the strands and the scales with them.
    FlowSet south, east;
    for (const Vec3f at : {Vec3f{0, 0.8f, 0.6f}, Vec3f{0.6f, 0.8f, 0}, Vec3f{-0.6f, 0.8f, 0}, Vec3f{0, 0.8f, -0.6f}}) {
        south.pins.push_back(FlowSet::makePin(*skin, skin->closestPoint(at), {0, 0, 1}));
        east.pins.push_back(FlowSet::makePin(*skin, skin->closestPoint(at), {1, 0, 0}));
    }
    GroomData f;
    f.graphJson = scaleGroomGraph(R"({"name": "shaped", "type": "Flow"})", R"({"from": "scatter", "to": "shaped", "input": "strands"})");
    f.flowJson = south.toJson();
    f.scalp = skin;
    GroomSession flowed(f);
    ScaleMeshBuilder flowView;
    const StrandsPtr alongZ = flowed.evaluate(ViewRequest{}).strands;
    ASSERT_TRUE(alongZ);
    EXPECT_GT(expectScalesAlongTheirStrands(*flowView.build(*alongZ, skin), *alongZ, *skin).z, 0.5f);
    flowed.setFlowJson(east.toJson());
    const StrandsPtr alongX = flowed.evaluate(ViewRequest{}).strands;
    const auto turned = flowView.build(*alongX, skin);
    EXPECT_GT(expectScalesAlongTheirStrands(*turned, *alongX, *skin).x, 0.5f);
    EXPECT_EQ(turned->points, buildScaleMesh(*alongX, skin)->points);
}

// Past this share of a plate's length a scale following its strand is all the strand's (scales.cpp
// kStrandBase, and a little).
constexpr float kStrandBaseForTests = 0.26f;

// The shortest distance from p to a polyline.
float distanceToPolyline(const Vec3f& p, const Vec3f* pts, std::size_t n) {
    float best = 1e30f;
    for (std::size_t i = 1; i < n; ++i) {
        const Vec3f seg = pts[i] - pts[i - 1];
        const float t = std::clamp((p - pts[i - 1]).dot(seg) / std::max(seg.dot(seg), 1e-12f), 0.0f, 1.0f);
        best = std::min(best, (p - (pts[i - 1] + seg * t)).length());
    }
    return best;
}

TEST(Scales, AimByTheRootAndFollowTheStrandsCurve) {
    // A strand that leaves the root along +X and curls back over it: its chord points along -X,
    // its root along +X. No skin: flat on the plane the node saw (+Y up).
    StrandSet strands;
    const std::vector<Vec3f> curl = {{0, 0, 0}, {0.3f, 0.05f, 0}, {0.5f, 0.3f, 0}, {0.3f, 0.55f, 0}, {-0.4f, 0.6f, 0}};
    strands.addStrand(7, curl, std::vector<float>(curl.size(), 0.1f));
    auto set = std::make_shared<InstanceSet>();
    set->prototypes = {"groomlab:scale"};
    InstanceSet::Instance inst;
    inst.strand = 7;
    inst.id = 70;
    inst.onSkin = true;
    inst.t = 0.0f;
    inst.tile = 0.0f;
    set->instances = {inst};
    strands.setInstances(set);
    EXPECT_LT(set->resolve(strands)[0].y.normalized().x, -0.99f);  // aim tip: back along the chord
    set->instances[0].aimRoot = true;
    EXPECT_GT(set->resolve(strands)[0].y.normalized().x, 0.99f);  // aim root: the way it leaves
    const std::uint64_t aimed = set->contentHash();
    set->instances[0].aimRoot = false;
    EXPECT_NE(set->contentHash(), aimed);

    // Following it: the base flat on the plane, the rest about the strand's curve (no farther
    // from it than the plate's half width and height), the tip at the strand's tip.
    set->instances[0].followStrand = true;
    const auto mesh = buildScaleMesh(strands, nullptr);
    ASSERT_TRUE(mesh);
    const InstancePrototype& plate = *builtinPrototype("groomlab:scale");
    const ScaleMesh::Scale& sc = mesh->scales[0];
    EXPECT_GT(sc.y.normalized().x, 0.99f);  // follow strand aims by the root
    const float across = sc.x.length(), up = sc.z.length(), length = arcLength(curl.data(), curl.size());
    float tipY = -1.0f;
    std::size_t tip = 0;
    for (std::size_t v = 0; v < sc.vertexCount; ++v) {
        const Vec3f& q = plate.points[v];
        const Vec3f& w = mesh->points[sc.firstVertex + v];
        if (q.y > tipY) tipY = q.y, tip = v;
        if (q.y <= 1e-4f) EXPECT_NEAR(w.y, q.z * up, 1e-4f) << v;  // the base: as flat on the plane
        if (q.y >= kStrandBaseForTests) {
            const float reach = std::sqrt(q.x * q.x * across * across + q.z * q.z * up * up);
            EXPECT_LT(distanceToPolyline(w, curl.data(), curl.size()), reach + 1e-3f * length) << v;
        }
    }
    EXPECT_LT((mesh->points[sc.firstVertex + tip] - curl.back()).length(), 0.05f * length);
    // Flat, the same plate reaches nowhere near the curl's top.
    set->instances[0].followStrand = false;
    const auto flat = buildScaleMesh(strands, nullptr);
    float highest = 0.0f;
    for (const Vec3f& w : flat->points) highest = std::max(highest, w.y);
    EXPECT_LT(highest, 0.1f);
}

TEST(Scales, FollowTheStrandOffTheSkinKeptPerScale) {
    // Strands standing straight off a sphere (no bend): flat on the skin, the scales lie
    // low; following the strands, they stand up as spikes, their bases on the skin and nothing
    // inside it. The per-scale cache keeps a scale while its strand stays, bends it again when the
    // strand moves.
    ScaleGraph sg(uvSphere(5.0f), 1.0, 1.0, 0.3);
    sg.g.node(sg.comb).params().set("angle", 0.0);
    sg.params().set("tile", 0.0);
    auto highest = [&](const ScaleMesh& mesh) {
        float h = 0.0f;
        for (const Vec3f& w : mesh.points) h = std::max(h, w.length() - 5.0f);
        return h;
    };
    const StrandsPtr flat = sg.eval();
    EXPECT_LT(highest(*buildScaleMesh(*flat, sg.skin)), 0.2f);
    sg.params().set("follow", std::string("strand"));
    const StrandsPtr spiky = sg.eval();
    ScaleMeshBuilder builder;
    const auto spikes = builder.build(*spiky, sg.skin);
    EXPECT_EQ(builder.stats().bent, spikes->scales.size());
    EXPECT_GT(highest(*spikes), 0.9f);
    float lowest = 1e9f;
    for (const Vec3f& w : spikes->points) lowest = std::min(lowest, w.length() - 5.0f);
    EXPECT_GT(lowest, -0.02f);  // (the sphere's facets: a little under the true sphere)
    builder.build(*spiky, sg.skin);
    EXPECT_EQ(builder.stats().kept, spikes->scales.size());
    // Combed a little: every strand moved, every scale bent again, as a fresh build.
    sg.g.node(sg.comb).params().set("angle", 23.0);
    const StrandsPtr leaning = sg.eval();
    const auto leaned = builder.build(*leaning, sg.skin);
    EXPECT_EQ(builder.stats().kept, 0u);
    EXPECT_EQ(leaned->points, buildScaleMesh(*leaning, sg.skin)->points);
    EXPECT_NE(leaned->points, spikes->points);
    sg.params().set("follow", std::string("hair"));
    EXPECT_THROW(sg.eval(), GraphError);
    sg.params().set("follow", std::string("strand"));
    sg.params().set("aim", std::string("middle"));
    EXPECT_THROW(sg.eval(), GraphError);
}

TEST(Budget, Scales50k) {
    // About 50k scales on a sphere of radius 10 (area 1257): the graph to the Instance node, then
    // the bend (each scale ~100 vertices), then a rebuild that keeps them all.
    ScaleGraph sg(uvSphere(10.0f, 96, 192), 40.0, 0.5, 0.35);
    auto ms = [](std::chrono::steady_clock::time_point t0) {
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    };
    auto t0 = std::chrono::steady_clock::now();
    const StrandsPtr s = sg.eval();
    const double evaluate = ms(t0);
    ASSERT_GT(s->instances()->size(), 40000u);
    ScaleMeshBuilder builder;
    t0 = std::chrono::steady_clock::now();
    const auto mesh = builder.build(*s, sg.skin);
    const double bend = ms(t0);
    t0 = std::chrono::steady_clock::now();
    builder.build(*s, sg.skin);
    const double kept = ms(t0);
    EXPECT_EQ(builder.stats().kept, mesh->scales.size());
    ScaleMeshOptions preview;
    preview.preview = true;
    preview.triangleBudget = 2000000;
    t0 = std::chrono::steady_clock::now();
    const auto quads = buildScaleMesh(*s, sg.skin, {}, preview);
    const double quadMs = ms(t0);
    std::printf("[ budget ] %zu scales: graph %.0f ms, bend %.0f ms (%zu vertices, %zu triangles), kept %.0f ms, "
                "preview quads %.0f ms\n",
                mesh->scales.size(), evaluate, bend, mesh->vertexCount(), mesh->triangleCount(), kept, quadMs);
    EXPECT_TRUE(quads->scales.front().quad);  // past the viewport's budget
    EXPECT_LT(bend, 5000.0);
}

}  // namespace
}  // namespace groomlab
