// SPDX-License-Identifier: Apache-2.0
// Braid (Strands + Guides -> Strands): braids strands along braid paths.
//
// Each path (a guide, e.g. from a "braids" guide group) gathers the strands
// rooted within `region` of its root. Without a braids input it picks its own
// paths among the strands, about `region` apart over the scalp (as a Clump
// picks its centers by size), and every strand joins the nearest: box braids all
// over the head with nothing drawn.
//
// The weave is a pattern of crossings, as a braider makes it. The hair is split
// into sections, each with a slot across the braid; each step of the pattern
// takes one section across others, over one and under the next, and the
// sections it passes shift one slot to fill the gap. The styles are patterns:
//   three     right over the middle, then left over the middle
//   four      an outer section to the middle (over, under), each side in turn
//   five      the same over two (the five-strand flat braid)
//   fishtail  thin strips (`sections` a side), the outer strip of one half
//             crossing over to the other's inner edge
//   rope      `sections` plies (two by default) twisted round each other
//   cornrow   three crossing under, raised off the scalp it lies on
// `dutch` crosses under instead of over (a raised braid; a rope's other twist).
// Each section is a band along its own course (its thickness out of the braid,
// `flatness` from round to flat), placed so sections pass over and under each
// other instead of through: a strand keeps its spot within its section. A step
// is a share of the braid's width long (`tightness`: shorter, more crossings),
// so a tapering braid's crossings shrink with it, at any scene scale.
//
// Strands leave their roots over the first `gather` of their length (sweeping
// over the scalp into the braid, a clearance above it), the braid narrows
// along `widthProfile`, and over the last `tail` the sections let go and the
// ends fan out. `gatherSmoothness` and `tailSmoothness` fade the weave in and
// out: the sections begin (end) as one round bundle, the crossings growing
// (shrinking) over up to four weave steps. The hair is parted
// into the sections as it lies: a strand takes the section on its side of the
// braid where it joins (the region's roots cut into wedges side to side, one a
// section), so the sections leave the scalp without crossing each other.
// The braid faces out of the scalp at its root, or (a path leaving along the
// scalp normal, say a ponytail) away from the head; `roll` turns it.
//
// A braided strand gets the points its weave needs (a few a step, at most
// 1024), however few it came with. `display` paths shows each braid's path and
// its sections' center curves in place of the hair; the `paths` output hands
// the paths on as guides (those it picked, without a braids input).
//
// With `feed`, strands rooted within `region` of anywhere along a path's first
// `feedUntil` share join the braid where they are and run along it from there,
// as hair is picked up into cornrows and French braids (each strand to the
// nearer braid: two rows part halfway); past `feedUntil` the braid hangs free.
// A strand picked up joins the outer section on its side, as a braider adds
// hair, the braid thickening with the hair it holds, and is fitted to the path:
// it ends within the braid's last stretch (`tail`), wherever it joined.
//
// The ends (`end`): fan (over the last `tail` the sections let go and the ends
// spread), tuft (they let go into a gathered tuft, as below a hair tie) or bound
// (woven to the tip, narrowing: sealed box-braid ends, no tail). `ties` puts
// that many ties along each braid (the last where the weave ends), the braid
// tightening round each (`cinch`); the `ties` output hands them on as short
// strands along the braid, as wide as it is there, for an Instance node's
// `along` input: beads, cuffs and hair ties (groomlab:bead, groomlab:cuff,
// groomlab:hairTie; sizeMode fitStrand, fitWidth).
//
// Loose braids: `pancake` pulls the sections apart and loosens them (a braid pulled
// out by hand); with `loops`, in a share of the sections' stretches between
// crossings the hair on the section's outer side bows out off the braid's side
// (`loopSize` braid widths) and goes back in before the next crossing, as a loose
// braid's sections do. Ties hold them all the same.
//
// Version 2 (a node saved before keeps version 1 until it is upgraded: the sections plain
// elliptical tubes that pass through each other where they cross): the sections are pressed
// against each other. Each has an outline of its own in the braid's cross-section: grown
// (`inflate`), flat where it lies against another section, round where it lies free, never inside
// another, its corners and its changes along the braid rounded as a lock's are, the braid no
// wider or deeper for it (pressSections). `separate` blends that in. Version 2 also plaits the
// four- and five-strand braids (plaitWeave: each section from edge to edge, over one and under the
// next, where version 1's sections run through each other), lets a fishtail's halves take turns
// (fishtailWeave) and gives those braids' and a rope's strands more points a weave step
// (Braider::press).
//
// Output channels, per strand: `braidId` (the path's index among the guides, or for paths of its own a
// number from the picked strand's ID; -1 when not
// braided) and `braidFlat` (1 for a cornrow, lying on the scalp), for nodes
// downstream (Cards builds braid cards from them), and `braidSection` (the section of the
// braid the strand runs in, -1 when not braided; with display paths, -1 the path, else the
// section: Cards makes a tube per section from it). Per point, `braidTail`: 0 within the braid,
// rising to 1 down its tail (fan, tuft; free: over the release, 1 in the free tail), a mask for nodes after it (Curl, Clump,
// Noise with affect $braidTail shape the ends alone), and `braidWoven`: 0 on the way in (from
// the root to where the strand joins the braid: along the scalp when fed), rising to 1 in the
// braid (Cards builds a braid's tubes from the woven part), and `braidPress`: how much the strand
// lies against another section there (Braider::pressAt; 0 with plain sections), for a Clump's
// strength mapped by 1 - $braidPress to leave the contacts as pressed.

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <unordered_map>

#include "groomlab/braid_tubes.h"
#include "groomlab/curve.h"
#include "groomlab/frames.h"
#include "groomlab/graph.h"
#include "groomlab/operators.h"
#include "groomlab/parallel.h"
#include "groomlab/point_index.h"
#include "groomlab/portable_math.h"
#include "groomlab/process_caches.h"
#include "groomlab/profile.h"
#include "groomlab/random.h"
#include "incremental.h"
#include "internal.h"

namespace groomlab {

namespace {

constexpr float kTwoPi = 6.2831853f;
constexpr float kPi = 3.14159265f;
// A tie's width (its bore's diameter: a bead's or cuff's) as a share of the braid's width there; the
// hair inside it is held to just within that.
constexpr float kTieBore = 0.9f;

enum class Style { Three, Four, Five, Fishtail, Rope, Cornrow };
enum class End { Fan, Tuft, Bound, Free };

End endFrom(const std::string& name) {
    if (name == "fan") return End::Fan;
    if (name == "tuft") return End::Tuft;
    if (name == "bound") return End::Bound;
    if (name == "free") return End::Free;
    throw GraphError("Braid: end must be fan, tuft or bound");
}

Style styleFrom(const std::string& name) {
    if (name == "three") return Style::Three;
    if (name == "four") return Style::Four;
    if (name == "five") return Style::Five;
    if (name == "fishtail") return Style::Fishtail;
    if (name == "rope") return Style::Rope;
    if (name == "cornrow") return Style::Cornrow;
    throw GraphError("Braid: style must be three, four, five, fishtail, rope or cornrow");
}

float ease(float s) { return 0.5f - 0.5f * pm::cos(kPi * std::clamp(s, 0.0f, 1.0f)); }

// The weave: each section's track across the braid as the braid goes on, in braid widths ([0]
// side to side, along the frame's binormal; [1] depth, along its normal, out of the scalp), a
// periodic curve through kQ samples a step over the cycle (the steps after which every section is
// back in its slot). A section's cross-section is an ellipse: halfWidth across its band, halfThick
// out of it (round: plies, thick along the radius from the braid's axis).
struct Weave {
    static constexpr int kQ = 4;
    static constexpr int kMostSections = 24;  // a fishtail of twelve strips a side
    int sections = 0;
    int cycle = 1;
    float halfWidth = 0.2f, halfThick = 0.1f;
    float stepLength = 0.6f;  // a step, in braid widths, at tightness 1
    bool round = false;
    bool alternates = false;  // a flat braid: its crossings alternate sides, the right (+binormal) on even steps
    float lift = 0.0f;  // raised by (braid widths): a cornrow lies on the scalp
    std::vector<std::vector<std::array<float, 2>>> tracks;
    // Each section's stretches between crossings, where it runs along the braid's edge (its side
    // out there: the part of it a braid shows), as [start, end) in steps from the cycle's start (an
    // end past the cycle wraps round).
    std::vector<std::vector<std::array<float, 2>>> lobes;
    // A weave patterned anew for version 2 (plaitWeave): version 1's of the same style, whose reach
    // side to side and in depth the pressed braid is scaled back to (pressSections). Null: its own.
    std::shared_ptr<const Weave> plain;

    // Finds the lobes: where a section is out toward the braid's edge (over 0.55 of the farthest
    // any section goes), run by run round the cycle.
    void findLobes() {
        float widest = 0.0f;
        for (const auto& t : tracks) {
            for (const auto& c : t) widest = std::max(widest, std::abs(c[0]));
        }
        lobes.assign(tracks.size(), {});
        if (!(widest > 0.0f)) return;
        const float edge = 0.55f * widest, q = 1.0f / static_cast<float>(kQ);
        for (std::size_t k = 0; k < tracks.size(); ++k) {
            const auto& t = tracks[k];
            const std::size_t count = t.size();
            auto out = [&](std::size_t i) { return std::abs(t[i % count][0]) >= edge; };
            std::size_t first = 0;  // a sample where it is in, to start the runs from
            while (first < count && out(first)) ++first;
            if (first == count) continue;  // always out: no stretch of its own (a rope's plies are)
            for (std::size_t i = first; i < first + count; ++i) {
                if (!out(i) || out(i - 1 + count)) continue;  // a run starts here
                std::size_t j = i;
                while (out(j)) ++j;
                const float start = static_cast<float>(i % count) * q;
                lobes[k].push_back({start, start + static_cast<float>(j - i) * q});
            }
        }
    }

    // The lowest a section reaches (its underside), braid widths below the path.
    float lowest() const {
        float low = 0.0f;
        for (const auto& t : tracks) {
            for (const auto& c : t) low = std::min(low, c[1] - halfThick);
        }
        return low;
    }

    std::array<float, 2> at(int section, float steps, std::array<float, 2>* slope = nullptr) const {
        const auto& t = tracks[static_cast<std::size_t>(section)];
        const int count = static_cast<int>(t.size());
        float f = steps * static_cast<float>(kQ);
        f -= std::floor(f / static_cast<float>(count)) * static_cast<float>(count);
        const int i = std::min(static_cast<int>(f), count - 1);
        const float u = f - static_cast<float>(i);
        auto p = [&](int k) { return t[static_cast<std::size_t>(((k % count) + count) % count)]; };
        const auto p0 = p(i - 1), p1 = p(i), p2 = p(i + 1), p3 = p(i + 2);
        std::array<float, 2> out{};
        for (std::size_t c = 0; c < 2; ++c) {
            const float a = 2.0f * p1[c], b = p2[c] - p0[c], d = 2.0f * p0[c] - 5.0f * p1[c] + 4.0f * p2[c] - p3[c],
                        e = -p0[c] + 3.0f * p1[c] - 3.0f * p2[c] + p3[c];
            out[c] = 0.5f * (a + b * u + d * u * u + e * u * u * u);
            if (slope) (*slope)[c] = 0.5f * (b + 2.0f * d * u + 3.0f * e * u * u) * static_cast<float>(kQ);
        }
        return out;
    }
};

// A track made to flow: its slower motions kept (each harmonic of the cycle whose period is at
// least `shortest` steps), the step-by-step jerks of the pattern (a section crossing, then
// lying still) smoothed away, as a section of hair runs on in one curve.
void smoothTrack(std::vector<std::array<float, 2>>& t, int cycle, float shortest) {
    const std::size_t count = t.size();
    const int highest = std::max(1, static_cast<int>(static_cast<float>(cycle) / shortest));
    std::vector<std::array<float, 2>> out(count, {0.0f, 0.0f});
    for (int h = 0; h <= highest; ++h) {
        double re[2] = {0, 0}, im[2] = {0, 0};
        for (std::size_t i = 0; i < count; ++i) {
            const double a = 6.283185307179586 * h * static_cast<double>(i) / static_cast<double>(count);
            for (std::size_t c = 0; c < 2; ++c) {
                re[c] += t[i][c] * std::cos(a);
                im[c] -= t[i][c] * std::sin(a);
            }
        }
        const double scale = (h == 0 ? 1.0 : 2.0) / static_cast<double>(count);
        for (std::size_t i = 0; i < count; ++i) {
            const double a = 6.283185307179586 * h * static_cast<double>(i) / static_cast<double>(count);
            for (std::size_t c = 0; c < 2; ++c) {
                out[i][c] += static_cast<float>(scale * (re[c] * std::cos(a) - im[c] * std::sin(a)));
            }
        }
    }
    t = std::move(out);
}

// A flat braid of `n` sections: each step an outer section (right, then left) goes `reach` slots
// toward the middle, over the first section it passes and under the next (`sign` -1: under first),
// the sections it passes shifting one slot out to fill the gap.
Weave flatWeave(int n, int reach, float sign, float flatness) {
    Weave w;
    w.sections = n;
    w.alternates = true;
    w.halfWidth = 1.0f / (2.0f + 1.6f * static_cast<float>(n - 1));  // sections a little wider than their slots
    w.halfThick = w.halfWidth * std::max(1.0f - 0.75f * flatness, 0.2f);
    const float spacing = 1.6f * w.halfWidth;
    auto slotX = [&](int j) { return -0.5f + w.halfWidth + spacing * static_cast<float>(j); };
    const float b = w.halfThick;
    std::vector<int> at(static_cast<std::size_t>(n));  // the section in each slot
    for (int j = 0; j < n; ++j) at[static_cast<std::size_t>(j)] = j;
    w.tracks.assign(static_cast<std::size_t>(n), {});
    for (int step = 0; step < 512; ++step) {
        const bool right = step % 2 == 0;
        const int from = right ? n - 1 : 0, dir = right ? -1 : 1, to = from + dir * reach;
        const int mover = at[static_cast<std::size_t>(from)];
        for (int q = 0; q < Weave::kQ; ++q) {
            const float s = static_cast<float>(q) / static_cast<float>(Weave::kQ);
            for (int j = 0; j < n; ++j) {
                const int section = at[static_cast<std::size_t>(j)];
                std::array<float, 2> c{slotX(j), 0.0f};
                if (j == from) {
                    c[0] = slotX(from) + (slotX(to) - slotX(from)) * ease(s);
                    const float passed = s * static_cast<float>(reach);  // over each passed section in turn
                    const int k = std::min(static_cast<int>(passed), reach - 1);
                    c[1] = sign * (k % 2 == 0 ? 1.0f : -1.0f) * b * pm::sin(kPi * (passed - static_cast<float>(k)));
                } else if ((j - from) * dir > 0 && (j - from) * dir <= reach) {  // passed: shifting out
                    const int k = (j - from) * dir - 1;
                    c[0] = slotX(j) + (slotX(j - dir) - slotX(j)) * ease(s);
                    c[1] = -sign * (k % 2 == 0 ? 1.0f : -1.0f) * b * pm::sin(kPi * s);
                }
                w.tracks[static_cast<std::size_t>(section)].push_back(c);
            }
        }
        for (int k = 0; k < reach; ++k) at[static_cast<std::size_t>(from + dir * k)] = at[static_cast<std::size_t>(from + dir * (k + 1))];
        at[static_cast<std::size_t>(to)] = mover;
        bool home = step % 2 == 1;
        for (int j = 0; j < n && home; ++j) home = at[static_cast<std::size_t>(j)] == j;
        if (home) {
            w.cycle = step + 1;
            break;
        }
    }
    // (a section crossing two slots a step moves faster: less is smoothed away)
    for (auto& t : w.tracks) smoothTrack(t, w.cycle, reach > 1 ? 2.0f : 3.0f);
    return w;
}

// A flat braid of `n` sections plaited, as a braider's hands leave it (version 2's four- and
// five-strand braids): every section runs from one edge of the braid to the other and back, over
// the first section it meets and under the next, while those it passes go the other way; across
// the braid the sections so lie in two layers, up and down in turn, and two trade places by going
// round each other, never nearer than about a section's size (flatWeave's take an outer section two
// slots in a step, through the ones it passes). A cycle is 2 n steps: one edge to the other and
// back, each section two steps behind the one before; with an odd count the sections turn at the
// right edge on even steps and at the left on odd ones.
Weave plaitWeave(int n, float sign, float flatness) {
    constexpr float kDepth = 1.2f;  // how far over and under, in half thicknesses
    constexpr float kTurn = 0.9f;   // how sharp the turn at an edge is (1: a zigzag's corner; lower, rounder)
    Weave w;
    w.sections = n;
    w.cycle = 2 * n;
    w.alternates = n % 2 == 1;
    w.halfWidth = 1.0f / (2.0f + 1.6f * static_cast<float>(n - 1));
    w.halfThick = w.halfWidth * std::max(1.0f - 0.75f * flatness, 0.2f);
    const float reach = 0.5f - w.halfWidth, depth = kDepth * w.halfThick;
    const float crossings = static_cast<float>(n - 1);  // a section's ups and downs from one edge and back
    w.tracks.assign(static_cast<std::size_t>(n), {});
    for (int k = 0; k < n; ++k) {
        for (int q = 0; q < w.cycle * Weave::kQ; ++q) {
            const float steps = static_cast<float>(q) / static_cast<float>(Weave::kQ);
            const float turn = 0.5f * kPi + kTwoPi * (steps - 2.0f * static_cast<float>(k)) / static_cast<float>(w.cycle);
            // (over first from the right edge; an even count's sections go under first from the left,
            // which is what lets two that meet anywhere pass one over the other)
            const float wave = n % 2 == 1 ? pm::sin(crossings * turn) : pm::cos(crossings * turn);
            w.tracks[static_cast<std::size_t>(k)].push_back({reach * pm::asin(kTurn * pm::sin(turn)) / pm::asin(kTurn), sign * depth * wave});
        }
    }
    return w;
}

// A rope of `n` plies round its axis, each step a ply's turn past the next (`sign`: the twist's
// direction).
Weave ropeWeave(int n, float sign, float flatness) {
    Weave w;
    w.sections = n;
    w.round = true;
    const float s = pm::sin(kPi / static_cast<float>(n));
    w.halfWidth = w.halfThick = 0.5f * s / (1.0f + s);
    const float r = 0.5f - w.halfWidth, squash = std::max(1.0f - 0.6f * flatness, 0.3f);
    w.cycle = n;
    w.stepLength = 2.6f / static_cast<float>(n);
    w.tracks.assign(static_cast<std::size_t>(n), {});
    for (int i = 0; i < n; ++i) {
        for (int q = 0; q < n * Weave::kQ; ++q) {
            const float a = kTwoPi * (static_cast<float>(i) + sign * static_cast<float>(q) / static_cast<float>(Weave::kQ)) /
                            static_cast<float>(n);
            w.tracks[static_cast<std::size_t>(i)].push_back({r * pm::cos(a), r * squash * pm::sin(a)});
        }
    }
    return w;
}

// A fishtail of `m` thin strips a side: each step the outer strip of one half crosses over the
// front of its half to the other's inner edge (right, then left). Each half is a round bundle: a
// strip that comes in lies on top at its inner edge, is buried under the ones that come after it,
// and comes round to the outer edge to be taken across. `pressed` (version 2): the halves take
// turns, as a braider's hands do (a strip lies half a step less in the right half than in the left,
// so the strip crossing one way is half a step ahead of the one crossing back and they pass one
// over the other, where crossing together they meet in the middle), and a strip crosses lower,
// close over the bundle.
Weave fishtailWeave(int m, float sign, float flatness, bool pressed = false) {
    Weave w;
    const int n = 2 * m;
    w.sections = n;
    w.cycle = n;
    const float rx = 0.24f, rd = rx * std::max(1.0f - 0.6f * flatness, 0.3f), mid = 0.5f - rx;
    w.halfWidth = 1.3f * rx / static_cast<float>(m);
    w.halfThick = w.halfWidth * std::max(1.0f - 0.75f * flatness, 0.25f);
    w.stepLength = 2.0f / static_cast<float>(n);
    auto lying = [&](float j, float side) -> std::array<float, 2> {  // j: 0 just in (on top) to m - 1 outermost
        const float u = j / static_cast<float>(std::max(m - 1, 1));
        const float a = kPi * (0.75f + 1.25f * u);  // from the top, inner side, round the back to the outer edge
        return {side * (mid + rx * pm::cos(a)), rd * pm::sin(a)};
    };
    w.tracks.assign(static_cast<std::size_t>(n), {});
    for (int i = 0; i < n; ++i) {
        for (int q = 0; q < n * Weave::kQ; ++q) {
            const float phi = std::fmod(static_cast<float>(q) / static_cast<float>(Weave::kQ) + static_cast<float>(i), static_cast<float>(n));
            std::array<float, 2> c;
            if (pressed) {
                const float turn = static_cast<float>(m) - 0.5f;  // the steps a strip spends in the right half
                const float half = phi < turn ? 1.0f : -1.0f;
                const float j = phi - (half > 0.0f ? 0.0f : turn);
                const float lies = (half > 0.0f ? turn : static_cast<float>(n) - turn) - 1.0f;  // lying, then a step crossing
                if (j <= lies) {
                    c = lying(j * static_cast<float>(m - 1) / lies, half);
                } else {
                    const float s = j - lies;
                    const auto from = lying(static_cast<float>(m - 1), half), onto = lying(0.0f, -half);
                    c = {from[0] + (onto[0] - from[0]) * ease(s),
                         from[1] + (onto[1] - from[1]) * ease(s) + sign * 0.7f * rd * pm::sin(kPi * s)};
                }
                w.tracks[static_cast<std::size_t>(i)].push_back(c);
                continue;
            }
            const float half = phi < static_cast<float>(m) ? 1.0f : -1.0f;  // right half first
            const float j = phi - (half > 0.0f ? 0.0f : static_cast<float>(m));
            if (j <= static_cast<float>(m - 1)) {
                c = lying(j, half);
            } else {  // crossing over the front of its half to the other's inner edge
                const float s = j - static_cast<float>(m - 1);
                const auto from = lying(static_cast<float>(m - 1), half), onto = lying(0.0f, -half);
                c = {from[0] + (onto[0] - from[0]) * ease(s),
                     from[1] + (onto[1] - from[1]) * ease(s) + sign * 1.2f * rd * pm::sin(kPi * s)};
            }
            w.tracks[static_cast<std::size_t>(i)].push_back(c);
        }
    }
    return w;
}

Weave weaveOf(Style style, std::int64_t sections, bool dutch, float flatness, bool pressed) {
    const float sign = dutch ? -1.0f : 1.0f;
    switch (style) {
        case Style::Three: return flatWeave(3, 1, sign, flatness);
        case Style::Four:
        case Style::Five: {
            const int n = style == Style::Four ? 4 : 5;
            if (!pressed) return flatWeave(n, 2, sign, flatness);
            Weave w = plaitWeave(n, sign, flatness);
            w.plain = std::make_shared<const Weave>(flatWeave(n, 2, sign, flatness));
            return w;
        }
        case Style::Fishtail:
            return fishtailWeave(sections > 0 ? static_cast<int>(std::clamp<std::int64_t>(sections, 3, 12)) : 6, sign, flatness, pressed);
        case Style::Rope: return ropeWeave(sections > 0 ? static_cast<int>(std::clamp<std::int64_t>(sections, 2, 8)) : 2, sign, flatness);
        case Style::Cornrow: {
            Weave w = flatWeave(3, 1, -1.0f, flatness);  // crossing under: raised
            w.lift = -w.lowest();                         // the lowest section on the scalp
            return w;
        }
    }
    return flatWeave(3, 1, sign, flatness);
}

// A style's weave. `pressed`: as a version 2 node patterns it (its sections pressed against each
// other, pressSections): a four- and a five-strand braid are plaited (plaitWeave) and a fishtail's
// halves take turns (fishtailWeave); a three-strand braid's, a cornrow's and a rope's courses are
// version 1's.
Weave weaveFor(Style style, std::int64_t sections, bool dutch, float flatness, bool pressed) {
    Weave w = weaveOf(style, sections, dutch, flatness, pressed);
    w.findLobes();
    return w;
}

// ---- Version 2: the sections pressed against each other (`separate`, `inflate`) -----------------
//
// As patterned a section is an elliptical tube along its course, and the tubes pass through each
// other where they cross and lie apart elsewhere. A version 2 node presses the tubes against each
// other instead. Each section stays a tube swept along its own course: rings square to the course
// (across its band, through it), the hair running along it as the tube's own lengthwise lines.
//
//   - Where two courses cross closer than the sections' thickness lets them, the centers move
//     apart in depth first, so each keeps the room its hair takes.
//   - A ring point lies out along its ray (the ellipse's radius at that angle) as far as the point
//     is still deeper in its own tube than in any other, each in its own size: two tubes that would
//     overlap end on the surface halfway between them, a flat facet both lie against.
//   - Free, a tube is as large as `inflate` grows it (by up to kGrow, times what the style's
//     sections lack to touch at all, a fishtail's thin strips), and a little larger where a contact
//     presses hair out of it: its ring keeps about the same area all along, the same hair.
//   - The scales are smoothed round the ring and along the section, never past a limit: a contact
//     fades in and out, its edge is a rounded corner.
//   - The whole is solved at the reach the weave has with its sections as plain ellipses:
//     `inflate` makes the sections fat for their spacing, not the braid wider or deeper than its
//     width says.
//
// A ring is a scale along each of kAround rays from the section's center, worked out at kPerStep
// samples a weave step over the stretch after which the weave repeats with its sections renamed.
// Nothing is iterated to rest. Kept for the process (pressedFor). A strand keeps its ray and its
// share of the way out along it, so it stays in its section wherever that goes.

// A section where the braid's cross-section cuts it, in braid widths ([0] side to side, [1] depth).
struct SectionCut {
    std::array<float, 2> center;
    // Its unit axes (across its band, through it) carried along its course onto the cross-section:
    // times its half sizes, the cut ellipse's half axes.
    std::array<float, 2> across, thick;
    // The axes as they are (the tube's own cross-section, square to its course) seen along the
    // braid: how far a plain elliptical tube reaches.
    std::array<float, 2> acrossFlat, thickFlat;
};

// The steepest a course counts as (its slope in the cross-section, a unit along the braid: 60
// degrees), so crossings packed far tighter than hair braids keep cuts of a sane length.
constexpr float kSteepest = 1.73f;

// Section `section` at `steps` cut by the braid's cross-section. `tilt`: the weave's steps a braid
// width along the braid (what a course's slope a step comes to a unit along).
SectionCut cutOf(const Weave& w, int section, float steps, float pancake, float tilt) {
    std::array<float, 2> slope{};
    std::array<float, 2> c = w.at(section, steps, &slope);
    if (pancake > 0.0f) {  // pulled apart: the sections out from the middle (Braider::sectionAt)
        const float out = 1.0f + 0.45f * pancake;
        c = {c[0] * out, c[1] * (1.0f + 0.25f * pancake)};
        slope = {slope[0] * out, slope[1] * (1.0f + 0.25f * pancake)};
    }
    float side = slope[0] * tilt, depth = slope[1] * tilt;
    const float steep = std::sqrt(side * side + depth * depth);
    if (steep > kSteepest) {
        side *= kSteepest / steep;
        depth *= kSteepest / steep;
    }
    // (in the frame's order: normal, binormal, tangent)
    const Vec3f along = Vec3f{depth, side, 1.0f}.normalized();
    Vec3f thick = w.round ? Vec3f{c[1], c[0], 0.0f} : Vec3f{1.0f, 0.0f, 0.0f};
    thick = thick - along * thick.dot(along);
    if (thick.length() < 1e-5f) thick = Vec3f{1.0f, 0.0f, 0.0f} - along * along.x;
    thick = thick.normalized();
    const Vec3f across = along.cross(thick);
    SectionCut cut;
    cut.center = c;
    cut.across = {across.y - side * across.z, across.x - depth * across.z};
    cut.thick = {thick.y - side * thick.z, thick.x - depth * thick.z};
    cut.acrossFlat = {across.y, across.x};
    cut.thickFlat = {thick.y, thick.x};
    return cut;
}

// A weave's sections pressed against each other (pressSections).
struct Pressed {
    static constexpr int kPerStep = 64;  // samples a weave step
    static constexpr int kAround = 48;   // rays round a section
    int sections = 0;
    int count = 0;  // samples over the cycle
    // The samples worked out: after `span` the weave is as it began with its sections renamed
    // (repeatOf), and so are their tubes. `from`[(sample / span) * sections + section]: the
    // section whose rows hold that section's there.
    int span = 0;
    std::vector<std::uint8_t> from;
    std::vector<float> scales;  // [(section * span + sample) * kAround + ray]: the outline's scale along the ray
    // As `scales`: how much the outline lies against another section along the ray (markContacts),
    // 0 where it reaches as far as the section's free side, 1 on a contact's flat facet.
    std::vector<float> contact;
    // [(section * span + sample) * 8]: the tube's ring there, in braid widths: its center (side to
    // side, depth), then the ring's two directions in the path's frame (normal, binormal, tangent),
    // across the band and through it, square to the section's own course, each as long as the
    // section's size was scaled (the braid's reach kept).
    std::vector<float> rings;
    // What is left to scale, side to side and in depth, to the braid's reach (about 1: the tubes are
    // solved at the reach already).
    std::array<float, 2> keep{1.0f, 1.0f};
    float lowest = 0.0f;  // the lowest any outline reaches, scaled (braid widths below the path: negative)
    float grown = 1.0f;   // what the sections were grown by before the press
    // What `inflate` alone grows a plain section by (no press, no room to fill): the size the
    // sections have with `separate` 0, where they pass through each other.
    float inflated = 1.0f;

