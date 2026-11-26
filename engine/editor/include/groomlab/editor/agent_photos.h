// SPDX-License-Identifier: Apache-2.0
#pragma once

// Reference photos for the agent tools (agent_tools.h: add_photo, photo_view, mesh_landmarks,
// align_photo, photo_edit, guides_from_photos): the photos a groom keeps as the app's Reference
// window keeps them (groom_file.h "referencePhotos": each photo's file, size, mask, solved camera
// and landmarks / flow arrows / mask touch-ups, reference_edit.h), a camera that frames the scalp
// from a named side, the mesh's points an agent can pair with a photo's pixels, and the pictures
// that let it read them: the photo under a labelled pixel grid, the mesh with numbered points, the
// mesh lined up over the photo.

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "groomlab/reference_edit.h"
#include "groomlab/reference_guides.h"

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
// "three_quarter": the character's, as render_preview's; `upAxis` "Y" or "Z"), far enough to frame
// it in a `width` x `height` image at `focalMm`. Throws std::invalid_argument for another view.
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

// The photo under a pixel grid every `gridStep` photo pixels (labelled with its pixel coordinates),
// its landmarks numbered (green: paired, yellow: pending), its flow arrows and mask touch-ups, at
// most `maxSize` pixels across (labels in photo pixels). With `preview` (previewReferenceView: the
// photo lined up), the hair mask tinted and the flow it reads.
std::vector<std::uint8_t> photoGridPng(const RefImage& image, const AgentPhoto& photo, int gridStep, int maxSize,
                                       const ReferenceViewPreview* preview = nullptr, int* shownWidth = nullptr,
                                       int* shownHeight = nullptr);
// The mesh through `camera`, the candidates' numbers on it.
std::vector<std::uint8_t> meshCandidatesPng(const ScalpMesh& scalp, const RefCamera& camera,
                                            const std::vector<MeshCandidate>& candidates, int maxSize);
// The mesh's outline and wireframe over the photo through the photo's camera, each landmark pair's
// photo point (green) and its mesh point through the camera (red) joined by a line: how well it
// lines up.
std::vector<std::uint8_t> photoOverlayPng(const RefImage& image, const ScalpMesh& scalp, const AgentPhoto& photo, int maxSize);

}  // namespace groomlab::agent
