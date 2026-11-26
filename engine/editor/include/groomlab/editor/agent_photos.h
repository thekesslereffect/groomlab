// SPDX-License-Identifier: Apache-2.0
#pragma once

// Reference photos for the agent tools (agent_tools.h: add_photo, photo_view, mesh_landmarks,
// align_photo, photo_edit, guides_from_photos): the photos a groom keeps as the app's Reference
// window keeps them (groom_file.h "referencePhotos": each photo's file, size, mask, solved camera
// and landmarks / flow arrows / mask touch-ups, reference_edit.h), a camera that frames the scalp
// from a named side, the mesh's points an agent can pair with a photo's pixels, and the pictures
// that let it read them: the photo under a labelled pixel grid, the mesh with numbered points, the
// mesh lined up over the photo.

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "groomlab/reference_edit.h"
#include "groomlab/reference_guides.h"
#include "groomlab/transfer_markers_edit.h"

namespace groomlab::agent {

// One reference photo, as the groom file's "referencePhotos" holds it.
struct AgentPhoto {
    std::string name;
    std::string image;  // the image file as stored (here: its full path)
    int width = 0, height = 0;
    std::string mask;   // a mask image ("" none)
    std::optional<RefCamera> camera;  // lined up (null: not yet)
    ReferencePhotoEditor::State state;
    nlohmann::json display;  // the Photos tab's display settings, kept as they were (null: none)
};

// The groom's photos from its "referencePhotos" text ("" none) and back ("" for none), the app's
// form. FromJson throws std::invalid_argument for text that is not such an array.
std::vector<AgentPhoto> photosFromJson(const std::string& text);
std::string photosToJson(const std::vector<AgentPhoto>& photos);
nlohmann::json cameraToJson(const RefCamera& camera);

// Focal lengths: millimeters on a 36 mm wide film back <-> pixels of an image `width` wide.
double focalMmToPixels(double mm, double width);
double focalPixelsToMm(double px, double width);

// A camera looking at the scalp's middle from a side ("front", "back", "left", "right", "top",
// "three_quarter": the character's, as render_preview's; three_quarter_front_left / _right and
// three_quarter_back_left / _right: 45 degrees round from the front or the back toward that side;
// `upAxis` "Y" or "Z"), far enough to frame it in a `width` x `height` image at `focalMm`. Throws
// std::invalid_argument for another view.
const std::vector<std::string>& viewCameraNames();
RefCamera viewCamera(const ScalpMesh& scalp, const std::string& upAxis, const std::string& view, int width, int height,
                     double focalMm = 50.0);

// Points of the mesh an agent can find on a photo too: those the camera sees, its silhouette's
// extremes (top, bottom, left, right), the points nearest the camera, then spread evenly over what
// it sees (farthest first), `count` in all, numbered from 1 in the order given.
struct MeshCandidate {
    Vec3f point, normal;
    float u = 0.0f, v = 0.0f;  // where the camera sees it (pixels)
};
std::vector<MeshCandidate> meshCandidates(const ScalpMesh& scalp, const RefCamera& camera, int count);

// Morph to Photos as the hosts' Reference windows do it (pin_morph.h): a pin for each lined-up
// photo's landmark pair (the same spot paired in several photos: one pin, seen in each), its point
// on `restRefined` (the scalp before the first morph, as the session refines it) found by the pair's
// spot on `current` (the scalp as it is now, where the pairs were picked: the same triangles), and
// `rest`'s points moved (stiffness 0..0.95; symmetric: mirrored across the plane through the rest's
// middle along `axis`, 0 x).
struct PhotoMorph {
    bool ok = false;
    std::string error;
    std::vector<Vec3f> points;  // rest's points, moved
    int pins = 0, views = 0;
    double rmsBefore = 0.0, rmsAfter = 0.0;  // the pins' reprojection error, pixels
};
PhotoMorph morphToPhotos(const ScalpMesh& rest, const ScalpMesh& restRefined, const ScalpMesh& current, const std::vector<AgentPhoto>& photos,
                         double stiffness, bool symmetric, int axis = 0);
// Where a pair's mesh point is on `to` (the same triangles as `from`): its spot on the surface (its
// root, else the spot of `from` nearest the point as stored).
Vec3f markerOn(const ScalpMesh& from, const ScalpMesh& to, const MarkerPoint& point);

// The photo under a pixel grid every `gridStep` photo pixels (labelled with its pixel coordinates),
// its landmarks numbered (green: paired, yellow: pending), its flow arrows and mask touch-ups, at
// most `maxSize` pixels across (labels in photo pixels). With `preview` (previewReferenceView: the
// photo lined up), the hair mask tinted and the flow it reads.
std::vector<std::uint8_t> photoGridPng(const RefImage& image, const AgentPhoto& photo, int gridStep, int maxSize,
                                       const ReferenceViewPreview* preview = nullptr, int* shownWidth = nullptr,
                                       int* shownHeight = nullptr);
// A part of the photo, `region` (x0, y0, x1, y1 in its pixels), enlarged to at most `maxSize`
// across under a finer grid every `gridStep` pixels (labelled in the photo's pixels), its landmarks.
std::vector<std::uint8_t> photoRegionPng(const RefImage& image, const AgentPhoto& photo, const std::array<int, 4>& region, int gridStep,
                                         int maxSize, int* shownWidth = nullptr, int* shownHeight = nullptr);
// The mesh through `camera`, the candidates' numbers on it.
// `labels`: named points besides (their code drawn at their pixel, cyan).
struct PointLabel {
    std::string code;
    float u = 0.0f, v = 0.0f;
};
std::vector<std::uint8_t> meshCandidatesPng(const ScalpMesh& scalp, const RefCamera& camera,
                                            const std::vector<MeshCandidate>& candidates, int maxSize,
                                            const std::vector<PointLabel>& labels = {});
// compare_to_photo's picture: the photo dimmed, the hair the groom misses tinted green, the groom's
// hair outside the photo's tinted magenta, both outlines (the photo's green, the groom's magenta).
std::vector<std::uint8_t> comparisonPng(const RefImage& image, const PhotoComparison& c, int maxSize);

// The mesh's outline and wireframe over the photo through the photo's camera, each landmark pair's
// photo point (green) and its mesh point through the camera (red) joined by a line: how well it
// lines up.
std::vector<std::uint8_t> photoOverlayPng(const RefImage& image, const ScalpMesh& scalp, const AgentPhoto& photo, int maxSize);

}  // namespace groomlab::agent
