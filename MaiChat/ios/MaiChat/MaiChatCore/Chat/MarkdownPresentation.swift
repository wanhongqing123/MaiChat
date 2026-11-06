// 文件作用：把消息 Markdown 解析为可展示的文本、附件和预览结构。
// 把消息 Markdown 转为可展示与可复制的内容结构。
// 附件标题和代码内容保持原意，不把文件名误当格式语法。
import Foundation

/// 在解析器给出的行内 HTML 节点中呈现换行，不改写 Markdown 原文或代码字面量。
public enum MarkdownInlinePresentation {
    private static let lineBreak = try! NSRegularExpression(
        pattern: "\\A<br[ \\t]*/?>\\z", options: .caseInsensitive)

    public static func parse(_ markdown: String) -> AttributedString {
        // 源码位置让相邻 HTML 标签保持独立 run，避免把注释或属性里的 <br> 当作换行。
        let parsed = (try? AttributedString(markdown: markdown, options: .init(
            interpretedSyntax: .inlineOnlyPreservingWhitespace,
            appliesSourcePositionAttributes: true))) ?? AttributedString(markdown)
        return renderingLineBreaks(in: parsed)
    }

    static func renderingLineBreaks(in source: AttributedString) -> AttributedString {
        var result = source
        let ranges = result.runs.compactMap { run -> Range<AttributedString.Index>? in
            guard run.inlinePresentationIntent?.contains(.inlineHTML) == true else { return nil }
            let text = String(result[run.range].characters)
            return lineBreak.firstMatch(in: text, range: NSRange(text.startIndex..., in: text)) != nil
                ? run.range : nil
        }
        // 从后向前替换，保持尚未处理的字符区间有效；粗体和链接等外层属性继续保留。
        for range in ranges.reversed() {
            var replacement = AttributedString("\n", attributes: result[range].runs.first!.attributes)
            replacement.inlinePresentationIntent?.remove(.inlineHTML)
            result.replaceSubrange(range, with: replacement)
        }
        result.markdownSourcePosition = nil
        return result
    }
}

private final class ConversationPreviewCache: @unchecked Sendable {
    let values = NSCache<NSString, NSString>()
    init() {
        values.countLimit = 512
        values.totalCostLimit = 2 * 1024 * 1024
    }
}

public enum MarkdownConversationPreview {
    private static let cache = ConversationPreviewCache()

    public static func text(for message: RemoteIMMessage?) -> String {
        guard let message else { return "暂无消息" }
        // 保留附件标题和占位内容，不把文件名当作 Markdown 语法解析。
        if message.imageAttachment != nil || message.fileAttachment != nil
            || message.videoAttachment != nil || message.voiceAttachment != nil {
            return message.text
        }
        let preview = plainText(message.text)
        return preview.isEmpty ? "新消息" : preview
    }

    /// 从 Markdown 结构提取适合通知和摘要展示的纯文本。
    /// 跳过格式标记但保留正文顺序，避免把代码或链接路径误当消息主体。
    public static func plainText(_ markdown: String) -> String {
        let input = String(String.UnicodeScalarView(markdown.unicodeScalars.prefix(8192)))
        let key = input as NSString
        if let cached = cache.values.object(forKey: key) { return cached as String }
        var blocks: [String] = []
        if let document = try? AttributedString(markdown: input, options: .init(
            appliesSourcePositionAttributes: true)) {
            let parsed = MarkdownInlinePresentation.renderingLineBreaks(in: document)
            var blockID: Int?
            var text = ""
            var quote = false
            var list = false
            var literalStart = false
            func flush() {
                guard !text.isEmpty else { return }
                if !literalStart, list, let task = MarkdownTaskPresentation.parse(text) {
                    text = (task.checked ? "☑ " : "☐ ") + task.text
                } else if !literalStart, quote, text.hasPrefix("[!"), let close = text.firstIndex(of: "]") {
                    let marker = MarkdownQuotePresentation(String(text[...close]))
                    let rest = text[text.index(after: close)...]
                    if let kind = marker.kind, rest.isEmpty || rest.first?.isWhitespace == true {
                        text = kind.title + "：" + rest.trimmingCharacters(in: .whitespacesAndNewlines)
                    }
                }
                blocks.append(text)
                text = ""
            }
            // 完整 Markdown 解析去掉格式标记但保留代码内容；
            // 标题、段落和表格单元格保持独立块标识，避免纯文本拼接成一行。
            for run in parsed.runs {
                let components = run.presentationIntent?.components ?? []
                if components.contains(where: { if case .thematicBreak = $0.kind { return true }; return false }) {
                    flush()
                    blockID = nil
                    continue
                }
                let id = components.first?.identity
                if id != blockID { flush(); blockID = id }
                if text.isEmpty {
                    quote = components.contains { if case .blockQuote = $0.kind { return true }; return false }
                    list = components.contains { if case .listItem = $0.kind { return true }; return false }
                    literalStart = run.inlinePresentationIntent?.contains(.code) == true || run.link != nil
                        || components.contains { if case .codeBlock = $0.kind { return true }; return false }
                }
                text += String(parsed[run.range].characters)
            }
            flush()
        } else {
            blocks = [input]
        }
        let compact = blocks.joined(separator: " ").split(whereSeparator: \.isWhitespace).joined(separator: " ")
        let result = compact.count > 160 ? String(compact.prefix(160)) + "…" : compact
        cache.values.setObject(result as NSString, forKey: key, cost: input.utf8.count + result.utf8.count)
        return result
    }
}

/// 仅保存呈现元数据，不改写原始消息正文。
public struct MarkdownQuotePresentation: Equatable, Sendable {
    public enum Kind: String, CaseIterable, Sendable {
        case note = "NOTE", tip = "TIP", important = "IMPORTANT", warning = "WARNING", caution = "CAUTION"

        public var title: String {
            switch self {
            case .note: "提示"
            case .tip: "建议"
            case .important: "重要"
            case .warning: "注意"
            case .caution: "警告"
            }
        }
    }

    public let kind: Kind?
    public let text: String

    public init(_ quote: String) {
        let normalized = quote.replacingOccurrences(of: "\r\n", with: "\n")
            .replacingOccurrences(of: "\r", with: "\n")
        let lines = normalized.split(separator: "\n", maxSplits: 1, omittingEmptySubsequences: false)
        let first = String(lines.first ?? "").trimmingCharacters(in: .whitespacesAndNewlines).uppercased()
        if let kind = Kind.allCases.first(where: { first == "[!\($0.rawValue)]" }) {
            self.kind = kind
            self.text = lines.count > 1 ? String(lines[1]) : ""
        } else {
            self.kind = nil
            self.text = quote
        }
    }
}

public struct MarkdownTaskPresentation: Equatable, Sendable {
    public let checked: Bool
    public let text: String

    public static func parse(_ text: String) -> Self? {
        for (prefix, checked) in [("[ ]", false), ("[x]", true), ("[X]", true)] {
            guard text.hasPrefix(prefix) else { continue }
            let rest = text.dropFirst(prefix.count)
            guard rest.isEmpty || rest.first == " " || rest.first == "\t" else { return nil }
            return Self(checked: checked, text: String(rest.drop(while: { $0 == " " || $0 == "\t" })))
        }
        return nil
    }
}
