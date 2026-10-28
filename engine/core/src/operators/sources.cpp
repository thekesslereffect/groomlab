// SPDX-License-Identifier: Apache-2.0
// Source operators: bring host-supplied data (ExternalInputs) into the graph.

#include <algorithm>
#include <functional>
#include <mutex>
#include <set>
#include <unordered_map>

#include "groomlab/graph.h"
#include "groomlab/operators.h"
#include "groomlab/scalp_pieces.h"
#include "groomlab/subdivision.h"
#include "internal.h"

namespace groomlab {

namespace {

// The `subdivision` param of Scalp and ColliderMesh: Catmull-Clark levels (subdivision.h).
ParamSpec subdivisionSpec(const std::string& what) {
    return notAnimatable({"subdivision", std::string("matchRender"),
                          "Catmull-Clark levels the " + what +
                              " is refined by, so hair sits on the surface the render draws: matchRender takes the "
                              "mesh's render subdivision (Maya: its Arnold subdivision, else its smooth mesh preview; "
                              "a groom file: the level saved with it), off the mesh as it is",
                          "", {"matchRender", "off", "1", "2", "3"}});
}

// What a node's note says of a mesh's subdivision ("" for a cage the node leaves as it is).
std::string subdivisionNote(const std::string& param, const ScalpMesh* refined, const ScalpMesh* cage) {
    const int levels = refined ? refined->subdivisionLevel() : 0;
    std::string from;
    if (param == "matchRender" && cage) {
        from = cage->renderSubdivisionFrom().empty() ? std::string("match render")
                                                     : "match render: " + cage->renderSubdivisionFrom();
    }
    if (levels == 0) return from.empty() ? std::string() : "Subdivision off (" + from + ")";
    std::string note = "Catmull-Clark x" + std::to_string(levels) + ": " + std::to_string(refined->triangleCount()) +
                       " triangles from " + std::to_string(refined->cageFaceCount()) + " faces";
    return from.empty() ? note : note + " (" + from + ")";
}

// The scalp: the session refines it (GroomSession::scalpSubdivision reads this node's
// `subdivision`) before it binds guides, parts and flow to it, so the external is the refined mesh.
// A groom with several scalps (scalp_pieces.h): `input` picks one ("scalp" the groom's own, else
// another's name), handed on as its own mesh (the external scalpPieceExternal(input)); with one
// scalp, the "scalp" external as it is. Another name the session does not know reads the
// external of that name (a host's own mesh input).
class ScalpOp : public Operator {
public:
    std::string typeName() const override { return op::kScalp; }
    std::string category() const override { return "source"; }
    std::vector<PortSpec> inputs() const override { return {}; }
    PortSpec output() const override { return {"scalp", PortType::Mesh}; }
    std::vector<ParamSpec> paramSpecs() const override {
        return {ParamSpec{"input", std::string(kMainScalpName),
                          "Which of the groom's scalps: scalp (its own mesh) or the name of another scalp of the groom "
                          "(Maya: Attribute Editor > Scalps), so a Scatter fed this node grows hair on that mesh alone"},
                subdivisionSpec("scalp")};
    }
    std::vector<std::string> externalInputs() const override {
        const std::string name = params().getString("input");
        return {name, scalpPieceExternal(name)};
    }
    bool refreshesOnCacheHit() const override { return true; }  // the scalp's key leaves its color sets out

