// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

#include "groomlab/flow.h"
#include "groomlab/guide_hierarchy.h"
#include "groomlab/guide_set.h"
#include "groomlab/map_ref.h"
#include "groomlab/node_catalog.h"
#include "groomlab/parts.h"
#include "groomlab/paths.h"
#include "groomlab/ramp.h"
#include "groomlab/scalp_mesh.h"
#include "groomlab/strand_set.h"

namespace groomlab {

// ---------------------------------------------------------------------------
// Values flowing between operators
// ---------------------------------------------------------------------------

// Field: a value per strand, produced by field nodes and plugged into a
// node's exposed (mappable) param. Curve: a ramp (a Curve node) plugged into
// an exposed ramp param, e.g. Clump's profile along the length.
// Curves: strands or guides. A node whose main input and output are Curves
// (the shaping nodes: Noise, Frizz, Smooth, ...) works on either: its output
// is whatever feeds it, and guides keep their IDs, roots and painted maps.
// Value: one value for the whole evaluation (a UniformValue), not one per strand: what a Value
// node or an Expression set to give a value hands on, plugged into an exposed param that takes no
// field (a count, a seed, a switch, a choice, a size...). Its ValueKind says what it holds.
enum class PortType { Strands, Guides, Mesh, Field, Curve, Curves, Value };

// What a Value port holds: a number (float), an integer, a switch, a vector (a color too), one of a
// param's choices, or text. Any: whatever comes (a compound's Value input, an Input node's output),
// checked when it is evaluated.
enum class ValueKind { Any, Number, Integer, Bool, Vec3, Choice, Text };
const char* valueKindName(ValueKind kind);  // "any", "number", "integer", "bool", "vec3", "choice", "text"
// The kind of that name; throws GraphError for an unknown one.
ValueKind valueKindFromName(const std::string& name);
// Whether a Value output of kind `out` can feed a Value input of kind `in`: numbers, integers and
// switches feed one another (an integer rounds, a switch is on above 0.5) and vectors (each
// component the number); a vector feeds only a vector; choices and text feed one another (a choice
// must be one of the param's when evaluated). Any matches every kind.
bool valueKindsMatch(ValueKind out, ValueKind in);

// Whether an output of type `out` can feed an input of type `in`: the same
// type, or Curves on either side with Strands or Guides on the other (an
// output not yet resolved is checked again when evaluated), or a Value into a Field (a number as
// the same value on every strand; Graph::connect checks it is a number). A Field never feeds a
// Value: a value per strand cannot set a param that has one value for the whole node.
bool portTypesMatch(PortType out, PortType in);

const char* portTypeName(PortType type);
// The type of that name; throws GraphError for an unknown one.
PortType portTypeFromName(const std::string& name);

// Immutable, shared results so cached outputs can feed many consumers.
using StrandsPtr = std::shared_ptr<const StrandSet>;
using GuidesPtr = std::shared_ptr<const GuideSet>;
using MeshPtr = std::shared_ptr<const ScalpMesh>;
using HierarchyPtr = std::shared_ptr<const GuideHierarchy>;  // external input only, not a port type

// A scalar per strand: what a Field port carries. Read at a root before
// strands exist (Scatter), or per strand of a set.
class ScalarField;  // field.h

class FieldValue {
public:
    virtual ~FieldValue() = default;
    virtual float atRoot(const StrandRoot& root) const = 0;
    virtual float at(const StrandSet& strands, std::size_t strand) const = 0;
    // Value at one point of a strand. Fields that vary along the strand (an
    // expression of t, a point channel) override it; the rest read at().
    virtual float atPoint(const StrandSet& strands, std::size_t strand, std::size_t) const { return at(strands, strand); }
    // True when atPoint varies along a strand: operators then read per point
    // inside their loops (valid after validate()).
    virtual bool perPoint() const { return false; }
    // The largest value it can take (Scatter scatters at this density).
    virtual float maximum() const = 0;
    // Throws GraphError when the field cannot be read on these strands.
    virtual void validate(const StrandSet&) const {}
    // Whether its values can change as the strands' points move (it reads their positions or
    // shape), not only their roots, IDs and channels. Fields that read only those say false.
    virtual bool readsPositions() const { return true; }
    // True when a strand's value depends on that strand alone (its root, ID, channels and points),
    // not on the rest of the set (an expression of `longest` reads every strand): what lets the
    // node it is plugged into make again only some strands (Operator::evaluateIncremental).
    virtual bool strandLocal() const { return false; }
    // When all it reads of the rest of the set is `longest` (an expression of it: strandLocal false
    // for that alone), the longest strand's length its last validate() read; else negative. With
    // that the same, a strand's value is still the strand's own (LongestKept, operators/internal.h).
    virtual float longestRead() const { return -1.0f; }
    // A field made of a ScalarField (a Map or Expression node's): it, for what reads the field's
    // inside (a GPU evaluating it, groomlab_gpu); null for any other.
    virtual const ScalarField* scalarField() const { return nullptr; }
    // A color per strand (linear RGB), for a color param it is plugged into (ColorField, field.h):
    // a color field's own (a Map's rgb output), any other's value as gray.
    virtual bool isColor() const { return false; }
    virtual std::array<float, 3> colorAt(const StrandSet& strands, std::size_t strand) const {
        const float v = at(strands, strand);
        return {v, v, v};
    }
    virtual std::array<float, 3> colorAtPoint(const StrandSet& strands, std::size_t strand, std::size_t point) const {
        const float v = atPoint(strands, strand, point);
        return {v, v, v};
    }
};
using FieldPtr = std::shared_ptr<const FieldValue>;
using CurvePtr = std::shared_ptr<const Ramp>;

// The maps painted on the scalp (Groom Tools > Paint) by name: where each lives ("guides": a
// layer on the guides; "vertices": a color set of the scalp of the map's name; "texture": the
// image file) so a Map node finds its map by name alone. The host registers it as the external
// "maps"; a map missing from it is looked for on the scalp, then on the guides.
struct MapInfo {
    std::string kind;
    std::string texture;
    int channels = 1;  // 1 gray, 4 color (r g b a): kept in groom files, so a color map comes back as one
};
using MapCatalog = std::map<std::string, MapInfo>;
struct MapCatalogValue {  // the catalog as an external input value
    MapCatalog maps;
};
using MapCatalogPtr = std::shared_ptr<const MapCatalogValue>;

// The scalp's recent motion: its points at earlier times, for nodes that react to how the scalp
// moved (Jiggle) without a simulation cache. frames[k] holds the scalp's points (the "scalp"
// external's vertices, in its order) at time t - (k + 1) * step, t being the evaluated time. The
// host registers it as the external "scalpHistory" (setScalpHistoryExternal, groom.h) when a
// node asks for it (scalpHistoryFrames); its hash covers the step, the times and every point.
// After those frames, a looping Jiggle's (scalpHistoryLoopFrames): the scalp at the frames it
// reads in place of the ones before its loop's start, at loopSeconds (one each, in order).
struct ScalpHistory {
    double step = 0.0;  // seconds between samples (a frame)
    std::vector<std::vector<Vec3f>> frames;
    std::vector<double> loopSeconds;  // the last loopSeconds.size() frames' times
    std::uint64_t hash = 0;

