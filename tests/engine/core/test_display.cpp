// SPDX-License-Identifier: Apache-2.0
// Viewport geometry (display.h): the buffers a host draws, on a fixture groom with every kind of
// item (strands in each color mode, ribbons, dots, guides, tubes, selection, parts, flow, cards,
// instances, maps on the scalp) and the images hosts make textures of, against the golden record
// of what Maya's viewport built before the builders moved to core (tests/data/golden/
// display_buffers.json, recorded once the core's output equalled the old Maya code's, run side by
// side, byte for byte; checked byte for byte where the floating point is MSVC's, counts
// everywhere). And the builder: what rebuilds when.

#include <gtest/gtest.h>

#include <bitset>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <set>

#include <nlohmann/json.hpp>

#include "groomlab/display.h"
#include "groomlab/export_view.h"
#include "groomlab/feathers.h"
#include "groomlab/groom_session.h"
#include "groomlab/guide_hierarchy.h"
#include "groomlab/hair_color.h"
#include "groomlab/parts.h"
#include "groomlab/random.h"
#include "test_helpers.h"

namespace groomlab {
namespace {

namespace fs = std::filesystem;
using nlohmann::json;

const char* kGraph = R"({
  "version": 1,
  "nodes": [
    {"name": "scalp", "type": "Scalp"},
    {"name": "guides", "type": "Guides"},
    {"name": "scatter", "type": "Scatter", "params": {"density": 160.0}},
    {"name": "interpolate", "type": "Interpolate"},
    {"name": "clump", "type": "Clump", "params": {"count": 30, "strength": 0.6}},
    {"name": "width", "type": "Width"},
    {"name": "instance", "type": "Instance", "params": {"probability": 0.2, "prototype": "box", "seed": 3}},
    {"name": "cards", "type": "Cards", "params": {"segments": 4, "maxCards": 30}},
    {"name": "output", "type": "Output"}
  ],
  "connections": [
    {"from": "scalp", "to": "scatter", "input": "scalp"},
    {"from": "scatter", "to": "interpolate", "input": "strands"},
    {"from": "guides", "to": "interpolate", "input": "guides"},
    {"from": "scalp", "to": "interpolate", "input": "scalp"},
    {"from": "interpolate", "to": "clump", "input": "strands"},
    {"from": "clump", "to": "width", "input": "strands"},
    {"from": "width", "to": "instance", "input": "strands"},
    {"from": "instance", "to": "cards", "input": "strands"},
    {"from": "cards", "to": "output", "input": "strands"}
  ],
  "output": "output"
})";

// A sphere with UVs and a color set.
MeshPtr fixtureScalp() {
    const auto sphere = test::makeSphere(16, 32);
    auto mesh = std::make_shared<ScalpMesh>(sphere->points(), sphere->triangles());
    std::vector<ScalpMesh::UV> uvs;
    std::vector<ScalpMesh::Color> colors;
    for (const auto& tri : mesh->triangles()) {
        for (const std::int32_t v : tri) {
            const Vec3f& p = mesh->points()[static_cast<std::size_t>(v)];
            uvs.push_back({0.5f + 0.5f * p.x, 0.5f + 0.5f * p.z});
            colors.push_back({0.5f + 0.5f * p.x, 0.5f + 0.5f * p.y, 0.5f - 0.5f * p.z, 1.0f});
        }
    }
    mesh->setCornerUVs(std::move(uvs));
    mesh->setCornerColors("colorSet1", std::move(colors));
    return mesh;
}

std::string fixtureHierarchy(const MeshPtr& scalp) {
    GuideHierarchy h;
    const GroupId primary = h.addGroup("primary");
    TubeProfile tube;
    tube.width = {0.15f, 0.1f};
    std::vector<GuideId> ids;
    for (int i = 0; i < 8; ++i) {
        const float a = 6.2831853f * static_cast<float>(i) / 8.0f;  // (pm::: the same fixture on every platform)
        const Vec3f n = Vec3f{pm::cos(a) * 0.6f, 0.8f, pm::sin(a) * 0.6f}.normalized();
        ids.push_back(h.addGuide(primary, test::straightCurve(n, n * 0.8f + Vec3f{0.0f, 0.0f, 0.2f}, 6), tube));
    }
    ChildGenerationParams params;
    params.count = 3;
    h.generateChildren(primary, "children", params);
    h.setAttribute(ids[0], "freeze", 1.0f);
    h.setAttribute(ids[1], "len", 0.7f);
    h.setAttribute(ids[2], "len", 0.2f);
    h.evaluate(scalp.get());
    return h.toJson();
}

std::string fixtureParts(const ScalpMesh& scalp) {
    PartSet parts;
    parts.add("crown", {{0.1f, 1.0f, -0.4f}, {0.15f, 1.0f, 0.0f}, {0.2f, 0.95f, 0.4f}}, scalp);
    parts.add("side", {{-0.7f, 0.7f, -0.2f}, {-0.75f, 0.6f, 0.2f}}, scalp, 8);
    return parts.toJson();
}

std::string fixtureFlow(const ScalpMesh& scalp) {
    FlowSet flow;
    auto pin = [&](const Vec3f& at, const Vec3f& dir, int line, int seq) {
        FlowPin p = FlowSet::makePin(scalp, scalp.closestPoint(at), dir);
        p.line = line;
        p.seq = seq;
        flow.pins.push_back(p);
    };
    pin({0.0f, 0.2f, 1.0f}, {1.0f, 0.0f, 0.0f}, -1, 0);
    pin({-0.5f, 0.8f, 0.2f}, {0.0f, 0.0f, 1.0f}, 0, 0);
    pin({-0.5f, 0.75f, 0.4f}, {0.0f, 0.0f, 1.0f}, 0, 1);
    pin({-0.5f, 0.7f, 0.6f}, {0.0f, -0.3f, 1.0f}, 0, 2);
    pin({-0.5f, 0.5f, 0.8f}, {0.0f, -1.0f, 0.3f}, 0, 4);  // a gap: a run of its own
    FlowSink sink;
    sink.root = scalp.closestPoint({0.6f, 0.6f, 0.5f}).root;
    sink.angle = 70.0f;
    sink.radius = 0.4f;
    flow.sinks.push_back(sink);
    return flow.toJson();
}

