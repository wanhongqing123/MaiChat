// 测试宿主与素材仅供 Debug 使用，正式页面不在本文件实现。
#if DEBUG
import Foundation
import UIKit
import Combine
import CryptoKit
import MaiChatCore

struct AIQuestionAnswerUITestRequest {
    let sessionID: String
    let questionID: String
    let requestID: String
    let text: String
}

@MainActor
final class AIAssistantModelUITestSupport: ObservableObject {
    var sessionSwitchEnabled = false
    var screenFrameAccountID: String?
    // Isolated composer fixtures confirm routing without contacting a model.
    var steeringHandler: ((AISteeringRequest) async throws -> String)?
    var questionHandler: ((AIQuestionAnswerUITestRequest) async throws -> String)?
    // Simulator-owned files replace only the final download boundary.
    var remoteImageSources: [String: URL] = [:]
    var remoteImageDownloadHandler: ((AIRemoteImageRequest, URL) async throws -> Void)?
    @Published var remoteImageDownloadCount = 0
    @Published var remoteImageRequestedIDs: [String] = []
    @Published var remoteImagePolicyResult = "未执行"
    #if targetEnvironment(simulator)
    // Replace only the peer I/O boundary. History intentionally has no model fields;
    // the production owner command/ACK path must discover the configuration.
    var sessionModelHistoryHandler: (@MainActor (AIRemoteSession) async throws -> Data)?
    var sessionModelCommandHandler: (@MainActor (AIRemoteSession, String, [String: Any]) async throws -> Void)?
    var sessionModelPreparationHandler: (@MainActor () async throws -> Void)?
    var sessionModelPublishHandler: (@MainActor (String, Data) async throws -> Void)?
    var sessionModelSnapshotHandler: (@MainActor (String) throws -> Data?)?
    var sessionModelCreateObserver: (@MainActor ([String: String]) -> Void)?
    @Published var sessionModelPolicyResult = "未执行"
    #endif
}

enum AIAssistantUITestMutation {
    case workspace(String)
    case remoteSessions([AIRemoteSession])
    case clearRemoteSelection
    case attachments([String: [AIImportedFile]], firstMessageSessionID: String)
    case selectedHistory(String, [AIMessage])
    case remoteImagePage(Data, AIRemoteSession)
}

extension AIAssistantModel {
    func installVideoToolTestWorkspace(_ path: String) {
        applyUITestMutation(.workspace(path))
    }
}