    // The frames before now (the rest are a loop's).
    std::size_t plainCount() const { return frames.size() - loopSeconds.size(); }
    // The history of these frames, hashed (`loopSeconds`: the last frames' times, at most as many).
    static std::shared_ptr<const ScalpHistory> make(double step, std::vector<std::vector<Vec3f>> frames,
                                                    std::vector<double> loopSeconds = {});
};
using ScalpHistoryPtr = std::shared_ptr<const ScalpHistory>;

// A Sculpt node's edits (sculpt.h), handed in by the session as the external "sculpt:<node>".
struct SculptEdits;
using SculptPtr = std::shared_ptr<const SculptEdits>;

struct UniformValue;  // below, after ParamValue
using UniformPtr = std::shared_ptr<const UniformValue>;

using Value = std::variant<std::monostate, StrandsPtr, GuidesPtr, MeshPtr, HierarchyPtr, PartsPtr,
                           FieldPtr, CurvePtr, FlowPtr, MapCatalogPtr,
                           ScalpHistoryPtr, SculptPtr,  // hierarchy, parts, flow, maps, scalp history, sculpts: external only
                           UniformPtr>;

// Typed accessor: the pointer held in v, or null if v holds something else.
template <class Ptr>
Ptr valueAs(const Value& v) {
    const Ptr* p = std::get_if<Ptr>(&v);
    return p ? *p : Ptr{};
}

struct PortSpec {
    std::string name;
    PortType type = PortType::Strands;
    bool optional = false;
    ValueKind valueKind = ValueKind::Any;  // a Value port's
};

// ---------------------------------------------------------------------------
// Parameters
// ---------------------------------------------------------------------------

using ParamValue = std::variant<bool, std::int64_t, double, std::string, Vec3f, Ramp, MapRef>;

// What a Value port carries: one param value (a bool, an integer, a number, text or a vector) for
// the whole evaluation. `varies`: it may change from frame to frame (an expression reading the
// time, a keyed Value node), which a param that must stay the same on every frame refuses.
struct UniformValue {
    ParamValue value;
    bool varies = false;
};
// The kind of value a param's value is (a ramp or a map: Any, as no Value port takes one).
ValueKind valueKindOf(const ParamValue& value, bool choice = false);
// A number (an integer, a switch as 0 or 1) as a field of that value on every strand, for a Value
// plugged into a Field port; null for a vector or text.
FieldPtr uniformField(const UniformValue& value);

// A node's params. The getters return the stored value when it has the type asked for, else the
// param's spec default (ParamSpec::defaultValue, which Operator::applyDefaults hands every node's
// set: the one place a param's default is written), else `fallback` (a set made by hand, a param
// no spec declares). Operators read their params without a fallback.
// The names a set's getters read (get, the typed getters, field, a default), kept while a log is set
// on it (ParamSet::setReadLog): what a test asks to learn which params an operator reads in a state
// (test_param_use.cpp). Safe from any thread.
class ParamReadLog {
public:
    void add(const std::string& name) {
        const std::lock_guard<std::mutex> lock(mutex_);
        names_.insert(name);
    }
    std::set<std::string> names() const {
        const std::lock_guard<std::mutex> lock(mutex_);
        return names_;
    }

private:
    mutable std::mutex mutex_;
    std::set<std::string> names_;
};

class ParamSet {
public:
    void set(const std::string& name, ParamValue value) { values_[name] = std::move(value); }
    bool has(const std::string& name) const { return values_.count(name) != 0; }
    const ParamValue& get(const std::string& name) const;
    // The specs' defaults the getters fall back to (not stored, not hashed: the operator's type
    // decides them).
    void setDefaults(std::shared_ptr<const std::map<std::string, ParamValue>> defaults) { defaults_ = std::move(defaults); }
    // The spec default of `name`; null when no spec declares it.
    const ParamValue* defaultValue(const std::string& name) const;
    // The specs' hard limits (ParamSpec::min, max) getInt and getDouble clamp to: the one place
    // a param's range is enforced (not stored, not hashed: the operator's type decides them).
    struct Limits {
        std::optional<double> min, max;
    };
    void setLimits(std::shared_ptr<const std::map<std::string, Limits>> limits) { limits_ = std::move(limits); }
    // `value` within the limits of `name` (NaN: the lower limit, when there is one).
    double clamped(const std::string& name, double value) const;

    bool getBool(const std::string& name, bool fallback = false) const;
    std::int64_t getInt(const std::string& name, std::int64_t fallback = 0) const;
    double getDouble(const std::string& name, double fallback = 0.0) const;
    std::string getString(const std::string& name, const std::string& fallback = {}) const;
    Vec3f getVec3(const std::string& name, Vec3f fallback = {}) const;
    // A ramp, or a constant ramp when the param is a number.
    Ramp getRamp(const std::string& name, const Ramp& fallback = Ramp::constant(1.0f)) const;

    const std::map<std::string, ParamValue>& values() const { return values_; }
    std::uint64_t hash() const;

    // Fields plugged into exposed params for the current evaluation (set by
    // the graph around Operator::evaluate; not stored, not hashed: they reach
    // the cache key through the node's input keys). They override the value.
    void setField(const std::string& name, FieldPtr field) { fields_[name] = std::move(field); }
    FieldPtr field(const std::string& name) const {
        noteRead(name);
        auto it = fields_.find(name);
        return it == fields_.end() ? nullptr : it->second;
    }
    // Curves plugged into exposed ramp params, the same way: getRamp returns them.
    void setCurve(const std::string& name, CurvePtr curve) { curves_[name] = std::move(curve); }
    void clearFields() {
        fields_.clear();
        curves_.clear();
    }
    // Keeps the names the getters read in `log` from now on (shared by copies of the set); null:
    // none kept (the default; a branch per read).
    void setReadLog(std::shared_ptr<ParamReadLog> log) { readLog_ = std::move(log); }

private:
    void noteRead(const std::string& name) const {
        if (readLog_) readLog_->add(name);
    }
    std::shared_ptr<ParamReadLog> readLog_;
    std::map<std::string, ParamValue> values_;
    std::shared_ptr<const std::map<std::string, ParamValue>> defaults_;
    std::shared_ptr<const std::map<std::string, Limits>> limits_;
    std::map<std::string, FieldPtr> fields_;
    std::map<std::string, CurvePtr> curves_;
};

// ---------------------------------------------------------------------------
// Operators
// ---------------------------------------------------------------------------

// When a UI shows a param: while another param of the node holds one of `in` (a bool as "true" or
// "false", a number as written by JSON) and that param shows itself (its own condition, and so
// on up). Empty (no `param`): always. Clump's count shows with clumpBy count, and clumpBy only with
// centersFrom pick. Never by whether an input is linked: a param stays where the user finds it,
// and a dropdown says what the node does with an input.
struct ParamCondition {
    std::string param;
    std::vector<std::string> in;

