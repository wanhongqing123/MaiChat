// 文件作用：桌面端 Agent 工具；MaiChatHostTools 负责 Agent 工具注册、调用与结果返回，本文件给出其实现。
// 主要入口：unreadSummary。
// 直接依赖：MaiTool.h、app/RemoteIMApplication.h、model/MessageQuote.h、model/MessageSearch.h；实现与这些接口协作。
#include "MaiChatTools/MaiChatHostTools.h"
#include "ui/DesktopRasterImage.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QMetaObject>
#include <QPointer>
#include <QRegularExpression>
#include <QSaveFile>
#include <QScopeGuard>
#include <QSet>
#include <QStandardPaths>
#include <QThread>
#include <QTimer>
#include <QUuid>

#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <utility>

#include "MaiTool.h"
#include "MaiChangePasswordTool.h"
#include "MaiAccountTools.h"
#include "app/RemoteIMApplication.h"
#include "model/MessageQuote.h"
#include "model/MessageSearch.h"

namespace {

enum class HostToolKind {
  ListContacts,
  RequestFriend,
  SetNickname,
  ListConversations,
  GetMessages,
  SearchMessages,
  GetUnreadSummary,
  SendText,
  SendMedia,
  ReplyMessage,
  BroadcastText,
  ListGroups,
  CreateGroup,
  GetGroupMessages,
  SendGroupText,
};

std::string toStdString(const QByteArray &bytes) {
  return std::string(bytes.constData(), static_cast<std::size_t>(bytes.size()));
}

MaiToolResult jsonResult(const QJsonObject &object) {
  return MaiToolResult::success(
      toStdString(QJsonDocument(object).toJson(QJsonDocument::Compact)));
}

MaiToolResult invalidInput(const QString &message) {
  return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                toStdString(message.toUtf8()));
}

bool parseObject(const std::string &arguments, QJsonObject *object,
                 QString *error) {
  QJsonParseError parseError;
  const QJsonDocument document = QJsonDocument::fromJson(
      QByteArray(arguments.data(), static_cast<int>(arguments.size())),
      &parseError);
  if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
    if (error)
      *error = QStringLiteral("arguments must be a JSON object");
    return false;
  }
  if (object)
    *object = document.object();
  return true;
}

int boundedLimit(const QJsonObject &arguments, int fallback = 50) {
  return std::clamp(arguments.value(QStringLiteral("limit")).toInt(fallback), 1,
                    200);
}

QString messageKind(const RemoteIMMessage &message) {
  if (message.hasImage)
    return QStringLiteral("image");
  if (message.hasVideo)
    return QStringLiteral("video");
  if (message.hasVoice)
    return QStringLiteral("voice");
  if (message.hasFile)
    return QStringLiteral("file");
  return QStringLiteral("text");
}

QJsonObject messageJson(const RemoteIMMessage &message, const QString &peerId) {
  QJsonObject object;
  object.insert(QStringLiteral("id"), message.id);
  object.insert(QStringLiteral("peer_id"), peerId);
  object.insert(QStringLiteral("direction"),
                message.direction == RemoteIMMessageDirection::Incoming
                    ? QStringLiteral("incoming")
                    : QStringLiteral("outgoing"));
  object.insert(QStringLiteral("sender_id"), message.fromUserId);
  object.insert(QStringLiteral("text"), message.text);
  object.insert(QStringLiteral("kind"), messageKind(message));
  object.insert(QStringLiteral("created_at_ms"),
                QString::number(message.createdAtMillis));
  return object;
}

bool hasContact(const ChatState &state, const QString &peerId) {
  const QList<RemoteIMContact> contacts = state.contacts();
  return std::any_of(
      contacts.cbegin(), contacts.cend(),
      [&](const RemoteIMContact &contact) { return contact.userId == peerId; });
}

template <typename Action>
MaiToolResult onApplicationThread(const QPointer<RemoteIMApplication> &app,
                                  Action &&action) {
  if (app.isNull()) {
    return MaiToolResult::failure(MaiErrorCode::NotConfigured,
                                  "the MaiChat host is no longer available");
  }
  if (QThread::currentThread() == app->thread())
    return action(*app);

  MaiToolResult result = MaiToolResult::failure(
      MaiErrorCode::Internal,
      "failed to dispatch the tool to the MaiChat application thread");
  const bool invoked = QMetaObject::invokeMethod(
      app.data(),
      [&] {
        if (!app.isNull())
          result = action(*app);
      },
      Qt::BlockingQueuedConnection);
  if (!invoked || app.isNull()) {
    return MaiToolResult::failure(
        MaiErrorCode::Internal,
        "failed to access the MaiChat application state");
  }
  return result;
}

LocalMessageSearchResult queryFailure(const QString &error, int limit) {
  LocalMessageSearchResult result;
  result.errorCode = error;
  result.limit = limit;
  return result;
}

// Query-only handoff. Native tool cancellation belongs to the synchronous
// caller; queued callbacks/SQL retain only this owned state and absolute budget.
// QCoreApplication outlives the host's tool registry and supplies a stable UI
// receiver, so a queued callback need not dereference a deleted host off-thread.
LocalMessageSearchResult awaitMessageQuery(
    const QPointer<RemoteIMApplication> &app, const LocalMessageSearchRequest &request,
    const std::shared_ptr<const std::atomic<quint64>> &generation, quint64 expectedGeneration,
    QDeadlineTimer deadline, const MaiToolContext &context) {
  struct PendingQuery {
    std::mutex mutex;
    std::condition_variable changed;
    std::atomic_bool abandoned{false};
    std::shared_ptr<std::atomic_bool> canceled = std::make_shared<std::atomic_bool>(false);
    bool finished = false;
    LocalMessageSearchResult result;
  };
  auto *dispatcher = QCoreApplication::instance();
  if (!dispatcher) return queryFailure(QStringLiteral("host_unavailable"), request.limit);
  if (context.isCanceled()) return queryFailure(QStringLiteral("canceled"), request.limit);
  auto pending = std::make_shared<PendingQuery>();
  auto finish = [pending](const LocalMessageSearchResult &result) {
    std::lock_guard<std::mutex> lock(pending->mutex);
    if (pending->abandoned.load()) return;
    pending->result = result;
    pending->finished = true;
    pending->changed.notify_all();
  };
  auto begin = [app, request, generation, expectedGeneration, deadline, pending, finish] {
    if (pending->abandoned.load()) return;
    if (pending->canceled->load() || deadline.hasExpired() || generation->load() != expectedGeneration) {
      finish(queryFailure(pending->canceled->load() ? QStringLiteral("canceled") :
          generation->load() != expectedGeneration ? QStringLiteral("identity_changed") :
                                                    QStringLiteral("query_timeout"), request.limit));
      return;
    }
    if (!app || app->thread() != QThread::currentThread()) {
      finish(queryFailure(QStringLiteral("host_unavailable"), request.limit));
      return;
    }
    app->queryLocalMessages(request, pending->canceled, finish, deadline);
  };
  const bool applicationThread = QThread::currentThread() == dispatcher->thread();
  if (applicationThread) begin();
  else if (!QMetaObject::invokeMethod(dispatcher, begin, Qt::QueuedConnection))
    return queryFailure(QStringLiteral("host_unavailable"), request.limit);

  for (;;) {
    const QString stopped = context.isCanceled() ? QStringLiteral("canceled") :
        generation->load() != expectedGeneration ? QStringLiteral("identity_changed") :
        deadline.hasExpired() ? QStringLiteral("query_timeout") : QString();
    if (!stopped.isEmpty()) {
      pending->abandoned.store(true);
      pending->canceled->store(true);
      return queryFailure(stopped, request.limit);
    }
    {
      std::unique_lock<std::mutex> lock(pending->mutex);
      if (pending->finished) return pending->result;
      if (!applicationThread) {
        pending->changed.wait_for(lock, std::chrono::milliseconds(10));
        continue;
      }
    }
    // Direct UI callers still process ordinary UI events; model workers use a
    // cancelable condition wait independent of whether this event queue runs.
    QEventLoop frame;
    QTimer tick;
    tick.setSingleShot(true);
    QObject::connect(&tick, &QTimer::timeout, &frame, &QEventLoop::quit);
    tick.start(10);
    frame.exec();
  }
}

