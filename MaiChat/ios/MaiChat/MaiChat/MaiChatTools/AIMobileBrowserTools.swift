import Foundation
import SwiftUI
import UIKit
import WebKit

// 会话持有一个独立网页。任务入口与全屏页面共用同一 WKWebView，收起页面不重载。
// 固定脚本检查和操作网页；模型输入不作为 JavaScript 执行。
@MainActor
final class AIMobileBrowserTools: NSObject, ObservableObject, WKNavigationDelegate {
    static let shared = AIMobileBrowserTools()

    @Published private(set) var ownerSessionID = ""
    @Published private(set) var sourceMessageID = ""
    @Published private(set) var sourcePartID = ""
    @Published private(set) var pageTitle = ""
    @Published private(set) var pageURL = ""
    @Published private(set) var state = ""
    @Published private(set) var generation = 0
    @Published private(set) var isPresented = false
    @Published private(set) var isWorking = false
    @Published private(set) var needsUserInput = false
    @Published private(set) var loadError = ""

    private(set) var webView: WKWebView?
    private var allowedHost = ""
    private var snapshotID = ""
    private var observedLabels: [String: String] = [:]
    private var pageVisible = false
    private var loaded = false
    private var documentCommitted = false
    private var navigationRevision = 0
    private var stopped = false
    // WebKit 操作会在 MainActor 的 await 处交错；最后一个操作结束才退出忙碌态。
    private var activeOperations = Set<UUID>()
    // 收起时仍在聊天背景的非交互宿主中布局和渲染，支持真实 DOM 读取与截图。
    private weak var renderingContainer: UIView?
    private weak var presentationContainer: UIView?
    private var savedScrollOffset: CGPoint?
    private var pendingScrollOffset: CGPoint?
    #if DEBUG && targetEnvironment(simulator)
    private var fixturePage = false
    #endif

    var isOpen: Bool { webView != nil }
    var isStopped: Bool { stopped }
    var domain: String { URL(string: pageURL)?.host ?? pageTitle }
    var statusText: String {
        if !state.isEmpty { return state }
        return needsUserInput ? "需要你操作" : "网页已就绪"
    }

    func presentPage(sessionID: String) {
        #if DEBUG && targetEnvironment(simulator)
        AIBrowserUITestDiagnostics.trace("present visible=\(pageVisible) selected=\(AIAssistantModel.shared.selected == sessionID) owner=\(ownerSessionID == sessionID)")
        #endif
        guard isOpen, ownerSessionID == sessionID, pageVisible,
              AIAssistantModel.shared.selected == sessionID,
              AIAssistantModel.shared.selectedRemote == nil else { return }
        pendingScrollOffset = savedScrollOffset
        isPresented = true
    }

    func dismissPage() {
        if isPresented { savedScrollOffset = webView?.scrollView.contentOffset }
        isPresented = false
    }

    func stop() {
        // 序号同时作废正在等待 WebKit 的旧操作，防止停止后的结果更新页面或写截图。
        generation += 1
        stopped = true
        activeOperations.removeAll()
        isWorking = false
        snapshotID = ""
        observedLabels.removeAll()
        webView?.stopLoading()
        state = "已停止网页操作"
    }

    func reload() {
        guard let page = webView else { return }
        let retryURL = (!loadError.isEmpty || page.url == nil) ? URL(string: pageURL) : nil
        generation += 1
        stopped = false
        activeOperations.removeAll()
        loadError = ""
        needsUserInput = false
        isWorking = true
        state = "正在加载网页…"
        snapshotID = ""
        observedLabels.removeAll()
        // 首次导航失败时没有已提交的历史项，WKWebView.reload() 可能不会发出请求。
        if let retryURL { page.load(URLRequest(url: retryURL)) }
        else { page.reload() }
    }

    func attachRenderingContainer(_ container: UIView) {
        renderingContainer = container
        mountPage()
    }

