import AppIntents
import Foundation

/// 在“消息”个人自动化中连接“快捷指令输入”；不读取系统收件箱或其他 App 通知。
struct AIMobileSMSImportIntent: AppIntent {
    static let title: LocalizedStringResource = "将短信交给 MaiChat"
    static let description = IntentDescription("将本次自动化传入的短信保存在本机，供你要求 Agent 时读取。需先在 MaiChat 设置中开启短信接入。")
    static let openAppWhenRun = false
    static let authenticationPolicy: IntentAuthenticationPolicy = .alwaysAllowed

    @Parameter(title: "短信内容") var body: String
    @Parameter(title: "发送方") var sender: String?
    @Parameter(title: "收到时间") var receivedAt: Date?

    func perform() async throws -> some IntentResult & ProvidesDialog {
        let settings = LocalSettingsStore().load()
        try await AIMobileSMSInbox.shared.importMessage(body: body, sender: sender ?? "",
                                                       receivedAt: receivedAt, ownerID: settings.masterUserID)
        // 回执不含短信正文，避免验证码出现在 Siri 或系统反馈中。
        return .result(dialog: "已交给 MaiChat；Agent 会在你要求读取时使用。")
    }
}
