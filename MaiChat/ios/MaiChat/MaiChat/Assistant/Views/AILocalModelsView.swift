// 文件作用：普通用户入口完成本机 GGUF 导入、选择和实际推理测试，不经过 Agent 或云端模型。
// 页面取消只作用于自己创建的 Task；模型与 Agent 共用驻留 runtime，卸载不抢占其他请求。
import SwiftUI
import UniformTypeIdentifiers
import ImageIO
import UIKit
import MaiChatCore

struct AILocalModelsView: View {
    @Environment(\.dismiss) private var dismiss
    @StateObject private var model = AILocalModelsViewModel()
    @State private var showingImporter = false
    @State private var importingKind = AILocalModelsViewModel.ImportKind.model

    var body: some View {
        NavigationStack {
            Form {
                Section {
                    Text("在这台设备上选择文件并测试本地推理。这里的选择不会切换创作中心或主聊天的模型。")
                        .font(.subheadline).foregroundStyle(.secondary)
                }
                Section {
                    Picker("文本模型", selection: Binding(get: { model.selectedModelID }, set: model.selectModel)) {
                        Text("请选择模型").tag("")
                        ForEach(model.entries.filter { $0.role == .model }) { entry in
                            Text(entry.name).tag(entry.id)
                        }
                    }
                    .disabled(model.working)
                    .accessibilityIdentifier("local-model-picker")
                    Button("导入 GGUF 模型") { importFile(.model) }
                        .disabled(model.working)
                        .accessibilityIdentifier("local-model-import")
                    if let selected = model.selectedEntry {
                        modelDetails(selected)
                    } else {
                        Text("此版本没有可用的内置测试权重。可从“文件”导入已有 GGUF。")
                            .font(.caption).foregroundStyle(.secondary)
                    }
                } header: { Text("模型文件") } footer: {
                    if model.isBuiltin {
                        Text("内置英文测试模型，非聊天/OCR模型。上游 GGUF 仅供 CI 测试，要求不要用于生产；这里只验证加载和生成，不代表模型质量。原始 TinyStories 模型为 MIT。")
                    } else {
                        Text("每次导入创建独立副本，不覆盖原文件。最多 16 GB、128 个文件；GGUF 容器检查通过不代表架构和设备内存一定兼容。")
                    }
                }
                Section {
                    Picker("视觉投影 mmproj", selection: Binding(get: { model.selectedProjectorID },
                                                                   set: model.selectProjector)) {
                        Text("无").tag("")
                        ForEach(model.entries.filter { $0.role == .projector }) { entry in
                            Text(entry.name).tag(entry.id)
                        }
                    }
                    .disabled(model.working || model.isBuiltin)
                    Button("导入 mmproj GGUF") { importFile(.projector) }.disabled(model.working)
                    if let projector = model.selectedProjector { modelDetails(projector) }
                    Button(model.imageName.isEmpty ? "选择测试图片" : "更换测试图片") { importFile(.image) }
                        .disabled(model.working || model.isBuiltin)
                        .accessibilityIdentifier("local-model-image")
                    if !model.imageName.isEmpty {
                        Text(model.imageName).font(.caption).foregroundStyle(.secondary)
                        Button("移除测试图片", role: .destructive) { model.clearImage() }.disabled(model.working)
                    }
                } header: { Text("可选图片测试") } footer: {
                    Text("图片需要与主模型匹配的 mmproj 和当前 mtmd 支持的架构。仅文本时不加载 mmproj。图片在本机转正，限 8 百万像素、20 MB；不会修改原图。内置英文模型不支持图片。")
                }
                Section {
                    TextEditor(text: $model.prompt).frame(minHeight: 100)
                        .accessibilityIdentifier("local-model-prompt")
                        .disabled(model.working)
                    if model.prompt.utf8.count > 8192 {
                        Text("测试提示词最多 8 KB，请缩短后再运行").font(.caption).foregroundStyle(.red)
                    }
                    Toggle("使用模型聊天模板", isOn: $model.useChatTemplate)
                        .disabled(model.working || model.isBuiltin)
                    Stepper("最多生成 \(model.maximumTokens) 个 token", value: $model.maximumTokens,
                            in: 16...256, step: 16).disabled(model.working)
                    Button("运行本地测试") { model.runTest() }
                        .disabled(!model.canRun)
                        .accessibilityIdentifier("local-model-run")
                    if model.testing {
                        HStack {
                            ProgressView()
                            Text(model.cancelRequested ? "正在取消本次测试…" : "正在本机运行…")
                        }
                        Button("取消本次测试", role: .destructive) { model.cancelTest() }
                            .disabled(model.cancelRequested)
                            .accessibilityIdentifier("local-model-cancel")
                    }
                    if model.importing {
                        HStack { ProgressView(); Text("正在复制并校验文件…") }
                        Button("取消导入", role: .destructive) { model.cancelImport() }
                    }
                } header: { Text("测试提示词") } footer: {
                    Text(model.isBuiltin ? "英文 TinyStories 使用原始补全文本，context 256，2 个 CPU 线程；不使用聊天模板。" :
                            "独立测试使用 context 1024、2 个 CPU 线程；不延续前次 KV。大型模型仍可能超过设备内存，请选择适合本机的量化文件。")
                }
                Section {
                    Text(model.runtimeDescription).font(.subheadline)
                        .accessibilityIdentifier("local-model-runtime-status")
                    Button("卸载驻留模型") { model.unload() }
                        .disabled(model.working || model.runtimeBusy || !model.hasResidentModel)
                        .accessibilityIdentifier("local-model-unload")
                    if model.unloading { HStack { ProgressView(); Text("正在卸载…") } }
                } header: { Text("共享运行库") } footer: {
                    Text("本地测试与 Agent 的 llama 工具共用运行库。卸载只在空闲时执行，不停止其他任务。")
                }
                if !model.notice.isEmpty {
                    Section { Text(model.notice).foregroundStyle(model.failed ? Color.red : Color.secondary)
                        .accessibilityIdentifier("local-model-notice")
                        if model.failed { Button("重新加载模型列表") { Task { await model.load() } }.disabled(model.working) }
                    }
                }
                if let result = model.result {
                    Section {
                        Text(result.text.isEmpty ? "（模型未返回文本）" : result.text)
                            .textSelection(.enabled).accessibilityIdentifier("local-model-result")
                        Text("输入 \(result.promptTokens) · 生成 \(result.generatedTokens) token · \(model.stopDescription(result))")
                            .font(.caption).foregroundStyle(.secondary)
                    } header: { Text("最近一次结果") }
                }
            }
            .navigationTitle("本地模型")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar { ToolbarItem(placement: .confirmationAction) {
                Button("完成") { model.close(); dismiss() }.accessibilityIdentifier("local-model-done")
            } }
            .fileImporter(isPresented: $showingImporter,
                allowedContentTypes: importingKind == .image ? [.image] : [.item],
                allowsMultipleSelection: false) { result in
                    switch result {
                    case .success(let urls): if let first = urls.first { model.importFile(first, kind: importingKind) }
                    case .failure: model.importerFailed()
                    }
                }
            .task {
                model.open()
                await model.load()
                while !Task.isCancelled {
                    await model.refreshRuntime()
                    try? await Task.sleep(nanoseconds: 1_000_000_000)
                }
            }
            .onDisappear { model.close() }
        }
    }

