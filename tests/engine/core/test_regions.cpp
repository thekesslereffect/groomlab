// SPDX-License-Identifier: Apache-2.0
// Region maps: Interpolate's regionMap (each strand blends only the guides rooted in its own
// painted region) and the region channel it writes, which Clump, BlendNeighbors and Tangle keep
// within.

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <functional>
#include <map>
#include <set>
#include <string>

#include "groomlab/graph.h"
#include "groomlab/graph_io.h"
#include "groomlab/groom.h"
#include "groomlab/operator_state.h"
#include "groomlab/operators.h"
#include "test_helpers.h"

namespace groomlab {
namespace {

constexpr ScalpMesh::Color kRed{1, 0, 0, 1}, kGreen{0, 1, 0, 1}, kBlue{0, 0, 1, 1}, kYellow{1, 1, 0, 1};

// The unit plane (8 x 8 cells) with regions painted per vertex in a color set "regions": bands along
// x, red (x < 0.3), green, blue (x > 0.7), and yellow in the red band past z = 0.7. Corners blend
// across the cells between two bands: soft edges.
MeshPtr regionPlane() {
    const auto flat = test::makePlane(8, 1.0f);
    auto mesh = std::make_shared<ScalpMesh>(*flat);
    std::vector<ScalpMesh::Color> colors;
    for (const ScalpMesh::Triangle& t : flat->triangles()) {
        for (const std::int32_t v : t) {
            const Vec3f& p = flat->points()[static_cast<std::size_t>(v)];
            colors.push_back(p.x < 0.3f ? (p.z > 0.7f ? kYellow : kRed) : p.x < 0.7f ? kGreen : kBlue);
        }
    }
    mesh->setCornerColors("regions", std::move(colors));
    return mesh;
}

// Guides of each band, each band's leaning its own way: red +x, green +z, blue -x (none in yellow).
enum Band { kRedBand, kGreenBand, kBlueBand };
const Vec3f kLean[] = {{0.4f, 0.5f, 0.0f}, {0.0f, 0.5f, 0.4f}, {-0.4f, 0.5f, 0.0f}};
std::shared_ptr<GuideSet> bandGuides(const ScalpMesh& scalp, std::initializer_list<Band> bands) {
    auto guides = std::make_shared<GuideSet>();
    StrandId id = 1;
    for (const Band band : bands) {
        const std::vector<float> xs = band == kRedBand ? std::vector<float>{0.06f, 0.18f}
                                      : band == kGreenBand ? std::vector<float>{0.44f, 0.56f}
                                                           : std::vector<float>{0.82f, 0.94f};
        const std::vector<float> zs = band == kRedBand ? std::vector<float>{0.1f, 0.3f, 0.5f} : std::vector<float>{0.1f, 0.5f, 0.9f};
        for (const float x : xs) {
            for (const float z : zs) guides->add(id++ + 100 * static_cast<StrandId>(band), test::straightCurve({x, 0.0f, z}, kLean[band], 6));
        }
    }
    guides->rootOnMesh(scalp);
    return guides;
}

// Scalp and guides into Scatter and Interpolate (with regions), and Clump after it.
struct RegionGraph {
    GraphDocument doc = loadGraphJson(R"({"version": 1, "output": "interp", "nodes": [
        {"name": "scalp", "type": "Scalp"}, {"name": "guides", "type": "Guides"},
        {"name": "scatter", "type": "Scatter", "params": {"density": 2500.0}},
        {"name": "interp", "type": "Interpolate", "params": {"regionMap": "regions"}},
        {"name": "clump", "type": "Clump", "params": {"count": 40, "strength": 1.0}}],
      "connections": [{"from": "scalp", "to": "scatter", "input": "scalp"},
                      {"from": "scatter", "to": "interp", "input": "strands"},
                      {"from": "guides", "to": "interp", "input": "guides"},
                      {"from": "scalp", "to": "interp", "input": "scalp"},
                      {"from": "interp", "to": "clump", "input": "strands"}]})");
    Operator& node(const char* name) { return doc.graph.node(doc.node(name)); }
    StrandsPtr eval(const char* name, const MeshPtr& scalp, const GuidesPtr& guides) {
        ExternalInputs ext;
        setGroomExternals(ext, scalp, guides, nullptr, nullptr);
        EvalContext ctx;
        ctx.externals = &ext;
        return doc.graph.evaluateStrands(doc.node(name), ctx);
    }
};

