// 文件作用：实现 Agent 对 iOS 照片库和本地媒体资源的查询与导出工具。
// 处理系统相册浏览、导入和保存照片／视频。
// 相册变更在 Photos 自有队列执行，主线程只接收完成结果。
import Foundation
import Combine
import AVFoundation
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

enum MaiPhotoLibraryMediaWriter {
    @MainActor
    static func requestAddAccess() async -> Bool {
        let current = PHPhotoLibrary.authorizationStatus(for: .addOnly)
        let status = current == .notDetermined
            ? await PHPhotoLibrary.requestAuthorization(for: .addOnly) : current
        return status == .authorized || status == .limited
    }

    // Photos 在自己的串行队列执行变更块；此函数不能继承调用方的 MainActor 隔离。
    nonisolated static func saveVideo(at fileURL: URL, createdAt: Date) async throws {
        try await PHPhotoLibrary.shared().performChanges {
            let request = PHAssetCreationRequest.forAsset()
            let options = PHAssetResourceCreationOptions()
            options.shouldMoveFile = false
            request.addResource(with: .video, fileURL: fileURL, options: options)
            request.creationDate = createdAt
        }
    }

    nonisolated static func saveImage(at fileURL: URL, createdAt: Date) async throws {
        try await PHPhotoLibrary.shared().performChanges {
            let request = PHAssetCreationRequest.forAsset()
            let options = PHAssetResourceCreationOptions()
            options.shouldMoveFile = false
            request.addResource(with: .photo, fileURL: fileURL, options: options)
            request.creationDate = createdAt
        }
    }

    nonisolated static func createAlbum(named name: String) async throws {
        try await PHPhotoLibrary.shared().performChanges {
            PHAssetCollectionChangeRequest.creationRequestForAssetCollection(withTitle: name)
        }
    }

    nonisolated static func addPhotos(_ ids: [String], to albumID: String) async throws {
        guard let album = PHAssetCollection.fetchAssetCollections(
            withLocalIdentifiers: [albumID], options: nil).firstObject,
            album.canPerform(.addContent) else {
            throw NSError(domain: "MaiChat.Photos", code: -1,
                          userInfo: [NSLocalizedDescriptionKey: "相簿不可编辑"])
        }
        let assets = ids.compactMap {
            PHAsset.fetchAssets(withLocalIdentifiers: [$0], options: nil).firstObject
        }
        guard assets.count == ids.count else {
            throw NSError(domain: "MaiChat.Photos", code: -2,
                          userInfo: [NSLocalizedDescriptionKey: "照片已不可访问"])
        }
        try await PHPhotoLibrary.shared().performChanges {
            PHAssetCollectionChangeRequest(for: album)?.addAssets(assets as NSArray)
        }
    }
}

@MainActor
extension AIMobileHostToolProvider {
    private func photoAuthorization() async -> PHAuthorizationStatus {
        let status = PHPhotoLibrary.authorizationStatus(for: .readWrite)
        return status == .notDetermined
            ? await PHPhotoLibrary.requestAuthorization(for: .readWrite)
            : status
    }

    func listPhotos(_ arguments: [String: Any]) async -> AIMaiChatHostToolExecution {
        let status = await photoAuthorization()
        guard status == .authorized || status == .limited else {
            return .failure(code: "canceled", message: "photo library access was not granted")
        }
        let offset = max(0, arguments["offset"] as? Int ?? 0)
        let limit = min(100, max(1, arguments["limit"] as? Int ?? 50))
        let options = PHFetchOptions()
        options.sortDescriptors = [NSSortDescriptor(key: "creationDate", ascending: false)]
        let albumID = (arguments["album_id"] as? String ?? "").trimmingCharacters(in: .whitespacesAndNewlines)
        let photos: PHFetchResult<PHAsset>
        if !albumID.isEmpty && albumID != "all" {
            guard let album = PHAssetCollection.fetchAssetCollections(
                withLocalIdentifiers: [albumID], options: nil
            ).firstObject else { return .failure(code: "not_found", message: "album not found") }
            photos = PHAsset.fetchAssets(in: album, options: options)
        } else {
            photos = PHAsset.fetchAssets(with: options)
        }
        let end = min(photos.count, offset + limit)
        var items: [[String: Any]] = []
        if offset < end {
            for index in offset..<end {
                let photo = photos.object(at: index)
                let mediaType = Self.galleryMediaType(photo)
                items.append([
                    "id": photo.localIdentifier,
                    "mediaType": mediaType,
                    "created_at_ms": Int64(
                        ((photo.creationDate ?? .distantPast).timeIntervalSince1970 * 1000).rounded()),
                    "width": photo.pixelWidth,
                    "height": photo.pixelHeight,
                    "duration_ms": mediaType == "video" ? Int64((photo.duration * 1000).rounded()) : 0,
                    "favorite": photo.isFavorite,
                ])
            }
        }
        return Self.jsonSuccess([
            "access": status == .authorized ? "full" : "limited",
            "total": photos.count, "offset": offset, "items": items,
        ])
    }

