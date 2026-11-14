// 文件作用：验证桌面端 IM 发收消息、附件、状态和会话切换。
// 运行边界：由测试进程驱动实际组件或替身，不作为正式 App 的用户入口。
#include <QFile>
#include <QSignalSpy>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPainter>
#include <QTemporaryDir>
#include <QTest>
#include <algorithm>
#include <memory>

#include "app/RemoteIMApplication.h"
#include "im/FakeRemoteIMClient.h"
#include "im/TimSdkApi.h"
#include "im/TimSdkRemoteIMClient.h"
#include "storage/LocalMessageDatabase.h"

namespace {
// Hold only SDK completions so the real parser and application can consume both
// arrival orders. No avatar downloader, account service or real SDK is invoked.
class AvatarTimSdkApi final : public TimSdkApi {
public:
    int init(quint64, const QString&) override {
        return 0;
    }
    void uninit() override {}
    int login(const QString&, const QString&, TimSdkCompletion completion) override {
        if (completion) completion(0, {}, {});
        return 0;
    }
    int logout(TimSdkCompletion completion) override {
        if (completion) completion(0, {}, {});
        return 0;
    }
    int sendMessage(const QString&, int, const QString&, TimSdkCompletion) override {
        return -1;
    }
    int getConversationList(TimSdkCompletion completion) override {
        conversations.append(std::move(completion));
        return 0;
    }
    int getFriendList(TimSdkCompletion completion) override {
        ++friendRequests;
        friends.append(std::move(completion));
        return 0;
    }
    int getUserProfiles(const QString& request, TimSdkCompletion completion) override {
        profileRequests.append(QJsonDocument::fromJson(request.toUtf8()).object());
        QJsonArray profiles;
        const auto ids = QJsonDocument::fromJson(request.toUtf8())
                             .object()
                             .value(QStringLiteral("friendship_getprofilelist_param_identifier_array"))
                             .toArray();
        for (const auto& id : ids) {
            const QString user = id.toString();
            if (user == QStringLiteral("peer")) ++peerProfileRequests;
            profiles.append(QJsonObject{
                {QStringLiteral("user_profile_identifier"), user},
                {QStringLiteral("user_profile_nick_name"), QStringLiteral("SDK profile")},
                {QStringLiteral("user_profile_face_url"),
                 user == QStringLiteral("peer") ? (peerProfileAvatar.isNull() ? friendAvatar : peerProfileAvatar)
                                                : QString()}});
        }
        if (completion) completion(0, {}, encode(profiles));
        return 0;
    }
    int modifySelfProfile(const QString&, TimSdkCompletion completion) override {
        if (completion) completion(0, {}, {});
        return 0;
    }
    int getJoinedGroupList(TimSdkCompletion completion) override {
        if (completion) completion(0, {}, QStringLiteral("[]"));
        return 0;
    }
    int deleteFriend(const QString&, TimSdkCompletion) override {
        return -1;
    }
    int deleteConversation(const QString&, int, TimSdkCompletion) override {
        return -1;
    }
    int getMessageList(const QString&, int, const QString&, TimSdkCompletion completion) override {
        if (completion) completion(0, {}, QStringLiteral("[]"));
        return 0;
    }
    void addReceiveMessageCallback(TimSdkReceiveMessagesCallback) override {}
    void removeReceiveMessageCallback() override {}

    void finishFriends() {
        QJsonArray rows;
        if (includeAcceptedPeer) rows.append(friendRow(QStringLiteral("peer"), friendAvatar));
        if (includeUnacceptedFriend)
            rows.append(friendRow(QStringLiteral("unaccepted-peer"),
                                  QStringLiteral("https://avatar.example.invalid/unaccepted.png")));
        auto pending = std::move(friends);
        friends.clear();
        for (auto& completion : pending)
            if (completion) completion(0, {}, encode(rows));
    }
    void finishConversations(const QString& avatar) {
        const QJsonArray rows{QJsonObject{{QStringLiteral("conv_id"), QStringLiteral("peer")},
                                          {QStringLiteral("conv_type"), 1},
                                          {QStringLiteral("conv_show_name"), QStringLiteral("Cached conversation")},
                                          {QStringLiteral("conv_face_url"), avatar}}};
        auto pending = std::move(conversations);
        conversations.clear();
        for (auto& completion : pending)
            if (completion) completion(0, {}, encode(rows));
    }

    QString friendAvatar = QStringLiteral("https://avatar.example.invalid/fresh-profile.png");
    QString peerProfileAvatar;
    int friendRequests = 0;
    int peerProfileRequests = 0;
    bool includeUnacceptedFriend = false;
    bool includeAcceptedPeer = true;
    QList<QJsonObject> profileRequests;
    QList<TimSdkCompletion> friends, conversations;

private:
    static QString encode(const QJsonArray& values) {
        return QString::fromUtf8(QJsonDocument(values).toJson(QJsonDocument::Compact));
    }
    static QJsonObject friendRow(const QString& user, const QString& avatar) {
        return {{QStringLiteral("friend_profile_identifier"), user},
                {QStringLiteral("friend_profile_user_profile"),
                 QJsonObject{{QStringLiteral("user_profile_nick_name"), QStringLiteral("SDK friend")},
                             {QStringLiteral("user_profile_face_url"), avatar}}}};
    }
};

QString contactAvatar(const ChatState& state, const QString& user) {
    for (const auto& contact : state.contacts())
        if (contact.userId == user) return contact.avatarUrl;
    return {};
}

bool seedPagedHistory(const QString& path) {
    LocalMessageDatabase database(path);
    if (!database.isOpen()) return false;
    database.upsertContact(RemoteIMContact{QStringLiteral("phone-user"), QStringLiteral("Phone")});
    for (int i = 1; i <= 85; ++i) {
        RemoteIMMessage message;
        message.id = QStringLiteral("m%1").arg(i);
        message.fromUserId = QStringLiteral("phone-user");
        message.toUserId = QStringLiteral("desktop-user");
        message.direction = RemoteIMMessageDirection::Incoming;
        message.status = RemoteIMMessageStatus::Received;
        message.createdAtMillis = i;
        message.text = QStringLiteral("message-%1").arg(i);
        if (!database.insertMessageIfAbsent(message, message.fromUserId)) return false;
    }
    return true;
}
}  // namespace

class RemoteIMApplicationTest : public QObject {
    Q_OBJECT

private slots:
    void avatarPreparationCropsWithoutChangingOriginalAndRejectsOversizedInput();
    void avatarPreparationReadsTinyWebp();
    void avatarPreparationPreservesHundredMegapixelPolicy();
    void avatarUpdateRejectsCanceledOrInvalidAccountBeforeSdk();
    void friendAvatarRemainsAuthoritativeAcrossConversationArrival_data();
    void friendAvatarRemainsAuthoritativeAcrossConversationArrival();
    void accountFriendRefreshAlsoRefreshesSdkAvatar();
    void acceptedPeerProfileRefreshDoesNotDependOnSdkFriendship();
    void friendRequestWaitsForRemoteAcceptance();
    void maiChatFriendListFiltersHistoryAndDeletionKeepsMessages();
    void activityIsIndependentFromContentAndExplicitStopClosesIt();
    void sendsTextThroughClientAndMarksSent();
    void sendsTextToExplicitPeerWithoutChangingSelection();
    void sendsStructuredApprovalDecisionAndMarksSent();
    void sendsFileThroughClientAndMarksSent();
    void sendsVideoWithParsedMetadataAndGeneratedCover();
    void refusesUnsupportedVideoContainers();
    void receivesIncomingTextIntoSelectedConversation();
    void deletesContactThroughClientAfterRemoteSuccess();
    void keepsContactWhenRemoteDeletionFails();
    void persistsMessagesAcrossRestart();
    void mergesRoamingMessagesWithoutDuplicates();
    void cascadesLocalHistoryOnContactDeletion();
    void adoptsSentRemoteIdSoRoamingDoesNotDuplicate();
    void loadsRecentPageOnStartAndPagesEarlier();
    void initialRoamingSelectionLoadsRecentLocalPage();
    void olderRoamingMessageDoesNotSkipUnloadedLocalHistory();
    void contactDeletionHydratesAutomaticallySelectedConversation();
    void sameTimeHistoryKeepsDatabaseOrderAndLiveArrivalOrder();
    void liveTiesKeepArrivalOrderDuringFirstHydration();
    void liveChannelCountsUnreadRoamingDoesNot();
    void clearMessagesKeepsContactAndPersists();
    void selectingKnownPeerUsesLocalRefreshSignal();
    void broadcastDeliversOneMessagePerRecipientAndSkipsDuplicates();
    void broadcastNamesTheRecipientsThatFailed();
    void broadcastDoesNotDependOnTheSelectedConversation();
    void forwardsTextWithoutChangingTheSelectedConversation();
    void forwardsDownloadedImageAndRejectsMissingAttachment();
    void rejectsInternalAttachmentPlaceholderAsPlainText();
    void rejectsUnsupportedVoiceBeforeQueueing();
    void disconnectsActiveImOnAccountExit();
    void groupChatIsSeparateFromDirectMessagesAndSurvivesRestart();
    void membershipRemovalHidesGroupWithoutErasingHistory();
    void selfEchoBeforeAckWithClockSkewKeepsOneGroupMessage();
    void mixedLocalAndServerGroupSequencesHaveStableOrder();
    void groupUnreadAndPreviewArriveBeforeLazyHistoryAndClearOnOpen();
};

void RemoteIMApplicationTest::friendAvatarRemainsAuthoritativeAcrossConversationArrival_data() {
    QTest::addColumn<bool>("friendFirst");
    QTest::newRow("fresh-friend-before-stale-conversation") << true;
    QTest::newRow("stale-conversation-before-fresh-friend") << false;
}

