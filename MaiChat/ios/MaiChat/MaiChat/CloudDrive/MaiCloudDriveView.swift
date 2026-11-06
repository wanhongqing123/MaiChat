// Account-private media cloud drive. The page never exposes an upload publicly.
import ImageIO
import PhotosUI
import SwiftUI
import UniformTypeIdentifiers

private final class MaiCloudPhotoStream {
    let file: FileHandle
    init(url: URL) throws { file = try FileHandle(forReadingFrom: url) }
    deinit { try? file.close() }
}

private let maiCloudPhotoReadNext: MaiCloudDriveReadNext = {
    context, output, capacity, bytesRead in
    guard let context, let output, let bytesRead else { return 0 }
    do {
        let stream = Unmanaged<MaiCloudPhotoStream>.fromOpaque(context).takeUnretainedValue()
        let bytes = try stream.file.read(upToCount: capacity) ?? Data()
        bytes.withUnsafeBytes { raw in
            if let base = raw.baseAddress {
                output.update(from: base.assumingMemoryBound(to: UInt8.self),
                              count: bytes.count)
            }
        }
        bytesRead.pointee = bytes.count
        return 1
    } catch { return 0 }
}

private struct MaiCloudPhotoMedia: Transferable, Sendable {
    let url: URL
    let displayName: String

    static var transferRepresentation: some TransferRepresentation {
        FileRepresentation(contentType: .movie) { media in
            SentTransferredFile(media.url)
        } importing: { received in
            try stage(source: received.file, isImage: false)
        }
        FileRepresentation(contentType: .image) { media in
            SentTransferredFile(media.url)
        } importing: { received in
            try stage(source: received.file, isImage: true)
        }
    }

    private static func stage(source: URL, isImage: Bool) throws -> MaiCloudPhotoMedia {
        let extensionName: String
        if isImage {
            guard let imageSource = CGImageSourceCreateWithURL(source as CFURL, nil),
                  let imageType = CGImageSourceGetType(imageSource) as String?,
                  let type = UTType(imageType) else {
                throw MaiCloudDriveError(status: 0, httpStatus: 0,
                                         detail: "无法识别选中的相册图片")
            }
            if type.conforms(to: .jpeg) { extensionName = "jpg" }
            else if type.conforms(to: .png) { extensionName = "png" }
            else if type.conforms(to: .heic) { extensionName = "heic" }
            else if let webp = UTType(filenameExtension: "webp"),
                    type.conforms(to: webp) { extensionName = "webp" }
            else {
                throw MaiCloudDriveError(status: 0, httpStatus: 0,
                                         detail: "云盘支持 JPEG、PNG、WebP 和 HEIC 图片")
            }
        } else {
            extensionName = source.pathExtension.lowercased() == "mp4" ? "mp4" : "mov"
        }
        let size = try source.resourceValues(forKeys: [.fileSizeKey]).fileSize ?? 0
        let limit = isImage ? 20_000_000 : 2_000_000_000
        guard size > 0, size <= limit else {
            throw MaiCloudDriveError(status: 0, httpStatus: 0,
                                     detail: isImage ? "相册图片必须在 20 MB 以内"
                                                     : "相册视频必须在 2 GB 以内")
        }
        let directory = FileManager.default.urls(for: .applicationSupportDirectory,
            in: .userDomainMask)[0]
            .appendingPathComponent("MaiCloudDrivePhotoImports", isDirectory: true)
        try FileManager.default.createDirectory(at: directory,
                                                withIntermediateDirectories: true)
        let target = directory.appendingPathComponent(UUID().uuidString)
            .appendingPathExtension(extensionName)
        let stream = try MaiCloudPhotoStream(url: source)
        var result = MaiCloudDriveResult()
        target.path.withCString { path in
            mai_cloud_drive_stage_stream(path, UInt64(size),
                maiCloudPhotoReadNext, Unmanaged.passUnretained(stream).toOpaque(),
                nil, nil, nil, &result)
        }
        let status = result.status
        mai_cloud_drive_result_free(&result)
        guard status == MAI_CLOUD_DRIVE_OK else {
            throw MaiCloudDriveError(status: Int32(status.rawValue), httpStatus: 0,
                                     detail: "无法准备相册文件")
        }
        let sourceName = source.lastPathComponent
        let sourceStem = source.deletingPathExtension().lastPathComponent
        let usefulName = sourceName.utf8.count <= 240 &&
            UUID(uuidString: sourceStem) == nil &&
            sourceName.rangeOfCharacter(from: CharacterSet(charactersIn: "/\\")) == nil &&
            source.pathExtension.lowercased() == extensionName
        let formatter = DateFormatter()
        formatter.dateFormat = "yyyyMMdd_HHmmss"
        let name = usefulName ? sourceName :
            "相册\(isImage ? "图片" : "视频")_\(formatter.string(from: Date())).\(extensionName)"
        return MaiCloudPhotoMedia(url: target, displayName: name)
    }
}