    func listAlbums() async -> AIMaiChatHostToolExecution {
        let status = await photoAuthorization()
        guard status == .authorized || status == .limited else {
            return .failure(code: "canceled", message: "photo library access was not granted")
        }
        let albums = PHAssetCollection.fetchAssetCollections(with: .album, subtype: .any, options: nil)
        var items: [[String: Any]] = [["id": "all", "name": "所有照片"]]
        albums.enumerateObjects { album, _, _ in
            items.append(["id": album.localIdentifier, "name": album.localizedTitle ?? "未命名相簿"])
        }
        return Self.jsonSuccess([
            "access": status == .authorized ? "full" : "limited", "albums": items,
        ])
    }

    func readPhoto(_ arguments: [String: Any]) async -> AIMaiChatHostToolExecution {
        let status = await photoAuthorization()
        guard status == .authorized || status == .limited else {
            return .failure(code: "canceled", message: "photo library access was not granted")
        }
        guard let id = arguments["id"] as? String, !id.isEmpty,
              let asset = PHAsset.fetchAssets(withLocalIdentifiers: [id], options: nil).firstObject else {
            return .failure(code: "not_found", message: "photo not found in the authorized library")
        }
        guard asset.mediaType == .image else {
            return .failure(code: "invalid_input",
                            message: "video requires mobile_export_photo_original to get its media file")
        }
        let options = PHImageRequestOptions()
        options.isNetworkAccessAllowed = true
        let data: Data? = await withCheckedContinuation { continuation in
            PHImageManager.default().requestImageDataAndOrientation(for: asset, options: options) {
                data, _, _, _ in continuation.resume(returning: data)
            }
        }
        guard let data, let source = CGImageSourceCreateWithData(data as CFData, nil),
              let preview = CGImageSourceCreateThumbnailAtIndex(source, 0, [
                kCGImageSourceCreateThumbnailFromImageAlways: true,
                kCGImageSourceCreateThumbnailWithTransform: true,
                kCGImageSourceThumbnailMaxPixelSize: 2048,
              ] as CFDictionary),
              let jpeg = UIImage(cgImage: preview).jpegData(compressionQuality: 0.88) else {
            return .failure(code: "internal", message: "photo could not be read")
        }
        let temporary = FileManager.default.temporaryDirectory
            .appendingPathComponent("agent-photo-\(UUID().uuidString).jpg")
        do {
            try jpeg.write(to: temporary, options: .atomic)
            defer { try? FileManager.default.removeItem(at: temporary) }
            guard let imported = await AIAssistantModel.shared.importFile(temporary) else {
                return .failure(code: "internal", message: "photo could not be imported")
            }
            return Self.jsonSuccess([
                "id": id, "path": imported.relativePath, "mime_type": imported.mimeType,
                "next_tool": "mobile_preview_image", "vision_tool": "view_image",
            ])
        } catch {
            return .failure(code: "internal", message: error.localizedDescription)
        }
    }

