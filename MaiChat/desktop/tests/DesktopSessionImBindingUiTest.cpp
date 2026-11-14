// The ordinary binding UI uses owned settings, fake IM and empty local Sessions.
// These cases do not start a model turn or send an IM message.
#include <QApplication>
#include <QDialog>
#include <QDir>
#include <QFile>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QSettings>
#include <QScopeGuard>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTextEdit>
#include <QTimer>
#include <QUuid>
#include <QtTest>

#include <memory>

#include "agent/AgentController.h"
#include "agent/DesktopSessionImBridge.h"
#include "agent/AgentChatPanel.h"
#include "app/RemoteIMApplication.h"
#include "im/FakeRemoteIMClient.h"
#include "ui/MainWindow.h"

namespace {

bool captureOwnedWidget(QWidget& widget, const QString& name) {
    const QString directory = qEnvironmentVariable("MAICHAT_SESSION_IM_UI_EVIDENCE_DIR");
    if (directory.isEmpty()) return true;
    return QDir().mkpath(directory) && widget.grab().save(QDir(directory).filePath(name + QStringLiteral(".png")));
}

class OwnedSettings {
public:
    OwnedSettings()
        : organization_(QCoreApplication::organizationName()), application_(QCoreApplication::applicationName()),
          format_(QSettings::defaultFormat()), testMode_(QStandardPaths::isTestModeEnabled()) {
        QCoreApplication::setOrganizationName(QStringLiteral("MaiChatSessionImUiTests"));
        QCoreApplication::setApplicationName(QUuid::createUuid().toString(QUuid::WithoutBraces));
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, directory_.path());
        QStandardPaths::setTestModeEnabled(true);
    }
    ~OwnedSettings() {
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

struct Fixture {
    OwnedSettings settings;
    QTemporaryDir workspace;
    QTemporaryDir historyDirectory;
    FakeRemoteIMClient* im = new FakeRemoteIMClient;
    RemoteIMApplication app{QStringLiteral("owned-ui-owner"), std::unique_ptr<RemoteIMClient>(im),
        std::make_unique<LocalMessageDatabase>(historyDirectory.filePath(QStringLiteral("owned-ui-history.db")))};
    std::unique_ptr<MainWindow> window;

    Fixture() {
        app.addContact(QStringLiteral("owned-ui-friend"), QStringLiteral("Owned Friend"));
        app.addContact(QStringLiteral("owned-ui-other"), QStringLiteral("Other Friend"));
        app.connectToService(123, QStringLiteral("owned-unused-signature"));
    }
    void show() {
        window = std::make_unique<MainWindow>(app);
        window->resize(1100, 750);
        window->show();
        app.selectPeer(QStringLiteral("owned-ui-friend"));
    }
    QPushButton* entry() const {
        return window->findChild<QPushButton*>(QStringLiteral("composerSessionBindingButton"));
    }
    QLabel* boundName() const {
        return window->findChild<QLabel*>(QStringLiteral("imSessionBindingName"));
    }
    QString session(const QString& title = QStringLiteral("Owned UI Session")) {
        return window->activeAgentController()->createSession(workspace.path(), title);
    }
    bool bind(const QString& sessionId) {
        bool selected = false;
        bool captured = true;
        QTimer::singleShot(0, window.get(), [&, sessionId] {
            auto* picker = window->findChild<QDialog*>(QStringLiteral("imSessionBindingDialog"));
            if (!picker) return;
            auto* list = picker->findChild<QListWidget*>(QStringLiteral("imSessionBindingSessions"));
            auto* confirm = picker->findChild<QPushButton*>(QStringLiteral("imSessionBindingConfirm"));
            if (!list || !confirm) { picker->reject(); return; }
            for (int row = 0; row < list->count(); ++row)
                if (list->item(row)->data(Qt::UserRole).toString() == sessionId) list->setCurrentRow(row);
            selected = list->currentItem() && confirm->isEnabled();
            captured = captureOwnedWidget(*picker, QStringLiteral("session-binding-picker"));
            if (!qEnvironmentVariableIsEmpty("MAICHAT_SESSION_IM_UI_EVIDENCE_DIR") && list->count() > 1) {
                list->scrollToTop();
                captured = captureOwnedWidget(*picker, QStringLiteral("session-binding-picker-long-list")) && captured;
                if (list->currentItem()) list->scrollToItem(list->currentItem());
            }
            if (selected) confirm->click();
            else picker->reject();
        });
        entry()->click();
        QCoreApplication::processEvents();
        auto* bridge = window->findChild<DesktopSessionImBridge*>();
        return selected && captured && bridge && QTest::qWaitFor([&] {
            return bridge->boundSession(QStringLiteral("owned-ui-friend")) == sessionId &&
                !bridge->historyPending(QStringLiteral("owned-ui-friend"));
        }, 10000);
    }
};

}  // namespace

class DesktopSessionImBindingUiTest final : public QObject {
    Q_OBJECT
private slots:
    void pickerBindsExistingSessionWithoutSubmitting();
    void cancelKeepsDraftAndBindingOff();
    void savedBindingReturnsForTheSameAccountAndPeer();
    void savedBindingDoesNotActivateForAnotherAccount();
    void anotherPeerCannotReuseTheBoundSession();
    void localComposerSendRevokesAutomaticReplyBeforeSubmission();
    void openPickerTracksSessionsTitlesAndDeletionWithoutChangingDraft();
    void peerChangeInvalidatesOpenPicker();
};

void DesktopSessionImBindingUiTest::pickerBindsExistingSessionWithoutSubmitting() {
    Fixture f;
    QTRY_VERIFY(f.app.isConnected());
    f.show();
    QVERIFY(QTest::qWaitForWindowExposed(f.window.get()));
    const QString session = f.session();
    // 静态视觉取证用普通会话入口准备多行和长标题，不改绑定断言或触发模型请求。
    if (!qEnvironmentVariableIsEmpty("MAICHAT_SESSION_IM_UI_EVIDENCE_DIR")) {
        for (const auto& title : {QStringLiteral("自有会话 · 周计划"), QStringLiteral("自有会话 · 资料整理"),
                                  QStringLiteral("自有会话 · 产品需求、问题跟踪与本周回归验证的较长标题"),
                                  QStringLiteral("自有会话 · 旅行笔记")})
            f.session(title);
    }
    auto* command = f.window->findChild<QPushButton*>(QStringLiteral("composerCommandButton"));
    QVERIFY(f.entry() && command && f.boundName());
    QVERIFY(QFile::exists(QStringLiteral(":/maichat/icons/ai-sparkles.svg")));
    QVERIFY(!f.boundName()->isVisible());
    for (const QSize size : {QSize(1100, 750), QSize(850, 650)}) {
        f.window->resize(size);
        QCoreApplication::processEvents();
        QTRY_VERIFY(f.entry()->x() + f.entry()->width() <= command->x());
        QVERIFY(f.window->findChild<QObject*>(QStringLiteral("aiReplyButton")) == nullptr);
        QVERIFY(f.window->findChild<QObject*>(QStringLiteral("replySuggestionController")) == nullptr);
        QVERIFY(f.entry()->geometry().right() < f.entry()->parentWidget()->width());
        QVERIFY(captureOwnedWidget(*f.window, QStringLiteral("session-binding-blank-%1").arg(f.window->width())));
    }
    QVERIFY(f.bind(session));
    QTRY_VERIFY(f.boundName()->isVisible());
    QVERIFY(f.boundName()->text().contains(QStringLiteral("Owned UI Session")));
    QVERIFY(captureOwnedWidget(*f.window, QStringLiteral("session-binding-bound-window")));
    for (const QSize size : {QSize(1100, 750), QSize(850, 650)}) {
        f.window->resize(size);
        QCoreApplication::processEvents();
        QVERIFY(captureOwnedWidget(*f.window, QStringLiteral("session-binding-bound-%1").arg(f.window->width())));
    }
    QVERIFY(f.window->activeAgentController()->agent().listMessages(session.toStdString()).empty());
    QVERIFY(f.im->lastText().isEmpty());

    auto* unbind = f.window->findChild<QPushButton*>(QStringLiteral("imSessionBindingUnbind"));
    QVERIFY(unbind && unbind->isVisible());
    unbind->click();
    QTRY_VERIFY(!f.boundName()->isVisible());
}

void DesktopSessionImBindingUiTest::cancelKeepsDraftAndBindingOff() {
    Fixture f;
    QTRY_VERIFY(f.app.isConnected());
    f.show();
    QVERIFY(QTest::qWaitForWindowExposed(f.window.get()));
    const QString session = f.session();
    auto* editor = f.window->findChild<QTextEdit*>(QStringLiteral("messageEditor"));
    QVERIFY(editor && f.entry());
    editor->setPlainText(QStringLiteral("Owned unsent IM draft."));
    QTimer::singleShot(0, f.window.get(), [&] {
        auto* picker = f.window->findChild<QDialog*>(QStringLiteral("imSessionBindingDialog"));
        QVERIFY(picker);
        picker->reject();
    });
    f.entry()->click();
    QCOMPARE(editor->toPlainText(), QStringLiteral("Owned unsent IM draft."));
    QVERIFY(!f.boundName()->isVisible());
    QVERIFY(f.window->activeAgentController()->agent().listMessages(session.toStdString()).empty());
    QVERIFY(f.im->lastText().isEmpty());
}

void DesktopSessionImBindingUiTest::savedBindingReturnsForTheSameAccountAndPeer() {
    Fixture f;
    QTRY_VERIFY(f.app.isConnected());
    f.show();
    QVERIFY(QTest::qWaitForWindowExposed(f.window.get()));
    QVERIFY2(f.window->activeAgentController()->openError().isEmpty(),
             qPrintable(f.window->activeAgentController()->openError()));
    const QString session = f.session();
    QVERIFY(f.bind(session));
    bool saved = false;
    for (const auto& key : QSettings().allKeys())
        if (key.startsWith(QStringLiteral("imSessionBindings/")))
            saved |= QSettings().value(key).toMap().value(QStringLiteral("owned-ui-friend")).toString() == session;
    QVERIFY(saved);
    f.window.reset();
    f.show();
    QVERIFY(QTest::qWaitForWindowExposed(f.window.get()));
    MaiSession restored;
    QVERIFY(f.window->activeAgentController()->agent().getSession(session.toStdString(), restored));
    auto* bridge = f.window->findChild<DesktopSessionImBridge*>();
    QVERIFY(bridge);
    QCOMPARE(bridge->boundSession(QStringLiteral("owned-ui-friend")), session);
    QTRY_VERIFY(f.boundName() && f.boundName()->isVisible());
    QVERIFY(f.boundName()->text().contains(QStringLiteral("Owned UI Session")));
    f.app.selectPeer(QStringLiteral("owned-ui-other"));
    QTRY_VERIFY(!f.boundName()->isVisible());
    f.app.selectPeer(QStringLiteral("owned-ui-friend"));
    QTRY_VERIFY(f.boundName()->isVisible());
    QVERIFY(f.window->activeAgentController()->agent().listMessages(session.toStdString()).empty());
    QVERIFY(f.im->lastText().isEmpty());
}

void DesktopSessionImBindingUiTest::savedBindingDoesNotActivateForAnotherAccount() {
    Fixture f;
    QTRY_VERIFY(f.app.isConnected());
    f.show();
    QVERIFY(QTest::qWaitForWindowExposed(f.window.get()));
    QVERIFY(f.bind(f.session()));
    f.window.reset();
    auto* otherIm = new FakeRemoteIMClient;
    RemoteIMApplication otherApp(QStringLiteral("owned-ui-different-owner"), std::unique_ptr<RemoteIMClient>(otherIm));
    otherApp.addContact(QStringLiteral("owned-ui-friend"), QStringLiteral("Same Named Friend"));
    otherApp.connectToService(123, QStringLiteral("owned-unused-signature"));
    QTRY_VERIFY(otherApp.isConnected());
    MainWindow otherWindow(otherApp);
    otherWindow.show();
    otherApp.selectPeer(QStringLiteral("owned-ui-friend"));
    auto* otherName = otherWindow.findChild<QLabel*>(QStringLiteral("imSessionBindingName"));
    QVERIFY(otherName && !otherName->isVisible());
    QVERIFY(otherIm->lastText().isEmpty());
}

void DesktopSessionImBindingUiTest::anotherPeerCannotReuseTheBoundSession() {
    Fixture f;
    QTRY_VERIFY(f.app.isConnected());
    f.show();
    QVERIFY(QTest::qWaitForWindowExposed(f.window.get()));
    const QString session = f.session();
    QVERIFY(f.bind(session));
    f.app.selectPeer(QStringLiteral("owned-ui-other"));
    bool unavailable = false;
    bool captured = true;
    QTimer::singleShot(0, f.window.get(), [&] {
        auto* picker = f.window->findChild<QDialog*>(QStringLiteral("imSessionBindingDialog"));
        QVERIFY(picker);
        auto* list = picker->findChild<QListWidget*>(QStringLiteral("imSessionBindingSessions"));
        if (!list) { picker->reject(); return; }
        for (int row = 0; row < list->count(); ++row)
            if (list->item(row)->data(Qt::UserRole).toString() == session)
                unavailable = !(list->item(row)->flags() & Qt::ItemIsEnabled);
        captured = captureOwnedWidget(*picker, QStringLiteral("session-binding-unavailable-picker"));
        picker->reject();
    });
    f.entry()->click();
    QVERIFY(unavailable);
    QVERIFY(captured);
    QVERIFY(!f.boundName()->isVisible());
    QVERIFY(f.im->lastText().isEmpty());
}

void DesktopSessionImBindingUiTest::localComposerSendRevokesAutomaticReplyBeforeSubmission() {
    Fixture f;
    QTRY_VERIFY(f.app.isConnected());
    f.show();
    QVERIFY(QTest::qWaitForWindowExposed(f.window.get()));
    const QString session = f.session();
    QVERIFY(f.bind(session));
    auto* panel = f.window->findChild<QWidget*>(QStringLiteral("agentPage"));
    auto* editor = f.window->findChild<QTextEdit*>(QStringLiteral("agentPromptEditor"));
    QVERIFY(panel && editor);
    // Selecting a local Session is ordinary navigation. The no-key model state
    // makes this a UI takeover check, without submitting any model request.
    // Session selection is performed by the ordinary Agent panel API.
    auto* agentPanel = f.window->findChild<AgentChatPanel*>();
    QVERIFY(agentPanel);
    agentPanel->openSession(session);
    editor->setPlainText(QStringLiteral("Owned local draft"));
    QTest::keyClick(editor, Qt::Key_Return);
    QTRY_VERIFY(!f.boundName()->isVisible());
    QVERIFY(f.im->lastText().isEmpty());
}

void DesktopSessionImBindingUiTest::openPickerTracksSessionsTitlesAndDeletionWithoutChangingDraft() {
    Fixture f;
    QTRY_VERIFY(f.app.connectionReady());
    f.show();
    QVERIFY(QTest::qWaitForWindowExposed(f.window.get()));
    const QString first = f.session(QStringLiteral("Owned dynamic first"));
    auto* editor = f.window->findChild<QTextEdit*>(QStringLiteral("messageEditor"));
    QVERIFY(editor);
    editor->setPlainText(QStringLiteral("Owned IM draft stays untouched"));
    QTimer::singleShot(0, f.window.get(), [&] {
        auto* dialog = f.window->findChild<QDialog*>(QStringLiteral("imSessionBindingDialog"));
        QVERIFY(dialog);
        const auto close = qScopeGuard([&] { dialog->reject(); });
        auto* list = dialog->findChild<QListWidget*>(QStringLiteral("imSessionBindingSessions"));
        auto* confirm = dialog->findChild<QPushButton*>(QStringLiteral("imSessionBindingConfirm"));
        QVERIFY(list && confirm);
        const auto find = [&](const QString& id) -> QListWidgetItem* {
            for (int index = 0; index < list->count(); ++index)
                if (list->item(index)->data(Qt::UserRole).toString() == id) return list->item(index);
            return nullptr;
        };
        QVERIFY(find(first));
        list->setCurrentItem(find(first));
        const QString second = f.session(QStringLiteral("Owned dynamic second"));
        QVERIFY(QTest::qWaitFor([&] { return find(second) != nullptr; }, 1000));
        QCOMPARE(list->currentItem()->data(Qt::UserRole).toString(), first);
        QVERIFY(f.window->activeAgentController()->agent().submit(
            MaiUpdateSession{first.toStdString(), "Owned renamed while picker open", "", ""}));
        QVERIFY(QTest::qWaitFor([&] { return find(first) && find(first)->text().contains(QStringLiteral("renamed")); }, 1000));
        QCOMPARE(list->currentItem()->data(Qt::UserRole).toString(), first);
        QVERIFY(f.window->activeAgentController()->agent().submit(MaiDeleteSession{first.toStdString()}));
        QVERIFY(QTest::qWaitFor([&] { return find(first) == nullptr; }, 1000));
        QVERIFY(!confirm->isEnabled());
    });
    f.entry()->click();
    QCOMPARE(editor->toPlainText(), QStringLiteral("Owned IM draft stays untouched"));
    QVERIFY(f.im->lastText().isEmpty());
}

void DesktopSessionImBindingUiTest::peerChangeInvalidatesOpenPicker() {
    Fixture f;
    QTRY_VERIFY(f.app.connectionReady());
    f.show();
    QVERIFY(QTest::qWaitForWindowExposed(f.window.get()));
    f.session();
    auto* editor = f.window->findChild<QTextEdit*>(QStringLiteral("messageEditor"));
    editor->setPlainText(QStringLiteral("Owned peer-specific draft"));
    QTimer::singleShot(0, f.window.get(), [&] {
        auto* dialog = f.window->findChild<QDialog*>(QStringLiteral("imSessionBindingDialog"));
        QVERIFY(dialog);
        const auto close = qScopeGuard([&] { dialog->reject(); });
        f.app.selectPeer(QStringLiteral("owned-ui-other"));
        QVERIFY(QTest::qWaitFor([&] { return !dialog->isVisible(); }, 1000));
    });
    f.entry()->click();
    f.app.selectPeer(QStringLiteral("owned-ui-friend"));
    QCOMPARE(editor->toPlainText(), QStringLiteral("Owned peer-specific draft"));
    QVERIFY(f.im->lastText().isEmpty());
}

QTEST_MAIN(DesktopSessionImBindingUiTest)
#include "DesktopSessionImBindingUiTest.moc"
