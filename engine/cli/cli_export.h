// SPDX-License-Identifier: Apache-2.0
#pragma once

// Writing a groom's strands to Alembic, USD or a V-Ray scene frame by frame: what `groomlab-cli eval` does, and
// the MCP server's export tool (mcp_tools.cpp) with it, so the two write the same files.

#include <cstddef>
#include <ostream>
#include <string>

namespace groomlab {

class GroomScene;

namespace cli {

struct ExportOptions {
    std::string out;  // .abc, .usda, .usd or .vrscene
    double start = 1.0;
    double end = 1.0;
    double step = 1.0;
    float density = 1.0f;
    bool timings = false;         // the milliseconds each graph node took, per frame
    bool stats = false;           // the session's stats (GroomStats JSON) after the first frame
    std::string scales = "mesh";  // how scales are written (USD: mesh, instancer, both, none)
    bool velocities = false;      // Alembic: each point's velocity (.velocities), for motion blur
};

struct ExportReport {
    std::size_t frames = 0;
    std::size_t strandTotal = 0;  // over every frame
    double milliseconds = 0.0;
};

// Whether the path names a USD file (.usda, .usd) rather than Alembic.
bool isUsdPath(const std::string& path);
// Whether the path names a V-Ray scene (.vrscene).
bool isVrscenePath(const std::string& path);

// Checks the options (throws std::invalid_argument saying what is wrong), then evaluates `scene`
// at each frame and writes the strands. Progress lines ("frame 1: 1200 strands") go to `log` when
// given. Throws what the evaluation or the writers throw.
ExportReport exportStrands(GroomScene& scene, const ExportOptions& options, std::ostream* log);

}  // namespace cli
}  // namespace groomlab
