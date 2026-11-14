// 文件作用：验证桌面端Agent 读取 IM 数据和需要审批的发送工具。
// 运行边界：由测试进程驱动实际组件或替身，不作为正式 App 的用户入口。
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QFile>
#include <QDir>
#include <QCoreApplication>
#include <QUuid>
#include <QFileInfo>
#include <QImage>
#include <QTemporaryDir>
#include <QTimer>
#include <QThread>
#include <QSet>
#include <QSemaphore>
#include <QScopeGuard>
#include <QThreadPool>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QtTest>

#include <memory>
#include <atomic>
#include <future>
#include <chrono>
#include <thread>
#include <utility>

#include "MaiTool.h"
#include "MaiChatTools/MaiChatHostTools.h"
#include "app/RemoteIMApplication.h"
#include "im/FakeRemoteIMClient.h"

class MaiChatHostToolsTest : public QObject {
  Q_OBJECT

private slots:
  void registersMaiChatReadAndWriteTools();
  void nicknameToolUsesTheAccountUpdater();
  void accountToolsExposeOnlyPublicProfileAndGateAvatarWrites();
  void readsContactsMessagesSearchAndUnreadState();
  void searchesPersistedHistoryOutsidePreviewAndOpenConversationPage();
  void searchPagesPersistedHistoryWithFiltersAndStableTies();
  void searchRejectsUnsupportedArgumentsAndHonorsCancellation();
  void messageQueriesRequireAnAvailableDatabase();
  void ordinarySearchIsAsynchronousAndRejectsChangedAccountOrCancellation();
  void databaseQueryDoesNotWaitForTheGlobalThreadPool();
  void messageQueryChecksCancellationBeforePublishing();
  void blockedApplicationQueueDoesNotHoldMessageQueries_data();
  void blockedApplicationQueueDoesNotHoldMessageQueries();
  void preCanceledMessageQueriesNeverEnterTheDatabasePath();
  void sendsAndRepliesOnlyThroughApprovalGatedTools();
  void sendsAgentImageAsPersistentMediaMessage();
  void sendsAgentFileAsPersistentMessage();
  void fileMediaRejectsUnavailableOrEscapedFilesBeforeQueueing();
  void groupToolsUseTheSameApplicationFlowAndApprovalRules();
  void groupMessageToolWaitsForInitialHistoryInsteadOfReportingEmpty();
  void createGroupToolReturnsExistingPendingGroupWithoutDuplicating();
};

void MaiChatHostToolsTest::registersMaiChatReadAndWriteTools() {
  RemoteIMApplication app(QStringLiteral("owner"),
                          std::make_unique<FakeRemoteIMClient>());
  MaiToolRegistry registry;
  registerMaiChatHostTools(registry, app);

  for (const char *name :
       {"maichat_list_contacts", "maichat_request_friend", "maichat_set_nickname",
        "maichat_change_password", "maichat_get_current_account", "maichat_set_avatar",
        "maichat_list_conversations", "maichat_get_messages", "maichat_search_messages",
        "maichat_get_unread_summary", "maichat_send_text", "maichat_send_media",
        "maichat_reply_message", "maichat_broadcast_text", "maichat_list_groups",
        "maichat_create_group", "maichat_get_group_messages", "maichat_send_group_text"}) {
    QVERIFY2(registry.find(name) != nullptr, name);
  }
  QVERIFY(!registry.find("maichat_list_contacts")->requiresApproval("{}"));
  QVERIFY(!registry.find("maichat_change_password")->requiresApproval(R"({"action":"open"})"));
  QVERIFY(registry.find("maichat_request_friend")->requiresApproval("{}"));
  QVERIFY(registry.find("maichat_set_nickname")->requiresApproval("{}"));
  QVERIFY(registry.find("maichat_set_nickname")->requiresPerCallApproval("{}"));
  QVERIFY(registry.find("maichat_send_text")->requiresApproval("{}"));
  QVERIFY(registry.find("maichat_send_media")->requiresApproval("{}"));
  QVERIFY(registry.find("maichat_reply_message")->requiresApproval("{}"));
  QVERIFY(registry.find("maichat_broadcast_text")->requiresApproval("{}"));
  QVERIFY(!registry.find("maichat_list_contacts")
               ->requiresPerCallApproval("{}"));
  QVERIFY(registry.find("maichat_send_text")
              ->requiresPerCallApproval("{}"));
  QVERIFY(registry.find("maichat_send_media")
              ->requiresPerCallApproval("{}"));
  QVERIFY(registry.find("maichat_reply_message")
              ->requiresPerCallApproval("{}"));
  QVERIFY(registry.find("maichat_broadcast_text")
              ->requiresPerCallApproval("{}"));
  QVERIFY(!registry.find("maichat_list_groups")->requiresApproval("{}"));
  QVERIFY(!registry.find("maichat_get_group_messages")->requiresApproval("{}"));
  QVERIFY(registry.find("maichat_create_group")->requiresPerCallApproval("{}"));
  QVERIFY(registry.find("maichat_send_group_text")->requiresPerCallApproval("{}"));
}

void MaiChatHostToolsTest::accountToolsExposeOnlyPublicProfileAndGateAvatarWrites() {
  auto client = std::make_unique<FakeRemoteIMClient>();
  auto *fake = client.get();
  RemoteIMApplication app(QStringLiteral("owner"), std::move(client));
  MaiToolRegistry registry;
  registerMaiChatHostTools(registry, app);
  MaiToolContext context;
  auto *get = registry.find("maichat_get_current_account");
  auto *set = registry.find("maichat_set_avatar");
  QVERIFY(get && set);
  QVERIFY(!get->requiresApproval("{}"));
  QVERIFY(set->requiresApproval(R"({"path":"avatar.png"})"));
  QVERIFY(set->requiresPerCallApproval(R"({"path":"avatar.png"})"));
  const auto loggedOut = get->execute("{}", context);
  QVERIFY(!loggedOut.hasError());
  const auto empty =
      QJsonDocument::fromJson(QByteArray::fromStdString(loggedOut.output())).object();
  QCOMPARE(empty.size(), 1);
  QCOMPARE(empty.value(QStringLiteral("logged_in")).toBool(), false);
  app.setAccountProfileProvider([] {
    return RemoteIMApplication::AccountProfile{true,
                                               QStringLiteral("public-account"),
                                               QStringLiteral("owner"),
                                               QStringLiteral("Account name"),
                                               QStringLiteral("138****0000"),
                                               {}};
  });
  app.connectToService(123456, QStringLiteral("synthetic-im-credential"));
  fake->setSelfAvatar(QStringLiteral("https://example.invalid/avatar/immutable.jpg"), {});
  const auto response = get->execute("{}", context);
  QVERIFY(!response.hasError());
  const auto profile =
      QJsonDocument::fromJson(QByteArray::fromStdString(response.output())).object();
  QCOMPARE(profile.size(), 6);
  QCOMPARE(profile.value(QStringLiteral("account_id")).toString(),
           QStringLiteral("public-account"));
  QCOMPARE(profile.value(QStringLiteral("im_user_id")).toString(), QStringLiteral("owner"));
  QCOMPARE(profile.value(QStringLiteral("nickname")).toString(), QStringLiteral("Account name"));
  QCOMPARE(profile.value(QStringLiteral("phone_masked")).toString(), QStringLiteral("138****0000"));
  QCOMPARE(profile.value(QStringLiteral("avatar_url")).toString(), fake->lastAvatarUrl());
  QVERIFY(!QByteArray::fromStdString(response.output()).contains("synthetic-im-credential"));
  app.setAccountProfileProvider([] {
    return RemoteIMApplication::AccountProfile{
        true, QStringLiteral("another-account"), QStringLiteral("other-im-user"), {}, {}, {}};
  });
  const auto switched = get->execute("{}", context);
  QCOMPARE(QJsonDocument::fromJson(QByteArray::fromStdString(switched.output())).object().size(),
           1);
}

