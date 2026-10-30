// SPDX-License-Identifier: Apache-2.0
#pragma once

// The Properties dock's Groom workspace, as Maya's Attribute Editor for the groom lays it out, from
// the shared UI definitions (engine/ui/properties.json through definitions.h): the sections in
// Maya's order (Groom, Inputs, Controls, Hair Color, Display > Guides / Strand Shading, Sculpting,
// Game Cards, Rendering > Render Overrides / Level of Detail), each a header that folds it away
// and rows of a label (the item's, for the app) and a control, the item's tip on both.
//
// The window makes the controls (MainWindow::buildPanels: what each one edits) and hands each to
// the panel by its item's app key; a custom block (Inputs, Controls, Hair Color: rows of their own)
// comes whole. An item the app gives no control is hidden (as an unbound action is), a section
// left with nothing shown too; tests/hosts/app/test_definitions.cpp lists the items the app does
// not do yet (ALLOWED_MISSING_PROPERTIES).
//
//     auto* props = new PropertiesPanel();
//     props->setControl("density", densitySlider);   // previewDensity's row: "Preview Density"
//     props->setBlock("inputs", inputsBlock);        // the Inputs custom block
//     props->finish();                               // what has no control: hidden

#include <map>
#include <string>
#include <vector>

#include <QString>
#include <QWidget>

#include "definitions.h"

class QGridLayout;
class QLabel;
class QPushButton;
class QScrollArea;
class QVBoxLayout;

namespace groomlab::app {

// A section of the panel: a header (a chevron, the title) that folds its rows away.
class PropertySection : public QWidget {
    Q_OBJECT
public:
    PropertySection(const QString& title, bool nested, QWidget* parent = nullptr);
    void setExpanded(bool expanded);
    bool expanded() const { return expanded_; }
    QPushButton* header() const { return header_; }
    QWidget* body() const { return body_; }
    QGridLayout* grid() const { return grid_; }

signals:
    void toggled(bool expanded);

private:
    QString title_;
    QPushButton* header_ = nullptr;
    QWidget* body_ = nullptr;
    QGridLayout* grid_ = nullptr;
    bool expanded_ = false;
};

class PropertiesPanel : public QWidget {
    Q_OBJECT
public:
    // An item as the app shows it: its id, its app key (the control's name), its label and tip
    // for the app, its section, and its parts (a custom block's rows) as written.
    struct Item {
        std::string id, key, label, tip, section;
        bool custom = false;
        ui::json parts;
    };
    // The items properties.json shows in the app, in order (hosts without "app" left out).
    static std::vector<Item> appItems(const ui::Definitions& definitions = ui::Definitions::shipped());
    // A custom block's part by id ("scalp" of "inputs"), its label and tip for the app ("" none).
    static QString partLabel(const std::string& key, const std::string& part);
    static QString partTip(const std::string& key, const std::string& part);

    explicit PropertiesPanel(QWidget* parent = nullptr);

    // The control of the item whose app key is `key`, in its row (its label at left, the item's tip
    // on both unless the control has its own); a number field (ValueSlider) takes the whole row with
    // the label as its caption, inside it. False when no item has that key.
    bool setControl(const std::string& key, QWidget* control);
    // A custom block's widget, spanning its row (no label column).
    bool setBlock(const std::string& key, QWidget* block);
    // The items without a control hidden, the sections left empty too. Once, after the controls.
    void finish();

    // ---- What it shows (tests, the window) ------------------------------------------------------
    PropertySection* section(const std::string& id) const;
    std::vector<PropertySection*> sections() const;  // every one, nested ones after their parent
    QWidget* control(const std::string& key) const;  // null: none
    QLabel* label(const std::string& key) const;     // null: none (a block)
    bool shown(const std::string& key) const;        // an item with a control, its row not hidden
    std::vector<std::string> boundKeys() const;      // the keys with a control, in order
    // The row of an item shown or hidden (an item that applies only now and then: Source with the
    // color modes that read one).
    void setRowVisible(const std::string& key, bool visible);
    void expandAll(bool expanded);  // every section open (a screenshot) or folded
    QScrollArea* scrollArea() const { return scroll_; }

private:
    struct Row {
        PropertySection* section = nullptr;
        int row = 0;
        QLabel* label = nullptr;
        QWidget* control = nullptr;
        QWidget* rule = nullptr;  // a divider row
        Item item;
        bool wanted = true;
        bool inField = false;  // the label is the number field's caption (its own stays hidden)
    };
    void build(const ui::json& sections, QVBoxLayout* into, PropertySection* parent);
    QScrollArea* scroll_ = nullptr;
    std::map<std::string, PropertySection*> sections_;
    std::vector<std::string> sectionOrder_;
    std::vector<Row> rows_;
    std::map<std::string, std::size_t> byKey_;
};

}  // namespace groomlab::app
