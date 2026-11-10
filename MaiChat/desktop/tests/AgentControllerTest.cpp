// 文件作用：验证桌面端Agent 事件的 Qt 主线程交付、多模态输入和工具注册边界。
// 运行边界：由测试进程驱动实际组件或替身，不作为正式 App 的用户入口。
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QThread>
#include <QtTest>
#include <httplib.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "agent/AgentController.h"
#include "agent/DesktopAccountModelClient.h"
#include "MaiScreenshot.h"
#include "MaiScreenshotTool.h"
#include "MaiSessionSchedule.h"
#include "MaiSqliteStore.h"
#include "MaiTime.h"
#include "../../../MaiAgent/third_party/sqlite/sqlite3.h"

// AgentController 的活儿只有一件：把 MaiAgent 的事件从**核心的线程**搬到 Qt 主线程。
// 所以这份用例的重点不是"信号发出来了没有"，而是"它们到达的时候人在哪个线程上"。
//
// 光断言"收到了信号"是验不到东西的：即使一行排队代码都没有、直接在网络线程上 emit，
// QSignalSpy 照样能记到（它就是个槽）。必须比对线程 id 才算数。
namespace {

// Observe the real temporary store rather than adding a store-injection API to
// the controller. Row counts make history-size regressions deterministic and do
// not depend on the speed of the machine running this test.
struct ControllerReadProbe {
    explicit ControllerReadProbe(QString path)
        : databasePath(QFileInfo(path).canonicalFilePath()) {
        active = this;
        installed = sqlite3_auto_extension(reinterpret_cast<void (*)()>(attach)) == SQLITE_OK;
    }
    ~ControllerReadProbe() {
        sqlite3_cancel_auto_extension(reinterpret_cast<void (*)()>(attach));
        active = nullptr;
    }

    static int attach(sqlite3* database, char**, const sqlite3_api_routines*) {
        // SQLite VFS 会解析 /var 等符号链接；按同一真实文件匹配，避免探针漏掉连接。
        const QString path = QFileInfo(
            QString::fromUtf8(sqlite3_db_filename(database, "main"))).canonicalFilePath();
        if (active && path == active->databasePath) {
            ++active->attachedConnections;
            sqlite3_trace_v2(database, SQLITE_TRACE_ROW, recordRow, active);
        }
        return SQLITE_OK;
    }
    static int recordRow(unsigned, void* context, void* statement, void*) {
        auto* probe = static_cast<ControllerReadProbe*>(context);
        if (QThread::currentThread() != QCoreApplication::instance()->thread() || !probe->enabled) return 0;
        const char* sql = sqlite3_sql(static_cast<sqlite3_stmt*>(statement));
        if (sql && std::strstr(sql, "FROM messages ")) ++probe->messageRows;
        if (sql && std::strstr(sql, "FROM parts ")) ++probe->partRows;
        return 0;
    }

    static ControllerReadProbe* active;
    QString databasePath;
    bool installed = false;
    bool enabled = false;
    int attachedConnections = 0;
    int messageRows = 0;
    int partRows = 0;
};

ControllerReadProbe* ControllerReadProbe::active = nullptr;

// 把一段 UTF-8 切成一个个完整字符。首字节的高位决定这个字符占几个字节。
std::vector<std::string_view> splitUtf8(const std::string& text) {
    std::vector<std::string_view> pieces;
    const std::string_view view(text);
    for (std::size_t i = 0; i < view.size();) {
        const unsigned char lead = static_cast<unsigned char>(view[i]);
        std::size_t width = 1;
        if ((lead & 0xE0u) == 0xC0u) {
            width = 2;
        } else if ((lead & 0xF0u) == 0xE0u) {
            width = 3;
        } else if ((lead & 0xF8u) == 0xF0u) {
            width = 4;
        }
        width = std::min<qsizetype>(width, view.size() - i);
        pieces.push_back(view.substr(i, width));
        i += width;
    }
    return pieces;
}

// 一个最小的模型实现。直接实现公开头里的 MaiModelClient 接口，
// 不依赖 MaiAgent 的测试支持库——这样验的正好是"第三方怎么嵌这个库"。
class ScriptedModel final : public MaiModelClient {
public:
    ScriptedModel(std::string reasoning, std::string text)
        : reasoning_(std::move(reasoning)), text_(std::move(text)) {}

    MaiError stream(const MaiModelRequest& request, const MaiStreamSink& sink,
                    const std::atomic<bool>& cancel) override {
        {
            std::lock_guard<std::mutex> lock(requestMutex_);
            lastRequest_ = request;
        }
        // 记下这一轮跑在哪个线程上。待会儿要和槽里看到的线程比对。
        workerThread_.store(QThread::currentThreadId(), std::memory_order_relaxed);
        requests_.fetch_add(1, std::memory_order_relaxed);

        // 一个字符一个字符吐，**按 UTF-8 的字符边界切，不按字节**。
        //
        // 这不是图省事：MaiStreamSink 的契约要求每次交付都是完整的 UTF-8。
        // 第一版这里写的是 substr(i, 1)，按字节切，
        // 结果中文全成了问号——而真实的客户端不会这样（MaiOpenAiClient 交出去的是解析完的 JSON 字符
        // 串值），所以那是假模型在造一个现实中不存在的输入。
        if (sink.onReasoning) {
            for (const std::string_view piece : splitUtf8(reasoning_)) {
                if (cancel.load(std::memory_order_relaxed)) return {};
                sink.onReasoning(piece);
            }
        }
        if (sink.onText) {
            for (const std::string_view piece : splitUtf8(text_)) {
                if (cancel.load(std::memory_order_relaxed)) return {};
                sink.onText(piece);
            }
        }
        return {};
    }

    MaiWireApi wireApi() const override {
        return MaiWireApi::ChatCompletions;
    }

    Qt::HANDLE workerThread() const {
        return workerThread_.load(std::memory_order_relaxed);
    }
    int requests() const {
        return requests_.load(std::memory_order_relaxed);
    }
    MaiModelRequest lastRequest() const {
        std::lock_guard<std::mutex> lock(requestMutex_);
        return lastRequest_;
    }

private:
    std::string reasoning_;
    std::string text_;
    std::atomic<Qt::HANDLE> workerThread_{nullptr};
    std::atomic<int> requests_{0};
    mutable std::mutex requestMutex_;
    MaiModelRequest lastRequest_;
};

class HostProbeTool final : public MaiTool {
public:
    std::string name() const override { return "maichat_probe"; }
    std::string description() const override { return "Probe a host-provided capability."; }
    std::string parametersSchema() const override {
        return R"({"type":"object","properties":{}})";
    }
    MaiToolResult execute(const std::string&, const MaiToolContext&) override {
        return MaiToolResult::success("ok");
    }
};

class OutsideReadModel final : public MaiModelClient {
public:
    explicit OutsideReadModel(std::string arguments, std::string tool = "file_read")
        : arguments_(std::move(arguments)), tool_(std::move(tool)) {}
    MaiError stream(const MaiModelRequest&, const MaiStreamSink& sink,
                    const std::atomic<bool>&) override {
        if (calls_++ == 0 && sink.onToolCall)
            sink.onToolCall(MaiToolInvocation{"outside-file", tool_, arguments_});
        else if (sink.onText)
            sink.onText("done");
        return {};
    }
    MaiWireApi wireApi() const override { return MaiWireApi::ChatCompletions; }

private:
    std::string arguments_;
    std::string tool_;
    std::atomic<int> calls_{0};
};

class ConcurrentReasoningModel final : public MaiModelClient {
public:
    std::atomic<bool> firstReasoningSent{false};
    std::atomic<bool> release{false};

    MaiError stream(const MaiModelRequest& request, const MaiStreamSink& sink,
                    const std::atomic<bool>& cancel) override {
        if (request.messages.back().content == "session A") {
            if (sink.onReasoning) sink.onReasoning("first");
            firstReasoningSent.store(true);
            while (!release.load() && !cancel.load())
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            if (sink.onReasoning && !cancel.load()) sink.onReasoning("second");
        } else if (sink.onText) {
            sink.onText("session B done");
        }
        return {};
    }

    MaiWireApi wireApi() const override { return MaiWireApi::ChatCompletions; }
};

// Record the actual request crossing the controller/Core boundary. Named
// sessions avoid unrelated automatic-title requests; a cancellable hold keeps
// the busy-session test deterministic without involving a network model.
class SessionSelectionModel final : public MaiModelClient {
public:
    explicit SessionSelectionModel(MaiWireApi wire = MaiWireApi::ChatCompletions) : wire_(wire) {}

    std::atomic<bool> heldRequestStarted{false};
    std::atomic<bool> releaseHeldRequest{false};

    MaiError stream(const MaiModelRequest& request, const MaiStreamSink& sink,
                    const std::atomic<bool>& cancel) override {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            models_.push_back(QString::fromStdString(request.model));
            efforts_.push_back(QString::fromStdString(request.reasoningEffort));
        }
        if (!request.messages.empty() && request.messages.back().content == "hold first session") {
            heldRequestStarted.store(true);
            while (!releaseHeldRequest.load() && !cancel.load())
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        if (!cancel.load() && sink.onText) sink.onText("session reply");
        return {};
    }

    MaiWireApi wireApi() const override { return wire_; }

    QStringList requestedModels() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return models_;
    }

    QStringList requestedEfforts() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return efforts_;
    }

private:
    MaiWireApi wire_;
    mutable std::mutex mutex_;
    QStringList models_;
    QStringList efforts_;
};

// Accept both protocols so a wrong production route is observed as an actual
// HTTP request, rather than being disguised by a fixture 404 or parse failure.
struct AccountRouteProbe {
    struct Request {
        QString path;
        QString authorization;
        QJsonObject body;
    };

    httplib::Server server;
    std::thread thread;
    std::atomic<bool> listenerExited{false};
    mutable std::mutex mutex;
    std::vector<Request> requests;
    int port = 0;

