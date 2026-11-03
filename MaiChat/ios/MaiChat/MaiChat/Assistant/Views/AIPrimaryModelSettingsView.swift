// 文件作用：云端和本地主模型共用名称选择、应用与恢复路径；本机文件管理单独按需打开。
// 导入复用普通 Store，配置成功后才应用账号设置；独立运行库自测不改变主模型。
import SwiftUI
import UniformTypeIdentifiers
import MaiChatCore

struct AIPrimaryModelSettingsView: View {
    @Environment(\.dismiss) private var dismiss
    @ObservedObject var assistant: AIAssistantModel
    @State private var draft: AIModelSettings
    @State private var entries: [AILocalModelEntry] = []
    @State private var loading = true
    @State private var importing = false
    @State private var saving = false
    @State private var showingManagement = false
    @State private var showingImporter = false
    @State private var showingSelfTest = false
    @State private var importTask: Task<Void, Never>?
    @State private var notice = ""
    @State private var failed = false

    init(assistant: AIAssistantModel) {
        self.assistant = assistant
        _draft = State(initialValue: assistant.settings)
    }

    private var working: Bool { importing || saving }
    private var localModels: [AILocalModelEntry] { entries.filter { $0.role == .model && !$0.isBuiltin } }
    private var selectedEntry: AILocalModelEntry? { localModels.first { $0.id == draft.localModelID } }
    private var canApply: Bool {
        !working && assistant.selectedRemote == nil && !assistant.busy && !assistant.isSubmitting &&
            (!draft.isLocalModel || (!loading && selectedEntry != nil))
    }
    private var selection: String {
        draft.isLocalModel ? "llama" : "remote"
    }

