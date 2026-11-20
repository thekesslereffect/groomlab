// SPDX-License-Identifier: Apache-2.0
#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <sstream>
#include <thread>
#include <regex>
#include <cmath>
#include <map>
#include <set>
#include <fstream>
#include <filesystem>

#include <nlohmann/json.hpp>

#include "groomlab/braid_tubes.h"
#include "groomlab/display.h"
#include "groomlab/field.h"
#include "groomlab/graph.h"
#include "groomlab/graph_controls.h"
#include "groomlab/graph_edit.h"
#include "groomlab/graph_inserts.h"
#include "groomlab/graph_io.h"
#include "groomlab/groom.h"
#include "groomlab/instances.h"
#include "groomlab/node_catalog.h"
#include "groomlab/param_conditions.h"
#include "groomlab/random.h"
#include "test_helpers.h"
#include "groomlab/simulation.h"
#include "groomlab/presets.h"
#include "groomlab/operators.h"
#include "groomlab/groom_session.h"
#include "groomlab/scales.h"
#include "groomlab/drive_guides.h"
#include "groomlab/guide_hierarchy.h"
#include "groomlab/scalp_maps.h"
#include "groomlab/scalp_paint.h"

namespace groomlab {
namespace {

// Test source: `count` straight strands of 4 points each.
class LinesOp : public Operator {
public:
    std::string typeName() const override { return "TestLines"; }
    std::vector<PortSpec> inputs() const override { return {}; }
    Value evaluate(const EvalContext& ctx, const std::vector<Value>&) const override {
        auto out = std::make_shared<StrandSet>();
        const auto count = params().getInt("count", 10);
        for (std::int64_t i = 0; i < count; ++i) {
            out->addStrand(static_cast<StrandId>(i),
                           {{0, 0, 0}, {0, 1, 0}, {0, 2, 0}, {0, 3, 0}});
        }
        return StrandsPtr(std::make_shared<StrandSet>(out->withDensity(ctx.previewDensity)));
    }
};

// Test modifier: offsets every point along x.
class OffsetOp : public Operator {
public:
    std::string typeName() const override { return "TestOffset"; }
    std::vector<PortSpec> inputs() const override { return {{"in", PortType::Strands}}; }
    Value evaluate(const EvalContext&, const std::vector<Value>& inputs) const override {
        const auto* in = std::get_if<StrandsPtr>(&inputs.at(0));
        if (!in || !*in) throw GraphError("TestOffset: input not connected");
        auto out = std::make_shared<StrandSet>(**in);
        const float dx = static_cast<float>(params().getDouble("dx", 1.0));
        for (Vec3f& p : out->mutablePositions()) p.x += dx;
        return StrandsPtr(out);
    }
};

TEST(Graph, EvaluatesChain) {
    Graph g;
    const NodeId src = g.addNode(std::make_unique<LinesOp>());
    const NodeId off = g.addNode(std::make_unique<OffsetOp>());
    g.node(src).params().set("count", std::int64_t{5});
    g.node(off).params().set("dx", 2.0);
    g.connect(src, off, 0);

    StrandsPtr result = g.evaluateStrands(off);
    ASSERT_TRUE(result);
    EXPECT_EQ(result->strandCount(), 5u);
    EXPECT_FLOAT_EQ(result->positions()[0].x, 2.0f);
}

TEST(Graph, CachesUnchangedNodes) {
    Graph g;
    const NodeId src = g.addNode(std::make_unique<LinesOp>());
    const NodeId off = g.addNode(std::make_unique<OffsetOp>());
    g.connect(src, off, 0);

    g.evaluate(off);
    EXPECT_EQ(g.evaluationCount(), 2u);

    g.evaluate(off);  // nothing changed
    EXPECT_EQ(g.evaluationCount(), 2u);

    g.node(off).params().set("dx", 3.0);  // only downstream node re-runs
    g.evaluate(off);
    EXPECT_EQ(g.evaluationCount(), 3u);

    g.node(src).params().set("count", std::int64_t{20});  // both re-run
    g.evaluate(off);
    EXPECT_EQ(g.evaluationCount(), 5u);
}

TEST(Graph, ACacheLimitLetsResultsGoOldestAndCheapestFirst) {
    // A chain of three offsets on 20000 strands (each node's points its own: about 1 MB each) and a
    // side branch: within the limit everything stays; past it, the side branch (not reached by the
    // evaluation) goes first, then nodes of the chain, never the evaluated one; what was let go
    // evaluates again when asked, to the same bits.
    Graph g;
    const NodeId src = g.addNode(std::make_unique<LinesOp>());
    g.node(src).params().set("count", std::int64_t{20000});
    NodeId last = src;
    std::vector<NodeId> chain;
    for (int k = 0; k < 3; ++k) {
        const NodeId off = g.addNode(std::make_unique<OffsetOp>());
        g.node(off).params().set("dx", 1.0 + k);
        g.connect(last, off, 0);
        chain.push_back(last = off);
    }
    const NodeId side = g.addNode(std::make_unique<OffsetOp>());
    g.connect(src, side, 0);
    g.setCacheLimit(0);
    ASSERT_TRUE(g.evaluateStrands(side));
    const StrandsPtr full = g.evaluateStrands(last);
    ASSERT_TRUE(full);
    const std::size_t all = g.cachedBytes();
    const std::size_t points = full->pointCount() * sizeof(Vec3f);
    EXPECT_GT(all, 4 * points) << "five results' points (the source's, three offsets', the side's)";
    EXPECT_EQ(g.cacheEvictions(), 0u);

    // Room for all but the side branch's points: it goes (the chain's results stay).
    g.setCacheLimit(all - points / 2);
    const std::size_t runs = g.evaluationCount();
    ASSERT_TRUE(g.evaluateStrands(last));  // (nothing changed: served from the cache, then trimmed)
    EXPECT_EQ(g.evaluationCount(), runs);
    EXPECT_EQ(g.cacheEvictions(), 1u);
    EXPECT_FALSE(valueAs<StrandsPtr>(g.cachedValue(side)));
    for (const NodeId id : chain) EXPECT_TRUE(valueAs<StrandsPtr>(g.cachedValue(id))) << id;
    EXPECT_LE(g.cachedBytes(), all - points / 2);

    // Room for two results: the chain loses nodes too, never the evaluated one.
    g.setCacheLimit(2 * points + points / 2);
    ASSERT_TRUE(g.evaluateStrands(last));
    EXPECT_LE(g.cachedBytes(), 2 * points + points / 2 + full->strandCount() * 64);
    EXPECT_TRUE(valueAs<StrandsPtr>(g.cachedValue(last)));
    EXPECT_GE(g.cacheEvictions(), 3u);

    // Let go, made again when asked: the same strands.
    g.setCacheLimit(0);
    g.node(chain.front()).params().set("dx", 1.5);
    g.node(chain.front()).params().set("dx", 1.0);
    const StrandsPtr again = g.evaluateStrands(last);
    ASSERT_TRUE(again);
    EXPECT_EQ(again->positions(), full->positions());
    ASSERT_TRUE(g.evaluateStrands(side));
}

TEST(Graph, ContextChangeInvalidates) {
    Graph g;
    const NodeId src = g.addNode(std::make_unique<LinesOp>());
    g.node(src).params().set("count", std::int64_t{1000});

    EvalContext full;
    EvalContext preview;
    preview.previewDensity = 0.1f;

    const auto all = g.evaluateStrands(src, full);
    const auto some = g.evaluateStrands(src, preview);
    EXPECT_EQ(all->strandCount(), 1000u);
    EXPECT_LT(some->strandCount(), 200u);
    EXPECT_EQ(g.evaluationCount(), 2u);
}

TEST(Graph, RejectsCycles) {
    Graph g;
    const NodeId a = g.addNode(std::make_unique<OffsetOp>());
    const NodeId b = g.addNode(std::make_unique<OffsetOp>());
    g.connect(a, b, 0);
    EXPECT_THROW(g.connect(b, a, 0), GraphError);
    EXPECT_THROW(g.connect(a, a, 0), GraphError);
}

TEST(Graph, RejectsBadSlot) {
    Graph g;
    const NodeId a = g.addNode(std::make_unique<LinesOp>());
    const NodeId b = g.addNode(std::make_unique<OffsetOp>());
    EXPECT_THROW(g.connect(a, b, 1), GraphError);
}

TEST(Graph, RemoveNodeDropsConnections) {
    Graph g;
    const NodeId a = g.addNode(std::make_unique<LinesOp>());
    const NodeId b = g.addNode(std::make_unique<OffsetOp>());
    g.connect(a, b, 0);
    g.removeNode(a);

    NodeId src = 0;
    EXPECT_FALSE(g.inputSource(b, 0, &src));
    EXPECT_FALSE(g.hasNode(a));
    EXPECT_THROW(g.evaluate(b), GraphError);
}

// Test node with two outputs: the lines (primary) and the lines moved up by `dy` (Strands), plus
// the count as a field. Counts its runs.
class SplitOp : public Operator {
public:
    explicit SplitOp(int* runs) : runs_(runs) {}
    std::string typeName() const override { return "TestSplit"; }
    std::vector<PortSpec> inputs() const override { return {{"in", PortType::Strands}}; }
    std::vector<PortSpec> outputs() const override {
        return {output(), {"up", PortType::Strands}, {"count", PortType::Field}};
    }
    Value evaluate(const EvalContext& ctx, const std::vector<Value>& inputs) const override {
        return evaluateOutputs(ctx, inputs)[0];
    }
    std::vector<Value> evaluateOutputs(const EvalContext&, const std::vector<Value>& inputs) const override {
        ++*runs_;
        const StrandsPtr in = valueAs<StrandsPtr>(inputs.at(0));
        if (!in) throw GraphError("TestSplit: input not connected");
        auto up = std::make_shared<StrandSet>(*in);
        for (Vec3f& p : up->mutablePositions()) p.y += static_cast<float>(params().getDouble("dy", 10.0));
        return {in, StrandsPtr(up), constantField(static_cast<float>(in->strandCount()))};
    }

private:
    int* runs_;
};

// Test node joining two strand inputs: the first's strands, then the second's (moved IDs).
class JoinOp : public Operator {
public:
    std::string typeName() const override { return "TestJoin"; }
    std::vector<PortSpec> inputs() const override { return {{"a", PortType::Strands}, {"b", PortType::Strands}}; }
    Value evaluate(const EvalContext&, const std::vector<Value>& inputs) const override {
        auto out = std::make_shared<StrandSet>();
        StrandId next = 0;
        for (const Value& v : inputs) {
            const StrandsPtr s = valueAs<StrandsPtr>(v);
            if (!s) throw GraphError("TestJoin: connect a and b");
            for (std::size_t i = 0; i < s->strandCount(); ++i) {
                const Vec3f* p = s->strandPoints(i);
                out->addStrand(next++, std::vector<Vec3f>(p, p + s->strandPointCount(i)));
            }
        }
        return StrandsPtr(out);
    }
};

TEST(Graph, AMultiOutputNodeRunsOnceForAllItsConsumers) {
    for (const bool caching : {true, false}) {
        SCOPED_TRACE(caching ? "cached" : "transient");
        int runs = 0;
        Graph g;
        g.setCacheIntermediates(caching);
        const NodeId src = g.addNode(std::make_unique<LinesOp>());
        g.node(src).params().set("count", std::int64_t{3});
        const NodeId split = g.addNode(std::make_unique<SplitOp>(&runs));
        const NodeId join = g.addNode(std::make_unique<JoinOp>());
        g.connect(src, split, 0);
        g.connect(split, join, 0);     // the primary
        g.connect(split, join, 1, 1);  // "up"
        NodeId from = 0;
        std::size_t output = 9;
        ASSERT_TRUE(g.inputSource(join, 1, &from, &output));
        EXPECT_EQ(from, split);
        EXPECT_EQ(output, 1u);
        const StrandsPtr joined = g.evaluateStrands(join);
        ASSERT_TRUE(joined);
        ASSERT_EQ(joined->strandCount(), 6u);
        EXPECT_FLOAT_EQ(joined->strandPoints(0)[0].y, 0.0f);   // from the primary
        EXPECT_FLOAT_EQ(joined->strandPoints(3)[0].y, 10.0f);  // from "up"
        EXPECT_EQ(runs, 1);
        EXPECT_EQ(g.evaluationCount(), 3u);
        if (caching) {
            // Every output of the cached node, without running it again.
            const FieldPtr count = valueAs<FieldPtr>(g.evaluateOutput(split, 2));
            ASSERT_TRUE(count);
            EXPECT_FLOAT_EQ(count->maximum(), 3.0f);
            EXPECT_EQ(g.evaluateOutputs(split).size(), 3u);
            EXPECT_EQ(runs, 1);
            // Relinking to another output re-runs the consumer only.
            g.connect(split, join, 1, 0);
            EXPECT_EQ(g.evaluateStrands(join)->strandPoints(3)[0].y, 0.0f);
            EXPECT_EQ(runs, 1);
            EXPECT_EQ(g.evaluationCount(), 4u);
        }
    }
}

// A SplitOp taking a while, so its time shows.
class SlowSplitOp : public SplitOp {
public:
    using SplitOp::SplitOp;
    std::vector<Value> evaluateOutputs(const EvalContext& ctx, const std::vector<Value>& inputs) const override {
        std::this_thread::sleep_for(std::chrono::milliseconds(3));
        return SplitOp::evaluateOutputs(ctx, inputs);
    }
};

TEST(Graph, ANodeReadTwiceInOneEvaluationKeepsItsTime) {
    // Its second output read in the evaluation it ran in is served from its cache: its time stays
    // (a Braid with its ties connected showed 0 ms). An evaluation it did not run in: 0.
    int runs = 0;
    Graph g;
    const NodeId src = g.addNode(std::make_unique<LinesOp>());
    const NodeId split = g.addNode(std::make_unique<SlowSplitOp>(&runs));
    const NodeId join = g.addNode(std::make_unique<JoinOp>());
    g.connect(src, split, 0);
    g.connect(split, join, 0);
    g.connect(split, join, 1, 1);
    ASSERT_TRUE(g.evaluateStrands(join));
    EXPECT_EQ(runs, 1);
    ASSERT_EQ(g.lastEvaluationTimes().count(split), 1u);
    EXPECT_GE(g.lastEvaluationTimes().at(split), 1.0);
    // Nothing ran: the node asked for is served (the nodes before it are not reached).
    ASSERT_TRUE(g.evaluateStrands(join));
    EXPECT_EQ(runs, 1);
    EXPECT_EQ(g.lastEvaluationTimes().at(join), 0.0);
    // Only the node after it runs (its link moved to another output): the split's is served.
    g.connect(split, join, 1, 0);
    ASSERT_TRUE(g.evaluateStrands(join));
    EXPECT_EQ(runs, 1);
    EXPECT_EQ(g.lastEvaluationTimes().at(split), 0.0);
    // It runs again and is read twice again.
    g.node(split).params().set("dy", 5.0);
    ASSERT_TRUE(g.evaluateStrands(join));
    EXPECT_EQ(runs, 2);
    EXPECT_GE(g.lastEvaluationTimes().at(split), 1.0);
}

TEST(Graph, ConnectionsTypeCheckTheOutputTheyRead) {
    int runs = 0;
    Graph g;
    const NodeId src = g.addNode(std::make_unique<LinesOp>());
    const NodeId split = g.addNode(std::make_unique<SplitOp>(&runs));
    const NodeId off = g.addNode(std::make_unique<OffsetOp>());
    g.connect(src, split, 0);
    EXPECT_EQ(g.outputType(split), PortType::Strands);
    EXPECT_EQ(g.outputType(split, 2), PortType::Field);
    EXPECT_THROW(g.outputType(split, 3), GraphError);
    EXPECT_THROW(g.connect(split, off, 0, 2), GraphError);  // a Field into strands
    EXPECT_THROW(g.connect(split, off, 0, 3), GraphError);  // no such output
    EXPECT_THROW(g.connect(src, off, 0, 1), GraphError);    // LinesOp has one output
    g.connect(split, off, 0, 1);
    EXPECT_EQ(g.outputIndex(split, "up"), 1u);
    EXPECT_EQ(g.outputIndex(split, "out"), 0u);
    EXPECT_THROW(g.outputIndex(split, "down"), GraphError);
    EXPECT_FLOAT_EQ(g.evaluateStrands(off)->positions()[0].y, 10.0f);
    // Removing the source clears the output index with the link.
    g.removeNode(split);
    std::size_t output = 9;
    EXPECT_FALSE(g.inputSource(off, 0, nullptr, &output));
    g.connect(src, off, 0);
    NodeId from = 0;
    ASSERT_TRUE(g.inputSource(off, 0, &from, &output));
    EXPECT_EQ(output, 0u);
}

TEST(GraphJson, ConnectionsNameTheOutputTheyRead) {
    // A link's "output" names the source's output (absent: the first); saved only when not the
    // first; an unknown name is an error. The operator specs list every output.
    const std::string text = R"({"version": 1, "output": "out", "nodes": [
        {"name": "scalp", "type": "Scalp"},
        {"name": "scatter", "type": "Scatter", "paramInputs": ["density"]},
        {"name": "length", "type": "Length", "paramInputs": ["keep"]},
        {"name": "mask", "type": "Map", "params": {"name": "m"}},
        {"name": "out", "type": "Output"}],
      "connections": [{"from": "scalp", "to": "scatter", "input": "scalp"},
        {"from": "mask", "to": "scatter", "input": "density", "output": "b"},
        {"from": "mask", "to": "length", "input": "keep", "output": "luminance"},
        {"from": "scatter", "to": "length", "input": "strands"},
        {"from": "length", "to": "out", "input": "strands"}]})";
    GraphDocument doc = loadGraphJson(text);
    auto slot = [&](const std::string& node, const std::string& input) {
        const auto specs = doc.graph.inputSpecs(doc.node(node));
        return static_cast<std::size_t>(std::find_if(specs.begin(), specs.end(), [&](const PortSpec& s) {
                                            return s.name == input;
                                        }) - specs.begin());
    };
    NodeId src = 0;
    std::size_t output = 9;
    ASSERT_TRUE(doc.graph.inputSource(doc.node("scatter"), slot("scatter", "density"), &src, &output));
    EXPECT_EQ(src, doc.node("mask"));
    EXPECT_EQ(output, 3u);
    ASSERT_TRUE(doc.graph.inputSource(doc.node("length"), slot("length", "keep"), &src, &output));
    EXPECT_EQ(output, 0u);
    const std::string saved = saveGraphJson(doc);
    EXPECT_NE(saved.find(R"("output": "b")"), std::string::npos);
    EXPECT_EQ(saved.find(R"("output": "luminance")"), std::string::npos);  // the first: left out
    EXPECT_EQ(saveGraphJson(loadGraphJson(saved)), saved);

    std::string bad = text;
    bad.replace(bad.find(R"("output": "b")"), 13, R"("output": "q")");
    try {
        loadGraphJson(bad);
        ADD_FAILURE() << "an unknown output loaded";
    } catch (const GraphError& e) {
        EXPECT_NE(std::string(e.what()).find("no output 'q'"), std::string::npos) << e.what();
    }
    // A Map output is a Field: it cannot feed strands.
    std::string wrong = text;
    const std::string link = R"({"from": "scatter", "to": "length", "input": "strands"})";
    wrong.replace(wrong.find(link), link.size(), R"({"from": "mask", "to": "length", "input": "strands", "output": "r"})");
    EXPECT_THROW(loadGraphJson(wrong), GraphError);

    const std::string specs = operatorSpecsJson();
    const std::size_t map = specs.find(R"("type":"Map")");
    ASSERT_NE(map, std::string::npos);
    EXPECT_NE(specs.find(R"("outputs":[{"name":"luminance","type":"Field"},{"name":"r","type":"Field"},)"
                         R"({"name":"g","type":"Field"},{"name":"b","type":"Field"},{"name":"a","type":"Field"},)" R"({"name":"rgb","type":"Field"}])"),
              std::string::npos);
    EXPECT_NE(specs.find(R"("output":{"name":"luminance","type":"Field"})"), std::string::npos);
    EXPECT_NE(specs.find(R"("outputs":[{"name":"out","type":"Strands"}])"), std::string::npos);  // one output
}

