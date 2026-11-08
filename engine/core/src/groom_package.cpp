// SPDX-License-Identifier: Apache-2.0
#include "groomlab/groom_package.h"

#include <cctype>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <sstream>

#include <nlohmann/json.hpp>

#include "groomlab/asset_files.h"
#include "groomlab/card_files.h"
#include "groomlab/display.h"
#include "groomlab/paths.h"

namespace groomlab {

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

std::string bytesOf(const std::string& path) {
    std::ifstream in(fs::u8path(path), std::ios::binary);
    return in ? std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()) : std::string();
}

bool sameBytes(const std::string& a, const std::string& b) {
    std::error_code ec;
    if (fs::file_size(fs::u8path(a), ec) != fs::file_size(fs::u8path(b), ec) || ec) return false;
    return bytesOf(a) == bytesOf(b);
}

bool isFile(const std::string& path) {
    std::error_code ec;
    return !path.empty() && fs::is_regular_file(fs::u8path(path), ec);
}

// A groom's files copied into its own folder of the file: the slot is the groom's (a file of the
// same name there is its earlier copy: written over), but two files of one name this write keep
// both (name_2...).
class GroomFolderCopier {
public:
    GroomFolderCopier(std::string folder, GroomWriteReport& report) : folder_(std::move(folder)), report_(report) {}

    // `source` in the folder: itself when it is there already, else its copy (absolute).
    std::string place(const std::string& source) {
        if (insideFolder(source, folder_)) return fs::u8path(source).generic_u8string();
        const fs::path name = fs::u8path(source).filename();
        fs::path target = fs::u8path(folder_) / name;
        for (int k = 2; taken_.count(key(target)) && taken_[key(target)] != key(fs::u8path(source)); ++k) {
            target = fs::u8path(folder_) / (name.stem().u8string() + "_" + std::to_string(k) + name.extension().u8string());
        }
        taken_[key(target)] = key(fs::u8path(source));
        copy(source, target.generic_u8string());
        return target.generic_u8string();
    }

    // A file to `target` exactly (a card bake's files keep their names), written over.
    void copy(const std::string& source, const std::string& target) {
        if (samePathText(source, target)) return;
        std::error_code ec;
        if (isFile(target) && sameBytes(source, target)) return;
        fs::create_directories(fs::u8path(target).parent_path(), ec);
        fs::copy_file(fs::u8path(source), fs::u8path(target), fs::copy_options::overwrite_existing, ec);
        if (ec) {
            report_.warnings.push_back("could not copy " + source + " to " + target + ": " + ec.message());
            return;
        }
        report_.copied.push_back(target);
    }

private:
    static std::string key(const fs::path& p) {
        std::string out = p.lexically_normal().generic_u8string();
#ifdef _WIN32
        for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
#endif
        return out;
    }
    std::string folder_;
    GroomWriteReport& report_;
    std::map<std::string, std::string> taken_;  // target -> the source written there this write
};

// A groom file's grooms as JSON objects (the first: the top without format, version, grooms).
std::vector<json> entriesOf(const json& root) {
    std::vector<json> out;
    json first = root;
    for (const char* key : {"format", "version", "grooms"}) first.erase(key);
    out.push_back(std::move(first));
    if (root.contains("grooms") && root.at("grooms").is_array()) {
        for (const json& g : root.at("grooms")) out.push_back(g);
    }
    return out;
}

json parsedFile(const std::string& text, const char* what) {
    json root = json::parse(text, nullptr, false);
    if (root.is_discarded() || !root.is_object() || root.value("format", std::string()) != "groomlab.groom") {
        throw GroomFileError(std::string(what) + " is not a GroomLab groom file");
    }
    return root;
}

}  // namespace

std::string groomFileFolder(const std::string& groomFilePath, const std::string& groomName, const std::string& kind) {
    const fs::path file = fs::u8path(groomFilePath);
    const std::string stem = file.stem().u8string();
    const std::string folder = assetFolder(file.parent_path().generic_u8string(), stem, kind);
    return folder + "/" + fileSafeName(groomName.empty() ? stem : groomName);
}

int groomIndex(const std::vector<GroomFile>& grooms, const std::string& name) {
    for (std::size_t i = 0; i < grooms.size(); ++i) {
        if (!name.empty() && grooms[i].name == name) return static_cast<int>(i);
    }
    if (name.empty() && !grooms.empty()) return 0;
    return grooms.size() == 1 ? 0 : -1;
}

