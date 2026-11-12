// SPDX-License-Identifier: Apache-2.0
#include "cli_export.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <memory>
#include <stdexcept>

#include "groomlab/alembic_io.h"
#include "groomlab/frame_time.h"
#include "groomlab/groom_scene.h"
#include "groomlab/groom_session.h"
#include "groomlab/scales.h"
#include "groomlab/usd_writer.h"
#include "groomlab/vrscene_writer.h"

namespace groomlab::cli {

namespace {

bool endsWith(const std::string& s, const std::string& suffix) {
    return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

// The groom file's own prototype meshes (GroomFile::prototypes) by name.
PrototypeLookup storedPrototypes(const GroomScene& scene) {
    return [&scene](const std::string& name) -> const InstancePrototype* {
        const auto it = scene.file().prototypes.find(name);
        return it != scene.file().prototypes.end() && it->second.found ? &it->second : nullptr;
    };
}

}  // namespace

bool isUsdPath(const std::string& path) { return endsWith(path, ".usda") || endsWith(path, ".usd"); }

bool isVrscenePath(const std::string& path) { return endsWith(path, ".vrscene"); }

ExportReport exportStrands(GroomScene& scene, const ExportOptions& a, std::ostream* log) {
    if (a.out.empty()) throw std::invalid_argument("no output file");
    if (!(a.step > 0.0)) throw std::invalid_argument("the frame step must be positive");
    if (a.end < a.start) throw std::invalid_argument("the end frame is before the start frame");
    if (!(a.density >= 0.0f && a.density <= 1.0f)) throw std::invalid_argument("the density must be between 0 and 1");
    const bool usd = isUsdPath(a.out);
    const bool vrscene = isVrscenePath(a.out);
    if (!usd && !vrscene && !endsWith(a.out, ".abc")) {
        throw std::invalid_argument("the output must be .abc, .usda, .usd or .vrscene");
    }
    if (a.scales != "mesh" && a.scales != "instancer" && a.scales != "both" && a.scales != "none") {
        throw std::invalid_argument("scales must be mesh, instancer, both or none");
    }
    if (!usd && (a.scales == "instancer" || a.scales == "both")) throw std::invalid_argument("an instancer goes to USD");

    const auto t0 = std::chrono::steady_clock::now();
    const double fps = scene.framesPerSecond();
    const auto frameCount = static_cast<std::size_t>(std::floor((a.end - a.start) / a.step + 1e-6)) + 1;

    std::unique_ptr<AlembicCurvesWriter> abc;
    std::unique_ptr<UsdCurvesWriter> usdWriter;
    std::unique_ptr<VrsceneWriter> vrsceneWriter;
    const std::string name = "groom";
    if (vrscene) {
        VrsceneWriter::Options options;
        options.framesPerSecond = fps;
        options.comment = scene.file().source;
        options.upAxis = scene.file().upAxis == "Z" ? "Z" : "Y";
        options.metersPerUnit = usdMetersPerUnit(scene.file().units);
        vrsceneWriter = std::make_unique<VrsceneWriter>(a.out, name, options);
    } else if (usd) {
        UsdCurvesWriter::Options options;
        options.framesPerSecond = fps;
        options.comment = scene.file().source;
        // The file's own up axis and unit (a Z-up groom stays Z-up; meters stay meters).
        options.upAxis = scene.file().upAxis == "Z" ? "Z" : "Y";
        options.metersPerUnit = usdMetersPerUnit(scene.file().units);
        using S = UsdCurvesWriter::Options::Scales;
        options.scales = a.scales == "instancer" ? S::Instancer : a.scales == "both" ? S::Both : a.scales == "none" ? S::None : S::Mesh;
        options.prototypes = storedPrototypes(scene);
        usdWriter = std::make_unique<UsdCurvesWriter>(a.out, name, options);
    } else {
        abc = std::make_unique<AlembicCurvesWriter>(a.out, name, frameToSeconds(a.start, fps), frameToSeconds(a.step, fps));
    }

    ExportReport report;
    // A groom that nothing moves (GroomScene::readsTime) is evaluated once, for every frame.
    const bool moves = scene.readsTime();
    StrandsPtr still;
    ScaleMeshBuilder scales;  // bends only the scales that moved since the last frame
    for (std::size_t i = 0; i < frameCount; ++i) {
        const double frame = a.start + static_cast<double>(i) * a.step;
        const double seconds = frameToSeconds(frame, fps);  // every host's rule (frame_time.h)
        const StrandsPtr strands = moves || i == 0 ? scene.evaluate(seconds, a.density) : still;
        // Velocities (Alembic): the frames a step before and after, a central difference by strand
        // ID (a groom nothing moves is still). The neighbours are evaluated after, so this frame's
        // own evaluation is the one written.
        std::vector<Vec3f> velocities;
        if (abc && a.velocities && strands) {
            const double dt = frameToSeconds(a.step, fps);
            if (moves) {
                const StrandsPtr before = scene.evaluate(seconds - dt, a.density);
                const StrandsPtr after = scene.evaluate(seconds + dt, a.density);
                velocities = strandVelocities(*strands, before.get(), after.get(), dt);
                scene.setTime(seconds);  // the scalp and inputs of this frame again, for its scales
            } else {
                velocities.assign(strands->pointCount(), Vec3f{0.0f, 0.0f, 0.0f});
            }
        }
        if (i == 0) still = strands;
        const StrandSet empty;
        const StrandSet& s = strands ? *strands : empty;
        const MeshPtr scalp = scene.rootScalpAt(seconds);
        const bool scaled = !vrscene && a.scales != "none" && s.instances() && s.instances()->hasMeshes();
        const auto scaleMesh = scaled ? scales.build(s, scalp, storedPrototypes(scene)) : nullptr;
        if (vrsceneWriter) {
            if (i == 0) vrsceneWriter->setLobes(hairLobes(scene.session().hairLook()));  // HairColor's highlight
            vrsceneWriter->writeFrame(frame, s, scalp.get());
        } else if (usdWriter) {
            usdWriter->writeFrame(frame, s, scalp.get(), scaleMesh.get());
        } else {
            abc->writeFrame(s, scalp.get(), scaleMesh.get(), velocities.empty() ? nullptr : &velocities);  // root UVs, as Maya's
        }
        report.strandTotal += s.strandCount();
        if (log) *log << "frame " << frame << ": " << s.strandCount() << " strands\n";
        if (i == 0 && log) {  // what the nodes have to say (a Jiggle without scalp motion), once
            for (const auto& [node, note] : scene.nodeNotes()) *log << "  note: " << node << ": " << note << "\n";
            if (a.stats) *log << "stats: " << scene.session().stats().toJson() << "\n";
        }
        if (a.timings && log) {
            auto times = scene.nodeTimes();
            std::sort(times.begin(), times.end(), [](const auto& x, const auto& y) { return x.second > y.second; });
            for (const auto& [node, ms] : times) *log << "  " << node << ": " << ms << " ms\n";
        }
    }
    if (vrsceneWriter) vrsceneWriter->close();
    if (usdWriter) usdWriter->close();
    if (abc) abc->close();
    report.frames = frameCount;
    report.milliseconds = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    return report;
}

}  // namespace groomlab::cli