    std::size_t at(int section, int sample) const {
        const int block = sample / span;
        const std::size_t source = from[static_cast<std::size_t>(block * sections + section)];
        return source * static_cast<std::size_t>(span) + static_cast<std::size_t>(sample - block * span);
    }
    // A section's outline at a sample of the cycle: kAround scales.
    const float* row(int section, int sample) const { return scales.data() + at(section, sample) * kAround; }
    // Its ring there: 8 numbers.
    const float* ring(int section, int sample) const { return rings.data() + at(section, sample) * 8; }
    // The contact along each ray (kAround) of the outline that row() gave as `row`.
    const float* contactOf(const float* row) const { return contact.data() + (row - scales.data()); }
};

// Marks where each outline lies against another section (Pressed::contact). An outline reaches as
// far as its section's free side along a ray that touches nothing (pressSections: as far as the
// grown section's area lets it; pressAcross: its cell's rounded rim) and is pressed in short of
// that where another section takes over. The free reach is the ring's farthest, at least the
// grown size (a section pressed on every side); a ray pressed in by kFrom of it or less is free,
// by kTo or more fully on a contact, smoothly between. The outlines are smoothed round the ring and
// along the section already, so the contacts fade in and out with them.
void markContacts(Pressed& p) {
    constexpr float kFrom = 0.04f, kTo = 0.25f;
    constexpr std::size_t around = Pressed::kAround;
    p.contact.assign(p.scales.size(), 0.0f);
    for (std::size_t at = 0; at + around <= p.scales.size(); at += around) {
        const float* s = p.scales.data() + at;
        float free = p.grown;
        for (std::size_t j = 0; j < around; ++j) free = std::max(free, s[j]);
        if (!(free > 0.0f)) continue;
        for (std::size_t j = 0; j < around; ++j) p.contact[at + j] = smoothstep(kFrom, kTo, 1.0f - s[j] / free);
    }
}

// The steps after which a weave is as it began with its sections renamed (a three-strand braid
// after a crossing from each side, a rope after one ply's turn), at most its cycle. `next`: the
// section that lies then where each lay.
int repeatOf(const Weave& w, std::vector<int>& next) {
    const int n = w.sections;
    const int count = static_cast<int>(w.tracks.front().size());
    for (int steps = 1; steps < w.cycle; ++steps) {
        if (w.cycle % steps != 0) continue;
        const int shift = steps * Weave::kQ;
        next.assign(static_cast<std::size_t>(n), -1);
        std::vector<char> taken(static_cast<std::size_t>(n), 0);
        bool all = true;
        for (int k = 0; k < n && all; ++k) {
            const auto& mine = w.tracks[static_cast<std::size_t>(k)];
            for (int o = 0; o < n && next[static_cast<std::size_t>(k)] < 0; ++o) {
                if (taken[static_cast<std::size_t>(o)]) continue;
                const auto& other = w.tracks[static_cast<std::size_t>(o)];
                bool same = true;
                for (int q = 0; q < count && same; ++q) {
                    const std::array<float, 2>& p = mine[static_cast<std::size_t>(q)];
                    const std::array<float, 2>& r = other[static_cast<std::size_t>((q + shift) % count)];
                    same = std::abs(p[0] - r[0]) <= 1e-5f && std::abs(p[1] - r[1]) <= 1e-5f;
                }
                if (same) {
                    next[static_cast<std::size_t>(k)] = o;
                    taken[static_cast<std::size_t>(o)] = 1;
                }
            }
            all = next[static_cast<std::size_t>(k)] >= 0;
        }
        if (all) return steps;
    }
    next.resize(static_cast<std::size_t>(n));
    for (int k = 0; k < n; ++k) next[static_cast<std::size_t>(k)] = k;
    return w.cycle;
}

// The most `inflate` counts for: above 1 it is overdriven (sections fatter than the braid's room;
// pressed, they flatten harder against each other), up to this.
constexpr double kMostInflate = 4.0;

// Presses a weave's sections against each other (above). `tilt`: cutOf's.
Pressed pressSections(const Weave& w, float pancake, float inflate, float tilt) {
    constexpr int kAround = Pressed::kAround, kPerStep = Pressed::kPerStep;
    constexpr double kGrow = 1.6;    // the growth at inflate 1
    constexpr double kRoom = 1.25;   // the most a tube spreads where it is free, past its grown size, to keep the area others press out of it
    constexpr double kClear = 0.85;  // crossing sections' centers are set this share of touching apart in depth
    constexpr double kLeast = 0.05;  // (a tube never closes: two courses through one point)
    constexpr int kMarch = 10;       // steps out along a ray to find where another tube takes over
    constexpr double kLean = 8.0;    // the steepest a course counts as (its slope a unit along the braid: 83 degrees)
    constexpr int kEaseRounds = 3;   // push-and-ease rounds for the crossings' depth (below)
    Pressed out;
    out.inflated = static_cast<float>(1.0 + (kGrow - 1.0) * std::clamp(static_cast<double>(inflate), 0.0, kMostInflate));
    const int n = w.sections;
    out.sections = n;
    out.count = w.cycle * kPerStep;
    std::vector<int> next;
    const int repeat = repeatOf(w, next);
    out.span = repeat * kPerStep;
    const int blocks = out.count / out.span;
    out.from.resize(static_cast<std::size_t>(blocks * n));
    for (int k = 0; k < n; ++k) out.from[static_cast<std::size_t>(k)] = static_cast<std::uint8_t>(k);
    for (int block = 1; block < blocks; ++block) {
        for (int k = 0; k < n; ++k) {
            out.from[static_cast<std::size_t>(block * n + next[static_cast<std::size_t>(k)])] = out.from[static_cast<std::size_t>((block - 1) * n + k)];
        }
    }
    const std::size_t sections = static_cast<std::size_t>(n);
    const double loose = 1.0 + 0.6 * pancake;
    const double a = w.halfWidth * loose, b = w.halfThick * loose;
    // (square roots, not hypot: the same bits on every platform)
    const auto far = [](double x, double y) { return std::sqrt(x * x + y * y); };
    // The reach of the plain tubes, side to side and in depth (version 1's weave where this one is
    // patterned anew: the braid is as wide and deep as it was), and how far apart the centers lie
    // in the sections' own size.
    std::array<double, 2> reachPlain{0.0, 0.0};
    {
        const Weave& was = w.plain ? *w.plain : w;
        for (int k = 0; k < was.sections; ++k) {
            for (int i = 0; i < was.cycle * kPerStep; ++i) {
                const SectionCut cut = cutOf(was, k, static_cast<float>(i) / static_cast<float>(kPerStep), pancake, tilt);
                reachPlain = {std::max(reachPlain[0], std::abs(static_cast<double>(cut.center[0])) + far(cut.acrossFlat[0] * a, cut.thickFlat[0] * b)),
                              std::max(reachPlain[1], std::abs(static_cast<double>(cut.center[1])) + far(cut.acrossFlat[1] * a, cut.thickFlat[1] * b))};
            }
        }
    }
    double near = 0.0;
    for (int i = 0; i < out.span; ++i) {
        std::array<double, 2> at[Weave::kMostSections];
        for (int k = 0; k < n; ++k) {
            const SectionCut cut = cutOf(w, k, static_cast<float>(i) / static_cast<float>(kPerStep), pancake, tilt);
            at[k] = {cut.center[0] / a, cut.center[1] / b};
        }
        for (int k = 0; k < n; ++k) {
            double nearest = std::numeric_limits<double>::infinity();
            for (int o = 0; o < n; ++o) {
                if (o != k) nearest = std::min(nearest, far(at[o][0] - at[k][0], at[o][1] - at[k][1]));
            }
            near += nearest;
        }
    }
    // Grown: from where the sections touch at all (two a distance d apart touch at half of it).
    const double touch = 0.5 * near / static_cast<double>(out.span * n);
    const double grown = std::min(std::max(touch, 1.0), 1e9) * (1.0 + (kGrow - 1.0) * std::clamp(static_cast<double>(inflate), 0.0, kMostInflate));
    const double most = grown * kRoom;
    out.grown = static_cast<float>(grown);

    // A tube's ring at a sample: its center and frame (normal: depth, binormal: side, tangent).
    struct Ring {
        double c[3], along[3], across[3], thick[3];
    };
    // The tubes solved with the weave scaled (kx side to side, ky in depth) at `per` samples a step
    // and `rays` round; returns their reach side to side, in depth, and their lowest. `keepIt`:
    // into `out` (at kPerStep, kAround).
    const auto solve = [&](double kx, double ky, int per, int rays, bool keepIt) {
        const int total = w.cycle * per, span = repeat * per;
        const std::size_t whole = static_cast<std::size_t>(total);
        const double size = w.round ? std::sqrt(kx * ky) : 0.0;
        const double sa = a * (w.round ? size : kx), sb = b * (w.round ? size : ky);
        const double dz = 1.0 / (static_cast<double>(per) * tilt);  // a sample, in braid widths along the braid
        const auto wrap = [total](int g) { return ((g % total) + total) % total; };
        // A section's own rows hold `span` samples; elsewhere in the cycle another section's do.
        const auto rowOf = [&](int k, int g) {
            g = wrap(g);
            const int block = g / span;
            return static_cast<std::size_t>(out.from[static_cast<std::size_t>(block * n + k)]) * static_cast<std::size_t>(span) +
                   static_cast<std::size_t>(g - block * span);
        };
        std::vector<Ring> tubes(sections * whole);
        const double outX = (1.0 + 0.45 * pancake) * kx, outY = (1.0 + 0.25 * pancake) * ky;
        for (int k = 0; k < n; ++k) {
            for (int i = 0; i < total; ++i) {
                const std::array<float, 2> c = w.at(k, static_cast<float>(i) / static_cast<float>(per));
                Ring& r = tubes[static_cast<std::size_t>(k) * whole + static_cast<std::size_t>(i)];
                r.c[0] = c[1] * outY, r.c[1] = c[0] * outX, r.c[2] = 0.0;
            }
        }
        // Where two sections cross one over the other closer than their thickness lets them, their
        // centers move apart in depth (each half the way), eased along their courses: each keeps
        // the room its hair takes instead of one being carved down to a sliver.
        if (!w.round) {
            std::vector<double> lift(sections * whole, 0.0), push(sections * whole);
            std::vector<int> pushed(sections * whole);
            // Pushed apart, then eased along the courses over about a step, a few rounds: the depth
            // a crossing asks for swells in and out smoothly instead of bending the tubes at corners
            // where it starts and ends (the press flattens what easing leaves touching).
            const int easeTo = std::max(1, per);
            for (int round = 0; round < kEaseRounds; ++round) {
            for (int pass = 0; pass < 8; ++pass) {
                std::fill(push.begin(), push.end(), 0.0);
                std::fill(pushed.begin(), pushed.end(), 0);
                for (int i = 0; i < total; ++i) {
                    for (int k = 0; k < n; ++k) {
                        for (int o = k + 1; o < n; ++o) {
                            const std::size_t mine = static_cast<std::size_t>(k) * whole + static_cast<std::size_t>(i);
                            const std::size_t other = static_cast<std::size_t>(o) * whole + static_cast<std::size_t>(i);
                            const double dx = (tubes[mine].c[1] - tubes[other].c[1]) / (2.0 * sa);
                            if (std::abs(dx) >= 1.0) continue;
                            const double need = kClear * 2.0 * sb * std::sqrt(1.0 - dx * dx);
                            const double dy = (tubes[mine].c[0] + lift[mine]) - (tubes[other].c[0] + lift[other]);
                            if (std::abs(dy) >= need) continue;
                            const double half = 0.5 * (need - std::abs(dy)) * (dy >= 0.0 ? 1.0 : -1.0);
                            push[mine] += half, push[other] -= half;
                            ++pushed[mine], ++pushed[other];
                        }
                    }
                }
                // (each by the mean of what its crossings ask: many at once, a fishtail's strips, never overshoot)
                for (std::size_t q = 0; q < lift.size(); ++q) lift[q] += push[q] / std::max(pushed[q], 1);
            }
            // (eased: with its neighbours one, two, four... samples away, up to easeTo)
            for (int stride = 1; stride <= easeTo; stride *= 2) {
                for (int k = 0; k < n; ++k) {
                    const double* from = lift.data() + static_cast<std::size_t>(k) * whole;
                    double* to = push.data() + static_cast<std::size_t>(k) * whole;
                    for (int i = 0; i < total; ++i) to[i] = 0.25 * from[wrap(i - stride)] + 0.5 * from[i] + 0.25 * from[wrap(i + stride)];
                }
                lift.swap(push);
            }
            }
            for (std::size_t q = 0; q < lift.size(); ++q) tubes[q].c[0] += lift[q];
        }
        // Each ring square to its section's own course: along it, through the band (a ply: out from
        // the braid's axis), across it.
        for (int k = 0; k < n; ++k) {
            Ring* mine = tubes.data() + static_cast<std::size_t>(k) * whole;
            for (int i = 0; i < total; ++i) {
                const Ring& p = mine[wrap(i - 1)];
                const Ring& q = mine[wrap(i + 1)];
                double depth = (q.c[0] - p.c[0]) / (2.0 * dz), side = (q.c[1] - p.c[1]) / (2.0 * dz);
                const double steep = std::sqrt(side * side + depth * depth);
                if (steep > kLean) depth *= kLean / steep, side *= kLean / steep;
                const double length = std::sqrt(depth * depth + side * side + 1.0);
                Ring& r = mine[i];
                r.along[0] = depth / length, r.along[1] = side / length, r.along[2] = 1.0 / length;
                double thick[3] = {w.round ? r.c[0] : 1.0, w.round ? r.c[1] : 0.0, 0.0};
                double dot = thick[0] * r.along[0] + thick[1] * r.along[1];
                for (int c = 0; c < 3; ++c) thick[c] -= r.along[c] * dot;
                double l = std::sqrt(thick[0] * thick[0] + thick[1] * thick[1] + thick[2] * thick[2]);
                if (l < 1e-5) {
                    thick[0] = 1.0 - r.along[0] * r.along[0], thick[1] = -r.along[1] * r.along[0], thick[2] = -r.along[2] * r.along[0];
                    l = std::sqrt(thick[0] * thick[0] + thick[1] * thick[1] + thick[2] * thick[2]);
                }
                for (int c = 0; c < 3; ++c) r.thick[c] = thick[c] / l;
                r.across[0] = r.along[1] * r.thick[2] - r.along[2] * r.thick[1];
                r.across[1] = r.along[2] * r.thick[0] - r.along[0] * r.thick[2];
                r.across[2] = r.along[0] * r.thick[1] - r.along[1] * r.thick[0];
            }
        }
        // How far point p lies out in tube o, in that tube's own size (1: on its plain surface):
        // from the tube's ring whose plane holds p, found from sample g on.
        const auto within = [&](int o, const double* p, double g) {
            const Ring* its = tubes.data() + static_cast<std::size_t>(o) * whole;
            double x = 0.0, y = 0.0;
            for (int turn = 0; turn < 5; ++turn) {
                const double base = std::floor(g), u = g - base;
                const Ring& r0 = its[wrap(static_cast<int>(base))];
                const Ring& r1 = its[wrap(static_cast<int>(base) + 1)];
                double e[3], along = 0.0;
                for (int c = 0; c < 3; ++c) {
                    const double center = c == 2 ? g * dz : r0.c[c] + (r1.c[c] - r0.c[c]) * u;
                    e[c] = p[c] - center;
                    along += e[c] * (r0.along[c] + (r1.along[c] - r0.along[c]) * u);
                }
                if (turn < 4) {
                    const double tangent = r0.along[2] + (r1.along[2] - r0.along[2]) * u;
                    g += std::clamp(along * tangent / dz, -0.5 * per, 0.5 * per);
                    continue;
                }
                for (int c = 0; c < 3; ++c) {
                    x += e[c] * (r0.across[c] + (r1.across[c] - r0.across[c]) * u);
                    y += e[c] * (r0.thick[c] + (r1.thick[c] - r0.thick[c]) * u);
                }
            }
            x /= sa, y /= sb;
            return std::sqrt(x * x + y * y);
        };
        std::vector<double> turnCos(static_cast<std::size_t>(rays)), turnSin(static_cast<std::size_t>(rays));
        for (int j = 0; j < rays; ++j) {
            turnCos[static_cast<std::size_t>(j)] = pm::cos(6.283185307179586 * j / rays);
            turnSin[static_cast<std::size_t>(j)] = pm::sin(6.283185307179586 * j / rays);
        }
        const std::size_t rows = sections * static_cast<std::size_t>(span), around = static_cast<std::size_t>(rays);
        // A ray's limit: how far out along it the point still lies deeper in this tube than in any
        // other, each in its own size (the surface halfway between two tubes, which both end on).
        std::vector<double> limit(rows * around), scale(rows * around), room(rows), work(rows * around);
        parallelFor(0, rows, [&](std::size_t at) {
            const int k = static_cast<int>(at / static_cast<std::size_t>(span)), i = static_cast<int>(at % static_cast<std::size_t>(span));
            const Ring& me = tubes[static_cast<std::size_t>(k) * whole + static_cast<std::size_t>(i)];
            int others[Weave::kMostSections], count = 0;
            for (int o = 0; o < n; ++o) {
                if (o == k) continue;
                const Ring& it = tubes[static_cast<std::size_t>(o) * whole + static_cast<std::size_t>(i)];
                const double dx = (it.c[1] - me.c[1]) / sa, dy = (it.c[0] - me.c[0]) / sb;
                if (dx * dx + dy * dy < (most + 3.0) * (most + 3.0)) others[count++] = o;
            }
            double* lim = limit.data() + at * around;
            double sum = 0.0;
            for (std::size_t j = 0; j < around; ++j) {
                double way[3];
                for (int c = 0; c < 3; ++c) way[c] = me.across[c] * sa * turnCos[j] + me.thick[c] * sb * turnSin[j];
                const auto gap = [&](double s) {
                    const double p[3] = {me.c[0] + s * way[0], me.c[1] + s * way[1], i * dz + s * way[2]};
                    double nearest = std::numeric_limits<double>::infinity();
                    for (int q = 0; q < count; ++q) nearest = std::min(nearest, within(others[q], p, i));
                    return nearest - s;
                };
                double inside = 0.0, outside = most;
                bool met = gap(0.0) < 0.0;
                if (met) {
                    outside = 0.0;
                } else {
                    for (int m = 1; m <= kMarch && !met; ++m) {
                        const double s = most * m / kMarch;
                        if (gap(s) < 0.0) outside = s, met = true;
                        else inside = s;
                    }
                    for (int halve = 0; met && halve < 6; ++halve) {
                        const double mid = 0.5 * (inside + outside);
                        if (gap(mid) < 0.0) outside = mid;
                        else inside = mid;
                    }
                }
                lim[j] = met ? std::max(inside, kLeast) : most;
                sum += lim[j] * lim[j];
            }
            // Free, the tube is as large as lets its ring hold the grown section's area (its hair
            // pressed out of a contact goes to its free sides), no more than kRoom past it.
            double free = most;
            if (sum > grown * grown * static_cast<double>(rays)) {
                double low = kLeast, high = most;
                for (int halve = 0; halve < 24; ++halve) {
                    free = 0.5 * (low + high);
                    double area = 0.0;
                    for (std::size_t j = 0; j < around; ++j) area += std::min(lim[j], free) * std::min(lim[j], free);
                    if (area > grown * grown * static_cast<double>(rays)) high = free;
                    else low = free;
                }
            }
            room[at] = free;
        }, 2);
        // ... eased along the section (a contact's coming and going swells it gently),
        {
            std::vector<double> eased(rows);
            for (int stride = 1; stride <= std::max(1, per / 8); stride *= 2) {
                for (int k = 0; k < n; ++k) {
                    for (int i = 0; i < span; ++i) {
                        eased[static_cast<std::size_t>(k * span + i)] =
                            0.25 * room[rowOf(k, i - stride)] + 0.5 * room[static_cast<std::size_t>(k * span + i)] + 0.25 * room[rowOf(k, i + stride)];
                    }
                }
                room.swap(eased);
            }
        }
        for (std::size_t at = 0; at < rows; ++at) {
            for (std::size_t j = 0; j < around; ++j) scale[at * around + j] = std::min(limit[at * around + j], room[at]);
        }
        // ... and rounded: smoothed round the ring and along the section, never past a limit, so a
        // contact is a flat facet that fades in and out, its edge a rounded corner.
        for (int pass = 0; pass < 2; ++pass) {
            for (std::size_t at = 0; at < rows; ++at) {
                const double* s = scale.data() + at * around;
                double* to = work.data() + at * around;
                for (std::size_t j = 0; j < around; ++j) {
                    to[j] = (s[(j + around - 2) % around] + 4.0 * s[(j + around - 1) % around] + 6.0 * s[j] + 4.0 * s[(j + 1) % around] +
                             s[(j + 2) % around]) / 16.0;
                }
            }
            int turns = 0;
            for (int ahead = 1; ahead <= std::max(1, per / 16); ahead *= 2, ++turns) {
                for (int k = 0; k < n; ++k) {
                    for (int i = 0; i < span; ++i) {
                        const double* before = work.data() + rowOf(k, i - ahead) * around;
                        const double* here = work.data() + static_cast<std::size_t>(k * span + i) * around;
                        const double* after = work.data() + rowOf(k, i + ahead) * around;
                        double* to = scale.data() + static_cast<std::size_t>(k * span + i) * around;
                        for (std::size_t j = 0; j < around; ++j) to[j] = 0.25 * before[j] + 0.5 * here[j] + 0.25 * after[j];
                    }
                }
                work.swap(scale);
            }
            (void)turns;
            for (std::size_t q = 0; q < scale.size(); ++q) scale[q] = std::max(std::min(work[q], limit[q]), kLeast);
        }
        std::array<double, 3> reach{0.0, 0.0, std::numeric_limits<double>::infinity()};
        for (std::size_t at = 0; at < rows; ++at) {
            const int k = static_cast<int>(at / static_cast<std::size_t>(span)), i = static_cast<int>(at % static_cast<std::size_t>(span));
            const Ring& me = tubes[static_cast<std::size_t>(k) * whole + static_cast<std::size_t>(i)];
            for (std::size_t j = 0; j < around; ++j) {
                const double s = scale[at * around + j];
                const double depth = me.c[0] + s * (me.across[0] * sa * turnCos[j] + me.thick[0] * sb * turnSin[j]);
                const double side = me.c[1] + s * (me.across[1] * sa * turnCos[j] + me.thick[1] * sb * turnSin[j]);
                reach = {std::max(reach[0], std::abs(side)), std::max(reach[1], std::abs(depth)), std::min(reach[2], depth)};
            }
            if (!keepIt) continue;
            float* ring = out.rings.data() + at * 8;
            ring[0] = static_cast<float>(me.c[1]), ring[1] = static_cast<float>(me.c[0]);
            for (int c = 0; c < 3; ++c) {
                ring[2 + c] = static_cast<float>(me.across[c] * sa / a);
                ring[5 + c] = static_cast<float>(me.thick[c] * sb / b);
            }
            for (std::size_t j = 0; j < around; ++j) out.scales[at * around + j] = static_cast<float>(scale[at * around + j]);
        }
        return reach;
    };
    // Solved coarsely to find the reach, then at that reach: the tubes' rings stay square to
    // their courses, which scaling a finished braid side to side or in depth would shear.
    const std::array<double, 3> first = solve(1.0, 1.0, kPerStep / 4, kAround / 3, false);
    const double kx = first[0] > 0.0 ? std::clamp(reachPlain[0] / first[0], 0.25, 1.5) : 1.0;
    const double ky = first[1] > 0.0 ? std::clamp(reachPlain[1] / first[1], 0.25, 1.5) : 1.0;
    out.scales.assign(sections * static_cast<std::size_t>(out.span) * kAround, 1.0f);
    out.rings.assign(sections * static_cast<std::size_t>(out.span) * 8, 0.0f);
    const std::array<double, 3> reach = solve(kx, ky, kPerStep, kAround, true);
    for (std::size_t c = 0; c < 2; ++c) {
        out.keep[c] = reach[c] > 0.0 ? static_cast<float>(std::clamp(reachPlain[c] / reach[c], 0.8, 1.25)) : 1.0f;
    }
    out.lowest = static_cast<float>(reach[2]) * out.keep[1];
    return out;
}

// A convex outline opened with a disc: what discs of `radius` cover while lying wholly inside it
// (its corners rounded to the radius, its flat and round sides as they are; never more than it
// was). The outline: `count` points round the origin, (px, py), each `far` out along the unit
// direction (ux, uy); `far` gets how far out the opened outline is along each direction. The
// radius is no more than 0.7 of the origin's distance to the outline's nearest side (a section
// pressed thinner than the radius is rounded by less).
void openOutline(std::size_t count, const double* px, const double* py, const double* ux, const double* uy, double radius,
                 std::vector<double>& work, double* far) {
    work.resize(count * 5);
    double* nx = work.data();
    double* ny = nx + count;
    double* off = ny + count;
    double* cx = off + count;
    double* cy = cx + count;
    double turning = 0.0, nearest = std::numeric_limits<double>::infinity();
    for (std::size_t j = 0; j < count; ++j) {
        const std::size_t to = j + 1 < count ? j + 1 : 0;
        turning += px[j] * py[to] - py[j] * px[to];
    }
    const double side = turning >= 0.0 ? 1.0 : -1.0;
    // Each side's line: its outward normal and how far it lies from the origin.
    for (std::size_t j = 0; j < count; ++j) {
        const std::size_t to = j + 1 < count ? j + 1 : 0;
        const double ex = px[to] - px[j], ey = py[to] - py[j], length = std::sqrt(ex * ex + ey * ey);
        nx[j] = length > 0.0 ? side * ey / length : 0.0;
        ny[j] = length > 0.0 ? -side * ex / length : 0.0;
        off[j] = nx[j] * px[j] + ny[j] * py[j];
        if (length > 0.0) nearest = std::min(nearest, off[j]);
    }
    radius = std::min(radius, 0.7 * nearest);
    if (!(radius > 1e-6)) return;
    // Where the discs' centers may lie (inside every side's line by the radius): its rim along
    // each direction.
    for (std::size_t j = 0; j < count; ++j) {
        double least = std::numeric_limits<double>::infinity();
        for (std::size_t e = 0; e < count; ++e) {
            const double toward = nx[e] * ux[j] + ny[e] * uy[j];
            if (toward > 1e-9) least = std::min(least, (off[e] - radius) / toward);
        }
        least = std::isfinite(least) ? std::max(least, 0.0) : 0.0;
        cx[j] = ux[j] * least, cy[j] = uy[j] * least;
    }
    // A disc slid round that rim, from one direction's center to the next: the farthest it reaches
    // along direction j is at an end, or where its reach stops growing between them.
    const double radius2 = radius * radius;
    for (std::size_t j = 0; j < count; ++j) {
        double most = 0.0;
        for (std::size_t c = 0; c < count; ++c) {
            const std::size_t to = c + 1 < count ? c + 1 : 0;
            const double aside0 = cy[c] * ux[j] - cx[c] * uy[j], aside1 = cy[to] * ux[j] - cx[to] * uy[j];
            if ((aside0 > radius && aside1 > radius) || (aside0 < -radius && aside1 < -radius)) continue;
            const double along0 = cx[c] * ux[j] + cy[c] * uy[j], along1 = cx[to] * ux[j] + cy[to] * uy[j];
            const double run = along1 - along0, drift = aside1 - aside0;
            double stops[3] = {0.0, 1.0, 0.0};
            int tried = 2;
            if (std::abs(drift) > 1e-12) {
                const double slope = run / drift;
                const double at = (radius * slope / std::sqrt(1.0 + slope * slope) - aside0) / drift;
                if (at > 0.0 && at < 1.0) stops[tried++] = at;
            }
            for (int q = 0; q < tried; ++q) {
                const double aside = aside0 + stops[q] * drift;
                if (aside * aside < radius2) most = std::max(most, along0 + stops[q] * run + std::sqrt(radius2 - aside * aside));
            }
        }
        far[j] = std::min(far[j], most);
    }
}

// A fishtail's strips pressed in the braid's cross-section (the plane square to the path, which
// cuts every strip at the same place along the braid): each strip's outline there its cell among
// the strips grown, cut as its course tilts (cutOf), rounded, eased along the braid. Its thin
// fast strips turn too sharply for tubes swept along them (pressSections): the hair of a strip's
// inner side would fold. No rings: Braider::sectionAt lays the hair in that plane.
Pressed pressAcross(const Weave& w, float pancake, float inflate, float tilt) {
    constexpr int kAround = Pressed::kAround, kPerStep = Pressed::kPerStep;
    constexpr double kGrow = 2.0;   // the growth at inflate 1
    constexpr double kRound = 0.6;  // the radius an outline's corners are rounded to, in the sections' own size (openOutline)
    constexpr int kFine = 3;        // the outline is rounded this many times as fine round the ring as it is kept
    constexpr double kAlong = 0.56;  // the most a ray's scale changes along the braid, for the way the fastest section goes meanwhile (below)
    constexpr double kGive = 0.5;   // the most a face shifts toward a section with room to spare: this share of the way to its center
    constexpr double kEager = 2.0;  // how soon it shifts that far: by this times the share of the mean area a section holds over it
    constexpr int kBalance = 4;     // the passes that share the room out
    constexpr double kLeast = 0.05;  // (an outline never closes: two courses through one point)
    Pressed out;
    const int n = w.sections;
    out.sections = n;
    out.count = w.cycle * kPerStep;
    std::vector<int> next;
    out.span = repeatOf(w, next) * kPerStep;
    const int blocks = out.count / out.span;
    out.from.resize(static_cast<std::size_t>(blocks * n));
    for (int k = 0; k < n; ++k) out.from[static_cast<std::size_t>(k)] = static_cast<std::uint8_t>(k);
    for (int block = 1; block < blocks; ++block) {
        for (int k = 0; k < n; ++k) {
            out.from[static_cast<std::size_t>(block * n + next[static_cast<std::size_t>(k)])] = out.from[static_cast<std::size_t>((block - 1) * n + k)];
        }
    }
    const std::size_t span = static_cast<std::size_t>(out.span), sections = static_cast<std::size_t>(n);
    out.scales.assign(sections * span * kAround, 1.0f);
    const double loose = 1.0 + 0.6 * pancake;
    const double a = w.halfWidth * loose, b = w.halfThick * loose;
    double turnCos[kAround], turnSin[kAround];
    for (int j = 0; j < kAround; ++j) {
        turnCos[j] = pm::cos(6.283185307179586 * j / kAround);
        turnSin[j] = pm::sin(6.283185307179586 * j / kAround);
    }
    double fineCos[kFine * kAround], fineSin[kFine * kAround];  // (every kFine-th: the rays kept)
    for (int j = 0; j < kFine * kAround; ++j) {
        fineCos[j] = pm::cos(6.283185307179586 * j / (kFine * kAround));
        fineSin[j] = pm::sin(6.283185307179586 * j / (kFine * kAround));
    }
    // The sections as cut, in their own size (side to side in half widths, in depth in half
    // thicknesses: there a section as patterned is about a unit circle): the center, and the cut
    // ellipse's half axes.
    struct Laid {
        double x, y, acrossX, acrossY, thickX, thickY;
    };
    std::vector<Laid> laid(span * sections);
    // Per sample: the plain ellipses' reach side to side and in depth (braid widths), and each
    // section's distance to its nearest, summed.
    std::vector<std::array<double, 3>> plain(span);
    parallelFor(0, span, [&](std::size_t i) {
        Laid* at = laid.data() + i * sections;
        std::array<double, 3>& sums = plain[i];
        sums = {0.0, 0.0, 0.0};
        // (square roots, not hypot: the same bits on every platform)
        auto far = [](double x, double y) { return std::sqrt(x * x + y * y); };
        for (int k = 0; k < n; ++k) {
            const SectionCut cut = cutOf(w, k, static_cast<float>(i) / static_cast<float>(kPerStep), pancake, tilt);
            at[k] = {cut.center[0] / a, cut.center[1] / b, cut.across[0], cut.across[1] * a / b, cut.thick[0] * b / a, cut.thick[1]};
            sums[0] = std::max(sums[0], std::abs(static_cast<double>(cut.center[0])) + far(cut.acrossFlat[0] * a, cut.thickFlat[0] * b));
            sums[1] = std::max(sums[1], std::abs(static_cast<double>(cut.center[1])) + far(cut.acrossFlat[1] * a, cut.thickFlat[1] * b));
        }
        for (int k = 0; k < n; ++k) {
            double nearest = std::numeric_limits<double>::infinity();
            for (int o = 0; o < n; ++o) {
                if (o != k) nearest = std::min(nearest, far(at[o].x - at[k].x, at[o].y - at[k].y));
            }
            sums[2] += nearest;
        }
    }, 1);
    std::array<double, 2> reachPlain{0.0, 0.0};
    double near = 0.0;
    for (const std::array<double, 3>& sums : plain) {
        reachPlain = {std::max(reachPlain[0], sums[0]), std::max(reachPlain[1], sums[1])};
        near += sums[2];
    }
    if (w.plain) {  // (patterned anew: the reach of version 1's weave, so the braid is as wide and deep as it was)
        reachPlain = {0.0, 0.0};
        const Weave& was = *w.plain;
        for (int k = 0; k < was.sections; ++k) {
            for (int i = 0; i < was.cycle * kPerStep; ++i) {
                const SectionCut cut = cutOf(was, k, static_cast<float>(i) / static_cast<float>(kPerStep), pancake, tilt);
                const double across = std::sqrt(cut.acrossFlat[0] * a * cut.acrossFlat[0] * a + cut.thickFlat[0] * b * cut.thickFlat[0] * b);
                const double deep = std::sqrt(cut.acrossFlat[1] * a * cut.acrossFlat[1] * a + cut.thickFlat[1] * b * cut.thickFlat[1] * b);
                reachPlain = {std::max(reachPlain[0], std::abs(static_cast<double>(cut.center[0])) + across),
                              std::max(reachPlain[1], std::abs(static_cast<double>(cut.center[1])) + deep)};
            }
        }
    }
    // Grown: from where the sections touch at all (two a distance d apart touch at half of it).
    const double touch = 0.5 * near / static_cast<double>(span * sections);
    const double grown = std::max(touch, 1.0) * (1.0 + (kGrow - 1.0) * std::clamp(static_cast<double>(inflate), 0.0, kMostInflate));
    out.grown = static_cast<float>(grown);
    out.inflated = static_cast<float>(1.0 + (kGrow - 1.0) * std::clamp(static_cast<double>(inflate), 0.0, kMostInflate));
    // (The limits are summed as 1 / limit to the tenth power: `tenth`.)
    const auto tenth = [](double r) {
        const double r2 = r * r, r4 = r2 * r2;
        return r4 * r4 * r2;
    };
    const double capped = tenth(1.0 / grown);

    std::vector<std::array<double, 3>> reached(span);  // per sample: the outlines' reach side to side, in depth, their lowest
    parallelFor(0, span, [&](std::size_t i) {
        const Laid* at = laid.data() + i * sections;
        std::vector<double> rays(sections * kAround * 2), scale(sections * kAround), between(sections * sections * 3);
        double least = std::numeric_limits<double>::infinity();
        for (int k = 0; k < n; ++k) {
            for (int j = 0; j < kAround; ++j) {
                double* ray = rays.data() + (static_cast<std::size_t>(k) * kAround + static_cast<std::size_t>(j)) * 2;
                ray[0] = at[k].acrossX * turnCos[j] + at[k].thickX * turnSin[j];
                ray[1] = at[k].acrossY * turnCos[j] + at[k].thickY * turnSin[j];
            }
            for (int o = 0; o < n; ++o) {
                double* d = between.data() + (static_cast<std::size_t>(k) * sections + static_cast<std::size_t>(o)) * 3;
                d[0] = at[o].x - at[k].x;
                d[1] = at[o].y - at[k].y;
                d[2] = d[0] * d[0] + d[1] * d[1];
                if (o != k) least = std::min(least, d[2]);
            }
        }
        const double most = 0.5 * kGive * least;
        std::array<double, Weave::kMostSections> weight{}, area{};
        for (int pass = 0;; ++pass) {
            for (int k = 0; k < n; ++k) {
                const double* d = between.data() + static_cast<std::size_t>(k) * sections * 3;
                for (int j = 0; j < kAround; ++j) {
                    const double* ray = rays.data() + (static_cast<std::size_t>(k) * kAround + static_cast<std::size_t>(j)) * 2;
                    double sum = capped;
                    for (int o = 0; o < n; ++o) {
                        if (o == k) continue;
                        // Along the ray the face shared with section o is met at (d.d + its weight
                        // less o's) / (2 ray.d), if the ray heads its way.
                        const double toward = 2.0 * (ray[0] * d[o * 3] + ray[1] * d[o * 3 + 1]);
                        if (!(toward > 0.0)) continue;
                        const double r = toward / std::max(d[o * 3 + 2] + weight[static_cast<std::size_t>(k)] - weight[static_cast<std::size_t>(o)], 1e-9);
                        sum += tenth(r);
                    }
                    scale[static_cast<std::size_t>(k) * kAround + static_cast<std::size_t>(j)] = std::max(pm::pow(sum, -0.1), kLeast);
                }
            }
            if (pass == kBalance || n < 2) break;
            // Each outline's area (in the sections' own size), and the room shared out.
            double mean = 0.0;
            for (int k = 0; k < n; ++k) {
                const double* ray = rays.data() + static_cast<std::size_t>(k) * kAround * 2;
                const double* s = scale.data() + static_cast<std::size_t>(k) * kAround;
                double twice = 0.0;
                for (int j = 0; j < kAround; ++j) {
                    const int to = j + 1 < kAround ? j + 1 : 0;
                    twice += s[j] * s[to] * (ray[j * 2] * ray[to * 2 + 1] - ray[j * 2 + 1] * ray[to * 2]);
                }
                area[static_cast<std::size_t>(k)] = 0.5 * std::abs(twice);
                mean += area[static_cast<std::size_t>(k)];
            }
            mean /= static_cast<double>(n);
            // (Each weight a smooth, bounded function of the areas, which the weights move little:
            // the passes settle, and along the braid the faces shift as smoothly as the areas change.)
            for (int k = 0; k < n; ++k) {
                weight[static_cast<std::size_t>(k)] = most * pm::tanh(kEager * (mean - area[static_cast<std::size_t>(k)]) / mean);
            }
        }
        // Rounded. As pressed an outline has a corner where a face ends and the section's free side
        // begins (and where two faces meet), and as a face sweeps on along the braid that corner
        // runs over the lock as a ledge: hair does not fold so. Each outline is opened with a disc
        // (openOutline) of radius kRound, in the sections' own size: every corner rounded to the
        // radius, the faces and the round sides left as they are, nothing added (so nothing
        // overlaps for it). What it takes is the ends of a contact: the groove between two locks.
        // (On the outline worked out kFine times as fine round the ring: a corner sits between
        // the rays kept, and opened as kept the rounding would step as it passes from one to the
        // next along the braid.)
        std::vector<double> work, fine(static_cast<std::size_t>(kFine * kAround) * 5);
        for (int k = 0; k < n; ++k) {
            float* row = out.scales.data() + (static_cast<std::size_t>(k) * span + i) * kAround;
            const double* own = scale.data() + static_cast<std::size_t>(k) * kAround;
            const std::size_t count = static_cast<std::size_t>(kFine * kAround);
            double* px = fine.data();
            double* py = px + count;
            double* ux = py + count;
            double* uy = ux + count;
            double* far = uy + count;
            const double* d = between.data() + static_cast<std::size_t>(k) * sections * 3;
            for (std::size_t f = 0; f < count; ++f) {
                const double x = at[k].acrossX * fineCos[f] + at[k].thickX * fineSin[f];
                const double y = at[k].acrossY * fineCos[f] + at[k].thickY * fineSin[f];
                double sum = capped;
                for (int o = 0; o < n; ++o) {
                    if (o == k) continue;
                    const double toward = 2.0 * (x * d[o * 3] + y * d[o * 3 + 1]);
                    if (!(toward > 0.0)) continue;
                    const double r = toward / std::max(d[o * 3 + 2] + weight[static_cast<std::size_t>(k)] - weight[static_cast<std::size_t>(o)], 1e-9);
                    sum += tenth(r);
                }
                const double length = std::sqrt(x * x + y * y);
                const double s = std::max(pm::pow(sum, -0.1), kLeast);
                ux[f] = x / length, uy[f] = y / length;
                far[f] = s * length;
                px[f] = x * s, py[f] = y * s;
            }
            openOutline(count, px, py, ux, uy, kRound, work, far);
            for (int j = 0; j < kAround; ++j) {
                const std::size_t f = static_cast<std::size_t>(j * kFine);
                const double length = std::sqrt(px[f] * px[f] + py[f] * py[f]) / std::max(own[j], kLeast);  // (the ray's own length)
                row[j] = static_cast<float>(std::max(std::min(own[j], far[f] / length), kLeast));
            }
        }
    }, 1);
    // Eased along the braid. A face turns as its two sections pass each other, and far from its
    // middle it sweeps over the rays faster than hair gives way: a ledge along the lock. No ray's
    // scale may change faster along the braid than kAlong times as fast as the weave's fastest
    // section goes across (in the sections' own size: a three-strand's 1.4 a weave step, twice
    // that where a section crosses two slots a step, a fishtail's strips hardly held): each is
    // held under every scale near it plus that rate times how far off it is. Never more than it was (nothing overlaps for it), and a scale that changes slower,
    // the middle of a contact, stays as it was; what gives is a contact's fast end and the free
    // side just before a face arrives and after it has gone.
    {
        const auto row = [&](const std::vector<float>& rows, std::size_t k, int sample) {  // (Pressed::row, in `rows`)
            sample = ((sample % out.count) + out.count) % out.count;
            const int block = sample / out.span;
            const std::size_t source = out.from[static_cast<std::size_t>(block * n) + k];
            return rows.data() + (source * span + static_cast<std::size_t>(sample - block * out.span)) * kAround;
        };
        double fastest = 0.0;  // a sample on, in the sections' own size
        for (std::size_t i = 0; i + 1 < span; ++i) {
            for (std::size_t k = 0; k < sections; ++k) {
                const Laid& here = laid[i * sections + k];
                const Laid& on = laid[(i + 1) * sections + k];
                fastest = std::max(fastest, std::sqrt((on.x - here.x) * (on.x - here.x) + (on.y - here.y) * (on.y - here.y)));
            }
        }
        const float rate = static_cast<float>(std::max(kAlong * fastest, 1e-6));
        const int reach = std::min(static_cast<int>(grown / rate) + 1, out.count / 2);
        std::vector<float> pressed;
        const auto ease = [&] {
            pressed = out.scales;
            parallelFor(0, sections * span, [&](std::size_t index) {
                const std::size_t k = index / span;
                const int i = static_cast<int>(index % span);
                float* to = out.scales.data() + index * kAround;
                for (int step = -reach; step <= reach; ++step) {
                    if (step == 0) continue;
                    const float* from = row(pressed, k, i + step);
                    const float rise = rate * static_cast<float>(step < 0 ? -step : step);
                    for (int j = 0; j < kAround; ++j) to[j] = std::min(to[j], from[j] + rise);
                }
            }, 8);
        };
        ease();
        // (Held back ray by ray, an outline has corners again where one ray is held and the next
        // is not: opened once more, as kept, and eased again, which takes the little steps that
        // leaves along the braid.)
        {
            parallelFor(0, sections * span, [&](std::size_t index) {
                const std::size_t k = index / span, i = index % span;
                const Laid& here = laid[i * sections + k];
                float* scales = out.scales.data() + index * kAround;
                double px[kAround], py[kAround], ux[kAround], uy[kAround], far[kAround], length[kAround];
                for (int j = 0; j < kAround; ++j) {
                    const double x = here.acrossX * turnCos[j] + here.thickX * turnSin[j];
                    const double y = here.acrossY * turnCos[j] + here.thickY * turnSin[j];
                    length[j] = std::sqrt(x * x + y * y);
                    ux[j] = x / length[j], uy[j] = y / length[j];
                    far[j] = scales[j] * length[j];
                    px[j] = x * scales[j], py[j] = y * scales[j];
                }
                std::vector<double> work;
                openOutline(kAround, px, py, ux, uy, kRound, work, far);
                for (int j = 0; j < kAround; ++j) scales[j] = std::max(std::min(scales[j], static_cast<float>(far[j] / length[j])), static_cast<float>(kLeast));
            }, 4);
            ease();
        }
    }
    parallelFor(0, span, [&](std::size_t i) {
        const Laid* at = laid.data() + i * sections;
        std::array<double, 3>& reach = reached[i];
        reach = {0.0, 0.0, std::numeric_limits<double>::infinity()};
        for (int k = 0; k < n; ++k) {
            const float* row = out.scales.data() + (static_cast<std::size_t>(k) * span + i) * kAround;
            for (int j = 0; j < kAround; ++j) {
                const double s = row[j];
                const double rayX = at[k].acrossX * turnCos[j] + at[k].thickX * turnSin[j];
                const double rayY = at[k].acrossY * turnCos[j] + at[k].thickY * turnSin[j];
                const double x = (at[k].x + s * rayX) * a, y = (at[k].y + s * rayY) * b;
                reach = {std::max(reach[0], std::abs(x)), std::max(reach[1], std::abs(y)), std::min(reach[2], y)};
            }
        }
    }, 4);
    std::array<double, 3> reach{0.0, 0.0, std::numeric_limits<double>::infinity()};
    for (const std::array<double, 3>& r : reached) reach = {std::max(reach[0], r[0]), std::max(reach[1], r[1]), std::min(reach[2], r[2])};
    for (std::size_t c = 0; c < 2; ++c) {
        out.keep[c] = reach[c] > 0.0 ? static_cast<float>(std::clamp(reachPlain[c] / reach[c], 0.25, 1.5)) : 1.0f;
    }
    out.lowest = static_cast<float>(reach[2]) * out.keep[1];
    return out;
}

// pressSections' results kept for the process (process_caches.h): a weave's is the same whenever
// it is asked again (an evaluation after another param's edit, the next frame, the next Braid node
// of the same style), and `separate` only blends it in.
// `across`: in the braid's cross-section (pressAcross).
std::shared_ptr<const Pressed> pressedFor(const Weave& w, float pancake, float inflate, float tilt, bool across) {
    struct Kept {
        std::mutex mutex;
        std::vector<std::pair<std::uint64_t, std::shared_ptr<const Pressed>>> solved;  // the last asked first
    };
    static Kept kept;
    static const bool registered = (registerProcessCache(
                                        "braid sections",
                                        [] {
                                            const std::lock_guard<std::mutex> lock(kept.mutex);
                                            kept.solved.clear();
                                        },
                                        [] {
                                            const std::lock_guard<std::mutex> lock(kept.mutex);
                                            std::size_t bytes = 0;
                                            for (const auto& entry : kept.solved) {
                                                bytes += (entry.second->scales.size() + entry.second->contact.size()) * sizeof(float);
                                            }
                                            return bytes;
                                        }),
                                    true);
    (void)registered;
    auto bits = [](float v) {
        std::uint32_t u = 0;
        std::memcpy(&u, &v, sizeof(u));
        return static_cast<std::uint64_t>(u);
    };
    std::uint64_t key = hashCombine(static_cast<std::uint64_t>(w.sections) * 4 + (w.round ? 1 : 0), static_cast<std::uint64_t>(w.cycle));
    for (const float v : {w.halfWidth, w.halfThick, pancake, inflate, tilt}) key = hashCombine(key, bits(v));
    key = hashCombine(key, (w.plain ? 1u : 0u) + (across ? 2u : 0u));
    for (const auto& t : w.tracks) {
        for (const auto& c : t) key = hashCombine(key, bits(c[0]) << 32 | bits(c[1]));
    }
    {
        const std::lock_guard<std::mutex> lock(kept.mutex);
        for (std::size_t k = 0; k < kept.solved.size(); ++k) {
            if (kept.solved[k].first != key) continue;
            std::rotate(kept.solved.begin(), kept.solved.begin() + static_cast<std::ptrdiff_t>(k), kept.solved.begin() + static_cast<std::ptrdiff_t>(k) + 1);
            return kept.solved.front().second;
        }
    }
    const ProfileScope profile("braid.press");
    Pressed made = across ? pressAcross(w, pancake, inflate, tilt) : pressSections(w, pancake, inflate, tilt);
    markContacts(made);
    auto solved = std::make_shared<const Pressed>(std::move(made));
    const std::lock_guard<std::mutex> lock(kept.mutex);
    kept.solved.insert(kept.solved.begin(), {key, solved});
    if (kept.solved.size() > 16) kept.solved.pop_back();
    return solved;
}

struct BraidPath {
    std::vector<Vec3f> points;  // resampled evenly
    std::vector<Frame> frames;
    float length = 0.0f;
    Vec3f root;
    StrandId id = 0;           // the guide's (a picked strand's) ID
    StrandRoot guideRoot;      // and its root
    std::vector<float> steps;  // weave steps travelled at each point
    // End free (or tailSmoothness): how the tail's one bundle is turned (Braider::layBundle):
    // section k's sector lies at bundleTurn + bundleSign x k of a turn's n-th round the path. And
    // gatherSmoothness: the bundle the weave starts from, turned so (startTurn, startSign).
    float bundleTurn = 0.0f, bundleSign = 1.0f;
    float startTurn = 0.0f, startSign = 1.0f;
    // gatherSmoothness, tailSmoothness: where the weave has faded in (from `weaveFrom` on) and from
    // where it fades out (to the weave's end), along the path (Braider::bundledAt).
    float weaveFrom = 0.0f, fadedIn = 0.0f, fadeOut = 0.0f;
    std::vector<float> rate;   // steps a unit of length at each point
    float repeats = 0.0f;      // weave steps along the whole path
    std::vector<float> grown;  // feed: the width's factor at each point (empty: 1)
    // Version 2: each point's height above the scalp along its normal (negative: below it, down to a
    // braid width; empty: no scalp), so the braid is lifted only as far as keeps it out of the scalp
    // (Braider::liftAt).
    std::vector<float> overScalp;
    std::vector<float> ties;   // where each tie's middle is along the path, the last first
    float tieHalf = 0.0f;      // half a tie's length
    std::vector<Vec3f> tieFrom, tieDirection;  // each tie's straight axis (Braider::layTieAxes)

