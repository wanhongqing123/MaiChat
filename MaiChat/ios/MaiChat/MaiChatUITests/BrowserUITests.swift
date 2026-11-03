import XCTest

/// 使用正常 Root、合成网页和本机 HTTP 服务；不调用模型或真实账号服务。
final class BrowserUITests: XCTestCase {
    @MainActor func testBrowserEntryStaysBesideOpeningCallWhenRepliesAreAppended() throws {
        continueAfterFailure = false
        let app = XCUIApplication()
        app.launchArguments = ["--ai-ui-test", "--ai-browser-ui-test"]
        app.launchEnvironment["MAICHAT_BROWSER_ORDER_TEST"] = "1"
        app.launch()
        defer { app.terminate() }
        let entry = app.buttons["ai-browser-task-entry"]
        let after = app.descendants(matching: .any).matching(NSPredicate(
            format: "label == %@", "网页之后的回复 1：继续讨论新的问题。")).firstMatch
        XCTAssertTrue(entry.waitForExistence(timeout: 15))
        XCTAssertTrue(after.waitForExistence(timeout: 5))
        let sameTurnAnswer = app.descendants(matching: .any).matching(NSPredicate(
            format: "label == %@", "这是本轮工具之后生成的正文。")).firstMatch
        XCTAssertTrue(sameTurnAnswer.exists)
        capture(app, "browser-entry-after-tool-before-following-reply")
        XCTAssertLessThan(entry.frame.maxY, sameTurnAnswer.frame.minY,
                          "同一条助手消息中的工具卡片也应在工具之后的正文前面")
        XCTAssertLessThan(entry.frame.maxY, after.frame.minY,
                          "网页入口必须留在创建它的调用之后，不能跟在后续回复后面")
        app.buttons["追加测试消息"].tap()
        let newest = app.descendants(matching: .any).matching(NSPredicate(
            format: "label == %@", "网页之后的回复 2：继续讨论新的问题。")).firstMatch
        XCTAssertTrue(newest.waitForExistence(timeout: 5))
        XCTAssertLessThan(entry.frame.maxY, after.frame.minY)
        XCTAssertLessThan(after.frame.maxY, newest.frame.minY)
        XCTAssertEqual(app.buttons.matching(identifier: "ai-browser-task-entry").count, 1)
        openPage(app)
        returnToChat(app)
        XCTAssertLessThan(entry.frame.maxY, after.frame.minY)
        capture(app, "browser-entry-keeps-order-after-append-and-webview-return")
        app.buttons["推进测试分页"].tap()
        XCTAssertEqual(XCTWaiter.wait(for: [XCTNSPredicateExpectation(
            predicate: NSPredicate(format: "exists == false"), object: entry)], timeout: 5), .completed,
            "来源消息不在当前页时，不允许在列表末尾补挂网页入口")
        app.buttons["加载测试历史"].tap()
        XCTAssertTrue(entry.waitForExistence(timeout: 5))
        let conversation = try XCTUnwrap(app.scrollViews.allElementsBoundByIndex.first { $0.isHittable },
                                         "使用当前可交互的消息视口，不能选到隐藏的对话抽屉")
        for _ in 0..<4 where !entry.isHittable { conversation.swipeDown() }
        XCTAssertTrue(entry.isHittable)
        XCTAssertLessThan(entry.frame.maxY, sameTurnAnswer.frame.minY)
        capture(app, "browser-entry-restored-in-older-messages")
    }

    @MainActor private func launchPage(_ path: String = "/page", approval: Bool = false) -> XCUIApplication {
        continueAfterFailure = false
        XCUIDevice.shared.orientation = .portrait
        let app = XCUIApplication()
        app.launchArguments = ["--ai-ui-test", "--ai-browser-ui-test"]
        app.launchEnvironment["MAICHAT_BROWSER_PRESENTATION_TEST"] = "1"
        app.launchEnvironment["MAICHAT_BROWSER_TEST_PATH"] = path
        if approval { app.launchEnvironment["MAICHAT_BROWSER_TEST_APPROVAL"] = "1" }
        app.launch()
        let entry = app.buttons["ai-browser-task-entry"]
        XCTAssertTrue(entry.waitForExistence(timeout: 20))
        for _ in 0..<3 where !entry.isHittable { app.scrollViews.firstMatch.swipeUp() }
        XCTAssertTrue(entry.isHittable)
        XCTAssertEqual(app.buttons.matching(identifier: "ai-browser-task-entry").count, 1)
        XCTAssertFalse(app.webViews.firstMatch.isHittable, "打开工具只显示入口，网页不能占用聊天视口")
        return app
    }

