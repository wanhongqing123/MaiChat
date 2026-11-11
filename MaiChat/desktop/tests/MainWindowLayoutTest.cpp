// 文件作用：验证桌面端主窗口消息、视频封面和状态提示的布局行为。
// 运行边界：由测试进程驱动实际组件或替身，不作为正式 App 的用户入口。
#include <httplib.h>

#include <QAbstractButton>
#include <QBuffer>
#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QDateTime>
#include <QCryptographicHash>
#include <QDialog>
#include <QDialogButtonBox>
#include <QLabel>
#include <QListWidget>
#include <QScrollBar>
#include <QScopeGuard>
#include <QStyle>
#include <QStyleOptionSlider>
#include <QLayout>
#include <QLineEdit>
#include <QMessageBox>
#include <QMenu>
#include <QPushButton>
#include <QProcess>
#include <QProcessEnvironment>
#include <QScrollArea>
#include <QScreen>
#include <QSettings>
#include <QSystemTrayIcon>
#include <QStandardPaths>
#include <QSignalSpy>
#include <QSplitter>
#include <QSplitterHandle>
#include <QMouseEvent>
#include <QStringList>
#include <QStackedWidget>
#include <QDir>
#include <QDropEvent>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QFontMetrics>
#include <QFontDatabase>
#include <QImage>
#include <QInputMethodEvent>
#include <QMimeData>
#include <QTemporaryDir>
#include <QTest>
#include "agent/AgentChatPanel.h"
#include "agent/AgentController.h"
#include "agent/AgentSessionList.h"
#include "agent/DesktopSessionSyncCoordinator.h"
#include "agent/DesktopScreenCaptureControl.h"
#include "agent/RemoteAgentPanel.h"
#include "MaiSqliteStore.h"
#include "markdown/MarkdownView.h"
#include <QTextBlock>
#include <QTextList>
#include <QTextFragment>
#include <QAbstractTextDocumentLayout>
#include <QTextEdit>
#include <QTimer>
#include <QUrl>
#include <QUuid>
#include <QVBoxLayout>
#include <QVector>
#include <QWidget>
#include <memory>
#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>

#ifdef Q_OS_WIN
#include <qt_windows.h>
#include <dwmapi.h>
#endif

#include "agent/ReplySuggestionController.h"
#include "app/RemoteIMApplication.h"
#include "im/FakeRemoteIMClient.h"
#include "markdown/MarkdownDocument.h"
#include "markdown/MarkdownLabel.h"
#include "ui/ImagePreviewDialog.h"
#include "ui/AppMessageDialog.h"
#include "model/MessageQuote.h"
#include "storage/LocalMessageDatabase.h"
#include "ui/MainWindow.h"
#include "ui/UiZoom.h"

namespace {

// Isolate both preference identity and backing files; no normal account settings
// or database is opened by the notification fixtures.
class ScopedNotificationSettings {
public:
    ScopedNotificationSettings()
        : organization_(QCoreApplication::organizationName()),
          application_(QCoreApplication::applicationName()), format_(QSettings::defaultFormat()) {
        QCoreApplication::setOrganizationName(QStringLiteral("MaiChatDndTests"));
        QCoreApplication::setApplicationName(QUuid::createUuid().toString(QUuid::WithoutBraces));
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, directory_.path());
    }
    ~ScopedNotificationSettings() {
        QSettings::setDefaultFormat(format_);
        QCoreApplication::setOrganizationName(organization_);
        QCoreApplication::setApplicationName(application_);
    }
    QString directory() const { return directory_.path(); }
private:
    QString organization_, application_;
    QSettings::Format format_;
    QTemporaryDir directory_;
};

// Observe the real host's existing notification-submit point; transport and
// reception still use Fake IM and the ordinary application/store pipeline.
class ScopedNotificationSubmissions {
public:
    ScopedNotificationSubmissions() {
        current_ = this;
        previous_ = qInstallMessageHandler(&capture);
    }
    ~ScopedNotificationSubmissions() {
        qInstallMessageHandler(previous_);
        current_ = nullptr;
    }
    int count = 0;
private:
    static void capture(QtMsgType type, const QMessageLogContext& context, const QString& text) {
        if (current_ && text.startsWith(QStringLiteral("[notify] requested peer="))) ++current_->count;
        if (current_ && current_->previous_) current_->previous_(type, context, text);
    }
    static ScopedNotificationSubmissions* current_;
    QtMessageHandler previous_ = nullptr;
};
ScopedNotificationSubmissions* ScopedNotificationSubmissions::current_ = nullptr;

// 头像仍经真实 loopback HTTP 下载；服务持有 PNG 和计数器，关闭后等待全部 handler 结束。
class AvatarHttpServer {
public:
    ~AvatarHttpServer() {
        close();
    }

    bool listen() {
        server_.Get(".*", [this](const httplib::Request&, httplib::Response& response) {
            std::string bytes;
            {
                std::lock_guard<std::mutex> lock(pngMutex_);
                bytes = png_;
            }
            ++requests;
            response.set_content(bytes, "image/png");
            response.set_header("Connection", "close");
        });
        server_.set_read_timeout(2);
        server_.set_write_timeout(2);
        port_ = server_.bind_to_any_port("127.0.0.1");
        if (port_ <= 0) return false;
        worker_ = std::thread([this] {
            server_.listen_after_bind();
            listenerExited_ = true;
        });
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (!server_.is_running() && !listenerExited_.load() && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        if (!server_.is_running() && !listenerExited_.load()) qFatal("Cannot start avatar HTTP fixture");
        return server_.is_running();
    }

    void servePng(const QByteArray& png) {
        std::lock_guard<std::mutex> lock(pngMutex_);
        png_ = png.toStdString();
    }

    int serverPort() const {
        return port_;
    }

    void close() {
        server_.stop();
        if (worker_.joinable()) worker_.join();
    }

    std::atomic<int> requests{0};

private:
    httplib::Server server_;
    std::thread worker_;
    std::atomic<bool> listenerExited_{false};
    std::mutex pngMutex_;
    std::string png_;
    int port_ = 0;
};

// 消息及窗口标题栏可视回归使用正常 App 随包字体；结束后恢复，避免影响其他套件。
class ScopedMessageHeaderFont {
public:
    ScopedMessageHeaderFont() : previous_(QApplication::font()) {
        const QDir fonts(QFINDTESTDATA("../resources/fonts"));
        for (const QString& file :
             fonts.entryList({QStringLiteral("Inter-*.ttf"), QStringLiteral("NotoSansSC-*.otf")}, QDir::Files)) {
            const int id = QFontDatabase::addApplicationFont(fonts.filePath(file));
            if (id >= 0) fontIds_.append(id);
        }
        QFont font;
        font.setFamilies({QStringLiteral("Inter"), QStringLiteral("Noto Sans SC"), QStringLiteral("Microsoft YaHei UI"),
                          QStringLiteral("Microsoft YaHei"), QStringLiteral("PingFang SC")});
        font.setHintingPreference(QFont::PreferNoHinting);
        font.setPixelSize(13);
        QApplication::setFont(font);
    }
    ~ScopedMessageHeaderFont() {
        QApplication::setFont(previous_);
        for (int id : fontIds_) QFontDatabase::removeApplicationFont(id);
    }

private:
    QFont previous_;
    QList<int> fontIds_;
};

// 确认弹窗是模态的，exec() 会挡住测试线程；用定时器在其嵌套事件循环里点掉。
// 删除类确认的主按钮是 appMessageDanger（红色），普通确认是 appMessageConfirm。
void confirmNextContactDeletion() {
    QTimer::singleShot(100, [] {
        for (QWidget* widget : QApplication::topLevelWidgets()) {
            auto* dialog = qobject_cast<AppMessageDialog*>(widget);
            if (dialog == nullptr || !dialog->isVisible()) continue;
            auto* confirm = dialog->findChild<QPushButton*>(QStringLiteral("appMessageDanger"));
            if (confirm == nullptr) {
                confirm = dialog->findChild<QPushButton*>(QStringLiteral("appMessageConfirm"));
            }
            if (confirm != nullptr) confirm->click();
            return;
        }
    });
}

}  // namespace

class MainWindowLayoutTest : public QObject {
    Q_OBJECT

private slots:
    void agentSessionSwitchReusesTheExistingPage();
    void agentCaptureStopEntryLivesAboveBothConversationPanes();
    void agentCommonCaptureStopSurvivesSessionAndOwnerPageChanges();
    void groupMenuCreatesAndOpensTextConversation();
    void transientActivityFollowsOnlyActivityEventsAcrossConversationSwitches();
    void transientActivityStaysAfterNewOutgoingMessage();
    void activityDurationUsesMinutesAfterSixtySeconds();
    void videoCoverRetainsPhysicalPixels();
    void renamedHeaderButtonsKeepTheirCompactAppearance();
    void messageTextUsesNativeResolutionAndRegularBodyFont();
    void outgoingTextBubbleDoesNotReserveUnusedLineWidth_data();
    void outgoingTextBubbleDoesNotReserveUnusedLineWidth();
    void exposesDesktopChatLayoutControls();
    void mainWindowUsesRoomierIconFreeTitleArea();
    void windowsCaptionStaysInTitleBarAbovePageActions();
    void windowsCaptionStaysVisibleWhileSettingsScrolls();
    void windowsCaptionPreservesNativeWindowActions();
    void windowsWindowBoundaryDoesNotConsumeLayoutOrInput();
    void windowsWindowShadowSeparatesTheOwnedBackdrop();
    void conversationSidebarUsesLighterBackground();
    void conversationPreviewUsesPlainMarkdown_data();
    void conversationPreviewUsesPlainMarkdown();
    void composerUsesEmbeddedIconSendAction();
    void aiReplySuggestionOnlyFillsComposer();
    void exposesResizableSplitters();
    void agentConversationListUsesResizableSplitter();
    void localSessionCreationLivesInTheDeviceHeader_data();
    void localSessionCreationLivesInTheDeviceHeader();
    void agentAccountsUseDistinctDefaultStorage();
    void agentSessionDeletionUsesChineseProductDialog();
    void remoteSessionLifecycleUsesProductDialogsAndOwnerRoute();
    void compactSplittersKeepDragTargets_data();
    void compactSplittersKeepDragTargets();
    void composerResizeHandleAlignsWithEditorBorder_data();
    void composerResizeHandleAlignsWithEditorBorder();
    void composerResizeHandleBlendsIntoCanvas();
    void composerResizeCursorClearsAfterPointerLeaves();
    void composerResizeKeepsLatestMessageVisible();
    void rendersEmptyConversationState();
    void sendsTextFromComposer();
    void returnKeySendsComposerText();
    void commandOrControlReturnInsertsNewlineInComposer();
    void sendsMultilineTextWithoutFlatteningReturns();
    void rendersSentMessageFromTopWithMetadata();
    void rendersRelativeMessageDates();
    void contactsNavigationShowsProfileBeforeChat();
    void composerDraftAndQuoteStayWithTheirContact();
    void attachmentDraftStaysWithItsContact();
    void sentImageDoesNotReturnAsDraftDuringConversationChange();
    void contactsCurrentSelectionDoesNotLeaveContactsPage();
    void settingsNavigationShowsAccountWithoutDiagnostics();
    void settingsAvatarUpdatesWhenSelfProfileArrives();
    void settingsAvatarReadsTinyWebpFromMemory();
    void settingsAvatarSurvivesNetworkLossAfterRestart();
    void settingsAvatarLoadsRealProfileUrl();
    void settingsLogoutRequiresConfirmationAndCallsHandler();
    void agentSettingsUseInlineActionsWithoutModelDialog();
    void agentReasoningEffortKeepsConversation();
    void agentReasoningEffortBeforeFirstSessionKeepsDeepSeek();
    void agentAccountKeysUpdateModelWithoutSavingSecrets();
    void agentDeepSeekKeyAloneConfiguresMainModel();
    void agentModelFailureRefreshesWithoutDialog();
    void delayedModelCredentialsApplyOnlyToTheRequestedSession();
    void leftNavigationRailIsResizableAndWider();
    void navigationIconsSitNearTopEdge();
    void cloudDriveNavigationUsesMainPageInRequestedOrder();
    void removesRedundantChromeLabels();
    void conversationListsUseDelegateItemsForSmoothScrolling();
    void selectedConversationRowReachesTheListEdge();
    void selectedConversationRowReachesTheListEdgeWithScrollBar();
    void listScrollBarsAreNarrowAndStillScrollableAcrossZoomLevels();
    void contactDirectoryStaysFlatUntilAGroupExists();
    void contactDirectoryShowsGroupsWithUngroupedLast();
    void clickingGroupHeaderCollapsesItsMembers();
    void rendersMarkdownMessageContent();
    void notificationClickPreservesWindowState_data();
    void notificationClickPreservesWindowState();
    void doNotDisturbReadsLatestPreferenceAndRetainsIncomingMessages();
    void doNotDisturbSwitchPersistsAndClearsNotificationDeliveryState();
    void markdownBubbleHasCompactBottomSpacing_data();
    void markdownBubbleHasCompactBottomSpacing();
    void rendersApprovalButtonsAndSendsStructuredDecision();
    void restoresSubmittedApprovalStateFromDatabase();
    void authoritativeApprovalResolutionOverridesLateSentDecision();
    void copiesOriginalMarkdownFromMessageContextMenu();
    void addContactButtonRemainsVisibleWithoutSearch();
    void conversationHeaderNamesActions();
    void conversationAddMenuKeepsActionsAndAnchorsToButton();
    void selfAvatarOpensProfileSettings();
    void navigationTextIsLeftAlignedAndContactsDoNotShowMessagePreview();
    void sectionTitleFollowsSelectedNavigation();
    void visibleContactsNavigationSwitchesMiddlePane();
    void navigationSelectionFollowsContentStackCurrentPage();
    void contactsDirectoryUsesSingleLineRows();
    void wideChatUsesWiderMessageBubbles();
    void receivedMessagesKeepTheSameLeftEdge_data();
    void receivedMessagesKeepTheSameLeftEdge();
    void restoredLongMessagesExpandAfterWindowIsShown();
    void slashCommandMenuSendsWithoutChangingDraft();
    void typingSlashDoesNotOpenCommandMenu();
    void deleteKeyClearsMessagesButKeepsContactInConversationList();
    void deleteKeyRemovesContactAndMessagesFromContactsList();
    void navigationIconsDoNotUsePrivateFontGlyphProperties();
    void conversationListShowsUnreadBadgeAndClearsOnOpen();
    void everyNavButtonIsStyledConsistently();
    void accountAvatarFollowsIMProfile();
    void connectionStatusLivesInSelfProfile();
    void fileBubbleOffersContextMenu();
    void imageBubbleOffersContextMenu();
    void imageBubbleUsesNativePixmapPreview();
    void maximizedImageBubbleOpensOnlyOnePreview();
    void imagePreviewKeepsConversationBackgroundStable_data();
    void imagePreviewKeepsConversationBackgroundStable();
    void copyAttachmentToPathCopiesOverwritesAndReportsErrors();
    void ctrlShortcutsZoomWholeUi();
    void settingsPanelBorderIsNotCoveredByRows();
    void droppingFilesIntoComposerAttachesThemInsteadOfPastingPaths();
    void quotedReplyRendersQuoteBlockAboveBody();
    void clickingQuoteBlockJumpsToTheOriginalMessage();
    void clickingQuoteBlockSaysSoWhenTheOriginalIsNotLocal();
    void replyBarShowsTargetAndClearsAfterSending();
    void conversationListPutsNewestMessageFirst();
    void conversationListBreaksTimeTiesByName();
    void droppingAnImageFileSendsTheOriginalFileAsAnImage();
};

void MainWindowLayoutTest::groupMenuCreatesAndOpensTextConversation() {
    auto client = std::make_unique<FakeRemoteIMClient>();
    auto* fake = client.get();
    RemoteIMApplication app(QStringLiteral("owner"), std::move(client));
    app.connectToService(123456, QStringLiteral("sig"));
    app.addContact(QStringLiteral("friend-a"), QStringLiteral("Friend A"));
    MainWindow window(app);
    window.show();
    QTRY_VERIFY(window.isVisible());
    auto* add = window.findChild<QPushButton*>(QStringLiteral("addConversationButton"));
    QVERIFY(add);
    add->click();
    QMenu* menu = nullptr;
    QTRY_VERIFY_WITH_TIMEOUT((menu = add->findChild<QMenu*>()) != nullptr && menu->isVisible(), 1000);
    QAction* createAction = nullptr;
    for (QAction* action : menu->actions())
        if (action->text() == QStringLiteral("发起群聊")) createAction = action;
    QVERIFY(createAction);
    QTimer::singleShot(80, [] {
        for (QWidget* widget : QApplication::topLevelWidgets()) {
            auto* dialog = qobject_cast<QDialog*>(widget);
            if (!dialog || dialog->objectName() != QStringLiteral("createGroupDialog")) continue;
            auto* name = dialog->findChild<QLineEdit*>(QStringLiteral("groupNameInput"));
            auto* members = dialog->findChild<QListWidget*>(QStringLiteral("groupMemberPicker"));
            auto* create = dialog->findChild<QPushButton*>(QStringLiteral("groupCreateButton"));
            if (!name || !members || members->count() != 1 || !create) return;
            name->setText(QStringLiteral("Project chat"));
            members->item(0)->setCheckState(Qt::Checked);
            create->click();
            return;
        }
    });
    menu->hide();
    createAction->trigger();
    QTRY_COMPARE(app.groups().size(), 1);
    QTRY_VERIFY(window.findChild<QWidget*>(QStringLiteral("groupConversationPane"))->isVisible());
    QCOMPARE(window.findChild<QLabel*>(QStringLiteral("groupChatTitle"))->text(), QStringLiteral("Project chat"));
    auto* editor = window.findChild<QTextEdit*>(QStringLiteral("groupMessageEditor"));
    auto* send = window.findChild<QPushButton*>(QStringLiteral("groupSendButton"));
    QVERIFY(editor && send);
    editor->setPlainText(QStringLiteral("hello group"));
    send->click();
    QTRY_COMPARE(fake->lastGroupText(), QStringLiteral("hello group"));
    QCOMPARE(fake->lastGroupId(), app.groups().first().groupId);
    fake->emitIncomingGroupText(app.groups().first().groupId, QStringLiteral("friend-a"),
                                QStringLiteral("reply from friend"), QStringLiteral("group-reply#0"));
    QTRY_VERIFY(window.findChildren<QLabel*>(QStringLiteral("groupMessageText")).size() >= 2);
    QTest::qWait(100);
    int visibleMessages = 0;
    for (auto* label : window.findChildren<QLabel*>(QStringLiteral("groupMessageText")))
        if (label->isVisible()) ++visibleMessages;
    QCOMPARE(visibleMessages, 2);
    const QString screenshot = qEnvironmentVariable("MAICHAT_GROUP_SCREENSHOT");
    if (!screenshot.isEmpty()) QVERIFY(window.grab().save(screenshot));
    auto* conversations = window.findChild<QListWidget*>(QStringLiteral("conversationList"));
    QVERIFY(conversations && conversations->count() == 2);
    conversations->setCurrentRow(1);
    QTRY_VERIFY(!window.findChild<QWidget*>(QStringLiteral("groupConversationPane"))->isVisible());
    QVERIFY(app.chatState().messagesWith(QStringLiteral("friend-a")).isEmpty());
}

void MainWindowLayoutTest::transientActivityFollowsOnlyActivityEventsAcrossConversationSwitches() {
    auto client = std::make_unique<FakeRemoteIMClient>();
    auto* fake = client.get();
    RemoteIMApplication app(QStringLiteral("owner"), std::move(client));
    app.addContact(QStringLiteral("peer"), QStringLiteral("Peer"));
    app.addContact(QStringLiteral("other"), QStringLiteral("Other"));
    app.selectPeer(QStringLiteral("peer"));
    MainWindow window(app);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    emit fake->activityReceived(
        QStringLiteral("peer"),
        RemoteIMActivitySignal{QStringLiteral("machine:test"), RemoteIMActivityKind::MachineTool, true, 12000, 1});
    QTRY_VERIFY(window.findChild<QWidget*>(QStringLiteral("remoteImActivityRow")) != nullptr);
    QVERIFY(app.chatState().messagesWith(QStringLiteral("peer")).isEmpty());
    app.selectPeer(QStringLiteral("other"));
    QTRY_VERIFY(window.findChild<QWidget*>(QStringLiteral("remoteImActivityRow")) == nullptr);
    app.selectPeer(QStringLiteral("peer"));
    QTRY_VERIFY(window.findChild<QWidget*>(QStringLiteral("remoteImActivityRow")) != nullptr);
    emit fake->incomingText(QStringLiteral("peer"), QStringLiteral("真正的回复"));
    QTRY_VERIFY(window.findChild<QWidget*>(QStringLiteral("remoteImActivityRow")) != nullptr);
    QCOMPARE(app.chatState().messagesWith(QStringLiteral("peer")).size(), 1);
    emit fake->activityReceived(
        QStringLiteral("peer"),
        RemoteIMActivitySignal{QStringLiteral("machine:test"), RemoteIMActivityKind::MachineTool, false, 1000, 2});
    QTRY_VERIFY(window.findChild<QWidget*>(QStringLiteral("remoteImActivityRow")) == nullptr);
}

void MainWindowLayoutTest::activityDurationUsesMinutesAfterSixtySeconds() {
    auto client = std::make_unique<FakeRemoteIMClient>();
    auto* fake = client.get();
    RemoteIMApplication app(QStringLiteral("owner"), std::move(client));
    app.addContact(QStringLiteral("peer"), QStringLiteral("Peer"));
    app.selectPeer(QStringLiteral("peer"));
    MainWindow window(app);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));

    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    emit fake->activityReceived(
        QStringLiteral("peer"),
        RemoteIMActivitySignal{QStringLiteral("machine:duration"), RemoteIMActivityKind::MachineWorking, true, 12000, 1,
                               now - 62 * 1000, now - 417 * 1000});
    auto* bubble = window.findChild<QWidget*>(QStringLiteral("remoteImActivityBubble"));
    QTRY_VERIFY(bubble != nullptr);
    QTRY_COMPARE(bubble->accessibleName(), QStringLiteral("正在执行1分2秒，任务总耗时6分57秒"));
}

void MainWindowLayoutTest::transientActivityStaysAfterNewOutgoingMessage() {
    auto client = std::make_unique<FakeRemoteIMClient>();
    auto* fake = client.get();
    RemoteIMApplication app(QStringLiteral("owner"), std::move(client));
    app.addContact(QStringLiteral("peer"), QStringLiteral("Peer"));
    // 超过默认渲染窗口，复现完整历史下标大于当前布局数量的长会话。
    const qint64 historyStart = QDateTime::currentMSecsSinceEpoch() - 100000;
    for (int i = 0; i < 140; ++i) {
        RemoteIMMessage history;
        history.id = QStringLiteral("history-%1").arg(i);
        history.fromUserId = QStringLiteral("peer");
        history.toUserId = QStringLiteral("owner");
        history.text = QStringLiteral("history %1").arg(i);
        history.createdAtMillis = historyStart + i;
        app.chatState().appendMessageForRestore(history);
    }
    MainWindow window(app);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    QVERIFY(!window.findChildren<QWidget*>(QStringLiteral("messageRowIncoming")).isEmpty());

    emit fake->activityReceived(
        QStringLiteral("peer"),
        RemoteIMActivitySignal{QStringLiteral("machine:test"), RemoteIMActivityKind::MachineWorking, true, 60000, 1});
    auto* activityRow = window.findChild<QWidget*>(QStringLiteral("remoteImActivityRow"));
    QTRY_VERIFY(activityRow != nullptr);
    auto* activityBubble = activityRow->findChild<QWidget*>(QStringLiteral("remoteImActivityBubble"));
    QVERIFY(activityBubble != nullptr);
    QVERIFY(activityBubble->testAttribute(Qt::WA_TranslucentBackground));
    QVERIFY(!activityBubble->autoFillBackground());

    QCOMPARE(app.chatState().selectedPeerId(), QStringLiteral("peer"));
    app.sendText(QStringLiteral("new prompt"));
    const QList<RemoteIMMessage> updated = app.chatState().messagesWith(QStringLiteral("peer"));
    QCOMPARE(updated.size(), 141);
    bool hasOutgoing = false;
    for (const RemoteIMMessage& message : updated) {
        if (message.direction == RemoteIMMessageDirection::Outgoing) hasOutgoing = true;
    }
    QVERIFY(hasOutgoing);
    QTRY_VERIFY(window.findChild<QWidget*>(QStringLiteral("messageRowOutgoing")) != nullptr);
    auto* outgoingRow = window.findChild<QWidget*>(QStringLiteral("messageRowOutgoing"));
    QLayout* messageLayout = activityRow->parentWidget()->layout();
    QVERIFY(messageLayout != nullptr);
    QVERIFY(messageLayout->indexOf(outgoingRow) < messageLayout->indexOf(activityRow));
    QCOMPARE(messageLayout->indexOf(activityRow), messageLayout->count() - 2);
}

// 模拟一次真实拖放：Qt 的 drop 依赖前面的 dragEnter/dragMove 建立内部状态，
// 只发 QDropEvent 会被直接丢掉。位置取 viewport 中心。
static void dropOnComposer(QTextEdit* editor, QMimeData* mime) {
    QWidget* target = editor->viewport();
    const QPointF pos(target->width() / 2.0, target->height() / 2.0);

    QDragEnterEvent enter(pos.toPoint(), Qt::CopyAction, mime, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(target, &enter);
    QDragMoveEvent move(pos.toPoint(), Qt::CopyAction, mime, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(target, &move);
    QDropEvent drop(pos, Qt::CopyAction, mime, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(target, &drop);
}

// 拖一个文件进输入框，必须变成附件。此前 QTextEdit 默认行为把 file:/// URL 当成
// 纯文本插进去，用户看到一行路径、发出去的也是一行路径，附件根本没产生。
namespace {

int highlightedRowCount(const QWidget& window) {
    int highlighted = 0;
    for (QWidget* row : window.findChildren<QWidget*>()) {
        if (row->property("searchHit").toBool()) ++highlighted;
    }
    return highlighted;
}

}  // namespace

// 引用回复必须在气泡里真的画出来。
//
// 这条测的是**渲染路径**，不是摘要那几个纯函数——纯函数全绿也可能一个像素都没画上去：
// 之前我改过两处只落地了一处，编译通过、单测通过，是靠把界面跑起来才发现的。
// 「回复」的交互状态：提示条要显示回复对象，发出去之后必须自动清掉。
//
// 不清掉的后果很隐蔽：用户回复完一条，接着发一句无关的话，那句话会莫名其妙
// 也带着上一条的引用——而且发送方自己的界面上看着是对的，只有对端能看出不对。
// 会话列表必须把「刚来消息的人」排到最前面。
//
// 此前它沿用联系人列表的顺序，而那是按 userId 的字母序排的：
// 刚回你消息的人可能排在第 11 位，顶上却是几周没说过话的。
// 用户报「会话列表滚动条留白」：选中一条会话时，高亮块右侧留出一条空白，
// 看起来像滚动条位置一直空着。这条用例把它变成可测量的量——
// 抓真实绘制结果，从视口右缘往左数有多少列不是高亮色。
void MainWindowLayoutTest::selectedConversationRowReachesTheListEdge() {
    auto client = std::make_unique<FakeRemoteIMClient>();
    RemoteIMApplication app(QStringLiteral("desktop-user"), std::move(client));
    app.addContact(QStringLiteral("alpha"), QStringLiteral("Alpha"));

    MainWindow window(app);
    window.resize(1280, 800);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));

    app.selectPeer(QStringLiteral("alpha"));
    app.sendText(QStringLiteral("hello"));
    QCoreApplication::processEvents();
    QCoreApplication::processEvents();

    auto* list = window.findChild<QListWidget*>(QStringLiteral("conversationList"));
    QVERIFY(list != nullptr);
    QVERIFY2(list->count() > 0, "会话列表应至少有一行");
    list->setCurrentRow(0);
    QCoreApplication::processEvents();

    // 少量会话时不该出现纵向滚动条；留白若仍在，就与滚动条无关，是行本身没画到边。
    QVERIFY2(!list->verticalScrollBar()->isVisible(), "前提不成立：只有一条会话却出现了纵向滚动条");

    const QRect rowRect = list->visualItemRect(list->item(0));
    const QImage painted = list->viewport()->grab().toImage();
    QVERIFY(!painted.isNull());

    const QColor selected(QStringLiteral("#dff3ff"));
    // grab() 返回的是物理像素图：rowRect 是逻辑坐标，取样前必须乘 dpr，
    // 否则高分屏上会取到别的行。
    const qreal dpr = painted.devicePixelRatio();
    const int y = qRound(rowRect.center().y() * dpr);
    QVERIFY2(y >= 0 && y < painted.height(),
             qPrintable(QStringLiteral("取样行 y=%1 超出抓图高度 %2（dpr=%3）").arg(y).arg(painted.height()).arg(dpr)));

    // 从右缘往左找第一列高亮像素，中间隔了多少列就是留白宽度（物理像素）。
    int gap = 0;
    for (int x = painted.width() - 1; x >= 0; --x) {
        if (painted.pixelColor(x, y) == selected) {
            break;
        }
        ++gap;
    }

    const int tolerance = qMax(1, qRound(dpr));
    QVERIFY2(gap <= tolerance,
             qPrintable(QStringLiteral("选中行右侧留白 %1 物理像素，应贴到列表边缘（容 %2）").arg(gap).arg(tolerance)));
}

// 上一条特意断言「没有滚动条」，只能证明委托多缩了 6px。
// 但用户截图里滚动条是在的，所以这一条把会话灌到溢出、让滚动条真的出现，
// 再量一次右侧空白——用来区分「委托内缩」和「滚动条本身占位」两件事。
void MainWindowLayoutTest::selectedConversationRowReachesTheListEdgeWithScrollBar() {
    auto client = std::make_unique<FakeRemoteIMClient>();
    RemoteIMApplication app(QStringLiteral("desktop-user"), std::move(client));
    for (int i = 0; i < 40; ++i) {
        const QString id = QStringLiteral("peer%1").arg(i, 2, 10, QLatin1Char('0'));
        app.addContact(id, QStringLiteral("联系人 %1").arg(i));
    }

    MainWindow window(app);
    window.resize(1280, 800);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));

    app.selectPeer(QStringLiteral("peer00"));
    app.sendText(QStringLiteral("hello"));
    QCoreApplication::processEvents();
    QCoreApplication::processEvents();

    auto* list = window.findChild<QListWidget*>(QStringLiteral("conversationList"));
    QVERIFY(list != nullptr);
    list->setCurrentRow(0);
    QCoreApplication::processEvents();

    // 这条用例的前提正好和上一条相反：必须真的有滚动条，否则它什么都没测到。
    QVERIFY2(list->verticalScrollBar()->isVisible(),
             qPrintable(QStringLiteral("前提不成立：%1 条会话仍未出现纵向滚动条").arg(list->count())));

    const QRect rowRect = list->visualItemRect(list->item(0));
    const QImage painted = list->viewport()->grab().toImage();
    QVERIFY(!painted.isNull());

    const QColor selected(QStringLiteral("#dff3ff"));
    const qreal dpr = painted.devicePixelRatio();
    const int y = qRound(rowRect.center().y() * dpr);
    QVERIFY2(y >= 0 && y < painted.height(),
             qPrintable(QStringLiteral("取样行 y=%1 超出抓图高度 %2（dpr=%3）").arg(y).arg(painted.height()).arg(dpr)));
    int gap = 0;
    for (int x = painted.width() - 1; x >= 0; --x) {
        if (painted.pixelColor(x, y) == selected) {
            break;
        }
        ++gap;
    }

    // 视口宽度已经不含滚动条，所以这里量到的仍然只该是委托自己的内缩。
    // 用户看到的整条空白 = 这里的 gap + 滚动条自身宽度；后者是 Qt 的正常占位，
    // 本用例不改也不断言它，只钉住委托这部分为 0。
    const int tolerance = qMax(1, qRound(dpr));
    QVERIFY2(gap <= tolerance, qPrintable(QStringLiteral("有滚动条时选中行右侧仍留白 %1 物理像素（容 %2；"
                                                         "抓图宽 %3，滚动条宽 %4 逻辑像素，dpr=%5）")
                                              .arg(gap)
                                              .arg(tolerance)
                                              .arg(painted.width())
                                              .arg(list->verticalScrollBar()->width())
                                              .arg(dpr)));
}