    // Interpolated along the path at distance `at` (clamped), from a value per point.
    float sample(const std::vector<float>& v, float at) const {
        const std::size_t last = points.size() - 1;
        const float f = std::clamp(at / length, 0.0f, 1.0f) * static_cast<float>(last);
        const std::size_t k = std::min(static_cast<std::size_t>(f), last - 1);
        const float w = f - static_cast<float>(k);
        return v[k] * (1.0f - w) + v[k + 1] * w;
    }
    // Weave steps travelled `at` a distance along the path (past its end: at the end's rate).
    float weaveAt(float at) const {
        if (at >= length) return steps.back() + (at - length) * rate.back();
        return sample(steps, std::max(at, 0.0f));
    }
    float grownAt(float at) const { return grown.empty() ? 1.0f : sample(grown, at); }
    // The distance along the path where the weave has gone `s` steps (past its end: at the end's rate).
    float distanceAt(float s) const {
        if (s >= steps.back()) return length + (s - steps.back()) / std::max(rate.back(), 1e-6f);
        if (s <= 0.0f) return 0.0f;
        const std::size_t k = static_cast<std::size_t>(std::upper_bound(steps.begin(), steps.end(), s) - steps.begin()) - 1;
        const float span = steps[k + 1] - steps[k];
        const float f = span > 0.0f ? (s - steps[k]) / span : 0.0f;
        return (static_cast<float>(k) + f) * length / static_cast<float>(points.size() - 1);
    }
};

// The gathering stretch pressed over the scalp: a point on its way into the
// braid (blend strictly between 0 and 1) is held `clearance` above the scalp,
// rising to where the braid has it as the blend completes; a point that would
// cut through the head is lifted out. The clearance comes in over the first
// few clearances from the root (`fromRoot`, the distance there) and goes again
// over the last few before the braid (`toBraid`), so the hair leaves its root
// along the scalp rather than straight out of it, and runs into a braid lying
// lower than the clearance without dropping into it. Only these points query
// the scalp.
struct ScalpGather {
    const ScalpMesh* scalp = nullptr;  // null: straight lines
    float clearance = 0.0f;