class MaiChatHostTool final : public MaiTool {
public:
  MaiChatHostTool(HostToolKind kind, const char *name, const char *description,
                  const char *schema, RemoteIMApplication &app, bool approval)
      : mKind(kind), mName(name), mDescription(description), mSchema(schema),
        mApp(&app), mQueryGeneration(app.messageQueryGeneration()), mApproval(approval) {}

  std::string name() const override { return mName; }
  std::string description() const override { return mDescription; }
  std::string parametersSchema() const override { return mSchema; }
  bool requiresApproval(const std::string &) const override {
    return mApproval;
  }
  bool requiresPerCallApproval(const std::string &) const override {
    return mApproval;
  }

  MaiToolResult execute(const std::string &argumentsJson,
                        const MaiToolContext &context) override {
    QJsonObject arguments;
    QString parseError;
    if (!parseObject(argumentsJson, &arguments, &parseError))
      return invalidInput(parseError);
    if ((mKind == HostToolKind::CreateGroup || mKind == HostToolKind::SendGroupText) &&
        context.isCanceled())
      return MaiToolResult::failure(MaiErrorCode::Canceled, "group operation was canceled");
    if (mKind == HostToolKind::SendMedia)
      return sendMedia(arguments, context);
    if (mKind == HostToolKind::SearchMessages || mKind == HostToolKind::GetMessages)
      return queryMessages(arguments, context, mKind == HostToolKind::SearchMessages);
    return onApplicationThread(mApp, [&](RemoteIMApplication &app) {
      switch (mKind) {
      case HostToolKind::ListContacts:
        return listContacts(app, arguments);
      case HostToolKind::RequestFriend:
        return requestFriend(app, arguments);
      case HostToolKind::SetNickname:
        return setNickname(app, arguments);
      case HostToolKind::ListConversations:
        return listConversations(app, arguments);
      case HostToolKind::GetMessages:
        break;
      case HostToolKind::SearchMessages:
        break;
      case HostToolKind::GetUnreadSummary:
        return unreadSummary(app);
      case HostToolKind::SendText:
        return sendText(app, arguments);
      case HostToolKind::SendMedia:
        break;
      case HostToolKind::ReplyMessage:
        return replyMessage(app, arguments);
      case HostToolKind::BroadcastText:
        return broadcastText(app, arguments);
      case HostToolKind::ListGroups:
        return listGroups(app, arguments);
      case HostToolKind::CreateGroup:
        return createGroup(app, arguments);
      case HostToolKind::GetGroupMessages:
        return getGroupMessages(app, arguments);
      case HostToolKind::SendGroupText:
        return sendGroupText(app, arguments);
      }
      return MaiToolResult::failure(MaiErrorCode::Internal,
                                    "unknown MaiChat host tool");
    });
  }

private:
  MaiToolResult requestFriend(RemoteIMApplication &app,
                              const QJsonObject &arguments) const {
    const QString account = arguments.value(QStringLiteral("account")).toString().trimmed();
    static const QRegularExpression accountId(QStringLiteral("^[A-Za-z0-9_-]{1,128}$"));
    static const QRegularExpression phone(QStringLiteral("^(\\+86)?1[3-9][0-9]{9}$"));
    QString normalized = account;
    normalized.remove(QRegularExpression(QStringLiteral("[ -]")));
    if ((!accountId.match(account).hasMatch() && !phone.match(normalized).hasMatch()) ||
        account == app.chatState().ownerUserId())
      return invalidInput(QStringLiteral("account must be a phone or another MaiChat account ID"));
    if (!app.isConnected())
      return MaiToolResult::failure(MaiErrorCode::NotConfigured, "MaiChat IM is disconnected");
    app.requestFriend(account, account);
    return jsonResult(QJsonObject{{QStringLiteral("queued"), true},
                                  {QStringLiteral("account"), account}});
  }

  MaiToolResult setNickname(RemoteIMApplication &app,
                            const QJsonObject &arguments) const {
    const QJsonValue value = arguments.value(QStringLiteral("nickname"));
    if (!value.isString()) return invalidInput(QStringLiteral("nickname must be a string"));
    const QString nickname = value.toString().trimmed();
    if (nickname.toUcs4().size() > 32 || nickname.toUtf8().size() > 120)
      return invalidInput(QStringLiteral("nickname is too long"));
    if (!app.updateNickname(nickname, {}))
      return MaiToolResult::failure(MaiErrorCode::NotConfigured,
                                    "MaiChat account profile is unavailable");
    return jsonResult(QJsonObject{{QStringLiteral("queued"), true},
                                  {QStringLiteral("nickname"), nickname}});
  }

