// SPDX-License-Identifier: Apache-2.0
// Compound nodes (PG-6).

#include <gtest/gtest.h>

#include <atomic>
#include <map>
#include <set>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "groomlab/cards.h"
#include "groomlab/graph_edit.h"
#include "groomlab/graph_io.h"
#include "groomlab/groom.h"
#include "groomlab/operators.h"
#include "test_helpers.h"

namespace groomlab {
namespace {

using json = nlohmann::json;

// scatter -> [noise -> curl] -> output, inline or as a compound.
json baseGraph() {
    return json::parse(R"j({"version": 1, "output": "out",
      "nodes": [{"name": "scalp", "type": "Scalp"},
                {"name": "scatter", "type": "Scatter", "params": {"density": 200.0}},
                {"name": "out", "type": "Output"}],
      "connections": [{"from": "scalp", "to": "scatter", "input": "scalp"}]})j");
}

json inlineGraph(double amplitude) {
    json g = baseGraph();
    g["nodes"].push_back({{"name", "noise"}, {"type", "Noise"}, {"params", {{"amplitude", amplitude}}}});
    g["nodes"].push_back({{"name", "curl"}, {"type", "Curl"}});
    g["connections"].push_back({{"from", "scatter"}, {"to", "noise"}, {"input", "strands"}});
    g["connections"].push_back({{"from", "noise"}, {"to", "curl"}, {"input", "strands"}});
    g["connections"].push_back({{"from", "curl"}, {"to", "out"}, {"input", "strands"}});
    return g;
}

json frizzyDefinition() {
    return json::parse(R"j({
      "graph": {"version": 1, "output": "curl",
        "nodes": [{"name": "in", "type": "Input", "params": {"name": "strands", "type": "Strands"}},
                  {"name": "noise", "type": "Noise", "params": {"amplitude": 0.05}},
                  {"name": "curl", "type": "Curl"}],
        "connections": [{"from": "in", "to": "noise", "input": "strands"},
                        {"from": "noise", "to": "curl", "input": "strands"}]},
      "inputs": [{"name": "strands", "type": "Strands"}],
      "expose": {"amount": "noise.amplitude"}})j");
}

json compoundGraph(const json& params) {
    json g = baseGraph();
    json p = params;
    p["definition"] = frizzyDefinition();
    g["nodes"].push_back({{"name", "frizzy"}, {"type", "Compound"}, {"params", p}});
    g["connections"].push_back({{"from", "scatter"}, {"to", "frizzy"}, {"input", "strands"}});
    g["connections"].push_back({{"from", "frizzy"}, {"to", "out"}, {"input", "strands"}});
    return g;
}

StrandsPtr eval(const std::string& graph, MeshPtr scalp = test::makeSphere(12, 24, 1.0f)) {
    GroomData d;
    d.scalp = std::move(scalp);
    d.graphJson = graph;
    return Groom(d).evaluate(1.0f, 0.0);
}

TEST(Compound, MatchesTheSameNodesInline) {
    const StrandsPtr a = eval(inlineGraph(0.05).dump());
    const StrandsPtr b = eval(compoundGraph(json::object()).dump());
    ASSERT_GT(a->strandCount(), 100u);
    EXPECT_EQ(a->positions(), b->positions());

    // Exposed params set the inner node.
    const StrandsPtr c = eval(inlineGraph(0.2).dump());
    const StrandsPtr d = eval(compoundGraph({{"amount", 0.2}}).dump());
    EXPECT_EQ(c->positions(), d->positions());
    EXPECT_NE(c->positions(), a->positions());
}

// A node that counts how often it runs, handing its strands on.
std::atomic<int> gCountingRuns{0};
class CountingOp : public Operator {
public:
    std::string typeName() const override { return "TestCounting"; }
    std::vector<PortSpec> inputs() const override { return {{"strands", PortType::Strands}}; }
    Value evaluate(const EvalContext&, const std::vector<Value>& inputs) const override {
        ++gCountingRuns;
        return inputs.empty() ? Value{} : inputs[0];
    }
};

