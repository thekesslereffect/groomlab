// SPDX-License-Identifier: Apache-2.0
// The Reference window's model (reference_edit.h): landmarks paired Wrap-style, flow arrows and
// mask touch-ups with undo, the state's JSON, the live solve from landmarks, the mesh drawn over
// the photo, the arrows' drawing, and the mask touch-ups reaching the mask the core works with.

#include <algorithm>
#include <cmath>
#include <vector>

#include <gtest/gtest.h>

#include "groomlab/reference_edit.h"
#include "groomlab/scalp_mesh.h"

namespace groomlab {
namespace {

constexpr float kPi = 3.14159265358979323846f;
constexpr MarkerSide P = kLandmarkPhoto;
constexpr MarkerSide M = kLandmarkMesh;

ScalpMesh sphere(float radius, int rings = 16, int segments = 32) {
    std::vector<Vec3f> points;
    std::vector<ScalpMesh::Triangle> tris;
    for (int r = 0; r <= rings; ++r) {
        const float theta = kPi * static_cast<float>(r) / static_cast<float>(rings);
        for (int s = 0; s < segments; ++s) {
            const float phi = 2.0f * kPi * static_cast<float>(s) / static_cast<float>(segments);
            points.push_back(Vec3f(std::sin(theta) * std::sin(phi), std::cos(theta), std::sin(theta) * std::cos(phi)) * radius);
        }
    }
    for (int r = 0; r < rings; ++r)
        for (int s = 0; s < segments; ++s) {
            const int a = r * segments + s, b = r * segments + (s + 1) % segments, c = a + segments, d = b + segments;
            tris.push_back({a, c, d});
            tris.push_back({a, d, b});
        }
    return ScalpMesh(points, tris);
}

// A camera on +Z (turned a little) at `distance`, looking at the origin.
RefCamera frontCamera(float distance, int width, int height, float fovDegrees = 40.0f) {
    const Vec3f z = Vec3f(0.2f, 0.15f, 1.0f).normalized();
    const Vec3f x = Vec3f(0, 1, 0).cross(z).normalized();
    const Vec3f y = z.cross(x);
    RefCamera c;
    const Vec3f axes[3] = {x, y, z};
    for (std::size_t i = 0; i < 3; ++i) {
        c.pose.rotation[0][i] = axes[i].x;
        c.pose.rotation[1][i] = axes[i].y;
        c.pose.rotation[2][i] = axes[i].z;
    }
    const Vec3f eye = z * distance;
    c.pose.position = {eye.x, eye.y, eye.z};
    c.width = width;
    c.height = height;
    c.focalPx = 0.5 * width / std::tan(0.5 * fovDegrees * kPi / 180.0f);
    return c;
}

MarkerPoint world(float x, float y, float z) {
    MarkerPoint p;
    p.point = Vec3f(x, y, z);
    return p;
}

TEST(ReferenceEdit, LandmarksPairByIndexInEitherOrderWithUndo) {
    ReferencePhotoEditor e;
    EXPECT_EQ(e.landmarks().summary("No landmarks"), "No landmarks");
    EXPECT_EQ(e.addLandmark(M, world(0, 1, 0)), 0u);  // the mesh first: fine
    EXPECT_EQ(e.addLandmark(P, ReferencePhotoEditor::photoPoint(10, 20)), 0u);
    EXPECT_EQ(e.addLandmark(P, ReferencePhotoEditor::photoPoint(30, 40)), 1u);
    EXPECT_EQ(e.landmarks().pairCount(), 1u);
    EXPECT_FALSE(e.landmarks().matched(P, 1));  // pending: drawn hollow
    EXPECT_EQ(e.landmarks().summary(), "1 pair, 1 unmatched");
    EXPECT_EQ(e.landmarks().point(P, 0).root.face, -1);
    EXPECT_FLOAT_EQ(e.landmarks().point(P, 1).point.y, 40.0f);
    // Deleting a matched point deletes its partner; both lists renumber.
    e.addLandmark(M, world(1, 0, 0));
    EXPECT_EQ(e.removeLandmark(P, 0), std::vector<std::size_t>{0});
    EXPECT_EQ(e.landmarks().count(P), 1u);
    EXPECT_EQ(e.landmarks().count(M), 1u);
    EXPECT_FLOAT_EQ(e.landmarks().point(M, 0).point.x, 1.0f);
    EXPECT_THROW(e.removeLandmark(P, 5), std::out_of_range);
    // A drag: its moves after the first join one step.
    const std::size_t before = e.undoDepth();
    e.moveLandmark(P, 0, ReferencePhotoEditor::photoPoint(31, 41), true);
    e.moveLandmark(P, 0, ReferencePhotoEditor::photoPoint(32, 42), false);
    e.moveLandmark(P, 0, ReferencePhotoEditor::photoPoint(33, 43), false);
    EXPECT_EQ(e.undoDepth(), before + 1);
    EXPECT_EQ(e.undoText(), "move landmark");
    ASSERT_TRUE(e.undo());
    EXPECT_FLOAT_EQ(e.landmarks().point(P, 0).point.x, 30.0f);
    ASSERT_TRUE(e.redo());
    EXPECT_FLOAT_EQ(e.landmarks().point(P, 0).point.x, 33.0f);
    const std::uint64_t revision = e.revision();
    e.clearLandmarks();
    EXPECT_TRUE(e.landmarks().empty());
    EXPECT_GT(e.revision(), revision);
    ASSERT_TRUE(e.undo());
    EXPECT_EQ(e.landmarks().pairCount(), 1u);
}

TEST(ReferenceEdit, FlowArrowsAndMaskStrokesAreUndoSteps) {
    ReferencePhotoEditor e;
    EXPECT_THROW(e.addStroke({{1, 1}}), std::invalid_argument);
    EXPECT_EQ(e.addStroke({{10, 10}, {10, 50}}), 0u);
    EXPECT_EQ(e.addStroke({{40, 10}, {45, 30}, {50, 50}}, false), 1u);
    EXPECT_EQ(e.strokes().size(), 2u);
    EXPECT_FALSE(e.strokes()[1].directed);
    // An erase near the first only; a miss changes nothing (no undo step).
    const std::size_t depth = e.undoDepth();
    EXPECT_EQ(e.eraseStrokes({{100, 100}}, 3.0f), 0u);
    EXPECT_EQ(e.undoDepth(), depth);
    EXPECT_EQ(e.eraseStrokes({{12, 30}}, 3.0f), 1u);
    ASSERT_EQ(e.strokes().size(), 1u);
    EXPECT_FLOAT_EQ(e.strokes()[0].points[0][0], 40.0f);
    EXPECT_EQ(e.undoText(), "erase guide lines");
    e.undo();
    EXPECT_EQ(e.strokes().size(), 2u);
    e.addMaskStroke({{5, 5}, {20, 5}}, 4.0f, true);
    e.addMaskStroke({{60, 60}}, 6.0f, false);
    ASSERT_EQ(e.maskStrokes().size(), 2u);
    EXPECT_FALSE(e.maskStrokes()[1].include);
    EXPECT_EQ(e.undoText(), "mask: backdrop");
    e.clearMaskStrokes();
    EXPECT_TRUE(e.maskStrokes().empty());
    e.undo();
    EXPECT_EQ(e.maskStrokes().size(), 2u);
    e.clearStrokes();
    EXPECT_TRUE(e.strokes().empty());
    EXPECT_EQ(e.redoText(), "");
}

TEST(ReferenceEdit, StateRoundTripsThroughJson) {
    ReferencePhotoEditor e;
    e.addLandmark(P, ReferencePhotoEditor::photoPoint(10, 20));
    e.addLandmark(M, world(1, 2, 3));
    e.addLandmark(P, ReferencePhotoEditor::photoPoint(30, 40));  // pending
    e.addStroke({{1, 2}, {3, 4}}, false);
    e.addMaskStroke({{5, 6}}, 7.0f, false);
    const std::string text = referencePhotoStateToJson(e.state());
    const ReferencePhotoEditor::State back = referencePhotoStateFromJson(text);
    EXPECT_EQ(back.landmarks.count(P), 2u);
    EXPECT_EQ(back.landmarks.count(M), 1u);
    EXPECT_FLOAT_EQ(back.landmarks.point(M, 0).point.z, 3.0f);
    EXPECT_FLOAT_EQ(back.landmarks.point(P, 1).point.x, 30.0f);
    ASSERT_EQ(back.strokes.size(), 1u);
    EXPECT_FALSE(back.strokes[0].directed);
    ASSERT_EQ(back.maskStrokes.size(), 1u);
    EXPECT_FLOAT_EQ(back.maskStrokes[0].radius, 7.0f);
    EXPECT_FALSE(back.maskStrokes[0].include);
    EXPECT_EQ(referencePhotoStateToJson(back), text);
    EXPECT_TRUE(referencePhotoStateFromJson("").strokes.empty());
    EXPECT_THROW(referencePhotoStateFromJson("{\"strokes\": [{\"points\": [1]}]}"), std::invalid_argument);
}

TEST(ReferenceEdit, TheCameraSolvesLiveFromLandmarks) {
    const RefCamera truth = frontCamera(40.0f, 320, 240);
    const std::vector<Vec3f> points = {{-6, 5, 4}, {6, 5, 3}, {0, -4, 8}, {-5, -3, 2}, {5, -2, 6},
                                       {0, 7, -1}, {-2, 1, 9}, {7, 0, -2}, {-7, 1, -3}, {2, -7, 1}};
    ReferencePhotoEditor e;
    const double guess = truth.focalPx * 1.1;  // the focal length now: a little off
    for (std::size_t i = 0; i < points.size(); ++i) {
        float u = 0, v = 0;
        ASSERT_TRUE(truth.project(points[i], u, v));
        e.addLandmark(M, world(points[i].x, points[i].y, points[i].z));
        e.addLandmark(P, ReferencePhotoEditor::photoPoint(u, v));
        const ReferenceLandmarkSolve s = solveReferenceLandmarks(e.landmarks(), 320, 240, guess, false);
        if (i + 1 < kReferenceMinPairs) {
            EXPECT_FALSE(s.ok);
            EXPECT_NE(s.why.find("4 landmark pairs"), std::string::npos) << s.why;
            continue;
        }
        ASSERT_TRUE(s.ok) << s.why;
        EXPECT_EQ(s.pairs, i + 1);
        EXPECT_EQ(s.errors.size(), i + 1);
        EXPECT_EQ(s.projected.size(), i + 1);
        EXPECT_EQ(s.focalSolved, i + 1 >= kReferenceMinPairsFocal);
        if (s.focalSolved) {
            EXPECT_NEAR(s.camera.focalPx, truth.focalPx, 0.01 * truth.focalPx);
            EXPECT_LT(s.rms, 0.05);
            for (int k = 0; k < 3; ++k) EXPECT_NEAR(s.camera.pose.position[static_cast<std::size_t>(k)], truth.pose.position[static_cast<std::size_t>(k)], 0.05);
            EXPECT_NEAR(s.projected[0][0], e.landmarks().point(P, 0).point.x, 0.1);
        } else {
            EXPECT_DOUBLE_EQ(s.camera.focalPx, guess);  // kept
        }
    }
    // A pair placed badly stands out.
    e.moveLandmark(P, 2, ReferencePhotoEditor::photoPoint(e.landmarks().point(P, 2).point.x + 40.0f,
                                                         e.landmarks().point(P, 2).point.y));
    const ReferenceLandmarkSolve bad = solveReferenceLandmarks(e.landmarks(), 320, 240, guess, false);
    ASSERT_TRUE(bad.ok);
    ASSERT_EQ(bad.bad.size(), points.size());
    EXPECT_TRUE(bad.bad[2]);
    EXPECT_EQ(std::count(bad.bad.begin(), bad.bad.end(), true), 1);
    // The focal length kept: 4 pairs are enough even with 10.
    const ReferenceLandmarkSolve kept = solveReferenceLandmarks(e.landmarks(), 320, 240, guess, true);
    ASSERT_TRUE(kept.ok);
    EXPECT_FALSE(kept.focalSolved);
    EXPECT_FALSE(solveReferenceLandmarks(e.landmarks(), 0, 0, guess, false).ok);
    EXPECT_EQ(referenceBadLandmarks({1.0, 1.2, 0.9}, 320), std::vector<bool>({false, false, false}));
}

TEST(ReferenceEdit, TheSolveNeverRunsOffToATelephoto) {
    // Pairs seen nearly flat (a profile's): a free focal length can slide toward a lens miles away
    // (the camera past the far clip, no mesh left to pick); a runaway focal length left on the
    // camera comes back to a normal lens, kept or not.
    const RefCamera truth = frontCamera(40.0f, 320, 240);
    const std::vector<Vec3f> points = {{-6, 5, 1.0f}, {6, 5, 0}, {0, -4, 1.5f}, {-5, -3, 0.3f}, {5, -2, 1.2f}, {0, 7, 0}};
    ReferencePhotoEditor e;
    for (const Vec3f& p : points) {
        float u = 0, v = 0;
        ASSERT_TRUE(truth.project(p, u, v));
        e.addLandmark(M, world(p.x, p.y, p.z));
        e.addLandmark(P, ReferencePhotoEditor::photoPoint(u + 1.5f, v - 1.0f));  // a click's slop
    }
    const ReferenceLandmarkSolve s = solveReferenceLandmarks(e.landmarks(), 320, 240, truth.focalPx, false);
    ASSERT_TRUE(s.ok) << s.why;
    EXPECT_LE(s.camera.focalPx, 30.0 * 320);
    const double runaway = 500.0 * 320;
    const ReferenceLandmarkSolve back = solveReferenceLandmarks(e.landmarks(), 320, 240, runaway, false);
    ASSERT_TRUE(back.ok) << back.why;
    EXPECT_LE(back.camera.focalPx, 30.0 * 320);
    const ReferenceLandmarkSolve kept = solveReferenceLandmarks(e.landmarks(), 320, 240, runaway, true);
    ASSERT_TRUE(kept.ok) << kept.why;
    EXPECT_DOUBLE_EQ(kept.camera.focalPx, 320.0);  // even kept: a normal lens
    EXPECT_DOUBLE_EQ(solveReferenceLandmarks(e.landmarks(), 320, 240, 900.0, true).camera.focalPx, 900.0);
}

TEST(ReferenceEdit, TheMeshOverThePhotoOutlinesItAndDrawsItsFrontFaces) {
    const ScalpMesh ball = sphere(10.0f);
    const RefCamera cam = frontCamera(60.0f, 320, 240);
    const std::vector<std::array<float, 4>> outline = referenceMeshOverlay(ball, cam, true);
    const std::vector<std::array<float, 4>> wire = referenceMeshOverlay(ball, cam, false);
    ASSERT_GT(outline.size(), 16u);
    EXPECT_GT(wire.size(), 4 * outline.size());
    // The outline lies on the ball's silhouette: a circle about the center of the photo.
    float c[2] = {0, 0};
    ASSERT_TRUE(cam.project(Vec3f(0, 0, 0), c[0], c[1]));
    float lo = 1e9f, hi = 0.0f;
    for (const auto& s : outline) {
        const float r = std::hypot(s[0] - c[0], s[1] - c[1]);
        lo = std::min(lo, r);
        hi = std::max(hi, r);
    }
    EXPECT_GT(lo, 0.85f * hi);
    // The wireframe: only the faces toward the camera (half the ball's edges or so, of ~3 per vertex).
    EXPECT_LT(wire.size(), 3 * ball.pointCount() * 3 / 4);
    // Polygons: two triangles a quad, the diagonal left out.
    ScalpMesh quads = ball;
    quads.setPolygons(std::vector<std::int32_t>(ball.triangleCount() / 2, 2));
    EXPECT_LT(referenceMeshOverlay(quads, cam, false).size(), wire.size());
}

TEST(ReferenceEdit, ArrowsAreASmoothCurveWithAHead) {
    const ReferenceArrow a = referenceArrow({{0, 0}, {0, 50}, {10, 100}}, 10.0f);
    EXPECT_GT(a.line.size(), 3u);
    ASSERT_TRUE(a.hasHead);
    EXPECT_FLOAT_EQ(a.head[1][0], 10.0f);
    EXPECT_FLOAT_EQ(a.head[1][1], 100.0f);
    EXPECT_LT(a.head[0][1], 100.0f);  // the sides back along the stroke
    EXPECT_FALSE(referenceArrow({{0, 0}}, 10.0f).hasHead);
}

TEST(ReferenceEdit, MaskStrokesTouchUpTheMaskThePreviewShows) {
    // A plain light backdrop, a dark block in the middle (the hair); the ball fills the middle.
    const int w = 160, h = 120;
    ReferenceView view;
    view.camera = frontCamera(60.0f, w, h);
    view.image.width = w;
    view.image.height = h;
    view.image.channels = 3;
    view.image.pixels.assign(static_cast<std::size_t>(w * h * 3), 0.9f);
    for (int y = 30; y < 90; ++y)
        for (int x = 50; x < 110; ++x)
            for (int k = 0; k < 3; ++k)
                view.image.pixels[static_cast<std::size_t>((y * w + x) * 3 + k)] = (x % 3 == 0) ? 0.2f : 0.35f;
    ReferenceGuideOptions options;
    options.cropToSubject = false;
    const ScalpMesh ball = sphere(10.0f);
    const auto at = [&](const ReferenceViewPreview& p, float u, float v) {
        const int x = static_cast<int>((u - static_cast<float>(p.crop[0])) * static_cast<float>(p.mask.width) / static_cast<float>(p.crop[2]));
        const int y = static_cast<int>((v - static_cast<float>(p.crop[1])) * static_cast<float>(p.mask.height) / static_cast<float>(p.crop[3]));
        return p.mask.at(x, y);
    };
    const ReferenceViewPreview plain = previewReferenceView(ball, view, options);
    EXPECT_EQ(plain.crop[2], w);
    EXPECT_GT(at(plain, 80, 60), 0.5f);   // the dark block is hair
    EXPECT_LT(at(plain, 70, 100), 0.5f);  // the backdrop is not
    EXPECT_FALSE(plain.flowLines.empty());
    // Exclude a corner of the block, include a patch of backdrop next to it.
    view.maskStrokes.push_back({{{55, 35}, {65, 35}}, 4.0f, false});
    view.maskStrokes.push_back({{{80, 95}, {90, 95}}, 3.0f, true});
    const ReferenceViewPreview touched = previewReferenceView(ball, view, options);
    EXPECT_LT(at(touched, 60, 35), 0.5f);
    EXPECT_GT(at(touched, 85, 95), 0.5f);
    EXPECT_GT(at(touched, 80, 60), 0.5f);  // the rest as it was
    for (const auto& l : touched.flowLines) {
        EXPECT_GE(std::min(l[0], l[2]), -5.0f);
        EXPECT_LE(std::max(l[0], l[2]), static_cast<float>(w) + 5.0f);
    }
}

}  // namespace
}  // namespace groomlab