    func detachRenderingContainer(_ container: UIView) {
        if renderingContainer === container { renderingContainer = nil }
    }

    func attachPresentationContainer(_ container: UIView) {
        guard isPresented else { return }
        presentationContainer = container
        mountPage()
    }

    func detachPresentationContainer(_ container: UIView) {
        if presentationContainer === container {
            presentationContainer = nil
            mountPage()
        }
    }

    func restorePresentationPosition(in container: UIView, finished: Bool) {
        guard presentationContainer === container, let offset = pendingScrollOffset,
              let page = webView, page.window != nil,
              container.bounds.width > 0, container.bounds.height > 0 else { return }
        // 两个宿主高度不同，WebKit 会在移交时钳制 offset。全屏布局完成后恢复原阅读位置，
        // 动画完成即消费本轮恢复值，后续键盘、旋转和手指滚动不再被旧值覆盖。
        page.layoutIfNeeded()
        page.scrollView.setContentOffset(offset, animated: false)
        if finished { pendingScrollOffset = nil }
    }

    private func mountPage() {
        guard let page = webView,
              let container = presentationContainer ?? renderingContainer else { return }
        let interactive = isPresented && presentationContainer != nil
        if !interactive { page.endEditing(true) }
        // WebKit 的独立无障碍树和输入焦点不会随 reparent 自动隐藏，必须随宿主同步移交。
        page.isUserInteractionEnabled = interactive
        page.accessibilityElementsHidden = !interactive
        if page.superview !== container {
            #if DEBUG && targetEnvironment(simulator)
            AIBrowserUITestDiagnostics.trace("mount presentation=\(interactive)")
            #endif
            page.removeFromSuperview()
            page.frame = container.bounds
            page.autoresizingMask = [.flexibleWidth, .flexibleHeight]
            container.addSubview(page)
        }
    }

    // Ordinary App callers use these typed entry points; Agent calls share execute().
    func openPage(_ url: URL, sessionID: String, sourceMessageID: String? = nil,
                  sourcePartID: String? = nil) async -> AIMaiChatHostToolExecution {
        var arguments: [String: Any] = ["action": "open", "session_id": sessionID, "url": url.absoluteString]
        if let sourceMessageID { arguments["message_id"] = sourceMessageID }
        if let sourcePartID { arguments["part_id"] = sourcePartID }
        return await execute(arguments)
    }

    func observePage(sessionID: String, waitMilliseconds: Int = 5000) async -> AIMaiChatHostToolExecution {
        await execute(["action": "observe", "session_id": sessionID, "wait_ms": waitMilliseconds])
    }

    func capturePage(sessionID: String, expectedURL: String,
                     outputPath: String) async -> AIMaiChatHostToolExecution {
        await execute(["action": "screenshot", "session_id": sessionID,
                       "expected_url": expectedURL, "output_path": outputPath])
    }

    func actOnPage(sessionID: String, action: String, expectedURL: String,
                   snapshotID: String, elementID: String, expectedLabel: String,
                   text: String = "") async -> AIMaiChatHostToolExecution {
        var arguments: [String: Any] = ["action": action, "session_id": sessionID,
                                        "expected_url": expectedURL,
                                        "snapshot_id": snapshotID,
                                        "element_id": elementID,
                                        "expected_label": expectedLabel]
        if action == "type" { arguments["text"] = text }
        return await execute(arguments)
    }

    func setPageVisible(_ visible: Bool) {
        #if DEBUG && targetEnvironment(simulator)
        if pageVisible != visible {
            AIBrowserUITestDiagnostics.trace("active=\(visible) presented=\(isPresented)")
        }
        #endif
        if pageVisible && !visible { invalidatePendingOperations() }
        pageVisible = visible
    }

    func leaveSession() {
        dismissPage()
        invalidatePendingOperations()
    }