  // 校验联系人、媒体类型与本地文件，再通过宿主 IM 发送对应附件。
  // 只将真实发送结果回给 Agent，不能把文件存在等同于消息已交付。
  MaiToolResult sendMedia(const QJsonObject &arguments,
                          const MaiToolContext &context) const {
    const QString peerId = arguments.value(QStringLiteral("peer_id")).toString().trimmed();
    const QString requested = arguments.value(QStringLiteral("file_path")).toString().trimmed();
    const QString kind = arguments.value(QStringLiteral("type")).toString().trimmed();
    const QString caption = arguments.value(QStringLiteral("caption")).toString().trimmed();
    if (peerId.isEmpty() || requested.isEmpty() ||
        (kind != QStringLiteral("image") && kind != QStringLiteral("video") &&
         kind != QStringLiteral("audio") && kind != QStringLiteral("file"))) {
      return invalidInput(QStringLiteral("peer_id, file_path, and type=image|video|audio|file are required"));
    }
    const std::string path = context.resolvePath(toStdString(requested.toUtf8()));
    if (path.empty())
      return invalidInput(QStringLiteral("file_path is outside the accessible area"));
    const QFileInfo source(QString::fromUtf8(path.data(), static_cast<int>(path.size())));
    const qint64 limit = kind == QStringLiteral("image") ? 20LL * 1024 * 1024
                                                       : 1024LL * 1024 * 1024;
    if (!source.isFile() || !source.isReadable() || source.size() <= 0 ||
        source.size() > limit) {
      return invalidInput(QStringLiteral("media file is missing, unreadable, empty, or too large"));
    }
    if (kind == QStringLiteral("image")) {
      MaiImageReadOptions options{};
      options.max_encoded_bytes = 20LL * 1024 * 1024;
      options.max_output_width = options.max_output_height = 1;
      options.context = const_cast<MaiToolContext*>(&context);
      options.is_canceled = [](void* opaque) {
        return static_cast<const MaiToolContext*>(opaque)->isCanceled() ? 1 : 0;
      };
      if (desktopReadRasterFile(source.filePath(), options).image.isNull())
        return invalidInput(QStringLiteral("image file cannot be decoded"));
    }
    if (kind == QStringLiteral("video") &&
        source.suffix().compare(QStringLiteral("mp4"), Qt::CaseInsensitive) != 0 &&
        source.suffix().compare(QStringLiteral("mov"), Qt::CaseInsensitive) != 0) {
      return invalidInput(QStringLiteral("video messages require mp4 or mov"));
    }
    const auto validatePeer = onApplicationThread(mApp, [&](RemoteIMApplication &app) {
      return hasContact(app.chatState(), peerId)
                 ? jsonResult({{QStringLiteral("valid"), true}})
                 : invalidInput(QStringLiteral("peer_id is not a MaiChat contact"));
    });
    if (validatePeer.hasError()) return validatePeer;

    const QString directory = QDir(QStandardPaths::writableLocation(
        QStandardPaths::AppDataLocation)).filePath(QStringLiteral("AgentMedia"));
    if (!QDir().mkpath(directory))
      return MaiToolResult::failure(MaiErrorCode::Internal,
                                    "cannot create MaiChat media storage");
    const QString unique = QUuid::createUuid().toString(QUuid::WithoutBraces);
    // 通用文件用独立目录去重，保留 SDK 文件卡片上的真实 basename。
    const QString copiedDirectory = kind == QStringLiteral("file")
        ? QDir(directory).filePath(unique) : directory;
    if (!QDir().mkpath(copiedDirectory))
      return MaiToolResult::failure(MaiErrorCode::Internal, "cannot create attachment storage");
    const QString copiedPath = QDir(copiedDirectory).filePath(kind == QStringLiteral("file")
        ? source.fileName() : unique + QStringLiteral("-") + source.fileName());
    const auto removeEmptyDirectory = qScopeGuard([copiedDirectory, kind] {
      if (kind == QStringLiteral("file")) QDir().rmdir(copiedDirectory);
    });
    QFile input(source.filePath());
    QSaveFile output(copiedPath);
    if (!input.open(QIODevice::ReadOnly) || !output.open(QIODevice::WriteOnly))
      return MaiToolResult::failure(MaiErrorCode::Internal,
                                    "cannot read or save media file");
    while (!input.atEnd()) {
      if (context.isCanceled()) {
        output.cancelWriting();
        return MaiToolResult::failure(MaiErrorCode::Canceled,
                                      "media send was canceled before queueing");
      }
      const QByteArray chunk = input.read(1024 * 1024);
      if (chunk.isEmpty() || output.write(chunk) != chunk.size()) {
        output.cancelWriting();
        return MaiToolResult::failure(MaiErrorCode::Internal,
                                      "failed to copy media into MaiChat storage");
      }
    }
    if (!output.commit())
      return MaiToolResult::failure(MaiErrorCode::Internal,
                                    "failed to finalize media in MaiChat storage");
    const auto result = onApplicationThread(mApp, [&](RemoteIMApplication &app) {
      if (!hasContact(app.chatState(), peerId))
        return invalidInput(QStringLiteral("peer_id is no longer a MaiChat contact"));
      const bool queued = kind == QStringLiteral("image")
          ? app.sendImageTo(peerId, copiedPath, caption)
          : kind == QStringLiteral("video")
              ? app.sendVideoTo(peerId, copiedPath, caption)
              : app.sendFileTo(peerId, copiedPath, caption);
      if (!queued)
        return MaiToolResult::failure(MaiErrorCode::Internal,
                                      "MaiChat did not queue the media message");
      return jsonResult({{QStringLiteral("queued"), true},
                         {QStringLiteral("peer_id"), peerId},
                         {QStringLiteral("type"), kind},
                         {QStringLiteral("caption_attached"), !caption.isEmpty()},
                         {QStringLiteral("message_kind"),
                          kind == QStringLiteral("audio") ? QStringLiteral("file") : kind},
                         {QStringLiteral("path"), copiedPath}});
    });
    if (result.hasError()) QFile::remove(copiedPath);
    return result;
  }

  static MaiToolResult listContacts(RemoteIMApplication &app,
                                    const QJsonObject &arguments) {
    const QString query =
        arguments.value(QStringLiteral("query")).toString().trimmed();
    const int limit = boundedLimit(arguments);
    QJsonArray contacts;
    for (const RemoteIMContact &contact : app.chatState().contacts()) {
      if (!query.isEmpty() &&
          !contact.userId.contains(query, Qt::CaseInsensitive) &&
          !contact.displayName.contains(query, Qt::CaseInsensitive)) {
        continue;
      }
      QJsonObject item;
      item.insert(QStringLiteral("user_id"), contact.userId);
      item.insert(QStringLiteral("display_name"), contact.displayName);
      item.insert(QStringLiteral("group"), contact.groupName);
      contacts.append(item);
      if (contacts.size() >= limit)
        break;
    }
    return jsonResult({{QStringLiteral("contacts"), contacts},
                       {QStringLiteral("count"), contacts.size()}});
  }

