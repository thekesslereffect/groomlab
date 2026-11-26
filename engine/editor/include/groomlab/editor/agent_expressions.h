// SPDX-License-Identifier: Apache-2.0
#pragma once

// Checking an expression for the agent tools (agent_tools.h check_expression): compiled as a param
// or an Expression node compiles it (expression.h), what it reads, and its values sampled over a
// groom's strands (or scalp roots before there are any), so an agent sees what it would do before
// it sets it.

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include "groomlab/expression.h"
#include "groomlab/scalp_mesh.h"
#include "groomlab/strand_set.h"

namespace groomlab::agent {

// The variables an expression may read (expression.h), each with a line on what it is.
const std::vector<std::pair<std::string, std::string>>& expressionVariables();

struct ExpressionCheck {
    bool ok = false;
    std::string error;            // why it does not compile (with its position)
    bool vector = false;          // its value is a vector
    bool perPoint = false;        // it varies along each strand (reads a point variable or channel)
    bool readsTime = false;
    std::vector<std::string> variables;  // the built-in variables it reads
    std::vector<std::string> channels;   // the strand or point channels it reads (and whether the strands carry them)
    std::vector<std::string> missingChannels;
    std::vector<std::string> maps;       // the maps it names
    // Its values: over strands (`sampledStrands`; per point, their points too) or scalp roots.
    std::string sampledOver;  // "strands", "points", "scalp" or "" (nothing to sample)
    std::size_t samples = 0;
    float min = 0.0f, mean = 0.0f, max = 0.0f;  // a vector: its length's
    std::vector<std::size_t> histogram;          // 10 bins over min..max
    std::vector<std::pair<std::string, float>> examples;  // a few samples: where, value
};

// Compiles `text` (number or vector: `want`) with the constants given and samples it over
// `strands` (at most `maxSamples` strands, evenly spread; may be null) on `scalp`, or over roots on
// the scalp spread by area when there are no strands. Maps read the scalp's color sets.
ExpressionCheck checkExpression(const std::string& text, const std::vector<std::pair<std::string, float>>& constants,
                                const StrandSet* strands, const ScalpMesh* scalp, std::size_t maxSamples = 2000,
                                ExpressionType want = ExpressionType::Any);

}  // namespace groomlab::agent
