// SPDX-License-Identifier: Apache-2.0
#include "maya_inputs.h"

#include <algorithm>
#include <cstdlib>
#include <map>
#include <filesystem>
#include <mutex>
#include <set>

#include <cstring>

#include <maya/MAnimControl.h>
#include <maya/MColorArray.h>
#include <maya/MDoubleArray.h>
#include <maya/MDGContext.h>
#include <maya/MDGContextGuard.h>
#include <maya/MFnAnimCurve.h>
#include <maya/MDagPath.h>
#include <maya/MFloatPointArray.h>
#include <maya/MFloatArray.h>
#include <maya/MFileIO.h>
#include <maya/MFnAttribute.h>
#include <maya/MFnDagNode.h>
#include <maya/MFnDependencyNode.h>
#include <maya/MGlobal.h>
#include <maya/MFnGeometryData.h>
#include <maya/MFnMesh.h>
#include <maya/MFnStringArrayData.h>
#include <maya/MFnNurbsCurve.h>
#include <maya/MFnReference.h>
#include <maya/MItDependencyGraph.h>
#include <maya/MItDependencyNodes.h>
#include <maya/MFnMatrixData.h>
#include <maya/MPlugArray.h>
#include <maya/MIntArray.h>
#include <maya/MItMeshPolygon.h>
#include <maya/MMatrix.h>
#include <maya/MObjectHandle.h>
#include <maya/MPlug.h>
#include <maya/MPoint.h>
#include <maya/MPointArray.h>
#include <maya/MSelectionList.h>
#include <maya/MStringArray.h>
#include <maya/MTime.h>

#include "groomlab/display.h"
#include "groomlab/groom_file.h"
#include "groomlab/graph_io.h"
#include "groomlab/paths.h"
#include "groomlab/random.h"
#include "groomlab/scalp_maps.h"
#include "groomlab/subdivision.h"

#if GROOMLAB_HAS_ALEMBIC
#include "groomlab/alembic_io.h"
#include "groomlab/alembic_operators.h"
#include "groomlab/sim_cache.h"
#endif

namespace groomlab::maya {

void registerHostOperators() {
#if GROOMLAB_HAS_ALEMBIC
    registerAlembicOperators(OperatorRegistry::instance());
#endif
}

namespace {

std::mutex gPathsMutex;
std::vector<std::string> gSearchPaths;
std::shared_ptr<const PathResolver> gResolver = std::make_shared<const PathResolver>();
// Groom nodes from referenced files (by MObjectHandle hash): their reference file's folder.
std::map<unsigned, std::string> gReferenceFolders;

std::string folderOf(const std::string& file) {
    const std::size_t slash = file.find_last_of("/\\");
    return slash == std::string::npos ? std::string() : file.substr(0, slash);
}

// Every groomLabShape from a loaded reference, with the folder of the file it came from (the
// innermost reference holding it, for nested references).
std::map<unsigned, std::string> readReferenceFolders() {
    std::map<unsigned, std::string> out;
    std::vector<std::pair<MObject, std::string>> references;
    for (MItDependencyNodes it(MFn::kReference); !it.isDone(); it.next()) {
        MObject node = it.thisNode();
        MStatus status;
        MFnReference ref(node, &status);
        if (!status) continue;
        const MString name = ref.name();
        if (name == "sharedReferenceNode" || name == "_UNKNOWN_REF_NODE_") continue;
        if (!ref.isLoaded(&status) || !status) continue;
        // The resolved file with its folder: Maya 2022 gives the bare name for includePath
        // true and the full path for false, so take whichever of the two has a folder.
        std::string folder;
        for (const bool flag : {false, true}) {
            const MString file = ref.fileName(true, flag, false, &status);
            if (status && folder.empty()) folder = folderOf(file.asChar());
        }
        if (folder.empty()) continue;
        references.emplace_back(node, folder);
    }
    if (references.empty()) return out;
    for (MItDependencyNodes it(MFn::kPluginShape); !it.isDone(); it.next()) {
        MObject node = it.thisNode();
        MFnDependencyNode fn(node);
        if (fn.typeName() != "groomLabShape" || !fn.isFromReferencedFile()) continue;
        for (const auto& [refNode, folder] : references) {
            MStatus status;
            if (MFnReference(refNode).containsNodeExactly(node, &status) && status) {
                out[MObjectHandle(node).hashCode()] = folder;
                break;
            }
        }
    }
    return out;
}

}  // namespace

void refreshMayaPaths() {
    std::vector<std::string> dirs;
    MString named;  // "" for an untitled scene (currentFile() names untitled under the project)
    MGlobal::executeCommand("file -q -sceneName", named);
    const std::string scene = named.asChar();
    const std::size_t slash = scene.find_last_of("/\\");
    if (slash != std::string::npos) dirs.push_back(scene.substr(0, slash));
    MString root;
    if (MGlobal::executeCommand("workspace -q -rootDirectory", root) && root.length() > 0) dirs.push_back(root.asChar());
    auto resolver = std::make_shared<const PathResolver>(searchPathResolver(dirs));
    std::map<unsigned, std::string> folders = readReferenceFolders();
    std::lock_guard<std::mutex> lock(gPathsMutex);
    gSearchPaths = std::move(dirs);
    gResolver = std::move(resolver);
    gReferenceFolders = std::move(folders);
}

std::string referenceFolderOf(const MObject& node) {
    if (node.isNull()) return {};
    MStatus status;
    MFnDependencyNode fn(node, &status);
    if (!status || !fn.isFromReferencedFile()) return {};
    const unsigned key = MObjectHandle(node).hashCode();
    std::lock_guard<std::mutex> lock(gPathsMutex);
    const auto it = gReferenceFolders.find(key);
    return it == gReferenceFolders.end() ? std::string() : it->second;
}

std::vector<std::string> nodeSearchPaths(const MObject& node) {
    std::vector<std::string> dirs = mayaSearchPaths();
    const std::string folder = referenceFolderOf(node);
    if (!folder.empty()) dirs.insert(dirs.begin(), folder);
    return dirs;
}

std::shared_ptr<const PathResolver> nodePathResolver(const MObject& node) {
    const std::string folder = referenceFolderOf(node);
    if (folder.empty()) return mayaPathResolver();
    return std::make_shared<const PathResolver>(searchPathResolver(nodeSearchPaths(node)));
}

std::string resolveNodePath(const MObject& node, const std::string& path) {
    const std::shared_ptr<const PathResolver> resolver = nodePathResolver(node);
    const ScopedPathResolver scope(resolver.get());
    return resolvePath(path);
}

std::vector<std::string> mayaSearchPaths() {
    std::lock_guard<std::mutex> lock(gPathsMutex);
    return gSearchPaths;
}

std::shared_ptr<const PathResolver> mayaPathResolver() {
    std::lock_guard<std::mutex> lock(gPathsMutex);
    return gResolver;
}

std::string resolveMayaPath(const std::string& path) {
    const std::shared_ptr<const PathResolver> resolver = mayaPathResolver();
    const ScopedPathResolver scope(resolver.get());
    return resolvePath(path);
}

std::string portableMayaPath(const std::string& path) {
    return portablePath(path, mayaSearchPaths(), false);  // every host's rule (paths.h): the scene's folder, then the project
}

namespace {

// World-space geometry data (worldMesh, worldSpace plugs) keeps points in
// object space plus a matrix; apply it to get world positions.
MMatrix dataMatrix(const MObject& data) {
    MFnGeometryData fn(data);
    MMatrix m;
    if (fn.getMatrix(m) != MS::kSuccess) return MMatrix::identity;
    return m;
}

}  // namespace

std::vector<Vec3f> sampleCurveData(const MObject& curveData, unsigned samples) {
    MStatus status;
    MFnNurbsCurve fn(curveData, &status);
    if (!status) return {};

    const MMatrix m = dataMatrix(curveData);
    const double length = fn.length();
    std::vector<Vec3f> pts(samples);
    for (unsigned i = 0; i < samples; ++i) {
        const double param = fn.findParamFromLength(length * i / (samples - 1));
        MPoint p;
        fn.getPointAtParam(param, p, MSpace::kObject);
        p *= m;
        pts[i] = {static_cast<float>(p.x), static_cast<float>(p.y), static_cast<float>(p.z)};
    }
    return pts;
}

namespace {

std::uint64_t hashInts(std::uint64_t h, const MIntArray& a) {
    h = hashCombine(h, a.length());
    for (unsigned i = 0; i < a.length(); ++i) h = hashCombine(h, static_cast<std::uint64_t>(static_cast<std::int64_t>(a[i])));
    return h;
}

std::uint64_t hashFloats(std::uint64_t h, const MFloatArray& a) {
    h = hashCombine(h, a.length());
    for (unsigned i = 0; i < a.length(); ++i) h = hashFloat(h, a[i]);
    return h;
}

std::uint64_t hashDataMatrix(std::uint64_t h, const MObject& data) {
    const MMatrix m = dataMatrix(data);
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) h = hashDouble(h, m(r, c));
    }
    return h;
}