    /// 导出用户授权照片的原始资源到受控工作区。
    /// 异步导出完成后才返回路径，取消或失败时清理不完整临时文件。
    func exportPhotoOriginal(_ arguments: [String: Any]) async -> AIMaiChatHostToolExecution {
        let status = await photoAuthorization()
        guard status == .authorized || status == .limited else {
            return .failure(code: "canceled", message: "photo library access was not granted")
        }
        guard let id = arguments["id"] as? String, !id.isEmpty,
              let asset = PHAsset.fetchAssets(withLocalIdentifiers: [id], options: nil).firstObject,
              asset.mediaType == .image || asset.mediaType == .video else {
            return .failure(code: "not_found", message: "media item not found in the authorized library")
        }
        let workspacePath = AIAssistantModel.shared.workspacePath
        guard !workspacePath.isEmpty else {
            return .failure(code: "not_configured", message: "Agent working directory is unavailable")
        }
        let component = Self.string(arguments, key: "component")
        if asset.mediaType == .video || component == "video" {
            guard asset.mediaType == .video || asset.mediaSubtypes.contains(.photoLive) else {
                return .failure(code: "invalid_input", message: "this media item has no video component")
            }
            let resources = PHAssetResource.assetResources(for: asset)
            let resource = resources.first(where: {
                $0.type == (asset.mediaType == .video ? .video : .pairedVideo)
            })
            guard let resource else {
                return .failure(code: "not_found", message: "original video resource is unavailable")
            }
            let mediaType = UTType(resource.uniformTypeIdentifier)
            let originalSuffix = URL(fileURLWithPath: resource.originalFilename)
                .pathExtension.lowercased()
            let suffix = ["mov", "mp4", "m4v"].contains(originalSuffix)
                ? originalSuffix : (mediaType?.preferredFilenameExtension ?? "mov")
            let targetName = "gallery-\(UUID().uuidString).\(suffix)"
            let target = URL(fileURLWithPath: workspacePath, isDirectory: true)
                .appendingPathComponent(targetName)
            let options = PHAssetResourceRequestOptions()
            options.isNetworkAccessAllowed = true
            let exportError: Error? = await withCheckedContinuation { continuation in
                PHAssetResourceManager.default().writeData(
                    for: resource, toFile: target, options: options
                ) { error in continuation.resume(returning: error) }
            }
            if let exportError {
                try? FileManager.default.removeItem(at: target)
                return .failure(code: "internal", message: "video export failed: \(exportError.localizedDescription)")
            }
            do {
                let bytes = try target.resourceValues(forKeys: [.fileSizeKey]).fileSize ?? 0
                guard bytes > 0 else {
                    try? FileManager.default.removeItem(at: target)
                    return .failure(code: "internal", message: "original video resource is empty")
                }
                return Self.jsonSuccess([
                    "id": id, "path": targetName,
                    "mediaType": asset.mediaType == .video ? "video" : "livephoto",
                    "mime_type": mediaType?.preferredMIMEType ??
                        (suffix == "mp4" || suffix == "m4v" ? "video/mp4" : "video/quicktime"),
                    "bytes": bytes, "width": asset.pixelWidth,
                    "height": asset.pixelHeight,
                    "duration_ms": Int64((asset.duration * 1000).rounded()),
                ])
            } catch {
                try? FileManager.default.removeItem(at: target)
                return .failure(code: "internal", message: error.localizedDescription)
            }
        }
        let options = PHImageRequestOptions()
        options.isNetworkAccessAllowed = true
        options.version = .original
        let result: (Data, String)? = await withCheckedContinuation { continuation in
            PHImageManager.default().requestImageDataAndOrientation(for: asset, options: options) {
                data, type, _, _ in
                guard let data, let type else { continuation.resume(returning: nil); return }
                continuation.resume(returning: (data, type))
            }
        }
        guard let (data, type) = result, !data.isEmpty, data.count <= 100 * 1024 * 1024 else {
            return .failure(code: "invalid_input", message: "photo is unavailable or exceeds 100 MB")
        }
        guard let imageType = UTType(type), imageType.conforms(to: .image),
              let suffix = imageType.preferredFilenameExtension,
              let mimeType = imageType.preferredMIMEType else {
            return .failure(code: "invalid_input", message: "unsupported system photo format")
        }
        let name = "gallery-\(UUID().uuidString).\(suffix)"
        let target = URL(fileURLWithPath: workspacePath, isDirectory: true)
            .appendingPathComponent(name)
        do {
            try await Task.detached(priority: .utility) {
                try data.write(to: target, options: .atomic)
            }.value
            return Self.jsonSuccess([
                "id": id, "path": name, "mime_type": mimeType,
                "mediaType": Self.galleryMediaType(asset),
                "bytes": data.count, "width": asset.pixelWidth, "height": asset.pixelHeight,
            ])
        } catch {
            return .failure(code: "internal", message: error.localizedDescription)
        }
    }