    private func invalidatePendingOperations() {
        generation += 1
        activeOperations.removeAll()
        snapshotID = ""
        observedLabels.removeAll()
        isWorking = webView?.isLoading == true && !stopped
        if !isWorking && !stopped && loadError.isEmpty { state = "" }
    }

    func close() {
        isPresented = false
        resetPage()
    }

    private func resetPage() {
        webView?.stopLoading()
        webView?.navigationDelegate = nil
        webView?.removeFromSuperview()
        webView = nil
        allowedHost = ""
        snapshotID = ""
        observedLabels.removeAll()
        loaded = false
        documentCommitted = false
        activeOperations.removeAll()
        ownerSessionID = ""
        sourceMessageID = ""
        sourcePartID = ""
        pageTitle = ""
        pageURL = ""
        state = ""
        isWorking = false
        needsUserInput = false
        loadError = ""
        stopped = false
        savedScrollOffset = nil
        pendingScrollOffset = nil
        #if DEBUG && targetEnvironment(simulator)
        fixturePage = false
        #endif
        generation += 1
    }

    func execute(_ arguments: [String: Any]) async -> AIMaiChatHostToolExecution {
        guard let action = arguments["action"] as? String,
              let sessionID = arguments["session_id"] as? String,
              !sessionID.isEmpty else {
            return .failure(code: "invalid_input", message: "浏览器操作缺少所属 Session")
        }
        if action == "close" {
            guard ownerSessionID == sessionID else {
                return .failure(code: "not_found", message: "此 Session 没有打开的网页")
            }
            close()
            return Self.jsonSuccess(["state": "closed"])
        }
        guard pageVisible,
              AIAssistantModel.shared.selected == sessionID,
              AIAssistantModel.shared.selectedRemote == nil else {
            return .failure(code: "not_configured",
                            message: "请先在前台打开此设备的创作中心 Session")
        }
        if action == "open" {
            guard let raw = arguments["url"] as? String,
                  let parts = URLComponents(string: raw),
                  let url = parts.url,
                  (parts.scheme == "http" || parts.scheme == "https"),
                  let host = parts.host, !host.isEmpty,
                  parts.user == nil, parts.password == nil else {
                return .failure(code: "invalid_input", message: "请提供不含账号密码的 HTTP(S) 网址")
            }
            // 同一会话替换网页时保留全屏容器，避免工具 open 把用户弹回聊天。
            if ownerSessionID != sessionID { isPresented = false }
            resetPage()
            let configuration = WKWebViewConfiguration()
            configuration.websiteDataStore = .nonPersistent()
            let page = WKWebView(frame: .zero, configuration: configuration)
            page.navigationDelegate = self
            webView = page
            allowedHost = host.lowercased()
            ownerSessionID = sessionID
            // 普通代码没有工具上下文时，记录调用当时的末条消息；后续追加不重新计算位置。
            sourceMessageID = (arguments["message_id"] as? String).flatMap { $0.isEmpty ? nil : $0 }
                ?? AIAssistantModel.shared.messages.last?.id ?? ""
            sourcePartID = arguments["part_id"] as? String ?? ""
            pageURL = url.absoluteString
            pageTitle = host
            state = "正在加载网页…"
            isWorking = true
            generation += 1
            mountPage()
            page.load(URLRequest(url: url))
            return Self.jsonSuccess(["state": "loading", "url": url.absoluteString,
                                "next_step": "Call observe; it waits briefly for a usable document."])
        }
        guard ownerSessionID == sessionID, let page = webView else {
            return .failure(code: "not_found", message: "此 Session 没有打开的网页")
        }
        guard !stopped else {
            return .failure(code: "canceled", message: "网页操作已停止，需要新任务时请重新打开网页")
        }
        let operationGeneration = generation
        func isCurrentOperation() -> Bool {
            !Task.isCancelled && webView === page && generation == operationGeneration && !stopped && pageVisible &&
                AIAssistantModel.shared.selected == sessionID &&
                AIAssistantModel.shared.selectedRemote == nil
        }
        let canceled = AIMaiChatHostToolExecution.failure(code: "canceled", message: "网页操作已取消或会话已变化")
        if action == "observe" {
            let wait = (arguments["wait_ms"] as? Int) ?? 5000
            guard (0...10000).contains(wait) else {
                return Self.targetFailure("invalid_wait", "wait_ms 必须在 0–10000 毫秒之间")
            }
            let operation = beginOperation("正在读取网页…")
            defer { endOperation(operation, generation: operationGeneration) }
            let deadline = ContinuousClock.now.advanced(by: .milliseconds(wait))
            do {
                while true {
                    guard isCurrentOperation() else { return canceled }
                    if !loadError.isEmpty {
                        return .failure(code: "internal", message: "[navigation_failed] 网页加载失败：\(loadError)")
                    }
                    var result: [String: Any] = ["url": pageURL, "title": pageTitle,
                                                 "text": "", "elements": []]
                    // 导航提交前仍可能读到上一页 DOM，不能把它误报成新网页。
                    let observedRevision = navigationRevision
                    if documentCommitted || loaded {
                        let nonce = UUID().uuidString.replacingOccurrences(of: "-", with: "")
                        result = try await evaluateObject(Self.snapshotScript(nonce), on: page)
                    }
                    guard isCurrentOperation() else { return canceled }
                    if observedRevision != navigationRevision {
                        result = ["url": pageURL, "title": pageTitle, "text": "", "elements": []]
                    }
                    let ready = result["document_ready"] as? Bool == true
                    if ready || ContinuousClock.now >= deadline {
                        if let id = result["snapshot_id"] as? String {
                            if snapshotID != id { observedLabels.removeAll() }
                            snapshotID = id
                            for element in result["elements"] as? [[String: Any]] ?? [] {
                                if let elementID = element["id"] as? String, let label = element["label"] as? String {
                                    observedLabels[elementID] = label
                                }
                            }
                            if observedLabels.count > 512 {
                                let current = result["elements"] as? [[String: Any]] ?? []
                                observedLabels = Dictionary(uniqueKeysWithValues: current.compactMap { element in
                                    guard let id = element["id"] as? String, let label = element["label"] as? String else { return nil }
                                    return (id, label)
                                })
                            }
                        }
                        result["state"] = ready ? "ready" : "loading"
                        result["partial"] = !ready
                        result["loading"] = page.isLoading
                        if !ready { result["retry_after_ms"] = 300 }
                        pageTitle = result["title"] as? String ?? pageTitle
                        pageURL = result["url"] as? String ?? pageURL
                        needsUserInput = result["needs_user_input"] as? Bool ?? false
                        state = ready ? "" : "正在加载网页…"
                        return Self.jsonSuccess(result)
                    }
                    try await Task.sleep(for: .milliseconds(100))
                }
            } catch {
                guard isCurrentOperation() else { return canceled }
                state = "读取网页失败"
                return .failure(code: "internal", message: "读取网页失败：\(error.localizedDescription)")
            }
        }
        if action == "screenshot" {
            guard documentCommitted || loaded,
                  let expectedURL = arguments["expected_url"] as? String,
                  page.url?.absoluteString == expectedURL,
                  let requestedPath = arguments["output_path"] as? String,
                  let destination = authorizedOutputPath(requestedPath) else {
                return .failure(code: "invalid_input", message: "网页未加载或截图路径无效")
            }
            let operation = beginOperation("正在截取网页…")
            defer { endOperation(operation, generation: operationGeneration) }
            do {
                let image = try await withCheckedThrowingContinuation {
                    (continuation: CheckedContinuation<UIImage, Error>) in
                    page.takeSnapshot(with: nil) { image, error in
                        if let image { continuation.resume(returning: image) }
                        else { continuation.resume(throwing: error ?? BrowserFailure.emptyScreenshot) }
                    }
                }
                guard isCurrentOperation() else { return canceled }
                guard page.url?.absoluteString == expectedURL else {
                    return Self.targetFailure("url_changed", "截图期间发生了导航，请重新 observe")
                }
                guard let png = image.pngData(), !png.isEmpty,
                      png.count <= 20 * 1024 * 1024 else {
                    return .failure(code: "internal", message: "网页截图为空或超出大小限制")
                }
                try png.write(to: destination, options: .atomic)
                state = ""
                return Self.jsonSuccess(["state": "captured", "url": page.url?.absoluteString ?? pageURL,
                                    "width": Int(image.size.width * image.scale),
                                    "height": Int(image.size.height * image.scale)])
            } catch {
                guard isCurrentOperation() else { return canceled }
                state = "网页截图失败"
                return .failure(code: "internal", message: "网页截图失败：\(error.localizedDescription)")
            }
        }
        if action == "click" || action == "type" {
            guard let expectedURL = arguments["expected_url"] as? String,
                  let requestedSnapshot = arguments["snapshot_id"] as? String,
                  let elementID = arguments["element_id"] as? String,
                  let expectedLabel = arguments["expected_label"] as? String else {
                return Self.targetFailure("missing_target", "请提供 observe 返回的网址、快照和目标标识")
            }
            guard !snapshotID.isEmpty, requestedSnapshot == snapshotID else {
                return Self.targetFailure("snapshot_expired", "快照已失效或属于其他文档，请重新 observe")
            }
            guard page.url?.absoluteString == expectedURL else {
                return Self.targetFailure("url_changed", "当前网址与 expected_url 不同，请重新 observe")
            }
            guard let observedLabel = observedLabels[elementID] else {
                return Self.targetFailure("element_not_observed", "element_id 不属于已观察到的目标")
            }
            guard observedLabel == expectedLabel else {
                return Self.targetFailure("label_mismatch", "expected_label 与已观察到的控件文案不符")
            }
            let value = arguments["text"] as? String ?? ""
            let operation = beginOperation(action == "click" ? "正在操作网页…" : "正在输入内容…")
            defer { endOperation(operation, generation: operationGeneration) }
            do {
                let script = Self.actionScript(action, id: elementID,
                                               label: expectedLabel, text: value,
                                               snapshot: requestedSnapshot, url: expectedURL)
                var result = try await evaluateObject(script, on: page)
                guard isCurrentOperation() else { return canceled }
                if let reason = result["error"] as? String {
                    state = reason == "manual_input_required" ? "需要你输入密码或验证码" : "请检查网页目标"
                    needsUserInput = reason == "manual_input_required"
                    return Self.targetFailure(reason, result["message"] as? String ?? "请重新 observe 核对目标")
                }
                // 同一文档中不变的目标继续有效；脚本按节点身份、标签和类型逐次核对。
                let needsFile = result["reason"] as? String == "file_selection_required"
                needsUserInput = needsFile || needsUserInput
                state = needsFile ? "需要你选择上传文件" : ""
                result["action"] = action
                result["snapshot_id"] = requestedSnapshot
                result["element_id"] = elementID
                result["verified"] = false
                return Self.jsonSuccess(result)
            } catch {
                guard isCurrentOperation() else { return canceled }
                state = "网页操作失败"
                return .failure(code: "internal", message: "网页操作失败：\(error.localizedDescription)")
            }
        }
        return .failure(code: "invalid_input", message: "未知的网页操作")
    }