void MaiChatHostToolsTest::groupToolsUseTheSameApplicationFlowAndApprovalRules() {
  auto client = std::make_unique<FakeRemoteIMClient>();
  auto *fake = client.get();
  RemoteIMApplication app(QStringLiteral("owner"), std::move(client));
  app.connectToService(123456, QStringLiteral("sig"));
  app.addContact(QStringLiteral("alice"), QStringLiteral("Alice"));
  MaiToolRegistry registry;
  registerMaiChatHostTools(registry, app);
  MaiToolContext context;

  const auto invalid = registry.find("maichat_create_group")->execute(
      R"({"name":"Team","member_ids":["owner"]})", context);
  QVERIFY(invalid.hasError());
  const auto created = registry.find("maichat_create_group")->execute(
      R"({"name":"Team","member_ids":["alice"]})", context);
  QVERIFY(!created.hasError());
  const QJsonObject createdJson = QJsonDocument::fromJson(
      QByteArray::fromStdString(created.output())).object();
  const QString groupId = createdJson.value(QStringLiteral("group_id")).toString();
  QVERIFY(!groupId.isEmpty());
  QCOMPARE(createdJson.value(QStringLiteral("member_count")).toInt(), 2);
  QVERIFY(createdJson.value(QStringLiteral("created")).toBool());
  QCOMPARE(app.groups().first().groupId, groupId);

  const auto listed = registry.find("maichat_list_groups")->execute(
      R"({"query":"Tea","limit":2})", context);
  QVERIFY(!listed.hasError());
  QCOMPARE(QJsonDocument::fromJson(QByteArray::fromStdString(listed.output()))
               .object().value(QStringLiteral("count")).toInt(), 1);
  const QByteArray sendArgs = QJsonDocument(QJsonObject{
      {QStringLiteral("group_id"), groupId}, {QStringLiteral("text"), QStringLiteral("hello")}
  }).toJson(QJsonDocument::Compact);
  const auto sent = registry.find("maichat_send_group_text")->execute(sendArgs.toStdString(), context);
  QVERIFY(!sent.hasError());
  const QJsonObject sentJson = QJsonDocument::fromJson(
      QByteArray::fromStdString(sent.output())).object();
  QVERIFY(sentJson.value(QStringLiteral("queued")).toBool());
  QVERIFY(sentJson.value(QStringLiteral("sent")).toBool());
  QVERIFY(!sentJson.value(QStringLiteral("message_id")).toString().isEmpty());
  QVERIFY(!sentJson.value(QStringLiteral("remote_id")).toString().isEmpty());
  QCOMPARE(fake->lastGroupId(), groupId);
  QCOMPARE(fake->lastGroupText(), QStringLiteral("hello"));
  fake->emitIncomingGroupText(groupId, QStringLiteral("alice"),
                              QStringLiteral("received"), QStringLiteral("remote-1#0"));
  const QByteArray getArgs = QJsonDocument(QJsonObject{
      {QStringLiteral("group_id"), groupId}, {QStringLiteral("limit"), 20}
  }).toJson(QJsonDocument::Compact);
  const auto messages = registry.find("maichat_get_group_messages")
                            ->execute(getArgs.toStdString(), context);
  QVERIFY(!messages.hasError());
  const auto rows = QJsonDocument::fromJson(QByteArray::fromStdString(messages.output()))
                        .object().value(QStringLiteral("messages")).toArray();
  QCOMPARE(rows.size(), 2);
  QCOMPARE(rows.last().toObject().value(QStringLiteral("sender_id")).toString(),
           QStringLiteral("alice"));
  QCOMPARE(rows.last().toObject().value(QStringLiteral("remote_id")).toString(),
           QStringLiteral("remote-1#0"));
  QCOMPARE(rows.last().toObject().value(QStringLiteral("direction")).toString(),
           QStringLiteral("incoming"));
}

void MaiChatHostToolsTest::groupMessageToolWaitsForInitialHistoryInsteadOfReportingEmpty() {
  auto client = std::make_unique<FakeRemoteIMClient>();
  auto* fake = client.get();
  RemoteIMApplication app(QStringLiteral("owner"), std::move(client));
  app.connectToService(123456, QStringLiteral("sig"));
  app.addContact(QStringLiteral("alice"), QStringLiteral("Alice"));
  fake->deferNextGroupHistory();
  RemoteIMGroup created;
  app.createGroup(QStringLiteral("Team"), {QStringLiteral("alice")},
                  [&](bool ok, const QString&, const RemoteIMGroup& group) {
    QVERIFY(ok);
    created = group;
  });
  QTRY_VERIFY(!created.groupId.isEmpty());
  QVERIFY(!app.hasLoadedGroupHistory(created.groupId));
  MaiToolRegistry registry;
  registerMaiChatHostTools(registry, app);
  const QByteArray args = QJsonDocument(QJsonObject{
      {QStringLiteral("group_id"), created.groupId}, {QStringLiteral("limit"), 20}
  }).toJson(QJsonDocument::Compact);
  QTimer::singleShot(50, [fake, groupId = created.groupId] {
    fake->finishDeferredGroupHistory({RemoteIMGroupMessage{
        QStringLiteral("remote-history#0"), groupId, QStringLiteral("alice"),
        QStringLiteral("already there"), 1700000000000LL, 1}});
  });
  MaiToolContext context;
  const auto result = registry.find("maichat_get_group_messages")
                          ->execute(args.toStdString(), context);
  QVERIFY(!result.hasError());
  const auto rows = QJsonDocument::fromJson(QByteArray::fromStdString(result.output()))
                        .object().value(QStringLiteral("messages")).toArray();
  QCOMPARE(rows.size(), 1);
  QCOMPARE(rows.first().toObject().value(QStringLiteral("text")).toString(),
           QStringLiteral("already there"));
}

