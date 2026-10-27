// SPDX-License-Identifier: Apache-2.0
// The lit strand lines' frames on the GPU (line_frames.h): shaders/lines.slang's FramesCS on
// DirectX 11 and OpenGL 4.5, a backend each below. Separate from the look engine (look_engine.h):
// a run here is one dispatch on buffers the host holds, with nothing of a graph's. Elsewhere than
// Windows only the shared part (LineFrames' own methods) is built: no backend (look_engine_none.cpp).

#include "groomlab/gpu/line_frames.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <string>
#include <type_traits>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d11.h>
#include <GL/gl.h>
#endif

#include "groomlab/parallel.h"
#include "groomlab/look/look_engine_internal.h"

#ifdef _WIN32
// FramesCS as DXBC (kLookDxbcFramesCS) and as GLSL (kLookGlslFramesCS), from shaders/lines.slang
// (engine/gpu/CMakeLists.txt).
#include "line_kernels_dxbc.h"
#include "line_kernels_glsl.h"
// FollowCS (kLookDxbcFollowCS, kLookGlslFollowCS), from shaders/follow.slang.
#include "follow_kernels_dxbc.h"
#include "follow_kernels_glsl.h"
// DensityClearCS, DensitySplatCS, DensityResolveCS, from shaders/density.slang.
#include "density_kernels_dxbc.h"
#include "density_kernels_glsl.h"
#endif