TEST(Graph, NodeCatalogIsEveryHostsAddNodeMenu) {
    // Every built-in type gives its menu category, a description and search words where it
    // registers (NodeInfo; not a host's own list), has its line in builtin_operators.inc, whose
    // sections follow the menu's categories, and the specs carry them.
    std::set<std::string> ids;
    for (const NodeCategory& c : nodeCategories()) ids.insert(c.id);
    const std::set<std::string> listed(builtinNodeTypes().begin(), builtinNodeTypes().end());
    EXPECT_EQ(listed.size(), builtinNodeTypes().size()) << "a type listed twice in builtin_operators.inc";
    for (const std::string& type : OperatorRegistry::instance().typeNames()) {
        if (type.rfind("Test", 0) == 0) continue;  // registered by these tests
        EXPECT_TRUE(listed.count(type)) << type << " has no line in engine/core/include/groomlab/builtin_operators.inc";
        const NodeInfo* info = nodeInfo(type);
        ASSERT_NE(info, nullptr) << type;
        EXPECT_TRUE(ids.count(info->category))
            << type << ": its NodeInfo (registerType, in its source file) names category '" << info->category
            << "', not one of nodeCategories() (node_catalog.cpp)";
        EXPECT_FALSE(info->description.empty()) << type << ": its NodeInfo (registerType) has no description";
    }
    // The list's order is the menu's: its types' categories in nodeCategories()' order.
    int last = 0;
    std::string lastType;
    for (const std::string& type : builtinNodeTypes()) {
        const NodeInfo* info = nodeInfo(type);
        if (!info) continue;  // registered outside the core (ImportCurves)
        const int order = nodeCategoryOrder(info->category);
        EXPECT_GE(order, last) << type << " (" << info->category << ") is listed after " << lastType
                               << " in builtin_operators.inc: move it to its category's section";
        last = order;
        lastType = type;
    }
    for (const nlohmann::json& s : nlohmann::json::parse(operatorSpecsJson())) {
        const std::string type = s["type"].get<std::string>();
        if (type.rfind("Test", 0) == 0) continue;
        EXPECT_TRUE(ids.count(s["category"].get<std::string>())) << type;
        EXPECT_FALSE(s["label"].get<std::string>().empty()) << type;
        EXPECT_FALSE(s["categoryTitle"].get<std::string>().empty()) << type;
        EXPECT_FALSE(s["description"].get<std::string>().empty()) << type;
        EXPECT_EQ(s["order"].get<int>(), nodeMenuOrder(type)) << type;
    }
    EXPECT_EQ(nodeLabel("Reference"), "Linked Groom");  // the type stays "Reference" in files
    EXPECT_EQ(nodeLabel("BlendNeighbors"), "Blend Neighbors");
    EXPECT_EQ(nodeLabel("HairColor"), "Hair Color");
    EXPECT_NE(nodeInfo("Mirror")->keywords.find("symmetry"), std::string::npos);
    EXPECT_EQ(nodeInfo("Cards")->category, "games");
    EXPECT_EQ(nodeInfo("Twist")->category, "look");
    EXPECT_EQ(nodeInfo("ToGuides")->category, "convert");
    EXPECT_TRUE(nodeInfo("Compound")->hidden);
    EXPECT_LT(nodeMenuOrder("Scalp"), nodeMenuOrder("ImportCurves"));  // its place, though the hosts register it
    EXPECT_LT(nodeMenuOrder("ImportCurves"), nodeMenuOrder("Reference"));
    // Decimals: none for counts, one for degrees, by the slider's span otherwise.
    for (const nlohmann::json& s : nlohmann::json::parse(operatorSpecsJson())) {
        if (s["type"] != "Scatter" && s["type"] != "Direction") continue;
        for (const nlohmann::json& p : s["params"]) {
            if (p["kind"] == "int") EXPECT_EQ(p["decimals"].get<int>(), 0) << p["name"];
            if (p["unit"] == "angle") EXPECT_EQ(p["decimals"].get<int>(), 1) << p["name"];
            if (p["kind"] == "float") EXPECT_TRUE(p.contains("decimals")) << p["name"];
        }
    }
}

TEST(Docs, NodeGraphListsEveryNode) {
    // The website's Node categories table (web/content/docs/node-graph.mdx) names every node of the
    // add-node menu in its category's row, by its label or its type, and nothing that is no node.
    const std::filesystem::path page = std::filesystem::path(GROOMLAB_TEST_DATA) / ".." / ".." / "web" / "content" / "docs" / "node-graph.mdx";
    std::ifstream in(page);
    if (!in) GTEST_SKIP() << "no " << page.generic_string();
    std::map<std::string, std::set<std::string>> rows;  // category title -> names listed
    bool inTable = false;
    for (std::string line; std::getline(in, line);) {
        if (line.rfind("## Node categories", 0) == 0) inTable = true;
        else if (inTable && line.rfind("## ", 0) == 0) break;
        if (!inTable || line.rfind("| ", 0) != 0 || line.rfind("| ---", 0) == 0 || line.rfind("| Category", 0) == 0) continue;
        std::vector<std::string> cells;
        std::stringstream cellText(line.substr(1));
        for (std::string cell; std::getline(cellText, cell, '|');) cells.push_back(cell);
        if (cells.size() < 2) continue;
        auto trim = [](std::string s) {
            s.erase(0, s.find_first_not_of(' '));
            s.erase(s.find_last_not_of(' ') + 1);
            return s;
        };
        std::stringstream names(cells[1]);
        for (std::string name; std::getline(names, name, ',');) rows[trim(cells[0])].insert(trim(name));
    }
    ASSERT_FALSE(rows.empty()) << page.generic_string() << " has no Node categories table";
    std::set<std::string> known(builtinNodeTypes().begin(), builtinNodeTypes().end());
    for (const std::string& type : OperatorRegistry::instance().typeNames()) {
        if (type.rfind("Test", 0) == 0) continue;
        const NodeInfo* info = nodeInfo(type);
        known.insert(nodeLabel(type));
        if (info->hidden) continue;  // not in the menu (Compound, Input)
        const int order = nodeCategoryOrder(info->category);
        ASSERT_LT(order, static_cast<int>(nodeCategories().size())) << type;
        const std::string title = nodeCategories()[static_cast<std::size_t>(order)].title;
        const std::set<std::string>& listed = rows[title];
        EXPECT_TRUE(listed.count(type) || listed.count(nodeLabel(type)))
            << type << " is missing from web/content/docs/node-graph.mdx's Node categories table: add \"" << nodeLabel(type)
            << "\" to its " << title << " row (and a line on it where the page says more)";
    }
    for (const auto& [title, names] : rows) {
        for (const std::string& name : names) {
            EXPECT_TRUE(known.count(name)) << "web/content/docs/node-graph.mdx's Node categories table lists \"" << name << "\" under "
                                           << title << ", which is no node type or label";
        }
    }
}

TEST(Graph, RegistryCreatesByName) {
    OperatorRegistry::instance().registerType("TestLines", {}, [] { return std::make_unique<LinesOp>(); });
    Graph g;
    const NodeId id = g.addNode("TestLines");
    EXPECT_EQ(g.node(id).typeName(), "TestLines");
    EXPECT_THROW(g.addNode("NoSuchOp"), GraphError);
}

TEST(ParamSet, TypedAccessWithFallbacks) {
    ParamSet p;
    p.set("i", std::int64_t{3});
    p.set("d", 0.5);
    p.set("s", std::string("abc"));
    EXPECT_EQ(p.getInt("i"), 3);
    EXPECT_DOUBLE_EQ(p.getDouble("i"), 3.0);  // ints widen to double
    EXPECT_DOUBLE_EQ(p.getDouble("d"), 0.5);
    EXPECT_EQ(p.getString("s"), "abc");
    EXPECT_EQ(p.getInt("missing", 9), 9);
    EXPECT_EQ(p.getInt("s", 9), 9);  // wrong type falls back

    ParamSet q = p;
    EXPECT_EQ(p.hash(), q.hash());
    q.set("d", 0.25);
    EXPECT_NE(p.hash(), q.hash());
}


TEST(Graph, TimeAloneDoesNotInvalidate) {
    // Time is not in the key (only ImportCurves reads it, salting its own):
    // a static groom scrubbed on the timeline is a cache hit every frame.
    // Time-varying data comes in as hashed externals.
    Graph g;
    const NodeId src = g.addNode(std::make_unique<LinesOp>());
    EvalContext a;
    a.time = 0.0;
    EvalContext b = a;
    b.time = 1.0 / 24.0;
    g.evaluateStrands(src, a);
    g.evaluateStrands(src, b);
    EXPECT_EQ(g.evaluationCount(), 1u);
    // Cards build on demand: only the Cards node keys on buildCards.
    EvalContext c = a;
    c.buildCards = true;
    g.evaluateStrands(src, c);
    EXPECT_EQ(g.evaluationCount(), 1u);
}

TEST(ParamSet, HashesAnExpressionMapsConstants) {
    // MapRefs that compare unequal (their constants differ) hash apart, so a cached result isn't reused.
    MapRef map;
    map.source = MapRef::Source::Expression;
    map.name = "amount * u";
    map.constants = {{"amount", 0.5f}};
    ParamSet a, b;
    a.set("w", map);
    map.constants = {{"amount", 0.75f}};
    b.set("w", map);
    EXPECT_NE(a.get("w"), b.get("w"));
    EXPECT_NE(a.hash(), b.hash());
    map.constants = {{"amount", 0.5f}};
    b.set("w", map);
    EXPECT_EQ(a.hash(), b.hash());
}

TEST(ParamSet, HashesStringsWithTheirLength) {
    // Consecutive strings can't run together: "ab" + "c" is not "a" + "bc".
    EXPECT_NE(hashString(hashString(0, "ab"), "c"), hashString(hashString(0, "a"), "bc"));
    ExternalInputs one, two;
    setMapExternals(one, {{"m", {"ab", "c"}}});
    setMapExternals(two, {{"m", {"a", "bc"}}});
    EXPECT_NE(one.find("maps")->hash, two.find("maps")->hash);
}

TEST(GraphJson, AnIntegerParamOutOfRangeIsAnError) {
    // A float where an int is declared is truncated; one past the int range is a GraphError, not undefined behavior.
    const std::string graph = R"({"version": 1, "output": "r", "nodes": [{"name": "r", "type": "Resample", "params": {"segments": %s}}]})";
    auto with = [&](const std::string& value) {
        std::string text = graph;
        text.replace(text.find("%s"), 2, value);
        return text;
    };
    GraphDocument doc = loadGraphJson(with("12.7"));
    EXPECT_EQ(doc.graph.node(doc.output).params().getInt("segments"), 12);
    // Stored truncated (getInt then clamps it to the spec's min, 0).
    EXPECT_EQ(std::get<std::int64_t>(loadGraphJson(with("-3.5")).graph.node(1).params().get("segments")), -3);
    EXPECT_EQ(loadGraphJson(with("-3.5")).graph.node(1).params().getInt("segments"), 0);
    EXPECT_THROW(loadGraphJson(with("1e300")), GraphError);
    EXPECT_THROW(loadGraphJson(with("-1e19")), GraphError);
    EXPECT_THROW(loadGraphJson(with("9.3e18")), GraphError);
}

TEST(GraphInserts, EveryHostAddsAFeatureAlike) {
    // graph_inserts.h: Maya's sim.py and collision.py, the Blender helper and the app add these.
    using nlohmann::json;
    auto feeds = [](const GraphEdit& e, const std::string& from, const std::string& to, const std::string& input) {
        const json g = json::parse(e.toJson());  // (held: a temporary's member would dangle in the loop)
        for (const json& c : g["connections"]) {
            if (c["from"] == from && c["to"] == to && c["input"] == input) return true;
        }
        return false;
    };
    GraphEdit edit(defaultGraphJson());
    setNodePosition(edit, "guides", 100.0, 50.0);
    // Simulate between the guides and the Interpolate, half a column right and a row below.
    const std::string sim = insertSimulate(edit, R"({"damping": 0.5})");
    EXPECT_EQ(edit.nodeType(sim), "Simulate");
    EXPECT_TRUE(feeds(edit, "guides", sim, "guides"));
    EXPECT_TRUE(feeds(edit, sim, "interpolate", "guides"));
    EXPECT_FALSE(feeds(edit, "guides", "interpolate", "guides"));
    EXPECT_EQ(nodePosition(edit, sim), std::make_pair(100.0 + kInsertColumn / 2.0, 50.0 + kInsertRow));
    // (Load Guide Cache: tests/engine/io, where the GuideCache operator is registered.)
    // A collider: its ColliderMesh, a Collide before the output on the scalp too.
    const std::string output = edit.outputNode();
    const std::string collider = insertCollider(edit, "collar.obj", true, 0.1);
    EXPECT_EQ(edit.nodeType(collider), "ColliderMesh");
    std::string collide;
    const json wired = json::parse(edit.toJson());
    for (const json& c : wired["connections"]) {
        if (c["to"] == output && c["input"] == "strands") collide = c["from"].get<std::string>();
    }
    ASSERT_EQ(edit.nodeType(collide), "Collide");
    EXPECT_TRUE(feeds(edit, collider, collide, "collider"));
    EXPECT_TRUE(feeds(edit, "scalp", collide, "scalp"));
    // The same mesh with no wire: the graph left alone.
    const std::string before = edit.toJson();
    EXPECT_EQ(insertCollider(edit, "collar.obj", false), collider);
    EXPECT_EQ(edit.toJson(), before);
    EXPECT_THROW(insertCollider(edit, "scalp"), GraphError);
    EXPECT_THROW(insertBefore(edit, "scalp", sim), GraphError) << "nothing feeds the scalp's strands";
}

TEST(GraphInserts, DriversGoRightAfterTheGuidesAndASimulationAfterThem) {
    using nlohmann::json;
    auto feeds = [](const GraphEdit& e, const std::string& from, const std::string& to, const std::string& input) {
        const json g = json::parse(e.toJson());
        for (const json& c : g["connections"]) {
            if (c["from"] == from && c["to"] == to && c["input"] == input) return true;
        }
        return false;
    };
    // Drivers first, then a simulation: Guides > DriveGuides > Simulate > Interpolate.
    GraphEdit edit(defaultGraphJson());
    const std::string drive = insertDriveGuides(edit);
    EXPECT_EQ(edit.nodeType(drive), "DriveGuides");
    EXPECT_TRUE(feeds(edit, "guides", drive, "strands"));
    EXPECT_TRUE(feeds(edit, drive, "interpolate", "guides"));
    EXPECT_EQ(insertDriveGuides(edit), drive) << "one there: kept";
    const std::string sim = insertSimulate(edit);
    EXPECT_TRUE(feeds(edit, drive, sim, "guides"));
    EXPECT_TRUE(feeds(edit, sim, "interpolate", "guides"));
    EXPECT_FALSE(feeds(edit, drive, "interpolate", "guides"));
    // A simulation first, then drivers: they go before it.
    GraphEdit other(defaultGraphJson());
    const std::string sim2 = insertSimulate(other);
    const std::string drive2 = insertDriveGuides(other);
    EXPECT_TRUE(feeds(other, "guides", drive2, "strands"));
    EXPECT_TRUE(feeds(other, drive2, sim2, "guides"));
    EXPECT_NO_THROW(loadGraphJson(other.toJson())) << "a graph that loads";
}

