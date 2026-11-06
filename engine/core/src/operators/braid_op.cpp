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
// ends fan out. A share of strands (`strays`) stay loose. The hair is parted
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
// Output channels, per strand: `braidId` (the path's index among the guides, or for paths of its own a
// number from the picked strand's ID; -1 when not
// braided) and `braidFlat` (1 for a cornrow, lying on the scalp), for nodes
// downstream (Cards builds braid cards from them), and `braidSection` (the section of the
// braid the strand runs in, -1 when not braided; with display paths, -1 the path, else the
// section: Cards makes a tube per section from it). Per point, `braidTail`: 0 within the braid,
// rising to 1 down its tail (fan, tuft), a mask for nodes after it (Curl, Clump,
// Noise with affect $braidTail shape the ends alone), and `braidWoven`: 0 on the way in (from
// the root to where the strand joins the braid: along the scalp when fed), rising to 1 in the
// braid (Cards builds a braid's tubes from the woven part).

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <limits>

#include "groomlab/curve.h"
#include "groomlab/frames.h"
#include "groomlab/graph.h"
#include "groomlab/operators.h"
#include "groomlab/parallel.h"
#include "groomlab/point_index.h"
#include "groomlab/portable_math.h"
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
enum class End { Fan, Tuft, Bound };

End endFrom(const std::string& name) {
    if (name == "fan") return End::Fan;
    if (name == "tuft") return End::Tuft;
    if (name == "bound") return End::Bound;
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
// and comes round to the outer edge to be taken across.
Weave fishtailWeave(int m, float sign, float flatness) {
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
            const float half = phi < static_cast<float>(m) ? 1.0f : -1.0f;  // right half first
            const float j = phi - (half > 0.0f ? 0.0f : static_cast<float>(m));
            std::array<float, 2> c;
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

Weave weaveOf(Style style, std::int64_t sections, bool dutch, float flatness) {
    const float sign = dutch ? -1.0f : 1.0f;
    switch (style) {
        case Style::Three: return flatWeave(3, 1, sign, flatness);
        case Style::Four: return flatWeave(4, 2, sign, flatness);
        case Style::Five: return flatWeave(5, 2, sign, flatness);
        case Style::Fishtail: return fishtailWeave(sections > 0 ? static_cast<int>(std::clamp<std::int64_t>(sections, 3, 12)) : 6, sign, flatness);
        case Style::Rope: return ropeWeave(sections > 0 ? static_cast<int>(std::clamp<std::int64_t>(sections, 2, 8)) : 2, sign, flatness);
        case Style::Cornrow: {
            Weave w = flatWeave(3, 1, -1.0f, flatness);  // crossing under: raised
            w.lift = -w.lowest();                         // the lowest section on the scalp
            return w;
        }
    }
    return flatWeave(3, 1, sign, flatness);
}

Weave weaveFor(Style style, std::int64_t sections, bool dutch, float flatness) {
    Weave w = weaveOf(style, sections, dutch, flatness);
    w.findLobes();
    return w;
}

struct BraidPath {
    std::vector<Vec3f> points;  // resampled evenly
    std::vector<Frame> frames;
    float length = 0.0f;
    Vec3f root;
    StrandId id = 0;           // the guide's (a picked strand's) ID
    StrandRoot guideRoot;      // and its root
    std::vector<float> steps;  // weave steps travelled at each point
    std::vector<float> rate;   // steps a unit of length at each point
    float repeats = 0.0f;      // weave steps along the whole path
    std::vector<float> grown;  // feed: the width's factor at each point (empty: 1)
    std::vector<float> ties;   // where each tie's middle is along the path, the last first
    float tieHalf = 0.0f;      // half a tie's length

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

// A strand's own draws, by its ID: whether it stays loose, a section for a root with no side, its
// spot within its section (in the unit disc), how its end fans out and wanders.
struct StrandDraws {
    bool stray = false;
    int lobe = 0;
    Vec3f inLobe;
    float fanAngle = 0.0f, wanderPhase = 0.0f;
    float endDraw = 0.0f;  // where in the tail a fed strand ends
    float fan = 0.9f;      // the fan-out's reach, in braid widths
    float fuzz = 0.0f;     // the wander, scene units

    StrandDraws() = default;
    StrandDraws(std::uint64_t id, std::uint64_t seed, float strays, int lobes, float wander) : fuzz(wander) {
        Rng rng(hashCombine(id, seed ^ 0xB4A1D5ull));
        stray = rng.nextFloat() < strays;
        lobe = static_cast<int>(rng.nextU64() % static_cast<std::uint64_t>(lobes));
        const float a = kTwoPi * rng.nextFloat();
        const float r = 0.92f * std::sqrt(rng.nextFloat());
        inLobe = {r * pm::cos(a), r * pm::sin(a), 0.0f};
        fanAngle = kTwoPi * rng.nextFloat();
        wanderPhase = kTwoPi * rng.nextFloat();
        endDraw = rng.nextFloat();
    }
};

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

// The braid's settings and paths: where each strand joins and where its points go.
struct Braider {
    Style style = Style::Three;
    Weave weave;
    float width = 0.0f, tightness = 1.0f, roll = 0.0f;
    Ramp taper;
    float region2 = 0.0f, gather = 0.0f, tail = 0.0f, strays = 0.0f, fuzz = 0.0f;
    std::uint64_t seed = 0;
    Sampling sampling;
    ScalpGather overScalp;
    float feedUntil = 0.0f;
    // Fed along the scalp: raised by (braid widths) so its lowest section lies on the scalp, easing
    // back to the path's middle where the pick-up ends (it leaves the head there).
    float feedLift = 0.0f;
    End end = End::Fan;
    float pancake = 0.0f, loops = 0.0f, loopSize = 0.5f;
    int ties = 0;
    float tieLength = 0.5f, tieSpacing = 0.0f, cinch = 0.0f;
    std::vector<BraidPath> paths;

    float widthAt(const BraidPath& path, float at) const {
        return width * std::max(taper(std::clamp(at / path.length, 0.0f, 1.0f)), 0.0f) * path.grownAt(at);
    }
    // Where the weave ends: the tail's start (bound: the path's end).
    float weaveEnd(const BraidPath& path) const { return end == End::Bound ? path.length : (1.0f - tail) * path.length; }
    // How far down its tail `at` is: 0 in the braid, 1 at the end (bound: no tail).
    float tailAt(const BraidPath& path, float at) const {
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
            if (k > 0 && (at < first - 1e-6f || !(step > 0.0f))) break;
            path.ties.push_back(std::max(at, path.tieHalf));
        }
    }
    // How much `at` is inside a tie: 1 within one, easing to 0 either side (`which`: the tie).
    float inTie(const BraidPath& path, float at, std::size_t* which = nullptr) const {
        if (path.ties.empty()) return 0.0f;
        const float fade = path.tieHalf + 0.4f * width;
        float most = 0.0f;
        for (std::size_t k = 0; k < path.ties.size(); ++k) {
            const float d = std::abs(at - path.ties[k]) - path.tieHalf;
            const float w = d <= 0.0f ? 1.0f : 1.0f - smoothstep(0.0f, fade, d);
            if (w > most) {
                most = w;
                if (which) *which = k;
            }
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
    // it leaves the head.
    float liftAt(const BraidPath& path, float at) const {
        float lift = weave.lift;
        if (feedLift > 0.0f) {
            const float leaves = feedUntil * path.length;
            lift += feedLift * (1.0f - smoothstep(leaves, leaves + 2.0f * width, at));
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
    StrandDraws drawsFor(std::uint64_t id) const { return StrandDraws(id, seed, strays, weave.sections, fuzz); }

    // A section's offset from the path at `steps`, in the frame's (normal, binormal, tangent),
    // for a strand at `spot` within it (the unit disc), the braid `wid` wide, its steps a unit of
    // length `rate`: the section's band follows its own course, so it passes over and under its
    // neighbours instead of through them.
    Vec3f sectionOffset(int section, float steps, const Vec3f& spot, float wid, float rate, float lift) const {
        std::array<float, 2> slope{};
        std::array<float, 2> c = weave.at(section, steps, &slope);
        if (pancake > 0.0f) {  // pulled apart: the sections out from the middle, looser and wider
            const float out = 1.0f + 0.45f * pancake;
            c = {c[0] * out, c[1] * (1.0f + 0.25f * pancake)};
            slope = {slope[0] * out, slope[1] * (1.0f + 0.25f * pancake)};
        }
        const Vec3f center{(c[1] + lift) * wid, c[0] * wid, 0.0f};
        const Vec3f along = Vec3f{slope[1] * rate * wid, slope[0] * rate * wid, 1.0f}.normalized();
        Vec3f thick = weave.round ? Vec3f{c[1], c[0], 0.0f} : Vec3f{1.0f, 0.0f, 0.0f};
        thick = thick - along * thick.dot(along);
        if (thick.length() < 1e-5f) thick = Vec3f{1.0f, 0.0f, 0.0f} - along * along.x;
        thick = thick.normalized();
        const Vec3f across = along.cross(thick);
        const float loose = 1.0f + 0.6f * pancake;
        return center + across * (spot.x * weave.halfWidth * loose * wid) + thick * (spot.y * weave.halfThick * loose * wid);
    }
    // The hair a section lets out of the braid at `steps` for a strand at `spot` within it: at a
    // share `loops` of the sections' stretches between crossings along the braid's edge (each
    // section's own), the hair on its outer side bows out off the braid's side and goes back in
    // before the next crossing, most at the section's outer edge, less toward its inside (how far
    // in it reaches, each loop its own): a section of the braid pulled loose, its hair together.
    Vec3f loopAt(int section, float steps, const Vec3f& spot, float wid) const {
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
                const float toX = (side * 0.5f * (reachIn + 1.0f) - spot.x) * clump * weave.halfWidth * loose * wid;
                const float toY = -spot.y * clump * weave.halfThick * loose * wid;
                return {0.35f * bow + toY, side * bow + toX, 0.0f};  // off the side, a little up
            }
        }
        return {};
    }

    // The braid's point `at` a distance along `path` for a strand in `section` (-1: the path
    // itself) with draws `d`: its section's offset in the path's frame, letting go and fanning out
    // over the tail.
    Vec3f braidedAt(const BraidPath& path, float at, int section, const StrandDraws& d) const {
        Vec3f center;
        const Frame& fr = frameAt(path, at, &center);
        if (section < 0) return center;
        const float u = std::min(at / path.length, 1.0f);
        const float wid = braidWidthAt(path, at);
        const float letGo = tailAt(path, at);
        const float wander = d.fuzz * pm::sin(d.wanderPhase + 7.0f * u);
        const float rate = at < path.length ? path.sample(path.rate, at) : path.rate.back();
        const Vec3f local = sectionOffset(section, path.weaveAt(at), d.inLobe, wid, rate, liftAt(path, at)) * (1.0f - letGo);
        Vec3f loose;  // where the strand goes as it lets go
        if (end == End::Tuft) {  // gathered: its spot within the tuft, a little looser down it
            loose = Vec3f{d.inLobe.y, d.inLobe.x, 0.0f} * (letGo * wid * (0.3f + 0.25f * letGo));
        } else {  // fanned out
            const float fanDepth = style == Style::Cornrow ? std::abs(pm::sin(d.fanAngle)) : pm::sin(d.fanAngle);
            loose = Vec3f{fanDepth, pm::cos(d.fanAngle), 0.0f} * (letGo * wid * d.fan);
        }
        Vec3f offset = local + loose + Vec3f{0.0f, wander, 0.0f};
        // Loose bundles looping out of their section (loops) go before the ties: those hold them too.
        offset = offset + loopAt(section, path.weaveAt(at), d.inLobe, wid) * (1.0f - letGo);
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
            const float release = smoothstep(0.25f * width, width, outside);
            if (release < 1.0f) {
                Vec3f from, direction;
                tieAxis(path, which, &from, &direction);
                const float along = std::clamp(at - (path.ties[which] - path.tieHalf), 0.0f, 2.0f * path.tieHalf);
                const Vec3f onTie = fr.toLocal(from + direction * along - center);
                const Vec3f own = fr.toLocal(axisAt(path, at) - center);
                const Vec3f axis = onTie + (own - onTie) * release;
                const float most = 0.47f * kTieBore * wid + 0.15f * outside;
                const float n = offset.x - axis.x, b = offset.y - axis.y, r = std::sqrt(n * n + b * b);
                if (r > most) {
                    const float k = 1.0f + (most / r - 1.0f) * (1.0f - release);
                    offset.x = axis.x + n * k;
                    offset.y = axis.y + b * k;
                }
            }
        }
        return center + fr.toWorld(offset);
    }

    // The section `share` of the way across the braid (0: its one side, 1: the other, along the
    // frame's binormal) at `steps`: the sections ranked side to side there, an equal share each.
    int sectionAcross(float share, float steps) const {
        const int n = weave.sections;
        std::vector<std::pair<float, int>> order(static_cast<std::size_t>(n));
        for (int k = 0; k < n; ++k) order[static_cast<std::size_t>(k)] = {weave.at(k, steps)[0], k};
        std::sort(order.begin(), order.end());
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
    // braided (not loose, not a stray) the points its path's weave needs.
    std::size_t pointsFor(const StrandSet& in, std::size_t s) const {
        const std::size_t n = in.strandPointCount(s);
        const int path = n < 2 ? -1 : pathOf(in.strandPoints(s)[0]);
        if (path < 0 || drawsFor(in.ids()[s]).stray) return n;
        return sampling.count(n, paths[static_cast<std::size_t>(path)].repeats, 1.0f);
    }
    // Without feed, the strand's points along its path from root to tip (pts[0], its root, stays);
    // `tails` (null: not asked) gets each point's braidTail.
    int braidAlong(Vec3f* pts, std::size_t n, const BraidPath& path, const StrandDraws& d, float* tails, float* woven = nullptr) const {
        // The section on its side where it comes in, halfway through the gather.
        const float in = 0.5f * gather * path.length;
        const int section = sectionOfRoot(pts[0] - path.root, path.frames.front(), path.weaveAt(in), d.lobe);
        if (tails) tails[0] = 0.0f;
        if (woven) woven[0] = 0.0f;
        for (std::size_t i = 1; i < n; ++i) {
            const float u = static_cast<float>(i) / static_cast<float>(n - 1);
            const Vec3f braided = braidedAt(path, u * path.length, section, d);
            const float blend = smoothstep(0.0f, gather, u);  // from the root into the braid
            pts[i] = pts[i] * (1.0f - blend) + braided * blend;
            overScalp.apply(pts[i], blend, (pts[i] - pts[0]).length());
            if (tails) tails[i] = tailAt(path, u * path.length);
            if (woven) woven[i] = blend;
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

// Feed mode: where strand `id` (points `pts`) joins a path: where it reaches it, or at the point
// nearest its root; `along` gets the arc length from its root to each point.
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
        const std::size_t q = index.pickUp.nearest(pts[0], &d2);
        if (q < index.sampleOf.size() && d2 <= br.region2) {
            best = static_cast<int>(index.sampleOf[q].first);
            bestK = index.sampleOf[q].second;
            bestD2 = d2;
        }
        const std::size_t r = index.rootIndex.nearest(pts[0], &d2);
        if (r < index.rootCount && d2 <= br.region2 && d2 <= bestD2) {
            best = static_cast<int>(r);
            bestK = 0;
            bestD2 = d2;
        }
    }
    if (best < 0) return plan;
    plan.draws = br.drawsFor(id);
    if (plan.draws.stray) return plan;
    const BraidPath& path = br.paths[static_cast<std::size_t>(best)];
    const std::size_t last = path.points.size() - 1;
    const float step = path.length / static_cast<float>(last);
    along.resize(n);
    cumulativeArcLength(pts, n, along.data());
    plan.strandLength = along[n - 1];

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
        const float limit = std::max(std::min(br.feedUntil * path.length, br.weaveEnd(path)), 0.0f);
        joinAt = std::min(joinAt + off, std::max(limit, joinAt));
        if (br.weave.alternates) {
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
    // Fitted to the path: the strand's length is shared out so it ends within the braid's last
    // stretch (a random spot in the tail, so the end isn't cut square), never past it. A strand
    // joining at or past that end has no braid to run along: it stays loose.
    const float endAt = path.length * (1.0f - 0.5f * (br.end == End::Bound ? 0.0f : br.tail) * plan.draws.endDraw);
    if (plan.joinAt >= endAt) return plan;
    const float room = plan.approach + endAt - plan.joinAt;
    if (plan.strandLength > room && plan.strandLength > 1e-6f) plan.squeeze = room / plan.strandLength;
    plan.path = best;
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
    std::vector<ChannelWrite> channelWrites() const override {
        return {{"braidId", ""}, {"braidFlat", ""}, {"braidSection", ""}, {"braidTail", ""}, {"braidWoven", ""}};
    }
    std::string category() const override { return "look"; }
    std::vector<std::string> externalInputs() const override { return {"scalp"}; }
    std::vector<PortSpec> inputs() const override {
        return {{"strands", PortType::Strands}, {"braids", PortType::Guides, true}};
    }
    // The braided strands, and the paths they are braided along (the braids input's, or those it
    // picked among the strands), evenly resampled, for nodes downstream.
    // And its ties (beads, cuffs, hair ties): a short strand along the braid at each, as wide as
    // the braid there, for an Instance node's `along` input.
    std::vector<PortSpec> outputs() const override {
        return {output(), {"paths", PortType::Guides}, {"ties", PortType::Strands}};
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
            ParamSpec{"width", 0.25, "Braid width at its start, scene units", "length"}.soft(0, 1),
            {"widthProfile", Ramp::linear(1.0f, 0.55f), "Width along the braid, start to end"},
            ParamSpec{"tightness", 1.0, "How close the crossings are: 1 a natural braid, 2 twice as many in the same "
                                        "length. A crossing is a share of the width long, so a tapering braid's "
                                        "crossings shrink with it"}
                .atLeast(0.05)
                .soft(0.25, 3),
            ParamSpec{"flatness", 0.3, "0: round sections, 1: flat bands (a flatter braid)", "fraction"}.range(0, 1),
            ParamSpec{"pancake", 0.0, "Pulls the sections apart and loosens them: the loose, chunky look of a braid "
                                      "pulled out by hand", "fraction"}
                .range(0, 1)
                .in("Loose"),
            ParamSpec{"loops", 0.0, "Share of the sections' stretches between crossings where the hair on the "
                                    "section's outer side bows out of the braid in a loop and goes back in before the "
                                    "next crossing", "fraction"}
                .range(0, 1)
                .in("Loose"),
            ParamSpec{"loopSize", 0.5, "How far a loop reaches out, in braid widths (each a little more or less)"}
                .atLeast(0)
                .soft(0, 1.5)
                .in("Loose"),
            ParamSpec{"roll", 0.0, "Turns the braid's face about its path, degrees (it faces out of the scalp, or away "
                                   "from the head)", "angle"}
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
            ParamSpec{"gather", 0.15, "Share of the strand spent leaving the scalp into the braid", "fraction"}
                .range(0, 0.9)
                .in("Strands"),
            ParamSpec{"end", std::string("fan"),
                      "How the braid ends: fan (over the tail the sections let go and the ends spread), tuft (they "
                      "let go into a gathered tuft, as below a hair tie) or bound (woven to the tip, narrowing: sealed "
                      "box-braid ends, no tail)",
                      "", {"fan", "tuft", "bound"}}
                .in("Ends"),
            ParamSpec{"tail", 0.12, "Share at the end where the sections let go (fan, tuft)", "fraction"}
                .range(0, 0.9)
                .in("Ends")
                .when("end", {"fan", "tuft"}),
            notAnimatable({"ties", std::int64_t{0}, "Ties along each braid (beads, cuffs, hair ties): the braid tightens "
                                                    "round each, and the ties output hands them on for an Instance "
                                                    "node's along input. 1: one where the weave ends"})
                .range(0, 32)
                .soft(0, 8)
                .in("Ties"),
            ParamSpec{"tieLength", 0.5, "Each tie's length, in braid widths"}.atLeast(0.05).soft(0.1, 2).in("Ties"),
            ParamSpec{"tieSpacing", 0.0, "From one tie to the next, in braid widths (0: spread along the braid)"}
                .atLeast(0)
                .soft(0, 4)
                .in("Ties"),
            ParamSpec{"cinch", 0.35, "How much the braid narrows inside a tie", "fraction"}.range(0, 0.9).in("Ties"),
            ParamSpec{"strays", 0.03, "Share of the region's strands left loose", "fraction"}.range(0, 1).in("Strands"),
            ParamSpec{"fuzz", 0.02, "Random wander within the sections, scene units", "length"}.soft(0, 0.1).in("Strands"),
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
        GuidesPtr paths;
        StrandsPtr ties;
        run(ctx, inputs, nullptr, &out, nullptr, &paths, &ties);
        return {out, paths, ties};
    }
    // Without feed each strand is braided by its own root alone (the nearest path's root within
    // the region): while the paths stay, the strands that changed are braided again (each to as
    // many points as before, or it evaluates in full) and the rest keep the last result's points;
    // the paths are the last ones.
    bool evaluateOutputsIncremental(const EvalContext& ctx, const std::vector<Value>& inputs, const IncrementalInputs& incremental,
                                    std::vector<Value>* outs, std::vector<StrandChanges>* changed) const override {
        if (incremental.previousOutputs.size() != 3) return false;
        IncrementalInputs strands;
        strands.previous = incremental.previousOutputs[0];
        strands.inputs = incremental.inputs;
        Value out;
        StrandChanges made;
        if (!run(ctx, inputs, &strands, &out, &made, nullptr, nullptr)) return false;
        *outs = {out, incremental.previousOutputs[1], incremental.previousOutputs[2]};
        *changed = {std::move(made), StrandChanges::listed({}), StrandChanges::listed({})};
        return true;
    }

private:
    // In full (`paths`: the paths' guides, `ties` the ties' strands, null: not asked), or (with
    // `incremental`) only the strands the input changed in; false when that cannot be done.
    bool run(const EvalContext& ctx, const std::vector<Value>& inputs, const IncrementalInputs* incremental, Value* result,
             StrandChanges* changed, GuidesPtr* paths, StrandsPtr* ties) const {
        const StrandsPtr in = valueAs<StrandsPtr>(inputs.at(0));
        if (!in) throw GraphError("Braid: strands input not connected");
        GuidesPtr braids = valueAs<GuidesPtr>(inputs.at(1));
        auto handOn = [&] {  // nothing to braid along: the graph passes the input's changes on
            if (incremental) return false;
            *result = in;
            if (paths) *paths = GuidesPtr(std::make_shared<GuideSet>());
            if (ties) *ties = StrandsPtr(std::make_shared<StrandSet>());
            return true;
        };
        const ScalpMesh* scalp = valueAs<MeshPtr>(ctx.external("scalp")).get();
        const bool own = !braids;
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

        Braider br;
        br.style = styleFrom(params().getString("style"));
        br.weave = weaveFor(br.style, params().getInt("sections"), params().getBool("dutch"),
                            static_cast<float>(params().getDouble("flatness")));
        br.width = static_cast<float>(params().getDouble("width"));
        br.taper = params().getRamp("widthProfile");
        br.tightness = std::max(static_cast<float>(params().getDouble("tightness")), 0.05f);
        br.roll = static_cast<float>(params().getDouble("roll"));
        const float region = static_cast<float>(params().getDouble("region"));
        // (Paths of its own, about region apart, cover the scalp: every strand joins the nearest.)
        br.region2 = own && !params().getBool("feed") ? std::numeric_limits<float>::infinity() : region * region;
        br.gather = static_cast<float>(params().getDouble("gather"));
        br.end = endFrom(params().getString("end"));
        br.tail = br.end == End::Bound ? 0.0f : static_cast<float>(params().getDouble("tail"));
        br.ties = static_cast<int>(params().getInt("ties"));
        br.pancake = static_cast<float>(params().getDouble("pancake"));
        br.loops = static_cast<float>(params().getDouble("loops"));
        br.loopSize = static_cast<float>(params().getDouble("loopSize"));
        br.tieLength = static_cast<float>(params().getDouble("tieLength"));
        br.tieSpacing = static_cast<float>(params().getDouble("tieSpacing"));
        br.cinch = static_cast<float>(params().getDouble("cinch"));
        br.strays = static_cast<float>(params().getDouble("strays"));
        br.fuzz = static_cast<float>(params().getDouble("fuzz"));
        br.seed = static_cast<std::uint64_t>(params().getInt("seed"));
        const bool feed = params().getBool("feed");
        if (feed) {
            br.feedUntil = static_cast<float>(params().getDouble("feedUntil"));
            if (br.style != Style::Cornrow) br.feedLift = -br.weave.lowest();  // (a cornrow lies on the scalp already)
        }
        const bool showPaths = params().getString("display") == "paths";

        // The head's middle (the scalp's points' mean): a braid leaving along the scalp normal faces
        // away from it.
        Vec3f headMiddle;
        if (scalp && scalp->pointCount() > 0) {
            for (const Vec3f& q : scalp->points()) headMiddle = headMiddle + q;
            headMiddle = headMiddle * (1.0f / static_cast<float>(scalp->pointCount()));
        }
        // Braid paths, evenly resampled, with frames facing out.
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
            p.frames = rotationMinimizingFrames(p.points.data(), p.points.size(), facing(scalp, headMiddle, g, p, br.roll));
            // The weave travelled along the path: the steps a unit of length integrated.
            const std::size_t count = p.points.size();
            const float step = p.length / static_cast<float>(count - 1);
            p.rate.resize(count);
            p.steps.assign(count, 0.0f);
            for (std::size_t k = 0; k < count; ++k) p.rate[k] = br.rateAt(static_cast<float>(k) / static_cast<float>(count - 1));
            for (std::size_t k = 1; k < count; ++k) p.steps[k] = p.steps[k - 1] + 0.5f * (p.rate[k - 1] + p.rate[k]) * step;
            p.repeats = p.steps.back();
            br.placeTies(p);
            br.paths.push_back(std::move(p));
        }
        if (br.paths.empty()) return handOn();
        if (paths) {
            auto made = std::make_shared<GuideSet>();
            for (const BraidPath& p : br.paths) made->add(p.id, p.points).root = p.guideRoot;
            *paths = GuidesPtr(made);
        }

        // Incrementally: the same paths, no feed, no earlier Braid's ids (numbered on from their
        // highest), the hair shown.
        const StrandChanges* changes = nullptr;
        StrandsPtr last;
        if (incremental) {
            changes = someStrandsChanged(*incremental, 0);
            last = valueAs<StrandsPtr>(incremental->previous);
            if (!changes || !last || last->strandCount() != in->strandCount() || feed || showPaths ||
                !inputUnchanged(*incremental, 1) || in->strandChannel("braidId")) {
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
        BraidChannels channels{nullptr, nullptr, nullptr, nullptr, nullptr, firstId};
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
        }

        const std::size_t count = in->strandCount();
        std::vector<std::size_t> counts(count);
        if (feed) {
            // Where each strand joins (on its points as they came), for the points it needs and
            // how much of each braid's hair it holds where.
            const FeedIndex index(br.paths, br.feedUntil);
            std::vector<FedPlan> plans(count);
            parallelFor(0, count, [&](std::size_t s) {
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
            const PreviewCore core(*in);
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
        if (ties) *ties = tieStrands(br, channels);
        if (showPaths) {
            *result = pathStrands(br, channels);
            return true;
        }

        if (incremental) {
            // Each changed strand braided again into as many points as it had (as the rest keep
            // theirs, their input as it was), else in full.
            std::atomic<bool> moved{false};
            forEachListed(changes->strands, [&](std::size_t s) {
                if (br.pointsFor(*in, s) != last->strandPointCount(s)) moved = true;
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
            if (!channels.id || !channels.flat || !channels.tail || !channels.section || !channels.woven) return false;
            forEachListed(changes->strands, [&](std::size_t s) {
                for (const auto& [from, to] : strandValues) (*to)[s] = (*from)[s];
                resampleStrand(*in, *made, s, false, widths, inPoint, outPoint, outWidths);
                (*channels.id)[s] = -1.0f;
                (*channels.flat)[s] = 0.0f;
                (*channels.section)[s] = -1.0f;
                std::fill_n(channels.tail->begin() + static_cast<std::ptrdiff_t>(made->strandBegin(s)), made->strandPointCount(s), 0.0f);
                std::fill_n(channels.woven->begin() + static_cast<std::ptrdiff_t>(made->strandBegin(s)), made->strandPointCount(s), 0.0f);
                braidOne(br, *made, s, channels);
            });
            *result = StrandsPtr(made);
            *changed = *changes;
            return true;
        }

        if (!feed) {
            parallelFor(0, count, [&](std::size_t s) { counts[s] = br.pointsFor(*in, s); });
        }
        // The strands given the points they need (each evenly along itself, as Resample does).
        bool adds = false;
        for (std::size_t s = 0; s < count && !adds; ++s) adds = counts[s] != in->strandPointCount(s);
        // (A set of its own when made here: nobody else holds it.)
        const std::shared_ptr<StrandSet> out =
            adds ? std::const_pointer_cast<StrandSet>(resampleStrands(*in, counts)) : std::make_shared<StrandSet>(*in);
        channels.id = &out->addStrandChannel("braidId", -1.0f);
        channels.flat = &out->addStrandChannel("braidFlat", 0.0f);
        channels.tail = &out->addPointChannel("braidTail", 0.0f);
        channels.section = &out->addStrandChannel("braidSection", -1.0f);
        channels.woven = &out->addPointChannel("braidWoven", 0.0f);
        if (!earlierIds.empty()) {
            *channels.id = earlierIds;
            if (earlierFlat.size() == channels.flat->size()) *channels.flat = earlierFlat;
            if (earlierSections.size() == channels.section->size()) *channels.section = earlierSections;
        }
        if (feed) {
            const FeedIndex index(br.paths, br.feedUntil);
            parallelFor(0, count, [&](std::size_t s) {
                thread_local std::vector<float> along;
                Vec3f* pts = out->strandPoints(s);
                const std::size_t n = out->strandPointCount(s);
                const FedPlan plan = planFed(br, index, pts, n, out->ids()[s], along);
                if (plan.path < 0) return;
                channels.set(s, plan.path, br.style);
                (*channels.section)[s] = static_cast<float>(feedOne(br, plan, pts, n, along, channels.tail->data() + out->strandBegin(s),
                                                                    channels.woven->data() + out->strandBegin(s)));
            });
        } else {
            parallelFor(0, count, [&](std::size_t s) { braidOne(br, *out, s, channels); });
        }
        *result = StrandsPtr(out);
        return true;
    }

    // Without feed: strand s of `out` braided along the path it joins, if any.
    static void braidOne(const Braider& br, StrandSet& out, std::size_t s, const BraidChannels& channels) {
        Vec3f* pts = out.strandPoints(s);
        const std::size_t n = out.strandPointCount(s);
        if (n < 2) return;
        const int best = br.pathOf(pts[0]);
        if (best < 0) return;
        const StrandDraws draws = br.drawsFor(out.ids()[s]);
        if (draws.stray) return;
        channels.set(s, best, br.style);
        const int section = br.braidAlong(pts, n, br.paths[static_cast<std::size_t>(best)], draws,
                                          channels.tail ? channels.tail->data() + out.strandBegin(s) : nullptr,
                                          channels.woven ? channels.woven->data() + out.strandBegin(s) : nullptr);
        if (channels.section) (*channels.section)[s] = static_cast<float>(section);
    }

    // Feed mode: a strand is drawn in from its root to where it joins the path, and runs along it
    // by its own length.
    // Returns the section it joins.
    static int feedOne(const Braider& br, const FedPlan& plan, Vec3f* pts, std::size_t n, const std::vector<float>& along,
                       float* tail, float* woven) {
        const BraidPath& path = br.paths[static_cast<std::size_t>(plan.path)];
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
        const Vec3f joins = br.braidedAt(path, plan.joinAt, section, plan.draws);
        const Vec3f way = (joins - root) / plan.approach;
        const float turn = std::min(0.5f * br.widthAt(path, plan.joinAt), 0.4f * plan.approach);
        tail[0] = 0.0f;
        woven[0] = 0.0f;
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
                continue;
            }
            const Vec3f braided = br.braidedAt(path, std::max(at, 0.0f), section, plan.draws);
            pts[i] = straight + (braided - straight) * w;
            tail[i] = fitted < plan.approach ? 0.0f : br.tailAt(path, at);
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
            for (std::size_t k = 0; k < path.ties.size(); ++k, ++s) {
                ids[s] = hashCombine(path.id ^ 0x71E5B7A1ull, static_cast<std::uint64_t>(k));
                roots[s] = path.guideRoot;
                id[s] = numbering.number(static_cast<int>(b));
                Vec3f* pts = out->strandPoints(s);
                const std::size_t begin = out->strandBegin(s);
                // Straight, as a rigid bead lies (the hair is held round this axis).
                Vec3f from, direction;
                br.tieAxis(path, k, &from, &direction);
                const float length = (br.axisAt(path, path.ties[k] + path.tieHalf) - from).length();
                for (std::size_t j = 0; j < kPoints; ++j) {
                    const float u = static_cast<float>(j) / static_cast<float>(kPoints - 1);
                    const float at = path.ties[k] - path.tieHalf + 2.0f * path.tieHalf * u;
                    pts[j] = from + direction * (length * u);
                    widths[begin + j] = kTieBore * br.braidWidthAt(path, at);  // snug round the hair held in it
                }
            }
        }
        return StrandsPtr(out);
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
            for (std::size_t j = 0; j < m; ++j) {
                pts[j] = br.braidedAt(path, path.length * static_cast<float>(j) / static_cast<float>(m - 1), lobe, centered);
            }
        });
        return StrandsPtr(out);
    }
};

}  // namespace

void registerBraidOperator(OperatorRegistry& registry) {
    registry.registerType(op::kBraid,
                          {"look",
                           "Braids strands along paths: three-, four- and five-strand braids, fishtails, ropes, "
                           "cornrows.",
                           "plait cornrow fishtail rope"},
                          [] { return std::make_unique<BraidOp>(); });
}

}  // namespace groomlab
