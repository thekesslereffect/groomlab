// SPDX-License-Identifier: Apache-2.0
// Spectrum: an equalizer for the strands' shape. Each strand's way along it (its unit tangents, a
// segment's each) is split into three bands by the length of hair over which it swings (Gaussian
// smoothing along the strand, crossovers at lowWavelength and highWavelength, scene units along
// the hair): the low band, its overall sweep and broad waves (swings longer than lowWavelength);
// the high band, frizz and kinks (shorter than highWavelength); and between them the middle, curls
// and waves. A curl's way turns round once a turn of it, so its band is the length of hair a turn
// takes. Each band is scaled by its gain (0 removes it, 1 keeps it, more boosts it: the low band
// about the strand's overall way, root to tip), and the strand is walked again from its root
// along the ways that come out, each segment its own length: taking the frizz out of a strand
// lets it reach farther, as straightened hair does.
//
// So one node takes the frizz out and keeps the curls (high 0), tightens or relaxes waves (mid
// above or below 1), straightens the sweep without touching the texture (low below 1). strength
// (mappable: a mask) blends each strand from the input to the result, profile root to tip.

#include <algorithm>
#include <cmath>
#include <memory>
#include <vector>

#include "groomlab/field.h"
#include "groomlab/graph.h"
#include "groomlab/operators.h"
#include "groomlab/parallel.h"
#include "internal.h"

namespace groomlab {

namespace {

// `x` smoothed by a Gaussian of `sigma` samples, `before` and `after` standing in past its ends
// (what the strand is taken to go on as: a mirror there would read a curl's turning back as a lean
// of its root); sigma below about a third of a sample: as it is.
void gaussian(const std::vector<Vec3f>& x, float sigma, std::vector<Vec3f>& out, const Vec3f& before, const Vec3f& after) {
    const std::size_t n = x.size();
    out = x;
    if (n < 2 || !(sigma > 0.3f)) return;
    const int last = static_cast<int>(n) - 1;
    const int radius = std::max(1, static_cast<int>(std::ceil(3.0f * sigma)));
    std::vector<float> weights(static_cast<std::size_t>(radius) + 1);
    const float inv = -0.5f / (sigma * sigma);
    for (int k = 0; k <= radius; ++k) weights[static_cast<std::size_t>(k)] = std::exp(static_cast<float>(k * k) * inv);
    auto at = [&](int i) { return i < 0 ? before : i > last ? after : x[static_cast<std::size_t>(i)]; };
    for (int i = 0; i <= last; ++i) {
        Vec3f sum = x[static_cast<std::size_t>(i)] * weights[0];
        float total = weights[0];
        for (int k = 1; k <= radius; ++k) {
            const float w = weights[static_cast<std::size_t>(k)];
            sum = sum + (at(i - k) + at(i + k)) * w;
            total += 2.0f * w;
        }
        out[static_cast<std::size_t>(i)] = sum * (1.0f / total);
    }
}

class SpectrumOp : public Operator {
public:
    std::string typeName() const override { return op::kSpectrum; }
    std::string category() const override { return "look"; }
    std::vector<PortSpec> inputs() const override { return {{"strands", PortType::Curves}}; }
    PortSpec output() const override { return {"out", PortType::Curves}; }
    std::vector<std::string> externalInputs() const override { return {"scalp"}; }

    std::vector<ParamSpec> paramSpecs() const override {
        return {
            ParamSpec{"strength", 1.0, "How much of the result each strand takes (mappable: a mask; 0 leaves it as it comes)",
                      "fraction"}
                .range(0, 1),
            {"profile", Ramp::constant(1.0f), "The strength along the length, root to tip"},
            ParamSpec{"lowGain", 1.0, "The low band: the overall sweep and broad waves, swings longer than lowWavelength "
                                      "(0 straightens the strand toward its end, 1 keeps them, more deepens them)", ""}
                .range(0, 4)
                .soft(0, 2)
                .in("Bands"),
            ParamSpec{"midGain", 1.0, "The middle band: curls and waves, swings between highWavelength and lowWavelength "
                                      "(0 relaxes them out, 1 keeps them, more tightens them)", ""}
                .range(0, 4)
                .soft(0, 2)
                .in("Bands"),
            ParamSpec{"highGain", 0.0, "The high band: frizz and kinks, swings shorter than highWavelength (0 removes them, "
                                       "1 keeps them, more roughens)", ""}
                .range(0, 4)
                .soft(0, 2)
                .in("Bands"),
            ParamSpec{"lowWavelength", 6.0, "Where the low band ends: the length of hair a swing takes, this long or longer "
                                            "is low (scene units)", "length"}
                .range(0.001, 1000)
                .soft(0.5, 20)
                .in("Bands"),
            ParamSpec{"highWavelength", 1.0, "Where the high band starts: the length of hair a swing takes, this short or "
                                             "shorter is high (scene units)", "length"}
                .range(0.001, 1000)
                .soft(0.05, 5)
                .in("Bands"),
        };
    }

