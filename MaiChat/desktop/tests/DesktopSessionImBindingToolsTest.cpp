// Owned native tool fixtures do not send IM, start a model turn, or use a real
// account/database. Blocked-main tests deliberately keep queued callbacks late.
#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSemaphore>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QThread>
#include <QUuid>
#include <QtTest>

#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <thread>

#include "MaiTool.h"
#include "MaiChatTools/DesktopSessionImBindingTools.h"
#include "agent/AgentController.h"
#include "agent/DesktopSessionImBridge.h"
#include "app/RemoteIMApplication.h"
#include "im/FakeRemoteIMClient.h"
#include "ui/MainWindow.h"

namespace {

class OwnedHostSettings {
public:
    OwnedHostSettings()
        : organization_(QCoreApplication::organizationName()), application_(QCoreApplication::applicationName()),
          format_(QSettings::defaultFormat()), testMode_(QStandardPaths::isTestModeEnabled()) {
        QCoreApplication::setOrganizationName(QStringLiteral("MaiChatBindingToolHostTests"));
        QCoreApplication::setApplicationName(QUuid::createUuid().toString(QUuid::WithoutBraces));
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, directory_.path());
        QStandardPaths::setTestModeEnabled(true);
    }
    ~OwnedHostSettings() {
        QSettings::setDefaultFormat(format_);
        QStandardPaths::setTestModeEnabled(testMode_);
        QCoreApplication::setOrganizationName(organization_);
        QCoreApplication::setApplicationName(application_);
    }
private:
    QString organization_, application_;
    QSettings::Format format_;
    bool testMode_;
    QTemporaryDir directory_;
};

class OwnedModel final : public MaiModelClient {
public:
    std::atomic<int> calls{0};
    MaiWireApi wireApi() const override { return MaiWireApi::ChatCompletions; }
    MaiError stream(const MaiModelRequest&, const MaiStreamSink&, const std::atomic<bool>&) override {
        ++calls;
        return {};
    }
};

struct Fixture {
    QTemporaryDir workspace;
    QTemporaryDir historyDirectory;
    FakeRemoteIMClient* im = new FakeRemoteIMClient;
    OwnedModel* model = new OwnedModel;
    RemoteIMApplication app{QStringLiteral("owned-tool-owner"), std::unique_ptr<RemoteIMClient>(im),
        std::make_unique<LocalMessageDatabase>(historyDirectory.filePath(QStringLiteral("owned-tool-history.db")))};
    AgentController controller{std::unique_ptr<MaiModelClient>(model), QString()};
    DesktopSessionImBridge bridge{app, controller};
    DesktopSessionImBridge* currentBridge = &bridge;
    MaiToolRegistry registry;
    QString session;
    quint64 generation = 1;
    int providerCalls = 0;
    bool providerOnlyOnMain = true;
    std::atomic_bool* cancelDuringProvider = nullptr;
    Fixture(int budget = 5000) {
        app.addContact(QStringLiteral("owned-tool-friend"), QStringLiteral("Owned Friend"));
        app.connectToService(123, QStringLiteral("owned-unused-signature"));
        session = controller.createSession(workspace.path(), QStringLiteral("Owned Tool Session"));
        const auto account = app.client().currentAccount();
        const auto registration = generation;
        registerDesktopSessionImBindingTools(registry, &app, [this, account, registration] {
            ++providerCalls;
            providerOnlyOnMain = providerOnlyOnMain && QThread::currentThread() == app.thread();
            if (cancelDuringProvider && providerCalls == 2) cancelDuringProvider->store(true);
            if (!app.connectionReady() || account != app.client().currentAccount() ||
                account.ownerUserId != app.chatState().ownerUserId() || generation != registration)
                return static_cast<DesktopSessionImBridge*>(nullptr);
            return currentBridge;
        }, budget);
    }
    MaiTool* tool() const { return registry.find("maichat_session_im_binding"); }
    std::string args(const char* action, bool withSession = false) const {
        QJsonObject json{{QStringLiteral("action"), QString::fromLatin1(action)}};
        if (QByteArray(action) != "list") json.insert(QStringLiteral("peer_id"), QStringLiteral("owned-tool-friend"));
        if (withSession) json.insert(QStringLiteral("session_id"), session);
        return QJsonDocument(json).toJson(QJsonDocument::Compact).toStdString();
    }
};

