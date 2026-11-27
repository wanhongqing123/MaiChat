// 普通 Agent/Core 工具与持久界面合同；只用内存模型、自有文件和 Fake IM。
#include <QApplication>
#include <QDialog>
#include <QDir>
#include <QEvent>
#include <QFile>
#include <QLabel>
#include <QPainter>
#include <QPdfWriter>
#include <QPointer>
#include <QPushButton>
#include <QSettings>
#include <QSaveFile>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTimer>
#include <QtTest>

#include <atomic>
#include <memory>

#include "agent/AgentChatPanel.h"
#include "agent/AgentController.h"
#include "agent/AgentDocumentThumbnailView.h"
#include "app/RemoteIMApplication.h"
#include "im/FakeRemoteIMClient.h"
#include "ui/MainWindow.h"
#include "markdown/MarkdownView.h"

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
        return MaiWireApi::Responses;
    }

private:
    std::atomic<int> calls_{0};
};

class ThumbnailDeliveringModel final : public MaiModelClient {
public:
    explicit ThumbnailDeliveringModel(bool markdownOnly = false) : markdownOnly_(markdownOnly) {}
    MaiError stream(const MaiModelRequest&, const MaiStreamSink& sink, const std::atomic<bool>&) override {
        if (calls_++ == 0 && sink.onToolCall) {
            if (!markdownOnly_)
                sink.onToolCall({"thumb-pdf", "agent_send_file", R"({"file_path":"report.pdf"})"});
            sink.onToolCall({"thumb-md", "agent_send_file", R"({"file_path":"report.md"})"});
            if (!markdownOnly_)
                sink.onToolCall({"thumb-bad", "agent_send_file", R"({"file_path":"bad.pdf"})"});
        } else if (sink.onText) {
            sink.onText("Owned document thumbnails");
        }
        return {};
    }
    MaiWireApi wireApi() const override {
        return MaiWireApi::Responses;
    }

private:
    bool markdownOnly_ = false;
    std::atomic<int> calls_{0};
};

