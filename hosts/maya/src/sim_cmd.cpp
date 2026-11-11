// SPDX-License-Identifier: Apache-2.0
#include "sim_cmd.h"

#include <maya/MArgDatabase.h>
#include <maya/MDagPath.h>
#include <maya/MFnDagNode.h>
#include <maya/MGlobal.h>
#include <maya/MSelectionList.h>
#include <maya/MStringArray.h>

#include <nlohmann/json.hpp>

#include "command_args.h"
#include "groom_shape.h"
#include "groomlab/sim_cache.h"
#include "groomlab/simulation.h"
#include "guarded.h"

namespace groomlab::maya {

const char* SimulateCmd::kName = "groomLabSimulate";

void* SimulateCmd::creator() { return new SimulateCmd(); }

MSyntax SimulateCmd::newSyntax() {
    MSyntax s;
    s.addFlag("-tf", "-toFrame", MSyntax::kLong);
    s.addFlag("-inv", "-invalidate");
    s.addFlag("-st", "-status");
    s.addFlag("-nds", "-nodes");
    s.addFlag("-bk", "-bake", MSyntax::kString);
    s.addFlag("-nd", "-node", MSyntax::kString);
    s.addFlag("-sf", "-startFrame", MSyntax::kLong);
    s.addFlag("-ef", "-endFrame", MSyntax::kLong);
    s.addFlag("-stp", "-stamp");
    s.setObjectType(MSyntax::kSelectionList, 1, 1);
    s.useSelectionAsDefault(true);
    return s;
}

MStatus SimulateCmd::doIt(const MArgList& args) {
    return guarded("SimulateCmd::doIt", [&]() -> MStatus {
        MStatus status;
        MArgDatabase db(syntax(), args, &status);
        if (!status) return status;
        MDagPath path;
        if (!groomArgument("groomLabSimulate", db, &path)) return MS::kFailure;
        GroomShape* shape = groomShapeOf(path);
        if (!shape) return MS::kFailure;
        MString node;
        if (db.isFlagSet("-node")) db.getFlagArgument("-node", 0, node);
        if (db.isFlagSet("-toFrame")) {
            int frame = 0;
            db.getFlagArgument("-toFrame", 0, frame);
            const SimRun run = shape->simulateTo(frame);
            for (const std::string& e : run.errors) MGlobal::displayWarning(MString("GroomLab: ") + e.c_str());
            setResult(run.frames);
            return MS::kSuccess;
        }
        if (db.isFlagSet("-invalidate")) {
            shape->invalidateSimulation(node.asChar());
            return MS::kSuccess;
        }
        if (db.isFlagSet("-nodes")) {
            shape->ensureEvaluated();
            MStringArray names;
            for (const std::string& n : shape->session().simulateNodes()) names.append(n.c_str());
            setResult(names);
            return MS::kSuccess;
        }
        if (db.isFlagSet("-stamp")) {
            shape->ensureEvaluated();
            setResult(MString(formatSimulationStamp(shape->session().simulationStamp(node.asChar())).c_str()));
            return MS::kSuccess;
        }
        if (db.isFlagSet("-bake")) {
            MString file;
            db.getFlagArgument("-bake", 0, file);
            const std::string name = node.asChar();
            int first = 0, last = 0;
            bool found = false;
            for (const SimStatus& s : shape->simulationStatus()) {
                if (s.node != name) continue;
                found = true;
                first = s.startFrame;
                last = s.ranges.empty() ? s.startFrame - 1 : s.ranges.back().second;
            }
            if (!found) {
                MGlobal::displayError(MString("groomLabSimulate: no Simulate node '") + node + "' makes the groom");
                return MS::kFailure;
            }
            if (db.isFlagSet("-startFrame")) db.getFlagArgument("-startFrame", 0, first);
            if (db.isFlagSet("-endFrame")) db.getFlagArgument("-endFrame", 0, last);
            try {
                writeSimulationCache(shape->session(), name, file.asChar(), first, last);
            } catch (const std::exception& e) {
                MGlobal::displayError(MString("groomLabSimulate: ") + e.what());
                return MS::kFailure;
            }
            setResult(MString(formatSimulationStamp(shape->session().simulationStamp(name)).c_str()));
            return MS::kSuccess;
        }
        // -status (the default): every Simulate node's cache.
        nlohmann::json out = nlohmann::json::array();
        for (const SimStatus& s : shape->simulationStatus()) {
            nlohmann::json ranges = nlohmann::json::array();
            for (const auto& [a, b] : s.ranges) ranges.push_back({a, b});
            out.push_back({{"node", s.node}, {"startFrame", s.startFrame}, {"ranges", ranges}, {"frames", s.frames},
                           {"live", s.live}, {"baked", s.baked}, {"bakeStale", s.bakeStale}, {"bakedFirst", s.bakedFirst},
                           {"bakedLast", s.bakedLast}, {"bakedFile", s.bakedFile}, {"stepMs", s.stepMs}, {"bytes", s.bytes},
                           {"stamp", formatSimulationStamp(s.stamp)}});
        }
        setResult(MString(out.dump().c_str()));
        return MS::kSuccess;
    });
}

}  // namespace groomlab::maya
