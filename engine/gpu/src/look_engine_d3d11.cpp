// SPDX-License-Identifier: Apache-2.0
// The look engine on DirectX 11 (look_engine.h): the kernels bound by their DirectX registers
// (look.slang: t0-t8 in, u0-u2 out, b0 the constants).

#include <algorithm>
#include <cstring>
#include <map>
#include <stdexcept>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <d3d11.h>

#include "groomlab/look/look_engine_internal.h"

// The kernels as DXBC (cs_5_0), compiled from shaders/look.slang by slangc when the module builds
// (engine/gpu/CMakeLists.txt): kLookDxbcNoiseCS, kLookDxbcCurlCS...
#include "look_kernels_dxbc.h"

namespace groomlab::gpu {

namespace {

// A COM pointer that releases what it holds (plain d3d11.h, no WRL).
template <class T>
class Com {
public:
    Com() = default;
    ~Com() { reset(); }
    Com(const Com&) = delete;
    Com& operator=(const Com&) = delete;
    Com(Com&& o) noexcept : p_(o.p_) { o.p_ = nullptr; }
    Com& operator=(Com&& o) noexcept {
        if (this != &o) {
            reset();
            p_ = o.p_;
            o.p_ = nullptr;
        }
        return *this;
    }
    T* get() const { return p_; }
    T* operator->() const { return p_; }
    T** put() {
        reset();
        return &p_;
    }
    void reset() {
        if (p_) p_->Release();
        p_ = nullptr;
    }
    void adopt(T* p) {  // a reference of the caller's: one more taken
        if (p) p->AddRef();
        reset();
        p_ = p;
    }
    explicit operator bool() const { return p_ != nullptr; }

private:
    T* p_ = nullptr;
};

void check(HRESULT hr, const char* what) {
    if (FAILED(hr)) {
        char text[160];
        std::snprintf(text, sizeof text, "GPU preview: %s failed (0x%08lx)", what, static_cast<unsigned long>(hr));
        throw std::runtime_error(text);
    }
}

// A raw (byte address) buffer with its views: the engine's own, or a target the host gave.
struct RawBuffer {
    Com<ID3D11Buffer> buffer;
    Com<ID3D11ShaderResourceView> srv;
    Com<ID3D11UnorderedAccessView> uav;
    std::size_t bytes = 0;
    bool writable = false;

    void reset() {
        srv.reset();
        uav.reset();
        buffer.reset();
        bytes = 0;
        writable = false;
    }
    void views(ID3D11Device* device, bool readable, bool canWrite) {
        D3D11_BUFFER_DESC desc{};
        buffer->GetDesc(&desc);
        bytes = desc.ByteWidth;
        writable = canWrite;
        if (readable) {
            D3D11_SHADER_RESOURCE_VIEW_DESC s{};
            s.Format = DXGI_FORMAT_R32_TYPELESS;
            s.ViewDimension = D3D11_SRV_DIMENSION_BUFFEREX;
            s.BufferEx.NumElements = static_cast<UINT>(bytes / 4);
            s.BufferEx.Flags = D3D11_BUFFEREX_SRV_FLAG_RAW;
            check(device->CreateShaderResourceView(buffer.get(), &s, srv.put()), "CreateShaderResourceView");
        }
        if (canWrite) {
            D3D11_UNORDERED_ACCESS_VIEW_DESC u{};
            u.Format = DXGI_FORMAT_R32_TYPELESS;
            u.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
            u.Buffer.NumElements = static_cast<UINT>(bytes / 4);
            u.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
            check(device->CreateUnorderedAccessView(buffer.get(), &u, uav.put()), "CreateUnorderedAccessView");
        }
    }
    void create(ID3D11Device* device, std::size_t size, bool canWrite) {
        size = std::max<std::size_t>((size + 3) & ~std::size_t{3}, 4);
        if (bytes == size && buffer && writable == canWrite) return;
        reset();
        D3D11_BUFFER_DESC desc{};
        desc.ByteWidth = static_cast<UINT>(size);
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | (canWrite ? D3D11_BIND_UNORDERED_ACCESS : 0);
        desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
        check(device->CreateBuffer(&desc, nullptr, buffer.put()), "CreateBuffer");
        views(device, true, canWrite);
    }
    // A host's buffer the kernels write in place (and read: positions feed the frames).
    void adopt(ID3D11Device* device, ID3D11Buffer* target) {
        if (buffer.get() == target) return;
        reset();
        buffer.adopt(target);
        views(device, true, true);
    }
};

// Whether the kernels can write `b` in place: made for it (createD3D11DisplayBuffer) and long
// enough for `bytes`.
bool writableInPlace(ID3D11Buffer* b, std::size_t bytes) {
    if (!b) return false;
    D3D11_BUFFER_DESC d{};
    b->GetDesc(&d);
    const UINT need = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;
    return (d.BindFlags & need) == need && (d.MiscFlags & D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS) && d.ByteWidth >= bytes &&
           d.Usage == D3D11_USAGE_DEFAULT;
}

constexpr UINT kSrvs = 9, kUavs = 4;  // u0-u2 the buffers, u3 the app's point texture

// The compute state a run touches, kept and put back (Maya's renderer owns the context).
struct ComputeState {
    ID3D11ComputeShader* shader = nullptr;
    ID3D11Buffer* constants = nullptr;
    ID3D11ShaderResourceView* srvs[kSrvs] = {};
    ID3D11UnorderedAccessView* uavs[kUavs] = {};