TEST(GraphInserts, MotionGoesBeforeTheCollideEndingTheChain) {
    // insertMotion: Maya's add_wind/add_jiggle, the Blender helper's insert_motion and the app's Add
    // > Wind put the node in one place: before the output, but before a Collide or Cards ending it.
    auto feeds = [](const GraphEdit& e, const std::string& from, const std::string& to, const std::string& input) {
        const nlohmann::json g = nlohmann::json::parse(e.toJson());
        for (const nlohmann::json& c : g["connections"]) {
            if (c["from"] == from && c["to"] == to && c["input"] == input) return true;
        }
        return false;
    };
    GraphEdit edit(defaultGraphJson());
    const std::string output = edit.outputNode();
    EXPECT_EQ(motionAnchor(edit), output);
    insertCollider(edit, "collar.obj", true);
    const std::string collide = motionAnchor(edit);
    ASSERT_EQ(edit.nodeType(collide), "Collide");
    const std::string wind = insertMotion(edit, "Wind");
    EXPECT_EQ(edit.nodeType(wind), "Wind");
    EXPECT_TRUE(feeds(edit, wind, collide, "strands"));
    const std::string jiggle = insertMotion(edit, "Jiggle", R"({"strength": 0.5})");
    EXPECT_TRUE(feeds(edit, jiggle, collide, "strands"));
    EXPECT_TRUE(feeds(edit, wind, jiggle, "strands"));
    EXPECT_NO_THROW(loadGraphJson(edit.toJson())) << "a graph that loads";
}

TEST(GraphInserts, RemovingAScalpOrColliderTakesItsNodesBackOut) {
    // removeScalpNodes / removeColliderNodes: Maya's remove_scalp and remove_collider, the Blender
    // add-on's: what insertScalp and insertCollider made goes, the flow as it was.
    const GraphEdit start(defaultGraphJson());
    const std::vector<std::string> before = start.nodeNames();
    GraphEdit edit(defaultGraphJson());
    const std::string output = edit.outputNode();
    insertScalp(edit, "patch");
    insertCollider(edit, "collar", true);
    insertCards(edit);
    EXPECT_GT(edit.nodeNames().size(), before.size() + 4);
    EXPECT_TRUE(removeScalpNodes(edit, "patch"));
    EXPECT_FALSE(removeScalpNodes(edit, "patch")) << "nothing reads it now";
    EXPECT_TRUE(removeColliderNodes(edit, "collar"));
    std::vector<std::string> left = edit.nodeNames();
    left.erase(std::remove_if(left.begin(), left.end(), [&](const std::string& n) { return edit.nodeType(n) == "Cards"; }), left.end());
    EXPECT_EQ(left, before) << "the scalp's Scalp, Scatter, Interpolate and Merge, the collider's two nodes: gone";
    EXPECT_EQ(edit.outputNode(), output);
    EXPECT_EQ(edit.nodeType(motionAnchor(edit)), "Cards") << "the cards still end the chain";
    EXPECT_NO_THROW(loadGraphJson(edit.toJson())) << "a graph that loads";
    bool made = true;
    EXPECT_EQ(edit.nodeType(insertCards(edit, {}, std::nullopt, &made)), "Cards");
    EXPECT_FALSE(made) << "one there: kept";
}

TEST(GraphInserts, PartsOpenTheHairAndTheStarterCountsTheGroomsOwnLook) {
    // insertPart: every host's part tools give the graph a Part node, one only.
    GraphEdit edit(defaultGraphJson());
    bool made = false;
    const std::string part = insertPart(edit, R"({"width": 0.2})", {}, std::nullopt, &made);
    EXPECT_TRUE(made);
    EXPECT_EQ(edit.nodeType(part), "Part");
    EXPECT_EQ(insertPart(edit, {}, {}, std::nullopt, &made), part);
    EXPECT_FALSE(made) << "one there: kept";
    EXPECT_NO_THROW(loadGraphJson(edit.toJson()));
    // starterLookGiven: the default graph's own look nodes are no look of the groom's.
    const std::vector<std::string> looks = {"Clump", "Noise", "Curl", "HairColor"};
    EXPECT_FALSE(starterLookGiven(defaultGraphJson(), looks));
    GraphEdit curled(defaultGraphJson());
    curled.addNode("Curl");
    EXPECT_TRUE(starterLookGiven(curled.toJson(), looks));
}

TEST(GuideHierarchy, EveryHostsGuideEditsAlike) {
    // childGroupFor: Generate Children adds to the group's own child group, else a new one.
    GuideHierarchy h;
    const GroupId primary = h.addGroup("primary");
    EXPECT_EQ(h.childGroupFor(primary), "primary_children");
    h.addGroup("primary_children", primary);
    EXPECT_EQ(h.childGroupFor(primary), "primary_children") << "its own child group, not primary_children2";
    // DriverBind attach and detach: a driver not attached detaches nothing.
    DriverBind bind;
    bind.attachGuides({1, 2}, 7);
    bind.attachGuides({1}, 8);
    EXPECT_EQ(bind.detachGuides({1, 2}, std::uint64_t{9}), 0u);
    EXPECT_EQ(bind.detachGuides({1}, std::uint64_t{7}), 1u);
    EXPECT_EQ(bind.attached.at(1), (std::vector<std::uint64_t>{8}));
    EXPECT_EQ(bind.detachGuides({1, 2}), 2u);
    EXPECT_TRUE(bind.attached.empty());
}

TEST(ScalpMaps, VertexMapsFollowTheScalpsJoiningOrLeaving) {
    // refitVertexMaps: Add / Remove Scalp in every host; a kept scalp keeps its values.
    ScalpMaps maps;
    ScalpMapEntry gray;
    gray.kind = "vertices";
    gray.values = {1, 2, 3, 4, 5};  // the groom's own 3 vertices, then a patch's 2
    maps.maps["dens"] = gray;
    ScalpMapEntry color;
    color.kind = "vertices";
    color.channels = kColorMap;
    color.values.assign(5 * 4, 0.5f);
    maps.maps["tint"] = color;
    const ScalpCounts before{{"scalp", 3}, {"patch", 2}};
    EXPECT_TRUE(refitVertexMaps(maps, before, {{"scalp", 3}, {"patch", 2}, {"ear", 1}}));
    EXPECT_EQ(maps.maps["dens"].values, (std::vector<float>{1, 2, 3, 4, 5, 0}));
    EXPECT_EQ(std::vector<float>(maps.maps["tint"].values.end() - 4, maps.maps["tint"].values.end()), (std::vector<float>{0, 0, 0, 1}));
    EXPECT_TRUE(refitVertexMaps(maps, {{"scalp", 3}, {"patch", 2}, {"ear", 1}}, {{"scalp", 3}, {"ear", 1}}));
    EXPECT_EQ(maps.maps["dens"].values, (std::vector<float>{1, 2, 3, 0}));
    EXPECT_EQ(mapKindOf(maps, nullptr, "dens"), "vertices");
    EXPECT_EQ(mapChannelsOf(maps, nullptr, "tint"), kColorMap);
    EXPECT_EQ(mapChannelsOf(maps, nullptr, "nope"), 0);
}

TEST(GraphInserts, AGuideGroupsReadersFollowItsRename) {
    // groupReaders / renameGroupReaders: every host's "Read by" and group rename (Maya's, the app's,
    // the Blender helper's), lists split as the Guides node splits them, compounds followed.
    using nlohmann::json;
    const json g = {
        {"nodes",
         {{{"name", "hair"}, {"type", "Guides"}, {"params", {{"exclude", "b,c"}}}},
          {{"name", "bangs"}, {"type", "Guides"}, {"params", {{"group", "a b"}}}},
          {{"name", "other"}, {"type", "Guides"}, {"params", {{"group", "bb"}}}},
          {{"name", "box"},
           {"type", "Compound"},
           {"params", {{"definition", {{"graph", {{"nodes", {{{"name", "inner"}, {"type", "Guides"}, {"params", {{"group", "b"}}}}}}}}}}}}}}},
        {"connections", json::array()}};
    EXPECT_EQ(groupReaders(g.dump(), "b"),
              (std::vector<std::pair<std::string, std::string>>{{"hair", "exclude"}, {"bangs", "group"}}));
    bool changed = false;
    const json renamed = json::parse(renameGroupReaders(g.dump(), "b", "back", &changed));
    EXPECT_TRUE(changed);
    EXPECT_EQ(renamed["nodes"][0]["params"]["exclude"], "back c");
    EXPECT_EQ(renamed["nodes"][1]["params"]["group"], "a back");
    EXPECT_EQ(renamed["nodes"][2]["params"]["group"], "bb") << "another group's name holding it: kept";
    EXPECT_EQ(renamed["nodes"][3]["params"]["definition"]["graph"]["nodes"][0]["params"]["group"], "back");
    EXPECT_EQ(renameGroupReaders(g.dump(), "zz", "y", &changed), g.dump());
    EXPECT_FALSE(changed);
}

TEST(GraphInserts, ANodeGoesIntoTheFlowAlikeInEveryHost) {
    // insertBetween, insertAfter and insertOnLink: Maya's GraphModel.insert_*, the Blender helper's
    // edit_graph ops and the app's node graph (a node dropped on a wire) make these.
    using nlohmann::json;
    auto feeds = [](const GraphEdit& e, const std::string& from, const std::string& to, const std::string& input) {
        const json g = json::parse(e.toJson());
        for (const json& c : g["connections"]) {
            if (c["from"] == from && c["to"] == to && c["input"] == input) return true;
        }
        return false;
    };
    auto firstFedBy = [](const GraphEdit& e, const std::string& from) {
        const json g = json::parse(e.toJson());
        for (const json& c : g["connections"]) {
            if (c["from"] == from) return std::make_pair(c["to"].get<std::string>(), c["input"].get<std::string>());
        }
        return std::make_pair(std::string(), std::string());
    };
    GraphEdit edit(defaultGraphJson());
    setNodePosition(edit, "interpolate", 300.0, 20.0);
    const auto [next, nextInput] = firstFedBy(edit, "interpolate");
    ASSERT_FALSE(next.empty());
    // After a node: into its chain, half a column right of and a row below it.
    const std::string curl = edit.addNode("Curl");
    insertAfter(edit, "interpolate", curl);
    EXPECT_TRUE(feeds(edit, "interpolate", curl, "strands"));
    EXPECT_TRUE(feeds(edit, curl, next, nextInput));
    EXPECT_FALSE(feeds(edit, "interpolate", next, nextInput));
    EXPECT_EQ(nodePosition(edit, curl), std::make_pair(300.0 + kInsertColumn / 2.0, 20.0 + kInsertRow));
    // After a node feeding nothing: fed by it, on its first input or the one named.
    const std::string leaf = edit.addNode("Noise");
    const std::string tail = edit.addNode("Noise");
    insertAfter(edit, leaf, tail);
    EXPECT_TRUE(feeds(edit, leaf, tail, "strands"));
    const std::string merge = edit.addNode("Merge");
    insertAfter(edit, tail, merge, "b");
    EXPECT_TRUE(feeds(edit, tail, merge, "b"));
    EXPECT_THROW(insertAfter(edit, "nope", curl), GraphError);
    // Between two nodes, on the node's first input when none is named; nothing changes when it cannot.
    const std::string between = edit.addNode("Merge");
    insertBetween(edit, curl, next, between, "");
    EXPECT_TRUE(feeds(edit, curl, between, "a"));
    EXPECT_TRUE(feeds(edit, between, next, nextInput));
    std::string before = edit.toJson();
    EXPECT_THROW(insertBetween(edit, "scalp", next, leaf), GraphError) << "the scalp does not feed it";
    EXPECT_THROW(insertBetween(edit, curl, between, "scalp", ""), GraphError) << "a Scalp node has no input";
    EXPECT_EQ(edit.toJson(), before);

    // On a link: the node reads the source on its first input that takes it (a Collide's strands, a
    // Simulate's guides), whatever the link's own input is named.
    GraphEdit wire(defaultGraphJson());
    const std::string sim = wire.addNode("Simulate");
    insertOnLink(wire, "guides", "interpolate", "guides", sim);
    EXPECT_TRUE(feeds(wire, "guides", sim, "guides"));
    EXPECT_TRUE(feeds(wire, sim, "interpolate", "guides"));
    EXPECT_FALSE(feeds(wire, "guides", "interpolate", "guides"));
    const auto [fed, fedInput] = firstFedBy(wire, "interpolate");
    const std::string noise = wire.addNode("Noise");
    const std::string firstOutput = json::parse(wire.nodeSpecJson("interpolate"))["outputs"][0]["name"].get<std::string>();
    insertOnLink(wire, "interpolate", fed, fedInput, noise, firstOutput);  // the first output by its name: the same link
    EXPECT_TRUE(feeds(wire, "interpolate", noise, "strands"));
    EXPECT_TRUE(feeds(wire, noise, fed, fedInput));
    EXPECT_NO_THROW(loadGraphJson(wire.toJson())) << "a graph that loads";
    // What it refuses, the graph as it was: a link that is not there, an end of the link, an unknown
    // node or output, a node with no input, a node that cannot feed the far end.
    before = wire.toJson();
    EXPECT_THROW(insertOnLink(wire, "interpolate", fed, fedInput, sim), GraphError) << "that link went through the Noise";
    EXPECT_THROW(insertOnLink(wire, "interpolate", noise, "strands", noise), GraphError);
    EXPECT_THROW(insertOnLink(wire, "interpolate", noise, "strands", "interpolate"), GraphError);
    EXPECT_THROW(insertOnLink(wire, "interpolate", noise, "strands", "nope"), GraphError);
    EXPECT_THROW(insertOnLink(wire, "nope", noise, "strands", sim), GraphError);
    EXPECT_THROW(insertOnLink(wire, "interpolate", noise, "strands", noise, "nope"), GraphError);
    const std::string scalp2 = wire.addNode("Scalp");
    const std::string withScalp = wire.toJson();
    EXPECT_THROW(insertOnLink(wire, "interpolate", noise, "strands", scalp2), GraphError) << "no input at all";
    const std::string collider = wire.addNode("ColliderMesh");
    EXPECT_THROW(insertOnLink(wire, "interpolate", noise, "strands", collider), GraphError) << "a mesh cannot feed strands";
    wire.removeNode(collider);
    EXPECT_EQ(wire.toJson(), withScalp);
    wire.removeNode(scalp2);
    EXPECT_EQ(wire.toJson(), before);
}