// The mesh's points as stored (object space, read in place), into `h`.
std::uint64_t hashMeshPoints(std::uint64_t h, MFnMesh& fn) {
    MStatus status;
    const int n = fn.numVertices();
    h = hashCombine(h, static_cast<std::uint64_t>(n));
    const float* raw = fn.getRawPoints(&status);
    if (status && raw) {
        for (std::size_t i = 0; i < static_cast<std::size_t>(n) * 3; ++i) h = hashFloat(h, raw[i]);
    } else {
        MPointArray points;
        fn.getPoints(points, MSpace::kObject);
        for (unsigned i = 0; i < points.length(); ++i) {
            h = hashDouble(hashDouble(hashDouble(h, points[i].x), points[i].y), points[i].z);
        }
    }
    return h;
}

// Everything toScalpMesh reads of mesh data but the points and the matrix: topology, the
// current UV set's UVs and their assignment, every color set's face-vertex colors.
std::uint64_t hashMeshAttributes(std::uint64_t h, MFnMesh& fn) {
    MIntArray counts, vertices;
    fn.getVertices(counts, vertices);
    h = hashInts(hashInts(h, counts), vertices);
    h = hashCombine(h, std::hash<std::string>{}(fn.currentUVSetName().asChar()));
    MFloatArray us, vs;
    fn.getUVs(us, vs);
    h = hashFloats(hashFloats(h, us), vs);
    MIntArray uvCounts, uvIds;
    fn.getAssignedUVs(uvCounts, uvIds);
    h = hashInts(hashInts(h, uvCounts), uvIds);
    MStringArray colorSets;
    fn.getColorSetNames(colorSets);
    for (unsigned s = 0; s < colorSets.length(); ++s) {
        h = hashCombine(h, std::hash<std::string>{}(colorSets[s].asChar()));
        MColorArray colors;
        fn.getFaceVertexColors(colors, &colorSets[s]);
        h = hashCombine(h, colors.length());
        for (unsigned i = 0; i < colors.length(); ++i) {
            h = hashFloat(hashFloat(hashFloat(hashFloat(h, colors[i].r), colors[i].g), colors[i].b), colors[i].a);
        }
    }
    return h;
}

// A fingerprint of everything toScalpMesh reads of mesh data: points, topology, the current
// UV set's UVs and their assignment, every color set's face-vertex colors, the matrix.
std::uint64_t meshDataKey(std::uint64_t h, const MObject& data) {
    if (data.isNull()) return hashCombine(h, 0);
    MStatus status;
    MFnMesh fn(data, &status);
    if (!status) return hashCombine(h, 1);
    h = hashMeshPoints(h, fn);
    h = hashMeshAttributes(h, fn);
    return hashDataMatrix(h, data);
}

// A fingerprint of everything sampleCurveData reads of curve data: CVs, knots, degree, form, matrix.
std::uint64_t curveDataKey(std::uint64_t h, const MObject& data) {
    MStatus status;
    MFnNurbsCurve fn(data, &status);
    if (!status) return hashCombine(h, 1);
    MPointArray cvs;
    fn.getCVs(cvs, MSpace::kObject);
    h = hashCombine(h, cvs.length());
    for (unsigned i = 0; i < cvs.length(); ++i) {
        h = hashDouble(hashDouble(hashDouble(hashDouble(h, cvs[i].x), cvs[i].y), cvs[i].z), cvs[i].w);
    }
    MDoubleArray knots;
    fn.getKnots(knots);
    h = hashCombine(h, knots.length());
    for (unsigned i = 0; i < knots.length(); ++i) h = hashDouble(h, knots[i]);
    h = hashCombine(hashCombine(h, static_cast<std::uint64_t>(fn.degree())), static_cast<std::uint64_t>(fn.form()));
    return hashDataMatrix(h, data);
}

// World-space vertex positions of mesh data; empty if it is not a mesh.
std::vector<Vec3f> meshPoints(const MObject& meshData) {
    if (meshData.isNull()) return {};
    MStatus status;
    MFnMesh fn(meshData, &status);
    if (!status) return {};
    MPointArray mpoints;
    fn.getPoints(mpoints, MSpace::kObject);
    const MMatrix m = dataMatrix(meshData);
    std::vector<Vec3f> points(mpoints.length());
    for (unsigned i = 0; i < mpoints.length(); ++i) {
        const MPoint p = mpoints[i] * m;
        points[i] = {static_cast<float>(p.x), static_cast<float>(p.y), static_cast<float>(p.z)};
    }
    return points;
}

}  // namespace

std::uint64_t meshPointsKey(std::uint64_t h, const MObject& data) {
    if (data.isNull()) return hashCombine(h, 0);
    MStatus status;
    MFnMesh fn(data, &status);
    if (!status) return hashCombine(h, 1);
    h = hashCombine(hashMeshPoints(h, fn), static_cast<std::uint64_t>(fn.numPolygons()));
    return hashDataMatrix(h, data);
}

std::uint64_t meshTopologyKey(std::uint64_t h, const MObject& data) {
    if (data.isNull()) return hashCombine(h, 0);
    MStatus status;
    MFnMesh fn(data, &status);
    if (!status) return hashCombine(h, 1);
    h = hashCombine(hashCombine(h, static_cast<std::uint64_t>(fn.numVertices())), static_cast<std::uint64_t>(fn.numPolygons()));
    h = hashCombine(h, static_cast<std::uint64_t>(fn.numFaceVertices()));
    MIntArray counts, vertices;
    fn.getVertices(counts, vertices);
    h = hashInts(hashInts(h, counts), vertices);
    h = hashCombine(h, std::hash<std::string>{}(fn.currentUVSetName().asChar()));
    h = hashCombine(h, static_cast<std::uint64_t>(fn.numUVs()));
    MStringArray colorSets;
    fn.getColorSetNames(colorSets);
    for (unsigned c = 0; c < colorSets.length(); ++c) h = hashCombine(h, std::hash<std::string>{}(colorSets[c].asChar()));
    return h;
}

std::uint64_t meshAttributesKey(std::uint64_t h, const MObject& data) {
    if (data.isNull()) return hashCombine(h, 0);
    MStatus status;
    MFnMesh fn(data, &status);
    if (!status) return hashCombine(h, 1);
    return hashMeshAttributes(hashCombine(h, static_cast<std::uint64_t>(fn.numVertices())), fn);
}

std::vector<Vec3f> meshWorldPoints(const MObject& meshData) { return meshPoints(meshData); }

namespace {

// Maya plays or scrubs: the Evaluation Manager (and cached playback) evaluates the scene frame by
// frame, and nothing is evaluated at another time from inside it (an MDGContextGuard in compute
// pulls the scalp's history while the graph plays): readers of other times read nothing then.
bool playing() { return MAnimControl::isPlaying() || MAnimControl::isScrubbing(); }

}  // namespace

std::vector<Vec3f> scalpPointsAt(const MObject& node, const MTime& time) {
    const MPlug plug = MFnDependencyNode(node).findPlug("inMesh", true);
    if (plug.isNull() || !plug.isConnected()) return {};
    MDGContext context(time);
    MDGContextGuard guard(context);
    return meshPoints(plug.asMObject());
}

namespace {

// A matrix plug's uniform scale (the cube root of its 3x3 determinant's size); 0 where unreadable.
double uniformScaleOf(const MPlug& plug) {
    if (plug.isNull()) return 0.0;
    MObject data = plug.asMObject();
    if (data.isNull()) return 0.0;
    MFnMatrixData fn(data);
    const MMatrix m = fn.matrix();
    const double det = m(0, 0) * (m(1, 1) * m(2, 2) - m(1, 2) * m(2, 1)) - m(0, 1) * (m(1, 0) * m(2, 2) - m(1, 2) * m(2, 0)) +
                       m(0, 2) * (m(1, 0) * m(2, 1) - m(1, 1) * m(2, 0));
    const double s = std::cbrt(std::abs(det));
    return std::isfinite(s) ? s : 0.0;
}

}  // namespace

double scalpTransformScaleAt(const MObject& node, const MTime& time) {
    const MPlug plug = MFnDependencyNode(node).findPlug("inMesh", true);
    if (plug.isNull() || !plug.isConnected()) return 0.0;
    const MPlug source = plug.source();
    if (source.isNull()) return 0.0;
    MObject mesh = source.node();
    MDGContext context(time);
    MDGContextGuard guard(context);
    // The mesh's world matrix (its instance's: worldMesh[i] feeds inMesh).
    double scale = 1.0;
    MPlug world = MFnDependencyNode(mesh).findPlug("worldMatrix", true);
    if (!world.isNull()) {
        const unsigned index = source.isElement() ? source.logicalIndex() : 0u;
        const double s = uniformScaleOf(world.elementByLogicalIndex(index));
        if (s > 0.0) scale = s;
    }
    // A skinned mesh: its rig's root joint (the influence nearest the top of the DAG) at that time.
    MStatus status;
    MItDependencyGraph skins(mesh, MFn::kSkinClusterFilter, MItDependencyGraph::kUpstream, MItDependencyGraph::kDepthFirst,
                             MItDependencyGraph::kNodeLevel, &status);
    if (status == MS::kSuccess && !skins.isDone()) {
        const MPlug matrices = MFnDependencyNode(skins.currentItem()).findPlug("matrix", true);
        unsigned shallowest = ~0u;
        MPlug rootMatrix;
        for (unsigned i = 0; !matrices.isNull() && i < matrices.numConnectedElements(); ++i) {
            const MPlug from = matrices.connectionByPhysicalIndex(i).source();
            if (from.isNull()) continue;
            MDagPath path;
            if (MDagPath::getAPathTo(from.node(), path) != MS::kSuccess) continue;
            if (path.length() < shallowest) {
                shallowest = path.length();
                rootMatrix = from;
            }
        }
        const double s = uniformScaleOf(rootMatrix);
        if (s > 0.0) scale *= s;
    }
    return scale;
}

