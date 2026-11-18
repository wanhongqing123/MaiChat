// 文件作用：在正常对话消息区展示已经本地保存的待发送消息及真实队列状态。
import MaiChatCore
import SwiftUI

/// One sender appearance across pending rows, accepted question echoes and history.
/// Delivery status remains outside the bubble and does not change its identity/color.
struct AICreationSenderBubble: ViewModifier {
    func body(content: Content) -> some View {
        content.padding(14)
            .background(Color(uiColor: .secondarySystemBackground), in: RoundedRectangle(cornerRadius: 18))
    }
}

struct AIOutgoingMessageRowView: View {
    let row: AIOutgoingMessageRow
    let retry: () -> Void

    var body: some View {
        HStack {
            Spacer(minLength: 30)
            VStack(alignment: .trailing, spacing: 6) {
                VStack(alignment: .leading, spacing: 6) {
                    if !row.payload.text.isEmpty { Text(row.payload.text).textSelection(.enabled) }
                    ForEach(Array(row.payload.files.enumerated()), id: \.offset) { _, file in
                        Label((file.path as NSString).lastPathComponent,
                              systemImage: file.isImage ? "photo" : "paperclip")
                            .font(AppTypography.metadata).lineLimit(2)
                    }
                }
                .font(ChatTypography.body)
                .lineSpacing(ChatTypography.lineSpacing)
                .foregroundStyle(RemoteIMStyle.textPrimary)
                .modifier(AICreationSenderBubble())
                HStack(spacing: 8) {
                    Text(row.status).foregroundStyle(.secondary)
                        .accessibilityIdentifier("outgoing-status-" + row.entry.requestID)
                    if row.allowsRetry {
                        Button("重试确认", action: retry)
                            .accessibilityIdentifier("outgoing-retry-" + row.entry.requestID)
                    }
                }.font(AppTypography.metadata)
            }
        }
        .accessibilityElement(children: .contain)
        .accessibilityIdentifier("outgoing-message-" + row.entry.requestID)
    }
}