    Value evaluate(const EvalContext& ctx, const std::vector<Value>&) const override { return mesh(ctx); }
    std::string note(const EvalContext& ctx) const override {
        const MeshPtr scalp = mesh(ctx);
        const std::string param = params().getString("subdivision");
        if (!scalp) {
            const std::string name = params().getString("input");
            return name == kMainScalpName ? std::string() : "No scalp named '" + name + "' on this groom: nothing to grow on";
        }
        if (scalp->subdivisionLevel() > 0 || param != "matchRender") return subdivisionNote(param, scalp.get(), scalp.get());
        return {};
    }

private:
    MeshPtr mesh(const EvalContext& ctx) const {
        const std::string name = params().getString("input");
        if (MeshPtr piece = valueAs<MeshPtr>(ctx.external(scalpPieceExternal(name)))) return piece;
        return valueAs<MeshPtr>(ctx.external(name));
    }
};

// A collider (or any other mesh) from the host: the external "mesh:<name>"
// (GroomData::meshes, Groom::setMesh; in Maya the groom's colliderMeshes, see
// groomlab.add_collider), for Collide's `collider` input. Null while the host
// has no mesh of that name, which Collide takes as nothing to hit.
class ColliderMeshOp : public Operator {
public:
    std::string typeName() const override { return op::kColliderMesh; }
    std::string category() const override { return "source"; }
    std::vector<PortSpec> inputs() const override { return {}; }
    PortSpec output() const override { return {"mesh", PortType::Mesh}; }
    std::vector<ParamSpec> paramSpecs() const override {
        return {{"name", std::string("collider"), "The collider mesh's name, as registered with the groom (Maya: "
                                                   "Attribute Editor > Colliders, or groomlab.add_collider)"},
                subdivisionSpec("collider"),
                // Read by the session for a Simulate node this feeds (SimInputs::colliderFriction).
                notAnimatable({"friction", -1.0, "How much simulated hair resting on this mesh is kept from sliding, 0-1, "
                                                 "in a Simulate node it feeds (-1: the Simulate node's friction)"})
                    .range(-1, 1)};
    }
    std::vector<std::string> externalInputs() const override { return {externalName()}; }

