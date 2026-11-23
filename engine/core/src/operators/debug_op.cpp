// SPDX-License-Identifier: Apache-2.0
// Debug: shows a value on the hair, to see what a channel, a field or an expression holds. It colors
// the strands (the colorR/G/B channels every host draws and every renderer reads, as HairColor's)
// by a strand or point channel of the strands (clumpId, tubeId, guideInfluence...), by a number
// (a field node or an expression plugged into or typed in `value`) or by a color (a vector
// expression or a Map's rgb plugged into `color`), and says in its note what it found: the values'
// range, how many there are, per strand or per point. A number also goes to the channel "debug",
// which a viewport's Color by Channel shows unlit.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "groomlab/display.h"
#include "groomlab/export_view.h"
#include "groomlab/field.h"
#include "groomlab/graph.h"
#include "groomlab/operators.h"
#include "groomlab/parallel.h"
#include "internal.h"

namespace groomlab {

namespace {

using Rgb = std::array<float, 3>;

constexpr const char* kDebugChannel = "debug";

std::string number(double v) {
    char text[32];
    std::snprintf(text, sizeof(text), "%.4g", v);
    return text;
}

// The strands' channels by name, for a note: "clumpId, tubeId (strand); colorR (point)".
std::string channelList(const StrandSet& strands) {
    auto join = [](const std::vector<std::string>& names) {
        std::string out;
        for (const std::string& n : names) {
            if (n.empty() || n[0] == '_') continue;  // a node's own (a preview's roles)
            out += (out.empty() ? "" : ", ") + n;
        }
        return out;
    };
    const std::string strand = join(strands.strandChannelNames()), point = join(strands.pointChannelNames());
    std::string out;
    if (!strand.empty()) out += strand + " (per strand)";
    if (!point.empty()) out += (out.empty() ? "" : "; ") + point + " (per point)";
    return out.empty() ? "none" : out;
}

// What a run of numbers holds, for the note: "0 to 11, 12 values, whole numbers, per strand".
std::string summary(const std::vector<float>& values, bool perPoint) {
    float lo = 0.0f, hi = 0.0f;
    std::size_t bad = 0;
    bool whole = true;
    std::set<float> distinct;
    for (const float v : values) {
        if (!std::isfinite(v)) {
            ++bad;
            continue;
        }
        whole = whole && v == std::floor(v);
        if (distinct.size() <= 1000) distinct.insert(v);
    }
    std::string out;
    if (channelRange(values, &lo, &hi)) {
        out = lo == hi ? number(lo) + " everywhere" : number(lo) + " to " + number(hi);
        out += distinct.size() > 1000 ? ", over 1000 values" : ", " + std::to_string(distinct.size()) + (distinct.size() == 1 ? " value" : " values");
        if (whole && lo != hi) out += ", whole numbers";
    } else {
        out = "no finite value";
    }
    if (bad > 0) out += "; " + std::to_string(bad) + " not finite (magenta)";
    out += perPoint ? ", per point" : ", per strand";
    return out;
}

class DebugOp : public Operator {
public:
    std::string typeName() const override { return op::kDebug; }
    std::string category() const override { return "utility"; }
    std::vector<PortSpec> inputs() const override { return {{"strands", PortType::Curves}}; }
    PortSpec output() const override { return {"out", PortType::Curves}; }
    std::vector<std::string> externalInputs() const override { return {"scalp"}; }

