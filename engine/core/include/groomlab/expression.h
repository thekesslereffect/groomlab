// SPDX-License-Identifier: Apache-2.0
#pragma once

// Per-strand expressions for parameters (PG-2). The language follows SeExpr (as Maya XGen uses
// it), so an XGen expression pastes in with few or no changes, e.g.
//   "0.5 + 0.5 * sin($u * 6.2832)"            stripes around the scalp
//   "smoothstep($y, 0.2, 0.6)"                more toward the top (SeExpr's (x, a, b) order)
//   "$clumpId > 10 ? 1 : 0.3"                 per clump (a channel)
//   "$a = noise($P * 4); $a->contrast(0.7)"   locals, the apply operator ->
//
// Grammar (SeExpr's; precedence lowest first): x -> f(a) is f(x, a); c ? a : b; ||; &&; == !=;
// < <= > >=; + -; * / % (C's fmod); unary - ! ~ (~x is 1 - x); ^ (power, right-associative);
// v[i]; [x, y, z]. Comparisons and logic give 1 or 0. A block: statements `$name = expression;`
// (also += -= *= /= %= ^=) and `if (c) { ... } else if (c) { ... } else { ... }` of them make
// locals for the lines below, and the last expression is the value (comments: # or //).
// Names may be written with a `$` or without: `$length` is `length`. Constants (the Expression
// node's own variables) are passed in; a vector is three of them, `dir.x`, `dir.y`, `dir.z`, read
// as `$dir.x` and so on, or `$dir` whole. Strings appear only as map names: map("name").
//
// Variables:
//   u, v        root UV on the scalp (0 without UVs)
//   x, y, z     root position          nx, ny, nz   root normal
//   rand        stable random number in [0, 1) per strand (from its ID)
//   id          the strand's ID, its low 24 bits (a whole number)     faceId   the root's triangle
//   prefx, prefy, prefz   the root on the scalp's rest pose (where it is without one); Pref whole
//   index      strand position in the set, 0 to 1 (its place among the set's strands: strands
//               added or removed before it change it, and so does a thinned preview, which
//               holds fewer: the one variable a preview gives otherwise than the render)
//   length      the strand's length (along it)
//   longest     the set's longest strand, of its core strands (preview_core.h: the same at every
//               preview density; a strand edited longer outside them leaves it)
//   reach       root to tip, straight            straightness   reach / length (1: straight)
//   lift        how far it points off the scalp: 1 straight out, 0 along it, below 0 into it
//   tipx, tipy, tipz   tip position
//   (the strand shape variables read 0 where strands don't exist yet, e.g. Scatter density)
//   Along the strand (per point; an expression using one is evaluated per point
//   by the operators that shape or color points, see perPoint()):
//   t           0 at the root to 1 at the tip, by arc length
//   pt          0 at the root to 1 at the tip, by point index
//   px, py, pz  the point's position       pointIndex   the point's index on its strand
//   frame (also F)   the evaluated frame (fractional between frames)
//   time        the evaluated time in seconds (frame_time.h's rule turns one into the other)
//   (an expression reading the time re-evaluates on every frame; one that does not stays cached)
//   XGen's names: cLength is length; Pg, Pw, Pgw are P (the scalp is in world space, undisplaced);
//   Ng is N; Prefg, Prefw, Prefgw are Pref; faceid is faceId. XGen names without an equivalent
//   ($Nref, $dPdu, $cWidth, $Cs, ...) are errors that say so.
//   any other name: the strand channel of that name (e.g. clumpId), or the
//   point channel of that name (per point) when the strands carry one instead
// Vectors: [x, y, z] makes one; v.x v.y v.z (or .r .g .b) and v[i] (i any number expression: 0, 1,
//   2, else 0) read a component; + - * / % ^ work on vectors component by component, with a
//   number on either side applied to every component; c ? a : b picks between vectors. Built-in
//   vectors: P (the root's position: x, y, z), N (the root's normal), Pref, tip (tipx, tipy, tipz),
//   pointP (px, py, pz, per point). A node's own vector variable reads as a whole: $center.
// Functions: SeExpr's builtins as SeExpr defines them (kFns in expression.cpp lists every one,
//   Expression::functions() the ones written by name; web/content/docs/expressions.mdx describes
//   each, and a test checks it names them all): noise() 0 to 1, snoise() -1 to 1, fbm()
//   0.5 + 0.5 * an unnormalized sum, smoothstep(x, a, b), remap(x, source, range, falloff, interp),
//   voronoi(p, type 1-5, jitter 0.5, ...), curve(t, p0, v0, i0, ...), spline(t, y1, ..., yn) (y1
//   and yn end controls), rand() / rand(min, max[, seed]) per strand. A number argument where a
//   vector is wanted is that number on every component, except a noise's point: [x, 0, 0]. Their
//   random values come from GroomLab's own hash and noise: seeded, the same on every machine and
//   thread count, but not XGen's numbers. GroomLab's own: mod (floored) step mix lerp gamma gain
//   radians degrees normalize distance linearcurve, rand(seed), and smoothstep / linearstep(e0,
//   e1, x) when e0 and e1 are constants and x is not (GLSL's order, which older GroomLab
//   expressions use); curve(t, p0, v0, p1, v1, ...) (pairs, a smooth ramp.h curve) when its
//   arguments can't be SeExpr's triples. Curve keys are constants (numbers, the node's own
//   variables). Unsupported functions (printf, getVar, ...): an error naming them.
// Maps: map("name") the groom's map of that name (the one a Map node reads: a guide layer, a
//   vertex map or a texture) at the strand's root, its luminance; map("name", "r") a channel
//   ("r", "g", "b", "a", "luminance" or 0-3 for r-a); cmap("name") (or vmap) its color as a
//   vector. An XGen path, map('${DESC}/paintmaps/length/'), reads the groom's map named by its
//   last part ("length").
//
// Expressions compile once and evaluate without allocation; evaluation is
// thread-safe and deterministic.

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "groomlab/math.h"
#include "groomlab/ramp.h"
#include "groomlab/scalp_mesh.h"
#include "groomlab/strand_set.h"