    @MainActor private func openPage(_ app: XCUIApplication) {
        let entry = app.buttons["ai-browser-task-entry"]
        for _ in 0..<3 where !entry.isHittable { app.scrollViews.firstMatch.swipeUp() }
        app.buttons["ai-browser-task-entry"].tap()
        XCTAssertTrue(app.buttons["ai-browser-return"].waitForExistence(timeout: 5))
        XCTAssertTrue(app.webViews.firstMatch.waitForExistence(timeout: 10))
        for title in ["消息", "云盘", "创作中心", "好友列表", "我的"] {
            XCTAssertFalse(app.buttons[title].isHittable, "独立网页必须覆盖主导航")
        }
    }

    @MainActor private func returnToChat(_ app: XCUIApplication) {
        app.buttons["ai-browser-return"].tap()
        XCTAssertEqual(XCTWaiter.wait(for: [XCTNSPredicateExpectation(
            predicate: NSPredicate(format: "exists == false"),
            object: app.buttons["ai-browser-return"])], timeout: 5), .completed)
        XCTAssertTrue(app.buttons["创作中心"].isHittable)
        capture(app, "browser-returned-to-chat")
    }

    @MainActor private func capture(_ app: XCUIApplication, _ name: String) {
        let attachment = XCTAttachment(screenshot: app.screenshot())
        attachment.name = name
        attachment.lifetime = .keepAlways
        add(attachment)
    }

    @MainActor func testBrowserRetainsInputAndScrollAcrossChatAndRotation() throws {
        let app = launchPage()
        defer { app.terminate(); XCUIDevice.shared.orientation = .portrait }
        capture(app, "browser-task-entry-in-normal-chat")
        openPage(app)
        let input = app.webViews.textFields["网页备注"]
        XCTAssertTrue(input.waitForExistence(timeout: 10))
        capture(app, "browser-independent-page-top")
        input.tap()
        input.typeText("保留这条备注")
        // 从键盘打开状态直接返回，复开不能销毁输入或露出底部主导航。
        returnToChat(app)
        openPage(app)
        XCTAssertTrue((input.value as? String ?? "").contains("保留这条备注"))
        app.webViews.buttons["保存备注"].tap()
        let bottom = app.webViews.buttons["网页末尾按钮"]
        for _ in 0..<8 where !bottom.isHittable { app.webViews.firstMatch.swipeUp() }
        XCTAssertTrue(bottom.isHittable, "网页末项不能被安全区或状态条遮住")
        bottom.tap()
        let position = bottom.frame.minY
        capture(app, "browser-independent-page-bottom")
        returnToChat(app)
        openPage(app)
        XCTAssertTrue(bottom.isHittable)
        XCTAssertEqual(bottom.frame.minY, position, accuracy: 8, "返回对话再打开应保持阅读位置")
        XCUIDevice.shared.orientation = .landscapeRight
        XCTAssertEqual(XCTWaiter.wait(for: [XCTNSPredicateExpectation(
            predicate: NSPredicate { _, _ in app.frame.width > app.frame.height },
            object: app)], timeout: 8), .completed)
        XCTAssertEqual(XCTWaiter.wait(for: [XCTNSPredicateExpectation(
            predicate: NSPredicate { _, _ in
                let size = XCUIScreen.main.screenshot().image.size
                return size.width > size.height
            }, object: app)], timeout: 8), .completed,
            "实际屏幕缓冲也必须结束旋转，不能只依据提前变化的窗口几何截取过渡帧")
        XCTAssertTrue(app.buttons["ai-browser-return"].isHittable)
        let landscape = XCTAttachment(screenshot: XCUIScreen.main.screenshot())
        landscape.name = "browser-independent-page-landscape"
        landscape.lifetime = .keepAlways
        add(landscape)
        XCUIDevice.shared.orientation = .portrait
        XCTAssertEqual(XCTWaiter.wait(for: [XCTNSPredicateExpectation(
            predicate: NSPredicate { _, _ in app.frame.height > app.frame.width },
            object: app)], timeout: 8), .completed)
        returnToChat(app)
        XCTAssertEqual(app.buttons.matching(identifier: "ai-browser-task-entry").count, 1)
    }