#if targetEnvironment(simulator)
extension AIAssistantModel {
    func runSessionModelRemoteUITestChecks() async {
        guard ProcessInfo.processInfo.arguments.contains("--ai-remote-session-model-ui-test") else { return }
        uiTestSupport.sessionModelPolicyResult = "运行中"
        var checks = 0
        var failures: [String] = []
        func require(_ value: Bool, _ detail: String) { checks += 1; if !value { failures.append(detail) } }
        func owner(_ id: String, online: Bool = true, login: String = "owned-model-login", slot: String = "windows") -> AIRemoteSession {
            AIRemoteSession(sessionID: id, title: "远端模型 " + id, updatedAt: 1, busy: false,
                deviceSlot: slot, deviceName: "Owned desktop", deviceID: "owned-model-device",
                loginSessionID: login, online: online)
        }
        let a = owner("A"), b = owner("B"), local = owner("local")
        var states: [String: [String: Any]] = [
            a.sessionID: ["model": "glm-5.3", "reasoningEffort": "low", "wire": "chat_completions"],
            b.sessionID: ["model": "deepseek-flash", "reasoningEffort": "high", "wire": "responses"],
            local.sessionID: ["model": "local:owned-model", "reasoningEffort": "", "wire": "local"]]
        var commands: [(AIRemoteSession, String, [String: Any])] = []
        var readCount = 0
        func result(_ peer: AIRemoteSession, model: String? = nil, effort: String? = nil) -> [String: Any] {
            var value = states[peer.sessionID] ?? [:]
            if let model { value["model"] = model; value["wire"] = model.hasPrefix("glm-") ? "chat_completions" : "responses" }
            if let effort { value["reasoningEffort"] = effort }
            value["ok"] = true; value["session"] = peer.sessionID; value["defaultReasoningEffort"] = "high"
            return value
        }
        uiTestSupport.sessionModelHistoryHandler = { peer in
            // The deployed service history contract carries no model/effort/wire.
            try JSONSerialization.data(withJSONObject: ["ok": true, "messages": [[
                "id": "owned-message-" + peer.sessionID, "role": "assistant", "created": 1, "completed": 2,
                "active": false, "parts": [["id": "owned-text-" + peer.sessionID, "kind": "text", "text": "Owned history " + peer.sessionID]]]],
                "busy": false, "permissions": [], "questions": [], "questionMessages": []])
        }
        uiTestSupport.sessionModelCommandHandler = { [weak self] peer, id, payload in
            guard let self else { throw CancellationError() }
            commands.append((peer, id, payload))
            if payload["action"] as? String == "get_session" {
                readCount += 1
                await self.deliverSessionModelUITestReceipt(owner: peer, id: id, result: result(peer))
            }
        }
        applyUITestMutation(.remoteSessions([a, b, local]))
        await selectRemoteConversation(a)
        require(readCount == 1 && selectedRemote?.model == "glm-5.3" && settings.model == "glm-5.3" &&
                settings.effectiveReasoningEffort == "low" && canChangeSessionModel,
                "plain-history open did not request/read owner configuration; reads=\(readCount)")
        guard failures.isEmpty else { uiTestSupport.sessionModelPolicyResult = "FAIL " + failures.joined(separator: " | "); return }
        let draftA = "remote:windows:owned-model-login:A", draftB = "remote:windows:owned-model-login:B"
        drafts[draftA] = "owned draft A"; drafts[draftB] = "owned draft B"
        await selectPrimaryModel("deepseek-v4-pro", reasoningEffort: "max")
        guard let first = commands.last else { uiTestSupport.sessionModelPolicyResult = "FAIL no set command"; return }
        require(first.2["action"] as? String == "set_model" && selectedRemote?.model == "glm-5.3", "optimistic selection before owner ACK")
        let accepted = result(a, model: "deepseek-v4-pro", effort: "max")
        for wrong in [owner("A", login: "other-login"), owner("A", slot: "mac"), owner("other-session")] {
            await deliverSessionModelUITestReceipt(owner: wrong, id: first.1, result: accepted)
        }
        await deliverSessionModelUITestReceipt(owner: a, id: "not-the-command", result: accepted)
        await deliverSessionModelUITestReceipt(owner: a, id: first.1, result: result(a, model: "glm-5.3-flash", effort: "max"))
        await deliverSessionModelUITestReceipt(owner: a, id: first.1, result: result(a, model: "deepseek-v4-pro", effort: "low"))
        require(selectedRemote?.model == "glm-5.3", "mismatched tuple/model/effort consumed the pending request")
        await deliverSessionModelUITestReceipt(owner: a, id: first.1, result: accepted)
        require(selectedRemote?.model == "deepseek-v4-pro" && settings.effectiveReasoningEffort == "max", "valid ACK was lost after malformed receipts")
        await deliverSessionModelUITestReceipt(owner: a, id: first.1, result: result(a))
        require(settings.model == "deepseek-v4-pro", "duplicate old ACK reverted configuration")
        await selectPrimaryModel("glm-5.3-flash")
        guard let rejected = commands.last else { return }
        await selectRemoteConversation(b)
        error = "owned B status"
        let bMessages = messages
        await deliverSessionModelUITestReceipt(owner: a, id: rejected.1, result: ["ok": false, "error": "owned A rejection"])
        require(selectedRemote?.id == b.id && error == "owned B status" && messages == bMessages, "A rejection repainted B")
        require(drafts[draftA] == "owned draft A" && drafts[draftB] == "owned draft B", "ACK changed another room's draft")

        // A local preparation failure may arrive only after another real native session is selected.
        if let nativeA = sessions.first?.id {
            // Seed a saved draft before selecting the room, so the normal composer
            // restores that value; changing the backing dictionary after display
            // would bypass the actual editor and let its old value overwrite it.
            drafts[nativeA] = "owned local draft A"
            await selectConversation(nativeA)
            uiTestSupport.sessionModelPreparationHandler = { [weak self] in
                guard let self else { throw CancellationError() }
                await self.create()
                self.error = "owned local B status"
                throw AIBackendError(message: "owned local A preparation failure")
            }
            await selectPrimaryModel("local:" + AILocalModelStore.bundledPrimaryID)
            require(selected != nativeA && selectedRemote == nil && error == "owned local B status", "local A preparation failure repainted B")
            require(drafts[nativeA] == "owned local draft A", "local preparation failure lost A draft")
            uiTestSupport.sessionModelPreparationHandler = nil
        } else { require(false, "native session control missing") }

        if let nativeA = sessions.first?.id, let cloudB = sessions.first(where: { $0.id != nativeA })?.id {
            // A persisted local choice is represented at the native snapshot boundary;
            // the actual restoration/prepare await/catch is the production refresh path.
            await selectConversation(cloudB)
            var restorePrepares = 0
            uiTestSupport.sessionModelSnapshotHandler = { target in
                guard target == nativeA else { return nil }
                return try JSONSerialization.data(withJSONObject: ["ok": true, "changed": true,
                    "selectedModel": "local:" + AILocalModelStore.bundledPrimaryID,
                    "selectedReasoningEffort": "high", "selectedWire": "local", "configured": false])
            }
            uiTestSupport.sessionModelPreparationHandler = { [weak self] in
                guard let self else { throw CancellationError() }
                await self.selectConversation(cloudB)
                restorePrepares += 1
                self.error = "owned restored B status"
                throw AIBackendError(message: "owned restored A preparation failure")
            }
            await selectConversation(nativeA)
            require(restorePrepares == 1 && selected == cloudB && error == "owned restored B status",
                    "restored local A failure repainted B; prepare=\(restorePrepares), selected=\(selected == cloudB), error=\(error)")
            uiTestSupport.sessionModelSnapshotHandler = nil
            uiTestSupport.sessionModelPreparationHandler = nil

            await selectConversation("")
            drafts[""] = "owned canceled blank draft"
            let before = Set(sessions.map(\.id))
            var creates = 0
            uiTestSupport.sessionModelCreateObserver = { _ in creates += 1 }
            uiTestSupport.sessionModelPreparationHandler = { [weak self] in
                await self?.selectConversation(cloudB)
            }
            await selectPrimaryModel("local:" + AILocalModelStore.bundledPrimaryID, reasoningEffort: "max")
            require(creates == 0 && Set(sessions.map(\.id)) == before && selected == cloudB,
                    "canceled blank prepare dispatched an orphan session create")
            uiTestSupport.sessionModelCreateObserver = nil
            uiTestSupport.sessionModelPreparationHandler = nil
        } else { require(false, "local restore controls missing") }

        // A blank page's explicit model/effort action creates one native session
        // and transfers exactly its composer ownership, without sending a prompt.
        let attachment = AIImportedFile(relativePath: "owned-draft.txt", mimeType: "text/plain", isImage: false)
        for effortOnly in [false, true] {
            await selectConversation("")
            drafts[""] = "owned blank draft"
            applyUITestMutation(.attachments(["": [attachment]], firstMessageSessionID: ""))
            if effortOnly { await selectReasoningEffort("max") }
            else { await selectPrimaryModel("glm-5.3-flash", reasoningEffort: "low") }
            let target = selected
            require(!target.isEmpty && selectedRemote == nil && drafts[target] == "owned blank draft" && drafts[""] == nil,
                    "blank selection did not migrate original draft")
            require(pendingAttachments[target] == [attachment] && pendingAttachments[""] == nil,
                    "blank selection did not migrate original attachment ownership")
            require(settings.model == (effortOnly ? "deepseek-flash" : "glm-5.3-flash") &&
                    settings.effectiveReasoningEffort == (effortOnly ? "max" : "low"), "blank selection persisted the wrong native configuration")
        }

        await selectRemoteConversation(local)
        require(selectedRemote?.wire == "local" && canChangeSessionModel, "local owner get_session ACK was not consumed")
        let beforeEffort = commands.count
        await selectReasoningEffort("max")
        require(commands.count == beforeEffort, "remote local sent vendor reasoning effort")
        await selectPrimaryModel("glm-5.3-flash")
        guard let localSwitch = commands.last else { return }
        require(localSwitch.2["action"] as? String == "set_model" && localSwitch.2["model"] as? String == "glm-5.3-flash", "remote local could not select a cloud model")
        await deliverSessionModelUITestReceipt(owner: local, id: localSwitch.1, result: result(local, model: "glm-5.3-flash", effort: "high"))
        require(settings.model == "glm-5.3-flash", "cloud switch from local was not committed")
        states[local.sessionID] = ["model": "deepseek-v4-pro", "reasoningEffort": "max", "wire": "responses"]
        let beforeReconnect = readCount
        let writes = commands.filter { $0.2["action"] as? String == "set_model" }.count
        await reconnectSessionModelUITestPeer()
        require(readCount == beforeReconnect + 1 && settings.model == "deepseek-v4-pro", "reconnect did not read fresh owner configuration")
        require(commands.filter { $0.2["action"] as? String == "set_model" }.count == writes, "reconnect replayed a model mutation")
        await deliverSessionModelUITestReceipt(owner: local, id: localSwitch.1, result: result(local, model: "glm-5.3-flash", effort: "high"))
        require(settings.model == "deepseek-v4-pro", "pre-reconnect ACK reverted new owner read")

        // Only an explicitly owned publisher frame is captured; nothing is sent to a real account.
        var frames: [[String: Any]] = []
        uiTestSupport.sessionModelPublishHandler = { _, data in
            frames.append(try JSONSerialization.jsonObject(with: data) as? [String: Any] ?? [:])
        }
        func page(_ model: String, effort: String, wire: String) throws -> Data {
            try JSONSerialization.data(withJSONObject: ["ok": true, "session": "owned-publisher", "model": model,
                "reasoningEffort": effort, "wire": wire, "defaultReasoningEffort": "high",
                "messages": [], "permissions": [], "questions": [], "questionMessages": [], "busy": false, "turnId": ""])
        }
        do {
            try await publishSessionModelUITestState(page("glm-5.3", effort: "low", wire: "chat_completions"))
            let count = frames.count
            require(frames.last?["model"] as? String == "glm-5.3" && frames.last?["reasoningEffort"] as? String == "low", "ordinary publication dropped configuration")
            try await publishSessionModelUITestState(page("glm-5.3", effort: "low", wire: "chat_completions"))
            require(frames.count == count, "unchanged configuration republished")
            try await publishSessionModelUITestState(page("deepseek-v4-pro", effort: "max", wire: "responses"))
            require(frames.count == count + 1 && frames.last?["model"] as? String == "deepseek-v4-pro", "config-only change was dropped by state digest")
        } catch { require(false, "publisher fixture failed: " + error.localizedDescription) }
        uiTestSupport.sessionModelPublishHandler = nil
        let offline = owner("offline", online: false)
        applyUITestMutation(.remoteSessions([a, b, local, offline]))
        let beforeOffline = commands.count
        await selectRemoteConversation(offline)
        require(commands.count == beforeOffline && !canChangeSessionModel, "offline owner accepted model mutation")
        await selectRemoteConversation(b)
        uiTestSupport.sessionModelPolicyResult = failures.isEmpty ? "PASS \(checks) owner protocol checks" : "FAIL " + failures.joined(separator: " | ")
    }

