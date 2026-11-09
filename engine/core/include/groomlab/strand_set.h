// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <type_traits>
#include <vector>

#include "groomlab/parallel.h"

#include "groomlab/instances.h"
#include "groomlab/math.h"

namespace groomlab {

struct CardMesh;
struct FeatherLayer;
using StrandId = std::uint64_t;

// Where a strand is rooted on the scalp mesh: triangle index plus barycentric
// (u, v); the third weight is 1 - u - v. face < 0 means "not rooted".
struct StrandRoot {
    std::int32_t face = -1;
    float u = 0.0f;
    float v = 0.0f;
};

namespace detail {

// Large arrays StrandSets let go of, a couple per element type, kept for the next copy-on-write
// clone of the same size: tens of MB freshly allocated are page-faulted in as they are first
// written, which costs about three times the copy itself; a recycled array's pages are in. A
// re-evaluated node frees its last output's arrays just as the next node clones its own, so a
// tweak's clones all land on recycled arrays. Never destroyed (arrays freed at exit still come).
template <class T>
class ArrayPool {
public:
    static constexpr std::size_t kMinBytes = std::size_t{4} << 20;
    static constexpr std::size_t kKept = 2;

    static ArrayPool& instance() {
        static ArrayPool* pool = new ArrayPool;
        return *pool;
    }
    // A kept array of exactly `size` elements (contents stale), or null.
    std::vector<T>* take(std::size_t size) {
        if (size * sizeof(T) < kMinBytes) return nullptr;
        std::lock_guard<std::mutex> guard(lock_);
        for (auto it = kept_.begin(); it != kept_.end(); ++it) {
            if ((*it)->size() == size) {
                std::vector<T>* v = *it;
                kept_.erase(it);
                return v;
            }
        }
        return nullptr;
    }
    // Keeps a large array (dropping the oldest kept beyond kKept), deletes a small one.
    void give(std::vector<T>* v) {
        if (v->size() * sizeof(T) < kMinBytes) {
            delete v;
            return;
        }
        std::vector<T>* drop = nullptr;
        {
            std::lock_guard<std::mutex> guard(lock_);
            kept_.push_back(v);
            if (kept_.size() > kKept) {
                drop = kept_.front();
                kept_.erase(kept_.begin());
            }
        }
        delete drop;
    }
    // Every kept array deleted (releaseProcessCaches), and the bytes they held.
    void clear() {
        std::vector<std::vector<T>*> drop;
        {
            std::lock_guard<std::mutex> guard(lock_);
            drop.swap(kept_);
        }
        for (std::vector<T>* v : drop) delete v;
    }
    std::size_t bytes() {
        std::lock_guard<std::mutex> guard(lock_);
        std::size_t total = 0;
        for (const std::vector<T>* v : kept_) total += v->capacity() * sizeof(T);
        return total;
    }

private:
    std::mutex lock_;
    std::vector<std::vector<T>*> kept_;
};

template <class T>
struct ToArrayPool {
    void operator()(std::vector<T>* v) const { ArrayPool<T>::instance().give(v); }
};

// `bytes` from `from` to `to`, a large copy in parallel blocks.
void copyBytesParallel(void* to, const void* from, std::size_t bytes);
// A byte written in every page of `bytes` at `at`, across the core's threads: a fresh allocation's
// pages faulted in (and zeroed by the system) in parallel rather than one by one as a single
// thread first writes them.
void touchPagesParallel(void* at, std::size_t bytes);

// A new array of `size` elements (value-initialized, as std::vector<T>(size)). A large one has
// its pages faulted in across the threads first (touchPagesParallel), so its zeroing runs over
// memory already in: a fresh 384 MB array (2M strands' points) took about 47 ms on one thread.
// Not from inside a parallel loop's body (insideParallelBody), where it may hold a lock the
// loop's other bodies take: serially there.
template <class T>
std::vector<T>* freshArray(std::size_t size) {
    auto* array = new std::vector<T>();
    if (size * sizeof(T) >= ArrayPool<T>::kMinBytes && !insideParallelBody()) {
        array->reserve(size);
        touchPagesParallel(static_cast<void*>(array->data()), size * sizeof(T));
    }
    array->resize(size);
    return array;
}

// Arrays taken rather than copied (CowArray::takeFrom) since the process started: tests and
// timings tell an incremental evaluation's copies saved.
void countTaken();
std::size_t arraysTaken();

}  // namespace detail

// An array shared between copies of a StrandSet until one of them writes to
// it (copy on write). Reading never copies; the first mutable access after a
// copy clones the array, once, under a lock, so operators may take mutable
// pointers from inside parallel loops. Large arrays go back to a pool when the
// last copy lets go, for the next clone of the same size (detail::ArrayPool).
template <class T>
class CowArray {
public:
    CowArray() : data_(fresh()), owned_(true) {}
    CowArray(const CowArray& o) : data_(o.data_), owned_(false) { o.owned_.store(false, std::memory_order_release); }
    CowArray(CowArray&& o) noexcept : data_(std::move(o.data_)), owned_(o.owned_.load(std::memory_order_acquire)) {
        o.data_ = fresh();
        o.owned_.store(true, std::memory_order_release);
    }
    CowArray& operator=(const CowArray& o) {
        if (this != &o) {
            data_ = o.data_;
            owned_.store(false, std::memory_order_release);
            o.owned_.store(false, std::memory_order_release);
        }
        return *this;
    }
    CowArray& operator=(CowArray&& o) noexcept {
        if (this != &o) {
            data_ = std::move(o.data_);
            owned_.store(o.owned_.load(std::memory_order_acquire), std::memory_order_release);
            o.data_ = fresh();
            o.owned_.store(true, std::memory_order_release);
        }
        return *this;
    }

