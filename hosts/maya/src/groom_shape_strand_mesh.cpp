// SPDX-License-Identifier: Apache-2.0
// The groom's outMesh: the ToMesh nodes' strands as one live polygon mesh (tubes or
// ribbons, strand_geometry.h). While the strands stay the same ones with the same point counts
// (strandMeshTopologyKey), a new frame copies the last mesh and sets its points alone: the
// topology and UVs are made once, and Alembic and Maya's caches see a deforming mesh.

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include <maya/MFloatArray.h>
#include <maya/MFloatPointArray.h>
#include <maya/MFnMesh.h>
#include <maya/MFnMeshData.h>
#include <maya/MIntArray.h>

#include "groom_shape.h"
#include "groomlab/parallel.h"
#include "groomlab/random.h"
#include "groomlab/strand_geometry.h"
#include "maya_inputs.h"

namespace groomlab::maya {

namespace {

// A new mesh data object holding a copy of `mesh` with `points`: the topology and UVs as they were.
MObject withPoints(const MObject& mesh, MFloatPointArray& points) {
    MFnMeshData data;
    MObject out = data.create();
    MFnMesh fn;
    MStatus status;
    MObject made = fn.copy(mesh, out, &status);
    if (!status || made.isNull()) return MObject::kNullObj;
    MFnMesh copy(made);
    if (copy.setPoints(points) != MS::kSuccess) return MObject::kNullObj;
    return out;
}

MFloatPointArray toMaya(const std::vector<Vec3f>& points) {
    MFloatPointArray out(static_cast<unsigned>(points.size()));
    parallelFor(0, points.size(), [&](std::size_t i) {
        const Vec3f& p = points[i];
        out[static_cast<unsigned>(i)] = MFloatPoint(p.x, p.y, p.z);
    }, 4096);
    return out;
}

}  // namespace

MStatus GroomShape::computeStrandMesh(MDataBlock& data, Lane& lane) {
    // Not connected: nothing reads the mesh (the Evaluation Manager computes every output it dirtied).
    const std::shared_ptr<const NodeLinks> taken = lane.live ? nullptr : links();
    if (!(taken ? taken->strandMeshConnected : MPlug(thisMObject(), aOutMesh).isConnected())) {
        if (lane.unusedStrandMeshData.isNull()) lane.unusedStrandMeshData = MFnMeshData().create();
        data.outputValue(aOutMesh).set(lane.unusedStrandMeshData);
        data.setClean(aOutMesh);
        return MS::kSuccess;
    }
    data.inputValue(aOutStrandCount);  // the evaluation that hands the session its inputs (scalp, guides, sims...)
    if (lane.live) finishCommit(false, &data, "outMesh");  // (the session is the worker's while a commit evaluates)
    std::vector<GroomSession::StrandMeshSource> sources;
    try {
        sources = lane.session.evaluateStrandMeshes(mayaSeconds(data.inputValue(aTime).asTime()));
    } catch (const std::exception& e) {
        if (lane.live) report(std::string("strand mesh: ") + e.what(), true);
        sources.clear();
    }
    std::vector<StrandsPtr> from;
    std::vector<float> widths;
    for (const auto& s : sources) {
        from.push_back(s.strands);
        widths.push_back(s.options.widthScale);
    }
    // The same strands as last time (a cache hit: another input dirtied the plug): the same mesh.
    if (lane.strandMeshMade && from == lane.strandMeshFrom && widths == lane.strandMeshWidths) {
        data.outputValue(aOutMesh).set(lane.strandMeshData);
        data.setClean(aOutMesh);
        return MS::kSuccess;
    }
    const MeshPtr scalp = lane.session.scalp();  // the strands' roots are on it (normals: the ribbons' plane)
    // The instances' prototypes (scene meshes), read once each on this evaluation.
    std::map<std::string, InstancePrototype> scene;
    const PrototypeLookup prototypes = [&scene](const std::string& name) -> const InstancePrototype* {
        auto it = scene.find(name);
        if (it == scene.end()) {
            it = scene.emplace(name, InstancePrototype{}).first;
            readInstancePrototype(name, it->second);
        }
        return it->second.found ? &it->second : nullptr;
    };
    std::uint64_t key = hash64(0x5D3E5u);
    std::size_t pointCount = 0;
    for (const auto& s : sources) {
        key = hashCombine(key, hashString(hash64(0x4E0Du), s.node));
        if (!s.strands) continue;
        key = hashCombine(key, strandMeshTopologyKey(*s.strands, s.options));
        pointCount += strandMeshPointCount(*s.strands, s.options, prototypes);
    }

    MObject meshObject;
    if (lane.strandMeshMade && !lane.strandMeshData.isNull() && key == lane.strandMeshKey &&
        pointCount == lane.strandMeshPoints && pointCount > 0) {
        // Only the points moved: each node's points (in parallel) into a copy of the last mesh.
        std::vector<Vec3f> points(pointCount);
        std::size_t at = 0;
        for (const auto& s : sources) {
            if (!s.strands) continue;
            strandMeshPoints(*s.strands, scalp.get(), s.options, points.data() + at, prototypes);
            at += strandMeshPointCount(*s.strands, s.options, prototypes);
        }
        MFloatPointArray moved = toMaya(points);
        meshObject = withPoints(lane.strandMeshData, moved);
    }
    if (meshObject.isNull()) {
        // The topology anew: each node's mesh, joined (UV set map1; strandUVs where a node asks).
        RachisMesh joined;
        std::vector<float> u2, v2;
        bool strandUVs = false;
        for (const auto& s : sources) {
            if (!s.strands) continue;
            StrandMesh m;
            try {
                m = buildStrandMesh(*s.strands, scalp.get(), s.options, prototypes);
            } catch (const std::exception& e) {
                if (lane.live) report(std::string("strand mesh: ") + e.what(), true);
                continue;
            }
            const int pointBase = static_cast<int>(joined.points.size()), uvBase = static_cast<int>(joined.u.size());
            joined.points.insert(joined.points.end(), m.mesh.points.begin(), m.mesh.points.end());
            joined.faceCounts.insert(joined.faceCounts.end(), m.mesh.faceCounts.begin(), m.mesh.faceCounts.end());
            for (const int v : m.mesh.faceVertices) joined.faceVertices.push_back(v + pointBase);
            for (const int v : m.mesh.faceUVs) joined.faceUVs.push_back(v + uvBase);
            joined.u.insert(joined.u.end(), m.mesh.u.begin(), m.mesh.u.end());
            joined.v.insert(joined.v.end(), m.mesh.v.begin(), m.mesh.v.end());
            strandUVs = strandUVs || !m.u2.empty();
            if (m.u2.empty()) {
                u2.resize(joined.u.size(), 0.0f);
                v2.resize(joined.v.size(), 0.0f);
            } else {
                u2.insert(u2.end(), m.u2.begin(), m.u2.end());
                v2.insert(v2.end(), m.v2.begin(), m.v2.end());
            }
        }
        MFnMeshData meshData;
        meshObject = meshData.create();
        if (!joined.faceCounts.empty()) {
            const MIntArray counts(joined.faceCounts.data(), static_cast<unsigned>(joined.faceCounts.size()));
            const MIntArray connects(joined.faceVertices.data(), static_cast<unsigned>(joined.faceVertices.size()));
            const MIntArray uvIds(joined.faceUVs.data(), static_cast<unsigned>(joined.faceUVs.size()));
            const MFloatArray us(joined.u.data(), static_cast<unsigned>(joined.u.size()));
            const MFloatArray vs(joined.v.data(), static_cast<unsigned>(joined.v.size()));
            MFnMesh mesh;
            MStatus status;
            mesh.create(static_cast<int>(joined.points.size()), static_cast<int>(counts.length()), toMaya(joined.points), counts,
                        connects, us, vs, meshObject, &status);
            if (status) {
                mesh.assignUVs(counts, uvIds);
                if (strandUVs) {
                    MString set("strandUVs");
                    mesh.createUVSetDataMesh(set);
                    mesh.setUVs(MFloatArray(u2.data(), static_cast<unsigned>(u2.size())),
                                MFloatArray(v2.data(), static_cast<unsigned>(v2.size())), &set);
                    mesh.assignUVs(counts, uvIds, &set);
                }
            }
        }
        lane.strandMeshKey = key;
        lane.strandMeshPoints = static_cast<unsigned>(joined.points.size());
    }
    lane.strandMeshData = meshObject;
    lane.strandMeshFrom = std::move(from);
    lane.strandMeshWidths = std::move(widths);
    lane.strandMeshMade = true;
    data.outputValue(aOutMesh).set(meshObject);
    data.setClean(aOutMesh);
    return MS::kSuccess;
}

}  // namespace groomlab::maya