private struct MaiCloudDrivePreview: Identifiable {
    let id = UUID().uuidString
    var items: [MaiCloudDriveVideo]
    let initialIndex: Int
    let accountID: String
    let folderID: String?
    var nextCursor: String?
    var visitedCursors = Set<String>()
}

@MainActor private final class MaiCloudThumbnailCache {
    static let shared = MaiCloudThumbnailCache()
    private let images = NSCache<NSString, UIImage>()
    private var failures: [String: Date] = [:]

    private init() { images.totalCostLimit = 24 * 1024 * 1024 }

    func image(for key: String) -> UIImage? { images.object(forKey: key as NSString) }
    func shouldRetry(_ key: String) -> Bool {
        guard let failedAt = failures[key] else { return true }
        return Date().timeIntervalSince(failedAt) > 300
    }
    func store(_ image: UIImage, for key: String) {
        let cost = Int(image.size.width * image.scale * image.size.height * image.scale * 4)
        images.setObject(image, forKey: key as NSString, cost: cost)
        failures.removeValue(forKey: key)
    }
    func failed(_ key: String) {
        if failures.count >= 512 {
            failures = failures.filter { Date().timeIntervalSince($0.value) < 300 }
            if failures.count >= 512, let first = failures.keys.first {
                failures.removeValue(forKey: first)
            }
        }
        failures[key] = Date()
    }
    func clear() { images.removeAllObjects(); failures.removeAll() }
}

private struct MaiCloudThumbnail: View {
    let accountID: String
    let media: MaiCloudDriveVideo
    @Binding var loaded: Bool
    @State private var image: UIImage?

    var body: some View {
        ZStack {
            RoundedRectangle(cornerRadius: 12)
                .fill(RemoteIMStyle.blueSoft)
            if let image {
                Image(uiImage: image)
                    .resizable()
                    .scaledToFill()
            } else {
                Image(systemName: media.isImage ? "photo" : "play.fill")
                    .font(AppTypography.symbol(size: 18, weight: .semibold))
                    .foregroundStyle(RemoteIMStyle.blue)
            }
            if image != nil && !media.isImage {
                Image(systemName: "play.fill")
                    .font(AppTypography.symbol(size: 11, weight: .bold))
                    .foregroundStyle(.white)
                    .frame(width: 23, height: 23)
                    .background(.black.opacity(0.6), in: Circle())
            }
        }
        .frame(width: 72, height: 72)
        .clipShape(RoundedRectangle(cornerRadius: 12))
        .task(id: accountID + ":" + media.id) {
            let key = accountID + ":" + media.id
            if let cached = MaiCloudThumbnailCache.shared.image(for: key) {
                image = cached
                loaded = true
                return
            }
            guard MaiCloudThumbnailCache.shared.shouldRetry(key) else { return }
            do {
                let bytes = try await MaiCloudDriveClient.shared.thumbnailData(videoID: media.id)
                guard !Task.isCancelled, let decoded = UIImage(data: bytes) else {
                    if !Task.isCancelled { MaiCloudThumbnailCache.shared.failed(key) }
                    return
                }
                MaiCloudThumbnailCache.shared.store(decoded, for: key)
                image = decoded
                loaded = true
            } catch {
                if !Task.isCancelled { MaiCloudThumbnailCache.shared.failed(key) }
            }
        }
    }
}

// ScrollView does not host native swipeActions. Keep the existing card alive
// while revealing its trailing action, and let vertical drags scroll normally.
private struct MaiCloudDriveHorizontalPan: UIViewRepresentable {
    let enabled: Bool
    let changed: (CGFloat) -> Void
    let ended: () -> Void

    func makeUIView(context: Context) -> PanView { PanView() }
    func updateUIView(_ view: PanView, context: Context) {
        view.pan.isEnabled = enabled
        view.changed = changed
        view.ended = ended
    }

    static func dismantleUIView(_ view: PanView, coordinator: ()) {
        view.pan.view?.removeGestureRecognizer(view.pan)
    }

