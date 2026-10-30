// SPDX-License-Identifier: GPL-2.0-or-later
#include "../shadps4/common/wipeout_oc_env.h"
#include "wipeout_oc_settings.h"
#include "wipeout_oc_graphics.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>
#ifdef _WIN32
#include <io.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace WipeoutOCSettings {

namespace {

namespace fs = std::filesystem;
fs::path selected_path;
std::vector<std::string> retained_keys;

fs::path EnvironmentPath(const char* suffix) {
#ifdef _WIN32
    const auto current = fs::path(std::string("WIPEOUT_OC_") + suffix).wstring();
    const auto legacy = fs::path(std::string("GOW3_") + suffix).wstring();
    const wchar_t* value = _wgetenv(current.c_str());
    if (!value) value = _wgetenv(legacy.c_str());
    return value ? fs::path(value) : fs::path{};
#else
    const char* value = wipeout_oc_getenv((std::string("WIPEOUT_OC_") + suffix).c_str());
    return value ? fs::path(value) : fs::path{};
#endif
}

FILE* Open(const fs::path& path, bool write) {
#ifdef _WIN32
    return _wfopen(path.c_str(), write ? L"wb" : L"rb");
#else
    return std::fopen(path.c_str(), write ? "wb" : "rb");
#endif
}

std::string DisplayPath(const fs::path& path) {
    const auto utf8 = path.u8string();
    return {reinterpret_cast<const char*>(utf8.data()), utf8.size()};
}

fs::path ResolvePath() {
    if (const auto override = EnvironmentPath("CONFIG"); !override.empty()) {
        return fs::absolute(override); // Do not collapse symlinks or parent components.
    }
    const auto data = EnvironmentPath("DATA_DIR");
    const auto directory = data.empty() ? fs::current_path() : fs::absolute(data);
    const auto destination = directory / "wipeout_oc.ini";
    const auto source = directory / "gow3.ini";
    if (fs::exists(destination) || !fs::exists(source)) return destination;

    // copy_file creates a separate, exclusive temporary file. Only its complete, flushed bytes
    // are published; neither rename nor link is ever allowed to replace a new user's file.
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    static std::atomic<unsigned> sequence{};
    const auto temporary = directory / (".wipeout_oc-" + std::to_string(stamp) + "-" +
                                        std::to_string(sequence++) + ".tmp");
    std::error_code error;
    if (!fs::copy_file(source, temporary, fs::copy_options::none, error)) {
        if (error != std::errc::file_exists) {
            std::error_code ignored;
            fs::remove(temporary, ignored);
        }
        throw fs::filesystem_error("cannot copy legacy settings", source, temporary, error);
    }
    try {
        // The original may be readonly; only our independent temporary copy must be writable.
        fs::permissions(temporary, fs::perms::owner_write, fs::perm_options::add);
#ifdef _WIN32
        FILE* stream = _wfopen(temporary.c_str(), L"r+b");
#else
        FILE* stream = std::fopen(temporary.c_str(), "r+b");
#endif
        if (!stream) throw std::system_error(errno, std::generic_category(), "open settings copy");
#ifdef _WIN32
        const int flushed = _commit(_fileno(stream));
#else
        const int flushed = fsync(fileno(stream));
#endif
        const int saved_errno = errno;
        const int closed = std::fclose(stream);
        if (flushed || closed) throw std::system_error(flushed ? saved_errno : errno,
                                                       std::generic_category(), "flush settings copy");
#ifdef _WIN32
        if (!MoveFileExW(temporary.c_str(), destination.c_str(), 0)) {
            const auto code = GetLastError();
            if (code != ERROR_ALREADY_EXISTS && code != ERROR_FILE_EXISTS) {
                throw std::system_error(code, std::system_category(), "publish settings copy");
            }
        }
#else
        fs::create_hard_link(temporary, destination, error);
        if (error && error != std::errc::file_exists) {
            throw fs::filesystem_error("publish settings copy", destination, error);
        }
#endif
    } catch (...) {
        fs::remove(temporary, error);
        throw;
    }
    fs::remove(temporary, error);
    return destination;
}

float Clamp(float v, float lo, float hi) {
    return std::clamp(v, lo, hi);
}

bool Set(Values& v, const std::string& key, const std::string& value) {
    const float f = float(std::atof(value.c_str()));
    const int i = std::atoi(value.c_str());
    static const char* const overlay_keys[] = {"overlay_fps", "overlay_frametime", "overlay_upscaler",
                                               "overlay_gpu", "overlay_cpu", "overlay_ram", "overlay_vram"};
    for (unsigned item = 0; item < 7; ++item) {
        if (key == overlay_keys[item]) {
            const unsigned bit = 1u << item;
            v.overlay_items = i ? (v.overlay_items | bit) : (v.overlay_items & ~bit);
            return true;
        }
    }
    if (key == "overlay_scale") {
        v.overlay_scale = std::clamp(i, 50, 300);
        return true;
    }
    if (key == "overlay_corner") {
        v.overlay_corner = i >= 0 && i <= 3 ? i : 1;
        return true;
    }
    if (key == "overlay_opacity") {
        v.overlay_opacity = std::clamp(i, 0, 100);
        return true;
    }
    if (key == "overlay_layout") {
        v.overlay_layout = i == 1 ? 1 : 0;
        return true;
    }
    if (key == "menu_tab") {
        v.menu_tab = i >= 0 && i <= 4 ? i : 0;
        return true;
    }
    if (key == "upscaler") {
        for (int u = 0; u < UpscalerCount; ++u) {
            if (value == UpscalerName(u)) {
                v.upscaler = u;
            }
        }
    } else if (key == "preset") {
        v.preset = std::clamp(i, 0, PresetCount - 1);
    } else if (key == "sharpen") {
        v.sharpen = i != 0;
    } else if (key == "sharpness") {
        v.sharpness = Clamp(f, 0.0f, 2.0f);
    } else if (key == "jitter") {
        v.jitter = i != 0;
    } else if (key == "reactive") {
        v.reactive = i != 0;
    } else if (key == "object_motion") {
        v.object_motion = i != 0;
    } else if (key == "reactive_scale") {
        v.reactive_scale = Clamp(f, 0.0f, 16.0f);
    } else if (key == "reactive_threshold") {
        v.reactive_threshold = Clamp(f, 0.0f, 1.0f);
    } else if (key == "reactive_max") {
        v.reactive_max = Clamp(f, 0.0f, 1.0f);
    } else if (key == "debug_view") {
        v.debug_view = std::clamp(i, 0, DebugViewCount - 1);
    } else if (key == "show_fps") {
        v.show_fps = i != 0;
    } else if (key == "display_mode") {
        v.display_mode = WipeoutOCGraphics::ParseDisplayMode(value);
    } else if (key == "vsync") {
        v.vsync = i != 0;
    } else if (key == "fps_limit") {
        v.fps_limit = WipeoutOCGraphics::ParseFpsLimit(i);
    } else if (key == "async_shaders") {
        v.async_shaders = i != 0;
    } else if (key == "fsr1") {
        // FSR 1 left the launcher and the in-game menu: a value saved by an older version has no
        // control left to turn it off, so it stays off.
        v.fsr1 = false;
    } else if (key == "rcas") {
        v.rcas = i != 0;
    } else if (key == "rcas_strength") {
        v.rcas_strength = WipeoutOCGraphics::ParseRcasStrength(value.empty() ? -1 : i);
    } else if (key == "frames_queued") {
        v.frames_queued = WipeoutOCGraphics::ParseFramesQueued(value.empty() ? -1 : i);
    } else if (key == "render_resolution") {
        v.render_resolution = WipeoutOCGraphics::ParseResolution(value);
    } else if (key == "engine_fps") {
        v.engine_fps = WipeoutOCGraphics::ParseEngineFps(i);
    } else if (key == "deferred_readback") {
        v.deferred_readback = i != 0;
    } else if (key == "stale_readback") {
        v.stale_readback = i != 0;
    } else if (key == "anisotropy") {
        v.anisotropy = WipeoutOCGraphics::ParseAnisotropy(i);
    } else if (key == "vibration") {
        v.vibration = value.empty() ? 100 : std::clamp(i, 0, 100);
    } else if (key == "stick_deadzone") {
        v.stick_deadzone = std::clamp(i, 0, 50);

    } else if (key == "mouse_lock") {
        v.mouse_lock = value.empty() || i != 0;
    } else if (key == "music") {
        v.music = value.empty() || i != 0;
    } else if (key == "music_shuffle") {
        v.music_shuffle = value.empty() || i != 0;
    } else if (key == "music_continue") {
        v.music_continue = !value.empty() && i != 0;
    } else if (key == "music_volume") {
        v.music_volume = value.empty() ? 100 : std::clamp(i, 0, 200);


    } else if (key == "restart_unconfirmed") {
        v.restart_unconfirmed = i != 0;







    } else if (key == "fsr4_auto_exposure") {
        v.fsr4_auto_exposure = i != 0;
    } else if (key == "fsr4_invert_jitter") {
        v.fsr4_invert_jitter = i != 0;
    } else if (key == "dlss_preset") {
        v.dlss_preset = i >= 10 && i <= 13 ? i : 0;
    } else if (key == "live_resolution") {
        v.live_resolution = value == "auto" ? -1 : std::clamp(i, 0, 1);
    } else if (key == "output_res") {
        for (int r = 0; r < OutputCount; ++r) {
            if (value == std::to_string(OutputWidths[r]) + "x" + std::to_string(OutputHeights[r])) {
                v.output_res = r;
            }
        }
    } else {
        return false;
    }
    return true;
}

} // namespace

