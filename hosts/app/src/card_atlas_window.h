// SPDX-License-Identifier: Apache-2.0
#pragma once

// GroomLab > Card Atlas...: hair card textures made block by block, as FiberShop makes them
// (card_blocks.h), a window of its own, never modal. The document's grooms that are blocks (their
// cardBlock) are the atlas: a new block is another groom of the document, on its own root band,
// and the document saved is the atlas (Maya's Card Atlas window opens the same file).
//
// Left, the atlas square with each block's rectangle (drag to move, its bottom-right corner to
// resize: placed by hand from then on; Auto Layout packs them all again) over the last preview
// bake of the map picked above it (Bake Preview). Right: the atlas' texture size, padding, map
// preset and name; its blocks (Add, Duplicate, Remove; picking one makes its groom the active one,
// so every panel, tool and the node graph work on it); the block's card (width, length, the root
// band's thickness) and the root band's Tilt, Turn and Roll (roots spread in depth: the texture
// reads with depth); and the exports (the maps as the preset packs them, the cards' shapes).

#include <array>
#include <vector>

#include <QImage>
#include <QString>
#include <QWidget>

#include "groomlab/card_blocks.h"

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QFrame;
class QSpinBox;

namespace groomlab::app {

class MainWindow;
class ValueSlider;

// The atlas square: the preview under the blocks' rectangles, dragged and resized with the mouse.
class AtlasCanvas : public QWidget {
    Q_OBJECT
public:
    explicit AtlasCanvas(QWidget* parent = nullptr);
    void setBlocks(const std::vector<std::array<float, 4>>& cells, const std::vector<QString>& names, int selected);
    void setImage(QImage image);
    const QImage& image() const { return image_; }
    QRectF square() const;  // the atlas' square in the widget
    QRectF rectOf(const std::array<float, 4>& cell) const;
    std::array<float, 4> cellOf(const QRectF& rect) const;
    QSize sizeHint() const override { return {520, 520}; }

signals:
    void picked(int block);
    void moved(int block, std::array<float, 4> cell);

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;

private:
    std::vector<std::array<float, 4>> cells_;
    std::vector<QString> names_;
    int selected_ = -1;
    QImage image_;
    int dragging_ = -1;
    bool resizing_ = false;
    QPointF dragStart_;
    QRectF dragRect_;
};

class CardAtlasWindow : public QWidget {
    Q_OBJECT
public:
    explicit CardAtlasWindow(MainWindow* main, QWidget* parent = nullptr);

    // The atlas the document's grooms make (their blocks, in the document's order) and, by block,
    // the groom it is (an index of MainWindow::groomAt).
    CardBlockAtlas atlas() const;
    std::vector<int> blockGrooms() const;
    // The window filled again from the document (its grooms changed, another opened).
    void refresh();

    // What its buttons do (scripts and tests call them too). Each returns false (and says why in
    // the status line) when it cannot.
    bool newAtlas();  // a new document of one block (asked first when edits would be lost)
    bool addBlock();
    bool duplicateBlock();
    bool removeBlock();
    bool pickBlock(int block);
    bool moveBlock(int block, const std::array<float, 4>& cell);
    bool editBlock(int block, float width, float length, float thickness, const Vec3f& rootRotation);
    // Every block's card fitted to its hair now (the bake, the exports and the edits here
    // do it first); true when a card changed.
    bool fitCards();
    bool autoLayout();
    bool setSettings(const CardAtlasSettings& settings);
    bool bakePreview(int resolution = 512);
    // The maps written into `folder` as the atlas' preset packs them; the paths (empty: failed).
    std::vector<std::string> exportMaps(const QString& folder, int resolution = 0);
    bool exportShapes(const QString& path, int segments = 8, float bend = 0.0f);
    QString status() const;
    int selectedBlock() const { return selected_; }
    AtlasCanvas* canvas() const { return canvas_; }
    QComboBox* mapBox() const { return map_; }

private:
    void writeAtlas(const CardBlockAtlas& atlas);  // every block's text back on its groom
    void showMap();
    void fillBlock();
    void say(const QString& text);

    MainWindow* main_;
    AtlasCanvas* canvas_ = nullptr;
    QComboBox* map_ = nullptr;
    QComboBox* size_ = nullptr;
    ValueSlider* padding_ = nullptr;
    QComboBox* preset_ = nullptr;
    QLineEdit* name_ = nullptr;
    QCheckBox* rootAtTop_ = nullptr;
    QListWidget* list_ = nullptr;
    QWidget* empty_ = nullptr;     // the Blocks block's empty state (no blocks yet)
    QFrame* item_ = nullptr;       // the picked block's box under the list
    QLabel* itemTitle_ = nullptr;  // ... its name
    ValueSlider* width_ = nullptr;
    ValueSlider* length_ = nullptr;
    ValueSlider* thickness_ = nullptr;
    ValueSlider* tilt_ = nullptr;
    ValueSlider* turn_ = nullptr;
    ValueSlider* roll_ = nullptr;
    QLabel* cardSize_ = nullptr;
    QLineEdit* folder_ = nullptr;
    ValueSlider* segments_ = nullptr;
    ValueSlider* bend_ = nullptr;
    QLabel* status_ = nullptr;
    int selected_ = -1;
    bool filling_ = false;
    std::shared_ptr<const CardMaps> preview_;
};

}  // namespace groomlab::app