void RemoteIMApplicationTest::friendAvatarRemainsAuthoritativeAcrossConversationArrival() {
    QFETCH(bool, friendFirst);
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("owned-avatar.sqlite"));
    const QString fresh = QStringLiteral("https://avatar.example.invalid/fresh-profile.png");
    const QString stale = QStringLiteral("https://avatar.example.invalid/stale-conversation.png");
    {
        auto api = std::make_unique<AvatarTimSdkApi>();
        auto* sdk = api.get();
        auto client = std::make_unique<TimSdkRemoteIMClient>(std::move(api));
        RemoteIMApplication app(QStringLiteral("owner"), std::move(client),
                                std::make_unique<LocalMessageDatabase>(path));
        app.connectToService(123456, QStringLiteral("owned-unused-signature"));
        QVERIFY(app.connectionReady());
        QVERIFY(!sdk->friends.isEmpty() && !sdk->conversations.isEmpty());
        if (friendFirst) {
            sdk->finishFriends();
            QCOMPARE(contactAvatar(app.chatState(), QStringLiteral("peer")), fresh);
            sdk->finishConversations(stale);
        } else {
            sdk->finishConversations(stale);
            QCOMPARE(contactAvatar(app.chatState(), QStringLiteral("peer")), stale);
            sdk->finishFriends();
        }
        // Check the real final application state, rather than each SDK signal
        // separately: a stale conversation avatar must stay a fallback.
        QCOMPARE(contactAvatar(app.chatState(), QStringLiteral("peer")), fresh);
    }
    LocalMessageDatabase reopened(path);
    QVERIFY(reopened.isOpen());
    ChatState restored(QStringLiteral("owner"));
    reopened.loadRecentInto(restored, 1);
    QCOMPARE(contactAvatar(restored, QStringLiteral("peer")), fresh);
}

void RemoteIMApplicationTest::accountFriendRefreshAlsoRefreshesSdkAvatar() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("owned-account-avatar.sqlite"));
    const QString oldAvatar = QStringLiteral("https://avatar.example.invalid/old-profile.png");
    const QString fresh = QStringLiteral("https://avatar.example.invalid/refreshed-profile.png");
    {
        auto api = std::make_unique<AvatarTimSdkApi>();
        auto* sdk = api.get();
        sdk->friendAvatar = oldAvatar;
        sdk->includeUnacceptedFriend = true;
        auto client = std::make_unique<TimSdkRemoteIMClient>(std::move(api));
        RemoteIMApplication app(QStringLiteral("owner"), std::move(client),
                                std::make_unique<LocalMessageDatabase>(path));
        int accountRequests = 0;
        RemoteIMApplication::AccountFriendOperations operations;
        operations.list = [&accountRequests](auto completion) {
            ++accountRequests;
            completion({RemoteIMApplication::AccountFriend{QStringLiteral("m_peer"), QStringLiteral("Account Friend"),
                                                           QStringLiteral("peer")}},
                       {});
        };
        app.setAccountFriendOperations(std::move(operations));
        app.connectToService(123456, QStringLiteral("owned-unused-signature"));
        QVERIFY(app.connectionReady());
        sdk->finishFriends();
        sdk->finishConversations(oldAvatar);
        QCOMPARE(contactAvatar(app.chatState(), QStringLiteral("peer")), oldAvatar);
        const int beforeAccount = accountRequests;
        const int beforeSdk = sdk->friendRequests + sdk->peerProfileRequests;
        sdk->friendAvatar = fresh;
        app.refreshFriendList();
        QCOMPARE(accountRequests, beforeAccount + 1);
        QVERIFY2(sdk->friendRequests + sdk->peerProfileRequests > beforeSdk,
                 "Account friendship refresh must also refresh SDK profile avatars");
        sdk->finishFriends();
        QCOMPARE(contactAvatar(app.chatState(), QStringLiteral("peer")), fresh);
        QVERIFY(app.isAcceptedFriend(QStringLiteral("peer")));
        QVERIFY(!app.isAcceptedFriend(QStringLiteral("unaccepted-peer")));
        QCOMPARE(app.maiChatAccountForImId(QStringLiteral("peer")), QStringLiteral("m_peer"));
        for (const auto& contact : app.chatState().contacts())
            if (contact.userId == QStringLiteral("peer"))
                QCOMPARE(contact.displayName, QStringLiteral("Account Friend"));
    }
    LocalMessageDatabase reopened(path);
    QVERIFY(reopened.isOpen());
    ChatState restored(QStringLiteral("owner"));
    reopened.loadRecentInto(restored, 1);
    QCOMPARE(contactAvatar(restored, QStringLiteral("peer")), fresh);
}

void RemoteIMApplicationTest::acceptedPeerProfileRefreshDoesNotDependOnSdkFriendship() {
    const QString oldAvatar = QStringLiteral("https://avatar.example.invalid/old-nonfriend.png");
    const QString fresh = QStringLiteral("https://avatar.example.invalid/fresh-nonfriend.png");
    auto api = std::make_unique<AvatarTimSdkApi>();
    auto* sdk = api.get();
    sdk->includeAcceptedPeer = false;  // 合法的账号已接受、TIM 异步同步尚未完成状态。
    sdk->friendAvatar = oldAvatar;
    sdk->peerProfileAvatar = oldAvatar;
    RemoteIMApplication app(QStringLiteral("owner"), std::make_unique<TimSdkRemoteIMClient>(std::move(api)));
    app.chatState().upsertContact({QStringLiteral("peer"), QStringLiteral("Cached peer"), oldAvatar});
    RemoteIMApplication::AccountFriendOperations operations;
    operations.list = [](auto completion) {
        completion({{QStringLiteral("m_peer"), QStringLiteral("Account Friend"), QStringLiteral("peer")}}, {});
    };
    app.setAccountFriendOperations(std::move(operations));
    app.connectToService(123456, QStringLiteral("owned-unused-signature"));
    QVERIFY(app.connectionReady());
    sdk->finishFriends();
    QCOMPARE(contactAvatar(app.chatState(), QStringLiteral("peer")), oldAvatar);
    const int before = sdk->peerProfileRequests;
    sdk->peerProfileAvatar = fresh;
    app.refreshFriendList();
    QVERIFY2(sdk->peerProfileRequests > before, "Accepted non-SDK friends need an explicit profile refresh");
    QCOMPARE(contactAvatar(app.chatState(), QStringLiteral("peer")), fresh);
    QVERIFY(sdk->profileRequests.last().value(QStringLiteral("friendship_getprofilelist_param_force_update")).toBool());
    QCOMPARE(
        sdk->profileRequests.last().value(QStringLiteral("friendship_getprofilelist_param_identifier_array")).toArray(),
        QJsonArray{QStringLiteral("peer")});
    sdk->finishConversations(oldAvatar);
    QCOMPARE(contactAvatar(app.chatState(), QStringLiteral("peer")), fresh);
    // 后到的 SDK 好友快照也不能把已显式刷新的一般 profile 降级为旧头像。
    sdk->includeAcceptedPeer = true;
    app.client().refreshFriendList();
    sdk->finishFriends();
    QCOMPARE(contactAvatar(app.chatState(), QStringLiteral("peer")), fresh);
    QVERIFY(app.isAcceptedFriend(QStringLiteral("peer")));
    for (const auto& contact : app.chatState().contacts())
        if (contact.userId == QStringLiteral("peer")) QCOMPARE(contact.displayName, QStringLiteral("Account Friend"));
}

void RemoteIMApplicationTest::
    avatarPreparationCropsWithoutChangingOriginalAndRejectsOversizedInput() {
    QTemporaryDir directory;
    const QString path = directory.filePath(QStringLiteral("original.png"));
    QImage original(1200, 600, QImage::Format_ARGB32);
    original.fill(Qt::transparent);
    {
        QPainter painter(&original);
        painter.fillRect(QRect(0, 0, 300, 600), Qt::red);
        painter.fillRect(QRect(300, 0, 600, 600), Qt::green);
        painter.fillRect(QRect(900, 0, 300, 600), Qt::blue);
    }
    QVERIFY(original.save(path, "PNG"));
    QFile source(path);
    QVERIFY(source.open(QIODevice::ReadOnly));
    const QByteArray before = source.readAll();
    source.close();
    const QByteArray prepared = RemoteIMApplication::prepareAvatarJpeg(path);
    QVERIFY(prepared.startsWith(QByteArray::fromHex("ffd8")));
    QVERIFY(prepared.size() < 2000000);
    const QImage avatar = QImage::fromData(prepared, "JPEG");
    QCOMPARE(avatar.size(), QSize(512, 512));
    QVERIFY(!avatar.hasAlphaChannel());
    for (const QPoint point : {QPoint(4, 4), QPoint(256, 256), QPoint(507, 507)}) {
        const QColor color = avatar.pixelColor(point);
        QVERIFY(color.green() > 240);
        QVERIFY(color.red() < 15 && color.blue() < 15);
    }
    QVERIFY(source.open(QIODevice::ReadOnly));
    QCOMPARE(source.readAll(), before);
    source.close();
    const QString large = directory.filePath(QStringLiteral("too-large.png"));
    QFile oversized(large);
    QVERIFY(oversized.open(QIODevice::WriteOnly));
    QVERIFY(oversized.resize(20LL * 1024 * 1024 + 1));
    oversized.close();
    QVERIFY(RemoteIMApplication::prepareAvatarJpeg(large).isEmpty());
    QVERIFY(RemoteIMApplication::prepareAvatarJpeg(directory.filePath(QStringLiteral("missing")))
                .isEmpty());
}

void RemoteIMApplicationTest::avatarPreparationReadsTinyWebp() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("owned.webp"));
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    const QByteArray bytes = QByteArray::fromHex(
        "524946461e000000574542505650384c110000002f0fc0020007d0b1b6b4b1ff8188e87f0000");
    QCOMPARE(file.write(bytes), qint64(38));
    file.close();
    const QByteArray prepared = RemoteIMApplication::prepareAvatarJpeg(path);
    QVERIFY(!prepared.isEmpty());
    const QImage image = QImage::fromData(prepared, "JPEG");
    QCOMPARE(image.size(), QSize(512, 512));
    const QColor center = image.pixelColor(256, 256);
    QVERIFY(qAbs(center.red() - 45) <= 3);
    QVERIFY(qAbs(center.green() - 99) <= 3);
    QVERIFY(qAbs(center.blue() - 141) <= 3);
    QVERIFY(file.open(QIODevice::ReadOnly));
    QCOMPARE(file.readAll(), bytes);
}

void RemoteIMApplicationTest::avatarPreparationPreservesHundredMegapixelPolicy() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("owned-100mp.png"));
    {
        QImage gray(10000, 10000, QImage::Format_Grayscale8);
        QVERIFY(!gray.isNull());
        gray.fill(QColor(127, 127, 127));
        QVERIFY(gray.save(path, "PNG"));
    }
    const QByteArray prepared = RemoteIMApplication::prepareAvatarJpeg(path);
    QVERIFY(!prepared.isEmpty());
    const QImage image = QImage::fromData(prepared, "JPEG");
    QCOMPARE(image.size(), QSize(512, 512));
    const QColor center = image.pixelColor(256, 256);
    QVERIFY(qAbs(center.red() - 127) <= 3);
    QVERIFY(qAbs(center.green() - 127) <= 3);
    QVERIFY(qAbs(center.blue() - 127) <= 3);
}

