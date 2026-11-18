// Owned native consumer: no GUI, input, permission prompt, model or user history.
#include "MaiChatTools/DesktopComputerPlatform.h"
#include "MaiChatTools/DesktopComputerTools.h"
#include <ApplicationServices/ApplicationServices.h>
#include <iostream>
#include <unistd.h>

int main() {
    const bool trusted = AXIsProcessTrusted(), capture = CGPreflightScreenCaptureAccess();
    if (trusted || capture) { std::cout << "permission fixture changed; no positive-path test attempted\n"; return 77; }
    int checks = 0, failed = 0;
    auto check = [&](bool value, const char* name) {
        ++checks; if (!value) { ++failed; std::cerr << "FAIL " << name << '\n'; }
    };
    // The provider must report unavailable before examining any native object.
    DesktopNativeWindow target; target.id = 1; target.processId = std::uint64_t(getpid());
    target.title = "owned-unavailable-descriptor";
    auto snapshot = maiDesktopInspectElements(target);
    check(snapshot.isOk(), "observation supports coordinate-only degradation");
    if (snapshot) {
        const auto& value = snapshot.value();
        check(value.provider == "mac_ax" && value.status == "unavailable" &&
            value.limitReason == "not_authorized", "honest provider availability");
        check(!value.countsKnown && value.visitedNodes == 0 && value.elements.empty() && !value.truncated,
            "unavailable provider does not claim traversal or actionable references");
    }
    auto legacy = maiDesktopElements(target);
    check(!legacy && legacy.error().code() == MaiErrorCode::NotConfigured, "legacy permission error preserved");
    DesktopNativeElement empty;
    auto fresh = maiDesktopResolveElement(target, empty);
    check(!fresh && fresh.error().code() == MaiErrorCode::NotConfigured, "resolution cannot bypass AX");
    check(maiDesktopSetWindowState(target, DesktopWindowStateAction::Minimize).code() == MaiErrorCode::NotConfigured,
        "minimize cannot bypass AX");
    check(maiDesktopSetWindowState(target, DesktopWindowStateAction::Restore).code() == MaiErrorCode::NotConfigured,
        "restore cannot bypass AX");
    const auto canceled = [] { return true; };
    const auto observedCanceled = maiDesktopInspectElements(target, canceled);
    check(!observedCanceled && observedCanceled.error().code() == MaiErrorCode::Canceled, "inspect canceled before provider");
    const auto resolvedCanceled = maiDesktopResolveElement(target, empty, canceled);
    check(!resolvedCanceled && resolvedCanceled.error().code() == MaiErrorCode::Canceled, "resolve canceled before provider");
    check(maiDesktopSetWindowState(target, DesktopWindowStateAction::Minimize, canceled).code() == MaiErrorCode::Canceled,
        "state canceled before provider/dispatch");
    DesktopNativeWindow invalid;
    const auto invalidObservation = maiDesktopInspectElements(invalid);
    check(!invalidObservation && invalidObservation.error().code() == MaiErrorCode::InvalidInput,
        "invalid new observation identity rejected");
    check(maiDesktopSetWindowState(target, static_cast<DesktopWindowStateAction>(42)).code() == MaiErrorCode::InvalidInput,
        "invalid state enum rejected without effects");
    // Instantiate the final shared Controller, forcing its new native vtable and
    // all three free functions into the same consumer link. No old DTO archive.
    DesktopComputerController controller;
    auto windows = controller.windows();
    check(!windows && windows.error().code() == MaiErrorCode::NotConfigured, "same-ABI Controller reaches screen permission guard");
    check(!AXIsProcessTrusted() && !CGPreflightScreenCaptureAccess(), "permissions unchanged");
    std::cout << "{\"checks\":" << checks << ",\"failed\":" << failed
              << ",\"AXTrusted\":false,\"screenCapture\":false,\"positiveAXStateTested\":false}\n";
    return failed ? 1 : 0;
}