    static void respond(const httplib::Request& request, httplib::Response& response) {
        response.set_header("Connection", "close");
        if (QString::fromStdString(request.path).endsWith(QStringLiteral("/chat/completions"))) {
            response.set_content(
                "data: {\"choices\":[{\"delta\":{\"content\":\"ok\"},\"finish_reason\":null}]}\n\n"
                "data: {\"choices\":[{\"delta\":{},\"finish_reason\":\"stop\"}]}\n\n"
                "data: [DONE]\n\n", "text/event-stream");
        } else {
            response.set_content(
                "data: {\"type\":\"response.output_item.added\",\"output_index\":0,\"item\":{"
                "\"id\":\"fixture-output\",\"type\":\"message\",\"role\":\"assistant\",\"content\":[]}}\n\n"
                "data: {\"type\":\"response.output_text.delta\",\"output_index\":0,\"content_index\":0,"
                "\"item_id\":\"fixture-output\",\"delta\":\"ok\"}\n\n"
                "data: {\"type\":\"response.completed\",\"response\":"
                "{\"status\":\"completed\",\"output\":[{\"id\":\"fixture-output\",\"type\":\"message\","
                "\"role\":\"assistant\",\"content\":[{\"type\":\"output_text\",\"text\":\"ok\"}]}]}}\n\n",
                "text/event-stream");
        }
    }

