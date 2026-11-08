// SPDX-License-Identifier: Apache-2.0
// The USD export's layout is a public contract (web/content/docs/usd-layout.mdx, layout version
// kUsdLayoutVersion): integrators read the layers GroomLab writes by these prim paths, attribute
// names, types and interpolations. These tests parse what UsdCurvesWriter writes (small grooms:
// with and without scales, one frame and a range) and compare it with the spec, both ways: an
// attribute the spec does not list fails as much as a missing one.
//
// When one fails: either the writer changed by accident (put it back), or on purpose: update
// usd-layout.mdx and the expectations below together, and if a name, type, interpolation or
// meaning changed (not just a new primvar), raise kUsdLayoutVersion (usd_writer.h) too.

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <map>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "groomlab/graph.h"
#include "groomlab/operators.h"
#include "groomlab/scales.h"
#include "groomlab/usd_writer.h"
#include "groomlab/version.h"
#include "test_helpers.h"

namespace groomlab {
namespace {

namespace fs = std::filesystem;

constexpr const char* kSpec = "web/content/docs/usd-layout.mdx";

std::string readText(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::stringstream s;
    s << in.rdbuf();
    return s.str();
}

// ---- A reader of the .usda subset the writer writes --------------------------------------------

struct Attribute {
    std::string type;           // as declared, "uniform " included: "uniform token", "float[]"
    std::string interpolation;  // its metadata's; empty: none
    bool sampled = false;       // a .timeSamples block (else a plain value)
    std::vector<double> times;  // the sample times, in order
    std::vector<std::size_t> counts;  // elements per sample (a plain value: one entry)
    std::string value;          // a plain value, as written
};

struct Prim {
    std::string type;
    std::map<std::string, std::string> metadata;  // flattened: "customData.groomlab.layoutVersion"
    std::map<std::string, Attribute> attributes;
};

struct Layer {
    std::string firstLine;
    std::map<std::string, std::string> metadata;  // flattened as Prim::metadata
    std::map<std::string, Prim> prims;            // by path
    std::vector<std::string> errors;              // lines the reader did not understand
};

std::string trim(const std::string& s) {
    const std::size_t a = s.find_first_not_of(' ');
    if (a == std::string::npos) return {};
    return s.substr(a, s.find_last_not_of(' ') - a + 1);
}

// The elements of a list "[a, (b, c), d]"'s inside: commas outside parentheses, plus one.
std::size_t elementCount(const std::string& inside) {
    if (trim(inside).empty()) return 0;
    std::size_t n = 1;
    int depth = 0;
    for (const char c : inside) {
        depth += c == '(' ? 1 : c == ')' ? -1 : 0;
        n += (c == ',' && depth == 0) ? 1 : 0;
    }
    return n;
}

Layer readLayer(const std::string& text) {
    Layer layer;
    std::vector<std::string> lines;
    {
        std::istringstream in(text);
        for (std::string line; std::getline(in, line);) lines.push_back(line);
    }
    std::size_t i = 0;
    if (!lines.empty()) layer.firstLine = lines[i++];
    // A metadata block's lines up to the one that is `close`, into `out` with keys under their
    // dictionaries ("customLayerData.groomlab.version").
    auto readMetadata = [&](const std::string& close, std::map<std::string, std::string>& out) {
        static const std::regex open(R"re(^\s*(?:dictionary )?([\w:]+) = \{$)re");
        static const std::regex entry(R"re(^\s*(?:(?:uniform )?[\w\[\]]+ )?([\w:]+) = (.*)$)re");
        std::vector<std::string> scope;
        for (; i < lines.size() && lines[i] != close; ++i) {
            std::smatch m;
            const std::string prefix = [&] {
                std::string p;
                for (const std::string& s : scope) p += s + ".";
                return p;
            }();
            if (std::regex_match(lines[i], m, open)) {
                scope.push_back(m[1]);
            } else if (trim(lines[i]) == "}") {
                if (scope.empty()) layer.errors.push_back(lines[i]);
                else scope.pop_back();
            } else if (std::regex_match(lines[i], m, entry)) {
                out[prefix + m[1].str()] = m[2];
            } else {
                layer.errors.push_back(lines[i]);
            }
        }
        ++i;  // past `close`
    };
    if (i < lines.size() && lines[i] == "(") {
        ++i;
        readMetadata(")", layer.metadata);
    }
    static const std::regex def(R"re(^( *)def (\w+) "(\w+)"( \()?$)re");
    // Only a line's head goes through a regex: libstdc++'s std::regex recurses per character and
    // overflows the stack on a long line (a frame of a scales mesh's points), so the rest of the
    // line is taken by position.
    static const std::regex declaration(R"re(^( *)((?:uniform )?[\w\[\]]+) ([A-Za-z_][\w:]*))re");
    static const std::regex sampleTime(R"re(^ *(-?[0-9.e+-]+): \[)re");
    std::vector<std::pair<std::string, std::size_t>> open;  // prim paths and their indents
    for (; i < lines.size(); ++i) {
        const std::string& line = lines[i];
        std::smatch m;
        if (trim(line).empty()) continue;
        if (std::regex_match(line, m, def)) {
            const std::size_t indent = m[1].length();
            const std::string path = (open.empty() ? std::string() : open.back().first) + "/" + m[3].str();
            Prim& prim = layer.prims[path];
            prim.type = m[2];
            ++i;
            if (m[4].matched) readMetadata(std::string(indent, ' ') + ")", prim.metadata);
            if (i >= lines.size() || lines[i] != std::string(indent, ' ') + "{") layer.errors.push_back("no body: " + line);
            open.emplace_back(path, indent);
            continue;
        }
        if (!open.empty() && line == std::string(open.back().second, ' ') + "}") {
            open.pop_back();
            continue;
        }
        const std::string head = line.substr(0, 256);
        if (open.empty() || !std::regex_search(head, m, declaration)) {
            layer.errors.push_back(line);
            continue;
        }
        Attribute& a = layer.prims[open.back().first].attributes[m[3]];
        if (!a.type.empty() && a.type != m[2].str()) layer.errors.push_back("declared twice, differently: " + line);
        a.type = m[2];
        const std::string indent = m[1];
        std::string rest = line.substr(static_cast<std::size_t>(m.length(0)));
        if (rest == ".timeSamples = {") {
            a.sampled = true;
            for (++i; i < lines.size() && lines[i] != indent + "}"; ++i) {
                std::smatch s;
                const std::string& sampleLine = lines[i];
                const std::string sampleHead = sampleLine.substr(0, 64);
                if (!std::regex_search(sampleHead, s, sampleTime) || sampleLine.size() < 2 + static_cast<std::size_t>(s.length(0)) ||
                    sampleLine.compare(sampleLine.size() - 2, 2, "],") != 0) {
                    layer.errors.push_back(sampleLine);
                    continue;
                }
                a.times.push_back(std::stod(s[1]));
                const std::size_t from = static_cast<std::size_t>(s.length(0));
                a.counts.push_back(elementCount(sampleLine.substr(from, sampleLine.size() - 2 - from)));
            }
            continue;
        }
        const bool metadata = rest.size() >= 2 && rest.compare(rest.size() - 2, 2, " (") == 0;
        if (metadata) rest.resize(rest.size() - 2);
        if (rest.rfind(" = ", 0) == 0) {
            a.value = rest.substr(3);
            if (a.value.size() >= 2 && a.value.front() == '[' && a.value.back() == ']') {
                a.counts.push_back(elementCount(a.value.substr(1, a.value.size() - 2)));
            }
        } else if (!rest.empty()) {
            layer.errors.push_back(line);
        }
        if (metadata) {
            std::map<std::string, std::string> meta;
            ++i;
            readMetadata(indent + ")", meta);
            --i;
            for (const auto& [key, value] : meta) {
                if (key == "interpolation") {
                    a.interpolation = value.size() >= 2 ? value.substr(1, value.size() - 2) : value;
                } else {
                    layer.errors.push_back("unknown attribute metadata: " + key);
                }
            }
        }
    }
    if (!open.empty()) layer.errors.push_back("unclosed prim " + open.back().first);
    return layer;
}

// ---- The spec ------------------------------------------------------------------------------------

struct Spec {
    std::string type;
    std::string interpolation;
    bool sampled = true;
    bool optional = false;  // may be left out (a prototype mesh without normals or uvs)
};
using PrimSpec = std::map<std::string, Spec>;

Spec sampled(const char* type, const char* interpolation = "") { return {type, interpolation, true, false}; }
Spec plain(const char* type, const char* interpolation = "") { return {type, interpolation, false, false}; }
Spec optionalPlain(const char* type, const char* interpolation) { return {type, interpolation, false, true}; }

// The strands' BasisCurves; `uvs`, `color`: whether the scalp had UVs and the hair a color;
// `channels`: name -> interpolation.
PrimSpec strandsSpec(bool uvs, bool color, const std::map<std::string, std::string>& channels, bool hidden = false) {
    PrimSpec s{{"type", plain("uniform token")},
               {"wrap", plain("uniform token")},
               {"curveVertexCounts", sampled("int[]")},
               {"points", sampled("point3f[]")},
               {"extent", sampled("float3[]")},
               {"widths", sampled("float[]", "vertex")},
               {"primvars:strandId", sampled("int64[]", "uniform")}};
    if (uvs) s["primvars:st"] = sampled("texCoord2f[]", "uniform");
    if (color) s["primvars:displayColor"] = sampled("color3f[]", "uniform");
    if (hidden) s["visibility"] = plain("token");
    for (const auto& [name, interpolation] : channels) s["primvars:" + name] = sampled("float[]", interpolation.c_str());
    // Unreal's groom attributes (its USD import's types).
    s["primvars:groom_id"] = sampled("int[]", "uniform");
    if (uvs) s["primvars:groom_root_uv"] = sampled("float2[]", "uniform");
    if (color) s["primvars:groom_color"] = sampled("float3[]", "uniform");
    if (channels.count("clumpId")) s["primvars:groom_clump_id"] = sampled("int[]", "uniform");
    if (channels.count("roughness")) s["primvars:groom_roughness"] = sampled("float[]", "uniform");
    return s;
}

PrimSpec instancerSpec(bool scales) {
    PrimSpec s{{"prototypes", plain("rel")},
               {"protoIndices", sampled("int[]")},
               {"positions", sampled("point3f[]")},
               {"orientations", sampled("quath[]")},
               {"scales", sampled("float3[]")},
               {"ids", sampled("int64[]")}};
    if (scales) {
        s["primvars:scaleRootColor"] = sampled("color3f[]", "vertex");
        s["primvars:scaleId"] = sampled("float[]", "vertex");
        s["primvars:scaleUV"] = sampled("float2[]", "vertex");
        s["primvars:scaleVariant"] = sampled("int[]", "vertex");
    }
    return s;
}

PrimSpec prototypeMeshSpec() {
    return {{"subdivisionScheme", plain("uniform token")},
            {"faceVertexCounts", plain("int[]")},
            {"faceVertexIndices", plain("int[]")},
            {"points", plain("point3f[]")},
            {"extent", plain("float3[]")},
            {"normals", optionalPlain("normal3f[]", "vertex")},
            {"primvars:st", optionalPlain("texCoord2f[]", "vertex")}};
}

PrimSpec scaleMeshSpec() {
    return {{"subdivisionScheme", plain("uniform token")},
            {"faceVertexCounts", sampled("int[]")},
            {"faceVertexIndices", sampled("int[]")},
            {"points", sampled("point3f[]")},
            {"extent", sampled("float3[]")},
            {"normals", sampled("normal3f[]", "vertex")},
            {"primvars:st", sampled("texCoord2f[]", "vertex")},
            {"primvars:displayColor", sampled("color3f[]", "vertex")},
            {"primvars:scaleColor", sampled("color3f[]", "vertex")},
            {"primvars:scaleRootColor", sampled("color3f[]", "uniform")},
            {"primvars:scaleId", sampled("float[]", "uniform")},
            {"primvars:scaleUV", sampled("float2[]", "uniform")},
            {"primvars:scaleVariant", sampled("int[]", "uniform")}};
}

struct LayerSpec {
    std::string root;
    std::vector<double> frames;
    double metersPerUnit = 0.01;
    std::string upAxis = "Y";
    double framesPerSecond = 24.0;
    std::map<std::string, std::pair<std::string, PrimSpec>> prims;  // path -> type, attributes
};

std::string number(double v) {
    std::ostringstream s;
    s << v;
    return s.str();
}

// Compares `layer` with `spec`; every difference is a failure naming the spec page.
void expectLayout(const std::string& text, const LayerSpec& spec) {
    SCOPED_TRACE(std::string("the USD layout changed: update ") + kSpec +
                 " and tests/engine/core/test_usd_layout.cpp together (a rename, retype or new meaning also raises "
                 "kUsdLayoutVersion in usd_writer.h)");
    const Layer layer = readLayer(text);
    EXPECT_TRUE(layer.errors.empty()) << "lines the layout reader does not know:\n  " << [&] {
        std::string all;
        for (const std::string& e : layer.errors) all += e + "\n  ";
        return all;
    }();
    EXPECT_EQ(layer.firstLine, "#usda 1.0");

    // Layer metadata.
    const std::map<std::string, std::string> want{
        {"defaultPrim", "\"" + spec.root + "\""},
        {"customLayerData.groomlab.layoutVersion", std::to_string(kUsdLayoutVersion)},
        {"customLayerData.groomlab.version", "\"" + std::string(kVersionString) + "\""},
        {"startTimeCode", number(spec.frames.front())},
        {"endTimeCode", number(spec.frames.back())},
        {"framesPerSecond", number(spec.framesPerSecond)},
        {"timeCodesPerSecond", number(spec.framesPerSecond)},
        {"metersPerUnit", number(spec.metersPerUnit)},
        {"upAxis", "\"" + spec.upAxis + "\""},
    };
    std::set<std::string> keys{"doc"};
    for (const auto& [key, value] : want) {
        keys.insert(key);
        const auto it = layer.metadata.find(key);
        if (it == layer.metadata.end()) {
            ADD_FAILURE() << "layer metadata " << key << " is missing";
        } else {
            EXPECT_EQ(it->second, value) << "layer metadata " << key;
        }
    }
    for (const auto& [key, value] : layer.metadata) {
        EXPECT_TRUE(keys.count(key)) << "layer metadata " << key << " = " << value << " is not in the spec";
    }
    const auto doc = layer.metadata.find("doc");
    EXPECT_TRUE(doc != layer.metadata.end() && doc->second.rfind("\"GroomLab " + std::string(kVersionString), 0) == 0)
        << "the layer's doc starts \"GroomLab <version>\"";

    // The root prim and its marker.
    const std::string rootPath = "/" + spec.root;
    const auto root = layer.prims.find(rootPath);
    ASSERT_NE(root, layer.prims.end()) << "no root prim " << rootPath;
    EXPECT_EQ(root->second.type, "Xform");
    const std::map<std::string, std::string> rootMetadata{
        {"customData.groomlab.layoutVersion", std::to_string(kUsdLayoutVersion)},
        {"customData.groomlab.version", "\"" + std::string(kVersionString) + "\""},
        {"kind", "\"component\""},
        {"apiSchemas", "[\"GroomAPI\"]"}};  // Unreal's: its USD importer makes a groom of the curves under it
    EXPECT_EQ(root->second.metadata, rootMetadata) << "the root prim's metadata";
    EXPECT_TRUE(root->second.attributes.empty()) << "the root prim has no attributes";

    // Every prim, every attribute, both ways.
    for (const auto& [path, prim] : layer.prims) {
        if (path == rootPath) continue;
        const auto expected = spec.prims.find(path);
        if (expected == spec.prims.end()) {
            ADD_FAILURE() << "prim " << path << " (" << prim.type << ") is not in the spec";
            continue;
        }
        EXPECT_EQ(prim.type, expected->second.first) << path;
        for (const auto& [name, a] : prim.attributes) {
            const auto s = expected->second.second.find(name);
            if (s == expected->second.second.end()) {
                ADD_FAILURE() << path << "." << name << " (" << a.type << ") is not in the spec";
                continue;
            }
            EXPECT_EQ(a.type, s->second.type) << path << "." << name;
            EXPECT_EQ(a.interpolation, s->second.interpolation) << path << "." << name << " interpolation";
            EXPECT_EQ(a.sampled, s->second.sampled) << path << "." << name << (s->second.sampled ? " is time-sampled" : " is a plain value");
            if (a.sampled) EXPECT_EQ(a.times, spec.frames) << path << "." << name << ": one sample per frame written";
        }
        for (const auto& [name, s] : expected->second.second) {
            EXPECT_TRUE(s.optional || prim.attributes.count(name)) << path << "." << name << " (" << s.type << ") is missing";
        }
    }
    for (const auto& [path, prim] : spec.prims) {
        EXPECT_TRUE(layer.prims.count(path)) << "prim " << path << " (" << prim.first << ") is missing";
    }
}

// The sizes a frame's arrays must have: uniform per curve (or face, or instance), vertex per point.
void expectCounts(const std::string& text, const std::string& prim, std::size_t uniform, std::size_t vertex) {
    const Layer layer = readLayer(text);
    const auto it = layer.prims.find(prim);
    ASSERT_NE(it, layer.prims.end()) << prim;
    for (const auto& [name, a] : it->second.attributes) {
        if (!a.sampled || a.interpolation.empty()) continue;
        const std::size_t want = a.interpolation == "uniform" ? uniform : vertex;
        for (const std::size_t n : a.counts) EXPECT_EQ(n, want) << prim << "." << name << " (" << a.interpolation << ")";
    }
}

// ---- Grooms ----------------------------------------------------------------------------------------

std::shared_ptr<const ScalpMesh> uvSphere(float radius) {
    auto mesh = std::make_shared<ScalpMesh>(*test::makeSphere(12, 24, radius));
    mesh->setCornerUVs(std::vector<ScalpMesh::UV>(mesh->triangleCount() * 3, {0.25f, 0.75f}));
    return mesh;
}

// Three strands with a per-strand color (melanin), the RGB per strand and per point (a tip color:
// colorR and colorR_2), a clump id, a per-point wetness, and an Instance node's leaf on one.
StrandSet channelStrands() {
    StrandSet s;
    s.addStrand(11, {{0, 1, 0}, {0, 2, 0}, {0, 3, 0}}, {0.01f, 0.008f, 0.002f});
    s.addStrand(12, {{1, 1, 0}, {1, 2, 0}}, {0.01f, 0.002f});
    s.addStrand(13, {{0, 1, 1}, {0, 2, 1}, {0, 3, 1}, {0, 4, 1}});
    s.addStrandChannel("melanin", 0.3f);
    s.addStrandChannel("clumpId", 2.0f);
    for (const char* c : {"colorR", "colorG", "colorB"}) {
        s.addStrandChannel(c, 0.5f);
        s.addPointChannel(c, 0.4f);
    }
    s.addPointChannel("wetness", 0.0f);
    auto instances = std::make_shared<InstanceSet>();
    instances->prototypeIndex("leaf");
    InstanceSet::Instance leaf;
    leaf.strand = 11;
    leaf.id = 5;
    leaf.t = 0.5f;
    instances->instances.push_back(leaf);
    s.setInstances(instances);
    return s;
}

const std::map<std::string, std::string> kChannelPrimvars{
    {"clumpId", "uniform"}, {"colorB", "uniform"},   {"colorG", "uniform"},   {"colorR", "uniform"},
    {"melanin", "uniform"}, {"colorB_2", "vertex"}, {"colorG_2", "vertex"}, {"colorR_2", "vertex"},
    {"wetness", "vertex"}};

// Scatter, Width, HairColor, Scales on a sphere with uvs: a scaled body.
struct ScaledBody {
    Graph g;
    ExternalInputs ext;
    MeshPtr skin = uvSphere(2.0f);
    NodeId out = 0;
    ScaledBody() {
        ext.set("scalp", skin, skin->surfaceHash());
        const NodeId scalp = g.addNode(op::kScalp), scatter = g.addNode(op::kScatter), w = g.addNode(op::kWidth);
        const NodeId color = g.addNode(op::kHairColor);
        out = g.addNode(op::kScales);
        g.node(scatter).params().set("density", 2.0);
        g.node(w).params().set("width", 0.3);
        g.node(out).params().set("prototype", std::string("groomlab:scale"));
        g.connect(scalp, scatter, 0);
        g.connect(scatter, w, 0);
        g.connect(w, color, 0);
        g.connect(color, out, 0);
    }
    StrandsPtr eval() {
        EvalContext ctx;
        ctx.externals = &ext;
        return g.evaluateStrands(out, ctx);
    }
};

fs::path tempLayer(const char* name) { return fs::temp_directory_path() / name; }

// ---- Tests ------------------------------------------------------------------------------------------

TEST(UsdLayout, StrandsChannelsAndInstancesOneFrameAndARange) {
    const StrandSet strands = channelStrands();
    const auto scalp = uvSphere(1.0f);
    for (const std::vector<double>& frames : {std::vector<double>{7.0}, std::vector<double>{1.0, 1.5, 2.0}}) {
        SCOPED_TRACE(frames.size() == 1 ? "one frame" : "a range of frames");
        const fs::path path = tempLayer("groomlab_usd_layout_strands.usda");
        UsdCurvesWriter::Options options;
        options.framesPerSecond = 25.0;
        options.metersPerUnit = 0.001;
        options.upAxis = "Z";
        {
            UsdCurvesWriter w(path.u8string(), "hero:hair", options);
            for (const double f : frames) w.writeFrame(f, strands, scalp.get());
        }
        const std::string text = readText(path);
        fs::remove(path);
        LayerSpec spec;
        spec.root = "hero_hair";
        spec.frames = frames;
        spec.metersPerUnit = 0.001;
        spec.upAxis = "Z";
        spec.framesPerSecond = 25.0;
        spec.prims["/hero_hair/strands"] = {"BasisCurves", strandsSpec(true, true, kChannelPrimvars)};
        spec.prims["/hero_hair/instances"] = {"PointInstancer", instancerSpec(false)};
        spec.prims["/hero_hair/instances/prototypes"] = {"Scope", {}};
        spec.prims["/hero_hair/instances/prototypes/leaf"] = {"Xform", {}};  // a placeholder to reference into
        expectLayout(text, spec);
        expectCounts(text, "/hero_hair/strands", strands.strandCount(), strands.pointCount());

        const Layer layer = readLayer(text);
        const Prim& curves = layer.prims.at("/hero_hair/strands");
        EXPECT_EQ(curves.attributes.at("type").value, "\"linear\"");
        EXPECT_EQ(curves.attributes.at("wrap").value, "\"nonperiodic\"");
        // Unreal's importer takes it as a groom (its GroomAPI applied to the groom's Xform).
        EXPECT_NE(text.find("    kind = \"component\"\n    prepend apiSchemas = [\"GroomAPI\"]\n)"), std::string::npos);
        EXPECT_EQ(curves.attributes.at("extent").counts.front(), 2u);
        EXPECT_EQ(curves.attributes.at("points").counts.front(), strands.pointCount());
        EXPECT_EQ(layer.prims.at("/hero_hair/instances").attributes.at("prototypes").value,
                  "[</hero_hair/instances/prototypes/leaf>]");
        // widths are diameters: the strand's own widths, as they are.
        EXPECT_NE(text.find("float[] widths.timeSamples = {\n        " + number(frames.front()) + ": [0.01, 0.008, 0.002, 0.01, 0.002, 1, 1, 1, 1]"),
                  std::string::npos);
    }
}

TEST(UsdLayout, WithoutUvsColorOrChannelsTheirPrimvarsAreLeftOut) {
    StrandSet strands;
    strands.addStrand(1, {{0, 0, 0}, {0, 1, 0}});
    const fs::path path = tempLayer("groomlab_usd_layout_plain.usda");
    {
        UsdCurvesWriter w(path.u8string(), "", {});  // no name: "groom"
        w.writeFrame(1.0, strands);
    }
    const std::string text = readText(path);
    fs::remove(path);
    LayerSpec spec;
    spec.root = "groom";
    spec.frames = {1.0};
    spec.prims["/groom/strands"] = {"BasisCurves", strandsSpec(false, false, {})};
    expectLayout(text, spec);
}

TEST(UsdLayout, ScalesAsMeshInstancerBothOrNone) {
    ScaledBody body;
    const StrandsPtr strands = body.eval();
    ASSERT_TRUE(strands && strands->strandCount() > 10);
    const auto mesh = buildScaleMesh(*strands, body.skin);
    ASSERT_TRUE(mesh && !mesh->scales.empty());
    std::map<std::string, std::string> channels;
    for (const std::string& c : strands->strandChannelNames()) channels[c] = "uniform";
    for (const std::string& c : strands->pointChannelNames()) channels[channels.count(c) ? c + "_2" : c] = "vertex";
    using Mode = UsdCurvesWriter::Options::Scales;
    for (const Mode mode : {Mode::Mesh, Mode::Instancer, Mode::Both, Mode::None}) {
        const bool meshed = mode == Mode::Mesh || mode == Mode::Both, instanced = mode == Mode::Instancer || mode == Mode::Both;
        for (const std::vector<double>& frames : {std::vector<double>{3.0}, std::vector<double>{3.0, 4.0}}) {
            SCOPED_TRACE(std::string("scales ") +
                         (mode == Mode::Mesh ? "mesh" : mode == Mode::Instancer ? "instancer" : mode == Mode::Both ? "both" : "none") +
                         (frames.size() == 1 ? ", one frame" : ", two frames"));
            const fs::path path = tempLayer("groomlab_usd_layout_scales.usda");
            UsdCurvesWriter::Options options;
            options.scales = mode;
            {
                UsdCurvesWriter w(path.u8string(), "dragon", options);
                for (const double f : frames) w.writeFrame(f, *strands, body.skin.get(), mesh.get());
            }
            const std::string text = readText(path);
            fs::remove(path);
            LayerSpec spec;
            spec.root = "dragon";
            spec.frames = frames;
            spec.prims["/dragon/strands"] = {"BasisCurves", strandsSpec(true, true, channels, true)};
            if (meshed) spec.prims["/dragon/scales"] = {"Mesh", scaleMeshSpec()};
            if (instanced) {
                spec.prims["/dragon/scaleInstances"] = {"PointInstancer", instancerSpec(true)};
                spec.prims["/dragon/scaleInstances/prototypes"] = {"Scope", {}};
                spec.prims["/dragon/scaleInstances/prototypes/groomlab_scale"] = {"Mesh", prototypeMeshSpec()};
            }
            expectLayout(text, spec);
            if (meshed) expectCounts(text, "/dragon/scales", mesh->triangleCount(), mesh->vertexCount());
            if (instanced) expectCounts(text, "/dragon/scaleInstances", mesh->scales.size(), mesh->scales.size());
        }
    }
}

// The spec page names everything the tests above hold the writer to, and shows this version.
TEST(UsdLayout, TheSpecPageNamesEveryPrimAndAttribute) {
    const fs::path page = fs::path(GROOMLAB_SOURCE_ROOT) / kSpec;
    const std::string doc = readText(page);
    ASSERT_FALSE(doc.empty()) << "cannot read " << page.u8string();
    std::set<std::string> names{"strands", "instances", "prototypes", "scales", "scaleInstances", "layoutVersion",
                                "customLayerData", "customData", "defaultPrim", "metersPerUnit", "upAxis",
                                "startTimeCode", "endTimeCode", "timeCodesPerSecond", "framesPerSecond", "visibility"};
    for (const PrimSpec& prim : {strandsSpec(true, true, {}), instancerSpec(true), prototypeMeshSpec(), scaleMeshSpec()}) {
        for (const auto& entry : prim) names.insert(entry.first);
    }
    for (const std::string& name : names) {
        EXPECT_NE(doc.find("`" + name + "`"), std::string::npos) << kSpec << " does not mention `" << name << "`";
    }
    EXPECT_NE(doc.find("int layoutVersion = " + std::to_string(kUsdLayoutVersion)), std::string::npos)
        << kSpec << "'s example shows another layout version than kUsdLayoutVersion (" << kUsdLayoutVersion << ")";
}

}  // namespace
}  // namespace groomlab
