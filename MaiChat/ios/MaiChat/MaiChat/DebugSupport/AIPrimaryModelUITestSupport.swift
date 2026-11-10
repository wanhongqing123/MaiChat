// 文件作用：观察正常 Root 随包本地主模型的实际配置、推理与生成内容，不替换模型结果。
#if DEBUG && targetEnvironment(simulator)
import SwiftUI
import Metal

enum AIPrimaryModelUITestSupport {
    // 与固定上游支持 Qwen 矩阵乘法/FlashAttention 的必要 SIMD 能力一致。
    // 当前 Simulator 仅报告 Apple2；不能把存在一个 MTL 设备当作能执行这些算子。
    static let supportsQwenGPUOperations: Bool = {
        guard let device = MTLCreateSystemDefaultDevice() else { return false }
        return device.supportsFamily(.apple7) || device.supportsFamily(.metal3)
    }()
    // 独立会话配置验收使用正常 C ABI 和两种 wire 的 loopback 服务，不请求真实供应商。
    static var isSessionConfigurationRequested: Bool {
        let arguments = ProcessInfo.processInfo.arguments
        return arguments.contains("--ai-ui-test") && arguments.contains("--ai-session-model-ui-test")
    }

    static var isRequested: Bool {
        let arguments = ProcessInfo.processInfo.arguments
        return arguments.contains("--ai-ui-test") &&
            (arguments.contains("--ai-primary-model-ui-test") || arguments.contains("--ai-session-model-ui-test") || arguments.contains("--ai-remote-session-model-ui-test"))
    }

}

/// 只观察真实配置和已持久化消息，绝不安装 handler 或生成测试回复。
struct AIPrimaryModelUITestStatus: View {
    @ObservedObject private var model = AIAssistantModel.shared

    var body: some View {
        let count = model.messages.filter { message in
            message.role == "assistant" && message.parts.contains {
                $0.kind == "text" && !($0.text ?? "").trimmingCharacters(in: .whitespacesAndNewlines).isEmpty
            }
        }.count
        let latest = model.messages.last(where: { $0.role == "assistant" })?.parts
            .filter { $0.kind == "text" }.compactMap(\.text).joined(separator: "\n") ?? ""
        VStack(spacing: 0) {
            Text("实际主模型状态")
                .font(.caption2)
                .accessibilityIdentifier("primary-ui-model-state")
                .accessibilityValue("provider:\(model.settings.isLocalModel ? "llama" : "remote");configured:\(model.configured);busy:\(model.busy);assistant-text-count:\(count);session:\(model.selected);native-wire:\(model.actualWire);native-model:\(model.actualModelID);gpu-selected:\(model.actualGPUSelected);gpu-bytes:\(model.actualGPUModelBytes);gpu-backend:\(model.actualGPUBackend);error-empty:\(model.error.isEmpty && !model.actualBackendHasError);qwen-gpu-ops-supported:\(AIPrimaryModelUITestSupport.supportsQwenGPUOperations)")
            Text("会话模型配置")
                .font(.caption2)
                .accessibilityIdentifier("primary-ui-session-configuration")
                .accessibilityValue("session:\(model.selected);model:\(model.settings.model);effort:\(model.settings.effectiveReasoningEffort);wire:\(model.actualWire);remote:\(model.selectedRemote != nil)")
            if ProcessInfo.processInfo.arguments.contains("--ai-remote-session-model-ui-test") {
                AISessionModelRemoteUITestStatus()
            }
            Text("实际回复内容")
                .font(.caption2)
                .accessibilityIdentifier("primary-ui-model-output")
                .accessibilityValue(String(latest.prefix(2000)))
        }
    }
}

private struct AISessionModelRemoteUITestStatus: View {
    @ObservedObject private var support = AIAssistantModel.shared.uiTestSupport
    var body: some View {
        Text("远端会话模型检查").font(.caption2)
            .accessibilityIdentifier("remote-session-model-checks")
            .accessibilityValue(support.sessionModelPolicyResult)
    }
}
#endif
