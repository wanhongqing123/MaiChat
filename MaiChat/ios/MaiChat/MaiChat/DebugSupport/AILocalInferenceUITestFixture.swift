// 测试宿主：正常 Root + 真实 Swift 普通入口 + C ABI + 已链接共享 runtime，无权重/网络。
#if DEBUG && targetEnvironment(simulator)
import Foundation
import CryptoKit
import MaiChatCore
import SwiftUI

struct AILocalInferenceUITestRoot: View {
    @StateObject private var state = RemoteIMAppState(loadedSettings: .empty)
    @State private var prepared = false
    @State private var running = false
    @State private var attempt = 0
    @State private var receipt = "idle"

    var body: some View {
        RootView()
            .environmentObject(state)
            .onAppear {
                guard !prepared else { return }
                prepared = true
                state.masterUserID = "local-inference-ui-test"
                state.chatState = MasterChatState(ownerUserID: "local-inference-ui-test")
                state.hasCompletedInitialLogin = true
                AIAssistantModel.shared.installSessionSwitchUITestFixture()
                Task { await verifySelectedFixture() }
            }
            .overlay(alignment: .bottomTrailing) {
                VStack(alignment: .trailing, spacing: 6) {
                    Text("本地推理普通入口检验")
                        .accessibilityIdentifier("local-inference-result")
                        .accessibilityValue(receipt)
                    Button("再次检查") { Task { await verifySelectedFixture() } }
                        .accessibilityIdentifier("local-inference-recheck")
                        .disabled(running)
                }
                .font(.caption).padding(8).background(.regularMaterial)
                .padding(.bottom, 110).padding(.trailing, 8)
            }
    }

    @MainActor private func verifySelectedFixture() async {
        let relative = ProcessInfo.processInfo.environment["MAICHAT_LOCAL_INFERENCE_TEXT_MODEL_RELATIVE"] ?? ""
        if relative.isEmpty {
            await verifyOrdinaryAPI()
        } else if relative == "qa-local-inference/tiny.gguf" {
            await verifyTextFixture()
        } else {
            // 仅此固定 QA 文件名可 opt in，不能把环境变量变成任意路径或网络入口。
            publish(["status": "failed", "stage": "model-fixture-path"])
        }
    }