void RemoteIMApplicationTest::avatarUpdateRejectsCanceledOrInvalidAccountBeforeSdk() {
    auto client = std::make_unique<FakeRemoteIMClient>();
    auto *fake = client.get();
    RemoteIMApplication app(QStringLiteral("owner"), std::move(client));
    app.setAccountProfileProvider([] {
        return RemoteIMApplication::AccountProfile{
            true, QStringLiteral("public-account"), QStringLiteral("owner"), {}, {}, {}};
    });
    app.setAvatarAccessProvider([] {
        return RemoteIMApplication::AvatarAccess{
            QStringLiteral("public-account"), QStringLiteral("other-owner"),
            QStringLiteral("test-generation"), QStringLiteral("invalid-local-fixture"),
            QStringLiteral("test-token")};
    });
    app.connectToService(123456, QStringLiteral("test-sig"));
    bool finished = false;
    app.updateAvatar(QStringLiteral("missing.png"), {}, [&](const auto &result) {
        finished = true;
        QVERIFY(!result.updated);
        QCOMPARE(result.errorCode, QStringLiteral("not_authenticated"));
    });
    QVERIFY(finished);
    app.setAvatarAccessProvider([] {
        return RemoteIMApplication::AvatarAccess{
            QStringLiteral("public-account"), QStringLiteral("owner"),
            QStringLiteral("test-generation"), QStringLiteral("invalid-local-fixture"),
            QStringLiteral("test-token")};
    });
    finished = false;
    app.updateAvatar(QStringLiteral("missing.png"), std::make_shared<std::atomic_bool>(true),
                     [&](const auto &result) {
                         finished = true;
                         QVERIFY(!result.updated);
                         QCOMPARE(result.errorCode, QStringLiteral("canceled"));
                     });
    QVERIFY(finished);
    finished = false;
    app.updateAvatar(QStringLiteral("missing.png"), {}, [&](const auto &result) {
        finished = true;
        QVERIFY(!result.updated);
        QCOMPARE(result.errorCode, QStringLiteral("invalid_image"));
    });
    QTRY_VERIFY(finished);
    QVERIFY(fake->lastAvatarUrl().isEmpty());
}

void RemoteIMApplicationTest::groupChatIsSeparateFromDirectMessagesAndSurvivesRestart() {
    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("messages.sqlite"));
    {
        auto client = std::make_unique<FakeRemoteIMClient>();
        auto* fake = client.get();
        RemoteIMApplication app(QStringLiteral("owner"), std::move(client),
                                std::make_unique<LocalMessageDatabase>(path));
        app.connectToService(123456, QStringLiteral("sig"));
        app.addContact(QStringLiteral("friend-a"), QStringLiteral("Friend A"));
        RemoteIMGroup created;
        app.createGroup(QStringLiteral("Project group"), {QStringLiteral("friend-a")},
                        [&](bool ok, const QString&, const RemoteIMGroup& group) {
            QVERIFY(ok);
            created = group;
        });
        QTRY_VERIFY(!created.groupId.isEmpty());
        QCOMPARE(app.groups().size(), 1);
        QCOMPARE(app.selectedGroupId(), created.groupId);
        QVERIFY(app.sendGroupText(created.groupId, QStringLiteral("hello group")));
        QTRY_COMPARE(fake->lastGroupId(), created.groupId);
        QCOMPARE(fake->lastGroupText(), QStringLiteral("hello group"));
        fake->emitIncomingGroupText(created.groupId, QStringLiteral("friend-a"),
                                    QStringLiteral("reply"), QStringLiteral("remote-reply#0"));
        QTRY_COMPARE(app.groupMessages(created.groupId).size(), 2);
        QVERIFY(app.chatState().messagesWith(QStringLiteral("friend-a")).isEmpty());
        const auto contacts = app.chatState().contacts();
        QVERIFY(std::none_of(contacts.cbegin(), contacts.cend(),
            [&](const RemoteIMContact& contact) { return contact.userId == created.groupId; }));
        QCOMPARE(app.groupUnreadCount(created.groupId), 0);
        app.clearSelectedGroup();
        fake->emitIncomingGroupText(created.groupId, QStringLiteral("friend-a"),
                                    QStringLiteral("second reply"), QStringLiteral("remote-reply-2#0"));
        QCOMPARE(app.groupUnreadCount(created.groupId), 1);
        app.selectGroup(created.groupId);
        QCOMPARE(app.groupUnreadCount(created.groupId), 0);
    }
    RemoteIMApplication reopened(QStringLiteral("owner"), std::make_unique<FakeRemoteIMClient>(),
                                 std::make_unique<LocalMessageDatabase>(path));
    QCOMPARE(reopened.groups().size(), 1);
    QCOMPARE(reopened.groupMessages(reopened.groups().first().groupId).size(), 3);
    QVERIFY(reopened.chatState().messages().isEmpty());
}

void RemoteIMApplicationTest::membershipRemovalHidesGroupWithoutErasingHistory() {
    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("group.sqlite"));
    auto client = std::make_unique<FakeRemoteIMClient>();
    auto* fake = client.get();
    RemoteIMApplication app(QStringLiteral("owner"), std::move(client),
                            std::make_unique<LocalMessageDatabase>(path));
    app.connectToService(123456, QStringLiteral("sig"));
    app.addContact(QStringLiteral("friend"), QStringLiteral("Friend"));
    RemoteIMGroup created;
    app.createGroup(QStringLiteral("Team"), {QStringLiteral("friend")},
                    [&](bool ok, const QString&, const RemoteIMGroup& group) {
        QVERIFY(ok);
        created = group;
    });
    QTRY_VERIFY(!created.groupId.isEmpty());
    app.clearSelectedGroup();
    emit fake->groupUnreadReceived(created.groupId, 2);
    emit fake->groupsReceived({RemoteIMGroup{created.groupId, created.name, 2, false, 0}});
    QCOMPARE(app.groupUnreadCount(created.groupId), 2);
    app.selectGroup(created.groupId);
    QCOMPARE(app.groupUnreadCount(created.groupId), 0);
    QCOMPARE(fake->lastMarkedGroupId(), created.groupId);
    fake->emitIncomingGroupText(created.groupId, QStringLiteral("friend"),
                                QStringLiteral("archive me"), QStringLiteral("stable-group-message#0"));
    QTRY_COMPARE(app.groupMessages(created.groupId).size(), 1);
    fake->removeGroupForTest(created.groupId);
    QTRY_VERIFY(app.groups().isEmpty());
    QCOMPARE(app.selectedGroupId(), QString());
    LocalMessageDatabase independent(path);
    QCOMPARE(independent.groupMessageCount(created.groupId), 1);
}

void RemoteIMApplicationTest::selfEchoBeforeAckWithClockSkewKeepsOneGroupMessage() {
    auto client = std::make_unique<FakeRemoteIMClient>();
    auto* fake = client.get();
    RemoteIMApplication app(QStringLiteral("owner"), std::move(client));
    app.connectToService(123456, QStringLiteral("sig"));
    app.addContact(QStringLiteral("friend"), QStringLiteral("Friend"));
    RemoteIMGroup group;
    app.createGroup(QStringLiteral("Team"), {QStringLiteral("friend")},
                    [&](bool ok, const QString&, const RemoteIMGroup& created) {
        QVERIFY(ok);
        group = created;
    });
    QTRY_VERIFY(!group.groupId.isEmpty());
    emit fake->groupsReceived({RemoteIMGroup{group.groupId, group.name, 2, true}});
    QVERIFY(app.groups().first().pendingActivation);
    fake->deferNextSend();
    QVERIFY(app.sendGroupText(group.groupId, QStringLiteral("activate")));
    QCOMPARE(app.groupMessages(group.groupId).size(), 1);
    RemoteIMGroupMessage echo;
    echo.id = QStringLiteral("fake-remote-1#0");
    echo.groupId = group.groupId;
    echo.senderId = QStringLiteral("owner");
    echo.text = QStringLiteral("activate");
    echo.createdAtMillis = QDateTime::currentMSecsSinceEpoch() + 60000;
    echo.sequence = 77;
    echo.fromSelf = true;
    emit fake->groupMessagesReceived({echo}, true);
    QCOMPARE(app.groupMessages(group.groupId).size(), 2);
    fake->finishDeferredSend();
    QTRY_COMPARE(app.groupMessages(group.groupId).size(), 1);
    const auto result = app.groupMessages(group.groupId).first();
    QCOMPARE(result.id, echo.id);
    QCOMPARE(result.sequence, echo.sequence);
    QVERIFY(!result.pending);
    QVERIFY(!app.groups().first().pendingActivation);
}

void RemoteIMApplicationTest::mixedLocalAndServerGroupSequencesHaveStableOrder() {
    auto client = std::make_unique<FakeRemoteIMClient>();
    auto* fake = client.get();
    RemoteIMApplication app(QStringLiteral("owner"), std::move(client));
    emit fake->groupsReceived({RemoteIMGroup{QStringLiteral("group"), QStringLiteral("Group"), 3}});
    const RemoteIMGroupMessage sequenceOne{
        QStringLiteral("z"), QStringLiteral("group"), QStringLiteral("a"),
        QStringLiteral("one"), 1000, 1};
    const RemoteIMGroupMessage sequenceTwo{
        QStringLiteral("a"), QStringLiteral("group"), QStringLiteral("b"),
        QStringLiteral("two"), 1000, 2};
    const RemoteIMGroupMessage noSequence{
        QStringLiteral("m"), QStringLiteral("group"), QStringLiteral("owner"),
        QStringLiteral("local"), 1000, 0, true, true};
    emit fake->groupMessagesReceived({sequenceOne, sequenceTwo, noSequence}, false);
    const auto messages = app.groupMessages(QStringLiteral("group"));
    QCOMPARE(messages.size(), 3);
    QCOMPARE(messages.at(0).id, QStringLiteral("m"));
    QCOMPARE(messages.at(1).id, QStringLiteral("z"));
    QCOMPARE(messages.at(2).id, QStringLiteral("a"));
}

