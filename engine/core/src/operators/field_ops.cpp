// SPDX-License-Identifier: Apache-2.0
// Field operators (PG-7): nodes that output a value per strand (PortType::Field)
// for exposed params. A Map node turns a painted map (or an image) into a node, an
// Expression node a formula; FieldNoise, FieldRemap and FieldCombine build on fields,
// so one map or noise can drive many params, reshaped per param.

#include <algorithm>
#include <cmath>

#include "groomlab/expression.h"
#include "groomlab/field.h"
#include "groomlab/map_ref.h"
#include "groomlab/graph.h"
#include "groomlab/guide_hierarchy.h"
#include "groomlab/noise.h"
#include "groomlab/ramp.h"
#include "groomlab/operators.h"
#include "groomlab/paths.h"
#include "groomlab/random.h"
#include "groomlab/texture.h"
#include "internal.h"

namespace groomlab {

namespace {

const ScalpMesh* scalpIn(const EvalContext& ctx, MeshPtr* keep) {
    *keep = valueAs<MeshPtr>(ctx.external("scalp"));
    return keep->get();
}

// Root position on the rest scalp (stable while it deforms), else the current one.
Vec3f rootPosition(const ScalpMesh* scalp, const StrandRoot& root) {
    if (!scalp || !scalp->contains(root)) return {};
    return scalp->restPositionOr(root, scalp->position(root));
}

FieldPtr requireField(const std::vector<Value>& inputs, std::size_t slot, const char* op, const char* port) {
    FieldPtr f = slot < inputs.size() ? valueAs<FieldPtr>(inputs[slot]) : nullptr;
    if (!f) throw GraphError(std::string(op) + ": connect a field to '" + port + "'");
    return f;
}

// ---------------------------------------------------------------------------

class MapFieldValue : public FieldValue {
public:
    MapFieldValue(const ParamSet& params, MeshPtr scalp, GuidesPtr guides)
        : scalp_(std::move(scalp)), guides_(std::move(guides)),
          field_(params, "map", scalp_.get(), guides_.get()) {}
    float atRoot(const StrandRoot& root) const override { return field_.atRoot(root); }
    float at(const StrandSet& strands, std::size_t s) const override { return field_.at(strands, s); }
    float atPoint(const StrandSet& strands, std::size_t s, std::size_t point) const override {
        return field_.atPoint(strands, s, point);
    }
    bool perPoint() const override { return field_.perPoint(); }
    void validate(const StrandSet& strands) const override { field_.validate(strands); }
    float maximum() const override { return field_.maximum(); }
    bool readsPositions() const override { return field_.readsPositions(); }
    bool strandLocal() const override { return field_.strandLocal(); }
    float longestRead() const override { return field_.longestRead(); }
    const ScalarField* scalarField() const override { return &field_; }

private:
    MeshPtr scalp_;  // kept alive: the field reads them
    GuidesPtr guides_;
    ScalarField field_;
};

// A Map's rgb output: the map's color (its r, g and b, each remapped to min..max) for a color
// param it is plugged into (ColorField), its luminance for a number param.
class MapColorFieldValue : public FieldValue {
public:
    MapColorFieldValue(const MapRef& map, MeshPtr scalp, GuidesPtr guides) : scalp_(std::move(scalp)), guides_(std::move(guides)) {
        for (int k = 0; k < 4; ++k) {
            MapRef channel = map;
            channel.component = k == 0 ? MapRef::Component::R : k == 1 ? MapRef::Component::G
                                : k == 2                    ? MapRef::Component::B
                                                            : MapRef::Component::Luminance;
            ParamSet values;
            values.set("map", channel);
            fields_.push_back(std::make_unique<ScalarField>(values, "map", scalp_.get(), guides_.get()));
        }
    }
    float atRoot(const StrandRoot& root) const override { return fields_[3]->atRoot(root); }
    float at(const StrandSet& strands, std::size_t s) const override { return fields_[3]->at(strands, s); }
    float atPoint(const StrandSet& strands, std::size_t s, std::size_t point) const override {
        return fields_[3]->atPoint(strands, s, point);
    }
    bool perPoint() const override { return fields_[3]->perPoint(); }
    void validate(const StrandSet& strands) const override {
        for (const auto& f : fields_) f->validate(strands);
    }
    float maximum() const override { return fields_[3]->maximum(); }
    bool readsPositions() const override { return fields_[3]->readsPositions(); }
    bool strandLocal() const override { return fields_[3]->strandLocal(); }
    float longestRead() const override { return fields_[3]->longestRead(); }
    const ScalarField* scalarField() const override { return fields_[3].get(); }
    bool isColor() const override { return true; }
    std::array<float, 3> colorAt(const StrandSet& strands, std::size_t s) const override {
        return {fields_[0]->at(strands, s), fields_[1]->at(strands, s), fields_[2]->at(strands, s)};
    }
    std::array<float, 3> colorAtPoint(const StrandSet& strands, std::size_t s, std::size_t point) const override {
        return {fields_[0]->atPoint(strands, s, point), fields_[1]->atPoint(strands, s, point),
                fields_[2]->atPoint(strands, s, point)};
    }

private:
    MeshPtr scalp_;  // kept alive: the fields read them
    GuidesPtr guides_;
    std::vector<std::unique_ptr<ScalarField>> fields_;  // r, g, b, luminance
};

// A map painted on the scalp as a node (Groom Tools > Paint), found by name: the host's map
// catalog (external "maps") says whether it is a layer on the guides, a value per vertex (a
// color set of the scalp of that name) or an image at the scalp's UVs (and where); without a
// catalog entry, a color set of the name on the scalp, else a layer of the name on the guides,
// else an image file of that name (a path) read at the scalp's UVs. Any
// mappable param takes it through an exposed input, where it multiplies the param's own value.
// Six outputs from one evaluation: the map's luminance (the first: what a link naming no
// output reads), each channel r, g, b, a of a color map (masks packed in one map), and rgb, its
// color, which a color param takes (ParamSpec::colorField: HairColor's color, dye, tipColor...;
// a number param takes its luminance); a gray map gives its value on luminance, r, g, b and rgb
// and 1 on a, a guide layer its value on all six.
class MapOp : public Operator {
public:
    std::string typeName() const override { return op::kMap; }
    std::string category() const override { return "field"; }
    // The guides and hierarchy join the key only when the map is read from them (contextSalt), so
    // a guide stroke does not re-evaluate a vertex or texture map and all it feeds.
    std::vector<std::string> externalInputs() const override { return {"scalp", "maps"}; }
    std::vector<PortSpec> inputs() const override { return {}; }
    PortSpec output() const override { return {"luminance", PortType::Field}; }
    std::vector<PortSpec> outputs() const override {
        return {output(), {"r", PortType::Field}, {"g", PortType::Field}, {"b", PortType::Field}, {"a", PortType::Field},
                {"rgb", PortType::Field}};
    }
    // What the map is read from joins the key: a texture's file stamp (a rewritten texture
    // re-evaluates the node and what it feeds, as a texture map's does), a vertex map's color set
    // (the scalp's key leaves color sets out), a guide layer's guides and hierarchy. Without a kind
    // in the catalog the scalp is looked at first (its color set of the name), then the guides.
    std::uint64_t contextSalt(const EvalContext& ctx) const override {
        const MapInfo* info = lookup(ctx);
        const std::string kind = info ? info->kind : "";
        if (kind == "texture") return info->texture.empty() ? 0 : fileStamp(resolvePath(info->texture));
        const std::string name = params().getString("name");
        if (!info && isImagePath(name)) return fileStamp(resolvePath(name));
        std::uint64_t salt = 0;
        if (kind.empty() || kind == "vertices") {
            salt = colorSetSalt(ctx, name);
            const ScalpMesh* scalp = scalpOf(ctx);
            if (kind == "vertices" || (scalp && scalp->hasColorSet(name))) return salt;
        }
        for (const char* external : {"guides", "hierarchy"}) {
            const ExternalInputs::Entry* e = ctx.externals ? ctx.externals->find(external) : nullptr;
            salt = hashCombine(hashString(salt, external), e ? e->hash : 0);
        }
        return salt;
    }
    std::vector<ParamSpec> paramSpecs() const override {
        return {
            {"name", std::string("mask"), "The map (Groom Tools > Paint lists them): a layer on the guides, a value per "
                                          "scalp vertex or a texture at the scalp's UVs, found by its name; or an image "
                                          "file's path, read at the scalp's UVs"},
            ParamSpec{"min", 0.0, "Output where the map is black: what its 0 becomes (an output range, not a limit; "
                                  "above max inverts the map)"}
                .soft(0, 1),
            ParamSpec{"max", 1.0, "Output where the map is white: what its 1 becomes (an output range, not a limit)"}
                .soft(0, 1),
        };
    }
    Value evaluate(const EvalContext& ctx, const std::vector<Value>& inputs) const override {
        return evaluateOutputs(ctx, inputs).front();
    }
    std::vector<Value> evaluateOutputs(const EvalContext& ctx, const std::vector<Value>&) const override {
        MeshPtr scalp;
        scalpIn(ctx, &scalp);
        const std::string name = params().getString("name");
        if (name.empty()) throw GraphError("Map: name the map (Groom Tools > Paint)");
        auto carries = [&](const GuideSet& set) {
            for (const Guide& g : set.guides()) {
                if (g.attributes.count(name) || g.pointAttributes.count(name)) return true;
            }
            return false;
        };
        // The guides carrying the layer: the flat guides (curves), else the groom's hierarchy
        // (children inherit their parents' layers when it flattens).
        GuidesPtr guides = valueAs<GuidesPtr>(ctx.external("guides"));
        if (!guides || !carries(*guides)) {
            if (const HierarchyPtr h = valueAs<HierarchyPtr>(ctx.external("hierarchy"))) {
                auto flat = std::make_shared<const GuideSet>(h->flatten());
                if (carries(*flat) || !guides) guides = flat;
            }
        }
        const bool hasLayer = guides && carries(*guides);
        const MapInfo* info = lookup(ctx);
        std::string kind = info ? info->kind : "";
        if (kind.empty()) {
            kind = scalp && scalp->hasColorSet(name) ? "vertices" : hasLayer ? "guides" : isImagePath(name) ? "image" : "";
        }
        MapRef map;
        if (kind == "image") {
            map.source = MapRef::Source::Texture;
            map.name = name;
        } else if (kind == "texture") {
            map.source = MapRef::Source::Texture;
            map.name = info ? info->texture : std::string();
            if (map.name.empty()) throw GraphError("Map '" + name + "': the texture map has no image file yet (paint it in Groom Tools > Paint)");
        } else if (kind == "vertices") {
            map.source = MapRef::Source::ColorSet;
            map.name = name;
            if (!scalp || !scalp->hasColorSet(name)) {
                throw GraphError("Map '" + name + "': the vertex map does not fit this scalp (paint it again, or convert it, in Groom Tools > Paint)");
            }
        } else if (kind == "guides") {
            map.source = MapRef::Source::Guide;
            map.name = name;
            if (!hasLayer) throw GraphError("Map '" + name + "': no guide carries the layer (paint it in Groom Tools > Paint)");
        } else {
            throw GraphError("Map: no map or image named '" + name + "' (Groom Tools > Paint > New... makes a map)");
        }
        map.min = static_cast<float>(params().getDouble("min"));
        map.max = static_cast<float>(params().getDouble("max"));
        // One field per output, each reading its channel of a color map (a texture's, or the vertex
        // map's color set); a guide layer is gray.
        std::vector<Value> out;
        for (const auto component : {MapRef::Component::Luminance, MapRef::Component::R, MapRef::Component::G,
                                     MapRef::Component::B, MapRef::Component::A}) {
            map.component = component;
            ParamSet values;
            values.set("map", map);
            out.emplace_back(FieldPtr(std::make_shared<MapFieldValue>(values, scalp, kind == "guides" ? guides : nullptr)));
        }
        out.emplace_back(FieldPtr(std::make_shared<MapColorFieldValue>(map, scalp, kind == "guides" ? guides : nullptr)));
        return out;
    }

private:
    const MapInfo* lookup(const EvalContext& ctx) const {
        const MapCatalogPtr catalog = valueAs<MapCatalogPtr>(ctx.external("maps"));
        if (!catalog) return nullptr;
        const auto it = catalog->maps.find(params().getString("name"));
        return it == catalog->maps.end() ? nullptr : &it->second;
    }
};

// A per-strand expression as a node, for any exposed param: e.g. "$length / $longest"
// to clump the long strands, "smoothstep(0.3, 0.8, $guideInfluence)", "1 - $straightness".
// A block of lines with locals and the node's own variables (numbers, vectors) the editor
// shows as sliders.
class ExpressionOp : public Operator {
public:
    std::string typeName() const override { return op::kExpression; }
    std::string category() const override { return "field"; }
    // The scalp, and the host's controls its keyable variables read (their changes re-evaluate it).
    // (One value: no scalp, so a deforming scalp does not re-evaluate it and all it drives.)
    std::vector<std::string> externalInputs() const override {
        std::vector<std::string> out;
        if (!givesValue()) out.push_back("scalp");
        for (const auto& [variable, control] : expressionVariableControls(params().getString("variables"))) {
            out.push_back(controlInputName(control));
        }
        return out;
    }
    std::vector<PortSpec> inputs() const override { return {}; }
    PortSpec output() const override {
        if (givesValue()) return {"value", PortType::Value, false, ValueKind::Number};
        return {"field", PortType::Field};
    }
    std::vector<ParamSpec> paramSpecs() const override {
        return {
            ParamSpec{"output", std::string("field"),
                      "field: a value per strand (and per point) for a mappable param; value: one number for the whole node, "
                      "of the frame, the time and the node's own variables, for any param (a count, a seed, a switch...)",
                      "",
                      {"field", "value"}}
                .noInput(),
            {"expression", std::string("$length / $longest"),
             "Per strand: length, longest, reach, straightness, lift, tipx/y/z, u, v, x, y, z, nx, ny, nz, rand, "
             "index, any strand channel (e.g. clumpId, guideInfluence); per point: t (root 0 to tip 1 by arc "
             "length), pt (by point index), px/py/pz, pointIndex, any point channel; frame, time; vectors [x, y, z], "
             "P, N, tip, pointP, .x .y .z; + - * / ^, comparisons, c ? a : b; sin cos abs sqrt min max clamp mix "
             "smoothstep fit remap step, dot cross length normalize rotate, noise fbm turbulence voronoi cellnoise "
             "vnoise, curve spline, pick choose cycle, map(\"name\") ..."},
            ParamSpec{"min", 0.0, "Output where the expression is 0: what its 0 becomes (an output range, not a limit)"}
                .soft(0, 1),
            ParamSpec{"max", 1.0, "Output where the expression is 1: what its 1 becomes (in between: linear, and beyond "
                                  "it too; an output range, not a limit)"}
                .soft(0, 1),
            {"variables", std::string("{}"),
             "The node's own variables as JSON, numbers and vectors: {\"amount\": 0.5, \"center\": [0, 1, 0]}, read "
             "as $amount and $center.x (.y, .z); a keyable number is {\"value\": 0.5, \"control\": \"name\"}, set by "
             "the host's control of that name (ctl_name on the groom in Maya); a number measuring something says so, "
             "{\"value\": 2, \"unit\": \"length\"} (length, width, density, perLength), so presets fit it to the scalp"},
        };
    }
    Value evaluate(const EvalContext& ctx, const std::vector<Value>&) const override {
        MapRef map;
        map.source = MapRef::Source::Expression;
        map.name = params().getString("expression");
        if (map.name.empty()) throw GraphError("Expression: the expression is empty");
        map.min = static_cast<float>(params().getDouble("min"));
        map.max = static_cast<float>(params().getDouble("max"));
        const std::string variables = params().getString("variables");
        try {
            map.constants = parseExpressionVariables(variables);
        } catch (const ExpressionError& e) {
            throw GraphError(std::string("Expression: ") + e.what());
        }
        // Keyable variables: the control's value on this frame, where the host sets one.
        const auto controls = expressionVariableControls(variables);
        for (const auto& [variable, control] : controls) {
            const FieldPtr set = valueAs<FieldPtr>(ctx.external(controlInputName(control)));
            if (!set) continue;
            for (auto& [name, value] : map.constants) {
                if (name == variable) value = set->maximum();
            }
        }
        if (givesValue()) return uniformValue(map, !controls.empty());
        MeshPtr scalp;
        scalpIn(ctx, &scalp);
        ParamSet values;
        values.set("map", map);
        return FieldPtr(std::make_shared<MapFieldValue>(values, scalp, nullptr));
    }

private:
    bool givesValue() const { return params().getString("output") == "value"; }

