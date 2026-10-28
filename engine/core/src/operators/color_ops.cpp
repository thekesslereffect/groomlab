// SPDX-License-Identifier: Apache-2.0
// The color nodes. HairColor: the hair's color per strand as one linear RGB, a hair color in
// aiStandardHair's base-color sense (the colorR, colorG and colorB channels: Arnold's base color
// over no melanin, the viewport's and the app's hairColorSigma, Unreal's groom_color, USD
// displayColor, Alembic Cd, RenderMan's rman_color and the cards bake all read it through
// strandColors(), export_view.h). It works as RenderMan's PxrHairColor does: in mode melanin the
// color is the pigment's (melanin and redness, hair_color.h melaninColor) dyed by a dye; in mode
// color it is the color picked. Per-strand randomness (melanin or value, hue, saturation), shared
// by clumps as clumpShare says, greys, a tip color along the length, and the look's highlight
// (specular, roughness, tilt: hair_color.h hairLobes), which every host shades from the first
// HairColor (GroomSession::hairLook) and which it writes per strand where set off its defaults.
// It also writes each strand's melanin and melaninRedness (as data: no host renders by them while
// the RGB is there). Bands and FeatherColor recolor what comes in (HairColor's colors, else the
// pigment's): bands of another pigment along each hair, on the guard hairs (fur: agouti, ticked);
// a pattern in a feather's own flat space, a color for its down, and iridescence.
//
// Random choices hash the stable strand ID with the seed, so results never
// depend on thread order or strand order.

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <memory>

#include "groomlab/color_patterns.h"
#include "groomlab/curve.h"
#include "groomlab/export_view.h"
#include "groomlab/field.h"
#include "groomlab/graph.h"
#include "groomlab/hair_color.h"
#include "groomlab/operators.h"
#include "groomlab/parallel.h"
#include "groomlab/paths.h"
#include "groomlab/random.h"
#include "groomlab/texture.h"
#include "incremental.h"
#include "internal.h"

namespace groomlab {

namespace {

using Rgb = std::array<float, 3>;

constexpr float kDegrees = 0.0174532925f;

// The colors coming into a recoloring node (Bands, FeatherColor): HairColor's RGB per strand
// (else the pigment's, else HairColor's default look) and per point where it has them.
class IncomingColors {
public:
    explicit IncomingColors(const StrandSet& in) : in_(in) {
        bool strandRgb = true, pointRgb = true;
        for (int k = 0; k < 3; ++k) {
            strand_[k] = in.strandChannel(kColorChannels[k]);
            point_[k] = in.pointChannel(kColorChannels[k]);
            strandRgb = strandRgb && strand_[k] && strand_[k]->size() == in.strandCount();
            pointRgb = pointRgb && point_[k] && point_[k]->size() == in.pointCount();
        }
        if (!strandRgb) strand_ = {};
        if (!pointRgb) point_ = {};
        melanin_ = in.strandChannel(kMelaninChannel);
        if (melanin_ && melanin_->size() != in.strandCount()) melanin_ = nullptr;
        redness_ = in.strandChannel(kMelaninRednessChannel);
        if (redness_ && redness_->size() != in.strandCount()) redness_ = nullptr;
    }
    Rgb strand(std::size_t s) const {
        if (strand_[0]) return {(*strand_[0])[s], (*strand_[1])[s], (*strand_[2])[s]};
        if (melanin_) return melaninColor((*melanin_)[s], redness_ ? (*redness_)[s] : kDefaultMelaninRedness);
        return defaultHairColor();
    }
    // Point i of strand s (whose strand color is `own`).
    Rgb point(std::size_t s, std::size_t i, const Rgb& own) const {
        if (!point_[0]) return own;
        const std::size_t p = in_.strandBegin(s) + i;
        return {(*point_[0])[p], (*point_[1])[p], (*point_[2])[p]};
    }

private:
    const StrandSet& in_;
    std::array<const std::vector<float>*, 3> strand_{}, point_{};
    const std::vector<float>* melanin_ = nullptr;
    const std::vector<float>* redness_ = nullptr;
};

// HairColor, PxrHairColor's way: a base color (the pigment dyed by the dye, or the color picked; a
// map at the root), varied per strand (melanin or value, then hue and saturation, a share of it
// common to the strand's clump), some strands grey, a tip color blended in along the length.
class HairColorOp : public Operator {
public:
    std::string typeName() const override { return op::kHairColor; }
    std::vector<ChannelWrite> channelWrites() const override {
        std::vector<ChannelWrite> out = {{kMelaninChannel, ""}, {kMelaninRednessChannel, ""}};
        for (const char* name : kColorChannels) out.push_back({name, ""});
        // The highlight's, where set off its defaults (a plugged field aside: run decides that).
        auto set = [&](const char* name, const char* random, float fallback) {
            const ParamValue& v = params().get(name);
            return std::holds_alternative<MapRef>(v) || static_cast<float>(params().getDouble(name)) != fallback ||
                   (random && params().getDouble(random) > 0.0);
        };
        if (set("specular", "specularRandom", kDefaultHairSpecular)) out.push_back({kSpecularChannel, ""});
        if (set("roughness", "roughnessRandom", kDefaultHairRoughness)) out.push_back({kRoughnessChannel, ""});
        if (set("tilt", nullptr, kDefaultHairTilt)) out.push_back({kTiltChannel, ""});
        return out;
    }
    std::string category() const override { return "color"; }
    std::vector<PortSpec> inputs() const override { return {{"strands", PortType::Strands}}; }
    std::vector<ParamSpec> paramSpecs() const override {
        return {
            // ---- Base: what colors the hair.
            ParamSpec{"mode", std::string("melanin"),
                      "What colors the hair: melanin (natural hair: melanin and redness, and a dye) or color (the color "
                      "you pick, as it shows and renders); PxrHairColor's Physical and Artistic modes",
                      "", {"melanin", "color"}}
                .in("Base"),
            // kDefaultMelanin and kDefaultMelaninRedness (as doubles, so the catalog shows 0.35, not 0.3499..).
            ParamSpec{"melanin", 0.35, "Pigment, 0 (white, blond) to 1 (black) (mappable)"}.range(0, 1).in("Base").when("mode", {"melanin"}),
            ParamSpec{"redness", 0.2, "Share of red pigment (pheomelanin), 0 (brown, black) to 1 (red) (mappable)"}
                .range(0, 1)
                .in("Base")
                .when("mode", {"melanin"}),
            ParamSpec{"dye", Vec3f(1.0f, 1.0f, 1.0f),
                      "A color dyeing the pigment (white: none): its absorption adds to the pigment's, as aiStandardHair "
                      "combines its melanin and base color (a Map's rgb output can drive it)",
                      "color"}
                .colorMap()
                .in("Base")
                .when("mode", {"melanin"}),
            ParamSpec{"color", Vec3f(0.8f, 0.8f, 0.8f),
                      "The hair's color, as it shows and renders (white hair: 1, 1, 1); a Map's rgb output plugged in "
                      "colors each strand by its root (times this color)",
                      "color"}
                .colorMap()
                .in("Base")
                .when("mode", {"color"}),
            ParamSpec{"map", std::string(),
                      "A color read at each root (empty: none): a map painted in Groom Tools > Paint, by name, a color set "
                      "of that name on the scalp, or an image file at the scalp's UVs. Mode color: it is the color; mode "
                      "melanin: it dyes the pigment"}
                .in("Base"),
            ParamSpec{"greys", 0.0, "Share of strands with almost no pigment: grey and white hairs (mappable)", "fraction"}
                .range(0, 1)
                .in("Base")
                .when("mode", {"melanin"}),
            // ---- Randomness, per strand; clumpShare of it per clump.
            ParamSpec{"melaninRandom", 0.1, "Each strand's melanin up to this much more or less, at random"}
                .range(0, 1)
                .soft(0, 0.5)
                .in("Random")
                .when("mode", {"melanin"}),
            ParamSpec{"valueRandom", 0.0, "Each strand's color up to this share lighter or darker, at random", "fraction"}
                .range(0, 1)
                .in("Random")
                .when("mode", {"color"}),
            ParamSpec{"hueRandom", 0.0, "Each strand's hue turned at random, up to half a turn either way at 1", "fraction"}
                .range(0, 1)
                .in("Random"),
            ParamSpec{"saturationRandom", 0.0,
                      "Each strand's saturation up to this share more or less, at random (grey and white stay neutral)",
                      "fraction"}
                .range(0, 1)
                .in("Random"),
            ParamSpec{"clumpShare", 0.5,
                      "How much of the randomness the strands of one clump share: 0 each strand its own, 1 the whole clump "
                      "alike (needs a Clump before HairColor: its clumpChannel)",
                      "fraction"}
                .range(0, 1)
                .in("Random"),
            ParamSpec{"clumpChannel", std::string("clumpId"),
                      "The channel naming each strand's clump, for clumpShare: a Clump's clumpChannel (clumpId by default; "
                      "with chained Clumps, the one whose clumps should share a color)"}
                .in("Random")
                .readsChannel(true),
            notAnimatable({"seed", std::int64_t{0}, "Random seed"}).in("Advanced").adv(),
            // ---- The tips.
            ParamSpec{"tipColor", Vec3f(0.55f, 0.42f, 0.28f),
                      "The color the strands blend toward at the tip (with tipStrength above 0; a Map's rgb output can "
                      "drive it)",
                      "color"}
                .colorMap()
                .in("Tip"),
            ParamSpec{"tipStrength", 0.0,
                      "How far toward tipColor the tips go, 0-1 (mappable, per point: a painted per-point layer or an "
                      "expression of t paints color along the strands)",
                      "fraction"}
                .range(0, 1)
                .in("Tip"),
            ParamSpec{"tipProfile", Ramp::linear(0.0f, 1.0f), "Where along the strand the tip color shows, root to tip, times tipStrength"}
                .in("Tip"),
            ParamSpec{"tipOffsetRandom", 0.0,
                      "Each strand's tip color starts up to this share of its length earlier or later, at random",
                      "fraction"}
                .range(0, 1)
                .soft(0, 0.5)
                .in("Tip"),
            // ---- The highlight (hair_color.h hairLobes: kDefaultHairSpecular, kDefaultHairRoughness
            // and kDefaultHairTilt, as doubles).
            ParamSpec{"specular", 0.3, "Highlight strength, 0 none to 1 bright (mappable; 0.3 is aiStandardHair's full specular tint)"}
                .range(0, 1)
                .in("Highlight"),
            ParamSpec{"specularRandom", 0.0, "Each strand's specular up to this share more or less, at random", "fraction"}
                .range(0, 1)
                .in("Highlight")
                .adv(),
            ParamSpec{"roughness", 0.5, "Highlight spread, 0 sharp to 1 broad (mappable)"}.range(0, 1).in("Highlight"),
            ParamSpec{"roughnessRandom", 0.0, "Each strand's roughness up to this share more or less, at random", "fraction"}
                .range(0, 1)
                .in("Highlight")
                .adv(),
            ParamSpec{"tilt", 3.0,
                      "Cuticle tilt, degrees: how far the highlight and its colored second highlight move apart along "
                      "the strand (real hair: 2-4)",
                      "angle"}
                .range(-20, 20)
                .soft(0, 10)
                .in("Highlight")
                .adv(),
        };
    }
    // A painted map is found through the host's map catalog ("maps"): it joins the cache key.
    std::vector<std::string> externalInputs() const override {
        if (params().getString("map").empty()) return {"scalp"};
        return {"scalp", "maps"};
    }
    // What the map is read from joins the key: a texture map's or an image's file stamp (a
    // rewritten file re-evaluates the node), a color set's hash (the scalp's key leaves color sets
    // out).
    std::uint64_t contextSalt(const EvalContext& ctx) const override {
        const std::string name = params().getString("map");
        if (name.empty()) return 0;
        const MapInfo* info = catalogMap(ctx, name);
        if (info && info->kind == "texture") return info->texture.empty() ? 0 : fileStamp(resolvePath(info->texture));
        std::uint64_t salt = colorSetSalt(ctx, name);
        if (!info && isImagePath(name)) salt = hashCombine(salt, fileStamp(resolvePath(name)));
        return salt;
    }

