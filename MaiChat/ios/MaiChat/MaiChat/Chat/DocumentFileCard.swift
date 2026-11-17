// 文件作用：IM 和创作中心共用的 PDF、Markdown、HTML 内容缩略图。
// 只读取宿主已交付的本地附件；完整文件预览、下载与消息持久化仍由原入口负责。
import CryptoKit
import Darwin
import MaiChatCore
import PDFKit
import SwiftUI
import UIKit
import WebKit

struct DocumentFileCardRequest: Sendable {
    let attachment: RemoteIMFileAttachment
    let identity: String
    private let artifact: AIAssistantFileArtifact?
    private let workspace: String

    init(file: AIAssistantFileArtifact, workspace: String, partID: String) {
        attachment = RemoteIMFileAttachment(localFilePath: file.url.path, fileName: file.name,
            mimeType: file.mimeType, sizeBytes: Int(file.bytes))
        identity = "agent|" + workspace + "|" + partID
        artifact = file
        self.workspace = workspace
    }

    init(attachment: RemoteIMFileAttachment, owner: String, messageID: UUID) {
        self.attachment = attachment
        identity = "im|" + owner + "|" + messageID.uuidString
        artifact = nil
        workspace = ""
    }

    // 工作区产物保留原严格策略。IM 只读应用沙盒中的附件；SDK 文件名与磁盘 UUID
    // 可以不同，不能把显示名当磁盘名。所有路径及实际 fd 身份均在后台再次核实。
    func validatedURL() -> URL? {
        if let artifact { return AIAssistantFileArtifactPolicy.existingURL(artifact, workspacePath: workspace) }
        guard !attachment.localFilePath.isEmpty else { return nil }
        let url = URL(fileURLWithPath: attachment.localFilePath).standardizedFileURL.resolvingSymlinksInPath()
        let root = URL(fileURLWithPath: NSHomeDirectory()).resolvingSymlinksInPath().path
        guard url.path.hasPrefix(root + "/"),
              let info = try? url.resourceValues(forKeys: [.isRegularFileKey, .fileSizeKey]),
              info.isRegularFile == true,
              attachment.sizeBytes == nil || info.fileSize == attachment.sizeBytes else { return nil }
        return url
    }
}

private enum DocumentThumbnail {
    enum Kind: String, Sendable { case pdf, markdown, html }
    struct Source: Sendable {
        let kind: Kind
        let data: Data
        let version: String
        let key: String
    }
    enum Failure: Error { case unavailable }
    static let canvas = CGSize(width: 300, height: 200)

    static func kind(_ attachment: RemoteIMFileAttachment) -> Kind? {
        let ext = (attachment.fileName as NSString).pathExtension.lowercased()
        let mime = attachment.mimeType.split(separator: ";").first?.trimmingCharacters(in: .whitespaces).lowercased() ?? ""
        if ext == "pdf", mime == "application/pdf" { return .pdf }
        if ["md", "markdown"].contains(ext), ["text/markdown", "text/x-markdown", "text/plain"].contains(mime) { return .markdown }
        if ["html", "htm"].contains(ext), ["text/html", "application/xhtml+xml"].contains(mime) { return .html }
        return nil
    }

    private static func open(_ request: DocumentFileCardRequest) throws -> (FileHandle, String) {
        guard let url = request.validatedURL() else { throw Failure.unavailable }
        let fd = Darwin.open(url.path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW)
        guard fd >= 0 else { throw Failure.unavailable }
        let handle = FileHandle(fileDescriptor: fd, closeOnDealloc: true)
        var info = stat()
        var path = [CChar](repeating: 0, count: Int(MAXPATHLEN))
        let pathOK = path.withUnsafeMutableBufferPointer { fcntl(fd, F_GETPATH, $0.baseAddress!) == 0 }
        guard fstat(fd, &info) == 0, (info.st_mode & S_IFMT) == S_IFREG,
              request.attachment.sizeBytes == nil || info.st_size == Int64(request.attachment.sizeBytes!),
              pathOK, URL(fileURLWithPath: String(cString: path)).standardizedFileURL.resolvingSymlinksInPath() == url,
              request.validatedURL() == url else {
            try? handle.close()
            throw Failure.unavailable
        }
        let version = "\(info.st_dev):\(info.st_ino):\(info.st_size):" +
            "\(info.st_mtimespec.tv_sec):\(info.st_mtimespec.tv_nsec):" +
            "\(info.st_ctimespec.tv_sec):\(info.st_ctimespec.tv_nsec)"
        return (handle, version)
    }

