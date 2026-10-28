// SPDX-License-Identifier: Apache-2.0
// Parameter expressions (PG-2).

#include <gtest/gtest.h>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <set>
#include <sstream>

#if GROOMLAB_HAS_TBB
#include <tbb/global_control.h>
#endif

#include <nlohmann/json.hpp>

#include "groomlab/expression.h"
#include "groomlab/field.h"
#include "groomlab/graph_io.h"
#include "groomlab/groom.h"
#include "groomlab/map_ref.h"
#include "groomlab/operators.h"
#include "test_helpers.h"

namespace groomlab {
namespace {

float eval(const std::string& text) {
    StrandSet s;
    s.addStrand(42, {{1, 2, 3}, {1, 3, 3}});
    return Expression(text).evaluate(s, 0, nullptr);
}

TEST(Expression, ArithmeticAndPrecedence) {
    EXPECT_FLOAT_EQ(eval("1 + 2 * 3"), 7.0f);
    EXPECT_FLOAT_EQ(eval("(1 + 2) * 3"), 9.0f);
    EXPECT_FLOAT_EQ(eval("2 ^ 3 ^ 2"), 512.0f);  // right-associative
    EXPECT_FLOAT_EQ(eval("-2 ^ 2"), -4.0f);
    EXPECT_FLOAT_EQ(eval("2 ^ -1"), 0.5f);
    EXPECT_FLOAT_EQ(eval("10 / 4 - 1"), 1.5f);
    EXPECT_FLOAT_EQ(eval("1 / 0"), 0.0f);  // no inf or NaN escapes
    EXPECT_FLOAT_EQ(eval("3 > 2 ? 5 : 6"), 5.0f);
    EXPECT_FLOAT_EQ(eval("1 == 2 ? 5 : 1 <= 1 ? 7 : 8"), 7.0f);
    EXPECT_NEAR(eval("sin(pi / 2) + clamp(5, 0, 1) + mix(0, 10, 0.25)"), 4.5f, 1e-5f);
    EXPECT_NEAR(eval("smoothstep(0.5, 0, 1) + step(0.5, 0.7) + mod(-1, 3) + fract(2.25)"), 3.75f, 1e-5f);
    EXPECT_FLOAT_EQ(eval("x + y * 10 + z * 100"), 321.0f);  // root position without a scalp: the first point
}

TEST(Expression, RandomIsStablePerStrandAndInRange) {
    StrandSet s;
    for (int i = 0; i < 200; ++i) s.addStrand(static_cast<StrandId>(1000 + i), {{0, 0, 0}, {0, 1, 0}});
    const Expression e("rand");
    const Expression seeded("rand(3)");
    float lo = 1, hi = 0;
    for (std::size_t i = 0; i < s.strandCount(); ++i) {
        const float r = e.evaluate(s, i, nullptr);
        EXPECT_EQ(r, e.evaluate(s, i, nullptr));
        EXPECT_NE(r, seeded.evaluate(s, i, nullptr));
        lo = std::min(lo, r);
        hi = std::max(hi, r);
    }
    EXPECT_LT(lo, 0.1f);
    EXPECT_GT(hi, 0.9f);
    EXPECT_FLOAT_EQ(Expression("index").evaluate(s, 199, nullptr), 1.0f);
}

TEST(Expression, ChannelsAndErrors) {
    StrandSet s;
    s.addStrand(1, {{0, 0, 0}, {0, 1, 0}});
    s.addStrand(2, {{0, 0, 0}, {0, 1, 0}});
    s.addStrandChannel("clumpId")[1] = 7.0f;
    const Expression e("clumpId * 2 + melanin");
    ASSERT_EQ(e.channels().size(), 2u);
    const Expression::Bound bound = e.bind(s);
    EXPECT_FLOAT_EQ(e.evaluate(s, 1, nullptr, &bound), 14.0f);  // missing channels read 0

    for (const char* bad : {"1 +", "sin(1, 2)", "foo(1)", "(1", "1 ? 2", "2 $ 3", ""}) {
        EXPECT_THROW(Expression{bad}, ExpressionError) << bad;
    }
    try {
        Expression("1 + * 2");
        FAIL();
    } catch (const ExpressionError& err) {
        EXPECT_NE(std::string(err.what()).find("position"), std::string::npos);
    }
}

TEST(Expression, BlocksWithLocalsCommentsAndVariables) {
    // `$` names are the same variables; locals are computed once per line and read below.
    EXPECT_FLOAT_EQ(eval("$x + $y * 10"), eval("x + y * 10"));
    EXPECT_FLOAT_EQ(eval("# a block\n$a = 2;\n$b = $a * 3;  // six\n$a + $b"), 8.0f);
    EXPECT_FLOAT_EQ(eval("$a = 1; $a == 1 ? 5 : 6;"), 5.0f);  // `==` is no assignment; a closing ; is fine
    EXPECT_FLOAT_EQ(eval("$x = 10; $x"), 10.0f);                // a local hides a built-in variable

    // The node's own variables: numbers and vectors (three constants each).
    StrandSet s;
    s.addStrand(42, {{1, 2, 3}, {1, 3, 3}});
    const std::vector<std::pair<std::string, float>> constants = {
        {"amount", 0.5f}, {"center.x", 1.0f}, {"center.y", 4.0f}, {"center.z", 3.0f}};
    EXPECT_FLOAT_EQ(Expression("$amount * 4", constants).evaluate(s, 0, nullptr), 2.0f);
    EXPECT_FLOAT_EQ(Expression("$d = $y - $center.y;\nabs($d) * $amount", constants).evaluate(s, 0, nullptr), 1.0f);
    for (const char* bad : {"$center", "$other.x", "$a = 1\n$a", "$ = 2; 1"}) {
        EXPECT_THROW(Expression(bad, constants), ExpressionError) << bad;
    }
    try {  // in a block, the line and column
        Expression("$a = 1;\n$a + * 2");
        FAIL();
    } catch (const ExpressionError& err) {
        EXPECT_NE(std::string(err.what()).find("line 2"), std::string::npos) << err.what();
    }
    try {  // two values: a line above lacks its `;`
        Expression("$y * 2\n$x");
        FAIL();
    } catch (const ExpressionError& err) {
        EXPECT_NE(std::string(err.what()).find("line above"), std::string::npos) << err.what();
    }
}

TEST(Expression, SaysWhetherItReadsThePointsPositions) {
    // What a host running operators elsewhere needs (the GPU preview): values it may read once on
    // the incoming strands, or ones that change as the points move.
    for (const char* text : {"u * 2", "rand", "clumpId > 3 ? 1 : 0", "pt", "pointIndex * 0.1", "nx + index"}) {
        EXPECT_FALSE(Expression(text).readsPositions()) << text;
    }
    for (const char* text : {"length", "longest", "t", "px", "tipy", "straightness", "reach * 2"}) {
        EXPECT_TRUE(Expression(text).readsPositions()) << text;
    }
    // The same through a param: a number, an expression map; a plugged field says for itself.
    ParamSet params;
    params.set("width", 0.01);
    EXPECT_FALSE(ScalarField(params, "width", nullptr).readsPositions());
    MapRef map;
    map.source = MapRef::Source::Expression;
    map.name = "0.5 + 0.5 * t";
    params.set("width", map);
    EXPECT_TRUE(ScalarField(params, "width", nullptr).readsPositions());
    map.name = "rand";
    params.set("width", map);
    EXPECT_FALSE(ScalarField(params, "width", nullptr).readsPositions());
}

TEST(Expression, TheNodeReadsItsVariables) {
    const std::string json = R"j({"version": 1, "output": "out", "nodes": [
        {"name": "scalp", "type": "Scalp"},
        {"name": "scatter", "type": "Scatter", "params": {"density": 400.0}, "paramInputs": ["density"]},
        {"name": "mask", "type": "Expression", "params": {"expression": "$x < $split ? $keep : 0",
                                                          "variables": "{\"split\": 0.5, \"keep\": 1}"}},
        {"name": "out", "type": "Output"}],
      "connections": [{"from": "scalp", "to": "scatter", "input": "scalp"},
        {"from": "mask", "to": "scatter", "input": "density"}, {"from": "scatter", "to": "out", "input": "strands"}]})j";
    GroomData d;
    d.scalp = test::makePlane(8);
    d.graphJson = json;
    const StrandsPtr s = Groom(d).evaluate(1.0f, 0.0);
    ASSERT_GT(s->strandCount(), 50u);
    for (std::size_t i = 0; i < s->strandCount(); ++i) EXPECT_LT(s->strandPoints(i)[0].x, 0.5f + 1e-3f);
    GroomData bad = d;
    bad.graphJson.replace(bad.graphJson.find("{\\\"split\\\""), 1, "[");
    EXPECT_THROW(Groom(bad).evaluate(1.0f, 0.0), GraphError);

    // A keyable variable: the host's control sets it (its value where there is none), and a
    // new value re-evaluates the node.
    GroomData keyed = d;
    const std::string plain = "\\\"split\\\": 0.5";
    keyed.graphJson.replace(keyed.graphJson.find(plain), plain.size(),
                            "\\\"split\\\": {\\\"value\\\": 0.5, \\\"control\\\": \\\"mask_split\\\"}");
    Groom groom(keyed);
    auto widest = [](const StrandSet& strands) {
        float x = 0.0f;
        for (std::size_t i = 0; i < strands.strandCount(); ++i) x = std::max(x, strands.strandPoints(i)[0].x);
        return x;
    };
    EXPECT_NEAR(widest(*groom.evaluate(1.0f, 0.0)), 0.5f, 0.05f);  // no control: its value
    groom.setControls({{"mask_split", 0.25f}});
    EXPECT_NEAR(widest(*groom.evaluate(1.0f, 0.0)), 0.25f, 0.05f);
    EXPECT_EQ(expressionVariableControls("{\"a\": 1, \"b\": {\"value\": 2, \"control\": \"n_b\"}}"),
              (std::vector<std::pair<std::string, std::string>>{{"b", "n_b"}}));
}

