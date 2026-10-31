// SPDX-License-Identifier: Apache-2.0
// Feathers for engines (the Meerkat eagle workflow): Propagate's shafts as a rachis mesh to skin
// into the character, barbs rooted on its surface (so Unreal's skinning binding picks their own
// rachis), groom groups, and the lighter groom for heavy shots.

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <set>

#include "groomlab/curve.h"
#include "groomlab/export_view.h"
#include "groomlab/feathers.h"
#include "groomlab/graph.h"
#include "groomlab/operators.h"
#include "groomlab/strand_geometry.h"
#include "test_helpers.h"

namespace groomlab {
namespace {

float channel(const StrandSet& s, const char* name, std::size_t i) { return (*s.strandChannel(name))[i]; }

// A plane scalp and `count` shafts rising off it along +X, curving up (so their frames turn),
// 0.45 apart in z.
struct Shafts {
    MeshPtr scalp;
    StrandsPtr shafts;
    ExternalInputs ext;

    explicit Shafts(int count = 4, bool curved = true) {
        scalp = test::makePlane(8, 2.0f);
        auto s = std::make_shared<StrandSet>();
        for (int k = 0; k < count; ++k) {
            std::vector<Vec3f> pts;
            for (int i = 0; i <= 10; ++i) {
                const float t = static_cast<float>(i) / 10.0f;
                pts.push_back({0.3f + t, curved ? 0.3f * t * t : 0.0f, 0.3f + 0.45f * static_cast<float>(k)});
            }
            s->addStrand(100 + static_cast<StrandId>(k), pts, std::vector<float>(pts.size(), 0.01f),
                         scalp->closestPoint(pts[0]).root);
        }
        shafts = s;
        ext.set("scalp", scalp, 1);
    }

