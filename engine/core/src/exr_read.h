// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <string>
#include <vector>

namespace groomlab::detail {

// An OpenEXR file (half or float) as RGBA floats, rows top-down as stb_image reads other images;
// false with *error when it cannot be read.
bool readExr(const std::string& path, std::vector<float>* rgba, int* width, int* height, std::string* error);

}  // namespace groomlab::detail