TEST(Expression, PointVariablesEvaluatePerPoint) {
    StrandSet s;
    // Uneven spacing: t (arc length) and pt (index) differ.
    s.addStrand(7, {{0, 0, 0}, {0, 1, 0}, {0, 3, 0}, {0, 4, 0}});
    s.addStrand(8, {{2, 0, 0}, {2, 2, 0}});
    for (const char* text : {"t", "pt", "px", "pointIndex", "py * 2"}) EXPECT_TRUE(Expression(text).readsPointVariables()) << text;
    for (const char* text : {"length", "u + rand", "clumpId"}) EXPECT_FALSE(Expression(text).readsPointVariables()) << text;

    const Expression t("t"), pt("pt"), py("py"), idx("pointIndex"), len("length + t");
    const Expression::Bound bound = t.bind(s);
    EXPECT_TRUE(bound.perPoint);
    EXPECT_FLOAT_EQ(t.evaluatePoint(s, 0, 0, nullptr, &bound), 0.0f);
    EXPECT_FLOAT_EQ(t.evaluatePoint(s, 0, 1, nullptr, &bound), 0.25f);
    EXPECT_FLOAT_EQ(t.evaluatePoint(s, 0, 2, nullptr, &bound), 0.75f);
    EXPECT_FLOAT_EQ(t.evaluatePoint(s, 0, 3, nullptr, &bound), 1.0f);
    EXPECT_FLOAT_EQ(t.evaluatePoint(s, 1, 1, nullptr, &bound), 1.0f);  // another strand: its own arc
    EXPECT_FLOAT_EQ(t.evaluatePoint(s, 0, 2, nullptr, &bound), 0.75f);  // and back
    EXPECT_FLOAT_EQ(t.evaluate(s, 0, nullptr, &bound), 0.0f);           // per strand: the root's
    EXPECT_FLOAT_EQ(pt.evaluatePoint(s, 0, 2, nullptr), 2.0f / 3.0f);
    EXPECT_FLOAT_EQ(py.evaluatePoint(s, 0, 2, nullptr), 3.0f);
    EXPECT_FLOAT_EQ(idx.evaluatePoint(s, 0, 3, nullptr), 3.0f);
    EXPECT_FLOAT_EQ(len.evaluatePoint(s, 0, 1, nullptr), 4.25f);  // strand variables come along
    EXPECT_FLOAT_EQ(Expression("px + 10 * t").evaluatePoint(s, 1, 1, nullptr), 12.0f);

    // A point channel binds when the strands have no strand channel of the name; per point.
    s.addPointChannel("band", 0.0f)[2] = 5.0f;
    const Expression channel("band * 2");
    EXPECT_FALSE(channel.readsPointVariables());
    const Expression::Bound bandBound = channel.bind(s);
    EXPECT_TRUE(bandBound.perPoint);
    EXPECT_FLOAT_EQ(channel.evaluatePoint(s, 0, 2, nullptr, &bandBound), 10.0f);
    EXPECT_FLOAT_EQ(channel.evaluatePoint(s, 0, 1, nullptr, &bandBound), 0.0f);
    EXPECT_FLOAT_EQ(channel.evaluate(s, 0, nullptr, &bandBound), 0.0f);  // per strand: the root point
    s.addStrandChannel("band", 3.0f);  // a strand channel of the name wins, and it is per strand again
    const Expression::Bound strandBound = channel.bind(s);
    EXPECT_FALSE(strandBound.perPoint);
    EXPECT_FLOAT_EQ(channel.evaluatePoint(s, 0, 2, nullptr, &strandBound), 6.0f);

    // Through a ScalarField: perPoint() follows the expression, and the values remap.
    ParamSet params;
    MapRef map;
    map.source = MapRef::Source::Expression;
    map.name = "t";
    map.min = 1.0f;
    map.max = 3.0f;
    params.set("w", map);
    const ScalarField field(params, "w", nullptr);
    field.validate(s);
    EXPECT_TRUE(field.perPoint());
    EXPECT_FLOAT_EQ(field.atPoint(s, 0, 2), 2.5f);
    EXPECT_FLOAT_EQ(field.at(s, 0), 1.0f);
    map.name = "length";
    params.set("w", map);
    const ScalarField strandField(params, "w", nullptr);
    strandField.validate(s);
    EXPECT_FALSE(strandField.perPoint());
    EXPECT_FLOAT_EQ(strandField.atPoint(s, 0, 2), strandField.at(s, 0));
}

TEST(Expression, PerPointSetupNeverOutlivesItsBinding) {
    // The same set rewritten in place (same address, point array and count) and bound again
    // into the same variable, as the next frame's set may reuse the last one's memory: the
    // strand's setup (its tip here) is redone, not read from the last binding.
    StrandSet s;
    s.addStrand(1, {{0, 0, 0}, {0, 1, 0}, {0, 2, 0}});
    const Expression e("tipy + 0 * t");
    Expression::Bound bound = e.bind(s);
    EXPECT_FLOAT_EQ(e.evaluatePoint(s, 0, 1, nullptr, &bound), 2.0f);
    const Vec3f* before = s.strandPoints(0);
    s.mutablePositions()[2] = {0, 5, 0};
    ASSERT_EQ(s.strandPoints(0), before);
    bound = e.bind(s);
    EXPECT_FLOAT_EQ(e.evaluatePoint(s, 0, 1, nullptr, &bound), 5.0f);
    EXPECT_NE(e.bind(s).serial, bound.serial);
}

TEST(Expression, LocalsThatDoubleEachLineAreAnErrorNotAHang) {
    // Each local is put in where it is read: $b = $a + $a, $c = $b + $b, ... doubles the program
    // per line, past any memory in 40 lines. It stops with a clear error instead.
    std::string text = "$a0 = x;\n";
    for (int i = 1; i <= 40; ++i) text += "$a" + std::to_string(i) + " = $a" + std::to_string(i - 1) + " + $a" + std::to_string(i - 1) + ";\n";
    text += "$a40";
    try {
        Expression e(text);
        ADD_FAILURE() << "no error";
    } catch (const ExpressionError& err) {
        EXPECT_NE(std::string(err.what()).find("too long"), std::string::npos) << err.what();
    }
    // A few doublings are fine.
    std::string few = "$a0 = x;\n";
    for (int i = 1; i <= 8; ++i) few += "$a" + std::to_string(i) + " = $a" + std::to_string(i - 1) + " + $a" + std::to_string(i - 1) + ";\n";
    EXPECT_FLOAT_EQ(eval(few + "$a8"), 256.0f);
}

TEST(Expression, DrivesParametersInAGroom) {
    // Length cut by an expression of the root height: shorter toward the bottom.
    GroomData d;
    d.scalp = test::makeSphere(24, 48, 1.0f);
    std::string graph = defaultGraphJson();
    const std::string from = "\"type\": \"Length\", \"params\": {\"fraction\": 1.0, \"randomness\": 0.2}";
    ASSERT_NE(defaultGraphJson().find(from), std::string::npos);
    graph.replace(graph.find(from), from.size(),
                  "\"type\": \"Length\"" + std::string(R"j(, "params": {"keep": {"expression": "smoothstep(-1, 1, y)", "min": 0.2, "max": 1.0}})j"));
    d.graphJson = graph;
    const StrandsPtr strands = Groom(d).evaluate(1.0f, 0.0);
    ASSERT_GT(strands->strandCount(), 100u);
    float topLength = 0, bottomLength = 10;
    for (std::size_t s = 0; s < strands->strandCount(); ++s) {
        const Vec3f* p = strands->strandPoints(s);
        const float len = (p[strands->strandPointCount(s) - 1] - p[0]).length();
        if (p[0].y > 0.9f) topLength = std::max(topLength, len);
        if (p[0].y < -0.9f) bottomLength = std::min(bottomLength, len);
    }
    EXPECT_GT(topLength, 3.0f * bottomLength);

    // A bad expression is a graph error on the node, with the position.
    graph = defaultGraphJson();
    graph.replace(graph.find(from), from.size(), R"j("type": "Length", "params": {"keep": {"expression": "y +"}})j");
    d.graphJson = graph;
    EXPECT_THROW(Groom(d).evaluate(1.0f, 0.0), GraphError);
    // An unknown name that is not a channel is caught before the loop.
    graph = defaultGraphJson();
    graph.replace(graph.find(from), from.size(), R"j("type": "Length", "params": {"keep": {"expression": "wobble"}})j");
    d.graphJson = graph;
    EXPECT_THROW(Groom(d).evaluate(1.0f, 0.0), GraphError);
    // Scatter density from an expression (evaluated at roots, before strands exist).
    graph = defaultGraphJson();
    const std::string scatter = "\"type\": \"Scatter\"";
    graph.replace(graph.find(scatter), scatter.size(),
                  scatter + R"j(, "params": {"density": {"expression": "y > 0 ? 1 : 0", "max": 200.0}})j");
    d.graphJson = graph;
    const StrandsPtr top = Groom(d).evaluate(1.0f, 0.0);
    for (std::size_t s = 0; s < top->strandCount(); ++s) EXPECT_GT(top->strandPoints(s)[0].y, -0.05f);
}

// ---------------------------------------------------------------------------
// Vectors, time, noise, curves, maps, picking (N1)

Vec3f evalVector(const std::string& text, const std::vector<std::pair<std::string, float>>& constants = {}) {
    StrandSet s;
    s.addStrand(42, {{1, 2, 3}, {1, 3, 3}});
    return Expression(text, constants, ExpressionType::Vector).evaluateVector(s, 0, nullptr);
}

void expectVector(const Vec3f& got, const Vec3f& want, float tolerance = 1e-5f) {
    EXPECT_NEAR(got.x, want.x, tolerance);
    EXPECT_NEAR(got.y, want.y, tolerance);
    EXPECT_NEAR(got.z, want.z, tolerance);
}

