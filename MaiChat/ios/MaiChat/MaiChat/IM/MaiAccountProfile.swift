// 普通账号资料与头像服务；设置页和 Agent 复用，网络上传由共享 C++ 核心执行。
// 只输出公开资料，绝不把鉴权响应、临时签名地址或账号凭据交给工具调用方。
import Foundation
import ImageIO
import UIKit

struct MaiChatCurrentAccountProfile: Sendable {
    let loggedIn: Bool
    var accountID: String?
    var imUserID: String?
    var nickname: String?
    var phoneMasked: String?
    var avatarURL: String?

    var toolOutput: [String: Any] {
        guard loggedIn else { return ["logged_in": false] }
        return ["logged_in": true,
                "account_id": accountID ?? "", "im_user_id": imUserID ?? "",
                "nickname": nickname.map { $0 as Any } ?? NSNull(),
                "phone_masked": phoneMasked.flatMap {
                    $0.contains("*") && $0.count <= 32 ? $0 as Any : nil
                } ?? NSNull(),
                "avatar_url": avatarURL.map { $0 as Any } ?? NSNull()]
    }
}

enum MaiChatAvatarError: LocalizedError {
    case signInRequired, imUnavailable, invalidImage, inputTooLarge, busy
    case accountChanged, network, invalidResponse, sdkRejected

    var errorDescription: String? {
        switch self {
        case .signInRequired: return "请先登录 MaiChat 账号"
        case .imUnavailable: return "请先连接 IM，再修改头像"
        case .invalidImage: return "无法读取这张头像图片"
        case .inputTooLarge: return "头像原图不能超过 20 MB"
        case .busy: return "已有头像修改正在进行，请等待完成"
        case .accountChanged: return "登录账号已变化，请重新选择头像"
        case .network: return "头像上传失败，请检查网络后重试"
        case .invalidResponse: return "头像服务返回的结果无效"
        case .sdkRejected: return "IM 尚未确认头像修改，请稍后重试"
        }
    }
}

// 令牌由请求、工作线程及 SDK 回调共同持有，最后一个持有者退出后才释放 C 句柄。
// 主线程切账号或 Task 取消会同时关闭 SDK 写入闸门并取消尚未结束的网络传输。
final class MaiAvatarMutationRequest: @unchecked Sendable {
    let id = UUID()
    let externalID: String?
    let pointer: OpaquePointer
    private let lock = NSLock()
    private var canceled = false
    private var sdkDispatched = false
    private var sdkConfirmed = false
    private var cancellationHandlers: [@Sendable () -> Void] = []

    init?(externalID: String? = nil) {
        guard let pointer = mai_avatar_cancel_create() else { return nil }
        self.pointer = pointer
        self.externalID = externalID
    }
    deinit { mai_avatar_cancel_destroy(pointer) }
    func cancel() {
        lock.lock()
        canceled = true
        // 已派发给 SDK 的修改无法撤回，等它的真实确认，不能把成功误报为已取消。
        let handlers = sdkDispatched ? [] : cancellationHandlers
        cancellationHandlers.removeAll()
        lock.unlock()
        mai_avatar_cancel_request(pointer)
        for handler in handlers { handler() }
    }
    var isActive: Bool {
        lock.lock(); defer { lock.unlock() }
        return !canceled
    }
    func checkActive() throws {
        guard isActive else { throw CancellationError() }
    }
    func whenCanceled(_ action: @escaping @Sendable () -> Void) {
        lock.lock()
        let alreadyCanceled = canceled && !sdkDispatched
        if !alreadyCanceled { cancellationHandlers.append(action) }
        lock.unlock()
        if alreadyCanceled { action() }
    }
    func beginSDKMutation() -> Bool {
        lock.lock(); defer { lock.unlock() }
        guard !canceled else { return false }
        sdkDispatched = true
        return true
    }
    func confirmSDKMutation() {
        lock.lock(); sdkConfirmed = true; lock.unlock()
    }
    var wasConfirmedBySDK: Bool {
        lock.lock(); defer { lock.unlock() }
        return sdkConfirmed
    }
}

enum MaiAvatarImagePreparation {
    static let maximumInputBytes = 20 * 1024 * 1024

    // ImageIO 先按方向生成小图，避免把手机原图完整解码到巨大的 RGBA 缓冲区。
    static func jpeg(fileURL: URL) throws -> Data {
        let size = try fileURL.resourceValues(forKeys: [.fileSizeKey, .isRegularFileKey])
        guard size.isRegularFile == true, let bytes = size.fileSize, bytes > 0 else {
            throw MaiChatAvatarError.invalidImage
        }
        guard bytes <= maximumInputBytes else { throw MaiChatAvatarError.inputTooLarge }
        let data = try Data(contentsOf: fileURL, options: .mappedIfSafe)
        guard data.count <= maximumInputBytes else { throw MaiChatAvatarError.inputTooLarge }
        guard let source = CGImageSourceCreateWithData(data as CFData,
                                                       [kCGImageSourceShouldCache: false] as CFDictionary),
              let image = CGImageSourceCreateThumbnailAtIndex(source, 0, [
                kCGImageSourceCreateThumbnailFromImageAlways: true,
                kCGImageSourceCreateThumbnailWithTransform: true,
                kCGImageSourceThumbnailMaxPixelSize: 1024,
                kCGImageSourceShouldCacheImmediately: true
              ] as CFDictionary) else { throw MaiChatAvatarError.invalidImage }
        return try jpeg(image: UIImage(cgImage: image))
    }