  static MaiToolResult listGroups(RemoteIMApplication &app,
                                  const QJsonObject &arguments) {
    const QString query = arguments.value(QStringLiteral("query")).toString().trimmed();
    const int limit = boundedLimit(arguments);
    QJsonArray values;
    for (const RemoteIMGroup& group : app.groups()) {
      if (!query.isEmpty() && !group.name.contains(query, Qt::CaseInsensitive) &&
          !group.groupId.contains(query, Qt::CaseInsensitive)) continue;
      values.append(QJsonObject{{QStringLiteral("group_id"), group.groupId},
                                {QStringLiteral("name"), group.name},
                                {QStringLiteral("member_count"), group.memberCount},
                                {QStringLiteral("pending_activation"), group.pendingActivation},
                                {QStringLiteral("unread_count"), app.groupUnreadCount(group.groupId)}});
      if (values.size() >= limit) break;
    }
    return jsonResult({{QStringLiteral("groups"), values},
                       {QStringLiteral("count"), values.size()}});
  }

  static MaiToolResult createGroup(RemoteIMApplication &app,
                                   const QJsonObject &arguments) {
    for (const RemoteIMGroup& group : app.groups()) {
      if (!group.pendingActivation) continue;
      return jsonResult({{QStringLiteral("created"), false},
                         {QStringLiteral("group_id"), group.groupId},
                         {QStringLiteral("name"), group.name},
                         {QStringLiteral("member_count"), group.memberCount},
                         {QStringLiteral("pending_activation"), true},
                         {QStringLiteral("next_step"), QStringLiteral(
                             "Open this existing group and send one text message; do not create it again.")}});
    }
    const QString name = arguments.value(QStringLiteral("name")).toString().trimmed();
    const QJsonArray rawMembers = arguments.value(QStringLiteral("member_ids")).toArray();
    if (name.isEmpty() || name.size() > 30 || name.toUtf8().size() > 100 || rawMembers.size() < 1 ||
        rawMembers.size() > 19)
      return invalidInput(QStringLiteral("name must be <=100 UTF-8 bytes and member_ids must contain 1..19 friends"));
    QStringList members;
    for (const QJsonValue& value : rawMembers) {
      if (!value.isString()) return invalidInput(QStringLiteral("member_ids must contain strings"));
      const QString id = value.toString().trimmed();
      if (id.isEmpty() || id == app.chatState().ownerUserId() || members.contains(id) ||
          !hasContact(app.chatState(), id) || !app.isAcceptedFriend(id))
        return invalidInput(QStringLiteral("member_ids must be distinct accepted friends other than self"));
      members.append(id);
    }
    if (!app.isConnected())
      return MaiToolResult::failure(MaiErrorCode::NotConfigured, "MaiChat IM is disconnected");
    struct Result {
      bool done = false;
      bool ok = false;
      QString error;
      RemoteIMGroup group;
    };
    auto result = std::make_shared<Result>();
    QEventLoop loop;
    QPointer<QEventLoop> waiting(&loop);
    app.createGroup(name, members,
        [result, waiting](bool ok, const QString& error, const RemoteIMGroup& group) {
      result->done = true;
      result->ok = ok;
      result->error = error;
      result->group = group;
      if (waiting) waiting->quit();
    });
    if (!result->done) {
      QTimer timer;
      timer.setSingleShot(true);
      QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
      timer.start(30000);
      loop.exec();
    }
    if (!result->done)
      return MaiToolResult::failure(MaiErrorCode::Internal,
                                    "group creation status is unknown; list groups before retrying");
    if (!result->ok && !result->group.groupId.isEmpty())
      return jsonResult({{QStringLiteral("created"), true},
                         {QStringLiteral("group_id"), result->group.groupId},
                         {QStringLiteral("name"), result->group.name},
                         {QStringLiteral("member_count"), result->group.memberCount},
                         {QStringLiteral("pending_activation"), true},
                         {QStringLiteral("next_step"), QStringLiteral(
                             "Send one text message to this group; do not create it again.")},
                         {QStringLiteral("error"), result->error}});
    if (!result->ok)
      return MaiToolResult::failure(MaiErrorCode::Internal,
          toStdString((result->error.isEmpty() ? QStringLiteral("group creation failed") :
                       result->error).toUtf8()));
    return jsonResult({{QStringLiteral("created"), true},
                       {QStringLiteral("group_id"), result->group.groupId},
                       {QStringLiteral("name"), result->group.name},
                       {QStringLiteral("member_count"), result->group.memberCount},
                       {QStringLiteral("pending_activation"), result->group.pendingActivation}});
  }

  static MaiToolResult getGroupMessages(RemoteIMApplication &app,
                                        const QJsonObject &arguments) {
    const QString id = arguments.value(QStringLiteral("group_id")).toString().trimmed();
    if (id.isEmpty()) return invalidInput(QStringLiteral("group_id is required"));
    const auto groups = app.groups();
    if (std::none_of(groups.cbegin(), groups.cend(), [&id](const auto& group) {
          return group.groupId == id;
        })) return invalidInput(QStringLiteral("group_id is not a joined MaiChat group"));
    if (!app.hasLoadedGroupHistory(id)) {
      if (!app.isConnected())
        return MaiToolResult::failure(MaiErrorCode::NotConfigured,
                                      "MaiChat IM is disconnected and group history is not loaded");
      QEventLoop loop;
      bool finished = false;
      bool succeeded = false;
      QObject::connect(&app, &RemoteIMApplication::groupHistoryFinished, &loop,
                       [&](const QString& groupId, bool ok) {
        if (groupId != id) return;
        finished = true;
        succeeded = ok;
        loop.quit();
      });
      app.ensureGroupHistory(id);
      if (!app.hasLoadedGroupHistory(id) && !finished) {
        QTimer timeout;
        timeout.setSingleShot(true);
        QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
        timeout.start(30000);
        loop.exec();
      }
      if (!app.hasLoadedGroupHistory(id) || (finished && !succeeded))
        return MaiToolResult::failure(MaiErrorCode::Internal,
                                      "group history is still loading or failed; retry this read later");
    }
    const int limit = std::clamp(arguments.value(QStringLiteral("limit")).toInt(20), 1, 20);
    const auto all = app.groupMessages(id);
    QJsonArray values;
    for (int i = qMax(0, all.size() - limit); i < all.size(); ++i) {
      const auto& message = all.at(i);
      values.append(QJsonObject{{QStringLiteral("id"), message.id},
                                {QStringLiteral("sender_id"), message.senderId},
                                {QStringLiteral("direction"),
                                 message.fromSelf ? QStringLiteral("outgoing") : QStringLiteral("incoming")},
                                {QStringLiteral("text"), message.text},
                                {QStringLiteral("remote_id"),
                                 message.pending || message.failed ? QString() : message.id},
                                {QStringLiteral("created_at_ms"),
                                 static_cast<double>(message.createdAtMillis)}});
    }
    return jsonResult({{QStringLiteral("group_id"), id},
                       {QStringLiteral("messages"), values},
                       {QStringLiteral("count"), values.size()}});
  }