    void apply(Vec3f& p, float blend, float fromRoot,
               float toBraid = std::numeric_limits<float>::infinity()) const {
        if (!scalp || !(blend > 0.0f) || !(blend < 1.0f)) return;
        const SurfacePoint sp = scalp->closestPoint(p);
        if (sp.root.face < 0) return;
        const float height = (p - sp.position).dot(sp.normal);
        const float ramp = 4.0f * clearance;
        const float above = clearance * smoothstep(0.0f, ramp, fromRoot) * smoothstep(0.0f, ramp, toBraid);
        const float held = above + std::max(height - above, 0.0f) * blend;
        p = p + sp.normal * (held - height);
    }
};

// Per-strand output channels for nodes downstream.
struct BraidChannels {
    std::vector<float>* id;    // the path braided into, -1 when loose
    std::vector<float>* flat;  // 1 when the braid lies on the scalp (cornrow)
    std::vector<float>* tail = nullptr;  // per point: 0 in the braid, rising to 1 down its tail
    std::vector<float>* section = nullptr;  // the section a strand runs in, -1 when loose
    std::vector<float>* woven = nullptr;    // per point: 0 on the way in, rising to 1 in the braid
    std::vector<float>* press = nullptr;    // per point: how much it lies against another section (pressAt)
    int first = 0;             // this node's ids start here (after an earlier Braid node's)
    // Paths of its own: each path's number, from its strand's ID (stableGroupNumbers), so an edit
    // of the scalp in one place renumbers no braid elsewhere; null: the path's index (the guides').
    const std::vector<float>* numbers = nullptr;
    float number(int path) const {
        return static_cast<float>(first) + (numbers ? (*numbers)[static_cast<std::size_t>(path)] : static_cast<float>(path));
    }
    void set(std::size_t s, int path, Style style) const {
        (*id)[s] = number(path);
        (*flat)[s] = style == Style::Cornrow ? 1.0f : 0.0f;
    }
};

// How far out of its section's disc a strand's spot reaches at most (StrandDraws::inLobe).
constexpr float kLobeReach = 0.92f;

// A strand's own draws, by its ID: a section for a root with no side, its spot within its section
// (in the unit disc), how its end fans out.
struct StrandDraws {
    int lobe = 0;
    int section = -1;  // the section it goes in, given (its clump's: ClumpPlace); -1: the one on its side
    Vec3f inLobe;
    float fanAngle = 0.0f;
    float endDraw = 0.0f;  // where in the tail a fed strand ends
    float fan = 0.9f;      // the fan-out's reach, in braid widths

    StrandDraws() = default;
    StrandDraws(std::uint64_t id, std::uint64_t seed, int lobes) {
        Rng rng(hashCombine(id, seed ^ 0xB4A1D5ull));
        // (Draws that strays and fuzz used, still drawn: every strand keeps the spot it had.)
        rng.nextFloat();
        lobe = static_cast<int>(rng.nextU64() % static_cast<std::uint64_t>(lobes));
        const float a = kTwoPi * rng.nextFloat();
        const float r = kLobeReach * std::sqrt(rng.nextFloat());
        inLobe = {r * pm::cos(a), r * pm::sin(a), 0.0f};
        fanAngle = kTwoPi * rng.nextFloat();
        rng.nextFloat();
        endDraw = rng.nextFloat();
    }
};

// Clumps kept through the braid (clumpChannel): a clump's place, the same for each of its strands.
// It goes in the section its roots lie on (their mean), at one spot of that section's disc (by its
// number), its strands round that spot within the share of the disc its hair takes (the square root
// of its share of the section's strands) times how tight it came in (its strands' spread halfway
// along against their roots': a tight clump stays a thin lock, a loose one spreads), each at its
// own spot scaled into it.
struct ClumpPlace {
    int section = 0;
    Vec3f spot;          // its middle in the section's unit disc (within kLobeReach)
    float share = 1.0f;  // the radius its strands take, a share of the disc's
    // A strand's draws as a member: its section, its spot moved into the clump's.
    void place(StrandDraws& d) const {
        d.section = section;
        d.inLobe = spot + d.inLobe * share;
    }
};
using ClumpPlaces = std::unordered_map<std::int64_t, ClumpPlace>;

// A clump's number from its channel's value (a whole number; none for a negative or non-finite one).
bool clumpKey(float v, std::int64_t* key) {
    if (!std::isfinite(v) || v < 0.0f) return false;
    *key = static_cast<std::int64_t>(std::floor(v));
    return true;
}

// The points a braided strand gets: `perStep` a weave step over the share of it that weaves, at
// most `most` segments, never fewer than it came with.
struct Sampling {
    float perStep = 4.0f;
    std::size_t most = 1024;

    std::size_t count(std::size_t n, float steps, float share) const {
        if (n < 2 || !(steps > 0.0f)) return n;
        const float want = std::ceil(perStep * steps / std::max(share, 1e-3f));
        const auto segments = static_cast<std::size_t>(std::min(want, static_cast<float>(most)));
        return std::max(n, segments + 1);
    }
};

// Where a fed strand joins its braid and how it runs along it (planFed).
struct FedPlan {
    int path = -1;            // -1: stays as it is
    std::size_t joinK = 0;    // the path point it joins at
    float joinAt = 0.0f;      // that distance along the path
    float approach = 0.0f;    // the distance along the strand it reaches the braid at (from its root, straight)
    float squeeze = 1.0f;     // its length fitted to the path
    bool region = false;      // joined from the root region (not picked up along the path)
    float strandLength = 0.0f;
    StrandDraws draws;
};

// A braid's point a distance along its path, as far as the path alone gives it: the same for every
// section and strand there (Braider::stationAt).
struct PathStation {
    Vec3f center;                  // the path's point
    const Frame* frame = nullptr;  // and its frame
    float u = 0.0f;                // the share of the path's length gone (at most 1)
    float wid = 0.0f;              // the braid's width
    float letGo = 0.0f;            // how far down its tail (tailAt)
    float bundled = 0.0f;          // how much the sections lie as one round bundle (bundledAt)
    float bundleTurn = 0.0f, bundleSign = 1.0f;  // that bundle's (the path's start or tail: BraidPath)
    float rate = 0.0f, steps = 0.0f, lift = 0.0f;  // the weave's steps a unit of length, those gone; raised by
    // The nearest tie's hold (braidedFrom): the hair kept within `most` of `tieAxis` (in the
    // frame), let go as `release` rises to 1 (1: no tie holds here).
    float release = 1.0f, most = 0.0f;
    Vec3f tieAxis;
    // Pressed sections (version 2): where in the cycle's samples the outlines are read, and how far
    // on to the next sample.
    int sample = 0;
    float between = 0.0f;
};

// A section's band at a station, the same for every strand in it (Braider::sectionAt): its center
// off the path and the directions across the band and through it, in the path's frame (unit
// vectors). Pressed (version 2): the section as the braid's cross-section cuts it (the two
// directions as long as the cut makes them, in that plane), and its outline at the two samples of
// the cycle the station lies between (Pressed::row).
struct SectionStation {
    Vec3f center, across, thick;
    const float* rows[2] = {nullptr, nullptr};
};

// A strand's own part of where it lies, the same at every point of it (Braider::spotOf): its spot
// across its section's band and through it (braid widths), how its end fans out.
struct StrandSpot {
    float across = 0.0f, thick = 0.0f;
    float fanDepth = 0.0f, fanSide = 0.0f;
    float ray = 0.0f;  // pressed sections: the outline's ray the spot lies along, in [0, Pressed::kAround)
    float out = 0.0f;  // how far out of its section's middle the spot lies (1: at its rim)
    // A strand's (spotOf): with end free it takes its place in the tail's one bundle. Not a
    // section's center line or a tube's rim point: those run onto the path where the sections end.
    bool inBundle = false;
};

// A path's stations for the strands of `count` points spread evenly along it (the points a braided
// strand is given, Braider::tableFor): worked out once a path, read by every strand braided along
// it, each point of which would work out the same values.
struct PathTable {
    std::size_t count = 0;
    std::vector<PathStation> stations;     // [point]
    std::vector<SectionStation> sections;  // [section * count + point]
    std::vector<float> blend;              // [point]: from the root into the braid (the gather)
};

// The braid's settings and paths: where each strand joins and where its points go.
struct Braider {
    Style style = Style::Three;
    Weave weave;
    float width = 0.0f, tightness = 1.0f, roll = 0.0f;
    Ramp taper;
    std::vector<float> taperTangents;  // the taper's (Ramp::tangents), for widthAt at every point
    float region2 = 0.0f, gather = 0.0f, tail = 0.0f;
    // How gradually the weave starts after the gather and ends before the tail (0-1: from and into
    // one round bundle over up to kFadeSteps weave steps: bundledAt, fadePeak, fadeSteps).
    float gatherSmoothness = 0.0f, tailSmoothness = 0.0f;
    static constexpr float kFadeSteps = 4.0f;
    bool feeds = false;  // feed: the braid starts at the path's root (no gather)
    std::uint64_t seed = 0;
    Sampling sampling;
    ScalpGather overScalp;
    float feedUntil = 0.0f;
    // Version 1, fed along the scalp: raised by (braid widths) so its lowest section lies on the
    // scalp, easing back to the path's middle where the pick-up ends (it leaves the head there).
    float feedLift = 0.0f;
    // Version 2, fed or not: how far the braid's underside lies below its path (braid widths). It
    // is raised only as far as keeps that underside out of the scalp (the scalp collides with it:
    // liftAt), so a braid drawn on the scalp lies on it and one off the head stays on its guide.
    float clearLift = 0.0f;
    End end = End::Fan;
    float release = 1.5f;  // end free: braid widths the sections take to let go (releaseLength)
    float pancake = 0.0f, loops = 0.0f, loopSize = 0.5f;
    // Version 2: the sections pressed against each other (press), null: plain ellipses. `separate`
    // blends the outlines in, `keep` is the cross-section's scale with them (Pressed::keep, blended),
    // `tilt` the weave's steps a braid width (cutOf).
    std::shared_ptr<const Pressed> pressed;
    float separate = 0.0f, tilt = 0.0f;
    std::array<float, 2> keep{1.0f, 1.0f};
    int ties = 0;
    float tieLength = 0.5f, tieSpacing = 0.0f, cinch = 0.0f;
    std::vector<BraidPath> paths;
    // Each path's own settings where a param is mapped (BraidOp::run: read at the path's root), the
    // paths themselves kept here alone; empty: these for every path.
    std::vector<std::shared_ptr<const Braider>> perPath;
    // The settings path b is braided with.
    const Braider& of(std::size_t b) const { return perPath.empty() ? *this : *perPath[b]; }

    // The sections pressed against each other (a version 2 node's: `weave` is patterned for it,
    // tightness is set): `separate` of the way from plain ellipses to their outlines. A braid lying
    // on the scalp (a cornrow; any other where the scalp collides with it) is raised to where its
    // pressed underside is.
    void press(float inflate, float share) {
        if (weave.sections < 2) return;
        tilt = tightness / std::max(weave.stepLength, 1e-6f);
        pressed = pressedFor(weave, pancake, inflate, tilt, style == Style::Fishtail);
        separate = std::clamp(share, 0.0f, 1.0f);
        keep = {1.0f + (pressed->keep[0] - 1.0f) * separate, 1.0f + (pressed->keep[1] - 1.0f) * separate};
        if (style == Style::Cornrow) weave.lift += (-pressed->lowest - weave.lift) * separate;
        else clearLift += (-pressed->lowest - clearLift) * separate;
        // Twice plain sections' four points a step for a four- and a five-strand braid (their
        // sections go over and under every two and a half steps or so) and a rope (a ply's turn is
        // few steps long); the others' four.
        sampling.perStep = style == Style::Four || style == Style::Five || style == Style::Rope ? 8.0f : 4.0f;
    }
    // Where a station lies in the pressed outlines' samples (stationAt).
    void sampleAt(PathStation& st) const {
        const float count = static_cast<float>(pressed->count);
        float f = st.steps * static_cast<float>(Pressed::kPerStep);
        f -= std::floor(f / count) * count;
        st.sample = std::clamp(static_cast<int>(f), 0, pressed->count - 1);
        st.between = f - static_cast<float>(st.sample);
    }
    // A pressed section's outline along `ray` at a station between two samples (`rows`: its
    // outlines there): the scale that takes a spot of the plain ellipse to its place. `separate` of
    // the way from the plain ellipse grown by `inflate` alone (Pressed::inflated: fat sections that
    // pass through each other) to the pressed outline, so inflate works at any separate.
    float outlineAt(const float* const rows[2], float between, float ray) const {
        const int j = std::min(static_cast<int>(ray), Pressed::kAround - 1), next = j + 1 < Pressed::kAround ? j + 1 : 0;
        const float u = ray - static_cast<float>(j);
        const float here = rows[0][j] + (rows[0][next] - rows[0][j]) * u;
        const float there = rows[1][j] + (rows[1][next] - rows[1][j]) * u;
        const float plain = pressed->inflated;
        return plain + (here + (there - here) * between - plain) * separate;
    }
    // How much a strand at `spot` lies against another section there (braidPress): its outline's
    // contact along its ray (Pressed::contact, between two samples as outlineAt), more the farther
    // out of its section's middle the spot lies (a contact's facet is the section's rim: the hair
    // in its middle lies against none), `separate` of it (sections that pass through each other
    // press nothing).
    float pressAt(const float* const rows[2], float between, const StrandSpot& spot) const {
        if (pressed->contact.empty()) return 0.0f;
        const int j = std::min(static_cast<int>(spot.ray), Pressed::kAround - 1), next = j + 1 < Pressed::kAround ? j + 1 : 0;
        const float u = spot.ray - static_cast<float>(j);
        const float* c0 = pressed->contactOf(rows[0]);
        const float* c1 = pressed->contactOf(rows[1]);
        const float here = c0[j] + (c0[next] - c0[j]) * u;
        const float there = c1[j] + (c1[next] - c1[j]) * u;
        return (here + (there - here) * between) * smoothstep(0.2f, 0.9f, spot.out) * separate;
    }

