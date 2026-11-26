// SPDX-License-Identifier: Apache-2.0
#include "groomlab/editor/agent_target.h"

#include "groomlab/editor/agent_photos.h"

#include <algorithm>
#include <filesystem>

#include "groomlab/presets.h"

namespace groomlab::agent {

namespace fs = std::filesystem;

// ---- Files ------------------------------------------------------------------------------------------

bool AgentFolders::insideRoot(const std::string& path) const {
    std::error_code ec;
    fs::path p = fs::u8path(path);
    if (p.is_relative()) p = fs::u8path(root) / p;
    fs::path canonical = fs::weakly_canonical(p, ec);
    if (ec) canonical = p.lexically_normal();
    const fs::path rel = canonical.lexically_relative(fs::u8path(root));
    if (rel.empty() || rel.is_absolute()) return false;
    const std::string first = rel.begin()->u8string();
    return first != "..";
}

std::string AgentFolders::resolve(const std::string& path) const {
    if (path.empty()) throw ToolError("no path given");
    if (path.find('\0') != std::string::npos) throw ToolError("the path holds a NUL character");
    if (!insideRoot(path)) {
        throw ToolError("'" + path + "' is outside the folder this server may read and write (" + root +
                        "): use a path inside it, relative to it");
    }
    std::error_code ec;
    fs::path p = fs::u8path(path);
    if (p.is_relative()) p = fs::u8path(root) / p;
    fs::path canonical = fs::weakly_canonical(p, ec);
    if (ec) canonical = p.lexically_normal();
    return canonical.u8string();
}

std::string AgentFolders::resolveForWrite(const std::string& path) const {
    const std::string full = resolve(path);
    std::error_code ec;
    if (fs::is_directory(fs::u8path(full), ec)) throw ToolError("'" + path + "' is a folder: give a file name");
    fs::create_directories(fs::u8path(full).parent_path(), ec);
    return full;
}

std::string AgentFolders::shown(const std::string& path) const {
    if (!insideRoot(path)) return path;
    std::error_code ec;
    const fs::path rel = fs::weakly_canonical(fs::u8path(path), ec).lexically_relative(fs::u8path(root));
    return rel.empty() ? path : rel.generic_u8string();
}

std::vector<std::string> AgentFolders::presetDirs() const { return presetSearchDirs(presetUserDir(), presetDir); }

// ---- Strokes ----------------------------------------------------------------------------------------

AgentStrokeHost::AgentStrokeHost(AgentGroom& groom) : SessionStrokeHost(groom.session()), groom_(groom) {}

void AgentStrokeHost::stage(StrokeField field, std::string text) {
    if (field == StrokeField::Guides) {
        auto h = std::make_shared<GuideHierarchy>(GuideHierarchy::fromJson(text));
        GroomSession& s = groom_.session();
        if (s.groomScale() > 0.0) h->setScale(static_cast<float>(s.groomScale()));
        const MeshPtr scalp = s.scalp();
        h->evaluate(scalp.get());
        stagedHierarchy_ = std::move(h);
    }
    if (field == StrokeField::ScalpMaps) stagedMaps_ = ScalpMaps::fromJson(text);
    staged_.set(field, std::move(text));
}

void AgentStrokeHost::clearStage() {
    staged_ = StrokeEdit{};
    stagedHierarchy_.reset();
    stagedMaps_ = ScalpMaps{};
}

std::shared_ptr<const GuideHierarchy> AgentStrokeHost::authoredHierarchy() {
    if (stagedHierarchy_) return stagedHierarchy_;
    return SessionStrokeHost::authoredHierarchy();
}

std::string AgentStrokeHost::storedText(StrokeField field) {
    const StrokeEdit::Text& t = staged_.text(field);
    if (t.set) return t.after;
    return SessionStrokeHost::storedText(field);
}

const ScalpMaps& AgentStrokeHost::scalpMaps() {
    if (staged_.text(StrokeField::ScalpMaps).set) return stagedMaps_;
    return SessionStrokeHost::scalpMaps();
}

void AgentStrokeHost::commit(StrokeEdit edit) {
    // A text the stroke wrote as it found it (the guides written again from the hierarchy it read,
    // unchanged) is no change; the staged texts the stroke did not change go in the same step.
    for (std::size_t i = 0; i < kStrokeFields; ++i) {
        const StrokeField field = static_cast<StrokeField>(i);
        StrokeEdit::Text& t = edit.texts[i];
        const StrokeEdit::Text& s = staged_.texts[i];
        if (t.set) {
            std::string found = storedText(field);
            if (field == StrokeField::Guides) {
                const std::shared_ptr<const GuideHierarchy> h = authoredHierarchy();
                found = h ? h->toJson() : std::string();
            }
            if (t.after == found) t = StrokeEdit::Text{};
        }
        if (s.set && !t.set) edit.set(field, s.after);
    }
    bool any = false;
    for (std::size_t i = 0; i < kStrokeFields; ++i) {
        StrokeEdit::Text& t = edit.texts[i];
        if (!t.set) continue;
        t.before = SessionStrokeHost::storedText(static_cast<StrokeField>(i));
        t.hasBefore = true;
        if (t.before == t.after) {
            t = StrokeEdit::Text{};
            continue;
        }
        any = true;
    }
    if (!any && edit.textureMap.empty()) return;
    GroomEdit step;
    step.label = label;
    step.stroke = std::make_shared<const StrokeEdit>(std::move(edit));
    groom_.commit(std::move(step));
    committed_ = true;
}

// ---- Grooms ------------------------------------------------------------------------------------------

AgentGroom::AgentGroom(std::string name) : name_(std::move(name)) {}
AgentGroom::~AgentGroom() = default;

std::string AgentGroom::apply(const GroomEdit& edit, bool after) {
    GroomSession& s = session();
    if (edit.referencePhotos) referencePhotos = after ? edit.referencePhotos->second : edit.referencePhotos->first;
    if (edit.scalp) s.setScalp(after ? edit.scalp->second : edit.scalp->first);
    if (edit.whole) {
        try {
            s.setGraphJson((after ? edit.graphAfter : edit.graphBefore).dump());
        } catch (const std::exception& e) {
            return e.what();
        }
    }
    if (!edit.stroke) return {};
    // The graph first: when it would not load, nothing else changes either.
    StrokeEdit rest = *edit.stroke;
    StrokeEdit::Text& graph = rest.text(StrokeField::Graph);
    if (graph.set && (after || graph.hasBefore)) {
        try {
            s.setGraphJson(after ? graph.after : graph.before);
        } catch (const std::exception& e) {
            return e.what();
        }
    }
    graph = StrokeEdit::Text{};
    rest.apply(s, after);
    return {};
}

void AgentGroom::commitPhotos(const std::string& label, const std::string& after) {
    if (after == referencePhotos) return;
    GroomEdit step;
    step.side = true;
    step.label = label;
    step.referencePhotos = std::make_pair(referencePhotos, after);
    commit(std::move(step));
}

void AgentGroom::commit(GroomEdit edit) {
    const std::string error = apply(edit, true);
    if (!error.empty()) throw ToolError("the graph would not load after this edit, so nothing changed: " + error);
    history_.record(std::move(edit));
    history_.limit(kMaxUndo);
}

std::string AgentGroom::undo() { return undoStep(history_, *this); }
std::string AgentGroom::redo() { return redoStep(history_, *this); }

AgentGroom::MorphOutcome AgentGroom::morphToPhotos(const MorphRequest& request) {
    GroomSession& s = session();
    const std::shared_ptr<const ScalpMesh> now = s.rawScalp();
    if (!now || now->triangleCount() == 0) throw ToolError("the groom has no scalp");
    MorphOutcome out;
    if (request.reset) {
        if (!morphRest_) throw ToolError("the scalp is not morphed (morph_to_photos first)");
        if (morphRest_ == now) {
            out.note = "the scalp is as it was before the first morph already";
            return out;
        }
        GroomEdit step;
        step.side = true;
        step.label = "reset photo morph";
        step.scalp = std::make_pair(now, morphRest_);
        commit(std::move(step));
        return out;
    }
    // The scalp before the first morph: kept from now on (the pairs' spots are found on it).
    if (!morphRest_ || morphRest_->triangleCount() != now->triangleCount()) {
        morphRest_ = now;
        morphRestRefined_ = s.scalp();
    }
    const std::shared_ptr<const ScalpMesh> current = s.scalp();
    const PhotoMorph m = agent::morphToPhotos(*morphRest_, morphRestRefined_ ? *morphRestRefined_ : *morphRest_, current ? *current : *now,
                                              photosFromJson(referencePhotos), request.stiffness, request.symmetric);
    if (!m.ok) throw ToolError(m.error);
    auto morphed = std::make_shared<ScalpMesh>(morphRest_->withPoints(m.points));
    GroomEdit step;
    step.side = true;
    step.label = "morph to photos";
    step.scalp = std::make_pair(now, std::shared_ptr<const ScalpMesh>(morphed));
    commit(std::move(step));
    out.pins = m.pins;
    out.pinsRmsBefore = m.rmsBefore;
    out.pinsRmsAfter = m.rmsAfter;
    return out;
}

SimRun AgentGroom::simulateTo(int toFrame) { return session().simulateTo(toFrame); }

void AgentGroom::setCollider(const std::string& name, MeshPtr collider) { session().setMesh(name, std::move(collider)); }

CardBake AgentGroom::bakeCards(int resolution, float occlusion) {
    ViewRequest request;
    request.density = 1.0f;
    request.time = frame / std::max(framesPerSecond(), 1e-9);
    request.wantCards = true;
    request.cardDensity = 1.0f;
    request.gameExport = true;
    const ViewResult& view = this->view(request);
    if (!view.errors.empty()) throw ToolError(view.errors.front());
    if (!view.cardStrands || !view.cards) throw ToolError("the groom has no cards: add a Cards node (list_nodes \"cards\")");
    return bakeCardsInMemory(view.cardStrands, view.scalp ? view.scalp : session().scalp(), resolution, occlusion);
}

AgentStrokeHost& AgentGroom::strokeHost() {
    if (!strokes_) strokes_ = std::make_unique<AgentStrokeHost>(*this);
    return *strokes_;
}

// ---- The target ------------------------------------------------------------------------------------

AgentGroom* AgentTarget::find(const std::string& name) {
    for (AgentGroom* g : grooms()) {
        if (g->name() == name) return g;
    }
    return nullptr;
}

AgentGroom& AgentTarget::groom(const std::string& name) {
    const std::vector<AgentGroom*> open = grooms();
    std::string names;
    for (const AgentGroom* g : open) names += (names.empty() ? "" : ", ") + g->name();
    if (name.empty()) {
        if (open.size() == 1) return *open.front();
        if (open.empty()) throw ToolError("no groom is open: make one with new_groom or open one with open_groom");
        throw ToolError("several grooms are open (" + names + "): say which with \"groom\"");
    }
    if (AgentGroom* g = find(name)) return *g;
    throw ToolError("no groom named '" + name + "'" + (names.empty() ? std::string(" (none is open)") : " (open: " + names + ")"));
}

void AgentTarget::setAnimation(AgentGroom&, const AnimationInputs&) { throw ToolError("this host reads no animation caches"); }

void AgentTarget::writeMeshCache(const std::string&, const std::string&, const std::vector<std::shared_ptr<const ScalpMesh>>&, double, double) {
    throw ToolError("this host writes no mesh caches");
}

std::size_t AgentTarget::bakeSimulation(AgentGroom&, const std::string&, const std::string&, int, int) {
    throw ToolError("this host writes no simulation caches");
}

void AgentTarget::loadSimulationCache(AgentGroom&, const std::string&, const std::string&, const std::string&) {
    throw ToolError("this host reads no simulation caches");
}

std::optional<PreviewImage> AgentTarget::viewportImage(AgentGroom&, int) { return std::nullopt; }

RenderedFrame AgentTarget::render(AgentGroom&, const RenderRequest& request) {
    throw ToolError("this host does not render '" + request.renderer + "' itself");
}

PreviewImage AgentTarget::preview(AgentGroom&, const ScalpMesh* scalp, const StrandSet* strands, const PreviewOptions& options) {
    return renderPreview(scalp, strands, options);
}

}  // namespace groomlab::agent
