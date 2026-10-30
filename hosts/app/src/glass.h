// SPDX-License-Identifier: Apache-2.0
#pragma once

// The painted widgets the app's UI is made of, where a style sheet is not enough: glass panels
// (a translucent rounded rectangle over the viewport, or nothing at all docked), round and pill
// buttons with tinted icons, number fields filled by their value, the segmented mode
// switcher, and text drawn straight on the viewport with a shadow. Every widget paints with alpha and no background of
// its own, so whatever is under it shows: a true blur of the 3D frame is not possible for
// widgets over a QRhiWidget (they cannot sample its texture), so the glass is translucency alone.
//
// Their cost: while the viewport alone changes, Qt composes its texture under the widgets as they
// were; once any of these widgets changes, Qt paints every one of them again and uploads the
// window's widgets whole (about 10 ms a frame at 1440p). So the setters change nothing when given
// what a widget already shows, and what follows the frames (the stats line) is throttled.

#include <functional>
#include <vector>

#include <QAbstractButton>
#include <QColor>
#include <QPixmap>
#include <QScrollArea>
#include <QString>
#include <QWidget>

class QLineEdit;

namespace groomlab::app {

// An icon from the resources (hosts/app/icons/*.svg) tinted to one color (its own colors become
// alpha), `size` pixels square at the device pixel ratio.
QPixmap tintedIcon(const QString& resource, const QColor& color, int size, qreal devicePixelRatio);
// The icon as drawn (the three-strand logo keeps its colors).
QPixmap coloredIcon(const QString& resource, int size, qreal devicePixelRatio);
// A tool icon in two tones, as the Maya host recolors them (widgets.icon_svg): the SVG's
// placeholder colors (tools/make_icons.py) replaced, its main color (and a darker shade and a
// lighter tint of it) by `main`, the hair and neutral parts by `ink`, the palette's by the theme's.
// The icon as drawn when its SVG cannot be read so.
QPixmap duotoneIcon(const QString& resource, const QColor& main, const QColor& ink, int size, qreal devicePixelRatio);

// A folding section's header button (the Groom Tools', the Properties dock's, a node's settings):
// its title in capitals, spaced out a little, after a chevron (down: open, right: folded) in the
// muted text color; the style sheet draws the small muted text and the hairline above it.
void setSectionHeader(QAbstractButton* header, const QString& title, bool expanded);

// A number with at most `decimals` decimals and its trailing zeros dropped ("0.35", not "0.350";
// "1", not "1.000"), a small one with two significant digits ("0.035", up to six decimals), as
// every host shows a param (docs/APP_PARITY.md, Glossary: Numbers).
QString trimmedNumber(double value, int decimals);

// A tooltip with the keys of shortcut `id` from the one shortcut table (groomlab/shortcuts.h, the
// app's rows) after it: "Frame the groom (F)"; the tip alone when the app has no such row.
QString withShortcut(const QString& tip, const char* id);

// Fills a rounded rectangle with the glass color (or another).
void paintGlass(QPainter& painter, const QRectF& rect, qreal radius, const QColor& color, const QColor& edge = QColor());

// ---- GlassPanel -------------------------------------------------------------------------------------
class GlassPanel : public QWidget {
    Q_OBJECT
public:
    explicit GlassPanel(QWidget* parent = nullptr, qreal radius = 24.0);
    void setRadius(qreal radius);
    void setColor(const QColor& color);  // the glass by default
    QColor color() const { return color_; }
    // Docked (a dock's section, app_shell.h): nothing painted, the dock's solid panel shows
    // through; its fixed widths are lifted so the dock sizes it.
    void setDocked(bool docked);
    bool docked() const { return docked_; }

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    qreal radius_;
    QColor color_;
    bool docked_ = false;
};

// ---- Windows on small screens -------------------------------------------------------------------
// The usable area of the screen `widget` is on (else the primary screen's); empty without a screen.
QRect availableGeometry(const QWidget* widget);
// Resizes a top-level window to theme::fitToAvailable(wanted, its screen) and centers it there.
void fitWindowToScreen(QWidget* window, QSize wanted);

// ---- FitScrollArea ------------------------------------------------------------------------------
// A panel's content that scrolls when the panel is placed shorter than the content (a small
// screen, or 125-150% text): frameless, see-through, no horizontal bar. Its size hint is the
// content's whole (a plain QScrollArea caps it), so a panel sized to its hint shows everything
// where there is room.
class FitScrollArea : public QScrollArea {
    Q_OBJECT
public:
    explicit FitScrollArea(QWidget* content, QWidget* parent = nullptr);
    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;
};

// ---- RoundButton --------------------------------------------------------------------------------
// A circle (icon alone) or a pill (text, with or without an icon): a fill at rest, another when
// checked, brighter under the mouse; the icon tinted by a color (and another when checked).
class RoundButton : public QAbstractButton {
    Q_OBJECT
public:
    explicit RoundButton(QWidget* parent = nullptr);
    void setIcon(const QString& resource, int size);  // the resource path of an SVG
    void setIconColor(const QColor& rest, const QColor& checked);
    void setFill(const QColor& rest, const QColor& checked);
    void setTextColor(const QColor& rest, const QColor& checked);
    void setDiameter(int diameter);  // the circle's size (a pill's height)
    void setPadding(int padding);    // a pill's horizontal padding
    void setFontWeight(int weight);
    void setFontSize(int pixelSize);
    // A pill's text may hold a dim part after the name ("Comb" and "C"): drawn in the muted color.
    void setDimText(const QString& dim);
    QString dimText() const { return dim_; }
    // A pill whose icon keeps its own colors (the logo).
    void setIconColored(bool colored);
    // The corners: half the height (a circle or pill) unless set.
    void setRadius(qreal radius);
    // A ring around the button when checked (a swatch's selection).
    void setCheckedRing(const QColor& color, qreal width);
    // A tile: the circle (the diameter) at the left with a name beside it at `labelX`, faded by
    // `opacity` (the brush rail's rows as they widen). The whole widget is the hit target and
    // the hover covers it as one, under the circle and the name; "" goes back to a plain button.
    void setTileLabel(const QString& name, int labelX, qreal opacity);
    QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent* event) override;
    void enterEvent(QEnterEvent* event) override;
    void leaveEvent(QEvent* event) override;

private:
    QString icon_;
    int iconSize_ = 20;
    QColor iconRest_, iconChecked_;
    QColor fillRest_, fillChecked_;
    QColor textRest_, textChecked_;
    int diameter_ = 44;
    int padding_ = 20;
    int fontWeight_ = 700;
    int fontSize_ = 15;
    QString dim_;
    bool colored_ = false;
    bool hover_ = false;
    qreal radius_ = -1.0;
    QColor ring_;
    qreal ringWidth_ = 0.0;
    QString tileLabel_;
    int tileLabelX_ = 0;
    qreal tileOpacity_ = 0.0;
};