std::vector<float> regionsOf(const StrandSet& s) {
    const std::vector<float>* r = s.strandChannel("region");
    return r ? *r : std::vector<float>{};
}

// The largest distance between strand s's points in a and b.
float strandGap(const StrandSet& a, const StrandSet& b, std::size_t s) {
    float worst = 0.0f;
    for (std::size_t i = 0; i < a.strandPointCount(s); ++i) worst = std::max(worst, (a.strandPoints(s)[i] - b.strandPoints(s)[i]).length());
    return worst;
}

// The region of the strand rooted nearest p.
float regionNear(const StrandSet& s, const Vec3f& p) {
    std::size_t best = 0;
    float best2 = 1e30f;
    for (std::size_t i = 0; i < s.strandCount(); ++i) {
        const Vec3f d = s.strandPoints(i)[0] - p;
        if (d.dot(d) < best2) {
            best2 = d.dot(d);
            best = i;
        }
    }
    return (*s.strandChannel("region"))[best];
}

TEST(Regions, EachStrandBlendsOnlyItsOwnRegionsGuides) {
    const MeshPtr scalp = regionPlane();
    const auto all = bandGuides(*scalp, {kRedBand, kGreenBand, kBlueBand});
    RegionGraph g;
    const StrandsPtr out = g.eval("interp", scalp, all);
    ASSERT_TRUE(out);
    const std::vector<float> regions = regionsOf(*out);
    ASSERT_EQ(regions.size(), out->strandCount());
    const float red = regionNear(*out, {0.1f, 0.0f, 0.2f}), green = regionNear(*out, {0.5f, 0.0f, 0.5f}),
                blue = regionNear(*out, {0.9f, 0.0f, 0.5f});
    EXPECT_EQ(std::set<float>({red, green, blue}).size(), 3u);
    for (const float r : {red, green, blue}) EXPECT_GE(r, 0.0f);

    // Every strand in its band's region; one of the two bands' in the soft cells between them;
    // none where yellow is painted (no guide is rooted there).
    std::size_t soft = 0, yellow = 0;
    for (std::size_t s = 0; s < out->strandCount(); ++s) {
        const Vec3f p = out->strandPoints(s)[0];
        const float r = regions[s];
        if (p.x < 0.25f && p.z > 0.75f) {
            EXPECT_EQ(r, -1.0f) << p.x << " " << p.z;
            ++yellow;
        } else if (p.x < 0.25f && p.z < 0.62f) {
            EXPECT_EQ(r, red) << p.x << " " << p.z;
        } else if (p.x > 0.375f && p.x < 0.625f) {
            EXPECT_EQ(r, green) << p.x << " " << p.z;
        } else if (p.x > 0.75f) {
            EXPECT_EQ(r, blue) << p.x << " " << p.z;
        } else if (p.x > 0.25f && p.x < 0.375f && p.z < 0.62f) {
            EXPECT_TRUE(r == red || r == green) << p.x << " " << p.z;
            ++soft;
        } else if (p.x > 0.625f && p.x < 0.75f) {
            EXPECT_TRUE(r == green || r == blue) << p.x << " " << p.z;
            ++soft;
        }
    }
    EXPECT_GT(soft, 100u);
    EXPECT_GT(yellow, 50u);

    // Each region's strands as if its guides were the only ones (an Interpolate without regions
    // given that band's guides alone).
    float worst = 0.0f;
    for (const auto& [band, region] : std::vector<std::pair<Band, float>>{{kRedBand, red}, {kGreenBand, green}, {kBlueBand, blue}}) {
        RegionGraph alone;
        alone.node("interp").params().set("regionMap", std::string());
        const StrandsPtr only = alone.eval("interp", scalp, bandGuides(*scalp, {band}));
        ASSERT_TRUE(only);
        ASSERT_EQ(only->ids(), out->ids());
        std::size_t compared = 0;
        for (std::size_t s = 0; s < out->strandCount(); ++s) {
            if (regions[s] != region) continue;
            worst = std::max(worst, strandGap(*out, *only, s));
            ++compared;
        }
        EXPECT_GT(compared, 300u);
    }
    EXPECT_LE(worst, 1e-6f) << "a region's strand differs from its guides' blend alone";

    // The strands in no region: as an Interpolate without regions blends them, every guide.
    RegionGraph plain;
    plain.node("interp").params().set("regionMap", std::string());
    const StrandsPtr unregioned = plain.eval("interp", scalp, all);
    float worstAny = 0.0f, mixedAway = 0.0f;
    for (std::size_t s = 0; s < out->strandCount(); ++s) {
        if (regions[s] == -1.0f) {
            worstAny = std::max(worstAny, strandGap(*out, *unregioned, s));
        } else {
            mixedAway = std::max(mixedAway, strandGap(*out, *unregioned, s));
        }
    }
    EXPECT_LE(worstAny, 1e-6f);
    EXPECT_GT(mixedAway, 0.05f);  // regions do change the strands near their edges
    EXPECT_EQ(unregioned->strandChannel("region"), nullptr);

    // A guide rooted in the soft paint between red and green (its color a blend of the two) joins
    // one of them: still three regions.
    auto edgeGuides = std::make_shared<GuideSet>(*all);
    edgeGuides->add(999, test::straightCurve({0.31f, 0.0f, 0.2f}, {0.0f, 0.5f, -0.4f}, 6));
    edgeGuides->rootOnMesh(*scalp);
    RegionGraph edged;
    const StrandsPtr withSoft = edged.eval("interp", scalp, edgeGuides);
    ASSERT_TRUE(withSoft);
    const std::vector<float> softRegions = regionsOf(*withSoft);
    std::set<float> found(softRegions.begin(), softRegions.end());
    found.erase(-1.0f);
    EXPECT_EQ(found.size(), 3u);
}

