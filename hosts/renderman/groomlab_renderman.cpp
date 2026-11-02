// SPDX-License-Identifier: Apache-2.0
// RenderMan procedural: renders a groom file at render time in prman, without Maya, the
// counterpart of the Arnold procedural (hosts/arnold). It evaluates the groom at the frame (and at
// each motion key across the shutter) and emits one RiCurves: linear, nonperiodic, a width per
// point, and the look as primvars (renderman_look.h curvePrimvars) for a PxrHairColor and
// PxrMarschnerHair network the scene provides (examples/groom.rib).
//
// A Procedural2 "DynamicLoad" DSO: prman calls Subdivide2 with the RIB's typed parameters, and
// Bound for a "DSOBound" bounder (the groom evaluated once: the bound and the curves both come
// from it).
//
//   Procedural2 "DynamicLoad" "DSOBound"
//       "string dsoname" ["groomlab_renderman"]
//       "string filename" ["C:/shots/010/hair.groom"]
//       "float frame" [12]
//       "float motionStart" [0] "float motionEnd" [0.5] "int motionKeys" [2]
//
// Parameters (all optional but filename):
//   filename      the .groom file (its scalp cache is found next to it)
//   frame         the frame to render; times are frame / fps
//   fps           frames per second; 0 uses the groom file's
//   density       fraction of strands to keep, by stable strand ID (0-1)
//   widthScale    multiplies strand widths
//   lengthScale   scales each strand about its root (render_overrides.h)
//   motionStart, motionEnd
//                 the shutter, in frames relative to `frame` (as the Arnold procedural's); the
//                 MotionBegin times are these, so they must be in the units of the scene's
//                 Shutter (frames: Shutter 0 0.5 for a half-frame shutter opening at the frame)
//   motionKeys    keys across [motionStart, motionEnd]; deformation blur when 2 or more and the
//                 shutter is open
//   searchPaths   folders, separated by ';', where relative paths in the groom (texture maps)
//                 resolve after the groom file's folder and its own search paths
//   name          the curves' identifier name
//   debug         1: print the strand and point counts and every primvar with its count
//   revision      ignored: a host changes it to say the groom changed (RenderMan for Maya's IPR
//                 renders a procedural again when its data string changes, and the groom file's
//                 name stays the same)
//
// RenderMan's Procedural2 hands a DSO each string as one value, so searchPaths is one string.
//
// The older Procedural "DynamicLoad" API too (ConvertParameters, Subdivide, Free): the one
// RenderMan for Maya's generic RenderManProcedural node drives, its "data" string the parameters as
// `-key value` pairs, the bound the RIB's (or the host's):
//
//   Procedural "DynamicLoad" ["groomlab_renderman" "-file 'C:/shots/010/hair.groom' -frame 12"]
//       [-100 100 -100 100 -100 100]
//
// Keys (the names above; -file for filename): -file, -frame, -fps, -density, -widthScale,
// -lengthScale, -motionStart, -motionEnd, -motionKeys, -searchPaths, -name, -debug (alone: 1),
// -revision. A
// value with spaces goes in double or single quotes (no escapes inside: Windows paths keep their
// backslashes). An unknown key warns and is skipped with its value. A Procedural2 call's "data"
// string parameter is read the same way, before its typed parameters (RenderMan for Maya's node
// also has a "frame" parameter, which a Procedural2 call would pass).

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include <RiEntrypoints.h>
#include <RixInterfaces.h>
#include <ri.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

#include "groomlab/alembic_operators.h"
#include "groomlab/export_view.h"
#include "groomlab/frame_time.h"
#include "groomlab/graph.h"
#include "groomlab/groom_file.h"
#include "groomlab/groom_scene.h"
#include "groomlab/groom_session.h"
#include "groomlab/paths.h"
#include "groomlab/render_overrides.h"
#include "groomlab/renderman_look.h"

