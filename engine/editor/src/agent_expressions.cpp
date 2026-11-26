// SPDX-License-Identifier: Apache-2.0
#include "groomlab/editor/agent_expressions.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <memory>
#include <set>

namespace groomlab::agent {

namespace {

// A map read from the scalp's color set of its name (the vertex maps a session lays on its scalp).
class ColorSetMap : public ExpressionMap {
public:
    ColorSetMap(const ScalpMesh* scalp, std::string name) : scalp_(scalp), name_(std::move(name)) {}
    float at(const StrandRoot& root, int component) const override {
        const ScalpMesh::Color c = scalp_->colorAt(name_, root);
        if (component >= 0 && component < 4) return c[static_cast<std::size_t>(component)];
        return 0.2126f * c[0] + 0.7152f * c[1] + 0.0722f * c[2];
    }

private:
    const ScalpMesh* scalp_;
    std::string name_;
};

// The names an expression's text writes (with or without `$`), not calls and not inside quotes.
std::vector<std::string> namesIn(const std::string& text) {
    std::vector<std::string> out;
    char quote = 0;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (quote) {
            if (c == quote) quote = 0;
            continue;
        }
        if (c == '"' || c == '\'') {
            quote = c;
            continue;
        }
        if (c == '#' || (c == '/' && i + 1 < text.size() && text[i + 1] == '/')) {  // a comment
            while (i < text.size() && text[i] != '\n') ++i;
            continue;
        }
        if (!(std::isalpha(static_cast<unsigned char>(c)) || c == '_')) continue;
        if (i > 0 && (std::isalnum(static_cast<unsigned char>(text[i - 1])) || text[i - 1] == '.' || text[i - 1] == '_')) {
            while (i + 1 < text.size() && (std::isalnum(static_cast<unsigned char>(text[i + 1])) || text[i + 1] == '_')) ++i;
            continue;  // (inside a number or after a dot: a component)
        }
        std::size_t j = i;
        while (j < text.size() && (std::isalnum(static_cast<unsigned char>(text[j])) || text[j] == '_')) ++j;
        std::size_t k = j;
        while (k < text.size() && std::isspace(static_cast<unsigned char>(text[k]))) ++k;
        if (k >= text.size() || text[k] != '(') out.push_back(text.substr(i, j - i));
        i = j - 1;
    }
    return out;
}

}  // namespace

const std::vector<std::pair<std::string, std::string>>& expressionVariables() {
    static const std::vector<std::pair<std::string, std::string>> vars{
        {"u", "root UV on the scalp (u)"}, {"v", "root UV on the scalp (v)"},
        {"x", "root position x"}, {"y", "root position y"}, {"z", "root position z"},
        {"nx", "root normal x"}, {"ny", "root normal y"}, {"nz", "root normal z"},
        {"P", "root position (vector)"}, {"N", "root normal (vector)"},
        {"prefx", "root on the scalp's rest pose x"}, {"prefy", "... y"}, {"prefz", "... z"}, {"Pref", "the rest root (vector)"},
        {"rand", "stable random number in [0, 1) per strand"}, {"id", "the strand's ID (low 24 bits)"},
        {"faceId", "the root's triangle"}, {"index", "the strand's place in the set, 0 to 1"},
        {"length", "the strand's length"}, {"longest", "the set's longest strand"},
        {"reach", "root to tip, straight"}, {"straightness", "reach / length (1 straight)"},
        {"lift", "how far it points off the scalp: 1 straight out, 0 along it"},
        {"tipx", "tip position x"}, {"tipy", "... y"}, {"tipz", "... z"}, {"tip", "tip position (vector)"},
        {"t", "per point: 0 at the root to 1 at the tip, by length"}, {"pt", "per point: 0 to 1 by point index"},
        {"px", "per point: its position x"}, {"py", "... y"}, {"pz", "... z"}, {"pointP", "per point: its position (vector)"},
        {"pointIndex", "per point: its index"},
        {"frame", "the evaluated frame (also F)"}, {"time", "the evaluated time in seconds"},
        {"<channel>", "any other name: the strands' channel of that name (e.g. clumpId), per point when it is a point channel"}};
    return vars;
}

