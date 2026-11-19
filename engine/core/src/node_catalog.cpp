// SPDX-License-Identifier: Apache-2.0
#include "groomlab/node_catalog.h"

#include <algorithm>
#include <cctype>
#include <map>

#include "groomlab/graph.h"

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