    var body: some View {
        NavigationStack {
            Form {
                Section {
                    Text("当前：\(currentName)").accessibilityIdentifier("primary-model-current")
                    Picker("主模型", selection: Binding(get: { selection }, set: selectSource)) {
                        Text("云端模型").tag("remote")
                        Text("本地模型").tag("llama")
                    }
                    .pickerStyle(.inline)
                    .disabled(working)
                    .accessibilityIdentifier("primary-model-provider")
                    if draft.isLocalModel {
                        if loading { ProgressView("正在读取本机模型…") }
                        if selectedEntry == nil {
                            Button("添加本地模型") { showingManagement = true }
                                .accessibilityIdentifier("primary-local-add")
                        }
                        Text("当前支持已验证的 Qwen2.5 Instruct 模板，其他模板暂不支持。")
                            .font(.footnote).foregroundStyle(.secondary)
                    }
                }
                Section {
                    Button(saving ? "正在应用…" : "使用此模型", action: apply)
                        .disabled(!canApply)
                        .accessibilityIdentifier("primary-model-apply")
                    if assistant.selectedRemote != nil {
                        Text("请返回这台设备的对话后更改主模型。")
                            .font(.caption).foregroundStyle(.secondary)
                    } else if assistant.busy || assistant.isSubmitting {
                        Text("请先等待当前任务结束。")
                            .font(.caption).foregroundStyle(.secondary)
                    }
                    if !notice.isEmpty {
                        Text(notice).foregroundStyle(failed ? Color.red : Color.secondary)
                            .accessibilityIdentifier("primary-model-notice")
                    }
                }
            }
            .navigationTitle("主模型")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .navigationBarLeading) {
                    Button("本机模型") { showingManagement = true }.disabled(working)
                        .accessibilityIdentifier("primary-model-management")
                }
                ToolbarItem(placement: .confirmationAction) {
                    Button("完成") { dismiss() }.disabled(working)
                        .accessibilityIdentifier("primary-model-done")
                }
            }
            .interactiveDismissDisabled(working)
            .sheet(isPresented: $showingManagement) { management }
            .task { await load() }
            .onDisappear { importTask?.cancel() }
        }
    }

    private var management: some View {
        NavigationStack {
            Form {
                Section {
                    if !localModels.isEmpty {
                        Picker("本地模型", selection: Binding(get: { draft.localModelID ?? "" }, set: selectLocalModel)) {
                            Text("请选择文件").tag("")
                            ForEach(localModels) { entry in Text(entry.name).tag(entry.id) }
                        }
                        .disabled(working)
                        .accessibilityIdentifier("primary-local-model-picker")
                    }
                    Button("添加 GGUF 文件", action: chooseImport).disabled(working)
                        .accessibilityIdentifier("primary-local-model-import")
                    if importing {
                        ProgressView("正在复制并校验模型…")
                        Button("取消导入", role: .destructive) { importTask?.cancel() }
                            .accessibilityIdentifier("primary-local-model-cancel-import")
                    }
                    if failed && !notice.isEmpty { Text(notice).foregroundStyle(.red) }
                } footer: {
                    Text("当前支持已验证的 Qwen2.5 Instruct 模板。添加文件只复制到本机，不改动源文件。")
                }
                Section {
                    Button("打开本地运行库自测") { showingSelfTest = true }.disabled(working)
                        .accessibilityIdentifier("primary-local-self-test")
                } footer: {
                    Text("内置 TinyStories 仅供英文补全和运行库自测，不能作为聊天主模型。自测选择不会改变主模型。")
                }
            }
            .navigationTitle("本机模型")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar { ToolbarItem(placement: .confirmationAction) {
                Button("完成") { showingManagement = false }.disabled(working)
                    .accessibilityIdentifier("primary-model-management-done")
            } }
            .interactiveDismissDisabled(working)
            .fileImporter(isPresented: $showingImporter, allowedContentTypes: [.item]) { result in
                switch result {
                case .success(let url): importModel(url)
                case .failure(let error):
                    if (error as NSError).code != CocoaError.userCancelled.rawValue {
                        failed = true; notice = "无法打开所选文件，请确认文件已下载后重试。"
                    }
                }
            }
            .sheet(isPresented: $showingSelfTest) { AILocalModelsView() }
        }
    }

    private var currentName: String {
        assistant.settings.displayName
    }

    private func selectSource(_ value: String) {
        if value == "llama" {
            if !draft.isLocalModel { draft.cloudModel = draft.effectiveCloudModel }
            draft.modelProvider = "llama"
            let previous = localModels.first { $0.id == draft.localModelID }
            draft.localModelID = (previous ?? localModels.first { $0.name.localizedCaseInsensitiveContains("qwen") }
                ?? localModels.first)?.id
            draft.localProjectorID = nil
            draft.model = "local:" + (draft.localModelID ?? "unselected")
        } else { draft.selectModel(draft.effectiveCloudModel) }
        notice = ""
    }

    private func selectLocalModel(_ id: String) {
        if !draft.isLocalModel { draft.cloudModel = draft.effectiveCloudModel }
        draft.modelProvider = "llama"
        draft.localModelID = id.isEmpty ? nil : id
        draft.localProjectorID = nil
        draft.model = "local:" + (draft.localModelID ?? "unselected")
        notice = ""
    }

    private func load() async {
        defer { loading = false }
        do {
            entries = try await AILocalModelStore.shared.snapshot().entries
            // 用户可在目录读取完成前选“本地”；只补尚未选择的文件，已存失效 ID 保留供修复。
            if draft.isLocalModel, draft.localModelID == nil,
               let available = localModels.first(where: { $0.name.localizedCaseInsensitiveContains("qwen") }) ?? localModels.first {
                selectLocalModel(available.id)
            }
        }
        catch { show(error) }
    }

    private func importModel(_ url: URL) {
        guard !working else { return }
        importing = true; notice = ""; failed = false
        importTask = Task {
            defer { importing = false; importTask = nil }
            do {
                let entry = try await AILocalModelStore.shared.importModel(from: url, role: .model)
                await load()
                selectLocalModel(entry.id)
                notice = "模型已导入，点击“使用此模型”后生效。"
                showingManagement = false
            } catch is CancellationError { notice = "已取消导入" }
            catch { show(error) }
        }
    }

    private func chooseImport() {
        #if DEBUG && targetEnvironment(simulator)
        // 显式 QA 只提供文件选择结果，之后仍实际复制、哈希并由正式主模型后端加载。
        if let source = AIPrimaryModelUITestSupport.modelSource { importModel(source); return }
        #endif
        showingImporter = true
    }

    private func apply() {
        guard canApply else { return }
        saving = true; notice = ""; failed = false
        let next = draft
        Task {
            defer { saving = false }
            if await assistant.save(next) { dismiss() }
            else { failed = true; notice = assistant.error }
        }
    }

    private func show(_ error: Error) {
        failed = true
        notice = (error as? AILocalModelStoreError)?.localizedDescription ?? "本机模型不可读取，请检查文件与可用空间后重试。"
    }
}
