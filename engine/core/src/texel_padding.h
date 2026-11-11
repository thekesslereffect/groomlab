// SPDX-License-Identifier: Apache-2.0
#pragma once

// A baked square map's padding (the scales' maps, the follicle map): the empty texels next to
// filled ones take a neighbor's values, a ring per pass, so a render's filtering at an island's
// edge reads the island's values, not the background. (A painted texture map grows its padding
// differently: TexturePaint::toBytes averages, the live preview copies the nearest.)

#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

#include "groomlab/parallel.h"

namespace groomlab {

// `filled`: res x res, 1 where a texel holds a value (2 is taken over while a pass runs). Each pass
// gives every empty texel beside a filled one the values of its first filled neighbor (left,
// right, above, below: copy(to, from)), up to `passes` rings or until nothing grows; `filled` then
// marks them all.
template <typename Copy>
void padByNeighbors(std::vector<std::uint8_t>& filled, std::size_t res, int passes, Copy&& copy) {
    for (int pass = 0; pass < passes; ++pass) {
        std::vector<std::vector<std::pair<std::size_t, std::size_t>>> rows(res);
        parallelFor(0, res, [&](std::size_t y) {
            for (std::size_t x = 0; x < res; ++x) {
                const std::size_t i = y * res + x;
                if (filled[i]) continue;
                std::size_t from = i;
                if (x > 0 && filled[i - 1] == 1) from = i - 1;
                else if (x + 1 < res && filled[i + 1] == 1) from = i + 1;
                else if (y > 0 && filled[i - res] == 1) from = i - res;
                else if (y + 1 < res && filled[i + res] == 1) from = i + res;
                if (from != i) rows[y].emplace_back(i, from);
            }
        }, 16);
        bool grew = false;
        for (const auto& row : rows) {
            for (const auto& [to, from] : row) {
                copy(to, from);
                filled[to] = 2;  // (not a source until the next pass)
                grew = true;
            }
        }
        if (!grew) break;
        for (std::uint8_t& f : filled) f = f ? 1 : 0;
    }
}

}  // namespace groomlab
