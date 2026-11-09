// SPDX-License-Identifier: Apache-2.0
#include "shared_memory.h"

#include <atomic>
#include <cerrno>
#include <cstring>
#include <stdexcept>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace groomlab::helper {

namespace {

#ifdef _WIN32
std::wstring wide(const std::string& text) {
    if (text.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring out(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), n);
    return out;
}

std::string lastError() {
    return "Windows error " + std::to_string(static_cast<unsigned long>(GetLastError()));
}
#else
std::string posixName(const std::string& name) { return name.empty() || name[0] != '/' ? "/" + name : name; }
#endif

bool validName(const std::string& name) {
    if (name.empty() || name.size() > 200) return false;
    for (const char c : name) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' ||
                        c == '-' || c == '.' || c == '/';
        if (!ok) return false;
    }
    return true;
}

}  // namespace

std::unique_ptr<SharedBlock> SharedBlock::create(const std::string& name, std::size_t size) {
    if (!validName(name)) throw std::runtime_error("bad shared memory name '" + name + "'");
    if (size == 0) size = 1;
    std::unique_ptr<SharedBlock> block(new SharedBlock());
    block->name_ = name;
    block->size_ = size;
    block->owner_ = true;
#ifdef _WIN32
    const auto big = static_cast<unsigned long long>(size);
    HANDLE mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, static_cast<DWORD>(big >> 32),
                                        static_cast<DWORD>(big & 0xFFFFFFFFull), wide(name).c_str());
    if (!mapping) throw std::runtime_error("cannot make shared memory '" + name + "': " + lastError());
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(mapping);
        throw std::runtime_error("shared memory '" + name + "' exists already");
    }
    void* view = MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, size);
    if (!view) {
        const std::string why = lastError();
        CloseHandle(mapping);
        throw std::runtime_error("cannot map shared memory '" + name + "': " + why);
    }
    block->mapping_ = mapping;
    block->view_ = view;
#else
    const std::string path = posixName(name);
    const int fd = shm_open(path.c_str(), O_CREAT | O_EXCL | O_RDWR, 0600);
    if (fd < 0) throw std::runtime_error("cannot make shared memory '" + name + "': " + std::strerror(errno));
    if (ftruncate(fd, static_cast<off_t>(size)) != 0) {
        const std::string why = std::strerror(errno);
        close(fd);
        shm_unlink(path.c_str());
        throw std::runtime_error("cannot size shared memory '" + name + "': " + why);
    }
    void* view = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (view == MAP_FAILED) {
        shm_unlink(path.c_str());
        throw std::runtime_error("cannot map shared memory '" + name + "': " + std::strerror(errno));
    }
    block->view_ = view;
#endif
    return block;
}

std::unique_ptr<SharedBlock> SharedBlock::open(const std::string& name) {
    if (!validName(name)) throw std::runtime_error("bad shared memory name '" + name + "'");
    std::unique_ptr<SharedBlock> block(new SharedBlock());
    block->name_ = name;
#ifdef _WIN32
    HANDLE mapping = OpenFileMappingW(FILE_MAP_READ, FALSE, wide(name).c_str());
    if (!mapping) throw std::runtime_error("no shared memory named '" + name + "' (" + lastError() + ")");
    void* view = MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0);
    if (!view) {
        const std::string why = lastError();
        CloseHandle(mapping);
        throw std::runtime_error("cannot map shared memory '" + name + "': " + why);
    }
    MEMORY_BASIC_INFORMATION info{};
    VirtualQuery(view, &info, sizeof info);
    block->mapping_ = mapping;
    block->view_ = view;
    block->size_ = info.RegionSize;
#else
    const std::string path = posixName(name);
    const int fd = shm_open(path.c_str(), O_RDONLY, 0);
    if (fd < 0) throw std::runtime_error("no shared memory named '" + name + "' (" + std::strerror(errno) + ")");
    struct stat st {};
    if (fstat(fd, &st) != 0 || st.st_size <= 0) {
        close(fd);
        throw std::runtime_error("shared memory '" + name + "' is empty");
    }
    void* view = mmap(nullptr, static_cast<std::size_t>(st.st_size), PROT_READ, MAP_SHARED, fd, 0);
    close(fd);
    if (view == MAP_FAILED) throw std::runtime_error("cannot map shared memory '" + name + "': " + std::strerror(errno));
    block->view_ = view;
    block->size_ = static_cast<std::size_t>(st.st_size);
#endif
    return block;
}

SharedBlock::~SharedBlock() {
#ifdef _WIN32
    if (view_) UnmapViewOfFile(view_);
    if (mapping_) CloseHandle(mapping_);
#else
    if (view_) munmap(view_, size_);
    if (owner_) shm_unlink(posixName(name_).c_str());
#endif
}

const char* blockNamePrefix() {
#ifdef __APPLE__
    // macOS caps a POSIX shared memory name at 31 bytes with its slash (PSHMNAMLEN): "/gl_<pid>_..."
    // leaves a long session's numbers room ("/gl_99999_s12overlay_100000": 27).
    return "gl_";
#else
    return "groomlab_";
#endif
}

std::string blockName(const std::string& purpose, unsigned n) {
#ifdef _WIN32
    const unsigned long pid = GetCurrentProcessId();
#else
    const unsigned long pid = static_cast<unsigned long>(getpid());
#endif
    return blockNamePrefix() + std::to_string(pid) + "_" + purpose + "_" + std::to_string(n);
}

void Arena::begin(std::size_t bytes) {
    used_ = 0;
    if (block_ && block_->size() >= bytes) return;
    // Room for this answer and half again (a groom grows while it is groomed): fewer new names.
    std::size_t size = bytes + bytes / 2;
    size = (size + 0xFFFFF) & ~static_cast<std::size_t>(0xFFFFF);  // whole MiB
    block_.reset();
    // Numbered across the process, not per arena: an arena made again for the same purpose (a
    // session's tools reset when a groom file is loaded into it) would otherwise name its first
    // block as the old one did, which the client still holds open (Windows keeps the name while it
    // does), and the answer would fail.
    static std::atomic<unsigned> made{0};
    block_ = SharedBlock::create(blockName(prefix_, ++made), size);
}

void* Arena::add(std::size_t count, std::size_t itemSize, const std::string& dtype, std::vector<std::size_t> shape,
                 ArrayRef* ref) {
    const std::size_t offset = (used_ + 63) & ~static_cast<std::size_t>(63);
    const std::size_t bytes = count * itemSize;
    if (!block_ || offset + bytes > block_->size()) throw std::logic_error("an answer outgrew its shared memory");
    used_ = offset + bytes;
    if (ref) {
        ref->block = block_->name();
        ref->offset = offset;
        ref->dtype = dtype;
        ref->shape = std::move(shape);
    }
    return block_->data() + offset;
}

}  // namespace groomlab::helper