  static MaiToolResult sendGroupText(RemoteIMApplication &app,
                                     const QJsonObject &arguments) {
    const QString id = arguments.value(QStringLiteral("group_id")).toString().trimmed();
    const QString text = arguments.value(QStringLiteral("text")).toString().trimmed();
    if (id.isEmpty() || text.isEmpty())
      return invalidInput(QStringLiteral("group_id and text are required"));
    QSet<QString> beforeIds;
    for (const auto& message : app.groupMessages(id)) beforeIds.insert(message.id);
    if (!app.sendGroupText(id, text))
      return MaiToolResult::failure(MaiErrorCode::NotConfigured,
                                    "MaiChat did not queue the group message");
    auto findQueued = [&]() {
      const auto messages = app.groupMessages(id);
      for (int index = messages.size() - 1; index >= 0; --index) {
        const auto& message = messages.at(index);
        if (message.fromSelf && message.text == text && !beforeIds.contains(message.id))
          return message;
      }
      return RemoteIMGroupMessage{};
    };
    RemoteIMGroupMessage message = findQueued();
    const QString queuedId = message.id;
    if (message.pending) {
      QEventLoop loop;
      QTimer timeout;
      timeout.setSingleShot(true);
      QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
      QObject::connect(&app, &RemoteIMApplication::groupsChanged, &loop, [&] {
        message = findQueued();
        if (!message.pending) loop.quit();
      });
      timeout.start(30000);
      loop.exec();
      message = findQueued();
    }
    if (message.failed)
      return MaiToolResult::failure(MaiErrorCode::Internal, "group message delivery failed");
    return jsonResult({{QStringLiteral("queued"), true},
                       {QStringLiteral("sent"), !message.pending && !message.id.isEmpty()},
                       {QStringLiteral("group_id"), id},
                       {QStringLiteral("message_id"),
                        message.id.isEmpty() ? queuedId : message.id},
                       {QStringLiteral("remote_id"),
                        message.pending ? QString() : message.id}});
  }

  static MaiToolResult listConversations(RemoteIMApplication &app,
                                         const QJsonObject &arguments) {
    struct Conversation {
      RemoteIMContact contact;
      RemoteIMMessage latest;
      bool hasLatest = false;
    };
    QList<Conversation> conversations;
    for (const RemoteIMContact &contact : app.chatState().contacts()) {
      Conversation item;
      item.contact = contact;
      item.hasLatest =
          app.chatState().latestMessageWith(contact.userId, &item.latest);
      if (item.hasLatest)
        conversations.append(item);
    }
    std::sort(conversations.begin(), conversations.end(),
              [](const Conversation &left, const Conversation &right) {
                return left.latest.createdAtMillis >
                       right.latest.createdAtMillis;
              });
    QJsonArray result;
    const int limit = boundedLimit(arguments);
    for (const Conversation &conversation : conversations) {
      QJsonObject item;
      item.insert(QStringLiteral("peer_id"), conversation.contact.userId);
      item.insert(QStringLiteral("display_name"),
                  conversation.contact.displayName);
      item.insert(QStringLiteral("unread"),
                  app.chatState().unreadCount(conversation.contact.userId));
      item.insert(
          QStringLiteral("latest"),
          messageJson(conversation.latest, conversation.contact.userId));
      result.append(item);
      if (result.size() >= limit)
        break;
    }
    return jsonResult({{QStringLiteral("conversations"), result},
                       {QStringLiteral("count"), result.size()}});
  }

