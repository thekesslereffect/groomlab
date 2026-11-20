// SPDX-License-Identifier: Apache-2.0
// Scale invariance: a groom looks the same whatever the scene's scale. Every shipped preset fitted
// to the reference groom (its scalp, guides and hierarchy) and to the same groom scaled by k makes
// the same strands (the same count and IDs), their points and widths k times as far and wide.

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdio>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>

#include <nlohmann/json.hpp>

#include "groomlab/graph_edit.h"
#include "groomlab/graph_io.h"
#include "groomlab/groom_file.h"
#include "groomlab/groom_session.h"
#include "groomlab/presets.h"
#include "test_helpers.h"

namespace groomlab {
namespace {

using json = nlohmann::json;

const std::string kShipped = GROOMLAB_SHIPPED_PRESETS;
constexpr float kDensity = 0.05f;  // as PresetLooks: the preview's share keeps every preset quick

std::vector<Vec3f> scaled(std::vector<Vec3f> points, float k) {
    for (Vec3f& p : points) p = p * k;
    return points;
}

MeshPtr scaledMesh(const MeshPtr& mesh, float k) {
    if (!mesh) return mesh;
    auto out = std::make_shared<ScalpMesh>(scaled(mesh->points(), k), mesh->triangles());
    if (mesh->hasUVs()) out->setCornerUVs(mesh->cornerUVs());
    for (const std::string& name : mesh->colorSetNames()) out->setCornerColors(name, *mesh->cornerColors(name));
    if (mesh->hasRest()) out->setRestPoints(scaled(mesh->restPoints(), k));
    out->setPolygons(mesh->polygons());
    out->setRenderSubdivision(mesh->renderSubdivision(), mesh->renderSubdivisionFrom());
    return out;
}

// The groom k times as big: the scalp, colliders, flat guides and the hierarchy's shapes (world
// points, rooted shapes, children's offsets along their parents and the tubes' radii).
GroomData scaledGroom(GroomData data, float k) {
    data.scalp = scaledMesh(data.scalp, k);
    for (auto& [name, mesh] : data.meshes) mesh = scaledMesh(mesh, k);
    if (data.guides) {
        auto guides = std::make_shared<GuideSet>(*data.guides);
        for (Guide& g : guides->guides()) g.points = scaled(std::move(g.points), k);
        data.guides = guides;
    }
    if (!data.hierarchyJson.empty()) {
        json h = json::parse(data.hierarchyJson);
        for (json& g : h["guides"]) {
            for (const char* key : {"points", "rootLocal"}) {
                if (!g.contains(key)) continue;
                for (json& v : g[key]) v = v.get<double>() * k;
            }
            if (g.contains("offsets")) {  // x, y in the parent's tube radii; z along it, in scene units
                json& o = g["offsets"];
                for (std::size_t i = 2; i < o.size(); i += 3) o[i] = o[i].get<double>() * k;
            }
            if (g.contains("tube") && g["tube"].contains("width")) {
                for (json& r : g["tube"]["width"]) r = r.get<double>() * k;
            }
        }
        data.hierarchyJson = h.dump();
    }
    return data;
}

struct Made {
    StrandsPtr strands;
    std::string error;
    std::vector<std::pair<std::string, StrandsPtr>> nodes;  // each node's strands, in the graph's order (with `nodes`)
};

Made make(const GroomData& groom, const std::string& preset, std::optional<double> size, const std::string& dir,
          bool nodes = false) {
    GroomData data = groom;
    data.graphJson = presetGraphJson(preset, size);
    Made made;
    try {
        GroomSession session(data);
        session.setPathResolver(searchPathResolver({dir, kShipped}));
        made.strands = session.evaluate(kDensity, 0.0);
        if (nodes) {
            const json graph = json::parse(data.graphJson);
            for (const json& n : graph["nodes"]) {
                const std::string name = n.value("name", std::string());
                try {
                    if (StrandsPtr s = valueAs<StrandsPtr>(session.evaluateNode(name, kDensity, 0.0))) made.nodes.emplace_back(name, s);
                } catch (const std::exception& e) {
                    made.error += name + ": " + e.what() + "; ";
                }
            }
        }
    } catch (const std::exception& e) {
        made.error = e.what();
    }
    return made;
}

constexpr double kTolerance = 1e-3;  // of the groom's size (0.2 mm on a head): a field's cells, never a hidden length

// How far `b` is from `a` scaled by k: the strands' count and IDs must match (else a message), the
// largest point and width difference as a share of `extent` x k (the groom's size there).
// A thousandth of the strands may lie farther than `tolerance` (`strays` of them, `farthest` the
// worst; `points` is then the largest of the rest): a strand on the border between two clumps
// joins the one its rounding at that scale puts nearer, as its clump channel below may differ.
// (Clump version 1 does it too, more rarely: agouti_fur at full density, 781,369 strands, one at x10.)
struct Difference {
    std::string mismatch;
    double points = 0.0, widths = 0.0;
    std::size_t strays = 0;
    double farthest = 0.0;
    std::string channel;  // a channel that is neither the same nor k times
};

Difference compare(const StrandSet& a, const StrandSet& b, float k, double extent, double tolerance = kTolerance) {
    Difference d;
    if (a.strandCount() != b.strandCount()) {
        std::set<StrandId> ia(a.ids().begin(), a.ids().end()), ib(b.ids().begin(), b.ids().end());
        std::size_t onlyA = 0, onlyB = 0;
        for (const StrandId id : ia) onlyA += ib.count(id) ? 0 : 1;
        for (const StrandId id : ib) onlyB += ia.count(id) ? 0 : 1;
        d.mismatch = std::to_string(a.strandCount()) + " strands vs " + std::to_string(b.strandCount()) + " (" +
                     std::to_string(onlyA) + " only at 1, " + std::to_string(onlyB) + " only at k)";
        return d;
    }
    for (std::size_t i = 0; i < a.strandCount(); ++i) {
        if (a.ids()[i] != b.ids()[i]) {
            d.mismatch = "strand " + std::to_string(i) + "'s ID differs";
            return d;
        }
        if (a.strandPointCount(i) != b.strandPointCount(i)) {
            d.mismatch = "strand " + std::to_string(i) + " has " + std::to_string(a.strandPointCount(i)) + " points vs " +
                         std::to_string(b.strandPointCount(i));
            return d;
        }
    }
    const double scale = k * extent;
    double within = 0.0;  // the largest among the strands within the tolerance
    for (std::size_t i = 0; i < a.strandCount(); ++i) {
        const Vec3f* p = a.strandPoints(i);
        const Vec3f* q = b.strandPoints(i);
        double off = 0.0;
        for (std::size_t j = 0; j < a.strandPointCount(i); ++j) off = std::max(off, (q[j] - p[j] * k).length() / scale);
        d.farthest = std::max(d.farthest, off);
        if (off < tolerance) within = std::max(within, off);
        else ++d.strays;
    }
    d.points = d.strays * 1000 > a.strandCount() ? d.farthest : within;
    // Widths of 1 everywhere are unset (no Width node: StrandSet's default, which hosts replace).
    const auto unset = [](const StrandSet& s) {
        return std::all_of(s.widths().begin(), s.widths().end(), [](float w) { return w == 1.0f; });
    };
    if (a.widths().size() == b.widths().size() && !(unset(a) && unset(b))) {
        for (std::size_t i = 0; i < a.widths().size(); ++i) {
            d.widths = std::max(d.widths, std::abs(b.widths()[i] - a.widths()[i] * k) / scale);
        }
    }
    for (const std::string& name : a.strandChannelNames()) {
        const auto* ca = a.strandChannel(name);
        const auto* cb = b.strandChannel(name);
        if (!cb || ca->size() != cb->size()) {
            d.channel = name + " (missing at k)";
            break;
        }
        std::size_t notSame = 0, notTimesK = 0;
        for (std::size_t i = 0; i < ca->size(); ++i) {
            const double x = (*ca)[i], y = (*cb)[i];
            notSame += std::abs(y - x) <= 1e-3 * std::max(1.0, std::abs(x)) ? 0 : 1;
            notTimesK += std::abs(y - x * k) <= 1e-3 * std::max(1.0, std::abs(x * k)) ? 0 : 1;
        }
        // A share of a thousandth may differ: a choice among the strands (Clump's centers, a
        // tangle's partner) that a tie decides, which the rounding at another scale breaks the other way.
        if (std::min(notSame, notTimesK) * 1000 > ca->size()) {
            d.channel = name + " (" + std::to_string(std::min(notSame, notTimesK)) + " of " + std::to_string(ca->size()) + ")";
            break;
        }
    }
    return d;
}

std::string readText(const std::string& path) {
    std::ifstream in(std::filesystem::u8path(path), std::ios::binary);
    std::stringstream text;
    text << in.rdbuf();
    return text.str();
}

}  // namespace

// Scales by powers of ten: a scene in other units (mm, cm, m) or ten times as big. The rounding
// lattices operators keep for edit stability are decimal (roundToDecadeStep), so these round
// alike; Collide's, Gravity's and Interpolate keepClearance's distance fields sit on power-of-two
// world grids, so their interpolation differs a little (well within kTolerance).
TEST(ScaleInvariance, EveryShippedPresetLooksTheSameAtEveryScale) {
    const GroomFile reference = readGroomFile(std::string(GROOMLAB_TEST_DATA) + "/strokes/strokes.groom");
    ASSERT_TRUE(reference.data.scalp);
    const double size = *presetScalpSize(*reference.data.scalp);
    int checked = 0;
    for (const PresetInfo& p : listPresets({kShipped})) {
        const std::string text = readText(p.path);
        const Made base = make(reference.data, text, size, reference.directory);
        for (const float k : {0.01f, 0.1f, 10.0f, 100.0f}) {
            const Made big = make(scaledGroom(reference.data, k), text, size * k, reference.directory);
            const std::string what = p.name + " at x" + std::to_string(k);
            if (!base.error.empty() || !big.error.empty()) {
                EXPECT_EQ(big.error, base.error) << what << ": the same error at every scale";
                continue;
            }
            ASSERT_TRUE(base.strands && big.strands) << what;
            const Difference d = compare(*base.strands, *big.strands, k, size);
            const auto fine = [&](const Difference& x) {
                return x.mismatch.empty() && x.points < kTolerance && x.widths < kTolerance && x.channel.empty();
            };
            std::string where;
            if (!fine(d)) {  // the nodes whose strands differ (in the graph's order), to say where it goes wrong
                const Made a = make(reference.data, text, size, reference.directory, true);
                const Made b = make(scaledGroom(reference.data, k), text, size * k, reference.directory, true);
                for (std::size_t i = 0; i < a.nodes.size() && i < b.nodes.size(); ++i) {
                    const Difference n = compare(*a.nodes[i].second, *b.nodes[i].second, k, size);
                    if (fine(n)) continue;
                    where += "\n  " + a.nodes[i].first + ": " +
                             (n.mismatch.empty() ? "points " + std::to_string(n.points) + " widths " + std::to_string(n.widths) +
                                                       " channel " + n.channel
                                                 : n.mismatch);
                }
            }
            EXPECT_TRUE(d.mismatch.empty()) << what << ": " << d.mismatch << where;
            EXPECT_LT(d.points, kTolerance) << what << ": points off by " << d.points << " of the groom's size" << where;
            EXPECT_LT(d.widths, kTolerance) << what << ": widths off by " << d.widths << " of the groom's size" << where;
            EXPECT_TRUE(d.channel.empty()) << what << ": channel " << d.channel << " is neither the same nor k times" << where;
            if (d.strays > 0) {
                std::printf("[ ties     ] %s: %zu of %zu strands off by more than the tolerance (the farthest %.5f)\n", what.c_str(),
                            d.strays, base.strands->strandCount(), d.farthest);
            }
            ++checked;
        }
    }
    EXPECT_GT(checked, 100);
}

// A Clump (version 2) picking its own centers places them on the scalp and relaxes them apart: the
// same scalp in other units gives the same clumps. Strands that differ from their neighbours
// (Length, Flow), so a clump whose center moved or took another axis shows. Every strand but a
// thousandth lies where it lay to a ten-thousandth of the groom's size, a tenth of what a preset
// is held to. (One center in fifty lands a step of Scatter's lattice, 1/1024 of a spacing, apart:
// the rounding of the relaxing's single-precision steps tips it; its strands lie a few
// hundred-thousandths off, about one strand in a hundred. A strand on the border of two clumps
// joins the other, a thousandth at most.)
TEST(ScaleInvariance, AClumpPickingItsOwnCentersGivesTheSameClumpsAtEveryScale) {
    const GroomFile reference = readGroomFile(std::string(GROOMLAB_TEST_DATA) + "/strokes/strokes.groom");
    ASSERT_TRUE(reference.data.scalp);
    const double size = *presetScalpSize(*reference.data.scalp);
    const std::string preset = R"({"version": 1, "size": 20.0, "nodes": [
        {"name": "scalp", "type": "Scalp"},
        {"name": "scatter", "type": "Scatter", "params": {"density": 80.0, "length": 8.0}},
        {"name": "length", "type": "Length", "params": {"fraction": 1.0, "randomness": 0.35}},
        {"name": "flow", "type": "Flow", "params": {"lift": 14.0, "bend": 0.55, "rollRandom": 8.0}},
        {"name": "clump", "type": "Clump", "params": {"clumpBy": "size", "size": 0.7, "strength": 0.8}},
        {"name": "output", "type": "Output"}],
      "connections": [{"from": "scalp", "to": "scatter", "input": "scalp"}, {"from": "scatter", "to": "length", "input": "strands"},
        {"from": "length", "to": "flow", "input": "strands"}, {"from": "flow", "to": "clump", "input": "strands"},
        {"from": "clump", "to": "output", "input": "strands"}], "output": "output"})";
    const Made base = make(reference.data, preset, size, reference.directory);
    ASSERT_TRUE(base.strands) << base.error;
    const std::size_t strands = base.strands->strandCount();
    ASSERT_GT(strands, 4000u);
    const std::vector<float>* clumps = base.strands->strandChannel("clumpId");
    ASSERT_TRUE(clumps);
    ASSERT_GT(std::set<float>(clumps->begin(), clumps->end()).size(), 1000u);
    for (const float k : {0.01f, 0.1f, 10.0f}) {
        const Made big = make(scaledGroom(reference.data, k), preset, size * k, reference.directory);
        ASSERT_TRUE(big.strands) << big.error;
        const std::string what = "at x" + std::to_string(k);
        const Difference d = compare(*base.strands, *big.strands, k, size);
        EXPECT_TRUE(d.mismatch.empty()) << what << ": " << d.mismatch;
        EXPECT_LT(d.points, kTolerance) << what << ": " << d.strays << " strands lie elsewhere, the farthest " << d.farthest;
        EXPECT_TRUE(d.channel.empty()) << what << ": " << d.channel;
        const Difference near = compare(*base.strands, *big.strands, k, size, 1e-4);
        EXPECT_LT(near.points, 1e-4) << what << ": " << near.strays << " of " << strands
                                     << " strands are more than a ten-thousandth of the groom's size off";
        const Difference fine = compare(*base.strands, *big.strands, k, size, 1e-5);
        EXPECT_LE(fine.strays * 20, strands) << what << ": " << fine.strays << " of " << strands
                                              << " strands are more than a hundred-thousandth of the groom's size off";
        std::printf("[ clumps   ] %s: %zu of %zu strands more than 1e-5 off, %zu more than 1e-3 (the farthest %.5f)\n", what.c_str(),
                    fine.strays, strands, d.strays, d.farthest);
    }
}

}  // namespace groomlab