    final class PanView: UIView, UIGestureRecognizerDelegate {
        var changed: (CGFloat) -> Void = { _ in }
        var ended: () -> Void = {}
        lazy var pan: UIPanGestureRecognizer = {
            let value = UIPanGestureRecognizer(target: self, action: #selector(dragged))
            value.delegate = self
            value.maximumNumberOfTouches = 1
            return value
        }()

        override func hitTest(_ point: CGPoint, with event: UIEvent?) -> UIView? { nil }
        override func didMoveToWindow() {
            super.didMoveToWindow()
            pan.view?.removeGestureRecognizer(pan)
            window?.addGestureRecognizer(pan)
        }

        func gestureRecognizer(_ recognizer: UIGestureRecognizer,
                               shouldReceive touch: UITouch) -> Bool {
            guard bounds.contains(touch.location(in: self)), window != nil else { return false }
            var ancestor: UIView? = self
            while let current = ancestor {
                if current.isHidden || current.alpha < 0.01 { return false }
                if current.clipsToBounds && !current.bounds.contains(touch.location(in: current)) {
                    return false
                }
                ancestor = current.superview
            }
            return true
        }

        override func gestureRecognizerShouldBegin(_ recognizer: UIGestureRecognizer) -> Bool {
            let velocity = pan.velocity(in: self)
            return abs(velocity.x) > abs(velocity.y) * 1.2
        }

        @objc private func dragged() {
            switch pan.state {
            case .began, .changed: changed(pan.translation(in: self).x)
            case .ended, .cancelled, .failed: ended()
            default: break
            }
        }
    }
}

private struct MaiCloudDriveSwipeRow<Content: View>: View {
    let id: String
    let enabled: Bool
    @Binding var revealedID: String?
    let delete: () -> Void
    @ViewBuilder let content: () -> Content
    @State private var dragOffset: CGFloat?
    @State private var visible = true
    private let actionWidth: CGFloat = 84

    private var offset: CGFloat { dragOffset ?? (revealedID == id ? -actionWidth : 0) }

    var body: some View {
        ZStack(alignment: .trailing) {
            if offset < 0 {
                Button(role: .destructive, action: delete) {
                    Label("删除", systemImage: "trash")
                        .labelStyle(.titleAndIcon)
                        .font(AppTypography.detail.weight(.semibold))
                        .foregroundStyle(.white)
                        .frame(width: actionWidth - 8)
                        .frame(maxHeight: .infinity)
                        .background(.red, in: RoundedRectangle(cornerRadius: 14))
                }
                .buttonStyle(.plain)
                .accessibilityIdentifier("cloud-drive-delete-\(id)")
            }
            content()
                .offset(x: offset)
        }
        .clipShape(RoundedRectangle(cornerRadius: 14))
        .contentShape(Rectangle())
        .background(MaiCloudDriveHorizontalPan(enabled: enabled && visible,
            changed: { translation in
                let start = revealedID == id ? -actionWidth : 0
                dragOffset = min(max(start + translation, -actionWidth), 0)
            }, ended: {
                let shouldReveal = offset < -actionWidth / 2
                withAnimation(.easeOut(duration: 0.2)) {
                    revealedID = shouldReveal ? id : nil
                    dragOffset = nil
                }
            }))
        .onAppear { visible = true }
        .onDisappear { visible = false }
        .accessibilityElement(children: .contain)
        .accessibilityAction(named: "删除") { delete() }
        .accessibilityIdentifier("cloud-drive-row-\(id)")
    }
}

struct MaiCloudDriveView: View {
    @EnvironmentObject private var appState: RemoteIMAppState
    @ObservedObject private var model = MaiCloudDriveModel.shared
    @State private var importing = false
    @State private var fileSelectionAccountID = ""
    @State private var showingPhotoPicker = false
    @State private var photoSelectionAccountID = ""
    @State private var photoItem: PhotosPickerItem?
    @State private var deletingID: String?
    @State private var revealedMediaID: String?
    @State private var discardingTaskID: String?
    @State private var preview: MaiCloudDrivePreview?
    @State private var previewPageTask: Task<Void, Never>?
    @State private var previewPageRequestID: UUID?
    @State private var savingID: String?
    @State private var saveTask: Task<Void, Never>?
    @State private var loadedThumbnailIDs = Set<String>()

