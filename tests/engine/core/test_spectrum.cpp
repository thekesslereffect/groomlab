// SPDX-License-Identifier: Apache-2.0
// Spectrum: the strands' bending as bands by wavelength, each scaled. Helices of a known pitch put
// their bending in one band: taking the band out straightens them, boosting it tightens them, and
// every result keeps its root and its segments' lengths.

#include <gtest/gtest.h>

#include <cmath>
#include <functional>
#include <memory>
#include <vector>

#include "groomlab/graph.h"
#include "groomlab/operators.h"
#include "groomlab/strand_set.h"

namespace groomlab {
namespace {

// Strands along +y, each a helix of `radius` and `pitch` (its rise a turn) about its own axis,
// `points` points over `length`; strand i's axis at x = 3i.
StrandsPtr helices(int count, float radius, float pitch, float length = 12.0f, int points = 97) {
    auto s = std::make_shared<StrandSet>();
    for (int i = 0; i < count; ++i) {
        std::vector<Vec3f> p;
        for (int k = 0; k < points; ++k) {
            const float y = length * static_cast<float>(k) / static_cast<float>(points - 1);
            const float a = 6.2831853f * y / pitch;
            p.push_back({3.0f * static_cast<float>(i) + radius * (std::cos(a) - 1.0f), y, radius * std::sin(a)});
        }
        s->addStrand(static_cast<StrandId>(i + 1), p);
    }
    return s;
}

StrandsPtr spectrum(const StrandsPtr& in, const std::function<void(ParamSet&)>& set) {
    auto node = OperatorRegistry::instance().create(op::kSpectrum);
    set(node->params());
    ExternalInputs ext;
    EvalContext ctx;
    ctx.externals = &ext;
    return valueAs<StrandsPtr>(node->evaluate(ctx, {in}));
}

// How far strand s's points stand off the line through its root and tip, at most.
float offLine(const StrandSet& s, std::size_t strand) {
    const Vec3f* p = s.strandPoints(strand);
    const std::size_t n = s.strandPointCount(strand);
    const Vec3f a = p[0], d = (p[n - 1] - p[0]).normalized();
    float most = 0.0f;
    for (std::size_t k = 0; k < n; ++k) {
        const Vec3f r = p[k] - a;
        most = std::max(most, (r - d * r.dot(d)).length());
    }
    return most;
}

void expectRootsAndLengthsKept(const StrandSet& in, const StrandSet& out) {
    ASSERT_EQ(in.pointCount(), out.pointCount());
    for (std::size_t s = 0; s < in.strandCount(); ++s) {
        const Vec3f* a = in.strandPoints(s);
        const Vec3f* b = out.strandPoints(s);
        EXPECT_EQ(a[0], b[0]) << "strand " << s << ": the root stays";
        for (std::size_t k = 1; k < in.strandPointCount(s); ++k) {
            EXPECT_NEAR((a[k] - a[k - 1]).length(), (b[k] - b[k - 1]).length(), 1e-4f) << s << " " << k;
        }
    }
}

TEST(Spectrum, EveryBandKeptGivesTheStrandsBack) {
    const StrandsPtr in = helices(3, 0.4f, 2.0f);
    const StrandsPtr out = spectrum(in, [](ParamSet& p) { p.set("highGain", 1.0); });
    EXPECT_EQ(out, in) << "gains of 1: the very strands";
    // Gains that change nothing a strand has (no bending in the high band here) and strength 0.
    const StrandsPtr none = spectrum(in, [](ParamSet& p) {
        p.set("midGain", 3.0);
        p.set("strength", 0.0);
    });
    EXPECT_EQ(none->positions(), in->positions());
}

TEST(Spectrum, TakingTheHighBandOutStraightensFrizz) {
    // A tight helix (pitch 0.3: about 0.6 of hair a turn, well under highWavelength 2): the high band.
    const StrandsPtr frizz = helices(4, 0.08f, 0.3f);
    ASSERT_GT(offLine(*frizz, 0), 0.1f);
    const StrandsPtr out = spectrum(frizz, [](ParamSet& p) {
        p.set("highGain", 0.0);
        p.set("highWavelength", 2.0);
    });
    expectRootsAndLengthsKept(*frizz, *out);
    for (std::size_t s = 0; s < out->strandCount(); ++s) {
        // Straight within a percent of its reach, reaching nearly as far as it is long (its 12
        // along the helix's axis before): straightened, it lengthens.
        float hair = 0.0f;
        for (std::size_t k = 1; k < 97; ++k) hair += (frizz->strandPoints(s)[k] - frizz->strandPoints(s)[k - 1]).length();
        const float reach = (out->strandPoints(s)[96] - out->strandPoints(s)[0]).length();
        EXPECT_LT(offLine(*out, s), 0.01f * reach) << "strand " << s << " straightened";
        EXPECT_GT(reach, 0.98f * hair) << s;
        EXPECT_GT(reach, 1.5f * (frizz->strandPoints(s)[96] - frizz->strandPoints(s)[0]).length()) << s;
    }
}

TEST(Spectrum, TheMiddleBandTightensOrRelaxesCurls) {
    // A curl of pitch 2 (about 3.7 of hair a turn): between highWavelength 0.6 and lowWavelength 12,
    // the middle band.
    const StrandsPtr curls = helices(2, 0.5f, 2.0f);
    auto with = [&](double mid) {
        return spectrum(curls, [mid](ParamSet& p) {
            p.set("midGain", mid);
            p.set("highGain", 1.0);
            p.set("highWavelength", 0.6);
            p.set("lowWavelength", 12.0);
        });
    };
    const StrandsPtr tighter = with(2.0), relaxed = with(0.0);
    expectRootsAndLengthsKept(*curls, *tighter);
    expectRootsAndLengthsKept(*curls, *relaxed);
    for (std::size_t s = 0; s < curls->strandCount(); ++s) {
        // Deeper bending: the curl winds more and the strand reaches less far; none: it reaches out.
        const float reach = (curls->strandPoints(s)[96] - curls->strandPoints(s)[0]).length();
        EXPECT_LT((tighter->strandPoints(s)[96] - tighter->strandPoints(s)[0]).length(), reach) << s;
        EXPECT_GT((relaxed->strandPoints(s)[96] - relaxed->strandPoints(s)[0]).length(), reach) << s;
        EXPECT_LT(offLine(*relaxed, s), 0.5f * offLine(*curls, s)) << s << ": relaxed nearly straight";
    }
}

TEST(Spectrum, StrengthBlendsTowardTheResult) {
    const StrandsPtr frizz = helices(1, 0.08f, 0.3f);
    auto off = [&](double strength) {
        return offLine(*spectrum(frizz, [strength](ParamSet& p) {
            p.set("highGain", 0.0);
            p.set("strength", strength);
        }), 0);
    };
    const float full = off(1.0), half = off(0.5), none = off(0.0);
    EXPECT_FLOAT_EQ(none, offLine(*frizz, 0));
    EXPECT_GT(half, full);
    EXPECT_LT(half, none);
}

}  // namespace
}  // namespace groomlab
