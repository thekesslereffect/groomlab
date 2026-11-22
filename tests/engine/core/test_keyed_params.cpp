// SPDX-License-Identifier: Apache-2.0
// Keyed params (Graph::setParamControls, "paramControls" in the graph JSON): a node's number,
// switch, bool or vec3 param takes the host's control values in place, so a host keys it
// without a Control node. Params that set counts, segments or seeds are not animatable.

#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include "groomlab/curve.h"
#include "groomlab/frame_time.h"
#include "groomlab/field.h"
#include "groomlab/graph_io.h"
#include "groomlab/groom.h"
#include "groomlab/operators.h"
#include "test_helpers.h"

namespace groomlab {
namespace {

// A source whose one strand spells its params: x from "amount", y from "count", z from "on",
// and a second point at "dir".
class ParamsOp : public Operator {
public:
    std::string typeName() const override { return "TestParams"; }
    std::vector<PortSpec> inputs() const override { return {}; }
    std::vector<ParamSpec> paramSpecs() const override {
        return {
            {"amount", 1.0, "a number"},
            {"count", std::int64_t{0}, "a switch"},
            {"on", false, "a toggle"},
            {"dir", Vec3f(0.0f, 1.0f, 0.0f), "a vector"},
            {"label", std::string("x"), "text"},
            notAnimatable({"seed", std::int64_t{0}, "Random seed"}),
        };
    }
    Value evaluate(const EvalContext&, const std::vector<Value>&) const override {
        auto out = std::make_shared<StrandSet>();
        const Vec3f p(static_cast<float>(params().getDouble("amount")), static_cast<float>(params().getInt("count")),
                      params().getBool("on") ? 1.0f : 0.0f);
        out->addStrand(1, {p, params().getVec3("dir")});
        return StrandsPtr(out);
    }
};

EvalContext withControls(ExternalInputs& ext, const std::map<std::string, float>& controls) {
    setControlExternals(ext, controls);
    EvalContext ctx;
    ctx.externals = &ext;
    return ctx;
}

TEST(KeyedParams, TakeTheHostsControlsElseTheirOwnValue) {
    Graph g;
    const NodeId n = g.addNode(std::make_unique<ParamsOp>());
    g.node(n).params().set("amount", 2.0);
    g.setParamControls(n, {{"amount", {"a"}}, {"count", {"c"}}, {"on", {"o"}}, {"dir", {"dx", "dy", "dz"}}});

    ExternalInputs frame1, frame2;
    StrandsPtr s = g.evaluateStrands(n, withControls(frame1, {{"a", 0.5f}, {"c", 1.6f}, {"o", 1.0f},
                                                             {"dx", 1.0f}, {"dy", 2.0f}, {"dz", 3.0f}}));
    EXPECT_FLOAT_EQ(s->strandPoints(0)[0].x, 0.5f);
    EXPECT_FLOAT_EQ(s->strandPoints(0)[0].y, 2.0f);  // an int rounds: stepped
    EXPECT_FLOAT_EQ(s->strandPoints(0)[0].z, 1.0f);
    EXPECT_FLOAT_EQ(s->strandPoints(0)[1].z, 3.0f);
    s = g.evaluateStrands(n, withControls(frame2, {{"a", 0.75f}, {"c", 1.4f}, {"o", 0.0f},
                                                   {"dx", 1.0f}, {"dy", 2.0f}, {"dz", 3.0f}}));
    EXPECT_FLOAT_EQ(s->strandPoints(0)[0].x, 0.75f);
    EXPECT_FLOAT_EQ(s->strandPoints(0)[0].y, 1.0f);
    EXPECT_FLOAT_EQ(s->strandPoints(0)[0].z, 0.0f);
    // No controls: the params' own values (a vector needs all three).
    ExternalInputs partial;
    s = g.evaluateStrands(n, withControls(partial, {{"dx", 5.0f}}));
    EXPECT_FLOAT_EQ(s->strandPoints(0)[0].x, 2.0f);
    EXPECT_FLOAT_EQ(s->strandPoints(0)[0].y, 0.0f);
    EXPECT_FLOAT_EQ(s->strandPoints(0)[1].x, 0.0f);
    EXPECT_FLOAT_EQ(s->strandPoints(0)[1].y, 1.0f);
    // Stored, a keyed param keeps its own value.
    EXPECT_DOUBLE_EQ(g.storedParams(n).getDouble("amount"), 2.0);
}

TEST(KeyedParams, TheValueKeysTheCache) {
    Graph g;
    const NodeId n = g.addNode(std::make_unique<ParamsOp>());
    g.setParamControls(n, {{"amount", {"a"}}});
    ExternalInputs a1, a2, a3;
    g.evaluateStrands(n, withControls(a1, {{"a", 0.5f}}));
    g.evaluateStrands(n, withControls(a2, {{"a", 0.5f}}));  // another frame, the same value: cached
    EXPECT_EQ(g.evaluationCount(), 1u);
    g.evaluateStrands(n, withControls(a3, {{"a", 0.7f}}));
    EXPECT_EQ(g.evaluationCount(), 2u);
}

TEST(KeyedParams, AnEditToTheOwnValueSticks) {
    // Set from outside after an evaluation (an edit, a compound's exposed value): the new own value.
    Graph g;
    const NodeId n = g.addNode(std::make_unique<ParamsOp>());
    g.setParamControls(n, {{"amount", {"a"}}});
    ExternalInputs keyed, none;
    g.evaluateStrands(n, withControls(keyed, {{"a", 0.5f}}));
    g.node(n).params().set("amount", 3.0);
    EXPECT_FLOAT_EQ(g.evaluateStrands(n, withControls(none, {}))->strandPoints(0)[0].x, 3.0f);
    // Unlinked after an evaluation: back to its own value.
    g.evaluateStrands(n, withControls(keyed, {{"a", 0.5f}}));
    g.setParamControls(n, {});
    EXPECT_DOUBLE_EQ(g.node(n).params().getDouble("amount"), 3.0);
}

TEST(KeyedParams, OnlyAnimatableNumbersAndVectors) {
    Graph g;
    const NodeId n = g.addNode(std::make_unique<ParamsOp>());
    EXPECT_THROW(g.setParamControls(n, {{"seed", {"s"}}}), GraphError);   // not animatable
    EXPECT_THROW(g.setParamControls(n, {{"label", {"l"}}}), GraphError);  // text
    EXPECT_THROW(g.setParamControls(n, {{"nope", {"n"}}}), GraphError);
    EXPECT_THROW(g.setParamControls(n, {{"dir", {"d"}}}), GraphError);    // a vector takes three
    EXPECT_THROW(g.setParamControls(n, {{"amount", {"a", "b", "c"}}}), GraphError);
    EXPECT_THROW(g.setParamControls(n, {{"amount", {""}}}), GraphError);
    EXPECT_TRUE(g.paramControls(n).empty());
}

TEST(KeyedParams, TheSchemaSaysWhatCanBeKeyed) {
    const auto ops = nlohmann::json::parse(operatorSpecsJson());
    auto animatable = [&](const std::string& type, const std::string& param) {
        for (const auto& op : ops) {
            if (op.at("type") != type) continue;
            for (const auto& p : op.at("params")) {
                if (p.at("name") == param) return p.at("animatable").get<bool>();
            }
        }
        ADD_FAILURE() << type << "." << param << " not found";
        return false;
    };
    // Strand and point counts, card topology and random layouts stay put.
    EXPECT_FALSE(animatable("Scatter", "density"));
    EXPECT_FALSE(animatable("Resample", "segments"));
    EXPECT_FALSE(animatable("Cull", "keep"));
    EXPECT_FALSE(animatable("Cards", "segments"));
    EXPECT_FALSE(animatable("Propagate", "count"));
    EXPECT_FALSE(animatable("Instance", "count"));
    // Everything else keys.
    EXPECT_TRUE(animatable("Length", "keep"));
    EXPECT_TRUE(animatable("Switch", "index"));
    EXPECT_TRUE(animatable("Blend", "mix"));
    EXPECT_TRUE(animatable("Wind", "direction"));
    EXPECT_TRUE(animatable("HairColor", "tipColor"));
    // Every seed is layout.
    for (const auto& op : ops) {
        for (const auto& p : op.at("params")) {
            if (p.at("name") == "seed") EXPECT_FALSE(p.at("animatable").get<bool>()) << op.at("type");
        }
    }
    // Colors say so (hosts key them as color attributes).
    for (const auto& op : ops) {
        if (op.at("type") != "HairColor") continue;
        for (const auto& p : op.at("params")) {
            if (p.at("name") == "tipColor" || p.at("name") == "dye") EXPECT_EQ(p.at("unit"), "color");
        }
    }
}

// Scalp -> Scatter -> Transform (its scale keyed) -> Output, with a Wind whose direction is keyed.
std::string keyedGraph(const std::string& lengthLinks) {
    return R"j({"version": 1, "output": "out", "nodes": [
        {"name": "scalp", "type": "Scalp"},
        {"name": "scatter", "type": "Scatter", "params": {"density": 100.0}},
        {"name": "length", "type": "Transform", "params": {"scale": 1.0})j" + lengthLinks + R"j(},
        {"name": "out", "type": "Output"}],
      "connections": [{"from": "scalp", "to": "scatter", "input": "scalp"},
        {"from": "scatter", "to": "length", "input": "strands"}, {"from": "length", "to": "out", "input": "strands"}]})j";
}