TEST(Compound, TheInnerGraphCachesWhatAnExposedParamTweakDoesNotReach) {
    // in -> counting -> noise(amount exposed) -> curl: a tweak of `amount` runs noise and curl
    // again, not the counting node before them (the inner graph keeps its own cache); other
    // strands fed in run everything.
    OperatorRegistry::instance().registerType("TestCounting", {}, [] { return std::make_unique<CountingOp>(); });
    json definition = frizzyDefinition();
    definition["graph"]["nodes"].push_back({{"name", "count"}, {"type", "TestCounting"}});
    definition["graph"]["connections"] = json::array({
        json{{"from", "in"}, {"to", "count"}, {"input", "strands"}},
        json{{"from", "count"}, {"to", "noise"}, {"input", "strands"}},
        json{{"from", "noise"}, {"to", "curl"}, {"input", "strands"}}});
    auto graphWith = [&](double amount) {
        json g = baseGraph();
        g["nodes"].push_back({{"name", "frizzy"}, {"type", "Compound"}, {"params", {{"definition", definition}, {"amount", amount}}}});
        g["connections"].push_back({{"from", "scatter"}, {"to", "frizzy"}, {"input", "strands"}});
        g["connections"].push_back({{"from", "frizzy"}, {"to", "out"}, {"input", "strands"}});
        return g.dump();
    };
    GroomData d;
    d.scalp = test::makeSphere(12, 24, 1.0f);
    d.graphJson = graphWith(0.05);
    Groom groom(d);
    gCountingRuns = 0;
    const StrandsPtr a = groom.evaluate(1.0f, 0.0);
    ASSERT_TRUE(a);
    EXPECT_EQ(gCountingRuns.load(), 1);
    groom.setGraphJson(graphWith(0.2));  // the exposed param: the compound runs again
    const StrandsPtr b = groom.evaluate(1.0f, 0.0);
    ASSERT_TRUE(b);
    EXPECT_EQ(gCountingRuns.load(), 1) << "the counting node before the tweaked one ran again";
    EXPECT_NE(a->positions(), b->positions());
    // The tweaked compound matches the same nodes inline.
    EXPECT_EQ(b->positions(), eval(inlineGraph(0.2).dump())->positions());
    // Other strands in: everything runs.
    GroomData other = d;
    other.scalp = test::makeSphere(14, 28, 1.0f);
    other.graphJson = graphWith(0.2);
    Groom second(other);
    ASSERT_TRUE(second.evaluate(1.0f, 0.0));
    EXPECT_EQ(gCountingRuns.load(), 2);
    OperatorRegistry::instance().unregisterType("TestCounting");
}

TEST(Compound, RoundTripsAndNests) {
    const GraphDocument doc = loadGraphJson(compoundGraph({{"amount", 0.1}}).dump());
    const json saved = json::parse(saveGraphJson(doc));
    json definition;
    for (const json& n : saved.at("nodes")) {
        if (n.at("name") == "frizzy") definition = n.at("params").at("definition");
    }
    ASSERT_TRUE(definition.is_object());  // written back as an object, not an escaped string
    EXPECT_EQ(definition.at("expose").at("amount"), "noise.amplitude");
    EXPECT_EQ(eval(saved.dump())->positions(), eval(compoundGraph({{"amount", 0.1}}).dump())->positions());

    // A compound inside a compound.
    json outer = json::parse(R"j({"graph": {"version": 1, "output": "inner", "nodes": [
        {"name": "in", "type": "Input", "params": {"name": "strands"}}], "connections": [
        {"from": "in", "to": "inner", "input": "strands"}]},
      "inputs": [{"name": "strands", "type": "Strands"}]})j");
    outer["graph"]["nodes"].push_back({{"name", "inner"}, {"type", "Compound"}, {"params", {{"definition", frizzyDefinition()}}}});
    json g = baseGraph();
    g["nodes"].push_back({{"name", "wrapped"}, {"type", "Compound"}, {"params", {{"definition", outer}}}});
    g["connections"].push_back({{"from", "scatter"}, {"to", "wrapped"}, {"input", "strands"}});
    g["connections"].push_back({{"from", "wrapped"}, {"to", "out"}, {"input", "strands"}});
    EXPECT_EQ(eval(g.dump())->positions(), eval(inlineGraph(0.05).dump())->positions());
}

