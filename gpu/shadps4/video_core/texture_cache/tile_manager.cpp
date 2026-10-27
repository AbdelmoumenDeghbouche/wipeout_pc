// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_runtime.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"
#include "video_core/texture_cache/image.h"
#include "video_core/texture_cache/image_info.h"
#include "video_core/texture_cache/image_view.h"
#include "video_core/texture_cache/tile_manager.h"

#include "video_core/host_shaders/tiling_macro_128_comp.h"
#include "video_core/host_shaders/tiling_macro_16_comp.h"
#include "video_core/host_shaders/tiling_macro_32_comp.h"
#include "video_core/host_shaders/tiling_macro_64_comp.h"
#include "video_core/host_shaders/tiling_macro_8_comp.h"
#include "video_core/host_shaders/tiling_macro_96_comp.h"
#include "video_core/host_shaders/tiling_micro_128_comp.h"
#include "video_core/host_shaders/tiling_micro_16_comp.h"
#include "video_core/host_shaders/tiling_micro_32_comp.h"
#include "video_core/host_shaders/tiling_micro_64_comp.h"
#include "video_core/host_shaders/tiling_micro_8_comp.h"
#include "video_core/host_shaders/tiling_micro_96_comp.h"

#include <algorithm>
#include <bit>
#include <boost/container/small_vector.hpp>
#include <magic_enum/magic_enum.hpp>
#include <vk_mem_alloc.h>