QJsonObject output(const MaiToolResult& result) {
    return QJsonDocument::fromJson(QByteArray::fromStdString(result.output())).object();
}

void verifyNotStarted(const MaiToolResult& result) {
    QVERIFY(result.hasError());
    QVERIFY(result.execution().has_value());
    QVERIFY(result.execution()->started.has_value());
    QCOMPARE(result.execution()->started.value(), false);
    QCOMPARE(result.execution()->effectStatus, MaiToolEffectStatus::None);
}

}  // namespace

class DesktopSessionImBindingToolsTest final : public QObject {
    Q_OBJECT
private slots:
    void listBindUnbindUseTheSameOrdinaryBridgeAndApprovalPolicy();
    void actualHostRegistrarPinsConfiguredStartupIdentityAndRejectsOldGenerations();
    void strictArgumentsAreRejectedWithoutCallingTheProvider_data();
    void strictArgumentsAreRejectedWithoutCallingTheProvider();
    void staleRegistrationCannotAdoptAnotherControllerOrSdkAccount();
    void blockedMainCancellationRevokesTheNativeCancelLease();
    void blockedMainDeadlineLeavesNoLateMutation();
    void finalProviderCancellationIsCheckedBeforeMutation();
    void timeoutOrCancellationAfterMutationStartsReportsUnknownEffects_data();
    void timeoutOrCancellationAfterMutationStartsReportsUnknownEffects();
};

void DesktopSessionImBindingToolsTest::listBindUnbindUseTheSameOrdinaryBridgeAndApprovalPolicy() {
    Fixture f;
    QTRY_VERIFY(f.app.connectionReady());
    QVERIFY(f.tool());
    const auto schema = QJsonDocument::fromJson(QByteArray::fromStdString(f.tool()->parametersSchema())).object();
    QCOMPARE(schema.value(QStringLiteral("type")).toString(), QStringLiteral("object"));
    QVERIFY(!schema.contains(QStringLiteral("oneOf")));
    QVERIFY(!schema.contains(QStringLiteral("anyOf")));
    const auto properties = schema.value(QStringLiteral("properties")).toObject();
    QCOMPARE(properties.size(), 3);
    QVERIFY(properties.contains(QStringLiteral("action")));
    QVERIFY(properties.contains(QStringLiteral("peer_id")));
    QVERIFY(properties.contains(QStringLiteral("session_id")));
    QCOMPARE(properties.value(QStringLiteral("action")).toObject().value(QStringLiteral("enum")).toArray(),
             QJsonArray({QStringLiteral("list"), QStringLiteral("bind"), QStringLiteral("unbind")}));
    QCOMPARE(schema.value(QStringLiteral("required")).toArray(), QJsonArray({QStringLiteral("action")}));
    QCOMPARE(schema.value(QStringLiteral("additionalProperties")).toBool(true), false);
    QVERIFY(!f.tool()->requiresApproval(f.args("list")));
    QVERIFY(!f.tool()->requiresPerCallApproval(f.args("list")));
    QVERIFY(f.tool()->requiresApproval(f.args("bind", true)));
    QVERIFY(f.tool()->requiresPerCallApproval(f.args("bind", true)));
    QVERIFY(f.tool()->requiresApproval(f.args("unbind")));
    QVERIFY(f.tool()->requiresPerCallApproval(f.args("unbind")));
    QVERIFY(!f.tool()->requiresApproval("not-json"));
    QVERIFY(!f.tool()->requiresPerCallApproval("not-json"));
    MaiToolContext context;
    const auto listed = f.tool()->execute(f.args("list"), context);
    QVERIFY(!listed.hasError());
    QCOMPARE(output(listed).value(QStringLiteral("bindings")).toArray().size(), 0);
    const auto bound = f.tool()->execute(f.args("bind", true), context);
    QVERIFY(!bound.hasError());
    QVERIFY(output(bound).value(QStringLiteral("changed")).toBool());
    QVERIFY(output(bound).value(QStringLiteral("bound")).toBool());
    QCOMPARE(output(bound).value(QStringLiteral("session_id")).toString(), f.session);
    QCOMPARE(f.bridge.boundSession(QStringLiteral("owned-tool-friend")), f.session);
    QCOMPARE(bound.execution()->effectStatus, MaiToolEffectStatus::Confirmed);
    const auto same = f.tool()->execute(f.args("bind", true), context);
    QVERIFY(!same.hasError());
    QVERIFY(!output(same).value(QStringLiteral("changed")).toBool());
    QCOMPARE(same.execution()->effectStatus, MaiToolEffectStatus::None);
    const auto current = f.tool()->execute(f.args("list"), context);
    QCOMPARE(output(current).value(QStringLiteral("bindings")).toArray().first().toObject()
                 .value(QStringLiteral("session_id")).toString(), f.session);
    const auto unbound = f.tool()->execute(f.args("unbind"), context);
    QVERIFY(!unbound.hasError());
    QVERIFY(output(unbound).value(QStringLiteral("changed")).toBool());
    QVERIFY(!output(unbound).value(QStringLiteral("bound")).toBool());
    QVERIFY(f.bridge.boundSession(QStringLiteral("owned-tool-friend")).isEmpty());
    QVERIFY(f.providerOnlyOnMain);
    QCOMPARE(f.model->calls.load(), 0);
    QVERIFY(f.im->lastText().isEmpty());
}