namespace groomlab::gpu {

namespace {

constexpr std::uint32_t kPointGroup = 256;  // lines.slang POINT_GROUP_SIZE (look.slang)
constexpr std::uint32_t kMaxGroups = 65535;

}  // namespace

std::size_t LineFrames::setLayout(const std::size_t* offsets, std::size_t strands, std::uint64_t key) {
    if (hasLayout_ && key != 0 && key == layoutKey_ && strands == strands_) return 0;
    const std::size_t points = strands ? offsets[strands] : 0;
    if (points > 0xffffffffull) throw std::runtime_error("GPU frames: more points than 32-bit offsets hold");
    std::vector<std::uint32_t> words(strands + 1);
    std::size_t uniform = strands ? offsets[1] - offsets[0] : 0;
    for (std::size_t i = 0; i <= strands; ++i) {
        words[i] = static_cast<std::uint32_t>(offsets[i]);
        if (i < strands && offsets[i + 1] - offsets[i] != uniform) uniform = 0;
    }
    upload(kOffsets, words.data(), words.size() * sizeof(std::uint32_t));
    strands_ = strands;
    points_ = points;
    uniform_ = uniform;
    layoutKey_ = key;
    hasLayout_ = key != 0;
    hasNormals_ = false;  // held for another layout
    colorsKey_ = 0;
    followRestKey_ = followRootsKey_ = 0;
    return words.size() * sizeof(std::uint32_t);
}

std::size_t LineFrames::setRootNormals(const float* xyzw, std::uint64_t key) {
    if (hasNormals_ && key != 0 && key == normalsKey_) return 0;
    upload(kRootNormals, xyzw, strands_ * 4 * sizeof(float));
    normalsKey_ = key;
    hasNormals_ = key != 0;
    return strands_ * 4 * sizeof(float);
}

void LineFrames::run(void* positions, void* normals, void* tangents) {
    if (!positions || !normals || !tangents) throw std::runtime_error("GPU frames: a target is missing");
    const std::size_t bytes = points_ * 12;
    Binding b;
    b.in = positions;
    b.out[0] = normals;
    b.out[1] = tangents;
    b.outBytes[0] = b.outBytes[1] = bytes;
    dispatchPoints(Kernel::Frames, b);
}

std::size_t LineFrames::writeWidths(void* uvs, const float* widths) {
    if (!uvs) throw std::runtime_error("GPU frames: no uvs");
    if (points_ == 0) return 0;
    upload(kWidths, widths, points_ * sizeof(float));
    Binding b;
    b.out[0] = uvs;
    b.outBytes[0] = points_ * 8;
    dispatchPoints(Kernel::Widths, b);
    return points_ * sizeof(float);
}

void LineFrames::writeRibbonLayout(void* indices, void* uvs, void* lineUvs) {
    if (!indices) throw std::runtime_error("GPU ribbons: no indices");
    Binding b;
    b.out[0] = indices;
    b.outBytes[0] = points_ * 24;
    b.out[1] = uvs;
    b.outBytes[1] = uvs ? points_ * 16 : 0;
    b.lineUvs = uvs ? lineUvs : nullptr;
    dispatchPoints(Kernel::RibbonLayout, b, nullptr, !uvs ? 0u : lineUvs ? 2u : 1u);
}

void LineFrames::writeFacingSplit(void* indices, void* lineIndices, void* points, void* lineUvs, float widthScale,
                                  const RibbonFacing& facing, float splitPixels) {
    if (!indices || !lineIndices || !points || !lineUvs) throw std::runtime_error("GPU ribbons: a split's buffer is missing");
    Binding b;
    b.in = points;
    b.lineUvs = lineUvs;
    b.out[0] = indices;
    b.outBytes[0] = points_ * 24;
    b.out[1] = lineIndices;
    b.outBytes[1] = points_ * 8;
    // (lines.slang RibbonLayoutCS gFlag.y 4: gValue the scale, the split's pixels, a pixel at distance 1; gValue2 the viewer.)
    const float values[4] = {widthScale, splitPixels, facing.pixelWorld, 0.0f};
    const float viewer[4] = {facing.eye[0], facing.eye[1], facing.eye[2], 0.0f};
    dispatchPoints(Kernel::RibbonLayout, b, values, 4u, facing.orthographic ? 2u : 1u, viewer);
}

void LineFrames::writeRibbonUvs(void* uvs, void* lineUvs) {
    if (!uvs || !lineUvs) throw std::runtime_error("GPU ribbons: no uvs");
    Binding b;
    b.out[1] = uvs;
    b.outBytes[1] = points_ * 16;
    b.lineUvs = lineUvs;
    dispatchPoints(Kernel::RibbonLayout, b, nullptr, 3u);  // (lines.slang RibbonLayoutCS: the uvs alone)
}

std::size_t LineFrames::setRibbonColors(const float* rgba, std::size_t count, std::uint64_t key) {
    if (ribbonColorsHeld(key)) return 0;
    upload(kRibbonColors, rgba, count * 4 * sizeof(float));
    colorsKey_ = key;
    return count * 4 * sizeof(float);
}

void LineFrames::writeRibbonColors(void* colors) {
    if (!colors) throw std::runtime_error("GPU ribbons: no colors");
    Binding b;
    b.out[0] = colors;
    b.outBytes[0] = points_ * 32;
    dispatchPoints(Kernel::RibbonColors, b);
}

void LineFrames::runRibbons(void* points, void* lineUvs, float widthScale, bool lit, const RibbonTargets& t, const float light[3],
                            const RibbonFacing* facing) {
    if (!points || !lineUvs || !t.positions || (lit ? !t.normals || !t.tangents : !t.colors)) {
        throw std::runtime_error("GPU ribbons: a target is missing");
    }
    Binding b;
    b.in = points;
    b.lineUvs = lineUvs;
    b.out[0] = t.positions;
    b.outBytes[0] = points_ * 24;
    if (lit) {
        b.out[1] = t.normals;
        b.out[2] = t.tangents;
        b.outBytes[1] = b.outBytes[2] = points_ * 24;
    } else {
        b.out[1] = t.colors;
        b.outBytes[1] = points_ * 32;
    }
    const float values[4] = {widthScale, light[0], light[1], light[2]};
    if (!facing) {
        dispatchPoints(Kernel::Ribbons, b, values, lit ? 0u : 1u);
        return;
    }
    // (lines.slang RibbonsCS: gValue2 the viewer and the least half width at distance 1, gFlag.z how it faces.)
    const float viewer[4] = {facing->eye[0], facing->eye[1], facing->eye[2], 0.5f * facing->minPixels * facing->pixelWorld};
    dispatchPoints(Kernel::Ribbons, b, values, lit ? 0u : 1u, (facing->orthographic ? 2u : 1u) | (facing->unshaded ? 4u : 0u), viewer);
}

std::size_t LineFrames::setFollowRest(const float* points, std::uint64_t key) {
    if (followRestHeld(key)) return 0;
    upload(kFollowRest, points, points_ * 12);
    followRestKey_ = key;
    return points_ * 12;
}

std::size_t LineFrames::setFollowRoots(const void* roots, std::uint64_t key) {
    if (followRootsHeld(key)) return 0;
    upload(kFollowRoots, roots, strands_ * 32);
    followRootsKey_ = key;
    return strands_ * 32;
}

std::size_t LineFrames::setFollowScalp(bool now, const float* points, const float* normals, std::size_t vertices, std::uint64_t key) {
    const int i = now ? 1 : 0;
    if (key != 0 && key == followScalpKeys_[i] && vertices == followVertices_[i]) return 0;
    // Points, then normals (follow.slang scalpPoint, scalpNormal).
    std::vector<float> both(vertices * 6);
    if (vertices) {
        std::memcpy(both.data(), points, vertices * 12);
        std::memcpy(both.data() + vertices * 3, normals, vertices * 12);
    }
    upload(now ? kFollowScalpNow : kFollowScalpRest, both.data(), both.size() * sizeof(float));
    followScalpKeys_[i] = key;
    followVertices_[i] = vertices;
    return both.size() * sizeof(float);
}

void LineFrames::runFollow(void* positions) {
    if (!positions) throw std::runtime_error("GPU follow: no points to write");
    if (followRestKey_ == 0 || followRootsKey_ == 0 || followScalpKeys_[0] == 0 || followScalpKeys_[1] == 0) {
        throw std::runtime_error("GPU follow: the strands at rest, their roots or the scalps are missing");
    }
    if (followVertices_[0] != followVertices_[1]) throw std::runtime_error("GPU follow: the scalps have other vertices");
    if (followVertices_[0] > 0xffffffffull / 24) throw std::runtime_error("GPU follow: too many scalp vertices");
    upload(kRootNormals, nullptr, strands_ * 4 * sizeof(float));  // (written by the kernel)
    hasNormals_ = false;  // the follow's from now: setRootNormals sends its own again
    normalsKey_ = 0;
    Binding b;
    b.out[0] = positions;
    b.outBytes[0] = points_ * 12;
    dispatchPoints(Kernel::Follow, b, nullptr, 0, static_cast<std::uint32_t>(followVertices_[0]));
}

void LineFrames::runDensity(void* positions, void* lineUvs, std::size_t count, const DensityGrid& grid, void* volume) {
    if (!positions || !lineUvs || !volume) throw std::runtime_error("GPU density: a target is missing");
    const std::size_t cells = std::size_t{grid.size[0]} * grid.size[1] * grid.size[2];
    if (cells == 0 || count == 0) return;
    if (count > 0xffffffffull || cells > 0xffffffffull) throw std::runtime_error("GPU density: too many points or cells");
    StageConstants c{};
    for (int k = 0; k < 3; ++k) {
        c.value[k] = grid.origin[k];
        c.flag[k] = grid.size[k];
    }
    c.value[3] = grid.cell;
    c.flag[3] = static_cast<std::uint32_t>(count);
    c.value2[0] = grid.pieceWeight;
    c.value2[1] = grid.defaultWidth;
    c.value2[2] = grid.occlusionReach;
    // The groups of each dispatch (clear and resolve a thread a cell, the splat a thread a point), in rows.
    std::uint32_t groups[3][2];
    const std::size_t threads[3] = {cells, count, cells};
    for (int k = 0; k < 3; ++k) {
        const std::uint64_t g = (threads[k] + kPointGroup - 1) / kPointGroup;
        groups[k][0] = static_cast<std::uint32_t>(std::min<std::uint64_t>(g, kMaxGroups));
        groups[k][1] = static_cast<std::uint32_t>((g + groups[k][0] - 1) / groups[k][0]);
        if (groups[k][1] > kMaxGroups) throw std::runtime_error("GPU density: too many threads for one dispatch");
    }
    dispatchDensity(&c, positions, lineUvs, count, cells, groups, volume);
    densityCells_ = cells;
    densityPoints_ = count;
}

double LineFrames::densitySum() {
    if (densityCells_ == 0) return 0.0;
    double sum = 0.0;
    for (const std::uint32_t v : readDensityGrid(densityCells_)) sum += static_cast<double>(v);
    return sum / 65536.0;  // (density.slang's kFixed)
}

void LineFrames::dispatchPoints(Kernel kernel, const Binding& binding, const float* values, std::uint32_t flag, std::uint32_t flagZ,
                                const float* values2) {
    if (points_ == 0) return;
    const std::uint64_t groups = (points_ + kPointGroup - 1) / kPointGroup;
    const auto groupsX = static_cast<std::uint32_t>(std::min<std::uint64_t>(groups, kMaxGroups));
    const auto groupsY = static_cast<std::uint32_t>((groups + groupsX - 1) / groupsX);
    if (groupsY > kMaxGroups) throw std::runtime_error("GPU frames: too many points for one dispatch");
    StageConstants c{};
    c.strandCount = static_cast<std::uint32_t>(strands_);
    c.uniformCount = static_cast<std::uint32_t>(uniform_);
    c.pointCount = static_cast<std::uint32_t>(points_);
    c.flag[0] = groupsX * kPointGroup;  // the threads in a row
    c.flag[1] = flag;
    c.flag[2] = flagZ;
    if (values) std::memcpy(c.value, values, sizeof c.value);
    if (values2) std::memcpy(c.value2, values2, sizeof c.value2);
    c.table[1] = 0;                    // the root normals: block 0 of gStrandData
    dispatch(kernel, &c, sizeof c, binding, groupsX, groupsY);
}

#ifdef _WIN32
namespace {

// ---- DirectX 11 ---------------------------------------------------------------------------------

void check(HRESULT hr, const char* what) {
    if (FAILED(hr)) {
        char text[160];
        std::snprintf(text, sizeof text, "GPU frames: %s failed (0x%08lx)", what, static_cast<unsigned long>(hr));
        throw std::runtime_error(text);
    }
}

template <class T>
void releaseAll(T** items, std::size_t n) {
    for (std::size_t i = 0; i < n; ++i) {
        if (items[i]) items[i]->Release();
        items[i] = nullptr;
    }
}

class D3D11LineFrames final : public LineFrames {
public:
    explicit D3D11LineFrames(ID3D11Device* device) : device_(device) {
        if (!device_) throw std::runtime_error("GPU frames: no device");
        device_->GetImmediateContext(&context_);
        check(device_->CreateComputeShader(kLookDxbcFramesCS, sizeof kLookDxbcFramesCS, nullptr, &shaders_[0]), "FramesCS");
        check(device_->CreateComputeShader(kLookDxbcWidthsCS, sizeof kLookDxbcWidthsCS, nullptr, &shaders_[1]), "WidthsCS");
        check(device_->CreateComputeShader(kLookDxbcRibbonsCS, sizeof kLookDxbcRibbonsCS, nullptr, &shaders_[2]), "RibbonsCS");
        check(device_->CreateComputeShader(kLookDxbcRibbonLayoutCS, sizeof kLookDxbcRibbonLayoutCS, nullptr, &shaders_[3]),
              "RibbonLayoutCS");
        check(device_->CreateComputeShader(kLookDxbcRibbonColorsCS, sizeof kLookDxbcRibbonColorsCS, nullptr, &shaders_[4]),
              "RibbonColorsCS");
        check(device_->CreateComputeShader(kLookDxbcFollowCS, sizeof kLookDxbcFollowCS, nullptr, &shaders_[5]), "FollowCS");
        check(device_->CreateComputeShader(kLookDxbcDensityClearCS, sizeof kLookDxbcDensityClearCS, nullptr, &density_[0]), "DensityClearCS");
        check(device_->CreateComputeShader(kLookDxbcDensitySplatCS, sizeof kLookDxbcDensitySplatCS, nullptr, &density_[1]), "DensitySplatCS");
        check(device_->CreateComputeShader(kLookDxbcDensityResolveCS, sizeof kLookDxbcDensityResolveCS, nullptr, &density_[2]),
              "DensityResolveCS");
        D3D11_BUFFER_DESC desc{};
        desc.ByteWidth = sizeof(StageConstants);
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        check(device_->CreateBuffer(&desc, nullptr, &constants_), "CreateBuffer (constants)");
        D3D11_QUERY_DESC q{};
        q.Query = D3D11_QUERY_TIMESTAMP_DISJOINT;
        check(device_->CreateQuery(&q, &disjoint_), "CreateQuery");
        q.Query = D3D11_QUERY_TIMESTAMP;
        for (auto*& s : stamps_) check(device_->CreateQuery(&q, &s), "CreateQuery");
    }
    ~D3D11LineFrames() override {
        for (Raw& b : buffers_) b.reset();
        for (Staging& s : staging_) {
            if (s.buffer) s.buffer->Release();
        }
        releaseAll(stamps_, 2);
        if (disjoint_) disjoint_->Release();
        if (constants_) constants_->Release();
        releaseAll(shaders_, kKernels);
        releaseAll(density_, 3);
        releaseGrid();
        if (context_) context_->Release();
    }

