// SPDX-License-Identifier: GPL-2.0-or-later
// The frame analyzer's buffer dump (vk_frame_capture.cpp). A binding whose bytes the host cannot
// read (data null: a constant-ring copy of the flattened user data carries guest address 0,
// issue #174) is skipped instead of read through the null pointer. No game or Vulkan device.
#include <array>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>

#include "video_core/renderer_vulkan/vk_frame_capture.h"
#include "video_core/texture_cache/image_info.h"

int main(int argc, char** argv) {
    // A buffer with host bytes is dumped as before.
    std::array<float, 16> words{};
    Vulkan::FrameCapture::Buffer(0x1234, 0, 0x1000, words.data(), sizeof(words));
    // Flattened user data in the constant ring: host copy only, guest address 0, and here
    // nothing readable at all. Must not dereference `data`.
    Vulkan::FrameCapture::Buffer(0x1234, 1, 0, nullptr, 64);
    if (argc > 1) {
#ifdef _WIN32
        _putenv_s("GOW3_CAPTURE_DIR", argv[1]);
#else
        setenv("GOW3_CAPTURE_DIR", argv[1], 1);
#endif
        VideoCore::ImageInfo a{}, b{}, texture{};
        a.guest_address = 0x1000; b.guest_address = 0x2000; texture.guest_address = 0x3000;
        for (auto* info : {&a, &b, &texture}) {
            info->pixel_format = vk::Format::eR8G8B8A8Unorm;
            info->size = {1920, 1080, 1}; info->pitch = 1920;
        }
        using Vulkan::FrameCapture;
        FrameCapture::AddDisplayBuffer(a.guest_address);
        FrameCapture::AddDisplayBuffer(b.guest_address);
        FrameCapture::Request(); FrameCapture::OnFlip(a.guest_address);
        const VideoCore::ImageInfo* colors[] = {&a};
        FrameCapture::BeginPass(colors, 1, nullptr, "synthetic display");
        FrameCapture::Sampled(texture, false); FrameCapture::Draw(0xabc, 0x111, 6, 1);
        FrameCapture::Sampled(texture, true); FrameCapture::Draw(0xabc, 0x222, 12, 2);
        for (unsigned i = 2; i < 514; ++i) FrameCapture::Draw(0xabc, 0x333, 6, 1);
        colors[0] = &texture;
        FrameCapture::BeginPass(colors, 1, nullptr, "synthetic offscreen");
        FrameCapture::Draw(0xabc, 0x444, 6, 1);
        colors[0] = &b;
        FrameCapture::BeginPass(colors, 1, nullptr, "next display");
        const auto result = FrameCapture::LastResult();
        const auto separator = result.find(": ");
        if (separator == std::string::npos) return 21;
        std::ifstream file(result.substr(separator + 2));
        const std::string text{std::istreambuf_iterator<char>(file), {}};
        unsigned rows = 0;
        for (std::size_t p = 0; (p = text.find("ordered draw ", p)) != std::string::npos; p += 13) ++rows;
        const auto first = text.find("ordered draw 0 vs 0000000000000abc ps 0000000000000111");
        const auto second = text.find("ordered draw 1 vs 0000000000000abc ps 0000000000000222");
        const auto sample = text.find("\n    samples 0x3000", first);
        const auto write = text.find("\n    writes 0x3000", second);
        const bool negative = argc == 3 && std::string(argv[2]) == "--negative-control";
        const bool pass = rows == (negative ? 513u : 512u) && first < second &&
            sample > first && sample < second && write > second && write < text.size() &&
            text.find("PASS draws 514") != std::string::npos &&
            text.find("ordered draw 513") == std::string::npos &&
            text.find("ordered draw 0 vs 0000000000000abc ps 0000000000000444") == std::string::npos;
        std::printf("FRAME_ORDER rows=%u pass=%u\n", rows, pass);
        if (!pass) return 20;
    }
    std::puts("frame-capture-test ok");
    return 0;
}