// Observe actual card paints, including the first frame before decode completes.
// Only the owned thumbnail's scheduling timer is paused; layout and native paint
// events keep running, and releaseDecoding resumes the unchanged real renderer.
class DocumentCardPaintProbe final : public QObject {
public:
    explicit DocumentCardPaintProbe(QWidget* panel) : panel_(panel) {
        qApp->installEventFilter(this);
    }
    ~DocumentCardPaintProbe() override {
        qApp->removeEventFilter(this);
    }
    void setPhase(const QString& phase) {
        phase_ = phase;
    }
    void pauseDecoding() {
        pauseDecoding_ = true;
    }
    void releaseDecoding() {
        pauseDecoding_ = false;
        const auto timers = pausedTimers_;
        pausedTimers_.clear();
        for (const auto& timer : timers)
            if (timer) timer->start();
    }
    int loadingPaints(const QString& phase) const {
        return loadingPaints_.value(phase);
    }
    const QStringList& failures() const {
        return failures_;
    }

protected:
    bool eventFilter(QObject* watched, QEvent* event) override {
        if (pauseDecoding_ && event->type() == QEvent::Timer) {
            auto* timer = qobject_cast<QTimer*>(watched);
            auto* view = timer ? qobject_cast<QWidget*>(timer->parent()) : nullptr;
            if (view && panel_ && panel_->isAncestorOf(view) &&
                view->objectName() == QStringLiteral("agentDocumentThumbnail")) {
                timer->stop();
                pausedTimers_.push_back(timer);
                return true;
            }
        }
        if (event->type() != QEvent::Paint || !panel_) return false;
        auto* widget = qobject_cast<QWidget*>(watched);
        if (!widget || !panel_->isAncestorOf(widget)) return false;
        auto* card = widget;
        while (card && card != panel_ && card->objectName() != QStringLiteral("agentFileCard"))
            card = card->parentWidget();
        if (!card || card == panel_) return false;
        auto* button = card->findChild<QPushButton*>(QStringLiteral("agentFileOpen"));
        auto* view = card->findChild<QWidget*>(QStringLiteral("agentDocumentThumbnail"));
        if (!button || !view || !button->isEnabled()) return false;
        if (view->isVisible() && view->property("agentThumbnailState").toString() == QStringLiteral("loading"))
            ++loadingPaints_[phase_];
        QStringList clipped;
        for (const auto* label : button->findChildren<QLabel*>()) {
            if (label->objectName() != QStringLiteral("agentFileName") &&
                label->objectName() != QStringLiteral("agentFileDetail"))
                continue;
            const QRect bounds(label->mapTo(button, QPoint()), label->size());
            if (!button->rect().contains(bounds) || label->height() < label->fontMetrics().height())
                clipped << QStringLiteral("%1=%2x%3").arg(label->objectName()).arg(label->width()).arg(label->height());
        }
        if (view->isVisible()) {
            const QRect bounds(view->mapTo(button, QPoint()), view->size());
            if (!button->rect().contains(bounds) || view->height() < view->heightForWidth(view->width()))
                clipped << QStringLiteral("thumbnail=%1x%2").arg(view->width()).arg(view->height());
        }
        if (!clipped.isEmpty() && failures_.size() < 24)
            failures_ << QStringLiteral("%1 paint: %2").arg(phase_, clipped.join(QStringLiteral(", ")));
        return false;
    }

private:
    QPointer<QWidget> panel_;
    QString phase_ = QStringLiteral("initial loading");
    bool pauseDecoding_ = true;
    QVector<QPointer<QTimer>> pausedTimers_;
    QMap<QString, int> loadingPaints_;
    QStringList failures_;
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
    void documentCardLoadingPaintAndFailedRecoveryKeepTheirGeometry() {
        QTemporaryDir workspace;
        QVERIFY(workspace.isValid());
        QFile markdown(workspace.filePath(QStringLiteral("report.md")));
        QVERIFY(markdown.open(QIODevice::WriteOnly));
        QVERIFY(markdown.write("# Owned loading document\n\nFirst screen remains readable.\n") > 0);
        markdown.close();
        QFile corrupt(workspace.filePath(QStringLiteral("corrupt.md")));
        QVERIFY(corrupt.open(QIODevice::WriteOnly));
        QCOMPARE(corrupt.write(QByteArray::fromHex("c080eda080")), qint64(5));
        corrupt.close();
        RemoteIMApplication app(QStringLiteral("owned-document-loading"), std::make_unique<FakeRemoteIMClient>());
        MainWindow main(app);
        AgentController controller(std::make_unique<ThumbnailDeliveringModel>(true), QString());
        AgentChatPanel panel(controller, &main);
        main.resize(1100, 760);
        main.show();
        panel.setGeometry(100, 60, 760, 650);
        panel.show();
        DocumentCardPaintProbe paints(&panel);
        const QString session = controller.createSession(workspace.path());
        panel.openSession(session);
        QVERIFY(controller.sendPrompt(session, QStringLiteral("Deliver the owned loading document")));
        QTRY_VERIFY_WITH_TIMEOUT(!controller.agent().isBusy(session.toStdString()), 5000);
        QTRY_COMPARE_WITH_TIMEOUT(panel.findChildren<QPushButton*>(QStringLiteral("agentFileOpen")).size(), 1, 5000);
        auto* button = panel.findChild<QPushButton*>(QStringLiteral("agentFileOpen"));
        auto* view = dynamic_cast<AgentDocumentThumbnailView*>(
            button->findChild<QWidget*>(QStringLiteral("agentDocumentThumbnail")));
        QVERIFY(view);
        QTRY_VERIFY_WITH_TIMEOUT(button->isEnabled() && view->isVisible(), 5000);
        QCOMPARE(view->property("agentThumbnailState").toString(), QStringLiteral("loading"));
        button->repaint();
        QTRY_VERIFY_WITH_TIMEOUT(paints.loadingPaints(QStringLiteral("initial loading")) > 0, 5000);
        paints.releaseDecoding();
        QTRY_VERIFY_WITH_TIMEOUT(!view->thumbnail().isNull(), 10000);
        QTRY_VERIFY_WITH_TIMEOUT(button->rect().contains(QRect(view->mapTo(button, QPoint()), view->size())) &&
                                     view->height() >= view->heightForWidth(view->width()),
                                 5000);
        const int expandedHeight = button->height();

        // A real decoder failure must collapse to the original clickable card.
        // Rebinding that same production card exercises recovery from hidden HFW.
        paints.setPhase(QStringLiteral("decode failure"));
        AgentDocumentThumbnailRequest bad;
        bad.path = corrupt.fileName();
        bad.allowedRoot = workspace.path();
        bad.mimeType = QStringLiteral("text/markdown");
        view->setDocument(bad);
        QTRY_COMPARE_WITH_TIMEOUT(view->property("agentThumbnailState").toString(), QStringLiteral("unavailable"), 10000);
        QVERIFY(view->isHidden() && view->thumbnail().isNull());
        QVERIFY(button->isEnabled());
        QTRY_VERIFY_WITH_TIMEOUT(button->height() < expandedHeight, 5000);
        button->repaint();
        paints.setPhase(QStringLiteral("retry loading"));
        paints.pauseDecoding();
        auto recovered = bad;
        recovered.path = markdown.fileName();
        view->setDocument(recovered);
        button->repaint();
        QTRY_VERIFY_WITH_TIMEOUT(paints.loadingPaints(QStringLiteral("retry loading")) > 0, 5000);
        paints.releaseDecoding();
        QTRY_VERIFY_WITH_TIMEOUT(!view->thumbnail().isNull(), 10000);
        QCOMPARE(view->sourcePath(), markdown.fileName());
        QVERIFY(button->isEnabled());
        QVERIFY(controller.agent().listPendingPermissions().empty());
        qInfo() << "[owned-document-loading-paints] initial"
                << paints.loadingPaints(QStringLiteral("initial loading")) << "retry"
                << paints.loadingPaints(QStringLiteral("retry loading")) << "clipped" << paints.failures();
        QVERIFY2(paints.failures().isEmpty(), qPrintable(paints.failures().join(QLatin1Char('\n'))));
    }
    void documentCardsShowContentAndClickTheFullOriginalDocument() {
        QTemporaryDir workspace;
        QVERIFY(workspace.isValid());
        {
            QPdfWriter pdf(workspace.filePath(QStringLiteral("report.pdf")));
            pdf.setResolution(72);
            QPainter painter(&pdf);
            painter.fillRect(QRect(30, 30, 400, 200), Qt::red);
        }
        const QByteArray original = QByteArray("# Original document\n\nOwned first screen.\n\n") +
                                    QByteArray("Long body paragraph for the complete file preview.\n\n").repeated(100) +
                                    "FULL PREVIEW TAIL 926\n";
        QFile markdown(workspace.filePath(QStringLiteral("report.md")));
        QVERIFY(markdown.open(QIODevice::WriteOnly));
        QCOMPARE(markdown.write(original), qint64(original.size()));
        markdown.close();
        QFile bad(workspace.filePath(QStringLiteral("bad.pdf")));
        QVERIFY(bad.open(QIODevice::WriteOnly));
        QVERIFY(bad.write("%PDF-1.7\nowned malformed fixture") > 0);
        bad.close();
        RemoteIMApplication app(QStringLiteral("owned-document-thumbnails"), std::make_unique<FakeRemoteIMClient>());
        MainWindow main(app);
        AgentController controller(std::make_unique<ThumbnailDeliveringModel>(), QString());
        AgentChatPanel panel(controller, &main);
        main.resize(1100, 900);
        main.show();
        panel.setGeometry(100, 60, 760, 800);
        panel.show();
        const QString session = controller.createSession(workspace.path());
        panel.openSession(session);
        QVERIFY(controller.sendPrompt(session, QStringLiteral("Deliver owned documents")));
        QTRY_VERIFY_WITH_TIMEOUT(!controller.agent().isBusy(session.toStdString()), 5000);
        QTRY_COMPARE_WITH_TIMEOUT(panel.findChildren<QPushButton*>(QStringLiteral("agentFileOpen")).size(), 3, 5000);
        QPushButton* markdownOpen = nullptr;
        AgentDocumentThumbnailView* markdownView = nullptr;
        for (auto* button : panel.findChildren<QPushButton*>(QStringLiteral("agentFileOpen"))) {
            QTRY_VERIFY_WITH_TIMEOUT(button->isEnabled(), 5000);
            auto* view = dynamic_cast<AgentDocumentThumbnailView*>(
                button->findChild<QWidget*>(QStringLiteral("agentDocumentThumbnail")));
            QVERIFY(view);
            if (button->accessibleName().contains(QStringLiteral("bad.pdf"))) {
                QTRY_COMPARE_WITH_TIMEOUT(view->property("agentThumbnailState").toString(),
                                          QStringLiteral("unavailable"), 10000);
                QVERIFY(view->isHidden() && view->thumbnail().isNull());
                QVERIFY(button->isEnabled());  // Decode failure retains the ordinary file action.
            } else {
                QTRY_VERIFY_WITH_TIMEOUT(!view->thumbnail().isNull(), 10000);
                QVERIFY(button->rect().contains(view->geometry()));
                if (button->accessibleName().contains(QStringLiteral("report.md"))) {
                    markdownOpen = button;
                    markdownView = view;
                } else {
                    const QColor sample =
                        view->thumbnail().pixelColor(view->thumbnail().width() / 3, view->thumbnail().height() / 3);
                    QVERIFY(sample.red() > 180 && sample.green() < 80 && sample.blue() < 80);
                }
            }
        }
        QVERIFY(markdownOpen && markdownView);
        QCOMPARE(markdownView->sourcePath(), markdown.fileName());
        const QImage first = markdownView->thumbnail();
        QByteArray revised = original;
        revised.replace("Original", "Revised!");  // Keep artifact byte metadata valid for the original click contract.
        QSaveFile replacement(markdown.fileName());
        QVERIFY(replacement.open(QIODevice::WriteOnly));
        QCOMPARE(replacement.write(revised), qint64(revised.size()));
        QVERIFY(replacement.commit());
        QTRY_VERIFY_WITH_TIMEOUT(markdownView->thumbnail() != first && !markdownView->thumbnail().isNull(), 10000);
        bool fullPreviewSeen = false;
        QTimer closePreview;
        connect(&closePreview, &QTimer::timeout, &panel, [&] {
            for (auto* top : QApplication::topLevelWidgets()) {
                auto* dialog = qobject_cast<QDialog*>(top);
                if (!dialog || dialog->parentWidget() != &main || !dialog->isVisible()) continue;
                auto* document = dialog->findChild<MarkdownView*>(QStringLiteral("filePreviewMarkdownView"));
                if (!document) continue;
                document->selectAll();
                const QString full = document->selectedText();
                fullPreviewSeen = full.contains(QStringLiteral("Revised! document")) &&
                                  full.contains(QStringLiteral("FULL PREVIEW TAIL 926"));
                dialog->reject();
            }
        });
        closePreview.start(20);
        QTest::mouseClick(markdownOpen, Qt::LeftButton, Qt::NoModifier,
                         markdownView->mapTo(markdownOpen, markdownView->rect().center()));
        QTRY_VERIFY_WITH_TIMEOUT(fullPreviewSeen, 5000);
        QVERIFY(controller.agent().listPendingPermissions().empty());
        const QString capture = qEnvironmentVariable("MAICHAT_AGENT_DOCUMENT_CAPTURE_PATH");
        if (!capture.isEmpty()) QVERIFY(panel.grab().save(capture));
    }
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
