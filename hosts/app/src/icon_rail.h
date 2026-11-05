// SPDX-License-Identifier: Apache-2.0
// IconRail: a slim column of page icons on a side dock's outer edge (Blender's Properties editor,
// VS Code's activity bar). An icon opens its page in the dock; the open page's icon again folds
// the dock down to the rail (the viewport takes the room), any icon opens it again. Its sizes and
// tones are components.iconRail (hosts/maya/themes/charcoal.json), so the hosts draw the
// same rail; folding is app chrome (in Maya and Blender the rail only switches pages).
#pragma once

#include <vector>

#include <QString>
#include <QWidget>

class QToolButton;
class QVBoxLayout;

namespace groomlab::app {

class IconRail : public QWidget {
    Q_OBJECT
public:
    enum class Side { Left, Right };  // the dock's side: the rail on its outer edge

    explicit IconRail(Side side, QWidget* parent = nullptr);
    static int railWidth();  // its width (components.iconRail.width)

    // A page: its icon (a resource path), its name (the tooltip).
    QToolButton* addPage(const QString& icon, const QString& name, const QString& objectName);
    int count() const { return static_cast<int>(buttons_.size()); }
    QToolButton* button(int index) const;
    void setPageKey(int index, const QString& key);  // its shortcut, shown in the tooltip ("Ctrl+1")
    void setFoldKey(const QString& key);              // the fold's shortcut, in the open page's tooltip

    void setCurrent(int index);  // the open page's icon lit (no signal)
    int current() const { return current_; }
    void setCollapsed(bool collapsed);  // folded: no icon lit, a mark at the page that opens again (no signal)
    bool collapsed() const { return collapsed_; }
    void setSide(Side side);  // the dock moved to the other side: the edge rule on the dock's side
    Side side() const { return side_; }
    void refreshIcons();  // drawn again in the theme's tones (and at the screen's pixel ratio)

signals:
    void picked(int index);                 // an icon clicked: open its page
    void collapseRequested(bool collapse);  // the open page's icon clicked: fold or unfold the dock

protected:
    void paintEvent(QPaintEvent* event) override;
    bool event(QEvent* event) override;

private:
    void refreshChecks();
    Side side_;
    QVBoxLayout* column_ = nullptr;
    std::vector<QToolButton*> buttons_;
    std::vector<QString> icons_, names_, keys_;
    QString foldKey_;
    int current_ = 0;
    bool collapsed_ = false;
};

}  // namespace groomlab::app