    GpuApi api() const override { return GpuApi::D3D11; }
    void* device() const override { return device_; }

    void write(void* target, const void* data, std::size_t offset, std::size_t bytes) override {
        if (!target || !bytes) return;
        put(static_cast<ID3D11Buffer*>(target), data, offset, bytes, 0);
    }

    void* beginWrite(void* target, std::size_t offset, std::size_t bytes) override {
        Staging& s = staging_[kFillSlot];
        if (!target || !bytes || filling_ || !stage(s, bytes)) return nullptr;
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (FAILED(context_->Map(s.buffer, 0, D3D11_MAP_WRITE, 0, &mapped))) return nullptr;
        filling_ = static_cast<ID3D11Buffer*>(target);
        fillOffset_ = offset;
        fillBytes_ = bytes;
        return mapped.pData;
    }
    void endWrite() override {
        if (!filling_) return;
        ID3D11Buffer* staging = staging_[kFillSlot].buffer;
        context_->Unmap(staging, 0);
        const D3D11_BOX box{0, 0, 0, static_cast<UINT>(fillBytes_), 1, 1};
        context_->CopySubresourceRegion(filling_, 0, static_cast<UINT>(fillOffset_), 0, 0, staging, 0, &box);
        filling_ = nullptr;
    }

    double gpuMs(bool wait) override {
        if (!pending_) return lastMs_;
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj{};
        for (;;) {
            const HRESULT hr = context_->GetData(disjoint_, &dj, sizeof dj, 0);
            if (hr == S_OK) break;
            if (!wait || FAILED(hr)) return -1.0;
        }
        UINT64 t[2] = {};
        for (int i = 0; i < 2; ++i) {
            while (context_->GetData(stamps_[i], &t[i], sizeof(UINT64), 0) != S_OK) {
            }
        }
        pending_ = false;
        lastMs_ = dj.Disjoint || !dj.Frequency ? -1.0 : static_cast<double>(t[1] - t[0]) * 1000.0 / static_cast<double>(dj.Frequency);
        return lastMs_;
    }

    std::vector<float> read(void* target, std::size_t count) override {
        std::vector<float> out(count);
        if (!target || !count) return out;
        D3D11_BUFFER_DESC desc{};
        desc.ByteWidth = static_cast<UINT>(count * 4);
        desc.Usage = D3D11_USAGE_STAGING;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ID3D11Buffer* staging = nullptr;
        check(device_->CreateBuffer(&desc, nullptr, &staging), "CreateBuffer (staging)");
        const D3D11_BOX box{0, 0, 0, desc.ByteWidth, 1, 1};
        context_->CopySubresourceRegion(staging, 0, 0, 0, 0, static_cast<ID3D11Buffer*>(target), 0, &box);
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (SUCCEEDED(context_->Map(staging, 0, D3D11_MAP_READ, 0, &mapped))) {
            std::memcpy(out.data(), mapped.pData, count * 4);
            context_->Unmap(staging, 0);
        }
        staging->Release();
        return out;
    }

    std::vector<float> readVolume(void* texture) override {
        std::vector<float> out;
        ID3D11Texture3D* source = texture ? texture3DOf(texture) : volume_;
        if (!source) return out;
        if (!texture) source->AddRef();
        D3D11_TEXTURE3D_DESC desc{};
        source->GetDesc(&desc);
        if (desc.Format != DXGI_FORMAT_R32G32_FLOAT) {
            source->Release();
            return out;
        }
        D3D11_TEXTURE3D_DESC s = desc;
        s.Usage = D3D11_USAGE_STAGING;
        s.BindFlags = 0;
        s.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        s.MiscFlags = 0;
        s.MipLevels = 1;
        ID3D11Texture3D* staging = nullptr;
        if (FAILED(device_->CreateTexture3D(&s, nullptr, &staging))) {
            source->Release();
            return out;
        }
        context_->CopySubresourceRegion(staging, 0, 0, 0, 0, source, 0, nullptr);
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (SUCCEEDED(context_->Map(staging, 0, D3D11_MAP_READ, 0, &mapped))) {
            const std::size_t row = std::size_t{desc.Width} * 2;
            out.resize(row * desc.Height * desc.Depth);
            for (UINT z = 0; z < desc.Depth; ++z) {
                for (UINT y = 0; y < desc.Height; ++y) {
                    const auto* from = static_cast<const unsigned char*>(mapped.pData) + std::size_t{z} * mapped.DepthPitch +
                                       std::size_t{y} * mapped.RowPitch;
                    std::memcpy(out.data() + (std::size_t{z} * desc.Height + y) * row, from, row * sizeof(float));
                }
            }
            context_->Unmap(staging, 0);
        }
        staging->Release();
        source->Release();
        return out;
    }

    bool writeVolume(void* texture, const std::vector<float>& texels) override {
        ID3D11Texture3D* target = texture ? texture3DOf(texture) : nullptr;
        if (!target) return false;
        D3D11_TEXTURE3D_DESC desc{};
        target->GetDesc(&desc);
        const std::size_t row = std::size_t{desc.Width} * 2;
        const bool fits = desc.Format == DXGI_FORMAT_R32G32_FLOAT && texels.size() == row * desc.Height * desc.Depth;
        if (fits) {
            context_->UpdateSubresource(target, 0, nullptr, texels.data(), static_cast<UINT>(row * sizeof(float)),
                                        static_cast<UINT>(row * desc.Height * sizeof(float)));
        }
        target->Release();
        return fits;
    }

protected:
    // The 3D texture a handle names (a texture, or a view of one), referenced; null where it names none.
    static ID3D11Texture3D* texture3DOf(void* handle) {
        ID3D11Texture3D* target = nullptr;
        auto* unknown = static_cast<IUnknown*>(handle);
        if (SUCCEEDED(unknown->QueryInterface(__uuidof(ID3D11Texture3D), reinterpret_cast<void**>(&target)))) return target;
        ID3D11View* view = nullptr;
        if (SUCCEEDED(unknown->QueryInterface(__uuidof(ID3D11View), reinterpret_cast<void**>(&view)))) {
            ID3D11Resource* resource = nullptr;
            view->GetResource(&resource);
            view->Release();
            if (resource) {
                resource->QueryInterface(__uuidof(ID3D11Texture3D), reinterpret_cast<void**>(&target));
                resource->Release();
            }
        }
        return target;
    }

    std::vector<std::uint32_t> readDensityGrid(std::size_t cells) override {
        std::vector<std::uint32_t> out(cells);
        if (!grid_ || cells == 0 || cells > gridCells_) return out;
        const std::vector<float> raw = read(grid_, cells);
        std::memcpy(out.data(), raw.data(), cells * 4);
        return out;
    }

    void upload(Buffer which, const void* data, std::size_t bytes) override {
        Raw& b = buffers_[which];
        const std::size_t size = std::max<std::size_t>((bytes + 3) & ~std::size_t{3}, 16);
        if (b.bytes < size || b.bytes > 2 * size) {  // (room to grow: other counts, a playback's, reuse it)
            b.reset();
            const std::size_t room = (size + size / 8 + 3) & ~std::size_t{3};
            D3D11_BUFFER_DESC desc{};
            desc.ByteWidth = static_cast<UINT>(room);
            desc.Usage = D3D11_USAGE_DEFAULT;
            // (The root normals: runFollow's kernel writes them too.)
            desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | (which == kRootNormals ? D3D11_BIND_UNORDERED_ACCESS : 0u);
            desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
            check(device_->CreateBuffer(&desc, nullptr, &b.buffer), "CreateBuffer");
            b.srv = rawView(b.buffer, room);
            b.bytes = room;
        }
        if (bytes && data) put(b.buffer, data, 0, bytes, 1 + static_cast<int>(which));
    }