    AccountRouteProbe() {
        server.set_read_timeout(2);
        server.set_write_timeout(2);
        server.Post(".*", [this](const httplib::Request& request, httplib::Response& response) {
            {
                std::lock_guard<std::mutex> lock(mutex);
                requests.push_back({QString::fromStdString(request.path),
                                    QString::fromStdString(request.get_header_value("Authorization")),
                                    QJsonDocument::fromJson(QByteArray::fromStdString(request.body)).object()});
            }
            respond(request, response);
        });
        port = server.bind_to_any_port("127.0.0.1");
        if (port > 0) {
            thread = std::thread([this] {
                server.listen_after_bind();
                listenerExited.store(true);
            });
            for (int i = 0; i < 200 && !server.is_running() && !listenerExited.load(); ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }

    ~AccountRouteProbe() {
        if (thread.joinable()) {
            // httplib::stop() is a no-op before listen_after_bind() enters.
            // Even a failed readiness assertion must not lose that shutdown.
            while (!server.is_running() && !listenerExited.load())
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            server.stop();
            thread.join();
        }
    }

    QString baseUrl(const QString& profile) const {
        return QStringLiteral("http://127.0.0.1:%1/%2").arg(port).arg(profile);
    }

    std::vector<Request> recordedRequests() const {
        std::lock_guard<std::mutex> lock(mutex);
        return requests;
    }
};

}  // namespace

class AgentControllerTest : public QObject {
    Q_OBJECT

private slots:
    void streams_on_the_main_thread();
    void separates_reasoning_from_text();
    void reports_a_missing_model_instead_of_hanging();
    void sends_selected_images_as_multimodal_input();
    void registers_a_screenshot_tool_that_requires_approval_data();
    void registers_a_screenshot_tool_that_requires_approval();
    void registers_host_tools_after_builtin_tools();
    void desktopSessionToolUsesAccountDefaultWhenPathOmitted();
    void desktopSessionToolConfirmsExplicitDirectory();
    void finishingAnotherSessionDoesNotReclassifyReasoning();
    void messagePartEventsReadOnlyTheirOwner();
    void desktopReadsOutsideSessionWorkingDirectory();
    void desktopWriteOutsideWaitsForApproval();
    void selectedModelsStayWithTheirSessionsWhenSending();
    void defaultModelOnlyAffectsSubsequentSessions();
    void newSessionDoesNotInheritAnotherSessionSelection();
    void sqliteSessionModelsSurviveRestartAndNewDefault();
    void busySessionSelectionIsRejectedWhileAnotherSessionCanChange();
    void sessionEffortAndModelPersistTogetherAcrossRestart();
    void sessionConfigurationUsesFixedLegacyDefaultsAndAtomicValidation();
    void sessionConfigurationRejectsMissingProviderWithoutMutation();
    void sessionConfigurationReflectsOnlySelectedProviderCredentials();
    void sessionConfigurationWithoutClientRejectsUnknownModel();
    void sessionConfigurationAcceptsInjectedLocalClient();
    void credentialWaitDoesNotRetargetOtherSession();
    void accountModelClientUsesProviderProtocolAndProfile_data();
    void accountModelClientUsesProviderProtocolAndProfile();
    void accountModelClientRoutesBySelectedModelAndFreshKey();
#ifdef Q_OS_MACOS
    void accountModelClientUsesResolvedProxy();
#endif
    void scheduledSessionPromptRunsOnceOnOwner();
    void scheduledSessionCatchesUpAfterOwnerRestarts();
};

void AgentControllerTest::scheduledSessionCatchesUpAfterOwnerRestarts() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString database = QDir(directory.path()).filePath(QStringLiteral("sessions.sqlite"));
    QString session;
    {
        auto model = std::make_unique<ScriptedModel>(std::string(), "old instance");
        AgentController original(std::move(model), database);
        session = original.createSession(directory.path());
        QVERIFY(!session.isEmpty());
        const auto opened = MaiSessionScheduleStore::open(
            database.toUtf8().toStdString() + ".schedule.sqlite");
        QVERIFY(opened.isOk());
        QVERIFY(opened.value()->create(session.toStdString(), "恢复后检查",
            "设备恢复后继续", MaiTime::getCurrentTime() + 2'000,
            MaiTime::getCurrentTime()).isOk());
    }
    QTest::qWait(2'300);
    auto model = std::make_unique<ScriptedModel>(std::string(), "resumed");
    ScriptedModel* scripted = model.get();
    AgentController resumed(std::move(model), database);
    QSignalSpy finished(&resumed, &AgentController::turnFinished);
    QTRY_COMPARE_WITH_TIMEOUT(scripted->requests(), 1, 6'000);
    QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 6'000);
    const auto opened = MaiSessionScheduleStore::open(
        database.toUtf8().toStdString() + ".schedule.sqlite");
    QVERIFY(opened.isOk());
    QCOMPARE(QString::fromStdString(opened.value()->list(session.toStdString())
        .value().front().status), QStringLiteral("completed"));
}

void AgentControllerTest::scheduledSessionPromptRunsOnceOnOwner() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString database = QDir(directory.path()).filePath(QStringLiteral("sessions.sqlite"));
    auto model = std::make_unique<ScriptedModel>(std::string(), "scheduled reply");
    ScriptedModel* scripted = model.get();
    AgentController controller(std::move(model), database);
    const QString session = controller.createSession(directory.path());
    QVERIFY(!session.isEmpty());
    const auto opened = MaiSessionScheduleStore::open(
        database.toUtf8().toStdString() + ".schedule.sqlite");
    QVERIFY(opened.isOk());
    const auto due = opened.value()->create(session.toStdString(), "测试提醒",
        "只执行一次", MaiTime::getCurrentTime() + 2'000, MaiTime::getCurrentTime());
    QVERIFY(due.isOk());
    QSignalSpy finished(&controller, &AgentController::turnFinished);
    QTRY_COMPARE_WITH_TIMEOUT(scripted->requests(), 1, 6'000);
    QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 6'000);
    QTRY_COMPARE_WITH_TIMEOUT(QString::fromStdString(
        opened.value()->list(session.toStdString()).value().front().status),
        QStringLiteral("completed"), 6'000);
    QTest::qWait(1'300);
    QCOMPARE(scripted->requests(), 1);
}

void AgentControllerTest::selectedModelsStayWithTheirSessionsWhenSending() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    auto model = std::make_unique<SessionSelectionModel>();
    SessionSelectionModel* recorded = model.get();
    AgentController controller(std::move(model), QString());
    const QString first = controller.createSession(directory.path(), QStringLiteral("first session"));
    const QString second = controller.createSession(directory.path(), QStringLiteral("second session"));
    QVERIFY(!first.isEmpty() && !second.isEmpty());
    QVERIFY(controller.setModel(first, QStringLiteral("glm-5.3")));
    QVERIFY(controller.setModel(second, QStringLiteral("deepseek-v4-pro")));
    QSignalSpy finished(&controller, &AgentController::turnFinished);
    QVERIFY(controller.sendPrompt(first, QStringLiteral("first request")));
    QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 10'000);
    QVERIFY(controller.sendPrompt(second, QStringLiteral("second request")));
    QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 2, 10'000);
    QVERIFY(controller.sendPrompt(first, QStringLiteral("first request again")));
    QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 3, 10'000);
    QCOMPARE(recorded->requestedModels(),
             (QStringList{QStringLiteral("glm-5.3"), QStringLiteral("deepseek-v4-pro"),
                          QStringLiteral("glm-5.3")}));
    MaiSession firstStored;
    MaiSession secondStored;
    QVERIFY(controller.agent().getSession(first.toStdString(), firstStored));
    QVERIFY(controller.agent().getSession(second.toStdString(), secondStored));
    QCOMPARE(QString::fromStdString(firstStored.model), QStringLiteral("glm-5.3"));
    QCOMPARE(QString::fromStdString(secondStored.model), QStringLiteral("deepseek-v4-pro"));
}

void AgentControllerTest::defaultModelOnlyAffectsSubsequentSessions() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    auto model = std::make_unique<SessionSelectionModel>();
    SessionSelectionModel* recorded = model.get();
    AgentController controller(std::move(model), QString());
    const QString first = controller.createSession(directory.path(), QStringLiteral("existing first"));
    const QString second = controller.createSession(directory.path(), QStringLiteral("existing second"));
    QVERIFY(!first.isEmpty() && !second.isEmpty());
    QVERIFY(controller.setModel(first, QStringLiteral("glm-5.3")));
    QVERIFY(controller.setModel(second, QStringLiteral("deepseek-v4-pro")));
    QVERIFY(controller.setDefaultModel(QStringLiteral("glm-5.3-flash")));
    const QString next = controller.createSession(directory.path(), QStringLiteral("new default session"));
    QVERIFY(!next.isEmpty());
    QSignalSpy finished(&controller, &AgentController::turnFinished);
    const QStringList sessions{first, second, next};
    for (int i = 0; i < sessions.size(); ++i) {
        QVERIFY(controller.sendPrompt(sessions[i], QStringLiteral("check session default")));
        QTRY_COMPARE_WITH_TIMEOUT(finished.count(), i + 1, 10'000);
    }
    QCOMPARE(recorded->requestedModels(),
             (QStringList{QStringLiteral("glm-5.3"), QStringLiteral("deepseek-v4-pro"),
                          QStringLiteral("glm-5.3-flash")}));
    MaiSession created;
    QVERIFY(controller.agent().getSession(next.toStdString(), created));
    QCOMPARE(QString::fromStdString(created.model), QStringLiteral("glm-5.3-flash"));
}

void AgentControllerTest::newSessionDoesNotInheritAnotherSessionSelection() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    auto model = std::make_unique<SessionSelectionModel>();
    SessionSelectionModel* recorded = model.get();
    AgentController controller(std::move(model), QString());
    QVERIFY(controller.setDefaultModel(QStringLiteral("deepseek-flash")));
    const QString selected = controller.createSession(directory.path(), QStringLiteral("selected session"));
    QVERIFY(!selected.isEmpty());
    QVERIFY(controller.setModel(selected, QStringLiteral("glm-5.3-flash")));
    const QString next = controller.createSession(directory.path(), QStringLiteral("independent new session"));
    QVERIFY(!next.isEmpty());
    QSignalSpy finished(&controller, &AgentController::turnFinished);
    QVERIFY(controller.sendPrompt(next, QStringLiteral("new session request")));
    QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 10'000);
    QCOMPARE(recorded->requestedModels(), (QStringList{QStringLiteral("deepseek-flash")}));
    MaiSession created;
    QVERIFY(controller.agent().getSession(next.toStdString(), created));
    QCOMPARE(QString::fromStdString(created.model), QStringLiteral("deepseek-flash"));
}

void AgentControllerTest::sqliteSessionModelsSurviveRestartAndNewDefault() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString database = QDir(directory.path()).filePath(QStringLiteral("session-models.sqlite"));
    QString first;
    QString second;
    {
        AgentController original(std::make_unique<SessionSelectionModel>(), database);
        QVERIFY2(original.openError().isEmpty(), qPrintable(original.openError()));
        first = original.createSession(directory.path(), QStringLiteral("persisted first"));
        second = original.createSession(directory.path(), QStringLiteral("persisted second"));
        QVERIFY(!first.isEmpty() && !second.isEmpty());
        QVERIFY(original.setModel(first, QStringLiteral("glm-5.3")));
        QVERIFY(original.setModel(second, QStringLiteral("deepseek-v4-pro")));
    }
    auto model = std::make_unique<SessionSelectionModel>();
    SessionSelectionModel* recorded = model.get();
    AgentController resumed(std::move(model), database);
    QVERIFY2(resumed.openError().isEmpty(), qPrintable(resumed.openError()));
    MaiSession firstStored;
    MaiSession secondStored;
    QVERIFY(resumed.agent().getSession(first.toStdString(), firstStored));
    QVERIFY(resumed.agent().getSession(second.toStdString(), secondStored));
    QCOMPARE(QString::fromStdString(firstStored.model), QStringLiteral("glm-5.3"));
    QCOMPARE(QString::fromStdString(secondStored.model), QStringLiteral("deepseek-v4-pro"));
    QVERIFY(resumed.setDefaultModel(QStringLiteral("glm-5.3-flash")));
    QSignalSpy finished(&resumed, &AgentController::turnFinished);
    QVERIFY(resumed.sendPrompt(first, QStringLiteral("resumed first request")));
    QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 10'000);
    QVERIFY(resumed.sendPrompt(second, QStringLiteral("resumed second request")));
    QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 2, 10'000);
    QCOMPARE(recorded->requestedModels(),
             (QStringList{QStringLiteral("glm-5.3"), QStringLiteral("deepseek-v4-pro")}));
    QVERIFY(resumed.agent().getSession(first.toStdString(), firstStored));
    QVERIFY(resumed.agent().getSession(second.toStdString(), secondStored));
    QCOMPARE(QString::fromStdString(firstStored.model), QStringLiteral("glm-5.3"));
    QCOMPARE(QString::fromStdString(secondStored.model), QStringLiteral("deepseek-v4-pro"));
}

void AgentControllerTest::busySessionSelectionIsRejectedWhileAnotherSessionCanChange() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    auto model = std::make_unique<SessionSelectionModel>();
    SessionSelectionModel* recorded = model.get();
    AgentController controller(std::move(model), QString());
    struct ReleaseOnExit {
        std::atomic<bool>& release;
        ~ReleaseOnExit() { release.store(true); }
    } releaseOnExit{recorded->releaseHeldRequest};
    const QString first = controller.createSession(directory.path(), QStringLiteral("busy first"));
    const QString second = controller.createSession(directory.path(), QStringLiteral("idle second"));
    QVERIFY(!first.isEmpty() && !second.isEmpty());
    QVERIFY(controller.setModel(second, QStringLiteral("deepseek-flash")));
    QVERIFY(controller.setModel(first, QStringLiteral("glm-5.3")));
    QSignalSpy finished(&controller, &AgentController::turnFinished);
    QVERIFY(controller.sendPrompt(first, QStringLiteral("hold first session")));
    QTRY_VERIFY_WITH_TIMEOUT(recorded->heldRequestStarted.load(), 10'000);
    QVERIFY(controller.agent().isBusy(first.toStdString()));
    const bool unchangedAccepted = controller.setSessionModelConfiguration(
        first, QStringLiteral("glm-5.3"), QStringLiteral("high"));
    const bool busyEffortChanged = controller.setSessionModelConfiguration(
        first, QStringLiteral("glm-5.3"), QStringLiteral("low"));
    const bool busyChanged = controller.setModel(first, QStringLiteral("glm-5.3-flash"));
    const bool idleChanged = controller.setSessionModelConfiguration(
        second, QStringLiteral("deepseek-v4-pro"), QStringLiteral("max"));
    MaiSession firstStored;
    MaiSession secondStored;
    const bool firstFound = controller.agent().getSession(first.toStdString(), firstStored);
    const bool secondFound = controller.agent().getSession(second.toStdString(), secondStored);
    recorded->releaseHeldRequest.store(true);
    QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 10'000);
    QVERIFY(controller.sendPrompt(second, QStringLiteral("idle session request")));
    QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 2, 10'000);
    QVERIFY(unchangedAccepted);
    QVERIFY(!busyEffortChanged);
    QVERIFY2(!busyChanged, "A busy target must reject the model change.");
    QVERIFY(idleChanged);
    QVERIFY(firstFound && secondFound);
    QCOMPARE(QString::fromStdString(firstStored.model), QStringLiteral("glm-5.3"));
    QCOMPARE(QString::fromStdString(secondStored.model), QStringLiteral("deepseek-v4-pro"));
    QCOMPARE(QString::fromStdString(firstStored.reasoningEffort), QStringLiteral("high"));
    QCOMPARE(QString::fromStdString(secondStored.reasoningEffort), QStringLiteral("max"));
    QCOMPARE(recorded->requestedModels(),
             (QStringList{QStringLiteral("glm-5.3"), QStringLiteral("deepseek-v4-pro")}));
    QCOMPARE(recorded->requestedEfforts(), (QStringList{QStringLiteral("high"), QStringLiteral("max")}));
}

void AgentControllerTest::sessionEffortAndModelPersistTogetherAcrossRestart() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString database = QDir(directory.path()).filePath(QStringLiteral("session-effort.sqlite"));
    const QStringList models{QStringLiteral("glm-5.3"), QStringLiteral("glm-5.3-flash"),
                             QStringLiteral("deepseek-flash"), QStringLiteral("deepseek-v4-pro")};
    const QStringList efforts{QStringLiteral("low"), QStringLiteral("high"),
                              QStringLiteral("max"), QStringLiteral("low")};
    QStringList sessions;
    {
        AgentController original(std::make_unique<SessionSelectionModel>(), database);
        QVERIFY2(original.openError().isEmpty(), qPrintable(original.openError()));
        for (int i = 0; i < models.size(); ++i) {
            const QString session = original.createSession(directory.path(), QStringLiteral("persisted %1").arg(i));
            QVERIFY(!session.isEmpty());
            QVERIFY(original.setSessionModelConfiguration(session, models[i], efforts[i]));
            sessions.push_back(session);
        }
    }
    auto model = std::make_unique<SessionSelectionModel>();
    SessionSelectionModel* recorded = model.get();
    AgentController resumed(std::move(model), database);
    QVERIFY2(resumed.openError().isEmpty(), qPrintable(resumed.openError()));
    QVERIFY(resumed.setDefaultModel(QStringLiteral("deepseek-flash")));
    QSignalSpy finished(&resumed, &AgentController::turnFinished);
    for (int i = 0; i < sessions.size(); ++i) {
        AgentController::SessionModelConfiguration config;
        QVERIFY(resumed.sessionModelConfiguration(sessions[i], config));
        QCOMPARE(config.model, models[i]);
        QCOMPARE(config.reasoningEffort, efforts[i]);
        QVERIFY(config.configured);
        QVERIFY(resumed.sendPrompt(sessions[i], QStringLiteral("check persisted model and effort")));
        QTRY_COMPARE_WITH_TIMEOUT(finished.count(), i + 1, 10'000);
    }
    QCOMPARE(recorded->requestedModels(), models);
    QCOMPARE(recorded->requestedEfforts(), efforts);
}

void AgentControllerTest::sessionConfigurationUsesFixedLegacyDefaultsAndAtomicValidation() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    auto model = std::make_unique<SessionSelectionModel>();
    SessionSelectionModel* recorded = model.get();
    AgentController controller(std::move(model), QString());
    const auto created = controller.agent().submit(
        MaiCreateSession{directory.path().toStdString(), "legacy inherited session", "", ""});
    QVERIFY(created.isOk());
    const QString legacy = QString::fromStdString(created.value());
    const QString initialDefault = QString::fromStdString(MaiAgent::Options{}.defaultModel);
    QVERIFY(controller.setDefaultModel(QStringLiteral("deepseek-v4-pro")));
    AgentController::SessionModelConfiguration config;
    QVERIFY(controller.sessionModelConfiguration(legacy, config));
    QCOMPARE(config.model, initialDefault);
    QCOMPARE(config.reasoningEffort, QStringLiteral("high"));
    const QString next = controller.createSession(directory.path(), QStringLiteral("new explicit default"));
    QVERIFY(!next.isEmpty());
    QVERIFY(controller.sessionModelConfiguration(next, config));
    QCOMPARE(config.model, QStringLiteral("deepseek-v4-pro"));
    QVERIFY(controller.setSessionModelConfiguration(next, QStringLiteral("custom-injected-model"),
                                                     QStringLiteral("max")));
    QVERIFY(!controller.setSessionModelConfiguration(next, QStringLiteral("glm-5.3"),
                                                      QStringLiteral("invalid-effort")));
    const QString rejectedError = controller.lastError();
    QVERIFY(!rejectedError.isEmpty());
    QVERIFY(controller.sessionModelConfiguration(next, config));
    QCOMPARE(config.model, QStringLiteral("custom-injected-model"));
    QCOMPARE(config.reasoningEffort, QStringLiteral("max"));
    QVERIFY(!controller.sessionModelConfiguration(QStringLiteral("missing-session"), config));
    QVERIFY(config.model.isEmpty() && config.reasoningEffort.isEmpty() && !config.configured);
    QCOMPARE(controller.lastError(), rejectedError);
    QVERIFY(controller.setModel(next, QStringLiteral("glm-5.3-flash")));
    QVERIFY(controller.sessionModelConfiguration(next, config));
    QCOMPARE(config.reasoningEffort, QStringLiteral("max"));
    QVERIFY(controller.setSessionModelConfiguration(next, QStringLiteral("glm-5.3-flash"), QString()));
    MaiSession stored;
    QVERIFY(controller.agent().getSession(next.toStdString(), stored));
    QVERIFY(stored.reasoningEffort.empty());
    QVERIFY(controller.sessionModelConfiguration(next, config));
    QCOMPARE(config.reasoningEffort, QStringLiteral("high"));
    QSignalSpy finished(&controller, &AgentController::turnFinished);
    QVERIFY(controller.sendPrompt(legacy, QStringLiteral("legacy still uses fixed default")));
    QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 10'000);
    QCOMPARE(recorded->requestedModels(), (QStringList{initialDefault}));
    QCOMPARE(recorded->requestedEfforts(), (QStringList{QStringLiteral("high")}));
    QVERIFY(controller.agent().getSession(legacy.toStdString(), stored));
    QVERIFY(stored.model.empty() && stored.reasoningEffort.empty());
}

void AgentControllerTest::sessionConfigurationRejectsMissingProviderWithoutMutation() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QHash<QString, QString> keys{{QStringLiteral("glm"), QStringLiteral("synthetic-glm-key")}};
    QStringList requestedProviders;
    AgentController::ModelConfig model;
    // No server is needed: configuration projection must not open a connection.
    model.glmBaseUrl = QStringLiteral("http://127.0.0.1:1/glm-profile");
    model.deepseekBaseUrl = QStringLiteral("http://127.0.0.1:1/deepseek-profile");
    model.modelName = QStringLiteral("glm-5.3");
    model.reasoningEffort = QStringLiteral("low");
    model.accountModelKeyProvider = [&](const QString& provider) {
        requestedProviders.push_back(provider);
        return keys.value(provider);
    };
    AgentController controller(model, QString());
    const QString session = controller.createSession(directory.path(), QStringLiteral("missing provider rejection"));
    QVERIFY(!session.isEmpty());
    MaiSession before;
    QVERIFY(controller.agent().getSession(session.toStdString(), before));
    requestedProviders.clear();
    const bool changed = controller.setSessionModelConfiguration(
        session, QStringLiteral("deepseek-v4-pro"), QStringLiteral("max"));
    MaiSession after;
    QVERIFY(controller.agent().getSession(session.toStdString(), after));
    const QString diagnostic = QStringLiteral("accepted=%1; model=%2 (expected %3); effort=%4 (expected %5)")
        .arg(changed).arg(QString::fromStdString(after.model), QString::fromStdString(before.model),
                         QString::fromStdString(after.reasoningEffort), QString::fromStdString(before.reasoningEffort));
    QVERIFY2(!changed && after.model == before.model && after.reasoningEffort == before.reasoningEffort,
             qPrintable(diagnostic));
    QVERIFY(!controller.lastError().isEmpty());
    QCOMPARE(requestedProviders, (QStringList{QStringLiteral("deepseek")}));
}