namespace {

// 真正按住滑块拖一段，而不是 setValue —— 后者绕开了命中测试，
// 收窄之后滑块是否还抓得住，只有真实鼠标事件能证明。
bool dragScrollBarHandle(QScrollBar* bar, int dyLogical) {
    QStyleOptionSlider option;
    option.initFrom(bar);
    option.orientation = bar->orientation();
    option.minimum = bar->minimum();
    option.maximum = bar->maximum();
    option.sliderPosition = bar->sliderPosition();
    option.sliderValue = bar->value();
    option.pageStep = bar->pageStep();
    option.singleStep = bar->singleStep();
    const QRect handle = bar->style()->subControlRect(QStyle::CC_ScrollBar, &option, QStyle::SC_ScrollBarSlider, bar);
    if (handle.isEmpty()) {
        return false;
    }
    const QPoint from = handle.center();
    const QPoint to = from + QPoint(0, dyLogical);
    QTest::mousePress(bar, Qt::LeftButton, Qt::NoModifier, from);
    // QTest::mouseMove 发出的移动事件 buttons 为 NoButton，QScrollBar 会当成
    // 普通悬停忽略掉，拖不动。这里手工构造一个「左键仍按住」的移动事件。
    QMouseEvent move(QEvent::MouseMove, to, bar->mapToGlobal(to), Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
    QCoreApplication::sendEvent(bar, &move);
    QCoreApplication::processEvents();
    QTest::mouseRelease(bar, Qt::LeftButton, Qt::NoModifier, to);
    QCoreApplication::processEvents();
    return true;
}

}  // namespace

// 收窄滚动条之后要同时成立三件事，缺一件都不算修好：
//   1. 有滚动条时确实变窄了，且宽度跟着 UiZoom 缩放（不是写死物理像素）
//   2. 还能拖、还能滚 —— 收窄不能把交互弄没
//   3. 没有滚动条时不占位，别用一条恒定留白换掉原来的问题
void MainWindowLayoutTest::listScrollBarsAreNarrowAndStillScrollableAcrossZoomLevels() {
    struct RestoreZoom {
        qreal old;
        ~RestoreZoom() {
            UiZoom::setFactor(old);
        }
    } restore{UiZoom::factor()};

    for (const qreal zoom : {0.8, 1.0, 1.5, 2.0}) {
        UiZoom::setFactor(zoom);

        auto client = std::make_unique<FakeRemoteIMClient>();
        RemoteIMApplication app(QStringLiteral("desktop-user"), std::move(client));
        for (int i = 0; i < 40; ++i) {
            app.addContact(QStringLiteral("peer%1").arg(i, 2, 10, QLatin1Char('0')),
                           QStringLiteral("联系人 %1").arg(i));
        }

        MainWindow window(app);
        window.resize(1280, 800);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        QCoreApplication::processEvents();

        auto* list = window.findChild<QListWidget*>(QStringLiteral("conversationList"));
        QVERIFY(list != nullptr);
        QScrollBar* bar = list->verticalScrollBar();
        QVERIFY2(bar->isVisible(), qPrintable(QStringLiteral("缩放 %1：40 条会话却没有纵向滚动条").arg(zoom)));

        // 1. 宽度：基准 8px 随缩放走。Qt 默认约 17px，这里必须明显更窄。
        const int expected = UiZoom::s(8);
        QVERIFY2(qAbs(bar->width() - expected) <= 1,
                 qPrintable(
                     QStringLiteral("缩放 %1：滚动条宽 %2px，期望约 %3px").arg(zoom).arg(bar->width()).arg(expected)));

        // 2. 还能滚：直接改值 + 真实滚轮事件都要生效。
        // 注意别假设初始位置在顶部——列表会自动滚到当前项，起始值不一定是 minimum。
        QVERIFY2(bar->maximum() > bar->minimum(),
                 qPrintable(QStringLiteral("缩放 %1：滚动范围为空，无法验证滚动").arg(zoom)));
        bar->setValue(bar->maximum());
        QCOMPARE(bar->value(), bar->maximum());
        bar->setValue(bar->minimum());
        QCOMPARE(bar->value(), bar->minimum());
        QWheelEvent wheel(QPointF(list->viewport()->rect().center()),
                          list->viewport()->mapToGlobal(list->viewport()->rect().center()), QPoint(0, -40),
                          QPoint(0, -120), Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, /*inverted*/ false);
        QCoreApplication::sendEvent(list->viewport(), &wheel);
        QCoreApplication::processEvents();
        QVERIFY2(bar->value() > bar->minimum(), qPrintable(QStringLiteral("缩放 %1：滚轮没有滚动列表").arg(zoom)));

        // 真实拖动：8px 宽的滑块必须还抓得住。setValue 走不到命中测试，
        // 证明不了这一点。
        bar->setValue(bar->minimum());
        QCoreApplication::processEvents();
        QVERIFY2(dragScrollBarHandle(bar, 120),
                 qPrintable(QStringLiteral("缩放 %1：会话列表滑块矩形为空，无法拖动").arg(zoom)));
        QVERIFY2(
            bar->value() > bar->minimum(),
            qPrintable(
                QStringLiteral("缩放 %1：按住会话列表滑块拖动后 value 没变（仍为 %2）").arg(zoom).arg(bar->value())));

        // 联系人列表用的是同一套样式，一并验证它也拖得动。
        auto* contacts = window.findChild<QListWidget*>(QStringLiteral("contactsList"));
        QVERIFY(contacts != nullptr);
        auto* contactsNav = window.findChild<QPushButton*>(QStringLiteral("contactsNavButton"));
        QVERIFY(contactsNav != nullptr);
        contactsNav->click();
        QCoreApplication::processEvents();
        QScrollBar* contactsBar = contacts->verticalScrollBar();
        QVERIFY(contacts->isVisible());
        QVERIFY(contactsBar->isVisible());
        QVERIFY2(contactsBar->maximum() > contactsBar->minimum(), "联系人滚动范围必须非空，不能跳过拖动验证");
        QVERIFY2(qAbs(contactsBar->width() - expected) <= 1,
                 qPrintable(QStringLiteral("缩放 %1：联系人列表滚动条宽 %2px，期望约 %3px")
                                .arg(zoom)
                                .arg(contactsBar->width())
                                .arg(expected)));
        auto* contactsPane = window.findChild<QWidget*>(QStringLiteral("contactsDirectoryPane"));
        QVERIFY(contactsPane != nullptr);
        const int barRight = contactsBar->mapTo(contactsPane, QPoint(contactsBar->width(), 0)).x();
        const int trailingGap = contactsPane->width() - barRight;
        QVERIFY2(trailingGap <= 1,
                 qPrintable(QStringLiteral("缩放 %1：联系人滚动条到分割线仍有 %2px 空隙").arg(zoom).arg(trailingGap)));
        contactsBar->setValue(contactsBar->minimum());
        QCoreApplication::processEvents();
        QVERIFY2(dragScrollBarHandle(contactsBar, 120),
                 qPrintable(QStringLiteral("缩放 %1：联系人列表滑块矩形为空").arg(zoom)));
        QVERIFY2(contactsBar->value() > contactsBar->minimum(),
                 qPrintable(QStringLiteral("缩放 %1：拖动联系人列表滑块后 value 没变").arg(zoom)));
    }

    // 3. 会话很少时不该有滚动条，也不该留下它的占位宽度。
    UiZoom::setFactor(1.0);
    auto client = std::make_unique<FakeRemoteIMClient>();
    RemoteIMApplication app(QStringLiteral("desktop-user"), std::move(client));
    app.addContact(QStringLiteral("solo"), QStringLiteral("Solo"));

    MainWindow window(app);
    window.resize(1280, 800);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    QCoreApplication::processEvents();

    auto* list = window.findChild<QListWidget*>(QStringLiteral("conversationList"));
    QVERIFY(list != nullptr);
    QVERIFY2(!list->verticalScrollBar()->isVisible(), "只有一条会话时不该出现滚动条");
    QCOMPARE(list->viewport()->width(), list->width());
}

// 同秒消息和状态刷新仍保持会话列表稳定倒序，避免仅因重绘而抖动。
void MainWindowLayoutTest::conversationListPutsNewestMessageFirst() {
    auto client = std::make_unique<FakeRemoteIMClient>();
    RemoteIMApplication app(QStringLiteral("desktop-user"), std::move(client));
    // 故意让字母序和时间序相反：alpha 名字最小但消息最旧。
    app.addContact(QStringLiteral("alpha"), QStringLiteral("Alpha"));
    app.addContact(QStringLiteral("bravo"), QStringLiteral("Bravo"));
    app.addContact(QStringLiteral("charlie"), QStringLiteral("Charlie"));
    // 从没聊过的联系人，应当沉到最后。
    app.addContact(QStringLiteral("delta"), QStringLiteral("Delta"));

    MainWindow window(app);
    window.resize(1280, 800);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));

    app.selectPeer(QStringLiteral("alpha"));
    app.sendText(QStringLiteral("最早"));
    app.selectPeer(QStringLiteral("bravo"));
    app.sendText(QStringLiteral("中间"));
    app.selectPeer(QStringLiteral("charlie"));
    app.sendText(QStringLiteral("最新"));
    QCoreApplication::processEvents();

    // 显式给三条消息设置**严格递增且互不相同**的时间。
    //
    // 原来这条用例连发三条就直接断言，时间戳由机器快慢决定：
    // 恰好不同就走时间分支（通过），恰好相同就走并列分支（失败）。
    // 结果是它在 Windows 恒绿、在 macOS 恒红，看着像平台差异，
    // 其实是**用例根本没控制住自己的前提**。
    //
    // 产品口径是「时间相同按首字母」，所以这里要测的是时间不同时按时间排——
    // 那就必须让时间确实不同，而不是指望它不同。
    struct Seed {
        const char* peer;
        qint64 at;
    };
    const QList<Seed> seeds{{"alpha", 1000}, {"bravo", 2000}, {"charlie", 3000}};
    for (const Seed& seed : seeds) {
        RemoteIMMessage m;
        QVERIFY(app.chatState().latestMessageWith(QString::fromLatin1(seed.peer), &m));
        QVERIFY(app.chatState().updateMessageTime(m.id, seed.at));
    }
    emit app.stateChanged();  // 走 MainWindow 平时重排列表的那条真实路径
    QCoreApplication::processEvents();
    QCoreApplication::processEvents();

    auto* list = window.findChild<QListWidget*>(QStringLiteral("conversationList"));
    QVERIFY(list != nullptr);
    QStringList order;
    for (int row = 0; row < list->count(); ++row) {
        // Qt::UserRole 就是 MainWindow.cpp 里的 UserIdRole（存 userId）。
        order.append(list->item(row)->data(Qt::UserRole).toString());
    }
    QVERIFY2(order.size() >= 4, qPrintable(QStringLiteral("会话数不对：%1").arg(order.size())));
    QCOMPARE(order.at(0), QStringLiteral("charlie"));
    QCOMPARE(order.at(1), QStringLiteral("bravo"));
    QCOMPARE(order.at(2), QStringLiteral("alpha"));
    // 没有消息的排最后，而不是因为名字靠前就冒到上面。
    QCOMPARE(order.at(3), QStringLiteral("delta"));

    // 给最旧的那个补一条新消息、并显式设成最大时间，它必须立刻升到第一。
    // 这里同样不依赖「新发的消息时间一定更大」——那又会变成碰运气。
    app.selectPeer(QStringLiteral("alpha"));
    app.sendText(QStringLiteral("又说话了"));
    RemoteIMMessage revived;
    QVERIFY(app.chatState().latestMessageWith(QStringLiteral("alpha"), &revived));
    QVERIFY(app.chatState().updateMessageTime(revived.id, 9000));
    emit app.stateChanged();
    QCoreApplication::processEvents();
    QCOMPARE(list->item(0)->data(Qt::UserRole).toString(), QStringLiteral("alpha"));
}

// 时间完全相同时按首字母稳定排序。
//
// 产品口径：时间戳是秒级精度，同秒并列按名字即可，不引入额外的活动顺序状态。
// 这条用例钉住的是「并列时的行为是确定的」——不是随机、也不会每次刷新变一个样。
void MainWindowLayoutTest::conversationListBreaksTimeTiesByName() {
    auto client = std::make_unique<FakeRemoteIMClient>();
    RemoteIMApplication app(QStringLiteral("desktop-user"), std::move(client));
    app.addContact(QStringLiteral("charlie"), QStringLiteral("Charlie"));
    app.addContact(QStringLiteral("alpha"), QStringLiteral("Alpha"));
    app.addContact(QStringLiteral("bravo"), QStringLiteral("Bravo"));

    MainWindow window(app);
    window.resize(1280, 800);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));

    for (const char* peer : {"charlie", "alpha", "bravo"}) {
        app.selectPeer(QString::fromLatin1(peer));
        app.sendText(QStringLiteral("同一时刻"));
    }
    // 三条设成**完全相同**的时间，强制走并列分支。
    for (const char* peer : {"charlie", "alpha", "bravo"}) {
        RemoteIMMessage m;
        QVERIFY(app.chatState().latestMessageWith(QString::fromLatin1(peer), &m));
        QVERIFY(app.chatState().updateMessageTime(m.id, 5000));
    }
    emit app.stateChanged();
    QCoreApplication::processEvents();

    auto* list = window.findChild<QListWidget*>(QStringLiteral("conversationList"));
    QVERIFY(list != nullptr);
    QStringList order;
    for (int row = 0; row < list->count(); ++row) {
        order.append(list->item(row)->data(Qt::UserRole).toString());
    }
    QCOMPARE(order, QStringList({QStringLiteral("alpha"), QStringLiteral("bravo"), QStringLiteral("charlie")}));

    // 再刷一次必须还是同样的顺序：并列时如果顺序不稳定，列表会自己抖。
    emit app.stateChanged();
    QCoreApplication::processEvents();
    QStringList again;
    for (int row = 0; row < list->count(); ++row) {
        again.append(list->item(row)->data(Qt::UserRole).toString());
    }
    QCOMPARE(again, order);
}

// 引用栏必须显示目标消息，发送后清掉引用状态，避免下一条消息误带旧引用。
void MainWindowLayoutTest::replyBarShowsTargetAndClearsAfterSending() {
    auto client = std::make_unique<FakeRemoteIMClient>();
    RemoteIMApplication app(QStringLiteral("desktop-user"), std::move(client));
    app.addContact(QStringLiteral("phone-user"), QStringLiteral("iPhone"));

    MainWindow window(app);
    window.resize(1280, 800);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    app.selectPeer(QStringLiteral("phone-user"));

    app.sendText(QStringLiteral("原始消息内容"));
    QCoreApplication::processEvents();

    auto* replyBar = window.findChild<QWidget*>(QStringLiteral("pendingReplyBar"));
    auto* replyLabel = window.findChild<QLabel*>(QStringLiteral("pendingReplyLabel"));
    auto* editor = window.findChild<QTextEdit*>(QStringLiteral("messageEditor"));
    QVERIFY(replyBar != nullptr);
    QVERIFY(replyLabel != nullptr);
    QVERIFY(editor != nullptr);
    // 还没点「回复」时提示条不该出现。
    QVERIFY(replyBar->isHidden());

    // 直接调菜单动作背后的入口，而不是模拟右键弹窗：QMenu::exec 会阻塞事件循环，
    // 在测试里点它需要额外的定时器配合，测的还是 Qt 自己的菜单而不是我们的逻辑。
    window.beginReplyTo(app.chatState().messages().last());
    QCoreApplication::processEvents();
    QVERIFY2(!replyBar->isHidden(), "点了回复之后提示条必须出现");
    QVERIFY2(replyLabel->text().contains(QStringLiteral("原始消息内容")),
             qPrintable(QStringLiteral("提示条没显示回复对象：") + replyLabel->text()));

    editor->setPlainText(QStringLiteral("这是回复内容"));
    // 点真实的发送按钮，走用户实际走的那条路。
    auto* sendButton = window.findChild<QPushButton*>(QStringLiteral("sendButton"));
    QVERIFY(sendButton != nullptr);
    QTest::mouseClick(sendButton, Qt::LeftButton);
    QCoreApplication::processEvents();

    QVERIFY2(replyBar->isHidden(), "发送后提示条必须收起，否则下一条会带上旧引用");
    const RemoteIMMessage sent = app.chatState().messages().last();
    QCOMPARE(sent.text, QStringLiteral("这是回复内容"));
    QVERIFY2(sent.hasQuote, "回复必须带上引用");
    QVERIFY(sent.quote.digest.contains(QStringLiteral("原始消息内容")));

    // 再发一条普通消息，绝不能继续带引用。
    editor->setPlainText(QStringLiteral("一句无关的话"));
    QTest::mouseClick(sendButton, Qt::LeftButton);
    QCoreApplication::processEvents();
    const RemoteIMMessage plain = app.chatState().messages().last();
    QCOMPARE(plain.text, QStringLiteral("一句无关的话"));
    QVERIFY2(!plain.hasQuote, "引用是单条回复的属性，不能粘在后续消息上");
}

namespace {

// 引用块所在的气泡会在「排队 → 已发送」时被整块重建，旧的走 deleteLater：
// 那一瞬间 findChild 能同时看见新旧两个同名部件，而旧的马上就要被销毁。
// 所以必须先让事件循环把重建和销毁都跑完，再去取——顺序反了就会拿到野指针
// （实测直接崩在 QWidget::mapTo）。取到后还要求「只有一个」，否则说明没settle。
QWidget* settledQuoteBlock(QWidget& window) {
    QWidget* block = nullptr;
    // QTRY_ 宏只能用在测试方法里，这里手动转事件循环。
    for (int i = 0; i < 50; ++i) {
        QTest::qWait(20);
        const QList<QWidget*> blocks = window.findChildren<QWidget*>(QStringLiteral("messageQuoteBlock"));
        if (blocks.size() != 1) continue;
        block = blocks.first();
        // 高度为 0 表示布局还没跑，这时候的坐标是假的，点下去会落到窗口外。
        if (block->height() > 0) return block;
    }
    return block;
}

}  // namespace

void MainWindowLayoutTest::clickingQuoteBlockJumpsToTheOriginalMessage() {
    auto client = std::make_unique<FakeRemoteIMClient>();
    RemoteIMApplication app(QStringLiteral("desktop-user"), std::move(client));
    app.addContact(QStringLiteral("phone-user"), QStringLiteral("iPhone"));

    MainWindow window(app);
    window.resize(1280, 800);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    app.selectPeer(QStringLiteral("phone-user"));

    app.sendText(QStringLiteral("这是被引用的原始消息"));
    QCoreApplication::processEvents();
    const QList<RemoteIMMessage> seeded = app.chatState().messagesWith(QStringLiteral("phone-user"));
    QVERIFY(!seeded.isEmpty());
    const RemoteIMMessage original = seeded.last();

    // 中间塞一批，保证原消息不是「刚好就在回复旁边」——否则不跳转也能碰巧通过。
    for (int i = 0; i < 12; ++i) {
        app.sendText(QStringLiteral("填充消息 %1").arg(i));
        QCoreApplication::processEvents();
    }

    const RemoteIMQuote quote = MessageQuote::quoteFor(original);
    QVERIFY2(!quote.msgId.isEmpty(), "引用必须带上原始 SDK 消息 ID，否则块根本不可点");
    app.sendText(QStringLiteral("这是回复"), quote, true);

    QWidget* block = settledQuoteBlock(window);
    QVERIFY2(block != nullptr, "带引用的消息必须渲染出引用块");
    QVERIFY2(block->height() > 0, "引用块必须已经完成布局，否则点击坐标是假的");
    QCOMPARE(block->property("quoteMsgId").toString(), quote.msgId);

    const auto highlightedRow = [&window]() -> QWidget* {
        QWidget* found = nullptr;
        for (QWidget* candidate : window.findChildren<QWidget*>()) {
            if (!candidate->property("searchHit").toBool()) continue;
            // 同一时刻只该有一条高亮；多于一条说明上一次的没清掉。
            if (found) return nullptr;
            found = candidate;
        }
        return found;
    };

    QVERIFY2(highlightedRow() == nullptr, "还没点，不该有任何消息被高亮");

    QTest::mouseClick(block, Qt::LeftButton);
    // 跳转被推迟到下一轮事件循环（它可能触发整屏刷新），所以要真正转起事件循环。
    QTest::qWait(100);

    QWidget* row = highlightedRow();
    QVERIFY2(row != nullptr, "点击引用块必须定位并高亮原消息");
    // 高亮到「某一条」不算数，必须高亮到「被引用的那一条」：
    // 只断言存在高亮的话，跳到任意一条都能过。
    auto* body = row->findChild<MarkdownLabel*>(QStringLiteral("messageMarkdownView"));
    QVERIFY2(body != nullptr, "高亮的行里应当有正文");
    QVERIFY2(body->plainText().contains(QStringLiteral("这是被引用的原始消息")),
             qPrintable(QStringLiteral("跳错了行，正文是：") + body->plainText()));
}

void MainWindowLayoutTest::clickingQuoteBlockSaysSoWhenTheOriginalIsNotLocal() {
    auto client = std::make_unique<FakeRemoteIMClient>();
    RemoteIMApplication app(QStringLiteral("desktop-user"), std::move(client));
    app.addContact(QStringLiteral("phone-user"), QStringLiteral("iPhone"));

    MainWindow window(app);
    window.resize(1280, 800);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    app.selectPeer(QStringLiteral("phone-user"));

    // 引用一条本地根本没有的消息：别的设备发的、或本地已清理过的历史。
    RemoteIMQuote quote;
    quote.msgId = QStringLiteral("not-in-this-database");
    quote.senderId = QStringLiteral("phone-user");
    quote.digest = QStringLiteral("只剩快照的原文");
    quote.kind = QStringLiteral("text");
    app.sendText(QStringLiteral("这是回复"), quote, true);

    QWidget* block = settledQuoteBlock(window);
    QVERIFY(block != nullptr);
    QVERIFY(block->height() > 0);

    QTest::mouseClick(block, Qt::LeftButton);
    QTest::qWait(100);

    // 手型光标等于承诺了会有反应，所以定位不到时必须说一句，不能静默。
    auto* toast = window.findChild<QLabel*>(QStringLiteral("toast"));
    QVERIFY2(toast != nullptr && toast->isVisible(), "定位不到原消息时必须给出提示");
    QVERIFY2(toast->text().contains(QStringLiteral("原消息")),
             qPrintable(QStringLiteral("提示文案不对：") + toast->text()));
}

void MainWindowLayoutTest::quotedReplyRendersQuoteBlockAboveBody() {
    auto client = std::make_unique<FakeRemoteIMClient>();
    RemoteIMApplication app(QStringLiteral("desktop-user"), std::move(client));
    app.addContact(QStringLiteral("phone-user"), QStringLiteral("iPhone"));

    MainWindow window(app);
    window.resize(1280, 800);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    app.selectPeer(QStringLiteral("phone-user"));

    app.sendText(QStringLiteral("这是被引用的原始消息"));
    QCoreApplication::processEvents();
    // 没有引用时不该凭空冒出引用块。
    QVERIFY(window.findChild<QWidget*>(QStringLiteral("messageQuoteBlock")) == nullptr);

    RemoteIMQuote quote;
    quote.senderId = QStringLiteral("phone-user");
    quote.digest = QStringLiteral("这是被引用的原始消息");
    quote.kind = QStringLiteral("text");
    app.sendText(QStringLiteral("这是回复"), quote, true);
    QCoreApplication::processEvents();

    QWidget* block = window.findChild<QWidget*>(QStringLiteral("messageQuoteBlock"));
    QVERIFY2(block != nullptr, "带引用的消息必须渲染出引用块");
    // findChild 找得到 ≠ 真的画出来了：部件只要 parent 还在就找得到，
    // 哪怕它压根没被加进布局（deleteLater 也不会立刻销毁它）。
    // 所以断言必须落在「进了父气泡的布局」上，否则这条测试是恒真的。
    QWidget* bubble = block->parentWidget();
    QVERIFY(bubble != nullptr);
    QVERIFY(bubble->layout() != nullptr);
    QVERIFY2(bubble->layout()->indexOf(block) >= 0, "引用块必须被加入气泡布局");
    // 并且排在正文之上。
    QCOMPARE(bubble->layout()->indexOf(block), 0);
    auto* text = block->findChild<QLabel*>(QStringLiteral("messageQuoteText"));
    QVERIFY(text != nullptr);
    // 状态更新可能经 deleteLater 替换待处理行；每次重试重新找当前标签，
    // 不持有已删除的控件指针。
    const auto currentQuoteText = [&window] {
        const auto* label = window.findChild<QLabel*>(QStringLiteral("messageQuoteText"));
        return label ? label->text() : QString();
    };
    QTRY_VERIFY2(currentQuoteText().contains(QStringLiteral("这是被引用的原始消息")),
                 qPrintable(QStringLiteral("引用块内容不对：") + currentQuoteText()));
    QVERIFY(currentQuoteText().contains(QStringLiteral("phone-user")));
}

void MainWindowLayoutTest::droppingFilesIntoComposerAttachesThemInsteadOfPastingPaths() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString docPath = QDir(dir.path()).filePath(QStringLiteral("report.txt"));
    QFile doc(docPath);
    QVERIFY(doc.open(QIODevice::WriteOnly));
    doc.write("hello");
    doc.close();

    auto client = std::make_unique<FakeRemoteIMClient>();
    auto* fakeClient = client.get();
    RemoteIMApplication app(QStringLiteral("desktop-user"), std::move(client));
    app.addContact(QStringLiteral("phone-user"), QStringLiteral("iPhone"));

    MainWindow window(app);
    window.resize(1280, 800);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    auto* editor = window.findChild<QTextEdit*>(QStringLiteral("messageEditor"));
    auto* sendButton = window.findChild<QPushButton*>(QStringLiteral("sendButton"));
    QVERIFY(editor != nullptr);
    QVERIFY(sendButton != nullptr);

    QMimeData mime;
    mime.setUrls({QUrl::fromLocalFile(docPath)});
    dropOnComposer(editor, &mime);

    // 输入框里不能出现路径文本，只能有一枚内联对象（U+FFFC）。
    QVERIFY(!editor->toPlainText().contains(QStringLiteral("file://")));
    QVERIFY(!editor->toPlainText().contains(QStringLiteral("report.txt")));
    QVERIFY(editor->toPlainText().contains(QChar(0xFFFC)));
    QVERIFY(sendButton->isEnabled());

    sendButton->click();
    QCOMPARE(fakeClient->lastFilePeerId(), QStringLiteral("phone-user"));
    QCOMPARE(fakeClient->lastFilePath(), docPath);
    QCOMPARE(fakeClient->lastFileName(), QStringLiteral("report.txt"));
}

// 拖进来的图片按图片发，且发的是原文件本身——不重新编码成 PNG，
// 否则一张 3MB 的 JPG 会被放大成十几 MB。
void MainWindowLayoutTest::droppingAnImageFileSendsTheOriginalFileAsAnImage() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString imagePath = QDir(dir.path()).filePath(QStringLiteral("photo.png"));
    QImage image(40, 20, QImage::Format_RGB32);
    image.fill(Qt::red);
    QVERIFY(image.save(imagePath, "PNG"));

    auto client = std::make_unique<FakeRemoteIMClient>();
    auto* fakeClient = client.get();
    RemoteIMApplication app(QStringLiteral("desktop-user"), std::move(client));
    app.addContact(QStringLiteral("phone-user"), QStringLiteral("iPhone"));

    MainWindow window(app);
    window.resize(1280, 800);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    auto* editor = window.findChild<QTextEdit*>(QStringLiteral("messageEditor"));
    auto* sendButton = window.findChild<QPushButton*>(QStringLiteral("sendButton"));
    QVERIFY(editor != nullptr);

    QMimeData mime;
    mime.setUrls({QUrl::fromLocalFile(imagePath)});
    dropOnComposer(editor, &mime);

    QVERIFY(editor->toPlainText().contains(QChar(0xFFFC)));
    sendButton->click();

    QCOMPARE(fakeClient->lastImagePeerId(), QStringLiteral("phone-user"));
    QCOMPARE(fakeClient->lastImagePath(), imagePath);  // 原文件，不是临时 PNG 副本
    QCOMPARE(fakeClient->lastFilePath(), QString());   // 没有走文件卡分支
}

// 设置项增多时边框仍须可见，行背景不能覆盖面板层级。
void MainWindowLayoutTest::settingsPanelBorderIsNotCoveredByRows() {
    auto client = std::make_unique<FakeRemoteIMClient>();
    RemoteIMApplication app(QStringLiteral("desktop-user"), std::move(client));
    MainWindow window(app);

    auto* panel = window.findChild<QWidget*>(QStringLiteral("settingsPanel"));
    QVERIFY(panel != nullptr);
    panel->resize(600, 400);
    const QImage painted = panel->grab().toImage();
    QVERIFY(!painted.isNull());

    // 子控件不得覆盖面板外框。这里断言的是"画出来了什么"：
    // 沿高度多点采样，左右边框都得在。
    const auto nearBorder = [](QRgb c) {
        // 抗锯齿会让边框像素略有出入，允许小幅偏差但必须明显不是白色。
        return qAbs(qRed(c) - 0xda) < 24 && qAbs(qGreen(c) - 0xe4) < 24 && qAbs(qBlue(c) - 0xf0) < 24;
    };

    int leftHits = 0;
    int rightHits = 0;
    int sampled = 0;
    QStringList leftMisses;
    QStringList rightMisses;
    const auto describeMiss = [](int y, QRgb first, QRgb second) {
        return QStringLiteral("y=%1 [%2,%3]")
            .arg(y)
            .arg(QString::number(first & 0x00ffffff, 16).rightJustified(6, QLatin1Char('0')))
            .arg(QString::number(second & 0x00ffffff, 16).rightJustified(6, QLatin1Char('0')));
    };
    // 圆角边界会产生平台相关的抗锯齿像素，竖边检查应避开四个圆角。
    const int cornerGuard = UiZoom::s(12);
    for (int y = cornerGuard; y < painted.height() - cornerGuard; y += UiZoom::s(12)) {
        ++sampled;
        const QRgb leftOuter = painted.pixel(0, y);
        const QRgb leftInner = painted.pixel(1, y);
        if (nearBorder(leftOuter) || nearBorder(leftInner)) {
            ++leftHits;
        } else {
            leftMisses.append(describeMiss(y, leftOuter, leftInner));
        }
        const int w = painted.width();
        const QRgb rightOuter = painted.pixel(w - 1, y);
        const QRgb rightInner = painted.pixel(w - 2, y);
        if (nearBorder(rightOuter) || nearBorder(rightInner)) {
            ++rightHits;
        } else {
            rightMisses.append(describeMiss(y, rightOuter, rightInner));
        }
    }
    QVERIFY(sampled > 5);
    QVERIFY2(leftHits == sampled, qPrintable(QStringLiteral("左边框只在 %1/%2 个采样点上存在，面板 %3x%4，缺失：%5")
                                                 .arg(leftHits)
                                                 .arg(sampled)
                                                 .arg(painted.width())
                                                 .arg(painted.height())
                                                 .arg(leftMisses.join(QStringLiteral("; ")))));
    QVERIFY2(rightHits == sampled, qPrintable(QStringLiteral("右边框只在 %1/%2 个采样点上存在，面板 %3x%4，缺失：%5")
                                                  .arg(rightHits)
                                                  .arg(sampled)
                                                  .arg(painted.width())
                                                  .arg(painted.height())
                                                  .arg(rightMisses.join(QStringLiteral("; ")))));
}

void MainWindowLayoutTest::exposesDesktopChatLayoutControls() {
    auto client = std::make_unique<FakeRemoteIMClient>();
    RemoteIMApplication app(QStringLiteral("desktop-user"), std::move(client));
    app.addContact(QStringLiteral("phone-user"), QStringLiteral("iPhone"));

    MainWindow window(app);

    QVERIFY(window.findChild<QWidget*>(QStringLiteral("navRail")) != nullptr);
    QVERIFY(window.findChild<QWidget*>(QStringLiteral("conversationPane")) != nullptr);
    QVERIFY(window.findChild<QWidget*>(QStringLiteral("chatContentPane")) != nullptr);
    QVERIFY(window.findChild<QTextEdit*>(QStringLiteral("messageEditor")) != nullptr);
    QVERIFY(window.findChild<QPushButton*>(QStringLiteral("sendButton")) != nullptr);
}

void MainWindowLayoutTest::mainWindowUsesRoomierIconFreeTitleArea() {
    auto client = std::make_unique<FakeRemoteIMClient>();
    RemoteIMApplication app(QStringLiteral("desktop-user"), std::move(client));
    MainWindow window(app);

    auto* inset = window.findChild<QWidget*>(QStringLiteral("windowTopInset"));
#ifdef Q_OS_WIN
    auto* navRail = window.findChild<QWidget*>(QStringLiteral("navRail"));
    QVERIFY(inset != nullptr);
    QVERIFY(navRail != nullptr);
    QVERIFY(inset->minimumHeight() >= UiZoom::s(32));
    QCOMPARE(inset->maximumHeight(), inset->minimumHeight());
    QCOMPARE(inset->palette().color(QPalette::Window), QColor(QStringLiteral("#f3f3f3")));
    QCOMPARE(navRail->palette().color(QPalette::Window), inset->palette().color(QPalette::Window));

    QEvent activateEvent(QEvent::WindowActivate);
    QApplication::sendEvent(&window, &activateEvent);
    QCOMPARE(inset->palette().color(QPalette::Window), QColor(QStringLiteral("#eef4f9")));
    QCOMPARE(navRail->palette().color(QPalette::Window), inset->palette().color(QPalette::Window));

    QEvent deactivateEvent(QEvent::WindowDeactivate);
    QApplication::sendEvent(&window, &deactivateEvent);
    QCOMPARE(inset->palette().color(QPalette::Window), QColor(QStringLiteral("#f3f3f3")));
    QCOMPARE(navRail->palette().color(QPalette::Window), inset->palette().color(QPalette::Window));

    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    const HWND handle = reinterpret_cast<HWND>(window.winId());
    QVERIFY(window.windowFlags().testFlag(Qt::FramelessWindowHint));
    QVERIFY(!(GetWindowLongPtrW(handle, GWL_EXSTYLE) & WS_EX_DLGMODALFRAME));
    const LONG_PTR nativeStyle = GetWindowLongPtrW(handle, GWL_STYLE);
    QVERIFY(nativeStyle & WS_THICKFRAME);
    QVERIFY(nativeStyle & WS_SYSMENU);
    QVERIFY(nativeStyle & WS_MINIMIZEBOX);
    QVERIFY(nativeStyle & WS_MAXIMIZEBOX);
#else
    QVERIFY(inset == nullptr);
#endif
}

void MainWindowLayoutTest::windowsCaptionStaysInTitleBarAbovePageActions() {
#ifdef Q_OS_WIN
    ScopedMessageHeaderFont font;
    auto client = std::make_unique<FakeRemoteIMClient>();
    RemoteIMApplication app(QStringLiteral("desktop-caption-layout"), std::move(client));
    MainWindow window(app);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    auto* controls = window.findChild<QWidget*>(QStringLiteral("windowCaptionControls"));
    auto* minimize = window.findChild<QPushButton*>(QStringLiteral("windowMinimizeButton"));
    auto* maximize = window.findChild<QPushButton*>(QStringLiteral("windowMaximizeButton"));
    auto* close = window.findChild<QPushButton*>(QStringLiteral("windowCloseButton"));
    QVERIFY(controls && minimize && maximize && close);
    QVERIFY(minimize->isVisible() && maximize->isVisible() && close->isVisible());
    QCOMPARE(minimize->accessibleName(), QStringLiteral("最小化"));
    QCOMPARE(maximize->accessibleName(), QStringLiteral("最大化"));
    QCOMPARE(close->accessibleName(), QStringLiteral("关闭"));
    const auto mapped = [&window](QWidget* widget) { return QRect(widget->mapTo(&window, QPoint()), widget->size()); };
    auto* titleBar = window.findChild<QWidget*>(QStringLiteral("windowTopInset"));
    QVERIFY(titleBar);
    QTRY_VERIFY(mapped(titleBar).contains(mapped(controls)));
    QVERIFY(titleBar->isAncestorOf(controls));
    QVERIFY(mapped(controls).top() > mapped(titleBar).top());
    const QRect originalCaption = mapped(controls);
    auto* chatHeader = window.findChild<QWidget*>(QStringLiteral("chatHeader"));
    auto* chatMore = window.findChild<QPushButton*>(QStringLiteral("moreButton"));
    QVERIFY(chatHeader && chatMore);
    QTRY_VERIFY(mapped(chatHeader).top() > mapped(controls).bottom());
    QVERIFY(mapped(chatMore).top() > mapped(titleBar).bottom());

    window.findChild<QPushButton*>(QStringLiteral("agentNavButton"))->click();
    auto* title = window.findChild<QLabel*>(QStringLiteral("agentHeaderTitle"));
    auto* more = window.findChild<QPushButton*>(QStringLiteral("agentMoreActions"));
    QVERIFY(title && more);
    QTRY_VERIFY(title->isVisible());
    QTRY_COMPARE(mapped(controls), originalCaption);
    QVERIFY(mapped(more).top() > mapped(titleBar).bottom());
    // Narrowing the content must keep two independent rows instead of moving
    // caption controls into whichever page header happens to be active.
    window.resize(window.minimumWidth(), window.height());
    QTRY_VERIFY(mapped(more).top() > mapped(titleBar).bottom());
    QVERIFY(mapped(controls).right() < window.width());
    QVERIFY(mapped(titleBar).contains(mapped(controls)));
    const QByteArray capture = qgetenv("MAICHAT_CAPTURE_MAIN_CAPTION");
    if (!capture.isEmpty()) QVERIFY(window.grab().save(QString::fromUtf8(capture)));

    window.findChild<QPushButton*>(QStringLiteral("cloudNavButton"))->click();
    auto* upload = window.findChild<QPushButton*>(QStringLiteral("cloudDriveUpload"));
    QVERIFY(upload);
    QTRY_VERIFY(upload->isVisible());
    QTRY_VERIFY(mapped(upload).top() > mapped(titleBar).bottom());
    window.findChild<QPushButton*>(QStringLiteral("contactsNavButton"))->click();
    auto* directory = window.findChild<QListWidget*>(QStringLiteral("contactsList"));
    QVERIFY(directory);
    QTRY_VERIFY(directory->isVisible());
    QTRY_VERIFY(mapped(controls).bottom() < mapped(directory).top());
    window.showFullScreen();
    QTRY_VERIFY(!titleBar->isVisible() && !controls->isVisible());
    window.showNormal();
    QTRY_VERIFY(titleBar->isVisible() && controls->isVisible());
    QVERIFY(mapped(titleBar).contains(mapped(controls)));
#else
    QSKIP("Windows custom caption; other platforms retain their native title bar.");
#endif
}

void MainWindowLayoutTest::windowsCaptionStaysVisibleWhileSettingsScrolls() {
#ifdef Q_OS_WIN
    auto client = std::make_unique<FakeRemoteIMClient>();
    RemoteIMApplication app(QStringLiteral("desktop-caption-settings-scroll"), std::move(client));
    MainWindow window(app);
    window.resize(window.minimumSize());
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    auto* settings = window.findChild<QPushButton*>(QStringLiteral("settingsNavButton"));
    auto* scroll = window.findChild<QScrollArea*>(QStringLiteral("settingsScroll"));
    auto* controls = window.findChild<QWidget*>(QStringLiteral("windowCaptionControls"));
    QVERIFY(settings && scroll && controls);
    settings->click();
    QTRY_VERIFY(scroll->isVisible());
    QTRY_VERIFY(scroll->verticalScrollBar()->maximum() > 0);
    QCoreApplication::processEvents();
    const auto captionRect = [&] { return QRect(controls->mapTo(&window, QPoint()), controls->size()); };
    const int captionCenter = captionRect().center().y();
    // The settings body really moves through its QScrollArea. A later native
    // resize must not reposition window actions using that scrolled title.
    scroll->verticalScrollBar()->setValue(scroll->verticalScrollBar()->maximum());
    QTRY_VERIFY(scroll->verticalScrollBar()->value() > 0);
    window.resize(window.width() + UiZoom::s(80), window.height() + UiZoom::s(40));
    QCoreApplication::processEvents();
    QTRY_COMPARE(captionRect().center().y(), captionCenter);
    QVERIFY(window.rect().contains(captionRect()));
    QVERIFY(captionRect().bottom() < scroll->mapTo(&window, QPoint()).y());
    for (const char* name : {"windowMinimizeButton", "windowMaximizeButton", "windowCloseButton"}) {
        auto* button = window.findChild<QPushButton*>(QString::fromLatin1(name));
        QVERIFY(button && button->isVisible() && button->isEnabled());
        QVERIFY(window.rect().contains(QRect(button->mapTo(&window, QPoint()), button->size())));
    }
#else
    QSKIP("Windows custom caption; other platforms retain their native title bar.");
#endif
}

