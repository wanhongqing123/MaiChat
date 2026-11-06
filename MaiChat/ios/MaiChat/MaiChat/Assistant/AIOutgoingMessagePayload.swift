// 文件作用：保存发送队列的文本、引用和媒体描述；文件仍在原工作区，不把媒体字节写进队列。
import Foundation
import MaiChatCore

struct AIOutgoingMessagePayload: Codable, Sendable {
    struct RemoteOwner: Codable, Sendable {
        let slot: String
        let login: String
        let session: String
    }

    struct File: Codable, Sendable {
        struct Archive: Codable, Sendable {
            let ownerSessionID: String
            let batchID: String
            let referenceID: String
            let frameID: String
            let capturedAtMS: Int64
            let elapsedMS: Int64
            let sizeBytes: Int64
            let width: Int
            let height: Int

            init(_ reference: AIMobileScreenArchiveReference) {
                ownerSessionID = reference.context.ownerSessionID
                batchID = reference.context.batchID
                referenceID = reference.referenceID
                frameID = reference.frame.id
                capturedAtMS = reference.frame.capturedAtMS
                elapsedMS = reference.frame.elapsedMS
                sizeBytes = reference.frame.sizeBytes
                width = reference.frame.width
                height = reference.frame.height
            }
        }

        let path: String
        let mimeType: String
        let isImage: Bool
        let archive: Archive?

        init(_ file: AIImportedFile) {
            path = file.relativePath
            mimeType = file.mimeType
            isImage = file.isImage
            archive = file.archiveReference.map(Archive.init)
        }

        func imported(workspace: String) throws -> AIImportedFile {
            guard !path.isEmpty, !(path as NSString).isAbsolutePath, !path.contains("\0"),
                  let source = AIAssistantPathPolicy.resolve(path, workspacePath: workspace) else {
                throw AIBackendError(message: "排队附件路径无效")
            }
            let reference: AIMobileScreenArchiveReference?
            if let archive {
                let context = AIMobileScreenArchiveContext(workspace: URL(fileURLWithPath: workspace),
                    ownerSessionID: archive.ownerSessionID, batchID: archive.batchID)
                let frame = AIMobileScreenArchiveFrame(id: archive.frameID, batchID: archive.batchID,
                    capturedAtMS: archive.capturedAtMS, elapsedMS: archive.elapsedMS,
                    sizeBytes: archive.sizeBytes, width: archive.width, height: archive.height, path: path)
                reference = AIMobileScreenArchiveReference(context: context, frame: frame,
                    referenceID: archive.referenceID, fileURL: source)
            } else { reference = nil }
            return AIImportedFile(relativePath: path, mimeType: mimeType,
                                  isImage: isImage, archiveReference: reference)
        }
    }

    let text: String
    let quotedMessageID: String
    var files: [File]
    let localSession: String
    let emptyDraftToken: UUID?
    let remote: RemoteOwner?
    // 旧 outbox 无此可选字段仍可解码；问题编号在点击时固定，不在派发时重新选择问题。
    var questionID: String? = nil
    var question: AIQuestion? = nil
    // 派发前持久化稳定服务器附件引用；未知回执重试不重新上传或更改原请求。
    var remoteAttachmentIDs: [String] = []
    var archivePinsPrepared = false

    func importedFiles(workspace: String) throws -> [AIImportedFile] {
        try files.map { try $0.imported(workspace: workspace) }
    }
}

struct AIOutgoingMessageRow: Identifiable {
    let entry: AIOutgoingMessageQueueEntry
    let payload: AIOutgoingMessagePayload
    let allowsRetry: Bool
    var id: UUID { entry.id }
    var status: String {
        switch entry.state {
        case .queued: "排队中"
        case .dispatching: "正在发送"
        case .unknown: "送达待确认"
        case .failed: "发送失败，内容已保留"
        case .accepted: "已送达"
        }
    }
}
