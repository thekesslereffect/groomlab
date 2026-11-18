// SPDX-License-Identifier: Apache-2.0
#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include "groomlab/graph_pins.h"

namespace groomlab {
namespace {

using json = nlohmann::json;

// scalp feeds scatter and interpolate; interpolate feeds length, length feeds width.
json graph() {
    return json::parse(R"({"nodes": [{"name": "scalp", "type": "Scalp"}, {"name": "scatter", "type": "Scatter"},
        {"name": "interpolate", "type": "Interpolate"}, {"name": "length", "type": "Length"}, {"name": "width", "type": "Width"}],
        "connections": [{"from": "scalp", "to": "scatter", "input": "scalp"}, {"from": "scalp", "to": "interpolate", "input": "scalp"},
                        {"from": "interpolate", "to": "length", "input": "strands"}, {"from": "length", "to": "width", "input": "strands"}]})");
}

TEST(GraphPins, RoutesRunThroughPinsSourceFirst) {
    json g = graph();
    const std::string a = addPin(g, "width", "strands", 10, 20);
    const std::string b = addPin(g, "width", "strands", 30, 40, 0);
    EXPECT_EQ(a, "pin1");
    EXPECT_EQ(b, "pin2");
    EXPECT_EQ(pinRoute(g, "width", "strands"), (std::vector<std::string>{b, a}));
    EXPECT_EQ(pinRouteTo(g, a), (std::vector<std::string>{b, a}));
    EXPECT_EQ(pinRouteTo(g, b), (std::vector<std::string>{b}));
    double x = 0, y = 0;
    ASSERT_TRUE(pinPosition(g, a, &x, &y));
    EXPECT_DOUBLE_EQ(x, 10);
    EXPECT_TRUE(movePin(g, a, 5, 6));
    ASSERT_TRUE(pinPosition(g, a, &x, &y));
    EXPECT_DOUBLE_EQ(y, 6);
    EXPECT_EQ(pinSource(g, a), (PinSource{"length", ""}));
    EXPECT_EQ(addPin(g, "scalp", "mesh", 0, 0), "");  // nothing feeds it
    // Stored in order, the way Maya's Graph Editor writes it.
    EXPECT_EQ(g["pins"]["points"][0]["id"], "pin1");
    EXPECT_EQ(g["pins"]["routes"][0]["via"], json({"pin2", "pin1"}));
}

TEST(GraphPins, PinsOfOneSignalJoinIntoABundle) {
    json g = graph();
    const std::string a = addPin(g, "scatter", "scalp", 0, 0);
    const std::string b = addPin(g, "interpolate", "scalp", 1, 1);
    const std::string c = addPin(g, "width", "strands", 2, 2);
    EXPECT_FALSE(mergePins(g, a, c));  // another signal
    EXPECT_TRUE(mergePins(g, a, b));
    EXPECT_EQ(pinIds(g), (std::vector<std::string>{a, c}));
    const std::vector<PinLink> links = pinLinks(g, a);
    ASSERT_EQ(links.size(), 2u);
    EXPECT_EQ(links[0].to, "interpolate");
    EXPECT_EQ(links[1].to, "scatter");
    EXPECT_TRUE(removePin(g, a));
    EXPECT_EQ(pinRoute(g, "scatter", "scalp"), std::vector<std::string>{});
    EXPECT_EQ(pinIds(g), (std::vector<std::string>{c}));
}