TEST(Compound, SeesTheScalpAndItsMaps) {
    // The inner graph reads the scalp (a Mesh input) and a color set map.
    auto scalp = std::make_shared<ScalpMesh>(*test::makeSphere(12, 24, 1.0f));
    scalp->setCornerColors("mask", std::vector<ScalpMesh::Color>(scalp->triangleCount() * 3, {0.5f, 0.5f, 0.5f, 1.0f}));
    json definition = json::parse(R"j({
      "graph": {"version": 1, "output": "collide",
        "nodes": [{"name": "in", "type": "Input", "params": {"name": "strands"}},
                  {"name": "mesh", "type": "Input", "params": {"name": "scalp", "type": "Mesh"}},
                  {"name": "len", "type": "Length", "params": {"keep": {"colorSet": "mask"}}},
                  {"name": "collide", "type": "Collide"}],
        "connections": [{"from": "in", "to": "len", "input": "strands"},
                        {"from": "len", "to": "collide", "input": "strands"},
                        {"from": "mesh", "to": "collide", "input": "scalp"}]},
      "inputs": [{"name": "strands", "type": "Strands"}, {"name": "scalp", "type": "Mesh"}]})j");
    json g = baseGraph();
    g["nodes"].push_back({{"name", "c"}, {"type", "Compound"}, {"params", {{"definition", definition}}}});
    g["connections"].push_back({{"from", "scatter"}, {"to", "c"}, {"input", "strands"}});
    g["connections"].push_back({{"from", "scalp"}, {"to", "c"}, {"input", "scalp"}});
    g["connections"].push_back({{"from", "c"}, {"to", "out"}, {"input", "strands"}});
    const StrandsPtr s = eval(g.dump(), scalp);
    const Vec3f* p = s->strandPoints(0);
    EXPECT_NEAR((p[s->strandPointCount(0) - 1] - p[0]).length(), 0.5f, 1e-3f);  // length 1 scaled by the map
}

TEST(Compound, InnerLinksReadTheOutputTheyName) {
    // Inside a compound a link picks an inner node's output as outside: a Map node's g drives
    // the inner Length (g = 0.5 on the right half, 1 on the left; r the other way round). The
    // compound gives one output, its output node's first.
    auto scalp = std::make_shared<ScalpMesh>(*test::makeSphere(12, 24, 1.0f));
    std::vector<ScalpMesh::Color> colors;
    for (const auto& tri : scalp->triangles()) {
        for (const std::int32_t v : tri) {
            const bool right = scalp->points()[v].x > 0.0f;
            colors.push_back({right ? 1.0f : 0.5f, right ? 0.5f : 1.0f, 0.0f, 1.0f});
        }
    }
    scalp->setCornerColors("packed", colors);
    auto definition = [](const std::string& output) {
        json d = json::parse(R"j({
          "graph": {"version": 1, "output": "len",
            "nodes": [{"name": "in", "type": "Input", "params": {"name": "strands"}},
                      {"name": "mask", "type": "Map", "params": {"name": "packed"}},
                      {"name": "len", "type": "Length", "params": {"keep": 1.0}, "paramInputs": ["keep"]}],
            "connections": [{"from": "in", "to": "len", "input": "strands"},
                            {"from": "mask", "to": "len", "input": "keep"}]},
          "inputs": [{"name": "strands", "type": "Strands"}]})j");
        if (!output.empty()) d["graph"]["connections"][1]["output"] = output;
        return d;
    };
    auto graphWith = [&](const std::string& output) {
        json g = baseGraph();
        g["nodes"].push_back({{"name", "c"}, {"type", "Compound"}, {"params", {{"definition", definition(output)}}}});
        g["connections"].push_back({{"from", "scatter"}, {"to", "c"}, {"input", "strands"}});
        g["connections"].push_back({{"from", "c"}, {"to", "out"}, {"input", "strands"}});
        return g.dump();
    };
    auto reach = [](const StrandSet& s, bool right) {  // mean root-to-tip distance on one side
        double sum = 0.0;
        int n = 0;
        for (std::size_t i = 0; i < s.strandCount(); ++i) {
            const Vec3f* p = s.strandPoints(i);
            if (std::abs(p[0].x) < 0.3f || (p[0].x > 0.0f) != right) continue;
            sum += (p[s.strandPointCount(i) - 1] - p[0]).length();
            ++n;
        }
        return n ? sum / n : 0.0;
    };
    const StrandsPtr g = eval(graphWith("g"), scalp);
    const StrandsPtr r = eval(graphWith("r"), scalp);
    ASSERT_TRUE(g && r);
    EXPECT_NEAR(reach(*g, true), 0.5, 0.05);
    EXPECT_NEAR(reach(*g, false), 1.0, 0.05);
    EXPECT_NEAR(reach(*r, true), 1.0, 0.05);
    EXPECT_NEAR(reach(*r, false), 0.5, 0.05);
    // The inner link keeps its output through a save; the compound has the one output.
    const GraphDocument doc = loadGraphJson(graphWith("g"));
    EXPECT_EQ(doc.graph.outputSpecs(doc.node("c")).size(), 1u);
    EXPECT_NE(saveGraphJson(doc).find(R"("output": "g")"), std::string::npos);
    EXPECT_THROW(eval(graphWith("q"), scalp), GraphError);  // no such output: the definition does not load
}

