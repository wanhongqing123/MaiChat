// Cross-call ownership for ordinary desktop input. Capture does not own or
// reset this state; model protocols are adapted by DesktopComputerController.
#pragma once

#include "MaiChatTools/DesktopComputerBackend.h"
#include "MaiChatTools/DesktopComputerTools.h"

#include <chrono>
#include <condition_variable>
#include <map>
#include <mutex>
#include <thread>

class DesktopComputerInputOwnership {
public:
    DesktopComputerInputOwnership(std::shared_ptr<DesktopComputerBackend> backend, std::function<void()> invalidate,
                                  std::chrono::milliseconds timeout = std::chrono::seconds(120));
    ~DesktopComputerInputOwnership();
    MaiResult<std::shared_ptr<DesktopNativeInputSession>> prepare(const std::string& session,
                                                                  const DesktopComputerAction& action, bool create,
                                                                  bool releasing = false);
    void touch(const std::string& session, const std::string& expectedTurn = {});
    DesktopNativeInputState state(const std::string& session) const;
    MaiResult<DesktopInputFeedbackBatch> feedback(const std::string& session, std::uint64_t afterSequence,
                                                  std::uint32_t limit) const;
    MaiError release(const std::string& session, const std::string& expectedTurn = {}, bool* released = nullptr);
    // Neutralize only this task's controls, retaining its native gamepads and
    // target lease. Expired owners still take the stronger teardown path.
    MaiError reset(const std::string& session, const std::string& expectedTurn = {}, bool* reset = nullptr);

private:
    struct Entry {
        std::shared_ptr<DesktopNativeInputSession> native;
        std::string turn;
        bool display = false;
        std::uint64_t target = 0;
        std::uint64_t executionWindowId = 0;
        std::uint64_t executionWindowProcessId = 0;
        std::chrono::steady_clock::time_point deadline;
        bool expired = false;
    };
    void watch();
    void expire(Entry& entry);
    std::shared_ptr<DesktopComputerBackend> backend_;
    std::function<void()> invalidate_;
    std::chrono::milliseconds timeout_;
    mutable std::mutex mutex_;
    std::condition_variable changed_;
    std::map<std::string, Entry> entries_;
    std::thread watchdog_;
    bool stopping_ = false;
};
