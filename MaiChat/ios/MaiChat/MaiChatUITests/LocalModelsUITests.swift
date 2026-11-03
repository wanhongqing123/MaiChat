import XCTest

final class LocalModelsUITests: XCTestCase {
    /// 从正常 Root 菜单运行随 Debug 包提供的小模型；不发起 Agent、网络或付费模型请求。
    @MainActor
    func testBuiltinModelGeneratesTextAndUnloadsFromCreationCenter() throws {
        #if DEBUG && targetEnvironment(simulator)
        continueAfterFailure = false
        let app = XCUIApplication()
        app.launchArguments = ["--ai-ui-test", "--ai-session-reveal-ui-test",
            "-maichat.notification-preprompt-dismissed", "YES"]
        app.launchEnvironment["MAICHAT_AI_TEST_ID"] = UUID().uuidString
        app.launch()
        defer { app.terminate() }

        let tab = app.buttons["创作中心"]
        XCTAssertTrue(tab.waitForExistence(timeout: 12))
        tab.tap()
        XCTAssertTrue(app.textViews["ai-composer"].waitForExistence(timeout: 8))
        app.buttons["更多"].tap()
        let entry = app.buttons["ai-local-models"]
        XCTAssertTrue(entry.waitForExistence(timeout: 4))
        entry.tap()

        let picker = app.buttons["local-model-picker"]
        XCTAssertTrue(picker.waitForExistence(timeout: 8))
        let builtinSelected = XCTNSPredicateExpectation(
            predicate: NSPredicate(format: "label CONTAINS %@", "内置英文测试模型"), object: picker)
        XCTAssertEqual(XCTWaiter.wait(for: [builtinSelected], timeout: 8), .completed,
                       "Debug 正常页面应默认选中已随包安装的本地测试模型")

        let form: XCUIElement = app.collectionViews.firstMatch.exists
            ? app.collectionViews.firstMatch : app.tables.firstMatch
        func reveal(_ element: XCUIElement) {
            for _ in 0..<8 where !element.isHittable { form.swipeUp() }
            XCTAssertTrue(element.isHittable)
        }
        let run = app.buttons["local-model-run"]
        reveal(run)
        XCTAssertTrue(run.isEnabled)
        run.tap()
        // 向下露出完成状态；不靠按钮尚未来得及变灰时的瞬时状态判断推理完成。
        form.swipeUp()
        form.swipeUp()
        let completion = app.staticTexts["local-model-notice"]
        XCTAssertTrue(completion.waitForExistence(timeout: 30))
        XCTAssertEqual(completion.label, "本次本地测试已完成")

        let result = app.staticTexts["local-model-result"]
        reveal(result)
        let generatedText = result.label.trimmingCharacters(in: .whitespacesAndNewlines)
        XCTAssertFalse(generatedText.isEmpty)
        XCTAssertNotEqual(generatedText, "（模型未返回文本）")
        let tokens = app.staticTexts.matching(NSPredicate(format: "label MATCHES %@",
            "输入 [1-9][0-9]* · 生成 [1-9][0-9]* token · .*")).firstMatch
        XCTAssertTrue(tokens.exists, "结果必须显示实际大于零的输入和生成 token 数")
        let generated = XCTAttachment(screenshot: app.screenshot())
        generated.name = "local-model-real-generated-text"
        generated.lifetime = .keepAlways
        add(generated)

        // 结果位于 runtime 区下方；向下滚回同一页面，验证卸载的是本机已驻留模型。
        let unload = app.buttons["local-model-unload"]
        for _ in 0..<6 where !unload.isHittable { form.swipeDown() }
        XCTAssertTrue(unload.isHittable)
        let status = app.staticTexts["local-model-runtime-status"]
        XCTAssertTrue(status.label.contains("模型已驻留"))
        XCTAssertTrue(unload.isEnabled)
        unload.tap()
        let unloaded = XCTNSPredicateExpectation(
            predicate: NSPredicate(format: "label CONTAINS %@", "未加载模型"), object: status)
        XCTAssertEqual(XCTWaiter.wait(for: [unloaded], timeout: 8), .completed)
        XCTAssertFalse(unload.isEnabled)
        let emptyRuntime = XCTAttachment(screenshot: app.screenshot())
        emptyRuntime.name = "local-model-runtime-unloaded"
        emptyRuntime.lifetime = .keepAlways
        add(emptyRuntime)
        app.buttons["local-model-done"].tap()
        XCTAssertTrue(app.textViews["ai-composer"].waitForExistence(timeout: 4))
        #else
        throw XCTSkip("仅在 Debug 模拟器使用随包本地权重与独立内存会话")
        #endif
    }
}
