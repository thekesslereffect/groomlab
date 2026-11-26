// SPDX-License-Identifier: Apache-2.0
#pragma once

// The agent link in Maya (engine/editor agent_link.h, agent_tools.h): groomLabAgent answers an
// AI agent's tool calls on the grooms of the scene. The listener is the Groom Tools' Python
// (groomlab.agent_link: a QTcpServer on Maya's main thread), which hands each request line to this
// command inside an undo chunk named "Agent: <tool>"; the tools run against the scene's grooms
// (MayaAgentTarget: their sessions read, their edits set on the groom's plugs through this
// command's MDGModifier, so undo and redo are Maya's).
//
//   groomLabAgent -start -port 52011 [-folders '{"root", "presets", "meshes", "docs"}']
//       the link on: a token made and the discovery file written; returns {"token", "file"} (JSON)
//   groomLabAgent -stop                    the link off: its file removed
//   groomLabAgent -status                  {"running", "port", "file", "groom"} (no token)
//   groomLabAgent -request '<line>'        one request answered (the answer line); undoable
//   groomLabAgent -groomName               the groom the tools work on by default ("" none)

#include <maya/MDGModifier.h>
#include <maya/MPxCommand.h>
#include <maya/MSyntax.h>

namespace groomlab::maya {

class AgentCmd : public MPxCommand {
public:
    static const char* kName;
    static void* creator();
    static MSyntax newSyntax();

    MStatus doIt(const MArgList& args) override;
    MStatus redoIt() override;
    MStatus undoIt() override;
    bool isUndoable() const override { return changed_; }

    // The command running a request now (its edits go into its modifier), or null.
    static AgentCmd* current();
    MDGModifier& modifier() { return modifier_; }
    void touched() { changed_ = true; }

    // The link turned off and the tools let go (the plugin unloading).
    static void shutdown();

private:
    MDGModifier modifier_;
    bool changed_ = false;
};

}  // namespace groomlab::maya
