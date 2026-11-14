// 文件作用：把腾讯 IM SDK 的登录、消息、联系人和媒体能力适配给应用的 IM 协议。
// 阅读边界：SDK 回调在这里转换为应用事件，界面不直接依赖 SDK 类型。
// 把腾讯 IM SDK 回调转换为应用消息和附件事件。
import Foundation
import MaiChatCore
import UniformTypeIdentifiers

#if canImport(ImSDK_Plus)
import ImSDK_Plus

private final class IMSDKLifecycleState: @unchecked Sendable {
    var initializedSDKAppID: Int?
    var hasRegisteredIMSDKListener = false
}

private struct PreparedSDKMessage: @unchecked Sendable {
    let value: V2TIMMessage
    let account: String?
}

private struct SDKCallbackValue<Value>: @unchecked Sendable { let value: Value }

/// Mutable scan state is accessed only on sdkQueue, including SDK callback delivery.
private final class SDKGroupHistoryScan: @unchecked Sendable {
    var messages: [IncomingRemoteIMGroupText] = []
    var nextCursor: String?
    var seenCursors = Set<String>()
    var seenMessageIDs = Set<String>()
    var pageCount = 0
    var rawMessageCount = 0
}

private final class ApplicationBadgeSnapshot: @unchecked Sendable {
    private let lock = NSLock()
    private var value: UInt32 = 0

    func store(_ nextValue: UInt32) {
        lock.lock()
        value = nextValue
        lock.unlock()
    }

    func load() -> UInt32 {
        lock.lock()
        let currentValue = value
        lock.unlock()
        return currentValue
    }
}

private final class DiagnosticAccountSnapshot: @unchecked Sendable {
    private let lock = NSLock()
    private var value: String?
    func store(_ value: String?) {
        lock.lock()
        self.value = value
        lock.unlock()
    }
    func load() -> String? {
        lock.lock()
        defer { lock.unlock() }
        return value
    }
}

// SDK 工作队列不能读取 MainActor 的账号状态；同步镜像让排队中的旧头像写入立即失效。
private final class IMProfileAccountSnapshot: @unchecked Sendable {
    struct Session: Sendable, Equatable { let userID: String?; let generation: UUID }
    private let lock = NSLock()
    private var value = Session(userID: nil, generation: UUID())
    func begin(userID: String?) {
        lock.lock(); value = Session(userID: userID, generation: UUID()); lock.unlock()
    }
    func load() -> Session {
        lock.lock(); defer { lock.unlock() }
        return value
    }
}

// SDK 可能晚回调或不回调；取消、超时及 SDK 完成只能恢复同一 continuation 一次。
private final class AvatarSDKCompletion: @unchecked Sendable {
    private let lock = NSLock()
    private var continuation: CheckedContinuation<Void, Error>?
    init(_ continuation: CheckedContinuation<Void, Error>) { self.continuation = continuation }
    @discardableResult func finish(_ result: Result<Void, Error>) -> Bool {
        lock.lock()
        let pending = continuation
        continuation = nil
        lock.unlock()
        pending?.resume(with: result)
        return pending != nil
    }
}

/// SDK 网络 callback 可能迟到或缺失；只影响普通历史行，不阻塞已 ack 的内核 FIFO。
private final class IMSessionSDKCompletion: @unchecked Sendable {
    private let lock = NSLock()
    private var continuation: CheckedContinuation<RemoteIMSendReceipt, Error>?
    init(_ continuation: CheckedContinuation<RemoteIMSendReceipt, Error>) { self.continuation = continuation }
    var isPending: Bool { lock.lock(); defer { lock.unlock() }; return continuation != nil }
    func finish(_ result: Result<RemoteIMSendReceipt, Error>) {
        lock.lock(); let current = continuation; continuation = nil; lock.unlock()
        current?.resume(with: result)
    }
}