std::vector<Vec3f> joinedScalpPointsAt(const MObject& node, const MTime& time, const NamedScalps& extras) {
    std::vector<Vec3f> points = scalpPointsAt(node, time);
    if (points.empty() || extras.empty()) return points;
    // Each other scalp's mesh plug by its name (the first element of a name, as they join).
    std::map<std::string, MPlug> plugs;
    MPlug array = MFnDependencyNode(node).findPlug("extraScalps", true);
    if (!array.isNull()) {
        const unsigned count = array.evaluateNumElements();
        for (unsigned i = 0; i < count; ++i) {
            const MPlug element = array.elementByPhysicalIndex(i);
            const MPlug meshPlug = element.child(0);
            if (!meshPlug.isConnected()) continue;
            const std::string name = colliderNameFor(meshPlug, element.child(1).asString().asChar());
            if (!name.empty()) plugs.emplace(name, meshPlug);
        }
    }
    MDGContext context(time);
    MDGContextGuard guard(context);
    for (const auto& [name, mesh] : extras) {
        if (!mesh) continue;
        std::vector<Vec3f> own;
        if (const auto it = plugs.find(name); it != plugs.end()) own = meshPoints(it->second.asMObject());
        // Not read (another topology then, or no longer connected): its points now (still).
        const std::vector<Vec3f>& use = own.size() == mesh->pointCount() ? own : mesh->points();
        points.insert(points.end(), use.begin(), use.end());
    }
    return points;
}

double mayaFramesPerSecond() { return canonicalFramesPerSecond(MTime(1.0, MTime::kSeconds).as(MTime::uiUnit())); }

double mayaSeconds(const MTime& time) { return frameToSeconds(time.as(MTime::uiUnit()), mayaFramesPerSecond()); }

MTime mayaTime(double seconds) { return MTime(secondsToFrame(seconds, mayaFramesPerSecond()), MTime::uiUnit()); }

void prepareSimulation(const MObject& node, GroomSession& session, SimCacheInputs& inputs) {
    session.setFramesPerSecond(mayaFramesPerSecond());
    // The frames on the way: the scalp's points at that time on the scalp the session has, and the
    // groom's other scalps and its colliders then (each read as a frame shows it), so Simulate
    // Range, and the bake it makes, meets a body moving through the hair where playing met it (it
    // was met where it stood at the frame shown), and its driver curves then. Guide curves, guide caches and controls at other
    // times are not read. Not while Maya plays or scrubs (playing): frames stepped over then
    // (playback started past a node's last frame, frames real-time playback dropped) are stepped in
    // one go toward the inputs as shown (GroomSession::setSimulationFeed).
    const MObjectHandle handle(node);
    session.setSimulationFeed([handle, &session](double seconds) {
        const MeshPtr& scalp = session.rawScalp();
        if (!handle.isValid() || !scalp || playing()) return false;
        const MTime time = mayaTime(seconds);
        std::vector<Vec3f> points = scalpPointsAt(handle.object(), time);
        if (points.size() != scalp->pointCount()) return false;
        session.updateScalpPoints(std::move(points));
        MDGContext context(time);
        MDGContextGuard guard(context);
        session.setScalps(extraScalpsFromNode(handle.object()));
        session.setMeshes(colliderMeshesFromNode(handle.object()));
        session.setDrivers(driverCurvesFromNode(handle.object()));  // a rig's drivers then (DriveGuides)
        return true;
    });
    const MPlug plug = MFnDependencyNode(node).findPlug("simCaches", true);
    attachSimulationCaches(nodePathResolver(node), plug.isNull() ? std::string() : plug.asString().asChar(), session, inputs);
}

void attachSimulationCaches(const std::shared_ptr<const PathResolver>& paths, const std::string& text, GroomSession& session,
                            SimCacheInputs& inputs) {
    if (text == inputs.text) return;
    inputs.text = text;
    for (const std::string& name : inputs.attached) session.setBakedSimulation(name, nullptr);
    inputs.attached.clear();
    inputs.error.clear();
#if GROOMLAB_HAS_ALEMBIC
    std::map<std::string, SimCacheRef> caches;
    try {
        caches = simCachesFromJson(text);
    } catch (const std::exception& e) {
        inputs.error = e.what();
        return;
    }
    for (const auto& [name, ref] : caches) {
        try {
            std::string file;
            {
                const ScopedPathResolver scope(paths.get());
                file = resolvePath(ref.file);
            }
            session.setBakedSimulation(name, readSimulationCache(file, ref.object, ref.stamp));
            inputs.attached.push_back(name);
        } catch (const std::exception& e) {
            inputs.error = "simCaches: " + name + ": " + e.what();
        }
    }
#endif
}

std::vector<Vec3f> joinedScalpPoints(const MeshPtr& scalp, const NamedScalps& extras) {
    if (!scalp) return {};
    std::vector<Vec3f> points = scalp->points();
    for (const auto& [name, mesh] : extras) {
        if (mesh) points.insert(points.end(), mesh->points().begin(), mesh->points().end());
    }
    return points;
}

ScalpHistoryPtr sampleScalpHistory(const MObject& node, const MTime& now, int frames, const std::vector<Vec3f>& current,
                                   ScalpPointCache& cache, bool reread, const std::vector<double>& loopFrames,
                                   const NamedScalps& extras) {
    if (frames <= 0 || current.empty()) return nullptr;
    const MTime::Unit unit = MTime::uiUnit();
    const double step = frameToSeconds(1.0, mayaFramesPerSecond());  // the CLI's and the procedural's
    const double nowFrame = now.as(unit);
    auto key = [](double seconds) { return static_cast<std::int64_t>(std::llround(seconds * 1e6)); };
    std::vector<MTime> times;
    std::vector<std::int64_t> keys;
    for (int k = 1; k <= frames; ++k) {
        times.emplace_back(nowFrame - k, unit);
        keys.push_back(key(mayaSeconds(times.back())));
    }
    // Then a looping Jiggle's frames from the end of its range (by the session's rule, frames to seconds).
    std::vector<double> loopSeconds;
    for (const double f : loopFrames) {
        times.emplace_back(f, unit);
        loopSeconds.push_back(mayaSeconds(times.back()));
        keys.push_back(key(loopSeconds.back()));
    }
    return cache.history(key(mayaSeconds(now)), keys, step, current,
                         [&](std::size_t k) {
                             // Playing or scrubbing: a frame not seen yet reads as the scalp now (still);
                             // stopped on a frame, every frame is read again (reread).
                             // Every scalp's own frame: the other scalps' after the groom's own.
                             if (playing()) return std::vector<Vec3f>();
                             return extras.empty() ? scalpPointsAt(node, times[k]) : joinedScalpPointsAt(node, times[k], extras);
                         },
                         reread, loopSeconds);
}