namespace {

RixMessages* messages() {
    static RixMessages* msgs = [] {
        RixContext* context = RixGetContext();
        return context ? static_cast<RixMessages*>(context->GetRixInterface(k_RixMessages)) : nullptr;
    }();
    return msgs;
}

template <typename... Args>
void info(bool always, const char* format, Args... args) {
    if (RixMessages* m = messages()) {
        if (always) m->InfoAlways(format, args...);
        else m->Info(format, args...);
    }
}
template <typename... Args>
void warning(const char* format, Args... args) {
    if (RixMessages* m = messages()) m->Warning(format, args...);
}
template <typename... Args>
void error(const char* format, Args... args) {
    if (RixMessages* m = messages()) m->Error(format, args...);
}

#if defined(_WIN32) && defined(GROOMLAB_TBB_DLL)
// TBB is delay-loaded (see CMakeLists.txt): load it from this DSO's folder unless the host already
// has it, before anything calls into it.
void loadRuntimeDlls() {
    if (GetModuleHandleW(GROOMLAB_TBB_DLL)) return;
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&loadRuntimeDlls), &self);
    wchar_t path[MAX_PATH];
    const DWORD n = GetModuleFileNameW(self, path, MAX_PATH);
    std::wstring dir(path, n);
    dir.resize(dir.find_last_of(L"\\/") + 1);
    if (!LoadLibraryExW((dir + GROOMLAB_TBB_DLL).c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH)) {
        warning("[groomlab] cannot load %ls beside the procedural; put it on PATH", GROOMLAB_TBB_DLL);
    }
}
#else
void loadRuntimeDlls() {}
#endif

void initializeOnce() {
    static std::once_flag once;
    std::call_once(once, [] {
        loadRuntimeDlls();
        groomlab::registerAlembicOperators(groomlab::OperatorRegistry::instance());
    });
}

// The procedural's parameters, read from the RIB's typed list.
struct Params {
    std::string filename, searchPaths, name;
    float frame = 1.0f, fps = 0.0f, density = 1.0f, widthScale = 1.0f, lengthScale = 1.0f;
    float motionStart = 0.0f, motionEnd = 0.0f;
    int motionKeys = 2;
    bool debug = false;
    // What identifies the groom made: Bound's result is reused by Subdivide2 for the same key.
    std::string key() const {
        std::ostringstream s;
        s.precision(9);
        s << filename << '|' << searchPaths << '|' << name << '|' << frame << '|' << fps << '|' << density << '|'
          << widthScale << '|' << lengthScale << '|' << motionStart << '|' << motionEnd << '|' << motionKeys;
        return s.str();
    }
};

// A token's name: "float frame" and "frame" alike.
std::string tokenName(const char* token) {
    std::string t = token ? token : "";
    const std::size_t space = t.find_last_of(" \t");
    return space == std::string::npos ? t : t.substr(space + 1);
}

// The old API's data string in words: whitespace separates them, double or single quotes hold a
// word with spaces (the quotes go; nothing is escaped inside them).
std::vector<std::string> splitWords(const std::string& text) {
    std::vector<std::string> words;
    std::size_t i = 0;
    while (i < text.size()) {
        while (i < text.size() && std::isspace(static_cast<unsigned char>(text[i]))) ++i;
        if (i >= text.size()) break;
        std::string word;
        while (i < text.size() && !std::isspace(static_cast<unsigned char>(text[i]))) {
            const char c = text[i];
            if (c == '"' || c == '\'') {
                const std::size_t end = text.find(c, i + 1);
                if (end == std::string::npos) {
                    warning("[groomlab] unmatched %c in the procedural's data: read to the end", c);
                    word += text.substr(i + 1);
                    i = text.size();
                } else {
                    word += text.substr(i + 1, end - i - 1);
                    i = end + 1;
                }
            } else {
                word += c;
                ++i;
            }
        }
        words.push_back(word);
    }
    return words;
}

bool parseFloat(const std::string& s, float& out) {
    char* end = nullptr;
    const float v = std::strtof(s.c_str(), &end);
    if (s.empty() || !end || *end != '\0') return false;
    out = v;
    return true;
}