void MainWindowLayoutTest::windowsCaptionPreservesNativeWindowActions() {
#ifdef Q_OS_WIN
    auto client = std::make_unique<FakeRemoteIMClient>();
    RemoteIMApplication app(QStringLiteral("desktop-caption-actions"), std::move(client));
    MainWindow window(app);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    auto* minimize = window.findChild<QPushButton*>(QStringLiteral("windowMinimizeButton"));
    auto* maximize = window.findChild<QPushButton*>(QStringLiteral("windowMaximizeButton"));
    auto* close = window.findChild<QPushButton*>(QStringLiteral("windowCloseButton"));
    auto* header = window.findChild<QWidget*>(QStringLiteral("chatHeader"));
    auto* titleBar = window.findChild<QWidget*>(QStringLiteral("windowTopInset"));
    QVERIFY(minimize && maximize && close && header && titleBar);
    const HWND handle = reinterpret_cast<HWND>(window.winId());
    const auto nativePoint = [&window, handle](QPoint local) {
        const qreal ratio = window.devicePixelRatioF();
        POINT point{qRound(local.x() * ratio), qRound(local.y() * ratio)};
        ClientToScreen(handle, &point);
        return MAKELPARAM(point.x, point.y);
    };
    const auto maximizePoint = [&] { return nativePoint(maximize->mapTo(&window, maximize->rect().center())); };
    QCOMPARE(SendMessageW(handle, WM_NCHITTEST, 0, maximizePoint()), LRESULT(HTMAXBUTTON));
    QCOMPARE(SendMessageW(handle, WM_NCHITTEST, 0, nativePoint(QPoint(1, 1))), LRESULT(HTTOPLEFT));
    QCOMPARE(SendMessageW(handle, WM_NCHITTEST, 0,
                          nativePoint(titleBar->mapTo(&window, QPoint(12, titleBar->height() / 2)))),
             LRESULT(HTCAPTION));
    QCOMPARE(
        SendMessageW(handle, WM_NCHITTEST, 0, nativePoint(header->mapTo(&window, QPoint(12, header->height() / 2)))),
        LRESULT(HTCLIENT));

    // Snap hover uses HTMAXBUTTON, so actual mouse clicks arrive as non-client messages.
    SendMessageW(handle, WM_NCLBUTTONDOWN, HTMAXBUTTON, maximizePoint());
    SendMessageW(handle, WM_NCLBUTTONUP, HTMAXBUTTON, maximizePoint());
    QTRY_VERIFY(window.isMaximized());
    QTRY_COMPARE(maximize->accessibleName(), QStringLiteral("还原"));
    MONITORINFO monitorInfo{};
    monitorInfo.cbSize = sizeof(monitorInfo);
    QVERIFY(GetMonitorInfoW(MonitorFromWindow(handle, MONITOR_DEFAULTTONEAREST), &monitorInfo));
    RECT bounds{};
    QVERIFY(GetWindowRect(handle, &bounds));
    QVERIFY(bounds.left >= monitorInfo.rcWork.left && bounds.top >= monitorInfo.rcWork.top);
    QVERIFY(bounds.right <= monitorInfo.rcWork.right && bounds.bottom <= monitorInfo.rcWork.bottom);
    SendMessageW(handle, WM_NCLBUTTONDOWN, HTMAXBUTTON, maximizePoint());
    SendMessageW(handle, WM_NCLBUTTONUP, HTMAXBUTTON, maximizePoint());
    QTRY_VERIFY(!window.isMaximized());
    SendMessageW(handle, WM_NCLBUTTONDOWN, HTMAXBUTTON, maximizePoint());
    SendMessageW(handle, WM_LBUTTONUP, 0, MAKELPARAM(20, 100));
    QVERIFY(!window.isMaximized());
    QVERIFY(!maximize->property("captionPressed").toBool());
    SendMessageW(handle, WM_NCLBUTTONDBLCLK, HTCAPTION,
                 nativePoint(titleBar->mapTo(&window, QPoint(12, titleBar->height() / 2))));
    QTRY_VERIFY(window.isMaximized());
    window.showNormal();
    QTRY_VERIFY(!window.isMaximized());
    QTest::mouseClick(maximize, Qt::LeftButton);
    QTRY_VERIFY(window.isMaximized());
    QTest::mouseClick(maximize, Qt::LeftButton);
    QTRY_VERIFY(!window.isMaximized());
    QTest::mouseClick(minimize, Qt::LeftButton);
    QTRY_VERIFY(window.isMinimized());
    window.showNormal();
    QTRY_VERIFY(!window.isMinimized());
    QTest::mouseClick(close, Qt::LeftButton);
    QTRY_VERIFY(!window.isVisible());
#else
    QSKIP("Windows custom caption; other platforms retain their native title bar.");
#endif
}

void MainWindowLayoutTest::conversationSidebarUsesLighterBackground() {
    auto client = std::make_unique<FakeRemoteIMClient>();
    RemoteIMApplication app(QStringLiteral("desktop-user"), std::move(client));
    MainWindow window(app);

    auto* navRail = window.findChild<QWidget*>(QStringLiteral("navRail"));
    auto* conversationPane = window.findChild<QWidget*>(QStringLiteral("conversationPane"));
    auto* contactsDirectoryPane = window.findChild<QWidget*>(QStringLiteral("contactsDirectoryPane"));
    QVERIFY(navRail != nullptr);
    QVERIFY(conversationPane != nullptr);
    QVERIFY(contactsDirectoryPane != nullptr);

    const QColor navBackground = navRail->palette().color(QPalette::Window);
    const QColor sidebarBackground = conversationPane->palette().color(QPalette::Window);
    QCOMPARE(sidebarBackground, QColor(QStringLiteral("#f8fafc")));
    QCOMPARE(contactsDirectoryPane->palette().color(QPalette::Window), sidebarBackground);
    QVERIFY2(sidebarBackground.lightness() > navBackground.lightness(),
             "The conversation sidebar should be lighter than the navigation rail");
    QVERIFY2(sidebarBackground.lightness() < QColor(Qt::white).lightness(),
             "The conversation sidebar should remain distinct from white content");
}

void MainWindowLayoutTest::windowsWindowBoundaryDoesNotConsumeLayoutOrInput() {
#ifdef Q_OS_WIN
    RemoteIMApplication app("owned-window-boundary", std::make_unique<FakeRemoteIMClient>());
    MainWindow window(app);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    auto* outline = window.findChild<QWidget*>(QStringLiteral("windowBoundaryOutline"));
    QVERIFY2(outline, "The frameless main window has no visible perimeter boundary");
    QVERIFY(outline->isVisible());
    QVERIFY(outline->testAttribute(Qt::WA_TransparentForMouseEvents));
    QCOMPARE(outline->focusPolicy(), Qt::NoFocus);
    QCOMPARE(outline->geometry(), window.rect());
    QCOMPARE(window.centralWidget()->geometry(), window.rect());
    QCOMPARE(window.centralWidget()->layout()->contentsMargins(), QMargins());
    QVERIFY(!window.graphicsEffect());

    QEvent activate(QEvent::WindowActivate);
    QApplication::sendEvent(&window, &activate);
    const QImage active = window.grab().toImage().convertToFormat(QImage::Format_RGB32);
    const auto edge = [](const QImage& image) { return image.pixelColor(image.width() / 2, 0); };
    QVERIFY2(edge(active).lightness() < 200, "The title edge still blends into the light desktop");
    QVERIFY(active.pixelColor(0, active.height() / 2).lightness() < 200);
    QVERIFY(active.pixelColor(active.width() - 1, active.height() / 2).lightness() < 200);
    QVERIFY(active.pixelColor(active.width() / 2, active.height() - 1).lightness() < 200);
    QEvent deactivate(QEvent::WindowDeactivate);
    QApplication::sendEvent(&window, &deactivate);
    const QColor inactive = edge(window.grab().toImage());
    QVERIFY(inactive.lightness() > edge(active).lightness());
    QVERIFY(inactive.lightness() < 220);

    window.resize(window.width() - 40, window.height() - 20);
    QTRY_COMPARE(outline->geometry(), window.rect());
    QCOMPARE(window.centralWidget()->geometry(), window.rect());
    window.showMaximized();
    QTRY_VERIFY(window.isMaximized());
    QTRY_VERIFY(!outline->isVisible());
    QCOMPARE(window.centralWidget()->geometry(), window.rect());
    window.showFullScreen();
    QTRY_VERIFY(window.isFullScreen());
    QTRY_VERIFY(!outline->isVisible());
    window.showNormal();
    QTRY_VERIFY(!window.isFullScreen() && !window.isMaximized());
    QTRY_VERIFY(outline->isVisible());
    QTRY_COMPARE(outline->geometry(), window.rect());
#else
    QSKIP("Other platforms retain their native window frame and shadow.");
#endif
}

void MainWindowLayoutTest::windowsWindowShadowSeparatesTheOwnedBackdrop() {
#ifdef Q_OS_WIN
    if (QGuiApplication::platformName() != QStringLiteral("windows"))
        QSKIP("The native compositor capture requires the Windows platform plugin.");
    const auto module = LoadLibraryW(L"dwmapi.dll");
    QVERIFY(module);
    const auto releaseModule = qScopeGuard([module] { FreeLibrary(module); });
    const auto attribute = reinterpret_cast<decltype(&DwmGetWindowAttribute)>(
        GetProcAddress(module, "DwmGetWindowAttribute"));
    const auto flush = reinterpret_cast<decltype(&DwmFlush)>(GetProcAddress(module, "DwmFlush"));
    QVERIFY(attribute && flush);

    // Capture only an owned blank fixture and its own plain backdrop. Window
    // corners are checked before each grab so unrelated desktop data is never
    // substituted for the expected backdrop when another window is in front.
    QWidget backdrop(nullptr, Qt::Window | Qt::FramelessWindowHint);
    backdrop.setWindowTitle(QStringLiteral("MaiChat owned boundary backdrop"));
    backdrop.setStyleSheet(QStringLiteral("background: #eef2f6;"));
    QScreen* screen = QGuiApplication::primaryScreen();
    QVERIFY(screen);
    const QRect available = screen->availableGeometry();
    RemoteIMApplication app("owned-window-shadow", std::make_unique<FakeRemoteIMClient>());
    MainWindow window(app);
    window.resize(window.minimumSize());
    const int padding = 32;
    const QSize outerSize = window.size() + QSize(padding * 2, padding * 2);
    QVERIFY(available.width() >= outerSize.width() && available.height() >= outerSize.height());
    const QRect captureRegion(available.center() - QPoint(outerSize.width() / 2, outerSize.height() / 2), outerSize);
    backdrop.setGeometry(captureRegion);
    backdrop.show();
    backdrop.raise();
    backdrop.activateWindow();
    QVERIFY(QTest::qWaitForWindowExposed(&backdrop));
    backdrop.repaint();
    QCoreApplication::processEvents();
    QTest::qWait(200);
    const HWND backdropHandle = reinterpret_cast<HWND>(backdrop.winId());
    const auto cornerIsOwned = [&](QPoint point) {
        const qreal ratio = screen->devicePixelRatio();
        const POINT nativePoint{qRound(point.x() * ratio), qRound(point.y() * ratio)};
        return WindowFromPoint(nativePoint) == backdropHandle;
    };
    QTRY_VERIFY(cornerIsOwned(captureRegion.topLeft() + QPoint(2, 2)));
    flush();
    const QImage before = screen->grabWindow(0, captureRegion.x(), captureRegion.y(),
                                             captureRegion.width(), captureRegion.height()).toImage();
    QVERIFY(!before.isNull());
    int unexpectedBackdropPixels = 0;
    for (int y = 0; y < before.height(); ++y)
        for (int x = 0; x < before.width(); ++x)
            if (before.pixelColor(x, y) != QColor(238, 242, 246)) ++unexpectedBackdropPixels;
    QVERIFY2(unexpectedBackdropPixels == 0,
             "The compositor has not presented the exclusively owned plain backdrop; no capture is saved");
    window.move(captureRegion.topLeft() + QPoint(padding, padding));
    window.show();
    window.raise();
    window.activateWindow();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    QTRY_VERIFY(window.isActiveWindow());
    QTRY_VERIFY(cornerIsOwned(captureRegion.topLeft() + QPoint(2, 2)));
    QTRY_VERIFY(cornerIsOwned(captureRegion.bottomRight() - QPoint(2, 2)));
    // DWM completes its asynchronous activation/shadow transition before the
    // bounded capture; this does not alter the system's visual settings.
    QTest::qWait(200);
    flush();
    const QImage after = screen->grabWindow(0, captureRegion.x(), captureRegion.y(),
                                            captureRegion.width(), captureRegion.height()).toImage();
    QCOMPARE(after.size(), before.size());
    const QByteArray prefix = qgetenv("MAICHAT_CAPTURE_WINDOW_BOUNDARY");
    if (!prefix.isEmpty()) {
        QVERIFY(before.save(QString::fromUtf8(prefix) + QStringLiteral("-backdrop.png")));
        QVERIFY(after.save(QString::fromUtf8(prefix) + QStringLiteral("-window.png")));
    }
    BOOL rendered = FALSE;
    QCOMPARE(attribute(reinterpret_cast<HWND>(window.winId()), DWMWA_NCRENDERING_ENABLED,
                       &rendered, sizeof(rendered)), HRESULT(S_OK));
    QVERIFY2(rendered, "DWM non-client rendering is disabled for this frameless window");
    const qreal ratio = qreal(after.width()) / captureRegion.width();
    int darker = 0;
    // Only pixels outside the application's HWND count as shadow. A painted
    // client border or a shrunken content rectangle cannot satisfy this check.
    for (int y = qRound((padding + window.height() + 2) * ratio);
         y < qRound((padding + window.height() + 16) * ratio); ++y)
        for (int x = qRound((padding + 40) * ratio);
             x < qRound((padding + window.width() - 40) * ratio); ++x) {
            if (qGray(after.pixel(x, y)) + 2 < qGray(before.pixel(x, y))) ++darker;
        }
    qInfo() << "owned-window-shadow" << "outside darker pixels" << darker << "dpr" << ratio;
    QVERIFY2(darker > qRound(window.width() * ratio), "No soft shadow separates the owned window from its backdrop");
#else
    QSKIP("Other platforms retain their native window frame and shadow.");
#endif
}

void MainWindowLayoutTest::composerUsesEmbeddedIconSendAction() {
    auto client = std::make_unique<FakeRemoteIMClient>();
    RemoteIMApplication app(QStringLiteral("desktop-user"), std::move(client));
    app.addContact(QStringLiteral("phone-user"), QStringLiteral("iPhone"));

    MainWindow window(app);
    window.show();

    auto* editor = window.findChild<QTextEdit*>(QStringLiteral("messageEditor"));
    auto* sendButton = window.findChild<QPushButton*>(QStringLiteral("sendButton"));
    QVERIFY(editor != nullptr);
    QVERIFY(sendButton != nullptr);
    QTRY_VERIFY(editor->width() > sendButton->width());

    QCOMPARE(sendButton->parentWidget(), editor);
    QCOMPARE(sendButton->text(), QString());
    QVERIFY(!sendButton->icon().isNull());
    QCOMPARE(sendButton->toolTip(), QStringLiteral("发送消息"));
    QVERIFY(editor->rect().contains(sendButton->geometry().topLeft()));
    QVERIFY(editor->rect().contains(sendButton->geometry().bottomRight()));

    const int rightInset = editor->width() - sendButton->geometry().right() - 1;
    const int bottomInset = editor->height() - sendButton->geometry().bottom() - 1;
    QVERIFY(rightInset > 0);
    QVERIFY(bottomInset > 0);
    QVERIFY(rightInset <= sendButton->width() / 2);
    QVERIFY(bottomInset <= sendButton->height() / 2);

    const QList<QPushButton*> buttons = window.findChildren<QPushButton*>();
    for (const QPushButton* button : buttons) {
        QVERIFY(button->toolTip() != QStringLiteral("语音消息"));
    }
}

// AI 回复建议只填入输入框供用户审阅，不能自动发送或悄悄改变选中会话。
void MainWindowLayoutTest::aiReplySuggestionOnlyFillsComposer() {
    auto client = std::make_unique<FakeRemoteIMClient>();
    auto* fake = client.get();
    RemoteIMApplication app(QStringLiteral("desktop-user"), std::move(client));
    app.addContact(QStringLiteral("phone-user"), QStringLiteral("iPhone"));
    app.addContact(QStringLiteral("other-user"), QStringLiteral("Other"));
    app.selectPeer(QStringLiteral("phone-user"));
    fake->emitIncomingText(QStringLiteral("phone-user"), QStringLiteral("今天能审核完吗？"));
    const QString latestId = app.chatState().messagesWith(QStringLiteral("phone-user")).last().id;

    MainWindow window(app);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    auto* editor = window.findChild<QTextEdit*>(QStringLiteral("messageEditor"));
    auto* aiButton = window.findChild<QPushButton*>(QStringLiteral("aiReplyButton"));
    auto* aiHint = window.findChild<QLabel*>(QStringLiteral("aiReplyHint"));
    auto* bar = window.findChild<QWidget*>(QStringLiteral("replySuggestionBar"));
    auto* professional = window.findChild<QPushButton*>(QStringLiteral("professionalReplySuggestion"));
    auto* controller = window.findChild<ReplySuggestionController*>(QStringLiteral("replySuggestionController"));
    QVERIFY(editor != nullptr);
    QVERIFY(aiButton != nullptr);
    QVERIFY(aiHint != nullptr);
    QVERIFY(bar != nullptr);
    QVERIFY(professional != nullptr);
    QVERIFY(controller != nullptr);
    QCOMPARE(aiButton->parentWidget(), editor);
    QCOMPARE(aiButton->toolTip(), QString());
    QVERIFY(aiHint->isHidden());
    QVERIFY(editor->rect().contains(aiButton->geometry().bottomLeft()));

    QEvent enterEvent(QEvent::Enter);
    QApplication::sendEvent(aiButton, &enterEvent);
    QVERIFY(aiHint->isVisible());
    QVERIFY(aiHint->height() < aiButton->height());
    QVERIFY(aiHint->geometry().bottom() < aiButton->geometry().top());
    QEvent leaveEvent(QEvent::Leave);
    QApplication::sendEvent(aiButton, &leaveEvent);
    QVERIFY(aiHint->isHidden());

    aiButton->click();
    QVERIFY(bar->isVisible());
    QVERIFY(QMetaObject::invokeMethod(controller, "suggestionsReady", Qt::DirectConnection, Q_ARG(quint64, quint64(1)),
                                      Q_ARG(QString, QStringLiteral("desktop-user")),
                                      Q_ARG(QString, QStringLiteral("phone-user")), Q_ARG(QString, latestId),
                                      Q_ARG(QString, QStringLiteral("好，我晚点看")),
                                      Q_ARG(QString, QStringLiteral("收到，马上看 😄")),
                                      Q_ARG(QString, QStringLiteral("已收到，我会在今天完成审核。"))));
    QVERIFY(professional->isEnabled());
    QCOMPARE(fake->lastText(), QString());

    professional->click();
    QCOMPARE(editor->toPlainText(), QStringLiteral("已收到，我会在今天完成审核。"));
    QCOMPARE(fake->lastText(), QString());
    QVERIFY(bar->isHidden());
    app.selectPeer(QStringLiteral("other-user"));
    QCOMPARE(editor->toPlainText(), QString());
    app.selectPeer(QStringLiteral("phone-user"));
    QCOMPARE(editor->toPlainText(), QStringLiteral("已收到，我会在今天完成审核。"));
}

void MainWindowLayoutTest::conversationPreviewUsesPlainMarkdown_data() {
    QTest::addColumn<QString>("source");
    QTest::addColumn<QString>("expected");
    QTest::addColumn<bool>("attachment");
    QTest::newRow("heading-inline") << QStringLiteral("## 标题\n\n**重点** 与 `code`")
                                    << QStringLiteral("标题 重点 与 code") << false;
    QTest::newRow("link-strike") << QStringLiteral("[文档](https://example.com) 与 ~~旧版~~")
                                 << QStringLiteral("文档 与 旧版") << false;
    QTest::newRow("code-literals") << QStringLiteral("```cpp\na*b*c\npath_with_under_score\n```")
                                   << QStringLiteral("a*b*c path_with_under_score") << false;
    QTest::newRow("callout") << QStringLiteral("> [!TIP]\n> 使用 **独立目录**") << QStringLiteral("建议：使用 独立目录")
                             << false;
    QTest::newRow("same-line-preview-callout")
        << QStringLiteral("> [!TIP] 正文") << QStringLiteral("建议：正文") << false;
    QTest::newRow("emphasized-preview-callout")
        << QStringLiteral("> **[!TIP]** 正文") << QStringLiteral("建议：正文") << false;
    QTest::newRow("linked-literal-callout")
        << QStringLiteral("> [[!TIP]](https://example.com)") << QStringLiteral("[!TIP]") << false;
    QTest::newRow("tasks") << QStringLiteral("- [x] 完成\n- [ ] 待办") << QStringLiteral("☑ 完成 ☐ 待办") << false;
    QTest::newRow("table") << QStringLiteral("| A | B |\n|---|---|\n|1|2|") << QStringLiteral("A B 1 2") << false;
    QTest::newRow("reference-link") << QStringLiteral("[文档][ref]\n\n[ref]: https://example.com")
                                    << QStringLiteral("文档") << false;
    QTest::newRow("image-alt") << QStringLiteral("![替代文字](https://example.com/a.png)") << QStringLiteral("替代文字")
                               << false;
    QTest::newRow("inline-code-literals")
        << QStringLiteral("`**literal**` 与 `a_b`") << QStringLiteral("**literal** 与 a_b") << false;
    QTest::newRow("callout-in-code") << QStringLiteral("```text\n> [!TIP]\n```") << QStringLiteral("> [!TIP]") << false;
    QTest::newRow("inline-callout-code") << QStringLiteral("> `[!TIP]`") << QStringLiteral("[!TIP]") << false;
    QTest::newRow("crlf") << QStringLiteral("## 标题\r\n\r\n下一段") << QStringLiteral("标题 下一段") << false;
    QTest::newRow("attachment") << QStringLiteral("[文件消息] **report**.md")
                                << QStringLiteral("[文件消息] **report**.md") << true;
    QTest::newRow("only-markup") << QStringLiteral("---") << QStringLiteral("新消息") << false;
    const QString emoji = QString::fromUtf8("👨‍👩‍👧‍👦");
    QTest::newRow("long-emoji") << emoji.repeated(170) << (emoji.repeated(160) + QStringLiteral("…")) << false;
    for (const QString& sequence : {emoji, QString::fromUtf8("👩🏽‍💻"), QString::fromUtf8("❤️"),
                                    QString::fromUtf8("1️⃣"), QString::fromUtf8("🇨🇳")}) {
        const QByteArray name = QByteArray("emoji-after-159-") + sequence.toUtf8().toHex();
        QTest::newRow(name.constData()) << (QString(159, QLatin1Char('a')) + sequence + sequence)
                                        << (QString(159, QLatin1Char('a')) + sequence + QStringLiteral("…")) << false;
    }
}

void MainWindowLayoutTest::conversationPreviewUsesPlainMarkdown() {
    QFETCH(QString, source);
    QFETCH(QString, expected);
    QFETCH(bool, attachment);
    auto client = std::make_unique<FakeRemoteIMClient>();
    RemoteIMApplication app(QStringLiteral("desktop-user"), std::move(client));
    app.addContact(QStringLiteral("phone-user"), QStringLiteral("iPhone"));
    RemoteIMMessage message;
    message.fromUserId = QStringLiteral("phone-user");
    message.toUserId = QStringLiteral("desktop-user");
    message.text = source;
    message.hasFile = attachment;
    app.chatState().appendMessageForRestore(message);
    MainWindow window(app);
    auto* list = window.findChild<QListWidget*>(QStringLiteral("conversationList"));
    QVERIFY(list != nullptr);
    QCOMPARE(list->count(), 1);
    const QString actual = list->item(0)->data(Qt::UserRole + 2).toString();
    if (actual != expected) {
        QStringList tail;
        const auto codepoints = actual.toUcs4();
        for (int i = qMax(0, codepoints.size() - 12); i < codepoints.size(); ++i) {
            tail.append(QString::number(codepoints.at(i), 16));
        }
        qInfo().noquote() << "preview mismatch: actual UTF16=" << actual.size() << "expected UTF16=" << expected.size()
                          << "actual ZWJ=" << actual.count(QChar(0x200D))
                          << "expected ZWJ=" << expected.count(QChar(0x200D))
                          << "actual tail scalars=" << tail.join(QLatin1Char(','));
    }
    QCOMPARE(actual, expected);
    RemoteIMMessage stored;
    QVERIFY(app.chatState().latestMessageWith(QStringLiteral("phone-user"), &stored));
    QCOMPARE(stored.text, source);
    // 通过真实列表刷新走缓存路径，不只调用辅助函数。
    emit app.stateChanged();
    QCOMPARE(list->item(0)->data(Qt::UserRole + 2).toString(), expected);
}

void MainWindowLayoutTest::compactSplittersKeepDragTargets_data() {
    QTest::addColumn<QString>("name");
    QTest::addColumn<qreal>("zoom");
    for (const char* name : {"contentSplitter", "messageComposerSplitter"}) {
        for (const qreal zoom : {0.8, 1.0, 1.5, 2.0}) {
            const QByteArray row = QByteArray(name) + '-' + QByteArray::number(zoom);
            QTest::newRow(row.constData()) << QString::fromLatin1(name) << zoom;
        }
    }
}

void MainWindowLayoutTest::compactSplittersKeepDragTargets() {
    QFETCH(QString, name);
    QFETCH(qreal, zoom);
    struct RestoreZoom {
        qreal old;
        ~RestoreZoom() {
            UiZoom::setFactor(old);
        }
    } restore{UiZoom::factor()};
    UiZoom::setFactor(zoom);
    auto client = std::make_unique<FakeRemoteIMClient>();
    RemoteIMApplication app(QStringLiteral("desktop-user"), std::move(client));
    MainWindow window(app);
    window.resize(1800, 1200);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    auto* splitter = window.findChild<QSplitter*>(name);
    QVERIFY(splitter != nullptr);
    QCOMPARE(splitter->count(), 2);
    const bool horizontal = splitter->orientation() == Qt::Horizontal;
    const QRect first = splitter->widget(0)->geometry();
    const QRect second = splitter->widget(1)->geometry();
    const int gap = horizontal ? second.left() - first.right() - 1 : second.top() - first.bottom() - 1;
    QVERIFY2(gap <= UiZoom::s(1), qPrintable(QStringLiteral("visible splitter gap=%1").arg(gap)));
    QVERIFY(gap > 0);
    auto* handle = splitter->handle(1);
    QVERIFY(handle != nullptr);
    // Qt 会把 1px 分隔条的鼠标区域扩展到相邻控件；
    // 可见细线不能因此变成只有一像素宽的拖动目标。
    const int grabSize = horizontal ? handle->width() : handle->height();
    QVERIFY(grabSize > gap);
    const int before = splitter->sizes().at(0);
    const QPoint start = handle->rect().center();
    const QPoint finish = start + (horizontal ? QPoint(40, 0) : QPoint(0, -40));
    QTest::mousePress(handle, Qt::LeftButton, Qt::NoModifier, start);
    QMouseEvent move(QEvent::MouseMove, QPointF(finish), QPointF(handle->mapToGlobal(finish)), Qt::NoButton,
                     Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(handle, &move);
    QTest::mouseRelease(handle, Qt::LeftButton, Qt::NoModifier, finish);
    QVERIFY2(splitter->sizes().at(0) != before, "The thin splitter must remain draggable");
}

void MainWindowLayoutTest::exposesResizableSplitters() {
    auto client = std::make_unique<FakeRemoteIMClient>();
    RemoteIMApplication app(QStringLiteral("desktop-user"), std::move(client));
    app.addContact(QStringLiteral("phone-user"), QStringLiteral("iPhone"));

    MainWindow window(app);

    auto* contentSplitter = window.findChild<QSplitter*>(QStringLiteral("contentSplitter"));
    auto* messageComposerSplitter = window.findChild<QSplitter*>(QStringLiteral("messageComposerSplitter"));
    QVERIFY(contentSplitter != nullptr);
    QVERIFY(messageComposerSplitter != nullptr);
    QCOMPARE(contentSplitter->orientation(), Qt::Horizontal);
    QCOMPARE(messageComposerSplitter->orientation(), Qt::Vertical);
    QVERIFY(contentSplitter->childrenCollapsible() == false);
    QVERIFY(messageComposerSplitter->childrenCollapsible() == false);
}

void MainWindowLayoutTest::agentConversationListUsesResizableSplitter() {
    auto client = std::make_unique<FakeRemoteIMClient>();
    RemoteIMApplication app(QStringLiteral("desktop-user"), std::move(client));
    MainWindow window(app);
    window.resize(1400, 900);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));

    auto* agentButton = window.findChild<QPushButton*>(QStringLiteral("agentNavButton"));
    QVERIFY(agentButton != nullptr);
    agentButton->click();

    auto* splitter = window.findChild<QSplitter*>(QStringLiteral("agentContentSplitter"));
    QVERIFY(splitter != nullptr);
    QCOMPARE(splitter->orientation(), Qt::Horizontal);
    QCOMPARE(splitter->count(), 2);
    QVERIFY(!splitter->childrenCollapsible());
    QCOMPARE(splitter->handleWidth(), 1);

    QSplitterHandle* handle = splitter->handle(1);
    QVERIFY(handle != nullptr);
    const int before = splitter->sizes().at(0);
    const QPoint start = handle->rect().center();
    const QPoint finish = start + QPoint(60, 0);
    QTest::mousePress(handle, Qt::LeftButton, Qt::NoModifier, start);
    QMouseEvent move(QEvent::MouseMove, QPointF(finish), QPointF(handle->mapToGlobal(finish)), Qt::NoButton,
                     Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(handle, &move);
    QTest::mouseRelease(handle, Qt::LeftButton, Qt::NoModifier, finish);
    QVERIFY(splitter->sizes().at(0) != before);
}

void MainWindowLayoutTest::agentCaptureStopEntryLivesAboveBothConversationPanes() {
    RemoteIMApplication app("creation-public-capture", std::make_unique<FakeRemoteIMClient>());
    MainWindow window(app);
    auto* panel = window.findChild<AgentChatPanel*>();
    auto* splitter = window.findChild<QSplitter*>(QStringLiteral("agentContentSplitter"));
    auto* bar = window.findChild<QWidget*>(QStringLiteral("agentScreenCaptureStatus"));
    QVERIFY(panel && splitter);
    auto* page = splitter->parentWidget();
    QVERIFY(page);
    QVERIFY2(bar, "Creation Center has no common screen capture stop entry");
    QVERIFY2(!panel->isAncestorOf(bar), "Process-wide screen capture controls must not live inside a conversation");
    QCOMPARE(bar->parentWidget(), page);
    QCOMPARE(splitter->parentWidget(), page);
    QCOMPARE(page->layout()->indexOf(bar), 0);
    QCOMPARE(page->layout()->indexOf(splitter), 1);
    QCOMPARE(window.findChildren<QWidget*>(QStringLiteral("agentScreenCaptureStatus")).size(), 1);
    auto* control = bar->findChild<DesktopScreenCaptureControl*>();
    QVERIFY(control);
    QCOMPARE(control->parent(), bar);
}