    var body: some View {
        ScrollViewReader { scroll in
            ScrollView {
                LazyVStack(alignment: .leading, spacing: 18) {
                    if model.availability == .disabled {
                        VStack(spacing: 12) {
                            Image(systemName: "icloud.slash")
                                .font(AppTypography.symbol(size: 36, weight: .light))
                                .foregroundStyle(RemoteIMStyle.textSecondary)
                            Text("云盘暂不可用").font(AppTypography.heading)
                            Text("当前账号尚未开通云盘，或服务暂时不可用")
                                .font(AppTypography.detail)
                                .foregroundStyle(RemoteIMStyle.textSecondary)
                            Button("重试") { Task { await model.reload() } }
                                .font(AppTypography.detail.weight(.semibold))
                        }
                        .frame(maxWidth: .infinity, minHeight: 240)
                        .accessibilityIdentifier("cloud-drive-unavailable")
                    } else {
                        if model.availability == .available { quotaCard }
                        if !model.notice.isEmpty {
                            Text(model.notice)
                                .font(AppTypography.detail)
                                .foregroundStyle(RemoteIMStyle.textPrimary)
                                .padding(12)
                                .frame(maxWidth: .infinity, alignment: .leading)
                                .background(RemoteIMStyle.blueSoft,
                                            in: RoundedRectangle(cornerRadius: 12))
                        }
                        if !model.localTasks.isEmpty {
                            sectionTitle("待上传")
                            ForEach(model.localTasks) { task in uploadCard(task) }
                        }
                        // 根目录不重复显示“我的云盘”；进入子目录后保留可点击的返回路径。
                        if model.currentFolderID != nil {
                            folderNavigation
                                .id("cloud-drive-directory-navigation-top")
                        }
                        if model.availability == .unknown && model.listError.isEmpty {
                            ProgressView()
                                .accessibilityLabel("正在连接云盘")
                                .frame(maxWidth: .infinity, minHeight: 180)
                        } else if model.loading && model.videos.isEmpty {
                            ProgressView()
                                .accessibilityLabel("正在加载云盘")
                                .frame(maxWidth: .infinity, minHeight: 180)
                        } else if model.videos.isEmpty && model.childFolders.isEmpty && model.listError.isEmpty {
                            VStack(spacing: 12) {
                                Image(systemName: "photo.on.rectangle.angled")
                                    .font(AppTypography.symbol(size: 36, weight: .light))
                                    .foregroundStyle(RemoteIMStyle.blue)
                                Text(model.currentFolderID == nil ? "还没有图片或视频" : "文件夹是空的")
                                    .font(AppTypography.heading)
                                Text(model.currentFolderID == nil
                                     ? "上传后仅当前 MaiChat 账号可见"
                                     : "可以让 AI 将云盘文件整理到这里")
                                    .font(AppTypography.detail).foregroundStyle(.secondary)
                            }
                            .frame(maxWidth: .infinity, minHeight: 180)
                            .accessibilityIdentifier("cloud-drive-empty")
                        }
                        ForEach(model.childFolders, id: \.id) { folder in folderRow(folder) }
                        ForEach(model.videos) { media in
                            MaiCloudDriveSwipeRow(id: media.id,
                                enabled: preview == nil && deletingID == nil &&
                                    discardingTaskID == nil && !importing && !showingPhotoPicker,
                                revealedID: $revealedMediaID,
                                delete: { deletingID = media.id }) {
                                mediaCard(media)
                            }
                        }
                        if model.nextCursor != nil {
                            Button("加载更多") {
                                Task { await model.loadMore() }
                            }
                            .disabled(model.loadingMore)
                            .frame(maxWidth: .infinity)
                            .padding(12)
                            .accessibilityIdentifier("cloud-drive-load-more")
                        }
                        if !model.listError.isEmpty {
                            VStack(alignment: .leading, spacing: 9) {
                                Text(model.listError).foregroundStyle(.red)
                                Button("重试") { Task { await model.reload() } }
                            }
                            .font(AppTypography.detail)
                            .padding(14)
                            .frame(maxWidth: .infinity, alignment: .leading)
                            .background(Color.red.opacity(0.07),
                                        in: RoundedRectangle(cornerRadius: 12))
                        }
                    }
                }
                .padding(18)
                .id("cloud-drive-root-top")
            }
            .onChange(of: model.currentFolderID) { _ in
                scroll.scrollTo(model.currentFolderID == nil ? "cloud-drive-root-top" :
                    "cloud-drive-directory-navigation-top", anchor: .top)
            }
            .onChange(of: appState.maiChatAccountID) { _ in
                scroll.scrollTo(model.currentFolderID == nil ? "cloud-drive-root-top" :
                    "cloud-drive-directory-navigation-top", anchor: .top)
            }
        }
        .background(Color(uiColor: .systemGroupedBackground))
        .appNavigationTitle("我的云盘")
        .navigationBarTitleDisplayMode(.inline)
        .toolbar {
            ToolbarItem(placement: .topBarTrailing) {
                Menu {
                    Button(model.currentFolderID == nil ? "从文件选择" : "从文件选择并上传到根目录",
                           systemImage: "folder") {
                        fileSelectionAccountID = appState.maiChatAccountID
                        importing = true
                    }
                    Button(model.currentFolderID == nil ? "从相册选择" : "从相册选择并上传到根目录",
                           systemImage: "photo.on.rectangle") {
                        photoSelectionAccountID = appState.maiChatAccountID
                        showingPhotoPicker = true
                    }
                } label: {
                    Image(systemName: "plus")
                        .font(AppTypography.symbol(size: 18, weight: .semibold))
                        .frame(width: 36, height: 36)
                }
                .accessibilityLabel(model.currentFolderID == nil ? "上传图片或视频" : "上传到根目录")
                .accessibilityIdentifier("cloud-drive-upload")
                .disabled(model.availability != .available)
            }
            #if DEBUG && targetEnvironment(simulator)
            MaiCloudDriveUITestToolbar(model: model, appState: appState)
            #endif
        }
        .fileImporter(isPresented: $importing,
                      allowedContentTypes: filePickerTypes) { result in
            switch result {
            case let .success(file):
                let expected = fileSelectionAccountID
                Task { await model.addFile(file, expectedAccountID: expected) }
            case let .failure(error):
                let nsError = error as NSError
                if nsError.domain != NSCocoaErrorDomain ||
                    nsError.code != NSUserCancelledError {
                    model.showError(error.localizedDescription)
                }
            }
        }
        .photosPicker(isPresented: $showingPhotoPicker,
                      selection: $photoItem, matching: .any(of: [.images, .videos]),
                      preferredItemEncoding: .current)
        .onChange(of: photoItem) { item in
            guard let item else { return }
            let expected = photoSelectionAccountID
            Task {
                defer { photoItem = nil }
                do {
                    guard let media = try await item.loadTransferable(
                        type: MaiCloudPhotoMedia.self) else {
                        model.showError("无法读取选中的相册文件")
                        return
                    }
                    await model.addStagedPhoto(media.url,
                                               displayName: media.displayName,
                                               expectedAccountID: expected)
                } catch { model.showError(error.localizedDescription) }
            }
        }
        .refreshable { await model.reload() }
        .task(id: appState.maiChatAccountID) {
            model.activate(accountID: appState.maiChatAccountID)
            await model.reload()
        }
        .onChange(of: appState.maiChatAccountID) { _ in
            saveTask?.cancel()
            closePreview()
            MaiCloudThumbnailCache.shared.clear()
            loadedThumbnailIDs.removeAll()
            revealedMediaID = nil
        }
        .onChange(of: model.currentFolderID) { _ in
            closePreview()
            revealedMediaID = nil
            deletingID = nil
            loadedThumbnailIDs.removeAll()
        }
        .onDisappear {
            if preview == nil { previewPageTask?.cancel() }
            saveTask?.cancel()
        }
        .fullScreenCover(item: $preview) { item in
            MaiCloudDriveMediaPager(items: item.items,
                initialIndex: item.initialIndex, loadImage: { media in
                    let file = try await MaiCloudDriveClient.shared.downloadForPreview(media)
                    guard !Task.isCancelled,
                          item.accountID == appState.maiChatAccountID,
                          item.folderID == model.currentFolderID else {
                        try? FileManager.default.removeItem(at: file)
                        throw CancellationError()
                    }
                    return file
                }, loadVideo: { media in
                    let vod = try await MaiCloudDriveClient.shared.openVod(
                        videoID: media.id,
                        extensionName: media.contentType == "video/quicktime" ? "mov" : "mp4")
                    guard !Task.isCancelled,
                          item.accountID == appState.maiChatAccountID,
                          item.folderID == model.currentFolderID else {
                        vod.cancel()
                        throw CancellationError()
                    }
                    return vod
                }, close: { closePreview() }, hasMore: item.nextCursor != nil,
                loadMore: { loadMorePreviewMedia(item.id) })
                .ignoresSafeArea()
        }
        // 删除确认不依赖滚动列表的锚点，避免系统将 popover 定位到页面顶部。
        .alert("从我的云盘删除这个文件？",
                            isPresented: Binding(get: { deletingID != nil },
                                                 set: { if !$0 { deletingID = nil } })) {
            if let id = deletingID {
                Button("删除文件", role: .destructive) {
                    deletingID = nil
                    revealedMediaID = nil
                    Task { await model.delete(videoID: id) }
                }
            }
            Button("取消", role: .cancel) { deletingID = nil }
        } message: {
            Text("删除后无法恢复，其他设备上的云盘列表也会同步移除。")
        }
        .confirmationDialog("取消这次上传？",
                            isPresented: Binding(get: { discardingTaskID != nil },
                                                 set: { if !$0 { discardingTaskID = nil } })) {
            if let id = discardingTaskID,
               let task = model.localTasks.first(where: { $0.id == id }) {
                Button("取消并删除上传任务", role: .destructive) {
                    discardingTaskID = nil
                    model.discardLocalTask(task)
                }
            }
            Button("继续保留", role: .cancel) { discardingTaskID = nil }
        } message: {
            Text("已上传的分片会从云端清理；原文件不会删除。")
        }
        .accessibilityIdentifier("cloud-drive-screen")
    }

