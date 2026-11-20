// SPDX-License-Identifier: Apache-2.0
#include "groomlab/node_catalog.h"

#include <algorithm>
#include <cctype>
#include <map>

#include "groomlab/graph.h"

#include <nlohmann/json.hpp>

namespace groomlab {

const std::vector<NodeCategory>& nodeCategories() {
    // What it reads, the strands grown from it, their shape, deformers, collisions, the look
    // (clumps and detail), motion, feathers, color, conversions, fields, plumbing, games.
    static const std::vector<NodeCategory> categories = {
        {"source", "Sources"}, {"grow", "Grow"},         {"shape", "Shape"},           {"deform", "Deform"},
        {"collide", "Collide"}, {"look", "Look"},        {"motion", "Motion"},         {"feathers", "Feathers"},
        {"color", "Color"},    {"convert", "Convert"},   {"field", "Fields & Curves"}, {"utility", "Utility"},
        {"games", "Games"},
    };
    return categories;
}

int nodeCategoryOrder(const std::string& id) {
    const std::vector<NodeCategory>& categories = nodeCategories();
    for (std::size_t i = 0; i < categories.size(); ++i) {
        if (categories[i].id == id) return static_cast<int>(i);
    }
    return static_cast<int>(categories.size());
}

const NodeInfo* nodeInfo(const std::string& type) { return OperatorRegistry::instance().info(type); }

const std::vector<std::string>& builtinNodeTypes() {
    static const std::vector<std::string> types = {
#define GROOMLAB_OPERATOR(Type) #Type,
#define GROOMLAB_OPERATOR_ELSEWHERE(Type) #Type,
#include "groomlab/builtin_operators.inc"
#undef GROOMLAB_OPERATOR
#undef GROOMLAB_OPERATOR_ELSEWHERE
    };
    return types;
}

int nodeMenuOrder(const std::string& type) {
    static const std::map<std::string, int> order = [] {
        std::map<std::string, int> out;
        for (const std::string& t : builtinNodeTypes()) out.emplace(t, static_cast<int>(out.size()));
        return out;
    }();
    const auto it = order.find(type);
    return it == order.end() ? static_cast<int>(order.size()) : it->second;
}

std::string paramLabel(const std::string& name) {
    // Words: split before a capital that follows a lower-case letter or a digit, and at underscores.
    std::vector<std::string> words;
    std::string word;
    for (std::size_t i = 0; i < name.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(name[i]);
        const bool after = i > 0 && (std::islower(static_cast<unsigned char>(name[i - 1])) || std::isdigit(static_cast<unsigned char>(name[i - 1])));
        if (c == '_' || c == ' ' || (std::isupper(c) && after)) {
            if (!word.empty()) words.push_back(word);
            word.clear();
            if (c == '_' || c == ' ') continue;
        }
        word += static_cast<char>(c);
    }
    if (!word.empty()) words.push_back(word);
    std::string out;
    for (std::string w : words) {
        const bool capitals = w.size() > 1 && std::any_of(w.begin(), w.end(), [](unsigned char c) { return std::isalpha(c) != 0; }) &&
                              std::none_of(w.begin(), w.end(), [](unsigned char c) { return std::islower(c) != 0; });
        if (!capitals) {
            for (char& c : w) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        out += (out.empty() ? "" : " ") + w;
    }
    if (!out.empty()) out[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(out[0])));
    return out;
}

namespace {

std::string lowered(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::vector<std::string> spaceWords(const std::string& text) {
    std::vector<std::string> out;
    std::string word;
    for (const char c : text) {
        if (c == ' ') {
            if (!word.empty()) out.push_back(std::move(word));
            word.clear();
        } else {
            word += c;
        }
    }
    if (!word.empty()) out.push_back(std::move(word));
    return out;
}

bool allIn(const std::vector<std::string>& words, const std::string& text, const std::string& orIn) {
    for (const std::string& w : words) {
        if (text.find(w) == std::string::npos && orIn.find(w) == std::string::npos) return false;
    }
    return true;
}

}  // namespace

NodeSearchWords nodeSearchWords(const std::string& specJson) {
    const nlohmann::json spec = nlohmann::json::parse(specJson, nullptr, false);
    NodeSearchWords w;
    if (!spec.is_object()) return w;
    const std::string type = spec.value("type", std::string());
    const std::string label = spec.value("label", std::string());
    const std::string category = spec.value("categoryTitle", std::string());
    w.name = lowered(type);
    w.label = lowered(label.empty() ? nodeLabel(type) : label);
    w.keywords = lowered(spec.value("keywords", std::string()) + " " + category);
    for (const nlohmann::json& p : spec.value("params", nlohmann::json::array())) {
        if (!p.is_object()) continue;
        const std::string name = p.value("name", std::string());
        w.params += lowered(name) + " " + lowered(paramLabel(name)) + " ";
        w.details += lowered(p.value("description", std::string())) + " ";
    }
    w.about = lowered(category + " " + spec.value("description", std::string()));
    return w;
}

int nodeSearchRank(const std::string& search, const NodeSearchWords& w) {
    std::string text = lowered(search);
    while (!text.empty() && text.front() == ' ') text.erase(text.begin());
    while (!text.empty() && text.back() == ' ') text.pop_back();
    if (text.empty()) return 5;
    if (w.name.rfind(text, 0) == 0 || w.label.rfind(text, 0) == 0) return 0;
    std::string joined = text;
    joined.erase(std::remove(joined.begin(), joined.end(), ' '), joined.end());
    if (w.name.find(text) != std::string::npos || w.label.find(text) != std::string::npos || w.name.find(joined) != std::string::npos) return 1;
    const std::vector<std::string> words = spaceWords(text);
    if (allIn(words, w.keywords, w.name)) return 2;
    if (allIn(words, w.params, w.name)) return 3;
    if (allIn(words, w.about, w.name)) return 4;
    const std::string everything = w.name + " " + w.keywords + " " + w.params + " " + w.about + " " + w.details;
    if (allIn(words, everything, everything)) return 5;
    return -1;
}

std::string nodeLabel(const std::string& type) {
    const NodeInfo* info = nodeInfo(type);
    if (info && !info->label.empty()) return info->label;
    std::string out;
    for (std::size_t i = 0; i < type.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(type[i]);
        const bool lowerBefore = i > 0 && std::islower(static_cast<unsigned char>(type[i - 1]));
        if (std::isupper(c) && lowerBefore) out += ' ';
        out += static_cast<char>(c);
    }
    return out;
}

}  // namespace groomlab