void MainWindowLayoutTest::agentCommonCaptureStopSurvivesSessionAndOwnerPageChanges() {
    struct CaptureProbe {
        std::mutex mutex;
        MaiScreenshotCaptureState state{true, true, 1, 1, 1, 1};
        std::atomic<int> stops{0};
    };
    auto probe = std::make_shared<CaptureProbe>();
    DesktopScreenCaptureControl control({
        [probe] {
            std::lock_guard<std::mutex> lock(probe->mutex);
            return probe->state;
        },
        [probe] {
            ++probe->stops;
            std::lock_guard<std::mutex> lock(probe->mutex);
            probe->state.workerRunning = false;
            probe->state.capturing = false;
            return MaiError{};
        }});
    QTemporaryDir workspace;
    QVERIFY(workspace.isValid());
    const QString owner = QStringLiteral("capture-common-%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
    RemoteIMApplication app(owner, std::make_unique<FakeRemoteIMClient>());
    MainWindow window(app, control);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    auto* navigation = window.findChild<QPushButton*>(QStringLiteral("agentNavButton"));
    auto* splitter = window.findChild<QSplitter*>(QStringLiteral("agentContentSplitter"));
    QPointer<QWidget> bar = window.findChild<QWidget*>(QStringLiteral("agentScreenCaptureStatus"));
    QVERIFY(navigation && splitter && bar);
    navigation->click();
    auto* stop = bar->findChild<QPushButton*>(QStringLiteral("agentStopScreenCapture"));
    auto* label = bar->findChild<QLabel*>(QStringLiteral("agentScreenCaptureLabel"));
    QVERIFY(stop && label);
    QTRY_VERIFY(bar->isVisible() && stop->isVisible() && stop->isEnabled());
    QCOMPARE(bar->parentWidget(), splitter->parentWidget());
    QCOMPARE(bar->width(), splitter->width());
    QVERIFY(bar->geometry().bottom() < splitter->geometry().top());
    QVERIFY(label->text().contains(QStringLiteral("本机")));
    QCOMPARE(control.parent(), nullptr);

    auto* controller = window.activeAgentController();
    auto* list = window.findChild<AgentSessionList*>();
    auto* panel = window.findChild<AgentChatPanel*>();
    QVERIFY(controller && list && panel);
    const QString first = controller->createSession(workspace.path());
    const QString second = controller->createSession(workspace.path());
    QVERIFY(!first.isEmpty() && !second.isEmpty());
    list->refresh();
    auto* rows = list->findChild<QListWidget*>();
    QVERIFY(rows);
    const auto clickLocal = [&](const QString& id) {
        for (int row = 0; row < rows->count(); ++row) {
            auto* item = rows->item(row);
            if (item->data(Qt::UserRole).toString() != id) continue;
            rows->scrollToItem(item);
            QTest::mouseClick(rows->viewport(), Qt::LeftButton, Qt::NoModifier, rows->visualItemRect(item).center());
            return true;
        }
        return false;
    };
    QVERIFY(clickLocal(first));
    auto* editor = panel->findChild<QTextEdit*>(QStringLiteral("agentPromptEditor"));
    QVERIFY(editor);
    editor->setPlainText(QStringLiteral("first local draft remains private"));
    QVERIFY(clickLocal(second));
    editor->setPlainText(QStringLiteral("second local draft remains private"));
    QVERIFY(clickLocal(first));
    QCOMPARE(editor->toPlainText(), QStringLiteral("first local draft remains private"));
    QCOMPARE(window.findChild<QWidget*>(QStringLiteral("agentScreenCaptureStatus")), bar.data());
    QVERIFY(bar->isVisible());
    const QString capturePrefix = qEnvironmentVariable("MAICHAT_CAPTURE_CREATION_PUBLIC");
    if (!capturePrefix.isEmpty())
        QVERIFY(window.grab().save(capturePrefix + QStringLiteral("-local.png")));

    DesktopSessionSyncCoordinator coordinator(
        [](const QString&, const QByteArray&, const QJsonObject&,
           DesktopSessionSyncCoordinator::Completion completion) {
            QTimer::singleShot(0, [completion = std::move(completion)] { completion({}, {}); });
        }, [&] { return window.activeAgentController(); });
    window.setSessionSyncCoordinator(&coordinator);
    const QJsonObject remote{{QStringLiteral("id"), QStringLiteral("owned-remote-fixture")},
                            {QStringLiteral("title"), QStringLiteral("Owned remote fixture")},
                            {QStringLiteral("device_slot"), QStringLiteral("ios_phone")},
                            {QStringLiteral("device_id"), QStringLiteral("owned-phone")},
                            {QStringLiteral("login_session_id"), QStringLiteral("owned-login")},
                            {QStringLiteral("online"), true}};
    coordinator.acceptCatalog(QJsonArray{remote});
    QVERIFY(QMetaObject::invokeMethod(list, "remoteSelected", Qt::DirectConnection, Q_ARG(QJsonObject, remote)));
    auto* panelStack = qobject_cast<QStackedWidget*>(panel->parentWidget());
    QVERIFY(panelStack);
    auto* remotePanel = dynamic_cast<RemoteAgentPanel*>(panelStack->currentWidget());
    QVERIFY(remotePanel && remotePanel->isVisible());
    QCOMPARE(remotePanel->session().value(QStringLiteral("id")).toString(), QStringLiteral("owned-remote-fixture"));
    QVERIFY(!remotePanel->isAncestorOf(bar));
    QVERIFY(bar->isVisible() && label->text().contains(QStringLiteral("本机")));
    QCOMPARE(window.findChild<QWidget*>(QStringLiteral("agentScreenCaptureStatus")), bar.data());
    if (!capturePrefix.isEmpty())
        QVERIFY(window.grab().save(capturePrefix + QStringLiteral("-remote.png")));
    QTest::mouseClick(stop, Qt::LeftButton);
    QTRY_COMPARE(probe->stops.load(), 1);
    QTRY_VERIFY(!control.isStopping() && !control.captureState().capturing && !control.captureState().workerRunning);
    QVERIFY(clickLocal(first));
    QCOMPARE(editor->toPlainText(), QStringLiteral("first local draft remains private"));
    QVERIFY(clickLocal(second));
    QCOMPARE(editor->toPlainText(), QStringLiteral("second local draft remains private"));
    {
        std::lock_guard<std::mutex> lock(probe->mutex);
        probe->state.workerRunning = probe->state.capturing = true;
        ++probe->state.sessionStarts;
    }
    control.refresh();
    QTRY_VERIFY(bar->isVisible() && stop->isEnabled());

    // This existing setter really rebuilds the local/remote owner pages. Draft
    // retention across that older destructive API is not changed here; the new
    // process-level control and public bar must remain the same live objects.
    QPointer<AgentChatPanel> oldPanel = panel;
    window.setMediaServiceAccessProvider({});
    QVERIFY(oldPanel.isNull());
    QVERIFY(window.findChild<AgentChatPanel*>());
    QVERIFY(bar);
    QCOMPARE(window.findChild<QWidget*>(QStringLiteral("agentScreenCaptureStatus")), bar.data());
    QCOMPARE(window.findChildren<QWidget*>(QStringLiteral("agentScreenCaptureStatus")).size(), 1);
    QCOMPARE(control.parent(), nullptr);
    QCOMPARE(probe->stops.load(), 1);
    QVERIFY(control.captureState().capturing);
    QTRY_VERIFY(bar->isVisible());
    QTest::mouseClick(stop, Qt::LeftButton);
    QTRY_COMPARE(probe->stops.load(), 2);
    QTRY_VERIFY(!control.isStopping() && !control.captureState().capturing);
    window.setSessionSyncCoordinator(nullptr);
}

void MainWindowLayoutTest::agentSessionSwitchReusesTheExistingPage() {
    ScopedMessageHeaderFont productFonts;
    QTemporaryDir workspace;
    QVERIFY(workspace.isValid());
    const QString owner = QStringLiteral("switch-fixture-%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
    const QString dataRoot = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    const QString accountHash =
        QString::fromLatin1(QCryptographicHash::hash(owner.toUtf8(), QCryptographicHash::Sha256).toHex());
    const QString accountRoot = QDir(dataRoot).filePath(QStringLiteral("Agent/Accounts/%1").arg(accountHash));
    QVERIFY(QDir().mkpath(accountRoot));
    const QStringList ids{QStringLiteral("ses_window_switch_a"), QStringLiteral("ses_window_switch_b"),
                          QStringLiteral("ses_window_switch_c")};
    {
        auto opened = makeMaiSqliteStore(QDir(accountRoot).filePath(QStringLiteral("agent.db")).toStdString());
        QVERIFY(opened);
        auto store = std::move(opened.value());
        for (int sessionIndex = 0; sessionIndex < ids.size(); ++sessionIndex) {
            MaiSession session;
            session.id = ids[sessionIndex].toStdString();
            session.title = QStringLiteral("本地回归会话 %1").arg(sessionIndex + 1).toStdString();
            session.directory = workspace.path().toStdString();
            session.created = session.updated = 100 + sessionIndex;
            store->putSession(session);
            for (int index = 0; index < 40; ++index) {
                MaiMessage message;
                message.id = QStringLiteral("msg_%1_%2")
                                 .arg(ids[sessionIndex])
                                 .arg(index, 4, 10, QLatin1Char('0'))
                                 .toStdString();
                message.role = index % 2 ? MaiRole::Assistant : MaiRole::User;
                message.created = message.completed = index + 1;
                const QString body =
                    message.role == MaiRole::User
                        ? QStringLiteral("请查看这个本地回归结果 %1").arg(index)
                        : QStringLiteral("## 已完成检查 %1\n\n").arg(index) +
                              QStringLiteral("这段长正文用于检查会话切换，包含 **格式化文字** 与 `code`。\n\n")
                                  .repeated(8) +
                              QStringLiteral(
                                  "| 检查项 | 结果 |\n| --- | --- |\n| 消息隔离 | 通过 |\n| 历史内容 | 保留 |\n");
                message.parts.push_back({"prt_" + message.id, MaiTextPart{body.toStdString()}, index + 1});
                if (message.role == MaiRole::Assistant)
                    for (int toolIndex = 0; toolIndex < 12; ++toolIndex) {
                        MaiToolPart tool;
                        tool.callId = "call_" + message.id + "_" + std::to_string(toolIndex);
                        tool.tool = "shell";
                        tool.input = R"({"command":"local fixture"})";
                        tool.output = std::string(4096, 'x');
                        tool.state = MaiToolState::Completed;
                        message.parts.push_back({tool.callId, std::move(tool), index + 1});
                    }
                store->putMessage(session.id, message);
            }
        }
        QVERIFY(!store->lastWriteError().hasError());
    }
    {
        RemoteIMApplication app(owner, std::make_unique<FakeRemoteIMClient>());
        MainWindow window(app);
        window.resize(1300, 850);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        auto* navigation = window.findChild<QPushButton*>(QStringLiteral("agentNavButton"));
        auto* panel = window.findChild<AgentChatPanel*>();
        auto* sessionList = window.findChild<AgentSessionList*>();
        auto* rows = sessionList ? sessionList->findChild<QListWidget*>() : nullptr;
        QVERIFY(navigation && panel && rows);
        navigation->click();
        const auto clickSession = [&](const QString& id) {
            for (int row = 0; row < rows->count(); ++row) {
                auto* item = rows->item(row);
                if (item->data(Qt::UserRole).toString() != id) continue;
                rows->scrollToItem(item);
                QTest::mouseClick(rows->viewport(), Qt::LeftButton, Qt::NoModifier, rows->visualItemRect(item).center(),
                                  0);
                return true;
            }
            return false;
        };
        QVERIFY(clickSession(ids[0]));
        QCoreApplication::processEvents();
        QPointer<QPushButton> original;
        const QString lastId = QStringLiteral("msg_%1_0039").arg(ids[0]);
        for (QPushButton* quote : panel->findChildren<QPushButton*>(QStringLiteral("agentQuoteAction")))
            if (quote->isVisible() && quote->property("messageId").toString() == lastId) original = quote;
        QVERIFY(original);
        QVERIFY(clickSession(ids[1]));
        QCoreApplication::processEvents();
        QVERIFY(clickSession(ids[2]));
        QCoreApplication::processEvents();
        QElapsedTimer timer;
        timer.start();
        QVERIFY(clickSession(ids[0]));
        const qint64 clickUs = timer.nsecsElapsed() / 1000;
        QCoreApplication::processEvents();
        qInfo("main_window_switch_fixture click_us=%lld first_paint_us=%lld", clickUs, timer.nsecsElapsed() / 1000);
        QCOMPARE(panel->sessionId(), ids[0]);
        QVERIFY(original && original->isVisible());
        const QString output = qEnvironmentVariable("MAICHAT_SESSION_SWITCH_CAPTURE_DIR");
        if (!output.isEmpty()) {
            QVERIFY(QDir().mkpath(output));
            QVERIFY(window.grab().save(QDir(output).filePath(QStringLiteral("session-switch-main-window.png"))));
        }
    }
    // Delete only this freshly generated, canonical fixture account directory.
    const QString canonical = QFileInfo(accountRoot).canonicalFilePath();
    const QString allowed = QDir(dataRoot).canonicalPath() + QStringLiteral("/Agent/Accounts/");
    QVERIFY(!canonical.isEmpty() && canonical.startsWith(allowed) && QFileInfo(canonical).fileName() == accountHash);
    QVERIFY(QDir(canonical).removeRecursively());
}

void MainWindowLayoutTest::localSessionCreationLivesInTheDeviceHeader_data() {
    QTest::addColumn<qreal>("zoom");
    for (const qreal zoom : {0.8, 1.0, 1.5, 2.0})
        QTest::newRow(qPrintable(QString::number(zoom))) << zoom;
}

void MainWindowLayoutTest::localSessionCreationLivesInTheDeviceHeader() {
    QFETCH(qreal, zoom);
    struct RestoreZoom {
        qreal previous = UiZoom::factor();
        ~RestoreZoom() { UiZoom::setFactor(previous); }
    } restore;
    UiZoom::setFactor(zoom);
    AgentController controller(AgentController::ModelConfig{}, QString());
    QTemporaryDir workspace;
    QVERIFY(workspace.isValid());
    AgentSessionList sidebar(controller);
    sidebar.resize(UiZoom::s(320), UiZoom::s(350));
    sidebar.refresh();
    sidebar.show();
    QVERIFY(QTest::qWaitForWindowExposed(&sidebar));
    auto* rows = sidebar.findChild<QListWidget*>();
    auto* create = sidebar.findChild<QPushButton*>(QStringLiteral("agentLocalCreateSessionButton"));
    QVERIFY(rows && create);
    // There is one list, with no detached full-width new-conversation row above it.
    QCOMPARE(sidebar.layout()->count(), 1);
    QCOMPARE(create->parentWidget(), rows->viewport());
    QCOMPARE(rows->geometry().top(), 0);
    QCOMPARE(rows->item(0)->data(Qt::UserRole + 3).toString(), QStringLiteral("local"));
    QCOMPARE(rows->item(0)->data(Qt::UserRole + 5).toInt(), 0);
    QCOMPARE(create->text(), QStringLiteral("+"));
    QVERIFY(!create->accessibleName().isEmpty());
    QVERIFY(!create->toolTip().isEmpty());
    const auto verifyPosition = [&] {
        const QRect header = rows->visualItemRect(rows->item(0));
        const QRect card = header.adjusted(UiZoom::s(7), UiZoom::s(3), -UiZoom::s(7), -UiZoom::s(3));
        const QRect expected(card.right() - UiZoom::s(110), card.center().y() - UiZoom::s(13),
                             UiZoom::s(24), UiZoom::s(27));
        QCOMPARE(create->geometry(), expected);
        QVERIFY(header.contains(create->geometry()));
        QVERIFY(create->isVisible());
    };
    verifyPosition();
    QSignalSpy localRequests(&sidebar, &AgentSessionList::newSessionRequested);
    QSignalSpy remoteRequests(&sidebar, &AgentSessionList::remoteCreateRequested);
    QTest::mouseClick(create, Qt::LeftButton);
    QCOMPARE(localRequests.count(), 1);
    QCOMPARE(remoteRequests.count(), 0);
    QVERIFY(!rows->item(0)->data(Qt::UserRole + 8).toBool());
    create->setFocus();
    QTest::keyClick(create, Qt::Key_Space);
    QCOMPARE(localRequests.count(), 2);

    // New local sessions, collapse/expand and catalog refresh reuse the header action.
    const QString id = controller.createSession(workspace.path());
    QVERIFY(!id.isEmpty());
    QPointer<QPushButton> original(create);
    sidebar.refresh();
    QCOMPARE(sidebar.findChild<QPushButton*>(QStringLiteral("agentLocalCreateSessionButton")), original.data());
    QTest::mouseClick(rows->viewport(), Qt::LeftButton, Qt::NoModifier,
                      rows->visualItemRect(rows->item(0)).topLeft() + QPoint(UiZoom::s(35), UiZoom::s(21)));
    QVERIFY(rows->item(0)->data(Qt::UserRole + 8).toBool());
    QTest::mouseClick(create, Qt::LeftButton);
    QCOMPARE(localRequests.count(), 3);
    QVERIFY(rows->item(0)->data(Qt::UserRole + 8).toBool());
    verifyPosition();
    sidebar.resize(UiZoom::s(400), UiZoom::s(350));
    QCoreApplication::processEvents();
    verifyPosition();

    const QJsonObject remote{{QStringLiteral("device_slot"), QStringLiteral("ios_phone")},
                             {QStringLiteral("device_id"), QStringLiteral("header-test-phone")},
                             {QStringLiteral("device_name"), QStringLiteral("iPhone")},
                             {QStringLiteral("online"), true}};
    sidebar.setRemoteDevices(QJsonArray{remote});
    QListWidgetItem* remoteHeader = nullptr;
    for (int row = 0; row < rows->count(); ++row)
        if (rows->item(row)->data(Qt::UserRole + 7).toBool()) remoteHeader = rows->item(row);
    QVERIFY(remoteHeader);
    const QRect remoteCard = rows->visualItemRect(remoteHeader).adjusted(
        UiZoom::s(7), UiZoom::s(3), -UiZoom::s(7), -UiZoom::s(3));
    const QPoint remotePlus(remoteCard.right() - UiZoom::s(110) + UiZoom::s(12), remoteCard.center().y());
    QTest::mouseClick(rows->viewport(), Qt::LeftButton, Qt::NoModifier, remotePlus);
    QCOMPARE(remoteRequests.count(), 1);
    QCOMPARE(localRequests.count(), 3);
    QVERIFY(!remoteHeader->data(Qt::UserRole + 8).toBool());
    // A scrolled-out local header must not leave its floating action over remote rows.
    for (int index = 0; index < 12; ++index) QVERIFY(!controller.createSession(workspace.path()).isEmpty());
    QTest::mouseClick(rows->viewport(), Qt::LeftButton, Qt::NoModifier,
                      rows->visualItemRect(rows->item(0)).topLeft() + QPoint(UiZoom::s(35), UiZoom::s(21)));
    QVERIFY(!rows->item(0)->data(Qt::UserRole + 8).toBool());
    sidebar.refresh();
    rows->verticalScrollBar()->setValue(rows->verticalScrollBar()->maximum());
    QTRY_VERIFY(!create->isVisible());
    rows->verticalScrollBar()->setValue(0);
    QTRY_VERIFY(create->isVisible());
    verifyPosition();
    QCOMPARE(localRequests.count(), 3);
    QCOMPARE(remoteRequests.count(), 1);
    const QString output = qEnvironmentVariable("MAICHAT_SESSION_HEADER_CAPTURE_DIR");
    if (!output.isEmpty()) {
        QVERIFY(QDir().mkpath(output));
        QVERIFY(sidebar.grab().save(QDir(output).filePath(QStringLiteral("session-header-%1.png").arg(zoom))));
    }
}

void MainWindowLayoutTest::agentAccountsUseDistinctDefaultStorage() {
    const QString suffix = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString ownerA = QStringLiteral("storage-a-%1").arg(suffix);
    const QString ownerB = QStringLiteral("storage-b-%1").arg(suffix);
    const QString dataRoot = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    QVERIFY(!dataRoot.isEmpty());
    const auto accountRoot = [&dataRoot](const QString& owner) {
        const QString hash =
            QString::fromLatin1(QCryptographicHash::hash(owner.toUtf8(), QCryptographicHash::Sha256).toHex());
        return QDir(dataRoot).filePath(QStringLiteral("Agent/Accounts/%1").arg(hash));
    };
    const QString rootA = accountRoot(ownerA);
    const QString rootB = accountRoot(ownerB);
    QString sessionA;
    QString sessionB;
    {
        RemoteIMApplication app(ownerA, std::make_unique<FakeRemoteIMClient>());
        MainWindow window(app);
        auto* panel = window.findChild<AgentChatPanel*>();
        QVERIFY(panel);
        panel->openSession();
        sessionA = panel->sessionId();
        QVERIFY(!sessionA.isEmpty());
        auto* controller = window.findChild<AgentController*>();
        QVERIFY(controller);
        MaiSession session;
        QVERIFY(controller->agent().getSession(sessionA.toStdString(), session));
        QCOMPARE(QString::fromStdString(session.directory),
                 QDir(QDir(rootA).filePath(QStringLiteral("Workspace"))).canonicalPath());
        QVERIFY(QFileInfo(QDir(rootA).filePath(QStringLiteral("agent.db"))).isFile());
    }
    {
        RemoteIMApplication app(ownerB, std::make_unique<FakeRemoteIMClient>());
        MainWindow window(app);
        auto* panel = window.findChild<AgentChatPanel*>();
        QVERIFY(panel);
        panel->openSession();
        sessionB = panel->sessionId();
        QVERIFY(!sessionB.isEmpty());
        auto* controller = window.findChild<AgentController*>();
        QVERIFY(controller);
        MaiSession session;
        QVERIFY(controller->agent().getSession(sessionB.toStdString(), session));
        QCOMPARE(QString::fromStdString(session.directory),
                 QDir(QDir(rootB).filePath(QStringLiteral("Workspace"))).canonicalPath());
        QVERIFY(QFileInfo(QDir(rootB).filePath(QStringLiteral("agent.db"))).isFile());
        QVERIFY(!controller->agent().getSession(sessionA.toStdString(), session));
    }
    QVERIFY(sessionA != sessionB);
    QDir(rootA).removeRecursively();
    QDir(rootB).removeRecursively();
}

void MainWindowLayoutTest::agentSessionDeletionUsesChineseProductDialog() {
    const QString owner = QStringLiteral("delete-dialog-%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
    const QString dataRoot = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    const QString accountHash =
        QString::fromLatin1(QCryptographicHash::hash(owner.toUtf8(), QCryptographicHash::Sha256).toHex());
    const QString accountRoot = QDir(dataRoot).filePath(QStringLiteral("Agent/Accounts/%1").arg(accountHash));

    {
        RemoteIMApplication app(owner, std::make_unique<FakeRemoteIMClient>());
        MainWindow window(app);
        window.resize(1200, 800);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        auto* navigation = window.findChild<QPushButton*>(QStringLiteral("agentNavButton"));
        auto* panel = window.findChild<AgentChatPanel*>();
        auto* list = window.findChild<AgentSessionList*>();
        auto* controller = window.findChild<AgentController*>();
        QVERIFY(navigation != nullptr);
        QVERIFY(panel != nullptr);
        QVERIFY(list != nullptr);
        QVERIFY(controller != nullptr);
        navigation->click();
        panel->openSession();
        const QString sessionId = panel->sessionId();
        QVERIFY(!sessionId.isEmpty());
        MaiSession session;
        QVERIFY(controller->agent().getSession(sessionId.toStdString(), session));
        QVERIFY(session.isUntitled());

        struct DialogState {
            bool shown = false;
            QString title;
            QString body;
            QString cancel;
            QString deleteText;
            bool cancelDefault = false;
            bool nativeQuestionBox = false;
            int buttonGap = -1;
        } state;
        const auto requestDelete = [&](bool confirm) {
            state = {};
            QTimer::singleShot(80, &window, [&] {
                QWidget* modal = QApplication::activeModalWidget();
                state.nativeQuestionBox = qobject_cast<QMessageBox*>(modal) != nullptr;
                auto* dialog = qobject_cast<AppMessageDialog*>(modal);
                if (dialog == nullptr) return;
                state.shown = true;
                auto* title = dialog->findChild<QLabel*>(QStringLiteral("appMessageTitle"));
                auto* body = dialog->findChild<QLabel*>(QStringLiteral("appMessageBody"));
                auto* cancel = dialog->findChild<QPushButton*>(QStringLiteral("appMessageCancel"));
                auto* remove = dialog->findChild<QPushButton*>(QStringLiteral("appMessageDanger"));
                state.title = title ? title->text() : QString();
                state.body = body ? body->text() : QString();
                state.cancel = cancel ? cancel->text() : QString();
                state.deleteText = remove ? remove->text() : QString();
                state.cancelDefault = cancel != nullptr && cancel->isDefault();
                if (cancel != nullptr && remove != nullptr)
                    state.buttonGap = remove->geometry().left() - cancel->geometry().right() - 1;
                const QString screenshot = qEnvironmentVariable("MAICHAT_DELETE_DIALOG_SCREENSHOT");
                if (!confirm && !screenshot.isEmpty()) dialog->grab().save(screenshot);
                if (confirm && remove)
                    remove->click();
                else if (cancel)
                    cancel->click();
                else
                    dialog->reject();
            });
            return QMetaObject::invokeMethod(list, "deleteSessionRequested", Qt::DirectConnection,
                                             Q_ARG(QString, sessionId));
        };

        QVERIFY(requestDelete(false));
        QVERIFY(state.shown);
        QVERIFY(!state.nativeQuestionBox);
        QCOMPARE(state.title, QStringLiteral("删除对话"));
        QVERIFY(state.body.contains(QStringLiteral("新对话")));
        QVERIFY(!state.body.contains(QStringLiteral("New session")));
        QCOMPARE(state.cancel, QStringLiteral("取消"));
        QCOMPARE(state.deleteText, QStringLiteral("删除"));
        QVERIFY(state.cancelDefault);
        QVERIFY(state.buttonGap >= UiZoom::s(8));
        QVERIFY(controller->agent().getSession(sessionId.toStdString(), session));

        QVERIFY(requestDelete(true));
        QVERIFY(state.shown);
        QTRY_VERIFY(!controller->agent().getSession(sessionId.toStdString(), session));
    }
    QDir(accountRoot).removeRecursively();
}

void MainWindowLayoutTest::remoteSessionLifecycleUsesProductDialogsAndOwnerRoute() {
    const QString owner = QStringLiteral("remote-lifecycle-%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
    RemoteIMApplication app(owner, std::make_unique<FakeRemoteIMClient>());
    MainWindow window(app);
    window.resize(1200, 800);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    auto* list = window.findChild<AgentSessionList*>();
    QVERIFY(list);
    auto* navigation = window.findChild<QPushButton*>(QStringLiteral("agentNavButton"));
    QVERIFY(navigation);
    navigation->click();
    QList<QJsonObject> sent;
    DesktopSessionSyncCoordinator coordinator(
        [&](const QString& path, const QByteArray&, const QJsonObject& body,
            DesktopSessionSyncCoordinator::Completion completion) {
            if (path == QStringLiteral("session-sync/send")) sent.append(body);
            QTimer::singleShot(0, [completion = std::move(completion)] { completion({}, {}); });
        },
        [&] { return window.activeAgentController(); }, nullptr, QStringLiteral("mac-install"), QStringLiteral("Mac"));
    window.setSessionSyncCoordinator(&coordinator);
    const QJsonObject device{{QStringLiteral("device_slot"), QStringLiteral("ios_phone")},
                             {QStringLiteral("device_name"), QStringLiteral("iPhone")},
                             {QStringLiteral("device_id"), QStringLiteral("phone-install")},
                             {QStringLiteral("login_session_id"), QStringLiteral("phone-login")},
                             {QStringLiteral("online"), true}};
    coordinator.acceptDevices(QJsonArray{device});
    const QString createScreenshot = qEnvironmentVariable("MAICHAT_REMOTE_CREATE_SCREENSHOT");
    if (!createScreenshot.isEmpty()) QVERIFY(list->grab().save(createScreenshot));
    QVERIFY(QMetaObject::invokeMethod(list, "remoteCreateRequested", Qt::DirectConnection, Q_ARG(QJsonObject, device)));
    QVERIFY(QApplication::activeModalWidget() == nullptr);
    QCOMPARE(sent.size(), 1);
    QCOMPARE(sent.first().value(QStringLiteral("session_id")).toString(), QStringLiteral("session_lifecycle"));
    QCOMPARE(sent.first().value(QStringLiteral("payload")).toObject().value(QStringLiteral("action")).toString(),
             QStringLiteral("session_create"));
    QVERIFY(!sent.first().value(QStringLiteral("payload")).toObject().contains(QStringLiteral("workspace_path")));
    QJsonObject remote = device;
    remote.insert(QStringLiteral("id"), QStringLiteral("ses_phone"));
    remote.insert(QStringLiteral("title"), QStringLiteral("手机对话"));
    coordinator.acceptCatalog(QJsonArray{remote});
    QTimer::singleShot(80, &window, [] {
        auto* dialog = qobject_cast<AppMessageDialog*>(QApplication::activeModalWidget());
        if (!dialog) return;
        const QString screenshot = qEnvironmentVariable("MAICHAT_REMOTE_DELETE_DIALOG_SCREENSHOT");
        if (!screenshot.isEmpty()) dialog->grab().save(screenshot);
        auto* remove = dialog->findChild<QPushButton*>(QStringLiteral("appMessageDanger"));
        if (remove) remove->click();
    });
    QVERIFY(QMetaObject::invokeMethod(list, "remoteDeleteRequested", Qt::DirectConnection, Q_ARG(QJsonObject, remote)));
    QCOMPARE(sent.size(), 2);
    QCOMPARE(sent.last().value(QStringLiteral("payload")).toObject().value(QStringLiteral("action")).toString(),
             QStringLiteral("session_delete"));
    QCOMPARE(
        sent.last().value(QStringLiteral("payload")).toObject().value(QStringLiteral("target_session_id")).toString(),
        QStringLiteral("ses_phone"));
    window.setSessionSyncCoordinator(nullptr);
}

void MainWindowLayoutTest::rendersEmptyConversationState() {
    auto client = std::make_unique<FakeRemoteIMClient>();
    RemoteIMApplication app(QStringLiteral("desktop-user"), std::move(client));
    app.addContact(QStringLiteral("phone-user"), QStringLiteral("iPhone"));

    MainWindow window(app);

    QVERIFY(window.findChild<QWidget*>(QStringLiteral("emptyMessagesView")) != nullptr);
}

void MainWindowLayoutTest::sendsTextFromComposer() {
    auto client = std::make_unique<FakeRemoteIMClient>();
    auto* fakeClient = client.get();
    RemoteIMApplication app(QStringLiteral("desktop-user"), std::move(client));
    app.addContact(QStringLiteral("phone-user"), QStringLiteral("iPhone"));

    MainWindow window(app);
    auto* editor = window.findChild<QTextEdit*>(QStringLiteral("messageEditor"));
    auto* sendButton = window.findChild<QPushButton*>(QStringLiteral("sendButton"));

    QVERIFY(editor != nullptr);
    QVERIFY(sendButton != nullptr);
    QVERIFY(!sendButton->isEnabled());

    editor->setPlainText(QStringLiteral("hello from desktop"));
    QVERIFY(sendButton->isEnabled());
    sendButton->click();

    QCOMPARE(fakeClient->lastTextPeerId(), QStringLiteral("phone-user"));
    QCOMPARE(fakeClient->lastText(), QStringLiteral("hello from desktop"));
    QCOMPARE(editor->toPlainText(), QString());
    const QList<RemoteIMMessage> messages = app.chatState().messagesWith(QStringLiteral("phone-user"));
    QCOMPARE(messages.size(), 1);
    QCOMPARE(messages.first().status, RemoteIMMessageStatus::Sent);
}

void MainWindowLayoutTest::returnKeySendsComposerText() {
    auto client = std::make_unique<FakeRemoteIMClient>();
    auto* fakeClient = client.get();
    RemoteIMApplication app(QStringLiteral("desktop-user"), std::move(client));
    app.addContact(QStringLiteral("phone-user"), QStringLiteral("iPhone"));

    MainWindow window(app);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    auto* editor = window.findChild<QTextEdit*>(QStringLiteral("messageEditor"));
    QVERIFY(editor != nullptr);

    editor->setFocus();
    QTest::keyClicks(editor, "line 1");
    QTest::keyClick(editor, Qt::Key_Return);

    QCOMPARE(fakeClient->lastText(), QStringLiteral("line 1"));
    QCOMPARE(editor->toPlainText(), QString());
}

void MainWindowLayoutTest::commandOrControlReturnInsertsNewlineInComposer() {
    auto client = std::make_unique<FakeRemoteIMClient>();
    RemoteIMApplication app(QStringLiteral("desktop-user"), std::move(client));
    app.addContact(QStringLiteral("phone-user"), QStringLiteral("iPhone"));

    MainWindow window(app);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    auto* editor = window.findChild<QTextEdit*>(QStringLiteral("messageEditor"));
    QVERIFY(editor != nullptr);

    editor->setFocus();
    QTest::keyClicks(editor, "line 1");
    QTest::keyClick(editor, Qt::Key_Return, Qt::ControlModifier);
    QTest::keyClicks(editor, "line 2");

    QCOMPARE(editor->toPlainText(), QStringLiteral("line 1\nline 2"));
}

void MainWindowLayoutTest::sendsMultilineTextWithoutFlatteningReturns() {
    auto client = std::make_unique<FakeRemoteIMClient>();
    auto* fakeClient = client.get();
    RemoteIMApplication app(QStringLiteral("desktop-user"), std::move(client));
    app.addContact(QStringLiteral("phone-user"), QStringLiteral("iPhone"));

    MainWindow window(app);
    auto* editor = window.findChild<QTextEdit*>(QStringLiteral("messageEditor"));
    auto* sendButton = window.findChild<QPushButton*>(QStringLiteral("sendButton"));

    QVERIFY(editor != nullptr);
    QVERIFY(sendButton != nullptr);

    editor->setPlainText(QStringLiteral("line 1\nline 2"));
    sendButton->click();

    QCOMPARE(fakeClient->lastText(), QStringLiteral("line 1\nline 2"));
    const QList<RemoteIMMessage> messages = app.chatState().messagesWith(QStringLiteral("phone-user"));
    QCOMPARE(messages.size(), 1);
    QCOMPARE(messages.first().text, QStringLiteral("line 1\nline 2"));

    auto* markdownView = window.findChild<MarkdownLabel*>(QStringLiteral("messageMarkdownView"));
    QVERIFY(markdownView != nullptr);
    QVERIFY(markdownView->plainText().contains(QStringLiteral("line 1")));
    QVERIFY(markdownView->plainText().contains(QStringLiteral("line 2")));
}

// 发送消息的元信息在气泡内完整可见，长文本换行不应把时间和状态挤出可视区。
void MainWindowLayoutTest::rendersSentMessageFromTopWithMetadata() {
    auto client = std::make_unique<FakeRemoteIMClient>();
    RemoteIMApplication app(QStringLiteral("desktop-user"), std::move(client));
    app.addContact(QStringLiteral("phone-user"), QStringLiteral("iPhone"));

    MainWindow window(app);
    auto* editor = window.findChild<QTextEdit*>(QStringLiteral("messageEditor"));
    auto* sendButton = window.findChild<QPushButton*>(QStringLiteral("sendButton"));
    auto* messageLayout = window.findChild<QVBoxLayout*>(QStringLiteral("messageLayout"));

    QVERIFY(editor != nullptr);
    QVERIFY(sendButton != nullptr);
    QVERIFY(messageLayout != nullptr);

    editor->setPlainText(QStringLiteral("hello from desktop"));
    sendButton->click();

    // 布局 [0] 是「加载更早」按钮行（无更早历史时隐藏），消息行从 [1] 开始。
    QTRY_VERIFY(messageLayout->count() > 1);
    auto* loadEarlierButton = window.findChild<QPushButton*>(QStringLiteral("loadEarlierButton"));
    QVERIFY(loadEarlierButton != nullptr);
    QVERIFY(loadEarlierButton->isHidden());
    QVERIFY(messageLayout->itemAt(1)->widget() != nullptr);
    QCOMPARE(messageLayout->itemAt(1)->widget()->objectName(), QStringLiteral("messageRowOutgoing"));
    QVERIFY(window.findChild<QWidget*>(QStringLiteral("messageBubbleOutgoing")) != nullptr);

    QVERIFY(window.findChild<QLabel*>(QStringLiteral("messageAuthorLabel")) == nullptr);
    QVERIFY(window.findChild<QLabel*>(QStringLiteral("messageTimeLabel")) == nullptr);
    QVERIFY(window.findChild<QLabel*>(QStringLiteral("messageAvatarOutgoing")) == nullptr);
    auto* markdown = window.findChild<MarkdownLabel*>(QStringLiteral("messageMarkdownView"));
    QVERIFY(markdown != nullptr);
    const auto timestamp = markdown->property("messageTimestamp").toString();
    QVERIFY(!timestamp.isEmpty());
    QVERIFY(markdown->plainText().startsWith(QStringLiteral("hello from desktop")));
    QVERIFY(markdown->plainText().endsWith(timestamp));
    // 时间戳必须接在正文最后一行的**末尾**，不能另起一行。
    // 自绘那条路里，块与块之间会补一个换行；时间戳如果被当成单独一块，
    // 这里就会出现换行。所以「整条没有换行」正好卡住那个退化。
    QVERIFY2(!markdown->plainText().contains(QLatin1Char('\n')),
             qPrintable(QStringLiteral("时间戳另起了一行：") + markdown->plainText()));
    auto* statusLabel = window.findChild<QLabel*>(QStringLiteral("messageStatusLabel"));
    QVERIFY(statusLabel != nullptr);
    QCOMPARE(statusLabel->text(), QStringLiteral("✓"));
    QCOMPARE(statusLabel->alignment(), Qt::AlignCenter);
    QCOMPARE(statusLabel->minimumWidth(), 16);
    QCOMPARE(statusLabel->minimumHeight(), 16);
    QVERIFY(statusLabel->styleSheet().contains(QStringLiteral("border: 1px solid #12a150")));
    QVERIFY(statusLabel->styleSheet().contains(QStringLiteral("background: transparent")));
    // 状态图标要和气泡平级挂在消息行上，不能落进气泡内部。
    // 必须从 statusLabel 反查所在行，不能用 window.findChild 直接找气泡：pending→sent
    // 是「建新行 + 旧行 deleteLater + 插入新行」，旧 pending 行在事件循环真正销毁前仍留在
    // 对象树里，findChild 可能拿到那条旧气泡。而 Pending 态不生成状态图标，statusLabel
    // 只可能来自新行——拿旧气泡和新行的图标比祖先关系恒为假，断言会假通过，退回旧实现照样绿。
    QWidget* statusRow = statusLabel->parentWidget();
    QVERIFY(statusRow != nullptr);
    QCOMPARE(statusRow->objectName(), QStringLiteral("messageRowOutgoing"));
    QCOMPARE(statusRow, messageLayout->itemAt(1)->widget());
    // 气泡应当是同一行里的兄弟节点。父子关系已被上面的 parentWidget 断言锁死，
    // 再补一条 isAncestorOf 是恒真的废断言，故不写。
    QVERIFY(statusRow->findChild<QWidget*>(QStringLiteral("messageBubbleOutgoing"), Qt::FindDirectChildrenOnly) !=
            nullptr);
}

void MainWindowLayoutTest::rendersRelativeMessageDates() {
    auto client = std::make_unique<FakeRemoteIMClient>();
    RemoteIMApplication app(QStringLiteral("desktop-user"), std::move(client));
    app.addContact(QStringLiteral("phone-user"), QStringLiteral("iPhone"));

    const QDate today = QDate::currentDate();
    const QTime messageTime(16, 13);
    const QDateTime todayTime(today, messageTime, Qt::LocalTime);
    const QDateTime yesterdayTime(today.addDays(-1), messageTime, Qt::LocalTime);
    const QDateTime olderTime(today.addDays(-2), messageTime, Qt::LocalTime);

    RemoteIMMessage todayMessage;
    todayMessage.fromUserId = QStringLiteral("phone-user");
    todayMessage.toUserId = QStringLiteral("desktop-user");
    todayMessage.text = QStringLiteral("today");
    todayMessage.createdAtMillis = todayTime.toMSecsSinceEpoch();

    RemoteIMMessage yesterdayMessage = todayMessage;
    yesterdayMessage.id = QStringLiteral("yesterday-message");
    yesterdayMessage.text = QStringLiteral("yesterday");
    yesterdayMessage.createdAtMillis = yesterdayTime.toMSecsSinceEpoch();

    RemoteIMMessage olderMessage = todayMessage;
    olderMessage.id = QStringLiteral("older-message");
    olderMessage.text = QStringLiteral("older");
    olderMessage.createdAtMillis = olderTime.toMSecsSinceEpoch();

    app.chatState().appendMessageForRestore(todayMessage);
    app.chatState().appendMessageForRestore(yesterdayMessage);
    app.chatState().appendMessageForRestore(olderMessage);

    MainWindow window(app);

    QStringList actualTimes;
    for (const auto* view : window.findChildren<MarkdownLabel*>(QStringLiteral("messageMarkdownView"))) {
        const auto timestamp = view->property("messageTimestamp").toString();
        QVERIFY(view->plainText().endsWith(timestamp));
        actualTimes.append(timestamp);
    }

    QVERIFY(actualTimes.contains(QStringLiteral("16:13")));
    QVERIFY(actualTimes.contains(QStringLiteral("昨天 16:13")));
    QVERIFY(actualTimes.contains(olderTime.toString(QStringLiteral("M 月 d 日 HH:mm"))));
}

void MainWindowLayoutTest::contactsNavigationShowsProfileBeforeChat() {
    auto client = std::make_unique<FakeRemoteIMClient>();
    RemoteIMApplication app(QStringLiteral("desktop-user"), std::move(client));
    app.addContact(QStringLiteral("phone-user"), QStringLiteral("iPhone"));
    RemoteIMApplication::AccountFriendOperations operations;
    operations.list = [](auto completion) {
        completion({RemoteIMApplication::AccountFriend{QStringLiteral("m_friend"), QStringLiteral("好友昵称"),
                                                       QStringLiteral("phone-user")}},
                   {});
    };
    app.setAccountFriendOperations(std::move(operations));

    MainWindow window(app);
    auto* contactsNavButton = window.findChild<QPushButton*>(QStringLiteral("contactsNavButton"));
    QVERIFY(contactsNavButton != nullptr);

    contactsNavButton->click();

    auto* contactsPage = window.findChild<QWidget*>(QStringLiteral("contactsPage"));
    auto* contentStack = window.findChild<QStackedWidget*>(QStringLiteral("contentStack"));
    auto* contactsList = window.findChild<QListWidget*>(QStringLiteral("contactsList"));
    QVERIFY(contactsPage != nullptr);
    QVERIFY(contentStack != nullptr);
    QCOMPARE(contentStack->currentWidget(), contactsPage);
    QVERIFY(contactsList != nullptr);
    QCOMPARE(contactsList->count(), 1);
    QCOMPARE(contactsList->item(0)->data(Qt::UserRole).toString(), QStringLiteral("phone-user"));

    const QRect itemRect = contactsList->visualItemRect(contactsList->item(0));
    QTest::mouseClick(contactsList->viewport(), Qt::LeftButton, Qt::NoModifier, itemRect.center());
    auto* profileStack = window.findChild<QStackedWidget*>(QStringLiteral("contactProfileStack"));
    auto* profileName = window.findChild<QLabel*>(QStringLiteral("contactProfileName"));
    auto* profileId = window.findChild<QLabel*>(QStringLiteral("contactProfileAccountId"));
    auto* message = window.findChild<QPushButton*>(QStringLiteral("contactProfileMessageButton"));
    QVERIFY(profileStack && profileName && profileId && message);
    QCOMPARE(contentStack->currentWidget(), contactsPage);
    QCOMPARE(profileStack->currentIndex(), 1);
    QCOMPARE(profileName->text(), QStringLiteral("好友昵称"));
    QCOMPARE(profileId->text(), QStringLiteral("m_friend"));
    const QByteArray capture = qgetenv("MAICHAT_CAPTURE_CONTACT_PROFILE");
    if (!capture.isEmpty()) QVERIFY(profileStack->grab().save(QString::fromUtf8(capture)));
    message->click();
    QCOMPARE(app.chatState().selectedPeerId(), QStringLiteral("phone-user"));
    auto* messagesPage = window.findChild<QWidget*>(QStringLiteral("messagesPage"));
    QVERIFY(messagesPage != nullptr);
    QCOMPARE(contentStack->currentWidget(), messagesPage);
}

void MainWindowLayoutTest::composerResizeHandleAlignsWithEditorBorder_data() {
    QTest::addColumn<qreal>("zoom");
    for (const qreal zoom : {0.8, 1.0, 1.5, 2.0}) {
        QTest::newRow(QByteArray::number(zoom).constData()) << zoom;
    }
}

void MainWindowLayoutTest::composerResizeHandleAlignsWithEditorBorder() {
    QFETCH(qreal, zoom);
    struct RestoreZoom {
        qreal old;
        ~RestoreZoom() {
            UiZoom::setFactor(old);
        }
    } restore{UiZoom::factor()};
    UiZoom::setFactor(zoom);
    RemoteIMApplication app(QStringLiteral("desktop-user"), std::make_unique<FakeRemoteIMClient>());
    MainWindow window(app);
    window.resize(1400, 900);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    auto* splitter = window.findChild<QSplitter*>(QStringLiteral("messageComposerSplitter"));
    auto* editor = window.findChild<QTextEdit*>(QStringLiteral("messageEditor"));
    QVERIFY(splitter != nullptr);
    QVERIFY(editor != nullptr);
    QSplitterHandle* handle = splitter->handle(1);
    QVERIFY(handle != nullptr);

    const QRect handleOnScreen(handle->mapToGlobal(QPoint(0, 0)), handle->size());
    const QRect editorOnScreen(editor->mapToGlobal(QPoint(0, 0)), editor->size());
    QCOMPARE(editorOnScreen.top(), handleOnScreen.bottom() + 1);
    QCOMPARE(editorOnScreen.center().x(), handleOnScreen.center().x());
}

void MainWindowLayoutTest::composerResizeHandleBlendsIntoCanvas() {
    RemoteIMApplication app(QStringLiteral("desktop-user"), std::make_unique<FakeRemoteIMClient>());
    MainWindow window(app);
    window.resize(1400, 900);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    auto* splitter = window.findChild<QSplitter*>(QStringLiteral("messageComposerSplitter"));
    QVERIFY(splitter != nullptr);
    QSplitterHandle* handle = splitter->handle(1);
    QVERIFY(handle != nullptr);
    const QImage image = window.grab().toImage();
    const qreal scale = image.devicePixelRatio();
    const QPoint center = handle->mapTo(&window, handle->rect().center());
    const QColor color = image.pixelColor(qRound(center.x() * scale), qRound(center.y() * scale));
    QVERIFY2(color.red() >= 250 && color.green() >= 250 && color.blue() >= 250,
             qPrintable(QStringLiteral("Composer divider is visible: %1").arg(color.name())));
}

void MainWindowLayoutTest::composerResizeCursorClearsAfterPointerLeaves() {
    RemoteIMApplication app(QStringLiteral("desktop-user"), std::make_unique<FakeRemoteIMClient>());
    MainWindow window(app);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    auto* splitter = window.findChild<QSplitter*>(QStringLiteral("messageComposerSplitter"));
    QVERIFY(splitter != nullptr);
    QSplitterHandle* handle = splitter->handle(1);
    QVERIFY(handle != nullptr);

    QEvent enter(QEvent::Enter);
    QApplication::sendEvent(handle, &enter);
    QCOMPARE(handle->cursor().shape(), Qt::SplitVCursor);
    QEvent leave(QEvent::Leave);
    QApplication::sendEvent(handle, &leave);
    QCOMPARE(handle->cursor().shape(), Qt::ArrowCursor);
    QApplication::sendEvent(handle, &enter);
    QEvent ungrab(QEvent::UngrabMouse);
    QApplication::sendEvent(handle, &ungrab);
    QCOMPARE(handle->cursor().shape(), Qt::ArrowCursor);

    auto* agentButton = window.findChild<QPushButton*>(QStringLiteral("agentNavButton"));
    QVERIFY(agentButton != nullptr);
    agentButton->click();
    auto* agentSplitter = window.findChild<QSplitter*>(QStringLiteral("agentMessageComposerSplitter"));
    QVERIFY(agentSplitter != nullptr);
    QSplitterHandle* agentHandle = agentSplitter->handle(1);
    QVERIFY(agentHandle != nullptr);
    QApplication::sendEvent(agentHandle, &enter);
    QCOMPARE(agentHandle->cursor().shape(), Qt::SplitVCursor);
    QApplication::sendEvent(agentHandle, &leave);
    QCOMPARE(agentHandle->cursor().shape(), Qt::ArrowCursor);
}

// 拖动输入区分隔条时保留最新消息可见；布局变化不能让滚动锚点落进空白。
void MainWindowLayoutTest::composerResizeKeepsLatestMessageVisible() {
    auto client = std::make_unique<FakeRemoteIMClient>();
    RemoteIMApplication app(QStringLiteral("desktop-user"), std::move(client));
    app.addContact(QStringLiteral("phone-user"), QStringLiteral("iPhone"));
    app.selectPeer(QStringLiteral("phone-user"));
    for (int index = 0; index < 30; ++index) app.sendText(QStringLiteral("message %1").arg(index));

    MainWindow window(app);
    window.resize(1280, 800);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    QCoreApplication::processEvents();
    auto* splitter = window.findChild<QSplitter*>(QStringLiteral("messageComposerSplitter"));
    auto* scroll = window.findChild<QScrollArea*>(QStringLiteral("messageScroll"));
    QVERIFY(splitter && scroll);
    QScrollBar* bar = scroll->verticalScrollBar();
    // Showing the window posts its first content-layout pass. A debugger used
    // to hide this timing gap; exercise dragging only after that pass finished.
    QTRY_VERIFY_WITH_TIMEOUT(bar->maximum() > 0, 2000);
    bar->setValue(bar->maximum());
    const int oldHeight = scroll->height();
    QSplitterHandle* handle = splitter->handle(1);
    const QPoint start = handle->rect().center();
    const QPoint startGlobal = handle->mapToGlobal(start);
    QTest::mousePress(handle, Qt::LeftButton, Qt::NoModifier, start);
    bool latestVisibleDuringDrag = true;
    QString clippedDuringDrag;
    for (int step = 1; step <= 5; ++step) {
        const QPoint global = startGlobal - QPoint(0, step * 24);
        const QPoint local = handle->mapFromGlobal(global);
        QMouseEvent move(QEvent::MouseMove, QPointF(local), QPointF(global), Qt::NoButton, Qt::LeftButton,
                         Qt::NoModifier);
        QApplication::sendEvent(handle, &move);
        QCoreApplication::processEvents();
        if (bar->value() != bar->maximum()) {
            latestVisibleDuringDrag = false;
            clippedDuringDrag = QStringLiteral("step=%1 scroll=%2/%3").arg(step).arg(bar->value()).arg(bar->maximum());
            break;
        }
    }
    const QPoint finishGlobal = startGlobal - QPoint(0, 120);
    QTest::mouseRelease(handle, Qt::LeftButton, Qt::NoModifier, handle->mapFromGlobal(finishGlobal));
    QCoreApplication::processEvents();
    QVERIFY2(latestVisibleDuringDrag, qPrintable(clippedDuringDrag));
    QVERIFY(scroll->height() < oldHeight);
    QCOMPARE(bar->value(), bar->maximum());
    const QList<QWidget*> rows = window.findChildren<QWidget*>(QStringLiteral("messageRowOutgoing"));
    QVERIFY(!rows.isEmpty());
    QWidget* latest = rows.first();
    for (QWidget* row : rows) {
        if (row->mapTo(scroll->viewport(), QPoint(0, 0)).y() > latest->mapTo(scroll->viewport(), QPoint(0, 0)).y())
            latest = row;
    }
    const QRect latestRect(latest->mapTo(scroll->viewport(), QPoint(0, 0)), latest->size());
    QVERIFY2(latestRect.top() < scroll->viewport()->height() && latestRect.bottom() >= 0,
             qPrintable(QStringLiteral("latest row=%1..%2 viewport=%3 scroll=%4/%5")
                            .arg(latestRect.top())
                            .arg(latestRect.bottom())
                            .arg(scroll->viewport()->height())
                            .arg(bar->value())
                            .arg(bar->maximum())));
}

// 编辑器控件被会话共用，切换联系人时必须同时切换富文本草稿和引用。
void MainWindowLayoutTest::composerDraftAndQuoteStayWithTheirContact() {
    auto client = std::make_unique<FakeRemoteIMClient>();
    auto* fakeClient = client.get();
    RemoteIMApplication app(QStringLiteral("desktop-user"), std::move(client));
    app.addContact(QStringLiteral("peer-a"), QStringLiteral("A"));
    app.addContact(QStringLiteral("peer-b"), QStringLiteral("B"));
    app.selectPeer(QStringLiteral("peer-a"));
    MainWindow window(app);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    auto* editor = window.findChild<QTextEdit*>(QStringLiteral("messageEditor"));
    auto* replyBar = window.findChild<QWidget*>(QStringLiteral("pendingReplyBar"));
    auto* send = window.findChild<QPushButton*>(QStringLiteral("sendButton"));
    QVERIFY(editor != nullptr);
    QVERIFY(replyBar != nullptr);
    QVERIFY(send != nullptr);

    editor->setPlainText(QStringLiteral("unsent draft for A"));
    RemoteIMMessage quoted;
    quoted.fromUserId = QStringLiteral("peer-a");
    quoted.toUserId = QStringLiteral("desktop-user");
    quoted.text = QStringLiteral("A's message");
    window.beginReplyTo(quoted);
    QVERIFY(replyBar->isVisible());

    app.selectPeer(QStringLiteral("peer-b"));
    QCOMPARE(editor->toPlainText(), QString());
    QVERIFY(!replyBar->isVisible());
    window.beginReplyTo(quoted);
    QVERIFY(!replyBar->isVisible());
    editor->setPlainText(QStringLiteral("unsent draft for B"));

    app.selectPeer(QStringLiteral("peer-a"));
    QCOMPARE(editor->toPlainText(), QStringLiteral("unsent draft for A"));
    QVERIFY(replyBar->isVisible());
    app.selectPeer(QStringLiteral("peer-b"));
    QCOMPARE(editor->toPlainText(), QStringLiteral("unsent draft for B"));
    QVERIFY(!replyBar->isVisible());

    send->click();
    QCOMPARE(fakeClient->lastTextPeerId(), QStringLiteral("peer-b"));
    QCOMPARE(fakeClient->lastText(), QStringLiteral("unsent draft for B"));
    const QList<RemoteIMMessage> sent = app.chatState().messagesWith(QStringLiteral("peer-b"));
    QVERIFY(!sent.isEmpty());
    QVERIFY(!sent.last().hasQuote);
    app.selectPeer(QStringLiteral("peer-a"));
    QCOMPARE(editor->toPlainText(), QStringLiteral("unsent draft for A"));
    QVERIFY(replyBar->isVisible());
}

void MainWindowLayoutTest::attachmentDraftStaysWithItsContact() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString imagePath = QDir(directory.path()).filePath(QStringLiteral("draft.png"));
    QImage image(32, 24, QImage::Format_RGB32);
    image.fill(Qt::green);
    QVERIFY(image.save(imagePath, "PNG"));

    auto client = std::make_unique<FakeRemoteIMClient>();
    auto* fakeClient = client.get();
    RemoteIMApplication app(QStringLiteral("desktop-user"), std::move(client));
    app.addContact(QStringLiteral("peer-a"), QStringLiteral("A"));
    app.addContact(QStringLiteral("peer-b"), QStringLiteral("B"));
    app.selectPeer(QStringLiteral("peer-a"));
    MainWindow window(app);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    auto* editor = window.findChild<QTextEdit*>(QStringLiteral("messageEditor"));
    auto* send = window.findChild<QPushButton*>(QStringLiteral("sendButton"));
    QVERIFY(editor != nullptr);
    QVERIFY(send != nullptr);
    QMimeData mime;
    mime.setUrls({QUrl::fromLocalFile(imagePath)});
    dropOnComposer(editor, &mime);
    QVERIFY(editor->toPlainText().contains(QChar(0xFFFC)));

    app.selectPeer(QStringLiteral("peer-b"));
    QCOMPARE(editor->toPlainText(), QString());
    editor->setPlainText(QStringLiteral("B text"));
    app.selectPeer(QStringLiteral("peer-a"));
    QVERIFY(editor->toPlainText().contains(QChar(0xFFFC)));
    send->click();
    QCOMPARE(fakeClient->lastImagePeerId(), QStringLiteral("peer-a"));
    QCOMPARE(fakeClient->lastImagePath(), imagePath);
    app.selectPeer(QStringLiteral("peer-b"));
    QCOMPARE(editor->toPlainText(), QStringLiteral("B text"));
}

void MainWindowLayoutTest::sentImageDoesNotReturnAsDraftDuringConversationChange() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString imagePath = directory.filePath(QStringLiteral("sent.png"));
    QImage image(120, 90, QImage::Format_RGB32);
    image.fill(Qt::green);
    QVERIFY(image.save(imagePath, "PNG"));

    auto client = std::make_unique<FakeRemoteIMClient>();
    auto* fakeClient = client.get();
    RemoteIMApplication app(QStringLiteral("desktop-user"), std::move(client));
    app.addContact(QStringLiteral("peer-a"), QStringLiteral("A"));
    app.addContact(QStringLiteral("peer-b"), QStringLiteral("B"));
    app.selectPeer(QStringLiteral("peer-a"));
    MainWindow window(app);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    auto* editor = window.findChild<QTextEdit*>(QStringLiteral("messageEditor"));
    auto* send = window.findChild<QPushButton*>(QStringLiteral("sendButton"));
    QVERIFY(editor != nullptr);
    QVERIFY(send != nullptr);
    QMimeData mime;
    mime.setUrls({QUrl::fromLocalFile(imagePath)});
    dropOnComposer(editor, &mime);
    editor->insertPlainText(QStringLiteral("First with image"));

    bool switched = false;
    connect(&app, &RemoteIMApplication::stateChanged, &window, [&] {
        if (switched || app.chatState().selectedPeerId() != QStringLiteral("peer-a")) return;
        const auto messages = app.chatState().messagesWith(QStringLiteral("peer-a"));
        if (messages.isEmpty() || !messages.last().hasImage) return;
        switched = true;
        app.selectPeer(QStringLiteral("peer-b"));
    });
    send->click();
    QVERIFY(switched);
    app.selectPeer(QStringLiteral("peer-a"));
    QVERIFY(!editor->toPlainText().contains(QChar(0xFFFC)));
    editor->insertPlainText(QStringLiteral("Second text only"));
    send->click();
    QCOMPARE(fakeClient->lastText(), QStringLiteral("Second text only"));
    const auto messages = app.chatState().messagesWith(QStringLiteral("peer-a"));
    QCOMPARE(messages.size(), 2);
    QVERIFY(messages.first().hasImage);
    QVERIFY(!messages.last().hasImage);
    const QString capturePath = qEnvironmentVariable("MAICHAT_REPEAT_IMAGE_CAPTURE");
    if (!capturePath.isEmpty()) {
        QTest::qWait(150);
        QVERIFY(window.grab().save(capturePath));
    }
}

void MainWindowLayoutTest::contactsCurrentSelectionDoesNotLeaveContactsPage() {
    auto client = std::make_unique<FakeRemoteIMClient>();
    RemoteIMApplication app(QStringLiteral("desktop-user"), std::move(client));
    app.addContact(QStringLiteral("phone-user-1"), QStringLiteral("iPhone 1"));
    app.addContact(QStringLiteral("phone-user-2"), QStringLiteral("iPhone 2"));

    MainWindow window(app);
    auto* contactsNavButton = window.findChild<QPushButton*>(QStringLiteral("contactsNavButton"));
    auto* contactsPage = window.findChild<QWidget*>(QStringLiteral("contactsPage"));
    auto* contentStack = window.findChild<QStackedWidget*>(QStringLiteral("contentStack"));
    auto* contactsList = window.findChild<QListWidget*>(QStringLiteral("contactsList"));

    QVERIFY(contactsNavButton != nullptr);
    QVERIFY(contactsPage != nullptr);
    QVERIFY(contentStack != nullptr);
    QVERIFY(contactsList != nullptr);

    contactsNavButton->click();
    QCOMPARE(contentStack->currentWidget(), contactsPage);

    contactsList->setCurrentRow(0);

    QCOMPARE(contentStack->currentWidget(), contactsPage);
}

void MainWindowLayoutTest::settingsNavigationShowsAccountWithoutDiagnostics() {
    auto client = std::make_unique<FakeRemoteIMClient>();
    RemoteIMApplication app(QStringLiteral("desktop-user"), std::move(client));
    app.addContact(QStringLiteral("phone-user"), QStringLiteral("iPhone"));

    MainWindow window(app);
    window.setMaiChatNicknameProvider([] { return QStringLiteral("昵称测试"); });
    window.setMaiChatNicknameUpdater([](const QString&, RemoteIMCompletion) {});
    auto* settingsNavButton = window.findChild<QPushButton*>(QStringLiteral("settingsNavButton"));
    QVERIFY(settingsNavButton != nullptr);

    settingsNavButton->click();

    auto* settingsPage = window.findChild<QWidget*>(QStringLiteral("settingsPage"));
    auto* contentStack = window.findChild<QStackedWidget*>(QStringLiteral("contentStack"));
    auto* accountValue = window.findChild<QLabel*>(QStringLiteral("settingsAccountValue"));
    auto* nicknameValue = window.findChild<QLabel*>(QStringLiteral("settingsNicknameValue"));
    auto* nicknameButton = window.findChild<QPushButton*>(QStringLiteral("settingsNicknameButton"));
    QVERIFY(settingsPage != nullptr);
    QVERIFY(contentStack != nullptr);
    QCOMPARE(contentStack->currentWidget(), settingsPage);
    QVERIFY(accountValue != nullptr);
    QVERIFY(nicknameValue != nullptr);
    QVERIFY(nicknameButton != nullptr);
    QCOMPARE(accountValue->text(), QStringLiteral("desktop-user"));
    QCOMPARE(nicknameValue->text(), QStringLiteral("昵称测试"));
    QVERIFY(nicknameButton->isEnabled());
    const QByteArray capture = qgetenv("MAICHAT_CAPTURE_SETTINGS");
    if (!capture.isEmpty()) {
        window.show();
        QCoreApplication::processEvents();
        QVERIFY(settingsPage->grab().save(QString::fromUtf8(capture)));
    }
    for (const char* name :
         {"settingsConnectionValue", "settingsSdkAppIdValue", "settingsSignatureValue", "settingsAgentModelValue"}) {
        QVERIFY(settingsPage->findChild<QLabel*>(QString::fromLatin1(name)) == nullptr);
    }
    // 设置页的值应是只读标签而非输入框；导航栏的搜索框是唯一合法的 QLineEdit。
    QVERIFY(settingsPage->findChildren<QLineEdit*>().isEmpty());
}

void MainWindowLayoutTest::settingsAvatarUpdatesWhenSelfProfileArrives() {
    AvatarHttpServer server;
    QVERIFY(server.listen());
    QImage red(32, 32, QImage::Format_RGB32);
    red.fill(QColor(230, 20, 20));
    QByteArray png;
    QBuffer imageBuffer(&png);
    QVERIFY(imageBuffer.open(QIODevice::WriteOnly));
    QVERIFY(red.save(&imageBuffer, "PNG"));
    std::atomic<int>& requests = server.requests;
    server.servePng(png);
    auto client = std::make_unique<FakeRemoteIMClient>();
    FakeRemoteIMClient* fake = client.get();
    RemoteIMApplication app(QStringLiteral("desktop-user"), std::move(client));
    MainWindow window(app);
    window.show();
    auto* settings = window.findChild<QPushButton*>(QStringLiteral("settingsNavButton"));
    auto* avatar = window.findChild<QLabel*>(QStringLiteral("selfProfileAvatar"));
    QVERIFY(settings && avatar);
    settings->click();
    const QString url = QStringLiteral("http://127.0.0.1:%1/avatar.png").arg(server.serverPort());
    fake->selfProfileReceived(QStringLiteral("desktop-user"), QStringLiteral("真实头像"), url);
    QVERIFY(avatar->property("avatarRequestsEnabled").toBool());
    QCOMPARE(avatar->accessibleName(), QStringLiteral("真实头像"));
    QTRY_VERIFY_WITH_TIMEOUT(requests > 0, 3000);
    QTRY_VERIFY_WITH_TIMEOUT(
        [&] {
            const QColor center = avatar->grab().toImage().pixelColor(avatar->width() / 2, avatar->height() / 2);
            return center.red() > 200 && center.green() < 80 && center.blue() < 80;
        }(),
        3000);
    const QByteArray capture = qgetenv("MAICHAT_AVATAR_CAPTURE");
    if (!capture.isEmpty()) QVERIFY(window.grab().save(QString::fromUtf8(capture)));
}

void MainWindowLayoutTest::settingsAvatarReadsTinyWebpFromMemory() {
    AvatarHttpServer server;
    QVERIFY(server.listen());
    server.servePng(QByteArray::fromHex(
        "524946461e000000574542505650384c110000002f0fc0020007d0b1b6b4b1ff8188e87f0000"));
    auto client = std::make_unique<FakeRemoteIMClient>();
    FakeRemoteIMClient* fake = client.get();
    RemoteIMApplication app(QStringLiteral("desktop-owned-webp-avatar"), std::move(client));
    MainWindow window(app);
    window.show();
    auto* settings = window.findChild<QPushButton*>(QStringLiteral("settingsNavButton"));
    auto* avatar = window.findChild<QLabel*>(QStringLiteral("selfProfileAvatar"));
    QVERIFY(settings && avatar);
    settings->click();
    const QString url = QStringLiteral("http://127.0.0.1:%1/owned-tiny.webp").arg(server.serverPort());
    fake->selfProfileReceived(QStringLiteral("desktop-owned-webp-avatar"), QStringLiteral("Owned fixture"), url);
    QTRY_VERIFY_WITH_TIMEOUT(server.requests > 0, 3000);
    QTRY_VERIFY_WITH_TIMEOUT([&] {
        const QColor center = avatar->grab().toImage().pixelColor(avatar->width() / 2, avatar->height() / 2);
        return qAbs(center.red() - 45) <= 3 && qAbs(center.green() - 99) <= 3 &&
               qAbs(center.blue() - 141) <= 3;
    }(), 3000);
}

void MainWindowLayoutTest::settingsAvatarSurvivesNetworkLossAfterRestart() {
    const QByteArray childUrl = qgetenv("MAICHAT_AVATAR_CACHE_CHILD_URL");
    auto client = std::make_unique<FakeRemoteIMClient>();
    FakeRemoteIMClient* fake = client.get();
    RemoteIMApplication app(QStringLiteral("desktop-cache-user"), std::move(client));
    MainWindow window(app);
    window.show();
    auto* avatar = window.findChild<QLabel*>(QStringLiteral("selfProfileAvatar"));
    auto* settings = window.findChild<QPushButton*>(QStringLiteral("settingsNavButton"));
    QVERIFY(avatar && settings);
    settings->click();

    const auto isRed = [avatar] {
        const QColor center = avatar->grab().toImage().pixelColor(avatar->width() / 2, avatar->height() / 2);
        return center.red() > 200 && center.green() < 80 && center.blue() < 80;
    };
    if (!childUrl.isEmpty()) {
        fake->selfProfileReceived(QStringLiteral("desktop-cache-user"), QStringLiteral("Cached"),
                                  QString::fromUtf8(childUrl));
        QTRY_VERIFY_WITH_TIMEOUT(isRed(), 3000);
        return;
    }

    AvatarHttpServer server;
    QVERIFY(server.listen());
    QImage red(32, 32, QImage::Format_RGB32);
    red.fill(QColor(230, 20, 20));
    QByteArray png;
    QBuffer imageBuffer(&png);
    QVERIFY(imageBuffer.open(QIODevice::WriteOnly));
    QVERIFY(red.save(&imageBuffer, "PNG"));
    server.servePng(png);
    const QString url = QStringLiteral("http://127.0.0.1:%1/cache-avatar.png").arg(server.serverPort());
    const QString cachePath =
        QDir(QStandardPaths::writableLocation(QStandardPaths::CacheLocation))
            .filePath(QStringLiteral("avatars/") +
                      QString::fromLatin1(QCryptographicHash::hash(url.toUtf8(), QCryptographicHash::Sha256).toHex()) +
                      QStringLiteral(".png"));
    QFile::remove(cachePath);
    fake->selfProfileReceived(QStringLiteral("desktop-cache-user"), QStringLiteral("Cached"), url);
    QTRY_VERIFY_WITH_TIMEOUT(isRed(), 3000);
    QVERIFY(QFileInfo::exists(cachePath));
    server.close();

    QProcess child;
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral("MAICHAT_AVATAR_CACHE_CHILD_URL"), url);
    child.setProcessEnvironment(environment);
    child.start(QCoreApplication::applicationFilePath(),
                {QStringLiteral("settingsAvatarSurvivesNetworkLossAfterRestart")});
    QVERIFY(child.waitForFinished(10000));
    QCOMPARE(child.exitStatus(), QProcess::NormalExit);
    QCOMPARE(child.exitCode(), 0);
    QFile::remove(cachePath);
}

void MainWindowLayoutTest::settingsAvatarLoadsRealProfileUrl() {
    const QByteArray url = qgetenv("MAICHAT_AVATAR_LIVE_URL");
    if (url.isEmpty()) QSKIP("No live avatar URL supplied");
    auto client = std::make_unique<FakeRemoteIMClient>();
    FakeRemoteIMClient* fake = client.get();
    RemoteIMApplication app(QStringLiteral("desktop-live-avatar-user"), std::move(client));
    MainWindow window(app);
    window.show();
    auto* settings = window.findChild<QPushButton*>(QStringLiteral("settingsNavButton"));
    auto* avatar = window.findChild<QLabel*>(QStringLiteral("selfProfileAvatar"));
    auto* navAvatar = window.findChild<QLabel*>(QStringLiteral("navLogo"));
    QVERIFY(settings && avatar && navAvatar);
    settings->click();
    const QImage fallback = avatar->grab().toImage();
    const QImage navFallback = navAvatar->grab().toImage();
    fake->selfProfileReceived(QStringLiteral("desktop-live-avatar-user"), QStringLiteral("Real profile"),
                              QString::fromUtf8(url));
    QTRY_VERIFY_WITH_TIMEOUT(avatar->grab().toImage() != fallback, 20000);
    QTRY_VERIFY_WITH_TIMEOUT(navAvatar->grab().toImage() != navFallback, 20000);
    const QByteArray capture = qgetenv("MAICHAT_AVATAR_CAPTURE");
    if (!capture.isEmpty()) QVERIFY(window.grab().save(QString::fromUtf8(capture)));
}

void MainWindowLayoutTest::settingsLogoutRequiresConfirmationAndCallsHandler() {
    auto client = std::make_unique<FakeRemoteIMClient>();
    RemoteIMApplication app(QStringLiteral("desktop-user"), std::move(client));
    MainWindow window(app);
    auto* button = window.findChild<QPushButton*>(QStringLiteral("settingsLogoutButton"));
    QVERIFY(button != nullptr);
    QCOMPARE(button->text(), QStringLiteral("退出登录 / 切换账号"));
    QVERIFY(!button->isEnabled());

    bool loggedOut = false;
    window.setLogoutHandler([&loggedOut] { loggedOut = true; });
    QVERIFY(button->isEnabled());
    bool sawCancel = false;
    QTimer::singleShot(0, &window, [&] {
        auto* dialog = window.findChild<QDialog*>(QStringLiteral("appMessageDialog"));
        auto* cancel = dialog ? dialog->findChild<QPushButton*>(QStringLiteral("appMessageCancel")) : nullptr;
        sawCancel = cancel != nullptr;
        if (cancel) cancel->click();
    });
    button->click();
    QVERIFY(sawCancel);
    QVERIFY(!loggedOut);

    bool sawConfirm = false;
    QTimer::singleShot(0, &window, [&] {
        auto* dialog = window.findChild<QDialog*>(QStringLiteral("appMessageDialog"));
        auto* confirm = dialog ? dialog->findChild<QPushButton*>(QStringLiteral("appMessageDanger")) : nullptr;
        sawConfirm = confirm != nullptr;
        if (confirm) confirm->click();
    });
    button->click();
    QVERIFY(sawConfirm);
    QVERIFY(loggedOut);
    QVERIFY(!button->isEnabled());
}

void MainWindowLayoutTest::agentSettingsUseInlineActionsWithoutModelDialog() {
    auto client = std::make_unique<FakeRemoteIMClient>();
    RemoteIMApplication app(QStringLiteral("desktop-user"), std::move(client));
    MainWindow window(app);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    QVERIFY(window.findChild<QPushButton*>(QStringLiteral("settingsAgentModelButton")) == nullptr);
    QVERIFY(window.findChild<QDialog*>(QStringLiteral("agentModelDialog")) == nullptr);
    auto* more = window.findChild<QPushButton*>(QStringLiteral("agentMoreActions"));
    QVERIFY(more);
    QTest::mouseClick(more, Qt::LeftButton);
    auto* actions = window.findChild<QWidget*>(QStringLiteral("agentActionPanel"));
    QVERIFY(actions && actions->isVisible());
    QVERIFY(actions->findChild<QPushButton*>(QStringLiteral("agentEffortSection")));
    QVERIFY(actions->findChild<QPushButton*>(QStringLiteral("agentPolicySection")));
}

void MainWindowLayoutTest::agentReasoningEffortKeepsConversation() {
    const QString oldOrganization = QCoreApplication::organizationName();
    const QString oldApplication = QCoreApplication::applicationName();
    QCoreApplication::setOrganizationName(QStringLiteral("MaiChatTests"));
    QCoreApplication::setApplicationName(QStringLiteral("InlineReasoningEffort"));
    QSettings settings;
    settings.clear();
    auto client = std::make_unique<FakeRemoteIMClient>();
    RemoteIMApplication app(QStringLiteral("desktop-user"), std::move(client));
    MainWindow window(app);
    const QHash<QString, QString> keys = {{QStringLiteral("glm"), QString(40, QLatin1Char('G'))},
                                          {QStringLiteral("deepseek"), QString(40, QLatin1Char('D'))}};
    window.setAccountModelKeys(keys);
    int refreshed = 0;
    window.setModelKeyRefreshHandler([&] {
        ++refreshed;
        QTimer::singleShot(0, &window, [&] { window.setAccountModelKeys(keys); });
    });
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    auto* panel = window.findChild<AgentChatPanel*>();
    QVERIFY(panel != nullptr);
    panel->openSession();
    const QString session = panel->sessionId();
    QVERIFY(!session.isEmpty());
    auto* draft = panel->findChild<QTextEdit*>(QStringLiteral("agentPromptEditor"));
    auto* view = panel->findChild<MarkdownView*>();
    QVERIFY(draft && view);
    draft->setPlainText(QStringLiteral("未发送的草稿"));
    QTest::qWait(100);
    view->verticalScrollBar()->setRange(0, 100);
    view->verticalScrollBar()->setValue(37);
    QCOMPARE(view->verticalScrollBar()->value(), 37);
    auto* more = panel->findChild<QPushButton*>(QStringLiteral("agentMoreActions"));
    QVERIFY(more);
    QTest::mouseClick(more, Qt::LeftButton);
    auto* section = panel->findChild<QPushButton*>(QStringLiteral("agentEffortSection"));
    QVERIFY(section);
    QTest::mouseClick(section, Qt::LeftButton);
    auto* option = panel->findChild<QPushButton*>(QStringLiteral("agentEffortOption-max"));
    QVERIFY(option);
    QTest::mouseClick(option, Qt::LeftButton);
    // 强度保存到本会话；账号默认、消息页、草稿和阅读位置都不随之改变。
    MaiSession configured;
    auto* controller = window.findChild<AgentController*>();
    QVERIFY(controller && controller->agent().getSession(session.toStdString(), configured));
    QCOMPARE(QString::fromStdString(configured.reasoningEffort), QStringLiteral("max"));
    QVERIFY(!settings.contains(QStringLiteral("agent/reasoningEffort")));
    QCOMPARE(refreshed, 0);
    QCOMPARE(window.findChild<AgentChatPanel*>(), panel);
    QCOMPARE(panel->sessionId(), session);
    QCOMPARE(draft->toPlainText(), QStringLiteral("未发送的草稿"));
    QCOMPARE(view->verticalScrollBar()->value(), 37);
    QVERIFY(window.findChild<QDialog*>(QStringLiteral("agentModelDialog")) == nullptr);
    QVERIFY(panel->findChild<QPushButton*>(QStringLiteral("agentModelOption-glm-5.3")) != nullptr);
    QVERIFY(window.findChild<AgentChatPanel*>() == panel);
    QCOMPARE(panel->sessionId(), session);
    QCOMPARE(draft->toPlainText(), QStringLiteral("未发送的草稿"));
    QCOMPARE(view->verticalScrollBar()->value(), 37);
    settings.clear();
    QCoreApplication::setOrganizationName(oldOrganization);
    QCoreApplication::setApplicationName(oldApplication);
}

void MainWindowLayoutTest::agentReasoningEffortBeforeFirstSessionKeepsDeepSeek() {
    const QString oldOrganization = QCoreApplication::organizationName();
    const QString oldApplication = QCoreApplication::applicationName();
    QCoreApplication::setOrganizationName(QStringLiteral("MaiChatTests"));
    QCoreApplication::setApplicationName(QStringLiteral("InlineEffortBeforeSession"));
    QSettings settings;
    settings.clear();
    auto client = std::make_unique<FakeRemoteIMClient>();
    RemoteIMApplication app(QStringLiteral("desktop-user"), std::move(client));
    MainWindow window(app);
    const QHash<QString, QString> keys = {{QStringLiteral("glm"), QString(40, QLatin1Char('G'))},
                                          {QStringLiteral("deepseek"), QString(40, QLatin1Char('D'))}};
    window.setAccountModelKeys(keys);
    window.setModelKeyRefreshHandler([&] { window.setAccountModelKeys(keys); });
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    auto* panel = window.findChild<AgentChatPanel*>();
    QVERIFY(panel);
    QVERIFY(panel->sessionId().isEmpty());
    auto* editor = panel->findChild<QTextEdit*>(QStringLiteral("agentPromptEditor"));
    QVERIFY(editor);
    const QString draft = QStringLiteral("Unsent first-page draft before selecting effort");
    editor->setPlainText(draft);
    QTest::mouseClick(panel->findChild<QPushButton*>(QStringLiteral("agentMoreActions")), Qt::LeftButton);
    QTest::mouseClick(panel->findChild<QPushButton*>(QStringLiteral("agentEffortSection")), Qt::LeftButton);
    QTest::mouseClick(panel->findChild<QPushButton*>(QStringLiteral("agentEffortOption-low")), Qt::LeftButton);
    // 空白页第一次选择即建立自己的会话，不再更改所有会话的全局默认。
    QVERIFY(!settings.contains(QStringLiteral("agent/reasoningEffort")));
    QVERIFY(!panel->sessionId().isEmpty());
    QCOMPARE(editor->toPlainText(), draft);
    MaiSession session;
    QVERIFY(window.findChild<AgentController*>()->agent().getSession(panel->sessionId().toStdString(), session));
    QCOMPARE(QString::fromStdString(session.model), QStringLiteral("deepseek-flash"));
    QCOMPARE(QString::fromStdString(session.reasoningEffort), QStringLiteral("low"));
    settings.clear();
    QCoreApplication::setOrganizationName(oldOrganization);
    QCoreApplication::setApplicationName(oldApplication);
}

void MainWindowLayoutTest::agentAccountKeysUpdateModelWithoutSavingSecrets() {
    const QString originalOrganization = QCoreApplication::organizationName();
    const QString originalApplication = QCoreApplication::applicationName();
    QCoreApplication::setOrganizationName(QStringLiteral("MaiChatTests"));
    QCoreApplication::setApplicationName(QStringLiteral("AccountModelKeys"));
    QSettings settings;
    settings.clear();
    auto client = std::make_unique<FakeRemoteIMClient>();
    RemoteIMApplication app(QStringLiteral("desktop-user"), std::move(client));
    MainWindow window(app);

    const QHash<QString, QString> accountKeys = {{QStringLiteral("glm"), QString(40, QLatin1Char('G'))},
                                                 {QStringLiteral("deepseek"), QString(40, QLatin1Char('D'))}};
    window.setAccountModelKeys(accountKeys);
    auto* chip = window.findChild<QPushButton*>(QStringLiteral("agentModelChip"));
    QVERIFY(chip != nullptr);
    QCOMPARE(chip->text(), QStringLiteral("deepseek-flash"));
    auto* draft = window.findChild<QTextEdit*>(QStringLiteral("agentPromptEditor"));
    QVERIFY(draft != nullptr);
    draft->setPlainText(QStringLiteral("正在编辑的草稿"));
    window.setAccountModelKeys(accountKeys);
    draft = window.findChild<QTextEdit*>(QStringLiteral("agentPromptEditor"));
    QVERIFY(draft != nullptr);
    QCOMPARE(draft->toPlainText(), QStringLiteral("正在编辑的草稿"));
    settings.sync();
    QVERIFY(!settings.contains(QStringLiteral("agent/apiKey")));
    QVERIFY(!settings.contains(QStringLiteral("agent/deepseekApiKey")));
    QVERIFY(!settings.contains(QStringLiteral("agent/cloudServiceToken")));

    window.setAccountModelKeys({}, QStringLiteral("连接暂时失败"));
    chip = window.findChild<QPushButton*>(QStringLiteral("agentModelChip"));
    QVERIFY(chip != nullptr);
    QCOMPARE(chip->text(), QStringLiteral("模型连接失败"));
    QVERIFY(!chip->isEnabled());
    settings.clear();
    QCoreApplication::setOrganizationName(originalOrganization);
    QCoreApplication::setApplicationName(originalApplication);
}

void MainWindowLayoutTest::agentDeepSeekKeyAloneConfiguresMainModel() {
    const QString originalOrganization = QCoreApplication::organizationName();
    const QString originalApplication = QCoreApplication::applicationName();
    QCoreApplication::setOrganizationName(QStringLiteral("MaiChatTests"));
    QCoreApplication::setApplicationName(QStringLiteral("PartialModelKeys"));
    QSettings settings;
    settings.clear();
    auto client = std::make_unique<FakeRemoteIMClient>();
    RemoteIMApplication app(QStringLiteral("desktop-user"), std::move(client));
    MainWindow window(app);
    window.setAccountModelKeys({{QStringLiteral("deepseek"), QString(40, QLatin1Char('D'))}});
    auto* chip = window.findChild<QPushButton*>(QStringLiteral("agentModelChip"));
    QVERIFY(chip);
    QCOMPARE(chip->text(), QStringLiteral("deepseek-flash"));
    QVERIFY(chip->isEnabled());
    auto* panel = window.findChild<AgentChatPanel*>();
    QVERIFY(panel);
    panel->openSession();
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    auto* more = panel->findChild<QPushButton*>(QStringLiteral("agentMoreActions"));
    QVERIFY(more);
    QTest::mouseClick(more, Qt::LeftButton);
    auto* section = panel->findChild<QPushButton*>(QStringLiteral("agentEffortSection"));
    QVERIFY(section);
    QTest::mouseClick(section, Qt::LeftButton);
    auto* option = panel->findChild<QPushButton*>(QStringLiteral("agentEffortOption-max"));
    QVERIFY(option);
    QTest::mouseClick(option, Qt::LeftButton);
    QCOMPARE(chip->text(), QStringLiteral("deepseek-flash"));
    QVERIFY(chip->isEnabled());
    QCOMPARE(window.findChild<AgentChatPanel*>(), panel);
    QVERIFY(window.findChild<QDialog*>(QStringLiteral("agentModelDialog")) == nullptr);
    settings.sync();
    MaiSession configured;
    QVERIFY(window.findChild<AgentController*>()->agent().getSession(
        panel->sessionId().toStdString(), configured));
    QCOMPARE(QString::fromStdString(configured.reasoningEffort), QStringLiteral("max"));
    QVERIFY(!settings.contains(QStringLiteral("agent/reasoningEffort")));
    settings.clear();
    QCoreApplication::setOrganizationName(originalOrganization);
    QCoreApplication::setApplicationName(originalApplication);
}

void MainWindowLayoutTest::delayedModelCredentialsApplyOnlyToTheRequestedSession() {
    const QString previousOrganization = QCoreApplication::organizationName();
    const QString previousApplication = QCoreApplication::applicationName();
    const bool previousTestMode = QStandardPaths::isTestModeEnabled();
    QCoreApplication::setOrganizationName(QStringLiteral("MaiChatSessionModelTests"));
    QCoreApplication::setApplicationName(QUuid::createUuid().toString(QUuid::WithoutBraces));
    QStandardPaths::setTestModeEnabled(true);
    const auto restore = qScopeGuard([previousOrganization, previousApplication, previousTestMode] {
        QSettings ownSettings;
        ownSettings.clear();
        QCoreApplication::setOrganizationName(previousOrganization);
        QCoreApplication::setApplicationName(previousApplication);
        QStandardPaths::setTestModeEnabled(previousTestMode);
    });
    QSettings settings;
    settings.setValue(QStringLiteral("agent/model"), QStringLiteral("legacy-model-choice"));
    settings.sync();
    QTemporaryDir workspace;
    QVERIFY(workspace.isValid());
    auto client = std::make_unique<FakeRemoteIMClient>();
    RemoteIMApplication app(
        QStringLiteral("model-fixture-") + QUuid::createUuid().toString(QUuid::WithoutBraces),
        std::move(client));
    MainWindow window(app);
    window.setAccountModelKeys(
        {{QStringLiteral("deepseek"), QStringLiteral("fixture-deepseek-key")}});
    int refreshes = 0;
    window.setModelKeyRefreshHandler([&] { ++refreshes; });
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    auto* panel = window.findChild<AgentChatPanel*>();
    auto* controller = window.findChild<AgentController*>();
    QVERIFY(panel && controller);
    const QString requestedSession = controller->createSession(workspace.path());
    const QString subsequentlyVisibleSession = controller->createSession(workspace.path());
    QVERIFY(!requestedSession.isEmpty() && !subsequentlyVisibleSession.isEmpty());
    QVERIFY(controller->setModel(requestedSession, QStringLiteral("deepseek-flash")));
    QVERIFY(controller->setModel(subsequentlyVisibleSession, QStringLiteral("deepseek-flash")));
    panel->openSession(requestedSession);
    QVERIFY(QMetaObject::invokeMethod(panel, "modelChangeRequested", Qt::DirectConnection,
                                      Q_ARG(QString, QStringLiteral("glm-5.3-flash"))));
    QCOMPARE(refreshes, 1);
    panel->openSession(subsequentlyVisibleSession);
    window.setAccountModelKeys(
        {{QStringLiteral("deepseek"), QStringLiteral("fixture-deepseek-key")},
         {QStringLiteral("glm"), QStringLiteral("fixture-glm-key")}});
    MaiSession requested, visible;
    QVERIFY(controller->agent().getSession(requestedSession.toStdString(), requested));
    QVERIFY(controller->agent().getSession(subsequentlyVisibleSession.toStdString(), visible));
    QVERIFY2(requested.model == "glm-5.3-flash" && visible.model == "deepseek-flash",
             qPrintable(QStringLiteral("Requested session=%1; subsequently visible session=%2")
                            .arg(QString::fromStdString(requested.model),
                                 QString::fromStdString(visible.model))));
    QCOMPARE(panel->sessionId(), subsequentlyVisibleSession);
    auto* displayed = panel->findChild<QLabel*>(QStringLiteral("agentModelValue"));
    QVERIFY(displayed);
    QCOMPARE(displayed->text(), QStringLiteral("deepseek-flash"));

    // A later explicit choice of the current model cancels the pending change.
    window.setAccountModelKeys(
        {{QStringLiteral("deepseek"), QStringLiteral("fixture-deepseek-key")}});
    QVERIFY(QMetaObject::invokeMethod(panel, "modelChangeRequested", Qt::DirectConnection,
                                      Q_ARG(QString, QStringLiteral("glm-5.3-flash"))));
    QCOMPARE(refreshes, 2);
    QVERIFY(QMetaObject::invokeMethod(panel, "modelChangeRequested", Qt::DirectConnection,
                                      Q_ARG(QString, QStringLiteral("deepseek-flash"))));
    window.setAccountModelKeys(
        {{QStringLiteral("deepseek"), QStringLiteral("fixture-deepseek-key")},
         {QStringLiteral("glm"), QStringLiteral("fixture-glm-key")}});
    QVERIFY(controller->agent().getSession(subsequentlyVisibleSession.toStdString(), visible));
    QCOMPARE(QString::fromStdString(visible.model), QStringLiteral("deepseek-flash"));
    QCOMPARE(displayed->text(), QStringLiteral("deepseek-flash"));
    settings.sync();
    QCOMPARE(settings.value(QStringLiteral("agent/model")).toString(),
             QStringLiteral("legacy-model-choice"));
}

void MainWindowLayoutTest::agentModelFailureRefreshesWithoutDialog() {
    auto client = std::make_unique<FakeRemoteIMClient>();
    RemoteIMApplication app(QStringLiteral("desktop-user"), std::move(client));
    MainWindow window(app);
    int retries = 0;
    window.setModelKeyRefreshHandler([&] { ++retries; });
    auto* draft = window.findChild<QTextEdit*>(QStringLiteral("agentPromptEditor"));
    QVERIFY(draft != nullptr);
    draft->setPlainText(QStringLiteral("尚未发送的草稿"));
    window.setAccountModelKeys({}, QStringLiteral("连接暂时失败"));
    draft = window.findChild<QTextEdit*>(QStringLiteral("agentPromptEditor"));
    QVERIFY(draft != nullptr);
    QCOMPARE(draft->toPlainText(), QStringLiteral("尚未发送的草稿"));
    auto* send = window.findChild<QPushButton*>(QStringLiteral("agentSendButton"));
    QVERIFY(send);
    send->click();
    QCOMPARE(retries, 1);
    QCOMPARE(draft->toPlainText(), QStringLiteral("尚未发送的草稿"));
    QVERIFY(window.findChild<QDialog*>(QStringLiteral("agentModelDialog")) == nullptr);
}

void MainWindowLayoutTest::leftNavigationRailIsResizableAndWider() {
    auto client = std::make_unique<FakeRemoteIMClient>();
    RemoteIMApplication app(QStringLiteral("desktop-user"), std::move(client));
    app.addContact(QStringLiteral("phone-user"), QStringLiteral("iPhone"));

    MainWindow window(app);

    auto* rootNavigationSplitter = window.findChild<QSplitter*>(QStringLiteral("rootNavigationSplitter"));
    auto* navRail = window.findChild<QWidget*>(QStringLiteral("navRail"));
    auto* contactsNavButton = window.findChild<QPushButton*>(QStringLiteral("contactsNavButton"));
    QVERIFY(rootNavigationSplitter != nullptr);
    QVERIFY(navRail != nullptr);
    QVERIFY(contactsNavButton != nullptr);
    QCOMPARE(rootNavigationSplitter->orientation(), Qt::Horizontal);
    QVERIFY(rootNavigationSplitter->childrenCollapsible() == false);
    // 导航按钮按需求只留图标：文字移到 accessibleName / tooltip。这里同时钉住三点——
    // 确实没有文字、仍能认出是哪个入口、图标没漏设（只去文字不给图标就成了空按钮）。
    QVERIFY(contactsNavButton->text().isEmpty());
    QCOMPARE(contactsNavButton->accessibleName(), QStringLiteral("好友列表"));
    QVERIFY(!contactsNavButton->icon().isNull());
    // 纯图标之后导航栏按需求收窄，并且不再可拉伸：min==max 才能保证它不会被
    // splitter 拖回原来那么宽。
    QVERIFY2(navRail->maximumWidth() <= 96, "导航栏应当是窄条，不该再占一整列");
    QCOMPARE(navRail->minimumWidth(), navRail->maximumWidth());
    QCOMPARE(rootNavigationSplitter->handleWidth(), 1);

    // 一列图标的尺寸必须一致：混用尺寸时肉眼看到的就是「有的大有的小」，
    // 而每个按钮单独看都正常，所以要横向比。
    const QList<QPushButton*> navButtons{window.findChild<QPushButton*>(QStringLiteral("messagesNavButton")),
                                         window.findChild<QPushButton*>(QStringLiteral("cloudNavButton")),
                                         window.findChild<QPushButton*>(QStringLiteral("agentNavButton")),
                                         contactsNavButton,
                                         window.findChild<QPushButton*>(QStringLiteral("settingsNavButton"))};
    for (QPushButton* button : navButtons) {
        QVERIFY(button != nullptr);
        QCOMPARE(button->iconSize(), navButtons.first()->iconSize());
    }

    // 纯图标导航栏里，图标是四个入口唯一的辨识依据，太小就只能靠位置记。
    // 但放大图标最容易越过的界线是「图标撑出按钮被裁掉」，所以两条一起钉：
    // 够大，且仍装得进按钮。
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    for (QPushButton* button : navButtons) {
        QVERIFY2(button->iconSize().width() >= 24, "导航栏图标应当比会话栏的「+」大一档");
        QVERIFY2(button->iconSize().width() + 8 <= button->width(), "图标放大后必须仍装得进按钮，否则会被裁掉");
        QVERIFY2(button->width() <= navRail->width(), "按钮不能宽过导航栏本身");
    }
}

void MainWindowLayoutTest::navigationIconsSitNearTopEdge() {
    auto client = std::make_unique<FakeRemoteIMClient>();
    RemoteIMApplication app(QStringLiteral("desktop-user"), std::move(client));
    MainWindow window(app);

    auto* navRail = window.findChild<QWidget*>(QStringLiteral("navRail"));
    QVERIFY(navRail != nullptr);
    QVERIFY(navRail->layout() != nullptr);
    QCOMPARE(navRail->layout()->contentsMargins().top(), 0);
}

void MainWindowLayoutTest::cloudDriveNavigationUsesMainPageInRequestedOrder() {
    auto client = std::make_unique<FakeRemoteIMClient>();
    RemoteIMApplication app(QStringLiteral("desktop-user"), std::move(client));
    MainWindow window(app);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));

    const QStringList names{QStringLiteral("messagesNavButton"), QStringLiteral("cloudNavButton"),
                            QStringLiteral("agentNavButton"), QStringLiteral("contactsNavButton"),
                            QStringLiteral("settingsNavButton")};
    const QStringList labels{QStringLiteral("消息"), QStringLiteral("云盘"), QStringLiteral("创作中心"),
                             QStringLiteral("好友列表"), QStringLiteral("我的")};
    int previousTop = -1;
    for (int index = 0; index < names.size(); ++index) {
        auto* button = window.findChild<QPushButton*>(names.at(index));
        QVERIFY(button != nullptr);
        QCOMPARE(button->accessibleName(), labels.at(index));
        QVERIFY(button->isVisible());
        const int top = button->mapTo(&window, QPoint(0, 0)).y();
        QVERIFY(top > previousTop);
        previousTop = top;
    }
    QVERIFY(window.findChild<QPushButton*>(QStringLiteral("remoteNavButton")) == nullptr);
    QVERIFY(window.findChild<QPushButton*>(QStringLiteral("remoteDesktopButton")) != nullptr);
    QVERIFY(window.findChild<QPushButton*>(QStringLiteral("remoteViewerReturnButton")) != nullptr);
    const QByteArray messagesCapture = qgetenv("MAICHAT_CAPTURE_MESSAGES_NAV");
    if (!messagesCapture.isEmpty()) QVERIFY(window.grab().save(QString::fromUtf8(messagesCapture)));

    auto* cloudButton = window.findChild<QPushButton*>(QStringLiteral("cloudNavButton"));
    cloudButton->click();
    auto* stack = window.findChild<QStackedWidget*>(QStringLiteral("contentStack"));
    auto* page = window.findChild<QWidget*>(QStringLiteral("cloudDrivePage"));
    auto* drive = window.findChild<QWidget*>(QStringLiteral("cloudDriveDialog"));
    QVERIFY(stack != nullptr);
    QVERIFY(page != nullptr);
    QVERIFY(drive != nullptr);
    QCOMPARE(stack->currentWidget(), page);
    QVERIFY(drive->isVisible());
    QVERIFY(!drive->isWindow());
    QVERIFY(cloudButton->property("selected").toBool());
    QVERIFY(window.findChild<QWidget*>(QStringLiteral("settingsCloudDriveOpen")) == nullptr);
    const QByteArray capture = qgetenv("MAICHAT_CAPTURE_CLOUD_NAV");
    if (!capture.isEmpty()) QVERIFY(window.grab().save(QString::fromUtf8(capture)));
}

