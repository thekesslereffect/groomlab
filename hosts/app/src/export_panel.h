// SPDX-License-Identifier: Apache-2.0
#pragma once

// Export mode's card (the glass panel at the right): the target (Alembic curves, USD curves,
// Unreal's groom, cards as glTF, Unity's strands, card maps), the frames (the current one, or a range for
// an animated groom), the density, the target's own options (as the Maya host's export takes them:
// groom groups, the feathers' shafts and barbs for both engines, guides and feather guides for Unreal,
// the scale, points per strand and barb points for Unity, the resolution, occlusion and binary form for cards),
// card maps' preset, resolution, format, bit depth, padding and supersampling with the packing
// table under Advanced (as the Maya host's Export Card Maps window), the output file (typed, or
// picked with the "..." button: the only file dialog, on a click; card maps: <folder>/<name>)
// and the Export button with a progress bar and a line saying what was written. Save and Save
// as stay here too. The window runs the export (GroomDocument's) and feeds the progress back;
// nothing here opens a dialog on its own.

#include <string>

#include "glass.h"
#include "groom_document.h"

#include <vector>

#include "groomlab/card_maps.h"

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QSpinBox;
class QDoubleSpinBox;
class QVBoxLayout;

namespace groomlab::app {

class ExportPanel : public GlassPanel {
    Q_OBJECT
public:
    enum class Target { Alembic, Usd, Unreal, Cards, Unity, CardMaps, Vrscene, Count };

    explicit ExportPanel(QWidget* parent = nullptr);
    Target target() const { return target_; }
    void setTarget(Target target);  // no signal
    static const char* targetName(Target target);  // "alembic", "usd", "unreal", "cards", "unity", "maps", "vrscene"
    static bool targetFromName(const std::string& name, Target* target);
    // The options as set: the frames (the current frame when the range is off), the density
    // and the target's own.
    ExportOptions options() const;
    QString outputPath() const;
    void setOutputPath(const QString& path);
    // The document's state the panel shows: its frame, the caches' range (a range is offered
    // when the groom animates), and the default output beside the groom.
    void setDocumentState(bool open, double frame, bool animated, double first, double last, const QString& groomPath);
    // A running export: the button becomes Cancel, the bar fills; done, the line says so.
    void setProgress(std::size_t done, std::size_t count);
    void setBusy(bool busy);
    bool busy() const { return busy_; }
    RoundButton* exportButton() const { return export_; }  // Export, or Cancel while busy
    void setStatus(const QString& text, bool error = false);

    // Card maps (the "maps" target, card_maps.h): the preset picked ("Custom" once the packing is
    // edited), picking one (its packing, format and bit depth fill the panel; false: no such
    // preset), the packing as it stands, and Save as Preset / Delete (the user's folder; no
    // dialog: the name comes from the field beside the button). The packing's rows: a file each.
    QString mapPreset() const;
    bool setMapPreset(const QString& name);
    CardMapPreset mapPacking() const;
    int mapFileCount() const { return static_cast<int>(packRows_.size()); }
    void setMapChannel(int file, int channel, const QString& value, bool invert = false);  // channel 0-3: R G B A
    void addMapFile();
    void removeMapFile(int file);
    bool saveMapPreset(const QString& name);  // false: refused (the status says why)
    bool deleteMapPreset();                   // the current one, when it is the user's
    void setMapSettings(int resolution, const QString& format, int bitDepth, int padding, int supersample);

signals:
    void exportRequested();
    void cancelRequested();
    void browseRequested();  // the "..." button: the window opens the file dialog
    void saveRequested();
    void saveAsRequested();

private:
    void refresh();  // the options shown for the target, the path's extension
    QString defaultExtension() const;
    // Card maps: the presets listed again (`select` picked), the packing rows from a preset, the
    // bit depths of the format, the preset turned Custom after an edit.
    void refreshMapPresets(const QString& select);
    void fillPacking(const std::vector<CardMapFile>& files);
    void addPackRow(const CardMapFile& file);
    void refreshBitDepths(int prefer);
    void packingEdited();

    struct PackRow {
        QWidget* box = nullptr;
        QLineEdit* name = nullptr;
        QComboBox* channels[4] = {};
        QCheckBox* invert[4] = {};
        QComboBox* space = nullptr;
    };

    Target target_ = Target::Alembic;
    bool busy_ = false;
    bool updating_ = false;
    std::vector<RoundButton*> targets_;
    QLabel* description_ = nullptr;
    QCheckBox* range_ = nullptr;
    QWidget* framesBox_ = nullptr;  // From, to and Step: shown while Frame range is on
    QSpinBox* first_ = nullptr;
    QSpinBox* last_ = nullptr;
    QDoubleSpinBox* step_ = nullptr;
    ValueSlider* density_ = nullptr;
    QWidget* engineOptions_ = nullptr;  // Unreal's and Unity's: groups, shafts, barbs
    QCheckBox* groups_ = nullptr;
    QComboBox* shafts_ = nullptr;
    QDoubleSpinBox* barbDensity_ = nullptr;
    QDoubleSpinBox* barbWidth_ = nullptr;
    QWidget* unrealOptions_ = nullptr;
    QCheckBox* guides_ = nullptr;
    QCheckBox* featherGuides_ = nullptr;
    QCheckBox* iridescenceAo_ = nullptr;
    QWidget* unityOptions_ = nullptr;
    QDoubleSpinBox* scale_ = nullptr;
    QSpinBox* points_ = nullptr;
    QSpinBox* barbPoints_ = nullptr;
    QWidget* cardOptions_ = nullptr;
    QComboBox* resolution_ = nullptr;
    ValueSlider* occlusion_ = nullptr;
    QCheckBox* binary_ = nullptr;
    QWidget* mapOptions_ = nullptr;
    QComboBox* mapPreset_ = nullptr;
    RoundButton* mapDelete_ = nullptr;
    QLabel* mapDescription_ = nullptr;
    QComboBox* mapResolution_ = nullptr;
    QComboBox* mapFormat_ = nullptr;
    QComboBox* mapBitDepth_ = nullptr;
    QSpinBox* mapPadding_ = nullptr;
    QComboBox* mapSupersample_ = nullptr;
    QCheckBox* mapAdvanced_ = nullptr;
    QWidget* packing_ = nullptr;
    QComboBox* mapNormal_ = nullptr;
    QLineEdit* mapTemplate_ = nullptr;
    QVBoxLayout* packRowsLayout_ = nullptr;
    std::vector<PackRow> packRows_;
    QLineEdit* mapSaveName_ = nullptr;
    std::vector<CardMapPreset> mapPresets_;
    bool filling_ = false;
    QLineEdit* path_ = nullptr;
    RoundButton* browse_ = nullptr;
    RoundButton* export_ = nullptr;
    QWidget* progressBar_ = nullptr;
    QLabel* status_ = nullptr;
    double frame_ = 1.0;
    std::size_t done_ = 0, count_ = 0;
    QString groomStem_;
};

}  // namespace groomlab::app
