// SPDX-License-Identifier: Apache-2.0
#pragma once

// When a param shows, or measures in its unit: its spec's ParamCondition (graph.h: showWhen,
// unitWhen) held against the node's value of another param. Every host's rule, so the Node panel,
// the Graph Editors and the core's own readers agree on what a value reads as.

#include <string>

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

}  // namespace groomlab