Values& Get() {
    static Values values;
    return values;
}

bool Load() {
    auto& v = Get();
    fs::path path;
    try {
        path = ResolvePath();
    } catch (const std::exception& error) {
        std::fprintf(stderr, "Settings: cannot select configuration: %s\n", error.what());
        return false;
    }
    std::vector<std::string> pending_keys;
    if (FILE* file = Open(path, false)) {
        const auto read_line = [&](std::string raw) {
            while (!raw.empty() && (raw.back() == '\r' || raw.back() == '\n')) raw.pop_back();
            std::string text = raw;
            text.erase(text.find_last_not_of(" \t") + 1);
            const auto eq = text.find('=');
            if (text.empty() || text[0] == '#' || eq == std::string::npos) return;
            if (!Set(v, text.substr(0, eq), text.substr(eq + 1))) pending_keys.push_back(raw);
        };
        char chunk[256];
        std::string line;
        while (std::fgets(chunk, sizeof(chunk), file)) {
            line += chunk;
            if (line.back() == '\n') { read_line(line); line.clear(); }
        }
        if (!line.empty()) read_line(line);
        const bool failed = std::ferror(file);
        std::fclose(file);
        if (failed) {
            std::fprintf(stderr, "Settings: cannot read %s\n", DisplayPath(path).c_str());
            return false;
        }
        std::printf("Settings: %s\n", DisplayPath(path).c_str());
    } else if (errno != ENOENT) {
        std::fprintf(stderr, "Settings: cannot read %s: %s\n", DisplayPath(path).c_str(), std::strerror(errno));
        return false;
    }
    selected_path = path;
    retained_keys = std::move(pending_keys);
    // Environment overrides (scripts, A/B tests).
    if (const char* env = wipeout_oc_getenv("WIPEOUT_OC_UPSCALER")) {
        v.upscaler = UpscalerOff;
        for (int u = 0; u < UpscalerCount; ++u) {
            if (std::strcmp(env, UpscalerName(u)) == 0) v.upscaler = u;
        }
    }
    const std::pair<const char*, const char*> env_keys[] = {
        {"WIPEOUT_OC_FSR_SHARPNESS", "sharpness"},        {"WIPEOUT_OC_JITTER", "jitter"},
        {"WIPEOUT_OC_REACTIVE", "reactive"},              {"WIPEOUT_OC_REACTIVE_SCALE", "reactive_scale"},
        {"WIPEOUT_OC_REACTIVE_THRESHOLD", "reactive_threshold"}, {"WIPEOUT_OC_REACTIVE_MAX", "reactive_max"},
        {"WIPEOUT_OC_UPSCALE_PRESET", "preset"},            {"WIPEOUT_OC_OBJECT_MOTION", "object_motion"},
        {"WIPEOUT_OC_DLSS_PRESET", "dlss_preset"},
        {"WIPEOUT_OC_ASYNC_SHADERS", "async_shaders"},      {"WIPEOUT_OC_DEFERRED_READBACK", "deferred_readback"},
        {"WIPEOUT_OC_STALE_READBACK", "stale_readback"},     {"WIPEOUT_OC_FSR1", "fsr1"},
        {"WIPEOUT_OC_RCAS", "rcas"},                        {"WIPEOUT_OC_FRAMES_AHEAD", "frames_queued"},
        {"WIPEOUT_OC_ANISOTROPY", "anisotropy"},            {"WIPEOUT_OC_VIBRATION", "vibration"}, {"WIPEOUT_OC_STICK_DEADZONE", "stick_deadzone"}, {"WIPEOUT_OC_MOUSE_LOCK", "mouse_lock"},
    };
    for (const auto& [env, key] : env_keys) {
        if (const char* value = wipeout_oc_getenv(env); value && value[0]) {
            Set(v, key, value);
        }
    }
    v.startup_preset = v.preset;
    v.startup_upscaler = v.upscaler;
    v.startup_object_motion = v.object_motion;
    v.startup_output_res = v.output_res;
    v.startup_live_resolution = v.live_resolution;
    v.startup_render_resolution = v.render_resolution;
    v.startup_engine_fps = v.engine_fps;
    const char* applied = wipeout_oc_getenv("WIPEOUT_OC_ENGINE_FPS");
    v.applied_engine_fps = applied && applied[0] ? WipeoutOCGraphics::ParseEngineFps(std::atoi(applied))
                                                 : v.engine_fps.load();
    v.startup_deferred_readback = v.deferred_readback;
    v.startup_stale_readback = v.stale_readback;
    v.startup_anisotropy = v.anisotropy;
    return true;
}