TEST(GraphInserts, AddScalpGrowsTheSameHairOnAnotherMesh) {
    // graph_inserts.h insertScalp: Maya's extra_scalps.py and the Blender add-on's add_scalp.
    using nlohmann::json;
    auto feeds = [](const GraphEdit& e, const std::string& from, const std::string& to, const std::string& input) {
        const json g = json::parse(e.toJson());
        for (const json& c : g["connections"]) {
            if (c["from"] == from && c["to"] == to && c["input"] == input) return true;
        }
        return false;
    };
    auto param = [](const GraphEdit& e, const std::string& node, const std::string& name) {
        const json g = json::parse(e.toJson());
        for (const json& n : g["nodes"]) {
            if (n["name"] == node) return n["params"].value(name, json());
        }
        return json();
    };
    auto fedBy = [](const GraphEdit& e, const std::string& from) {
        std::vector<std::pair<std::string, std::string>> out;
        const json g = json::parse(e.toJson());
        for (const json& c : g["connections"]) {
            if (c["from"] == from) out.emplace_back(c["to"].get<std::string>(), c["input"].get<std::string>());
        }
        return out;
    };
    GraphEdit edit(defaultGraphJson());
    // A braid's Guides node first in the graph: the new Interpolate reads the hair's guides, not it.
    json g = json::parse(edit.toJson());
    g["nodes"].insert(g["nodes"].begin(), json{{"name", "braidPaths"}, {"type", "Guides"}, {"params", {{"group", "braids"}}}});
    for (json& n : g["nodes"]) {
        if (n["name"] == "scatter") n["params"]["density"] = 7.5;
        if (n["name"] == "interpolate") n["params"]["guideCount"] = 2;
        if (n["name"] == "scalp") n["params"]["subdivision"] = "2";
    }
    edit = GraphEdit(g.dump());
    const auto fedByInterpolate = fedBy(edit, "interpolate");
    ASSERT_FALSE(fedByInterpolate.empty());
    const ScalpNodes made = insertScalp(edit, "ear");
    EXPECT_EQ(made.scalp, "scalp_ear");
    EXPECT_EQ(made.scatter, "scatter_ear");
    EXPECT_EQ(made.interpolate, "interpolate_ear");
    EXPECT_EQ(made.merge, "merge_ear");
    EXPECT_EQ(param(edit, made.scalp, "input"), "ear");
    EXPECT_EQ(param(edit, made.scalp, "subdivision"), "2") << "the own Scalp's";
    EXPECT_EQ(param(edit, made.scatter, "density"), 7.5) << "the own Scatter's params";
    EXPECT_NE(param(edit, made.scatter, "seed"), param(edit, "scatter", "seed")) << "a seed of its own";
    EXPECT_EQ(param(edit, made.interpolate, "guideCount"), 2) << "the own Interpolate's params";
    EXPECT_TRUE(feeds(edit, made.scalp, made.scatter, "scalp"));
    EXPECT_TRUE(feeds(edit, made.scatter, made.interpolate, "strands"));
    EXPECT_TRUE(feeds(edit, made.scalp, made.interpolate, "scalp"));
    EXPECT_TRUE(feeds(edit, "guides", made.interpolate, "guides"));
    EXPECT_FALSE(feeds(edit, "braidPaths", made.interpolate, "guides"));
    // The Merge right after the own Interpolate, the new hair on its b.
    EXPECT_TRUE(feeds(edit, "interpolate", made.merge, "a"));
    EXPECT_TRUE(feeds(edit, made.interpolate, made.merge, "b"));
    EXPECT_TRUE(feeds(edit, made.merge, fedByInterpolate.front().first, fedByInterpolate.front().second));
    const auto at = nodePosition(edit, made.merge);
    ASSERT_TRUE(at.has_value());
    EXPECT_EQ(nodePosition(edit, made.scalp), std::make_pair(at->first - 660.0, at->second + 160.0));
    EXPECT_EQ(nodePosition(edit, made.scatter), std::make_pair(at->first - 440.0, at->second + 160.0));
    EXPECT_EQ(nodePosition(edit, made.interpolate), std::make_pair(at->first - 220.0, at->second + 160.0));
    EXPECT_NO_THROW(loadGraphJson(edit.toJson())) << "a graph that loads";
    // The same mesh again: its Scalp node, the graph as it is.
    const std::string before = edit.toJson();
    const ScalpNodes again = insertScalp(edit, "ear");
    EXPECT_EQ(again.scalp, made.scalp);
    EXPECT_TRUE(again.scatter.empty() && again.interpolate.empty() && again.merge.empty());
    EXPECT_EQ(edit.toJson(), before);
    EXPECT_THROW(insertScalp(edit, "scalp"), GraphError);
    EXPECT_THROW(insertScalp(edit, ""), GraphError);
    EXPECT_EQ(edit.toJson(), before);

    // No Interpolate of its own (scattered hair alone): the Merge after the own Scatter, no Interpolate made.
    GraphEdit bare(R"({"nodes": [{"name": "scalp", "type": "Scalp"}, {"name": "scatter", "type": "Scatter"},
                                  {"name": "width", "type": "Width"}],
                       "connections": [{"from": "scalp", "to": "scatter", "input": "scalp"},
                                       {"from": "scatter", "to": "width", "input": "strands"}], "output": "width"})");
    const ScalpNodes few = insertScalp(bare, "brow L");
    EXPECT_EQ(few.scalp, "scalp_brow_L") << "a node name of letters, digits and _";
    EXPECT_EQ(param(bare, few.scalp, "input"), "brow L");
    EXPECT_EQ(param(bare, few.scalp, "subdivision"), "matchRender");
    EXPECT_TRUE(few.interpolate.empty());
    EXPECT_TRUE(feeds(bare, "scatter", few.merge, "a"));
    EXPECT_TRUE(feeds(bare, few.scatter, few.merge, "b"));
    EXPECT_TRUE(feeds(bare, few.merge, "width", "strands"));
    // The own Scatter feeding nothing: the Merge before the output.
    GraphEdit loose(R"({"nodes": [{"name": "scalp", "type": "Scalp"}, {"name": "scatter", "type": "Scatter"},
                                   {"name": "other", "type": "Scatter"}, {"name": "width", "type": "Width"}],
                        "connections": [{"from": "scalp", "to": "scatter", "input": "scalp"},
                                        {"from": "scalp", "to": "other", "input": "scalp"},
                                        {"from": "other", "to": "width", "input": "strands"}], "output": "width"})");
    const ScalpNodes last = insertScalp(loose, "ear");
    EXPECT_TRUE(feeds(loose, "other", last.merge, "a"));
    EXPECT_TRUE(feeds(loose, last.merge, "width", "strands"));
    EXPECT_TRUE(feeds(loose, last.scatter, last.merge, "b"));
}

TEST(GraphInserts, BraidsAndFeathersLandAlikeInEveryHost) {
    // graph_inserts.h insertBraid / insertFeathers: Maya's braids.py and feathers.py, the Blender
    // helper and the app add them through these.
    using nlohmann::json;
    auto graph = [](const GraphEdit& e) { return json::parse(e.toJson()); };
    auto param = [&](const GraphEdit& e, const std::string& node, const std::string& name) {
        const json g = graph(e);  // (held: a temporary's member would dangle in the loop)
        for (const json& n : g["nodes"]) {
            if (n["name"] == node) return n.value("params", json::object()).value(name, json());
        }
        return json();
    };
    auto source = [&](const GraphEdit& e, const std::string& to, const std::string& input) {
        const json g = graph(e);
        for (const json& c : g["connections"]) {
            if (c["to"] == to && c["input"] == input) return c["from"].get<std::string>();
        }
        return std::string();
    };
    GraphEdit edit(defaultGraphJson());
    setNodePosition(edit, "interpolate", 300.0, 0.0);
    std::string fed;  // what the Interpolate fed first
    const json start = graph(edit);
    for (const json& c : start["connections"]) {
        if (fed.empty() && c["from"] == "interpolate") fed = c["to"].get<std::string>();
    }
    ASSERT_FALSE(fed.empty());

    // A cornrow after the Interpolate, fed along the scalp, its paths a Guides node of the group.
    const std::string braid = insertBraid(edit, "braids", R"({"style": "cornrow", "width": 0.3})");
    EXPECT_EQ(edit.nodeType(braid), "Braid");
    EXPECT_EQ(source(edit, braid, "strands"), "interpolate");
    EXPECT_EQ(source(edit, fed, "strands"), braid);
    EXPECT_EQ(param(edit, braid, "feed"), true);
    EXPECT_EQ(param(edit, braid, "width"), 0.3);
    const std::string paths = source(edit, braid, "braids");
    ASSERT_EQ(edit.nodeType(paths), "Guides");
    EXPECT_EQ(param(edit, paths, "group"), "braids");
    EXPECT_EQ(param(edit, "guides", "exclude"), "braids") << "braid paths are not hair";
    EXPECT_EQ(nodePosition(edit, braid), std::make_pair(300.0 + kInsertColumn / 2.0, kInsertRow));
    EXPECT_EQ(nodePosition(edit, paths), std::make_pair(300.0, 2.0 * kInsertRow));
    // A second braid of the group reads the same paths; the exclusion is not repeated.
    const std::string second = insertBraid(edit, "braids", R"({"style": "three"})");
    EXPECT_EQ(source(edit, second, "braids"), paths);
    EXPECT_EQ(param(edit, second, "feed"), false);
    EXPECT_EQ(param(edit, "guides", "exclude"), "braids");
    EXPECT_THROW(insertBraid(edit, "braids", R"({"style": "plait"})"), GraphError);
    EXPECT_THROW(insertBraid(edit, "braids", {}, "nothing"), GraphError);
    EXPECT_THROW(insertBraid(edit, ""), GraphError);

    // Two designs sharing scattered shafts through a Split, merged with the hair before the output.
    auto design = [](const std::string& name) {
        std::ifstream in(std::filesystem::u8path(presetPath("feathers_" + name, {GROOMLAB_SHIPPED_PRESETS})));
        std::stringstream text;
        text << in.rdbuf();
        return FeatherDesign{name, presetGraphJson(text.str(), std::nullopt)};
    };
    const std::string output = edit.outputNode();
    const std::string hair = source(edit, output, "strands");
    setNodePosition(edit, output, 1000.0, 0.0);
    const FeatherNodes made = insertFeathers(edit, {design("contour"), design("down")}, {3.0, 1.0});
    EXPECT_EQ(edit.nodeType(made.shafts), "Interpolate");
    EXPECT_EQ(edit.nodeType(source(edit, made.shafts, "strands")), "Scatter");
    EXPECT_EQ(source(edit, made.split, "strands"), made.shafts);
    EXPECT_EQ(param(edit, made.split, "weightA"), 3.0);
    EXPECT_EQ(param(edit, made.split, "weightC"), 0.0);
    ASSERT_EQ(made.designs.size(), 2u);
    ASSERT_EQ(made.merges.size(), 1u);
    EXPECT_EQ(source(edit, made.designs[1], "shafts"), made.split);
    EXPECT_EQ(source(edit, output, "strands"), made.merge);
    EXPECT_EQ(source(edit, made.merge, "a"), hair);
    EXPECT_EQ(source(edit, made.merge, "b"), made.merges.front());
    EXPECT_EQ((featherGroups(edit)), (std::map<std::string, int>{{"contour", 1}, {"down", 2}}));
    EXPECT_EQ(nodePosition(edit, made.designs[0]), std::make_pair(1000.0 - 2.0 * kInsertColumn, 2.0 * kInsertRow));
    EXPECT_TRUE(featherGuideGroups(edit).empty()) << "scattered shafts are no guide group";

    // Hand-placed: a guide group's guides, the same design joining its groom group, the hair's
    // Guides leaving the group out; without keepHair the feathers feed the output alone.
    const FeatherNodes placed = insertFeathers(edit, {design("contour")}, {}, "guides", "flight", false);
    EXPECT_EQ(param(edit, placed.shafts, "group"), "flight");
    EXPECT_TRUE(placed.split.empty());
    EXPECT_TRUE(placed.merge.empty());
    EXPECT_EQ(source(edit, output, "strands"), placed.designs.front());
    EXPECT_EQ(param(edit, "guides", "exclude"), "braids flight");
    EXPECT_EQ(featherGroups(edit).at("contour"), 1);
    EXPECT_EQ(featherGuideGroups(edit), std::vector<std::string>{"flight"});
    // A node's output as the shafts.
    const FeatherNodes split = insertFeathers(edit, {design("down")}, {}, made.split + ".b");
    EXPECT_EQ(split.shafts, made.split);
    EXPECT_EQ(featherGroups(edit).at("down"), 2);

    EXPECT_THROW(insertFeathers(edit, {}), GraphError);
    EXPECT_THROW(insertFeathers(edit, {design("down"), design("down")}, {1.0}), GraphError);
    EXPECT_THROW(insertFeathers(edit, {design("down")}, {}, "nothing"), GraphError);
    EXPECT_THROW(insertFeathers(edit, {FeatherDesign{"empty", R"({"nodes": []})"}}), GraphError);
    EXPECT_FALSE(excludeFromHairGuides(edit, "flight")) << "already left out";
}

TEST(GraphControls, EveryHostMakesAndReadsControlsAlike) {
    // graph_controls.h: Maya's params.py, the Blender helper and the app make controls through it.
    using nlohmann::json;
    EXPECT_EQ(controlName("width"), "width");
    EXPECT_EQ(controlName("noise.amplitude"), "noise_amplitude");
    EXPECT_EQ(controlName("3d"), "p_3d");
    EXPECT_EQ(controlName(""), "p_");
    GraphEdit edit(defaultGraphJson());
    // A Control node driving width.width: named after the param, its slider 0-1 widened to take
    // the value in, the param exposed and connected; a second one numbered; a name asked for and
    // taken refused.
    const std::string control = addControl(edit, "width", "width", {}, 3.0);
    json g = json::parse(edit.toJson());
    json made, width;
    for (const json& n : g["nodes"]) {
        if (n["name"] == control) made = n;
        if (n["name"] == "width") width = n;
    }
    EXPECT_EQ(made["type"], "Control");
    EXPECT_EQ(made["params"]["name"], "width");
    EXPECT_DOUBLE_EQ(made["params"]["default"].get<double>(), 3.0);
    EXPECT_DOUBLE_EQ(made["params"]["min"].get<double>(), 0.0);
    EXPECT_DOUBLE_EQ(made["params"]["max"].get<double>(), 3.0) << "widened to take the start in";
    EXPECT_NE(std::find(width["paramInputs"].begin(), width["paramInputs"].end(), json("width")), width["paramInputs"].end());
    const double widthX = width.contains("ui") ? width["ui"].value("x", 0.0) : 0.0;  // (the default graph: no positions)
    EXPECT_DOUBLE_EQ(made["ui"]["x"].get<double>(), widthX - 240.0);
    const std::string second = addControl(edit, "length", "fraction");
    EXPECT_THROW(addControl(edit, "noise", "amplitude", "width"), GraphError);
    // Keyed params: <node>_<param>, a vector three, the value stored; keyed again: the same.
    EXPECT_EQ(linkControl(edit, "noise", "amplitude"), "\"noise_amplitude\"");
    EXPECT_EQ(linkControl(edit, "noise", "amplitude"), "\"noise_amplitude\"");
    EXPECT_EQ(linkControl(edit, "color", "dye"), R"(["color_dyeR","color_dyeG","color_dyeB"])");
    EXPECT_EQ(linkControl(edit, "length", "randomness", {}, {"length_randomness"}), "\"length_randomness1\"") << "a name the host holds";
    EXPECT_NE(notKeyable(edit, "scatter", "density"), "") << "not animatable";
    EXPECT_NE(notKeyable(edit, control, "default"), "") << "a Control node's own";
    EXPECT_EQ(notKeyable(edit, "width", "width"), "an input drives it: unplug its input to key it") << "a Control drives it";
    EXPECT_THROW(linkControl(edit, "scatter", "density"), GraphError);
    // Every control read back: a keyed param's default its own value, else its spec's.
    const json controls = json::parse(controlsJson(edit.toJson()));
    EXPECT_EQ(controls["width"]["kind"], "control");
    EXPECT_EQ(controls["noise_amplitude"]["kind"], "keyed");
    EXPECT_EQ(controls["color_dyeG"]["component"], 1);
    json unset = json::parse(edit.toJson());
    for (json& n : unset["nodes"]) {
        if (n["name"] == "noise") n["params"].erase("amplitude");
    }
    const json spec = json::parse(GraphEdit(unset.dump()).nodeSpecJson("noise"));
    double amplitudeDefault = 0.0;
    for (const json& p : spec["params"]) {
        if (p["name"] == "amplitude") amplitudeDefault = p["default"].get<double>();
    }
    EXPECT_DOUBLE_EQ(json::parse(controlsJson(unset.dump()))["noise_amplitude"]["default"].get<double>(), amplitudeDefault);
    // Remove Animation: the param back to its control's value now (a vector's three), the link gone.
    EXPECT_EQ(unlinkControl(edit, "noise", "amplitude", {{"noise_amplitude", 0.25f}}), "0.25");
    EXPECT_EQ(unlinkControl(edit, "color", "dye", {{"color_dyeG", 0.5f}}).substr(0, 1), "[");
    EXPECT_EQ(unlinkControl(edit, "noise", "amplitude"), "") << "not keyed any more";
    const json after = json::parse(edit.toJson());
    for (const json& n : after["nodes"]) {
        if (n["name"] == "noise") {
            EXPECT_FALSE(n.contains("paramControls"));
            EXPECT_DOUBLE_EQ(n["params"]["amplitude"].get<double>(), 0.25);
        }
        if (n["name"] == "color") EXPECT_DOUBLE_EQ(n["params"]["dye"][1].get<double>(), 0.5);
    }
    EXPECT_TRUE(json::parse(controlsJson(edit.toJson())).contains("length_randomness1")) << "the others stay keyed";
    (void)second;
}