MeshPtr toScalpMesh(const MObject& meshData, const MObject& restData, std::string* restError) {
    if (restError) restError->clear();
    if (meshData.isNull()) return nullptr;
    MStatus status;
    MFnMesh fn(meshData, &status);
    if (!status) return nullptr;

    std::vector<Vec3f> points = meshPoints(meshData);

    // Maya triangulates a polygon by its shape (a quad across its shorter diagonal): a deformed
    // mesh can split its quads otherwise than at rest, and strands are placed by triangle. With
    // a rest mesh of the same faces, the rest mesh's split: the same triangles at every frame,
    // whichever frame the scalp is first read at (a jump, cached playback's background fill).
    MFnMesh triangulated(meshData);
    if (!restData.isNull()) {
        MFnMesh rest(restData, &status);
        if (status && rest.numVertices() == fn.numVertices() && rest.numPolygons() == fn.numPolygons() &&
            rest.numFaceVertices() == fn.numFaceVertices()) {
            MIntArray counts, vertices, restCounts, restVertices;
            fn.getVertices(counts, vertices);
            rest.getVertices(restCounts, restVertices);
            bool same = counts.length() == restCounts.length() && vertices.length() == restVertices.length();
            for (unsigned i = 0; same && i < counts.length(); ++i) same = counts[i] == restCounts[i];
            for (unsigned i = 0; same && i < vertices.length(); ++i) same = vertices[i] == restVertices[i];
            if (same) triangulated.setObject(restData);
        }
    }
    MIntArray triCounts, triVerts;
    triangulated.getTriangles(triCounts, triVerts);
    std::vector<ScalpMesh::Triangle> tris(triVerts.length() / 3);
    for (unsigned t = 0; t < tris.size(); ++t) {
        tris[t] = {triVerts[3 * t], triVerts[3 * t + 1], triVerts[3 * t + 2]};
    }
    auto scalp = std::make_shared<ScalpMesh>(std::move(points), std::move(tris));
    // The polygons the triangles came from (each polygon's triangle count, in order): what
    // Catmull-Clark refines (subdivision.h), as Arnold and Maya's smooth preview do.
    std::vector<std::int32_t> polygons(triCounts.length());
    for (unsigned f = 0; f < triCounts.length(); ++f) polygons[f] = triCounts[f];
    scalp->setPolygons(std::move(polygons));
    std::vector<Vec3f> rest = meshPoints(restData);
    if (!rest.empty() && rest.size() == scalp->pointCount()) {
        scalp->setRestPoints(std::move(rest));
    } else if (!rest.empty() && restError) {
        // Without the rest pose the strands scatter on the deformed scalp, so they
        // change from frame to frame: say why.
        *restError = "restMesh: ignored, it has " + std::to_string(rest.size()) + " vertices and the scalp " +
                     std::to_string(scalp->pointCount()) + " (the rest mesh must be the same mesh, undeformed)";
    }

    // Per-corner data for maps. getTriangleOffsets gives, for each triangle
    // corner (in getTriangles order), its index in the mesh-wide face-vertex
    // list, which is how UV ids and face-vertex colors are ordered.
    MIntArray offsetCounts, cornerFaceVertex;
    triangulated.getTriangleOffsets(offsetCounts, cornerFaceVertex);
    const auto faceVertexCount = static_cast<unsigned>(fn.numFaceVertices());
    if (cornerFaceVertex.length() != scalp->triangleCount() * 3) return scalp;

    // UVs of the current UV set (only when every polygon is mapped).
    MFloatArray us, vs;
    MIntArray uvCounts, uvIds;
    if (fn.numUVs() > 0 && fn.getUVs(us, vs) == MS::kSuccess && fn.getAssignedUVs(uvCounts, uvIds) == MS::kSuccess &&
        uvIds.length() == faceVertexCount) {
        std::vector<ScalpMesh::UV> uvs(cornerFaceVertex.length());
        bool valid = true;
        for (unsigned c = 0; c < cornerFaceVertex.length() && valid; ++c) {
            const int id = uvIds[static_cast<unsigned>(cornerFaceVertex[c])];
            valid = id >= 0 && static_cast<unsigned>(id) < us.length();
            if (valid) uvs[c] = {us[static_cast<unsigned>(id)], vs[static_cast<unsigned>(id)]};
        }
        if (valid) scalp->setCornerUVs(std::move(uvs));
    }

    // Color sets (Paint Vertex Color). Unassigned colors read as black.
    MStringArray colorSets;
    fn.getColorSetNames(colorSets);
    for (unsigned s = 0; s < colorSets.length(); ++s) {
        MColorArray colors;
        if (fn.getFaceVertexColors(colors, &colorSets[s]) != MS::kSuccess || colors.length() != faceVertexCount) {
            continue;
        }
        std::vector<ScalpMesh::Color> corners(cornerFaceVertex.length());
        for (unsigned c = 0; c < cornerFaceVertex.length(); ++c) {
            const MColor& col = colors[static_cast<unsigned>(cornerFaceVertex[c])];
            corners[c] = col.r < 0.0f ? ScalpMesh::Color{0, 0, 0, 0} : ScalpMesh::Color{col.r, col.g, col.b, col.a};
        }
        scalp->setCornerColors(colorSets[s].asChar(), std::move(corners));
    }
    return scalp;
}

int renderSubdivisionOf(const MPlug& meshPlug, std::string* from) {
    if (from) from->clear();
    if (meshPlug.isNull() || !meshPlug.isConnected()) return 0;
    const MPlug source = meshPlug.source();
    if (source.isNull()) return 0;
    const MFnDependencyNode mesh(source.node());
    // Arnold (MtoA's attributes on the mesh): Catmull-Clark by its iterations.
    const MPlug type = mesh.findPlug("aiSubdivType", true);
    if (!type.isNull() && type.asInt() == 1) {
        const MPlug iterations = mesh.findPlug("aiSubdivIterations", true);
        const int n = iterations.isNull() ? 1 : iterations.asInt();
        if (n > 0) {
            if (from) *from = "Arnold catclark, " + std::to_string(n) + (n == 1 ? " iteration" : " iterations");
            return std::min(n, kMaxSubdivisionLevels);
        }
        return 0;
    }
    if (!type.isNull() && type.asInt() != 0) return 0;  // Arnold's linear subdivision: the cage's surface
    // Else Maya's smooth mesh preview (the 3 key), when it is on.
    const MPlug smooth = mesh.findPlug("displaySmoothMesh", true);
    if (!smooth.isNull() && smooth.asInt() != 0) {
        const MPlug level = mesh.findPlug("smoothLevel", true);
        const int n = level.isNull() ? 0 : level.asInt();
        if (n > 0) {
            if (from) *from = "smooth mesh preview, level " + std::to_string(n);
            return std::min(n, kMaxSubdivisionLevels);
        }
    }
    return 0;
}

MeshPtr withRenderSubdivision(const MeshPtr& mesh, int levels, const std::string& from) {
    if (!mesh || (levels == mesh->renderSubdivision() && from == mesh->renderSubdivisionFrom())) return mesh;
    auto copy = std::make_shared<ScalpMesh>(*mesh);
    copy->setRenderSubdivision(levels, from);
    return copy;
}

namespace {

// `mesh` with the render subdivision of the mesh feeding `meshPlug` (match render).
MeshPtr withRenderSubdivisionOf(const MeshPtr& mesh, const MPlug& meshPlug) {
    std::string from;
    const int levels = renderSubdivisionOf(meshPlug, &from);
    return withRenderSubdivision(mesh, levels, from);
}

}  // namespace

std::string colliderNameFor(const MPlug& meshChild, const std::string& given) {
    if (meshChild.isNull() || !meshChild.isConnected()) return {};
    if (!given.empty()) return given;
    // The connected mesh's transform (the shape's parent), its short name without namespaces.
    const MPlug source = meshChild.source();
    if (source.isNull()) return {};
    MObject node = source.node();
    if (node.hasFn(MFn::kDagNode)) {
        MFnDagNode dag(node);
        if (node.hasFn(MFn::kShape) && dag.parentCount() > 0) node = dag.parent(0);
    }
    std::string name = MFnDependencyNode(node).name().asChar();
    const std::size_t colon = name.find_last_of(':');
    if (colon != std::string::npos) name = name.substr(colon + 1);
    return name;
}

std::map<std::string, MeshPtr> colliderMeshesFromNode(const MObject& node, NodeInputCache* cache) {
    std::map<std::string, MeshPtr> meshes;
    MFnDependencyNode fn(node);
    MPlug array = fn.findPlug("colliderMeshes", true);
    if (array.isNull()) return meshes;
    std::vector<std::pair<std::string, MObject>> found;
    std::vector<MObject> rests;  // each one's rest mesh (colliderRestMesh, read only while connected), or null
    std::vector<MPlug> plugs;
    std::uint64_t key = hash64(0xC011);
    const unsigned count = array.evaluateNumElements();
    for (unsigned i = 0; i < count; ++i) {
        const MPlug element = array.elementByPhysicalIndex(i);
        const MPlug meshPlug = element.child(0);
        const std::string name = colliderNameFor(meshPlug, element.child(1).asString().asChar());
        if (name.empty()) continue;  // stale data once disconnected
        found.emplace_back(name, meshPlug.asMObject());
        const MPlug restPlug = element.numChildren() > 2 ? element.child(2) : MPlug();
        rests.push_back(!restPlug.isNull() && restPlug.isConnected() ? restPlug.asMObject() : MObject::kNullObj);
        plugs.push_back(meshPlug);
        if (cache) {
            key = meshDataKey(hashCombine(key, std::hash<std::string>{}(name)), found.back().second);
            key = meshDataKey(hashCombine(key, 0x2E57), rests.back());
            key = hashCombine(key, static_cast<std::uint64_t>(renderSubdivisionOf(meshPlug, nullptr)));  // match render
        }
    }
    if (cache && cache->collidersRead && key == cache->collidersKey) return cache->colliders;
    for (std::size_t i = 0; i < found.size(); ++i) {
        MeshPtr mesh = withRenderSubdivisionOf(toScalpMesh(found[i].second, rests[i]), plugs[i]);
        if (mesh && mesh->triangleCount() > 0) meshes[found[i].first] = std::move(mesh);
    }
    if (cache) {
        cache->collidersRead = true;
        cache->collidersKey = key;
        cache->colliders = meshes;
    }
    return meshes;
}

NamedScalps extraScalpsFromNode(const MObject& node, NodeInputCache* cache) {
    NamedScalps scalps;
    MFnDependencyNode fn(node);
    MPlug array = fn.findPlug("extraScalps", true);
    if (array.isNull()) return scalps;
    std::vector<std::pair<std::string, MObject>> found;
    std::vector<MObject> rests;  // each one's rest mesh (read only while connected), or null
    std::vector<MPlug> plugs;
    std::uint64_t key = hash64(0x5CA195);
    std::set<std::string> taken;
    const unsigned count = array.evaluateNumElements();
    for (unsigned i = 0; i < count; ++i) {  // in index order: the order they join in
        const MPlug element = array.elementByPhysicalIndex(i);
        const MPlug meshPlug = element.child(0);
        const std::string name = colliderNameFor(meshPlug, element.child(1).asString().asChar());
        if (name.empty() || !validScalpName(name) || !taken.insert(name).second) continue;  // unconnected, or not a name
        found.emplace_back(name, meshPlug.asMObject());
        const MPlug restPlug = element.numChildren() > 2 ? element.child(2) : MPlug();
        rests.push_back(!restPlug.isNull() && restPlug.isConnected() ? restPlug.asMObject() : MObject::kNullObj);
        plugs.push_back(meshPlug);
        if (cache) {
            key = meshDataKey(hashCombine(key, std::hash<std::string>{}(name)), found.back().second);
            key = meshDataKey(hashCombine(key, 0x2E57), rests.back());
            key = hashCombine(key, static_cast<std::uint64_t>(renderSubdivisionOf(meshPlug, nullptr)));  // match render
        }
    }
    if (cache && cache->scalpsRead && key == cache->scalpsKey) return cache->scalps;
    for (std::size_t i = 0; i < found.size(); ++i) {
        MeshPtr mesh = withRenderSubdivisionOf(toScalpMesh(found[i].second, rests[i]), plugs[i]);
        if (mesh && mesh->triangleCount() > 0) scalps.emplace_back(found[i].first, std::move(mesh));
    }
    if (cache) {
        cache->scalpsRead = true;
        cache->scalpsKey = key;
        cache->scalps = scalps;
    }
    return scalps;
}

