// SPDX-License-Identifier: Apache-2.0
#pragma once

// The tube shape editor (Groom Tools > Tubes), Maya's tube_shape_editor.py TubeShapeEditor in
// C++: a square showing a tube's cross-section outline, its control points draggable.
//
// The square is the section's unit domain: u across (left to right), v up, the guide through its
// center (the cross). The outline is the closed curve through the points as the core draws it
// (sampleTubeShapeCurve: smooth, or straight edges). Drag a point to move it; click on the
// outline between two points to add one there; right-click a point to remove it (3 stay at
// least). With mirror (on by default) the outline stays symmetric about the v axis: a point and
// its mirror image move, come and go together, points on the axis stay on it. Built-in shapes
// show read-only.
//
// Signals: dragged(points) while a point moves, committed(points) when an edit settles (a
// release, an added or removed point). The core fits the points into the square when they are
// saved (GroomDocument::setTubeShape), so a shape drawn small comes back filling it.

#include <optional>
#include <utility>
#include <vector>

#include <QPointF>
#include <QRectF>
#include <QString>
#include <QWidget>

#include "groomlab/tube.h"

namespace groomlab::app {

namespace tube_editor {
constexpr float kExtent = 1.25f;  // the square shows -kExtent..kExtent on both axes (the outline is fitted to -1..1)
constexpr float kAxis = kMirrorAxis;  // points this near the v axis are on it (mirror: tube.h)
constexpr qreal kHit = 7.0;       // pixels: how near a press must be to a point or the outline

// The points made symmetric about the v axis from their right side (u > 0): the right chain in
// order, the points on the axis at its ends set on it, its mirror image closing the outline.
// Points with nothing right of the axis come back mirrored from the left. Fewer than 3 after:
// the points as they were.
std::vector<SectionPoint> symmetric(const std::vector<SectionPoint>& points);
// Each point's mirror image among the points (its own index on the axis; -1 when it has none).
std::vector<int> partners(const std::vector<SectionPoint>& points);
}  // namespace tube_editor

class TubeShapeEditor : public QWidget {
    Q_OBJECT
public:
    explicit TubeShapeEditor(QWidget* parent = nullptr);

    // Shows an outline (control points (u, v)); not while a point is dragged.
    void setShape(const std::vector<SectionPoint>& points, bool smooth, bool editable, const QString& note = {});
    const std::vector<SectionPoint>& points() const { return points_; }
    bool smooth() const { return smooth_; }
    bool editable() const { return editable_; }
    bool busy() const { return drag_ >= 0; }
    bool mirror() const { return mirror_; }
    // Mirror on: the outline made symmetric (from its right side) when it is not. Returns whether
    // the points changed.
    bool setMirror(bool on);
    // What is wrong with the outline as it is (drawn red): makeCustomTubeShape's complaint; "" fine.
    const QString& problem() const { return problem_; }
    const QString& note() const { return note_; }

    // The square's geometry (widget pixels).
    QRectF square() const;
    QPointF toScreen(float u, float v) const;
    SectionPoint fromScreen(const QPointF& at) const;

    // The edits, as the mouse makes them (tests): a point moved (its mirror image with it), a
    // point added on the span after control point `span`, a point removed (false when fewer
    // than 3 would stay).
    void movePoint(int index, float u, float v);
    void addPoint(int span, SectionPoint at);
    bool removePoint(int index);
    int pointAt(const QPointF& at) const;
    // (span, the outline's point) nearest `at` within kHit pixels; nothing off the outline.
    std::optional<std::pair<int, SectionPoint>> edgeAt(const QPointF& at) const;

    QSize sizeHint() const override;

signals:
    void dragged(const std::vector<groomlab::SectionPoint>& points);
    void committed(const std::vector<groomlab::SectionPoint>& points);

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void leaveEvent(QEvent* event) override;

private:
    void updateCurve();

    std::vector<SectionPoint> points_;
    std::vector<SectionPoint> curve_;
    bool smooth_ = true;
    bool mirror_ = true;
    bool editable_ = false;
    QString problem_, note_;
    int drag_ = -1, hover_ = -1;
};

}  // namespace groomlab::app
