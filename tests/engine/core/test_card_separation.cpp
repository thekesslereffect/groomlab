// SPDX-License-Identifier: Apache-2.0
#include <gtest/gtest.h>

#include <cmath>
#include <vector>

#include "groomlab/cards.h"
#include "groomlab/strand_set.h"
#include "test_helpers.h"

namespace groomlab {
namespace {

constexpr std::size_t kRef = 17;

// A straight card rising along +y from `root`, `width` wide across x, facing +z; `tilt` leans it
// toward +z by that much at the tip (a card through another's plane).
CardMesh::Card card(const Vec3f& root, float width, float tilt = 0.0f, int feather = -1) {
    CardMesh::Card c;
    c.stations = 9;
    c.feather = feather;
    for (std::size_t i = 0; i < kRef; ++i) {
        const float u = static_cast<float>(i) / static_cast<float>(kRef - 1);
        c.refCenter.push_back(root + Vec3f{0.0f, u, tilt * u});
        c.refSide.push_back({1.0f, 0.0f, 0.0f});
        c.refNormal.push_back(Vec3f{0.0f, -tilt, 1.0f}.normalized());
        c.refWidth.push_back(width);
    }
    return c;
}

// How far card b lies off card a's plane (+z side positive) at a reference station, at its middle.
float offAt(const CardMesh::Card& a, const CardMesh::Card& b, std::size_t i) {
    const Vec3f pa = a.refCenter[i] + (a.refShift.empty() ? Vec3f{} : a.refShift[i]);
    const Vec3f pb = b.refCenter[i] + (b.refShift.empty() ? Vec3f{} : b.refShift[i]);
    return (pb - pa).dot(a.refNormal[i]);
}

TEST(CardSeparation, CardsTooCloseMoveApartByTheGapRootsHeld) {
    // Two parallel cards 1% of their width apart; a 10% gap asked.
    std::vector<CardMesh::Card> cards{card({0, 0, 0}, 0.5f), card({0, 0, 0.005f}, 0.5f)};
    const std::vector<Vec3f> before = cards[1].refCenter;
    separateCards(cards, 0.1f, 0.0f, 8);
    const float gap = 0.1f * 0.5f;
    for (std::size_t i = kRef / 2; i < kRef; ++i) EXPECT_GE(offAt(cards[0], cards[1], i), 0.9f * gap) << i;
    for (const CardMesh::Card& c : cards) {
        ASSERT_EQ(c.refShift.size(), kRef);
        EXPECT_EQ(c.refShift.front().length(), 0.0f) << "the root holds";
        EXPECT_EQ(c.center.front().z, c.refCenter.front().z);
        EXPECT_GT(std::abs(c.center.back().z - c.refCenter.back().z), 0.0f) << "the stations carry the shift";
    }
    EXPECT_EQ(cards[1].refCenter, before) << "the texture's shape stays the strands'";
}

TEST(CardSeparation, ACardThroughAnotherGoesToTheSideItIsMostlyOn) {
    // b starts under a and passes up through it near the tip (crossing at 80% of its length).
    std::vector<CardMesh::Card> cards{card({0, 0, 0}, 0.5f), card({0, 0, -0.2f}, 0.5f, 0.25f)};
    EXPECT_GT(offAt(cards[0], cards[1], kRef - 1), 0.0f);  // above a at the tip, before
    separateCards(cards, 0.1f, 0.0f, 16);
    // Mostly under a: under it now from where the push may move it on, by the gap.
    for (std::size_t i = kRef / 2; i < kRef; ++i) EXPECT_LE(offAt(cards[0], cards[1], i), -0.9f * 0.05f) << i;
}

TEST(CardSeparation, ADownFeathersCrossAndAZeroGapAreLeftAlone) {
    std::vector<CardMesh::Card> cross{card({0, 0, 0}, 0.5f, 0.0f, 3), card({0, 0, 0.001f}, 0.5f, 0.0f, 3)};
    separateCards(cross, 0.0f, 0.2f, 8);
    EXPECT_TRUE(cross[0].refShift.empty());
    EXPECT_TRUE(cross[1].refShift.empty());
    std::vector<CardMesh::Card> hair{card({0, 0, 0}, 0.5f), card({0, 0, 0.001f}, 0.5f)};
    separateCards(hair, 0.0f, 0.2f, 8);  // feathers' gap only: hair cards keep their place
    EXPECT_TRUE(hair[0].refShift.empty());
    // A feather card and a hair card: the larger share, of the narrower card's width.
    std::vector<CardMesh::Card> mixed{card({0, 0, 0}, 0.5f), card({0, 0, 0.001f}, 0.25f, 0.0f, 1)};
    separateCards(mixed, 0.0f, 0.2f, 8);
    EXPECT_GE(offAt(mixed[0], mixed[1], kRef - 1), 0.9f * 0.2f * 0.25f);
}

TEST(CardSeparation, ACardBendsAsAWholeNeverKinked) {
    // A short card crossing the middle of a long one: the long one bends smoothly (along its own
    // normal, no station off the curve its neighbours make), not just where the short one is.
    CardMesh::Card shortCard = card({0, 0.4f, -0.02f}, 0.5f, 0.04f);
    for (std::size_t i = 0; i < kRef; ++i) shortCard.refCenter[i].y = 0.4f + 0.2f * static_cast<float>(i) / (kRef - 1);
    std::vector<CardMesh::Card> cards{card({0, 0, 0}, 0.5f), shortCard};
    separateCards(cards, 0.1f, 0.0f, 8);
    const std::vector<Vec3f>& shift = cards[0].refShift;
    ASSERT_EQ(shift.size(), kRef);
    float most = 0.0f, bend = 0.0f;
    for (std::size_t i = 0; i < kRef; ++i) {
        EXPECT_NEAR(shift[i].x, 0.0f, 1e-6f);  // along its normal (z) alone
        EXPECT_NEAR(shift[i].y, 0.0f, 1e-6f);
        most = std::max(most, std::abs(shift[i].z));
        if (i > 0 && i + 1 < kRef) bend = std::max(bend, std::abs(shift[i - 1].z - 2.0f * shift[i].z + shift[i + 1].z));
    }
    EXPECT_GT(most, 0.0f);
    EXPECT_LT(bend, 0.15f * most) << "no kink: each station on the curve its neighbours make";
}

TEST(CardSeparation, ACrowdedCardStaysNearItsStrands) {
    // Many cards stacked through one another (a dense coat): however many neighbours push, no
    // card moves more than half its width off where its strands are.
    std::vector<CardMesh::Card> cards;
    for (int k = 0; k < 16; ++k) {
        cards.push_back(card({0.01f * (k % 4), 0, 0.003f * k}, 0.4f, 0.05f * ((k % 5) - 2)));
    }
    separateCards(cards, 0.2f, 0.0f, 64);
    for (std::size_t c = 0; c < cards.size(); ++c) {
        for (const Vec3f& s : cards[c].refShift) EXPECT_LE(s.length(), 0.5f * 0.4f + 1e-5f) << c;
    }
}

TEST(CardSeparation, TheSameCardsComeOutTheSame) {
    auto make = [] {
        std::vector<CardMesh::Card> cards;
        for (int k = 0; k < 6; ++k) cards.push_back(card({0.07f * k, 0, 0.002f * (k % 3)}, 0.5f, 0.03f * (k % 2)));
        return cards;
    };
    std::vector<CardMesh::Card> a = make(), b = make();
    separateCards(a, 0.1f, 0.0f, 8);
    separateCards(b, 0.1f, 0.0f, 8);
    for (std::size_t c = 0; c < a.size(); ++c) EXPECT_EQ(a[c].refShift, b[c].refShift) << c;
}

TEST(CardSeparation, TheCardsNodesSeparationMovesTheMeshNotTheBake) {
    // Built cards: separation moves vertices; the bake's stamp (what the texture is made from)
    // stays the same whatever the separation.
    const auto scalp = test::makePlane(16);
    auto strands = std::make_shared<StrandSet>();
    std::vector<float> clump;
    for (int i = 0; i < 120; ++i) {
        const float x = 0.02f * static_cast<float>(i % 12), z = 0.02f * static_cast<float>(i / 12);
        std::vector<Vec3f> pts;
        for (int k = 0; k <= 8; ++k) pts.push_back({x + 0.01f * k, 0.1f * k, z});
        strands->addStrand(static_cast<StrandId>(i + 1), pts, std::vector<float>(pts.size(), 0.004f),
                           scalp->closestPoint(pts.front()).root);
        clump.push_back(static_cast<float>((i % 12) / 4 + 3 * ((i / 12) / 4)));
    }
    strands->addStrandChannel("clumpId", 0.0f) = clump;
    CardSettings settings;
    settings.cardsPerClump = 2;
    const CardMesh plain = buildCards(*strands, scalp.get(), settings);
    settings.separation = 0.1f;
    const CardMesh apart = buildCards(*strands, scalp.get(), settings);
    settings.separation = 0.2f;
    const CardMesh further = buildCards(*strands, scalp.get(), settings);
    ASSERT_EQ(plain.cards.size(), apart.cards.size());
    bool moved = false;
    for (std::size_t c = 0; c < plain.cards.size(); ++c) {
        EXPECT_EQ(plain.cards[c].refCenter, apart.cards[c].refCenter);
        moved = moved || !apart.cards[c].refShift.empty();
    }
    EXPECT_TRUE(moved);
    EXPECT_NE(plain.positions, apart.positions);
    // The bake's stamp: the same with no separation, some, or more (nothing to bake again).
    const auto stamp = [&](const CardMesh& m) { return summarizeCardNodes(m, strands.get()).front().bakeHash; };
    EXPECT_EQ(stamp(plain), stamp(apart));
    EXPECT_EQ(stamp(apart), stamp(further));
}

}  // namespace
}  // namespace groomlab