    void save(ID3D11DeviceContext* ctx) {
        ctx->CSGetShader(&shader, nullptr, nullptr);
        ctx->CSGetConstantBuffers(0, 1, &constants);
        ctx->CSGetShaderResources(0, kSrvs, srvs);
        ctx->CSGetUnorderedAccessViews(0, kUavs, uavs);
    }
    void restore(ID3D11DeviceContext* ctx) {
        ctx->CSSetShader(shader, nullptr, 0);
        ctx->CSSetConstantBuffers(0, 1, &constants);
        ID3D11UnorderedAccessView* none[kUavs] = {};
        ctx->CSSetUnorderedAccessViews(0, kUavs, none, nullptr);  // before the SRVs: a buffer may be both
        ctx->CSSetShaderResources(0, kSrvs, srvs);
        ctx->CSSetUnorderedAccessViews(0, kUavs, uavs, nullptr);
        if (shader) shader->Release();
        if (constants) constants->Release();
        for (auto*& v : srvs) {
            if (v) v->Release();
            v = nullptr;
        }
        for (auto*& v : uavs) {
            if (v) v->Release();
            v = nullptr;
        }
        shader = nullptr;
        constants = nullptr;
    }
};

class D3D11LookEngine final : public LookEngine {
public:
    explicit D3D11LookEngine(ID3D11Device* d) : device_(d) {
        if (!device_) throw std::runtime_error("GPU preview: no device");
        device_->GetImmediateContext(context_.put());
        struct Code {
            Kernel kernel;
            const unsigned char* code;
            std::size_t size;
            const char* entry;
        };
        const Code kernels[] = {
#define GROOMLAB_KERNEL(Name) {Kernel::Name, kLookDxbc##Name##CS, sizeof kLookDxbc##Name##CS, #Name "CS"},
#include "groomlab/look/look_kernels.inc"
#undef GROOMLAB_KERNEL
        };
        for (const Code& k : kernels) {
            check(device_->CreateComputeShader(k.code, k.size, nullptr, shaders_[static_cast<int>(k.kernel)].put()), k.entry);
        }
        makeQueries();
    }
    // A sibling (LookEngine::sibling): the same device and kernels, buffers and queries of its own.
    D3D11LookEngine(const D3D11LookEngine& parent, int) : device_(parent.device_) {
        device_->GetImmediateContext(context_.put());
        for (int k = 0; k < static_cast<int>(Kernel::Count); ++k) shaders_[k].adopt(parent.shaders_[k].get());
        makeQueries();
    }
    void makeQueries() {
        D3D11_QUERY_DESC q{};
        q.Query = D3D11_QUERY_TIMESTAMP_DISJOINT;
        check(device_->CreateQuery(&q, disjoint_.put()), "CreateQuery");
        q.Query = D3D11_QUERY_TIMESTAMP;
        for (auto& s : stamps_) check(device_->CreateQuery(&q, s.put()), "CreateQuery");
    }