    private var folderNavigation: some View {
        VStack(alignment: .leading, spacing: 10) {
            ScrollView(.horizontal, showsIndicators: false) {
                HStack(spacing: 8) {
                    Button("我的云盘") {
                        Task { await model.browse(folderID: nil) }
                    }
                    .accessibilityIdentifier("cloud-drive-folder-root")
                    ForEach(model.folderPath, id: \.id) { folder in
                        Image(systemName: "chevron.right")
                            .font(AppTypography.metadata.weight(.semibold))
                            .foregroundStyle(.secondary)
                        Button(folder.name) {
                            Task { await model.browse(folderID: folder.id) }
                        }
                        .accessibilityIdentifier("cloud-drive-breadcrumb-\(folder.id)")
                    }
                }
                .font(AppTypography.detail)
                .frame(minHeight: 36)
            }
        }
    }

    private func folderRow(_ folder: MaiCloudDriveFolder) -> some View {
        Button {
            revealedMediaID = nil
            Task { await model.browse(folderID: folder.id) }
        } label: {
            HStack(spacing: 14) {
                Image(systemName: "folder.fill")
                    .font(AppTypography.symbol(size: 28))
                    .foregroundStyle(RemoteIMStyle.blue)
                    .frame(width: 56, height: 56)
                    .background(RemoteIMStyle.blueSoft, in: RoundedRectangle(cornerRadius: 12))
                Text(folder.name)
                    .font(AppTypography.body.weight(.medium))
                    .foregroundStyle(RemoteIMStyle.textPrimary)
                    .lineLimit(2)
                    .frame(maxWidth: .infinity, alignment: .leading)
                Image(systemName: "chevron.right")
                    .font(AppTypography.detail.weight(.semibold))
                    .foregroundStyle(.secondary)
            }
            .padding(12)
            .background(Color(uiColor: .secondarySystemGroupedBackground),
                        in: RoundedRectangle(cornerRadius: 14))
        }
        .buttonStyle(.plain)
        .accessibilityLabel(folder.name)
        .accessibilityIdentifier("cloud-drive-folder-\(folder.id)")
    }