// Strands and their region channels the same, to the bit.
void expectSame(const StrandSet& a, const StrandSet& b, const std::string& what) {
    ASSERT_EQ(a.ids(), b.ids()) << what;
    EXPECT_EQ(a.positions(), b.positions()) << what;
    EXPECT_EQ(regionsOf(a), regionsOf(b)) << what;
}

TEST(Regions, ABrushDabMatchesAFullEvaluation) {
    const MeshPtr scalp = regionPlane();
    RegionGraph kept;
    std::shared_ptr<GuideSet> guides = bandGuides(*scalp, {kRedBand, kGreenBand, kBlueBand});
    ASSERT_TRUE(kept.eval("interp", scalp, guides));
    auto moved = [&](std::size_t g, const Vec3f& to) {
        auto edited = std::make_shared<GuideSet>(*guides);
        Guide& guide = edited->guides()[g];
        const Vec3f by = to - guide.points.front();
        for (Vec3f& p : guide.points) p += by;
        guide.root = scalp->closestPoint(to).root;
        return edited;
    };
    std::size_t green = 0;  // a green guide (the first)
    while (guides->guides()[green].id < 100 + 1) ++green;
    const std::vector<std::pair<std::string, std::function<std::shared_ptr<GuideSet>()>>> dabs = {
        {"reshaped", [&] {
             auto edited = std::make_shared<GuideSet>(*guides);
             for (Guide& g : edited->guides()) {
                 for (std::size_t i = 1; i < g.points.size(); ++i) g.points[i].z += 0.02f * static_cast<float>(i);
             }
             return edited;
         }},
        {"moved within its region", [&] { return moved(green, {0.48f, 0.0f, 0.3f}); }},
        {"moved into another region", [&] { return moved(green, {0.88f, 0.0f, 0.3f}); }},
        {"moved where no guide was", [&] { return moved(green, {0.1f, 0.0f, 0.9f}); }},
    };
    for (const auto& [what, dab] : dabs) {
        guides = dab();
        const std::size_t bound = interpolateBindCount();
        const StrandsPtr out = kept.eval("interp", scalp, guides);
        if (what == "reshaped") EXPECT_EQ(interpolateBindCount(), bound) << what << ": bound again";
        RegionGraph fresh;
        const StrandsPtr expected = fresh.eval("interp", scalp, guides);
        ASSERT_TRUE(out && expected) << what;
        expectSame(*out, *expected, what);
        if (what == "moved where no guide was") {  // yellow is a region now
            EXPECT_GE(regionNear(*out, {0.1f, 0.0f, 0.9f}), 0.0f);
        }
    }
}

