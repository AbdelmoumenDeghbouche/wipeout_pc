// SPDX-License-Identifier: GPL-2.0-or-later
// gow3: keys of the texture description cache and of the texture-set memo. One definition for the
// draw-preparation workers and the GPU thread, so a set recorded by one is found by the other.
#pragma once

#include <array>
#include <cstring>

#include <xxhash.h>

#include "common/types.h"
#include "shader_recompiler/resource.h"

namespace Vulkan {

/// Key of Rasterizer's texture description cache for a T# bound through `res`.
inline u64 ImageDescHash(const AmdGpu::Image& sharp, const Shader::ImageResource& res) {
    std::array<u64, 4> key;
    std::memcpy(key.data(), &sharp, sizeof(key));
    const u32 flags = u32(res.is_written) | u32(res.is_depth) << 1 | u32(res.is_array) << 2;
    u64 hash = flags * 0x9E3779B97F4A7C15ull;
    for (const u64 word : key) {
        hash = (hash ^ word) * 0xFF51AFD7ED558CCDull;
        hash ^= hash >> 32;
    }
    return hash;
}

/// TextureSetMemo key of a stage's images, from their ImageDescHash values. Never 0, which marks
/// an empty slot.
[[nodiscard]] inline u64 TextureSetKey(const u64* hashes, u32 count, const void* stage) {
    return XXH3_64bits_withSeed(hashes, count * sizeof(u64), reinterpret_cast<u64>(stage)) | 1;
}

} // namespace Vulkan