    // Refined here (the plan kept per topology: a deforming collider re-runs only its weights).
    Value evaluate(const EvalContext& ctx, const std::vector<Value>&) const override {
        const MeshPtr mesh = valueAs<MeshPtr>(ctx.external(externalName()));
        if (!mesh) return mesh;
        return subdivideScalp(mesh, subdivisionLevels(params().getString("subdivision"), mesh->renderSubdivision()));
    }
    std::string note(const EvalContext& ctx) const override {
        const MeshPtr mesh = valueAs<MeshPtr>(ctx.external(externalName()));
        if (!mesh) return {};
        const std::string param = params().getString("subdivision");
        const int levels = subdivisionLevels(param, mesh->renderSubdivision());
        if (levels == 0) return subdivisionNote(param, nullptr, mesh.get());
        // The counts the refined mesh has, from the kept plan (nothing refined again).
        const auto plan = SubdivisionPlan::of(*mesh, levels);
        std::string note = "Catmull-Clark x" + std::to_string(levels) + ": " + std::to_string(plan->triangles().size()) +
                           " triangles from " + std::to_string(plan->cageFaceCount()) + " faces";
        if (param == "matchRender") {
            note += mesh->renderSubdivisionFrom().empty() ? " (match render)" : " (match render: " + mesh->renderSubdivisionFrom() + ")";
        }
        return note;
    }

private:
    std::string externalName() const { return "mesh:" + params().getString("name"); }
};

// Guides from the host: the guide hierarchy ("hierarchy" external, flattened
// to one level or group) plus any flat guides ("guides" external, e.g.
// connected Maya curves). Both are optional; without either the output is null.
// Names in a list separated by spaces or commas.
std::vector<std::string> splitNames(const std::string& names) {
    std::vector<std::string> out;
    std::string name;
    for (const char c : names + " ") {
        if (c == ' ' || c == ',') {
            if (!name.empty()) out.push_back(name);
            name.clear();
        } else {
            name += c;
        }
    }
    return out;
}

// Removes from `set` the guides in the named groups or their child groups.
void excludeGroups(const GuideHierarchy& hierarchy, const std::string& names, GuideSet& set) {
    std::vector<GroupId> excluded;
    std::string name;
    for (const char c : names + " ") {
        if (c == ' ' || c == ',') {
            if (!name.empty()) {
                const GroupId id = hierarchy.findGroup(name);
                if (id == kNoGroup) throw GraphError("Guides: no guide group named '" + name + "' to exclude");
                excluded.push_back(id);
            }
            name.clear();
        } else {
            name += c;
        }
    }
    if (excluded.empty()) return;
    auto isExcluded = [&](GroupId group) {
        for (int depth = 0; group != kNoGroup && depth < 64; ++depth) {
            if (std::find(excluded.begin(), excluded.end(), group) != excluded.end()) return true;
            const GuideGroup* g = hierarchy.group(group);
            group = g ? g->parent : kNoGroup;
        }
        return false;
    };
    auto& guides = set.guides();
    guides.erase(std::remove_if(guides.begin(), guides.end(),
                                [&](const Guide& g) {
                                    const HierarchyGuide* h = hierarchy.guide(g.id);
                                    return h && isExcluded(h->group);
                                }),
                 guides.end());
}

// The guides of the other levels of the picked groups' family (GuideSet::levelTubes): those of the
// groups above any picked group and below any (a group below an excluded one is excluded too),
// in the hierarchy's order, each handed to `fn` with its group's depth. Nothing for a hierarchy
// of one level.
template <class IsExcluded, class Fn>
void forEachFamilyGuide(const GuideHierarchy& hierarchy, const std::set<GroupId>& picked, const IsExcluded& isExcluded,
                        const Fn& fn) {
    if (picked.empty() || hierarchy.maxDepth() == 0) return;
    std::set<GroupId> above;  // every picked group's ancestors
    for (const GroupId id : picked) {
        const GuideGroup* g = hierarchy.group(id);
        for (int depth = 0; g && g->parent != kNoGroup && depth < 64; ++depth) {
            above.insert(g->parent);
            g = hierarchy.group(g->parent);
        }
    }
    std::unordered_map<GroupId, int> family;  // group -> depth
    for (const GuideGroup& grp : hierarchy.groups()) {
        if (picked.count(grp.id) || isExcluded(grp.id)) continue;
        bool in = above.count(grp.id) > 0;
        for (GroupId up = grp.parent; !in && up != kNoGroup;) {  // below a picked group
            in = picked.count(up) > 0;
            const GuideGroup* g = hierarchy.group(up);
            up = g ? g->parent : kNoGroup;
        }
        if (in) family[grp.id] = hierarchy.groupDepth(grp.id);
    }
    if (family.empty()) return;
    for (const HierarchyGuide& g : hierarchy.guides()) {
        const auto it = family.find(g.group);
        if (it != family.end() && g.points.size() >= 2) fn(g, it->second);
    }
}

class GuidesOp : public Operator {
public:
    std::string typeName() const override { return op::kGuides; }
    std::string category() const override { return "source"; }
    std::vector<PortSpec> inputs() const override { return {}; }
    PortSpec output() const override { return {"guides", PortType::Guides}; }
    std::vector<ParamSpec> paramSpecs() const override {
        return {
            ParamSpec{"input", std::string("guides"), "Name of the external flat guide input"}.adv(),
            ParamSpec{"hierarchy", std::string("hierarchy"), "Name of the external guide hierarchy input"}.adv(),
            notAnimatable({"level", std::int64_t{-1}, "Hierarchy level to use without groups (-1: each family's deepest groups: a group's "
                                                  "children where it has some, else its own guides)"})
                .soft(-1, 5)
                .when("group", {""}),
            {"group", std::string(), "Hierarchy groups to use instead of a level, space or comma separated, with "
                                     "their guides together (empty: use level)"},
            {"exclude", std::string(), "Groups left out, with their child groups (space or comma separated), "
                                       "e.g. braid paths that are not hair"},
        };
    }
    std::vector<std::string> externalInputs() const override {
        return {params().getString("input"), params().getString("hierarchy")};
    }