    GpuApi api() const override { return GpuApi::D3D11; }
    void* device() const override { return device_; }

    std::unique_ptr<LookEngine> sibling() override { return std::make_unique<D3D11LookEngine>(*this, 0); }
    void borrow(int id, LookEngine& from, int fromId) override {
        auto& other = dynamic_cast<D3D11LookEngine&>(from);
        if (!other.has(fromId)) throw std::runtime_error("GPU preview: a sibling's buffer to borrow is missing");
        const RawBuffer& b = other.buffer(fromId);
        RawBuffer& to = buffers_[id];
        to.buffer.adopt(b.buffer.get());
        to.srv.adopt(b.srv.get());
        to.uav.adopt(b.uav.get());
        to.bytes = b.bytes;
        to.writable = b.writable;
    }

    std::size_t videoBytes() const override {
        std::size_t n = constants_.size() * sizeof(StageConstants);
        for (const auto& [id, b] : buffers_) n += b.bytes;
        return n;
    }

    void dropTargets() override {
        for (RawBuffer& t : targets_) t.reset();
        uvs_.reset();
        uvsBound_ = false;
        for (int i = 0; i < 4; ++i) {
            if (out_[i] == &targets_[i]) out_[i] = nullptr;
        }
        textureUav_.reset();
        texture_.reset();
    }

    void finish() override {
        D3D11_QUERY_DESC q{};
        q.Query = D3D11_QUERY_EVENT;
        Com<ID3D11Query> event;
        check(device_->CreateQuery(&q, event.put()), "CreateQuery (event)");
        context_->End(event.get());
        BOOL done = FALSE;
        while (context_->GetData(event.get(), &done, sizeof done, 0) != S_OK || !done) {
        }
    }

protected:
    void write(int id, const void* data, std::size_t bytes, bool writable) override {
        RawBuffer& b = buffers_[id];
        b.create(device_, bytes, writable);
        if (!bytes) return;
        const D3D11_BOX box{0, 0, 0, static_cast<UINT>(bytes), 1, 1};
        context_->UpdateSubresource(b.buffer.get(), 0, &box, data, 0, 0);
    }
    void writeRange(int id, std::size_t offset, const void* data, std::size_t bytes) override {
        auto it = buffers_.find(id);
        if (it == buffers_.end() || !bytes) return;
        const D3D11_BOX box{static_cast<UINT>(offset), 0, 0, static_cast<UINT>(offset + bytes), 1, 1};
        context_->UpdateSubresource(it->second.buffer.get(), 0, &box, data, 0, 0);
    }
    void allocate(int id, std::size_t bytes) override { buffers_[id].create(device_, bytes, true); }
    bool has(int id) const override {
        if (id >= kOut0 && id <= kOut3) return out_[id - kOut0] != nullptr;
        if (id == kOutUvs) return uvsBound_;
        return buffers_.count(id) != 0;
    }
    void release(int id) override {
        if (id >= kOut0 && id <= kOut3) {
            out_[id - kOut0] = nullptr;
            targets_[id - kOut0].reset();
        }
        buffers_.erase(id);
    }

