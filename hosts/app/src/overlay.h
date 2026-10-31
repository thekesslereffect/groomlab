// SPDX-License-Identifier: Apache-2.0
#pragma once

// The pieces the app's docked window is made of (app_shell.h places them) and what stays over the
// viewport. OverlayHost is the window's central widget: the viewport fills it, and the few things
// that belong on the picture float over it, placed by theme::layoutViewportOverlay:
//
//   top-left     the active tool's name ("Comb  B", Groom mode)
//   bottom-left  the stats (Groom: strands, the time per dab, the GPU or LINES badge)
//   bottom-right the refine line (Look: frames accumulated of the target)
//   bottom       a toast above them (an error tinted and longer, an info message brief)
//   center       a prompt (never a dialog), the empty-state card (no groom open: open, new,
//                a preset, the recent files); a .groom, .obj or .abc dropped opens
//
// The other piece here lives in a dock: LookCard (Look mode's material, lighting and render
// settings, in the Properties dock). The brush tiles and the active tool's card are the Groom
// Tools' (groom_tools.h).

#include <functional>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include <QColor>
#include <QString>
#include <QStringList>
#include <QWidget>

#include "glass.h"
#include "look.h"

class QBoxLayout;
class QCheckBox;
class QComboBox;
class QLabel;
class QMenu;
class QPushButton;
class QScrollArea;
class QTimer;
class QVariantAnimation;

namespace groomlab::app {

class ColorEditor;
class Viewport;

// ---- LookCard ------------------------------------------------------------------------------------
// Look mode's card, three tabs. Material: the hair color presets as swatches, melanin, redness
// and roughness, and behind "More settings" every aiStandardHair parameter (the pigment's
// source, randomize, the dye, the azimuthal roughness, IOR, shift, the lobes' weights and
// tints, diffuse, multiple scattering, the width scale and shadow density) and the surfaces
// (the scalp, the props, the eyes: a preset, the kind, its parameters with the skin's
// scattering controls). Lighting: the presets, each light's azimuth, elevation, intensity,
// color and shadows, the environment, its rotation and intensity, exposure, the tone map, the
// background. Render: the quality, the refine sample count, the resolution scale, the
// interactive quality (adaptive or full). The card
// scrolls when a tab is taller than the view; "All settings" opens the full look panel (the
// surfaces' texture files live there).
class LookCard : public GlassPanel {
    Q_OBJECT
public:
    enum class Tab { Material, Lighting, Render };

