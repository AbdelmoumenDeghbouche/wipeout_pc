// SPDX-License-Identifier: GPL-2.0-or-later
// gow3: WipEout's main scene pass, recognised by its targets. The reduced-size scene starts at
// God of War III's G-buffer (five or more colour targets); WipEout's scene pass has three, so the
// rasterizer needs this signature both for the upscaler's inputs and to start the reduced scene.
#pragma once

#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"

namespace Vulkan::SceneSignature {

struct Target {
    bool present;
    vk::Format format;
    u32 width;
    u32 height;
};

/// WipEout's main scene pass: colour targets in slots 0, 1 and 2 only (slots past a pass's mask
/// keep earlier passes' targets), B10G11R11 colour in slot 0, R16G16F motion vectors in slot 2,
/// both the size of the scene depth.
[[nodiscard]] constexpr bool WipeoutScenePass(u32 mrt_mask, Target color0, Target color2,
                                              Target depth) noexcept {
    const auto depth_sized = [&](const Target& target) {
        return target.width == depth.width && target.height == depth.height;
    };
    return mrt_mask == 0b111 && depth.present && color0.present && color2.present &&
           color0.format == vk::Format::eB10G11R11UfloatPack32 &&
           color2.format == vk::Format::eR16G16Sfloat && depth_sized(color0) && depth_sized(color2);
}

} // namespace Vulkan::SceneSignature