    /// Use the real wire decoder/projection. The remote path is deliberately
    /// unusable on this phone; only its temporary attachment may supply pixels.
    func installRemoteImageUITestFixture() {
        let directory = FileManager.default.temporaryDirectory
            .appendingPathComponent("remote-images-" + ProcessInfo.processInfo.environment[
                "MAICHAT_AI_TEST_ID", default: UUID().uuidString], isDirectory: true)
        try? FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
        let renderer = UIGraphicsImageRenderer(size: CGSize(width: 96, height: 64))
        let png = renderer.pngData { context in
            UIColor(red: 45 / 255, green: 99 / 255, blue: 141 / 255, alpha: 1).setFill()
            context.fill(CGRect(x: 0, y: 0, width: 96, height: 64))
        }
        let file = directory.appendingPathComponent("screenshot.png")
        try? png.write(to: file, options: .atomic)
        let attachment = "5f7c1716-9fe4-47f6-9c8d-b60acff14752"
        uiTestSupport.remoteImageSources = [attachment: file]
        uiTestSupport.remoteImageDownloadHandler = { [weak self] request, target in
            guard let source = self?.uiTestSupport.remoteImageSources[request.attachmentID] else {
                throw AIBackendError(message: "Isolated image source is unavailable")
            }
            try await RemoteIMBackgroundWork.file { try FileManager.default.copyItem(at: source, to: target) }
        }
        uiTestSupport.screenFrameAccountID = "remote-image-fixture-" + ProcessInfo.processInfo.environment[
            "MAICHAT_AI_TEST_ID", default: UUID().uuidString]
        let remote = AIRemoteSession(sessionID: "remote-image-session", title: "远端图片测试",
            updatedAt: 1, busy: false, deviceSlot: "mac", deviceName: "图片测试 Mac",
            deviceID: "remote-image-fixture-mac", loginSessionID: "remote-image-fixture-login", online: false)
        let mode = ProcessInfo.processInfo.environment["MAICHAT_REMOTE_IMAGE_MODE", default: "assistant"]
        var image: [String: Any] = [
            "state": "ready", "retention": "temporary", "attachment_id": attachment,
            "owner_login_session_id": remote.loginSessionID,
            "room_key": "mac:remote-image-session", "name": "screenshot.png",
            "media_type": "image", "content_type": "image/png", "size_bytes": png.count,
            "expires_at": Int64(Date().addingTimeInterval(86_400).timeIntervalSince1970 * 1_000)]
        if mode == "pending" { image = ["state": "pending", "retention": "temporary"] }
        if mode == "failed" { image = ["state": "failed", "retention": "temporary", "error_code": "upload_failed"] }
        if mode == "expired" { image["expires_at"] = Int64(Date().timeIntervalSince1970 * 1_000) - 1 }
        if mode == "malformed" { image["room_key"] = "mac:another-session" }
        if mode == "network-failure" {
            uiTestSupport.remoteImageDownloadHandler = { [weak self] request, target in
                guard let self else { throw CancellationError() }
                if self.uiTestSupport.remoteImageDownloadCount == 1 { throw URLError(.cannotConnectToHost) }
                guard let source = self.uiTestSupport.remoteImageSources[request.attachmentID] else {
                    throw AIBackendError(message: "Isolated image source is unavailable")
                }
                try await RemoteIMBackgroundWork.file { try FileManager.default.copyItem(at: source, to: target) }
            }
        }
        let page: [String: Any] = ["ok": true, "messages": [[
            "id": "remote-image-message", "role": mode == "user" ? "user" : "assistant", "created": 1,
            "completed": 2, "active": false, "parts": [
                ["id": "before-image", "kind": "text", "text": "截图前的说明"],
                ["id": "remote-image-part", "kind": "image", "mimeType": "image/png",
                 "path": "images/not-on-this-phone.png", "filename": "screenshot.png",
                 "remotePartId": "remote-image-part", "remoteImage": image],
                ["id": "after-image", "kind": "text", "text": "截图后的说明"]
            ]
        ]]]
        applyUITestMutation(.remoteSessions([remote]))
        if let data = try? JSONSerialization.data(withJSONObject: page) {
            applyUITestMutation(.remoteImagePage(data, remote))
        }
        configured = true
        ready = true
    }

