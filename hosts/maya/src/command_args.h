// SPDX-License-Identifier: Apache-2.0
#pragma once

// What the GroomLab commands share: errors in the Script Editor, flag values
// with a fallback, and the groom a command acts on.

#include <maya/MArgDatabase.h>
#include <maya/MDagPath.h>
#include <maya/MFnDagNode.h>
#include <maya/MGlobal.h>
#include <maya/MSelectionList.h>
#include <maya/MPlug.h>
#include <maya/MStatus.h>
#include <maya/MString.h>
#include <maya/MStringArray.h>

#include "groom_shape.h"

namespace groomlab::maya {

// Shows "command: message" as an error and fails.
inline MStatus fail(const char* command, const MString& message) {
    MGlobal::displayError(MString(command) + ": " + message);
    return MS::kFailure;
}

// A flag's first argument, or `fallback` when the flag is not set. Takes an
// MArgDatabase or an MArgParser (a context's edit flags).
template <class Args>
double flagDouble(const Args& db, const char* flag, double fallback) {
    double v = fallback;
    if (db.isFlagSet(flag)) db.getFlagArgument(flag, 0, v);
    return v;
}

template <class Args>
MString flagString(const Args& db, const char* flag, const MString& fallback = "") {
    MString v = fallback;
    if (db.isFlagSet(flag)) db.getFlagArgument(flag, 0, v);
    return v;
}

// `path` (a transform: its shape) as a groom shape; reports and fails when it is none.
inline MStatus groomAt(const char* command, MDagPath* path) {
    path->extendToShape();
    if (MFnDagNode(*path).typeName() != GroomShape::kTypeName) {
        return fail(command, path->partialPathName() + " is not a groomLabShape");
    }
    return MS::kSuccess;
}

// The groom shape named by the command's object (or the selection: kSelectionList
// syntax). Reports and fails when there is none or it is not a groomLabShape.
inline MStatus groomArgument(const char* command, const MArgDatabase& db, MDagPath* path) {
    MSelectionList selection;
    db.getObjects(selection);
    if (selection.getDagPath(0, *path) != MS::kSuccess) return fail(command, "select or name a groomLabShape");
    return groomAt(command, path);
}

// The same for a command whose object is a name (kStringObjects syntax).
inline MStatus groomArgument(const char* command, const MStringArray& names, MDagPath* path) {
    MSelectionList selection;
    if (names.length() < 1) return fail(command, "name a groomLabShape");
    if (selection.add(names[0]) != MS::kSuccess || selection.getDagPath(0, *path) != MS::kSuccess) {
        return fail(command, "no object " + names[0]);
    }
    return groomAt(command, path);
}

// The groom shape's node object (null when `path` is no groomLabShape).
inline GroomShape* groomShapeOf(const MDagPath& path) { return dynamic_cast<GroomShape*>(MFnDagNode(path).userNode()); }

// A groom from a referenced file: an edit rewrites its whole data, which the reference records
// as an edit (and reapplies on load). Refused unless the groom carries a true
// `allowReferenceEdits` attribute (add it to opt in).
inline bool referenceEditAllowed(const MDagPath& groom) {
    MFnDagNode fn(groom);
    if (!fn.isFromReferencedFile()) return true;
    MStatus status;
    const MPlug allow = fn.findPlug("allowReferenceEdits", true, &status);
    return status && !allow.isNull() && allow.asBool();
}

constexpr const char* kReferenceRefused =
    ": the groom is from a referenced file; an edit would record its whole data (guides, parts) as a "
    "reference edit. Edit it in its own scene, or add a boolean attribute allowReferenceEdits (on) to allow it";

// Whether brushes and part edits may change the groom: not when it is locked (lock_groom:
// finished work stays finished), nor from a referenced file (referenceEditAllowed). Reports why.
inline MStatus editableGroom(const char* command, const MDagPath& groom) {
    if (MFnDagNode(groom).findPlug("locked", true).asBool()) return fail(command, groom.partialPathName() + " is locked");
    if (!referenceEditAllowed(groom)) return fail(command, groom.partialPathName() + kReferenceRefused);
    return MS::kSuccess;
}

}  // namespace groomlab::maya