// ---- ValueSlider ----------------------------------------------------------------------------------
// A number typed or arithmetic ("0.5*2", "1/3", "-(2+1)"; "," as the decimal point): numbers,
// + - * /, parentheses and unary signs, nothing else (no names, no scripting). False (and *value
// untouched) for anything else, a division by zero included.
bool parseNumberExpression(const QString& text, double* value);

// A numeric param as one fill field (the design's FillField, Blender's and Maya's number fields):
// a rounded rect a row tall (the theme's raised tone, a 1 px edge, the control radius), the
// caption inside at the left (dim) and the value at the right (tabular digits, the format's unit),
// filled from the left by the value's place in the range (the theme's selection tone).
//   - A drag (past 3 px) scrubs relative to the press: the field's width is the range; Shift a
//     tenth of the speed, Ctrl steps of ten; the drag stays inside the range.
//   - A click (no drag) opens the inline edit with the number selected: Enter, Tab or clicking
//     away sets it, Escape puts the old one back; it takes arithmetic (parseNumberExpression). A
//     value typed past the range widens the range (within the limits), as Maya's soft ranges do.
//   - Focused (a click or Tab): Enter or F2 types, the arrows nudge one step (Shift: ten); the
//     wheel steps it only while focused, so it never steals the panel's scrolling.
// A state color (a driven param) tints the field and its edge; disabled, it is drawn faded.
class ValueSlider : public QWidget {
    Q_OBJECT
public:
    explicit ValueSlider(QWidget* parent = nullptr);
    void setRange(double min, double max);   // the fill's span, the drag's (a typed value may widen it)
    void setLimits(double min, double max);  // what a typed value may reach (unbounded by default)
    void setValue(double value);  // clamped to the limits (the range widens to show it); no signal
    double value() const { return value_; }
    QString shownText() const { return format_(value_); }  // the value as drawn (its format)
    double minimum() const { return min_; }
    double maximum() const { return max_; }
    void setFill(const QColor& color);  // a glass theme's fill (a solid theme fills with ui.selection)
    QColor fillColor() const;           // the fill as drawn
    void setTrackWidth(int width);  // the field's preferred width (0: what its texts need)
    void setValueWidth(int width);  // the room the value's text keeps at least
    void setFormat(std::function<QString(double)> format);
    void setDecimals(int decimals);  // the default format's and the inline edit's precision
    void setStep(double step);       // the arrows' and the wheel's step, what a drag snaps to (0: a 100th of the range, rounded)
    double step() const;             // as used
    void setInteger(bool integer);   // whole values only (the step at least 1)
    // The name inside the field at the left ("Radius"), dim.
    void setCaption(const QString& caption);
    QString caption() const { return caption_; }
    // A param's state shown on the field (Maya's keyed, animated or driven colors: ui.keyed,
    // ui.animated, ui.driven); invalid: none.
    void setStateColor(const QColor& color);
    QColor stateColor() const { return state_; }
    bool editing() const;  // the inline edit is open
    void beginEdit();      // opens the inline edit (a click, Enter, F2), the number selected
    // Enter: the text parsed and set as one change (pressed, valueChanged, released); false
    // when the text is no number (the edit stays open).
    bool commitEdit();
    void cancelEdit();         // Escape: closed, the value as it was
    QString editText() const;  // what the inline edit holds ("" when closed)
    void setEditText(const QString& text);
    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

signals:
    // A drag's or a step's start, each change, its end; a click that only opens the edit sends none.
    void valueChanged(double value);
    void pressed();
    void released();

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void focusInEvent(QFocusEvent* event) override;
    void focusOutEvent(QFocusEvent* event) override;
    void enterEvent(QEnterEvent* event) override;
    void leaveEvent(QEvent* event) override;
    void changeEvent(QEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;  // the inline edit's keys and focus

private:
    int captionAdvance() const;
    int valueAdvance() const;
    double snapped(double value) const;  // to the limits, and to whole numbers for an integer slider
    void setLive(double value);          // a step of a drag: signalled when it changed
    void stepBy(double steps);           // an arrow or the wheel: one change of its own
    void placeEditor();
    void closeEditor();
    double min_ = 0.0, max_ = 1.0, value_ = 0.5, step_ = 0.0;
    double lowLimit_ = -1e300, highLimit_ = 1e300;
    int decimals_ = 2;
    bool integer_ = false;
    QColor fill_;
    QColor state_;
    int trackWidth_ = 0, valueWidth_ = 0;
    QString caption_;
    std::function<QString(double)> format_;
    bool dragging_ = false;
    bool moved_ = false;  // the press went past the threshold (a drag, not a click)
    qreal pressX_ = 0.0, lastX_ = 0.0;
    double raw_ = 0.0;    // the drag's value before snapping (small moves add up)
    bool hover_ = false;
    bool keyFocus_ = false;  // focused by the keyboard (the edge shows it)
    bool closing_ = false;   // the edit closing (its focus-out is not a commit)
    QLineEdit* editor_ = nullptr;
};

// ---- Segmented ------------------------------------------------------------------------------------
// A row of pills in one glass pill: the current one filled with its own color, the others
// plain text. The mode switcher and the look card's tabs.
class Segmented : public QWidget {
    Q_OBJECT
public:
    explicit Segmented(QWidget* parent = nullptr);
    struct Item {
        QString label;
        QColor fill;
        QColor text;
        QString tip;  // the segment's own tooltip (its key, say)
    };
    void setItems(std::vector<Item> items);
    const std::vector<Item>& items() const { return items_; }
    void setCurrent(int index);  // no signal
    int current() const { return current_; }
    void setBackground(const QColor& color);  // the glass by default; the chip tone inside a panel
    void setSegmentHeight(int height);
    void setPadding(int padding);  // around the segments
    void setSegmentPadding(int padding);
    void setFontSize(int pixelSize);
    void setStretch(bool stretch);  // the segments share the width equally
    QSize sizeHint() const override;
    QSize minimumSizeHint() const override { return QSize(48, sizeHint().height()); }

signals:
    void changed(int index);  // another segment chosen
    void clicked(int index);  // any segment pressed, the current one too (before `changed`)

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    bool event(QEvent* event) override;  // the tooltip of the segment under the mouse

private:
    std::vector<QRect> segmentRects() const;
    std::vector<Item> items_;
    int current_ = 0;
    QColor background_;
    int segmentHeight_ = 40, padding_ = 4, segmentPadding_ = 20, fontSize_ = 14;
    bool stretch_ = false;
};

// ---- ShadowText -----------------------------------------------------------------------------------
// Words and numbers drawn straight on the viewport with a soft shadow, as runs: a run has its
// own font, color, an optional leading dot and an optional pill behind it (the GPU badge).
class ShadowText : public QWidget {
    Q_OBJECT
public:
    explicit ShadowText(QWidget* parent = nullptr);
    struct Run {
        QString text;
        QFont font;
        QColor color;
        QColor dot;    // a colored dot before the text (invalid: none)
        QColor pill;   // a pill behind the text (invalid: none)
        int gapBefore = 0;
        bool operator==(const Run& o) const {
            return text == o.text && font == o.font && color == o.color && dot == o.dot && pill == o.pill && gapBefore == o.gapBefore;
        }
    };
    // The same runs as shown change nothing (a stats line set every frame repaints nothing);
    // false then, true when they changed.
    bool setRuns(std::vector<Run> runs);
    const std::vector<Run>& runs() const { return runs_; }
    void setShadow(bool shadow);
    QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    std::vector<Run> runs_;
    bool shadow_ = true;
};

}  // namespace groomlab::app