MeshPtr joinedScalpFromNode(const MObject& node) {
    return combineScalps(rawScalpFromNode(node), extraScalpsFromNode(node));
}

GuidesPtr toGuides(const std::vector<GuideCurveData>& curves, const ScalpMesh* scalp) {
    auto guides = std::make_shared<GuideSet>();
    for (const GuideCurveData& c : curves) {
        std::vector<Vec3f> pts = sampleCurveData(c.curve, kGuideCurveSamples);
        if (pts.size() >= 2) guides->add(c.id, std::move(pts));
    }
    if (scalp) guides->orientAndRoot(*scalp);
    return guides;
}

GuidesPtr toDrivers(const std::vector<GuideCurveData>& curves) {
    auto drivers = std::make_shared<GuideSet>();
    for (const GuideCurveData& c : curves) {
        std::vector<Vec3f> pts = sampleCurveData(c.curve, kGuideCurveSamples);
        if (pts.size() >= 2) drivers->add(c.id, std::move(pts));
    }
    return drivers->empty() ? nullptr : GuidesPtr(drivers);
}

GuidesPtr driverCurvesFromNode(const MObject& node) {
    MPlug plug = MFnDependencyNode(node).findPlug("driverCurves", true);
    std::vector<GuideCurveData> curves;
    const unsigned count = plug.evaluateNumElements();
    for (unsigned i = 0; i < count; ++i) {
        MPlug element = plug.elementByPhysicalIndex(i);
        curves.push_back({element.logicalIndex(), element.asMObject()});
    }
    return toDrivers(curves);
}

struct GuideCacheReader {
#if GROOMLAB_HAS_ALEMBIC
    std::string path, object;
    std::unique_ptr<AlembicCurvesReader> reader;
#endif
};

GuidesPtr toGuidePoses(const std::vector<SimCurveData>& curves, const std::string& cacheFile,
                       const std::string& cacheObject, double seconds, std::string* error,
                       std::shared_ptr<GuideCacheReader>* reader) {
    std::map<std::uint64_t, std::vector<Vec3f>> poses;
#if GROOMLAB_HAS_ALEMBIC
    if (!cacheFile.empty()) {
        try {
            const std::string path = resolveMayaPath(cacheFile);
            StrandSet cached;
            if (reader) {  // opened once, read at every time asked
                if (!*reader || (*reader)->path != path || (*reader)->object != cacheObject) {
                    auto open = std::make_shared<GuideCacheReader>();
                    open->path = path;
                    open->object = cacheObject;
                    open->reader = std::make_unique<AlembicCurvesReader>(path, cacheObject);
                    *reader = std::move(open);
                }
                cached = (*reader)->reader->at(seconds);
            } else {
                cached = readAlembicCurvesAt(path, seconds, cacheObject);
            }
            for (std::size_t s = 0; s < cached.strandCount(); ++s) {
                const Vec3f* p = cached.strandPoints(s);
                poses[cached.ids()[s]].assign(p, p + cached.strandPointCount(s));
            }
        } catch (const std::exception& e) {
            if (error) *error = std::string("guideCache: ") + e.what();
        }
    }
#else
    (void)reader;
    if (!cacheFile.empty() && error) *error = "guideCache: this build has no Alembic support";
#endif
    for (const SimCurveData& c : curves) {
        std::vector<Vec3f> pts = sampleCurveData(c.curve, kGuideCurveSamples);
        if (pts.size() >= 2) poses[c.guideId] = std::move(pts);
    }
    if (poses.empty()) return nullptr;
    auto set = std::make_shared<GuideSet>();
    for (auto& [id, pts] : poses) set->add(id, std::move(pts));
    return set;
}

std::set<std::string> readHiddenGuideGroups(const MObject& node) {
    MFnDependencyNode fn(node);
    MPlug plug = fn.findPlug("hiddenGuideGroups", true);
    if (plug.isNull()) return {};
    return parseHiddenGuideGroups(plug.asString().asChar());
}

std::vector<GuideId> readGuideSelection(const MObject& node) {
    std::vector<GuideId> ids;
    MFnDependencyNode fn(node);
    MObject data = fn.findPlug("guideSelection", true).asMObject();
    if (data.isNull()) return ids;
    MFnStringArrayData array(data);
    const MStringArray values = array.array();
    for (unsigned i = 0; i < values.length(); ++i) {
        const GuideId id = std::strtoull(values[i].asChar(), nullptr, 10);
        if (id != kNoGuide) ids.push_back(id);
    }
    return ids;
}

float readSoftSelectRadius(const MObject& node) {
    return MFnDependencyNode(node).findPlug("softSelectRadius", true).asFloat();
}

std::shared_ptr<GuideHierarchy> toHierarchy(const std::string& json, const ScalpMesh* scalp, std::string* error, float scale) {
    try {
        auto hierarchy = std::make_shared<GuideHierarchy>(GuideHierarchy::fromJson(json));
        // What it could not read as saved (a guide's missing library shape): said once a session.
        static std::mutex warnedMutex;
        static std::set<std::string> warned;
        for (const std::string& w : hierarchy->loadWarnings()) {
            const std::lock_guard<std::mutex> lock(warnedMutex);
            if (warned.insert(w).second) MGlobal::displayWarning(MString("GroomLab: ") + w.c_str());
        }
        hierarchy->setScale(scale);
        hierarchy->evaluate(scalp);
        return hierarchy;
    } catch (const std::exception& e) {
        if (error) *error = std::string("guideData: ") + e.what();
        return nullptr;
    }
}

bool curveWorldData(const MString& name, MObject& data) {
    MSelectionList sel;
    if (sel.add(name) != MS::kSuccess) return false;
    MDagPath path;
    if (sel.getDagPath(0, path) != MS::kSuccess) return false;
    if (!path.hasFn(MFn::kNurbsCurve) && (path.extendToShape() != MS::kSuccess || !path.hasFn(MFn::kNurbsCurve))) {
        return false;
    }
    data = MFnDagNode(path).findPlug("worldSpace", true).elementByLogicalIndex(path.instanceNumber()).asMObject();
    return !data.isNull();
}

bool isControlAttribute(const MObject& attribute) {
    MFnAttribute fn(attribute);
    if (!fn.isDynamic()) return false;
    const MString name = fn.name();
    const MString prefix(kControlPrefix);
    return name.length() > prefix.length() && name.substring(0, prefix.length() - 1) == prefix;
}

std::vector<MObject> controlAttributes(const MObject& node) {
    std::vector<MObject> attributes;
    MFnDependencyNode fn(node);
    for (unsigned i = 0; i < fn.attributeCount(); ++i) {
        const MObject attribute = fn.attribute(i);
        if (!isControlAttribute(attribute)) continue;
        const MPlug plug(node, attribute);
        if (plug.isArray() || plug.isCompound()) continue;
        attributes.push_back(attribute);
    }
    return attributes;
}

std::map<std::string, float> readControls(const MObject& node, const std::vector<MObject>& attributes) {
    std::map<std::string, float> controls;
    const std::size_t prefix = std::strlen(kControlPrefix);
    for (const MObject& attribute : attributes) {
        controls[std::string(MFnAttribute(attribute).name().asChar()).substr(prefix)] = MPlug(node, attribute).asFloat();
    }
    return controls;
}

std::map<std::string, float> readControls(const MObject& node) { return readControls(node, controlAttributes(node)); }

float correctiveWeightPlug(const MObject& node, int slot) {
    MPlug array = MFnDependencyNode(node).findPlug("correctiveWeight", true);
    MIntArray indices;
    array.getExistingArrayAttributeIndices(indices);
    for (const int i : indices) {
        if (i == slot) return array.elementByLogicalIndex(static_cast<unsigned>(slot)).asFloat();
    }
    return 1.0f;
}

