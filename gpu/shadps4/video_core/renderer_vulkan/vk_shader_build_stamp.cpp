// SPDX-License-Identifier: GPL-2.0-or-later
// gow3: its own file so that it alone recompiles (new time) when the recompiler changes.
#include "video_core/renderer_vulkan/vk_shader_build_stamp.h"

namespace Vulkan {
std::string_view ShaderBuildStamp() {
    return "gow3 disk shaders, built " __DATE__ " " __TIME__;
}
} // namespace Vulkan