TEST(Expression, VectorsAndTheirOperators) {
    EXPECT_FLOAT_EQ(eval("[1, 2, 3].y"), 2.0f);
    EXPECT_FLOAT_EQ(eval("[1, 2, 3][2]"), 3.0f);
    EXPECT_FLOAT_EQ(eval("([1, 2, 3] * 2 + 1).z"), 7.0f);
    EXPECT_FLOAT_EQ(eval("(2 * [1, 2, 3] - [1, 1, 1]).y"), 3.0f);
    EXPECT_FLOAT_EQ(eval("([2, 4, 6] / 2).x + ([2, 4, 6] / [1, 0, 3]).y"), 1.0f);  // a 0 divisor gives 0
    EXPECT_FLOAT_EQ(eval("(-[1, 2, 3]).x"), -1.0f);
    EXPECT_FLOAT_EQ(eval("([1, 2, 3] ^ 2).z"), 9.0f);
    EXPECT_FLOAT_EQ(eval("dot([1, 2, 3], [4, 5, 6])"), 32.0f);
    EXPECT_FLOAT_EQ(eval("length(cross([1, 0, 0], [0, 1, 0]))"), 1.0f);
    EXPECT_FLOAT_EQ(eval("cross([1, 0, 0], [0, 1, 0]).z"), 1.0f);
    EXPECT_FLOAT_EQ(eval("normalize([3, 0, 4]).z"), 0.8f);
    EXPECT_FLOAT_EQ(eval("distance(P, [1, 2, 3])"), 0.0f);  // P: the root (the first point without a scalp)
    EXPECT_FLOAT_EQ(eval("P.y + N.y + tip.y"), 2.0f + 1.0f + 3.0f);
    EXPECT_FLOAT_EQ(eval("$v = [1, 2, 3] * 3; $v.y + $v[2]"), 15.0f);  // a vector local
    EXPECT_FLOAT_EQ(eval("(x > 0 ? [1, 1, 1] : 0).y + (x < 0 ? [1, 1, 1] : 5).z"), 6.0f);  // a number beside a vector
    // Number functions on vectors, component by component (a number argument on every component).
    expectVector(evalVector("abs([-1, 2, -3])"), {1, 2, 3});
    expectVector(evalVector("mix([0, 0, 0], [2, 4, 6], 0.5)"), {1, 2, 3});
    expectVector(evalVector("clamp([5, -5, 0.5], 0, 1)"), {1, 0, 0.5f});
    expectVector(evalVector("max([1, 5, 2], [3, 1, 2])"), {3, 5, 2});
    expectVector(evalVector("lerp(P, tip, 0.5)"), {1, 2.5f, 3});
    expectVector(evalVector("rotate([1, 0, 0], [0, 0, 1], pi / 2)"), {0, 1, 0});
    expectVector(evalVector("P"), {1, 2, 3});
    // The node's own vector, whole or by component.
    const std::vector<std::pair<std::string, float>> constants = {
        {"center.x", 1.0f}, {"center.y", 4.0f}, {"center.z", 3.0f}, {"center.r", 1.0f}, {"center.g", 4.0f}, {"center.b", 3.0f}};
    expectVector(evalVector("$center - P", constants), {0, 2, 0});
    EXPECT_FLOAT_EQ(Expression("length($center - P) + $center.g", constants).evaluate(*[] {
        auto s = std::make_shared<StrandSet>();
        s->addStrand(42, {{1, 2, 3}, {1, 3, 3}});
        return s;
    }(), 0, nullptr), 6.0f);
    // A number expression read as a vector is that number on every component.
    StrandSet s;
    s.addStrand(42, {{1, 2, 3}, {1, 3, 3}});
    const Expression number("y", {}, ExpressionType::Any);
    EXPECT_FALSE(number.isVector());
    expectVector(number.evaluateVector(s, 0, nullptr), {2, 2, 2});
    EXPECT_TRUE(Expression("P", {}, ExpressionType::Any).isVector());

    // SeExpr: a number where a vector is wanted is that number on every component.
    EXPECT_FLOAT_EQ(eval("dot(1, [1, 2, 3])"), 6.0f);
    EXPECT_NEAR(eval("length(2)"), std::sqrt(12.0f), 1e-5f);
    for (const char* bad : {"[1, 2]", "[1, 2, 3", "[[1, 2, 3], 2, 3]", "[1, 2, 3] < 2", "P ? 1 : 2", "rotate(P, N, P)",
                            "x.y", "(x + 1).x", "P[3]", "rand(P)", "P"}) {
        EXPECT_THROW(Expression{bad}, ExpressionError) << bad;  // a number wanted: "P" gives a vector
    }
    EXPECT_THROW(Expression("x * 2", {}, ExpressionType::Vector), ExpressionError);
    try {
        Expression("normalize(P)");
        FAIL();
    } catch (const ExpressionError& err) {
        EXPECT_NE(std::string(err.what()).find("gives a vector"), std::string::npos) << err.what();
    }
}

TEST(Expression, TimeComesFromTheEnvironmentItCompilesIn) {
    StrandSet s;
    s.addStrand(1, {{0, 0, 0}, {0, 1, 0}});
    EXPECT_FLOAT_EQ(Expression("frame + time").evaluate(s, 0, nullptr), 0.0f);  // none: frame 0
    ExpressionEnvironment environment;
    environment.seconds = 2.0;
    environment.frame = 48.0;
    {
        const ExpressionEnvironmentScope scope(&environment);
        const Expression e("$frame + $time * 100 + F");
        EXPECT_TRUE(e.readsTime());
        EXPECT_FLOAT_EQ(e.evaluate(s, 0, nullptr), 48.0f + 200.0f + 48.0f);
        EXPECT_FLOAT_EQ(e.evaluatePoint(s, 0, 1, nullptr), 296.0f);
        EXPECT_FLOAT_EQ(e.evaluateAt(StrandRoot{}, nullptr), 296.0f);
        EXPECT_FALSE(Expression("rand + 1").readsTime());
        {
            ExpressionEnvironment inner;
            inner.frame = 7.0;
            const ExpressionEnvironmentScope nested(&inner);
            EXPECT_FLOAT_EQ(Expression("frame").evaluate(s, 0, nullptr), 7.0f);
        }
        EXPECT_FLOAT_EQ(Expression("frame").evaluate(s, 0, nullptr), 48.0f);  // the outer one back
    }
    // What a cache key asks, without compiling: the time and the maps named.
    EXPECT_TRUE(scanExpression("sin($frame * 0.1)").time);
    EXPECT_TRUE(scanExpression("F > 10").time);
    EXPECT_TRUE(scanExpression("$t = time; $t").time);
    EXPECT_FALSE(scanExpression("rand * length # frame in a comment").time);
    EXPECT_FALSE(scanExpression("$dir.x + frames").time);
    const ExpressionDependencies maps = scanExpression("map(\"mask\") * cmap('tint').x + map( \"mask\" , \"r\")");
    EXPECT_FALSE(maps.time);
    EXPECT_EQ(maps.maps, (std::vector<std::string>{"mask", "tint"}));
}

TEST(Expression, OnlyExpressionsReadingTheTimeEvaluateAgainOnAnotherFrame) {
    GroomData d;
    d.scalp = test::makePlane(8);
    auto withFactor = [&](const std::string& expression) {
        std::string graph = defaultGraphJson();
        const std::string from = "\"type\": \"Length\", \"params\": {\"fraction\": 1.0, \"randomness\": 0.2}";
        graph.replace(graph.find(from), from.size(),
                      "\"type\": \"Length\", \"params\": {\"keep\": {\"expression\": \"" + expression + "\"}}");
        GroomData data = d;
        data.graphJson = graph;
        return data;
    };
    {
        Groom groom(withFactor("0.5 + 0.25 * rand"));
        const StrandsPtr a = groom.evaluate(1.0f, 0.0);
        const StrandsPtr b = groom.evaluate(1.0f, 1.0);
        EXPECT_EQ(a, b);  // the same result: nothing evaluated again
    }
    {
        Groom groom(withFactor("0.5 + 0.25 * sin($time * 3)"));
        groom.setFramesPerSecond(24.0);
        const StrandsPtr a = groom.evaluate(1.0f, 0.0);
        const std::uint64_t first = a->contentHash();
        const StrandsPtr b = groom.evaluate(1.0f, 1.0);
        EXPECT_NE(first, b->contentHash());
        EXPECT_EQ(groom.evaluate(1.0f, 0.0)->contentHash(), first);  // back on frame 0: the same strands
    }
    {
        // $frame by the session's frame rate: 1 second at 24 fps is frame 24, at 30 frame 30.
        Groom groom(withFactor("$frame / 48"));
        groom.setFramesPerSecond(24.0);
        const std::uint64_t at24 = groom.evaluate(1.0f, 1.0)->contentHash();
        groom.setFramesPerSecond(30.0);
        EXPECT_NE(groom.evaluate(1.0f, 1.0)->contentHash(), at24);
        Groom half(withFactor("0.5"));
        EXPECT_EQ(half.evaluate(1.0f, 0.0)->contentHash(), at24);  // 24 / 48
    }
}

TEST(Expression, NoiseFamilyIsSeExprsAndSeeded) {
    StrandSet s;
    s.addStrand(1, {{0, 0, 0}, {0, 1, 0}});
    auto at = [&](const std::string& text) { return Expression(text).evaluate(s, 0, nullptr); };
    auto atVector = [&](const std::string& text) { return Expression(text, {}, ExpressionType::Vector).evaluateVector(s, 0, nullptr); };
    // noise of a point is noise(x, y, z), bit for bit; noise(x, y) is noise(x, y, 0).
    for (const char* p : {"0.3, 1.7, -2.2", "10.5, 0.25, 3"}) {
        EXPECT_EQ(at(std::string("noise(") + p + ")"), at(std::string("noise([") + p + "])")) << p;
    }
    EXPECT_EQ(at("noise(0.37)"), at("noise([0.37, 0, 0])"));  // a number x: [x, 0, 0]
    EXPECT_EQ(at("noise(0.3, 1.7)"), at("noise(0.3, 1.7, 0)"));
    float lo = 1e9f, hi = -1e9f, slo = 1e9f, shi = -1e9f;
    for (int i = 0; i < 200; ++i) {
        const std::string p = "[" + std::to_string(i * 0.137) + ", " + std::to_string(i * 0.071) + ", " + std::to_string(-i * 0.053) + "]";
        // SeExpr's ranges: noise 0 to 1, snoise -1 to 1 (noise = 0.5 + 0.5 snoise).
        const float n = at("noise(" + p + ")"), sn = at("snoise(" + p + ")");
        EXPECT_GE(n, 0.0f);
        EXPECT_LE(n, 1.0f);
        EXPECT_NEAR(n, 0.5f + 0.5f * sn, 1e-6f);
        lo = std::min(lo, n);
        hi = std::max(hi, n);
        slo = std::min(slo, sn);
        shi = std::max(shi, sn);
        const float n4 = at("noise(" + p + "[0], " + p + "[1], " + p + "[2], 0.4)");
        EXPECT_GE(n4, 0.0f);
        EXPECT_LE(n4, 1.0f);
        EXPECT_NEAR(n4, 0.5f + 0.5f * at("snoise4(" + p + ", 0.4)"), 1e-6f);
        // fbm: its first octave is noise; more octaves add up, 0.5 + 0.5 * the sum (not normalized).
        EXPECT_NEAR(at("fbm(" + p + ", 1)"), n, 1e-6f);
        EXPECT_NEAR(at("turbulence(" + p + ", 1)"), 0.5f + 0.5f * std::abs(sn), 1e-6f);
        EXPECT_NEAR(at("fbm4(" + p + ", 0.4, 1)"), n4, 1e-6f);
        const float f = at("fbm(" + p + ")");
        EXPECT_GE(f, -0.5f);
        EXPECT_LE(f, 1.5f);
        EXPECT_GE(at("turbulence(" + p + ", 3)"), 0.5f);
        expectVector(atVector("vfbm(" + p + ", 1)"), atVector("vnoise(" + p + ")"), 1e-6f);
        expectVector(atVector("cnoise(" + p + ")"), atVector("vnoise(" + p + ") * 0.5 + 0.5"), 1e-6f);
        expectVector(atVector("cfbm(" + p + ")"), atVector("vfbm(" + p + ") * 0.5 + 0.5"), 1e-6f);
        expectVector(atVector("cturbulence(" + p + ")"), atVector("vturbulence(" + p + ") * 0.5 + 0.5"), 1e-6f);
        expectVector(atVector("cfbm4(" + p + ", 2)"), atVector("vfbm4(" + p + ", 2) * 0.5 + 0.5"), 1e-6f);
        expectVector(atVector("cnoise4(" + p + ", 2)"), atVector("vnoise4(" + p + ", 2) * 0.5 + 0.5"), 1e-6f);
        const float c = at("cellnoise(" + p + ")");
        EXPECT_GE(c, 0.0f);
        EXPECT_LT(c, 1.0f);
        const Vec3f cc = atVector("ccellnoise(" + p + ")");
        EXPECT_GE(std::min({cc.x, cc.y, cc.z}), 0.0f);
        EXPECT_LT(std::max({cc.x, cc.y, cc.z}), 1.0f);
        // voronoi's types: 1 a value per cell, 2 F1, 3 F2, 4 F2 - F1, 5 0-1 at the cells' edges.
        const float f1 = at("voronoi(" + p + ", 2)"), f2 = at("voronoi(" + p + ", 3)");
        EXPECT_GE(f1, 0.0f);
        EXPECT_GE(f2, f1);
        EXPECT_NEAR(at("voronoi(" + p + ", 4)"), f2 - f1, 1e-5f);
        const float cell = at("voronoi(" + p + ")"), edge = at("voronoi(" + p + ", 5)");
        EXPECT_GE(cell, 0.0f);
        EXPECT_LT(cell, 1.0f);
        EXPECT_GE(edge, 0.0f);
        EXPECT_LE(edge, 1.0f);
        expectVector(atVector("cvoronoi(" + p + ", 2)"), atVector("cvoronoi(" + p + ", 1)") * f1, 1e-5f);
        // pnoise repeats every period along each axis.
        const std::string shifted = "(" + p + " + [4, 0, 0])";
        EXPECT_NEAR(at("pnoise(" + p + ", 4)"), at("pnoise(" + shifted + ", 4)"), 1e-4f);
    }
    EXPECT_LT(lo, 0.4f);  // it varies
    EXPECT_GT(hi, 0.6f);
    EXPECT_LT(slo, -0.2f);
    EXPECT_GT(shi, 0.2f);
    // One value per unit cell; a jitter of (nearly) 0 puts every cell's point in its middle.
    EXPECT_EQ(at("cellnoise([2.1, 3.2, 4.3])"), at("cellnoise([2.9, 3.8, 4.01])"));
    EXPECT_NE(at("cellnoise([2.1, 3.2, 4.3])"), at("cellnoise([3.1, 3.2, 4.3])"));
    EXPECT_NEAR(at("voronoi([0.5, 0.5, 0.5], 2, 0)"), 0.0f, 1e-3f);
    EXPECT_NEAR(at("voronoi([0.5, 0.5, 0.5], 3, 0)"), 1.0f, 1e-3f);
    expectVector(atVector("pvoronoi([0.4, 0.4, 0.4], 0)"), {0.5f, 0.5f, 0.5f}, 1e-3f);
    EXPECT_EQ(at("voronoi([0.4, 0.45, 0.5], 1, 0.1)"), at("voronoi([0.55, 0.5, 0.6], 1, 0.1)"));  // the same cell, its value
    // A noise in time: noise(x, y, z, w) changes with w.
    EXPECT_NE(at("noise(0.3, 0.6, 0.9, 0.2)"), at("noise(0.3, 0.6, 0.9, 0.7)"));
    for (const char* bad : {"fbm(P, 1, 2, 3, 4)", "fbm(P, [1, 2, 3])", "noise(P, 2)", "noise(1, 2, 3, 4, 5)", "noise()",
                            "snoise4(P)", "pnoise(P)"}) {
        EXPECT_THROW(Expression{bad}, ExpressionError) << bad;
    }
}