namespace {

// Keyed corrective weights as weight curves (every frame from the first key to
// the last, plus the keys themselves); unkeyed ones as their value now.
std::string withWeightCurves(const MObject& node, const std::string& text) {
    if (text.find("\"slot\"") == std::string::npos) return text;  // no layer reads a keyable weight
    Correctives correctives;
    try {
        correctives = Correctives::fromJson(text);
    } catch (const std::exception&) {
        return text;  // the evaluation reports it
    }
    const MPlug array = MFnDependencyNode(node).findPlug("correctiveWeight", true);
    for (CorrectiveLayer& l : correctives.layers) {
        if (l.slot < 0) continue;
        l.holdBefore = l.holdAfter = true;
        l.weightCurve.clear();
        l.weight = correctiveWeightPlug(node, l.slot);
        MStatus status;
        const MPlug plug = array.elementByLogicalIndex(static_cast<unsigned>(l.slot), &status);
        if (!status || !plug.isDestination()) continue;
        // The keys: an anim curve on the weight itself, or on the groom transform's
        // attribute driving it (where shots.py keeps them, for the timeline).
        MObject curveNode;
        MPlug source = plug.source();
        for (int hop = 0; hop < 2 && !source.isNull() && curveNode.isNull(); ++hop) {
            if (source.node().hasFn(MFn::kAnimCurve)) {
                curveNode = source.node();
            } else {
                source = source.source();
            }
        }
        if (curveNode.isNull()) continue;
        MFnAnimCurve curve(curveNode, &status);
        if (!status || curve.numKeys() == 0) continue;
        const MTime::Unit unit = MTime::uiUnit();
        const double first = curve.time(0).as(unit), last = curve.time(curve.numKeys() - 1).as(unit);
        std::vector<double> frames;
        for (double f = std::floor(first); f <= std::ceil(last) + 1e-9; f += 1.0) frames.push_back(f);
        for (unsigned k = 0; k < curve.numKeys(); ++k) frames.push_back(curve.time(k).as(unit));
        std::sort(frames.begin(), frames.end());
        frames.erase(std::unique(frames.begin(), frames.end(), [](double a, double b) { return std::abs(a - b) < 1e-6; }),
                     frames.end());
        for (const double f : frames) {
            double value = 0.0;
            curve.evaluate(MTime(f, unit), value);
            l.weightCurve.emplace_back(mayaSeconds(MTime(f, unit)), static_cast<float>(value));
        }
    }
    return correctives.toJson();
}

}  // namespace

MeshPtr rawScalpFromNode(const MObject& node, NodeInputCache* cache) {
    MFnDependencyNode fn(node);
    const MPlug rest = fn.findPlug("restMesh", true);  // stale data once disconnected: read only while connected
    const MObject meshData = fn.findPlug("inMesh", true).asMObject();
    const MObject restData = rest.isConnected() ? rest.asMObject() : MObject::kNullObj;
    MeshPtr raw;
    if (!cache) {
        raw = toScalpMesh(meshData, restData);
    } else {
        // Converting the mesh (triangles, UVs, color sets) costs more than reading its points.
        // The same points (and matrix) as last time: the same scalp, unless something else of it
        // was edited (the full fingerprint says). Other points on the same topology (a deforming
        // scalp's next frame or motion key): only the points move (ScalpMesh::withPoints), the
        // UVs and color sets as last converted; the first read after refreshStoredInputs checks
        // those too (an edit between two renders).
        const std::uint64_t restKey = meshPointsKey(hash64(0x2E57), restData);
        const std::uint64_t restTopologyKey = meshTopologyKey(hash64(0x70B0), restData);
        const std::uint64_t pointsKey = meshPointsKey(hash64(0x5CA1F), meshData);
        const std::uint64_t topologyKey = meshTopologyKey(hash64(0x70B0), meshData);
        bool converted = false;
        if (cache->scalpRead && cache->rawScalp && restTopologyKey == cache->restTopologyKey && topologyKey == cache->topologyKey) {
            const bool samePoints = pointsKey == cache->pointsKey && restKey == cache->restKey;
            // The same points: something else of the mesh may have been edited (its UVs or
            // colors); new points: that is checked only on a verifying read.
            const bool kept = (!samePoints && !cache->verifyScalp) ||
                              meshAttributesKey(hash64(0xA77B), meshData) == cache->attributesKey;
            if (kept && samePoints) {
                raw = cache->rawScalp;
                converted = true;
            } else if (kept) {
                // Points on the same topology; a rest mesh that moved (its transform animates) too.
                std::vector<Vec3f> points = meshPoints(meshData);
                const bool restKept = restKey == cache->restKey;
                const std::vector<Vec3f> rest = restKept ? std::vector<Vec3f>() : meshPoints(restData);
                if (points.size() == cache->rawScalp->pointCount() &&
                    (restKept || (cache->rawScalp->hasRest() && rest.size() == cache->rawScalp->pointCount()))) {
                    auto moved = std::make_shared<ScalpMesh>(cache->rawScalp->withPoints(std::move(points)));
                    if (!restKept) moved->setRestPoints(rest);
                    raw = std::move(moved);
                    converted = true;
                }
            }
        }
        if (!converted) {
            raw = toScalpMesh(meshData, restData);
            // The same scalp as last time: keep the old one, with what it has built.
            if (raw && cache->rawScalp && raw->contentHash() == cache->rawScalp->contentHash()) raw = cache->rawScalp;
            cache->scalpRead = true;
            cache->restTopologyKey = restTopologyKey;
            cache->topologyKey = topologyKey;
            cache->attributesKey = meshAttributesKey(hash64(0xA77B), meshData);
        }
        cache->restKey = restKey;
        cache->pointsKey = pointsKey;
        cache->verifyScalp = false;
    }
    // Match render: what the renderer refines the mesh by (read on every evaluation: Arnold's and
    // the smooth preview's attributes are on the mesh, not in its data).
    raw = withRenderSubdivisionOf(raw, fn.findPlug("inMesh", true));
    // Scaling with the scalp: the rest mesh sized as the node sizes it (GroomShape::scaleScalp).
    if (raw && raw->hasRest()) {
        const ScaleRead scale = readScale(node, cache);
        if (scale.on) raw = withRestArea(raw, scale.measure);
    }
    if (cache) cache->rawScalp = raw;
    return raw;
}

float groomScaleOf(const MObject& node) {
    const MPlug plug = MFnDependencyNode(node).findPlug("groomScale", true);
    if (plug.isNull()) return 1.0f;
    MDGContextGuard normal(MDGContext::fsNormal);
    const double k = plug.asDouble();
    return k > 0.0 && std::isfinite(k) ? static_cast<float>(k) : 1.0f;
}

ScaleRead readScale(const MObject& node, NodeInputCache* cache) {
    auto sourceOf = [](int value) { return value == 1 ? ScaleSource::Transform : value == 2 ? ScaleSource::Off : ScaleSource::Area; };
    if (cache && cache->scaleRead) {
        ScaleRead kept;
        kept.on = cache->scaleOn;
        kept.source = sourceOf(cache->scaleSource);
        kept.measure = cache->scaleMeasure;
        kept.reference = cache->scaleReference;
        kept.transform = cache->scaleTransform;
        kept.transformReference = cache->scaleTransformReference;
        kept.k = cache->scaleK;
        return kept;
    }
    ScaleRead out;
    MFnDependencyNode fn(node);
    const MPlug source = fn.findPlug("scaleSource", true);
    int sourceValue = 2;
    if (source.isNull()) {
        out.on = false;
        out.source = ScaleSource::Off;
    } else {
        MDGContextGuard normal(MDGContext::fsNormal);  // an export at another frame: the node's own measure
        sourceValue = source.asShort();
        out.source = sourceOf(sourceValue);
        out.on = out.source != ScaleSource::Off;
        const double area = fn.findPlug("scaleArea", true).asDouble();  // evaluates the groom when dirty
        const double k = fn.findPlug("groomScale", true).asDouble();
        const double set = fn.findPlug("scaleReference", true).asDouble();
        const double transform = fn.findPlug("scaleTransform", true).asDouble();
        const double setTransform = fn.findPlug("scaleTransformReference", true).asDouble();
        out.k = k > 0.0 ? k : 1.0;
        out.measure = area;
        out.transform = transform;
        // Transform Only: the area reference that gives the same k (hosts without transforms work k
        // out from the area: a groom file, the procedural).
        out.reference = out.source == ScaleSource::Transform && k > 0.0 && area > 0.0 ? area / (k * k)
                        : set > 0.0                                                   ? set
                        : (k > 0.0 && area > 0.0 ? area / (k * k) : 0.0);
        out.transformReference = setTransform > 0.0 ? setTransform : (k > 0.0 && transform > 0.0 ? transform / k : 0.0);
    }
    if (cache) {
        cache->scaleRead = true;
        cache->scaleOn = out.on;
        cache->scaleSource = sourceValue;
        cache->scaleMeasure = out.measure;
        cache->scaleReference = out.reference;
        cache->scaleTransform = out.transform;
        cache->scaleTransformReference = out.transformReference;
        cache->scaleK = out.k;
    }
    return out;
}

