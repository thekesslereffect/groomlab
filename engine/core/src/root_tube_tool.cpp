// SPDX-License-Identifier: Apache-2.0
// The Tube tool's root-first mode (root_tube_tool.h): Maya's root_tube_tool.py ported to the core,
// so every host's viewport runs the same tool.

#include "groomlab/root_tube_tool.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

#include "groomlab/display.h"
#include "groomlab/stroke.h"
#include "groomlab/tube.h"
#include "groomlab/view_projection.h"

namespace groomlab {

namespace {

constexpr std::array<float, 3> kOutline{1.0f, 0.75f, 0.15f};  // the outline being placed
constexpr std::array<float, 3> kClosing{0.3f, 1.0f, 0.4f};    // ... closed (drawing its path)
constexpr std::array<float, 3> kStrokeColor{0.3f, 0.85f, 1.0f};
constexpr std::array<float, 3> kRootColor{0.85f, 0.85f, 0.9f};  // a root with no color yet

std::string plural(std::size_t n, const char* one, const char* many) { return n == 1 ? one : many; }
std::string ids(const std::vector<GuideId>& v) {
    std::string out;
    for (const GuideId g : v) out += (out.empty() ? "" : ", ") + std::to_string(g);
    return out;
}

struct Hit {
    Vec3f point, normal;
};
std::optional<Hit> hit(const ToolView& view, const ScalpMesh& scalp, float x, float y) {
    Vec3f o, d;
    if (!view.ray(x, y, o, d)) return std::nullopt;
    SurfacePoint sp;
    if (!scalp.intersectRay(o, d, &sp)) return std::nullopt;
    return Hit{sp.position, scalp.contains(sp.root) ? scalp.normal(sp.root) : sp.normal};
}
std::array<float, 2> toScreen(const ToolView& view, const Vec3f& p) {
    float x = 0.0f, y = 0.0f;
    if (!view.project(p, x, y)) return {1e9f, 1e9f};
    return {x, y};
}
float screenDistance(const ToolView& view, const Vec3f& p, float x, float y) {
    const std::array<float, 2> s = toScreen(view, p);
    return std::hypot(s[0] - x, s[1] - y);
}
// View point (x, y) on the plane through `origin` facing the camera.
std::optional<Vec3f> onPlane(const ToolView& view, float x, float y, const Vec3f& origin) {
    Vec3f o, d;
    if (!view.ray(x, y, o, d)) return std::nullopt;
    const float dd = d.dot(d);
    if (dd <= 0.0f) return std::nullopt;
    const float t = (origin - o).dot(d) / dd;
    return o + d * t;
}
float dist(const Vec3f& a, const Vec3f& b) { return (a - b).length(); }
float polyLength(const std::vector<Vec3f>& pts) {
    float total = 0.0f;
    for (std::size_t i = 1; i < pts.size(); ++i) total += dist(pts[i - 1], pts[i]);
    return total;
}
std::uint64_t hashOf(StrokeHost& host) {
    const std::shared_ptr<const GuideHierarchy> h = host.authoredHierarchy();
    return h ? h->contentHash() : 0;
}

}  // namespace

std::array<float, 3> RootTubeTool::edgeColor(const Item& item) {
    if (item.style & Highlight) return kHighlight;
    return {0.6f * item.color[0] + 0.4f, 0.6f * item.color[1] + 0.4f, 0.6f * item.color[2] + 0.4f};
}

// ---- Plumbing ---------------------------------------------------------------------------------------------

void RootTubeTool::say(std::string text) {
    message_ = std::move(text);
    ++messageCount_;
}

void RootTubeTool::reset() {
    cvs_.clear();
    stroke_.clear();
    drag_.clear();
    center_.reset();
    edits_.clear();
    original_.clear();
    touched_.clear();
    current_ = kNoGuide;
    history_.clear();
    roots_.clear();
    middles_.clear();
    move_.reset();
    ghost_.reset();
}

void RootTubeTool::seen(StrokeHost& host) { seen_ = hashOf(host); }

bool RootTubeTool::commit(StrokeHost& host, const std::string& label,
                          const std::function<void(GuideHierarchy&, const ScalpMesh&)>& edit) {
    const std::shared_ptr<const GuideHierarchy> authored = host.authoredHierarchy();
    const MeshPtr scalp = host.scalp();
    if (!scalp) throw std::invalid_argument("the groom has no scalp to root a tube on");
    if (!host.prepareStroke()) throw std::runtime_error("the groom can't be edited now");
    auto h = std::make_shared<GuideHierarchy>(authored ? *authored : GuideHierarchy{});
    edit(*h, *scalp);
    StrokeEdit e;
    e.mode = label;
    e.set(StrokeField::Guides, h->toJson());
    host.commit(std::move(e));
    seen(host);
    return true;
}

namespace {

// The roots the tool works on: those (rootTubesOf) whose guide group shows (a tube hierarchy's
// levels: stepping through them shows that level's roots, cells and halves alone).
std::vector<GuideId> shownRoots(StrokeHost& host, const GuideHierarchy& h, const std::vector<GuideId>& selected = {}) {
    std::vector<GuideId> out = rootTubesOf(h, selected);
    const std::set<std::string> hidden = host.hiddenGroups();
    const MixedView mixed(&h, host.mixedView());
    if (hidden.empty() && !mixed.on()) return out;
    out.erase(std::remove_if(out.begin(), out.end(), [&](GuideId id) {
                  const HierarchyGuide* g = h.guide(id);
                  if (!g) return true;
                  // (Mixed view: the level worked on, and the levels above it does not continue.)
                  const int m = mixed.on() ? mixed.shows(h, *g) : -1;
                  if (m >= 0) return m == 0;
                  const GuideGroup* grp = h.group(g->group);
                  return grp && hidden.count(grp->name) > 0;
              }),
              out.end());
    return out;
}

}  // namespace

RootTubeTool::Outlines RootTubeTool::snapTargets(StrokeHost& host, const std::set<GuideId>& skip) const {
    Outlines out;
    const std::shared_ptr<const GuideHierarchy> h = host.authoredHierarchy();
    const MeshPtr scalp = host.scalp();
    if (!h || !scalp) return out;
    for (const GuideId g : shownRoots(host, *h)) {
        if (skip.count(g)) continue;
        try {
            out.emplace_back(g, rootTubeOutline(*h, *scalp, g));
        } catch (const std::exception&) {
        }
    }
    return out;
}

void RootTubeTool::reloadNeighbours(StrokeHost& host) { neighbours_ = settings.snap ? snapTargets(host) : Outlines{}; }

std::array<float, 3> RootTubeTool::colorOf(GuideId g) const {
    const auto it = colors_.find(g);
    return it == colors_.end() ? kRootColor : it->second;
}

void RootTubeTool::recolor(StrokeHost& host) {
    colors_.clear();
    const std::shared_ptr<const GuideHierarchy> h = host.authoredHierarchy();
    if (!h) return;
    const std::vector<GuideId> tubes = rootTubesOf(*h);
    for (std::size_t k = 0; k < tubes.size(); ++k) colors_[tubes[k]] = rootTubeColor(k);
}

// ---- Starting and leaving ----------------------------------------------------------------------------------

void RootTubeTool::start(StrokeHost& host, Mode mode) {
    if (started_) leave(host);
    reset();
    started_ = true;
    mode_ = mode == Mode::Edit || mode == Mode::Select ? mode : Mode::Place;
    lastMade_.reset();
    restored_.reset();
    seen(host);
    recolor(host);
    if (mode_ == Mode::Edit) {
        beginEdit(host);
    } else if (mode_ == Mode::Select) {
        picked_.clear();
        reloadRoots(host);
        say(roots_.empty() ? "The groom has no root tubes: draw one first."
                           : "Click a root's middle to select it; drag it to move the root over the scalp.");
    } else {
        reloadNeighbours(host);
        say("Click the root's outline on the scalp, one CV at a time; click the first CV to close it.");
    }
}

void RootTubeTool::leave(StrokeHost& host) {
    if (started_ && mode_ == Mode::Edit && !edits_.empty()) {
        bool changed = false;
        for (const auto& [g, cvs] : edits_) changed = changed || cvs != original_[g];
        if (changed) finishEdit(host);
    }
    reset();
    started_ = false;
    mode_ = Mode::Place;
    picked_.clear();
    lastMade_.reset();
    restored_.reset();
    say("");
}

void RootTubeTool::sync(StrokeHost& host) {
    if (!started_) return;
    const std::uint64_t now = hashOf(host);
    if (now == seen_) return;
    seen_ = now;
    const std::shared_ptr<const GuideHierarchy> h = host.authoredHierarchy();
    const std::vector<GuideId> tubes = h ? shownRoots(host, *h) : std::vector<GuideId>{};
    auto has = [&](GuideId g) { return std::find(tubes.begin(), tubes.end(), g) != tubes.end(); };
    if (mode_ == Mode::Select || mode_ == Mode::Duplicate) return reloadRoots(host);
    if (mode_ == Mode::Edit) return reconcileEdits(host);
    // Place and Draw: an undo that took back the tube just made gives its outline back; a redo
    // puts it away again.
    if (lastMade_ && mode_ == Mode::Place && cvs_.empty() && !has(lastMade_->guide)) {
        restored_ = lastMade_;
        lastMade_.reset();
        recolor(host);
        cvs_ = restored_->cvs;
        reloadNeighbours(host);
        if (restored_->drawn) {
            mode_ = Mode::Draw;
            center_ = ScalpPointNormal{restored_->center, restored_->normal};
            stroke_ = {restored_->center};
            say("Tube undone: drag from the root out to draw its path again.");
        } else {
            say("Tube undone: its outline is back; click its first CV to close it again.");
        }
        return;
    }
    if (restored_ && (mode_ == Mode::Place || mode_ == Mode::Draw) && has(restored_->guide)) {
        lastMade_ = restored_;
        restored_.reset();
        recolor(host);
        mode_ = Mode::Place;
        cvs_.clear();
        stroke_.clear();
        center_.reset();
        reloadNeighbours(host);
        say("Tube redone. Draw the next root.");
        return;
    }
    recolor(host);
    reloadNeighbours(host);
}

// ---- Events -------------------------------------------------------------------------------------------------

bool RootTubeTool::press(StrokeHost& host, const ToolView& view, float x, float y, unsigned modifiers) {
    if (!started_) return false;
    sync(host);
    const MeshPtr scalp = host.scalp();
    if (!scalp) {
        say("Select a groom first.");
        return false;
    }
    switch (mode_) {
        case Mode::Edit: {
            if (modifiers & kStrokeCtrl) {
                if (removeCv(view, x, y)) return true;
                insertCv(host, view, x, y);
                return !drag_.empty();
            }
            Vec3f picked;
            std::vector<std::pair<GuideId, int>> owners;
            if (!cvUnder(view, x, y, &picked, &owners)) {
                drag_.clear();
                return false;
            }
            remember();
            drag_ = owners;  // (a CV shared with a neighbour, sealed, moves with it)
            current_ = meant(view, owners, x, y);
            touched_.clear();
            for (const auto& [g, i] : drag_) touched_.insert(g);
            return true;
        }
        case Mode::Select: pressSelect(host, view, x, y, modifiers); return true;
        case Mode::Duplicate: follow(host, view, x, y); return true;
        case Mode::Draw:
            if (center_) stroke_ = {center_->point};
            return true;
        case Mode::Place: break;
    }
    if (cvs_.size() >= 3 && screenDistance(view, cvs_.front(), x, y) <= kPickPixels) {
        close(host);
        return true;
    }
    const std::optional<Hit> at = hit(view, *scalp, x, y);
    if (!at) {
        say("Click on the scalp.");
        return false;
    }
    RootSnap how = RootSnap::None;
    const Vec3f point = snapped(view, at->point, &how);
    lastMade_.reset();
    restored_.reset();  // (a new outline begun: undo and redo no longer touch the last one)
    cvs_.push_back(point);
    if (how != RootSnap::None) {
        say("CV " + std::to_string(cvs_.size()) + " snapped to a neighbouring root's " + (how == RootSnap::Cv ? "CV." : "edge."));
    }
    return true;
}

void RootTubeTool::drag(StrokeHost& host, const ToolView& view, float x, float y) {
    if (!started_) return;
    const MeshPtr scalp = host.scalp();
    if (!scalp) return;
    switch (mode_) {
        case Mode::Edit: {
            if (drag_.empty()) return;
            const std::optional<Hit> at = hit(view, *scalp, x, y);
            if (!at) return;
            Vec3f point = at->point;
            if (settings.snap) {
                // Onto another root's CVs and edges only: never its own (two of its CVs would become one).
                std::set<GuideId> mine;
                for (const auto& [g, i] : drag_) mine.insert(g);
                std::vector<std::vector<Vec3f>> others;
                for (const auto& [g, cvs] : edits_) {
                    if (!mine.count(g) && cvs.size() >= 2) others.push_back(cvs);
                }
                for (const auto& [g, cvs] : neighbours_) {
                    if (!edits_.count(g) && !mine.count(g)) others.push_back(cvs);
                }
                snapRootCv(point, others, [&](const Vec3f& p) { return toScreen(view, p); }, kPickPixels);
            }
            for (const auto& [g, i] : drag_) edits_[g][static_cast<std::size_t>(i)] = point;
            return;
        }
        case Mode::Select: {
            if (!move_) return;
            const std::optional<Hit> at = hit(view, *scalp, x, y);
            if (!at) return;
            const Vec3f to = move_->middle + (at->point - move_->press);
            for (const auto& [g, base] : move_->base) {
                const auto held = move_->held.find(g);
                roots_[g] = carryOutline(*scalp, base, move_->middle, to, held == move_->held.end() ? std::set<int>{} : held->second);
            }
            move_->moved = true;
            return;
        }
        case Mode::Duplicate: follow(host, view, x, y); return;
        case Mode::Draw: {
            if (!center_) return;
            const std::optional<Vec3f> p = onPlane(view, x, y, stroke_.empty() ? center_->point : stroke_.back());
            if (p && (stroke_.empty() || dist(*p, stroke_.back()) > 1e-6f)) stroke_.push_back(*p);
            return;
        }
        case Mode::Place: {
            if (cvs_.empty()) return;
            const std::optional<Hit> at = hit(view, *scalp, x, y);
            if (at) cvs_.back() = snapped(view, at->point);
            return;
        }
    }
}

void RootTubeTool::release(StrokeHost& host, const ToolView& /*view*/) {
    if (!started_) return;
    switch (mode_) {
        case Mode::Draw:
            if (!center_) return;
            if (stroke_.size() < 3 || polyLength(stroke_) < 1e-4f) {
                say("Drag out from the root to draw the tube's path.");
                stroke_ = {center_->point};
                return;
            }
            make(host, stroke_);
            return;
        case Mode::Edit:
            drag_.clear();
            if (!history_.empty() && history_.back() == edits_) history_.pop_back();  // (a click that moved nothing)
            return;
        case Mode::Select: releaseSelect(host); return;
        case Mode::Duplicate: placeCopy(host); return;
        case Mode::Place: return;
    }
}

void RootTubeTool::hover(StrokeHost& host, const ToolView& view, float x, float y) {
    if (started_ && mode_ == Mode::Duplicate) follow(host, view, x, y);
}

// ---- Placing the root's CVs ----------------------------------------------------------------------------------

Vec3f RootTubeTool::snapped(const ToolView& view, const Vec3f& p, RootSnap* how) const {
    Vec3f out = p;
    RootSnap kind = RootSnap::None;
    if (settings.snap && !neighbours_.empty()) {
        std::vector<std::vector<Vec3f>> outlines;
        for (const auto& [g, cvs] : neighbours_) outlines.push_back(cvs);
        kind = snapRootCv(out, outlines, [&](const Vec3f& q) { return toScreen(view, q); }, kPickPixels);
    }
    if (how) *how = kind;
    return out;
}

void RootTubeTool::close(StrokeHost& host) {
    const MeshPtr scalp = host.scalp();
    center_ = rootOutlineCenter(*scalp, cvs_);
    if (settings.drawPath) {
        mode_ = Mode::Draw;
        stroke_ = {center_->point};
        say("Outline closed: drag from the root out to draw the tube's path.");
    } else {
        make(host, {});
    }
}

GuideId RootTubeTool::make(StrokeHost& host, const std::vector<Vec3f>& path) {
    GuideId made = kNoGuide;
    try {
        RootTubeOptions o;
        o.length = settings.length;
        o.segments = settings.segments;
        o.taper = settings.taper;
        o.density = settings.density;
        o.group = settings.group;
        commit(host, "root tube", [&](GuideHierarchy& h, const ScalpMesh& scalp) { made = makeRootTube(h, scalp, cvs_, path, o); });
    } catch (const std::exception& e) {
        say(std::string("The tube wasn't made: ") + e.what());
        mode_ = Mode::Place;
        cvs_.clear();
        stroke_.clear();
        center_.reset();
        return kNoGuide;
    }
    lastMade_ = Made{made, cvs_, center_ ? center_->point : Vec3f{}, center_ ? center_->normal : Vec3f{}, !path.empty()};
    restored_.reset();
    mode_ = Mode::Place;
    cvs_.clear();
    stroke_.clear();
    center_.reset();
    recolor(host);
    reloadNeighbours(host);
    say("Made root tube " + std::to_string(made) + ". Draw the next root.");
    return made;
}

void RootTubeTool::undoCv(StrokeHost& host) {
    sync(host);
    if (mode_ == Mode::Edit) {
        if (history_.empty()) {
            say("No CV move to take back.");
            return;
        }
        const std::map<GuideId, std::vector<Vec3f>> before = history_.back();
        history_.pop_back();
        touched_.clear();
        for (const auto& [g, cvs] : before) {
            const auto it = edits_.find(g);
            if (it == edits_.end()) continue;
            if (cvs != it->second) touched_.insert(g);
            it->second = cvs;
        }
        drag_.clear();
        say("CV move taken back.");
        return;
    }
    if (mode_ == Mode::Draw) {
        mode_ = Mode::Place;
        stroke_.clear();
        center_.reset();
    } else if (mode_ == Mode::Place && !cvs_.empty()) {
        cvs_.pop_back();
    }
}

// ---- Editing roots --------------------------------------------------------------------------------------------

void RootTubeTool::beginEdit(StrokeHost& host) {
    const std::shared_ptr<const GuideHierarchy> h = host.authoredHierarchy();
    const MeshPtr scalp = host.scalp();
    if (!h || !scalp) {
        mode_ = Mode::Place;
        say("Select a groom first.");
        return;
    }
    const std::vector<GuideId> selected = host.selectedGuides();
    std::vector<GuideId> tubes = selected.empty() ? std::vector<GuideId>{} : shownRoots(host, *h, selected);
    scope_ = !tubes.empty();  // (only the selected guides' roots: an undo brings no others in)
    if (tubes.empty()) tubes = shownRoots(host, *h);
    if (tubes.empty()) {
        mode_ = Mode::Place;
        say("The groom has no root tubes to edit: draw one first.");
        return;
    }
    edits_.clear();
    for (const GuideId g : tubes) {
        try {
            edits_[g] = rootTubeOutline(*h, *scalp, g);
        } catch (const std::exception&) {
        }
    }
    original_ = edits_;
    touched_.clear();
    reloadNeighbours(host);
    say("Editing " + std::to_string(edits_.size()) + plural(edits_.size(), " root", " roots") +
        ": drag their CVs on the scalp (Ctrl+click an edge to add one, a CV to take it out), then Finish edit.");
}

float RootTubeTool::tolerance() const {
    float scale = 0.0f;
    for (const auto& [g, cvs] : edits_) {
        for (std::size_t i = 0; i < cvs.size(); ++i) scale = std::max(scale, dist(cvs[i], cvs[(i + 1) % cvs.size()]));
    }
    return 1e-4f * std::max(scale, 1e-6f);
}

void RootTubeTool::remember() {
    history_.push_back(edits_);
    if (history_.size() > 100) history_.erase(history_.begin(), history_.end() - 100);
}

bool RootTubeTool::cvUnder(const ToolView& view, float x, float y, Vec3f* picked, std::vector<std::pair<GuideId, int>>* owners) const {
    float best = kPickPixels;
    bool found = false;
    for (const auto& [g, cvs] : edits_) {
        for (std::size_t i = 0; i < cvs.size(); ++i) {
            const float d = screenDistance(view, cvs[i], x, y);
            if (d <= best && (!found || d < best)) {
                best = d;
                *picked = cvs[i];
                found = true;
            }
        }
    }
    if (!found) return false;
    const float tol = tolerance();
    owners->clear();
    for (const auto& [g, cvs] : edits_) {
        for (std::size_t i = 0; i < cvs.size(); ++i) {
            if (dist(cvs[i], *picked) <= tol) {
                owners->emplace_back(g, static_cast<int>(i));
                break;
            }
        }
    }
    return true;
}

GuideId RootTubeTool::meant(const ToolView& view, const std::vector<std::pair<GuideId, int>>& owners, float x, float y) const {
    std::vector<GuideId> roots, touched;
    for (const auto& [g, i] : owners) {
        roots.push_back(g);
        if (touched_.count(g)) touched.push_back(g);
    }
    if (touched.size() == 1) return touched.front();
    if (std::find(roots.begin(), roots.end(), current_) != roots.end()) return current_;
    GuideId best = roots.front();
    float nearest = 1e30f;
    for (const GuideId g : roots) {
        const float d = screenDistance(view, outlineCentroid(edits_.at(g)), x, y);
        if (d < nearest) nearest = d, best = g;
    }
    return best;
}

bool RootTubeTool::removeCv(const ToolView& view, float x, float y) {
    Vec3f picked;
    std::vector<std::pair<GuideId, int>> owners;
    if (!cvUnder(view, x, y, &picked, &owners)) return false;
    const GuideId g = meant(view, owners, x, y);
    int index = 0;
    for (const auto& [h, i] : owners) {
        if (h == g) index = i;
    }
    drag_.clear();
    if (edits_[g].size() <= 3) {
        say("A root needs at least 3 CVs.");
        return true;
    }
    remember();
    edits_[g].erase(edits_[g].begin() + index);
    current_ = g;
    touched_ = {g};
    say("Took a CV out of root " + std::to_string(g) + (owners.size() > 1 ? " (its neighbour keeps its own)." : "."));
    return true;
}

void RootTubeTool::insertCv(StrokeHost& host, const ToolView& view, float x, float y) {
    float best = kPickPixels;
    bool found = false;
    GuideId g = kNoGuide;
    std::size_t edge = 0;
    float along = 0.0f;
    for (const auto& [h, cvs] : edits_) {
        const std::size_t n = cvs.size();
        for (std::size_t i = 0; i < n; ++i) {
            const std::array<float, 2> a = toScreen(view, cvs[i]), b = toScreen(view, cvs[(i + 1) % n]);
            const float dx = b[0] - a[0], dy = b[1] - a[1];
            const float span = dx * dx + dy * dy;
            const float f = span > 1e-9f ? std::clamp(((x - a[0]) * dx + (y - a[1]) * dy) / span, 0.0f, 1.0f) : 0.0f;
            const float d = std::hypot(a[0] + f * dx - x, a[1] + f * dy - y);
            if (d <= best && (!found || d < best)) {
                best = d;
                found = true;
                g = h;
                edge = i;
                along = f;
            }
        }
    }
    drag_.clear();
    if (!found) {
        say("Ctrl+click on a root's edge to add a CV there.");
        return;
    }
    remember();
    current_ = g;
    const std::vector<Vec3f>& cvs = edits_[g];
    const Vec3f a = cvs[edge], b = cvs[(edge + 1) % cvs.size()];
    const Vec3f point = scalpPointNormal(*host.scalp(), a + (b - a) * along).point;
    const float tol = tolerance();
    int full = 0;
    for (auto& [h, other] : edits_) {
        const std::size_t n = other.size();
        for (std::size_t j = 0; j < n; ++j) {
            const Vec3f p = other[j], q = other[(j + 1) % n];
            if ((dist(p, a) <= tol && dist(q, b) <= tol) || (dist(p, b) <= tol && dist(q, a) <= tol)) {
                if (n >= kMaxTubeShapePoints) {
                    ++full;
                    break;
                }
                other.insert(other.begin() + static_cast<std::ptrdiff_t>(j + 1), point);
                drag_.emplace_back(h, static_cast<int>(j + 1));
                break;
            }
        }
    }
    touched_.clear();
    for (const auto& [h, j] : drag_) touched_.insert(h);
    if (touched_.empty()) touched_ = {g};
    if (full && drag_.empty()) {
        say("A root takes at most " + std::to_string(kMaxTubeShapePoints) + " CVs.");
    } else {
        say(std::string("Added a CV") + (drag_.size() > 1 ? " on " + std::to_string(drag_.size()) + " sealed outlines" : "") +
            ": drag it, then Finish edit.");
    }
}

void RootTubeTool::reconcileEdits(StrokeHost& host) {
    const std::shared_ptr<const GuideHierarchy> h = host.authoredHierarchy();
    const MeshPtr scalp = host.scalp();
    if (!h || !scalp) return;
    const std::vector<GuideId> tubes = shownRoots(host, *h);
    const std::set<GuideId> now(tubes.begin(), tubes.end());
    std::size_t changed = 0;
    for (auto it = edits_.begin(); it != edits_.end();) {
        const GuideId g = it->first;
        if (!now.count(g)) {
            original_.erase(g);
            it = edits_.erase(it);
            ++changed;
            continue;
        }
        try {
            const std::vector<Vec3f> stored = rootTubeOutline(*h, *scalp, g);
            const std::vector<Vec3f>& old = original_[g];
            bool differs = stored.size() != old.size();
            for (std::size_t i = 0; !differs && i < stored.size(); ++i) differs = dist(stored[i], old[i]) > 1e-6f;
            if (differs) {
                it->second = stored;
                original_[g] = stored;
                ++changed;
            }
        } catch (const std::exception&) {
        }
        ++it;
    }
    if (!scope_) {
        for (const GuideId g : tubes) {
            if (edits_.count(g)) continue;
            try {
                edits_[g] = rootTubeOutline(*h, *scalp, g);
                original_[g] = edits_[g];
                ++changed;
            } catch (const std::exception&) {
            }
        }
    }
    drag_.clear();
    history_.clear();
    for (auto it = touched_.begin(); it != touched_.end();) it = edits_.count(*it) ? std::next(it) : touched_.erase(it);
    recolor(host);
    reloadNeighbours(host);
    if (changed) {
        say("Undo changed " + std::to_string(changed) + plural(changed, " root", " roots") + " in the scene: " +
            plural(changed, "it is as the scene has it now", "they are as the scene has them now") + " (Undo CV takes back a CV move).");
    }
}

std::vector<Vec3f> RootTubeTool::cleaned(const std::vector<Vec3f>& cvs) const {
    const float tol = tolerance();
    std::vector<Vec3f> out;
    for (const Vec3f& p : cvs) {
        if (out.empty() || dist(p, out.back()) > tol) out.push_back(p);
    }
    while (out.size() > 1 && dist(out.front(), out.back()) <= tol) out.pop_back();
    return out;
}

int RootTubeTool::finishEdit(StrokeHost& host) {
    if (!started_ || mode_ != Mode::Edit) return 0;
    const std::shared_ptr<const GuideHierarchy> h = host.authoredHierarchy();
    std::set<GuideId> now;
    if (h) {
        const std::vector<GuideId> tubes = shownRoots(host, *h);
        now.insert(tubes.begin(), tubes.end());
    }
    std::vector<GuideId> changed;
    for (const auto& [g, cvs] : edits_) {
        if (now.count(g) && cvs != original_[g]) changed.push_back(g);
    }
    std::vector<GuideId> done;
    std::vector<std::string> failed;
    if (!changed.empty()) {
        try {
            commit(host, "edit root tubes", [&](GuideHierarchy& hh, const ScalpMesh& scalp) {
                for (const GuideId g : changed) {
                    const std::vector<Vec3f> cvs = cleaned(edits_[g]);
                    if (cvs.size() < 3) {
                        failed.push_back("root " + std::to_string(g) + " (fewer than 3 CVs)");
                        continue;
                    }
                    const GuideHierarchy before = hh;
                    try {
                        editRootTube(hh, scalp, g, cvs);
                        done.push_back(g);
                    } catch (const std::exception& e) {  // (one root's trouble never strands the edit)
                        hh = before;
                        failed.push_back("root " + std::to_string(g) + " (" + e.what() + ")");
                    }
                }
            });
        } catch (const std::exception& e) {
            failed.push_back(e.what());
            done.clear();
        }
    }
    edits_.clear();
    original_.clear();
    drag_.clear();
    touched_.clear();
    current_ = kNoGuide;
    history_.clear();
    mode_ = Mode::Place;
    recolor(host);
    reloadNeighbours(host);
    std::string text = "Finished: " + std::to_string(done.size()) + plural(done.size(), " root", " roots") + " changed.";
    if (!failed.empty()) {
        std::string why;
        for (const std::string& f : failed) why += (why.empty() ? "" : "; ") + f;
        text += std::string(" Kept as ") + plural(failed.size(), "it was", "they were") + ": " + why + ".";
    }
    say(text);
    return static_cast<int>(done.size());
}

// ---- Selecting, carrying, duplicating and deleting roots -------------------------------------------------------

void RootTubeTool::reloadRoots(StrokeHost& host) {
    roots_.clear();
    middles_.clear();
    recolor(host);
    const std::shared_ptr<const GuideHierarchy> h = host.authoredHierarchy();
    const MeshPtr scalp = host.scalp();
    if (h && scalp) {
        for (const GuideId g : shownRoots(host, *h)) {
            try {
                const std::vector<Vec3f> cvs = rootTubeOutline(*h, *scalp, g);
                roots_[g] = cvs;
                middles_[g] = rootOutlineCenter(*scalp, cvs);
            } catch (const std::exception&) {
            }
        }
    }
    picked_.erase(std::remove_if(picked_.begin(), picked_.end(), [&](GuideId g) { return !roots_.count(g); }), picked_.end());
    if (picked_.empty() && mode_ == Mode::Duplicate) {
        mode_ = Mode::Select;
        ghost_.reset();
    }
}

Vec3f RootTubeTool::groupMiddle(StrokeHost& host) const {
    Vec3f mean;
    for (const GuideId g : picked_) mean += middles_.at(g).point;
    mean = mean / static_cast<float>(std::max<std::size_t>(picked_.size(), 1));
    return scalpPointNormal(*host.scalp(), mean).point;
}

void RootTubeTool::pressSelect(StrokeHost& host, const ToolView& view, float x, float y, unsigned modifiers) {
    float best = kPickPixels;
    GuideId g = kNoGuide;
    for (const auto& [h, m] : middles_) {
        const float d = screenDistance(view, m.point, x, y);
        if (d <= best && (g == kNoGuide || d < best)) best = d, g = h;
    }
    const bool shift = modifiers & kStrokeShift;
    move_.reset();
    auto count = [&] { return std::to_string(picked_.size()) + plural(picked_.size(), " root", " roots") + " selected."; };
    if (g == kNoGuide) {
        if (!shift) picked_.clear();
        say(picked_.empty() ? "Nothing selected: click a root's middle (Shift+click: add one)." : count());
        return;
    }
    const auto at = std::find(picked_.begin(), picked_.end(), g);
    if (shift && at != picked_.end()) {  // (Shift+click a selected one: out of the selection)
        picked_.erase(at);
        say(count());
        return;
    }
    if (shift) {
        picked_.push_back(g);
    } else if (at == picked_.end()) {
        picked_ = {g};
    } else {  // (a selected one: carried with the others, last picked)
        picked_.erase(at);
        picked_.push_back(g);
    }
    const std::optional<Hit> under = hit(view, *host.scalp(), x, y);
    Move m;
    m.press = under ? under->point : middles_.at(g).point;
    m.middle = groupMiddle(host);
    std::size_t sealedCount = 0;
    std::vector<std::vector<Vec3f>> others;
    if (settings.snap) {
        for (const auto& [h, cvs] : roots_) {
            if (std::find(picked_.begin(), picked_.end(), h) == picked_.end()) others.push_back(cvs);
        }
    }
    for (const GuideId h : picked_) {
        m.base[h] = roots_.at(h);
        if (settings.snap) {
            const std::vector<int> held = sealedCvs(roots_.at(h), others);
            m.held[h] = std::set<int>(held.begin(), held.end());
            sealedCount += held.size();
        }
    }
    move_ = std::move(m);
    const std::string them = plural(picked_.size(), "it", "them");
    say(std::to_string(picked_.size()) + plural(picked_.size(), " root", " roots") + " selected" +
        (sealedCount ? " (" + std::to_string(sealedCount) + plural(sealedCount, " CV", " CVs") + " sealed to a neighbour stay put)" : "") +
        ": drag to move " + them + "; Duplicate or Delete " + them + ".");
}

void RootTubeTool::releaseSelect(StrokeHost& host) {
    std::optional<Move> m;
    m.swap(move_);
    if (!m || !m->moved) return;
    std::vector<GuideId> changed;
    for (const auto& [g, base] : m->base) {
        if (roots_[g] != base) changed.push_back(g);
    }
    try {
        commit(host, "move root tubes", [&](GuideHierarchy& h, const ScalpMesh& scalp) {
            for (const GuideId g : changed) editRootTube(h, scalp, g, roots_[g]);
        });
    } catch (const std::exception& e) {
        say(std::string("The roots weren't moved: ") + e.what());
        reloadRoots(host);
        return;
    }
    reloadRoots(host);
    say(std::to_string(changed.size()) + plural(changed.size(), " root", " roots") + " moved.");
}

bool RootTubeTool::beginDuplicate(StrokeHost& host) {
    sync(host);
    if ((mode_ != Mode::Select && mode_ != Mode::Duplicate) || picked_.empty()) {
        say("Select a root first (Select root, then click its middle).");
        return false;
    }
    mode_ = Mode::Duplicate;
    ghost_.reset();
    say(std::string("Move the cursor over the scalp and click to set the cop") + plural(picked_.size(), "y", "ies") + " down.");
    return true;
}

void RootTubeTool::endDuplicate() {
    if (mode_ == Mode::Duplicate) {
        mode_ = Mode::Select;
        ghost_.reset();
    }
}

void RootTubeTool::follow(StrokeHost& host, const ToolView& view, float x, float y) {
    if (picked_.empty()) return;
    const MeshPtr scalp = host.scalp();
    const std::optional<Hit> at = scalp ? hit(view, *scalp, x, y) : std::nullopt;
    if (!at) return;
    const Vec3f middle = groupMiddle(host);
    std::vector<std::vector<Vec3f>> copies;
    for (const GuideId g : picked_) copies.push_back(carryOutline(*scalp, roots_.at(g), middle, at->point));
    ghost_ = std::make_pair(at->point, std::move(copies));
}

void RootTubeTool::placeCopy(StrokeHost& host) {
    if (!ghost_) {
        say("Click on the scalp to set the copy down.");
        return;
    }
    const Vec3f to = ghost_->first;
    const Vec3f middle = groupMiddle(host);
    std::vector<GuideId> made;
    try {
        commit(host, "duplicate root tubes", [&](GuideHierarchy& h, const ScalpMesh& scalp) {
            for (const GuideId g : picked_) made.push_back(duplicateRootTube(h, scalp, g, to, &middle));
        });
    } catch (const std::exception& e) {
        say(std::string("The roots weren't duplicated: ") + e.what());
        return;
    }
    const std::vector<GuideId> sources = picked_;
    picked_ = made;
    endDuplicate();
    reloadRoots(host);
    say("Duplicated root" + plural(sources.size(), "", "s") + " " + ids(sources) + " as " + ids(made) +
        " (tubes and hierarchies with them); the copies are selected.");
}

int RootTubeTool::selectShown(StrokeHost& host) {
    if (mode_ != Mode::Select && mode_ != Mode::Duplicate) start(host, Mode::Select);
    endDuplicate();
    reloadRoots(host);  // (the roots shown now: the level in view)
    move_.reset();
    picked_.clear();
    for (const auto& [g, middle] : middles_) picked_.push_back(g);
    say(picked_.empty() ? std::string("No roots shown to select.")
                        : std::to_string(picked_.size()) + plural(picked_.size(), " root", " roots") + " selected (the level in view).");
    return static_cast<int>(picked_.size());
}

int RootTubeTool::deleteSelected(StrokeHost& host) {
    sync(host);
    if ((mode_ != Mode::Select && mode_ != Mode::Duplicate) || picked_.empty()) {
        say("Select a root first (Select root, then click its middle).");
        return 0;
    }
    const std::vector<GuideId> gone = picked_;
    endDuplicate();
    try {
        commit(host, "delete root tubes", [&](GuideHierarchy& h, const ScalpMesh& scalp) { deleteRootTubes(h, scalp, gone); });
    } catch (const std::exception& e) {
        say(std::string("The roots weren't deleted: ") + e.what());
        return 0;
    }
    picked_.clear();
    reloadRoots(host);
    say("Deleted root" + plural(gone.size(), "", "s") + " " + ids(gone) + ", with " + plural(gone.size(), "its tube and hierarchy", "their tubes and hierarchies") + ".");
    return static_cast<int>(gone.size());
}

// ---- The overlay ------------------------------------------------------------------------------------------------

void RootTubeTool::laid(const ScalpMesh& scalp, const std::vector<Vec3f>& cvs, bool closed, bool filled, Item& item) {
    std::vector<float> key;
    key.reserve(cvs.size() * 3 + 2);
    for (const Vec3f& p : cvs) key.insert(key.end(), {p.x, p.y, p.z});
    key.push_back(closed ? 1.0f : 0.0f);
    key.push_back(filled ? 1.0f : 0.0f);
    const auto it = laidCache_.find(key);
    if (it != laidCache_.end()) {
        item.edge = it->second.edge;
        item.triangles = it->second.triangles;
        return;
    }
    const std::size_t n = cvs.size();
    Vec3f middle;
    if (n >= 3) {
        middle = outlineCentroid(cvs);
    } else {
        for (const Vec3f& p : cvs) middle += p;
        middle = middle / static_cast<float>(std::max<std::size_t>(n, 1));
    }
    float reach = 1e-6f;
    for (const Vec3f& p : cvs) reach = std::max(reach, dist(middle, p));
    auto on = [&](const Vec3f& p) { return scalpPointNormal(scalp, p).point; };
    const bool loop = closed && n >= 3;
    std::vector<Vec3f> edge;
    for (std::size_t i = 0; i + (loop ? 0 : 1) < n; ++i) {
        const Vec3f a = cvs[i], b = cvs[(i + 1) % n];
        const int m = std::clamp(static_cast<int>(std::ceil(dist(a, b) / (0.06f * reach))), 1, 8);
        for (int j = 0; j < m; ++j) edge.push_back(on(a + (b - a) * (static_cast<float>(j) / static_cast<float>(m))));
    }
    if (!loop && n > 0) edge.push_back(on(cvs.back()));
    std::vector<Vec3f> tris;
    if (filled && loop) {
        // From the root's apex (rootOutlineCenter: the patch's convex side), its rings over a dome
        // about it (from a center `reach` under the apex), so a root drawn round much of a head
        // fills its top, not straight through the head onto the face.
        const ScalpPointNormal apex = rootOutlineCenter(scalp, cvs);
        const Vec3f c = apex.point;
        const Vec3f o = c - apex.normal.normalized() * reach;
        std::vector<std::vector<Vec3f>> rings{std::vector<Vec3f>(edge.size(), c)};
        for (const float t : {1.0f / 3.0f, 2.0f / 3.0f}) {
            std::vector<Vec3f> ring;
            for (const Vec3f& q : edge) {
                const Vec3f a = c - o, b = q - o;
                const Vec3f way = a * (1.0f - t) + b * t;
                const float len = a.length() * (1.0f - t) + b.length() * t;
                ring.push_back(on(way.length() > 1e-9f ? o + way.normalized() * len : c + (q - c) * t));
            }
            rings.push_back(std::move(ring));
        }
        rings.push_back(edge);
        const std::size_t m = edge.size();
        for (std::size_t r = 0; r + 1 < rings.size(); ++r) {
            const std::vector<Vec3f>& inner = rings[r];
            const std::vector<Vec3f>& outer = rings[r + 1];
            for (std::size_t i = 0; i < m; ++i) {
                const std::size_t j = (i + 1) % m;
                if (r > 0) tris.insert(tris.end(), {inner[i], outer[i], inner[j]});
                tris.insert(tris.end(), {inner[j], outer[i], outer[j]});
            }
        }
    }
    if (laidCache_.size() > 400) laidCache_.clear();
    Item cached;
    cached.edge = edge;
    cached.triangles = tris;
    laidCache_[key] = cached;
    item.edge = std::move(edge);
    item.triangles = std::move(tris);
}

std::vector<RootTubeTool::Item> RootTubeTool::overlay(StrokeHost& host) {
    std::vector<Item> out;
    {
        // (The levels shown changed: the roots of those shown, read again.)
        std::set<std::string> hidden = host.hiddenGroups();
        std::string mixed = host.mixedView();
        if (hidden != hidden_ || mixed != mixed_) {
            hidden_ = std::move(hidden);
            mixed_ = std::move(mixed);
            if (mode_ == Mode::Select || mode_ == Mode::Place) reloadRoots(host);
            if (mode_ == Mode::Edit) beginEdit(host);  // (Edit roots: the roots shown now)
        }
    }
    if (!started_) return out;
    sync(host);
    const MeshPtr scalp = host.scalp();
    if (!scalp) return out;
    auto outline = [&](const std::vector<Vec3f>& cvs, std::array<float, 3> color, unsigned style) {
        Item item;
        item.color = color;
        item.style = style;
        if (!cvs.empty()) laid(*scalp, cvs, style & Closed, style & Filled, item);
        if (style & Dots) item.dots = cvs;
        out.push_back(std::move(item));
    };
    auto marker = [&](const Vec3f& at, std::array<float, 3> color, unsigned style) {
        Item item;
        item.edge = {at};
        item.color = color;
        item.style = Marker | style;
        out.push_back(std::move(item));
    };
    if (mode_ == Mode::Edit) {
        for (const auto& [g, cvs] : neighbours_) {  // (editing the selected guides' roots: the others, unfilled)
            if (!edits_.count(g)) outline(cvs, colorOf(g), Closed);
        }
        for (const auto& [g, cvs] : edits_) outline(cvs, colorOf(g), Closed | Filled | Dots | (touched_.count(g) ? Highlight : 0u));
    } else if (mode_ == Mode::Select || mode_ == Mode::Duplicate) {
        for (const auto& [g, cvs] : roots_) {
            const bool picked = std::find(picked_.begin(), picked_.end(), g) != picked_.end();
            outline(cvs, colorOf(g), Closed | Filled | (picked ? Highlight | Dots : 0u));
            marker(middles_.at(g).point, colorOf(g), picked ? Highlight : 0u);
        }
        if (mode_ == Mode::Duplicate && ghost_) {
            for (std::size_t k = 0; k < picked_.size() && k < ghost_->second.size(); ++k) {
                outline(ghost_->second[k], colorOf(picked_[k]), Closed | Filled | Highlight | Dots);
            }
        }
    } else {
        for (const auto& [g, cvs] : neighbours_) outline(cvs, colorOf(g), Closed | Dots | Dim);  // (what a new CV snaps onto)
        if (!cvs_.empty()) {  // (the root being drawn: edged green, as a selected root)
            const bool closed = mode_ == Mode::Draw;
            outline(cvs_, closed ? kClosing : kOutline, Dots | Highlight | (closed ? Closed : 0u));
        }
        if (mode_ == Mode::Draw && center_) {
            marker(center_->point, kClosing, Highlight);
            if (stroke_.size() >= 2) {
                Item item;
                item.edge = stroke_;
                item.color = kStrokeColor;
                item.style = Stroke;
                out.push_back(std::move(item));
            }
        }
    }
    return out;
}

}  // namespace groomlab