float strandLength(const StrandSet& s, std::size_t i) { return arcLength(s.strandPoints(i), s.strandPointCount(i)); }

TEST(KeyedParams, AGroomKeysAParamFromItsControls) {
    GroomData d;
    d.scalp = test::makePlane(8);
    d.graphJson = keyedGraph(R"j(, "paramControls": {"scale": "length_keep"})j");
    Groom groom(d);
    const float own = strandLength(*groom.evaluate(1.0f, 0.0), 0);
    groom.setControls({{"length_keep", 0.5f}});
    const StrandsPtr half = groom.evaluate(1.0f, 1.0);
    EXPECT_NEAR(strandLength(*half, 0), own * 0.5f, 1e-3f);
    groom.setControls({{"length_keep", 2.0f}});
    const StrandsPtr twice = groom.evaluate(1.0f, 2.0);
    EXPECT_NEAR(strandLength(*twice, 0), own * 2.0f, 1e-3f);
    EXPECT_EQ(half->strandCount(), twice->strandCount());  // the topology stays
}

TEST(KeyedParams, AGroomsControlKeysAnimateItWhereverItIsEvaluated) {
    // A .groom's controlKeys (the app's param keys): the session takes each keyed control's value at
    // the frame evaluated, over the value set, so the CLI, Python and every host animate it alike.
    GroomData d;
    d.scalp = test::makePlane(8);
    d.graphJson = keyedGraph(R"j(, "paramControls": {"scale": "length_keep"})j");
    Groom groom(d);
    const float own = strandLength(*groom.evaluate(1.0f, 0.0), 0);
    groom.setFramesPerSecond(24.0);
    groom.setControls({{"length_keep", 3.0f}});  // (a keyed control: its keys win)
    groom.setControlKeys({{"length_keep", {{1.0, 0.5}, {25.0, 2.0}}}});
    EXPECT_EQ(groom.controlKeys().size(), 1u);
    EXPECT_NEAR(strandLength(*groom.evaluate(1.0f, frameToSeconds(1.0, 24.0)), 0), own * 0.5f, 1e-3f);
    EXPECT_NEAR(strandLength(*groom.evaluate(1.0f, frameToSeconds(25.0, 24.0)), 0), own * 2.0f, 1e-3f);
    const float between = strandLength(*groom.evaluate(1.0f, frameToSeconds(13.0, 24.0)), 0);
    EXPECT_GT(between, own * 0.5f);
    EXPECT_LT(between, own * 2.0f);
    // No keys: the value set again.
    groom.setControlKeys({});
    EXPECT_NEAR(strandLength(*groom.evaluate(1.0f, frameToSeconds(13.0, 24.0)), 0), own * 3.0f, 1e-3f);
}