    private static func galleryMediaType(_ asset: PHAsset) -> String {
        if asset.mediaType == .video { return "video" }
        if asset.mediaType == .image && asset.mediaSubtypes.contains(.photoLive) {
            return "livephoto"
        }
        return "photo"
    }

    func saveImage(_ arguments: [String: Any]) async -> AIMaiChatHostToolExecution {
        let saveRequestedAt = Date()
        guard await MaiPhotoLibraryMediaWriter.requestAddAccess() else {
            return .failure(code: "canceled", message: "photo library access was not granted")
        }
        let path = Self.string(arguments, key: "path")
        guard let source = AIAssistantPathPolicy.resolve(
                  path, workspacePath: AIAssistantModel.shared.workspacePath) else {
            return .failure(code: "invalid_input", message: "image must be inside the App container")
        }
        do {
            let values = try source.resourceValues(forKeys: [.isRegularFileKey, .fileSizeKey])
            let suffix = source.pathExtension.lowercased()
            guard values.isRegularFile == true, let bytes = values.fileSize,
                  bytes > 0, bytes <= 50 * 1024 * 1024,
                  ["jpg", "jpeg", "png", "heic", "heif"].contains(suffix),
                  let image = CGImageSourceCreateWithURL(source as CFURL, nil),
                  CGImageSourceGetCount(image) > 0 else {
                return .failure(code: "invalid_input", message: "image must be a JPEG, PNG, or HEIF file up to 50 MB")
            }
            try await MaiPhotoLibraryMediaWriter.saveImage(at: source, createdAt: saveRequestedAt)
            return Self.jsonSuccess(["saved": true, "source_path": path])
        } catch {
            return .failure(code: "internal", message: error.localizedDescription)
        }
    }

    func saveVideo(_ arguments: [String: Any]) async -> AIMaiChatHostToolExecution {
        let saveRequestedAt = Date()
        guard await MaiPhotoLibraryMediaWriter.requestAddAccess() else {
            return .failure(code: "canceled", message: "photo library access was not granted")
        }
        let path = Self.string(arguments, key: "path")
        guard let source = AIAssistantPathPolicy.resolve(
            path, workspacePath: AIAssistantModel.shared.workspacePath) else {
            return .failure(code: "invalid_input", message: "video must be inside the App container")
        }
        do {
            let values = try source.resourceValues(forKeys: [.isRegularFileKey, .fileSizeKey])
            guard values.isRegularFile == true, let bytes = values.fileSize,
                  bytes > 0, bytes <= 2 * 1024 * 1024 * 1024,
                  ["mp4", "mov", "m4v"].contains(source.pathExtension.lowercased()) else {
                return .failure(code: "invalid_input", message: "video must be an MP4, MOV, or M4V file up to 2 GB")
            }
            let tracks = try await AVURLAsset(url: source).loadTracks(withMediaType: .video)
            guard !tracks.isEmpty else {
                return .failure(code: "invalid_input", message: "file has no video track")
            }
            try await MaiPhotoLibraryMediaWriter.saveVideo(at: source, createdAt: saveRequestedAt)
            return Self.jsonSuccess(["saved": true, "source_path": path, "bytes": bytes])
        } catch {
            return .failure(code: "internal", message: error.localizedDescription)
        }
    }

