// 普通 Agent/Core 工具与持久界面合同；只用内存模型、自有文件和 Fake IM。
#include <QApplication>
#include <QDialog>
#include <QDir>
#include <QFile>
#include <QLabel>
#include <QPainter>
#include <QPdfWriter>
#include <QPushButton>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTimer>
#include <QtTest>

#include <atomic>
#include <memory>

#include "agent/AgentChatPanel.h"
#include "agent/AgentController.h"
#include "app/RemoteIMApplication.h"
#include "im/FakeRemoteIMClient.h"
#include "ui/MainWindow.h"

#if defined(Q_OS_MACOS)
bool closeOwnedAgentFilePdfPreview(const QString& title);
#endif

#if defined(Q_OS_WIN)
// Windows uses the existing modeless PDF dialog, not activeModalWidget().
// Match only this fixture's owner and the real native-PDF presentation controls.
static QDialog* ownedWindowsPdfPreview(QWidget* owner, const QString& title) {
    for (auto* widget : QApplication::topLevelWidgets()) {
        auto* dialog = qobject_cast<QDialog*>(widget);
        if (dialog && dialog->isVisible() && dialog->parentWidget() == owner && dialog->windowTitle() == title &&
            dialog->findChild<QLabel*>(QStringLiteral("pdfPreviewPageLabel")) &&
            dialog->findChild<QLabel*>(QStringLiteral("pdfPreviewPageImage")))
            return dialog;
    }
    return nullptr;
}
#endif

class FileDeliveringModel final : public MaiModelClient {
public:
    MaiError stream(const MaiModelRequest&, const MaiStreamSink& sink, const std::atomic<bool>&) override {
        if (calls_++ == 0 && sink.onToolCall) {
            sink.onToolCall({"owned-pdf", "agent_send_file", R"({"file_path":"report.pdf","caption":"本轮报告"})"});
            sink.onToolCall(
                {"owned-archive", "agent_send_file", R"({"file_path":"archive.zip","caption":"本轮附件"})"});
        } else if (sink.onText) {
            sink.onText("Owned files delivered");
        }
        return {};
    }
    MaiWireApi wireApi() const override {
        return MaiWireApi::ChatCompletions;
    }

private:
    std::atomic<int> calls_{0};
};