    static func isCurrent(_ request: DocumentFileCardRequest, version: String) -> Bool {
        guard let (handle, current) = try? open(request) else { return false }
        defer { try? handle.close() }
        return current == version
    }

    static func source(_ request: DocumentFileCardRequest) throws -> Source {
        guard let kind = kind(request.attachment) else { throw Failure.unavailable }
        // 覆盖真实目录报告等常见文档；仍完整解析且受2/8许可约束，不截断正文。
        let limit = kind == .pdf ? 24 * 1024 * 1024 : kind == .html ? 512 * 1024 : 256 * 1024
        if let bytes = request.attachment.sizeBytes, bytes > limit { throw Failure.unavailable }
        let (handle, version) = try open(request)
        defer { try? handle.close() }
        var info = stat()
        guard fstat(handle.fileDescriptor, &info) == 0, info.st_size > 0, info.st_size <= limit else { throw Failure.unavailable }
        // 所有类型都完整读取；尾表/样式/定义可能改变首屏，不截前缀后重排。
        let data = try handle.read(upToCount: limit) ?? Data()
        guard Int64(data.count) == info.st_size, isCurrent(request, version: version) else { throw Failure.unavailable }
        let digest = SHA256.hash(data: data).map { String(format: "%02x", $0) }.joined()
        if RemoteIMGitDiffDisplayPolicy.isGitDiff(request.attachment),
           let expected = RemoteIMGitDiffDisplayPolicy.expectedSHA256(fileName: request.attachment.fileName),
           digest != expected { throw Failure.unavailable }
        return Source(kind: kind, data: data, version: version,
            key: request.identity + "|" + request.attachment.localFilePath + "|" + kind.rawValue + "|" + version + "|" + digest)
    }

    static func pdf(_ data: Data) -> Data? {
        guard data.count <= 24 * 1024 * 1024, let document = PDFDocument(data: data), !document.isLocked,
              let page = document.page(at: 0) else { return nil }
        return page.thumbnail(of: CGSize(width: 600, height: 400), for: .cropBox).pngData()
    }

    static func text(_ data: Data) -> String? {
        guard let text = String(data: data, encoding: .utf8), !text.contains("\0") else { return nil }
        return text
    }
}

// 许可覆盖完整的“读文件→后台解析/原生 HTML 排版→版本校验→发布”生命周期。
// 等待项不读取文件；取消移除 UUID ticket，许可交接取消只能归还一次。
@MainActor
private final class DocumentThumbnailSlots {
    enum Failure: Error { case overloaded }
    static let shared = DocumentThumbnailSlots()
    private var active = 0
    private var waiting: [(id: UUID, continuation: CheckedContinuation<Void, Error>)] = []
    func acquire() async throws {
        try Task.checkCancellation()
        if active < 2 { active += 1; return }
        guard waiting.count < 8 else { throw Failure.overloaded }
        let id = UUID()
        try await withTaskCancellationHandler {
            try await withCheckedThrowingContinuation { (continuation: CheckedContinuation<Void, Error>) in
                waiting.append((id, continuation))
            }
        } onCancel: { Task { @MainActor in self.cancel(id) } }
        do { try Task.checkCancellation() }
        catch { release(); throw error }
    }
    func release() {
        if !waiting.isEmpty { waiting.removeFirst().continuation.resume() }
        else { precondition(active > 0); active -= 1 }
    }
    private func cancel(_ id: UUID) {
        guard let index = waiting.firstIndex(where: { $0.id == id }) else { return }
        waiting.remove(at: index).continuation.resume(throwing: CancellationError())
    }
}

@MainActor
private final class DocumentThumbnailImageCache {
    static let shared = DocumentThumbnailImageCache()
    private let images = NSCache<NSString, UIImage>()
    private init() { images.countLimit = 32; images.totalCostLimit = 16 * 1024 * 1024 }
    func image(_ key: String) -> UIImage? { images.object(forKey: key as NSString) }
    func insert(_ image: UIImage, key: String) {
        images.setObject(image, forKey: key as NSString,
            cost: image.cgImage.map { $0.bytesPerRow * $0.height } ?? 600 * 400 * 4)
    }
}