  MaiToolResult queryMessages(const QJsonObject &arguments,
                              const MaiToolContext &context, bool textSearch) const {
    const QDeadlineTimer deadline(5000);
    const quint64 generation = mQueryGeneration->load();
    if (context.isCanceled())
      return MaiToolResult::failure(MaiErrorCode::Canceled, "message query was canceled");
    static const QSet<QString> keys{QStringLiteral("query"), QStringLiteral("peer_id"),
        QStringLiteral("kind"), QStringLiteral("direction"), QStringLiteral("since_ms"),
        QStringLiteral("until_ms"), QStringLiteral("cursor"), QStringLiteral("limit")};
    for (auto it = arguments.begin(); it != arguments.end(); ++it) {
      if (!keys.contains(it.key()) || (!textSearch && it.key() == QStringLiteral("query")))
        return invalidInput(QStringLiteral("unsupported message query argument: ") + it.key());
      if (it.key() != QStringLiteral("limit") && it.key() != QStringLiteral("since_ms") &&
          it.key() != QStringLiteral("until_ms") && !it.value().isString())
        return invalidInput(it.key() + QStringLiteral(" must be a string"));
    }
    LocalMessageSearchRequest request;
    request.query = arguments.value(QStringLiteral("query")).toString().trimmed();
    request.peerId = arguments.value(QStringLiteral("peer_id")).toString().trimmed();
    request.kind = arguments.value(QStringLiteral("kind")).toString();
    request.direction = arguments.value(QStringLiteral("direction")).toString();
    request.cursor = arguments.value(QStringLiteral("cursor")).toString();
    if (textSearch && request.query.isEmpty()) return invalidInput(QStringLiteral("query is required"));
    if (!textSearch && request.peerId.isEmpty()) return invalidInput(QStringLiteral("peer_id is required"));
    if (arguments.contains(QStringLiteral("limit"))) {
      const auto value = arguments.value(QStringLiteral("limit"));
      const double limit = value.toDouble(-1);
      if (!value.isDouble() || !std::isfinite(limit) || std::floor(limit) != limit || limit < 1 || limit > 200)
        return invalidInput(QStringLiteral("limit must be an integer between 1 and 200"));
      request.limit = static_cast<int>(limit);
    }
    auto timestamp = [&](const QString &key, std::optional<qint64> &target) {
      if (!arguments.contains(key)) return true;
      const auto value = arguments.value(key);
      if (value.isString()) {
        bool ok = false;
        const qint64 millis = value.toString().toLongLong(&ok);
        if (!ok || millis < 0) return false;
        target = millis;
        return true;
      }
      // JSON doubles are exact integers only through 2^53-1. Larger epochs
      // must use the documented decimal string representation.
      const double millis = value.toDouble(-1);
      if (!value.isDouble() || !std::isfinite(millis) || millis < 0 ||
          millis > 9007199254740991.0 || std::floor(millis) != millis) return false;
      target = static_cast<qint64>(millis);
      return true;
    };
    if (!timestamp(QStringLiteral("since_ms"), request.sinceMillis) ||
        !timestamp(QStringLiteral("until_ms"), request.untilMillis))
      return invalidInput(QStringLiteral("time bounds must be nonnegative integer milliseconds or decimal strings"));
    // The same deadline starts before UI handoff and continues through the
    // ordinary API, dedicated pool and SQL. No queued callback owns context.
    const auto result = awaitMessageQuery(mApp, request, mQueryGeneration, generation, deadline, context);
    if (context.isCanceled())
      return MaiToolResult::failure(MaiErrorCode::Canceled, "message query was canceled");
    if (!result.errorCode.isEmpty()) {
      const MaiErrorCode code = result.errorCode == QStringLiteral("canceled") ||
          result.errorCode == QStringLiteral("identity_changed") ? MaiErrorCode::Canceled :
          result.errorCode.startsWith(QStringLiteral("invalid_")) ? MaiErrorCode::InvalidInput :
          result.errorCode == QStringLiteral("account_unavailable") ? MaiErrorCode::NotConfigured :
          result.errorCode == QStringLiteral("host_unavailable") ? MaiErrorCode::NotConfigured :
          result.errorCode == QStringLiteral("query_busy") || result.errorCode == QStringLiteral("query_timeout")
              ? MaiErrorCode::Busy :
                                                                    MaiErrorCode::Internal;
      return MaiToolResult::failure(code, toStdString(result.errorCode.toUtf8()));
    }
    QJsonArray matches;
    for (const auto &match : result.matches) {
      matches.append(QJsonObject{{QStringLiteral("id"), match.id},
          {QStringLiteral("peer_id"), match.peerId}, {QStringLiteral("sender_id"), match.senderId},
          {QStringLiteral("text"), match.text}, {QStringLiteral("kind"), match.kind},
          {QStringLiteral("direction"), match.direction},
          {QStringLiteral("created_at_ms"), QString::number(match.createdAtMillis)}});
    }
    QJsonObject output{{QStringLiteral("count"), matches.size()},
        {QStringLiteral("coverage"), result.coverage},
        {QStringLiteral("local_available"), result.localAvailable},
        {QStringLiteral("cloud_history_complete"), false},
        {QStringLiteral("no_matches"), result.noMatches}, {QStringLiteral("limit"), result.limit},
        {QStringLiteral("has_more"), result.hasMore}, {QStringLiteral("next_cursor"), result.nextCursor},
        {QStringLiteral("snapshot"), false},
        {QStringLiteral("order"), QStringLiteral("created_at_ms_desc_id_desc")}};
    if (textSearch) {
      output.insert(QStringLiteral("query"), request.query);
      output.insert(QStringLiteral("matches"), matches);
    } else {
      output.insert(QStringLiteral("peer_id"), request.peerId);
      output.insert(QStringLiteral("messages"), matches);
    }
    if (context.isCanceled())
      return MaiToolResult::failure(MaiErrorCode::Canceled, "message query was canceled");
    if (mQueryGeneration->load() != generation)
      return MaiToolResult::failure(MaiErrorCode::Canceled, "identity_changed");
    if (deadline.hasExpired())
      return MaiToolResult::failure(MaiErrorCode::Busy, "query_timeout");
    return jsonResult(output);
  }

  static MaiToolResult unreadSummary(RemoteIMApplication &app) {
    int total = 0;
    QJsonArray conversations;
    for (const RemoteIMContact &contact : app.chatState().contacts()) {
      const int unread = app.chatState().unreadCount(contact.userId);
      if (unread <= 0)
        continue;
      total += unread;
      conversations.append(
          QJsonObject{{QStringLiteral("peer_id"), contact.userId},
                      {QStringLiteral("display_name"), contact.displayName},
                      {QStringLiteral("unread"), unread}});
    }
    return jsonResult({{QStringLiteral("total_unread"), total},
                       {QStringLiteral("conversations"), conversations}});
  }

  static MaiToolResult sendText(RemoteIMApplication &app,
                                const QJsonObject &arguments) {
    const QString peerId =
        arguments.value(QStringLiteral("peer_id")).toString().trimmed();
    const QString text =
        arguments.value(QStringLiteral("text")).toString().trimmed();
    if (peerId.isEmpty() || text.isEmpty())
      return invalidInput(QStringLiteral("peer_id and text are required"));
    if (!hasContact(app.chatState(), peerId))
      return invalidInput(QStringLiteral("peer_id is not a MaiChat contact"));
    if (!app.sendTextTo(peerId, text))
      return MaiToolResult::failure(MaiErrorCode::Internal,
                                    "MaiChat did not queue the message");
    return jsonResult({{QStringLiteral("queued"), true},
                       {QStringLiteral("peer_id"), peerId}});
  }

  static MaiToolResult replyMessage(RemoteIMApplication &app,
                                    const QJsonObject &arguments) {
    const QString peerId =
        arguments.value(QStringLiteral("peer_id")).toString().trimmed();
    const QString messageId =
        arguments.value(QStringLiteral("message_id")).toString().trimmed();
    const QString text =
        arguments.value(QStringLiteral("text")).toString().trimmed();
    if (peerId.isEmpty() || messageId.isEmpty() || text.isEmpty())
      return invalidInput(
          QStringLiteral("peer_id, message_id, and text are required"));
    RemoteIMMessage quoted;
    bool found = false;
    app.chatState().forEachMessageWith(
        peerId, [&](const RemoteIMMessage &message) {
          if (!found && message.id == messageId) {
            quoted = message;
            found = true;
          }
        });
    if (!found)
      return invalidInput(
          QStringLiteral("message_id was not found for peer_id"));
    if (!app.sendTextTo(peerId, text, MessageQuote::quoteFor(quoted), true))
      return MaiToolResult::failure(MaiErrorCode::Internal,
                                    "MaiChat did not queue the reply");
    return jsonResult({{QStringLiteral("queued"), true},
                       {QStringLiteral("peer_id"), peerId},
                       {QStringLiteral("reply_to"), messageId}});
  }