    private var quotaCard: some View {
        VStack(alignment: .leading, spacing: 13) {
            HStack {
                Label("云盘空间", systemImage: "icloud.fill")
                    .font(AppTypography.heading)
                    .foregroundStyle(RemoteIMStyle.textPrimary)
                Spacer()
                Text(formatBytes(model.quota.usedBytes + model.quota.reservedBytes)
                     + " / " + formatBytes(model.quota.limitBytes))
                    .font(AppTypography.detail.monospacedDigit())
                    .foregroundStyle(RemoteIMStyle.textSecondary)
            }
            ProgressView(value: Double(model.quota.usedBytes + model.quota.reservedBytes),
                         total: Double(max(model.quota.limitBytes, 1)))
                .tint(RemoteIMStyle.blue)
            Text(model.quota.reservedBytes > 0
                 ? "其中 \(formatBytes(model.quota.reservedBytes)) 已预留 · 视频 2 GB / 图片 20 MB"
                 : "仅自己可见 · 视频 2 GB / 图片 20 MB")
                .font(AppTypography.detail).foregroundStyle(RemoteIMStyle.textSecondary)
        }
        .padding(17)
        .background(Color(uiColor: .secondarySystemGroupedBackground),
                    in: RoundedRectangle(cornerRadius: 16))
        .accessibilityIdentifier("cloud-drive-quota")
    }

