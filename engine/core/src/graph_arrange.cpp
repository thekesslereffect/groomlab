// SPDX-License-Identifier: Apache-2.0
// Arranging a graph by hand, every host's node editor's (graph_arrange.h).

#include "groomlab/graph_arrange.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <set>

#include "groomlab/graph_inserts.h"
#include "groomlab/graph_pins.h"
#include "groomlab/graph_portals.h"

namespace groomlab {

namespace {

using json = nlohmann::json;

using Positions = std::map<std::string, std::pair<double, double>>;

Positions unmoved(const std::vector<LayoutBox>& boxes) {
    Positions out;
    for (const LayoutBox& b : boxes) out[b.name] = {b.x, b.y};
    return out;
}

// Boxes in order along an axis (then the other, then by name: the same order every time).
std::vector<LayoutBox> sortedAlong(std::vector<LayoutBox> boxes, bool acrossX) {
    std::stable_sort(boxes.begin(), boxes.end(), [&](const LayoutBox& a, const LayoutBox& b) {
        const double ka = acrossX ? a.x : a.y, kb = acrossX ? b.x : b.y;
        if (ka != kb) return ka < kb;
        const double la = acrossX ? a.y : a.x, lb = acrossX ? b.y : b.x;
        if (la != lb) return la < lb;
        return a.name < b.name;
    });
    return boxes;
}

}  // namespace

std::vector<std::string> alignModes() {
    return {"left", "centerX", "right", "top", "centerY", "bottom", "row", "column", "distributeX", "distributeY"};
}

std::map<std::string, std::pair<double, double>> alignBoxes(const std::vector<LayoutBox>& boxes, const std::string& mode, double gap) {
    const std::vector<std::string> modes = alignModes();
    if (std::find(modes.begin(), modes.end(), mode) == modes.end()) throw GraphError("no align mode '" + mode + "'");
    Positions out = unmoved(boxes);
    const bool distribute = mode == "distributeX" || mode == "distributeY";
    if (boxes.size() < (distribute ? 3u : 2u)) return out;
    double left = boxes[0].x, top = boxes[0].y, right = boxes[0].right(), bottom = boxes[0].bottom();
    for (const LayoutBox& b : boxes) {
        left = std::min(left, b.x);
        top = std::min(top, b.y);
        right = std::max(right, b.right());
        bottom = std::max(bottom, b.bottom());
    }
    const double midX = (left + right) * 0.5, midY = (top + bottom) * 0.5;
    if (mode == "left" || mode == "centerX" || mode == "right" || mode == "top" || mode == "centerY" || mode == "bottom") {
        for (const LayoutBox& b : boxes) {
            auto& [x, y] = out[b.name];
            if (mode == "left") x = left;
            if (mode == "right") x = right - b.w;
            if (mode == "centerX") x = midX - b.w * 0.5;
            if (mode == "top") y = top;
            if (mode == "bottom") y = bottom - b.h;
            if (mode == "centerY") y = midY - b.h * 0.5;
        }
        return out;
    }
    const bool acrossX = mode == "row" || mode == "distributeX";
    const std::vector<LayoutBox> sorted = sortedAlong(boxes, acrossX);
    const LayoutBox& first = sorted.front();
    if (mode == "row" || mode == "column") {
        // One line from the first box, `gap` apart, their centers on the first one's.
        double at = acrossX ? first.right() + gap : first.bottom() + gap;
        for (std::size_t i = 1; i < sorted.size(); ++i) {
            const LayoutBox& b = sorted[i];
            auto& [x, y] = out[b.name];
            if (acrossX) {
                x = at;
                y = first.centerY() - b.h * 0.5;
                at += b.w + gap;
            } else {
                y = at;
                x = first.centerX() - b.w * 0.5;
                at += b.h + gap;
            }
        }
        return out;
    }
    // Distribute: the two outer boxes stay, the gaps between all of them made equal.
    const LayoutBox& last = sorted.back();
    double sizes = 0.0;
    for (const LayoutBox& b : sorted) sizes += acrossX ? b.w : b.h;
    const double span = acrossX ? last.right() - first.x : last.bottom() - first.y;
    const double step = (span - sizes) / static_cast<double>(sorted.size() - 1);
    double at = (acrossX ? first.right() : first.bottom()) + step;
    for (std::size_t i = 1; i + 1 < sorted.size(); ++i) {
        const LayoutBox& b = sorted[i];
        auto& [x, y] = out[b.name];
        (acrossX ? x : y) = at;
        at += (acrossX ? b.w : b.h) + step;
    }
    return out;
}

std::map<std::string, std::pair<double, double>> arrangeSelection(const GraphEdit& edit, const std::vector<LayoutBox>& selected,
                                                                  double column, double row) {
    std::set<std::string> wanted;
    double left = 0.0, top = 0.0;
    bool any = false;
    for (const LayoutBox& b : selected) {
        if (!edit.hasNode(b.name)) continue;
        wanted.insert(b.name);
        left = any ? std::min(left, b.x) : b.x;
        top = any ? std::min(top, b.y) : b.y;
        any = true;
    }
    Positions out;
    if (!any) return out;
    // The selection as a graph of its own: its nodes and the links among them.
    const json g = json::parse(edit.toJson());
    json sub = {{"nodes", json::array()}, {"connections", json::array()}};
    for (const json& n : g.value("nodes", json::array())) {
        if (n.is_object() && wanted.count(n.value("name", std::string()))) sub["nodes"].push_back({{"name", n["name"]}, {"type", n.value("type", std::string())}});
    }
    for (const json& c : g.value("connections", json::array())) {
        if (c.is_object() && wanted.count(c.value("from", std::string())) && wanted.count(c.value("to", std::string()))) sub["connections"].push_back(c);
    }
    GraphLayout layout;
    layout.column = column;
    layout.row = row;
    for (const auto& [name, at] : layoutPositions(GraphEdit(sub.dump()), layout)) out[name] = {left + at.first, top + at.second};
    return out;
}

bool onBackdrop(const LayoutBox& backdrop, const LayoutBox& node) {
    const double cx = node.centerX(), cy = node.centerY();
    return cx >= backdrop.x && cx <= backdrop.right() && cy >= backdrop.y && cy <= backdrop.bottom();
}

LayoutBox grownBackdrop(const LayoutBox& backdrop, const std::vector<LayoutBox>& nodes, double margin, double header) {
    LayoutBox out = backdrop;
    double right = out.right(), bottom = out.bottom();
    for (const LayoutBox& n : nodes) {
        out.x = std::min(out.x, n.x - margin);
        out.y = std::min(out.y, n.y - margin - header);
        right = std::max(right, n.right() + margin);
        bottom = std::max(bottom, n.bottom() + margin);
    }
    out.w = right - out.x;
    out.h = bottom - out.y;
    return out;
}

LayoutBox foldedBackdrop(const LayoutBox& backdrop, double header, double minWidth, double maxWidth) {
    LayoutBox out = backdrop;
    out.w = std::clamp(backdrop.w, minWidth, std::max(minWidth, maxWidth));
    out.h = header + 16.0;
    return out;
}

double backdropHeader(const json& backdrop) {
    const json size = backdrop.is_object() ? backdrop.value("fontSize", json()) : json();
    const double points = size.is_number() ? size.get<double>() : 0.0;
    return points > 0.0 ? std::max(28.0, points * 1.9) : 28.0;
}

double backdropContentTop(const json& backdrop) {
    double top = backdropHeader(backdrop);
    const std::string text = backdrop.is_object() ? backdrop.value("description", std::string()) : std::string();
    if (text.empty()) return top;
    // Wrapped to the backdrop's width (12 pt text: about 6.5 a character), 16 a line.
    const double perLine = std::max(8.0, (backdrop.value("w", 300.0) - 24.0) / 6.5);
    int lines = 0;
    std::size_t start = 0;
    while (start <= text.size()) {
        const std::size_t end = std::min(text.find('\n', start), text.size());
        lines += std::max(1, static_cast<int>(std::ceil(static_cast<double>(end - start) / perLine)));
        start = end + 1;
    }
    return top + 6.0 + lines * 16.0;
}

double backdropTitleWidth(const json& backdrop) {
    const json size = backdrop.is_object() ? backdrop.value("fontSize", json()) : json();
    const double points = size.is_number() && size.get<double>() > 0.0 ? size.get<double>() : 14.0;
    const std::string title = backdrop.is_object() ? backdrop.value("title", std::string()) : std::string();
    std::size_t chars = 0;
    for (const unsigned char c : title) chars += (c & 0xC0) != 0x80 ? 1 : 0;  // (UTF-8: characters)
    return chars * points * 0.62 + 56.0;
}

bool backdropFolded(const json& backdrop) {
    if (!backdrop.is_object()) return false;
    const auto it = backdrop.find("collapsed");
    return it != backdrop.end() && it->is_boolean() && it->get<bool>();
}

namespace {

// The graph's links as (from, to) pairs, in its order, without those drawn out of a portal's exit
// (teleported: what feeds the portal is somewhere else).
std::vector<std::pair<std::string, std::string>> drawnLinks(const json& graph) {
    std::vector<std::pair<std::string, std::string>> out;
    for (const json& c : graph.value("connections", json::array())) {
        if (!c.is_object()) continue;
        const std::string from = c.value("from", std::string()), to = c.value("to", std::string());
        const auto portal = portalOfLink(graph, to, c.value("input", std::string()));
        if (portal && portal->exit >= 0) continue;
        out.emplace_back(from, to);
    }
    return out;
}

std::vector<std::string> nodesWith(const GraphEdit& edit, const std::vector<std::string>& names, bool down) {
    const json graph = json::parse(edit.toJson());
    const auto links = drawnLinks(graph);
    std::set<std::string> carried;
    std::vector<std::string> todo;
    for (const std::string& n : names) {
        if (edit.hasNode(n) && carried.insert(n).second) todo.push_back(n);
    }
    while (!todo.empty()) {
        const std::string n = todo.back();
        todo.pop_back();
        for (const auto& [from, to] : links) {
            const std::string& next = down ? to : from;
            if ((down ? from : to) == n && edit.hasNode(next) && carried.insert(next).second) todo.push_back(next);
        }
    }
    std::vector<std::string> out;
    for (const std::string& n : edit.nodeNames()) {
        if (carried.count(n)) out.push_back(n);
    }
    return out;
}

bool pointOn(const LayoutBox& box, double x, double y) { return x >= box.x && x <= box.right() && y >= box.y && y <= box.bottom(); }

}  // namespace

std::vector<std::string> nodesWithUpstream(const GraphEdit& edit, const std::vector<std::string>& names) {
    return nodesWith(edit, names, false);
}

std::vector<std::string> nodesWithDownstream(const GraphEdit& edit, const std::vector<std::string>& names) {
    return nodesWith(edit, names, true);
}

CarriedLayout carriedWith(const json& graph, const std::vector<std::string>& nodes) {
    CarriedLayout out;
    const std::set<std::string> moved(nodes.begin(), nodes.end());
    for (const std::string& pin : pinIds(graph)) {
        const auto source = pinSource(graph, pin);
        if (!source || !moved.count(source->from)) continue;
        bool all = true;
        for (const PinLink& l : pinLinks(graph, pin)) all = all && moved.count(l.to) > 0;
        if (all) out.pins.push_back(pin);
    }
    const json portals = graph.value("portals", json::array());
    for (const json& p : portals.is_array() ? portals : json::array()) {
        if (!p.is_object()) continue;
        const std::string name = p.value("name", std::string());
        auto allMoved = [&](const json& links) {
            for (const json& l : links.is_array() ? links : json::array()) {
                if (!moved.count(l.value("to", std::string()))) return false;
            }
            return true;
        };
        if (moved.count(p.value("from", std::string())) && allMoved(p.value("links", json::array()))) out.dots.emplace_back(name, -1);
        const json exits = p.value("exits", json::array());
        for (std::size_t e = 0; e < exits.size(); ++e) {
            const json links = exits[e].value("links", json::array());
            if (links.is_array() && !links.empty() && allMoved(links)) out.dots.emplace_back(name, static_cast<int>(e));
        }
    }
    return out;
}

CarriedLayout layoutOnBackdrop(const json& graph, const LayoutBox& backdrop) {
    CarriedLayout out;
    for (const std::string& pin : pinIds(graph)) {
        double x = 0.0, y = 0.0;
        if (pinPosition(graph, pin, &x, &y) && pointOn(backdrop, x, y)) out.pins.push_back(pin);
    }
    const json portals = graph.value("portals", json::array());
    for (const json& p : portals.is_array() ? portals : json::array()) {
        if (!p.is_object()) continue;
        const std::string name = p.value("name", std::string());
        if (pointOn(backdrop, p.value("x", 0.0), p.value("y", 0.0))) out.dots.emplace_back(name, -1);
        const json exits = p.value("exits", json::array());
        for (std::size_t e = 0; e < exits.size(); ++e) {
            if (exits[e].is_object() && pointOn(backdrop, exits[e].value("x", 0.0), exits[e].value("y", 0.0))) out.dots.emplace_back(name, static_cast<int>(e));
        }
    }
    return out;
}

std::vector<std::string> alignGestureModes() { return {"chain", "selected", "distribute"}; }

std::string alignGestureAxis(double dx, double dy, double threshold) {
    if (std::hypot(dx, dy) < threshold) return {};
    return std::abs(dx) >= std::abs(dy) ? "row" : "column";
}

Positions alignGesture(const GraphEdit& edit, const std::vector<LayoutBox>& boxes, const std::string& anchor,
                       const std::vector<std::string>& selected, const std::string& mode, double dx, double dy, double gap) {
    Positions out;
    const std::string axis = alignGestureAxis(dx, dy);
    if (axis.empty()) return out;
    const bool row = axis == "row";
    std::map<std::string, LayoutBox> byName;
    for (const LayoutBox& b : boxes) byName[b.name] = b;
    const auto at = byName.find(anchor);
    if (at == byName.end()) return out;
    const LayoutBox a = at->second;
    if (mode == "chain") {
        // Each node's first fed input's source (upstream), or its first reader (downstream), not
        // through a portal's exit, until the chain ends (or comes round).
        const bool up = row ? dx < 0.0 : dy < 0.0;
        const json graph = json::parse(edit.toJson());
        const auto links = drawnLinks(graph);
        std::vector<std::string> chain;
        std::set<std::string> seen{anchor};
        std::string n = anchor;
        for (;;) {
            std::string next;
            for (const auto& [from, to] : links) {
                if ((up ? to : from) == n) {
                    next = up ? from : to;
                    break;
                }
            }
            if (next.empty() || !seen.insert(next).second || !byName.count(next)) break;
            chain.push_back(next);
            n = next;
        }
        double edge = up ? a.y : a.bottom();  // (a column: stacked from the anchor out)
        for (const std::string& c : chain) {
            const LayoutBox& b = byName[c];
            if (row) {
                out[c] = {b.x, a.centerY() - b.h * 0.5};
            } else {
                const double y = up ? edge - gap - b.h : edge + gap;
                out[c] = {a.centerX() - b.w * 0.5, y};
                edge = up ? y : y + b.h;
            }
        }
        return out;
    }
    std::vector<LayoutBox> chosen;
    for (const std::string& s : selected) {
        if (s != anchor && byName.count(s)) chosen.push_back(byName[s]);
    }
    if (mode == "selected") {
        for (const LayoutBox& b : chosen) out[b.name] = row ? std::make_pair(b.x, a.centerY() - b.h * 0.5) : std::make_pair(a.centerX() - b.w * 0.5, b.y);
        return out;
    }
    if (mode == "distribute") {
        chosen.push_back(a);
        if (chosen.size() < 3) return out;
        auto center = [&](const LayoutBox& b) { return row ? b.centerX() : b.centerY(); };
        std::stable_sort(chosen.begin(), chosen.end(), [&](const LayoutBox& l, const LayoutBox& r) { return center(l) < center(r); });
        const double first = center(chosen.front());
        const double step = (center(chosen.back()) - first) / static_cast<double>(chosen.size() - 1);
        double shift = 0.0;  // the anchor stays where it is
        for (std::size_t i = 0; i < chosen.size(); ++i) {
            if (chosen[i].name == anchor) shift = center(a) - (first + step * static_cast<double>(i));
        }
        for (std::size_t i = 0; i < chosen.size(); ++i) {
            const LayoutBox& b = chosen[i];
            const double c = first + step * static_cast<double>(i) + shift;
            if (b.name == anchor) continue;
            out[b.name] = row ? std::make_pair(c - b.w * 0.5, b.y) : std::make_pair(b.x, c - b.h * 0.5);
        }
        return out;
    }
    throw GraphError("unknown align gesture '" + mode + "'");
}


std::vector<std::string> wireStyles() { return {"smooth", "square", "rounded"}; }

namespace {

struct Point {
    double x, y;
};

// A polyline's steps with each corner rounded to at most `radius` (half of either run at most).
std::vector<WireStep> polylineSteps(std::vector<Point> points, double radius) {
    // Runs too short to see and points in the middle of a straight run go.
    std::vector<Point> p;
    for (const Point& q : points) {
        if (!p.empty() && std::abs(q.x - p.back().x) < 1e-6 && std::abs(q.y - p.back().y) < 1e-6) continue;
        if (p.size() >= 2) {
            const Point& a = p[p.size() - 2];
            const Point& b = p.back();
            const double cross = (b.x - a.x) * (q.y - b.y) - (b.y - a.y) * (q.x - b.x);
            const double dot = (b.x - a.x) * (q.x - b.x) + (b.y - a.y) * (q.y - b.y);
            if (std::abs(cross) < 1e-6 && dot > 0.0) p.back() = q;  // straight on: one run
            else p.push_back(q);
        } else {
            p.push_back(q);
        }
    }
    std::vector<WireStep> steps;
    if (p.size() < 2) return steps;
    constexpr double kappa = 0.5523;  // a quarter circle as a cubic
    for (std::size_t i = 1; i < p.size(); ++i) {
        const Point& a = p[i - 1];
        const Point& b = p[i];
        if (i + 1 == p.size() || radius <= 0.0) {
            steps.push_back({false, 0, 0, 0, 0, b.x, b.y});
            continue;
        }
        const Point& c = p[i + 1];
        const double l1 = std::hypot(b.x - a.x, b.y - a.y), l2 = std::hypot(c.x - b.x, c.y - b.y);
        const double r = std::min({radius, l1 * 0.5, l2 * 0.5});
        const double d1x = (b.x - a.x) / l1, d1y = (b.y - a.y) / l1, d2x = (c.x - b.x) / l2, d2y = (c.y - b.y) / l2;
        const Point start{b.x - d1x * r, b.y - d1y * r}, end{b.x + d2x * r, b.y + d2y * r};
        steps.push_back({false, 0, 0, 0, 0, start.x, start.y});
        steps.push_back({true, start.x + d1x * r * kappa, start.y + d1y * r * kappa, end.x - d2x * r * kappa, end.y - d2y * r * kappa, end.x, end.y});
        // The next run starts at the corner's end: its line is drawn from there.
        p[i] = end;
    }
    return steps;
}

}  // namespace

double wireGap(double topA, double bottomA, double topB, double bottomB, double margin) {
    if (bottomA + margin <= topB) return 0.5 * (bottomA + topB);
    if (bottomB + margin <= topA) return 0.5 * (bottomB + topA);
    return std::nan("");
}

std::vector<WireStep> wireRoute(const std::string& style, double ax, double ay, double bx, double by, double below, bool through,
                                double gap) {
    // (Its input right of its output, however little: straight across. Before issue 131, within 30
    // units it looped under the cards, a knot between cards side by side.)
    const bool forward = through || bx >= ax;
    // Where a backwards wire runs back: between the cards when there is room, else under them.
    const double back = !std::isnan(gap) ? gap : std::isnan(below) ? std::max(ay, by) + 60.0 : below + 24.0;
    if (style == "square" || style == "rounded") {
        const double radius = style == "rounded" ? 14.0 : 0.0;
        if (forward) {
            const double mid = (ax + bx) * 0.5;
            return polylineSteps({{ax, ay}, {mid, ay}, {mid, by}, {bx, by}}, radius);
        }
        // Backwards: out to the right, down (between the cards, or under both), back along, into the input.
        const double y = back;
        constexpr double out = 24.0;
        return polylineSteps({{ax, ay}, {ax + out, ay}, {ax + out, y}, {bx - out, y}, {bx - out, by}, {bx, by}}, radius);
    }
    if (forward) {
        // Its handles half the way across, and longer by the drop between its ends (up to 50 or 60):
        // a wide or steep wire curves as it did, a short level one runs straight (handles past the
        // other end double back: the knot of issue 131).
        const double across = std::abs(bx - ax) * 0.5;
        const double dx = std::max(across, std::min(through ? 50.0 : 60.0, across + std::abs(by - ay) * 0.5));
        return {{true, ax + dx, ay, bx - dx, by, bx, by}};
    }
    const double y = back;
    constexpr double r = 40.0;
    return {{true, ax + r, ay, ax + r, y, ax, y}, {false, 0, 0, 0, 0, bx, y}, {true, bx - r, y, bx - r, by, bx, by}};
}

}  // namespace groomlab