// A box (per face-vertex points and normals) for the Instance node's "box".
InstancePrototype box() {
    InstancePrototype mesh;
    const Vec3f axes[3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    for (int a = 0; a < 3; ++a) {
        for (const float sign : {1.0f, -1.0f}) {
            const Vec3f n = axes[a] * sign, u = axes[(a + 1) % 3], v = axes[(a + 2) % 3];
            const auto base = static_cast<std::uint32_t>(mesh.points.size());
            for (const auto& [x, y] : {std::pair{-1.0f, -1.0f}, {1.0f, -1.0f}, {1.0f, 1.0f}, {-1.0f, 1.0f}}) {
                mesh.points.push_back(n * 0.5f + u * (0.5f * x) + v * (0.5f * y) + Vec3f{0.0f, 0.5f, 0.0f});
                mesh.normals.push_back(n);
            }
            mesh.triangles.insert(mesh.triangles.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
        }
    }
    mesh.signature = 42;
    mesh.found = true;
    return mesh;
}

struct Fixture {
    std::unique_ptr<GroomSession> session;
    ViewResult view;
    std::map<std::string, InstancePrototype> prototypes;
    std::map<GuideId, float> selection;
    std::shared_ptr<const std::vector<float>> gray, color;
};

const Fixture& fixture() {
    static const Fixture f = [] {
        Fixture out;
        const MeshPtr scalp = fixtureScalp();
        GroomData data;
        data.graphJson = kGraph;
        data.scalp = scalp;
        data.hierarchyJson = fixtureHierarchy(scalp);
        data.partsJson = fixtureParts(*scalp);
        data.flowJson = fixtureFlow(*scalp);
        out.session = std::make_unique<GroomSession>(data);
        ViewRequest request;
        request.wantCards = true;
        request.cardDensity = 0.5f;
        out.view = out.session->evaluate(request);
        out.prototypes["box"] = box();
        const auto& guides = out.view.hierarchy->guides();
        out.selection[guides[0].id] = 1.0f;
        out.selection[guides[3].id] = 0.5f;
        out.selection[guides.back().id] = 1.0f;
        std::vector<float> gray(scalp->pointCount()), color(scalp->pointCount() * 4);
        for (std::size_t v = 0; v < scalp->pointCount(); ++v) {
            const Vec3f& p = scalp->points()[v];
            gray[v] = 0.5f + 0.6f * p.y;  // past 1 at the top: clamped
            for (int c = 0; c < 4; ++c) color[v * 4 + static_cast<std::size_t>(c)] = 0.5f + 0.5f * pm::sin(3.0f * p.x + static_cast<float>(c));
        }
        out.gray = std::make_shared<const std::vector<float>>(std::move(gray));
        out.color = std::make_shared<const std::vector<float>>(std::move(color));
        return out;
    }();
    return f;
}

struct Scenario {
    std::string name;
    DisplaySettings settings;
    bool stroke = false;
    bool roots = false;
    ScalpMapView map;
    bool mapTexture = false;
    bool noPrototypes = false;  // the instances' mesh is not in the host's scene: markers
};

std::vector<Scenario> scenarios() {
    const Fixture& f = fixture();
    std::vector<Scenario> out;
    auto add = [&](const std::string& name, auto&& edit) {
        Scenario s;
        s.name = name;
        s.settings.hairColor = {0.4f, 0.3f, 0.2f};
        edit(s);
        out.push_back(std::move(s));
    };
    add("lit", [](Scenario&) {});
    add("flat", [](Scenario& s) { s.settings.lighting = false; });
    add("colorSet", [](Scenario& s) { s.settings.colorMode = kColorSetColors, s.settings.colorSource = "colorSet1"; });
    add("attribute", [](Scenario& s) { s.settings.colorMode = kAttributeColors, s.settings.colorMax = 30.0f; });
    add("random", [](Scenario& s) { s.settings.colorMode = kRandomColors; });
    add("clump", [](Scenario& s) { s.settings.colorMode = kClumpColors; });
    add("density", [](Scenario& s) { s.settings.colorMode = kDensityColors; });
    add("ribbons", [](Scenario& s) { s.settings.colorMode = kClumpColors, s.settings.ribbons = true, s.settings.ribbonWidthScale = 3.0f; });
    add("litRibbons", [](Scenario& s) { s.settings.ribbons = true, s.settings.ribbonWidthScale = 2.0f; });
    add("strokeDim", [](Scenario& s) { s.stroke = true, s.settings.sculptDisplay = 1; });
    add("strokeDimColored", [](Scenario& s) { s.stroke = true, s.settings.sculptDisplay = 1, s.settings.colorMode = kRandomColors; });
    add("strokeHidden", [](Scenario& s) { s.stroke = true, s.settings.sculptDisplay = 2, s.settings.tubes = true; });
    add("roots", [](Scenario& s) { s.roots = true; });
    add("guideLevels", [](Scenario& s) {
        s.settings.isolateGroup = "children", s.settings.tubes = true, s.settings.guidePoints = 1;
    });
    add("guideLayer", [](Scenario& s) {
        s.settings.guideColorMode = 1, s.settings.guideColorLayer = "len", s.settings.guideSmoothing = 1;
    });
    add("guideGroups", [](Scenario& s) {
        s.settings.guideGroupColors = "children:#ff8800 nope:#12 primary:#zz0000";
        s.settings.hiddenGuideGroups = "nothing, children";
    });
    add("noOverlays", [](Scenario& s) { s.settings.guides = false, s.settings.flow = false, s.settings.instances = false; });
    add("instanceMarkers", [](Scenario& s) { s.noPrototypes = true; });
    add("cards", [](Scenario& s) { s.settings.cardsView = s.settings.drawCards = true; });
    add("cardsTextured", [](Scenario& s) { s.settings.cardsView = s.settings.drawCards = s.settings.cardTextures = true; });
    add("mapGray", [&](Scenario& s) {
        s.map.name = "fur", s.map.kind = "vertices", s.map.values = f.gray, s.map.version = 7;
    });
    add("mapColorG", [&](Scenario& s) {
        s.map.name = "tint", s.map.kind = "vertices", s.map.channels = 4, s.map.channel = "g", s.map.values = f.color;
        s.map.version = 8;
    });
    add("mapColor", [&](Scenario& s) {
        s.map.name = "tint", s.map.kind = "vertices", s.map.channels = 4, s.map.values = f.color, s.map.version = 9;
    });
    add("mapTexture", [&](Scenario& s) {
        s.map.name = "mask", s.map.kind = "texture", s.map.texture = "mask.png", s.map.version = 10, s.mapTexture = true;
    });
    return out;
}

DisplayInputs inputsFor(const Scenario& s) {
    const Fixture& f = fixture();
    DisplayInputs in = DisplayInputs::fromView(f.view, s.stroke);
    in.viewedRoots = s.roots;
    in.prototypes = s.noPrototypes ? nullptr : &f.prototypes;
    in.selection = [&f] { return f.selection; };
    in.selectionHash = 77;
    in.scalpMap = s.map;
    in.scalpMapTexture = s.mapTexture;
    in.scalpMapTextureGeneration = s.mapTexture ? 1 : 0;
    return in;
}

std::string hex(std::uint64_t v) {
    char text[17];
    std::snprintf(text, sizeof text, "%016llx", static_cast<unsigned long long>(v));
    return text;
}

json record(const DisplayGeometry& g) {
    return json::array({g.vertexCount(), g.indexCount(), hex(g.digest())});
}

// Every item (and the dots and the pull) of a scenario's first update.
json recordFrame(const DisplayFrame& frame) {
    json out = json::object();
    for (const DisplayItem& item : frame.items) {
        out[item.name] = item.geometry ? record(*item.geometry) : json();
    }
    std::vector<float> dots;
    for (std::size_t i = 0; i < frame.dots.points.size(); ++i) {
        const Vec3f& p = frame.dots.points[i];
        const auto& c = frame.dots.colors[i];
        dots.insert(dots.end(), {p.x, p.y, p.z, c[0], c[1], c[2]});
    }
    out["dots"] = json::array({frame.dots.points.size(), hex(bufferDigest(dots.data(), dots.size() * sizeof(float)))});
    out["overlayPull"] = frame.overlayPull;
    return out;
}

// Compares with the golden record: counts always, digests where the floating point is the one
// recorded (MSVC's math: test::kRecordedMath).
void expectRecorded(const json& expected, const json& actual, const std::string& where) {
    if (expected.is_null() || actual.is_null()) {
        EXPECT_EQ(expected.is_null(), actual.is_null()) << where;
        return;
    }
    if (expected.is_number()) {
        EXPECT_NEAR(expected.get<double>(), actual.get<double>(), 1e-4) << where;
        return;
    }
    ASSERT_EQ(expected.size(), actual.size()) << where;
    for (std::size_t i = 0; i < expected.size(); ++i) {
        if (expected[i].is_string()) {
            if constexpr (test::kRecordedMath) EXPECT_EQ(expected[i], actual[i]) << where << ": the buffers differ from the recorded ones";
            continue;
        }
        EXPECT_EQ(expected[i], actual[i]) << where;
    }
}

void checkGolden(const std::string& name, const json& actual) {
    const fs::path file = fs::path(GROOMLAB_TEST_DATA) / "golden" / (name + ".json");
    const char* update = std::getenv("GROOMLAB_UPDATE_GOLDEN");
    if (update && std::string(update) == "1") {
        std::ofstream(file, std::ios::binary) << actual.dump(1) << "\n";
        return;
    }
    std::ifstream in(file, std::ios::binary);
    ASSERT_TRUE(in) << "no golden file " << file.generic_string() << " (run with GROOMLAB_UPDATE_GOLDEN=1)";
    const json expected = json::parse(in);
    ASSERT_EQ(expected.size(), actual.size());
    for (const auto& [scenario, items] : expected.items()) {
        ASSERT_TRUE(actual.contains(scenario)) << scenario;
        for (const auto& [item, value] : items.items()) {
            ASSERT_TRUE(actual[scenario].contains(item)) << scenario << "/" << item;
            expectRecorded(value, actual[scenario][item], scenario + "/" + item);
        }
    }
}

TEST(Display, FixtureHasEveryKindOfItem) {
    const Fixture& f = fixture();
    ASSERT_TRUE(f.view.strands);
    EXPECT_GT(f.view.strands->strandCount(), 300u);
    ASSERT_TRUE(f.view.strands->instances());
    ASSERT_TRUE(f.view.cards);
    EXPECT_FALSE(f.view.cards->cards.empty());
    ASSERT_TRUE(f.view.parts);
    ASSERT_TRUE(f.view.flow);
    EXPECT_FALSE(f.view.flow->sinkPositions().empty());
    ASSERT_TRUE(f.view.hierarchy);
    EXPECT_GT(f.view.hierarchy->guides().size(), 8u);
}

template <class T>
json recordBytes(const std::vector<T>& v) {
    return json::array({v.size(), hex(bufferDigest(v.data(), v.size() * sizeof(T)))});
}

// The images and colors a host makes textures and shader values of: the card map, a texture map
// as the scalp shows it (from the core's decode and from the file's pixels), the root-to-tip
// ramp, the lit color without a material, the overlays' colors stream.
json recordImages() {
    json out = json::object();
    const std::size_t w = 37, h = 23;
    std::vector<std::uint8_t> color(w * h * 4), coverage(w * h * 4), ao(w * h * 4);
    Rng rng(5);
    for (auto* image : {&color, &coverage, &ao}) {
        for (auto& v : *image) v = static_cast<std::uint8_t>(rng.nextU64() & 0xFF);
    }
    out["cardMap"] = recordBytes(combineCardMaps(color.data(), coverage.data(), ao.data(), w, h));
    out["cardMapNoAo"] = recordBytes(combineCardMaps(color.data(), coverage.data(), nullptr, w, h));
    Texture::Image image;
    image.width = static_cast<int>(w);
    image.height = static_cast<int>(h);
    image.bits = 8;
    for (std::size_t i = 0; i < w * h * 4; ++i) image.rgba.push_back(static_cast<float>(color[i]) / 255.0f);
    for (const int channels : {1, 4}) {
        for (const std::string channel : {"rgb", "r", "g", "b", "a"}) {
            const std::string name = "map" + std::to_string(channels) + channel;
            out[name + "Cached"] = recordBytes(scalpMapCachedPixels(image, channels, scalpMapPick(channel)));
            std::vector<std::uint8_t> pixels = color;
            scalpMapFilePixels(pixels.data(), w * h, channels, scalpMapPick(channel));
            out[name + "File"] = recordBytes(pixels);
        }
    }
    out["rootToTip"] = recordBytes(rootToTipRamp(32));
    StrandSet strands = *fixture().view.strands;
    StrandChannelMeans means;
    const std::array<float, 3> display{0.1f, 0.2f, 0.3f};
    std::vector<float> looks;
    auto look = [&](std::uint64_t version) {
        const std::array<float, 3> c = strandsLookColor(&strands, version, display, means);
        looks.insert(looks.end(), c.begin(), c.end());
    };
    look(1);
    strands.addStrandChannel("melanin", 0.4f);
    look(2);
    for (const char* c : {"colorR", "colorG", "colorB"}) strands.addStrandChannel(c, 0.3f);
    look(3);
    out["lookColors"] = recordBytes(looks);
    // The guides' colors stream with pull limits against the scalp, from a camera behind the head.
    DisplayBuilder builder;
    const DisplayGeometry& guides = *builder.update(inputsFor(scenarios()[0]), scenarios()[0].settings).item(DisplayItemId::Guides).geometry;
    const std::vector<float> limits =
        overlayPullLimits(guides, {0.3f, 0.5f, -4.0f}, builder.frame().overlayPull, {fixture().view.scalp});
    std::vector<float> rgba(guides.vertexCount() * 4);
    writeOverlayColors(guides, limits, rgba.data());
    out["overlayColors"] = recordBytes(rgba);
    return out;
}

TEST(Display, BuffersAsRecorded) {
    json actual = json::object();
    for (const Scenario& s : scenarios()) {
        DisplayBuilder builder;
        actual[s.name] = recordFrame(builder.update(inputsFor(s), s.settings));
        for (const DisplayItem& item : builder.frame().items) {
            ASSERT_TRUE(item.rebuilt) << s.name << " " << item.name;  // the first update builds everything
        }
    }
    actual["images"] = recordImages();
    checkGolden("display_buffers", actual);
}

// A host that writes the strand lines straight into its buffers (setWriteLines) gets the very
// bytes the vectors hold: each item's streams written from its StrandLines hash as the
// recorded ones, and the vectors stay empty.
TEST(Display, WrittenLinesAreTheBuffers) {
    std::size_t written = 0;
    for (const Scenario& s : scenarios()) {
        DisplayBuilder vectors, writer;
        writer.setWriteLines(true);
        const DisplayFrame& a = vectors.update(inputsFor(s), s.settings);
        const DisplayFrame& b = writer.update(inputsFor(s), s.settings);
        for (std::size_t i = 0; i < kDisplayItemCount; ++i) {
            const DisplayGeometry& want = *a.items[i].geometry;
            const DisplayGeometry& got = *b.items[i].geometry;
            const std::string where = s.name + "/" + a.items[i].name;
            EXPECT_EQ(want.digest(), got.digest()) << where;
            EXPECT_EQ(want.vertexCount(), got.vertexCount()) << where;
            EXPECT_EQ(want.indexCount(), got.indexCount()) << where;
            EXPECT_EQ(want.lit(), got.lit()) << where;
            EXPECT_EQ(want.empty(), got.empty()) << where;
            EXPECT_EQ(a.items[i].shown, b.items[i].shown) << where;
            EXPECT_EQ(a.items[i].vertexCount, b.items[i].vertexCount) << where;
            if (!got.lines) continue;
            ++written;
            EXPECT_TRUE(got.positions.empty() && got.indices.empty() && got.normals.empty() && got.colors.empty()) << where;
            // Each stream on its own, as a host writes it.
            const StrandLines& lines = *got.lines;
            std::vector<float> positions(lines.vertexCount() * 3);
            lines.writePositions(positions.data());
            EXPECT_EQ(positions, want.positions) << where;
            std::vector<std::uint32_t> indices(lines.indexCount());
            lines.writeIndices(indices.data());
            EXPECT_EQ(indices, want.indices) << where;
            if (lines.lit) {
                std::vector<float> normals(positions.size()), tangents(positions.size()), uvs(lines.vertexCount());
                lines.writeFrames(normals.data(), tangents.data());
                lines.writeUvs(uvs.data());
                EXPECT_EQ(want.uvFloats, 1u) << where;
                EXPECT_TRUE(want.bitangents.empty()) << where;
                EXPECT_EQ(normals, want.normals) << where;
                EXPECT_EQ(tangents, want.tangents) << where;
                EXPECT_EQ(uvs, want.uvs) << where;
            }
            if (lines.colors) {
                std::vector<float> rgba(lines.vertexCount() * 4);
                lines.writeColors(rgba.data());
                EXPECT_EQ(rgba, want.colors) << where;
            }
        }
    }
    EXPECT_GE(written, 3u);  // lit, flat (dimmed) and colored lines among the scenarios
}

// The indices and uvs of strand lines depend on their point counts alone (layoutKey): equal
// for new positions on the same strands, not for other counts.
TEST(Display, LinesLayoutKeyFollowsThePointCounts) {
    const StrandsPtr strands = fixture().view.strands;
    auto moved = std::make_shared<StrandSet>(*strands);
    for (std::size_t i = 0; i < moved->strandCount(); ++i) moved->strandPoints(i)[1].x += 0.1f;
    const StrandLines a(strands, nullptr, true, nullptr), b(moved, nullptr, true, nullptr);
    EXPECT_EQ(a.layoutKey(), b.layoutKey());
    std::vector<std::size_t> some;
    for (std::size_t i = 0; i + 1 < strands->strandCount(); ++i) some.push_back(i);
    const StrandLines c(std::make_shared<StrandSet>(strands->subset(some)), nullptr, true, nullptr);
    EXPECT_NE(a.layoutKey(), c.layoutKey());
    EXPECT_EQ(a.indexCount(), c.indexCount() + 2 * (strands->strandPointCount(strands->strandCount() - 1) - 1));
}

// The lines' uvs of two floats a point carry each point's width (the coverage's), kNoLineWidth
// for strands without widths, and u with the strand's number (lineIdOf: its ID's); whole or
// strand by strand alike; widthsKey follows the widths and the IDs alone.
TEST(Display, LinesUvsCarryTheWidths) {
    const StrandsPtr strands = fixture().view.strands;
    ASSERT_EQ(strands->widths().size(), strands->pointCount());
    const StrandLines lines(strands, nullptr, false, nullptr);
    const std::size_t n = lines.vertexCount();
    std::vector<float> whole(n * 2), parts(n * 2, -9.0f), u(n);
    lines.writeUvs(whole.data(), 2);
    lines.writeUvs(u.data());
    std::set<std::uint32_t> numbers;
    for (std::size_t s = 0; s < strands->strandCount(); ++s) {
        for (std::size_t p = strands->strandBegin(s); p < strands->strandBegin(s + 1); ++p) {
            EXPECT_NEAR(lineUvU(whole[p * 2]), u[p], 2e-4f);
            EXPECT_EQ(lineUvId(whole[p * 2]), lineIdOf(strands->ids()[s]));
            EXPECT_EQ(whole[p * 2 + 1], strands->widths()[p]);
        }
        numbers.insert(lineIdOf(strands->ids()[s]));
    }
    EXPECT_GT(numbers.size(), std::min<std::size_t>(strands->strandCount(), kLineIdCount) / 2);  // spread
    EXPECT_EQ(lineUvU(1.0f + 2.0f * 1023.0f), 1.0f);  // a tip: u 1 stays the strand's
    EXPECT_EQ(lineUvId(1.0f + 2.0f * 1023.0f), 1023u);
    // Other IDs, the same strands otherwise: another key (the numbers changed).
    auto renumbered = std::make_shared<StrandSet>(*strands);
    for (StrandId& id : renumbered->mutableIds()) id += 7;
    EXPECT_NE(StrandLines(renumbered, nullptr, false, nullptr).widthsKey(), lines.widthsKey());
    const std::size_t half = strands->strandCount() / 2;
    lines.writeUvs(parts.data(), 2, 0, half);
    lines.writeUvs(parts.data() + strands->strandBegin(half) * 2, 2, half, strands->strandCount());
    EXPECT_EQ(parts, whole);
    // Moved points keep the key; other widths change it; none: 0, the uvs kNoLineWidth.
    auto moved = std::make_shared<StrandSet>(*strands);
    moved->strandPoints(0)[1].x += 0.1f;
    EXPECT_EQ(StrandLines(moved, nullptr, false, nullptr).widthsKey(), lines.widthsKey());
    auto thinner = std::make_shared<StrandSet>(*strands);
    thinner->mutableWidths()[3] *= 0.5f;
    EXPECT_NE(StrandLines(thinner, nullptr, false, nullptr).widthsKey(), lines.widthsKey());
    EXPECT_NE(lines.widthsKey(), 0u);
    auto none = std::make_shared<StrandSet>(*strands);
    none->mutableWidths().clear();
    const StrandLines bare(none, nullptr, false, nullptr);
    EXPECT_EQ(bare.widthsKey(), 0u);
    bare.writeUvs(parts.data(), 2);
    for (std::size_t p = 0; p < n; ++p) EXPECT_EQ(parts[p * 2 + 1], kNoLineWidth);
    // idsKey: the uvs' first float's (the counts and IDs), whatever the widths and points.
    EXPECT_EQ(StrandLines(thinner, nullptr, false, nullptr).idsKey(), lines.idsKey());
    EXPECT_EQ(StrandLines(moved, nullptr, false, nullptr).idsKey(), lines.idsKey());
    EXPECT_EQ(bare.idsKey(), lines.idsKey());
    EXPECT_NE(StrandLines(renumbered, nullptr, false, nullptr).idsKey(), lines.idsKey());
    EXPECT_NE(lines.idsKey(), 0u);
}

// The root normals a host computing the frames on the GPU sends (writeRootNormals) are those the
// CPU's frames lean by: a lone point's tangent is its root normal; equal strands, equal keys.
TEST(Display, LinesRootNormalsAreTheFramesUp) {
    const auto scalp = groomlab::test::makeSphere(8, 16);
    auto strands = std::make_shared<StrandSet>();
    StrandRoot root;
    root.face = 5;
    root.u = root.v = 0.25f;
    strands->addStrand(1, {scalp->position(root)}, {}, root);  // one point: t = up
    strands->addStrand(2, {Vec3f{0, 0, 0}}, {}, StrandRoot{});  // unrooted: up
    strands->addStrand(3, {Vec3f{0, 0, 0}, Vec3f{0, 1, 0}, Vec3f{0, 2, 0.5f}}, {}, root);
    const StrandLines lines(strands, scalp, true, nullptr);
    std::vector<float> up(12), normals(15), tangents(15);
    const std::uint64_t key = lines.writeRootNormals(up.data());
    lines.writeFrames(normals.data(), tangents.data());
    const Vec3f n = scalp->normal(root);
    EXPECT_EQ((std::vector<float>{up[0], up[1], up[2], up[3]}), (std::vector<float>{n.x, n.y, n.z, 0.0f}));
    EXPECT_EQ((std::vector<float>{tangents[0], tangents[1], tangents[2]}), (std::vector<float>{n.x, n.y, n.z}));
    EXPECT_EQ((std::vector<float>{up[4], up[5], up[6]}), (std::vector<float>{0.0f, 1.0f, 0.0f}));
    std::vector<float> again(12);
    EXPECT_EQ(StrandLines(std::make_shared<StrandSet>(*strands), scalp, true, nullptr).writeRootNormals(again.data()), key);
    EXPECT_EQ(again, up);
    EXPECT_NE(key, 0u);
}

// The color modes' colors (one a strand) carry a key: equal for the same colors (the strands
// moved), another for other colors.
TEST(Display, ColoredLinesKeyTheirColors) {
    const StrandsPtr strands = fixture().view.strands;
    DisplaySettings s;
    s.colorMode = kRandomColors;
    DisplayBuilder builder;
    builder.setWriteLines(true);
    DisplayInputs in;
    in.strands = strands;
    in.strandsVersion = 1;
    const DisplayFrame& first = builder.update(in, s);
    const std::shared_ptr<const DisplayGeometry> a = first.item(DisplayItemId::ColoredStrands).geometry;
    ASSERT_TRUE(a && a->lines && a->lines->colors);
    const std::uint64_t key = a->lines->colors->key;
    EXPECT_NE(key, 0u);
    auto moved = std::make_shared<StrandSet>(*strands);
    moved->strandPoints(0)[1].x += 0.1f;
    in.strands = moved;
    in.strandsVersion = 2;
    const std::shared_ptr<const DisplayGeometry> b = builder.update(in, s).item(DisplayItemId::ColoredStrands).geometry;
    ASSERT_TRUE(b && b->lines && b->lines->colors);
    EXPECT_EQ(b->lines->colors->key, key);
    s.colorMode = kClumpColors;
    in.strandsVersion = 3;
    const std::shared_ptr<const DisplayGeometry> c = builder.update(in, s).item(DisplayItemId::ColoredStrands).geometry;
    if (c && c->lines && c->lines->colors && c->lines->colors->colors != b->lines->colors->colors) {
        EXPECT_NE(c->lines->colors->key, key);
    }
}

// Width as coverage: a pixel's share is the width projected to pixels (perspective: by the view
// depth; orthographic: clip w 1), clamped to [floor, 1]; no width (or behind the eye) is opaque.
TEST(Display, LineCoverageIsTheWidthInPixels) {
    // A 54.43 degree vertical field of view on a 1000-pixel-high viewport: P11 = 1 / tan(27.215).
    const float p11 = 1.0f / std::tan(27.215f * 3.14159265f / 180.0f);
    const float scale = linePixelScale(p11, 1000.0f);
    EXPECT_NEAR(scale, 0.5f * 1000.0f * p11, 1e-3f);
    // 1000 px / (2 tan(fov/2) d) per unit: at d = 100, a 0.1 wide strand is 0.97 px.
    const float expected = 0.1f * 1000.0f / (2.0f * std::tan(27.215f * 3.14159265f / 180.0f) * 100.0f);
    EXPECT_NEAR(lineCoverage(0.1f, 1.0f, scale, 100.0f), expected, 1e-4f);
    EXPECT_NEAR(lineCoverage(0.05f, 2.0f, scale, 100.0f), expected, 1e-4f);  // the scale multiplies
    EXPECT_NEAR(lineCoverage(0.1f, 1.0f, scale, 200.0f), expected * 0.5f, 1e-4f);  // twice as far: half
    EXPECT_EQ(lineCoverage(1.0f, 1.0f, scale, 100.0f), 1.0f);  // wider than a pixel: solid
    EXPECT_EQ(lineCoverage(0.0f, 1.0f, scale, 100.0f), kLineCoverageFloor);  // a tip of width 0
    EXPECT_EQ(lineCoverage(kNoLineWidth, 1.0f, scale, 100.0f), 1.0f);
    EXPECT_EQ(lineCoverage(0.01f, 1.0f, scale, 0.0f), 1.0f);
    EXPECT_EQ(lineCoverage(0.0f, 1.0f, scale, 1.0f, 0.25f), 0.25f);
    // Orthographic, 20 units high: P11 = 2 / 20; 50 px a unit on 1000 px, at any depth (w 1).
    EXPECT_NEAR(lineCoverage(0.01f, 1.0f, linePixelScale(0.1f, 1000.0f), 1.0f), 0.5f, 1e-5f);
}

// A thinned preview's strand covers what the 1 / density strands it stands for would together:
// 1 - (1 - c)^(1 / density), never more than the pixel; the coverage itself at density 1.
TEST(Display, LineCoverageAtDensityStandsForTheThinnedStrands) {
    EXPECT_EQ(lineCoverageAtDensity(0.3f, 1.0f), 0.3f);
    EXPECT_EQ(lineCoverageAtDensity(0.3f, 0.0f), 0.3f);  // unknown: as it is
    EXPECT_NEAR(lineCoverageAtDensity(0.05f, 0.1f), 1.0f - std::pow(0.95f, 10.0f), 1e-6f);
    EXPECT_NEAR(lineCoverageAtDensity(0.2f, 0.5f), 1.0f - 0.8f * 0.8f, 1e-6f);
    EXPECT_EQ(lineCoverageAtDensity(1.0f, 0.1f), 1.0f);
    float last = 0.0f;
    for (float c = 0.05f; c <= 1.0f; c += 0.05f) {
        for (float d : {0.01f, 0.1f, 0.35f}) {
            const float k = lineCoverageAtDensity(c, d);
            EXPECT_GE(k, c);
            EXPECT_LE(k, 1.0f);
        }
        EXPECT_GE(lineCoverageAtDensity(c, 0.1f), last);  // more coverage, more of it
        last = lineCoverageAtDensity(c, 0.1f);
    }
}

// The hashed coverage: thresholds uniform over strands and stretches, the same for the same
// fragment, a pixel's strands kept on their own (n of them cover 1 - (1 - c)^n of it, alone or
// over a multisampled pixel's samples), few kept fragments changing as the view zooms a little.
TEST(Display, LineHashedCoverageAccumulatesAndHolds) {
    // Uniform: ten bins over 40000 fragments (strands x stretches, multisampled), each within 5% of a tenth.
    std::array<int, 10> bins{};
    const int count = 40000;
    for (int i = 0; i < count; ++i) {
        const float t = lineHashThreshold(static_cast<std::uint32_t>(i % kLineIdCount), (static_cast<float>(i / kLineIdCount) + 0.5f) / 64.0f,
                                          1.0f / 90.0f, 8);  // between two powers of two: a blend
        ASSERT_GT(t, 0.0f);
        ASSERT_LE(t, 1.0f);
        ++bins[std::min(9, static_cast<int>(t * 10.0f))];
    }
    for (int b : bins) EXPECT_NEAR(b, count / 10, count / 200) << "a bin of the thresholds";
    // The same fragment, the same threshold; a stretch's fragments share theirs.
    EXPECT_EQ(lineHashThreshold(17, 0.4f, 1.0f / 64.0f, 8), lineHashThreshold(17, 0.4f, 1.0f / 64.0f, 8));
    EXPECT_EQ(lineHashThreshold(17, 0.4f, 1.0f / 64.0f, 8), lineHashThreshold(17, 0.4f + 0.2f / 64.0f, 1.0f / 64.0f, 8));
    EXPECT_NE(lineHashThreshold(17, 0.4f, 1.0f / 64.0f, 8), lineHashThreshold(18, 0.4f, 1.0f / 64.0f, 8));
    // A pixel crossed by n strands (their numbers and places along them at random) of coverage c:
    // its covered share over many such pixels, kept or not (one sample) or by 8 samples.
    std::uint64_t seed = 1;
    auto next = [&seed]() {
        seed = seed * 6364136223846793005ull + 1442695040888963407ull;
        return static_cast<float>(seed >> 40) / static_cast<float>(1 << 24);
    };
    for (const auto& [n, c] : {std::pair<int, float>{1, 0.3f}, {8, 0.1f}, {30, 0.05f}, {60, 0.12f}}) {
        double single = 0.0, sampled = 0.0;
        const int pixels = 4000;
        for (int p = 0; p < pixels; ++p) {
            bool any = false;
            std::uint32_t mask = 0;
            for (int s = 0; s < n; ++s) {
                const auto strand = static_cast<std::uint32_t>(next() * kLineIdCount);
                const float u = next(), uPerPixel = 1.0f / (40.0f + 100.0f * next());
                any = any || lineSampleMask(c, lineHashThreshold(strand, u, uPerPixel, 1), strand, 1) != 0u;
                mask |= lineSampleMask(c, lineHashThreshold(strand, u, uPerPixel, 8), strand, 8);
            }
            single += any ? 1.0 : 0.0;
            sampled += std::bitset<32>(mask).count() / 8.0;
        }
        const double expected = 1.0 - std::pow(1.0 - c, n);
        EXPECT_NEAR(single / pixels, expected, 0.03) << n << " strands of coverage " << c << ", one sample";
        EXPECT_NEAR(sampled / pixels, expected, 0.03) << n << " strands of coverage " << c << ", 8 samples";
    }
    // A strand's samples: coverage x samples of them on average, every one at coverage 1, none
    // below its threshold with one sample.
    double samples = 0.0;
    for (int i = 0; i < 4000; ++i) {
        const auto strand = static_cast<std::uint32_t>(i % kLineIdCount);
        samples += std::bitset<32>(lineSampleMask(0.3f, lineHashThreshold(strand, next(), 0.01f, 8), strand, 8)).count();
    }
    EXPECT_NEAR(samples / 4000.0, 0.3 * 8.0, 0.1);
    EXPECT_EQ(lineSampleMask(1.0f, 1.0f, 5, 8), 0xFFu);
    EXPECT_EQ(lineSampleMask(1.0f, 1.0f, 5, 1), 1u);
    EXPECT_EQ(lineSampleMask(0.2f, 0.3f, 5, 1), 0u);
    EXPECT_EQ(lineSampleMask(0.31f, 0.3f, 5, 1), 1u);
    // One sample a pixel: the coverage itself kept (hashed, opaque) up to kLineHashedBelow, every
    // fragment kept (blended at its coverage) from kLineBlendedAbove, smoothly between; the share
    // times the alpha it is drawn with (coverage / share) is the coverage.
    EXPECT_EQ(lineKeepShare(0.05f, 0.05f), 0.05f);
    EXPECT_EQ(lineKeepShare(kLineHashedBelow, kLineHashedBelow), kLineHashedBelow);
    EXPECT_EQ(lineKeepShare(kLineBlendedAbove, kLineBlendedAbove), 1.0f);
    EXPECT_EQ(lineKeepShare(1.0f, 1.0f), 1.0f);
    float share = 0.0f;
    for (float c = 0.01f; c <= 1.0f; c += 0.01f) {
        EXPECT_GE(lineKeepShare(c, c), c);
        EXPECT_GE(lineKeepShare(c, c), share);  // more coverage, more kept
        share = lineKeepShare(c, c);
        EXPECT_NEAR(share * (c / share), c, 1e-6f);
    }
    // A thinned preview from afar: its strands' own coverage low, the density's taking it up: hashed.
    const float thinned = lineCoverageAtDensity(0.05f, 0.25f);
    EXPECT_GT(thinned, kLineHashedBelow);
    EXPECT_EQ(lineKeepShare(thinned, 0.05f), thinned);
    // Zooming in by 1%: few of the kept fragments of coverage 0.5 change (the pattern holds).
    int flipped = 0;
    for (int i = 0; i < count; ++i) {
        const auto strand = static_cast<std::uint32_t>(i % kLineIdCount);
        const float u = (static_cast<float>(i / kLineIdCount) + 0.5f) / 40.0f;
        const bool before = lineHashThreshold(strand, u, 1.0f / 100.0f, 8) <= 0.5f;
        const bool after = lineHashThreshold(strand, u, 1.0f / 101.0f, 8) <= 0.5f;
        flipped += before != after ? 1 : 0;
    }
    EXPECT_LT(flipped, count / 50);
}

// One sample a pixel: stretches of about four pixels (short dashes, not scattered dots; not whole
// strands, which strands sharing a number would keep or drop together): the threshold a
// multisampled target takes at a quarter of the pixels a unit of u spans, the same over a
// stretch's four pixels, changing from stretch to stretch.
TEST(Display, LineHashThresholdStretchesFourPixelsOnOneSample) {
    for (std::uint32_t strand : {0u, 5u, 17u, 1023u}) {
        for (float u : {0.1f, 0.37f, 0.9f}) {
            for (float uPerPixel : {1.0f / 7.0f, 1.0f / 90.0f, 1.0f / 3000.0f}) {
                EXPECT_NEAR(lineHashThreshold(strand, u, uPerPixel, 1), lineHashThreshold(strand, u, 4.0f * uPerPixel, 8), 1e-5f) << strand;
                EXPECT_EQ(lineHashThreshold(strand, u, uPerPixel, 0), lineHashThreshold(strand, u, uPerPixel, 1));  // (0: one too)
            }
        }
    }
    // At 64 pixels a unit of u (a power of two: no blend), 16 stretches of four pixels.
    int differ = 0;
    for (int i = 0; i < 64; ++i) {
        const float u = (static_cast<float>(i) + 0.5f) / 64.0f;
        const float t = lineHashThreshold(17, u, 1.0f / 64.0f, 1);
        if (i % 4 != 0) EXPECT_EQ(t, lineHashThreshold(17, (static_cast<float>(i - i % 4) + 0.5f) / 64.0f, 1.0f / 64.0f, 1)) << i;
        else if (i > 0) differ += t != lineHashThreshold(17, (static_cast<float>(i - 4) + 0.5f) / 64.0f, 1.0f / 64.0f, 1) ? 1 : 0;
    }
    EXPECT_GE(differ, 12);
}

std::set<std::string> rebuiltItems(const DisplayFrame& frame) {
    std::set<std::string> out;
    for (const DisplayItem& item : frame.items) {
        if (item.rebuilt) out.insert(item.name);
    }
    return out;
}

TEST(Display, BuildsAgainOnlyWhatChanged) {
    const Fixture& f = fixture();
    Scenario s = scenarios()[0];
    DisplayInputs in = inputsFor(s);
    int selections = 0;
    in.selection = [&] {
        ++selections;
        return f.selection;
    };
    DisplayBuilder builder;
    const DisplayFrame& first = builder.update(in, s.settings);
    EXPECT_EQ(selections, 1);
    const std::uint64_t strandsVersion = first.item(DisplayItemId::Strands).geometryVersion;
    // Nothing changed: nothing built, the geometry kept (retain), only styles move.
    s.settings.hairColor = {0.9f, 0.1f, 0.1f};
    const DisplayFrame& same = builder.update(in, s.settings);
    EXPECT_TRUE(rebuiltItems(same).empty());
    EXPECT_EQ(selections, 1);  // not asked again
    EXPECT_EQ(same.item(DisplayItemId::Strands).geometryVersion, strandsVersion);
    ASSERT_TRUE(same.item(DisplayItemId::Strands).geometry);
    EXPECT_EQ(same.item(DisplayItemId::Strands).color, (std::array<float, 4>{0.9f, 0.1f, 0.1f, 1.0f}));
    EXPECT_FLOAT_EQ(same.item(DisplayItemId::DimStrands).color[0], 0.9f * 0.35f);
    // Guides changed (a brush stamp): the guides and the selection, not the strands or the overlays.
    ++in.hierarchyVersion;
    EXPECT_EQ(rebuiltItems(builder.update(in, s.settings)),
              (std::set<std::string>{"guides", "tubes", "selectedGuides", "softSelectedGuides"}));
    // The selection alone.
    ++in.selectionHash;
    EXPECT_EQ(rebuiltItems(builder.update(in, s.settings)), (std::set<std::string>{"selectedGuides", "softSelectedGuides"}));
    // A theme change rebuilds the guides; its colors restyle the rest.
    s.settings.theme.version++;
    s.settings.theme.parts = {0.0f, 1.0f, 0.0f, 1.0f};
    const DisplayFrame& themed = builder.update(in, s.settings);
    EXPECT_EQ(rebuiltItems(themed), (std::set<std::string>{"guides", "tubes"}));
    EXPECT_EQ(themed.item(DisplayItemId::Parts).color, s.settings.theme.parts);
    // The host lost an item: its part builds again.
    builder.invalidate(DisplayPart::Flow);
    EXPECT_EQ(rebuiltItems(builder.update(in, s.settings)), (std::set<std::string>{"flowArrows", "flowField"}));
    // New strands: the strands and the instances (and the overlay pull).
    ++in.strandsVersion;
    EXPECT_EQ(rebuiltItems(builder.update(in, s.settings)),
              (std::set<std::string>{"dimStrands", "strands", "coloredStrands", "ribbons", "instanceMarkers", "instanceMeshes",
                                      "scales"}));
    // New strands holding the very same points (a Width or HairColor edit: StrandChanges::pointsKept):
    // the strands, the instances kept.
    ASSERT_NE(in.strandsSerial, 0u);
    StrandChanges kept;
    kept.pointsKept = true;
    in.changesBase = in.strandsSerial;
    ++in.strandsSerial;
    in.strandsChanges = std::make_shared<const StrandChanges>(kept);
    ++in.strandsVersion;
    EXPECT_EQ(rebuiltItems(builder.update(in, s.settings)), (std::set<std::string>{"dimStrands", "strands", "coloredStrands", "ribbons"}));
}

TEST(Display, HiddenHairBuildsNoHairItemsAndKeepsTheGuides) {
    // DisplaySettings::hair off (the hosts' hair eye): none of the hair's items (isHairItem) shows,
    // in any view of it; the guides, their dots, the selection, the parts and the flow do, and are
    // not built again for it. On again: the hair is back.
    for (const char* name : {"lit", "ribbons", "roots", "cards"}) {
        Scenario s;
        for (const Scenario& each : scenarios()) {
            if (each.name == name) s = each;
        }
        ASSERT_EQ(s.name, name);
        DisplayInputs in = inputsFor(s);
        DisplayBuilder builder;
        const DisplayFrame& shown = builder.update(in, s.settings);
        bool anyHair = false;
        for (const DisplayItem& item : shown.items) anyHair = anyHair || (isHairItem(item.id) && item.shown);
        EXPECT_TRUE(anyHair) << name;
        s.settings.hair = false;
        const DisplayFrame& hidden = builder.update(in, s.settings);
        for (const DisplayItem& item : hidden.items) {
            if (isHairItem(item.id)) EXPECT_FALSE(item.shown) << name << ": " << item.name;
        }
        for (const DisplayItemId id : {DisplayItemId::Guides, DisplayItemId::Selected, DisplayItemId::Parts, DisplayItemId::Flow}) {
            EXPECT_TRUE(hidden.item(id).shown) << name << ": " << hidden.item(id).name;
        }
        EXPECT_FALSE(hidden.dots.points.empty());
        for (const DisplayItem& item : hidden.items) {
            if (item.rebuilt) EXPECT_TRUE(isHairItem(item.id)) << name << ": " << item.name << " built again for the hair";
        }
        s.settings.hair = true;
        const DisplayFrame& again = builder.update(in, s.settings);
        anyHair = false;
        for (const DisplayItem& item : again.items) anyHair = anyHair || (isHairItem(item.id) && item.shown);
        EXPECT_TRUE(anyHair) << name;
    }
}

TEST(Display, AColorRangeRecolorsTheLinesAlone) {
    const Fixture& f = fixture();
    Scenario s = scenarios()[3];  // attribute colors
    ASSERT_EQ(s.settings.colorMode, kAttributeColors);
    const DisplayInputs in = inputsFor(s);
    DisplayBuilder builder;
    builder.setRetain(false);
    const DisplayFrame& first = builder.update(in, s.settings);
    const DisplayItem& colored = first.item(DisplayItemId::ColoredStrands);
    ASSERT_TRUE(colored.rebuilt && colored.geometry && colored.shown);
    const std::uint64_t geometryVersion = colored.geometryVersion;
    // Not kept: the next update holds no geometry.
    builder.update(in, s.settings);
    EXPECT_FALSE(colored.geometry);
    EXPECT_FALSE(colored.rebuilt);
    // Another range: new colors, the same lines.
    s.settings.colorMax = 5.0f;
    builder.update(in, s.settings);
    EXPECT_FALSE(colored.rebuilt);
    ASSERT_TRUE(colored.recolor && colored.recolored);
    EXPECT_EQ(colored.geometryVersion, geometryVersion);
    EXPECT_GT(colored.colorVersion, geometryVersion);
    EXPECT_EQ(colored.recolored->rgba(),
              strandPointColors(*f.view.strands, strandColors(*f.view.strands, f.view.scalp.get(), kAttributeColors, "clumpId", 0.0f, 5.0f)));
    // The same colors a build gives.
    DisplayBuilder fresh;
    EXPECT_EQ(fresh.update(in, s.settings).item(DisplayItemId::ColoredStrands).geometry->colors, colored.recolored->rgba());
    // A host that no longer has the lines as built gets them built again.
    DisplayInputs gone = in;
    gone.recolorable = false;
    s.settings.colorMax = 9.0f;
    builder.update(gone, s.settings);
    EXPECT_TRUE(colored.rebuilt);
    EXPECT_FALSE(colored.recolor);
    // A setting of their geometry (the ribbons' width) builds them again too.
    s.settings.ribbonWidthScale = 2.0f;
    builder.update(in, s.settings);
    EXPECT_TRUE(colored.rebuilt);
}

// The fixture's strands with HairColor's channels as `paint` writes them.
template <class Paint>
StrandsPtr paintedStrands(Paint&& paint) {
    auto strands = std::make_shared<StrandSet>(*fixture().view.strands);
    paint(*strands);
    return strands;
}

// Each vertex's RGBA of the lit strands item, as built from `strands` (lit, solid color mode).
const DisplayItem& litStrands(DisplayBuilder& builder, const StrandsPtr& strands, bool own = true) {
    Scenario s = scenarios()[0];
    EXPECT_TRUE(s.settings.lighting && s.settings.colorMode == kSolidColors);
    s.settings.strandLookColors = own;
    DisplayInputs in = inputsFor(s);
    in.strands = strands;
    return builder.update(in, s.settings).item(DisplayItemId::Strands);
}

// Wet's wetness rides in the lit strands' colors' alpha (packed with the iridescence: hair_color.h
// packWetLook) for the hair shading: the colors kept where they would not vary, the item's color
// for every strand where they have none of their own or the host shows a material's; without a
// wetness channel the colors are as they were.
TEST(Display, LitLinesCarryTheWetness) {
    const StrandsPtr plain = fixture().view.strands;
    const std::size_t count = plain->strandCount();
    auto wetted = [&](float wetness, bool melanin) {
        return paintedStrands([&](StrandSet& s) {
            if (melanin) s.addStrandChannel(kMelaninChannel, 0.4f);  // one color for all: no stream when dry
            std::vector<float>& w = s.addPointChannel(kWetnessChannel, 0.0f);
            for (std::size_t p = 0; p < w.size(); ++p) w[p] = wetness * static_cast<float>(p % 3) / 2.0f;
        });
    };
    for (const bool melanin : {true, false}) {
        for (const bool own : {true, false}) {
            const StrandsPtr wet = wetted(0.8f, melanin);
            DisplayBuilder builder;
            const DisplayGeometry& g = *litStrands(builder, wet, own).geometry;
            ASSERT_EQ(g.colors.size(), g.vertexCount() * 4) << melanin << own;
            const std::array<float, 3> want = melanin && own ? melaninColor(0.4f, 0.2f) : scenarios()[0].settings.hairColor;
            for (std::size_t p = 0; p < g.vertexCount(); p += 5) {
                ASSERT_EQ(g.colors[p * 4 + 3], packWetLook(0.0f, (*wet->pointChannel(kWetnessChannel))[p])) << p;
                ASSERT_EQ((std::array<float, 3>{g.colors[p * 4], g.colors[p * 4 + 1], g.colors[p * 4 + 2]}), want) << p;
            }
        }
    }
    // The wetness keys the colors: another wetness, another key.
    const auto a = strandLookColors(wetted(0.8f, true)), b = strandLookColors(wetted(0.4f, true));
    ASSERT_TRUE(a && b);
    EXPECT_NE(a->key, b->key);
    EXPECT_FALSE(wetLookColors(plain, {0.5f, 0.4f, 0.3f}));
    // Per-strand colors and iridescence with it: both in the alpha.
    const StrandsPtr both = paintedStrands([&](StrandSet& s) {
        std::vector<float>& m = s.addStrandChannel(kMelaninChannel, 0.0f);
        for (std::size_t i = 0; i < count; ++i) m[i] = static_cast<float>(i % 5) / 4.0f;
        s.addPointChannel(kIridescenceChannel, 0.5f);
        s.addPointChannel(kWetnessChannel, 1.0f);
    });
    DisplayBuilder builder;
    const DisplayGeometry& g = *litStrands(builder, both).geometry;
    ASSERT_EQ(g.colors.size(), g.vertexCount() * 4);
    EXPECT_EQ(g.colors[3], packWetLook(0.5f, 1.0f));
    // A strand channel of the name (an Attribute node's): its strand's wetness on every point.
    const StrandsPtr byStrand = paintedStrands([&](StrandSet& s) {
        std::vector<float>& w = s.addStrandChannel(kWetnessChannel, 0.0f);
        for (std::size_t i = 0; i < count; ++i) w[i] = static_cast<float>(i % 4) / 3.0f;
    });
    DisplayBuilder strandBuilder;
    const DisplayGeometry& s = *litStrands(strandBuilder, byStrand).geometry;
    ASSERT_EQ(s.colors.size(), s.vertexCount() * 4);
    for (const std::size_t i : {0u, 1u, 2u, 3u, 9u}) {
        for (std::size_t p = byStrand->strandBegin(i); p < byStrand->offsets()[i + 1]; ++p) {
            ASSERT_EQ(s.colors[p * 4 + 3], packWetLook(0.0f, static_cast<float>(i % 4) / 3.0f)) << i << " " << p;
        }
    }
}

// The lit strands show each strand's own color (HairColor's: per point with a tip blend, else
// per strand, else its melanin's) as a colors stream beside their frames, and none (the item's
// one color: no memory for it) when every strand reads the same, or when the host says the
// color is a material's.
TEST(Display, LitLinesTakeTheStrandsOwnColors) {
    const StrandsPtr plain = fixture().view.strands;
    const std::size_t count = plain->strandCount();
    DisplayBuilder reference;
    const std::uint64_t litDigest = litStrands(reference, plain).geometry->digest();
    ASSERT_TRUE(reference.frame().item(DisplayItemId::Strands).geometry->colors.empty());
    // Three painted colors, per strand: each vertex its strand's, alpha 1.
    const std::array<float, 3> paints[3] = {{0.9f, 0.1f, 0.1f}, {0.1f, 0.8f, 0.2f}, {0.1f, 0.2f, 0.9f}};
    const StrandsPtr perStrand = paintedStrands([&](StrandSet& s) {
        for (int k = 0; k < 3; ++k) {
            std::vector<float>& c = s.addStrandChannel(kColorChannels[k], 0.0f);
            for (std::size_t i = 0; i < count; ++i) c[i] = paints[i % 3][static_cast<std::size_t>(k)];
        }
    });
    {
        DisplayBuilder builder;
        const DisplayGeometry& g = *litStrands(builder, perStrand).geometry;
        ASSERT_TRUE(g.lit());
        ASSERT_EQ(g.colors.size(), g.vertexCount() * 4);
        EXPECT_EQ(g.positions, reference.frame().item(DisplayItemId::Strands).geometry->positions);
        EXPECT_EQ(g.normals, reference.frame().item(DisplayItemId::Strands).geometry->normals);
        for (std::size_t i = 0; i < count; ++i) {
            for (std::size_t p = perStrand->strandBegin(i); p < perStrand->offsets()[i + 1]; ++p) {
                const float* c = g.colors.data() + p * 4;
                ASSERT_EQ((std::array<float, 4>{c[0], c[1], c[2], c[3]}),
                          (std::array<float, 4>{paints[i % 3][0], paints[i % 3][1], paints[i % 3][2], 1.0f}))
                    << "strand " << i << " point " << p;
            }
        }
        // Written straight into a host's buffers: the same bytes, whole or strand runs at a time.
        DisplayBuilder writer;
        writer.setWriteLines(true);
        const StrandLines& lines = *litStrands(writer, perStrand).geometry->lines;
        ASSERT_TRUE(lines.lit && lines.colors);
        std::vector<float> rgba(lines.vertexCount() * 4, -1.0f);
        lines.writeColors(rgba.data());
        EXPECT_EQ(rgba, g.colors);
        std::vector<float> parts(rgba.size(), -1.0f);
        for (const auto& [first, end] : strandRuns({0, 1, 2, 7, 8, 40, 41, 42}, 2)) {
            lines.writeColors(parts.data() + perStrand->strandBegin(first) * 4, first, end);
        }
        for (const std::size_t i : {0u, 2u, 7u, 41u}) {
            const std::size_t p = perStrand->strandBegin(i) * 4;
            EXPECT_EQ(std::vector<float>(parts.begin() + p, parts.begin() + p + 4), std::vector<float>(g.colors.begin() + p, g.colors.begin() + p + 4));
        }
        EXPECT_EQ(parts[perStrand->strandBegin(5) * 4], -1.0f);  // a strand outside the runs: untouched
        // The key: the same colors in another set, the same key; other colors, another.
        const auto again = strandLookColors(std::make_shared<StrandSet>(*perStrand));
        ASSERT_TRUE(again);
        EXPECT_EQ(again->key, lines.colors->key);
        const auto other = strandLookColors(paintedStrands([&](StrandSet& s) {
            for (int k = 0; k < 3; ++k) {
                std::vector<float>& c = s.addStrandChannel(kColorChannels[k], 0.0f);
                for (std::size_t i = 0; i < count; ++i) c[i] = paints[(i + 1) % 3][static_cast<std::size_t>(k)];
            }
        }));
        ASSERT_TRUE(other);
        EXPECT_NE(other->key, lines.colors->key);
    }
    // Per point (a tip blend) before per strand.
    const StrandsPtr perPoint = paintedStrands([&](StrandSet& s) {
        for (int k = 0; k < 3; ++k) {
            s.addStrandChannel(kColorChannels[k], 0.5f);
            std::vector<float>& c = s.addPointChannel(kColorChannels[k], 0.0f);
            for (std::size_t p = 0; p < c.size(); ++p) c[p] = static_cast<float>((p * 7 + static_cast<std::size_t>(k)) % 11) / 10.0f;
        }
    });
    {
        DisplayBuilder builder;
        const DisplayGeometry& g = *litStrands(builder, perPoint).geometry;
        ASSERT_EQ(g.colors.size(), g.vertexCount() * 4);
        for (std::size_t p = 0; p < g.vertexCount(); ++p) {
            for (int k = 0; k < 3; ++k) {
                ASSERT_EQ(g.colors[p * 4 + static_cast<std::size_t>(k)], (*perPoint->pointChannel(kColorChannels[k]))[p]) << p;
            }
        }
    }
    // Melanin alone: each strand its pigment's color, as the render shows it.
    const StrandsPtr pigment = paintedStrands([&](StrandSet& s) {
        std::vector<float>& m = s.addStrandChannel(kMelaninChannel, 0.0f);
        for (std::size_t i = 0; i < count; ++i) m[i] = static_cast<float>(i % 5) / 4.0f;
    });
    {
        DisplayBuilder builder;
        const DisplayGeometry& g = *litStrands(builder, pigment).geometry;
        ASSERT_EQ(g.colors.size(), g.vertexCount() * 4);
        for (const std::size_t i : {0u, 1u, 3u, 4u}) {
            const std::array<float, 3> want = melaninColor(static_cast<float>(i % 5) / 4.0f, 0.2f);
            const float* c = g.colors.data() + pigment->strandBegin(i) * 4;
            EXPECT_EQ((std::array<float, 3>{c[0], c[1], c[2]}), want) << i;
        }
    }
    // One color for every strand (and every point): no stream, the very lit buffers.
    const StrandsPtr uniform = paintedStrands([&](StrandSet& s) {
        for (int k = 0; k < 3; ++k) {
            s.addStrandChannel(kColorChannels[k], 0.3f);
            s.addPointChannel(kColorChannels[k], 0.3f);
        }
        s.addStrandChannel(kMelaninChannel, 0.4f);
    });
    EXPECT_FALSE(strandLookColors(uniform));
    EXPECT_FALSE(strandLookColors(plain));
    for (const auto& [strands, own] : {std::pair{uniform, true}, std::pair{perStrand, false}}) {
        DisplayBuilder builder;
        const DisplayGeometry& g = *litStrands(builder, strands, own).geometry;
        EXPECT_TRUE(g.colors.empty());
        EXPECT_EQ(g.digest(), litDigest);
    }
    // Flat (lighting off) and dimmed for a stroke, the lines take no colors; lit ribbons do.
    {
        Scenario s = scenarios()[0];
        DisplayInputs in = inputsFor(s);
        in.strands = perStrand;
        s.settings.lighting = false;
        DisplayBuilder flat;
        EXPECT_TRUE(flat.update(in, s.settings).item(DisplayItemId::Strands).geometry->colors.empty());
        s.settings.lighting = true;
        DisplayBuilder dimmed;
        DisplayInputs stroke = in;
        stroke.stroke = true;
        s.settings.sculptDisplay = 1;
        const DisplayFrame& f = dimmed.update(stroke, s.settings);
        EXPECT_TRUE(f.item(DisplayItemId::DimStrands).geometry->colors.empty());
        EXPECT_FALSE(f.item(DisplayItemId::DimStrands).geometry->empty());
        s.settings.ribbons = true;
        DisplayBuilder ribbons;
        const DisplayGeometry& r = *ribbons.update(in, s.settings).item(DisplayItemId::Ribbons).geometry;
        ASSERT_TRUE(r.lit());
        ASSERT_EQ(r.colors.size(), r.vertexCount() * 4);
        EXPECT_EQ(r.colors[0], paints[0][0]);  // strand 0's first ribbon vertex
    }
}

// Flat ribbons in the solid color take the plain lines' one color (the groom's: s.hairColor) on
// every strand, not the fallback strand color, and a new color builds them again.
TEST(Display, FlatRibbonsInTheSolidColorTakeTheLinesColor) {
    Scenario s = scenarios()[0];
    const DisplayInputs in = inputsFor(s);
    s.settings.lighting = false;
    s.settings.colorMode = kSolidColors;
    s.settings.ribbons = true;
    s.settings.hairColor = {0.1f, 0.2f, 0.3f};
    DisplayBuilder builder;
    builder.setWriteLines(true);  // (as Maya's override builds them: the ribbons as lines, colored per strand)
    builder.setRibbonLines(true);
    auto colorsOf = [&](const DisplayFrame& f) {
        const DisplayGeometry& r = *f.item(DisplayItemId::Ribbons).geometry;
        EXPECT_TRUE(r.ribbons);
        return r.lines && r.lines->colors ? r.lines->colors->colors : std::vector<std::array<float, 3>>();
    };
    const std::vector<std::array<float, 3>> first = colorsOf(builder.update(in, s.settings));
    ASSERT_EQ(first.size(), in.strands->strandCount());
    for (const auto& c : first) EXPECT_EQ(c, s.settings.hairColor);
    s.settings.hairColor = {0.7f, 0.6f, 0.5f};
    const std::vector<std::array<float, 3>> second = colorsOf(builder.update(in, s.settings));
    ASSERT_EQ(second.size(), in.strands->strandCount());
    for (const auto& c : second) EXPECT_EQ(c, s.settings.hairColor);
}

TEST(Display, OverlayPullLimitsStopAtTheScalp) {
    const Fixture& f = fixture();
    const std::vector<std::shared_ptr<const ScalpMesh>> occluders{f.view.scalp};
    // In front of the sphere toward the camera: free; behind it: stopped short of it (a dot pulled
    // right up to the surface showed half of itself through it: issue 12).
    const std::vector<Vec3f> points{{0.0f, 0.0f, 1.2f}, {0.0f, 0.0f, -1.2f}, {0.0f, 0.0f, -3.0f}};
    const std::vector<float> limits = overlayPullLimits(points, {0.0f, 0.0f, 5.0f}, 1.0f, occluders);
    ASSERT_EQ(limits.size(), 3u);
    EXPECT_FLOAT_EQ(limits[0], 1.0f);
    EXPECT_NEAR(limits[1], 0.18f, 0.02f);  // the sphere's back 0.2 away: nine tenths of the way
    EXPECT_LT(limits[1], 0.2f);
    EXPECT_FLOAT_EQ(limits[2], 1.0f);     // more than the pull away from it
    EXPECT_EQ(overlayPullLimits(points, {0.0f, 0.0f, 5.0f}, 0.0f, occluders), std::vector<float>(3, 1.0f));
    DisplayGeometry g;
    appendPolyline(g, points.data(), points.size());
    std::vector<float> rgba(12);
    writeOverlayColors(g, limits, rgba.data());
    EXPECT_EQ(rgba[3], limits[0]);
    EXPECT_EQ(rgba[7], limits[1]);
    EXPECT_EQ(rgba[4], 1.0f);  // no colors of its own: white, the item's color on top
}

TEST(Display, ParsesGroupColorsAndHiddenGroups) {
    const auto colors = parseGuideGroupColors("a:#ff0000 b:#00ff80 bad:#12 c:#zz0000 d#ffffff");
    ASSERT_EQ(colors.size(), 2u);
    EXPECT_FLOAT_EQ(colors.at("a")[0], 1.0f);
    EXPECT_FLOAT_EQ(colors.at("b")[1], 1.0f);
    EXPECT_NEAR(colors.at("b")[2], 0.2158605f, 1e-6f);  // sRGB 0x80 in linear
    EXPECT_EQ(parseHiddenGuideGroups(" a,b  c,"), (std::set<std::string>{"a", "b", "c"}));
}

TEST(Display, GuideColorHexIsTheParsersReverse) {
    EXPECT_EQ(guideColorHex({1.0f, 0.0f, 0.2158605f}), "#ff0080");
    EXPECT_EQ(guideColorHex({-1.0f, 2.0f, 0.0f}), "#00ff00");  // clamped
    for (const std::string hex : {"#3d7eff", "#000000", "#ffffff", "#808080", "#12ab9c"}) {
        EXPECT_EQ(guideColorHex(parseGuideGroupColors("g:" + hex).at("g")), hex);
    }
}

// The smallest hue distance (around the circle) between two automatic colors.
float hueApart(const std::array<float, 3>& a, const std::array<float, 3>& b) {
    auto hue = [](const std::array<float, 3>& c) {
        const float hi = std::max({c[0], c[1], c[2]}), lo = std::min({c[0], c[1], c[2]}), range = hi - lo;
        float h = hi == c[0] ? (c[1] - c[2]) / range : hi == c[1] ? 2.0f + (c[2] - c[0]) / range : 4.0f + (c[0] - c[1]) / range;
        h /= 6.0f;
        return h - std::floor(h);
    };
    const float d = std::abs(hue(a) - hue(b));
    return std::min(d, 1.0f - d);
}

// Guides were colored by level unless asked: a groom's child group shared every other child
// group's color. Now every group, child groups too, is its own hue: a parent and its children
// and siblings well apart, however many groups were made between them.
TEST(Display, EveryGuideGroupHasItsOwnColor) {
    GuideHierarchy h;
    const GroupId primary = h.addGroup("primary");
    const GroupId children = h.addGroup("children", primary);
    const GroupId bangs = h.addGroup("bangs");
    const GroupId bangKids = h.addGroup("bangKids", bangs);
    std::vector<GroupId> filler;  // groups made between a parent and its next child
    for (int i = 0; i < 4; ++i) filler.push_back(h.addGroup("filler" + std::to_string(i)));
    // 8 ids after their parents: golden-ratio steps alone would land 0.056 from the parent's hue.
    const GroupId late = h.addGroup("late", primary);
    const GroupId grandchild = h.addGroup("grandchild", children);
    ASSERT_EQ(late, primary + 8);
    ASSERT_EQ(grandchild, children + 8);
    for (const float seed : {0.0f, guideHueSeed("hair"), guideHueSeed("brows"), guideHueSeed("groom1"), 0.37f}) {
        const auto colors = autoGuideColors(h, seed);
        ASSERT_EQ(colors.size(), h.groups().size());
        for (const auto& [id, c] : colors) {
            EXPECT_NEAR(std::max({c[0], c[1], c[2]}), 0.95f, 1e-5f);  // bright and saturated, never gray
            EXPECT_GT(std::max({c[0], c[1], c[2]}) - std::min({c[0], c[1], c[2]}), 0.5f);
        }
        auto apart = [&](GroupId a, GroupId b) { return hueApart(colors.at(a), colors.at(b)); };
        // Parents and their children.
        for (const auto& [parent, child] : std::vector<std::pair<GroupId, GroupId>>{
                 {primary, children}, {primary, late}, {bangs, bangKids}, {children, grandchild}}) {
            EXPECT_GE(apart(parent, child), 0.12f) << "seed " << seed << ": group " << child << " near its parent";
        }
        // Siblings.
        EXPECT_GE(apart(children, late), 0.12f) << seed;
        EXPECT_GE(apart(primary, bangs), 0.12f) << seed;
        // The common case, the first group and its first child: as far apart as a golden step.
        EXPECT_GE(apart(primary, children), 0.3f) << seed;
    }
    // Deterministic, and a new group leaves the others' colors as they were.
    const auto before = autoGuideColors(h, 0.25f);
    EXPECT_EQ(before, autoGuideColors(h, 0.25f));
    h.addGroup("newest", bangs);
    const auto after = autoGuideColors(h, 0.25f);
    for (const auto& [id, c] : before) EXPECT_EQ(after.at(id), c) << id;
    // The seed turns every hue: two grooms' first groups differ.
    EXPECT_GE(hueApart(autoGuideColors(h, guideHueSeed("groom1")).at(primary), autoGuideColors(h, guideHueSeed("groom2")).at(primary)), 0.1f);
}

// Isolate dimmed every level but one, so a groom of several top-level groups (all level 0) dimmed
// nothing. It isolates a group now: the group and its child groups at full color, the rest dimmed.
TEST(Display, IsolateKeepsAGroupAndItsChildGroups) {
    auto h = std::make_shared<GuideHierarchy>();
    const GroupId bangs = h->addGroup("bangs");
    const GroupId nape = h->addGroup("nape");
    for (int i = 0; i < 3; ++i) {
        const Vec3f at{0.3f * static_cast<float>(i), 1.0f, 0.0f}, up{0.0f, 0.5f, 0.0f};
        h->addGuide(bangs, test::straightCurve(at, up));
        h->addGuide(nape, test::straightCurve(at + Vec3f{0.0f, 0.0f, 0.4f}, up));
    }
    ChildGenerationParams params;
    params.count = 2;
    h->generateChildren(bangs, "fringe", params);  // a child group of bangs
    DisplayInputs in;
    in.hierarchy = h;
    in.hierarchyVersion = 1;
    // Vertices drawn at full color and dimmed (the automatic colors are bright, a dimmed one a quarter).
    auto shares = [&](const std::string& isolate) {
        DisplaySettings s;
        s.guideSmoothing = 1;
        s.isolateGroup = isolate;
        DisplayBuilder builder;
        const std::vector<float>& colors = builder.update(in, s).item(DisplayItemId::Guides).geometry->colors;
        std::pair<std::size_t, std::size_t> out{0, 0};
        for (std::size_t i = 0; i + 3 < colors.size(); i += 4) {
            (std::max({colors[i], colors[i + 1], colors[i + 2]}) > 0.5f ? out.first : out.second) += 1;
        }
        return out;
    };
    const auto all = shares("");
    ASSERT_GT(all.first, 0u);
    EXPECT_EQ(all.second, 0u);
    const auto withChildren = shares("bangs"), sibling = shares("nape"), child = shares("fringe");
    // bangs (with fringe) and nape, two top-level groups: each dims the other, together every guide.
    EXPECT_GT(withChildren.second, 0u);
    EXPECT_GT(sibling.second, 0u);
    EXPECT_EQ(withChildren.first + sibling.first, all.first);
    EXPECT_EQ(withChildren.first, sibling.second);
    // A child group alone: bangs' own guides dim too.
    EXPECT_GT(child.first, 0u);
    EXPECT_LT(child.first, withChildren.first);
    EXPECT_EQ(child.first + child.second, all.first);
    EXPECT_EQ(shares("nothing"), all);  // no such group: none dimmed
}

// A feather's rachis (a Propagate node's shaftWidth) drew only as its shaft's strand line, a
// hairline however wide: the strands' feather layers' rachis tubes now come in the StrandMesh
// item, as exported and baked, in each feather's shaft color; the barbs stay lines.
TEST(Display, FeatherRachisDrawsAsItsTubes) {
    auto strands = std::make_shared<StrandSet>();
    for (int i = 0; i < 4; ++i) {
        strands->addStrand(static_cast<StrandId>(i + 1), test::straightCurve({0.5f * static_cast<float>(i), 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}));
    }
    strands->mutableWidths().assign(strands->pointCount(), 0.001f);
    std::vector<float>& isShaft = strands->addStrandChannel("isShaft", 0.0f);
    std::vector<float>& featherId = strands->addStrandChannel("featherId", 0.0f);
    isShaft[0] = isShaft[2] = 1.0f;
    featherId[2] = featherId[3] = 1.0f;
    std::vector<float>& red = strands->addStrandChannel("colorR", 0.2f);
    strands->addStrandChannel("colorG", 0.2f);
    strands->addStrandChannel("colorB", 0.2f);
    red[2] = 0.9f;  // feather 1's shaft
    DisplayInputs in;
    in.strands = strands;
    in.strandsVersion = 1;
    DisplaySettings s;
    DisplayBuilder builder;
    // No rachis recorded: no mesh.
    {
        const DisplayItem& none = builder.update(in, s).item(DisplayItemId::StrandMesh);
        EXPECT_TRUE(!none.geometry || none.geometry->indices.empty());
    }
    // The two shafts' rachis, 0.05 across at the root: 6-sided tubes.
    auto layer = std::make_shared<FeatherLayer>();
    layer->sides = 6;
    for (int shaft : {0, 2}) {
        const float x = 0.5f * static_cast<float>(shaft);
        for (int k = 0; k <= 4; ++k) {
            RachisRing ring;
            ring.center = {x, 0.25f * static_cast<float>(k), 0.0f};
            ring.tangent = {0.0f, 1.0f, 0.0f};
            ring.normal = {0.0f, 0.0f, 1.0f};
            ring.side = ring.tangent.cross(ring.normal);
            ring.radius = 0.025f;
            ring.u = 0.25f * static_cast<float>(k);
            layer->rings.push_back(ring);
        }
        layer->offsets.push_back(layer->rings.size());
        layer->featherIds.push_back(shaft == 0 ? 0 : 1);
    }
    auto feathers = std::make_shared<StrandSet>(*strands);
    feathers->addFeatherLayer(layer);
    in.strands = feathers;
    in.strandsVersion = 2;
    const DisplayItem& mesh = builder.update(in, s).item(DisplayItemId::StrandMesh);
    ASSERT_TRUE(mesh.geometry);
    EXPECT_TRUE(mesh.geometry->triangles && mesh.geometry->scaleSurface);
    const RachisMesh rachis = buildRachisMesh(feathers->featherLayers());
    EXPECT_EQ(mesh.vertexCount, rachis.points.size());
    EXPECT_EQ(mesh.geometry->indices.size(), 3 * rachis.triangleCount());
    float across = 0.0f;  // the tubes' radius about the shafts' axes (x 0 and x 1)
    const std::vector<float>& p = mesh.geometry->positions;
    for (std::size_t i = 0; i + 2 < p.size(); i += 3) {
        const float x = p[i] < 0.5f ? p[i] : p[i] - 1.0f;
        across = std::max(across, std::sqrt(x * x + p[i + 2] * p[i + 2]));
    }
    EXPECT_NEAR(across, 0.025f, 0.002f);
    // Each tube in its shaft's color: feather 1's red.
    bool redSeen = false, graySeen = false;
    for (std::size_t v = 0; v < mesh.vertexCount; ++v) {
        const float r = mesh.geometry->colors[v * 4];
        redSeen = redSeen || std::abs(r - 0.9f) < 1e-6f;
        graySeen = graySeen || std::abs(r - 0.2f) < 1e-6f;
    }
    EXPECT_TRUE(redSeen && graySeen);
    EXPECT_TRUE(builder.update(in, s).item(DisplayItemId::Strands).shown);  // the barbs still lines
    // Cards shown instead of the strands: no tubes either.
    s.cardsView = true;
    const DisplayItem& carded = builder.update(in, s).item(DisplayItemId::StrandMesh);
    EXPECT_TRUE(!carded.geometry || carded.geometry->indices.empty());
}

// Guide Color Mode: 1 a painted layer (black to white); anything else each group's color.
TEST(Display, GuideColorModeIsGroupOrPaintedLayer) {
    const Scenario plain = scenarios()[0];
    auto guideColors = [&](int mode) {
        DisplaySettings settings = plain.settings;
        settings.guideColorMode = mode;
        settings.guideColorLayer = "len";
        settings.guideColorSeed = "hair";
        DisplayBuilder builder;
        return builder.update(inputsFor(plain), settings).item(DisplayItemId::Guides).geometry->colors;
    };
    const std::vector<float> byGroup = guideColors(0);
    ASSERT_FALSE(byGroup.empty());
    EXPECT_EQ(guideColors(2), byGroup);   // a value of old: by group
    EXPECT_EQ(guideColors(-3), byGroup);
    const std::vector<float> byLayer = guideColors(1);
    ASSERT_EQ(byLayer.size(), byGroup.size());
    EXPECT_NE(byLayer, byGroup);
    const DisplayTheme theme;
    std::size_t grays = 0;
    for (std::size_t i = 0; i + 3 < byLayer.size(); i += 4) {  // grays (frozen guides in the theme's color)
        if (byLayer[i] == theme.frozen[0] && byLayer[i + 1] == theme.frozen[1] && byLayer[i + 2] == theme.frozen[2]) continue;
        ++grays;
        EXPECT_FLOAT_EQ(byLayer[i], byLayer[i + 1]);
        EXPECT_FLOAT_EQ(byLayer[i], byLayer[i + 2]);
    }
    EXPECT_GT(grays, 0u);
}

// Testers saw the first guide group of every groom in the same color: the automatic colors start
// from a hue seeded by the groom's name (every host passes it as guideColorSeed), deterministic,
// and a group's own color still over them.
TEST(Display, GuideColorsStartFromTheGroomsOwnHue) {
    EXPECT_EQ(guideHueSeed(""), 0.0f);
    EXPECT_EQ(guideHueSeed("hair"), guideHueSeed("hair"));
    const float hair = guideHueSeed("hair"), brows = guideHueSeed("brows"), groom1 = guideHueSeed("groom1"),
                groom2 = guideHueSeed("groom2");
    for (const float h : {hair, brows, groom1, groom2}) {
        EXPECT_GE(h, 0.0f);
        EXPECT_LT(h, 1.0f);
    }
    EXPECT_NE(hair, brows);
    EXPECT_NE(groom1, groom2);  // names a step apart: hues apart
    // The first group starts at the groom's hue, the next a golden-ratio step on.
    GuideHierarchy h;
    const GroupId first = h.addGroup("primary"), second = h.addGroup("bangs"), third = h.addGroup("nape");
    EXPECT_EQ(autoGuideColors(h, 0.0f).at(first), hueColor(std::fmod(0.6f + 0.618034f * static_cast<float>(first), 1.0f), 0.75f, 0.95f));
    EXPECT_EQ(autoGuideColors(h, 0.0f).at(third), hueColor(std::fmod(0.6f + 0.618034f * static_cast<float>(third), 1.0f), 0.75f, 0.95f));
    EXPECT_GE(hueApart(autoGuideColors(h, groom1).at(first), autoGuideColors(h, groom2).at(first)), 0.1f);
    // Maya's own names a digit apart: well apart (FNV alone put them 0.004 of a turn apart).
    EXPECT_GE(hueApart(autoGuideColors(h, guideHueSeed("groomLab1")).at(first), autoGuideColors(h, guideHueSeed("groomLab2")).at(first)), 0.1f);
    EXPECT_GE(hueApart(autoGuideColors(h, hair).at(first), autoGuideColors(h, hair).at(second)), 0.3f);

    // Drawn: two grooms' guides differ by name alone; a group's own color wins over the seed.
    const Scenario plain = scenarios()[0];
    auto guideColors = [&](const std::string& seed, const std::string& own) {
        DisplaySettings settings = plain.settings;
        settings.guideColorSeed = seed;
        settings.guideGroupColors = own;
        DisplayBuilder builder;
        return builder.update(inputsFor(plain), settings).item(DisplayItemId::Guides).geometry->colors;
    };
    EXPECT_NE(guideColors("groom1", ""), guideColors("groom2", ""));
    EXPECT_EQ(guideColors("groom1", ""), guideColors("groom1", ""));
    const std::string own = "primary:#ff8800 children:#00ff80";
    EXPECT_EQ(guideColors("groom1", own), guideColors("groom2", own));
}

}  // namespace
}  // namespace groomlab
