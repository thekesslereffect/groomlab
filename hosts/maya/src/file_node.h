// SPDX-License-Identifier: Apache-2.0
#pragma once

// groomLabFile: one file a groom uses (a texture map, a file param of its graph, a guide cache, a
// Simulate node's bake, its card bake, the groom file Arnold renders), mirrored on a node of its
// own so Maya's File Path Editor lists it under GroomLab (the editor takes one file attribute per
// node: a multi attribute crashes it) and pipeline tools can find it (ls -type groomLabFile).
//
// groomlab.dependencies keeps them in step with their grooms (made, updated and removed on idle
// after a groom's files change, and before every save): `fileName` holds the file as it resolves
// (an absolute path, what the editor checks), `groom` the groom it belongs to (a message
// connection), `fileKind` and `fileKey` which of its files it is. A path set on `fileName` from
// outside (the editor's Repath or Replace String, a script) goes back to the groom on idle, kept
// portable as GroomLab keeps its paths (groomlab.dependencies.file_node_changed).

#include <maya/MPxNode.h>
#include <maya/MString.h>
#include <maya/MTypeId.h>

namespace groomlab::maya {

class GroomFileNode : public MPxNode {
public:
    static void* creator() { return new GroomFileNode(); }
    static MStatus initialize();

    bool setInternalValue(const MPlug& plug, const MDataHandle& handle) override;

    static const MTypeId kTypeId;
    static const MString kTypeName;

    static MObject aFileName;  // string, usedAsFilename: the file as it resolves
    static MObject aGroom;     // message: the groom it belongs to
    static MObject aFileKind;  // string: map, param, attribute, simCache, cardBake
    static MObject aFileKey;   // string: which one (the map's name, node.param, the attribute...)
};

}  // namespace groomlab::maya