    /// 按工具参数在本地对图片执行变换并生成新文件。
    /// 源图片保持不变，输出路径、尺寸和失败原因在返回前校验。
    func transformImage(_ arguments: [String: Any]) async -> AIMaiChatHostToolExecution {
        let path = Self.string(arguments, key: "path")
        let workspacePath = AIAssistantModel.shared.workspacePath
        guard !path.isEmpty, !workspacePath.isEmpty,
              let operationData = try? JSONSerialization.data(withJSONObject: arguments),
              let operation = String(data: operationData, encoding: .utf8) else {
            return .failure(code: "invalid_input", message: "image path and an image operation are required")
        }
        guard let source = AIAssistantPathPolicy.resolve(path, workspacePath: workspacePath) else {
            return .failure(code: "invalid_input", message: "image must be inside the App container")
        }
        let workspace = URL(fileURLWithPath: workspacePath, isDirectory: true)
            .resolvingSymlinksInPath().standardizedFileURL
        if Self.string(arguments, key: "operation") == "beautify" {
            return await beautifyImage(arguments, encoded: operation, source: source, workspace: workspace)
        }
        let portrait = Self.string(arguments, key: "operation") == "reshape_body"
        if portrait, let invalid = operation.withCString({ maiPortraitValidateArguments("reshape_body", $0, 1) }) {
            defer { maiPortraitFree(invalid) }
            return .failure(code: "invalid_input", message: String(cString: invalid))
        }
        let profileDirectory = workspace.deletingLastPathComponent()
            .appendingPathComponent("PortraitPreferences", isDirectory: true).path
        do {
            let values = try source.resourceValues(forKeys: [.isRegularFileKey, .fileSizeKey])
            guard values.isRegularFile == true, let size = values.fileSize,
                  size > 0, size <= 50 * 1024 * 1024 else {
                return .failure(code: "invalid_input", message: "source image must be at most 50 MB")
            }
            let name = "edited-\(UUID().uuidString).png"
            let target = workspace.appendingPathComponent(name)
            let dimensions = try await Task.detached(priority: .userInitiated) {
                var original = source
                if portrait {
                    let restored = profileDirectory.withCString { profile in
                        source.path.withCString { maiPortraitOriginalSource(profile, $0) }
                    }
                    guard let restored else { throw AIBackendError(message: "无法读取个人修图记录") }
                    let originalPath = String(cString: restored)
                    maiPortraitFree(restored)
                    guard let resolved = AIAssistantPathPolicy.resolve(originalPath, workspacePath: workspacePath)
                    else { throw AIBackendError(message: "原图不在当前 App 目录内") }
                    original = resolved
                }
                guard let image = CIImage(contentsOf: original,
                                          options: [.applyOrientationProperty: true]) else {
                    throw AIBackendError(message: "无法读取图片")
                }
                let extent = image.extent.integral
                let width = Int(extent.width), height = Int(extent.height)
                guard width > 0, height > 0, Int64(width) * Int64(height) <= 12_000_000 else {
                    throw MaiPortraitHostFailure("image_too_large", stage: "budget",
                        message: "Body or full-raster image exceeds 12 MP",
                        hint: "Face beautification automatically uses bounded native regions; body reshaping currently requires at most 12 MP",
                        width: width, height: height)
                }
                var pixels = Data(count: width * height * 4)
                let context = CIContext()
                pixels.withUnsafeMutableBytes { bytes in
                    context.render(image, toBitmap: bytes.baseAddress!, rowBytes: width * 4,
                                   bounds: extent, format: .RGBA8,
                                   colorSpace: CGColorSpace(name: CGColorSpace.sRGB))
                }
                var operationToRun = operation
                if portrait {
                    var specification = try JSONSerialization.jsonObject(with: Data(operation.utf8)) as! [String: Any]
                    specification["_face_hints"] = []
                    operationToRun = String(data: try JSONSerialization.data(withJSONObject: specification), encoding: .utf8)!
                }
                var portraitMetadata: String?
                let result = pixels.withUnsafeBytes { bytes in
                    operationToRun.withCString { spec in
                        if portrait {
                            let edited = profileDirectory.withCString { profile in
                                original.path.withCString { path in
                                    maiPortraitProcessRgba(bytes.bindMemory(to: UInt8.self).baseAddress,
                                        Int32(width), Int32(height), Int32(width * 4), spec, profile, path)
                                }
                            }
                            if let metadata = edited.metadata {
                                portraitMetadata = String(cString: metadata)
                                maiPortraitFree(metadata)
                            }
                            return MaiImageFilterResult(rgba: edited.rgba, width: edited.width,
                                                        height: edited.height, error: edited.error)
                        }
                        return maiImageFilterRgba(bytes.bindMemory(to: UInt8.self).baseAddress,
                                           Int32(width), Int32(height), Int32(width * 4), spec)
                    }
                }
                if let error = result.error {
                    let message = String(cString: error)
                    maiImageFilterFree(error)
                    throw AIBackendError(message: message)
                }
                guard let output = result.rgba, result.width > 0, result.height > 0 else {
                    throw AIBackendError(message: "图片处理没有返回结果")
                }
                defer { maiImageFilterFree(output) }
                let resultWidth = Int(result.width), resultHeight = Int(result.height)
                let outputData = Data(bytes: output, count: resultWidth * resultHeight * 4)
                guard let provider = CGDataProvider(data: outputData as CFData),
                      let image = CGImage(
                        width: resultWidth, height: resultHeight, bitsPerComponent: 8,
                        bitsPerPixel: 32, bytesPerRow: resultWidth * 4,
                        space: CGColorSpace(name: CGColorSpace.sRGB)!,
                        bitmapInfo: CGBitmapInfo(rawValue: CGImageAlphaInfo.last.rawValue)
                            .union(.byteOrder32Big),
                        provider: provider, decode: nil, shouldInterpolate: true,
                        intent: .defaultIntent),
                      let destination = CGImageDestinationCreateWithURL(
                        target as CFURL, UTType.png.identifier as CFString, 1, nil) else {
                    throw AIBackendError(message: "无法保存处理结果")
                }
                CGImageDestinationAddImage(destination, image, nil)
                guard CGImageDestinationFinalize(destination) else {
                    try? FileManager.default.removeItem(at: target)
                    throw AIBackendError(message: "无法写入处理结果")
                }
                var receipt: String?
                if let portraitMetadata {
                    let registered = profileDirectory.withCString { profile in
                        portraitMetadata.withCString { metadata in
                            target.path.withCString { maiPortraitCommitResult(profile, metadata, $0) }
                        }
                    }
                    guard let registered else { throw AIBackendError(message: "无法保存个人修图记录") }
                    receipt = String(cString: registered)
                    maiPortraitFree(registered)
                }
                return (resultWidth, resultHeight, receipt)
            }.value
            guard workspacePath == AIAssistantModel.shared.workspacePath else {
                return .failure(code: "canceled", message: "Account workspace changed during portrait processing")
            }
            // 图片处理只交付聊天成品；显式全屏预览仍由独立工具响应用户请求。
            var response: [String: Any] = [
                "path": name, "mime_type": "image/png", "width": dimensions.0,
                "height": dimensions.1, "next_tool": "agent_send_media",
                "delivery_hint": "Attach this path to the current AI conversation with agent_send_media (file_path=path, type=image). Open a full-screen preview only when the user explicitly requests it.",
            ]
            if let receipt = dimensions.2,
               let envelope = try JSONSerialization.jsonObject(with: Data(receipt.utf8)) as? [String: Any] {
                guard envelope["ok"] as? Bool == true else {
                    return .failure(code: "invalid_input", message: envelope["error"] as? String ?? "Portrait registration failed")
                }
                if let detail = envelope["output"] as? [String: Any] {
                    response.merge(detail) { _, new in new }
                }
            }
            return Self.jsonSuccess(response)
        } catch let error as MaiPortraitHostFailure {
            return .failure(code: "invalid_input", message: error.detail)
        } catch {
            return .failure(code: "internal", message: error.localizedDescription)
        }
    }

