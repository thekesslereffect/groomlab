// SPDX-License-Identifier: Apache-2.0
// GroomDocument: what the GroomLab menu's Add, Simulate, Import and Utilities helpers do to the
// groom (groom_document.h), as Maya's (hosts/maya/scripts/groomlab: motion.py, sim.py,
// collision.py, feathers and braids) and Blender's pipeline.py: the graph edited through the core's
// GraphEdit (graph_edit.h: names, positions, fitted defaults, checked connections), the files
// through the core's writers.

#include "groom_document.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <set>
#include <fstream>
#include <map>
#include <stdexcept>

#include "groomlab/graph_inserts.h"
#include "groomlab/alembic_io.h"
#include "groomlab/asset_files.h"
#include "groomlab/card_files.h"
#include "groomlab/graph_edit.h"
#include "groomlab/paths.h"
#include "groomlab/presets.h"
#include "groomlab/follicle_map.h"
#include "groomlab/scale_maps.h"
#include "groomlab/scales.h"

namespace groomlab::app {

namespace fs = std::filesystem;
using nlohmann::json;

namespace {

struct Feed {
    std::string from, output;  // "" nothing feeds it
};

// What feeds `to`'s `input`.
Feed feedOf(const json& g, const std::string& to, const std::string& input) {
    for (const json& c : g.value("connections", json::array())) {
        if (c.value("to", std::string()) == to && c.value("input", std::string()) == input) {
            return {c.value("from", std::string()), c.value("output", std::string())};
        }
    }
    return {};
}

// Every (node, input) `from` feeds.
std::vector<std::pair<std::string, std::string>> fedBy(const json& g, const std::string& from) {
    std::vector<std::pair<std::string, std::string>> out;
    for (const json& c : g.value("connections", json::array())) {
        if (c.value("from", std::string()) == from) out.emplace_back(c.value("to", std::string()), c.value("input", std::string()));
    }
    return out;
}

const json* nodeIn(const json& g, const std::string& name) {
    const auto nodes = g.find("nodes");  // (a pointer into g itself, not into a copy)
    if (nodes == g.end() || !nodes->is_array()) return nullptr;
    for (const json& n : *nodes) {
        if (n.is_object() && n.value("name", std::string()) == name) return &n;
    }
    return nullptr;
}

std::string typeOf(const json& g, const std::string& name) {
    const json* n = nodeIn(g, name);
    return n ? n->value("type", std::string()) : std::string();
}

json paramOf(const json& g, const std::string& node, const std::string& param) {
    const json* n = nodeIn(g, node);
    if (!n || !n->contains("params") || !(*n)["params"].is_object()) return nullptr;
    const auto it = (*n)["params"].find(param);
    return it == (*n)["params"].end() ? json() : *it;
}

std::vector<std::string> ofType(const json& g, const std::string& type) {
    std::vector<std::string> out;
    for (const json& n : g.value("nodes", json::array())) {
        if (n.value("type", std::string()) == type) out.push_back(n.value("name", std::string()));
    }
    return out;
}

// The graph edited as GraphEdit sees it, its JSON read back for the lookups between the edits.
struct GraphEdits {
    GraphEdit e;
    std::optional<double> size;
    explicit GraphEdits(const json& g, std::optional<double> scalpSize) : e(g.dump()), size(scalpSize) {}
    json now() const { return json::parse(e.toJson()); }
    std::string add(const std::string& type, const std::string& base, const json& params,
                    std::optional<std::pair<double, double>> at = std::nullopt) {
        const std::string name = e.uniqueName(base.empty() ? type : base);
        return e.addNode(type, name, params.is_null() ? std::string() : params.dump(), at ? std::optional<double>(at->first) : std::nullopt,
                         at ? std::optional<double>(at->second) : std::nullopt, size);
    }
    // `node` on what feeds `anchor`'s strands, drawn a row under the anchor: every host's (graph_inserts.h).
    void insertBefore(const std::string& anchor, const std::string& node) { groomlab::insertBefore(e, anchor, node); }
};

// Where a motion node goes (Maya's motion_anchor): just before the output, but before the Collide
// and Cards nodes ending its chain, so the strands collide where they moved to and the cards take
// the motion.
std::string motionAnchor(const json& g, const std::string& output) {
    std::string anchor = output;
    std::set<std::string> seen;
    while (seen.insert(anchor).second) {
        const Feed feed = feedOf(g, anchor, "strands");
        const std::string type = feed.from.empty() ? std::string() : typeOf(g, feed.from);
        if (type != "Collide" && type != "Cards") break;
        anchor = feed.from;
    }
    return anchor;
}

// ---- Files ----------------------------------------------------------------------------------------

std::string trimmed(const std::string& s) {
    const auto a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return {};
    return s.substr(a, s.find_last_not_of(" \t\r\n") - a + 1);
}

}  // namespace

// ---- Add and Simulate -------------------------------------------------------------------------------

std::string GroomDocument::addMotionNode(const std::string& type, const json& params, std::string* made) {
    std::string name;
    const MeshPtr& scalp = file().data.scalp;
    const std::string error = editGraph("add " + type, [&](json& g) {
        GraphEdits edit(g, scalp ? presetScalpSize(*scalp) : std::nullopt);
        const std::string output = edit.e.outputNode();
        if (output.empty()) throw std::runtime_error("the graph has no output");
        name = edit.add(type, type, params);
        edit.insertBefore(motionAnchor(edit.now(), output), name);
        g = edit.now();
    });
    if (error.empty() && made) *made = name;
    return error;
}

std::string GroomDocument::addSimulateNode(std::string* made) {
    std::string name;
    const MeshPtr& scalp = file().data.scalp;
    const std::string error = editGraph("add Simulate", [&](json& g) {
        // Every host's (graph_inserts.h): between the guides and the Interpolates they feed.
        GraphEdit edit(g.dump());
        name = insertSimulate(edit, {}, scalp ? presetScalpSize(*scalp) : std::nullopt);
        g = json::parse(edit.toJson());
    });
    if (error.empty() && made) *made = name;
    return error;
}

std::string GroomDocument::addGuideCacheNode(const std::string& path, std::string* made) {
    std::string name;
    const MeshPtr& scalp = file().data.scalp;
    const std::string error = editGraph("load guide cache", [&](json& g) {
        // Every host's (graph_inserts.h): right after the Guides node, or the GuideCache there.
        GraphEdit edit(g.dump());
        name = insertGuideCache(edit, path, {}, scalp ? presetScalpSize(*scalp) : std::nullopt);
        g = json::parse(edit.toJson());
    });
    if (error.empty() && made) *made = name;
    return error;
}

std::string GroomDocument::addCollider(const std::string& meshName, std::string* made) {
    if (meshName.empty() || meshName == "scalp") return "pick another collider name than '" + meshName + "'";
    for (const std::string& n : ofType(graph_, "ColliderMesh")) {
        const json own = paramOf(graph_, n, "name");
        if (own.is_string() && own.get<std::string>() == meshName) {
            if (made) *made = n;  // that mesh collides already
            return {};
        }
    }
    std::string name;
    const MeshPtr& scalp = file().data.scalp;
    const std::string error = editGraph("add collider " + meshName, [&](json& g) {
        // Every host's graph (graph_inserts.h): a ColliderMesh, a Collide before the output.
        GraphEdit edit(g.dump());
        name = insertCollider(edit, meshName, true, std::nullopt, std::nullopt, scalp ? presetScalpSize(*scalp) : std::nullopt);
        g = json::parse(edit.toJson());
    });
    if (error.empty() && made) *made = name;
    return error;
}

std::string GroomDocument::ensureGuideGroup(const std::string& name) {
    if (name.empty()) return "a guide group needs a name";
    const HierarchyPtr h = session().authoredHierarchy();
    if (h && h->findGroup(name) != kNoGroup) return {};
    return editGuides("add group " + name, [&](GuideHierarchy& hierarchy) { hierarchy.addGroup(name); });
}

std::vector<std::string> GroomDocument::featherGuideGroups() {
    // The groups of the Guides nodes feeding a feather design, every host's (graph_inserts.h).
    std::vector<std::string> groups = groomlab::featherGuideGroups(GraphEdit(graph_.dump()));
    const HierarchyPtr h = session().authoredHierarchy();
    if (std::find(groups.begin(), groups.end(), "feathers") == groups.end() && h && h->findGroup("feathers") != kNoGroup) {
        groups.push_back("feathers");
    }
    return groups;
}

std::string GroomDocument::addFeathers(const std::vector<std::pair<std::string, std::string>>& designs, const std::vector<double>& weights,
                                       const std::string& source, const std::string& group, std::string* made) {
    const MeshPtr& scalp = file().data.scalp;
    const std::optional<double> size = scalp ? presetScalpSize(*scalp) : std::nullopt;
    // Each design's preset fitted to this scalp.
    std::vector<FeatherDesign> fitted;
    for (const auto& [design, text] : designs) {
        try {
            fitted.push_back({design, presetGraphJson(text, size)});
        } catch (const std::exception& e) {
            return "cannot read the feather design " + design + ": " + e.what();
        }
    }
    // Every host's graph edit (graph_inserts.h insertFeathers), tried first: the guide group is not
    // made for feathers the graph cannot take.
    auto insert = [&](json& g) {
        GraphEdit edit(g.dump());
        const FeatherNodes nodes = insertFeathers(edit, fitted, weights, source, group, true, std::nullopt, size);
        g = json::parse(edit.toJson());
        return nodes.designs.front();
    };
    try {
        json trial = graph_;
        insert(trial);
    } catch (const std::exception& e) {
        return e.what();
    }
    if (source == "guides") {
        if (const std::string error = ensureGuideGroup(group); !error.empty()) return error;
    }
    std::string first;
    const std::string error = editGraph("add feathers", [&](json& g) { first = insert(g); });
    if (error.empty() && made) *made = first;
    return error;
}

std::vector<std::string> GroomDocument::braidStyles() const {
    std::vector<std::string> out;
    if (const nlohmann::json* braid = spec("Braid")) {
        for (const nlohmann::json& p : braid->value("params", nlohmann::json::array())) {
            if (p.value("name", std::string()) != "style") continue;
            for (const nlohmann::json& c : p.value("choices", nlohmann::json::array())) {
                if (c.is_string()) out.push_back(c.get<std::string>());
            }
        }
    }
    return out;
}

std::string GroomDocument::addBraid(const std::string& style, const std::string& group, std::string* made) {
    const MeshPtr& scalp = file().data.scalp;
    const std::optional<double> size = scalp ? presetScalpSize(*scalp) : std::nullopt;
    // Every host's graph edit (graph_inserts.h insertBraid: the style one of the Braid's, after the
    // Interpolate), tried first: the group is not made for a graph that cannot take a braid.
    auto insert = [&](json& g) {
        GraphEdit edit(g.dump());
        const std::string name = insertBraid(edit, group, json{{"style", style}}.dump(), {}, size);
        g = json::parse(edit.toJson());
        return name;
    };
    try {
        json trial = graph_;
        insert(trial);
    } catch (const std::exception& e) {
        return e.what();
    }
    if (const std::string error = ensureGuideGroup(group); !error.empty()) return error;
    std::string name;
    const std::string error = editGraph("add braid", [&](json& g) { name = insert(g); });
    if (error.empty() && made) *made = name;
    return error;
}

// ---- Import and Utilities ---------------------------------------------------------------------------

std::string GroomDocument::importAlembicCurves(const std::string& path, const std::string& object, int samples, std::size_t* curves) {
    if (samples < 0 || samples > 1024) return "samples is 0 (as read) to 1024";
    std::error_code ec;
    if (!fs::is_regular_file(fs::u8path(path), ec)) return "no file at " + path;
    std::size_t count = 0;
    try {
        count = readAlembicCurvesAt(path, time_, object).strandCount();
    } catch (const std::exception& e) {
        return std::string("cannot read the Alembic curves: ") + e.what();
    }
    if (count == 0) return object.empty() ? "the file has no curves" : "the file has no curves under " + object;
    const std::string error = editGraph("import Alembic curves", [&](json& g) {
        json nodes = json::array();
        bool hasScalp = false;
        json drawn;
        for (const json& n : g.value("nodes", json::array())) {
            const std::string name = n.value("name", std::string());
            if (name == "guides") {
                if (n.contains("ui")) drawn = n["ui"];  // where the Guides node was drawn
                continue;
            }
            hasScalp = hasScalp || name == "scalp";
            nodes.push_back(n);
        }
        json params = {{"path", path}, {"samples", samples}};
        if (!object.empty()) params["object"] = object;
        json made = {{"name", "guides"}, {"type", "ImportCurves"}, {"params", params}};
        if (!drawn.is_null()) made["ui"] = drawn;
        nodes.push_back(made);
        g["nodes"] = nodes;
        json connections = json::array();
        for (const json& c : g.value("connections", json::array())) {
            if (c.value("to", std::string()) != "guides") connections.push_back(c);
        }
        if (hasScalp) connections.push_back({{"from", "scalp"}, {"to", "guides"}, {"input", "scalp"}});
        g["connections"] = connections;
        GraphEdit check(g.dump());  // (the links checked: an Interpolate takes curves on its guides input)
        check.checkConnections();
    });
    if (error.empty() && curves) *curves = count;
    return error;
}

std::string GroomDocument::writeStrandMesh(const std::string& objPath, const StrandMeshOptions& options, std::size_t maxStrands,
                                           std::size_t* strandsWritten) {
    if (options.sides < 3) return "a tube has 3 sides or more";
    if (maxStrands < 1) return "at most 1 strand or more";
    try {
        MeshPtr scalp;
        const bool cards = options.make == StrandMeshOptions::Make::Auto || options.make == StrandMeshOptions::Make::Cards;
        const StrandsPtr all = exportFrame(time_, 1.0f, &scalp, cards, true);
        // Scales, feathers, cards and instances whole (their plates and rachis are the strand set's); tubes thinned.
        const bool whole = resolveMeshMake(*all, options) != StrandMeshOptions::Make::Strands;
        const std::vector<std::size_t> keep = thinStrandsById(*all, whole ? all->strandCount() : maxStrands);
        StrandSet kept;
        const StrandSet* strands = all.get();
        if (keep.size() < all->strandCount()) {
            for (const std::size_t i : keep) {
                const Vec3f* p = all->strandPoints(i);
                const std::size_t n = all->strandPointCount(i);
                std::vector<float> w;
                if (all->widths().size() == all->pointCount()) {
                    w.assign(all->widths().begin() + static_cast<std::ptrdiff_t>(all->strandBegin(i)),
                             all->widths().begin() + static_cast<std::ptrdiff_t>(all->strandBegin(i) + n));
                }
                kept.addStrand(all->ids()[i], std::vector<Vec3f>(p, p + n), w, i < all->roots().size() ? all->roots()[i] : StrandRoot{});
            }
            strands = &kept;
        }
        const GroomFile& groomFile = file();
        const PrototypeLookup prototypes = [&groomFile](const std::string& n) -> const InstancePrototype* {
            const auto it = groomFile.prototypes.find(n);
            return it != groomFile.prototypes.end() && it->second.found ? &it->second : nullptr;
        };
        const StrandMesh mesh = buildStrandMesh(*strands, scalp.get(), options, prototypes);
        if (mesh.mesh.faceCount() == 0) return "nothing to make a mesh of (no faces)";
        const bool second = options.strandUVs && mesh.u2.size() == mesh.mesh.u.size();
        const std::vector<float>& us = second ? mesh.u2 : mesh.mesh.u;
        const std::vector<float>& vs = second ? mesh.v2 : mesh.mesh.v;
        std::ofstream out(fs::u8path(objPath), std::ios::binary);
        if (!out) return "cannot write " + objPath;
        out << "# GroomLab: " << strands->strandCount() << " strands as " << (options.style == StrandMeshOptions::Style::Tube ? "tubes" : "ribbons")
            << "\n";
        out << "o " << fs::u8path(objPath).stem().u8string() << "\n";
        char line[96];
        for (const Vec3f& p : mesh.mesh.points) {
            std::snprintf(line, sizeof(line), "v %.6g %.6g %.6g\n", p.x, p.y, p.z);
            out << line;
        }
        const bool uvs = !us.empty() && mesh.mesh.faceUVs.size() == mesh.mesh.faceVertices.size();
        if (uvs) {
            for (std::size_t i = 0; i < us.size(); ++i) {
                std::snprintf(line, sizeof(line), "vt %.6g %.6g\n", us[i], i < vs.size() ? vs[i] : 0.0f);
                out << line;
            }
        }
        std::size_t k = 0;
        for (const int count : mesh.mesh.faceCounts) {
            out << "f";
            for (int c = 0; c < count; ++c, ++k) {
                out << ' ' << mesh.mesh.faceVertices[k] + 1;
                if (uvs) out << '/' << mesh.mesh.faceUVs[k] + 1;
            }
            out << "\n";
        }
        if (!out) return "cannot write " + objPath;
        if (strandsWritten) *strandsWritten = strands->strandCount();
    } catch (const std::exception& e) {
        return e.what();
    }
    return {};
}

std::string GroomDocument::bakeScaleMaps(const std::string& directory, const std::string& name, int resolution,
                                         std::vector<std::string>* files) {
    if (resolution < 16 || resolution > 8192) return "the resolution is 16 to 8192";
    try {
        MeshPtr scalp;
        const StrandsPtr strands = exportFrame(time_, 1.0f, &scalp, false, true);
        const GroomFile& groomFile = file();
        const PrototypeLookup prototypes = [&groomFile](const std::string& n) -> const InstancePrototype* {
            const auto it = groomFile.prototypes.find(n);
            return it != groomFile.prototypes.end() && it->second.found ? &it->second : nullptr;
        };
        ScaleMeshBuilder builder;
        const auto mesh = builder.build(*strands, scalp, prototypes);
        if (!mesh || mesh->triangles.empty()) return "the groom has no scales (a Scales node)";
        if (!scalp || !scalp->hasUVs()) return "the scalp has no uvs to bake the scales onto";
        std::error_code ec;
        fs::create_directories(fs::u8path(directory), ec);
        ScaleMapOptions options;
        options.resolution = resolution;
        const ScaleMaps maps = groomlab::bakeScaleMaps(*scalp, *mesh, options);
        const std::vector<std::string> written = writeScaleMaps(maps, (fs::u8path(directory) / fs::u8path(name + "_")).generic_u8string());
        if (files) *files = written;
    } catch (const std::exception& e) {
        return e.what();
    }
    return {};
}

std::string GroomDocument::bakeFollicleMap(const std::string& directory, const std::string& name, int resolution, float radius,
                                           std::vector<std::string>* files, float* used) {
    if (resolution < 16 || resolution > 8192) return "the resolution is 16 to 8192";
    try {
        MeshPtr scalp;
        const StrandsPtr strands = exportFrame(time_, 1.0f, &scalp, false, true);  // (every root, none buried)
        if (!scalp || !scalp->hasUVs()) return "the scalp has no uvs to bake the follicles onto";
        std::error_code ec;
        fs::create_directories(fs::u8path(directory), ec);
        FollicleMapOptions options;
        options.resolution = resolution;
        options.radius = radius;
        const FollicleMap map = groomlab::bakeFollicleMap(*scalp, *strands, options);
        const std::vector<std::string> written = writeFollicleMap(map, (fs::u8path(directory) / fs::u8path(name + "_")).generic_u8string());
        if (files) *files = written;
        if (used) *used = map.radius;
    } catch (const std::exception& e) {
        return e.what();
    }
    return {};
}

float GroomDocument::follicleAutoRadius() {
    try {
        MeshPtr scalp;
        const StrandsPtr strands = exportFrame(time_, 1.0f, &scalp, false, true);
        return scalp && strands ? follicleMapAutoRadius(*scalp, *strands) : 0.0f;
    } catch (const std::exception&) {
        return 0.0f;
    }
}

std::vector<GroomDocument::MissingFile> GroomDocument::missingFiles() const {
    std::vector<MissingFile> out;
    for (const PaintedMap& m : maps()) {
        if (m.texture.empty()) continue;
        if (!missingMapFile(m.name).empty()) out.push_back({"maps", m.name, m.texture});
    }
    // (Every host's rules: asset_files.h graphFileParams, fileThere.)
    for (const auto& [label, value] : graphFileParams(graph_.dump())) {
        const std::string stored = trimmed(value);
        if (!stored.empty() && !fileThere(resolveMapPath(stored))) out.push_back({"params", label, stored});
    }
    return out;
}

std::string GroomDocument::findMissingFiles(const std::string& folder, bool recursive, RelinkReport* report) {
    std::error_code ec;
    if (folder.empty() || !fs::is_directory(fs::u8path(folder), ec)) return "no folder " + folder;
    const FileIndex index = indexFolder(folder, recursive);  // (every host's: asset_files.h)
    RelinkReport r;
    const std::vector<MissingFile> missing = missingFiles();
    std::map<std::string, std::string> params;  // label -> file found
    for (const MissingFile& m : missing) {
        std::vector<std::string> candidates;
        const std::string found = matchMissingFile(index, m.stored, &candidates);
        if (found.empty()) {
            if (candidates.empty()) r.stillMissing.push_back(m);
            else r.ambiguous.emplace_back(m, candidates);
            continue;
        }
        if (m.kind == "maps") {
            const std::string error = relinkMap(m.key, found);
            if (!error.empty()) {
                r.stillMissing.push_back(m);
                continue;
            }
        } else {
            params[m.key] = found;
        }
        r.relinked.emplace_back(m, found);
    }
    if (!params.empty()) {
        const std::string error = editGraph("relink missing files", [&](json& g) { g = json::parse(withGraphFileParams(g.dump(), params)); });
        if (!error.empty()) return error;
    }
    if (report) *report = std::move(r);
    return {};
}

void GroomDocument::followFiles(const std::string& newPath) {
    const fs::path to = fs::u8path(newPath);
    const std::string newBase = fs::absolute(to).parent_path().generic_u8string();
    const std::string newName = to.stem().u8string();
    const std::string newRoot = groomlab::assetFolder(newBase, newName);
    std::string oldBase = newBase, oldRoot;
    std::vector<std::pair<std::string, std::string>> extra;
    if (!path_.empty()) {
        const fs::path from = fs::u8path(path_);
        oldBase = fs::absolute(from).parent_path().generic_u8string();
        const std::string stem = from.stem().u8string();
        oldRoot = groomlab::assetFolder(oldBase, stem);
        // The groom's maps folder, masks/<name>, is the new name's (FileFollower looks at these
        // before the old root).
        // (A named groom of a document of several keeps its folders' name; the file's own groom's
        // is the file's.)
        const std::string oldFolder = fileSafeName(name_.empty() ? stem : name_);
        const std::string newFolder = fileSafeName(name_.empty() ? newName : name_);
        const std::string newMaps = newRoot + "/masks/" + newFolder;
        extra.push_back({oldRoot + "/masks/" + oldFolder, newMaps});
        const std::string newCards = newRoot + "/cards/" + newFolder;
        extra.push_back({oldRoot + "/cards/" + oldFolder, newCards});
        extra.push_back({oldRoot + "/caches/" + oldFolder, newRoot + "/caches/" + newFolder});
        // Where earlier versions of the app kept them, beside the groom: into the new folders.
        extra.push_back({oldBase + "/masks/" + stem, newMaps});
        extra.push_back({oldBase + "/" + stem + "_cards", newCards});
        extra.push_back({oldBase + "/" + stem + "_sim", newRoot + "/caches"});
    } else if (!scene_->file().directory.empty()) {
        oldBase = scene_->file().directory;  // a groom made from another file: its paths resolve there
    }
    if (!untitledRoot_.empty()) extra.push_back({untitledRoot_, newRoot});
    FileFollower f(oldBase, oldRoot, newBase, newRoot, extra);

    // Texture maps (no undo step: the groom names the copies, as Maya's save does).
    ScalpMaps maps = session().scalpMaps();
    bool mapsMoved = false;
    for (auto& [name, entry] : maps.maps) {
        if (entry.kind != "texture" || entry.texture.empty()) continue;
        const std::string followed = f.follow(entry.texture);
        if (followed != entry.texture) {
            entry.texture = followed;
            mapsMoved = true;
        }
    }
    if (mapsMoved) {
        session().setScalpMaps(maps);
        paints_.clear();
    }
    // The files the graph reads (images, Alembic files, guide caches; inside compounds too).
    bool graphMoved = false;
    const std::string followed = followGraphFiles(graph_.dump(), [&](const std::string& stored) { return f.follow(trimmed(stored)); }, &graphMoved);
    if (graphMoved) graph_ = json::parse(followed);
    if (graphMoved) applyGraph();
    // The card bake: every file of its prefix together.
    if (bake_ && !bake_->prefix.empty()) {
        std::vector<std::string> suffixes;
        const std::string prefix = bake_->prefix;
        for (const std::string& file : CardBakePaths(prefix).files()) suffixes.push_back(file.substr(prefix.size()));
        suffixes.push_back("Preview.png");
        const std::string followed = f.followPrefix(prefix, suffixes);
        if (followed != prefix) {
            auto moved = [&](std::string& p) {
                if (p.compare(0, prefix.size(), prefix) == 0) p = followed + p.substr(prefix.size());
            };
            bake_->prefix = followed;
            for (std::string& p : bake_->paths) moved(p);
            moved(bake_->preview);
            moved(bake_->atlas);
            session().setCardMaps(formatCardMaps(followed, bake_->stamp, 0));
        }
    }
    // The Simulate nodes' bakes (what the session holds stays: the same frames).
    for (auto& [node, ref] : simBakes_) ref.file = f.follow(ref.file);
    // Reference photos and their masks.
    if (!referencePhotos().empty()) {
        json photos = json::parse(referencePhotos(), nullptr, false);
        bool photosMoved = false;
        if (photos.is_array()) {
            for (json& p : photos) {
                for (const char* key : {"image", "mask"}) {
                    if (!p.contains(key) || !p[key].is_string()) continue;
                    const std::string stored = p[key].get<std::string>(), followed = f.follow(stored);
                    if (followed != stored) {
                        p[key] = followed;
                        photosMoved = true;
                    }
                }
            }
        }
        if (photosMoved) referencePhotos_ = photos.dump();
    }
    savedCopies_ = f.copied();
    saveWarnings_ = f.warnings();
    if (!untitledRoot_.empty() && !f.copied().empty()) untitledRoot_.clear();  // saved: its own folders now
}

std::string GroomDocument::collectFiles(CollectReport* out) {
    if (path_.empty()) return "save the groom first: its files are collected into its folder";
    CollectReport r;
    const std::string dir = fs::absolute(fs::u8path(path_)).parent_path().generic_u8string();
    auto resolve = [&](const std::string& stored) {
        if (stored.empty() || isAbsolutePath(stored) || stored.find("<UDIM>") != std::string::npos) {
            return isAbsolutePath(stored) || stored.empty() ? stored : (fs::u8path(dir) / fs::u8path(stored)).generic_u8string();
        }
        const std::string found = resolveMapPath(stored);
        return found.empty() ? (fs::u8path(dir) / fs::u8path(stored)).generic_u8string() : fs::u8path(found).generic_u8string();
    };
    // The bookkeeping every host's Collect Files keeps (asset_files.h FileCollector).
    FileCollector files(dir);
    // `stored` from outside the folder into `kind`: the path to store now, or `stored` itself.
    auto collect = [&](const std::string& what, const std::string& stored, const std::string& kind) -> std::string {
        // A map or an image into the groom's maps folder (Maya's masks/<groom>), else the kind's.
        // ("groomCaches": the groom's own folder in caches, as its Simulate bakes are written.)
        const std::string folder = kind == "masks"         ? mapsDirectory()
                                   : kind == "groomCaches" ? assetFolder("caches") + "/" + fileSafeName(displayName())
                                                           : assetFolder(kind);
        const std::string now = files.collect(what, stored, resolve(stored), folder);
        return now == stored ? stored : pathFromGroomFolder(now, dir);
    };
    ScalpMaps maps = session().scalpMaps();
    bool mapsChanged = false;
    for (auto& [name, entry] : maps.maps) {
        if (entry.kind != "texture") continue;
        const std::string stored = collect("map " + name, entry.texture, "masks");
        if (stored != entry.texture) entry.texture = stored, mapsChanged = true;
    }
    if (mapsChanged) {
        session().setScalpMaps(maps);
        paints_.clear();
    }
    bool graphChanged = false;
    std::map<std::string, std::string> collected;
    for (const auto& [label, value] : graphFileParams(graph_.dump())) {
        const std::string stored = trimmed(value);
        const std::string now = collect(label, stored, "masks");
        if (now != stored) collected[label] = now;
    }
    if (!collected.empty()) graph_ = json::parse(withGraphFileParams(graph_.dump(), collected, &graphChanged));
    if (graphChanged) applyGraph();
    if (bake_ && !bake_->prefix.empty()) {
        // The card bake as a whole: every file of its prefix into cards.
        const std::string prefix = bake_->prefix, full = files.collectCardBake(prefix, defaultBakeDirectory());
        if (full != prefix) {
            auto moved = [&](std::string& p) {
                if (p.compare(0, prefix.size(), prefix) == 0) p = full + p.substr(prefix.size());
            };
            bake_->prefix = full;
            for (std::string& p : bake_->paths) moved(p);
            moved(bake_->preview);
            moved(bake_->atlas);
            session().setCardMaps(formatCardMaps(full, bake_->stamp, 0));
        }
    }
    for (auto& [node, ref] : simBakes_) ref.file = collect("simulation " + node, ref.file, "groomCaches");
    if (!referencePhotos().empty()) {
        json photos = json::parse(referencePhotos(), nullptr, false);
        bool photosChanged = false;
        if (photos.is_array()) {
            for (json& p : photos) {
                for (const char* key : {"image", "mask"}) {
                    if (!p.contains(key) || !p[key].is_string()) continue;
                    const std::string stored = p[key].get<std::string>(), now = collect(std::string("photo ") + p.value("name", std::string()), stored, "references");
                    if (now != stored) p[key] = now, photosChanged = true;
                }
            }
        }
        if (photosChanged) referencePhotos_ = photos.dump();
    }
    r.copied = files.copied();
    r.missing = files.missing();
    r.inside = files.inside();
    if (!r.copied.empty()) dirty_ = true;
    if (out) *out = std::move(r);
    return {};
}

std::string GroomDocument::importReferenceImage(const std::string& path) {
    std::error_code ec;
    if (!fs::is_regular_file(fs::u8path(path), ec)) return {};
    // Into the references folder: the same file there as it is, other content a free name (every
    // host's: asset_files.h collectFile).
    const std::string full = collectFile(fs::absolute(fs::u8path(path), ec).generic_u8string(), assetFolder("references"));
    if (full.empty()) return {};
    return path_.empty() ? full : pathFromGroomFolder(full, fs::u8path(path_).parent_path().generic_u8string());
}

std::string GroomDocument::referenceImageFile(const std::string& stored) const {
    if (stored.empty() || isAbsolutePath(stored)) return stored;
    std::vector<std::string> dirs;
    if (!path_.empty()) dirs.push_back(fs::u8path(path_).parent_path().generic_u8string());
    if (!scene_->file().directory.empty()) dirs.push_back(scene_->file().directory);
    const std::string found = resolvePathIn(stored, dirs);
    return found.empty() ? stored : fs::u8path(found).generic_u8string();
}

void GroomDocument::recoveredAs(const std::string& path) {
    const bool moved = path != path_;
    path_ = path;
    dirty_ = true;
    if (moved && !path_.empty()) {
        // Relative paths (texture maps) resolve against the file's folder first, then the copy's
        // and its search paths (the autosave's: the file's folder among them).
        std::vector<std::string> dirs{fs::u8path(path_).parent_path().u8string()};
        if (!scene_->file().directory.empty()) dirs.push_back(scene_->file().directory);
        dirs.insert(dirs.end(), scene_->file().searchPaths.begin(), scene_->file().searchPaths.end());
        session().setPathResolver(searchPathResolver(std::move(dirs)));
        paints_.clear();
    }
}

}  // namespace groomlab::app
