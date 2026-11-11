// SPDX-License-Identifier: Apache-2.0
#pragma once

// Render > Render Frame in RenderMan / Render Turntable in RenderMan: the groom rendered by
// RenderMan's prman (RenderMan Pro Server, free for non-commercial use; a separate process:
// render_job.h's RenderJob and RenderWindow) as the viewport frames and lights it.
//
// The groom goes as a .groom (GroomDocument::writeRenderGroom: the graph and guides as they are
// now) that GroomLab's RenderMan procedural (groomlab_renderman, hosts/renderman: beside the app in
// procedurals/renderman) evaluates at render time, shaded by the procedural's hair network
// (hosts/renderman/examples/groom.rib: PxrHairColor + PxrMarschnerHair fitted to the Arnold look).
// A RIB beside it holds the stage (writePrmanScene; the Maya host's groomlab.prman_scene writes the
// same conventions):
//   - the camera: the viewport's eye, target and vertical field of view (PxrPerspective's fov
//     spans the image's shorter side), Z flipped (RenderMan's camera looks down its +Z); a
//     turntable orbits it around the target about Y, a FrameBegin block per frame;
//   - each enabled directional light as a PxrDistantLight (the app's and Arnold's intensity is the
//     irradiance; PxrDistantLight's the radiance of its disk: divided by its solid angle), the
//     environment as a PxrDomeLight of a lat-long texture (txmake of the .hdr makeEnvironment
//     gives, turned by environmentRotation) at environmentIntensity, the exposure (2^EV) on the
//     lights; the background the solid color (PxrBackgroundDisplayFilter), or the dome seen;
//   - the scalp mesh (when shown) with a PxrSurface of its material's base color.
// The image is prman's linear result written as an sRGB PNG (d_png): no tone map.

#include <QProcessEnvironment>
#include <QString>
#include <QStringList>

#include <string>
#include <vector>

#include "render_job.h"

namespace groomlab::app {

// PxrDistantLight's disk (degrees): the sun's.
constexpr float kPrmanDistantAngle = 0.53f;

struct PrmanScene {
    std::string groomPath;        // the .groom the procedural renders
    double frame = 1.0;           // the groom's frame
    float hairTilt = 3.0f;        // HairColor's cuticle tilt (PxrMarschnerHair's specularOffset, negated)
    std::string procedurals;      // the folder holding groomlab_renderman ("" : prman's own search path)
    std::string imagePath;        // the PNG (a turntable: <stem>.0001.png...)
    std::string environmentTexture;  // the dome's texture (prmanEnvironmentTexture's); "" with no environment
    // Render overrides (render_overrides.h) the procedural applies: the share of the strands, the
    // widths' and lengths' scales (1: as groomed; left out of the RIB then).
    float density = 1.0f, widthScale = 1.0f, lengthScale = 1.0f;
};

// Writes the RIB at `scenePath`. Throws std::runtime_error when it cannot be written.
void writePrmanScene(const std::string& scenePath, const PrmanScene& scene, const RenderStage& stage);
// The dome's texture for `stage`'s environment beside `scenePath`: the .hdr written and made a
// lat-long texture by txmake (beside prman). "" when the lighting has no environment; throws
// std::runtime_error when txmake fails.
std::string prmanEnvironmentTexture(const QString& prman, const std::string& scenePath, const RenderStage& stage);

// prman, as Maya's prman_scene.find_prman looks: the one chosen in Render > RenderMan Location...
// (saved), else GROOMLAB_PRMAN (the executable, its folder or RenderMan Pro Server's), RMANTREE's,
// the newest default install (Pixar/RenderManProServer-*/bin), prman on PATH. Empty when there is
// none.
QString findPrman();
void setPrman(const QString& path);
// prman's executable in `path` (the file, its folder, or an install folder with bin/), or "".
QString prmanIn(const QString& path);
// GroomLab's RenderMan procedural's folder: GROOMLAB_RENDERMAN_PROCEDURAL (the file or its folder),
// else procedurals/renderman beside the app, else an installed GroomLab Maya module's. "" when none.
QString findRenderManProcedural();

// prman's arguments (progress on the console) and environment (RMANTREE its install).
QStringList prmanArguments(const QString& scenePath);
QProcessEnvironment prmanEnvironment(const QString& prman);

// prman's console: "R90000  45%" lines; a frame ends at 100%. Errors: "{ERROR}" or "{SEVERE}".
struct PrmanProgress : RenderProgress {
    bool feed(const QString& line) override;
};

}  // namespace groomlab::app