    float widthAt(const BraidPath& path, float at) const {
        return width * std::max(taper.at(std::clamp(at / path.length, 0.0f, 1.0f), taperTangents.data()), 0.0f) * path.grownAt(at);
    }
    // Where the weave ends: the tail's start (bound: the path's end).
    float weaveEnd(const BraidPath& path) const { return end == End::Bound ? path.length : (1.0f - tail) * path.length; }
    // How far down its tail `at` is: 0 in the braid, 1 at the end (bound: no tail).
    // End free: how far past the weave's end the sections take to let go: `release` braid widths,
    // no farther than the path goes.
    float releaseLength(const BraidPath& path) const {
        return std::clamp(release * width, 0.0f, std::max(path.length - weaveEnd(path), 0.0f));
    }
    // End free: the tail is one round bundle along the path, each section in a sector of it. The
    // sectors go round in the sections' order (so sections that lie side by side stay neighbours
    // whatever the weave's phase), turned so that each lies as near as can be to where its section
    // is, side to side and in depth, where the weave ends: no strand crosses the bundle to get there.
    void layBundle(BraidPath& path) const {
        path.bundleTurn = 0.0f, path.bundleSign = 1.0f;
        path.startTurn = 0.0f, path.startSign = 1.0f;
        if (weave.sections < 1) return;
        if (end == End::Free || tailSmoothness > 0.0f) {
            bundleTurnAt(path.weaveAt(weaveEnd(path)), &path.bundleTurn, &path.bundleSign);
        }
        // The weave fading in after the gather and out before the tail (bundledAt): over up to
        // kFadeSteps steps each, never past the other's.
        path.weaveFrom = feeds ? 0.0f : std::min(gather * path.length, weaveEnd(path));
        const float end0 = weaveEnd(path), steps0 = path.weaveAt(path.weaveFrom), steps1 = path.weaveAt(end0);
        const float room = std::max(steps1 - steps0, 0.0f);
        const float inSteps = std::min(fadeSteps(gatherSmoothness), 0.5f * room);
        const float outSteps = std::min(fadeSteps(tailSmoothness), 0.5f * room);
        path.fadedIn = gatherSmoothness > 0.0f ? path.distanceAt(steps0 + inSteps) : path.weaveFrom;
        path.fadeOut = tailSmoothness > 0.0f ? path.distanceAt(steps1 - outSteps) : end0;
        if (gatherSmoothness > 0.0f) bundleTurnAt(steps0, &path.startTurn, &path.startSign);
    }
    // How much the sections lie as one round bundle at `at` along the path (0: woven): fadePeak
    // where the weave starts, fading to 0 by where it has faded in (gatherSmoothness), and back up
    // where the weave ends (tailSmoothness), smoothly.
    float bundledAt(const BraidPath& path, float at) const {
        float bundled = 0.0f;
        if (gatherSmoothness > 0.0f && path.fadedIn > path.weaveFrom) {
            bundled = fadePeak(gatherSmoothness) * (1.0f - smoothstep(path.weaveFrom, path.fadedIn, at));
        }
        const float end0 = weaveEnd(path);
        if (tailSmoothness > 0.0f) {
            const float out = at >= end0 || !(end0 > path.fadeOut) ? 1.0f : smoothstep(path.fadeOut, end0, at);
            bundled = std::max(bundled, fadePeak(tailSmoothness) * out);
        }
        return bundled;
    }
    // A smoothness's fade (bundledAt): how much the sections lie as one bundle where the weave
    // starts or ends (rising smoothly from none at 0, all of it from 0.4 on), over how many weave
    // steps (one at least, kFadeSteps at 1). The braid changes gradually as the slider moves off
    // 0, never at once.
    static float fadePeak(float smoothness) { return smoothstep(0.0f, 0.4f, smoothness); }
    static float fadeSteps(float smoothness) { return 1.0f + (kFadeSteps - 1.0f) * smoothness; }
    // The turn of the one bundle best matching the sections' places `steps` along the weave:
    // section k's sector at *turnOut + *signOut x k of a turn's n-th, each as near where its section lies.
    void bundleTurnAt(float steps, float* turnOut, float* signOut) const {
        const float n = static_cast<float>(weave.sections);
        *turnOut = 0.0f, *signOut = 1.0f;
        float best = -1.0f;
        for (const float sign : {1.0f, -1.0f}) {
            float re = 0.0f, im = 0.0f;  // the sum of (side - i depth) e^(i sector)
            for (int k = 0; k < weave.sections; ++k) {
                const std::array<float, 2> c = weave.at(k, steps);
                const float turn = sign * kTwoPi * static_cast<float>(k) / n, cs = pm::cos(turn), sn = pm::sin(turn);
                re += c[0] * cs + c[1] * sn;
                im += c[0] * sn - c[1] * cs;
            }
            const float fit = re * re + im * im;
            if (fit > best) {
                best = fit;
                *signOut = sign;
                *turnOut = fit > 1e-12f ? -pm::atan2(im, re) : 0.0f;
            }
        }
    }
    float tailAt(const BraidPath& path, float at) const {
        if (end == End::Free) {  // the sections let go over the release, then the hair is free
            if (!(tail > 0.0f)) return 0.0f;
            const float from = weaveEnd(path), over = releaseLength(path);
            return over > 0.0f ? smoothstep(from, from + over, at) : (at >= from ? 1.0f : 0.0f);
        }
        return end == End::Bound || !(tail > 0.0f) ? 0.0f : smoothstep((1.0f - tail) * path.length, path.length, at);
    }
    // The ties along `path`: the last where the weave ends, the rest spread back toward where the
    // braid forms (or `tieSpacing` braid widths apart).
    void placeTies(BraidPath& path) const {
        path.ties.clear();
        if (ties <= 0) return;
        const float here = width * std::max(taper(std::clamp(weaveEnd(path) / path.length, 0.0f, 1.0f)), 0.05f);
        path.tieHalf = 0.5f * std::max(tieLength, 0.05f) * here;
        const float last = weaveEnd(path) - path.tieHalf - (end == End::Bound ? 0.5f * here : 0.0f);
        const float first = (feedUntil > 0.0f ? feedUntil : gather) * path.length + path.tieHalf;
        const float step = tieSpacing > 0.0f ? tieSpacing * width : ties > 1 ? (last - first) / static_cast<float>(ties - 1) : 0.0f;
        for (int k = 0; k < ties; ++k) {
            const float at = last - static_cast<float>(k) * step;
            // (A tie a rounding short of `first` still counts: the slack by the path's length, not a
            // fixed scene length, which a braid in centimetres rounds past.)
            if (k > 0 && (at < first - 1e-5f * path.length || !(step > 0.0f))) break;
            path.ties.push_back(std::max(at, path.tieHalf));
        }
    }
    // How much `at` is inside a tie: 1 within one, easing to 0 either side.
    float inTie(const BraidPath& path, float at) const {
        if (path.ties.empty()) return 0.0f;
        const float fade = path.tieHalf + 0.4f * width;
        float most = 0.0f;
        for (std::size_t k = 0; k < path.ties.size(); ++k) {
            const float d = std::abs(at - path.ties[k]) - path.tieHalf;
            const float w = d <= 0.0f ? 1.0f : 1.0f - smoothstep(0.0f, fade, d);
            if (w > most) most = w;
        }
        return most;
    }
    // The braid's axis where it is raised `at` a distance along (the path's point, lifted).
    Vec3f axisAt(const BraidPath& path, float at) const {
        Vec3f center;
        const Frame& fr = frameAt(path, at, &center);
        return center + fr.normal * (liftAt(path, at) * braidWidthAt(path, at));
    }
    // Tie k's straight axis, as a rigid bead lies: from the braid's axis at the tie's start to where
    // it is at the tie's end (`from` and the unit direction).
    void tieAxis(const BraidPath& path, std::size_t k, Vec3f* from, Vec3f* direction) const {
        const float c = path.ties[k];
        *from = axisAt(path, c - path.tieHalf);
        const Vec3f to = axisAt(path, c + path.tieHalf);
        const Vec3f chord = to - *from;
        *direction = chord.length() > 1e-9f ? chord * (1.0f / chord.length()) : frameAt(path, c, &*from).tangent;
        if (!(chord.length() > 1e-9f)) *from = axisAt(path, c - path.tieHalf);
    }
    // Every tie's axis kept on its path (tieAxis's, for braidedAt at every point): once the paths
    // are as they stay (a fed braid's grown).
    void layTieAxes() {
        for (std::size_t b = 0; b < paths.size(); ++b) {
            BraidPath& path = paths[b];
            const Braider& own = of(b);
            path.tieFrom.resize(path.ties.size());
            path.tieDirection.resize(path.ties.size());
            for (std::size_t k = 0; k < path.ties.size(); ++k) own.tieAxis(path, k, &path.tieFrom[k], &path.tieDirection[k]);
        }
    }
    // The braid's width factor at `at`: narrowed to 1 - cinch inside a tie, easing back either side.
    float cinchAt(const BraidPath& path, float at) const {
        if (!(cinch > 0.0f)) return 1.0f;
        return 1.0f - std::clamp(cinch, 0.0f, 0.95f) * inTie(path, at);
    }
    // The braid's width at `at`, its ties and a bound end narrowing it.
    float braidWidthAt(const BraidPath& path, float at) const {
        float wid = widthAt(path, at) * cinchAt(path, at);
        if (end == End::Bound) wid *= 1.0f - 0.65f * smoothstep(path.length - 1.5f * width, path.length, at);
        return wid;
    }
    // Raised off the path (braid widths): a cornrow's, a fed braid's on the scalp, easing off where
    // it leaves the head. A fed braid is raised only as far as its underside would sink into the
    // scalp: a path drawn above the scalp, or one hanging off the head, stays where it was drawn.
    float liftAt(const BraidPath& path, float at) const {
        float lift = weave.lift;
        if (feedLift > 0.0f) {  // (version 1)
            const float leaves = feedUntil * path.length;
            lift += feedLift * (1.0f - smoothstep(leaves, leaves + 2.0f * width, at));
        }
        if (clearLift > 0.0f && !path.overScalp.empty()) {
            // Version 2: raised as far as its underside would lie inside the scalp, the raise eased
            // in over a quarter of that depth (no kink where the braid comes to touch the scalp).
            const float wid = std::max(braidWidthAt(path, at), 1e-6f);
            const float above = path.sample(path.overScalp, at) / wid, band = 0.25f * clearLift;
            if (above < clearLift - band) {
                lift += clearLift - above;
            } else if (above < clearLift + band) {
                const float d = clearLift + band - above;
                lift += d * d / (4.0f * band);
            }
        }
        return lift;
    }
    // The path's point and frame `at` a distance along it (past its end, straight on).
    const Frame& frameAt(const BraidPath& path, float at, Vec3f* center) const {
        const std::size_t last = path.points.size() - 1;
        if (at < path.length) {
            const float f = std::max(at, 0.0f) / path.length * static_cast<float>(last);
            const std::size_t k = std::min(static_cast<std::size_t>(f), last - 1);
            const float w = f - static_cast<float>(k);
            *center = path.points[k] * (1.0f - w) + path.points[k + 1] * w;
            return path.frames[w < 0.5f ? k : k + 1];
        }
        *center = path.points[last] + (path.points[last] - path.points[last - 1]).normalized() * (at - path.length);
        return path.frames[last];
    }
    // Steps a unit of length where the taper is `u` along: a step is stepLength braid widths long
    // (over tightness), never longer than at the braid's start nor shorter than a sixth of it.
    float rateAt(float u) const {
        const float start = std::max(taper(0.0f), 1e-3f);
        const float w = width * std::clamp(taper(u), 0.15f * start, start);
        return tightness / std::max(weave.stepLength * w, 1e-6f);
    }
    StrandDraws drawsFor(std::uint64_t id) const { return StrandDraws(id, seed, weave.sections); }
    // The places of the clumps among `members` (strands of `strands` braided along `path`), by
    // their numbers in `clumps` (ClumpPlace).
    ClumpPlaces clumpPlaces(const BraidPath& path, const StrandSet& strands, const std::uint32_t* members, std::size_t count,
                            const std::vector<float>& clumps) const {
        struct Sum {
            Vec3f roots, middles;  // summed, then the means
            double rootSpread = 0.0, middleSpread = 0.0;
            std::size_t count = 0;
        };
        std::unordered_map<std::int64_t, Sum> sums;
        const auto middleOf = [&](std::size_t s) { return strands.strandPoints(s)[strands.strandPointCount(s) / 2]; };
        for (std::size_t k = 0; k < count; ++k) {
            const std::size_t s = members[k];
            std::int64_t key = 0;
            if (strands.strandPointCount(s) < 1 || !clumpKey(clumps[s], &key)) continue;
            Sum& sum = sums[key];
            sum.roots = sum.roots + strands.strandPoints(s)[0];
            sum.middles = sum.middles + middleOf(s);
            ++sum.count;
        }
        for (auto& [key, sum] : sums) {
            sum.roots = sum.roots / static_cast<float>(sum.count);
            sum.middles = sum.middles / static_cast<float>(sum.count);
        }
        // How far its strands lie from their mean, at the roots and halfway along (how tight it is).
        for (std::size_t k = 0; k < count; ++k) {
            const std::size_t s = members[k];
            std::int64_t key = 0;
            if (strands.strandPointCount(s) < 1 || !clumpKey(clumps[s], &key)) continue;
            Sum& sum = sums[key];
            sum.rootSpread += (strands.strandPoints(s)[0] - sum.roots).length();
            sum.middleSpread += (middleOf(s) - sum.middles).length();
        }
        ClumpPlaces places;
        const float perSection = std::max(static_cast<float>(count) / static_cast<float>(std::max(weave.sections, 1)), 1.0f);
        const float in = 0.5f * gather * path.length;
        for (const auto& [key, sum] : sums) {
            Rng rng(hashCombine(static_cast<std::uint64_t>(key), seed ^ 0xC1A3Bull));
            ClumpPlace place;
            const Vec3f& mean = sum.roots;
            const int fallback = static_cast<int>(rng.nextU64() % static_cast<std::uint64_t>(std::max(weave.sections, 1)));
            place.section = sectionOfRoot(mean - path.root, path.frames.front(), path.weaveAt(in), fallback);
            const float room = std::clamp(std::sqrt(static_cast<float>(sum.count) / perSection), 0.02f, 1.0f);
            const double tight = sum.rootSpread > 0.0 ? sum.middleSpread / sum.rootSpread : 1.0;
            place.share = room * static_cast<float>(std::clamp(tight, 0.02, 1.0));
            const float a = kTwoPi * rng.nextFloat();
            const float r = kLobeReach * (1.0f - room) * std::sqrt(rng.nextFloat());  // (a member's spot within kLobeReach too)
            place.spot = Vec3f{r * pm::cos(a), r * pm::sin(a), 0.0f};
            places.emplace(key, place);
        }
        return places;
    }

    // What the path alone gives of the braid's point `at` a distance along it: its point and
    // frame, the braid's width, tail and weave there, the hold of the nearest tie.
    PathStation stationAt(const BraidPath& path, float at) const {
        PathStation st;
        st.frame = &frameAt(path, at, &st.center);
        st.u = std::min(at / path.length, 1.0f);
        st.wid = braidWidthAt(path, at);
        st.letGo = tailAt(path, at);
        st.bundled = bundledAt(path, at);
        // (the start's bundle up to the weave's middle, the tail's past it)
        const bool start = gatherSmoothness > 0.0f && at < 0.5f * (path.weaveFrom + weaveEnd(path));
        st.bundleTurn = start ? path.startTurn : path.bundleTurn, st.bundleSign = start ? path.startSign : path.bundleSign;
        st.rate = at < path.length ? path.sample(path.rate, at) : path.rate.back();
        st.steps = path.weaveAt(at);
        st.lift = liftAt(path, at);
        // At a tie the hair is held within it, round the tie's straight axis (the rigid bead's, from
        // the braid's raised axis at its start to its end): inside the tie to just within its bore
        // (kTieBore of the braid's width), past its ends within a cone widening from there, so every
        // strand goes in and out through the bore's opening, never across the tie's rim. The hold
        // lets go over a braid width past the tie (its axis easing back onto the braid's own, which
        // a curving braid takes away from the tie's straight line), so nothing kinks where it ends.
        if (!path.ties.empty()) {
            std::size_t which = 0;
            float nearest = std::numeric_limits<float>::infinity();
            for (std::size_t k = 0; k < path.ties.size(); ++k) {
                const float gap = std::abs(at - path.ties[k]);
                if (gap < nearest) {
                    nearest = gap;
                    which = k;
                }
            }
            const float outside = std::max(nearest - path.tieHalf, 0.0f);
            st.release = smoothstep(0.25f * width, width, outside);
            if (st.release < 1.0f) {
                const Frame& fr = *st.frame;
                const Vec3f& from = path.tieFrom[which];  // (layTieAxes)
                const Vec3f& direction = path.tieDirection[which];
                const float along = std::clamp(at - (path.ties[which] - path.tieHalf), 0.0f, 2.0f * path.tieHalf);
                const Vec3f onTie = fr.toLocal(from + direction * along - st.center);
                // (the braid's own axis here: axisAt's, from the point, lift and width in hand)
                const Vec3f own = fr.toLocal(st.center + fr.normal * (st.lift * st.wid) - st.center);
                st.tieAxis = onTie + (own - onTie) * st.release;
                st.most = 0.47f * kTieBore * st.wid + 0.15f * outside;
            }
        }
        if (pressed) sampleAt(st);
        return st;
    }
    // A section's band at a station, in the frame's (normal, binormal, tangent): the section's band
    // follows its own course, so it passes over and under its neighbours instead of through them.
    SectionStation sectionAt(int section, const PathStation& st) const {
        if (pressed && pressed->rings.empty()) {
            // (pressAcross: the section where the braid's cross-section cuts it, the whole scaled to the braid's reach)
            const SectionCut cut = cutOf(weave, section, st.steps, pancake, tilt);
            SectionStation sec;
            sec.center = Vec3f{(cut.center[1] * keep[1] + st.lift) * st.wid, cut.center[0] * keep[0] * st.wid, 0.0f};
            sec.across = Vec3f{cut.across[1] * keep[1], cut.across[0] * keep[0], 0.0f};
            sec.thick = Vec3f{cut.thick[1] * keep[1], cut.thick[0] * keep[0], 0.0f};
            sec.rows[0] = pressed->row(section, st.sample);
            sec.rows[1] = pressed->row(section, st.sample + 1 < pressed->count ? st.sample + 1 : 0);
            return sec;
        }
        std::array<float, 2> slope{};
        std::array<float, 2> c = weave.at(section, st.steps, &slope);
        if (pancake > 0.0f) {  // pulled apart: the sections out from the middle, looser and wider
            const float out = 1.0f + 0.45f * pancake;
            c = {c[0] * out, c[1] * (1.0f + 0.25f * pancake)};
            slope = {slope[0] * out, slope[1] * (1.0f + 0.25f * pancake)};
        }
        const float wid = st.wid, rate = st.rate;
        SectionStation sec;
        sec.center = Vec3f{(c[1] + st.lift) * wid, c[0] * wid, 0.0f};
        const Vec3f along = Vec3f{slope[1] * rate * wid, slope[0] * rate * wid, 1.0f}.normalized();
        Vec3f thick = weave.round ? Vec3f{c[1], c[0], 0.0f} : Vec3f{1.0f, 0.0f, 0.0f};
        thick = thick - along * thick.dot(along);
        if (thick.length() < 1e-5f) thick = Vec3f{1.0f, 0.0f, 0.0f} - along * along.x;
        sec.thick = thick.normalized();
        sec.across = along.cross(sec.thick);
        if (pressed) {
            // The pressed tube's ring here (between two samples of it): its center and its two
            // directions, square to the section's course as the weave's own tilt has it (a braid
            // thinner for its steps, a fed braid's start, a tie's waist, keeps the tubes worked out
            // for it), `separate` of the way from the plain section.
            const int after = st.sample + 1 < pressed->count ? st.sample + 1 : 0;
            const float* r0 = pressed->ring(section, st.sample);
            const float* r1 = pressed->ring(section, after);
            const auto ring = [&](int c) { return r0[c] + (r1[c] - r0[c]) * st.between; };
            const float kx = pressed->keep[0], ky = pressed->keep[1];
            const Vec3f center{(ring(1) * ky + st.lift) * wid, ring(0) * kx * wid, 0.0f};
            const Vec3f across{ring(2) * ky, ring(3) * kx, ring(4)}, through{ring(5) * ky, ring(6) * kx, ring(7)};
            sec.center = sec.center + (center - sec.center) * separate;
            sec.across = sec.across + (across - sec.across) * separate;
            sec.thick = sec.thick + (through - sec.thick) * separate;
            sec.rows[0] = pressed->row(section, st.sample);
            sec.rows[1] = pressed->row(section, after);
        }
        return sec;
    }
    // A strand's spot in its section (from the unit disc: looser, wider sections with pancake) and
    // its end's fan, by its draws.
    StrandSpot spotOf(const StrandDraws& d) const {
        const float loose = 1.0f + 0.6f * pancake;
        StrandSpot spot;
        spot.inBundle = true;
        spot.across = d.inLobe.x * weave.halfWidth * loose;
        spot.thick = d.inLobe.y * weave.halfThick * loose;
        spot.out = std::min(std::sqrt(d.inLobe.x * d.inLobe.x + d.inLobe.y * d.inLobe.y) / kLobeReach, 1.0f);
        if (end != End::Tuft) {
            spot.fanDepth = style == Style::Cornrow ? std::abs(pm::sin(d.fanAngle)) : pm::sin(d.fanAngle);
            spot.fanSide = pm::cos(d.fanAngle);
        }
        if (pressed) {  // the outline's ray it lies along
            const float turn = pm::atan2(d.inLobe.y, d.inLobe.x) * (static_cast<float>(Pressed::kAround) / kTwoPi);
            spot.ray = std::clamp(turn < 0.0f ? turn + static_cast<float>(Pressed::kAround) : turn, 0.0f,
                                  static_cast<float>(Pressed::kAround) - 1e-3f);
        }
        return spot;
    }
    // The stations of `path` for a strand of `count` points evenly along it (braidAlong's).
    PathTable tableFor(const BraidPath& path, std::size_t count) const {
        PathTable table;
        table.count = count;
        table.stations.resize(count);
        table.blend.assign(count, 0.0f);
        const std::size_t sections = static_cast<std::size_t>(weave.sections);
        table.sections.resize(sections * count);
        for (std::size_t i = 1; i < count; ++i) {
            const float u = static_cast<float>(i) / static_cast<float>(count - 1);
            table.stations[i] = stationAt(path, u * path.length);
            table.blend[i] = smoothstep(0.0f, gather, u);
            for (std::size_t k = 0; k < sections; ++k) {
                table.sections[k * count + i] = sectionAt(static_cast<int>(k), table.stations[i]);
            }
        }
        return table;
    }
    // The hair a section lets out of the braid at `steps` for a strand at `spot` within it: at a
    // share `loops` of the sections' stretches between crossings along the braid's edge (each
    // section's own), the hair on its outer side bows out off the braid's side and goes back in
    // before the next crossing, most at the section's outer edge, less toward its inside (how far
    // in it reaches, each loop its own): a section of the braid pulled loose, its hair together.
    // `pressedTo`: the scale the section's outline puts the strand's spot at (1: a plain ellipse).
    Vec3f loopAt(int section, float steps, const Vec3f& spot, float wid, float pressedTo) const {
        if (!(loops > 0.0f) || !(loopSize > 0.0f) || weave.lobes.empty()) return {};
        const auto& mine = weave.lobes[static_cast<std::size_t>(section)];
        if (mine.empty()) return {};
        const float cycle = static_cast<float>(weave.cycle);
        const float turn = std::floor(steps / cycle), within = steps - turn * cycle;
        for (std::size_t k = 0; k < mine.size(); ++k) {
            for (int wrap = 0; wrap < 2; ++wrap) {  // (a stretch past the cycle's end, from the turn before)
                const float at = within + static_cast<float>(wrap) * cycle;
                if (at < mine[k][0] || at >= mine[k][1]) continue;
                const float u = (at - mine[k][0]) / (mine[k][1] - mine[k][0]);
                const std::int64_t visit = static_cast<std::int64_t>(turn) - wrap;
                const std::uint64_t key = hash64(hashCombine(hashCombine(seed ^ 0x100F5ull, static_cast<std::uint64_t>(section * 64 + static_cast<int>(k))),
                                                             static_cast<std::uint64_t>(visit + (1ll << 32))));
                if (unitFloat(key) >= loops) return {};
                // Its side: where the stretch's middle is.
                const float mid = 0.5f * (mine[k][0] + mine[k][1]);
                const float side = weave.at(section, mid)[0] >= 0.0f ? 1.0f : -1.0f;
                // The bundle: the hair out from how far in this loop reaches (spot.x along the band's
                // width, toward the side), out together, a gap left between it and the rest; a
                // little more toward the outer edge, as a loose loop fans.
                const float reachIn = -0.1f + 0.6f * unitFloat(hash64(key ^ 0xB0D1ull));
                const float edge = side * spot.x;
                // (in the loop or not: hair between would web the gap shut)
                const float pulled = edge > reachIn ? 0.85f + 0.15f * std::clamp((edge - reachIn) / (1.0f - reachIn), 0.0f, 1.0f) : 0.0f;
                if (!(pulled > 0.0f)) return {};
                const float size = loopSize * (0.6f + 0.8f * unitFloat(hash64(key ^ 0x51Eull)));
                const float swell = pm::sin(kPi * u);
                const float bow = swell * size * wid * pulled;
                // Clumped as it leaves: the bundle's hair drawn in toward its own middle (half way
                // out its share of the section), a lock of its own with a gap between it and the braid.
                const float clump = 0.85f * swell;
                const float loose = 1.0f + 0.6f * pancake;
                const float toX = (side * 0.5f * (reachIn + 1.0f) - spot.x) * clump * weave.halfWidth * loose * wid * pressedTo;
                const float toY = -spot.y * clump * weave.halfThick * loose * wid * pressedTo;
                return {0.35f * bow + toY, side * bow + toX, 0.0f};  // off the side, a little up
            }
        }
        return {};
    }

    // The braid's point `at` a distance along `path` for a strand in `section` (-1: the path
    // itself) with draws `d`: its section's offset in the path's frame, letting go and fanning out
    // over the tail.
    Vec3f braidedAt(const BraidPath& path, float at, int section, const StrandDraws& d) const {
        if (section < 0) {
            Vec3f center;
            frameAt(path, at, &center);
            return center;
        }
        const PathStation st = stationAt(path, at);
        return braidedFrom(st, sectionAt(section, st), section, d, spotOf(d));
    }
    // A strand's spot in the one round bundle (end free's tail, the weave fading in or out), off
    // the path in the frame: as large as the sections' hair takes together (their ellipses' area),
    // each section the wedge of it that its sector is (layBundle, the station's turn). A strand's
    // spot in its section's disc gives its spot in the wedge: how far out the wedge's way the spot
    // lies in the disc (`out`) is how far from the path it goes, the disc's side nearest the path
    // to the path itself and its far side to the bundle's rim, as much of the wedge's hair inside
    // it there as of the disc's here; where it lies across the disc at that height (`side`) is its
    // angle across the wedge. Evenly spread hair stays evenly spread, so the bundle is one body with
    // no seam between the sections' hair, and neighbours in the disc are neighbours in the wedge:
    // nothing tears as the sections go into it or come out of it.
    Vec3f bundleSpot(const PathStation& st, int section, const StrandDraws& d) const {
        const float n = static_cast<float>(weave.sections);
        const float sector = st.bundleTurn + st.bundleSign * kTwoPi * static_cast<float>(section) / n;
        const float sn = pm::sin(sector), cs = pm::cos(sector);
        const float qx = d.inLobe.y / kLobeReach, qy = d.inLobe.x / kLobeReach;  // in the unit disc, the path's frame
        const float out = std::clamp(qx * sn + qy * cs, -1.0f, 1.0f);
        const float chord = std::sqrt(std::max(1.0f - out * out, 1e-12f));
        const float side = std::clamp((qx * cs - qy * sn) / chord, -1.0f, 1.0f);
        const float within = 1.0f - (pm::acos(out) - out * chord) / kPi;  // the disc's share no farther out
        const float angle = sector + side * kPi / n;
        const float radius = std::sqrt(n * weave.halfWidth * weave.halfThick) * (1.0f + 0.6f * pancake) * st.wid;
        return Vec3f{pm::sin(angle), pm::cos(angle), 0.0f} * (kLobeReach * std::sqrt(std::max(within, 0.0f)) * radius);
    }
    // The same from the station and its section's band (stationAt, sectionAt: as they are worked
    // out there, or kept in a path's table), for a strand with draws `d` at `spot` (spotOf).
    // `press` (null: not asked) gets how much it lies against another section there (pressAt), none
    // as it lets go of the braid or a tie holds it.
    Vec3f braidedFrom(const PathStation& st, const SectionStation& sec, int section, const StrandDraws& d,
                      const StrandSpot& spot, float* press = nullptr) const {
        const float wid = st.wid, letGo = st.letGo, bundled = st.bundled;
        // Its spot in its section: of the plain ellipse, or along its ray as far as the pressed
        // outline lets it.
        float pressedTo = 1.0f;
        Vec3f local;
        if (press) *press = sec.rows[0] ? pressAt(sec.rows, st.between, spot) * (1.0f - letGo) * (1.0f - bundled) * st.release : 0.0f;
        if (sec.rows[0]) {
            pressedTo = outlineAt(sec.rows, st.between, spot.ray);
            local = (sec.center + (sec.across * (spot.across * wid) + sec.thick * (spot.thick * wid)) * pressedTo) * (1.0f - letGo);
        } else {
            local = (sec.center + sec.across * (spot.across * wid) + sec.thick * (spot.thick * wid)) * (1.0f - letGo);
        }
        Vec3f loose;  // where the strand goes as it lets go
        if (end == End::Tuft) {  // gathered: its spot within the tuft, a little looser down it
            loose = Vec3f{d.inLobe.y, d.inLobe.x, 0.0f} * (letGo * wid * (0.3f + 0.25f * letGo));
        } else if (end == End::Free) {
            // Free: one round bundle along the path (bundleSpot). The Braid adds no shape of its own there.
            if (spot.inBundle && letGo > 0.0f) loose = bundleSpot(st, section, d) * letGo;
        } else {  // fanned out
            loose = Vec3f{spot.fanDepth, spot.fanSide, 0.0f} * (letGo * wid * d.fan);
        }
        // The weave fading in or out (gatherSmoothness, tailSmoothness): the sections toward the one
        // round bundle, a strand to its spot there (a section's center line, a tube's rim, onto the
        // path), so the crossings grow from it or shrink into it.
        if (bundled > 0.0f) {
            local = local * (1.0f - bundled);
            if (spot.inBundle) loose = loose + bundleSpot(st, section, d) * (bundled * (1.0f - letGo));
        }
        Vec3f offset = local + loose;
        // Loose bundles looping out of their section (loops) go before the ties: those hold them too.
        offset = offset + loopAt(section, st.steps, d.inLobe, wid, pressedTo) * ((1.0f - letGo) * (1.0f - bundled));
        if (st.release < 1.0f) {  // held by a tie (stationAt)
            const float n = offset.x - st.tieAxis.x, b = offset.y - st.tieAxis.y, r = std::sqrt(n * n + b * b);
            if (r > st.most) {
                const float k = 1.0f + (st.most / r - 1.0f) * (1.0f - st.release);
                offset.x = st.tieAxis.x + n * k;
                offset.y = st.tieAxis.y + b * k;
            }
            // (a pressed tube's ring leans along the braid: straight through the tie's bore)
            if (pressed) offset.z *= st.release;
        }
        return st.center + st.frame->toWorld(offset);
    }

