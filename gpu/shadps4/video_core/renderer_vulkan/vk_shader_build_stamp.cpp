// SPDX-License-Identifier: GPL-2.0-or-later
// gow3: the content of the shader recompiler, hashed by gpu/CMakeLists.txt at configure time.
#include "video_core/renderer_vulkan/vk_shader_build_stamp.h"

#ifndef GOW3_SHADER_SOURCE_HASH
#error "GOW3_SHADER_SOURCE_HASH: gpu/CMakeLists.txt defines it for this file"
#endif

namespace Vulkan {
std::string_view ShaderBuildStamp() {
    return "gow3 disk shaders, recompiler " GOW3_SHADER_SOURCE_HASH;
}
} // namespace Vulkan