void AgentControllerTest::sessionConfigurationReflectsOnlySelectedProviderCredentials() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QHash<QString, QString> keys{{QStringLiteral("glm"), QStringLiteral("synthetic-glm-key")},
                                {QStringLiteral("deepseek"), QStringLiteral("synthetic-deepseek-key")}};
    QStringList requestedProviders;
    AgentController::ModelConfig model;
    model.glmBaseUrl = QStringLiteral("http://127.0.0.1:1/glm-profile");
    model.deepseekBaseUrl = QStringLiteral("http://127.0.0.1:1/deepseek-profile");
    model.accountModelKeyProvider = [&](const QString& provider) {
        requestedProviders.push_back(provider);
        return keys.value(provider);
    };
    AgentController controller(model, QString());
    const QString session = controller.createSession(directory.path(), QStringLiteral("credential projection"));
    QVERIFY(!session.isEmpty());
    QVERIFY(controller.setSessionModelConfiguration(session, QStringLiteral("deepseek-v4-pro"),
                                                     QStringLiteral("low")));
    requestedProviders.clear();
    AgentController::SessionModelConfiguration config;
    QVERIFY(controller.sessionModelConfiguration(session, config));
    QCOMPARE(config.model, QStringLiteral("deepseek-v4-pro"));
    QCOMPARE(config.reasoningEffort, QStringLiteral("low"));
    QCOMPARE(config.wire, MaiWireApi::Responses);
    QVERIFY(config.configured);
    QCOMPARE(requestedProviders, (QStringList{QStringLiteral("deepseek")}));
    keys.remove(QStringLiteral("deepseek"));
    requestedProviders.clear();
    QVERIFY(controller.sessionModelConfiguration(session, config));
    QVERIFY(!config.configured);
    QCOMPARE(config.model, QStringLiteral("deepseek-v4-pro"));
    QCOMPARE(config.reasoningEffort, QStringLiteral("low"));
    QCOMPARE(requestedProviders, (QStringList{QStringLiteral("deepseek")}));
    keys.insert(QStringLiteral("deepseek"), QStringLiteral("new-synthetic-key"));
    QVERIFY(controller.sessionModelConfiguration(session, config));
    QVERIFY(config.configured);
    QVERIFY(controller.setSessionModelConfiguration(session, QStringLiteral("glm-5.3-flash"),
                                                     QStringLiteral("max")));
    QVERIFY(controller.sessionModelConfiguration(session, config));
    QCOMPARE(config.wire, MaiWireApi::ChatCompletions);
    QVERIFY(config.configured);
    QVERIFY(!controller.setSessionModelConfiguration(session, QStringLiteral("unknown-cloud-model"),
                                                      QStringLiteral("low")));
    QVERIFY(controller.sessionModelConfiguration(session, config));
    QCOMPARE(config.model, QStringLiteral("glm-5.3-flash"));
    QCOMPARE(config.reasoningEffort, QStringLiteral("max"));
}

void AgentControllerTest::sessionConfigurationWithoutClientRejectsUnknownModel() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    AgentController controller(AgentController::ModelConfig{}, QString());
    const QString session = controller.createSession(directory.path(), QStringLiteral("unconfigured cloud client"));
    QVERIFY(!session.isEmpty());
    MaiSession before;
    QVERIFY(controller.agent().getSession(session.toStdString(), before));
    const bool changed = controller.setSessionModelConfiguration(
        session, QStringLiteral("unknown-cloud-model"), QStringLiteral("max"));
    MaiSession after;
    QVERIFY(controller.agent().getSession(session.toStdString(), after));
    const QString diagnostic = QStringLiteral("accepted=%1; model=%2 (expected %3); effort=%4 (expected %5)")
        .arg(changed).arg(QString::fromStdString(after.model), QString::fromStdString(before.model),
                         QString::fromStdString(after.reasoningEffort), QString::fromStdString(before.reasoningEffort));
    QVERIFY2(!changed && after.model == before.model && after.reasoningEffort == before.reasoningEffort,
             qPrintable(diagnostic));
    // Known cloud metadata remains readable without a client, but an actual
    // selection or effort change must not be persisted as configured success.
    QVERIFY(!controller.setSessionModelConfiguration(session, QStringLiteral("glm-5.3"), QStringLiteral("low")));
    QVERIFY(!controller.setSessionModelConfiguration(session, QString::fromStdString(before.model),
                                                      QStringLiteral("max")));
    QVERIFY(controller.agent().getSession(session.toStdString(), after));
    QCOMPARE(QString::fromStdString(after.model), QString::fromStdString(before.model));
    QCOMPARE(QString::fromStdString(after.reasoningEffort), QString::fromStdString(before.reasoningEffort));
    QVERIFY(!controller.setDefaultModel(QStringLiteral("unknown-cloud-model")));
    const QString next = controller.createSession(directory.path(), QStringLiteral("default still valid"));
    AgentController::SessionModelConfiguration config;
    QVERIFY(controller.sessionModelConfiguration(next, config));
    QCOMPARE(config.model, QString::fromStdString(before.model));
    QVERIFY(!config.configured);
}

void AgentControllerTest::sessionConfigurationAcceptsInjectedLocalClient() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    AgentController controller(std::make_unique<SessionSelectionModel>(MaiWireApi::Local), QString());
    const QString session = controller.createSession(directory.path(), QStringLiteral("injected local client"));
    QVERIFY(!session.isEmpty());
    QVERIFY(controller.setSessionModelConfiguration(session, QStringLiteral("local:owned-test-model"),
                                                     QStringLiteral("low")));
    AgentController::SessionModelConfiguration config;
    QVERIFY(controller.sessionModelConfiguration(session, config));
    QCOMPARE(config.model, QStringLiteral("local:owned-test-model"));
    QCOMPARE(config.wire, MaiWireApi::Local);
    QVERIFY(config.configured);
}