// Displacement of every point of `a` from the same point of `b`, summed.
float totalMove(const StrandSet& a, const StrandSet& b) {
    float sum = 0.0f;
    for (std::size_t i = 0; i < a.pointCount() && i < b.pointCount(); ++i) sum += (a.positions()[i] - b.positions()[i]).length();
    return sum;
}

TEST(Compound, RepeatRunsTheSubgraphAgainOnItsOwnOutput) {
    json noisy = json::parse(R"j({
      "graph": {"version": 1, "output": "noise",
        "nodes": [{"name": "in", "type": "Input", "params": {"name": "strands", "type": "Strands"}},
                  {"name": "noise", "type": "Noise", "params": {"amplitude": 0.03, "frequency": 2.0}}],
        "connections": [{"from": "in", "to": "noise", "input": "strands"}]},
      "inputs": [{"name": "strands", "type": "Strands"}]})j");
    auto graphWith = [&](const json& params) {
        json g = baseGraph();
        json p = params;
        p["definition"] = noisy;
        g["nodes"].push_back({{"name", "c"}, {"type", "Compound"}, {"params", p}});
        g["connections"].push_back({{"from", "scatter"}, {"to", "c"}, {"input", "strands"}});
        g["connections"].push_back({{"from", "c"}, {"to", "out"}, {"input", "strands"}});
        return g.dump();
    };
    json plain = baseGraph();
    plain["connections"].push_back({{"from", "scatter"}, {"to", "out"}, {"input", "strands"}});
    const StrandsPtr base = eval(plain.dump());
    const StrandsPtr once = eval(graphWith(json::object()));
    const StrandsPtr one = eval(graphWith({{"repeat", 1}}));
    const StrandsPtr three = eval(graphWith({{"repeat", 3}}));
    EXPECT_EQ(once->positions(), one->positions());  // repeat 1 is the compound as before
    ASSERT_EQ(three->strandCount(), base->strandCount());
    EXPECT_GT(totalMove(*three, *base), 1.5f * totalMove(*once, *base));
    EXPECT_EQ(three->ids(), base->ids());
    for (std::size_t s = 0; s < three->strandCount(); ++s) EXPECT_EQ(three->strandPoints(s)[0], base->strandPoints(s)[0]);
}

TEST(Compound, IterationInputVariesPerPass) {
    // Pass p scales the strands by 0.5 + 0.25 p (a FieldRemap of the iteration over 0..2).
    json stepped = json::parse(R"j({
      "graph": {"version": 1, "output": "len",
        "nodes": [{"name": "in", "type": "Input", "params": {"name": "strands", "type": "Strands"}},
                  {"name": "pass", "type": "Input", "params": {"name": "iteration", "type": "Field"}},
                  {"name": "remap", "type": "FieldRemap", "params": {"inMin": 0.0, "inMax": 2.0, "outMin": 0.5, "outMax": 1.0}},
                  {"name": "len", "type": "Length", "paramInputs": ["keep"]}],
        "connections": [{"from": "in", "to": "len", "input": "strands"},
                        {"from": "pass", "to": "remap", "input": "in"},
                        {"from": "remap", "to": "len", "input": "keep"}]},
      "inputs": [{"name": "strands", "type": "Strands"}]})j");
    auto graphWith = [&](int repeat) {
        json g = baseGraph();
        g["nodes"].push_back({{"name", "c"}, {"type", "Compound"}, {"params", {{"definition", stepped}, {"repeat", repeat}}}});
        g["connections"].push_back({{"from", "scatter"}, {"to", "c"}, {"input", "strands"}});
        g["connections"].push_back({{"from", "c"}, {"to", "out"}, {"input", "strands"}});
        return g.dump();
    };
    json plain = baseGraph();
    plain["connections"].push_back({{"from", "scatter"}, {"to", "out"}, {"input", "strands"}});
    const StrandsPtr base = eval(plain.dump());
    auto tipLength = [](const StrandSet& s) {
        return (s.strandPoints(0)[s.strandPointCount(0) - 1] - s.strandPoints(0)[0]).length();
    };
    const float full = tipLength(*base);
    EXPECT_NEAR(tipLength(*eval(graphWith(1))), 0.5f * full, 1e-4f);                  // pass 0 alone
    EXPECT_NEAR(tipLength(*eval(graphWith(2))), 0.5f * 0.75f * full, 1e-4f);          // then pass 1
    EXPECT_NEAR(tipLength(*eval(graphWith(3))), 0.5f * 0.75f * 1.0f * full, 1e-4f);   // then pass 2
}

