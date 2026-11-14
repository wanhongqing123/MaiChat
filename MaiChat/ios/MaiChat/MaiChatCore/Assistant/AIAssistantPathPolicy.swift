// 文件作用：规范创作中心工作区路径和附件路径，阻止越界访问或错误引用。
// 校验创作中心工具可访问的工作区路径。
// 路径解析拒绝越界与不可信别名，文件操作由具体工具执行。
import Foundation
import CoreFoundation

public enum AIAssistantPathPolicy {
    public static func appRoot(workspacePath: String) -> String {
        #if targetEnvironment(simulator)
        let arguments = ProcessInfo.processInfo.arguments
        // 旧合成 fixture 的文件只在 tmp；真实 Primary QA 与产品一样还需访问本 App Documents。
        // 仍以整个 App 沙盒为边界，不信任导入源目录或放宽 native 的路径守卫。
        if arguments.contains("--ai-ui-test") && !arguments.contains("--ai-primary-model-ui-test") {
            return URL(fileURLWithPath: workspacePath, isDirectory: true)
                .deletingLastPathComponent().deletingLastPathComponent().path
        }
        #endif
        return NSHomeDirectory()
    }

    public static func resolve(_ path: String, workspacePath: String,
                        appRootPath: String? = nil) -> URL? {
        guard !path.isEmpty, !workspacePath.isEmpty else { return nil }
        let workspace = URL(fileURLWithPath: workspacePath, isDirectory: true)
            .standardizedFileURL.resolvingSymlinksInPath()
        let root = URL(fileURLWithPath: appRootPath ?? appRoot(workspacePath: workspacePath),
                       isDirectory: true).standardizedFileURL.resolvingSymlinksInPath()
        let unresolved = ((path as NSString).isAbsolutePath
            ? URL(fileURLWithPath: path) : workspace.appendingPathComponent(path))
            .standardizedFileURL
        var existing = unresolved
        var suffix: [String] = []
        while !FileManager.default.fileExists(atPath: existing.path) {
            let parent = existing.deletingLastPathComponent()
            guard parent.path != existing.path else { return nil }
            suffix.insert(existing.lastPathComponent, at: 0)
            existing = parent
        }
        var candidate = existing.resolvingSymlinksInPath().standardizedFileURL
        for component in suffix { candidate.appendPathComponent(component) }
        candidate = candidate.standardizedFileURL
        guard candidate.path == root.path || candidate.path.hasPrefix(root.path + "/") else {
            return nil
        }
        return candidate
    }
}

/// A completed manual capture is display metadata, not image input for a model.
/// Resolve it from the persisted tool receipt so both live and restored messages
/// show the same thumbnail without opening a viewer or changing the composer.
public enum AIAssistantPhotoArtifactPolicy {
    public static func photoURL(tool: String?, state: String?, output: String?,
                                workspacePath: String) -> URL? {
        guard tool == "mobile_take_photo", state == "completed",
              let output, output.utf8.count <= 100_000,
              let data = output.data(using: .utf8),
              let object = (try? JSONSerialization.jsonObject(with: data)) as? [String: Any],
              object["mime_type"] as? String == "image/jpeg",
              let path = object["path"] as? String,
              !path.isEmpty, !(path as NSString).isAbsolutePath,
              ["jpg", "jpeg"].contains((path as NSString).pathExtension.lowercased())
        else { return nil }
        // Capture receipts belong to this workspace. The broader App-container
        // policy used by imported media must not admit a sibling or symlink escape.
        return AIAssistantPathPolicy.resolve(path, workspacePath: workspacePath,
                                             appRootPath: workspacePath)
    }
}

public struct AIAssistantFileArtifact: Equatable, Sendable {
    public let url: URL
    public let name: String
    public let mimeType: String
    public let bytes: Int64
    public let caption: String
}

/// 文件卡片只消费 Core 已完成的交付元数据；不把文件内容传给模型或自动打开文件。
/// 文件移动后仍保留原卡片标题，点击时再检查当前工作区中的普通文件和大小。
public enum AIAssistantFileArtifactPolicy {
    public static func artifact(tool: String?, state: String?, output: String?,
                                workspacePath: String) -> AIAssistantFileArtifact? {
        guard tool == "agent_send_file", state == "completed",
              let output, output.utf8.count <= 100_000,
              let data = output.data(using: .utf8),
              let value = (try? JSONSerialization.jsonObject(with: data)) as? [String: Any],
              value["delivery"] as? String == "current_ai_session", value["type"] as? String == "file",
              let path = value["path"] as? String, !path.isEmpty, !(path as NSString).isAbsolutePath,
              let url = AIAssistantPathPolicy.resolve(path, workspacePath: workspacePath,
                                                     appRootPath: workspacePath),
              let name = value["name"] as? String, name == url.lastPathComponent,
              let mime = value["mime_type"] as? String, !mime.isEmpty,
              let caption = value["caption"] as? String,
              let bytes = value["bytes"] as? NSNumber,
              // 保留 JSON 的整数类型边界；1.0/1e0 或精度折叠的 fraction 不能当成原生整数。
              CFGetTypeID(bytes) != CFBooleanGetTypeID(),
              ["c", "s", "i", "l", "q", "C", "S", "I", "L", "Q"].contains(String(cString: bytes.objCType)),
              bytes.int64Value > 0, bytes.int64Value <= 512 * 1024 * 1024 else { return nil }
        return AIAssistantFileArtifact(url: url, name: name, mimeType: mime,
            bytes: bytes.int64Value, caption: caption)
    }

