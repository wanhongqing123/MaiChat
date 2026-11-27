// Safe native catalog/validation regression: never launches an application.
#include "MaiChatTools/DesktopComputerPlatform.h"

#include <windows.h>
#include <cstdio>

namespace {
DesktopNativeWindow fixture;
bool check(bool value, const char* message) {
    if (!value) std::fprintf(stderr, "%s\n", message);
    return value;
}
}

MaiResult<std::vector<DesktopNativeWindow>> maiDesktopWindows() {
    return std::vector<DesktopNativeWindow>{fixture};
}

int main() {
    bool passed = true;
    const auto canceled = [] { return true; };
    passed &= check(maiDesktopApplications(canceled).error().code() == MaiErrorCode::Canceled,
                    "catalog must honor cancellation before native enumeration");
    passed &= check(maiDesktopLaunchApplication("C:\\does-not-exist.exe", canceled).code() == MaiErrorCode::Canceled,
                    "launch must honor cancellation before dispatch");
    const std::vector<std::string> invalid = {
        "", "relative.exe", "cmd.exe /c echo unsafe", "\"C:\\test.exe\"", "C:\\test.exe --argument",
        std::string("test\0.exe", 9), std::string("\xff", 1), "test\nidentity", "test\tidentity",
        "shell:AppsFolder\\anything", "https://example.invalid"
    };
    for (const auto& app : invalid)
        passed &= check(maiDesktopLaunchApplication(app).code() == MaiErrorCode::InvalidInput,
                        "invalid app identity must fail without dispatch");
    fixture.id = 123;
    fixture.processId = GetCurrentProcessId();
    wchar_t path[32768]{};
    const auto length = GetModuleFileNameW(nullptr, path, 32768);
    const auto bytes = WideCharToMultiByte(CP_UTF8, 0, path, int(length), nullptr, 0, nullptr, nullptr);
    fixture.application.resize(bytes);
    WideCharToMultiByte(CP_UTF8, 0, path, int(length), fixture.application.data(), bytes, nullptr, nullptr);
    const HWND ownedWindow = CreateWindowExW(0, L"STATIC", L"MaiChat application identity fixture", 0,
                                             0, 0, 10, 10, HWND_MESSAGE, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!check(ownedWindow != nullptr, "owned invisible fixture window must be created")) return 1;
    fixture.id = std::uint64_t(reinterpret_cast<std::uintptr_t>(ownedWindow));
    const auto nativeIdentity = maiDesktopApplicationId(fixture);
    passed &= check(nativeIdentity.isOk() && nativeIdentity.value() == fixture.application,
                    "application identity must come from the owned window's native process");
    auto forged = fixture;
    ++forged.processId;
    passed &= check(maiDesktopApplicationId(forged).error().code() == MaiErrorCode::NotFound,
                    "a forged window owner must not resolve an application");
    const auto catalog = maiDesktopApplications();
    passed &= check(catalog.isOk(), "native installed application catalog must be readable");
    bool associated = false;
    if (catalog) for (const auto& app : catalog.value()) {
        if (app.id != fixture.application) continue;
        associated = app.windows.size() == 1 && app.windows.front().id == fixture.id && app.isRunning == true;
        passed &= check(!app.lastUsedDate && !app.useCount, "unknown usage data must remain absent");
    }
    passed &= check(associated, "real native executable identity must preserve the associated window");
    DestroyWindow(ownedWindow);
    passed &= check(maiDesktopApplicationId(fixture).error().code() == MaiErrorCode::NotFound,
                    "destroyed window application identity must not rebind");
    return passed ? 0 : 1;
}
