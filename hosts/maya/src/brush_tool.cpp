// SPDX-License-Identifier: Apache-2.0
#include "brush_tool.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <stdexcept>

#include <maya/MTime.h>
#include <maya/M3dView.h>
#include <maya/MDagPath.h>
#include <maya/MArgDatabase.h>
#include <maya/MArgList.h>
#include <maya/MArgParser.h>
#include <maya/MColor.h>
#include <maya/MEvent.h>
#include <maya/MFnDagNode.h>
#include <maya/MGlobal.h>
#include <maya/MPoint.h>
#include <maya/MPointArray.h>
#include <maya/MSelectionList.h>
#include <maya/MStringArray.h>
#include <maya/MToolsInfo.h>
#include <maya/MUIDrawManager.h>

#include "command_args.h"
#include "gpu_preview.h"
#include "guarded.h"
#include "active_groom.h"
#include "asset_files.h"
#include "groom_shape.h"
#include "groomlab/flow.h"
#include "groomlab/texture.h"
#include "groom_draw_override.h"
#include "maya_inputs.h"
#include "pen_pressure.h"
#include "select_tool.h"
#include "tool_view.h"
#include "groomlab/scalp_maps.h"
#include "groomlab/subdivision.h"

namespace groomlab::maya {

namespace {

MColor mayaColor(const std::array<float, 3>& c) { return MColor(c[0], c[1], c[2]); }

const char* kModeFlag[] = {"-m", "-mode"};
const char* kRadiusFlag[] = {"-r", "-radius"};
const char* kStrengthFlag[] = {"-s", "-strength"};
const char* kLevelFlag[] = {"-l", "-level"};
const char* kGroupFlag[] = {"-g", "-group"};
const char* kPropagateFlag[] = {"-pr", "-propagate"};
const char* kPointFlag[] = {"-p", "-point"};
const char* kLengthFlag[] = {"-len", "-length"};
const char* kSegmentsFlag[] = {"-seg", "-segments"};
const char* kTubeRadiusFlag[] = {"-tr", "-tubeRadius"};
const char* kInteractiveFlag[] = {"-i", "-interactive"};
const char* kDebugStrokeFlag[] = {"-dbs", "-debugStroke"};
const char* kSelectedOnlyFlag[] = {"-so", "-selectedOnly"};
const char* kEraseGuidesFlag[] = {"-erg", "-eraseGuides"};
const char* kErasePartsFlag[] = {"-erp", "-eraseParts"};
const char* kEraseFlowFlag[] = {"-erf", "-eraseFlow"};
const char* kSymmetryFlag[] = {"-sym", "-symmetry"};
const char* kSymmetryCenterFlag[] = {"-syc", "-symmetryCenter"};
const char* kAttributeFlag[] = {"-at", "-attribute"};
const char* kValueFlag[] = {"-v", "-value"};
const char* kMatchFlag[] = {"-mn", "-matchNeighbors"};
const char* kSoloNewFlag[] = {"-sng", "-soloNewGuides"};
// Place and Draw: the new guides' tube as JSON (setPlacementTube: shape, width, aspect, twist).
const char* kPlaceTubeFlag[] = {"-ptb", "-placeTube"};
const char* kSpacingFlag[] = {"-sp", "-spacing"};
const char* kCollideFlag[] = {"-col", "-collide"};
const char* kMaskScalpFlag[] = {"-msk", "-maskByScalp"};
const char* kStretchFlag[] = {"-sr", "-stretch"};
const char* kTubeChannelFlag[] = {"-tch", "-tubeChannel"};
const char* kPressureFlag[] = {"-prs", "-pressure"};
const char* kPartNameFlag[] = {"-pn", "-partName"};
const char* kFlowSinkFlag[] = {"-fsk", "-flowSink"};
const char* kDragSliderFlag[] = {"-ds", "-dragSlider"};
const char* kDebugPickFlag[] = {"-dbp", "-debugPick"};
const char* kTargetsFlag[] = {"-tg", "-targets"};
const char* kPaintKindFlag[] = {"-pk", "-paintKind"};
const char* kPaintTextureFlag[] = {"-ptx", "-paintTexture"};
const char* kPaintResolutionFlag[] = {"-prz", "-paintResolution"};  // short flags: 3 characters at most
const char* kHardnessFlag[] = {"-hdn", "-hardness"};
const char* kFalloffProfileFlag[] = {"-fop", "-falloffProfile"};
const char* kStrengthProfileFlag[] = {"-stp", "-strengthProfile"};
const char* kSmoothStrengthFlag[] = {"-sms", "-smoothStrength"};
const char* kPaintColorFlag[] = {"-pco", "-paintColor"};
const char* kPaintChannelsFlag[] = {"-pch", "-paintChannels"};
const char* kPaintModeFlag[] = {"-pmd", "-paintMode"};
const char* kDebugHoverFlag[] = {"-dbh", "-debugHover"};
const char* kHeldBytesFlag[] = {"-hb", "-heldUndoBytes"};
const char* kPreviewUploadFlag[] = {"-pup", "-previewUpload"};
const char* kFileImageFlag[] = {"-fim", "-fileImage"};
const char* kDebugStrokeTimesFlag[] = {"-dst", "-debugStrokeTimes"};
const char* kDebugStrokeDrawsFlag[] = {"-dsd", "-debugStrokeDraws"};

// A file's bytes (none when it is not there), read at once.
FileBytes readFileBytes(const std::string& path) { return std::make_shared<const std::vector<std::uint8_t>>(fileBytes(path)); }

// The bytes the texture strokes in the undo queue keep.
std::atomic<std::size_t> gHeldTextureBytes{0};

// Writes the bytes (a texture map's file), or removes the file for none.
void writeFileBytes(const std::string& path, const std::vector<std::uint8_t>& bytes) {
    noteFileWritten();
    std::error_code ec;
    if (bytes.empty()) {
        std::filesystem::remove(std::filesystem::u8path(path), ec);
        return;
    }
    std::filesystem::create_directories(std::filesystem::u8path(path).parent_path(), ec);
    std::ofstream out(std::filesystem::u8path(path), std::ios::binary | std::ios::trunc);
    if (!out) throw std::runtime_error("can't write " + path);
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}
}  // namespace

// The paint, its file, the scalp and the file's stamp as every host keeps them (scalp_paint.h's
// KeptTexturePaint: the stamp is the file's as this paint last wrote it, or as it was when
// opened), plus what the undo queue needs.
struct PaintedTexture : KeptTexturePaint {
    std::uint64_t revision = 0;  // up by each stroke on it, down and up again by its undo and redo
    // fileWrites() when the stamp was taken. `continuable`: the interactive brush goes on painting
    // these values while the file is what it kept (a stroke wrote it); after an undo, a redo, or a
    // file written by anything else, the next stroke opens the file again, as it reads.
    std::uint64_t writtenWrites = 0;
    bool wrote = false, continuable = false;
};

namespace {

// The paint that last wrote each texture file, while one holds it.
std::map<std::string, std::weak_ptr<PaintedTexture>>& paintedFiles() {
    static std::map<std::string, std::weak_ptr<PaintedTexture>> files;
    return files;
}

// The paint whose PNG the file holds now: it wrote the file last, and nothing wrote a file since
// (the plugin's fileWrites(), the file's stamp). Null when none does.
std::shared_ptr<PaintedTexture> lastWriter(const std::string& path) {
    auto& files = paintedFiles();
    const auto it = files.find(path);
    if (it == files.end()) return nullptr;
    std::shared_ptr<PaintedTexture> painted = it->second.lock();
    if (!painted) {
        files.erase(it);
        return nullptr;
    }
    const std::uint64_t stamp = fileStamp(path);
    if (!painted->wrote || stamp == 0 || painted->stamp != stamp || painted->writtenWrites != fileWrites()) return nullptr;
    return painted;
}

void forgetWriter(const std::string& path) { paintedFiles().erase(path); }

// Writes the PNG of the paint as it is to its file, notes the write (the file is this paint's
// now), and puts what reading the file gives into the texture cache, so the groom's evaluation
// (and the viewport) do not read the file back.
void writePaintedFile(const std::shared_ptr<PaintedTexture>& painted, bool continuable) {
    const TexturePaint& paint = *painted->paint;
    const std::vector<std::uint8_t> pixels = paint.toBytes();
    writeFileBytes(painted->path, storedPngBytes(paint.width(), paint.height(), paint.channels(), pixels));
    painted->written();
    painted->writtenWrites = fileWrites();
    painted->wrote = true;
    painted->continuable = continuable;
    paintedFiles()[painted->path] = painted;
    primeTextureCache(painted->path, painted->stamp, textureFromPixels(paint.width(), paint.height(), paint.channels(), pixels));
}

// The file of the groom's texture map `name` (resolved for the groom), "" when the groom has no
// texture map of that name. The groom's own entry decides, never the tool's -paintTexture: the
// tool may still name another groom's file (the active groom changed since it was set).
std::string textureMapFile(const MDagPath& groom, GroomShape& shape, const std::string& name) {
    const ScalpMaps& maps = shape.scalpMaps();
    const auto it = maps.maps.find(name);
    if (it == maps.maps.end() || it->second.kind != "texture" || it->second.texture.empty()) return {};
    return resolveNodePath(groom.node(), it->second.texture);
}

// Before a stroke writes a texture map's file: when another groom reads that file too, this
// groom gets its own copy (forkTextureFile) and `*mapsJson` its scalp maps pointing at it
// (for the stroke's undoable edit). Returns the file to write.
std::string ownTextureFile(const MDagPath& groom, GroomShape& shape, const std::string& name, const std::string& full,
                           std::string* mapsJson) {
    mapsJson->clear();
    if (!fileUsedByOtherGroom(groom.node(), full)) return full;
    const std::string copy = forkTextureFile(groom, name, full);
    ScalpMaps maps = shape.scalpMaps();
    maps.maps[name].texture = portableMayaPath(copy);
    *mapsJson = maps.toJson();
    return copy;
}

// The texture map a stroke paints: a PNG (strokes write PNG), opened as every host opens it
// (scalp_paint.h: the file's image, or blank at the given size).
std::shared_ptr<TexturePaint> openStrokePaint(const ScalpMesh& scalp, const std::string& path, int resolution, int channels) {
    requirePaintablePng(path);
    return openTexturePaint(scalp, path, channels, resolution);
}

// The flags groomLabBrushStroke and groomLabBrushCtx share. -radius is in both
// but means world units for a stroke and pixels for the tool, so each reads it.
void addBrushFlags(MSyntax& s) {
    s.addFlag(kModeFlag[0], kModeFlag[1], MSyntax::kString);
    s.addFlag(kRadiusFlag[0], kRadiusFlag[1], MSyntax::kDouble);
    s.addFlag(kStrengthFlag[0], kStrengthFlag[1], MSyntax::kDouble);
    s.addFlag(kLevelFlag[0], kLevelFlag[1], MSyntax::kLong);
    s.addFlag(kGroupFlag[0], kGroupFlag[1], MSyntax::kString);
    s.addFlag(kPropagateFlag[0], kPropagateFlag[1], MSyntax::kBoolean);
    s.addFlag(kLengthFlag[0], kLengthFlag[1], MSyntax::kDouble);
    s.addFlag(kSegmentsFlag[0], kSegmentsFlag[1], MSyntax::kLong);
    s.addFlag(kTubeRadiusFlag[0], kTubeRadiusFlag[1], MSyntax::kDouble);
    s.addFlag(kSelectedOnlyFlag[0], kSelectedOnlyFlag[1], MSyntax::kBoolean);
    s.addFlag(kSymmetryFlag[0], kSymmetryFlag[1], MSyntax::kString);
    s.addFlag(kSymmetryCenterFlag[0], kSymmetryCenterFlag[1], MSyntax::kDouble);
    s.addFlag(kAttributeFlag[0], kAttributeFlag[1], MSyntax::kString);
    s.addFlag(kValueFlag[0], kValueFlag[1], MSyntax::kDouble);
    s.addFlag(kMatchFlag[0], kMatchFlag[1], MSyntax::kBoolean);
    s.addFlag(kSoloNewFlag[0], kSoloNewFlag[1], MSyntax::kBoolean);
    s.addFlag(kPlaceTubeFlag[0], kPlaceTubeFlag[1], MSyntax::kString);
    s.addFlag(kSpacingFlag[0], kSpacingFlag[1], MSyntax::kDouble);
    s.addFlag(kCollideFlag[0], kCollideFlag[1], MSyntax::kBoolean);
    s.addFlag(kMaskScalpFlag[0], kMaskScalpFlag[1], MSyntax::kBoolean);
    s.addFlag(kStretchFlag[0], kStretchFlag[1], MSyntax::kBoolean);
    s.addFlag(kTubeChannelFlag[0], kTubeChannelFlag[1], MSyntax::kString);
    s.addFlag(kPartNameFlag[0], kPartNameFlag[1], MSyntax::kString);
    s.addFlag(kFlowSinkFlag[0], kFlowSinkFlag[1], MSyntax::kString);
    s.addFlag(kTargetsFlag[0], kTargetsFlag[1], MSyntax::kString);
    s.addFlag(kPaintKindFlag[0], kPaintKindFlag[1], MSyntax::kString);
    s.addFlag(kPaintTextureFlag[0], kPaintTextureFlag[1], MSyntax::kString);
    s.addFlag(kPaintResolutionFlag[0], kPaintResolutionFlag[1], MSyntax::kLong);
    s.addFlag(kHardnessFlag[0], kHardnessFlag[1], MSyntax::kDouble);
    s.addFlag(kFalloffProfileFlag[0], kFalloffProfileFlag[1], MSyntax::kString);
    s.addFlag(kStrengthProfileFlag[0], kStrengthProfileFlag[1], MSyntax::kString);
    s.addFlag(kSmoothStrengthFlag[0], kSmoothStrengthFlag[1], MSyntax::kDouble);
    s.addFlag(kPaintColorFlag[0], kPaintColorFlag[1], MSyntax::kDouble, MSyntax::kDouble, MSyntax::kDouble, MSyntax::kDouble);
    s.addFlag(kPaintChannelsFlag[0], kPaintChannelsFlag[1], MSyntax::kString);
    s.addFlag(kPaintModeFlag[0], kPaintModeFlag[1], MSyntax::kString);
}

// Applies the shared flags that are set (not -radius) to `s`, from an
// MArgDatabase (a stroke) or an MArgParser (the tool's edit flags), kept in range as every host
// keeps them (BrushToolSettings' setters). Reports and fails on a bad value.
template <class Args>
MStatus applyBrushFlags(const Args& args, BrushToolSettings& s, const char* command) {
    MString text;
    double d = 0;
    int n = 0;
    bool b = false;
    try {
        if (args.isFlagSet(kModeFlag[0]) && args.getFlagArgument(kModeFlag[0], 0, text)) s.setMode(text.asChar());
        if (args.isFlagSet(kSymmetryFlag[0]) && args.getFlagArgument(kSymmetryFlag[0], 0, text)) s.setSymmetry(text.asChar());
        if (args.isFlagSet(kTargetsFlag[0]) && args.getFlagArgument(kTargetsFlag[0], 0, text)) s.setTargets(text.asChar());
        if (args.isFlagSet(kTubeChannelFlag[0]) && args.getFlagArgument(kTubeChannelFlag[0], 0, text)) {
            s.brush.tubeChannel = tubeChannelFromString(text.asChar());
        }
        // The curves as ramps' JSON ("" clears one).
        if (args.isFlagSet(kFalloffProfileFlag[0]) && args.getFlagArgument(kFalloffProfileFlag[0], 0, text)) {
            s.setFalloffProfile(text.asChar());
        }
        if (args.isFlagSet(kStrengthProfileFlag[0]) && args.getFlagArgument(kStrengthProfileFlag[0], 0, text)) {
            s.setStrengthProfile(text.asChar());
        }
        if (args.isFlagSet(kPlaceTubeFlag[0]) && args.getFlagArgument(kPlaceTubeFlag[0], 0, text)) {
            setPlacementTube(s.place, text.asChar());
        }
        if (args.isFlagSet(kFlowSinkFlag[0]) && args.getFlagArgument(kFlowSinkFlag[0], 0, text)) {
            if (text.length() > 0) flowSinkAngle(text.asChar());  // throws for an unknown kind
            s.flowSink = text.asChar();
        }
    } catch (const std::invalid_argument& e) {
        return fail(command, e.what());
    }
    if (args.isFlagSet(kPaintKindFlag[0]) && args.getFlagArgument(kPaintKindFlag[0], 0, text)) {
        if (text != "guides" && text != "vertices" && text != "texture") return fail(command, "-paintKind takes guides, vertices or texture");
        s.paintKind = text.asChar();
    }
    if (args.isFlagSet(kStrengthFlag[0]) && args.getFlagArgument(kStrengthFlag[0], 0, d)) s.brush.strength = float(d);
    if (args.isFlagSet(kLevelFlag[0]) && args.getFlagArgument(kLevelFlag[0], 0, n)) s.brush.level = n;
    if (args.isFlagSet(kGroupFlag[0]) && args.getFlagArgument(kGroupFlag[0], 0, text)) s.brush.group = text.asChar();
    if (args.isFlagSet(kPropagateFlag[0]) && args.getFlagArgument(kPropagateFlag[0], 0, b)) s.brush.propagate = b;
    if (args.isFlagSet(kLengthFlag[0]) && args.getFlagArgument(kLengthFlag[0], 0, d)) s.place.length = float(d);
    if (args.isFlagSet(kSegmentsFlag[0]) && args.getFlagArgument(kSegmentsFlag[0], 0, n)) s.setSegments(n);
    if (args.isFlagSet(kTubeRadiusFlag[0]) && args.getFlagArgument(kTubeRadiusFlag[0], 0, d)) {
        s.place.tube.width = {float(d), float(d)};
    }
    if (args.isFlagSet(kSpacingFlag[0]) && args.getFlagArgument(kSpacingFlag[0], 0, d)) s.place.spacing = float(d);
    if (args.isFlagSet(kMatchFlag[0]) && args.getFlagArgument(kMatchFlag[0], 0, b)) s.place.matchNeighbors = b;
    if (args.isFlagSet(kSoloNewFlag[0]) && args.getFlagArgument(kSoloNewFlag[0], 0, b)) s.place.soloNewGuides = b;
    if (args.isFlagSet(kCollideFlag[0]) && args.getFlagArgument(kCollideFlag[0], 0, b)) s.brush.collide = b;
    if (args.isFlagSet(kMaskScalpFlag[0]) && args.getFlagArgument(kMaskScalpFlag[0], 0, b)) s.maskByScalp = b;
    if (args.isFlagSet(kStretchFlag[0]) && args.getFlagArgument(kStretchFlag[0], 0, b)) s.brush.stretch = b;
    if (args.isFlagSet(kSelectedOnlyFlag[0]) && args.getFlagArgument(kSelectedOnlyFlag[0], 0, b)) s.selectedOnly = b;
    if (args.isFlagSet(kSymmetryCenterFlag[0]) && args.getFlagArgument(kSymmetryCenterFlag[0], 0, d)) s.brush.mirrorCenter = float(d);
    if (args.isFlagSet(kAttributeFlag[0]) && args.getFlagArgument(kAttributeFlag[0], 0, text)) s.brush.attribute = text.asChar();
    if (args.isFlagSet(kPartNameFlag[0]) && args.getFlagArgument(kPartNameFlag[0], 0, text)) s.partName = text.asChar();
    if (args.isFlagSet(kValueFlag[0]) && args.getFlagArgument(kValueFlag[0], 0, d)) s.brush.value = float(d);
    if (args.isFlagSet(kPaintTextureFlag[0]) && args.getFlagArgument(kPaintTextureFlag[0], 0, text)) s.paintTexture = text.asChar();
    if (args.isFlagSet(kPaintResolutionFlag[0]) && args.getFlagArgument(kPaintResolutionFlag[0], 0, n)) s.setPaintResolution(n);
    if (args.isFlagSet(kHardnessFlag[0]) && args.getFlagArgument(kHardnessFlag[0], 0, d)) s.setHardness(float(d));
    if (args.isFlagSet(kSmoothStrengthFlag[0]) && args.getFlagArgument(kSmoothStrengthFlag[0], 0, d)) s.setSmoothStrength(float(d));
    if (args.isFlagSet(kPaintColorFlag[0])) {
        PaintColor c = s.paintColor;
        for (unsigned k = 0; k < 4; ++k) {
            if (args.getFlagArgument(kPaintColorFlag[0], k, d)) c[k] = float(d);
        }
        s.setPaintColor(c);
    }
    if (args.isFlagSet(kPaintChannelsFlag[0]) && args.getFlagArgument(kPaintChannelsFlag[0], 0, text)) {
        try {
            s.paintChannels = channelMask(text.asChar());
        } catch (const std::invalid_argument& e) {
            return fail(command, (std::string("-paintChannels: ") + e.what()).c_str());
        }
    }
    if (args.isFlagSet(kPaintModeFlag[0]) && args.getFlagArgument(kPaintModeFlag[0], 0, text)) {
        try {
            s.brush.paintMode = paintModeFromString(text.asChar());
        } catch (const std::invalid_argument& e) {
            return fail(command, (std::string("-paintMode: ") + e.what()).c_str());
        }
    }
    return MS::kSuccess;
}

GroomShape* groomShape(const MDagPath& path) {
    return dynamic_cast<GroomShape*>(MFnDagNode(path).userNode());
}

// A groom from a referenced file: every stroke rewrites its whole guideData,
// which the reference records as an edit (and reapplies on load). Refused unless
// the groom carries a true `allowReferenceEdits` attribute (add it to opt in).
bool referenceEditAllowed(const MDagPath& groom) {
    MFnDagNode fn(groom);
    if (!fn.isFromReferencedFile()) return true;
    MStatus status;
    const MPlug allow = fn.findPlug("allowReferenceEdits", true, &status);
    return status && !allow.isNull() && allow.asBool();
}

const char* kReferenceRefused =
    ": the groom is from a referenced file; brushing would record its whole guide data as a "
    "reference edit. Edit it in its own scene, or add a boolean attribute allowReferenceEdits (on) to allow it";

}  // namespace

// ---------------------------------------------------------------------------
// groomLabBrushStroke

// A scripted stroke's host: the groom's plugs as they are (its scalps converted from inMesh and
// extraScalps, the guides parsed and rooted on them), its texture map opened from the file; the
// edit goes into the command's modifier, a texture map's file written with its bytes before and
// after kept.
class ScriptedStrokeHost : public StrokeHost {
public:
    ScriptedStrokeHost(BrushStrokeCmd& cmd, GroomShape* shape) : cmd_(cmd), shape_(shape) {}