    /// Run before opening the transcript, so only explicit calls exercise the
    /// real loader/cache and no visible tile can race its own request counter.
    func runRemoteImagePolicyUITestChecks() async {
        guard ProcessInfo.processInfo.arguments.contains("--ai-remote-image-ui-test"),
              let original = messages.first, let remote = selectedRemote,
              let source = uiTestSupport.remoteImageSources.values.first else { return }
        uiTestSupport.remoteImagePolicyResult = "运行中"
        let originalAccount = uiTestSupport.screenFrameAccountID
        let originalHandler = uiTestSupport.remoteImageDownloadHandler
        var checks = 0
        func require(_ condition: Bool, _ detail: String) throws {
            guard condition else { throw AIBackendError(message: detail) }
            checks += 1
        }
        func page(attachment: String, expires: Int64, login: String? = nil,
                  room: String? = nil, session: String? = nil, partID: String = "policy-image-part") throws
            -> (AIRemoteImageOwner, AIPart) {
            let sessionID = session ?? remote.sessionID
            let owner = AIRemoteSession(sessionID: sessionID, title: remote.title, updatedAt: 1, busy: false,
                deviceSlot: remote.deviceSlot, deviceName: remote.deviceName, deviceID: remote.deviceID,
                loginSessionID: remote.loginSessionID, online: false)
            let size = try source.resourceValues(forKeys: [.fileSizeKey]).fileSize!
            let object: [String: Any] = ["ok": true, "messages": [[
                "id": "policy-image-message", "role": "assistant", "created": 1, "completed": 2,
                "active": false, "parts": [["id": partID, "kind": "image", "mimeType": "image/png",
                    "remotePartId": partID, "remoteImage": ["state": "ready", "retention": "temporary",
                        "attachment_id": attachment, "owner_login_session_id": login ?? remote.loginSessionID,
                        "room_key": room ?? remote.deviceSlot + ":" + sessionID, "name": "screenshot.png",
                        "media_type": "image", "content_type": "image/png", "size_bytes": size,
                        "expires_at": expires]]]
            ]]]
            applyUITestMutation(.remoteSessions([owner]))
            applyUITestMutation(.remoteImagePage(try JSONSerialization.data(withJSONObject: object), owner))
            guard let identity = remoteImageOwner, let part = messages.first?.parts.first else {
                throw AIBackendError(message: "Wire image projection was lost")
            }
            return (identity, part)
        }
        func load(_ pair: (AIRemoteImageOwner, AIPart)) async throws -> URL {
            try await loadRemoteImage(owner: pair.0, messageID: "policy-image-message", part: pair.1)
        }
        func expectExpired(_ pair: (AIRemoteImageOwner, AIPart)) async throws {
            do { _ = try await load(pair); throw AIBackendError(message: "Expired image unexpectedly succeeded") }
            catch AIRemoteImageError.expired { checks += 1 }
        }
        do {
            let id = UUID().uuidString
            uiTestSupport.remoteImageSources[id] = source
            let expiry = Int64(Date().timeIntervalSince1970 * 1_000) + 2_000
            let first = try page(attachment: id, expires: expiry)
            let count = uiTestSupport.remoteImageDownloadCount
            let cached = try await load(first)
            try require(uiTestSupport.remoteImageDownloadCount == count + 1, "Initial image did not use exactly one download")
            let expected = try Data(contentsOf: source)
            try require(try Data(contentsOf: cached) == expected, "Cached pixels differ from the source fixture")
            let wait = max(0, expiry + 100 - Int64(Date().timeIntervalSince1970 * 1_000))
            try await Task.sleep(for: .milliseconds(wait))
            try require(try await load(first) == cached, "Complete cache was not retained after expiry")
            try require(uiTestSupport.remoteImageDownloadCount == count + 1, "Expired cached image fetched again")
            let uncached = try page(attachment: UUID().uuidString, expires: expiry)
            try await expectExpired(uncached)
            try require(uiTestSupport.remoteImageDownloadCount == count + 1, "Expired uncached image made a request")
            try Data([0]).write(to: cached, options: .atomic)
            let corrupt = try page(attachment: id, expires: expiry)
            try await expectExpired(corrupt)
            try require(uiTestSupport.remoteImageDownloadCount == count + 1, "Incomplete cache was fetched after expiry")

            let future = Int64(Date().addingTimeInterval(86_400).timeIntervalSince1970 * 1_000)
            let other = UUID().uuidString; uiTestSupport.remoteImageSources[other] = source
            let base = try page(attachment: other, expires: future)
            let basePath = try await load(base)
            let beforeNamespaces = uiTestSupport.remoteImageDownloadCount
            let newLogin = try page(attachment: other, expires: future, login: "another-original-login")
            let loginPath = try await load(newLogin)
            try require(loginPath != basePath && uiTestSupport.remoteImageDownloadCount == beforeNamespaces + 1,
                        "Original owner login shared a cache entry")
            uiTestSupport.screenFrameAccountID = "another-" + (originalAccount ?? "fixture")
            let newAccount = try page(attachment: other, expires: future)
            let accountPath = try await load(newAccount)
            try require(accountPath != basePath && uiTestSupport.remoteImageDownloadCount == beforeNamespaces + 2,
                        "Account switch shared image bytes")
            uiTestSupport.screenFrameAccountID = originalAccount
            let anotherPart = try page(attachment: other, expires: future, partID: "another-policy-part")
            try require(try await load(anotherPart) != basePath, "Another part reused the first part's cache")
            let anotherSession = try page(attachment: other, expires: future, session: "another-policy-session")
            try require(try await load(anotherSession) != basePath, "Another Session reused the first Session's cache")
            let invalid = try page(attachment: other, expires: future, room: "mac:wrong-room")
            let beforeInvalid = uiTestSupport.remoteImageDownloadCount
            do { _ = try await load(invalid); throw AIBackendError(message: "Wrong room binding was accepted") }
            catch AIRemoteImageError.unavailable { checks += 1 }
            try require(uiTestSupport.remoteImageDownloadCount == beforeInvalid, "Wrong room reached the downloader")

            // Ignore cancellation at the test boundary, then release the late
            // result after changing account. The production post-await guard and
            // staging cleanup must still prevent publication.
            let delayed = try page(attachment: UUID().uuidString, expires: future)
            var gate: CheckedContinuation<Void, Never>?
            var staging: URL?
            uiTestSupport.remoteImageDownloadHandler = { _, target in
                staging = target
                await withCheckedContinuation { gate = $0 }
                try FileManager.default.copyItem(at: source, to: target)
            }
            let pending = Task { try await load(delayed) }
            let deadline = Date().addingTimeInterval(2)
            while gate == nil && Date() < deadline { try await Task.sleep(for: .milliseconds(10)) }
            guard let release = gate else { pending.cancel(); throw AIBackendError(message: "Delayed downloader did not start") }
            pending.cancel()
            uiTestSupport.screenFrameAccountID = "canceled-" + (originalAccount ?? "fixture")
            release.resume()
            do { _ = try await pending.value; throw AIBackendError(message: "Canceled late image was published") }
            catch is CancellationError { checks += 1 }
            try require(staging.map { !FileManager.default.fileExists(atPath: $0.deletingLastPathComponent().path) } == true,
                        "Canceled image staging directory remained")
            uiTestSupport.screenFrameAccountID = originalAccount
            // Also reject late results without cancellation: neither a new
            // reference at the same part nor another account may inherit them.
            for changeAccount in [false, true] {
                let stale = try page(attachment: UUID().uuidString, expires: future)
                var lateGate: CheckedContinuation<Void, Never>?
                var lateStage: URL?
                uiTestSupport.remoteImageDownloadHandler = { _, target in
                    lateStage = target
                    await withCheckedContinuation { lateGate = $0 }
                    try FileManager.default.copyItem(at: source, to: target)
                }
                let late = Task { try await load(stale) }
                let deadline = Date().addingTimeInterval(2)
                while lateGate == nil && Date() < deadline { try await Task.sleep(for: .milliseconds(10)) }
                guard let release = lateGate else { late.cancel(); throw AIBackendError(message: "Late downloader did not start") }
                if changeAccount { uiTestSupport.screenFrameAccountID = "late-" + (originalAccount ?? "fixture") }
                else { _ = try page(attachment: UUID().uuidString, expires: future) }
                release.resume()
                do { _ = try await late.value; throw AIBackendError(message: "Stale callback was accepted") }
                catch AIRemoteImageError.changed { checks += 1 }
                try require(lateStage.map { !FileManager.default.fileExists(atPath: $0.deletingLastPathComponent().path) } == true,
                            "Late callback left an incomplete cache directory")
                uiTestSupport.screenFrameAccountID = originalAccount
            }
            uiTestSupport.remoteImagePolicyResult = "PASS \(checks)"
        } catch { uiTestSupport.remoteImagePolicyResult = "FAIL \(checks): \(error.localizedDescription)" }
        uiTestSupport.screenFrameAccountID = originalAccount
        uiTestSupport.remoteImageDownloadHandler = originalHandler
        applyUITestMutation(.remoteSessions([remote]))
        let encoder = JSONEncoder()
        if let messageData = try? encoder.encode([original]),
           let object = try? JSONSerialization.jsonObject(with: messageData),
           let data = try? JSONSerialization.data(withJSONObject: ["ok": true, "messages": object]) {
            applyUITestMutation(.remoteImagePage(data, remote))
        }
    }

