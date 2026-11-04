// 文件作用：显式 opt-in 的真机 QA，在正常 App 内以独立内存句柄验证真实本地主模型 client。
// 不安装宿主工具 handler，不读取账号/聊天数据库；源权重只读，所有试验产物位于独立目录。
#if DEBUG && !targetEnvironment(simulator)
import Foundation
import MaiChatCore
import Metal

@MainActor
enum AIPrimaryModelDeviceProbe {
    private static var started = false

    static func startIfRequested() {
        guard !started, ProcessInfo.processInfo.arguments.contains("--ai-primary-model-device-probe") else { return }
        started = true
        let environment = ProcessInfo.processInfo.environment
        let runID = environment["MAICHAT_PRIMARY_DEVICE_RUN_ID"].flatMap(UUID.init(uuidString:)) ?? UUID()
        let probe = AIPrimaryDeviceProbeRun(runID: runID)
        Task.detached(priority: .utility) { await probe.run() }
    }
}

/// setup 与 worker 串行衔接；所有 native handle 调用（含销毁）只在专用串行队列执行。
private final class AIPrimaryDeviceProbeRun: @unchecked Sendable {
    private let expectedSHA = "57a1085840f497d764a7fc5d346922dbde961efb54cc792ea81d694fd846a1d8"
    private let runID: UUID
    private let temporary: URL
    private let reportURL: URL
    private let queue = DispatchQueue(label: "com.kongshang.maichat.primary-device-probe", qos: .utility)
    private let started = ProcessInfo.processInfo.systemUptime
    private var report: [String: Any]
    private var createdTemporary = false

    init(runID: UUID) {
        self.runID = runID
        temporary = FileManager.default.temporaryDirectory
            .appendingPathComponent("MaiChatLocalPrimaryQA-" + runID.uuidString, isDirectory: true)
        reportURL = FileManager.default.urls(for: .documentDirectory, in: .userDomainMask)[0]
            .appendingPathComponent("MaiChatLocalPrimaryQA", isDirectory: true)
            .appendingPathComponent("report-" + runID.uuidString + ".json")
        report = ["run_id": runID.uuidString, "format": 1, "status": "starting",
            "model": "Qwen3.5-2B-Q4_K_M", "source_sha256": expectedSHA,
            "source": "packaged_primary_model",
            "database": "memory", "host_handler_installed": false, "rounds": [],
            "scope": "configured_local_client_full_tools_without_tool_execution",
            "agent_loop_exercised": false, "user_sessions_read_or_written": false,
            "probe_cloud_credentials_requested": false]
        let gpu = MTLCreateSystemDefaultDevice()
        report["metal_device_available"] = gpu != nil
        report["metal_device_name"] = gpu?.name ?? ""
        report["metal_supports_apple7"] = gpu?.supportsFamily(.apple7) ?? false
        report["metal_supports_metal3"] = gpu?.supportsFamily(.metal3) ?? false
    }

