// Native .app catalog and Launch Services identity launch. No shell commands.
#include "MaiChatTools/DesktopComputerPlatform.h"
#include "MaiFilePath.h"
#include "MaiFileSystem.h"

#import <AppKit/AppKit.h>
#import <ApplicationServices/ApplicationServices.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace {

bool canceled(const std::function<bool()>& callback) {
    return callback && callback();
}
MaiError canceledError() {
    return {MaiErrorCode::Canceled, "application operation canceled before dispatch"};
}

std::string utf8(NSString* value) {
    if (![value isKindOfClass:NSString.class]) return {};
    const char* bytes = value.UTF8String;
    return bytes ? bytes : "";
}

MaiResult<std::string> literalIdentity(const std::string& input) {
    if (input.empty() || input.size() > 32767 || input.find('\0') != std::string::npos)
        return {MaiErrorCode::InvalidInput, "app must be a bundle identity or absolute .app path"};
    NSString* value = [NSString stringWithUTF8String:input.c_str()];
    if (!value) return {MaiErrorCode::InvalidInput, "app must contain valid UTF-8"};
    if ([value rangeOfCharacterFromSet:NSCharacterSet.controlCharacterSet].location != NSNotFound ||
        [value rangeOfString:@"\""].location != NSNotFound)
        return {MaiErrorCode::InvalidInput,
                "app must be a literal bundle identity without quoting or control characters"};
    return input;
}

std::string bundleIdentity(NSString* identifier, NSURL* bundleUrl) {
    const auto id = utf8(identifier);
    if (!id.empty()) return id;
    if (!bundleUrl.isFileURL) return {};
    return utf8(bundleUrl.URLByResolvingSymlinksInPath.path);
}

bool applicationBundle(NSBundle* bundle) {
    // NSBundle also represents frameworks/documents. Only executable .app
    // bundles are launch targets; never ask Workspace to open arbitrary files.
    if (!bundle || ![bundle.bundleURL.pathExtension.lowercaseString isEqualToString:@"app"] ||
        !bundle.executableURL.isFileURL)
        return false;
    return MaiFileSystem::exists(MaiFilePath::fromUtf8(utf8(bundle.executableURL.path)));
}

void addBundle(std::vector<DesktopNativeApplication>& output, NSBundle* bundle) {
    if (!applicationBundle(bundle)) return;
    const auto id = bundleIdentity(bundle.bundleIdentifier, bundle.bundleURL);
    if (id.empty()) return;
    if (std::find_if(output.begin(), output.end(), [&](const auto& app) { return app.id == id; }) != output.end())
        return;
    DesktopNativeApplication app;
    app.id = id;
    NSString* name = [bundle objectForInfoDictionaryKey:@"CFBundleDisplayName"];
    if (![name isKindOfClass:NSString.class] || !name.length)
        name = [bundle objectForInfoDictionaryKey:@"CFBundleName"];
    if (![name isKindOfClass:NSString.class] || !name.length)
        name = bundle.bundleURL.lastPathComponent.stringByDeletingPathExtension;
    if (name.length) app.displayName = utf8(name);
    output.push_back(std::move(app));
}

// The main queue owns Workspace launch. A timed-out queued block observes live
// before dispatch and cannot launch later. Once native dispatch starts we wait
// for its actual result, so cancellation never reports a false "not launched".
struct LaunchTicket {
    std::mutex mutex;
    std::condition_variable ready;
    std::function<bool()> isCanceled;
    bool live = true;
    bool started = false;
    bool done = false;
    MaiError error;
};