void DesktopSessionImBindingToolsTest::actualHostRegistrarPinsConfiguredStartupIdentityAndRejectsOldGenerations() {
    OwnedHostSettings settings;
    QTemporaryDir workspace;
    auto client = std::make_unique<FakeRemoteIMClient>();
    auto* im = client.get();
    RemoteIMApplication app(QStringLiteral("owned-startup-owner"), std::move(client),
        std::make_unique<LocalMessageDatabase>(workspace.filePath(QStringLiteral("owned-startup-history.db"))));
    app.addContact(QStringLiteral("owned-startup-friend"), QStringLiteral("Owned Startup Friend"));
    // Match main.cpp: construct the real host with trusted SDK configuration
    // before its ordinary IM connection establishes currentAccount().
    MainWindow window(app, 123, [](const QString&) { return QStringLiteral("owned-unused-signature"); });
    MaiToolRegistry originalRegistry;
    window.registerSessionImBindingTool(originalRegistry);
    auto* original = originalRegistry.find("maichat_session_im_binding");
    QVERIFY(original);
    MaiToolContext context;
    const std::string list = R"({"action":"list"})";
    verifyNotStarted(original->execute(list, context));

    app.connectToService(123, QStringLiteral("owned-unused-signature"));
    QTRY_VERIFY(app.connectionReady());
    const auto listed = original->execute(list, context);
    QVERIFY(!listed.hasError());
    const QString session = window.activeAgentController()->createSession(workspace.path(), QStringLiteral("Owned Startup Session"));
    const auto bind = QJsonDocument(QJsonObject{
        {QStringLiteral("action"), QStringLiteral("bind")},
        {QStringLiteral("peer_id"), QStringLiteral("owned-startup-friend")},
        {QStringLiteral("session_id"), session}}).toJson(QJsonDocument::Compact).toStdString();
    const auto bound = original->execute(bind, context);
    QVERIFY(!bound.hasError());
    QVERIFY(output(bound).value(QStringLiteral("bound")).toBool());
    QCOMPARE(output(original->execute(list, context)).value(QStringLiteral("bindings")).toArray().size(), 1);
    QVERIFY(window.activeAgentController()->agent().listMessages(session.toStdString()).empty());
    QVERIFY(im->lastText().isEmpty());

    // The ordinary configuration setter rebuilds the actual controller/bridge
    // with the same SDK identity. A retained old registry cannot adopt it.
    window.setMediaServiceAccessProvider([] { return QPair<QString, QString>{}; });
    verifyNotStarted(original->execute(list, context));
    MaiToolRegistry rebuiltRegistry;
    window.registerSessionImBindingTool(rebuiltRegistry);
    auto* rebuilt = rebuiltRegistry.find("maichat_session_im_binding");
    QVERIFY(rebuilt);
    QVERIFY(!rebuilt->execute(list, context).hasError());

    app.disconnectFromService();
    QTRY_VERIFY(!app.connectionReady());
    app.connectToService(456, QStringLiteral("owned-unused-signature"));
    QTRY_VERIFY(app.connectionReady());
    verifyNotStarted(rebuilt->execute(list, context));
    QVERIFY(window.activeAgentController()->agent().listMessages(session.toStdString()).empty());
    QVERIFY(im->lastText().isEmpty());
}

