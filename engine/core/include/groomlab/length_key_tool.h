// SPDX-License-Identifier: Apache-2.0
#pragma once

// The Length Keys tool (length_field.h): keys clicked on the scalp, each dragged to scale the
// guides' lengths round it, the scale blending smoothly between keys over the scalp; the guides
// preview at their new lengths until Apply commits them (one undo step, the keys saved with the
// guides at x1 again) or Cancel drops the changes. The model and its mouse handling, the same in
// every host; hosts draw overlay() and give the panel's edits (setKeyScale, setKeyLength...) and
// Apply / Cancel.
//
// Mouse (port pixels, origin bottom left, as BrushTool's):
//   - left click on the scalp: a key there, at the scale the field already has there (placing a
//     key changes nothing; the first is x1: a hold).
//   - left press on a key (anywhere inside its ring, kPickPixels) selects it; dragging moves it
//     over the scalp (a new key too).
//   - the adjusting button (kStrokeAdjust) on a key's ring, or anywhere for the selected key: dragging up
//     scales it up, down scales it down (doubles every kPixelsPerDoubling pixels).
//   - Ctrl+click on a key deletes it.
// The guides it changes: the selected guides, else those the brush settings target (the level or
// group, not hidden or frozen: the soloed ones). With symmetry on the mirrored keys act too:
// across the world plane, or with topology symmetry each key at its partner spot on the scalp
// (ScalpMirror: right for a scalp posed or not quite symmetric in world space).

#include <array>
#include <memory>
#include <string>
#include <vector>

#include "groomlab/length_field.h"
#include "groomlab/math.h"

namespace groomlab {

class GuideHierarchy;
class StrokeHost;
struct BrushToolSettings;
struct ToolView;

class LengthKeyTool {
public:
    // A key's ring (drawn round it): a press anywhere inside takes the key (the nearest key's
    // centre when rings overlap).
    static constexpr float kPickPixels = 24.0f;
    static constexpr float kPixelsPerDoubling = 150.0f;  // a key's drag: its scale doubles per this

    LengthChange change = LengthChange::Scale;

    // Mouse events (BrushTool hands them on with Tool::Lengths). press returns false when it did
    // nothing (not on a key or the scalp; the host may say why).
    bool press(StrokeHost& host, const ToolView& view, const BrushToolSettings& settings, float x, float y, unsigned modifiers);
    void drag(StrokeHost& host, const ToolView& view, const BrushToolSettings& settings, float x, float y);
    void release(StrokeHost& host, const BrushToolSettings& settings);
    int keyAt(StrokeHost& host, const ToolView& view, float x, float y);  // the key under (x, y); -1: none
    void hover(StrokeHost& host, const ToolView& view, float x, float y);  // the key under the cursor drawn so

    // The keys as edited (loaded from the guides on first use, after Apply and after Cancel).
    const LengthKeySet& keys(StrokeHost& host);
    bool edited() const { return edited_; }  // changes not yet applied
    int activeKey() const { return activeKey_; }  // -1: none
    void select(StrokeHost& host, int key);
    // The length a key gives the guide nearest it: its scale times that guide's length now.
    float keyLength(StrokeHost& host, const BrushToolSettings& settings, int key);
    // The active key's scale, or its scale set so the guide nearest it takes `length`.
    void setKeyScale(StrokeHost& host, const BrushToolSettings& settings, float scale);
    void setKeyLength(StrokeHost& host, const BrushToolSettings& settings, float length);
    void setChange(StrokeHost& host, const BrushToolSettings& settings, LengthChange change);
    void deleteKey(StrokeHost& host, const BrushToolSettings& settings);
    void clear(StrokeHost& host, const BrushToolSettings& settings);  // every key

    // Commits the guides at their new lengths and the keys back at x1 (one undo step); false when
    // nothing was to apply. Cancel drops the edits: the stored keys again, the preview off.
    bool apply(StrokeHost& host, const BrushToolSettings& settings);
    void cancel(StrokeHost& host);
    // Forgets the loaded keys (the groom changed: another one, an undo): read again next time.
    void reset(StrokeHost* host);

    // What the host draws: each key (on the scalp as shown) with its scale and the length it gives
    // the guide nearest it, the active and hovered one marked. With symmetry on, the mirrored keys
    // too (`mirror`: drawn fainter; they follow their keys and take no clicks).
    struct Overlay {
        struct Key {
            Vec3f at;
            float scale = 1.0f;
            float length = 0.0f;
            bool active = false;
            bool hovered = false;
            bool mirror = false;
        };
        std::vector<Key> keys;
    };
    Overlay overlay(StrokeHost& host, const BrushToolSettings& settings);
    // The overlay as colored lines, for hosts that draw lines only (the app, Blender): each key a
    // dot in its ring (kPickPixels: where a press takes it; the selected one yellow, the hovered
    // one white, a key scaling up blue, down orange, a hold grey), symmetry's mirrored ones darker
    // and smaller (no ring: they take no clicks). The key under (x, y) is hovered first. Rings face
    // the camera.
    struct ColoredLine {
        std::vector<Vec3f> points;
        std::array<float, 3> color{1.0f, 1.0f, 1.0f};
    };
    std::vector<ColoredLine> overlayLines(StrokeHost& host, const ToolView& view, const BrushToolSettings& settings, float x,
                                          float y);
    // A key's color as overlayLines draws it (hosts drawing their own use it too).
    static std::array<float, 3> keyColor(const Overlay::Key& key);

private:
    void load(StrokeHost& host);
    // The guides it changes now (selection, else the brush's targets).
    std::vector<GuideId> targets(StrokeHost& host, const BrushToolSettings& settings, const GuideHierarchy& h) const;
    LengthKeySet acting(StrokeHost& host, const BrushToolSettings& settings) const;  // with symmetry's mirror
    std::shared_ptr<GuideHierarchy> result(StrokeHost& host, const BrushToolSettings& settings);
    void preview(StrokeHost& host, const BrushToolSettings& settings);
    bool scalpPoint(StrokeHost& host, const ToolView& view, float x, float y, LengthKey& key) const;

    LengthKeySet keys_;
    bool loaded_ = false, edited_ = false;
    int activeKey_ = -1, hoverKey_ = -1;
    // A press: dragging a key's scale (from `startScale_` at `startY_`) or moving a key.
    enum class Drag { None, Scale, Move } drag_ = Drag::None;
    float startY_ = 0.0f, startScale_ = 1.0f;
};

}  // namespace groomlab