namespace VideoCore {

struct TilingInfo {
    u32 bank_swizzle;
    u32 num_slices;
    u32 num_mips;
    std::array<ImageInfo::MipInfo, 16> mips;
};

namespace {

// gow3: tiling.comp's address math on the CPU, for one texel of sample 0. Kept in step with
// the shader (same names and cases) so both put a texel at the same tiled address.
struct TexelTiling {
    u32 bits_per_pixel;
    AmdGpu::MicroTileMode micro_tile_mode;
    u32 thickness;
    bool is_macro;
    AmdGpu::ArrayMode array_mode;
    u32 pipe_config;
    u32 bank_width;
    u32 bank_height;
    u32 num_banks;
    u32 num_bank_bits;
    u32 tile_split_bytes;
    u32 macro_tile_aspect;
    u32 bank_swizzle;
};

constexpr u32 MicroTileWidth = 8;
constexpr u32 MicroTileHeight = 8;
constexpr u32 NumPipeInterleaveBits = 8;

constexpr u32 Bit(u32 value, u32 bit) {
    return (value >> bit) & 1;
}

u32 PixelIndexWithinMicroTile(const TexelTiling& t, u32 x, u32 y, u32 z) {
    const u32 x0 = Bit(x, 0), x1 = Bit(x, 1), x2 = Bit(x, 2);
    const u32 y0 = Bit(y, 0), y1 = Bit(y, 1), y2 = Bit(y, 2);
    const u32 z0 = Bit(z, 0), z1 = Bit(z, 1), z2 = Bit(z, 2);
    u32 p0 = 0, p1 = 0, p2 = 0, p3 = 0, p4 = 0, p5 = 0, p6 = 0, p7 = 0, p8 = 0;
    if (t.micro_tile_mode == AmdGpu::MicroTileMode::Display) {
        switch (t.bits_per_pixel) {
        case 8:
            p0 = x0, p1 = x1, p2 = x2, p3 = y1, p4 = y0, p5 = y2;
            break;
        case 16:
            p0 = x0, p1 = x1, p2 = x2, p3 = y0, p4 = y1, p5 = y2;
            break;
        case 32:
            p0 = x0, p1 = x1, p2 = y0, p3 = x2, p4 = y1, p5 = y2;
            break;
        case 64:
            p0 = x0, p1 = y0, p2 = x1, p3 = x2, p4 = y1, p5 = y2;
            break;
        case 128:
            p0 = y0, p1 = x0, p2 = x1, p3 = x2, p4 = y1, p5 = y2;
            break;
        }
    } else if (t.micro_tile_mode == AmdGpu::MicroTileMode::Thin ||
               t.micro_tile_mode == AmdGpu::MicroTileMode::Depth) {
        p0 = x0, p1 = y0, p2 = x1, p3 = y1, p4 = x2, p5 = y2;
    } else {
        if (t.bits_per_pixel == 8 || t.bits_per_pixel == 16) {
            p0 = x0, p1 = y0, p2 = x1, p3 = y1, p4 = z0, p5 = z1;
        } else if (t.bits_per_pixel == 32) {
            p0 = x0, p1 = y0, p2 = x1, p3 = z0, p4 = y1, p5 = z1;
        } else {
            p0 = x0, p1 = y0, p2 = z0, p3 = x1, p4 = y1, p5 = z1;
        }
        p6 = x2, p7 = y2;
        if (t.thickness == 8) {
            p8 = z2;
        }
    }
    return p0 | (p1 << 1) | (p2 << 2) | (p3 << 3) | (p4 << 4) | (p5 << 5) | (p6 << 6) |
           (p7 << 7) | (p8 << 8);
}

u32 MicroTiledAddress(const TexelTiling& t, u32 x, u32 y, u32 slice, u32 pitch, u32 height) {
    const u32 micro_tile_bytes = MicroTileWidth * MicroTileHeight * t.thickness * t.bits_per_pixel / 8;
    const u32 slice_bytes = (pitch * height * t.thickness * t.bits_per_pixel + 7) / 8;
    const u32 micro_tiles_per_row = pitch / MicroTileWidth;
    const u32 slice_offset = (slice / t.thickness) * slice_bytes;
    const u32 micro_tile_offset =
        ((y / MicroTileHeight) * micro_tiles_per_row + x / MicroTileWidth) * micro_tile_bytes;
    const u32 pixel_offset = PixelIndexWithinMicroTile(t, x, y, slice) * t.bits_per_pixel;
    return slice_offset + micro_tile_offset + pixel_offset / 8;
}

u32 PipeFromCoord(const TexelTiling& t, u32 num_pipes, u32 x, u32 y, u32 slice) {
    const u32 tx = x / MicroTileWidth;
    const u32 ty = y / MicroTileHeight;
    const u32 x3 = Bit(tx, 0), x4 = Bit(tx, 1), x5 = Bit(tx, 2), x6 = Bit(tx, 3);
    const u32 y3 = Bit(ty, 0), y4 = Bit(ty, 1), y5 = Bit(ty, 2), y6 = Bit(ty, 3);
    u32 p0 = 0, p1 = 0, p2 = 0, p3 = 0;
    if (t.pipe_config == u32(AmdGpu::PipeConfig::P2)) {
        p0 = x3 ^ y3;
    } else if (t.pipe_config == u32(AmdGpu::PipeConfig::P8_32x32_8x16)) {
        p0 = x4 ^ y3 ^ x5;
        p1 = x3 ^ y4;
        p2 = x5 ^ y5;
    } else if (t.pipe_config == u32(AmdGpu::PipeConfig::P8_32x32_16x16)) {
        p0 = x3 ^ y3 ^ x4;
        p1 = x4 ^ y4;
        p2 = x5 ^ y5;
    } else if (t.pipe_config == u32(AmdGpu::PipeConfig::P16_32x32_8x16)) {
        // PS4 Pro (#33).
        p0 = x4 ^ y3;
        p1 = x3 ^ y4;
        p2 = x5 ^ y6;
        p3 = x6 ^ y5;
    }
    u32 pipe = p0 | (p1 << 1) | (p2 << 2) | (p3 << 3);
    u32 pipe_swizzle = 0;
    if (t.array_mode == AmdGpu::ArrayMode::Array3DTiledThin1 ||
        t.array_mode == AmdGpu::ArrayMode::Array3DTiledThick ||
        t.array_mode == AmdGpu::ArrayMode::Array3DTiledXThick) {
        pipe_swizzle += std::max(1u, num_pipes / 2 - 1) * (slice / t.thickness);
    }
    pipe_swizzle &= num_pipes - 1;
    return pipe ^ pipe_swizzle;
}

u32 BankFromCoord(const TexelTiling& t, u32 num_pipes, u32 x, u32 y, u32 slice,
                  u32 tile_split_slice) {
    const u32 tx = x / MicroTileWidth / (t.bank_width * num_pipes);
    const u32 ty = y / MicroTileHeight / t.bank_height;
    const u32 x3 = Bit(tx, 0), x4 = Bit(tx, 1), x5 = Bit(tx, 2), x6 = Bit(tx, 3);
    const u32 y3 = Bit(ty, 0), y4 = Bit(ty, 1), y5 = Bit(ty, 2), y6 = Bit(ty, 3);
    u32 b0 = 0, b1 = 0, b2 = 0, b3 = 0;
    if (t.num_banks == 16) {
        b0 = x3 ^ y6;
        b1 = x4 ^ y5 ^ y6;
        b2 = x5 ^ y4;
        b3 = x6 ^ y3;
    } else if (t.num_banks == 8) {
        b0 = x3 ^ y5;
        b1 = x4 ^ y4 ^ y5;
        b2 = x5 ^ y3;
    } else if (t.num_banks == 4) {
        b0 = x3 ^ y4;
        b1 = x4 ^ y3;
    } else if (t.num_banks == 2) {
        b0 = x3 ^ y3;
    }
    u32 bank = b0 | (b1 << 1) | (b2 << 2) | (b3 << 3);

    using enum AmdGpu::ArrayMode;
    const auto mode = t.array_mode;
    u32 slice_rotation = 0;
    if (mode == Array2DTiledThin1 || mode == Array2DTiledThick || mode == Array2DTiledXThick) {
        slice_rotation = (t.num_banks / 2 - 1) * (slice / t.thickness);
    } else if (mode == Array3DTiledThin1 || mode == Array3DTiledThick ||
               mode == Array3DTiledXThick) {
        slice_rotation = std::max(1u, num_pipes / 2 - 1) * (slice / t.thickness) / num_pipes;
    }
    u32 tile_split_rotation = 0;
    if (mode == Array2DTiledThin1 || mode == Array3DTiledThin1 || mode == ArrayPrt2DTiledThin1 ||
        mode == ArrayPrt3DTiledThin1) {
        tile_split_rotation = (t.num_banks / 2 + 1) * tile_split_slice;
    }
    bank ^= t.bank_swizzle + slice_rotation;
    bank ^= tile_split_rotation;
    return bank & (t.num_banks - 1);
}

u32 MacroTiledAddress(const TexelTiling& t, u32 x, u32 y, u32 slice, u32 pitch, u32 height) {
    const bool p2 = t.pipe_config == u32(AmdGpu::PipeConfig::P2);
    const bool p16 = t.pipe_config == u32(AmdGpu::PipeConfig::P16_32x32_8x16);
    const u32 num_pipes = p2 ? 2 : (p16 ? 16 : 8);
    const u32 num_pipe_bits = p2 ? 1 : (p16 ? 4 : 3);
    const u32 pixel_offset = PixelIndexWithinMicroTile(t, x, y, slice) * t.bits_per_pixel;
    u32 element_offset = pixel_offset / 8;

    u32 micro_tile_bytes = MicroTileWidth * MicroTileHeight * t.thickness * t.bits_per_pixel / 8;
    u32 slices_per_tile = 1;
    u32 tile_split_slice = 0;
    if (micro_tile_bytes > t.tile_split_bytes && t.thickness == 1) {
        slices_per_tile = micro_tile_bytes / t.tile_split_bytes;
        tile_split_slice = element_offset / t.tile_split_bytes;
        element_offset %= t.tile_split_bytes;
        micro_tile_bytes = t.tile_split_bytes;
    }

    const u32 macro_tile_pitch = MicroTileWidth * t.bank_width * num_pipes * t.macro_tile_aspect;
    const u32 macro_tile_height = MicroTileHeight * t.bank_height * t.num_banks / t.macro_tile_aspect;
    const u32 macro_tile_bytes = micro_tile_bytes * (macro_tile_pitch / MicroTileWidth) *
                                 (macro_tile_height / MicroTileHeight) / (num_pipes * t.num_banks);
    const u32 macro_tiles_per_row = pitch / macro_tile_pitch;
    const u32 macro_tile_offset =
        ((y / macro_tile_height) * macro_tiles_per_row + x / macro_tile_pitch) * macro_tile_bytes;
    const u32 macro_tiles_per_slice = macro_tiles_per_row * (height / macro_tile_height);
    const u32 slice_bytes = macro_tiles_per_slice * macro_tile_bytes;
    const u32 slice_offset =
        slice_bytes * (tile_split_slice + slices_per_tile * (slice / t.thickness));

    const u32 tile_row_index = (y / MicroTileHeight) % t.bank_height;
    const u32 tile_column_index = ((x / MicroTileWidth) / num_pipes) % t.bank_width;
    const u32 tile_offset = (tile_row_index * t.bank_width + tile_column_index) * micro_tile_bytes;
    const u32 total_offset = slice_offset + macro_tile_offset + element_offset + tile_offset;

    if (AmdGpu::IsPrt(t.array_mode)) {
        x %= macro_tile_pitch;
        y %= macro_tile_height;
    }
    const u32 pipe = PipeFromCoord(t, num_pipes, x, y, slice);
    const u32 bank = BankFromCoord(t, num_pipes, x, y, slice, tile_split_slice);

    const u32 pipe_interleave_mask = (1u << NumPipeInterleaveBits) - 1;
    u32 addr = total_offset & pipe_interleave_mask;
    addr |= pipe << NumPipeInterleaveBits;
    addr |= bank << (NumPipeInterleaveBits + num_pipe_bits);
    addr |= (total_offset >> NumPipeInterleaveBits)
            << (NumPipeInterleaveBits + num_pipe_bits + t.num_bank_bits);
    return addr;
}

} // Anonymous namespace

TileManager::TileManager(const Vulkan::Instance& instance_, Vulkan::Scheduler& scheduler_,
                         Vulkan::Runtime& runtime_, StreamBuffer& stream_buffer_)
    : instance{instance_}, scheduler{scheduler_}, runtime{runtime_}, stream_buffer{stream_buffer_} {
    const auto device = instance.GetDevice();
    const std::array<vk::DescriptorSetLayoutBinding, 3> bindings = {{
        {
            .binding = 0,
            .descriptorType = vk::DescriptorType::eStorageBuffer,
            .descriptorCount = 1,
            .stageFlags = vk::ShaderStageFlagBits::eCompute,
        },
        {
            .binding = 1,
            .descriptorType = vk::DescriptorType::eStorageBuffer,
            .descriptorCount = 1,
            .stageFlags = vk::ShaderStageFlagBits::eCompute,
        },
        {
            .binding = 2,
            .descriptorType = vk::DescriptorType::eUniformBuffer,
            .descriptorCount = 1,
            .stageFlags = vk::ShaderStageFlagBits::eCompute,
        },
    }};

    const vk::DescriptorSetLayoutCreateInfo desc_layout_ci = {
        .flags = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR,
        .bindingCount = static_cast<u32>(bindings.size()),
        .pBindings = bindings.data(),
    };
    auto desc_layout_result = device.createDescriptorSetLayoutUnique(desc_layout_ci);
    ASSERT_MSG(desc_layout_result.result == vk::Result::eSuccess,
               "Failed to create descriptor set layout: {}",
               vk::to_string(desc_layout_result.result));
    desc_layout = std::move(desc_layout_result.value);

    const vk::DescriptorSetLayout set_layout = *desc_layout;
    const vk::PipelineLayoutCreateInfo layout_info = {
        .setLayoutCount = 1U,
        .pSetLayouts = &set_layout,
        .pushConstantRangeCount = 0U,
        .pPushConstantRanges = nullptr,
    };
    auto [layout_result, layout] = device.createPipelineLayoutUnique(layout_info);
    ASSERT_MSG(layout_result == vk::Result::eSuccess, "Failed to create pipeline layout: {}",
               vk::to_string(layout_result));
    pl_layout = std::move(layout);
}

TileManager::~TileManager() = default;

vk::Pipeline TileManager::GetTilingPipeline(const ImageInfo& info, bool is_tiler) {
    const TilingKey key{
        .tile_mode = info.tile_mode,
        .num_bits = info.num_bits,
        .num_samples = info.num_samples,
        .is_tiler = is_tiler,
        .alt_tile = info.alt_tile,
    };
    if (const auto it = tiling_pipelines.find(key); it != tiling_pipelines.end()) {
        return *it->second;
    }

    const auto device = instance.GetDevice();
    const auto micro_tile_mode = AmdGpu::GetMicroTileMode(info.tile_mode);
    const bool is_macro = AmdGpu::IsMacroTiled(info.array_mode);
    std::array<u32, 12> spec_data{
        info.num_samples,
        u32(micro_tile_mode),
        AmdGpu::GetMicroTileThickness(info.array_mode),
        u32(is_tiler),
    };
    if (is_macro) {
        const auto macro_tile_mode =
            AmdGpu::CalculateMacrotileMode(info.tile_mode, info.num_bits, info.num_samples);
        spec_data[4] = u32(info.array_mode);
        spec_data[5] = u32(info.alt_tile ? AmdGpu::GetAltPipeConfig(info.tile_mode)
                                       : AmdGpu::GetPipeConfig(info.tile_mode));
        spec_data[6] = AmdGpu::GetBankWidth(macro_tile_mode);
        spec_data[7] = info.alt_tile ? AmdGpu::GetAltBankHeight(macro_tile_mode)
                                    : AmdGpu::GetBankHeight(macro_tile_mode);
        spec_data[8] = info.alt_tile ? AmdGpu::GetAltNumBanks(macro_tile_mode)
                                    : AmdGpu::GetNumBanks(macro_tile_mode);
        spec_data[9] = std::bit_width(spec_data[8]) - 1;
        spec_data[10] = AmdGpu::CalculateTileSplit(info.tile_mode, info.array_mode, micro_tile_mode,
                                                   info.num_bits);
        spec_data[11] = info.alt_tile ? AmdGpu::GetAltMacrotileAspect(macro_tile_mode)
                                     : AmdGpu::GetMacrotileAspect(macro_tile_mode);
    }

    std::span<const u32> code;
    switch (info.num_bits) {
    case 8:
        code = is_macro ? std::span<const u32>{TILING_MACRO_8_COMP}
                        : std::span<const u32>{TILING_MICRO_8_COMP};
        break;
    case 16:
        code = is_macro ? std::span<const u32>{TILING_MACRO_16_COMP}
                        : std::span<const u32>{TILING_MICRO_16_COMP};
        break;
    case 32:
        code = is_macro ? std::span<const u32>{TILING_MACRO_32_COMP}
                        : std::span<const u32>{TILING_MICRO_32_COMP};
        break;
    case 64:
        code = is_macro ? std::span<const u32>{TILING_MACRO_64_COMP}
                        : std::span<const u32>{TILING_MICRO_64_COMP};
        break;
    case 96:
        code = is_macro ? std::span<const u32>{TILING_MACRO_96_COMP}
                        : std::span<const u32>{TILING_MICRO_96_COMP};
        break;
    case 128:
        code = is_macro ? std::span<const u32>{TILING_MACRO_128_COMP}
                        : std::span<const u32>{TILING_MICRO_128_COMP};
        break;
    default:
        UNREACHABLE_MSG("Unsupported tiling pixel width {}", info.num_bits);
    }

    const auto module = Vulkan::CompileSPV(code, device);
    static constexpr auto spec_entries = [] {
        std::array<vk::SpecializationMapEntry, 12> entries{};
        for (u32 i = 0; i < entries.size(); ++i) {
            entries[i] = vk::SpecializationMapEntry{i, i * u32(sizeof(u32)), sizeof(u32)};
        }
        return entries;
    }();

    const u32 spec_count = is_macro ? 12 : 4;
    const vk::SpecializationInfo specialization{
        .mapEntryCount = spec_count,
        .pMapEntries = spec_entries.data(),
        .dataSize = spec_count * sizeof(u32),
        .pData = spec_data.data(),
    };

    const auto module_name = fmt::format("{}_{} {}", magic_enum::enum_name(info.tile_mode),
                                         info.num_bits, is_tiler ? "tiler" : "detiler");
    LOG_INFO(Render_Vulkan, "Creating tiling pipeline {}", module_name);
    Vulkan::SetObjectName(device, module, module_name);

    const vk::PipelineShaderStageCreateInfo shader_ci = {
        .stage = vk::ShaderStageFlagBits::eCompute,
        .module = module,
        .pName = "main",
        .pSpecializationInfo = &specialization,
    };
    const vk::ComputePipelineCreateInfo compute_pipeline_ci = {
        .stage = shader_ci,
        .layout = *pl_layout,
    };

    auto [result, pipeline] =
        device.createComputePipelineUnique(VK_NULL_HANDLE, compute_pipeline_ci);
    ASSERT_MSG(result == vk::Result::eSuccess, "Detiler pipeline creation failed {}",
               vk::to_string(result));
    const auto handle = *pipeline;
    tiling_pipelines.emplace(key, std::move(pipeline));
    device.destroyShaderModule(module);
    return handle;
}

void TileManager::RecordTilingDispatch(vk::Pipeline pipeline, const vk::DescriptorBufferInfo& tiled,
                                       const vk::DescriptorBufferInfo& linear,
                                       const vk::DescriptorBufferInfo& params, u32 dim_x) {
    scheduler.Record([pipeline, layout = *pl_layout, tiled, linear, params,
                      dim_x](vk::CommandBuffer cmdbuf) {
        if (Vulkan::Scheduler::FullBarriers()) { // gow3: GOW3_FULL_BARRIERS=1
            Vulkan::Scheduler::RecordFullBarrier(cmdbuf);
        }
        cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, pipeline);
        const std::array<vk::WriteDescriptorSet, 3> set_writes = {{
            {
                .dstBinding = 0,
                .descriptorCount = 1,
                .descriptorType = vk::DescriptorType::eStorageBuffer,
                .pBufferInfo = &tiled,
            },
            {
                .dstBinding = 1,
                .descriptorCount = 1,
                .descriptorType = vk::DescriptorType::eStorageBuffer,
                .pBufferInfo = &linear,
            },
            {
                .dstBinding = 2,
                .descriptorCount = 1,
                .descriptorType = vk::DescriptorType::eUniformBuffer,
                .pBufferInfo = &params,
            },
        }};
        cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, layout, 0, set_writes);
        cmdbuf.dispatch(dim_x, 1, 1);
        if (Vulkan::Scheduler::FullBarriers()) {
            Vulkan::Scheduler::RecordFullBarrier(cmdbuf);
        }
    });
}