    Value evaluate(const EvalContext& ctx, const std::vector<Value>& inputs) const override {
        Value out;
        run(ctx, inputs, nullptr, &out, nullptr);
        return out;
    }
    bool evaluateIncremental(const EvalContext& ctx, const std::vector<Value>& inputs, const IncrementalInputs& incremental,
                             Value* out, StrandChanges* changed) const override {
        return run(ctx, inputs, &incremental, out, changed);
    }

private:
    // In full, or (with `incremental`) only the strands the input changed in, into a copy of the
    // last result's colors; false when that cannot be done.
    bool run(const EvalContext& ctx, const std::vector<Value>& inputs, const IncrementalInputs* incremental, Value* result,
             StrandChanges* changed) const {
        const StrandsPtr in = requireStrands(inputs, op::kHairColor);
        const ScalpMesh* scalp = scalpOf(ctx);
        const std::string where = std::string(op::kHairColor) + ": ";

        const std::string modeName = params().getString("mode");
        if (modeName != "melanin" && modeName != "color") throw GraphError(where + "mode must be melanin or color");
        const bool colorMode = modeName == "color";
        const ScalarField melaninField(params(), "melanin", scalp);
        const ScalarField rednessField(params(), "redness", scalp);
        const ScalarField greysField(params(), "greys", scalp);
        for (const ScalarField* f : {&melaninField, &rednessField, &greysField}) f->validate(*in);
        // The colors, each a param's own times a color field plugged in (a Map's rgb output).
        const ColorField dyeField(params(), "dye"), pickedField(params(), "color"), tipColorField(params(), "tipColor");
        for (const ColorField* f : {&dyeField, &pickedField, &tipColorField}) f->validate(*in);

        // The map by name: where it lives says how it is read (a texture map's file, a vertex map's
        // color set on the scalp, a guide layer's strand channel after Interpolate: gray); a name the
        // catalog does not hold is a color set on the scalp, a strand channel, or an image file.
        const std::string mapName = params().getString("map");
        TexturePtr mapTexture;
        std::string mapColorSet;
        const std::vector<float>* mapLayer = nullptr;
        if (!mapName.empty()) {
            const MapInfo* info = catalogMap(ctx, mapName);
            std::string kind = info ? info->kind : "";
            if (kind.empty()) {
                kind = scalp && scalp->hasColorSet(mapName) ? "vertices"
                       : in->strandChannel(mapName)        ? "guides"
                       : isImagePath(mapName)              ? "image"
                                                           : "";
            }
            const std::string about = where + "map '" + mapName + "': ";
            if (kind == "texture" || kind == "image") {
                const std::string file = kind == "image" ? mapName : info->texture;
                if (file.empty()) throw GraphError(about + "the texture map has no image file yet");
                if (!scalp || !scalp->hasUVs()) throw GraphError(about + "an image needs a scalp with UVs");
                try {
                    mapTexture = loadTexture(resolvePath(file));
                } catch (const TextureError& e) {
                    throw GraphError(about + e.what());
                }
            } else if (kind == "vertices") {
                if (!scalp || !scalp->hasColorSet(mapName)) {
                    throw GraphError(about + "the vertex map does not fit this scalp (paint it again, or convert it)");
                }
                mapColorSet = mapName;
            } else if (kind == "guides") {
                mapLayer = in->strandChannel(mapName);
                if (!mapLayer) {
                    throw GraphError(about + "a guide layer reaches the strands through Interpolate (put HairColor after "
                                             "it); for painted color, convert the map to vertices or a texture");
                }
            } else {
                throw GraphError(about + "no map, color set or image of that name (Groom Tools > Paint > New... makes a map)");
            }
        }
        const bool hasMap = mapTexture || !mapColorSet.empty() || mapLayer;

        const ScalarField tipField(params(), "tipStrength", scalp);
        tipField.validate(*in);
        const bool tipped = !tipField.isConstant() || tipField.at(*in, 0) > 0.0f;
        const bool tipPerPoint = tipField.perPoint();
        if (!tipped) {
            const ParamValue* fallback = params().defaultValue("tipColor");
            const Vec3f* tipDefault = fallback ? std::get_if<Vec3f>(fallback) : nullptr;
            const Vec3f tipSet = params().getVec3("tipColor");
            if (tipDefault && !(tipSet.x == tipDefault->x && tipSet.y == tipDefault->y && tipSet.z == tipDefault->z)) {
                ctx.note("tipColor is set but tipStrength is 0: raise tipStrength to see it");
            }
        }
        const Ramp tipProfile = params().getRamp("tipProfile");
        const float tipOffsetRandom = tipped ? static_cast<float>(params().getDouble("tipOffsetRandom")) : 0.0f;

        // The randomness: melanin (mode melanin) or value (mode color), hue and saturation.
        const float melaninRandom = colorMode ? 0.0f : static_cast<float>(params().getDouble("melaninRandom"));
        const float valueRandom = colorMode ? static_cast<float>(params().getDouble("valueRandom")) : 0.0f;
        const float hueRandom = static_cast<float>(params().getDouble("hueRandom"));
        const float saturationRandom = static_cast<float>(params().getDouble("saturationRandom"));
        const float clumpShare = std::clamp(static_cast<float>(params().getDouble("clumpShare")), 0.0f, 1.0f);
        const auto seed = static_cast<std::uint64_t>(params().getInt("seed"));
        const bool random = melaninRandom > 0.0f || valueRandom > 0.0f || hueRandom > 0.0f || saturationRandom > 0.0f;

        // The highlight: its strength and spread (mappable, each a random share more or less per
        // strand) and the cuticle tilt, per strand for the renderers (export_view.h: Arnold's user
        // data, RenderMan's primvars), written only where set off the defaults (the strands
        // without them take those).
        const ScalarField specularField(params(), "specular", scalp), roughnessField(params(), "roughness", scalp);
        specularField.validate(*in);
        roughnessField.validate(*in);
        const auto specularRandom = static_cast<float>(params().getDouble("specularRandom"));
        const auto roughnessRandom = static_cast<float>(params().getDouble("roughnessRandom"));
        const auto tilt = static_cast<float>(params().getDouble("tilt"));
        auto highlightSet = [](const ScalarField& f, float spread, float fallback) {
            return !f.isConstant() || spread > 0.0f || f.maximum() != fallback;
        };
        const bool specularSet = highlightSet(specularField, specularRandom, kDefaultHairSpecular);
        const bool roughnessSet = highlightSet(roughnessField, roughnessRandom, kDefaultHairRoughness);
        const bool tiltSet = tilt != kDefaultHairTilt;

        const StrandChanges* changes = nullptr;
        StrandsPtr last;  // the last result, to color again only the strands the input changed in
        if (incremental) {
            changes = someStrandsChanged(*incremental, 0);
            last = changes ? previousStrandsFor(*incremental, *in) : nullptr;
            if (!last || !melaninField.strandLocal() || !rednessField.strandLocal() || !greysField.strandLocal() ||
                !tipField.strandLocal() || !specularField.strandLocal() || !roughnessField.strandLocal() ||
                !dyeField.strandLocal() || !pickedField.strandLocal() || !tipColorField.strandLocal()) {
                return false;
            }
            if (changes->none()) {  // the same strands in: the same result
                *result = last;
                *changed = *changes;
                return true;
            }
        }
        auto out = std::make_shared<StrandSet>(*in);
        const std::string clumpChannel = params().getString("clumpChannel");
        const std::vector<float>* clumps = out->strandChannel(clumpChannel);
        if (clumps && clumps->size() != out->strandCount()) clumps = nullptr;
        // (Said when clumpShare is set off its default: a fresh groom's HairColor has no Clump before it.)
        const ParamValue* clumpShareDefault = params().defaultValue("clumpShare");
        const bool clumpShareSet = !clumpShareDefault || !std::holds_alternative<double>(*clumpShareDefault) ||
                                   std::get<double>(*clumpShareDefault) != params().getDouble("clumpShare");
        if (!clumps && random && clumpShare > 0.0f && clumpShareSet && !last) {
            ctx.note("clumpShare: no " + clumpChannel + " channel reaches HairColor (put it after a Clump), so each strand "
                     "varies on its own");
        }
        const std::vector<StrandRoot>& roots = out->roots();
        // Every strand's colors are written: as made fresh, or the last result's (the strands left
        // alone keep theirs).
        auto channel = [&](const char* name, float fallbackValue) {
            return last ? out->copyStrandChannelFrom(name, *last) : &out->addStrandChannel(name, fallbackValue);
        };
        // The defaults are HairColor's own look: strands merged in later without color read brown, not black.
        std::vector<float>* const melaninChannel = channel(kMelaninChannel, kDefaultMelanin);
        std::vector<float>* const rednessChannel = channel(kMelaninRednessChannel, kDefaultMelaninRedness);
        const std::array<float, 3> fallback = defaultHairColor();
        std::array<std::vector<float>*, 3> strand{};
        for (int k = 0; k < 3; ++k) strand[k] = channel(kColorChannels[k], fallback[k]);
        std::array<std::vector<float>*, 3> point{};
        for (int k = 0; k < 3; ++k) {
            // Without a tip the color is per strand only: an earlier color node's per-point colors
            // would go stale (exporters prefer per-point), so they go.
            if (tipped) {
                point[k] = last ? out->copyPointChannelFrom(kColorChannels[k], *last) : &out->addPointChannel(kColorChannels[k], fallback[k]);
            } else {
                out->removePointChannel(kColorChannels[k]);
            }
        }
        // HairColor sets a look afresh: an earlier node's iridescence would show through.
        out->removePointChannel(kIridescenceChannel);
        // The highlight's channels (a later HairColor's replace an earlier one's: at the defaults, none).
        auto highlightChannel = [&](bool set, const char* name, float fallbackValue) -> std::vector<float>* {
            if (set) return channel(name, fallbackValue);
            out->removeStrandChannel(name);
            return nullptr;
        };
        std::vector<float>* const specularOut = highlightChannel(specularSet, kSpecularChannel, kDefaultHairSpecular);
        std::vector<float>* const roughnessOut = highlightChannel(roughnessSet, kRoughnessChannel, kDefaultHairRoughness);
        std::vector<float>* const tiltOut = highlightChannel(tiltSet, kTiltChannel, kDefaultHairTilt);
        if ((specularSet && !specularOut) || (roughnessSet && !roughnessOut) || (tiltSet && !tiltOut)) return false;
        if (!melaninChannel || !rednessChannel) return false;
        for (int k = 0; k < 3; ++k) {
            if (!strand[k] || (tipped && !point[k])) return false;
        }
        std::vector<float>& melanin = *melaninChannel;
        std::vector<float>& redness = *rednessChannel;
        const RampAlong along(tipProfile, *out);

        // A value in [-1, 1) of strand s, draw k, for the highlight's randomness.
        auto spread = [&](std::size_t s, std::uint64_t k) {
            return 2.0f * unitFloat(hashCombine(hashCombine(out->ids()[s], seed ^ 0x2E4DA7Aull), k)) - 1.0f;
        };
        auto varied = [&](float base, float amount, std::size_t s, std::uint64_t k) {
            return std::clamp(base * (amount > 0.0f ? 1.0f + amount * spread(s, k) : 1.0f), 0.0f, 1.0f);
        };
        // Clump and strand shares of the randomness, mixed so the spread stays the same at any
        // clumpShare (their squares add up to 1).
        const float clumpWeight = clumps ? std::sqrt(clumpShare) : 0.0f;
        const float strandWeight = clumps ? std::sqrt(1.0f - clumpShare) : 1.0f;
        auto color = [&](std::size_t s) {
            if (specularOut) (*specularOut)[s] = varied(specularField.at(*out, s), specularRandom, s, 1);
            if (roughnessOut) (*roughnessOut)[s] = varied(roughnessField.at(*out, s), roughnessRandom, s, 2);
            if (tiltOut) (*tiltOut)[s] = tilt;
            // Draw k in [-1, 1] of this strand: its own, part of it its clump's.
            const std::uint64_t own = hashCombine(out->ids()[s], seed ^ 0xC0105);
            const std::uint64_t clumpKey =
                clumps ? hashCombine(static_cast<std::uint64_t>(static_cast<std::int64_t>(std::floor((*clumps)[s]))), seed ^ 0xC1C1)
                       : 0;
            auto draw = [&](std::uint64_t k) {
                const float mine = 2.0f * unitFloat(hashCombine(own, k)) - 1.0f;
                if (!clumps) return mine;
                const float theirs = 2.0f * unitFloat(hashCombine(clumpKey, k)) - 1.0f;
                return std::clamp(mine * strandWeight + theirs * clumpWeight, -1.0f, 1.0f);
            };
            // The base color: the pigment (melanin and redness, varied, some strands grey) times
            // the dye, or the color picked; the map's at the root.
            float m = 0.0f, r = 0.0f;
            Rgb c = colorMode ? pickedField.at(*out, s) : Rgb{};
            if (!colorMode) {
                m = melaninField.at(*out, s) + (melaninRandom > 0.0f ? melaninRandom * draw(1) : 0.0f);
                r = rednessField.at(*out, s);
                if (unitFloat(hashCombine(own, 3)) < greysField.at(*out, s)) {
                    m *= 0.05f;
                    r = 0.0f;
                }
                m = std::clamp(m, 0.0f, 1.0f);
                r = std::clamp(r, 0.0f, 1.0f);
                c = melaninColor(m, r, dyeField.at(*out, s));
            }
            melanin[s] = m;
            redness[s] = r;
            if (hasMap && scalp && s < roots.size() && scalp->contains(roots[s])) {
                Rgb map{1.0f, 1.0f, 1.0f};
                if (mapTexture) {
                    const ScalpMesh::UV uv = scalp->uvAt(roots[s]);
                    const std::array<float, 4> t = mapTexture->sample(uv[0], uv[1]);
                    for (int k = 0; k < 3; ++k) map[k] = t[k];
                } else if (!mapColorSet.empty()) {
                    const ScalpMesh::Color cs = scalp->colorAt(mapColorSet, roots[s]);
                    for (int k = 0; k < 3; ++k) map[k] = cs[k];
                } else if (mapLayer) {
                    const float v = std::clamp((*mapLayer)[s], 0.0f, 1.0f);
                    map = {v, v, v};
                }
                c = colorMode ? map : dyedHairColor(c, map);
            }
            // The strand's value, hue and saturation draws, on every color it takes (its tip's too).
            const float valueScale = valueRandom > 0.0f ? 1.0f + valueRandom * draw(4) : 1.0f;
            const float hueTurn = hueRandom > 0.0f ? 0.5f * hueRandom * draw(5) : 0.0f;
            const float saturationScale = saturationRandom > 0.0f ? 1.0f + saturationRandom * draw(6) : 1.0f;
            auto vary = [&](const Rgb& x) {
                return varyHairColor(x, hueTurn, saturationScale, valueScale);
            };
            c = vary(c);
            for (int k = 0; k < 3; ++k) (*strand[k])[s] = std::clamp(c[k], 0.0f, 1.0f);

            if (tipped) {
                const std::size_t n = out->strandPointCount(s), begin = out->strandBegin(s);
                const float strandTip = tipPerPoint ? 0.0f : tipField.at(*out, s);
                // The tip color varied as the strand's own, and where it starts shifted along the
                // strand at random (tipOffsetRandom).
                const bool tipAlong = tipColorField.perPoint();
                const Rgb tipColor = vary(tipColorField.at(*out, s));
                const float shift =
                    tipOffsetRandom > 0.0f ? tipOffsetRandom * (2.0f * unitFloat(hashCombine(own, 0x71B0)) - 1.0f) : 0.0f;
                for (std::size_t i = 0; i < n; ++i) {
                    Rgb ci = c;
                    const float amount = tipPerPoint ? tipField.atPoint(*out, s, i) : strandTip;
                    // Shifted, the profile still ends at the tip: t from the shifted start to 1.
                    const float profile =
                        shift != 0.0f ? tipProfile(std::clamp((paramAt(i, n) - shift) / std::max(1.0f - shift, 0.05f), 0.0f, 1.0f))
                                      : along(i, n);
                    const float w = std::clamp(amount * profile, 0.0f, 1.0f);
                    const Rgb tipHere = tipAlong ? vary(tipColorField.atPoint(*out, s, i)) : tipColor;
                    for (int k = 0; k < 3; ++k) ci[k] = ci[k] + (tipHere[k] - ci[k]) * w;
                    for (int k = 0; k < 3; ++k) (*point[k])[begin + i] = std::clamp(ci[k], 0.0f, 1.0f);
                }
            }
        };
        if (last) {
            forEachListed(changes->strands, color);
            *changed = *changes;
        } else {
            parallelFor(0, out->strandCount(), color);
        }
        *result = StrandsPtr(out);
        return true;
    }
};

// The per-point colors a recoloring node writes (and the strand colors, when it changes them):
// fresh (each strand's incoming colors to start from), or copies of the last result's for an
// incremental evaluation. Null entries when the last result lacks them (evaluate in full then).
struct RecolorChannels {
    std::array<std::vector<float>*, 3> point{}, strand{};

