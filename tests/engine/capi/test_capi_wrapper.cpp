// SPDX-License-Identifier: Apache-2.0
// The C API through its C++ wrapper (groomlab.hpp), against the core's own GroomSession: the same
// groom evaluated both ways gives the same strands, bit for bit. Also params, errors, lifetimes,
// and that the library exports the C API only and loads no TBB.

#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#include <gtest/gtest.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "groomlab/export_view.h"
#include "groomlab/frame_time.h"
#include "groomlab/groom_file.h"
#include "groomlab/graph_edit.h"
#include "groomlab/groom_session.h"
#include "groomlab/groomlab.hpp"
#include "groomlab/node_library.h"
#include "groomlab/paths.h"
#include "groomlab/presets.h"
#include "groomlab/scalp_mesh.h"

namespace {

namespace fs = std::filesystem;
namespace api = groomlab::api;
using nlohmann::json;

const fs::path kData = fs::u8path(GROOMLAB_TEST_DATA);
const fs::path kPresets = fs::u8path(GROOMLAB_SHIPPED_PRESETS);
const std::string kCompatGroom = (kData / "compat" / "v1" / "groom.groom").u8string();

std::string readText(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::stringstream s;
    s << in.rdbuf();
    return s.str();
}

groomlab::ViewRequest request(double seconds = 0.0, float density = 1.0f) {
    groomlab::ViewRequest r;
    r.density = density;
    r.time = seconds;
    r.wantBounds = false;
    return r;
}

// The API's result is the core's, bit for bit: counts, IDs, positions, widths, every channel.
void expectSame(const api::Strands& got, const groomlab::StrandSet& want) {
    ASSERT_EQ(got.strandCount(), want.strandCount());
    ASSERT_EQ(got.pointCount(), want.pointCount());
    ASSERT_GT(want.strandCount(), 0u);
    EXPECT_EQ(0, std::memcmp(got.positions().data(), want.positions().data(), sizeof(float) * 3 * want.pointCount()));
    EXPECT_EQ(0, std::memcmp(got.widths().data(), want.widths().data(), sizeof(float) * want.pointCount()));
    EXPECT_EQ(0, std::memcmp(got.ids().data(), want.ids().data(), sizeof(std::uint64_t) * want.strandCount()));
    for (std::size_t s = 0; s < want.strandCount(); ++s) {
        ASSERT_EQ(static_cast<std::size_t>(got.pointCounts()[s]), want.strandPointCount(s));
        ASSERT_EQ(got.offsets()[s], want.strandBegin(s));
    }
    EXPECT_EQ(got.offsets()[want.strandCount()], want.pointCount());
    for (const std::string& name : want.strandChannelNames()) {
        const api::Attribute a = got.attribute(name);
        EXPECT_EQ(a.scope, api::Scope::Strand) << name;
        ASSERT_EQ(a.values.size(), want.strandCount()) << name;
        EXPECT_EQ(0, std::memcmp(a.values.data(), want.strandChannel(name)->data(), sizeof(float) * want.strandCount())) << name;
    }
    for (const std::string& name : want.pointChannelNames()) {
        // By scope: a strand channel of the same name (HairColor's colorR/G/B per strand, and per
        // point when banded) does not hide it.
        const api::Attribute a = got.attribute(name, api::Scope::Point);
        EXPECT_EQ(a.scope, api::Scope::Point) << name;
        ASSERT_EQ(a.values.size(), want.pointCount()) << name;
        EXPECT_EQ(0, std::memcmp(a.values.data(), want.pointChannel(name)->data(), sizeof(float) * want.pointCount())) << name;
    }
}

// The compat groom as the core reads it, in a session set up the way the API sets one up.
struct CoreGroom {
    groomlab::GroomFile file = groomlab::readGroomFile(kCompatGroom);
    groomlab::GroomSession session{file.data};
    CoreGroom() {
        session.setPathResolver(groomlab::searchPathResolver({file.directory}));
        session.setFramesPerSecond(file.framesPerSecond);
    }
};

TEST(CApi, VersionAndOperators) {
    EXPECT_TRUE(api::compatible());
    EXPECT_FALSE(api::versionString().empty());
    const json types = json::parse(api::operatorTypesJson());
    ASSERT_TRUE(types.is_array());
    EXPECT_NE(std::find(types.begin(), types.end(), json("Scatter")), types.end());
    const json specs = json::parse(api::operatorSpecsJson());
    ASSERT_EQ(specs.size(), types.size());
    const json scatter = json::parse(api::operatorSpecJson("Scatter"));
    EXPECT_EQ(scatter.at("type"), "Scatter");
    EXPECT_FALSE(scatter.at("params").empty());
    EXPECT_EQ(api::defaultGraphJson(), groomlab::defaultGraphJson());
}

TEST(CApi, GroomFileEvaluatesAsTheCoreSession) {
    CoreGroom core;
    api::Session session = api::Session::fromFile(kCompatGroom);
    EXPECT_EQ(session.framesPerSecond(), core.file.framesPerSecond);
    expectSame(session.evaluate(), *core.session.evaluate(request()).strands);

    // At another frame and density.
    session.setFrame(7.0);
    EXPECT_DOUBLE_EQ(session.time(), 7.0 / core.file.framesPerSecond);
    api::EvalOptions half;
    half.density = 0.5f;
    expectSame(session.evaluate(half), *core.session.evaluate(request(7.0 / core.file.framesPerSecond, 0.5f)).strands);

    // The derived attributes, from the core's export description.
    const api::Strands strands = session.evaluate();
    const groomlab::StrandsPtr want = core.session.evaluate(request(7.0 / core.file.framesPerSecond)).strands;
    const groomlab::ExportView view = groomlab::describeExport(*want, core.session.last().scalp.get());
    const api::Attribute uv = strands.attribute("rootUV");
    EXPECT_EQ(uv.components, 2);
    ASSERT_EQ(uv.values.size(), 2 * view.rootUVs.size());
    EXPECT_EQ(0, std::memcmp(uv.values.data(), view.rootUVs.data(), sizeof(float) * uv.values.size()));
    EXPECT_EQ(strands.hasAttribute("color"), !view.colors.empty());  // no HairColor or melanin here
}

TEST(CApi, ScalpFromPolygonsEvaluatesAsTheCoreSession) {
    // The compat groom's scalp handed over as polygons (triangles), with its UVs, and a shipped preset.
    const groomlab::GroomFile file = groomlab::readGroomFile(kCompatGroom);
    const groomlab::ScalpMesh& mesh = *file.data.scalp;
    ASSERT_TRUE(mesh.hasUVs());
    std::vector<float> points, uvs;
    std::vector<std::int32_t> counts(mesh.triangleCount(), 3), indices;
    for (const groomlab::Vec3f& p : mesh.points()) points.insert(points.end(), {p.x, p.y, p.z});
    for (const auto& t : mesh.triangles()) indices.insert(indices.end(), t.begin(), t.end());
    for (const auto& uv : mesh.cornerUVs()) uvs.insert(uvs.end(), uv.begin(), uv.end());
    const std::string preset = readText(kPresets / "hair_basics.json");

    api::Session session;
    session.setGraphJson(preset);
    session.setScalp(points, counts, indices, uvs);
    EXPECT_EQ(session.scalpPointCount(), mesh.pointCount());
    EXPECT_EQ(session.scalpTriangleCount(), mesh.triangleCount());

    groomlab::GroomSession core;
    core.setGraphJson(preset);
    auto scalp = std::make_shared<groomlab::ScalpMesh>(mesh.points(), mesh.triangles());
    scalp->setCornerUVs(mesh.cornerUVs());
    scalp->setPolygons(std::vector<std::int32_t>(mesh.triangleCount(), 1));
    core.setScalp(scalp);
    {
        const api::Strands strands = session.evaluate();
        const groomlab::StrandsPtr want = core.evaluate(request()).strands;
        expectSame(strands, *want);
        // HairColor's look as the exporters see it: per-strand linear RGB.
        const groomlab::ExportView view = groomlab::describeExport(*want, core.last().scalp.get());
        ASSERT_FALSE(view.colors.empty());
        const api::Attribute color = strands.attribute("color");
        EXPECT_EQ(color.scope, api::Scope::Strand);
        EXPECT_EQ(color.components, 3);
        ASSERT_EQ(color.values.size(), 3 * view.colors.size());
        EXPECT_EQ(0, std::memcmp(color.values.data(), view.colors.data(), sizeof(float) * color.values.size()));
        EXPECT_EQ(strands.attribute("rootUV").values.size(), 2 * strands.strandCount());
    }

    // The scalp deforms (a rest pose kept): the same on both sides again.
    std::vector<float> moved = points;
    std::vector<groomlab::Vec3f> movedCore;
    for (std::size_t i = 0; i < moved.size(); i += 3) {
        moved[i + 1] += 0.5f + 0.1f * std::sin(moved[i]);
        movedCore.emplace_back(moved[i], moved[i + 1], moved[i + 2]);
    }
    session.updateScalpPoints(moved, &points);
    core.updateScalpPoints(movedCore, &mesh.points());
    expectSame(session.evaluate(), *core.evaluate(request()).strands);

    // A param set through the API is the graph the core gets.
    session.setFloat("noise", "amplitude", 0.6);
    core.setGraphJson(session.graphJson());
    expectSame(session.evaluate(), *core.evaluate(request()).strands);
}

TEST(CApi, EveryShippedPresetEvaluatesAsTheCoreSessionBitForBit) {
    // Every shipped preset on the reference groom (PresetLooks': tests/data/strokes, the preset
    // fitted to its size), written as a groom file by the core and read by the library: the
    // library's own threads (parallel_pool.cpp) give what the core's TBB gives, bit for bit, at
    // the first frame and (Wind, Jiggle, Simulate: the presets that read time) a later one.
    const groomlab::GroomFile reference = groomlab::readGroomFile((kData / "strokes" / "strokes.groom").u8string());
    ASSERT_TRUE(reference.data.scalp);
    const std::optional<double> size = groomlab::presetScalpSize(*reference.data.scalp);
    const fs::path dir = fs::temp_directory_path() / "groomlab_capi_presets";
    fs::create_directories(dir);
    std::size_t compared = 0;
    for (const groomlab::PresetInfo& p : groomlab::listPresets({kPresets.u8string()})) {
        groomlab::GroomFile file = reference;
        file.data.graphJson = groomlab::presetGraphJson(readText(fs::u8path(p.path)), size);
        file.searchPaths = {reference.directory, kPresets.u8string()};
        const std::string path = (dir / (p.name + ".groom")).u8string();
        groomlab::writeGroomFile(path, file);

        // The core as the library sets a groom file up (adoptFile): its folder, then its search paths.
        const groomlab::GroomFile read = groomlab::readGroomFile(path);
        groomlab::GroomSession core(read.data);
        std::vector<std::string> dirs{read.directory};
        dirs.insert(dirs.end(), read.searchPaths.begin(), read.searchPaths.end());
        core.setPathResolver(groomlab::searchPathResolver(dirs));
        core.setFramesPerSecond(read.framesPerSecond);
        api::Session session = api::Session::fromFile(path);
        api::EvalOptions options;
        options.density = 0.05f;
        const bool timed = core.readsTime();
        for (const double frame : timed ? std::vector<double>{0.0, 9.0} : std::vector<double>{0.0}) {
            const double seconds = frame / read.framesPerSecond;
            const groomlab::ViewResult& want = core.evaluate(request(seconds, options.density));
            session.setTime(seconds);
            if (!want.strands || want.strands->strandCount() == 0) {
                EXPECT_THROW(session.evaluate(options), api::Error) << p.name;
                continue;
            }
            SCOPED_TRACE(p.name + " at frame " + std::to_string(frame));
            expectSame(session.evaluate(options), *want.strands);
            ++compared;
        }
    }
    EXPECT_GT(compared, 30u);
    std::error_code ec;
    fs::remove_all(dir, ec);
}

TEST(CApi, AGroomWithLibraryNodesRendersWithoutTheLibrary) {
    // Every shipped compound node added to the reference groom before its output by the core
    // (the library at hand), the groom saved, the library gone: the API (its own copy of the core,
    // which never saw the library) gives what the core gives, bit for bit.
    const fs::path empty = fs::temp_directory_path() / "groomlab_capi_no_nodes";
    fs::create_directories(empty);
#ifdef _WIN32
    _putenv_s("GROOMLAB_USER_NODES", empty.u8string().c_str());
    _putenv_s("GROOMLAB_NODE_PATH", "");
#else
    setenv("GROOMLAB_USER_NODES", empty.u8string().c_str(), 1);
    unsetenv("GROOMLAB_NODE_PATH");
#endif
    groomlab::setShippedNodeDir((kPresets.parent_path() / "nodes").u8string());
    groomlab::GroomFile file = groomlab::readGroomFile((kData / "strokes" / "strokes.groom").u8string());
    ASSERT_TRUE(file.data.scalp);
    groomlab::GraphEdit edit(groomlab::defaultGraphJsonFor(groomlab::presetScalpSize(*file.data.scalp)));
    const std::string out = edit.outputNode();
    std::string last;
    const json graph = json::parse(edit.toJson());
    for (const json& c : graph["connections"]) {
        if (c["to"] == out && c["input"] == "strands") last = c["from"];
    }
    ASSERT_FALSE(last.empty());
    std::size_t added = 0;
    for (const groomlab::LibraryNode& node : groomlab::libraryNodes()) {
        const std::string name = edit.addNode(groomlab::libraryNodeType(node.id), {}, {}, std::nullopt, std::nullopt,
                                              groomlab::presetScalpSize(*file.data.scalp));
        edit.connect(last, name, "strands");
        last = name;
        ++added;
    }
    EXPECT_EQ(added, 7u);  // the compound nodes and the script nodes
    edit.connect(last, out, "strands");
    groomlab::setShippedNodeDir("");
    file.data.graphJson = edit.toJson();
    const std::string path = (empty / "with_nodes.groom").u8string();
    groomlab::writeGroomFile(path, file);

    const groomlab::GroomFile read = groomlab::readGroomFile(path);
    groomlab::GroomSession core(read.data);
    core.setPathResolver(groomlab::searchPathResolver({read.directory}));
    core.setFramesPerSecond(read.framesPerSecond);
    api::Session session = api::Session::fromFile(path);
    api::EvalOptions options;
    options.density = 0.1f;
    const groomlab::ViewResult& want = core.evaluate(request(0.0, options.density));
    ASSERT_TRUE(want.strands);
    expectSame(session.evaluate(options), *want.strands);
    std::error_code ec;
    fs::remove_all(empty, ec);
}

TEST(CApi, AChannelPerStrandAndPerPointIsThereInBoth) {
    // A banded HairColor writes colorR/G/B per strand and per point: both are listed, each read in
    // its own scope; by name alone the per strand one.
    const groomlab::GroomFile reference = groomlab::readGroomFile((kData / "strokes" / "strokes.groom").u8string());
    groomlab::GroomFile file = reference;
    file.data.graphJson =
        groomlab::presetGraphJson(readText(kPresets / "agouti_fur.json"), groomlab::presetScalpSize(*reference.data.scalp));
    file.searchPaths = {reference.directory, kPresets.u8string()};
    const std::string text = groomlab::groomFileToJson(file);
    api::Session session = api::Session::fromMemory(text, reference.directory);
    api::EvalOptions options;
    options.density = 0.05f;
    const api::Strands strands = session.evaluate(options);
    groomlab::GroomSession core(groomlab::groomFileFromJson(text, reference.directory).data);
    std::vector<std::string> dirs{reference.directory, kPresets.u8string()};
    core.setPathResolver(groomlab::searchPathResolver(dirs));
    const groomlab::StrandsPtr want = core.evaluate(request(0.0, options.density)).strands;
    ASSERT_TRUE(want && want->strandChannel("colorR") && want->pointChannel("colorR"));
    expectSame(strands, *want);
    std::size_t both = 0;
    const std::vector<api::Attribute> all = strands.attributes();
    for (const api::Attribute& a : all) {
        both += a.name == "colorR" ? 1 : 0;
        const std::vector<float>* values = a.scope == api::Scope::Point ? want->pointChannel(a.name) : want->strandChannel(a.name);
        if (!values) continue;  // a derived one
        ASSERT_EQ(a.values.size(), values->size()) << a.name;
        EXPECT_EQ(0, std::memcmp(a.values.data(), values->data(), sizeof(float) * values->size())) << a.name;
    }
    EXPECT_EQ(both, 2u);
    EXPECT_EQ(strands.attribute("colorR").scope, api::Scope::Strand);
    EXPECT_EQ(strands.attribute("colorR", api::Scope::Point).values.size(), strands.pointCount());
}

TEST(CApi, SaysWhenTheScalpIsNotTheGroomsTopology) {
    // A groom file keeps the fingerprint of its scalp: a bridge feeding the same triangles hears
    // nothing; another vertex order (each triangle's corners turned) is said in the evaluation's
    // errors, and the strands still come.
    const groomlab::GroomFile file = groomlab::readGroomFile(kCompatGroom);
    const groomlab::ScalpMesh& mesh = *file.data.scalp;
    std::vector<float> points, uvs, turnedUVs;
    std::vector<std::int32_t> counts(mesh.triangleCount(), 3), same, turned;
    for (const groomlab::Vec3f& p : mesh.points()) points.insert(points.end(), {p.x, p.y, p.z});
    ASSERT_TRUE(mesh.hasUVs());  // its scatter reads a texture map
    for (std::size_t t = 0; t < mesh.triangleCount(); ++t) {
        const auto& tri = mesh.triangles()[t];
        same.insert(same.end(), tri.begin(), tri.end());
        turned.insert(turned.end(), {tri[1], tri[2], tri[0]});
        for (const int c : {0, 1, 2}) uvs.insert(uvs.end(), mesh.cornerUVs()[3 * t + c].begin(), mesh.cornerUVs()[3 * t + c].end());
        for (const int c : {1, 2, 0}) turnedUVs.insert(turnedUVs.end(), mesh.cornerUVs()[3 * t + c].begin(), mesh.cornerUVs()[3 * t + c].end());
    }
    api::Session session = api::Session::fromFile(kCompatGroom);
    session.setGraphJson(readText(kPresets / "hair_basics.json"));  // reads no color sets (the API takes none)
    session.setScalp(points, counts, same, uvs);
    EXPECT_GT(session.evaluate().strandCount(), 0u);
    EXPECT_EQ(json::parse(session.evalErrorsJson()), json::array());
    session.setScalp(points, counts, turned, turnedUVs);
    EXPECT_GT(session.evaluate().strandCount(), 0u);
    EXPECT_EQ(json::parse(session.evalErrorsJson()), json::array({groomlab::kScalpTopologyChanged}));
    // Saved, the file holds the scalp it has now (and its fingerprint): read again, nothing to say.
    api::Session again = api::Session::fromMemory(session.saveMemory());
    again.evaluate();
    EXPECT_EQ(json::parse(again.evalErrorsJson()), json::array());
}

TEST(CApi, TurnsTheGroomIntoTheHostsUpAxis) {
    // gl_session_set_up_axis is the core's convertUpAxis: a Z-up bridge gets the groom the app or
    // Maya's import would make of the file in a Z-up scene, bit for bit, params and all.
    groomlab::GroomFile file = groomlab::readGroomFile(kCompatGroom);
    groomlab::convertUpAxis(file, "Z");
    groomlab::GroomSession core(file.data);
    core.setPathResolver(groomlab::searchPathResolver({file.directory}));
    core.setFramesPerSecond(file.framesPerSecond);
    api::Session session = api::Session::fromFile(kCompatGroom);
    EXPECT_EQ(session.upAxis(), "Y");
    session.setUpAxis("Z");
    EXPECT_EQ(session.upAxis(), "Z");
    expectSame(session.evaluate(), *core.evaluate(request()).strands);
    EXPECT_EQ(session.graphJson(), file.data.graphJson);
    session.setUpAxis("Z");  // already: nothing
    EXPECT_EQ(session.upAxis(), "Z");
    EXPECT_EQ(json::parse(session.saveMemory()).at("upAxis"), "Z");
    // Frames by every host's rule.
    EXPECT_EQ(api::frameToSeconds(7.0, 23.976), groomlab::frameToSeconds(7.0, 24000.0 / 1001.0));
    session.setFramesPerSecond(23.976);
    session.setFrame(7.0);
    EXPECT_EQ(session.time(), 7.0 * 1001.0 / 24000.0);
}

TEST(CApi, EveryGroomExportsAndAnOldLicenseTagIsIgnored) {
    // GroomLab is free: nothing is gated. A file of the old Learning mode ("license": "learning")
    // loads and evaluates, the deprecated calls (API 1.4) always succeed, and saving drops the tag.
    api::Session session = api::Session::fromFile(kCompatGroom);
    json text = json::parse(session.saveMemory());
    EXPECT_FALSE(text.contains("license"));
    text["license"] = "learning";
    api::Session old = api::Session::fromMemory(text.dump(), std::filesystem::u8path(kCompatGroom).parent_path().u8string());
    EXPECT_EQ(gl_session_check_export(old.handle()), GL_OK);
    char* tag = nullptr;
    ASSERT_EQ(gl_session_license(old.handle(), &tag), GL_OK);
    ASSERT_NE(tag, nullptr);
    EXPECT_STREQ(tag, "");
    gl_free(tag);
    EXPECT_EQ(gl_session_check_export(nullptr), GL_ERROR_INVALID_ARGUMENT);
    EXPECT_GT(old.evaluate().strandCount(), 0u);
    EXPECT_FALSE(json::parse(old.saveMemory()).contains("license"));
    EXPECT_EQ(gl_api_version(), (1u << 16) | 4u);
}

TEST(CApi, ParamsRoundTrip) {
    api::Session session = api::Session::fromFile(kCompatGroom);
    const std::vector<std::string> nodes = session.nodeNames();
    ASSERT_EQ(nodes.size(), 20u);
    EXPECT_EQ(session.nodeType("comb"), "Bend");

    EXPECT_DOUBLE_EQ(session.getFloat("noise", "amplitude"), 0.03);
    session.setFloat("noise", "amplitude", 0.125);
    EXPECT_EQ(session.getFloat("noise", "amplitude"), 0.125);
    EXPECT_EQ(json::parse(session.paramJson("noise", "amplitude")), json(0.125));
    EXPECT_EQ(session.getInt("switch", "index"), 0);
    EXPECT_EQ(session.getFloat("switch", "index"), 0.0);  // an int reads as a float too
    session.setInt("clump", "count", 31);
    EXPECT_EQ(session.getInt("clump", "count"), 31);
    session.setVec3("comb", "direction", {1.0, 0.0, 0.5});
    EXPECT_EQ(session.getVec3("comb", "direction"), (std::array<double, 3>{1.0, 0.0, 0.5}));
    session.setString("attr", "name", "lengthFactor");
    EXPECT_EQ(session.getString("attr", "name"), "lengthFactor");
    session.setParamJson("clump2", "profile", "[[0, 0], [1, 1]]");
    EXPECT_EQ(json::parse(session.paramJson("clump2", "profile")), json::parse("[[0, 0], [1, 1]]"));

    // A bool param of one of the nodes: flipped and read back. Defaults read as the spec's.
    bool found = false;
    for (const std::string& node : nodes) {
        const json spec = json::parse(session.nodeSpecJson(node));
        for (const json& p : spec.at("params")) {
            if (p.value("kind", "") != "bool") continue;
            const std::string name = p.at("name").get<std::string>();
            const bool was = session.getBool(node, name);
            session.setBool(node, name, !was);
            EXPECT_EQ(session.getBool(node, name), !was) << node << "." << name;
            found = true;
            break;
        }
        if (found) break;
    }
    EXPECT_TRUE(found);
    const json defaultResample = json::parse(session.nodeSpecJson("resample"));
    for (const json& p : defaultResample.at("params")) {
        if (p.at("name") == "segments") continue;
        EXPECT_EQ(json::parse(session.paramJson("resample", p.at("name").get<std::string>())), p.at("default"));
    }

    // The edits are in the graph and in a saved file.
    api::Session reloaded = api::Session::fromMemory(session.saveMemory());
    EXPECT_EQ(reloaded.getFloat("noise", "amplitude"), 0.125);
    EXPECT_EQ(reloaded.getInt("clump", "count"), 31);
}

TEST(CApi, ErrorsCarryTheirCodeAndMessage) {
    try {
        api::Session::fromFile("missing/nothing.groom");
        FAIL() << "no error";
    } catch (const api::Error& e) {
        EXPECT_EQ(e.code(), GL_ERROR_IO);
        EXPECT_NE(e.message().find("missing/nothing.groom"), std::string::npos);
    }
    api::Session session = api::Session::fromFile(kCompatGroom);
    const std::string graph = session.graphJson();
    auto codeOf = [](auto&& call) -> gl_result {
        try {
            call();
        } catch (const api::Error& e) {
            EXPECT_FALSE(e.message().empty());
            return e.code();
        }
        return GL_OK;
    };
    EXPECT_EQ(codeOf([&] { session.setString("noise", "amplitude", "x"); }), GL_ERROR_TYPE_MISMATCH);
    EXPECT_EQ(codeOf([&] { session.getFloat("scatter", "density"); }), GL_ERROR_TYPE_MISMATCH);  // mapped
    EXPECT_EQ(codeOf([&] { session.getFloat("nobody", "x"); }), GL_ERROR_NOT_FOUND);
    EXPECT_EQ(codeOf([&] { session.setGraphJson("{\"nodes\": [{\"name\": \"a\", \"type\": \"Nope\"}]}"); }), GL_ERROR_GRAPH);
    EXPECT_EQ(codeOf([&] { session.loadMemory("{]"); }), GL_ERROR_PARSE);
    EXPECT_EQ(codeOf([&] { session.updateScalpPoints({1.0f, 2.0f, 3.0f}); }), GL_ERROR_INVALID_ARGUMENT);
    EXPECT_EQ(session.graphJson(), graph);  // every failure left the session as it was
    EXPECT_STREQ(gl_last_error(), "");      // the last call succeeded
    api::Strands strands = session.evaluate();
    EXPECT_GT(strands.strandCount(), 0u);
}

// A node at its operator's newest version: an upgrade changes nothing. (The compat groom's nodes
// are all current but its Interpolate, saved at version 1, before blend directions became the
// default.) test_capi_upgrade.cpp upgrades a node that is outdated.
TEST(CApi, UpgradingACurrentNodeChangesNothing) {
    api::Session session = api::Session::fromFile(kCompatGroom);
    const std::string graph = session.graphJson();
    for (const std::string& node : session.nodeNames()) {
        const json spec = json::parse(session.nodeSpecJson(node));
        if (spec.value("type", std::string()) == "Interpolate") {
            EXPECT_TRUE(spec.at("outdated").get<bool>()) << node;
            continue;
        }
        EXPECT_FALSE(spec.at("outdated").get<bool>()) << node;
        EXPECT_FALSE(session.upgradeNode(node)) << node;
    }
    EXPECT_EQ(session.graphJson(), graph);
    try {
        session.upgradeNode("nobody");
        ADD_FAILURE() << "upgraded a node that is not there";
    } catch (const api::Error& e) {
        EXPECT_EQ(e.code(), GL_ERROR_NOT_FOUND);
        EXPECT_NE(e.message().find("nobody"), std::string::npos) << e.message();
    }
}

TEST(CApi, ResultsOutliveTheirSession) {
    api::Strands strands;
    std::vector<float> copy;
    {
        api::Session session = api::Session::fromFile(kCompatGroom);
        strands = session.evaluate();
        copy = strands.positions().toVector();
        api::Session moved = std::move(session);
        moved.setFloat("noise", "amplitude", 0.5);
        const api::Strands other = moved.evaluate();
        EXPECT_EQ(other.strandCount(), strands.strandCount());
    }
    ASSERT_EQ(strands.positions().size(), copy.size());
    EXPECT_EQ(0, std::memcmp(strands.positions().data(), copy.data(), sizeof(float) * copy.size()));
    const api::View<float> first = strands.strandPositions(0);
    EXPECT_EQ(first.size(), 3u * static_cast<std::size_t>(strands.pointCounts()[0]));
    EXPECT_EQ(first.data(), strands.positions().data());
}

#ifdef _WIN32
TEST(CApi, TheLibraryExportsTheCApiOnly) {
    const HMODULE module = GetModuleHandleW(L"groomlab.dll");
    ASSERT_NE(module, nullptr);
    const auto* base = reinterpret_cast<const unsigned char*>(module);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    const IMAGE_DATA_DIRECTORY& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    ASSERT_GT(dir.Size, 0u);
    const auto* exports = reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(base + dir.VirtualAddress);
    EXPECT_EQ(exports->NumberOfFunctions, exports->NumberOfNames);  // nothing exported by ordinal only
    const auto* names = reinterpret_cast<const DWORD*>(base + exports->AddressOfNames);
    std::size_t api = 0;
    for (DWORD i = 0; i < exports->NumberOfNames; ++i) {
        const std::string name = reinterpret_cast<const char*>(base + names[i]);
        EXPECT_EQ(name.rfind("gl_", 0), 0u) << name;
        api += name.rfind("gl_", 0) == 0 ? 1 : 0;
    }
    EXPECT_GT(api, 40u);
}

// The library brings no TBB: a host that loaded its own (another version under the same tbb.dll)
// never hands it one. Its imports, delay-loaded ones too, are the system's and the C++ runtime's.
TEST(CApi, TheLibraryImportsNoTbb) {
    const HMODULE module = GetModuleHandleW(L"groomlab.dll");
    ASSERT_NE(module, nullptr);
    const auto* base = reinterpret_cast<const unsigned char*>(module);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + reinterpret_cast<const IMAGE_DOS_HEADER*>(base)->e_lfanew);
    std::vector<std::string> dlls;
    const IMAGE_DATA_DIRECTORY& imports = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    ASSERT_GT(imports.Size, 0u);
    for (auto* d = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(base + imports.VirtualAddress); d->Name; ++d) {
        dlls.emplace_back(reinterpret_cast<const char*>(base + d->Name));
    }
    const IMAGE_DATA_DIRECTORY& delayed = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT];
    if (delayed.Size) {
        for (auto* d = reinterpret_cast<const IMAGE_DELAYLOAD_DESCRIPTOR*>(base + delayed.VirtualAddress); d->DllNameRVA; ++d) {
            dlls.emplace_back(reinterpret_cast<const char*>(base + d->DllNameRVA));
        }
    }
    EXPECT_NE(std::find_if(dlls.begin(), dlls.end(), [](const std::string& n) { return _stricmp(n.c_str(), "KERNEL32.dll") == 0; }),
              dlls.end());  // the walk found the imports
    for (std::string name : dlls) {
        std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        EXPECT_NE(name.rfind("tbb", 0), 0u) << name;
    }
}
#endif

}  // namespace