TEST(Compound, CachesAsOneNodeAndReportsErrors) {
    GraphDocument doc = loadGraphJson(compoundGraph(json::object()).dump());
    auto scalp = test::makeSphere(12, 24, 1.0f);
    ExternalInputs ext;
    ext.set("scalp", scalp, scalp->contentHash());
    EvalContext ctx;
    ctx.externals = &ext;
    doc.graph.evaluateStrands(doc.output, ctx);
    const std::size_t count = doc.graph.evaluationCount();
    doc.graph.evaluateStrands(doc.output, ctx);
    EXPECT_EQ(doc.graph.evaluationCount(), count);  // cached
    doc.graph.node(doc.node("frizzy")).params().set("amount", 0.3);
    doc.graph.evaluateStrands(doc.output, ctx);
    EXPECT_EQ(doc.graph.evaluationCount(), count + 2);  // the compound and the output, not scatter

    // Ports come from the definition: a Mesh into its Strands input is refused.
    EXPECT_THROW(doc.graph.connect(doc.node("scalp"), doc.node("frizzy"), 0), GraphError);

    json bad = compoundGraph(json::object());
    for (json& n : bad["nodes"]) {
        if (n["name"] == "frizzy") n["params"]["definition"] = json::object({{"graph", "nope"}});
    }
    EXPECT_THROW(eval(bad.dump()), GraphError);
    json unknown = frizzyDefinition();
    unknown["expose"]["x"] = "missing.param";
    json g = baseGraph();
    g["nodes"].push_back({{"name", "c"}, {"type", "Compound"}, {"params", {{"definition", unknown}}}});
    g["connections"].push_back({{"from", "c"}, {"to", "out"}, {"input", "strands"}});
    EXPECT_THROW(eval(g.dump()), GraphError);
}

TEST(Compound, KeysOnTheContextItsInnerNodesRead) {
    // A Cards node inside: the compound's key follows buildCards the way the Cards node's own would.
    json g = baseGraph();
    const json definition = json::parse(R"j({
      "graph": {"version": 1, "output": "cards",
        "nodes": [{"name": "in", "type": "Input", "params": {"name": "strands", "type": "Strands"}},
                  {"name": "cards", "type": "Cards", "params": {"segments": 4}}],
        "connections": [{"from": "in", "to": "cards", "input": "strands"}]},
      "inputs": [{"name": "strands", "type": "Strands"}]})j");
    g["nodes"].push_back({{"name", "c"}, {"type", "Compound"}, {"params", {{"definition", definition}}}});
    g["connections"].push_back({{"from", "scatter"}, {"to", "c"}, {"input", "strands"}});
    g["connections"].push_back({{"from", "c"}, {"to", "out"}, {"input", "strands"}});
    GraphDocument doc = loadGraphJson(g.dump());
    auto scalp = test::makePlane(8);
    ExternalInputs ext;
    ext.set("scalp", scalp, scalp->contentHash());
    EvalContext ctx;
    ctx.externals = &ext;
    const StrandsPtr plain = doc.graph.evaluateStrands(doc.output, ctx);
    ASSERT_TRUE(plain);
    EXPECT_TRUE(plain->cardLayers().empty());
    ctx.buildCards = true;
    const StrandsPtr withCards = doc.graph.evaluateStrands(doc.output, ctx);  // not the cached pass-through
    ASSERT_EQ(withCards->cardLayers().size(), 1u);
    EXPECT_FALSE(withCards->cardLayers().front()->cards.empty());
    ctx.buildCards = false;
    EXPECT_TRUE(doc.graph.evaluateStrands(doc.output, ctx)->cardLayers().empty());
}