void AgentControllerTest::credentialWaitDoesNotRetargetOtherSession() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    AccountRouteProbe glm;
    AccountRouteProbe deepseek;
    QVERIFY(glm.port > 0 && deepseek.port > 0);
    QVERIFY(glm.server.is_running() && deepseek.server.is_running());
    std::atomic<bool> keyWaitStarted{false};
    std::atomic<bool> keyWaitArmed{false};
    std::atomic<bool> releaseKey{false};
    std::atomic<bool> glmKeyAvailable{true};
    QThread* const ownerThread = QThread::currentThread();
    AgentController::ModelConfig model;
    model.glmBaseUrl = glm.baseUrl(QStringLiteral("glm-profile"));
    model.deepseekBaseUrl = deepseek.baseUrl(QStringLiteral("deepseek-profile"));
    model.accountModelKeyProvider = [&](const QString& provider) {
        // Only delay streaming acquisition after setup. Cached configuration
        // checks on the owner thread must remain nonblocking even while armed.
        if (provider == QStringLiteral("glm") && keyWaitArmed.load() && QThread::currentThread() != ownerThread) {
            keyWaitStarted.store(true);
            while (!releaseKey.load())
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return provider == QStringLiteral("glm") && !glmKeyAvailable.load()
                   ? QString() : provider + QStringLiteral("-owned-fixture-key");
    };
    AgentController controller(model, QString());
    struct ReleaseOnExit {
        std::atomic<bool>& release;
        ~ReleaseOnExit() { release.store(true); }
    } releaseOnExit{releaseKey};
    const QString first = controller.createSession(directory.path(), QStringLiteral("waiting first"));
    const QString second = controller.createSession(directory.path(), QStringLiteral("selected second"));
    QVERIFY(!first.isEmpty() && !second.isEmpty());
    QVERIFY(controller.setSessionModelConfiguration(first, QStringLiteral("glm-5.3"), QStringLiteral("low")));
    QVERIFY(controller.setSessionModelConfiguration(second, QStringLiteral("deepseek-v4-pro"), QStringLiteral("max")));
    QSignalSpy finished(&controller, &AgentController::turnFinished);
    keyWaitArmed.store(true);
    QVERIFY(controller.sendPrompt(first, QStringLiteral("wait for first provider credential")));
    QTRY_VERIFY_WITH_TIMEOUT(keyWaitStarted.load(), 10'000);
    AgentController::SessionModelConfiguration firstConfig;
    QVERIFY(controller.sessionModelConfiguration(first, firstConfig));
    QCOMPARE(firstConfig.model, QStringLiteral("glm-5.3"));
    QVERIFY(firstConfig.configured);
    glmKeyAvailable.store(false);
    QVERIFY(controller.setSessionModelConfiguration(first, QStringLiteral("glm-5.3"), QStringLiteral("low")));
    QVERIFY(!controller.setSessionModelConfiguration(first, QStringLiteral("glm-5.3-flash"), QStringLiteral("max")));
    MaiSession heldSession;
    QVERIFY(controller.agent().getSession(first.toStdString(), heldSession));
    QCOMPARE(QString::fromStdString(heldSession.model), QStringLiteral("glm-5.3"));
    QCOMPARE(QString::fromStdString(heldSession.reasoningEffort), QStringLiteral("low"));
    glmKeyAvailable.store(true);
    QVERIFY(controller.setSessionModelConfiguration(second, QStringLiteral("deepseek-flash"), QStringLiteral("high")));
    releaseKey.store(true);
    QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 10'000);
    QVERIFY(controller.sendPrompt(second, QStringLiteral("second keeps its own selection")));
    QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 2, 10'000);
    const auto firstRequests = glm.recordedRequests();
    const auto secondRequests = deepseek.recordedRequests();
    QCOMPARE(firstRequests.size(), std::size_t(1));
    QCOMPARE(secondRequests.size(), std::size_t(1));
    QCOMPARE(firstRequests.front().body.value(QStringLiteral("model")).toString(), QStringLiteral("glm-5.3"));
    QCOMPARE(firstRequests.front().body.value(QStringLiteral("reasoning_effort")).toString(), QStringLiteral("low"));
    QCOMPARE(secondRequests.front().body.value(QStringLiteral("model")).toString(), QStringLiteral("deepseek-flash"));
    QCOMPARE(secondRequests.front().body.value(QStringLiteral("reasoning")).toObject()
                 .value(QStringLiteral("effort")).toString(), QStringLiteral("high"));
}

void AgentControllerTest::accountModelClientUsesProviderProtocolAndProfile_data() {
    QTest::addColumn<QString>("model");
    QTest::addColumn<QString>("provider");
    QTest::addColumn<QString>("effort");
    for (const QString& model : {QStringLiteral("glm-5.3"), QStringLiteral("glm-5.3-flash"),
                                 QStringLiteral("deepseek-flash"), QStringLiteral("deepseek-v4-pro")}) {
        for (const QString& effort : {QStringLiteral("low"), QStringLiteral("high"), QStringLiteral("max")}) {
            const QByteArray name = (model + QLatin1Char('-') + effort).toUtf8();
            QTest::newRow(name.constData()) << model
                << (model.startsWith(QStringLiteral("glm-")) ? QStringLiteral("glm")
                                                            : QStringLiteral("deepseek"))
                << effort;
        }
    }
}

void AgentControllerTest::accountModelClientUsesProviderProtocolAndProfile() {
    QFETCH(QString, model);
    QFETCH(QString, provider);
    QFETCH(QString, effort);
    AccountRouteProbe glm;
    AccountRouteProbe deepseek;
    QVERIFY(glm.port > 0 && deepseek.port > 0);
    QVERIFY(glm.server.is_running() && deepseek.server.is_running());
    QStringList requestedProviders;
    DesktopAccountModelClient::Config config;
    config.glmBaseUrl = glm.baseUrl(QStringLiteral("glm-profile"));
    config.deepseekBaseUrl = deepseek.baseUrl(QStringLiteral("deepseek-profile"));
    config.keyProvider = [&](const QString& requestedProvider) {
        requestedProviders.push_back(requestedProvider);
        return requestedProvider + QStringLiteral("-synthetic-key");
    };
    DesktopAccountModelClient client(config);
    QCOMPARE(client.wireApi(), MaiWireApi::Mixed);
    QCOMPARE(client.wireApiForModel("glm-5.3").value_or(MaiWireApi::Mixed), MaiWireApi::ChatCompletions);
    QCOMPARE(client.wireApiForModel("glm-5.3-flash").value_or(MaiWireApi::Mixed), MaiWireApi::ChatCompletions);
    QCOMPARE(client.wireApiForModel("deepseek-flash").value_or(MaiWireApi::Mixed), MaiWireApi::Responses);
    QCOMPARE(client.wireApiForModel("deepseek-v4-pro").value_or(MaiWireApi::Mixed), MaiWireApi::Responses);
    QVERIFY(!client.wireApiForModel("unknown-cloud-model"));
    QVERIFY(requestedProviders.isEmpty());
    MaiModelRequest request;
    request.model = model.toStdString();
    request.reasoningEffort = effort.toStdString();
    request.messages.push_back(MaiModelMessage{MaiModelRole::User, "provider route probe"});
    const MaiStreamSink sink;
    const std::atomic<bool> cancel{false};
    const MaiError error = client.stream(request, sink, cancel);
    QVERIFY2(!error, error.message().c_str());
    QCOMPARE(requestedProviders, (QStringList{provider}));
    const bool isGlm = provider == QStringLiteral("glm");
    const auto received = isGlm ? glm.recordedRequests() : deepseek.recordedRequests();
    const auto unrelated = isGlm ? deepseek.recordedRequests() : glm.recordedRequests();
    QCOMPARE(received.size(), std::size_t(1));
    QVERIFY(unrelated.empty());
    QCOMPARE(received.front().authorization, QStringLiteral("Bearer ") + provider + QStringLiteral("-synthetic-key"));
    QCOMPARE(received.front().body.value(QStringLiteral("model")).toString(), model);
    QCOMPARE(received.front().path,
             isGlm ? QStringLiteral("/glm-profile/chat/completions")
                   : QStringLiteral("/deepseek-profile/responses"));
    if (isGlm) {
        QVERIFY(received.front().body.contains(QStringLiteral("messages")));
        QVERIFY(!received.front().body.contains(QStringLiteral("input")));
        QCOMPARE(received.front().body.value(QStringLiteral("thinking")).toObject()
                     .value(QStringLiteral("type")).toString(), QStringLiteral("enabled"));
        QCOMPARE(received.front().body.value(QStringLiteral("reasoning_effort")).toString(), effort);
    } else {
        QVERIFY(received.front().body.contains(QStringLiteral("input")));
        QVERIFY(!received.front().body.contains(QStringLiteral("messages")));
        QCOMPARE(received.front().body.value(QStringLiteral("reasoning")).toObject()
                     .value(QStringLiteral("effort")).toString(), effort);
    }
}

void AgentControllerTest::accountModelClientRoutesBySelectedModelAndFreshKey() {
    AccountRouteProbe glm;
    AccountRouteProbe deepseek;
    QVERIFY(glm.port > 0 && deepseek.port > 0);
    QVERIFY(glm.server.is_running() && deepseek.server.is_running());
    QHash<QString, QString> keys = {
        {QStringLiteral("glm"), QStringLiteral("glm-account-key")},
        {QStringLiteral("deepseek"), QStringLiteral("deepseek-account-key")}};
    DesktopAccountModelClient::Config config;
    QStringList requestedProviders;
    config.glmBaseUrl = glm.baseUrl(QStringLiteral("glm-profile"));
    config.deepseekBaseUrl = deepseek.baseUrl(QStringLiteral("deepseek-profile"));
    config.keyProvider = [&](const QString& provider) {
        requestedProviders.push_back(provider);
        return keys.value(provider);
    };
    DesktopAccountModelClient client(config);
    MaiModelRequest request;
    request.messages.push_back(MaiModelMessage{MaiModelRole::User, "Hello"});
    MaiStreamSink sink;
    const std::atomic<bool> cancel{false};
    request.reasoningEffort = "max";
    request.model = "glm-5.3";
    QVERIFY(!client.stream(request, sink, cancel));
    request.model = "deepseek-flash";
    QVERIFY(!client.stream(request, sink, cancel));
    QCOMPARE(glm.recordedRequests().size(), std::size_t(1));
    QCOMPARE(glm.recordedRequests().back().authorization, QStringLiteral("Bearer glm-account-key"));
    QCOMPARE(deepseek.recordedRequests().size(), std::size_t(1));
    QCOMPARE(deepseek.recordedRequests().back().authorization, QStringLiteral("Bearer deepseek-account-key"));
    keys.insert(QStringLiteral("deepseek"), QStringLiteral("rotated-account-key"));
    QVERIFY(!client.stream(request, sink, cancel));
    QCOMPARE(deepseek.recordedRequests().size(), std::size_t(2));
    QCOMPARE(deepseek.recordedRequests().back().authorization, QStringLiteral("Bearer rotated-account-key"));
    keys.remove(QStringLiteral("deepseek"));
    QCOMPARE(client.stream(request, sink, cancel).code(), MaiErrorCode::NotConfigured);
    QCOMPARE(deepseek.recordedRequests().size(), std::size_t(2));
    QCOMPARE(glm.recordedRequests().size(), std::size_t(1));
    // The other provider still has a valid key, so these no-HTTP assertions also
    // prove that missing credentials cannot silently switch providers.
    QCOMPARE(requestedProviders,
             (QStringList{QStringLiteral("glm"), QStringLiteral("deepseek"),
                          QStringLiteral("deepseek"), QStringLiteral("deepseek")}));
    keys.insert(QStringLiteral("deepseek"), QStringLiteral("restored-deepseek-key"));
    keys.insert(QStringLiteral("glm"), QStringLiteral("rotated-glm-key"));
    request.model = "glm-5.3";
    QVERIFY(!client.stream(request, sink, cancel));
    QCOMPARE(glm.recordedRequests().size(), std::size_t(2));
    QCOMPARE(glm.recordedRequests().back().authorization, QStringLiteral("Bearer rotated-glm-key"));
    keys.remove(QStringLiteral("glm"));
    QCOMPARE(client.stream(request, sink, cancel).code(), MaiErrorCode::NotConfigured);
    QCOMPARE(glm.recordedRequests().size(), std::size_t(2));
    QCOMPARE(deepseek.recordedRequests().size(), std::size_t(2));
    QCOMPARE(requestedProviders.mid(4), (QStringList{QStringLiteral("glm"), QStringLiteral("glm")}));
    request.model = "unknown-cloud-model";
    QCOMPARE(client.stream(request, sink, cancel).code(), MaiErrorCode::InvalidInput);
    QCOMPARE(requestedProviders.size(), 6);
    QCOMPARE(glm.recordedRequests().size(), std::size_t(2));
    QCOMPARE(deepseek.recordedRequests().size(), std::size_t(2));
}


#ifdef Q_OS_MACOS
void AgentControllerTest::accountModelClientUsesResolvedProxy() {
    // 本地假代理收到完整请求，证明账号客户端把目标 URL 交给路由并使用返回的代理。
    struct Proxy {
        httplib::Server server;
        std::thread thread;
        std::atomic<bool> listenerExited{false};
        std::mutex mutex;
        std::string target;
        std::string authorization;
        int count = 0;
        int port = 0;
        Proxy() {
            server.set_read_timeout(2);
            server.set_write_timeout(2);
            server.Post(".*", [this](const httplib::Request& request, httplib::Response& response) {
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    target = request.target;
                    authorization = request.get_header_value("Authorization");
                    ++count;
                }
                AccountRouteProbe::respond(request, response);
            });
            port = server.bind_to_any_port("127.0.0.1");
            if (port > 0) {
                thread = std::thread([this] {
                    server.listen_after_bind();
                    listenerExited.store(true);
                });
                for (int i = 0; i < 200 && !server.is_running() && !listenerExited.load(); ++i)
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        }
        ~Proxy() {
            if (thread.joinable()) {
                while (!server.is_running() && !listenerExited.load())
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                server.stop();
                thread.join();
            }
        }
    } proxy;
    QVERIFY(proxy.port > 0);
    std::vector<std::string> resolved;
    DesktopAccountModelClient::Config config;
    config.glmBaseUrl = QStringLiteral("http://model.test");
    config.keyProvider = [](const QString&) { return QStringLiteral("account-key"); };
    config.proxyResolver = [&](const std::string& url, MaiModelProxy& route) {
        resolved.push_back(url);
        route.url = "http://127.0.0.1:" + std::to_string(proxy.port);
        return true;
    };
    DesktopAccountModelClient client(config);
    MaiModelRequest request;
    request.model = "glm-5.3";
    MaiStreamSink sink;
    const std::atomic<bool> cancel{false};
    QVERIFY(!client.stream(request, sink, cancel));
    QCOMPARE(resolved.size(), std::size_t(1));
    QCOMPARE(QString::fromStdString(resolved.front()), QStringLiteral("http://model.test/chat/completions"));
    std::lock_guard<std::mutex> lock(proxy.mutex);
    QCOMPARE(proxy.count, 1);
    QCOMPARE(QString::fromStdString(proxy.target), QStringLiteral("http://model.test/chat/completions"));
    QCOMPARE(QString::fromStdString(proxy.authorization), QStringLiteral("Bearer account-key"));
}
#endif

void AgentControllerTest::desktopReadsOutsideSessionWorkingDirectory() {
    QTemporaryDir workspace;
    QTemporaryDir otherDirectory;
    QVERIFY(workspace.isValid());
    QVERIFY(otherDirectory.isValid());
    const QString source = QDir(otherDirectory.path()).filePath(QStringLiteral("outside.txt"));
    QFile file(source);
    QVERIFY(file.open(QIODevice::WriteOnly));
    QVERIFY(file.write("outside-visible") > 0);
    file.close();
    const std::string arguments =
        QJsonDocument(QJsonObject{{QStringLiteral("path"), source}})
            .toJson(QJsonDocument::Compact).toStdString();
    AgentController controller(std::make_unique<OutsideReadModel>(arguments), QString());
    const QString sessionId = controller.createSession(workspace.path());
    QVERIFY(controller.sendPrompt(sessionId, QStringLiteral("read the file")));
    controller.agent().waitIdle();

    bool readOutside = false;
    for (const MaiMessage& message : controller.agent().listMessages(sessionId.toStdString())) {
        for (const MaiMessagePart& part : message.parts) {
            const auto* tool = std::get_if<MaiToolPart>(&part.body);
            if (tool != nullptr && tool->tool == "file_read" &&
                tool->output.find("outside-visible") != std::string::npos)
                readOutside = true;
        }
    }
    QVERIFY(readOutside);
}

void AgentControllerTest::desktopWriteOutsideWaitsForApproval() {
    QTemporaryDir workspace;
    QTemporaryDir otherDirectory;
    QVERIFY(workspace.isValid());
    QVERIFY(otherDirectory.isValid());
    const QString target = QDir(otherDirectory.path()).filePath(QStringLiteral("outside.txt"));
    const std::string arguments = QJsonDocument(QJsonObject{
        {QStringLiteral("path"), target}, {QStringLiteral("content"), QStringLiteral("approved")}})
                                      .toJson(QJsonDocument::Compact).toStdString();
    AgentController controller(
        std::make_unique<OutsideReadModel>(arguments, "file_write"), QString());
    const QString sessionId = controller.createSession(workspace.path());
    QVERIFY(controller.sendPrompt(sessionId, QStringLiteral("write outside")));
    QTRY_COMPARE(controller.agent().listPendingPermissions().size(), std::size_t(1));
    QVERIFY(!QFile::exists(target));
    const QString permission =
        QString::fromStdString(controller.agent().listPendingPermissions().front().id);
    QVERIFY(controller.approvePermission(permission));
    controller.agent().waitIdle();
    QFile written(target);
    QVERIFY(written.open(QIODevice::ReadOnly));
    QCOMPARE(written.readAll(), QByteArray("approved"));
}

// 载荷不是文案：中文 + emoji，验证 UTF-8 一路（回调 -> 事件 -> QString）不走样。
// 合起来是 "你好，我在主线程上 🙂"
static const char kText[] =
    "你好，我在主线程上 \xF0\x9F\x99\x82";
static const char kDraft[] = "the user said hi; answer briefly";

void AgentControllerTest::streams_on_the_main_thread() {
    auto model = std::make_unique<ScriptedModel>(std::string(kDraft), std::string(kText));
    ScriptedModel* scripted = model.get();

    AgentController controller(std::move(model), QString());
    const QString sessionId = controller.createSession(QDir::currentPath());
    QVERIFY(!sessionId.isEmpty());

    // 记下每一条增量到达时所在的线程。
    //
    // **这里必须显式 DirectConnection。** 用默认的 Auto 会让这条检查变成假的：
    // Auto 自己就会在发射线程和接收线程不同的时候排队到主线程，
    // 于是不管 AgentController 有没有做排队，
    // lambda 都在主线程上跑——量到的是**Qt 替测试做的那一跳**，不是被测对象做的那一跳。
    //
    // 这不是推测：第一版就是 Auto，
    // 把 AgentController 里的 QueuedConnection 改成 DirectConnection 之后用例照样全绿。
    // 改成 Direct 之后才分得开。
    QString assembled;
    QVector<Qt::HANDLE> deliveryThreads;
    connect(&controller, &AgentController::textDelta, this,
            [&](const QString&, const QString&, const QString&, const QString& delta) {
                assembled += delta;
                deliveryThreads.append(QThread::currentThreadId());
            },
            Qt::DirectConnection);

    QSignalSpy finished(&controller, &AgentController::turnFinished);
    QVERIFY(controller.sendPrompt(sessionId, QStringLiteral("hi")));

    // sendPrompt 是异步的：这里必须转事件循环，否则排队的事件永远投递不出去。
    QVERIFY(finished.wait(15000));

    QCOMPARE(assembled, QString::fromUtf8(kText));

    // ---- 这一条才是这个类存在的理由 ----核心那一轮跑在自己的线程上，
    // 而每一条增量都必须在**主线程**上到达。
    QVERIFY(!deliveryThreads.isEmpty());
    const Qt::HANDLE mainThread = QThread::currentThreadId();
    for (Qt::HANDLE seen : deliveryThreads) {
        QCOMPARE(seen, mainThread);
    }
    // 而且两者确实**不是同一个线程**——否则上面那条断言恒真，等于什么都没验。
    QVERIFY(scripted->workerThread() != nullptr);
    QVERIFY(scripted->workerThread() != mainThread);
}

void AgentControllerTest::separates_reasoning_from_text() {
    auto model = std::make_unique<ScriptedModel>(std::string(kDraft), std::string(kText));
    AgentController controller(std::move(model), QString());
    const QString sessionId = controller.createSession(QDir::currentPath());

    QString text;
    QString reasoning;
    connect(&controller, &AgentController::textDelta, this,
            [&](const QString&, const QString&, const QString&, const QString& d) { text += d; });
    connect(&controller, &AgentController::reasoningDelta, this,
            [&](const QString&, const QString&, const QString&, const QString& d) {
                reasoning += d;
            });

    QSignalSpy finished(&controller, &AgentController::turnFinished);
    QVERIFY(controller.sendPrompt(sessionId, QStringLiteral("hi")));
    QVERIFY(finished.wait(15000));

    // 思考过程是模型的草稿。混进正文的话，
    // 界面上就是一段自言自语接着真答案——MaiAgent 那边真踩过这个，
    // 根因是事件广播早于落库导致订阅方分不出片段类型。
    QCOMPARE(text, QString::fromUtf8(kText));
    QCOMPARE(reasoning, QString::fromUtf8(kDraft));
}

void AgentControllerTest::reports_a_missing_model_instead_of_hanging() {
    // 没配模型时（界面刚装好、还没填 key）：建会话照常，发消息要**当场**拿到错误，
    // 不能挂在那儿等一个永远不会来的回答。
    AgentController::ModelConfig empty;
    AgentController controller(empty, QString());

    const QString sessionId = controller.createSession(QDir::currentPath());
    QVERIFY(!sessionId.isEmpty());

    QSignalSpy failed(&controller, &AgentController::turnFailed);
    controller.sendPrompt(sessionId, QStringLiteral("hi"));
    QVERIFY(failed.wait(10000));
    QVERIFY(!failed.first().at(1).toString().isEmpty());
}

void AgentControllerTest::sends_selected_images_as_multimodal_input() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString imagePath = directory.filePath(QStringLiteral("photo.png"));
    QFile image(imagePath);
    QVERIFY(image.open(QIODevice::WriteOnly));
    QCOMPARE(image.write("\x89PNG", 4), 4);
    image.close();

    auto model = std::make_unique<ScriptedModel>(std::string(), std::string(kText));
    ScriptedModel* scripted = model.get();
    AgentController controller(std::move(model), QString());
    const QString sessionId = controller.createSession(directory.path());
    QVERIFY(controller.setModel(sessionId, QStringLiteral("glm-5.3-flash")));
    QSignalSpy finished(&controller, &AgentController::turnFinished);
    QVERIFY(controller.sendPrompt(sessionId, QStringLiteral("看图"), {imagePath}));
    QVERIFY(finished.wait(15000));

    const MaiModelRequest request = scripted->lastRequest();
    QCOMPARE(request.messages.size(), std::size_t(1));
    QCOMPARE(request.messages.back().images.size(), std::size_t(1));
    QCOMPARE(QString::fromStdString(request.messages.back().images.front().path), imagePath);
    QCOMPARE(QString::fromStdString(request.messages.back().images.front().mimeType),
             QStringLiteral("image/png"));
}