TEST(GraphPins, RoutesFollowTheLinks) {
    json g = graph();
    const std::string a = addPin(g, "width", "strands", 0, 0);
    // A rename follows (the connections renamed first, as GraphEdit does).
    for (json& c : g["connections"]) {
        if (c["to"] == "width") c["to"] = "thick";
    }
    renamePinNode(g, "width", "thick");
    EXPECT_EQ(pinRoute(g, "thick", "strands"), (std::vector<std::string>{a}));
    // A link from another source now: its route goes, and the pin with it.
    for (json& c : g["connections"]) {
        if (c["to"] == "thick") c["from"] = "interpolate";
    }
    prunePins(g);
    EXPECT_FALSE(g.contains("pins"));
    // Set by hand: a link drawn out of a pin runs through the pins up to it.
    const std::string b = addPin(g, "length", "strands", 0, 0);
    EXPECT_TRUE(setPinRoute(g, "thick", "strands", pinRouteTo(g, b)));
    EXPECT_EQ(pinRoute(g, "thick", "strands"), (std::vector<std::string>{b}));
    EXPECT_EQ(pinLinks(g, b).size(), 2u);
    EXPECT_FALSE(setPinRoute(g, "scalp", "mesh", {b}));
}

TEST(GraphPins, OneFeedKeepsOnlyRoutesAPinWithOneInputDraws) {
    // Two links from scalp: interpolate's through a then b, scatter's through b alone, so b is fed
    // from a in one route and from scalp in the other. Every host but one whose pin has one input
    // (Blender's reroute) draws both.
    json g = graph();
    const std::string a = addPin(g, "interpolate", "scalp", 0, 0);
    const std::string b = addPin(g, "interpolate", "scalp", 1, 1);
    ASSERT_TRUE(setPinRoute(g, "scatter", "scalp", {b}));
    json all = g;
    prunePins(all);
    EXPECT_EQ(all["pins"]["routes"].size(), 2u);
    prunePins(g, true);  // by (to, input): interpolate's first, scatter's reaches b from elsewhere
    ASSERT_EQ(g["pins"]["routes"].size(), 1u);
    EXPECT_EQ(g["pins"]["routes"][0]["to"], "interpolate");
    EXPECT_EQ(pinRoute(g, "scatter", "scalp"), std::vector<std::string>{});
    // A bundle (the same pins from the source on) is one feed each: both kept.
    ASSERT_TRUE(setPinRoute(g, "scatter", "scalp", {a, b}));
    prunePins(g, true);
    EXPECT_EQ(g["pins"]["routes"].size(), 2u);
    // A route passing a pin twice is none a one-input pin draws.
    ASSERT_TRUE(setPinRoute(g, "width", "strands", {addPin(g, "width", "strands", 2, 2)}));
    const std::string c = pinRoute(g, "width", "strands").front();
    ASSERT_TRUE(setPinRoute(g, "width", "strands", {c, c}));
    prunePins(g, true);
    EXPECT_EQ(pinRoute(g, "width", "strands"), std::vector<std::string>{});
    EXPECT_EQ(pinIds(g), (std::vector<std::string>{a, b}));
}

TEST(GraphPins, PinsWrittenByHandAreReadAsFarAsTheyHold) {
    // A coordinate that is no number: no pin (its route runs through the others); an id past any
    // number sorts as a name does; nothing throws.
    json g = graph();
    g["pins"] = json::parse(R"({"points": [{"id": "pin2", "x": 1, "y": 2}, {"id": "pin1", "x": "left", "y": 0}, {"id": "pin3"},
                                           {"id": "pin99999999999999999999", "x": 0, "y": 0}, {"id": 7}, "pin"],
                                "routes": [{"from": "length", "to": "width", "input": "strands",
                                            "via": ["pin1", "pin99999999999999999999", "pin3", "pin2", 5]},
                                           {"from": "length", "to": "width"}, 3]})");
    EXPECT_EQ(pinRoute(g, "width", "strands"), (std::vector<std::string>{"pin99999999999999999999", "pin3", "pin2"}));
    prunePins(g);
    EXPECT_EQ(pinIds(g), (std::vector<std::string>{"pin99999999999999999999", "pin2", "pin3"}));
    double x = 1, y = 1;
    ASSERT_TRUE(pinPosition(g, "pin3", &x, &y));
    EXPECT_DOUBLE_EQ(x, 0);
    EXPECT_EQ(addPin(g, "width", "strands", 0, 0), "pin4");
}

}  // namespace
}  // namespace groomlab