final class TencentIMClient:
    NSObject,
    RemoteIMClient,
    V2TIMSimpleMsgListener,
    V2TIMAdvancedMsgListener,
    V2TIMSDKListener,
    V2TIMGroupListener,
    V2TIMAPNSListener
{
    var onIncomingMetadata: ((IncomingRemoteIMMetadata) async -> RemoteIMMetadataReceipt?)?
    var incomingPersistenceFence: IMIngressPersistenceFence? { persistenceFence }
    private nonisolated let persistenceFence = IMIngressPersistenceFence()
    private nonisolated let rawIngressLock = NSLock()
    var onSessionIMInput: ((IMSessionIncomingPayload) -> Void)?
    var onIncomingText: ((IncomingRemoteIMText) -> Void)?
    var onIncomingActivity: ((IncomingRemoteIMActivity) -> Void)?
    var onIncomingVoice: ((IncomingRemoteIMVoice) -> Void)?
    var onIncomingImage: ((IncomingRemoteIMImage) -> Void)?
    var onIncomingFile: ((IncomingRemoteIMFile) -> Void)?
    var onIncomingVideo: ((IncomingRemoteIMVideo) -> Void)?
    var onIncomingGroupText: ((IncomingRemoteIMGroupText) -> Void)?
    var onGroupMembershipChanged: (() -> Void)?
    var onPresenceStatusChanged: (([String: RemoteIMPresenceStatus]) -> Void)?
    var onSelfProfileChanged: ((RemoteIMUserProfile) -> Void)?
    private nonisolated let lifecycleState = IMSDKLifecycleState()
    private nonisolated let applicationBadgeSnapshot = ApplicationBadgeSnapshot()
    private nonisolated let attachmentDownloads = MaiChatAttachmentDownloader()
    private nonisolated let profileAccountSnapshot = IMProfileAccountSnapshot()
    private nonisolated static let diagnosticAccountSnapshot = DiagnosticAccountSnapshot()
    private nonisolated static let sdkQueue = DispatchQueue(label: "MaiChat.IMSDKWork", qos: .userInitiated)
    private nonisolated static let incomingQueue = DispatchQueue(label: "MaiChat.IMIncoming", qos: .userInitiated)

    /// 使用服务端签发的 IM 凭证连接腾讯 IM SDK。
    /// 连接阶段分别处理初始化、登录和回调注册，失败向上层返回可定位错误。
    #if DEBUG && targetEnvironment(simulator)
    /// 自有 raw-envelope 验收仅设置原 SDK 身份镜像，不调用 init/login 或网络 API。
    func configureRawIngressIdentityForUITest(sdkAppID: Int, owner: String) throws {
        guard ProcessInfo.processInfo.arguments.contains("--im-raw-metadata-ui-test") else { throw CancellationError() }
        profileAccountSnapshot.begin(userID: owner)
        let tag = RemoteDiagnosticsProtocol.accountTag(sdkAppID: sdkAppID, ownerUserID: owner)
        Self.diagnosticAccountSnapshot.store(tag)
        persistenceFence.activate(accountTag: tag, generation: profileAccountSnapshot.load().generation)
    }
    #endif

    func connect(sdkAppID: Int, userID: String, userSig: String) async throws {
        attachmentDownloads.cancelAll()
        profileAccountSnapshot.begin(userID: userID)
        let diagnosticAccount = RemoteDiagnosticsProtocol.accountTag(sdkAppID: sdkAppID, ownerUserID: userID)
        Self.diagnosticAccountSnapshot.store(diagnosticAccount)
        persistenceFence.activate(accountTag: diagnosticAccount, generation: profileAccountSnapshot.load().generation)
        return try await withCheckedThrowingContinuation { (continuation: CheckedContinuation<Void, Error>) in
            let state = lifecycleState
            Self.sdkQueue.async {
                if state.initializedSDKAppID != sdkAppID {
                    let config = V2TIMSDKConfig()
                    let initialized = V2TIMManager.sharedInstance().initSDK(
                        Int32(sdkAppID),
                        config: config
                    )
                    guard initialized else {
                        Self.logSDK(level: .error, event: "sdk-init-failed")
                        continuation.resume(throwing: RemoteIMClientError.sdkInitializationFailed)
                        return
                    }
                    state.initializedSDKAppID = sdkAppID
                }
                if !state.hasRegisteredIMSDKListener {
                    V2TIMManager.sharedInstance().addIMSDKListener(listener: self)
                    state.hasRegisteredIMSDKListener = true
                }
                V2TIMManager.sharedInstance().setAPNSListener(apnsListener: self)
                V2TIMManager.sharedInstance().addSimpleMsgListener(listener: self)
                V2TIMManager.sharedInstance().addAdvancedMsgListener(listener: self)
                V2TIMManager.sharedInstance().addGroupListener(listener: self)
                Self.logSDK(level: .info, event: "login-start", fields: ["peer": Self.peerTag(userID)])
                V2TIMManager.sharedInstance().login(
                    userID: userID,
                    userSig: userSig,
                    succ: {
                        Self.logSDK(
                            level: .info,
                            event: "login-finished",
                            fields: ["result": "ok", "peer": Self.peerTag(userID), "account": diagnosticAccount]
                        )
                        let profile = V2TIMUserFullInfo()
                        profile.allowType = V2TIMFriendAllowType(rawValue: 1)!
                        V2TIMManager.sharedInstance().setSelfInfo(info: profile, succ: {},
                            fail: { code, _ in
                                Self.logSDK(level: .warning, event: "friend-verification-update-failed",
                                            fields: ["code": String(code)])
                            })
                        V2TIMManager.sharedInstance().getTotalUnreadMessageCount(
                            succ: { count in
                                Self.logSDK(
                                    level: .info,
                                    event: "sdk-unread-count-read",
                                    fields: ["unread_count": String(count)]
                                )
                            },
                            fail: { code, _ in
                                Self.logSDK(
                                    level: .warning,
                                    event: "sdk-unread-count-read-failed",
                                    fields: ["code": String(code)]
                                )
                            }
                        )
                        continuation.resume()
                    },
                    fail: { code, desc in
                        Self.logSDK(
                            level: .error,
                            event: "login-finished",
                            fields: [
                                "result": "failed",
                                "code": String(code),
                                "account": diagnosticAccount,
                                "peer": Self.peerTag(userID)
                            ]
                        )
                        continuation.resume(
                            throwing: RemoteIMClientError.operationFailed(
                                code: code,
                                description: desc ?? "login failed"
                            )
                        )
                    }
                )
            }
        }
    }

    func disconnect() async {
        attachmentDownloads.cancelAll()
        persistenceFence.invalidate()
        profileAccountSnapshot.begin(userID: nil)
        Self.diagnosticAccountSnapshot.store(nil)
        await withCheckedContinuation { continuation in
            let state = lifecycleState
            Self.sdkQueue.async {
                if state.hasRegisteredIMSDKListener {
                    V2TIMManager.sharedInstance().removeIMSDKListener(listener: self)
                    state.hasRegisteredIMSDKListener = false
                }
                V2TIMManager.sharedInstance().removeSimpleMsgListener(listener: self)
                V2TIMManager.sharedInstance().removeAdvancedMsgListener(listener: self)
                V2TIMManager.sharedInstance().removeGroupListener(listener: self)
                V2TIMManager.sharedInstance().logout(
                    succ: {
                        continuation.resume()
                    },
                    fail: { _, _ in
                        continuation.resume()
                    }
                )
            }
        }
    }

    func updateApplicationBadgeCount(_ count: Int) {
        let safeCount = UInt32(clamping: max(0, count))
        applicationBadgeSnapshot.store(safeCount)
    }

    @objc nonisolated func onSetAPPUnreadCount() -> UInt32 {
        let count = applicationBadgeSnapshot.load()
        Self.logSDK(
            level: .info,
            event: "sdk-app-badge-overridden",
            fields: ["badge_count": String(count)]
        )
        return count
    }

    @objc nonisolated func onConnecting() {
        Self.logSDK(level: .info, event: "sdk-connecting")
    }

    @objc nonisolated func onConnectSuccess() {
        Self.logSDK(level: .info, event: "sdk-connected")
    }

    @objc nonisolated func onConnectFailed(_ code: Int32, err: String?) {
        Self.logSDK(
            level: .error,
            event: "sdk-connect-failed",
            fields: ["code": String(code)]
        )
    }

    @objc nonisolated func onKickedOffline() {
        persistenceFence.invalidate()
        profileAccountSnapshot.begin(userID: nil)
        Self.diagnosticAccountSnapshot.store(nil)
        Self.logSDK(level: .error, event: "sdk-kicked-offline")
    }

    @objc nonisolated func onUserSigExpired() {
        persistenceFence.invalidate()
        profileAccountSnapshot.begin(userID: nil)
        Self.diagnosticAccountSnapshot.store(nil)
        Self.logSDK(level: .error, event: "sdk-user-sig-expired")
    }

    func refreshPresenceStatuses(userIDs: [String]) async throws -> [String: RemoteIMPresenceStatus] {
        let cleanedUserIDs = Self.cleanUserIDs(userIDs)
        guard !cleanedUserIDs.isEmpty else { return [:] }
        return try await withCheckedThrowingContinuation { continuation in
            Self.sdkQueue.async {
                V2TIMManager.sharedInstance().getUserStatus(
                    userIDList: cleanedUserIDs,
                    succ: { userStatusList in
                        continuation.resume(returning: Self.statusMap(from: userStatusList ?? []))
                    },
                    fail: { code, desc in
                        continuation.resume(
                            throwing: RemoteIMClientError.operationFailed(
                                code: code,
                                description: desc ?? "getUserStatus failed"
                            )
                        )
                    }
                )
            }
        }
    }

    func refreshUserProfiles(userIDs: [String]) async throws -> [RemoteIMUserProfile] {
        let cleanedUserIDs = Self.cleanUserIDs(userIDs)
        guard !cleanedUserIDs.isEmpty else { return [] }
        return try await withCheckedThrowingContinuation { continuation in
            Self.sdkQueue.async {
                V2TIMManager.sharedInstance().getUsersInfo(
                    cleanedUserIDs,
                    succ: { infoList in
                        continuation.resume(returning: (infoList ?? []).compactMap(Self.profile(from:)))
                    },
                    fail: { code, desc in
                        continuation.resume(
                            throwing: RemoteIMClientError.operationFailed(
                                code: code,
                                description: desc ?? "getUsersInfo failed"
                            )
                        )
                    }
                )
            }
        }
    }

    func setSelfAvatar(url: String, expectedUserID: String, request: MaiAvatarMutationRequest) async throws {
        try Task.checkCancellation()
        try request.checkActive()
        let session = profileAccountSnapshot.load()
        guard session.userID == expectedUserID else { throw MaiChatAvatarError.accountChanged }
        try await withCheckedThrowingContinuation { (continuation: CheckedContinuation<Void, Error>) in
            let completion = AvatarSDKCompletion(continuation)
            request.whenCanceled { completion.finish(.failure(CancellationError())) }
            DispatchQueue.global(qos: .utility).asyncAfter(deadline: .now() + 15) {
                if completion.finish(.failure(MaiChatAvatarError.sdkRejected)) { request.cancel() }
            }
            Self.sdkQueue.async {
                guard request.isActive, self.profileAccountSnapshot.load() == session,
                      V2TIMManager.sharedInstance().getLoginUser() == expectedUserID,
                      request.beginSDKMutation() else {
                    completion.finish(.failure(CancellationError()))
                    return
                }
                let info = V2TIMUserFullInfo()
                info.faceURL = url
                V2TIMManager.sharedInstance().setSelfInfo(info: info, succ: {
                    // SDK 已接受并确认属于旧登录的写入；停止不能把真实成功改写为取消。
                    request.confirmSDKMutation()
                    completion.finish(.success(()))
                }, fail: { code, description in
                    completion.finish(.failure(RemoteIMClientError.operationFailed(
                        code: code, description: description ?? "setSelfInfo failed"
                    )))
                })
            }
        }
    }

    @objc nonisolated func onSelfInfoUpdated(info: V2TIMUserFullInfo) {
        let session = profileAccountSnapshot.load()
        guard let profile = Self.profile(from: info), profile.userID == session.userID else { return }
        Task { @MainActor [weak self] in
            guard let self, self.profileAccountSnapshot.load() == session else { return }
            self.onSelfProfileChanged?(profile)
        }
    }

    func subscribePresenceStatuses(userIDs: [String]) async throws {
        let cleanedUserIDs = Self.cleanUserIDs(userIDs)
        guard !cleanedUserIDs.isEmpty else { return }
        try await withCheckedThrowingContinuation {
            (continuation: CheckedContinuation<Void, Error>) in
            Self.sdkQueue.async {
                V2TIMManager.sharedInstance().subscribeUserStatus(
                    userIDList: cleanedUserIDs,
                    succ: {
                        continuation.resume()
                    },
                    fail: { code, desc in
                        continuation.resume(
                            throwing: RemoteIMClientError.operationFailed(
                                code: code,
                                description: desc ?? "subscribeUserStatus failed"
                            )
                        )
                    }
                )
            }
        }
    }

    func sendText(
        to userID: String,
        text: String,
        origin: RemoteIMMessageOrigin,
        quote: RemoteIMQuote?
    ) async throws -> RemoteIMSendReceipt {
            guard
                let message = try await Self.makeMessage({ V2TIMManager.sharedInstance().createTextMessage(text: text) }
                )
            else {
                Self.logMessageCreateFailure(kind: "text", peerUserID: userID)
            throw RemoteIMClientError.operationFailed(code: -1, description: "create text message failed")
        }
        return try await send(
            message: message,
            to: userID,
            kind: "text",
            origin: origin,
            cloudMetadata: RemoteIMCloudMetadata(
                origin: origin,
                quote: quote
            ),
            metadata: ["content_bytes": String(text.lengthOfBytes(using: .utf8))],
            failureDescription: "send text failed"
        )
    }

    func sendSessionReply(_ payload: IMSessionOutgoingPayload, to userID: String,
                          permit: IMSessionDispatchPermit) async throws -> RemoteIMSendReceipt {
        guard userID == permit.delivery.peerID else { permit.acknowledge(); throw CancellationError() }
        let loginGeneration = profileAccountSnapshot.load().generation
        let kind: String
        let prepared: PreparedSDKMessage?
        switch payload {
        case .text(let text):
            kind = "text"
            prepared = try await Self.makeMessage { V2TIMManager.sharedInstance().createTextMessage(text: text) }
        case .image(let image, let text):
            kind = "image"
            prepared = try await Self.makeMessage {
                let message = V2TIMManager.sharedInstance().createImageMessage(imagePath: image.fileURL.path)
                return Self.attachCaption(text, to: message, firstElem: message?.imageElem)
            }
        case .video(let video, let text):
            kind = "video"
            prepared = try await Self.makeMessage {
                let message = V2TIMManager.sharedInstance().createVideoMessage(videoFilePath: video.fileURL.path,
                    type: video.fileType, duration: Int32(clamping: video.durationSeconds), snapshotPath: video.coverFileURL.path)
                return Self.attachCaption(text, to: message, firstElem: message?.videoElem)
            }
        case .file(let file, let text):
            kind = "file"
            prepared = try await Self.makeMessage {
                let message = V2TIMManager.sharedInstance().createFileMessage(filePath: file.fileURL.path, fileName: file.fileName)
                return Self.attachCaption(text, to: message, firstElem: message?.fileElem)
            }
        }
        guard let prepared else { permit.acknowledge(); throw RemoteIMClientError.operationFailed(
            code: -1, description: "无法创建会话回复") }
        return try await send(message: prepared, to: userID, kind: kind, origin: .machine,
            cloudMetadata: RemoteIMCloudMetadata(origin: .machine, interaction: permit.delivery.interaction,
                captionAbove: false),
            metadata: [:], failureDescription: "会话回复发送失败", sessionPermit: permit, expectedLoginGeneration: loginGeneration)
    }

    func createGroup(name: String, memberUserIDs: [String]) async throws -> RemoteIMGroup {
        let cleanName = name.trimmingCharacters(in: .whitespacesAndNewlines)
        let members = Self.cleanUserIDs(memberUserIDs)
        guard !cleanName.isEmpty, cleanName.lengthOfBytes(using: .utf8) <= 100,
              !members.isEmpty, members.count <= 19 else {
            throw RemoteIMClientError.operationFailed(
                code: -1, description: "群名称需在 100 字节内，且须选择 1 至 19 位成员"
            )
        }
        let account = Self.diagnosticAccountSnapshot.load()
        return try await withCheckedThrowingContinuation { continuation in
            Self.sdkQueue.async {
                guard account == Self.diagnosticAccountSnapshot.load() else {
                    continuation.resume(throwing: CancellationError()); return
                }
                let ownerID = V2TIMManager.sharedInstance().getLoginUser() ?? ""
                guard !ownerID.isEmpty, !members.contains(ownerID) else {
                    continuation.resume(throwing: RemoteIMClientError.operationFailed(
                        code: -1, description: "群成员不能包含当前账号"
                    ))
                    return
                }
                let info = V2TIMGroupInfo()
                info.groupType = GroupType_Work
                info.groupName = cleanName
                let sdkMembers = members.map { userID in
                    let member = V2TIMCreateGroupMemberInfo()
                    member.userID = userID
                    return member
                }
                V2TIMManager.sharedInstance().createGroup(
                    info: info, memberList: sdkMembers,
                    succ: { groupID in
                        guard account == Self.diagnosticAccountSnapshot.load() else {
                            continuation.resume(throwing: CancellationError()); return
                        }
                        guard let groupID = Self.nonEmpty(groupID) else {
                            continuation.resume(throwing: RemoteIMClientError.operationFailed(
                                code: -1, description: "createGroup returned no group ID"
                            ))
                            return
                        }
                        Self.sdkQueue.async {
                            guard account == Self.diagnosticAccountSnapshot.load() else {
                                continuation.resume(throwing: CancellationError()); return
                            }
                            guard let activation = V2TIMManager.sharedInstance().createCustomMessage(
                                data: RemoteIMGroupActivationCodec.encode()
                            ) else {
                                continuation.resume(throwing: RemoteIMClientError.groupActivationFailed(
                                    groupID: groupID, code: -1,
                                    description: "无法创建群激活消息"
                                ))
                                return
                            }
                            activation.isExcludedFromUnreadCount = true
                            activation.isExcludedFromLastMessage = true
                            V2TIMManager.sharedInstance().sendMessage(
                                message: activation,
                                receiver: nil,
                                groupID: groupID,
                                priority: V2TIMMessagePriority(rawValue: 0)!,
                                onlineUserOnly: false,
                                offlinePushInfo: nil,
                                progress: nil,
                                succ: {
                                    guard account == Self.diagnosticAccountSnapshot.load() else {
                                        continuation.resume(throwing: CancellationError()); return
                                    }
                                    continuation.resume(returning: RemoteIMGroup(
                                        groupID: groupID,
                                        name: cleanName,
                                        memberCount: members.count + 1
                                    ))
                                },
                                fail: { code, description in
                                    continuation.resume(throwing: RemoteIMClientError.groupActivationFailed(
                                        groupID: groupID, code: code,
                                        description: description ?? "群激活消息发送失败"
                                    ))
                                }
                            )
                        }
                    },
                    fail: { code, description in
                        continuation.resume(throwing: RemoteIMClientError.operationFailed(
                            code: code, description: description ?? "createGroup failed"
                        ))
                    }
                )
            }
        }
    }

    func joinedGroups() async throws -> [RemoteIMGroup] {
        let account = Self.diagnosticAccountSnapshot.load()
        return try await withCheckedThrowingContinuation { continuation in
            Self.sdkQueue.async {
                guard account == Self.diagnosticAccountSnapshot.load() else {
                    continuation.resume(throwing: CancellationError()); return
                }
                V2TIMManager.sharedInstance().getJoinedGroupList(succ: { groups in
                    guard account == Self.diagnosticAccountSnapshot.load() else {
                        continuation.resume(throwing: CancellationError()); return
                    }
                    continuation.resume(returning: (groups ?? []).compactMap { info in
                        guard let groupID = Self.nonEmpty(info.groupID),
                              info.groupType == GroupType_Work else { return nil }
                        return RemoteIMGroup(
                            groupID: groupID,
                            name: Self.nonEmpty(info.groupName) ?? groupID,
                            memberCount: Int(info.memberCount)
                        )
                    })
                }, fail: { code, description in
                    continuation.resume(throwing: RemoteIMClientError.operationFailed(
                        code: code, description: description ?? "getJoinedGroupList failed"
                    ))
                })
            }
        }
    }

    func sendGroupText(to groupID: String, text: String) async throws -> RemoteIMSendReceipt {
        guard let cleanGroupID = Self.nonEmpty(groupID),
              !text.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty else {
            throw RemoteIMClientError.operationFailed(code: -1, description: "群 ID 或消息不能为空")
        }
        guard let message = try await Self.makeMessage({
            V2TIMManager.sharedInstance().createTextMessage(text: text)
        }) else {
            Self.logMessageCreateFailure(kind: "group-text", peerUserID: cleanGroupID)
            throw RemoteIMClientError.operationFailed(code: -1, description: "create group text failed")
        }
        return try await send(
            message: message, to: cleanGroupID, groupID: cleanGroupID,
            kind: "group-text", origin: .human,
            metadata: ["content_bytes": String(text.lengthOfBytes(using: .utf8))],
            failureDescription: "send group text failed"
        )
    }

    func groupHistory(groupID: String, count: Int, beforeRemoteID: String?) async throws
        -> RemoteIMGroupHistoryPage {
        guard let cleanGroupID = Self.nonEmpty(groupID) else {
            throw RemoteIMClientError.operationFailed(code: -1, description: "群 ID 不能为空")
        }
        guard count > 0 else {
            return RemoteIMGroupHistoryPage(messages: [], nextCursor: nil, hasMore: false)
        }
        let requestedCount = min(count, 20)
        guard let account = Self.diagnosticAccountSnapshot.load() else {
            throw RemoteIMClientError.operationFailed(code: -1, description: "IM 尚未登录")
        }
        return try await withCheckedThrowingContinuation { continuation in
            Self.sdkQueue.async {
                guard account == Self.diagnosticAccountSnapshot.load() else {
                    continuation.resume(throwing: CancellationError()); return
                }
                let scan = SDKGroupHistoryScan()
                if let beforeRemoteID = Self.nonEmpty(beforeRemoteID) {
                    scan.seenCursors.insert(beforeRemoteID)
                }

                func finish(hasMore: Bool) {
                    continuation.resume(returning: RemoteIMGroupHistoryPage(
                        messages: Array(scan.messages.reversed()),
                        nextCursor: hasMore ? scan.nextCursor : nil,
                        hasMore: hasMore
                    ))
                }

                func fetch(before cursor: V2TIMMessage?) {
                    let remaining = requestedCount - scan.messages.count
                    guard remaining > 0, scan.pageCount < 10, scan.rawMessageCount < 200 else {
                        finish(hasMore: true)
                        return
                    }
                    let pageSize = min(20, remaining, 200 - scan.rawMessageCount)
                    scan.pageCount += 1
                    V2TIMManager.sharedInstance().getGroupHistoryMessageList(
                        groupID: cleanGroupID, count: Int32(pageSize), lastMsg: cursor,
                        succ: { messages in
                            let callback = SDKCallbackValue(value: messages ?? [])
                            Self.sdkQueue.async {
                                guard account == Self.diagnosticAccountSnapshot.load() else {
                                    continuation.resume(throwing: CancellationError()); return
                                }
                                let rawMessages = callback.value
                                guard !rawMessages.isEmpty else {
                                    finish(hasMore: false)
                                    return
                                }
                                guard let oldest = rawMessages.last,
                                      let oldestID = Self.nonEmpty(oldest.msgID),
                                      scan.seenCursors.insert(oldestID).inserted else {
                                    continuation.resume(throwing: RemoteIMClientError.operationFailed(
                                        code: -1, description: "群历史分页没有向前推进"
                                    ))
                                    return
                                }
                                scan.nextCursor = oldestID
                                scan.rawMessageCount += rawMessages.count
                                for raw in rawMessages {
                                    guard let incoming = Self.groupText(from: raw, groupID: cleanGroupID) else {
                                        continue
                                    }
                                    if let remoteID = incoming.remoteID,
                                       !scan.seenMessageIDs.insert(remoteID).inserted { continue }
                                    scan.messages.append(incoming)
                                }
                                if scan.messages.count >= requestedCount || scan.pageCount >= 10 ||
                                    scan.rawMessageCount >= 200 {
                                    finish(hasMore: true)
                                } else {
                                    fetch(before: oldest)
                                }
                            }
                        },
                        fail: { code, description in
                            Self.sdkQueue.async {
                                continuation.resume(throwing: RemoteIMClientError.operationFailed(
                                    code: code, description: description ?? "getGroupHistoryMessageList failed"
                                ))
                            }
                        }
                    )
                }
                guard let beforeRemoteID = Self.nonEmpty(beforeRemoteID) else {
                    fetch(before: nil)
                    return
                }
                V2TIMManager.sharedInstance().findMessages(messageIDList: [beforeRemoteID],
                    succ: { messages in
                        let callback = SDKCallbackValue(value: messages ?? [])
                        Self.sdkQueue.async {
                            guard account == Self.diagnosticAccountSnapshot.load() else {
                                continuation.resume(throwing: CancellationError()); return
                            }
                            guard let cursor = callback.value.first,
                                  cursor.groupID == cleanGroupID else {
                                continuation.resume(throwing: RemoteIMClientError.operationFailed(
                                    code: -1, description: "群历史游标已失效"
                                ))
                                return
                            }
                            fetch(before: cursor)
                        }
                    }, fail: { code, description in
                        Self.sdkQueue.async {
                            continuation.resume(throwing: RemoteIMClientError.operationFailed(
                                code: code, description: description ?? "find group history cursor failed"
                            ))
                        }
                    })
            }
        }
    }

    func sendActivity(to userID: String, signal: RemoteIMActivitySignal) async throws {
        try await sendActivity(to: userID, signal: signal, sessionPermit: nil)
    }

    func sendSessionActivity(_ signal: RemoteIMActivitySignal, to userID: String,
                             permit: IMSessionActivityPermit) async throws {
        guard userID == permit.activity.peerID, signal == permit.activity.signal else {
            permit.acknowledge(); throw CancellationError()
        }
        try await sendActivity(to: userID, signal: signal, sessionPermit: permit)
    }

    private func sendActivity(to userID: String, signal: RemoteIMActivitySignal,
                              sessionPermit: IMSessionActivityPermit?) async throws {
        let loginGeneration = profileAccountSnapshot.load().generation
        guard let message = try await Self.makeMessage({
            V2TIMManager.sharedInstance().createCustomMessage(
                data: RemoteIMActivityCodec.encode(signal)
            )
        }) else {
            Self.logMessageCreateFailure(kind: "activity", peerUserID: userID)
            throw RemoteIMClientError.operationFailed(
                code: -1,
                description: "create activity message failed"
            )
        }
        try await withCheckedThrowingContinuation {
            (continuation: CheckedContinuation<Void, Error>) in
            Self.sdkQueue.async {
                guard message.account == Self.diagnosticAccountSnapshot.load(),
                      loginGeneration == self.profileAccountSnapshot.load().generation else {
                    continuation.resume(throwing: CancellationError()); return
                }
                guard sessionPermit == nil || sessionPermit!.claim(accountTag: message.account) else {
                    sessionPermit?.acknowledge()
                    continuation.resume(throwing: CancellationError()); return
                }
                defer { sessionPermit?.acknowledge() }
                message.value.isExcludedFromUnreadCount = true
                message.value.isExcludedFromLastMessage = true
                V2TIMManager.sharedInstance().sendMessage(
                    message: message.value,
                    receiver: userID,
                    groupID: nil,
                    priority: V2TIMMessagePriority(rawValue: 0)!,
                    onlineUserOnly: true,
                    offlinePushInfo: nil,
                    progress: nil,
                    succ: { continuation.resume() },
                    fail: { code, desc in
                        continuation.resume(throwing: RemoteIMClientError.operationFailed(
                            code: code,
                            description: desc ?? "send activity failed"
                        ))
                    }
                )
            }
        }
    }

    func sendApprovalDecision(
        to userID: String,
        token: String,
        action: RemoteIMApprovalAction
    ) async throws -> RemoteIMSendReceipt {
        guard RemoteIMApprovalRequest.isValidToken(token),
              let message = try await Self.makeMessage({ V2TIMManager.sharedInstance().createTextMessage(
                text: action.decisionDisplayText
              ) })
        else {
            Self.logMessageCreateFailure(kind: "approval-decision", peerUserID: userID)
            throw RemoteIMClientError.operationFailed(
                code: -1,
                description: "create approval decision failed"
            )
        }
        return try await send(
            message: message,
            to: userID,
            kind: "approval-decision",
            origin: .human,
            cloudMetadata: RemoteIMCloudMetadata(
                origin: .human,
                interaction: .approvalDecision(token: token, action: action)
            ),
            metadata: ["action": action.rawValue],
            failureDescription: "send approval decision failed"
        )
    }

        func sendVoice(to userID: String, recording: RemoteIMVoiceRecording, origin: RemoteIMMessageOrigin) async throws
            -> RemoteIMSendReceipt
        {
            guard let message = try await Self.makeMessage({ V2TIMManager.sharedInstance().createSoundMessage(
            audioFilePath: recording.fileURL.path,
            duration: Int32(recording.durationSeconds)
        ) }) else {
            Self.logMessageCreateFailure(kind: "voice", peerUserID: userID)
            throw RemoteIMClientError.operationFailed(code: -1, description: "create voice message failed")
        }
        return try await send(
            message: message,
            to: userID,
            kind: "voice",
            origin: origin,
            metadata: ["duration_seconds": String(recording.durationSeconds)],
            failureDescription: "send voice failed"
        )
    }

        func sendImage(to userID: String, image: RemoteIMImageFile, caption: String?,
                       captionAbove: Bool, origin: RemoteIMMessageOrigin) async throws
            -> RemoteIMSendReceipt
        {
            guard
                let message = try await Self.makeMessage({
                    let message = V2TIMManager.sharedInstance().createImageMessage(
                        imagePath: image.fileURL.path)
                    return Self.attachCaption(caption, to: message, firstElem: message?.imageElem)
                })
            else {
                Self.logMessageCreateFailure(kind: "image", peerUserID: userID)
            throw RemoteIMClientError.operationFailed(code: -1, description: "create image message failed")
        }
        return try await send(
            message: message,
            to: userID,
            kind: "image",
            origin: origin,
            cloudMetadata: RemoteIMCloudMetadata(origin: origin,
                captionAbove: captionAbove &&
                    caption?.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty == false),
            metadata: [
                "width": image.width.map(String.init) ?? "unknown",
                "height": image.height.map(String.init) ?? "unknown",
                "bytes": image.sizeBytes.map(String.init) ?? "unknown",
            ],
            failureDescription: "send image failed"
        )
    }

        func sendVideo(to userID: String, video: RemoteIMVideoFile, caption: String?,
                       captionAbove: Bool, origin: RemoteIMMessageOrigin) async throws
            -> RemoteIMSendReceipt
        {
            guard let message = try await Self.makeMessage({
                let message = V2TIMManager.sharedInstance().createVideoMessage(
                    videoFilePath: video.fileURL.path,
                    type: video.fileType,
                    duration: Int32(clamping: video.durationSeconds),
                    snapshotPath: video.coverFileURL.path)
                return Self.attachCaption(caption, to: message, firstElem: message?.videoElem)
            }) else {
            Self.logMessageCreateFailure(kind: "video", peerUserID: userID)
            throw RemoteIMClientError.operationFailed(code: -1, description: "create video message failed")
        }
        return try await send(
            message: message,
            to: userID,
            kind: "video",
            origin: origin,
            cloudMetadata: RemoteIMCloudMetadata(origin: origin,
                captionAbove: captionAbove &&
                    caption?.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty == false),
            metadata: [
                "duration_seconds": String(video.durationSeconds),
                "width": String(video.width),
                "height": String(video.height),
                "bytes": String(video.sizeBytes),
            ],
            failureDescription: "send video failed"
        )
    }

        func sendFile(to userID: String, file: RemoteIMFile, caption: String?,
                      captionAbove: Bool, origin: RemoteIMMessageOrigin) async throws
            -> RemoteIMSendReceipt
        {
            guard let message = try await Self.makeMessage({
                let message = V2TIMManager.sharedInstance().createFileMessage(
                    filePath: file.fileURL.path, fileName: file.fileName)
                return Self.attachCaption(caption, to: message, firstElem: message?.fileElem)
            }) else {
            Self.logMessageCreateFailure(kind: "file", peerUserID: userID)
            throw RemoteIMClientError.operationFailed(code: -1, description: "create file message failed")
        }
        return try await send(
            message: message,
            to: userID,
            kind: "file",
            origin: origin,
            cloudMetadata: RemoteIMCloudMetadata(origin: origin,
                captionAbove: captionAbove &&
                    caption?.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty == false),
            metadata: [
                "bytes": file.sizeBytes.map(String.init) ?? "unknown",
                "mime_type": file.mimeType,
            ],
            failureDescription: "send file failed"
        )
    }

    func requestFriend(userID: String) async throws {
        try await withCheckedThrowingContinuation { (continuation: CheckedContinuation<Void, Error>) in
            Self.sdkQueue.async {
                let application = V2TIMFriendAddApplication()
                application.userID = userID
                application.addType = V2TIMFriendType(rawValue: 2)!
                application.addSource = "AddSource_Type_MaiChat"
                application.addWording = "请求添加为好友"
                V2TIMManager.sharedInstance().addFriend(application: application, succ: { result in
                    if let result, result.resultCode != 0 && result.resultCode != 30539 {
                        continuation.resume(throwing: RemoteIMClientError.operationFailed(
                            code: Int32(result.resultCode),
                            description: result.resultInfo ?? "add friend failed"
                        ))
                    } else {
                        continuation.resume()
                    }
                }, fail: { code, description in
                    if code == 30539 { continuation.resume() }
                    else {
                        continuation.resume(throwing: RemoteIMClientError.operationFailed(
                            code: code, description: description ?? "add friend failed"
                        ))
                    }
                })
            }
        }
    }

    func friendUserIDs() async throws -> [String] {
        try await withCheckedThrowingContinuation { continuation in
            Self.sdkQueue.async {
                V2TIMManager.sharedInstance().getFriendList(succ: { friends in
                    continuation.resume(returning: friends?.compactMap { $0.userID } ?? [])
                }, fail: { code, description in
                    continuation.resume(throwing: RemoteIMClientError.operationFailed(
                        code: code, description: description ?? "get friend list failed"
                    ))
                })
            }
        }
    }

    func pendingFriendRequests() async throws -> [RemoteIMFriendRequest] {
        try await withCheckedThrowingContinuation { continuation in
            Self.sdkQueue.async {
                V2TIMManager.sharedInstance().getFriendApplicationList(succ: { result in
                    let requests = result?.applicationList.compactMap { raw -> RemoteIMFriendRequest? in
                        guard let application = raw as? V2TIMFriendApplication,
                              application.type.rawValue == 1,
                              let userID = application.userID, !userID.isEmpty else { return nil }
                        return RemoteIMFriendRequest(
                            userID: userID,
                            displayName: application.nickName?.isEmpty == false
                                ? application.nickName! : userID,
                            avatarURL: application.faceUrl,
                            message: application.addWording ?? ""
                        )
                    } ?? []
                    continuation.resume(returning: requests)
                }, fail: { code, description in
                    continuation.resume(throwing: RemoteIMClientError.operationFailed(
                        code: code, description: description ?? "get friend requests failed"
                    ))
                })
            }
        }
    }

    func acceptFriendRequest(userID: String) async throws {
        try await withCheckedThrowingContinuation { (continuation: CheckedContinuation<Void, Error>) in
            Self.sdkQueue.async {
                V2TIMManager.sharedInstance().getFriendApplicationList(succ: { list in
                    guard let application = list?.applicationList.first(where: { raw in
                        guard let request = raw as? V2TIMFriendApplication else { return false }
                        return request.type.rawValue == 1 && request.userID == userID
                    }) as? V2TIMFriendApplication else {
                        continuation.resume(throwing: RemoteIMClientError.operationFailed(
                            code: -1, description: "Friend request no longer exists"
                        ))
                        return
                    }
                    V2TIMManager.sharedInstance().acceptFriendApplication(
                        application: application, acceptType: V2TIMFriendAcceptType(rawValue: 1)!,
                        succ: { result in
                            if let result, result.resultCode != 0 {
                                continuation.resume(throwing: RemoteIMClientError.operationFailed(
                                    code: Int32(result.resultCode),
                                    description: result.resultInfo ?? "accept friend failed"
                                ))
                            } else { continuation.resume() }
                        }, fail: { code, description in
                            continuation.resume(throwing: RemoteIMClientError.operationFailed(
                                code: code, description: description ?? "accept friend failed"
                            ))
                        }
                    )
                }, fail: { code, description in
                    continuation.resume(throwing: RemoteIMClientError.operationFailed(
                        code: code, description: description ?? "get friend requests failed"
                    ))
                })
            }
        }
    }

    func refuseFriendRequest(userID: String) async throws {
        try await withCheckedThrowingContinuation { (continuation: CheckedContinuation<Void, Error>) in
            Self.sdkQueue.async {
                V2TIMManager.sharedInstance().getFriendApplicationList(succ: { list in
                    guard let application = list?.applicationList.first(where: { raw in
                        guard let request = raw as? V2TIMFriendApplication else { return false }
                        return request.type.rawValue == 1 && request.userID == userID
                    }) as? V2TIMFriendApplication else {
                        continuation.resume(throwing: RemoteIMClientError.operationFailed(
                            code: -1, description: "Friend request no longer exists"
                        ))
                        return
                    }
                    V2TIMManager.sharedInstance().refuseFriendApplication(
                        application: application, succ: { result in
                            if let result, result.resultCode != 0 {
                                continuation.resume(throwing: RemoteIMClientError.operationFailed(
                                    code: Int32(result.resultCode),
                                    description: result.resultInfo ?? "refuse friend failed"
                                ))
                            } else { continuation.resume() }
                        }, fail: { code, description in
                            continuation.resume(throwing: RemoteIMClientError.operationFailed(
                                code: code, description: description ?? "refuse friend failed"
                            ))
                        }
                    )
                }, fail: { code, description in
                    continuation.resume(throwing: RemoteIMClientError.operationFailed(
                        code: code, description: description ?? "get friend requests failed"
                    ))
                })
            }
        }
    }

    func blockUser(userID: String) async throws {
        try await withCheckedThrowingContinuation { (continuation: CheckedContinuation<Void, Error>) in
            Self.sdkQueue.async {
                V2TIMManager.sharedInstance().addToBlackList(userIDList: [userID], succ: { results in
                    if let failure = results?.first(where: { $0.resultCode != 0 }) {
                        continuation.resume(throwing: RemoteIMClientError.operationFailed(
                            code: Int32(failure.resultCode),
                            description: failure.resultInfo ?? "block user failed"
                        ))
                    } else { continuation.resume() }
                }, fail: { code, description in
                    continuation.resume(throwing: RemoteIMClientError.operationFailed(
                        code: code, description: description ?? "block user failed"
                    ))
                })
            }
        }
    }

    func unblockUser(userID: String) async throws {
        try await withCheckedThrowingContinuation { (continuation: CheckedContinuation<Void, Error>) in
            Self.sdkQueue.async {
                V2TIMManager.sharedInstance().deleteFromBlackList(userIDList: [userID],
                    succ: { results in
                        if let failure = results?.first(where: { $0.resultCode != 0 }) {
                            continuation.resume(throwing: RemoteIMClientError.operationFailed(
                                code: Int32(failure.resultCode),
                                description: failure.resultInfo ?? "unblock user failed"
                            ))
                        } else { continuation.resume() }
                    }, fail: { code, description in
                        continuation.resume(throwing: RemoteIMClientError.operationFailed(
                            code: code, description: description ?? "unblock user failed"
                        ))
                    })
            }
        }
    }

    func blockedUserIDs() async throws -> [String] {
        try await withCheckedThrowingContinuation { continuation in
            Self.sdkQueue.async {
                V2TIMManager.sharedInstance().getBlackList(succ: { users in
                    continuation.resume(returning: users?.compactMap { $0.userID } ?? [])
                }, fail: { code, description in
                    continuation.resume(throwing: RemoteIMClientError.operationFailed(
                        code: code, description: description ?? "get blocked users failed"
                    ))
                })
            }
        }
    }

    func deleteContact(userID: String) async throws {
        try await withCheckedThrowingContinuation { (continuation: CheckedContinuation<Void, Error>) in
            V2TIMManager.sharedInstance().deleteFromFriendList(
                userIDList: [userID],
                deleteType: V2TIMFriendType(rawValue: 2)!,
                succ: { results in
                    if let failure = results?.first(where: { $0.resultCode != 0 }) {
                        continuation.resume(
                            throwing: RemoteIMClientError.operationFailed(
                                code: Int32(failure.resultCode),
                                description: failure.resultInfo ?? "delete friend failed"
                            )
                        )
                    } else {
                        continuation.resume()
                    }
                },
                fail: { code, desc in
                    continuation.resume(
                        throwing: RemoteIMClientError.operationFailed(
                            code: code,
                            description: desc ?? "delete friend failed"
                        )
                    )
                }
            )
        }
    }

    func clearHistory(userID: String) async throws {
        try await withCheckedThrowingContinuation { (continuation: CheckedContinuation<Void, Error>) in
            V2TIMManager.sharedInstance().clearC2CHistoryMessage(
                userID: userID,
                succ: {
                    continuation.resume()
                },
                fail: { code, desc in
                    continuation.resume(
                        throwing: RemoteIMClientError.operationFailed(
                            code: code,
                            description: desc ?? "clear history failed"
                        )
                    )
                }
            )
        }
    }

    private nonisolated static func makeMessage(
        _ build: @escaping @Sendable () -> V2TIMMessage?
    ) async throws -> PreparedSDKMessage? {
        let account = diagnosticAccountSnapshot.load()
        try Task.checkCancellation()
        return try await withCheckedThrowingContinuation { continuation in
            sdkQueue.async {
                guard account == diagnosticAccountSnapshot.load() else {
                    continuation.resume(throwing: CancellationError()); return
                }
                let value = autoreleasepool(invoking: build)
                continuation.resume(returning: value.map { PreparedSDKMessage(value: $0, account: account) })
            }
        }
    }

    /// 在 SDK 创建的媒体消息后追加文本元素；两者共用同一个云端消息 ID。
    private nonisolated static func attachCaption(
        _ caption: String?, to message: V2TIMMessage?, firstElem: V2TIMElem?
    ) -> V2TIMMessage? {
        guard let cleanCaption = caption?.trimmingCharacters(in: .whitespacesAndNewlines),
              !cleanCaption.isEmpty else { return message }
        guard let message, let firstElem else { return nil }
        let text = V2TIMTextElem()
        text.text = cleanCaption
        firstElem.appendElem(elem: text)
        return message
    }

    /// 按消息类型提交腾讯 IM 发送请求并回传稳定结果。
    /// 本地待发送状态在服务端确认前保留，失败不能伪装成已送达。
    private func send(
        message: PreparedSDKMessage,
        to userID: String,
        groupID: String? = nil,
        kind: String,
        origin: RemoteIMMessageOrigin,
        cloudMetadata: RemoteIMCloudMetadata? = nil,
        metadata: [String: String],
        failureDescription: String,
        sessionPermit: IMSessionDispatchPermit? = nil,
        expectedLoginGeneration: UUID? = nil
    ) async throws -> RemoteIMSendReceipt {
        let startedAt = ProcessInfo.processInfo.systemUptime
        var startFields = metadata
        startFields["operation"] = DiagnosticLogPrivacy.stableTag(
            UUID().uuidString,
            prefix: "op"
        )
        startFields["kind"] = kind
        startFields["peer"] = Self.peerTag(userID)
        startFields["account"] = message.account ?? "unbound"
        Self.logSDK(level: .info, event: "message-send-start", fields: startFields)
        let fieldsSnapshot = startFields
        return try await withCheckedThrowingContinuation { continuation in
            let completion = IMSessionSDKCompletion(continuation)
            if sessionPermit != nil {
                DispatchQueue.global(qos: .utility).asyncAfter(deadline: .now() + 30) {
                    completion.finish(.failure(RemoteIMClientError.operationFailed(code: -1,
                        description: "回复送达状态未知，未自动重发")))
                }
            }
            Self.sdkQueue.async {
                guard completion.isPending, message.account == Self.diagnosticAccountSnapshot.load(),
                      expectedLoginGeneration == nil || expectedLoginGeneration == self.profileAccountSnapshot.load().generation else {
                    sessionPermit?.acknowledge()
                    completion.finish(.failure(CancellationError())); return
                }
                // 最后一次 claim 与真实 SDK send 之间无 await/准备工作；撤销后的迟到媒体只能放弃。
                guard sessionPermit == nil || sessionPermit!.claim(accountTag: message.account) else {
                    sessionPermit?.acknowledge()
                    completion.finish(.failure(CancellationError())); return
                }
                defer { sessionPermit?.acknowledge() }
                    message.value.cloudCustomData = RemoteIMCloudMetadataCodec.encode(
                        cloudMetadata ?? RemoteIMCloudMetadata(origin: origin))
                    V2TIMManager.sharedInstance().sendMessage(
                    message: message.value,
                    receiver: groupID == nil ? userID : nil,
                    groupID: groupID,
                    priority: V2TIMMessagePriority(rawValue: 0)!,
                    onlineUserOnly: false,
                    offlinePushInfo: nil,
                    progress: nil,
                    succ: {
                        let receipt = Self.receipt(for: message.value)
                        var fields = fieldsSnapshot
                        fields["result"] = "ok"
                        fields["duration_ms"] = Self.elapsedMilliseconds(since: startedAt)
                        fields["message"] = Self.messageTag(receipt.remoteID)
                        Self.logSDK(level: .info, event: "message-send-finished", fields: fields)
                        completion.finish(.success(receipt))
                    },
                    fail: { code, desc in
                        var fields = fieldsSnapshot
                        fields["result"] = "failed"
                        fields["code"] = String(code)
                        fields["duration_ms"] = Self.elapsedMilliseconds(since: startedAt)
                        Self.logSDK(level: .error, event: "message-send-finished", fields: fields)
                        completion.finish(.failure(RemoteIMClientError.operationFailed(
                            code: code, description: desc ?? failureDescription)))
                    }
                )
            }
        }
    }

    private nonisolated static func receipt(for message: V2TIMMessage) -> RemoteIMSendReceipt {
        RemoteIMSendReceipt(remoteID: message.msgID, createdAt: message.timestamp)
    }

    private nonisolated static func groupText(
        from message: V2TIMMessage, groupID: String
    ) -> IncomingRemoteIMGroupText? {
        guard message.elemType.rawValue == 1,
              let fromUserID = nonEmpty(message.sender),
              let text = message.textElem?.text,
              !text.isEmpty else { return nil }
        return IncomingRemoteIMGroupText(
            groupID: groupID,
            fromUserID: fromUserID,
            text: text,
            remoteID: message.msgID,
            createdAt: message.timestamp ?? Date()
        )
    }

    private nonisolated static func messageMetadata(for message: V2TIMMessage) -> RemoteIMCloudMetadata? {
        RemoteIMCloudMetadataCodec.decode(message.cloudCustomData)
    }

    private nonisolated static func profile(from info: V2TIMUserFullInfo) -> RemoteIMUserProfile? {
        guard let userID = info.userID?.trimmingCharacters(in: .whitespacesAndNewlines),
              !userID.isEmpty
        else {
            return nil
        }
        let nickname = info.nickName?.trimmingCharacters(in: .whitespacesAndNewlines)
        let avatarURL = info.faceURL?.trimmingCharacters(in: .whitespacesAndNewlines)
        return RemoteIMUserProfile(
            userID: userID,
            displayName: nickname?.isEmpty == false ? nickname! : userID,
            avatarURL: avatarURL?.isEmpty == false ? avatarURL : nil
        )
    }

    nonisolated func onRecvC2CTextMessage(msgID: String, sender: V2TIMUserInfo, text: String?) {
        // AdvancedMsgListener 提供完整的原始 metadata；同一条文本只走一次持久化/ready 路径。
        // 不再另发 findMessages lookup，也不从迟到回调重新捕获绑定。
    }

    private struct IncomingElementParts {
        var caption: String?
        var image: V2TIMImageElem?
        var sound: V2TIMSoundElem?
        var video: V2TIMVideoElem?
        var file: V2TIMFileElem?
    }

    private nonisolated static func firstElement(in message: V2TIMMessage) -> V2TIMElem? {
        switch message.elemType.rawValue {
        case 1: return message.textElem
        case 3: return message.imageElem
        case 4: return message.soundElem
        case 5: return message.videoElem
        case 6: return message.fileElem
        default: return nil
        }
    }

    private nonisolated static func incomingElementParts(
        from message: V2TIMMessage
    ) -> IncomingElementParts {
        var parts = IncomingElementParts()
        var element = firstElement(in: message)
        while let current = element {
            if let text = current as? V2TIMTextElem,
               parts.caption == nil,
               let clean = text.text?.trimmingCharacters(in: .whitespacesAndNewlines),
               !clean.isEmpty
            {
                parts.caption = clean
            } else if let image = current as? V2TIMImageElem, parts.image == nil {
                parts.image = image
            } else if let sound = current as? V2TIMSoundElem, parts.sound == nil {
                parts.sound = sound
            } else if let video = current as? V2TIMVideoElem, parts.video == nil {
                parts.video = video
            } else if let file = current as? V2TIMFileElem, parts.file == nil {
                parts.file = file
            }
            element = current.next()
        }
        return parts
    }

    private nonisolated static func initialMetadata(_ msg: V2TIMMessage, owner: String, localID: UUID) -> RemoteIMMessage? {
        guard !msg.isSelf, nonEmpty(msg.groupID) == nil, let peer = nonEmpty(msg.sender) ?? nonEmpty(msg.userID),
              !owner.isEmpty, peer != owner, msg.customElem == nil else { return nil }
        let parts = incomingElementParts(from: msg), metadata = messageMetadata(for: msg)
        let text = msg.textElem?.text ?? parts.caption ?? ""
        guard RemoteDesktopSignal.decodeText(text) == nil else { return nil }
        let id = localID, remoteID = nonEmpty(msg.msgID), date = msg.timestamp ?? Date()
        var voice: RemoteIMVoiceAttachment?, image: RemoteIMImageAttachment?
        var file: RemoteIMFileAttachment?, video: RemoteIMVideoAttachment?
        var display = text
        if let sound = parts.sound {
            let duration = max(1, Int(sound.duration))
            voice = RemoteIMVoiceAttachment(localFilePath: voiceCacheURL(remoteID: sound.uuid ?? msg.msgID,
                messageID: msg.msgID).path, durationSeconds: duration, remoteID: remoteID)
            display = "[语音消息 \(duration)s]"
        } else if let item = parts.image {
            let resource = preferredImage(from: item.imageList)
            image = RemoteIMImageAttachment(localFilePath: imageCacheURL(remoteID: resource?.uuid ?? msg.msgID ?? id.uuidString,
                messageID: msg.msgID, imageURL: resource?.url).path, remoteID: remoteID,
                width: resource.flatMap { $0.width > 0 ? Int($0.width) : nil },
                height: resource.flatMap { $0.height > 0 ? Int($0.height) : nil },
                sizeBytes: resource.flatMap { $0.size > 0 ? Int($0.size) : nil })
            if display.isEmpty { display = "[图片消息]" }
        } else if let item = parts.video {
            let mediaID = nonEmpty(item.videoUUID) ?? remoteID ?? id.uuidString
            let hasCover = nonEmpty(item.snapshotUUID) != nil || item.snapshotSize > 0 || item.snapshotWidth > 0 || item.snapshotHeight > 0
            video = RemoteIMVideoAttachment(localPath: videoCacheURL(remoteID: mediaID, videoType: item.videoType).path,
                coverPath: hasCover ? videoCoverCacheURL(remoteID: mediaID).path : nil,
                durationSeconds: max(0, Int(item.duration)), width: max(0, Int(item.snapshotWidth)),
                height: max(0, Int(item.snapshotHeight)), sizeBytes: max(0, Int64(item.videoSize)))
            if display.isEmpty { display = "[视频消息]" }
        } else if let item = parts.file {
            let name = cleanFileName(item.filename)
            file = RemoteIMFileAttachment(localFilePath: fileCacheURL(remoteID: item.uuid ?? msg.msgID,
                messageID: msg.msgID, fileName: name).path, fileName: name, mimeType: mimeType(for: name),
                remoteID: remoteID, sizeBytes: item.fileSize > 0 ? Int(item.fileSize) : nil)
            if display.isEmpty { display = "[文件] " + name }
        } else if display.isEmpty { return nil }
        return RemoteIMMessage(id: id, remoteID: remoteID, fromUserID: peer, toUserID: owner,
            text: display, mediaCaption: (voice != nil || image != nil || file != nil || video != nil) ? parts.caption : nil,
            captionAbove: metadata?.captionAbove == true, quote: metadata?.quote,
            voiceAttachment: voice, imageAttachment: image, fileAttachment: file, videoAttachment: video,
            approvalRequest: metadata?.approvalRequest, approvalDecision: metadata?.approvalDecision,
            direction: .incoming, status: .received, createdAt: date)
    }

    nonisolated func onRecvNewMessage(msg: V2TIMMessage) {
        // 原 SDK 入口先登记持久化序列并固定只读 epoch；解析/排队不能让旧未绑定消息改投新绑定。
        // 多线程 SDK 回调只在这个纯 metadata 区间串行化，不跨 SQL/下载/await 持锁。
        rawIngressLock.lock(); defer { rawIngressLock.unlock() }
        let value = SDKCallbackValue(value: msg)
        let account = Self.diagnosticAccountSnapshot.load()
        let login = profileAccountSnapshot.load()
        let peer = Self.nonEmpty(msg.sender) ?? Self.nonEmpty(msg.userID) ?? ""
        let direct = Self.nonEmpty(msg.groupID) == nil && !msg.isSelf
        let localID = UUID()
        let sourceID = Self.nonEmpty(msg.msgID).map { "sdk:" + $0 } ?? "local:" + localID.uuidString
        let incomingMessageID = msg.msgID ?? ""
        let fenceTicket = direct ? persistenceFence.begin(accountTag: account, generation: login.generation,
            peerID: peer, sourceID: sourceID) : nil
        let captured = IMSessionBindingCaptureRegistry.shared.captureIngress(accountTag: account,
            peerID: peer, messageID: msg.msgID ?? "", recipientID: login.userID ?? "")
        let parts = Self.incomingElementParts(from: msg)
        let metadata = Self.messageMetadata(for: msg)
        let text = parts.caption ?? ""
        let ticket = IMSessionBindingCaptureRegistry.shared.admit(captured, origin: metadata?.origin,
            group: !direct, controlFrame: metadata?.approvalRequest != nil || metadata?.approvalDecision != nil ||
                msg.customElem != nil || RemoteDesktopSignal.decodeText(text) != nil,
            hasContent: !text.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty ||
                parts.image != nil || parts.sound != nil || parts.video != nil || parts.file != nil)
        let initial = Self.initialMetadata(msg, owner: login.userID ?? "", localID: localID)
        Task { @MainActor [weak self] in
            guard let self, account == Self.diagnosticAccountSnapshot.load(), self.profileAccountSnapshot.load() == login else {
                IMSessionBindingControl.abandon(ticket)
                if let fenceTicket { self?.persistenceFence.finish(fenceTicket, persisted: false) }
                return
            }
            var receipt: RemoteIMMetadataReceipt?
            if let initial, let fenceTicket, let account, let persist = self.onIncomingMetadata {
                receipt = await persist(IncomingRemoteIMMetadata(accountTag: account, fenceTicket: fenceTicket, message: initial))
            }
            var eligible = initial == nil || (receipt?.persisted == true && receipt?.wasInserted == true)
            if let ticket, let receipt, receipt.persisted {
                let confirmed = IMSessionBindingControl.persistedInput(ticket, receipt: receipt, remoteID: initial?.remoteID)
                eligible = eligible && confirmed
            } else if initial != nil { IMSessionBindingControl.abandon(ticket, reason: "persistence_failed") }
            if let fenceTicket { self.persistenceFence.finish(fenceTicket, persisted: initial == nil || receipt?.persisted == true) }
            guard account == Self.diagnosticAccountSnapshot.load(), self.profileAccountSnapshot.load() == login else { return }
            // 重复 SDK delivery 已在普通 DB 核实；不要再启动第二次下载/原 live 输入。
            if initial != nil && (receipt?.persisted != true || receipt?.wasInserted != true) { return }
            // 仅原 SDK 新到且已持久化的 Human v2 控制帧可回复审批；普通正文不参与猜测。
            // 保存原 SDK ID、收发身份和原不可变请求，纯 control 不直接调用 Core。
            if eligible, initial != nil, let decision = metadata?.approvalDecision {
                IMSessionApprovalRequests.shared.receive(decision, accountTag: account, peerID: peer,
                    messageID: incomingMessageID, recipientID: login.userID ?? "",
                    origin: metadata?.origin, group: !direct)
            }
            let persistedReceipt = receipt
            Self.incomingQueue.async { [weak self] in
                guard let self, account == Self.diagnosticAccountSnapshot.load() else { return }
                self.processIncomingMessage(value.value, sessionTicket: eligible ? ticket : nil,
                                            metadataReceipt: persistedReceipt)
            }
        }
    }

    /// 把 SDK 消息分派给文本、语音、文件、图片或视频处理器。
    /// 分派前确认消息身份和方向，防止重复回执形成重复气泡。
    private nonisolated func processIncomingMessage(_ msg: V2TIMMessage, sessionTicket: IMSessionIngressTicket?, metadataReceipt: RemoteIMMetadataReceipt?) {
        dispatchPrecondition(condition: .onQueue(Self.incomingQueue))
        if let groupID = Self.nonEmpty(msg.groupID) {
            if let activationData = msg.customElem?.data,
               RemoteIMGroupActivationCodec.isActivation(activationData) {
                notifyGroupMembershipChanged()
                return
            }
            if let incoming = Self.groupText(from: msg, groupID: groupID) {
                guard let account = Self.diagnosticAccountSnapshot.load() else { return }
                Task { @MainActor [weak self] in
                    guard account == Self.diagnosticAccountSnapshot.load() else { return }
                    self?.onIncomingGroupText?(incoming)
                }
            } else {
                Self.logSDK(level: .warning, event: "group-message-unhandled", fields: [
                    "group": Self.peerTag(groupID),
                    "message": Self.messageTag(msg.msgID),
                    "elem_type": String(msg.elemType.rawValue)
                ])
            }
            return
        }
        guard !msg.isSelf else { return }
        let fromUserID = msg.sender ?? msg.userID ?? ""
        guard !fromUserID.isEmpty else {
            Self.logSDK(
                level: .warning,
                event: "message-dropped",
                fields: ["reason": "no-sender", "message": Self.messageTag(msg.msgID)]
            )
            return
        }

        if let signal = RemoteIMActivityCodec.decode(msg.customElem?.data) {
            let account = Self.diagnosticAccountSnapshot.load()
            Task { @MainActor [weak self] in
                guard account == Self.diagnosticAccountSnapshot.load() else { return }
                self?.onIncomingActivity?(
                    IncomingRemoteIMActivity(fromUserID: fromUserID, signal: signal)
                )
            }
            return
        }

        let parts = Self.incomingElementParts(from: msg)
        let metadata = Self.messageMetadata(for: msg)
        let captionAbove = metadata?.captionAbove == true

        // 多元素消息必须先找附件，不能因为存在 textElem 就提前 return；否则
        // [文本, 图片] 会只收到文字、整张图静默消失。
        if let soundElem = parts.sound {
            handleIncomingSound(msg: msg, soundElem: soundElem, fromUserID: fromUserID, sessionTicket: sessionTicket, metadataReceipt: metadataReceipt)
            return
        }

        if let imageElem = parts.image {
            handleIncomingImage(
                msg: msg,
                imageElem: imageElem,
                fromUserID: fromUserID,
                caption: parts.caption,
                captionAbove: captionAbove, sessionTicket: sessionTicket, metadataReceipt: metadataReceipt
            )
            return
        }

        if let videoElem = parts.video {
            handleIncomingVideo(
                msg: msg,
                videoElem: videoElem,
                fromUserID: fromUserID,
                caption: parts.caption,
                captionAbove: captionAbove, sessionTicket: sessionTicket, metadataReceipt: metadataReceipt
            )
            return
        }

        if let fileElem = parts.file {
            handleIncomingFile(
                msg: msg,
                fileElem: fileElem,
                fromUserID: fromUserID,
                caption: parts.caption,
                captionAbove: captionAbove, sessionTicket: sessionTicket, metadataReceipt: metadataReceipt
            )
            return
        }

        // 文本也沿唯一 advanced metadata→持久化→ready 链交付，不再并行 lookup。
        if let text = msg.textElem?.text ?? parts.caption {
            emitIncomingText(fromUserID: fromUserID, text: text, remoteID: msg.msgID,
                origin: metadata?.origin, approvalRequest: metadata?.approvalRequest,
                approvalDecision: metadata?.approvalDecision, quote: metadata?.quote,
                createdAt: metadataReceipt?.createdAt ?? msg.timestamp ?? Date(), metadataReceipt: metadataReceipt)
            emitSessionIMInput(ticket: sessionTicket, text: text, kind: "text", fileURL: nil, quote: metadata?.quote)
            return
        }
        IMSessionBindingControl.abandon(sessionTicket)

        // 一个分支都没命中就说明这类消息本端不认，而且原来是悄悄丢掉的——桌面端那次
        // 语音消息丢失就是死在同样的静默里。elem_type 是这条日志的关键：光知道
        // "有消息没人处理"还得再猜一轮，有了类型号才能直接对上缺哪个分支
        // （例如视频是 5，本端目前没有对应实现）。
        Self.logSDK(
            level: .warning,
            event: "message-unhandled",
            fields: [
                "reason": "no-branch-matched",
                "peer": Self.peerTag(fromUserID),
                "message": Self.messageTag(msg.msgID),
                "elem_type": String(msg.elemType.rawValue)
            ]
        )
    }

    /// 把入站语音下载或引用转换为可播放附件。
    /// 附件未就绪时保留消息本体，待文件可用后再更新气泡。
    private nonisolated func handleIncomingSound(
        msg: V2TIMMessage,
        soundElem: V2TIMSoundElem,
        fromUserID: String, sessionTicket: IMSessionIngressTicket?, metadataReceipt: RemoteIMMetadataReceipt?
    ) {
        let createdAt = metadataReceipt?.createdAt ?? msg.timestamp ?? Date()
        let durationSeconds = max(1, Int(soundElem.duration))
        let mediaID = soundElem.uuid ?? msg.msgID
        let remoteID = Self.nonEmpty(msg.msgID)
        let metadata = Self.messageMetadata(for: msg)
        let origin = metadata?.origin
        let quote = metadata?.quote
        let targetURL = metadataReceipt?.message.voiceAttachment.map { URL(fileURLWithPath: $0.localFilePath) } ??
            Self.voiceCacheURL(remoteID: mediaID, messageID: msg.msgID)
        let transferGeneration = attachmentDownloads.generation
        let startedAt = ProcessInfo.processInfo.systemUptime
        let diagnosticFields = Self.messageDiagnosticFields(
            kind: "voice",
            peerUserID: fromUserID,
            messageID: remoteID,
            metadata: ["duration_seconds": String(durationSeconds)]
        )
        Self.logSDK(level: .info, event: "message-receive-callback", fields: diagnosticFields)
        Self.logSDK(level: .info, event: "media-download-start", fields: diagnosticFields)

        soundElem.getUrl { [weak self] signedURL in
            self?.downloadAttachment(
                signedURL: signedURL, targetURL: targetURL,
                maximumBytes: 64 * 1024 * 1024,
                generation: transferGeneration, expectedAccount: diagnosticFields["account"]
            ) { result in
                Self.incomingQueue.async {
                    guard diagnosticFields["account"] == Self.diagnosticAccountSnapshot.load() else { return }
                    var fields = diagnosticFields
                    fields["duration_ms"] = Self.elapsedMilliseconds(since: startedAt)
                    guard result.succeeded else {
                        fields["result"] = "failed"
                        fields["code"] = String(result.curlCode)
                        fields["download_status"] = String(result.status)
                        Self.logSDK(level: .warning, event: "media-download-finished", fields: fields)
                        IMSessionBindingControl.abandon(sessionTicket, reason: "preparation_failed")
                        return
                    }
                    fields["result"] = "ok"
                    Self.logSDK(level: .info, event: "media-download-finished", fields: fields)
                    Task {
                        @MainActor [
                            weak self, sessionTicket, fromUserID, targetURL, durationSeconds, remoteID, origin, quote, createdAt
                        ] in
                        guard diagnosticFields["account"] == Self.diagnosticAccountSnapshot.load() else { return }
                        let event = IncomingRemoteIMVoice(
                            fromUserID: fromUserID,
                            fileURL: targetURL,
                            durationSeconds: durationSeconds,
                            remoteID: remoteID,
                            origin: origin,
                            quote: quote,
                            createdAt: createdAt, metadataReceipt: metadataReceipt
                        )
                        self?.onIncomingVoice?(event)
                        self?.emitSessionIMInput(ticket: sessionTicket, text: "", kind: "voice", fileURL: targetURL, quote: quote)
                    }
                }
            }
        }
    }

    /// 处理入站文件的名称、大小与本地下载状态。
    /// 文件下载失败不应移除消息，用户仍可看到并重试。
    private nonisolated func handleIncomingFile(
        msg: V2TIMMessage,
        fileElem: V2TIMFileElem,
        fromUserID: String,
        caption: String?,
        captionAbove: Bool, sessionTicket: IMSessionIngressTicket?, metadataReceipt: RemoteIMMetadataReceipt?
    ) {
        let fileName = Self.cleanFileName(fileElem.filename)
        let createdAt = metadataReceipt?.createdAt ?? msg.timestamp ?? Date()
        let mediaID = fileElem.uuid ?? msg.msgID
        let remoteID = Self.nonEmpty(msg.msgID)
        let metadata = Self.messageMetadata(for: msg)
        let origin = metadata?.origin
        let quote = metadata?.quote
        let targetURL = metadataReceipt?.message.fileAttachment.map { URL(fileURLWithPath: $0.localFilePath) } ??
            Self.fileCacheURL(remoteID: mediaID, messageID: msg.msgID, fileName: fileName)
        let transferGeneration = attachmentDownloads.generation
        let mimeType = Self.mimeType(for: fileName)
        let sizeBytes = fileElem.fileSize > 0 ? Int(fileElem.fileSize) : nil
        let startedAt = ProcessInfo.processInfo.systemUptime
        var fileMetadata = [
            "bytes": sizeBytes.map(String.init) ?? "unknown",
            "mime_type": mimeType,
        ]
        if let requestID = RemoteDiagnosticsProtocol.requestID(reportFileName: fileName) {
            fileMetadata["requestId"] = requestID.uuidString.lowercased()
        }
        let diagnosticFields = Self.messageDiagnosticFields(
            kind: "file",
            peerUserID: fromUserID,
            messageID: remoteID,
            metadata: fileMetadata
        )
        Self.logSDK(level: .info, event: "message-receive-callback", fields: diagnosticFields)
        Self.logSDK(level: .info, event: "media-download-start", fields: diagnosticFields)

        fileElem.getUrl { [weak self] signedURL in
            self?.downloadAttachment(
                signedURL: signedURL, targetURL: targetURL,
                maximumBytes: 512 * 1024 * 1024,
                generation: transferGeneration, expectedAccount: diagnosticFields["account"]
            ) { result in
                Self.incomingQueue.async {
                    guard diagnosticFields["account"] == Self.diagnosticAccountSnapshot.load() else { return }
                    var fields = diagnosticFields
                    fields["duration_ms"] = Self.elapsedMilliseconds(since: startedAt)
                    guard result.succeeded else {
                        fields["result"] = "failed"
                        fields["code"] = String(result.curlCode)
                        fields["download_status"] = String(result.status)
                        Self.logSDK(level: .warning, event: "media-download-finished", fields: fields)
                        IMSessionBindingControl.abandon(sessionTicket, reason: "preparation_failed")
                        return
                    }
                    fields["result"] = "ok"
                    Self.logSDK(level: .info, event: "media-download-finished", fields: fields)
                    Task {
                        @MainActor [
                            weak self, sessionTicket, fromUserID, targetURL, fileName, mimeType, remoteID, sizeBytes, origin,
                            quote, createdAt, caption, captionAbove
                        ] in
                        guard diagnosticFields["account"] == Self.diagnosticAccountSnapshot.load() else { return }
                        let event = IncomingRemoteIMFile(
                            accountTag: diagnosticFields["account"] ?? "unbound",
                            fromUserID: fromUserID,
                            fileURL: targetURL,
                            fileName: fileName,
                            mimeType: mimeType,
                            remoteID: remoteID,
                            sizeBytes: sizeBytes,
                            caption: caption,
                            captionAbove: captionAbove,
                            origin: origin,
                            quote: quote,
                            createdAt: createdAt, metadataReceipt: metadataReceipt
                        )
                        self?.onIncomingFile?(event)
                        self?.emitSessionIMInput(ticket: sessionTicket, text: caption ?? "", kind: "file", fileURL: targetURL, quote: quote)
                    }
                }
            }
        }
    }

    /// 处理入站图片缩略图、原图和尺寸信息。
    /// 图片未下载完成时保持占位尺寸，防止消息列表布局跳动。
    private nonisolated func handleIncomingImage(
        msg: V2TIMMessage,
        imageElem: V2TIMImageElem,
        fromUserID: String,
        caption: String?,
        captionAbove: Bool, sessionTicket: IMSessionIngressTicket?, metadataReceipt: RemoteIMMetadataReceipt?
    ) {
        guard let image = Self.preferredImage(from: imageElem.imageList) else { IMSessionBindingControl.abandon(sessionTicket); return }
        let createdAt = metadataReceipt?.createdAt ?? msg.timestamp ?? Date()
        let mediaID = image.uuid ?? msg.msgID
        let remoteID = Self.nonEmpty(msg.msgID)
        let metadata = Self.messageMetadata(for: msg)
        let origin = metadata?.origin
        let quote = metadata?.quote
        let targetURL = metadataReceipt?.message.imageAttachment.map { URL(fileURLWithPath: $0.localFilePath) } ??
            Self.imageCacheURL(remoteID: mediaID, messageID: msg.msgID, imageURL: image.url)
        let transferGeneration = attachmentDownloads.generation
        let width = image.width > 0 ? Int(image.width) : nil
        let height = image.height > 0 ? Int(image.height) : nil
        let sizeBytes = image.size > 0 ? Int(image.size) : nil
        let startedAt = ProcessInfo.processInfo.systemUptime
        let diagnosticFields = Self.messageDiagnosticFields(
            kind: "image",
            peerUserID: fromUserID,
            messageID: remoteID,
            metadata: [
                "width": width.map(String.init) ?? "unknown",
                "height": height.map(String.init) ?? "unknown",
                "bytes": sizeBytes.map(String.init) ?? "unknown",
            ]
        )
        Self.logSDK(level: .info, event: "message-receive-callback", fields: diagnosticFields)
        Self.logSDK(level: .info, event: "media-download-start", fields: diagnosticFields)
        downloadAttachment(
            signedURL: image.url, targetURL: targetURL,
            maximumBytes: 32 * 1024 * 1024,
            generation: transferGeneration, expectedAccount: diagnosticFields["account"]
        ) { result in
                Self.incomingQueue.async {
                    guard diagnosticFields["account"] == Self.diagnosticAccountSnapshot.load() else { return }
                    var fields = diagnosticFields
                    fields["duration_ms"] = Self.elapsedMilliseconds(since: startedAt)
                    guard result.succeeded else {
                        fields["result"] = "failed"
                        fields["code"] = String(result.curlCode)
                        fields["download_status"] = String(result.status)
                        Self.logSDK(level: .warning, event: "media-download-finished", fields: fields)
                        IMSessionBindingControl.abandon(sessionTicket, reason: "preparation_failed")
                        return
                    }
                    fields["result"] = "ok"
                    Self.logSDK(level: .info, event: "media-download-finished", fields: fields)
                    Task {
                        @MainActor [
                            weak self, sessionTicket, fromUserID, targetURL, remoteID, width, height, sizeBytes, origin, quote,
                            createdAt, caption, captionAbove
                        ] in
                        guard diagnosticFields["account"] == Self.diagnosticAccountSnapshot.load() else { return }
                        let event = IncomingRemoteIMImage(
                            fromUserID: fromUserID,
                            fileURL: targetURL,
                            remoteID: remoteID,
                            width: width,
                            height: height,
                            sizeBytes: sizeBytes,
                            caption: caption,
                            captionAbove: captionAbove,
                            origin: origin,
                            quote: quote,
                            createdAt: createdAt, metadataReceipt: metadataReceipt
                        )
                        self?.onIncomingImage?(event)
                        self?.emitSessionIMInput(ticket: sessionTicket, text: caption ?? "", kind: "image", fileURL: targetURL, quote: quote)
                    }
                }
            }
    }

    /// 处理入站视频封面、媒体文件与下载进度。
    /// 消息先保持稳定标识，媒体就绪后只更新附件状态而不重建整页。
    private nonisolated func handleIncomingVideo(
        msg: V2TIMMessage,
        videoElem: V2TIMVideoElem,
        fromUserID: String,
        caption: String?,
        captionAbove: Bool, sessionTicket: IMSessionIngressTicket?, metadataReceipt: RemoteIMMetadataReceipt?
    ) {
        let createdAt = metadataReceipt?.createdAt ?? msg.timestamp ?? Date()
        let mediaID = Self.nonEmpty(videoElem.videoUUID) ?? Self.nonEmpty(msg.msgID) ?? UUID().uuidString
        let remoteID = Self.nonEmpty(msg.msgID)
        let metadata = Self.messageMetadata(for: msg)
        let origin = metadata?.origin
        let quote = metadata?.quote
        let durationSeconds = max(0, Int(videoElem.duration))
        let width = max(0, Int(videoElem.snapshotWidth))
        let height = max(0, Int(videoElem.snapshotHeight))
        let sizeBytes = max(0, Int64(videoElem.videoSize))
        let videoURL = metadataReceipt?.message.videoAttachment.map { URL(fileURLWithPath: $0.localPath) } ??
            Self.videoCacheURL(remoteID: mediaID, videoType: videoElem.videoType)
        let transferGeneration = attachmentDownloads.generation
        let hasSnapshot = Self.nonEmpty(videoElem.snapshotUUID) != nil ||
            videoElem.snapshotSize > 0 || width > 0 || height > 0
        let coverURL = metadataReceipt?.message.videoAttachment?.coverPath.map { URL(fileURLWithPath: $0) } ??
            (hasSnapshot ? Self.videoCoverCacheURL(remoteID: mediaID) : nil)
        let diagnosticFields = Self.messageDiagnosticFields(
            kind: "video",
            peerUserID: fromUserID,
            messageID: remoteID,
            metadata: [
                "duration_seconds": String(durationSeconds),
                "width": String(width),
                "height": String(height),
                "bytes": String(sizeBytes),
            ]
        )
        Self.logSDK(level: .info, event: "message-receive-callback", fields: diagnosticFields)

        // 先用云消息元数据创建稳定气泡；封面和视频分别就绪时只更新同一个 remoteID。
        emitIncomingVideo(
            fromUserID: fromUserID,
            videoFileURL: videoURL,
            coverFileURL: coverURL,
            durationSeconds: durationSeconds,
            width: width,
            height: height,
            sizeBytes: sizeBytes,
            remoteID: remoteID,
            caption: caption,
            captionAbove: captionAbove,
            origin: origin,
            quote: quote,
            createdAt: createdAt,
            sessionTicket: sessionTicket, metadataReceipt: metadataReceipt, stage: .metadata
        )

        // 封面可先于视频到达。下载到临时路径，验证成功后再原子替换正式缓存。
        if let coverURL {
            if Self.isUsableCachedFile(coverURL) {
                emitIncomingVideo(
                    fromUserID: fromUserID,
                    videoFileURL: videoURL,
                    coverFileURL: coverURL,
                    durationSeconds: durationSeconds,
                    width: width,
                    height: height,
                    sizeBytes: sizeBytes,
                    remoteID: remoteID,
                    caption: caption,
                    captionAbove: captionAbove,
                    origin: origin,
                    quote: quote,
                    createdAt: createdAt,
                    sessionTicket: sessionTicket, metadataReceipt: metadataReceipt, stage: .coverReady
                )
            } else {
                var coverFields = diagnosticFields
                coverFields["asset"] = "cover"
                let coverStartedAt = ProcessInfo.processInfo.systemUptime
                Self.logSDK(level: .info, event: "media-download-start", fields: coverFields)
                videoElem.getSnapshotUrl { [weak self] signedURL in
                    self?.downloadAttachment(
                        signedURL: signedURL, targetURL: coverURL,
                        maximumBytes: 16 * 1024 * 1024,
                        generation: transferGeneration, expectedAccount: diagnosticFields["account"]
                    ) { [weak self] result in
                        Self.incomingQueue.async {
                            guard diagnosticFields["account"] == Self.diagnosticAccountSnapshot.load() else { return }
                            var fields = coverFields
                            guard result.succeeded else {
                                fields["result"] = "failed"
                                fields["code"] = String(result.curlCode)
                                fields["download_status"] = String(result.status)
                                fields["duration_ms"] = Self.elapsedMilliseconds(since: coverStartedAt)
                                Self.logSDK(level: .warning, event: "media-download-finished", fields: fields)
                                return
                            }
                            fields["result"] = "ok"
                            fields["duration_ms"] = Self.elapsedMilliseconds(since: coverStartedAt)
                            Self.logSDK(level: .info, event: "media-download-finished", fields: fields)
                            self?.emitIncomingVideo(
                                fromUserID: fromUserID,
                                videoFileURL: videoURL,
                                coverFileURL: coverURL,
                                durationSeconds: durationSeconds,
                                width: width,
                                height: height,
                                sizeBytes: sizeBytes,
                                remoteID: remoteID,
                                caption: caption,
                                captionAbove: captionAbove,
                                origin: origin,
                                quote: quote,
                                createdAt: createdAt,
                                sessionTicket: sessionTicket, metadataReceipt: metadataReceipt, stage: .coverReady
                            )
                        }
                    }
                }
            }
        }

        // 历史消息可能已有完整本地视频；命中缓存时不重新下载，也不改变原消息顺序。
        if Self.isUsableCachedFile(videoURL) {
            emitIncomingVideo(
                fromUserID: fromUserID,
                videoFileURL: videoURL,
                coverFileURL: coverURL,
                durationSeconds: durationSeconds,
                width: width,
                height: height,
                sizeBytes: sizeBytes,
                remoteID: remoteID,
                caption: caption,
                captionAbove: captionAbove,
                origin: origin,
                quote: quote,
                createdAt: createdAt,
                sessionTicket: sessionTicket, metadataReceipt: metadataReceipt, stage: .videoReady
            )
            return
        }

        var videoFields = diagnosticFields
        videoFields["asset"] = "video"
        let videoStartedAt = ProcessInfo.processInfo.systemUptime
        Self.logSDK(level: .info, event: "media-download-start", fields: videoFields)
        // 下载回调在独立队列核对账号快照；失败只更新附件状态，不能删掉消息本体。
        videoElem.getVideoUrl { [weak self] signedURL in
            self?.downloadAttachment(
                signedURL: signedURL, targetURL: videoURL,
                maximumBytes: 1024 * 1024 * 1024,
                generation: transferGeneration, expectedAccount: diagnosticFields["account"]
            ) { [weak self] result in
                Self.incomingQueue.async {
                    guard diagnosticFields["account"] == Self.diagnosticAccountSnapshot.load() else { return }
                    var fields = videoFields
                    guard result.succeeded else {
                        fields["result"] = "failed"
                        fields["code"] = String(result.curlCode)
                        fields["download_status"] = String(result.status)
                        fields["duration_ms"] = Self.elapsedMilliseconds(since: videoStartedAt)
                        Self.logSDK(level: .warning, event: "media-download-finished", fields: fields)
                        self?.emitIncomingVideo(
                            fromUserID: fromUserID,
                            videoFileURL: videoURL,
                            coverFileURL: coverURL,
                            durationSeconds: durationSeconds,
                            width: width,
                            height: height,
                            sizeBytes: sizeBytes,
                            remoteID: remoteID,
                            caption: caption,
                            captionAbove: captionAbove,
                            origin: origin,
                            quote: quote,
                            createdAt: createdAt,
                            sessionTicket: sessionTicket, metadataReceipt: metadataReceipt, stage: .videoFailed
                        )
                        return
                    }
                    fields["result"] = "ok"
                    fields["duration_ms"] = Self.elapsedMilliseconds(since: videoStartedAt)
                    Self.logSDK(level: .info, event: "media-download-finished", fields: fields)
                    self?.emitIncomingVideo(
                        fromUserID: fromUserID,
                        videoFileURL: videoURL,
                        coverFileURL: coverURL,
                        durationSeconds: durationSeconds,
                        width: width,
                        height: height,
                        sizeBytes: sizeBytes,
                        remoteID: remoteID,
                        caption: caption,
                        captionAbove: captionAbove,
                        origin: origin,
                        quote: quote,
                        createdAt: createdAt,
                        sessionTicket: sessionTicket, metadataReceipt: metadataReceipt, stage: .videoReady
                    )
                }
            }
        }
    }

    private nonisolated func downloadAttachment(
        signedURL: String?, targetURL: URL, maximumBytes: UInt64,
        generation: UInt64, expectedAccount: String?,
        completion: @escaping @Sendable (MaiChatAttachmentDownloadResult) -> Void
    ) {
        guard expectedAccount != nil, expectedAccount == Self.diagnosticAccountSnapshot.load() else { return }
        guard let signedURL = Self.nonEmpty(signedURL) else {
            completion(MaiChatAttachmentDownloadResult(
                status: Int32(MAI_DOWNLOAD_INVALID_ARGUMENT.rawValue),
                httpStatus: 0, curlCode: 0, bytes: 0
            ))
            return
        }
        attachmentDownloads.download(
            signedURL: signedURL, targetURL: targetURL,
            maximumBytes: maximumBytes, generation: generation,
            completion: completion
        )
    }

    private nonisolated func emitIncomingVideo(
        fromUserID: String,
        videoFileURL: URL,
        coverFileURL: URL?,
        durationSeconds: Int,
        width: Int,
        height: Int,
        sizeBytes: Int64,
        remoteID: String?,
        caption: String?,
        captionAbove: Bool,
        origin: RemoteIMMessageOrigin?,
        quote: RemoteIMQuote?,
        createdAt: Date,
        sessionTicket: IMSessionIngressTicket?, metadataReceipt: RemoteIMMetadataReceipt?,
        stage: RemoteIMVideoDownloadStage
    ) {
        if stage == .videoFailed { IMSessionBindingControl.abandon(sessionTicket, reason: "preparation_failed") }
        if stage == .videoReady {
            emitSessionIMInput(ticket: sessionTicket, text: caption ?? "", kind: "video", fileURL: videoFileURL, quote: quote)
        }
        let account = Self.diagnosticAccountSnapshot.load()
        Task { @MainActor [weak self] in
            guard account == Self.diagnosticAccountSnapshot.load() else { return }
            self?.onIncomingVideo?(
                IncomingRemoteIMVideo(
                    fromUserID: fromUserID,
                    videoFileURL: videoFileURL,
                    coverFileURL: coverFileURL,
                    durationSeconds: durationSeconds,
                    width: width,
                    height: height,
                    sizeBytes: sizeBytes,
                    caption: caption,
                    captionAbove: captionAbove,
                    remoteID: remoteID,
                    origin: origin,
                    quote: quote,
                    createdAt: createdAt,
                    stage: stage, metadataReceipt: metadataReceipt
                )
            )
        }
    }

    private nonisolated func emitSessionIMInput(ticket: IMSessionIngressTicket?, text: String,
                                               kind: String, fileURL: URL?, quote: RemoteIMQuote?) {
        guard let ticket else { return }
        let payload = IMSessionIncomingPayload(ticket: ticket, text: text, kind: kind,
            fileURL: fileURL, quoteText: quote?.digest)
        Task { @MainActor [weak self] in
            guard let self, IMSessionBindingCaptureRegistry.shared.isCurrent(ticket) else {
                IMSessionBindingControl.abandon(ticket); return
            }
            guard let receive = self.onSessionIMInput else { IMSessionBindingControl.abandon(ticket); return }
            receive(payload)
        }
    }

    private nonisolated func emitIncomingText(
        fromUserID: String,
        text: String,
        remoteID: String?,
        origin: RemoteIMMessageOrigin?,
        approvalRequest: RemoteIMApprovalRequest?,
        approvalDecision: RemoteIMApprovalDecision?,
        quote: RemoteIMQuote?,
        createdAt: Date, metadataReceipt: RemoteIMMetadataReceipt? = nil
    ) {
        let account = Self.diagnosticAccountSnapshot.load()
            Task {
                @MainActor [
                    weak self, fromUserID, text, remoteID, origin, approvalRequest, approvalDecision, quote, createdAt
                ] in
                guard account == Self.diagnosticAccountSnapshot.load() else { return }
            let event = IncomingRemoteIMText(
                fromUserID: fromUserID,
                text: text,
                remoteID: remoteID,
                origin: origin,
                approvalRequest: approvalRequest,
                approvalDecision: approvalDecision,
                quote: quote,
                createdAt: createdAt, metadataReceipt: metadataReceipt
            )
            self?.onIncomingText?(event)
        }
    }

    private nonisolated static func voiceCacheURL(remoteID: String?, messageID: String?) -> URL {
        let rawName = remoteID ?? messageID ?? UUID().uuidString
        return RemoteIMMediaStorage.fileURL(category: .incomingVoices, stem: rawName, pathExtension: "m4a")
    }

    private nonisolated static func videoCacheURL(
        remoteID: String,
        videoType: String?
    ) -> URL {
        let rawExtension = videoType?
            .trimmingCharacters(in: .whitespacesAndNewlines)
            .trimmingCharacters(in: CharacterSet(charactersIn: "."))
            .lowercased() ?? ""
        let fileExtension = !rawExtension.isEmpty && rawExtension.allSatisfy({ $0.isLetter || $0.isNumber })
            ? rawExtension
            : "mp4"
        return RemoteIMMediaStorage.fileURL(
            category: .incomingVideos,
            stem: remoteID,
            pathExtension: fileExtension
        )
    }

    private nonisolated static func videoCoverCacheURL(remoteID: String) -> URL {
        RemoteIMMediaStorage.fileURL(category: .incomingVideoCovers, stem: remoteID, pathExtension: "jpg")
    }

    private nonisolated static func isUsableCachedFile(_ url: URL) -> Bool {
        guard let values = try? url.resourceValues(forKeys: [.isRegularFileKey, .fileSizeKey]) else {
            return false
        }
        return values.isRegularFile == true && (values.fileSize ?? 0) > 0
    }

    private nonisolated static func nonEmpty(_ value: String?) -> String? {
        let cleanValue = value?.trimmingCharacters(in: .whitespacesAndNewlines) ?? ""
        return cleanValue.isEmpty ? nil : cleanValue
    }

    private nonisolated static func preferredImage(from imageList: [V2TIMImage]) -> V2TIMImage? {
        imageList.max { left, right in
            imageScore(left) < imageScore(right)
        }
    }

    private nonisolated static func imageScore(_ image: V2TIMImage) -> Int {
        let sizeScore = max(0, Int(image.size))
        if sizeScore > 0 { return sizeScore }
        return max(0, Int(image.width)) * max(0, Int(image.height))
    }

    private nonisolated static func imageCacheURL(
        remoteID: String?,
        messageID: String?,
        imageURL: String?
    ) -> URL {
        let rawName = remoteID ?? messageID ?? UUID().uuidString
        let pathExtension = URL(string: imageURL ?? "")?.pathExtension
        return RemoteIMMediaStorage.fileURL(
            category: .incomingImages,
            stem: rawName,
            pathExtension: pathExtension?.isEmpty == false ? pathExtension! : "jpg"
        )
    }

    private nonisolated static func fileCacheURL(
        remoteID: String?,
        messageID: String?,
        fileName: String
    ) -> URL {
        let rawName = remoteID ?? messageID ?? UUID().uuidString
        let fileExtension = URL(fileURLWithPath: fileName).pathExtension
        return RemoteIMMediaStorage.fileURL(
            category: .incomingFiles,
            stem: rawName,
            pathExtension: fileExtension.isEmpty ? "bin" : fileExtension
        )
    }

    private nonisolated static func cleanFileName(_ value: String?) -> String {
        let cleanValue = value?.trimmingCharacters(in: .whitespacesAndNewlines) ?? ""
        return cleanValue.isEmpty ? "remote-im-file.md" : cleanValue
    }

    private nonisolated static func mimeType(for fileName: String) -> String {
        let fileExtension = URL(fileURLWithPath: fileName).pathExtension
        return UTType(filenameExtension: fileExtension)?.preferredMIMEType ?? "application/octet-stream"
    }

    @objc nonisolated func onUserStatusChanged(userStatusList: [V2TIMUserStatus]!) {
        guard let userStatusList else { return }
        let updates = Self.statusMap(from: userStatusList)
        guard !updates.isEmpty else { return }
        Task { @MainActor [weak self, updates] in
            self?.onPresenceStatusChanged?(updates)
        }
    }

    private nonisolated func notifyGroupMembershipChanged() {
        guard let account = Self.diagnosticAccountSnapshot.load() else { return }
        Task { @MainActor [weak self] in
            guard account == Self.diagnosticAccountSnapshot.load() else { return }
            self?.onGroupMembershipChanged?()
        }
    }

    @objc nonisolated func onGroupCreated(groupID: String?) {
        notifyGroupMembershipChanged()
    }

    @objc nonisolated func onMemberEnter(groupID: String?, memberList: [V2TIMGroupMemberInfo]) {
        notifyGroupMembershipChanged()
    }

    @objc nonisolated func onMemberLeave(groupID: String?, member: V2TIMGroupMemberInfo) {
        notifyGroupMembershipChanged()
    }

    @objc nonisolated func onMemberInvited(
        groupID: String?, opUser: V2TIMGroupMemberInfo, memberList: [V2TIMGroupMemberInfo]
    ) {
        notifyGroupMembershipChanged()
    }

    @objc nonisolated func onMemberKicked(
        groupID: String?, opUser: V2TIMGroupMemberInfo, memberList: [V2TIMGroupMemberInfo]
    ) {
        notifyGroupMembershipChanged()
    }

    @objc nonisolated func onGroupDismissed(groupID: String?, opUser: V2TIMGroupMemberInfo) {
        notifyGroupMembershipChanged()
    }

    @objc nonisolated func onGroupRecycled(groupID: String?, opUser: V2TIMGroupMemberInfo) {
        notifyGroupMembershipChanged()
    }

    @objc nonisolated func onQuitFromGroup(groupID: String?) {
        notifyGroupMembershipChanged()
    }

    private nonisolated static func cleanUserIDs(_ userIDs: [String]) -> [String] {
        let normalized = userIDs
            .map { $0.trimmingCharacters(in: .whitespacesAndNewlines) }
            .filter { !$0.isEmpty }
        var deduped: [String] = []
        var visited = Set<String>()
        for userID in normalized where visited.insert(userID).inserted {
            deduped.append(userID)
        }
        return deduped
    }

    private nonisolated static func logMessageCreateFailure(
        kind: String,
        peerUserID: String
    ) {
        logSDK(
            level: .error,
            event: "message-create-failed",
            fields: [
                "kind": kind,
                "peer": peerTag(peerUserID),
            ]
        )
    }

    private nonisolated static func logIncomingMessage(
        kind: String,
        peerUserID: String,
        messageID: String?,
        metadata: [String: String] = [:]
    ) {
        logSDK(
            level: .info,
            event: "message-receive-callback",
            fields: messageDiagnosticFields(
                kind: kind,
                peerUserID: peerUserID,
                messageID: messageID,
                metadata: metadata
            )
        )
    }

    private nonisolated static func messageDiagnosticFields(
        kind: String,
        peerUserID: String,
        messageID: String?,
        metadata: [String: String] = [:]
    ) -> [String: String] {
        var fields = metadata
        fields["kind"] = kind
        fields["peer"] = peerTag(peerUserID)
        fields["message"] = messageTag(messageID)
        fields["account"] = diagnosticAccountSnapshot.load() ?? "unbound"
        return fields
    }

    private nonisolated static func peerTag(_ userID: String) -> String {
        guard !userID.isEmpty else { return "none" }
        return DiagnosticLogPrivacy.stableTag(userID, prefix: "u")
    }

    private nonisolated static func messageTag(_ messageID: String?) -> String {
        guard let messageID, !messageID.isEmpty else { return "none" }
        return DiagnosticLogPrivacy.stableTag(messageID, prefix: "m")
    }

    private nonisolated static func elapsedMilliseconds(since startedAt: TimeInterval) -> String {
        let elapsed = max(ProcessInfo.processInfo.systemUptime - startedAt, 0)
        return String(Int((elapsed * 1_000).rounded()))
    }

    private nonisolated static func logSDK(
        level: DiagnosticLogLevel,
        event: String,
        fields: [String: String] = [:]
    ) {
        var scopedFields = fields
        if scopedFields["account"] == nil { scopedFields["account"] = diagnosticAccountSnapshot.load() }
        let context = scopedFields
        Task { @MainActor in
            AppDiagnosticLog.shared.record(
                level: level,
                category: "remote-im",
                event: event,
                fields: context
            )
        }
    }

        private nonisolated static func statusMap(from userStatusList: [V2TIMUserStatus]) -> [String:
            RemoteIMPresenceStatus]
        {
            userStatusList.reduce(into: [:]) { partialResult, item in
            guard let userID = item.userID,
                  !userID.isEmpty else { return }
            partialResult[userID] = Self.presenceStatus(from: item.statusType)
        }
    }

    private nonisolated static func presenceStatus(from sdkStatus: V2TIMUserStatusType) -> RemoteIMPresenceStatus {
        let statusValue = Int(sdkStatus.rawValue)
        switch statusValue {
        case 1:
            return .online
        case 2, 3:
            return .offline
        default:
            return .unknown
        }
    }

}
#else
final class TencentIMClient: RemoteIMClient {
    var onSessionIMInput: ((IMSessionIncomingPayload) -> Void)?
    var onIncomingText: ((IncomingRemoteIMText) -> Void)?
    var onIncomingActivity: ((IncomingRemoteIMActivity) -> Void)?
    var onIncomingVoice: ((IncomingRemoteIMVoice) -> Void)?
    var onIncomingImage: ((IncomingRemoteIMImage) -> Void)?
    var onIncomingFile: ((IncomingRemoteIMFile) -> Void)?
    var onIncomingVideo: ((IncomingRemoteIMVideo) -> Void)?
    var onIncomingGroupText: ((IncomingRemoteIMGroupText) -> Void)?
    var onGroupMembershipChanged: (() -> Void)?
    var onPresenceStatusChanged: (([String: RemoteIMPresenceStatus]) -> Void)?