TEST(KeyedParams, TheJsonKeepsLinksAndOwnValues) {
    GraphDocument doc = loadGraphJson(keyedGraph(R"j(, "paramControls": {"scale": "len"})j"));
    ExternalInputs ext;
    ext.set("scalp", test::makePlane(4), 1);
    setControlExternals(ext, {{"len", 0.25f}});
    EvalContext ctx;
    ctx.externals = &ext;
    doc.graph.evaluateStrands(doc.output, ctx);
    const auto saved = nlohmann::json::parse(saveGraphJson(doc));
    for (const auto& n : saved.at("nodes")) {
        if (n.at("name") != "length") continue;
        EXPECT_EQ(n.at("paramControls").at("scale"), "len");
        EXPECT_DOUBLE_EQ(n.at("params").at("scale").get<double>(), 1.0);  // its own value, not the key's
    }
    // A vector names three controls; a param that must not be keyed is an error naming the node.
    const std::string wind = R"j({"version": 1, "output": "w", "nodes": [
        {"name": "w", "type": "Wind", "paramControls": {"direction": ["wx", "wy", "wz"]}}], "connections": []})j";
    EXPECT_NO_THROW(loadGraphJson(wind));
    try {
        loadGraphJson(R"j({"version": 1, "output": "s", "nodes": [
            {"name": "s", "type": "Scatter", "paramControls": {"density": "d"}}], "connections": []})j");
        FAIL() << "density keyed";
    } catch (const GraphError& e) {
        EXPECT_NE(std::string(e.what()).find("node 's'"), std::string::npos) << e.what();
        EXPECT_NE(std::string(e.what()).find("cannot be keyed"), std::string::npos) << e.what();
    }
    EXPECT_THROW(loadGraphJson(R"j({"version": 1, "output": "w", "nodes": [
        {"name": "w", "type": "Wind", "paramControls": {"direction": "d"}}], "connections": []})j"), GraphError);
    EXPECT_THROW(loadGraphJson(R"j({"version": 1, "output": "w", "nodes": [
        {"name": "w", "type": "Wind", "paramControls": {"strength": 3}}], "connections": []})j"), GraphError);
}