    private func importFile(_ kind: AILocalModelsViewModel.ImportKind) {
        importingKind = kind
        showingImporter = true
    }

    private func modelDetails(_ entry: AILocalModelEntry) -> some View {
        VStack(alignment: .leading, spacing: 4) {
            Text(ByteCountFormatter.string(fromByteCount: Int64(entry.bytes), countStyle: .file) + " · GGUF 已校验")
            Text("导入时 SHA-256").fontWeight(.medium)
            Text(entry.sha256).font(.system(.caption2, design: .monospaced)).textSelection(.enabled)
        }
        .font(.caption).foregroundStyle(.secondary)
        .accessibilityIdentifier(entry.role == .model ? "local-model-metadata" : "local-projector-metadata")
    }
}

@MainActor
private final class AILocalModelsViewModel: ObservableObject {
    enum ImportKind { case model, projector, image }
    @Published private(set) var entries: [AILocalModelEntry] = []
    @Published private(set) var selectedModelID = ""
    @Published private(set) var selectedProjectorID = ""
    @Published private(set) var testing = false
    @Published private(set) var importing = false
    @Published private(set) var unloading = false
    @Published private(set) var cancelRequested = false
    @Published private(set) var runtimeBusy = false
    @Published private(set) var hasResidentModel = false
    @Published private(set) var runtimeDescription = "正在查询运行库…"
    @Published private(set) var result: AILocalInferenceResponse?
    @Published private(set) var notice = ""
    @Published private(set) var failed = false
    @Published private(set) var imageName = ""
    @Published var prompt = "Once upon a time, a little bird"
    @Published var useChatTemplate = true
    @Published var maximumTokens = 32
    private var imageURL: URL?
    private var testTask: Task<Void, Never>?
    private var importTask: Task<Void, Never>?
    private var closed = false