bool parseInt(const std::string& s, int& out) {
    float v = 0.0f;
    if (!parseFloat(s, v) || v != std::floor(v)) return false;
    out = static_cast<int>(v);
    return true;
}

// The old API's data string (`-key value` pairs, the header's) into `p`, over what it holds.
void parseData(const std::string& text, Params& p) {
    const std::vector<std::string> words = splitWords(text);
    const std::map<std::string, float Params::*> floats = {
        {"frame", &Params::frame}, {"fps", &Params::fps}, {"density", &Params::density},
        {"widthscale", &Params::widthScale}, {"lengthscale", &Params::lengthScale},
        {"motionstart", &Params::motionStart}, {"motionend", &Params::motionEnd}};
    const std::map<std::string, std::string Params::*> strings = {
        {"file", &Params::filename}, {"filename", &Params::filename}, {"searchpaths", &Params::searchPaths},
        {"name", &Params::name}};
    for (std::size_t i = 0; i < words.size(); ++i) {
        const std::string& word = words[i];
        if (word.size() < 2 || word[0] != '-') {
            warning("[groomlab] procedural data: '%s' is not a -key (ignored)", word.c_str());
            continue;
        }
        std::string key = word.substr(1);
        std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        const bool hasValue = i + 1 < words.size();
        const std::string value = hasValue ? words[i + 1] : std::string();
        if (key == "revision") {  // the host's change counter: nothing to read
            if (hasValue) ++i;
            continue;
        }
        if (key == "debug") {  // a flag, or -debug 0/1
            int on = 1;
            if (hasValue && parseInt(value, on)) ++i;
            p.debug = on != 0;
            continue;
        }
        const auto f = floats.find(key);
        const auto s = strings.find(key);
        if (f == floats.end() && s == strings.end() && key != "motionkeys") {
            // Skip its value too, unless the next word is a key.
            const bool nextIsKey = hasValue && value.size() > 1 && value[0] == '-' && std::isalpha(static_cast<unsigned char>(value[1]));
            warning("[groomlab] procedural data: unknown key %s (ignored)", word.c_str());
            if (hasValue && !nextIsKey) ++i;
            continue;
        }
        if (!hasValue) {
            warning("[groomlab] procedural data: %s has no value (ignored)", word.c_str());
            continue;
        }
        ++i;
        if (s != strings.end()) {
            p.*(s->second) = value;
        } else if (key == "motionkeys") {
            if (!parseInt(value, p.motionKeys)) warning("[groomlab] procedural data: %s %s is not a whole number (ignored)", word.c_str(), value.c_str());
        } else if (!parseFloat(value, p.*(f->second))) {
            warning("[groomlab] procedural data: %s %s is not a number (ignored)", word.c_str(), value.c_str());
        }
    }
}

Params readParams(int n, RtToken const toks[], RtPointer const vals[]) {
    Params p;
    // A "data" string first (the old API's arguments: RenderMan for Maya's node), the typed ones over it.
    for (int i = 0; i < n; ++i) {
        if (vals[i] && tokenName(toks[i]) == "data") {
            const RtString* s = static_cast<const RtString*>(vals[i]);
            if (s[0]) parseData(s[0], p);
        }
    }
    for (int i = 0; i < n; ++i) {
        if (!vals[i]) continue;
        const std::string name = tokenName(toks[i]);
        auto str = [&] {
            const RtString* s = static_cast<const RtString*>(vals[i]);
            return std::string(s[0] ? s[0] : "");
        };
        auto flt = [&] { return *static_cast<const float*>(vals[i]); };
        auto num = [&] { return *static_cast<const int*>(vals[i]); };
        if (name == "filename") p.filename = str();
        else if (name == "searchPaths") p.searchPaths = str();
        else if (name == "name") p.name = str();
        else if (name == "frame") p.frame = flt();
        else if (name == "fps") p.fps = flt();
        else if (name == "density") p.density = flt();
        else if (name == "widthScale") p.widthScale = flt();
        else if (name == "lengthScale") p.lengthScale = flt();
        else if (name == "motionStart") p.motionStart = flt();
        else if (name == "motionEnd") p.motionEnd = flt();
        else if (name == "motionKeys") p.motionKeys = num();
        else if (name == "debug") p.debug = num() != 0;
    }
    return p;
}