TEST(Expression, CurvesAsSeExprAndAsGroomLabPairs) {
    // SeExpr's curve: keys (position, value, interpolation), the interpolation of the span after a key.
    EXPECT_FLOAT_EQ(eval("curve(0.25, 0, 0, 1, 1, 4, 1)"), 1.0f);         // 1 linear
    EXPECT_FLOAT_EQ(eval("curve(0.7, 0, 2, 0, 1, 5, 0)"), 2.0f);          // 0 none: the key's value
    EXPECT_FLOAT_EQ(eval("curve(0.25, 0, 0, 2, 1, 1, 2)"), 0.15625f);     // 2 smooth
    EXPECT_FLOAT_EQ(eval("curve(0.5, 0, 0, 3, 1, 1, 3, 2, 4, 3)"), 0.25f);  // 3 spline: Catmull-Rom slopes
    EXPECT_FLOAT_EQ(eval("curve(1.5, 0, 0, 3, 1, 1, 3, 2, 1, 3)"), 1.0625f);  // which overshoot
    EXPECT_FLOAT_EQ(eval("curve(1.5, 0, 0, 4, 1, 1, 4, 2, 1, 4)"), 1.0f);     // 4 monotone: which don't
    EXPECT_FLOAT_EQ(eval("curve(-1, 0, 3, 1, 1, 5, 1)"), 3.0f);  // held before the first key
    EXPECT_FLOAT_EQ(eval("curve(2, 0, 3, 1, 1, 5, 1)"), 5.0f);   // and after the last
    EXPECT_FLOAT_EQ(eval("curve(0.75, 1, 4, 1, 0, 0, 1)"), 3.0f);  // keys in any order
    EXPECT_FLOAT_EQ(eval("curve(0.3, 0, 1, 1)"), 1.0f);            // a single key
    expectVector(evalVector("ccurve(0.5, 0, [0, 0, 0], 1, 1, [2, 4, 6], 1)"), {1, 2, 3});
    expectVector(evalVector("ccurve(0.5, 0, 0, 1, 1, [2, 2, 2], 1)"), {1, 1, 1});  // a number value: a grey
    // GroomLab's pairs (position, value): a smooth ramp.h curve, when the arguments can't be triples.
    const Ramp ramp{{0.0f, 0.0f}, {0.5f, 1.0f}, {1.0f, 0.0f}};
    EXPECT_FLOAT_EQ(eval("curve(0.3, 0, 0, 0.5, 1, 1, 0)"), ramp(0.3f));
    EXPECT_FLOAT_EQ(eval("curve(0.5, 1, 2, 0, 0)"), 1.0f);  // keys in any order; two keys: a line
    EXPECT_FLOAT_EQ(eval("linearcurve(0.25, 0, 0, 1, 4)"), 1.0f);
    EXPECT_FLOAT_EQ(eval("curve(2, 0, 0, 1, 3)"), 3.0f);  // held past the last key
    StrandSet s;
    s.addStrand(42, {{0, 0, 0}, {0, 1, 0}, {0, 2, 0}});
    // Keys from the node's own variables; t per point.
    const Expression keyed("curve(t, 0, $root, 1, $tip * 2)", {{"root", 1.0f}, {"tip", 2.0f}});
    EXPECT_FLOAT_EQ(keyed.evaluatePoint(s, 0, 1, nullptr), 2.5f);
    const Expression keyedTriples("curve(t, 0, $root, 1, 1, $tip * 2, 1)", {{"root", 1.0f}, {"tip", 2.0f}});
    EXPECT_FLOAT_EQ(keyedTriples.evaluatePoint(s, 0, 1, nullptr), 2.5f);
    // SeExpr's spline: Catmull-Rom through the values but the first and last (the end controls).
    EXPECT_FLOAT_EQ(eval("spline(0, 5, 1, 2, 7)"), 1.0f);
    EXPECT_FLOAT_EQ(eval("spline(1, 5, 1, 2, 7)"), 2.0f);
    EXPECT_FLOAT_EQ(eval("spline(0.5, 0, 0, 1, 1)"), 0.5f);
    EXPECT_FLOAT_EQ(eval("spline(0.5, 0, 0, 1, 2, 2)"), 1.0f);  // two spans: the middle value at 0.5
    EXPECT_FLOAT_EQ(eval("spline(x * 0.5, 0, 0, 1, 1)"), 0.5f);  // its values and t may vary
    expectVector(evalVector("spline(0.5, 0, 0, [2, 4, 6], [2, 4, 6])"), {1, 2, 3});
    for (const char* bad : {"curve(t, 0, x, 1, 1)", "curve(t, 0, 0, 1, x, 1, 1)", "curve(0.5, 0, 0, 1, 1, 1)",
                            "curve(t, 0, [1, 1, 1], 1)", "ccurve(t, 0, 0, 7)", "spline(0.5, 0, 1, 0)", "curve(P, 0, 0, 1)"}) {
        EXPECT_THROW(Expression{bad}, ExpressionError) << bad;
    }
}

TEST(Expression, PickingAndHashing) {
    for (int i = 0; i < 50; ++i) {
        const float p = eval("pick(" + std::to_string(i) + ", 3, 6)");
        EXPECT_GE(p, 3.0f);
        EXPECT_LE(p, 6.0f);
        EXPECT_EQ(p, std::floor(p));
        EXPECT_EQ(eval("pick(" + std::to_string(i) + ", 1, 3, 0, 1, 0)"), 2.0f);  // only 2 weighs anything
    }
    EXPECT_EQ(eval("pick(7, 1, 100)"), eval("pick(7, 1, 100)"));
    EXPECT_FLOAT_EQ(eval("pick(7, 5, 2)"), 5.0f);  // an empty range: lo, as SeExpr
    EXPECT_FLOAT_EQ(eval("choose(0, 10, 20, 30)"), 10.0f);
    EXPECT_FLOAT_EQ(eval("choose(0.5, 10, 20, 30)"), 20.0f);
    EXPECT_FLOAT_EQ(eval("choose(1, 10, 20, 30)"), 30.0f);
    expectVector(evalVector("choose(0.9, [1, 0, 0], [0, 1, 0])"), {0, 1, 0});
    expectVector(evalVector("swatch(0.1, [1, 0, 0], [0, 1, 0])"), {1, 0, 0});
    EXPECT_FLOAT_EQ(eval("wchoose(0.5, 10, 1, 20, 0, 30, 1)"), 10.0f);
    EXPECT_FLOAT_EQ(eval("wchoose(0.75, 10, 1, 20, 0, 30, 1)"), 30.0f);  // a 0 weight is never chosen
    EXPECT_FLOAT_EQ(eval("wchoose(0, 10, 0, 20, 1)"), 20.0f);
    // SeExpr's cycle: lo + index wrapped into the range's size.
    EXPECT_FLOAT_EQ(eval("cycle(5, 1, 3)"), 3.0f);
    EXPECT_FLOAT_EQ(eval("cycle(0, 1, 3)"), 1.0f);
    EXPECT_FLOAT_EQ(eval("cycle(-1, 0, 2)"), 2.0f);
    EXPECT_FLOAT_EQ(eval("hash(3) == hash(3)"), 1.0f);
    EXPECT_NE(eval("hash(3)"), eval("hash(4)"));
    EXPECT_NE(eval("hash(1, 2)"), eval("hash(2, 1)"));
    // rand: () the strand's, (seed), (min, max[, seed]); per strand, the same on every evaluation.
    StrandSet s;
    for (int i = 0; i < 100; ++i) s.addStrand(static_cast<StrandId>(500 + i), {{0, 0, 0}, {0, 1, 0}});
    const Expression r("rand()"), var("$rand"), range("rand(2, 4)"), seeded("rand(2, 4, 7)");
    float lo = 10, hi = 0;
    for (std::size_t i = 0; i < s.strandCount(); ++i) {
        EXPECT_EQ(r.evaluate(s, i, nullptr), var.evaluate(s, i, nullptr));
        const float v = range.evaluate(s, i, nullptr);
        EXPECT_FLOAT_EQ(v, 2.0f + 2.0f * var.evaluate(s, i, nullptr));
        const float w = seeded.evaluate(s, i, nullptr);
        EXPECT_GE(w, 2.0f);
        EXPECT_LE(w, 4.0f);
        EXPECT_NE(w, v);
        lo = std::min(lo, w);
        hi = std::max(hi, w);
    }
    EXPECT_LT(lo, 2.3f);
    EXPECT_GT(hi, 3.7f);
    EXPECT_THROW(Expression("rand(1, 2, 3, 4)"), ExpressionError);
}

