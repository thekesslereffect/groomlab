// SPDX-License-Identifier: Apache-2.0
// Batch N2, parity with Yeti and XGen: Clump's per-clump variation (volumize, strength and radius
// variance, copy, clump noise), the Transform, Bend, Direction, MeshCut and RenderData nodes,
// Scatter's layouts (blueNoise, uvGrid) and backface culling in the render LOD. Each: what it
// does, the identity at its defaults (and bypassed), and the same result whatever the thread
// count, strand order or evaluation (determinism).

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <set>

#if GROOMLAB_HAS_TBB
#include <tbb/task_arena.h>
#endif

#include "groomlab/curve.h"
#include "groomlab/export_view.h"
#include "groomlab/graph.h"
#include "groomlab/hair_color.h"
#include "groomlab/lod.h"
#include "groomlab/map_ref.h"
#include "groomlab/operators.h"
#include "groomlab/random.h"
#include "test_helpers.h"

namespace groomlab {
namespace {

constexpr float kPi = 3.14159265f;

// The unit plane (XZ, normal +Y) with UVs u = x, v = z: the scalp's U direction is +x.
MeshPtr uvPlane(int cells = 8) {
    auto mesh = std::make_shared<ScalpMesh>(*test::makePlane(cells));
    std::vector<ScalpMesh::UV> uvs;
    for (const ScalpMesh::Triangle& t : mesh->triangles()) {
        for (const std::int32_t v : t) {
            const Vec3f& p = mesh->points()[static_cast<std::size_t>(v)];
            uvs.push_back({p.x, p.z});
        }
    }
    mesh->setCornerUVs(std::move(uvs));
    return mesh;
}

// Straight strands of 16 segments grown up (+Y, length 1) from a scalp, and nodes after them.
struct Rig {
    Graph g;
    ExternalInputs ext;
    MeshPtr scalp;
    NodeId scalpNode = 0, scatter = 0;