TEST(GraphEdit, EveryHostEditsAGraphAlike) {
    // Maya's GraphModel and the app's node graph edit through this (graph_edit.h): names, seeds,
    // positions, connections with the type and cycle checks, a compound's ports, the warnings.
    GraphEdit edit(defaultGraphJson());
    edit.removeNode("noise");  // the chain without the default graph's look: Length -> Width
    edit.removeNode("color");
    edit.connect("length", "width", "strands");
    EXPECT_EQ(edit.uniqueName("Noise"), "noise");
    EXPECT_EQ(edit.addNode("Noise"), "noise");
    EXPECT_EQ(edit.addNode("Noise"), "noise1");
    EXPECT_EQ(edit.uniqueName("scatter"), "scatter1");
    EXPECT_THROW(edit.addNode("Noise", "noise"), GraphError);  // a name taken
    EXPECT_THROW(edit.addNode("Teleport"), GraphError);
    const std::string scatter2 = edit.addNode("Scatter", {}, {}, 10.0, 20.0);
    nlohmann::json g = nlohmann::json::parse(edit.toJson());
    const auto node = [&g](const std::string& name) {
        for (const nlohmann::json& n : g["nodes"]) {
            if (n["name"] == name) return n;
        }
        return nlohmann::json();
    };
    EXPECT_EQ(node(scatter2)["ui"], nlohmann::json({{"x", 10.0}, {"y", 20.0}}));
    EXPECT_TRUE(node(scatter2)["params"].contains("seed"));  // the default is the first Scatter's
    EXPECT_DOUBLE_EQ(node("noise1")["ui"]["x"].get<double>(), node("noise")["ui"]["x"].get<double>() + 240.0);
    EXPECT_TRUE(edit.seedWarnings().empty());
    GraphEdit shared(edit);
    shared.addNode("Scatter", "scatter3", R"({"seed": 0})");
    shared.addNode("Scatter", "scatter4", R"({"seed": 0})");
    ASSERT_EQ(shared.seedWarnings().size(), 1u);
    EXPECT_NE(shared.seedWarnings()[0].find("scatter3 and scatter4 share seed 0"), std::string::npos);

    // Connections: what fed the input is replaced; types must fit; no cycle; the output named
    // only when it is not the first.
    edit.connect("length", "noise", "strands");
    EXPECT_EQ(edit.outputType("noise"), "Strands");  // Curves gives what feeds it
    try {
        edit.connect("scalp", "noise1", "strands");
        ADD_FAILURE() << "a mesh into strands";
    } catch (const GraphError& e) {
        EXPECT_EQ(std::string(e.what()), "cannot connect scalp (it gives Mesh) to noise1.strands (it takes strands or guides)");
    }
    try {
        edit.connect("noise", "scatter", "scalp");
        ADD_FAILURE() << "a cycle";
    } catch (const GraphError& e) {
        EXPECT_NE(std::string(e.what()).find("cannot connect"), std::string::npos);  // a Curves into a Mesh: the type first
    }
    edit.connect("noise", "width", "strands");
    EXPECT_THROW(edit.connect("width", "length", "strands"), GraphError);  // width is downstream of length now
    EXPECT_THROW(edit.connect("noise", "width", "nope"), GraphError);
    const std::vector<std::string> up = edit.upstream("width");
    EXPECT_NE(std::find(up.begin(), up.end(), "scatter"), up.end());
    edit.addNode("Map", "mask");
    EXPECT_THROW(edit.connect("mask", "noise1", "amplitude"), GraphError);  // not exposed: no such input
    edit.removeNode("noise");
    EXPECT_FALSE(edit.hasNode("noise"));
    // The parsed graph held in a local: a range-for over a temporary's member dangles.
    const nlohmann::json removed = nlohmann::json::parse(edit.toJson());
    for (const nlohmann::json& c : removed["connections"]) {
        EXPECT_NE(c.value("from", std::string()), "noise");
        EXPECT_NE(c.value("to", std::string()), "noise");
    }
    // A rename carries the connections and the output; the output can be chosen; the positions
    // can be dropped for a layout afresh.
    EXPECT_EQ(edit.outputNode(), "output");
    edit.renameNode("output", "result");
    EXPECT_TRUE(edit.hasNode("result"));
    EXPECT_FALSE(edit.hasNode("output"));
    EXPECT_EQ(edit.outputNode(), "result");
    bool fedResult = false;
    const nlohmann::json renamed = nlohmann::json::parse(edit.toJson());
    for (const nlohmann::json& c : renamed["connections"]) fedResult = fedResult || c.value("to", std::string()) == "result";
    EXPECT_TRUE(fedResult);
    EXPECT_THROW(edit.renameNode("result", "scatter"), GraphError);  // taken
    EXPECT_THROW(edit.renameNode("result", ""), GraphError);
    EXPECT_THROW(edit.renameNode("nope", "x"), GraphError);
    edit.setOutputNode("width");
    EXPECT_EQ(edit.outputNode(), "width");
    EXPECT_THROW(edit.setOutputNode("nope"), GraphError);
    edit.clearPositions();
    const nlohmann::json tidied = nlohmann::json::parse(edit.toJson());
    for (const nlohmann::json& n : tidied["nodes"]) EXPECT_FALSE(n.contains("ui"));

    // An exposed param's port, a Map node's outputs, a compound's ports from its definition.
    GraphEdit ports(R"({"nodes": [{"name": "mask", "type": "Map"},
        {"name": "noise", "type": "Noise", "paramInputs": ["amplitude"]},
        {"name": "group", "type": "Compound", "params": {"definition": {"inputs": [{"name": "hair"}],
            "graph": {"nodes": [{"name": "in_hair", "type": "Input", "params": {"name": "hair"}},
                                {"name": "lift", "type": "Bend", "params": {"end": 0.0}}],
                      "connections": [{"from": "in_hair", "to": "lift", "input": "strands"}], "output": "lift"}}}}],
        "connections": []})");
    ports.connect("mask", "noise", "amplitude", "r");
    ports.connect("mask", "noise", "amplitude");  // replaced, by the first output: named none
    const nlohmann::json links = nlohmann::json::parse(ports.toJson())["connections"];
    ASSERT_EQ(links.size(), 1u);
    EXPECT_FALSE(links[0].contains("output"));
    ports.connect("mask", "noise", "amplitude", "r");
    EXPECT_EQ(nlohmann::json::parse(ports.toJson())["connections"][0]["output"], "r");
    const nlohmann::json spec = nlohmann::json::parse(ports.nodeSpecJson("group"));
    ASSERT_EQ(spec["inputs"].size(), 1u);
    EXPECT_EQ(spec["inputs"][0]["name"], "hair");
    EXPECT_EQ(spec["outputs"][0]["type"], "Curves");  // Lift's
    const nlohmann::json noise = nlohmann::json::parse(ports.nodeSpecJson("noise"));
    EXPECT_EQ(noise["inputs"].back()["name"], "amplitude");
    EXPECT_EQ(noise["inputs"].back()["type"], "Field");
    EXPECT_EQ(nlohmann::json::parse(nodeSpecJson(R"({"type": "Input", "params": {"type": "Guides"}})"))["outputs"][0]["type"], "Guides");
}


TEST(GraphInserts, EveryHostLaysAGraphOutAlike) {
    // graph_inserts.h layoutPositions: Maya's and Blender's auto layout and the app's (its own
    // spacing, a long chain wrapped into blocks over the viewport).
    const GraphEdit edit(defaultGraphJson());
    const auto at = layoutPositions(edit);
    EXPECT_EQ(at.size(), edit.nodeNames().size());
    // Columns by the longest path in from a source, rows in the graph's order.
    EXPECT_EQ(at.at("scalp"), std::make_pair(0.0, 0.0));
    EXPECT_EQ(at.at("guides"), std::make_pair(0.0, kInsertRow));
    EXPECT_EQ(at.at("scatter"), std::make_pair(kInsertColumn, 0.0));
    EXPECT_EQ(at.at("interpolate"), std::make_pair(2 * kInsertColumn, 0.0));
    EXPECT_EQ(at.at("output"), std::make_pair(8 * kInsertColumn, 0.0));
    // Blocks of 7 columns, one under the other, each as tall as its tallest column plus the gap.
    GraphLayout wrapped;
    wrapped.column = 230.0;
    wrapped.row = 120.0;
    wrapped.columnsPerBlock = 7;
    const auto blocks = layoutPositions(edit, wrapped);
    EXPECT_EQ(blocks.at("guides"), std::make_pair(0.0, 120.0));
    EXPECT_EQ(blocks.at("width"), std::make_pair(6 * 230.0, 0.0));
    EXPECT_EQ(blocks.at("resample"), std::make_pair(0.0, 2 * 120.0 + 60.0));
    EXPECT_EQ(blocks.at("output"), std::make_pair(230.0, 2 * 120.0 + 60.0));
    // Only some nodes (the ones with no position yet): their columns the whole graph's, the rows
    // counted among them.
    const std::vector<std::string> only = {"guides", "output"};
    const auto some = layoutPositions(edit, {}, &only);
    EXPECT_EQ(some.size(), 2u);
    EXPECT_EQ(some.at("guides"), std::make_pair(0.0, 0.0));
    EXPECT_EQ(some.at("output"), std::make_pair(8 * kInsertColumn, 0.0));
    // A cycle (a graph written by hand) still lays out.
    const GraphEdit loop(R"({"nodes": [{"name": "a", "type": "Noise"}, {"name": "b", "type": "Noise"}], "connections": [
        {"from": "a", "to": "b", "input": "strands"}, {"from": "b", "to": "a", "input": "strands"}]})");
    EXPECT_EQ(layoutPositions(loop).size(), 2u);
    EXPECT_TRUE(layoutPositions(GraphEdit("{}")).empty());
}

TEST(GraphEdit, EveryHostCopiesAndPastesNodesAlike) {
    // graph_edit.h copyNodes / pasteNodes: Maya's GraphModel.copy_nodes / paste_nodes and the app's
    // GroomDocument make them through these.
    using nlohmann::json;
    GraphEdit edit(defaultGraphJson());
    setNodePosition(edit, "length", 100.0, 50.0);
    exposeParam(edit, "noise", "amplitude");
    // In the graph's order, with the links among them; the Output node is one of a kind.
    const std::string text = edit.copyNodes({"output", "noise", "length", "nope"});
    const json copied = json::parse(text);
    EXPECT_EQ(copied["groomlabNodes"], 1);
    ASSERT_EQ(copied["nodes"].size(), 2u);
    EXPECT_EQ(copied["nodes"][0]["name"], "length");
    EXPECT_EQ(copied["nodes"][1]["name"], "noise");
    ASSERT_EQ(copied["connections"].size(), 1u);
    EXPECT_EQ(copied["connections"][0]["from"], "length");
    EXPECT_EQ(edit.copyNodes({"output"}), "");
    EXPECT_EQ(edit.copyNodes({}), "");

    const std::string before = edit.toJson();
    const auto made = edit.pasteNodes(text, 40.0, 60.0);
    ASSERT_EQ(made.size(), 2u);
    EXPECT_EQ(made[0], std::make_pair(std::string("length1"), std::string("length")));
    EXPECT_EQ(made[1], std::make_pair(std::string("noise1"), std::string("noise")));
    const json g = json::parse(edit.toJson());
    json length1, noise1, length;
    for (const json& n : g["nodes"]) {
        if (n["name"] == "length1") length1 = n;
        if (n["name"] == "noise1") noise1 = n;
        if (n["name"] == "length") length = n;
    }
    // Every field as copied, moved by the offset.
    EXPECT_EQ(length1["params"], length["params"]);
    EXPECT_DOUBLE_EQ(length1["ui"]["x"].get<double>(), 140.0);
    EXPECT_DOUBLE_EQ(length1["ui"]["y"].get<double>(), 110.0);
    EXPECT_EQ(noise1["paramInputs"], json::array({"amplitude"}));
    // The link among them made again, none to the rest of the graph.
    int links = 0;
    for (const json& c : g["connections"]) {
        if (c["to"] == "noise1") {
            EXPECT_EQ(c["from"], "length1");
            ++links;
        }
        EXPECT_NE(c["to"], "length1");
        EXPECT_FALSE(c["from"] == "noise1");
    }
    EXPECT_EQ(links, 1);
    EXPECT_EQ(edit.outputNode(), "output");
    // Pasted again: the next free names.
    EXPECT_EQ(edit.pasteNodes(text)[0].first, "length2");

    // Text that holds no nodes, a type this build lacks, a link that does not fit: the graph as it was.
    GraphEdit same(before);
    EXPECT_THROW(same.pasteNodes("not nodes"), GraphError);
    EXPECT_THROW(same.pasteNodes("{}"), GraphError);
    EXPECT_THROW(same.pasteNodes(R"({"groomlabNodes": 1, "nodes": []})"), GraphError);
    EXPECT_THROW(same.pasteNodes(R"({"groomlabNodes": 1, "nodes": [{"name": "x", "type": "NoSuchNode"}]})"), GraphError);
    EXPECT_THROW(same.pasteNodes(R"({"groomlabNodes": 1, "nodes": [{"name": "a", "type": "Scalp"}, {"name": "b", "type": "Noise"}],
        "connections": [{"from": "a", "to": "b", "input": "strands"}]})"), GraphError);
    EXPECT_EQ(same.toJson(), before);
    // An Output node in a clipboard (one copied before the rule) is left out.
    const auto one = same.pasteNodes(R"({"groomlabNodes": 1, "nodes": [{"name": "output", "type": "Output"}, {"type": "Noise"}]})");
    ASSERT_EQ(one.size(), 1u);
    EXPECT_EQ(one[0].first, "noise1");
    EXPECT_THROW(same.pasteNodes(R"({"groomlabNodes": 1, "nodes": [{"name": "output", "type": "Output"}]})"), GraphError);
}

TEST(GraphControls, EveryHostExposesAndUnexposesParamsAlike) {
    // graph_controls.h unexposeParam, compoundExpose, compoundUnexpose: Maya's GraphModel, the
    // Blender helper's edit_graph ops and the app's GroomDocument make them through these.
    using nlohmann::json;
    GraphEdit edit(defaultGraphJson());
    auto nodeOf = [&](const std::string& name) {
        const json g = json::parse(edit.toJson());
        for (const json& n : g["nodes"]) {
            if (n["name"] == name) return n;
        }
        return json();
    };
    exposeParam(edit, "noise", "amplitude");
    exposeParam(edit, "noise", "frequency");
    const std::string field = edit.addNode("FieldNoise");
    edit.connect(field, "noise", "amplitude");
    // The port goes with what fed it; the others stay.
    EXPECT_TRUE(unexposeParam(edit, "noise", "amplitude"));
    EXPECT_EQ(nodeOf("noise")["paramInputs"], json::array({"frequency"}));
    const json unexposed = json::parse(edit.toJson());
    for (const json& c : unexposed["connections"]) EXPECT_NE(c["from"], field);
    EXPECT_FALSE(unexposeParam(edit, "noise", "amplitude")) << "no port: nothing changes";
    EXPECT_TRUE(unexposeParam(edit, "noise", "frequency"));
    EXPECT_FALSE(nodeOf("noise").contains("paramInputs"));
    EXPECT_THROW(unexposeParam(edit, "nope", "amplitude"), GraphError);

    // A compound shows an inner param under a name, and stops: the value it set goes back inside.
    const std::string compound = edit.collapse({"length", "noise"});
    compoundExpose(edit, compound, "wave", "noise.amplitude");
    EXPECT_EQ(nodeOf(compound)["params"]["definition"]["expose"]["wave"], "noise.amplitude");
    bool shown = false;
    const json spec = json::parse(edit.nodeSpecJson(compound));
    for (const json& p : spec["params"]) shown = shown || p["name"] == "wave";
    EXPECT_TRUE(shown);
    {  // No name: the param's own, or node + Param when the compound has one of that name (every host's).
        GraphEdit named = edit;
        EXPECT_EQ(compoundExpose(named, compound, "", "noise.frequency"), "frequency");
        EXPECT_EQ(compoundExpose(named, compound, "", "noise.frequency"), "noiseFrequency");
    }
    EXPECT_THROW(compoundExpose(edit, compound, "x", "amplitude"), GraphError);
    EXPECT_THROW(compoundExpose(edit, compound, "x", "noise.nope"), GraphError);
    EXPECT_THROW(compoundExpose(edit, compound, "x", "nope.amplitude"), GraphError);
    EXPECT_THROW(compoundExpose(edit, "width", "x", "noise.amplitude"), GraphError);
    {
        json g = json::parse(edit.toJson());
        for (json& n : g["nodes"]) {
            if (n["name"] == compound) n["params"]["wave"] = 0.75;
        }
        edit = GraphEdit(g.dump());
    }
    compoundExpose(edit, compound, "wave2", "noise.amplitude");
    EXPECT_TRUE(compoundUnexpose(edit, compound, "noise.amplitude"));
    const json after = nodeOf(compound);
    EXPECT_TRUE(after["params"]["definition"]["expose"].empty());
    EXPECT_FALSE(after["params"].contains("wave"));
    for (const json& inner : after["params"]["definition"]["graph"]["nodes"]) {
        if (inner["name"] == "noise") EXPECT_DOUBLE_EQ(inner["params"]["amplitude"].get<double>(), 0.75);
    }
    EXPECT_FALSE(compoundUnexpose(edit, compound, "noise.amplitude"));
    EXPECT_THROW(compoundUnexpose(edit, "width", "noise.amplitude"), GraphError);
}