    // The groom's scalps joined (inMesh, then its other scalps: scalp_pieces.h), refined as the
    // groom's session refines them (each scalp at its own level, subdivision.h): the scalp the
    // interactive brush reads. Vertex maps are painted on the joined cage.
    MeshPtr scalp() override {
        if (!scalpRead_) {
            const MeshPtr joined = joinedScalpFromNode(cmd_.groom_.node());
            scalp_ = shape_ ? shape_->session().refineAsScalp(joined) : joined;
            cage_ = scalp_ && scalp_ != joined && scalp_->cage() ? scalp_->cage() : joined;
            scalpRead_ = true;
        }
        return scalp_;
    }
    MeshPtr paintScalp() override {
        scalp();
        return cage_;
    }
    std::shared_ptr<GuideHierarchy> editableHierarchy() override {
        std::string error;
        std::shared_ptr<GuideHierarchy> h = toHierarchy(storedText(StrokeField::Guides), scalp().get(), &error, groomScaleOf(cmd_.groom_.node()));
        if (!h) throw std::runtime_error(error);
        return h;
    }
    std::shared_ptr<const GuideHierarchy> authoredHierarchy() override { return editableHierarchy(); }
    std::shared_ptr<const GuideHierarchy> shownHierarchy() override { return editableHierarchy(); }
    std::string storedText(StrokeField field) override {
        return MFnDagNode(cmd_.groom_).findPlug(strokeFieldName(field), true).asString().asChar();
    }
    const ScalpMaps& scalpMaps() override {
        if (!shape_) throw std::runtime_error("the groom node is not loaded");
        return shape_->scalpMaps();
    }
    std::vector<GuideId> selectedGuides() override { return readGuideSelection(cmd_.groom_.node()); }
    float softSelectRadius() override { return readSoftSelectRadius(cmd_.groom_.node()); }
    std::set<std::string> hiddenGroups() override { return readHiddenGuideGroups(cmd_.groom_.node()); }
    // The Sculpt node brushes shape (the groom's sculptNode), fed as the groom's evaluation feeds it.
    std::string sculptNode() override { return MFnDagNode(cmd_.groom_).findPlug("sculptNode", true).asString().asChar(); }
    SculptInput sculptInput(const std::string& node) override {
        if (!shape_) throw std::runtime_error("the groom node is not loaded");
        MFnDagNode(cmd_.groom_).findPlug("outStrandCount", true).asInt();  // the groom evaluated as it is
        return shape_->sculptInput(node);
    }
    std::shared_ptr<TexturePaint> texturePaint(const std::string& name, int channels, int resolution) override {
        refreshMayaPaths();
        source_ = textureMapFile(cmd_.groom_, *shape_, name);
        if (source_.empty()) throw std::runtime_error("the groom has no texture map named " + name);
        paint_ = openStrokePaint(*scalp(), source_, resolution, channels);
        return paint_;
    }
    void previewHierarchy(std::shared_ptr<GuideHierarchy>, bool) override {}
    void previewOverlays(std::shared_ptr<const PartSet>, std::shared_ptr<const FlowSet>) override {}
    void previewVertexMap(const std::string&, std::shared_ptr<const std::vector<float>>, int) override {}
    void previewTextureMap(const std::string&, std::shared_ptr<const TexturePreview>) override {}
    void liveStroke(bool) override {}
    void warn(const std::string& message) override { MGlobal::displayWarning(message.c_str()); }