    /// Load the unmodified C ABI snapshot exported after a real native Agent
    /// turn. Only the isolated workspace changes; message IDs, text, and tool
    /// receipts come from the snapshot and survive the normal Codable round-trip.
    @discardableResult
    func installNativePhotoReplySnapshotForUITesting(_ data: Data, sessionID: String,
                                                     workspacePath: String) -> Bool {
        guard AINativePhotoReplyUITestSupport.isRequested,
              let response = try? JSONDecoder().decode(AIResponse.self, from: data), response.ok,
              let sourceSessions = response.sessions, sourceSessions.contains(where: { $0.id == sessionID }),
              let sourceMessages = response.messages, !sourceMessages.isEmpty,
              let stored = try? JSONEncoder().encode(sourceMessages),
              let restored = try? JSONDecoder().decode([AIMessage].self, from: stored),
              restored == sourceMessages else { return false }
        applyUITestMutation(.workspace(workspacePath))
        applyUITestMutation(.clearRemoteSelection)
        sessions = sourceSessions
        applyUITestMutation(.selectedHistory(sessionID, restored.sorted { $0.id < $1.id }))
        configured = true
        ready = true
        return true
    }

    /// The host receipt comes from the real manual-photo execution. Round-trip
    /// the persisted part representation so a restored result uses the same row.
    @discardableResult
    func installManualPhotoToolReceiptForUITesting(_ output: String, in sessionID: String) -> Bool {
        guard AICameraUITestSupport.isManualToolRequested, selected == sessionID else { return false }
        let part = AIPart(id: "manual-photo-result-part", kind: "tool", tool: "mobile_take_photo",
                          input: "{}", output: output, state: "completed")
        let completed = AIMessage(id: "manual-photo-result-message", role: "assistant", created: 1,
                                  completed: 2, active: false, parts: [part])
        guard let data = try? JSONEncoder().encode(completed),
              let restored = try? JSONDecoder().decode(AIMessage.self, from: data) else { return false }
        applyUITestMutation(.selectedHistory(sessionID, [restored]))
        return restored == completed
    }

