// SPDX-License-Identifier: Apache-2.0
#pragma once

// Controls: sliders on the groom that drive graph params, every host's (Maya's ctl_ attributes,
// the app's and Blender's sliders). A control is a Control node's slider, an Expression variable
// with a "control", or a keyed param: a param linked to a control of its own (the node's
// "paramControls"; a vector's three, X Y Z, or R G B for a color). The host sets each control's
// value per frame (its keys) and the graph reads it (setControlExternals, groom.h).
//
// Maya's params.py / graph.py, the Blender helper and the app make and read controls through
// these, so a control is named, ranged and placed the same in every host.

#include <map>
#include <optional>
#include <string>
#include <vector>

#include "groomlab/graph_edit.h"

namespace groomlab {

// A control's name as every host makes it from `wanted`: letters, digits and _ (every other
// character _), "p_" before a leading digit or for nothing.
std::string controlName(const std::string& wanted);

// Every control the graph reads (compounds' inner graphs too), as JSON {name: {"kind", "node",
// "default", ...}}: "control" (a Control node's slider: "min", "max"), "variable" (an Expression
// variable with a control: "variable", "min", "max"; the variables as an object or its text), or
// "keyed" (a keyed param: "param", and "component" for a vector's). A keyed param's default is its
// own value, else its spec's default (a bool as 0 or 1).
std::string controlsJson(const std::string& graphJson);

// Why node.param cannot be keyed in place ("" when it can): a Control node's own params, a param
// that is not animatable (it sets how many strands there are, or where), one that is not a number,
// switch or vector, or one reading a map. GraphError for no such node or param.
std::string notKeyable(const GraphEdit& edit, const std::string& node, const std::string& param);

// node.param as an input port of the node (a mappable number or color a Field port, a ramp a Curve
// port, any other a Value port), its ports kept in the spec's order: Maya's expose_param. Nothing
// when it is one already. GraphError for a param that cannot be one, or one named as a port the
// node has.
void exposeParam(GraphEdit& edit, const std::string& node, const std::string& param);

// A Control node driving node.param (exposed first): Maya's add_control, the app's and Blender's
// "Drive with a slider". Its control is controlName(`name`) (GraphError when the graph has a
// control of that name), else the param's name, numbered until free (width, width1...): the
// Control node's "name" param says which. Its slider starts at `defaultValue`, else the param's
// value (a number), else `min`, and spans min to max, widened to take the start in. It sits left
// of and below the node. `groomSize` fits it as addNode does. Returns the Control node's name.
std::string addControl(GraphEdit& edit, const std::string& node, const std::string& param, const std::string& name = {},
                       std::optional<double> defaultValue = std::nullopt, double min = 0.0, double max = 1.0,
                       std::optional<double> groomSize = std::nullopt);

// Keys node.param: links it to a control of its own, named controlName(`wanted`, else
// "<node>_<param>"), numbered (name1, name2...) until free of the graph's controls and of `taken`
// (names the host holds otherwise: Maya's ctl_ attributes); a vector three, name+X Y Z, or R G B for
// a color. Its value is stored (so its kind is known). Returns the control as JSON: a name, or the
// three names. A param keyed already keeps its control (returned). GraphError when it cannot be
// keyed (notKeyable).
std::string linkControl(GraphEdit& edit, const std::string& node, const std::string& param, const std::string& wanted = {},
                        const std::vector<std::string>& taken = {});

// Unkeys node.param (Maya's and Blender's Remove Animation): its link to its control(s) gone, the
// param back to a value of its own: each control's value in `values` (the host's controls now),
// else the value it keeps; an int rounded, a switch on from 0.5. Returns that value as JSON, ""
// when the param was not keyed (nothing changed). GraphError for no such node or param.
std::string unlinkControl(GraphEdit& edit, const std::string& node, const std::string& param,
                          const std::map<std::string, float>& values = {});

}  // namespace groomlab