TEST(KeyedParams, KeyedInsideACompound) {
    // A keyed param inside a compound: the compound re-evaluates as the control changes.
    GroomData d;
    d.scalp = test::makePlane(8);
    d.graphJson = R"j({"version": 1, "output": "out", "nodes": [
        {"name": "scalp", "type": "Scalp"},
        {"name": "scatter", "type": "Scatter", "params": {"density": 100.0}},
        {"name": "grow", "type": "Compound", "params": {"definition": {
            "inputs": [{"name": "strands", "type": "Strands"}],
            "graph": {"version": 1, "output": "length", "nodes": [
                {"name": "in", "type": "Input", "params": {"name": "strands"}},
                {"name": "length", "type": "Transform", "params": {}, "paramControls": {"scale": "g"}}],
              "connections": [{"from": "in", "to": "length", "input": "strands"}]}}}},
        {"name": "out", "type": "Output"}],
      "connections": [{"from": "scalp", "to": "scatter", "input": "scalp"},
        {"from": "scatter", "to": "grow", "input": "strands"}, {"from": "grow", "to": "out", "input": "strands"}]})j";
    Groom groom(d);
    groom.setControls({{"g", 1.0f}});
    const float one = strandLength(*groom.evaluate(1.0f, 0.0), 0);
    groom.setControls({{"g", 3.0f}});
    EXPECT_NEAR(strandLength(*groom.evaluate(1.0f, 1.0), 0), one * 3.0f, 1e-3f);
}

TEST(KeyedParams, ASwitchFollowsItsKeyedIndexWhenWalked) {
    // The walk hosts use for the hair look and cards follows a keyed Switch after an evaluation.
    GraphDocument doc = loadGraphJson(R"j({"version": 1, "output": "out", "nodes": [
        {"name": "scalp", "type": "Scalp"},
        {"name": "scatter", "type": "Scatter", "params": {"density": 50.0}},
        {"name": "a", "type": "Length"}, {"name": "b", "type": "Width"},
        {"name": "pick", "type": "Switch", "paramControls": {"index": "pick"}},
        {"name": "out", "type": "Output"}],
      "connections": [{"from": "scalp", "to": "scatter", "input": "scalp"},
        {"from": "scatter", "to": "a", "input": "strands"}, {"from": "scatter", "to": "b", "input": "strands"},
        {"from": "a", "to": "pick", "input": "a"}, {"from": "b", "to": "pick", "input": "b"},
        {"from": "pick", "to": "out", "input": "strands"}]})j");
    auto walked = [&](float index) {
        ExternalInputs ext;
        ext.set("scalp", test::makePlane(4), 1);
        setControlExternals(ext, {{"pick", index}});
        EvalContext ctx;
        ctx.externals = &ext;
        doc.graph.evaluateStrands(doc.output, ctx);
        std::vector<std::string> names;
        doc.graph.forEachActiveUpstream(doc.output, [&](const std::string& path, const Operator&) {
            names.push_back(path);
            return true;
        });
        return names;
    };
    const auto first = walked(0.0f);
    EXPECT_NE(std::find(first.begin(), first.end(), "a"), first.end());
    EXPECT_EQ(std::find(first.begin(), first.end(), "b"), first.end());
    const auto second = walked(1.0f);
    EXPECT_NE(std::find(second.begin(), second.end(), "b"), second.end());
    EXPECT_EQ(std::find(second.begin(), second.end(), "a"), second.end());
}

}  // namespace
}  // namespace groomlab