    bool empty() const { return param.empty(); }
};

// What a text param naming a strand channel (an "attribute" to artists: a value per strand or per
// point the strands carry, by name) does with it: writes it (Clump's clumpChannel: the node makes
// the channel, overwriting one of the name made upstream) or reads it (Curl's clumpChannel: a
// channel some node upstream wrote; one that does not reach the node is ignored). UIs show which
// and offer, for a reading param, the channels reaching the node (graphChannels, attributes.h).
enum class ChannelUse { None, Writes, Reads };
const char* channelUseName(ChannelUse use);  // "", "writes", "reads"

struct ParamSpec {
    std::string name;
    ParamValue defaultValue;
    std::string description;
    // What the number measures, for UIs that show real units: "length"
    // (scene units: centimeters in Maya), "width" (the same unit, shown finer)
    // "density" (strands per square scene unit) or "perLength" (a rate per scene
    // unit: turns, crossings, features; presets fitted to a bigger scalp divide
    // it); "color" for a vec3 that is a linear RGB color; "angle" (degrees), "turns"
    // (whole turns: 1 is 360 degrees), "fraction" (0-1, shown as a percentage),
    // "seconds", "pixels"; empty: unitless. Only length, width, density and perLength
    // are fitted to a scalp's size. A vec3 of length (a position, a move, a box's size) has it
    // on every component. A position scales about the world's origin, as the whole scene does.
    std::string unit = {};
    // A text param with a fixed set of values: UIs show a dropdown of these.
    std::vector<std::string> choices = {};
    // Whether a host may key it (Graph::setParamControls). False for a param that sets how many
    // strands, points or cards there are, or where they go (a density, a count, segments, a
    // seed): those must stay the same on every frame for motion blur and for Alembic and
    // Unreal groom caches.
    bool animatable = true;
    // Hard limits of a number (int or float) param: ParamSet's getters clamp the stored value to
    // them, so operators never see a value past them (a field or map plugged in is per strand,
    // not clamped here). Unset: no limit on that side.
    std::optional<double> min = {}, max = {};
    // The slider's range in a UI (a typed value may go past it, up to min and max). Unset: the
    // hard limit on that side, else what the UI picks.
    std::optional<double> softMin = {}, softMax = {};
    // Where a UI lists it: "Base", "Variation", "Tip", "Bands", "Render", "Advanced"... Empty: the
    // operator's main params.
    std::string group = {};
    // A tuning knob most grooms never touch (a seed, iterations, a channel's name): UIs may fold it away.
    bool advanced = false;
    // When UIs show it (a scales-only param of Instance: mode in [scales]; Clump's count: clumpBy
    // in [count], clumpBy itself only with centersFrom pick).
    ParamCondition showWhen = {};
    // A direction in world space: a vec3 (Gravity's, Wind's, Bend's direction), or a choice of
    // world axis ("x", "y" or "z": Mirror's plane normal, whose `offset` and `side` place the plane
    // along it). A groom turned into another up axis (convertUpAxis, groom_file.h) turns these
    // with its points, so the hair falls, blows and mirrors the same way in the world.
    bool worldDirection = false;
    // A vec3 that also takes an expression giving a vector, per strand (VectorField, field.h):
    // {"expression": "normalize(N + [0, -1, 0])"} in place of [x, y, z].
    bool vectorExpression = false;
    // A color (unit "color") that takes a field node plugged into it (an exposed param, ColorField,
    // field.h): a Map's rgb output colors it per strand, times the param's own color.
    bool colorField = false;
    // When `unit` holds: while another param of the node holds one of `in` (ParamCondition's
    // `param` and `in`; empty: always). Noise's frequency is per scene unit in world space and
    // per strand length in strand space. Fitting and UIs showing units both read it.
    // A field node plugged into a param needs no unit: the param's own number times the field's
    // value is what it takes (field.h), so field nodes' ranges are factors (masks, 0-1) and fitting
    // the param fits it.
    ParamCondition unitWhen = {};
    // A text param naming a strand channel: whether the node writes or reads it (ChannelUse).
    ChannelUse channel = ChannelUse::None;
    // A reading param whose default channel may well be missing, the node then doing without it by
    // design (Clump's regionChannel: region, written only with an Interpolate's regionMap): a read
    // of the default that does not reach is no mistake (ChannelRead::optional, attributes.h).
    bool channelOptional = false;
    // The input port it takes when exposed (paramPortSpec): Auto (a Curve port for a ramp, a Field
    // port for a mappable number or a colorField color, a Value port for the rest), Value (a Value
    // port whatever it is: a compound's exposed params), or None: it cannot be an input (it sets the
    // node's ports: a Value node's kind, an Input's type).
    enum class Port { Auto, Value, None };
    Port port = Port::Auto;

    // The spec with hard limits (and the slider's range, unless one is set).
    ParamSpec range(double lo, double hi) const {
        ParamSpec s = *this;
        s.min = lo;
        s.max = hi;
        return s;
    }
    ParamSpec atLeast(double lo) const {
        ParamSpec s = *this;
        s.min = lo;
        return s;
    }
    // The slider's range only (no clamp).
    ParamSpec soft(double lo, double hi) const {
        ParamSpec s = *this;
        s.softMin = lo;
        s.softMax = hi;
        return s;
    }
    ParamSpec in(std::string groupName) const {
        ParamSpec s = *this;
        s.group = std::move(groupName);
        return s;
    }
    ParamSpec adv() const {
        ParamSpec s = *this;
        s.advanced = true;
        return s;
    }
    ParamSpec when(std::string param, std::vector<std::string> values) const {
        ParamSpec s = *this;
        s.showWhen.param = std::move(param);
        s.showWhen.in = std::move(values);
        return s;
    }
    ParamSpec units(std::string u) const {
        ParamSpec s = *this;
        s.unit = std::move(u);
        return s;
    }
    // `unit` only while `param` holds one of `values` (unitWhen).
    ParamSpec unitsWhen(std::string param, std::vector<std::string> values) const {
        ParamSpec s = *this;
        s.unitWhen.param = std::move(param);
        s.unitWhen.in = std::move(values);
        return s;
    }
    ParamSpec world() const {
        ParamSpec s = *this;
        s.worldDirection = true;
        return s;
    }
    ParamSpec vectorExpr() const {
        ParamSpec s = *this;
        s.vectorExpression = true;
        return s;
    }
    // A color a Map's rgb output can drive (colorField).
    ParamSpec colorMap() const {
        ParamSpec s = *this;
        s.colorField = true;
        return s;
    }
    // A channel the node writes under the name the param holds (empty: none).
    ParamSpec writesChannel() const {
        ParamSpec s = *this;
        s.channel = ChannelUse::Writes;
        return s;
    }
    // A channel the node reads, by the name the param holds (empty: none); `optional`: its default
    // may well be missing (channelOptional).
    ParamSpec readsChannel(bool optional = false) const {
        ParamSpec s = *this;
        s.channel = ChannelUse::Reads;
        s.channelOptional = optional;
        return s;
    }
    // A param that cannot be an input (port None).
    ParamSpec noInput() const {
        ParamSpec s = *this;
        s.port = Port::None;
        return s;
    }
};

// Whether a param is mappable: a number a map, an expression or a field drives per strand (its
// description says "(mappable").
bool paramMappable(const ParamSpec& spec);
// The input port a param takes when exposed (Graph::setParamInputs), named after it and optional:
// a Curve port for a ramp, a Field port for a mappable number or a colorField color, else a Value
// port of the param's kind (a text param with choices: Choice). False when it takes none
// (ParamSpec::port None, or a map param, which no Value holds).
bool paramPortSpec(const ParamSpec& spec, PortSpec* port);
// `value` (a Value port's) as the param of spec `spec` takes it: a number rounded for an integer, on
// above 0.5 for a switch, the same on every component for a vector, text one of the choices for a
// choice. Throws GraphError (naming the param) for a value of a kind the param cannot take.
ParamValue uniformAsParam(const ParamValue& value, const ParamSpec& spec);

// A strand channel a node writes as set: its name, and the param naming it ("" for a name the node
// always writes: Flyaway's flyaway, HairColor's melanin and colors). `extends`: the node adds to a
// channel of the name made upstream by design rather than replacing it (a Group combining with
// one of its name), so writing it again is no overwrite.
struct ChannelWrite {
    std::string name;
    std::string param;
    bool extends = false;
};

// A spec a host must not key (ParamSpec::animatable false).
inline ParamSpec notAnimatable(ParamSpec spec) {
    spec.animatable = false;
    return spec;
}

class Operator;

// The params every node of a kind has besides its own (its operator's paramSpecs): "affect"
// (kAffectParam) on the nodes that reshape or recolor curves and hand on the same strands, a
// per-strand 0-1 mask the graph mixes the node's result back toward its input by (Graph). Not on
// nodes that make, remove, add or resample strands, nor on sources, fields and utilities.
std::vector<ParamSpec> commonParamSpecs(const Operator& op);
// Its own params, then the common ones: what UIs list, defaults fill and graph files hold.
std::vector<ParamSpec> allParamSpecs(const Operator& op);
// Its params as this node is set: allParamSpecs, then the ones its setting adds
// (Operator::extraParamSpecs: a Compound's exposed params). What exposing a param reads.
std::vector<ParamSpec> nodeParamSpecs(const Operator& op);
constexpr const char* kAffectParam = "affect";
// Whether a node of this kind takes "affect" (commonParamSpecs).
bool takesAffect(const Operator& op);
// A node's affect as it is set (with the field plugged into it, during an evaluation): all of the
// node's work (a number 1 or more, or a node without affect: its result as ever), none of it (a
// number 0 or less: its input handed on, as bypassed) or a mask (a map, an expression, a plugged
// field or a number between: the result mixed back toward the input strand by strand).
enum class AffectKind { All, None, Mask };
AffectKind affectOf(const Operator& op);

// When a node runs (Yeti's per-node evaluation): always; viewportOnly: only in the viewport,
// bypassed in renders, exports, the Arnold procedural and the CLI; renderOnly: bypassed in the
// viewport (EvalContext::viewport). A node bypassed so hands its first input on, as a bypass does.
enum class EvaluationMode { Always, ViewportOnly, RenderOnly };
const char* evaluationModeName(EvaluationMode mode);  // "always", "viewportOnly", "renderOnly"
// The mode of that name; throws GraphError for another.
EvaluationMode evaluationModeFromName(const std::string& name);

// Data the host (Maya, CLI, procedural) feeds into the graph by name, such as
// the scalp mesh and guide curves. Each entry carries a content hash so only
// operators that read it are invalidated when it changes.
class ExternalInputs {
public:
    struct Entry {
        Value value;
        std::uint64_t hash = 0;
    };