  static MaiToolResult broadcastText(RemoteIMApplication &app,
                                     const QJsonObject &arguments) {
    const QJsonArray values = arguments.value(QStringLiteral("peer_ids")).toArray();
    const QString text =
        arguments.value(QStringLiteral("text")).toString().trimmed();
    if (values.isEmpty() || text.isEmpty())
      return invalidInput(QStringLiteral("peer_ids and text are required"));
    if (values.size() > 200)
      return invalidInput(QStringLiteral("peer_ids cannot contain more than 200 contacts"));

    QStringList recipients;
    QSet<QString> seen;
    for (const QJsonValue &value : values) {
      const QString peerId = value.toString().trimmed();
      if (peerId.isEmpty() || seen.contains(peerId))
        continue;
      if (!hasContact(app.chatState(), peerId))
        return invalidInput(
            QStringLiteral("peer_ids contains a non-contact: %1").arg(peerId));
      seen.insert(peerId);
      recipients.append(peerId);
    }
    if (recipients.isEmpty())
      return invalidInput(QStringLiteral("peer_ids must contain at least one contact"));

    const int queued = app.broadcastText(recipients, text);
    if (queued != recipients.size())
      return MaiToolResult::failure(MaiErrorCode::Internal,
                                    "MaiChat did not queue every broadcast message");
    QJsonArray peerIds;
    for (const QString &peerId : recipients)
      peerIds.append(peerId);
    return jsonResult({{QStringLiteral("queued"), true},
                       {QStringLiteral("recipient_count"), queued},
                       {QStringLiteral("peer_ids"), peerIds}});
  }

  HostToolKind mKind;
  std::string mName;
  std::string mDescription;
  std::string mSchema;
  QPointer<RemoteIMApplication> mApp;
  std::shared_ptr<const std::atomic<quint64>> mQueryGeneration;
  bool mApproval = false;
};

void addTool(MaiToolRegistry &registry, HostToolKind kind, const char *name,
             const char *description, const char *schema,
             RemoteIMApplication &app, bool approval = false) {
  registry.add(std::make_unique<MaiChatHostTool>(kind, name, description,
                                                 schema, app, approval));
}

} // namespace

