// Native entry points use the same configuration policy as the launcher and runner.
#include <cassert>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include "wipeout_oc_settings.h"

namespace fs = std::filesystem;
static void Env(const char* name, const std::string& value) {
#ifdef _WIN32
    _wputenv_s(fs::path(reinterpret_cast<const char8_t*>(name)).c_str(),
               fs::path(reinterpret_cast<const char8_t*>(value.c_str())).c_str());
#else
    setenv(name, value.c_str(), 1);
#endif
}
static std::string Read(const fs::path& path) {
    std::ifstream stream(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), {}};
}
static void Write(const fs::path& path, const std::string& bytes) {
    std::ofstream stream(path, std::ios::binary);
    stream << bytes;
    assert(stream.good());
}

int main(int argc, char** argv) {
    const bool play = argc >= 2 && std::string(argv[1]) == "--play";
    if (argc >= 2 && (std::string(argv[1]) == "--load" || play)) {
#ifdef _WIN32
        if (play) {
            // Stand in for the GUI child of the real Play stub, recording the inherited path.
            const auto value = _wgetenv(L"WIPEOUT_OC_CONFIG");
            const auto selected = fs::path(value ? value : L"").u8string();
            std::ofstream trace("play-config.txt", std::ios::binary);
            trace.write(reinterpret_cast<const char*>(selected.data()), selected.size());
        }
#endif
        if (!WipeoutOCSettings::Load()) return 1;
        if (play || (argc == 3 && std::string(argv[2]) == "--save")) WipeoutOCSettings::Save();
        return 0;
    }
    assert(argc <= 2);
    const auto start = fs::current_path();
    const bool own_directory = argc == 1;
    const auto root = own_directory ? fs::temp_directory_path() /
        ("wipeout-settings-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())) :
        fs::absolute(fs::path(reinterpret_cast<const char8_t*>(argv[1])));
    fs::create_directories(root);
    fs::current_path(root);
    Env("WIPEOUT_OC_CONFIG", "");
    Env("GOW3_CONFIG", "");
    Env("WIPEOUT_OC_DATA_DIR", "");
    Env("GOW3_DATA_DIR", "");
    const std::string original = "# original\r\nrender_resolution=4K\r\noutput_res=3840x2160\r\nps4_pro=0\r\nneo=0\r\nunknown=keep\r\n";
    Write("gow3.ini", original);
    assert(WipeoutOCSettings::Load());
    assert(Read("wipeout_oc.ini") == original);
    assert(Read("gow3.ini") == original);
    // Both files exist; the new one wins and the old one is never modified by Save.
    Write("wipeout_oc.ini", "engine_fps=240\n");
    assert(WipeoutOCSettings::Load());
    assert(WipeoutOCSettings::Get().engine_fps == 240);
    fs::create_directory("other");
    fs::current_path("other");
    WipeoutOCSettings::Save();
    assert(!fs::exists("wipeout_oc.ini"));
    assert(Read(root / "wipeout_oc.ini").find("engine_fps=240\n") != std::string::npos);
    assert(Read(root / "gow3.ini") == original);
    // An explicit missing path neither loads nor migrates an unrelated legacy file.
    Env("WIPEOUT_OC_CONFIG", "chosen.ini");
    Write("gow3.ini", original);
    assert(WipeoutOCSettings::Load());
    assert(!fs::exists("chosen.ini") && !fs::exists("wipeout_oc.ini"));
    WipeoutOCSettings::Save();
    assert(fs::exists("chosen.ini"));
    assert(Read("gow3.ini") == original);
    // Native data override, including Unicode, selects that directory without a binary-path guess.
    const auto data = root / fs::path(u8"datos-ó-Ω");
    fs::create_directory(data);
    Write(data / "gow3.ini", original);
    Env("WIPEOUT_OC_CONFIG", "");
    const auto utf8 = data.u8string();
    Env("WIPEOUT_OC_DATA_DIR", {reinterpret_cast<const char*>(utf8.data()), utf8.size()});
    assert(WipeoutOCSettings::Load());
    assert(Read(data / "wipeout_oc.ini") == original);
    // A readable, readonly legacy file can migrate in a writable directory; only the copy
    // becomes writable, so the native Save never needs to change the original's permissions.
    fs::remove(data / "wipeout_oc.ini");
    fs::permissions(data / "gow3.ini", fs::perms::owner_read, fs::perm_options::replace);
    assert(WipeoutOCSettings::Load());
    assert(Read(data / "wipeout_oc.ini") == original);
    WipeoutOCSettings::Save();
    assert(Read(data / "gow3.ini") == original);
    assert((fs::status(data / "gow3.ini").permissions() & fs::perms::owner_write) == fs::perms::none);
    fs::permissions(data / "gow3.ini", fs::perms::owner_write, fs::perm_options::add);
    WipeoutOCSettings::Save();
    assert(Read(data / "gow3.ini") == original);
    // I/O errors are distinct from absence and must fail startup.
    Env("WIPEOUT_OC_CONFIG", {reinterpret_cast<const char*>(utf8.data()), utf8.size()});
    assert(!WipeoutOCSettings::Load());
    // A failed default migration is retried, rather than cached as an absent config.
    Env("WIPEOUT_OC_CONFIG", "");
    fs::remove(data / "wipeout_oc.ini");
    fs::remove(data / "gow3.ini");
    fs::create_directory(data / "gow3.ini");
    assert(!WipeoutOCSettings::Load());
    fs::remove(data / "gow3.ini");
    Write(data / "gow3.ini", original);
    assert(WipeoutOCSettings::Load());
    assert(Read(data / "wipeout_oc.ini") == original);
    fs::current_path(start);
    if (own_directory) fs::remove_all(root);
}
