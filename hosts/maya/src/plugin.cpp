// SPDX-License-Identifier: Apache-2.0
#include <maya/MDrawRegistry.h>
#include <maya/MFnPlugin.h>
#include <maya/MCallbackIdArray.h>
#include <maya/MConditionMessage.h>
#include <maya/MGlobal.h>
#include <maya/MFileIO.h>
#include <maya/MSceneMessage.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#include "brush_tool.h"
#include "convert_cmd.h"
#include "crash_log.h"
#include "display_theme.h"
#include "export_cmds.h"
#include "gpu_preview.h"
#include "groom_draw_override.h"
#include "groom_shape.h"
#include "groomlab/process_caches.h"
#include "groomlab/version.h"
#include "guides_cmd.h"
#include "import_cmd.h"
#include "file_node.h"
#include "marker_set.h"
#include "parts_cmd.h"
#include "scalp_maps_cmd.h"
#include "pen_pressure.h"
#include "maya_inputs.h"
#include "reference_tool.h"
#include "select_tool.h"
#include "transfer_cmd.h"
#include "sim_cmd.h"
#include "version_cmd.h"

using groomlab::maya::BrushContextCmd;
using groomlab::maya::BrushStrokeCmd;
using groomlab::maya::DefaultGraphCmd;
using groomlab::maya::ExportGroomCmd;
using groomlab::maya::ExportUsdCmd;
using groomlab::maya::GroomShape;
using groomlab::maya::GroomShapeUI;
using groomlab::maya::GroomSubSceneOverride;
using groomlab::maya::GuideSelectContextCmd;
using groomlab::maya::GuidesCmd;
using groomlab::maya::OperatorsCmd;
using groomlab::maya::PartsCmd;
using groomlab::maya::TransferCmd;
using groomlab::maya::PathsCmd;
using groomlab::maya::VersionCmd;

namespace {
MCallbackIdArray gCallbacks;
// The name Maya loaded the plugin under: "GroomLab", or a development hot reload's copy
// (GroomLab_<tag>.mll, swapped in without restarting Maya).
std::string gLoadedAs;

// A scene a hot-reload copy saved requires that copy ("GroomLab_hot_1793..."), which no other
// session has: opened as Maya starts (a double-clicked file), before GroomLab loads, its grooms
// would come in as unknown nodes. A Maya ASCII scene it saves is given "GroomLab" back in its
// requires line (the header only; a binary scene is left as it is, with a warning).
void requireGroomLabByName(void*) {
    try {
        const std::string path = MFileIO::currentFile().asChar();
        if (path.size() < 3) return;
        const std::string ext = path.substr(path.size() - 3);
        if (ext == ".mb" || ext == ".MB") {
            MGlobal::displayWarning(MString("GroomLab: this scene requires the plugin as \"") + gLoadedAs.c_str() +
                                    "\" (a development build): save it as Maya ASCII, or with GroomLab.mll loaded, "
                                    "for it to open anywhere");
            return;
        }
        std::ifstream in(std::filesystem::u8path(path), std::ios::binary);
        if (!in) return;
        std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        in.close();
        const std::size_t header = std::min(text.find("\ncreateNode "), text.size());
        const std::string from = "\"" + gLoadedAs + "\"";
        const std::size_t at = text.find(from);
        if (at == std::string::npos || at > header) return;
        text.replace(at, from.size(), "\"GroomLab\"");
        std::ofstream out(std::filesystem::u8path(path), std::ios::binary | std::ios::trunc);
        out << text;
    } catch (...) {
    }
}

void callGroomlab(const char* call) {
    MGlobal::executePythonCommand(MString("try:\n    import groomlab\n    groomlab.") + call +
                                  "()\nexcept ImportError:\n    pass\nexcept Exception as e:\n"
                                  "    import maya.cmds\n    maya.cmds.warning('GroomLab: %s' % e)");
}
}  // namespace