    var selectedEntry: AILocalModelEntry? { entries.first { $0.id == selectedModelID } }
    var selectedProjector: AILocalModelEntry? { entries.first { $0.id == selectedProjectorID } }
    var isBuiltin: Bool { selectedEntry?.isBuiltin == true }
    var working: Bool { testing || importing || unloading }
    var canRun: Bool {
        !working && !runtimeBusy && selectedEntry != nil &&
        !prompt.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty && prompt.utf8.count <= 8192 &&
        (imageURL == nil || (!isBuiltin && selectedProjector != nil))
    }

    func load() async {
        do {
            let catalog = try await AILocalModelStore.shared.snapshot()
            entries = catalog.entries
            selectedModelID = entries.contains(where: { $0.id == catalog.selectedModelID }) ? catalog.selectedModelID ?? "" : ""
            selectedProjectorID = catalog.selectedProjectorID ?? ""
            if isBuiltin { useChatTemplate = false }
        } catch { show(error) }
    }

    func selectModel(_ id: String) {
        guard !working, !id.isEmpty else { return }
        Task {
            do {
                try await AILocalModelStore.shared.selectModel(id: id)
                await load()
                if isBuiltin { useChatTemplate = false; clearImage() }
                else { useChatTemplate = true }
            } catch { show(error) }
        }
    }

    func selectProjector(_ id: String) {
        guard !working else { return }
        Task {
            do { try await AILocalModelStore.shared.selectProjector(id: id.isEmpty ? nil : id); await load() }
            catch { show(error) }
        }
    }

    func importFile(_ url: URL, kind: ImportKind) {
        guard !working else { return }
        importing = true
        importTask = Task {
            defer { importing = false; importTask = nil; if closed { clearImage() } }
            do {
                if kind == .image {
                    let worker = Task.detached(priority: .userInitiated) { try Self.prepareImage(url) }
                    let prepared = try await withTaskCancellationHandler { try await worker.value }
                        onCancel: { worker.cancel() }
                    clearImage()
                    imageURL = prepared
                    imageName = String(url.lastPathComponent.prefix(120))
                } else {
                    try await AILocalModelStore.shared.importModel(from: url, role: kind == .model ? .model : .projector)
                    await load()
                    useChatTemplate = !isBuiltin
                }
                failed = false; notice = "文件已在本机准备好"
            } catch is CancellationError { failed = false; notice = "已取消导入" }
            catch { show(error) }
        }
    }

    func runTest() {
        guard canRun else { return }
        testing = true; cancelRequested = false; notice = ""; failed = false; result = nil
        let input = prompt, tokens = UInt32(maximumTokens), image = imageURL, template = useChatTemplate
        testTask = Task {
            defer { testing = false; cancelRequested = false; testTask = nil; if closed { clearImage() } }
            do {
                let choice = try await AILocalModelStore.shared.selection()
                let request = AILocalInferenceRequest(modelURL: choice.modelURL,
                    projectorURL: image == nil ? nil : choice.projectorURL, imageURL: image, prompt: input,
                    contextSize: choice.isBuiltin ? 256 : 1024, maximumTokens: tokens, threads: 2,
                    temperature: 0, seed: 1, useChatTemplate: choice.isBuiltin ? false : template)
                result = try await AILocalInferenceRuntime.shared.generate(request)
                failed = false; notice = "本次本地测试已完成"
            } catch is CancellationError { failed = false; notice = "已取消本次测试" }
            catch { show(error) }
            await refreshRuntime()
        }
    }

    func cancelTest() { if testing { cancelRequested = true; testTask?.cancel() } }
    func cancelImport() { importTask?.cancel() }
    func importerFailed() { failed = true; notice = "无法打开所选文件，请在“文件”中确认文件已下载后重试" }
    func open() { closed = false }
    func close() { closed = true; cancelTest(); cancelImport(); if !testing && !importing { clearImage() } }
    func clearImage() {
        if let imageURL { try? FileManager.default.removeItem(at: imageURL) }
        imageURL = nil; imageName = ""
    }