void RemoteIMApplicationTest::groupUnreadAndPreviewArriveBeforeLazyHistoryAndClearOnOpen() {
    auto client = std::make_unique<FakeRemoteIMClient>();
    auto* fake = client.get();
    RemoteIMApplication app(QStringLiteral("owner"), std::move(client));
    app.connectToService(123456, QStringLiteral("sig"));
    const QString id = QStringLiteral("@TGS#new");
    const RemoteIMGroupMessage preview{QStringLiteral("sdk-preview#0"), id,
        QStringLiteral("friend"), QStringLiteral("offline message"), 1700000000000LL, 1};
    emit fake->groupUnreadReceived(id, 2);
    emit fake->groupMessagesReceived({preview}, false);
    emit fake->groupsReceived({RemoteIMGroup{id, QStringLiteral("New group"), 3, false, 0}});
    QCOMPARE(app.groupUnreadCount(id), 2);
    QCOMPARE(app.groupMessages(id).size(), 1);
    QCOMPARE(app.groupMessages(id).first().text, QStringLiteral("offline message"));
    QCOMPARE(fake->groupHistoryRequestCount(), 0);
    QVERIFY(!app.hasLoadedGroupHistory(id));
    app.selectGroup(id);
    QCOMPARE(fake->groupHistoryRequestCount(), 1);
    QCOMPARE(fake->lastMarkedGroupId(), id);
    QCOMPARE(app.groupUnreadCount(id), 0);
}

void RemoteIMApplicationTest::friendRequestWaitsForRemoteAcceptance() {
    auto client = std::make_unique<FakeRemoteIMClient>();
    auto* fake = client.get();
    RemoteIMApplication app(QStringLiteral("owner"), std::move(client));
    app.setFriendAccountResolver([](const QString& phone,
                                    RemoteIMApplication::FriendAccountResolution completion) {
        QCOMPARE(phone, QStringLiteral("+8613900000002"));
        completion(QStringLiteral("peer"), {});
    });
    QSignalSpy sent(&app, &RemoteIMApplication::friendRequestSent);
    app.requestFriend(QStringLiteral("+86 13900000002"), QStringLiteral("Peer"));
    QTRY_COMPARE(sent.size(), 1);
    QCOMPARE(fake->lastFriendRequestId(), QStringLiteral("peer"));
    QVERIFY(app.chatState().contacts().isEmpty());
    emit fake->contactsReceived({RemoteIMContact{QStringLiteral("peer"), QStringLiteral("Peer")}});
    QTRY_COMPARE(app.chatState().contacts().size(), 1);
}

void RemoteIMApplicationTest::maiChatFriendListFiltersHistoryAndDeletionKeepsMessages() {
    auto client = std::make_unique<FakeRemoteIMClient>();
    auto* fake = client.get();
    RemoteIMApplication app(QStringLiteral("owner"), std::move(client));
    app.addContact(QStringLiteral("friend-im"), QStringLiteral("Friend"));
    app.addContact(QStringLiteral("ghost-im"), QStringLiteral("Past chat"));
    app.selectPeer(QStringLiteral("friend-im"));
    app.sendText(QStringLiteral("hello"));
    RemoteIMApplication::AccountFriendOperations operations;
    operations.list = [](auto completion) {
        completion({RemoteIMApplication::AccountFriend{
            QStringLiteral("m_friend"), QStringLiteral("好友昵称"),
            QStringLiteral("friend-im")}}, {});
    };
    QString requestedAccount;
    operations.send = [&requestedAccount](const QString& account, RemoteIMCompletion completion) {
        requestedAccount = account;
        completion(true, {});
    };
    operations.remove = [](const QString& account, RemoteIMCompletion completion) {
        QCOMPARE(account, QStringLiteral("m_friend"));
        completion(true, {});
    };
    app.setAccountFriendOperations(std::move(operations));
    QTRY_VERIFY(app.isAcceptedFriend(QStringLiteral("friend-im")));
    QCOMPARE(app.chatState().contacts().first().displayName, QStringLiteral("好友昵称"));
    emit fake->contactsReceived({RemoteIMContact{
        QStringLiteral("friend-im"), QStringLiteral("旧 IM 昵称")}});
    QCOMPARE(app.chatState().contacts().first().displayName, QStringLiteral("好友昵称"));
    QVERIFY(!app.isAcceptedFriend(QStringLiteral("ghost-im")));
    app.requestFriend(QStringLiteral("m_other"), {});
    QCOMPARE(requestedAccount, QStringLiteral("m_other"));
    QVERIFY(fake->lastFriendRequestId().isEmpty());
    app.deleteContact(QStringLiteral("friend-im"));
    QVERIFY(!app.isAcceptedFriend(QStringLiteral("friend-im")));
    QCOMPARE(app.chatState().contacts().size(), 2);
    QCOMPARE(app.chatState().messagesWith(QStringLiteral("friend-im")).size(), 1);
}

void RemoteIMApplicationTest::activityIsIndependentFromContentAndExplicitStopClosesIt() {
    auto client = std::make_unique<FakeRemoteIMClient>();
    auto* fake = client.get();
    RemoteIMApplication app(QStringLiteral("owner"), std::move(client));
    app.addContact(QStringLiteral("peer"), QStringLiteral("Peer"));
    QSignalSpy changed(&app, &RemoteIMApplication::activityChanged);
    RemoteIMActivitySignal signal{QStringLiteral("run-1"), RemoteIMActivityKind::MachineThinking,
                                  true, 1000, 1};
    emit fake->activityReceived(QStringLiteral("peer"), signal);
    QCOMPARE(app.activityForPeer(QStringLiteral("peer")).activityId, QStringLiteral("run-1"));
    QVERIFY(app.chatState().messagesWith(QStringLiteral("peer")).isEmpty());
    QCOMPARE(app.chatState().unreadCount(QStringLiteral("peer")), 0);
    signal.sequence = 2;
    emit fake->activityReceived(QStringLiteral("peer"), signal);
    QCOMPARE(changed.size(), 1); // Heartbeats must not cause UI rebuilds.
    emit fake->activityReceived(QStringLiteral("peer"),
        RemoteIMActivitySignal{QStringLiteral("old-run"), RemoteIMActivityKind::HumanTyping, false, 1000, 3});
    QTRY_VERIFY_WITH_TIMEOUT(app.activityForPeer(QStringLiteral("peer")).activityId.isEmpty(), 1800);
    signal.activityId = QStringLiteral("run-2");
    emit fake->activityReceived(QStringLiteral("peer"), signal);
    emit fake->incomingText(QStringLiteral("peer"), QStringLiteral("answer"));
    QCOMPARE(app.activityForPeer(QStringLiteral("peer")).activityId, QStringLiteral("run-2"));
    signal.sequence = 3;
    emit fake->activityReceived(QStringLiteral("peer"), signal);
    QCOMPARE(app.activityForPeer(QStringLiteral("peer")).activityId, QStringLiteral("run-2"));
    emit fake->activityReceived(QStringLiteral("peer"),
        RemoteIMActivitySignal{QStringLiteral("run-2"), RemoteIMActivityKind::MachineThinking,
                               false, 1000, 2});
    QCOMPARE(app.activityForPeer(QStringLiteral("peer")).activityId, QStringLiteral("run-2"));
    emit fake->activityReceived(QStringLiteral("peer"),
        RemoteIMActivitySignal{QStringLiteral("run-2"), RemoteIMActivityKind::MachineThinking,
                               false, 1000, 4});
    QVERIFY(app.activityForPeer(QStringLiteral("peer")).activityId.isEmpty());
    signal.sequence = 5;
    emit fake->activityReceived(QStringLiteral("peer"), signal);
    QVERIFY(app.activityForPeer(QStringLiteral("peer")).activityId.isEmpty());
}

void RemoteIMApplicationTest::sendsTextThroughClientAndMarksSent() {
    auto client = std::make_unique<FakeRemoteIMClient>();
    auto* fakeClient = client.get();
    RemoteIMApplication app(QStringLiteral("desktop-user"), std::move(client));
    QSignalSpy stateSpy(&app, &RemoteIMApplication::stateChanged);

    app.addContact(QStringLiteral("phone-user"), QStringLiteral("iPhone"));
    app.sendText(QStringLiteral("hello"));

    QCOMPARE(fakeClient->lastTextPeerId(), QStringLiteral("phone-user"));
    QCOMPARE(fakeClient->lastText(), QStringLiteral("hello"));
    const QList<RemoteIMMessage> messages = app.chatState().messagesWith(QStringLiteral("phone-user"));
    QCOMPARE(messages.size(), 1);
    QCOMPARE(messages.first().status, RemoteIMMessageStatus::Sent);
    QVERIFY(stateSpy.count() >= 2);
}

void RemoteIMApplicationTest::disconnectsActiveImOnAccountExit() {
    auto client = std::make_unique<FakeRemoteIMClient>();
    auto* fake = client.get();
    RemoteIMApplication app(QStringLiteral("desktop-user"), std::move(client));
    app.connectToService(1600148979, QStringLiteral("server-signed-user-sig"));
    QVERIFY(app.isConnected());
    QCOMPARE(fake->connectedUserId(), QStringLiteral("desktop-user"));

    bool completed = false;
    app.disconnectFromService([&completed](bool ok, const QString&) {
        completed = ok;
    });
    QVERIFY(completed);
    QVERIFY(!app.isConnected());
    QVERIFY(fake->connectedUserId().isEmpty());
}

void RemoteIMApplicationTest::sendsTextToExplicitPeerWithoutChangingSelection() {
    auto client = std::make_unique<FakeRemoteIMClient>();
    auto* fakeClient = client.get();
    RemoteIMApplication app(QStringLiteral("desktop-user"), std::move(client));
    app.addContact(QStringLiteral("selected"), QStringLiteral("Selected"));
    app.addContact(QStringLiteral("target"), QStringLiteral("Target"));
    app.selectPeer(QStringLiteral("selected"));

    QVERIFY(app.sendTextTo(QStringLiteral("target"), QStringLiteral("hello target")));

    QCOMPARE(app.chatState().selectedPeerId(), QStringLiteral("selected"));
    QCOMPARE(fakeClient->lastTextPeerId(), QStringLiteral("target"));
    QCOMPARE(fakeClient->lastText(), QStringLiteral("hello target"));
    QCOMPARE(app.chatState().messagesWith(QStringLiteral("target")).size(), 1);
    QVERIFY(app.chatState().messagesWith(QStringLiteral("selected")).isEmpty());
}

void RemoteIMApplicationTest::sendsStructuredApprovalDecisionAndMarksSent() {
    auto client = std::make_unique<FakeRemoteIMClient>();
    auto* fakeClient = client.get();
    RemoteIMApplication app(QStringLiteral("desktop-user"), std::move(client));

    app.addContact(QStringLiteral("phone-user"), QStringLiteral("iPhone"));
    app.sendApprovalDecision(
        QStringLiteral("approval-desktop-1"),
        RemoteIMApprovalAction::ApprovePrefix);

    QCOMPARE(fakeClient->lastTextPeerId(), QStringLiteral("phone-user"));
    QCOMPARE(fakeClient->lastApprovalToken(), QStringLiteral("approval-desktop-1"));
    QCOMPARE(fakeClient->lastApprovalAction(), RemoteIMApprovalAction::ApprovePrefix);
    const QList<RemoteIMMessage> messages = app.chatState().messagesWith(QStringLiteral("phone-user"));
    QCOMPARE(messages.size(), 1);
    QCOMPARE(messages.first().text, QStringLiteral("审批操作：同意并记住"));
    QCOMPARE(messages.first().status, RemoteIMMessageStatus::Sent);
    QVERIFY(messages.first().hasApprovalDecision);
    QCOMPARE(messages.first().approvalDecision.token, QStringLiteral("approval-desktop-1"));
    QCOMPARE(messages.first().approvalDecision.action, RemoteIMApprovalAction::ApprovePrefix);
}