std::string mergeGroomFilesJson(const std::string& existing, const std::string& written) {
    const json was = parsedFile(existing, "the existing file");
    const json now = parsedFile(written, "the grooms written");
    std::vector<json> entries = entriesOf(was);
    // A file of one unnamed groom (one groom a file, as hosts wrote them): the groom written takes its place.
    if (entries.size() == 1 && entries.front().value("name", std::string()).empty()) entries.clear();
    for (json& added : entriesOf(now)) {
        const std::string name = added.value("name", std::string());
        bool replaced = false;
        for (json& e : entries) {
            if (e.value("name", std::string()) == name) {
                e = added;
                replaced = true;
                break;
            }
        }
        if (!replaced) entries.push_back(std::move(added));
    }
    json root = entries.front();
    root["format"] = now.at("format");
    root["version"] = now.value("version", 1);
    if (entries.size() > 1) root["grooms"] = json(std::vector<json>(entries.begin() + 1, entries.end()));
    return root.dump();
}

GroomWriteReport writeGroomsToFile(const std::string& path, std::vector<GroomFile> grooms, const GroomWriteOptions& options) {
    if (grooms.empty()) throw GroomFileError("groom file: no groom to write");
    {
        std::set<std::string> names;
        for (const GroomFile& g : grooms) {
            if (!names.insert(g.name).second) throw GroomFileError("groom file: two grooms named '" + g.name + "'");
        }
    }
    GroomWriteReport report;
    const std::string folder = fs::absolute(fs::u8path(path)).parent_path().generic_u8string();
    auto relative = [&](const std::string& full) { return full.empty() ? full : pathFromGroomFolder(full, folder); };
    for (GroomFile& g : grooms) {
        auto resolved = [&](const std::string& stored) { return stored.empty() ? stored : resolveGroomPath(g, stored); };

        // Texture maps: into masks/<groom>/; the params reading their images follow.
        GroomFolderCopier masks(groomFileFolder(path, g.name, "masks"), report);
        std::vector<std::pair<std::string, std::string>> images;  // (the image read, where it is now)
        for (auto& [name, info] : g.data.maps) {
            if (info.kind != "texture" || info.texture.empty()) continue;
            const std::string source = resolved(info.texture);
            std::string at = source;
            if (!isFile(source)) {
                report.warnings.push_back("map " + name + "'s image " + source + " is not there; the file names it as it is");
            } else if (options.copyFiles) {
                at = masks.place(source);
            }
            images.emplace_back(source, at);
            info.texture = relative(at);
        }
        g.data.graphJson = followGraphFiles(g.data.graphJson, [&](const std::string& stored) {
            const std::string full = resolved(stored);
            for (const auto& [read, now] : images) {
                if (samePathText(full, read)) return relative(now);
            }
            return isFile(full) ? relative(full) : stored;  // the files the graph only reads, as they are
        });

        // The card bake: into cards/<groom>/<groom>_, its preview with it.
        if (!g.cardBake.empty()) {
            const CardBakePaths from(resolved(g.cardBake.prefix));
            std::string at = from.prefix;
            if (!from.exists()) {
                report.warnings.push_back("the card bake " + from.prefix + "* is not there; the file names it as it is");
            } else if (options.copyFiles) {
                const std::string groomFolder = groomFileFolder(path, g.name, "cards");
                const std::string to = groomFolder + "/" + fs::u8path(groomFolder).filename().u8string() + "_";
                if (!samePathText(from.prefix + "Atlas.json", to + "Atlas.json")) {
                    GroomFolderCopier cards(groomFolder, report);
                    for (std::string suffix : cardBakeSuffixes()) {
                        if (isFile(from.prefix + suffix)) cards.copy(from.prefix + suffix, to + suffix);
                    }
                    if (isFile(from.preview())) cards.copy(from.preview(), to + "Preview.png");
                    at = to;
                }
                if (!isFile(at + "Preview.png")) {
                    try {
                        writeCardBakePreview(at);
                    } catch (const std::exception& e) {
                        report.warnings.push_back(std::string("the card bake has no preview map: ") + e.what());
                    }
                }
            }
            g.cardBake.prefix = relative(at);
        }

        // Simulate bakes: into caches/<groom>/.
        GroomFolderCopier caches(groomFileFolder(path, g.name, "caches"), report);
        for (auto& [node, ref] : g.simCaches) {
            const std::string source = resolved(ref.file);
            ref.file = relative(options.copyFiles && isFile(source) ? caches.place(source) : source);
        }
        // The caches the host wrote for the file (the scalp's, the guides'): where they are.
        if (!g.scalpCache.empty()) g.scalpCache.file = relative(resolved(g.scalpCache.file));
        if (!g.guideCache.empty()) g.guideCache.file = relative(resolved(g.guideCache.file));
        g.directory = folder;
    }

    std::string text = groomFilesToJson(grooms);
    if (options.merge && isFile(path)) {
        try {
            text = mergeGroomFilesJson(bytesOf(path), text);
        } catch (const GroomFileError& e) {
            throw GroomFileError(path + ": " + e.what() + " (not merged into)");
        }
    }
    writeGroomFileText(path, text);
    return report;
}

}  // namespace groomlab