    const std::vector<T>& get() const { return *data_; }
    // The array for writing: cloned first when another set shares it. (A copy marks both sides
    // shared and nothing marks them owned again when the other side dies, so the array is this
    // one's own again, without a copy, when nobody else holds it any more.)
    std::vector<T>& mut(std::mutex& lock) {
        if (!owned_.load(std::memory_order_acquire)) {
            std::lock_guard<std::mutex> guard(lock);
            if (!owned_.load(std::memory_order_acquire) && !reclaim()) {
                const std::vector<T>& from = *data_;
                std::vector<T>* copy = detail::ArrayPool<T>::instance().take(from.size());
                if constexpr (std::is_trivially_copyable_v<T>) {
                    // (A large clone outside any parallel body: its pages in and its copy in
                    // parallel. Inside one, serially: this thread holds `lock`, which the loop's
                    // other bodies may take, and a thread waiting on a loop runs other bodies.)
                    if (!copy) copy = detail::freshArray<T>(from.size());
                    if (from.empty()) {
                    } else if (insideParallelBody()) {
                        std::copy(from.begin(), from.end(), copy->begin());
                    } else {
                        detail::copyBytesParallel(copy->data(), from.data(), from.size() * sizeof(T));
                    }
                } else if (copy) {
                    std::copy(from.begin(), from.end(), copy->begin());
                } else {
                    copy = new std::vector<T>(from);
                }
                data_ = std::shared_ptr<std::vector<T>>(copy, detail::ToArrayPool<T>{});
                owned_.store(true, std::memory_order_release);
            }
        }
        return *data_;
    }
    // The array for writing every element anew: when another set shares it, one of its size
    // (recycled, or fresh), its contents left as they come instead of copied.
    std::vector<T>& overwrite(std::mutex& lock) {
        if (!owned_.load(std::memory_order_acquire)) {
            std::lock_guard<std::mutex> guard(lock);
            if (!owned_.load(std::memory_order_acquire) && !reclaim()) {
                const std::size_t size = data_->size();
                std::vector<T>* array = detail::ArrayPool<T>::instance().take(size);
                if (!array) array = detail::freshArray<T>(size);
                data_ = std::shared_ptr<std::vector<T>>(array, detail::ToArrayPool<T>{});
                owned_.store(true, std::memory_order_release);
            }
        }
        return *data_;
    }
    // An array of `size` elements of this one's own, its contents as they come: one the pool kept
    // of that size (stale contents, its pages in; `recycled` true) or a fresh one (zeros, filled
    // as allocated). What was shared is not touched. For a caller that then fills it, in parallel
    // (StrandSet::resizeUniform), or writes every element itself.
    std::vector<T>& sized(std::size_t size, bool* recycled = nullptr) {
        std::vector<T>* array = detail::ArrayPool<T>::instance().take(size);
        if (recycled) *recycled = array != nullptr;
        if (!array) array = detail::freshArray<T>(size);
        data_ = std::shared_ptr<std::vector<T>>(array, detail::ToArrayPool<T>{});
        owned_.store(true, std::memory_order_release);
        return *data_;
    }
    // A copy of `from`'s array of its own (recycled when one of the size is kept), copied in
    // parallel; what was shared is not touched.
    std::vector<T>& copyOf(const CowArray& from) {
        const std::vector<T>& source = *from.data_;
        std::vector<T>* array = detail::ArrayPool<T>::instance().take(source.size());
        if (!array) array = detail::freshArray<T>(source.size());
        if (!source.empty()) detail::copyBytesParallel(array->data(), source.data(), source.size() * sizeof(T));
        data_ = std::shared_ptr<std::vector<T>>(array, detail::ToArrayPool<T>{});
        owned_.store(true, std::memory_order_release);
        return *data_;
    }
    // `from`'s array as this one's own: the very array when `from` is the only one holding it
    // (moved here; `from` is left empty), else a copy (copyOf).
    std::vector<T>& takeFrom(CowArray& from) {
        if (&from == this) {  // its own array already (moving it onto itself would then wipe it)
            if (data_.use_count() != 1) return copyOf(from);
            owned_.store(true, std::memory_order_release);
            return *data_;
        }
        if (from.data_.use_count() != 1) return copyOf(from);
        detail::countTaken();
        data_ = std::move(from.data_);
        owned_.store(true, std::memory_order_release);
        from.data_ = fresh();
        from.owned_.store(true, std::memory_order_release);
        return *data_;
    }
    // `tail` appended. An array of this one's own is grown in place (amortized, as insert); one
    // shared with another set (a copy about to grow: Merge's) is not cloned and then grown (two
    // copies and a reallocation) but made once at its new size (recycled when one is kept) and
    // both parts copied into it in parallel. What was shared is not touched.
    std::vector<T>& appended(const std::vector<T>& tail, std::mutex& lock) {
        const std::size_t head = data_->size(), total = head + tail.size();
        if (tail.empty() || owned_.load(std::memory_order_acquire) || data_.use_count() == 1 ||
            total * sizeof(T) < (std::size_t{1} << 20)) {
            std::vector<T>& own = mut(lock);
            own.insert(own.end(), tail.begin(), tail.end());
            return own;
        }
        const std::vector<T>& before = *data_;
        std::vector<T>* array = detail::ArrayPool<T>::instance().take(total);
        if (!array) array = detail::freshArray<T>(total);
        if (head) detail::copyBytesParallel(array->data(), before.data(), head * sizeof(T));
        detail::copyBytesParallel(array->data() + head, tail.data(), tail.size() * sizeof(T));
        data_ = std::shared_ptr<std::vector<T>>(array, detail::ToArrayPool<T>{});
        owned_.store(true, std::memory_order_release);
        return *data_;
    }
    // Replaces the contents with a fresh array (nothing shared is touched).
    std::vector<T>& reset() {
        data_ = fresh();
        owned_.store(true, std::memory_order_release);
        return *data_;
    }

private:
    static std::shared_ptr<std::vector<T>> fresh() {
        return std::shared_ptr<std::vector<T>>(new std::vector<T>(), detail::ToArrayPool<T>{});
    }
    // Under the clone lock, marked shared: owned again when no other holder is left (the copies
    // that shared it died), so the array is written in place; false when one still shares it.
    bool reclaim() {
        if (data_.use_count() != 1) return false;
        owned_.store(true, std::memory_order_release);
        return true;
    }

