// SPDX-License-Identifier: Apache-2.0
// Arranging a graph by hand, every host's node editor's (graph_arrange.h).

#include "groomlab/graph_arrange.h"

#include <algorithm>
#include <set>

#include "groomlab/graph_inserts.h"

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

std::vector<std::string> nodesWithUpstream(const GraphEdit& edit, const std::vector<std::string>& names) {
    std::set<std::string> carried;
    for (const std::string& n : names) {
        if (!edit.hasNode(n)) continue;
        carried.insert(n);
        for (const std::string& up : edit.upstream(n)) carried.insert(up);
    }
    std::vector<std::string> out;
    for (const std::string& n : edit.nodeNames()) {
        if (carried.count(n)) out.push_back(n);
    }
    return out;
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