TEST(ParamSpecs, EachDefaultIsWrittenOnceInItsSpec) {
    // A param's default lives in its spec alone: every getter falls back to it (ParamSet), so an
    // operator reads its params without a fallback of its own. A stored value of another type
    // than the spec's reads as the spec default, for every param of every operator.
    const auto unit = [](float x) { return std::clamp(x, 0.0f, 1.0f); };
    for (const std::string& type : OperatorRegistry::instance().typeNames()) {
        const std::unique_ptr<Operator> op = OperatorRegistry::instance().create(type);
        for (const ParamSpec& spec : op->paramSpecs()) {
            ParamSet p = op->params();
            const bool text = std::holds_alternative<std::string>(spec.defaultValue);
            p.set(spec.name, text ? ParamValue(Vec3f(9.0f, 9.0f, 9.0f)) : ParamValue(std::string("not this")));
            ASSERT_NE(p.defaultValue(spec.name), nullptr) << type << "." << spec.name;
            const std::string where = type + "." + spec.name;
            if (const bool* b = std::get_if<bool>(&spec.defaultValue)) EXPECT_EQ(p.getBool(spec.name, !*b), *b) << where;
            if (const auto* i = std::get_if<std::int64_t>(&spec.defaultValue)) EXPECT_EQ(p.getInt(spec.name, *i + 7), *i) << where;
            if (const double* d = std::get_if<double>(&spec.defaultValue)) EXPECT_EQ(p.getDouble(spec.name, *d + 7.0), *d) << where;
            if (const auto* s = std::get_if<std::string>(&spec.defaultValue)) EXPECT_EQ(p.getString(spec.name, *s + "x"), *s) << where;
            if (const Vec3f* v = std::get_if<Vec3f>(&spec.defaultValue)) EXPECT_EQ(p.getVec3(spec.name), *v) << where;
            if (const Ramp* r = std::get_if<Ramp>(&spec.defaultValue)) {
                const Ramp got = p.getRamp(spec.name, Ramp::constant(0.123f));
                for (const float t : {0.0f, 0.3f, 0.7f, 1.0f}) EXPECT_FLOAT_EQ(got(t), unit((*r)(t))) << where << " at " << t;
            }
        }
    }
    // The simulation's settings struct starts where the Simulate node's specs do.
    EXPECT_EQ(SimSettings::fromParams(OperatorRegistry::instance().create(op::kSimulate)->params()).hash(), SimSettings{}.hash());
    // No operator writes a default a second time: no getter or field there takes a fallback.
    const std::filesystem::path root = std::filesystem::u8path(GROOMLAB_TEST_DATA) / ".." / "..";
    const std::regex fallback(R"((params\(\)|\bp|\bparams)\s*\.\s*get(Bool|Int|Double|String|Vec3|Ramp)\(\s*"[^"]*"\s*,)");
    const std::regex fieldFallback(R"(ScalarField\b[^;]*\(\s*params\(\)\s*,\s*"[^"]*"\s*,\s*-?[0-9.]+f?\s*,)");
    std::size_t files = 0;
    for (const char* dir : {"engine/core/src/operators", "engine/gpu/src"}) {
        for (const auto& entry : std::filesystem::directory_iterator(root / dir)) {
            if (entry.path().extension() != ".cpp") continue;
            std::ifstream in(entry.path());
            std::stringstream text;
            text << in.rdbuf();
            const std::string source = text.str();
            EXPECT_FALSE(std::regex_search(source, fallback)) << entry.path().generic_string() << " reads a param with a fallback";
            EXPECT_FALSE(std::regex_search(source, fieldFallback)) << entry.path().generic_string();
            ++files;
        }
    }
    EXPECT_GT(files, 15u);
}

TEST(GraphEdit, ProblemsSayWhatStopsTheGroomFromEvaluating) {
    // Every host's check before a graph is applied (Maya's GraphModel.validate and unused_problems
    // read these): the graph's own problems and those of the nodes the output is made from stop
    // it; a node off to the side only warns.
    const auto has = [](const std::vector<std::string>& all, const std::string& one) {
        return std::find(all.begin(), all.end(), one) != all.end();
    };
    GraphEdit edit(defaultGraphJson());
    EXPECT_TRUE(edit.problems().empty());
    EXPECT_TRUE(edit.problems(false).empty());
    const std::vector<std::string> feeding = edit.feedingOutput();
    EXPECT_EQ(feeding.size(), edit.nodeNames().size());
    edit.addNode("Clump", "side");
    EXPECT_FALSE(has(edit.feedingOutput(), "side"));
    EXPECT_TRUE(edit.problems().empty());  // never evaluated: a warning
    EXPECT_TRUE(has(edit.problems(false), "side.strands is not connected"));
    edit.disconnect("length", "strands");
    EXPECT_EQ(edit.problems(), (std::vector<std::string>{"length.strands is not connected"}));
    // (What fed length is off to the side now, whole.)
    EXPECT_FALSE(has(edit.feedingOutput(), "interpolate"));

    // Graphs edited outside connect's checks: a link to an output its source lacks, a link whose
    // types do not fit, a map with no name, in the graph's order.
    nlohmann::json g = nlohmann::json::parse(defaultGraphJson());
    for (nlohmann::json& c : g["connections"]) {
        if (c["to"] == "length" && c["input"] == "strands") c["from"] = "scalp";
        if (c["to"] == "noise" && c["input"] == "strands") c["output"] = "q";
    }
    for (nlohmann::json& n : g["nodes"]) {
        if (n["name"] == "length") n["params"]["fraction"] = {{"texture", ""}};
        if (n["name"] == "width") n["params"]["width"] = {{"guide", "taper"}};  // named: fine
    }
    EXPECT_EQ(GraphEdit(g.dump()).problems(),
              (std::vector<std::string>{"scalp gives Mesh but length.strands takes strands or guides", "length.fraction: the texture has no name",
                                        "length has no output 'q' (noise.strands reads it)"}));
    // The graph's own, first.
    g["output"] = "ghost";
    EXPECT_EQ(GraphEdit(g.dump()).problems(), (std::vector<std::string>{"output node 'ghost' does not exist"}));
    EXPECT_EQ(GraphEdit(g.dump()).problems(false).size(), 3u);  // nothing feeds an output: every node's are warnings
    g.erase("output");
    EXPECT_EQ(GraphEdit(g.dump()).problems(), (std::vector<std::string>{"no output node"}));
    // A node this GroomLab has no type for is a problem, not a throw.
    g["output"] = "output";
    g["nodes"].push_back({{"name", "odd"}, {"type", "Teleport"}});
    EXPECT_TRUE(has(GraphEdit(g.dump()).problems(false), "odd: unknown operator type 'Teleport'"));
}

TEST(GraphEdit, MovedGuidesLinkedByHandMakeAClumpFollowThem) {
    // Guides a Simulate node moves, linked by hand into a picking Clump's centers, directly or
    // through a curve node: each clump along its guide (inputCurves), as a Guides node's are (the
    // GuideCache's and ImportCurves': test_guide_cache.cpp, where those types are registered).
    for (const char* kind : {"Simulate", "Guides"}) {
        for (const char* source : {"moved", "wave"}) {
            const nlohmann::json g = {
                {"version", 1},
                {"nodes",
                 {{{"name", "s"}, {"type", "Scatter"}},
                  {{"name", "c"}, {"type", "Clump"}},
                  {{"name", "moved"}, {"type", kind}},
                  {{"name", "wave"}, {"type", "Noise"}}}},
                {"connections",
                 {{{"from", "s"}, {"to", "c"}, {"input", "strands"}}, {{"from", "moved"}, {"to", "wave"}, {"input", "strands"}}}}};
            GraphEdit edit(g.dump());
            EXPECT_TRUE(edit.connectByHand(source, "c", "centers")) << kind << " " << source;
            const nlohmann::json now = nlohmann::json::parse(edit.toJson());
            EXPECT_EQ(now["nodes"][1]["params"].value("centersFrom", std::string()), "inputCurves") << kind << " " << source;
        }
    }
}

TEST(GraphEdit, DetachTakesANodeOutOfTheFlowAndKeepsIt) {
    // Detach (every host's node menu, Ctrl+Shift+X; issue #80): what fed the node feeds what it fed,
    // the graph's output passes on, the node stays unlinked below the new link.
    const nlohmann::json g = {
        {"version", 1},
        {"nodes",
         {{{"name", "s"}, {"type", "Scatter"}, {"ui", {{"x", 0.0}, {"y", 0.0}}}},
          {{"name", "c"}, {"type", "Clump"}, {"ui", {{"x", 200.0}, {"y", 0.0}}}},
          {{"name", "n"}, {"type", "Noise"}, {"ui", {{"x", 400.0}, {"y", 20.0}}}},
          {{"name", "w"}, {"type", "Width"}, {"ui", {{"x", 600.0}, {"y", 0.0}}}}}},
        {"connections",
         {{{"from", "s"}, {"to", "c"}, {"input", "strands"}},
          {{"from", "c"}, {"to", "n"}, {"input", "strands"}},
          {{"from", "n"}, {"to", "w"}, {"input", "strands"}}}},
        {"output", "w"}};
    GraphEdit edit(g.dump());
    const GraphEdit::Detached done = edit.detach("n");
    EXPECT_TRUE(done.changed);
    EXPECT_EQ(done.source, "c");
    ASSERT_EQ(done.relinked.size(), 1u);
    EXPECT_EQ(done.relinked[0], std::make_pair(std::string("w"), std::string("strands")));
    nlohmann::json now = nlohmann::json::parse(edit.toJson());
    for (const nlohmann::json& c : now["connections"]) {
        EXPECT_NE(c["from"], "n");
        EXPECT_NE(c["to"], "n");
    }
    EXPECT_TRUE(edit.hasNode("n"));
    EXPECT_EQ(edit.upstream("w"), (std::vector<std::string>{"c", "s"}));
    const nlohmann::json& ui = now["nodes"][2]["ui"];
    EXPECT_DOUBLE_EQ(ui["x"].get<double>(), 400.0);  // midway between c and w
    EXPECT_DOUBLE_EQ(ui["y"].get<double>(), 90.0);   // below the new link
    EXPECT_FALSE(edit.detach("n").changed);            // already out: nothing to do

    // The output node: the output passes to what fed it.
    GraphEdit last(g.dump());
    last.detach("w");
    EXPECT_EQ(last.outputNode(), "n");
    // The first node: nothing fed it, so what it fed is left unconnected.
    GraphEdit first(g.dump());
    const GraphEdit::Detached head = first.detach("s");
    EXPECT_TRUE(head.changed);
    EXPECT_TRUE(head.source.empty());
    EXPECT_TRUE(first.upstream("c").empty());
    EXPECT_THROW(first.detach("nope"), GraphError);
}

TEST(GraphEdit, ClumpColorsNameTheirClumpAndLightIt) {
    // Clump Colors (every host's button, issue #77): each Clump's source names it, so chained
    // Clumps writing one channel light alone; a bare channel (grooms from before) lights the last.
    const nlohmann::json g = {
        {"version", 1},
        {"nodes",
         {{{"name", "s"}, {"type", "Scatter"}},
          {{"name", "c1"}, {"type", "Clump"}},
          {{"name", "c2"}, {"type", "Clump"}, {"params", {{"clumpChannel", "clumpId"}}}},
          {{"name", "big"}, {"type", "Clump"}, {"params", {{"clumpChannel", "bigClumpId"}}}},
          {{"name", "none"}, {"type", "Clump"}, {"params", {{"clumpChannel", ""}}}},
          {{"name", "w"}, {"type", "Width"}}}},
        {"connections",
         {{{"from", "s"}, {"to", "c1"}, {"input", "strands"}},
          {{"from", "c1"}, {"to", "c2"}, {"input", "strands"}},
          {{"from", "c2"}, {"to", "big"}, {"input", "strands"}},
          {{"from", "big"}, {"to", "none"}, {"input", "strands"}},
          {{"from", "none"}, {"to", "w"}, {"input", "strands"}}}},
        {"output", "w"}};
    const GraphEdit edit(g.dump());
    EXPECT_EQ(edit.clumpChannel("c1"), "clumpId") << "unset: the default";
    EXPECT_EQ(edit.clumpChannel("w"), "") << "not a Clump";
    EXPECT_EQ(edit.clumpColorSource("c1"), "clumpId@c1");
    EXPECT_EQ(edit.clumpColorSource("big"), "bigClumpId@big");
    EXPECT_EQ(edit.clumpColorSource("none"), "") << "writes no channel";
    EXPECT_EQ(edit.clumpColorSource("nope"), "");
    EXPECT_TRUE(edit.clumpColored("c1", "clumpId@c1"));
    EXPECT_FALSE(edit.clumpColored("c2", "clumpId@c1")) << "the other Clump on the channel stays dark";
    EXPECT_FALSE(edit.clumpColored("big", "clumpId@c1"));
    EXPECT_FALSE(edit.clumpColored("c1", "clumpId")) << "a bare channel: the last Clump writing it";
    EXPECT_TRUE(edit.clumpColored("c2", "clumpId"));
    EXPECT_TRUE(edit.clumpColored("big", "bigClumpId"));
    EXPECT_FALSE(edit.clumpColored("none", ""));
    EXPECT_FALSE(edit.clumpColored("nope", "clumpId"));
    // Renamed, the source follows; others as they are.
    EXPECT_EQ(renamedColorSource("clumpId@c1", "c1", "front"), "clumpId@front");
    EXPECT_EQ(renamedColorSource("clumpId@c2", "c1", "front"), "clumpId@c2");
    EXPECT_EQ(renamedColorSource("clumpId", "c1", "front"), "clumpId");
}

TEST(GraphEdit, ViewBakeAndValueKindsAreOneRuleForEveryHost) {
    // View Up to Here, Bake Node to Guides and a Value node taking a param's kind: the core's rules,
    // every host's editor asking them (they were three of each before).
    const nlohmann::json g = {
        {"version", 1},
        {"nodes",
         {{{"name", "g"}, {"type", "Guides"}},
          {{"name", "curl"}, {"type", "Curl"}},
          {{"name", "free"}, {"type", "Curl"}},
          {{"name", "s"}, {"type", "Scatter"}},
          {{"name", "c"}, {"type", "Clump"}, {"paramInputs", {"strength"}}},
          {{"name", "v"}, {"type", "Value"}, {"params", {{"kind", "integer"}}}},
          {{"name", "o"}, {"type", "Output"}}}},
        {"connections",
         {{{"from", "g"}, {"to", "curl"}, {"input", "strands"}},
          {{"from", "s"}, {"to", "c"}, {"input", "strands"}},
          {{"from", "c"}, {"to", "o"}, {"input", "strands"}}}},
        {"output", "o"}};
    GraphEdit edit(g.dump());
    EXPECT_EQ(edit.outputTypes("curl"), (std::vector<std::string>{"Guides"})) << "a Curl on guides gives guides";
    EXPECT_EQ(edit.outputTypes("free"), (std::vector<std::string>{"Curves"}));
    EXPECT_TRUE(edit.viewable("s"));
    EXPECT_TRUE(edit.viewable("c"));
    EXPECT_TRUE(edit.viewable("free")) << "a Curves node fed nothing yet";
    EXPECT_FALSE(edit.viewable("curl")) << "guides: no strands to view";
    EXPECT_FALSE(edit.viewable("g"));
    EXPECT_FALSE(edit.viewable("v"));
    EXPECT_FALSE(edit.viewable("nope"));
    EXPECT_TRUE(edit.bakeable("s"));
    EXPECT_TRUE(edit.bakeable("curl")) << "guides bake too";
    EXPECT_TRUE(edit.bakeable("g"));
    EXPECT_FALSE(edit.bakeable("free")) << "nothing fed: neither strands nor guides";
    EXPECT_FALSE(edit.bakeable("o")) << "the Output node";
    EXPECT_FALSE(edit.bakeable("v"));
    // A Value no link leaves, drawn into a float param's input: it becomes a number.
    EXPECT_EQ(edit.valueKindForInput("v", "c", "strength"), "number");
    EXPECT_EQ(edit.valueKindForInput("s", "c", "strength"), "") << "not a Value";
    EXPECT_EQ(edit.valueKindForInput("v", "c", "nope"), "");
    EXPECT_TRUE(edit.connectByHand("v", "c", "strength")) << "its kind changed";
    const nlohmann::json after = nlohmann::json::parse(edit.toJson());
    for (const nlohmann::json& n : after["nodes"]) {
        if (n["name"] == "v") EXPECT_EQ(n["params"]["kind"], "number");
    }
    EXPECT_EQ(edit.valueKindForInput("v", "c", "strength"), "") << "it feeds a param now: its kind kept";
}

TEST(GraphEdit, ShowTubesIsABraidNodesAndFollowsIt) {
    // Show Tubes (every host's button on a Braid node): the nodes showing their tubes are a display
    // setting, names one a line; offered on Braid nodes (and nodes giving guides: Session.ShowTubesOnANodeGivingGuides...), toggled per node (several may show),
    // following a rename, dropped with the node.
    const nlohmann::json g = {
        {"version", 1},
        {"nodes",
         {{{"name", "s"}, {"type", "Scatter"}},
          {{"name", "plait"}, {"type", "Braid"}},
          {{"name", "side braid, left"}, {"type", "Braid"}},
          {{"name", "c"}, {"type", "Clump"}}}},
        {"connections",
         {{{"from", "s"}, {"to", "plait"}, {"input", "strands"}},
          {{"from", "plait"}, {"to", "side braid, left"}, {"input", "strands"}},
          {{"from", "side braid, left"}, {"to", "c"}, {"input", "strands"}}}},
        {"output", "c"}};
    GraphEdit edit(g.dump());
    EXPECT_TRUE(edit.braidTubesOffered("plait"));
    EXPECT_TRUE(edit.braidTubesOffered("side braid, left"));
    EXPECT_FALSE(edit.braidTubesOffered("c")) << "not a Braid (a Clump of strands)";
    EXPECT_FALSE(edit.braidTubesOffered("nope"));
    std::string shown;
    EXPECT_FALSE(edit.braidTubesShown("plait", shown));
    shown = edit.braidTubesToggled("plait", shown);
    EXPECT_EQ(shown, "plait");
    EXPECT_TRUE(edit.braidTubesShown("plait", shown));
    EXPECT_FALSE(edit.braidTubesShown("side braid, left", shown)) << "each node its own";
    shown = edit.braidTubesToggled("side braid, left", shown);
    EXPECT_EQ(braidTubeNames(shown), (std::vector<std::string>{"plait", "side braid, left"})) << "a name with a space and a comma";
    EXPECT_TRUE(edit.braidTubesShown("plait", shown));
    EXPECT_TRUE(edit.braidTubesShown("side braid, left", shown));
    EXPECT_EQ(edit.braidTubesToggled("c", shown), shown) << "a node without the button changes nothing";
    EXPECT_FALSE(edit.braidTubesShown("c", "c")) << "named, but no Braid: not lit";
    EXPECT_EQ(edit.braidTubesToggled("plait", shown), "side braid, left") << "again: off";
    // The names as text: each once, blank lines left out.
    EXPECT_EQ(braidTubeNames("a\n\nb\r\na\n"), (std::vector<std::string>{"a", "b"}));
    EXPECT_EQ(braidTubesText({"a", "", "b"}), "a\nb");
    EXPECT_TRUE(braidTubeNames("").empty());
    // Renamed, the name follows; another node's rename leaves it.
    EXPECT_EQ(renamedBraidTubes(shown, "plait", "crown"), "crown\nside braid, left");
    EXPECT_EQ(renamedBraidTubes(shown, "c", "crown"), shown);
    EXPECT_EQ(renamedBraidTubes(shown, "plait", ""), shown);
    edit.renameNode("plait", "crown");
    shown = renamedBraidTubes(shown, "plait", "crown");
    EXPECT_TRUE(edit.braidTubesShown("crown", shown));
    EXPECT_EQ(edit.braidTubesKept(shown), shown) << "every name a Braid node: kept";
    // Deleted, its name goes; so does one that is no Braid.
    edit.removeNode("crown");
    EXPECT_EQ(edit.braidTubesKept(shown), "side braid, left");
    EXPECT_EQ(edit.braidTubesKept("c\nside braid, left\ngone"), "side braid, left");
    EXPECT_EQ(edit.braidTubesKept(""), "");
    // The sections' colors: each its own, any two of a braid's (up to seven sections) well apart.
    for (int a = 0; a < 7; ++a) {
        for (int b = a + 1; b < 7; ++b) {
            const std::array<float, 3> x = braidSectionColor(a), y = braidSectionColor(b);
            const float apart = std::abs(x[0] - y[0]) + std::abs(x[1] - y[1]) + std::abs(x[2] - y[2]);
            EXPECT_GT(apart, 0.3f) << a << " and " << b;
        }
    }
}

TEST(GraphEdit, DeletingANodeKeepsTheFlowThroughIt) {
    // Delete (every host's graph editor; issue #80): as Detach, then the node goes.
    const nlohmann::json g = {
        {"version", 1},
        {"nodes",
         {{{"name", "s"}, {"type", "Scatter"}},
          {{"name", "c"}, {"type", "Clump"}},
          {{"name", "n"}, {"type", "Noise"}},
          {{"name", "w"}, {"type", "Width"}}}},
        {"connections",
         {{{"from", "s"}, {"to", "c"}, {"input", "strands"}},
          {{"from", "c"}, {"to", "n"}, {"input", "strands"}},
          {{"from", "n"}, {"to", "w"}, {"input", "strands"}}}},
        {"output", "w"}};
    GraphEdit edit(g.dump());
    const GraphEdit::Detached done = edit.removeNodeKeepingFlow("n");
    EXPECT_EQ(done.source, "c");
    EXPECT_FALSE(edit.hasNode("n"));
    EXPECT_EQ(edit.upstream("w"), (std::vector<std::string>{"c", "s"}));
    // Two chained nodes, one after the other: the flow skips both.
    GraphEdit both(g.dump());
    both.removeNodeKeepingFlow("c");
    both.removeNodeKeepingFlow("n");
    EXPECT_EQ(both.upstream("w"), (std::vector<std::string>{"s"}));
    // The output node: the output passes to what fed it.
    GraphEdit last(g.dump());
    last.removeNodeKeepingFlow("w");
    EXPECT_EQ(last.outputNode(), "n");
    // The first node: nothing fed it, so it simply goes; no output when the last node goes alone.
    GraphEdit first(g.dump());
    first.removeNodeKeepingFlow("s");
    EXPECT_TRUE(first.upstream("c").empty());
    EXPECT_THROW(first.removeNodeKeepingFlow("nope"), GraphError);
}

TEST(GraphEdit, NewNodesFitTheGroomsSize) {
    // The spec defaults are authored at a 20-unit scalp (a head in cm, as the presets are): a node
    // added to a groom of another size takes the defaults that measure something fitted to it, as
    // a preset's params are; the params given stay as given.
    GraphEdit edit(R"({"nodes": [], "connections": []})");
    edit.addNode("Noise", "atReference", {}, std::nullopt, std::nullopt, kDefaultsScalpSize);
    edit.addNode("Noise", "twice", {}, std::nullopt, std::nullopt, 40.0);
    edit.addNode("Noise", "given", R"({"amplitude": 0.05})", std::nullopt, std::nullopt, 40.0);
    edit.addNode("Noise", "unsized");
    edit.addNode("Scatter", "fur", {}, std::nullopt, std::nullopt, 2.0);
    edit.addNode("Curl", "curl", {}, std::nullopt, std::nullopt, 40.0);
    const nlohmann::json g = nlohmann::json::parse(edit.toJson());
    std::map<std::string, nlohmann::json> params;
    for (const nlohmann::json& n : g["nodes"]) params[n["name"].get<std::string>()] = n.value("params", nlohmann::json::object());
    // Every param is written (graph_io.h): the spec's default where nothing fits it.
    EXPECT_DOUBLE_EQ(params["atReference"]["amplitude"].get<double>(), 0.2);
    EXPECT_DOUBLE_EQ(params["unsized"]["amplitude"].get<double>(), 0.2);
    EXPECT_DOUBLE_EQ(params["twice"]["amplitude"].get<double>(), 0.4);  // a length: times 2
    EXPECT_EQ(params["twice"]["octaves"].get<int>(), 1);                // unitless: the spec's
    EXPECT_DOUBLE_EQ(params["given"]["amplitude"].get<double>(), 0.05);
    EXPECT_DOUBLE_EQ(params["fur"]["density"].get<double>(), 80.0 * 100.0);  // a density: over the square
    EXPECT_DOUBLE_EQ(params["fur"]["length"].get<double>(), 0.1);
    EXPECT_DOUBLE_EQ(params["curl"]["radius"].get<double>(), 1.0);
    EXPECT_DOUBLE_EQ(params["curl"]["frequency"].get<double>(), 0.2);  // a rate per length: divided

    // A preset fitted to another scalp: the defaults its nodes leave fitted with the rest.
    const nlohmann::json preset = nlohmann::json::parse(fitPresetJson(
        R"({"size": 20.0, "nodes": [{"name": "noise", "type": "Noise", "params": {"amplitude": 0.1}}, {"name": "curl", "type": "Curl"}]})",
        10.0));
    EXPECT_DOUBLE_EQ(preset["nodes"][0]["params"]["amplitude"].get<double>(), 0.05);
    EXPECT_DOUBLE_EQ(preset["nodes"][1]["params"]["radius"].get<double>(), 0.25);
    EXPECT_DOUBLE_EQ(preset["nodes"][1]["params"]["frequency"].get<double>(), 0.8);
}

TEST(GroomSession, StrandsThatAreNotNumbersAreSaidAtTheirNode) {
    // A node making points that are not numbers (an amplitude past a float's range): the view says so, naming it,
    // and keeps them out of the bounds.
    GroomData d;
    d.scalp = test::makePlane(4);
    d.graphJson = R"j({"version": 1, "output": "output", "nodes": [
        {"name": "scalp", "type": "Scalp"}, {"name": "scatter", "type": "Scatter", "params": {"density": 50.0}},
        {"name": "noise", "type": "Noise", "params": {"amplitude": 1e39}},
        {"name": "width", "type": "Width"}, {"name": "output", "type": "Output"}],
      "connections": [{"from": "scalp", "to": "scatter", "input": "scalp"}, {"from": "scatter", "to": "noise", "input": "strands"},
        {"from": "noise", "to": "width", "input": "strands"}, {"from": "width", "to": "output", "input": "strands"}]})j";
    GroomSession session(d);
    const ViewResult& view = session.evaluate(ViewRequest{});
    bool said = false;
    for (const std::string& e : view.errors) said = said || (e.find("node 'noise'") != std::string::npos && e.find("NaN") != std::string::npos);
    EXPECT_TRUE(said) << (view.errors.empty() ? std::string("no errors") : view.errors[0]) << " strands " << (view.strands ? view.strands->strandCount() : 0);
    EXPECT_TRUE(std::isfinite(view.bounds.lo.x) && std::isfinite(view.bounds.hi.y));
    // Finite strands say nothing of it.
    d.graphJson.replace(d.graphJson.find("1e39"), 4, "0.01");
    GroomSession fine(d);
    for (const std::string& e : fine.evaluate(ViewRequest{}).errors) EXPECT_EQ(e.find("NaN"), std::string::npos) << e;
}