std::pair<const Buffer*, u64> TileManager::DetileImage(const VideoCore::Buffer* in_buffer,
                                                       u64 in_offset, const ImageInfo& info) {
    if (!info.props.is_tiled) {
        return {in_buffer, in_offset};
    }

    TilingInfo params{};
    params.bank_swizzle = info.bank_swizzle;
    params.num_slices = info.props.is_volume ? info.size.depth : info.resources.layers;
    params.num_mips = info.resources.levels;
    for (u32 mip = 0; mip < params.num_mips; ++mip) {
        auto& mip_info = params.mips[mip];
        mip_info = info.mips_layout[mip];
        if (info.props.is_block) {
            mip_info.pitch = std::max((mip_info.pitch + 3) / 4, 1U);
            mip_info.height = std::max((mip_info.height + 3) / 4, 1U);
        }
    }

    const vk::DescriptorBufferInfo params_buffer_info{
        .buffer = stream_buffer.Handle(),
        .offset = stream_buffer.Copy(&params, sizeof(params), instance.UniformMinAlignment()),
        .range = sizeof(params),
    };

    // gow3: detiled uploads up to DetileScratchSize share one device buffer. From the staging
    // pool, each upload of a track load kept its own device buffer until the GPU finished the
    // submission: about 1 GB of video memory on top of the new textures in a 2048 race (#72,
    // #59). The copy into the image marks its read (UploadImage), FlushBarriers orders the
    // next write after it, and the barrier below does so after the previous submission's copies.
    constexpr u64 DetileScratchSize = 64_MB;
    Vulkan::StagingBufferRef staging;
    const bool scratch = info.guest_size <= DetileScratchSize;
    if (scratch) {
        if (!detile_scratch) {
            detile_scratch = std::make_unique<VideoCore::Buffer>(
                instance, 0, DetileScratchSize, MemoryType::DeviceLocal, "TileManager:DetileScratch");
        }
        staging = {.buffer = detile_scratch.get(), .size = info.guest_size, .type = MemoryType::DeviceLocal};
    } else {
        staging = runtime.GetStagingPool().Request(info.guest_size, MemoryType::DeviceLocal);
    }

    scheduler.EndRendering();
    runtime.FlushBarriers();
    if (scratch) {
        scheduler.Record([](vk::CommandBuffer cmdbuf) {
            const vk::MemoryBarrier2 barrier{
                .srcStageMask = vk::PipelineStageFlagBits2::eAllTransfer,
                .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
            };
            cmdbuf.pipelineBarrier2(vk::DependencyInfo{.memoryBarrierCount = 1, .pMemoryBarriers = &barrier});
        });
    }

    const vk::DescriptorBufferInfo tiled_buffer_info{
        .buffer = in_buffer->Handle(),
        .offset = in_offset,
        .range = info.guest_size,
    };
    const vk::DescriptorBufferInfo linear_buffer_info{
        .buffer = staging.buffer->Handle(),
        .offset = staging.offset,
        .range = info.guest_size,
    };
    const auto dim_x = static_cast<u32>((info.guest_size / (info.num_bits / 8)) / 64);
    // gow3: recorded with copies of the descriptor infos (threaded recording).
    RecordTilingDispatch(GetTilingPipeline(info, false), tiled_buffer_info, linear_buffer_info,
                         params_buffer_info, dim_x);

    runtime.AccessBuffer(staging.buffer, staging.offset, info.guest_size,
                         vk::PipelineStageFlagBits2::eComputeShader,
                         vk::AccessFlagBits2::eShaderWrite);

    return {staging.buffer, staging.offset};
}