    func run() async {
        do {
            // run ID 不复用，避免把旧报告或仍被 mmap 的旧 QA 文件当成本次产物覆盖/删除。
            guard !FileManager.default.fileExists(atPath: reportURL.path) else { return }
            try FileManager.default.createDirectory(at: reportURL.deletingLastPathComponent(), withIntermediateDirectories: true)
            try publish(stage: "source_validation")
            // 与菜单切换复用普通随包模型入口，不依赖开发机预置文件或用户导入目录。
            let bundled = try await AILocalModelStore.shared.bundledPrimarySelection()
            let source = bundled.modelURL
            let home = URL(fileURLWithPath: NSHomeDirectory(), isDirectory: true).resolvingSymlinksInPath().path + "/"
            guard source.resolvingSymlinksInPath().path.hasPrefix(home),
                  !FileManager.default.fileExists(atPath: temporary.path) else { throw ProbeFailure("invalid_source_or_existing_run") }
            let status = mai_local_inference_status()
            guard status.status == MAI_LOCAL_INFERENCE_OK, status.busy == 0 else { throw ProbeFailure("shared_runtime_busy") }
            try FileManager.default.createDirectory(at: temporary, withIntermediateDirectories: false)
            createdTemporary = true
            let store = AILocalModelStore(directory: temporary.appendingPathComponent("Models", isDirectory: true))
            try publish(stage: "importing_public_model")
            let imported = try await store.importModel(from: source, role: .model)
            guard imported.sha256 == expectedSHA, imported.bytes == 1_396_198_496 else { throw ProbeFailure("source_fingerprint_mismatch") }
            let selection = try await store.resolve(modelID: imported.id, projectorID: nil)
            report["copied_model_verified"] = true
            await withCheckedContinuation { (continuation: CheckedContinuation<Void, Never>) in
                queue.async {
                    self.runNative(model: selection.modelURL)
                    continuation.resume()
                }
            }
        } catch {
            report["status"] = "failed"
            report["error"] = safe((error as? ProbeFailure)?.code ?? "probe_setup_failed")
            if createdTemporary { try? FileManager.default.removeItem(at: temporary) }
            try? publish(stage: "finished")
        }
    }