    std::vector<ParamSpec> paramSpecs() const override {
        return {
            ParamSpec{"show", std::string("channel"),
                      "What it shows: a channel of the strands by name; a number (a field or an expression plugged "
                      "into or typed in value); or a color (a vector expression or a Map's rgb plugged into color)",
                      "",
                      {"channel", "value", "color"}},
            ParamSpec{"channel", std::string(),
                      "The strand or point channel shown (clumpId, tubeId, guideInfluence, braidId...); empty: the "
                      "note lists the strands' channels"}
                .readsChannel(true)
                .when("show", {"channel"}),
            ParamSpec{"value", 1.0,
                      "The number shown on each strand (mappable): plug an Expression, Map or other field node in, or "
                      "type an expression (e.g. $length / $longest)"}
                .soft(0, 1)
                .when("show", {"value"}),
            ParamSpec{"color", Vec3f(1.0f, 1.0f, 1.0f),
                      "The color shown, times a color plugged in: a vector expression ([$u, $v, 0], "
                      "ccellnoise($clumpId)) or a Map's rgb",
                      "color"}
                .colorMap()
                .when("show", {"color"}),
            ParamSpec{"colors", std::string("ramp"),
                      "How a number shows: ramp (blue, green, yellow, red from min to max), gray (black to white) or "
                      "random (a color of its own for each whole number: IDs, clumps, tubes)",
                      "",
                      {"ramp", "gray", "random"}}
                .when("show", {"channel", "value"}),
            ParamSpec{"range", std::string("auto"),
                      "auto: from the values' own lowest to highest; set: from min to max", "", {"auto", "set"}}
                .when("show", {"channel", "value"}),
            ParamSpec{"min", 0.0, "The value shown at the ramp's start (blue; black in gray)"}.soft(0, 1).when("range", {"set"}),
            ParamSpec{"max", 1.0, "The value shown at the ramp's end (red; white in gray)"}.soft(0, 1).when("range", {"set"}),
        };
    }

