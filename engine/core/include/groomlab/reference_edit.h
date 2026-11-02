// SPDX-License-Identifier: Apache-2.0
#pragma once

// The model behind every host's Reference window (Guides from Reference: Maya's
// groomlab.ui.reference_window, the app's ReferenceWindow): one photo's landmarks, flow arrows and
// mask touch-ups, edits with an undo stack, and what the window draws and solves from them. The
// views are the hosts'; the rules are the same in both:
//
//   - Landmarks, Wrap-style (the Transfer window's pairing rules, MarkerLists): the photo keeps an
//     ordered list of points (pixels from its top-left, v down: a MarkerPoint at (u, v, 0) without a
//     face) and the mesh another (world points on the scalp); pair N is the Nth point on the photo
//     and the Nth on the mesh, placed in either order. Deleting a point deletes its partner; a point
//     without a partner yet is pending (drawn hollow).
//   - The camera solves live from the complete pairs (solveReferenceLandmarks): 4 pairs line it up
//     with the focal length kept, 6 or more solve the focal length too (unless kept). Each pair gets
//     its reprojection error; pairs far worse than the rest are flagged (referenceBadLandmarks).
//   - Flow arrows: polylines in the photo's pixels from the root end to the tip (what the core's
//     guidesFromReference takes as hints); an erase removes those within a radius of a path.
//   - Mask touch-ups: brush lines in the photo's pixels that include hair or exclude backdrop
//     (RefMaskStroke: the mask refinement takes them as sure).
//   - Every edit is one undo step; a drag's moves after its first join that step. Hosts store the
//     state where they keep it (Maya: the camera's glRefPairs and the groom's glReferenceViews; the
//     app: its document) and redraw when revision() moved.

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "groomlab/camera_solve.h"
#include "groomlab/reference_guides.h"
#include "groomlab/transfer_markers_edit.h"

namespace groomlab {

class ScalpMesh;

// The photo view's side of a landmark pair (left) and the mesh view's (right).
constexpr MarkerSide kLandmarkPhoto = MarkerSide::Old;
constexpr MarkerSide kLandmarkMesh = MarkerSide::New;

class ReferencePhotoEditor {
public:
    struct State {
        MarkerLists landmarks;
        std::vector<RefHintStroke> strokes;      // flow arrows (photo pixels, root first)
        std::vector<RefMaskStroke> maskStrokes;  // mask touch-ups (photo pixels)
    };

    ReferencePhotoEditor() = default;
    explicit ReferencePhotoEditor(State state) { reset(std::move(state)); }

    // The state as stored (a window opening on a photo, the host's data changed elsewhere); no history.
    void reset(State state);
    const State& state() const { return state_; }
    const MarkerLists& landmarks() const { return state_.landmarks; }
    const std::vector<RefHintStroke>& strokes() const { return state_.strokes; }
    const std::vector<RefMaskStroke>& maskStrokes() const { return state_.maskStrokes; }

    // ---- Landmarks (each one undo step) ----
    static MarkerPoint photoPoint(float u, float v);  // a photo pixel as a landmark point
    std::size_t addLandmark(MarkerSide side, const MarkerPoint& p);                     // its index
    std::vector<std::size_t> removeLandmark(MarkerSide side, std::size_t i);           // throws std::out_of_range
    void moveLandmark(MarkerSide side, std::size_t i, const MarkerPoint& p, bool newStep = true);  // throws
    void clearLandmarks();  // nothing when there are none

    // ---- Flow arrows ----
    // A stroke of 2 or more points (root first); returns its index. Throws std::invalid_argument.
    std::size_t addStroke(std::vector<std::array<float, 2>> points, bool directed = true);
    // Removes the strokes within `radius` pixels of `path` (one point: a click); one step when any
    // went. Returns how many.
    std::size_t eraseStrokes(const std::vector<std::array<float, 2>>& path, float radius);
    void clearStrokes();

    // ---- Mask touch-ups ----
    std::size_t addMaskStroke(std::vector<std::array<float, 2>> points, float radius, bool include);  // throws on none
    void clearMaskStrokes();