    void set(const std::string& name, Value value, std::uint64_t hash) { entries_[name] = {std::move(value), hash}; }
    const Entry* find(const std::string& name) const {
        auto it = entries_.find(name);
        if (it != entries_.end()) return &it->second;
        return parent_ ? parent_->find(name) : nullptr;
    }
    // Entries not found here are looked up in `parent` (compound subgraphs).
    void setParent(const ExternalInputs* parent) { parent_ = parent; }
    // Every entry's name and hash (this level only, not the parent's).
    std::uint64_t hash() const;

private:
    std::map<std::string, Entry> entries_;
    const ExternalInputs* parent_ = nullptr;
};

struct EvalContext {
    float previewDensity = 1.0f;  // PG-4: 1.0 for render, < 1.0 for interactive preview
    double time = 0.0;
    // The frame rate the time's frame is counted at (expressions' frame: frame_time.h's rule).
    double framesPerSecond = 24.0;
    // An evaluation for the viewport (a host's interactive view): nodes set to renderOnly are
    // bypassed, and viewportOnly ones run. False (renders, exports, the CLI, the procedural):
    // the other way round (EvaluationMode).
    bool viewport = false;
    // A host's live param drag (GroomSession::setLiveDrag), in the viewport: the nodes that only
    // refine the look for it (Subdivide) hand their input on, so the GPU's preview runs the nodes
    // before them; the release evaluates them again. The nodes reading it salt their keys with it.
    bool liveDrag = false;
    const ExternalInputs* externals = nullptr;
    const PathResolver* paths = nullptr;  // relative file paths (maps, caches); see paths.h
    bool buildCards = false;  // Cards nodes build their cards (else they pass strands through)
    // The viewport's memory budget: each Scatter thins its preview to at most
    // this many points (strands x points each), keeping stable strand IDs.
    // Every node downstream holds a copy while previewing. 0: no limit (render).
    double maxPoints = 0.0;
    // What Graph::evaluate* hands back: false (every host), the strands a preview shows, without
    // the core strands it holds hidden and without their roles (preview_core.h: shownStrands);
    // true, as the nodes made them (a Compound's inner graph, whose result its own node hands on).
    // Not part of the hash: the cache holds the nodes' results either way.
    bool keepPreviewCore = false;
    // Segments each Scatter grows its strands with (points = segments + 1), for every Scatter.
    // 0: each Scatter's own branch sets it (Graph::strandResolution): the most "segments" any
    // Resample downstream of it asks for, except one bypassed or fed guides (with an Output node,
    // only those on the way to it); without one, 8 (with no Output node, the most a Resample fed
    // nothing asks for). The graph hands each
    // node its own (a Compound its inner graph's Scatters, 0 when it has none: they go by the
    // inner graph's Resamples).
    int strandSegments = 0;
    // The groom's scale (GroomSession::groomScale: a groom scaled with its scalp): the sizes
    // operators take from the whole scalp or all the strands and round to 1% steps for edit
    // stability (roundToPercentStep: Clump's spacing by count, Resample's reference length, Bands'
    // guard length, the hair grids') round on a lattice anchored at it, so the groom at k is the one
    // at 1x scaled, not one rounded otherwise. 1: anchored at 1 (decades, as a scene in other units).
    double scale = 1.0;

    // Hash of density and point budget (and the scale, when not 1). Time is left out (time-varying
    // data arrives as hashed externals), and so is buildCards: the operators
    // that read either (ImportCurves and Wind the time, Cards buildCards) salt
    // their own keys with it (Operator::contextSalt), so only they and what is
    // downstream of them re-evaluate from frame to frame. So are the segments: each Scatter
    // salts its key with its own (a Resample's edit makes only its own branch again).
    std::uint64_t hash() const;

    Value external(const std::string& name) const {
        const ExternalInputs::Entry* e = externals ? externals->find(name) : nullptr;
        return e ? e->value : Value{};
    }

    // Tells the user something about the node being evaluated that is not an error: it does
    // nothing as set (a Clump with no centers, a Flow with no arrows). Informational: the result
    // stands. The graph keeps the node's notes from its last run (Graph::lastNotes, the session's
    // stats "notes"); safe from any thread of the operator's own work. Outside a graph's
    // evaluation it does nothing.
    void note(const std::string& text) const;
    struct NoteSink;                        // the graph's, for the node it runs
    mutable NoteSink* noteSink = nullptr;   // set by Graph around each operator call
};

class GraphError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// How a node's result differs from the result it made before (incremental evaluation,
// Operator::evaluateIncremental). `all`: anything may differ, or nobody knows. Otherwise the two
// hold the same strands (count, IDs, roots, point counts, channel names and defaults, instances,
// card and feather layers) and only the listed strands' points, widths and channel values may
// differ; an empty list: nothing does. For a Guides result only `all` is ever said.
// `pointsKept` (with `all` or a list): the points are the very ones of the result before, bit for
// bit (the same strands, IDs, roots, point counts and positions, channel names and defaults, the
// same instances, card and feather layers): only widths and channel values may differ (a Width or
// HairColor edit). The
// graph says it (Graph::evaluateWithKeys, from the arrays the two results share), never an
// operator: what one makes of its inputs' (a Collide keeping its last points) it finds again.
struct StrandChanges {
    bool all = true;
    std::vector<std::uint32_t> strands;  // sorted, each once (when not all)
    bool pointsKept = false;

    bool none() const { return !all && strands.empty(); }
    static StrandChanges listed(std::vector<std::uint32_t> strands) {
        StrandChanges c;
        c.all = false;
        c.strands = std::move(strands);
        return c;
    }
};
using ChangesPtr = std::shared_ptr<const StrandChanges>;

// How `next` differs from `previous`, when the two hold the same strands (as StrandChanges says:
// count, IDs, roots, point counts, channel names and defaults, instances, card and feather
// layers): the strands whose points, widths or channel values differ, byte for byte, found in
// parallel blocks, the arrays the two share skipped. Null when they do not hold the same strands
// (or either is null). What a node that evaluated in full hands on (Graph::setDiffFullResults).
ChangesPtr strandChangesBetween(const StrandsPtr& previous, const StrandsPtr& next);

// What an incremental evaluation gets besides the inputs: the node's last result (its primary
// output, made by the same operator with the same params, context, salts and externals) and how
// each input changed since the value that result was made from (per input slot; null: unknown,
// anything may have changed). `sole`: the graph let go of the last result for this evaluation and
// holds it only through `previous` (it reads nothing of it after), so an operator holding it too
// and nobody else may take its arrays rather than copy them (StrandSet::takePositionsFrom);
// what the last result holds is then not read again. For an operator with several outputs
// (Operator::evaluateOutputsIncremental) `previous` is empty and `previousOutputs` holds every
// output's last value, in outputs() order (`sole` as for `previous`, for each).
struct IncrementalInputs {
    Value previous;
    std::vector<ChangesPtr> inputs;
    bool sole = false;
    std::vector<Value> previousOutputs;
};

class Operator;
// Called for each operator whose result makes a node's (Graph::forEachActiveUpstream) with its
// path: its node's name, inside a compound "compound/inner". Returns false to stop the walk.
using ActiveNodeVisitor = std::function<bool(const std::string& path, const Operator& op)>;

// Base class for every graph node. Operators are pure: evaluate() must depend
// only on params(), the context and the input values, so results can be cached.
class Operator {
public:
    virtual ~Operator() = default;

