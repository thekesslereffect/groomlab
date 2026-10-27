// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include "groomlab/expression.h"
#include "groomlab/graph.h"
#include "groomlab/subdivision.h"
#include "groomlab/texture.h"

namespace groomlab {

// A per-strand scalar parameter (PG-2): a constant, or read from a map.
//
// A param holding a number is a constant. A param holding a MapRef reads:
//   texture   - an image sampled at the strand root's UV (the scalp needs UVs)
//   colorSet  - a color set on the scalp, at the root
//   attribute - a channel written by an earlier operator (a point channel of
//               the name first, else the strand channel); where none exists
//               yet (before Interpolate) and guides are given, the guide
//               layer of that name, blended at the root
//   expression - a per-strand expression (expression.h)
//   guide     - a guide attribute layer: the strand channel of that name when
//               Interpolate has written it, else blended at the root from the
//               nearest guides (needs `guides`; e.g. Scatter density)
// then picks a component (r, g, b, a or luminance) and remaps [0, 1] to
// [min, max]. Construction loads the texture; evaluation is thread-safe.
//
// Some maps vary along each strand (perPoint()): an attribute map naming a
// POINT channel of the strands, an expression of t, pt, px/py/pz, pointIndex
// or a point channel, or a field node that does. Operators that shape or
// color points read those with atPoint() inside their loops, and at() (the
// root's value) otherwise.

// The same value for every strand.
FieldPtr constantField(float value);

// The external input a Control node named `name` reads: a hosts sets it (a
// constantField) to drive the node, e.g. from a keyable attribute.
inline std::string controlInputName(const std::string& name) { return "control:" + name; }

class ScalarField {
public:
    // Param `name` of `params`: its number (else its spec default, ParamSet), a map or a plugged
    // field. Throws GraphError when a map cannot be used (missing file, no UVs, ...).
    ScalarField(const ParamSet& params, const std::string& name, const ScalpMesh* scalp,
                const GuideSet* guides = nullptr);

    bool isConstant() const { return !hasMap_; }
    // True when a strand's value depends on that strand alone (its root, ID, channels and points)
    // and on nothing of the rest of the set: what an operator that makes again only some strands
    // needs (Operator::evaluateIncremental). Not so for an expression reading `longest` (the
    // longest strand's length, which a dab may change); `index` (the strand's place in the set)
    // counts as the strand's own: an incremental evaluation is made only on the strands the last
    // result was made from, in the same places (StrandChanges), where every strand's index is what
    // it was. A field node plugged in says for itself (FieldValue::strandLocal).
    bool strandLocal() const {
        if (field_) return field_->strandLocal();
        return !(hasMap_ && map_.source == MapRef::Source::Expression && expression_.readsLongest());
    }
    // When all it reads of the rest of the set is `longest`, the longest strand's length its last
    // validate() read; else negative (FieldValue::longestRead).
    float longestRead() const {
        if (field_) return field_->longestRead();
        return strandLocal() ? -1.0f : bound_.longest;
    }

    // Largest value the field can take (the constant, or the map's max).
    float maximum() const;
    // Whether a strand's value can change as the strands' points move: an expression of the
    // strand's shape, `longest`, t or the point's position, or a plugged field that reads them.
    // False for constants, textures, color sets, guide layers and channels (roots, IDs, channels).
    bool readsPositions() const;
    // Whether a strand's value can change as its widths or channels do (an attribute or guide
    // map, an expression reading a channel, a plugged field that may): what an operator whose
    // input changed only those (StrandChanges::pointsKept) asks before keeping what it made.
    bool readsAttributes() const;

    // Value for strand s of `strands` (attribute maps need the channel there).
    // A per-point map gives its root value.
    float at(const StrandSet& strands, std::size_t s) const;
    // Value at point `point` of strand s: the same as at() unless perPoint().
    float atPoint(const StrandSet& strands, std::size_t s, std::size_t point) const;
    // True when the value varies along each strand (after validate(), which
    // decides it for the strands at hand: a point channel, a point expression).
    bool perPoint() const;

    // Value at a scalp point, before strands exist: a constant, a plugged
    // field, a texture, color set or expression map, or a guide or attribute
    // map blended from the guides. Throws GraphError for a guide or attribute
    // map without guides to blend.
    float atRoot(const StrandRoot& root) const;
    // The same at a root of the scalp's cage (CageBinding): what Scatter, placing roots on the
    // cage, reads. Texture, color set and guide maps are read on the cage either way, so a map
    // gives each strand the same value whatever the scalp's subdivision.
    float atCageRoot(const StrandRoot& onCage) const;

    // Throws GraphError if an attribute map's channel is missing from
    // `strands`. Call before parallel loops, so at() never throws inside them.
    void validate(const StrandSet& strands) const;