    @MainActor private func verifyTextFixture() async {
        guard !running else { return }
        running = true
        receipt = "running"
        defer { running = false }
        var stage = "text-model-fixture"
        do {
            let documents = FileManager.default.urls(for: .documentDirectory, in: .userDomainMask)[0]
                .resolvingSymlinksInPath().standardizedFileURL
            let model = documents.appendingPathComponent("qa-local-inference/tiny.gguf").standardizedFileURL
            let values = try model.resourceValues(forKeys: [.isRegularFileKey, .fileSizeKey])
            guard model.resolvingSymlinksInPath().standardizedFileURL == model,
                  values.isRegularFile == true, let size = values.fileSize, size == 1_185_376 else {
                publish(["status": "failed", "stage": stage]); return
            }
            // 固定公开 TinyStories QA 权重只由主任务显式拷到隔离 App Documents；不打包/下载，
            // 校验文件防止误加载用户模型或多 GB 权重，哈希不是模型内容/质量验收。
            let sha = SHA256.hash(data: try Data(contentsOf: model))
                .map { String(format: "%02x", $0) }.joined()
            guard sha == "270cba1bd5109f42d03350f60406024560464db173c0e387d91f0426d3bd256d" else {
                publish(["status": "failed", "stage": "model-fixture-hash"]); return
            }
            let runtime = AILocalInferenceRuntime.shared
            let request = AILocalInferenceRequest(modelURL: model, prompt: "Once upon a time",
                contextSize: 256, maximumTokens: 16, threads: 2, temperature: 0, useChatTemplate: false)
            stage = "text-first-generation"
            let first = try await runtime.generate(request)
            let firstState = try await runtime.status()
            guard first.promptTokens > 0, first.generatedTokens > 0,
                  !first.text.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty,
                  !firstState.busy, firstState.hasModel, !firstState.hasProjector,
                  firstState.contextSize == 256, firstState.modelGeneration > 0 else {
                publish(["status": "failed", "stage": stage]); return
            }
            stage = "text-second-generation"
            let second = try await runtime.generate(request)
            let secondState = try await runtime.status()
            guard second.promptTokens > 0, second.generatedTokens > 0,
                  !second.text.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty,
                  !secondState.busy, secondState.hasModel, !secondState.hasProjector,
                  secondState.contextSize == 256,
                  secondState.modelGeneration == firstState.modelGeneration else {
                publish(["status": "failed", "stage": stage]); return
            }
            stage = "text-unload"
            try await runtime.unload()
            let unloaded = try await runtime.status()
            guard !unloaded.busy, !unloaded.hasModel, !unloaded.hasProjector,
                  unloaded.contextSize == 0, unloaded.modelGeneration == secondState.modelGeneration else {
                publish(["status": "failed", "stage": stage]); return
            }
            attempt += 1
            publish(["status": "passed", "mode": "text", "attempt": attempt,
                     "ordinary_api": true, "generation_count": 2,
                     "first_prompt_tokens": first.promptTokens, "first_generated_tokens": first.generatedTokens,
                     "second_prompt_tokens": second.promptTokens, "second_generated_tokens": second.generatedTokens,
                     "first_text_nonempty": true, "second_text_nonempty": true,
                     "busy": false, "has_model": true, "has_projector": false,
                     "context_size": secondState.contextSize,
                     "resident_generation_unchanged": true, "model_generation": secondState.modelGeneration,
                     "unloaded": true, "has_model_after_unload": false,
                     "context_size_after_unload": unloaded.contextSize,
                     "generation_unchanged_after_unload": true])
        } catch {
            publish(["status": "failed", "stage": stage])
        }
    }

    @MainActor private func verifyOrdinaryAPI() async {
        guard !running else { return }
        running = true
        receipt = "running"
        defer { running = false }
        var stage = "initial-status"
        do {
            let runtime = AILocalInferenceRuntime.shared
            let initial = try await runtime.status()
            guard !initial.busy, !initial.hasModel, !initial.hasProjector else {
                publish(["status": "failed", "stage": stage]); return
            }
            let missing = FileManager.default.temporaryDirectory
                .appendingPathComponent("missing-local-inference-\(UUID().uuidString)", isDirectory: true)
                .appendingPathComponent("model.gguf")
            var missingCount = 0
            for _ in 0..<2 {
                stage = "missing-model"
                do {
                    _ = try await runtime.generate(AILocalInferenceRequest(
                        modelURL: missing, prompt: "Local inference boundary fixture"))
                    publish(["status": "failed", "stage": "unexpected-generation"]); return
                } catch let error as AILocalInferenceError where error.code == .notFound {
                    missingCount += 1
                }
                // 此 status 必须仍进入真实 linked runtime，不能用 Swift 缓存伪造 App 存活。
                stage = "status-after-failure"
                let state = try await runtime.status()
                guard !state.busy, !state.hasModel, !state.hasProjector,
                      state.modelGeneration == initial.modelGeneration else {
                    publish(["status": "failed", "stage": stage]); return
                }
            }
            attempt += 1
            publish(["status": "passed", "attempt": attempt, "not_found_count": missingCount,
                     "ordinary_api": true, "status_after_failure": true,
                     "busy": false, "has_model": false, "has_projector": false,
                     "generation_unchanged": true, "model_generation": initial.modelGeneration])
        } catch {
            // 固定阶段分类，不把系统路径、提示词或引擎日志放入 QA 标签/聊天历史。
            publish(["status": "failed", "stage": stage])
        }
    }

    private func publish(_ fields: [String: Any]) {
        guard let data = try? JSONSerialization.data(withJSONObject: fields, options: .sortedKeys),
              let value = String(data: data, encoding: .utf8) else {
            receipt = "invalid-receipt"
            return
        }
        receipt = value
    }
}
#endif