    void dispatch(Kernel kernel, const void* constants, std::size_t, const Binding& binding, std::uint32_t groupsX,
                  std::uint32_t groupsY) override {
        const bool timed = kernel == Kernel::Frames || kernel == Kernel::Ribbons;
        const bool framed = kernel == Kernel::Frames || kernel == Kernel::Ribbons;  // the root normals read
        const bool follow = kernel == Kernel::Follow;  // the follow buffers read, the root normals written
        if (!buffers_[kOffsets].srv || ((framed || follow) && !buffers_[kRootNormals].srv)) throw std::runtime_error("GPU frames: no layout or root normals");
        if (follow && (!buffers_[kFollowRoots].srv || !buffers_[kFollowRest].srv || !buffers_[kFollowScalpRest].srv || !buffers_[kFollowScalpNow].srv)) {
            throw std::runtime_error("GPU follow: the strands at rest, their roots or the scalps are missing");
        }
        if (kernel == Kernel::Widths && !buffers_[kWidths].srv) throw std::runtime_error("GPU frames: no widths");
        const bool colored = kernel == Kernel::RibbonColors || (kernel == Kernel::Ribbons && binding.out[2] == nullptr);
        if (colored && !buffers_[kRibbonColors].srv) throw std::runtime_error("GPU ribbons: no colors");
        // What is read: the points (float3 a point), the lines' uvs (float2); what is written, as
        // long as the binding says.
        const std::size_t points = pointCount();
        auto usable = [](ID3D11Buffer* b, std::size_t bytes) {
            D3D11_BUFFER_DESC d{};
            b->GetDesc(&d);
            const UINT need = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
            if ((d.BindFlags & need) != need || !(d.MiscFlags & D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS) || d.ByteWidth < bytes) {
                throw std::runtime_error("GPU frames: a target is not a buffer compute may use (createD3D11DisplayBuffer)");
            }
        };
        auto* in = static_cast<ID3D11Buffer*>(binding.in);
        auto* uvs = static_cast<ID3D11Buffer*>(binding.lineUvs);
        if (in) usable(in, points * 12);
        if (uvs) usable(uvs, points * 8);
        for (int i = 0; i < 3; ++i) {
            if (binding.out[i]) usable(static_cast<ID3D11Buffer*>(binding.out[i]), binding.outBytes[i]);
        }
        ID3D11ShaderResourceView* reads[2] = {in ? rawView(in, points * 12) : nullptr, uvs ? rawView(uvs, points * 8) : nullptr};
        ID3D11UnorderedAccessView* out[3] = {};
        for (int i = 0; i < 3; ++i) {
            if (binding.out[i]) out[i] = rawAccess(static_cast<ID3D11Buffer*>(binding.out[i]), binding.outBytes[i]);
        }
        if (follow) out[1] = rawAccess(buffers_[kRootNormals].buffer, std::max<std::size_t>(strandCount() * 16, 16));  // (gOut2)
        context_->UpdateSubresource(constants_, 0, nullptr, constants, 0, 0);
        // The compute state touched, kept and put back (the renderer owns the context).
        constexpr UINT kSrvs = 7;  // t0-t6: look.slang's gOffsets, gStrandData, gIn, (gRamps, gFields), gAux0, gAux1
        ID3D11ComputeShader* keptShader = nullptr;
        ID3D11Buffer* keptConstants = nullptr;
        ID3D11ShaderResourceView* keptSrvs[kSrvs] = {};
        ID3D11UnorderedAccessView* keptUavs[3] = {};
        context_->CSGetShader(&keptShader, nullptr, nullptr);
        context_->CSGetConstantBuffers(0, 1, &keptConstants);
        context_->CSGetShaderResources(0, kSrvs, keptSrvs);
        context_->CSGetUnorderedAccessViews(0, 3, keptUavs);

        if (timed) {
            context_->Begin(disjoint_);
            context_->End(stamps_[0]);
        }
        ID3D11ShaderResourceView* srvs[kSrvs] = {buffers_[kOffsets].srv, buffers_[kRootNormals].srv,
                                                 kernel == Kernel::Widths ? buffers_[kWidths].srv : reads[0], nullptr, nullptr,
                                                 colored ? buffers_[kRibbonColors].srv : nullptr, reads[1]};
        if (follow) {  // follow.slang: gStrandData the roots, gIn the points at rest, gAux0 and gAux1 the scalps
            srvs[1] = buffers_[kFollowRoots].srv;
            srvs[2] = buffers_[kFollowRest].srv;
            srvs[5] = buffers_[kFollowScalpRest].srv;
            srvs[6] = buffers_[kFollowScalpNow].srv;
        }
        context_->CSSetShaderResources(0, kSrvs, srvs);
        context_->CSSetUnorderedAccessViews(0, 3, out, nullptr);
        context_->CSSetConstantBuffers(0, 1, &constants_);
        context_->CSSetShader(shaders_[static_cast<int>(kernel)], nullptr, 0);
        context_->Dispatch(groupsX, groupsY, 1);
        if (timed) {
            context_->End(stamps_[1]);
            context_->End(disjoint_);
            pending_ = true;
        }

        ID3D11UnorderedAccessView* noUavs[3] = {};
        context_->CSSetUnorderedAccessViews(0, 3, noUavs, nullptr);  // before the SRVs: a buffer may be both
        context_->CSSetShader(keptShader, nullptr, 0);
        context_->CSSetConstantBuffers(0, 1, &keptConstants);
        context_->CSSetShaderResources(0, kSrvs, keptSrvs);
        context_->CSSetUnorderedAccessViews(0, 3, keptUavs, nullptr);
        if (keptShader) keptShader->Release();
        if (keptConstants) keptConstants->Release();
        releaseAll(keptSrvs, kSrvs);
        releaseAll(keptUavs, 3);
        releaseAll(reads, 2);
        releaseAll(out, 3);
    }

    void dispatchDensity(void* constants, void* positions, void* lineUvs, std::size_t points, std::size_t cells,
                         const std::uint32_t groups[3][2], void* volume) override {
        auto* c = static_cast<StageConstants*>(constants);
        // The target: a 3D texture of the grid's size and format (the host's, copied into).
        // (A texture, or a view of one: Viewport 2.0's MTexture hands out its shader resource view.)
        ID3D11Texture3D* target = texture3DOf(volume);
        if (!target) throw std::runtime_error("GPU density: the target is not a 3D texture");
        D3D11_TEXTURE3D_DESC want{};
        target->GetDesc(&want);
        if (want.Width != c->flag[0] || want.Height != c->flag[1] || want.Depth != c->flag[2] || want.Format != DXGI_FORMAT_R32G32_FLOAT) {
            target->Release();
            throw std::runtime_error("GPU density: the target texture is not the grid's size and format");
        }
        // The grid (a word a cell) and a texture of our own the kernels write, made again for another size.
        if (gridCells_ != cells || !volume_ || volumeDesc_.Width != want.Width || volumeDesc_.Height != want.Height ||
            volumeDesc_.Depth != want.Depth) {
            releaseGrid();
            D3D11_BUFFER_DESC desc{};
            desc.ByteWidth = static_cast<UINT>(cells * 4);
            desc.Usage = D3D11_USAGE_DEFAULT;
            desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
            desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
            check(device_->CreateBuffer(&desc, nullptr, &grid_), "CreateBuffer (density grid)");
            gridUav_ = rawAccess(grid_, cells * 4);
            D3D11_TEXTURE3D_DESC t{};
            t.Width = want.Width;
            t.Height = want.Height;
            t.Depth = want.Depth;
            t.MipLevels = 1;
            t.Format = DXGI_FORMAT_R32G32_FLOAT;
            t.Usage = D3D11_USAGE_DEFAULT;
            t.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
            check(device_->CreateTexture3D(&t, nullptr, &volume_), "CreateTexture3D (density)");
            D3D11_UNORDERED_ACCESS_VIEW_DESC u{};
            u.Format = DXGI_FORMAT_R32G32_FLOAT;
            u.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE3D;
            u.Texture3D.WSize = want.Depth;
            check(device_->CreateUnorderedAccessView(volume_, &u, &volumeUav_), "CreateUnorderedAccessView (density)");
            volumeDesc_ = t;
            gridCells_ = cells;
        }
        ID3D11ShaderResourceView* reads[2] = {rawView(static_cast<ID3D11Buffer*>(positions), points * 12),
                                              rawView(static_cast<ID3D11Buffer*>(lineUvs), points * 8)};
        // The compute state touched, kept and put back (the renderer owns the context).
        constexpr UINT kSrvs = 7, kUavs = 4;
        ID3D11ComputeShader* keptShader = nullptr;
        ID3D11Buffer* keptConstants = nullptr;
        ID3D11ShaderResourceView* keptSrvs[kSrvs] = {};
        ID3D11UnorderedAccessView* keptUavs[kUavs] = {};
        context_->CSGetShader(&keptShader, nullptr, nullptr);
        context_->CSGetConstantBuffers(0, 1, &keptConstants);
        context_->CSGetShaderResources(0, kSrvs, keptSrvs);
        context_->CSGetUnorderedAccessViews(0, kUavs, keptUavs);
        context_->Begin(disjoint_);
        context_->End(stamps_[0]);
        ID3D11ShaderResourceView* srvs[kSrvs] = {nullptr, nullptr, reads[0], nullptr, nullptr, nullptr, reads[1]};
        context_->CSSetShaderResources(0, kSrvs, srvs);
        ID3D11UnorderedAccessView* uavs[kUavs] = {gridUav_, nullptr, nullptr, volumeUav_};
        context_->CSSetUnorderedAccessViews(0, kUavs, uavs, nullptr);
        context_->CSSetConstantBuffers(0, 1, &constants_);
        for (int k = 0; k < 3; ++k) {
            c->dispatch[0] = groups[k][0] * kPointGroup;  // the threads in a row
            context_->UpdateSubresource(constants_, 0, nullptr, c, 0, 0);
            context_->CSSetShader(density_[k], nullptr, 0);
            context_->Dispatch(groups[k][0], groups[k][1], 1);
        }
        context_->End(stamps_[1]);
        context_->End(disjoint_);
        pending_ = true;
        ID3D11UnorderedAccessView* noUavs[kUavs] = {};
        context_->CSSetUnorderedAccessViews(0, kUavs, noUavs, nullptr);
        context_->CSSetShader(keptShader, nullptr, 0);
        context_->CSSetConstantBuffers(0, 1, &keptConstants);
        context_->CSSetShaderResources(0, kSrvs, keptSrvs);
        context_->CSSetUnorderedAccessViews(0, kUavs, keptUavs, nullptr);
        if (keptShader) keptShader->Release();
        if (keptConstants) keptConstants->Release();
        releaseAll(keptSrvs, kSrvs);
        releaseAll(keptUavs, kUavs);
        releaseAll(reads, 2);
        context_->CopyResource(target, volume_);
        target->Release();
    }

private:
    void releaseGrid() {
        if (volumeUav_) volumeUav_->Release();
        if (volume_) volume_->Release();
        if (gridUav_) gridUav_->Release();
        if (grid_) grid_->Release();
        volumeUav_ = nullptr;
        volume_ = nullptr;
        gridUav_ = nullptr;
        grid_ = nullptr;
        gridCells_ = 0;
        volumeDesc_ = D3D11_TEXTURE3D_DESC{};
    }
    ID3D11ComputeShader* density_[3] = {};
    ID3D11Buffer* grid_ = nullptr;
    ID3D11UnorderedAccessView* gridUav_ = nullptr;
    ID3D11Texture3D* volume_ = nullptr;
    ID3D11UnorderedAccessView* volumeUav_ = nullptr;
    D3D11_TEXTURE3D_DESC volumeDesc_{};
    std::size_t gridCells_ = 0;