namespace groomlab {

class ExpressionError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// What an expression must give: a number (a param's value), a vector (a direction param's), or
// either (a tool asking what it is).
enum class ExpressionType { Number, Vector, Any };

// A map as an expression reads it (map("name")): its value at a strand's root.
class ExpressionMap {
public:
    virtual ~ExpressionMap() = default;
    // component: 0-3 r, g, b, a; 4 luminance. In 0-1.
    virtual float at(const StrandRoot& root, int component) const = 0;
};
using ExpressionMapPtr = std::shared_ptr<const ExpressionMap>;

// What expressions read beyond the strands: the evaluated time and the groom's maps. The graph
// sets it around each evaluation from its EvalContext (field.h, expressionEnvironment); an
// expression takes the one set on its thread when it compiles. None: time 0, map() reads 0.
struct ExpressionEnvironment {
    double seconds = 0.0;
    double frame = 0.0;
    // The map of that name; null when there is none (map("name") is then an error).
    std::function<ExpressionMapPtr(const std::string& name)> map;
    // The image file of the groom's texture map of that name (its "maps" catalog); empty when the
    // name is no texture map. A param naming a painted map reads a texture map through it
    // (ScalarField), whatever kind the map picked was.
    std::function<std::string(const std::string& name)> paintedTexture;
};
// Sets the environment for expressions compiled on this thread while it lives; nested scopes
// put the outer one back. Null: none.
class ExpressionEnvironmentScope {
public:
    explicit ExpressionEnvironmentScope(const ExpressionEnvironment* environment);
    ~ExpressionEnvironmentScope();
    ExpressionEnvironmentScope(const ExpressionEnvironmentScope&) = delete;
    ExpressionEnvironmentScope& operator=(const ExpressionEnvironmentScope&) = delete;
    static const ExpressionEnvironment* current();

private:
    const ExpressionEnvironment* saved_;
};

// What an expression's text reads beyond the strands, found without compiling it (cheap: what
// a node's cache key asks on every evaluation): the time (frame, F, time), and the maps it names
// with map("..."), cmap("...") or vmap("...") (an XGen path by expressionMapName). May say time for
// a local named so (only a needless re-evaluation).
struct ExpressionDependencies {
    bool time = false;
    std::vector<std::string> maps;
};
ExpressionDependencies scanExpression(const std::string& text);

// The groom map a map("...") names: the text itself, or for an XGen path
// ('${DESC}/paintmaps/length/', '.../length.ptx') its last part without an image extension.
std::string expressionMapName(const std::string& text);

// A curve() or ccurve() as SeExpr's: keys of a position, a value and the interpolation of the span
// to the next key (0 none: the key's value, 1 linear, 2 smooth, 3 spline (Catmull-Rom slopes),
// 4 monotone spline), sorted by position; the first key's value before it, the last's after.
struct ExpressionCurve {
    struct Key {
        float position = 0.0f;
        Vec3f value;
        int interpolation = 1;
        Vec3f slope;  // prepare()
    };
    std::vector<Key> keys;
    void prepare();  // sorts the keys (stably) and works out their slopes
    Vec3f operator()(float t) const;
};

// An Expression node's own variables, stored as JSON: {"amount": 0.5, "center": [0, 1, 0]},
// as constants for Expression (a vector as name.x/.y/.z and name.r/.g/.b). A keyable number is
// {"value": 0.5, "control": "mask_amount"}: the host's control of that name (a keyable
// attribute ctl_mask_amount in Maya) sets it, `value` where there is none. Throws
// ExpressionError for anything but numbers, keyable numbers and 3-vectors under names.
std::vector<std::pair<std::string, float>> parseExpressionVariables(const std::string& json);
// The keyable variables of that JSON: (variable, control name) pairs.
std::vector<std::pair<std::string, std::string>> expressionVariableControls(const std::string& json);

// A builtin function as the editors list it (Expression::functions).
struct ExpressionFunction {
    std::string name;
    std::string arguments;  // as the editors show them: "x, a, b"
    std::string about;      // what it does, one line
    int minArgs = 0, maxArgs = 0;
    bool vector = false;    // its value is a vector
    bool gpu = false;       // the GPU preview runs it (else a field reading it runs on the CPU)
};

class Expression {
public:
    Expression() = default;
    // Throws ExpressionError (with the position, or the line and column in a block), also when
    // its value is not of the type wanted. Reads the thread's ExpressionEnvironment (time, maps).
    explicit Expression(const std::string& text, const std::vector<std::pair<std::string, float>>& constants = {},
                        ExpressionType want = ExpressionType::Number);

