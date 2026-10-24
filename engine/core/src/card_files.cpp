// SPDX-License-Identifier: Apache-2.0
#include "groomlab/card_files.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <stdexcept>

#include "groomlab/display.h"
#include "groomlab/groom_session.h"
#include "groomlab/parallel.h"
#include "groomlab/png.h"

namespace groomlab {

namespace fs = std::filesystem;

const std::vector<std::string>& cardMapNames() {
    static const std::vector<std::string> names = {"Depth", "Coverage", "Tangent", "Attribute", "Color", "AO", "Normal"};
    return names;
}

const std::vector<std::string>& cardBakeSuffixes() {
    static const std::vector<std::string> suffixes = [] {
        std::vector<std::string> out;
        for (const std::string& map : cardMapNames()) out.push_back(map + ".tga");
        out.push_back("Atlas.json");
        return out;
    }();
    return suffixes;
}

CardBakePaths CardBakePaths::in(const std::string& folder, const std::string& name) {
    std::string at = folder;
    for (char& c : at) {
        if (c == '\\') c = '/';
    }
    if (!at.empty() && at.back() != '/') at += '/';
    return CardBakePaths(at + name + "_");
}

CardBakePaths CardBakePaths::besideGroomFile(const std::string& groomPath) {
    const fs::path groom = fs::u8path(groomPath);
    return CardBakePaths((groom.parent_path() / fs::u8path(groomFileCardBakePrefix(groom.stem().u8string()))).generic_u8string());
}

std::vector<std::string> CardBakePaths::maps() const {
    std::vector<std::string> out;
    for (const std::string& name : cardMapNames()) out.push_back(map(name));
    return out;
}

std::vector<std::string> CardBakePaths::files() const {
    std::vector<std::string> out;
    for (const std::string& suffix : cardBakeSuffixes()) out.push_back(prefix + suffix);
    return out;
}

bool CardBakePaths::exists() const {
    std::error_code ec;
    return !prefix.empty() && fs::exists(fs::u8path(atlas()), ec) && fs::exists(fs::u8path(map("Color")), ec);
}

std::string groomFileCardBakePrefix(const std::string& stem) { return stem + "_cards/" + stem + "_"; }

std::string formatCardMaps(const std::string& prefix, const std::string& stamp, int bake) {
    return "Color=" + prefix + "Color.tga;Coverage=" + prefix + "Coverage.tga;AO=" + prefix + "AO.tga;Atlas=" + prefix +
           "Atlas.json" + (stamp.empty() ? std::string() : ";Strands=" + stamp) + ";Bake=" + std::to_string(bake);
}

std::string cardMapsPrefix(const std::string& cardMaps) {
    const auto maps = parseCardMaps(cardMaps);
    const auto color = maps.find("Color");
    static const std::string suffix = "Color.tga";
    if (color == maps.end() || color->second.size() < suffix.size() ||
        color->second.compare(color->second.size() - suffix.size(), suffix.size(), suffix) != 0) {
        return {};
    }
    return color->second.substr(0, color->second.size() - suffix.size());
}

std::string withCardMapsPrefix(const std::string& cardMaps, const std::string& prefix) {
    const std::string old = cardMapsPrefix(cardMaps);
    if (old.empty()) return cardMaps;
    std::string out;
    std::size_t start = 0;
    while (start <= cardMaps.size()) {
        std::size_t end = cardMaps.find(';', start);
        if (end == std::string::npos) end = cardMaps.size();
        std::string entry = cardMaps.substr(start, end - start);
        const std::size_t eq = entry.find('=');
        if (eq != std::string::npos) {
            const std::string key = entry.substr(0, eq);
            std::string value = entry.substr(eq + 1);
            for (char& c : value) {
                if (c == '\\') c = '/';
            }
            std::string from = old;
            for (char& c : from) {
                if (c == '\\') c = '/';
            }
            if ((key == "Color" || key == "Coverage" || key == "AO" || key == "Atlas") && value.compare(0, from.size(), from) == 0) {
                entry = key + "=" + prefix + value.substr(from.size());
            }
        }
        if (!entry.empty()) out += (out.empty() ? "" : ";") + entry;
        start = end + 1;
    }
    return out;
}

namespace {

// The cards of `strands` laid out for a bake (nothing baked yet).
CardBake laidOutCards(StrandsPtr strands, MeshPtr scalp) {
    if (!strands || strands->cardLayers().empty()) throw std::runtime_error("no cards: add a Cards node to the graph");
    CardBake bake;
    bake.strands = std::move(strands);
    bake.scalp = std::move(scalp);
    // A fresh layout for the bake (the atlas written with it is what later views keep).
    bake.cards = std::make_shared<CardMesh>(combineCardLayers(bake.strands->cardLayers()));
    if (bake.cards->cards.empty()) throw std::runtime_error("no cards: the Cards node made none (too few strands per clump?)");
    layoutCards(*bake.cards, bake.cards->padding);
    return bake;
}

}  // namespace

int cardMapResolution(const CardMesh& cards, const CardMapExport& settings) {
    const int resolution = settings.resolution > 0 ? settings.resolution : (cards.atlasSize > 0 ? cards.atlasSize : 2048);
    return std::clamp(resolution, 16, 8192);
}

CardBake bakeCardMapsInMemory(StrandsPtr strands, MeshPtr scalp, const CardMapExport& settings, float occlusion) {
    CardBake bake = laidOutCards(std::move(strands), std::move(scalp));
    const int resolution = cardMapResolution(*bake.cards, settings);
    CardBakeSettings base;
    base.scalp = bake.scalp.get();
    base.occlusion = occlusion;
    base.minStrandTexels = bake.cards->thinnestStrand;  // (the Cards nodes' Thinnest Strand)
    const CardBakeSettings bakeSettings = cardMapBakeSettings(base, settings, resolution);
    CardMaps maps = bakeCardMaps(*bake.strands, *bake.cards, bakeSettings);
    if (maps.size > resolution) maps = downsampleCardMaps(maps, maps.size / resolution);
    bake.maps = std::make_shared<const CardMaps>(std::move(maps));
    return bake;
}

CardBake bakeCardsInMemory(StrandsPtr strands, MeshPtr scalp, int resolution, float occlusion) {
    CardBake bake = laidOutCards(std::move(strands), std::move(scalp));
    CardBakeSettings settings;
    settings.size = resolution > 0 ? resolution : (bake.cards->atlasSize > 0 ? bake.cards->atlasSize : 2048);
    settings.scalp = bake.scalp.get();
    settings.occlusion = occlusion;
    settings.minStrandTexels = bake.cards->thinnestStrand;  // (the Cards nodes' Thinnest Strand)
    bake.textures = bakeCardTextures(*bake.strands, *bake.cards, settings);
    return bake;
}

std::string writeCardBake(const CardBake& bake, const CardBakePaths& at, bool preview) {
    const CardTextures& t = bake.textures;
    std::error_code ec;
    const fs::path folder = fs::u8path(at.prefix).parent_path();
    if (!folder.empty()) fs::create_directories(folder, ec);
    // Unreal's hair card textures, the ambient occlusion and the normal: written side by side.
    // With iridescence, the Color map carries it as its alpha.
    std::vector<std::uint8_t> colorAlpha;
    if (t.iridescence.size() * 3 == t.color.size()) {
        colorAlpha.resize(t.iridescence.size() * 4);
        for (std::size_t i = 0; i < t.iridescence.size(); ++i) {
            for (std::size_t k = 0; k < 3; ++k) colorAlpha[i * 4 + k] = t.color[i * 3 + k];
            colorAlpha[i * 4 + 3] = t.iridescence[i];
        }
    }
    const std::vector<std::pair<int, const std::vector<std::uint8_t>*>> maps = {
        {1, &t.depth}, {1, &t.coverage}, {3, &t.tangent}, {4, &t.attribute},
        colorAlpha.empty() ? std::make_pair(3, &t.color) : std::make_pair(4, static_cast<const std::vector<std::uint8_t>*>(&colorAlpha)), {1, &t.occlusion}, {3, &t.normal}};
    const std::vector<std::string> paths = at.maps();
    parallelFor(0, maps.size(), [&](std::size_t i) { writeTga(paths[i], t.size, t.size, maps[i].first, *maps[i].second); }, 1);
    // The atlas: every card's cell by its strands, so the cards rebuilt from now on (guides
    // brushed, the groom re-evaluated) keep the texture they were baked with.
    {
        std::ofstream out(fs::u8path(at.atlas()), std::ios::binary | std::ios::trunc);
        if (!out) throw std::runtime_error("can't write " + at.atlas());
        out << captureCardAtlas(*bake.cards, bake.strands.get()).toJson();
    }
    if (preview) {
        // The viewport's map from the maps in memory, read as a viewport loads the files (RGBA8, a
        // gray map in r g b): the bake's RGB color and gray coverage and occlusion are widened.
        const std::size_t pixels = static_cast<std::size_t>(t.size) * static_cast<std::size_t>(t.size);
        std::vector<std::uint8_t> color(pixels * 4), coverage(pixels * 4), occlusion(pixels * 4);
        for (std::size_t i = 0; i < pixels; ++i) {
            for (std::size_t k = 0; k < 3; ++k) color[i * 4 + k] = t.color[i * 3 + k];
            color[i * 4 + 3] = 255;
            for (std::size_t k = 0; k < 4; ++k) {
                coverage[i * 4 + k] = t.coverage[i];
                occlusion[i * 4 + k] = t.occlusion[i];
            }
        }
        const std::vector<std::uint8_t> rgba = combineCardMaps(color.data(), coverage.data(), occlusion.data(),
                                                               static_cast<std::size_t>(t.size), static_cast<std::size_t>(t.size));
        writeCompressedPng(at.preview(), t.size, t.size, 4, rgba);
    }
    // What this bake is made from, per Cards node: the cards say which changed since (a rebake due).
    return cardsBakeStamp(summarizeCardNodes(*bake.cards, bake.strands.get()));
}

}  // namespace groomlab