    // `bytes` of `data` into `target` at `offset`: small writes by UpdateSubresource; large ones
    // copied in parallel chunks into a staging buffer the CPU writes (mapped), then copied on the
    // GPU (UpdateSubresource copies them on one thread: ~18 ms for 100 MB). A staging buffer per
    // `slot` (the points, each kind of upload), so one write never waits for the GPU to finish
    // copying another's.
    void put(ID3D11Buffer* target, const void* data, std::size_t offset, std::size_t bytes, int slot) {
        constexpr std::size_t kStagedFrom = std::size_t{1} << 20;
        Staging& s = staging_[slot];
        if (bytes >= kStagedFrom && stage(s, bytes)) {
            D3D11_MAPPED_SUBRESOURCE mapped{};
            if (SUCCEEDED(context_->Map(s.buffer, 0, D3D11_MAP_WRITE, 0, &mapped))) {
                constexpr std::size_t kChunk = std::size_t{1} << 20;
                parallelFor(0, (bytes + kChunk - 1) / kChunk, [&](std::size_t c) {
                    const std::size_t at = c * kChunk;
                    std::memcpy(static_cast<char*>(mapped.pData) + at, static_cast<const char*>(data) + at, std::min(kChunk, bytes - at));
                }, 1);
                context_->Unmap(s.buffer, 0);
                const D3D11_BOX box{0, 0, 0, static_cast<UINT>(bytes), 1, 1};
                context_->CopySubresourceRegion(target, 0, static_cast<UINT>(offset), 0, 0, s.buffer, 0, &box);
                return;
            }
        }
        const D3D11_BOX box{static_cast<UINT>(offset), 0, 0, static_cast<UINT>(offset + bytes), 1, 1};
        context_->UpdateSubresource(target, 0, &box, data, 0, 0);
    }
    // A staging buffer, at least `bytes` long (made again when smaller, or more than twice as long).
    struct Staging {
        ID3D11Buffer* buffer = nullptr;
        std::size_t bytes = 0;
    };
    bool stage(Staging& s, std::size_t bytes) {
        if (s.buffer && s.bytes >= bytes && s.bytes <= 2 * bytes) return true;
        if (s.buffer) s.buffer->Release();
        s = Staging{};
        const std::size_t room = bytes + bytes / 8;  // (other counts reuse it)
        D3D11_BUFFER_DESC desc{};
        desc.ByteWidth = static_cast<UINT>(room);
        desc.Usage = D3D11_USAGE_STAGING;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(device_->CreateBuffer(&desc, nullptr, &s.buffer))) return false;
        s.bytes = room;
        return true;
    }

    struct Raw {
        ID3D11Buffer* buffer = nullptr;
        ID3D11ShaderResourceView* srv = nullptr;
        std::size_t bytes = 0;
        void reset() {
            if (srv) srv->Release();
            if (buffer) buffer->Release();
            srv = nullptr;
            buffer = nullptr;
            bytes = 0;
        }
    };

    ID3D11ShaderResourceView* rawView(ID3D11Buffer* b, std::size_t bytes) {
        D3D11_SHADER_RESOURCE_VIEW_DESC s{};
        s.Format = DXGI_FORMAT_R32_TYPELESS;
        s.ViewDimension = D3D11_SRV_DIMENSION_BUFFEREX;
        s.BufferEx.NumElements = static_cast<UINT>(bytes / 4);
        s.BufferEx.Flags = D3D11_BUFFEREX_SRV_FLAG_RAW;
        ID3D11ShaderResourceView* view = nullptr;
        check(device_->CreateShaderResourceView(b, &s, &view), "CreateShaderResourceView");
        return view;
    }
    ID3D11UnorderedAccessView* rawAccess(ID3D11Buffer* b, std::size_t bytes) {
        D3D11_UNORDERED_ACCESS_VIEW_DESC u{};
        u.Format = DXGI_FORMAT_R32_TYPELESS;
        u.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
        u.Buffer.NumElements = static_cast<UINT>(bytes / 4);
        u.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
        ID3D11UnorderedAccessView* view = nullptr;
        check(device_->CreateUnorderedAccessView(b, &u, &view), "CreateUnorderedAccessView");
        return view;
    }

