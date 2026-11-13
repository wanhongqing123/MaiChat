// 文件作用：桌面端IM 会话、消息与登录；FakeRemoteIMClient 负责外部接口请求与结果转换，本文件给出其声明。
// 类型关系：FakeRemoteIMClient final : public RemoteIMClient；审阅时可据此追踪父类、接口或回调契约。
// 对外边界：本文件给出 FakeRemoteIMClient 的类型和函数声明；参数、返回值和对象生命周期以此为准。
// 直接依赖：im/RemoteIMClient.h；实现与这些接口协作。
#pragma once

#include "im/RemoteIMClient.h"

class FakeRemoteIMClient final : public RemoteIMClient {
    Q_OBJECT

public:
    explicit FakeRemoteIMClient(QObject* parent = nullptr);
    RemoteDiagnostics::AccountTag currentAccount() const override { return {sdkAppId_, connectedUserId_}; }

    void connectToService(int sdkAppId, const QString& userId, const QString& userSig,
                          RemoteIMCompletion completion) override;
    void disconnectFromService(RemoteIMCompletion completion) override;
    void deleteContact(const QString& userId, RemoteIMCompletion completion) override;
    void requestFriend(const QString& userId, RemoteIMCompletion completion) override;
    void setSelfAvatar(const QString &avatarUrl, RemoteIMCompletion completion,
                       std::function<bool()> canCommit = {}) override;
    QString lastAvatarUrl() const { return lastAvatarUrl_; }
    void joinedGroups(RemoteIMGroupsCompletion completion) override;
    void createGroup(const QString& name, const QStringList& memberIds,
                     RemoteIMGroupCompletion completion) override;
    void sendGroupText(const QString& groupId, const QString& text,
                       RemoteIMSendCompletion completion) override;
    void groupHistory(const QString& groupId, int count, qint64 beforeSequence,
                      RemoteIMGroupHistoryCompletion completion) override;
    void markGroupRead(const QString& groupId, RemoteIMCompletion completion) override;
    QString lastMarkedGroupId() const { return lastMarkedGroupId_; }
    void deferNextGroupHistory();
    void finishDeferredGroupHistory(const QList<RemoteIMGroupMessage>& messages);
    int groupHistoryRequestCount() const { return groupHistoryRequestCount_; }
    void emitIncomingGroupText(const QString& groupId, const QString& senderId,
                               const QString& text, const QString& messageId);
    void removeGroupForTest(const QString& groupId);
    void sendText(const QString& peerId, const QString& text, RemoteIMSendCompletion completion) override;
    void sendMachineText(const QString& peerId, const QString& text, RemoteIMSendCompletion completion) override;
    void sendMachineTextWithQuote(const QString& peerId, const QString& text,
                                  const RemoteIMQuote& quote, bool hasQuote,
                                  RemoteIMSendCompletion completion) override;
    void sendMachineImageWithText(const QString& peerId, const QString& path, const QString& text,
                                  bool captionAbove, const RemoteIMQuote& quote, bool hasQuote,
                                  RemoteIMSendCompletion completion) override;
    void sendMachineFileWithText(const QString& peerId, const QString& path, const QString& fileName,
                                 const QString& text, bool captionAbove, const RemoteIMQuote& quote,
                                 bool hasQuote, RemoteIMSendCompletion completion) override;
    void sendMachineVideoWithText(const QString& peerId, const RemoteIMVideoPayload& video,
                                  const QString& text, bool captionAbove, const RemoteIMQuote& quote,
                                  bool hasQuote, RemoteIMSendCompletion completion) override;
    void sendApprovalDecision(const QString& peerId,
                              const QString& token,
                              RemoteIMApprovalAction action,
                              RemoteIMSendCompletion completion) override;
    void sendImage(const QString& peerId, const QString& localPath, RemoteIMSendCompletion completion) override;
    void sendVoice(const QString& peerId, const QString& localPath, int durationSeconds,
                   RemoteIMCompletion completion) override;
    void sendFile(const QString& peerId, const QString& localPath, const QString& fileName,
                  RemoteIMSendCompletion completion) override;
    void sendVideo(const QString& peerId, const RemoteIMVideoPayload& video,
                   RemoteIMSendCompletion completion) override;

    QString connectedUserId() const;
    QString lastDeletedContactId() const;
    QString lastFriendRequestId() const;
    QString lastTextPeerId() const;
    QString lastText() const;
    RemoteIMMessageOrigin lastOrigin() const { return lastOrigin_; }
    QString lastCaption() const { return lastCaption_; }
    bool lastCaptionAbove() const { return lastCaptionAbove_; }
    RemoteIMQuote lastQuote() const { return lastQuote_; }
    bool lastHasQuote() const { return lastHasQuote_; }
    QString lastGroupId() const { return lastGroupId_; }
    QString lastGroupText() const { return lastGroupText_; }
    QString lastApprovalToken() const;
    RemoteIMApprovalAction lastApprovalAction() const;
    QString lastImagePeerId() const;
    QString lastImagePath() const;
    QString lastFilePeerId() const;
    QString lastFilePath() const;
    QString lastFileName() const;
    QString lastVideoPeerId() const;
    RemoteIMVideoPayload lastVideo() const;
    void failNext(const QString& error);
    void deferNextSend();
    void finishDeferredSend();
    void emitIncomingText(const QString& fromUserId, const QString& text);
    void emitIncomingImage(const QString& fromUserId, const QString& localPath, int width, int height,
                           qint64 sizeBytes);

private:
    void complete(RemoteIMCompletion completion);
    void completeSend(RemoteIMSendCompletion completion);
    void recordMessageMetadata(RemoteIMMessageOrigin origin, const QString& caption, bool captionAbove, const RemoteIMQuote& quote, bool hasQuote);

    QString connectedUserId_;
    quint64 sdkAppId_ = 0;
    QString lastDeletedContactId_;
    QString lastFriendRequestId_;
    QString lastTextPeerId_;
    QString lastText_;
    RemoteIMMessageOrigin lastOrigin_ = RemoteIMMessageOrigin::Unknown;
    QString lastCaption_;
    bool lastCaptionAbove_ = false;
    RemoteIMQuote lastQuote_;
    bool lastHasQuote_ = false;
    QString lastGroupId_;
    QString lastGroupText_;
    QString lastMarkedGroupId_;
    QList<RemoteIMGroup> groups_;
    bool deferGroupHistory_ = false;
    int groupHistoryRequestCount_ = 0;
    RemoteIMGroupHistoryCompletion deferredGroupHistoryCompletion_;
    QString lastApprovalToken_;
    RemoteIMApprovalAction lastApprovalAction_ = RemoteIMApprovalAction::Reject;
    QString lastImagePeerId_;
    QString lastImagePath_;
    QString lastFilePeerId_;
    QString lastFilePath_;
    QString lastFileName_;
    QString lastVideoPeerId_;
    RemoteIMVideoPayload lastVideo_;
    QString nextError_;
    QString lastAvatarUrl_;
    bool deferNextSend_ = false;
    RemoteIMSendCompletion deferredSendCompletion_;
    int sentSequence_ = 0;
};
