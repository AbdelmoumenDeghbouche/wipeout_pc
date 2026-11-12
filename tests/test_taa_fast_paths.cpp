// SPDX-License-Identifier: GPL-2.0-or-later
// The TAA shortcuts (vk_temporal_upscaler.cpp RecordTaa) against the passes they replace, on the
// production SPIR-V over a run of frames with history: the scene color written back and the
// history written must be the same bits every frame. Plain Vulkan, without libgow3gpu (Lavapipe
// works: VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json). An optional argument is a SPIR-V
// file to use in place of taa_sharpen_scene_r11_comp.
//   old: taa_comp (switches pushed as 0), taa_sharpen_comp into RGBA16F, upscale_merge_r11_comp
//   resolve: taa_default_switches_comp in place of taa_comp
//   sharpen: taa.comp without its output (pad bit 31), taa_sharpen_scene_r11_comp into the scene
#include <vulkan/vulkan.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <span>
#include <vector>

#include "video_core/host_shaders/taa_comp.h"
#include "video_core/host_shaders/taa_default_switches_comp.h"
#include "video_core/host_shaders/taa_sharpen_comp.h"
#include "video_core/host_shaders/taa_sharpen_scene_r11_comp.h"
#include "video_core/host_shaders/upscale_merge_r11_comp.h"

