import SwiftUI

struct AIMobileSMSSettingsView: View {
    let ownerID: String
    @Environment(\.scenePhase) private var scenePhase
    @State private var enabled = false
    @State private var count = 0
    @State private var busy = false
    @State private var error = ""

    var body: some View {
        Form {
            Section {
                Toggle("接收快捷指令传入的短信", isOn: Binding(get: { enabled }, set: changeEnabled))
                    .disabled(busy || ownerID.isEmpty)
                    .accessibilityIdentifier("sms-shortcut-enabled")
                LabeledContent("本机保留", value: "\(count) 条")
                Button("清除已接入的短信", role: .destructive) {
                    busy = true
                    Task {
                        do { try await AIMobileSMSInbox.shared.clear(ownerID: ownerID); await refresh() }
                        catch { self.error = error.localizedDescription }
                        busy = false
                    }
                }
                .disabled(busy || count == 0)
                .accessibilityIdentifier("sms-clear-imported")
            } footer: {
                Text("仅保留最近 20 条，24 小时后过期。关闭接入会清除已接收的短信。Agent 读取的内容会用于你要求的模型任务。")
            }
            Section {
                Text("1. 在系统“快捷指令”中创建“信息”个人自动化，选择发送方或包含的文字。")
                Text("2. 添加“将短信交给 MaiChat”，把“快捷指令输入”的消息内容连接到“短信内容”；发送方和收到时间可选。")
                Text("3. 选择立即运行。之后可在创作中心让 Agent 读取已接入的短信。")
            } header: {
                Text("配置一次即可接收新短信")
            } footer: {
                Text("iOS 不允许 MaiChat 直接读取“信息”收件箱。这里只接收你配置的自动化传入的新消息，无法补读历史短信。")
            }
            if !error.isEmpty { Section { Text(error).foregroundStyle(.red) } }
        }
        .appNavigationTitle("短信接入")
        .navigationBarTitleDisplayMode(.inline)
        .task { await refresh() }
        .onChange(of: scenePhase) { phase in if phase == .active { Task { await refresh() } } }
    }

    private func changeEnabled(_ value: Bool) {
        busy = true
        Task {
            do { try await AIMobileSMSInbox.shared.setEnabled(value, ownerID: ownerID); await refresh() }
            catch { self.error = error.localizedDescription }
            busy = false
        }
    }

    private func refresh() async {
        do {
            let status = try await AIMobileSMSInbox.shared.status(ownerID: ownerID)
            enabled = status.enabled
            count = status.count
            error = ""
        } catch { self.error = error.localizedDescription }
    }
}
