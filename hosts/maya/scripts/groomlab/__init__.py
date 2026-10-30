# SPDX-License-Identifier: Apache-2.0
"""Maya-side helpers for GroomLab: the artist API, `groomlab.<name>`.

The functions live in modules by what they work on, each imported into the package here, so
`groomlab.<name>` is the whole API and this list its table of contents:

    base       the plugin, one-step undo (undo_chunk, undoable), shape lookups
    params     the graph (get_graph, set_graph, set_param), controls, keyed params
    paths      project, scene and asset folders, portable paths, shared libraries
    collision  colliders
    extra_scalps  several scalps on one groom: other meshes the hair grows on
    guides     guides from curves, groups, selection, hierarchy tools, sculpt display
    brushes    sculpting brushes, the brush tool and its alphas
    painting   maps: painted, used by params, their files; the freeze mask
    flow       parting lines, flow arrows and sinks, the Flow and Part nodes
    motion     Wind and Jiggle
    shading    the hair shader and the look it reads from the graph
    groom      the groom node: creating one, scalp and rest mesh, the active groom
    transfer   moving a groom onto another scalp mesh
    cards      game cards: presets, bakes, material, scalp cap
    exports    Alembic and USD curves, the scales mesh and its maps
    braids     braids and instancing
    feathers   feathers, feather cards, the rachis mesh
    game       Unreal, Unity and glTF exports
    files      .groom files: export, render from, import, reload, save
    links      the docs and Support GroomLab (donations) in the browser

A module imports from the ones above it in this list at its top, and from one below it only inside
the function that needs it, so they never import in a cycle.

Beside them: graph (the graph model), presets, nodes (compound and script nodes: the .glnode library), sim, shots, sculpts, bake, expressions, units, review, studio,
reference, renderman (RenderMan for Maya's procedural node, experimental), vray_render (V-Ray Standalone renders
from any Maya; vray_scene its scene text and vray.exe job), prman_render (RenderMan renders with prman from any
Maya; prman_scene its RIB and prman job), hardware2_render (Maya Hardware 2.0 renders in the session), render_gather and render_common (what those two share), vray_hook (V-Ray for Maya 2024+, experimental), utils, importers (other hair
systems' grooms: XGen; importers/foreign.py the layer they share) and the UI (ui/). What the hosts share is computed in the core's Python module,
groomlab_core (engine/python, beside the plugin in the module's python folder), never copied here
(card presets, map names, card bake files, relative paths, graph edits).
"""

from groomlab.base import (  # noqa: F401
    GROOM_TYPE, PLUGIN_NAME, ensure_plugin, undo_chunk, undoable, _original_shape, _shape_of)
from groomlab.params import (  # noqa: F401
    CONTROL_PREFIX, VECTOR_SUFFIXES, add_control, control_base, copy_graph_text, delete_param_key, get_graph,
    list_controls, param_control, param_key_state, param_plugs, param_value, paste_graph_text, remove_param_animation,
    select_param_attribute, set_graph, set_param, set_param_key, set_param_value, sync_controls, _control_plugs_of,
    _controls_in, _graph_nodes)
from groomlab.nodes import (import_node, library_nodes, node_dirs, save_node, update_node, add_script_node, set_script,  # noqa: F401
                            save_script_node, script_errors)
from groomlab.paths import (  # noqa: F401
    ASSET_FOLDER, ASSET_KINDS, absolute_paths, asset_folder, asset_name, follow_scene_files, library_dirs,
    library_user_dir, portable_path, project_root, remember_scene_folder, resolve_path, scene_folder, _relative_to,
    _same_file, _unique_path)