    const std::string& text() const { return text_; }
    // True when its value is a vector (evaluate*Vector), false for a number.
    bool isVector() const { return vector_; }
    // True when it reads frame, F or time.
    bool readsTime() const { return readsTime_; }
    // Strand channels the expression reads (names not otherwise known).
    const std::vector<std::string>& channels() const { return channels_; }

    // The strand set's channels for channels() (and its longest strand, when
    // read), looked up once before a loop. A name the strands carry as a point
    // channel (and not a strand channel) binds to that, and the expression is
    // then per point.
    struct Bound {
        std::vector<const std::vector<float>*> channels;       // strand channels, null where absent
        std::vector<const std::vector<float>*> pointChannels;  // point channels, null where absent
        float longest = 0.0f;
        bool perPoint = false;  // a point variable or a point channel: varies along each strand
        std::uint64_t serial = 0;  // unique per bind() (copies share it): evaluatePoint's per-thread cache key
    };
    Bound bind(const StrandSet& strands) const;
    // True when the expression reads a point variable (t, pt, px, py, pz,
    // pointIndex). A bound point channel makes Bound::perPoint true as well.
    bool readsPointVariables() const { return readsPoint_; }
    // True when it reads `longest`: a value of the whole set, not of the strand alone.
    bool readsLongest() const { return readsLongest_; }
    // True when its value can change as strands' points move (a strand shape variable, `longest`,
    // t, px, py, pz); false when it reads only roots, IDs, channels and point indices.
    bool readsPositions() const { return readsShape_ || readsLongest_ || readsPointPosition_; }

    // Value for strand s. Missing channels read 0. Pass bind(strands) in loops.
    // Point variables read the root's; a point channel reads its root value.
    float evaluate(const StrandSet& strands, std::size_t s, const ScalpMesh* scalp, const Bound* bound = nullptr) const;
    // Value at point `point` of strand s. Per-strand work (root, shape, arc
    // lengths) is done once per strand and reused for its points on the same
    // thread, so calling it for every point of a strand in turn costs about
    // what evaluate() does; no allocation per point.
    float evaluatePoint(const StrandSet& strands, std::size_t s, std::size_t point, const ScalpMesh* scalp,
                        const Bound* bound = nullptr) const;
    // Value at a scalp point before strands exist: channels read 0, rand
    // comes from the root, index is 0.
    float evaluateAt(const StrandRoot& root, const ScalpMesh* scalp) const;
    // The same for a vector expression (a number expression gives [v, v, v]).
    Vec3f evaluateVector(const StrandSet& strands, std::size_t s, const ScalpMesh* scalp, const Bound* bound = nullptr) const;
    Vec3f evaluatePointVector(const StrandSet& strands, std::size_t s, std::size_t point, const ScalpMesh* scalp,
                              const Bound* bound = nullptr) const;
    Vec3f evaluateVectorAt(const StrandRoot& root, const ScalpMesh* scalp) const;

