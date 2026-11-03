// 网页入口和独立页面共用正式浏览器实例。UIKit 宿主只负责布局与移交，不发起导航。
import SwiftUI
import UIKit
import WebKit

struct AIBrowserTaskEntry: View {
    let sessionID: String
    let isLocal: Bool
    var needsConfirmation = false
    let open: () -> Void
    @ObservedObject private var browser = AIMobileBrowserTools.shared
    @Environment(\.aiConversationActionsAllowed) private var actionsAllowed

    var body: some View {
        if isLocal && browser.isOpen && browser.ownerSessionID == sessionID {
            Button {
                let allowed = actionsAllowed()
                #if DEBUG && targetEnvironment(simulator)
                AIBrowserUITestDiagnostics.trace("entry allowed=\(allowed)")
                #endif
                guard allowed else { return }
                open()
            } label: {
                HStack(spacing: 12) {
                    Image(systemName: "globe")
                        .font(.system(size: 21, weight: .medium))
                        .foregroundStyle(.blue)
                        .frame(width: 40, height: 40)
                        .background(Color.blue.opacity(0.08), in: RoundedRectangle(cornerRadius: 11))
                    VStack(alignment: .leading, spacing: 4) {
                        Text(browser.domain.isEmpty ? "网页任务" : browser.domain)
                            .font(.system(size: 15, weight: .semibold))
                            .foregroundStyle(.primary)
                            .lineLimit(1)
                        HStack(spacing: 6) {
                            if browser.isWorking { ProgressView().controlSize(.mini) }
                            Text(statusText).font(.system(size: 13)).lineLimit(1)
                        }
                        .foregroundStyle(browser.needsUserInput || needsConfirmation ? Color.orange : Color.secondary)
                    }
                    Spacer(minLength: 8)
                    Image(systemName: "chevron.right")
                        .font(.system(size: 12, weight: .semibold)).foregroundStyle(.secondary)
                }
                .padding(12)
                .background(Color(uiColor: .secondarySystemGroupedBackground),
                            in: RoundedRectangle(cornerRadius: 15))
                .overlay(RoundedRectangle(cornerRadius: 15).stroke(Color.primary.opacity(0.08)))
                .contentShape(RoundedRectangle(cornerRadius: 15))
            }
            .buttonStyle(.plain)
            .frame(maxWidth: 540, alignment: .leading)
            .accessibilityIdentifier("ai-browser-task-entry")
            .accessibilityLabel("网页任务，\(browser.domain)，\(statusText)")
        }
    }

    private var statusText: String { needsConfirmation ? "需要你确认" : browser.statusText }
}

struct AIAssistantBrowserScreen: View {
    @ObservedObject var browser: AIMobileBrowserTools
    @ObservedObject private var model = AIAssistantModel.shared
    @Environment(\.openURL) private var openURL
    private var needsConfirmation: Bool { !model.permissions.isEmpty || !model.questions.isEmpty }

    var body: some View {
        NavigationStack {
            AIBrowserWebContainer(browser: browser, presentation: true)
                .background(Color(uiColor: .systemBackground))
                .safeAreaInset(edge: .bottom, spacing: 0) {
                    if needsConfirmation || browser.isWorking || browser.isStopped || !browser.loadError.isEmpty || browser.needsUserInput {
                        statusBar
                    }
                }
                .navigationTitle(browser.domain.isEmpty ? "网页" : browser.domain)
                .navigationBarTitleDisplayMode(.inline)
                .remoteIMNavigationBarMaterial()
                .toolbar {
                    ToolbarItem(placement: .topBarLeading) {
                        Button { browser.dismissPage() } label: {
                            Label("对话", systemImage: "chevron.left")
                        }
                        .accessibilityIdentifier("ai-browser-return")
                    }
                    ToolbarItem(placement: .topBarTrailing) {
                        Menu {
                            Button("刷新", systemImage: "arrow.clockwise") { browser.reload() }
                            if let url = URL(string: browser.pageURL),
                               url.scheme == "http" || url.scheme == "https" {
                                ShareLink(item: url) { Label("分享网页", systemImage: "square.and.arrow.up") }
                                Button("用 Safari 打开", systemImage: "safari") { openURL(url) }
                            }
                            Button("结束网页任务", systemImage: "xmark.circle", role: .destructive) {
                                stopCurrentTask()
                                browser.close()
                            }
                        } label: {
                            Image(systemName: "ellipsis").frame(width: 36, height: 36)
                        }
                        .accessibilityIdentifier("ai-browser-more")
                        .accessibilityLabel("网页更多操作")
                    }
                }
        }
    }