    bool bindTargets(const LookTargets& targets, bool frames, std::size_t bytes, std::size_t uvBytes) override {
        // The lines' uvs: written in place or not at all (a copy would need the host's u too).
        auto* uvs = static_cast<ID3D11Buffer*>(targets.uvs);
        uvsBound_ = writableInPlace(uvs, uvBytes);
        if (uvsBound_) {
            uvs_.adopt(device_, uvs);
        } else {
            uvs_.reset();
        }
        // The app's point texture: an unordered access view on it (kept while the host hands the
        // same texture), checked to be the RGBA32F it reads back through its vertex shader.
        auto* texture = static_cast<ID3D11Texture2D*>(targets.pointTexture);
        if (texture != texture_.get()) {
            textureUav_.reset();
            texture_.reset();
            if (texture) {
                D3D11_TEXTURE2D_DESC d{};
                texture->GetDesc(&d);
                if (!(d.BindFlags & D3D11_BIND_UNORDERED_ACCESS) || d.Format != DXGI_FORMAT_R32G32B32A32_FLOAT) {
                    throw std::runtime_error("GPU preview: the point texture is not an RGBA32F texture compute may write");
                }
                if (d.Width != targets.textureWidth) throw std::runtime_error("GPU preview: the point texture is not as wide as said");
                D3D11_UNORDERED_ACCESS_VIEW_DESC u{};
                u.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
                u.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
                u.Texture2D.MipSlice = 0;
                check(device_->CreateUnorderedAccessView(texture, &u, textureUav_.put()), "CreateUnorderedAccessView (texture)");
                texture_.adopt(texture);
            }
        }
        void* const want[4] = {targets.positions, targets.normals, targets.tangents, targets.bitangents};
        bool inPlace = targets.positions != nullptr;
        for (int i = 0; i < 4; ++i) {
            want_[i] = nullptr;
            if (i > 0 && !frames) {
                out_[i] = nullptr;
                continue;
            }
            auto* target = static_cast<ID3D11Buffer*>(want[i]);
            if (writableInPlace(target, bytes)) {
                targets_[i].adopt(device_, target);
                out_[i] = &targets_[i];
            } else {
                // The engine's own (copied into the target after, where there is one).
                targets_[i].reset();
                RawBuffer& own = buffers_[kOut0 + i];
                own.create(device_, bytes, true);
                out_[i] = &own;
                if (target) {
                    want_[i] = target;
                    inPlace = false;
                }
            }
        }
        return inPlace;
    }
    void copyToTargets(bool frames, std::size_t bytes) override {
        const D3D11_BOX box{0, 0, 0, static_cast<UINT>(bytes), 1, 1};
        for (int i = 0; i < (frames ? 4 : 1); ++i) {
            if (want_[i] && out_[i] && bytes) context_->CopySubresourceRegion(want_[i], 0, 0, 0, 0, out_[i]->buffer.get(), 0, &box);
        }
    }
    void copy(int from, int to, std::size_t bytes, std::size_t fromOffset, std::size_t toOffset) override {
        if (!bytes) return;
        const D3D11_BOX box{static_cast<UINT>(fromOffset), 0, 0, static_cast<UINT>(fromOffset + bytes), 1, 1};
        context_->CopySubresourceRegion(buffer(to).buffer.get(), 0, static_cast<UINT>(toOffset), 0, 0, buffer(from).buffer.get(), 0, &box);
    }
    void setConstants(const void* slots, std::size_t count, std::size_t slotBytes) override {
        while (constants_.size() < count) {
            D3D11_BUFFER_DESC desc{};
            desc.ByteWidth = sizeof(StageConstants);
            desc.Usage = D3D11_USAGE_DEFAULT;
            desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            Com<ID3D11Buffer> cb;
            check(device_->CreateBuffer(&desc, nullptr, cb.put()), "CreateBuffer (constants)");
            constants_.push_back(std::move(cb));
        }
        for (std::size_t i = 0; i < count; ++i) {
            context_->UpdateSubresource(constants_[i].get(), 0, nullptr, static_cast<const char*>(slots) + i * slotBytes, 0, 0);
        }
    }
    void dispatch(Kernel kernel, std::size_t slot, const Binding& b, std::size_t groupsX, std::size_t groupsY) override {
        ID3D11UnorderedAccessView* none[kUavs] = {};
        context_->CSSetUnorderedAccessViews(0, kUavs, none, nullptr);
        auto srv = [&](int id) -> ID3D11ShaderResourceView* { return id >= 0 && has(id) ? buffer(id).srv.get() : nullptr; };
        auto uav = [&](int id) -> ID3D11UnorderedAccessView* { return id >= 0 && has(id) ? buffer(id).uav.get() : nullptr; };
        // (A buffer the kernel writes is not also bound for reading: FieldCS writes the fields.)
        ID3D11ShaderResourceView* srvs[kSrvs] = {srv(b.offsets >= 0 ? b.offsets : static_cast<int>(kOffsets)), srv(kStrandData), srv(b.in), srv(kRamps),
                                                 b.out[0] == kFields ? nullptr : srv(kFields), srv(b.aux[0]),
                                                 srv(b.aux[1]), srv(b.aux[2]), b.out[0] == kPointStrands ? nullptr : srv(kPointStrands)};
        context_->CSSetShaderResources(0, kSrvs, srvs);
        if (b.texture && !textureUav_) throw std::runtime_error("GPU preview: no point texture bound");
        ID3D11UnorderedAccessView* uavs[kUavs] = {uav(b.out[0]), uav(b.out[1]), uav(b.out[2]), b.texture ? textureUav_.get() : nullptr};
        context_->CSSetUnorderedAccessViews(0, kUavs, uavs, nullptr);
        ID3D11Buffer* cb = constants_[slot].get();
        context_->CSSetConstantBuffers(0, 1, &cb);
        context_->CSSetShader(shaders_[static_cast<int>(kernel)].get(), nullptr, 0);
        if (groupsX && groupsY) context_->Dispatch(static_cast<UINT>(groupsX), static_cast<UINT>(groupsY), 1);
        // Unbound, so the next kernel can read what this one wrote.
        ID3D11ShaderResourceView* noSrv[kSrvs] = {};
        context_->CSSetUnorderedAccessViews(0, kUavs, none, nullptr);
        context_->CSSetShaderResources(0, kSrvs, noSrv);
    }
    // (Bound once: only the constants change between them; a dispatch sees the one before's writes
    // to the views bound.)
    void dispatchAlternating(Kernel kernel, std::size_t slot, const Binding& b, std::size_t groupsX, std::size_t groupsY,
                             std::size_t count) override {
        if (count == 0) return;
        if (b.texture) {
            LookEngine::dispatchAlternating(kernel, slot, b, groupsX, groupsY, count);
            return;
        }
        ID3D11UnorderedAccessView* none[kUavs] = {};
        context_->CSSetUnorderedAccessViews(0, kUavs, none, nullptr);
        auto srv = [&](int id) -> ID3D11ShaderResourceView* { return id >= 0 && has(id) ? buffer(id).srv.get() : nullptr; };
        auto uav = [&](int id) -> ID3D11UnorderedAccessView* { return id >= 0 && has(id) ? buffer(id).uav.get() : nullptr; };
        ID3D11ShaderResourceView* srvs[kSrvs] = {srv(b.offsets >= 0 ? b.offsets : static_cast<int>(kOffsets)), srv(kStrandData), srv(b.in), srv(kRamps),
                                                 b.out[0] == kFields ? nullptr : srv(kFields), srv(b.aux[0]),
                                                 srv(b.aux[1]), srv(b.aux[2]), b.out[0] == kPointStrands ? nullptr : srv(kPointStrands)};
        context_->CSSetShaderResources(0, kSrvs, srvs);
        ID3D11UnorderedAccessView* uavs[kUavs] = {uav(b.out[0]), uav(b.out[1]), uav(b.out[2]), nullptr};
        context_->CSSetUnorderedAccessViews(0, kUavs, uavs, nullptr);
        context_->CSSetShader(shaders_[static_cast<int>(kernel)].get(), nullptr, 0);
        for (std::size_t i = 0; i < count; ++i) {
            ID3D11Buffer* cb = constants_[slot + (i & 1)].get();
            context_->CSSetConstantBuffers(0, 1, &cb);
            if (groupsX && groupsY) context_->Dispatch(static_cast<UINT>(groupsX), static_cast<UINT>(groupsY), 1);
        }
        ID3D11ShaderResourceView* noSrv[kSrvs] = {};
        context_->CSSetUnorderedAccessViews(0, kUavs, none, nullptr);
        context_->CSSetShaderResources(0, kSrvs, noSrv);
    }
    void beginRun() override {
        kept_.save(context_.get());
        context_->Begin(disjoint_.get());  // (a last run's timestamps never read are dropped)
    }
    void stamp(std::size_t index) override {
        if (index < kMaxStamps) context_->End(stamps_[index].get());
    }
    void endRun(std::size_t stamps) override {
        kept_.restore(context_.get());
        context_->End(disjoint_.get());
        stampCount_ = std::min(stamps, kMaxStamps);
        ticksPending_ = true;
    }
    bool stampTicks(bool wait, std::vector<std::uint64_t>* ticks, double* frequency) override {
        if (!ticksPending_) return false;
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj{};
        for (;;) {
            const HRESULT hr = context_->GetData(disjoint_.get(), &dj, sizeof dj, 0);
            if (hr == S_OK) break;
            if (!wait || FAILED(hr)) return false;
        }
        ticks->assign(stampCount_, 0);
        for (std::size_t i = 0; i < stampCount_; ++i) {
            while (context_->GetData(stamps_[i].get(), &(*ticks)[i], sizeof(UINT64), 0) != S_OK) {
            }
        }
        ticksPending_ = false;
        *frequency = dj.Disjoint ? 0.0 : static_cast<double>(dj.Frequency);
        return true;
    }
    std::vector<std::uint8_t> readBack(int id, std::size_t size) override {
        std::vector<std::uint8_t> bytes(size);
        if (!has(id) || size == 0) return bytes;
        const RawBuffer& b = buffer(id);
        if (!b.buffer) return bytes;
        D3D11_BUFFER_DESC desc{};
        desc.ByteWidth = static_cast<UINT>(size);
        desc.Usage = D3D11_USAGE_STAGING;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        Com<ID3D11Buffer> staging;
        check(device_->CreateBuffer(&desc, nullptr, staging.put()), "CreateBuffer (staging)");
        const D3D11_BOX box{0, 0, 0, static_cast<UINT>(size), 1, 1};
        context_->CopySubresourceRegion(staging.get(), 0, 0, 0, 0, b.buffer.get(), 0, &box);
        D3D11_MAPPED_SUBRESOURCE mapped{};
        check(context_->Map(staging.get(), 0, D3D11_MAP_READ, 0, &mapped), "Map");
        std::memcpy(bytes.data(), mapped.pData, size);
        context_->Unmap(staging.get(), 0);
        return bytes;
    }

private:
    RawBuffer& buffer(int id) {
        if (id == kOutUvs) {
            if (!uvsBound_) throw std::runtime_error("GPU preview: the uvs not bound");
            return uvs_;
        }
        if (id >= kOut0 && id <= kOut3) {
            if (!out_[id - kOut0]) throw std::runtime_error("GPU preview: an output not bound");
            return *out_[id - kOut0];
        }
        return buffers_[id];
    }

