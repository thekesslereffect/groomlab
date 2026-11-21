// SPDX-License-Identifier: Apache-2.0
// OpenEXR images read by tinyexr (third_party/tinyexr, BSD-3-Clause; its zlib through
// third_party/miniz, MIT): texture maps and displacement maps stored as half or float EXR, the
// format film displacement maps come in. Read from memory, so a UTF-8 path opens on every
// platform as the other images do.

#define TINYEXR_IMPLEMENTATION
#define TINYEXR_USE_MINIZ 1
#define TINYEXR_USE_THREAD 0
#define TINYEXR_USE_OPENMP 0
#include "miniz.h"
#include "tinyexr.h"

#include "exr_read.h"

#include <filesystem>
#include <fstream>
#include <iterator>

namespace groomlab::detail {

bool readExr(const std::string& path, std::vector<float>* rgba, int* width, int* height, std::string* error) {
    std::ifstream in(std::filesystem::u8path(path), std::ios::binary);
    if (!in) {
        if (error) *error = "cannot open '" + path + "'";
        return false;
    }
    const std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    float* out = nullptr;
    const char* message = nullptr;
    // RGBA floats, a single-channel image (a displacement map's Y or R) in all four.
    const int status = LoadEXRFromMemory(&out, width, height, bytes.data(), bytes.size(), &message);
    if (status != TINYEXR_SUCCESS || !out) {
        if (error) *error = "cannot read '" + path + "': " + (message ? message : "not an EXR image");
        if (message) FreeEXRErrorMessage(message);
        if (out) free(out);
        return false;
    }
    rgba->assign(out, out + static_cast<std::size_t>(*width) * static_cast<std::size_t>(*height) * 4);
    free(out);
    return true;
}

}  // namespace groomlab::detail