void DesktopSessionImBindingToolsTest::strictArgumentsAreRejectedWithoutCallingTheProvider_data() {
    QTest::addColumn<QByteArray>("arguments");
    for (const auto& value : {
             QByteArray("not-json"), QByteArray("[]"), QByteArray("{}"),
             QByteArray(R"({"action":"unknown"})"), QByteArray(R"({"action":"list","peer_id":"friend"})"),
             QByteArray(R"({"action":"list","session_id":"ses_owned"})"),
             QByteArray(R"({"action":"bind","peer_id":"friend"})"),
             QByteArray(R"({"action":"bind","peer_id":5,"session_id":"ses_owned"})"),
             QByteArray(R"({"action":"bind","peer_id":" friend ","session_id":"ses_owned"})"),
             QByteArray(R"({"action":"unbind","peer_id":"friend","session_id":"ses_owned"})"),
             QByteArray(R"({"action":"unbind","peer_id":"friend","extra":true})")})
        QTest::newRow(value.constData()) << value;
    QTest::newRow("oversized") << QByteArray(4097, 'x');
}

void DesktopSessionImBindingToolsTest::strictArgumentsAreRejectedWithoutCallingTheProvider() {
    QFETCH(QByteArray, arguments);
    Fixture f;
    QTRY_VERIFY(f.app.connectionReady());
    MaiToolContext context;
    QVERIFY(!f.tool()->requiresApproval(arguments.toStdString()));
    QVERIFY(!f.tool()->requiresPerCallApproval(arguments.toStdString()));
    const auto invalid = f.tool()->execute(arguments.toStdString(), context);
    verifyNotStarted(invalid);
    QCOMPARE(invalid.error().code(), MaiErrorCode::InvalidInput);
    QCOMPARE(f.providerCalls, 0);
    QVERIFY(f.bridge.bindings().isEmpty());
}

void DesktopSessionImBindingToolsTest::staleRegistrationCannotAdoptAnotherControllerOrSdkAccount() {
    Fixture f;
    QTRY_VERIFY(f.app.connectionReady());
    MaiToolContext context;
    AgentController replacement(std::make_unique<OwnedModel>(), QString());
    DesktopSessionImBridge replacementBridge(f.app, replacement);
    const QString replacementSession = replacement.createSession(f.workspace.path(), QStringLiteral("Replacement Session"));
    f.currentBridge = &replacementBridge;
    ++f.generation;
    const auto replacementArgs = QJsonDocument(QJsonObject{
        {QStringLiteral("action"), QStringLiteral("bind")},
        {QStringLiteral("peer_id"), QStringLiteral("owned-tool-friend")},
        {QStringLiteral("session_id"), replacementSession}}).toJson(QJsonDocument::Compact).toStdString();
    verifyNotStarted(f.tool()->execute(replacementArgs, context));
    QVERIFY(replacementBridge.bindings().isEmpty());
    QVERIFY(f.bridge.bindings().isEmpty());
    f.currentBridge = &f.bridge;
    --f.generation;
    f.im->connectToService(456, QStringLiteral("owned-tool-owner"), QStringLiteral("owned-unused-signature"), {});
    verifyNotStarted(f.tool()->execute(f.args("bind", true), context));
    QVERIFY(f.bridge.bindings().isEmpty());
}

