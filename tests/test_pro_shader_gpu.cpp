// SPDX-License-Identifier: GPL-2.0-or-later
// Execute translated guest FP16 operations on Vulkan and read the resulting color channels.
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/frontend/decode.h"
#include "shader_recompiler/frontend/translate/translate.h"
#include "shader_recompiler/ir/ir_emitter.h"
#include "shader_recompiler/ir/passes/ir_passes.h"
#include "video_core/host_shaders/fs_tri_vert.h"
#include "video_core/texture_cache/image.h"
#include "video_core/buffer_cache/buffer.h"

struct Case {
    const char* name;
    u32 opcode, select, source, previous;
    bool float_result;
    std::array<float, 2> expected;
    u32 source_operand = 262;
    bool clamp = false;
    u32 image_mask = 0;
    u32 result_reg = 9;
    bool image_half = true;
    u32 sdwa = 0;
    bool uint_result = false;
};

static std::vector<u32> Compile(const Case& test, const Shader::Profile& profile) {
    using namespace Shader;
    const bool mixed = test.opcode == 33 || test.opcode == 34;
    const std::array<u32, 2> code = test.sdwa ? std::array<u32, 2>{
        (test.opcode == 53 ? (53u << 25) | (7u << 9) : 0x7e000000u | (1u << 9)) |
            (9u << 17) | 249u,
        test.sdwa | (test.source_operand & 255u)} : test.image_mask ? std::array<u32, 2>{
        0xf0000000u | ((test.opcode ? test.opcode : 32u) << 18) | (test.image_mask << 8),
        6u | (9u << 8) | (u32(test.image_half) << 31)} : mixed ? std::array<u32, 2>{
        0xcc000000u | (test.opcode << 16) | (u32(test.clamp) << 15) | 9u,
        262u | (263u << 9) | (264u << 18)} : std::array<u32, 2>{
        0xd0000000u | ((test.opcode & 511u) << 17) | ((test.opcode >> 9) << 16) |
            (test.select << 12) | 9u,
        test.source_operand};
    Gcn::GcnCodeSlice slice(code.data(), code.data() + code.size());
    Gcn::GcnDecodeContext decoder;
    const auto inst = decoder.decodeInstruction(slice);
    if (!slice.atEnd() || (!test.image_mask && !test.sdwa &&
        (inst.src[0].op_sel.op_sel != bool(test.select & 1) ||
         inst.dst[0].op_sel.op_sel != bool(test.select & 8)))) {
        std::fputs("FAIL: conversion operand selection did not decode\n", stderr);
        return {};
    }
    Info info{};
    info.hw_stage = HwStage::Fragment;
    info.sw_stage = SwStage::Fragment;
    RuntimeInfo runtime{};
    runtime.Initialize(info.hw_stage, info.sw_stage);
    runtime.hw.fs.color_buffers[0].num_format = AmdGpu::NumberFormat::Float;
    Common::ObjectPool<IR::Inst> pool;
    IR::Block block(pool);
    Gcn::Translator translator(info, runtime, profile);
    translator.Translate(&block, 0, IR::Condition::True, {&inst, 1});
    if (info.translation_failed) {
        std::fputs("FAIL: conversion did not translate\n", stderr);
        return {};
    }
    IR::IREmitter initial(block, block.begin());
    initial.SetVectorReg(IR::VectorReg(6), initial.Imm32(test.source));
    initial.SetVectorReg(IR::VectorReg(7), initial.Imm32(2.f));
    initial.SetVectorReg(IR::VectorReg(8), initial.Imm32(2.f));
    initial.SetVectorReg(IR::VectorReg(9), initial.Imm32(test.previous));
    initial.SetVectorReg(IR::VectorReg(10), initial.Imm32(test.previous));
    initial.SetVectorReg(IR::VectorReg(11), initial.Imm32(test.previous));
    if (test.image_mask) {
        // Supply a known texel; exercise guest channel/register packing without a texture fixture.
        const auto texel = initial.CompositeConstruct(initial.Imm32(0.25f), initial.Imm32(0.5f),
                                                     initial.Imm32(0.75f), initial.Imm32(1.f));
        bool replaced = false;
        for (auto& op : block) {
            if (op.GetOpcode() == IR::Opcode::ImageSampleRaw) {
                op.ReplaceUsesWith(texel);
                replaced = true;
            }
        }
        if (!replaced) return {};
    }
    IR::IREmitter output(block);
    const auto result = output.GetVectorReg<IR::U32>(IR::VectorReg(test.result_reg));
    const auto halves = output.Unpack2x16(AmdGpu::NumberFormat::Float, result);
    for (u32 channel = 0; channel < 4; ++channel) {
        const IR::F32 value = channel < 2
            ? (test.uint_result
                ? (channel == 0 ? IR::F32{output.ConvertUToF(32, 32, result)} : output.Imm32(0.f))
                : test.float_result
                ? (channel == 0 ? output.BitCast<IR::F32>(result) : output.Imm32(0.f))
                : IR::F32{output.CompositeExtract(halves, channel)})
            : output.Imm32(channel == 3 ? 1.f : 0.f);
        info.stores.Set(IR::Attribute::RenderTarget0, channel);
        output.SetAttribute(IR::Attribute::RenderTarget0, value, channel);
    }
    output.Epilogue();
    IR::Program program(info);
    program.blocks.push_back(&block);
    program.post_order_blocks.push_back(&block);
    program.syntax_list.push_back({.data = {.block = &block},
                                   .type = IR::AbstractSyntaxNode::Type::Block});
    program.syntax_list.push_back({.type = IR::AbstractSyntaxNode::Type::Return});
    Optimization::SsaRewritePass(program);
    Optimization::ConstantPropagationPass(program.blocks);
    Optimization::DeadCodeEliminationPass(program);
    Optimization::CollectShaderInfoPass(program, profile);
    Backend::Bindings bindings{};
    return Backend::SPIRV::EmitSPIRV(profile, runtime, program, bindings);
}

