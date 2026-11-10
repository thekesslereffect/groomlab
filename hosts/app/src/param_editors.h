// SPDX-License-Identifier: Apache-2.0
#pragma once

// The editors a node's params need beyond a spin box: a ramp (a curve over 0..1 with draggable
// keys, keys added and removed, smooth or straight segments, presets), a color (a swatch with a
// picker, shown in sRGB and stored linear as the core's "color" vec3 params are) and a map
// source for a mappable param (a constant, a painted map by name, a texture file, a color set,
// an expression, or a field port on the node the graph wires), each holding its value as the
// graph JSON the document stores. The panel (param_panel.h) wires them to the document.

#include <functional>
#include <string>
#include <vector>

#include <QColor>
#include <QDoubleSpinBox>
#include <QString>
#include <QWidget>

#include <nlohmann/json.hpp>

class QComboBox;
class QLabel;
class QLineEdit;
class QStackedWidget;

namespace groomlab::app {

class RoundButton;

// ---- TrimmedSpinBox -------------------------------------------------------------------------------
// A spin box showing its number as the sliders do (trimmedNumber: the decimals at most, trailing
// zeros dropped, 0.35 not 0.350); typing takes any of those decimals.
class TrimmedSpinBox : public QDoubleSpinBox {
    Q_OBJECT
public:
    using QDoubleSpinBox::QDoubleSpinBox;
    QString textFromValue(double value) const override;
};

// ---- RampEditor ---------------------------------------------------------------------------------
// The curve as the core evaluates it (ramp.h: monotone cubic, or straight segments), over
// t in 0..1 and a vertical range that fits the keys (at least 0..1). A drag moves a key (a
// live edit each move, one recorded edit on release), a double click on the curve adds one,
// Delete or a double click on a key removes it (one key stays), the buttons switch smooth
// and straight and pick a preset.
class RampEditor : public QWidget {
    Q_OBJECT
public:
    explicit RampEditor(QWidget* parent = nullptr);
    // [[t, v], ...] or {"keys": [[t, v], ...], "linear": true}; anything else: one key at 1.
    void setValue(const nlohmann::json& value);  // no signal
    nlohmann::json value() const;
    bool straight() const { return straight_; }
    std::size_t keyCount() const { return keys_.size(); }
    // The presets by name (engine/ui/choices.json's rampPresets, as every host offers them): what
    // the preset menu offers, and a preset's keys (one key at 1 for a name there is none of).
    static std::vector<std::string> presetNames();
    static nlohmann::json preset(const std::string& name);
    // A ramp value mirrored end to end (each key at 1 - its position), straight or smooth as it was.
    static nlohmann::json flipped(const nlohmann::json& value);
    QSize sizeHint() const override;

signals:
    // `live` while a key drags (the host previews); false for an edit to record.
    void edited(bool live);

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;

private:
    QRectF plotRect() const;
    QPointF toScreen(float t, float v) const;
    std::pair<float, float> fromScreen(const QPointF& p) const;
    int keyAt(const QPointF& p) const;  // -1 none within reach
    void fitRange();
    std::vector<std::pair<float, float>> keys_;
    bool straight_ = false;
    float lo_ = 0.0f, hi_ = 1.0f;  // the vertical range shown
    int selected_ = -1;
    bool dragging_ = false;
    RoundButton* smooth_ = nullptr;
    RoundButton* linear_ = nullptr;
    RoundButton* presets_ = nullptr;
};

// ---- ColorEditor --------------------------------------------------------------------------------
// A swatch (the color as seen) with a hex field and a picker (QColorDialog, non-modal, live
// while it is open); the value is the linear RGB the graph stores.
class ColorEditor : public QWidget {
    Q_OBJECT
public:
    explicit ColorEditor(QWidget* parent = nullptr);
    void setLinear(const std::array<float, 3>& rgb);  // no signal
    std::array<float, 3> linear() const { return linear_; }
    QColor shown() const;  // the sRGB color of the swatch
    // A value shown, not edited (Hair Color's Result): readable (muted, not the disabled tone).
    void setReadOnly(bool readOnly);

signals:
    void edited(bool live);

private:
    void refresh();
    void setShown(const QColor& color, bool live);
    std::array<float, 3> linear_{1.0f, 1.0f, 1.0f};
    RoundButton* swatch_ = nullptr;
    QLineEdit* hex_ = nullptr;
    bool updating_ = false;
};

// ---- MapSourceEditor ----------------------------------------------------------------------------
// A mappable param: where its value comes from. Constant shows the ordinary editor the panel
// made (a spin box and slider); the map sources show their own fields (the map or file, the
// component read of a color map, the range the map's 0..1 maps onto); Port exposes the param as
// an input the graph wires (and says what feeds it). Switching sources never loses the map set
// before: it is kept and comes back when its source is chosen again.
class MapSourceEditor : public QWidget {
    Q_OBJECT
public:
    enum class Source { Constant, Painted, Texture, ColorSet, Expression, Port };

    struct Choices {
        // The groom's painted maps (name, kind: vertices, texture or guides, a texture map's
        // stored file) and the scalp's other color sets.
        struct Map {
            std::string name, kind, texture;
            int channels = 1;
        };
        std::vector<Map> maps;
        std::vector<std::string> colorSets;
    };

    // `constant` is the panel's editor for a plain value (owned here from now on).
    explicit MapSourceEditor(QWidget* constant, QWidget* parent = nullptr);
    void setChoices(const Choices& choices);
    // The param's value as stored (a number, or a map object), whether it is exposed as a port
    // and what feeds it ("" nothing). No signal.
    void setValue(const nlohmann::json& value, bool exposed, const std::string& fedBy);
    // The value the editor shows: a number for Constant (the panel reads its own editor then;
    // this gives the last number seen) or the map object.
    nlohmann::json value() const;
    Source source() const { return source_; }
    QWidget* constantEditor() const { return constant_; }
    // Picks a source as the combo does (the signals follow): tests and scripted runs.
    void chooseSource(Source source);

signals:
    // The map value changed (value() has it); the host records an edit.
    void mapEdited();
    // The source switched to or from Constant: the host writes value() (or its own number).
    void sourceChanged(Source source);
    // Port chosen or left: the host exposes or hides the param's port.
    void portRequested(bool exposed);

private:
    void refreshFields();
    nlohmann::json mapJson() const;
    Source sourceOfMap(const nlohmann::json& map) const;
    std::string componentName() const;
    QWidget* constant_;
    Choices choices_;
    Source source_ = Source::Constant;
    nlohmann::json lastMap_;      // the map set before, kept across source switches
    nlohmann::json lastConstant_;  // the number before a map was chosen
    bool exposed_ = false;
    QComboBox* sourceBox_ = nullptr;
    QStackedWidget* pages_ = nullptr;
    QComboBox* painted_ = nullptr;
    QLineEdit* texture_ = nullptr;
    QComboBox* colorSet_ = nullptr;
    QLineEdit* expression_ = nullptr;
    QLabel* port_ = nullptr;
    QWidget* rangeRow_ = nullptr;
    QComboBox* component_ = nullptr;
    QDoubleSpinBox* min_ = nullptr;
    QDoubleSpinBox* max_ = nullptr;
    bool updating_ = false;
};

}  // namespace groomlab::app