void RemoteIMApplicationTest::sendsFileThroughClientAndMarksSent() {
    auto client = std::make_unique<FakeRemoteIMClient>();
    auto* fakeClient = client.get();
    RemoteIMApplication app(QStringLiteral("desktop-user"), std::move(client));

    app.addContact(QStringLiteral("phone-user"), QStringLiteral("iPhone"));
    app.selectPeer(QStringLiteral("phone-user"));

    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString filePath = dir.filePath(QStringLiteral("report.md"));
    QFile file(filePath);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write("# hello");
    file.close();

    app.sendFile(filePath);

    QCOMPARE(fakeClient->lastFilePeerId(), QStringLiteral("phone-user"));
    QCOMPARE(fakeClient->lastFilePath(), filePath);
    QCOMPARE(fakeClient->lastFileName(), QStringLiteral("report.md"));
    const QList<RemoteIMMessage> messages = app.chatState().messagesWith(QStringLiteral("phone-user"));
    QCOMPARE(messages.size(), 1);
    QVERIFY(messages.first().hasFile);
    QCOMPARE(messages.first().file.fileName, QStringLiteral("report.md"));
    QCOMPARE(messages.first().status, RemoteIMMessageStatus::Sent);
}

namespace {

void appendU32(QByteArray& out, quint32 value) {
    out.append(static_cast<char>((value >> 24) & 0xFF));
    out.append(static_cast<char>((value >> 16) & 0xFF));
    out.append(static_cast<char>((value >> 8) & 0xFF));
    out.append(static_cast<char>(value & 0xFF));
}

QByteArray box(const char* type, const QByteArray& content) {
    QByteArray out;
    appendU32(out, static_cast<quint32>(8 + content.size()));
    out.append(type, 4);
    out.append(content);
    return out;
}

// 一个最小但结构正确的 mp4：只有 moov/mvhd + moov/trak/tkhd，够解出时长和尺寸。
QByteArray minimalMp4(quint32 timescale, quint32 duration, quint32 width, quint32 height) {
    QByteArray mvhd;
    appendU32(mvhd, 0);
    appendU32(mvhd, 0);
    appendU32(mvhd, 0);
    appendU32(mvhd, timescale);
    appendU32(mvhd, duration);
    appendU32(mvhd, 0x00010000);

    QByteArray tkhd;
    appendU32(tkhd, 0);
    for (int i = 0; i < 5; ++i) appendU32(tkhd, 0);
    for (int i = 0; i < 2; ++i) appendU32(tkhd, 0);
    appendU32(tkhd, 0);
    appendU32(tkhd, 0);
    appendU32(tkhd, 0x00010000);
    appendU32(tkhd, 0);
    appendU32(tkhd, 0);
    appendU32(tkhd, 0);
    appendU32(tkhd, 0x00010000);
    appendU32(tkhd, 0);
    appendU32(tkhd, 0);
    appendU32(tkhd, 0);
    appendU32(tkhd, 0x40000000);
    appendU32(tkhd, width << 16);
    appendU32(tkhd, height << 16);

    QByteArray ftypContent;
    ftypContent.append("isom", 4);
    appendU32(ftypContent, 512);
    ftypContent.append("isomiso2avc1mp41", 16);

    return box("ftyp", ftypContent) + box("moov", box("mvhd", mvhd) + box("trak", box("tkhd", tkhd)));
}

QString writeVideo(const QString& path, const QByteArray& bytes) {
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly)) return QString();
    file.write(bytes);
    file.close();
    return path;
}

}  // namespace

void RemoteIMApplicationTest::sendsVideoWithParsedMetadataAndGeneratedCover() {
    auto client = std::make_unique<FakeRemoteIMClient>();
    auto* fakeClient = client.get();
    RemoteIMApplication app(QStringLiteral("desktop-user"), std::move(client));

    app.addContact(QStringLiteral("phone-user"), QStringLiteral("iPhone"));
    app.selectPeer(QStringLiteral("phone-user"));

    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    // 用 960x540（小于封面 1080 的最长边上限），封面尺寸才应当与画面尺寸一一对应。
    const QByteArray bytes = minimalMp4(600, 600 * 9, 960, 540);
    const QString videoPath =
        writeVideo(dir.filePath(QStringLiteral("screen-record.mp4")), bytes);
    QVERIFY(!videoPath.isEmpty());

    app.sendVideo(videoPath, QStringLiteral("录屏在这"));

    QCOMPARE(fakeClient->lastVideoPeerId(), QStringLiteral("phone-user"));
    const RemoteIMVideoPayload sent = fakeClient->lastVideo();
    // SDK 的九个必填字段必须全部备齐，否则对端收到的是一条打不开的空视频。
    QVERIFY(sent.isValid());
    QCOMPARE(sent.videoPath, videoPath);
    QCOMPARE(sent.videoType, QStringLiteral("mp4"));
    QCOMPARE(sent.videoSizeBytes, qint64(bytes.size()));
    QCOMPARE(sent.durationSeconds, 9);
    QVERIFY(QFileInfo::exists(sent.coverPath));
    QVERIFY(sent.coverSizeBytes > 0);
    QCOMPARE(sent.coverWidth, 960);
    QCOMPARE(sent.coverHeight, 540);

    // 本地回显是真正的视频附件：气泡据此显示封面 + 时长并支持点击播放。
    const QList<RemoteIMMessage> messages = app.chatState().messagesWith(QStringLiteral("phone-user"));
    QCOMPARE(messages.size(), 1);
    const RemoteIMMessage& echoed = messages.first();
    QVERIFY(echoed.hasVideo);
    QVERIFY(!echoed.hasFile);
    QCOMPARE(echoed.video.localPath, videoPath);
    QCOMPARE(echoed.video.fileName, QStringLiteral("screen-record.mp4"));
    QCOMPARE(echoed.video.durationSeconds, 9);
    QCOMPARE(echoed.video.sizeBytes, qint64(bytes.size()));
    // 封面路径必须落到回显里，否则气泡只能画一块黑底。
    QCOMPARE(echoed.video.coverPath, sent.coverPath);
    QVERIFY(QFileInfo::exists(echoed.video.coverPath));
    QCOMPARE(echoed.text, QStringLiteral("录屏在这"));
    QCOMPARE(echoed.status, RemoteIMMessageStatus::Sent);
}

void RemoteIMApplicationTest::refusesUnsupportedVideoContainers() {
    auto client = std::make_unique<FakeRemoteIMClient>();
    auto* fakeClient = client.get();
    RemoteIMApplication app(QStringLiteral("desktop-user"), std::move(client));
    QSignalSpy errorSpy(&app, &RemoteIMApplication::errorMessage);

    app.addContact(QStringLiteral("phone-user"), QStringLiteral("iPhone"));
    app.selectPeer(QStringLiteral("phone-user"));

    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    // mkv 之类 IM 各端播不了的容器要在本端就拦下，别等传完 100MB 再被服务端打回。
    const QString videoPath = writeVideo(dir.filePath(QStringLiteral("clip.mkv")),
                                         minimalMp4(600, 600, 640, 360));
    QVERIFY(!videoPath.isEmpty());

    app.sendVideo(videoPath);

    QCOMPARE(errorSpy.count(), 1);
    QVERIFY(fakeClient->lastVideoPeerId().isEmpty());
    QVERIFY(app.chatState().messagesWith(QStringLiteral("phone-user")).isEmpty());
}

void RemoteIMApplicationTest::receivesIncomingTextIntoSelectedConversation() {
    auto client = std::make_unique<FakeRemoteIMClient>();
    auto* fakeClient = client.get();
    RemoteIMApplication app(QStringLiteral("desktop-user"), std::move(client));

    fakeClient->emitIncomingText(QStringLiteral("phone-user"), QStringLiteral("from phone"));

    QCOMPARE(app.chatState().selectedPeerId(), QStringLiteral("phone-user"));
    const QList<RemoteIMMessage> messages = app.chatState().messagesWith(QStringLiteral("phone-user"));
    QCOMPARE(messages.size(), 1);
    QCOMPARE(messages.first().text, QStringLiteral("from phone"));
    QCOMPARE(messages.first().direction, RemoteIMMessageDirection::Incoming);
}

void RemoteIMApplicationTest::deletesContactThroughClientAfterRemoteSuccess() {
    auto client = std::make_unique<FakeRemoteIMClient>();
    auto* fakeClient = client.get();
    RemoteIMApplication app(QStringLiteral("desktop-user"), std::move(client));
    app.addContact(QStringLiteral("phone-user"), QStringLiteral("iPhone"));
    app.chatState().receiveText(QStringLiteral("phone-user"), QStringLiteral("remove me"));

    app.deleteContact(QStringLiteral(" phone-user "));

    QCOMPARE(fakeClient->lastDeletedContactId(), QStringLiteral("phone-user"));
    QVERIFY(app.chatState().contacts().isEmpty());
    QVERIFY(app.chatState().messagesWith(QStringLiteral("phone-user")).isEmpty());
}

void RemoteIMApplicationTest::keepsContactWhenRemoteDeletionFails() {
    auto client = std::make_unique<FakeRemoteIMClient>();
    auto* fakeClient = client.get();
    RemoteIMApplication app(QStringLiteral("desktop-user"), std::move(client));
    app.addContact(QStringLiteral("phone-user"), QStringLiteral("iPhone"));
    fakeClient->failNext(QStringLiteral("network failed"));
    QSignalSpy errorSpy(&app, &RemoteIMApplication::errorMessage);

    app.deleteContact(QStringLiteral("phone-user"));

    QCOMPARE(app.chatState().contacts().size(), 1);
    QCOMPARE(errorSpy.count(), 1);
    QCOMPARE(errorSpy.takeFirst().at(0).toString(), QStringLiteral("network failed"));
}