TEST(Regions, ClumpsStayInTheirRegion) {
    const MeshPtr scalp = regionPlane();
    RegionGraph g;
    const auto guides = bandGuides(*scalp, {kRedBand, kGreenBand, kBlueBand});
    auto clumpsSpanning = [&]() {
        const StrandsPtr out = g.eval("clump", scalp, guides);
        EXPECT_TRUE(out);
        const std::vector<float> regions = regionsOf(*out);
        const std::vector<float>& clumps = *out->strandChannel("clumpId");
        std::map<float, std::set<float>> regionsOfClump;
        for (std::size_t s = 0; s < out->strandCount(); ++s) {
            if (clumps[s] >= 0.0f) regionsOfClump[clumps[s]].insert(regions[s]);
        }
        EXPECT_GT(regionsOfClump.size(), 20u);
        std::size_t spanning = 0;
        for (const auto& [clump, in] : regionsOfClump) spanning += in.size() > 1;
        return spanning;
    };
    EXPECT_EQ(clumpsSpanning(), 0u);
    g.node("clump").params().set("regionChannel", std::string());  // across regions: some do
    EXPECT_GT(clumpsSpanning(), 0u);
}

// Strands on the plane in two interleaved regions (alternate rows), region 0 leaning +x and 1 -x.
StrandsPtr interleaved(const ScalpMesh& plane) {
    auto s = std::make_shared<StrandSet>();
    StrandId id = 1;
    for (int i = 0; i < 20; ++i) {
        for (int j = 0; j < 20; ++j) {
            const SurfacePoint sp = plane.closestPoint({0.025f + 0.05f * static_cast<float>(i), 0.0f, 0.025f + 0.05f * static_cast<float>(j)});
            s->addStrand(id++, test::straightCurve(sp.position, {j % 2 ? -0.5f : 0.5f, 0.5f, 0.0f}, 8), {}, sp.root);
        }
    }
    std::vector<float>& region = s->addStrandChannel("region", 0.0f);
    for (std::size_t k = 0; k < region.size(); ++k) region[k] = static_cast<float>(k % 2);
    return s;
}

StrandsPtr runOp(const char* type, const StrandsPtr& in, const MeshPtr& scalp, const std::map<std::string, ParamValue>& params) {
    ExternalInputs ext;
    ext.set("scalp", scalp, scalp->contentHash());
    EvalContext ctx;
    ctx.externals = &ext;
    auto op = OperatorRegistry::instance().create(type);
    for (const auto& [k, v] : params) op->params().set(k, v);
    return valueAs<StrandsPtr>(op->evaluate(ctx, {in}));
}

TEST(Regions, BlendNeighborsAndTangleKeepWithinRegions) {
    const MeshPtr plane = test::makePlane(8, 1.0f);
    const StrandsPtr in = interleaved(*plane);
    for (const char* type : {op::kBlendNeighbors, op::kTangle}) {
        const std::map<std::string, ParamValue> params =
            type == std::string(op::kBlendNeighbors)
                ? std::map<std::string, ParamValue>{{"strength", 1.0}, {"neighbors", std::int64_t{6}}}
                : std::map<std::string, ParamValue>{{"fraction", 1.0}, {"strength", 1.0}, {"radius", 0.08},
                                                    {"reach", 0.0}, {"span", 1.0}, {"start", 0.0}};
        // Within regions: every strand's neighbours lean its way, so it keeps its lean (a stuck
        // strand moves by no more than the roots are apart).
        const StrandsPtr kept = runOp(type, in, plane, params);
        for (std::size_t s = 0; s < in->strandCount(); ++s) EXPECT_LT(strandGap(*kept, *in, s), 0.1f) << type << " " << s;
        // Across regions: neighbours of the other region pull strands the other way.
        auto across = params;
        across["regionChannel"] = std::string();
        const StrandsPtr mixed = runOp(type, in, plane, across);
        std::size_t pulled = 0;
        for (std::size_t s = 0; s < in->strandCount(); ++s) pulled += strandGap(*mixed, *in, s) > 0.15f;
        EXPECT_GT(pulled, in->strandCount() / 4) << type;
    }
}