// ---------------------------------------------------------------------------
// Per-node versions: a node saved with an older behaviour keeps it until upgraded.

// A test operator at version 2: version 1 made `gain`, version 2 makes twice it. A node saved at
// version 1 keeps making `gain` (and notes it), as a real operator keeps its old behaviour for
// grooms saved before a change.
class VersionedTestOp : public Operator {
public:
    std::string typeName() const override { return "TestVersioned"; }
    std::vector<PortSpec> inputs() const override { return {}; }
    PortSpec output() const override { return {"out", PortType::Field}; }
    int version() const override { return 2; }
    std::vector<ParamSpec> paramSpecs() const override {
        return {ParamSpec{"gain", 1.0, "What it makes"}.range(0.0, 5.0).soft(0.0, 2.0).in("Base"),
                ParamSpec{"mode", std::string("a"), "A mode", "", {"a", "b"}},
                ParamSpec{"onlyB", std::int64_t{1}, "Shown in mode b"}.range(0.0, 3.0).when("mode", {"b"}).adv()};
    }
    Value evaluate(const EvalContext& ctx, const std::vector<Value>&) const override {
        const double gain = params().getDouble("gain");
        if (nodeVersion() < 2) ctx.note("kept at version 1");
        return constantField(static_cast<float>(nodeVersion() < 2 ? gain : 2.0 * gain));
    }
};

// Registers an operator for one test.
struct TestOperator {
    TestOperator(const std::string& type, OperatorRegistry::Factory factory) : type_(type) {
        OperatorRegistry::instance().registerType(type, {}, std::move(factory));
    }
    ~TestOperator() { OperatorRegistry::instance().unregisterType(type_); }
    std::string type_;
};

float fieldOf(Graph& graph, NodeId id) {
    const FieldPtr field = valueAs<FieldPtr>(graph.evaluate(id, EvalContext{}));
    return field ? field->atRoot(StrandRoot{}) : -1.0f;
}

TEST(NodeVersions, OldNodesKeepTheirBehaviourUntilUpgraded) {
    const TestOperator registered("TestVersioned", [] { return std::make_unique<VersionedTestOp>(); });
    const std::string saved =
        R"({"nodes": [{"name": "old", "type": "TestVersioned", "version": 1, "params": {"gain": 3.0}},
                      {"name": "new", "type": "TestVersioned", "params": {"gain": 3.0}}], "output": "old"})";
    GraphDocument doc = loadGraphJson(saved);
    EXPECT_FLOAT_EQ(fieldOf(doc.graph, doc.node("old")), 3.0f);  // version 1's behaviour
    EXPECT_FLOAT_EQ(fieldOf(doc.graph, doc.node("new")), 6.0f);  // none stored: the newest
    EXPECT_EQ(doc.graph.lastNotes().at(doc.node("old")), "kept at version 1");
    EXPECT_EQ(doc.graph.lastNotes().count(doc.node("new")), 0u);
    // Saving keeps each node's version, and every param.
    const nlohmann::json written = nlohmann::json::parse(saveGraphJson(doc));
    for (const nlohmann::json& n : written["nodes"]) {
        EXPECT_EQ(n["version"].get<int>(), n["name"] == "old" ? 1 : 2);
        EXPECT_TRUE(n["params"].contains("onlyB"));
    }
    // A clone keeps it too.
    Graph copy = doc.graph.clone();
    EXPECT_FLOAT_EQ(fieldOf(copy, doc.node("old")), 3.0f);

    // The editors see an Upgrade; upgrading switches the node to the newest behaviour.
    GraphEdit edit(saved);
    const nlohmann::json spec = nlohmann::json::parse(edit.nodeSpecJson("old"));
    EXPECT_EQ(spec["version"].get<int>(), 2);
    EXPECT_EQ(spec["nodeVersion"].get<int>(), 1);
    EXPECT_TRUE(spec["outdated"].get<bool>());
    EXPECT_FALSE(nlohmann::json::parse(edit.nodeSpecJson("new"))["outdated"].get<bool>());
    EXPECT_TRUE(edit.upgradeNode("old"));
    EXPECT_FALSE(edit.upgradeNode("old"));
    EXPECT_EQ(edit.nodeVersion("old"), 2);
    EXPECT_FALSE(nlohmann::json::parse(edit.nodeSpecJson("old"))["outdated"].get<bool>());
    GraphDocument upgraded = loadGraphJson(edit.toJson());
    EXPECT_FLOAT_EQ(fieldOf(upgraded.graph, upgraded.node("old")), 6.0f);
    EXPECT_EQ(upgraded.graph.lastNotes().count(upgraded.node("old")), 0u);

    // A node newer than the build's operator is refused with a clear error.
    try {
        loadGraphJson(R"({"nodes": [{"name": "x", "type": "TestVersioned", "version": 3}], "output": "x"})");
        ADD_FAILURE() << "a newer node loaded";
    } catch (const GraphError& e) {
        EXPECT_NE(std::string(e.what()).find("newer than this GroomLab"), std::string::npos) << e.what();
    }
    EXPECT_THROW(loadGraphJson(R"({"nodes": [{"name": "x", "type": "TestVersioned", "version": 0}], "output": "x"})"),
                 GraphError);
}

TEST(NodeVersions, AChangedDefaultKeepsTheFormerOneForNodesSavedBefore) {
    // Interpolate 2 made blend directions the default (ParamSpec::formerDefault): a node saved at 1
    // that leaves it out keeps offsets,
    // loaded or completed by an editor; one set otherwise keeps its value; a new node takes the
    // new defaults; an Upgrade moves a param still at its former default to the new one.
    const std::string saved =
        R"({"nodes": [{"name": "old", "type": "Interpolate", "version": 1, "params": {}},
                      {"name": "chose", "type": "Interpolate", "version": 1, "params": {"blend": "directions"}},
                      {"name": "new", "type": "Interpolate", "params": {}}], "output": "old"})";
    GraphDocument doc = loadGraphJson(saved);
    auto blendOf = [&](const char* name) { return doc.graph.node(doc.node(name)).params().getString("blend"); };
    EXPECT_EQ(blendOf("old"), "offsets");
    EXPECT_EQ(blendOf("chose"), "directions");
    EXPECT_EQ(blendOf("new"), "directions");
    // Saved again: the old node's values written down, its version kept.
    const nlohmann::json written = nlohmann::json::parse(saveGraphJson(doc));
    for (const nlohmann::json& n : written["nodes"]) {
        EXPECT_EQ(n["params"]["blend"].get<std::string>(), n["name"] == "old" ? "offsets" : "directions") << n["name"];
        EXPECT_EQ(n["version"].get<int>(), n["name"] == "new" ? 2 : 1) << n["name"];
    }
    // An editor completing the params (what a host saves) gives the same.
    GraphEdit edit(saved);
    edit.completeParams();
    auto editedBlend = [&](const char* name) {
        const nlohmann::json graph = nlohmann::json::parse(edit.toJson());
        for (const nlohmann::json& n : graph["nodes"]) {
            if (n["name"] == name) return n["params"]["blend"].get<std::string>();
        }
        return std::string();
    };
    EXPECT_EQ(editedBlend("old"), "offsets");
    EXPECT_EQ(editedBlend("new"), "directions");
    // Upgraded: the new defaults.
    EXPECT_TRUE(edit.upgradeNode("old"));
    EXPECT_EQ(editedBlend("old"), "directions");
    GraphDocument upgraded = loadGraphJson(edit.toJson());
    EXPECT_EQ(upgraded.graph.node(upgraded.node("old")).params().getString("blend"), "directions");
}

TEST(Graph, AScattersOwnSegmentsOutrankTheResampleAfterIt) {
    // A Scatter's segments (above 0): its strands grow with them whatever a Resample after it asks
    // for (strands that take detailed guides through an Interpolate); the notes say so, and the
    // Resample's no longer claims it. 0: the Resample's, as ever.
    ExternalInputs ext;
    ext.set("scalp", test::makePlane(4, 2.0f), 1);
    auto docWith = [](int segments) {
        return loadGraphJson(R"({"version": 1, "output": "out", "nodes": [
            {"name": "scalp", "type": "Scalp"},
            {"name": "fur", "type": "Scatter", "params": {"density": 10.0, "segments": )" + std::to_string(segments) + R"(}},
            {"name": "furRes", "type": "Resample", "params": {"segments": 5}}, {"name": "out", "type": "Output"}],
          "connections": [{"from": "scalp", "to": "fur", "input": "scalp"}, {"from": "fur", "to": "furRes", "input": "strands"},
            {"from": "furRes", "to": "out", "input": "strands"}]})");
    };
    EvalContext ctx;
    ctx.externals = &ext;
    for (const int segments : {0, 40}) {
        GraphDocument doc = docWith(segments);
        ASSERT_TRUE(doc.graph.evaluateStrands(doc.output, ctx));
        const StrandsPtr fur = valueAs<StrandsPtr>(doc.graph.cachedValue(doc.node("fur")));
        ASSERT_TRUE(fur && fur->strandCount() > 0);
        EXPECT_EQ(fur->strandPointCount(0), segments ? 41u : 6u) << segments;
        const std::string scatterNote = doc.graph.strandResolutionNote(doc.node("fur"));
        const std::string resampleNote = doc.graph.strandResolutionNote(doc.node("furRes"));
        EXPECT_EQ(scatterNote.find("its own segments") != std::string::npos, segments > 0) << scatterNote;
        EXPECT_EQ(resampleNote.find("Scatter 'fur'") != std::string::npos, segments == 0) << resampleNote;
    }
}