    Value evaluate(const EvalContext& ctx, const std::vector<Value>& inputs) const override {
        const StrandsPtr in = requireStrands(inputs, op::kSpectrum);
        const ScalpMesh* scalp = scalpOf(ctx);
        const ScalarField strength(params(), "strength", scalp);
        strength.validate(*in);
        const Ramp profile = params().getRamp("profile");
        const auto lowGain = static_cast<float>(params().getDouble("lowGain"));
        const auto midGain = static_cast<float>(params().getDouble("midGain"));
        const auto highGain = static_cast<float>(params().getDouble("highGain"));
        // (Lengths: the session scales them with a groom scaled with its scalp.)
        const auto lowWave = static_cast<float>(params().getDouble("lowWavelength"));
        const float highWave = std::min(static_cast<float>(params().getDouble("highWavelength")), lowWave);  // (crossed: no middle)
        if (lowGain == 1.0f && midGain == 1.0f && highGain == 1.0f) return in;  // every band kept: the strands as they come

        auto out = std::make_shared<StrandSet>(*in);
        out->mutablePositions();
        const RampAlong along(profile, *out);
        // A Gaussian halves a swing of wavelength L where its sigma is L sqrt(ln 2 / 2) / pi.
        const float toSigma = std::sqrt(std::log(2.0f) / 2.0f) / 3.14159265f;
        parallelFor(0, out->strandCount(), [&](std::size_t s) {
            const std::size_t n = out->strandPointCount(s);
            if (n < 3) return;
            const float amount = std::clamp(strength.at(*in, s), 0.0f, 1.0f);
            if (!(amount > 0.0f)) return;
            Vec3f* p = out->strandPoints(s);
            // Each segment's length and way, and the strand's overall way (its chord, root to tip).
            const std::size_t segs = n - 1;
            std::vector<float> len(segs);
            std::vector<Vec3f> way(segs);
            Vec3f overall{};
            float total = 0.0f;
            for (std::size_t i = 0; i < segs; ++i) {
                const Vec3f d = p[i + 1] - p[i];
                len[i] = d.length();
                if (!(len[i] > 0.0f)) return;  // a point on its neighbour: as it comes
                way[i] = d * (1.0f / len[i]);
                overall = overall + d;
                total += len[i];
            }
            const float overallLength = overall.length();
            overall = overallLength > 0.0f ? overall * (1.0f / overallLength) : way[0];
            // The bands, by the strand's mean segment: low (smoothed over lowWavelength) and low +
            // middle (over highWavelength); the high band what is left.
            const float step = total / static_cast<float>(segs);
            std::vector<Vec3f> low, smooth;
            // (The low band goes on past the ends as the strand's overall way; low + middle as the
            // low band's ends, so taking the frizz out keeps a root's own lean.)
            gaussian(way, toSigma * lowWave / step, low, overall, overall);
            gaussian(way, toSigma * highWave / step, smooth, low.front(), low.back());
            // Walked again from the root, each segment its own length.
            Vec3f at = p[0];
            for (std::size_t i = 0; i < segs; ++i) {
                const Vec3f result = overall + (low[i] - overall) * lowGain + (smooth[i] - low[i]) * midGain + (way[i] - smooth[i]) * highGain;
                // (Blended by strength x profile at the segment's middle.)
                const float w = amount * 0.5f * (along(i, n) + along(i + 1, n));
                const Vec3f blended = way[i] + (result - way[i]) * w;
                const float bl = blended.length();
                at = at + (bl > 1e-12f ? blended * (1.0f / bl) : way[i]) * len[i];
                p[i + 1] = at;
            }
        });
        return StrandsPtr(out);
    }
};

}  // namespace

void registerSpectrumOperator(OperatorRegistry& registry) {
    registry.registerType(op::kSpectrum,
                          {"look",
                           "An equalizer for the strands' shape: frizz, curls and the overall sweep as bands, each kept, "
                           "removed or boosted.",
                           "frequency equalizer frizz smooth curl wave band filter"},
                          [] { return std::make_unique<SpectrumOp>(); });
}

}  // namespace groomlab