// Group and Ungroup as every host's editor does them (GraphEdit::collapse, expand): links from
// outside become the compound's inputs (strands, a Field into an exposed param, a Value into one),
// the same groom evaluates, and expanding gives every node, field and link back.
TEST(Compound, TheSessionHoldsACompoundAsItsNodesInline) {
    // Flattened: the compound's nodes by their paths, its exposed values on them; where its
    // output and exposed params went (a compound inside another followed through).
    GraphEdit edit(compoundGraph({{"amount", 0.2}}).dump());
    const GraphEdit::FlatCompounds flat = edit.flattenCompounds();
    const json g = json::parse(edit.toJson());
    std::map<std::string, json> nodes;
    for (const json& n : g["nodes"]) nodes[n["name"].get<std::string>()] = n;
    EXPECT_FALSE(nodes.count("frizzy"));
    ASSERT_TRUE(nodes.count("frizzy/noise") && nodes.count("frizzy/curl"));
    EXPECT_DOUBLE_EQ(nodes["frizzy/noise"]["params"]["amplitude"].get<double>(), 0.2);
    EXPECT_EQ(flat.result("frizzy"), "frizzy/curl");
    EXPECT_EQ(flat.param("frizzy", "amount"), std::make_pair(std::string("frizzy/noise"), std::string("amplitude")));
    EXPECT_EQ(flat.param("scatter", "density"), std::make_pair(std::string("scatter"), std::string("density")));
    json outer = baseGraph();
    json inner = compoundGraph(json::object());
    json wrapped = json::parse(R"j({"graph": {"version": 1, "output": "frizzy",
        "nodes": [{"name": "in", "type": "Input", "params": {"name": "strands", "type": "Strands"}}],
        "connections": [{"from": "in", "to": "frizzy", "input": "strands"}]},
      "inputs": [{"name": "strands", "type": "Strands"}], "expose": {"amt": "frizzy.amount"}})j");
    wrapped["graph"]["nodes"].push_back({{"name", "frizzy"}, {"type", "Compound"}, {"params", {{"definition", frizzyDefinition()}}}});
    outer["nodes"].push_back({{"name", "outer"}, {"type", "Compound"}, {"params", {{"definition", wrapped}, {"amt", 0.3}}}});
    outer["connections"].push_back({{"from", "scatter"}, {"to", "outer"}, {"input", "strands"}});
    outer["connections"].push_back({{"from", "outer"}, {"to", "out"}, {"input", "strands"}});
    GraphEdit nested(outer.dump());
    const GraphEdit::FlatCompounds two = nested.flattenCompounds();
    EXPECT_EQ(two.result("outer"), "outer/frizzy/curl");
    EXPECT_EQ(two.param("outer", "amt"), std::make_pair(std::string("outer/frizzy/noise"), std::string("amplitude")));
    EXPECT_EQ(eval(outer.dump())->positions(), eval(compoundGraph({{"amount", 0.3}}).dump())->positions());
    // A compound running its nodes again (repeat) stays one.
    GraphEdit repeated(compoundGraph({{"repeat", 2}}).dump());
    EXPECT_TRUE(repeated.flattenCompounds().output.empty());
    EXPECT_TRUE(repeated.hasNode("frizzy"));

    // The session: what it is asked of the compound by name (its result, its time, a view of
    // it), answered by its nodes.
    GroomData d;
    d.scalp = test::makeSphere(12, 24, 1.0f);
    d.graphJson = compoundGraph({{"amount", 0.2}}).dump();
    GroomSession session(d);
    const StrandsPtr out = session.evaluate(1.0f, 0.0);
    EXPECT_EQ(session.flatCompounds().result("frizzy"), "frizzy/curl");
    EXPECT_EQ(valueAs<StrandsPtr>(session.evaluateNode("frizzy", 1.0f, 0.0))->positions(), out->positions());
    std::map<std::string, double> times;
    for (const auto& [name, ms] : session.nodeTimes()) times[name] = ms;
    ASSERT_TRUE(times.count("frizzy") && times.count("frizzy/noise") && times.count("frizzy/curl"));
    EXPECT_DOUBLE_EQ(times["frizzy"], times["frizzy/noise"] + times["frizzy/curl"]);
    ViewRequest viewed;
    viewed.viewNode = "frizzy";
    EXPECT_EQ(session.evaluate(viewed).viewedNode, "frizzy");
}