    ID3D11Device* device_ = nullptr;  // (not held: the host's device outlives this)
    ID3D11DeviceContext* context_ = nullptr;
    static constexpr int kKernels = static_cast<int>(Kernel::Count);
    ID3D11ComputeShader* shaders_[kKernels] = {};  // by Kernel: FramesCS, WidthsCS, RibbonsCS, RibbonLayoutCS, RibbonColorsCS, FollowCS
    ID3D11Buffer* constants_ = nullptr;
    Raw buffers_[kBufferCount];
    Staging staging_[2 + kBufferCount];  // large writes go through them (put): the points, each upload, beginWrite's
    static constexpr int kFillSlot = 1 + kBufferCount;
    ID3D11Buffer* filling_ = nullptr;  // beginWrite's target (not held: the caller's, until endWrite)
    std::size_t fillOffset_ = 0, fillBytes_ = 0;
    ID3D11Query* disjoint_ = nullptr;
    ID3D11Query* stamps_[2] = {};
    bool pending_ = false;
    double lastMs_ = -1.0;
};

// ---- OpenGL ---------------------------------------------------------------------------------------

using GLchar = char;
using GLintptr = std::ptrdiff_t;
using GLsizeiptr = std::ptrdiff_t;
using GLuint64 = std::uint64_t;
using GLint64 = std::int64_t;

constexpr GLenum kComputeShader = 0x91B9;
constexpr GLenum kCompileStatus = 0x8B81;
constexpr GLenum kLinkStatus = 0x8B82;
constexpr GLenum kInfoLogLength = 0x8B84;
constexpr GLenum kShaderStorageBuffer = 0x90D2;
constexpr GLenum kShaderStorageBinding = 0x90D3;
constexpr GLenum kShaderStorageStart = 0x90D4;
constexpr GLenum kShaderStorageSize = 0x90D5;
constexpr GLenum kUniformBuffer = 0x8A11;
constexpr GLenum kUniformBinding = 0x8A28;
constexpr GLenum kUniformStart = 0x8A29;
constexpr GLenum kUniformSize = 0x8A2A;
constexpr GLenum kCopyReadBuffer = 0x8F36;
constexpr GLenum kCopyWriteBuffer = 0x8F37;
constexpr GLenum kCurrentProgram = 0x8B8D;
constexpr GLenum kStaticDraw = 0x88E4;
constexpr GLenum kDynamicDraw = 0x88E8;
constexpr GLenum kBufferSize = 0x8764;
constexpr GLbitfield kShaderStorageBarrier = 0x00002000;
constexpr GLbitfield kVertexAttribArrayBarrier = 0x00000001;
constexpr GLbitfield kBufferUpdateBarrier = 0x00000200;
constexpr GLenum kTimestamp = 0x8E28;
constexpr GLenum kQueryResult = 0x8866;
constexpr GLenum kQueryResultAvailable = 0x8867;
constexpr GLenum kMajorVersion = 0x821B;
constexpr GLenum kMinorVersion = 0x821C;
constexpr GLbitfield kMapWrite = 0x0002;
constexpr GLbitfield kMapInvalidateBuffer = 0x0008;

struct Gl {
    GLuint(APIENTRY* CreateShader)(GLenum);
    void(APIENTRY* ShaderSource)(GLuint, GLsizei, const GLchar* const*, const GLint*);
    void(APIENTRY* CompileShader)(GLuint);
    void(APIENTRY* GetShaderiv)(GLuint, GLenum, GLint*);
    void(APIENTRY* GetShaderInfoLog)(GLuint, GLsizei, GLsizei*, GLchar*);
    GLuint(APIENTRY* CreateProgram)();
    void(APIENTRY* AttachShader)(GLuint, GLuint);
    void(APIENTRY* LinkProgram)(GLuint);
    void(APIENTRY* GetProgramiv)(GLuint, GLenum, GLint*);
    void(APIENTRY* DeleteShader)(GLuint);
    void(APIENTRY* DeleteProgram)(GLuint);
    void(APIENTRY* UseProgram)(GLuint);
    GLboolean(APIENTRY* IsProgram)(GLuint);
    void(APIENTRY* DispatchCompute)(GLuint, GLuint, GLuint);
    void(APIENTRY* MemoryBarrier)(GLbitfield);
    void(APIENTRY* BindBufferBase)(GLenum, GLuint, GLuint);
    void(APIENTRY* BindBufferRange)(GLenum, GLuint, GLuint, GLintptr, GLsizeiptr);
    void(APIENTRY* GenBuffers)(GLsizei, GLuint*);
    void(APIENTRY* DeleteBuffers)(GLsizei, const GLuint*);
    void(APIENTRY* BindBuffer)(GLenum, GLuint);
    void(APIENTRY* BufferData)(GLenum, GLsizeiptr, const void*, GLenum);
    void(APIENTRY* BufferSubData)(GLenum, GLintptr, GLsizeiptr, const void*);
    void(APIENTRY* GetBufferSubData)(GLenum, GLintptr, GLsizeiptr, void*);
    void(APIENTRY* GetBufferParameteriv)(GLenum, GLenum, GLint*);
    void(APIENTRY* GenQueries)(GLsizei, GLuint*);
    void(APIENTRY* DeleteQueries)(GLsizei, const GLuint*);
    void(APIENTRY* QueryCounter)(GLuint, GLenum);
    void(APIENTRY* GetQueryObjectui64v)(GLuint, GLenum, GLuint64*);
    void(APIENTRY* GetQueryObjectiv)(GLuint, GLenum, GLint*);
    void(APIENTRY* GetIntegeri_v)(GLenum, GLuint, GLint*);
    void(APIENTRY* GetInteger64i_v)(GLenum, GLuint, GLint64*);
    void*(APIENTRY* MapBufferRange)(GLenum, GLintptr, GLsizeiptr, GLbitfield);
    GLboolean(APIENTRY* UnmapBuffer)(GLenum);
    void(APIENTRY* BindImageTexture)(GLuint, GLuint, GLint, GLboolean, GLint, GLenum, GLenum);

    bool load() {
        bool ok = true;
        auto get = [&](auto& fn, const char* name) {
            PROC p = wglGetProcAddress(name);
            if (!p || p == reinterpret_cast<PROC>(1) || p == reinterpret_cast<PROC>(2) || p == reinterpret_cast<PROC>(3) ||
                p == reinterpret_cast<PROC>(-1)) {
                ok = false;
                return;
            }
            fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(p);
        };
        get(CreateShader, "glCreateShader");
        get(ShaderSource, "glShaderSource");
        get(CompileShader, "glCompileShader");
        get(GetShaderiv, "glGetShaderiv");
        get(GetShaderInfoLog, "glGetShaderInfoLog");
        get(CreateProgram, "glCreateProgram");
        get(AttachShader, "glAttachShader");
        get(LinkProgram, "glLinkProgram");
        get(GetProgramiv, "glGetProgramiv");
        get(DeleteShader, "glDeleteShader");
        get(DeleteProgram, "glDeleteProgram");
        get(UseProgram, "glUseProgram");
        get(IsProgram, "glIsProgram");
        get(DispatchCompute, "glDispatchCompute");
        get(MemoryBarrier, "glMemoryBarrier");
        get(BindBufferBase, "glBindBufferBase");
        get(BindBufferRange, "glBindBufferRange");
        get(GenBuffers, "glGenBuffers");
        get(DeleteBuffers, "glDeleteBuffers");
        get(BindBuffer, "glBindBuffer");
        get(BufferData, "glBufferData");
        get(BufferSubData, "glBufferSubData");
        get(GetBufferSubData, "glGetBufferSubData");
        get(GetBufferParameteriv, "glGetBufferParameteriv");
        get(GenQueries, "glGenQueries");
        get(DeleteQueries, "glDeleteQueries");
        get(QueryCounter, "glQueryCounter");
        get(GetQueryObjectui64v, "glGetQueryObjectui64v");
        get(GetQueryObjectiv, "glGetQueryObjectiv");
        get(GetIntegeri_v, "glGetIntegeri_v");
        get(GetInteger64i_v, "glGetInteger64i_v");
        get(MapBufferRange, "glMapBufferRange");
        get(UnmapBuffer, "glUnmapBuffer");
        get(BindImageTexture, "glBindImageTexture");
        return ok;
    }
};

// The bindings a run touches (uniform block 0, shader storage 1-6, 9 and 10), the program and the
// generic bindings, kept and put back (the renderer owns the context).
struct GlKept {
    struct Indexed {
        GLint name = 0;
        GLint64 start = 0, size = 0;
    };
    static constexpr GLuint kStorages = 11;
    GLint program = 0, storage = 0, uniform = 0, copyRead = 0, copyWrite = 0;
    Indexed storages[kStorages], uniform0;