MaiError launchOnMain(NSURL* url, const std::function<bool()>& isCanceled) {
    auto ticket = std::make_shared<LaunchTicket>();
    ticket->isCanceled = isCanceled;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(250);
    auto operation = ^{
      @autoreleasepool {
          {
              std::lock_guard<std::mutex> lock(ticket->mutex);
              if (!ticket->live) return;
              if (std::chrono::steady_clock::now() >= deadline) {
                  ticket->error = {MaiErrorCode::Busy, "application launch expired on the main queue before dispatch"};
              } else if (canceled(ticket->isCanceled)) {
                  ticket->error = canceledError();
              } else {
                  ticket->started = true;
              }
              // The block never calls or retains caller cancellation after
              // dispatch starts; no callback outlives this ordinary API call.
              ticket->isCanceled = {};
              if (!ticket->started) {
                  ticket->done = true;
                  ticket->ready.notify_all();
                  return;
              }
          }
          NSError* error = nil;
          // This synchronous native API reports Launch Services' result. It
          // passes no arguments and does not imply a window/focus guarantee.
          NSRunningApplication* launched = [NSWorkspace.sharedWorkspace launchApplicationAtURL:url
                                                                                       options:NSWorkspaceLaunchDefault
                                                                                 configuration:@{}
                                                                                         error:&error];
          std::lock_guard<std::mutex> lock(ticket->mutex);
          if (!launched)
              ticket->error = {MaiErrorCode::Internal,
                               "macOS rejected the application launch: " + utf8(error.localizedDescription)};
          ticket->done = true;
          ticket->ready.notify_all();
      }
    };
    if ([NSThread isMainThread])
        operation();
    else
        dispatch_async(dispatch_get_main_queue(), operation);
    for (;;) {
        std::unique_lock<std::mutex> lock(ticket->mutex);
        if (ticket->done) return ticket->error;
        if (!ticket->started) {
            if (canceled(ticket->isCanceled)) {
                ticket->live = false;
                ticket->isCanceled = {};
                return canceledError();
            }
            if (std::chrono::steady_clock::now() >= deadline) {
                ticket->live = false;
                ticket->isCanceled = {};
                return {MaiErrorCode::Busy, "application main queue was unavailable; launch was not dispatched"};
            }
        }
        ticket->ready.wait_for(lock, std::chrono::milliseconds(5));
    }
}

}  // namespace

MaiResult<std::string> maiDesktopApplicationId(const DesktopNativeWindow& window) {
    @autoreleasepool {
        // Resolve the live CG window PID before asking Workspace. Reused PIDs or
        // display names alone must never rebind a stale window to another app.
        CFArrayRef details = CGWindowListCopyWindowInfo(kCGWindowListOptionIncludingWindow, CGWindowID(window.id));
        bool matches = false;
        if (details) {
            for (CFIndex index = 0; index < CFArrayGetCount(details); ++index) {
                auto dictionary = static_cast<CFDictionaryRef>(CFArrayGetValueAtIndex(details, index));
                if (!dictionary || CFGetTypeID(dictionary) != CFDictionaryGetTypeID()) continue;
                const auto pidValue = static_cast<CFNumberRef>(CFDictionaryGetValue(dictionary, kCGWindowOwnerPID));
                const auto idValue = static_cast<CFNumberRef>(CFDictionaryGetValue(dictionary, kCGWindowNumber));
                std::int64_t pid = 0;
                std::int64_t id = 0;
                if (pidValue && idValue && CFGetTypeID(pidValue) == CFNumberGetTypeID() &&
                    CFGetTypeID(idValue) == CFNumberGetTypeID() &&
                    CFNumberGetValue(pidValue, kCFNumberSInt64Type, &pid) &&
                    CFNumberGetValue(idValue, kCFNumberSInt64Type, &id) && std::uint64_t(pid) == window.processId &&
                    std::uint64_t(id) == window.id)
                    matches = true;
            }
            CFRelease(details);
        }
        if (!matches) return {MaiErrorCode::NotFound, "window application identity is no longer available"};
        NSRunningApplication* app =
            [NSRunningApplication runningApplicationWithProcessIdentifier:pid_t(window.processId)];
        const auto id = bundleIdentity(app.bundleIdentifier, app.bundleURL);
        if (id.empty()) return {MaiErrorCode::NotFound, "window owner has no native application bundle identity"};
        return id;
    }
}