void RemoteIMApplicationTest::persistsMessagesAcrossRestart() {
    QTemporaryDir dir;
    const QString dbPath = dir.filePath("messages.db");
    {
        auto client = std::make_unique<FakeRemoteIMClient>();
        auto* fakeClient = client.get();
        RemoteIMApplication app(QStringLiteral("desktop-user"), std::move(client),
                                std::make_unique<LocalMessageDatabase>(dbPath));
        app.addContact(QStringLiteral("phone-user"), QStringLiteral("iPhone"));
        app.sendText(QStringLiteral("hello"));
        fakeClient->emitIncomingText(QStringLiteral("phone-user"), QStringLiteral("from phone"));
    }

    // 重启：新 app + 同一库文件，历史（含消息状态）应完整恢复。
    RemoteIMApplication restarted(QStringLiteral("desktop-user"), std::make_unique<FakeRemoteIMClient>(),
                                  std::make_unique<LocalMessageDatabase>(dbPath));
    restarted.selectPeer(QStringLiteral("phone-user"));
    const QList<RemoteIMMessage> messages = restarted.chatState().messagesWith(QStringLiteral("phone-user"));
    QCOMPARE(messages.size(), 2);
    const auto sent = std::find_if(messages.cbegin(), messages.cend(), [](const RemoteIMMessage& message) {
        return message.text == QStringLiteral("hello");
    });
    const auto received = std::find_if(messages.cbegin(), messages.cend(), [](const RemoteIMMessage& message) {
        return message.text == QStringLiteral("from phone");
    });
    QVERIFY(sent != messages.cend());
    QVERIFY(received != messages.cend());
    QCOMPARE(sent->status, RemoteIMMessageStatus::Sent);
    QCOMPARE(restarted.chatState().contacts().size(), 1);
}

void RemoteIMApplicationTest::mergesRoamingMessagesWithoutDuplicates() {
    QTemporaryDir dir;
    const QString dbPath = dir.filePath("messages.db");

    RemoteIMMessage roamed;
    roamed.id = QStringLiteral("sdk-100#0");
    roamed.fromUserId = QStringLiteral("phone-user");
    roamed.toUserId = QStringLiteral("desktop-user");
    roamed.direction = RemoteIMMessageDirection::Incoming;
    roamed.status = RemoteIMMessageStatus::Received;
    roamed.createdAtMillis = 1000;
    roamed.text = QStringLiteral("roamed once");

    {
        auto client = std::make_unique<FakeRemoteIMClient>();
        auto* fakeClient = client.get();
        RemoteIMApplication app(QStringLiteral("desktop-user"), std::move(client),
                                std::make_unique<LocalMessageDatabase>(dbPath));
        // 同一条 SDK 消息重复投递（实时 + 漫游），按 id 只展示一次。
        emit fakeClient->messagesReceived({roamed});
        emit fakeClient->messagesReceived({roamed});
        QCOMPARE(app.chatState().messagesWith(QStringLiteral("phone-user")).size(), 1);
    }

    // 重启后 SDK 再次漫游同一条消息：本地库已有，不重复。
    auto client = std::make_unique<FakeRemoteIMClient>();
    auto* fakeClient = client.get();
    RemoteIMApplication restarted(QStringLiteral("desktop-user"), std::move(client),
                                  std::make_unique<LocalMessageDatabase>(dbPath));
    emit fakeClient->messagesReceived({roamed});
    QCOMPARE(restarted.chatState().messagesWith(QStringLiteral("phone-user")).size(), 1);
}

void RemoteIMApplicationTest::cascadesLocalHistoryOnContactDeletion() {
    QTemporaryDir dir;
    const QString dbPath = dir.filePath("messages.db");
    {
        auto client = std::make_unique<FakeRemoteIMClient>();
        auto* fakeClient = client.get();
        RemoteIMApplication app(QStringLiteral("desktop-user"), std::move(client),
                                std::make_unique<LocalMessageDatabase>(dbPath));
        fakeClient->emitIncomingText(QStringLiteral("phone-user"), QStringLiteral("remove me"));
        app.deleteContact(QStringLiteral("phone-user"));
    }

    RemoteIMApplication restarted(QStringLiteral("desktop-user"), std::make_unique<FakeRemoteIMClient>(),
                                  std::make_unique<LocalMessageDatabase>(dbPath));
    QVERIFY(restarted.chatState().messagesWith(QStringLiteral("phone-user")).isEmpty());
    QVERIFY(restarted.chatState().contacts().isEmpty());
}

void RemoteIMApplicationTest::adoptsSentRemoteIdSoRoamingDoesNotDuplicate() {
    QTemporaryDir dir;
    const QString dbPath = dir.filePath("messages.db");
    QString adoptedId;
    {
        auto client = std::make_unique<FakeRemoteIMClient>();
        RemoteIMApplication app(QStringLiteral("desktop-user"), std::move(client),
                                std::make_unique<LocalMessageDatabase>(dbPath));
        app.addContact(QStringLiteral("phone-user"), QStringLiteral("iPhone"));
        app.sendText(QStringLiteral("hello"));
        const QList<RemoteIMMessage> messages = app.chatState().messagesWith(QStringLiteral("phone-user"));
        QCOMPARE(messages.size(), 1);
        adoptedId = messages.first().id;
        // Fake 客户端回执 fake-remote-<n>#0：临时 UUID 已被替换成稳定 id。
        QVERIFY(adoptedId.startsWith(QStringLiteral("fake-remote-")));
        QCOMPARE(messages.first().status, RemoteIMMessageStatus::Sent);
    }

    // 重启后 SDK 漫游重投同一条已发消息（相同稳定 id）：不重复显示。
    auto client = std::make_unique<FakeRemoteIMClient>();
    auto* fakeClient = client.get();
    RemoteIMApplication restarted(QStringLiteral("desktop-user"), std::move(client),
                                  std::make_unique<LocalMessageDatabase>(dbPath));
    RemoteIMMessage roamed;
    roamed.id = adoptedId;
    roamed.fromUserId = QStringLiteral("desktop-user");
    roamed.toUserId = QStringLiteral("phone-user");
    roamed.direction = RemoteIMMessageDirection::Outgoing;
    roamed.status = RemoteIMMessageStatus::Sent;
    roamed.createdAtMillis = 1;
    roamed.text = QStringLiteral("hello");
    emit fakeClient->messagesReceived({roamed});
    QCOMPARE(restarted.chatState().messagesWith(QStringLiteral("phone-user")).size(), 1);
}

void RemoteIMApplicationTest::loadsRecentPageOnStartAndPagesEarlier() {
    QTemporaryDir dir;
    const QString dbPath = dir.filePath("messages.db");
    {
        LocalMessageDatabase db(dbPath);
        for (const QString& peer : {QStringLiteral("phone-user"), QStringLiteral("background-user")}) {
            db.upsertContact(RemoteIMContact{peer, peer});
            const int count = peer == QStringLiteral("phone-user") ? 450 : 60;
            for (int i = 1; i <= count; ++i) {
                RemoteIMMessage message;
                message.id = peer + QStringLiteral("-m%1").arg(i);
                message.fromUserId = peer;
                message.toUserId = QStringLiteral("desktop-user");
                message.direction = RemoteIMMessageDirection::Incoming;
                message.status = RemoteIMMessageStatus::Received;
                message.createdAtMillis = i;
                message.text = QStringLiteral("msg-%1").arg(i);
                QVERIFY(db.insertMessageIfAbsent(message, peer));
            }
        }
    }

    auto client = std::make_unique<FakeRemoteIMClient>();
    auto* fakeClient = client.get();
    RemoteIMApplication app(QStringLiteral("desktop-user"), std::move(client),
                            std::make_unique<LocalMessageDatabase>(dbPath));
    // 登录时每个未打开会话只恢复最后一条预览，不解析所有会话的历史正文。
    QCOMPARE(app.chatState().messagesWith(QStringLiteral("phone-user")).size(), 1);
    QCOMPARE(app.chatState().messagesWith(QStringLiteral("background-user")).size(), 1);
    QCOMPARE(app.chatState().messagesWith(QStringLiteral("phone-user")).first().id,
             QStringLiteral("phone-user-m450"));
    QVERIFY(app.hasEarlierMessages(QStringLiteral("phone-user")));

    app.selectPeer(QStringLiteral("phone-user"));
    QCOMPARE(app.chatState().messagesWith(QStringLiteral("phone-user")).size(), 40);
    QCOMPARE(app.chatState().messagesWith(QStringLiteral("phone-user")).first().id,
             QStringLiteral("phone-user-m411"));
    QCOMPARE(app.chatState().messagesWith(QStringLiteral("background-user")).size(), 1);
    // 漫游重复最后一条不增加消息；反复切换复用已加载页，不叠加或重置历史。
    const RemoteIMMessage last = app.chatState().messagesWith(QStringLiteral("phone-user")).last();
    emit fakeClient->messagesReceived({last});
    app.selectPeer(QStringLiteral("background-user"));
    QCOMPARE(app.chatState().messagesWith(QStringLiteral("background-user")).size(), 40);
    app.selectPeer(QStringLiteral("phone-user"));
    QCOMPARE(app.chatState().messagesWith(QStringLiteral("phone-user")).size(), 40);

    int total = 40;
    while (app.hasEarlierMessages(QStringLiteral("phone-user"))) {
        const int loaded = app.loadEarlierMessages(QStringLiteral("phone-user"));
        QVERIFY(loaded > 0 && loaded <= 40);
        total += loaded;
        QCOMPARE(app.chatState().messagesWith(QStringLiteral("phone-user")).size(), total);
    }
    QCOMPARE(total, 450);
    QCOMPARE(app.chatState().messagesWith(QStringLiteral("phone-user")).first().id,
             QStringLiteral("phone-user-m1"));
    QCOMPARE(app.loadEarlierMessages(QStringLiteral("phone-user")), 0);
    app.selectPeer(QStringLiteral("background-user"));
    app.selectPeer(QStringLiteral("phone-user"));
    QCOMPARE(app.chatState().messagesWith(QStringLiteral("phone-user")).size(), 450);
}

void RemoteIMApplicationTest::initialRoamingSelectionLoadsRecentLocalPage() {
    QTemporaryDir directory;
    const QString path = directory.filePath(QStringLiteral("messages.db"));
    QVERIFY(seedPagedHistory(path));
    auto client = std::make_unique<FakeRemoteIMClient>();
    auto* fake = client.get();
    RemoteIMApplication app(QStringLiteral("desktop-user"), std::move(client),
                            std::make_unique<LocalMessageDatabase>(path));
    const auto preview = app.chatState().messagesWith(QStringLiteral("phone-user"));
    QCOMPARE(preview.size(), 1);
    emit fake->messagesReceived(preview);
    QCOMPARE(app.chatState().selectedPeerId(), QStringLiteral("phone-user"));
    QCOMPARE(app.chatState().messagesWith(QStringLiteral("phone-user")).size(), 40);
    QCOMPARE(app.chatState().messagesWith(QStringLiteral("phone-user")).first().id, QStringLiteral("m46"));
}