    // The hierarchy keyed by the guides this node takes from it (its groups', or its level's, as
    // they come out: points, roots, IDs, attributes), not by all of it: a stroke on one group
    // leaves the Guides node of another, and its whole branch, cached.
    std::uint64_t externalKey(const std::string& name, const ExternalInputs::Entry& entry) const override {
        if (name != params().getString("hierarchy") || name == params().getString("input")) return entry.hash;
        const HierarchyPtr hierarchy = valueAs<HierarchyPtr>(entry.value);
        if (!hierarchy || hierarchy->guides().empty()) return entry.hash;
        const std::uint64_t memoKey = hashCombine(entry.hash, params().hash());
        {
            const std::lock_guard<std::mutex> lock(memo_->mutex);
            if (memo_->keyKnown && memo_->keyOf == memoKey) return memo_->keyValue;
        }
        const std::uint64_t key = pickedKey(*hierarchy, entry.hash);
        const std::lock_guard<std::mutex> lock(memo_->mutex);
        memo_->keyKnown = true;
        memo_->keyOf = memoKey;
        memo_->keyValue = key;
        return key;
    }

    Value evaluate(const EvalContext& ctx, const std::vector<Value>&) const override {
        const GuidesPtr flat = valueAs<GuidesPtr>(ctx.external(params().getString("input")));
        const HierarchyPtr hierarchy = valueAs<HierarchyPtr>(ctx.external(params().getString("hierarchy")));
        if (!hierarchy || hierarchy->guides().empty()) {
            // No guides yet, but the groups named here made (a hand-placed group, empty until its
            // guides are drawn): no guides rather than nothing, so what it feeds grows nothing
            // instead of saying its input is missing.
            const std::vector<std::string> groupNames = splitNames(params().getString("group"));
            const bool made = hierarchy && !groupNames.empty() &&
                              std::all_of(groupNames.begin(), groupNames.end(),
                                          [&](const std::string& g) { return hierarchy->findGroup(g) != kNoGroup; });
            if (!flat && made) return GuidesPtr(std::make_shared<const GuideSet>());
            return flat;
        }
        const std::shared_ptr<const GuideSet> fromHierarchy = picked(*hierarchy);
        if (!flat) return GuidesPtr(fromHierarchy);
        GuideSet set = *fromHierarchy;
        for (const Guide& g : flat->guides()) {
            Guide& added = set.add(g.id, g.points);
            added.root = g.root;
            added.attributes = g.attributes;
            added.pointAttributes = g.pointAttributes;
        }
        return GuidesPtr(std::make_shared<GuideSet>(std::move(set)));
    }

private:
    // The guides taken from `hierarchy` as the params pick them, made once per hierarchy content
    // and params (the key and the evaluation share them). Throws GraphError for a missing group.
    std::shared_ptr<const GuideSet> picked(const GuideHierarchy& hierarchy) const {
        const std::uint64_t key = hashCombine(hierarchy.contentHash(), params().hash());
        {
            const std::lock_guard<std::mutex> lock(memo_->mutex);
            if (memo_->set && memo_->key == key) return memo_->set;
        }
        auto made = std::make_shared<const GuideSet>(pick(hierarchy));
        const std::lock_guard<std::mutex> lock(memo_->mutex);
        memo_->key = key;
        memo_->set = made;
        return made;
    }

    GuideSet pick(const GuideHierarchy& hierarchy) const {
        const std::vector<std::string> groupNames = splitNames(params().getString("group"));
        GuideSet set;
        if (!groupNames.empty()) {
            std::set<std::uint64_t> seen;  // a group listed twice counts once
            for (const std::string& groupName : groupNames) {
                const GroupId id = hierarchy.findGroup(groupName);
                if (id == kNoGroup) throw GraphError("Guides: no guide group named '" + groupName + "'");
                const GuideSet one = hierarchy.flattenGroup(id);  // kept alive for the loop
                for (const Guide& g : one.guides()) {
                    if (seen.insert(g.id).second) set.guides().push_back(g);
                }
            }
        } else {
            set = hierarchy.flatten(static_cast<int>(params().getInt("level")));
        }
        excludeGroups(hierarchy, params().getString("exclude"), set);
        // The tubes of the family's other levels, for Interpolate's fillTubes (one level: none).
        auto tubes = std::make_shared<std::vector<Guide>>();
        const float scale = hierarchy.scale();
        forEachFamilyGuide(hierarchy, pickedGroups(hierarchy), excludedTest(hierarchy), [&](const HierarchyGuide& g, int depth) {
            Guide out;
            out.id = g.id;
            out.root = g.root;
            out.points = g.points;
            out.hasTube = true;
            out.tube = g.tube;
            out.tube.scaleWidth(scale);
            out.frameUp = g.frameUp;
            out.depth = depth;
            tubes->push_back(std::move(out));
        });
        if (!tubes->empty()) set.setLevelTubes(std::move(tubes));
        return set;
    }