    private func sectionTitle(_ title: String) -> some View {
        Text(title).font(AppTypography.heading).foregroundStyle(RemoteIMStyle.textPrimary)
            .padding(.horizontal, 2)
    }

    private var filePickerTypes: [UTType] {
        var types: [UTType] = [.mpeg4Movie, .quickTimeMovie, .jpeg, .png, .heic]
        if let webp = UTType(filenameExtension: "webp") { types.append(webp) }
        return types
    }

    private func uploadCard(_ task: MaiCloudDriveLocalTask) -> some View {
        VStack(alignment: .leading, spacing: 10) {
            HStack(spacing: 10) {
                Image(systemName: task.contentType.hasPrefix("image/")
                      ? "photo" : "arrow.up.doc")
                    .font(AppTypography.symbol(size: 18, weight: .medium))
                    .foregroundStyle(RemoteIMStyle.blue)
                    .frame(width: 38, height: 38)
                    .background(RemoteIMStyle.blueSoft,
                                in: RoundedRectangle(cornerRadius: 10))
                VStack(alignment: .leading, spacing: 4) {
                    Text(task.name).font(AppTypography.detail.weight(.semibold))
                        .lineLimit(1).truncationMode(.middle)
                    Text(formatBytes(task.sizeBytes))
                        .font(AppTypography.metadata).foregroundStyle(.secondary)
                }
                Spacer(minLength: 0)
                Button(model.isUploading(task.id) ? "暂停" : "继续") {
                    if model.isUploading(task.id) { model.pause(taskID: task.id) }
                    else { model.upload(task) }
                }
                .font(AppTypography.detail.weight(.semibold))
                .accessibilityIdentifier("cloud-drive-upload-action-\(task.id)")
                if !model.isUploading(task.id) {
                    Menu {
                        Button("取消上传", role: .destructive) {
                            discardingTaskID = task.id
                        }
                    } label: {
                        Image(systemName: "ellipsis")
                            .frame(width: 36, height: 36)
                    }
                }
            }
            if let progress = model.progress[task.id] {
                ProgressView(value: Double(progress.uploaded),
                             total: Double(max(progress.total, 1)))
                Text("已上传 \(formatBytes(progress.uploaded)) / \(formatBytes(progress.total))")
                    .font(AppTypography.metadata.monospacedDigit()).foregroundStyle(.secondary)
            } else if let error = model.errors[task.id] {
                Text(error).font(AppTypography.detail).foregroundStyle(.red)
            } else {
                Text("已暂停，稍后可继续")
                    .font(AppTypography.detail).foregroundStyle(.secondary)
            }
        }
        .padding(14)
        .frame(maxWidth: .infinity, alignment: .leading)
        .background(Color(uiColor: .secondarySystemGroupedBackground),
                    in: RoundedRectangle(cornerRadius: 14))
        .accessibilityIdentifier("cloud-drive-upload-task-\(task.id)")
    }

    private func mediaCard(_ media: MaiCloudDriveVideo) -> some View {
        HStack(spacing: 12) {
            Button { open(media) } label: {
                HStack(spacing: 12) {
                    ZStack {
                        MaiCloudThumbnail(accountID: appState.maiChatAccountID,
                                          media: media,
                                          loaded: Binding(
                                            get: { loadedThumbnailIDs.contains(media.id) },
                                            set: { ready in
                                                if ready { loadedThumbnailIDs.insert(media.id) }
                                                else { loadedThumbnailIDs.remove(media.id) }
                                            }))
                        if savingID == media.id {
                            RoundedRectangle(cornerRadius: 12)
                                .fill(.black.opacity(0.45))
                            ProgressView().tint(.white)
                        }
                    }
                    .frame(width: 72, height: 72)
                    VStack(alignment: .leading, spacing: 5) {
                        Text(media.name).font(AppTypography.detail.weight(.semibold))
                            .lineLimit(1).truncationMode(.middle)
                        Text(formatBytes(media.sizeBytes))
                            .font(AppTypography.metadata).foregroundStyle(.secondary)
                    }
                    Spacer(minLength: 0)
                }
                .frame(maxWidth: .infinity, alignment: .leading)
                .contentShape(Rectangle())
            }
            .buttonStyle(.plain)
            .accessibilityLabel(media.isImage ? "查看图片" : "播放视频")
            .accessibilityValue(loadedThumbnailIDs.contains(media.id)
                ? "封面已加载" : "封面待加载")
            .accessibilityIdentifier("cloud-drive-play-\(media.id)")
            if !media.isImage {
                // 单项保存操作直接执行；无障碍标签仍说明用途，页面只显示下载图标。
                Button {
                    saveVideo(media)
                } label: {
                    Image(systemName: "arrow.down.to.line")
                        .frame(width: 44, height: 44)
                }
                .buttonStyle(.plain)
                .disabled(savingID != nil)
                .accessibilityLabel("保存到相册")
                .accessibilityIdentifier("cloud-drive-video-menu-\(media.id)")
            }
        }
        .padding(12)
        .background(Color(uiColor: .secondarySystemGroupedBackground),
                    in: RoundedRectangle(cornerRadius: 14))
    }