    Value evaluate(const EvalContext& ctx, const std::vector<Value>& inputs) const override {
        const StrandsPtr in = requireStrands(inputs, op::kDebug);
        const ScalpMesh* scalp = scalpOf(ctx);
        const std::string show = params().getString("show");
        auto out = std::make_shared<StrandSet>(*in);
        const std::size_t strands = out->strandCount(), points = out->pointCount();

        std::array<std::vector<float>*, 3> strandColor{}, pointColor{};
        auto colorChannels = [&](bool perPoint) {
            for (int k = 0; k < 3; ++k) {
                strandColor[k] = &out->addStrandChannel(kColorChannels[k], 0.0f);
                if (perPoint) {
                    pointColor[k] = &out->addPointChannel(kColorChannels[k], 0.0f);
                } else {
                    out->removePointChannel(kColorChannels[k]);  // an earlier color node's would show over these
                    pointColor[k] = nullptr;
                }
            }
            out->removePointChannel(kIridescenceChannel);
        };

        if (show == "color") {
            const ColorField color(params(), "color");
            color.validate(*in);
            const bool along = color.perPoint();
            colorChannels(along);
            parallelFor(0, strands, [&](std::size_t s) {
                const Rgb c = color.at(*in, s);
                for (int k = 0; k < 3; ++k) (*strandColor[k])[s] = c[k];
                if (!along) return;
                const std::size_t begin = out->strandBegin(s);
                for (std::size_t i = 0; i < out->strandPointCount(s); ++i) {
                    const Rgb ci = color.atPoint(*in, s, i);
                    for (int k = 0; k < 3; ++k) (*pointColor[k])[begin + i] = ci[k];
                }
            });
            const char* names[3] = {"r", "g", "b"};
            std::string note = "color:";
            for (int k = 0; k < 3; ++k) {
                float lo = 0.0f, hi = 0.0f;
                const std::vector<float>& values = along ? *pointColor[k] : *strandColor[k];
                note += std::string(k ? "; " : " ") + names[k] + " " +
                        (channelRange(values, &lo, &hi) ? (lo == hi ? number(lo) : number(lo) + " to " + number(hi)) : "-");
            }
            note += along ? ", per point" : ", per strand";
            ctx.note(note);
            return StrandsPtr(out);
        }

        // A number per strand (and per point when it varies along them), shown by `colors` over a range.
        const std::string colors = params().getString("colors");
        const bool rangeSet = params().getString("range") == "set";
        float lo = static_cast<float>(params().getDouble("min")), hi = static_cast<float>(params().getDouble("max"));
        std::vector<float> strandValues(strands, 0.0f), pointValues;
        bool along = false;
        std::string what;
        if (show == "channel") {
            const std::string name = params().getString("channel");
            if (name.empty()) {
                ctx.note("name a channel to show; the strands carry: " + channelList(*in));
                return in;
            }
            const std::vector<float>* pointChannel = in->pointChannel(name);
            const std::vector<float>* strandChannel = in->strandChannel(name);
            if (!pointChannel && !strandChannel) {
                throw GraphError("Debug: the strands carry no channel '" + name + "' (they carry: " + channelList(*in) + ")");
            }
            what = name;
            if (pointChannel) {  // read over a strand channel of the name, as Color by Channel does
                along = true;
                pointValues = *pointChannel;
                for (std::size_t s = 0; s < strands; ++s) {
                    strandValues[s] = out->strandPointCount(s) > 0 ? pointValues[out->strandBegin(s)] : 0.0f;
                }
            } else {
                strandValues = *strandChannel;
            }
        } else if (show == "value") {
            const ScalarField value(params(), "value", scalp);
            value.validate(*in);
            along = value.perPoint();
            if (along) pointValues.assign(points, 0.0f);
            parallelFor(0, strands, [&](std::size_t s) {
                strandValues[s] = value.at(*in, s);
                if (!along) return;
                const std::size_t begin = out->strandBegin(s);
                for (std::size_t i = 0; i < out->strandPointCount(s); ++i) pointValues[begin + i] = value.atPoint(*in, s, i);
            });
            what = "value";
        } else {
            throw GraphError("Debug: show is channel, value or color");
        }

        const std::vector<float>& shown = along ? pointValues : strandValues;
        if (!rangeSet && !channelRange(shown, &lo, &hi)) lo = 0.0f, hi = 1.0f;
        auto colorOf = [&](float v) -> Rgb {
            if (!std::isfinite(v)) return {1.0f, 0.0f, 1.0f};
            if (colors == "random") return clumpColor(static_cast<std::int64_t>(std::floor(v + 0.5f)));
            const float t = std::clamp(hi != lo ? (v - lo) / (hi - lo) : 0.5f, 0.0f, 1.0f);
            if (colors == "gray") return {t, t, t};
            return rampColor(t);
        };

        colorChannels(along);
        out->addStrandChannel(kDebugChannel, 0.0f) = strandValues;
        if (along) {
            out->addPointChannel(kDebugChannel, 0.0f) = pointValues;
        } else {
            out->removePointChannel(kDebugChannel);
        }
        parallelFor(0, strands, [&](std::size_t s) {
            const Rgb c = colorOf(strandValues[s]);
            for (int k = 0; k < 3; ++k) (*strandColor[k])[s] = c[k];
            if (!along) return;
            const std::size_t begin = out->strandBegin(s);
            for (std::size_t i = 0; i < out->strandPointCount(s); ++i) {
                const Rgb ci = colorOf(pointValues[begin + i]);
                for (int k = 0; k < 3; ++k) (*pointColor[k])[begin + i] = ci[k];
            }
        });
        std::string note = what + ": " + summary(shown, along);
        if (colors != "random") note += " (shown " + number(lo) + " to " + number(hi) + ")";
        ctx.note(note);
        return StrandsPtr(out);
    }
};

}  // namespace

void registerDebugOperator(OperatorRegistry& registry) {
    registry.registerType(op::kDebug,
                          {"utility",
                           "Colors the hair by a channel, a field or an expression, and notes its values: to see what "
                           "each strand, clump or tube holds.",
                           "inspect visualize show channel attribute value heatmap id preview"},
                          [] { return std::make_unique<DebugOp>(); });
}

}  // namespace groomlab
