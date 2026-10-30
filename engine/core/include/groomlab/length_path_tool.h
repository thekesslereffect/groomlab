// SPDX-License-Identifier: Apache-2.0
#pragma once

// The Length Paths tool (length_paths.h): keys clicked on the scalp make paths, a key dragged up or
// down sets its length, and the guides near the paths preview at their new lengths until Apply
// commits them (one undo step, the paths saved with the guides) or Cancel drops the changes. The
// model and its mouse handling, the same in every host; hosts draw overlay() and give the panel's
// edits (setKeyLength, setReach...) and Apply / Cancel.
//
// Mouse (port pixels, origin bottom left, as BrushTool's):
//   - left click on the scalp: a key at the end of the active path (Shift: starts a new path). Its
//     length: the guides' around it (averageGuideLength), so placing keys changes nothing until
//     one is set. Its reach: the path's last key's (a new path's first: the brush circle's radius
//     there).
//   - left click on a path's line, between two keys: a key there, its length and reach the path's
//     there (the same lengths as before).
//   - left press on a key selects it; dragging moves it over the scalp (a new key too).
//   - Shift+drag from a key: its reach follows the cursor (the ring through it).
//   - the adjusting button (kStrokeAdjust) on a key, or anywhere for the selected key: dragging up
//     lengthens it, down shortens it (doubles every kPixelsPerDoubling pixels).
//   - Ctrl+click on a key deletes it (and its path with its last key).
// The guides it changes: the selected guides, else those the brush settings target (the level or
// group, not hidden or frozen: the soloed ones). With symmetry on (World X; topology as X) the
// paths mirrored across x = 0 act too.

#include <array>
#include <memory>
#include <string>
#include <vector>

#include "groomlab/length_paths.h"
#include "groomlab/math.h"

namespace groomlab {

class GuideHierarchy;
class StrokeHost;
struct BrushToolSettings;
struct ToolView;

class LengthPathTool {
public:
    static constexpr float kPickPixels = 10.0f;          // a key within this of the cursor is under it
    static constexpr float kPixelsPerDoubling = 150.0f;  // a key's drag: its length doubles per this

    LengthChange change = LengthChange::Scale;

    // Mouse events (BrushTool hands them on with Tool::Lengths). press returns false when it did
    // nothing (not on a key or the scalp; the host may say why).
    bool press(StrokeHost& host, const ToolView& view, const BrushToolSettings& settings, float x, float y, unsigned modifiers);
    void drag(StrokeHost& host, const ToolView& view, const BrushToolSettings& settings, float x, float y);
    void release(StrokeHost& host, const BrushToolSettings& settings);
    // The key under (x, y), or false: its path and index.
    bool keyAt(StrokeHost& host, const ToolView& view, float x, float y, int* path, int* key);
    // The path line under (x, y) between two keys, or false: its path and the key after the spot.
    bool segmentAt(StrokeHost& host, const ToolView& view, float x, float y, int* path, int* nextKey);
    void hover(StrokeHost& host, const ToolView& view, float x, float y);  // the key under the cursor drawn so

    // The paths as edited (loaded from the guides on first use, after Apply and after Cancel).
    const LengthPathSet& paths(StrokeHost& host);
    bool edited() const { return edited_; }  // changes not yet applied
    int activePath() const { return activePath_; }
    int activeKey() const { return activeKey_; }  // in the active path; -1: none
    void select(StrokeHost& host, int path, int key);
    // The active key's length and reach, the active path's smoothness (none active: nothing).
    void setKeyLength(StrokeHost& host, const BrushToolSettings& settings, float length);
    void setReach(StrokeHost& host, const BrushToolSettings& settings, float reach);
    void setSmooth(StrokeHost& host, const BrushToolSettings& settings, bool smooth);
    void setChange(StrokeHost& host, const BrushToolSettings& settings, LengthChange change);
    void newPath();  // the next click starts a path
    void deleteKey(StrokeHost& host, const BrushToolSettings& settings);
    void deletePath(StrokeHost& host, const BrushToolSettings& settings);

    // Commits the guides at their new lengths and the paths (one undo step); false when nothing
    // was to apply. Cancel drops the edits: the stored paths again, the preview off.
    bool apply(StrokeHost& host, const BrushToolSettings& settings);
    void cancel(StrokeHost& host);
    // Forgets the loaded paths (the groom changed: another one, an undo): read again next time.
    void reset(StrokeHost* host);

    // What the host draws: each path as a line through its keys (on the scalp as shown), each key
    // with its length and reach (hosts ring the active path's keys that wide), the active and
    // hovered one marked. With symmetry on, the mirrored paths too (`mirror`: drawn fainter; they
    // follow their keys and take no clicks).
    struct Overlay {
        struct Key {
            Vec3f at;
            float length = 0.0f;
            float reach = 0.0f;
            int path = 0;
            bool active = false;
            bool hovered = false;
            bool mirror = false;
        };
        std::vector<std::vector<Vec3f>> lines;
        std::vector<bool> mirrorLines;  // per line: a mirrored path's
        std::vector<Key> keys;
        int activePath = -1;
    };
    Overlay overlay(StrokeHost& host, const BrushToolSettings& settings);
    // The overlay as colored lines, for hosts that draw lines only (the app, Blender): the paths,
    // a dot round each key (two rings: the selected one larger, yellow; the hovered one white),
    // the active path's keys ringed as far as each reaches, symmetry's mirrored ones darker. The
    // key under (x, y) is hovered first. Rings face the camera.
    struct ColoredLine {
        std::vector<Vec3f> points;
        std::array<float, 3> color{1.0f, 1.0f, 1.0f};
    };
    std::vector<ColoredLine> overlayLines(StrokeHost& host, const ToolView& view, const BrushToolSettings& settings, float x,
                                          float y);

private:
    void load(StrokeHost& host);
    // The guides it changes now (selection, else the brush's targets).
    std::vector<GuideId> targets(StrokeHost& host, const BrushToolSettings& settings, const GuideHierarchy& h) const;
    LengthPathSet acting(StrokeHost& host, const BrushToolSettings& settings) const;  // with symmetry's mirror
    std::shared_ptr<GuideHierarchy> result(StrokeHost& host, const BrushToolSettings& settings);
    void preview(StrokeHost& host, const BrushToolSettings& settings);
    bool scalpPoint(StrokeHost& host, const ToolView& view, float x, float y, LengthKey& key) const;

    LengthPathSet paths_;
    bool loaded_ = false, edited_ = false;
    int activePath_ = -1, activeKey_ = -1;
    int hoverPath_ = -1, hoverKey_ = -1;
    bool startNew_ = false;
    // A press: dragging a key's length (from `startLength_` at `startY_`) or moving a new key.
    enum class Drag { None, Length, Move, Reach } drag_ = Drag::None;
    float startY_ = 0.0f, startLength_ = 0.0f;
    float keyX_ = 0.0f, keyY_ = 0.0f;  // Reach: the key's pixel
};

}  // namespace groomlab