    /// 普通服务完成有界处理后，在 MainActor 上再次核对账号域，再发布产物和训练记录。
    /// 工作区切换会永久取消这一请求，即使之后又切回原账号，也不能继续旧请求。
    private func beautifyImage(_ arguments: [String: Any], encoded: String,
                              source: URL, workspace: URL) async -> AIMaiChatHostToolExecution {
        let model = AIAssistantModel.shared
        let workspacePath = model.workspacePath
        if let expected = arguments["workspace_path"] as? String, expected != workspacePath {
            return .failure(code: "canceled", message: "Account workspace changed before portrait processing")
        }
        let cancellation = MaiPortraitCancellation()
        let observation = model.$workspacePath.dropFirst().sink { value in
            if value != workspacePath { cancellation.cancel() }
        }
        defer { observation.cancel() }
        let profile = workspace.deletingLastPathComponent().appendingPathComponent("PortraitPreferences", isDirectory: true).path
        let allowedRoot = URL(fileURLWithPath: AIAssistantPathPolicy.appRoot(workspacePath: workspacePath), isDirectory: true)
        do {
            let prepared = try await withTaskCancellationHandler {
                try await Task.detached(priority: .userInitiated) {
                    try MaiPortraitPhotoProcessor.prepare(arguments: encoded, sourceURL: source,
                        workspace: workspace, allowedRoot: allowedRoot, profile: profile, cancellation: cancellation)
                }.value
            } onCancel: { cancellation.cancel() }
            guard model === AIAssistantModel.shared, model.workspacePath == workspacePath else {
                cancellation.cancel()
                throw CancellationError()
            }
            // 这段不能跨 await；否则校验之后账号仍可能改变，导致旧请求登记新账号。
            return Self.jsonSuccess(try MaiPortraitPhotoProcessor.publish(prepared, cancellation: cancellation))
        } catch is CancellationError {
            return .failure(code: "canceled", message: "Portrait processing was canceled; no output was committed")
        } catch let error as MaiPortraitHostFailure {
            return .failure(code: "invalid_input", message: error.detail)
        } catch {
            return .failure(code: "internal", message: "Portrait processing failed; no output was committed")
        }
    }

