// SPDX-License-Identifier: Apache-2.0
#pragma once

// Render > Render Frame in V-Ray / Render Turntable in V-Ray: the groom rendered by V-Ray
// Standalone (vray.exe, a separate process: render_job.h's RenderJob and RenderWindow) as the
// viewport frames and lights it.
//
// The groom goes as a .vrscene (GroomDocument::exportVrscene: vrscene_writer.h's GeomMayaHair,
// per-point colors, BRDFHair4 fitted to the Arnold look) and a scene beside it #includes it with
// the stage (writeVrayScene; tests/data/app/vray_compare.py is the same mapping in Python):
//   - the camera: the viewport's eye, target and vertical field of view at the image's aspect
//     (RenderView: V-Ray's fov is the horizontal one); a turntable orbits it around the target
//     about Y, its transform keyed per frame (one image per frame);
//   - the lighting preset: each enabled directional light as a LightDirect (V-Ray gives pi x its
//     intensity as irradiance, the app and Arnold their intensity: intensity / pi), the
//     environment as an invisible LightDome from a lat-long .hdr of makeEnvironment (turned by
//     environmentRotation) at environmentIntensity, the background as SettingsEnvironment's
//     (the solid color, or the environment at backgroundIntensity), the exposure as
//     SettingsColorMapping's (2^EV);
//   - the scalp mesh (when shown) as a GeomStaticMesh with a BRDFDiffuse of its material's base
//     color.
// The image is V-Ray's linear result written as an sRGB PNG: no tone map (the viewport's ACES and
// AgX are its own). Not carried: props and eyes, the look panel's override material (V-Ray
// renders the groom's own colors, as Maya's renderers do), shadow softness, ambient occlusion.

#include <QString>
#include <QStringList>

#include <string>

#include "render_job.h"

namespace groomlab::app {

using VrayStage = RenderStage;

// Writes the scene at `scenePath` #including `groomPath` (and "<stem>_env.hdr" beside it when the
// lighting has an environment). Throws std::runtime_error when a file cannot be written.
void writeVrayScene(const std::string& scenePath, const std::string& groomPath, const RenderStage& stage);

// V-Ray Standalone's executable, as Maya's vray_scene.find_vray looks: the one chosen in Render >
// V-Ray Standalone Location... (saved), else GROOMLAB_VRAY, VRAY_PATH (each the file, its folder
// or an install folder with bin/), the default installs (Chaos or Chaos Group/V-Ray/Standalone
// for x64/bin), vray on PATH. Empty when there is none.
QString findVrayStandalone();
void setVrayStandalone(const QString& path);

// vray.exe's arguments: no window, closed when done, progress on the console.
QStringList vrayArguments(const QString& scenePath, const QString& imagePath, int turntableFrames);

// V-Ray's console: "Starting frame N.", "Rendering image...: P%", "Successfully written image
// file \"...\"".
struct VrayProgress : RenderProgress {
    bool feed(const QString& line) override;
};

}  // namespace groomlab::app