    // What it reads, for a GPU evaluating it (groomlab_gpu's look_chain.cpp): its map (null: a
    // number or a plugged field), its expression (a map of source expression; bound to the strands
    // last validated: boundExpression), the field plugged in and the number it multiplies.
    const MapRef* map() const { return hasMap_ && !field_ ? &map_ : nullptr; }
    const Expression* expression() const {
        return hasMap_ && !field_ && map_.source == MapRef::Source::Expression ? &expression_ : nullptr;
    }
    const Expression::Bound& boundExpression() const { return bound_; }
    const FieldPtr& plugged() const { return field_; }
    float scale() const { return scale_; }

private:
    float remap(const ScalpMesh::Color& c) const;
    float mapOnCage(const StrandRoot& onCage) const;  // a texture, color set or guide map
    // The point channel an attribute map reads on `strands`, if any.
    const std::vector<float>* pointChannelOn(const StrandSet& strands) const;
    // The strand channel an attribute or guide map reads on `strands`, if any.
    const std::vector<float>* strandChannelOn(const StrandSet& strands) const;

    float constant_ = 0.0f;
    bool hasMap_ = false;
    MapRef map_;
    const ScalpMesh* scalp_ = nullptr;
    CageBinding cage_;  // where texture, color set and guide maps are read
    TexturePtr texture_;
    Expression expression_;
    FieldPtr field_;  // a field node plugged into the exposed param: the param's own number times it
    float scale_ = 1.0f;  // that number (1 when the param's own value is a map)
    struct GuideSampler;
    std::shared_ptr<const GuideSampler> guideSampler_;
    // Channels of the strand set last validated (kept alive by the caller), for expressions in loops.
    mutable const StrandSet* boundStrands_ = nullptr;
    mutable std::size_t boundPoints_ = 0;
    mutable Expression::Bound bound_;
    mutable const std::vector<float>* boundPointChannel_ = nullptr;  // an attribute map's point channel there
    mutable const std::vector<float>* boundStrandChannel_ = nullptr;  // an attribute or guide map's strand channel there
    std::string param_;
};

// A per-strand vector parameter (a direction): its vec3, or an expression giving a vector
// (ParamSpec::vectorExpression; a MapRef of source expression, e.g. "normalize(N + [0, -1, 0])").
// Evaluation is thread-safe; validate() before parallel loops, as ScalarField.
class VectorField {
public:
    // Throws GraphError for a map that is not an expression, or an expression giving a number.
    VectorField(const ParamSet& params, const std::string& name, const ScalpMesh* scalp);

    bool isConstant() const { return !hasExpression_; }
    // The param's vec3 (its spec default while it holds an expression).
    const Vec3f& constant() const { return constant_; }
    bool perPoint() const;
    bool readsPositions() const { return hasExpression_ && expression_.readsPositions(); }
    bool strandLocal() const { return !(hasExpression_ && expression_.readsLongest()); }
    void validate(const StrandSet& strands) const;
    Vec3f at(const StrandSet& strands, std::size_t s) const;
    Vec3f atPoint(const StrandSet& strands, std::size_t s, std::size_t point) const;
    Vec3f atRoot(const StrandRoot& root) const;

private:
    Vec3f constant_;
    bool hasExpression_ = false;
    Expression expression_;
    const ScalpMesh* scalp_ = nullptr;
    std::string param_;
    mutable const StrandSet* boundStrands_ = nullptr;
    mutable Expression::Bound bound_;
};

// A per-strand color parameter (ParamSpec::colorField): its linear RGB, times the color of a field
// node plugged into it (a Map's rgb output: a painted or imported color map read at each root;
// another field's value as gray). Evaluation is thread-safe; validate() before parallel loops.
class ColorField {
public:
    ColorField(const ParamSet& params, const std::string& name);
    bool isConstant() const { return !field_; }
    const std::array<float, 3>& constant() const { return constant_; }
    bool perPoint() const { return field_ && field_->perPoint(); }
    bool strandLocal() const { return !field_ || field_->strandLocal(); }
    void validate(const StrandSet& strands) const {
        if (field_) field_->validate(strands);
    }
    std::array<float, 3> at(const StrandSet& strands, std::size_t s) const;
    std::array<float, 3> atPoint(const StrandSet& strands, std::size_t s, std::size_t point) const;

private:
    std::array<float, 3> constant_{};
    FieldPtr field_;
};

// The environment expressions compile in while the graph evaluates in `ctx` (expression.h): its
// time (the frame by ctx.framesPerSecond, frame_time.h) and the groom's maps by name, found as a
// Map node finds them (the "maps" catalog, the scalp's color sets, the guides' layers). What it
// finds holds what it reads (no reference to ctx is kept).
ExpressionEnvironment expressionEnvironment(const EvalContext& ctx);
// What an expression's text adds to the cache key of a node holding it, in ctx: the time and
// frame rate when it reads the time, and each map it names (its catalog entry, texture stamp,
// color set or guide layers). 0 when it reads neither: a time-independent expression keeps its
// node cached from frame to frame.
std::uint64_t expressionSalt(const std::string& text, const EvalContext& ctx);

}  // namespace groomlab