    void commit(StrokeEdit edit) override {
        if (!edit.textureMap.empty() && paint_) {
            // The texture map's file rewritten (a groom sharing it gets its own copy first), its
            // bytes before and after kept for undo and redo.
            std::string mapsJson;
            const std::string full = ownTextureFile(cmd_.groom_, *shape_, edit.textureMap, source_, &mapsJson);
            const FileBytes before = readFileBytes(full);
            // The PNG made once: written, kept as the file's bytes after the stroke, and what
            // reading it gives put in the texture cache (the groom does not read it back).
            const std::vector<std::uint8_t> pixels = paint_->toBytes();
            const FileBytes after = std::make_shared<const std::vector<std::uint8_t>>(
                storedPngBytes(paint_->width(), paint_->height(), paint_->channels(), pixels));
            writeFileBytes(full, *after);
            forgetWriter(full);
            primeTextureCache(full, fileStamp(full), textureFromPixels(paint_->width(), paint_->height(), paint_->channels(), pixels));
            cmd_.setTextureBytes(cmd_.groom_, full, before, after, mapsJson);
        }
        cmd_.addTexts(edit);
    }

private:
    BrushStrokeCmd& cmd_;
    GroomShape* shape_;
    bool scalpRead_ = false;
    MeshPtr scalp_;
    MeshPtr cage_;
    std::string source_;
    std::shared_ptr<TexturePaint> paint_;
};

const char* BrushStrokeCmd::kName = "groomLabBrushStroke";

void* BrushStrokeCmd::creator() { return new BrushStrokeCmd(); }

BrushStrokeCmd::BrushStrokeCmd() { setCommandString(kName); }

MSyntax BrushStrokeCmd::newSyntax() {
    MSyntax s;
    addBrushFlags(s);
    s.addFlag(kPointFlag[0], kPointFlag[1], MSyntax::kDouble, MSyntax::kDouble, MSyntax::kDouble);
    s.makeFlagMultiUse(kPointFlag[0]);
    s.addFlag(kInteractiveFlag[0], kInteractiveFlag[1], MSyntax::kBoolean);
    s.setObjectType(MSyntax::kSelectionList, 1, 1);
    s.useSelectionAsDefault(true);
    s.enableQuery(false);
    s.enableEdit(false);
    return s;
}

MStatus BrushStrokeCmd::doIt(const MArgList& args) {
    return groomlab::maya::guarded("BrushStrokeCmd::doIt", [&]() -> MStatus {
        MStatus status;
        MArgDatabase db(syntax(), args, &status);
        if (!status) return status;
        if (db.isFlagSet(kInteractiveFlag[0])) {
            return fail(kName, "interactive strokes are recorded for undo and cannot be replayed");
        }

        MSelectionList selection;
        db.getObjects(selection);
        if (!groomInSelection(selection, groom_)) return fail(kName, "select or name a groomLabShape");
        if (MFnDagNode(groom_).findPlug("locked", true).asBool()) return fail(kName, groom_.partialPathName() + " is locked");
        if (!referenceEditAllowed(groom_)) return fail(kName, groom_.partialPathName() + kReferenceRefused);

        settings_.setMode("comb");  // unless -mode says otherwise
        const MStatus flags = applyBrushFlags(db, settings_, kName);
        if (!flags) return flags;
        double d = 0;
        if (db.isFlagSet(kRadiusFlag[0]) && db.getFlagArgument(kRadiusFlag[0], 0, d)) settings_.brush.radius = float(d);
        mode_ = settings_.modeName();

        for (unsigned i = 0; i < db.numberOfFlagUses(kPointFlag[0]); ++i) {
            MArgList p;
            db.getFlagArgumentList(kPointFlag[0], i, p);
            path_.emplace_back(float(p.asDouble(0)), float(p.asDouble(1)), float(p.asDouble(2)));
        }
        if (path_.empty()) return fail(kName, "give at least one -point");

        auto* shape = dynamic_cast<GroomShape*>(MFnDagNode(groom_).userNode());
        const bool maps = settings_.tool == BrushToolSettings::Tool::Brush && settings_.paintsScalp();
        if (maps && !shape) return fail(kName, "the groom node is not loaded");
        ScriptedStrokeHost host(*this, shape);
        std::size_t changed = 0;
        try {
            changed = scriptedStroke(host, settings_, path_);
        } catch (const std::exception& e) {
            return fail(kName, e.what());
        }
        if (maps) setResult(static_cast<int>(changed));
        return redoIt();
    });
}

std::size_t TextureUndo::bytes() const { return edit.bytes() + (beforeBytes ? beforeBytes->size() : 0); }

BrushStrokeCmd::~BrushStrokeCmd() { holdBytes(0); }

std::size_t BrushStrokeCmd::heldTextureBytes() { return gHeldTextureBytes.load(); }

void BrushStrokeCmd::holdBytes(std::size_t bytes) {
    gHeldTextureBytes -= heldBytes_;
    heldBytes_ = bytes;
    gHeldTextureBytes += heldBytes_;
}

void BrushStrokeCmd::addTexts(const StrokeEdit& edit) {
    MFnDagNode fn(groom_);
    for (std::size_t i = 0; i < kStrokeFields; ++i) {
        const StrokeEdit::Text& t = edit.texts[i];
        if (t.set) modifier_.newPlugValueString(fn.findPlug(strokeFieldName(static_cast<StrokeField>(i)), true), t.after.c_str());
    }
}

void BrushStrokeCmd::bumpBrushTick() {
    // The groom reads the file again (the Map node's cache key carries the file's stamp).
    MPlug tick = MFnDagNode(groom_).findPlug("brushTick", true);
    modifier_.newPlugValueInt(tick, tick.asInt() + 1);
}

void BrushStrokeCmd::setInteractiveEdit(const MDagPath& groom, const StrokeEdit& edit) {
    groom_ = groom;
    mode_ = edit.mode;
    interactive_ = true;
    addTexts(edit);
}

void BrushStrokeCmd::setTextureEdit(const MDagPath& groom, const std::string& path, TextureUndo undo,
                                    const std::string& scalpMapsJson) {
    groom_ = groom;
    interactive_ = true;
    texturePath_ = path;
    textureUndo_ = std::make_unique<TextureUndo>(std::move(undo));
    textureWritten_ = true;
    holdBytes(textureUndo_->bytes());
    if (!scalpMapsJson.empty()) modifier_.newPlugValueString(MFnDagNode(groom).findPlug("scalpMaps", true), scalpMapsJson.c_str());
    bumpBrushTick();
}

void BrushStrokeCmd::setTextureBytes(const MDagPath& groom, const std::string& path, FileBytes before, FileBytes after,
                                     const std::string& scalpMapsJson) {
    groom_ = groom;
    texturePath_ = path;
    static const FileBytes none = std::make_shared<const std::vector<std::uint8_t>>();
    textureBefore_ = before ? std::move(before) : none;
    textureAfter_ = after ? std::move(after) : none;
    textureWritten_ = true;
    holdBytes(textureBefore_->size() + textureAfter_->size());
    // The map moved to its own copy of a shared file: repointed in the same undo step.
    if (!scalpMapsJson.empty()) modifier_.newPlugValueString(MFnDagNode(groom).findPlug("scalpMaps", true), scalpMapsJson.c_str());
    bumpBrushTick();
}

void BrushStrokeCmd::writeTextureBefore() {
    if (!textureUndo_) {
        writeFileBytes(texturePath_, *textureBefore_);
        forgetWriter(texturePath_);
        return;
    }
    TextureUndo& u = *textureUndo_;
    PaintedTexture& painted = *u.painted;
    if (painted.revision != u.revisionAfter) throw std::runtime_error("the texture map's paint is not where this stroke left it");
    painted.paint->applyEdit(u.edit, false);
    painted.revision = u.revisionBefore;
    if (u.beforePainted) {
        // The file was another paint's PNG (the stroke's own was opened from it).
        if (u.beforePainted->revision != u.beforeRevision) throw std::runtime_error("the texture map's paint is not where it was");
        writePaintedFile(u.beforePainted, false);
    } else if (u.beforeBytes) {
        writeFileBytes(texturePath_, *u.beforeBytes);
        forgetWriter(texturePath_);
    } else {
        writePaintedFile(u.painted, false);
    }
}

void BrushStrokeCmd::writeTextureAfter() {
    if (!textureUndo_) {
        writeFileBytes(texturePath_, *textureAfter_);
        forgetWriter(texturePath_);
        return;
    }
    TextureUndo& u = *textureUndo_;
    PaintedTexture& painted = *u.painted;
    if (painted.revision != u.revisionBefore) throw std::runtime_error("the texture map's paint is not where this stroke found it");
    painted.paint->applyEdit(u.edit, true);
    painted.revision = u.revisionAfter;
    writePaintedFile(u.painted, false);
}

MStatus BrushStrokeCmd::redoIt() {
    return groomlab::maya::guarded("BrushStrokeCmd::redoIt", [&]() -> MStatus {
        if (!texturePath_.empty() && !textureWritten_) {
            writeTextureAfter();
            textureWritten_ = true;
        }
        return modifier_.doIt();
    });
}

MStatus BrushStrokeCmd::undoIt() {
    return groomlab::maya::guarded("BrushStrokeCmd::undoIt", [&]() -> MStatus {
        if (!texturePath_.empty() && textureWritten_) {
            writeTextureBefore();
            textureWritten_ = false;
        }
        return modifier_.undoIt();
    });
}

MStatus BrushStrokeCmd::finalize() {
    // Journal the stroke. Interactive strokes are marked so replaying them fails
    // clearly instead of doing something different from what the artist saw.
    MArgList journal;
    journal.addArg(commandString());
    journal.addArg(MString(kModeFlag[1]));
    journal.addArg(MString(mode_.c_str()));
    if (interactive_) {
        journal.addArg(MString(kInteractiveFlag[1]));
        journal.addArg(true);
    } else {
        journal.addArg(MString(kRadiusFlag[1]));
        journal.addArg(double(settings_.brush.radius));
        journal.addArg(MString(kStrengthFlag[1]));
        journal.addArg(double(settings_.brush.strength));
        if (settings_.targetsName() != "guides") {
            journal.addArg(MString(kTargetsFlag[1]));
            journal.addArg(MString(settings_.targetsName().c_str()));
        }
        if (settings_.brush.mode == BrushMode::ShapeTube) {
            journal.addArg(MString(kTubeChannelFlag[1]));
            journal.addArg(MString(tubeChannelName(settings_.brush.tubeChannel)));
        }
        if (mode_ == "place" || mode_ == "draw") {
            journal.addArg(MString(kPlaceTubeFlag[1]));
            journal.addArg(MString(placementTubeJson(settings_.place).c_str()));
        }
        for (const Vec3f& p : path_) {
            journal.addArg(MString(kPointFlag[1]));
            journal.addArg(double(p.x));
            journal.addArg(double(p.y));
            journal.addArg(double(p.z));
        }
    }
    journal.addArg(groom_.partialPathName());
    return MPxToolCommand::doFinalize(journal);
}

// ---------------------------------------------------------------------------
// Interactive context

// The interactive tool's host: the active groom's plugs and evaluation (its shape), previews
// through the shape, texture maps' files kept between strokes (the context's painted_), and each
// stroke committed as a groomLabBrushStroke (one undo step).
class ContextStrokeHost : public StrokeHost {
public:
    explicit ContextStrokeHost(BrushContext& context) : c_(context) {}

