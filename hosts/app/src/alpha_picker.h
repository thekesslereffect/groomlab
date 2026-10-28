// SPDX-License-Identifier: Apache-2.0
#pragma once

// The Paint brush's alpha (brush_alpha.h) as Maya's Groom Tools pick it (widgets.py AlphaPicker):
// a thumbnail button and the alpha's name; the button opens a grid of the library (None first,
// the user's own, then the shipped ones) with Add Alpha... and Open Folder below it, and its
// right-click menu has Clear Alpha, Add Alpha... and Open Alpha Folder.

#include <string>

#include <QWidget>

class QLabel;
class QListWidget;
class QListWidgetItem;
class QMenu;
class QToolButton;

namespace groomlab::app {

class AlphaPicker : public QWidget {
    Q_OBJECT
public:
    explicit AlphaPicker(QWidget* parent = nullptr);

    const std::string& path() const { return path_; }
    // Shows the alpha ("" or a file gone: none); with `notify`, says so (changed).
    void setPath(const std::string& path, bool notify = true);
    // The library's grid filled again (as the popup opens).
    void fill();
    QListWidget* grid() const { return grid_; }
    // Picks a grid item, as a click does.
    void pick(QListWidgetItem* item);
    // A file added to the library (copied into the user's folder) and picked; "" or why not.
    std::string add(const std::string& file);

signals:
    void changed(const QString& path);

private:
    void showAlpha();
    void addFromDialog();
    void openFolder();

    std::string path_;
    QToolButton* button_ = nullptr;
    QLabel* name_ = nullptr;
    QMenu* popup_ = nullptr;
    QListWidget* grid_ = nullptr;
};

}  // namespace groomlab::app