TEST(Expression, MathAsSeExpr) {
    // eval's strand: the root at [1, 2, 3], so x is 1 and y is 2.
    EXPECT_FLOAT_EQ(eval("fit(5, 0, 10, 100, 200)"), 150.0f);
    EXPECT_FLOAT_EQ(eval("fit(20, 0, 10, 100, 200)"), 300.0f);
    // smoothstep and linearstep in SeExpr's (x, a, b) order; a > b runs the other way.
    EXPECT_FLOAT_EQ(eval("smoothstep(x, 0, 2)"), 0.5f);
    EXPECT_FLOAT_EQ(eval("smoothstep(y, 4, 0)"), 0.5f);
    EXPECT_FLOAT_EQ(eval("smoothstep(x * 0.5, 0, 1)"), 0.5f);
    EXPECT_FLOAT_EQ(eval("smoothstep(x, 1, 1)"), 1.0f);  // a == b: a step at a
    EXPECT_FLOAT_EQ(eval("linearstep(x, 0, 4)"), 0.25f);
    EXPECT_FLOAT_EQ(eval("linearstep(x, 4, 0)"), 0.75f);
    // GroomLab's older (e0, e1, x) order still reads, when the edges are constants and x is not.
    EXPECT_FLOAT_EQ(eval("smoothstep(0, 2, x)"), 0.5f);
    EXPECT_FLOAT_EQ(eval("linearstep(0, 4, x)"), 0.25f);
    const std::vector<std::pair<std::string, float>> edges = {{"lo", 0.0f}, {"hi", 2.0f}};
    StrandSet one;
    one.addStrand(42, {{1, 2, 3}, {1, 3, 3}});
    EXPECT_FLOAT_EQ(Expression("smoothstep($lo, $hi, $x)", edges).evaluate(one, 0, nullptr), 0.5f);
    EXPECT_FLOAT_EQ(Expression("smoothstep($x, $lo, $hi)", edges).evaluate(one, 0, nullptr), 0.5f);
    // Both orders make the same program (the GPU runs it edges first).
    for (const auto& [seexpr, older] : std::vector<std::pair<std::string, std::string>>{
             {"smoothstep(y, 0.2, 0.8)", "smoothstep(0.2, 0.8, y)"}, {"linearstep(t * 2, -1, 3)", "linearstep(-1, 3, t * 2)"}}) {
        const Expression first(seexpr), second(older);  // program() lives as long as its expression
        const auto& a = first.program();
        const auto& b = second.program();
        ASSERT_EQ(a.size(), b.size()) << seexpr;
        for (std::size_t i = 0; i < a.size(); ++i) {
            EXPECT_EQ(a[i].kind, b[i].kind) << seexpr;
            EXPECT_EQ(a[i].code, b[i].code) << seexpr;
            EXPECT_EQ(a[i].value, b[i].value) << seexpr;
        }
    }
    EXPECT_FLOAT_EQ(eval("gaussstep(x, 0, 2)"), 0.25f);  // 2^(-8 t^2), t = 0.5
    EXPECT_FLOAT_EQ(eval("gaussstep(3, 0, 2) + gaussstep(-1, 0, 2)"), 1.0f);
    // remap(x, source, range, falloff, interp): 1 within range of source, to 0 over the falloff.
    EXPECT_FLOAT_EQ(eval("remap(0.5, 0, 1, 1, 0)"), 1.0f);
    EXPECT_FLOAT_EQ(eval("remap(1.5, 0, 1, 1, 0)"), 0.5f);
    EXPECT_FLOAT_EQ(eval("remap(-1.5, 0, 1, 1, 0)"), 0.5f);
    EXPECT_FLOAT_EQ(eval("remap(3, 0, 1, 1, 0)"), 0.0f);
    EXPECT_FLOAT_EQ(eval("remap(1.5, 0, 1, 1, 1)"), 0.5f);
    EXPECT_FLOAT_EQ(eval("remap(0.5, 0, 1, 0, 0) + remap(1.5, 0, 1, 0, 0)"), 1.0f);  // no falloff: a box
    EXPECT_FLOAT_EQ(eval("compress(0.5, 2, 4)"), 3.0f);
    EXPECT_FLOAT_EQ(eval("expand(3, 2, 4)"), 0.5f);
    EXPECT_NEAR(eval("contrast(0.25, 0.5)"), 0.25f, 1e-6f);  // 0.5: unchanged
    EXPECT_NEAR(eval("contrast(0.25, 0.8)"), 0.1f, 1e-6f);
    EXPECT_NEAR(eval("contrast(0.75, 0.8)"), 0.9f, 1e-6f);
    EXPECT_FLOAT_EQ(eval("boxstep(1, 2) + boxstep(2, 2)"), 1.0f);
    EXPECT_FLOAT_EQ(eval("invert(0.25)"), 0.75f);
    EXPECT_FLOAT_EQ(eval("fmod(-1, 3)"), -1.0f);  // C's fmod; GroomLab's mod is floored
    EXPECT_FLOAT_EQ(eval("mod(-1, 3)"), 2.0f);
    EXPECT_FLOAT_EQ(eval("hypot(3, 4)"), 5.0f);
    EXPECT_NEAR(eval("cbrt(27)"), 3.0f, 1e-5f);
    EXPECT_NEAR(eval("log10(1000)"), 3.0f, 1e-5f);
    EXPECT_FLOAT_EQ(eval("sinh(0) + cosh(0) + tanh(0) + asinh(0) + atanh(0) + acosh(1)"), 1.0f);
    EXPECT_NEAR(eval("cosd(60) + sind(30) + tand(45)"), 2.0f, 1e-5f);
    EXPECT_NEAR(eval("acosd(0.5) + asind(0.5) + atand(1) + atan2d(1, 1)"), 180.0f, 1e-3f);
    EXPECT_NEAR(eval("deg(pi) + rad(180)"), 180.0f + 3.14159265f, 1e-3f);
    EXPECT_FLOAT_EQ(eval("round(2.5) + trunc(-1.7)"), 2.0f);
    EXPECT_NEAR(eval("degrees(radians(90))"), 90.0f, 1e-4f);
    EXPECT_NEAR(eval("gamma(0.25, 2)"), 0.5f, 1e-6f);
    EXPECT_NEAR(eval("bias(0.5, 0.8)"), 0.8f, 1e-5f);
    EXPECT_NEAR(eval("gain(0.5, 0.3)"), 0.5f, 1e-5f);
    EXPECT_FLOAT_EQ(eval("lerp(2, 4, 0.5)"), 3.0f);
    // Vectors.
    EXPECT_NEAR(eval("angle([1, 0, 0], [0, 1, 0])"), 3.14159265f / 2.0f, 1e-6f);
    expectVector(evalVector("ortho([2, 0, 0], [0, 3, 0])"), {0, 0, 1});
    expectVector(evalVector("norm([3, 0, 4])"), {0.6f, 0, 0.8f});
    EXPECT_FLOAT_EQ(eval("dist(P, [1, 2, 3])"), 0.0f);
    expectVector(evalVector("up([0, 1, 0], [1, 0, 0])"), {1, 0, 0});  // +Y turned onto upvec
    expectVector(evalVector("up([0, 1, 0], [0, 2, 0])"), {0, 1, 0});
    expectVector(evalVector("up([0, 1, 0], [0, -1, 0])"), {0, -1, 0});
    expectVector(evalVector("up([1, 0, 0], [1, 0, 0])"), {0, -1, 0});
    // Colors (SeExpr's hue is 0-1 in rgbtohsl, degrees in hsi).
    expectVector(evalVector("rgbtohsl([1, 0, 0])"), {0, 1, 0.5f});
    expectVector(evalVector("hsltorgb([1 / 3, 1, 0.5])"), {0, 1, 0});
    expectVector(evalVector("hsltorgb(rgbtohsl([0.2, 0.5, 0.7]))"), {0.2f, 0.5f, 0.7f});
    expectVector(evalVector("hsi([1, 0, 0], 120, 1, 1)"), {0, 1, 0});
    expectVector(evalVector("hsi([1, 0, 0], 0, 1, 0.5)"), {0.5f, 0, 0});
    expectVector(evalVector("hsi([1, 0, 0], 120, 1, 1, 0)"), {1, 0, 0});  // a mask of 0: unchanged
    expectVector(evalVector("midhsi([1, 0, 0], 120, 1, 1, 1)"), {0, 1, 0});
    expectVector(evalVector("midhsi([1, 0, 0], 120, 1, 1, 0.5)"), {1, 0, 0});  // the mask's middle: unchanged
    expectVector(evalVector("saturate([1, 0, 0], 1)"), {1, 0, 0});
    expectVector(evalVector("saturate([1, 0, 0], 0)"), {0.2126f, 0.2126f, 0.2126f});
}

