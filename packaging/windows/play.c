/* Play WipEout.exe: starts the game with the settings saved by the launcher, without opening
 * the launcher (it runs `WipEout.exe --play` from its own folder and waits for it, so Steam
 * and other front ends see the game running). Extra arguments are passed on. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wchar.h>

static int IsSeparator(wchar_t value) {
    return value == L'\\' || value == L'/';
}

static int AnchorConfiguration(void) {
    static wchar_t value[32768], base[32768], absolute[32768];
    SetLastError(ERROR_SUCCESS);
    DWORD length = GetEnvironmentVariableW(L"WIPEOUT_OC_CONFIG", value, 32768);
    if (!length && GetLastError() == ERROR_ENVVAR_NOT_FOUND) {
        SetLastError(ERROR_SUCCESS);
        length = GetEnvironmentVariableW(L"GOW3_CONFIG", value, 32768);
    }
    if (!length) return GetLastError() == ERROR_SUCCESS || GetLastError() == ERROR_ENVVAR_NOT_FOUND;
    if (length >= 32768) return 0;
    const int drive = ((value[0] >= L'A' && value[0] <= L'Z') ||
                       (value[0] >= L'a' && value[0] <= L'z')) && value[1] == L':';
    if ((drive && IsSeparator(value[2])) || (IsSeparator(value[0]) && IsSeparator(value[1]))) {
        return SetEnvironmentVariableW(L"WIPEOUT_OC_CONFIG", value);
    }
    const wchar_t* tail = value;
    if (drive) {
        const wchar_t prefix[] = {value[0], L':', L'.', 0};
        length = GetFullPathNameW(prefix, 32768, base, NULL);
        tail += 2;
    } else if (IsSeparator(value[0])) {
        length = GetFullPathNameW(L"\\", 32768, base, NULL);
        ++tail;
    } else {
        length = GetCurrentDirectoryW(32768, base);
    }
    if (!length || length >= 32768 || length + wcslen(tail) + 2 > 32768) return 0;
    // Only resolve the base; keep the caller's symlinks and '..' components in the override.
    if (swprintf(absolute, 32768, L"%ls%ls%ls", base, IsSeparator(base[length - 1]) ? L"" : L"\\", tail) < 0)
        return 0;
    return SetEnvironmentVariableW(L"WIPEOUT_OC_CONFIG", absolute);
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE previous, PWSTR arguments, int show)
{
    (void)instance, (void)previous, (void)show;
    if (!AnchorConfiguration()) {
        MessageBoxW(NULL, L"Could not prepare the configuration path. Check WIPEOUT_OC_CONFIG (or GOW3_CONFIG) and its length.",
                    L"WipEout", MB_OK | MB_ICONERROR);
        return 1;
    }
    static wchar_t dir[32768], exe[32768], line[32768];
    DWORD length = GetModuleFileNameW(NULL, dir, 32768);
    if (length == 0 || length >= 32768)
        return 1;
    wchar_t *slash = wcsrchr(dir, L'\\');
    if (slash)
        *slash = 0;
    swprintf(exe, 32768, L"%ls\\WipEout.exe", dir);
    swprintf(line, 32768, L"\"%ls\" --play%ls%ls", exe, arguments && *arguments ? L" " : L"",
             arguments ? arguments : L"");

    STARTUPINFOW startup = {.cb = sizeof(startup)};
    PROCESS_INFORMATION process;
    if (!CreateProcessW(exe, line, NULL, NULL, FALSE, 0, NULL, dir, &startup, &process)) {
        MessageBoxW(NULL, L"WipEout.exe was not found next to this file.", L"WipEout",
                    MB_OK | MB_ICONERROR);
        return 1;
    }
    CloseHandle(process.hThread);
    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(process.hProcess, &code);
    CloseHandle(process.hProcess);
    return (int)code;
}