    /// 当前账号的修图反馈和有界个人模型训练；只在后台执行普通 C++ 服务。
    func portraitPersonalization(_ arguments: [String: Any], feedback: Bool = false) async -> AIMaiChatHostToolExecution {
        let workspace = AIAssistantModel.shared.workspacePath
        guard !workspace.isEmpty else { return .failure(code: "not_configured", message: "Current account workspace is unavailable") }
        let profile = URL(fileURLWithPath: workspace).deletingLastPathComponent()
            .appendingPathComponent("PortraitPreferences", isDirectory: true).path
        var request = arguments
        if feedback { request["action"] = "record_feedback" }
        do {
            let encoded = String(data: try JSONSerialization.data(withJSONObject: request), encoding: .utf8)!
            let response = try await Task.detached(priority: .utility) {
                let result = profile.withCString { directory in
                    encoded.withCString { maiPortraitPersonalizationRequest(directory, $0) }
                }
                guard let result else { throw AIBackendError(message: "个人模型服务没有返回结果") }
                defer { maiPortraitFree(result) }
                return String(cString: result)
            }.value
            guard workspace == AIAssistantModel.shared.workspacePath else {
                return .failure(code: "canceled", message: "Account workspace changed during personalization")
            }
            let envelope = try JSONSerialization.jsonObject(with: Data(response.utf8)) as! [String: Any]
            guard envelope["ok"] as? Bool == true else {
                return .failure(code: "invalid_input", message: envelope["error"] as? String ?? "Personalization failed")
            }
            return Self.jsonSuccess(envelope["output"] as? [String: Any] ?? [:])
        } catch { return .failure(code: "internal", message: error.localizedDescription) }
    }

    func addPhotosToAlbum(_ arguments: [String: Any]) async -> AIMaiChatHostToolExecution {
        let status = await photoAuthorization()
        guard status == .authorized || status == .limited else {
            return .failure(code: "canceled", message: "photo library access was not granted")
        }
        guard let rawName = arguments["album_name"] as? String,
              let rawIDs = arguments["photo_ids"] as? [String] else {
            return .failure(code: "invalid_input", message: "album_name and photo_ids are required")
        }
        let name = rawName.trimmingCharacters(in: .whitespacesAndNewlines)
        let ids = Array(Set(rawIDs.map { $0.trimmingCharacters(in: .whitespacesAndNewlines) }))
        guard !name.isEmpty, name.count <= 64, !ids.isEmpty, ids.count <= 50,
              ids.allSatisfy({ !$0.isEmpty }) else {
            return .failure(code: "invalid_input", message: "invalid album name or photo IDs")
        }
        for id in ids {
            guard let asset = PHAsset.fetchAssets(withLocalIdentifiers: [id], options: nil).firstObject,
                  asset.mediaType == .image else {
                return .failure(code: "not_found", message: "a photo is not accessible")
            }
        }

        func findAlbum() -> PHAssetCollection? {
            let albums = PHAssetCollection.fetchAssetCollections(with: .album, subtype: .any, options: nil)
            for index in 0..<albums.count {
                let album = albums.object(at: index)
                if album.localizedTitle == name { return album }
            }
            return nil
        }

        do {
            var album = findAlbum()
            if album == nil {
                try await MaiPhotoLibraryMediaWriter.createAlbum(named: name)
                album = findAlbum()
            }
            guard let album, album.canPerform(.addContent) else {
                return .failure(code: "internal", message: "album cannot be edited")
            }
            try await MaiPhotoLibraryMediaWriter.addPhotos(ids, to: album.localIdentifier)
            return Self.jsonSuccess([
                "album_id": album.localIdentifier, "album_name": name,
                "added_count": ids.count, "copied_originals": false,
            ])
        } catch {
            return .failure(code: "internal", message: error.localizedDescription)
        }
    }

}
