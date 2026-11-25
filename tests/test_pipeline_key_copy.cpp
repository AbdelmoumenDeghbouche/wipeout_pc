// SPDX-License-Identifier: GPL-2.0-or-later
// GraphicsPipelineKey copies keep every byte (vk_graphics_pipeline.h). The key is hashed and
// compared as raw bytes, so a copy that leaves padding as the destination held it gives the
// same key another hash. Each copy lands in storage filled with 0xAA first.
#include <cstdio>
#include <cstring>
#include <memory>
#include <new>
#include <type_traits>
#include <utility>
#include <vector>
#include "video_core/renderer_vulkan/vk_graphics_pipeline.h"

using Vulkan::GraphicsPipelineKey;

namespace {

// The warm-up's built record (vk_pipeline_serialization.cpp, BackgroundWarmup::Built).
struct Record {
    GraphicsPipelineKey key{};
    u64 other{};
    std::unique_ptr<int> owned;
};

// A holder with the key last, after a member with its own move constructor.
struct KeyLast {
    std::unique_ptr<int> owned;
    GraphicsPipelineKey key{};
};

// Every allocation starts filled with 0xAA, so a growing vector moves into dirty storage.
template <typename T>
struct FilledAllocator {
    using value_type = T;
    FilledAllocator() = default;
    template <typename U>
    FilledAllocator(const FilledAllocator<U>&) {}
    T* allocate(size_t n) {
        void* p = ::operator new(n * sizeof(T));
        std::memset(p, 0xAA, n * sizeof(T));
        return static_cast<T*>(p);
    }
    void deallocate(T* p, size_t) {
        ::operator delete(p);
    }
    bool operator==(const FilledAllocator&) const = default;
};

// The pipeline map's element type stays nothrow-movable, as before (robin_map's guarantee).
static_assert(std::is_nothrow_move_constructible_v<
              std::pair<GraphicsPipelineKey, std::unique_ptr<int>>>);
static_assert(std::is_nothrow_copy_constructible_v<GraphicsPipelineKey>);
static_assert(std::is_nothrow_copy_assignable_v<GraphicsPipelineKey>);

[[gnu::noinline]] const GraphicsPipelineKey* CopyConstruct(void* dst,
                                                           const GraphicsPipelineKey& src) {
    return ::new (dst) GraphicsPipelineKey(src);
}

[[gnu::noinline]] const GraphicsPipelineKey* AggregateMember(void* dst,
                                                             const GraphicsPipelineKey& src) {
    return &(::new (dst) Record{src, 1, {}})->key;
}

[[gnu::noinline]] const GraphicsPipelineKey* LambdaCapture(void* dst,
                                                           const GraphicsPipelineKey& src) {
    return (*::new (dst) auto([key = src] { return &key; }))();
}

[[gnu::noinline]] const GraphicsPipelineKey* Assign(GraphicsPipelineKey& dst,
                                                    const GraphicsPipelineKey& src) {
    dst = src;
    return &dst;
}

[[gnu::noinline]] const GraphicsPipelineKey* MoveAssign(GraphicsPipelineKey& dst,
                                                        GraphicsPipelineKey& src) {
    dst = std::move(src);
    return &dst;
}

// robin_map displaces entries with std::swap (move construction and two assignments).
[[gnu::noinline]] void Swap(GraphicsPipelineKey& a, GraphicsPipelineKey& b) {
    std::swap(a, b);
}

int failures = 0;

void Check(const char* what, const GraphicsPipelineKey* copy, const GraphicsPipelineKey& src) {
    const auto* a = reinterpret_cast<const unsigned char*>(copy);
    const auto* b = reinterpret_cast<const unsigned char*>(&src);
    int stale = 0;
    for (size_t i = 0; i < sizeof(GraphicsPipelineKey); ++i) {
        if (a[i] != b[i]) {
            if (stale++ == 0) {
                std::printf("FAIL %s: bytes differ at", what);
            }
            std::printf(" %zu", i);
        }
    }
    const bool same_hash =
        std::hash<GraphicsPipelineKey>{}(*copy) == std::hash<GraphicsPipelineKey>{}(src);
    if (stale) {
        std::printf(" (%d of %zu)\n", stale, sizeof(GraphicsPipelineKey));
    }
    if (!same_hash || !(*copy == src)) {
        std::printf("FAIL %s: hash or equality differs\n", what);
    }
    failures += stale != 0 || !same_hash || !(*copy == src);
}

// Built in place (parenthesised aggregate init), then moved by every reallocation into a
// new 0xAA-filled block: 1, 2, 4, 8 and 16 elements.
template <typename Holder>
[[gnu::noinline]] void GrowVector(const char* what, const GraphicsPipelineKey& src) {
    std::vector<Holder, FilledAllocator<Holder>> v;
    v.reserve(1);
    for (int i = 0; i < 9; ++i) {
        if constexpr (std::is_same_v<Holder, Record>) {
            v.emplace_back(src, u64(i), nullptr);
        } else {
            v.emplace_back(nullptr, src);
        }
    }
    if (v.capacity() < 9 || v.capacity() > 16) {
        std::printf("FAIL %s: capacity %zu\n", what, v.capacity());
        ++failures;
    }
    int bad = 0;
    const GraphicsPipelineKey* first = nullptr;
    for (const auto& h : v) {
        if (std::memcmp(&h.key, &src, sizeof(src)) != 0 && bad++ == 0) {
            first = &h.key;
        }
    }
    if (first) {
        std::printf("FAIL %s: %d of %zu elements differ, the first:\n", what, bad, v.size());
        Check(what, first, src);
    }
}

// Fill and copy in one function the optimiser sees whole (no noinline), through a local
// temporary. The source's elements have fields set and their own padding zero; each
// destination starts as 0xAA in every byte and must end byte-equal to the source.
void SameFunction() {
    GraphicsPipelineKey src;
    src.stage_hashes[1] = 0x1111222233334444ull;
    src.num_color_attachments = 2;
    src.color_buffers[1].data_format = AmdGpu::DataFormat(10);
    src.color_buffers[1].num_format = AmdGpu::NumberFormat(7);
    src.color_buffers[1].export_format = AmdGpu::ShaderExportFormat(9);
    src.color_buffers[1].swizzle = AmdGpu::IdentityMapping;
    src.blend_controls[1].enable = 1;
    src.blend_controls[1].color_dst_factor = AmdGpu::BlendControl::BlendFactor::OneMinusSrcAlpha;
    src.blend_controls[1].alpha_dst_factor = AmdGpu::BlendControl::BlendFactor::One;
    src.mrt_mask = 2;
    src.polygon_mode = AmdGpu::PolygonMode(2);

    alignas(Record) unsigned char out[sizeof(Record)];
    std::memset(out, 0xAA, sizeof(out));
    {
        const GraphicsPipelineKey tmp = src;
        Check("same function, construction from a temporary",
              ::new (static_cast<void*>(out)) GraphicsPipelineKey(tmp), src);
    }
    auto* dst = std::launder(reinterpret_cast<GraphicsPipelineKey*>(out));
    std::memset(out, 0xAA, sizeof(out));
    {
        GraphicsPipelineKey tmp = src;
        *dst = tmp;
        Check("same function, assignment from a temporary", dst, src);
    }
    std::memset(out, 0xAA, sizeof(out));
    {
        const GraphicsPipelineKey tmp = src;
        Check("same function, braced member from a temporary",
              &(::new (static_cast<void*>(out)) Record{tmp, 1, {}})->key, src);
        std::launder(reinterpret_cast<Record*>(out))->~Record();
    }
}

// No bit of the key is left unowned at the top level: with every member set to all ones (the
// element arrays as whole bytes), every byte reads 0xFF. A bit-field filler that is too short
// leaves a zero bit. Headers without the named fillers skip this leg.
template <typename K>
void AllBitsOwned() {
    if constexpr (requires(K& k) { k.unused_raster_bits = 0; }) {
        // Not a constant, so a bit-field takes its low bits without a truncation warning.
        volatile u32 all_ones = ~0u;
        const u32 ones = all_ones;
        K k;
        k.stage_hashes.fill(~size_t{0});
        std::memset(k.vertex_buffer_formats.data(), 0xFF, sizeof(k.vertex_buffer_formats));
        k.patch_control_points = ~0u;
        k.num_color_attachments = ~0u;
        std::memset(k.color_buffers.data(), 0xFF, sizeof(k.color_buffers));
        std::memset(k.blend_controls.data(), 0xFF, sizeof(k.blend_controls));
        for (auto& mask : k.write_masks) {
            mask = vk::ColorComponentFlags(~0u);
        }
        k.cb_shader_mask.raw = ~0u;
        k.logic_op = AmdGpu::ColorControl::LogicOp(~0u);
        k.num_samples = 0xFF;
        k.depth_samples = 0xFF;
        k.color_samples.fill(0xFF);
        k.unused_gap.fill(0xFF);
        k.mrt_mask = ~0u;
        k.motion_vectors = ~0u;
        k.z_format = AmdGpu::DepthBuffer::ZFormat(ones);
        k.stencil_format = AmdGpu::DepthBuffer::StencilFormat(ones);
        k.depth_clamp_enable = ones;
        k.unused_depth_bits = ones;
        k.prim_type = AmdGpu::PrimitiveType(ones);
        k.polygon_mode = AmdGpu::PolygonMode(ones);
        k.clip_space = AmdGpu::ClipSpace(ones);
        k.provoking_vtx_last = AmdGpu::ProvokingVtxLast(ones);
        k.depth_clip_enable = ones;
        k.ps_iter_samples = ones;
        k.unused_raster_bits = ones;
        k.unused_tail.fill(0xFF);
        const auto* b = reinterpret_cast<const unsigned char*>(&k);
        int unowned = 0;
        for (size_t i = 0; i < sizeof(K); ++i) {
            if (b[i] != 0xFF) {
                std::printf("FAIL unowned bits: byte %zu has 0x%02x unowned\n", i, 0xFF & ~b[i]);
                ++unowned;
            }
        }
        failures += unowned != 0;
    } else {
        std::printf("skipped: unowned bits (the header names no fillers)\n");
    }
}

} // namespace