from groomlab.collision import add_collider, colliders, remove_collider, set_collider_friction  # noqa: F401
from groomlab.extra_scalps import add_scalp, remove_scalp, scalps, valid_name as valid_scalp_name  # noqa: F401
from groomlab.guides import (  # noqa: F401
    SELECT_CONTEXT, add_guide_group, add_guides, clear_guide_selection, clear_guides, color_guides_by_map,
    demote_group, end_guide_preview, generate_children, get_hierarchy, group_users,
    guide_group_colors, guide_ids, guide_segments, guides_from_curves, hidden_guide_groups, import_guides_from_alembic,
    isolate_group, list_guide_groups, lock_guides, mirror_group, mirror_guide_selection, preview_resample_guides,
    promote_group, remove_guide_group, rename_guide_group, reparent_guides, resample_guides, reset_group_segments,
    select_guides, select_tool,
    selected_guides, set_group_segments, set_guide_attribute, set_guide_group_color, set_sculpt_display, set_soft_selection,
    show_guide_group, symmetry_sync, unlock_guides, set_tube, get_tube, TUBE_SHAPES, _group_list,
    scale_tube, smooth_tube, fill_tubes, set_fill_tubes, tube_shapes, add_tube_shape, set_tube_shape_points,
    rename_tube_shape, remove_tube_shape, preview_tube_shape, tube_targets)
from groomlab.brushes import (BRUSH_CONTEXT, BRUSH_MODES, TUBE_CHANNELS, add_brush_alpha, brush_alpha_folder,  # noqa: F401
                              brush_alphas, brush_stroke, brush_tool, length_paths_edit, length_paths_state,
                              placement_tube)
from groomlab.painting import (  # noqa: F401
    MAP_CHANNELS, MAP_DISPLAY_CHANNELS, MAP_KINDS, MISSING_KINDS, add_map, attribute_map, clump_paint, color_set_map,
    convert_map, export_map, expression_map, feed_map_node, fill_map, find_missing_files, freeze_guides, freeze_tool,
    guide_map, import_map, list_maps, map_channels, map_kind, map_nodes, map_texture, map_users, maps, missing_files,
    missing_map_files, paint_map, paint_param, paint_scalp_map, param_map, reload_map, remove_map, rename_map,
    set_map_channels, set_map_texture, setting_targets, show_map_on_scalp, texture_map, unfreeze_all, use_map, view_map,
    _map_texture_default)
from groomlab.flow import (  # noqa: F401
    FLOW_SINKS, add_flow_node, add_part, add_part_node, clear_flow, clear_parts, draw_part_tool, flow_arrows, flow_counts,
    flow_sink, flow_sinks, flow_stroke, flow_tool, list_parts, remove_part)
from groomlab.motion import add_jiggle, add_wind, motion_anchor  # noqa: F401
from groomlab.shading import (  # noqa: F401
    assign_shading_group, color_nodes, create_hair_shader, hair_color_node, hair_color_params, hair_look,
    iridescent_tint, render_data_node, render_data_params, sync_hair_shader)
from groomlab.groom import (  # noqa: F401
    ACTIVE_VAR, GPU_PREVIEW_VAR, PIN_VAR, active_groom, add_sample, adopt_scene_groom, adopted_groom,
    clear_rest_mesh, create_groom, errors, gpu_preview,
    guides_text,
    groom_scale, inputs_summary, list_grooms, lock_groom, pin_groom, pin_new_groom, pinned_groom, rest_mesh, scalp_mesh,
    SEEN_VAR, current_groom, on_current_groom, remove_current_groom_listener, set_current_groom,
    hair_visible, guides_visible, set_all_hair_visible, set_all_guides_visible, set_hair_visible,
    set_guides_visible, toggle_all_guides,
    set_follow_scalp, set_scale_reference, set_scale_source, scale_source, set_scale_with_scalp, store_scale_references,
    set_rest_mesh, set_scalp)
from groomlab.transfer import (transfer_groom, get_transfer_markers, set_transfer_markers,  # noqa: F401
                               snap_markers)