namespace {

#define CHECK(call)                                                                                \
    do {                                                                                           \
        const VkResult result_ = (call);                                                           \
        if (result_ != VK_SUCCESS) {                                                               \
            std::fprintf(stderr, "%s failed: %d (line %d)\n", #call, result_, __LINE__);           \
            std::exit(1);                                                                          \
        }                                                                                          \
    } while (0)

constexpr uint32_t W = 263, H = 77, N = W * H; // odd sizes: partial work groups, edges
constexpr uint32_t FRAMES = 12;

VkDevice device;
VkPhysicalDevice physical;
VkQueue queue;
VkCommandPool pool;
VkCommandBuffer cmd;

uint32_t MemoryType(uint32_t bits, VkMemoryPropertyFlags flags) {
    VkPhysicalDeviceMemoryProperties properties;
    vkGetPhysicalDeviceMemoryProperties(physical, &properties);
    for (uint32_t i = 0; i < properties.memoryTypeCount; ++i) {
        if ((bits & (1u << i)) && (properties.memoryTypes[i].propertyFlags & flags) == flags) {
            return i;
        }
    }
    std::fprintf(stderr, "no memory type\n");
    std::exit(1);
}

struct Image {
    VkImage image;
    VkImageView view;
    VkDeviceMemory memory;
    uint32_t bytes; // per pixel
};

Image MakeImage(VkFormat format, uint32_t bytes, VkImageUsageFlags usage) {
    Image out{.bytes = bytes};
    const VkImageCreateInfo info{.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                                 .imageType = VK_IMAGE_TYPE_2D,
                                 .format = format,
                                 .extent = {W, H, 1},
                                 .mipLevels = 1,
                                 .arrayLayers = 1,
                                 .samples = VK_SAMPLE_COUNT_1_BIT,
                                 .tiling = VK_IMAGE_TILING_OPTIMAL,
                                 .usage = usage | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                          VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                 .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    CHECK(vkCreateImage(device, &info, nullptr, &out.image));
    VkMemoryRequirements requirements;
    vkGetImageMemoryRequirements(device, out.image, &requirements);
    const VkMemoryAllocateInfo alloc{
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = requirements.size,
        .memoryTypeIndex = MemoryType(requirements.memoryTypeBits,
                                      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
    CHECK(vkAllocateMemory(device, &alloc, nullptr, &out.memory));
    CHECK(vkBindImageMemory(device, out.image, out.memory, 0));
    const VkImageViewCreateInfo view{.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                                     .image = out.image,
                                     .viewType = VK_IMAGE_VIEW_TYPE_2D,
                                     .format = format,
                                     .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    CHECK(vkCreateImageView(device, &view, nullptr, &out.view));
    return out;
}

void Barrier() {
    const VkMemoryBarrier barrier{.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                                  .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
                                  .dstAccessMask =
                                      VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                         VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &barrier, 0, nullptr, 0,
                         nullptr);
}

struct Pipeline {
    VkDescriptorSetLayout set_layout;
    VkPipelineLayout layout;
    VkPipeline pipeline;
};

// Same layouts as CreatePipelines: `types` per binding, one push constant range.
Pipeline MakePipeline(std::span<const uint32_t> code, std::span<const VkDescriptorType> types,
                      uint32_t push_size) {
    Pipeline out{};
    std::vector<VkDescriptorSetLayoutBinding> bindings;
    for (uint32_t i = 0; i < types.size(); ++i) {
        bindings.push_back({i, types[i], 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr});
    }
    const VkDescriptorSetLayoutCreateInfo set_info{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = uint32_t(bindings.size()),
        .pBindings = bindings.data()};
    CHECK(vkCreateDescriptorSetLayout(device, &set_info, nullptr, &out.set_layout));
    const VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0, push_size};
    const VkPipelineLayoutCreateInfo layout_info{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1,
        .pSetLayouts = &out.set_layout,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &push};
    CHECK(vkCreatePipelineLayout(device, &layout_info, nullptr, &out.layout));
    const VkShaderModuleCreateInfo module_info{.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                                               .codeSize = code.size_bytes(),
                                               .pCode = code.data()};
    VkShaderModule module;
    CHECK(vkCreateShaderModule(device, &module_info, nullptr, &module));
    const VkComputePipelineCreateInfo info{
        .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                  .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                  .module = module,
                  .pName = "main"},
        .layout = out.layout};
    CHECK(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &info, nullptr, &out.pipeline));
    vkDestroyShaderModule(device, module, nullptr);
    return out;
}

VkDescriptorPool descriptor_pool;
VkSampler sampler;

void Dispatch(const Pipeline& p, std::span<const VkImageView> views, const void* push,
              uint32_t push_size, std::span<const VkDescriptorType> types) {
    VkDescriptorSet set;
    const VkDescriptorSetAllocateInfo alloc{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                            .descriptorPool = descriptor_pool,
                                            .descriptorSetCount = 1,
                                            .pSetLayouts = &p.set_layout};
    CHECK(vkAllocateDescriptorSets(device, &alloc, &set));
    std::vector<VkDescriptorImageInfo> infos(views.size());
    std::vector<VkWriteDescriptorSet> writes(views.size());
    for (uint32_t i = 0; i < views.size(); ++i) {
        infos[i] = {types[i] == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER ? sampler
                                                                           : VK_NULL_HANDLE,
                    views[i], VK_IMAGE_LAYOUT_GENERAL};
        writes[i] = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                     .dstSet = set,
                     .dstBinding = i,
                     .descriptorCount = 1,
                     .descriptorType = types[i],
                     .pImageInfo = &infos[i]};
    }
    vkUpdateDescriptorSets(device, uint32_t(writes.size()), writes.data(), 0, nullptr);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, p.pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, p.layout, 0, 1, &set, 0, nullptr);
    vkCmdPushConstants(cmd, p.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, push_size, push);
    vkCmdDispatch(cmd, (W + 7) / 8, (H + 7) / 8, 1);
    Barrier();
}

float Halton(uint32_t index, uint32_t base) {
    float f = 1, r = 0;
    for (; index; index /= base) {
        f /= float(base);
        r += f * float(index % base);
    }
    return r;
}

uint16_t ToHalf(float v) { // round to nearest even, normal and subnormal range
    uint32_t bits;
    std::memcpy(&bits, &v, 4);
    const uint32_t sign = (bits >> 16) & 0x8000;
    const float a = std::fabs(v);
    if (!(a < 65520.0f)) return uint16_t(sign | (a != a ? 0x7e00 : 0x7c00));
    if (a < std::ldexp(1.0f, -14)) {
        return uint16_t(sign | uint32_t(std::nearbyint(a * std::ldexp(1.0f, 24))));
    }
    int e;
    const float m = std::frexp(a, &e); // a = m * 2^e, m in [0.5, 1)
    uint32_t mant = uint32_t(std::nearbyint((m * 2 - 1) * 1024));
    uint32_t exp = uint32_t(e - 1 + 15);
    if (mant == 1024) {
        mant = 0;
        ++exp;
    }
    return uint16_t(sign | (exp << 10) | mant);
}

} // namespace

