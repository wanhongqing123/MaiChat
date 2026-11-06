// 文件作用：把普通共享存储服务的真实统计、精确预览及单次清理接到“存储空间”。
// Swift 不自行枚举或删除文件；工作区只读，不把聊天或成品伪装成可恢复缓存。
import Foundation
import SwiftUI

private struct AppStorageSummary: Decodable, Sendable {
    let target: String
    let bytes: UInt64
    let files: UInt64
    let eligible_bytes: UInt64
    let eligible_files: UInt64
    let truncated: Bool
}

private struct AppStoragePreview: Decodable, Sendable, Identifiable {
    let preview_id: String
    let summaries: [AppStorageSummary]
    let candidate_bytes: UInt64
    let candidate_files: UInt64
    let limited: Bool
    let min_age_hours: Int
    var id: String { preview_id }
}

private struct AppStorageCleanup: Decodable, Sendable {
    let removed_files: UInt64
    let reclaimed_bytes: UInt64
    let skipped_files: UInt64
}

private struct AppStorageCleanupPlan: Identifiable {
    struct Entry { let target: String; let preview: AppStoragePreview }
    let id = UUID()
    let entries: [Entry]
    var candidateFiles: UInt64 { entries.reduce(0) { $0 + $1.preview.candidate_files } }
    var candidateBytes: UInt64 { entries.reduce(0) { $0 + $1.preview.candidate_bytes } }
    var limited: Bool { entries.contains { $0.preview.limited } }
}

private struct AppStorageResponse<Value: Decodable>: Decodable {
    let ok: Bool
    let output: Value?
}

private enum AppStorageFailure: Error { case unavailable }

/// 句柄的创建、调用、销毁全部在同一后台串行队列；只向主线程返回值类型。
private final class AppStorageClient: @unchecked Sendable {
    private final class State: @unchecked Sendable { var handle: UnsafeMutableRawPointer? }
    private let queue = DispatchQueue(label: "MaiChat.Settings.Storage", qos: .utility)
    private let state = State()
    private let accountID: String

    init(accountID: String) { self.accountID = accountID }

    deinit {
        let state = state
        queue.async {
            if let handle = state.handle { maiMobileAppStorageDestroy(handle) }
            state.handle = nil
        }
    }

    func preview(target: String) async throws -> AppStoragePreview {
        try await perform(["action": "preview", "target": target])
    }

    func cleanup(previewID: String) async throws -> AppStorageCleanup {
        try await perform(["action": "cleanup", "preview_id": previewID])
    }

    private func perform<Value: Decodable & Sendable>(_ request: [String: String]) async throws -> Value {
        try await withCheckedThrowingContinuation { continuation in
            queue.async { [self] in
                do {
                    if state.handle == nil {
                        let manager = FileManager.default
                        let temporary = manager.temporaryDirectory
                        let cache = try manager.url(for: .cachesDirectory, in: .userDomainMask,
                                                    appropriateFor: nil, create: true)
                        // 仅定位当前账号普通工作区，不打开 Agent、不领凭据、不启动模型。
                        let workspace = try AIAssistantWorkspaceLocator.directory(for: accountID)
                            .appendingPathComponent("Workspace", isDirectory: true)
                        try manager.createDirectory(at: workspace, withIntermediateDirectories: true)
                        state.handle = temporary.path.withCString { temp in
                            cache.path.withCString { cache in
                                workspace.path.withCString { maiMobileAppStorageCreate(temp, cache, $0) }
                            }
                        }
                    }
                    guard let handle = state.handle,
                          let json = String(data: try JSONSerialization.data(withJSONObject: request), encoding: .utf8),
                          let raw = json.withCString({ maiMobileAppStorageRequest(handle, $0) }) else {
                        throw AppStorageFailure.unavailable
                    }
                    defer { maiMobileAppStorageFree(raw) }
                    let result = try JSONDecoder().decode(AppStorageResponse<Value>.self,
                        from: Data(String(cString: raw).utf8))
                    guard result.ok, let output = result.output else { throw AppStorageFailure.unavailable }
                    continuation.resume(returning: output)
                } catch { continuation.resume(throwing: error) }
            }
        }
    }
}

@MainActor
private final class AppStorageSettingsModel: ObservableObject {
    @Published private(set) var snapshot: AppStoragePreview?
    @Published private(set) var busy = false
    @Published var pending: AppStorageCleanupPlan?
    @Published var message: String?
    @Published private(set) var error: String?
    private var client: AppStorageClient?
    private var cleanupClients: [String: AppStorageClient] = [:]
    private var accountID = ""
    private var generation = 0

