// 本机 WebKit 验收素材与断言，不进入 Release。
#if DEBUG && targetEnvironment(simulator)
import Foundation
import WebKit

enum AIBrowserUITestDiagnostics {
    static func trace(_ value: String) {
        if ProcessInfo.processInfo.arguments.contains("--ai-browser-ui-test") {
            NSLog("[BrowserPresentation] %@", value)
        }
    }
}

extension AIMobileBrowserTools {
    func prepareOrderUITestPage() async {
        let model = AIAssistantModel.shared
        let part = AIPart(id: "browser-order-open-part", kind: "tool", text: nil,
                          tool: "mobile_browser", input: #"{"action":"open","url":"http://127.0.0.1:18680/page"}"#,
                          output: #"{"state":"loading"}"#, error: nil, state: "completed", path: nil, mimeType: nil)
        let answer = AIPart(id: "browser-order-answer-part", kind: "text",
                            text: "这是本轮工具之后生成的正文。", tool: nil, input: nil, output: nil,
                            error: nil, state: nil, path: nil, mimeType: nil)
        model.messages = [AIMessage(id: "browser-order-open", role: "assistant", created: 1, completed: 2,
                                    active: false, parts: [part, answer])]
        // 模拟宿主执行时 UI 已接到后续消息，验证位置来自可信调用上下文而非当前末条。
        model.appendBrowserOrderUITestReply()
        _ = await execute(["action": "open", "session_id": "switch-A", "url": "http://127.0.0.1:18680/page",
                           "message_id": "browser-order-open", "part_id": "browser-order-open-part"])
        _ = await observePage(sessionID: "switch-A")
    }

    /// 与现场相同的 httpbin 表单，经本机服务提供并回显 POST；只使用合成数据。
    func runFormUITestFixture() async {
        let root = FileManager.default.temporaryDirectory.appendingPathComponent("maichat-browser-ui-test")
        try? FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
        AIAssistantModel.shared.installVideoToolTestWorkspace(root.path)
        var failures: [String] = []
        func check(_ condition: Bool, _ message: String) {
            if !condition { failures.append(message) }
        }
        func decode(_ result: AIMaiChatHostToolExecution) -> [String: Any] {
            guard let data = result.output?.data(using: .utf8),
                  let value = try? JSONSerialization.jsonObject(with: data) as? [String: Any] else { return [:] }
            return value
        }
        func observe() async -> [String: Any] { decode(await observePage(sessionID: "switch-A")) }
        func open(_ path: String, awaitLoad: Bool = true) async {
            _ = await openPage(URL(string: "http://127.0.0.1:18680" + path)!, sessionID: "switch-A")
            if awaitLoad {
                for _ in 0..<100 {
                    if webView?.isLoading == false { break }
                    try? await Task.sleep(for: .milliseconds(100))
                }
            }
        }
        func element(_ snapshot: [String: Any], _ label: String, fallback: String = "") -> [String: Any] {
            (snapshot["elements"] as? [[String: Any]] ?? []).first {
                ($0["label"] as? String) == label || (!fallback.isEmpty && ($0["label"] as? String) == fallback)
            } ?? [:]
        }
        func act(_ snapshot: [String: Any], _ target: [String: Any],
                 action: String = "type", text: String = "") async -> AIMaiChatHostToolExecution {
            await actOnPage(sessionID: "switch-A", action: action,
                            expectedURL: snapshot["url"] as? String ?? "",
                            snapshotID: snapshot["snapshot_id"] as? String ?? "",
                            elementID: target["id"] as? String ?? "",
                            expectedLabel: target["label"] as? String ?? "", text: text)
        }

        await open("/form")
        let initial = await observe()
        let name = element(initial, "Customer name:", fallback: "custname")
        let telephone = element(initial, "Telephone:", fallback: "custtel")
        let email = element(initial, "E-mail address:", fallback: "custemail")
        check(name["label"] as? String == "Customer name:", "associated label fell back to custname")
        check(!element(initial, "Small").isEmpty, "radio label Small missing")
        check(!element(initial, "Extra Cheese").isEmpty, "checkbox label Extra Cheese missing")
        check(!element(initial, "Accessible first name").isEmpty, "aria-labelledby missing")
        let first = await act(initial, name, text: "Mai Fixture")
        let second = await act(initial, telephone, text: "123456789")
        let third = await act(initial, email, text: "fixture@example.invalid")
        check(first.errorCode == nil, "first input failed")
        check(second.errorCode == nil, "same-snapshot second input failed: " + (second.errorMessage ?? ""))
        check(third.errorCode == nil, "same-snapshot third input failed: " + (third.errorMessage ?? ""))
        let next = await observe()
        check(initial["snapshot_id"] as? String == next["snapshot_id"] as? String, "document snapshot changed after typing")
        check(name["id"] as? String == element(next, "Customer name:", fallback: "custname")["id"] as? String,
              "unchanged node received a new ID")
        let parallelName = Task { @MainActor in
            await act(next, element(next, "Customer name:", fallback: "custname"), text: "Parallel Fixture")
        }
        let parallelEmail = Task { @MainActor in
            await act(next, element(next, "E-mail address:", fallback: "custemail"), text: "parallel@example.invalid")
        }
        let parallel = await (parallelName.value, parallelEmail.value)
        check(parallel.0.errorCode == nil && parallel.1.errorCode == nil, "parallel independent inputs failed")

        // Same label on a replacement DOM node must not authorize the old handle.
        let beforeMutation = await observe()
        let oldName = element(beforeMutation, "Customer name:", fallback: "custname")
        _ = try? await webView?.evaluateJavaScript("const old=document.querySelector('[name=custname]');old.replaceWith(old.cloneNode(true));")
        let replaced = await act(beforeMutation, oldName, text: "must not be written")
        check(replaced.errorMessage?.contains("[element_detached]") == true, "replacement did not report element_detached")
        let refreshed = await observe()
        check(oldName["id"] as? String != element(refreshed, "Customer name:", fallback: "custname")["id"] as? String,
              "replacement reused the original node ID")
        var unknown = element(refreshed, "Telephone:", fallback: "custtel")
        unknown["id"] = "never-observed"
        let unknownResult = await act(refreshed, unknown, text: "x")
        check(unknownResult.errorMessage?.contains("[element_not_observed]") == true, "unknown element error was ambiguous")
        let password = await act(refreshed, element(refreshed, "Account password", fallback: "password"), text: "synthetic")
        check(password.errorMessage?.contains("[manual_input_required]") == true, "password restriction was ambiguous")
        let file = await act(refreshed, element(refreshed, "Upload a file", fallback: "input"), action: "click")
        check(decode(file)["reason"] as? String == "file_selection_required", "file picker handoff missing")
        let locked = await act(refreshed, element(refreshed, "Locked input", fallback: "locked"), text: "x")
        check(locked.errorMessage?.contains("[element_readonly]") == true, "readonly input was not rejected precisely")

        // Submit using the original page handles; observe after dispatch verifies real POST output.
        let order = await observe()
        let size = element(order, "Small")
        let topping = element(order, "Extra Cheese")
        _ = await act(order, size, action: "click")
        _ = await act(order, topping, action: "click")
        _ = await act(order, element(order, "Customer name:", fallback: "custname"), text: "Submitted Fixture")
        let submitted = await act(order, element(order, "Submit order"), action: "click")
        check(submitted.errorCode == nil, "submit action failed")
        try? await Task.sleep(for: .milliseconds(300))
        let receipt = await observe()
        check((receipt["text"] as? String ?? "").contains("Submitted Fixture"), "POST did not contain the entered name")
        check((receipt["text"] as? String ?? "").contains("cheese"), "POST did not contain selected topping")
        let afterNavigation = await act(order, name, text: "x")
        check(afterNavigation.errorCode != nil, "navigation accepted old snapshot")

        await open("/slow-form", awaitLoad: false)
        let waited = await observe()
        check(waited["state"] as? String == "ready" && !element(waited, "Customer name:").isEmpty,
              "single observe did not wait for usable DOM")
        await open("/form-with-loading-image", awaitLoad: false)
        let streaming = await observe()
        check(streaming["state"] as? String == "ready" && !element(streaming, "Telephone:").isEmpty,
              "pending subresource hid usable form")
        let readyAction = await act(streaming, element(streaming, "Telephone:"), text: "123")
        check(readyAction.errorCode == nil, "usable DOM action blocked by loading image")
        await open("/form-timeout", awaitLoad: false)
        let start = Date()
        let partial = decode(await execute(["action": "observe", "session_id": "switch-A", "wait_ms": 200]))
        check(partial["state"] as? String == "loading" && partial["elements"] != nil && partial["text"] != nil,
              "loading timeout omitted partial structure")
        check(Date().timeIntervalSince(start) < 2, "observe exceeded requested wait bound")

        // Finish on a real password page so the normal task entry retains its manual-input state.
        await open("/form")
        _ = await observe()
        let result = failures.isEmpty ? "PASS: browser form targets, labels, loading and input handoff"
            : failures.joined(separator: "\n")
        try? result.write(to: root.appendingPathComponent("result.txt"), atomically: true, encoding: .utf8)
    }

    func preparePresentationUITestPage() async {
        let root = FileManager.default.temporaryDirectory.appendingPathComponent("browser-presentation-test")
        try? FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
        AIAssistantModel.shared.installVideoToolTestWorkspace(root.path)
        let path = ProcessInfo.processInfo.environment["MAICHAT_BROWSER_TEST_PATH"] ?? "/page"
        guard ["/page", "/slow", "/failure", "/login", "/form"].contains(path),
              let url = URL(string: "http://127.0.0.1:18680" + path) else { return }
        _ = await openPage(url, sessionID: "switch-A")
        for _ in 0..<150 {
            if let page = webView, !page.isLoading {
                _ = await observePage(sessionID: "switch-A")
                return
            }
            try? await Task.sleep(for: .milliseconds(100))
        }
    }

    func runUITestFixture() async {
        let root = FileManager.default.temporaryDirectory
            .appendingPathComponent("maichat-browser-ui-test", isDirectory: true)
        let images = root.appendingPathComponent("images", isDirectory: true)
        try? FileManager.default.createDirectory(at: images, withIntermediateDirectories: true)
        AIAssistantModel.shared.installVideoToolTestWorkspace(root.path)
        let fixture = installUITestPage(
            html: """
        <!doctype html><html><head><meta name="viewport" content="width=device-width,initial-scale=1">
        <title>浏览器验收</title><style>body{font-family:-apple-system;padding:18px}
        button,input{font-size:20px;padding:8px;margin:8px 0}button{background:#dff2ff}</style></head>
        <body><h2>MaiChat 浏览器测试</h2><input aria-label="测试输入" id="input">
        <button aria-label="确认按钮" onclick="document.getElementById('result').textContent=
        '操作已完成：'+document.getElementById('input').value">确认按钮</button>
        <p id="result">等待操作</p><input type="password" aria-label="密码输入"></body></html>
        """, sessionID: "switch-A", title: "浏览器验收",
            baseURL: URL(string: "https://fixture.maichat.local/page")!)
        for _ in 0..<80 {
            if fixture.isReady() { break }
            try? await Task.sleep(for: .milliseconds(100))
        }
        var failures: [String] = []
        let first = await execute(["action": "observe", "session_id": "switch-A"])
        guard let firstText = first.output,
              let firstData = firstText.data(using: .utf8),
              let firstJSON = (try? JSONSerialization.jsonObject(with: firstData)) as? [String: Any],
              let firstID = firstJSON["snapshot_id"] as? String,
              let firstURL = firstJSON["url"] as? String,
              let firstElements = firstJSON["elements"] as? [[String: Any]],
              let input = firstElements.first(where: { $0["label"] as? String == "测试输入" }),
              let inputID = input["id"] as? String else {
            try? "observe failed: \(first.errorMessage ?? "invalid result")"
                .write(to: root.appendingPathComponent("result.txt"), atomically: true, encoding: .utf8)
            return
        }
        let typed = await execute(["action": "type", "session_id": "switch-A",
                                   "expected_url": firstURL, "snapshot_id": firstID,
                                   "element_id": inputID, "expected_label": "测试输入",
                                   "text": "iOS WebKit OK"])
        if typed.errorCode != nil { failures.append("type: \(typed.errorMessage ?? "unknown")") }
        let repeatInput = await execute(["action": "type", "session_id": "switch-A",
                                   "expected_url": firstURL, "snapshot_id": firstID,
                                   "element_id": inputID, "expected_label": "测试输入",
                                   "text": "iOS WebKit OK"])
        if repeatInput.errorCode != nil { failures.append("unchanged input handle was rejected") }
        let otherSession = await execute(["action": "observe", "session_id": "switch-B"])
        if otherSession.errorCode == nil { failures.append("other Session accessed the page") }
        let second = await execute(["action": "observe", "session_id": "switch-A"])
        if let text = second.output, let data = text.data(using: .utf8),
           let json = (try? JSONSerialization.jsonObject(with: data)) as? [String: Any],
           let id = json["snapshot_id"] as? String,
           let elements = json["elements"] as? [[String: Any]],
           let button = elements.first(where: { $0["label"] as? String == "确认按钮" }),
           let buttonID = button["id"] as? String {
            let clicked = await execute(["action": "click", "session_id": "switch-A",
                                         "expected_url": firstURL, "snapshot_id": id,
                                         "element_id": buttonID, "expected_label": "确认按钮"])
            if clicked.errorCode != nil { failures.append("click: \(clicked.errorMessage ?? "unknown")") }
        } else { failures.append("second observe failed") }
        try? await Task.sleep(for: .milliseconds(250))
        let third = await execute(["action": "observe", "session_id": "switch-A"])
        if !(third.output ?? "").contains("操作已完成：iOS WebKit OK") {
            failures.append("result not observed")
        }
        if let text = third.output, let data = text.data(using: .utf8),
           let json = (try? JSONSerialization.jsonObject(with: data)) as? [String: Any],
           let id = json["snapshot_id"] as? String,
           let elements = json["elements"] as? [[String: Any]],
           let password = elements.first(where: { $0["label"] as? String == "密码输入" }),
           let passwordID = password["id"] as? String {
            let denied = await execute(["action": "type", "session_id": "switch-A",
                                        "expected_url": firstURL, "snapshot_id": id,
                                        "element_id": passwordID, "expected_label": "密码输入",
                                        "text": "forbidden"])
            if denied.errorCode == nil { failures.append("password field was accepted") }
        } else { failures.append("password input not observed") }
        let wrongURL = await execute(["action": "screenshot", "session_id": "switch-A",
                                      "expected_url": "https://wrong.example/",
                                      "output_path": images.appendingPathComponent("wrong.png").path])
        if wrongURL.errorCode == nil { failures.append("wrong URL screenshot was accepted") }
        let screenshot = await execute(["action": "screenshot", "session_id": "switch-A",
                                        "expected_url": firstURL,
                                        "output_path": images.appendingPathComponent("page.png").path])
        if screenshot.errorCode != nil { failures.append("screenshot: \(screenshot.errorMessage ?? "unknown")") }
        let result = failures.isEmpty
            ? "PASS: observe, type, click, verify, screenshot"
            : failures.joined(separator: "\n")
        try? result.write(to: root.appendingPathComponent("result.txt"), atomically: true, encoding: .utf8)
    }
}

extension AIAssistantModel {
    func appendBrowserOrderUITestReply() {
        let count = messages.count
        let part = AIPart(id: "browser-order-after-part-\(count)", kind: "text",
                          text: "网页之后的回复 \(count)：继续讨论新的问题。", tool: nil, input: nil, output: nil,
                          error: nil, state: nil, path: nil, mimeType: nil)
        messages.append(AIMessage(id: "browser-order-after-\(count)", role: "assistant", created: Int64(count + 2),
                                  completed: Int64(count + 3), active: false, parts: [part]))
    }
}
#endif