    // The section `share` of the way across the braid (0: its one side, 1: the other, along the
    // frame's binormal) at `steps`: the sections ranked side to side there, an equal share each.
    int sectionAcross(float share, float steps) const {
        const int n = weave.sections;
        std::array<std::pair<float, int>, Weave::kMostSections> order;
        for (int k = 0; k < n; ++k) order[static_cast<std::size_t>(k)] = {weave.at(k, steps)[0], k};
        std::sort(order.begin(), order.begin() + n);
        const int rank = std::clamp(static_cast<int>(share * static_cast<float>(n)), 0, n - 1);
        return order[static_cast<std::size_t>(rank)].second;
    }
    // The section a strand whose root lies `offset` from where it joins the braid (frame `fr`)
    // takes: the root region cut into equal wedges side to side, around the join, one a section
    // (`fallback`: a root on the path itself).
    int sectionOfRoot(const Vec3f& offset, const Frame& fr, float steps, int fallback) const {
        const float d = offset.length();
        if (!(d > 0.0f)) return fallback;
        const float t = std::clamp(offset.dot(fr.binormal) / d, -1.0f, 1.0f);
        return sectionAcross(pm::acos(-t) / kPi, steps);
    }

    // Without feed: the path a strand rooted at `root` joins, the nearest root within the region
    // (-1: none).
    int pathOf(const Vec3f& root) const {
        int best = -1;
        float bestD2 = region2;
        for (std::size_t b = 0; b < paths.size(); ++b) {
            const float d2 = (paths[b].root - root).dot(paths[b].root - root);
            if (d2 <= bestD2) {
                bestD2 = d2;
                best = static_cast<int>(b);
            }
        }
        return best;
    }
    // Without feed, the points strand s of `in` is braided with: as many as it has, or for one
    // braided the points its path's weave needs. `joins` gets the path its
    // root joins (pathOf; -1: none), for braidOne.
    std::size_t pointsFor(const StrandSet& in, std::size_t s, int* joins) const {
        const std::size_t n = in.strandPointCount(s);
        const int path = n < 2 ? -1 : pathOf(in.strandPoints(s)[0]);
        *joins = path;
        if (path < 0) return n;
        return sampling.count(n, paths[static_cast<std::size_t>(path)].repeats, 1.0f);
    }
    // Without feed, the strand's points along its path from root to tip (pts[0], its root, stays);
    // `tails` (null: not asked) gets each point's braidTail, `woven` its braidWoven, `presses` its
    // braidPress. `table`: the path's stations for a strand of n points (tableFor), read in place of
    // working each point's out; null: worked out.
    int braidAlong(Vec3f* pts, std::size_t n, const BraidPath& path, const StrandDraws& d, float* tails, float* woven = nullptr,
                   const PathTable* table = nullptr, float* presses = nullptr) const {
        // The section on its side where it comes in, halfway through the gather.
        const float in = 0.5f * gather * path.length;
        const int section = d.section >= 0 ? d.section : sectionOfRoot(pts[0] - path.root, path.frames.front(), path.weaveAt(in), d.lobe);
        const StrandSpot spot = spotOf(d);
        if (tails) tails[0] = 0.0f;
        if (woven) woven[0] = 0.0f;
        if (presses) presses[0] = 0.0f;
        const SectionStation* bands = table ? table->sections.data() + static_cast<std::size_t>(section) * n : nullptr;
        for (std::size_t i = 1; i < n; ++i) {
            float blend = 0.0f, letGo = 0.0f, press = 0.0f;  // from the root into the braid; down its tail
            Vec3f braided;
            if (table) {
                const PathStation& st = table->stations[i];
                braided = braidedFrom(st, bands[i], section, d, spot, &press);
                blend = table->blend[i];
                letGo = st.letGo;
            } else {
                const float u = static_cast<float>(i) / static_cast<float>(n - 1);
                const PathStation st = stationAt(path, u * path.length);
                braided = braidedFrom(st, sectionAt(section, st), section, d, spot, &press);
                blend = smoothstep(0.0f, gather, u);
                letGo = st.letGo;
            }
            pts[i] = pts[i] * (1.0f - blend) + braided * blend;
            overScalp.apply(pts[i], blend, (pts[i] - pts[0]).length());
            if (tails) tails[i] = letGo;
            if (woven) woven[i] = blend;
            if (presses) presses[i] = press * blend;
        }
        return section;
    }
};

// Feed mode's lookups: the path samples a strand may be picked up at (the first `feedUntil` of
// each path, past the root: that is the region's) in one index, the roots in another.
struct FeedIndex {
    std::vector<std::pair<std::size_t, std::size_t>> sampleOf;  // (path, sample)
    PointIndex pickUp, rootIndex;
    std::size_t rootCount = 0;

    static std::vector<Vec3f> samplesOf(const std::vector<BraidPath>& paths, float feedUntil,
                                        std::vector<std::pair<std::size_t, std::size_t>>& sampleOf) {
        std::vector<Vec3f> samples;
        for (std::size_t b = 0; b < paths.size(); ++b) {
            const std::size_t last = paths[b].points.size() - 1;
            const std::size_t until = static_cast<std::size_t>(feedUntil * static_cast<float>(last) + 1e-3f);
            for (std::size_t k = 1; k <= std::min(until, last); ++k) {
                samples.push_back(paths[b].points[k]);
                sampleOf.emplace_back(b, k);
            }
        }
        return samples;
    }
    static std::vector<Vec3f> rootsOf(const std::vector<BraidPath>& paths) {
        std::vector<Vec3f> roots;
        for (const BraidPath& p : paths) roots.push_back(p.root);
        return roots;
    }
    FeedIndex(const std::vector<BraidPath>& paths, float feedUntil)
        : pickUp(samplesOf(paths, feedUntil, sampleOf)), rootIndex(rootsOf(paths)), rootCount(paths.size()) {}
};

// A fed strand measured as its points are: `along` gets the arc length from its root to each point,
// the plan its length.
void measureFed(FedPlan& plan, const Vec3f* pts, std::size_t n, std::vector<float>& along) {
    along.resize(n);
    cumulativeArcLength(pts, n, along.data());
    plan.strandLength = along[n - 1];
}

// A fed strand (measured, its join planned) fitted to `path`: the strand's length is shared out so
// it ends within the braid's last stretch (a random spot in the tail, so the end isn't cut
// square), never past it. False for a strand joining at or past that end, which has no braid to
// run along: it stays loose.
bool fitFed(const Braider& br, const BraidPath& path, FedPlan& plan) {
    plan.squeeze = 1.0f;
    const float endAt = path.length * (1.0f - 0.5f * (br.end == End::Bound ? 0.0f : br.tail) * plan.draws.endDraw);
    if (plan.joinAt >= endAt) return false;
    const float room = plan.approach + endAt - plan.joinAt;
    if (plan.strandLength > room && plan.strandLength > 1e-6f) plan.squeeze = room / plan.strandLength;
    return true;
}

// Feed mode: where strand `id` (points `pts`) joins a path: where it reaches it, or at the point
// nearest its root; `along` gets the arc length from its root to each point. What of the plan the
// strand's root alone decides (its path, where it joins, its draws) stays while the root does;
// its length and fit are its points' (replanFed).
FedPlan planFed(const Braider& br, const FeedIndex& index, const Vec3f* pts, std::size_t n, std::uint64_t id,
                std::vector<float>& along) {
    FedPlan plan;
    if (n < 2) return plan;
    // Picked up at the nearest path point within the region, or from the nearest root within it:
    // whichever is closer.
    int best = -1;
    std::size_t bestK = 0;
    float bestD2 = std::numeric_limits<float>::infinity();
    {
        float d2 = 0.0f;
        // (Each the nearest if it is within reach: nearestWithin looks no farther than that.)
        const std::size_t q = index.pickUp.nearestWithin(pts[0], br.region2, &d2);
        if (q < index.sampleOf.size() && d2 <= br.region2) {
            best = static_cast<int>(index.sampleOf[q].first);
            bestK = index.sampleOf[q].second;
            bestD2 = d2;
        }
        const std::size_t r = index.rootIndex.nearestWithin(pts[0], std::min(br.region2, bestD2), &d2);
        if (r < index.rootCount && d2 <= br.region2 && d2 <= bestD2) {
            best = static_cast<int>(r);
            bestK = 0;
            bestD2 = d2;
        }
    }
    if (best < 0) return plan;
    plan.draws = br.drawsFor(id);
    const BraidPath& path = br.paths[static_cast<std::size_t>(best)];
    const Braider& own = br.of(static_cast<std::size_t>(best));  // (its settings: where the weave ends, how it alternates)
    const std::size_t last = path.points.size() - 1;
    const float step = path.length / static_cast<float>(last);
    measureFed(plan, pts, n, along);

    // Where the strand joins: drawn in from its root, along the scalp, swept on as a braider pulls
    // hair into the next crossing. Picked up along the path, it joins about as far on as its root
    // lies off it, and on a flat braid at the next crossing made from its side (each crossing picks
    // up the hair on its own side: the band of hair into every crossing that cornrows and French
    // braids show), never past where the braid picks hair up nor into its tail (hair left over
    // there joins the last crossing before). The root region's hair the same, from the start: the
    // first crossings take it in turn, not all at one point.
    const float off = std::sqrt(bestD2);
    float joinAt = static_cast<float>(bestK) * step;
    {
        const float limit = std::max(std::min(br.feedUntil * path.length, own.weaveEnd(path)), 0.0f);
        joinAt = std::min(joinAt + off, std::max(limit, joinAt));
        if (own.weave.alternates) {
            const float side = (pts[0] - path.points[bestK]).dot(path.frames[bestK].binormal);
            float k = std::ceil(path.weaveAt(joinAt) - 0.05f);
            if ((static_cast<long long>(k) % 2 == 0) != (side > 0.0f)) k += 1.0f;
            while (k >= 2.0f && path.distanceAt(k + 0.05f) > limit) k -= 2.0f;
            joinAt = path.distanceAt(k + 0.05f);
        }
    }
    plan.joinK = std::min(static_cast<std::size_t>(std::lround(joinAt / step)), last);
    plan.joinAt = joinAt;
    plan.region = bestK == 0;
    plan.approach = std::max((pts[0] - path.points[plan.joinK]).length(), 1e-6f);
    if (!fitFed(own, path, plan)) return plan;
    plan.path = best;
    return plan;
}

// planFed for a strand planned before (`planned`, on the points `was` it had then) whose points are
// now `pts`: while its root is where it was (as resampling leaves it), where it joins stays and
// only its length and fit are worked out again; else planned anew.
FedPlan replanFed(const Braider& br, const FeedIndex& index, const FedPlan& planned, const Vec3f* was, const Vec3f* pts,
                  std::size_t n, std::uint64_t id, std::vector<float>& along) {
    if (n < 2 || !was || !(pts[0].x == was->x && pts[0].y == was->y && pts[0].z == was->z)) {
        return planFed(br, index, pts, n, id, along);
    }
    FedPlan plan = planned;
    if (plan.path < 0) return plan;  // (loose for its root's or its draws' sake: as before)
    measureFed(plan, pts, n, along);
    fitFed(br.of(static_cast<std::size_t>(plan.path)), br.paths[static_cast<std::size_t>(plan.path)], plan);
    return plan;
}

// The direction a braid faces out (its frame's normal at the root): out of the scalp there, made
// square to the path; a path leaving along the scalp normal (a ponytail) faces away from the
// head's middle instead; turned `roll` degrees about the path.
Vec3f facing(const ScalpMesh* scalp, const Vec3f& headMiddle, const Guide& g, const BraidPath& p, float roll) {
    const Vec3f t0 = (p.points[1] - p.points[0]).normalized();
    auto square = [&](const Vec3f& v) { return v - t0 * v.dot(t0); };
    Vec3f up;
    if (scalp && scalp->contains(g.root)) up = scalp->normal(g.root);
    else if (scalp && scalp->triangleCount() > 0) up = scalp->closestPoint(p.root).normal;
    up = square(up);
    if (scalp && scalp->pointCount() > 0 && up.length() < 0.5f) {
        const Vec3f away = p.points[p.points.size() / 2] - headMiddle;
        const float far = away.length();
        const Vec3f out = far > 0.0f ? square(away * (1.0f / far)) : Vec3f{};
        if (out.length() > 1e-3f) up = out;
    }
    if (up.length() < 1e-4f) up = anyPerpendicular(t0);
    up = up.normalized();
    if (roll != 0.0f) {
        const float a = roll * kPi / 180.0f;
        up = up * pm::cos(a) + t0.cross(up) * pm::sin(a);
    }
    return up;
}

class BraidOp : public Operator {
public:
    std::string typeName() const override { return op::kBraid; }
    // Version 2: the sections pressed against each other (pressSections; `separate`, `inflate`), a
    // four- and a five-strand braid plaited and a fishtail's halves taking turns (weaveFor), more
    // points a weave step for some styles (Braider::press). A
    // version 1 node braids as before, whatever those two params hold.
    int version() const override { return 2; }
    std::vector<ChannelWrite> channelWrites() const override {
        return {{"braidId", ""}, {"braidFlat", ""}, {"braidSection", ""}, {"braidTail", ""}, {"braidWoven", ""}, {"braidPress", ""}};
    }
    std::string category() const override { return "look"; }
    std::vector<std::string> externalInputs() const override { return {"scalp"}; }
    std::vector<PortSpec> inputs() const override {
        return {{"strands", PortType::Strands}, {"braids", PortType::Guides, true}};
    }
    // The braided strands, and the paths they are braided along (the braids input's, or those it
    // picked among the strands), evenly resampled, for nodes downstream.
    // And its ties (beads, cuffs, hair ties): a short strand along the braid at each, as wide as
    // the braid there, for an Instance node's `along` input. And its sections: the center curve
    // of each section of each braid as it weaves (over and under the others, letting go down the
    // tail), as guides (braidId and braidSection attributes), for nodes downstream: clump centers
    // per section, tubes, curves of the weave itself.
    std::vector<PortSpec> outputs() const override {
        return {output(), {"paths", PortType::Guides}, {"ties", PortType::Strands}, {"sections", PortType::Guides}};
    }
    std::vector<ParamSpec> paramSpecs() const override {
        return {
            {"style", std::string("three"),
             "three, four or five sections, a fishtail, a rope (twisted plies) or a cornrow (raised, on the scalp)", "",
             {"three", "four", "five", "fishtail", "rope", "cornrow"}},
            notAnimatable({"sections", std::int64_t{0}, "A rope's plies (2-8, 0: two) or a fishtail's strips a side "
                                                        "(3-12, 0: six)"})
                .range(0, 12)
                .when("style", {"fishtail", "rope"}),
            ParamSpec{"dutch", false, "Cross the sections under instead of over (a raised braid; a rope twists the other "
                                      "way)"}
                .when("style", {"three", "four", "five", "fishtail", "rope"}),
            ParamSpec{"width", 0.25, "Braid width at its start, scene units (mappable, per braid: at its path's root)", "length"}
                .soft(0, 1),
            {"widthProfile", Ramp::linear(1.0f, 0.55f), "Width along the braid, start to end"},
            ParamSpec{"tightness", 1.0, "How close the crossings are: 1 a natural braid, 2 twice as many in the same "
                                        "length. A crossing is a share of the width long, so a tapering braid's "
                                        "crossings shrink with it (mappable, per braid)"}
                .atLeast(0.05)
                .soft(0.25, 3),
            ParamSpec{"flatness", 0.3, "0: round sections, 1: flat bands (a flatter braid; mappable, per braid)", "fraction"}
                .range(0, 1),
            ParamSpec{"separate", 1.0, "Presses the sections against each other instead of letting them pass through "
                                       "each other: flat where two touch, round where they lie free. 0: plain "
                                       "sections (as fat as inflate makes them) that overlap where they cross. A "
                                       "node made before this stays as it was until you upgrade it (mappable, per braid)",
                      "fraction"}
                .range(0, 1)
                .formerDefault(2, 0.0)
                .addedIn(2)
                .in("Sections"),
            ParamSpec{"inflate", 0.5, "How fat the sections are, whatever separate is: 0 their own size, 1 grown "
                                      "until, pressed, they fill the braid, the grooves between them shallow; above 1 "
                                      "(typed, up to 4) overdriven, pressed flatter against each other. Pressed "
                                      "(separate 1) the braid stays as wide as its width says; with separate 0 the "
                                      "fat sections overlap and reach farther (mappable, per braid)",
                      "fraction"}
                .atLeast(0)
                .soft(0, 1)
                .addedIn(2)
                .in("Sections"),
            ParamSpec{"pancake", 0.0, "Pulls the sections apart and loosens them: the loose, chunky look of a braid "
                                      "pulled out by hand (mappable, per braid)", "fraction"}
                .range(0, 1)
                .in("Loose"),
            ParamSpec{"loops", 0.0, "Share of the sections' stretches between crossings where the hair on the "
                                    "section's outer side bows out of the braid in a loop and goes back in before the "
                                    "next crossing (mappable, per braid)", "fraction"}
                .range(0, 1)
                .in("Loose"),
            ParamSpec{"loopSize", 0.5, "How far a loop reaches out, in braid widths (each a little more or less; mappable, per braid)"}
                .atLeast(0)
                .soft(0, 1.5)
                .in("Loose"),
            ParamSpec{"roll", 0.0, "Turns the braid's face about its path, degrees (it faces out of the scalp, or away "
                                   "from the head; mappable, per braid)", "angle"}
                .soft(-180, 180)
                .adv(),
            ParamSpec{"region", 0.25, "Strands rooted within this distance of a braid's root join it (large: a "
                                      "ponytail's), or with feed of its path (the nearer braid's, when two reach); "
                                      "without a braids input, its own braids are about this far apart", "length"}
                .soft(0, 2),
            ParamSpec{"feed", false, "Pick up strands all along the path, where they grow (cornrows, French braids)"}
                .in("Feed"),
            ParamSpec{"feedUntil", 1.0, "With feed: pick up strands along this share of the path only; past it the "
                                        "braid hangs free (0: only the root region, a ponytail braid)", "fraction"}
                .range(0, 1)
                .in("Feed")
                .when("feed", {"true"}),
            ParamSpec{"gather", 0.15, "Share of the strand spent leaving the scalp into the braid (mappable, per braid)", "fraction"}
                .range(0, 0.9)
                .in("Strands"),
            ParamSpec{"clumpChannel", std::string("clumpId"),
                      "Strands sharing a clump number (a Clump before the Braid writes clumpId) stay together in the "
                      "braid: each clump goes in the section its roots lie on, at one spot of it, its strands round "
                      "that spot (as many as its share of the section's hair takes), so clumps made before braiding "
                      "show inside the sections. Empty, or with feed: each strand its own spot"}
                .readsChannel(true)
                .in("Strands"),
            ParamSpec{"gatherSmoothness", 0.0, "How gradually the weave starts where the hair has gathered: 0 at once; "
                                               "more, the sections begin as one round bundle and their crossings grow "
                                               "over up to four weave steps (mappable, per braid)", "fraction"}
                .range(0, 1)
                .in("Strands"),
            ParamSpec{"end", std::string("fan"),
                      "How the braid ends: fan (over the tail the sections let go and the ends spread), tuft (they "
                      "let go into a gathered tuft, as below a hair tie), bound (woven to the tip, narrowing: sealed "
                      "box-braid ends, no tail) or free (past the weave the hair leaves its sections and runs on "
                      "along the path as one plain bundle, a ponytail the nodes after it style: their params "
                      "mapped to the braidTail channel act on the tail alone)",
                      "", {"fan", "tuft", "bound", "free"}}
                .in("Ends"),
            ParamSpec{"tail", 0.12, "Share at the end where the sections let go (fan, tuft, free; mappable, per braid)", "fraction"}
                .range(0, 0.9)
                .in("Ends")
                .when("end", {"fan", "tuft", "free"}),
            ParamSpec{"tailSmoothness", 0.0, "How gradually the weave ends before the tail: 0 at once; more, the "
                                             "crossings shrink over up to four weave steps into one round bundle, "
                                             "which then lets go as the end says (mappable, per braid)", "fraction"}
                .range(0, 1)
                .in("Ends")
                .when("end", {"fan", "tuft", "free"}),
            ParamSpec{"release", 1.5, "End free: how long the letting go takes past the weave's end, in braid widths. 0: "
                                      "the weave stops at once, as under a hair tie; more: over that length the sections "
                                      "loosen (the weave flattens out, its crossings no tighter) into the one bundle. "
                                      "Never past the path's end (mappable, per braid)"}
                .range(0, 8)
                .in("Ends")
                .when("end", {"free"}),
            notAnimatable({"ties", std::int64_t{0}, "Ties along each braid (beads, cuffs, hair ties): the braid tightens "
                                                    "round each, and the ties output hands them on for an Instance "
                                                    "node's along input. 1: one where the weave ends"})
                .range(0, 32)
                .soft(0, 8)
                .in("Ties"),
            ParamSpec{"tieLength", 0.5, "Each tie's length, in braid widths (mappable, per braid)"}.atLeast(0.05).soft(0.1, 2).in("Ties"),
            ParamSpec{"tieSpacing", 0.0, "From one tie to the next, in braid widths (0: spread along the braid; mappable, per braid)"}
                .atLeast(0)
                .soft(0, 4)
                .in("Ties"),
            ParamSpec{"cinch", 0.35, "How much the braid narrows inside a tie (mappable, per braid)", "fraction"}.range(0, 0.9).in("Ties"),
            notAnimatable({"seed", std::int64_t{0}, "Random seed"}).in("Strands").adv(),
            {"display", std::string("hair"), "hair: the braided strands; paths: each braid's path and its sections' "
                                             "center curves in their place, to see the weave",
             "", {"hair", "paths"}},
        };
    }