    func load(account: String) async {
        if accountID != account || client == nil {
            generation += 1
            accountID = account
            client = account.isEmpty ? nil : AppStorageClient(accountID: account)
            snapshot = nil
            pending = nil
            cleanupClients = [:]
            busy = false
        }
        guard !account.isEmpty else { return }
        await scan()
    }

    func scan() async {
        guard !busy, let client else { return }
        busy = true
        error = nil
        let revision = generation
        defer { if revision == generation { busy = false } }
        do {
            let result = try await client.preview(target: "app")
            guard revision == generation else { return }
            snapshot = result
        } catch {
            if revision == generation { self.error = "读取存储空间失败，请重试。" }
        }
    }

    func prepare(target: String) async {
        guard !busy, client != nil else { return }
        busy = true
        let revision = generation
        defer { if revision == generation { busy = false } }
        do {
            // app 统计 token 不能删除。组合清理也只使用两个独立普通服务句柄、
            // 各自明确的 temporary/cache 预览，互不覆盖十分钟单次 token。
            let targets = target == "app" ? ["temporary", "cache"] : [target]
            var clients: [String: AppStorageClient] = [:]
            var entries: [AppStorageCleanupPlan.Entry] = []
            for selected in targets {
                let scoped = AppStorageClient(accountID: accountID)
                clients[selected] = scoped
                let preview = try await scoped.preview(target: selected)
                guard revision == generation else { return }
                if preview.candidate_files > 0 { entries.append(.init(target: selected, preview: preview)) }
            }
            guard revision == generation else { return }
            cleanupClients = clients
            if entries.isEmpty { message = "当前没有符合清理条件的文件。" }
            else { pending = AppStorageCleanupPlan(entries: entries) }
        } catch {
            if revision == generation { message = "无法准备清理预览，请刷新后重试。" }
        }
    }

    func cleanup(_ plan: AppStorageCleanupPlan) async {
        guard !busy, let client else { return }
        busy = true
        pending = nil
        let revision = generation
        let scopedClients = cleanupClients
        var removed: UInt64 = 0
        var reclaimed: UInt64 = 0
        var skipped: UInt64 = 0
        do {
            for entry in plan.entries {
                guard revision == generation else { return }
                guard let scoped = scopedClients[entry.target] else { throw AppStorageFailure.unavailable }
                let result = try await scoped.cleanup(previewID: entry.preview.preview_id)
                removed += result.removed_files
                reclaimed += result.reclaimed_bytes
                skipped += result.skipped_files
            }
            guard revision == generation else { return }
            message = "已清理 \(removed) 个文件，释放 \(Self.size(reclaimed))。"
                + (skipped > 0 ? "另有 \(skipped) 个已变化或受保护的文件保留。" : "")
            let refreshed = try await client.preview(target: "app")
            if revision == generation { snapshot = refreshed }
        } catch {
            if revision == generation {
                message = "清理未完成或预览已过期。已清理 \(removed) 个文件（\(Self.size(reclaimed))），其余文件没有自动重试。请刷新后重新预览；已清理的文件无法恢复。"
            }
        }
        if revision == generation {
            cleanupClients = [:]
            busy = false
        }
    }

    static func size(_ bytes: UInt64) -> String {
        ByteCountFormatter.string(fromByteCount: Int64(min(bytes, UInt64(Int64.max))), countStyle: .file)
    }
}

struct AppStorageSettingsView: View {
    @EnvironmentObject private var appState: RemoteIMAppState
    @StateObject private var model = AppStorageSettingsModel()

