// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <cstddef>

#include <boost/container/static_vector.hpp>
#include <xxhash.h>

#include "shader_recompiler/frontend/fetch_shader.h"
#include "video_core/amdgpu/regs_color.h"
#include "video_core/amdgpu/regs_depth.h"
#include "video_core/amdgpu/regs_primitive.h"
#include "video_core/renderer_vulkan/vk_pipeline_common.h"

namespace VideoCore {
class BufferCache;
class TextureCache;
} // namespace VideoCore

namespace Vulkan {

static constexpr u32 MaxShaderStages = static_cast<u32>(Shader::SwStage::NumLogicalStages);
static constexpr u32 MaxVertexBufferCount = 32;

class Instance;
class Scheduler;
class DescriptorHeap;

template <typename T>
using VertexInputs = boost::container::static_vector<T, MaxVertexBufferCount>;

struct GraphicsPipelineKey {
    std::array<size_t, MaxShaderStages> stage_hashes;
    std::array<vk::Format, MaxVertexBufferCount> vertex_buffer_formats;
    u32 patch_control_points;
    u32 num_color_attachments;
    std::array<Shader::PsColorBuffer, AmdGpu::NUM_COLOR_BUFFERS> color_buffers;
    std::array<AmdGpu::BlendControl, AmdGpu::NUM_COLOR_BUFFERS> blend_controls;
    std::array<vk::ColorComponentFlags, AmdGpu::NUM_COLOR_BUFFERS> write_masks;
    AmdGpu::ColorBufferMask cb_shader_mask;
    AmdGpu::ColorControl::LogicOp logic_op;
    u8 num_samples;
    u8 depth_samples;
    std::array<u8, AmdGpu::NUM_COLOR_BUFFERS> color_samples;
    std::array<u8, 2> unused_gap; ///< gow3: always zero; named so that a copy carries it
    u32 mrt_mask;
    u32 motion_vectors = 0;
    struct {
        AmdGpu::DepthBuffer::ZFormat z_format : 2;
        AmdGpu::DepthBuffer::StencilFormat stencil_format : 1;
        u32 depth_clamp_enable : 1;
        u32 unused_depth_bits : 28; ///< gow3: always zero
    };
    struct {
        AmdGpu::PrimitiveType prim_type : 5;
        AmdGpu::PolygonMode polygon_mode : 2;
        AmdGpu::ClipSpace clip_space : 1;
        AmdGpu::ProvokingVtxLast provoking_vtx_last : 1;
        u32 depth_clip_enable : 1;
        u32 ps_iter_samples : 3; ///< gow3: DB_EQAA, log2 of the samples a pixel shader runs per pixel
        u32 unused_raster_bits : 19; ///< gow3: always zero
    };
    std::array<u8, 4> unused_tail; ///< gow3: always zero; without it bytes 348-351 are padding

    GraphicsPipelineKey() {
        std::memset(static_cast<void*>(this), 0, sizeof(*this));
    }

    // gow3: every copy is a memcpy of all 352 bytes (see the note below the struct).
    GraphicsPipelineKey(const GraphicsPipelineKey& other) noexcept {
        std::memcpy(static_cast<void*>(this), &other, sizeof(*this));
    }

    GraphicsPipelineKey& operator=(const GraphicsPipelineKey& other) noexcept {
        if (this != &other) {
            std::memcpy(static_cast<void*>(this), &other, sizeof(*this));
        }
        return *this;
    }

    bool operator==(const GraphicsPipelineKey& key) const noexcept {
        return std::memcmp(this, &key, sizeof(key)) == 0;
    }