TEST(Regions, AGuideMapsValuesAreRegionsEachStrandItsNearestGuides) {
    // Regions from the guides (Tubes to Curves' way): each guide's value on a guide layer is its
    // region, every distinct value its own (the green band's 0.5 lies between red's 0.2 and blue's
    // 0.8, and is still a region of its own), and each strand is in the region of the guide rooted
    // nearest it: no strand without one, and each region's strands are its guides' blend alone. By
    // name or with a Map node of the layer plugged into regionMap: the same.
    const MeshPtr scalp = test::makePlane(8, 1.0f);  // (nothing painted on the scalp)
    auto valued = bandGuides(*scalp, {kRedBand, kGreenBand, kBlueBand});
    for (Guide& g : valued->guides()) g.attributes["bands"] = g.id > 200 ? 0.8f : g.id > 100 ? 0.5f : 0.2f;
    RegionGraph byName;
    byName.node("interp").params().set("regionMap", std::string("bands"));
    const StrandsPtr out = byName.eval("interp", scalp, valued);
    ASSERT_TRUE(out);
    const std::vector<float> regions = regionsOf(*out);
    ASSERT_EQ(regions.size(), out->strandCount());
    EXPECT_EQ(std::count(regions.begin(), regions.end(), -1.0f), 0) << "every strand in a region";
    EXPECT_EQ(std::set<float>(regions.begin(), regions.end()).size(), 3u);
    // Each strand's region is its nearest guide's band.
    std::map<StrandId, float> regionOfBand;
    for (std::size_t s = 0; s < out->strandCount(); ++s) {
        const Vec3f root = out->strandPoints(s)[0];
        const Guide* nearest = nullptr;
        float best = 1e30f;
        for (const Guide& g : valued->guides()) {
            const float d = (g.points[0] - root).length();
            if (d < best) best = d, nearest = &g;
        }
        const StrandId band = nearest->id / 100;
        const auto [it, added] = regionOfBand.emplace(band, regions[s]);
        EXPECT_EQ(it->second, regions[s]) << root.x << " " << root.z;
    }
    EXPECT_EQ(regionOfBand.size(), 3u);
    // A region's strands: its guides' blend alone.
    float worst = 0.0f;
    for (const Band band : {kRedBand, kGreenBand, kBlueBand}) {
        RegionGraph alone;
        alone.node("interp").params().set("regionMap", std::string());
        const StrandsPtr only = alone.eval("interp", scalp, bandGuides(*scalp, {band}));
        ASSERT_TRUE(only);
        for (std::size_t s = 0; s < out->strandCount(); ++s) {
            if (regions[s] == regionOfBand[band]) worst = std::max(worst, strandGap(*out, *only, s));
        }
    }
    EXPECT_LE(worst, 1e-6f);

    // A Map node of the layer plugged into regionMap: the same strands and regions.
    GraphDocument plugged = loadGraphJson(R"({"version": 1, "output": "interp", "nodes": [
        {"name": "scalp", "type": "Scalp"}, {"name": "guides", "type": "Guides"},
        {"name": "scatter", "type": "Scatter", "params": {"density": 2500.0}},
        {"name": "bands", "type": "Map", "params": {"name": "bands"}},
        {"name": "interp", "type": "Interpolate", "paramInputs": ["regionMap"]}],
      "connections": [{"from": "scalp", "to": "scatter", "input": "scalp"},
                      {"from": "scatter", "to": "interp", "input": "strands"},
                      {"from": "guides", "to": "interp", "input": "guides"},
                      {"from": "scalp", "to": "interp", "input": "scalp"},
                      {"from": "bands", "to": "interp", "input": "regionMap", "output": "rgb"}]})");
    ExternalInputs ext;
    setGroomExternals(ext, scalp, valued, nullptr, nullptr);
    EvalContext ctx;
    ctx.externals = &ext;
    const StrandsPtr viaMap = plugged.graph.evaluateStrands(plugged.node("interp"), ctx);
    ASSERT_TRUE(viaMap);
    expectSame(*viaMap, *out, "a Map node plugged in");
}

}  // namespace
}  // namespace groomlab