void DesktopSessionImBindingToolsTest::blockedMainCancellationRevokesTheNativeCancelLease() {
    Fixture f(1000);
    QTRY_VERIFY(f.app.connectionReady());
    auto nativeCancel = std::make_unique<std::atomic_bool>(false);
    MaiToolContext context;
    context.cancel = nativeCancel.get();
    std::atomic_bool entered{false};
    auto pending = std::async(std::launch::async, [&] {
        entered = true;
        return f.tool()->execute(f.args("bind", true), context);
    });
    while (!entered.load()) QThread::msleep(1);
    QThread::msleep(20);  // Main does not pump the queued operation.
    nativeCancel->store(true);
    QVERIFY(pending.wait_for(std::chrono::seconds(1)) == std::future_status::ready);
    const auto canceled = pending.get();
    verifyNotStarted(canceled);
    QCOMPARE(canceled.error().code(), MaiErrorCode::Canceled);
    nativeCancel.reset();  // A late Main callback must not dereference it.
    QCoreApplication::processEvents();
    QVERIFY(f.bridge.bindings().isEmpty());
    QCOMPARE(f.providerCalls, 0);
}

void DesktopSessionImBindingToolsTest::blockedMainDeadlineLeavesNoLateMutation() {
    Fixture f(40);
    QTRY_VERIFY(f.app.connectionReady());
    MaiToolContext context;
    auto pending = std::async(std::launch::async, [&] { return f.tool()->execute(f.args("bind", true), context); });
    QVERIFY(pending.wait_for(std::chrono::seconds(1)) == std::future_status::ready);
    const auto expired = pending.get();
    verifyNotStarted(expired);
    QVERIFY(expired.execution()->timedOut);
    QCoreApplication::processEvents();
    QVERIFY(f.bridge.bindings().isEmpty());
    QCOMPARE(f.providerCalls, 0);
}

void DesktopSessionImBindingToolsTest::finalProviderCancellationIsCheckedBeforeMutation() {
    Fixture f;
    QTRY_VERIFY(f.app.connectionReady());
    std::atomic_bool nativeCancel{false};
    f.cancelDuringProvider = &nativeCancel;
    MaiToolContext context;
    context.cancel = &nativeCancel;
    const auto canceled = f.tool()->execute(f.args("bind", true), context);
    verifyNotStarted(canceled);
    QVERIFY(nativeCancel.load());
    QVERIFY(f.bridge.bindings().isEmpty());
}

void DesktopSessionImBindingToolsTest::timeoutOrCancellationAfterMutationStartsReportsUnknownEffects_data() {
    QTest::addColumn<bool>("cancelAfterStart");
    QTest::newRow("deadline-after-start") << false;
    QTest::newRow("cancel-after-start") << true;
}

void DesktopSessionImBindingToolsTest::timeoutOrCancellationAfterMutationStartsReportsUnknownEffects() {
    QFETCH(bool, cancelAfterStart);
    Fixture f(cancelAfterStart ? 1000 : 40);
    QTRY_VERIFY(f.app.connectionReady());
    QSemaphore mutationEntered;
    QSemaphore release;
    QObject::connect(&f.bridge, &DesktopSessionImBridge::bindingChanged, &f.app, [&] {
        mutationEntered.release();
        release.tryAcquire(1, 1000);
    });
    std::atomic_bool nativeCancel{false};
    MaiToolContext context;
    context.cancel = &nativeCancel;
    auto pending = std::async(std::launch::async, [&] { return f.tool()->execute(f.args("bind", true), context); });
    std::thread releaser([&] {
        if (mutationEntered.tryAcquire(1, 1000)) {
            if (cancelAfterStart) nativeCancel.store(true);
            QThread::msleep(100);
        }
        release.release();
    });
    while (pending.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready)
        QCoreApplication::processEvents();
    releaser.join();
    const auto uncertain = pending.get();
    QVERIFY(uncertain.hasError());
    QVERIFY(uncertain.execution().has_value());
    QVERIFY(uncertain.execution()->started.has_value());
    QCOMPARE(uncertain.execution()->started.value(), true);
    QCOMPARE(uncertain.execution()->effectStatus, MaiToolEffectStatus::Unknown);
    QCOMPARE(uncertain.execution()->nextAction, MaiToolNextAction::InspectState);
    QCOMPARE(uncertain.execution()->canceled, cancelAfterStart);
    QCOMPARE(f.bridge.boundSession(QStringLiteral("owned-tool-friend")), f.session);
    QCOMPARE(f.model->calls.load(), 0);
    QVERIFY(f.im->lastText().isEmpty());
}

QTEST_MAIN(DesktopSessionImBindingToolsTest)
#include "DesktopSessionImBindingToolsTest.moc"