    void save(const Gl& gl) {
        glGetIntegerv(kCurrentProgram, &program);
        glGetIntegerv(kShaderStorageBinding, &storage);
        glGetIntegerv(kUniformBinding, &uniform);
        glGetIntegerv(kCopyReadBuffer, &copyRead);
        glGetIntegerv(kCopyWriteBuffer, &copyWrite);
        for (GLuint i = 0; i < kStorages; ++i) indexed(gl, kShaderStorageBinding, kShaderStorageStart, kShaderStorageSize, i, &storages[i]);
        indexed(gl, kUniformBinding, kUniformStart, kUniformSize, 0, &uniform0);
    }
    void restore(const Gl& gl) const {
        gl.UseProgram(static_cast<GLuint>(program));
        for (GLuint i = 0; i < kStorages; ++i) bind(gl, kShaderStorageBuffer, i, storages[i]);
        bind(gl, kUniformBuffer, 0, uniform0);
        gl.BindBuffer(kShaderStorageBuffer, static_cast<GLuint>(storage));
        gl.BindBuffer(kUniformBuffer, static_cast<GLuint>(uniform));
        gl.BindBuffer(kCopyReadBuffer, static_cast<GLuint>(copyRead));
        gl.BindBuffer(kCopyWriteBuffer, static_cast<GLuint>(copyWrite));
    }
    static void indexed(const Gl& gl, GLenum binding, GLenum start, GLenum size, GLuint i, Indexed* out) {
        gl.GetIntegeri_v(binding, i, &out->name);
        gl.GetInteger64i_v(start, i, &out->start);
        gl.GetInteger64i_v(size, i, &out->size);
    }
    static void bind(const Gl& gl, GLenum target, GLuint i, const Indexed& b) {
        if (b.name && b.size > 0) {
            gl.BindBufferRange(target, i, static_cast<GLuint>(b.name), static_cast<GLintptr>(b.start), static_cast<GLsizeiptr>(b.size));
        } else {
            gl.BindBufferBase(target, i, static_cast<GLuint>(b.name));
        }
    }
};

GLuint glName(void* target) { return static_cast<GLuint>(reinterpret_cast<std::uintptr_t>(target)); }

class GLLineFrames final : public LineFrames {
public:
    GLLineFrames() {
        context_ = wglGetCurrentContext();
        if (!context_) throw std::runtime_error("GPU frames: no OpenGL context current");
        GLint major = 0, minor = 0;
        glGetIntegerv(kMajorVersion, &major);
        glGetIntegerv(kMinorVersion, &minor);
        if (major < 4 || (major == 4 && minor < 5)) throw std::runtime_error("GPU frames: OpenGL 4.5 needed");
        if (!gl_.load()) throw std::runtime_error("GPU frames: OpenGL compute functions missing");
        programs_[0] = program(kLookGlslFramesCS, "FramesCS");
        programs_[1] = program(kLookGlslWidthsCS, "WidthsCS");
        programs_[2] = program(kLookGlslRibbonsCS, "RibbonsCS");
        programs_[3] = program(kLookGlslRibbonLayoutCS, "RibbonLayoutCS");
        programs_[4] = program(kLookGlslRibbonColorsCS, "RibbonColorsCS");
        programs_[5] = program(kLookGlslFollowCS, "FollowCS");
        density_[0] = program(kLookGlslDensityClearCS, "DensityClearCS");
        density_[1] = program(kLookGlslDensitySplatCS, "DensitySplatCS");
        density_[2] = program(kLookGlslDensityResolveCS, "DensityResolveCS");
        gl_.GenQueries(2, queries_);
    }
    ~GLLineFrames() override {
        if (!wglGetCurrentContext()) return;  // (gone with the context)
        for (GLuint p : programs_) {
            if (p) gl_.DeleteProgram(p);
        }
        for (GLuint p : density_) {
            if (p) gl_.DeleteProgram(p);
        }
        if (grid_.name) gl_.DeleteBuffers(1, &grid_.name);
        for (Named& b : buffers_) {
            if (b.name) gl_.DeleteBuffers(1, &b.name);
        }
        if (constants_.name) gl_.DeleteBuffers(1, &constants_.name);
        if (context_ == wglGetCurrentContext()) gl_.DeleteQueries(2, queries_);
    }

    GpuApi api() const override { return GpuApi::OpenGL; }
    void* device() const override { return context_; }
    bool reachable() const override { return wglGetCurrentContext() && programs_[0] && gl_.IsProgram(programs_[0]); }

    void* beginWrite(void*, std::size_t, std::size_t) override { return nullptr; }
    void endWrite() override {}

    void write(void* target, const void* data, std::size_t offset, std::size_t bytes) override {
        if (!target || !bytes) return;
        requireReachable();
        GLint copyWrite = 0;
        glGetIntegerv(kCopyWriteBuffer, &copyWrite);
        gl_.BindBuffer(kCopyWriteBuffer, glName(target));
        gl_.BufferSubData(kCopyWriteBuffer, static_cast<GLintptr>(offset), static_cast<GLsizeiptr>(bytes), data);
        gl_.BindBuffer(kCopyWriteBuffer, static_cast<GLuint>(copyWrite));
    }

    double gpuMs(bool wait) override {
        if (!pending_ || context_ != wglGetCurrentContext()) return pending_ ? -1.0 : lastMs_;
        GLint available = 0;
        gl_.GetQueryObjectiv(queries_[1], kQueryResultAvailable, &available);
        if (!available && !wait) return -1.0;
        GLuint64 t[2] = {};
        for (int i = 0; i < 2; ++i) gl_.GetQueryObjectui64v(queries_[i], kQueryResult, &t[i]);
        pending_ = false;
        lastMs_ = static_cast<double>(t[1] - t[0]) / 1e6;
        return lastMs_;
    }

    std::vector<float> read(void* target, std::size_t count) override {
        std::vector<float> out(count);
        if (!target || !count) return out;
        requireReachable();
        GlKept kept;
        kept.save(gl_);
        gl_.MemoryBarrier(kBufferUpdateBarrier);
        gl_.BindBuffer(kCopyReadBuffer, glName(target));
        gl_.GetBufferSubData(kCopyReadBuffer, 0, static_cast<GLsizeiptr>(count * 4), out.data());
        kept.restore(gl_);
        return out;
    }

protected:
    std::vector<std::uint32_t> readDensityGrid(std::size_t cells) override {
        std::vector<std::uint32_t> out(cells);
        if (!grid_.name || cells == 0 || cells * 4 > grid_.bytes) return out;
        const std::vector<float> raw = read(reinterpret_cast<void*>(static_cast<std::uintptr_t>(grid_.name)), cells);
        std::memcpy(out.data(), raw.data(), cells * 4);
        return out;
    }

    void upload(Buffer which, const void* data, std::size_t bytes) override {
        requireReachable();
        GlKept kept;
        kept.save(gl_);
        put(buffers_[which], data, bytes, kStaticDraw);
        kept.restore(gl_);
    }

    void dispatch(Kernel kernel, const void* constants, std::size_t constantBytes, const Binding& binding, std::uint32_t groupsX,
                  std::uint32_t groupsY) override {
        requireReachable();
        const bool framed = kernel == Kernel::Frames || kernel == Kernel::Ribbons;  // the root normals read
        const bool follow = kernel == Kernel::Follow;  // the follow buffers read, the root normals written
        if (!buffers_[kOffsets].name || ((framed || follow) && !buffers_[kRootNormals].name)) throw std::runtime_error("GPU frames: no layout or root normals");
        if (follow && (!buffers_[kFollowRoots].name || !buffers_[kFollowRest].name || !buffers_[kFollowScalpRest].name || !buffers_[kFollowScalpNow].name)) {
            throw std::runtime_error("GPU follow: the strands at rest, their roots or the scalps are missing");
        }
        if (kernel == Kernel::Widths && !buffers_[kWidths].name) throw std::runtime_error("GPU frames: no widths");
        const bool colored = kernel == Kernel::RibbonColors || (kernel == Kernel::Ribbons && binding.out[2] == nullptr);
        if (colored && !buffers_[kRibbonColors].name) throw std::runtime_error("GPU ribbons: no colors");
        const std::size_t points = pointCount();
        GlKept kept;
        kept.save(gl_);
        const std::pair<void*, std::size_t> targets[5] = {{binding.in, points * 12}, {binding.lineUvs, points * 8},
                                                          {binding.out[0], binding.outBytes[0]}, {binding.out[1], binding.outBytes[1]},
                                                          {binding.out[2], binding.outBytes[2]}};
        for (const auto& [target, bytes] : targets) {
            if (!target) continue;
            gl_.BindBuffer(kCopyWriteBuffer, glName(target));
            GLint size = 0;
            gl_.GetBufferParameteriv(kCopyWriteBuffer, kBufferSize, &size);
            if (static_cast<std::size_t>(size) < bytes) {
                kept.restore(gl_);
                throw std::runtime_error("GPU frames: a target buffer is smaller than the points");
            }
        }
        put(constants_, constants, constantBytes, kDynamicDraw);
        const bool timed = (kernel == Kernel::Frames || kernel == Kernel::Ribbons) && context_ == wglGetCurrentContext();
        if (timed) gl_.QueryCounter(queries_[0], kTimestamp);
        gl_.UseProgram(programs_[static_cast<int>(kernel)]);
        gl_.BindBufferRange(kUniformBuffer, 0, constants_.name, 0, static_cast<GLsizeiptr>(constantBytes));
        // look.slang's bindings: 1 gOffsets, 2 gStrandData, 3 gIn, 4-6 gOut-gOut3, 9 gAux0, 10 gAux1.
        gl_.BindBufferBase(kShaderStorageBuffer, 1, buffers_[kOffsets].name);
        if (framed) gl_.BindBufferBase(kShaderStorageBuffer, 2, buffers_[kRootNormals].name);
        if (kernel == Kernel::Widths) {
            gl_.BindBufferBase(kShaderStorageBuffer, 3, buffers_[kWidths].name);
        } else if (binding.in) {
            gl_.BindBufferBase(kShaderStorageBuffer, 3, glName(binding.in));
        }
        for (GLuint i = 0; i < 3; ++i) {
            if (binding.out[i]) gl_.BindBufferBase(kShaderStorageBuffer, 4 + i, glName(binding.out[i]));
        }
        if (colored) gl_.BindBufferBase(kShaderStorageBuffer, 9, buffers_[kRibbonColors].name);
        if (binding.lineUvs) gl_.BindBufferBase(kShaderStorageBuffer, 10, glName(binding.lineUvs));
        if (follow) {  // follow.slang: gStrandData the roots, gIn the points at rest, gAux0 and gAux1 the scalps, gOut2 the root normals
            gl_.BindBufferBase(kShaderStorageBuffer, 2, buffers_[kFollowRoots].name);
            gl_.BindBufferBase(kShaderStorageBuffer, 3, buffers_[kFollowRest].name);
            gl_.BindBufferBase(kShaderStorageBuffer, 9, buffers_[kFollowScalpRest].name);
            gl_.BindBufferBase(kShaderStorageBuffer, 10, buffers_[kFollowScalpNow].name);
            gl_.BindBufferBase(kShaderStorageBuffer, 5, buffers_[kRootNormals].name);
        }
        gl_.DispatchCompute(groupsX, groupsY, 1);
        // What the renderer draws next reads them as vertex attributes.
        gl_.MemoryBarrier(kVertexAttribArrayBarrier | kShaderStorageBarrier | kBufferUpdateBarrier);
        if (timed) {
            gl_.QueryCounter(queries_[1], kTimestamp);
            pending_ = true;
        }
        kept.restore(gl_);
    }