    virtual std::string typeName() const = 0;
    virtual std::vector<PortSpec> inputs() const = 0;
    // The primary output: what Graph::evaluate returns and what a connection naming no output reads.
    virtual PortSpec output() const { return {"out", PortType::Strands}; }
    // Every output port, output() first. A node with several (a Map node: luminance, r, g, b, a)
    // overrides this and evaluateOutputs; one evaluation makes them all, and a connection picks one.
    virtual std::vector<PortSpec> outputs() const { return {output()}; }
    virtual Value evaluate(const EvalContext& ctx, const std::vector<Value>& inputs) const = 0;
    // One value per outputs() entry, in order, from one evaluation. Default: {evaluate()}.
    virtual std::vector<Value> evaluateOutputs(const EvalContext& ctx, const std::vector<Value>& inputs) const {
        return {evaluate(ctx, inputs)};
    }
    // Incremental evaluation (opt in, operators with one output; several: evaluateOutputsIncremental
    // below): the graph calls it instead of
    // evaluate() when the node's own key (type, params, salts, externals, context) is the one its
    // last result was made with. Sets *out to exactly what evaluate(ctx, inputs) returns, bit for
    // bit, making again only what the inputs' changes reach, sets *changed to how *out differs
    // from `incremental.previous` (StrandChanges), and returns true; false: the node evaluates in
    // full. Never writes into the previous result or anything it shares (the session, a viewport
    // or another document may be reading it): an array it writes starts as a copy. An operator
    // checks that whatever its result depends on beyond its inputs' changed strands (a whole-set
    // pass, a kept binding) is what the previous result was made with, and returns false if not.
    virtual bool evaluateIncremental(const EvalContext& /*ctx*/, const std::vector<Value>& /*inputs*/,
                                     const IncrementalInputs& /*incremental*/, Value* /*out*/,
                                     StrandChanges* /*changed*/) const {
        return false;
    }
    // The same for an operator with several outputs (a Split): sets *outs to exactly what
    // evaluateOutputs returns and *changed to one StrandChanges per output, each against that
    // output's last value (IncrementalInputs::previousOutputs). An output whose strands are not
    // the ones it held (a strand moved to another output) says `all`.
    virtual bool evaluateOutputsIncremental(const EvalContext& /*ctx*/, const std::vector<Value>& /*inputs*/,
                                            const IncrementalInputs& /*incremental*/, std::vector<Value>* /*outs*/,
                                            std::vector<StrandChanges>* /*changed*/) const {
        return false;
    }

    // Takes the work `previous` kept between evaluations (an Interpolate's binding, a Clump's
    // center choice: operator_state.h), an operator of the same type whose node this one stands for
    // in another copy of the graph (Graph::adoptCache, Graph::clone). Such work names the results
    // it was made for, so it serves only where this node's results are those (taken from the other
    // copy's cache). Default: nothing kept.
    virtual void adoptKeptWork(const Operator& /*previous*/) {}
    // Its result `made` mixed back toward its input by its affect (a masked node: applyAffect) into
    // `mixed`, which the graph keeps and hands on in its place: work kept for `made` (the state
    // operator_state.h answers for a result) now answers for `mixed`. Default: nothing kept.
    virtual void resultMixed(const StrandsPtr& /*made*/, const StrandsPtr& /*mixed*/) const {}

    // Parameters with defaults; OperatorRegistry::create applies the defaults.
    virtual std::vector<ParamSpec> paramSpecs() const { return {}; }
    // Params a node has by how it is set, besides its kind's (paramSpecs and the common ones): a
    // Compound's exposed params, each its inner param's spec under the exposed name. Not given
    // defaults (applyDefaults); what such a param can be exposed as an input by (nodeParamSpecs).
    virtual std::vector<ParamSpec> extraParamSpecs() const { return {}; }

    // Names of ExternalInputs entries this operator reads. Their hashes join
    // the operator's cache key.
    virtual std::vector<std::string> externalInputs() const { return {}; }
    // What of external `name` (one of externalInputs, present) joins the cache key: its hash by
    // default. A node whose result depends on a piece of a large external keys by that piece
    // (Guides: the guides of its own groups of the hierarchy), so an edit elsewhere in it leaves
    // the node's result, and everything made from it, cached. Called on every evaluation: memoize.
    virtual std::uint64_t externalKey(const std::string& /*name*/, const ExternalInputs::Entry& entry) const { return entry.hash; }

    // Extra cache-key input for state outside params and inputs, such as a
    // file's modification time. Must be cheap; called on every evaluation.
    virtual std::uint64_t cacheSalt() const { return 0; }
    // The parts of the context this operator reads beyond EvalContext::hash
    // (e.g. Cards reads buildCards). Joins the cache key.
    virtual std::uint64_t contextSalt(const EvalContext&) const { return 0; }
    // A source node that hands an external on as it is, whose key may stay while the object
    // changes: the "scalp" is keyed by its surface (ScalpMesh::surfaceHash), so a scalp with
    // repainted color sets has the same key. On a cache hit the graph takes the external's
    // current value again (not counted as an evaluation), so the nodes that read a color set
    // (and salt their keys with it) are handed the scalp they were keyed on. Inputs: none.
    virtual bool refreshesOnCacheHit() const { return false; }
    // Something to tell the user about this evaluation that is not an error (Jiggle without the
    // scalp's motion history: it passes the strands through). Graph::lastNotes collects them.
    virtual std::string note(const EvalContext&) const { return {}; }
    // Whether its last evaluation thinned its strands to the viewport's point budget
    // (EvalContext::maxPoints: a Scatter's): a render may take a preview's result only when no
    // node did (GroomSession::rendersAsViewed).
    virtual bool thinnedLastRun() const { return false; }

    // Whether input `slot` reaches the output as it is set (a Switch reads only the input it
    // picks). Graph::forEachActiveUpstream walks only these.
    virtual bool inputActive(std::size_t /*slot*/) const { return true; }
    // Operators inside this one (a Compound's subgraph) that make its result, each visited
    // with `prefix` before its inner name, as Graph::forEachActiveUpstream does. False: stopped.
    virtual bool forEachActiveInner(const std::string& /*prefix*/, const ActiveNodeVisitor& /*visit*/) const {
        return true;
    }

    // The strand channels (attributes) this node writes as set: each param marked writesChannel
    // holding a name (the default), and whatever an operator writes under fixed names or only in
    // some settings (its override). Names starting with "_" are a node's own (a preview's roles)
    // and never listed. What graphChannels (attributes.h) and the operators' spec JSON read.
    virtual std::vector<ChannelWrite> channelWrites() const;
    // Whether the channels on what feeds input `slot` reach the output: the first input by
    // default (a Clump's centers do not hand theirs on); a Merge's every input, a Switch's picked
    // one, an Interpolate's guides too (their painted maps become channels).
    virtual bool carriesChannels(std::size_t slot) const { return slot == 0; }

    // Where UIs list this operator: "source", "grow", "shape", "look",
    // "field", "utility"... Empty: unsorted.
    virtual std::string category() const { return {}; }

