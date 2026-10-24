// SPDX-License-Identifier: Apache-2.0
#include "file_node.h"

#include <maya/MFnDependencyNode.h>
#include <maya/MFnMessageAttribute.h>
#include <maya/MFnStringData.h>
#include <maya/MFnTypedAttribute.h>
#include <maya/MGlobal.h>
#include <maya/MPlug.h>

#include "node_ids.h"

namespace groomlab::maya {

const MTypeId GroomFileNode::kTypeId(kGroomLabFileTypeId);
const MString GroomFileNode::kTypeName("groomLabFile");

MObject GroomFileNode::aFileName;
MObject GroomFileNode::aGroom;
MObject GroomFileNode::aFileKind;
MObject GroomFileNode::aFileKey;

MStatus GroomFileNode::initialize() {
    MFnTypedAttribute tAttr;
    MFnMessageAttribute mAttr;
    MFnStringData stringData;
    MStatus status;
    aFileName = tAttr.create("fileName", "fn", MFnData::kString, stringData.create(""), &status);
    CHECK_MSTATUS_AND_RETURN_IT(status);
    tAttr.setStorable(true);
    tAttr.setUsedAsFilename(true);
    tAttr.setInternal(true);  // a path set from outside goes back to the groom (setInternalValue)
    aGroom = mAttr.create("groom", "gr", &status);
    CHECK_MSTATUS_AND_RETURN_IT(status);
    aFileKind = tAttr.create("fileKind", "fk", MFnData::kString, stringData.create(""), &status);
    CHECK_MSTATUS_AND_RETURN_IT(status);
    tAttr.setStorable(true);
    aFileKey = tAttr.create("fileKey", "fy", MFnData::kString, stringData.create(""), &status);
    CHECK_MSTATUS_AND_RETURN_IT(status);
    tAttr.setStorable(true);
    for (const MObject* attr : {&aFileName, &aGroom, &aFileKind, &aFileKey}) CHECK_MSTATUS_AND_RETURN_IT(addAttribute(*attr));
    return MS::kSuccess;
}

bool GroomFileNode::setInternalValue(const MPlug& plug, const MDataHandle& handle) {
    if (plug == aFileName) {
        // On idle, after the edit that set it (the editor's Repath sets several in a row): the
        // groom takes the path when it differs from what it has (groomlab.dependencies keeps the
        // node as the groom has it too, so its own writes come back as no change).
        MString call("try:\n    from groomlab import dependencies\n    dependencies.file_node_changed('");
        call += MFnDependencyNode(thisMObject()).name();
        call += "')\nexcept ImportError:\n    pass";
        MGlobal::executePythonCommandOnIdle(call);
    }
    return MPxNode::setInternalValue(plug, handle);  // stored as usual
}

}  // namespace groomlab::maya