namespace groomlab {

bool backdropKeeps(bool overIt, double speed, bool left) {
    if (left) return false;
    return overIt || speed < kBackdropEscapeSpeed;
}

}  // namespace groomlab

namespace groomlab {

namespace {

using json = nlohmann::json;
using Offset = std::pair<double, double>;

// Boxes (only their sizes count) laid out in columns by the longest path in along `edges`
// (index pairs, from -> to), each placed level with the mean of what feeds it where the column
// leaves room. Only edges going up in `rank` count (a group holding both ends of a chain would
// loop otherwise: it goes where its first node would). Top-lefts from (0, 0).
std::vector<Offset> layeredLayout(const std::vector<LayoutBox>& units, const std::vector<std::pair<int, int>>& edges,
                                  const std::vector<int>& rank, double gapX, double gapY) {
    const int n = static_cast<int>(units.size());
    std::vector<std::vector<int>> preds(n);
    for (const auto& [a, b] : edges) {
        if (a == b || a < 0 || b < 0 || a >= n || b >= n || rank[a] >= rank[b]) continue;
        if (std::find(preds[b].begin(), preds[b].end(), a) == preds[b].end()) preds[b].push_back(a);
    }
    std::vector<int> depth(n, 0), state(n, 0);
    std::vector<std::vector<int>> kept(n);  // the links that count (no loop's back link)
    std::function<void(int)> visit = [&](int v) {
        state[v] = 1;
        for (const int p : preds[v]) {
            if (state[p] == 1) continue;
            if (state[p] == 0) visit(p);
            kept[v].push_back(p);
            depth[v] = std::max(depth[v], depth[p] + 1);
        }
        state[v] = 2;
    };
    for (int v = 0; v < n; ++v) {
        if (state[v] == 0) visit(v);
    }
    const int columns = n ? *std::max_element(depth.begin(), depth.end()) + 1 : 0;
    std::vector<Offset> out(n, {0.0, 0.0});
    double x = 0.0;
    for (int c = 0; c < columns; ++c) {
        std::vector<std::pair<double, int>> column;  // (wanted center y, unit)
        double width = 0.0;
        for (int v = 0; v < n; ++v) {
            if (depth[v] != c) continue;
            width = std::max(width, units[v].w);
            double sum = 0.0;
            int count = 0;
            for (const int p : kept[v]) {
                sum += out[p].second + units[p].h * 0.5;
                ++count;
            }
            column.push_back({count ? sum / count : -1e300, v});
        }
        std::stable_sort(column.begin(), column.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
        double below = -1e300;
        for (const auto& [want, v] : column) {
            double top = want <= -1e299 ? 0.0 : want - units[v].h * 0.5;
            if (below > -1e299) top = std::max(top, below + gapY);
            out[v] = {x, top};
            below = top + units[v].h;
        }
        x += width + gapX;
    }
    double minY = 0.0;
    for (int v = 0; v < n; ++v) minY = v ? std::min(minY, out[v].second) : out[v].second;
    for (Offset& o : out) o.second -= minY;
    return out;
}

LayoutBox backdropBox(const json& b) {
    return {b.value("title", std::string()), b.value("x", 0.0), b.value("y", 0.0), b.value("w", 0.0), b.value("h", 0.0)};
}

}  // namespace

json tidyGraph(const json& graph, const std::vector<LayoutBox>& boxes, const std::vector<std::string>& selected, double gapX, double gapY,
               double margin, double header) {
    json out = graph;
    std::vector<std::string> order;  // the graph's nodes with a box, in its order
    std::map<std::string, LayoutBox> box;
    for (const LayoutBox& b : boxes) box[b.name] = b;
    for (const json& n : graph.value("nodes", json::array())) {
        const std::string name = n.is_object() ? n.value("name", std::string()) : std::string();
        if (box.count(name)) order.push_back(name);
    }
    std::set<std::string> tidied;
    for (const std::string& n : selected.empty() ? order : selected) {
        if (box.count(n)) tidied.insert(n);
    }
    if (tidied.empty()) return out;

    // The groups: each node on the smallest backdrop holding its card's center.
    const json noBackdrops = json::array();
    const json& backdrops = graph.contains("backdrops") && graph["backdrops"].is_array() ? graph["backdrops"] : noBackdrops;
    std::vector<LayoutBox> backdropBoxes;
    for (const json& b : backdrops) backdropBoxes.push_back(b.is_object() ? backdropBox(b) : LayoutBox{});
    const int nb = static_cast<int>(backdropBoxes.size());
    std::map<std::string, int> holder;
    std::vector<std::vector<std::string>> held(nb), heldTidied(nb);
    for (const std::string& name : order) {
        int best = -1;
        for (int i = 0; i < nb; ++i) {
            const LayoutBox& b = backdropBoxes[i];
            if (b.w <= 0.0 || b.h <= 0.0 || !onBackdrop(b, box[name])) continue;
            if (best < 0 || b.w * b.h < backdropBoxes[best].w * backdropBoxes[best].h) best = i;
        }
        holder[name] = best;
        if (best >= 0) {
            held[best].push_back(name);
            if (tidied.count(name)) heldTidied[best].push_back(name);
        }
    }
    const auto links = drawnLinks(graph);
    // Each node's longest path in, over the whole graph (a group's rank: its first node's).
    std::map<std::string, int> depth;
    {
        std::map<std::string, std::vector<std::string>> feeds;
        for (const auto& [from, to] : links) feeds[to].push_back(from);
        std::map<std::string, int> state;
        std::function<int(const std::string&)> visit = [&](const std::string& v) -> int {
            int& st = state[v];
            if (st == 1) return 0;  // (a loop: none in a graph, but never forever)
            if (st == 2) return depth[v];
            st = 1;
            int d = 0;
            for (const std::string& p : feeds[v]) d = std::max(d, visit(p) + 1);
            state[v] = 2;
            return depth[v] = d;
        };
        for (const std::string& name : order) visit(name);
    }
    // Nodes laid out among themselves by their links: top-lefts from (0, 0).
    auto layoutOf = [&](const std::vector<std::string>& names) {
        std::vector<LayoutBox> units;
        std::map<std::string, int> index;
        for (const std::string& n : names) {
            index[n] = static_cast<int>(units.size());
            units.push_back(box[n]);
        }
        std::vector<std::pair<int, int>> edges;
        for (const auto& [from, to] : links) {
            const auto a = index.find(from), b = index.find(to);
            if (a != index.end() && b != index.end()) edges.emplace_back(a->second, b->second);
        }
        std::vector<int> rank;
        for (const std::string& n : names) rank.push_back(depth[n]);
        const std::vector<Offset> at = layeredLayout(units, edges, rank, gapX, gapY);
        std::map<std::string, Offset> placed;
        for (std::size_t i = 0; i < names.size(); ++i) placed[names[i]] = at[i];
        return placed;
    };

    std::map<std::string, Offset> moved;  // each node's new top-left
    std::map<int, LayoutBox> newBackdrop;
    std::set<int> carriedBackdrops;  // moved as one card: their pins and dots go with them
    // The outer layout's cards: loose nodes and whole groups.
    struct Card {
        LayoutBox box;                          // where it is now (its size the laid-out one)
        int backdrop = -1;                      // a group's backdrop
        std::map<std::string, Offset> members;  // a group's nodes, from its top-left
    };
    std::vector<Card> cards;
    std::map<std::string, int> cardOf;
    std::set<int> grouped;
    for (const std::string& name : order) {
        if (!tidied.count(name)) continue;
        const int h = holder[name];
        if (h < 0) {
            cardOf[name] = static_cast<int>(cards.size());
            cards.push_back({box[name], -1, {}});
            continue;
        }
        if (heldTidied[h].size() != held[h].size()) continue;  // a part of a group: below
        if (!grouped.insert(h).second) {
            cardOf[name] = cardOf[held[h].front()];
            continue;
        }
        Card card;
        card.backdrop = h;
        card.box = backdropBoxes[h];
        if (backdropFolded(backdrops[h])) {
            for (const std::string& m : held[h]) card.members[m] = {box[m].x - card.box.x, box[m].y - card.box.y};
        } else {
            const double top = std::max(header, backdropContentTop(backdrops[h]));
            double w = 0.0, hgt = 0.0;
            for (const auto& [m, at] : layoutOf(held[h])) {
                card.members[m] = {margin + at.first, top + margin + at.second};
                w = std::max(w, at.first + box[m].w);
                hgt = std::max(hgt, at.second + box[m].h);
            }
            card.box.w = std::max(w + margin * 2.0, backdropTitleWidth(backdrops[h]));
            card.box.h = hgt + margin * 2.0 + top;
        }
        for (const std::string& m : held[h]) cardOf[m] = static_cast<int>(cards.size());
        cards.push_back(std::move(card));
    }
    // A group only partly tidied: its tidied nodes laid out where they are on it, the backdrop grown.
    for (int h = 0; h < nb; ++h) {
        if (heldTidied[h].empty() || heldTidied[h].size() == held[h].size()) continue;
        double left = 0.0, top = 0.0;
        for (std::size_t i = 0; i < heldTidied[h].size(); ++i) {
            const LayoutBox& b = box[heldTidied[h][i]];
            left = i ? std::min(left, b.x) : b.x;
            top = i ? std::min(top, b.y) : b.y;
        }
        std::vector<LayoutBox> now;
        for (const auto& [m, at] : layoutOf(heldTidied[h])) {
            moved[m] = {left + at.first, top + at.second};
            now.push_back({m, left + at.first, top + at.second, box[m].w, box[m].h});
        }
        newBackdrop[h] = grownBackdrop(backdropBoxes[h], now, margin, std::max(header, backdropContentTop(backdrops[h])));
    }
    // The outer layout, its top-left where the cards' was.
    if (!cards.empty()) {
        double left = 0.0, top = 0.0;
        std::vector<LayoutBox> units;
        for (std::size_t i = 0; i < cards.size(); ++i) {
            left = i ? std::min(left, cards[i].box.x) : cards[i].box.x;
            top = i ? std::min(top, cards[i].box.y) : cards[i].box.y;
            units.push_back(cards[i].box);
        }
        std::vector<std::pair<int, int>> edges;
        for (const auto& [from, to] : links) {
            const auto a = cardOf.find(from), b = cardOf.find(to);
            if (a != cardOf.end() && b != cardOf.end()) edges.emplace_back(a->second, b->second);
        }
        std::vector<int> rank(cards.size(), 1 << 30);
        for (const auto& [node, card] : cardOf) rank[card] = std::min(rank[card], depth[node]);
        const std::vector<Offset> at = layeredLayout(units, edges, rank, gapX, gapY);
        for (std::size_t i = 0; i < cards.size(); ++i) {
            const double x = left + at[i].first, y = top + at[i].second;
            const Card& card = cards[i];
            if (card.backdrop < 0) {
                moved[card.box.name] = {x, y};
                continue;
            }
            LayoutBox b = card.box;
            b.x = x;
            b.y = y;
            newBackdrop[card.backdrop] = b;
            carriedBackdrops.insert(card.backdrop);
            for (const auto& [m, off] : card.members) moved[m] = {x + off.first, y + off.second};
        }
    }

    // Written back: the nodes, the backdrops, then the pins and dots.
    for (json& n : out["nodes"]) {
        const auto it = moved.find(n.value("name", std::string()));
        if (it != moved.end()) n["ui"] = {{"x", it->second.first}, {"y", it->second.second}};
    }
    for (const auto& [i, b] : newBackdrop) {
        json& j = out["backdrops"][i];
        j["x"] = b.x;
        j["y"] = b.y;
        j["w"] = b.w;
        j["h"] = b.h;
    }
    auto shift = [&](const std::string& node) -> Offset {
        const auto it = moved.find(node);
        if (it == moved.end()) return {0.0, 0.0};
        return {it->second.first - box[node].x, it->second.second - box[node].y};
    };
    std::map<std::string, Offset> pinShift;
    std::map<std::pair<std::string, int>, Offset> dotShift;
    for (const int i : carriedBackdrops) {
        const Offset d{newBackdrop[i].x - backdropBoxes[i].x, newBackdrop[i].y - backdropBoxes[i].y};
        const CarriedLayout on = layoutOnBackdrop(graph, backdropBoxes[i]);
        for (const std::string& p : on.pins) pinShift.emplace(p, d);
        for (const auto& dot : on.dots) dotShift.emplace(dot, d);
    }
    auto mean = [&](const std::vector<std::string>& ends) -> Offset {
        double x = 0.0, y = 0.0;
        for (const std::string& e : ends) {
            const Offset d = shift(e);
            x += d.first;
            y += d.second;
        }
        return ends.empty() ? Offset{0.0, 0.0} : Offset{x / ends.size(), y / ends.size()};
    };
    for (const std::string& pin : pinIds(graph)) {
        if (pinShift.count(pin)) continue;
        std::vector<std::string> ends;
        if (const auto source = pinSource(graph, pin)) ends.push_back(source->from);
        for (const PinLink& l : pinLinks(graph, pin)) ends.push_back(l.to);
        pinShift[pin] = mean(ends);
    }
    for (const auto& [pin, d] : pinShift) {
        double x = 0.0, y = 0.0;
        if ((d.first != 0.0 || d.second != 0.0) && pinPosition(graph, pin, &x, &y)) movePin(out, pin, x + d.first, y + d.second);
    }
    if (out.contains("portals") && out["portals"].is_array()) {
        for (json& p : out["portals"]) {
            if (!p.is_object()) continue;
            const std::string name = p.value("name", std::string());
            auto place = [&](json& at, const Offset& d) {
                if (d.first == 0.0 && d.second == 0.0) return;
                at["x"] = at.value("x", 0.0) + d.first;
                at["y"] = at.value("y", 0.0) + d.second;
            };
            const auto entry = dotShift.find({name, -1});
            if (entry != dotShift.end()) {
                place(p, entry->second);
            } else {
                std::vector<std::string> ends{p.value("from", std::string())};
                place(p, mean(ends));
            }
            if (!p.contains("exits") || !p["exits"].is_array()) continue;
            for (std::size_t e = 0; e < p["exits"].size(); ++e) {
                json& exit = p["exits"][e];
                if (!exit.is_object()) continue;
                const auto known = dotShift.find({name, static_cast<int>(e)});
                if (known != dotShift.end()) {
                    place(exit, known->second);
                    continue;
                }
                std::vector<std::string> ends;
                for (const json& l : exit.value("links", json::array())) ends.push_back(l.value("to", std::string()));
                place(exit, mean(ends));
            }
        }
    }
    return out;
}

}  // namespace groomlab

namespace groomlab {

json clearBackdropTitle(const json& graph, int index, const std::vector<LayoutBox>& boxes, double margin) {
    json out = graph;
    if (!graph.contains("backdrops") || !graph["backdrops"].is_array() || index < 0 ||
        index >= static_cast<int>(graph["backdrops"].size()))
        return out;
    const json& b = graph["backdrops"][index];
    if (!b.is_object() || backdropFolded(b)) return out;
    const LayoutBox backdrop{b.value("title", std::string()), b.value("x", 0.0), b.value("y", 0.0), b.value("w", 0.0), b.value("h", 0.0)};
    std::set<std::string> held;
    double topmost = 0.0;
    for (const LayoutBox& n : boxes) {
        if (!onBackdrop(backdrop, n)) continue;
        topmost = held.empty() ? n.y : std::min(topmost, n.y);
        held.insert(n.name);
    }
    json& to = out["backdrops"][index];
    const double width = std::max(backdrop.w, backdropTitleWidth(b));
    const double push = held.empty() ? 0.0 : backdrop.y + backdropContentTop(b) + margin - topmost;
    if (push > 0.0) {
        const CarriedLayout on = layoutOnBackdrop(graph, backdrop);
        for (json& n : out["nodes"]) {
            if (!n.is_object() || !held.count(n.value("name", std::string()))) continue;
            const auto at = std::find_if(boxes.begin(), boxes.end(), [&](const LayoutBox& x) { return x.name == n["name"]; });
            n["ui"] = {{"x", at->x}, {"y", at->y + push}};
        }
        for (const std::string& pin : on.pins) {
            double x = 0.0, y = 0.0;
            if (pinPosition(graph, pin, &x, &y)) movePin(out, pin, x, y + push);
        }
        for (const auto& [portal, exit] : on.dots) {
            for (json& p : out["portals"]) {
                if (!p.is_object() || p.value("name", std::string()) != portal) continue;
                json& dot = exit < 0 ? p : p["exits"][exit];
                dot["y"] = dot.value("y", 0.0) + push;
            }
        }
        to["h"] = backdrop.h + push;
    }
    if (width > backdrop.w) to["w"] = width;
    return out;
}

}  // namespace groomlab