    // 点击前在后台执行；历史卡片不能用另一工作区、越界链接或已变化的文件替换原附件。
    public static func existingURL(_ artifact: AIAssistantFileArtifact, workspacePath: String) -> URL? {
        guard let candidate = AIAssistantPathPolicy.resolve(artifact.url.path, workspacePath: workspacePath,
                                                           appRootPath: workspacePath),
              candidate == artifact.url, candidate.lastPathComponent == artifact.name,
              let info = try? candidate.resourceValues(forKeys: [.isRegularFileKey, .fileSizeKey]),
              info.isRegularFile == true, Int64(info.fileSize ?? 0) == artifact.bytes else { return nil }
        return candidate
    }
}

public struct AIAssistantMediaMention: Hashable, Identifiable {
    public enum Kind: Hashable { case video, audio }

    public let url: URL
    public let kind: Kind
    public var id: String { url.path }
    public var fileName: String { url.lastPathComponent }
}

public enum AIAssistantMediaMentionPolicy {
    public static let maximumCardsPerMessage = 4

    private static let codePath = try? NSRegularExpression(
        pattern: #"`([^`\r\n]+)`"#,
        options: [.caseInsensitive]
    )
    private static let barePath = try? NSRegularExpression(
        pattern: #"[^\s`()<>\[\]{}"'“”‘’,，。;；:：|]+?\.(?:mp4|mov|wav|mp3|m4a|aac)(?![A-Za-z0-9_.])"#,
        options: [.caseInsensitive]
    )

    // 按一条已完成消息计数。五个及以上保持原文字列表；不存在、越界或代码块内
    // 的名字不产生可点击气泡，避免把清理文件的长清单变成一屏播放器。
    public static func cards(in text: String, workspacePath: String,
                             appRootPath: String? = nil) -> [AIAssistantMediaMention] {
        guard text.count <= 100_000, !workspacePath.isEmpty else { return [] }
        var result: [AIAssistantMediaMention] = []
        var seen = Set<String>()
        var inFence = false
        for lineSlice in text.split(omittingEmptySubsequences: false,
                                    whereSeparator: \.isNewline) {
            let line = String(lineSlice)
            if line.trimmingCharacters(in: .whitespaces).hasPrefix("```") {
                inFence.toggle()
                continue
            }
            if inFence || line.hasPrefix("    ") { continue }
            let bounds = NSRange(line.startIndex..<line.endIndex, in: line)
            let quoted = codePath?.matches(in: line, range: bounds) ?? []
            for match in quoted {
                guard let range = Range(match.range(at: 1), in: line) else { continue }
                appendMediaMention(String(line[range]), workspacePath: workspacePath,
                    appRootPath: appRootPath, seen: &seen, result: &result)
                if result.count > maximumCardsPerMessage { return [] }
            }
            for match in barePath?.matches(in: line, range: bounds) ?? [] {
                guard !quoted.contains(where: {
                    NSIntersectionRange($0.range, match.range).length > 0
                }), let range = Range(match.range, in: line) else { continue }
                appendMediaMention(String(line[range]), workspacePath: workspacePath,
                    appRootPath: appRootPath, seen: &seen, result: &result)
                if result.count > maximumCardsPerMessage { return [] }
            }
        }
        return result
    }

    private static func appendMediaMention(_ raw: String, workspacePath: String, appRootPath: String?,
                            seen: inout Set<String>,
                            result: inout [AIAssistantMediaMention]) {
        let path = raw.trimmingCharacters(in: .whitespacesAndNewlines)
        let suffix = (path as NSString).pathExtension.lowercased()
        let kind: AIAssistantMediaMention.Kind
        switch suffix {
        case "mp4", "mov": kind = .video
        case "wav", "mp3", "m4a", "aac": kind = .audio
        default: return
        }
        guard let url = AIAssistantPathPolicy.resolve(path, workspacePath: workspacePath,
                                                      appRootPath: appRootPath),
              let values = try? url.resourceValues(forKeys: [.isRegularFileKey, .fileSizeKey]),
              values.isRegularFile == true, (values.fileSize ?? 0) > 0,
              seen.insert(url.path).inserted else { return }
        result.append(AIAssistantMediaMention(url: url, kind: kind))
    }
}