// WebKit只做本地静态排版：无JS、无共享cookie/持久store、无baseURL、无外部资源。
// createPDF捕获固定首屏后交回同一PDF raster，不依赖离屏GPU截图是否已合成。
@MainActor
private final class DocumentHTMLPage: NSObject, WKNavigationDelegate {
    private var webView: WKWebView?
    private var continuation: CheckedContinuation<Data?, Never>?
    private var deadline: Task<Void, Never>?
    private var rendering = false
    private var abandoned = false
    private var acceptedInitialNavigation = false

    func render(_ html: String) async -> Data? {
        if Task.isCancelled { return nil }
        return await withTaskCancellationHandler {
            await withCheckedContinuation { continuation in
                self.continuation = continuation
                let config = WKWebViewConfiguration()
                config.websiteDataStore = .nonPersistent()
                config.defaultWebpagePreferences.allowsContentJavaScript = false
                let view = WKWebView(frame: CGRect(origin: .zero, size: DocumentThumbnail.canvas), configuration: config)
                view.navigationDelegate = self
                view.isUserInteractionEnabled = false
                view.accessibilityElementsHidden = true
                view.scrollView.contentInsetAdjustmentBehavior = .never
                webView = view
                // CSP先于文件内容生效；文件自带更宽松的策略不能放宽这条策略。
                let policy = "default-src 'none'; style-src 'unsafe-inline'; img-src data:; font-src data:; script-src 'none'; connect-src 'none'; frame-src 'none'; object-src 'none'; form-action 'none'; base-uri 'none'"
                let prefix = "<meta http-equiv=\"Content-Security-Policy\" content=\"" + policy + "\"><meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
                view.loadHTMLString(prefix + html, baseURL: nil)
                deadline = Task { @MainActor [weak self] in
                    do { try await Task.sleep(nanoseconds: 8_000_000_000) } catch { return }
                    self?.cancel()
                }
            }
        } onCancel: { Task { @MainActor in self.cancel() } }
    }

    private func cancel() {
        abandoned = true
        webView?.stopLoading()
        // 已提交的原生PDF请求不可同步取消，等待真实callback后再释放上层许可。
        if !rendering { finish(nil) }
    }

    private func finish(_ data: Data?) {
        guard let pending = continuation else { return }
        continuation = nil
        deadline?.cancel(); deadline = nil
        webView?.navigationDelegate = nil
        webView?.stopLoading(); webView = nil
        pending.resume(returning: abandoned ? nil : data)
    }

    func webView(_ webView: WKWebView, decidePolicyFor navigationAction: WKNavigationAction,
                 decisionHandler: @escaping (WKNavigationActionPolicy) -> Void) {
        let initial = !acceptedInitialNavigation && navigationAction.request.url?.absoluteString == "about:blank"
        if initial { acceptedInitialNavigation = true }
        decisionHandler(initial ? .allow : .cancel)
    }
    func webView(_ webView: WKWebView, didFinish navigation: WKNavigation!) {
        guard continuation != nil, !abandoned, !rendering else { return }
        rendering = true
        let config = WKPDFConfiguration()
        config.rect = CGRect(origin: .zero, size: DocumentThumbnail.canvas)
        webView.createPDF(configuration: config) { [weak self] result in
            guard let self else { return }
            self.rendering = false
            self.finish(try? result.get())
        }
    }
    func webView(_ webView: WKWebView, didFail navigation: WKNavigation!, withError error: Error) { cancel() }
    func webView(_ webView: WKWebView, didFailProvisionalNavigation navigation: WKNavigation!, withError error: Error) { cancel() }
    func webViewWebContentProcessDidTerminate(_ webView: WKWebView) { rendering = false; finish(nil) }
}

struct DocumentFileCardContent: View {
    let request: DocumentFileCardRequest
    let thumbnailIdentifier: String
    var revision: UInt64 = 0
    var decorated = true
    @Environment(\.appTextScale) private var textScale
    @Environment(\.appFontFamily) private var family
    @Environment(\.scenePhase) private var scenePhase
    @State private var image: UIImage?
    @State private var failed = false
    @State private var loadedID: String?
    @State private var refresh: UInt64 = 0