class AgentFileDeliveryUiTest : public QObject {
    Q_OBJECT
    void runOwnedCardCompletionRace(bool changeWorkspace, bool changeAfterLaunch = false) {
        QTemporaryDir workspace, replacement;
        QVERIFY(workspace.isValid() && replacement.isValid());
        {
            QPdfWriter pdf(workspace.filePath(QStringLiteral("report.pdf")));
            QPainter painter(&pdf);
            painter.drawText(100, 100, QStringLiteral("Owned old-root file"));
        }
        QFile archive(workspace.filePath(QStringLiteral("archive.zip")));
        QVERIFY(archive.open(QIODevice::WriteOnly));
        QCOMPARE(archive.write("PK-owned-fixture"), qint64(16));
        archive.close();
        RemoteIMApplication app(QStringLiteral("owned-file-race"), std::make_unique<FakeRemoteIMClient>());
        MainWindow main(app);
        AgentController controller(std::make_unique<FileDeliveringModel>(), QString());
        AgentChatPanel panel(controller, &main);
        main.show();
        panel.show();
        const QString session = controller.createSession(workspace.path());
        panel.openSession(session);
        QVERIFY(controller.sendPrompt(session, QStringLiteral("Deliver own files")));
        QTRY_VERIFY_WITH_TIMEOUT(!controller.agent().isBusy(session.toStdString()), 5000);
        QTRY_COMPARE_WITH_TIMEOUT(panel.findChildren<QPushButton*>(QStringLiteral("agentFileOpen")).size(), 2, 5000);
        QPushButton* pdf = nullptr;
        for (auto* button : panel.findChildren<QPushButton*>(QStringLiteral("agentFileOpen"))) {
            QTRY_VERIFY_WITH_TIMEOUT(button->isEnabled(), 5000);
            if (button->accessibleName().contains(QStringLiteral("report.pdf"))) pdf = button;
        }
        QVERIFY(pdf);
        bool previewSeen = false;
        QTimer closePreview;
        closePreview.setInterval(20);
        connect(&closePreview, &QTimer::timeout, &panel, [&] {
#if defined(Q_OS_MACOS)
            previewSeen = closeOwnedAgentFilePdfPreview(QStringLiteral("report.pdf")) || previewSeen;
#elif defined(Q_OS_WIN)
            // A forbidden old-root preview counts immediately, including its loading state.
            if (auto* dialog = ownedWindowsPdfPreview(&main, QStringLiteral("report.pdf"))) {
                previewSeen = true;
                dialog->close();
            }
#else
            auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
            if (dialog && dialog->windowTitle().contains(QStringLiteral("report.pdf"))) {
                previewSeen = true; dialog->reject();
            }
#endif
        });
        closePreview.start();
        if (changeWorkspace) {
            if (!changeAfterLaunch) QVERIFY(controller.setSessionDirectory(session, replacement.path()));
            pdf->click();
            if (changeAfterLaunch) QVERIFY(controller.setSessionDirectory(session, replacement.path()));
            QTRY_VERIFY_WITH_TIMEOUT(!pdf->isEnabled() && pdf->findChild<QLabel*>(QStringLiteral("agentFileDetail"))
                                                              ->text()
                                                              .contains(QStringLiteral("不可用")),
                                     1500);
        } else {
            pdf->click();  // completion 尚在事件队列时切到 B，随后回 A。
            const QString other = controller.createSession(workspace.path());
            panel.openSession(other);
            QTest::qWait(250);
            panel.openSession(session);
            QTRY_VERIFY_WITH_TIMEOUT(pdf->isEnabled(), 1500);
        }
        QVERIFY(!previewSeen);  // 隐藏 A 或已换根的 A 均不得打开旧文件。
    }
private slots:
    void fileCardCompletionSurvivesSessionSwitch() {
        runOwnedCardCompletionRace(false);
    }
    void fileCardRejectsSameSessionWorkspaceChange() {
        runOwnedCardCompletionRace(true);
    }
    void fileCardRejectsWorkspaceChangeBeforeCompletion() {
        runOwnedCardCompletionRace(true, true);
    }
    void currentReplyAndRestoredHistoryUseNormalFileCards() {
        QTemporaryDir workspace;
        QVERIFY(workspace.isValid());
        {
            QPdfWriter pdf(workspace.filePath(QStringLiteral("report.pdf")));
            QPainter painter(&pdf);
            painter.drawText(100, 100, QStringLiteral("Owned current-reply file preview"));
        }
        QFile archive(workspace.filePath(QStringLiteral("archive.zip")));
        QVERIFY(archive.open(QIODevice::WriteOnly));
        QCOMPARE(archive.write("PK-owned-fixture"), qint64(16));
        archive.close();

        RemoteIMApplication app(QStringLiteral("owned-file-ui"), std::make_unique<FakeRemoteIMClient>());
        MainWindow main(app);
        AgentController controller(std::make_unique<FileDeliveringModel>(), QString());
        AgentChatPanel panel(controller, &main);
        main.resize(1100, 760);
        main.show();
        panel.setGeometry(100, 100, 760, 560);
        panel.show();
        panel.raise();
        const QString session = controller.createSession(workspace.path());
        panel.openSession(session);
        QVERIFY(controller.sendPrompt(session, QStringLiteral("交付本轮已有文件")));
        QTRY_VERIFY_WITH_TIMEOUT(!controller.agent().isBusy(session.toStdString()), 5000);
        QTRY_COMPARE_WITH_TIMEOUT(panel.findChildren<QPushButton*>(QStringLiteral("agentFileOpen")).size(), 2, 5000);
        for (auto* button : panel.findChildren<QPushButton*>(QStringLiteral("agentFileOpen"))) {
            QTRY_VERIFY_WITH_TIMEOUT(button->isEnabled(), 5000);
            for (const auto* label : button->findChildren<QLabel*>()) {
                QVERIFY(label->isVisible());
                QVERIFY2(button->rect().contains(label->geometry()), "File metadata is clipped by the card");
            }
        }
        QVERIFY(controller.agent().listPendingPermissions().empty());
        const QString capture = qEnvironmentVariable("MAICHAT_AGENT_FILE_CAPTURE_PATH");
        if (!capture.isEmpty()) QVERIFY(panel.grab().save(capture));

        // 点 PDF 复用主窗口既有预览，不能启动外部程序或调用生成模型。
        bool previewSeen = false;
        QTimer closePreview;
        closePreview.setInterval(25);
        connect(&closePreview, &QTimer::timeout, &panel, [&] {
#if defined(Q_OS_MACOS)
            previewSeen = closeOwnedAgentFilePdfPreview(QStringLiteral("report.pdf")) || previewSeen;
#elif defined(Q_OS_WIN)
            if (auto* dialog = ownedWindowsPdfPreview(&main, QStringLiteral("report.pdf"))) {
                const auto* page = dialog->findChild<QLabel*>(QStringLiteral("pdfPreviewPageLabel"));
                const auto* image = dialog->findChild<QLabel*>(QStringLiteral("pdfPreviewPageImage"));
                if (page->text() == QStringLiteral("1 / 1") && !image->pixmap().isNull()) {
                    qInfo() << "[owned-windows-file-preview]" << dialog->windowTitle()
                            << "modal" << dialog->isModal() << "page" << page->text()
                            << "pixels" << image->pixmap().size();
                    previewSeen = true;
                    dialog->close();
                }
            }
#else
            auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
            if (dialog && dialog->windowTitle().contains(QStringLiteral("report.pdf"))) {
                previewSeen = true;
                dialog->reject();
            }
#endif
        });
        closePreview.start();
        for (auto* button : panel.findChildren<QPushButton*>(QStringLiteral("agentFileOpen")))
            if (button->accessibleName().contains(QStringLiteral("report.pdf"))) button->click();
        QTRY_VERIFY_WITH_TIMEOUT(previewSeen, 5000);
        closePreview.stop();

        const QString other = controller.createSession(workspace.path());
        panel.openSession(other);
        panel.openSession(session);
        QTRY_COMPARE_WITH_TIMEOUT(panel.findChildren<QPushButton*>(QStringLiteral("agentFileOpen")).size(), 2, 5000);
        QVERIFY(QFile::remove(workspace.filePath(QStringLiteral("report.pdf"))));
        panel.openSession(other);
        panel.openSession(session);
        qInfo() << "[owned-file-restore]" << panel.sessionId() << session << "cards"
                << panel.findChildren<QPushButton*>(QStringLiteral("agentFileOpen")).size();
        for (auto* button : panel.findChildren<QPushButton*>(QStringLiteral("agentFileOpen"))) {
            const auto* detail = button->findChild<QLabel*>(QStringLiteral("agentFileDetail"));
            qInfo() << "[owned-file-card]" << button->accessibleName() << button->isEnabled()
                    << (detail ? detail->text() : QStringLiteral("missing-detail"));
            if (button->accessibleName().contains(QStringLiteral("report.pdf"))) button->click();
        }
        QTRY_VERIFY_WITH_TIMEOUT(
            [&] {
                for (auto* button : panel.findChildren<QPushButton*>(QStringLiteral("agentFileOpen")))
                    if (button->accessibleName().contains(QStringLiteral("report.pdf"))) {
                        const auto* detail = button->findChild<QLabel*>(QStringLiteral("agentFileDetail"));
                        return !button->isEnabled() && detail && detail->text().contains(QStringLiteral("不可用"));
                    }
                return false;
            }(),
            5000);
    }
};

QTEST_MAIN(AgentFileDeliveryUiTest)
#include "AgentFileDeliveryUiTest.moc"