void MaiChatHostToolsTest::createGroupToolReturnsExistingPendingGroupWithoutDuplicating() {
  auto client = std::make_unique<FakeRemoteIMClient>();
  auto* fake = client.get();
  RemoteIMApplication app(QStringLiteral("owner"), std::move(client));
  app.connectToService(123456, QStringLiteral("sig"));
  emit fake->groupsReceived({RemoteIMGroup{QStringLiteral("existing-group"),
                                            QStringLiteral("Existing"), 2, true}});
  MaiToolRegistry registry;
  registerMaiChatHostTools(registry, app);
  MaiToolContext context;
  const auto result = registry.find("maichat_create_group")->execute(
      R"({"name":"Another","member_ids":["friend"]})", context);
  QVERIFY(!result.hasError());
  const auto output = QJsonDocument::fromJson(QByteArray::fromStdString(result.output())).object();
  QCOMPARE(output.value(QStringLiteral("created")).toBool(), false);
  QCOMPARE(output.value(QStringLiteral("pending_activation")).toBool(), true);
  QCOMPARE(output.value(QStringLiteral("group_id")).toString(), QStringLiteral("existing-group"));
  QCOMPARE(app.groups().size(), 1);
}

void MaiChatHostToolsTest::nicknameToolUsesTheAccountUpdater() {
  RemoteIMApplication app(QStringLiteral("owner"),
                          std::make_unique<FakeRemoteIMClient>());
  QString updated;
  app.setNicknameUpdater([&updated](const QString& nickname, RemoteIMCompletion completion) {
    updated = nickname;
    if (completion) completion(true, {});
  });
  MaiToolRegistry registry;
  registerMaiChatHostTools(registry, app);
  MaiToolContext context;
  const auto result = registry.find("maichat_set_nickname")
                          ->execute(R"({"nickname":" 小王 "})", context);
  QVERIFY(!result.hasError());
  QCOMPARE(updated, QStringLiteral("小王"));
  QVERIFY(QString::fromStdString(result.output()).contains(QStringLiteral("queued")));
  QVERIFY(registry.find("maichat_set_nickname")->execute("{}", context).hasError());
}

void MaiChatHostToolsTest::sendsAgentImageAsPersistentMediaMessage() {
  QTemporaryDir workspace;
  QVERIFY(workspace.isValid());
  const QString source = workspace.filePath(QStringLiteral("picture.png"));
  QImage image(2, 2, QImage::Format_ARGB32);
  image.fill(Qt::red);
  QVERIFY(image.save(source));

  auto client = std::make_unique<FakeRemoteIMClient>();
  RemoteIMApplication app(QStringLiteral("owner"), std::move(client));
  app.addContact(QStringLiteral("alice"), QStringLiteral("Alice"));
  MaiToolRegistry registry;
  registerMaiChatHostTools(registry, app);
  MaiToolContext context;
  context.root = workspace.path().toUtf8().toStdString();
  const auto sent = registry.find("maichat_send_media")->execute(
      R"({"peer_id":"alice","file_path":"picture.png","type":"image","caption":"Processed image"})",
      context);
  QVERIFY2(!sent.hasError(), sent.error().message().c_str());
  const auto sentBody = QJsonDocument::fromJson(QByteArray::fromStdString(sent.output())).object();
  QCOMPARE(sentBody.value(QStringLiteral("caption_attached")).toBool(), true);
  const auto messages = app.chatState().messagesWith(QStringLiteral("alice"));
  QCOMPARE(messages.size(), 1);
  QVERIFY(messages.last().hasImage);
  QCOMPARE(messages.last().text, QStringLiteral("Processed image"));
  QVERIFY(QFileInfo(messages.last().image.localPath).isFile());
  QVERIFY(messages.last().image.localPath != source);

  const auto denied = registry.find("maichat_send_media")->execute(
      R"({"peer_id":"alice","file_path":"../picture.png","type":"image"})",
      context);
  QVERIFY(denied.hasError());
}