    @MainActor func testBrowserIsolatedFromOtherSessionAndEndsExplicitly() throws {
        let app = launchPage()
        defer { app.terminate() }
        app.buttons["对话列表"].tap()
        waitForDrawer(app)
        app.buttons["ai-conversation-switch-B"].tap()
        XCTAssertFalse(app.buttons["ai-browser-task-entry"].exists)
        XCTAssertEqual(XCTWaiter.wait(for: [XCTNSPredicateExpectation(
            predicate: NSPredicate(format: "hittable == true"),
            object: app.buttons["展开更多功能"])], timeout: 5), .completed)
        app.buttons["对话列表"].tap()
        waitForDrawer(app)
        app.buttons["ai-conversation-switch-A"].tap()
        XCTAssertTrue(app.buttons["ai-browser-task-entry"].waitForExistence(timeout: 5))
        openPage(app)
        app.buttons["ai-browser-more"].tap()
        app.buttons["结束网页任务"].tap()
        XCTAssertEqual(XCTWaiter.wait(for: [XCTNSPredicateExpectation(
            predicate: NSPredicate(format: "exists == false"),
            object: app.buttons["ai-browser-return"])], timeout: 5), .completed)
        XCTAssertFalse(app.buttons["ai-browser-task-entry"].exists)
        XCTAssertFalse(app.webViews.firstMatch.isHittable)
        capture(app, "browser-task-ended-original-chat-restored")
    }

    @MainActor private func waitForDrawer(_ app: XCUIApplication) {
        XCTAssertEqual(XCTWaiter.wait(for: [XCTNSPredicateExpectation(
            predicate: NSPredicate(format: "enabled == true AND hittable == true"),
            object: app.buttons["新对话"])], timeout: 5), .completed)
    }

    @MainActor func testBrowserStopRejectsLateNavigationCompletion() throws {
        let app = launchPage("/slow")
        defer { app.terminate() }
        openPage(app)
        let stop = app.buttons["ai-browser-stop"]
        XCTAssertTrue(stop.waitForExistence(timeout: 5))
        stop.tap()
        XCTAssertTrue(app.staticTexts["已停止网页操作"].waitForExistence(timeout: 5))
        capture(app, "browser-stopped-page")
        returnToChat(app)
        // 等真实慢响应返回，确认旧完成事件不会重新启动页面或自动弹出网页。
        Thread.sleep(forTimeInterval: 21)
        XCTAssertTrue(app.buttons["ai-browser-task-entry"].label.contains("已停止"))
        XCTAssertFalse(app.buttons["ai-browser-return"].exists)
    }

    @MainActor func testBrowserToolsWorkWhilePageIsCollapsed() throws {
        continueAfterFailure = false
        let app = XCUIApplication()
        app.launchArguments = ["--ai-ui-test", "--ai-browser-ui-test"]
        app.launch()
        defer { app.terminate() }
        let result = app.staticTexts["browser-fixture-verification-result"]
        XCTAssertTrue(result.waitForExistence(timeout: 20))
        XCTAssertEqual(result.label, "PASS: observe, type, click, verify, screenshot")
        XCTAssertFalse(app.webViews.firstMatch.isHittable)
        XCTAssertFalse(app.buttons["ai-browser-return"].exists)
        XCTAssertTrue(app.buttons["ai-browser-task-entry"].label.contains("需要你操作"))
        capture(app, "browser-collapsed-tool-execution-verified")
    }

    @MainActor func testBrowserFormUsesStableTargetsAndAccessibleLabels() throws {
        continueAfterFailure = false
        let app = XCUIApplication()
        app.launchArguments = ["--ai-ui-test", "--ai-browser-ui-test"]
        app.launchEnvironment["MAICHAT_BROWSER_FORM_TEST"] = "1"
        app.launch()
        defer { app.terminate() }
        let result = app.staticTexts["browser-fixture-verification-result"]
        XCTAssertTrue(result.waitForExistence(timeout: 60))
        capture(app, "browser-form-tool-contract")
        XCTAssertEqual(result.label, "PASS: browser form targets, labels, loading and input handoff")
    }

    @MainActor func testSMSIntegrationSettingsCanBeEnabledAndDisabled() throws {
        let app = launchPage()
        defer { app.terminate() }
        app.buttons["我的"].tap()
        app.buttons["设置"].tap()
        let entry = app.buttons["sms-integration-entry"]
        XCTAssertTrue(entry.waitForExistence(timeout: 5))
        entry.tap()
        let toggle = app.switches["sms-shortcut-enabled"]
        XCTAssertTrue(toggle.waitForExistence(timeout: 5))
        // SwiftUI 的无障碍 Switch 覆盖整行，实际 UISwitch 位于右侧；按屏幕上的控件操作。
        func tapSwitch() { toggle.coordinate(withNormalizedOffset: CGVector(dx: 0.9, dy: 0.5)).tap() }
        if toggle.value as? String == "1" { tapSwitch() }
        capture(app, "sms-integration-default-off")
        tapSwitch()
        XCTAssertEqual(XCTWaiter.wait(for: [XCTNSPredicateExpectation(
            predicate: NSPredicate(format: "value == '1' AND enabled == true"), object: toggle)], timeout: 5), .completed)
        capture(app, "sms-integration-enabled-setup")
        tapSwitch()
        XCTAssertEqual(XCTWaiter.wait(for: [XCTNSPredicateExpectation(
            predicate: NSPredicate(format: "value == '0' AND enabled == true"), object: toggle)], timeout: 5), .completed)
        XCTAssertFalse(app.buttons["sms-clear-imported"].isEnabled)
    }

