#include "MaiChatTools/DesktopComputerPlatform.h"

#include <windows.h>
#include <commctrl.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <optional>
#include <thread>

namespace {

int checks = 0;
int failures = 0;

void check(bool condition, const char* name) {
    ++checks;
    if (!condition) {
        ++failures;
        std::printf("FAIL %s\n", name);
    }
}

std::optional<DesktopNativeElement> observedControl(const DesktopNativeAccessibilitySnapshot& snapshot, HWND control) {
    RECT frame{};
    GetWindowRect(control, &frame);
    for (const auto& element : snapshot.elements) {
        const double centerX = (frame.left + frame.right) / 2.0;
        const double centerY = (frame.top + frame.bottom) / 2.0;
        if (element.role == "edit" && centerX >= element.x && centerY >= element.y &&
            centerX < element.x + element.width && centerY < element.y + element.height)
            return element;
    }
    return std::nullopt;
}

LRESULT CALLBACK fixtureProcedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
    return DefWindowProcW(window, message, wparam, lparam);
}

}  // namespace

int main() {
    // 只创建测试自有、不激活的窗口；不注入键盘、鼠标或读取用户屏幕。
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const HWND originalForeground = GetForegroundWindow();
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSW windowClass{};
    windowClass.lpfnWndProc = fixtureProcedure;
    windowClass.hInstance = instance;
    windowClass.lpszClassName = L"MaiChatElementNativeFixture";
    RegisterClassW(&windowClass);
    const HWND window = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, windowClass.lpszClassName,
                                        L"MaiChat Element Native Fixture", WS_OVERLAPPEDWINDOW, 80, 80, 520, 320,
                                        nullptr, nullptr, instance, nullptr);
    if (!window) return 2;
    const HWND editable = CreateWindowExW(0, L"EDIT", L"initial", WS_CHILD | WS_VISIBLE | WS_BORDER, 10, 10, 220, 28,
                                          window, reinterpret_cast<HMENU>(101), instance, nullptr);
    const HWND password = CreateWindowExW(0, L"EDIT", L"protected", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_PASSWORD, 10,
                                          50, 220, 28, window, reinterpret_cast<HMENU>(102), instance, nullptr);
    const HWND disabled = CreateWindowExW(0, L"EDIT", L"disabled", WS_CHILD | WS_VISIBLE | WS_BORDER | WS_DISABLED, 10,
                                          90, 220, 28, window, reinterpret_cast<HMENU>(103), instance, nullptr);
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_TREEVIEW_CLASSES};
    InitCommonControlsEx(&controls);
    const HWND tree = CreateWindowExW(0, WC_TREEVIEWW, L"", WS_CHILD | WS_VISIBLE | WS_BORDER | TVS_HASBUTTONS, 250, 10,
                                      230, 200, window, reinterpret_cast<HMENU>(104), instance, nullptr);
    TVINSERTSTRUCTW parent{};
    parent.hParent = TVI_ROOT;
    parent.hInsertAfter = TVI_LAST;
    parent.item.mask = TVIF_TEXT;
    parent.item.pszText = const_cast<wchar_t*>(L"Expandable parent");
    const auto parentItem = reinterpret_cast<HTREEITEM>(SendMessageW(tree, TVM_INSERTITEMW, 0, LPARAM(&parent)));
    TVINSERTSTRUCTW child = parent;
    child.hParent = parentItem;
    child.item.pszText = const_cast<wchar_t*>(L"Child");
    SendMessageW(tree, TVM_INSERTITEMW, 0, LPARAM(&child));
    SendMessageW(tree, TVM_SELECTITEM, TVGN_CARET, LPARAM(parentItem));
    ShowWindow(window, SW_SHOWNOACTIVATE);
    UpdateWindow(window);
    check(GetForegroundWindow() == originalForeground, "fixture does not activate");
    std::atomic<bool> done{false};
    std::atomic<bool> canceled{false};
    std::thread worker([&] {
        const auto windows = maiDesktopWindows();
        std::optional<DesktopNativeWindow> target;
        if (windows) {
            for (const auto& candidate : windows.value()) {
                if (candidate.id == std::uint64_t(reinterpret_cast<std::uintptr_t>(window))) target = candidate;
            }
        }
        check(bool(target), "test-owned window is discovered");
        if (target) {
            const auto cancellation = [&] { return canceled.load(); };
            const auto snapshot = maiDesktopInspectElements(*target, cancellation);
            check(bool(snapshot), "UIA snapshot succeeds");
            if (snapshot) {
                std::printf("provider=%s status=%s nodes=%zu elements=%zu reason=%s\n",
                            snapshot.value().provider.c_str(), snapshot.value().status.c_str(),
                            snapshot.value().visitedNodes, snapshot.value().elements.size(),
                            snapshot.value().limitReason.c_str());
                const auto edit = observedControl(snapshot.value(), editable);
                const auto secure = observedControl(snapshot.value(), password);
                const auto inactive = observedControl(snapshot.value(), disabled);
                check(bool(edit), "editable element is observed");
                check(bool(secure), "password element is observed");
                check(bool(inactive), "disabled element is observed");
                check(!snapshot.value().focusedElement, "background fixture does not invent a focused element");
                check(snapshot.value().selectedElements && !snapshot.value().selectedElements->empty(),
                      "native tree selection is reported");
                if (edit) {
                    check(edit->depth > 0, "child depth is reported");
                    const auto set = maiDesktopSetElementValue(*target, *edit, "replacement", cancellation);
                    if (set) std::printf("set_value error=%s\n", set.message().c_str());
                    check(!set, "native ValuePattern replaces the test value");
                    check(maiDesktopPerformSecondaryAction(*target, *edit, "invented", cancellation).code() ==
                              MaiErrorCode::NotSupported,
                          "unobserved secondary action is rejected");
                    auto stale = *edit;
                    stale.x += 1;
                    check(maiDesktopSetElementValue(*target, stale, "stale", cancellation).code() == MaiErrorCode::Busy,
                          "stale observed bounds reject mutation");
                    check(maiDesktopSetElementValue(*target, *edit, "canceled", [] { return true; }).code() ==
                              MaiErrorCode::Canceled,
                          "canceled native value operation has no dispatch");
                    check(maiDesktopSetElementValue(*target, *edit, std::string(1, char(0xff)), cancellation).code() ==
                              MaiErrorCode::InvalidInput,
                          "invalid UTF-8 is rejected before provider dispatch");
                    auto wrongProcess = *target;
                    ++wrongProcess.processId;
                    check(maiDesktopSetElementValue(wrongProcess, *edit, "wrong process", cancellation).code() ==
                              MaiErrorCode::NotFound,
                          "stale window process identity is rejected");
                    SendMessageW(editable, EM_SETREADONLY, TRUE, 0);
                    const auto fresh = maiDesktopInspectElements(*target, cancellation);
                    const auto readOnly = fresh ? observedControl(fresh.value(), editable) : std::nullopt;
                    check(readOnly && maiDesktopSetElementValue(*target, *readOnly, "read only", cancellation).code() ==
                                          MaiErrorCode::NotSupported,
                          "read-only native value is rejected");
                }
                if (secure)
                    check(maiDesktopSetElementValue(*target, *secure, "forbidden", cancellation).code() ==
                              MaiErrorCode::NotSupported,
                          "password value mutation is rejected");
                if (inactive)
                    check(maiDesktopSetElementValue(*target, *inactive, "forbidden", cancellation).code() ==
                              MaiErrorCode::NotSupported,
                          "disabled value mutation is rejected");
                bool expanded = false;
                for (const auto& element : snapshot.value().elements) {
                    for (const auto& action : element.secondaryActions) {
                        if (action == "Expand") {
                            const auto result =
                                maiDesktopPerformSecondaryAction(*target, element, "eXpAnD", cancellation);
                            if (result) std::printf("expand error=%s\n", result.message().c_str());
                            check(!result, "observed Expand action executes case-insensitively");
                            expanded = !result;
                            break;
                        }
                    }
                    if (expanded) break;
                }
                check(expanded, "native tree reports a supported secondary action");
                const auto updated = maiDesktopInspectElements(*target, cancellation);
                bool collapse = false;
                if (updated) {
                    for (const auto& element : updated.value().elements) {
                        for (const auto& action : element.secondaryActions) {
                            if (action == "Collapse") {
                                check(!maiDesktopPerformSecondaryAction(*target, element, action, cancellation),
                                      "fresh Collapse action executes after Expand");
                                collapse = true;
                                break;
                            }
                        }
                        if (collapse) break;
                    }
                }
                check(collapse, "secondary actions reflect current native state");
            }
        }
        done = true;
    });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (!done) {
        if (std::chrono::steady_clock::now() >= deadline) canceled = true;
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        MsgWaitForMultipleObjects(0, nullptr, FALSE, 10, QS_ALLINPUT);
    }
    worker.join();
    wchar_t value[128]{};
    GetWindowTextW(editable, value, 128);
    check(std::wstring(value) == L"replacement", "native value readback excludes stale and canceled writes");
    GetWindowTextW(password, value, 128);
    check(std::wstring(value) == L"protected", "password value is unchanged");
    GetWindowTextW(disabled, value, 128);
    check(std::wstring(value) == L"disabled", "disabled value is unchanged");
    check(GetForegroundWindow() == originalForeground, "native operations preserve foreground");
    DestroyWindow(window);
    std::printf("checks=%d failures=%d\n", checks, failures);
    return failures ? 1 : 0;
}
