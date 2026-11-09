// SPDX-License-Identifier: Apache-2.0
#include "groomlab/strand_set.h"

#include <algorithm>
#include <cstring>
#include <functional>
#include <limits>
#include <stdexcept>

#include "groomlab/parallel.h"
#include "groomlab/random.h"

namespace groomlab {

namespace {

// (contentHash hashes channel names by their characters alone, hashChars, not hashString: it is
// stored in scenes, a cards bake's "Strands=" stamp, so its values stay as they are.)

}  // namespace

void detail::copyBytesParallel(void* to, const void* from, std::size_t bytes) {
    constexpr std::size_t kBlock = std::size_t{1} << 20;  // 1 MB a task
    auto* out = static_cast<char*>(to);
    const auto* in = static_cast<const char*>(from);
    if (bytes <= kBlock) {
        std::memcpy(out, in, bytes);
        return;
    }
    parallelFor(0, (bytes + kBlock - 1) / kBlock, [&](std::size_t b) {
        const std::size_t begin = b * kBlock;
        std::memcpy(out + begin, in + begin, std::min(kBlock, bytes - begin));
    }, 1);
}

void detail::touchPagesParallel(void* at, std::size_t bytes) {
    constexpr std::size_t kPage = 4096, kBlock = std::size_t{1} << 20;  // a page a write, 1 MB a task
    auto* p = static_cast<volatile unsigned char*>(at);
    if (!p || bytes == 0) return;
    parallelFor(0, (bytes + kBlock - 1) / kBlock, [&](std::size_t b) {
        const std::size_t end = std::min(bytes, (b + 1) * kBlock);
        for (std::size_t i = b * kBlock; i < end; i += kPage) p[i] = 0;
    }, 1);
}

namespace {
std::atomic<std::size_t> taken{0};
}  // namespace

void detail::countTaken() { taken.fetch_add(1, std::memory_order_relaxed); }
std::size_t detail::arraysTaken() { return taken.load(std::memory_order_relaxed); }

StrandSet::StrandSet() { offsets_.reset().push_back(0); }

StrandSet::StrandSet(const StrandSet& o)
    : ids_(o.ids_), roots_(o.roots_), offsets_(o.offsets_), positions_(o.positions_), widths_(o.widths_),
      strandChannels_(o.strandChannels_), pointChannels_(o.pointChannels_), instances_(o.instances_),
      cardLayers_(o.cardLayers_), featherLayers_(o.featherLayers_) {
    copyExtent(o);
}

StrandSet::StrandSet(StrandSet&& o) noexcept
    : ids_(std::move(o.ids_)), roots_(std::move(o.roots_)), offsets_(std::move(o.offsets_)),
      positions_(std::move(o.positions_)), widths_(std::move(o.widths_)), strandChannels_(std::move(o.strandChannels_)),
      pointChannels_(std::move(o.pointChannels_)), instances_(std::move(o.instances_)),
      cardLayers_(std::move(o.cardLayers_)), featherLayers_(std::move(o.featherLayers_)) {
    copyExtent(o);
    o.dropExtent();
    o.offsets_.reset().push_back(0);
}

StrandSet& StrandSet::operator=(const StrandSet& o) {
    if (this == &o) return *this;
    ids_ = o.ids_;
    roots_ = o.roots_;
    offsets_ = o.offsets_;
    positions_ = o.positions_;
    widths_ = o.widths_;
    strandChannels_ = o.strandChannels_;
    pointChannels_ = o.pointChannels_;
    instances_ = o.instances_;
    cardLayers_ = o.cardLayers_;
    featherLayers_ = o.featherLayers_;
    copyExtent(o);
    return *this;
}

StrandSet& StrandSet::operator=(StrandSet&& o) noexcept {
    if (this == &o) return *this;
    ids_ = std::move(o.ids_);
    roots_ = std::move(o.roots_);
    offsets_ = std::move(o.offsets_);
    positions_ = std::move(o.positions_);
    widths_ = std::move(o.widths_);
    strandChannels_ = std::move(o.strandChannels_);
    pointChannels_ = std::move(o.pointChannels_);
    instances_ = std::move(o.instances_);
    cardLayers_ = std::move(o.cardLayers_);
    featherLayers_ = std::move(o.featherLayers_);
    copyExtent(o);
    o.dropExtent();
    o.offsets_.reset().push_back(0);
    return *this;
}

void StrandSet::reserve(std::size_t strands, std::size_t points) {
    ids_.mut(lock_).reserve(strands);
    roots_.mut(lock_).reserve(strands);
    offsets_.mut(lock_).reserve(strands + 1);
    positions_.mut(lock_).reserve(points);
    widths_.mut(lock_).reserve(points);
}

void StrandSet::clear() {
    dropExtent();
    ids_.reset();
    roots_.reset();
    offsets_.reset().push_back(0);
    positions_.reset();
    widths_.reset();
    for (auto& [name, ch] : strandChannels_) ch.values.reset();
    for (auto& [name, ch] : pointChannels_) ch.values.reset();
    instances_.reset();
    cardLayers_.clear();
    featherLayers_.clear();
}

namespace {

// Elements a block takes when an array is filled in parallel: the blocks are fixed, so the
// bits are the same however the threads split them (each element gets the one value anyway).
constexpr std::size_t kFillBlock = std::size_t{1} << 16;

// `array` sized to `size` (from the pool when it keeps one, else fresh), every element `value`:
// a recycled array is filled in parallel blocks; a fresh one comes value-initialized (T{}, as
// allocated), so only another value is written over it. `leave`: sized, its contents as they
// come. (The element types are packed: no padding bytes to compare.)
template <class T>
std::vector<T>& sizedAndFilled(CowArray<T>& array, std::size_t size, const T& value, bool leave = false) {
    bool recycled = false;
    std::vector<T>& v = array.sized(size, &recycled);
    if (leave) return v;
    const T fresh{};
    if (!recycled && std::memcmp(&value, &fresh, sizeof(T)) == 0) return v;
    parallelFor(0, (size + kFillBlock - 1) / kFillBlock, [&](std::size_t b) {
        const std::size_t begin = b * kFillBlock;
        std::fill(v.begin() + static_cast<std::ptrdiff_t>(begin), v.begin() + static_cast<std::ptrdiff_t>(std::min(size, begin + kFillBlock)),
                  value);
    }, 1);
    return v;
}

}  // namespace

void StrandSet::resizeUniform(std::size_t strands, std::size_t pointsPerStrand, unsigned leave) {
    dropExtent();
    const std::size_t points = strands * pointsPerStrand;
    sizedAndFilled(ids_, strands, StrandId{0});
    sizedAndFilled(roots_, strands, StrandRoot{});
    std::vector<std::size_t>& offsets = offsets_.sized(strands + 1);
    parallelFor(0, (strands + 1 + kFillBlock - 1) / kFillBlock, [&](std::size_t b) {
        const std::size_t begin = b * kFillBlock, end = std::min(strands + 1, begin + kFillBlock);
        for (std::size_t i = begin; i < end; ++i) offsets[i] = i * pointsPerStrand;
    }, 1);
    sizedAndFilled(positions_, points, Vec3f{}, (leave & kLeavePositions) != 0);
    sizedAndFilled(widths_, points, 1.0f, (leave & kLeaveWidths) != 0);
    for (auto& [name, ch] : strandChannels_) sizedAndFilled(ch.values, strands, ch.defaultValue);
    for (auto& [name, ch] : pointChannels_) sizedAndFilled(ch.values, points, ch.defaultValue);
}

void StrandSet::resize(const std::vector<std::size_t>& pointsPerStrand, unsigned leave) {
    dropExtent();
    const std::size_t strands = pointsPerStrand.size();
    std::vector<std::size_t>& offsets = offsets_.sized(strands + 1);
    offsets[0] = 0;
    for (std::size_t i = 0; i < strands; ++i) offsets[i + 1] = offsets[i] + pointsPerStrand[i];
    const std::size_t points = offsets[strands];
    sizedAndFilled(ids_, strands, StrandId{0});
    sizedAndFilled(roots_, strands, StrandRoot{});
    sizedAndFilled(positions_, points, Vec3f{}, (leave & kLeavePositions) != 0);
    sizedAndFilled(widths_, points, 1.0f, (leave & kLeaveWidths) != 0);
    for (auto& [name, ch] : strandChannels_) sizedAndFilled(ch.values, strands, ch.defaultValue);
    for (auto& [name, ch] : pointChannels_) sizedAndFilled(ch.values, points, ch.defaultValue);
}

std::size_t StrandSet::uniformPointCount() const {
    if (empty()) return 0;
    const std::size_t n = strandPointCount(0);
    for (std::size_t i = 1; i < strandCount(); ++i) {
        if (strandPointCount(i) != n) return 0;
    }
    return n;
}

std::size_t StrandSet::maxPointCount() const {
    std::size_t most = 0;
    for (std::size_t i = 0; i < strandCount(); ++i) most = std::max(most, strandPointCount(i));
    return most;
}

std::size_t StrandSet::addStrand(StrandId id, const std::vector<Vec3f>& points,
                                 const std::vector<float>& widths, StrandRoot root) {
    if (!widths.empty() && widths.size() != points.size()) {
        throw std::invalid_argument("StrandSet::addStrand: widths must be empty or match points");
    }
    dropExtent();
    std::vector<StrandId>& ids = ids_.mut(lock_);
    std::vector<Vec3f>& positions = positions_.mut(lock_);
    std::vector<float>& w = widths_.mut(lock_);
    const std::size_t index = ids.size();
    ids.push_back(id);
    roots_.mut(lock_).push_back(root);
    positions.insert(positions.end(), points.begin(), points.end());
    if (widths.empty()) {
        w.insert(w.end(), points.size(), 1.0f);
    } else {
        w.insert(w.end(), widths.begin(), widths.end());
    }
    offsets_.mut(lock_).push_back(positions.size());

    for (auto& [name, ch] : strandChannels_) ch.values.mut(lock_).push_back(ch.defaultValue);
    for (auto& [name, ch] : pointChannels_) {
        std::vector<float>& values = ch.values.mut(lock_);
        values.insert(values.end(), points.size(), ch.defaultValue);
    }
    return index;
}

std::vector<float>& StrandSet::addStrandChannel(const std::string& name, float defaultValue) {
    auto& ch = strandChannels_[name];
    ch.defaultValue = defaultValue;
    std::vector<float>& values = ch.values.reset();
    values.assign(strandCount(), defaultValue);
    return values;
}

std::vector<float>& StrandSet::addPointChannel(const std::string& name, float defaultValue) {
    auto& ch = pointChannels_[name];
    ch.defaultValue = defaultValue;
    std::vector<float>& values = ch.values.reset();
    values.assign(pointCount(), defaultValue);
    return values;
}

const std::vector<float>* StrandSet::strandChannel(const std::string& name) const {
    auto it = strandChannels_.find(name);
    return it == strandChannels_.end() ? nullptr : &it->second.values.get();
}

const std::vector<float>* StrandSet::pointChannel(const std::string& name) const {
    auto it = pointChannels_.find(name);
    return it == pointChannels_.end() ? nullptr : &it->second.values.get();
}

float StrandSet::strandChannelDefault(const std::string& name) const {
    auto it = strandChannels_.find(name);
    return it == strandChannels_.end() ? 0.0f : it->second.defaultValue;
}

float StrandSet::pointChannelDefault(const std::string& name) const {
    auto it = pointChannels_.find(name);
    return it == pointChannels_.end() ? 0.0f : it->second.defaultValue;
}

std::vector<float>* StrandSet::mutableStrandChannel(const std::string& name) {
    auto it = strandChannels_.find(name);
    return it == strandChannels_.end() ? nullptr : &it->second.values.mut(lock_);
}

std::vector<float>* StrandSet::mutablePointChannel(const std::string& name) {
    auto it = pointChannels_.find(name);
    return it == pointChannels_.end() ? nullptr : &it->second.values.mut(lock_);
}

std::vector<Vec3f>& StrandSet::copyPositionsFrom(const StrandSet& from) {
    dropExtent();
    return positions_.copyOf(from.positions_);
}

std::vector<float>& StrandSet::copyWidthsFrom(const StrandSet& from) { return widths_.copyOf(from.widths_); }

void StrandSet::sharePositionsOf(const StrandSet& from) {
    if (&from == this) return;
    positions_ = from.positions_;
    copyExtent(from);  // (the same strands and points: its extent)
}

std::vector<Vec3f>& StrandSet::takePositionsFrom(StrandSet& from) {
    dropExtent();
    from.dropExtent();
    return positions_.takeFrom(from.positions_);
}

std::vector<float>& StrandSet::takeWidthsFrom(StrandSet& from) { return widths_.takeFrom(from.widths_); }

std::vector<float>* StrandSet::copyStrandChannelFrom(const std::string& name, const StrandSet& from) {
    const auto it = from.strandChannels_.find(name);
    if (it == from.strandChannels_.end() || it->second.values.get().size() != strandCount()) return nullptr;
    Channel& ch = strandChannels_[name];
    ch.defaultValue = it->second.defaultValue;
    return &ch.values.copyOf(it->second.values);
}

std::vector<float>* StrandSet::copyPointChannelFrom(const std::string& name, const StrandSet& from) {
    const auto it = from.pointChannels_.find(name);
    if (it == from.pointChannels_.end() || it->second.values.get().size() != pointCount()) return nullptr;
    Channel& ch = pointChannels_[name];
    ch.defaultValue = it->second.defaultValue;
    return &ch.values.copyOf(it->second.values);
}

std::vector<std::string> StrandSet::strandChannelNames() const {
    std::vector<std::string> names;
    for (const auto& [name, ch] : strandChannels_) names.push_back(name);
    return names;
}

std::vector<std::string> StrandSet::pointChannelNames() const {
    std::vector<std::string> names;
    for (const auto& [name, ch] : pointChannels_) names.push_back(name);
    return names;
}

StrandSet StrandSet::withDensity(float density) const {
    const std::vector<StrandId>& ids = ids_.get();
    std::vector<std::uint8_t> keep(ids.size());
    parallelFor(0, ids.size(), [&](std::size_t i) { keep[i] = keepAtDensity(ids[i], density) ? 1 : 0; });
    std::vector<std::size_t> kept;
    kept.reserve(ids.size());
    for (std::size_t i = 0; i < ids.size(); ++i) {
        if (keep[i]) kept.push_back(i);
    }
    return subset(kept);  // instances resolve by strand ID: dropped strands drop theirs
}

void StrandSet::append(const StrandSet& other) {
    dropExtent();
    const std::size_t strandsBefore = strandCount();
    const std::size_t base = positions_.get().size();
    // (A copy's shared arrays made once at their new size and filled in parallel: CowArray::appended.)
    std::vector<StrandId>& ids = ids_.appended(other.ids_.get(), lock_);
    roots_.appended(other.roots_.get(), lock_);
    std::vector<Vec3f>& positions = positions_.appended(other.positions_.get(), lock_);
    widths_.appended(other.widths_.get(), lock_);
    std::vector<std::size_t>& offsets = offsets_.mut(lock_);
    const auto& otherOffsets = other.offsets_.get();
    offsets.reserve(offsets.size() + other.strandCount());
    for (std::size_t i = 1; i < otherOffsets.size(); ++i) offsets.push_back(base + otherOffsets[i]);
    // Each channel carries both sides' values; where one side lacks it, its strands take the
    // default. A point channel one side lacks takes, on that side's points, the value of that
    // side's strand channel of the same name along each strand when it has one (`fillMine`,
    // `fillTheirs`: from `at` of the values, that side's points).
    using Fill = std::function<void(std::vector<float>& values, std::size_t at, const std::string& name)>;
    auto carry = [this](std::map<std::string, Channel>& mine, const std::map<std::string, Channel>& theirs,
                        std::size_t before, std::size_t total, const Fill& fillMine, const Fill& fillTheirs) {
        for (auto& [name, ch] : mine) {
            auto it = theirs.find(name);
            if (it != theirs.end()) {
                ch.values.appended(it->second.values.get(), lock_);
                continue;
            }
            std::vector<float>& values = ch.values.mut(lock_);
            const std::size_t at = values.size();
            values.resize(total, ch.defaultValue);
            if (fillTheirs) fillTheirs(values, at, name);
        }
        for (const auto& [name, ch] : theirs) {
            if (mine.count(name)) continue;
            Channel& added = mine[name];
            added.defaultValue = ch.defaultValue;
            std::vector<float>& values = added.values.reset();
            values.reserve(total);
            values.assign(before, ch.defaultValue);
            if (fillMine) fillMine(values, 0, name);
            values.insert(values.end(), ch.values.get().begin(), ch.values.get().end());
        }
    };
    // A side's points from its strand channel, along each strand (offsets[0..strands] index its points).
    auto alongStrands = [](std::vector<float>& values, std::size_t at, const std::vector<std::size_t>& offsets,
                           std::size_t strands, const std::vector<float>* strandValues) {
        if (!strandValues || strandValues->size() != strands || offsets.size() < strands + 1) return;
        for (std::size_t s = 0; s < strands; ++s) {
            std::fill(values.begin() + static_cast<std::ptrdiff_t>(at + offsets[s] - offsets[0]),
                      values.begin() + static_cast<std::ptrdiff_t>(at + offsets[s + 1] - offsets[0]), (*strandValues)[s]);
        }
    };
    // The point channels go first, while this set's strand channels are still its own.
    carry(
        pointChannels_, other.pointChannels_, base, positions.size(),
        [&](std::vector<float>& values, std::size_t at, const std::string& name) {
            alongStrands(values, at, offsets, strandsBefore, strandChannel(name));
        },
        [&](std::vector<float>& values, std::size_t at, const std::string& name) {
            alongStrands(values, at, otherOffsets, other.strandCount(), other.strandChannel(name));
        });
    carry(strandChannels_, other.strandChannels_, strandsBefore, ids.size(), nullptr, nullptr);
}

StrandSet StrandSet::subset(const std::vector<std::size_t>& strands) const {
    // Where each picked strand's points go (a running sum of their counts), then every array
    // gathered in parallel, each channel looked up once.
    const std::vector<std::size_t>& srcOffsets = offsets_.get();
    const std::size_t count = strands.size();
    StrandSet out;
    std::vector<std::size_t>& offsets = out.offsets_.reset();
    offsets.resize(count + 1);
    offsets[0] = 0;
    for (std::size_t k = 0; k < count; ++k) {
        const std::size_t i = strands[k];
        offsets[k + 1] = offsets[k] + (srcOffsets[i + 1] - srcOffsets[i]);
    }
    const std::size_t points = offsets[count];
    const std::vector<StrandId>& srcIds = ids_.get();
    const std::vector<StrandRoot>& srcRoots = roots_.get();
    const std::vector<Vec3f>& srcPositions = positions_.get();
    const std::vector<float>& srcWidths = widths_.get();
    std::vector<StrandId>& ids = out.ids_.reset();
    std::vector<StrandRoot>& roots = out.roots_.reset();
    std::vector<Vec3f>& positions = out.positions_.reset();
    std::vector<float>& widths = out.widths_.reset();
    ids.resize(count);
    roots.resize(count);
    positions.resize(points);
    widths.resize(points);
    std::vector<std::pair<const std::vector<float>*, std::vector<float>*>> perStrand, perPoint;
    for (const auto& [name, ch] : strandChannels_) {
        Channel& to = out.strandChannels_[name];
        to.defaultValue = ch.defaultValue;
        std::vector<float>& values = to.values.reset();
        values.resize(count);
        perStrand.emplace_back(&ch.values.get(), &values);
    }
    for (const auto& [name, ch] : pointChannels_) {
        Channel& to = out.pointChannels_[name];
        to.defaultValue = ch.defaultValue;
        std::vector<float>& values = to.values.reset();
        values.resize(points);
        perPoint.emplace_back(&ch.values.get(), &values);
    }
    parallelFor(0, count, [&](std::size_t k) {
        const std::size_t i = strands[k];
        ids[k] = srcIds[i];
        roots[k] = srcRoots[i];
        for (const auto& [from, to] : perStrand) (*to)[k] = (*from)[i];
        const std::size_t begin = srcOffsets[i], end = srcOffsets[i + 1], at = offsets[k];
        const auto first = static_cast<std::ptrdiff_t>(begin), last = static_cast<std::ptrdiff_t>(end);
        std::copy(srcPositions.begin() + first, srcPositions.begin() + last, positions.begin() + static_cast<std::ptrdiff_t>(at));
        std::copy(srcWidths.begin() + first, srcWidths.begin() + last, widths.begin() + static_cast<std::ptrdiff_t>(at));
        for (const auto& [from, to] : perPoint) {
            std::copy(from->begin() + first, from->begin() + last, to->begin() + static_cast<std::ptrdiff_t>(at));
        }
    });
    out.instances_ = instances_;
    out.cardLayers_ = cardLayers_;
    out.featherLayers_ = featherLayers_;
    return out;
}

StrandSet StrandSet::withFirstPoints(const std::vector<Vec3f>& first) const {
    const std::vector<std::size_t>& srcOffsets = offsets_.get();
    const std::size_t count = strandCount();
    StrandSet out;
    out.ids_ = ids_;
    out.roots_ = roots_;
    out.strandChannels_ = strandChannels_;
    out.instances_ = instances_;
    out.cardLayers_ = cardLayers_;
    out.featherLayers_ = featherLayers_;
    std::vector<std::size_t>& offsets = out.offsets_.reset();
    offsets.resize(count + 1);
    offsets[0] = 0;
    for (std::size_t i = 0; i < count; ++i) {
        const std::size_t n = srcOffsets[i + 1] - srcOffsets[i];
        offsets[i + 1] = offsets[i] + n + (n > 0 ? 1 : 0);
    }
    const std::size_t points = offsets[count];
    const std::vector<Vec3f>& srcPositions = positions_.get();
    const std::vector<float>& srcWidths = widths_.get();
    std::vector<Vec3f>& positions = out.positions_.sized(points);
    std::vector<float>& widths = out.widths_.sized(points);
    std::vector<std::pair<const std::vector<float>*, std::vector<float>*>> perPoint;
    for (const auto& [name, ch] : pointChannels_) {
        Channel& to = out.pointChannels_[name];
        to.defaultValue = ch.defaultValue;
        perPoint.emplace_back(&ch.values.get(), &to.values.sized(points));
    }
    parallelFor(0, count, [&](std::size_t i) {
        const std::size_t begin = srcOffsets[i], end = srcOffsets[i + 1], at = offsets[i];
        if (begin == end) return;
        const auto b = static_cast<std::ptrdiff_t>(begin), e = static_cast<std::ptrdiff_t>(end);
        const auto to = static_cast<std::ptrdiff_t>(at + 1);
        positions[at] = first[i];
        std::copy(srcPositions.begin() + b, srcPositions.begin() + e, positions.begin() + to);
        widths[at] = srcWidths[begin];
        std::copy(srcWidths.begin() + b, srcWidths.begin() + e, widths.begin() + to);
        for (const auto& [from, into] : perPoint) {
            (*into)[at] = (*from)[begin];
            std::copy(from->begin() + b, from->begin() + e, into->begin() + to);
        }
    }, 1024);
    return out;
}

bool StrandSet::validate(std::string* error) const {
    auto fail = [error](const std::string& msg) {
        if (error) *error = msg;
        return false;
    };
    const auto& ids = ids_.get();
    const auto& offsets = offsets_.get();
    const auto& positions = positions_.get();

    if (offsets.size() != ids.size() + 1) return fail("offsets size must be strandCount + 1");
    if (roots_.get().size() != ids.size()) return fail("roots size must equal strandCount");
    if (offsets.front() != 0) return fail("offsets must start at 0");
    if (offsets.back() != positions.size()) return fail("last offset must equal pointCount");
    for (std::size_t i = 1; i < offsets.size(); ++i) {
        if (offsets[i] < offsets[i - 1]) return fail("offsets must be non-decreasing");
    }
    if (widths_.get().size() != positions.size()) return fail("widths size must equal pointCount");
    for (const auto& [name, ch] : strandChannels_) {
        if (ch.values.get().size() != ids.size()) return fail("strand channel '" + name + "' has wrong size");
    }
    for (const auto& [name, ch] : pointChannels_) {
        if (ch.values.get().size() != positions.size()) return fail("point channel '" + name + "' has wrong size");
    }
    return true;
}

std::uint64_t StrandSet::contentHash() const {
    const auto& ids = ids_.get();
    const auto& roots = roots_.get();
    const auto& offsets = offsets_.get();
    std::uint64_t h = hash64(strandCount());
    for (std::size_t i = 0; i < strandCount(); ++i) {
        h = hashCombine(h, ids[i]);
        h = hashCombine(h, static_cast<std::uint64_t>(offsets[i + 1]));
        h = hashCombine(h, static_cast<std::uint64_t>(static_cast<std::int64_t>(roots[i].face)));
        h = hashFloat(hashFloat(h, roots[i].u), roots[i].v);
    }
    for (const Vec3f& p : positions_.get()) {
        h = hashFloat(hashFloat(hashFloat(h, p.x), p.y), p.z);
    }
    for (float w : widths_.get()) h = hashFloat(h, w);
    for (const auto& [name, ch] : strandChannels_) {
        h = hashChars(h, name);
        for (float f : ch.values.get()) h = hashFloat(h, f);
    }
    for (const auto& [name, ch] : pointChannels_) {
        h = hashChars(h, name);
        for (float f : ch.values.get()) h = hashFloat(h, f);
    }
    if (instances_) h = hashCombine(h, instances_->contentHash());
    return h;
}

void StrandSet::copyExtent(const StrandSet& o) {
    if (&o == this) return;
    std::shared_ptr<const Extent> theirs;
    std::shared_ptr<const std::vector<Extent>> blocks;
    if (o.hasExtent_.load(std::memory_order_acquire)) {
        std::lock_guard<std::mutex> guard(o.extentLock_);
        theirs = o.extent_;
        blocks = o.extentBlocks_;
    }
    std::lock_guard<std::mutex> guard(extentLock_);
    extent_ = std::move(theirs);
    extentBlocks_ = std::move(blocks);
    hasExtent_.store(extent_ != nullptr, std::memory_order_release);
}

namespace {

constexpr std::size_t kExtentBlock = 2048;  // strands a part of extent() covers

// The bounds and longest strand of strands [first, end): each strand's length summed in order along it.
StrandSet::Extent blockExtent(const std::vector<std::size_t>& offsets, const std::vector<Vec3f>& points, std::size_t first,
                              std::size_t end) {
    constexpr float kBig = std::numeric_limits<float>::max();
    StrandSet::Extent e{Vec3f(kBig, kBig, kBig), Vec3f(-kBig, -kBig, -kBig), 0.0f};
    float sum = 0.0f;  // NaN or infinite when any coordinate is (min and max pass a NaN over)
    for (std::size_t s = first; s < end; ++s) {
        const Vec3f* p = points.data() + offsets[s];
        const std::size_t n = offsets[s + 1] - offsets[s];
        float length = 0.0f;
        for (std::size_t k = 0; k < n; ++k) {
            e.lo = Vec3f(std::min(e.lo.x, p[k].x), std::min(e.lo.y, p[k].y), std::min(e.lo.z, p[k].z));
            e.hi = Vec3f(std::max(e.hi.x, p[k].x), std::max(e.hi.y, p[k].y), std::max(e.hi.z, p[k].z));
            sum += (p[k].x - p[k].x) + (p[k].y - p[k].y) + (p[k].z - p[k].z);  // 0, or NaN for a NaN or an infinity
            if (k > 0) length += (p[k] - p[k - 1]).length();
        }
        e.longestStrand = std::max(e.longestStrand, length);
    }
    e.finite = sum == 0.0f;
    return e;
}

// The parts combined: min and max per axis and the longest, none of which depends on the order.
StrandSet::Extent combineExtents(const std::vector<StrandSet::Extent>& parts) {
    constexpr float kBig = std::numeric_limits<float>::max();
    StrandSet::Extent out{Vec3f(kBig, kBig, kBig), Vec3f(-kBig, -kBig, -kBig), 0.0f};
    for (const StrandSet::Extent& e : parts) {
        out.lo = Vec3f(std::min(out.lo.x, e.lo.x), std::min(out.lo.y, e.lo.y), std::min(out.lo.z, e.lo.z));
        out.hi = Vec3f(std::max(out.hi.x, e.hi.x), std::max(out.hi.y, e.hi.y), std::max(out.hi.z, e.hi.z));
        out.longestStrand = std::max(out.longestStrand, e.longestStrand);
        out.finite = out.finite && e.finite;
    }
    return out;
}

}  // namespace

StrandSet::Extent StrandSet::extent() const {
    if (hasExtent_.load(std::memory_order_acquire)) {
        std::lock_guard<std::mutex> guard(extentLock_);
        if (extent_) return *extent_;
    }
    // Strands in blocks side by side; each strand's length summed in order along it, so the
    // longest is what a serial walk gives, and the bounds' min and max do not depend on order.
    const std::vector<std::size_t>& offsets = offsets_.get();
    const std::vector<Vec3f>& points = positions_.get();
    const std::size_t strands = strandCount();
    const std::size_t blocks = (strands + kExtentBlock - 1) / kExtentBlock;
    auto parts = std::make_shared<std::vector<Extent>>(blocks);
    parallelFor(0, blocks, [&](std::size_t b) {
        (*parts)[b] = blockExtent(offsets, points, b * kExtentBlock, std::min(strands, (b + 1) * kExtentBlock));
    }, 1);
    const Extent out = combineExtents(*parts);
    std::lock_guard<std::mutex> guard(extentLock_);
    extent_ = std::make_shared<const Extent>(out);
    extentBlocks_ = std::move(parts);
    hasExtent_.store(true, std::memory_order_release);
    return out;
}

StrandSet::Extent StrandSet::extentAfter(const StrandSet& previous, const std::vector<std::uint32_t>& changed) const {
    std::shared_ptr<const std::vector<Extent>> before;
    if (previous.hasExtent_.load(std::memory_order_acquire)) {
        std::lock_guard<std::mutex> guard(previous.extentLock_);
        before = previous.extentBlocks_;
    }
    const std::size_t strands = strandCount();
    const std::size_t blocks = (strands + kExtentBlock - 1) / kExtentBlock;
    if (!before || before->size() != blocks || previous.strandCount() != strands || previous.pointCount() != pointCount()) {
        return extent();
    }
    if (hasExtent_.load(std::memory_order_acquire)) {
        std::lock_guard<std::mutex> guard(extentLock_);
        if (extent_) return *extent_;
    }
    const std::vector<std::size_t>& offsets = offsets_.get();
    const std::vector<Vec3f>& points = positions_.get();
    auto parts = std::make_shared<std::vector<Extent>>(*before);
    std::vector<std::size_t> again;  // the blocks with a changed strand
    for (const std::uint32_t s : changed) {
        const std::size_t b = s / kExtentBlock;
        if (b < blocks && (again.empty() || again.back() != b)) again.push_back(b);
    }
    parallelFor(0, again.size(), [&](std::size_t i) {
        const std::size_t b = again[i];
        (*parts)[b] = blockExtent(offsets, points, b * kExtentBlock, std::min(strands, (b + 1) * kExtentBlock));
    }, 1);
    const Extent out = combineExtents(*parts);
    std::lock_guard<std::mutex> guard(extentLock_);
    extent_ = std::make_shared<const Extent>(out);
    extentBlocks_ = std::move(parts);
    hasExtent_.store(true, std::memory_order_release);
    return out;
}

void StrandSet::forEachArray(const std::function<void(const void*, std::size_t)>& visit) const {
    auto one = [&](const auto& array) {
        const auto& v = array.get();
        if (!v.empty()) visit(v.data(), v.size() * sizeof(v[0]));
    };
    one(ids_);
    one(roots_);
    one(offsets_);
    one(positions_);
    one(widths_);
    for (const auto& [name, channel] : strandChannels_) one(channel.values);
    for (const auto& [name, channel] : pointChannels_) one(channel.values);
}

}  // namespace groomlab