// The curves as they go to RiCurvesV: counts, one points array per motion key, widths and the
// primvars, the groom scene gone.
struct Curves {
    std::vector<RtInt> counts;
    std::vector<std::vector<float>> points;  // xyz per point, per key
    std::vector<float> times;                // the keys' MotionBegin times
    std::vector<float> widths;
    std::vector<groomlab::renderman::Primvar> primvars;
    std::size_t pointCount = 0;
    float bound[6] = {0, 0, 0, 0, 0, 0};
};

std::shared_ptr<Curves> makeCurves(const Params& p) {
    const auto t0 = std::chrono::steady_clock::now();
    auto out = std::make_shared<Curves>();
    const std::string filename = groomlab::expandEnvironment(p.filename);
    if (filename.empty()) throw std::runtime_error("no filename");
    groomlab::GroomFile file = groomlab::readGroomFile(filename);
    // The scene's search paths after the groom file's own.
    std::stringstream paths(p.searchPaths);
    for (std::string dir; std::getline(paths, dir, ';');) {
        if (!dir.empty()) file.searchPaths.push_back(dir);
    }
    auto scene = std::make_unique<groomlab::GroomScene>(std::move(file));
    // The param is a float: the file's own rate when it is that rate (frame_time.h: an NTSC rate exact).
    const double fps = p.fps > 0.0f && p.fps != static_cast<float>(scene->framesPerSecond()) ? p.fps : scene->framesPerSecond();
    const float density = std::clamp(std::isnan(p.density) ? 1.0f : p.density, 0.0f, 1.0f);
    groomlab::RenderOverrides lengths;
    lengths.lengthScale = p.lengthScale;
    auto evaluateAt = [&](double seconds) { return groomlab::applyRenderOverrides(scene->evaluate(seconds, density), lengths); };
    const unsigned keyCount = p.motionEnd > p.motionStart && p.motionKeys >= 2 ? static_cast<unsigned>(std::min(p.motionKeys, 64)) : 1u;
    scene->setCacheIntermediates(keyCount > 1);  // motion keys re-evaluate only what moves
    auto keyOffset = [&](unsigned k) {
        return keyCount == 1 ? 0.0 : p.motionStart + (p.motionEnd - p.motionStart) * static_cast<double>(k) / (keyCount - 1);
    };

    const groomlab::MeshPtr scalp = scene->rootScalpAt(groomlab::frameToSeconds(p.frame, fps));
    groomlab::StrandsPtr first = evaluateAt(groomlab::frameToSeconds(p.frame + keyOffset(0), fps));
    if (!first) first = std::make_shared<groomlab::StrandSet>();
    if (first->instances() && !first->instances()->instances.empty()) {
        warning("[groomlab] %s: instances and scales are not rendered by the RenderMan procedural yet", filename.c_str());
    }
    if (first->instances() && first->instances()->hidesStrands) first = std::make_shared<groomlab::StrandSet>();
    const groomlab::ExportView view = groomlab::describeExport(*first, scalp.get(), groomlab::kExportPointColors);
    const groomlab::HairLook look = scene->session().hairLook();

    out->pointCount = first->pointCount();
    out->counts.assign(view.counts.begin(), view.counts.end());
    out->widths.resize(out->pointCount);
    for (std::size_t i = 0; i < out->pointCount; ++i) out->widths[i] = view.width(i) * p.widthScale;
    auto copyPoints = [&](const groomlab::StrandSet& s) {
        const auto& pos = s.positions();
        std::vector<float> xyz(3 * pos.size());
        for (std::size_t i = 0; i < pos.size(); ++i) {
            xyz[3 * i] = pos[i].x;
            xyz[3 * i + 1] = pos[i].y;
            xyz[3 * i + 2] = pos[i].z;
        }
        return xyz;
    };
    out->points.push_back(copyPoints(*first));
    out->times.push_back(static_cast<float>(keyOffset(0)));
    // A groom that nothing moves: one key is every key (RenderMan needs no motion block for it).
    const bool still = !scene->readsTime();
    for (unsigned k = 1; k < keyCount && !still && !first->empty(); ++k) {
        const groomlab::StrandsPtr strands = evaluateAt(groomlab::frameToSeconds(p.frame + keyOffset(k), fps));
        if (!strands || strands->ids() != first->ids() || strands->offsets() != first->offsets()) {
            warning("[groomlab] %s: strands change between motion keys (give the groom a rest mesh); rendering "
                    "without deformation blur", filename.c_str());
            out->points.resize(1);
            out->times.resize(1);
            break;
        }
        out->points.push_back(copyPoints(*strands));
        out->times.push_back(static_cast<float>(keyOffset(k)));
    }
    out->primvars = groomlab::renderman::curvePrimvars(view, look);

    // The bound: every key's points, padded by the widest radius.
    float lo[3] = {std::numeric_limits<float>::max(), std::numeric_limits<float>::max(), std::numeric_limits<float>::max()};
    float hi[3] = {-lo[0], -lo[1], -lo[2]};
    for (const auto& key : out->points) {
        for (std::size_t i = 0; i + 2 < key.size(); i += 3) {
            for (int a = 0; a < 3; ++a) {
                lo[a] = std::min(lo[a], key[i + a]);
                hi[a] = std::max(hi[a], key[i + a]);
            }
        }
    }
    const float pad = out->widths.empty() ? 0.0f : 0.5f * *std::max_element(out->widths.begin(), out->widths.end());
    if (out->pointCount > 0) {
        for (int a = 0; a < 3; ++a) {
            out->bound[2 * a] = lo[a] - pad;
            out->bound[2 * a + 1] = hi[a] + pad;
        }
    }
    const std::size_t strands = out->counts.size();
    scene.reset();
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    info(p.debug, "[groomlab] %s: %zu strands, %zu points, %zu motion key(s) at frame %g (%.0f ms)", filename.c_str(),
         strands, out->pointCount, out->points.size(), static_cast<double>(p.frame), ms);
    if (p.debug) {
        for (const auto& pv : out->primvars) {
            info(true, "[groomlab] primvar %s: %zu values", pv.declaration().c_str(), pv.valueCount());
        }
    }
    return out;
}

