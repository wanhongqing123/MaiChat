// 文件作用：正常 Root 的本地主模型 QA 使用显式公共 GGUF 文件；不替换推理或生成内容。
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
    static var isRequested: Bool {
        let arguments = ProcessInfo.processInfo.arguments
        return arguments.contains("--ai-ui-test") && arguments.contains("--ai-primary-model-ui-test")
    }

    static var modelSource: URL? {
        guard isRequested,
              let path = ProcessInfo.processInfo.environment["MAICHAT_PRIMARY_MODEL_FIXTURE_PATH"],
              (path as NSString).isAbsolutePath else { return nil }
        let url = URL(fileURLWithPath: path).standardizedFileURL
        guard url.pathExtension.lowercased() == "gguf",
              FileManager.default.fileExists(atPath: url.path) else { return nil }
        return url
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
        Text("实际主模型状态")
            .font(.caption2)
            .accessibilityIdentifier("primary-ui-model-state")
            .accessibilityValue("provider:\(model.settings.isLocalModel ? "llama" : "remote");configured:\(model.configured);busy:\(model.busy);assistant-text-count:\(count);session:\(model.selected);native-wire:\(model.actualWire);native-model:\(model.actualModelID);gpu-selected:\(model.actualGPUSelected);gpu-bytes:\(model.actualGPUModelBytes);gpu-backend:\(model.actualGPUBackend);error-empty:\(model.error.isEmpty && !model.actualBackendHasError);qwen-gpu-ops-supported:\(AIPrimaryModelUITestSupport.supportsQwenGPUOperations)")
    }
}
#endif