TEST(Expression, SeExprOperatorsAndStatements) {
    // Logic: 1 or 0; && binds tighter than ||, comparisons tighter than both.
    EXPECT_FLOAT_EQ(eval("1 && 0"), 0.0f);
    EXPECT_FLOAT_EQ(eval("2 && 3"), 1.0f);
    EXPECT_FLOAT_EQ(eval("0 || 0"), 0.0f);
    EXPECT_FLOAT_EQ(eval("0 || -5"), 1.0f);
    EXPECT_FLOAT_EQ(eval("1 || 0 && 0"), 1.0f);
    EXPECT_FLOAT_EQ(eval("1 + 1 == 2 && 3 > 2"), 1.0f);
    EXPECT_FLOAT_EQ(eval("2 < 3 == 1"), 1.0f);
    EXPECT_FLOAT_EQ(eval("!0 + !3 + !x"), 1.0f);
    EXPECT_FLOAT_EQ(eval("x != 1 ? 5 : 6"), 6.0f);
    EXPECT_FLOAT_EQ(eval("~0.25"), 0.75f);
    expectVector(evalVector("~[0.25, 0.5, 1]"), {0.75f, 0.5f, 0});
    EXPECT_FLOAT_EQ(eval("7 % 3"), 1.0f);
    EXPECT_FLOAT_EQ(eval("-7 % 3"), -1.0f);
    EXPECT_FLOAT_EQ(eval("2 * 3 % 4"), 2.0f);
    expectVector(evalVector("[7, 8, 9] % 4"), {3, 0, 1});
    // The apply operator: x->f(a) is f(x, a), the lowest precedence, left to right.
    EXPECT_FLOAT_EQ(eval("x->clamp(0, 0.5)"), 0.5f);
    EXPECT_FLOAT_EQ(eval("2 + 3 -> pow(2)"), 25.0f);
    EXPECT_FLOAT_EQ(eval("$a = 0.25; $a->fit(0, 1, 10, 20)"), 12.5f);
    EXPECT_FLOAT_EQ(eval("x->max(2)->min(1.5)"), 1.5f);
    EXPECT_FLOAT_EQ(eval("[1, 2, 3]->dot([1, 1, 1])"), 6.0f);
    EXPECT_FLOAT_EQ(eval("1 ? 2 : 3 -> max(5)"), 5.0f);
    EXPECT_FLOAT_EQ(eval("(x - 0.5)->abs() * 2"), 1.0f);  // operators after a call take its value
    EXPECT_FLOAT_EQ(eval("x->max(2) + 1"), 3.0f);
    EXPECT_FLOAT_EQ(eval("2 + 3 -> pow(2) - 5"), 20.0f);
    EXPECT_FLOAT_EQ(eval("[3, 0, 4]->norm()[2] * 10"), 8.0f);
    EXPECT_FLOAT_EQ(eval("x->rand()"), eval("rand(x)"));
    EXPECT_FLOAT_EQ(eval("0.5->curve(0, 0, 1, 1, 2, 1)"), 1.0f);
    expectVector(evalVector("P->norm()"), Vec3f{1, 2, 3}.normalized());
    EXPECT_FLOAT_EQ(eval("x -> smoothstep(0, 2)"), 0.5f);
    EXPECT_FLOAT_EQ(eval("x-1"), 0.0f);  // a minus, not an apply
    // Assignments: += -= *= /= %= ^=, also to a built-in name.
    EXPECT_FLOAT_EQ(eval("$a = 2; $a += 3; $a *= 2; $a -= 1; $a /= 3; $a"), 3.0f);
    EXPECT_FLOAT_EQ(eval("$a = 7; $a %= 4; $a ^= 2; $a"), 9.0f);
    EXPECT_FLOAT_EQ(eval("$v = [1, 2, 3]; $v *= 2; $v[2]"), 6.0f);
    EXPECT_FLOAT_EQ(eval("$P += [0, 1, 0]; $P.y"), 3.0f);
    // A vector's index: any number expression (0, 1, 2; another reads 0).
    EXPECT_FLOAT_EQ(eval("$i = 1; P[$i]"), 2.0f);
    EXPECT_FLOAT_EQ(eval("[4, 5, 6][x]"), 5.0f);
    EXPECT_FLOAT_EQ(eval("[4, 5, 6][x + 5]"), 0.0f);
    EXPECT_FLOAT_EQ(eval("[4, 5, 6][1 + 1]"), 6.0f);
    // if / else if / else: locals set in a branch.
    EXPECT_FLOAT_EQ(eval("$a = 1; if (x > 0) { $a = 2; } $a"), 2.0f);
    EXPECT_FLOAT_EQ(eval("$a = 1; if (x < 0) { $a = 2; } $a"), 1.0f);
    EXPECT_FLOAT_EQ(eval("if (x < 0) { $a = 2; } else { $a = 3; } $a"), 3.0f);
    EXPECT_FLOAT_EQ(eval("if (x > 5) { $a = 1; } else if (x > 0) { $a = 2; } else { $a = 3; }\n$a"), 2.0f);
    EXPECT_FLOAT_EQ(eval("if (x > 5) { $a = 1; } else if (x > 3) { $a = 2; } else { $a = 3; }\n$a"), 3.0f);
    EXPECT_FLOAT_EQ(eval("$a = 0;\nif (x > 0) {\n  if (y > 5) { $a = 1; } else { $a = 2; }\n  $b = 4;\n} else {\n  $b = 5;\n}\n$a + $b"), 6.0f);
    EXPECT_FLOAT_EQ(eval("$c = 0; if (x > 0) { $c = [1, 2, 3]; } $c[1]"), 2.0f);
    EXPECT_FLOAT_EQ(eval("$c = 0; if (x < 0) { $c = [1, 2, 3]; } $c[1]"), 0.0f);
    EXPECT_FLOAT_EQ(eval("if (x > 0) { $y = 10; } $y"), 10.0f);  // a built-in name set in one branch
    EXPECT_FLOAT_EQ(eval("if (x < 0) { $y = 10; } $y"), 2.0f);
    EXPECT_FLOAT_EQ(eval("if (x > 0 && y > 1) { $a = 1; $a += 1; } else { $a = 0; } $a * 10"), 20.0f);
    EXPECT_FLOAT_EQ(eval("if (x > 0) { $unused = 1; } 7"), 7.0f);
    try {  // a local set in one branch only, read below
        Expression("if (x > 0) {\n  $q = 1;\n}\n$q");
        FAIL();
    } catch (const ExpressionError& err) {
        EXPECT_NE(std::string(err.what()).find("one branch"), std::string::npos) << err.what();
    }
    for (const char* bad : {"if x > 0 { $a = 1; } 1", "if (x > 0) $a = 1; 1", "if (x > 0) { $a = 1 } 1", "if (P) { $a = 1; } 1",
                            "else { $a = 1; } 1", "if (x) { 2 } 1", "$v.x = 1; 2", "x->", "x->2", "x->foo(1)", "x->map(\"a\")",
                            "1 && P", "!P", "[1, 2, 3][1.5]", "x[0]"}) {
        EXPECT_THROW(Expression{bad}, ExpressionError) << bad;
    }
    // Logic, if/else and constant indices make only the ops a GPU runs (numbers, variables,
    // comparisons, selects): such an expression still previews there.
    for (const char* text : {"x > 0 && y < 3 || !z", "$a = 1; if (x > 0) { $a = 2; } else if (y > 0) { $a = 3; } $a",
                              "~x"}) {
        const Expression compiled(text);  // the program lives as long as its expression (not a temporary's)
        for (const Expression::Op& op : compiled.program()) {
            EXPECT_TRUE(op.kind == Expression::Op::Number || op.kind == Expression::Op::Variable ||
                        op.kind == Expression::Op::Unary || op.kind == Expression::Op::Binary ||
                        op.kind == Expression::Op::Select)
                << text;
            if (op.kind == Expression::Op::Binary) EXPECT_LT(op.code, Expression::binaryCode("%")) << text;
        }
    }
}

TEST(Expression, XGenVariablesAndUnsupportedNames) {
    // eval's strand: ID 42, root [1, 2, 3] (no scalp: the first point), length 1.
    EXPECT_FLOAT_EQ(eval("$id"), 42.0f);
    EXPECT_FLOAT_EQ(eval("$cLength"), eval("$length"));
    EXPECT_FLOAT_EQ(eval("$faceId"), -1.0f);  // no root on a scalp
    expectVector(evalVector("$Pref"), {1, 2, 3});  // no rest pose: the root
    expectVector(evalVector("$Pg + $Pw - $Pgw"), {1, 2, 3});
    expectVector(evalVector("$Ng"), evalVector("$N"));
    EXPECT_FLOAT_EQ(eval("$Prefw.y + $Prefg[2]"), 5.0f);
    EXPECT_FLOAT_EQ(eval("$prefx + $faceid"), 0.0f);
    // On a scalp: the root's triangle, and the root on its rest pose.
    auto scalp = std::make_shared<ScalpMesh>(*test::makePlane(4));
    std::vector<Vec3f> rest = scalp->points();
    for (Vec3f& p : rest) p.y += 5.0f;
    scalp->setRestPoints(rest);
    StrandSet s;
    StrandRoot root;
    root.face = 3;
    root.u = 0.2f;
    root.v = 0.3f;
    const Vec3f at = scalp->position(root);
    s.addStrand(0x12345678ABCDull, {at, at + Vec3f{0, 1, 0}}, {}, root);
    EXPECT_FLOAT_EQ(Expression("$faceId").evaluate(s, 0, scalp.get()), 3.0f);
    EXPECT_FLOAT_EQ(Expression("$id").evaluate(s, 0, scalp.get()), static_cast<float>(0x78ABCDu));
    EXPECT_NEAR(Expression("$Pref.y - $P.y").evaluate(s, 0, scalp.get()), 5.0f, 1e-5f);
    EXPECT_FLOAT_EQ(Expression("$faceId").evaluateAt(root, scalp.get()), 3.0f);
    EXPECT_NEAR(Expression("$Pref.y - $P.y").evaluateAt(root, scalp.get()), 5.0f, 1e-5f);
    // XGen's names without an equivalent, and SeExpr's functions GroomLab does not run: errors
    // saying so, never a channel that reads 0.
    for (const char* name : {"$Nref", "$dPdu", "$dPdv.x", "$cWidth", "$cDepth", "$aCount", "$descId", "$patchId", "$objectId",
                             "length($Cs)", "$As", "$DESC"}) {
        try {
            Expression{name};
            FAIL() << name;
        } catch (const ExpressionError& err) {
            EXPECT_NE(std::string(err.what()).find("XGen"), std::string::npos) << err.what();
        }
    }
    for (const char* call : {"printf(\"%f\", 1)", "getVar(\"x\", 1)", "sprintf(\"a\")"}) {
        try {
            Expression{call};
            FAIL() << call;
        } catch (const ExpressionError& err) {
            EXPECT_NE(std::string(err.what()).find("not supported"), std::string::npos) << err.what();
        }
    }
    try {
        Expression("wobble(1)");
        FAIL();
    } catch (const ExpressionError& err) {
        EXPECT_NE(std::string(err.what()).find("unknown function 'wobble'"), std::string::npos) << err.what();
    }
    // A local of such a name is the artist's own.
    expectVector(evalVector("$Cs = [1, 0, 0]; $Cs->hsi(120, 1, 1)"), {0, 1, 0});
}

TEST(Expression, XGenMapPathsReadTheGroomsMapsByName) {
    EXPECT_EQ(expressionMapName("crown"), "crown");
    EXPECT_EQ(expressionMapName("${DESC}/paintmaps/length/"), "length");
    EXPECT_EQ(expressionMapName("${DESC}/paintmaps/region.ptx"), "region");
    EXPECT_EQ(expressionMapName("C:\\maps\\Density.TIF"), "Density");
    const ExpressionDependencies deps = scanExpression("$a = map('${DESC}/paintmaps/length/'); $a * vmap(\"x/y/tint\").x + map(\"crown\")");
    EXPECT_EQ(deps.maps, (std::vector<std::string>{"length", "tint", "crown"}));
    struct Fake : ExpressionMap {
        float at(const StrandRoot&, int component) const override { return 0.25f * static_cast<float>(component); }
    };
    ExpressionEnvironment environment;
    environment.map = [](const std::string& name) -> ExpressionMapPtr {
        return name == "length" ? std::make_shared<Fake>() : nullptr;
    };
    StrandSet s;
    StrandRoot root;
    root.face = 0;
    s.addStrand(1, {{0, 0, 0}, {0, 1, 0}}, {}, root);
    const ExpressionEnvironmentScope scope(&environment);
    EXPECT_FLOAT_EQ(Expression("map('${DESC}/paintmaps/length/')").evaluate(s, 0, nullptr), 1.0f);  // luminance: 4 x 0.25
    expectVector(Expression("vmap('${DESC}/paintmaps/length/')", {}, ExpressionType::Vector).evaluateVector(s, 0, nullptr),
                 {0, 0.25f, 0.5f});
    try {
        Expression("map('${DESC}/paintmaps/density/')");
        FAIL();
    } catch (const ExpressionError& err) {
        const std::string what = err.what();
        EXPECT_NE(what.find("XGen"), std::string::npos) << what;
        EXPECT_NE(what.find("'density'"), std::string::npos) << what;
    }
    EXPECT_THROW(Expression("map('${DESC}/paintmaps/')"), ExpressionError);
}