MeshPtr scalpFromNode(const MObject& node, NodeInputCache* cache, MapCatalog* maps, bool readMaps) {
    MFnDependencyNode fn(node);
    const MeshPtr raw = rawScalpFromNode(node, cache);
    // The vertex maps painted on it (scalpMaps) as color sets, as the shape evaluates them. The
    // text is read again unless the caller says it stands (a connected one is read anyway).
    const MPlug mapsPlug = fn.findPlug("scalpMaps", true);
    const bool kept = cache && cache->mapsRead && !readMaps && !mapsPlug.isDestination();
    const std::string text = kept ? std::string() : std::string(mapsPlug.asString().asChar());
    if (cache && cache->mapsRead && (kept || cache->mapsText == text)) {
        if (cache->mapsOn != raw) {  // the same maps on another scalp (a deforming one's next frame)
            if (raw && cache->mapsOn && cache->scalp && cache->scalp != cache->mapsOn && raw->samePointsLayout(*cache->mapsOn)) {
                // Only the points moved: the mapped scalp's color sets stay (what laying the maps gives).
                cache->scalp = std::make_shared<const ScalpMesh>(cache->scalp->withPoints(raw->points()));
            } else {
                cache->scalp = cache->parsedMaps ? withVertexMaps(raw, *cache->parsedMaps, "", nullptr, nullptr) : raw;
            }
            cache->mapsOn = raw;
        }
        if (maps) *maps = cache->maps;
        return cache->scalp;
    }
    MeshPtr scalp = raw;
    MapCatalog catalog;
    std::shared_ptr<const ScalpMaps> parsedMaps;
    try {
        parsedMaps = std::make_shared<const ScalpMaps>(ScalpMaps::fromJson(text));
        scalp = withVertexMaps(scalp, *parsedMaps, "", nullptr, nullptr);
        catalog = parsedMaps->catalog();
    } catch (const std::exception&) {
        // bad scalpMaps text: the shape reports it; here the maps are simply absent
        parsedMaps.reset();
        scalp = raw;
    }
    if (cache) {
        cache->mapsRead = true;
        cache->mapsText = text;
        cache->parsedMaps = std::move(parsedMaps);
        cache->mapsOn = raw;
        cache->scalp = scalp;
        cache->maps = catalog;
    }
    if (maps) *maps = std::move(catalog);
    return scalp;
}

GuidesPtr guidePosesFromNode(const MObject& node, NodeInputCache* cache, std::string* error) {
    MFnDependencyNode fn(node);
    std::vector<SimCurveData> simCurves;
    MPlug simPlug = fn.findPlug("simGuides", true);
    for (unsigned i = 0; i < simPlug.evaluateNumElements(); ++i) {
        MPlug element = simPlug.elementByPhysicalIndex(i);
        const MPlug curvePlug = element.child(0);
        if (!curvePlug.isConnected()) continue;  // stale data once disconnected
        simCurves.push_back({std::strtoull(element.child(1).asString().asChar(), nullptr, 10), curvePlug.asMObject()});
    }
    std::string poseError;
    const std::string guideCache = fn.findPlug("guideCache", true).asString().asChar();
    GuidesPtr poses = toGuidePoses(simCurves, guideCache.empty() ? guideCache : resolveNodePath(node, guideCache),
                                   fn.findPlug("guideCacheObject", true).asString().asChar(),
                                   mayaSeconds(fn.findPlug("time", true).asMTime()), &poseError,
                                   cache ? &cache->guideCache : nullptr);
    if (error) *error = poseError;
    return poses;
}

namespace {

// The guide curves as flat guides rooted on `scalp` (null when there are none), converted again
// only when the curves' data or the scalp changed (with a cache).
GuidesPtr guideCurvesFromNode(const MObject& node, const MeshPtr& scalp, NodeInputCache* cache) {
    MFnDependencyNode fn(node);
    std::vector<GuideCurveData> curves;
    MPlug curvesPlug = fn.findPlug("guideCurves", true);
    const unsigned count = curvesPlug.evaluateNumElements();
    std::uint64_t curvesKey = hash64(0xC0AE5);
    for (unsigned i = 0; i < count; ++i) {
        MPlug element = curvesPlug.elementByPhysicalIndex(i);
        curves.push_back({element.logicalIndex(), element.asMObject()});
        if (cache) curvesKey = curveDataKey(hashCombine(curvesKey, curves.back().id), curves.back().curve);
    }
    GuidesPtr guides;
    if (cache && cache->guidesRead && curvesKey == cache->curvesKey && cache->guidesOn == scalp) {
        guides = cache->guides;
    } else {
        guides = toGuides(curves, scalp.get());
        if (cache) {
            cache->guidesRead = true;
            cache->curvesKey = curvesKey;
            cache->guidesOn = scalp;
            cache->guides = guides;
        }
    }
    return guides && !guides->empty() ? guides : nullptr;
}

// The controls' values; with a cache, from the control attributes it listed.
std::map<std::string, float> controlsFromNode(const MObject& node, NodeInputCache* cache) {
    if (!cache) return readControls(node);
    if (!cache->controlsListed) {
        cache->controlAttributes = controlAttributes(node);
        cache->controlsListed = true;
    }
    return readControls(node, cache->controlAttributes);
}

}  // namespace

// Painted vertex maps are stored per vertex of the scalps joined (scalp_pieces.h): laid on the
// joined mesh and handed back to each scalp as its own color sets (what groom files keep), so a
// reader joining them again finds the maps where they were painted.
void layMapsOnScalps(const MObject& node, NodeInputCache* cache, GroomData& data) {
    const MPlug mapsPlug = MFnDependencyNode(node).findPlug("scalpMaps", true);
    ScalpMaps maps;
    try {
        maps = ScalpMaps::fromJson(mapsPlug.asString().asChar());
    } catch (const std::exception&) {
        return;  // bad scalpMaps text: the shape reports it; here the maps are simply absent
    }
    data.maps = maps.catalog();
    const MeshPtr joined = combineScalps(rawScalpFromNode(node, cache), data.scalps);
    const auto pieces = scalpPieces(withVertexMaps(joined, maps, "", nullptr, nullptr));
    auto own = [&](const std::string& name) -> MeshPtr {
        const auto it = pieces.find(name);
        if (it == pieces.end()) return nullptr;
        auto mesh = std::make_shared<ScalpMesh>(*it->second);
        mesh->setWhole(nullptr, 0);  // a scalp of its own again
        return mesh;
    };
    if (MeshPtr scalp = own(kMainScalpName)) data.scalp = std::move(scalp);
    for (auto& [name, mesh] : data.scalps) {
        if (MeshPtr mapped = own(name)) mesh = std::move(mapped);
    }
}

GroomData groomDataFromNode(const MObject& node, NodeInputCache* cache, std::string* scalpMapsText) {
    GroomData data;
    MFnDependencyNode fn(node);
    data.scalps = extraScalpsFromNode(node, cache);  // the groom's other scalps, joined after it
    if (scalpMapsText) {
        data.scalp = rawScalpFromNode(node, cache);  // the maps go to the session as text
        *scalpMapsText = fn.findPlug("scalpMaps", true).asString().asChar();
    } else if (!data.scalps.empty()) {
        layMapsOnScalps(node, cache, data);
    } else {
        data.scalp = scalpFromNode(node, cache, &data.maps);
    }
    // Guide curves rooted on the scalps joined, as the session binds them.
    data.guides = guideCurvesFromNode(node, combineScalps(data.scalp, data.scalps), cache);
    data.guidePoses = guidePosesFromNode(node, cache);
    data.graphJson = fn.findPlug("graph", true).asString().asChar();
    data.hierarchyJson = fn.findPlug("guideData", true).asString().asChar();
    data.partsJson = fn.findPlug("partData", true).asString().asChar();
    data.correctivesJson = withWeightCurves(node, fn.findPlug("correctiveData", true).asString().asChar());
    data.sculptsJson = fn.findPlug("sculptData", true).asString().asChar();
    data.drivers = driverCurvesFromNode(node);
    data.driverBindJson = fn.findPlug("driverBind", true).asString().asChar();
    data.flowJson = fn.findPlug("flowData", true).asString().asChar();
    if (cache) cache->controlsListed = false;  // a full read lists the control attributes again
    data.controls = controlsFromNode(node, cache);
    data.meshes = colliderMeshesFromNode(node, cache);  // "mesh:<name>" for the graph's ColliderMesh nodes
    data.time = mayaSeconds(fn.findPlug("time", true).asMTime());
    data.followScalp = fn.findPlug("followScalp", true).asBool();
    const ScaleRead scale = readScale(node, cache);  // scaling with the scalp, as the node evaluates
    data.scaleSource = scale.source;
    data.scaleTransformReference = scale.source == ScaleSource::Transform ? scale.transformReference : 0.0;
    data.scaleReference = scale.reference;
    data.scalpFingerprint = parseScalpFingerprint(fn.findPlug("scalpTopology", true).asString().asChar());
    return data;
}

GroomData groomMotionFromNode(const MObject& node, NodeInputCache& cache) {
    GroomData data;
    data.scalp = rawScalpFromNode(node, &cache);
    data.scalps = extraScalpsFromNode(node, &cache);
    data.guides = guideCurvesFromNode(node, combineScalps(data.scalp, data.scalps), &cache);
    data.guidePoses = guidePosesFromNode(node, &cache);
    data.drivers = driverCurvesFromNode(node);
    data.controls = controlsFromNode(node, &cache);
    data.meshes = colliderMeshesFromNode(node, &cache);
    data.time = mayaSeconds(MFnDependencyNode(node).findPlug("time", true).asMTime());
    return data;
}

namespace {

// Which stored input Groom failed to parse (it parses them all in one go): the
// first of guideData, partData, flowData and correctiveData that fails to parse
// again, else a generic label. Only runs on the error path.
std::string labelInputError(const GroomData& data, const std::string& what) {
    try {
        if (data.hierarchyJson.find_first_not_of(" \t\r\n") != std::string::npos) GuideHierarchy::fromJson(data.hierarchyJson);
    } catch (const std::exception&) {
        return "guideData: " + what;
    }
    try {
        PartSet::fromJson(data.partsJson);
    } catch (const std::exception&) {
        return "partData: " + what;
    }
    try {
        FlowSet::fromJson(data.flowJson);
    } catch (const std::exception&) {
        return "flowData: " + what;
    }
    try {
        Correctives::fromJson(data.correctivesJson);
    } catch (const std::exception&) {
        return "correctiveData: " + what;
    }
    try {
        SculptSet::fromJson(data.sculptsJson);
    } catch (const std::exception&) {
        return "sculptData: " + what;
    }
    return "groom: " + what;
}

}  // namespace