void MainWindowLayoutTest::removesRedundantChromeLabels() {
    auto client = std::make_unique<FakeRemoteIMClient>();
    RemoteIMApplication app(QStringLiteral("desktop-user"), std::move(client));
    app.addContact(QStringLiteral("phone-user"), QStringLiteral("iPhone"));

    MainWindow window(app);

    QVERIFY(window.windowTitle().trimmed().isEmpty());
    QVERIFY(window.findChild<QLabel*>(QStringLiteral("accountMark")) == nullptr);
    QVERIFY(window.findChild<QLabel*>(QStringLiteral("accountLabel")) == nullptr);
}

void MainWindowLayoutTest::conversationListsUseDelegateItemsForSmoothScrolling() {
    auto client = std::make_unique<FakeRemoteIMClient>();
    RemoteIMApplication app(QStringLiteral("desktop-user"), std::move(client));
    for (int index = 0; index < 80; ++index) {
        app.addContact(QStringLiteral("phone-user-%1").arg(index), QStringLiteral("iPhone %1").arg(index));
    }
    app.chatState().upsertContact(RemoteIMContact{QStringLiteral("phone-user-0"), QStringLiteral("iPhone 0"),
                                                  QStringLiteral("https://example.com/iphone.png")});

    MainWindow window(app);
    auto* conversationList = window.findChild<QListWidget*>(QStringLiteral("conversationList"));
    auto* contactsNavButton = window.findChild<QPushButton*>(QStringLiteral("contactsNavButton"));
    QVERIFY(conversationList != nullptr);
    QVERIFY(conversationList->uniformItemSizes());
    QCOMPARE(conversationList->item(0)->data(Qt::UserRole + 5).toString(),
             QStringLiteral("https://example.com/iphone.png"));
    for (int index = 0; index < conversationList->count(); ++index) {
        QVERIFY(conversationList->itemWidget(conversationList->item(index)) == nullptr);
    }

    QVERIFY(contactsNavButton != nullptr);
    contactsNavButton->click();
    auto* contactsList = window.findChild<QListWidget*>(QStringLiteral("contactsList"));
    QVERIFY(contactsList != nullptr);
    // 通讯录有意不等高：分组表头比联系人行矮。开着 uniformItemSizes 会让所有行
    // 都按第一行的高度画，表头和联系人叠在一起。会话列表仍然是等高的。
    QVERIFY(!contactsList->uniformItemSizes());
    QCOMPARE(contactsList->item(0)->data(Qt::UserRole + 5).toString(),
             QStringLiteral("https://example.com/iphone.png"));
    for (int index = 0; index < contactsList->count(); ++index) {
        QVERIFY(contactsList->itemWidget(contactsList->item(index)) == nullptr);
    }
}