void MaiChatHostToolsTest::sendsAgentFileAsPersistentMessage() {
  const QString originalApplication = QCoreApplication::applicationName();
  QCoreApplication::setApplicationName(QStringLiteral("owned-file-tool-%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces)));
  const auto restoreApplication = qScopeGuard([&] { QCoreApplication::setApplicationName(originalApplication); });
  QTemporaryDir workspace;
  QVERIFY(workspace.isValid());
  const QByteArray bytes("Owned file attachment\n");
  const QString source = workspace.filePath(QStringLiteral("report.txt"));
  QFile input(source);
  QVERIFY(input.open(QIODevice::WriteOnly));
  QCOMPARE(input.write(bytes), qint64(bytes.size()));
  input.close();
  auto client = std::make_unique<FakeRemoteIMClient>();
  auto* fake = client.get();
  RemoteIMApplication app(QStringLiteral("owner"), std::move(client),
      std::make_unique<LocalMessageDatabase>(workspace.filePath(QStringLiteral("owned-im.sqlite"))));
  app.addContact(QStringLiteral("alice"), QStringLiteral("Alice"));
  app.connectToService(123, QStringLiteral("owned-unused-signature"));
  QTRY_VERIFY(app.connectionReady());
  MaiToolRegistry registry;
  registerMaiChatHostTools(registry, app);
  auto* tool = registry.find("maichat_send_media");
  QVERIFY(tool);
  const auto schema = QJsonDocument::fromJson(QByteArray::fromStdString(tool->parametersSchema())).object();
  const auto types = schema.value(QStringLiteral("properties")).toObject()
      .value(QStringLiteral("type")).toObject().value(QStringLiteral("enum")).toArray();
  QVERIFY(types.contains(QStringLiteral("file")));
  const std::string arguments =
      R"({"peer_id":"alice","file_path":"report.txt","type":"file","caption":"Owned report"})";
  QVERIFY(tool->requiresApproval(arguments));
  QVERIFY(tool->requiresPerCallApproval(arguments));
  MaiToolContext context;
  context.root = workspace.path().toUtf8().toStdString();
  const auto sent = tool->execute(arguments, context);
  QVERIFY2(!sent.hasError(), sent.error().message().c_str());
  const auto body = QJsonDocument::fromJson(QByteArray::fromStdString(sent.output())).object();
  const auto removeOwnedCopy = qScopeGuard([&] { QFile::remove(body.value(QStringLiteral("path")).toString()); });
  QVERIFY(body.value(QStringLiteral("queued")).toBool());
  QCOMPARE(body.value(QStringLiteral("type")).toString(), QStringLiteral("file"));
  QCOMPARE(body.value(QStringLiteral("message_kind")).toString(), QStringLiteral("file"));
  QVERIFY(body.value(QStringLiteral("caption_attached")).toBool());
  const auto messages = app.chatState().messagesWith(QStringLiteral("alice"));
  QCOMPARE(messages.size(), 1);
  QVERIFY(messages.last().hasFile);
  QVERIFY(!messages.last().hasImage && !messages.last().hasVideo && !messages.last().hasVoice);
  QCOMPARE(messages.last().text, QStringLiteral("Owned report"));
  QCOMPARE(messages.last().file.fileName, QStringLiteral("report.txt"));
  QVERIFY(messages.last().file.localPath != source);
  QFile stored(messages.last().file.localPath);
  QVERIFY(stored.open(QIODevice::ReadOnly));
  QCOMPARE(stored.readAll(), bytes);
  QCOMPARE(fake->lastFilePeerId(), QStringLiteral("alice"));
  QCOMPARE(fake->lastFilePath(), messages.last().file.localPath);
  QCOMPARE(fake->lastFileName(), messages.last().file.fileName);
}

void MaiChatHostToolsTest::fileMediaRejectsUnavailableOrEscapedFilesBeforeQueueing() {
  QTemporaryDir directory;
  QVERIFY(directory.isValid());
  QVERIFY(QDir(directory.path()).mkdir(QStringLiteral("workspace")));
  const QString workspace = directory.filePath(QStringLiteral("workspace"));
  QFile empty(QDir(workspace).filePath(QStringLiteral("empty.txt")));
  QVERIFY(empty.open(QIODevice::WriteOnly));
  empty.close();
  QFile outside(directory.filePath(QStringLiteral("outside.txt")));
  QVERIFY(outside.open(QIODevice::WriteOnly));
  QCOMPARE(outside.write("owned"), qint64(5));
  outside.close();
  auto client = std::make_unique<FakeRemoteIMClient>();
  auto* fake = client.get();
  RemoteIMApplication app(QStringLiteral("owner"), std::move(client),
      std::make_unique<LocalMessageDatabase>(directory.filePath(QStringLiteral("owned-im.sqlite"))));
  app.addContact(QStringLiteral("alice"), QStringLiteral("Alice"));
  MaiToolRegistry registry;
  registerMaiChatHostTools(registry, app);
  MaiToolContext context;
  context.root = workspace.toUtf8().toStdString();
  for (const auto& path : {QStringLiteral("missing.txt"), QStringLiteral("empty.txt"),
                           QStringLiteral("../outside.txt")}) {
    const auto arguments = QJsonObject{{QStringLiteral("peer_id"), QStringLiteral("alice")},
        {QStringLiteral("file_path"), path}, {QStringLiteral("type"), QStringLiteral("file")}};
    const auto denied = registry.find("maichat_send_media")->execute(
        QJsonDocument(arguments).toJson(QJsonDocument::Compact).toStdString(), context);
    QVERIFY2(denied.hasError(), qPrintable(path));
    QVERIFY(app.chatState().messagesWith(QStringLiteral("alice")).isEmpty());
    QVERIFY(fake->lastFilePath().isEmpty());
  }
}

void MaiChatHostToolsTest::readsContactsMessagesSearchAndUnreadState() {
  QTemporaryDir directory;
  QVERIFY(directory.isValid());
  auto client = std::make_unique<FakeRemoteIMClient>();
  auto *fake = client.get();
  RemoteIMApplication app(QStringLiteral("owner"), std::move(client),
                          std::make_unique<LocalMessageDatabase>(directory.filePath(QStringLiteral("messages.sqlite"))));
  app.addContact(QStringLiteral("alice"), QStringLiteral("Alice"));
  app.addContact(QStringLiteral("bob"), QStringLiteral("Bob"));
  app.selectPeer(QStringLiteral("alice"));
  fake->emitIncomingText(QStringLiteral("alice"),
                         QStringLiteral("release is ready"));
  fake->emitIncomingText(QStringLiteral("bob"),
                         QStringLiteral("please review release"));

  MaiToolRegistry registry;
  registerMaiChatHostTools(registry, app);
  MaiToolContext context;

  const auto contacts =
      registry.find("maichat_list_contacts")->execute("{}", context);
  QVERIFY(!contacts.hasError());
  QVERIFY(QString::fromStdString(contacts.output())
              .contains(QStringLiteral("alice")));
  QVERIFY(QString::fromStdString(contacts.output())
              .contains(QStringLiteral("Alice")));

  const auto messages =
      registry.find("maichat_get_messages")
          ->execute(R"({"peer_id":"alice","limit":10})", context);
  QVERIFY(!messages.hasError());
  QVERIFY(QString::fromStdString(messages.output())
              .contains(QStringLiteral("release is ready")));

  const auto search =
      registry.find("maichat_search_messages")
          ->execute(R"({"query":"release","limit":10})", context);
  QVERIFY(!search.hasError());
  const QString searchText = QString::fromStdString(search.output());
  QVERIFY(searchText.contains(QStringLiteral("alice")));
  QVERIFY(searchText.contains(QStringLiteral("bob")));

  const auto unread =
      registry.find("maichat_get_unread_summary")->execute("{}", context);
  QVERIFY(!unread.hasError());
  const QJsonObject unreadObject =
      QJsonDocument::fromJson(QByteArray::fromStdString(unread.output()))
          .object();
  QCOMPARE(unreadObject.value(QStringLiteral("total_unread")).toInt(), 1);
}

// Search must inspect the private fixture database even when the UI deliberately
// keeps only a preview or a recent page in memory.
void MaiChatHostToolsTest::searchesPersistedHistoryOutsidePreviewAndOpenConversationPage() {
  QTemporaryDir directory;
  QVERIFY(directory.isValid());
  const QString path = directory.filePath(QStringLiteral("messages.sqlite"));
  {
    LocalMessageDatabase database(path);
    QVERIFY(database.isOpen());
    database.upsertContact({QStringLiteral("alice"), QStringLiteral("Alice")});
    for (int index = 0; index < 65; ++index) {
      RemoteIMMessage message;
      message.id = QStringLiteral("persisted-%1").arg(index, 3, 10, QLatin1Char('0'));
      message.fromUserId = QStringLiteral("alice");
      message.toUserId = QStringLiteral("owner");
      message.createdAtMillis = index + 1000;
      message.text = index == 0 ? QStringLiteral("ancient UNIQUE_NEEDLE")
                                : QStringLiteral("recent ordinary entry");
      QVERIFY(database.insertMessageIfAbsent(message, QStringLiteral("alice")));
    }
  }
  RemoteIMApplication app(QStringLiteral("owner"), std::make_unique<FakeRemoteIMClient>(),
                          std::make_unique<LocalMessageDatabase>(path));
  QCOMPARE(app.chatState().messagesWith(QStringLiteral("alice")).size(), 1);
  MaiToolRegistry registry;
  registerMaiChatHostTools(registry, app);
  MaiToolContext context;
  auto checkSearch = [&] {
    const auto result = registry.find("maichat_search_messages")->execute(
        R"({"query":"UNIQUE_NEEDLE","peer_id":"alice","limit":10})", context);
    QVERIFY2(!result.hasError(), result.error().message().c_str());
    const auto output = QJsonDocument::fromJson(QByteArray::fromStdString(result.output())).object();
    const auto matches = output.value(QStringLiteral("matches")).toArray();
    QCOMPARE(matches.size(), 1);
    QCOMPARE(matches.first().toObject().value(QStringLiteral("id")).toString(),
             QStringLiteral("persisted-000"));
    const auto fetched = registry.find("maichat_get_messages")->execute(
        R"({"peer_id":"alice","limit":10})", context);
    QVERIFY2(!fetched.hasError(), fetched.error().message().c_str());
    const auto fetchedBody = QJsonDocument::fromJson(QByteArray::fromStdString(fetched.output())).object();
    const auto messages = fetchedBody.value(QStringLiteral("messages")).toArray();
    QCOMPARE(messages.size(), 10);
    QCOMPARE(messages.first().toObject().value(QStringLiteral("id")).toString(),
             QStringLiteral("persisted-064"));
    QVERIFY(fetchedBody.value(QStringLiteral("has_more")).toBool());
    QSet<QString> ids;
    for (const auto &message : messages) ids.insert(message.toObject().value(QStringLiteral("id")).toString());
    auto page = fetchedBody;
    int pageCount = 1;
    while (page.value(QStringLiteral("has_more")).toBool() && pageCount++ < 10) {
      const QJsonObject arguments{{QStringLiteral("peer_id"), QStringLiteral("alice")},
          {QStringLiteral("limit"), 10}, {QStringLiteral("cursor"), page.value(QStringLiteral("next_cursor"))}};
      const auto next = registry.find("maichat_get_messages")->execute(
          QJsonDocument(arguments).toJson(QJsonDocument::Compact).toStdString(), context);
      QVERIFY2(!next.hasError(), next.error().message().c_str());
      page = QJsonDocument::fromJson(QByteArray::fromStdString(next.output())).object();
      for (const auto &message : page.value(QStringLiteral("messages")).toArray()) {
        const QString id = message.toObject().value(QStringLiteral("id")).toString();
        QVERIFY(!ids.contains(id));
        ids.insert(id);
      }
    }
    QCOMPARE(ids.size(), 65);
    QVERIFY(ids.contains(QStringLiteral("persisted-000")));
    QVERIFY(!page.value(QStringLiteral("has_more")).toBool());
  };
  checkSearch();
  QCOMPARE(app.chatState().messagesWith(QStringLiteral("alice")).size(), 1);
  app.selectPeer(QStringLiteral("alice"));
  QCOMPARE(app.chatState().messagesWith(QStringLiteral("alice")).size(), 40);
  checkSearch();
  QCOMPARE(app.chatState().messagesWith(QStringLiteral("alice")).size(), 40);
}

void MaiChatHostToolsTest::searchPagesPersistedHistoryWithFiltersAndStableTies() {
  QTemporaryDir directory;
  QVERIFY(directory.isValid());
  const QString path = directory.filePath(QStringLiteral("messages.sqlite"));
  {
    LocalMessageDatabase database(path);
    QVERIFY(database.isOpen());
    for (const QString &peer : {QStringLiteral("alice"), QStringLiteral("bob")}) {
      database.upsertContact({peer, peer});
      for (int index = 0; index < 6; ++index) {
        RemoteIMMessage message;
        message.id = peer + QString::number(index);
        message.direction = index == 5 ? RemoteIMMessageDirection::Incoming
                                        : RemoteIMMessageDirection::Outgoing;
        message.fromUserId = index == 5 ? peer : QStringLiteral("owner");
        message.toUserId = index == 5 ? QStringLiteral("owner") : peer;
        message.createdAtMillis = index == 0 ? 100 : 200;
        message.text = QStringLiteral("Release NOTE");
        message.hasFile = index != 4;
        QVERIFY(database.insertMessageIfAbsent(message, peer));
      }
    }
  }
  RemoteIMApplication app(QStringLiteral("owner"), std::make_unique<FakeRemoteIMClient>(),
                          std::make_unique<LocalMessageDatabase>(path));
  MaiToolRegistry registry;
  registerMaiChatHostTools(registry, app);
  MaiToolContext context;
  QJsonObject arguments{{QStringLiteral("query"), QStringLiteral("note release")},
                        {QStringLiteral("peer_id"), QStringLiteral("alice")},
                        {QStringLiteral("kind"), QStringLiteral("file")},
                        {QStringLiteral("direction"), QStringLiteral("outgoing")},
                        {QStringLiteral("since_ms"), QStringLiteral("200")},
                        {QStringLiteral("until_ms"), QStringLiteral("200")},
                        {QStringLiteral("limit"), 2}};
  auto search = [&](const QJsonObject &input) {
    return registry.find("maichat_search_messages")->execute(
        QJsonDocument(input).toJson(QJsonDocument::Compact).toStdString(), context);
  };
  const auto first = search(arguments);
  QVERIFY2(!first.hasError(), first.error().message().c_str());
  const auto firstBody = QJsonDocument::fromJson(QByteArray::fromStdString(first.output())).object();
  const auto firstRows = firstBody.value(QStringLiteral("matches")).toArray();
  QCOMPARE(firstRows.size(), 2);
  QCOMPARE(firstRows.at(0).toObject().value(QStringLiteral("id")).toString(), QStringLiteral("alice3"));
  QCOMPARE(firstRows.at(1).toObject().value(QStringLiteral("id")).toString(), QStringLiteral("alice2"));
  QCOMPARE(firstBody.value(QStringLiteral("coverage")).toString(), QStringLiteral("local_persisted"));
  QVERIFY(firstBody.value(QStringLiteral("local_available")).toBool());
  QVERIFY(!firstBody.value(QStringLiteral("cloud_history_complete")).toBool(true));
  QVERIFY(firstBody.value(QStringLiteral("has_more")).toBool());
  QVERIFY(!firstBody.value(QStringLiteral("no_matches")).toBool(true));
  QCOMPARE(firstBody.value(QStringLiteral("limit")).toInt(), 2);
  QCOMPARE(firstBody.value(QStringLiteral("order")).toString(), QStringLiteral("created_at_ms_desc_id_desc"));
  QVERIFY(!firstBody.value(QStringLiteral("snapshot")).toBool(true));
  const QString cursor = firstBody.value(QStringLiteral("next_cursor")).toString();
  QVERIFY(!cursor.isEmpty());
  arguments.insert(QStringLiteral("cursor"), cursor);
  const auto second = search(arguments);
  QVERIFY2(!second.hasError(), second.error().message().c_str());
  const auto secondBody = QJsonDocument::fromJson(QByteArray::fromStdString(second.output())).object();
  const auto secondRows = secondBody.value(QStringLiteral("matches")).toArray();
  QCOMPARE(secondRows.size(), 1);
  QCOMPARE(secondRows.first().toObject().value(QStringLiteral("id")).toString(), QStringLiteral("alice1"));
  QVERIFY(!secondBody.value(QStringLiteral("has_more")).toBool(true));
  QVERIFY(!secondBody.value(QStringLiteral("no_matches")).toBool(true));
  QVERIFY(secondBody.value(QStringLiteral("next_cursor")).toString().isEmpty());
  arguments.insert(QStringLiteral("peer_id"), QStringLiteral("bob"));
  QVERIFY(search(arguments).hasError()); // A continuation cannot cross filter/account scopes.
  arguments.insert(QStringLiteral("peer_id"), QStringLiteral("alice"));
  app.connectToService(123456, QStringLiteral("synthetic-signature"));
  QVERIFY(search(arguments).hasError()); // A reconnect invalidates an earlier session's cursor.
  arguments.remove(QStringLiteral("cursor"));
  arguments.insert(QStringLiteral("query"), QStringLiteral("absent-token-4937"));
  const auto empty = search(arguments);
  QVERIFY2(!empty.hasError(), empty.error().message().c_str());
  const auto emptyBody = QJsonDocument::fromJson(QByteArray::fromStdString(empty.output())).object();
  QVERIFY(emptyBody.value(QStringLiteral("no_matches")).toBool());
  QVERIFY(!emptyBody.value(QStringLiteral("has_more")).toBool(true));
  arguments.remove(QStringLiteral("query"));
  arguments.insert(QStringLiteral("peer_id"), QStringLiteral("alice"));
  const auto fetched = registry.find("maichat_get_messages")->execute(
      QJsonDocument(arguments).toJson(QJsonDocument::Compact).toStdString(), context);
  QVERIFY2(!fetched.hasError(), fetched.error().message().c_str());
  const auto fetchedBody = QJsonDocument::fromJson(QByteArray::fromStdString(fetched.output())).object();
  const auto fetchedRows = fetchedBody.value(QStringLiteral("messages")).toArray();
  QCOMPARE(fetchedRows.size(), 2);
  QCOMPARE(fetchedRows.first().toObject().value(QStringLiteral("id")).toString(), QStringLiteral("alice3"));
  arguments.insert(QStringLiteral("cursor"), fetchedBody.value(QStringLiteral("next_cursor")));
  const auto fetchedNext = registry.find("maichat_get_messages")->execute(
      QJsonDocument(arguments).toJson(QJsonDocument::Compact).toStdString(), context);
  QVERIFY2(!fetchedNext.hasError(), fetchedNext.error().message().c_str());
  QCOMPARE(QJsonDocument::fromJson(QByteArray::fromStdString(fetchedNext.output())).object()
      .value(QStringLiteral("messages")).toArray().size(), 1);
}

void MaiChatHostToolsTest::searchRejectsUnsupportedArgumentsAndHonorsCancellation() {
  QTemporaryDir directory;
  QVERIFY(directory.isValid());
  RemoteIMApplication app(QStringLiteral("owner"), std::make_unique<FakeRemoteIMClient>(),
                          std::make_unique<LocalMessageDatabase>(directory.filePath(QStringLiteral("messages.sqlite"))));
  MaiToolRegistry registry;
  registerMaiChatHostTools(registry, app);
  MaiToolContext context;
  auto *tool = registry.find("maichat_search_messages");
  QVERIFY(registry.find("maichat_get_messages")->execute(
      R"({"peer_id":"alice","query":"needle"})", context).hasError());
  for (const char *arguments : {
           R"({"query":"needle","unsupported_filter":true})",
           R"({"query":"needle","direction":"sideways"})",
           R"({"query":"needle","kind":"unknown"})",
           R"({"query":"needle","limit":0})",
           R"({"query":"needle","limit":2.5})",
           R"({"query":"needle","since_ms":"broken"})",
           R"({"query":"needle","since_ms":"20","until_ms":"10"})",
           R"({"query":"needle","cursor":"broken"})"}) {
    QVERIFY2(tool->execute(arguments, context).hasError(), arguments);
  }
  std::atomic_bool canceled{true};
  context.cancel = &canceled;
  const auto result = tool->execute(R"({"query":"needle"})", context);
  QVERIFY(result.hasError());
  QCOMPARE(result.error().code(), MaiErrorCode::Canceled);
}

void MaiChatHostToolsTest::messageQueriesRequireAnAvailableDatabase() {
  MaiToolContext context;
  RemoteIMApplication memoryApp(QStringLiteral("owner"), std::make_unique<FakeRemoteIMClient>());
  MaiToolRegistry memoryRegistry;
  registerMaiChatHostTools(memoryRegistry, memoryApp);
  QVERIFY(memoryRegistry.find("maichat_search_messages")->execute(
      R"({"query":"needle"})", context).hasError());
  QVERIFY(memoryRegistry.find("maichat_get_messages")->execute(
      R"({"peer_id":"alice"})", context).hasError());
  RemoteIMApplication unavailableApp(
      QStringLiteral("owner"), std::make_unique<FakeRemoteIMClient>(),
      std::make_unique<LocalMessageDatabase>(QString(QChar(0))));
  MaiToolRegistry unavailableRegistry;
  registerMaiChatHostTools(unavailableRegistry, unavailableApp);
  QVERIFY(unavailableRegistry.find("maichat_search_messages")->execute(
      R"({"query":"needle"})", context).hasError());
  QVERIFY(unavailableRegistry.find("maichat_get_messages")->execute(
      R"({"peer_id":"alice"})", context).hasError());
}

void MaiChatHostToolsTest::ordinarySearchIsAsynchronousAndRejectsChangedAccountOrCancellation() {
  QTemporaryDir directory;
  QVERIFY(directory.isValid());
  auto client = std::make_unique<FakeRemoteIMClient>();
  auto *fake = client.get();
  RemoteIMApplication app(QStringLiteral("owner"), std::move(client),
                          std::make_unique<LocalMessageDatabase>(directory.filePath(QStringLiteral("messages.sqlite"))));
  app.addContact(QStringLiteral("alice"), QStringLiteral("Alice"));
  fake->emitIncomingText(QStringLiteral("alice"), QStringLiteral("private needle"));
  QString account = QStringLiteral("account-a");
  app.setAccountProfileProvider([&] {
    return RemoteIMApplication::AccountProfile{true, account, QStringLiteral("owner"), {}, {}, {}};
  });
  LocalMessageSearchRequest request;
  request.query = QStringLiteral("needle");
  bool finished = false;
  LocalMessageSearchResult result;
  app.queryLocalMessages(request, {}, [&](const auto &searched) {
    result = searched;
    finished = true;
    QCOMPARE(QThread::currentThread(), app.thread());
  });
  QVERIFY(!finished);
  account = QStringLiteral("account-b");
  QTRY_VERIFY(finished);
  QCOMPARE(result.errorCode, QStringLiteral("identity_changed"));
  QVERIFY(result.matches.isEmpty());
  finished = false;
  auto canceled = std::make_shared<std::atomic_bool>(false);
  app.queryLocalMessages(request, canceled, [&](const auto &searched) {
    result = searched;
    finished = true;
  });
  canceled->store(true);
  QTRY_VERIFY(finished);
  QCOMPARE(result.errorCode, QStringLiteral("canceled"));
  QVERIFY(result.matches.isEmpty());
  finished = false;
  app.setAccountProfileProvider([] { return RemoteIMApplication::AccountProfile{}; });
  app.queryLocalMessages(request, {}, [&](const auto &searched) {
    result = searched;
    finished = true;
  });
  QTRY_VERIFY(finished);
  QCOMPARE(result.errorCode, QStringLiteral("account_unavailable"));
  QVERIFY(result.matches.isEmpty());
}

void MaiChatHostToolsTest::databaseQueryDoesNotWaitForTheGlobalThreadPool() {
  QTemporaryDir directory;
  QVERIFY(directory.isValid());
  auto client = std::make_unique<FakeRemoteIMClient>();
  auto *fake = client.get();
  RemoteIMApplication app(QStringLiteral("owner"), std::move(client),
                          std::make_unique<LocalMessageDatabase>(directory.filePath(QStringLiteral("messages.sqlite"))));
  app.addContact(QStringLiteral("alice"), QStringLiteral("Alice"));
  fake->emitIncomingText(QStringLiteral("alice"), QStringLiteral("needle"));
  auto* global = QThreadPool::globalInstance();
  const int originalThreads = global->maxThreadCount();
  QSemaphore entered;
  QSemaphore release;
  global->setMaxThreadCount(1);
  const auto restore = qScopeGuard([&] {
    release.release();
    global->waitForDone(1000);
    global->setMaxThreadCount(originalThreads);
  });
  global->start([&] {
    entered.release();
    release.tryAcquire(1, 2000);
  });
  QVERIFY(entered.tryAcquire(1, 1000));
  LocalMessageSearchRequest request;
  request.query = QStringLiteral("needle");
  bool finished = false;
  LocalMessageSearchResult result;
  QElapsedTimer elapsed;
  elapsed.start();
  app.queryLocalMessages(request, {}, [&](const auto &searched) {
    result = searched;
    finished = true;
  });
  QTRY_VERIFY_WITH_TIMEOUT(finished, 1000);
  qInfo() << "database query with global pool blocked ms:" << elapsed.elapsed();
  QVERIFY(result.errorCode.isEmpty());
  QCOMPARE(result.matches.size(), 1);
}

void MaiChatHostToolsTest::messageQueryChecksCancellationBeforePublishing() {
  QTemporaryDir directory;
  QVERIFY(directory.isValid());
  RemoteIMApplication app(QStringLiteral("owner"), std::make_unique<FakeRemoteIMClient>(),
                          std::make_unique<LocalMessageDatabase>(directory.filePath(QStringLiteral("messages.sqlite"))));
  MaiToolRegistry registry;
  registerMaiChatHostTools(registry, app);
  std::atomic_bool canceled{false};
  int profileReads = 0;
  app.setAccountProfileProvider([&] {
    // The second identity check runs in the completion callback, immediately
    // before publication and between cancellation-poll timer ticks.
    if (++profileReads == 2) canceled.store(true);
    return RemoteIMApplication::AccountProfile{true, QStringLiteral("account"), QStringLiteral("owner"), {}, {}, {}};
  });
  MaiToolContext context;
  context.cancel = &canceled;
  const auto result = registry.find("maichat_search_messages")->execute(R"({"query":"needle"})", context);
  QCOMPARE(profileReads, 2);
  QVERIFY(canceled.load());
  QVERIFY(result.hasError());
  QCOMPARE(result.error().code(), MaiErrorCode::Canceled);
}

void MaiChatHostToolsTest::blockedApplicationQueueDoesNotHoldMessageQueries_data() {
  QTest::addColumn<QString>("toolName");
  QTest::addColumn<QString>("termination");
  for (const QString &tool : {QStringLiteral("maichat_search_messages"), QStringLiteral("maichat_get_messages")}) {
    for (const QString &mode : {QStringLiteral("cancel"), QStringLiteral("deadline"), QStringLiteral("account")}) {
      const QByteArray name = (tool + QLatin1Char('-') + mode).toUtf8();
      QTest::newRow(name.constData()) << tool << mode;
    }
  }
}

void MaiChatHostToolsTest::blockedApplicationQueueDoesNotHoldMessageQueries() {
  QFETCH(QString, toolName);
  QFETCH(QString, termination);
  QTemporaryDir directory;
  QVERIFY(directory.isValid());
  RemoteIMApplication app(QStringLiteral("owner"), std::make_unique<FakeRemoteIMClient>(),
                          std::make_unique<LocalMessageDatabase>(directory.filePath(QStringLiteral("messages.sqlite"))));
  int profileReads = 0;
  auto profile = [&] {
    ++profileReads;
    return RemoteIMApplication::AccountProfile{true, QStringLiteral("account-a"), QStringLiteral("owner"), {}, {}, {}};
  };
  app.setAccountProfileProvider(profile);
  MaiToolRegistry registry;
  registerMaiChatHostTools(registry, app);
  auto *tool = registry.find(toolName.toStdString());
  QVERIFY(tool);
  const std::string arguments = toolName == QStringLiteral("maichat_search_messages")
      ? R"({"query":"needle"})" : R"({"peer_id":"alice"})";
  auto nativeCancel = std::make_unique<std::atomic_bool>(false);
  auto *cancelWhilePending = nativeCancel.get();
  QElapsedTimer elapsed;
  elapsed.start();
  auto future = std::async(std::launch::async,
      [tool, arguments, nativeCancel = std::move(nativeCancel)]() mutable {
        MaiToolContext context;
        context.cancel = nativeCancel.get();
        auto result = tool->execute(arguments, context);
        // Any remaining queued callback must survive the native turn's cancel
        // allocation being destroyed before the application's queue is released.
        nativeCancel.reset();
        return result;
      });
  // Blocking future waits deliberately keep this application's event queue
  // stalled; QTest::qWait/processEvents would invalidate the regression.
  const bool pendingBeforeTermination =
      future.wait_for(std::chrono::milliseconds(100)) == std::future_status::timeout;
  if (pendingBeforeTermination && termination == QStringLiteral("cancel"))
    cancelWhilePending->store(true);
  if (pendingBeforeTermination && termination == QStringLiteral("account")) {
    app.setAccountProfileProvider([&] {
      ++profileReads;
      return RemoteIMApplication::AccountProfile{true, QStringLiteral("account-b"), QStringLiteral("owner"), {}, {}, {}};
    });
  }
  const bool returnedWhileQueueBlocked = future.wait_for(std::chrono::milliseconds(
      termination == QStringLiteral("deadline") ? 5500 : 500)) == std::future_status::ready;
  if (!returnedWhileQueueBlocked) {
    // Release an old implementation's blocking invocation before asserting RED,
    // so a failed test never leaves a future destructor waiting on this UI thread.
    QElapsedTimer rescue;
    rescue.start();
    while (future.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready && rescue.elapsed() < 2500) {
      QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (future.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready)
      qFatal("synthetic message-query worker did not release after draining the application queue");
  }
  const auto result = future.get();
  QCoreApplication::sendPostedEvents();
  QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
  qInfo() << toolName << termination << "returned with UI stalled:" << returnedWhileQueueBlocked
          << "ms:" << elapsed.elapsed() << "database-path identity reads:" << profileReads;
  QVERIFY(pendingBeforeTermination);
  QVERIFY(returnedWhileQueueBlocked);
  QVERIFY(result.hasError());
  QCOMPARE(result.error().code(), termination == QStringLiteral("deadline") ? MaiErrorCode::Busy
                                                                           : MaiErrorCode::Canceled);
  QCOMPARE(profileReads, 0);
}

void MaiChatHostToolsTest::preCanceledMessageQueriesNeverEnterTheDatabasePath() {
  QTemporaryDir directory;
  QVERIFY(directory.isValid());
  RemoteIMApplication app(QStringLiteral("owner"), std::make_unique<FakeRemoteIMClient>(),
                          std::make_unique<LocalMessageDatabase>(directory.filePath(QStringLiteral("messages.sqlite"))));
  int profileReads = 0;
  app.setAccountProfileProvider([&] {
    ++profileReads;
    return RemoteIMApplication::AccountProfile{true, QStringLiteral("account"), QStringLiteral("owner"), {}, {}, {}};
  });
  MaiToolRegistry registry;
  registerMaiChatHostTools(registry, app);
  std::atomic_bool canceled{true};
  MaiToolContext context;
  context.cancel = &canceled;
  for (const auto &entry : {std::pair<const char*, const char*>{"maichat_search_messages", R"({"query":"needle"})"},
                            {"maichat_get_messages", R"({"peer_id":"alice"})"}}) {
    const auto result = registry.find(entry.first)->execute(entry.second, context);
    QVERIFY(result.hasError());
    QCOMPARE(result.error().code(), MaiErrorCode::Canceled);
  }
  QCOMPARE(profileReads, 0);
  LocalMessageSearchRequest request;
  request.query = QStringLiteral("needle");
  bool finished = false;
  LocalMessageSearchResult expired;
  app.queryLocalMessages(request, {}, [&](const auto &result) {
    expired = result;
    finished = true;
  }, QDeadlineTimer(0));
  QVERIFY(finished);
  QCOMPARE(expired.errorCode, QStringLiteral("query_timeout"));
  QCOMPARE(profileReads, 0);
}

// 发送和回复工具都必须经过审批门槛；测试同时确认只读工具不会意外获得写入能力。
void MaiChatHostToolsTest::sendsAndRepliesOnlyThroughApprovalGatedTools() {
  auto client = std::make_unique<FakeRemoteIMClient>();
  auto *fake = client.get();
  RemoteIMApplication app(QStringLiteral("owner"), std::move(client));
  app.addContact(QStringLiteral("alice"), QStringLiteral("Alice"));
  app.addContact(QStringLiteral("bob"), QStringLiteral("Bob"));
  fake->emitIncomingText(QStringLiteral("alice"),
                         QStringLiteral("Can you review this?"));
  const QString messageId =
      app.chatState().messagesWith(QStringLiteral("alice")).last().id;

  MaiToolRegistry registry;
  registerMaiChatHostTools(registry, app);
  MaiToolContext context;

  MaiTool *send = registry.find("maichat_send_text");
  QVERIFY(send->requiresApproval(R"({"peer_id":"alice","text":"Sure"})"));
  const auto sent =
      send->execute(R"({"peer_id":"alice","text":"Sure"})", context);
  QVERIFY(!sent.hasError());
  QCOMPARE(fake->lastTextPeerId(), QStringLiteral("alice"));
  QCOMPARE(fake->lastText(), QStringLiteral("Sure"));

  const QByteArray replyArguments =
      QJsonDocument(
          QJsonObject{
              {QStringLiteral("peer_id"), QStringLiteral("alice")},
              {QStringLiteral("message_id"), messageId},
              {QStringLiteral("text"), QStringLiteral("I will review it")},
          })
          .toJson(QJsonDocument::Compact);
  MaiTool *reply = registry.find("maichat_reply_message");
  QVERIFY(reply->requiresApproval(replyArguments.toStdString()));
  const auto replied = reply->execute(replyArguments.toStdString(), context);
  QVERIFY(!replied.hasError());
  const QList<RemoteIMMessage> messages =
      app.chatState().messagesWith(QStringLiteral("alice"));
  QVERIFY(messages.last().hasQuote);
  QCOMPARE(messages.last().text, QStringLiteral("I will review it"));

  MaiTool *broadcast = registry.find("maichat_broadcast_text");
  QVERIFY(broadcast->requiresPerCallApproval(
      R"({"peer_ids":["alice","bob"],"text":"Release is ready"})"));
  const auto broadcasted = broadcast->execute(
      R"({"peer_ids":["alice","bob","alice"],"text":"Release is ready"})",
      context);
  QVERIFY(!broadcasted.hasError());
  const QJsonObject broadcastResult = QJsonDocument::fromJson(
      QByteArray::fromStdString(broadcasted.output())).object();
  QCOMPARE(broadcastResult.value(QStringLiteral("recipient_count")).toInt(), 2);
  QCOMPARE(app.chatState().messagesWith(QStringLiteral("alice")).last().text,
           QStringLiteral("Release is ready"));
  QCOMPARE(app.chatState().messagesWith(QStringLiteral("bob")).last().text,
           QStringLiteral("Release is ready"));
}

QTEST_MAIN(MaiChatHostToolsTest)
#include "MaiChatHostToolsTest.moc"