void RemoteIMApplicationTest::olderRoamingMessageDoesNotSkipUnloadedLocalHistory() {
    QTemporaryDir directory;
    const QString path = directory.filePath(QStringLiteral("messages.db"));
    QVERIFY(seedPagedHistory(path));
    auto client = std::make_unique<FakeRemoteIMClient>();
    auto* fake = client.get();
    RemoteIMApplication app(QStringLiteral("desktop-user"), std::move(client),
                            std::make_unique<LocalMessageDatabase>(path));
    app.selectPeer(QStringLiteral("phone-user"));
    LocalMessageDatabase database(path);
    const auto oldest = database.loadMessagesBefore(QStringLiteral("phone-user"), 2, QStringLiteral("m2"), 1);
    QCOMPARE(oldest.size(), 1);
    emit fake->messagesReceived(oldest);
    QCOMPARE(app.chatState().messagesWith(QStringLiteral("phone-user")).size(), 41);
    // A separately received old item is not the continuation cursor of the loaded DB page.
    QCOMPARE(app.loadEarlierMessages(QStringLiteral("phone-user")), 40);
    QVERIFY(app.hasEarlierMessages(QStringLiteral("phone-user")));
    QCOMPARE(app.loadEarlierMessages(QStringLiteral("phone-user")), 5);
    QVERIFY(!app.hasEarlierMessages(QStringLiteral("phone-user")));
    const auto all = app.chatState().messagesWith(QStringLiteral("phone-user"));
    QCOMPARE(all.size(), 85);
    for (int i = 0; i < all.size(); ++i) QCOMPARE(all.at(i).id, QStringLiteral("m%1").arg(i + 1));
}

void RemoteIMApplicationTest::contactDeletionHydratesAutomaticallySelectedConversation() {
    QTemporaryDir directory;
    const QString path = directory.filePath(QStringLiteral("messages.db"));
    QVERIFY(seedPagedHistory(path));
    RemoteIMApplication app(QStringLiteral("desktop-user"), std::make_unique<FakeRemoteIMClient>(),
                            std::make_unique<LocalMessageDatabase>(path));
    app.addContact(QStringLiteral("temporary-peer"), QStringLiteral("Temporary"));
    QCOMPARE(app.chatState().messagesWith(QStringLiteral("phone-user")).size(), 1);
    app.deleteContact(QStringLiteral("temporary-peer"));
    QCOMPARE(app.chatState().selectedPeerId(), QStringLiteral("phone-user"));
    QCOMPARE(app.chatState().messagesWith(QStringLiteral("phone-user")).size(), 40);
}

void RemoteIMApplicationTest::sameTimeHistoryKeepsDatabaseOrderAndLiveArrivalOrder() {
    QTemporaryDir directory;
    const QString path = directory.filePath(QStringLiteral("messages.db"));
    LocalMessageDatabase database(path);
    database.upsertContact(RemoteIMContact{QStringLiteral("phone-user"), QStringLiteral("Phone")});
    RemoteIMMessage message;
    message.fromUserId = QStringLiteral("phone-user");
    message.toUserId = QStringLiteral("desktop-user");
    message.direction = RemoteIMMessageDirection::Incoming;
    message.status = RemoteIMMessageStatus::Received;
    message.createdAtMillis = 100;
    message.text = QStringLiteral("Same-time history fixture");
    for (int i = 1; i <= 60; ++i) {
        message.id = QStringLiteral("m%1").arg(i, 4, 10, QLatin1Char('0'));
        QVERIFY(database.insertMessageIfAbsent(message, message.fromUserId));
    }
    auto client = std::make_unique<FakeRemoteIMClient>();
    auto* fake = client.get();
    RemoteIMApplication app(QStringLiteral("desktop-user"), std::move(client),
                            std::make_unique<LocalMessageDatabase>(path));
    app.selectPeer(QStringLiteral("phone-user"));
    QCOMPARE(app.chatState().messagesWith(QStringLiteral("phone-user")).first().id, QStringLiteral("m0021"));
    QCOMPARE(app.chatState().messagesWith(QStringLiteral("phone-user")).last().id, QStringLiteral("m0060"));
    QCOMPARE(app.loadEarlierMessages(QStringLiteral("phone-user")), 20);
    auto all = app.chatState().messagesWith(QStringLiteral("phone-user"));
    for (int i = 0; i < 60; ++i)
        QCOMPARE(all.at(i).id, QStringLiteral("m%1").arg(i + 1, 4, 10, QLatin1Char('0')));
    // The database's deterministic tie order must not replace live arrival order.
    for (const QString& id : {QStringLiteral("z-live"), QStringLiteral("a-live")}) {
        message.id = id;
        emit fake->liveMessagesReceived({message});
    }
    all = app.chatState().messagesWith(QStringLiteral("phone-user"));
    QCOMPARE(all.at(60).id, QStringLiteral("z-live"));
    QCOMPARE(all.at(61).id, QStringLiteral("a-live"));
    QVERIFY(app.chatState().updateMessageTime(QStringLiteral("m0001"), 101));
    QVERIFY(app.chatState().updateMessageTime(QStringLiteral("m0001"), 100));
    QCOMPARE(app.chatState().messagesWith(QStringLiteral("phone-user")).first().id, QStringLiteral("m0001"));
}

void RemoteIMApplicationTest::liveTiesKeepArrivalOrderDuringFirstHydration() {
    QTemporaryDir directory;
    const QString path = directory.filePath(QStringLiteral("messages.db"));
    QVERIFY(seedPagedHistory(path));
    auto client = std::make_unique<FakeRemoteIMClient>();
    auto* fake = client.get();
    RemoteIMApplication app(QStringLiteral("desktop-user"), std::move(client),
                            std::make_unique<LocalMessageDatabase>(path));
    RemoteIMMessage message;
    message.fromUserId = QStringLiteral("phone-user");
    message.toUserId = QStringLiteral("desktop-user");
    message.direction = RemoteIMMessageDirection::Incoming;
    message.status = RemoteIMMessageStatus::Received;
    message.createdAtMillis = 1000;
    message.text = QStringLiteral("Live arrival fixture");
    message.id = QStringLiteral("z-live");
    RemoteIMMessage next = message;
    next.id = QStringLiteral("a-live");
    emit fake->liveMessagesReceived({message, next});
    const auto all = app.chatState().messagesWith(QStringLiteral("phone-user"));
    QCOMPARE(all.at(all.size() - 2).id, QStringLiteral("z-live"));
    QCOMPARE(all.last().id, QStringLiteral("a-live"));
}

void RemoteIMApplicationTest::liveChannelCountsUnreadRoamingDoesNot() {
    auto client = std::make_unique<FakeRemoteIMClient>();
    auto* fakeClient = client.get();
    RemoteIMApplication app(QStringLiteral("desktop-user"), std::move(client));
    app.addContact(QStringLiteral("phone-user"), QStringLiteral("iPhone"));

    // 漫游/历史恢复通道不产生红点。
    RemoteIMMessage roamed;
    roamed.id = QStringLiteral("sdk-1#0");
    roamed.fromUserId = QStringLiteral("mac-user");
    roamed.toUserId = QStringLiteral("desktop-user");
    roamed.direction = RemoteIMMessageDirection::Incoming;
    roamed.status = RemoteIMMessageStatus::Received;
    roamed.createdAtMillis = 1000;
    roamed.text = QStringLiteral("history");
    emit fakeClient->messagesReceived({roamed});
    QCOMPARE(app.chatState().unreadCount(QStringLiteral("mac-user")), 0);

    // 实时推送的新消息计红点；同 id 重复投递（实时重发/漫游重投）不重复累计。
    RemoteIMMessage live = roamed;
    live.id = QStringLiteral("sdk-2#0");
    live.text = QStringLiteral("fresh");
    emit fakeClient->liveMessagesReceived({live});
    QCOMPARE(app.chatState().unreadCount(QStringLiteral("mac-user")), 1);
    emit fakeClient->liveMessagesReceived({live});
    emit fakeClient->messagesReceived({live});
    QCOMPARE(app.chatState().unreadCount(QStringLiteral("mac-user")), 1);
    QCOMPARE(app.chatState().messagesWith(QStringLiteral("mac-user")).size(), 2);

    // 当前选中会话（phone-user）的实时消息不计红点。
    RemoteIMMessage onScreen = roamed;
    onScreen.id = QStringLiteral("sdk-3#0");
    onScreen.fromUserId = QStringLiteral("phone-user");
    onScreen.text = QStringLiteral("on screen");
    emit fakeClient->liveMessagesReceived({onScreen});
    QCOMPARE(app.chatState().unreadCount(QStringLiteral("phone-user")), 0);

    // 打开会话即清零。
    app.selectPeer(QStringLiteral("mac-user"));
    QCOMPARE(app.chatState().unreadCount(QStringLiteral("mac-user")), 0);
}

void RemoteIMApplicationTest::clearMessagesKeepsContactAndPersists() {
    QTemporaryDir dir;
    const QString dbPath = dir.filePath("messages.db");
    {
        auto client = std::make_unique<FakeRemoteIMClient>();
        auto* fakeClient = client.get();
        RemoteIMApplication app(QStringLiteral("desktop-user"), std::move(client),
                                std::make_unique<LocalMessageDatabase>(dbPath));
        fakeClient->emitIncomingText(QStringLiteral("phone-user"), QStringLiteral("clear me"));
        QCOMPARE(app.chatState().messagesWith(QStringLiteral("phone-user")).size(), 1);

        app.clearMessagesWith(QStringLiteral("phone-user"));

        QCOMPARE(app.chatState().messagesWith(QStringLiteral("phone-user")).size(), 0);
        QCOMPARE(app.chatState().contacts().size(), 1);
        QVERIFY(!app.hasEarlierMessages(QStringLiteral("phone-user")));
    }

    // 重启后本地库里的消息同样已删，联系人保留。
    RemoteIMApplication restarted(QStringLiteral("desktop-user"), std::make_unique<FakeRemoteIMClient>(),
                                  std::make_unique<LocalMessageDatabase>(dbPath));
    QVERIFY(restarted.chatState().messagesWith(QStringLiteral("phone-user")).isEmpty());
    QCOMPARE(restarted.chatState().contacts().size(), 1);
    QCOMPARE(restarted.chatState().contacts().first().userId, QStringLiteral("phone-user"));
}