// Markdown 气泡使用实际排版高度，文本与气泡边界不能被裁切。
void MainWindowLayoutTest::rendersMarkdownMessageContent() {
    auto client = std::make_unique<FakeRemoteIMClient>();
    RemoteIMApplication app(QStringLiteral("desktop-user"), std::move(client));
    app.addContact(QStringLiteral("phone-user"), QStringLiteral("iPhone"));
    const QString hiddenPrefix = QStringLiteral("\u2063\u200B\u200C\u200D\u2063");
    RemoteIMMessage restoredMessage;
    restoredMessage.fromUserId = QStringLiteral("phone-user");
    restoredMessage.toUserId = QStringLiteral("desktop-user");
    restoredMessage.text =
        hiddenPrefix +
        QStringLiteral("# Win/Mac 每周 Crash 详细报表 `code`\n\n**重点**\n\n- 第一条\n- [链接](https://example.com)");
    restoredMessage.direction = RemoteIMMessageDirection::Incoming;
    app.chatState().appendMessageForRestore(restoredMessage);

    MainWindow window(app);
    auto* markdownView = window.findChild<MarkdownLabel*>(QStringLiteral("messageMarkdownView"));
    QVERIFY(markdownView != nullptr);
    // 语法被吃掉了，不是原样显示出来。
    QVERIFY(!markdownView->plainText().contains(QStringLiteral("# Win/Mac")));
    QVERIFY(markdownView->plainText().contains(QStringLiteral("重点")));

    // 结构：一级标题、标题里的行内代码、两项列表、列表里的链接。
    // 原来这些是去 QTextDocument 里翻 fragment 查的；换成自绘之后没有文档可翻，
    // 改成对着视图手上的原文解析出块树来断言——视图画的就是这棵树。
    const auto blocks = MarkdownDocument::parse(markdownView->markdown()).blocks();
    QCOMPARE(blocks.size(), 3);
    QCOMPARE(blocks.at(0).kind, MarkdownBlockKind::Heading);
    QCOMPARE(blocks.at(0).headingLevel, 1);
    bool sawInlineCode = false;
    for (const MarkdownSpan& span : blocks.at(0).spans) {
        if (span.styles.testFlag(MarkdownStyle::Code)) sawInlineCode = true;
    }
    QVERIFY(sawInlineCode);
    QCOMPARE(blocks.at(2).kind, MarkdownBlockKind::List);
    QCOMPARE(blocks.at(2).items.size(), 2);
    bool sawLink = false;
    for (const MarkdownListItem& item : blocks.at(2).items) {
        for (const MarkdownSpan& span : item.spans) {
            if (span.href == QStringLiteral("https://example.com")) sawLink = true;
        }
    }
    QVERIFY(sawLink);

    // 「认出来是标题」证明不了「画出来不一样」——原来那条 bug 恰恰是标题被按
    // 正文字号画了。拿视图自己那份主题去量：一级标题必须真的比同样的字高。
    MarkdownLabel headingProbe;
    headingProbe.setTheme(markdownView->theme());
    headingProbe.setMarkdown(QStringLiteral("# 每周报表"));
    MarkdownLabel bodyProbe;
    bodyProbe.setTheme(markdownView->theme());
    bodyProbe.setMarkdown(QStringLiteral("每周报表"));
    QVERIFY2(headingProbe.heightForWidth(400) > bodyProbe.heightForWidth(400),
             qPrintable(QStringLiteral("标题和正文一样高：%1 vs %2")
                            .arg(headingProbe.heightForWidth(400))
                            .arg(bodyProbe.heightForWidth(400))));
}

void MainWindowLayoutTest::notificationClickPreservesWindowState_data() {
    QTest::addColumn<int>("state");
    QTest::newRow("normal") << int(Qt::WindowNoState);
    QTest::newRow("maximized") << int(Qt::WindowMaximized);
    QTest::newRow("minimized") << int(Qt::WindowMinimized);
    QTest::newRow("maximized-minimized") << int(Qt::WindowMaximized | Qt::WindowMinimized);
    QTest::newRow("fullscreen") << int(Qt::WindowFullScreen);
}

void MainWindowLayoutTest::notificationClickPreservesWindowState() {
    QFETCH(int, state);
    auto client = std::make_unique<FakeRemoteIMClient>();
    RemoteIMApplication app(QStringLiteral("desktop-user"), std::move(client));
    app.addContact(QStringLiteral("phone-user"), QStringLiteral("iPhone"));
    app.addContact(QStringLiteral("other-user"), QStringLiteral("Other"));
    app.selectPeer(QStringLiteral("other-user"));
    MainWindow window(app);
    window.setWindowState(Qt::WindowStates(state));
    window.show();
    QCoreApplication::processEvents();
    window.openConversationFromNotification(QStringLiteral("phone-user"));
    QVERIFY(window.isVisible());
    QVERIFY(!window.isMinimized());
    QCOMPARE(window.isMaximized(), bool(state & Qt::WindowMaximized));
    QCOMPARE(window.isFullScreen(), bool(state & Qt::WindowFullScreen));
    QCOMPARE(app.chatState().selectedPeerId(), QStringLiteral("phone-user"));
}

void MainWindowLayoutTest::doNotDisturbReadsLatestPreferenceAndRetainsIncomingMessages() {
    ScopedNotificationSettings preferences;
    auto database = std::make_unique<LocalMessageDatabase>(preferences.directory() + QStringLiteral("/owned.sqlite"));
    QVERIFY(database->isOpen());
    auto* stored = database.get();
    auto client = std::make_unique<FakeRemoteIMClient>();
    auto* incoming = client.get();
    RemoteIMApplication app(QStringLiteral("dnd-owned-user"), std::move(client), std::move(database));
    app.addContact(QStringLiteral("dnd-owned-peer"), QStringLiteral("DND test message"));
    app.addContact(QStringLiteral("dnd-other-peer"), QStringLiteral("Owned other conversation"));
    app.selectPeer(QStringLiteral("dnd-other-peer"));
    MainWindow window(app);
    window.showMinimized();
    QCoreApplication::processEvents();
    auto* tray = window.findChild<QSystemTrayIcon*>();
    QVERIFY2(tray && tray->isVisible(), "This host test needs the actual Windows tray submission path");
    ScopedNotificationSubmissions submissions;
    // Admission precedes the change: dispatch must use the latest preference,
    // not a value captured when the window or queued callback was created.
    QTimer::singleShot(0, incoming, [incoming] {
        incoming->emitIncomingText(QStringLiteral("dnd-owned-peer"), QStringLiteral("owned silent message"));
    });
    QSettings().setValue(QStringLiteral("maichat.preferences.do-not-disturb"), true);
    QTRY_COMPARE(app.chatState().messagesWith(QStringLiteral("dnd-owned-peer")).size(), 1);
    QCOMPARE(app.chatState().unreadCount(QStringLiteral("dnd-owned-peer")), 1);
    QCOMPARE(stored->loadRecentMessages(QStringLiteral("dnd-owned-peer"), 10).size(), 1);
    QCOMPARE(submissions.count, 0);
    QVERIFY(tray->isVisible());

    QSettings().setValue(QStringLiteral("maichat.preferences.do-not-disturb"), false);
    QCoreApplication::processEvents();
    QCOMPARE(submissions.count, 0); // Disabling does not replay silent history.
    incoming->emitIncomingText(QStringLiteral("dnd-owned-peer"), QStringLiteral("owned new realtime message"));
    QCOMPARE(submissions.count, 1); // Silent traffic did not consume the delivery gate.
    QCOMPARE(app.chatState().unreadCount(QStringLiteral("dnd-owned-peer")), 2);
    QCOMPARE(stored->loadRecentMessages(QStringLiteral("dnd-owned-peer"), 10).size(), 2);
}

void MainWindowLayoutTest::doNotDisturbSwitchPersistsAndClearsNotificationDeliveryState() {
    ScopedNotificationSettings preferences;
    auto client = std::make_unique<FakeRemoteIMClient>();
    auto* incoming = client.get();
    RemoteIMApplication app(QStringLiteral("dnd-owned-settings"), std::move(client));
    app.addContact(QStringLiteral("dnd-owned-peer"), QStringLiteral("DND test message"));
    app.addContact(QStringLiteral("dnd-other-peer"), QStringLiteral("Owned other conversation"));
    app.selectPeer(QStringLiteral("dnd-other-peer"));
    {
        MainWindow window(app);
        window.showMinimized();
        QCoreApplication::processEvents();
        auto* tray = window.findChild<QSystemTrayIcon*>();
        QVERIFY(tray && tray->isVisible());
        auto* toggle = window.findChild<QCheckBox*>(QStringLiteral("settingsDoNotDisturb"));
        QVERIFY2(toggle, "Settings must expose the per-device do-not-disturb switch");
        QVERIFY(!toggle->isChecked());
        ScopedNotificationSubmissions submissions;
        incoming->emitIncomingText(QStringLiteral("dnd-owned-peer"), QStringLiteral("owned earlier notification"));
        QCOMPARE(submissions.count, 1);
        toggle->setChecked(true);
        QCOMPARE(QSettings().value(QStringLiteral("maichat.preferences.do-not-disturb")).toBool(), true);
        QVERIFY(tray->isVisible());
        QVERIFY(QMetaObject::invokeMethod(tray, "messageClicked", Qt::DirectConnection));
        QCOMPARE(app.chatState().selectedPeerId(), QStringLiteral("dnd-other-peer"));
        incoming->emitIncomingText(QStringLiteral("dnd-owned-peer"), QStringLiteral("owned silent second message"));
        QCOMPARE(submissions.count, 1);
        toggle->setChecked(false);
        QCoreApplication::processEvents();
        QCOMPARE(submissions.count, 1);
        incoming->emitIncomingText(QStringLiteral("dnd-owned-peer"), QStringLiteral("owned new notification"));
        QCOMPARE(submissions.count, 2);
        QCOMPARE(app.chatState().unreadCount(QStringLiteral("dnd-owned-peer")), 3);
        toggle->setChecked(true);
        if (!qgetenv("MAICHAT_DND_CAPTURE").isEmpty()) {
            window.resize(1280, 820);
            window.showNormal();
            window.findChild<QPushButton*>(QStringLiteral("settingsNavButton"))->click();
            QCoreApplication::processEvents();
            QVERIFY(window.grab().save(QString::fromUtf8(qgetenv("MAICHAT_DND_CAPTURE"))));
        }
    }
    QSettings().sync();
    MainWindow recreated(app);
    auto* restored = recreated.findChild<QCheckBox*>(QStringLiteral("settingsDoNotDisturb"));
    QVERIFY(restored && restored->isChecked());
    QCOMPARE(QSettings().value(QStringLiteral("maichat.preferences.do-not-disturb")).toBool(), true);
}

void MainWindowLayoutTest::markdownBubbleHasCompactBottomSpacing_data() {
    QTest::addColumn<QString>("text");
    QTest::newRow("one-line") << QStringLiteral("同样先调节 iOS 的");
    QTest::newRow("paragraphs") << QStringLiteral("已调整样式。\n\n现在可以直接看效果。");
    QTest::newRow("list") << QStringLiteral("- 第一项\n- 最后一项");
    QTest::newRow("code") << QStringLiteral("```cpp\nint a = 1;\n```");
    QTest::newRow("table") << QStringLiteral("| A | B |\n| --- | --- |\n| 最后一行 | 内容 |");
    QTest::newRow("quote") << QStringLiteral("> 引用的最后一行");
}

void MainWindowLayoutTest::markdownBubbleHasCompactBottomSpacing() {
    QFETCH(QString, text);
    auto client = std::make_unique<FakeRemoteIMClient>();
    RemoteIMApplication app(QStringLiteral("desktop-user"), std::move(client));
    app.addContact(QStringLiteral("phone-user"), QStringLiteral("iPhone"));
    app.chatState().receiveText(QStringLiteral("phone-user"), text);
    MainWindow window(app);
    window.resize(1200, 850);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    auto* view = window.findChild<MarkdownLabel*>(QStringLiteral("messageMarkdownView"));
    auto* bubble = window.findChild<QWidget*>(QStringLiteral("messageBubbleIncoming"));
    QVERIFY(view != nullptr);
    QVERIFY(bubble != nullptr);
    QVERIFY(bubble->isAncestorOf(view));
    QTRY_VERIFY(view->height() > 0);
    QTest::qWait(100);
    // 部件的高度就是内容的高度，一个像素的富余都不留。
    // 原来那条路要靠压 lastBlock 的 bottomMargin 才做得到，自绘这边是
    // 排版的结果直接当高度——这条断言卡的是「有人又在外面加余量」。
    QCOMPARE(view->height(), view->heightForWidth(view->width()));
    const int bottomInset = bubble->height() - (view->mapTo(bubble, QPoint()).y() + view->height());
    QVERIFY2(bottomInset >= 0 && bottomInset <= UiZoom::s(8) + 1,
             qPrintable(QStringLiteral("bubble bottom inset=%1").arg(bottomInset)));
    QVERIFY(view->height() >= view->contentHeight());
}

// 审批按钮的可见状态与发送结构化决策绑定，不能仅更新本地按钮而没有真实发送。
void MainWindowLayoutTest::rendersApprovalButtonsAndSendsStructuredDecision() {
    auto client = std::make_unique<FakeRemoteIMClient>();
    auto* fakeClient = client.get();
    RemoteIMApplication app(QStringLiteral("desktop-user"), std::move(client));
    app.addContact(QStringLiteral("multi-ai-code"), QStringLiteral("Multi-AI Code"));

    RemoteIMMessage request;
    request.id = QStringLiteral("approval-message-1");
    request.fromUserId = QStringLiteral("multi-ai-code");
    request.toUserId = QStringLiteral("desktop-user");
    request.text = QStringLiteral("Codex 请求执行一条高风险命令");
    request.direction = RemoteIMMessageDirection::Incoming;
    request.status = RemoteIMMessageStatus::Received;
    request.origin = RemoteIMMessageOrigin::Machine;
    request.hasApprovalRequest = true;
    request.approvalRequest = RemoteIMApprovalRequest{
        QStringLiteral("approval-ui-1"),
        {RemoteIMApprovalAction::ApproveOnce, RemoteIMApprovalAction::ApprovePrefix, RemoteIMApprovalAction::Reject}};
    app.chatState().appendMessageForRestore(request);

    MainWindow window(app);
    window.show();
    QCoreApplication::processEvents();

    QVERIFY(window.findChild<QPushButton*>(QStringLiteral("approvalApproveOnceButton")) != nullptr);
    auto* prefixButton = window.findChild<QPushButton*>(QStringLiteral("approvalApprovePrefixButton"));
    QVERIFY(prefixButton != nullptr);
    QVERIFY(window.findChild<QPushButton*>(QStringLiteral("approvalRejectButton")) != nullptr);

    fakeClient->failNext(QStringLiteral("network unavailable"));
    fakeClient->deferNextSend();
    QTest::mouseClick(prefixButton, Qt::LeftButton);
    QCoreApplication::processEvents();

    auto* sendingLabel = window.findChild<QLabel*>(QStringLiteral("approvalSentLabel"));
    QVERIFY(sendingLabel != nullptr);
    QVERIFY(sendingLabel->text().contains(QStringLiteral("正在发送")));

    // 网络回调到达前切走再切回，会销毁点击时的气泡。失败处理必须依据消息模型
    // 重建当前气泡，不能只操作已经失效的旧控件指针。
    app.addContact(QStringLiteral("other-peer"), QStringLiteral("Other"));
    app.selectPeer(QStringLiteral("multi-ai-code"));
    QCoreApplication::processEvents();
    confirmNextContactDeletion();  // 关闭发送失败提示的模态框，继续检查按钮是否恢复。
    fakeClient->finishDeferredSend();
    QCoreApplication::processEvents();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);

    prefixButton = window.findChild<QPushButton*>(QStringLiteral("approvalApprovePrefixButton"));
    QVERIFY(prefixButton != nullptr);
    QVERIFY(prefixButton->isVisible());
    QVERIFY(window.findChild<QLabel*>(QStringLiteral("approvalSentLabel")) == nullptr);

    QTest::mouseClick(prefixButton, Qt::LeftButton);
    QCoreApplication::processEvents();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);

    QCOMPARE(fakeClient->lastTextPeerId(), QStringLiteral("multi-ai-code"));
    QCOMPARE(fakeClient->lastApprovalToken(), QStringLiteral("approval-ui-1"));
    QCOMPARE(fakeClient->lastApprovalAction(), RemoteIMApprovalAction::ApprovePrefix);
    auto* sentLabel = window.findChild<QLabel*>(QStringLiteral("approvalSentLabel"));
    QVERIFY(sentLabel != nullptr);
    QVERIFY(sentLabel->text().contains(QStringLiteral("已发送")));

    // 再切走/切回强制重建：已发送状态来自出站决定消息，而不是旧气泡控件。
    app.selectPeer(QStringLiteral("other-peer"));
    app.selectPeer(QStringLiteral("multi-ai-code"));
    QCoreApplication::processEvents();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QVERIFY(window.findChild<QPushButton*>(QStringLiteral("approvalApproveOnceButton")) == nullptr);
    QVERIFY(window.findChild<QLabel*>(QStringLiteral("approvalSentLabel")) != nullptr);
}

void MainWindowLayoutTest::restoresSubmittedApprovalStateFromDatabase() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString databasePath = dir.filePath(QStringLiteral("messages.db"));
    {
        LocalMessageDatabase database(databasePath);
        QVERIFY(database.isOpen());
        database.upsertContact(RemoteIMContact{QStringLiteral("multi-ai-code"), QStringLiteral("Multi-AI Code")});

        RemoteIMMessage request;
        request.id = QStringLiteral("approval-restart-request");
        request.fromUserId = QStringLiteral("multi-ai-code");
        request.toUserId = QStringLiteral("desktop-user");
        request.text = QStringLiteral("Codex 请求执行一条高风险命令");
        request.direction = RemoteIMMessageDirection::Incoming;
        request.status = RemoteIMMessageStatus::Received;
        request.origin = RemoteIMMessageOrigin::Machine;
        request.createdAtMillis = 1700000000100LL;
        request.hasApprovalRequest = true;
        request.approvalRequest =
            RemoteIMApprovalRequest{QStringLiteral("approval-restart-1"),
                                    {RemoteIMApprovalAction::ApproveOnce, RemoteIMApprovalAction::Reject}};
        QVERIFY(database.insertMessageIfAbsent(request, QStringLiteral("multi-ai-code")));

        RemoteIMMessage decision;
        decision.id = QStringLiteral("approval-restart-decision");
        decision.fromUserId = QStringLiteral("desktop-user");
        decision.toUserId = QStringLiteral("multi-ai-code");
        decision.text = QStringLiteral("审批操作：同意本次");
        decision.direction = RemoteIMMessageDirection::Outgoing;
        decision.status = RemoteIMMessageStatus::Sent;
        decision.origin = RemoteIMMessageOrigin::Human;
        decision.createdAtMillis = 1700000000200LL;
        decision.hasApprovalDecision = true;
        decision.approvalDecision =
            RemoteIMApprovalDecision{QStringLiteral("approval-restart-1"), RemoteIMApprovalAction::ApproveOnce};
        QVERIFY(database.insertMessageIfAbsent(decision, QStringLiteral("multi-ai-code")));
    }

    auto client = std::make_unique<FakeRemoteIMClient>();
    auto database = std::make_unique<LocalMessageDatabase>(databasePath);
    QVERIFY(database->isOpen());
    RemoteIMApplication app(QStringLiteral("desktop-user"), std::move(client), std::move(database));
    app.selectPeer(QStringLiteral("multi-ai-code"));

    MainWindow window(app);
    window.show();
    QCoreApplication::processEvents();

    QVERIFY(window.findChild<QPushButton*>(QStringLiteral("approvalApproveOnceButton")) == nullptr);
    auto* sentLabel = window.findChild<QLabel*>(QStringLiteral("approvalSentLabel"));
    QVERIFY(sentLabel != nullptr);
    QVERIFY(sentLabel->text().contains(QStringLiteral("已发送")));
}