    func webView(_ webView: WKWebView, decidePolicyFor action: WKNavigationAction,
                 decisionHandler: @escaping (WKNavigationActionPolicy) -> Void) {
        guard webView === self.webView, !stopped else { decisionHandler(.cancel); return }
        #if DEBUG && targetEnvironment(simulator)
        if fixturePage && action.request.url?.scheme == "about" {
            decisionHandler(.allow)
            return
        }
        #endif
        guard action.targetFrame?.isMainFrame == true,
              let url = action.request.url,
              (url.scheme == "http" || url.scheme == "https"),
              url.host?.lowercased() == allowedHost else {
            state = "已阻止离开当前网站；需要打开新网址请重新授权"
            isWorking = false
            needsUserInput = true
            decisionHandler(.cancel)
            return
        }
        snapshotID = ""
        observedLabels.removeAll()
        decisionHandler(.allow)
    }

    func webView(_ webView: WKWebView, didStartProvisionalNavigation navigation: WKNavigation!) {
        guard webView === self.webView, !stopped else { return }
        loaded = false
        documentCommitted = false
        navigationRevision += 1
        snapshotID = ""
        observedLabels.removeAll()
        savedScrollOffset = nil
        pendingScrollOffset = nil
        loadError = ""
        isWorking = true
        state = "正在加载网页…"
    }

