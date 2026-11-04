import XCTest

final class PrimaryModelsUITests: XCTestCase {
    /// 正常“更多”面板直接选择随包 Qwen；两轮回复和重启恢复均来自实际 native Agent。
    @MainActor
    func testBundledLocalPrimaryModelChatsTwiceAndRestoresAfterRelaunch() throws {
        #if DEBUG && targetEnvironment(simulator)
        continueAfterFailure = false
        let app = XCUIApplication()
        app.launchArguments = ["--ai-ui-test", "--ai-session-reveal-ui-test", "--ai-primary-model-ui-test",
            "-maichat.notification-preprompt-dismissed", "YES"]
        app.launchEnvironment["MAICHAT_AI_TEST_ID"] = UUID().uuidString
        app.launch()
        defer { app.terminate() }
        let state = app.staticTexts["primary-ui-model-state"]
        XCTAssertTrue(state.waitForExistence(timeout: 20))
        XCTAssertTrue(app.textViews["ai-composer"].waitForExistence(timeout: 20))
        app.buttons["更多"].tap()
        let settings = app.buttons["ai-primary-model-settings"]
        XCTAssertTrue(settings.waitForExistence(timeout: 4))
        XCTAssertEqual(XCTWaiter.wait(for: [XCTNSPredicateExpectation(
            predicate: NSPredicate(format: "enabled == true"), object: settings)], timeout: 20), .completed)
        settings.tap()
        let panel = app.descendants(matching: .any).matching(identifier: "ai-action-panel").firstMatch
        let menu = panel.descendants(matching: .any).matching(identifier: "ai-primary-model-menu").firstMatch
        XCTAssertTrue(menu.waitForExistence(timeout: 5))
        let local = menu.buttons["ai-primary-model-local"]
        XCTAssertTrue(local.isHittable); local.tap()
        let selected = XCTNSPredicateExpectation(predicate: NSPredicate(format:
            "value CONTAINS %@ AND value CONTAINS %@ AND value CONTAINS %@ AND value CONTAINS %@",
            "provider:llama;", "configured:true;", ";native-wire:local;",
            ";native-model:local:builtin-qwen3.5-2b-q4_k_m;"), object: state)
        XCTAssertEqual(XCTWaiter.wait(for: [selected], timeout: 30), .completed)
        XCTAssertEqual(XCTWaiter.wait(for: [XCTNSPredicateExpectation(
            predicate: NSPredicate(format: "value == %@ AND enabled == true", "本地模型"), object: settings)], timeout: 5), .completed)
        XCTAssertTrue(panel.exists, "选择后仍使用当前更多面板，不跳转主模型设置页")
        XCTAssertFalse(app.buttons["primary-model-apply"].exists)
        XCTAssertFalse(app.buttons["primary-model-management"].exists)
        XCTAssertFalse(app.buttons["primary-local-model-import"].exists)
        XCTAssertFalse(app.buttons["primary-local-self-test"].exists)
        let selection = XCTAttachment(screenshot: app.screenshot())
        selection.name = "bundled-local-primary-inline-selection"; selection.lifetime = .keepAlways; add(selection)
        func dismissPanel() {
            app.coordinate(withNormalizedOffset: .zero).withOffset(
                CGVector(dx: 12, dy: panel.frame.midY)).tap()
            XCTAssertEqual(XCTWaiter.wait(for: [XCTNSPredicateExpectation(
                predicate: NSPredicate(format: "exists == false"), object: panel)], timeout: 5), .completed)
        }
        dismissPanel()
        let supportsGPU = (state.value as? String ?? "").contains("qwen-gpu-ops-supported:true")
        // Simulator 的 Apple2 不支持随包模型所需的 SIMD 算子，只能验证纯 CPU 功能。
        // 真机相同 client 的 GPU/耗时另由正常 App 隔离设备探针测量；不更改产品超时。
        let replyTimeout: TimeInterval = supportsGPU ? 180 : 360
        let capability = XCTAttachment(string: supportsGPU
            ? "Device supports required Qwen GPU operations; require actual GPU buffers."
            : "Simulator lacks required SIMD operations; validate actual CPU replies. Physical GPU is validated separately.")
        capability.name = "actual-qwen-compute-capability"
        capability.lifetime = .keepAlways
        add(capability)

        func send(_ text: String, atLeast count: Int) {
            let composer = app.textViews["ai-composer"]
            XCTAssertTrue(composer.waitForExistence(timeout: 10)); composer.tap(); composer.typeText(text)
            XCTAssertFalse(app.buttons["ai-send-draft"].exists)
            XCTAssertTrue(app.buttons["展开更多功能"].isEnabled)
            composer.typeText("\n")
            // 实际推理进行时必须锁住主模型切换；不能用自测页面或伪造忙碌状态代替。
            if count == 1 {
                XCTAssertEqual(XCTWaiter.wait(for: [XCTNSPredicateExpectation(
                    predicate: NSPredicate(format: "value CONTAINS %@", ";busy:true;"), object: state)], timeout: 10), .completed)
                app.buttons["更多"].tap()
                XCTAssertTrue(settings.waitForExistence(timeout: 4))
                XCTAssertFalse(settings.isEnabled)
                dismissPanel()
            }
            let countPattern = count == 1 ? "[1-9][0-9]*" : "(?:[2-9]|[1-9][0-9]+)"
            let gpu = supportsGPU
                ? "gpu-selected:true;gpu-bytes:[1-9][0-9]*;gpu-backend:[^;]+;"
                : "gpu-selected:false;gpu-bytes:0;gpu-backend:;"
            let reply = XCTNSPredicateExpectation(predicate: NSPredicate(format: "value MATCHES %@",
                ".*provider:llama;configured:true;busy:false;assistant-text-count:" + countPattern + ";session:[^;]+;native-wire:local;native-model:local:[^;]+;" + gpu + "error-empty:true;qwen-gpu-ops-supported:(?:true|false)"), object: state)
            XCTAssertEqual(XCTWaiter.wait(for: [reply], timeout: replyTimeout), .completed,
                           "真实本地主模型必须生成非空、无错误的回复并完成轮次；实际状态：\(state.value ?? "missing")")
            XCTAssertTrue(app.buttons["复制回复"].firstMatch.exists, "回复应实际显示在聊天消息区")
        }
        send("请记住本次代号是「松柏」，只用一句话确认。", atLeast: 1)
        send("刚才我告诉你的代号是什么？", atLeast: 2)
        let output = app.staticTexts["primary-ui-model-output"]
        XCTAssertTrue(output.exists)
        XCTAssertTrue((output.value as? String ?? "").contains("松柏"),
                      "必须正确使用真实上一轮内容，非空套话不能算质量通过：\(output.value ?? "missing")")
        let generated = XCTAttachment(screenshot: app.screenshot())
        generated.name = "local-primary-real-two-turn-conversation"; generated.lifetime = .keepAlways; add(generated)

        let nativeID = try XCTUnwrap((state.value as? String)?.split(separator: ";")
            .first(where: { $0.hasPrefix("native-model:") }).map(String.init))

        app.terminate(); app.launch()
        let restored = XCTNSPredicateExpectation(predicate: NSPredicate(format: "value MATCHES %@",
            ".*provider:llama;configured:true;busy:false;assistant-text-count:(?:[2-9]|[1-9][0-9]+);session:[^;]+;native-wire:local;native-model:local:[^;]+;gpu-selected:(?:true|false);gpu-bytes:[0-9]+;gpu-backend:[^;]*;error-empty:true;qwen-gpu-ops-supported:(?:true|false)"), object: state)
        XCTAssertEqual(XCTWaiter.wait(for: [restored], timeout: 30), .completed)
        XCTAssertTrue((state.value as? String ?? "").contains(nativeID))
        app.buttons["更多"].tap(); app.buttons["ai-primary-model-settings"].tap()
        XCTAssertTrue(menu.waitForExistence(timeout: 8))
        XCTAssertEqual(settings.label, "主模型")
        XCTAssertEqual(settings.value as? String, "本地模型")
        XCTAssertTrue(menu.buttons["ai-primary-model-local"].isHittable)
        XCTAssertFalse(app.navigationBars["主模型"].exists)
        let persisted = XCTAttachment(screenshot: app.screenshot())
        persisted.name = "local-primary-selection-restored"; persisted.lifetime = .keepAlways; add(persisted)
        #else
        throw XCTSkip("仅在 Debug 模拟器使用随包 Qwen 和真实本地后端")
        #endif
    }
}