TEST(Compound, CollapseAndExpandRoundTrip) {
    GraphEdit edit(baseGraph().dump());
    edit.addNode("Noise", "noise", R"({"amplitude": 0.05})", 300.0, 40.0);
    edit.addNode("Curl", "curl", R"({"radius": 0.02})", 520.0, 80.0);
    edit.addNode("FieldNoise", "wobble", R"({"min": 0.02, "max": 0.06})", 100.0, 300.0);
    edit.addNode("Value", "octaves", R"({"kind": "integer", "integer": 3})", 100.0, 420.0);
    json g = json::parse(edit.toJson());
    for (json& n : g["nodes"]) {
        if (n["name"] == "noise") n["paramInputs"] = {"amplitude", "octaves"};
        if (n["name"] == "curl") n["paramControls"] = {{"frequency", "freq"}};
    }
    edit = GraphEdit(g.dump());
    edit.connect("scatter", "noise", "strands");
    edit.connect("noise", "curl", "strands");
    edit.connect("curl", "out", "strands");
    edit.connect("wobble", "noise", "amplitude");
    edit.connect("octaves", "noise", "octaves");
    const json before = json::parse(edit.toJson());
    const StrandsPtr inlined = eval(before.dump());
    ASSERT_GT(inlined->strandCount(), 100u);

    const std::string name = edit.collapse({"curl", "noise", "nope"});
    EXPECT_EQ(name, "compound");
    EXPECT_EQ(edit.nodeType(name), "Compound");
    EXPECT_FALSE(edit.hasNode("noise"));
    const json spec = json::parse(edit.nodeSpecJson(name));
    std::vector<std::pair<std::string, std::string>> inputs;
    for (const json& p : spec["inputs"]) inputs.emplace_back(p["name"], p["type"]);
    EXPECT_EQ(inputs, (std::vector<std::pair<std::string, std::string>>{{"strands", "Strands"}, {"amplitude", "Field"}, {"octaves", "Value"}}));
    const json grouped = json::parse(edit.toJson());
    std::set<std::string> links;
    for (const json& c : grouped["connections"]) {
        links.insert(c["from"].get<std::string>() + ">" + c["to"].get<std::string>() + "." + c["input"].get<std::string>());
    }
    EXPECT_EQ(links, (std::set<std::string>{"scalp>scatter.scalp", "scatter>compound.strands", "wobble>compound.amplitude",
                                            "octaves>compound.octaves", "compound>out.strands"}));
    EXPECT_EQ(eval(grouped.dump())->positions(), inlined->positions());  // the same groom, grouped
    // The inner nodes keep every field (exposed params, keyed params, position).
    json definition;
    for (const json& n : grouped["nodes"]) {
        if (n["name"] == name) definition = n["params"]["definition"];
    }
    EXPECT_EQ(definition["graph"]["output"], "curl");
    for (const json& n : definition["graph"]["nodes"]) {
        if (n["name"] == "noise") EXPECT_EQ(n["paramInputs"], json({"amplitude", "octaves"}));
        if (n["name"] == "curl") EXPECT_EQ(n["paramControls"], json({{"frequency", "freq"}}));
    }

    // An exposed param's value goes to its inner param on the way out; a name taken outside
    // meanwhile is made free.
    json withExposed = grouped;
    for (json& n : withExposed["nodes"]) {
        if (n["name"] != name) continue;
        n["params"]["definition"]["expose"] = {{"amount", "noise.amplitude"}};
        n["params"]["amount"] = 0.07;
    }
    GraphEdit exposed(withExposed.dump());
    exposed.addNode("Width", "curl", "", 0.0, 600.0);  // takes the inner node's name
    EXPECT_EQ(exposed.expand(name), (std::vector<std::string>{"noise", "curl1"}));
    for (const json& n : json::parse(exposed.toJson())["nodes"]) {
        if (n["name"] == "noise") EXPECT_EQ(n["params"]["amplitude"], 0.07);
    }
    EXPECT_EQ(exposed.outputNode(), "out");

    // Ungrouped: the nodes, their fields, the links and the output as they were.
    EXPECT_EQ(edit.expand(name), (std::vector<std::string>{"noise", "curl"}));
    const json after = json::parse(edit.toJson());
    std::set<std::string> beforeLinks, afterLinks;
    for (const json& c : before["connections"]) beforeLinks.insert(c.dump());
    for (const json& c : after["connections"]) afterLinks.insert(c.dump());
    EXPECT_EQ(afterLinks, beforeLinks);
    EXPECT_EQ(after["output"], before["output"]);
    std::map<std::string, json> was, now;
    for (const json& n : before["nodes"]) was[n["name"]] = n;
    for (const json& n : after["nodes"]) now[n["name"]] = n;
    ASSERT_EQ(now.size(), was.size());
    for (const auto& [n, node] : was) {
        ASSERT_TRUE(now.count(n)) << n;
        json a = node, b = now[n];
        if (a.contains("ui")) {
            EXPECT_NEAR(a["ui"]["x"].get<double>(), b["ui"]["x"].get<double>(), 1e-9) << n;
            EXPECT_NEAR(a["ui"]["y"].get<double>(), b["ui"]["y"].get<double>(), 1e-9) << n;
        }
        a.erase("ui");
        b.erase("ui");
        EXPECT_EQ(a, b) << n;
    }
    EXPECT_EQ(eval(after.dump())->positions(), inlined->positions());

    // The graph's output grouped: the compound is the output, and back.
    GraphEdit last(before.dump());
    const std::string tail = last.collapse({"curl", "out"});
    EXPECT_EQ(last.outputNode(), tail);
    last.expand(tail);
    EXPECT_EQ(last.outputNode(), "out");
}