    func refreshRuntime() async {
        do {
            let state = try await AILocalInferenceRuntime.shared.status()
            runtimeBusy = state.busy; hasResidentModel = state.hasModel
            runtimeDescription = (state.busy ? "运行库正在工作" : "运行库空闲") +
                (state.hasModel ? " · 模型已驻留 · context \(state.contextSize)" : " · 未加载模型")
        } catch { runtimeDescription = "无法查询运行库状态" }
    }

    func unload() {
        guard !working && !runtimeBusy else { return }
        unloading = true
        Task {
            defer { unloading = false }
            do {
                try await AILocalInferenceRuntime.shared.unload()
                failed = false; notice = "驻留模型已卸载"
            } catch { show(error) }
            await refreshRuntime()
        }
    }

    func stopDescription(_ response: AILocalInferenceResponse) -> String {
        switch response.stopReason {
        case .endOfSequence: "模型结束"
        case .tokenLimit: "达到 token 上限"
        case .outputLimit: "达到输出上限"
        }
    }

    private func show(_ error: Error) {
        failed = true
        // 系统文件错误可能含外部路径；界面只展示已知分类，不输出用户文件路径或引擎日志。
        if let known = error as? AILocalModelStoreError { notice = known.localizedDescription }
        else if let known = error as? AILocalInferenceError { notice = known.localizedDescription }
        else { notice = "无法读取或保存本地文件，请检查文件与可用空间后重试" }
    }

    private nonisolated static func prepareImage(_ source: URL) throws -> URL {
        guard source.isFileURL else { throw AILocalInferenceError(code: .invalidInput) }
        let scoped = source.startAccessingSecurityScopedResource()
        defer { if scoped { source.stopAccessingSecurityScopedResource() } }
        let values = try source.resourceValues(forKeys: [.isRegularFileKey, .isSymbolicLinkKey, .fileSizeKey])
        guard values.isRegularFile == true, values.isSymbolicLink != true,
              let size = values.fileSize, size > 0, size <= 20 * 1024 * 1024,
              let imageSource = CGImageSourceCreateWithURL(source as CFURL, [kCGImageSourceShouldCache: false] as CFDictionary),
              let properties = CGImageSourceCopyPropertiesAtIndex(imageSource, 0, nil) as? [CFString: Any],
              let width = (properties[kCGImagePropertyPixelWidth] as? NSNumber)?.intValue,
              let height = (properties[kCGImagePropertyPixelHeight] as? NSNumber)?.intValue,
              width > 0, height > 0, width <= 8192, height <= 8192,
              width * height <= 8_000_000 else { throw AILocalInferenceError(code: .invalidInput) }
        try Task.checkCancellation()
        guard let cgImage = CGImageSourceCreateImageAtIndex(imageSource, 0, nil) else {
            throw AILocalInferenceError(code: .invalidInput)
        }
        let exif = (properties[kCGImagePropertyOrientation] as? NSNumber)?.intValue ?? 1
        let orientations: [Int: UIImage.Orientation] = [1: .up, 2: .upMirrored, 3: .down, 4: .downMirrored,
                                                       5: .leftMirrored, 6: .right, 7: .rightMirrored, 8: .left]
        guard let orientation = orientations[exif] else { throw AILocalInferenceError(code: .invalidInput) }
        let input = UIImage(cgImage: cgImage, scale: 1, orientation: orientation)
        let displaySize = (5...8).contains(exif) ? CGSize(width: height, height: width) : CGSize(width: width, height: height)
        let format = UIGraphicsImageRendererFormat()
        format.scale = 1; format.preferredRange = .standard; format.opaque = false
        let data = UIGraphicsImageRenderer(size: displaySize, format: format).pngData { _ in
            input.draw(in: CGRect(origin: .zero, size: displaySize))
        }
        guard data.count <= 20 * 1024 * 1024 else { throw AILocalInferenceError(code: .invalidInput) }
        try Task.checkCancellation()
        let destination = FileManager.default.temporaryDirectory.appendingPathComponent("local-model-image-\(UUID().uuidString).png")
        do { try data.write(to: destination, options: .withoutOverwriting) }
        catch { try? FileManager.default.removeItem(at: destination); throw error }
        return destination
    }
}
