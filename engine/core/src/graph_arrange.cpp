// SPDX-License-Identifier: Apache-2.0
// Arranging a graph by hand, every host's node editor's (graph_arrange.h).

#include "groomlab/graph_arrange.h"

#include <algorithm>
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

void ShakeDetector::reset() {
    started_ = false;
    direction_ = 0;
    turnTimes_.clear();
}

bool ShakeDetector::add(double seconds, double x, double /*y*/) {
    if (!started_) {
        started_ = true;
        anchorX_ = extremeX_ = x;
        direction_ = 0;
        turnTimes_.clear();
        return false;
    }
    if (direction_ == 0) {
        if (std::abs(x - anchorX_) >= minTravel_) {
            direction_ = x > anchorX_ ? 1 : -1;
            extremeX_ = x;
        }
        return false;
    }
    if ((x - extremeX_) * direction_ > 0.0) {  // still going the same way
        extremeX_ = x;
        return false;
    }
    if ((extremeX_ - x) * direction_ < minTravel_) return false;  // not back far enough yet
    // A turn: this stroke went at least minTravel one way and has come back as far.
    if (std::abs(extremeX_ - anchorX_) >= minTravel_) turnTimes_.push_back(seconds);
    anchorX_ = extremeX_;
    extremeX_ = x;
    direction_ = -direction_;
    turnTimes_.erase(std::remove_if(turnTimes_.begin(), turnTimes_.end(), [&](double t) { return t < seconds - window_; }), turnTimes_.end());
    if (static_cast<int>(turnTimes_.size()) >= turns_) {
        turnTimes_.clear();
        anchorX_ = extremeX_ = x;
        direction_ = 0;
        return true;
    }
    return false;
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

std::vector<WireStep> wireRoute(const std::string& style, double ax, double ay, double bx, double by, double below, bool through) {
    const bool forward = through || bx >= ax + 30.0;
    if (style == "square" || style == "rounded") {
        const double radius = style == "rounded" ? 14.0 : 0.0;
        if (forward) {
            const double mid = (ax + bx) * 0.5;
            return polylineSteps({{ax, ay}, {mid, ay}, {mid, by}, {bx, by}}, radius);
        }
        // Backwards: out to the right, down under both cards, back along under them, up into the input.
        const double y = std::isnan(below) ? std::max(ay, by) + 60.0 : below + 24.0;
        constexpr double out = 24.0;
        return polylineSteps({{ax, ay}, {ax + out, ay}, {ax + out, y}, {bx - out, y}, {bx - out, by}, {bx, by}}, radius);
    }
    if (forward) {
        const double dx = std::max(through ? 50.0 : 60.0, std::abs(bx - ax) * 0.5);
        return {{true, ax + dx, ay, bx - dx, by, bx, by}};
    }
    const double y = std::isnan(below) ? std::max(ay, by) + 60.0 : below + 24.0;
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