    // The groups pick() takes guides from: the params' groups, else those at their level (missing
    // ones left out: pick() says so).
    std::set<GroupId> pickedGroups(const GuideHierarchy& hierarchy) const {
        std::set<GroupId> out;
        const std::vector<std::string> groupNames = splitNames(params().getString("group"));
        if (!groupNames.empty()) {
            for (const std::string& name : groupNames) {
                const GroupId id = hierarchy.findGroup(name);
                if (id != kNoGroup) out.insert(id);
            }
            return out;
        }
        const int level = static_cast<int>(params().getInt("level"));
        if (level < 0) return hierarchy.deepestGroups();
        for (const GuideGroup& grp : hierarchy.groups()) {
            if (hierarchy.groupDepth(grp.id) == level) out.insert(grp.id);
        }
        return out;
    }

    // Whether a group is left out by `exclude`, or lies under one that is (missing names skipped).
    std::function<bool(GroupId)> excludedTest(const GuideHierarchy& hierarchy) const {
        std::vector<GroupId> excluded;
        for (const std::string& name : splitNames(params().getString("exclude"))) {
            const GroupId id = hierarchy.findGroup(name);
            if (id != kNoGroup) excluded.push_back(id);
        }
        return [&hierarchy, excluded](GroupId group) {
            for (int depth = 0; group != kNoGroup && depth < 64 && !excluded.empty(); ++depth) {
                if (std::find(excluded.begin(), excluded.end(), group) != excluded.end()) return true;
                const GuideGroup* g = hierarchy.group(group);
                group = g ? g->parent : kNoGroup;
            }
            return false;
        };
    }