    @MainActor func testSMSImportActionIsDiscoverableInSystemShortcuts() throws {
        continueAfterFailure = false
        let shortcuts = XCUIApplication(bundleIdentifier: "com.apple.shortcuts")
        shortcuts.launch()
        defer { shortcuts.terminate() }
        XCTAssertTrue(shortcuts.navigationBars.firstMatch.waitForExistence(timeout: 10))
        // 系统“所有快捷指令”页面已现场截图确认，右上角为创建按钮。
        shortcuts.coordinate(withNormalizedOffset: CGVector(dx: 0.90, dy: 0.085)).tap()
        let addAction = shortcuts.buttons.matching(NSPredicate(format: "label IN %@", ["添加操作", "Add Action"])).firstMatch
        if addAction.waitForExistence(timeout: 3) { addAction.tap() }
        let search = shortcuts.searchFields.firstMatch
        XCTAssertTrue(search.waitForExistence(timeout: 10), shortcuts.debugDescription)
        search.tap()
        search.typeText("MaiChat")
        let action = shortcuts.descendants(matching: .any).matching(NSPredicate(
            format: "label == %@", "将短信交给 MaiChat")).firstMatch
        XCTAssertTrue(action.waitForExistence(timeout: 10), shortcuts.debugDescription)
        capture(shortcuts, "sms-import-action-in-system-shortcuts")
        // 尚未添加动作或执行；退出未配置的编辑器，不创建自动化或读取系统消息。
    }

    @MainActor func testBrowserFileControlOffersSystemSelectionAndCancel() throws {
        let app = launchPage("/form")
        defer { app.terminate() }
        openPage(app)
        capture(app, "browser-httpbin-form-top")
        let file = app.webViews.buttons["Upload a file"]
        for _ in 0..<6 where !file.isHittable { app.webViews.firstMatch.swipeUp() }
        XCTAssertTrue(file.isHittable, app.webViews.firstMatch.debugDescription)
        file.tap()
        let picker = app.buttons.matching(NSPredicate(format: "label IN %@", ["选取文件", "Choose File", "Choose Files"])).firstMatch
        XCTAssertTrue(picker.waitForExistence(timeout: 5), app.debugDescription)
        capture(app, "browser-system-file-source-menu")
        picker.tap()
        let cancel = app.buttons.matching(NSPredicate(format: "label IN %@", ["取消", "Cancel"])).firstMatch
        XCTAssertTrue(cancel.waitForExistence(timeout: 5), app.debugDescription)
        capture(app, "browser-system-file-picker")
        cancel.tap()
        XCTAssertTrue(app.buttons["ai-browser-return"].isHittable)
        returnToChat(app)
    }

    @MainActor func testBrowserShowsPendingConfirmationAndReturnsToOriginalDialog() throws {
        let app = launchPage(approval: true)
        defer { app.terminate() }
        openPage(app)
        let confirmation = app.buttons["ai-browser-confirmation"]
        XCTAssertTrue(confirmation.waitForExistence(timeout: 5))
        capture(app, "browser-pending-confirmation")
        confirmation.tap()
        XCTAssertTrue(app.buttons["创作中心"].isHittable)
        XCTAssertTrue(app.staticTexts["mobile_browser"].waitForExistence(timeout: 5))
        capture(app, "browser-confirmation-in-original-chat")
    }

    @MainActor func testBrowserFailureOffersRetryWithoutLeavingChat() async throws {
        _ = try await URLSession.shared.data(from: URL(string: "http://127.0.0.1:18680/test/retry-reset")!)
        let app = launchPage("/failure")
        defer { app.terminate() }
        openPage(app)
        XCTAssertTrue(app.buttons["ai-browser-retry"].waitForExistence(timeout: 15))
        capture(app, "browser-load-failure-retry")
        _ = try await URLSession.shared.data(from: URL(string: "http://127.0.0.1:18680/test/retry-ready")!)
        app.buttons["ai-browser-retry"].tap()
        XCTAssertTrue(app.webViews.textFields["网页备注"].waitForExistence(timeout: 15),
                      "重试必须真正恢复首次加载失败的页面")
        capture(app, "browser-load-failure-recovered")
        returnToChat(app)
        XCTAssertTrue(app.buttons["ai-browser-task-entry"].exists)
    }
}
