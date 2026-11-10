// SPDX-License-Identifier: Apache-2.0
#include "groomlab/groom_scene.h"

#include <algorithm>
#include <cmath>

#include "groomlab/alembic_io.h"
#include "groomlab/frame_time.h"
#include "groomlab/groom_package.h"
#include "groomlab/sim_cache.h"

namespace groomlab {

namespace {

// The time's inputs into the session: the cached scalp, its history for Jiggle, the guide
// cache's poses (evaluate and view share it, so a view sees what a render sees; a simulation
// steps over other frames with the same).
void feedTime(GroomSession& session, GroomScene& scene, const AlembicCurvesReader* guideCache, double seconds,
              const std::string& viewNode, bool viewport = false) {
    // (A Jiggle off where this evaluates, by its evaluation mode, reads no history.)
    const int where = viewport ? 1 : 0;
    session.setScalp(scene.scalpAt(seconds));
    session.setScalpHistory(scene.scalpHistoryAt(seconds, session.scalpHistoryFrames(viewNode, where),
                                                 session.scalpHistoryLoopFrames(secondsToFrame(seconds, session.framesPerSecond()),
                                                                                viewNode, where)));
    if (guideCache) {
        const StrandSet curves = guideCache->at(seconds);
        auto poses = std::make_shared<GuideSet>();
        for (std::size_t s = 0; s < curves.strandCount(); ++s) {
            const Vec3f* p = curves.strandPoints(s);
            poses->add(curves.ids()[s], std::vector<Vec3f>(p, p + curves.strandPointCount(s)));
        }
        session.setGuidePoses(poses);
    }
}

}  // namespace

GroomScene::GroomScene(const std::string& groomPath) : GroomScene(readGroomFile(groomPath)) {}

namespace {

GroomFile namedGroom(const std::string& path, const std::string& name) {
    std::vector<GroomFile> grooms = readGroomFiles(path);
    const int index = groomIndex(grooms, name);
    if (index < 0) throw GroomFileError(path + " holds no groom named '" + name + "'");
    return std::move(grooms[static_cast<std::size_t>(index)]);
}

}  // namespace

GroomScene::GroomScene(const std::string& groomPath, const std::string& groomName) : GroomScene(namedGroom(groomPath, groomName)) {}

GroomScene::GroomScene(GroomFile file) : file_(std::move(file)) {
    if (!file_.scalpCache.empty()) {
        if (!file_.data.scalp) throw GroomFileError("the groom file has a scalp cache but no scalp");
        scalpCache_ = std::make_unique<AlembicMeshReader>(file_.scalpCache.file, file_.scalpCache.object);
        scalpAnimated_ = scalpCache_->animated();
    }
    // The guide cache stays open: it is read at every frame and motion key.
    if (!file_.guideCache.empty()) {
        guideCache_ = std::make_unique<AlembicCurvesReader>(file_.guideCache.file, file_.guideCache.object);
    }
    session_ = std::make_unique<GroomSession>(file_.data);
    // Relative paths in the graph: the groom file's folder, then its search paths.
    std::vector<std::string> dirs;
    if (!file_.directory.empty()) dirs.push_back(file_.directory);
    dirs.insert(dirs.end(), file_.searchPaths.begin(), file_.searchPaths.end());
    session_->setPathResolver(searchPathResolver(std::move(dirs)));
    // Simulate nodes: frames at the file's rate, the frames on the way to a time fed as a view
    // feeds its own, the bakes the file names.
    session_->setFramesPerSecond(file_.framesPerSecond);
    session_->setSimulationFeed([this](double seconds) {
        feedTime(*session_, *this, guideCache_.get(), seconds, {});
        return true;
    });
    for (const auto& [node, ref] : file_.simCaches) {
        if (!ref.file.empty()) session_->setBakedSimulation(node, readSimulationCache(ref.file, ref.object, ref.stamp));
    }
}

GroomScene::~GroomScene() = default;

MeshPtr GroomScene::scalpAt(double seconds) {
    if (!scalpCache_) return file_.data.scalp;
    // A still cache is read once; an animated one once per time (evaluate()
    // and the caller asking for the scalp of the same frame share it).
    if (lastScalp_ && (!scalpAnimated_ || lastScalpSeconds_ == seconds)) return lastScalp_;
    std::vector<Vec3f> points = scalpCache_->pointsAt(seconds);
    if (points.size() != file_.data.scalp->pointCount()) {
        throw AlembicError("scalp cache '" + file_.scalpCache.file + "' has " + std::to_string(points.size()) +
                           " points; the groom's scalp has " + std::to_string(file_.data.scalp->pointCount()));
    }
    lastScalp_ = std::make_shared<ScalpMesh>(file_.data.scalp->withPoints(std::move(points)));
    lastScalpSeconds_ = seconds;
    return lastScalp_;
}

void GroomScene::setScalpRenderSubdivision(int levels, const std::string& from) {
    const MeshPtr& scalp = file_.data.scalp;
    levels = std::max(0, levels);
    const std::string source = levels > 0 ? from : std::string();
    if (!scalp || (scalp->renderSubdivision() == levels && scalp->renderSubdivisionFrom() == source)) return;
    auto copy = std::make_shared<ScalpMesh>(*scalp);
    copy->setRenderSubdivision(levels, source);
    file_.data.scalp = std::move(copy);
    lastScalp_.reset();  // the cache's frames on the new scalp
    joinedFrom_.reset();
    joined_.reset();
}

MeshPtr GroomScene::rootScalpAt(double seconds) {
    const MeshPtr scalp = scalpAt(seconds);
    if (file_.data.scalps.empty()) return scalp;
    if (scalp != joinedFrom_ || !joined_) {
        joined_ = combineScalps(scalp, file_.data.scalps);
        joinedFrom_ = scalp;
    }
    return joined_;
}

ScalpHistoryPtr GroomScene::scalpHistoryAt(double seconds, int frames, const std::vector<double>& loopFrames) {
    if (!scalpCache_ || !scalpAnimated_ || frames <= 0) return nullptr;
    const double step = frameToSeconds(1.0, file_.framesPerSecond);
    auto key = [](double t) { return static_cast<std::int64_t>(std::llround(t * 1e6)); };
    std::vector<std::int64_t> keys;
    std::vector<double> times;
    for (int k = 1; k <= frames; ++k) times.push_back(seconds - k * step);
    // Then a looping Jiggle's frames from the end of its range.
    std::vector<double> loopSeconds;
    for (const double f : loopFrames) loopSeconds.push_back(frameToSeconds(f, file_.framesPerSecond));
    times.insert(times.end(), loopSeconds.begin(), loopSeconds.end());
    for (const double t : times) keys.push_back(key(t));
    // The cache's frames read once each (the core's sampler, as Maya's history); a frame with
    // another point count is an error in the cache.
    return past_.history(key(seconds), keys, step, scalpAt(seconds)->points(), [&](std::size_t k) {
        std::vector<Vec3f> points = scalpCache_->pointsAt(times[k]);
        if (points.size() != file_.data.scalp->pointCount()) {
            throw AlembicError("scalp cache '" + file_.scalpCache.file + "' has " + std::to_string(points.size()) +
                               " points; the groom's scalp has " + std::to_string(file_.data.scalp->pointCount()));
        }
        return points;
    }, false, loopSeconds);
}

bool GroomScene::timeRange(double* startSeconds, double* endSeconds) const {
    double lo = 0.0, hi = 0.0;
    bool any = false;
    auto widen = [&](double a, double b) {
        lo = any ? std::min(lo, a) : a;
        hi = any ? std::max(hi, b) : b;
        any = true;
    };
    double a = 0.0, b = 0.0;
    if (scalpCache_ && scalpAnimated_ && scalpCache_->timeRange(&a, &b)) widen(a, b);
    if (guideCache_ && guideCache_->timeRange(&a, &b)) widen(a, b);
    if (!any) return false;
    if (startSeconds) *startSeconds = lo;
    if (endSeconds) *endSeconds = hi;
    return true;
}

void GroomScene::setTime(double seconds) { feedTime(*session_, *this, guideCache_.get(), seconds, {}); }

StrandsPtr GroomScene::evaluate(double seconds, float density) {
    feedTime(*session_, *this, guideCache_.get(), seconds, {});
    return session_->evaluate(density, seconds);
}

const ViewResult& GroomScene::view(const ViewRequest& request) {
    feedTime(*session_, *this, guideCache_.get(), request.time, request.viewNode, request.viewport);
    return session_->evaluate(request);
}

}  // namespace groomlab