int main(int argc, char** argv) {
    const VkApplicationInfo app{.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
                                .apiVersion = VK_API_VERSION_1_3};
    const VkInstanceCreateInfo instance_info{.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
                                             .pApplicationInfo = &app};
    VkInstance instance;
    CHECK(vkCreateInstance(&instance_info, nullptr, &instance));
    uint32_t count = 1;
    if (vkEnumeratePhysicalDevices(instance, &count, &physical) < 0 || count == 0) {
        std::fprintf(stderr, "no Vulkan device\n");
        return 1;
    }
    VkPhysicalDeviceProperties properties;
    vkGetPhysicalDeviceProperties(physical, &properties);
    std::printf("TAA fast paths: device %s\n", properties.deviceName);
    VkFormatProperties3 r11{.sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_3};
    VkFormatProperties2 r11_properties{.sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2,
                                       .pNext = &r11};
    vkGetPhysicalDeviceFormatProperties2(physical, VK_FORMAT_B10G11R11_UFLOAT_PACK32,
                                         &r11_properties);
    const VkFormatFeatureFlags2 r11_needs =
        VK_FORMAT_FEATURE_2_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_2_STORAGE_WRITE_WITHOUT_FORMAT_BIT;
    if ((r11.optimalTilingFeatures & r11_needs) != r11_needs) {
        std::printf("TAA fast paths: B10G11R11 storage writes without format unsupported: SKIP\n");
        return 0;
    }
    VkPhysicalDeviceFeatures features{};
    features.shaderStorageImageExtendedFormats = VK_TRUE; // the merge's r11f_g11f_b10f and r8
    features.shaderStorageImageWriteWithoutFormat = VK_TRUE;
    const float priority = 1;
    const VkDeviceQueueCreateInfo queue_info{.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                             .queueFamilyIndex = 0,
                                             .queueCount = 1,
                                             .pQueuePriorities = &priority};
    const VkDeviceCreateInfo device_info{.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
                                         .queueCreateInfoCount = 1,
                                         .pQueueCreateInfos = &queue_info,
                                         .pEnabledFeatures = &features};
    CHECK(vkCreateDevice(physical, &device_info, nullptr, &device));
    vkGetDeviceQueue(device, 0, 0, &queue);
    const VkCommandPoolCreateInfo pool_info{
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT};
    CHECK(vkCreateCommandPool(device, &pool_info, nullptr, &pool));
    const VkCommandBufferAllocateInfo cmd_info{
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1};
    CHECK(vkAllocateCommandBuffers(device, &cmd_info, &cmd));
    const VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 4096},
                                          {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 8192},
                                          {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 4096}};
    const VkDescriptorPoolCreateInfo descriptor_info{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = 4096,
        .poolSizeCount = 3,
        .pPoolSizes = sizes};
    CHECK(vkCreateDescriptorPool(device, &descriptor_info, nullptr, &descriptor_pool));
    const VkSamplerCreateInfo sampler_info{.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
                                           .magFilter = VK_FILTER_NEAREST,
                                           .minFilter = VK_FILTER_NEAREST,
                                           .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
                                           .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                                           .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                                           .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE};
    CHECK(vkCreateSampler(device, &sampler_info, nullptr, &sampler));

    constexpr VkDescriptorType S = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                               I = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                               T = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    const std::array<VkDescriptorType, 7> taa_types{S, S, S, S, I, I, I};
    const std::array<VkDescriptorType, 2> sharpen_types{I, I};
    const std::array<VkDescriptorType, 5> merge_types{I, I, I, T, T};
    const Pipeline taa_live = MakePipeline(TAA_COMP, taa_types, 64);
    const Pipeline taa_default = MakePipeline(TAA_DEFAULT_SWITCHES_COMP, taa_types, 64);
    const Pipeline sharpen_old = MakePipeline(TAA_SHARPEN_COMP, sharpen_types, 4);
    // An optional SPIR-V file in place of taa_sharpen_scene_r11_comp (to try a variant).
    std::vector<uint32_t> scene_code(std::begin(TAA_SHARPEN_SCENE_R11_COMP),
                                     std::end(TAA_SHARPEN_SCENE_R11_COMP));
    if (argc > 1) {
        FILE* f = std::fopen(argv[1], "rb");
        if (!f) return 1;
        std::fseek(f, 0, SEEK_END);
        scene_code.resize(size_t(std::ftell(f)) / 4);
        std::fseek(f, 0, SEEK_SET);
        if (std::fread(scene_code.data(), 4, scene_code.size(), f) != scene_code.size()) return 1;
        std::fclose(f);
        std::printf("TAA fast paths: sharpen into the scene from %s\n", argv[1]);
    }
    const Pipeline sharpen_scene = MakePipeline(scene_code, sharpen_types, 4);
    const Pipeline merge = MakePipeline(UPSCALE_MERGE_R11_COMP, merge_types, 4);

    constexpr VkImageUsageFlags SU = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT;
    // The game's inputs (scene color B10G11R11, depth as R32 color, motion in pixels R16G16),
    // shared, and one set of TAA images and one scene color per path.
    const Image input = MakeImage(VK_FORMAT_B10G11R11_UFLOAT_PACK32, 4, SU);
    const Image depth = MakeImage(VK_FORMAT_R32_SFLOAT, 4, SU);
    const Image motion = MakeImage(VK_FORMAT_R16G16_SFLOAT, 4, SU);
    const Image opaque = MakeImage(VK_FORMAT_R16G16B16A16_SFLOAT, 8, SU); // binding 6, unused
    const Image reactive = MakeImage(VK_FORMAT_R8_UNORM, 1, SU);
    struct Path {
        const char* name;
        bool default_switches, into_scene;
        Image scene, output;
        std::array<Image, 2> history;
    };
    std::array<Path, 4> paths{{{"old", false, false},
                               {"resolve", true, false},
                               {"sharpen", false, true},
                               {"both", true, true}}};
    for (auto& path : paths) {
        path.scene = MakeImage(VK_FORMAT_B10G11R11_UFLOAT_PACK32, 4, SU);
        path.output = MakeImage(VK_FORMAT_R16G16B16A16_SFLOAT, 8, SU);
        for (auto& h : path.history) h = MakeImage(VK_FORMAT_R32G32B32A32_SFLOAT, 16, SU);
    }

    const VkDeviceSize staging_size = VkDeviceSize(N) * 16 * 12;
    const VkBufferCreateInfo buffer_info{.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                                         .size = staging_size,
                                         .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                                                  VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    VkBuffer staging;
    CHECK(vkCreateBuffer(device, &buffer_info, nullptr, &staging));
    VkMemoryRequirements requirements;
    vkGetBufferMemoryRequirements(device, staging, &requirements);
    const VkMemoryAllocateInfo staging_alloc{
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = requirements.size,
        .memoryTypeIndex = MemoryType(requirements.memoryTypeBits,
                                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
    VkDeviceMemory staging_memory;
    CHECK(vkAllocateMemory(device, &staging_alloc, nullptr, &staging_memory));
    CHECK(vkBindBufferMemory(device, staging, staging_memory, 0));
    char* mapped;
    CHECK(vkMapMemory(device, staging_memory, 0, VK_WHOLE_SIZE, 0, (void**)&mapped));

    const auto begin = [] {
        const VkCommandBufferBeginInfo info{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        CHECK(vkResetCommandBuffer(cmd, 0));
        CHECK(vkBeginCommandBuffer(cmd, &info));
    };
    const auto submit = [] {
        CHECK(vkEndCommandBuffer(cmd));
        const VkSubmitInfo info{.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                                .commandBufferCount = 1,
                                .pCommandBuffers = &cmd};
        CHECK(vkQueueSubmit(queue, 1, &info, VK_NULL_HANDLE));
        CHECK(vkQueueWaitIdle(queue));
        CHECK(vkResetDescriptorPool(device, descriptor_pool, 0));
    };
    const auto copy = [&](const Image& image, VkDeviceSize offset, bool to_image) {
        const VkBufferImageCopy region{.bufferOffset = offset,
                                       .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                                       .imageExtent = {W, H, 1}};
        if (to_image) {
            vkCmdCopyBufferToImage(cmd, staging, image.image, VK_IMAGE_LAYOUT_GENERAL, 1, &region);
        } else {
            vkCmdCopyImageToBuffer(cmd, image.image, VK_IMAGE_LAYOUT_GENERAL, staging, 1, &region);
        }
    };
    // All images to General once.
    begin();
    {
        std::vector<VkImage> all{input.image, depth.image, motion.image, opaque.image,
                                 reactive.image};
        for (auto& path : paths) {
            all.insert(all.end(), {path.scene.image, path.output.image, path.history[0].image,
                                   path.history[1].image});
        }
        for (VkImage image : all) {
            const VkImageMemoryBarrier barrier{
                .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
                .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                .image = image,
                .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                 VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1,
                                 &barrier);
        }
    }
    submit();

    std::mt19937 rng(12345);
    std::uniform_real_distribution<float> unit(0.0f, 1.0f);
    // Random float of the 6- or 5-bit-mantissa format, as bits: HDR scene values with highlights.
    const auto small_float = [&](uint32_t mantissa_bits) {
        const float r = unit(rng);
        float v = r < 0.05f ? 0.0f : r < 0.08f ? 200.0f + 3000.0f * unit(rng)
                                               : std::pow(unit(rng), 2.2f) * 4.0f;
        int e;
        const float m = std::frexp(v, &e);
        if (v == 0.0f || e - 1 < -14) return 0u;
        const uint32_t mant = std::min(uint32_t((m * 2 - 1) * float(1u << mantissa_bits)),
                                       (1u << mantissa_bits) - 1);
        return (uint32_t(e - 1 + 15) << mantissa_bits) | mant;
    };
    bool failed = false;
    for (const float strength : {0.0f, 0.3f, 1.0f, 2.0f}) {
        // A smooth field with noise, edges and highlights, moving by a frame-dependent offset.
        for (uint32_t frame = 0; frame < FRAMES; ++frame) {
            auto* scene_in = reinterpret_cast<uint32_t*>(mapped);
            auto* depth_in = reinterpret_cast<float*>(mapped + N * 4);
            auto* motion_in = reinterpret_cast<uint16_t*>(mapped + N * 8);
            for (uint32_t p = 0; p < N; ++p) {
                const uint32_t x = p % W, y = p / W;
                scene_in[p] = small_float(6) | (small_float(6) << 11) | (small_float(5) << 22);
                const bool sky = y < 8 && x > 40;
                const bool near = (x / 17 + y / 13 + frame) % 5 == 0;
                depth_in[p] = sky ? 1.0f
                                  : std::clamp(0.9f + 0.0005f * float(x) - (near ? 0.3f : 0.0f) +
                                                   0.00002f * unit(rng),
                                               0.0f, 0.99999f);
                const float r = unit(rng);
                const float mx = r < 0.3f ? 0.0f : r < 0.8f ? (unit(rng) - 0.5f) * 2.0f
                                 : r < 0.97f ? (unit(rng) - 0.5f) * 24.0f : 400.0f;
                const float my = r < 0.3f ? 0.0f : (unit(rng) - 0.5f) * 3.0f;
                motion_in[2 * p] = ToHalf(mx);
                motion_in[2 * p + 1] = ToHalf(my);
            }
            const std::vector<uint32_t> scene_before(scene_in, scene_in + N);
            begin();
            copy(input, 0, true);
            copy(depth, N * 4, true);
            copy(motion, N * 8, true);
            for (auto& path : paths) {
                copy(path.scene, 0, true); // the game's scene color, which the result replaces
            }
            Barrier();
            const uint32_t next = frame % 2;
            const std::array<float, 2> jitter{Halton(frame % 8 + 1, 2) - 0.5f,
                                              Halton(frame % 8 + 1, 3) - 0.5f};
            for (auto& path : paths) {
                const bool into_scene = path.into_scene && strength > 0;
                struct Params {
                    std::array<float, 2> jitter;
                    uint32_t reset, pad;
                    std::array<std::array<float, 4>, 3> depth;
                } params{jitter, frame == 0 ? 1u : 0u, into_scene ? 1u << 31 : 0u,
                         {{{1, 1, 1, -0.05f}, {1, 1, 1, -0.05f}, {0, 0, 1, 0.01f}}}};
                const std::array<VkImageView, 7> taa_views{
                    input.view,  depth.view,  motion.view, path.history[1 - next].view,
                    path.output.view, path.history[next].view, opaque.view};
                Dispatch(path.default_switches ? taa_default : taa_live, taa_views, &params,
                         sizeof(params), taa_types);
                if (strength > 0) {
                    const std::array<VkImageView, 2> views{
                        path.history[next].view, into_scene ? path.scene.view : path.output.view};
                    Dispatch(into_scene ? sharpen_scene : sharpen_old, views, &strength, 4,
                             sharpen_types);
                }
                if (!into_scene) {
                    const uint32_t mode = 0;
                    const std::array<VkImageView, 5> views{path.output.view, path.scene.view,
                                                           reactive.view, motion.view,
                                                           motion.view};
                    Dispatch(merge, views, &mode, 4, merge_types);
                }
            }
            for (uint32_t i = 0; i < paths.size(); ++i) {
                copy(paths[i].scene, VkDeviceSize(N) * 20 * i, false);
                copy(paths[i].history[next], VkDeviceSize(N) * 20 * i + N * 4, false);
            }
            submit();
            const auto* old_scene = reinterpret_cast<const uint32_t*>(mapped);
            const char* old_history = mapped + N * 4;
            uint32_t changed = 0; // pixels the write-back changed (the test must see some)
            for (uint32_t p = 0; p < N; ++p) {
                changed += old_scene[p] != scene_before[p];
            }
            for (uint32_t i = 1; i < paths.size(); ++i) {
                const auto* scene = reinterpret_cast<const uint32_t*>(mapped + N * 20 * i);
                const char* history = mapped + N * 20 * i + N * 4;
                uint32_t scene_diff = 0, max_ulp = 0;
                for (uint32_t p = 0; p < N; ++p) {
                    if (scene[p] == old_scene[p]) continue;
                    ++scene_diff;
                    for (const auto& [shift, bits] : {std::pair{0, 11}, {11, 11}, {22, 10}}) {
                        const uint32_t a = (scene[p] >> shift) & ((1u << bits) - 1);
                        const uint32_t b = (old_scene[p] >> shift) & ((1u << bits) - 1);
                        max_ulp = std::max(max_ulp, a > b ? a - b : b - a);
                    }
                }
                const bool history_same = std::memcmp(history, old_history, N * 16) == 0;
                if (scene_diff || !history_same || changed < N / 2) {
                    failed = true;
                    std::printf("TAA fast paths: FAIL %s, sharpen %.1f, frame %u: %u of %u scene "
                                "pixels differ (max %u in a channel's bits), history %s, %u "
                                "written back\n",
                                paths[i].name, strength, frame, scene_diff, N, max_ulp,
                                history_same ? "same" : "differs", changed);
                }
            }
        }
        std::printf("TAA fast paths: sharpen %.1f, %u frames: %s\n", strength, FRAMES,
                    failed ? "FAIL" : "resolve, sharpen and both equal to the old passes");
    }
    std::printf("TAA fast paths: %s\n", failed ? "FAIL" : "PASS");
    return failed ? 1 : 0;
}
