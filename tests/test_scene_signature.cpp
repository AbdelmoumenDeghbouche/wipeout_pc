// SPDX-License-Identifier: GPL-2.0-or-later
// WipEout's main scene pass (vk_scene_signature.h). No game or Vulkan device needed.
#include <cassert>
#include <cstdio>

#include "video_core/renderer_vulkan/vk_scene_signature.h"

using Vulkan::SceneSignature::Target;
using Vulkan::SceneSignature::WipeoutScenePass;

constexpr Target Color{true, vk::Format::eB10G11R11UfloatPack32, 1920, 1080};
constexpr Target Motion{true, vk::Format::eR16G16Sfloat, 1920, 1080};
constexpr Target Depth{true, vk::Format::eD32Sfloat, 1920, 1080};

int main() {
    // Colour, linear depth, motion vectors, all the size of the scene depth.
    assert(WipeoutScenePass(0b111, Color, Motion, Depth));
    // Three colour targets in slots 0, 1 and 3: slot 2 holds whatever an earlier pass left.
    assert(!WipeoutScenePass(0b1011, Color, Motion, Depth));
    // God of War III's G-buffer: five colour targets.
    assert(!WipeoutScenePass(0b11111, Color, Motion, Depth));
    // Wrong colour-0 format.
    assert(!WipeoutScenePass(0b111, {true, vk::Format::eR16G16B16A16Sfloat, 1920, 1080}, Motion,
                             Depth));
    // Wrong colour-2 format.
    assert(!WipeoutScenePass(0b111, Color, {true, vk::Format::eR8G8B8A8Unorm, 1920, 1080}, Depth));
    // A colour target smaller than the depth (a half-resolution pass).
    assert(!WipeoutScenePass(0b111, {true, Color.format, 960, 540}, Motion, Depth));
    assert(!WipeoutScenePass(0b111, Color, {true, Motion.format, 960, 540}, Depth));
    // No depth (the other fields as a bound one, so only its absence rejects it).
    assert(!WipeoutScenePass(0b111, Color, Motion, {false, Depth.format, 1920, 1080}));
    // No colour-2 target.
    assert(!WipeoutScenePass(0b111, Color, {false, Motion.format, 1920, 1080}, Depth));
    std::puts("scene signature: ok");
}