// Bound's results, taken by the Subdivide2 of the same parameters (prman calls Bound first).
std::mutex cacheMutex;
std::map<std::string, std::shared_ptr<Curves>> cache;

std::shared_ptr<Curves> curvesFor(const Params& p, bool keep) {
    const std::string key = p.key();
    {
        std::lock_guard<std::mutex> lock(cacheMutex);
        const auto it = cache.find(key);
        if (it != cache.end()) {
            std::shared_ptr<Curves> found = it->second;
            if (!keep) cache.erase(it);
            return found;
        }
    }
    std::shared_ptr<Curves> made = makeCurves(p);
    if (keep) {
        std::lock_guard<std::mutex> lock(cacheMutex);
        cache[key] = made;
    }
    return made;
}

// A token that outlives the call: prman may hold on to a parameter list's tokens after RiCurvesV
// returns (it crashed now and then, at the end of the render, with tokens freed after the call),
// so each declaration is kept for the process's life (a few dozen distinct ones).
RtToken persistentToken(const std::string& declaration) {
    static std::mutex mutex;
    static std::set<std::string>* tokens = new std::set<std::string>;  // never freed
    std::lock_guard<std::mutex> lock(mutex);
    return tokens->insert(declaration).first->c_str();
}

void emitCurves(const Params& p, const Curves& c) {
    if (c.counts.empty() || c.pointCount == 0) return;
    // The primvars' tokens and values (the same in every motion key; P each key's own).
    std::vector<std::string> declarations;
    std::vector<RtPointer> values;
    for (const auto& pv : c.primvars) {
        declarations.push_back(pv.declaration());
        values.push_back(pv.type == groomlab::renderman::Type::Int ? static_cast<RtPointer>(const_cast<std::int32_t*>(pv.ints.data()))
                                                                    : static_cast<RtPointer>(const_cast<float*>(pv.floats.data())));
    }
    declarations.push_back("vertex float width");
    values.push_back(const_cast<float*>(c.widths.data()));
    declarations.push_back("vertex point P");
    values.push_back(nullptr);  // each key's
    std::vector<RtToken> tokens;
    for (const auto& d : declarations) tokens.push_back(persistentToken(d));

    RiAttributeBegin();
    if (!p.name.empty()) {
        RtString name = const_cast<char*>(persistentToken(p.name));
        RtToken token = "string name";
        RtPointer value = &name;
        RiAttributeV(RI_IDENTIFIER, 1, &token, &value);
    }
    const bool moving = c.points.size() > 1;
    if (moving) RiMotionBeginV(static_cast<int>(c.times.size()), const_cast<float*>(c.times.data()));
    std::vector<RtInt> counts = c.counts;
    for (const auto& key : c.points) {
        values.back() = const_cast<float*>(key.data());
        RiCurvesV(RI_LINEAR, static_cast<int>(counts.size()), counts.data(), RI_NONPERIODIC, static_cast<int>(tokens.size()),
                  tokens.data(), values.data());
    }
    if (moving) RiMotionEnd();
    RiAttributeEnd();
}

}  // namespace

