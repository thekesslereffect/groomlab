// SPDX-License-Identifier: Apache-2.0
#pragma once

// When a param shows, or measures in its unit: its spec's ParamCondition (graph.h: showWhen,
// unitWhen) held against the node's value of another param. Every host's rule, so the Node panel,
// the Graph Editors and the core's own readers agree on what a value reads as.

#include <functional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace groomlab {

// A param's value as a condition compares it: a bool as "true" or "false", a text as it is, a whole
// number as an integer ("3", also when a graph stores it as 3.0: an int param a Python host wrote
// as a float), any other number as JSON writes it.
std::string conditionText(const nlohmann::json& value);

// Whether `condition` ({"param", "in"}) holds for `value`, the node's value of its param: its text
// (conditionText) among `in`. No condition (not an object, or no param): it holds.
bool conditionHolds(const nlohmann::json& condition, const nlohmann::json& value);

// Whether a node at `nodeVersion` has the param of `spec` (a param's spec as JSON): not when a later
// version of its operator added it ("addedIn", ParamSpec::added). Such a node does nothing with the
// param until it is upgraded, so every panel leaves the param out there.
bool paramInVersion(const nlohmann::json& spec, int nodeVersion);

// The Node panel's sections (every host's): for each of `params` (a node spec's "params", in its
// order) its section: kAdvancedSection for an advanced param (or one of that group), its group
// when the params name two groups or more (one group reads as the main params) or a param says
// "groupShown" (a compound's params, grouped by the node inside they come from), else "" (the
// main params, no header). The panels show "", the groups in the spec's order, then Advanced.
constexpr const char* kAdvancedSection = "Advanced";
std::vector<std::string> paramSections(const nlohmann::json& params);

// Whether param `name` of `params` shows: its showWhen holds for the node's value of the param it
// names (valueOf), and that param shows too, and so on up (Clump's count hides with clumpBy). No
// condition, or no such param: it shows.
bool paramShown(const nlohmann::json& params, const std::string& name, const std::function<nlohmann::json(const std::string&)>& valueOf);

}  // namespace groomlab