int main(int argc, char** argv) {
    using namespace Vulkan;
    Instance instance(0, false);
    Scheduler scheduler(instance);
    static vk::detail::DynamicLoader loader;
    vk::detail::DispatchLoaderDynamic dispatch;
    dispatch.init(loader.getProcAddress<PFN_vkGetInstanceProcAddr>("vkGetInstanceProcAddr"));
    dispatch.init(instance.GetInstance());
    dispatch.init(instance.GetDevice());
    const auto device = instance.GetDevice();
    Shader::Profile profile{};
    profile.supported_spirv = 0x00010600;
    profile.support_int16 = instance.IsShaderInt16Supported();
    profile.support_float16 = instance.IsShaderFloat16Supported();
    profile.support_int64 = true;
    constexpr auto format = vk::Format::eR32G32B32A32Sfloat;
    VideoCore::UniqueImage target(device, instance.GetAllocator());
    target.Create({.imageType = vk::ImageType::e2D, .format = format,
        .extent = {1, 1, 1}, .mipLevels = 1, .arrayLayers = 1,
        .samples = vk::SampleCountFlagBits::e1, .tiling = vk::ImageTiling::eOptimal,
        .usage = vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eTransferSrc});
    auto view = device.createImageViewUnique({.image = vk::Image(target),
        .viewType = vk::ImageViewType::e2D, .format = format,
        .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1}}, nullptr, dispatch).value;
    VideoCore::Buffer download(instance, 0, 16, VideoCore::MemoryType::HostCached);
    auto vertex = device.createShaderModuleUnique({.codeSize = sizeof(FS_TRI_VERT),
        .pCode = FS_TRI_VERT}, nullptr, dispatch).value;
    auto layout = device.createPipelineLayoutUnique({}, nullptr, dispatch).value;
    const vk::PipelineVertexInputStateCreateInfo vertex_input{};
    const vk::PipelineInputAssemblyStateCreateInfo assembly{
        .topology = vk::PrimitiveTopology::eTriangleList};
    const vk::Viewport viewport{0, 0, 1, 1, 0, 1};
    const vk::Rect2D scissor{{0, 0}, {1, 1}};
    const vk::PipelineViewportStateCreateInfo viewport_state{
        .viewportCount = 1, .pViewports = &viewport, .scissorCount = 1, .pScissors = &scissor};
    const vk::PipelineRasterizationStateCreateInfo raster{
        .polygonMode = vk::PolygonMode::eFill, .cullMode = vk::CullModeFlagBits::eNone,
        .frontFace = vk::FrontFace::eCounterClockwise, .lineWidth = 1.f};
    const vk::PipelineMultisampleStateCreateInfo samples{
        .rasterizationSamples = vk::SampleCountFlagBits::e1};
    const vk::PipelineColorBlendAttachmentState color{
        .colorWriteMask = vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
                          vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA};
    const vk::PipelineColorBlendStateCreateInfo blend{.attachmentCount = 1, .pAttachments = &color};
    const vk::PipelineRenderingCreateInfo rendering{.colorAttachmentCount = 1,
                                                     .pColorAttachmentFormats = &format};
    const std::array cases{
        Case{"half to float, low", 395, 0, 0x3a003400, 0, true, {0.25f, 0.f}},
        Case{"half to float, high", 395, 1, 0x3a003400, 0, true, {0.75f, 0.f}},
        Case{"float to half, low", 394, 0, 0x3f400000, 0x38003400, false, {0.75f, 0.f}},
        Case{"float to half, high", 394, 8, 0x3f400000, 0x38003400, false, {0.25f, 0.75f}},
        Case{"integer bits to half value", 395, 0, 0, 0, true,
             {0.000003814697265625f, 0.f}, 192},
        Case{"mixed low unclamped", 33, 0, 0x40000000, 0x38003400, false, {6.f, 0.5f}},
        Case{"mixed high unclamped", 34, 0, 0x40000000, 0x38003400, false, {0.25f, 6.f}},
        Case{"mixed low clamp maximum", 33, 0, 0x40000000, 0x38003400, false,
             {1.f, 0.5f}, 262, true},
        Case{"mixed high clamp maximum", 34, 0, 0x40000000, 0x38003400, false,
             {0.25f, 1.f}, 262, true},
        Case{"mixed low clamp minimum", 33, 0, 0xc0000000, 0x38003400, false,
             {0.f, 0.5f}, 262, true},
        Case{"mixed high clamp minimum", 34, 0, 0xc0000000, 0x38003400, false,
             {0.25f, 0.f}, 262, true},
        Case{.name="sample D16 red/green", .expected={0.25f, 0.5f}, .image_mask=15},
        Case{.name="sample D16 blue/alpha", .expected={0.75f, 1.f}, .image_mask=15,
             .result_reg=10},
        Case{.name="sample D16 sparse red/alpha", .expected={0.25f, 1.f}, .image_mask=9},
        Case{.name="sample D16 odd blue", .expected={0.75f, 0.f}, .image_mask=7,
             .result_reg=10},
        Case{.name="sample D16 preserves following register", .previous=0x38003400,
             .expected={0.25f, 0.5f}, .image_mask=3, .result_reg=10},
        Case{.name="sample F32 red", .float_result=true, .expected={0.25f, 0.f},
             .image_mask=15, .image_half=false},
        Case{.name="sample F32 green", .float_result=true, .expected={0.5f, 0.f},
             .image_mask=15, .result_reg=10, .image_half=false},
        Case{.name="gather D16 first pair", .opcode=95, .expected={0.25f, 0.5f},
             .image_mask=1},
        Case{.name="gather D16 second pair", .opcode=95, .expected={0.75f, 1.f},
             .image_mask=1, .result_reg=10},
        Case{.name="gather D16 preserves following register", .opcode=95,
             .previous=0x38003400, .expected={0.25f, 0.5f}, .image_mask=1, .result_reg=11},
        Case{.name="gather F32 second value", .opcode=95, .float_result=true,
             .expected={0.5f, 0.f}, .image_mask=1, .result_reg=10, .image_half=false},
        Case{.name="SDWA preserves blue while clearing alpha", .previous=0x3c003a00,
             .expected={0.75f, 0.f}, .sdwa=(6u << 16) | (2u << 11) | (5u << 8)},
        Case{.name="SDWA low half preserve", .source=0x3a00, .previous=0x38003400,
             .expected={0.75f, 0.5f}, .sdwa=(6u << 16) | (2u << 11) | (4u << 8)},
        Case{.name="SDWA high half preserve", .source=0x3a00, .previous=0x38003400,
             .expected={0.25f, 0.75f}, .sdwa=(6u << 16) | (2u << 11) | (5u << 8)},
        Case{.name="SDWA high half pad", .source=0x3a00, .previous=0x38003400,
             .expected={0.f, 0.75f}, .sdwa=(6u << 16) | (5u << 8)},
        Case{.name="SDWA byte preserve", .source=0x56, .previous=0x1234abcd,
             .expected={float(0x1256abcd), 0.f}, .sdwa=(6u << 16) | (2u << 11) | (2u << 8),
             .uint_result=true},
        Case{.name="SDWA byte pad", .source=0x56, .previous=0x1234abcd,
             .expected={float(0x00560000), 0.f}, .sdwa=(6u << 16) | (2u << 8),
             .uint_result=true},
        Case{.name="SDWA byte sign extend", .source=0x80, .previous=0x1234abcd,
             .expected={float(0xffff8000), 0.f}, .sdwa=(6u << 16) | (1u << 11) | (1u << 8),
             .uint_result=true},
        Case{.name="SDWA move high source", .source=0x3a003400,
             .expected={0.75f, 0.f}, .sdwa=(5u << 16) | (6u << 8)},
        Case{.name="SDWA byte source", .source=0x00560000,
             .expected={86.f, 0.f}, .sdwa=(2u << 16) | (6u << 8), .uint_result=true},
        Case{.name="SDWA signed byte source", .source=0x00800000,
             .expected={float(0xffffff80), 0.f},
             .sdwa=(2u << 16) | (1u << 19) | (6u << 8), .uint_result=true},
        Case{.name="SDWA half multiply high sources/low result", .opcode=53,
             .source=0x3a003400, .previous=0x38003400, .expected={1.5f, 0.5f},
             .sdwa=(5u << 24) | (5u << 16) | (2u << 11) | (4u << 8)},
        Case{.name="SDWA half multiply high sources/high result", .opcode=53,
             .source=0x3a003400, .previous=0x38003400, .expected={0.25f, 1.5f},
             .sdwa=(5u << 24) | (5u << 16) | (2u << 11) | (5u << 8)},
        Case{.name="SDWA half multiply full result", .opcode=53,
             .source=0x3a003400, .previous=0x38003400, .expected={1.5f, 0.f},
             .sdwa=(5u << 24) | (5u << 16) | (6u << 8)},
    };
    unsigned failures = 0;
    unsigned index = 0;
    unsigned executed = 0;
    for (bool native_half : {false, true}) {
        if (native_half && !instance.IsShaderFloat16Supported()) continue;
        profile.support_float16 = native_half;
        for (const auto& test : cases) {
            const auto code = Compile(test, profile);
            if (code.empty()) return 1;
            if (argc > 1) {
                const std::filesystem::path directory(argv[1]);
                std::filesystem::create_directories(directory);
                std::ofstream file(directory / ("case-" + std::to_string(index++) + ".spv"),
                                   std::ios::binary);
                file.write(reinterpret_cast<const char*>(code.data()), code.size() * sizeof(u32));
                if (!file) return 1;
                continue;
            }
            auto fragment = device.createShaderModuleUnique({.codeSize = code.size() * sizeof(u32),
                .pCode = code.data()}, nullptr, dispatch).value;
            const std::array stages{
                vk::PipelineShaderStageCreateInfo{.stage = vk::ShaderStageFlagBits::eVertex,
                    .module = *vertex, .pName = "main"},
                vk::PipelineShaderStageCreateInfo{.stage = vk::ShaderStageFlagBits::eFragment,
                    .module = *fragment, .pName = "main"}};
            auto pipeline = device.createGraphicsPipelineUnique({}, {
                .pNext = &rendering, .stageCount = u32(stages.size()), .pStages = stages.data(),
                .pVertexInputState = &vertex_input, .pInputAssemblyState = &assembly,
                .pViewportState = &viewport_state, .pRasterizationState = &raster,
                .pMultisampleState = &samples, .pColorBlendState = &blend,
                .layout = *layout}, nullptr, dispatch).value;
            const auto cmd = scheduler.CommandBuffer();
            vk::ImageMemoryBarrier2 barrier{
                .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
                .dstStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
                .dstAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite,
                .oldLayout = vk::ImageLayout::eUndefined,
                .newLayout = vk::ImageLayout::eColorAttachmentOptimal,
                .image = vk::Image(target),
                .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1}};
            cmd.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &barrier}, dispatch);
            const vk::RenderingAttachmentInfo attachment{.imageView = *view,
                .imageLayout = vk::ImageLayout::eColorAttachmentOptimal,
                .loadOp = vk::AttachmentLoadOp::eClear, .storeOp = vk::AttachmentStoreOp::eStore};
            cmd.beginRendering({.renderArea = scissor, .layerCount = 1,
                .colorAttachmentCount = 1, .pColorAttachments = &attachment}, dispatch);
            cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, *pipeline, dispatch);
            cmd.draw(3, 1, 0, 0, dispatch);
            cmd.endRendering(dispatch);
            barrier.srcStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput;
            barrier.srcAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite;
            barrier.dstStageMask = vk::PipelineStageFlagBits2::eTransfer;
            barrier.dstAccessMask = vk::AccessFlagBits2::eTransferRead;
            barrier.oldLayout = vk::ImageLayout::eColorAttachmentOptimal;
            barrier.newLayout = vk::ImageLayout::eTransferSrcOptimal;
            cmd.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &barrier}, dispatch);
            const vk::BufferImageCopy copy{.imageSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
                                           .imageExtent = {1, 1, 1}};
            cmd.copyImageToBuffer(vk::Image(target), vk::ImageLayout::eTransferSrcOptimal,
                                 download.Handle(), copy, dispatch);
            const vk::MemoryBarrier2 host{.srcStageMask = vk::PipelineStageFlagBits2::eTransfer,
                .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
                .dstStageMask = vk::PipelineStageFlagBits2::eHost,
                .dstAccessMask = vk::AccessFlagBits2::eHostRead};
            cmd.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &host}, dispatch);
            scheduler.Finish();
            download.Invalidate(0, 16);
            std::array<float, 4> actual;
            std::memcpy(actual.data(), download.mapped_data.data(), sizeof(actual));
            ++executed;
            for (u32 channel = 0; channel < 2; ++channel) {
                if (!std::isfinite(actual[channel]) ||
                    std::abs(actual[channel] - test.expected[channel]) > (test.uint_result ? 0.f :
                        1e-10f + std::abs(test.expected[channel]) * 0.0001f)) {
                    std::fprintf(stderr, "FAIL: %s native=%u channel %u: %g, expected %g\n",
                                 test.name, unsigned(native_half), channel,
                                 actual[channel], test.expected[channel]);
                    ++failures;
                }
            }
        }
    }
    if (argc > 1) {
        std::puts("EMITTED: FP16 shaders for spirv-val");
        return 0;
    }
    if (failures) return 1;
    std::printf("PASS: translated FP16 operations on Vulkan (%u cases)\n", executed);
}
