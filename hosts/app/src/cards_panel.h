// SPDX-License-Identifier: Apache-2.0
#pragma once

// Cards mode's card (the glass panel at the right): the Cards node to work on (added when the
// graph has none), the preview switch (the strands, or the cards they make; with their baked
// maps), a level of detail (hero, mid, low: the Maya host's cards_preset shapes, cards per
// clump and depth layers, segments, variations), the main settings as sliders (cards per clump,
// layers, width scale, segments, variations, the atlas resolution, the card budget),
// the bake (resolution, occlusion; the maps written beside the groom), what the cards amount to
// (cards, triangles, textures; stale after the bake) and the baked atlas as a picture. Edits go
// to the document as undoable param edits (a slider's drag is one step); the window evaluates
// and shows them (edited()).

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "glass.h"

class QComboBox;
class QLabel;

namespace groomlab::app {

class GroomDocument;

class CardsPanel : public GlassPanel {
    Q_OBJECT
public:
    explicit CardsPanel(QWidget* parent = nullptr);
    void setDocument(GroomDocument* document);  // null: nothing to edit
    // The widgets from the document (the node's params, the bake, the stats); after any edit.
    void refresh();
    // The node the panel edits ("" none).
    std::string node() const { return node_; }
    // The preview switch: the cards shown instead of the strands, and with their baked maps.
    bool showCards() const;
    bool textures() const;
    void setShowCards(bool on);  // no signal
    // What the last view made of the cards: the stats line and the stale note.
    void setStats(std::size_t cards, std::size_t triangles, std::size_t textures, bool stale);
    // The bake's settings the Bake button asks for.
    int bakeResolution() const;  // 0: the node's atlasSize
    float bakeOcclusion() const;  // the full occlusion (the export panel has the dial)
    void setBakeStatus(const QString& text);
    // "hero", "mid" or "low" (the LOD segmented's current, "" custom).
    std::string lod() const;

signals:
    void edited(bool live);  // a param changed on the document (live: a slider mid-drag)
    void viewChanged();      // the preview switch moved
    void bakeRequested();
    void addNodeRequested();

private:
    struct Slider {
        std::string param;
        ValueSlider* slider = nullptr;
        bool integer = true;
        nlohmann::json dragStart;
    };
    void addSlider(const QString& caption, const std::string& param, double min, double max, bool integer, const QColor& fill,
                   const QString& tip);
    void applyLod(const std::string& lod);
    void refreshAtlas();

    GroomDocument* document_ = nullptr;
    std::string node_;
    bool updating_ = false;
    QComboBox* nodes_ = nullptr;
    RoundButton* add_ = nullptr;
    Segmented* preview_ = nullptr;
    RoundButton* textures_ = nullptr;
    Segmented* lod_ = nullptr;
    std::vector<Slider> sliders_;
    QComboBox* atlas_ = nullptr;
    QWidget* settings_ = nullptr;
    QWidget* previewBox_ = nullptr;
    QLabel* titleRowLabel_ = nullptr;  // the header: with a Cards node only (the empty state has its own title)  // the Strands/Cards switch and Maps: with a Cards node only
    QWidget* bakeBox_ = nullptr;     // Bake maps and its resolution: with a Cards node only
    QLabel* stats_ = nullptr;
    QLabel* emptyIcon_ = nullptr;  // over the empty state's title (no Cards node)
    QComboBox* bakeResolution_ = nullptr;
    RoundButton* bake_ = nullptr;
    QLabel* bakeStatus_ = nullptr;
    QLabel* atlasImage_ = nullptr;
    std::string atlasShown_;  // the preview file the picture was made from
};

}  // namespace groomlab::app