    Value evaluate(const EvalContext& ctx, const std::vector<Value>& inputs) const override {
        return evaluateOutputs(ctx, inputs).front();
    }
    std::vector<Value> evaluateOutputs(const EvalContext& ctx, const std::vector<Value>& inputs) const override {
        Value out;
        GuidesPtr paths, sections;
        StrandsPtr ties;
        run(ctx, inputs, nullptr, &out, nullptr, &paths, &ties, nullptr, 0, &sections);
        return {out, paths, ties, sections};
    }
    // Without feed each strand is braided by its own root alone (the nearest path's root within
    // the region): while the paths stay, the strands that changed are braided again (each to as
    // many points as before, or it evaluates in full) and the rest keep the last result's points;
    // the paths are the last ones.
    bool evaluateOutputsIncremental(const EvalContext& ctx, const std::vector<Value>& inputs, const IncrementalInputs& incremental,
                                    std::vector<Value>* outs, std::vector<StrandChanges>* changed) const override {
        if (incremental.previousOutputs.size() != 4) return false;
        IncrementalInputs strands;
        strands.previous = incremental.previousOutputs[0];
        strands.inputs = incremental.inputs;
        Value out;
        StrandChanges made;
        if (!run(ctx, inputs, &strands, &out, &made, nullptr, nullptr)) return false;
        *outs = {out, incremental.previousOutputs[1], incremental.previousOutputs[2], incremental.previousOutputs[3]};
        *changed = {std::move(made), StrandChanges::listed({}), StrandChanges::listed({}), StrandChanges::listed({})};
        return true;
    }
    // Its sections as tubes along its paths (braid_tubes.h), for these inputs: the paths laid as an
    // evaluation lays them (a fed braid's growth too), no strand braided.
    BraidTubes sectionTubes(const EvalContext& ctx, const std::vector<Value>& inputs, const BraidTubeDetail& detail) const {
        BraidTubes tubes;
        tubes.around = std::max(detail.around, 3);
        tubes.perStep = std::max(detail.perStep, 1.0f);
        Value none;
        run(ctx, inputs, nullptr, &none, nullptr, nullptr, nullptr, &tubes, detail.maxPoints);
        return tubes;
    }

private:
    // In full (`paths`: the paths' guides, `ties` the ties' strands, `sections` the sections'
    // center curves as guides, null: not asked), or (with `incremental`) only the strands the input
    // changed in; false when that cannot be done. With `tubes`: only those are made
    // (sectionTubes), once the paths are laid.
    bool run(const EvalContext& ctx, const std::vector<Value>& inputs, const IncrementalInputs* incremental, Value* result,
             StrandChanges* changed, GuidesPtr* paths, StrandsPtr* ties, BraidTubes* tubes = nullptr,
             std::size_t tubePoints = 0, GuidesPtr* sections = nullptr) const {
        const StrandsPtr in = valueAs<StrandsPtr>(inputs.at(0));
        if (!in) throw GraphError("Braid: strands input not connected");
        GuidesPtr braids = valueAs<GuidesPtr>(inputs.at(1));
        auto handOn = [&] {  // nothing to braid along: the graph passes the input's changes on
            if (incremental) return false;
            *result = in;
            if (paths) *paths = GuidesPtr(std::make_shared<GuideSet>());
            if (ties) *ties = StrandsPtr(std::make_shared<StrandSet>());
            if (sections) *sections = GuidesPtr(std::make_shared<GuideSet>());
            return true;
        };
        const ScalpMesh* scalp = valueAs<MeshPtr>(ctx.external("scalp")).get();
        const bool own = !braids;
        ProfilePhases phase("braid.pickPaths");
        if (own) {  // no braids input: paths of its own, strands about `region` apart
            if (incremental) return false;
            const float spacing = static_cast<float>(params().getDouble("region"));
            if (!(spacing > 0.0f) || in->empty()) return handOn();
            auto picked = std::make_shared<GuideSet>();
            for (const std::size_t s : pickSpreadStrands(*in, scalp, spacing, static_cast<std::uint64_t>(params().getInt("seed")))) {
                const Vec3f* pts = in->strandPoints(s);
                picked->add(in->ids()[s], std::vector<Vec3f>(pts, pts + in->strandPointCount(s))).root = in->roots()[s];
            }
            braids = GuidesPtr(picked);
        }
        if (braids->empty()) return handOn();

        phase.next("braid.setup");
        Braider br;
        const bool pressing = nodeVersion() >= 2;
        br.style = styleFrom(params().getString("style"));
        br.taper = params().getRamp("widthProfile");
        br.taperTangents = br.taper.tangents();
        const float region = static_cast<float>(params().getDouble("region"));
        // (Paths of its own, about region apart, cover the scalp: every strand joins the nearest.)
        br.region2 = own && !params().getBool("feed") ? std::numeric_limits<float>::infinity() : region * region;
        br.end = endFrom(params().getString("end"));
        br.ties = static_cast<int>(params().getInt("ties"));
        br.seed = static_cast<std::uint64_t>(params().getInt("seed"));
        const bool feed = params().getBool("feed");
        br.feeds = feed;
        if (feed) br.feedUntil = static_cast<float>(params().getDouble("feedUntil"));
        // The settings its numbers give (`number`: a param's number, or its value at a path).
        const auto sectionCount = params().getInt("sections");
        const bool dutch = params().getBool("dutch");
        auto configure = [&](Braider& b, const auto& number) {
            b.weave = weaveFor(b.style, sectionCount, dutch, number("flatness"), pressing);
            b.width = number("width");
            b.tightness = std::max(number("tightness"), 0.05f);
            b.roll = number("roll");
            b.gather = number("gather");
            b.tail = b.end == End::Bound ? 0.0f : number("tail");
            b.release = std::max(number("release"), 0.0f);
            b.pancake = number("pancake");
            b.loops = number("loops");
            b.loopSize = number("loopSize");
            b.tieLength = number("tieLength");
            b.tieSpacing = number("tieSpacing");
            b.cinch = number("cinch");
            b.gatherSmoothness = std::clamp(number("gatherSmoothness"), 0.0f, 1.0f);
            b.tailSmoothness = b.tail > 0.0f ? std::clamp(number("tailSmoothness"), 0.0f, 1.0f) : 0.0f;
            if (b.style != Style::Cornrow) {  // (a cornrow lies on the scalp already)
                // Version 2: the scalp collides with every braid; version 1 raised a fed one alone.
                if (pressing) b.clearLift = -b.weave.lowest();
                else if (feed) b.feedLift = -b.weave.lowest();
            }
            if (pressing) b.press(number("inflate"), number("separate"));
        };
        // Mapped params read per path, at its root (its guide's; a path of its own: its strand's):
        // each path braided with its own settings (Braider::perPath).
        static constexpr const char* kPerPath[] = {"width", "tightness", "flatness", "separate", "inflate", "pancake",
                                                   "loops", "loopSize", "roll", "gather", "gatherSmoothness", "tail",
                                                   "tailSmoothness", "release", "tieLength", "tieSpacing", "cinch"};
        bool perPath = false;
        for (const char* name : kPerPath) perPath = perPath || paramMapped(params(), name);
        const Braider base = br;  // (before its numbers: what each path's settings start from)
        configure(br, [&](const char* name) { return static_cast<float>(params().getDouble(name)); });
        std::map<std::string, std::unique_ptr<ScalarField>> pathFields;
        if (perPath) {
            const GuidesPtr given = valueAs<GuidesPtr>(inputs.at(1));  // (guide maps read on the braids input)
            for (const char* name : kPerPath) pathFields[name] = std::make_unique<ScalarField>(params(), name, scalp, given.get());
        }
        std::vector<std::shared_ptr<Braider>> ownSettings;  // per path, with perPath (br.perPath once laid)
        const bool showPaths = params().getString("display") == "paths";

        // The head's middle (the scalp's points' mean): a braid leaving along the scalp normal faces
        // away from it.
        Vec3f headMiddle;
        if (scalp && scalp->pointCount() > 0) {
            for (const Vec3f& q : scalp->points()) headMiddle = headMiddle + q;
            headMiddle = headMiddle * (1.0f / static_cast<float>(scalp->pointCount()));
        }
        // Braid paths, evenly resampled, with frames facing out.
        phase.next("braid.paths");
        for (const Guide& g : braids->guides()) {
            if (g.points.size() < 2) continue;
            BraidPath p;
            const std::size_t samples = std::max<std::size_t>(64, g.points.size() * 4);
            p.points = resampleByArcLength(g.points.data(), g.points.size(), samples);
            p.length = arcLength(p.points.data(), p.points.size());
            if (!(p.length > 1e-6f)) continue;  // also NaN
            p.root = p.points.front();
            p.id = g.id;
            p.guideRoot = g.root;
            // Its own settings (mapped: its values at its root), else the node's.
            std::shared_ptr<Braider> settings;
            if (perPath) {
                settings = std::make_shared<Braider>(base);
                configure(*settings, [&](const char* name) {
                    const float v = pathFields.at(name)->atRoot(g.root);  // (within the param's limits, as its number)
                    return std::isfinite(v) ? static_cast<float>(params().clamped(name, v)) : static_cast<float>(params().getDouble(name));
                });
            }
            const Braider& pb = settings ? *settings : br;
            p.frames = rotationMinimizingFrames(p.points.data(), p.points.size(), facing(scalp, headMiddle, g, p, pb.roll));
            if (pb.clearLift > 0.0f && scalp && scalp->triangleCount() > 0) {
                p.overScalp.resize(p.points.size());
                for (std::size_t k = 0; k < p.points.size(); ++k) {
                    const SurfacePoint sp = scalp->closestPoint(p.points[k]);
                    // Below the scalp counts (a guide a simulation let sink in: the braid comes out
                    // all the same), down to a braid width (deeper, the closest point may lie on
                    // the scalp's far side). (No face found: far above; finite, as sample() blends it.)
                    p.overScalp[k] = sp.root.face < 0 ? 1e30f : std::max((p.points[k] - sp.position).dot(sp.normal), -pb.width);
                }
            }
            // The weave travelled along the path: the steps a unit of length integrated.
            const std::size_t count = p.points.size();
            const float step = p.length / static_cast<float>(count - 1);
            p.rate.resize(count);
            p.steps.assign(count, 0.0f);
            for (std::size_t k = 0; k < count; ++k) p.rate[k] = pb.rateAt(static_cast<float>(k) / static_cast<float>(count - 1));
            for (std::size_t k = 1; k < count; ++k) p.steps[k] = p.steps[k - 1] + 0.5f * (p.rate[k - 1] + p.rate[k]) * step;
            p.repeats = p.steps.back();
            pb.placeTies(p);
            pb.layBundle(p);
            br.paths.push_back(std::move(p));
            if (settings) ownSettings.push_back(std::move(settings));
        }
        if (br.paths.empty()) return handOn();
        if (paths) {
            auto made = std::make_shared<GuideSet>();
            for (const BraidPath& p : br.paths) made->add(p.id, p.points).root = p.guideRoot;
            *paths = GuidesPtr(made);
        }

        // The clumps coming in (clumpChannel, without feed): each kept together in the braid.
        const std::string clumpName = params().getString("clumpChannel");
        const std::vector<float>* clumps = feed || clumpName.empty() ? nullptr : in->strandChannel(clumpName);
        if (clumps && clumps->size() != in->strandCount()) clumps = nullptr;

        // Incrementally: the same paths, no feed, no earlier Braid's ids (numbered on from their
        // highest), the hair shown, no clumps kept (a clump's place is all its strands').
        phase.next("braid.channelsBefore");
        const StrandChanges* changes = nullptr;
        StrandsPtr last;
        if (incremental) {
            changes = someStrandsChanged(*incremental, 0);
            last = valueAs<StrandsPtr>(incremental->previous);
            if (!changes || !last || last->strandCount() != in->strandCount() || feed || showPaths ||
                !inputUnchanged(*incremental, 1) || in->strandChannel("braidId") || clumps) {
                return false;
            }
            if (changes->none()) {
                *result = last;
                *changed = *changes;
                return true;
            }
        }

        // A Braid node after another keeps the earlier braids' ids and numbers its own on from
        // them, so every braid in the groom has an id of its own (the Cards node's card sets).
        int firstId = 0;
        std::vector<float> earlierIds, earlierFlat, earlierSections;
        if (const std::vector<float>* had = in->strandChannel("braidId")) {
            earlierIds = *had;
            if (const std::vector<float>* flat = in->strandChannel("braidFlat")) earlierFlat = *flat;
            if (const std::vector<float>* sections = in->strandChannel("braidSection")) earlierSections = *sections;
            float highest = -1.0f;  // of the core strands (preview_core.h): the same at every preview density
            const PreviewCore core(*in);
            for (std::size_t s = 0; s < earlierIds.size(); ++s) {
                if (core.core(s)) highest = std::max(highest, std::floor(earlierIds[s]));
            }
            firstId = static_cast<int>(groupNumberBase(static_cast<std::int64_t>(highest)));
        }
        BraidChannels channels{nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, firstId};
        std::vector<float> numbers;
        if (own) {
            std::vector<std::uint64_t> ids(br.paths.size());
            for (std::size_t b = 0; b < br.paths.size(); ++b) ids[b] = br.paths[b].id;
            numbers = stableGroupNumbers(ids);
            channels.numbers = &numbers;
        }
        if (scalp && scalp->triangleCount() > 0) {
            br.overScalp.scalp = scalp;
            // (at least a hundredth of the width: a share of the braid, whatever the scene's scale)
            br.overScalp.clearance = 0.25f * br.width * std::max(br.taper(0.0f), 0.04f);
            for (const std::shared_ptr<Braider>& o : ownSettings) {
                o->overScalp.scalp = scalp;
                o->overScalp.clearance = 0.25f * o->width * std::max(o->taper(0.0f), 0.04f);
            }
        }
        for (std::shared_ptr<Braider>& o : ownSettings) br.perPath.push_back(o);

        const std::size_t count = in->strandCount();
        std::vector<std::size_t> counts(count);
        // Feed's lookups: of the paths' points and roots, which stay (both passes').
        phase.next("braid.feedPlan");
        const std::unique_ptr<const FeedIndex> feedIndex = feed ? std::make_unique<const FeedIndex>(br.paths, br.feedUntil) : nullptr;
        std::vector<FedPlan> plans;  // feed: each strand's, on its points as they came
        if (feed) {
            // Where each strand joins (on its points as they came), for the points it needs and
            // how much of each braid's hair it holds where.
            const FeedIndex& index = *feedIndex;
            plans.resize(count);
            const PreviewCore core(*in);
            parallelFor(0, count, [&](std::size_t s) {
                // (The tubes alone asked: only the strands the braid's growth counts, below.)
                if (tubes && !core.core(s)) return;
                thread_local std::vector<float> along;
                const std::size_t n = in->strandPointCount(s);
                plans[s] = planFed(br, index, in->strandPoints(s), n, in->ids()[s], along);
                counts[s] = n;
                const FedPlan& p = plans[s];
                if (p.path < 0) return;
                const BraidPath& path = br.paths[static_cast<std::size_t>(p.path)];
                const float braided = std::max(p.strandLength * p.squeeze - p.approach, 0.0f);
                const float steps = path.weaveAt(p.joinAt + braided) - path.weaveAt(p.joinAt);
                const float share = p.strandLength * p.squeeze > 0.0f ? braided / (p.strandLength * p.squeeze) : 1.0f;
                counts[s] = br.sampling.count(n, steps, share);
            });
            // The braid thickens with the hair it holds: the share of each braid's hair (its core
            // strands: the same at every preview density) joined by each of its points.
            std::vector<std::vector<double>> held(br.paths.size());
            for (std::size_t b = 0; b < br.paths.size(); ++b) held[b].assign(br.paths[b].points.size(), 0.0);
            for (std::size_t s = 0; s < count; ++s) {
                if (plans[s].path >= 0 && core.core(s)) held[static_cast<std::size_t>(plans[s].path)][plans[s].joinK] += 1.0;
            }
            for (std::size_t b = 0; b < br.paths.size(); ++b) {
                std::vector<double>& h = held[b];
                for (std::size_t k = 1; k < h.size(); ++k) h[k] += h[k - 1];
                if (!(h.back() > 0.0)) continue;
                std::vector<float>& grown = br.paths[b].grown;
                grown.resize(h.size());
                for (std::size_t k = 0; k < h.size(); ++k) grown[k] = std::sqrt(std::max(static_cast<float>(h[k] / h.back()), 0.05f));
            }
        }
        phase.next("braid.ties");
        br.layTieAxes();
        if (ties) *ties = tieStrands(br, channels);
        if (sections) *sections = sectionGuides(br, channels);
        if (tubes) {
            br.loops = 0.0f;  // (hair let out of its section: not the section's)
            for (const std::shared_ptr<Braider>& o : ownSettings) o->loops = 0.0f;
            phase.next("braid.tubes");
            sectionTubesOf(br, channels, feed, tubePoints, *tubes);
            return true;
        }
        if (showPaths) {
            *result = pathStrands(br, channels);
            return true;
        }

        phase.next("braid.incremental");
        if (incremental) {
            // Each changed strand braided again into as many points as it had (as the rest keep
            // theirs, their input as it was), else in full.
            std::atomic<bool> moved{false};
            std::vector<int> joined(count, -1);  // each changed strand's path (pointsFor's)
            forEachListed(changes->strands, [&](std::size_t s) {
                if (br.pointsFor(*in, s, &joined[s]) != last->strandPointCount(s)) moved = true;
            });
            if (moved) return false;
            auto made = copyOfLast(last, *incremental);
            const bool widths = in->widths().size() == in->pointCount();
            std::vector<float>& outWidths = made->mutableWidths();
            std::vector<std::vector<float>*> outPoint;
            std::vector<const std::vector<float>*> inPoint;
            for (const std::string& name : in->pointChannelNames()) {
                outPoint.push_back(made->copyPointChannelFrom(name, *last));
                inPoint.push_back(in->pointChannel(name));
                if (!outPoint.back()) return false;
            }
            std::vector<std::pair<const std::vector<float>*, std::vector<float>*>> strandValues;
            for (const std::string& name : in->strandChannelNames()) {
                std::vector<float>* to = made->copyStrandChannelFrom(name, *last);
                if (!to) return false;
                strandValues.emplace_back(in->strandChannel(name), to);
            }
            channels.id = made->copyStrandChannelFrom("braidId", *last);
            channels.flat = made->copyStrandChannelFrom("braidFlat", *last);
            channels.tail = made->copyPointChannelFrom("braidTail", *last);
            channels.section = made->copyStrandChannelFrom("braidSection", *last);
            channels.woven = made->copyPointChannelFrom("braidWoven", *last);
            channels.press = made->copyPointChannelFrom("braidPress", *last);
            if (!channels.id || !channels.flat || !channels.tail || !channels.section || !channels.woven || !channels.press) return false;
            forEachListed(changes->strands, [&](std::size_t s) {
                for (const auto& [from, to] : strandValues) (*to)[s] = (*from)[s];
                resampleStrand(*in, *made, s, false, widths, inPoint, outPoint, outWidths);
                (*channels.id)[s] = -1.0f;
                (*channels.flat)[s] = 0.0f;
                (*channels.section)[s] = -1.0f;
                std::fill_n(channels.tail->begin() + static_cast<std::ptrdiff_t>(made->strandBegin(s)), made->strandPointCount(s), 0.0f);
                std::fill_n(channels.woven->begin() + static_cast<std::ptrdiff_t>(made->strandBegin(s)), made->strandPointCount(s), 0.0f);
                std::fill_n(channels.press->begin() + static_cast<std::ptrdiff_t>(made->strandBegin(s)), made->strandPointCount(s), 0.0f);
                braidOne(br, *in, joined[s], *made, s, channels);
            });
            *result = StrandsPtr(made);
            *changed = *changes;
            return true;
        }

        phase.next("braid.countPoints");
        std::vector<int> joined;  // without feed: each strand's path (pointsFor's)
        if (!feed) {
            joined.assign(count, -1);
            parallelFor(0, count, [&](std::size_t s) { counts[s] = br.pointsFor(*in, s, &joined[s]); });
        }
        // The strands given the points they need (each evenly along itself, as Resample does).
        phase.next("braid.resample");
        bool adds = false;
        for (std::size_t s = 0; s < count && !adds; ++s) adds = counts[s] != in->strandPointCount(s);
        // (A set of its own when made here: nobody else holds it.)
        const std::shared_ptr<StrandSet> out =
            adds ? std::const_pointer_cast<StrandSet>(resampleStrands(*in, counts)) : std::make_shared<StrandSet>(*in);
        phase.next("braid.addChannels");
        channels.id = &out->addStrandChannel("braidId", -1.0f);
        channels.flat = &out->addStrandChannel("braidFlat", 0.0f);
        channels.tail = &out->addPointChannel("braidTail", 0.0f);
        channels.section = &out->addStrandChannel("braidSection", -1.0f);
        channels.woven = &out->addPointChannel("braidWoven", 0.0f);
        channels.press = &out->addPointChannel("braidPress", 0.0f);
        if (!earlierIds.empty()) {
            *channels.id = earlierIds;
            if (earlierFlat.size() == channels.flat->size()) *channels.flat = earlierFlat;
            if (earlierSections.size() == channels.section->size()) *channels.section = earlierSections;
        }
        phase.next("braid.weaveStrands");
        if (feed) {
            const FeedIndex& index = *feedIndex;
            parallelFor(0, count, [&](std::size_t s) {
                thread_local std::vector<float> along;
                Vec3f* pts = out->strandPoints(s);
                const std::size_t n = out->strandPointCount(s);
                // (Where it joins: as planned on the points it came with, its root where it was.)
                const FedPlan plan = replanFed(br, index, plans[s], in->strandPointCount(s) >= 2 ? in->strandPoints(s) : nullptr, pts,
                                               n, out->ids()[s], along);
                if (plan.path < 0) return;
                channels.set(s, plan.path, br.style);
                (*channels.section)[s] = static_cast<float>(feedOne(br, plan, pts, n, along, channels.tail->data() + out->strandBegin(s),
                                                                    channels.woven->data() + out->strandBegin(s),
                                                                    channels.press->data() + out->strandBegin(s)));
            });
        } else {
            // Path by path: a path's strands of the points its weave needs all lie at the same
            // stations along it, worked out once (a table, kept while its strands are braided:
            // tableFor) and read by each. A path of few strands (the table is no less work than
            // they are), and a strand of other points, work theirs out: the same values.
            std::vector<std::size_t> first(br.paths.size() + 1, 0);
            for (std::size_t s = 0; s < count; ++s) {
                if (joined[s] >= 0) ++first[static_cast<std::size_t>(joined[s]) + 1];
            }
            for (std::size_t b = 0; b < br.paths.size(); ++b) first[b + 1] += first[b];
            std::vector<std::uint32_t> ofPath(first.back());
            {
                std::vector<std::size_t> next(first.begin(), first.end() - 1);
                for (std::size_t s = 0; s < count; ++s) {
                    if (joined[s] >= 0) ofPath[next[static_cast<std::size_t>(joined[s])]++] = static_cast<std::uint32_t>(s);
                }
            }
            const std::size_t fewest = 2 * static_cast<std::size_t>(br.weave.sections);
            parallelFor(0, br.paths.size(), [&](std::size_t b) {
                const std::size_t strands = first[b + 1] - first[b];
                if (!strands) return;
                const BraidPath& path = br.paths[b];
                PathTable table;
                const std::size_t points = br.sampling.count(2, path.repeats, 1.0f);
                if (strands > fewest && points > 2) table = br.of(b).tableFor(path, points);
                const PathTable* stations = table.count ? &table : nullptr;
                // (the clumps coming in kept together: clumpChannel)
                const ClumpPlaces places =
                    clumps ? br.of(b).clumpPlaces(path, *in, ofPath.data() + first[b], strands, *clumps) : ClumpPlaces{};
                parallelFor(0, strands, [&](std::size_t k) {
                    const std::size_t s = ofPath[first[b] + k];
                    braidOne(br, *in, joined[s], *out, s, channels, stations, static_cast<int>(b), clumps ? &places : nullptr, clumps);
                }, 64);
            }, 1);
            // (A strand that joined none: braidOne looks again only if its root moved.)
            parallelFor(0, count, [&](std::size_t s) {
                if (joined[s] < 0) braidOne(br, *in, joined[s], *out, s, channels);
            });
        }
        *result = StrandsPtr(out);
        return true;
    }

    // Without feed: strand s of `out` braided along the path it joins, if any: `joined`, the path
    // found for its root in `in` as its points were counted (pointsFor), while its root is where
    // it was then (as resampling leaves it; else found again). `table`: the stations of path
    // `tablePath` (Braider::tableFor), read when the strand is braided along that path with the
    // table's points.
    // `places` (with `clumps`, the clump channel): the clumps of path `tablePath` kept together.
    static void braidOne(const Braider& br, const StrandSet& in, int joined, StrandSet& out, std::size_t s,
                         const BraidChannels& channels, const PathTable* table = nullptr, int tablePath = -1,
                         const ClumpPlaces* places = nullptr, const std::vector<float>* clumps = nullptr) {
        Vec3f* pts = out.strandPoints(s);
        const std::size_t n = out.strandPointCount(s);
        if (n < 2) return;
        const Vec3f* was = in.strandPointCount(s) >= 2 ? in.strandPoints(s) : nullptr;
        const int best = was && pts[0].x == was->x && pts[0].y == was->y && pts[0].z == was->z ? joined : br.pathOf(pts[0]);
        if (best < 0) return;
        StrandDraws draws = br.drawsFor(out.ids()[s]);
        std::int64_t key = 0;
        if (places && clumps && best == tablePath && clumpKey((*clumps)[s], &key)) {
            if (const auto it = places->find(key); it != places->end()) it->second.place(draws);
        }
        channels.set(s, best, br.style);
        const int section = br.of(static_cast<std::size_t>(best)).braidAlong(pts, n, br.paths[static_cast<std::size_t>(best)], draws,
                                          channels.tail ? channels.tail->data() + out.strandBegin(s) : nullptr,
                                          channels.woven ? channels.woven->data() + out.strandBegin(s) : nullptr,
                                          table && best == tablePath && n == table->count ? table : nullptr,
                                          channels.press ? channels.press->data() + out.strandBegin(s) : nullptr);
        if (channels.section) (*channels.section)[s] = static_cast<float>(section);
    }