    private func runNative(model: URL) {
        var handle: UnsafeMutableRawPointer?
        var retainedTemporary = false
        var ownedGeneration: UInt64?
        // 能力过滤使用正式 local: 标识；独立 QA 不能以另一个名字声明更宽的工具目录。
        let primaryModel = "local:" + AILocalModelStore.bundledPrimaryID
        do {
            try publish(stage: "configuring_real_local_client")
            guard let created = maiMobileAgentCreate() else { throw ProbeFailure("native_create_failed") }
            handle = created
            let workspace = temporary.appendingPathComponent("Workspace", isDirectory: true)
            try FileManager.default.createDirectory(at: workspace, withIntermediateDirectories: false)
            _ = try request(created, ["op": "configure", "database": ":memory:", "workspace": workspace.path,
                "appRoot": temporary.path, "modelProvider": "llama", "wire": "local",
                "model": primaryModel, "localModelPath": model.path,
                "localProjectorPath": "", "localContextSize": 32768, "localMaximumTokens": 0,
                "localThreads": 2, "policy": "on-request",
                "supportsScreenCapture": aiSupportsWholeDeviceScreenCapture(),
                "temporaryDirectory": temporary.appendingPathComponent("Temporary").path,
                "cacheDirectory": temporary.appendingPathComponent("Cache").path,
                "rvmModelPath": Bundle.main.url(forResource: "rvm_mobilenetv3_fp32", withExtension: "onnx",
                    subdirectory: "MaiAgentModels")?.path ?? "",
                "lamaModelPath": Bundle.main.url(forResource: "lama_fp32", withExtension: "onnx",
                    subdirectory: "MaiAgentModels")?.path ?? ""])
            let prompts = ["Reply with one short sentence: Hello from Qwen. Do not call any tools.",
                "What exact phrase did you just say? Reply briefly. Do not call any tools."]
            var messages: [[String: String]] = []
            var rounds: [[String: Any]] = []
            for (index, prompt) in prompts.enumerated() {
                messages.append(["role": "user", "content": prompt])
                report["current_round"] = index + 1
                try publish(stage: "round_\(index + 1)_running")
                let roundStart = ProcessInfo.processInfo.systemUptime
                // 此 C ABI 只测同一配置的真实 client；完整工具定义入模板，但工具绝不执行。
                // 超时由 C++ 当前调用自己的取消 token 控制，不中断普通 Root 或其他 handle。
                let output = try request(created, ["op": "local_model_probe",
                    "messages": messages, "timeout_ms": 120000], requireSuccess: false)
                report["advertised_tool_names"] = output["advertised_tool_names"]
                ownedGeneration = mai_local_inference_status().model_generation
                let success = output["ok"] as? Bool == true
                let text = output["text"] as? String ?? ""
                let toolCalls = (output["tool_call_count"] as? NSNumber)?.intValue ?? -1
                var metrics: [String: Any] = ["round": index + 1,
                    "wall_seconds": ProcessInfo.processInfo.systemUptime - roundStart,
                    "wire": output["wire"] as? String ?? "",
                    "native_model": safe(output["model"] as? String ?? ""),
                    "gpu_selected": output["localGpuSelected"] as? Bool ?? false,
                    "gpu_model_bytes": (output["localGpuModelBytes"] as? NSNumber)?.uint64Value ?? 0,
                    "gpu_backend": safe(output["localGpuBackend"] as? String ?? ""),
                    "text": success ? safe(String(text.prefix(2000))) : "",
                    "text_characters": success ? text.count : 0,
                    "tool_call_count": toolCalls, "tools_executed": false,
                    "error_empty": success && (output["error"] as? String ?? "").isEmpty,
                    "passed": false]
                if let first = output["first_token_ms"] as? NSNumber {
                    metrics["first_text_seconds"] = first.doubleValue / 1000
                }
                if let total = output["total_ms"] as? NSNumber {
                    metrics["native_total_seconds"] = total.doubleValue / 1000
                }
                let passed = success && toolCalls == 0 && !(text.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty) &&
                    output["wire"] as? String == "local" && output["model"] as? String == primaryModel &&
                    output["localGpuSelected"] as? Bool == true &&
                    ((output["localGpuModelBytes"] as? NSNumber)?.uint64Value ?? 0) > 0 &&
                    (output["error"] as? String ?? "").isEmpty
                metrics["passed"] = passed
                if !success { metrics["error"] = safe(output["error"] as? String ?? "native_probe_failed") }
                rounds.append(metrics)
                report["rounds"] = rounds
                report.removeValue(forKey: "current_round")
                try publish(stage: "round_\(index + 1)_finished")
                guard passed else { throw ProbeFailure(success ? "round_not_completed_on_gpu" :
                    safe(output["error"] as? String ?? "native_probe_failed")) }
                messages.append(["role": "assistant", "content": text])
            }
            // 同现场自然问题核对真实调用，而非仅检查“回复非空”。这里只记录选择，
            // 不装宿主 handler、不调用搜索供应商，也不打开相机或读取用户会话。
            let selectionCases: [(String, [[String: String]], String)] = [
                ("news_after_repeated_replies", [
                    ["role": "assistant", "content": "好的，请告诉我你接下来需要什么帮助？"],
                    ["role": "user", "content": "新闻"],
                    ["role": "assistant", "content": "好的，请告诉我你接下来需要什么帮助？"],
                    ["role": "user", "content": "妈的 听不懂话吗"],
                    ["role": "assistant", "content": "抱歉，我可能理解错了。请告诉我你接下来需要什么帮助？"],
                    ["role": "user", "content": "我要看今天的新闻"]], "web_search"),
                ("fresh_news", [["role": "user", "content": "我要看今天的新闻"]], "web_search"),
                ("camera_selection", [["role": "user", "content": "打开相机拍照"]], "mobile_take_photo")
            ]
            var selections: [[String: Any]] = []
            for (name, input, expectedTool) in selectionCases {
                report["current_case"] = name
                try publish(stage: "tool_selection_running")
                let output = try request(created, ["op": "local_model_probe",
                    "messages": input, "timeout_ms": 120000], requireSuccess: false)
                ownedGeneration = mai_local_inference_status().model_generation
                let calls = output["tool_calls"] as? [[String: Any]] ?? []
                let correct = calls.contains { call in
                    guard call["name"] as? String == expectedTool,
                          let arguments = call["arguments"] as? String,
                          let data = arguments.data(using: .utf8),
                          let object = try? JSONSerialization.jsonObject(with: data) as? [String: Any] else { return false }
                    if expectedTool == "web_search" {
                        guard let query = object["query"] as? String else { return false }
                        return query.contains("新闻") && !query.contains("X月") && !query.contains("2024") && !query.contains("2025")
                    }
                    return true
                }
                let passed = output["ok"] as? Bool == true && correct &&
                    output["tools_executed"] as? Bool == false &&
                    output["localGpuSelected"] as? Bool == true
                selections.append(["case": name, "expected_tool": expectedTool,
                    "passed": passed, "tools_executed": false,
                    "native_total_ms": output["total_ms"] ?? -1,
                    "calls": calls.map { call in ["id": safe(call["id"] as? String ?? ""),
                        "name": safe(call["name"] as? String ?? ""),
                        "arguments": safe(call["arguments"] as? String ?? "")] }])
                report["tool_selection_cases"] = selections
                report.removeValue(forKey: "current_case")
                try publish(stage: "tool_selection_finished")
                guard passed else { throw ProbeFailure("native_tool_selection_failed_" + name) }
            }
            report["status"] = "passed"
        } catch {
            report["status"] = "failed"
            report["error"] = safe((error as? ProbeFailure)?.code ?? "native_probe_failed")
        }
        if let handle {
            try? publish(stage: "destroying_own_handle")
            maiMobileAgentDestroy(handle)
            let state = mai_local_inference_status()
            if state.status == MAI_LOCAL_INFERENCE_OK, state.busy == 0,
               let generation = ownedGeneration, state.model_generation == generation {
                if mai_local_inference_unload() != MAI_LOCAL_INFERENCE_OK { retainedTemporary = true }
            } else if state.status != MAI_LOCAL_INFERENCE_OK || state.has_model != 0 || state.busy != 0 {
                retainedTemporary = true
            }
        }
        if !retainedTemporary { try? FileManager.default.removeItem(at: temporary) }
        report["temporary_retained"] = retainedTemporary
        report["own_handle_destroyed"] = handle != nil
        try? publish(stage: "finished")
    }