// 服务端最终审批状态优先于延迟到达的本机发送回执，防止已经失效的按钮重新出现。
void MainWindowLayoutTest::authoritativeApprovalResolutionOverridesLateSentDecision() {
    auto client = std::make_unique<FakeRemoteIMClient>();
    RemoteIMApplication app(QStringLiteral("desktop-user"), std::move(client));
    app.addContact(QStringLiteral("multi-ai-code"), QStringLiteral("Multi-AI Code"));

    RemoteIMMessage request;
    request.id = QStringLiteral("approval-authoritative-request");
    request.fromUserId = QStringLiteral("multi-ai-code");
    request.toUserId = QStringLiteral("desktop-user");
    request.text = QStringLiteral("需要审批");
    request.direction = RemoteIMMessageDirection::Incoming;
    request.status = RemoteIMMessageStatus::Received;
    request.origin = RemoteIMMessageOrigin::Machine;
    request.hasApprovalRequest = true;
    request.approvalRequest =
        RemoteIMApprovalRequest{QStringLiteral("approval-authoritative-1"),
                                {RemoteIMApprovalAction::ApproveOnce, RemoteIMApprovalAction::Reject}};
    app.chatState().appendMessageForRestore(request);

    RemoteIMMessage resolution;
    resolution.id = QStringLiteral("approval-authoritative-resolution");
    resolution.fromUserId = QStringLiteral("multi-ai-code");
    resolution.toUserId = QStringLiteral("desktop-user");
    resolution.text = QStringLiteral("该审批收到新的 IM 消息后已自动拒绝");
    resolution.direction = RemoteIMMessageDirection::Incoming;
    resolution.status = RemoteIMMessageStatus::Received;
    resolution.origin = RemoteIMMessageOrigin::Machine;
    resolution.hasApprovalDecision = true;
    resolution.approvalDecision =
        RemoteIMApprovalDecision{QStringLiteral("approval-authoritative-1"), RemoteIMApprovalAction::AutoDeclined};
    app.chatState().appendMessageForRestore(resolution);

    RemoteIMMessage lateDecision;
    lateDecision.id = QStringLiteral("approval-authoritative-late-decision");
    lateDecision.fromUserId = QStringLiteral("desktop-user");
    lateDecision.toUserId = QStringLiteral("multi-ai-code");
    lateDecision.text = QStringLiteral("审批操作：同意本次");
    lateDecision.direction = RemoteIMMessageDirection::Outgoing;
    lateDecision.status = RemoteIMMessageStatus::Sent;
    lateDecision.origin = RemoteIMMessageOrigin::Human;
    lateDecision.hasApprovalDecision = true;
    lateDecision.approvalDecision =
        RemoteIMApprovalDecision{QStringLiteral("approval-authoritative-1"), RemoteIMApprovalAction::ApproveOnce};
    app.chatState().appendMessageForRestore(lateDecision);

    MainWindow window(app);
    window.show();
    QCoreApplication::processEvents();

    QVERIFY(window.findChild<QPushButton*>(QStringLiteral("approvalApproveOnceButton")) == nullptr);
    auto* resolvedLabel = window.findChild<QLabel*>(QStringLiteral("approvalSentLabel"));
    QVERIFY(resolvedLabel != nullptr);
    QVERIFY(resolvedLabel->text().contains(QStringLiteral("自动拒绝")));
}

void MainWindowLayoutTest::copiesOriginalMarkdownFromMessageContextMenu() {
    auto client = std::make_unique<FakeRemoteIMClient>();
    RemoteIMApplication app(QStringLiteral("desktop-user"), std::move(client));
    app.addContact(QStringLiteral("phone-user"), QStringLiteral("iPhone"));
    const QString markdown = QStringLiteral(
        "# 原始标题\n\n"
        "**加粗** 与 `code`\n\n"
        "- 列表项\n\n"
        "```cpp\nint answer = 42;\n```\n\n"
        "[链接](https://example.com?a=1&b=2)");
    app.chatState().receiveText(QStringLiteral("phone-user"), markdown);

    MainWindow window(app);
    auto* markdownView = window.findChild<MarkdownLabel*>(QStringLiteral("messageMarkdownView"));
    auto* copyOriginalAction = window.findChild<QAction*>(QStringLiteral("copyOriginalDataAction"));
    QVERIFY(markdownView != nullptr);
    QVERIFY(copyOriginalAction != nullptr);
    QCOMPARE(copyOriginalAction->text(), QStringLiteral("复制原始数据"));
    QVERIFY(!markdownView->plainText().contains(QStringLiteral("# 原始标题")));
    QVERIFY(!markdownView->plainText().contains(QStringLiteral("**加粗**")));

    QApplication::clipboard()->setText(QStringLiteral("旧剪贴板内容"));
    copyOriginalAction->trigger();

    QCOMPARE(QApplication::clipboard()->text(), markdown);
}

void MainWindowLayoutTest::addContactButtonRemainsVisibleWithoutSearch() {
    auto client = std::make_unique<FakeRemoteIMClient>();
    RemoteIMApplication app(QStringLiteral("desktop-user"), std::move(client));
    app.addContact(QStringLiteral("phone-user"), QStringLiteral("iPhone"));

    MainWindow window(app);
    auto* navRail = window.findChild<QWidget*>(QStringLiteral("navRail"));
    const QList<QPushButton*> addButtons = window.findChildren<QPushButton*>(QStringLiteral("addConversationButton"));

    QVERIFY(navRail != nullptr);
    // 去掉手动搜索后，添加联系人入口仍应保留在会话栏。
    QCOMPARE(addButtons.size(), 1);
    QVERIFY2(addButtons.first()->parentWidget() != navRail, "加号已从导航栏移出，不该还挂在导航栏上");

    auto* conversations = window.findChild<QListWidget*>(QStringLiteral("conversationList"));
    auto* search = window.findChild<QLineEdit*>(QStringLiteral("globalSearchBox"));
    auto* contactsSearch = window.findChild<QLineEdit*>(QStringLiteral("contactsSearchBox"));
    QVERIFY(conversations != nullptr);
    QVERIFY(search != nullptr);
    QVERIFY(contactsSearch != nullptr);
    QCOMPARE(addButtons.first()->parentWidget(), conversations->parentWidget());
    QVERIFY(search->isHidden());
    QVERIFY(contactsSearch->isHidden());
}

void MainWindowLayoutTest::conversationHeaderNamesActions() {
    ScopedMessageHeaderFont font;
    auto client = std::make_unique<FakeRemoteIMClient>();
    RemoteIMApplication app(QStringLiteral("desktop-user"), std::move(client));
    app.addContact(QStringLiteral("phone-user"), QStringLiteral("iPhone"));
    MainWindow window(app);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));

    auto* pane = window.findChild<QWidget*>(QStringLiteral("conversationPane"));
    auto* title = window.findChild<QLabel*>(QStringLiteral("conversationSectionTitle"));
    auto* add = window.findChild<QPushButton*>(QStringLiteral("addConversationButton"));
    QVERIFY(pane && title && add);
    QVERIFY(window.findChild<QPushButton*>(QStringLiteral("friendRequestsButton")) == nullptr);
    QCOMPARE(title->text(), QStringLiteral("消息"));
    QVERIFY(title->mapTo(pane, QPoint()).x() < add->mapTo(pane, QPoint()).x());
    QCOMPARE(add->accessibleName(), QStringLiteral("添加会话或好友"));
    QVERIFY(add->accessibleDescription().contains(QStringLiteral("好友申请")));
    QVERIFY(add->text().isEmpty());
    QVERIFY(!add->icon().isNull());
    const QByteArray capture = qgetenv("MAICHAT_CAPTURE_CONVERSATION_HEADER");
    if (!capture.isEmpty()) {
        const QRect header(0, 0, pane->width(), add->mapTo(pane, QPoint(0, add->height())).y() + UiZoom::s(12));
        QVERIFY(pane->grab(header).save(QString::fromUtf8(capture)));
    }
}

void MainWindowLayoutTest::conversationAddMenuKeepsActionsAndAnchorsToButton() {
    ScopedMessageHeaderFont font;
    auto client = std::make_unique<FakeRemoteIMClient>();
    RemoteIMApplication app(QStringLiteral("desktop-user"), std::move(client));
    app.addContact(QStringLiteral("phone-user"), QStringLiteral("iPhone"));
    app.selectPeer(QStringLiteral("phone-user"));
    int requestLoads = 0;
    QString decidedId;
    bool accepted = false;
    RemoteIMApplication::AccountFriendOperations operations;
    operations.list = [](auto completion) {
        completion({RemoteIMApplication::AccountFriend{QStringLiteral("m_friend"), QStringLiteral("iPhone"),
                                                       QStringLiteral("phone-user")}},
                   {});
    };
    operations.requests = [&requestLoads](auto completion) {
        ++requestLoads;
        completion(
            {RemoteIMApplication::AccountFriendRequest{QStringLiteral("request-id"), QStringLiteral("m_pending"),
                                                       QStringLiteral("待处理好友"), QStringLiteral("pending-user")}},
            {});
    };
    operations.decide = [&decidedId, &accepted](const QString& id, bool allow, auto completion) {
        decidedId = id;
        accepted = allow;
        completion(true, {});
    };
    app.setAccountFriendOperations(std::move(operations));
    MainWindow window(app);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    auto* add = window.findChild<QPushButton*>(QStringLiteral("addConversationButton"));
    auto* pane = window.findChild<QWidget*>(QStringLiteral("conversationPane"));
    QVERIFY(add && pane);
    QTest::mouseClick(add, Qt::LeftButton);
    auto visibleMenu = [add]() -> QMenu* {
        for (auto* menu : add->findChildren<QMenu*>(QStringLiteral("conversationAddMenu")))
            if (menu->isVisible()) return menu;
        return nullptr;
    };
    QMenu* menu = nullptr;
    QTRY_VERIFY((menu = visibleMenu()) != nullptr);
    QVERIFY(menu->windowFlags().testFlag(Qt::FramelessWindowHint));
    QVERIFY(menu->testAttribute(Qt::WA_TranslucentBackground));
    QCOMPARE(menu->accessibleName(), add->accessibleName());
    auto* create = menu->findChild<QAction*>(QStringLiteral("createGroupAction"));
    auto* friendAction = menu->findChild<QAction*>(QStringLiteral("addFriendAction"));
    auto* requests = menu->findChild<QAction*>(QStringLiteral("friendRequestsAction"));
    QVERIFY(create && friendAction && requests);
    QVERIFY(!create->icon().isNull() && !friendAction->icon().isNull() && !requests->icon().isNull());
    const QPoint buttonBottomRight = add->mapToGlobal(QPoint(add->width() - 1, add->height()));
    const QRect available = add->screen()->availableGeometry();
    const int expectedRight = qMin(buttonBottomRight.x(), available.right());
    QVERIFY(qAbs(menu->geometry().right() - expectedRight) <= 2);
    QVERIFY(menu->geometry().top() >= buttonBottomRight.y());
    QVERIFY(menu->geometry().top() < buttonBottomRight.y() + add->height());
    QVERIFY(menu->actionGeometry(requests).height() > menu->fontMetrics().height());
    const QString capture = qEnvironmentVariable("MAICHAT_CAPTURE_MESSAGE_ADD_MENU");
    if (!capture.isEmpty()) {
        // 两张截图均抓取本测试自己的 Qt 子树，不使用桌面截屏混入其他运行中的 App。
        QVERIFY(menu->grab().save(capture));
        const QFileInfo file(capture);
        const QRect header(0, 0, pane->width(), add->mapTo(pane, QPoint(0, add->height())).y() + UiZoom::s(12));
        QVERIFY(pane->grab(header).save(file.dir().filePath(file.completeBaseName() + QStringLiteral("-header.png"))));
    }
    QTest::keyClick(menu, Qt::Key_Escape);
    QTRY_VERIFY(visibleMenu() == nullptr);
    QCOMPARE(app.chatState().selectedPeerId(), QStringLiteral("phone-user"));
    QTRY_VERIFY(add->hasFocus());

    // 键盘也可打开同一菜单；审批从菜单进入后仍调用真实业务路径，不丢掉必要能力。
    QTest::keyClick(add, Qt::Key_Space);
    QTRY_VERIFY((menu = visibleMenu()) != nullptr);
    requests = menu->findChild<QAction*>(QStringLiteral("friendRequestsAction"));
    QVERIFY(requests);
    QTest::mouseClick(menu, Qt::LeftButton, Qt::NoModifier, menu->actionGeometry(requests).center());
    QTRY_COMPARE(requestLoads, 1);
    auto* dialog = window.findChild<QDialog*>(QStringLiteral("friendRequestsDialog"));
    QVERIFY(dialog && dialog->isVisible());
    QPushButton* accept = nullptr;
    for (auto* button : dialog->findChildren<QPushButton*>())
        if (button->text() == QStringLiteral("同意")) accept = button;
    QVERIFY(accept);
    QTest::mouseClick(accept, Qt::LeftButton);
    QCOMPARE(decidedId, QStringLiteral("request-id"));
    QVERIFY(accepted);
    QCOMPARE(app.chatState().selectedPeerId(), QStringLiteral("phone-user"));
    dialog->close();
}

void MainWindowLayoutTest::selfAvatarOpensProfileSettings() {
    auto client = std::make_unique<FakeRemoteIMClient>();
    RemoteIMApplication app(QStringLiteral("desktop-user"), std::move(client));
    app.addContact(QStringLiteral("phone-user"), QStringLiteral("iPhone"));
    MainWindow window(app);
    window.setMaiChatAccountIdProvider([] { return QStringLiteral("m_desktop"); });
    window.setMaiChatNicknameProvider([] { return QStringLiteral("我的昵称"); });
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));

    auto* avatarButton = window.findChild<QPushButton*>(QStringLiteral("navLogoContainer"));
    auto* profileCard = window.findChild<QWidget*>(QStringLiteral("selfProfileCard"));
    auto* profileAvatar = window.findChild<QLabel*>(QStringLiteral("selfProfileAvatar"));
    auto* profileName = window.findChild<QLabel*>(QStringLiteral("selfProfileName"));
    auto* profileId = window.findChild<QLabel*>(QStringLiteral("selfProfileId"));
    auto* stack = window.findChild<QStackedWidget*>(QStringLiteral("contentStack"));
    auto* settingsPage = window.findChild<QWidget*>(QStringLiteral("settingsPage"));
    QVERIFY(avatarButton && profileCard && profileAvatar && profileName && profileId);
    QVERIFY(stack && settingsPage);
    QCOMPARE(app.chatState().selectedPeerId(), QStringLiteral("phone-user"));
    QTest::mouseClick(avatarButton, Qt::LeftButton);
    QCOMPARE(stack->currentWidget(), settingsPage);
    QCOMPARE(profileName->text(), QStringLiteral("我的昵称"));
    QCOMPARE(profileId->text(), QStringLiteral("MaiChat ID  m_desktop"));
    QVERIFY(!profileAvatar->pixmap().isNull());
    QCOMPARE(app.chatState().selectedPeerId(), QStringLiteral("phone-user"));
    const QByteArray capture = qgetenv("MAICHAT_CAPTURE_SELF_PROFILE");
    if (!capture.isEmpty()) QVERIFY(settingsPage->grab().save(QString::fromUtf8(capture)));
}

void MainWindowLayoutTest::navigationTextIsLeftAlignedAndContactsDoNotShowMessagePreview() {
    auto client = std::make_unique<FakeRemoteIMClient>();
    RemoteIMApplication app(QStringLiteral("desktop-user"), std::move(client));
    app.addContact(QStringLiteral("phone-user"), QStringLiteral("iPhone"));
    app.chatState().receiveText(QStringLiteral("phone-user"), QStringLiteral("这条消息不应该显示在通讯录"));

    MainWindow window(app);
    // 原来这里断言样式表含 text-align:left——那是导航按钮还有文字时的要求。
    // 现在是纯图标，改为直接钉住「没有文字」这个事实。
    auto* messagesNavButton = window.findChild<QPushButton*>(QStringLiteral("messagesNavButton"));
    QVERIFY(messagesNavButton != nullptr);
    QVERIFY(messagesNavButton->text().isEmpty());

    auto* contactsNavButton = window.findChild<QPushButton*>(QStringLiteral("contactsNavButton"));
    QVERIFY(contactsNavButton != nullptr);
    contactsNavButton->click();

    auto* contactsList = window.findChild<QListWidget*>(QStringLiteral("contactsList"));
    QVERIFY(contactsList != nullptr);
    QVERIFY(contactsList->count() > 0);
    auto* item = contactsList->item(0);
    QVERIFY(item != nullptr);
    QVERIFY(item->data(Qt::UserRole + 2).toString().isEmpty());
    QVERIFY(item->data(Qt::UserRole + 3).toString().isEmpty());
}

void MainWindowLayoutTest::sectionTitleFollowsSelectedNavigation() {
    auto client = std::make_unique<FakeRemoteIMClient>();
    RemoteIMApplication app(QStringLiteral("desktop-user"), std::move(client));
    app.addContact(QStringLiteral("phone-user"), QStringLiteral("iPhone"));
    app.chatState().receiveText(QStringLiteral("phone-user"), QStringLiteral("通讯录里不应该显示这条消息摘要"));

    MainWindow window(app);
    const auto labels = window.findChildren<QLabel*>();
    for (QLabel* label : labels) {
        QVERIFY(label->text() != QStringLiteral("远程 IM"));
    }

    auto* messagesTitle = window.findChild<QLabel*>(QStringLiteral("messagesSectionTitle"));
    auto* conversationTitle = window.findChild<QLabel*>(QStringLiteral("conversationSectionTitle"));
    auto* contactsTitle = window.findChild<QLabel*>(QStringLiteral("contactsSectionTitle"));
    auto* contactsNavButton = window.findChild<QPushButton*>(QStringLiteral("contactsNavButton"));
    auto* contentStack = window.findChild<QStackedWidget*>(QStringLiteral("contentStack"));
    auto* contactsPage = window.findChild<QWidget*>(QStringLiteral("contactsPage"));
    auto* conversationList = window.findChild<QListWidget*>(QStringLiteral("conversationList"));
    auto* contactsList = window.findChild<QListWidget*>(QStringLiteral("contactsList"));

    // 旧标题和搜索框已移除；会话栏保留紧凑的「消息」标题。
    QVERIFY(messagesTitle == nullptr);
    QVERIFY(conversationTitle != nullptr);
    QCOMPARE(conversationTitle->text(), QStringLiteral("消息"));
    QVERIFY2(contactsTitle == nullptr, "通讯录页的标题应当已被搜索框取代");
    for (QLabel* label : labels) {
        QVERIFY2(label->text() != QStringLiteral("通讯录"), "不该再有「通讯录」这行标题文字");
    }
    auto* search = window.findChild<QLineEdit*>(QStringLiteral("globalSearchBox"));
    QVERIFY2(search != nullptr && search->isHidden(), "会话搜索入口应隐藏");
    auto* contactsSearch = window.findChild<QLineEdit*>(QStringLiteral("contactsSearchBox"));
    QVERIFY2(contactsSearch != nullptr && contactsSearch->isHidden(), "通讯录搜索入口应隐藏");
    QVERIFY(contactsNavButton != nullptr);
    QVERIFY(contentStack != nullptr);
    QVERIFY(contactsPage != nullptr);
    QVERIFY(conversationList != nullptr);
    QVERIFY(contactsList != nullptr);

    contactsNavButton->click();
    QCOMPARE(contentStack->currentWidget(), contactsPage);
    QCOMPARE(contactsList->count(), 1);
    QVERIFY(contactsList->item(0)->data(Qt::UserRole + 2).toString().isEmpty());
    QVERIFY(contactsList->item(0)->data(Qt::UserRole + 3).toString().isEmpty());
    QVERIFY(
        conversationList->item(0)->data(Qt::UserRole + 2).toString().contains(QStringLiteral("通讯录里不应该显示")));
}

// 通讯录页的搜索框必须搜联系人（而不是搜消息），且要和消息搜索一样容错：
// 严格 contains 的话，备注名记岔一个字就什么都搜不出来。
void MainWindowLayoutTest::visibleContactsNavigationSwitchesMiddlePane() {
    auto client = std::make_unique<FakeRemoteIMClient>();
    RemoteIMApplication app(QStringLiteral("desktop-user"), std::move(client));
    app.addContact(QStringLiteral("phone-user"), QStringLiteral("iPhone"));
    app.chatState().receiveText(QStringLiteral("phone-user"), QStringLiteral("消息页摘要"));

    MainWindow window(app);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));

    auto* contactsNavButton = window.findChild<QPushButton*>(QStringLiteral("contactsNavButton"));
    auto* contentStack = window.findChild<QStackedWidget*>(QStringLiteral("contentStack"));
    auto* contactsPage = window.findChild<QWidget*>(QStringLiteral("contactsPage"));
    auto* messagesPage = window.findChild<QWidget*>(QStringLiteral("messagesPage"));
    auto* contactsList = window.findChild<QListWidget*>(QStringLiteral("contactsList"));

    QVERIFY(contactsNavButton != nullptr);
    QVERIFY(contentStack != nullptr);
    QVERIFY(contactsPage != nullptr);
    QVERIFY(messagesPage != nullptr);
    QVERIFY(contactsList != nullptr);

    QTest::mouseClick(contactsNavButton, Qt::LeftButton);

    QTRY_COMPARE(contentStack->currentWidget(), contactsPage);
    QVERIFY(!messagesPage->isVisible());
    QVERIFY(contactsPage->isVisible());
    QCOMPARE(contactsList->count(), 1);
    QVERIFY(contactsList->item(0)->data(Qt::UserRole + 2).toString().isEmpty());
}

void MainWindowLayoutTest::navigationSelectionFollowsContentStackCurrentPage() {
    auto client = std::make_unique<FakeRemoteIMClient>();
    RemoteIMApplication app(QStringLiteral("desktop-user"), std::move(client));
    app.addContact(QStringLiteral("phone-user"), QStringLiteral("iPhone"));

    MainWindow window(app);

    auto* messageNavButton = window.findChild<QPushButton*>(QStringLiteral("messagesNavButton"));
    auto* contactsNavButton = window.findChild<QPushButton*>(QStringLiteral("contactsNavButton"));
    auto* contentStack = window.findChild<QStackedWidget*>(QStringLiteral("contentStack"));
    auto* messagesPage = window.findChild<QWidget*>(QStringLiteral("messagesPage"));
    auto* contactsPage = window.findChild<QWidget*>(QStringLiteral("contactsPage"));

    QVERIFY(messageNavButton != nullptr);
    QVERIFY(contactsNavButton != nullptr);
    QVERIFY(contentStack != nullptr);
    QVERIFY(messagesPage != nullptr);
    QVERIFY(contactsPage != nullptr);

    contactsNavButton->click();
    QCOMPARE(contentStack->currentWidget(), contactsPage);
    QCOMPARE(contactsNavButton->property("selected").toBool(), true);

    contentStack->setCurrentWidget(messagesPage);

    QCOMPARE(messageNavButton->property("selected").toBool(), true);
    QCOMPARE(contactsNavButton->property("selected").toBool(), false);
}

void MainWindowLayoutTest::contactsDirectoryUsesSingleLineRows() {
    auto client = std::make_unique<FakeRemoteIMClient>();
    RemoteIMApplication app(QStringLiteral("desktop-user"), std::move(client));
    app.addContact(QStringLiteral("house-obs-studio"), QStringLiteral("house-obs-studio"));

    MainWindow window(app);
    auto* contactsNavButton = window.findChild<QPushButton*>(QStringLiteral("contactsNavButton"));
    auto* contactsList = window.findChild<QListWidget*>(QStringLiteral("contactsList"));

    QVERIFY(contactsNavButton != nullptr);
    QVERIFY(contactsList != nullptr);

    contactsNavButton->click();

    QCOMPARE(contactsList->count(), 1);
    QVERIFY(contactsList->item(0)->sizeHint().height() <= 56);
    QVERIFY(contactsList->sizeHintForRow(0) <= 56);
}

void MainWindowLayoutTest::receivedMessagesKeepTheSameLeftEdge_data() {
    QTest::addColumn<QString>("text");
    QTest::newRow("short") << QStringLiteral("好");
    QTest::newRow("below-old-threshold") << QString(49, QChar(0x6587));
    QTest::newRow("at-old-threshold") << QString(50, QChar(0x6587));
    QTest::newRow("above-old-threshold") << QString(51, QChar(0x6587));
    QTest::newRow("multiline") << QStringLiteral("第一行\n第二行");
}

void MainWindowLayoutTest::receivedMessagesKeepTheSameLeftEdge() {
    QFETCH(QString, text);
    auto client = std::make_unique<FakeRemoteIMClient>();
    RemoteIMApplication app(QStringLiteral("desktop-user"), std::move(client));
    app.addContact(QStringLiteral("phone-user"), QStringLiteral("iPhone"));
    app.chatState().receiveText(QStringLiteral("phone-user"), text);
    MainWindow window(app);
    window.resize(1200, 850);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    QVERIFY(window.findChild<QLabel*>(QStringLiteral("messageAvatarIncoming")) == nullptr);
    auto* view = window.findChild<MarkdownLabel*>(QStringLiteral("messageMarkdownView"));
    auto* row = window.findChild<QWidget*>(QStringLiteral("messageRowIncoming"));
    QVERIFY(row);
    QVERIFY(view);
    QTRY_COMPARE(view->mapTo(row, QPoint()).x(), 0);
    QTRY_VERIFY(view->mapTo(row, QPoint()).y() <= UiZoom::s(8));
    QTRY_VERIFY(view->width() >= row->contentsRect().width() - UiZoom::s(14) - 2);
}

void MainWindowLayoutTest::wideChatUsesWiderMessageBubbles() {
    auto client = std::make_unique<FakeRemoteIMClient>();
    auto* fakeClient = client.get();
    RemoteIMApplication app(QStringLiteral("desktop-user"), std::move(client));
    app.addContact(QStringLiteral("phone-user"), QStringLiteral("iPhone"));
    RemoteIMMessage outgoingMessage;
    outgoingMessage.id = QStringLiteral("outgoing-avatar-anchor");
    outgoingMessage.fromUserId = QStringLiteral("desktop-user");
    outgoingMessage.toUserId = QStringLiteral("phone-user");
    outgoingMessage.direction = RemoteIMMessageDirection::Outgoing;
    outgoingMessage.status = RemoteIMMessageStatus::Sent;
    outgoingMessage.text = QStringLiteral("用于定位本方头像列");
    app.chatState().appendMessageForRestore(outgoingMessage);

    MainWindow window(app);
    window.resize(1680, 900);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));

    fakeClient->emitIncomingText(QStringLiteral("phone-user"),
                                 QStringLiteral("这是一段比较长的 AICLI 输出内容，用来验证桌面宽屏下消息气泡不会过窄，"
                                                "否则右侧会出现大片没有意义的空白，文本也会被迫换成太多行。"));

    auto* incomingBubble = window.findChild<QWidget*>(QStringLiteral("messageBubbleIncoming"));
    QVERIFY(incomingBubble != nullptr);
    QVERIFY(window.findChild<QLabel*>(QStringLiteral("messageAvatarIncoming")) == nullptr);
    QVERIFY(window.findChild<QLabel*>(QStringLiteral("messageAvatarOutgoing")) == nullptr);
    QVERIFY(window.findChild<QLabel*>(QStringLiteral("messageAuthorLabel")) == nullptr);
    QVERIFY(window.findChild<QLabel*>(QStringLiteral("messageRelationBadge")) == nullptr);
    QTRY_VERIFY(incomingBubble->maximumWidth() >= 820);
    QTRY_VERIFY(incomingBubble->minimumWidth() >= 820);
    auto* incomingRow = incomingBubble->parentWidget();
    QTRY_COMPARE(incomingBubble->mapTo(incomingRow, QPoint()).x(), 0);
    QTRY_VERIFY(incomingBubble->width() >= incomingRow->contentsRect().width() - 2);
    auto* outgoingRow = window.findChild<QWidget*>(QStringLiteral("messageRowOutgoing"));
    QVERIFY(outgoingRow);
    auto* outgoingView = outgoingRow->findChild<MarkdownLabel*>(QStringLiteral("messageMarkdownView"));
    QVERIFY(outgoingView);
    QVERIFY(outgoingView->plainText().endsWith(outgoingView->property("messageTimestamp").toString()));
}

void MainWindowLayoutTest::restoredLongMessagesExpandAfterWindowIsShown() {
    auto client = std::make_unique<FakeRemoteIMClient>();
    RemoteIMApplication app(QStringLiteral("desktop-user"), std::move(client));
    app.addContact(QStringLiteral("phone-user"), QStringLiteral("iPhone"));
    app.chatState().receiveText(
        QStringLiteral("phone-user"),
        QStringLiteral("这是一段在窗口显示前就已经存在的历史长消息，用来模拟登录后拉取到的 AICLI 输出。"
                       "窗口完成布局后，这类历史消息也应该使用宽屏消息区的可用宽度，不能继续保持窄气泡。"));

    MainWindow window(app);
    window.resize(1680, 900);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));

    auto* incomingBubble = window.findChild<QWidget*>(QStringLiteral("messageBubbleIncoming"));
    QVERIFY(incomingBubble != nullptr);
    auto* messageScroll = window.findChild<QScrollArea*>(QStringLiteral("messageScroll"));
    QVERIFY(messageScroll != nullptr);
    QTRY_VERIFY2(incomingBubble->minimumWidth() >= 820,
                 qPrintable(QStringLiteral("min=%1 max=%2 viewport=%3 expanded=%4")
                                .arg(incomingBubble->minimumWidth())
                                .arg(incomingBubble->maximumWidth())
                                .arg(messageScroll->viewport()->width())
                                .arg(incomingBubble->property("expandedTextBubble").toBool())));
}

void MainWindowLayoutTest::slashCommandMenuSendsWithoutChangingDraft() {
    auto client = std::make_unique<FakeRemoteIMClient>();
    auto* fake = client.get();
    RemoteIMApplication app(QStringLiteral("desktop-user"), std::move(client));
    app.addContact(QStringLiteral("phone-user"), QStringLiteral("iPhone"));
    MainWindow window(app);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    auto* editor = window.findChild<QTextEdit*>(QStringLiteral("messageEditor"));
    auto* button = window.findChild<QPushButton*>(QStringLiteral("composerCommandButton"));
    auto* menu = window.findChild<QMenu*>(QStringLiteral("composerCommandMenu"));
    QVERIFY(editor && button && menu);
    QCOMPARE(menu->actions().size(), 12);
    QSignalSpy shown(menu, &QMenu::aboutToShow);
    editor->setPlainText(QStringLiteral("Keep my draft"));
    QEvent enterButton(QEvent::Enter);
    QApplication::sendEvent(button, &enterButton);
    QVERIFY(menu->isVisible());
    QCOMPARE(shown.count(), 1);
    QEvent repeatedEnter(QEvent::Enter);
    QApplication::sendEvent(button, &repeatedEnter);
    QCOMPARE(shown.count(), 1);
    QEvent leaveButton(QEvent::Leave);
    QApplication::sendEvent(button, &leaveButton);
    QVERIFY(menu->isVisible());
    auto* action = menu->findChild<QAction*>(QStringLiteral("slashCommandButton_status"));
    QVERIFY(action);
    action->trigger();
    QCOMPARE(fake->lastText(), QStringLiteral("/status"));
    QCOMPARE(editor->toPlainText(), QStringLiteral("Keep my draft"));
    menu->hide();
    button->click();
    QVERIFY(!menu->isVisible());
    button->setFocus();
    QTest::keyClick(button, Qt::Key_Space);
    QVERIFY(menu->isVisible());
    menu->hide();
    app.addContact(QStringLiteral("other-user"), QStringLiteral("Other"));
    app.selectPeer(QStringLiteral("phone-user"));
    QApplication::sendEvent(button, &enterButton);
    QVERIFY(menu->isVisible());
    app.selectPeer(QStringLiteral("other-user"));
    action->trigger();
    QCOMPARE(fake->lastTextPeerId(), QStringLiteral("phone-user"));
}

void MainWindowLayoutTest::typingSlashDoesNotOpenCommandMenu() {
    auto client = std::make_unique<FakeRemoteIMClient>();
    RemoteIMApplication app(QStringLiteral("desktop-user"), std::move(client));
    app.addContact(QStringLiteral("phone-user"), QStringLiteral("iPhone"));
    MainWindow window(app);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    auto* editor = window.findChild<QTextEdit*>(QStringLiteral("messageEditor"));
    auto* menu = window.findChild<QMenu*>(QStringLiteral("composerCommandMenu"));
    QVERIFY(editor && menu);
    editor->setFocus();
    editor->setPlainText(QStringLiteral("/goal "));
    QTextCursor cursor = editor->textCursor();
    cursor.movePosition(QTextCursor::End);
    editor->setTextCursor(cursor);
    QInputMethodEvent composing(QStringLiteral("n"), {});
    QApplication::sendEvent(editor, &composing);
    QInputMethodEvent committed(QString(), {});
    committed.setCommitString(QStringLiteral("你好"));
    QApplication::sendEvent(editor, &committed);
    QCOMPARE(editor->toPlainText(), QStringLiteral("/goal 你好"));
    QVERIFY(!menu->isVisible());
}

void MainWindowLayoutTest::deleteKeyClearsMessagesButKeepsContactInConversationList() {
    auto client = std::make_unique<FakeRemoteIMClient>();
    RemoteIMApplication app(QStringLiteral("desktop-user"), std::move(client));
    app.addContact(QStringLiteral("phone-user"), QStringLiteral("iPhone"));
    app.chatState().receiveText(QStringLiteral("phone-user"), QStringLiteral("remove me"));
    app.addContact(QStringLiteral("other-user"), QStringLiteral("Other"));

    MainWindow window(app);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    auto* conversationList = window.findChild<QListWidget*>(QStringLiteral("conversationList"));
    QVERIFY(conversationList != nullptr);

    conversationList->setCurrentRow(0);
    conversationList->setFocus();
    QCOMPARE(conversationList->currentItem()->data(Qt::UserRole).toString(), QStringLiteral("phone-user"));
    confirmNextContactDeletion();
    QTest::keyClick(conversationList, Qt::Key_Delete);

    // 会话列表只清空聊天记录：好友保留（删除好友是通讯录的功能）。
    QCOMPARE(app.chatState().messagesWith(QStringLiteral("phone-user")).size(), 0);
    QCOMPARE(app.chatState().contacts().size(), 2);
    QCOMPARE(app.chatState().contacts().first().userId, QStringLiteral("phone-user"));
}

void MainWindowLayoutTest::deleteKeyRemovesContactAndMessagesFromContactsList() {
    auto client = std::make_unique<FakeRemoteIMClient>();
    RemoteIMApplication app(QStringLiteral("desktop-user"), std::move(client));
    app.addContact(QStringLiteral("phone-user"), QStringLiteral("iPhone"));
    app.chatState().receiveText(QStringLiteral("phone-user"), QStringLiteral("remove me"));
    app.addContact(QStringLiteral("other-user"), QStringLiteral("Other"));

    MainWindow window(app);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));

    auto* contactsNavButton = window.findChild<QPushButton*>(QStringLiteral("contactsNavButton"));
    auto* contactsList = window.findChild<QListWidget*>(QStringLiteral("contactsList"));
    QVERIFY(contactsNavButton != nullptr);
    QVERIFY(contactsList != nullptr);

    contactsNavButton->click();
    contactsList->setCurrentRow(0);
    contactsList->setFocus();
    QCOMPARE(contactsList->currentItem()->data(Qt::UserRole).toString(), QStringLiteral("phone-user"));
    confirmNextContactDeletion();
    QTest::keyClick(contactsList, Qt::Key_Delete);

    QCOMPARE(app.chatState().messagesWith(QStringLiteral("phone-user")).size(), 0);
    QCOMPARE(app.chatState().contacts().size(), 1);
    QCOMPARE(app.chatState().contacts().first().userId, QStringLiteral("other-user"));
    QCOMPARE(app.chatState().selectedPeerId(), QStringLiteral("other-user"));
}

void MainWindowLayoutTest::navigationIconsDoNotUsePrivateFontGlyphProperties() {
    auto client = std::make_unique<FakeRemoteIMClient>();
    RemoteIMApplication app(QStringLiteral("desktop-user"), std::move(client));
    app.addContact(QStringLiteral("phone-user"), QStringLiteral("iPhone"));

    MainWindow window(app);

    for (const QString& objectName : {QStringLiteral("messagesNavButton"), QStringLiteral("contactsNavButton"),
                                      QStringLiteral("settingsNavButton")}) {
        auto* button = window.findChild<QPushButton*>(objectName);
        QVERIFY(button != nullptr);
        QVERIFY(!button->property("navGlyph").isValid());
        QVERIFY(!button->icon().isNull());
    }
}