    std::shared_ptr<std::vector<T>> data_;
    mutable std::atomic<bool> owned_;
};

// Group numbers (the clumpId, braidId and featherId channels, FeatherLayer::featherIds): a
// group's number comes from what names it (its center's, path's or shaft's ID), below
// kGroupNumbers, so that an edit of the scalp in one place renumbers no group elsewhere. Sets
// joined after another (Merge, Mirror's copy, a second Braid, the next feather layer) number
// their groups from groupNumberBase of the earlier highest: the next multiple of kGroupNumbers,
// which which groups there are never moves. Whole floats hold four such blocks exactly.
constexpr std::int64_t kGroupNumbers = std::int64_t{1} << 22;
constexpr std::int64_t groupNumberBase(std::int64_t highest) {
    return highest < 0 ? 0 : (highest / kGroupNumbers + 1) * kGroupNumbers;
}

// Structure-of-arrays storage for a set of strands (curves).
//
// Strand i owns points [offsets[i], offsets[i + 1]). Per-strand arrays have
// strandCount() entries; per-point arrays have pointCount() entries. Named
// float channels hold extra attributes (e.g. "melanin", "clumpId").
//
// Copies share every array until written (CowArray): an operator that makes
// a copy of its input and reshapes the points shares the IDs, roots, offsets,
// widths and channels with it, and a node writing one channel shares the
// points. Const accessors never copy; the mutable* accessors (and the
// non-const strandPoints) clone the array on first use, once.
class StrandSet {
public:
    StrandSet();
    StrandSet(const StrandSet& o);
    StrandSet(StrandSet&& o) noexcept;
    StrandSet& operator=(const StrandSet& o);
    StrandSet& operator=(StrandSet&& o) noexcept;