    func installSteeringUITestFixture(
        handler: @escaping (AISteeringRequest) async throws -> String
    ) {
        let workspace = FileManager.default.temporaryDirectory
            .appendingPathComponent("steering-outbox-" + ProcessInfo.processInfo.environment[
                "MAICHAT_AI_TEST_ID", default: UUID().uuidString], isDirectory: true)
            .appendingPathComponent("Workspace", isDirectory: true)
        try? FileManager.default.createDirectory(at: workspace, withIntermediateDirectories: true)
        applyUITestMutation(.workspace(workspace.path))
        uiTestSupport.sessionSwitchEnabled = true
        uiTestSupport.steeringHandler = handler
        uiTestSupport.screenFrameAccountID = "steering-fixture-owner"
        sessions = [AISession(id: "switch-A", title: "正在整理素材", busy: true,
                              turnId: "msg_fixture_turn_A"),
                    AISession(id: "switch-B", title: "另一个对话", busy: false)]
        selected = "switch-A"
        messages = sessionSwitchUITestMessages("A")
        configured = true
        ready = true
        historyLoaded = true
    }

    /// Exercise the real message projection, composer and durable answer queue.
    /// Only the final native receipt is replaced; no model or user history is used.
    func installQuestionConversationUITestFixture(
        handler: @escaping (AIQuestionAnswerUITestRequest) async throws -> String
    ) {
        installSteeringUITestFixture { _ in
            throw AIBackendError(message: "Question answers must not become steering messages")
        }
        uiTestSupport.questionHandler = handler
        let question = AIQuestion(id: "question-fixture", question: "复现测试问题：选择哪份素材？",
            options: ["第一份本地素材", "第二份本地素材"], sessionId: "switch-A",
            messageId: "question-owner", partId: "question-part", asked: 1000)
        let input = try! JSONSerialization.data(withJSONObject: [
            "question": question.question, "options": question.options])
        questions = [question]
        messages = [AIMessage(id: "question-owner", role: "assistant", created: 900,
            completed: 0, active: true, parts: [
                AIPart(id: "question-intro", kind: "text", text: "请在下方聊天输入框直接回答。"),
                AIPart(id: "question-part", kind: "tool", tool: "question",
                    input: String(decoding: input, as: UTF8.self), state: "running")])]
    }

