// SPDX-License-Identifier: Apache-2.0
#pragma once

// Named shared memory between the helper and its client (the Blender add-on), for the big arrays
// the JSON lines only point at (docs/BLENDER.md, "Shared memory"). Windows: a named file mapping
// in the session's namespace (CreateFileMappingW / OpenFileMappingW, backed by the paging file);
// elsewhere POSIX shm (shm_open + mmap). Either opens from Python with
// multiprocessing.shared_memory.SharedMemory(name) (track=False on POSIX), so the add-on reads an
// array as numpy.ndarray(shape, dtype, buffer=shm.buf, offset=offset) without a copy.
//
// The helper creates the blocks it answers with (Arena: one per purpose, grown when an answer
// does not fit, a new name each time) and opens, read-only, the blocks a client names in a
// request (its own SharedMemory(create=True)).

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace groomlab::helper {

class SharedBlock {
public:
    // A new block of `size` bytes (zeroed) named `name` (no leading slash: it is added on POSIX).
    // Throws std::runtime_error.
    static std::unique_ptr<SharedBlock> create(const std::string& name, std::size_t size);
    // A client's block, read-only, mapped whole. Throws std::runtime_error when there is none.
    static std::unique_ptr<SharedBlock> open(const std::string& name);
    ~SharedBlock();
    SharedBlock(const SharedBlock&) = delete;
    SharedBlock& operator=(const SharedBlock&) = delete;

    const std::string& name() const { return name_; }
    std::size_t size() const { return size_; }
    std::uint8_t* data() { return static_cast<std::uint8_t*>(view_); }
    const std::uint8_t* data() const { return static_cast<const std::uint8_t*>(view_); }

private:
    SharedBlock() = default;
    std::string name_;
    std::size_t size_ = 0;
    void* view_ = nullptr;
    bool owner_ = false;  // created here: POSIX unlinks the name when it goes
#ifdef _WIN32
    void* mapping_ = nullptr;
#endif
};

// An array as a response describes it: where it lies and how to read it.
struct ArrayRef {
    std::string block;
    std::size_t offset = 0;
    std::string dtype;  // numpy's: "<f4", "<i4", "<u4", "<u8", "<i8"
    std::vector<std::size_t> shape;
};

// The blocks one purpose answers in (a session's strands, its guides...): every answer is laid out
// from the start of the current block, 64-byte aligned, so the arrays of the last answer are valid
// until the next answer of the same purpose. A block that is too small is replaced by a bigger one
// with a new name (numbered across the process: never one a client may still hold); the old one
// lives on for a client still holding it (Windows: until it closes it; POSIX: the name is
// unlinked, the client's mapping stays).
class Arena {
public:
    explicit Arena(std::string prefix) : prefix_(std::move(prefix)) {}
    // Starts an answer needing `bytes` in all (the sum of the arrays' sizes plus alignment:
    // `reserve` adds 64 per array).
    void begin(std::size_t bytes);
    // Room for `count` elements of `itemSize` bytes; the pointer stays valid until the next begin.
    void* add(std::size_t count, std::size_t itemSize, const std::string& dtype, std::vector<std::size_t> shape, ArrayRef* ref);
    static std::size_t reserve(std::size_t bytes) { return bytes + 64; }
    const SharedBlock* block() const { return block_.get(); }

private:
    std::string prefix_;
    std::unique_ptr<SharedBlock> block_;
    std::size_t used_ = 0;
};

// A unique block name for this process: "<blockNamePrefix()><pid>_<purpose>_<n>". The helper's
// temp folders are named the same way.
std::string blockName(const std::string& purpose, unsigned n);

// "groomlab_" ("gl_" on macOS, whose shared memory names are short).
const char* blockNamePrefix();

}  // namespace groomlab::helper