    func webView(_ webView: WKWebView, didCommit navigation: WKNavigation!) {
        guard webView === self.webView, !stopped else { return }
        documentCommitted = true
        pageURL = webView.url?.absoluteString ?? pageURL
    }

    func webView(_ webView: WKWebView, didFinish navigation: WKNavigation!) {
        guard webView === self.webView, !stopped else { return }
        loaded = true
        documentCommitted = true
        isWorking = !activeOperations.isEmpty
        pageURL = webView.url?.absoluteString ?? pageURL
        pageTitle = webView.title ?? pageTitle
        state = ""
    }

    func webView(_ webView: WKWebView, didFailProvisionalNavigation navigation: WKNavigation!,
                 withError error: Error) {
        failNavigation(webView, error: error)
    }

    func webView(_ webView: WKWebView, didFail navigation: WKNavigation!, withError error: Error) {
        failNavigation(webView, error: error)
    }

    private func failNavigation(_ page: WKWebView, error: Error) {
        guard page === webView, !stopped,
              (error as NSError).code != NSURLErrorCancelled else { return }
        loaded = false
        isWorking = !activeOperations.isEmpty
        loadError = error.localizedDescription
        state = "网页加载失败：\(error.localizedDescription)"
    }

    private func beginOperation(_ message: String) -> UUID {
        let id = UUID()
        activeOperations.insert(id)
        isWorking = true
        state = message
        return id
    }