bool GraphicsNeedRestart() {
    const auto& v = Get();
    return v.render_resolution != v.startup_render_resolution ||
           v.engine_fps != v.startup_engine_fps ||
           v.deferred_readback != v.startup_deferred_readback ||
           v.stale_readback != v.startup_stale_readback ||
           v.anisotropy != v.startup_anisotropy;
}

void ConfigureUpscalerSupport(bool fsr4, bool fsr411) {
    auto& v = Get();
    v.fsr4_supported = fsr4;
    v.fsr411_supported = fsr4 && fsr411;
    const int requested = v.upscaler;
    if ((requested == UpscalerFsr4 && !v.fsr4_supported) ||
        (requested == UpscalerFsr411 && !v.fsr411_supported)) {
        v.fsr4_problem = "GPU does not support the selected FSR 4 shaders; using FSR 3.1";
        std::printf("Upscaler: %s unsupported on this GPU; falling back to FSR 3.1 before the first frame\n",
                    UpscalerName(requested));
        v.upscaler = UpscalerFsr3;
    }
}

void ConfigureDlssSupport(bool available, const char* problem) {
    auto& v = Get();
    v.dlss_supported = available;
    static std::string kept;
    kept = problem ? problem : "";
    v.dlss_problem = available || kept.empty() ? nullptr : kept.c_str();
    if (v.upscaler == UpscalerDlss && !available) {
        std::printf("Upscaler: DLSS unavailable (%s); falling back to FSR 3.1\n",
                    kept.empty() ? "wipeout_oc_dlss.dll or nvngx_dlss.dll missing" : kept.c_str());
        v.upscaler = UpscalerFsr3;
    }
}