    explicit LookCard(QWidget* parent = nullptr);
    void setMaterial(const HairMaterial& material);  // no signals
    void setLighting(const LightingSetup& lighting);
    const HairMaterial& material() const { return material_; }
    const LightingSetup& lighting() const { return lighting_; }
    // The groom's own look: its last HairColor node's melanin, redness and roughness, which the
    // swatches and the main sliders edit (saved with the groom). Without a HairColor node (`node`
    // empty) they edit the viewport material instead, and say so.
    // HairColor's mode: the pigment (melanin, redness) or a picked RGB color (`picked`: mode color,
    // `color` rendered as it is); its tip color and how far the tips go to it, in either mode.
    struct GroomColor {
        std::string node;
        float melanin = 0.35f, redness = 0.2f, roughness = 0.5f;
        bool picked = false;
        RGB color{0.8f, 0.8f, 0.8f};
        RGB tipColor{0.55f, 0.42f, 0.28f};
        float tipStrength = 0.0f;
        float hueRandom = 0.0f, saturationRandom = 0.0f;  // each strand's hue and saturation spread
    };
    void setGroomColor(const GroomColor& color);  // no signals
    const GroomColor& groomColor() const { return groomColor_; }
    // The pieces (tests): the main sliders and the swatches (0 the groom's own pigment, then
    // black, brown, blonde, red, white, dyed).
    ValueSlider* melaninSlider() const { return melanin_; }
    ValueSlider* rednessSlider() const { return redness_; }
    ValueSlider* roughnessSlider() const { return roughness_; }
    Segmented* groomModeSwitch() const { return groomMode_; }  // 0 natural (melanin), 1 a picked color
    ColorEditor* groomColorEditor() const { return groomPicked_; }
    ColorEditor* tipColorEditor() const { return tipColor_; }
    ValueSlider* tipStrengthSlider() const { return tipStrength_; }
    ValueSlider* hueRandomSlider() const { return hueRandom_; }
    ValueSlider* saturationRandomSlider() const { return saturationRandom_; }
    RoundButton* swatch(int index) const { return swatches_.at(static_cast<std::size_t>(index)); }
    QString colorNote() const;
    // The surfaces to edit (the scalp first); each edit comes back with the surface's index.
    void setSurfaces(const std::vector<std::pair<QString, SurfaceMaterial>>& surfaces);
    const SurfaceMaterial* surfaceMaterial(int index) const;
    void setTab(Tab tab);
    Tab tab() const { return static_cast<Tab>(tabs_->current()); }
    // Its own Material / Lighting / Render switch shown (off: the Properties dock's tabs pick).
    void setTabsShown(bool shown);
    void setMoreShown(bool shown);  // the material's full settings and the surfaces
    bool moreShown() const { return moreShown_; }
    // The card's height for its content, at most `maxHeight` (the rest scrolls).
    void setMaxHeight(int maxHeight);
    QSize sizeHint() const override;

signals:
    void materialEdited();
    void lightingEdited();
    void surfaceEdited(int index, const SurfaceMaterial& material);
    void moreRequested();  // the full look panel
    // The groom's HairColor node edited: one param from a slider (`live` while it drags; the
    // drag's end comes with live false), or a swatch's melanin, redness and roughness together.
    void groomColorEdited(const std::string& param, double value, bool live);
    void groomSwatchChosen(const std::string& preset);
    void groomModeChosen(const std::string& mode);  // HairColor's mode: melanin or color
    // A color param of the groom's HairColor (color, tipColor) picked; `live` while it drags.
    void groomColorPicked(const std::string& param, const RGB& color, bool live);

private:
    enum class Target { Material, Lighting, Surface };
    void refreshGroomColor();
    struct LightRow {
        QWidget* host = nullptr;
        QCheckBox* enabled = nullptr;
        ValueSlider* azimuth = nullptr;
        ValueSlider* elevation = nullptr;
        ValueSlider* intensity = nullptr;
        ColorEditor* color = nullptr;
        QCheckBox* shadows = nullptr;
    };
    // A surface row shown for some kinds alone (bit 1 default, 2 skin, 4 eye).
    struct SurfaceRow {
        QWidget* widget = nullptr;
        int kinds = 7;
    };
    ValueSlider* slider(QBoxLayout* into, const QString& caption, const QColor& fill, double min, double max, int decimals,
                        std::function<double()> get, std::function<void(double)> set, Target target, int kinds = 7);
    ColorEditor* color(QBoxLayout* into, const QString& caption, std::function<RGB()> get, std::function<void(const RGB&)> set,
                       Target target, int kinds = 7);
    QLabel* caption(QBoxLayout* into, const QString& text, int kinds = 7, Target target = Target::Material);
    void edited(Target target);
    void refresh();
    void refreshSurfaceRows();
    SurfaceMaterial* currentSurface();
    HairMaterial material_;
    LightingSetup lighting_;
    GroomColor groomColor_;
    QLabel* colorNote_ = nullptr;  // "viewport only" without a HairColor node
    std::vector<std::pair<QString, SurfaceMaterial>> surfaces_;
    Segmented* tabs_ = nullptr;
    QScrollArea* scroll_ = nullptr;
    QWidget* pages_ = nullptr;
    QWidget* materialPage_ = nullptr;
    QWidget* lightingPage_ = nullptr;
    QWidget* renderPage_ = nullptr;
    QWidget* more_ = nullptr;
    RoundButton* moreButton_ = nullptr;
    bool moreShown_ = false;
    int maxHeight_ = 0;
    std::vector<RoundButton*> swatches_;
    std::vector<RoundButton*> lightingChips_;
    ValueSlider* melanin_ = nullptr;
    ValueSlider* redness_ = nullptr;
    ValueSlider* roughness_ = nullptr;
    Segmented* groomMode_ = nullptr;
    QWidget* groomPickedRow_ = nullptr;
    ColorEditor* groomPicked_ = nullptr;
    QWidget* tipColorRow_ = nullptr;
    ColorEditor* tipColor_ = nullptr;
    ValueSlider* tipStrength_ = nullptr;
    ValueSlider* hueRandom_ = nullptr;
    ValueSlider* saturationRandom_ = nullptr;
    QComboBox* pigment_ = nullptr;
    QComboBox* surface_ = nullptr;
    QComboBox* surfacePreset_ = nullptr;
    QComboBox* surfaceKind_ = nullptr;
    Segmented* sssMode_ = nullptr;
    std::vector<SurfaceRow> surfaceRows_;
    std::vector<LightRow> lightRows_;
    QComboBox* environment_ = nullptr;
    Segmented* toneMap_ = nullptr;
    Segmented* background_ = nullptr;
    QCheckBox* ambientOcclusion_ = nullptr;
    Segmented* quality_ = nullptr;
    Segmented* interactiveQuality_ = nullptr;
    std::vector<std::function<void()>> materialRefreshers_, lightingRefreshers_, surfaceRefreshers_;
    bool updating_ = false;
};

// ---- OverlayHost ---------------------------------------------------------------------------------
class OverlayHost : public QWidget {
    Q_OBJECT
public:
    // The window's modes (app_shell.h switches the docks; here: the stats, the refine line, the
    // empty card's sentence, the tool's name).
    enum class Mode { Groom, Look, Cards, Export };