    private func endOperation(_ id: UUID, generation expectedGeneration: Int) {
        guard generation == expectedGeneration else { return }
        activeOperations.remove(id)
        isWorking = !activeOperations.isEmpty || (webView?.isLoading == true && !stopped && loadError.isEmpty)
    }

    // 私有 content world 中的 WeakMap 记录真实节点；网页看不到或伪造不了 Agent 标识。
    private func evaluateObject(_ script: String, on page: WKWebView) async throws -> [String: Any] {
        let value = try await page.evaluateJavaScript(script, in: nil, contentWorld: .defaultClient)
        guard let raw = value as? String, let data = raw.data(using: .utf8),
              let result = try JSONSerialization.jsonObject(with: data) as? [String: Any] else {
            throw BrowserFailure.invalidStructure
        }
        return result
    }

    private static func targetFailure(_ reason: String, _ message: String) -> AIMaiChatHostToolExecution {
        .failure(code: "invalid_input", message: "[\(reason)] \(message)")
    }

    private func authorizedOutputPath(_ path: String) -> URL? {
        guard !path.isEmpty, path.hasSuffix(".png"),
              !AIAssistantModel.shared.workspacePath.isEmpty else { return nil }
        let root = URL(fileURLWithPath: AIAssistantModel.shared.workspacePath)
            .standardizedFileURL.resolvingSymlinksInPath().path
        let destination = URL(fileURLWithPath: path).standardizedFileURL
        let parent = destination.deletingLastPathComponent()
            .resolvingSymlinksInPath().path
        guard parent.hasPrefix(root + "/") else { return nil }
        return destination
    }