    bool attach() override { return c_.findGroom(); }
    bool prepareStroke() override {
        if (!c_.findGroom()) {
            MGlobal::displayWarning("GroomLab brush: select a groom (groomLabShape) to brush");
            return false;
        }
        if (MFnDagNode(c_.groom_).findPlug("locked", true).asBool()) {
            MGlobal::displayWarning("GroomLab brush: " + c_.groom_.partialPathName() + " is locked (unlock it in the Groom Manager)");
            return false;
        }
        if (!referenceEditAllowed(c_.groom_)) {
            const std::string name = c_.groom_.fullPathName().asChar();
            if (name != c_.warnedReference_) {  // once per groom, not per click
                c_.warnedReference_ = name;
                MGlobal::displayWarning(MString("GroomLab brush: ") + c_.groom_.partialPathName() + kReferenceRefused);
            }
            return false;
        }
        c_.lastRefreshEnd_ = {};
        // A stroke of the guides pressed while the last stroke's commit still evaluates chains on it
        // on the GPU, waiting for nothing (GroomShape::chainStroke); else the live session at the
        // frame shown (not a cached frame's).
        if (!(guidesStroke() && c_.shape_->chainStroke())) c_.shape_->ensureEvaluated();
        return true;
    }

    MeshPtr scalp() override {
        scalp_ = c_.shape_->evaluatedScalp();
        return scalp_;
    }
    MeshPtr paintScalp() override { return c_.shape_->evaluatedCageScalp(); }
    std::shared_ptr<const GuideHierarchy> authoredHierarchy() override { return c_.shape_->authoredHierarchy(); }
    std::shared_ptr<const GuideHierarchy> shownHierarchy() override { return c_.shape_->evaluatedHierarchy(); }
    std::uint64_t shownHierarchyVersion() override { return c_.shape_->hierarchyVersion(); }
    std::shared_ptr<const GuideHierarchy> posedHierarchy() override { return c_.shape_->posedHierarchy(); }
    std::string storedText(StrokeField field) override { return plug(strokeFieldName(field)).asString().asChar(); }
    const ScalpMaps& scalpMaps() override { return c_.shape_->scalpMaps(); }
    std::vector<GuideId> selectedGuides() override { return readGuideSelection(c_.groom_.node()); }
    float softSelectRadius() override { return readSoftSelectRadius(c_.groom_.node()); }
    std::set<std::string> hiddenGroups() override { return readHiddenGuideGroups(c_.groom_.node()); }
    int guideSmoothing() override { return MPlug(c_.shape_->thisMObject(), GroomShape::aGuideSmoothing).asInt(); }
    bool shotSculpt() override { return plug("shotSculpt").asBool(); }
    double seconds() override { return mayaSeconds(plug("time").asMTime()); }
    long frame() override { return std::lround(plug("time").asMTime().as(MTime::uiUnit())); }
    bool usesLiveCorrectiveWeights() override { return true; }
    float liveCorrectiveWeight(int slot) override { return correctiveWeightPlug(c_.groom_.node(), slot); }
    std::string scalpMapChannel() override { return plug("scalpMapChannel").asString().asChar(); }
    std::string sculptNode() override { return plug("sculptNode").asString().asChar(); }
    SculptInput sculptInput(const std::string& node) override { return c_.shape_->sculptInput(node); }
    void previewSculpt(const std::string& node, SculptPtr edits, std::shared_ptr<const GuideHierarchy> shown) override {
        c_.shape_->setPreviewSculpt(node, std::move(edits), std::move(shown));
    }