bool FixedRenderSession() {
    const char* size = wipeout_oc_getenv("WIPEOUT_OC_RENDER_RES");
    return size && size[0];
}

int RenderPreset() {
    const auto& v = Get();
    return FixedRenderSession() ? v.startup_preset :
        v.upscaler == UpscalerTaa ? NativeAA : v.preset.load();
}

bool ResolutionNeedsRestart() {
    const auto& v = Get();
    // TAA needs the live path (native guest targets): run.sh selects it on restart.
    return FixedRenderSession() &&
        (v.preset != v.startup_preset || v.output_res != v.startup_output_res ||
         (v.upscaler == UpscalerOff) != (v.startup_upscaler == UpscalerOff) ||
         (v.upscaler == UpscalerTaa) != (v.startup_upscaler == UpscalerTaa));
}

void Save() {
    const auto& v = Get();
    fs::path path;
    try {
        path = selected_path.empty() ? ResolvePath() : selected_path;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "Settings: cannot select configuration to save: %s\n", error.what());
        return;
    }
    FILE* file = Open(path, true);
    if (!file) {
        std::fprintf(stderr, "Settings: cannot write %s: %s\n", DisplayPath(path).c_str(), std::strerror(errno));
        return;
    }
    selected_path = path;
    std::fprintf(file,
                 "# WipEout Omega Collection settings (in-game menu: Insert / R3+L2)\n"
                 "upscaler=%s\npreset=%d\nsharpen=%d\nsharpness=%.2f\njitter=%d\n"
                 "reactive=%d\nobject_motion=%d\nreactive_scale=%.2f\nreactive_threshold=%.2f\nreactive_max=%.2f\n"
                 "debug_view=%d\nshow_fps=%d\nfsr4_auto_exposure=%d\nfsr4_invert_jitter=%d\n",
                 UpscalerName(v.upscaler), v.preset.load(), int(v.sharpen.load()),
                 v.sharpness.load(), int(v.jitter.load()), int(v.reactive.load()),
                 int(v.object_motion.load()),
                 v.reactive_scale.load(), v.reactive_threshold.load(), v.reactive_max.load(),
                 v.debug_view.load(), int(v.show_fps.load()),
                 int(v.fsr4_auto_exposure.load()), int(v.fsr4_invert_jitter.load()));
    std::fprintf(file, "dlss_preset=%d\n", v.dlss_preset.load());
    std::fprintf(file, "output_res=%dx%d\n", OutputWidths[v.output_res], OutputHeights[v.output_res]);
    std::fprintf(file,
                 "overlay_fps=%d\noverlay_frametime=%d\noverlay_upscaler=%d\noverlay_gpu=%d\n"
                 "overlay_cpu=%d\noverlay_ram=%d\noverlay_vram=%d\noverlay_scale=%d\noverlay_corner=%d\n"
                 "overlay_opacity=%d\noverlay_layout=%d\nmenu_tab=%d\n",
                 int(v.overlay_items >> 0 & 1), int(v.overlay_items >> 1 & 1), int(v.overlay_items >> 2 & 1),
                 int(v.overlay_items >> 3 & 1), int(v.overlay_items >> 4 & 1), int(v.overlay_items >> 5 & 1),
                 int(v.overlay_items >> 6 & 1), v.overlay_scale.load(), v.overlay_corner.load(),
                 v.overlay_opacity.load(), v.overlay_layout.load(), v.menu_tab.load());
    const std::vector<std::string> resolution_names = WipeoutOCGraphics::ResolutionNames();
    std::fprintf(file,
                 "display_mode=%s\nvsync=%d\nfps_limit=%d\nasync_shaders=%d\n"
                 "fsr1=%d\nrcas=%d\nrcas_strength=%d\nframes_queued=%d\n"
                 "render_resolution=%s\nengine_fps=%d\ndeferred_readback=%d\nstale_readback=%d\n"
                 "restart_unconfirmed=%d\n",
                 WipeoutOCGraphics::DisplayModeKeys[std::clamp(v.display_mode.load(), 0, 2)].data(),
                 int(v.vsync.load()), v.fps_limit.load(), int(v.async_shaders.load()),
                 int(v.fsr1.load()), int(v.rcas.load()), v.rcas_strength.load(), v.frames_queued.load(),
                 resolution_names[std::clamp(v.render_resolution.load(), 0, int(resolution_names.size()) - 1)].c_str(),
                 v.engine_fps.load(), int(v.deferred_readback.load()), int(v.stale_readback.load()),
                 int(v.restart_unconfirmed.load()));
    std::fprintf(file, "anisotropy=%d\nvibration=%d\nmouse_lock=%d\n"
                       "music=%d\nmusic_shuffle=%d\nmusic_volume=%d\n"
                       "stick_deadzone=%d\nmusic_continue=%d\n",
                 v.anisotropy.load(), v.vibration.load(), int(v.mouse_lock.load()), int(v.music.load()),
                 int(v.music_shuffle.load()), v.music_volume.load(), v.stick_deadzone.load(),
                 int(v.music_continue.load()));
    for (const auto& line : retained_keys) std::fprintf(file, "%s\n", line.c_str());
    // Read by run.sh at start.
    std::fprintf(file, "live_resolution=%s\n", v.live_resolution < 0 ? "auto"
                                                  : v.live_resolution ? "1" : "0");
    const bool failed = std::ferror(file);
    const int closed = std::fclose(file);
    if (failed || closed) {
        std::fprintf(stderr, "Settings: cannot write %s\n", DisplayPath(path).c_str());
    }
}

float PresetScale(int preset) {
    static constexpr float scales[PresetCount] = {1.0f, 1.5f, 1.7f, 2.0f, 3.0f};
    return scales[std::clamp(preset, 0, PresetCount - 1)];
}

const char* PresetName(int preset) {
    static constexpr const char* names[PresetCount] = {"Native AA", "Quality", "Balanced",
                                                       "Performance", "Ultra Performance"};
    return names[std::clamp(preset, 0, PresetCount - 1)];
}

const char* UpscalerName(int upscaler) {
    static constexpr const char* names[UpscalerCount] = {"off", "fsr3", "fsr4", "fsr411", "taa", "dlss"};
    return names[std::clamp(upscaler, 0, UpscalerCount - 1)];
}

} // namespace WipeoutOCSettings