    func installScreenFrameUITestWorkspace(_ path: String, accountID: String) {
        guard ProcessInfo.processInfo.arguments.contains("--ai-screen-frames-ui-test") else { return }
        applyUITestMutation(.workspace(path))
        uiTestSupport.screenFrameAccountID = accountID
    }

    func installImageZoomUITestFixture(_ path: String) {
        let session = "image-zoom-ui-test"
        sessions = [AISession(id: session, title: "图片预览测试", busy: false)]
        selected = session
        let object = ["delivery": "current_ai_session", "path": (path as NSString).lastPathComponent,
                      "type": "image", "mime_type": "image/png", "caption": "图片预览"]
        guard let data = try? JSONSerialization.data(withJSONObject: object),
              let output = String(data: data, encoding: .utf8) else { return }
        let part = AIPart(id: "image-zoom-part", kind: "tool", text: nil,
                          tool: "agent_send_media", input: "{}", output: output,
                          error: nil, state: "completed",
                          path: nil, mimeType: nil)
        messages = [AIMessage(id: "image-zoom-message", role: "assistant", created: 0,
                              completed: 1, active: false, parts: [part])]
        applyUITestMutation(.workspace((path as NSString).deletingLastPathComponent))
        ready = true
    }

    // 固定片段身份逐步追加正文和工具，走正常 Root/消息行而不执行模型或工具。
    func installToolOrderUITestFixture(stage: Int) {
        let session = "tool-order-ui-test"
        func text(_ id: String, _ value: String) -> AIPart {
            AIPart(id: id, kind: "text", text: value)
        }
        func tool(_ id: String, _ name: String, _ result: String, running: Bool = false) -> AIPart {
            AIPart(id: id, kind: "tool", tool: name, input: "{}", output: result,
                   state: running ? "running" : "completed")
        }
        var parts = [text("intro", "先检查文件，再执行处理。"),
                     tool("tool-a1", "file_read", "A1 执行结果"),
                     tool("tool-a2", "file_stat", "A2 执行结果"),
                     text("answer-a", "A 段说明：文件检查已经完成。")]
        if stage > 0 {
            parts.append(tool("tool-b1", "shell", stage == 1 ? "" : "B1 执行结果", running: stage == 1))
        }
        if stage > 1 {
            parts += [text("answer-b", "B 段说明：处理结果已经核对。"),
                      tool("tool-c1", "file_write", "C1 执行结果"),
                      tool("tool-c2", "shell", "C2 执行结果"),
                      text("answer-c", "C 段说明：全部完成。")]
        }
        sessions = [AISession(id: session, title: "工具与正文顺序", busy: stage < 2)]
        selected = session
        messages = [AIMessage(id: "tool-order-message", role: "assistant", created: 1000,
                              completed: stage < 2 ? 0 : 5000, active: stage < 2, parts: parts)]
        configured = true
        ready = true
        historyLoaded = true
    }

    func installDrawerFontUITestFixture() {
        installHistoryUITestFixture(markdown: "偏好设置")
        sessions = [AISession(id: "history-ui-test", title: "偏好设置", busy: false)]
        applyUITestMutation(.remoteSessions([AIRemoteSession(
            sessionID: "drawer-remote", title: "偏好设置", updatedAt: 1793880000000, busy: false,
            deviceSlot: "mac", deviceName: "字体测试 Mac", deviceID: "drawer-font-mac",
            loginSessionID: "drawer-font-login", online: false)]))
    }

    func installHistoryUITestFixture(markdown: String? = nil) {
        let session = "history-ui-test"
        sessions = [AISession(id: session, title: "历史会话", busy: false)]
        selected = session
        messages = (0..<80).map { index in
            let part = AIPart(id: "history-part-\(index)", kind: "text",
                              text: index == 79 ? (markdown ?? "历史消息 \(index)\n第二行内容")
                                                : "历史消息 \(index)\n第二行内容", tool: nil,
                              input: nil, output: nil, error: nil, state: nil,
                              path: nil, mimeType: nil)
            return AIMessage(id: "history-message-\(index)", role: "assistant",
                             created: Int64(index), completed: Int64(index + 1),
                             active: false, parts: [part])
        }
        ready = true
    }