// gow3: WipEout reads tiny tiled render targets back every frame (its auto-exposure chain:
// 1x1 to 8x8, about 35 per frame). Through the tiling shader each one added a staging copy,
// a barrier and a dispatch in the middle of the frame, and races ran ~18% slower than before
// the shader was used for them. Up to this many texels, each texel is copied to its tiled
// address instead, in the one copy the download needs anyway.
static constexpr u32 MaxTexelCopies = 64;

bool TileManager::TileSmallImage(Image& in_image, std::span<vk::BufferImageCopy> buffer_copies,
                                 const VideoCore::Buffer* out_buffer, u64 out_offset) {
    const auto& info = in_image.info;
    const u32 bpp = info.num_bits;
    if (buffer_copies.size() != 1 || info.num_samples != 1 || info.props.is_block ||
        info.props.is_volume || (bpp != 8 && bpp != 16 && bpp != 32 && bpp != 64 && bpp != 128)) {
        return false;
    }
    const auto& copy = buffer_copies[0];
    const u32 width = copy.imageExtent.width;
    const u32 height = copy.imageExtent.height;
    const u32 layers = copy.imageSubresource.layerCount;
    if (copy.imageExtent.depth != 1 || width * height * layers > MaxTexelCopies) {
        return false;
    }
    const auto micro_tile_mode = AmdGpu::GetMicroTileMode(info.tile_mode);
    if (micro_tile_mode == AmdGpu::MicroTileMode::Thick) {
        return false;
    }

    TexelTiling tiling{
        .bits_per_pixel = bpp,
        .micro_tile_mode = micro_tile_mode,
        .thickness = AmdGpu::GetMicroTileThickness(info.array_mode),
        .is_macro = AmdGpu::IsMacroTiled(info.array_mode),
        .array_mode = info.array_mode,
        .bank_swizzle = info.bank_swizzle,
    };
    if (tiling.is_macro) {
        const auto macro_tile_mode =
            AmdGpu::CalculateMacrotileMode(info.tile_mode, info.num_bits, info.num_samples);
        tiling.pipe_config = u32(AmdGpu::GetPipeConfig(info.tile_mode));
        tiling.bank_width = AmdGpu::GetBankWidth(macro_tile_mode);
        tiling.bank_height = AmdGpu::GetBankHeight(macro_tile_mode);
        tiling.num_banks = AmdGpu::GetNumBanks(macro_tile_mode);
        tiling.num_bank_bits = std::bit_width(tiling.num_banks) - 1;
        tiling.tile_split_bytes = AmdGpu::CalculateTileSplit(info.tile_mode, info.array_mode,
                                                             micro_tile_mode, info.num_bits);
        tiling.macro_tile_aspect = AmdGpu::GetMacrotileAspect(macro_tile_mode);
    }

    const auto& mip = info.mips_layout[0];
    boost::container::small_vector<vk::BufferImageCopy, MaxTexelCopies> texel_copies;
    for (u32 layer = 0; layer < layers; ++layer) {
        for (u32 y = 0; y < height; ++y) {
            for (u32 x = 0; x < width; ++x) {
                const u32 address =
                    tiling.is_macro ? MacroTiledAddress(tiling, x, y, layer, mip.pitch, mip.height)
                                    : MicroTiledAddress(tiling, x, y, layer, mip.pitch, mip.height);
                texel_copies.push_back(vk::BufferImageCopy{
                    .bufferOffset = out_offset + mip.offset + address,
                    .bufferRowLength = 0,
                    .bufferImageHeight = 0,
                    .imageSubresource{
                        .aspectMask = copy.imageSubresource.aspectMask,
                        .mipLevel = 0,
                        .baseArrayLayer = copy.imageSubresource.baseArrayLayer + layer,
                        .layerCount = 1,
                    },
                    .imageOffset = {s32(x), s32(y), 0},
                    .imageExtent = {1, 1, 1},
                });
            }
        }
    }
    runtime.DownloadImage(&in_image, out_buffer,
                          std::span{texel_copies.data(), texel_copies.size()});
    return true;
}

