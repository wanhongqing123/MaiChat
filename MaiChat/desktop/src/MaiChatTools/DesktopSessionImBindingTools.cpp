#include "MaiChatTools/DesktopSessionImBindingTools.h"

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMetaObject>
#include <QPointer>
#include <QRegularExpression>
#include <QStringList>
#include <QThread>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>

#include "MaiTool.h"
#include "agent/DesktopSessionImBridge.h"

namespace {

constexpr const char* kName = "maichat_session_im_binding";
using Clock = std::chrono::steady_clock;

struct Command {
    QString action;
    QString peer;
    QString session;
    bool mutates() const { return action != QStringLiteral("list"); }
};

bool parseCommand(const std::string& raw, Command& command) {
    if (raw.empty() || raw.size() > 4096) return false;
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(QByteArray(raw.data(), int(raw.size())), &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) return false;
    const QJsonObject object = document.object();
    if (!object.value(QStringLiteral("action")).isString()) return false;
    command.action = object.value(QStringLiteral("action")).toString();
    if (command.action == QStringLiteral("list")) return object.size() == 1;
    const bool binding = command.action == QStringLiteral("bind");
    if (!binding && command.action != QStringLiteral("unbind")) return false;
    if (object.size() != (binding ? 3 : 2) || !object.value(QStringLiteral("peer_id")).isString()) return false;
    command.peer = object.value(QStringLiteral("peer_id")).toString();
    if (command.peer.isEmpty() || command.peer != command.peer.trimmed() || command.peer.size() > 128 ||
        command.peer.toUtf8().size() > 256) return false;
    for (QChar ch : command.peer)
        if (ch.unicode() < 0x20 || (ch.unicode() >= 0x7f && ch.unicode() <= 0x9f)) return false;
    if (!binding) return !object.contains(QStringLiteral("session_id"));
    if (!object.value(QStringLiteral("session_id")).isString()) return false;
    command.session = object.value(QStringLiteral("session_id")).toString();
    static const QRegularExpression id(QStringLiteral("^[A-Za-z0-9_-]{1,128}$"));
    return id.match(command.session).hasMatch();
}

MaiToolResult failure(MaiErrorCode code, const std::string& message, bool started, bool mutating,
                      bool timedOut = false, bool canceled = false) {
    MaiToolExecution execution;
    execution.status = started ? MaiToolExecutionStatus::Unknown
                               : canceled ? MaiToolExecutionStatus::Canceled : MaiToolExecutionStatus::NotStarted;
    execution.started = started;
    execution.effectStatus = started && mutating ? MaiToolEffectStatus::Unknown : MaiToolEffectStatus::None;
    execution.nextAction = started && mutating ? MaiToolNextAction::InspectState
                                             : code == MaiErrorCode::InvalidInput ? MaiToolNextAction::ReviseArguments
                                                                                : MaiToolNextAction::None;
    execution.recoveryTool = started && mutating ? kName : "";
    execution.timedOut = timedOut;
    execution.canceled = canceled;
    return MaiToolResult::failure(code, message).withExecution(std::move(execution));
}

MaiToolResult success(QJsonObject object, bool changed, bool truncated = false) {
    MaiToolExecution execution;
    execution.status = MaiToolExecutionStatus::Completed;
    execution.started = true;
    execution.effectStatus = changed ? MaiToolEffectStatus::Confirmed : MaiToolEffectStatus::None;
    return MaiToolResult::success(QJsonDocument(object).toJson(QJsonDocument::Compact).toStdString(), truncated)
        .withExecution(std::move(execution));
}

struct Operation {
    std::mutex mutex;
    std::condition_variable changed;
    Clock::time_point deadline;
    // This pointer is a scoped lease, not copied context ownership. Both reads
    // and detachment hold mutex; execute clears it before its native caller can
    // destroy the cancel atomic. Late queued callbacks retain only this state.
    const std::atomic_bool* cancel = nullptr;
    bool leaseAlive = true;
    bool abandoned = false;
    bool started = false;
    bool done = false;
    std::optional<MaiToolResult> result;
};

class CancelLease final {
public:
    explicit CancelLease(std::shared_ptr<Operation> operation) : operation_(std::move(operation)) {}
    ~CancelLease() {
        std::lock_guard<std::mutex> lock(operation_->mutex);
        operation_->cancel = nullptr;
        operation_->leaseAlive = false;
    }
private:
    std::shared_ptr<Operation> operation_;
};

void complete(const std::shared_ptr<Operation>& operation, MaiToolResult result) {
    std::lock_guard<std::mutex> lock(operation->mutex);
    if (operation->abandoned || !operation->leaseAlive) return;
    operation->result = std::move(result);
    operation->done = true;
    operation->changed.notify_all();
}

bool enter(const std::shared_ptr<Operation>& operation, bool mutating, bool start) {
    std::lock_guard<std::mutex> lock(operation->mutex);
    if (operation->abandoned || !operation->leaseAlive || operation->done) return false;
    const bool canceled = operation->cancel && operation->cancel->load();
    const bool timedOut = Clock::now() >= operation->deadline;
    if (canceled || timedOut) {
        operation->result = failure(canceled ? MaiErrorCode::Canceled : MaiErrorCode::Network,
            canceled ? "Session binding operation was canceled before it started"
                     : "Session binding operation timed out before it started",
            false, mutating, timedOut, canceled);
        operation->done = true;
        operation->changed.notify_all();
        return false;
    }
    if (start) operation->started = true;
    return true;
}

class SessionImBindingTool final : public MaiTool {
public:
    SessionImBindingTool(QObject* dispatcher, std::function<DesktopSessionImBridge*()> provider, int timeoutMs)
        : dispatcher_(dispatcher), provider_(std::move(provider)), timeoutMs_(std::clamp(timeoutMs, 1, 5000)) {}
    std::string name() const override { return kName; }
    std::string description() const override {
        return "List, bind, or unbind this account's friend conversations and existing local Creation Center Sessions. "
               "Binding forwards new Human-origin IM messages into that Session and replies back to the same friend. "
               "Use list to inspect current bindings; bind requires peer_id and session_id; unbind requires peer_id. "
               "One Session can belong to only one friend binding. Bind and unbind follow the current Agent approval "
               "policy. Unbind revokes automatic forwarding without stopping the Session. No old messages are replayed.";
    }
    std::string parametersSchema() const override {
        // Providers inspect root properties when publishing tool parameters.
        // Keep the schema flat; execute validates each action's exact key set.
        return R"({"type":"object","properties":{"action":{"type":"string","enum":["list","bind","unbind"]},)"
               R"("peer_id":{"type":"string","minLength":1,"maxLength":128},)"
               R"("session_id":{"type":"string","minLength":1,"maxLength":128}},)"
               R"("required":["action"],"additionalProperties":false})";
    }
    bool requiresApproval(const std::string& arguments) const override {
        Command command;
        return parseCommand(arguments, command) && command.mutates();
    }
    bool requiresPerCallApproval(const std::string& arguments) const override {
        return requiresApproval(arguments);
    }
    MaiToolResult execute(const std::string& arguments, const MaiToolContext& context) override {
        const auto deadline = Clock::now() + std::chrono::milliseconds(timeoutMs_);
        Command command;
        if (!parseCommand(arguments, command))
            return failure(MaiErrorCode::InvalidInput,
                           "Use exactly action=list, action=bind with peer_id/session_id, or action=unbind with peer_id",
                           false, false);
        if (context.isCanceled())
            return failure(MaiErrorCode::Canceled, "Session binding operation was canceled before dispatch",
                           false, command.mutates(), false, true);
        auto* main = QCoreApplication::instance();
        if (!main)
            return failure(MaiErrorCode::NotConfigured, "MaiChat application is unavailable", false, command.mutates());
        auto operation = std::make_shared<Operation>();
        operation->deadline = deadline;
        operation->cancel = context.cancel;
        CancelLease lease(operation);
        // No callback retains this tool, the native context, or stack references.
        // Provider and QObject weak references are dereferenced only on Main.
        const auto run = [operation, command, dispatcher = dispatcher_, provider = provider_] {
            if (!enter(operation, command.mutates(), false)) return;
            if (!dispatcher || dispatcher->thread() != QThread::currentThread()) {
                complete(operation, failure(MaiErrorCode::NotConfigured, "MaiChat binding host is unavailable",
                                            false, command.mutates()));
                return;
            }
            QPointer<DesktopSessionImBridge> bridge(provider ? provider() : nullptr);
            if (!enter(operation, command.mutates(), false)) return;
            if (!bridge) {
                complete(operation, failure(MaiErrorCode::NotConfigured,
                    "Session binding registration identity changed or the host is unavailable", false, command.mutates()));
                return;
            }
            const QString before = command.mutates() ? bridge->boundSession(command.peer) : QString();
            // Account/controller scope can change during host validation. Read
            // the same provider again immediately before the mutation boundary.
            auto* validated = provider ? provider() : nullptr;
            if (!bridge || validated != bridge.data()) {
                if (!enter(operation, command.mutates(), false)) return;
                complete(operation, failure(MaiErrorCode::NotConfigured,
                    "Session binding registration identity changed before execution", false, command.mutates()));
                return;
            }
            if (!enter(operation, command.mutates(), true)) return;
            if (!command.mutates()) {
                const auto bindings = bridge->bindings();
                QStringList peers = bindings.keys();
                std::sort(peers.begin(), peers.end());
                QJsonArray items;
                for (const QString& peer : peers) {
                    if (items.size() >= 100) break;
                    items.append(QJsonObject{{QStringLiteral("peer_id"), peer},
                                             {QStringLiteral("session_id"), bindings.value(peer)}});
                }
                complete(operation, success({{QStringLiteral("action"), command.action},
                    {QStringLiteral("bindings"), items}, {QStringLiteral("count"), int(items.size())},
                    {QStringLiteral("total"), int(bindings.size())}}, false, bindings.size() > items.size()));
                return;
            }
            QString error;
            const bool accepted = command.action == QStringLiteral("bind")
                ? bridge->bind(command.peer, command.session, &error) : (bridge->unbind(command.peer), true);
            {
                std::lock_guard<std::mutex> lock(operation->mutex);
                if (operation->abandoned || !operation->leaseAlive) return;
            }
            validated = provider ? provider() : nullptr;
            if (!bridge || validated != bridge.data()) {
                complete(operation, failure(MaiErrorCode::Canceled,
                    "Binding operation started but its account/controller changed; inspect current bindings",
                    true, true));
                return;
            }
            const QString after = bridge->boundSession(command.peer);
            const bool correct = command.action == QStringLiteral("bind") ? after == command.session : after.isEmpty();
            if (!accepted || !correct) {
                complete(operation, failure(MaiErrorCode::InvalidInput,
                    error.isEmpty() ? "Binding operation was not confirmed; inspect current bindings"
                                    : error.toStdString(), true, true));
                return;
            }
            complete(operation, success({{QStringLiteral("action"), command.action},
                {QStringLiteral("peer_id"), command.peer}, {QStringLiteral("session_id"), after},
                {QStringLiteral("bound"), !after.isEmpty()}, {QStringLiteral("changed"), before != after}}, before != after));
        };
        if (QThread::currentThread() == main->thread()) run();
        else if (!QMetaObject::invokeMethod(main, run, Qt::QueuedConnection))
            return failure(MaiErrorCode::NotConfigured, "Could not queue the Session binding operation", false, command.mutates());
        for (;;) {
            std::unique_lock<std::mutex> lock(operation->mutex);
            if (operation->done) return *operation->result;
            const bool canceled = context.isCanceled();
            const bool timedOut = Clock::now() >= deadline;
            if (canceled || timedOut) {
                operation->abandoned = true;
                return failure(canceled ? MaiErrorCode::Canceled : MaiErrorCode::Network,
                    operation->started && command.mutates()
                        ? "Binding operation completion is unknown; inspect current bindings with action=list before retrying"
                        : operation->started ? "Session binding read completion was not observed before cancellation or timeout"
                        : canceled ? "Session binding operation was canceled before it started"
                                   : "Session binding operation timed out before it started",
                    operation->started, command.mutates(), timedOut, canceled);
            }
            operation->changed.wait_for(lock, std::chrono::milliseconds(5));
        }
    }
private:
    QPointer<QObject> dispatcher_;
    std::function<DesktopSessionImBridge*()> provider_;
    int timeoutMs_;
};

}  // namespace

void registerDesktopSessionImBindingTools(
    MaiToolRegistry& registry, QObject* dispatcher,
    std::function<DesktopSessionImBridge*()> bridgeProvider, int timeoutMs) {
    registry.add(std::make_unique<SessionImBindingTool>(dispatcher, std::move(bridgeProvider), timeoutMs));
}
