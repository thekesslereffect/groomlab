// SPDX-License-Identifier: Apache-2.0
#pragma once

// One groom the host keeps in the helper (docs/BLENDER.md): its GroomSession, what its file
// carries besides (caches, bakes), the host's space, its shared memory arenas,
// its exports and its interactive tools.

#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "groomlab/groom_file.h"
#include "groomlab/groom_session.h"
#include "groomlab/scales.h"
#include "groomlab/usd_writer.h"
#include "groomlab/vrscene_writer.h"

#if GROOMLAB_HAS_ALEMBIC
#include "groomlab/alembic_io.h"
#include "groomlab/engine_export.h"
#endif

#include "host_space.h"
#include "shared_memory.h"

namespace groomlab::helper {

struct ToolState;  // tools.cpp

struct ExportJob {
    std::string format, path;
    double fps = 24.0;
#if GROOMLAB_HAS_ALEMBIC
    std::unique_ptr<AlembicCurvesWriter> abc;
    // unrealGroom, unityStrands (engine_export.h): the strands as each engine's importer reads them.
    std::unique_ptr<EngineGroomWriter> engine;
#endif
    bool unreal = false;  // the engine writer's: Unreal (the primary guides go too) or Unity
    // format "guides" (Export Guides for Simulation): these guides' curves ("*": every guide the
    // Guides node feeds, else a group's), strandId = guide ID.
    std::string guideGroup;
    std::unique_ptr<UsdCurvesWriter> usd;
    std::unique_ptr<VrsceneWriter> vrscene;  // a V-Ray scene (vrscene_writer.h)
    bool withScales = true;  // an Alembic export writes the scales' mesh (scales "mesh")
    ScaleMeshBuilder scales;
    std::size_t frames = 0;
    double lastFrame = -std::numeric_limits<double>::infinity();
};

// Simulate Range in steps (pipeline.cpp: simulate_range, simulate_poll): each request steps the
// Simulate nodes on for at most `budgetMs`, so the protocol keeps answering between steps.
struct SimJob {
    int target = 1;           // the frame every node is stepped on to
    int from = 1;             // the earliest frame a node had left to step when the job began
    double budgetMs = 200.0;  // per request
    std::size_t frames = 0;   // frames stepped so far (every node's)
    double milliseconds = 0.0;
    std::vector<std::string> errors;
};

struct Session {
    explicit Session(int id);  // tools.cpp (with the tools' state)

    int id;
    HostSpace space;
    GroomFile file;  // what the groom carries besides the session's data (file.data unused)
    std::unique_ptr<GroomSession> groom = std::make_unique<GroomSession>();
    double seconds = 0.0;
    Arena strands, guides, scalp;
    StrandsPtr last;  // the last evaluation's strands (sameTopology)
    std::size_t evaluations = 0, incrementals = 0;
    std::vector<std::string> notes;  // what the last load said (a cache that did not open)
#if GROOMLAB_HAS_ALEMBIC
    std::unique_ptr<AlembicCurvesReader> guideCache;
#endif
    std::map<int, std::unique_ptr<ExportJob>> exports;
    int nextExport = 1;
    // Flat guides the host keeps on the scalp's rest pose (set_guides "pose": "rest": Blender's
    // guide curves do not deform with the scalp). The session is handed them carried to the scalp
    // as it is now by their roots' motion (update_scalp), themselves at the rest pose. Null: the
    // host gives its guides as they are at the evaluated time (Maya's curves).
    GuidesPtr restGuides;
    MeshPtr restMesh;  // the scalp's rest pose the rest guides are rooted on
    std::uint64_t restMeshKey = 0;
    // The interactive tools (tools.cpp): the brush, the selection, the undo stack, the display
    // buffers. Made on first use for the session's groom; dropped when the groom is replaced.
    std::unique_ptr<ToolState> tools;
    // The host's scalp at other frames (set_scalp_frames: the add-on samples a deforming scalp over
    // a range), what Simulate steps through to a frame: frame scalpFramesFirst + i holds
    // scalpFrames[i] (points, and rest points when given), the scalp's topology.
    int scalpFramesFirst = 1;
    std::vector<std::vector<Vec3f>> scalpFrames, scalpFramesRest;
    std::unique_ptr<SimJob> simJob;
    Arena meshes{"meshes"};  // meshes answered to the host (cards, scales, strands as a mesh, rachis)
    // Where new texture maps' images go ("" a folder in the temp folder, made when needed).
    std::string mapsDir;
    // The painted maps: owned by the session as its scalp maps (a file's, or made here) once
    // `mapsOwned`; the file's catalog entries for guide layers kept for the save.
    bool mapsOwned = false;
    MapCatalog fileCatalog;

    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;
    ~Session();
};

}  // namespace groomlab::helper