void TileManager::TileImage(Image& in_image, std::span<vk::BufferImageCopy> buffer_copies,
                            const VideoCore::Buffer* out_buffer, u64 out_offset) {
    const auto& info = in_image.info;
    if (!info.props.is_tiled) {
        for (auto& copy : buffer_copies) {
            copy.bufferOffset += out_offset;
        }
        runtime.DownloadImage(&in_image, out_buffer, buffer_copies);
        return;
    }
    if (TileSmallImage(in_image, buffer_copies, out_buffer, out_offset)) {
        return;
    }

    TilingInfo params{};
    params.bank_swizzle = info.bank_swizzle;
    params.num_slices = info.props.is_volume ? info.size.depth : info.resources.layers;
    params.num_mips = static_cast<u32>(buffer_copies.size());
    for (u32 mip = 0; mip < params.num_mips; ++mip) {
        auto& mip_info = params.mips[mip];
        mip_info = info.mips_layout[mip];
        if (info.props.is_block) {
            mip_info.pitch = std::max((mip_info.pitch + 3) / 4, 1U);
            mip_info.height = std::max((mip_info.height + 3) / 4, 1U);
        }
    }

    const vk::DescriptorBufferInfo params_buffer_info{
        .buffer = stream_buffer.Handle(),
        .offset = stream_buffer.Copy(&params, sizeof(params), instance.UniformMinAlignment()),
        .range = sizeof(params),
    };

    const auto staging = runtime.GetStagingPool().Request(info.guest_size, MemoryType::DeviceLocal);
    for (auto& copy : buffer_copies) {
        copy.bufferOffset += staging.offset;
    }

    runtime.DownloadImage(&in_image, staging.buffer, buffer_copies);
    runtime.FlushBarriers();

    const vk::DescriptorBufferInfo tiled_buffer_info{
        .buffer = out_buffer->Handle(),
        .offset = out_offset,
        .range = info.guest_size,
    };
    const vk::DescriptorBufferInfo linear_buffer_info{
        .buffer = staging.buffer->Handle(),
        .offset = staging.offset,
        .range = info.guest_size,
    };
    const auto dim_x = static_cast<u32>((info.guest_size / (info.num_bits / 8)) / 64);
    // gow3: recorded with copies of the descriptor infos (threaded recording).
    RecordTilingDispatch(GetTilingPipeline(info, true), tiled_buffer_info, linear_buffer_info,
                         params_buffer_info, dim_x);

    runtime.AccessBuffer(out_buffer, out_offset, info.guest_size,
                         vk::PipelineStageFlagBits2::eComputeShader,
                         vk::AccessFlagBits2::eShaderWrite);
}

} // namespace VideoCore
