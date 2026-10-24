// SPDX-License-Identifier: Apache-2.0
// The browser's binding of WebGroom (web_groom.h), with embind. The buffers come back as views
// into the module's memory (typed_memory_view): valid until the next call into the module, so
// JavaScript copies them (web/lib/groomlab-wasm does, in its worker).

#include <emscripten/bind.h>
#include <emscripten/val.h>

#include "groomlab/version.h"
#include "web_groom.h"

namespace {

using groomlab::web::WebGroom;
using emscripten::typed_memory_view;
using emscripten::val;

template <typename T>
val view(const std::vector<T>& v) {
    return val(typed_memory_view(v.size(), v.data()));
}

std::string version() { return groomlab::kVersionString; }

}  // namespace

EMSCRIPTEN_BINDINGS(groomlab_web) {
    emscripten::function("version", &version);
    emscripten::class_<WebGroom>("WebGroom")
        .constructor<>()
        .function("load", &WebGroom::load)
        .function("error", &WebGroom::error)
        .function("setDirectory", &WebGroom::setDirectory)
        .function("directory", &WebGroom::directory)
        .function("filesJson", &WebGroom::filesJson)
        .function("missingFilesJson", &WebGroom::missingFilesJson)
        // bytes: a Uint8Array (or ArrayBuffer), copied as they are (embind's std::string).
        .function("addFile", &WebGroom::addFile)
        .function("clearFiles", &WebGroom::clearFiles)
        .function("controlsJson", &WebGroom::controlsJson)
        .function("setParam", &WebGroom::setParam)
        .function("evaluate", &WebGroom::evaluate)
        .function("evaluateMs", &WebGroom::evaluateMs)
        .function("strandCount", &WebGroom::strandCount)
        .function("offsets", +[](const WebGroom& g) { return view(g.offsets()); })
        .function("positions", +[](const WebGroom& g) { return view(g.positions()); })
        .function("colors", +[](const WebGroom& g) { return view(g.colors()); })
        .function("widths", +[](const WebGroom& g) { return view(g.widths()); })
        .function("guideOffsets", +[](const WebGroom& g) { return view(g.guideOffsets()); })
        .function("guidePositions", +[](const WebGroom& g) { return view(g.guidePositions()); })
        .function("scalpPositions", +[](const WebGroom& g) { return view(g.scalpPositions()); })
        .function("scalpNormals", +[](const WebGroom& g) { return view(g.scalpNormals()); })
        .function("scalpIndices", +[](const WebGroom& g) { return view(g.scalpIndices()); })
        .function("statsJson", &WebGroom::statsJson)
        .function("saveGroom", &WebGroom::saveGroom)
        .function("graphJson", &WebGroom::graphJson)
        .function("setGraphJson", &WebGroom::setGraphJson)
        .function("bakeGuides", &WebGroom::bakeGuides);
}
