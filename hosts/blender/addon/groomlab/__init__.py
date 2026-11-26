# SPDX-License-Identifier: GPL-3.0-or-later
#
# GroomLab for Blender: groom with Blender's own hair curves and Curves Sculpt brushes while
# GroomLab's node graph grows, shapes and colors the hair from them.
#
# This add-on is free software: you can redistribute it and/or modify it under the terms of the
# GNU General Public License as published by the Free Software Foundation, either version 3 of
# the License, or (at your option) any later version (see LICENSE).
#
# It drives groomlab-helper (bin/), a separate program (Apache-2.0) it starts and talks to over
# pipes and shared memory (docs/BLENDER.md). Nothing of the helper is loaded into Blender.

from . import (actions, agent_link, alphas, blender_theme, cards_view, definitions, display, find_command, graph_arrange,
               graph_authoring, graph_canvas,
               graph_editor,
               graph_tree, graph_ui,
               handlers, icons, menu, ops, palette, photos, pipeline_ops, process, props, themes, tool_ops, tool_props, tools,
               ui,
               updates, window)


def register():
    props.register()
    tool_props.register()
    photos.register()
    icons.register()
    alphas.register()
    ops.register()
    ui.register()
    tool_ops.register()
    palette.register()
    menu.register()
    find_command.register()
    tools.register()
    display.register()
    graph_tree.register()
    graph_ui.register()
    graph_editor.register()
    graph_authoring.register()
    graph_arrange.register()
    graph_canvas.register()
    pipeline_ops.register()
    cards_view.register()
    handlers.register()
    blender_theme.register()
    window.register()
    agent_link.register()
    themes.register()
    updates.register()


def unregister():
    from . import groom
    updates.unregister()
    themes.unregister()
    agent_link.unregister()
    window.unregister()
    blender_theme.unregister()
    handlers.unregister()
    cards_view.unregister()
    pipeline_ops.unregister()
    display.unregister()
    tools.unregister()
    find_command.unregister()
    menu.unregister()
    palette.unregister()
    tool_ops.unregister()
    graph_canvas.unregister()
    graph_arrange.unregister()
    graph_authoring.unregister()
    graph_editor.unregister()
    graph_ui.unregister()
    graph_tree.unregister()
    ui.unregister()
    ops.unregister()
    alphas.unregister()
    icons.unregister()
    photos.unregister()
    tool_props.unregister()
    props.unregister()
    groom._runtimes.clear()
    process.shutdown()