    std::shared_ptr<TexturePaint> texturePaint(const std::string& name, int channels, int resolution) override {
        refreshMayaPaths();
        const std::string full = textureMapFile(c_.groom_, *c_.shape_, name);  // the active groom's own file
        if (full.empty()) throw std::runtime_error("this groom has no texture map named " + name);
        // Go on painting the values the last stroke left while the file is what it wrote (as the
        // scalp and the map's channels); otherwise open the file as it reads now.
        std::shared_ptr<PaintedTexture>& painted = c_.painted_;
        if (!painted || !painted->continuable || !painted->holds(full, *scalp_, channels)) {
            auto opened = std::make_shared<PaintedTexture>();  // a new one: the undo queue holds the old
            try {
                requirePaintablePng(full);
                static_cast<KeptTexturePaint&>(*opened) = KeptTexturePaint::open(*scalp_, full, channels, resolution);
            } catch (...) {
                painted.reset();
                throw;
            }
            opened->writtenWrites = fileWrites();
            opened->continuable = true;
            painted = std::move(opened);
        }
        return painted->paint;
    }
    void dropTexturePaint() override { c_.painted_.reset(); }

    void previewHierarchy(std::shared_ptr<GuideHierarchy> preview, bool final) override {
        c_.shape_->setPreviewHierarchy(std::move(preview), final);
    }
    void previewOverlays(std::shared_ptr<const PartSet> parts, std::shared_ptr<const FlowSet> flow) override {
        c_.shape_->setPreviewOverlays(std::move(parts), std::move(flow));
    }
    void previewVertexMap(const std::string& name, std::shared_ptr<const std::vector<float>> values, int channels) override {
        c_.shape_->setPreviewVertexMap(name, std::move(values), channels);
    }
    void previewTextureMap(const std::string& name, std::shared_ptr<const TexturePreview> image) override {
        c_.shape_->setPreviewTextureMap(name, std::move(image));
    }
    void liveStroke(bool on) override { c_.shape_->setLiveStroke(on); }
    void warn(const std::string& message) override { MGlobal::displayWarning(message.c_str()); }

    void commit(StrokeEdit edit) override {
        auto* cmd = static_cast<BrushStrokeCmd*>(c_.newToolCommand());
        if (!edit.textureMap.empty()) commitTexture(edit, *cmd);
        cmd->setInteractiveEdit(c_.groom_, edit);
        cmd->redoIt();
        cmd->finalize();
    }

private:
    MPlug plug(const char* name) const { return MFnDagNode(c_.groom_).findPlug(name, true); }
    // A brush shaping guides alone (no map, no shot sculpt, no Sculpt node, none erased): what
    // the GPU draws.
    bool guidesStroke() {
        const BrushToolSettings& s = c_.tool_.settings;
        return s.tool == BrushToolSettings::Tool::Brush && !s.paintsScalp() && s.brush.mode != BrushMode::Erase &&
               !shotSculpt() && sculptNode().empty();
    }

    // A texture stroke's tiles: the file written now (this groom's own copy first when another
    // groom reads it too), and the undo record of what it held before.
    void commitTexture(StrokeEdit& edit, BrushStrokeCmd& cmd) {
        std::shared_ptr<PaintedTexture>& painted = c_.painted_;
        if (!painted || !painted->paint) throw std::runtime_error("the texture map's paint is gone");
        TextureUndo undo;
        undo.edit = std::move(edit.texture);
        const std::shared_ptr<TexturePaint> stroked = painted->paint;
        std::string mapsJson, full;
        bool counted = false;
        try {
            // Another groom reads the file too (a duplicate, a reference): this groom's own copy,
            // painted from now on; the shared file's paint goes back to before the stroke for its
            // strokes' undo (a copy of its values, when strokes in the undo queue hold it).
            full = ownTextureFile(c_.groom_, *c_.shape_, edit.textureMap, painted->path, &mapsJson);
            if (full != painted->path) {
                auto forked = std::make_shared<PaintedTexture>();
                forked->path = full;
                forked->paint = painted->paint;
                forked->scalp = painted->scalp;
                if (painted.use_count() > 1) {
                    auto back = std::make_shared<TexturePaint>(*painted->paint);
                    back->applyEdit(undo.edit, false);
                    painted->paint = std::move(back);
                }
                painted = std::move(forked);
            }
            // What the file holds before the stroke writes it: this paint's PNG as the last
            // stroke left it, another paint's (this one was opened from it), or bytes read now
            // (nothing wrote the file while the stroke lasted).
            const std::shared_ptr<PaintedTexture> writer = lastWriter(full);
            if (writer && writer != painted) {
                undo.beforePainted = writer;
                undo.beforeRevision = writer->revision;
            } else if (!writer) {
                undo.beforeBytes = readFileBytes(full);
            }
            undo.painted = painted;
            undo.revisionBefore = painted->revision;
            undo.revisionAfter = ++painted->revision;
            counted = true;
            // The PNG made once and written; what reading it gives goes to the texture cache.
            writePaintedFile(painted, true);
        } catch (...) {
            // Nothing written: the stroke's values go (the strokes before it undo from them).
            stroked->applyEdit(undo.edit, false);
            if (counted) --painted->revision;
            painted.reset();
            throw;
        }
        cmd.setTextureEdit(c_.groom_, full, std::move(undo), mapsJson);
    }