    RecolorChannels(StrandSet& out, const StrandSet& in, const StrandsPtr& last, bool strands) {
        const std::array<float, 3> fallback = defaultHairColor();
        for (int k = 0; k < 3; ++k) {
            if (last) {
                point[k] = out.copyPointChannelFrom(kColorChannels[k], *last);
                if (strands) strand[k] = out.copyStrandChannelFrom(kColorChannels[k], *last);
            } else {
                point[k] = &out.addPointChannel(kColorChannels[k], fallback[k]);
                // A strand channel the input had is kept as it was (shared) until written.
                if (strands) {
                    const std::vector<float>* had = in.strandChannel(kColorChannels[k]);
                    strand[k] = had && had->size() == in.strandCount() ? out.mutableStrandChannel(kColorChannels[k])
                                                                       : &out.addStrandChannel(kColorChannels[k], fallback[k]);
                }
            }
        }
    }
    bool complete(bool strands) const {
        for (int k = 0; k < 3; ++k) {
            if (!point[k] || (strands && !strand[k])) return false;
        }
        return true;
    }
    void setPoint(std::size_t p, const Rgb& c) const {
        for (int k = 0; k < 3; ++k) (*point[k])[p] = std::clamp(c[k], 0.0f, 1.0f);
    }
    void setStrand(std::size_t s, const Rgb& c) const {
        for (int k = 0; k < 3; ++k) (*strand[k])[s] = std::clamp(c[k], 0.0f, 1.0f);
    }
};

// Bands of another pigment along each hair, over the colors coming in (fur: agouti, ticked,
// shaded), on the guard hairs alone when `guards` keeps some strands as a plain undercoat (which
// takes no bands and no tip color, and its own tint). The band pigment takes a random offset per
// strand (randomness), drawn as HairColor draws its own (so the same seed and amounts give the
// same offsets).
class BandsOp : public Operator {
public:
    std::string typeName() const override { return op::kBands; }
    std::vector<ChannelWrite> channelWrites() const override {
        std::vector<ChannelWrite> out;
        for (const char* name : kColorChannels) out.push_back({name, ""});
        return out;
    }
    std::string category() const override { return "color"; }
    std::vector<PortSpec> inputs() const override { return {{"strands", PortType::Strands}}; }
    std::vector<std::string> externalInputs() const override { return {"scalp"}; }
    std::vector<ParamSpec> paramSpecs() const override {
        return {
            ParamSpec{"strength", 1.0, "How strongly the bands show, 0 (none) to 1 (mappable, per point)", "fraction"}.range(0, 1),
            ParamSpec{"melanin", 0.08, "The bands' pigment, 0 (pale) to 1 (black)"}.range(0, 1),
            ParamSpec{"redness", 0.6, "The bands' redness, 0 (brown) to 1 (red): agouti's band is a pale yellow-red"}.range(0, 1),
            ParamSpec{"dye", Vec3f(1.0f, 1.0f, 1.0f), "Dye over the bands' pigment, linear RGB (white: none)", "color"}.colorMap(),
            ParamSpec{"profile", Ramp{{0.0f, 0.0f}, {0.5f, 0.0f}, {0.6f, 1.0f}, {0.78f, 1.0f}, {0.88f, 0.0f}, {1.0f, 0.0f}},
                      "Where the band color is along one period of the pattern, root to tip: 0 the hair's own, 1 the band's"},
            ParamSpec{"count", std::int64_t{1}, "How many times the profile repeats root to tip (ticked fur: several)"}
                .range(1, 1000)
                .soft(1, 10),
            ParamSpec{"guards", 1.0,
                      "Share of the strands that are guard hairs, which take the bands and keep the tip color (1: all); "
                      "the rest are undercoat in the plain color",
                      "fraction"}
                .range(0, 1)
                .in("Guard hairs"),
            ParamSpec{"guardsBy", std::string("random"), "Which strands are the guard hairs: at random, or the longest", "",
                      {"random", "longest"}}
                .in("Guard hairs"),
            ParamSpec{"undercoatDye", Vec3f(1.0f, 1.0f, 1.0f),
                      "Tint over the undercoat's color, linear RGB (white: the hair's own)", "color"}
                .colorMap()
                .in("Guard hairs"),
            ParamSpec{"melaninRandom", 0.1, "Random offset of each strand's band pigment (melanin and redness), +/-"}
                .soft(0, 0.5)
                .in("Random"),
            ParamSpec{"offsetRandom", 0.15,
                      "Random shift of each strand's bands along it, as a share of a period, so bands do not line up "
                      "across the coat",
                      "fraction"}
                .range(0, 1)
                .in("Random"),
            ParamSpec{"widthRandom", 0.1, "Random stretch of each strand's bands, +/- this share", "fraction"}
                .range(0, 0.9)
                .in("Random"),
            notAnimatable({"seed", std::int64_t{0}, "Random seed (HairColor's, for the same pigment offsets)"}).in("Advanced").adv(),
        };
    }
    Value evaluate(const EvalContext& ctx, const std::vector<Value>& inputs) const override {
        Value out;
        run(ctx, inputs, nullptr, &out, nullptr);
        return out;
    }
    bool evaluateIncremental(const EvalContext& ctx, const std::vector<Value>& inputs, const IncrementalInputs& incremental,
                             Value* out, StrandChanges* changed) const override {
        return run(ctx, inputs, &incremental, out, changed);
    }

private:
    bool run(const EvalContext& ctx, const std::vector<Value>& inputs, const IncrementalInputs* incremental, Value* result,
             StrandChanges* changed) const {
        const StrandsPtr in = requireStrands(inputs, op::kBands);
        const ScalpMesh* scalp = scalpOf(ctx);
        const std::string where = std::string(op::kBands) + ": ";
        const ScalarField strength(params(), "strength", scalp);
        strength.validate(*in);
        const bool banded = !strength.isConstant() || strength.at(*in, 0) > 0.0f;
        const bool perPoint = strength.perPoint();
        const float bandMelanin = static_cast<float>(params().getDouble("melanin"));
        const float bandRedness = static_cast<float>(params().getDouble("redness"));
        const ColorField bandDye(params(), "dye");
        const Ramp profile = params().getRamp("profile");
        const int count = static_cast<int>(params().getInt("count"));
        const float randomness = static_cast<float>(params().getDouble("melaninRandom"));
        const float offsetRandom = static_cast<float>(params().getDouble("offsetRandom"));
        const float widthRandom = static_cast<float>(params().getDouble("widthRandom"));
        const float guards = static_cast<float>(params().getDouble("guards"));
        const std::string guardsBy = params().getString("guardsBy");
        if (guardsBy != "random" && guardsBy != "longest") throw GraphError(where + "guardsBy must be random or longest");
        const bool undercoat = guards < 1.0f;
        const bool byLength = undercoat && guardsBy == "longest";
        const ColorField undercoatDye(params(), "undercoatDye");
        bandDye.validate(*in);
        undercoatDye.validate(*in);
        const auto seed = static_cast<std::uint64_t>(params().getInt("seed"));

        const StrandChanges* changes = nullptr;
        StrandsPtr last;
        if (incremental) {
            changes = someStrandsChanged(*incremental, 0);
            if (!changes) return false;
            if (!banded && !undercoat) {  // nothing to do: the input as it came
                *result = inputs[0];
                *changed = *changes;
                return true;
            }
            last = previousStrandsFor(*incremental, *in);
            if (!last || !strength.strandLocal() || !bandDye.strandLocal() || !undercoatDye.strandLocal() || byLength) return false;  // (by length: a quantile of every strand's)
            if (changes->none()) {
                *result = last;
                *changed = *changes;
                return true;
            }
        }
        if (!banded && !undercoat) {
            *result = inputs[0];
            return true;
        }
        const IncomingColors incoming(*in);
        auto out = std::make_shared<StrandSet>(*in);
        const RecolorChannels colors(*out, *in, last, undercoat);
        if (!colors.complete(undercoat)) return false;

        // Guard hairs by length: the shortest (1 - guards) of the strands are undercoat, the length
        // that parts them measured on the core strands (preview_core.h: the same at every preview
        // density).
        float guardLength = 0.0f;
        const std::vector<std::size_t> measured = byLength ? coreStrands(*out) : std::vector<std::size_t>{};
        if (byLength && !measured.empty()) {
            std::vector<float> lengths(measured.size());
            parallelFor(0, measured.size(), [&](std::size_t k) {
                lengths[k] = arcLength(out->strandPoints(measured[k]), out->strandPointCount(measured[k]));
            });
            const auto rank = static_cast<std::size_t>(std::floor((1.0f - guards) * static_cast<float>(lengths.size())));
            if (rank >= lengths.size()) {
                guardLength = std::numeric_limits<float>::infinity();
            } else {
                std::nth_element(lengths.begin(), lengths.begin() + static_cast<std::ptrdiff_t>(rank), lengths.end());
                guardLength = roundToPercentStep(lengths[rank], ctx.scale);  // in 1% steps: a few strands changing keeps it
            }
        }

        auto recolor = [&](std::size_t s) {
            const std::size_t n = out->strandPointCount(s), begin = out->strandBegin(s);
            const Rgb own = incoming.strand(s);
            const bool guard = !undercoat || (byLength ? arcLength(out->strandPoints(s), n) >= guardLength
                                                       : unitFloat(hashCombine(out->ids()[s], seed ^ 0x6A2D)) < guards);
            if (!guard) {  // the undercoat: its own tint, no bands and no tip color
                const Rgb tint = undercoatDye.at(*out, s);
                const Rgb c{own[0] * tint[0], own[1] * tint[1], own[2] * tint[2]};
                colors.setStrand(s, c);
                for (std::size_t i = 0; i < n; ++i) colors.setPoint(begin + i, c);
                return;
            }
            if (undercoat) colors.setStrand(s, own);
            if (!banded) {
                for (std::size_t i = 0; i < n; ++i) colors.setPoint(begin + i, incoming.point(s, i, own));
                return;
            }
            // The band's pigment, offset per strand as HairColor offsets the hair's.
            const std::uint64_t key = hashCombine(out->ids()[s], seed ^ 0xC0105);
            const float melaninOffset = randomness * (2.0f * unitFloat(hashCombine(key, 1)) - 1.0f);
            const float rednessOffset = randomness * (2.0f * unitFloat(hashCombine(key, 2)) - 1.0f);
            const Rgb band = melaninColor(std::clamp(bandMelanin + melaninOffset, 0.0f, 1.0f),
                                          std::clamp(bandRedness + rednessOffset, 0.0f, 1.0f), bandDye.at(*out, s));
            const std::uint64_t h = hashCombine(out->ids()[s], seed ^ 0xBA4D5);
            const float phase = offsetRandom * (2.0f * unitFloat(hashCombine(h, 1)) - 1.0f);
            const float scale = 1.0f + widthRandom * (2.0f * unitFloat(hashCombine(h, 2)) - 1.0f);
            const float strandStrength = perPoint ? 0.0f : strength.at(*out, s);
            for (std::size_t i = 0; i < n; ++i) {
                Rgb c = incoming.point(s, i, own);
                const float amount = perPoint ? strength.atPoint(*out, s, i) : strandStrength;
                const float w = std::clamp(amount * bandAt(profile, paramAt(i, n), count, scale, phase), 0.0f, 1.0f);
                for (int k = 0; k < 3; ++k) c[k] += (band[k] - c[k]) * w;
                colors.setPoint(begin + i, c);
            }
        };
        if (last) {
            forEachListed(changes->strands, recolor);
            *changed = *changes;
        } else {
            parallelFor(0, out->strandCount(), recolor);
        }
        *result = StrandsPtr(out);
        return true;
    }
};

// Colors on feathers (after a Propagate), over the colors coming in: a pattern in the feather's
// own flat space (vane space, color_patterns.h: a painted image of the feather, bars, chevrons,
// spots, an edge fringe, eye spots), a color for the down zone's barbs, and iridescence, which is
// not pigment: the per-point `iridescence` channel says how much structural color the hair
// shading mixes in, the node's iridescenceColor and iridescenceShift what it is
// (GroomSession::hairLook hands them to every host).
class FeatherColorOp : public Operator {
public:
    std::string typeName() const override { return op::kFeatherColor; }
    // 2: downStrength 1 by default (nodes saved before keep 0: ParamSpec::formerDefault).
    int version() const override { return 2; }
    std::vector<ChannelWrite> channelWrites() const override {
        std::vector<ChannelWrite> out;
        for (const char* name : kColorChannels) out.push_back({name, ""});
        out.push_back({kIridescenceChannel, ""});
        return out;
    }
    std::string category() const override { return "color"; }
    std::vector<PortSpec> inputs() const override { return {{"strands", PortType::Strands}}; }
    std::vector<std::string> externalInputs() const override { return {"scalp"}; }
    std::vector<ParamSpec> paramSpecs() const override {
        std::vector<std::string> patterns = vanePatternNames();
        const std::vector<std::string> any = {"texture", "bars", "chevrons", "spots", "fringe", "eye"};
        const std::vector<std::string> shapes = {"bars", "chevrons", "spots", "fringe", "eye"};
        return {
            ParamSpec{"pattern", std::string("bars"),
                      "A pattern on the feathers in their own flat space: a painted image of the feather, bars, chevrons, "
                      "spots, an edge fringe or an eye spot, in the pattern color (none: only the down color and "
                      "iridescence)",
                      "", patterns}
                .in("Pattern"),
            ParamSpec{"strength", 1.0, "How strongly the pattern shows, 0-1", "fraction"}.range(0, 1).in("Pattern").when("pattern", any),
            ParamSpec{"melanin", 0.0, "The pattern's pigment, 0 (white) to 1 (black)"}.range(0, 1).in("Pattern").when("pattern", shapes),
            ParamSpec{"redness", 0.0, "The pattern's redness"}.range(0, 1).in("Pattern").when("pattern", shapes),
            ParamSpec{"dye", Vec3f(1.0f, 1.0f, 1.0f), "Dye over the pattern's pigment, linear RGB", "color"}
                .colorMap()
                .in("Pattern")
                .when("pattern", shapes),
            ParamSpec{"vanes", std::string("both"),
                      "Which vanes the pattern covers (left and right seen from the vane's top, the tip up)", "",
                      {"both", "left", "right"}}
                .in("Pattern")
                .when("pattern", any),
            ParamSpec{"count", 6.0, "Bars, chevrons, rows of spots per feather length"}
                .atLeast(0.01)
                .soft(1, 20)
                .in("Pattern")
                .when("pattern", {"bars", "chevrons", "spots"}),
            ParamSpec{"width", 0.4,
                      "Bars, chevrons, spots: the share of each period the shape takes; eye: its diameter (a share of the "
                      "feather's length); fringe: the share of each barb from its tip",
                      "fraction"}
                .soft(0, 1)
                .in("Pattern")
                .when("pattern", shapes),
            ParamSpec{"angle", 0.0, "Degrees: bars' tilt from straight across; chevrons' arms (try 30); an eye's tilt", "angle"}
                .soft(-90, 90)
                .in("Pattern")
                .when("pattern", {"bars", "chevrons", "eye"}),
            ParamSpec{"sharpness", 0.7, "Edges, 0 soft to 1 hard"}.range(0, 1).in("Pattern").when("pattern", shapes),
            ParamSpec{"offset", 0.7, "Along the feather, 0 root .. 1 tip: shifts bars and spots; the eye spot's center",
                      "fraction"}
                .soft(0, 1)
                .in("Pattern")
                .when("pattern", {"bars", "chevrons", "spots", "eye"}),
            ParamSpec{"profile", Ramp::constant(1.0f),
                      "Across each shape, its center (0) to its edge (1): rings for an eye spot, a hollow bar"}
                .in("Pattern")
                .when("pattern", shapes),
            ParamSpec{"texture", std::string(),
                      "With pattern texture: an image of the feather laid flat, the shaft up the middle, the left vane on "
                      "the left, the root at the bottom; its alpha says where it covers"}
                .in("Pattern")
                .when("pattern", {"texture"}),
            ParamSpec{"textureRight", std::string(),
                      "Optional: the right vane's own image (texture is then the left vane's), the shaft along the left edge"}
                .in("Pattern")
                .when("pattern", {"texture"}),
            ParamSpec{"offsetRandom", 0.2, "Random shift of each feather's pattern, as a share of its spacing (eye: its size)"}
                .atLeast(0)
                .soft(0, 1)
                .in("Random")
                .when("pattern", {"bars", "chevrons", "spots", "eye"}),
            ParamSpec{"scaleRandom", 0.1, "Random size of each feather's pattern, +/- this share", "fraction"}
                .range(0, 0.95)
                .in("Random")
                .when("pattern", {"bars", "chevrons", "spots", "eye"}),
            ParamSpec{"angleRandom", 3.0, "Random turn of each feather's pattern, +/- degrees", "angle"}
                .soft(0, 45)
                .in("Random")
                .when("pattern", {"bars", "chevrons", "eye"}),
            ParamSpec{"downColor", Vec3f(0.9f, 0.88f, 0.82f), "The down zone's barbs' color (Propagate's down), linear RGB",
                      "color"}
                .colorMap()
                .in("Down"),
            ParamSpec{"downStrength", 1.0, "How far the down zone's barbs go to downColor, 0-1", "fraction"}
                .range(0, 1)
                .formerDefault(2, 0.0)
                .in("Down"),
            // Iridescence: structural color, angle-dependent, in the shading.
            ParamSpec{"iridescence", 0.0,
                      "Structural color, 0 none to 1 full (mappable, per point): an angle-dependent sheen the hair shading "
                      "mixes in (the iridescence channel)",
                      "fraction"}
                .range(0, 1)
                .in("Iridescence"),
            ParamSpec{"iridescenceColor", Vec3f(0.05f, 0.55f, 0.2f),
                      "The structural color seen face-on (along the strand's normal: a feather's vane), linear RGB. "
                      "Groom-wide: the first FeatherColor with iridescence sets it for the whole groom", "color"}
                .in("Iridescence"),
            ParamSpec{"iridescenceShift", 0.3, "How far its hue turns from face-on to edge-on, in turns (+/-). Groom-wide, "
                                               "as iridescenceColor", "turns"}
                .soft(-1, 1)
                .in("Iridescence"),
            ParamSpec{"iridescenceOn", std::string("all"),
                      "Where the iridescence shows: everywhere, on one vane of a feather, or where the pattern is (or is "
                      "not)",
                      "", {"all", "leftVane", "rightVane", "pattern", "notPattern"}}
                .in("Iridescence"),
            notAnimatable({"seed", std::int64_t{0}, "Random seed"}).in("Advanced").adv(),
        };
    }
    // A rewritten feather image re-evaluates the node (the texture cache re-reads it).
    std::uint64_t cacheSalt() const override {
        std::uint64_t salt = 0;
        for (const char* name : {"texture", "textureRight"}) {
            const std::string path = params().getString(name);
            if (!path.empty()) salt = hashCombine(salt, fileStamp(resolvePath(path)));
        }
        return salt;
    }
    Value evaluate(const EvalContext& ctx, const std::vector<Value>& inputs) const override {
        Value out;
        run(ctx, inputs, nullptr, &out, nullptr);
        return out;
    }
    bool evaluateIncremental(const EvalContext& ctx, const std::vector<Value>& inputs, const IncrementalInputs& incremental,
                             Value* out, StrandChanges* changed) const override {
        return run(ctx, inputs, &incremental, out, changed);
    }

private:
    // A feather image by param name: null when the param is empty.
    TexturePtr featherImage(const char* name) const {
        const std::string path = params().getString(name);
        if (path.empty()) return nullptr;
        try {
            return loadTexture(resolvePath(path));
        } catch (const TextureError& e) {
            throw GraphError(std::string(op::kFeatherColor) + ": " + name + ": " + e.what());
        }
    }