// 桌面宿主工具按读写能力注册；有外部副作用的工具必须经过审批，名称与 Schema 供模型发现。
void registerMaiChatHostTools(MaiToolRegistry &registry,
                              RemoteIMApplication &app) {
  const QPointer<RemoteIMApplication> accountHost(&app);
  registry.add(
      makeMaiCurrentAccountTool([accountHost](const std::string &, const MaiToolContext &context) {
        return onApplicationThread(accountHost, [&](RemoteIMApplication &application) {
          if (context.isCanceled())
            return MaiToolResult::failure(MaiErrorCode::Canceled, "account query was canceled");
          const auto profile = application.currentAccountProfile();
          if (!profile.loggedIn) return jsonResult({{QStringLiteral("logged_in"), false}});
          auto nullable = [](const QString &value) {
            return value.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(value);
          };
          return jsonResult({{QStringLiteral("logged_in"), true},
                             {QStringLiteral("account_id"), profile.accountId},
                             {QStringLiteral("im_user_id"), profile.imUserId},
                             {QStringLiteral("nickname"), nullable(profile.nickname)},
                             {QStringLiteral("phone_masked"), nullable(profile.phoneMasked)},
                             {QStringLiteral("avatar_url"), nullable(profile.avatarUrl)}});
        });
      }));
  registry.add(makeMaiSetAvatarTool([accountHost](const std::string &args,
                                                  const MaiToolContext &context) {
    QJsonObject arguments;
    if (!parseObject(args, &arguments, nullptr))
      return invalidInput(QStringLiteral("invalid avatar request"));
    const QString path = arguments.value(QStringLiteral("path")).toString();
    return onApplicationThread(accountHost, [&](RemoteIMApplication &application) {
      struct Outcome {
        bool done = false;
        RemoteIMApplication::AvatarUpdateResult result;
      };
      auto outcome = std::make_shared<Outcome>();
      auto canceled = std::make_shared<std::atomic_bool>(context.isCanceled());
      QEventLoop loop;
      const QPointer<QEventLoop> waiting(&loop);
      application.updateAvatar(path, canceled, [outcome, waiting](const auto &result) {
        outcome->done = true;
        outcome->result = result;
        if (waiting) waiting->quit();
      });
      if (!outcome->done) {
        QTimer cancellation;
        QObject::connect(&cancellation, &QTimer::timeout, &loop, [&] {
          if (context.isCanceled()) canceled->store(true);
        });
        QObject::connect(&application, &QObject::destroyed, &loop, &QEventLoop::quit);
        QTimer timeout;
        timeout.setSingleShot(true);
        QObject::connect(&timeout, &QTimer::timeout, &loop, [&] {
          canceled->store(true);
          loop.quit();
        });
        cancellation.start(25);
        timeout.start(92000);
        loop.exec();
      }
      if (!outcome->done)
        return MaiToolResult::failure(
            MaiErrorCode::Internal,
            "avatar update timed out; query the current account before retrying");
      const auto &result = outcome->result;
      if (result.updated)
        return jsonResult(
            {{QStringLiteral("updated"), true}, {QStringLiteral("avatar_url"), result.avatarUrl}});
      const MaiErrorCode code =
          result.errorCode == QStringLiteral("canceled")            ? MaiErrorCode::Canceled
          : result.errorCode == QStringLiteral("invalid_image")     ? MaiErrorCode::InvalidInput
          : result.errorCode == QStringLiteral("not_authenticated") ? MaiErrorCode::NotConfigured
          : result.errorCode == QStringLiteral("busy")              ? MaiErrorCode::Busy
                                                                    : MaiErrorCode::Internal;
      return MaiToolResult::failure(code,
                                    "avatar update failed: " + result.errorCode.toStdString());
    });
  }));
  registry.add(makeMaiChangePasswordTool(
      [host = QPointer<RemoteIMApplication>(&app)](const std::string& args, const MaiToolContext& context) {
        if (context.isCanceled())
          return MaiToolResult::failure(MaiErrorCode::Canceled, "password form request was canceled");
        QJsonObject arguments;
        if (!parseObject(args, &arguments, nullptr))
          return MaiToolResult::failure(MaiErrorCode::InvalidInput, "invalid password form request");
        return onApplicationThread(host, [&](RemoteIMApplication& application) {
          if (context.isCanceled())
            return MaiToolResult::failure(MaiErrorCode::Canceled, "password form request was canceled");
          const QJsonObject output = application.requestPasswordChange(arguments);
          return output.isEmpty() ? MaiToolResult::failure(MaiErrorCode::NotConfigured,
              "secure password form is unavailable") : jsonResult(output);
        });
      }));
  addTool(
      registry, HostToolKind::ListContacts, "maichat_list_contacts",
      "List MaiChat contacts. Use query to filter by user ID or display name.",
      R"({"type":"object","properties":{"query":{"type":"string"},"limit":{"type":"integer","minimum":1,"maximum":200})"
          R"(}})",
      app);
  addTool(registry, HostToolKind::RequestFriend, "maichat_request_friend",
          "Send a MaiChat friend request by phone or account ID. "
          "Approval follows the Agent policy. "
          "The recipient must accept.",
          R"({"type":"object","properties":{"account":{"type":"string"}},"required":["account"]})",
          app, true);
  addTool(registry, HostToolKind::SetNickname, "maichat_set_nickname",
          "Set or clear the current MaiChat account nickname. Empty string clears it. "
          "Approval follows the Agent policy.",
          R"({"type":"object","properties":{"nickname":{"type":"string","maxLength":32}})"
          R"(,"required":["nickname"]})",
          app, true);
  addTool(
      registry, HostToolKind::ListConversations, "maichat_list_conversations",
      "List direct MaiChat contact conversations ordered by latest message and unread count. "
      "Use maichat_list_groups for group conversations.",
      R"({"type":"object","properties":{"limit":{"type":"integer","minimum":1,"maximum":200}}})",
      app);
  addTool(
      registry, HostToolKind::GetMessages, "maichat_get_messages",
      "Read locally persisted direct messages in one MaiChat conversation, newest time and ID first, "
      "including records outside the UI page. Inclusive time bounds; use next_cursor with unchanged filters. "
      "Each page reflects the current local store; local coverage does not establish complete cloud history.",
      R"({"type":"object","properties":{"peer_id":{"type":"string"},"limit":{"type":"integer","minimum":1,)"
          R"("maximum":200},"kind":{"type":"string","enum":["text","image","video","voice","file"]},)"
          R"("direction":{"type":"string","enum":["incoming","outgoing"]},)"
          R"("since_ms":{"oneOf":[{"type":"integer","minimum":0},{"type":"string"}]},)"
          R"("until_ms":{"oneOf":[{"type":"integer","minimum":0},{"type":"string"}]},)"
          R"("cursor":{"type":"string"}},"required":["peer_id"],"additionalProperties":false})",
      app);
  addTool(
      registry, HostToolKind::SearchMessages, "maichat_search_messages",
      "Search locally persisted direct-message text across all peers or one peer, including history outside the UI page. "
      "Case-insensitive text/token/fuzzy matching; inclusive time bounds; newest time and ID first. "
      "Use next_cursor with unchanged filters to continue; each page reflects the current local store. "
      "Local coverage does not establish complete cloud history.",
      R"({"type":"object","properties":{"query":{"type":"string"},"peer_id":{"type":"string"},)"
          R"("kind":{"type":"string","enum":["text","image","video","voice","file"]},)"
          R"("direction":{"type":"string","enum":["incoming","outgoing"]},)"
          R"("since_ms":{"oneOf":[{"type":"integer","minimum":0},{"type":"string"}]},)"
          R"("until_ms":{"oneOf":[{"type":"integer","minimum":0},{"type":"string"}]},)"
          R"("cursor":{"type":"string"},"limit":{"type":"integer","minimum":1,"maximum":200}},)"
          R"("required":["query"],"additionalProperties":false})",
      app);
  addTool(registry, HostToolKind::GetUnreadSummary,
          "maichat_get_unread_summary",
          "Summarize unread MaiChat messages by contact.",
          R"({"type":"object","properties":{}})", app);
  addTool(
      registry, HostToolKind::SendText, "maichat_send_text",
      "Queue a text message to a MaiChat contact. This changes external state "
      "and follows the Agent approval policy.",
      R"({"type":"object","properties":{"peer_id":{"type":"string"},"text":{"type":"string"}},"required":["peer_id",)"
          R"("text"]})",
      app, true);
  addTool(
      registry, HostToolKind::SendMedia, "maichat_send_media",
      "Send an accessible workspace image or video as a persistent MaiChat message "
      "bubble, or audio/generic files as file cards. Use type=file for PDF, documents, "
      "archives and other ordinary files. Optional caption is kept with the attachment. "
      "This changes external state; approval follows the Agent policy.",
      R"({"type":"object","properties":{"peer_id":{"type":"string"},"file_path":{"type":"string"},)"
          R"("type":{"type":"string","enum":["image","video","audio","file"]},"caption":{"type":"string"}},)"
          R"("required":["peer_id","file_path","type"],"additionalProperties":false})",
      app, true);
  addTool(
      registry, HostToolKind::ReplyMessage, "maichat_reply_message",
      "Queue a quoted text reply to a MaiChat message. This changes external "
      "state; approval follows the Agent policy.",
      R"({"type":"object","properties":{"peer_id":{"type":"string"},"message_id":{"type":"string"},)"
          R"("text":{"type":"string"}},"required":["peer_id","message_id","text"]})",
      app, true);
  addTool(
      registry, HostToolKind::BroadcastText, "maichat_broadcast_text",
      "Queue the same text message to multiple MaiChat contacts. The complete "
      "recipient list and text follow the Agent approval policy.",
      R"({"type":"object","properties":{"peer_ids":{"type":"array","items":{"type":"string"},"minItems":1,)"
          R"("maxItems":200},"text":{"type":"string"}},"required":["peer_ids","text"]})",
      app, true);
  addTool(registry, HostToolKind::ListGroups, "maichat_list_groups",
          "List joined MaiChat Work groups by name or group ID.",
          R"({"type":"object","properties":{"query":{"type":"string"},"limit":{"type":"integer","minimum":1,"maximum":200}},"additionalProperties":false})",
          app);
  addTool(registry, HostToolKind::CreateGroup, "maichat_create_group",
          "Create a MaiChat Work group with accepted friend IDs. "
          "Approval follows the Agent policy.",
          R"({"type":"object","properties":{"name":{"type":"string","minLength":1},"member_ids":{"type":"array","items":{"type":"string"},"minItems":1,"maxItems":19,"uniqueItems":true}},"required":["name","member_ids"],"additionalProperties":false})",
          app, true);
  addTool(registry, HostToolKind::GetGroupMessages, "maichat_get_group_messages",
          "Read up to 20 recent text messages from a joined MaiChat group.",
          R"({"type":"object","properties":{"group_id":{"type":"string"},"limit":{"type":"integer","minimum":1,"maximum":20}},"required":["group_id"],"additionalProperties":false})",
          app);
  addTool(registry, HostToolKind::SendGroupText, "maichat_send_group_text",
          "Send a text message to a joined MaiChat group. "
          "Approval follows the Agent policy.",
          R"({"type":"object","properties":{"group_id":{"type":"string"},"text":{"type":"string","minLength":1}},"required":["group_id","text"],"additionalProperties":false})",
          app, true);
}