    BrushContext& c_;
    MeshPtr scalp_;  // the stroke's (texturePaint lays the map over it)
};

BrushContext::BrushContext() : host_(std::make_unique<ContextStrokeHost>(*this)) { setTitleString("GroomLab Brush"); }

BrushContext::~BrushContext() = default;

void BrushContext::getClassName(MString& name) const { name = "groomLabBrush"; }

void BrushContext::toolOnSetup(MEvent&) {
    setHelpString("Drag: brush the selected groom (Place: click one guide, drag for many; Draw: draw a guide "
                  "from the scalp). Shift+drag: smooth (Shape: the tubes' channel it paints). Ctrl+drag: paint "
                  "guides into the selection, the rest frozen; Ctrl+Shift+drag: lasso them (round none: deselect and "
                  "unfreeze all); Ctrl+click: add the "
                  "guides in the ring (on none: deselect and unfreeze all). Painting maps or guide attributes, "
                  "Ctrl is the opposite. "
                  "B/M or middle-drag: radius/strength. Double-click the tool for its settings.");
    installPenPressure();
    active_ = true;
    if (!findGroom()) MGlobal::displayWarning("GroomLab brush: select a groom (groomLabShape) to brush");
    showSymmetry();
}

void BrushContext::toolOffCleanup() {
    if (tool_.stroking()) guarded("tool off", [&] {
        endStroke(activeToolView());
        return MStatus(MS::kSuccess);
    });
    active_ = false;
    clearSymmetry();
    clearTubeWires();
    GroomShape::setBrushTarget(nullptr);  // what strokes keep (the stroke document) goes with the tool
    MPxContext::toolOffCleanup();
}

bool BrushContext::findGroom() {
    if (!activeGroom(groom_)) {
        shape_ = nullptr;
        return false;
    }
    shape_ = groomShape(groom_);
    // The groom about to be brushed: its stroke document is evaluated ahead in idle time.
    if (shape_) GroomShape::setBrushTarget(shape_);
    return shape_ != nullptr;
}

void BrushContext::expectGpuCommit() {
    // The dabs were the GPU's (the shape's evaluation of each stopped at its node), so what the
    // stroke commits is drawn already; shot sculpting commits correctives through an evaluation of
    // its own (the posed guides): in compute.
    if (shape_ && tool_.stroking() && !shape_->gpuStroke().empty() &&
        !MFnDagNode(groom_).findPlug("shotSculpt", true).asBool()) {
        shape_->expectCommit(GroomShape::CommitKind::Guides);
    }
}

void BrushContext::unchainUnlessStroking() {
    if (shape_ && !tool_.stroking()) shape_->endChain();
}

void BrushContext::endStroke(const ToolView& view) {
    expectGpuCommit();
    tool_.end(*host_, view);
    unchainUnlessStroking();
    // The commit dirtied the groom, which marks its drawing stale for the next redraw; nothing
    // else asks for one after a release, so the new arrows or parts would wait for the camera to
    // move. Ask now.
    if (shape_) M3dView::active3dView().refresh(false, true);
}

bool BrushContext::simulateStroke(const std::vector<std::pair<short, short>>& points, bool ctrl, bool shift, bool draw) {
    const ToolView view = activeToolView();
    BrushToolSettings& s = tool_.settings;
    const BrushMode saved = s.brush.mode;
    if (shift) useShiftSmoothing(s.brush);  // as a Shift+drag does (the Shape brush: smooth its channel)
    debugStrokeTimes_.clear();
    debugStrokeDraws_.clear();
    using Clock = std::chrono::steady_clock;
    auto start = Clock::now();
    // GROOMLAB_DEBUG_STROKE_SYNC=1: each step's time runs until the GPU has drawn it (its CPU part and
    // the wait for the GPU given apart: benchmarks).
    const char* syncVar = std::getenv("GROOMLAB_DEBUG_STROKE_SYNC");
    const bool syncSteps = syncVar && syncVar[0] == '1';
    auto lap = [&] {
        const double cpuMs = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
        if (syncSteps) waitForGpu();
        const double stepMs = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
        debugStrokeTimes_.push_back(stepMs);
        // The step's breakdown (not timed: the next step starts after it).
        char compute[224];
        const ChangesPtr changes = shape_ ? shape_->strandsChanges() : nullptr;
        std::snprintf(compute, sizeof compute, "{\"computeMs\": %.3f, \"changedStrands\": %lld, \"cpuMs\": %.3f, \"gpuWaitMs\": %.3f, \"draw\": ",
                      shape_ ? shape_->lastComputeMs() : 0.0,
                      changes && !changes->all ? static_cast<long long>(changes->strands.size()) : -1LL, cpuMs, stepMs - cpuMs);
        // (Not while the release's commit evaluates off the UI thread: the stats would wait for it.)
        debugStrokeDraws_.push_back(compute + lastDrawTimesJson() + ", \"stats\": " +
                                    (shape_ && !shape_->commitPending() ? shape_->graphStatsJson() : std::string("{}")) +
                                    ", \"pending\": " + (shape_ && shape_->commitPending() ? "true" : "false") + "}");
        start = Clock::now();
    };
    if (points.empty() || !tool_.begin(*host_, view, points[0].first, points[0].second, penPressure(), ctrl ? kStrokeCtrl : 0u)) {
        unchainUnlessStroking();
        endShiftSmoothing(s.brush, saved);
        return false;
    }
    if (draw) {
        M3dView::active3dView().refresh(false, true);  // the press, drawn
        lap();
    }
    for (std::size_t i = 1; i < points.size(); ++i) {
        tool_.moveTo(*host_, view, points[i].first, points[i].second, penPressure());
        if (draw) {
            M3dView::active3dView().refresh(false, true);  // as a drag redraws, every time
            lap();
        }
    }
    const bool changed = tool_.changed();
    endStroke(view);  // the commit, drawn
    if (draw) lap();
    endShiftSmoothing(s.brush, saved);
    return changed;
}

std::string BrushContext::debugStrokeDraws() const {
    std::string out = "[";
    for (std::size_t i = 0; i < debugStrokeDraws_.size(); ++i) out += (i ? ", " : "") + debugStrokeDraws_[i];
    return out + "]";
}

int BrushContext::pickLevel(short x, short y) {
    const int level = tool_.pickLevel(*host_, activeToolView(), x, y);
    if (level == -2) return level;
    MToolsInfo::setDirtyFlag(*this);
    // The group name goes into a MEL string: escape it (names come from the artist).
    std::string escaped;
    for (const char c : tool_.settings.brush.group) {
        if (c == '"' || c == '\\') escaped += '\\';
        escaped += c;
    }
    MGlobal::executeCommandOnIdle(MString("inViewMessage -fade -pos topCenter -amg \"GroomLab: solo, group ") + escaped.c_str() + "\"");
    return level;
}

std::vector<std::vector<Vec3f>> BrushContext::affectedLines(short x, short y) {
    return tool_.affectedLines(*host_, activeToolView(), x, y);
}

MStatus BrushContext::guarded(const char* what, const std::function<MStatus()>& fn) {
    // Drop the stroke rather than the session.
    auto dropStroke = [&](const char* why) {
        tool_.cancel(shape_ ? host_.get() : nullptr);
        painted_.reset();  // the texture map is opened again from its file next time
        MGlobal::displayWarning(MString("GroomLab brush (") + what + "): " + why);
        return MS::kFailure;
    };
    try {
        return fn();
    } catch (const std::exception& e) {
        return dropStroke(e.what());
    } catch (...) {
        return dropStroke("unexpected error");
    }
}

MStatus BrushContext::doPress(MEvent& event, MHWRender::MUIDrawManager& draw, const MHWRender::MFrameContext&) {
    return guarded("press", [&] { return press(event, draw); });
}
MStatus BrushContext::doDrag(MEvent& event, MHWRender::MUIDrawManager& draw, const MHWRender::MFrameContext&) {
    return guarded("drag", [&] { return drag(event, draw); });
}
MStatus BrushContext::doRelease(MEvent& event, MHWRender::MUIDrawManager&, const MHWRender::MFrameContext&) {
    return guarded("release", [&] { return release(event); });
}
MStatus BrushContext::doPtrMoved(MEvent& event, MHWRender::MUIDrawManager& draw, const MHWRender::MFrameContext&) {
    return guarded("hover", [&] {
        short x = 0, y = 0;
        event.getPosition(x, y);
        showSymmetry();
        drawAffected(draw, x, y);
        drawCircle(draw, x, y);
        return MStatus(MS::kSuccess);
    });
}

MStatus BrushContext::press(MEvent& event, MHWRender::MUIDrawManager& draw) {
    short x = 0, y = 0;
    event.getPosition(x, y);
    const bool slider = event.mouseButton() == MEvent::kLeftMouse && !dragSlider.empty();
    if (event.mouseButton() == MEvent::kMiddleMouse || slider) {
        // Middle-drag left/right resizes the brush; with Shift it changes strength.
        // B / M held: the same with the left button.
        adjusting_ = true;
        adjustStrength_ = slider ? dragSlider == "strength" : event.isModifierShift();
        adjustX_ = x;
        adjustY_ = y;
        adjustRadius_ = tool_.settings.screenRadius;
        adjustValue_ = tool_.settings.brush.strength;
        drawCircle(draw, x, y);
        return MS::kSuccess;
    }
    if (event.mouseButton() == MEvent::kLeftMouse && event.isModifierControl() && tool_.settings.ctrlSelects()) {
        // Ctrl on a guide brush: selecting, not brushing (a drag paints, a click adds the guides
        // in the ring: release); with Shift a drag lassos.
        beginPaintSelection(x, y, event.isModifierShift());
        drawCircle(draw, x, y);
        return MS::kSuccess;
    }
    const unsigned modifiers = (event.isModifierControl() ? kStrokeCtrl : 0u) | (event.isModifierShift() ? kStrokeShift : 0u);
    tool_.press(*host_, activeToolView(), x, y, penPressure(), modifiers);
    unchainUnlessStroking();
    showSymmetry();
    drawCircle(draw, x, y);
    return MS::kSuccess;
}

MStatus BrushContext::drag(MEvent& event, MHWRender::MUIDrawManager& draw) {
    short x = 0, y = 0;
    event.getPosition(x, y);
    BrushToolSettings& s = tool_.settings;
    if (adjusting_) {
        const float dx = float(x - adjustX_);
        MString label;
        if (adjustStrength_) {
            s.brush.strength = std::clamp(adjustValue_ + dx * 0.01f, -10.0f, 10.0f);
            label = MString("strength ") + s.brush.strength;
        } else {
            s.screenRadius = std::clamp(adjustRadius_ + dx, 5.0f, 2000.0f);
            label = MString("radius ") + static_cast<int>(s.screenRadius);
        }
        drawCircle(draw, adjustX_, adjustY_);
        draw.beginDrawable();
        draw.setColor(MColor(1.0f, 1.0f, 1.0f));
        draw.text2d(MPoint(adjustX_, adjustY_ + s.screenRadius + 12), label, MHWRender::MUIDrawManager::kCenter);
        draw.endDrawable();
        return MS::kSuccess;
    }
    if (painting_) {
        movePaintSelection(x, y);
        drawLasso(draw);
        drawCircle(draw, x, y);
        return MS::kSuccess;
    }
    tool_.drag(*host_, activeToolView(), x, y, penPressure());
    unchainUnlessStroking();
    if (tool_.stroking()) refreshDuringDrag();
    using Tool = BrushToolSettings::Tool;
    const std::vector<Vec3f>& path = tool_.drawnPath();
    if (tool_.stroking() && (s.tool == Tool::Part || s.tool == Tool::Flow) && path.size() > 1) {
        // The part so far, on the scalp.
        MPointArray line;
        for (const Vec3f& p : path) line.append(MPoint(p.x, p.y, p.z));
        draw.beginDrawable();
        draw.setColor(mayaColor(brushPathColor(s)));
        draw.setLineWidth(3.0f);
        draw.setDepthPriority(5);
        draw.lineStrip(line, false);
        draw.endDrawable();
    }
    showSymmetry();
    drawCircle(draw, x, y);
    return MS::kSuccess;
}

void BrushContext::refreshDuringDrag() {
    // Redraw now, unless the last redraw (the groom's evaluation and drawing) took longer than
    // the time since it ended: the events then come faster than the groom can follow, and the
    // view is redrawn once Maya is idle instead (the release always redraws).
    const auto start = std::chrono::steady_clock::now();
    if (lastRefreshEnd_ != std::chrono::steady_clock::time_point{} &&
        lastRefreshMs_ > std::chrono::duration<double, std::milli>(start - lastRefreshEnd_).count()) {
        M3dView::active3dView().scheduleRefresh();
        return;
    }
    M3dView::active3dView().refresh(false, true);
    lastRefreshEnd_ = std::chrono::steady_clock::now();
    lastRefreshMs_ = std::chrono::duration<double, std::milli>(lastRefreshEnd_ - start).count();
}

void BrushContext::beginPaintSelection(short x, short y, bool lasso) {
    painting_ = true;
    paintMoved_ = false;
    lasso_ = lasso;
    paintX_ = x;
    paintY_ = y;
    paintBefore_ = host_->selectedGuides();
    paintedGuides_.clear();
    lassoPath_ = {{float(x), float(y)}};
}

void BrushContext::movePaintSelection(short x, short y) {
    // Once it moves it paints (from the press on): the guides under the ring join the selection.
    // A lasso grows its path (a point every 2 pixels).
    if (!paintMoved_ && std::abs(x - paintX_) + std::abs(y - paintY_) >= 4) {
        paintMoved_ = true;
        if (!lasso_) paintSelection(paintX_, paintY_);
    }
    if (lasso_) {
        const std::array<float, 2>& last = lassoPath_.back();
        if (std::abs(float(x) - last[0]) + std::abs(float(y) - last[1]) >= 2.0f) lassoPath_.push_back({float(x), float(y)});
        return;
    }
    if (paintMoved_) paintSelection(x, y);
}

void BrushContext::drawLasso(MHWRender::MUIDrawManager& draw) const {
    if (!painting_ || !lasso_ || lassoPath_.size() < 2) return;
    draw.beginDrawable();
    draw.setColor(MColor(1.0f, 1.0f, 1.0f));
    draw.setLineWidth(1.5f);
    for (std::size_t i = 0; i < lassoPath_.size(); ++i) {
        const std::array<float, 2>& a = lassoPath_[i];
        const std::array<float, 2>& b = lassoPath_[(i + 1) % lassoPath_.size()];  // closed back to the press
        draw.line2d(MPoint(a[0], a[1]), MPoint(b[0], b[1]));
    }
    draw.endDrawable();
}

void BrushContext::finishPaintSelection() {
    painting_ = false;
    GuidePick pick;
    pick.level = tool_.settings.brush.level;
    pick.group = tool_.settings.brush.group;
    pick.symmetry = tool_.settings.symmetryName();
    if (!paintMoved_) {
        // A click: the guides in the ring added, the rest frozen; on none, nothing selected and
        // every guide unfrozen.
        std::vector<std::array<float, 2>> ring;
        const float r = std::max(tool_.settings.screenRadius, 1.0f);
        for (int k = 0; k < 24; ++k) {
            const float a = 6.2831853f * static_cast<float>(k) / 24.0f;
            ring.push_back({float(paintX_) + r * std::cos(a), float(paintY_) + r * std::sin(a)});
        }
        const std::vector<GuideId> picked = pickGuidesInView(ring, pick);
        applyGuideSelection(picked, picked.empty() ? "replace" : "add", true, true);
    } else if (lasso_) {
        // The guides inside the lasso added, the rest frozen: one undo step. Round none (off the
        // guides): nothing selected and every guide unfrozen, as a click on none.
        const std::vector<GuideId> lassoed = pickGuidesInView(lassoPath_, pick);
        std::vector<GuideId> selection = lassoed.empty() ? std::vector<GuideId>{} : paintBefore_;
        for (const GuideId id : lassoed) {
            if (std::find(selection.begin(), selection.end(), id) == selection.end()) selection.push_back(id);
        }
        applyGuideSelection(selection, "replace", true, true);
    } else {
        // The painted selection as one undo step (the rest frozen): back to the press's, then
        // that and what was painted.
        std::vector<GuideId> selection = paintBefore_;
        for (const GuideId id : paintedGuides_) {
            if (std::find(selection.begin(), selection.end(), id) == selection.end()) selection.push_back(id);
        }
        applyGuideSelection(paintBefore_, "replace", false, false);
        applyGuideSelection(selection, "replace", true, true);
    }
    paintedGuides_.clear();
    lassoPath_.clear();
}

int BrushContext::simulateSelectPaint(const std::vector<std::pair<short, short>>& points, bool lasso) {
    if (points.empty() || !findGroom()) return 0;
    beginPaintSelection(points.front().first, points.front().second, lasso);
    for (std::size_t i = 1; i < points.size(); ++i) movePaintSelection(points[i].first, points[i].second);
    finishPaintSelection();
    return static_cast<int>(host_->selectedGuides().size());
}

void BrushContext::paintSelection(short x, short y) {
    // The ring as a polygon in view pixels: the guides with a point inside it.
    std::vector<std::array<float, 2>> ring;
    const float r = tool_.settings.screenRadius;
    for (int k = 0; k < 24; ++k) {
        const float a = 6.2831853f * static_cast<float>(k) / 24.0f;
        ring.push_back({float(x) + r * std::cos(a), float(y) + r * std::sin(a)});
    }
    GuidePick pick;
    pick.level = tool_.settings.brush.level;
    pick.group = tool_.settings.brush.group;
    pick.symmetry = tool_.settings.symmetryName();
    std::vector<GuideId> fresh;
    for (const GuideId id : pickGuidesInView(ring, pick)) {
        if (paintedGuides_.insert(id).second) fresh.push_back(id);
    }
    // Shown live, as no undo step (the release makes the one), drawn now (not when the camera
    // next moves).
    if (fresh.empty()) return;
    applyGuideSelection(fresh, "add", false, false);
    refreshDuringDrag();
}

MStatus BrushContext::release(MEvent&) {
    if (adjusting_) {
        adjusting_ = false;
        MToolsInfo::setDirtyFlag(*this);  // refresh the Tool Settings window
        return MS::kSuccess;
    }
    if (painting_) {
        finishPaintSelection();
        M3dView::active3dView().refresh(true, true);  // the selection drawn now (the Select tool's release)
        return MS::kSuccess;
    }
    expectGpuCommit();
    const StrokeRelease released = tool_.release(*host_, activeToolView());
    unchainUnlessStroking();
    if (released == StrokeRelease::Stroke && shape_) {
        M3dView::active3dView().refresh(false, true);  // as endStroke: the commit's drawing now
    }
    return MS::kSuccess;
}

void BrushContext::drawAffected(MHWRender::MUIDrawManager& draw, short x, short y) {
    const std::vector<std::vector<Vec3f>> lines = affectedLines(x, y);
    const BrushToolSettings& s = tool_.settings;
    if (lines.empty() && (s.tool != BrushToolSettings::Tool::Brush || !shape_)) return;
    draw.beginDrawable();
    draw.setColor(mayaColor(brushHoverColor(s)));
    draw.setLineWidth(3.0f);
    draw.setDepthPriority(6);
    for (const std::vector<Vec3f>& points : lines) {
        MPointArray line;
        for (const Vec3f& p : points) line.append(MPoint(p.x, p.y, p.z));
        draw.lineStrip(line, false);
    }
    draw.endDrawable();
}

void BrushContext::clearSymmetry() {
    if (seamOn_.isAlive() && seamOn_.isValid()) {
        MFnDependencyNode node(seamOn_.object());
        if (auto* shape = dynamic_cast<GroomShape*>(node.userNode())) shape->setSymmetrySeam({});
    }
    seamOn_ = MObjectHandle();
}

void BrushContext::clearTubeWires() {
    if (tubeWiresOn_.isAlive() && tubeWiresOn_.isValid()) {
        MFnDependencyNode node(tubeWiresOn_.object());
        if (auto* shape = dynamic_cast<GroomShape*>(node.userNode())) shape->setTubeWires(false);
    }
    tubeWiresOn_ = MObjectHandle();
}

void BrushContext::showTubeWires() {
    const BrushToolSettings& s = tool_.settings;
    const bool on = active_ && shape_ && s.tool == BrushToolSettings::Tool::Brush && s.brush.mode == BrushMode::ShapeTube;
    if (!on || !(tubeWiresOn_.isAlive() && tubeWiresOn_.isValid() && tubeWiresOn_.object() == shape_->thisMObject())) {
        clearTubeWires();  // off, or on another groom
    }
    if (!on) return;
    tubeWiresOn_ = MObjectHandle(shape_->thisMObject());
    shape_->setTubeWires(true);
}

void BrushContext::showSymmetry() {
    showTubeWires();  // (called wherever the tool, its mode or its groom may have changed)
    const BrushSettings& b = tool_.settings.brush;
    const bool topology = b.mirrorTopology;
    const int axis = topology ? -1 : b.mirrorAxis;
    if (!active_ || !shape_ || !host_ || (!topology && (axis < 0 || axis > 2))) {
        clearSymmetry();
        return;
    }
    const MeshPtr scalp = host_->scalp();
    if (!scalp || scalp->triangleCount() == 0) {
        clearSymmetry();
        return;
    }
    if (!(seamOn_.isAlive() && seamOn_.isValid() && seamOn_.object() == shape_->thisMObject())) {
        clearSymmetry();  // the line was on another groom
        seamOn_ = MObjectHandle(shape_->thisMObject());
    }
    if (seam_.scalp != scalp || seam_.axis != axis || seam_.center != b.mirrorCenter || seam_.topology != topology) {
        seam_.scalp = scalp;
        seam_.axis = axis;
        seam_.center = b.mirrorCenter;
        seam_.topology = topology;
        seam_.points.clear();
        // Lifted off the surface by a little of the scalp's size, so the line is not hidden in it.
        Vec3f lo = scalp->points().front(), hi = lo;
        for (const Vec3f& p : scalp->points()) {
            lo = Vec3f(std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z));
            hi = Vec3f(std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z));
        }
        seam_.points = symmetrySeam(*scalp, StrokeSymmetry::of(b, scalp.get()), 0.002f * (hi - lo).length());
    }
    shape_->setSymmetrySeam(seam_.points);  // (nothing when it is the line the groom has)
}