    // Sets every param not set yet to its spec default (its own and the common ones,
    // allParamSpecs), and hands the params the defaults the getters fall back to (ParamSet).
    void applyDefaults() {
        auto defaults = std::make_shared<std::map<std::string, ParamValue>>();
        auto limits = std::make_shared<std::map<std::string, ParamSet::Limits>>();
        for (const ParamSpec& spec : allParamSpecs(*this)) {
            if (!params_.has(spec.name)) params_.set(spec.name, spec.defaultValue);
            defaults->emplace(spec.name, spec.defaultValue);
            if (spec.min || spec.max) limits->emplace(spec.name, ParamSet::Limits{spec.min, spec.max});
        }
        params_.setDefaults(std::move(defaults));
        params_.setLimits(std::move(limits));
    }

    // Versions (per operator type). version(): the newest behaviour this build's operator has (1
    // until a change of what it makes would alter saved grooms; then the operator bumps it and
    // keeps the old behaviour for nodes saved before, reading nodeVersion()). nodeVersion(): the
    // behaviour this node evaluates with, which a graph's JSON stores per node ("version") and a
    // host's Upgrade raises to version(). A new node takes version().
    virtual int version() const { return 1; }
    int nodeVersion() const { return nodeVersion_ > 0 ? nodeVersion_ : version(); }
    void setNodeVersion(int v) { nodeVersion_ = v; }

    ParamSet& params() { return params_; }
    const ParamSet& params() const { return params_; }

    // The node's name in the groom's graph (Graph::setLabel), inside a compound
    // "compound/inner" (the compound sets its inner nodes'). Empty for unnamed nodes. What
    // reads it (Cards names its card layer by it) salts its cache key with it.
    const std::string& nodePath() const { return nodePath_; }
    void setNodePath(std::string path) { nodePath_ = std::move(path); }

private:
    ParamSet params_;
    std::string nodePath_;
    int nodeVersion_ = 0;  // 0: version()
};

// Creates operators by type name, used by graph loading and host UIs.
class OperatorRegistry {
public:
    using Factory = std::function<std::unique_ptr<Operator>()>;

    static OperatorRegistry& instance();

    // `info`: what the add-node menu and the param panel show of it (node_catalog.h); a test's
    // own operator may pass {}.
    void registerType(const std::string& typeName, NodeInfo info, Factory factory);
    // Drops a type (a test's own operator, registered for the test only).
    void unregisterType(const std::string& typeName) { types_.erase(typeName); }
    bool contains(const std::string& typeName) const;
    std::unique_ptr<Operator> create(const std::string& typeName) const;
    std::vector<std::string> typeNames() const;
    // The info a type registered with; null for a type it does not know.
    const NodeInfo* info(const std::string& typeName) const;

private:
    struct Type {
        Factory factory;
        NodeInfo info;
    };
    std::map<std::string, Type> types_;
};

// ---------------------------------------------------------------------------
// Graph
// ---------------------------------------------------------------------------

using NodeId = std::uint32_t;

// A DAG of operators with pull-based, cached evaluation (PG-1, PG-3).
//
// Each node's cache key hashes its type, params, the context and its inputs'
// keys. Evaluating a node recomputes only nodes whose key changed, so editing
// one parameter re-runs that node and its downstream nodes only.
class Graph {
public:
    NodeId addNode(std::unique_ptr<Operator> op);
    NodeId addNode(const std::string& typeName);  // via OperatorRegistry
    void removeNode(NodeId id);                   // also removes its connections
    bool hasNode(NodeId id) const { return nodes_.count(id) != 0; }
    std::vector<NodeId> nodeIds() const;

    Operator& node(NodeId id);
    const Operator& node(NodeId id) const;

    // Connects src's output `output` (an index into its operator's outputs(); 0: the primary)
    // to dst's input slot. Throws GraphError on a type mismatch, bad slot or output, or if the
    // connection would create a cycle.
    void connect(NodeId src, NodeId dst, std::size_t inputSlot, std::size_t output = 0);
    // The node's output ports (its operator's outputs(): the primary first).
    std::vector<PortSpec> outputSpecs(NodeId id) const { return node(id).outputs(); }
    // The index of the node's output named `name`; throws GraphError when it has none.
    std::size_t outputIndex(NodeId id, const std::string& name) const;

    // Exposed params (PG-7): each named param gets an input slot after the
    // operator's own inputs, so another node can drive it (paramPortSpec): a Field slot for a
    // mappable number or a color field, a Curve slot for a ramp param, a Value slot for any other
    // (a count, a seed, a switch, a choice, a vector...). Params must exist (nodeParamSpecs), take
    // a port and not share a name with an input. Replacing the list drops the old param slots'
    // connections.
    //
    // A Value plugged into a param sets it for every evaluation (before the node's key is made, so
    // the node re-evaluates as for an edit of the param, and a GPU preview reads the node's params
    // as set); the param keeps its own value (storedParams, what a graph's JSON saves) and takes it
    // back when the link goes. A param that must stay the same on every frame (not animatable)
    // takes a Value that does not change over time (UniformValue::varies): one that does is the
    // node's error. A Value plugged into a Field slot is that number on every strand.
    void setParamInputs(NodeId id, std::vector<std::string> params);
    const std::vector<std::string>& paramInputs(NodeId id) const { return nodeRecord(id).paramInputs; }
    // The operator's inputs followed by one Field, Curve or Value port per exposed param.
    std::vector<PortSpec> inputSpecs(NodeId id) const;
    // Whether a Value input is plugged into the param (its value then comes from there).
    bool paramDriven(NodeId id, const std::string& param) const;

    // Keyed params: each linked param takes the value of the host's control of that name (the
    // external input controlInputName(name), field.h; a vec3 takes three, x y z or r g b) on
    // every evaluation, so a host keys any number, int, bool or vec3 param in place (Maya: a
    // keyable ctl_<name> attribute on the groom). Where the host sets no such control the param
    // keeps its own value. Throws GraphError for an unknown param, one whose spec is not
    // animatable, a text, ramp or map param, or a count of names that does not fit (1, or 3 for
    // a vec3). Replacing the list drops the old links.
    void setParamControls(NodeId id, std::map<std::string, std::vector<std::string>> links);
    const std::map<std::string, std::vector<std::string>>& paramControls(NodeId id) const {
        return nodeRecord(id).paramControls;
    }
    // The node's params as stored: a linked param at its own value, not its control's
    // (which it holds after an evaluation).
    ParamSet storedParams(NodeId id) const;
    // The external inputs every link reads (a compound's key depends on its inner links).
    std::vector<std::string> controlInputs() const;

    // Bypass: the node passes its first input through unchanged (when that
    // input has the node's output type), for comparing with and without it.
    void setBypass(NodeId id, bool bypass);
    bool bypassed(NodeId id) const { return nodeRecord(id).bypass; }
    // When the node runs (EvaluationMode): out of its evaluations it is bypassed, as above.
    void setEvaluationMode(NodeId id, EvaluationMode mode) { nodeRecord(id).evaluation = mode; }
    EvaluationMode evaluationMode(NodeId id) const { return nodeRecord(id).evaluation; }
    // Bypassed in an evaluation for the viewport (EvalContext::viewport) or not: by its bypass,
    // or by its evaluation mode.
    bool bypassedFor(NodeId id, bool viewport) const { return off(nodeRecord(id), viewport); }

    // A name for messages: errors from a labelled node start "node 'name': ".
    // Also the operator's nodePath.
    void setLabel(NodeId id, std::string label) {
        Node& node = nodeRecord(id);
        node.op->setNodePath(label);
        node.label = std::move(label);
    }
    const std::string& label(NodeId id) const { return nodeRecord(id).label; }