void AgentControllerTest::registers_a_screenshot_tool_that_requires_approval_data() {
    QTest::addColumn<QString>("selectedModel");
    QTest::addColumn<QString>("expectedModel");
    QTest::addColumn<bool>("expectedViewImage");
    // 注册实例不等于对所有主模型开放。与核心的实际能力门禁保持一致，
    // 默认 glm-5.3 和 local:* 不能接收像素，glm-5.3-flash 可以。
    QTest::newRow("default-text-only") << QString() << QStringLiteral("glm-5.3") << false;
    QTest::newRow("explicit-text-only")
        << QStringLiteral("glm-5.3") << QStringLiteral("glm-5.3") << false;
    QTest::newRow("image-capable")
        << QStringLiteral("glm-5.3-flash") << QStringLiteral("glm-5.3-flash") << true;
    QTest::newRow("deepseek-cloud-image-capable")
        << QStringLiteral("deepseek-flash") << QStringLiteral("deepseek-flash") << true;
    QTest::newRow("local-text-only")
        << QStringLiteral("local:qa") << QStringLiteral("local:qa") << false;
}

void AgentControllerTest::registers_a_screenshot_tool_that_requires_approval() {
    QFETCH(QString, selectedModel);
    QFETCH(QString, expectedModel);
    QFETCH(bool, expectedViewImage);
    std::unique_ptr<MaiTool> screenshot = makeMaiScreenshotTool();
    QCOMPARE(QString::fromStdString(screenshot->name()), QStringLiteral("screenshot"));
    QVERIFY(screenshot->requiresApproval(QStringLiteral("{}").toStdString()));
    const QJsonObject screenshotSchema =
        QJsonDocument::fromJson(QByteArray::fromStdString(screenshot->parametersSchema())).object();
    const QJsonObject screenshotProperties = screenshotSchema.value(QStringLiteral("properties")).toObject();
    QVERIFY(!screenshotProperties.contains(QStringLiteral("mode")));
    QVERIFY(!screenshotProperties.contains(QStringLiteral("windowTitle")));
    const QJsonArray screenshotActions =
        screenshotProperties.value(QStringLiteral("action")).toObject().value(QStringLiteral("enum")).toArray();
    QCOMPARE(screenshotActions.size(), 2);
    QVERIFY(screenshotActions.contains(QStringLiteral("capture")));
    QVERIFY(screenshotActions.contains(QStringLiteral("stop")));
    MaiToolContext textModelContext;
    textModelContext.model = "glm-5.3";
    const MaiToolResult unsupported = screenshot->execute("{}", textModelContext);
    QVERIFY(unsupported.hasError());
    QCOMPARE(unsupported.error().code(), MaiErrorCode::NotSupported);
    QVERIFY(QString::fromStdString(unsupported.error().message()).contains(
        QStringLiteral("glm-5.3-flash")));

    auto model = std::make_unique<ScriptedModel>(std::string(), std::string(kText));
    ScriptedModel* scripted = model.get();
    AgentController controller(std::move(model), QString());
    const QString sessionId = controller.createSession(QDir::currentPath());
    if (!selectedModel.isEmpty()) QVERIFY(controller.setModel(sessionId, selectedModel));
    QSignalSpy finished(&controller, &AgentController::turnFinished);
    QVERIFY(controller.sendPrompt(sessionId, QStringLiteral("inspect the screen")));
    QVERIFY(finished.wait(15000));

    bool foundScreenshot = false;
    bool foundViewImage = false;
    bool foundListWindows = false;
    const MaiModelRequest request = scripted->lastRequest();
    QCOMPARE(QString::fromStdString(request.model), expectedModel);
    for (const MaiToolSpec& tool : request.tools) {
        if (tool.name == "screenshot") foundScreenshot = true;
        if (tool.name == "view_image") foundViewImage = true;
        if (tool.name == "list_windows") foundListWindows = true;
    }
    QCOMPARE(foundScreenshot, maiIsScreenshotSupported() && expectedViewImage);
    QCOMPARE(foundViewImage, expectedViewImage);
    QVERIFY(!foundListWindows);
}

