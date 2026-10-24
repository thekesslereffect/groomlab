// SPDX-License-Identifier: Apache-2.0
#pragma once

// A groom for the browser (engine/web: the core built to WebAssembly, docs/WEB.md): a .groom's
// text loaded into a GroomSession, its sliders (the Control nodes, then the exposed params), live
// edits re-evaluated incrementally (GroomSession::setGraphJson keeps every node whose inputs did
// not change), and flat buffers of what to draw. Plain C++ with no Emscripten in it, so the same
// code builds natively (web_probe, the accuracy check against the desktop engine) and in the
// browser (bind.cpp). Errors never throw out of it: calls return false and error() says why.
//
// Files a groom reads (texture maps, painted texture maps, referenced grooms) are read by the
// engine's own loaders from a folder standing for the .groom's: in the browser a folder of the
// module's memory file system, filled with addFile; natively the file's own folder.

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace groomlab {

class GroomSession;
struct GroomFile;

namespace web {

class WebGroom {
public:
    WebGroom();
    ~WebGroom();
    WebGroom(const WebGroom&) = delete;
    WebGroom& operator=(const WebGroom&) = delete;

    // A .groom's text (its scalp inside it), or a preset's graph JSON ({"nodes": ...}) with
    // `objText`, an OBJ mesh to be its scalp. An OBJ given with a .groom replaces its scalp (the
    // same topology, else the guides root again by position). False on failure (error()).
    bool load(const std::string& groomText, const std::string& objText);
    const std::string& error() const { return error_; }

    // The folder the groom's relative paths resolve from, as the desktop resolves them from the
    // .groom's own folder, then its search paths. In the browser "/groom" (the module's memory file
    // system), natively none until set (the probe sets the file's folder).
    void setDirectory(const std::string& directory);
    const std::string& directory() const { return directory_; }
    // The files the groom reads: [{path (as stored), resolved (where the engine reads it), found,
    // kind ("texture" or "groom"), udim, users (["<node>.<param>", "map <name>"])}], each path once.
    std::string filesJson() const;
    // The stored paths of those not found, as a JSON array.
    std::string missingFilesJson() const;
    // Writes a file: `path` relative to the groom's folder (an absolute one as it is; backslashes
    // count as slashes), `bytes` its content. Then every referenced file still missing whose name
    // is this file's (any case; a UDIM set's tiles by their pattern, a 1001-style tile's set by its
    // number) gets a copy where the engine reads it: maps dropped without their folders are found.
    bool addFile(const std::string& path, const std::string& bytes);
    // Removes the files addFile wrote (and the copies it made), nothing else.
    void clearFiles();

    // The sliders: [{id, kind, label, node, param, min, max, step, default, value, unit,
    // description}]: the Control nodes ("control:<name>"), then each Compound's exposed params,
    // else the graph's "exposed" list (a preset's), else a default pick of the look's key params
    // ("param:<node>.<param>").
    std::string controlsJson() const;
    // A slider's new value (an id from controlsJson). False for an unknown id.
    bool setParam(const std::string& id, double value);

    // Evaluates at `density` (the graph's preview density, 0-1). False on failure; the buffers
    // below then hold nothing.
    bool evaluate(double density);
    double evaluateMs() const { return evaluateMs_; }
    // The last evaluation's strands: count, offsets (count + 1), xyz per point, sRGB rgb per point.
    std::uint32_t strandCount() const { return static_cast<std::uint32_t>(offsets_.empty() ? 0 : offsets_.size() - 1); }
    const std::vector<std::uint32_t>& offsets() const { return offsets_; }
    const std::vector<float>& positions() const { return positions_; }
    const std::vector<float>& colors() const { return colors_; }
    // Each point's width (the strand's diameter there, scene units; -1 every point: no widths).
    const std::vector<float>& widths() const { return widths_; }
    // The guides as drawn (the flat guides, else the hierarchy's), the same layout.
    const std::vector<std::uint32_t>& guideOffsets() const { return guideOffsets_; }
    const std::vector<float>& guidePositions() const { return guidePositions_; }
    // The scalp as evaluated: xyz per vertex, smooth normals, three indices per triangle.
    const std::vector<float>& scalpPositions() const { return scalpPositions_; }
    const std::vector<float>& scalpNormals() const { return scalpNormals_; }
    const std::vector<std::uint32_t>& scalpIndices() const { return scalpIndices_; }
    // The evaluation's messages (errors, one per line), node timings and the hair's highlight (Maya's
    // viewport's: hair_bsdf.glsl's material), as JSON: {errors, nodes, evaluateMs, strands, points, look}.
    std::string statsJson() const;

    // The groom as a .groom file's text, with the edits made since it loaded.
    std::string saveGroom() const;
    // The graph's JSON now.
    std::string graphJson() const;
    // Replaces the graph (GraphError's text in error() on failure).
    bool setGraphJson(const std::string& graphJson);
    // The output of `graphJson` evaluated on this groom's scalp at full density, made the groom's
    // flat guides (sample grooms: guides baked from a small combing graph). The graph is put back.
    bool bakeGuides(const std::string& graphJson);

private:
    bool setGraphText(const std::string& text);
    void readScalp();
    void linkFile(const std::string& written);

    std::string directory_;
    std::vector<std::string> written_;  // what addFile wrote, then the copies it made

    std::unique_ptr<GroomFile> file_;
    std::unique_ptr<GroomSession> session_;
    mutable std::string error_;
    double evaluateMs_ = 0.0;
    std::vector<std::uint32_t> offsets_, guideOffsets_, scalpIndices_;
    std::vector<float> positions_, colors_, widths_, guidePositions_, scalpPositions_, scalpNormals_;
    std::vector<std::string> errors_;
};

}  // namespace web
}  // namespace groomlab