from groomlab.cards import (  # noqa: F401
    CARD_MAPS, CARD_MAP_PRESET, CARD_NORMALS_NOTE, CARD_PRESETS, CARD_TINT_NOTE, FEATHER_CARD_NOTE, FEATHER_CARD_PRESETS,
    SCALP_MAPS, bake_card_maps, bake_cards, bake_scalp, card_display, card_map_presets, card_map_sources,
    delete_card_map_preset, save_card_map_preset, card_material, card_mesh, card_node_kind, card_stats, cards_preset,
    compare_cards, create_card_mesh, create_cards, feather_card_triangles, scalp_cap, _card_maps, _preset_params)
from groomlab.exports import (  # noqa: F401
    SCALE_MAPS, bake_scale_maps, export_alembic, export_scales, export_usd, export_vrscene, has_scales, scale_mesh,
    viewport_points)
from groomlab.braids import add_braid, add_instances, braid_paths_from_curves, set_instance_prototype  # noqa: F401
from groomlab.feathers import (  # noqa: F401
    add_feathers, create_feather_cards, create_rachis_mesh, export_rachis, feather_designs, feather_groups,
    feather_guide_groups, rachis_mesh, rachis_width, set_rachis_width)
from groomlab.game import (  # noqa: F401
    UNITY_CARD_MAPS, UNITY_HAIR_PACKAGES, UNITY_MAX_STRANDS, UNITY_STRAND_POINTS, UNREAL_LODS, export_cards_gltf,
    export_cards_unity, export_cards_unreal, export_unity_feathers, export_unity_strands, export_unreal_groom)
from groomlab.files import (  # noqa: F401
    export_groom, groom_file, groom_file_info, import_groom, reload_groom, render_curves, render_from_groom_file,
    save_groom)

# Review tools (density heatmap, HUD, turntable) live in their own module.
from groomlab.review import density_heatmap, groom_stats, render_turntable, show_hud, turntable  # noqa: F401
# Studio lighting (a backdrop, Arnold area lights, a camera and a mannequin material): groomlab.studio.
from groomlab import studio  # noqa: F401
from groomlab.studio import remove_studio, studio as add_studio  # noqa: F401
# V-Ray Standalone renders (the grooms, meshes, camera and lights as a .vrscene; vray.exe in the background).
from groomlab.vray_render import render_vray, set_vray_path, vray_path  # noqa: F401
# RenderMan renders (prman with GroomLab's procedural; the meshes, camera and lights as RIB).
from groomlab.prman_render import prman_path, render_prman, set_prman_path  # noqa: F401
# Maya Hardware 2.0 renders (no renderer needed: the viewport's drawing into images, in this session).
from groomlab.hardware2_render import render_hardware2  # noqa: F401
# Reference photos as cameras (Groom Tools > Photos): groomlab.reference.
from groomlab import reference  # noqa: F401
from groomlab.from_reference import guides_from_reference  # noqa: F401  (a prototype: guides from the reference photos)
# GroomLab > Utilities (guides and strands to and from curves and meshes, copying a look): groomlab.utils.
from groomlab import utils  # noqa: F401
from groomlab.utils import add_strand_mesh, strand_mesh_shape  # noqa: F401  (a StrandMesh node, its mesh in the scene)
# Sculpt nodes: brush the guides the graph made, the strokes kept in the node: groomlab.sculpts.
from groomlab import sculpts  # noqa: F401
# Importers (GroomLab > Import > From XGen... / From Yeti...): XGen's and Yeti's grooms as GroomLab
# grooms (Yeti's read from the scene data, without Yeti), groomlab.importers.
from groomlab.importers.xgen import import_xgen, xgen_descriptions  # noqa: F401
from groomlab.importers.yeti import import_yeti, yeti_nodes  # noqa: F401
# Every file a groom uses (maps, file params, caches, bakes, the groom file), for pipelines and
# Maya's File Path Editor (a groomLabFile node per file): groomlab.dependencies.
from groomlab.dependencies import collect_files, file_dependencies, repath_file, sync_all as sync_file_nodes  # noqa: F401