    // Milliseconds each node's operator took in the last evaluation: 0 for a node whose cached
    // result served (nodes that never ran are absent). What a profile of one evaluation shows.
    const std::map<NodeId, double>& lastEvaluationTimes() const { return lastTimes_; }
    // Milliseconds each node's operator took the last time it ran, cache hits since left out of
    // it (nodes that never ran are absent): what a node costs when it does run.
    const std::map<NodeId, double>& lastRunTimes() const { return lastRunTimes_; }
    // How each node made its result the last time it ran: "full", "full N" (in full, its result
    // differing from its last one in N strands: Graph::setDiffFullResults), "incremental N" (N
    // strands made again; "incremental all"), "bypass" (its input handed on). Profiles and tests.
    const std::map<NodeId, std::string>& lastRunKinds() const { return lastRunKinds_; }
    // Each node's Operator::note from the last time it ran (nodes with none are absent).
    const std::map<NodeId, std::string>& lastNotes() const { return lastNotes_; }
    // The strand resolution a node is evaluated with when the context leaves it to the graph
    // (EvalContext::strandSegments 0): the most segments any Resample downstream of it asks for
    // (one not bypassed in the viewport or in renders, as `viewport` says, nor fed guides; with an
    // Output node, one on the way to it), else (with no Output node) the most a Resample fed nothing
    // asks for, and the Resample asking it (`segments` 0 and `from` 0: none, a Scatter there grows 8).
    struct Resolution {
        int segments = 0;
        NodeId from = 0;  // (kNoNode)
    };
    Resolution strandResolution(NodeId id, bool viewport = true) const;
    // Every node's (strandResolution), in one pass.
    std::map<NodeId, Resolution> strandResolutions(bool viewport) const;
    // The nodes whose results reach an Output node; empty for a graph without one (a Compound's
    // inner graph), where every node counts. A Resample outside it sets no resolution.
    std::set<NodeId> nodesReachingOutput() const;
    // A Scatter's or a Resample's resolution in plain words, for the graph editors (empty for any
    // other node, a bypassed one, or a Resample fed guides).
    std::string strandResolutionNote(NodeId id, bool viewport = true) const;
    // A node's output type as connected (its output `output`, the primary by default): a Curves
    // output takes the type of what feeds its first Curves input (Curves while that is
    // unconnected). Throws GraphError for an output the node does not have.
    PortType outputType(NodeId id, std::size_t output = 0) const;
    // The strand channels in a node's cached (primary) result: what nodes downstream can
    // read through attribute maps. False when it has no cached strands.
    bool cachedChannels(NodeId id, std::vector<std::string>* names) const;
    // A node's cached (primary) result as the last evaluation left it; empty when it has none.
    // No evaluation: what a host reads to run the nodes after it elsewhere (a GPU preview). As the
    // node made it: a preview's hidden core strands and the roles' channel in it (preview_core.h).
    Value cachedValue(NodeId id) const;
    // The same for one of its outputs (`output`: which, as inputSource names it).
    Value cachedValue(NodeId id, std::size_t output) const;
    // What an evaluation handed its caller of that cached result (shownStrands: without hidden
    // strands or roles), the very value, when the caller was handed it; else cachedValue.
    Value shownCachedValue(NodeId id, std::size_t output = 0) const;
    void disconnect(NodeId dst, std::size_t inputSlot);
    // Returns true and sets *src (and *output: which of src's outputs) when dst's slot is connected.
    bool inputSource(NodeId dst, std::size_t inputSlot, NodeId* src, std::size_t* output = nullptr) const;

    // The operators whose results make `id`'s as the graph is set: `id` and, depth first
    // from it (the first input first, so its whole first-input chain comes before any side
    // branch), what feeds its strands and curves inputs, each node once. A bypassed node
    // passing its first input on is skipped (its input is walked), a Switch's unpicked input
    // is not walked (Operator::inputActive), and a compound's inner nodes that make its
    // result come right after it (Operator::forEachActiveInner). Unconnected nodes, field
    // and guide inputs never show. Paths start with `prefix`. False: `visit` stopped it.
    bool forEachActiveUpstream(NodeId id, const ActiveNodeVisitor& visit, const std::string& prefix = {}) const;

    // Evaluates a node, reusing cached results where keys are unchanged: its primary output.
    Value evaluate(NodeId id, const EvalContext& ctx = {});
    // One output of a node (GraphError for one it does not have), and all of them. A node runs
    // once for all its outputs: its cache holds every one.
    Value evaluateOutput(NodeId id, std::size_t output, const EvalContext& ctx = {});
    std::vector<Value> evaluateOutputs(NodeId id, const EvalContext& ctx = {});
    StrandsPtr evaluateStrands(NodeId id, const EvalContext& ctx = {});

    void clearCache();
    // Takes another graph's cached results for this graph's next evaluation:
    // nodes whose content key matches (same operator, params, inputs and
    // external data) reuse them instead of running. For a graph reloaded
    // after an edit, only what the edit reaches re-evaluates. Each node takes
    // the kept work of the node of its name and type there (adoptKeptWork), so
    // what it keeps for the results it reuses (Interpolate's binding) stays.
    void adoptCache(const Graph& previous);

    // A copy: the same nodes (each operator made again by its type through OperatorRegistry,
    // so a node added as an operator of an unregistered type throws GraphError), params,
    // links, exposed and keyed params, labels and bypasses, and the same cached results
    // (shared: values are immutable), so the copy's first evaluation reuses whatever is still
    // valid. From then on the two caches are their own. Operators' own kept work for the results
    // the copy shares comes along (Operator::adoptKeptWork: an Interpolate's binding, a Clump's
    // center choice, a Scatter's relaxed roots). Cheaper than writing
    // the graph's JSON and loading it again: hosts keeping several documents of one graph (a
    // card, rachis or rest evaluation) copy the first.
    Graph clone() const;

    // Off: nothing is cached, and each intermediate result is released as
    // soon as its last consumer has used it, which keeps peak memory near two
    // strand sets (render-time expansion, where nothing re-evaluates).
    void setCacheIntermediates(bool enabled);

    // A byte limit for the cached results (0: none). After each evaluation, while the arrays the
    // nodes' cached strands hold come to more (each array once: a result sharing another's points
    // adds nothing for them), results go: first those the evaluation did not reach, the longest unused
    // first, then those it reached, the cheapest to make again for the bytes they free first (by their
    // last run's time); never the evaluated node's. A node let go evaluates in full when next asked,
    // as do the nodes after it (they have no changes to go on). By default GROOMLAB_CACHE_LIMIT_MB
    // megabytes from the environment, else kDefaultCacheLimit.
    static constexpr std::size_t kDefaultCacheLimit = std::size_t{2048} << 20;
    void setCacheLimit(std::size_t bytes) { cacheLimit_ = bytes; }
    std::size_t cacheLimit() const { return cacheLimit_; }
    // The bytes the cached strands hold, as the limit counts them; how many results it let go so far.
    std::size_t cachedBytes() const;
    std::size_t cacheEvictions() const { return evictions_; }

    // Number of Operator::evaluate calls so far (cache effectiveness, tests).
    std::size_t evaluationCount() const { return evaluationCount_; }
    // Of those, the ones that ran incrementally (Operator::evaluateIncremental).
    std::size_t incrementalCount() const { return incrementalCount_; }
    // Counted as if made here (a copy of a graph taking that graph's place: the evaluations the
    // graph made before).
    void addCounts(std::size_t evaluations, std::size_t incrementals) {
        evaluationCount_ += evaluations;
        incrementalCount_ += incrementals;
    }