    std::size_t strandCount() const { return ids_.get().size(); }
    std::size_t pointCount() const { return positions_.get().size(); }
    bool empty() const { return ids_.get().empty(); }

    void reserve(std::size_t strands, std::size_t points);
    void clear();

    // What resizeUniform and resize leave as it comes, for a caller that writes every element
    // of the array itself (Scatter every point): a recycled array's stale contents, or zeros.
    enum ResizeLeave : unsigned { kFillAll = 0, kLeavePositions = 1, kLeaveWidths = 2 };
    // Replaces the contents with `strands` strands of `pointsPerStrand` points
    // each (IDs 0, roots unset, positions zero, widths 1, channels reset to
    // defaults). Operators then fill arrays in parallel. The large arrays come from the pool
    // (detail::ArrayPool, their pages in) and are filled in parallel, in fixed blocks, except
    // what `leave` names.
    void resizeUniform(std::size_t strands, std::size_t pointsPerStrand, unsigned leave = kFillAll);
    // Replaces the contents with `strands` strands of the given point counts
    // (the same defaults as resizeUniform, `leave` the same).
    void resize(const std::vector<std::size_t>& pointsPerStrand, unsigned leave = kFillAll);

    // Point count shared by every strand; 0 if counts differ or the set is empty.
    std::size_t uniformPointCount() const;
    // The most points any strand has (0 when empty).
    std::size_t maxPointCount() const;

    // Appends a strand and returns its index. widths may be empty (defaults
    // to 1.0 for each point) or must match points in size.
    std::size_t addStrand(StrandId id, const std::vector<Vec3f>& points,
                          const std::vector<float>& widths = {}, StrandRoot root = {});

    std::size_t strandPointCount(std::size_t strand) const {
        const auto& o = offsets_.get();
        return o[strand + 1] - o[strand];
    }
    std::size_t strandBegin(std::size_t strand) const { return offsets_.get()[strand]; }
    // Points of a strand for writing (the positions are cloned first when shared).
    Vec3f* strandPoints(std::size_t strand) {
        dropExtent();
        return positions_.mut(lock_).data() + offsets_.get()[strand];
    }
    const Vec3f* strandPoints(std::size_t strand) const { return positions_.get().data() + offsets_.get()[strand]; }

    const std::vector<StrandId>& ids() const { return ids_.get(); }
    const std::vector<StrandRoot>& roots() const { return roots_.get(); }
    const std::vector<std::size_t>& offsets() const { return offsets_.get(); }
    const std::vector<Vec3f>& positions() const { return positions_.get(); }
    const std::vector<float>& widths() const { return widths_.get(); }