void MainWindowLayoutTest::conversationListShowsUnreadBadgeAndClearsOnOpen() {
    auto client = std::make_unique<FakeRemoteIMClient>();
    RemoteIMApplication app(QStringLiteral("desktop-user"), std::move(client));
    app.addContact(QStringLiteral("phone-user"), QStringLiteral("iPhone"));
    // 非选中会话收到两条实时消息 → 红点数 2；选中会话（phone-user）不计。
    app.chatState().receiveText(QStringLiteral("phone-user"), QStringLiteral("已读消息"));
    app.chatState().receiveText(QStringLiteral("mac-user"), QStringLiteral("未读一"));
    app.chatState().receiveText(QStringLiteral("mac-user"), QStringLiteral("未读二"));

    MainWindow window(app);
    auto* conversationList = window.findChild<QListWidget*>(QStringLiteral("conversationList"));
    QVERIFY(conversationList != nullptr);
    QCOMPARE(conversationList->count(), 2);

    int phoneRow = -1;
    int macRow = -1;
    for (int row = 0; row < conversationList->count(); ++row) {
        const QString userId = conversationList->item(row)->data(Qt::UserRole).toString();
        if (userId == QStringLiteral("phone-user")) phoneRow = row;
        if (userId == QStringLiteral("mac-user")) macRow = row;
    }
    QVERIFY(phoneRow >= 0);
    QVERIFY(macRow >= 0);
    QCOMPARE(conversationList->item(phoneRow)->data(Qt::UserRole + 4).toInt(), 0);
    QCOMPARE(conversationList->item(macRow)->data(Qt::UserRole + 4).toInt(), 2);

    // 点开该会话（触发 selectPeer + stateChanged 重刷）：红点清零。
    conversationList->setCurrentRow(macRow);
    for (int row = 0; row < conversationList->count(); ++row) {
        const QString userId = conversationList->item(row)->data(Qt::UserRole).toString();
        if (userId == QStringLiteral("mac-user")) macRow = row;
    }
    QCOMPARE(conversationList->item(macRow)->data(Qt::UserRole + 4).toInt(), 0);
    QCOMPARE(app.chatState().unreadCount(QStringLiteral("mac-user")), 0);
}

void MainWindowLayoutTest::everyNavButtonIsStyledConsistently() {
    auto client = std::make_unique<FakeRemoteIMClient>();
    RemoteIMApplication app(QStringLiteral("desktop-user"), std::move(client));
    MainWindow window(app);

    // 导航按钮样式是按 objectName 逐个列举的，新增一项时极易漏进选择器——
    // 漏了就掉回 QPushButton 默认居中布局，与其余项对不齐（远程页上线时就踩过）。
    // 这里断言每个导航按钮都出现在样式表里，且共用同一条左对齐规则。
    const QString styleSheet = window.styleSheet();
    const QStringList navObjectNames{QStringLiteral("messagesNavButton"), QStringLiteral("cloudNavButton"),
                                     QStringLiteral("agentNavButton"), QStringLiteral("contactsNavButton"),
                                     QStringLiteral("settingsNavButton")};

    for (const QString& name : navObjectNames) {
        auto* button = window.findChild<QPushButton*>(name);
        QVERIFY2(button != nullptr, qPrintable(name));
        QVERIFY2(styleSheet.contains(QStringLiteral("#%1 ").arg(name)) ||
                     styleSheet.contains(QStringLiteral("#%1,").arg(name)),
                 qPrintable(QStringLiteral("%1 缺少基础样式规则").arg(name)));
        QVERIFY2(styleSheet.contains(QStringLiteral("#%1[selected=\"true\"]").arg(name)),
                 qPrintable(QStringLiteral("%1 缺少选中态样式规则").arg(name)));
    }

    // 所有导航按钮左边缘应当一致（同在导航栏布局里，宽度相同）。
    const int width = window.findChild<QPushButton*>(navObjectNames.first())->width();
    for (const QString& name : navObjectNames) {
        QCOMPARE(window.findChild<QPushButton*>(name)->width(), width);
    }
}

void MainWindowLayoutTest::accountAvatarFollowsIMProfile() {
    auto client = std::make_unique<FakeRemoteIMClient>();
    auto* fake = client.get();
    RemoteIMApplication app(QStringLiteral("desktop-user"), std::move(client));
    MainWindow window(app);
    auto* logo = window.findChild<QLabel*>(QStringLiteral("navLogo"));
    QVERIFY(logo);
    QVERIFY(!logo->pixmap().isNull());
    QCOMPARE(logo->accessibleName(), QStringLiteral("desktop-user"));
    fake->selfProfileReceived(QStringLiteral("desktop-user"), QStringLiteral("My Name"),
                              QStringLiteral("https://example.com/new-avatar.jpg"));
    QCOMPARE(app.selfProfile().avatarUrl, QStringLiteral("https://example.com/new-avatar.jpg"));
    QCOMPARE(logo->accessibleName(), QStringLiteral("My Name"));
    fake->selfProfileReceived(QStringLiteral("other-user"), QStringLiteral("Other"),
                              QStringLiteral("https://example.com/other.jpg"));
    QCOMPARE(app.selfProfile().displayName, QStringLiteral("My Name"));
    QCOMPARE(logo->accessibleName(), QStringLiteral("My Name"));
    QCOMPARE(app.chatState().contacts().size(), 0);
}

void MainWindowLayoutTest::connectionStatusLivesInSelfProfile() {
    auto client = std::make_unique<FakeRemoteIMClient>();
    RemoteIMApplication app(QStringLiteral("desktop-user"), std::move(client));
    MainWindow window(app);
    window.resize(1280, 820);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));

    auto* statusDot = window.findChild<QLabel*>(QStringLiteral("connectionStatusDot"));
    auto* statusText = window.findChild<QLabel*>(QStringLiteral("selfConnectionStatus"));
    auto* logoContainer = window.findChild<QWidget*>(QStringLiteral("navLogoContainer"));
    auto* profileCard = window.findChild<QWidget*>(QStringLiteral("selfProfileCard"));
    auto* chatHeader = window.findChild<QWidget*>(QStringLiteral("chatHeader"));
    QVERIFY(statusDot != nullptr);
    QVERIFY(statusText != nullptr);
    QVERIFY(logoContainer != nullptr);
    QVERIFY(profileCard != nullptr);
    QVERIFY(chatHeader != nullptr);
    QVERIFY(profileCard->isAncestorOf(statusDot));
    QVERIFY(profileCard->isAncestorOf(statusText));
    QVERIFY(!logoContainer->isAncestorOf(statusDot));
    QVERIFY(!chatHeader->isAncestorOf(statusDot));
    QCOMPARE(statusDot->text(), QString());
    QCOMPARE(statusDot->property("connected").toBool(), false);
    QCOMPARE(statusText->text(), QStringLiteral("消息未连接"));

    // 形状必须真是个圆。QSS 的 border-radius 在非整数缩放倍率下会塌成圆角方块
    // （尺寸与圆角各自四舍五入，半径小于半边长），所以这里直接看画出来的像素：
    // 四角透明、中心不透明。断言「有个 QLabel 在那儿」是看不出方块的。
    const QPixmap painted = statusDot->pixmap();
    QVERIFY2(!painted.isNull(), "状态点没有画出来");
    const QImage image = painted.toImage();
    QCOMPARE(qAlpha(image.pixel(0, 0)), 0);
    QCOMPARE(qAlpha(image.pixel(image.width() - 1, 0)), 0);
    QCOMPARE(qAlpha(image.pixel(0, image.height() - 1)), 0);
    QVERIFY2(qAlpha(image.pixel(image.width() / 2, image.height() / 2)) > 200, "状态点中心是透明的，等于没画");

    app.connectToService(1, QStringLiteral("test-user-sig"));
    QCOMPARE(statusDot->property("connected").toBool(), true);
    QCOMPARE(statusText->text(), QStringLiteral("消息已连接"));
    QCOMPARE(statusDot->text(), QString());
    QVERIFY(statusDot->toolTip().contains(QStringLiteral("已连接")));
    app.disconnectFromService();
    QTRY_COMPARE(statusText->text(), QStringLiteral("消息未连接"));
}

void MainWindowLayoutTest::fileBubbleOffersContextMenu() {
    QTemporaryDir dir;
    const QString sourcePath = dir.filePath(QStringLiteral("report.md"));
    {
        QFile source(sourcePath);
        QVERIFY(source.open(QIODevice::WriteOnly));
        source.write("# 报告\n正文");
    }

    auto client = std::make_unique<FakeRemoteIMClient>();
    RemoteIMApplication app(QStringLiteral("desktop-user"), std::move(client));
    app.addContact(QStringLiteral("phone-user"), QStringLiteral("iPhone"));
    app.chatState().receiveFile(QStringLiteral("phone-user"), sourcePath, QStringLiteral("report.md"),
                                QStringLiteral("text/markdown"), 10);

    MainWindow window(app);
    auto* fileButton = window.findChild<QPushButton*>(QStringLiteral("messageFileButton"));
    QVERIFY(fileButton != nullptr);
    // 自定义右键菜单（预览 / 保存到本地）挂在文件气泡按钮上。
    QCOMPARE(fileButton->contextMenuPolicy(), Qt::CustomContextMenu);
}

void MainWindowLayoutTest::imageBubbleOffersContextMenu() {
    QTemporaryDir dir;
    const QString imagePath = dir.filePath(QStringLiteral("shot.png"));
    QImage image(8, 8, QImage::Format_RGB32);
    image.fill(Qt::red);
    QVERIFY(image.save(imagePath));

    auto client = std::make_unique<FakeRemoteIMClient>();
    RemoteIMApplication app(QStringLiteral("desktop-user"), std::move(client));
    app.addContact(QStringLiteral("phone-user"), QStringLiteral("iPhone"));
    app.chatState().receiveImage(QStringLiteral("phone-user"), imagePath, 8, 8, 100);

    MainWindow window(app);
    auto* imageLabel = window.findChild<QLabel*>(QStringLiteral("messageImageLabel"));
    QVERIFY(imageLabel != nullptr);
    // 自定义右键菜单（复制 / 预览 / 保存到本地）挂在图片缩略图上。
    QCOMPARE(imageLabel->contextMenuPolicy(), Qt::CustomContextMenu);
}

void MainWindowLayoutTest::imageBubbleUsesNativePixmapPreview() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("chat-photo.png"));
    QImage original(3024, 1898, QImage::Format_RGBA8888);
    original.fill(Qt::red);
    QVERIFY(original.save(path, "PNG"));

    RemoteIMApplication app(QStringLiteral("desktop-user"), std::make_unique<FakeRemoteIMClient>());
    app.addContact(QStringLiteral("phone-user"), QStringLiteral("iPhone"));
    app.chatState().receiveImage(QStringLiteral("phone-user"), path, 3024, 1898, QFileInfo(path).size());
    MainWindow window(app);
    window.resize(1280, 800);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    auto* label = window.findChild<QLabel*>(QStringLiteral("messageImageLabel"));
    QVERIFY(label != nullptr);
    QTRY_VERIFY(!label->pixmap().isNull());
    QCOMPARE(label->pixmap().toImage().pixelColor(0, 0), QColor(Qt::red));
    QVERIFY(label->findChild<QWidget*>(QStringLiteral("maiGraphicsImageOverlay")) == nullptr);
}

void MainWindowLayoutTest::maximizedImageBubbleOpensOnlyOnePreview() {
    QTemporaryDir dir;
    const QString imagePath = dir.filePath(QStringLiteral("shot.png"));
    const QSize originalSize(320, 180);
    QImage image(originalSize, QImage::Format_RGB32);
    image.fill(Qt::red);
    QVERIFY(image.save(imagePath));

    auto client = std::make_unique<FakeRemoteIMClient>();
    RemoteIMApplication app(QStringLiteral("desktop-user"), std::move(client));
    app.addContact(QStringLiteral("phone-user"), QStringLiteral("iPhone"));
    app.chatState().receiveImage(QStringLiteral("phone-user"), imagePath, originalSize.width(), originalSize.height(),
                                 100);

    MainWindow window(app);
    window.showMaximized();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    auto* imageLabel = window.findChild<QLabel*>(QStringLiteral("messageImageLabel"));
    QVERIFY(imageLabel != nullptr);

    // 连续到达的点击事件也只能复用当前预览，不能叠出多个预览窗口。
    QCOMPARE(window.findChildren<ImagePreviewDialog*>().size(), 0);
    QTest::mouseClick(imageLabel, Qt::LeftButton);
    QCOMPARE(window.findChildren<ImagePreviewDialog*>().size(), 1);
    QTest::mouseClick(imageLabel, Qt::LeftButton);
    QVERIFY(window.findChild<QLabel*>(QStringLiteral("imagePreviewTransitionSnapshot")) != nullptr);
    const QByteArray capture = qgetenv("MAICHAT_TEST_CAPTURE_PATH");
    if (!capture.isEmpty()) {
        QTest::qWait(100);
        QVERIFY(window.screen()
                    ->grabWindow(0, window.x(), window.y(), window.width(), window.height())
                    .save(QString::fromUtf8(capture)));
    }
    QTRY_COMPARE(window.findChildren<ImagePreviewDialog*>().size(), 1);
    auto* preview = window.findChild<ImagePreviewDialog*>(QStringLiteral("imagePreviewDialog"));
    QVERIFY(preview != nullptr);
    QTRY_VERIFY(preview->isVisible());
    QVERIFY(!(preview->windowState() & Qt::WindowFullScreen));
    QVERIFY(preview->windowFlags().testFlag(Qt::FramelessWindowHint));
    QCOMPARE(preview->size(), originalSize);

    const QSize resizedPreview(420, 260);
    preview->resize(resizedPreview);
    QTRY_COMPARE(preview->size(), resizedPreview);

    QTest::mouseClick(preview, Qt::LeftButton);
    QVERIFY(window.findChild<QLabel*>(QStringLiteral("imagePreviewTransitionSnapshot")) != nullptr);
    if (!capture.isEmpty()) {
        QTest::qWait(100);
        QVERIFY(window.screen()
                    ->grabWindow(0, window.x(), window.y(), window.width(), window.height())
                    .save(QString::fromUtf8(capture) + QStringLiteral(".close.png")));
    }
    QTRY_VERIFY(window.findChildren<ImagePreviewDialog*>().isEmpty());
    QTRY_VERIFY(imageLabel->graphicsEffect() == nullptr);
    QVERIFY(imageLabel->isVisible());
    QTest::mouseClick(imageLabel, Qt::LeftButton);
    auto* reopened = window.findChild<ImagePreviewDialog*>(QStringLiteral("imagePreviewDialog"));
    QVERIFY(reopened);
    QTRY_VERIFY(reopened->isVisible());
    reopened->close();
    QTRY_VERIFY(window.findChildren<ImagePreviewDialog*>().isEmpty());
    QTRY_VERIFY(imageLabel->graphicsEffect() == nullptr);
}

void MainWindowLayoutTest::imagePreviewKeepsConversationBackgroundStable_data() {
    QTest::addColumn<bool>("maximized");
    QTest::newRow("normal") << false;
    QTest::newRow("maximized") << true;
}

void MainWindowLayoutTest::imagePreviewKeepsConversationBackgroundStable() {
    QFETCH(bool, maximized);
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("chat-photo.png"));
    const QSize originalSize(320, 180);
    QImage image(originalSize, QImage::Format_RGB32);
    image.fill(Qt::red);
    QVERIFY(image.save(path));

    RemoteIMApplication app(QStringLiteral("desktop-user"), std::make_unique<FakeRemoteIMClient>());
    app.addContact(QStringLiteral("phone-user"), QStringLiteral("iPhone"));
    app.chatState().receiveImage(QStringLiteral("phone-user"), path, originalSize.width(), originalSize.height(), 100);
    MainWindow window(app);
    window.resize(1280, 800);
    if (maximized)
        window.showMaximized();
    else
        window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    window.activateWindow();
    auto* label = window.findChild<QLabel*>(QStringLiteral("messageImageLabel"));
    auto* scroll = window.findChild<QScrollArea*>(QStringLiteral("messageScroll"));
    QVERIFY(label && scroll);
    QTRY_VERIFY(!label->pixmap().isNull());
    QTest::qWait(100);

    // Observe rendered conversation pixels, away from both image endpoints and
    // native title/navigation chrome. Ordinary window activation may change the
    // chrome, but opening an image must not darken and brighten the conversation.
    const QRect viewport(scroll->viewport()->mapTo(&window, QPoint()), scroll->viewport()->size());
    const QRect source(label->mapTo(&window, QPoint()), label->size());
    const QRect target(window.rect().center() - QPoint(originalSize.width() / 2, originalSize.height() / 2),
                       originalSize);
    const QRect imageMotionBounds = source.united(target).adjusted(-48, -48, 48, 48);
    const QPixmap baseline = window.grab();
    const QImage baselinePixels = baseline.toImage();
    const qreal dpr = baseline.devicePixelRatioF();
    QVector<QPoint> samples;
    QVector<QColor> colors;
    for (int yPercent : {20, 50, 80}) {
        for (int xPercent : {20, 50, 80}) {
            const QPoint point(viewport.left() + viewport.width() * xPercent / 100,
                               viewport.top() + viewport.height() * yPercent / 100);
            const QRect patch(point - QPoint(6, 6), QSize(12, 12));
            if (!viewport.contains(patch) || imageMotionBounds.intersects(patch)) continue;
            const QColor center = baselinePixels.pixelColor(qRound(point.x() * dpr), qRound(point.y() * dpr));
            if (center.red() < 180 || center.green() < 180 || center.blue() < 180) continue;
            bool uniform = true;
            for (int dy : {-5, 0, 5}) {
                for (int dx : {-5, 0, 5}) {
                    const QColor pixel =
                        baselinePixels.pixelColor(qRound((point.x() + dx) * dpr), qRound((point.y() + dy) * dpr));
                    if (qAbs(pixel.red() - center.red()) > 2 || qAbs(pixel.green() - center.green()) > 2 ||
                        qAbs(pixel.blue() - center.blue()) > 2)
                        uniform = false;
                }
            }
            if (uniform) {
                samples.append(point);
                colors.append(center);
            }
        }
    }
    QVERIFY2(samples.size() >= 2, "Need two unobscured, plain conversation background patches");

    const QString capturePrefix = QString::fromUtf8(qgetenv("MAICHAT_TEST_CAPTURE_PATH"));
    QStringList failures;
    QVector<QPair<QString, QPixmap>> captures;
    const auto captureScreen = [&](const QString& phase) {
        if (capturePrefix.isEmpty()) return;
        const QPoint origin = window.mapToGlobal(QPoint());
        const QString file = capturePrefix + QLatin1Char('.') + QString::fromLatin1(QTest::currentDataTag()) +
                             QLatin1Char('.') + phase + QStringLiteral(".png");
        captures.append(
            qMakePair(file, window.screen()->grabWindow(0, origin.x(), origin.y(), window.width(), window.height())));
    };
    captureScreen(QStringLiteral("before"));
    const auto sampleFrame = [&](const QString& phase) {
        // QWidget::grab composites actual child painting, including translucent
        // overlays, without depending on another application's screen position.
        const QPixmap frame = window.grab();
        const QImage pixels = frame.toImage();
        const qreal frameDpr = frame.devicePixelRatioF();
        for (int i = 0; i < samples.size(); ++i) {
            const QPoint point = samples.at(i);
            const QColor actual = pixels.pixelColor(qRound(point.x() * frameDpr), qRound(point.y() * frameDpr));
            const QColor expected = colors.at(i);
            if (qAbs(actual.red() - expected.red()) > 3 || qAbs(actual.green() - expected.green()) > 3 ||
                qAbs(actual.blue() - expected.blue()) > 3) {
                failures.append(QStringLiteral("%1 at (%2,%3): background %4 became %5")
                                    .arg(phase)
                                    .arg(point.x())
                                    .arg(point.y())
                                    .arg(expected.name(), actual.name()));
            }
        }
        captureScreen(phase);
    };
    const auto sampleTransition = [&](const QString& phase, QElapsedTimer& clock) {
        for (int time : {0, 50, 100, 150, 250, 400}) {
            const int wait = time - static_cast<int>(clock.elapsed());
            if (wait > 0) QTest::qWait(wait);
            sampleFrame(QStringLiteral("%1-%2ms-actual-%3ms").arg(phase).arg(time).arg(clock.elapsed()));
        }
    };

    QElapsedTimer opening;
    opening.start();
    QTest::mouseClick(label, Qt::LeftButton);
    sampleTransition(QStringLiteral("opening"), opening);
    auto* preview = window.findChild<ImagePreviewDialog*>(QStringLiteral("imagePreviewDialog"));
    QVERIFY(preview);
    QTRY_VERIFY(preview->isVisible());
    if (!preview->windowFlags().testFlag(Qt::FramelessWindowHint))
        failures.append(QStringLiteral("Image preview exposes native window title and buttons"));
    sampleFrame(QStringLiteral("preview-visible"));

    QElapsedTimer closing;
    closing.start();
    QTest::mouseClick(preview, Qt::LeftButton);
    sampleTransition(QStringLiteral("closing"), closing);
    QTRY_VERIFY(window.findChildren<ImagePreviewDialog*>().isEmpty());
    sampleFrame(QStringLiteral("closed"));
    // PNG compression is deferred until after the gestures so saving evidence
    // cannot consume an animation's sampling interval.
    for (const auto& capture : captures) {
        if (!capture.second.save(capture.first))
            failures.append(QStringLiteral("Could not save screen capture: ") + capture.first);
    }
    // Collect both directions before asserting so a failing entrance still
    // yields closing frames and useful evidence for the same reproduction.
    QVERIFY2(failures.isEmpty(), qPrintable(failures.join(QLatin1Char('\n'))));
}

void MainWindowLayoutTest::copyAttachmentToPathCopiesOverwritesAndReportsErrors() {
    QTemporaryDir dir;
    const QString sourcePath = dir.filePath(QStringLiteral("weekly.md"));
    {
        QFile source(sourcePath);
        QVERIFY(source.open(QIODevice::WriteOnly));
        source.write("# 周报内容");
    }
    RemoteIMFileAttachment attachment{sourcePath, QStringLiteral("weekly.md"), QStringLiteral("text/markdown"), 0};

    // 正常保存：内容一致。
    const QString targetPath = dir.filePath(QStringLiteral("saved/weekly.md"));
    QVERIFY(QDir(dir.path()).mkpath(QStringLiteral("saved")));
    QString error;
    QVERIFY(MainWindow::copyAttachmentToPath(attachment, targetPath, &error));
    QFile saved(targetPath);
    QVERIFY(saved.open(QIODevice::ReadOnly));
    QCOMPARE(saved.readAll(), QByteArray("# 周报内容"));
    saved.close();

    // 目标已存在：覆盖（「另存为」对话框已确认过覆盖语义）。
    {
        QFile source(sourcePath);
        QVERIFY(source.open(QIODevice::WriteOnly | QIODevice::Truncate));
        source.write("# 更新后的周报");
    }
    QVERIFY(MainWindow::copyAttachmentToPath(attachment, targetPath, &error));
    QVERIFY(saved.open(QIODevice::ReadOnly));
    QCOMPARE(saved.readAll(), QByteArray("# 更新后的周报"));
    saved.close();

    // 源缓存缺失（未下载完成/被清理）：失败并给出原因。
    RemoteIMFileAttachment missing{dir.filePath(QStringLiteral("gone.md")), QStringLiteral("gone.md"), QString(), 0};
    QVERIFY(!MainWindow::copyAttachmentToPath(missing, dir.filePath(QStringLiteral("out.md")), &error));
    QVERIFY(!error.isEmpty());

    // 空目标路径：失败。
    QVERIFY(!MainWindow::copyAttachmentToPath(attachment, QStringLiteral("  "), &error));
    QVERIFY(!error.isEmpty());
}

void MainWindowLayoutTest::ctrlShortcutsZoomWholeUi() {
    UiZoom::setFactor(1.0);
    auto client = std::make_unique<FakeRemoteIMClient>();
    RemoteIMApplication app(QStringLiteral("desktop-user"), std::move(client));
    app.addContact(QStringLiteral("phone-user"), QStringLiteral("iPhone"));

    MainWindow window(app);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));

    // Ctrl+= 放大一档并弹出百分比浮层。
    QTest::keyClick(&window, Qt::Key_Equal, Qt::ControlModifier);
    QCOMPARE(qRound(UiZoom::factor() * 100), 110);
    auto* toast = window.findChild<QLabel*>(QStringLiteral("toast"));
    QVERIFY(toast != nullptr);
    QVERIFY(toast->isVisible());
    QCOMPARE(toast->text(), QStringLiteral("110%"));
    // 代码级最小宽高随倍率重放（520 × 1.1 = 572）。
    auto* chatContentPane = window.findChild<QWidget*>(QStringLiteral("chatContentPane"));
    QVERIFY(chatContentPane != nullptr);
    QCOMPARE(chatContentPane->minimumWidth(), 572);
    // 导航栏缩放后仍须是定宽窄条。这里重放的曾经是「带文字时代」的 160~260，
    // 会把构造时的定宽顶掉——缩放一次导航栏就胖回去，还在旁边留出一条空白列。
    auto* navRail = window.findChild<QWidget*>(QStringLiteral("navRail"));
    QVERIFY(navRail != nullptr);
    QCOMPARE(navRail->minimumWidth(), navRail->maximumWidth());
    QCOMPARE(navRail->maximumWidth(), 70);  // 64 × 1.1

    // Ctrl+- 缩回，Ctrl+0 复位；最小宽高须一并还原，否则布局缩不回去。
    QTest::keyClick(&window, Qt::Key_Minus, Qt::ControlModifier);
    QCOMPARE(qRound(UiZoom::factor() * 100), 100);
    QCOMPARE(chatContentPane->minimumWidth(), 520);
    QTest::keyClick(&window, Qt::Key_Equal, Qt::ControlModifier);
    QTest::keyClick(&window, Qt::Key_0, Qt::ControlModifier);
    QCOMPARE(qRound(UiZoom::factor() * 100), 100);
    QCOMPARE(chatContentPane->minimumWidth(), 520);
    QCOMPARE(window.minimumWidth(), 980);

    UiZoom::setFactor(1.0);
}

namespace {

// 通讯录列表的一行：要么是分组表头，要么是联系人。
struct DirectoryRow {
    bool isHeader = false;
    QString text;
    QString group;
    int count = 0;
    bool hidden = false;
};

QList<DirectoryRow> readDirectory(QListWidget* list) {
    QList<DirectoryRow> rows;
    for (int index = 0; index < list->count(); ++index) {
        QListWidgetItem* item = list->item(index);
        DirectoryRow row;
        row.isHeader = item->data(Qt::UserRole + 8).toBool();
        row.text = item->data(Qt::UserRole + 1).toString();
        row.group = item->data(Qt::UserRole + 9).toString();
        row.count = item->data(Qt::UserRole + 11).toInt();
        row.hidden = item->isHidden();
        rows.append(row);
    }
    return rows;
}

QStringList visibleLabels(QListWidget* list) {
    QStringList labels;
    for (const DirectoryRow& row : readDirectory(list)) {
        if (row.hidden) continue;
        labels.append(row.isHeader ? QStringLiteral("[%1 %2]").arg(row.text).arg(row.count) : row.text);
    }
    return labels;
}

QListWidget* openContactsPage(MainWindow& window) {
    auto* navButton = window.findChild<QPushButton*>(QStringLiteral("contactsNavButton"));
    if (navButton) navButton->click();
    return window.findChild<QListWidget*>(QStringLiteral("contactsList"));
}

int headerRowOf(QListWidget* list, const QString& groupName) {
    for (int index = 0; index < list->count(); ++index) {
        QListWidgetItem* item = list->item(index);
        if (item->data(Qt::UserRole + 8).toBool() && item->data(Qt::UserRole + 9).toString() == groupName) return index;
    }
    return -1;
}

}  // namespace

void MainWindowLayoutTest::contactDirectoryStaysFlatUntilAGroupExists() {
    auto client = std::make_unique<FakeRemoteIMClient>();
    RemoteIMApplication app(QStringLiteral("desktop-user"), std::move(client));
    app.addContact(QStringLiteral("alice"), QStringLiteral("Alice"));
    app.addContact(QStringLiteral("bob"), QStringLiteral("Bob"));

    MainWindow window(app);
    QListWidget* list = openContactsPage(window);
    QVERIFY(list != nullptr);

    // 一个分组都没建过的人不该被迫看见分组结构：连「未分组」表头也不该出现。
    QCOMPARE(visibleLabels(list), QStringList({"Alice", "Bob"}));
    for (const DirectoryRow& row : readDirectory(list)) {
        QVERIFY(!row.isHeader);
    }
}

void MainWindowLayoutTest::contactDirectoryShowsGroupsWithUngroupedLast() {
    auto client = std::make_unique<FakeRemoteIMClient>();
    RemoteIMApplication app(QStringLiteral("desktop-user"), std::move(client));
    app.addContact(QStringLiteral("alice"), QStringLiteral("Alice"));
    app.addContact(QStringLiteral("bob"), QStringLiteral("Bob"));
    QVERIFY(app.createContactGroup(QStringLiteral("同事")));
    QVERIFY(app.createContactGroup(QStringLiteral("家人")));
    app.setContactGroup(QStringLiteral("alice"), QStringLiteral("同事"));

    MainWindow window(app);
    QListWidget* list = openContactsPage(window);
    QVERIFY(list != nullptr);

    // 分组按创建顺序；「家人」还没人也要显示，否则新建分组看起来什么都没发生。
    // 没有分组的 Bob 不套任何标题，直接跟在分组后面、和分组同一层。
    QCOMPARE(visibleLabels(list), QStringList({"[同事 1]", "Alice", "[家人 0]", "Bob"}));

    // 移动之后 Bob 落到「家人」下面，没有分组的人一个不剩，列表里也不该
    // 冒出一个空的「未分组」标题。
    app.setContactGroup(QStringLiteral("bob"), QStringLiteral("家人"));
    QCOMPARE(visibleLabels(list), QStringList({"[同事 1]", "Alice", "[家人 1]", "Bob"}));
}

void MainWindowLayoutTest::clickingGroupHeaderCollapsesItsMembers() {
    auto client = std::make_unique<FakeRemoteIMClient>();
    RemoteIMApplication app(QStringLiteral("desktop-user"), std::move(client));
    app.addContact(QStringLiteral("alice"), QStringLiteral("Alice"));
    app.addContact(QStringLiteral("bob"), QStringLiteral("Bob"));
    QVERIFY(app.createContactGroup(QStringLiteral("同事")));
    app.setContactGroup(QStringLiteral("alice"), QStringLiteral("同事"));

    MainWindow window(app);
    QListWidget* list = openContactsPage(window);
    QVERIFY(list != nullptr);

    const int headerRow = headerRowOf(list, QStringLiteral("同事"));
    QVERIFY(headerRow >= 0);
    emit list->itemClicked(list->item(headerRow));

    // 收起后成员不见了，但表头和人数还在——那是唯一还能看出组里有人的线索。
    // 没有分组的 Bob 不属于任何一节，折叠影响不到他。
    QCOMPARE(visibleLabels(list), QStringList({"[同事 1]", "Bob"}));

    emit list->itemClicked(list->item(headerRowOf(list, QStringLiteral("同事"))));
    QCOMPARE(visibleLabels(list), QStringList({"[同事 1]", "Alice", "Bob"}));
}

void MainWindowLayoutTest::videoCoverRetainsPhysicalPixels() {
    QTemporaryDir dir;
    QImage image(960, 540, QImage::Format_RGB32);
    image.fill(Qt::red);
    const auto path = dir.filePath("cover.png");
    QVERIFY(image.save(path));
    RemoteIMApplication app("preview-user", std::make_unique<FakeRemoteIMClient>());
    app.addContact("phone-user", "Phone");
    app.selectPeer("phone-user");
    RemoteIMMessage message;
    message.fromUserId = "phone-user";
    message.toUserId = "preview-user";
    message.hasVideo = true;
    message.video.coverPath = path;
    app.chatState().appendMessageForRestore(message);
    MainWindow window(app);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    auto* button = window.findChild<QPushButton*>("messageVideoButton");
    QVERIFY(button);
    const auto physical = (QSizeF(button->iconSize()) * button->devicePixelRatioF()).toSize();
    // 等待真正解码的红色封面，不能把高 DPI 占位图当作成功结果。
    QTRY_VERIFY(button->icon().pixmap(physical).toImage().pixelColor(5, 5).red() > 200);
    const auto rendered = button->icon().pixmap(physical);
    QVERIFY(rendered.width() >= physical.width());
    QVERIFY(rendered.height() >= physical.height());
}

void MainWindowLayoutTest::messageTextUsesNativeResolutionAndRegularBodyFont() {
    RemoteIMApplication app("preview-user", std::make_unique<FakeRemoteIMClient>());
    app.addContact("phone-user", "Phone");
    app.selectPeer("phone-user");
    app.chatState().receiveText("phone-user", QStringLiteral("清晰度验证 Native text 123\n\n**强调文字**"));
    MainWindow window(app);
    window.resize(1200, 800);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    auto* browser = window.findChild<MarkdownLabel*>("messageMarkdownView");
    QVERIFY(browser);
    const auto capture = browser->grab();
    const qreal dpr = browser->devicePixelRatioF();
    QCOMPARE(capture.devicePixelRatioF(), dpr);
    QCOMPARE(capture.size(), (QSizeF(browser->size()) * dpr).toSize());
    // 正文字号来自两端共用的那份主题。原来这里还顺带断言了字重是 Normal，
    // 挡的是 QTextDocument 把 CSS 的 700 读成 Black 那个坑；自绘之后没有 CSS，
    // 字重是主题里的常量，由 markdown_layout_test 直接盯着。
    QCOMPARE(browser->theme().bodyPixelSize, UiZoom::textPixels(UiZoom::TextRole::Body));
    for (auto* widget = static_cast<QWidget*>(browser); widget; widget = widget->parentWidget())
        QVERIFY2(!widget->graphicsEffect(), "Message text must not pass through a graphics-effect bitmap cache");
    qInfo() << "message-render-verification" << "dpr" << dpr << "logical" << browser->size() << "physical"
            << capture.size() << "body px" << browser->theme().bodyPixelSize;
}

void MainWindowLayoutTest::outgoingTextBubbleDoesNotReserveUnusedLineWidth_data() {
    QTest::addColumn<QString>("text");
    QTest::newRow("short") << QStringLiteral("Hello");
    QTest::newRow("more-than-fifty-characters") << QStringLiteral(
        "Please compare the tool execution limits and intermediate messages with the reference implementation.");
}

// Measure the actual painted body and inline timestamp, rather than assuming
// that a default QWidget size hint is the width of the visible message.
void MainWindowLayoutTest::outgoingTextBubbleDoesNotReserveUnusedLineWidth() {
    QFETCH(QString, text);
    ScopedMessageHeaderFont font;
    RemoteIMApplication app("compact-message-user", std::make_unique<FakeRemoteIMClient>());
    app.addContact("phone-user", "Phone");
    app.selectPeer("phone-user");
    MainWindow window(app);
    window.resize(1280, 820);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    auto* editor = window.findChild<QTextEdit*>(QStringLiteral("messageEditor"));
    auto* send = window.findChild<QPushButton*>(QStringLiteral("sendButton"));
    QVERIFY(editor && send);
    editor->setPlainText(text);
    QTest::mouseClick(send, Qt::LeftButton);
    // Fake transport acceptance and delivery each refresh the row. Let both
    // finish before keeping widget pointers for the paint measurement.
    QTest::qWait(100);
    auto* bubble = window.findChild<QWidget*>(QStringLiteral("messageBubbleOutgoing"));
    QVERIFY(bubble);
    auto* body = bubble->findChild<MarkdownLabel*>(QStringLiteral("messageMarkdownView"));
    QVERIFY(body);
    QTRY_VERIFY(body->isVisible() && body->height() > 0);
    const QImage image = body->grab().toImage().convertToFormat(QImage::Format_RGB32);
    int lastInk = -1;
    for (int y = 0; y < image.height(); ++y)
        for (int x = 0; x < image.width(); ++x) {
            const QColor pixel = image.pixelColor(x, y);
            if (pixel.red() < 180 && pixel.green() < 200) lastInk = qMax(lastInk, x);
        }
    QVERIFY(lastInk >= 0);
    const qreal emptyRight = body->width() - (lastInk + 1) / image.devicePixelRatioF();
    QVERIFY2(emptyRight <= UiZoom::s(20), qPrintable(QStringLiteral("Unused blue width: %1").arg(emptyRight)));
    const QByteArray capture = qgetenv("MAICHAT_CAPTURE_COMPACT_IM");
    if (!capture.isEmpty())
        QVERIFY(window.grab().save(QString::fromUtf8(capture) + QLatin1Char('-') +
                                   QString::fromLatin1(QTest::currentDataTag()) + QStringLiteral(".png")));
}

void MainWindowLayoutTest::renamedHeaderButtonsKeepTheirCompactAppearance() {
    RemoteIMApplication app("preview-user", std::make_unique<FakeRemoteIMClient>());
    app.addContact("phone-user", "Phone");
    app.selectPeer("phone-user");
    MainWindow window(app);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    for (const auto* name : {"remoteDesktopButton", "moreButton"}) {
        auto* button = window.findChild<QPushButton*>(QString::fromLatin1(name));
        QVERIFY(button);
        QCOMPARE(button->size(), QSize(UiZoom::s(28), UiZoom::s(28)));
        // 圆角处应露出白色标题栏，而不是原生灰色按钮底框。
        const auto image = button->grab().toImage();
        const auto corner = image.pixelColor(0, 0);
        QCOMPARE(corner.alpha(), 0);
    }
}

// 实际标题栏取证与正常 App 使用相同 DPI 配置，避免默认 QtTest 的 96 DPI/1x 图标路径。
int main(int argc, char* argv[]) {
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
    QApplication::setHighDpiScaleFactorRoundingPolicy(Qt::HighDpiScaleFactorRoundingPolicy::PassThrough);
    QApplication::setAttribute(Qt::AA_EnableHighDpiScaling);
    QApplication::setAttribute(Qt::AA_UseHighDpiPixmaps);
#endif
    QApplication application(argc, argv);
    MainWindowLayoutTest test;
    return QTest::qExec(&test, argc, argv);
}
#include "MainWindowLayoutTest.moc"