// 验证宿主工具注册顺序与内置工具同时可见；顺序变化不能覆盖需要审批的内置工具。
void AgentControllerTest::registers_host_tools_after_builtin_tools() {
    auto model = std::make_unique<ScriptedModel>(std::string(), std::string(kText));
    ScriptedModel* scripted = model.get();
    AgentController controller(
        std::move(model), QString(),
        [](MaiToolRegistry& registry) { registry.add(std::make_unique<HostProbeTool>()); });
    const QString sessionId = controller.createSession(QDir::currentPath());
    QSignalSpy finished(&controller, &AgentController::turnFinished);
    QVERIFY(controller.sendPrompt(sessionId, QStringLiteral("use the host")));
    QVERIFY(finished.wait(15000));

    bool foundBuiltin = false;
    bool foundHost = false;
    bool foundAgentMedia = false;
    bool foundSessionList = false;
    bool foundSessionCreate = false;
    bool foundSessionDelete = false;
    bool desktopCreateOffersDirectory = false;
    bool foundSeedance = false;
    bool foundSeedream = false;
    bool foundArkAssets = false;
#if defined(MAICHAT_TEST_CV_VIDEO_ANALYSIS)
    bool foundSceneDetect = false;
    bool foundMotionDetect = false;
#endif
#if defined(MAICHAT_TEST_RVM_BUNDLED)
    bool foundVideoMatting = false;
#endif
    for (const MaiToolSpec& tool : scripted->lastRequest().tools) {
        if (tool.name == "file_read") foundBuiltin = true;
        if (tool.name == "agent_send_media") foundAgentMedia = true;
        if (tool.name == "session_list") foundSessionList = true;
        if (tool.name == "session_create") {
            foundSessionCreate = true;
            const QJsonObject schema = QJsonDocument::fromJson(
                QByteArray::fromStdString(tool.parametersJson)).object();
            desktopCreateOffersDirectory = schema.value(QStringLiteral("properties"))
                                               .toObject().contains(QStringLiteral("directory")) &&
                                           !schema.value(QStringLiteral("required"))
                                                .toArray().contains(QStringLiteral("directory"));
        }
        if (tool.name == "session_delete") foundSessionDelete = true;
        if (tool.name == "maichat_probe") foundHost = true;
        if (tool.name == "seedance_video") foundSeedance = true;
        if (tool.name == "seedream_image") foundSeedream = true;
        if (tool.name == "ark_assets") foundArkAssets = true;
#if defined(MAICHAT_TEST_CV_VIDEO_ANALYSIS)
        if (tool.name == "cv_scene_detect") foundSceneDetect = true;
        if (tool.name == "cv_motion_detect") foundMotionDetect = true;
#endif
#if defined(MAICHAT_TEST_RVM_BUNDLED)
        if (tool.name == "cv_video_matting") foundVideoMatting = true;
#endif
    }
    QVERIFY(foundBuiltin);
    QVERIFY(foundHost);
    QVERIFY(foundAgentMedia);
    QVERIFY(foundSessionList && foundSessionCreate && foundSessionDelete);
    QVERIFY(desktopCreateOffersDirectory);
    QVERIFY(!foundSeedance);
    QVERIFY(!foundSeedream);
    QVERIFY(!foundArkAssets);
    const std::string& instructions = scripted->lastRequest().baseInstructions;
    QVERIFY(instructions.find("Seedance") == std::string::npos);
    QVERIFY(instructions.find("Seedream") == std::string::npos);
    QVERIFY(instructions.find("ark_assets") == std::string::npos);
#if defined(MAICHAT_TEST_CV_VIDEO_ANALYSIS)
    QVERIFY(foundSceneDetect);
    QVERIFY(foundMotionDetect);
#endif
#if defined(MAICHAT_TEST_RVM_BUNDLED)
    const QDir applicationDirectory(QCoreApplication::applicationDirPath());
    const bool mattingAssetsPresent =
        QFileInfo::exists(applicationDirectory.filePath(
            QStringLiteral("MaiAgentModels/rvm_mobilenetv3_fp32.onnx"))) &&
        QFileInfo::exists(applicationDirectory.filePath(QStringLiteral("onnxruntime.dll"))) &&
        QFileInfo::exists(applicationDirectory.filePath(
            QStringLiteral("onnxruntime_providers_shared.dll")));
    QCOMPARE(foundVideoMatting, mattingAssetsPresent);
#endif
}