    func connect(sdkAppID: Int, userID: String, userSig: String) async throws {
        throw RemoteIMClientError.sdkNotIntegrated
    }

    func disconnect() async {}

    func sendText(
        to userID: String,
        text: String,
        origin: RemoteIMMessageOrigin,
        quote: RemoteIMQuote?
    ) async throws -> RemoteIMSendReceipt {
        throw RemoteIMClientError.sdkNotIntegrated
    }

    func createGroup(name: String, memberUserIDs: [String]) async throws -> RemoteIMGroup {
        throw RemoteIMClientError.sdkNotIntegrated
    }

    func joinedGroups() async throws -> [RemoteIMGroup] {
        throw RemoteIMClientError.sdkNotIntegrated
    }

    func sendGroupText(to groupID: String, text: String) async throws -> RemoteIMSendReceipt {
        throw RemoteIMClientError.sdkNotIntegrated
    }

    func groupHistory(groupID: String, count: Int, beforeRemoteID: String?) async throws
        -> RemoteIMGroupHistoryPage {
        throw RemoteIMClientError.sdkNotIntegrated
    }

    func sendActivity(to userID: String, signal: RemoteIMActivitySignal) async throws {
        throw RemoteIMClientError.sdkNotIntegrated
    }

    func sendApprovalDecision(
        to userID: String,
        token: String,
        action: RemoteIMApprovalAction
    ) async throws -> RemoteIMSendReceipt {
        throw RemoteIMClientError.sdkNotIntegrated
    }

