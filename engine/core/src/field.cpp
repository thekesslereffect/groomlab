// SPDX-License-Identifier: Apache-2.0
#include "groomlab/field.h"
#include "groomlab/paths.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <map>
#include <mutex>

#include "groomlab/frame_time.h"
#include "groomlab/guide_hierarchy.h"
#include "groomlab/point_index.h"
#include "groomlab/random.h"
#include "operators/internal.h"

namespace groomlab {

// A guide attribute blended from the nearest guide roots (rest pose), as
// Interpolate blends it into strands: inverse-distance weights of 4 guides.
struct ScalarField::GuideSampler {
    std::vector<Vec3f> roots;
    std::vector<float> values;
    std::unique_ptr<PointIndex> index;
    bool carried = false;  // some guide carries the layer

    // Guide roots where they sit on the cage's rest pose (CageBinding::stablePosition).
    GuideSampler(const GuideSet& guides, const std::string& name, const CageBinding& cage) {
        for (const Guide& g : guides.guides()) {
            if (g.points.empty()) continue;
            roots.push_back(cage.stablePosition(g.root, g.points.front()));
            auto it = g.attributes.find(name);
            values.push_back(it == g.attributes.end() ? 0.0f : it->second);
            carried = carried || it != g.attributes.end();
        }
        if (!roots.empty()) index = std::make_unique<PointIndex>(roots);
    }
    float at(const Vec3f& p) const {
        if (!index) return 0.0f;
        std::array<std::size_t, 4> idx{};
        std::array<float, 4> d2{};
        const std::size_t found = index->kNearest(p, idx.size(), idx.data(), d2.data());
        float sum = 0.0f, wsum = 0.0f;
        for (std::size_t i = 0; i < found; ++i) {
            if (d2[i] <= 1e-12f) return values[idx[i]];
            const float w = 1.0f / d2[i];  // power 2
            sum += values[idx[i]] * w;
            wsum += w;
        }
        return wsum > 0.0f ? sum / wsum : 0.0f;
    }
};

ScalarField::ScalarField(const ParamSet& params, const std::string& name, const ScalpMesh* scalp,
                         const GuideSet* guides)
    : constant_(static_cast<float>(params.getDouble(name))), scalp_(scalp), param_(name) {
    if ((field_ = params.field(name))) {
        // A plugged field is multiplied by the param's own number (a 0-1 mask on a density of
        // 100 gives 0-100), so masks stay 0-1 and the param stays the amount; a param whose own
        // value is a map takes the field alone.
        hasMap_ = true;  // varies per strand
        const bool ownMap = params.has(name) && std::get_if<MapRef>(&params.get(name)) != nullptr;
        scale_ = ownMap ? 1.0f : constant_;
        // A root on the cage (Scatter's) goes to the scalp before the field reads it there: on a
        // refined scalp its face is the cage's, not the scalp's.
        cage_ = CageBinding(scalp_);
        return;
    }
    if (!params.has(name)) return;
    const ParamValue& value = params.get(name);
    const MapRef* map = std::get_if<MapRef>(&value);
    if (!map) return;  // the number (constant_)
    hasMap_ = true;
    map_ = *map;
    // Every source, expressions too: atCageRoot carries a root on the cage onto a refined scalp
    // before the expression reads it there.
    cage_ = CageBinding(scalp_);

    const std::string where = "param '" + name + "': ";
    switch (map_.source) {
        case MapRef::Source::Texture:
            if (!scalp_ || !scalp_->hasUVs()) throw GraphError(where + "a texture map needs a scalp with UVs");
            try {
                texture_ = loadTexture(resolvePath(map_.name));
            } catch (const TextureError& e) {
                throw GraphError(where + e.what());
            }
            break;
        case MapRef::Source::ColorSet:
            if (!scalp_ || !scalp_->hasColorSet(map_.name)) {
                throw GraphError(where + "the scalp has no color set '" + map_.name + "'");
            }
            break;
        case MapRef::Source::Attribute:
            // A painted guide layer of the same name stands in where no strand
            // channel exists yet (Scatter density, before Interpolate).
            if (guides) {
                auto sampler = std::make_shared<const GuideSampler>(*guides, map_.name, cage_);
                if (sampler->carried) guideSampler_ = std::move(sampler);
            }
            break;  // otherwise checked per strand set in at()
        case MapRef::Source::Expression:
            try {
                expression_ = Expression(map_.name, map_.constants);
            } catch (const ExpressionError& e) {
                throw GraphError(where + e.what());
            }
            break;
        case MapRef::Source::Guide:
            if (guides) {
                guideSampler_ = std::make_shared<const GuideSampler>(*guides, map_.name, cage_);
                if (!guideSampler_->carried && scalp_ && scalp_->hasColorSet(map_.name)) {
                    // A painted map that lives on the scalp's vertices (Groom Tools > Paint offers
                    // both kinds under one name): read it there, as a Map node would.
                    guideSampler_.reset();
                    map_.source = MapRef::Source::ColorSet;
                    break;
                }
                if (!guideSampler_->roots.empty() && !guideSampler_->carried) {
                    throw GraphError(where + "no guide carries the layer '" + map_.name + "'");
                }
            } else if (scalp_ && scalp_->hasColorSet(map_.name)) {
                map_.source = MapRef::Source::ColorSet;
            }
            break;
    }
}

float ScalarField::maximum() const {
    if (field_) return field_->maximum() * scale_;
    return hasMap_ ? std::max(map_.min, map_.max) : constant_;
}

float ScalarField::remap(const ScalpMesh::Color& c) const {
    float v = 0.0f;
    switch (map_.component) {
        case MapRef::Component::R: v = c[0]; break;
        case MapRef::Component::G: v = c[1]; break;
        case MapRef::Component::B: v = c[2]; break;
        case MapRef::Component::A: v = c[3]; break;
        case MapRef::Component::Luminance: v = 0.2126f * c[0] + 0.7152f * c[1] + 0.0722f * c[2]; break;
    }
    return map_.min + (map_.max - map_.min) * v;
}

float ScalarField::atRoot(const StrandRoot& root) const {
    if (!hasMap_) return constant_;
    if (field_) return field_->atRoot(root) * scale_;
    if (map_.source == MapRef::Source::Expression) {
        return map_.min + (map_.max - map_.min) * expression_.evaluateAt(root, scalp_);
    }
    return mapOnCage(cage_.toCage(root));
}

float ScalarField::atCageRoot(const StrandRoot& onCage) const {
    if (!hasMap_) return constant_;
    if (field_ || map_.source == MapRef::Source::Expression) return atRoot(cage_.toScalp(onCage));
    return mapOnCage(onCage);
}

float ScalarField::mapOnCage(const StrandRoot& root) const {
    const ScalpMesh* mesh = cage_.cage();
    switch (map_.source) {
        case MapRef::Source::Texture: {
            const ScalpMesh::UV uv = mesh->uvAt(root);
            return remap(texture_->sample(uv[0], uv[1]));
        }
        case MapRef::Source::ColorSet:
            return remap(mesh->colorAt(map_.name, root));
        case MapRef::Source::Expression:
            break;
        case MapRef::Source::Attribute:
            if (!guideSampler_) {
                throw GraphError("param '" + param_ + "': no strands yet, so '" + map_.name +
                                 "' must be a layer painted on the guides, with the guides connected to this node");
            }
            [[fallthrough]];
        case MapRef::Source::Guide: {
            if (!guideSampler_) {
                throw GraphError("param '" + param_ + "': a guide map here needs guides: connect them to this node's guides input");
            }
            const Vec3f p = cage_.stableCagePosition(root, Vec3f{});
            return map_.min + (map_.max - map_.min) * guideSampler_->at(p);
        }
    }
    return constant_;
}

const std::vector<float>* ScalarField::pointChannelOn(const StrandSet& strands) const {
    // Operators validate their input and read their output, a copy with the
    // same channels: the input's channel values serve for both.
    const bool same = boundStrands_ && boundStrands_->strandCount() == strands.strandCount() &&
                      boundPoints_ == strands.pointCount();
    return same ? boundPointChannel_ : strands.pointChannel(map_.name);
}

const std::vector<float>* ScalarField::strandChannelOn(const StrandSet& strands) const {
    // The same: a node rewriting the channel it reads (an Attribute remapping
    // one in place, HairColor's melanin from the melanin upstream) reads the
    // incoming values, not the defaults its copy was just reset to.
    const bool same = boundStrands_ && boundStrands_->strandCount() == strands.strandCount();
    return same ? boundStrandChannel_ : strands.strandChannel(map_.name);
}

void ScalarField::validate(const StrandSet& strands) const {
    if (field_) {
        field_->validate(strands);
        return;
    }
    if (hasMap_ && map_.source == MapRef::Source::Guide) {
        boundStrandChannel_ = strands.strandChannel(map_.name);
        boundStrands_ = &strands;
        boundPoints_ = strands.pointCount();
        if (!boundStrandChannel_ && !guideSampler_) {
            throw GraphError("param '" + param_ + "': no guide layer '" + map_.name +
                             "' on the strands (Interpolate writes it; paint it on the guides)");
        }
    }
    if (hasMap_ && map_.source == MapRef::Source::Attribute) {
        // A point channel of the name (a per-point guide layer Interpolate
        // wrote, HairColor's colors...) is read per point, before any strand channel.
        boundPointChannel_ = strands.pointChannel(map_.name);
        boundStrandChannel_ = strands.strandChannel(map_.name);
        boundStrands_ = &strands;
        boundPoints_ = strands.pointCount();
        if (!boundPointChannel_ && !boundStrandChannel_ && !guideSampler_) {
            throw GraphError("param '" + param_ + "': strands have no attribute '" + map_.name + "'");
        }
    }
    if (hasMap_ && map_.source == MapRef::Source::Expression) {
        for (const std::string& channel : expression_.channels()) {
            if (!strands.strandChannel(channel) && !strands.pointChannel(channel)) {
                throw GraphError("param '" + param_ + "': the expression reads '" + channel +
                                 "', which is neither a variable nor a channel of the strands");
            }
        }
        bound_ = expression_.bind(strands);
        boundStrands_ = &strands;
        boundPoints_ = strands.pointCount();
    }
}

bool ScalarField::readsPositions() const {
    if (!hasMap_) return false;
    if (field_) return field_->readsPositions();
    return map_.source == MapRef::Source::Expression && expression_.readsPositions();
}

bool ScalarField::readsAttributes() const {
    if (!hasMap_) return false;
    if (field_) {
        const ScalarField* inner = field_->scalarField();
        return !inner || inner->readsAttributes();  // a field of another kind: it may
    }
    switch (map_.source) {
        case MapRef::Source::Texture:
        case MapRef::Source::ColorSet: return false;
        case MapRef::Source::Expression: return !expression_.channels().empty();
        default: return true;  // an attribute map; a guide map's strand channel
    }
}

bool ScalarField::perPoint() const {
    if (!hasMap_) return false;
    if (field_) return field_->perPoint();
    if (map_.source == MapRef::Source::Expression) return boundStrands_ ? bound_.perPoint : expression_.readsPointVariables();
    if (map_.source == MapRef::Source::Attribute) return boundPointChannel_ != nullptr;
    return false;
}

float ScalarField::at(const StrandSet& strands, std::size_t s) const {
    if (!hasMap_) return constant_;
    if (field_) return field_->at(strands, s) * scale_;
    if (map_.source == MapRef::Source::Expression) {
        // Operators validate their input and read their output, a copy with the
        // same channels: the input's channel values serve for both.
        const bool same = boundStrands_ && boundStrands_->strandCount() == strands.strandCount();
        const Expression::Bound* bound = same ? &bound_ : nullptr;
        return map_.min + (map_.max - map_.min) * expression_.evaluate(strands, s, scalp_, bound);
    }
    if (map_.source == MapRef::Source::Guide) {
        if (const std::vector<float>* channel = strandChannelOn(strands)) {
            return map_.min + (map_.max - map_.min) * (*channel)[s];
        }
        return atRoot(strands.roots()[s]);
    }
    if (map_.source != MapRef::Source::Attribute) return atRoot(strands.roots()[s]);
    if (const std::vector<float>* points = pointChannelOn(strands)) {  // per point: the root's value
        return map_.min + (map_.max - map_.min) * (*points)[strands.strandBegin(s)];
    }
    const std::vector<float>* channel = strandChannelOn(strands);
    if (!channel && guideSampler_) return atRoot(strands.roots()[s]);
    if (!channel) throw GraphError("param '" + param_ + "': strands have no attribute '" + map_.name + "'");
    const float v = (*channel)[s];
    return map_.min + (map_.max - map_.min) * v;
}

float ScalarField::atPoint(const StrandSet& strands, std::size_t s, std::size_t point) const {
    if (!hasMap_) return constant_;
    if (field_) return field_->atPoint(strands, s, point) * scale_;
    if (map_.source == MapRef::Source::Expression) {
        const bool same = boundStrands_ && boundStrands_->strandCount() == strands.strandCount();
        const Expression::Bound* bound = same ? &bound_ : nullptr;
        const bool perPoint = bound ? bound->perPoint : expression_.readsPointVariables();
        if (!perPoint) return map_.min + (map_.max - map_.min) * expression_.evaluate(strands, s, scalp_, bound);
        return map_.min + (map_.max - map_.min) * expression_.evaluatePoint(strands, s, point, scalp_, bound);
    }
    if (map_.source == MapRef::Source::Attribute) {
        if (const std::vector<float>* points = pointChannelOn(strands)) {
            return map_.min + (map_.max - map_.min) * (*points)[strands.strandBegin(s) + point];
        }
    }
    return at(strands, s);
}

// ---------------------------------------------------------------------------
// Vector params

ColorField::ColorField(const ParamSet& params, const std::string& name) : field_(params.field(name)) {
    const Vec3f c = params.getVec3(name);
    constant_ = {c.x, c.y, c.z};
}

std::array<float, 3> ColorField::at(const StrandSet& strands, std::size_t s) const {
    if (!field_) return constant_;
    const std::array<float, 3> f = field_->colorAt(strands, s);
    return {constant_[0] * f[0], constant_[1] * f[1], constant_[2] * f[2]};
}

std::array<float, 3> ColorField::atPoint(const StrandSet& strands, std::size_t s, std::size_t point) const {
    if (!field_) return constant_;
    const std::array<float, 3> f = field_->colorAtPoint(strands, s, point);
    return {constant_[0] * f[0], constant_[1] * f[1], constant_[2] * f[2]};
}

std::array<float, 3> ColorField::atRoot(const StrandRoot& root) const {
    if (!field_) return constant_;
    const std::array<float, 3> f = field_->colorAtRoot(root);
    return {constant_[0] * f[0], constant_[1] * f[1], constant_[2] * f[2]};
}

float ColorField::maximum() const {
    const float c = std::max({constant_[0], constant_[1], constant_[2]});
    return field_ ? c * field_->maximum() : c;
}

VectorField::VectorField(const ParamSet& params, const std::string& name, const ScalpMesh* scalp)
    : constant_(params.getVec3(name)), scalp_(scalp), param_(name) {
    if (!params.has(name)) return;
    const MapRef* map = std::get_if<MapRef>(&params.get(name));
    if (!map) return;
    if (map->source != MapRef::Source::Expression) {
        throw GraphError("param '" + name + "': a vector takes an expression (a " + MapRef::sourceKey(map->source) +
                         " map gives one number)");
    }
    try {
        expression_ = Expression(map->name, map->constants, ExpressionType::Vector);
    } catch (const ExpressionError& e) {
        throw GraphError("param '" + name + "': " + e.what());
    }
    hasExpression_ = true;
}

void VectorField::validate(const StrandSet& strands) const {
    if (!hasExpression_) return;
    for (const std::string& channel : expression_.channels()) {
        if (!strands.strandChannel(channel) && !strands.pointChannel(channel)) {
            throw GraphError("param '" + param_ + "': the expression reads '" + channel +
                             "', which is neither a variable nor a channel of the strands");
        }
    }
    bound_ = expression_.bind(strands);
    boundStrands_ = &strands;
}

bool VectorField::perPoint() const {
    if (!hasExpression_) return false;
    return boundStrands_ ? bound_.perPoint : expression_.readsPointVariables();
}

Vec3f VectorField::at(const StrandSet& strands, std::size_t s) const {
    if (!hasExpression_) return constant_;
    const bool same = boundStrands_ && boundStrands_->strandCount() == strands.strandCount();
    return expression_.evaluateVector(strands, s, scalp_, same ? &bound_ : nullptr);
}

Vec3f VectorField::atPoint(const StrandSet& strands, std::size_t s, std::size_t point) const {
    if (!hasExpression_) return constant_;
    const bool same = boundStrands_ && boundStrands_->strandCount() == strands.strandCount();
    const Expression::Bound* bound = same ? &bound_ : nullptr;
    const bool varies = bound ? bound->perPoint : expression_.readsPointVariables();
    return varies ? expression_.evaluatePointVector(strands, s, point, scalp_, bound)
                  : expression_.evaluateVector(strands, s, scalp_, bound);
}

Vec3f VectorField::atRoot(const StrandRoot& root) const {
    return hasExpression_ ? expression_.evaluateVectorAt(root, scalp_) : constant_;
}

// ---------------------------------------------------------------------------
// What expressions read beyond the strands: the time, the groom's maps

namespace {

// A map as map("name") reads it: one ScalarField per component (r, g, b, a, luminance), raw 0-1.
class FieldMap : public ExpressionMap {
public:
    FieldMap(MapRef map, MeshPtr scalp, std::shared_ptr<const GuideSet> guides)
        : scalp_(std::move(scalp)), guides_(std::move(guides)) {
        for (const auto component : {MapRef::Component::R, MapRef::Component::G, MapRef::Component::B,
                                     MapRef::Component::A, MapRef::Component::Luminance}) {
            map.component = component;
            ParamSet values;
            values.set("map", map);
            fields_.emplace_back(values, "map", scalp_.get(), guides_.get());
        }
    }
    float at(const StrandRoot& root, int component) const override {
        return fields_[static_cast<std::size_t>(std::clamp(component, 0, 4))].atRoot(root);
    }

private:
    MeshPtr scalp_;
    std::shared_ptr<const GuideSet> guides_;
    std::vector<ScalarField> fields_;
};

bool carriesLayer(const GuideSet& set, const std::string& name) {
    for (const Guide& g : set.guides()) {
        if (g.attributes.count(name) || g.pointAttributes.count(name)) return true;
    }
    return false;
}

// What reading map `name` adds to a key, as the Map node's own key has it: the catalog and the
// scalp, then the texture's stamp, the vertex map's color set or the guides' layers.
std::uint64_t mapSalt(const EvalContext& ctx, const std::string& name) {
    std::uint64_t salt = hashString(hash64(0x3A95), name);
    for (const char* external : {"maps", "scalp"}) {
        const ExternalInputs::Entry* e = ctx.externals ? ctx.externals->find(external) : nullptr;
        salt = hashCombine(salt, e ? e->hash : 0);
    }
    const MapCatalogPtr catalog = valueAs<MapCatalogPtr>(ctx.external("maps"));
    const MapInfo* info = nullptr;
    if (catalog) {
        const auto it = catalog->maps.find(name);
        if (it != catalog->maps.end()) info = &it->second;
    }
    const std::string kind = info ? info->kind : "";
    if (kind == "texture") return hashCombine(salt, info->texture.empty() ? 0 : fileStamp(resolvePath(info->texture)));
    if (kind.empty() || kind == "vertices") {
        salt = hashCombine(salt, colorSetSalt(ctx, name));
        const ScalpMesh* scalp = scalpOf(ctx);
        if (kind == "vertices" || (scalp && scalp->hasColorSet(name))) return salt;
    }
    for (const char* external : {"guides", "hierarchy"}) {
        const ExternalInputs::Entry* e = ctx.externals ? ctx.externals->find(external) : nullptr;
        salt = hashCombine(hashString(salt, external), e ? e->hash : 0);
    }
    return salt;
}

}  // namespace

ExpressionEnvironment expressionEnvironment(const EvalContext& ctx) {
    ExpressionEnvironment environment;
    environment.seconds = ctx.time;
    environment.frame = secondsToFrame(ctx.time, ctx.framesPerSecond);
    struct Found {
        std::mutex mutex;
        std::map<std::string, ExpressionMapPtr> maps;
    };
    auto found = std::make_shared<Found>();
    const MeshPtr scalp = valueAs<MeshPtr>(ctx.external("scalp"));
    const MapCatalogPtr catalog = valueAs<MapCatalogPtr>(ctx.external("maps"));
    const GuidesPtr guides = valueAs<GuidesPtr>(ctx.external("guides"));
    const HierarchyPtr hierarchy = valueAs<HierarchyPtr>(ctx.external("hierarchy"));
    environment.map = [found, scalp, catalog, guides, hierarchy](const std::string& name) -> ExpressionMapPtr {
        std::lock_guard<std::mutex> lock(found->mutex);
        if (const auto it = found->maps.find(name); it != found->maps.end()) return it->second;
        // As the Map node finds it: the catalog's kind, else a color set of the scalp, else a
        // layer of the guides (the flat ones, else the hierarchy's).
        const MapInfo* info = nullptr;
        if (catalog) {
            const auto it = catalog->maps.find(name);
            if (it != catalog->maps.end()) info = &it->second;
        }
        std::shared_ptr<const GuideSet> layered = guides;
        if ((!layered || !carriesLayer(*layered, name)) && hierarchy) {
            auto flat = std::make_shared<const GuideSet>(hierarchy->flatten());
            if (carriesLayer(*flat, name) || !layered) layered = flat;
        }
        const bool hasLayer = layered && carriesLayer(*layered, name);
        std::string kind = info ? info->kind : "";
        if (kind.empty()) kind = scalp && scalp->hasColorSet(name) ? "vertices" : hasLayer ? "guides" : "";
        MapRef map;
        if (kind == "texture") {
            map.source = MapRef::Source::Texture;
            map.name = info ? info->texture : std::string();
            if (map.name.empty()) throw ExpressionError("map(\"" + name + "\"): the texture map has no image file yet");
        } else if (kind == "vertices") {
            map.source = MapRef::Source::ColorSet;
            map.name = name;
            if (!scalp || !scalp->hasColorSet(name)) {
                throw ExpressionError("map(\"" + name + "\"): the vertex map does not fit this scalp");
            }
        } else if (kind == "guides" && hasLayer) {
            map.source = MapRef::Source::Guide;
            map.name = name;
        } else {
            found->maps[name] = nullptr;
            return nullptr;
        }
        ExpressionMapPtr made;
        try {
            made = std::make_shared<const FieldMap>(map, scalp, kind == "guides" ? layered : nullptr);
        } catch (const GraphError& e) {
            throw ExpressionError("map(\"" + name + "\"): " + e.what());
        }
        found->maps[name] = made;
        return made;
    };
    return environment;
}

std::uint64_t expressionSalt(const std::string& text, const EvalContext& ctx) {
    const ExpressionDependencies deps = scanExpression(text);
    if (!deps.time && deps.maps.empty()) return 0;
    std::uint64_t salt = hash64(0xE59A);
    if (deps.time) salt = hashDouble(hashDouble(salt, ctx.time), ctx.framesPerSecond);
    for (const std::string& name : deps.maps) salt = hashCombine(salt, mapSalt(ctx, name));
    return salt;
}

}  // namespace groomlab