TEST(Expression, XGenExpressionsPastedVerbatim) {
    // From Autodesk's XGen expression examples and SeExpr's documentation, as written there.
    StrandSet s;
    for (int i = 0; i < 64; ++i) s.addStrand(static_cast<StrandId>(9000 + i), {{0.1f * i, 1, 0}, {0.1f * i, 2.5f, 0}});
    auto each = [&](const std::string& text, const std::function<void(float)>& check) {
        const Expression e(text);
        const Expression::Bound bound = e.bind(s);
        for (std::size_t i = 0; i < s.strandCount(); ++i) check(e.evaluate(s, i, nullptr, &bound));
    };
    // Randomize: rand($min, $max) per strand.
    float lo = 10, hi = 0;
    each("$min= 0.4000; #0.10,1.00\n$max=3.000; #1.0,5.0\nrand($min,$max)", [&](float v) {
        EXPECT_GE(v, 0.4f);
        EXPECT_LE(v, 3.0f);
        lo = std::min(lo, v);
        hi = std::max(hi, v);
    });
    EXPECT_LT(lo, 1.0f);
    EXPECT_GT(hi, 2.4f);
    // Noise: noise($lo, $hi) (two numbers: 2D noise, 0 to 1).
    each("$hi =50.0; #0.00,100.00\n$lo =-10.00; #-100.00,0.00\nnoise($lo, $hi)", [&](float v) {
        EXPECT_GE(v, 0.0f);
        EXPECT_LE(v, 1.0f);
    });
    // Conditional: the computed length clamped.
    each("$a =$cLength;\n$max =7.00; #0.00,10.0\nclamp($a,0,$max)", [&](float v) { EXPECT_NEAR(v, 1.5f, 1e-5f); });
    each("$a =$cLength;\n$max =1.00; #0.00,10.0\nclamp($a,0,$max)", [&](float v) { EXPECT_FLOAT_EQ(v, 1.0f); });
    // Expand a function's range: smoothstep's 0-1 onto -100 to 100 with ->.
    const Expression expand("$Scalemin=0.3;\n$Scalemax=4.0;\nsmoothstep(x,$Scalemin,$Scalemax)->fit(0,1,-100,100)");
    for (std::size_t i = 0; i < s.strandCount(); ++i) {
        const float x = s.strandPoints(i)[0].x;
        EXPECT_NEAR(expand.evaluate(s, i, nullptr), smoothstep(0.3f, 4.0f, x) * 200.0f - 100.0f, 1e-3f) << x;
    }
    // SeExpr's documentation: a color through contrast and clamp.
    const Expression contrast("$Cs = cnoise($P * 3);\n$Cs -> contrast(.7) -> clamp(0.2, 0.8)", {}, ExpressionType::Vector);
    for (std::size_t i = 0; i < s.strandCount(); ++i) {
        const Vec3f c = contrast.evaluateVector(s, i, nullptr);
        for (const float v : {c.x, c.y, c.z}) {
            EXPECT_GE(v, 0.2f);
            EXPECT_LE(v, 0.8f);
        }
    }
    // An XGen ramp's curve (monotone spline keys), of the root height.
    each("$t = $P[1] / 2;\ncurve($t, 0.000, 0.000, 4, 0.500, 1.000, 4, 1.000, 0.000, 4)", [&](float v) { EXPECT_NEAR(v, 1.0f, 1e-5f); });
    // Common idioms: a signed noise and a fractal mask with ranged sliders in comments.
    each("$freq = 4.0; #1,10\n$amp = 0.5; #0,1\n$n = fbm($P * $freq, 4, 2, 0.5) * 2 - 1;\n1 + $amp * $n", [&](float v) {
        EXPECT_GE(v, -1.0f);
        EXPECT_LE(v, 3.0f);
    });
    each("$mask = smoothstep(noise($P * 2), 0.4, 0.6);\n$mask * 0.5 + 0.5", [&](float v) {
        EXPECT_GE(v, 0.5f);
        EXPECT_LE(v, 1.0f);
    });
}

TEST(Expression, TheShippedLibraryCompilesAndEvaluates) {
    // hosts/maya/expressions: every example, with its own variables, on strands over a sphere.
    const std::filesystem::path dir = std::filesystem::path(GROOMLAB_SHIPPED_PRESETS).parent_path() / "expressions";
    ASSERT_TRUE(std::filesystem::is_directory(dir)) << dir.string();
    const auto scalp = test::makeSphere(8, 16, 1.0f);
    StrandSet s;
    for (std::size_t f = 0; f < scalp->triangleCount(); f += 7) {
        StrandRoot root;
        root.face = static_cast<std::int32_t>(f);
        root.u = root.v = 0.3f;
        const Vec3f p = scalp->position(root), n = scalp->normal(root);
        s.addStrand(static_cast<StrandId>(f + 1), {p, p + n * 0.5f, p + n * 1.0f}, {}, root);
    }
    s.addStrandChannel("clumpId");
    int count = 0;
    for (const auto& entry : std::filesystem::directory_iterator(dir)) {
        if (entry.path().extension() != ".json") continue;
        std::ifstream in(entry.path());
        const nlohmann::json j = nlohmann::json::parse(in);
        const std::string text = j.at("expression").get<std::string>();
        Expression e;
        ASSERT_NO_THROW(e = Expression(text, parseExpressionVariables(j.value("variables", nlohmann::json::object()).dump())))
            << entry.path().filename().string() << ": " << text;
        const Expression::Bound bound = e.bind(s);
        float lo = 1e9f, hi = -1e9f;
        for (std::size_t i = 0; i < s.strandCount(); ++i) {
            const float v = bound.perPoint ? e.evaluatePoint(s, i, 1, scalp.get(), &bound) : e.evaluate(s, i, scalp.get(), &bound);
            EXPECT_TRUE(std::isfinite(v));
            lo = std::min(lo, v);
            hi = std::max(hi, v);
        }
        EXPECT_GE(lo, j.value("min", 0.0) - 1e-4) << entry.path().filename().string();
        EXPECT_LE(hi, j.value("max", 1.0) + 1e-4) << entry.path().filename().string();
        ++count;
    }
    EXPECT_GE(count, 10);
}

TEST(Expression, TheDocsExamplesEvaluate) {
    // web/content/docs/expressions.mdx, "Examples", as written there: numbers in 0-1, vectors of length 1.
    const std::vector<std::string> numbers = {
        "# 1 where the scalp faces up, less toward the back of the head (+Z is the face)\n$crown = smoothstep($ny, -0.2, 0.8);\n"
        "$nape = smoothstep(-$nz, 0.3, 0.9);\nclamp($crown - 0.5 * $nape, 0, 1)",
        "# the band runs from U 0.3 to 0.7, with edges 0.05 wide\n$inside = smoothstep($u, 0.25, 0.3) * (1 - smoothstep($u, 0.7, 0.75));\n"
        "mix(0.2, 1, $inside)",
        "# most strands clump tightly; one in five stays loose\n$loose = $rand < 0.2;\n$loose ? rand(0.2, 0.5, 11) : rand(0.85, 1, 12)",
        "# nothing on the first 60% of each strand, full at the tip, on a third of the strands\n$tips = smoothstep($t, 0.6, 1);\n"
        "$frizzy = $rand < 0.33;\n$frizzy * $tips",
        "# soft-edged patches; the scale sets their size, the 0.45-0.55 band their edge\n$scale = 3;\n$n = fbm($P * $scale, 4);\n"
        "smoothstep($n, 0.45, 0.55)",
        "# a base of 0.55, each clump a little darker or lighter, each strand a little more,\n# and 3% grey strands\n"
        "$clump = (hash($clumpId) - 0.5) * 0.15;\n$strand = rand(-0.05, 0.05);\n$grey = $rand < 0.03;\n"
        "$grey ? 0.05 : clamp(0.55 + $clump + $strand, 0, 1)",
        "# the root's height from 1.2 to 1.8 onto 0-1, then a monotone curve through four keys\n"
        "$h = $P[1] -> fit(1.2, 1.8, 0, 1) -> clamp(0, 1);\ncurve($h, 0, 0.3, 4, 0.5, 1, 4, 1, 0.6, 4)",
    };
    const std::vector<std::string> vectors = {
        "# down, tipped out along the scalp's normal, more on the sides than the top\n$lean = 0.2 + 0.4 * (1 - abs($ny));\n"
        "normalize([0, -1, 0] + $N * $lean)",
        "if ($x < 0) {\n    $side = -1;\n} else {\n    $side = 1;\n}\nnormalize([$side, -0.3, 0])",
    };
    auto scalp = std::make_shared<ScalpMesh>(*test::makeSphere(12, 24, 1.5f));
    std::vector<ScalpMesh::UV> uvs;  // U around the sphere, V up it
    for (const auto& tri : scalp->triangles()) {
        for (const auto corner : tri) {
            const Vec3f p = scalp->points()[static_cast<std::size_t>(corner)];
            uvs.push_back({0.5f + std::atan2(p.z, p.x) / 6.2831853f, 0.5f + p.y / 3.0f});
        }
    }
    scalp->setCornerUVs(uvs);
    StrandSet s;
    for (std::size_t f = 0; f < scalp->triangleCount(); f += 3) {
        StrandRoot root;
        root.face = static_cast<std::int32_t>(f);
        root.u = root.v = 0.3f;
        const Vec3f p = scalp->position(root), n = scalp->normal(root);
        s.addStrand(static_cast<StrandId>(7 * f + 3), {p, p + n * 0.2f, p + n * 0.4f, p + n * 0.6f}, {}, root);
    }
    float* clumps = s.addStrandChannel("clumpId").data();
    for (std::size_t i = 0; i < s.strandCount(); ++i) clumps[i] = static_cast<float>(i / 10);
    for (const std::string& text : numbers) {
        Expression e;
        ASSERT_NO_THROW(e = Expression(text)) << text;
        const Expression::Bound bound = e.bind(s);
        float lo = 1e9f, hi = -1e9f;
        for (std::size_t i = 0; i < s.strandCount(); ++i) {
            for (std::size_t p = 0; p < (bound.perPoint ? 4u : 1u); ++p) {
                const float v = bound.perPoint ? e.evaluatePoint(s, i, p, scalp.get(), &bound) : e.evaluate(s, i, scalp.get(), &bound);
                lo = std::min(lo, v);
                hi = std::max(hi, v);
            }
        }
        EXPECT_GE(lo, 0.0f) << text;
        EXPECT_LE(hi, 1.0f) << text;
        EXPECT_LT(lo, hi) << text;  // it varies over the head
    }
    for (const std::string& text : vectors) {
        Expression e;
        ASSERT_NO_THROW(e = Expression(text, {}, ExpressionType::Vector)) << text;
        for (std::size_t i = 0; i < s.strandCount(); ++i) EXPECT_NEAR(e.evaluateVector(s, i, scalp.get()).length(), 1.0f, 1e-5f) << text;
    }
    // The map example, with the groom's map named "length".
    struct Half : ExpressionMap {
        float at(const StrandRoot&, int) const override { return 0.5f; }
    };
    ExpressionEnvironment environment;
    environment.map = [](const std::string& name) -> ExpressionMapPtr { return name == "length" ? std::make_shared<Half>() : nullptr; };
    const ExpressionEnvironmentScope scope(&environment);
    EXPECT_FLOAT_EQ(Expression("$a = map('${DESC}/paintmaps/length/');  #3dpaint, 200\n$a").evaluate(s, 0, scalp.get()), 0.5f);
}

