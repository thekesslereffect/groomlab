// SPDX-License-Identifier: Apache-2.0
// What every node has besides its own params and ports: its evaluation mode (graph.h,
// EvaluationMode) and, on the nodes that reshape or recolor the strands they are fed, "affect":
// a per-strand mask the graph mixes the node's result back toward its input by (Yeti's per-node
// group restriction). A Group node (group_op.cpp) writes named masks for it to read.

#include <algorithm>
#include <cmath>
#include <set>

#include "groomlab/field.h"
#include "groomlab/graph.h"
#include "groomlab/operators.h"
#include "groomlab/parallel.h"
#include "operators/internal.h"

namespace groomlab {

const char* evaluationModeName(EvaluationMode mode) {
    switch (mode) {
        case EvaluationMode::Always: return "always";
        case EvaluationMode::ViewportOnly: return "viewportOnly";
        case EvaluationMode::RenderOnly: return "renderOnly";
    }
    return "always";
}

EvaluationMode evaluationModeFromName(const std::string& name) {
    for (const EvaluationMode mode : {EvaluationMode::Always, EvaluationMode::ViewportOnly, EvaluationMode::RenderOnly}) {
        if (name == evaluationModeName(mode)) return mode;
    }
    throw GraphError("unknown evaluation '" + name + "' (always, viewportOnly or renderOnly)");
}

bool takesAffect(const Operator& op) {
    // Nodes that make, remove, add, split or resample strands, hand them on as they are, or mix
    // two sets themselves: their result is not the same strands as their input to mix with.
    static const std::set<std::string> kNot = {
        op::kOutput, op::kResample, op::kCull,  op::kMerge,       op::kSwitch,   op::kBlend, op::kMirror,
        op::kCards,  op::kToMesh, op::kInstance, op::kScales, op::kSplit, op::kInterpolate, op::kCompound, op::kInput, op::kGroup,
    };
    if (kNot.count(op.typeName())) return false;
    const std::vector<PortSpec> inputs = op.inputs();
    if (inputs.empty()) return false;
    const PortType in = inputs[0].type;
    if ((in != PortType::Strands && in != PortType::Curves) || op.output().type != in) return false;
    return op.outputs().size() == 1;
}

std::vector<ParamSpec> commonParamSpecs(const Operator& op) {
    if (!takesAffect(op)) return {};
    return {
        ParamSpec{kAffectParam, 1.0,
                  "Where the node works, 0-1 per strand (mappable, per point): 1 its whole result, 0 the strands as they "
                  "came in, between a mix of the two. Map it to a Group node's group (an attribute map of the group's "
                  "name), a painted map or an expression. IDs (clumpId...) switch at 0.5. A node that adds, removes or "
                  "resamples strands leaves it alone",
                  "fraction"}
            .range(0, 1)
            .in("Common"),
    };
}

std::vector<ParamSpec> allParamSpecs(const Operator& op) {
    std::vector<ParamSpec> specs = op.paramSpecs();
    for (ParamSpec& common : commonParamSpecs(op)) {
        const bool own = std::any_of(specs.begin(), specs.end(), [&](const ParamSpec& s) { return s.name == common.name; });
        if (!own) specs.push_back(std::move(common));
    }
    return specs;
}

std::vector<ParamSpec> nodeParamSpecs(const Operator& op) {
    std::vector<ParamSpec> specs = allParamSpecs(op);
    for (ParamSpec& extra : op.extraParamSpecs()) {
        const bool known = std::any_of(specs.begin(), specs.end(), [&](const ParamSpec& s) { return s.name == extra.name; });
        if (!known) specs.push_back(std::move(extra));
    }
    return specs;
}

// ---- Value ports ----

const char* valueKindName(ValueKind kind) {
    switch (kind) {
        case ValueKind::Any: return "any";
        case ValueKind::Number: return "number";
        case ValueKind::Integer: return "integer";
        case ValueKind::Bool: return "bool";
        case ValueKind::Vec3: return "vec3";
        case ValueKind::Choice: return "choice";
        case ValueKind::Text: return "text";
    }
    return "any";
}

ValueKind valueKindFromName(const std::string& name) {
    for (const ValueKind kind : {ValueKind::Any, ValueKind::Number, ValueKind::Integer, ValueKind::Bool, ValueKind::Vec3,
                                 ValueKind::Choice, ValueKind::Text}) {
        if (name == valueKindName(kind)) return kind;
    }
    throw GraphError("unknown value kind '" + name + "' (any, number, integer, bool, vec3, choice or text)");
}

namespace {
bool numeric(ValueKind k) { return k == ValueKind::Number || k == ValueKind::Integer || k == ValueKind::Bool; }
bool textual(ValueKind k) { return k == ValueKind::Choice || k == ValueKind::Text; }
}  // namespace

bool valueKindsMatch(ValueKind out, ValueKind in) {
    if (out == ValueKind::Any || in == ValueKind::Any || out == in) return true;
    if (numeric(out)) return numeric(in) || in == ValueKind::Vec3;
    if (textual(out)) return textual(in);
    return false;  // a vector feeds only a vector
}

ValueKind valueKindOf(const ParamValue& value, bool choice) {
    if (std::holds_alternative<bool>(value)) return ValueKind::Bool;
    if (std::holds_alternative<std::int64_t>(value)) return ValueKind::Integer;
    if (std::holds_alternative<double>(value)) return ValueKind::Number;
    if (std::holds_alternative<std::string>(value)) return choice ? ValueKind::Choice : ValueKind::Text;
    if (std::holds_alternative<Vec3f>(value)) return ValueKind::Vec3;
    return ValueKind::Any;
}

namespace {
class ConstantColorField final : public FieldValue {
public:
    explicit ConstantColorField(const Vec3f& value) : value_{{value.x, value.y, value.z}} {}
    float atRoot(const StrandRoot&) const override { return (value_[0] + value_[1] + value_[2]) / 3.0f; }
    float at(const StrandSet&, std::size_t) const override { return (value_[0] + value_[1] + value_[2]) / 3.0f; }
    float maximum() const override { return std::max({value_[0], value_[1], value_[2]}); }
    bool readsPositions() const override { return false; }
    bool strandLocal() const override { return true; }
    bool isColor() const override { return true; }
    std::array<float, 3> colorAt(const StrandSet&, std::size_t) const override { return value_; }
    std::array<float, 3> colorAtPoint(const StrandSet&, std::size_t, std::size_t) const override { return value_; }

private:
    std::array<float, 3> value_;
};
}  // namespace

FieldPtr uniformField(const UniformValue& value) {
    if (const double* d = std::get_if<double>(&value.value)) return constantField(static_cast<float>(*d));
    if (const std::int64_t* i = std::get_if<std::int64_t>(&value.value)) return constantField(static_cast<float>(*i));
    if (const bool* b = std::get_if<bool>(&value.value)) return constantField(*b ? 1.0f : 0.0f);
    if (const Vec3f* v = std::get_if<Vec3f>(&value.value)) return std::make_shared<const ConstantColorField>(*v);
    return nullptr;
}

bool paramMappable(const ParamSpec& spec) { return spec.description.find("(mappable") != std::string::npos; }

bool paramPortSpec(const ParamSpec& spec, PortSpec* port) {
    if (spec.port == ParamSpec::Port::None) return false;
    PortSpec p;
    p.name = spec.name;
    p.optional = true;
    const ParamValue& d = spec.defaultValue;
    const bool map = std::holds_alternative<MapRef>(d);
    const bool ramp = std::holds_alternative<Ramp>(d);
    if (spec.port == ParamSpec::Port::Auto && ramp) {
        p.type = PortType::Curve;
    } else if (spec.port == ParamSpec::Port::Auto && (paramMappable(spec) || spec.colorField || map)) {
        p.type = PortType::Field;
    } else if (map || ramp) {
        return false;  // no Value holds a map or a ramp
    } else {
        p.type = PortType::Value;
        p.valueKind = valueKindOf(d, !spec.choices.empty());
    }
    if (port) *port = p;
    return true;
}

ParamValue uniformAsParam(const ParamValue& value, const ParamSpec& spec) {
    const ParamValue& want = spec.defaultValue;
    double number = 0.0;
    bool isNumber = true;
    if (const double* d = std::get_if<double>(&value)) number = *d;
    else if (const std::int64_t* i = std::get_if<std::int64_t>(&value)) number = static_cast<double>(*i);
    else if (const bool* b = std::get_if<bool>(&value)) number = *b ? 1.0 : 0.0;
    else isNumber = false;
    const std::string what = "param '" + spec.name + "' takes " + valueKindName(valueKindOf(want, !spec.choices.empty()));
    if (std::holds_alternative<double>(want) && isNumber) return number;
    if (std::holds_alternative<std::int64_t>(want) && isNumber) {
        if (!(number >= -9.2233720368547758e18 && number < 9.2233720368547758e18)) {
            throw GraphError(what + ": " + std::to_string(number) + " is out of range for an integer");
        }
        return static_cast<std::int64_t>(std::llround(number));
    }
    if (std::holds_alternative<bool>(want) && isNumber) return number > 0.5;
    if (std::holds_alternative<Vec3f>(want)) {
        if (const Vec3f* v = std::get_if<Vec3f>(&value)) return *v;
        if (isNumber) {
            const auto f = static_cast<float>(number);
            return Vec3f(f, f, f);
        }
    }
    if (std::holds_alternative<std::string>(want)) {
        if (const std::string* text = std::get_if<std::string>(&value)) {
            if (!spec.choices.empty() && std::find(spec.choices.begin(), spec.choices.end(), *text) == spec.choices.end()) {
                std::string list;
                for (const std::string& c : spec.choices) list += (list.empty() ? "" : ", ") + c;
                throw GraphError(what + ": '" + *text + "' is not one of its choices (" + list + ")");
            }
            return *text;
        }
    }
    throw GraphError(what + " and cannot take " + valueKindName(valueKindOf(value)));
}

const char* channelUseName(ChannelUse use) {
    switch (use) {
        case ChannelUse::None: return "";
        case ChannelUse::Writes: return "writes";
        case ChannelUse::Reads: return "reads";
    }
    return "";
}

std::vector<ChannelWrite> Operator::channelWrites() const {
    std::vector<ChannelWrite> out;
    for (const ParamSpec& spec : allParamSpecs(*this)) {
        if (spec.channel != ChannelUse::Writes) continue;
        const std::string name = params().getString(spec.name);
        if (!name.empty() && name[0] != '_') out.push_back({name, spec.name});
    }
    return out;
}

AffectKind affectOf(const Operator& op) {
    const ParamSet& params = op.params();
    const bool plugged = params.field(kAffectParam) != nullptr;
    if (!plugged && !params.has(kAffectParam)) return AffectKind::All;
    // The common case first, without asking the node's ports: 1, all of it.
    const ParamValue& value = params.has(kAffectParam) ? params.get(kAffectParam) : ParamValue{1.0};
    const bool map = std::holds_alternative<MapRef>(value);
    const bool number = std::holds_alternative<double>(value) || std::holds_alternative<std::int64_t>(value);
    if (!plugged && !map && (!number || params.getDouble(kAffectParam) >= 1.0)) return AffectKind::All;
    if (!takesAffect(op)) return AffectKind::All;  // set on a node that does not take it: as ever
    if (plugged || map) return AffectKind::Mask;
    return params.getDouble(kAffectParam) <= 0.0 ? AffectKind::None : AffectKind::Mask;
}

namespace {

bool idChannel(const std::string& name) { return name.size() > 2 && name.compare(name.size() - 2, 2, "Id") == 0; }

}  // namespace

StrandsPtr applyAffect(const StrandSet& in, const StrandsPtr& out, const ScalarField& affect) {
    if (!out || in.strandCount() != out->strandCount() || in.pointCount() != out->pointCount() || in.ids() != out->ids() ||
        in.offsets() != out->offsets()) {
        return nullptr;
    }
    affect.validate(in);  // read on the strands as they came in
    const bool perPoint = affect.perPoint();
    auto mixed = std::make_shared<StrandSet>(*out);
    // The arrays the node wrote (those it shares with its input are the input's either way).
    const bool positions = &in.positions() != &out->positions();
    Vec3f* points = positions ? mixed->mutablePositions().data() : nullptr;
    const bool widths = &in.widths() != &out->widths() && out->widths().size() == out->pointCount() &&
                        in.widths().size() == in.pointCount();
    float* outWidths = widths ? mixed->mutableWidths().data() : nullptr;
    struct Channel {
        float* to;
        const float* from;  // the input's (null: it had none; its default)
        float fallback;
        bool id;
    };
    std::vector<Channel> pointChannels, strandChannels;
    for (const std::string& name : out->pointChannelNames()) {
        const std::vector<float>* from = in.pointChannel(name);
        if (from == out->pointChannel(name)) continue;
        if (from && from->size() != in.pointCount()) from = nullptr;
        pointChannels.push_back({mixed->mutablePointChannel(name)->data(), from ? from->data() : nullptr,
                                 out->pointChannelDefault(name), idChannel(name)});
    }
    for (const std::string& name : out->strandChannelNames()) {
        const std::vector<float>* from = in.strandChannel(name);
        if (from == out->strandChannel(name)) continue;
        if (from && from->size() != in.strandCount()) from = nullptr;
        strandChannels.push_back({mixed->mutableStrandChannel(name)->data(), from ? from->data() : nullptr,
                                  out->strandChannelDefault(name), idChannel(name)});
    }
    const Vec3f* inPoints = in.positions().data();
    const float* inWidths = in.widths().data();
    parallelFor(0, in.strandCount(), [&](std::size_t s) {
        const float strandWeight = clamp01(affect.at(in, s));
        // Strand channels by the strand's weight (the root's, for a per-point field).
        if (strandWeight < 1.0f) {
            for (const Channel& c : strandChannels) {
                const float from = c.from ? c.from[s] : c.fallback;
                if (c.id) {
                    if (strandWeight < 0.5f) c.to[s] = from;
                } else {
                    c.to[s] = strandWeight <= 0.0f ? from : mix1(from, c.to[s], strandWeight);
                }
            }
        }
        const std::size_t begin = in.strandBegin(s), n = in.strandPointCount(s);
        if (!perPoint && strandWeight >= 1.0f) return;
        for (std::size_t k = 0; k < n; ++k) {
            const float m = perPoint ? clamp01(affect.atPoint(in, s, k)) : strandWeight;
            if (m >= 1.0f) continue;  // the node's, exactly
            const std::size_t i = begin + k;
            if (m <= 0.0f) {  // the input's, exactly
                if (points) points[i] = inPoints[i];
                if (outWidths) outWidths[i] = inWidths[i];
                for (const Channel& c : pointChannels) c.to[i] = c.from ? c.from[i] : c.fallback;
                continue;
            }
            if (points) points[i] = inPoints[i] + (points[i] - inPoints[i]) * m;
            if (outWidths) outWidths[i] = mix1(inWidths[i], outWidths[i], m);
            for (const Channel& c : pointChannels) {
                const float from = c.from ? c.from[i] : c.fallback;
                c.to[i] = c.id ? (m < 0.5f ? from : c.to[i]) : mix1(from, c.to[i], m);
            }
        }
    });
    return mixed;
}

}  // namespace groomlab