    private var statusBar: some View {
        HStack(spacing: 10) {
            if browser.isWorking { ProgressView().controlSize(.small) }
            Text(needsConfirmation ? "需要你确认" : browser.loadError.isEmpty ? browser.statusText : "网页加载失败")
                .font(.system(size: 13)).foregroundStyle(.secondary).lineLimit(2)
            Spacer(minLength: 8)
            if needsConfirmation {
                // 审批和提问仍由原会话处理，全屏网页只提供明确的返回入口。
                Button("查看") { browser.dismissPage() }
                    .accessibilityIdentifier("ai-browser-confirmation")
            } else if !browser.loadError.isEmpty {
                Button("重试") { browser.reload() }
                    .accessibilityIdentifier("ai-browser-retry")
            } else if browser.isWorking {
                Button("停止") { stopCurrentTask(); browser.stop() }
                    .accessibilityIdentifier("ai-browser-stop")
            } else if browser.isStopped {
                Button("重新加载") { browser.reload() }
            }
        }
        .padding(.horizontal, 18).padding(.vertical, 12)
        .remoteIMBarBackground(edges: .bottom)
        .overlay(alignment: .top) { RemoteIMBarSeparator() }
    }

    private func stopCurrentTask() {
        // 停止按钮同时取消所属 Agent 轮次，避免下一步工具调用立即重新操作刚停下的网页。
        guard model.selected == browser.ownerSessionID, model.selectedRemote == nil, model.busy else { return }
        let sessionID = browser.ownerSessionID
        Task {
            guard model.selected == sessionID, model.selectedRemote == nil else { return }
            await model.action("stop")
        }
    }
}

/// 背景宿主持续提供真实网页视口；全屏时移交同一实例，返回时挂回，保留 DOM、输入和滚动位置。
struct AIBrowserWebContainer: UIViewControllerRepresentable {
    let browser: AIMobileBrowserTools
    let presentation: Bool

    func makeUIViewController(context: Context) -> AIBrowserContainerController {
        AIBrowserContainerController(browser: browser, presentation: presentation)
    }

    func updateUIViewController(_ controller: AIBrowserContainerController, context: Context) {
        controller.attachPage()
    }

    static func dismantleUIViewController(_ controller: AIBrowserContainerController, coordinator: ()) {
        controller.detachPage()
    }
}

final class AIBrowserContainerController: UIViewController {
    private let browser: AIMobileBrowserTools
    private let presentation: Bool

    init(browser: AIMobileBrowserTools, presentation: Bool) {
        self.browser = browser
        self.presentation = presentation
        super.init(nibName: nil, bundle: nil)
    }

    required init?(coder: NSCoder) { fatalError("init(coder:) has not been implemented") }

    override func viewDidLoad() {
        super.viewDidLoad()
        view.backgroundColor = .systemBackground
        view.clipsToBounds = true
        view.accessibilityElementsHidden = !presentation
        view.accessibilityViewIsModal = presentation
        attachPage()
    }

    override func viewDidAppear(_ animated: Bool) {
        super.viewDidAppear(animated)
        attachPage()
        if presentation { browser.restorePresentationPosition(in: view, finished: true) }
    }

    override func viewDidLayoutSubviews() {
        super.viewDidLayoutSubviews()
        if presentation { browser.restorePresentationPosition(in: view, finished: false) }
    }

    override func viewDidDisappear(_ animated: Bool) {
        super.viewDidDisappear(animated)
        // SwiftUI 会缓存已关闭的 cover 控制器，不能只等 dismantle 才归还网页。
        if presentation { browser.detachPresentationContainer(view) }
    }

    func attachPage() {
        guard isViewLoaded else { return }
        // SwiftUI 已把导航栏和底部状态区计入提议尺寸，避免 WebKit 再叠一层安全区。
        browser.webView?.scrollView.contentInsetAdjustmentBehavior = .never
        browser.webView?.accessibilityIdentifier = "ai-browser-web-content"
        if presentation { browser.attachPresentationContainer(view) }
        else { browser.attachRenderingContainer(view) }
    }

    func detachPage() {
        if presentation { browser.detachPresentationContainer(view) }
        else { browser.detachRenderingContainer(view) }
    }
}