    func installStreamingTypingUITestFixture() {
        let session = "stream-typing-ui-test"
        sessions = [AISession(id: session, title: "流式输入测试", busy: true)]
        selected = session
        messages = (0..<30).map { index in
            let part = AIPart(id: "stream-old-part-\(index)", kind: "text",
                              text: "先前的消息 \(index)", tool: nil,
                              input: nil, output: nil, error: nil, state: nil,
                              path: nil, mimeType: nil)
            return AIMessage(id: String(format: "stream-message-%02d", index),
                             role: "assistant", created: Int64(index),
                             completed: Int64(index + 1), active: false, parts: [part])
        }
        ready = true
        historyLoaded = true
        Task { @MainActor [weak self] in
            for index in 0..<60 {
                try? await Task.sleep(for: .milliseconds(100))
                guard let self, self.selected == session else { return }
                let part = AIPart(id: "stream-active-part", kind: "text",
                                  text: String(repeating: "正在输出内容。", count: index + 1),
                                  tool: nil, input: nil, output: nil, error: nil, state: nil,
                                  path: nil, mimeType: nil)
                let active = AIMessage(id: "stream-message-30", role: "assistant",
                                       created: 30, completed: 0, active: true, parts: [part])
                if self.messages.count == 30 { self.messages.append(active) }
                else { self.messages[30] = active }
            }
        }
    }

    func installSessionSwitchUITestFixture() {
        uiTestSupport.sessionSwitchEnabled = true
        sessions = ["A", "B", "C"].map {
            AISession(id: "switch-\($0)", title: "对话 \($0)", busy: false)
        }
        applyUITestMutation(.remoteSessions([AIRemoteSession(
            sessionID: "remote-demo", title: "Mac 上的编译任务", updatedAt: nil, busy: false,
            deviceSlot: "mac", deviceName: "工作 Mac", deviceID: "fixturemac",
            loginSessionID: "fixturelogin", online: false)]))
        selected = "switch-A"
        messages = sessionSwitchUITestMessages("A")
        ready = true
        historyLoaded = true
    }

    func installWelcomeUITestFixture() {
        sessions = []
        selected = ""
        applyUITestMutation(.clearRemoteSelection)
        messages = []
        configured = true
        ready = true
        historyLoaded = true
    }

    func installMediaDraftUITestFixture() {
        installSessionSwitchUITestFixture()
        let workspace = FileManager.default.temporaryDirectory
            .appendingPathComponent("ai-media-draft-ui-test", isDirectory: true)
        guard let video = Bundle.main.url(forResource: "ffplay-sample", withExtension: "mp4"),
              (try? FileManager.default.createDirectory(
                at: workspace, withIntermediateDirectories: true
              )) != nil else { return }
        let image = UIGraphicsImageRenderer(size: CGSize(width: 220, height: 160))
            .image { context in
                UIColor.systemBlue.setFill()
                context.fill(CGRect(x: 0, y: 0, width: 220, height: 160))
                UIColor.white.setFill()
                context.fill(CGRect(x: 45, y: 45, width: 130, height: 70))
            }
        let imageFile = workspace.appendingPathComponent("draft-image.png")
        let emptyDraftImageFile = workspace.appendingPathComponent("empty-draft-image.png")
        let videoFile = workspace.appendingPathComponent("draft-video.mp4")
        guard let imageData = image.pngData(),
              (try? imageData.write(to: imageFile, options: .atomic)) != nil,
              (try? imageData.write(to: emptyDraftImageFile, options: .atomic)) != nil else {
            return
        }
        if !FileManager.default.fileExists(atPath: videoFile.path) {
            try? FileManager.default.copyItem(at: video, to: videoFile)
        }
        applyUITestMutation(.workspace(workspace.path))
        var attachments: [String: [AIImportedFile]] = [:]
        attachments[selected] = [
            AIImportedFile(relativePath: imageFile.lastPathComponent,
                           mimeType: "image/png", isImage: true),
            AIImportedFile(relativePath: videoFile.lastPathComponent,
                           mimeType: "video/mp4", isImage: false)
        ]
        attachments[""] = [
            AIImportedFile(relativePath: emptyDraftImageFile.lastPathComponent,
                           mimeType: "image/png", isImage: true)
        ]
        // 模拟过去的首发目标仍存在，检验手动回到空草稿再选 A 不会误迁移文字。
        applyUITestMutation(.attachments(attachments, firstMessageSessionID: "switch-A"))
    }

    private func sessionSwitchUITestMessages(_ title: String) -> [AIMessage] {
        (0..<80).map { index in
            let part = AIPart(id: "switch-part-\(title)-\(index)", kind: "text",
                              text: "\(title) 历史消息 \(index)\n第二行内容", tool: nil,
                              input: nil, output: nil, error: nil, state: nil,
                              path: nil, mimeType: nil)
            return AIMessage(id: "switch-\(title)-message-\(index)", role: "assistant",
                             created: Int64(index), completed: Int64(index + 1),
                             active: false, parts: [part])
        }
    }

    func selectUITestConversationIfNeeded(_ id: String) -> Bool {
        guard uiTestSupport.sessionSwitchEnabled else { return false }
        applyUITestMutation(.selectedHistory(id, sessionSwitchUITestMessages(String(id.suffix(1)))))
        return true
    }
}
#endif

#endif