    private var loadID: String {
        let attachment = request.attachment
        return [request.identity, attachment.localFilePath, attachment.fileName, attachment.mimeType,
                String(attachment.sizeBytes ?? -1), family.rawValue, String(Double(textScale)),
                String(revision), String(refresh)].joined(separator: "|")
    }
    var body: some View {
        VStack(alignment: .leading, spacing: 0) {
            if DocumentThumbnail.kind(request.attachment) != nil {
                Rectangle().fill(Color.white).aspectRatio(3.0 / 2.0, contentMode: .fit)
                    .overlay {
                        if loadedID == loadID, let image {
                            Image(uiImage: image).resizable().scaledToFit()
                                .accessibilityLabel("文档首屏预览").accessibilityIdentifier(thumbnailIdentifier)
                        } else if loadedID == loadID, failed {
                            Image(systemName: "doc.text").foregroundStyle(RemoteIMStyle.blue)
                                .font(AppTypography.symbol(size: 36))
                                .accessibilityLabel("文档预览不可用")
                                .accessibilityIdentifier(thumbnailIdentifier.replacingOccurrences(of: "thumbnail-", with: "thumbnail-unavailable-"))
                        } else { ProgressView().accessibilityLabel("正在生成文档预览") }
                    }.clipped()
            }
            FileBubbleContent(attachment: request.attachment).padding(decorated ? 12 : 0)
                .padding(.top, !decorated && DocumentThumbnail.kind(request.attachment) != nil ? 8 : 0)
        }
        .frame(maxWidth: 280, alignment: .leading)
        .background(decorated ? RemoteIMStyle.blueSoft : Color.clear)
        .clipShape(RoundedRectangle(cornerRadius: 12))
        .overlay(RoundedRectangle(cornerRadius: 12).stroke(decorated ? RemoteIMStyle.border : .clear, lineWidth: 1))
        .task(id: loadID) { await load() }
        .onChange(of: scenePhase) { phase in if phase == .active { refresh &+= 1 } }
    }

    @MainActor private func load() async {
        image = nil; failed = false; loadedID = nil
        guard DocumentThumbnail.kind(request.attachment) != nil else { return }
        do {
            try await DocumentThumbnailSlots.shared.acquire()
            defer { DocumentThumbnailSlots.shared.release() }
            try Task.checkCancellation()
            let source = try await RemoteIMBackgroundWork.file { try DocumentThumbnail.source(request) }
            try Task.checkCancellation()
            let key = [source.key, family.rawValue, String(Double(textScale))].joined(separator: "|")
            var result = DocumentThumbnailImageCache.shared.image(key)
            if result == nil {
                switch source.kind {
                case .pdf:
                    let png = try await RemoteIMBackgroundWork.parse { DocumentThumbnail.pdf(source.data) }
                    if let png { result = UIImage(data: png) }
                case .html:
                    if let text = DocumentThumbnail.text(source.data), let pdf = await DocumentHTMLPage().render(text) {
                        try Task.checkCancellation()
                        let png = try await RemoteIMBackgroundWork.parse { DocumentThumbnail.pdf(pdf) }
                        if let png { result = UIImage(data: png) }
                    }
                case .markdown:
                    if let text = DocumentThumbnail.text(source.data) {
                        let content = try await MarkdownLikeText.preparedDocument(text, bodyFont: AppTypography.detail)
                        try Task.checkCancellation()
                        #if DEBUG
                        if ProcessInfo.processInfo.environment["MAICHAT_DOCUMENT_EVICT"] == "1" { MarkdownPreparation.evictForThumbnailTest(text) }
                        #endif
                        let page = content.frame(width: 268, alignment: .topLeading).padding(16)
                            .frame(width: 300, height: 200, alignment: .topLeading).background(Color.white).clipped()
                            .environment(\.colorScheme, .light).environment(\.appTextScale, textScale).environment(\.appFontFamily, family)
                        let renderer = ImageRenderer(content: page)
                        renderer.scale = 2
                        renderer.proposedSize = ProposedViewSize(DocumentThumbnail.canvas)
                        result = renderer.uiImage
                    }
                }
            }
            try Task.checkCancellation()
            let current = try await RemoteIMBackgroundWork.metadata { DocumentThumbnail.isCurrent(request, version: source.version) }
            try Task.checkCancellation()
            guard current, let result else { failed = true; loadedID = loadID; return }
            DocumentThumbnailImageCache.shared.insert(result, key: key)
            image = result; loadedID = loadID
        } catch {
            if !Task.isCancelled { failed = true; loadedID = loadID }
        }
    }
}