void BrushContext::drawCircle(MHWRender::MUIDrawManager& draw, short x, short y) const {
    const BrushToolSettings& s = tool_.settings;
    draw.beginDrawable();
    using Tool = BrushToolSettings::Tool;
    const bool creating = s.tool != Tool::Brush && s.tool != Tool::Flow;
    draw.setColor(mayaColor(brushRingColor(s)));  // the core's, the standalone app's too
    draw.setLineWidth(1.5f);
    draw.circle2d(MPoint(x, y), creating ? 6.0 : s.screenRadius, false);
    draw.endDrawable();
}

// ---------------------------------------------------------------------------
// groomLabBrushCtx

const char* BrushContextCmd::kName = "groomLabBrushCtx";

void* BrushContextCmd::creator() { return new BrushContextCmd(); }

MPxContext* BrushContextCmd::makeObj() {
    context_ = new BrushContext();
    return context_;
}

MStatus BrushContextCmd::appendSyntax() {
    MSyntax s = syntax();
    addBrushFlags(s);
    s.addFlag(kDebugStrokeFlag[0], kDebugStrokeFlag[1], MSyntax::kString);
    s.addFlag(kPressureFlag[0], kPressureFlag[1], MSyntax::kBoolean);
    s.addFlag(kEraseGuidesFlag[0], kEraseGuidesFlag[1], MSyntax::kBoolean);
    s.addFlag(kErasePartsFlag[0], kErasePartsFlag[1], MSyntax::kBoolean);
    s.addFlag(kEraseFlowFlag[0], kEraseFlowFlag[1], MSyntax::kBoolean);
    s.addFlag(kDragSliderFlag[0], kDragSliderFlag[1], MSyntax::kString);
    s.addFlag(kDebugPickFlag[0], kDebugPickFlag[1], MSyntax::kString);
    s.addFlag(kDebugHoverFlag[0], kDebugHoverFlag[1], MSyntax::kString);
    s.addFlag(kHeldBytesFlag[0], kHeldBytesFlag[1]);
    s.addFlag(kPreviewUploadFlag[0], kPreviewUploadFlag[1]);
    s.addFlag(kFileImageFlag[0], kFileImageFlag[1]);
    s.addFlag(kDebugStrokeTimesFlag[0], kDebugStrokeTimesFlag[1]);
    s.addFlag(kDebugStrokeDrawsFlag[0], kDebugStrokeDrawsFlag[1]);
    return MS::kSuccess;
}