    void Serialize(Serialization::Archive& ar) const;
    bool Deserialize(Serialization::Archive& ar);
};

// gow3: the key is hashed and compared as raw bytes, so a copy must carry every byte. The
// key's own gaps (bytes 330-331, the unused bits of the two bit-field words, the tail at
// 348-351) are named members, always zero, and the size and every offset are those of the
// saved key files. The element types Shader::PsColorBuffer and AmdGpu::BlendControl keep
// padding of their own, which no member names. An implicit copy would leave copying that
// padding to the compiler, so the copy operations memcpy all 352 bytes and it travels too
// (tests/test_pipeline_key_copy.cpp).
static_assert(sizeof(GraphicsPipelineKey) == 352);
// Each member starts where the one before it ends, at its offset in the saved key files, and
// the last one ends the struct. Bytes 340-347 are the two bit-field words, 32 named bits each.
static_assert([] {
    using K = GraphicsPipelineKey;
    constexpr size_t members[][3] = {
        // {offset in the files, offset, size}
        {0, offsetof(K, stage_hashes), sizeof(K::stage_hashes)},
        {48, offsetof(K, vertex_buffer_formats), sizeof(K::vertex_buffer_formats)},
        {176, offsetof(K, patch_control_points), sizeof(K::patch_control_points)},
        {180, offsetof(K, num_color_attachments), sizeof(K::num_color_attachments)},
        {184, offsetof(K, color_buffers), sizeof(K::color_buffers)},
        {248, offsetof(K, blend_controls), sizeof(K::blend_controls)},
        {280, offsetof(K, write_masks), sizeof(K::write_masks)},
        {312, offsetof(K, cb_shader_mask), sizeof(K::cb_shader_mask)},
        {316, offsetof(K, logic_op), sizeof(K::logic_op)},
        {320, offsetof(K, num_samples), sizeof(K::num_samples)},
        {321, offsetof(K, depth_samples), sizeof(K::depth_samples)},
        {322, offsetof(K, color_samples), sizeof(K::color_samples)},
        {330, offsetof(K, unused_gap), sizeof(K::unused_gap)},
        {332, offsetof(K, mrt_mask), sizeof(K::mrt_mask)},
        {336, offsetof(K, motion_vectors), sizeof(K::motion_vectors)},
        {340, 340, 2 * sizeof(u32)},
        {348, offsetof(K, unused_tail), sizeof(K::unused_tail)},
    };
    size_t end = 0;
    for (const auto& [file_offset, offset, size] : members) {
        if (offset != file_offset || offset != end) {
            return false;
        }
        end = offset + size;
    }
    return end == sizeof(K);
}());

class GraphicsPipeline : public Pipeline {
public:
    struct SerializationSupport {
        VertexInputs<vk::VertexInputAttributeDescription> vertex_attributes{};
        VertexInputs<vk::VertexInputBindingDescription> vertex_bindings{};
        VertexInputs<vk::VertexInputBindingDivisorDescriptionEXT> divisors{};
        vk::PipelineMultisampleStateCreateInfo multisampling{};
        std::vector<u32> tcs{};
        std::vector<u32> tes{};
        std::vector<u32> fragment{};

        void Serialize(Serialization::Archive& ar) const;
        bool Deserialize(Serialization::Archive& ar);
    };

    GraphicsPipeline(const Instance& instance, Scheduler& scheduler, DescriptorHeap& desc_heap,
                     const Shader::Profile& profile, const GraphicsPipelineKey& key,
                     vk::PipelineCache pipeline_cache,
                     std::span<const Shader::Info*, MaxShaderStages> stages,
                     std::span<const Shader::RuntimeInfo, MaxShaderStages> runtime_infos,
                     std::optional<const Shader::Gcn::FetchShaderData> fetch_shader,
                     std::span<const vk::ShaderModule> modules, SerializationSupport& sdata,
                     bool preloading);
    ~GraphicsPipeline();

    // gow3: switch from compiler-owned snapshots to live stage data on publication.
    void PublishStages(std::span<const Shader::Info*, MaxShaderStages> infos) {
        std::ranges::copy(infos, stages.begin());
    }

    const std::optional<const Shader::Gcn::FetchShaderData>& GetFetchShader() const noexcept {
        return fetch_shader;
    }

    const GraphicsPipelineKey& GetGraphicsKey() const {
        return key;
    }

    /// gow3: second draw squaring a self-scaled MIN/MAX blend, or null (vk_blend_rewrite.h).
    vk::Pipeline SquarePassHandle() const noexcept {
        return *square_pipeline;
    }

    /// Gets the attributes and bindings for vertex inputs.
    template <typename Attribute, typename Binding>
    void GetVertexInputs(VertexInputs<Attribute>& attributes, VertexInputs<Binding>& bindings,
                         VertexInputs<vk::VertexInputBindingDivisorDescriptionEXT>& divisors,
                         VertexInputs<AmdGpu::Buffer>& guest_buffers, u32 step_rate_0,
                         u32 step_rate_1) const;

private:
    void BuildDescSetLayout(bool preloading);

private:
    GraphicsPipelineKey key;
    std::optional<const Shader::Gcn::FetchShaderData> fetch_shader{};
    vk::UniquePipeline square_pipeline;
};

struct ClipDistanceShaderKey {
    std::array<std::tuple<u8, u8>, 8> clip_locations;

    bool operator==(const ClipDistanceShaderKey& key) const noexcept {
        return std::memcmp(this, &key, sizeof(key)) == 0;
    }
};

} // namespace Vulkan

template <>
struct std::hash<Vulkan::GraphicsPipelineKey> {
    std::size_t operator()(const Vulkan::GraphicsPipelineKey& key) const noexcept {
        return XXH3_64bits(&key, sizeof(key));
    }
};

template <>
struct std::hash<Vulkan::ClipDistanceShaderKey> {
    std::size_t operator()(const Vulkan::ClipDistanceShaderKey& key) const noexcept {
        return XXH3_64bits(&key, sizeof(key));
    }
};