PLUGIN_EXPORT MStatus initializePlugin(MObject obj) {
    MFnPlugin plugin(obj, "GroomLab", groomlab::kVersionString, "Any");
    gLoadedAs = plugin.name().asChar();
    groomlab::maya::registerHostOperators();

    MStatus status = plugin.registerData(groomlab::maya::GroomFrameData::kTypeName, groomlab::maya::GroomFrameData::kTypeId,
                                         groomlab::maya::GroomFrameData::creator);
    CHECK_MSTATUS_AND_RETURN_IT(status);
    status = plugin.registerShape(GroomShape::kTypeName, GroomShape::kTypeId, GroomShape::creator,
                                          GroomShape::initialize, GroomShapeUI::creator,
                                          &GroomShape::kDrawDbClassification);
    CHECK_MSTATUS_AND_RETURN_IT(status);

    status = MHWRender::MDrawRegistry::registerSubSceneOverrideCreator(
        GroomShape::kDrawDbClassification, GroomSubSceneOverride::kRegistrantId, GroomSubSceneOverride::creator);
    CHECK_MSTATUS_AND_RETURN_IT(status);

    // A groom's files, one node each, for Maya's File Path Editor (file_node.h), listed there under
    // GroomLab for this session (registered on every load: the type goes with the plugin). Another
    // groomLab type kept in the preferences (a development build's) would hold the label: it goes.
    status = plugin.registerNode(groomlab::maya::GroomFileNode::kTypeName, groomlab::maya::GroomFileNode::kTypeId,
                                 groomlab::maya::GroomFileNode::creator, groomlab::maya::GroomFileNode::initialize);
    CHECK_MSTATUS_AND_RETURN_IT(status);
    MGlobal::executeCommand("for ($t in `filePathEditor -query -listRegisteredTypes`) "
                            "if (startsWith($t, \"groomLab\") && $t != \"groomLabFile.fileName\") filePathEditor -deregisterType $t;"
                            "if (!stringArrayContains(\"groomLabFile.fileName\", `filePathEditor -query -listRegisteredTypes`)) "
                            "filePathEditor -registerType \"groomLabFile.fileName\" -typeLabel \"GroomLab\" -temporary;");

    // The Transfer Groom window's marker points (marker_set.h).
    status = plugin.registerNode(groomlab::maya::MarkerSet::kTypeName, groomlab::maya::MarkerSet::kTypeId,
                                 groomlab::maya::MarkerSet::creator, groomlab::maya::MarkerSet::initialize,
                                 MPxNode::kLocatorNode, &groomlab::maya::MarkerSet::kDrawDbClassification);
    CHECK_MSTATUS_AND_RETURN_IT(status);
    status = MHWRender::MDrawRegistry::registerDrawOverrideCreator(groomlab::maya::MarkerSet::kDrawDbClassification,
                                                                   groomlab::maya::MarkerSet::kRegistrantId,
                                                                   groomlab::maya::MarkerSetDrawOverride::creator);
    CHECK_MSTATUS_AND_RETURN_IT(status);

    status = plugin.registerCommand(VersionCmd::kName, VersionCmd::creator);
    CHECK_MSTATUS_AND_RETURN_IT(status);
    status = plugin.registerCommand(DefaultGraphCmd::kName, DefaultGraphCmd::creator);
    CHECK_MSTATUS_AND_RETURN_IT(status);
    status = plugin.registerCommand(OperatorsCmd::kName, OperatorsCmd::creator);
    CHECK_MSTATUS_AND_RETURN_IT(status);
    status = plugin.registerCommand(PathsCmd::kName, PathsCmd::creator, PathsCmd::newSyntax);
    CHECK_MSTATUS_AND_RETURN_IT(status);
    status = plugin.registerCommand(groomlab::maya::ExpressionCmd::kName, groomlab::maya::ExpressionCmd::creator,
                                    groomlab::maya::ExpressionCmd::newSyntax);
    CHECK_MSTATUS_AND_RETURN_IT(status);
    status = plugin.registerCommand(groomlab::maya::DisplayThemeCmd::kName, groomlab::maya::DisplayThemeCmd::creator,
                                    groomlab::maya::DisplayThemeCmd::newSyntax);
    CHECK_MSTATUS_AND_RETURN_IT(status);

    status = plugin.registerCommand(GuidesCmd::kName, GuidesCmd::creator, GuidesCmd::newSyntax);
    CHECK_MSTATUS_AND_RETURN_IT(status);
    status = plugin.registerCommand(groomlab::maya::BakeCardsCmd::kName, groomlab::maya::BakeCardsCmd::creator,
                                    groomlab::maya::BakeCardsCmd::newSyntax);
    CHECK_MSTATUS_AND_RETURN_IT(status);
    status = plugin.registerCommand(groomlab::maya::SimulateCmd::kName, groomlab::maya::SimulateCmd::creator,
                                    groomlab::maya::SimulateCmd::newSyntax);
    CHECK_MSTATUS_AND_RETURN_IT(status);
    status = plugin.registerCommand(groomlab::maya::GraphStatsCmd::kName, groomlab::maya::GraphStatsCmd::creator,
                                    groomlab::maya::GraphStatsCmd::newSyntax);
    CHECK_MSTATUS_AND_RETURN_IT(status);
    status = plugin.registerCommand(PartsCmd::kName, PartsCmd::creator, PartsCmd::newSyntax);
    CHECK_MSTATUS_AND_RETURN_IT(status);
    status = plugin.registerCommand(groomlab::maya::ScalpMapsCmd::kName, groomlab::maya::ScalpMapsCmd::creator,
                                    groomlab::maya::ScalpMapsCmd::newSyntax);
    CHECK_MSTATUS_AND_RETURN_IT(status);
    status = plugin.registerCommand(TransferCmd::kName, TransferCmd::creator, TransferCmd::newSyntax);
    CHECK_MSTATUS_AND_RETURN_IT(status);
    status = plugin.registerCommand(groomlab::maya::CopyColorSetsCmd::kName, groomlab::maya::CopyColorSetsCmd::creator,
                                    groomlab::maya::CopyColorSetsCmd::newSyntax);
    CHECK_MSTATUS_AND_RETURN_IT(status);
    status = plugin.registerCommand(groomlab::maya::ConvertCmd::kName, groomlab::maya::ConvertCmd::creator,
                                    groomlab::maya::ConvertCmd::newSyntax);
    CHECK_MSTATUS_AND_RETURN_IT(status);
    status = plugin.registerContextCommand(BrushContextCmd::kName, BrushContextCmd::creator, BrushStrokeCmd::kName,
                                           BrushStrokeCmd::creator, BrushStrokeCmd::newSyntax);
    CHECK_MSTATUS_AND_RETURN_IT(status);
    status = plugin.registerContextCommand(GuideSelectContextCmd::kName, GuideSelectContextCmd::creator);
    CHECK_MSTATUS_AND_RETURN_IT(status);
    status = plugin.registerContextCommand(groomlab::maya::ReferenceFlowContextCmd::kName,
                                           groomlab::maya::ReferenceFlowContextCmd::creator);
    CHECK_MSTATUS_AND_RETURN_IT(status);
    status = plugin.registerCommand(groomlab::maya::CameraSolveCmd::kName, groomlab::maya::CameraSolveCmd::creator,
                                    groomlab::maya::CameraSolveCmd::newSyntax);
    CHECK_MSTATUS_AND_RETURN_IT(status);
    status = plugin.registerCommand(ExportUsdCmd::kName, ExportUsdCmd::creator, ExportUsdCmd::newSyntax);
    CHECK_MSTATUS_AND_RETURN_IT(status);
    status = plugin.registerCommand(groomlab::maya::ExportVrsceneCmd::kName, groomlab::maya::ExportVrsceneCmd::creator,
                                    groomlab::maya::ExportVrsceneCmd::newSyntax);
    CHECK_MSTATUS_AND_RETURN_IT(status);
    status = plugin.registerCommand(ExportGroomCmd::kName, ExportGroomCmd::creator, ExportGroomCmd::newSyntax);
    CHECK_MSTATUS_AND_RETURN_IT(status);
    status = plugin.registerCommand(groomlab::maya::ImportGroomCmd::kName, groomlab::maya::ImportGroomCmd::creator,
                                    groomlab::maya::ImportGroomCmd::newSyntax);
    CHECK_MSTATUS_AND_RETURN_IT(status);
    status = plugin.registerCommand(groomlab::maya::ExportGltfCmd::kName, groomlab::maya::ExportGltfCmd::creator,
                                    groomlab::maya::ExportGltfCmd::newSyntax);
    CHECK_MSTATUS_AND_RETURN_IT(status);
    status = plugin.registerCommand(groomlab::maya::ScaleMeshCmd::kName, groomlab::maya::ScaleMeshCmd::creator,
                                    groomlab::maya::ScaleMeshCmd::newSyntax);
    CHECK_MSTATUS_AND_RETURN_IT(status);
    status = plugin.registerCommand(groomlab::maya::BakeScaleMapsCmd::kName, groomlab::maya::BakeScaleMapsCmd::creator,
                                    groomlab::maya::BakeScaleMapsCmd::newSyntax);
    CHECK_MSTATUS_AND_RETURN_IT(status);

#if GROOMLAB_HAS_ALEMBIC
    using groomlab::maya::ExportAlembicCmd;
    status = plugin.registerCommand(ExportAlembicCmd::kName, ExportAlembicCmd::creator, ExportAlembicCmd::newSyntax);
    CHECK_MSTATUS_AND_RETURN_IT(status);
#endif
    // The GPU look preview (gpu_preview.h): on or off by the user's preference, read here.
    status = plugin.registerCommand(groomlab::maya::GpuPreviewCmd::kName, groomlab::maya::GpuPreviewCmd::creator,
                                    groomlab::maya::GpuPreviewCmd::newSyntax);
    CHECK_MSTATUS_AND_RETURN_IT(status);
    groomlab::maya::loadGpuPreviewPreference();

    // Process-wide hooks last, once every registration succeeded: Maya unloads a plugin whose
    // initializePlugin fails without calling uninitializePlugin, and they would point into it.
    groomlab::maya::installCrashLog();  // a stack in %TEMP% if Maya goes down without a crash report

    // Relative paths follow the project and scene folder. The Python side notes where a
    // scene's files live (groomlab.remember_scene_folder) and, when a save gives the scene
    // another name or folder, copies the files the grooms name into the new scene's own
    // folder and repoints them there, before the file is written (groomlab.follow_scene_files):
    // MSceneMessage callbacks, since scriptJob events coalesce and a save before the save
    // would go unseen.
    groomlab::maya::refreshMayaPaths();
    // Where the open scene's files live is noted now and when a scene opens, never just before
    // a save: a Save As has renamed the scene by then.
    callGroomlab("remember_scene_folder");
    for (const MSceneMessage::Message m : {MSceneMessage::kAfterNew, MSceneMessage::kAfterOpen}) {
        gCallbacks.append(MSceneMessage::addCallback(m, [](void*) {
            // Nothing may be thrown into Maya from a callback: it would end the session.
            try {
                groomlab::maya::refreshMayaPaths();
                callGroomlab("remember_scene_folder");
                callGroomlab("sync_file_nodes");  // the File Path Editor's nodes as the grooms resolve here
            } catch (...) {
            }
        }));
    }
    if (gLoadedAs != "GroomLab") gCallbacks.append(MSceneMessage::addCallback(MSceneMessage::kAfterSave, requireGroomLabByName));
    gCallbacks.append(MSceneMessage::addCallback(MSceneMessage::kAfterImport, [](void*) {
        try {
            callGroomlab("sync_file_nodes");
        } catch (...) {
        }
    }));
    // The process-wide caches (a distance field per collider surface, surface graphs, subdivision
    // plans, decoded textures, the array pools: process_caches.h) outlive the grooms that filled
    // them: let go before a new or opened scene (the closing scene's grooms free what they hold).
    // After a new scene everything again (the closing grooms' arrays went to the pools as they
    // were freed); after an open, the pools alone (the opened grooms may have made their fields).
    for (const MSceneMessage::Message m : {MSceneMessage::kBeforeNew, MSceneMessage::kBeforeOpen, MSceneMessage::kAfterNew}) {
        gCallbacks.append(MSceneMessage::addCallback(m, [](void*) {
            try {
                groomlab::releaseProcessCaches();
            } catch (...) {
            }
        }));
    }
    gCallbacks.append(MSceneMessage::addCallback(MSceneMessage::kAfterOpen, [](void*) {
        try {
            groomlab::releaseArrayPools();
        } catch (...) {
        }
    }));
    // Playback stopped: a hair grid made every few frames while playing is made for this frame.
    gCallbacks.append(MConditionMessage::addConditionCallback("playingBack", [](bool playing, void*) {
        if (playing) return;
        try {
            groomlab::maya::hairGridsPlaybackStopped();
        } catch (...) {
        }
    }));
    // Before the save: a Save As has renamed the scene by then, and the grooms repointed at
    // the new scene's files are what the file stores.
    gCallbacks.append(MSceneMessage::addCallback(MSceneMessage::kBeforeSave, [](void*) {
        try {
            groomlab::maya::refreshMayaPaths();
            callGroomlab("follow_scene_files");
            callGroomlab("store_scale_references");  // a groom reopens at the scale it had
            callGroomlab("sync_file_nodes");  // the File Path Editor's nodes as the files are now (file_node.h)
        } catch (...) {
        }
    }));
    gCallbacks.append(MSceneMessage::addCallback(MSceneMessage::kAfterSave, [](void*) {
        try {
            groomlab::maya::refreshMayaPaths();
            callGroomlab("remember_scene_folder");
        } catch (...) {
        }
    }));
    // A referenced groom resolves its relative paths against its own file's folder first.
    for (const MSceneMessage::Message m : {MSceneMessage::kAfterCreateReference, MSceneMessage::kAfterLoadReference,
                                           MSceneMessage::kAfterUnloadReference, MSceneMessage::kAfterRemoveReference,
                                           MSceneMessage::kAfterImportReference}) {
        gCallbacks.append(MSceneMessage::addCallback(m, [](void*) {
            try {
                groomlab::maya::refreshMayaPaths();
            } catch (...) {
            }
        }));
    }
    MGlobal::executeCommand("global int $groomLabPathsJob = -1; if ($groomLabPathsJob < 0) "
                            "$groomLabPathsJob = `scriptJob -event workspaceChanged groomLabPaths`;");

    // The GroomLab menu, when the Python package is on the path (module install).
    if (MGlobal::mayaState() == MGlobal::kInteractive) {
        MGlobal::executePythonCommandOnIdle(
            "try:\n    import groomlab.ui.menu\n    groomlab.ui.menu.install()\nexcept ImportError:\n    pass");
    }
    return MS::kSuccess;
}

