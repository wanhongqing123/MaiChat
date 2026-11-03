// 文件作用：为普通 iOS 调用提供共享本地推理入口；与 Agent llama 工具复用同一 C++ runtime。
// 不下载模型、不添加界面；文件访问权由调用方提供，阻塞推理只在专用工作队列执行。
import Foundation

struct AILocalInferenceRequest: Sendable {
    var modelURL: URL
    var projectorURL: URL?
    var imageURL: URL?
    var prompt: String
    var contextSize: UInt32 = 4096
    var maximumTokens: UInt32 = 256
    var threads: UInt32 = 2
    var temperature: Float = 0
    var seed: UInt32 = 0
    var maximumOutputBytes: Int = 64 * 1024
    var useChatTemplate: Bool = true
}

struct AILocalInferenceResponse: Sendable {
    enum StopReason: Sendable {
        case endOfSequence
        case tokenLimit
        case outputLimit
    }

    let text: String
    let promptTokens: UInt32
    let generatedTokens: UInt32
    let stopReason: StopReason
    let truncated: Bool
}

struct AILocalInferenceState: Sendable {
    let busy: Bool
    let hasModel: Bool
    let hasProjector: Bool
    let contextSize: UInt32
    let modelGeneration: UInt64
}

struct AILocalInferenceError: Error, LocalizedError, Sendable {
    enum Code: Sendable, Equatable {
        case invalidInput, notFound, busy, notConfigured, notSupported, internalFailure
    }

    let code: Code
    var errorDescription: String? {
        switch code {
        case .invalidInput: "本地推理参数无效，请检查已授权的模型与图片文件"
        case .notFound: "找不到本地模型或投影文件，请先提供文件"
        case .busy: "本地推理请求已经开始，请等待原请求完成"
        case .notConfigured: "本地推理运行库尚未就绪"
        case .notSupported: "本地推理暂不支持此操作"
        case .internalFailure: "本地推理未完成，请重试"
        }
    }
}

// 不持有裸 native 指针；不可复用的 ID 由共享 C ABI 注册表校验。队列闭包持有本对象
// 到同步调用返回，Task 取消仅置标志，不能提前释放正在被 CPU abort callback 读取的状态。
private final class AILocalInferenceJob: @unchecked Sendable {
    let id: MaiLocalInferenceJob

    init() throws {
        id = mai_local_inference_job_create()
        guard id != 0 else { throw AILocalInferenceError(code: .busy) }
    }

    func cancel() { _ = mai_local_inference_job_cancel(id) }
    deinit { _ = mai_local_inference_job_destroy(id) }
}

