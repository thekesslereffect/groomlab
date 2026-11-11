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
#include <cctype>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <vector>

#include <maya/MArgDatabase.h>
#include <maya/MArgList.h>
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

// A multi-use string flag's values, one per use, in order (-guide 3 -guide 7).
inline std::vector<MString> multiStrings(const MArgDatabase& db, const char* flag) {
    std::vector<MString> values;
    const unsigned uses = db.numberOfFlagUses(flag);
    for (unsigned i = 0; i < uses; ++i) {
        MArgList args;
        db.getFlagArgumentList(flag, i, args);
        values.push_back(args.asString(0));
    }
    return values;
}

// A guide id as a command takes it: digits only (0 is no guide); anything else throws
// std::invalid_argument (a typo never reads as 0: -reparent would make the guides primary).
inline std::uint64_t guideIdOf(const MString& text) {
    const std::string s = text.asChar();
    bool digits = !s.empty();
    for (const char c : s) digits = digits && std::isdigit(static_cast<unsigned char>(c)) != 0;
    errno = 0;
    const unsigned long long id = digits ? std::strtoull(s.c_str(), nullptr, 10) : 0;
    if (!digits || errno == ERANGE) throw std::invalid_argument("not a guide id: '" + s + "'");
    return static_cast<std::uint64_t>(id);
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
