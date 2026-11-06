// SPDX-License-Identifier: GPL-2.0-or-later
// gow3: identifies the build that translated the saved shaders (vk_pipeline_serialization.cpp).
#pragma once

#include <string_view>

namespace Vulkan {
/// Changes exactly when a shader_recompiler source or header changes: a SHA-256 of their
/// contents, made by gpu/CMakeLists.txt at configure time (so rebuilding an unchanged
/// translator, as every CI release does, keeps the same stamp).
std::string_view ShaderBuildStamp();
} // namespace Vulkan
