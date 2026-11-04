import XCTest

final class LocalModelsUITests: XCTestCase {
    /// 与思考强度共用“更多”的内联展开层级；切换走真实配置，不发起推理或付费请求。
    @MainActor
    func testPrimaryModelSwitchesInlineToBundledLocalAndBackToCloud() throws {
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
        func waitForConfiguration(provider: String, wire: String, model: String) {
            let configured = XCTNSPredicateExpectation(predicate: NSPredicate(format:
                "value CONTAINS %@ AND value CONTAINS %@ AND value CONTAINS %@ AND value CONTAINS %@ AND value CONTAINS %@ AND value CONTAINS %@",
                "provider:\(provider);", ";configured:true;", ";busy:false;assistant-text-count:0;",
                ";native-wire:\(wire);", ";native-model:\(model);", ";error-empty:true;"), object: state)
            XCTAssertEqual(XCTWaiter.wait(for: [configured], timeout: 30), .completed,
                           "主模型切换必须等待真实 native 配置完成；实际状态：\(state.value ?? "missing")")
        }
        // 现有独立 QA 账号的云端指向本机 test-model；这里只配置，不提交任何模型请求。
        waitForConfiguration(provider: "remote", wire: "responses", model: "test-model")
        app.buttons["更多"].tap()
        let panel = app.descendants(matching: .any).matching(identifier: "ai-action-panel").firstMatch
        XCTAssertTrue(panel.waitForExistence(timeout: 5))
        let settings = panel.buttons["ai-primary-model-settings"]
        XCTAssertTrue(settings.isHittable)
        XCTAssertEqual(settings.label, "主模型")
        XCTAssertEqual(settings.value as? String, "云端模型")

        let effort = panel.buttons["思考强度"]
        effort.tap()
        let effortMenu = panel.descendants(matching: .any).matching(identifier: "ai-effort-menu").firstMatch
        XCTAssertTrue(effortMenu.waitForExistence(timeout: 4))
        settings.tap()
        let menu = panel.descendants(matching: .any).matching(identifier: "ai-primary-model-menu").firstMatch
        XCTAssertTrue(menu.waitForExistence(timeout: 4))
        XCTAssertFalse(effortMenu.exists, "主模型应复用思考强度的同一个展开层级")
        let cloud = menu.buttons["ai-primary-model-cloud"]
        let local = menu.buttons["ai-primary-model-local"]
        XCTAssertEqual(cloud.label, "云端模型")
        XCTAssertEqual(local.label, "本地模型")
        XCTAssertTrue(cloud.isHittable)
        XCTAssertTrue(local.isHittable)

        func assertInlineSelection(_ expected: String, capture name: String) {
            XCTAssertTrue(panel.exists)
            XCTAssertTrue(menu.exists, "主模型选项必须位于当前更多面板内")
            XCTAssertEqual(settings.value as? String, expected)
            XCTAssertGreaterThanOrEqual(menu.frame.minY, settings.frame.maxY - 1)
            XCTAssertGreaterThanOrEqual(menu.frame.minX, panel.frame.minX)
            XCTAssertLessThanOrEqual(menu.frame.maxX, panel.frame.maxX)
            XCTAssertLessThanOrEqual(menu.frame.maxY, panel.frame.maxY)
            XCTAssertFalse(app.navigationBars["主模型"].exists)
            XCTAssertFalse(app.navigationBars["本机模型"].exists)
            for identifier in ["primary-model-provider", "primary-model-current", "primary-model-apply",
                "primary-model-management", "primary-model-done", "primary-local-add",
                "primary-local-model-import", "primary-local-self-test", "local-model-form",
                "local-model-picker", "local-model-run"] {
                XCTAssertFalse(app.descendants(matching: .any).matching(identifier: identifier).firstMatch.exists,
                               "内联主模型切换不得出现旧设置、管理、导入或自测控件：\(identifier)")
            }
            let capture = XCTAttachment(screenshot: app.screenshot())
            capture.name = name
            capture.lifetime = .keepAlways
            add(capture)
        }
        assertInlineSelection("云端模型", capture: "primary-model-inline-cloud-options")

        local.tap()
        // 随包真实权重由普通 Store 解析；configured/native-wire 成功后才能继续切回云端。
        waitForConfiguration(provider: "llama", wire: "local",
                             model: "local:builtin-qwen2.5-0.5b-instruct-q4_k_m")
        XCTAssertEqual(XCTWaiter.wait(for: [XCTNSPredicateExpectation(
            predicate: NSPredicate(format: "enabled == true"), object: cloud)], timeout: 5), .completed)
        assertInlineSelection("本地模型", capture: "primary-model-inline-bundled-local-selected")

        cloud.tap()
        waitForConfiguration(provider: "remote", wire: "responses", model: "test-model")
        XCTAssertEqual(XCTWaiter.wait(for: [XCTNSPredicateExpectation(
            predicate: NSPredicate(format: "enabled == true"), object: local)], timeout: 5), .completed)
        assertInlineSelection("云端模型", capture: "primary-model-inline-cloud-restored")
        #else
        throw XCTSkip("仅在 Debug 模拟器使用随包 Qwen、独立账号和真实配置链")
        #endif
    }
}