void RemoteIMApplicationTest::selectingKnownPeerUsesLocalRefreshSignal() {
    RemoteIMApplication app(QStringLiteral("desktop-user"), std::make_unique<FakeRemoteIMClient>());
    app.addContact(QStringLiteral("peer-a"), QStringLiteral("A"));
    app.addContact(QStringLiteral("peer-b"), QStringLiteral("B"));
    QSignalSpy stateSpy(&app, &RemoteIMApplication::stateChanged);
    QSignalSpy selectionSpy(&app, &RemoteIMApplication::selectionChanged);

    app.selectPeer(QStringLiteral("peer-a"));

    QCOMPARE(stateSpy.count(), 0);
    QCOMPARE(selectionSpy.count(), 1);
    QCOMPARE(selectionSpy.takeFirst().at(0).toString(), QStringLiteral("peer-a"));
}

void RemoteIMApplicationTest::broadcastDeliversOneMessagePerRecipientAndSkipsDuplicates() {
    auto client = std::make_unique<FakeRemoteIMClient>();
    RemoteIMApplication app(QStringLiteral("desktop-user"), std::move(client));
    app.addContact(QStringLiteral("alice"), QStringLiteral("Alice"));
    app.addContact(QStringLiteral("bob"), QStringLiteral("Bob"));
    app.addContact(QStringLiteral("carol"), QStringLiteral("Carol"));

    // 同一个人在「选中的联系人」和「选中的分组」里各出现一次是很常见的；
    // 不去重他就会收到两条一模一样的消息。空串同样要被丢掉。
    const int sent = app.broadcastText(
        {QStringLiteral("alice"), QStringLiteral("bob"), QStringLiteral("alice"),
         QString(), QStringLiteral("  "), QStringLiteral("carol")},
        QStringLiteral("周会推迟到明天"));
    QCOMPARE(sent, 3);

    // 群发不是一条「群消息」：每个人的会话里各有一条独立消息，
    // 收件人看到的和平时的私聊消息毫无区别。
    for (const QString& peer : {QStringLiteral("alice"), QStringLiteral("bob"), QStringLiteral("carol")}) {
        const QList<RemoteIMMessage> messages = app.chatState().messagesWith(peer);
        QCOMPARE(messages.size(), 1);
        QCOMPARE(messages.first().text, QStringLiteral("周会推迟到明天"));
        QCOMPARE(messages.first().toUserId, peer);
        QCOMPARE(messages.first().status, RemoteIMMessageStatus::Sent);
    }
}

void RemoteIMApplicationTest::broadcastNamesTheRecipientsThatFailed() {
    auto client = std::make_unique<FakeRemoteIMClient>();
    auto* fakeClient = client.get();
    RemoteIMApplication app(QStringLiteral("desktop-user"), std::move(client));
    app.addContact(QStringLiteral("alice"), QStringLiteral("Alice"));
    app.addContact(QStringLiteral("bob"), QStringLiteral("Bob"));
    QSignalSpy finishedSpy(&app, &RemoteIMApplication::broadcastFinished);

    fakeClient->failNext(QStringLiteral("network down"));
    const int sent = app.broadcastText(
        {QStringLiteral("alice"), QStringLiteral("bob")}, QStringLiteral("发版了"));
    QCOMPARE(sent, 2);

    // 部分失败只说一句「部分失败」是没法补救的——必须说出是谁没收到，
    // 用户才知道该单独补发给谁。
    QCOMPARE(finishedSpy.count(), 1);
    QCOMPARE(finishedSpy.first().at(0).toInt(), 2);
    QCOMPARE(finishedSpy.first().at(1).toStringList(), QStringList({QStringLiteral("alice")}));

    // 失败的那条留在会话里并标成失败，不会凭空消失。
    QCOMPARE(app.chatState().messagesWith(QStringLiteral("alice")).first().status,
             RemoteIMMessageStatus::Failed);
    QCOMPARE(app.chatState().messagesWith(QStringLiteral("bob")).first().status,
             RemoteIMMessageStatus::Sent);
}

void RemoteIMApplicationTest::broadcastDoesNotDependOnTheSelectedConversation() {
    auto client = std::make_unique<FakeRemoteIMClient>();
    RemoteIMApplication app(QStringLiteral("desktop-user"), std::move(client));
    app.addContact(QStringLiteral("alice"), QStringLiteral("Alice"));
    app.addContact(QStringLiteral("bob"), QStringLiteral("Bob"));
    // 当前打开的是 alice 的会话，但群发的对象里没有她。
    app.selectPeer(QStringLiteral("alice"));

    QCOMPARE(app.broadcastText({QStringLiteral("bob")}, QStringLiteral("只发给 bob")), 1);

    // 普通发送走「发给当前选中的人」，群发不能沿用这条隐含约定：
    // 沿用的话消息会发错人，而且是发给一个用户没勾选的人。
    QCOMPARE(app.chatState().messagesWith(QStringLiteral("bob")).size(), 1);
    QVERIFY(app.chatState().messagesWith(QStringLiteral("alice")).isEmpty());
}

void RemoteIMApplicationTest::forwardsTextWithoutChangingTheSelectedConversation() {
    auto client = std::make_unique<FakeRemoteIMClient>();
    auto* fakeClient = client.get();
    RemoteIMApplication app(QStringLiteral("desktop-user"), std::move(client));
    app.addContact(QStringLiteral("alice"), QStringLiteral("Alice"));
    app.addContact(QStringLiteral("bob"), QStringLiteral("Bob"));
    app.selectPeer(QStringLiteral("alice"));

    RemoteIMMessage source;
    source.fromUserId = QStringLiteral("alice");
    source.toUserId = QStringLiteral("desktop-user");
    source.text = QStringLiteral("需要转发的正文");
    source.hasQuote = true;
    source.quote.digest = QStringLiteral("不应继承的引用");
    source.hasApprovalRequest = true;

    QVERIFY(app.forwardMessage(source, QStringLiteral(" bob ")));

    QCOMPARE(app.chatState().selectedPeerId(), QStringLiteral("alice"));
    QCOMPARE(fakeClient->lastTextPeerId(), QStringLiteral("bob"));
    QCOMPARE(fakeClient->lastText(), QStringLiteral("需要转发的正文"));
    const QList<RemoteIMMessage> forwarded = app.chatState().messagesWith(QStringLiteral("bob"));
    QCOMPARE(forwarded.size(), 1);
    QCOMPARE(forwarded.first().status, RemoteIMMessageStatus::Sent);
    QVERIFY(!forwarded.first().hasQuote);
    QVERIFY(!forwarded.first().hasApprovalRequest);
}

void RemoteIMApplicationTest::forwardsDownloadedImageAndRejectsMissingAttachment() {
    auto client = std::make_unique<FakeRemoteIMClient>();
    auto* fakeClient = client.get();
    RemoteIMApplication app(QStringLiteral("desktop-user"), std::move(client));
    app.addContact(QStringLiteral("alice"), QStringLiteral("Alice"));
    app.addContact(QStringLiteral("bob"), QStringLiteral("Bob"));

    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString imagePath = dir.filePath(QStringLiteral("photo.png"));
    QFile image(imagePath);
    QVERIFY(image.open(QIODevice::WriteOnly));
    image.write("not-an-image-but-a-readable-forwarding-payload");
    image.close();

    RemoteIMMessage source;
    source.fromUserId = QStringLiteral("alice");
    source.toUserId = QStringLiteral("desktop-user");
    source.text = QStringLiteral("配文仍然跟着图片");
    source.hasImage = true;
    source.captionAbove = true;
    source.image = RemoteIMImageAttachment{imagePath, 640, 480, image.size()};

    QVERIFY(app.forwardMessage(source, QStringLiteral("bob")));
    QCOMPARE(fakeClient->lastImagePeerId(), QStringLiteral("bob"));
    QCOMPARE(fakeClient->lastImagePath(), imagePath);
    const QList<RemoteIMMessage> forwarded = app.chatState().messagesWith(QStringLiteral("bob"));
    QCOMPARE(forwarded.size(), 1);
    QVERIFY(forwarded.first().hasImage);
    QCOMPARE(forwarded.first().text, QStringLiteral("配文仍然跟着图片"));
    QVERIFY(forwarded.first().captionAbove);

    source.image.localPath = dir.filePath(QStringLiteral("deleted.png"));
    QSignalSpy errorSpy(&app, &RemoteIMApplication::errorMessage);
    QVERIFY(!app.forwardMessage(source, QStringLiteral("bob")));
    QCOMPARE(errorSpy.count(), 1);
    QCOMPARE(app.chatState().messagesWith(QStringLiteral("bob")).size(), 1);
}

void RemoteIMApplicationTest::rejectsInternalAttachmentPlaceholderAsPlainText() {
    auto client = std::make_unique<FakeRemoteIMClient>();
    auto* fakeClient = client.get();
    RemoteIMApplication app(QStringLiteral("desktop-user"), std::move(client));
    app.addContact(QStringLiteral("bob"), QStringLiteral("Bob"));

    RemoteIMMessage degraded;
    degraded.fromUserId = QStringLiteral("alice");
    degraded.toUserId = QStringLiteral("desktop-user");
    degraded.text = QStringLiteral("[图片消息] private-photo.png");
    // 下载失败的历史退化消息没有附件标记；占位串仍不能被当作普通正文转发。
    QVERIFY(!degraded.hasImage);
    QVERIFY(!app.forwardMessage(degraded, QStringLiteral("bob")));
    QVERIFY(fakeClient->lastTextPeerId().isEmpty());
    QVERIFY(app.chatState().messagesWith(QStringLiteral("bob")).isEmpty());
}

void RemoteIMApplicationTest::rejectsUnsupportedVoiceBeforeQueueing() {
    RemoteIMApplication app(QStringLiteral("desktop-user"),
                            std::make_unique<FakeRemoteIMClient>());
    app.addContact(QStringLiteral("bob"), QStringLiteral("Bob"));
    QSignalSpy errorSpy(&app, &RemoteIMApplication::errorMessage);

    RemoteIMMessage voice;
    voice.fromUserId = QStringLiteral("alice");
    voice.toUserId = QStringLiteral("desktop-user");
    voice.text = QStringLiteral("[语音消息 3s]");
    voice.hasVoice = true;
    voice.voice = RemoteIMVoiceAttachment{QStringLiteral("/tmp/voice.amr"), 3};

    QVERIFY(!app.forwardMessage(voice, QStringLiteral("bob")));
    QCOMPARE(errorSpy.count(), 1);
    QVERIFY(app.chatState().messagesWith(QStringLiteral("bob")).isEmpty());
}

QTEST_MAIN(RemoteIMApplicationTest)
#include "RemoteIMApplicationTest.moc"