PLUGIN_EXPORT MStatus uninitializePlugin(MObject obj) {
    MFnPlugin plugin(obj);
    groomlab::maya::removePenPressure();  // the filter's code lives in this plugin
    groomlab::maya::GroomShape::stopStrokeWarmup();  // its idle callback too
    groomlab::maya::GroomShape::stopCommitLanding();
    if (MGlobal::mayaState() == MGlobal::kInteractive) {
        MGlobal::executePythonCommand(
            "try:\n    import groomlab.ui.menu\n    groomlab.ui.menu.uninstall()\nexcept ImportError:\n    pass");
    }
    CHECK_MSTATUS_AND_RETURN_IT(plugin.deregisterCommand(groomlab::maya::GpuPreviewCmd::kName));
#if GROOMLAB_HAS_ALEMBIC
    CHECK_MSTATUS_AND_RETURN_IT(plugin.deregisterCommand(groomlab::maya::ExportAlembicCmd::kName));
#endif
    CHECK_MSTATUS_AND_RETURN_IT(plugin.deregisterCommand(groomlab::maya::BakeScaleMapsCmd::kName));
    CHECK_MSTATUS_AND_RETURN_IT(plugin.deregisterCommand(groomlab::maya::ScaleMeshCmd::kName));
    CHECK_MSTATUS_AND_RETURN_IT(plugin.deregisterCommand(groomlab::maya::ExportGltfCmd::kName));
    CHECK_MSTATUS_AND_RETURN_IT(plugin.deregisterCommand(groomlab::maya::ImportGroomCmd::kName));
    CHECK_MSTATUS_AND_RETURN_IT(plugin.deregisterCommand(ExportGroomCmd::kName));
    CHECK_MSTATUS_AND_RETURN_IT(plugin.deregisterCommand(ExportUsdCmd::kName));
    CHECK_MSTATUS_AND_RETURN_IT(plugin.deregisterCommand(groomlab::maya::ExportVrsceneCmd::kName));
    CHECK_MSTATUS_AND_RETURN_IT(plugin.deregisterCommand(groomlab::maya::CameraSolveCmd::kName));
    CHECK_MSTATUS_AND_RETURN_IT(plugin.deregisterContextCommand(groomlab::maya::ReferenceFlowContextCmd::kName));
    CHECK_MSTATUS_AND_RETURN_IT(plugin.deregisterContextCommand(GuideSelectContextCmd::kName));
    CHECK_MSTATUS_AND_RETURN_IT(plugin.deregisterContextCommand(BrushContextCmd::kName, BrushStrokeCmd::kName));
    CHECK_MSTATUS_AND_RETURN_IT(plugin.deregisterCommand(PartsCmd::kName));
    CHECK_MSTATUS_AND_RETURN_IT(plugin.deregisterCommand(groomlab::maya::ScalpMapsCmd::kName));
    CHECK_MSTATUS_AND_RETURN_IT(plugin.deregisterCommand(groomlab::maya::GraphStatsCmd::kName));
    CHECK_MSTATUS_AND_RETURN_IT(plugin.deregisterCommand(groomlab::maya::SimulateCmd::kName));
    CHECK_MSTATUS_AND_RETURN_IT(plugin.deregisterCommand(groomlab::maya::BakeCardsCmd::kName));
    CHECK_MSTATUS_AND_RETURN_IT(plugin.deregisterCommand(groomlab::maya::ConvertCmd::kName));
    CHECK_MSTATUS_AND_RETURN_IT(plugin.deregisterCommand(groomlab::maya::CopyColorSetsCmd::kName));
    CHECK_MSTATUS_AND_RETURN_IT(plugin.deregisterCommand(TransferCmd::kName));
    CHECK_MSTATUS_AND_RETURN_IT(plugin.deregisterCommand(GuidesCmd::kName));
    CHECK_MSTATUS_AND_RETURN_IT(plugin.deregisterCommand(OperatorsCmd::kName));
    CHECK_MSTATUS_AND_RETURN_IT(plugin.deregisterCommand(PathsCmd::kName));
    CHECK_MSTATUS_AND_RETURN_IT(plugin.deregisterCommand(groomlab::maya::ExpressionCmd::kName));
    CHECK_MSTATUS_AND_RETURN_IT(plugin.deregisterCommand(groomlab::maya::DisplayThemeCmd::kName));
    MMessage::removeCallbacks(gCallbacks);
    groomlab::maya::removeCrashLog();
    gCallbacks.clear();
    MGlobal::executeCommand("global int $groomLabPathsJob; if ($groomLabPathsJob >= 0) "
                            "{ scriptJob -kill $groomLabPathsJob -force; $groomLabPathsJob = -1; }");
    CHECK_MSTATUS_AND_RETURN_IT(plugin.deregisterCommand(DefaultGraphCmd::kName));
    CHECK_MSTATUS_AND_RETURN_IT(plugin.deregisterCommand(VersionCmd::kName));
    CHECK_MSTATUS_AND_RETURN_IT(MHWRender::MDrawRegistry::deregisterSubSceneOverrideCreator(
        GroomShape::kDrawDbClassification, GroomSubSceneOverride::kRegistrantId));
    CHECK_MSTATUS_AND_RETURN_IT(MHWRender::MDrawRegistry::deregisterDrawOverrideCreator(
        groomlab::maya::MarkerSet::kDrawDbClassification, groomlab::maya::MarkerSet::kRegistrantId));
    CHECK_MSTATUS_AND_RETURN_IT(plugin.deregisterNode(groomlab::maya::MarkerSet::kTypeId));
    MGlobal::executeCommand("if (stringArrayContains(\"groomLabFile.fileName\", `filePathEditor -query -listRegisteredTypes`)) "
                            "filePathEditor -deregisterType \"groomLabFile.fileName\" -temporary;");
    CHECK_MSTATUS_AND_RETURN_IT(plugin.deregisterNode(groomlab::maya::GroomFileNode::kTypeId));
    CHECK_MSTATUS_AND_RETURN_IT(plugin.deregisterNode(GroomShape::kTypeId));
    CHECK_MSTATUS_AND_RETURN_IT(plugin.deregisterData(groomlab::maya::GroomFrameData::kTypeId));
    return MS::kSuccess;
}