MaiResult<std::vector<DesktopNativeApplication>> maiDesktopApplications(const std::function<bool()>& isCanceled) {
    if (canceled(isCanceled)) return canceledError();
    @autoreleasepool {
        std::vector<DesktopNativeApplication> output;
        std::vector<std::string> roots = {"/Applications", "/System/Applications", "/System/Library/CoreServices"};
        const auto userRoot = utf8([NSHomeDirectory() stringByAppendingPathComponent:@"Applications"]);
        if (!userRoot.empty()) roots.push_back(userRoot);
        MaiError scanError;
        for (const auto& root : roots) {
            const auto path = MaiFilePath::fromUtf8(root);
            if (!MaiFileSystem::exists(path)) continue;
            MaiFileSystem::walk(
                path,
                [&](const MaiFileEntry& entry) {
                    if (canceled(isCanceled)) return MaiWalkAction::Stop;
                    NSString* nativePath = [NSString stringWithUTF8String:entry.path.toUtf8().c_str()];
                    if (![nativePath.pathExtension.lowercaseString isEqualToString:@"app"])
                        return MaiWalkAction::Continue;
                    // Public application folders also contain .app symlinks (for
                    // example system-managed Safari); NSBundle resolves those.
                    addBundle(output, [NSBundle bundleWithPath:nativePath]);
                    // Nested helpers are part of their parent app, not independently
                    // installed applications. Do not descend into application data.
                    return MaiWalkAction::SkipDirectory;
                },
                [&](const MaiFilePath&, const MaiError& error) {
                    if (!scanError) scanError = error;
                });
            if (canceled(isCanceled)) return canceledError();
        }
        // A partial catalog must not silently claim installed-app completeness.
        if (scanError) return scanError;
        NSArray<NSRunningApplication*>* running = NSWorkspace.sharedWorkspace.runningApplications;
        for (NSRunningApplication* nativeApp in running) {
            if (canceled(isCanceled)) return canceledError();
            if (nativeApp.bundleURL) addBundle(output, [NSBundle bundleWithURL:nativeApp.bundleURL]);
        }
        for (auto& app : output) app.isRunning = false;
        for (NSRunningApplication* nativeApp in running) {
            const auto id = bundleIdentity(nativeApp.bundleIdentifier, nativeApp.bundleURL);
            for (auto& app : output)
                if (app.id == id) app.isRunning = true;
        }
        const auto windows = maiDesktopWindows();
        if (!windows) return windows.error();
        for (const auto& window : windows.value()) {
            if (canceled(isCanceled)) return canceledError();
            const auto id = maiDesktopApplicationId(window);
            if (!id) continue;  // The window may have disappeared during discovery.
            auto app =
                std::find_if(output.begin(), output.end(), [&](const auto& entry) { return entry.id == id.value(); });
            if (app == output.end()) {
                DesktopNativeApplication discovered;
                discovered.id = id.value();
                output.push_back(std::move(discovered));
                app = output.end() - 1;
            }
            app->isRunning = true;
            app->windows.push_back(window);
        }
        if (canceled(isCanceled)) return canceledError();
        return output;
    }
}

MaiError maiDesktopLaunchApplication(const std::string& app, const std::function<bool()>& isCanceled) {
    if (canceled(isCanceled)) return canceledError();
    @autoreleasepool {
        const auto literal = literalIdentity(app);
        if (!literal) return literal.error();
        NSString* identity = [NSString stringWithUTF8String:literal.value().c_str()];
        NSURL* url = nil;
        if (identity.isAbsolutePath) {
            if (![identity.pathExtension.lowercaseString isEqualToString:@"app"])
                return {MaiErrorCode::InvalidInput,
                        "an explicit app path must name an executable .app bundle without arguments"};
            url = [NSURL fileURLWithPath:identity isDirectory:YES];
        } else {
            if ([identity rangeOfCharacterFromSet:NSCharacterSet.whitespaceCharacterSet].location != NSNotFound ||
                [identity rangeOfString:@"/"].location != NSNotFound ||
                [identity rangeOfString:@":"].location != NSNotFound ||
                [identity rangeOfString:@"\\"].location != NSNotFound)
                return {MaiErrorCode::InvalidInput,
                        "app must be a native bundle identity or absolute .app path without arguments"};
            url = [NSWorkspace.sharedWorkspace URLForApplicationWithBundleIdentifier:identity];
        }
        NSBundle* bundle = url ? [NSBundle bundleWithURL:url] : nil;
        if (!applicationBundle(bundle)) return {MaiErrorCode::NotFound, "native application bundle was not found"};
        if (canceled(isCanceled)) return canceledError();
        return launchOnMain(bundle.bundleURL, isCanceled);
    }
}