    void dispatchDensity(void* constants, void* positions, void* lineUvs, std::size_t points, std::size_t cells,
                         const std::uint32_t groups[3][2], void* volume) override {
        requireReachable();
        auto* c = static_cast<StageConstants*>(constants);
        GlKept kept;
        kept.save(gl_);
        for (const auto& [target, bytes] : {std::pair<void*, std::size_t>{positions, points * 12}, {lineUvs, points * 8}}) {
            gl_.BindBuffer(kCopyWriteBuffer, glName(target));
            GLint size = 0;
            gl_.GetBufferParameteriv(kCopyWriteBuffer, kBufferSize, &size);
            if (static_cast<std::size_t>(size) < bytes) {
                kept.restore(gl_);
                throw std::runtime_error("GPU density: a points buffer is smaller than the points");
            }
        }
        if (grid_.bytes < cells * 4 || grid_.bytes > 2 * cells * 4 + 64) put(grid_, nullptr, cells * 4, kDynamicDraw);
        const bool timed = context_ == wglGetCurrentContext();
        if (timed) gl_.QueryCounter(queries_[0], kTimestamp);
        gl_.BindBufferBase(kShaderStorageBuffer, 3, glName(positions));
        gl_.BindBufferBase(kShaderStorageBuffer, 10, glName(lineUvs));
        gl_.BindBufferBase(kShaderStorageBuffer, 4, grid_.name);
        constexpr GLenum kRg32f = 0x8230, kWriteOnly = 0x88B9;
        constexpr GLbitfield kImageBarrier = 0x00000020, kTextureFetchBarrier = 0x00000008;
        gl_.BindImageTexture(0, glName(volume), 0, 1, 0, kWriteOnly, kRg32f);
        for (int k = 0; k < 3; ++k) {
            c->dispatch[0] = groups[k][0] * kPointGroup;
            put(constants_, c, sizeof(StageConstants), kDynamicDraw);
            gl_.UseProgram(density_[k]);
            gl_.BindBufferRange(kUniformBuffer, 0, constants_.name, 0, static_cast<GLsizeiptr>(sizeof(StageConstants)));
            gl_.DispatchCompute(groups[k][0], groups[k][1], 1);
            gl_.MemoryBarrier(kShaderStorageBarrier);
        }
        gl_.MemoryBarrier(kImageBarrier | kTextureFetchBarrier);
        gl_.BindImageTexture(0, 0, 0, 0, 0, kWriteOnly, kRg32f);
        if (timed) {
            gl_.QueryCounter(queries_[1], kTimestamp);
            pending_ = true;
        }
        kept.restore(gl_);
    }

private:
    struct Named {
        GLuint name = 0;
        std::size_t bytes = 0;
    };
    GLuint density_[3] = {};
    Named grid_;
    void requireReachable() const {
        if (!reachable()) throw std::runtime_error("GPU frames: the OpenGL context they were made in is gone");
    }
    GLuint program(const char* source, const char* entry) {
        const GLuint shader = gl_.CreateShader(kComputeShader);
        gl_.ShaderSource(shader, 1, &source, nullptr);
        gl_.CompileShader(shader);
        const GLuint p = gl_.CreateProgram();
        gl_.AttachShader(p, shader);
        gl_.LinkProgram(p);
        GLint compiled = 0, linked = 0;
        gl_.GetShaderiv(shader, kCompileStatus, &compiled);
        gl_.GetProgramiv(p, kLinkStatus, &linked);
        std::string log;
        if (!compiled) {
            GLint length = 0;
            gl_.GetShaderiv(shader, kInfoLogLength, &length);
            log.assign(static_cast<std::size_t>(std::max(length, 1)), '\0');
            gl_.GetShaderInfoLog(shader, length, nullptr, log.data());
        }
        gl_.DeleteShader(shader);
        if (!compiled || !linked) {
            gl_.DeleteProgram(p);
            for (GLuint& q : programs_) {
                if (q) gl_.DeleteProgram(q);
                q = 0;
            }
            throw std::runtime_error("GPU frames: " + std::string(entry) + " did not " + (compiled ? std::string("link") : "compile: " + std::string(log.c_str())));
        }
        return p;
    }
    // Made (or made again for another size), then mapped with its contents invalidated (no wait on
    // a dispatch still reading the last; no new storage each time) and the data copied in, in
    // parallel chunks.
    void put(Named& b, const void* data, std::size_t size, GLenum usage) {
        const std::size_t bytes = std::max<std::size_t>((size + 3) & ~std::size_t{3}, 16);
        if (!b.name) gl_.GenBuffers(1, &b.name);
        gl_.BindBuffer(kCopyWriteBuffer, b.name);
        if (b.bytes < bytes || b.bytes > 2 * bytes) {  // (room to grow: other counts, a playback's, reuse it)
            const std::size_t room = (bytes + bytes / 8 + 3) & ~std::size_t{3};
            gl_.BufferData(kCopyWriteBuffer, static_cast<GLsizeiptr>(room), nullptr, usage);
            b.bytes = room;
        }
        if (!size || !data) return;
        void* to = gl_.MapBufferRange(kCopyWriteBuffer, 0, static_cast<GLsizeiptr>(size), kMapWrite | kMapInvalidateBuffer);
        if (!to) {
            gl_.BufferSubData(kCopyWriteBuffer, 0, static_cast<GLsizeiptr>(size), data);
            return;
        }
        constexpr std::size_t kChunk = std::size_t{1} << 20;
        parallelFor(0, (size + kChunk - 1) / kChunk, [&](std::size_t c) {
            const std::size_t at = c * kChunk;
            std::memcpy(static_cast<char*>(to) + at, static_cast<const char*>(data) + at, std::min(kChunk, size - at));
        }, 1);
        gl_.UnmapBuffer(kCopyWriteBuffer);
    }

    Gl gl_{};
    HGLRC context_ = nullptr;
    GLuint programs_[static_cast<int>(Kernel::Count)] = {};  // by Kernel: FramesCS, WidthsCS, RibbonsCS, RibbonLayoutCS, RibbonColorsCS, FollowCS
    Named buffers_[kBufferCount];
    Named constants_;
    GLuint queries_[2] = {};
    bool pending_ = false;
    double lastMs_ = -1.0;
};

}  // namespace

std::unique_ptr<LineFrames> makeD3D11LineFrames(ID3D11Device* device) { return std::make_unique<D3D11LineFrames>(device); }
std::unique_ptr<LineFrames> makeGLLineFrames() { return std::make_unique<GLLineFrames>(); }

ID3D11Buffer* createD3D11IndexBuffer(ID3D11Device* device, std::size_t bytes) {
    if (!device) throw std::runtime_error("GPU ribbons: no device");
    D3D11_BUFFER_DESC desc{};
    desc.ByteWidth = static_cast<UINT>(std::max<std::size_t>((bytes + 3) & ~std::size_t{3}, 4));
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_INDEX_BUFFER | D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
    ID3D11Buffer* buffer = nullptr;
    check(device->CreateBuffer(&desc, nullptr, &buffer), "CreateBuffer (indices)");
    return buffer;
}
#endif  // _WIN32

}  // namespace groomlab::gpu