int main() {
    // Every byte zero, padding included, except the fields set here.
    GraphicsPipelineKey src;
    src.stage_hashes[0] = 0x0123456789abcdefull;
    src.num_color_attachments = 1;
    src.num_samples = 1;
    src.depth_samples = 1;
    src.color_samples[0] = 1;
    src.mrt_mask = 1;
    src.prim_type = AmdGpu::PrimitiveType::TriangleList;
    src.depth_clip_enable = 1;

    alignas(Record) unsigned char buf[sizeof(Record)];
    const auto filled = [&] {
        std::memset(buf, 0xAA, sizeof(buf));
        return static_cast<void*>(buf);
    };
    Check("copy construction", CopyConstruct(filled(), src), src);
    Check("aggregate member construction", AggregateMember(filled(), src), src);
    Check("lambda capture", LambdaCapture(filled(), src), src);

    auto* dst = ::new (static_cast<void*>(buf)) GraphicsPipelineKey;
    std::memset(buf, 0xAA, sizeof(buf));
    Check("copy assignment", Assign(*dst, src), src);
    GraphicsPipelineKey moved = src;
    std::memset(buf, 0xAA, sizeof(buf));
    Check("move assignment", MoveAssign(*dst, moved), src);

    // std::swap: *dst holds 0xAA in every byte, `other` the source key.
    GraphicsPipelineKey dirty;
    std::memset(static_cast<void*>(&dirty), 0xAA, sizeof(dirty));
    GraphicsPipelineKey other = src;
    std::memset(buf, 0xAA, sizeof(buf));
    Swap(*dst, other);
    Check("std::swap, first", dst, src);
    Check("std::swap, second", &other, dirty);

    GrowVector<Record>("vector growth, key first", src);
    GrowVector<KeyLast>("vector growth, key last", src);
    SameFunction();
    AllBitsOwned<GraphicsPipelineKey>();

    if (failures) {
        return 1;
    }
    std::printf("PASS: pipeline key copy\n");
    return 0;
}