void AgentControllerTest::desktopSessionToolUsesAccountDefaultWhenPathOmitted() {
    QTemporaryDir account;
    QTemporaryDir customWorkspace;
    QVERIFY(account.isValid());
    QVERIFY(customWorkspace.isValid());
    auto model = std::make_unique<OutsideReadModel>(
        R"({"title":"Default workspace session"})", "session_create");
    AgentController controller(std::move(model), account.filePath(QStringLiteral("agent.db")));
    const QString current = controller.createSession(customWorkspace.path());
    QSignalSpy finished(&controller, &AgentController::turnFinished);
    QVERIFY(controller.sendPrompt(current, QStringLiteral("Create another conversation")));
    QVERIFY(finished.wait(15000));

    const auto sessions = controller.agent().listSessions();
    QCOMPARE(sessions.size(), std::size_t(2));
    const QString expected = QDir(account.path()).filePath(QStringLiteral("Workspace"));
    QVERIFY(QFileInfo(expected).isDir());
    for (const MaiSession& session : sessions) {
        if (session.id == current.toStdString()) continue;
        QCOMPARE(QDir(QString::fromStdString(session.directory)).canonicalPath(),
                 QDir(expected).canonicalPath());
        QCOMPARE(QString::fromStdString(session.title), QStringLiteral("Default workspace session"));
    }
}

void AgentControllerTest::desktopSessionToolConfirmsExplicitDirectory() {
    QTemporaryDir account;
    QTemporaryDir original;
    QTemporaryDir chosen;
    QVERIFY(account.isValid() && original.isValid() && chosen.isValid());
    const QByteArray arguments = QJsonDocument(QJsonObject{
        {QStringLiteral("directory"), chosen.path()},
        {QStringLiteral("title"), QStringLiteral("Chosen workspace session")}})
        .toJson(QJsonDocument::Compact);
    auto model = std::make_unique<OutsideReadModel>(arguments.toStdString(), "session_create");
    AgentController controller(std::move(model), account.filePath(QStringLiteral("agent.db")));
    const QString current = controller.createSession(original.path());
    QSignalSpy asked(&controller, &AgentController::permissionAsked);
    QSignalSpy finished(&controller, &AgentController::turnFinished);
    QVERIFY(controller.sendPrompt(current, QStringLiteral("Create in the chosen folder")));
    QTRY_COMPARE(asked.size(), 1);
    QCOMPARE(controller.agent().listSessions().size(), std::size_t(1));
    QVERIFY(controller.approvePermission(asked.first().at(0).toString()));
    QVERIFY(finished.wait(15000));

    const auto sessions = controller.agent().listSessions();
    QCOMPARE(sessions.size(), std::size_t(2));
    for (const MaiSession& session : sessions) {
        if (session.id == current.toStdString()) continue;
        QCOMPARE(QDir(QString::fromStdString(session.directory)).canonicalPath(),
                 QDir(chosen.path()).canonicalPath());
    }
}

void AgentControllerTest::finishingAnotherSessionDoesNotReclassifyReasoning() {
    auto model = std::make_unique<ConcurrentReasoningModel>();
    ConcurrentReasoningModel* controlled = model.get();
    AgentController controller(std::move(model), QString());
    const QString firstSession = controller.createSession(QDir::currentPath());
    const QString secondSession = controller.createSession(QDir::currentPath());
    QSignalSpy reasoning(&controller, &AgentController::reasoningDelta);
    QSignalSpy text(&controller, &AgentController::textDelta);
    QSignalSpy finished(&controller, &AgentController::turnFinished);

    QVERIFY(controller.sendPrompt(firstSession, QStringLiteral("session A")));
    QTRY_VERIFY(controlled->firstReasoningSent.load());
    QTRY_VERIFY(reasoning.count() >= 1);
    QVERIFY(controller.sendPrompt(secondSession, QStringLiteral("session B")));
    QTRY_VERIFY(std::any_of(finished.cbegin(), finished.cend(), [&](const QList<QVariant>& args) {
        return args.first().toString() == secondSession;
    }));
    controlled->release.store(true);
    controller.agent().waitIdle();
    QTRY_VERIFY(std::any_of(finished.cbegin(), finished.cend(), [&](const QList<QVariant>& args) {
        return args.first().toString() == firstSession;
    }));
    int firstReasoningParts = 0;
    for (const QList<QVariant>& args : reasoning) {
        if (args.first().toString() == firstSession) ++firstReasoningParts;
    }
    QCOMPARE(firstReasoningParts, 2);
    for (const QList<QVariant>& args : text) {
        QVERIFY(args.first().toString() != firstSession);
    }
}

void AgentControllerTest::messagePartEventsReadOnlyTheirOwner() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString database = directory.filePath(QStringLiteral("event-owner.sqlite"));
    const std::string owner = "ses_event_owner";
    const std::string target = "msg_history_0031";
    {
        auto opened = makeMaiSqliteStore(database.toUtf8().toStdString());
        QVERIFY(opened);
        auto store = std::move(opened.value());
        MaiSession session;
        session.id = owner;
        session.directory = directory.path().toUtf8().toStdString();
        session.created = session.updated = 1;
        store->putSession(session);
        for (int index = 0; index < 32; ++index) {
            MaiMessage message;
            message.id = QStringLiteral("msg_history_%1").arg(index, 4, 10, QLatin1Char('0')).toStdString();
            message.role = MaiRole::Assistant;
            message.created = message.completed = index + 1;
            message.parts.push_back({"prt_" + message.id, MaiTextPart{"stored answer"}, index + 1});
            if (message.id == target) {
                message.parts.push_back({"prt_reasoning", MaiReasoningPart{"private reasoning"}, index + 2});
                MaiToolPart tool;
                tool.tool = "read";
                tool.state = MaiToolState::Completed;
                tool.output = "done";
                message.parts.push_back({"prt_tool", std::move(tool), index + 3});
            }
            store->putMessage(owner, message);
        }
        QVERIFY(!store->lastWriteError().hasError());
    }
    ControllerReadProbe reads(database);
    QVERIFY(reads.installed);
    AgentController controller(std::make_unique<ScriptedModel>("", ""), database);
    QVERIFY2(controller.openError().isEmpty(), qPrintable(controller.openError()));
    QCOMPARE(reads.attachedConnections, 1);
    MaiMessage stored;
    reads.enabled = true;
    const bool found = controller.agent().getMessage(owner, target, stored);
    reads.enabled = false;
    QVERIFY(found);
    QCOMPARE(reads.messageRows, 1);  // Prove the probe sees the real connection before testing event cost.
    QCOMPARE(reads.partRows, 3);
    QSignalSpy tools(&controller, &AgentController::toolPartChanged);
    QSignalSpy reasoning(&controller, &AgentController::reasoningDelta);
    QSignalSpy text(&controller, &AgentController::textDelta);

    // Run the actual queued event path, with the corresponding parts already
    // persisted as required by the Core event contract. Both first classification
    // and later tool-state refreshes must read only that event's owning message.
    for (const std::string& part : {"prt_" + target, std::string("prt_reasoning"),
                                    std::string("prt_tool"), std::string("prt_tool")}) {
        MaiEvent updated;
        updated.type = MaiEventType::MessagePartUpdated;
        updated.sessionId = owner;
        updated.messageId = target;
        updated.partId = part;
        reads.messageRows = reads.partRows = 0;
        reads.enabled = true;
        controller.agent().eventBus().publish(updated);
        QCoreApplication::sendPostedEvents(&controller, QEvent::MetaCall);
        reads.enabled = false;
        QCOMPARE(reads.messageRows, 1);
        QCOMPARE(reads.partRows, 3);
    }
    QCOMPARE(tools.size(), 2);
    for (const QList<QVariant>& arguments : tools) {
        QCOMPARE(arguments.at(0).toString(), QString::fromStdString(owner));
        QCOMPARE(arguments.at(1).toString(), QString::fromStdString(target));
        QCOMPARE(arguments.at(2).toString(), QStringLiteral("prt_tool"));
    }

    for (const std::string& part : {"prt_" + target, std::string("prt_reasoning")}) {
        MaiEvent delta;
        delta.type = MaiEventType::MessagePartDelta;
        delta.sessionId = owner;
        delta.messageId = target;
        delta.partId = part;
        delta.field = "text";
        delta.delta = "continued";
        controller.agent().eventBus().publish(delta);
    }
    QCoreApplication::sendPostedEvents(&controller, QEvent::MetaCall);
    QCOMPARE(text.size(), 1);
    QCOMPARE(reasoning.size(), 1);

    // A foreign session cannot claim a valid message/part ID from this store.
    MaiEvent foreign;
    foreign.type = MaiEventType::MessagePartUpdated;
    foreign.sessionId = "ses_other_owner";
    foreign.messageId = target;
    foreign.partId = "prt_tool";
    reads.messageRows = reads.partRows = 0;
    reads.enabled = true;
    controller.agent().eventBus().publish(foreign);
    QCoreApplication::sendPostedEvents(&controller, QEvent::MetaCall);
    reads.enabled = false;
    QCOMPARE(reads.messageRows, 0);
    QCOMPARE(reads.partRows, 0);
    QCOMPARE(tools.size(), 2);
}

QTEST_MAIN(AgentControllerTest)
#include "AgentControllerTest.moc"