    explicit OverlayHost(Viewport* viewport, QWidget* parent = nullptr);

    Viewport* viewport() const { return viewport_; }
    void setMode(Mode mode);  // no signal
    Mode mode() const { return mode_; }
    // A groom is open (its file name; "" none: the empty-state card shows in the middle).
    void setFileName(const QString& name, bool dirty);
    bool groomOpen() const { return !name_.isEmpty(); }
    // The empty-state card's recent files (at most five shown), newest first.
    void setRecentFiles(const QStringList& paths);
    GlassPanel* emptyCard() const { return emptyCard_; }
    // The card's sentence for the current mode (Groom: what to drop or pick; Look, Cards and
    // Export: what the mode does, once a groom is open).
    QString emptyText() const;
    int recentCount() const { return static_cast<int>(recentButtons_.size()); }
    // The active tool's name at the top-left ("Comb" with the key that brings it back), Groom mode.
    void setTool(const std::string& brushMode, bool select);
    QString toolText() const;  // "" when hidden
    // The stats line (Groom): strands, the guides (npos: not said), the time per dab (0: the
    // evaluation's), the path's badge.
    void setStats(std::size_t strands, double msPerDab, const QString& badge, const QColor& badgeColor,
                  std::size_t guides = static_cast<std::size_t>(-1));
    QString statsText() const;  // the stats line's words ("" hidden)
    // GroomLab > Review > Strand Count HUD: the stats line shown (on by default) or not.
    void setStatsWanted(bool on);
    bool statsWanted() const { return statsWanted_; }
    bool statsShown() const;
    // The refine line (Look): frames accumulated of the target, or the time per frame.
    void setRefine(bool refining, int frames, int target, double msPerFrame);
    // A message over the viewport for a few seconds (the stderr line stays the record): an error
    // tinted red and shown longer (its length's reading time), an info message (saved, opened,
    // a preset applied, a hint) plain and brief. Wrapped to the view's width; one timer, restarted
    // by each toast.
    void toast(const QString& text, bool error = true);
    bool toastShown() const;
    QString toastText() const;
    bool toastIsError() const { return toastError_; }
    int toastMilliseconds() const;  // the shown toast's time on screen
    // A question over the viewport in the app's own panel, never a dialog (the rest stays usable):
    // a title, a sentence and its choices left to right, the primary one filled; any choice
    // closes it, then runs its action. Another prompt replaces the shown one.
    struct PromptChoice {
        QString label;
        bool primary = false;
        std::function<void()> action;
    };
    void showPrompt(const QString& title, const QString& text, std::vector<PromptChoice> choices);
    // The same with a line to type in (`initial` filled, selected): Enter or `accept` hands the
    // text to `done`, Cancel or Escape drops it.
    void showTextPrompt(const QString& title, const QString& text, const QString& initial, const QString& accept,
                        std::function<void(const QString&)> done);
    // The same with several ways to take the line (New Group: top-level, or under the picked
    // group), left to right after Cancel; the last is the primary one (Enter).
    using TextChoice = std::pair<QString, std::function<void(const QString&)>>;
    void showTextPrompt(const QString& title, const QString& text, const QString& initial, std::vector<TextChoice> accepts);
    QString promptLine() const;  // the text prompt's line ("" without one)
    void setPromptLine(const QString& text);
    void hidePrompt();
    bool promptShown() const;
    GlassPanel* prompt() const { return prompt_; }
    // Clicks the shown prompt's choice by its label (tests, scripted runs); false when none is.
    bool choosePrompt(const QString& label);
    // The viewport's size follows the host at once, except while a mouse button is held (a dock's
    // or a splitter's drag resizes the host every move): then it catches up a moment after the
    // last move, so the QRhiWidget's texture is not made again for every pixel of the drag.
    bool viewportResizePending() const;
    // Undo and Redo at the top-right (the window's only buttons for them besides Edit and the keys):
    // enabled as the history allows, their tips the step's label and keys ("Undo comb stroke (Ctrl+Z)").
    void setUndoRedo(bool canUndo, bool canRedo, const QString& undoTip, const QString& redoTip);
    RoundButton* undoButton() const { return undo_; }
    RoundButton* redoButton() const { return redo_; }
    GlassPanel* history() const { return history_; }
    // A first groom, step by step (engine/ui palette.starter): a small card bottom-left over the
    // viewport, above the stats (Groom mode, a groom open, not dismissed, a step to go). Each step
    // ticks as the groom gets there; its x hides the card for good (starterDismissRequested: the
    // window keeps that). With no room for it (a view too short) it waits until there is.
    void setStarterProgress(bool guidesPlaced, bool lookGiven);
    void setStarterDismissed(bool dismissed);
    bool starterShown() const;
    GlassPanel* starterCard() const { return starter_; }

signals:
    // The empty-state card: Open groom, New groom on mesh, Start from a preset, a recent file; and
    // a .groom, .obj or .abc dropped on the window (any time).
    void openRequested();
    void newRequested();
    void presetsRequested();
    void fileDropped(const QString& path);
    void undoRequested();
    void redoRequested();
    void starterStepRequested(const QString& id);  // "guides" or "look"
    void starterDismissRequested();

protected:
    void resizeEvent(QResizeEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;  // Escape in the text prompt
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dropEvent(QDropEvent* event) override;

private:
    void relayout();
    void placeViewport();
    void refreshEmptyCard();
    void refreshToolLabel();
    void buildStarter();
    void refreshStarter();
    bool starterWanted() const;

    Viewport* viewport_;
    bool built_ = false;  // every piece exists: relayout may place them
    Mode mode_ = Mode::Groom;
    QString name_;
    bool dirty_ = false;
    QTimer* viewportTimer_ = nullptr;  // the viewport's deferred resize (placeViewport)
    // Top-left: the tool's name.
    GlassPanel* toolLabel_ = nullptr;
    QLabel* toolName_ = nullptr;
    std::string toolMode_;
    bool toolSelect_ = false;
    // Top-right: Undo and Redo.
    GlassPanel* history_ = nullptr;
    RoundButton* undo_ = nullptr;
    RoundButton* redo_ = nullptr;
    // The middle, with no groom open.
    GlassPanel* emptyCard_ = nullptr;
    QLabel* emptyHint_ = nullptr;  // emptyText
    QBoxLayout* recentLayout_ = nullptr;
    QLabel* recentCaption_ = nullptr;
    std::vector<RoundButton*> recentButtons_;
    QStringList recentFiles_;
    // Bottom.
    ShadowText* stats_ = nullptr;
    bool statsWanted_ = true;
    ShadowText* refine_ = nullptr;
    GlassPanel* toast_ = nullptr;
    QLabel* toastText_ = nullptr;
    QLabel* toastDot_ = nullptr;
    QTimer* toastTimer_ = nullptr;
    bool toastError_ = false;
    void sizeToast();
    // Bottom-left, above the stats: the first-groom steps.
    GlassPanel* starter_ = nullptr;
    std::map<std::string, QPushButton*> starterRows_;
    QLabel* starterTip_ = nullptr;
    bool guidesPlaced_ = false;
    bool lookGiven_ = false;
    bool starterDismissed_ = false;
    // The prompt (showPrompt): centered, over everything.
    GlassPanel* prompt_ = nullptr;
};

}  // namespace groomlab::app