TEST(Expression, MapsReadTheGroomsMapsAtTheRoot) {
    // A map the environment finds: its value at the strand's root.
    struct Fake : ExpressionMap {
        float at(const StrandRoot& root, int component) const override { return root.u + 0.1f * static_cast<float>(component); }
    };
    ExpressionEnvironment environment;
    environment.map = [](const std::string& name) -> ExpressionMapPtr {
        return name == "mask" ? std::make_shared<Fake>() : nullptr;
    };
    StrandSet s;
    StrandRoot root;
    root.face = 0;
    root.u = 0.5f;
    s.addStrand(1, {{0, 0, 0}, {0, 1, 0}}, {}, root);
    {
        const ExpressionEnvironmentScope scope(&environment);
        EXPECT_FLOAT_EQ(Expression("map(\"mask\")").evaluate(s, 0, nullptr), 0.9f);  // luminance: component 4
        EXPECT_FLOAT_EQ(Expression("map('mask', \"g\")").evaluate(s, 0, nullptr), 0.6f);
        EXPECT_FLOAT_EQ(Expression("map(\"mask\", 3)").evaluate(s, 0, nullptr), 0.8f);
        const Vec3f c = Expression("cmap(\"mask\")", {}, ExpressionType::Vector).evaluateVector(s, 0, nullptr);
        expectVector(c, {0.5f, 0.6f, 0.7f});
        EXPECT_FLOAT_EQ(Expression("map(\"mask\")").evaluateAt(root, nullptr), 0.9f);
        EXPECT_THROW(Expression("map(\"other\")"), ExpressionError);  // no such map
        EXPECT_THROW(Expression("map(\"mask\", \"q\")"), ExpressionError);
        EXPECT_THROW(Expression("map(mask)"), ExpressionError);
    }
    EXPECT_FLOAT_EQ(Expression("map(\"mask\")").evaluate(s, 0, nullptr), 0.0f);  // no environment: 0

    // In a groom: a vertex map on the scalp (the one a Map node reads) cuts the length by its
    // green, as the color set's green as a map does.
    auto scalp = std::make_shared<ScalpMesh>(*test::makePlane(8));
    scalp->setCornerColors("cut", std::vector<ScalpMesh::Color>(scalp->triangleCount() * 3, ScalpMesh::Color{0.2f, 0.5f, 0.8f, 1.0f}));
    GroomData d;
    d.scalp = scalp;
    std::string graph = defaultGraphJson();
    const std::string from = "\"type\": \"Length\", \"params\": {\"fraction\": 1.0, \"randomness\": 0.2}";
    graph.replace(graph.find(from), from.size(),
                  R"j("type": "Length", "params": {"keep": {"expression": "map(\"cut\", \"g\")"}})j");
    d.graphJson = graph;
    Groom painted(d);
    const StrandsPtr mapped = painted.evaluate(1.0f, 0.0);
    // Painted again (the color set's values change, the surface does not): the node reads the new map.
    auto repainted = std::make_shared<ScalpMesh>(*scalp);
    repainted->setCornerColors("cut", std::vector<ScalpMesh::Color>(scalp->triangleCount() * 3, ScalpMesh::Color{0.2f, 0.9f, 0.8f, 1.0f}));
    painted.setScalp(repainted);
    EXPECT_NE(painted.evaluate(1.0f, 0.0)->contentHash(), mapped->contentHash());
    graph = defaultGraphJson();
    const std::string colorSet = R"j("keep": {"colorSet": "cut", "component": "g"})j";
    graph.replace(graph.find(from), from.size(), R"j("type": "Length", "params": {)j" + colorSet + "}");
    d.graphJson = graph;
    EXPECT_EQ(mapped->contentHash(), Groom(d).evaluate(1.0f, 0.0)->contentHash());
    // A map the groom does not have is the node's error.
    graph.replace(graph.find(colorSet), colorSet.size(), R"j("keep": {"expression": "map(\"nothing\")"})j");
    d.graphJson = graph;
    EXPECT_THROW(Groom(d).evaluate(1.0f, 0.0), GraphError);
}

TEST(Expression, VectorParamsTakeExpressions) {
    // Gravity's direction per strand: an expression of [0, -1, 0] is the vec3 itself, bit for bit.
    GroomData d;
    d.scalp = test::makeSphere(12, 24, 1.0f);
    const std::string base = defaultGraphJson();
    const std::string from = "\"type\": \"Length\", \"params\": {\"fraction\": 1.0, \"randomness\": 0.2}";
    auto withGravity = [&](const std::string& direction) {
        std::string graph = base;
        graph.replace(graph.find(from), from.size(),
                      "\"type\": \"Gravity\", \"params\": {\"strength\": 0.6, \"direction\": " + direction + "}");
        GroomData data = d;
        data.graphJson = graph;
        return Groom(data).evaluate(1.0f, 0.0)->contentHash();
    };
    const std::uint64_t down = withGravity("[0, -1, 0]");
    EXPECT_EQ(withGravity(R"j({"expression": "[0, -1, 0]"})j"), down);
    EXPECT_EQ(withGravity(R"j({"expression": "[0, -0.5, 0] * 2"})j"), down);
    EXPECT_NE(withGravity(R"j({"expression": "x > 0 ? [0, -1, 0] : [1, 0, 0]"})j"), down);
    // A number where a vector is wanted, or a map that is not an expression: the node's error.
    EXPECT_THROW(withGravity(R"j({"expression": "y"})j"), GraphError);
    EXPECT_THROW(withGravity(R"j({"colorSet": "dir"})j"), GraphError);
}

#if GROOMLAB_HAS_TBB
TEST(Expression, NewFunctionsMakeTheSameOnAnyThreadCount) {
    GroomData d;
    d.scalp = test::makeSphere(16, 32, 1.0f);
    std::string graph = defaultGraphJson();
    const std::string from = "\"type\": \"Length\", \"params\": {\"fraction\": 1.0, \"randomness\": 0.2}";
    graph.replace(graph.find(from), from.size(),
                  R"j("type": "Length", "params": {"keep": {"expression": "$c = voronoi(P * 4, 4); if ($c > 0.2) { $c = $c->contrast(0.7); } 0.4 + 0.3 * fbm(P * 3, 5) - 0.1 + $c * 0.2 + curve(pick(rand, 0, 4) / 4, 0, 0, 4, 1, 0.2, 4) + 0.05 * pnoise(P * 3, 2) + 0.05 * rand(-1, 1, 3)"}})j");
    d.graphJson = graph;
    const std::uint64_t all = Groom(d).evaluate(1.0f, 0.0)->contentHash();
    {
        const tbb::global_control single(tbb::global_control::max_allowed_parallelism, 1);
        EXPECT_EQ(Groom(d).evaluate(1.0f, 0.0)->contentHash(), all);
    }
    {
        const tbb::global_control pair(tbb::global_control::max_allowed_parallelism, 2);
        EXPECT_EQ(Groom(d).evaluate(1.0f, 0.0)->contentHash(), all);
    }
}
#endif

// Every builtin the editors list is named in the docs page's Functions section (in a `code` span,
// alone or among others: `sin cos tan`) and described, and the GPU's codes (look.slang fieldCall's
// cases) are one function's each.
TEST(Expression, EveryBuiltinIsDocumentedAndHasItsOwnGpuCode) {
    std::ifstream in(std::filesystem::path(GROOMLAB_TEST_DATA) / ".." / ".." / "web" / "content" / "docs" / "expressions.mdx");
    ASSERT_TRUE(in) << "web/content/docs/expressions.mdx";
    std::stringstream text;
    text << in.rdbuf();
    const std::string page = text.str();
    const std::size_t from = page.find("\n## Functions");
    ASSERT_NE(from, std::string::npos);
    const std::size_t to = page.find("\n## ", from + 1);
    const std::string section = page.substr(from, to == std::string::npos ? std::string::npos : to - from);
    std::set<std::string> named;  // the words inside the section's code spans
    for (std::size_t open = section.find('`'); open != std::string::npos;) {
        const std::size_t close = section.find('`', open + 1);
        if (close == std::string::npos) break;
        std::string word;
        for (std::size_t i = open + 1; i <= close; ++i) {
            const char c = section[i];
            if (std::isalnum(static_cast<unsigned char>(c)) || c == '_') {
                word += c;
            } else if (!word.empty()) {
                named.insert(word);
                word.clear();
            }
        }
        open = section.find('`', close + 1);
    }
    const std::vector<ExpressionFunction>& functions = Expression::functions();
    EXPECT_GT(functions.size(), 100u);
    for (const ExpressionFunction& f : functions) {
        EXPECT_TRUE(named.count(f.name)) << f.name << " is not in expressions.mdx's Functions section";
        EXPECT_FALSE(f.arguments.empty() && f.maxArgs > 0) << f.name;
        EXPECT_FALSE(f.about.empty()) << f.name;
    }
    std::set<int> gpu;
    for (const char* name : {"sin", "smoothstep", "fit", "bias", "gain", "linearstep", "lerp", "mod"}) {
        const int code = Expression::gpuFunction(Expression::functionCode(name));
        EXPECT_GE(code, 0) << name;
    }
    for (int code = 0; code < 256; ++code) {
        const int g = Expression::gpuFunction(code);
        if (g >= 0) EXPECT_TRUE(gpu.insert(g).second) << "GPU code " << g << " is two functions'";
    }
    EXPECT_EQ(Expression::gpuFunction(Expression::functionCode("voronoi")), -1);
    EXPECT_EQ(Expression::gpuFunction(-1), -1);
}

}  // namespace
}  // namespace groomlab
