// SPDX-License-Identifier: Apache-2.0
// The groom editor's game exports (engine/editor game_exports.h), every host's options: the base
// name with its LOD, the maps' size kept in range, nothing written without cards.

#include <gtest/gtest.h>

#include <stdexcept>

#include "groomlab/editor/game_exports.h"

using namespace groomlab;

TEST(EditorGameExports, OneSetOfOptions) {
    GameExportOptions o;
    EXPECT_EQ(gameExportName(o), "groom");
    o.name = "hair";
    o.lod = "low";
    EXPECT_EQ(gameExportName(o), "hair_low") << "a LOD keeps apart in one folder";
    o.nameGiven = true;
    EXPECT_EQ(gameExportName(o), "hair") << "a name given is the name";
    EXPECT_EQ(gameExportResolution(0), 0) << "0: the Cards nodes' own";
    EXPECT_EQ(gameExportResolution(1), kMinBakeResolution);
    EXPECT_EQ(gameExportResolution(1 << 20), kMaxCardBakeResolution);
    EXPECT_THROW(writeEngineCards(CardBake{}, GroomStats{}, HairLook{}, nullptr, nullptr, "", false, o), std::runtime_error);
}