    // For writing: each clones its array on first use when a copy shares it.
    std::vector<StrandId>& mutableIds() { return ids_.mut(lock_); }
    std::vector<StrandRoot>& mutableRoots() { return roots_.mut(lock_); }
    std::vector<Vec3f>& mutablePositions() {
        dropExtent();
        return positions_.mut(lock_);
    }
    // The positions for writing every point anew: not copied from a set sharing them, so each
    // point must be written (one left out holds whatever the array held).
    std::vector<Vec3f>& overwritePositions() {
        dropExtent();
        return positions_.overwrite(lock_);
    }
    std::vector<float>& mutableWidths() { return widths_.mut(lock_); }

    // Per-strand and per-point float channels. Adding a channel fills it with
    // defaultValue (a fresh array, shared with nobody); strands appended later
    // extend it with defaultValue too.
    std::vector<float>& addStrandChannel(const std::string& name, float defaultValue = 0.0f);
    std::vector<float>& addPointChannel(const std::string& name, float defaultValue = 0.0f);
    const std::vector<float>* strandChannel(const std::string& name) const;
    const std::vector<float>* pointChannel(const std::string& name) const;
    // The value a channel was added with (0 when absent): what strands appended
    // without it take, so a node rebuilding a set passes it on.
    float strandChannelDefault(const std::string& name) const;
    float pointChannelDefault(const std::string& name) const;
    // An existing channel for writing (cloned first when shared); null if absent.
    std::vector<float>* mutableStrandChannel(const std::string& name);
    std::vector<float>* mutablePointChannel(const std::string& name);
    // For an incremental evaluation (Operator::evaluateIncremental): an array made a copy of
    // another set's (the operator's last result, holding the same strands), this set's own, to
    // write some strands of it again. Copied in parallel; nothing shared is touched. A channel is
    // added with `from`'s default when this set has none; null when `from` has no such channel or
    // one of another length.
    std::vector<Vec3f>& copyPositionsFrom(const StrandSet& from);
    std::vector<float>& copyWidthsFrom(const StrandSet& from);
    // The same, taking `from`'s very array when `from` alone holds it (no other set shares it):
    // moved here, no copy, `from` left without points (or widths). For a set nothing will read
    // again: an operator's last result the graph let go of (IncrementalInputs::sole).
    std::vector<Vec3f>& takePositionsFrom(StrandSet& from);
    std::vector<float>& takeWidthsFrom(StrandSet& from);
    // `from`'s points as this set's, shared with it (no copy; copy on write as ever), with its
    // extent: for a set holding the same strands as `from` (IDs, roots, offsets), whose points
    // are `from`'s (StrandChanges::pointsKept).
    void sharePositionsOf(const StrandSet& from);
    // The widths for writing every point anew (as overwritePositions): not copied from a set
    // sharing them, so each must be written.
    std::vector<float>& overwriteWidths() { return widths_.overwrite(lock_); }
    std::vector<float>* copyStrandChannelFrom(const std::string& name, const StrandSet& from);
    std::vector<float>* copyPointChannelFrom(const std::string& name, const StrandSet& from);
    void removeStrandChannel(const std::string& name) { strandChannels_.erase(name); }
    void removePointChannel(const std::string& name) { pointChannels_.erase(name); }
    std::vector<std::string> strandChannelNames() const;
    std::vector<std::string> pointChannelNames() const;

    // Geometry instanced on the strands (Instance operator); null when none.
    // Copies of the set share it, so nodes downstream carry it along.
    const std::shared_ptr<const InstanceSet>& instances() const { return instances_; }
    void setInstances(std::shared_ptr<const InstanceSet> instances) { instances_ = std::move(instances); }

    // Game cards made by Cards nodes upstream (cards only; the host lays them
    // out). Copies share them, so they ride along to the output.
    const std::vector<std::shared_ptr<const CardMesh>>& cardLayers() const { return cardLayers_; }
    void addCardLayer(std::shared_ptr<const CardMesh> layer) { cardLayers_.push_back(std::move(layer)); }
    void setCardLayers(std::vector<std::shared_ptr<const CardMesh>> layers) { cardLayers_ = std::move(layers); }