    // 图片选择器已提供 UIImage；它与文件工具入口使用相同的方形裁切和输出参数。
    static func jpeg(image: UIImage) throws -> Data {
        guard image.size.width > 0, image.size.height > 0 else {
            throw MaiChatAvatarError.invalidImage
        }
        let format = UIGraphicsImageRendererFormat()
        format.scale = 1
        format.opaque = true
        let renderer = UIGraphicsImageRenderer(size: CGSize(width: 512, height: 512), format: format)
        let scale = max(512 / image.size.width, 512 / image.size.height)
        let width = image.size.width * scale, height = image.size.height * scale
        let normalized = renderer.image { context in
            UIColor.white.setFill()
            context.fill(CGRect(x: 0, y: 0, width: 512, height: 512))
            image.draw(in: CGRect(x: (512 - width) / 2, y: (512 - height) / 2,
                                  width: width, height: height))
        }
        guard let jpeg = normalized.jpegData(compressionQuality: 0.88),
              !jpeg.isEmpty, jpeg.count <= 2_000_000 else { throw MaiChatAvatarError.invalidImage }
        return jpeg
    }
}

protocol MaiAccountAvatarUploading: Sendable {
    func upload(jpeg: Data, accountID: String, request: MaiAvatarMutationRequest) async throws -> String
}

struct MaiAccountAvatarUploader: MaiAccountAvatarUploading {
    func upload(jpeg: Data, accountID: String, request: MaiAvatarMutationRequest) async throws -> String {
        try Task.checkCancellation()
        try request.checkActive()
        guard let access = try await MaiChatAuthClient.shared.mediaServiceAccessIfAvailable() else {
            throw MaiChatAvatarError.signInRequired
        }
        // 取凭据期间也可能退出或重新登录；已取消的旧请求不得开始签名或上传。
        try request.checkActive()
        guard let certificate = Bundle.main.url(forResource: "cacert", withExtension: "pem",
                                                 subdirectory: "MaiAgentCertificates") else {
            throw MaiChatAvatarError.invalidResponse
        }
        return try await withTaskCancellationHandler {
            try await withCheckedThrowingContinuation { (continuation: CheckedContinuation<String, Error>) in
                DispatchQueue.global(qos: .utility).async {
                    continuation.resume(with: Self.performUpload(jpeg: jpeg, accountID: accountID,
                        access: access, certificate: certificate.path, request: request))
                }
            }
        } onCancel: { request.cancel() }
    }

    private static func performUpload(jpeg: Data, accountID: String,
                                      access: MaiChatMediaServiceAccess, certificate: String,
                                      request: MaiAvatarMutationRequest) -> Result<String, Error> {
        guard request.isActive else { return .failure(CancellationError()) }
        let proxy = MaiChatAttachmentProxyContext()
        var result = MaiAvatarResult()
        defer { mai_avatar_result_free(&result) }
        access.baseURL.withCString { base in
            access.accountToken.withCString { token in
                accountID.withCString { account in
                    certificate.withCString { ca in
                        "image/jpeg".withCString { contentType in
                            var options = MaiAvatarOptions()
                            options.media_base_url_utf8 = base
                            options.account_token_utf8 = token
                            options.account_id_utf8 = account
                            options.ca_path_utf8 = ca
                            options.proxy_resolver = maiChatAttachmentProxyResolver
                            options.proxy_context = Unmanaged.passUnretained(proxy).toOpaque()
                            options.first_party_route_selector = maiChatSelectFirstPartyRoute
                            options.first_party_route_invalidator = maiChatInvalidateFirstPartyRoute
                            jpeg.withUnsafeBytes { bytes in
                                mai_avatar_upload(&options, bytes.bindMemory(to: UInt8.self).baseAddress,
                                                  jpeg.count, contentType, request.pointer, &result)
                            }
                        }
                    }
                }
            }
        }
        guard request.isActive, result.status != MAI_AVATAR_CANCELED else {
            return .failure(CancellationError())
        }
        guard result.status == MAI_AVATAR_OK, let url = result.avatar_url_utf8 else {
            return .failure(result.status == MAI_AVATAR_NETWORK_ERROR || result.status == MAI_AVATAR_HTTP_ERROR
                ? MaiChatAvatarError.network : MaiChatAvatarError.invalidResponse)
        }
        return .success(String(cString: url))
    }
}