    // What pick() makes from `hierarchy`, hashed without making it (a brush dab asks on every
    // dab): each guide it takes as it comes out (ID, root, points, tube, its own attributes and layers
    // and those of the guides it hangs from, which its effective ones are made of), in order.
    // `whole` (the hierarchy's hash) where the params name a group it lacks: evaluate says so.
    std::uint64_t pickedKey(const GuideHierarchy& hierarchy, std::uint64_t whole) const {
        std::vector<GroupId> excluded;
        for (const std::string& name : splitNames(params().getString("exclude"))) {
            const GroupId id = hierarchy.findGroup(name);
            if (id == kNoGroup) return whole;
            excluded.push_back(id);
        }
        auto isExcluded = [&](GroupId group) {
            for (int depth = 0; group != kNoGroup && depth < 64 && !excluded.empty(); ++depth) {
                if (std::find(excluded.begin(), excluded.end(), group) != excluded.end()) return true;
                const GuideGroup* g = hierarchy.group(group);
                group = g ? g->parent : kNoGroup;
            }
            return false;
        };
        auto hashLayers = [](std::uint64_t h, const HierarchyGuide& g) {
            h = hashCombine(h, g.attributes.size());
            for (const auto& [k, v] : g.attributes) h = hashFloat(hashString(h, k), v);
            h = hashCombine(h, g.pointAttributes.size());
            for (const auto& [k, values] : g.pointAttributes) {
                h = hashCombine(hashString(h, k), values.size());
                for (const float v : values) h = hashFloat(h, v);
            }
            return h;
        };
        std::uint64_t key = hash64(0x6C1DE5u);
        auto take = [&](const HierarchyGuide& g) {
            key = hashCombine(hashCombine(key, g.id), static_cast<std::uint64_t>(static_cast<std::int64_t>(g.root.face)));
            key = hashFloat(hashFloat(key, g.root.u), g.root.v);
            key = hashCombine(key, g.points.size());
            for (const Vec3f& p : g.points) key = hashFloat(hashFloat(hashFloat(key, p.x), p.y), p.z);
            // Its tube (at the size shown) and frame reference: what Interpolate's fillTubes reads.
            key = hashFloat(hashTube(key, g.tube), hierarchy.scale());
            key = hashFloat(hashFloat(hashFloat(key, g.frameUp.x), g.frameUp.y), g.frameUp.z);
            key = hashLayers(key, g);
            const HierarchyGuide* up = hierarchy.guide(g.parent);
            for (int depth = 0; up && depth < 64; ++depth, up = hierarchy.guide(up->parent)) {
                key = hashLayers(hashCombine(key, up->id), *up);
            }
        };
        const std::vector<std::string> groupNames = splitNames(params().getString("group"));
        if (!groupNames.empty()) {
            std::set<std::uint64_t> seen;  // a group listed twice counts once
            for (const std::string& name : groupNames) {
                const GroupId id = hierarchy.findGroup(name);
                if (id == kNoGroup) return whole;
                for (const HierarchyGuide& g : hierarchy.guides()) {
                    if (g.group == id && g.points.size() >= 2 && !isExcluded(g.group) && seen.insert(g.id).second) take(g);
                }
            }
        } else {
            const std::set<GroupId> groups = pickedGroups(hierarchy);
            for (const HierarchyGuide& g : hierarchy.guides()) {
                if (groups.count(g.group) && g.points.size() >= 2 && !isExcluded(g.group)) take(g);
            }
        }
        // The family's other levels: their tubes as pick() hands them on (GuideSet::levelTubes).
        forEachFamilyGuide(hierarchy, pickedGroups(hierarchy), isExcluded, [&](const HierarchyGuide& g, int depth) {
            key = hashCombine(hashCombine(key, g.id + 0x7B5u), static_cast<std::uint64_t>(depth));
            key = hashCombine(key, static_cast<std::uint64_t>(static_cast<std::int64_t>(g.root.face)));
            key = hashFloat(hashFloat(key, g.root.u), g.root.v);
            key = hashCombine(key, g.points.size());
            for (const Vec3f& p : g.points) key = hashFloat(hashFloat(hashFloat(key, p.x), p.y), p.z);
            key = hashFloat(hashTube(key, g.tube), hierarchy.scale());
            key = hashFloat(hashFloat(hashFloat(key, g.frameUp.x), g.frameUp.y), g.frameUp.z);
        });
        return key;
    }

    struct Memo {
        std::mutex mutex;
        std::uint64_t key = 0;  // picked's
        std::shared_ptr<const GuideSet> set;
        bool keyKnown = false;  // externalKey's, for the hierarchy and params it was worked out for
        std::uint64_t keyOf = 0, keyValue = 0;
    };
    std::shared_ptr<Memo> memo_ = std::make_shared<Memo>();  // (shared by copies: the same params)
};

}  // namespace

void registerScalpOperator(OperatorRegistry& registry) {
    registry.registerType(op::kScalp,
                          {"source", "The mesh the hair grows on.", "mesh skin body surface"},
                          [] { return std::make_unique<ScalpOp>(); });
}

void registerColliderMeshOperator(OperatorRegistry& registry) {
    registry.registerType(op::kColliderMesh,
                          {"source",
                           "Another mesh (a body, a collar) for Collide to keep strands out of.",
                           "body collar obstacle mesh"},
                          [] { return std::make_unique<ColliderMeshOp>(); });
}

void registerGuidesOperator(OperatorRegistry& registry) {
    registry.registerType(op::kGuides,
                          {"source",
                           "The groom's guides, by group: what Interpolate, Clump and friends follow.",
                           "guide curves groups"},
                          [] { return std::make_unique<GuidesOp>(); });
}

}  // namespace groomlab