    explicit Rig(MeshPtr mesh = uvPlane(), double density = 300.0) : scalp(std::move(mesh)) {
        ext.set("scalp", scalp, scalp->contentHash());
        scalpNode = g.addNode(op::kScalp);
        scatter = g.addNode(op::kScatter);
        g.node(scatter).params().set("density", density);
        const NodeId segments = g.addNode(op::kResample);  // unconnected: the strands' segments
        g.node(segments).params().set("segments", std::int64_t{16});
        g.connect(scalpNode, scatter, 0);
    }
    NodeId add(const char* type, NodeId input, const std::map<std::string, ParamValue>& params = {}) {
        const NodeId id = g.addNode(type);
        g.connect(input, id, 0);
        for (const auto& [k, v] : params) g.node(id).params().set(k, v);
        return id;
    }
    StrandsPtr eval(NodeId id) {
        EvalContext ctx;
        ctx.externals = &ext;
        return g.evaluateStrands(id, ctx);
    }
};

float angleBetween(const Vec3f& a, const Vec3f& b) {
    return std::acos(std::clamp(a.normalized().dot(b.normalized()), -1.0f, 1.0f));
}

Vec3f firstSegment(const StrandSet& s, std::size_t i) { return (s.strandPoints(i)[1] - s.strandPoints(i)[0]).normalized(); }
Vec3f lastSegment(const StrandSet& s, std::size_t i) {
    const std::size_t n = s.strandPointCount(i);
    return (s.strandPoints(i)[n - 1] - s.strandPoints(i)[n - 2]).normalized();
}

// The same strands (IDs, roots, points, widths, channels), bit for bit.
void expectSame(const StrandSet& a, const StrandSet& b, const std::string& what) {
    EXPECT_EQ(a.contentHash(), b.contentHash()) << what;
}

// A node's result with one thread and with many, which must agree to the bit.
void expectThreadIndependent(Rig& rig, NodeId id, const std::string& what) {
#if GROOMLAB_HAS_TBB
    const StrandsPtr many = rig.eval(id);
    rig.g.clearCache();
    StrandsPtr one;
    tbb::task_arena single(1);
    single.execute([&] { one = rig.eval(id); });
    ASSERT_TRUE(many && one);
    expectSame(*one, *many, what + ": one thread and many differ");
#else
    (void)rig;
    (void)id;
    (void)what;
#endif
}

// ---- Transform -------------------------------------------------------------------------------

TEST(Transform, IdentityAtItsDefaultsAndWhenBypassed) {
    Rig rig;
    const NodeId t = rig.add(op::kTransform, rig.scatter);
    EXPECT_EQ(rig.eval(t), rig.eval(rig.scatter));  // handed on as it came
    rig.g.node(t).params().set("rotate", Vec3f{0.0f, 0.0f, 40.0f});
    EXPECT_NE(rig.eval(t)->contentHash(), rig.eval(rig.scatter)->contentHash());
    rig.g.setBypass(t, true);
    EXPECT_EQ(rig.eval(t), rig.eval(rig.scatter));
    rig.g.setBypass(t, false);
    rig.g.node(t).params().set(kAffectParam, 0.0);
    EXPECT_EQ(rig.eval(t)->contentHash(), rig.eval(rig.scatter)->contentHash());
}

TEST(Transform, MovesTurnsAndScalesEachStrandInItsRootFrame) {
    Rig rig;
    const StrandsPtr before = rig.eval(rig.scatter);
    // Scale 0.5 about the root, moved 0.1 along the normal (+y), turned 90 degrees about the
    // frame's x (the scalp's U, +x): straight up turns to lie along -z or +z, flat.
    const NodeId t = rig.add(op::kTransform, rig.scatter,
                             {{"scale", 0.5}, {"translate", Vec3f{0.0f, 0.1f, 0.0f}}, {"rotate", Vec3f{90.0f, 0.0f, 0.0f}}});
    const StrandsPtr after = rig.eval(t);
    ASSERT_EQ(after->strandCount(), before->strandCount());
    for (std::size_t s = 0; s < after->strandCount(); ++s) {
        const Vec3f root = before->strandPoints(s)[0];
        EXPECT_NEAR((after->strandPoints(s)[0] - (root + Vec3f(0, 0.1f, 0))).length(), 0.0f, 1e-5f);
        EXPECT_NEAR(arcLength(after->strandPoints(s), after->strandPointCount(s)), 0.5f, 1e-4f);
        const Vec3f d = lastSegment(*after, s);
        EXPECT_NEAR(d.y, 0.0f, 1e-4f) << "lies flat";
        EXPECT_NEAR(std::fabs(d.z), 1.0f, 1e-4f) << "across the U direction";
    }
    // About the normal (y): a straight strand along it stays.
    const NodeId spin = rig.add(op::kTransform, rig.scatter, {{"rotate", Vec3f{0.0f, 70.0f, 0.0f}}});
    const StrandsPtr spun = rig.eval(spin);
    for (std::size_t s = 0; s < spun->strandCount(); s += 7) {
        for (std::size_t i = 0; i < spun->strandPointCount(s); ++i) {
            EXPECT_NEAR((spun->strandPoints(s)[i] - before->strandPoints(s)[i]).length(), 0.0f, 1e-5f);
        }
    }
}

TEST(Transform, TheWholeSetTurnsAboutThePivot) {
    Rig rig;
    const StrandsPtr before = rig.eval(rig.scatter);
    const Vec3f pivot{0.5f, 0.0f, 0.5f};
    const NodeId t = rig.add(op::kTransform, rig.scatter,
                             {{"space", std::string("world")}, {"rotate", Vec3f{0.0f, 0.0f, 90.0f}}, {"pivot", pivot}, {"scale", 2.0}});
    const StrandsPtr after = rig.eval(t);
    for (std::size_t s = 0; s < after->strandCount(); s += 5) {
        for (std::size_t i = 0; i < after->strandPointCount(s); i += 4) {
            const Vec3f p = before->strandPoints(s)[i] - pivot;
            const Vec3f want = pivot + Vec3f(-p.y, p.x, p.z) * 2.0f;  // 90 degrees about +z
            EXPECT_NEAR((after->strandPoints(s)[i] - want).length(), 0.0f, 1e-4f);
        }
    }
}

TEST(Transform, RandomnessIsPerStrandIdAndDeterministic) {
    Rig rig;
    const std::map<std::string, ParamValue> params = {{"translateRandom", Vec3f{0.05f, 0.0f, 0.05f}},
                                                      {"rotateRandom", Vec3f{10.0f, 30.0f, 0.0f}},
                                                      {"scaleRandom", 0.3},
                                                      {"seed", std::int64_t{4}}};
    const NodeId a = rig.add(op::kTransform, rig.scatter, params);
    const NodeId b = rig.add(op::kTransform, rig.scatter, params);
    const StrandsPtr ra = rig.eval(a), rb = rig.eval(b);
    expectSame(*ra, *rb, "two nodes, the same params");
    // Different strands moved by different amounts; another seed, another groom.
    std::set<float> lengths;
    for (std::size_t s = 0; s < ra->strandCount(); ++s) lengths.insert(arcLength(ra->strandPoints(s), ra->strandPointCount(s)));
    EXPECT_GT(lengths.size(), ra->strandCount() / 2);
    rig.g.node(b).params().set("seed", std::int64_t{5});
    EXPECT_NE(rig.eval(b)->contentHash(), ra->contentHash());
    expectThreadIndependent(rig, a, "Transform");
    // A strand's result is its own: the same in a subset of the strands, in another order.
    const StrandsPtr in = rig.eval(rig.scatter);
    std::vector<std::size_t> picked;
    for (std::size_t s = in->strandCount(); s-- > 0;) {
        if (s % 3 == 0) picked.push_back(s);
    }
    auto op = OperatorRegistry::instance().create(op::kTransform);
    for (const auto& [k, v] : params) op->params().set(k, v);
    EvalContext ctx;
    ctx.externals = &rig.ext;
    const StrandsPtr sub = valueAs<StrandsPtr>(op->evaluate(ctx, {StrandsPtr(std::make_shared<StrandSet>(in->subset(picked)))}));
    for (std::size_t k = 0; k < picked.size(); ++k) {
        for (std::size_t i = 0; i < sub->strandPointCount(k); ++i) EXPECT_EQ(sub->strandPoints(k)[i], ra->strandPoints(picked[k])[i]);
    }
}

// ---- Bend ------------------------------------------------------------------------------------

TEST(Bend, TurnsEvenlyBetweenStartAndEndKeepingLengths) {
    Rig rig;
    const StrandsPtr before = rig.eval(rig.scatter);
    const NodeId zero = rig.add(op::kBend, rig.scatter, {{"angle", 0.0}});
    EXPECT_EQ(rig.eval(zero), before);
    // 90 degrees toward +x between 0.25 and 0.75: the first quarter stays upright, the last lies along +x.
    const NodeId bend = rig.add(op::kBend, rig.scatter,
                                {{"angle", 90.0}, {"along", std::string("direction")}, {"direction", Vec3f{1.0f, 0.0f, 0.0f}},
                                 {"start", 0.25}, {"end", 0.75}});
    const StrandsPtr bent = rig.eval(bend);
    for (std::size_t s = 0; s < bent->strandCount(); ++s) {
        const std::size_t n = bent->strandPointCount(s);
        EXPECT_NEAR(arcLength(bent->strandPoints(s), n), 1.0f, 1e-4f);
        for (std::size_t i = 0; i <= 4; ++i) EXPECT_NEAR((bent->strandPoints(s)[i] - before->strandPoints(s)[i]).length(), 0.0f, 1e-6f);
        EXPECT_NEAR(lastSegment(*bent, s).x, 1.0f, 1e-4f);
        EXPECT_NEAR(angleBetween(lastSegment(*bent, s), Vec3f(1, 0, 0)), 0.0f, 1e-3f);
    }
    // Along the normal, bending over (either sign): toward the scalp's U (+x) for a strand standing up.
    const NodeId over = rig.add(op::kBend, rig.scatter, {{"angle", -60.0}});
    const StrandsPtr leaned = rig.eval(over);
    for (std::size_t s = 0; s < leaned->strandCount(); s += 9) {
        const Vec3f d = lastSegment(*leaned, s);
        EXPECT_NEAR(angleBetween(d, Vec3f(0, 1, 0)), 60.0f * kPi / 180.0f, 1e-3f);
        EXPECT_GT(d.x, 0.5f);
    }
    expectThreadIndependent(rig, bend, "Bend");
}

TEST(Bend, AwayFromAndTowardTheScalp) {
    // Strands leaning over: a positive angle stands them up toward the normal, a negative one
    // lays them down; randomness bends each by its own share, the same every time.
    Rig rig;
    const NodeId lean = rig.add(op::kTransform, rig.scatter, {{"rotate", Vec3f{0.0f, 0.0f, -45.0f}}});
    const NodeId up = rig.add(op::kBend, lean, {{"angle", 30.0}});
    const NodeId down = rig.add(op::kBend, lean, {{"angle", -30.0}, {"randomness", 0.5}, {"seed", std::int64_t{3}}});
    const StrandsPtr leaning = rig.eval(lean), raised = rig.eval(up), lowered = rig.eval(down);
    const Vec3f normal{0, 1, 0};
    std::set<float> tilts;
    for (std::size_t s = 0; s < raised->strandCount(); ++s) {
        const float was = angleBetween(lastSegment(*leaning, s), normal);
        EXPECT_NEAR(angleBetween(lastSegment(*raised, s), normal), was - 30.0f * kPi / 180.0f, 1e-3f);
        const float now = angleBetween(lastSegment(*lowered, s), normal);
        EXPECT_GE(now, was - 1e-4f);
        EXPECT_LE(now, was + 30.0f * kPi / 180.0f + 1e-4f);
        tilts.insert(std::round(now * 1000.0f));
    }
    EXPECT_GT(tilts.size(), 5u) << "randomness bends strands by different amounts";
    EXPECT_EQ(rig.eval(down)->contentHash(), lowered->contentHash());
}

// ---- Direction -------------------------------------------------------------------------------

TEST(Direction, InclinationFromTheNormalAndRollAroundIt) {
    Rig rig;
    const StrandsPtr before = rig.eval(rig.scatter);
    // Inclination 0: along the normal, as Scatter grows them (each point to the bit? within rounding).
    const NodeId straight = rig.add(op::kDirection, rig.scatter, {{"inclination", 0.0}});
    const StrandsPtr up = rig.eval(straight);
    for (std::size_t s = 0; s < up->strandCount(); s += 5) EXPECT_NEAR(firstSegment(*up, s).y, 1.0f, 1e-5f);
    // Flat along the scalp's U (+x), then rolled a quarter turn to the bitangent (U x N = +z).
    const ParamValue tangent = std::string("tangent");  // (roll 0 along the scalp's U)
    const NodeId flat = rig.add(op::kDirection, rig.scatter, {{"inclination", 90.0}, {"roll", 0.0}, {"along", tangent}});
    const NodeId rolled = rig.add(op::kDirection, rig.scatter, {{"inclination", 90.0}, {"roll", 90.0}, {"along", tangent}});
    const NodeId half = rig.add(op::kDirection, rig.scatter, {{"inclination", 90.0}, {"strength", 0.5}, {"along", tangent}});
    const StrandsPtr f = rig.eval(flat), r = rig.eval(rolled), h = rig.eval(half);
    for (std::size_t s = 0; s < f->strandCount(); ++s) {
        EXPECT_NEAR(angleBetween(firstSegment(*f, s), Vec3f(1, 0, 0)), 0.0f, 1e-3f);
        EXPECT_NEAR(angleBetween(firstSegment(*r, s), Vec3f(0, 0, 1)), 0.0f, 1e-3f);
        EXPECT_NEAR(angleBetween(firstSegment(*h, s), Vec3f(0, 1, 0)), 45.0f * kPi / 180.0f, 1e-3f);
        EXPECT_EQ(f->strandPoints(s)[0], before->strandPoints(s)[0]);  // about the root
        EXPECT_NEAR(arcLength(f->strandPoints(s), f->strandPointCount(s)), 1.0f, 1e-4f);
    }
    // Without UVs roll 0 points along the direction laid onto the scalp.
    Rig noUV(test::makePlane(8));
    const NodeId byVector = noUV.add(op::kDirection, noUV.scatter,
                                     {{"inclination", 90.0}, {"direction", Vec3f{0.0f, -1.0f, 1.0f}}});
    const StrandsPtr v = noUV.eval(byVector);
    for (std::size_t s = 0; s < v->strandCount(); s += 3) EXPECT_NEAR(angleBetween(firstSegment(*v, s), Vec3f(0, 0, 1)), 0.0f, 1e-3f);
    const NodeId random = rig.add(op::kDirection, rig.scatter, {{"rollRandom", 90.0}, {"inclinationRandom", 10.0}});
    expectThreadIndependent(rig, random, "Direction");
    rig.g.setBypass(random, true);
    EXPECT_EQ(rig.eval(random), rig.eval(rig.scatter));  // (the cache cleared above: the scatter made again)
}

// ---- MeshCut ---------------------------------------------------------------------------------

// A horizontal plane at height y facing down (normal -Y, toward the scalp: the strands start
// outside it), wider than the unit scalp.
MeshPtr shelf(float y) {
    const auto plane = test::makePlane(4, 3.0f);
    std::vector<Vec3f> points = plane->points();
    for (Vec3f& p : points) p = p + Vec3f(-1.0f, y, -1.0f);
    std::vector<ScalpMesh::Triangle> flipped = plane->triangles();
    for (ScalpMesh::Triangle& t : flipped) std::swap(t[1], t[2]);
    return std::make_shared<const ScalpMesh>(std::move(points), std::move(flipped));
}

TEST(MeshCut, CutsWhereStrandsGoIntoTheMesh) {
    Rig rig;
    rig.ext.set("mesh:shelf", shelf(0.5f), 7);
    const NodeId mesh = rig.g.addNode(op::kColliderMesh);
    rig.g.node(mesh).params().set("name", std::string("shelf"));
    const NodeId cut = rig.add(op::kMeshCut, rig.scatter);
    rig.g.connect(mesh, cut, 1);
    const NodeId shorter = rig.add(op::kMeshCut, rig.scatter, {{"offset", 0.1}});
    rig.g.connect(mesh, shorter, 1);
    const StrandsPtr before = rig.eval(rig.scatter), a = rig.eval(cut), b = rig.eval(shorter);
    ASSERT_EQ(a->pointCount(), before->pointCount());  // the same points, spread over the kept part
    EXPECT_EQ(a->ids(), before->ids());
    for (std::size_t s = 0; s < a->strandCount(); ++s) {
        const std::size_t n = a->strandPointCount(s);
        EXPECT_EQ(a->strandPoints(s)[0], before->strandPoints(s)[0]);
        EXPECT_NEAR(a->strandPoints(s)[n - 1].y, 0.5f, 1e-4f);
        EXPECT_NEAR(b->strandPoints(s)[n - 1].y, 0.4f, 1e-4f);
        EXPECT_NEAR(a->strandPoints(s)[n / 2].y, 0.25f, 1e-4f);  // evenly spread
    }
    expectThreadIndependent(rig, shorter, "MeshCut");
    // No mesh: the strands as they came, with a note.
    const NodeId alone = rig.add(op::kMeshCut, rig.scatter);
    EXPECT_EQ(rig.eval(alone), rig.eval(rig.scatter));
}

TEST(MeshCut, InsideAHullAndRootsOnTheCutSide) {
    // A ball round the scalp's middle: kept inside (a haircut's hull), strands end on its surface;
    // kept outside, strands rooted in it are cut to nothing and the others stay whole.
    Rig rig;
    const Vec3f center{0.5f, 0.0f, 0.5f};
    const auto unit = test::makeSphere(24, 48, 0.5f);
    std::vector<Vec3f> points = unit->points();
    for (Vec3f& p : points) p += center;
    rig.ext.set("mesh:ball", MeshPtr(std::make_shared<const ScalpMesh>(std::move(points), unit->triangles())), 9);
    const NodeId ball = rig.g.addNode(op::kColliderMesh);
    rig.g.node(ball).params().set("name", std::string("ball"));
    const NodeId hull = rig.add(op::kMeshCut, rig.scatter, {{"keep", std::string("inside")}});
    const NodeId obstacle = rig.add(op::kMeshCut, rig.scatter);
    rig.g.connect(ball, hull, 1);
    rig.g.connect(ball, obstacle, 1);
    const StrandsPtr before = rig.eval(rig.scatter), in = rig.eval(hull), out = rig.eval(obstacle);
    std::size_t inside = 0, outside = 0;
    for (std::size_t s = 0; s < in->strandCount(); ++s) {
        const std::size_t n = in->strandPointCount(s);
        const Vec3f root = before->strandPoints(s)[0];
        const bool rootIn = (root - center).length() < 0.5f - 1e-3f;
        const bool rootOut = (root - center).length() > 0.5f + 1e-3f;
        if (rootIn) {
            ++inside;
            EXPECT_NEAR((in->strandPoints(s)[n - 1] - center).length(), 0.5f, 4e-3f) << "ends on the hull (a tessellated ball)";
            EXPECT_NEAR(arcLength(out->strandPoints(s), n), 0.0f, 1e-6f) << "rooted in the obstacle: nothing kept";
        } else if (rootOut) {
            ++outside;
            EXPECT_NEAR(arcLength(in->strandPoints(s), n), 0.0f, 1e-6f) << "rooted outside the hull: nothing kept";
            EXPECT_NEAR(arcLength(out->strandPoints(s), n), 1.0f, 1e-4f) << "never meets the obstacle: whole";
        }
    }
    EXPECT_GT(inside, 20u);
    EXPECT_GT(outside, 20u);
}

// ---- RenderData ------------------------------------------------------------------------------

TEST(RenderData, WritesItsDefaultsWithoutParams) {
    Rig rig;
    const NodeId data = rig.add(op::kRenderData, rig.scatter);
    const StrandsPtr out = rig.eval(data), in = rig.eval(rig.scatter);
    EXPECT_EQ(out->positions(), in->positions());
    // The highlight is HairColor's: RenderData writes the opacity alone.
    for (const char* name : {kSpecularChannel, kRoughnessChannel, kTiltChannel}) EXPECT_FALSE(out->strandChannel(name)) << name;
    const std::vector<float>* opacity = out->pointChannel(kOpacityChannel);
    ASSERT_TRUE(opacity);
    for (const float v : *opacity) EXPECT_EQ(v, 1.0f);
}

TEST(RenderData, OpacityFollowsItsProfileAndVariesPerStrand) {
    Rig rig;
    const NodeId data = rig.add(op::kRenderData, rig.scatter,
                                {{"opacityProfile", Ramp::linear(1.0f, 0.2f)}, {"opacityRandom", 0.5}, {"seed", std::int64_t{2}}});
    const StrandsPtr out = rig.eval(data);
    const std::vector<float>& opacity = *out->pointChannel(kOpacityChannel);
    std::set<float> distinct;
    for (std::size_t s = 0; s < out->strandCount(); ++s) {
        const std::size_t begin = out->strandBegin(s), n = out->strandPointCount(s);
        const float root = opacity[begin], tip = opacity[begin + n - 1];
        EXPECT_GE(root, 0.5f - 1e-6f);
        EXPECT_LE(root, 1.0f);
        EXPECT_NEAR(tip, root * 0.2f, 1e-5f);
        distinct.insert(root);
    }
    EXPECT_GT(distinct.size(), out->strandCount() / 2);
    expectThreadIndependent(rig, data, "RenderData");
}

// ---- HairColor's highlight -------------------------------------------------------------------

TEST(HairColorHighlight, DefaultsWriteNoChannels) {
    // At the defaults (the look every host shades without them) no highlight channel is written;
    // a later HairColor at the defaults takes an earlier one's away (the later look wins).
    Rig rig;
    const NodeId plain = rig.add(op::kHairColor, rig.scatter);
    const StrandsPtr out = rig.eval(plain);
    for (const char* name : {kSpecularChannel, kRoughnessChannel, kTiltChannel}) EXPECT_FALSE(out->strandChannel(name)) << name;
    const NodeId shiny = rig.add(op::kHairColor, rig.scatter, {{"specular", 0.6}, {"roughness", 0.2}, {"tilt", 5.0}});
    const NodeId again = rig.add(op::kHairColor, shiny);
    ASSERT_TRUE(rig.eval(shiny)->strandChannel(kTiltChannel));
    for (const char* name : {kSpecularChannel, kRoughnessChannel, kTiltChannel}) {
        EXPECT_FALSE(rig.eval(again)->strandChannel(name)) << name;
    }
    // The colors are the same with or without the highlight.
    const StrandsPtr lit = rig.eval(shiny);
    for (const char* name : kColorChannels) EXPECT_EQ(*lit->strandChannel(name), *out->strandChannel(name)) << name;
}

TEST(HairColorHighlight, ValuesVaryPerStrandAndMapsReachThem) {
    Rig rig;
    MapRef byX;
    byX.source = MapRef::Source::Expression;
    byX.name = "u";  // the root's U (0-1 across the plane)
    const NodeId color = rig.add(op::kHairColor, rig.scatter,
                                 {{"specular", 0.4}, {"specularRandom", 0.5}, {"roughness", byX}, {"tilt", 6.5}, {"seed", std::int64_t{2}}});
    const StrandsPtr out = rig.eval(color);
    const std::vector<float>& specular = *out->strandChannel(kSpecularChannel);
    const std::vector<float>& roughness = *out->strandChannel(kRoughnessChannel);
    const std::vector<float>& tilt = *out->strandChannel(kTiltChannel);
    EXPECT_FLOAT_EQ(out->strandChannelDefault(kSpecularChannel), kDefaultHairSpecular) << "strands merged in take the default";
    EXPECT_FLOAT_EQ(out->strandChannelDefault(kRoughnessChannel), kDefaultHairRoughness);
    EXPECT_FLOAT_EQ(out->strandChannelDefault(kTiltChannel), kDefaultHairTilt);
    std::set<float> distinct;
    for (std::size_t s = 0; s < out->strandCount(); ++s) {
        EXPECT_GE(specular[s], 0.2f - 1e-5f);
        EXPECT_LE(specular[s], 0.6f + 1e-5f);
        EXPECT_NEAR(roughness[s], out->strandPoints(s)[0].x, 2e-2f);  // the map: U is x
        EXPECT_FLOAT_EQ(tilt[s], 6.5f);
        distinct.insert(specular[s]);
    }
    EXPECT_GT(distinct.size(), out->strandCount() / 2);
    // Seeded by the strand's ID: another seed, other draws.
    const NodeId reseeded = rig.add(op::kHairColor, rig.scatter, {{"specular", 0.4}, {"specularRandom", 0.5}, {"seed", std::int64_t{3}}});
    EXPECT_NE(*rig.eval(reseeded)->strandChannel(kSpecularChannel), specular);
    expectThreadIndependent(rig, color, "HairColor's highlight");
}

// ---- Clump's per-clump variation -------------------------------------------------------------

// Strands leaning every which way (each its own shape, so the clumps' centers differ), clumped
// toward about 12 centers of the Clump's own.
struct Clumped {
    Rig rig;
    NodeId strands = 0;
    Clumped() : rig(uvPlane(), 600.0) {
        strands = rig.add(op::kTransform, rig.scatter, {{"rotateRandom", Vec3f{30.0f, 0.0f, 30.0f}}});
    }
    NodeId clump(std::map<std::string, ParamValue> params) {
        params.emplace("count", std::int64_t{12});
        return rig.add(op::kClump, strands, params);
    }
};

TEST(ClumpVariation, NothingChangesAtTheDefaults) {
    Clumped c;
    const NodeId plain = c.clump({});
    const NodeId zeros = c.clump({{"volumize", 0.0}, {"strengthRandom", 0.0}, {"lockRadiusRandom", 0.0}, {"copyAmount", 0.0},
                                  {"copyNeighbors", std::int64_t{3}}});
    expectSame(*c.rig.eval(zeros), *c.rig.eval(plain), "the variation at 0");
    for (const char* name : {"volumize", "strengthRandom", "copyAmount"}) {
        const NodeId one = c.clump({{name, 0.5}});
        EXPECT_NE(c.rig.eval(one)->contentHash(), c.rig.eval(plain)->contentHash()) << name;
    }
    const NodeId locks = c.clump({{"lockRadius", 0.05}});
    const NodeId varied = c.clump({{"lockRadius", 0.05}, {"lockRadiusRandom", 0.8}});
    EXPECT_NE(c.rig.eval(varied)->contentHash(), c.rig.eval(locks)->contentHash());
}

TEST(ClumpVariation, ClumpNoiseAndCopyMoveEachClumpAsOne) {
    // Pulled all the way (strength 1, a flat profile), every strand of a clump lies on its center:
    // with copy and a Noise on the centers (clump noise: guides made of some of the strands, waved
    // before the Clump), still on one curve per clump.
    Clumped c;
    const std::map<std::string, ParamValue> full = {
        {"strength", 1.0}, {"profile", Ramp::constant(1.0f)}, {"centersFrom", std::string("inputPoints")}};
    auto with = full;
    with["copyAmount"] = 0.7;
    with["copyNeighbors"] = std::int64_t{2};
    const NodeId centers = c.rig.add(op::kToGuides, c.strands, {{"fraction", 0.02}});
    const NodeId plain = c.rig.add(op::kClump, c.strands, full);
    c.rig.g.connect(centers, plain, 1);
    const NodeId wave = c.rig.add(op::kNoise, centers, {{"amplitude", 0.2}, {"frequency", 3.0}, {"octaves", std::int64_t{1}}});
    const NodeId waved = c.rig.add(op::kClump, c.strands, with);
    c.rig.g.connect(wave, waved, 1);
    const StrandsPtr a = c.rig.eval(plain), b = c.rig.eval(waved);
    const std::vector<float>& ids = *b->strandChannel("clumpId");
    std::map<int, Vec3f> tipOf;
    float moved = 0.0f;
    for (std::size_t s = 0; s < b->strandCount(); ++s) {
        const std::size_t n = b->strandPointCount(s);
        const Vec3f tip = b->strandPoints(s)[n - 1];
        const auto [it, fresh] = tipOf.emplace(static_cast<int>(ids[s]), tip);
        if (!fresh) EXPECT_NEAR((it->second - tip).length(), 0.0f, 1e-4f) << "one clump, one tip";
        moved = std::max(moved, (tip - a->strandPoints(s)[n - 1]).length());
    }
    EXPECT_GT(tipOf.size(), 5u);
    EXPECT_GT(moved, 0.02f) << "the noise and copy moved the clumps";
    expectThreadIndependent(c.rig, waved, "Clump noise and copy");
}

TEST(ClumpVariation, VolumizeFillsTheClumpNearTheRoot) {
    Clumped c;
    const NodeId plain = c.clump({{"strength", 0.6}});
    const NodeId full = c.clump({{"strength", 0.6}, {"volumize", 1.0}});
    const NodeId weaker = c.clump({{"strength", 0.6}, {"strengthRandom", 1.0}});
    const StrandsPtr a = c.rig.eval(plain), b = c.rig.eval(full), w = c.rig.eval(weaker);
    // The spread of each clump about its strands' mean at a quarter of the length: wider volumized.
    auto spread = [](const StrandSet& s) {
        const std::vector<float>& ids = *s.strandChannel("clumpId");
        std::map<int, std::pair<Vec3f, int>> mean;
        const std::size_t at = 4;
        for (std::size_t i = 0; i < s.strandCount(); ++i) {
            auto& m = mean[static_cast<int>(ids[i])];
            m.first += s.strandPoints(i)[at];
            ++m.second;
        }
        double sum = 0.0;
        for (std::size_t i = 0; i < s.strandCount(); ++i) {
            const auto& m = mean[static_cast<int>(ids[i])];
            sum += (s.strandPoints(i)[at] - m.first / static_cast<float>(m.second)).length();
        }
        return sum / static_cast<double>(s.strandCount());
    };
    EXPECT_GT(spread(*b), spread(*a) * 1.2);
    EXPECT_GT(spread(*w), spread(*a)) << "weaker clumps gather less";
    expectThreadIndependent(c.rig, full, "Clump volumize");
}

// Batch N4: Yeti's maintain length: a RestoreLength after the Clump, its reference the strands the
// Clump took.
TEST(ClumpVariation, RestoreLengthAfterAClumpKeepsEveryStrandsLength) {
    Clumped c;
    const NodeId plain = c.clump({{"strength", 1.0}});
    const NodeId keeping = c.rig.add(op::kRestoreLength, plain);
    c.rig.g.connect(c.strands, keeping, 1);
    const StrandsPtr in = c.rig.eval(c.strands), a = c.rig.eval(plain), b = c.rig.eval(keeping);
    auto length = [](const StrandSet& s, std::size_t i) {
        float sum = 0.0f;
        for (std::size_t k = 1; k < s.strandPointCount(i); ++k) sum += (s.strandPoints(i)[k] - s.strandPoints(i)[k - 1]).length();
        return sum;
    };
    float shortened = 0.0f, worst = 0.0f;
    for (std::size_t i = 0; i < in->strandCount(); ++i) {
        const float l = length(*in, i);
        shortened = std::max(shortened, std::abs(l - length(*a, i)) / l);
        worst = std::max(worst, std::abs(length(*b, i) - l) / l);
        EXPECT_EQ(b->strandPoints(i)[0], in->strandPoints(i)[0]) << "the root moved";
    }
    EXPECT_GT(shortened, 0.01f) << "strong clumping changed no length: the test shows nothing";
    EXPECT_LT(worst, 1e-5f) << "a strand's length changed by " << worst * 100.0f << "%";
    expectThreadIndependent(c.rig, keeping, "Clump then RestoreLength");
}

// ---- Scatter's layouts -----------------------------------------------------------------------

TEST(ScatterLayout, AUvGridPutsARootAtEachCellsCenter) {
    Rig rig;
    const NodeId grid = rig.add(op::kScatter, rig.scalpNode,
                                {{"layout", std::string("uvGrid")}, {"gridRows", std::int64_t{10}}, {"gridColumns", std::int64_t{20}}});
    const StrandsPtr s = rig.eval(grid);
    ASSERT_EQ(s->strandCount(), 200u);
    std::set<std::pair<int, int>> cells;
    std::set<StrandId> ids(s->ids().begin(), s->ids().end());
    EXPECT_EQ(ids.size(), 200u);
    for (std::size_t i = 0; i < s->strandCount(); ++i) {
        const Vec3f root = s->strandPoints(i)[0];
        const float col = root.x * 20.0f - 0.5f, row = root.z * 10.0f - 0.5f;
        EXPECT_NEAR(col, std::round(col), 1e-3f);
        EXPECT_NEAR(row, std::round(row), 1e-3f);
        cells.insert({static_cast<int>(std::lround(row)), static_cast<int>(std::lround(col))});
    }
    EXPECT_EQ(cells.size(), 200u);
    // Jittered: off the centers, inside the cells; the same IDs.
    rig.g.node(grid).params().set("gridRandom", 0.8);
    const StrandsPtr j = rig.eval(grid);
    EXPECT_EQ(j->ids(), s->ids());
    for (std::size_t i = 0; i < j->strandCount(); ++i) {
        EXPECT_LE(std::fabs(j->strandPoints(i)[0].x - s->strandPoints(i)[0].x), 0.4f / 20.0f + 1e-5f);
    }
    // A preview of so few (250 or fewer) keeps them all.
    EvalContext preview;
    preview.externals = &rig.ext;
    preview.previewDensity = 0.5f;
    const StrandsPtr thin = rig.g.evaluateStrands(grid, preview);
    EXPECT_EQ(thin->strandCount(), 200u);
    for (const StrandId id : thin->ids()) EXPECT_TRUE(ids.count(id));
    // On a deformed scalp the grid keeps its strands.
    std::vector<Vec3f> moved = rig.scalp->points();
    for (Vec3f& p : moved) p.y += 0.3f * p.x;
    rig.ext.set("scalp", MeshPtr(std::make_shared<const ScalpMesh>(rig.scalp->withPoints(moved))), 99);
    EXPECT_EQ(rig.eval(grid)->ids(), j->ids());
    expectThreadIndependent(rig, grid, "uvGrid");
    // Without UVs: an error.
    Rig noUV(test::makePlane(4));
    const NodeId bad = noUV.add(op::kScatter, noUV.scalpNode, {{"layout", std::string("uvGrid")}});
    EXPECT_THROW(noUV.eval(bad), GraphError);
}

TEST(ScatterLayout, ADenseGridEvaluatesSaysItIsHeavyAndAnAbsurdOneStopsWithAnError) {
    // A tester's 1000 x 1000 grid (a million roots) evaluated, previewed thinly and in full; past
    // 2M strands the node notes it is heavy; past 50M (20000 x 20000) an error, never an allocation.
    Rig rig;
    const NodeId grid = rig.add(op::kScatter, rig.scalpNode,
                                {{"layout", std::string("uvGrid")}, {"gridRows", std::int64_t{1000}}, {"gridColumns", std::int64_t{1000}}});
    EvalContext preview;
    preview.externals = &rig.ext;
    preview.previewDensity = 0.1f;
    const StrandsPtr thin = rig.g.evaluateStrands(grid, preview);
    EXPECT_GT(thin->strandCount(), 50000u);
    EXPECT_EQ(rig.g.lastNotes().count(grid), 0u);  // a million: no note
    EXPECT_EQ(rig.eval(grid)->strandCount(), 1000000u);
    rig.g.node(grid).params().set("gridRows", std::int64_t{2000});
    rig.g.node(grid).params().set("gridColumns", std::int64_t{1500});
    preview.previewDensity = 0.01f;
    EXPECT_GT(rig.g.evaluateStrands(grid, preview)->strandCount(), 10000u);
    ASSERT_EQ(rig.g.lastNotes().count(grid), 1u);
    EXPECT_NE(rig.g.lastNotes().at(grid).find("3.0M strands: heavy"), std::string::npos) << rig.g.lastNotes().at(grid);
    rig.g.node(grid).params().set("gridRows", std::int64_t{20000});
    rig.g.node(grid).params().set("gridColumns", std::int64_t{20000});
    EXPECT_THROW(rig.eval(grid), GraphError);
}

TEST(ScatterLayout, RandomIsTheDefaultAndRandomnessZeroSpreadsEvenly) {
    Rig rig;
    const NodeId zero = rig.add(op::kScatter, rig.scalpNode, {{"randomness", 0.0}, {"density", 200.0}});
    const NodeId random = rig.add(op::kScatter, rig.scalpNode, {{"density", 200.0}});
    EXPECT_NE(rig.eval(zero)->contentHash(), rig.eval(random)->contentHash());
    const NodeId explicitRandom = rig.add(op::kScatter, rig.scalpNode, {{"layout", std::string("random")}, {"density", 200.0}});
    expectSame(*rig.eval(explicitRandom), *rig.eval(random), "random is the default");
}

// ---- Backface culling ------------------------------------------------------------------------

TEST(Lod, BackfaceCullingDropsStrandsRootedFacingAway) {
    Rig rig(test::makeSphere(24, 48, 1.0f), 80.0);
    const StrandsPtr strands = rig.eval(rig.scatter);
    LodCamera cam;  // at z = 10 looking down -z, at the sphere
    cam.matrix[3][2] = 10.0f;
    LodSettings settings;
    EXPECT_EQ(planLod(*strands, cam, settings, rig.scalp.get()).kept.size(), strands->strandCount());  // off by default
    settings.backface = true;
    settings.backfaceAngle = 0.0f;
    const LodPlan plan = planLod(*strands, cam, settings, rig.scalp.get());
    EXPECT_GT(plan.backfacing, strands->strandCount() / 3);
    EXPECT_LT(plan.backfacing, strands->strandCount() * 2 / 3);
    EXPECT_EQ(plan.kept.size() + plan.backfacing, strands->strandCount());
    for (std::size_t s = 0, k = 0; s < strands->strandCount(); ++s) {
        const Vec3f root = strands->strandPoints(s)[0];
        const Vec3f toCamera = (Vec3f(0, 0, 10) - root).normalized();
        const bool kept = k < plan.kept.size() && plan.kept[k] == s;
        k += kept ? 1 : 0;
        EXPECT_EQ(kept, rig.scalp->normal(strands->roots()[s]).dot(toCamera) >= 0.0f) << s;
    }
    // Padding keeps more; the same plan without the scalp (the strands' first segments: here the
    // normals) and every time.
    settings.backfaceAngle = 30.0f;
    const LodPlan padded = planLod(*strands, cam, settings, rig.scalp.get());
    EXPECT_LT(padded.backfacing, plan.backfacing);
    settings.backfaceAngle = 0.0f;
    const LodPlan bySegments = planLod(*strands, cam, settings);
    EXPECT_GT(bySegments.backfacing, plan.backfacing * 9 / 10);  // the first segments: about the normals
    EXPECT_LT(bySegments.backfacing, plan.backfacing * 11 / 10);
    EXPECT_EQ(planLod(*strands, cam, settings, rig.scalp.get()).kept, plan.kept);
    // Another motion key takes the first key's plan: the same strands.
    StrandSet key = *strands;
    for (Vec3f& p : key.mutablePositions()) p += Vec3f(0.0f, 0.0f, 3.0f);
    EXPECT_EQ(applyLod(key, plan).ids(), applyLod(*strands, plan).ids());
}

}  // namespace
}  // namespace groomlab
