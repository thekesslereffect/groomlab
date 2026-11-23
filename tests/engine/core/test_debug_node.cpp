// SPDX-License-Identifier: Apache-2.0
// Debug: colors the hair by a channel, a number field or expression, or a color field, writes the
// number to the "debug" channel and notes what it found.

#include <gtest/gtest.h>

#include <array>
#include <map>
#include <set>
#include <string>

#include <nlohmann/json.hpp>

#include "groomlab/display.h"
#include "groomlab/groom.h"
#include "groomlab/groom_session.h"
#include "test_helpers.h"

namespace groomlab {
namespace {

using nlohmann::json;
using Rgb = std::array<float, 3>;

struct Shown {
    StrandsPtr strands;
    std::string note;
    std::string error;
};

// Scalp -> Scatter -> Length -> Clump (12) -> Resample -> Debug(params) [<- an Expression into `plug`].
Shown debugged(const json& params, const std::string& expression = {}, const std::string& plug = {}) {
    json graph = {
        {"version", 1},
        {"output", "debug"},
        {"nodes",
         {{{"name", "scalp"}, {"type", "Scalp"}},
          {{"name", "scatter"}, {"type", "Scatter"}, {"params", {{"density", 500.0}}}},
          {{"name", "length"}, {"type", "Length"}, {"params", {{"keep", 1.0}}}},
          {{"name", "clump"}, {"type", "Clump"}, {"params", {{"count", 12}, {"strength", 0.0}}}},
          {{"name", "resolution"}, {"type", "Resample"}, {"params", {{"segments", 6}}}},
          {{"name", "debug"}, {"type", "Debug"}, {"params", params}}}},
        {"connections",
         {{{"from", "scalp"}, {"to", "scatter"}, {"input", "scalp"}},
          {{"from", "scatter"}, {"to", "length"}, {"input", "strands"}},
          {{"from", "length"}, {"to", "clump"}, {"input", "strands"}},
          {{"from", "clump"}, {"to", "resolution"}, {"input", "strands"}},
          {{"from", "resolution"}, {"to", "debug"}, {"input", "strands"}}}}};
    if (!expression.empty()) {
        graph["nodes"].push_back({{"name", "expr"}, {"type", "Expression"}, {"params", {{"expression", expression}}}});
        for (json& n : graph["nodes"]) {
            if (n["name"] == "debug") n["paramInputs"] = {plug};
        }
        graph["connections"].push_back({{"from", "expr"}, {"to", "debug"}, {"input", plug}});
    }
    GroomData d;
    d.scalp = test::makePlane(8);
    d.graphJson = graph.dump();
    GroomSession session(d);
    const ViewResult& view = session.evaluate(ViewRequest{});
    Shown out;
    out.strands = view.strands;
    if (!view.errors.empty()) out.error = view.errors[0];
    for (const auto& [node, note] : session.nodeNotes()) {
        if (node == "debug") out.note = note;
    }
    return out;
}

Rgb strandColor(const StrandSet& s, std::size_t i) {
    return {(*s.strandChannel("colorR"))[i], (*s.strandChannel("colorG"))[i], (*s.strandChannel("colorB"))[i]};
}

TEST(Debug, AChannelColorsTheStrandsAndIsNoted) {
    const Shown shown = debugged({{"show", "channel"}, {"channel", "clumpId"}, {"colors", "random"}});
    ASSERT_TRUE(shown.error.empty()) << shown.error;
    ASSERT_TRUE(shown.strands);
    const StrandSet& s = *shown.strands;
    const std::vector<float>& clumps = *s.strandChannel("clumpId");
    std::map<int, Rgb> byClump;
    for (std::size_t i = 0; i < s.strandCount(); ++i) {
        const Rgb c = strandColor(s, i);
        EXPECT_EQ(c, clumpColor(static_cast<std::int64_t>(clumps[i])));  // a color per clump
        byClump.emplace(static_cast<int>(clumps[i]), c);
    }
    EXPECT_GT(byClump.size(), 8u);
    EXPECT_EQ(*s.strandChannel("debug"), clumps);
    EXPECT_NE(shown.note.find("clumpId"), std::string::npos) << shown.note;
    EXPECT_NE(shown.note.find("whole numbers"), std::string::npos) << shown.note;
    EXPECT_NE(shown.note.find("per strand"), std::string::npos) << shown.note;
}

TEST(Debug, NoChannelNamedListsTheStrandsChannels) {
    const Shown shown = debugged(json::object());
    EXPECT_TRUE(shown.error.empty()) << shown.error;
    EXPECT_NE(shown.note.find("clumpId"), std::string::npos) << shown.note;
    ASSERT_TRUE(shown.strands);
    EXPECT_FALSE(shown.strands->strandChannel("debug"));  // passed through as it came
    const Shown missing = debugged({{"channel", "nothing"}});
    EXPECT_NE(missing.error.find("no channel 'nothing'"), std::string::npos) << missing.error;
    EXPECT_NE(missing.error.find("clumpId"), std::string::npos) << missing.error;
}

TEST(Debug, AnExpressionAlongTheStrandShowsPerPoint) {
    // Typed in value, and plugged in from an Expression node: the same.
    for (const Shown& shown : {debugged({{"show", "value"}, {"value", {{"expression", "$t"}}}}),
                               debugged({{"show", "value"}}, "$t", "value")}) {
        ASSERT_TRUE(shown.error.empty()) << shown.error;
        ASSERT_TRUE(shown.strands);
        const StrandSet& s = *shown.strands;
        ASSERT_TRUE(s.pointChannel("colorR"));
        ASSERT_TRUE(s.pointChannel("debug"));
        for (std::size_t i = 0; i < s.strandCount(); ++i) {
            const std::size_t root = s.strandBegin(i), tip = root + s.strandPointCount(i) - 1;
            const Rgb rootColor{(*s.pointChannel("colorR"))[root], (*s.pointChannel("colorG"))[root], (*s.pointChannel("colorB"))[root]};
            const Rgb tipColor{(*s.pointChannel("colorR"))[tip], (*s.pointChannel("colorG"))[tip], (*s.pointChannel("colorB"))[tip]};
            EXPECT_EQ(rootColor, rampColor(0.0f));
            EXPECT_EQ(tipColor, rampColor(1.0f));
            EXPECT_NEAR((*s.pointChannel("debug"))[tip], 1.0f, 1e-5f);
        }
        EXPECT_NE(shown.note.find("0 to 1"), std::string::npos) << shown.note;
        EXPECT_NE(shown.note.find("per point"), std::string::npos) << shown.note;
    }
    // A set range, in gray.
    const Shown gray = debugged({{"show", "value"}, {"value", {{"expression", "$t"}}}, {"colors", "gray"}, {"range", "set"},
                                 {"min", 0.0}, {"max", 2.0}});
    ASSERT_TRUE(gray.strands);
    const std::size_t tip = gray.strands->strandBegin(0) + gray.strands->strandPointCount(0) - 1;
    EXPECT_NEAR((*gray.strands->pointChannel("colorG"))[tip], 0.5f, 1e-5f);
}

TEST(Debug, AVectorExpressionShowsAsItsColor) {
    const Shown shown = debugged({{"show", "color"}}, "[$x, 0.5, 1 - $x]", "color");
    ASSERT_TRUE(shown.error.empty()) << shown.error;
    ASSERT_TRUE(shown.strands);
    const StrandSet& s = *shown.strands;
    EXPECT_FALSE(s.pointChannel("colorR"));  // per strand
    for (std::size_t i = 0; i < s.strandCount(); ++i) {
        const float x = s.strandPoints(i)[0].x;  // the root's x
        EXPECT_NEAR(strandColor(s, i)[0], x, 1e-3f);
        EXPECT_NEAR(strandColor(s, i)[1], 0.5f, 1e-5f);
    }
    EXPECT_NE(shown.note.find("g 0.5"), std::string::npos) << shown.note;
}

}  // namespace
}  // namespace groomlab