TEST(Compound, GroupsThatCannotBeMadeLeaveTheGraphAlone) {
    GraphEdit edit(inlineGraph(0.05).dump());
    edit.addNode("Width", "width", "", 0.0, 0.0);
    edit.connect("scatter", "width", "strands");
    const std::string before = edit.toJson();
    EXPECT_THROW(edit.collapse({}), GraphError);
    try {
        edit.collapse({"scatter", "noise"});  // scatter feeds width, noise feeds curl
        ADD_FAILURE() << "two nodes feeding out were grouped";
    } catch (const GraphError& e) {
        EXPECT_NE(std::string(e.what()).find("only one of the nodes"), std::string::npos) << e.what();
    }
    EXPECT_THROW(edit.expand("noise"), GraphError);  // not a Compound
    EXPECT_EQ(edit.toJson(), before);

    // A node feeding out from another output than its first is refused.
    GraphEdit map(R"({"nodes": [{"name": "mask", "type": "Map"}, {"name": "remap", "type": "FieldRemap"}],
                      "connections": [{"from": "mask", "to": "remap", "input": "in", "output": "g"}]})");
    EXPECT_THROW(map.collapse({"mask"}), GraphError);
    // The group reads the output its source gave; ungrouped, the link names it again.
    map.expand(map.collapse({"remap"}));
    const json links = json::parse(map.toJson())["connections"];
    ASSERT_EQ(links.size(), 1u);
    EXPECT_EQ(links[0].value("output", std::string()), "g");

    // A definition from another build (an unknown operator): refused, the graph unchanged.
    json g = compoundGraph(json::object());
    for (json& n : g["nodes"]) {
        if (n["name"] == "frizzy") n["params"]["definition"]["graph"]["nodes"][1]["type"] = "Bogus";
    }
    GraphEdit bogus(g.dump());
    const std::string kept = bogus.toJson();
    EXPECT_THROW(bogus.expand("frizzy"), GraphError);
    EXPECT_EQ(bogus.toJson(), kept);
}

TEST(Compound, MalformedDefinitionsAreGraphErrors) {
    // The definition's parts, each malformed: a GraphError naming it, never a JSON exception.
    for (const char* definition :
         {R"({"graph": {"version": 1, "output": "x", "nodes": [{"name": "x", "type": "Output"}]}, "inputs": {"a": 1}})",
          R"({"graph": {"version": 1, "output": "x", "nodes": [{"name": "x", "type": "Output"}]}, "inputs": [{"type": "Strands"}]})",
          R"({"graph": {"version": 1, "output": "x", "nodes": [{"name": "x", "type": "Output"}]}, "inputs": [{"name": "s", "type": "Hair"}]})",
          R"({"graph": {"version": 1, "output": "x", "nodes": [{"name": "x", "type": "Output"}]}, "expose": {"a": 3}})",
          R"({"graph": {"version": 1, "output": "x", "nodes": [{"name": "x", "type": "Output"}]}, "expose": [1]})"}) {
        json g = baseGraph();
        g["nodes"].push_back({{"name", "c"}, {"type", "Compound"}, {"params", {{"definition", json::parse(definition)}}}});
        g["connections"].push_back({{"from", "c"}, {"to", "out"}, {"input", "strands"}});
        try {
            eval(g.dump());
            ADD_FAILURE() << "no error for " << definition;
        } catch (const GraphError& e) {
            EXPECT_EQ(std::string(e.what()).find("[json.exception"), std::string::npos) << e.what();
        }
    }
    // The port types by name, one list for Input nodes and compound inputs alike.
    for (const PortType type : {PortType::Strands, PortType::Guides, PortType::Mesh, PortType::Field, PortType::Curve,
                                PortType::Curves}) {
        EXPECT_EQ(portTypeFromName(portTypeName(type)), type);
    }
    EXPECT_THROW(portTypeFromName("Hair"), GraphError);
}

}  // namespace
}  // namespace groomlab