    ID3D11Device* device_ = nullptr;  // (not held: Maya's device outlives the engine)
    Com<ID3D11DeviceContext> context_;
    Com<ID3D11ComputeShader> shaders_[static_cast<int>(Kernel::Count)];
    std::vector<Com<ID3D11Buffer>> constants_;  // one per dispatch of a run
    std::map<int, RawBuffer> buffers_;
    RawBuffer targets_[4];                  // views on the targets written in place
    RawBuffer* out_[4] = {};                // kOut0-3 this run: a target, or the engine's own
    ID3D11Buffer* want_[4] = {};            // targets the own outputs are copied into
    RawBuffer uvs_;                         // the lines' uvs target (kOutUvs), written in place
    bool uvsBound_ = false;
    Com<ID3D11Texture2D> texture_;          // the app's point texture (a reference held with its view)
    Com<ID3D11UnorderedAccessView> textureUav_;
    ComputeState kept_;
    Com<ID3D11Query> disjoint_;
    Com<ID3D11Query> stamps_[kMaxStamps];
    std::size_t stampCount_ = 0;
    bool ticksPending_ = false;
};

}  // namespace

std::unique_ptr<LookEngine> makeD3D11LookEngine(ID3D11Device* device) { return std::make_unique<D3D11LookEngine>(device); }

ID3D11Buffer* createD3D11DisplayBuffer(ID3D11Device* device, std::size_t bytes) {
    if (!device) throw std::runtime_error("GPU preview: no device");
    D3D11_BUFFER_DESC desc{};
    desc.ByteWidth = static_cast<UINT>(std::max<std::size_t>((bytes + 3) & ~std::size_t{3}, 4));
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_VERTEX_BUFFER | D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
    ID3D11Buffer* buffer = nullptr;
    check(device->CreateBuffer(&desc, nullptr, &buffer), "CreateBuffer (display)");
    return buffer;
}

}  // namespace groomlab::gpu
