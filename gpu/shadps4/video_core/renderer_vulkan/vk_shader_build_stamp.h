// SPDX-License-Identifier: GPL-2.0-or-later
// gow3: identifies the build that translated the saved shaders (vk_pipeline_serialization.cpp).
#pragma once

#include <string_view>

namespace Vulkan {
/// Changes whenever the shader recompiler or the store format is rebuilt: gpu/CMakeLists.txt
/// makes vk_shader_build_stamp.cpp depend on every shader_recompiler source and header.
std::string_view ShaderBuildStamp();
} // namespace Vulkan