    private func request(_ handle: UnsafeMutableRawPointer, _ value: [String: Any],
                         requireSuccess: Bool = true) throws -> [String: Any] {
        let data = try JSONSerialization.data(withJSONObject: value)
        guard let json = String(data: data, encoding: .utf8),
              let response = json.withCString({ maiMobileAgentRequest(handle, $0) }) else { throw ProbeFailure("native_response_missing") }
        defer { maiMobileAgentFree(response) }
        guard let object = try JSONSerialization.jsonObject(with: Data(String(cString: response).utf8)) as? [String: Any] else {
            throw ProbeFailure("native_response_invalid")
        }
        if requireSuccess && object["ok"] as? Bool != true {
            throw ProbeFailure(safe(object["error"] as? String ?? "native_request_failed"))
        }
        return object
    }

    private func publish(stage: String) throws {
        report["stage"] = stage
        report["elapsed_seconds"] = ProcessInfo.processInfo.systemUptime - started
        let data = try JSONSerialization.data(withJSONObject: report, options: [.sortedKeys, .prettyPrinted])
        try data.write(to: reportURL, options: [.atomic, .completeFileProtectionUntilFirstUserAuthentication])
    }

    private func safe(_ value: String) -> String {
        // 报告只含本次合成对话；仍清除模型/错误中可能回显的 App 或临时绝对路径。
        let scrubbed = value.replacingOccurrences(of: temporary.path, with: "[probe-root]")
            .replacingOccurrences(of: NSHomeDirectory(), with: "[app-container]")
        return String(scrubbed.replacingOccurrences(of: #"/(?:private|var|Users|tmp|System|Library)/[^\s\"'<>]*"#,
            with: "[path]", options: .regularExpression).prefix(4000))
    }

    private struct ProbeFailure: Error { let code: String; init(_ code: String) { self.code = code } }
}
#endif