    private func open(_ media: MaiCloudDriveVideo) {
        if revealedMediaID == media.id {
            withAnimation(.easeOut(duration: 0.2)) { revealedMediaID = nil }
            return
        }
        guard preview == nil,
              let index = model.videos.firstIndex(where: { $0.id == media.id }) else { return }
        let next = MaiCloudDrivePreview(items: model.videos, initialIndex: index,
            accountID: appState.maiChatAccountID, folderID: model.currentFolderID,
            nextCursor: model.nextCursor)
        preview = next
        loadMorePreviewMedia(next.id)
    }

    private func closePreview() {
        previewPageTask?.cancel()
        previewPageTask = nil
        previewPageRequestID = nil
        preview = nil
    }

    private func loadMorePreviewMedia(_ id: String) {
        guard previewPageTask == nil, let initial = preview,
              initial.id == id, initial.nextCursor != nil else { return }
        let requestID = UUID()
        previewPageRequestID = requestID
        previewPageTask = Task { @MainActor in
            defer {
                if previewPageRequestID == requestID {
                    previewPageTask = nil
                    previewPageRequestID = nil
                }
            }
            // Render the selected media immediately. Append metadata in bounded
            // batches; approaching the loaded end can request another batch.
            for _ in 0..<50 {
                guard !Task.isCancelled, let current = preview,
                      current.id == id, current.accountID == appState.maiChatAccountID,
                      let cursor = current.nextCursor else { return }
                do {
                    let page = try await MaiCloudDriveClient.shared.list(
                        before: cursor, folderID: current.folderID ?? "")
                    guard !Task.isCancelled, var updated = preview, updated.id == id,
                          updated.accountID == appState.maiChatAccountID,
                          updated.folderID == model.currentFolderID else { return }
                    var known = Set(updated.items.map(\.id))
                    updated.items.append(contentsOf: page.videos.filter {
                        $0.folderId == updated.folderID && known.insert($0.id).inserted
                    })
                    if let next = page.nextCursor,
                       next == cursor || updated.visitedCursors.contains(next) {
                        // Keep the last request cursor retryable. A repeating
                        // server cursor is neither an end marker nor progress.
                        preview = updated
                        return
                    }
                    updated.visitedCursors.insert(cursor)
                    updated.nextCursor = page.nextCursor
                    preview = updated
                    if page.nextCursor == nil { return }
                } catch { return }
            }
        }
    }

    private func saveVideo(_ media: MaiCloudDriveVideo) {
        guard savingID == nil, !media.isImage else { return }
        let expectedAccount = appState.maiChatAccountID
        savingID = media.id
        saveTask = Task {
            defer { savingID = nil }
            do {
                let file = try await MaiCloudDriveClient.shared.downloadForExplicitSave(media)
                defer { try? FileManager.default.removeItem(at: file) }
                guard !Task.isCancelled,
                      expectedAccount == appState.maiChatAccountID else { return }
                guard await MaiPhotoLibraryMediaWriter.requestAddAccess() else {
                    model.showError("请允许 MaiChat 添加视频到相册")
                    return
                }
                guard !Task.isCancelled,
                      expectedAccount == appState.maiChatAccountID else { return }
                try await MaiPhotoLibraryMediaWriter.saveVideo(at: file, createdAt: Date())
                model.showNotice("已保存到相册")
            } catch {
                if !Task.isCancelled { model.showError(error.localizedDescription) }
            }
        }
    }

    private func formatBytes(_ bytes: UInt64) -> String {
        if bytes == 0 { return "0 B" }
        return ByteCountFormatter.string(fromByteCount: Int64(bytes), countStyle: .decimal)
    }
}
