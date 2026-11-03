// 文件作用：实现 Agent 在 iOS 上读取、检查与处理本地图片的工具入口。
// 提供工作区图片分析、转换和输出文件处理。
// 输入与输出路径分离，图片处理失败不覆盖原文件。
import Foundation
import Combine
import CoreFoundation
import Darwin
import CoreImage
import ImageIO
import MaiChatCore
import Photos
import SwiftUI
import UIKit
import UniformTypeIdentifiers
import Vision

@MainActor
extension AIMobileHostToolProvider {
    private func workspaceImageMetadata(_ arguments: [String: Any]) throws
        -> (url: URL, metadata: [String: Any]) {
        let path = Self.string(arguments, key: "path")
        let workspacePath = AIAssistantModel.shared.workspacePath
        guard let url = AIAssistantPathPolicy.resolve(path, workspacePath: workspacePath) else {
            throw AIBackendError(message: "图片必须位于当前 App 目录内")
        }
        let values = try url.resourceValues(forKeys: [.isRegularFileKey, .fileSizeKey])
        guard values.isRegularFile == true, let bytes = values.fileSize,
              bytes > 0, bytes <= 50 * 1024 * 1024,
              let source = CGImageSourceCreateWithURL(url as CFURL, nil),
              CGImageSourceGetCount(source) > 0,
              let properties = CGImageSourceCopyPropertiesAtIndex(source, 0, nil) as? [String: Any],
              let width = properties[kCGImagePropertyPixelWidth as String] as? Int,
              let height = properties[kCGImagePropertyPixelHeight as String] as? Int,
              width > 0, height > 0 else {
            throw AIBackendError(message: "需要不超过 50 MB 的有效图片")
        }
        let mimeType = CGImageSourceGetType(source)
            .flatMap { UTType($0 as String)?.preferredMIMEType } ?? "image/jpeg"
        return (url, [
            "path": path, "mime_type": mimeType, "bytes": bytes,
            "width": width, "height": height,
        ])
    }

    /// 对授权图片执行本地人脸检测并返回边界框。
    /// 输入路径和图片尺寸先验证，检测失败返回原因而不写回源文件。
    func detectFaces(_ arguments: [String: Any]) async -> AIMaiChatHostToolExecution {
        let model = AIAssistantModel.shared
        let workspace = model.workspacePath
        guard let source = AIAssistantPathPolicy.resolve(Self.string(arguments, key: "path"), workspacePath: workspace) else {
            return .failure(code: "invalid_input", message: "Image must be inside the current account file domain")
        }
        let cancellation = MaiPortraitCancellation()
        let observation = model.$workspacePath.dropFirst().sink { value in
            if value != workspace { cancellation.cancel() }
        }
        defer { observation.cancel() }
        do {
            let output = try await withTaskCancellationHandler {
                try await Task.detached(priority: .userInitiated) {
                    try MaiPortraitPhotoProcessor.detectFaces(sourceURL: source, cancellation: cancellation)
                }.value
            } onCancel: { cancellation.cancel() }
            try cancellation.check()
            guard model === AIAssistantModel.shared, model.workspacePath == workspace else { throw CancellationError() }
            return .success(output)
        } catch is CancellationError {
            return .failure(code: "canceled", message: "Face detection was canceled")
        } catch let error as MaiPortraitHostFailure {
            return .failure(code: "invalid_input", message: error.detail)
        } catch {
            return .failure(code: "internal", message: "Face detection could not read or analyze the source image")
        }
    }