    // The expression as one number: of the frame, the time and the node's own variables only (a
    // strand's, a point's, a channel, a map or rand() is an error: there is no strand to read).
    // Changes over time when it reads the time or a keyable variable.
    static Value uniformValue(const MapRef& map, bool keyable) {
        Expression expression;
        try {
            expression = Expression(map.name, map.constants, ExpressionType::Number);
        } catch (const ExpressionError& e) {
            throw GraphError(std::string("Expression: ") + e.what());
        }
        const int frame = Expression::variableCode("frame"), time = Expression::variableCode("time");
        const int perStrand[] = {Expression::functionCode("rand"), Expression::functionCode("rand range"),
                                 Expression::functionCode("map"), Expression::functionCode("cmap")};
        for (const Expression::Op& op : expression.program()) {
            const bool variable = op.kind == Expression::Op::Variable && op.code != frame && op.code != time;
            const bool call = (op.kind == Expression::Op::Call || op.kind == Expression::Op::VCall) &&
                              std::find(std::begin(perStrand), std::end(perStrand), static_cast<int>(op.code)) != std::end(perStrand);
            if (variable || call || op.kind == Expression::Op::Channel) {
                throw GraphError("Expression: set to give one value, it reads only frame, time and its own variables (no "
                                 "strand, point, channel, map or rand: those are per strand; set it to give a field)");
            }
        }
        const float v = expression.evaluateAt(StrandRoot{}, nullptr);
        const double value = static_cast<double>(map.min) + (static_cast<double>(map.max) - map.min) * static_cast<double>(v);
        return UniformPtr(std::make_shared<const UniformValue>(UniformValue{value, expression.readsTime() || keyable}));
    }
};

// One value for a whole node (a Value port): a number, an integer, a switch, a vector, a color or
// text, plugged into any exposed param that takes no field (a count, a seed, a choice...). Keyed (a
// keyable param in Maya), it changes over time, which a param that sets how many strands there are
// refuses.
class ValueOp : public Operator {
public:
    std::string typeName() const override { return op::kValue; }
    std::string category() const override { return "utility"; }
    std::vector<PortSpec> inputs() const override { return {}; }
    PortSpec output() const override { return {"value", PortType::Value, false, kindOf(params().getString("kind"))}; }
    std::vector<ParamSpec> paramSpecs() const override {
        return {
            ParamSpec{"kind", std::string("number"),
                      "What it gives: a number, an integer, a switch (bool), a vector, a color or text (text also feeds a "
                      "param's choices: one of them, by name)",
                      "",
                      {"number", "integer", "bool", "vector", "color", "text"}}
                .noInput(),
            ParamSpec{"number", 0.0, "The number"}.soft(0, 1).when("kind", {"number"}),
            ParamSpec{"integer", std::int64_t{0}, "The whole number"}.soft(0, 10).when("kind", {"integer"}),
            ParamSpec{"on", false, "The switch"}.when("kind", {"bool"}),
            ParamSpec{"vector", Vec3f(0.0f, 1.0f, 0.0f), "The vector"}.when("kind", {"vector"}),
            ParamSpec{"color", Vec3f(0.5f, 0.5f, 0.5f), "The color (linear RGB)", "color"}.when("kind", {"color"}),
            ParamSpec{"text", std::string(), "The text: a name, or one of a param's choices"}.when("kind", {"text"}),
        };
    }
    Value evaluate(const EvalContext&, const std::vector<Value>&) const override {
        const std::string kind = params().getString("kind");
        ParamValue value = params().getDouble("number");
        if (kind == "integer") value = params().getInt("integer");
        else if (kind == "bool") value = params().getBool("on");
        else if (kind == "vector") value = params().getVec3("vector");
        else if (kind == "color") value = params().getVec3("color");
        else if (kind == "text") value = params().getString("text");
        return UniformPtr(std::make_shared<const UniformValue>(UniformValue{std::move(value), false}));
    }

private:
    static ValueKind kindOf(const std::string& kind) {
        if (kind == "integer") return ValueKind::Integer;
        if (kind == "bool") return ValueKind::Bool;
        if (kind == "vector" || kind == "color") return ValueKind::Vec3;
        if (kind == "text") return ValueKind::Text;
        return ValueKind::Number;
    }
};

// ---------------------------------------------------------------------------

class ConstantFieldValue : public FieldValue {
public:
    explicit ConstantFieldValue(float value) : value_(value) {}
    float atRoot(const StrandRoot&) const override { return value_; }
    float at(const StrandSet&, std::size_t) const override { return value_; }
    float maximum() const override { return value_; }
    bool readsPositions() const override { return false; }
    bool strandLocal() const override { return true; }

private:
    float value_;
};

// A value the host sets by name: in Maya, a keyable attribute on the groom
// (named after the control), so a look can be dialed or animated, e.g. a
// Blend's amount from dry to wet. The default where the host sets none.
class ControlOp : public Operator {
public:
    std::string typeName() const override { return op::kControl; }
    std::string category() const override { return "field"; }
    std::vector<std::string> externalInputs() const override {
        return {controlInputName(params().getString("name"))};
    }
    std::vector<PortSpec> inputs() const override { return {}; }
    PortSpec output() const override { return {"field", PortType::Field}; }
    std::vector<ParamSpec> paramSpecs() const override {
        return {
            {"name", std::string("control"), "The attribute's name on the groom (letters, digits, _)"},
            ParamSpec{"default", 0.0, "Value until the attribute is set"}.soft(0, 1),
            ParamSpec{"min", 0.0, "The low end of the attribute's slider in hosts (only the slider's range: values "
                                  "past it are not clamped)"}
                .soft(0, 1),
            ParamSpec{"max", 1.0, "The high end of the attribute's slider in hosts (only the slider's range: values "
                                  "past it are not clamped)"}
                .soft(0, 1),
        };
    }
    Value evaluate(const EvalContext& ctx, const std::vector<Value>&) const override {
        if (FieldPtr set = valueAs<FieldPtr>(ctx.external(controlInputName(params().getString("name"))))) {
            return set;
        }
        return constantField(static_cast<float>(params().getDouble("default")));
    }
};

// ---------------------------------------------------------------------------

class NoiseFieldValue : public FieldValue {
public:
    NoiseFieldValue(MeshPtr scalp, float frequency, Vec3f offset, std::uint64_t seed, int octaves, float lo, float hi)
        : scalp_(std::move(scalp)), frequency_(frequency), offset_(offset), seed_(seed), octaves_(octaves),
          lo_(lo), hi_(hi) {}
    float atRoot(const StrandRoot& root) const override {
        const Vec3f p = rootPosition(scalp_.get(), root) * frequency_ + offset_;
        const float n = std::clamp(0.5f + 0.5f * fractalNoise(p, seed_, octaves_), 0.0f, 1.0f);
        return lo_ + (hi_ - lo_) * n;
    }
    float at(const StrandSet& strands, std::size_t s) const override { return atRoot(strands.roots()[s]); }
    float maximum() const override { return std::max(lo_, hi_); }
    bool readsPositions() const override { return false; }  // the roots on the scalp
    bool strandLocal() const override { return true; }

private:
    MeshPtr scalp_;
    float frequency_;
    Vec3f offset_;
    std::uint64_t seed_;
    int octaves_;
    float lo_, hi_;
};

// Smooth 3D noise over the scalp (at each root, rest pose), from min to max.
class FieldNoiseOp : public Operator {
public:
    std::string typeName() const override { return op::kFieldNoise; }
    std::string category() const override { return "field"; }
    std::vector<std::string> externalInputs() const override { return {"scalp"}; }
    std::vector<PortSpec> inputs() const override { return {}; }
    PortSpec output() const override { return {"field", PortType::Field}; }
    std::vector<ParamSpec> paramSpecs() const override {
        return {
            ParamSpec{"frequency", 2.0, "Features per unit", "perLength"}.soft(0, 10),
            ParamSpec{"octaves", std::int64_t{2}, "Layers of finer detail"}.range(1, 8).adv(),
            {"offset", Vec3f{0, 0, 0}, "Moves the pattern, in features (it is added after the frequency: unitless)"},
            notAnimatable({"seed", std::int64_t{0}, "Random seed"}).adv(),
            ParamSpec{"min", 0.0, "Output in the troughs: what the noise's 0 becomes (an output range, not a limit)"}
                .soft(0, 1),
            ParamSpec{"max", 1.0, "Output at the peaks: what the noise's 1 becomes (an output range, not a limit)"}
                .soft(0, 1),
        };
    }
    Value evaluate(const EvalContext& ctx, const std::vector<Value>&) const override {
        MeshPtr scalp;
        scalpIn(ctx, &scalp);
        const int octaves = static_cast<int>(params().getInt("octaves"));
        return FieldPtr(std::make_shared<NoiseFieldValue>(
            scalp, static_cast<float>(params().getDouble("frequency")), params().getVec3("offset"),
            static_cast<std::uint64_t>(params().getInt("seed")), octaves,
            static_cast<float>(params().getDouble("min")), static_cast<float>(params().getDouble("max"))));
    }
};

// ---------------------------------------------------------------------------

class RemapFieldValue : public FieldValue {
public:
    RemapFieldValue(FieldPtr in, float inLo, float inHi, Ramp curve, float outLo, float outHi)
        : in_(std::move(in)), inLo_(inLo), inHi_(inHi), curve_(std::move(curve)), outLo_(outLo), outHi_(outHi) {}
    float map(float v) const {
        float t = inHi_ != inLo_ ? (v - inLo_) / (inHi_ - inLo_) : 0.0f;
        t = std::clamp(t, 0.0f, 1.0f);
        return outLo_ + (outHi_ - outLo_) * curve_(t);
    }
    float atRoot(const StrandRoot& root) const override { return map(in_->atRoot(root)); }
    float at(const StrandSet& strands, std::size_t s) const override { return map(in_->at(strands, s)); }
    float atPoint(const StrandSet& strands, std::size_t s, std::size_t point) const override {
        return map(in_->atPoint(strands, s, point));
    }
    bool perPoint() const override { return in_->perPoint(); }
    void validate(const StrandSet& strands) const override { in_->validate(strands); }
    bool readsPositions() const override { return in_->readsPositions(); }
    bool strandLocal() const override { return in_->strandLocal(); }
    float longestRead() const override { return in_->longestRead(); }
    // The largest output: the curve at its keys (it never overshoots them) and at
    // samples between, mapped to [outMin, outMax] whichever way round they are.
    float maximum() const override {
        float m = out(curve_(0.0f));
        for (const auto& key : curve_.keys) m = std::max(m, out(curve_(key.first)));
        for (int i = 1; i <= 32; ++i) m = std::max(m, out(curve_(static_cast<float>(i) / 32.0f)));
        return m;
    }

private:
    // A curve value to the output range (its ends exactly).
    float out(float c) const {
        if (c == 0.0f) return outLo_;
        if (c == 1.0f) return outHi_;
        return outLo_ + (outHi_ - outLo_) * c;
    }
    FieldPtr in_;
    float inLo_, inHi_;
    Ramp curve_;
    float outLo_, outHi_;
};

// Reshapes a field: from [inMin, inMax] through a curve to [outMin, outMax] (inMin above
// inMax inverts it).
class FieldRemapOp : public Operator {
public:
    std::string typeName() const override { return op::kFieldRemap; }
    std::string category() const override { return "field"; }
    std::vector<PortSpec> inputs() const override { return {{"in", PortType::Field}}; }
    PortSpec output() const override { return {"field", PortType::Field}; }
    std::vector<ParamSpec> paramSpecs() const override {
        return {
            ParamSpec{"inMin", 0.0, "Input value mapped to the curve's start (above inMax: the input flipped)"}.soft(0, 1),
            ParamSpec{"inMax", 1.0, "Input value mapped to the curve's end"}.soft(0, 1),
            {"curve", Ramp::linear(0.0f, 1.0f), "Response curve, 0 to 1"},
            ParamSpec{"outMin", 0.0, "Output where the curve is 0"}.soft(0, 1),
            ParamSpec{"outMax", 1.0, "Output where the curve is 1"}.soft(0, 1),
        };
    }
    Value evaluate(const EvalContext&, const std::vector<Value>& inputs) const override {
        return FieldPtr(std::make_shared<RemapFieldValue>(
            requireField(inputs, 0, op::kFieldRemap, "in"), static_cast<float>(params().getDouble("inMin")),
            static_cast<float>(params().getDouble("inMax")), params().getRamp("curve"),
            static_cast<float>(params().getDouble("outMin")),
            static_cast<float>(params().getDouble("outMax"))));
    }
};

// ---------------------------------------------------------------------------

enum class Combine { Multiply, Add, Subtract, Min, Max, Mix };

class CombineFieldValue : public FieldValue {
public:
    CombineFieldValue(FieldPtr a, FieldPtr b, float bConstant, Combine mode, float mix)
        : a_(std::move(a)), b_(std::move(b)), bConstant_(bConstant), mode_(mode), mix_(mix) {}
    float combine(float a, float b) const {
        switch (mode_) {
            case Combine::Multiply: return a * b;
            case Combine::Add: return a + b;
            case Combine::Subtract: return a - b;
            case Combine::Min: return std::min(a, b);
            case Combine::Max: return std::max(a, b);
            case Combine::Mix: return a + (b - a) * mix_;
        }
        return a;
    }
    float atRoot(const StrandRoot& root) const override {
        return combine(a_->atRoot(root), b_ ? b_->atRoot(root) : bConstant_);
    }
    float at(const StrandSet& strands, std::size_t s) const override {
        return combine(a_->at(strands, s), b_ ? b_->at(strands, s) : bConstant_);
    }
    float atPoint(const StrandSet& strands, std::size_t s, std::size_t point) const override {
        return combine(a_->atPoint(strands, s, point), b_ ? b_->atPoint(strands, s, point) : bConstant_);
    }
    bool perPoint() const override { return a_->perPoint() || (b_ && b_->perPoint()); }
    bool readsPositions() const override { return a_->readsPositions() || (b_ && b_->readsPositions()); }
    bool strandLocal() const override { return a_->strandLocal() && (!b_ || b_->strandLocal()); }
    float longestRead() const override {  // (both of the same strands: the same longest)
        const float a = a_->strandLocal() ? 0.0f : a_->longestRead();
        const float b = !b_ || b_->strandLocal() ? 0.0f : b_->longestRead();
        if (a < 0.0f || b < 0.0f || strandLocal()) return -1.0f;
        return std::max(a, b);
    }
    void validate(const StrandSet& strands) const override {
        a_->validate(strands);
        if (b_) b_->validate(strands);
    }
    float maximum() const override {
        const float a = a_->maximum(), b = b_ ? b_->maximum() : bConstant_;
        switch (mode_) {
            case Combine::Multiply: return a * b;
            case Combine::Add: return a + b;
            // A field's least value isn't known: taken as 0 unless its largest is below
            // that (a number on b is exact).
            case Combine::Subtract: return b_ ? a - std::min(b, 0.0f) : a - b;
            case Combine::Min: return std::min(a, b);
            case Combine::Max:
            case Combine::Mix: return std::max(a, b);
        }
        return a;
    }

private:
    FieldPtr a_, b_;
    float bConstant_;
    Combine mode_;
    float mix_;
};

// Combines two fields (or a field and a number): multiply, add, subtract,
// min, max or mix.
class FieldCombineOp : public Operator {
public:
    std::string typeName() const override { return op::kFieldCombine; }
    std::string category() const override { return "field"; }
    std::vector<PortSpec> inputs() const override { return {{"a", PortType::Field}, {"b", PortType::Field, true}}; }
    PortSpec output() const override { return {"field", PortType::Field}; }
    std::vector<ParamSpec> paramSpecs() const override {
        return {
            {"mode", std::string("multiply"), "multiply, add, subtract, min, max or mix", "",
             {"multiply", "add", "subtract", "min", "max", "mix"}},
            ParamSpec{"b", 1.0, "The second value, while nothing is connected to the b input (a field there replaces it)"}
                .soft(0, 1),
            ParamSpec{"mix", 0.5, "mix: how much of b", "fraction"}.soft(0, 1).when("mode", {"mix"}),
        };
    }
    Value evaluate(const EvalContext&, const std::vector<Value>& inputs) const override {
        const std::string mode = params().getString("mode");
        const Combine m = mode == "add" ? Combine::Add
                          : mode == "subtract" ? Combine::Subtract
                          : mode == "min" ? Combine::Min
                          : mode == "max" ? Combine::Max
                          : mode == "mix" ? Combine::Mix
                          : mode == "multiply" ? Combine::Multiply
                                               : throw GraphError("FieldCombine: unknown mode '" + mode + "'");
        return FieldPtr(std::make_shared<CombineFieldValue>(
            requireField(inputs, 0, op::kFieldCombine, "a"), valueAs<FieldPtr>(inputs[1]),
            static_cast<float>(params().getDouble("b")), m, static_cast<float>(params().getDouble("mix"))));
    }
};

// A reusable curve for ramp params (Clump profile, Width along the length,
// FieldRemap's curve...): edit it once, plug it into several nodes. smooth
// eases between keys; flip runs it tip to root; the output runs from
// min (where the curve is 0) to max (where it is 1).
class CurveOp : public Operator {
public:
    std::string typeName() const override { return op::kCurve; }
    std::string category() const override { return "field"; }
    std::vector<PortSpec> inputs() const override { return {}; }
    PortSpec output() const override { return {"curve", PortType::Curve}; }
    std::vector<ParamSpec> paramSpecs() const override {
        return {
            {"curve", Ramp::linear(0.0f, 1.0f), "The curve, 0 to 1 along its input (root to tip for profiles)"},
            {"interpolation", std::string("smooth"),
             "How the curve runs between its keys: smooth, one smooth curve through them; ease, easing in and out of "
             "every key (flat at each key); linear, straight lines",
             "",
             {"smooth", "ease", "linear"}},
            {"flip", false, "Run the curve backwards (tip to root)"},
            ParamSpec{"min", 0.0, "Output where the curve is 0: output = min + (max - min) x curve"}.soft(-1, 1),
            ParamSpec{"max", 1.0, "Output where the curve is 1"}.soft(-1, 2),
        };
    }
    Value evaluate(const EvalContext&, const std::vector<Value>&) const override {
        const Ramp in = params().getRamp("curve");
        const std::string interpolation = params().getString("interpolation");
        const bool smooth = interpolation == "ease";  // eases between keys
        const bool linear = interpolation == "linear";
        const bool flip = params().getBool("flip");
        const float offset = static_cast<float>(params().getDouble("min"));
        const float scale = static_cast<float>(params().getDouble("max")) - offset;
        Ramp out;
        auto sample = [&](float t) {
            float x = flip ? 1.0f - t : t;
            if (smooth && in.keys.size() > 1) {
                // Smoothstep inside each span: flat tangents at the keys.
                const auto hi = std::upper_bound(in.keys.begin(), in.keys.end(), x,
                                                 [](float v, const auto& k) { return v < k.first; });
                if (hi != in.keys.begin() && hi != in.keys.end()) {
                    const auto lo = hi - 1;
                    const float span = hi->first - lo->first;
                    float f = span > 0.0f ? (x - lo->first) / span : 0.0f;
                    f = f * f * (3.0f - 2.0f * f);
                    return offset + scale * (lo->second + (hi->second - lo->second) * f);
                }
            }
            return offset + scale * in(x);
        };
        if (!smooth) {
            // The keys themselves, scaled (and mirrored with flip), joined as `linear` says.
            if (flip) {
                for (auto k = in.keys.rbegin(); k != in.keys.rend(); ++k) out.keys.emplace_back(1.0f - k->first, k->second);
            } else {
                out.keys = in.keys;
            }
            for (auto& k : out.keys) k.second = offset + scale * k.second;
            out.straight = linear;
        } else {  // smooth eases every span, in place of straight lines
            for (int i = 0; i <= 64; ++i) {
                const float t = static_cast<float>(i) / 64.0f;
                out.keys.emplace_back(t, sample(t));
            }
        }
        return CurvePtr(std::make_shared<Ramp>(std::move(out)));
    }
};

}  // namespace

FieldPtr constantField(float value) { return std::make_shared<ConstantFieldValue>(value); }

void registerCurveOperator(OperatorRegistry& registry) {
    registry.registerType(op::kCurve,
                          {"field", "A ramp shared by several params.", "ramp shared"},
                          [] { return std::make_unique<CurveOp>(); });
}

void registerExpressionOperator(OperatorRegistry& registry) {
    registry.registerType(op::kExpression,
                          {"field",
                           "A value per strand from a formula, feeding any param (or one value, of the frame and its variables).",
                           "formula code script"},
                          [] { return std::make_unique<ExpressionOp>(); });
}

void registerMapOperator(OperatorRegistry& registry) {
    registry.registerType(op::kMap,
                          {"field",
                           "A map painted in Groom Tools > Paint (or an image), feeding any param.",
                           "paint texture image weight"},
                          [] { return std::make_unique<MapOp>(); });
}

void registerControlOperator(OperatorRegistry& registry) {
    registry.registerType(op::kControl,
                          {"field", "A keyable slider on the groom (channel box) driving a param.", "slider channel box key"},
                          [] { return std::make_unique<ControlOp>(); });
}

void registerValueOperator(OperatorRegistry& registry) {
    registry.registerType(op::kValue,
                          {"utility",
                           "One value for a whole node: a number, integer, switch, vector, color or text that drives any exposed param.",
                           "number constant parameter"},
                          [] { return std::make_unique<ValueOp>(); });
}

void registerFieldNoiseOperator(OperatorRegistry& registry) {
    registry.registerType(op::kFieldNoise,
                          {"field", "Smooth noise over the scalp, feeding any param.", "noise texture variation"},
                          [] { return std::make_unique<FieldNoiseOp>(); });
}

void registerFieldRemapOperator(OperatorRegistry& registry) {
    registry.registerType(op::kFieldRemap,
                          {"field", "Reshapes a field through a curve.", "remap curve ramp"},
                          [] { return std::make_unique<FieldRemapOp>(); });
}

void registerFieldCombineOperator(OperatorRegistry& registry) {
    registry.registerType(op::kFieldCombine,
                          {"field", "Two fields multiplied, added, mixed...", "multiply add mix"},
                          [] { return std::make_unique<FieldCombineOp>(); });
}

}  // namespace groomlab