    bool run(const EvalContext& ctx, const std::vector<Value>& inputs, const IncrementalInputs* incremental, Value* result,
             StrandChanges* changed) const {
        const StrandsPtr in = requireStrands(inputs, op::kFeatherColor);
        const ScalpMesh* scalp = scalpOf(ctx);
        const std::string where = std::string(op::kFeatherColor) + ": ";
        VanePatternParams pattern;
        if (!parseVanePattern(params().getString("pattern"), &pattern.kind)) {
            throw GraphError(where + "pattern must be one of none, texture, bars, chevrons, spots, fringe, eye");
        }
        VaneSides patternVanes = VaneSides::Both;
        if (!parseVaneSides(params().getString("vanes"), &patternVanes)) throw GraphError(where + "vanes must be both, left or right");
        const VaneSpace vanes(*in);
        const bool patterned = pattern.kind != VanePattern::None && vanes.available();
        const float strength = static_cast<float>(params().getDouble("strength"));
        const float patternMelanin = static_cast<float>(params().getDouble("melanin"));
        const float patternRedness = static_cast<float>(params().getDouble("redness"));
        const ColorField patternDye(params(), "dye"), downColorField(params(), "downColor");
        patternDye.validate(*in);
        downColorField.validate(*in);
        pattern.count = static_cast<float>(params().getDouble("count"));
        pattern.width = static_cast<float>(params().getDouble("width"));
        pattern.angle = static_cast<float>(params().getDouble("angle")) * kDegrees;
        pattern.sharpness = static_cast<float>(params().getDouble("sharpness"));
        pattern.offset = static_cast<float>(params().getDouble("offset"));
        pattern.profile = params().getRamp("profile");
        pattern.jitter = static_cast<float>(params().getDouble("offsetRandom"));
        pattern.scaleJitter = static_cast<float>(params().getDouble("scaleRandom"));
        pattern.rotationJitter = static_cast<float>(params().getDouble("angleRandom")) * kDegrees;
        pattern.seed = static_cast<std::uint64_t>(params().getInt("seed"));
        TexturePtr featherLeft, featherRight;
        if (patterned && pattern.kind == VanePattern::Texture) {
            featherLeft = featherImage("texture");
            featherRight = featherImage("textureRight");
            if (!featherLeft) throw GraphError(where + "pattern texture needs a texture image");
        }
        const float downStrength = static_cast<float>(params().getDouble("downStrength"));
        const std::vector<float>* downChannel = in->strandChannel(kIsDownChannel);
        const bool downColored = downStrength > 0.0f && downChannel && downChannel->size() == in->strandCount();

        // Iridescence: a per-point channel of how much.
        const ScalarField iridescenceField(params(), "iridescence", scalp);
        iridescenceField.validate(*in);
        const bool iridescent = !iridescenceField.isConstant() || iridescenceField.at(*in, 0) > 0.0f;
        const std::string iridescenceOn = params().getString("iridescenceOn");
        if (iridescenceOn != "all" && iridescenceOn != "leftVane" && iridescenceOn != "rightVane" &&
            iridescenceOn != "pattern" && iridescenceOn != "notPattern") {
            throw GraphError(where + "iridescenceOn must be all, leftVane, rightVane, pattern or notPattern");
        }
        const bool recolors = patterned || downColored;

        const StrandChanges* changes = nullptr;
        StrandsPtr last;
        if (incremental) {
            changes = someStrandsChanged(*incremental, 0);
            if (!changes) return false;
            if (!recolors && !iridescent) {  // nothing to do: the input as it came
                *result = inputs[0];
                *changed = *changes;
                return true;
            }
            last = previousStrandsFor(*incremental, *in);
            if (!last || !iridescenceField.strandLocal() || !patternDye.strandLocal() || !downColorField.strandLocal()) return false;
            if (changes->none()) {
                *result = last;
                *changed = *changes;
                return true;
            }
        }
        if (!recolors && !iridescent) {
            *result = inputs[0];
            return true;
        }
        const IncomingColors incoming(*in);
        auto out = std::make_shared<StrandSet>(*in);
        std::unique_ptr<RecolorChannels> colors;
        if (recolors) {
            colors = std::make_unique<RecolorChannels>(*out, *in, last, false);
            if (!colors->complete(false)) return false;
        }
        std::vector<float>* iridescenceOut = nullptr;
        if (iridescent) {
            iridescenceOut = last ? out->copyPointChannelFrom(kIridescenceChannel, *last) : &out->addPointChannel(kIridescenceChannel, 0.0f);
            if (!iridescenceOut) return false;
        }

        auto recolor = [&](std::size_t s) {
            const std::size_t n = out->strandPointCount(s), begin = out->strandBegin(s);
            const Rgb own = incoming.strand(s);
            const Rgb patternColor = melaninColor(patternMelanin, patternRedness, patternDye.at(*out, s));
            const Rgb downColor = downColorField.at(*out, s);
            const bool feather = vanes.available() && vanes.inFeather(s);
            const std::uint64_t key = feather ? vanes.featherKey(s) : 0;
            const FeatherJitter jitter = feather && patterned ? featherJitter(pattern, key) : FeatherJitter{};
            // The pattern's weight and color at point i (0: none).
            auto patternAt = [&](std::size_t i, Rgb* tint) {
                if (!feather || !patterned) return 0.0f;
                const VanePoint vp = vanes.at(s, i, n);
                if (!onVane(patternVanes, vp.side)) return 0.0f;
                if (pattern.kind == VanePattern::Texture) {
                    const bool perSide = featherRight != nullptr;
                    const Texture* image = perSide && vp.side > 0 ? featherRight.get() : featherLeft.get();
                    const std::array<float, 2> st = vaneTextureCoordinates(vp, perSide);
                    // Kept within the edge texels' centers: the image does not wrap round the feather.
                    const float hs = 0.5f / static_cast<float>(std::max(image->width(), 1));
                    const float ht = 0.5f / static_cast<float>(std::max(image->height(), 1));
                    const std::array<float, 4> t = image->sample(std::clamp(st[0], hs, 1.0f - hs), std::clamp(st[1], ht, 1.0f - ht));
                    *tint = {t[0], t[1], t[2]};
                    return strength * std::clamp(t[3], 0.0f, 1.0f);
                }
                *tint = patternColor;
                return strength * vanePatternMask(pattern, vp, jitter, key);
            };
            if (colors) {
                const bool down = downColored && (*downChannel)[s] > 0.5f;
                for (std::size_t i = 0; i < n; ++i) {
                    Rgb c = incoming.point(s, i, own);
                    if (patterned) {
                        Rgb tint{};
                        const float w = patternAt(i, &tint);
                        if (w > 0.0f) {
                            for (int k = 0; k < 3; ++k) c[k] += (tint[k] - c[k]) * w;
                        }
                    }
                    if (down) {
                        for (int k = 0; k < 3; ++k) c[k] += (downColor[k] - c[k]) * downStrength;
                    }
                    colors->setPoint(begin + i, c);
                }
            }
            if (iridescenceOut) {
                for (std::size_t i = 0; i < n; ++i) {
                    float on = 1.0f;
                    if (iridescenceOn != "all") {
                        if (!feather) {
                            on = iridescenceOn == "notPattern" ? 1.0f : 0.0f;
                        } else if (iridescenceOn == "leftVane" || iridescenceOn == "rightVane") {
                            const int side = vanes.at(s, i, n).side;
                            on = (iridescenceOn == "leftVane" ? side < 0 : side > 0) ? 1.0f : 0.0f;
                        } else {
                            Rgb tint{};
                            const float w = patterned ? std::clamp(patternAt(i, &tint), 0.0f, 1.0f) : 0.0f;
                            on = iridescenceOn == "pattern" ? w : 1.0f - w;
                        }
                    }
                    (*iridescenceOut)[begin + i] = std::clamp(iridescenceField.atPoint(*out, s, i), 0.0f, 1.0f) * on;
                }
            }
        };
        if (last) {
            forEachListed(changes->strands, recolor);
            *changed = *changes;
        } else {
            parallelFor(0, out->strandCount(), recolor);
        }
        *result = StrandsPtr(out);
        return true;
    }
};

}  // namespace

void registerHairColorOperator(OperatorRegistry& registry) {
    registry.registerType(op::kHairColor,
                          {"color",
                           "The hair's look: pigment and dye, a map, greys, randomness, a tip color and the highlight (specular, roughness, tilt).",
                           "melanin pigment dye shading specular roughness"},
                          [] { return std::make_unique<HairColorOp>(); });
}

void registerBandsOperator(OperatorRegistry& registry) {
    registry.registerType(op::kBands,
                          {"color",
                           "Bands of another pigment along each hair, on the guard hairs: agouti, ticked, shaded fur. After HairColor.",
                           "agouti ticked stripes"},
                          [] { return std::make_unique<BandsOp>(); });
}

void registerFeatherColorOperator(OperatorRegistry& registry) {
    registry.registerType(op::kFeatherColor,
                          {"color",
                           "Patterns on feathers (bars, spots, an eye, a painted image), a down color and iridescence. After HairColor.",
                           "pattern bars spots iridescence"},
                          [] { return std::make_unique<FeatherColorOp>(); });
}

}  // namespace groomlab