    bool canUndo() const { return !undo_.empty(); }
    bool canRedo() const { return !redo_.empty(); }
    bool undo();
    bool redo();
    std::size_t undoDepth() const { return undo_.size(); }
    // What undo() and redo() would take back or do again ("landmark", "guide line", ...; "" none).
    std::string undoText() const { return undo_.empty() ? std::string() : undo_.back().what; }
    std::string redoText() const { return redo_.empty() ? std::string() : redo_.back().what; }
    std::uint64_t revision() const { return revision_; }

private:
    struct Step {
        State state;
        std::string what;
    };
    void record(const char* what);

    State state_;
    std::vector<Step> undo_, redo_;
    std::uint64_t revision_ = 0;
    static constexpr std::size_t kMaxUndo = 256;
};

// The state as JSON and back (the app's document keeps it; Maya keeps the same pieces on its
// nodes): {"landmarks": [{"image": [u, v] or null, "mesh": [x, y, z] or null}, ...] (pair N's two
// points, null where it has none yet), "strokes": [{"points": [[u, v], ...], "directed"}],
// "mask_strokes": [{"points", "radius", "include"}]}. FromJson throws std::invalid_argument.
std::string referencePhotoStateToJson(const ReferencePhotoEditor::State& state);
ReferencePhotoEditor::State referencePhotoStateFromJson(const std::string& text);

// ---- The live solve -------------------------------------------------------------------------------

struct ReferenceLandmarkSolve {
    bool ok = false;
    std::string why;           // when !ok: what is missing or failed
    std::size_t pairs = 0;     // complete pairs used
    bool focalSolved = false;  // the focal length was solved (6+ pairs, not kept)
    RefCamera camera;          // for the photo's own size
    double rms = 0.0;          // pixels
    std::vector<double> errors;                     // per complete pair, pixels
    std::vector<std::array<double, 2>> projected;   // each pair's mesh point through the camera (pixels)
    std::vector<bool> bad;  // referenceBadLandmarks of each pair's error through the camera the other pairs give (5+ pairs)
};

// Landmark pairs needed to solve: 4 with the focal length kept, 6 to solve it too.
constexpr std::size_t kReferenceMinPairs = 4;
constexpr std::size_t kReferenceMinPairsFocal = 6;

// The camera that puts each complete pair's mesh point on its photo point (camera_solve.h), for
// a photo `width` x `height` pixels. `focalPx`: the focal length now (kept with fewer than 6 pairs
// or with `keepFocal`); `initial`: where the camera is now (a start). `findBad`: flag the pairs that
// disagree with the rest (a solve per pair: a drag's live solves leave it to the drag's end).
ReferenceLandmarkSolve solveReferenceLandmarks(const MarkerLists& landmarks, double width, double height, double focalPx,
                                               bool keepFocal, const CameraPose* initial = nullptr, bool findBad = true);

// The pairs whose error stands out (over 2.5 times the median and over a 200th of the photo's
// width, at least 3 pixels): the ones to fix first. (The solve flags them by each pair's error
// through the camera the other pairs give: a bad pair pulls the camera toward it, spreading its
// error over the rest.)
std::vector<bool> referenceBadLandmarks(const std::vector<double>& errors, double width);

// ---- What the photo view draws ----------------------------------------------------------------

// The mesh over the photo through `camera`: line segments in the photo's pixels (x0, y0, x1, y1).
// silhouette: its outline (edges between a face toward the camera and one away, open edges of a
// face toward it); else its wireframe (the polygons' edges, ScalpMesh::polygons, of the faces toward
// the camera).
std::vector<std::array<float, 4>> referenceMeshOverlay(const ScalpMesh& mesh, const RefCamera& camera, bool silhouette);

// A flow arrow as both hosts draw it (the Reference Flow tool's style): the stroke as one smooth
// curve and a head at the tip, `headSize` pixels long along its last stretch.
struct ReferenceArrow {
    std::vector<std::array<float, 2>> line;
    std::array<std::array<float, 2>, 3> head{};  // one side, the tip, the other side
    bool hasHead = false;
};
ReferenceArrow referenceArrow(const std::vector<std::array<float, 2>>& points, float headSize);

}  // namespace groomlab