NodeEvaluator::NodeEvaluator(const MObject& node, bool cacheIntermediates)
    : node_(node), cacheIntermediates_(cacheIntermediates) {}

NodeEvaluator::~NodeEvaluator() = default;

void NodeEvaluator::setGraphJson(std::string graphJson) {
    if (graphJson == graphJson_) return;
    graphJson_ = std::move(graphJson);
    graphPending_ = session_ != nullptr;  // before the first evaluate the session is made with it
}

void NodeEvaluator::refreshStoredInputs() {
    // What is read from outside the node's stored texts: the scalp's earlier frames (its
    // animation may have been edited) and the guide cache (its file rewritten) are read afresh.
    scalpPast_.clear();
    inputs_.guideCache.reset();
    inputs_.verifyScalp = true;  // the scalp's UVs and color sets may have been edited too
    inputs_.scaleRead = false;   // scaling with the scalp: the node's measure read again
    if (!session_) return;  // the next evaluate reads everything
    MStatus status;
    MFnDependencyNode fn(node_, &status);
    if (!status) return;
    session_->setPathResolver(nodePathResolver(node_));  // the scene may have moved
    inputs_.controlsListed = false;                       // control attributes added or removed
    // The stored texts: each setter does nothing for the same text (what they feed stays cached).
    session_->setHierarchyJson(fn.findPlug("guideData", true).asString().asChar());
    session_->setScalpMapsJson(fn.findPlug("scalpMaps", true).asString().asChar());  // laid on the scalp by the session
    session_->setPartsJson(fn.findPlug("partData", true).asString().asChar());
    session_->setFlowJson(fn.findPlug("flowData", true).asString().asChar());
    session_->setCorrectivesJson(withWeightCurves(node_, fn.findPlug("correctiveData", true).asString().asChar()));
    session_->setSculptsJson(fn.findPlug("sculptData", true).asString().asChar());
    session_->setFollowScalp(fn.findPlug("followScalp", true).asBool());
    const ScaleRead scale = readScale(node_, &inputs_);
    session_->setScaleSource(scale.source);
    session_->setScaleTransformReference(scale.transformReference);
    session_->setScaleTransform(scale.transform);
    session_->setScaleReference(scale.reference);
    session_->setScalpFingerprint(parseScalpFingerprint(fn.findPlug("scalpTopology", true).asString().asChar()));
    const std::string graph = fn.findPlug("graph", true).asString().asChar();
    if (graph != nodeGraphJson_) {
        nodeGraphJson_ = graph;
        if (graphJson_.empty()) graphPending_ = true;  // swapped in: what it shares stays cached
    }
}

StrandsPtr NodeEvaluator::evaluate(float density, std::string* error, MeshPtr* scalp, bool buildCards, bool gameExport) {
    MStatus status;
    MFnDependencyNode fn(node_, &status);
    if (!status) {
        if (error) *error = "not a dependency node";
        return nullptr;
    }
    GroomData data;
    try {
        if (!session_) {
            // What reads the same as last time is not converted again; the raw scalp, the
            // session lays the painted maps on it (as the viewport's does).
            std::string mapsText;
            data = groomDataFromNode(node_, &inputs_, &mapsText);
            nodeGraphJson_ = data.graphJson;
            if (!graphJson_.empty()) data.graphJson = graphJson_;
            session_ = std::make_unique<GroomSession>(data);
            session_->setScalpMapsJson(mapsText);
            session_->setPathResolver(nodePathResolver(node_));
            session_->setCacheIntermediates(cacheIntermediates_);
            graphPending_ = false;
        } else {
            // Another level of detail (or the node's graph edited): swapped into the same session.
            if (graphPending_) {
                session_->setGraphJson(graphJson_.empty() ? nodeGraphJson_ : graphJson_);
                graphPending_ = false;
            }
            // The same groom at another time: only its time-varying inputs are read; the setters
            // do nothing for what reads the same.
            data = groomMotionFromNode(node_, inputs_);
            session_->setScalp(data.scalp);
            session_->setScalps(data.scalps);
            const MPlug mapsPlug = fn.findPlug("scalpMaps", true);
            if (mapsPlug.isDestination()) session_->setScalpMapsJson(mapsPlug.asString().asChar());  // connected: read anyway
            session_->setGuides(data.guides);
            session_->setGuidePoses(data.guidePoses);
            session_->setControls(data.controls);  // animated sliders
            session_->setMeshes(data.meshes);      // moving colliders
            // A stored text edited into something that does not parse: an error, as a fresh read gives
            // (bad painted maps are left out, as a fresh read leaves them).
            std::vector<std::string> bad = session_->inputErrors();
            bad.erase(std::remove(bad.begin(), bad.end(), session_->scalpMapsError()), bad.end());
            if (!bad.empty()) {
                if (error) *error = bad.front();
                return nullptr;
            }
        }
        if (scalp) *scalp = session_->scalp();  // with the painted maps as color sets
        prepareSimulation(node_, *session_, sims_);  // the frame rate, the scalp at other times, the bakes
        const MTime now = fn.findPlug("time", true).asMTime();
        // The scalp's earlier frames, when a Jiggle node reads them (as the viewport does).
        const int frames = session_->scalpHistoryFrames({}, 0);  // (renders and exports: a viewportOnly Jiggle is off)
        session_->setScalpHistory(frames > 0 && data.scalp
                                      ? sampleScalpHistory(node_, now, frames, joinedScalpPoints(data.scalp, data.scalps), scalpPast_,
                                                           false, session_->scalpHistoryLoopFrames(now.as(MTime::uiUnit()), {}, 0),
                                                           data.scalps)
                                      : nullptr);
        return session_->evaluate(density, mayaSeconds(now), buildCards, gameExport);
    } catch (const GraphError& e) {
        if (error) *error = e.what();
    } catch (const std::exception& e) {
        if (error) *error = labelInputError(data, e.what());
    }
    return nullptr;
}

HierarchyPtr NodeEvaluator::hierarchy() { return session_ ? session_->hierarchy() : nullptr; }

bool NodeEvaluator::hairLook(HairLook* look) const {
    if (!session_) return false;
    *look = session_->hairLook();
    return true;
}

StrandsPtr evaluateGroomNode(const MObject& node, float density, std::string* error, MeshPtr* scalp, bool buildCards,
                             const std::string* graphJson, bool gameExport) {
    NodeEvaluator evaluator(node, false);
    if (graphJson) evaluator.setGraphJson(*graphJson);
    return evaluator.evaluate(density, error, scalp, buildCards, gameExport);
}

bool prototypePath(const std::string& name, MDagPath* path) {
    MSelectionList sel;
    if (name.empty() || sel.add(name.c_str()) != MS::kSuccess || sel.getDagPath(0, *path) != MS::kSuccess) return false;
    if (path->hasFn(MFn::kMesh)) return true;
    return path->extendToShape() == MS::kSuccess && path->hasFn(MFn::kMesh);
}

void readInstancePrototype(const std::string& name, InstancePrototype& out) {
    out.points.clear();
    out.normals.clear();
    out.triangles.clear();
    out.uvs.clear();
    out.found = false;
    MDagPath path;
    if (!prototypePath(name, &path)) return;
    out.found = true;
    MFnMesh mesh(path);
    MFloatPointArray points;
    mesh.getPoints(points, MSpace::kObject);
    const bool uvs = mesh.numUVs() > 0;
    out.points.reserve(static_cast<std::size_t>(mesh.numFaceVertices()));
    out.normals.reserve(out.points.capacity());
    MIntArray verts, triVerts;
    MPointArray triPoints;
    for (MItMeshPolygon poly(path); !poly.isDone(); poly.next()) {
        poly.getVertices(verts);
        const auto base = static_cast<unsigned>(out.points.size());
        for (unsigned k = 0; k < verts.length(); ++k) {
            const MFloatPoint& q = points[static_cast<unsigned>(verts[k])];
            MVector n;
            poly.getNormal(k, n, MSpace::kObject);
            out.points.push_back({q.x, q.y, q.z});
            out.normals.push_back({static_cast<float>(n.x), static_cast<float>(n.y), static_cast<float>(n.z)});
            float2 uv = {0.0f, 0.0f};
            if (uvs && poly.getUV(static_cast<int>(k), uv) != MS::kSuccess) uv[0] = uv[1] = 0.0f;
            if (uvs) out.uvs.push_back({uv[0], uv[1]});
        }
        int triangleCount = 0;
        poly.numTriangles(triangleCount);
        for (int t = 0; t < triangleCount; ++t) {
            if (poly.getTriangle(t, triPoints, triVerts, MSpace::kObject) != MS::kSuccess || triVerts.length() != 3) continue;
            unsigned corners[3];
            bool known = true;
            for (unsigned c = 0; c < 3; ++c) {
                unsigned local = 0;
                while (local < verts.length() && verts[local] != triVerts[c]) ++local;
                known = known && local < verts.length();
                corners[c] = base + local;
            }
            if (!known) continue;
            out.triangles.insert(out.triangles.end(), corners, corners + 3);
        }
    }
}

}  // namespace groomlab::maya