MStatus BrushContextCmd::doEditFlags() {
    return groomlab::maya::guarded("BrushContextCmd::doEditFlags", [&]() -> MStatus {
        if (!context_) return MS::kFailure;
        MArgParser args = parser();
        BrushToolSettings& s = context_->settings();
        const MStatus flags = applyBrushFlags(args, s, kName);
        if (!flags) return flags;
        MString text;
        double d = 0;
        bool b = false;
        if (args.isFlagSet(kRadiusFlag[0]) && args.getFlagArgument(kRadiusFlag[0], 0, d)) s.setScreenRadius(float(d));
        if (args.isFlagSet(kPressureFlag[0]) && args.getFlagArgument(kPressureFlag[0], 0, b)) s.pressure = b;
        if (args.isFlagSet(kDragSliderFlag[0]) && args.getFlagArgument(kDragSliderFlag[0], 0, text)) {
            if (text != "radius" && text != "strength" && text != "none" && text != "") {
                return fail(kName, "-dragSlider takes radius, strength or none");
            }
            context_->dragSlider = text == "none" ? "" : text.asChar();
        }
        if (args.isFlagSet(kEraseGuidesFlag[0]) && args.getFlagArgument(kEraseGuidesFlag[0], 0, b)) s.eraseGuides = b;
        if (args.isFlagSet(kErasePartsFlag[0]) && args.getFlagArgument(kErasePartsFlag[0], 0, b)) s.eraseParts = b;
        if (args.isFlagSet(kEraseFlowFlag[0]) && args.getFlagArgument(kEraseFlowFlag[0], 0, b)) s.eraseFlow = b;
        MToolsInfo::setDirtyFlag(*context_);
        context_->showSymmetry();  // the line follows the symmetry set (or goes with it)

        // Testing aid: run a stroke through the interactive code path from view
        // coordinates "x y x y ..." (needs the Maya UI; Maya ignores synthetic mouse input).
        if (args.isFlagSet(kDebugPickFlag[0]) && args.getFlagArgument(kDebugPickFlag[0], 0, text)) {
            MStringArray words;
            text.split(' ', words);
            if (words.length() != 2) return fail(kName, "-debugPick takes \"x y\"");
            setResult(context_->pickLevel(static_cast<short>(words[0].asInt()), static_cast<short>(words[1].asInt())));
        }
        // Testing aid: what hovering at "x y" lights up (the guides the brush would touch), counted.
        if (args.isFlagSet(kDebugHoverFlag[0]) && args.getFlagArgument(kDebugHoverFlag[0], 0, text)) {
            MStringArray words;
            text.split(' ', words);
            if (words.length() != 2) return fail(kName, "-debugHover takes \"x y\"");
            setResult(static_cast<int>(
                context_->affectedLines(static_cast<short>(words[0].asInt()), static_cast<short>(words[1].asInt())).size()));
        }
        if (args.isFlagSet(kDebugStrokeFlag[0]) && args.getFlagArgument(kDebugStrokeFlag[0], 0, text)) {
            MStringArray words;
            text.split(' ', words);
            // "x y x y ...", or "ctrl x y ..." / "shift x y ..." for a stroke with Ctrl (Shift) held.
            // "draw x y ...": the viewport redrawn after each point, as a drag does.
            // "select x y ...": a Ctrl drag painting the selection (one point: a click adding the
            // guides in the ring, on none clearing); "lasso x y ...": a Ctrl+Shift drag lassoing
            // it. The result is how many guides are selected after it.
            if (words.length() >= 3 && (words[0] == "select" || words[0] == "lasso")) {
                std::vector<std::pair<short, short>> points;
                for (unsigned i = 1; i + 1 < words.length(); i += 2) {
                    points.emplace_back(static_cast<short>(words[i].asInt()), static_cast<short>(words[i + 1].asInt()));
                }
                setResult(context_->simulateSelectPaint(points, words[0] == "lasso"));
                return MS::kSuccess;
            }
            const bool draw = words.length() > 0 && words[0] == "draw";
            if (draw) words.remove(0);
            const bool ctrl = words.length() > 0 && words[0] == "ctrl";
            const bool shift = words.length() > 0 && words[0] == "shift";
            std::vector<std::pair<short, short>> points;
            for (unsigned i = ctrl || shift ? 1 : 0; i + 1 < words.length(); i += 2) {
                points.emplace_back(static_cast<short>(words[i].asInt()), static_cast<short>(words[i + 1].asInt()));
            }
            setResult(context_->simulateStroke(points, ctrl, shift, draw));
        }
        return MS::kSuccess;
    });
}

MStatus BrushContextCmd::doQueryFlags() {
    return groomlab::maya::guarded("BrushContextCmd::doQueryFlags", [&]() -> MStatus {
        if (!context_) return MS::kFailure;
        MArgParser args = parser();
        const BrushToolSettings& s = context_->settings();
        if (args.isFlagSet(kModeFlag[0])) setResult(MString(context_->toolModeName().c_str()));
        else if (args.isFlagSet(kRadiusFlag[0])) setResult(double(s.screenRadius));
        else if (args.isFlagSet(kStrengthFlag[0])) setResult(double(s.brush.strength));
        else if (args.isFlagSet(kLevelFlag[0])) setResult(s.brush.level);
        else if (args.isFlagSet(kGroupFlag[0])) setResult(MString(s.brush.group.c_str()));
        else if (args.isFlagSet(kPropagateFlag[0])) setResult(s.brush.propagate);
        else if (args.isFlagSet(kLengthFlag[0])) setResult(double(s.place.length));
        else if (args.isFlagSet(kSegmentsFlag[0])) setResult(s.place.segments);
        else if (args.isFlagSet(kTubeRadiusFlag[0])) {
            setResult(double(s.place.tube.width.empty() ? 0.1f : s.place.tube.width.front()));
        }
        else if (args.isFlagSet(kSpacingFlag[0])) setResult(double(s.place.spacing));
        else if (args.isFlagSet(kMatchFlag[0])) setResult(s.place.matchNeighbors);
        else if (args.isFlagSet(kSoloNewFlag[0])) setResult(s.place.soloNewGuides);
        else if (args.isFlagSet(kPlaceTubeFlag[0])) setResult(MString(placementTubeJson(s.place).c_str()));
        else if (args.isFlagSet(kCollideFlag[0])) setResult(s.brush.collide);
        else if (args.isFlagSet(kMaskScalpFlag[0])) setResult(s.maskByScalp);
        else if (args.isFlagSet(kStretchFlag[0])) setResult(s.brush.stretch);
        else if (args.isFlagSet(kTubeChannelFlag[0])) setResult(MString(tubeChannelName(s.brush.tubeChannel)));
        else if (args.isFlagSet(kPressureFlag[0])) setResult(s.pressure);
        else if (args.isFlagSet(kPartNameFlag[0])) setResult(MString(s.partName.c_str()));
        else if (args.isFlagSet(kFlowSinkFlag[0])) setResult(MString(s.flowSink.c_str()));
        else if (args.isFlagSet(kSelectedOnlyFlag[0])) setResult(s.selectedOnly);
        else if (args.isFlagSet(kEraseGuidesFlag[0])) setResult(s.eraseGuides);
        else if (args.isFlagSet(kErasePartsFlag[0])) setResult(s.eraseParts);
        else if (args.isFlagSet(kEraseFlowFlag[0])) setResult(s.eraseFlow);
        else if (args.isFlagSet(kTargetsFlag[0])) setResult(MString(s.targetsName().c_str()));
        else if (args.isFlagSet(kSymmetryFlag[0])) setResult(MString(s.symmetryName().c_str()));
        else if (args.isFlagSet(kSymmetryCenterFlag[0])) setResult(double(s.brush.mirrorCenter));
        else if (args.isFlagSet(kAttributeFlag[0])) setResult(MString(s.brush.attribute.c_str()));
        else if (args.isFlagSet(kValueFlag[0])) setResult(double(s.brush.value));
        else if (args.isFlagSet(kPaintKindFlag[0])) setResult(MString(s.paintKind.c_str()));
        else if (args.isFlagSet(kPaintTextureFlag[0])) setResult(MString(s.paintTexture.c_str()));
        else if (args.isFlagSet(kPaintResolutionFlag[0])) setResult(s.paintResolution);
        else if (args.isFlagSet(kHardnessFlag[0])) setResult(double(s.brush.hardness));
        else if (args.isFlagSet(kFalloffProfileFlag[0])) setResult(MString(s.falloffProfileJson().c_str()));
        else if (args.isFlagSet(kStrengthProfileFlag[0])) setResult(MString(s.strengthProfileJson().c_str()));
        else if (args.isFlagSet(kSmoothStrengthFlag[0])) setResult(double(s.smoothStrength));
        else if (args.isFlagSet(kPaintColorFlag[0])) {
            // A context command returns no arrays: "r g b a".
            MString color;
            for (std::size_t k = 0; k < 4; ++k) {
                if (k) color += " ";
                color += MString() + static_cast<double>(s.paintColor[k]);
            }
            setResult(color);
        } else if (args.isFlagSet(kPaintChannelsFlag[0])) setResult(MString(channelLetters(s.paintChannels).c_str()));
        else if (args.isFlagSet(kPaintModeFlag[0])) setResult(MString(paintModeName(s.brush.paintMode)));
        else if (args.isFlagSet(kDragSliderFlag[0])) setResult(MString(context_->dragSlider.empty() ? "none" : context_->dragSlider.c_str()));
        // Testing aids: what the texture strokes in the undo queue keep (bytes), and how the
        // viewport sends a stroke's texels to the map's texture.
        else if (args.isFlagSet(kHeldBytesFlag[0])) setResult(static_cast<double>(BrushStrokeCmd::heldTextureBytes()));
        else if (args.isFlagSet(kPreviewUploadFlag[0])) setResult(MString(scalpMapUploadMode().c_str()));
        else if (args.isFlagSet(kFileImageFlag[0])) setResult(MString(scalpMapFileImageSource().c_str()));
        else if (args.isFlagSet(kDebugStrokeTimesFlag[0])) {
            // The last drawn -debugStroke's steps, ms each to its redrawn frame: "press dab ... release".
            MString times;
            for (const double ms : context_->debugStrokeTimes()) times += MString(times.length() ? " " : "") + ms;
            setResult(times);
        } else if (args.isFlagSet(kDebugStrokeDrawsFlag[0])) {
            setResult(MString(context_->debugStrokeDraws().c_str()));
        }
        return MS::kSuccess;
    });
}

}  // namespace groomlab::maya