extern "C" {

// Procedural2 "DSOBound": the groom's bound, from its evaluation (kept for Subdivide2).
PRMANEXPORT void Bound(RtInt n, RtToken const toks[], RtPointer const vals[], RtBound result[2]) {
    initializeOnce();
    const Params p = readParams(n, toks, vals);
    float bound[6] = {0, 0, 0, 0, 0, 0};
    try {
        const std::shared_ptr<Curves> c = curvesFor(p, true);
        std::memcpy(bound, c->bound, sizeof(bound));
    } catch (const std::exception& e) {
        error("[groomlab] %s: %s", p.filename.c_str(), e.what());
    }
    std::memcpy(result[0], bound, sizeof(bound));  // (every motion key's points inside)
}

// Procedural2 "DynamicLoad": the groom's curves.
PRMANEXPORT void Subdivide2(RtContextHandle, RtFloat, RtInt n, RtToken const toks[], RtPointer const vals[]) {
    initializeOnce();
    const Params p = readParams(n, toks, vals);
    try {
        const std::shared_ptr<Curves> c = curvesFor(p, false);
        emitCurves(p, *c);
    } catch (const std::exception& e) {
        error("[groomlab] %s: %s", p.filename.c_str(), e.what());
    }
}

// Procedural "DynamicLoad" (the older API, RenderMan for Maya's RenderManProcedural node): the data
// string's parameters, held for Subdivide and freed by Free.
PRMANEXPORT RtPointer ConvertParameters(RtString paramstr) {
    initializeOnce();
    auto* p = new Params;
    parseData(paramstr ? paramstr : "", *p);
    return p;
}

// The groom's curves (Subdivide2's), from ConvertParameters' parameters.
PRMANEXPORT void Subdivide(RtPointer data, RtFloat) {
    if (!data) return;
    initializeOnce();
    const Params& p = *static_cast<const Params*>(data);
    try {
        const std::shared_ptr<Curves> c = curvesFor(p, false);
        emitCurves(p, *c);
    } catch (const std::exception& e) {
        error("[groomlab] %s: %s", p.filename.c_str(), e.what());
    }
}

PRMANEXPORT void Free(RtPointer data) {
    delete static_cast<Params*>(data);
}

}  // extern "C"