ExpressionCheck checkExpression(const std::string& text, const std::vector<std::pair<std::string, float>>& constants,
                                const StrandSet* strands, const ScalpMesh* scalp, std::size_t maxSamples, ExpressionType want) {
    ExpressionCheck out;
    ExpressionEnvironment env;
    if (scalp) {
        env.map = [scalp](const std::string& name) -> ExpressionMapPtr {
            if (!scalp->hasColorSet(name)) return nullptr;
            return std::make_shared<ColorSetMap>(scalp, name);
        };
    }
    out.maps = scanExpression(text).maps;
    std::unique_ptr<Expression> e;
    {
        const ExpressionEnvironmentScope scope(&env);
        try {
            e = std::make_unique<Expression>(text, constants, want);
        } catch (const ExpressionError& error) {
            out.error = error.what();
            return out;
        }
    }
    out.ok = true;
    out.vector = e->isVector();
    out.readsTime = e->readsTime();
    // The built-in variables it reads (XGen's names count as theirs).
    std::set<std::string> known;
    for (const auto& [name, about] : expressionVariables()) known.insert(name);
    for (const char* alias : {"F", "cLength", "Pg", "Pw", "Pgw", "Ng", "Prefg", "Prefw", "Prefgw", "faceid"}) known.insert(alias);
    std::set<std::string> constantNames;
    for (const auto& [name, value] : constants) constantNames.insert(name.substr(0, name.find('.')));
    for (const std::string& name : namesIn(text)) {
        if (known.count(name) && !constantNames.count(name) &&
            std::find(out.variables.begin(), out.variables.end(), name) == out.variables.end()) {
            out.variables.push_back(name);
        }
    }
    out.channels = e->channels();

    std::vector<float> values;
    if (strands && strands->strandCount() > 0) {
        const Expression::Bound bound = e->bind(*strands);
        out.perPoint = bound.perPoint;
        for (const std::string& c : out.channels) {
            if (!strands->strandChannel(c) && !strands->pointChannel(c)) out.missingChannels.push_back(c);
        }
        const std::size_t n = strands->strandCount();
        const std::size_t stride = std::max<std::size_t>(1, n / std::max<std::size_t>(1, maxSamples));
        out.sampledOver = bound.perPoint ? "points" : "strands";
        for (std::size_t s = 0; s < n; s += stride) {
            if (bound.perPoint) {
                const std::size_t points = strands->strandPointCount(s);
                for (std::size_t p = 0; p < points; ++p) {
                    const float v = out.vector ? e->evaluatePointVector(*strands, s, p, scalp, &bound).length()
                                               : e->evaluatePoint(*strands, s, p, scalp, &bound);
                    values.push_back(v);
                    if (out.examples.size() < 6 && (p == 0 || p + 1 == points)) {
                        out.examples.push_back({"strand " + std::to_string(s) + (p == 0 ? " root" : " tip"), v});
                    }
                }
            } else {
                const float v = out.vector ? e->evaluateVector(*strands, s, scalp, &bound).length() : e->evaluate(*strands, s, scalp, &bound);
                values.push_back(v);
                if (out.examples.size() < 6) out.examples.push_back({"strand " + std::to_string(s), v});
            }
        }
    } else if (scalp && scalp->triangleCount() > 0) {
        out.sampledOver = "scalp";
        const std::size_t n = scalp->triangleCount();
        const std::size_t stride = std::max<std::size_t>(1, n / std::max<std::size_t>(1, maxSamples));
        for (std::size_t t = 0; t < n; t += stride) {
            StrandRoot root;
            root.face = static_cast<std::int32_t>(t);
            root.u = root.v = 1.0f / 3.0f;
            const float v = out.vector ? e->evaluateVectorAt(root, scalp).length() : e->evaluateAt(root, scalp);
            values.push_back(v);
            if (out.examples.size() < 6) out.examples.push_back({"triangle " + std::to_string(t), v});
        }
    }
    std::size_t nonFinite = 0;
    values.erase(std::remove_if(values.begin(), values.end(), [&](float v) { return !std::isfinite(v) && ++nonFinite; }), values.end());
    if (nonFinite) out.error = std::to_string(nonFinite) + " samples were not finite numbers (a division by 0, a log of 0...)";
    out.samples = values.size();
    if (!values.empty()) {
        out.min = *std::min_element(values.begin(), values.end());
        out.max = *std::max_element(values.begin(), values.end());
        double sum = 0.0;
        for (const float v : values) sum += v;
        out.mean = static_cast<float>(sum / static_cast<double>(values.size()));
        out.histogram.assign(10, 0);
        const float span = out.max - out.min;
        for (const float v : values) {
            const int bin = span > 0.0f ? std::min(9, static_cast<int>((v - out.min) / span * 10.0f)) : 0;
            ++out.histogram[static_cast<std::size_t>(std::max(0, bin))];
        }
    }
    return out;
}

}  // namespace groomlab::agent
