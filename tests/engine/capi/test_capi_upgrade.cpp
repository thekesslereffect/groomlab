// SPDX-License-Identifier: Apache-2.0
// Node upgrades through the C++ wrapper on a node that is outdated: the C API is linked in
// statically (GROOMLAB_C_STATIC) beside capi_versioned_op.cpp's TestCApiLift, an operator at
// version 2, since no built-in operator has a second version yet.

#include <gtest/gtest.h>

#include <cmath>
#include <filesystem>
#include <string>

#include <nlohmann/json.hpp>

#include "groomlab/groomlab.hpp"

namespace {

namespace fs = std::filesystem;
namespace api = groomlab::api;
using nlohmann::json;

const std::string kCompatGroom = (fs::u8path(GROOMLAB_TEST_DATA) / "compat" / "v1" / "groom.groom").u8string();

// The groom's graph with a TestCApiLift node, saved at version 1 (before its "lift" param),
// between the groom and its output.
std::string graphWithOldLift(const std::string& graphJson) {
    json graph = json::parse(graphJson);
    graph["nodes"].push_back({{"name", "lift"}, {"type", "TestCApiLift"}, {"version", 1}, {"params", json::object()}});
    for (json& c : graph["connections"]) {
        if (c.at("to") == "output") c["to"] = "lift";
    }
    graph["connections"].push_back({{"from", "lift"}, {"to", "output"}, {"input", "strands"}});
    return graph.dump();
}

json liftNode(const api::Session& session) {
    const json graph = json::parse(session.graphJson());
    for (const json& n : graph.at("nodes")) {
        if (n.at("name") == "lift") return n;
    }
    return nullptr;
}

TEST(CApiUpgrade, AnOutdatedNodeTakesTheNewestBehaviour) {
    api::Session session = api::Session::fromFile(kCompatGroom);
    session.setGraphJson(graphWithOldLift(session.graphJson()));
    const api::Strands old = session.evaluate();
    ASSERT_GT(old.pointCount(), 0u);
    json spec = json::parse(session.nodeSpecJson("lift"));
    EXPECT_EQ(spec.at("nodeVersion"), 1);
    EXPECT_EQ(spec.at("version"), 2);
    EXPECT_TRUE(spec.at("outdated").get<bool>());
    EXPECT_FALSE(liftNode(session).at("params").contains("lift"));

    EXPECT_TRUE(session.upgradeNode("lift"));
    spec = json::parse(session.nodeSpecJson("lift"));
    EXPECT_EQ(spec.at("nodeVersion"), 2);
    EXPECT_FALSE(spec.at("outdated").get<bool>());
    const json node = liftNode(session);
    EXPECT_EQ(node.at("version"), 2);
    EXPECT_EQ(node.at("params").at("lift"), 1.0);  // the param version 2 added, at its default
    EXPECT_EQ(session.getFloat("lift", "lift"), 1.0);

    // Version 2's behaviour: the same strands, 1 higher.
    const api::Strands raised = session.evaluate();
    ASSERT_EQ(raised.pointCount(), old.pointCount());
    for (std::size_t i = 0; i < old.pointCount(); ++i) {
        ASSERT_EQ(raised.positions()[3 * i], old.positions()[3 * i]) << i;
        ASSERT_NEAR(raised.positions()[3 * i + 1], old.positions()[3 * i + 1] + 1.0f, 1e-5f) << i;
        ASSERT_EQ(raised.positions()[3 * i + 2], old.positions()[3 * i + 2]) << i;
    }

    // Once: the node has the newest now. A saved groom keeps it.
    const std::string graph = session.graphJson();
    EXPECT_FALSE(session.upgradeNode("lift"));
    EXPECT_EQ(session.graphJson(), graph);
    api::Session reloaded = api::Session::fromMemory(session.saveMemory());
    EXPECT_EQ(json::parse(reloaded.nodeSpecJson("lift")).at("nodeVersion"), 2);
}

TEST(CApiUpgrade, AFailedUpgradeLeavesTheGraph) {
    api::Session session = api::Session::fromFile(kCompatGroom);
    session.setGraphJson(graphWithOldLift(session.graphJson()));
    const std::string graph = session.graphJson();
    EXPECT_THROW(session.upgradeNode("nobody"), api::Error);
    EXPECT_FALSE(session.upgradeNode("scatter"));  // current already
    EXPECT_EQ(session.graphJson(), graph);
}

}  // namespace
