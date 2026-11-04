// SPDX-License-Identifier: GPL-2.0-or-later
// The texture-set memo key (vk_texture_set_key.h): one function for workers and the GPU thread.
#include <array>
#include <cassert>
#include <cstdio>
#include <cstring>

#include "video_core/renderer_vulkan/vk_texture_set_key.h"

int main() {
    using namespace Vulkan;
    AmdGpu::Image sharp{};
    std::array<u32, 8> words{0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88};
    std::memcpy(&sharp, words.data(), sizeof(words));
    Shader::ImageResource res{};
    const u64 base = ImageDescHash(sharp, res);
    assert(ImageDescHash(sharp, res) == base);
    // Every T# word counts.
    for (u32 w = 0; w < words.size(); ++w) {
        auto changed = words;
        changed[w] ^= 1;
        AmdGpu::Image other{};
        std::memcpy(&other, changed.data(), sizeof(changed));
        assert(ImageDescHash(other, res) != base);
    }
    // The binding flags count too.
    Shader::ImageResource array = res;
    array.is_array = true;
    assert(ImageDescHash(sharp, array) != base);
    Shader::ImageResource depth = res;
    depth.is_depth = true;
    assert(ImageDescHash(sharp, depth) != base);

    // The set key: never 0 (0 marks an empty slot), seeded by the stage, sensitive to every hash
    // and to the count.
    std::array<u64, 3> hashes{base, 2, 3};
    int stage_a = 0, stage_b = 0;
    const u64 key = TextureSetKey(hashes.data(), 3, &stage_a);
    assert(key & 1);
    assert(key == TextureSetKey(hashes.data(), 3, &stage_a));
    assert(key != TextureSetKey(hashes.data(), 3, &stage_b));
    assert(key != TextureSetKey(hashes.data(), 2, &stage_a));
    hashes[1] = 5;
    assert(key != TextureSetKey(hashes.data(), 3, &stage_a));
    std::puts("texture set key: ok");
}
