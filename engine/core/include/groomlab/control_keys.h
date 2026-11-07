// SPDX-License-Identifier: Apache-2.0
#pragma once

// A control's keys (a Control node's value over time, the app's param keys): what Maya keys on a
// groom's ctl_ attribute and Blender on its ctl_ property, kept in the groom file for the hosts
// that have no keyframes of their own ("controlKeys").

#include <utility>
#include <vector>

namespace groomlab {

// (frame, value), frames ascending, one key a frame.
using ControlKeys = std::vector<std::pair<double, double>>;

// The value at `frame`: a smooth curve through the keys that never overshoots between two of them
// (monotone cubic, Fritsch-Carlson: as Maya's auto tangents and Blender's auto-clamped handles
// behave), held at the first and last key's value outside them. 0 without keys.
double controlKeyValue(const ControlKeys& keys, double frame);
// A key at `frame` (the one there replaced).
void setControlKey(ControlKeys& keys, double frame, double value);
// The key at `frame` removed; whether there was one.
bool removeControlKey(ControlKeys& keys, double frame);
// Whether a key sits at `frame`.
bool hasControlKey(const ControlKeys& keys, double frame);

}  // namespace groomlab