    // For a GPU running the program (groomlab_gpu's look_chain.cpp): its ops (reverse Polish:
    // program(), below), the stack slots it takes, and the codes its ops name variables, functions
    // and operators by (-1: no such name; a function's first code of that name).
    std::size_t stackDepth() const { return stackDepth_; }
    static int variableCode(const std::string& name);
    static int functionCode(const std::string& name);
    // FieldCS's code for function code `code` (shared/groom_math.h kField*: look.slang fieldCall's
    // case), -1 for a function the GPU does not run.
    static int gpuFunction(int code);
    // Every builtin written by name, in the editors' order (forms a call picks itself left out).
    static const std::vector<ExpressionFunction>& functions();
    static int binaryCode(const std::string& op);  // "+", "-", "*", "/", "^", "<", "<=", ">", ">=", "==", "!=", "%"
    // Strand s's variables as an evaluation of it sets them, kVariables floats by Op::code (the
    // strand's own: point variables at its root, as evaluate() reads them).
    static constexpr std::size_t kVariables = 31;
    void strandVariablesInto(const StrandSet& strands, std::size_t s, const ScalpMesh* scalp, const Bound& bound,
                             float* vars) const;

    struct Op {
        // Number..Select: on numbers. VUnary, VBinary (args: bit 0 left, bit 1 right is a vector),
        // VSelect, VCall (every argument a vector, a number function per component): on vectors.
        // Component (code: 0-2) takes one of a vector; Splat makes a number a vector.
        // Script nodes' programs only (script.h): Load and Store read and write `args` slots from
        // slot `channel` (a local, an output, a param); Jump goes to op `channel`, JumpIfZero
        // does when the number it takes is 0; ScriptCall runs a script function (code: its own).
        enum Kind : std::uint8_t {
            Number, Variable, Channel, Unary, Binary, Call, Select,
            VUnary, VBinary, VSelect, VCall, Component, Splat,
            Load, Store, Jump, JumpIfZero, ScriptCall
        } kind;
        std::uint8_t code = 0;     // variable, operator or function
        std::uint8_t args = 0;     // calls: the stack slots the arguments take (a vector three)
        std::uint8_t results = 1;  // calls: the slots the result takes
        std::uint16_t channel = 0; // channels; a curve's or a map's index
        float value = 0.0f;        // numbers; a map's component
    };
    const std::vector<Op>& program() const { return program_; }

private:
    struct Env;
    struct StrandCache;
    float run(const Env& env) const;
    Vec3f runVector(const Env& env) const;
    void strandVariables(Env& env, const StrandSet& strands, std::size_t s, const ScalpMesh* scalp,
                         const Bound& bound) const;
    void rootEnv(Env& env, const StrandRoot& root, const ScalpMesh* scalp) const;
    Env pointEnv(const StrandSet& strands, std::size_t s, std::size_t point, const ScalpMesh* scalp, const Bound* bound) const;

    std::string text_;
    std::vector<Op> program_;  // reverse Polish
    std::vector<std::string> channels_;
    std::vector<Ramp> ramps_;    // curve() of pairs, linearcurve()
    std::vector<ExpressionCurve> curves_;  // curve(), ccurve()
    std::vector<std::string> mapNames_;  // map(), cmap()
    std::vector<ExpressionMapPtr> maps_;  // each one's map (null: reads 0, no environment to find it in)
    double seconds_ = 0.0, frame_ = 0.0;  // the environment's when compiled
    bool vector_ = false;
    bool readsTime_ = false;
    std::size_t stackDepth_ = 0;
    bool readsRoot_ = false;     // the root's u, v, x, y, z, nx, ny, nz (or lift, from the normal)
    bool readsRef_ = false;      // the root on the scalp's rest pose (Pref)
    bool readsShape_ = false;    // any strand shape variable
    bool readsLongest_ = false;  // `longest` (a pass over the set in bind)
    bool readsPoint_ = false;    // any point variable (t, pt, px, py, pz, pointIndex)
    bool readsPointPosition_ = false;  // a point variable from the points' positions (t, px, py, pz)
};

}  // namespace groomlab