    // Incremental evaluation: a node whose own key stayed while some strands of its inputs
    // changed asks its operator to make only what those reach (Operator::evaluateIncremental);
    // results are the full evaluation's, bit for bit, and keyed as ever. Process-wide, on by
    // default; off, every node whose key changed evaluates in full.
    static void setIncrementalEvaluation(bool on);
    static bool incrementalEvaluation();
    // Testing: each incremental result is checked against a full evaluation of the same inputs
    // (contentHash), and the strands it says it left alone against the node's last result; a
    // difference throws GraphError naming the node. Process-wide, off by default.
    static void setVerifyIncremental(bool on);
    // A node that evaluated in full while its own key stayed and its inputs changed in only some
    // strands (an operator with no incremental path, or one that could not take it: Tangle,
    // BlendNeighbors, a Compound) compares its result with its last one strand by strand, when the
    // two hold the same strands, and hands the strands that differ on as its changes, so the nodes
    // after it stay incremental (2.5 ms at 500k strands of 16 points, 1% of them moved). Process-wide, on by
    // default; off for timings.
    static void setDiffFullResults(bool on);

    // Each new result of a node gets a serial (a cache hit keeps it), unique in the process: what a
    // host compares to know whether the strands it holds are a node's current ones (0: never
    // evaluated). A result taken into another document (clone, adoptCache) keeps its serial, so
    // one held from another document names that very result or none.
    std::uint64_t resultSerial(NodeId id) const;
    // How the node's result differs from its result of serial `since` (null: not known, as when
    // that is not the result just before it). The same serial: no changes. Of the strands its
    // callers are handed (shownStrands): known only between results hiding the same strands;
    // `asMade`: of the result as the node made it (cachedValue's).
    ChangesPtr changesSince(NodeId id, std::uint64_t since, bool asMade = false) const;

private:
    struct Node {
        std::unique_ptr<Operator> op;
        std::vector<NodeId> inputs;  // one per input slot, kNoNode if unconnected
        std::vector<std::size_t> inputOutputs;  // per input slot: which output of its source feeds it
        std::vector<std::string> paramInputs;  // exposed params, in slot order after the op's inputs
        std::map<std::string, std::vector<std::string>> paramControls;  // keyed params: param -> control names
        struct Keyed {
            ParamValue own;      // the param's own value (what it was before the controls set it)
            ParamValue applied;  // what the last evaluation set it to
            bool hasApplied = false;
        };
        std::map<std::string, Keyed> keyed;  // per linked param
        std::map<std::string, Keyed> driven;  // per param a Value input sets (applyValueInputs)
        bool drivenVaries = false;  // a Value setting one of its params changes over time
        bool bypass = false;
        EvaluationMode evaluation = EvaluationMode::Always;
        std::string label;
        std::uint64_t cachedKey = 0;
        bool hasCache = false;
        std::vector<Value> cachedValues;  // one per output, from one evaluation
        // Incremental evaluation: the result's serial, the node's own key (computeKey without
        // the inputs' keys) and each input's serial it was made from; how each output differs
        // from the result of serial changesBase (the one before it); whether it is input 0's
        // value as it came (Output, a bypass, a Resample with nothing to do).
        std::uint64_t serial = 0;
        std::uint64_t ownKey = 0;
        std::vector<std::uint64_t> consumed;
        std::vector<ChangesPtr> changes;  // per output (missing: unknown)
        std::uint64_t changesBase = 0;
        bool passedOn = false;
        std::uint64_t usedTick = 0;  // the last evaluation reaching it (the cache limit's age)
        // What the callers were handed of the result (preview_core.h: shownStrands): per output,
        // the shown strands made from cachedValues (by identity), and for the result's serial
        // and the one before, which strands were hidden (a hash; 0: none), so changesSince
        // answers only between results showing the same strands.
        std::vector<Value> shown;
        std::vector<std::weak_ptr<const StrandSet>> shownFrom;
        std::shared_ptr<const std::vector<std::uint32_t>> shownAt;  // output 0: shownIndices
        std::uint64_t hiddenSerial[2] = {0, 0};
        std::uint64_t hiddenHash[2] = {0, 0};

        void resizeInputs(std::size_t n) {
            inputs.resize(n, 0);
            inputOutputs.resize(n, 0);
        }
    };

    static constexpr NodeId kNoNode = 0;

    // Bypassed in this evaluation: its bypass, or its evaluation mode for the viewport or not.
    static bool off(const Node& node, bool viewport) {
        return node.bypass || (node.evaluation == EvaluationMode::ViewportOnly && !viewport) ||
               (node.evaluation == EvaluationMode::RenderOnly && viewport);
    }

    Node& nodeRecord(NodeId id);
    const Node& nodeRecord(NodeId id) const;
    bool reaches(NodeId from, NodeId target) const;  // is target upstream of or equal to from
    // Sets every linked param from its controls in ctx's externals (its own value without them).
    void applyControls(const EvalContext& ctx);
    // Puts back the own value of each param no Value input sets any more (its link cut).
    void releaseDriven(Node& node);
    // Whether the node's param slot `slot` (after its operator's inputs) takes a Value port.
    bool valueSlot(const Node& node, std::size_t k) const;
    // Every node's key in one evaluation, and its own key (all but its inputs' keys).
    struct Keys {
        std::map<NodeId, std::uint64_t> full, own;
        // Each node's strand segments (strandResolutions), when the context leaves them to the
        // graph; empty when it sets them for every node.
        std::map<NodeId, Resolution> segments;
    };
    // The context node `id` is evaluated (and keyed) with: its own strand segments.
    EvalContext nodeContext(NodeId id, const EvalContext& ctx, const Keys& keys) const;
    std::uint64_t computeKey(NodeId id, const EvalContext& ctx, Keys& memo) const;
    struct Transient;  // per-evaluation results when not caching
    // Sets the params of `id` and every node upstream of it that a Value input drives, from those
    // inputs' values (evaluated here, upstream first, so every node's key is made from its params
    // as they evaluate). `done`: the nodes already set in this evaluation.
    void applyValueInputs(NodeId id, const EvalContext& ctx, Keys& keys, Transient* transient, std::set<NodeId>& done);
    // How input `slot` of `node` (reading output `output` of `source`) changed since the value
    // the node's last result was made from (null: unknown).
    ChangesPtr inputChanges(const Node& node, std::size_t slot, const Node& source) const;
    // Every output of the node (one evaluation).
    std::vector<Value> evaluateWithKeys(NodeId id, const EvalContext& ctx, Keys& keys, Transient* transient);
    // The values a caller is handed of node `id`'s result `values` (EvalContext::keepPreviewCore).
    std::vector<Value> shownValues(NodeId id, std::vector<Value> values, bool cached);

    std::map<NodeId, Node> nodes_;
    NodeId nextId_ = 1;
    std::size_t evaluationCount_ = 0;
    std::size_t incrementalCount_ = 0;
    std::map<NodeId, double> lastTimes_, lastRunTimes_;
    std::map<NodeId, std::string> lastRunKinds_;
    std::map<NodeId, std::string> lastNotes_;
    // adoptCache results, by key, until the next evaluate: the values with the serial they carry
    // and their inputs' serials when they were made. A serial names a result, so an adopted one
    // keeps its own (as clone() does): what reads it, adopted too, sees it unchanged, and the
    // nodes after an edit go on incrementally from it.
    struct Adopted {
        std::vector<Value> values;
        std::uint64_t serial = 0;
        std::vector<std::uint64_t> consumed;
        bool passedOn = false;
    };
    std::unordered_map<std::uint64_t, Adopted> adopted_;
    static void adopt(Node& node, const Adopted& adopted, std::uint64_t key, std::uint64_t ownKey);
    bool cacheIntermediates_ = true;
    // The cache limit (setCacheLimit): the evaluations so far, the results it let go.
    static std::size_t defaultCacheLimit();
    std::size_t cacheLimit_ = defaultCacheLimit();
    std::uint64_t tick_ = 0;
    std::size_t evictions_ = 0;
    // Lets results go while the cache holds more than the limit (never node `keep`'s).
    void trimCache(NodeId keep);
    static void dropCache(Node& node);
};

}  // namespace groomlab