    /// 对图片执行人像分割并生成可复用的遮罩结果。
    /// 遮罩与原图坐标保持一致，失败时不交付不完整产物。
    func segmentPerson(_ arguments: [String: Any]) async -> AIMaiChatHostToolExecution {
        do {
            let source = try workspaceImageMetadata(arguments).url
            let workspacePath = AIAssistantModel.shared.workspacePath
            let name = "person-mask-\(UUID().uuidString).png"
            let target = URL(fileURLWithPath: workspacePath, isDirectory: true)
                .appendingPathComponent(name)
            let dimensions = try await Task.detached(priority: .userInitiated) {
                guard let image = CIImage(contentsOf: source,
                                          options: [.applyOrientationProperty: true]) else {
                    throw AIBackendError(message: "无法读取图片")
                }
                let sourceWidth = Int(image.extent.width), sourceHeight = Int(image.extent.height)
                guard sourceWidth > 0, sourceHeight > 0,
                      sourceWidth <= 20_000, sourceHeight <= 20_000 else {
                    throw AIBackendError(message: "图片尺寸无效")
                }
                let scale = min(1.0, 2048.0 / max(image.extent.width, image.extent.height))
                let analysis = image.transformed(by: CGAffineTransform(scaleX: scale, y: scale))
                let request = VNGeneratePersonSegmentationRequest()
                request.qualityLevel = .accurate
                request.outputPixelFormat = kCVPixelFormatType_OneComponent8
                try VNImageRequestHandler(ciImage: analysis, options: [:]).perform([request])
                guard let buffer = request.results?.first?.pixelBuffer,
                      CVPixelBufferGetPixelFormatType(buffer) == kCVPixelFormatType_OneComponent8,
                      CVPixelBufferLockBaseAddress(buffer, .readOnly) == kCVReturnSuccess else {
                    throw AIBackendError(message: "没有生成人物遮罩")
                }
                let maskWidth = CVPixelBufferGetWidth(buffer)
                let maskHeight = CVPixelBufferGetHeight(buffer)
                defer { CVPixelBufferUnlockBaseAddress(buffer, .readOnly) }
                guard maskWidth > 0, maskHeight > 0, maskWidth * maskHeight <= 12_000_000,
                      let base = CVPixelBufferGetBaseAddress(buffer) else {
                    throw AIBackendError(message: "人物遮罩尺寸无效")
                }
                let stride = CVPixelBufferGetBytesPerRow(buffer)
                var bytes = Data(count: maskWidth * maskHeight)
                bytes.withUnsafeMutableBytes { destination in
                    for row in 0..<maskHeight {
                        memcpy(destination.baseAddress!.advanced(by: row * maskWidth),
                               base.advanced(by: row * stride), maskWidth)
                    }
                }
                guard let provider = CGDataProvider(data: bytes as CFData),
                      let mask = CGImage(width: maskWidth, height: maskHeight,
                                         bitsPerComponent: 8, bitsPerPixel: 8,
                                         bytesPerRow: maskWidth, space: CGColorSpaceCreateDeviceGray(),
                                         bitmapInfo: CGBitmapInfo(rawValue: CGImageAlphaInfo.none.rawValue),
                                         provider: provider, decode: nil, shouldInterpolate: false,
                                         intent: .defaultIntent),
                      let destination = CGImageDestinationCreateWithURL(
                        target as CFURL, UTType.png.identifier as CFString, 1, nil) else {
                    throw AIBackendError(message: "无法保存人物遮罩")
                }
                CGImageDestinationAddImage(destination, mask, nil)
                guard CGImageDestinationFinalize(destination) else {
                    try? FileManager.default.removeItem(at: target)
                    throw AIBackendError(message: "无法写入人物遮罩")
                }
                return (sourceWidth, sourceHeight, maskWidth, maskHeight)
            }.value
            return Self.jsonSuccess([
                "path": name, "mime_type": "image/png", "kind": "person_mask",
                "source_width": dimensions.0, "source_height": dimensions.1,
                "mask_width": dimensions.2, "mask_height": dimensions.3,
                "next_tool": "mobile_preview_image",
            ])
        } catch {
            return .failure(code: "internal", message: error.localizedDescription)
        }
    }

    func previewImage(_ arguments: [String: Any]) -> AIMaiChatHostToolExecution {
        do {
            let image = try workspaceImageMetadata(arguments)
            AIAssistantModel.shared.previewImage = AIImagePreview(filePath: image.url.path)
            return Self.jsonSuccess(["opened": true, "path": image.metadata["path"] ?? ""])
        } catch {
            return .failure(code: "invalid_input", message: error.localizedDescription)
        }
    }
}