    // Feed mode: a strand is drawn in from its root to where it joins the path, and runs along it
    // by its own length.
    // Returns the section it joins.
    static int feedOne(const Braider& all, const FedPlan& plan, Vec3f* pts, std::size_t n, const std::vector<float>& along,
                       float* tail, float* woven, float* press) {
        const BraidPath& path = all.paths[static_cast<std::size_t>(plan.path)];
        const Braider& br = all.of(static_cast<std::size_t>(plan.path));  // (its path's settings)
        // Picked up along the path: the outer section on its side, as a braider adds hair; from
        // the root region: the section on its side of the region.
        const Vec3f offset = pts[0] - path.points[plan.joinK];
        const Frame& fr = path.frames[plan.joinK];
        const float steps = path.weaveAt(plan.joinAt);
        int section = plan.draws.lobe;
        if (plan.region) {
            section = br.sectionOfRoot(offset, fr, steps, section);
        } else if (const float side = offset.dot(fr.binormal); side != 0.0f) {
            section = br.sectionAcross(side < 0.0f ? 0.0f : 1.0f, steps);
        }
        // From the root straight to where it joins its section, held over the scalp, then along it,
        // turning from the one into the other over a stretch about the join (half a braid width
        // either side, less on a short way in): a curve into the braid, not a corner.
        const Vec3f root = pts[0];
        const StrandSpot spot = br.spotOf(plan.draws);
        const Vec3f joins = br.braidedAt(path, plan.joinAt, section, plan.draws);
        const Vec3f way = (joins - root) / plan.approach;
        const float turn = std::min(0.5f * br.widthAt(path, plan.joinAt), 0.4f * plan.approach);
        tail[0] = 0.0f;
        woven[0] = 0.0f;
        press[0] = 0.0f;
        for (std::size_t i = 1; i < n; ++i) {
            const float fitted = along[i] * plan.squeeze;
            Vec3f straight = root + way * fitted;
            if (fitted < plan.approach) {
                const float u = fitted / plan.approach;
                // pressed down, rising into the braid
                br.overScalp.apply(straight, 0.05f + 0.9f * u * u, fitted, plan.approach - fitted);
            }
            const float at = plan.joinAt + fitted - plan.approach;
            const float w = turn > 0.0f ? smoothstep(plan.approach - turn, plan.approach + turn, fitted)
                                        : (fitted < plan.approach ? 0.0f : 1.0f);
            woven[i] = std::max(w, 0.0f);
            if (w <= 0.0f) {
                pts[i] = straight;
                tail[i] = 0.0f;
                press[i] = 0.0f;
                continue;
            }
            const PathStation st = br.stationAt(path, std::max(at, 0.0f));
            float pressed = 0.0f;
            const Vec3f braided = br.braidedFrom(st, br.sectionAt(section, st), section, plan.draws, spot, &pressed);
            pts[i] = straight + (braided - straight) * w;
            press[i] = pressed * w;
            // (in the braid `at` is not below 0: the station's tail is tailAt's there)
            tail[i] = fitted < plan.approach ? 0.0f : st.letGo;
        }
        return section;
    }

    // The ties: a short strand along each braid at each of its ties (its middle raised as the braid
    // is), as wide as the braid there at every point, rooted where the braid is, with its braidId.
    static StrandsPtr tieStrands(const Braider& br, const BraidChannels& numbering) {
        constexpr std::size_t kPoints = 7;
        std::size_t count = 0;
        for (const BraidPath& p : br.paths) count += p.ties.size();
        auto out = std::make_shared<StrandSet>();
        if (!count) return StrandsPtr(out);
        out->resize(std::vector<std::size_t>(count, kPoints), StrandSet::kLeavePositions);
        std::vector<StrandId>& ids = out->mutableIds();
        std::vector<StrandRoot>& roots = out->mutableRoots();
        std::vector<float>& widths = out->mutableWidths();
        std::vector<float>& id = out->addStrandChannel("braidId", -1.0f);
        std::size_t s = 0;
        for (std::size_t b = 0; b < br.paths.size(); ++b) {
            const BraidPath& path = br.paths[b];
            const Braider& own = br.of(b);
            for (std::size_t k = 0; k < path.ties.size(); ++k, ++s) {
                ids[s] = hashCombine(path.id ^ 0x71E5B7A1ull, static_cast<std::uint64_t>(k));
                roots[s] = path.guideRoot;
                id[s] = numbering.number(static_cast<int>(b));
                Vec3f* pts = out->strandPoints(s);
                const std::size_t begin = out->strandBegin(s);
                // Straight, as a rigid bead lies (the hair is held round this axis).
                const Vec3f& from = path.tieFrom[k];
                const Vec3f& direction = path.tieDirection[k];
                const float length = (own.axisAt(path, path.ties[k] + path.tieHalf) - from).length();
                for (std::size_t j = 0; j < kPoints; ++j) {
                    const float u = static_cast<float>(j) / static_cast<float>(kPoints - 1);
                    const float at = path.ties[k] - path.tieHalf + 2.0f * path.tieHalf * u;
                    pts[j] = from + direction * (length * u);
                    widths[begin + j] = kTieBore * own.braidWidthAt(path, at);  // snug round the hair held in it
                }
            }
        }
        return StrandsPtr(out);
    }

    // Each path's sections as tubes over the braid's woven stretch (BraidTubes): rings at stations
    // along the path (out.perStep a weave step, fewer within `maxPoints`: BraidTubeDetail), each
    // ring's points the braid's own for a strand on its section's rim (braidedFrom: held by the ties
    // as the hair is, and round the section's pressed outline). Laid out first, then filled a path
    // at a time in parallel.
    static void sectionTubesOf(const Braider& br, const BraidChannels& numbering, bool feed, std::size_t maxPoints, BraidTubes& out) {
        const std::size_t sections = static_cast<std::size_t>(std::max(br.weave.sections, 0));
        // The woven stretch of each path, and how many weave steps it holds (none: no tube).
        struct Stretch {
            float from = 0.0f, to = 0.0f, steps = 0.0f;
            std::size_t rings = 0, firstTube = 0, firstPoint = 0, firstQuad = 0;
        };
        std::vector<Stretch> stretches(br.paths.size());
        for (std::size_t b = 0; b < br.paths.size(); ++b) {
            const BraidPath& path = br.paths[b];
            const Braider& own = br.of(b);
            Stretch& s = stretches[b];
            s.from = feed ? 0.0f : own.gather * path.length;
            s.to = own.weaveEnd(path) + (own.end == End::Free ? own.releaseLength(path) : 0.0f);  // (free: to where the sections have let go)
            s.steps = s.to > s.from ? path.weaveAt(s.to) - path.weaveAt(s.from) : -1.0f;
        }
        auto ringsOf = [](float steps, float perStep) {
            return static_cast<std::size_t>(std::clamp(std::ceil(perStep * steps), 1.0f, 4095.0f)) + 1;
        };
        auto pointsAt = [&](float perStep, std::size_t around) {
            std::size_t points = 0;
            for (const Stretch& s : stretches) {
                if (s.steps >= 0.0f) points += ringsOf(s.steps, perStep) * sections * around;
            }
            return points;
        };
        // Within the budget: fewer rings a step first, then fewer points round, not under the least.
        std::size_t around = static_cast<std::size_t>(out.around);
        float perStep = out.perStep;
        if (maxPoints > 0) {
            std::size_t points = pointsAt(perStep, around);
            if (points > maxPoints && perStep > BraidTubeDetail::kLeastPerStep) {
                perStep = std::max(perStep * static_cast<float>(maxPoints) / static_cast<float>(points), BraidTubeDetail::kLeastPerStep);
                points = pointsAt(perStep, around);
            }
            const auto least = static_cast<std::size_t>(BraidTubeDetail::kLeastAround);
            if (points > maxPoints && around > least) {
                around = std::max(static_cast<std::size_t>(static_cast<double>(around) * static_cast<double>(maxPoints) / static_cast<double>(points)), least);
                points = pointsAt(perStep, around);
            }
            // Thousands of braids (a node picking its own paths among dense hair): past
            // kMostOverBudget budgets even so, fewer rings still, down to a tube's two ends.
            const std::size_t most = BraidTubeDetail::kMostOverBudget * maxPoints;
            if (points > most) perStep *= static_cast<float>(most) / static_cast<float>(points);
        }
        out.around = static_cast<int>(around);
        out.perStep = perStep;
        out.pressed = br.pressed != nullptr;
        std::size_t tubeCount = 0, pointCount = 0, quadCount = 0;
        for (Stretch& s : stretches) {
            if (s.steps < 0.0f) continue;
            s.rings = ringsOf(s.steps, perStep);
            s.firstTube = tubeCount;
            s.firstPoint = pointCount;
            s.firstQuad = quadCount;
            tubeCount += sections;
            pointCount += s.rings * sections * around;
            quadCount += (s.rings - 1) * sections * around;
        }
        out.tubes.assign(tubeCount, {});
        out.points.assign(pointCount, {});
        out.quads.assign(quadCount, {});
        out.quadTube.assign(quadCount, 0);
        std::vector<std::array<float, 2>> turn(around);
        for (std::size_t k = 0; k < around; ++k) {
            const float a = kTwoPi * static_cast<float>(k) / static_cast<float>(around);
            turn[k] = {pm::cos(a), pm::sin(a)};
        }
        parallelFor(0, br.paths.size(), [&](std::size_t b) {
            const Stretch& stretch = stretches[b];
            if (stretch.rings == 0) return;
            const BraidPath& path = br.paths[b];
            const Braider& own = br.of(b);  // (its settings)
            const float loose = 1.0f + 0.6f * own.pancake;
            const float halfWidth = own.weave.halfWidth * loose, halfThick = own.weave.halfThick * loose;
            const std::size_t rings = stretch.rings;
            const float from = stretch.from, to = stretch.to;
            StrandDraws centered;  // no spot of its own, no fan, no wander
            centered.fan = 0.0f;
            std::vector<PathStation> stations(rings);
            for (std::size_t j = 0; j < rings; ++j) {
                stations[j] = own.stationAt(path, from + (to - from) * static_cast<float>(j) / static_cast<float>(rings - 1));
            }
            for (std::size_t section = 0; section < sections; ++section) {
                const std::size_t index = stretch.firstTube + section;
                BraidTubes::Tube& tube = out.tubes[index];
                tube.path = static_cast<int>(b);
                tube.braidId = numbering.number(static_cast<int>(b));
                tube.section = static_cast<int>(section);
                tube.root = path.guideRoot;
                tube.firstPoint = stretch.firstPoint + section * rings * around;
                tube.firstQuad = stretch.firstQuad + section * (rings - 1) * around;
                tube.at.resize(rings);
                tube.centers.resize(rings);
                tube.across.resize(rings);
                tube.thick.resize(rings);
                Vec3f* points = out.points.data() + tube.firstPoint;
                std::array<std::uint32_t, 4>* quads = out.quads.data() + tube.firstQuad;
                std::uint32_t* quadTube = out.quadTube.data() + tube.firstQuad;
                for (std::size_t j = 0; j < rings; ++j) {
                    const PathStation& st = stations[j];
                    const SectionStation sec = own.sectionAt(static_cast<int>(section), st);
                    const float held = (1.0f - st.letGo) * st.wid;
                    tube.at[j] = from + (to - from) * static_cast<float>(j) / static_cast<float>(rings - 1);
                    tube.centers[j] = own.braidedFrom(st, sec, static_cast<int>(section), centered, StrandSpot{});
                    tube.across[j] = st.frame->toWorld(sec.across * (halfWidth * held));
                    tube.thick[j] = st.frame->toWorld(sec.thick * (halfThick * held));
                    for (std::size_t k = 0; k < around; ++k) {
                        StrandSpot spot;
                        spot.across = turn[k][0] * halfWidth;
                        spot.thick = turn[k][1] * halfThick;
                        spot.ray = static_cast<float>(k) * static_cast<float>(Pressed::kAround) / static_cast<float>(around);
                        points[j * around + k] = own.braidedFrom(st, sec, static_cast<int>(section), centered, spot);
                    }
                    if (j == 0) continue;
                    const auto ring = static_cast<std::uint32_t>(tube.firstPoint + j * around), last = static_cast<std::uint32_t>(ring - around);
                    for (std::uint32_t k = 0; k < around; ++k) {
                        const std::uint32_t next = (k + 1) % static_cast<std::uint32_t>(around);
                        quads[(j - 1) * around + k] = {last + k, ring + k, ring + next, last + next};
                        quadTube[(j - 1) * around + k] = static_cast<std::uint32_t>(index);
                    }
                }
            }
        }, 1);
    }

    // The sections output: each braid's sections' center curves (pathStrands' without the path),
    // as guides rooted where the braid's path is, each with its braidId and braidSection
    // attributes and an ID of its own (the path's with its section's number).
    static GuidesPtr sectionGuides(const Braider& br, const BraidChannels& numbering) {
        auto out = std::make_shared<GuideSet>();
        const StrandsPtr curves = pathStrands(br, numbering);
        const std::vector<float>* id = curves->strandChannel("braidId");
        const std::vector<float>* section = curves->strandChannel("braidSection");
        const std::size_t perPath = static_cast<std::size_t>(br.weave.sections) + 1;
        for (std::size_t s = 0; s < curves->strandCount(); ++s) {
            if ((*section)[s] < 0.0f) continue;  // (the path itself: the paths output's)
            const Vec3f* p = curves->strandPoints(s);
            Guide& g = out->add(curves->ids()[s], std::vector<Vec3f>(p, p + curves->strandPointCount(s)));
            g.root = br.paths[s / perPath].guideRoot;
            g.attributes["braidId"] = (*id)[s];
            g.attributes["braidSection"] = (*section)[s];
        }
        return GuidesPtr(out);
    }

    // display paths: each braid's path and the center curve of each of its sections, as strands
    // (braidId the braid's, braidSection -1 for the path, else the section).
    static StrandsPtr pathStrands(const Braider& br, const BraidChannels& numbering) {
        const std::size_t perPath = static_cast<std::size_t>(br.weave.sections) + 1;
        std::vector<std::size_t> counts;
        for (const BraidPath& p : br.paths) {
            const Sampling fine{std::max(br.sampling.perStep, 8.0f), 4096};
            counts.insert(counts.end(), perPath, fine.count(p.points.size(), p.repeats, 1.0f));
        }
        auto out = std::make_shared<StrandSet>();
        out->resize(counts, StrandSet::kLeavePositions);
        std::vector<StrandId>& ids = out->mutableIds();
        std::vector<float>& id = out->addStrandChannel("braidId", -1.0f);
        std::vector<float>& flat = out->addStrandChannel("braidFlat", 0.0f);
        std::vector<float>& section = out->addStrandChannel("braidSection", -1.0f);
        StrandDraws centered;  // a section's center: no spot within it, no fan, no wander
        centered.fan = 0.0f;
        parallelFor(0, counts.size(), [&](std::size_t s) {
            const std::size_t b = s / perPath;
            const int lobe = static_cast<int>(s % perPath) - 1;
            const BraidPath& path = br.paths[b];
            ids[s] = hashCombine(path.id, static_cast<std::uint64_t>(lobe + 1));
            id[s] = numbering.number(static_cast<int>(b));
            flat[s] = br.style == Style::Cornrow ? 1.0f : 0.0f;
            section[s] = static_cast<float>(lobe);
            Vec3f* pts = out->strandPoints(s);
            const std::size_t m = out->strandPointCount(s);
            // A section starts where the weave does (past the gather, where its hair has come in, as
            // Show Tubes' tubes do); before it no hair is braided, so a curve there would weave
            // through the air off the hair. The path itself runs from its root.
            const float from = lobe < 0 ? 0.0f : std::clamp(path.weaveFrom, 0.0f, path.length);
            for (std::size_t j = 0; j < m; ++j) {
                pts[j] = br.of(b).braidedAt(path, from + (path.length - from) * static_cast<float>(j) / static_cast<float>(m - 1), lobe, centered);
            }
        });
        return StrandsPtr(out);
    }
};

}  // namespace

namespace {

using Outline = std::array<float, 2>;

// A point's distance to a closed polygon's boundary, and whether it lies inside it.
float distanceToOutline(const Outline& p, const Outline* poly, std::size_t count, bool* inside) {
    double least = std::numeric_limits<double>::infinity();
    bool in = false;
    for (std::size_t i = 0; i < count; ++i) {
        const Outline& a = poly[i];
        const Outline& b = poly[i + 1 < count ? i + 1 : 0];
        const double abx = static_cast<double>(b[0]) - a[0], aby = static_cast<double>(b[1]) - a[1];
        const double apx = static_cast<double>(p[0]) - a[0], apy = static_cast<double>(p[1]) - a[1];
        const double length2 = abx * abx + aby * aby;
        const double t = length2 > 0.0 ? std::clamp((apx * abx + apy * aby) / length2, 0.0, 1.0) : 0.0;
        const double dx = apx - t * abx, dy = apy - t * aby;
        least = std::min(least, dx * dx + dy * dy);
        if ((a[1] > p[1]) != (b[1] > p[1]) && p[0] < abx * (p[1] - a[1]) / aby + a[0]) in = !in;
    }
    if (inside) *inside = in;
    return static_cast<float>(std::sqrt(least));
}

}  // namespace

float BraidSections::area(int section, std::size_t sample) const {
    const Outline* p = outline(section, sample);
    const std::size_t count = static_cast<std::size_t>(around);
    double twice = 0.0;
    for (std::size_t i = 0; i < count; ++i) {
        const Outline& a = p[i];
        const Outline& b = p[i + 1 < count ? i + 1 : 0];
        twice += static_cast<double>(a[0]) * b[1] - static_cast<double>(a[1]) * b[0];
    }
    return static_cast<float>(0.5 * std::abs(twice));
}

float BraidSections::overlap(std::size_t sample, int a, int b) const {
    const std::size_t count = static_cast<std::size_t>(around);
    float deepest = 0.0f;
    for (const auto& [one, other] : {std::pair<int, int>{a, b}, std::pair<int, int>{b, a}}) {
        const Outline* points = outline(one, sample);
        const Outline* poly = outline(other, sample);
        for (std::size_t i = 0; i < count; ++i) {
            bool inside = false;
            const float far = distanceToOutline(points[i], poly, count, &inside);
            if (inside) deepest = std::max(deepest, far);
        }
    }
    return deepest;
}

float BraidSections::overlap() const {
    float most = 0.0f;
    for (std::size_t i = 0; i < samples(); ++i) {
        for (int a = 0; a < sections; ++a) {
            for (int b = a + 1; b < sections; ++b) most = std::max(most, overlap(i, a, b));
        }
    }
    return most;
}

float BraidSections::gap(std::size_t sample, int a, int b) const {
    const std::size_t count = static_cast<std::size_t>(around);
    float least = std::numeric_limits<float>::infinity();
    for (const auto& [one, other] : {std::pair<int, int>{a, b}, std::pair<int, int>{b, a}}) {
        const Outline* points = outline(one, sample);
        const Outline* poly = outline(other, sample);
        for (std::size_t i = 0; i < count; ++i) {
            bool inside = false;
            const float far = distanceToOutline(points[i], poly, count, &inside);
            if (inside) return 0.0f;
            least = std::min(least, far);
        }
    }
    return least;
}

std::array<float, 2> BraidSections::reach() const {
    std::array<float, 2> most{0.0f, 0.0f};
    for (const auto& section : outlines) {
        for (const Outline& p : section) most = {std::max(most[0], std::abs(p[0])), std::max(most[1], std::abs(p[1]))};
    }
    return most;
}

BraidSections braidSections(const BraidWeaveSettings& settings, int perStep, int around) {
    Braider br;
    br.style = styleFrom(settings.style);
    const bool pressing = settings.version >= 2;
    br.weave = weaveFor(br.style, settings.sections, settings.dutch, settings.flatness, pressing);
    br.pancake = settings.pancake;
    br.tightness = std::max(settings.tightness, 0.05f);
    if (pressing) br.press(settings.inflate, settings.separate);
    BraidSections out;
    out.sections = br.weave.sections;
    out.cycle = br.weave.cycle;
    out.perStep = std::max(perStep, 1);
    out.around = std::max(around, 3);
    out.round = br.weave.round;
    out.pressed = br.pressed != nullptr;
    const float loose = 1.0f + 0.6f * br.pancake;
    out.halfWidth = br.weave.halfWidth * loose;
    out.halfThick = br.weave.halfThick * loose;
    if (br.pressed) {
        out.keep = br.keep;
        out.grown = br.pressed->grown;
    }
    // (A straight braid one wide, not raised: its stations' sections are the weave's own.)
    const Frame frame;
    PathStation st;
    st.frame = &frame;
    st.wid = 1.0f;
    st.rate = br.tightness / std::max(br.weave.stepLength, 1e-6f);
    const std::size_t count = static_cast<std::size_t>(out.cycle) * static_cast<std::size_t>(out.perStep);
    const std::size_t sections = static_cast<std::size_t>(out.sections), rays = static_cast<std::size_t>(out.around);
    out.centers.assign(sections, std::vector<std::array<float, 2>>(count));
    out.across = out.centers;
    out.thick = out.centers;
    out.outlines.assign(sections, std::vector<std::array<float, 2>>(count * rays));
    for (std::size_t k = 0; k < sections; ++k) {
        for (std::size_t i = 0; i < count; ++i) {
            st.steps = static_cast<float>(i) / static_cast<float>(out.perStep);
            if (br.pressed) br.sampleAt(st);
            const SectionStation sec = br.sectionAt(static_cast<int>(k), st);
            out.centers[k][i] = {sec.center.y, sec.center.x};
            out.across[k][i] = {sec.across.y * out.halfWidth, sec.across.x * out.halfWidth};
            out.thick[k][i] = {sec.thick.y * out.halfThick, sec.thick.x * out.halfThick};
            for (std::size_t r = 0; r < rays; ++r) {
                const float turn = static_cast<float>(r) / static_cast<float>(rays);
                const float scale = sec.rows[0] ? br.outlineAt(sec.rows, st.between, turn * static_cast<float>(Pressed::kAround)) : 1.0f;
                const Vec3f p = sec.center + (sec.across * (pm::cos(kTwoPi * turn) * out.halfWidth) + sec.thick * (pm::sin(kTwoPi * turn) * out.halfThick)) * scale;
                out.outlines[k][i * rays + r] = {p.y, p.x};
            }
        }
    }
    return out;
}


BraidTubes braidTubes(const Operator& braid, const EvalContext& ctx, const std::vector<Value>& inputs, const BraidTubeDetail& detail) {
    const auto* op = dynamic_cast<const BraidOp*>(&braid);
    if (!op) throw GraphError("braidTubes: not a Braid node");
    return op->sectionTubes(ctx, inputs, detail);
}

BraidTubes braidTubes(const Operator& braid, const EvalContext& ctx, const std::vector<Value>& inputs, int around) {
    BraidTubeDetail detail;
    detail.around = around;
    return braidTubes(braid, ctx, inputs, detail);
}

BraidTubes braidTubes(Graph& graph, NodeId braid, const EvalContext& ctx, int around) {
    BraidTubeDetail detail;
    detail.around = around;
    return braidTubes(graph, braid, ctx, detail);
}

BraidTubes braidTubes(Graph& graph, NodeId braid, const EvalContext& ctx, const BraidTubeDetail& detail) {
    const Operator& op = graph.node(braid);
    if (op.typeName() != op::kBraid) throw GraphError("braidTubes: not a Braid node");
    if (graph.bypassed(braid)) return {};
    // Its inputs as the graph hands them to it (a preview's hidden strands with them: a fed braid
    // grows by those). Evaluating them also sets the node's keyed params for this time.
    EvalContext whole = ctx;
    whole.keepPreviewCore = true;
    std::vector<Value> inputs(op.inputs().size());
    for (std::size_t slot = 0; slot < inputs.size(); ++slot) {
        NodeId source = 0;
        std::size_t output = 0;
        if (graph.inputSource(braid, slot, &source, &output)) inputs[slot] = graph.evaluateOutput(source, output, whole);
    }
    return braidTubes(op, whole, inputs, detail);
}

int braidTubesAround(const Operator& braid) {
    return braid.typeName() == op::kBraid && braid.nodeVersion() >= 2 ? BraidTubeDetail::kPressedAround : BraidTubeDetail::kAround;
}

void registerBraidOperator(OperatorRegistry& registry) {
    registry.registerType(op::kBraid,
                          {"look",
                           "Braids strands along paths: three-, four- and five-strand braids, fishtails, ropes, "
                           "cornrows.",
                           "plait cornrow fishtail rope"},
                          [] { return std::make_unique<BraidOp>(); });
}

}  // namespace groomlab