TEST(Graph, EachScatterGrowsWithTheResampleOfItsOwnBranch) {
    // Two branches merged, each with its own Resample: each Scatter grows its strands with the most
    // segments a Resample after it asks for, not the most any Resample in the graph does.
    ExternalInputs ext;
    ext.set("scalp", test::makePlane(4, 2.0f), 1);
    GraphDocument doc = loadGraphJson(R"({"version": 1, "output": "out", "nodes": [
        {"name": "scalp", "type": "Scalp"},
        {"name": "fur", "type": "Scatter", "params": {"density": 10.0}},
        {"name": "furRes", "type": "Resample", "params": {"segments": 5}},
        {"name": "whiskers", "type": "Scatter", "params": {"density": 3.0, "seed": 4}},
        {"name": "whiskerRes", "type": "Resample", "params": {"segments": 11}},
        {"name": "bare", "type": "Scatter", "params": {"density": 2.0, "seed": 9}},
        {"name": "merge", "type": "Merge"}, {"name": "merge2", "type": "Merge"}, {"name": "out", "type": "Output"}],
      "connections": [{"from": "scalp", "to": "fur", "input": "scalp"}, {"from": "fur", "to": "furRes", "input": "strands"},
        {"from": "scalp", "to": "whiskers", "input": "scalp"}, {"from": "whiskers", "to": "whiskerRes", "input": "strands"},
        {"from": "scalp", "to": "bare", "input": "scalp"},
        {"from": "furRes", "to": "merge", "input": "a"}, {"from": "whiskerRes", "to": "merge", "input": "b"},
        {"from": "merge", "to": "merge2", "input": "a"}, {"from": "bare", "to": "merge2", "input": "b"},
        {"from": "merge2", "to": "out", "input": "strands"}]})");
    EvalContext ctx;
    ctx.externals = &ext;
    auto pointsOf = [&](const char* name) {
        const StrandsPtr s = valueAs<StrandsPtr>(doc.graph.cachedValue(doc.node(name)));
        EXPECT_TRUE(s && s->strandCount() > 0) << name;
        return s && s->strandCount() > 0 ? s->strandPointCount(0) : 0;
    };
    ASSERT_TRUE(doc.graph.evaluateStrands(doc.output, ctx));
    EXPECT_EQ(pointsOf("fur"), 6u);        // its Resample's 5 segments: what the Resample hands on as it came
    EXPECT_EQ(pointsOf("whiskers"), 12u);  // 11
    EXPECT_EQ(pointsOf("bare"), 9u);       // no Resample after it: 8
    const StrandsPtr furBefore = valueAs<StrandsPtr>(doc.graph.cachedValue(doc.node("fur")));
    EXPECT_EQ(doc.graph.strandResolution(doc.node("fur")).from, doc.node("furRes"));
    EXPECT_EQ(doc.graph.strandResolution(doc.node("bare")).from, 0u);
    // The notes the graph editors show.
    EXPECT_NE(doc.graph.strandResolutionNote(doc.node("fur")).find("set by Resample 'furRes'"), std::string::npos)
        << doc.graph.strandResolutionNote(doc.node("fur"));
    EXPECT_NE(doc.graph.strandResolutionNote(doc.node("bare")).find("no Resample after this Scatter"), std::string::npos);
    EXPECT_NE(doc.graph.strandResolutionNote(doc.node("whiskerRes")).find("Scatter 'whiskers'"), std::string::npos)
        << doc.graph.strandResolutionNote(doc.node("whiskerRes"));
    EXPECT_EQ(doc.graph.strandResolutionNote(doc.node("merge")), "");
    // The whiskers' Resample edited: only its own branch is made again (the fur's Scatter stays cached).
    doc.graph.node(doc.node("whiskerRes")).params().set("segments", std::int64_t{13});
    ASSERT_TRUE(doc.graph.evaluateStrands(doc.output, ctx));
    EXPECT_EQ(valueAs<StrandsPtr>(doc.graph.cachedValue(doc.node("fur"))), furBefore);
    EXPECT_EQ(pointsOf("whiskers"), 14u);
    // A Resample fed nothing changes nothing (it set the default of the Scatters with none after
    // them, a node left unconnected changing the groom): the bare Scatter keeps the default.
    const std::size_t bareBefore = pointsOf("bare");
    const NodeId loose = doc.graph.addNode(op::kResample);
    doc.graph.node(loose).params().set("segments", std::int64_t{3});
    ASSERT_TRUE(doc.graph.evaluateStrands(doc.output, ctx));
    EXPECT_EQ(pointsOf("bare"), bareBefore);
    EXPECT_EQ(pointsOf("fur"), 6u);
    EXPECT_NE(doc.graph.strandResolutionNote(loose).find("sets no Scatter"), std::string::npos) << doc.graph.strandResolutionNote(loose);
    // Nor does one on a branch that reaches no Output (after the bare Scatter, leading nowhere).
    const NodeId deadEnd = doc.graph.addNode(op::kResample);
    doc.graph.node(deadEnd).params().set("segments", std::int64_t{30});
    doc.graph.connect(doc.node("bare"), deadEnd, 0);
    ASSERT_TRUE(doc.graph.evaluateStrands(doc.output, ctx));
    EXPECT_EQ(pointsOf("bare"), bareBefore);
    EXPECT_NE(doc.graph.strandResolutionNote(deadEnd).find("reach no Output"), std::string::npos)
        << doc.graph.strandResolutionNote(deadEnd);
    // Set by the context: every Scatter alike.
    ctx.strandSegments = 7;
    ASSERT_TRUE(doc.graph.evaluateStrands(doc.output, ctx));
    EXPECT_EQ(pointsOf("bare"), 8u);
    EXPECT_EQ(pointsOf("whiskers"), 8u);
}

TEST(NodeNotes, NodesThatDoNothingAsSetSaySo) {
    // EvalContext::note: informational, the result stands; the session's stats carry them.
    GroomData d;
    d.scalp = test::makePlane(4, 2.0f);
    d.graphJson = R"({"version": 1, "output": "out", "nodes": [
        {"name": "scalp", "type": "Scalp"}, {"name": "guides", "type": "Guides"},
        {"name": "scatter", "type": "Scatter", "params": {"density": 20.0}},
        {"name": "interp", "type": "Interpolate"},
        {"name": "clump", "type": "Clump", "params": {"count": 0}},
        {"name": "part", "type": "Part"}, {"name": "flow", "type": "Flow"},
        {"name": "color", "type": "HairColor", "params": {"tipColor": [1.0, 0.0, 0.0]}},
        {"name": "cards", "type": "Cards", "params": {"clumpChannel": "lockId"}},
        {"name": "out", "type": "Output"}],
      "connections": [{"from": "scalp", "to": "scatter", "input": "scalp"},
        {"from": "scatter", "to": "interp", "input": "strands"}, {"from": "guides", "to": "interp", "input": "guides"},
        {"from": "scalp", "to": "interp", "input": "scalp"}, {"from": "interp", "to": "clump", "input": "strands"},
        {"from": "clump", "to": "part", "input": "strands"}, {"from": "part", "to": "flow", "input": "strands"},
        {"from": "flow", "to": "color", "input": "strands"}, {"from": "color", "to": "cards", "input": "strands"},
        {"from": "cards", "to": "out", "input": "strands"}]})";
    GroomSession session(d);
    const ViewResult& view = session.evaluate(ViewRequest{});
    ASSERT_TRUE(view.strands);
    EXPECT_TRUE(view.errors.empty()) << view.errors[0];
    std::map<std::string, std::string> notes;
    for (const auto& [node, note] : session.nodeNotes()) notes[node] = note;
    EXPECT_NE(notes["interp"].find("no guides"), std::string::npos) << notes["interp"];
    EXPECT_NE(notes["clump"].find("no clumps"), std::string::npos) << notes["clump"];
    EXPECT_NE(notes["part"].find("no parts"), std::string::npos) << notes["part"];
    EXPECT_NE(notes["flow"].find("no flow arrows"), std::string::npos) << notes["flow"];
    EXPECT_NE(notes["color"].find("tipStrength is 0"), std::string::npos) << notes["color"];
    EXPECT_NE(notes["cards"].find("lockId"), std::string::npos) << notes["cards"];
    EXPECT_EQ(notes.count("scatter"), 0u);
    // The stats carry them too, and a node set to do something says nothing.
    EXPECT_NE(session.stats().notes.at("clump").find("no clumps"), std::string::npos);
    d.graphJson.replace(d.graphJson.find(R"("tipColor": [1.0, 0.0, 0.0])"), 27, R"("tipStrength": 0.5)");
    GroomSession tipped(d);
    tipped.evaluate(ViewRequest{});
    for (const auto& [node, note] : tipped.nodeNotes()) EXPECT_NE(node, "color") << note;
    // An Instance naming a mesh the host lacks (the built-in plates never are).
    EXPECT_EQ(missingInstancePrototypes("leaf groomlab:scale leaf twig", [](const std::string& n) { return n == "twig"; }),
              std::vector<std::string>{"leaf"});
}

TEST(ParamSpecs, LimitsClampOnceAndEveryFieldReachesTheSpecs) {
    const TestOperator registered("TestVersioned", [] { return std::make_unique<VersionedTestOp>(); });
    GraphDocument doc = loadGraphJson(
        R"({"nodes": [{"name": "v", "type": "TestVersioned", "params": {"gain": 9.0, "onlyB": -4}}], "output": "v"})");
    const Operator& op = doc.graph.node(doc.node("v"));
    EXPECT_DOUBLE_EQ(op.params().getDouble("gain"), 5.0);  // past max: max
    EXPECT_EQ(op.params().getInt("onlyB"), 0);             // past min: min
    EXPECT_FLOAT_EQ(fieldOf(doc.graph, doc.node("v")), 10.0f);
    // Stored as given (the clamp is on read: a saved groom keeps what was typed).
    EXPECT_DOUBLE_EQ(std::get<double>(op.params().get("gain")), 9.0);
    ParamSet nan = op.params();
    nan.set("gain", std::nan(""));
    EXPECT_DOUBLE_EQ(nan.getDouble("gain"), 0.0);

    nlohmann::json spec;
    for (const nlohmann::json& s : nlohmann::json::parse(operatorSpecsJson())) {
        if (s["type"] == "TestVersioned") spec = s;
    }
    ASSERT_FALSE(spec.is_null());
    EXPECT_EQ(spec["version"].get<int>(), 2);
    const nlohmann::json& gain = spec["params"][0];
    EXPECT_EQ(gain["min"].get<double>(), 0.0);
    EXPECT_EQ(gain["max"].get<double>(), 5.0);
    EXPECT_EQ(gain["softMax"].get<double>(), 2.0);
    EXPECT_EQ(gain["group"].get<std::string>(), "Base");
    EXPECT_FALSE(gain["advanced"].get<bool>());
    EXPECT_FALSE(gain.contains("showWhen"));
    const nlohmann::json& mode = spec["params"][1];
    EXPECT_FALSE(mode.contains("min"));
    EXPECT_FALSE(mode.contains("softMin"));
    const nlohmann::json& onlyB = spec["params"][2];
    EXPECT_EQ(onlyB["softMax"].get<double>(), 3.0);  // no slider range: the limits
    EXPECT_TRUE(onlyB["advanced"].get<bool>());
    EXPECT_EQ(onlyB["showWhen"]["param"].get<std::string>(), "mode");
    EXPECT_EQ(onlyB["showWhen"]["in"][0].get<std::string>(), "b");
}

TEST(ParamSpecs, ConditionsReadValuesByOneRule) {
    // Every host's rule (param_conditions.h; Blender's units.condition_text repeats it): a bool as
    // true/false, a text as it is, a whole number as an integer also when stored as 3.0.
    using nlohmann::json;
    EXPECT_EQ(conditionText(json(true)), "true");
    EXPECT_EQ(conditionText(json("fishtail")), "fishtail");
    EXPECT_EQ(conditionText(json(3)), "3");
    EXPECT_EQ(conditionText(json(3.0)), "3") << "an int param a Python host stored as a float";
    EXPECT_EQ(conditionText(json(-2.0)), "-2");
    EXPECT_EQ(conditionText(json(0.5)), "0.5");
    const json when = {{"param", "layers"}, {"in", {"2", "3"}}};
    EXPECT_TRUE(conditionHolds(when, json(3.0)));
    EXPECT_TRUE(conditionHolds(when, json(2)));
    EXPECT_FALSE(conditionHolds(when, json(4)));
    EXPECT_TRUE(conditionHolds(json(), json(4))) << "no condition: it holds";
    EXPECT_TRUE(conditionHolds(json::object(), json(4)));
}

TEST(ParamSpecs, EveryOperatorsRangesAreSound) {
    // Every number param's default sits within its limits, its slider range within them, and a
    // showWhen names a param of its own operator with values that param can hold.
    static const std::set<std::string> units = {"",      "length", "width",    "density", "perLength", "color",
                                                "angle", "turns",  "fraction", "seconds", "pixels"};
    for (const nlohmann::json& op : nlohmann::json::parse(operatorSpecsJson())) {
        std::map<std::string, nlohmann::json> byName;
        for (const nlohmann::json& p : op["params"]) byName[p["name"].get<std::string>()] = p;
        for (const nlohmann::json& p : op["params"]) {
            const std::string what = op["type"].get<std::string>() + "." + p["name"].get<std::string>();
            if (p["default"].is_number()) {
                const double d = p["default"].get<double>();
                if (p.contains("min")) EXPECT_GE(d, p["min"].get<double>()) << what;
                if (p.contains("max")) EXPECT_LE(d, p["max"].get<double>()) << what;
                if (p.contains("softMin") && p.contains("softMax")) {
                    EXPECT_LT(p["softMin"].get<double>(), p["softMax"].get<double>()) << what;
                }
                if (p.contains("min") && p.contains("softMin")) EXPECT_GE(p["softMin"].get<double>(), p["min"].get<double>()) << what;
                if (p.contains("max") && p.contains("softMax")) EXPECT_LE(p["softMax"].get<double>(), p["max"].get<double>()) << what;
            } else if (p["kind"] != "map") {
                EXPECT_FALSE(p.contains("min") || p.contains("max")) << what << ": limits on a param that is not a number";
            }
            if (p.contains("showWhen")) {
                // By a param's value alone (never by an input linked: graph.h ParamCondition).
                const nlohmann::json& when = p["showWhen"];
                ASSERT_EQ(when.size(), 2u) << what << ": showWhen is {param, in}: " << when.dump();
                ASSERT_TRUE(when.contains("param") && when.contains("in")) << what << ": " << when.dump();
                const std::string other = when["param"].get<std::string>();
                ASSERT_TRUE(byName.count(other)) << what << " shows when " << other;
                EXPECT_NE(other, p["name"].get<std::string>()) << what << " shows by itself";
                const nlohmann::json& choices = byName[other]["choices"];
                for (const nlohmann::json& v : when["in"]) {
                    if (!choices.empty()) EXPECT_NE(std::find(choices.begin(), choices.end(), v), choices.end()) << what << ": " << v;
                }
                // The chain up (a param shows while the one it names does) ends.
                std::set<std::string> seen = {p["name"].get<std::string>()};
                for (std::string on = other; !on.empty() && byName.count(on); on = byName[on].contains("showWhen") ? byName[on]["showWhen"]["param"].get<std::string>() : std::string()) {
                    ASSERT_TRUE(seen.insert(on).second) << what << ": its showWhen chain loops at " << on;
                }
            }
            EXPECT_TRUE(units.count(p["unit"].get<std::string>())) << what << ": unit " << p["unit"];
        }
    }
}

// An Instance node's Object(s): every host's text for meshes picked, read back as the node reads it.
TEST(Instance, ObjectsTextListsEachMeshOnceSeparatedByCommas) {
    EXPECT_EQ(instancePrototypeText({"leaf", "bead", "leaf", ""}), "leaf, bead");
    EXPECT_EQ(instancePrototypeNames(instancePrototypeText({"leaf", "bead"})), (std::vector<std::string>{"leaf", "bead"}));
    EXPECT_EQ(instancePrototypeText({}), "");
    EXPECT_EQ(instancePrototypeToggled("leaf, bead", "flower", true), "leaf, bead, flower");
    EXPECT_EQ(instancePrototypeToggled("leaf bead", "leaf", false), "bead");
    EXPECT_EQ(instancePrototypeToggled("leaf", "leaf", true), "leaf");
    EXPECT_EQ(instancePrototypeToggled("", "leaf", true), "leaf");
    // Names with spaces (Blender's): in quotes, read back as one.
    EXPECT_EQ(instancePrototypeText({"My Mesh", "bead"}), "\"My Mesh\", bead");
    EXPECT_EQ(instancePrototypeNames("\"My Mesh\", bead"), (std::vector<std::string>{"My Mesh", "bead"}));
    EXPECT_EQ(instancePrototypeNames(instancePrototypeText({"a, b"})), (std::vector<std::string>{"a, b"}));
    EXPECT_EQ(instancePrototypeToggled("\"My Mesh\", bead", "bead", false), "\"My Mesh\"");
    // Spaces and commas separate the others, as grooms always wrote them.
    EXPECT_EQ(instancePrototypeNames("leaf bead, cuff"), (std::vector<std::string>{"leaf", "bead", "cuff"}));
    EXPECT_EQ(instancePrototypeNames(""), (std::vector<std::string>{""}));
}

}  // namespace
}  // namespace groomlab