    var body: some View {
        List {
            Section {
                VStack(alignment: .leading, spacing: 10) {
                    MySettingsText("所列目录占用").font(AppTypography.detail).foregroundStyle(.secondary)
                    MySettingsText(totalSize).font(AppTypography.hero.weight(.semibold)).monospacedDigit()
                        .accessibilityIdentifier("storage-total-size")
                    Button("预览全部可清理项") { Task { await model.prepare(target: "app") } }
                        .buttonStyle(.bordered)
                        .disabled(model.busy || model.snapshot == nil)
                        .accessibilityIdentifier("storage-preview-all")
                }
                .padding(.vertical, 10)
            } footer: {
                MySettingsText("统计应用临时文件、缓存和当前账号的创作工作区，不代表整个 App 的占用。聊天数据库、云端文件和相册原件不在清理范围。")
            }
            if let snapshot = model.snapshot {
                ForEach(snapshot.summaries, id: \.target) { summary in
                    Section {
                        HStack(alignment: .top) {
                            VStack(alignment: .leading, spacing: 8) {
                                MySettingsText(title(summary.target)).font(AppTypography.heading)
                                MySettingsText(AppStorageSettingsModel.size(summary.bytes))
                                    .font(AppTypography.title.weight(.medium)).monospacedDigit()
                            }
                            Spacer()
                            if summary.target != "workspace" {
                                Button("清理") { Task { await model.prepare(target: summary.target) } }
                                    .buttonStyle(.bordered)
                                    .disabled(model.busy || summary.eligible_files == 0)
                                    .accessibilityIdentifier("storage-clean-\(summary.target)")
                            } else {
                                MySettingsText("保留").font(AppTypography.detail).foregroundStyle(.secondary)
                            }
                        }
                        .padding(.vertical, 6)
                        MySettingsText(description(summary.target)).font(AppTypography.detail).foregroundStyle(.secondary)
                        if summary.target != "workspace" {
                            MySettingsText("符合条件：\(summary.eligible_files) 个文件 · \(AppStorageSettingsModel.size(summary.eligible_bytes))")
                                .font(AppTypography.metadata).foregroundStyle(.secondary)
                        }
                        if summary.truncated {
                            MySettingsText("文件较多，本次统计未覆盖全部目录。")
                                .font(AppTypography.metadata).foregroundStyle(.orange)
                        }
                    }
                }
            } else if model.busy {
                Section { ProgressView("正在读取存储空间…") }
            }
            if let error = model.error {
                Section {
                    MySettingsText(error).foregroundStyle(.secondary)
                    Button("重新读取") { Task { await model.scan() } }
                }
            }
        }
        .listStyle(.insetGrouped)
        .appNavigationTitle("存储空间")
        .navigationBarTitleDisplayMode(.inline)
        .toolbar {
            ToolbarItem(placement: .navigationBarTrailing) {
                if model.busy { ProgressView() }
                else { Button("刷新") { Task { await model.scan() } } }
            }
        }
        .task(id: appState.masterUserID) { await model.load(account: appState.masterUserID) }
        .confirmationDialog("确认清理预览中的文件？", isPresented: Binding(
            get: { model.pending != nil }, set: { if !$0 { model.pending = nil } }), titleVisibility: .visible) {
            if let preview = model.pending {
                Button("清理 \(preview.candidateFiles) 个文件", role: .destructive) {
                    Task { await model.cleanup(preview) }
                }
            }
            Button("取消", role: .cancel) { model.pending = nil }
        } message: {
            if let preview = model.pending {
                MySettingsText("将清理 \(preview.candidateFiles) 个至少 24 小时未修改的临时或缓存文件（\(AppStorageSettingsModel.size(preview.candidateBytes))）。清理无法撤销，按需使用的缓存可能需要重新下载。最近文件、受保护的模型文件、画廊文件和工作区成品会保留。"
                     + (preview.limited ? "每类目录本次最多 1000 项，只处理当前精确预览，剩余文件不会自动加入。" : ""))
            }
        }
        .alert("存储空间", isPresented: Binding(get: { model.message != nil }, set: { if !$0 { model.message = nil } })) {
            Button("好", role: .cancel) {}
        } message: { MySettingsText(model.message ?? "") }
    }

    private var totalSize: String {
        guard let snapshot = model.snapshot else { return "—" }
        let total = snapshot.summaries.reduce(UInt64(0)) { partial, summary in
            let (sum, overflow) = partial.addingReportingOverflow(summary.bytes)
            return overflow ? UInt64.max : sum
        }
        return AppStorageSettingsModel.size(total)
    }

    private func title(_ target: String) -> String {
        switch target {
        case "temporary": return "临时文件"
        case "cache": return "缓存"
        default: return "创作工作区"
        }
    }

    private func description(_ target: String) -> String {
        switch target {
        case "temporary": return "媒体处理、分享等操作留下的临时文件。仅预览至少 24 小时未修改且不受保护的文件。"
        case "cache": return "应用下载与预览缓存。清理后使用相关内容时可能需要重新加载；最近文件和模型权重保留。"
        default: return "当前账号的创作素材、附件和产物。本页只统计，不删除；请在所属会话中管理内容。"
        }
    }
}