actor AILocalInferenceRuntime {
    static let shared = AILocalInferenceRuntime()
    // DispatchQueue 的 label 为实际执行队列命名；actor 本身不能直接执行阻塞 native 调用，
    // 否则会占用 Swift cooperative executor 并阻止取消/其他普通操作继续调度。
    private let queue = DispatchQueue(label: "com.kongshang.maichat.local-inference", qos: .userInitiated)

    /// 快速查询同一个共享核心，不等待生成锁；不返回用户模型路径或提示词。
    func status() throws -> AILocalInferenceState {
        let result = mai_local_inference_status()
        guard result.status == MAI_LOCAL_INFERENCE_OK else {
            throw AILocalInferenceError(code: .internalFailure)
        }
        return AILocalInferenceState(busy: result.busy != 0, hasModel: result.has_model != 0,
                                     hasProjector: result.has_projector != 0,
                                     contextSize: result.context_size, modelGeneration: result.model_generation)
    }

    /// 显式释放共享驻留模型。先取消自己的生成 Task 并等待它返回，再调用本方法。
    /// 不取消其他宿主的请求；等待其完成的阻塞发生在专用队列，不占用 actor/MainActor。
    /// 取消 unload Task 也不会杀解码线程或中断别人的生成；调用方须等待释放结果。
    func unload() async throws {
        try await withCheckedThrowingContinuation { (continuation: CheckedContinuation<Void, Error>) in
            queue.async {
                if mai_local_inference_unload() == MAI_LOCAL_INFERENCE_OK {
                    continuation.resume()
                } else {
                    continuation.resume(throwing: AILocalInferenceError(code: .internalFailure))
                }
            }
        }
    }

    /// 执行一次不延续 KV 状态的 CPU 推理。模型/单图/projector 必须是本地已授权文件 URL，
    /// 单图须已按显示方向规范化；不在此处改写原图或声称支持任意 HEIC/EXIF。
    /// 用户取消时等待 native 清理后抛 CancellationError；失败不把半截输出当成功返回。
    func generate(_ request: AILocalInferenceRequest) async throws -> AILocalInferenceResponse {
        guard request.modelURL.isFileURL,
              request.projectorURL?.isFileURL != false,
              request.imageURL?.isFileURL != false,
              request.maximumOutputBytes > 0 else {
            throw AILocalInferenceError(code: .invalidInput)
        }
        let job = try AILocalInferenceJob()
        return try await withTaskCancellationHandler {
            try Task.checkCancellation()
            return try await withCheckedThrowingContinuation { continuation in
                queue.async {
                    do {
                        continuation.resume(returning: try Self.run(request, job: job))
                    } catch {
                        continuation.resume(throwing: error)
                    }
                }
            }
        } onCancel: {
            job.cancel()
        }
    }

    private nonisolated static func run(_ request: AILocalInferenceRequest,
                                       job: AILocalInferenceJob) throws -> AILocalInferenceResponse {
        let urls = [request.modelURL, request.projectorURL, request.imageURL].compactMap { $0 }
        let scoped = urls.filter { $0.startAccessingSecurityScopedResource() }
        defer { scoped.forEach { $0.stopAccessingSecurityScopedResource() } }

        // withCString 的借用范围覆盖整个同步 C 调用；显式字节长度保留 NUL 检查，
        // 不能先把请求指针存进异步对象再离开这些作用域。
        let model = request.modelURL.path
        let projector = request.projectorURL?.path ?? ""
        let image = request.imageURL?.path ?? ""
        var result = model.withCString { modelBytes in
            projector.withCString { projectorBytes in
                image.withCString { imageBytes in
                    request.prompt.withCString { promptBytes in
                        var native = mai_local_inference_request_defaults()
                        native.model_path = MaiLocalInferenceUtf8(bytes: modelBytes, size: model.utf8.count)
                        native.projector_path = MaiLocalInferenceUtf8(bytes: projectorBytes,
                                                                     size: projector.utf8.count)
                        native.image_path = MaiLocalInferenceUtf8(bytes: imageBytes, size: image.utf8.count)
                        native.prompt = MaiLocalInferenceUtf8(bytes: promptBytes, size: request.prompt.utf8.count)
                        native.context_size = request.contextSize
                        native.maximum_tokens = request.maximumTokens
                        native.threads = request.threads
                        native.temperature = request.temperature
                        native.seed = request.seed
                        native.maximum_output_bytes = request.maximumOutputBytes
                        native.use_chat_template = request.useChatTemplate ? 1 : 0
                        return mai_local_inference_generate(job.id, &native)
                    }
                }
            }
        }
        defer { mai_local_inference_result_free(&result) }
        switch result.status {
        case MAI_LOCAL_INFERENCE_OK: break
        case MAI_LOCAL_INFERENCE_CANCELED: throw CancellationError()
        case MAI_LOCAL_INFERENCE_INVALID_INPUT: throw AILocalInferenceError(code: .invalidInput)
        case MAI_LOCAL_INFERENCE_NOT_FOUND: throw AILocalInferenceError(code: .notFound)
        case MAI_LOCAL_INFERENCE_BUSY: throw AILocalInferenceError(code: .busy)
        case MAI_LOCAL_INFERENCE_NOT_CONFIGURED: throw AILocalInferenceError(code: .notConfigured)
        case MAI_LOCAL_INFERENCE_NOT_SUPPORTED: throw AILocalInferenceError(code: .notSupported)
        default: throw AILocalInferenceError(code: .internalFailure)
        }
        guard let bytes = result.text_utf8, result.text_size <= request.maximumOutputBytes,
              let text = String(data: Data(bytes: bytes, count: result.text_size), encoding: .utf8) else {
            throw AILocalInferenceError(code: .internalFailure)
        }
        let stopReason: AILocalInferenceResponse.StopReason
        switch result.stop_reason {
        case MAI_LOCAL_INFERENCE_END_OF_SEQUENCE: stopReason = .endOfSequence
        case MAI_LOCAL_INFERENCE_TOKEN_LIMIT: stopReason = .tokenLimit
        case MAI_LOCAL_INFERENCE_OUTPUT_LIMIT: stopReason = .outputLimit
        default: throw AILocalInferenceError(code: .internalFailure)
        }
        return AILocalInferenceResponse(text: text, promptTokens: result.prompt_tokens,
                                        generatedTokens: result.generated_tokens,
                                        stopReason: stopReason, truncated: result.truncated != 0)
    }
}
