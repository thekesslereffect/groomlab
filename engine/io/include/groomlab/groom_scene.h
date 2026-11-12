// SPDX-License-Identifier: Apache-2.0
#pragma once

// A groom file ready to evaluate at any time: the scalp and guide caches it
// references are opened once, and its GroomSession (the graph's cache) carries
// across frames. The host of the CLI and the Arnold procedural: it feeds the
// session the cached scalp, its history and the guide poses of each time.

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "groomlab/groom.h"
#include "groomlab/groom_file.h"
#include "groomlab/scalp_history.h"

namespace groomlab {

class AlembicMeshReader;
class AlembicCurvesReader;

class GroomScene {
public:
    // Throws GroomFileError, AlembicError (caches) or GraphError.
    explicit GroomScene(const std::string& groomPath);
    // The groom named `groomName` of a file of several (groom_package.h groomIndex: "" the first);
    // throws GroomFileError when the file holds none of that name.
    GroomScene(const std::string& groomPath, const std::string& groomName);
    explicit GroomScene(GroomFile file);
    ~GroomScene();

    const GroomFile& file() const { return file_; }
    double framesPerSecond() const { return file_.framesPerSecond; }

    // Off for a one-shot evaluation (a render): the graph keeps no intermediate
    // results, so peak memory is lower. Leave on when evaluating frame after
    // frame, where the cache spares the unchanged nodes. See GroomSession.
    void setCacheIntermediates(bool enabled) { session_->setCacheIntermediates(enabled); }

    // The scalp at a time: the cache's positions on the file's topology,
    // UVs, color sets and rest pose; the file's scalp without a cache. The
    // same mesh comes back for the same time, and for every time when the
    // cache is not animated.
    MeshPtr scalpAt(double seconds);
    // The same joined with the groom's other scalps (GroomData::scalps, scalp_pieces.h): what the
    // strands' roots index, for what reads the scalp at them (root UVs in exports, scales, LOD).
    // scalpAt itself for a groom of one scalp.
    MeshPtr rootScalpAt(double seconds);
    // The scalp's render subdivision (ScalpMesh::setRenderSubdivision: what a Scalp node set to
    // match render refines it by; a groom file's scalp "subdivision"), a host's own setting of it
    // (the standalone app's Scalp Subdivision): the file's scalp takes it, and every time's scalp
    // after it (the cache's frames are read on it again). The same level and source: nothing.
    void setScalpRenderSubdivision(int levels, const std::string& from);

    // The scalp's points at the `frames` frames before `seconds` (1 / framesPerSecond apart), for
    // the graph's Jiggle nodes; null without an animated scalp cache (nothing moves). Frames read
    // once are kept, so evaluating frame after frame reads one new frame each time. Then the
    // scalp at `loopFrames` (frames: a looping Jiggle's, GroomSession::scalpHistoryLoopFrames).
    ScalpHistoryPtr scalpHistoryAt(double seconds, int frames, const std::vector<double>& loopFrames = {});

    // Strands at a time in seconds (frame / framesPerSecond). Throws GraphError or AlembicError.
    // The scalp's history is sampled when the graph has Jiggle nodes.
    StrandsPtr evaluate(double seconds, float density = 1.0f);
    // The same inputs at `request.time` (the cached scalp, its history, the guide poses), then
    // the session's view of them (point budget, a viewed node, cards): what a viewport host asks
    // for (the standalone app). Never throws for the evaluation (ViewResult::errors); AlembicError
    // for a cache that cannot be read.
    const ViewResult& view(const ViewRequest& request);

    // Whether another time can give other strands: the session's readsTime (Wind, ImportCurves,
    // Jiggle, fading correctives), an animated scalp cache, a guide cache or a driver cache. When it is false a
    // host evaluates once and keeps the strands for every frame and motion key (the CLI, the
    // procedural).
    bool readsTime() const { return (scalpCache_ && scalpAnimated_) || guideCache_ || driverCache_ || session_->readsTime(); }

    // The span of the caches' samples (the scalp cache's and the guide cache's together): what
    // a timeline should cover. False when no cache animates (`start` and `end` left alone).
    bool timeRange(double* startSeconds, double* endSeconds) const;

    // The inputs of a time into the session without evaluating (the cached scalp, its history,
    // the guide poses): what `evaluate` and `view` do first. A host evaluating the session its
    // own way at that time (a full-density export frame, a card build) calls it first.
    void setTime(double seconds);

    // Each graph node's note from the last evaluation (a Jiggle without scalp motion), by name.
    std::vector<std::pair<std::string, std::string>> nodeNotes() const { return session_->nodeNotes(); }

    // Milliseconds per graph node in the last evaluation, by name.
    std::vector<std::pair<std::string, double>> nodeTimes() const { return session_->nodeTimes(); }

    // The session the groom evaluates in: stats, versions, the hierarchy of the last time.
    GroomSession& session() { return *session_; }
    const GroomSession& session() const { return *session_; }

private:
    GroomFile file_;
    std::unique_ptr<AlembicMeshReader> scalpCache_;
    std::unique_ptr<AlembicCurvesReader> guideCache_;
    std::unique_ptr<AlembicCurvesReader> driverCache_;  // the drivers' animation (DriveGuides)
    std::unique_ptr<GroomSession> session_;
    bool scalpAnimated_ = false;
    MeshPtr lastScalp_;  // the scalp last read from the cache, at lastScalpSeconds_
    MeshPtr joinedFrom_, joined_;  // rootScalpAt's last: the scalp it joined, and the result
    double lastScalpSeconds_ = 0.0;
    ScalpHistorySampler past_;  // the scalp cache's points by time (microseconds), for the history
};

}  // namespace groomlab