    private static func jsonSuccess(_ object: [String: Any]) -> AIMaiChatHostToolExecution {
        guard JSONSerialization.isValidJSONObject(object),
              let data = try? JSONSerialization.data(withJSONObject: object),
              let text = String(data: data, encoding: .utf8) else {
            return .failure(code: "internal", message: "无法编码网页结果")
        }
        return .success(text)
    }

    private static func literal(_ value: String) -> String {
        guard let data = try? JSONEncoder().encode(value),
              let text = String(data: data, encoding: .utf8) else { return "\"\"" }
        return text
    }

    private static let scriptHelpers = #"""
      const normalize = s => (s || '').replace(/\s+/g, ' ').trim().slice(0, 160);
      const visible = e => {
        const r = e.getBoundingClientRect(), s = getComputedStyle(e);
        return r.width > 0 && r.height > 0 && s.visibility !== 'hidden' && s.display !== 'none';
      };
      const labelFor = e => {
        const references = (e.getAttribute('aria-labelledby') || '').split(/\s+/)
          .map(id => document.getElementById(id)?.textContent || '').join(' ');
        const associated = Array.from(e.labels || []).map(l => l.innerText || l.textContent).join(' ');
        return normalize(references) || normalize(e.getAttribute('aria-label')) || normalize(associated) ||
          normalize(e.innerText) || (['submit','button','reset'].includes(e.type) ? normalize(e.value) : '') ||
          normalize(e.getAttribute('placeholder')) || normalize(e.getAttribute('title')) ||
          normalize(e.getAttribute('name')) || e.tagName.toLowerCase();
      };
      const sensitive = e => e.type === 'password' || (e.autocomplete || '').toLowerCase().split(/\s+/)
        .some(t => ['current-password','new-password','one-time-code'].includes(t));
      const shape = e => [e.tagName, e.type || '', e.name || '', e.getAttribute('role') || ''].join('|');
    """#

    private static func snapshotScript(_ nonce: String) -> String {
        #"""
        (() => {
          \#(scriptHelpers)
          let registry = globalThis.__maichatBrowserTargets;
          if (!registry || registry.document !== document) {
            registry = {document, token: \#(literal(nonce)), next: 0, ids: new WeakMap(), nodes: new Map()};
            globalThis.__maichatBrowserTargets = registry;
          }
          for (const [id, target] of registry.nodes) {
            if (!target.node.isConnected) registry.nodes.delete(id);
          }
          const candidates = Array.from(document.querySelectorAll(
            'a[href],button,input,textarea,select,[role="button"]')).filter(visible).slice(0, 80);
          const elements = candidates.map(e => {
            let id = registry.ids.get(e);
            if (!id) { id = 'mai_' + (++registry.next); registry.ids.set(e, id); }
            registry.nodes.set(id, {node: e, shape: shape(e)});
            const item = {id, label: labelFor(e), tag: e.tagName.toLowerCase(), type: e.type || '',
              name: e.name || '', disabled: e.matches(':disabled'), readonly: !!e.readOnly};
            const group = e.closest('fieldset')?.querySelector(':scope > legend');
            if (group) item.group = normalize(group.textContent);
            if (['checkbox','radio'].includes(e.type)) item.checked = e.checked;
            if (!sensitive(e) && e.type !== 'file' && 'value' in e) item.value = e.value.slice(0, 512);
            if (sensitive(e)) item.requires_user_input = true;
            if (e.type === 'file') item.requires_user_selection = true;
            return item;
          });
          while (registry.nodes.size > 512) registry.nodes.delete(registry.nodes.keys().next().value);
          const needsUserInput = candidates.some(e => sensitive(e) && !e.value);
          return JSON.stringify({snapshot_id: registry.token, url: location.href, title: document.title,
            document_ready: !!document.body && document.readyState !== 'loading',
            needs_user_input: needsUserInput, text: (document.body?.innerText || '').slice(0, 20000), elements});
        })()
        """#
    }

    private static func actionScript(_ action: String, id: String, label: String,
                                     text: String, snapshot: String, url: String) -> String {
        #"""
        (() => {
          \#(scriptHelpers)
          const error = (reason, message) => JSON.stringify({error: reason, message});
          const registry = globalThis.__maichatBrowserTargets;
          if (!registry || registry.document !== document || registry.token !== \#(literal(snapshot)))
            return error('snapshot_expired', 'The document snapshot expired; observe again.');
          if (location.href !== \#(literal(url))) return error('url_changed', 'The page URL changed; observe again.');
          const target = registry.nodes.get(\#(literal(id))), e = target?.node;
          if (!e || !e.isConnected || e.ownerDocument !== document)
            return error('element_detached', 'The observed element was removed or replaced; observe again.');
          if (labelFor(e) !== \#(literal(label))) return error('label_mismatch', 'The element label changed; observe again.');
          if (shape(e) !== target.shape) return error('element_changed', 'The element type or name changed; observe again.');
          if (!visible(e)) return error('element_hidden', 'The target is no longer visible.');
          if (e.matches(':disabled')) return error('element_disabled', 'The target is disabled.');
          if (e.type === 'file') return JSON.stringify({state: 'needs_user_input', reason: 'file_selection_required',
            next_step: 'Ask the user to open the page, tap this file control and choose a file. Observe again after selection.'});
          const action = \#(literal(action));
          if (action === 'type') {
            if (sensitive(e)) return error('manual_input_required', 'Passwords and one-time codes require user input or system AutoFill.');
            if (e.readOnly) return error('element_readonly', 'The input is read-only.');
            if (e.tagName !== 'TEXTAREA' && (e.tagName !== 'INPUT' ||
                !['text','search','tel','url','email','number','date','time','datetime-local','month','week'].includes(e.type)))
              return error('unsupported_input_type', 'This control does not accept typed text; use click where appropriate.');
            const text = \#(literal(text));
            if (e.maxLength >= 0 && text.length > e.maxLength)
              return error('input_value_invalid', 'The text exceeds this field\'s maximum length.');
            const previous = e.value;
            const prototype = e.tagName === 'TEXTAREA' ? HTMLTextAreaElement.prototype : HTMLInputElement.prototype;
            const setter = Object.getOwnPropertyDescriptor(prototype, 'value').set;
            setter.call(e, text);
            if (e.value !== text) {
              setter.call(e, previous);
              return error('input_value_invalid', 'The input rejected this value format.');
            }
            e.dispatchEvent(new Event('input', {bubbles: true}));
            e.dispatchEvent(new Event('change', {bubbles: true}));
          } else {
            e.click();
          }
          return JSON.stringify({state: 'dispatched', next_step: 'Observe to verify the result. Unchanged targets in this document remain usable.'});
        })()
        """#
    }

    private enum BrowserFailure: Error { case emptyScreenshot, invalidStructure }

    #if DEBUG && targetEnvironment(simulator)
    // 测试场景在 DebugSupport；此处只保留受控的私有 WebKit 状态接入。
    func installUITestPage(html: String, sessionID: String, title: String, baseURL: URL)
        -> (page: WKWebView, isReady: @MainActor () -> Bool) {
        close()
        fixturePage = true
        pageVisible = true
        let config = WKWebViewConfiguration()
        config.websiteDataStore = .nonPersistent()
        let page = WKWebView(frame: .zero, configuration: config)
        page.navigationDelegate = self
        webView = page
        ownerSessionID = sessionID
        sourceMessageID = AIAssistantModel.shared.messages.last?.id ?? ""
        pageTitle = title
        pageURL = baseURL.absoluteString
        allowedHost = baseURL.host?.lowercased() ?? ""
        isWorking = true
        state = "正在加载网页…"
        generation += 1
        mountPage()
        page.loadHTMLString(html, baseURL: baseURL)
        return (page, { [weak self, weak page] in
            self?.loaded == true && page?.isLoading == false
        })
    }
    #endif
}