    StrandsPtr run(const Operator& op) const {
        EvalContext ctx;
        ctx.externals = &ext;
        return valueAs<StrandsPtr>(op.evaluate(ctx, {Value(shafts)}));
    }
};

std::unique_ptr<Operator> propagate(double shaftWidth) {
    auto op = OperatorRegistry::instance().create(op::kPropagate);
    op->params().set("count", std::int64_t{12});
    op->params().set("length", 0.3);
    op->params().set("shaftWidth", shaftWidth);
    return op;
}

// The mesh's triangles as a ScalpMesh (closest points), with each triangle's face.
struct Triangulated {
    ScalpMesh mesh;
    std::vector<int> face;
};
Triangulated triangulate(const RachisMesh& m) {
    std::vector<ScalpMesh::Triangle> tris;
    std::vector<int> face;
    std::size_t at = 0;
    for (std::size_t f = 0; f < m.faceCounts.size(); ++f) {
        const int* v = m.faceVertices.data() + at;
        for (int k = 1; k + 1 < m.faceCounts[f]; ++k) {
            tris.push_back({v[0], v[k], v[k + 1]});
            face.push_back(static_cast<int>(f));
        }
        at += static_cast<std::size_t>(m.faceCounts[f]);
    }
    return {ScalpMesh(m.points, tris), face};
}

Vec3f faceNormal(const RachisMesh& m, std::size_t firstCorner) {
    const Vec3f a = m.points[static_cast<std::size_t>(m.faceVertices[firstCorner])];
    const Vec3f b = m.points[static_cast<std::size_t>(m.faceVertices[firstCorner + 1])];
    const Vec3f c = m.points[static_cast<std::size_t>(m.faceVertices[firstCorner + 2])];
    return (b - a).cross(c - a);
}

TEST(Rachis, TubeTopologyUvsAndOutwardFaces) {
    auto layer = std::make_shared<FeatherLayer>();
    layer->sides = 6;
    for (int r = 0; r < 3; ++r) {
        RachisRing ring;
        ring.center = {static_cast<float>(r), 0.0f, 0.0f};
        ring.tangent = {1.0f, 0.0f, 0.0f};
        ring.normal = {0.0f, 1.0f, 0.0f};
        ring.side = ring.tangent.cross(ring.normal);
        ring.radius = 0.1f - 0.03f * static_cast<float>(r);
        ring.u = 0.5f * static_cast<float>(r);
        layer->rings.push_back(ring);
    }
    layer->offsets.push_back(3);
    layer->featherIds.push_back(0);
    const RachisMesh m = buildRachisMesh({layer});
    ASSERT_EQ(m.points.size(), 3u * 6u + 1u);                // a ring of 6 per shaft point, and the tip's center
    ASSERT_EQ(m.faceCount(), 2u * 6u + 6u);                   // quads between rings, a fan at the tip
    EXPECT_EQ(m.triangleCount(), 2u * 6u * 2u + 6u);
    EXPECT_EQ(m.u.size(), 3u * 7u + 1u);                      // a seam column of UVs of its own
    EXPECT_EQ(m.faceFeather, std::vector<int>(m.faceCount(), 0));
    EXPECT_EQ(m.featherCount, 1u);
    for (std::size_t p = 0; p < 18; ++p) {  // on the ring's radius
        const RachisRing& ring = layer->rings[p / 6];
        EXPECT_NEAR((m.points[p] - ring.center).length(), ring.radius, 1e-5f);
        EXPECT_EQ(m.featherIndex[p], 0.0f);
    }
    // UVs: u around, v root to tip.
    std::size_t corner = 0;
    for (std::size_t f = 0; f < m.faceCount(); ++f) {
        const int n = m.faceCounts[f];
        Vec3f centroid;
        for (int k = 0; k < n; ++k) centroid += m.points[static_cast<std::size_t>(m.faceVertices[corner + k])];
        centroid = centroid * (1.0f / static_cast<float>(n));
        const Vec3f normal = faceNormal(m, corner);
        if (n == 4) {  // a side: out of the axis
            EXPECT_GT(normal.dot(Vec3f{0.0f, centroid.y, centroid.z}), 0.0f) << "face " << f;
        } else {  // the tip: along the shaft
            EXPECT_GT(normal.x, 0.0f) << "face " << f;
        }
        for (int k = 0; k < n; ++k) {
            const auto uv = static_cast<std::size_t>(m.faceUVs[corner + k]);
            const auto point = static_cast<std::size_t>(m.faceVertices[corner + k]);
            if (point < 18) EXPECT_FLOAT_EQ(m.v[uv], layer->rings[point / 6].u);
            EXPECT_GE(m.u[uv], 0.0f);
            EXPECT_LE(m.u[uv], 1.0f);
        }
        corner += static_cast<std::size_t>(n);
    }
    // The last column of a ring's quads wraps to u 1 (the seam), not back to 0.
    EXPECT_FLOAT_EQ(m.u[static_cast<std::size_t>(m.faceUVs[5 * 4 + 2])], 1.0f);

    // A flat ribbon: two vertices a ring, one strip facing the vane's normal.
    auto ribbon = std::make_shared<FeatherLayer>(*layer);
    ribbon->sides = 2;
    const RachisMesh r = buildRachisMesh({ribbon});
    EXPECT_EQ(r.points.size(), 6u);
    ASSERT_EQ(r.faceCount(), 2u);
    EXPECT_GT(faceNormal(r, 0).y, 0.0f);
    EXPECT_NEAR((r.points[0] - r.points[1]).length(), 0.2f, 1e-5f);
}

TEST(Rachis, ToMeshMakesTheBarbsAndTheRachis) {
    // Feathers (Propagate's barbs and rachis): auto picks them; the mesh is the strands' tubes,
    // then the rachis tubes, each face named by its part.
    const Shafts f;
    auto op = propagate(0.06);
    const StrandsPtr out = f.run(*op);
    StrandMeshOptions o;
    o.sides = 3;
    EXPECT_EQ(resolveMeshMake(*out, o), StrandMeshOptions::Make::Feathers);
    const StrandMesh mesh = buildStrandMesh(*out, nullptr, o);
    StrandMeshOptions strands = o;
    strands.make = StrandMeshOptions::Make::Strands;
    const StrandMesh barbs = buildStrandMesh(*out, nullptr, strands);
    const RachisMesh rachis = buildRachisMesh(out->featherLayers());
    ASSERT_GT(rachis.faceCount(), 0u);
    EXPECT_EQ(mesh.mesh.points.size(), barbs.mesh.points.size() + rachis.points.size());
    EXPECT_EQ(mesh.mesh.faceCount(), barbs.mesh.faceCount() + rachis.faceCount());
    EXPECT_EQ(std::count(mesh.faceParts.begin(), mesh.faceParts.end(), static_cast<int>(MeshPart::Rachis)),
              static_cast<std::ptrdiff_t>(rachis.faceCount()));
    // The rachis' faces point at its own points, after the barbs'.
    const std::size_t corners = mesh.mesh.faceVertices.size() - rachis.faceVertices.size();
    EXPECT_EQ(mesh.mesh.faceVertices[corners], rachis.faceVertices[0] + static_cast<int>(barbs.mesh.points.size()));
    EXPECT_EQ(strandMeshPointCount(*out, o), mesh.mesh.points.size());
}

TEST(Rachis, PropagateMakesTubesAlongTheShaftsWithTheWidthProfile) {
    const Shafts f;
    auto op = propagate(0.06);
    op->params().set("shaftWidthProfile", Ramp{{0.0f, 1.0f}, {1.0f, 0.5f}});
    op->params().set("shaftSides", std::int64_t{5});
    const StrandsPtr out = f.run(*op);
    ASSERT_EQ(out->featherLayers().size(), 1u);
    const FeatherLayer& layer = *out->featherLayers().front();
    EXPECT_EQ(layer.sides, 5);
    EXPECT_EQ(layer.groupId, 0);
    ASSERT_EQ(layer.shaftCount(), 4u);
    // Each tube names its shaft's feather (the featherId channel: a number from the shaft's ID).
    std::vector<int> shaftFeathers;
    for (std::size_t s = 0; s < out->strandCount(); ++s) {
        if (channel(*out, "isShaft", s) > 0.5f) shaftFeathers.push_back(static_cast<int>(channel(*out, "featherId", s)));
    }
    EXPECT_EQ(layer.featherIds, shaftFeathers);
    for (std::size_t k = 0; k < layer.shaftCount(); ++k) {
        ASSERT_EQ(layer.offsets[k + 1] - layer.offsets[k], 11u);  // a ring at every shaft point
        for (std::size_t r = 0; r < 11; ++r) {
            const RachisRing& ring = layer.rings[layer.offsets[k] + r];
            EXPECT_LT((ring.center - f.shafts->strandPoints(k)[r]).length(), 1e-6f);
            EXPECT_NEAR(ring.radius, 0.03f * (1.0f - 0.5f * ring.u), 1e-5f);  // half the width, tapering
            EXPECT_NEAR(ring.tangent.dot(ring.normal), 0.0f, 1e-4f);
        }
        EXPECT_FLOAT_EQ(layer.rings[layer.offsets[k]].u, 0.0f);
        EXPECT_FLOAT_EQ(layer.rings[layer.offsets[k + 1] - 1].u, 1.0f);
    }
    const RachisMesh m = buildRachisMesh(out->featherLayers());
    EXPECT_EQ(m.points.size(), 4u * (11u * 5u + 1u));
    EXPECT_EQ(m.featherCount, 4u);
    EXPECT_EQ(std::set<int>(m.faceFeather.begin(), m.faceFeather.end()), std::set<int>(shaftFeathers.begin(), shaftFeathers.end()));

    // No width: no rachis, no layer, and the barbs root on the center line as before.
    const StrandsPtr plain = f.run(*propagate(0.0));
    EXPECT_TRUE(plain->featherLayers().empty());
    EXPECT_FALSE(hasRachis(plain->featherLayers()));
}

// A shaft kept with a rachis is as wide as it, point by point, so a renderer drawing it as a curve
// draws the rachis; a Width node after the feathers (the presets' barb width) leaves it so, and the
// barbs take its width. Without a rachis the Width node sets the shafts too, as ever.
TEST(Rachis, KeptShaftsAreAsWideAsTheirRachis) {
    const Shafts f;
    auto op = propagate(0.06);
    op->params().set("shaftWidthProfile", Ramp{{0.0f, 1.0f}, {1.0f, 0.5f}});
    const StrandsPtr out = f.run(*op);
    const FeatherLayer& layer = *out->featherLayers().front();
    auto width = OperatorRegistry::instance().create(op::kWidth);
    width->params().set("width", 0.004);
    width->params().set("profile", Ramp::constant(1.0f));
    EvalContext ctx;
    ctx.externals = &f.ext;
    const StrandsPtr widened = valueAs<StrandsPtr>(width->evaluate(ctx, {Value(out)}));
    std::size_t shafts = 0, barbs = 0;
    for (std::size_t s = 0; s < out->strandCount(); ++s) {
        const std::size_t begin = out->strandBegin(s), n = out->strandPointCount(s);
        if (channel(*out, "isShaft", s) > 0.5f) {
            EXPECT_EQ(channel(*out, kRachisChannel, s), 1.0f);
            const std::size_t first = layer.offsets[shafts++];
            for (std::size_t k = 0; k < n; ++k) {
                EXPECT_FLOAT_EQ(out->widths()[begin + k], 2.0f * layer.rings[first + k].radius) << s << ", " << k;
                EXPECT_FLOAT_EQ(widened->widths()[begin + k], out->widths()[begin + k]) << "the Width node left it";
            }
        } else {
            ++barbs;
            EXPECT_EQ(channel(*out, kRachisChannel, s), 0.0f);
            for (std::size_t k = 0; k < n; ++k) EXPECT_FLOAT_EQ(widened->widths()[begin + k], 0.004f);
        }
    }
    EXPECT_EQ(shafts, 4u);
    EXPECT_GT(barbs, 0u);
    // No rachis: no rachis channel, and the Width node sets every strand, the shafts too.
    const StrandsPtr bare = f.run(*propagate(0.0));
    EXPECT_FALSE(bare->strandChannel(kRachisChannel));
    const StrandsPtr plain = valueAs<StrandsPtr>(width->evaluate(ctx, {Value(bare)}));
    for (const float w : plain->widths()) EXPECT_FLOAT_EQ(w, 0.004f);
}

// Every barb root lies on its own feather's rachis (the closest triangle of the whole mesh is
// its feather's), off the center line by about the rachis's radius, and the barbs keep their
// designed lengths and shapes.
void expectBarbsOnTheirRachis(const char* sides, std::int64_t shaftSides, double lift) {
    SCOPED_TRACE(std::string(sides) + " sides " + std::to_string(shaftSides));
    const Shafts f;
    auto op = propagate(0.06);
    op->params().set("sides", std::string(sides));
    op->params().set("shaftSides", shaftSides);
    op->params().set("lift", lift);
    const StrandsPtr out = f.run(*op);
    op->params().set("rootOnShaft", false);
    const StrandsPtr centered = f.run(*op);
    ASSERT_EQ(out->strandCount(), centered->strandCount());
    const RachisMesh m = buildRachisMesh(out->featherLayers());
    const Triangulated tris = triangulate(m);
    std::size_t barbs = 0;
    for (std::size_t s = 0; s < out->strandCount(); ++s) {
        if (channel(*out, "isShaft", s) > 0.5f) continue;
        ++barbs;
        const Vec3f root = out->strandPoints(s)[0];
        const SurfacePoint hit = tris.mesh.closestPointBruteForce(root);
        EXPECT_LT((hit.position - root).length(), 1e-3f) << "barb " << s;  // on the surface (radius 0.03)
        const int feather = m.faceFeather[static_cast<std::size_t>(tris.face[static_cast<std::size_t>(hit.root.face)])];
        EXPECT_EQ(static_cast<float>(feather), channel(*out, "featherId", s)) << "barb " << s;
        // Off the center line, where the unmoved barb started, by up to the radius there.
        const Vec3f center = centered->strandPoints(s)[0];
        const float r = 0.03f * 1.2f;  // the ramp's widest (1 at the root) with room for the polygon
        EXPECT_GT((root - center).length(), 0.25f * 0.03f * 0.25f);
        EXPECT_LT((root - center).length(), r);
        // The same barb, moved: its length and shape as designed.
        const std::size_t n = out->strandPointCount(s);
        EXPECT_NEAR(arcLength(out->strandPoints(s), n), arcLength(centered->strandPoints(s), n), 1e-5f);
        const Vec3f shift = root - center;
        for (std::size_t j = 0; j < n; ++j) {
            EXPECT_LT((out->strandPoints(s)[j] - centered->strandPoints(s)[j] - shift).length(), 1e-5f);
        }
    }
    EXPECT_GT(barbs, 40u);
}

TEST(Rachis, BarbRootsLieOnTheirOwnRachisSurface) {
    expectBarbsOnTheirRachis("both", 6, 0.0);
    expectBarbsOnTheirRachis("both", 4, 25.0);  // lifted barbs leave the upper faces
    expectBarbsOnTheirRachis("around", 5, 0.0);
    expectBarbsOnTheirRachis("left", 2, 0.0);   // a flat ribbon: its edge
}

TEST(Rachis, GroupsNameTheFeathersAndMergeCarriesThem) {
    const Shafts f(2);
    auto contour = propagate(0.04);
    contour->params().set("groupId", std::int64_t{1});
    contour->params().set("groupName", std::string("contour"));
    auto down = propagate(0.0);  // a group without a rachis
    down->params().set("groupId", std::int64_t{2});
    down->params().set("groupName", std::string("down"));
    const StrandsPtr a = f.run(*contour);
    const StrandsPtr b = f.run(*down);
    for (std::size_t s = 0; s < a->strandCount(); ++s) EXPECT_EQ(channel(*a, "groupId", s), 1.0f);
    ASSERT_EQ(b->featherLayers().size(), 1u);
    EXPECT_FALSE(b->featherLayers().front()->hasRachis());

    // Hair (no group) merged with both designs: every layer rides to the output.
    auto hair = std::make_shared<StrandSet>();
    hair->addStrand(9, {{0.1f, 0.0f, 0.1f}, {0.1f, 0.5f, 0.1f}});
    auto merge = OperatorRegistry::instance().create(op::kMerge);
    EvalContext ctx;
    const auto ab = valueAs<StrandsPtr>(merge->evaluate(ctx, {Value(a), Value(b)}));
    const auto all = valueAs<StrandsPtr>(merge->evaluate(ctx, {Value(StrandsPtr(hair)), Value(ab)}));
    ASSERT_EQ(all->featherLayers().size(), 2u);
    EXPECT_EQ(featherGroupNames(all->featherLayers()), (std::map<int, std::string>{{1, "contour"}, {2, "down"}}));
    EXPECT_EQ(channel(*all, "groupId", 0), 0.0f);  // the hair: group 0
    EXPECT_TRUE(hasRachis(all->featherLayers()));

    const std::vector<GroomGroup> groups = splitGroomGroups(*all, kGroupIdChannel, "hair", featherGroupNames(all->featherLayers()));
    ASSERT_EQ(groups.size(), 3u);
    EXPECT_EQ(groups[0].id, 0);
    EXPECT_EQ(groups[0].name, "hair");
    EXPECT_FALSE(groups[0].feathers);
    EXPECT_EQ(groups[0].strands.strandCount(), 1u);
    EXPECT_EQ(groups[1].name, "contour");
    EXPECT_TRUE(groups[1].feathers);
    EXPECT_EQ(groups[1].strands.strandCount(), a->strandCount());
    EXPECT_EQ(groups[2].name, "down");
    EXPECT_EQ(groups[2].strands.strandCount(), b->strandCount());
    // Unnamed groups, and grooms without the channel.
    EXPECT_EQ(splitGroomGroups(*all, kGroupIdChannel, "hair", {})[1].name, "group1");
    EXPECT_EQ(splitGroomGroups(*all, "side", "hair", {}).back().name, "side1");
    const std::vector<GroomGroup> one = splitGroomGroups(*hair, kGroupIdChannel, "hair", {});
    ASSERT_EQ(one.size(), 1u);
    EXPECT_EQ(one[0].strands.strandCount(), 1u);
}

TEST(Rachis, MergedFeathersKeepTheirIndexInTheMesh) {
    // Two designs on separate shafts, merged: the mesh numbers b's feathers after a's, as Merge
    // numbers their featherId, so every barb's closest rachis triangle is still its own feather.
    const Shafts left(2);
    Shafts right(2);
    auto moved = std::make_shared<StrandSet>(*right.shafts);
    for (Vec3f& p : moved->mutablePositions()) p.z += 0.9f;
    right.shafts = moved;
    const StrandsPtr a = left.run(*propagate(0.05));
    const StrandsPtr b = right.run(*propagate(0.05));
    auto merge = OperatorRegistry::instance().create(op::kMerge);
    EvalContext ctx;
    const auto all = valueAs<StrandsPtr>(merge->evaluate(ctx, {Value(a), Value(b)}));
    const RachisMesh m = buildRachisMesh(all->featherLayers());
    EXPECT_EQ(m.featherCount, 4u);
    const Triangulated tris = triangulate(m);
    for (std::size_t s = 0; s < all->strandCount(); ++s) {
        if (channel(*all, "isShaft", s) > 0.5f) continue;
        const SurfacePoint hit = tris.mesh.closestPointBruteForce(all->strandPoints(s)[0]);
        EXPECT_EQ(static_cast<float>(m.faceFeather[static_cast<std::size_t>(tris.face[static_cast<std::size_t>(hit.root.face)])]),
                  channel(*all, "featherId", s));
    }
}

TEST(Rachis, ShaftsLeaveAndBarbsThinForEngines) {
    const Shafts f;
    auto op = propagate(0.05);
    op->params().set("count", std::int64_t{200});
    const StrandsPtr feathers = f.run(*op);
    auto groom = std::make_shared<StrandSet>(*feathers);
    groom->append(*f.shafts);  // hair beside the feathers (no featherId: -1)
    const std::size_t shafts = 4, barbs = 4 * 400, hair = 4;
    ASSERT_EQ(groom->strandCount(), shafts + barbs + hair);
    std::size_t counted = 0;
    for (std::size_t s = 0; s < groom->strandCount(); ++s) counted += isBarb(*groom, s) ? 1 : 0;
    EXPECT_EQ(counted, barbs);

    const StrandSet noShafts = withoutShafts(*groom);
    EXPECT_EQ(noShafts.strandCount(), barbs + hair);
    EXPECT_EQ(onlyShafts(*groom).strandCount(), shafts);
    EXPECT_EQ(noShafts.featherLayers().size(), 1u);  // the rachis rides along

    // A lighter groom: a quarter of the barbs, twice as wide; hair and shafts as they were (a shaft
    // as wide as its rachis: 0.05 at the root).
    const StrandSet low = thinBarbs(*groom, 0.25f, 2.0f);
    std::size_t kept = 0;
    for (std::size_t s = 0; s < low.strandCount(); ++s) {
        if (!isBarb(low, s)) continue;
        ++kept;
        EXPECT_FLOAT_EQ(low.widths()[low.strandBegin(s)], 2.0f * 0.01f);
    }
    EXPECT_GT(kept, barbs / 4 - barbs / 16);
    EXPECT_LT(kept, barbs / 4 + barbs / 16);
    EXPECT_EQ(low.strandCount() - kept, shafts + hair);
    const std::vector<float>* rachisShafts = low.strandChannel(kRachisChannel);
    ASSERT_TRUE(rachisShafts);
    for (std::size_t s = 0; s < low.strandCount(); ++s) {
        if (!isBarb(low, s)) EXPECT_FLOAT_EQ(low.widths()[low.strandBegin(s)], (*rachisShafts)[s] > 0.5f ? 0.05f : 0.01f);
    }
    // Stable: the same barbs every time, and a lower density keeps a subset.
    const StrandSet lower = thinBarbs(*groom, 0.1f, 1.0f);
    std::set<StrandId> lowIds(low.ids().begin(), low.ids().end());
    for (const StrandId id : lower.ids()) EXPECT_TRUE(lowIds.count(id));
    EXPECT_EQ(thinBarbs(*groom, 1.0f, 1.0f).strandCount(), groom->strandCount());
}

TEST(Rachis, UnityGroupsStayUnderTheStrandLimit) {
    // Unity's hair package takes a strand group per curves object: a design past the limit goes
    // in even parts, in order, the same every time; smaller groups stay whole.
    const Shafts f;
    auto op = propagate(0.05);
    op->params().set("count", std::int64_t{200});
    op->params().set("groupId", std::int64_t{1});
    op->params().set("groupName", std::string("flight"));
    const StrandsPtr feathers = f.run(*op);
    StrandSet groom = withoutShafts(*feathers);
    groom.append(*f.shafts);  // hair beside them, group 0
    const std::vector<GroomGroup> groups =
        splitGroomGroups(groom, kGroupIdChannel, "bird", featherGroupNames(feathers->featherLayers()));
    ASSERT_EQ(groups.size(), 2u);
    ASSERT_EQ(groups[1].strands.strandCount(), 1600u);
    EXPECT_EQ(kUnityMaxStrandsPerGroup, 64000u);

    const std::vector<GroomGroup> parts = limitGroupSize(groups, 500);
    ASSERT_EQ(parts.size(), 5u);  // the hair, then 1600 barbs in 4 parts of 400
    EXPECT_EQ(parts[0].name, "bird");
    EXPECT_EQ(parts[0].strands.strandCount(), 4u);
    std::vector<StrandId> ids;
    for (std::size_t p = 1; p < parts.size(); ++p) {
        EXPECT_EQ(parts[p].name, "flight_" + std::to_string(p - 1));
        EXPECT_EQ(parts[p].id, 1);
        EXPECT_TRUE(parts[p].feathers);
        EXPECT_EQ(parts[p].strands.strandCount(), 400u);
        ids.insert(ids.end(), parts[p].strands.ids().begin(), parts[p].strands.ids().end());
    }
    EXPECT_EQ(ids, groups[1].strands.ids());  // every barb once, in order
    const std::vector<GroomGroup> again = limitGroupSize(groups, 500);
    for (std::size_t p = 0; p < parts.size(); ++p) EXPECT_EQ(again[p].strands.ids(), parts[p].strands.ids());
    // Uneven: 1600 in parts of at most 700 -> 534, 533, 533.
    const std::vector<GroomGroup> uneven = limitGroupSize(groups, 700);
    ASSERT_EQ(uneven.size(), 4u);
    EXPECT_EQ(uneven[1].strands.strandCount(), 534u);
    EXPECT_EQ(uneven[3].strands.strandCount(), 533u);
    EXPECT_EQ(limitGroupSize(groups, 0).size(), 2u);     // no limit
    EXPECT_EQ(limitGroupSize(groups, 1600).size(), 2u);  // within it
    EXPECT_EQ(limitGroupSize(groups, 1600)[1].name, "flight");
}

TEST(Rachis, UnityKeepsRootWidthsAboveZero) {
    StrandSet s;
    s.addStrand(1, {{0, 0, 0}, {0, 1, 0}, {0, 2, 0}}, {0.0f, 0.03f, 0.01f});   // root 0: its widest
    s.addStrand(2, {{1, 0, 0}, {1, 1, 0}}, {0.02f, 0.0f});                      // fine as it is
    s.addStrand(3, {{2, 0, 0}, {2, 1, 0}}, {-1.0f, 0.0f});                      // none: the set's first root
    s.addStrand(4, {{3, 0, 0}, {3, 1, 0}}, {std::nanf(""), 0.004f});
    EXPECT_EQ(keepRootWidthsPositive(s, 0.5f), 3u);
    EXPECT_FLOAT_EQ(s.widths()[0], 0.03f);
    EXPECT_FLOAT_EQ(s.widths()[2], 0.01f);  // only the root changes
    EXPECT_FLOAT_EQ(s.widths()[3], 0.02f);
    EXPECT_FLOAT_EQ(s.widths()[4], 0.0f);
    EXPECT_FLOAT_EQ(s.widths()[5], 0.02f);
    EXPECT_FLOAT_EQ(s.widths()[7], 0.004f);
    EXPECT_EQ(keepRootWidthsPositive(s, 0.5f), 0u);
    StrandSet zero;
    zero.addStrand(1, {{0, 0, 0}, {0, 1, 0}}, {0.0f, 0.0f});
    EXPECT_EQ(keepRootWidthsPositive(zero, 0.5f), 1u);
    EXPECT_FLOAT_EQ(zero.widths()[0], 0.5f);  // nothing to go by: the fallback
}

}  // namespace
}  // namespace groomlab