    // What Propagate nodes upstream made besides strands (feathers.h: groom groups, rachis
    // tubes). Copies share them, so they ride along to the output as card layers do.
    const std::vector<std::shared_ptr<const FeatherLayer>>& featherLayers() const { return featherLayers_; }
    void addFeatherLayer(std::shared_ptr<const FeatherLayer> layer) { featherLayers_.push_back(std::move(layer)); }
    void setFeatherLayers(std::vector<std::shared_ptr<const FeatherLayer>> layers) { featherLayers_ = std::move(layers); }

    // Appends every strand of `other`: points, widths, IDs, roots and channels.
    // A channel only one of the sets has is kept, with its default on the
    // other set's strands; except that a point channel only one set has takes,
    // on the other set's points, that set's strand channel of the same name
    // (each strand's value along it) when it has one. So HairColor's per-point
    // colorR/G/B (a tip blend) merged with strands that have only per-strand
    // colorR/G/B gives those strands their own color, not the default.
    void append(const StrandSet& other);

    // Returns the subset of strands kept at the given preview density (PG-4).
    StrandSet withDensity(float density) const;
    // The given strands (indices, in the order given), with their channels.
    StrandSet subset(const std::vector<std::size_t>& strands) const;

    // Checks array sizes and offsets are consistent. On failure returns false
    // and, if error is non-null, describes the first problem found.
    bool validate(std::string* error = nullptr) const;

    // Order-sensitive content hash, used for cache keys and golden tests.
    std::uint64_t contentHash() const;

    // The points' bounds and the longest strand's length (each strand's segment lengths summed
    // root to tip, in order), worked out in one parallel pass on first use and kept until the set
    // is written (resizing, appending, a strand added, its points or positions taken for
    // writing). Ask a complete set (an evaluation's result): one still being filled gives what
    // it holds when asked. Copies keep it.
    struct Extent {
        Vec3f lo, hi;               // lo above hi on every axis when there are no points
        float longestStrand = 0.0f;
        bool finite = true;         // every point a finite number (no NaN, no infinity), which lo and hi leave out
        bool empty() const { return lo.x > hi.x; }
    };
    Extent extent() const;
    // Each array it holds: its elements' address (one array shared between sets has the same in all
    // of them) and its bytes. What a cache of sets holds counts each address once (Graph::setCacheLimit).
    void forEachArray(const std::function<void(const void*, std::size_t)>& visit) const;
    // extent() of a set holding the strands `previous` holds, where only `changed` strands'
    // points differ (StrandChanges): the blocks of strands none of which changed take previous'
    // parts (when it worked its extent out), the others are measured again. The same values
    // extent() gives.
    Extent extentAfter(const StrandSet& previous, const std::vector<std::uint32_t>& changed) const;

private:
    // extent()'s value, while the points and offsets it came from stand.
    void dropExtent() {
        if (hasExtent_.load(std::memory_order_acquire)) {
            std::lock_guard<std::mutex> guard(extentLock_);
            extent_.reset();
            extentBlocks_.reset();
            hasExtent_.store(false, std::memory_order_release);
        }
    }
    void copyExtent(const StrandSet& o);
    mutable std::mutex extentLock_;
    mutable std::shared_ptr<const Extent> extent_;
    mutable std::shared_ptr<const std::vector<Extent>> extentBlocks_;  // its parts, block by block
    mutable std::atomic<bool> hasExtent_{false};

    struct Channel {
        CowArray<float> values;
        float defaultValue = 0.0f;
    };

    CowArray<StrandId> ids_;
    CowArray<StrandRoot> roots_;
    CowArray<std::size_t> offsets_;
    CowArray<Vec3f> positions_;
    CowArray<float> widths_;
    std::map<std::string, Channel> strandChannels_;
    std::map<std::string, Channel> pointChannels_;
    std::shared_ptr<const InstanceSet> instances_;
    std::vector<std::shared_ptr<const CardMesh>> cardLayers_;
    std::vector<std::shared_ptr<const FeatherLayer>> featherLayers_;
    std::mutex lock_;  // guards the clone-on-write of every array above
};

}  // namespace groomlab