        func sendVoice(to userID: String, recording: RemoteIMVoiceRecording, origin: RemoteIMMessageOrigin) async throws
            -> RemoteIMSendReceipt
        { throw RemoteIMClientError.sdkNotIntegrated
    }

        func sendImage(to userID: String, image: RemoteIMImageFile, caption: String?,
                       captionAbove: Bool, origin: RemoteIMMessageOrigin) async throws
            -> RemoteIMSendReceipt
        { throw RemoteIMClientError.sdkNotIntegrated
    }

        func sendVideo(to userID: String, video: RemoteIMVideoFile, caption: String?,
                       captionAbove: Bool, origin: RemoteIMMessageOrigin) async throws
            -> RemoteIMSendReceipt
        { throw RemoteIMClientError.sdkNotIntegrated
    }

        func sendFile(to userID: String, file: RemoteIMFile, caption: String?,
                      captionAbove: Bool, origin: RemoteIMMessageOrigin) async throws
            -> RemoteIMSendReceipt
        { throw RemoteIMClientError.sdkNotIntegrated
    }

    func deleteContact(userID: String) async throws {
        throw RemoteIMClientError.sdkNotIntegrated
    }

    func clearHistory(userID: String) async throws {
        throw RemoteIMClientError.sdkNotIntegrated
    }

    func refreshUserProfiles(userIDs: [String]) async throws -> [RemoteIMUserProfile] {
        throw RemoteIMClientError.sdkNotIntegrated
    }

    func refreshPresenceStatuses(userIDs: [String]) async throws -> [String: RemoteIMPresenceStatus] {
        throw RemoteIMClientError.sdkNotIntegrated
    }

    func subscribePresenceStatuses(userIDs: [String]) async throws {
        throw RemoteIMClientError.sdkNotIntegrated
    }

    func updateApplicationBadgeCount(_ count: Int) {
    }

}
#endif
