// SPDX-License-Identifier: Apache-2.0
#include "groomlab/paths.h"

#include <cctype>
#include <cstdlib>
#include <filesystem>

namespace groomlab {

namespace {

namespace fs = std::filesystem;

thread_local const PathResolver* tCurrent = nullptr;

bool isNameChar(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

bool lookup(const std::string& name, std::string* value) {
    const std::optional<std::string> v = environmentVariable(name);
    if (!v) return false;
    *value = *v;
    return true;
}

}  // namespace

std::optional<std::string> environmentVariable(const std::string& name) {
#ifdef _MSC_VER
    char* buffer = nullptr;
    std::size_t size = 0;
    if (_dupenv_s(&buffer, &size, name.c_str()) != 0 || !buffer) return std::nullopt;
    std::string value = buffer;
    std::free(buffer);
    return value;
#else
    const char* v = std::getenv(name.c_str());
    if (!v) return std::nullopt;
    return std::string(v);
#endif
}

std::string expandEnvironment(const std::string& path) {
    std::string out;
    out.reserve(path.size());
    for (std::size_t i = 0; i < path.size();) {
        const char c = path[i];
        std::string name;
        std::size_t end = i;
        if (c == '$' && i + 1 < path.size() && path[i + 1] == '{') {
            const std::size_t close = path.find('}', i + 2);
            if (close != std::string::npos) {
                name = path.substr(i + 2, close - i - 2);
                end = close + 1;
            }
        } else if (c == '$') {
            std::size_t j = i + 1;
            while (j < path.size() && isNameChar(path[j])) ++j;
            name = path.substr(i + 1, j - i - 1);
            end = j;
#ifdef _WIN32
        } else if (c == '%') {
            const std::size_t close = path.find('%', i + 1);
            if (close != std::string::npos && close > i + 1) {
                name = path.substr(i + 1, close - i - 1);
                end = close + 1;
            }
#endif
        }
        std::string value;
        if (!name.empty() && lookup(name, &value)) {
            out += value;
            i = end;
        } else {
            out += c;
            ++i;
        }
    }
    return out;
}

bool isAbsolutePath(const std::string& path) {
    if (path.empty()) return false;
    if (path[0] == '/' || path[0] == '\\') return true;  // POSIX root, UNC share, drive-relative root
    return fs::u8path(path).is_absolute();
}

namespace {

bool existsOnDisk(const std::string& path) {
    std::error_code ec;
    return !path.empty() && fs::exists(fs::u8path(path), ec);
}

// Whether `path`'s folder is on this machine: a file missing from a folder that is here is
// missing (or about to be written); a folder that is not here is another machine's.
bool folderHere(const std::string& path) {
    std::error_code ec;
    const fs::path parent = fs::u8path(path).lexically_normal().parent_path();
    return parent.empty() || fs::is_directory(parent, ec);
}

// A file a scene or groom stores, missing where it says, found where it came along to (a scene
// shared with its files): through `resolver` (the scene's or groom's folders), the stored path
// in a folder of its own first name (a zip of groomlab/ extracted into a groomlab/ folder:
// groomlab/groomlab/...); an absolute path from another machine by its tails, longest first, and
// those doubled the same way (D:/show/scenes/groomlab/masks/a.png: groomlab/masks/a.png beside the
// scene). A tail keeps a folder and the file (a bare name could be any file of that name). ""
// when none is there.
std::string relocated(const std::string& path, bool absolute, const PathResolver& resolver) {
    std::vector<fs::path> parts;
    for (const fs::path& part : fs::u8path(path).lexically_normal().relative_path()) {
        if (!part.empty() && part != ".") parts.push_back(part);
    }
    const std::size_t last = absolute ? (parts.size() >= 2 ? parts.size() - 2 : 0) : 0;
    for (std::size_t i = 0; i < parts.size() && i <= last; ++i) {
        if (parts[i] == "..") continue;
        fs::path tail;
        for (std::size_t j = i; j < parts.size(); ++j) tail /= parts[j];
        if (absolute) {
            const std::string found = resolver(tail.generic_u8string());
            if (existsOnDisk(found)) return found;
        }
        const std::string doubled = resolver((parts[i] / tail).generic_u8string());
        if (existsOnDisk(doubled)) return doubled;
        if (!absolute) break;
    }
    return {};
}

}  // namespace

std::string resolvePath(const std::string& path) {
    const std::string expanded = expandEnvironment(path);
    if (expanded.empty() || !tCurrent || !*tCurrent) return expanded;
    if (isAbsolutePath(expanded)) {
        if (existsOnDisk(expanded) || folderHere(expanded)) return expanded;
        const std::string found = relocated(expanded, true, *tCurrent);
        return found.empty() ? expanded : found;
    }
    const std::string resolved = (*tCurrent)(expanded);
    if (resolved.empty()) return expanded;
    if (existsOnDisk(resolved) || folderHere(resolved)) return resolved;
    const std::string found = relocated(expanded, false, *tCurrent);
    return found.empty() ? resolved : found;
}

PathResolver searchPathResolver(std::vector<std::string> directories) {
    return [dirs = std::move(directories)](const std::string& relative) -> std::string {
        std::string first;
        for (const std::string& dir : dirs) {
            if (dir.empty()) continue;
            const fs::path candidate = (fs::u8path(expandEnvironment(dir)) / fs::u8path(relative)).lexically_normal();
            std::error_code ec;
            if (fs::exists(candidate, ec)) return candidate.u8string();
            if (first.empty()) first = candidate.u8string();
        }
        return first;
    };
}

std::string resolvePathIn(const std::string& path, std::vector<std::string> directories) {
    const PathResolver resolver = searchPathResolver(std::move(directories));
    const ScopedPathResolver scope(&resolver);
    return resolvePath(path);
}

namespace {

std::string forward(std::string path) {
    for (char& c : path) {
        if (c == '\\') c = '/';
    }
    return path;
}

// The path as the file system names it (its existing part resolved: case, links, short names).
fs::path canonical(const std::string& path) {
    std::error_code ec;
    const fs::path p = fs::weakly_canonical(fs::u8path(path), ec);
    return ec ? fs::u8path(path).lexically_normal() : p;
}

bool startsWithParent(const fs::path& relative) {
    const auto first = relative.begin();
    return first != relative.end() && *first == "..";
}

}  // namespace

std::string portablePath(const std::string& path, const std::vector<std::string>& folders, bool beside) {
    if (path.empty() || path.find('$') != std::string::npos || path.find('%') != std::string::npos) return path;
    if (!isAbsolutePath(path)) return forward(path);
    const fs::path full = canonical(path);
    std::vector<fs::path> bases;
    for (const std::string& folder : folders) {
        if (!folder.empty()) bases.push_back(canonical(folder));
    }
    // `full` relative to `base` when both are on one volume ("C:" and "c:" are one drive, which
    // lexically_relative would not see), else empty.
    auto relativeTo = [&full](const fs::path& base) {
        auto volume = [](const fs::path& p) {
            std::string root = p.root_name().generic_u8string();
            for (char& c : root) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return root;
        };
        if (volume(full) != volume(base)) return fs::path();
        return full.relative_path().lexically_relative(base.relative_path());
    };
    for (const fs::path& base : bases) {
        const fs::path relative = relativeTo(base);
        if (!relative.empty() && !startsWithParent(relative)) return relative.generic_u8string();
    }
    if (beside && !bases.empty()) {
        const fs::path relative = relativeTo(bases.front());
        if (!relative.empty()) return relative.generic_u8string();
    }
    return forward(path);
}

ScopedPathResolver::ScopedPathResolver(const PathResolver* resolver) : previous_(tCurrent) {
    if (resolver) tCurrent = resolver;
}

ScopedPathResolver::~ScopedPathResolver() { tCurrent = previous_; }

}  // namespace groomlab
