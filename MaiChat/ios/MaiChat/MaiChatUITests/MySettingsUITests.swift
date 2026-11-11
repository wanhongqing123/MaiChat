// 文件作用：在模拟器正常 Root 中验收跨页面字体、字号、“我的”、Markdown 表格及确认浮层取消。
import XCTest

final class MySettingsUITests: XCTestCase {
    @MainActor func testNotificationStalePermitCannotRegressBadge() {
        runNotificationContract("stale-permit")
    }

    @MainActor func testNotificationReorderedPreviewsUseAuthoritativeBadge() {
        runNotificationContract("reordered-previews")
    }

    @MainActor func testNotificationReadCanDecreaseBadgeDuringAsyncAdd() {
        runNotificationContract("read-decrease")
    }

    @MainActor func testDoNotDisturbControlledDeliveryInterleavings() {
        runNotificationContract("interleavings")
    }

    @MainActor private func runNotificationContract(_ scenario: String) {
        continueAfterFailure = false
        let app = XCUIApplication()
        app.launchArguments = ["--ai-ui-test", "--my-settings-ui-test", "--app-bars-layout-ui-test",
                               "--notification-dnd-contract-ui-test"]
        app.launchEnvironment["MAICHAT_DND_SCENARIO"] = scenario
        app.launch()
        defer { app.terminate() }
        let run = app.buttons["notification-dnd-contract-run"]
        XCTAssertTrue(run.waitForExistence(timeout: 15))
        run.tap()
        let result = app.staticTexts["notification-dnd-contract-result"]
        let finished = XCTNSPredicateExpectation(
            predicate: NSPredicate(format: "value == 'passed' OR value BEGINSWITH 'failed:'"),
            object: result
        )
        XCTAssertEqual(XCTWaiter.wait(for: [finished], timeout: 10), .completed)
        capture(app, "notification-\(scenario)")
        XCTAssertEqual(result.value as? String, "passed")
    }

    @MainActor func testDoNotDisturbPersistsAndCanBeDisabledFromNotificationSettings() {
        continueAfterFailure = false
        let app = XCUIApplication()
        app.launchArguments = ["--ai-ui-test", "--my-settings-ui-test", "--app-bars-layout-ui-test"]
        app.launch()
        defer { app.terminate() }
        func openNotifications() {
            XCTAssertTrue(app.buttons["my-preferences-entry"].waitForExistence(timeout: 15))
            app.buttons["my-preferences-entry"].tap()
            app.buttons["preferences-notifications"].tap()
        }
        openNotifications()
        let toggle = app.switches["notification-do-not-disturb"]
        capture(app, "notifications-before-do-not-disturb")
        XCTAssertTrue(toggle.waitForExistence(timeout: 5))
        XCTAssertEqual(toggle.value as? String, "0")
        // SwiftUI 的 switch AX 框包含整行标签；中心点落在空白区域。
        // 点截图中右侧实际可见的开关，保留同一真实 Toggle 及值断言。
        XCTAssertTrue(toggle.isHittable)
        toggle.coordinate(withNormalizedOffset: CGVector(dx: 0.94, dy: 0.5)).tap()
        XCTAssertEqual(toggle.value as? String, "1")
        capture(app, "notifications-do-not-disturb-enabled")
        app.terminate()
        app.launch()
        openNotifications()
        XCTAssertEqual(toggle.value as? String, "1")
        toggle.coordinate(withNormalizedOffset: CGVector(dx: 0.94, dy: 0.5)).tap()
        XCTAssertEqual(toggle.value as? String, "0")
        capture(app, "notifications-do-not-disturb-disabled")
    }

    // 对照真实列表 Button 内的同文标题、摘要与菜单，避免只检查字号常量。
    @MainActor func testIMConversationListUsesSelectedFontAndRelativeSizes() {
        continueAfterFailure = false
        let app = XCUIApplication()
        app.launchArguments = ["--ai-ui-test", "--my-settings-ui-test", "--typography-ui-test", "--im-list-font-ui-test"]
        app.launch()
        defer { app.terminate() }
        XCTAssertTrue(app.buttons["my-preferences-entry"].waitForExistence(timeout: 15))
        for family in ["notoSerif", "system", "notoSans"] {
            app.buttons["my-theme-entry"].tap()
            app.buttons[family == "notoSerif" ? "theme-dark" : "theme-light"].tap()
            app.navigationBars.buttons.firstMatch.tap()
            app.buttons["my-preferences-entry"].tap()
            app.buttons["preferences-font-family"].tap()
            app.buttons["font-family-\(family)"].tap()
            app.navigationBars.buttons.firstMatch.tap()
            app.navigationBars.buttons.firstMatch.tap()
            for preset in ["standard", "extraLarge"] {
                app.buttons["my-preferences-entry"].tap()
                app.buttons["preferences-font-size"].tap()
                app.buttons["font-size-\(preset)"].tap()
                app.navigationBars.buttons.firstMatch.tap()
                app.navigationBars.buttons.firstMatch.tap()
                let menuHeight = app.staticTexts["偏好设置"].firstMatch.frame.height
                capture(app, "im-menu-\(family)-\(preset)")
                app.buttons["消息"].firstMatch.tap()
                let row = app.buttons["im-conversation-bars-peer-34"]
                let group = app.buttons["im-group-conversation-font-group"]
                XCTAssertTrue(row.waitForExistence(timeout: 10))
                XCTAssertTrue(group.waitForExistence(timeout: 5))
                let sameText = NSPredicate(format: "label == %@", "偏好设置")
                let title = row.staticTexts.matching(sameText).element(boundBy: 0)
                let preview = row.staticTexts.matching(sameText).element(boundBy: 1)
                let groupTitle = group.staticTexts.matching(sameText).element(boundBy: 0)
                let groupPreview = group.staticTexts.matching(sameText).element(boundBy: 1)
                XCTAssertTrue(preview.waitForExistence(timeout: 5))
                capture(app, "im-list-\(family)-\(preset)")
                let metrics = XCTAttachment(string: "\(family)/\(preset): menu=\(menuHeight), title=\(title.frame.height), preview=\(preview.frame.height), group=\(groupTitle.frame.height), groupPreview=\(groupPreview.frame.height)")
                metrics.name = "im-list-font-metrics-\(family)-\(preset)"; metrics.lifetime = .keepAlways; add(metrics)
                XCTAssertEqual(title.frame.height, menuHeight, accuracy: 0.4, "会话名应与菜单使用同一正文基准")
                XCTAssertEqual(groupTitle.frame.height, menuHeight, accuracy: 0.4, "群聊名应与菜单使用同一正文基准")
                XCTAssertEqual(preview.frame.height, menuHeight * 0.875, accuracy: 0.5, "摘要按正文的统一比例缩小")
                XCTAssertEqual(groupPreview.frame.height, preview.frame.height, accuracy: 0.4)
                app.buttons["我的"].firstMatch.tap()
            }
        }
        app.buttons["my-preferences-entry"].tap()
        app.buttons["preferences-font-size"].tap()
        app.buttons["font-size-standard"].tap()
        app.navigationBars.buttons.firstMatch.tap()
        app.buttons["preferences-font-family"].tap()
        app.buttons["font-family-system"].tap()
    }

    @MainActor func testConversationDrawerUsesTheSelectedBodyFontAndSize() {
        continueAfterFailure = false
        let app = XCUIApplication()
        app.launchArguments = ["--ai-ui-test", "--my-settings-ui-test", "--drawer-font-ui-test"]
        app.launch()
        defer { app.terminate() }
        XCTAssertTrue(app.buttons["my-preferences-entry"].waitForExistence(timeout: 15))
        for family in ["system", "notoSans", "notoSerif"] {
            app.buttons["my-preferences-entry"].tap()
            app.buttons["preferences-font-family"].tap()
            app.buttons["font-family-\(family)"].tap()
            app.navigationBars.buttons.firstMatch.tap()
            app.navigationBars.buttons.firstMatch.tap()
            for preset in ["standard", "extraLarge"] {
                app.buttons["my-preferences-entry"].tap()
                app.buttons["preferences-font-size"].tap()
                app.buttons["font-size-\(preset)"].tap()
                app.navigationBars.buttons.firstMatch.tap()
                app.navigationBars.buttons.firstMatch.tap()
                app.buttons["创作中心"].firstMatch.tap()
                let body = app.staticTexts["偏好设置"].firstMatch
                XCTAssertTrue(body.waitForExistence(timeout: 10))
                let bodyHeight = body.frame.height
                capture(app, "drawer-body-\(family)-\(preset)")
                app.buttons["对话列表"].tap()
                let local = app.buttons["ai-conversation-history-ui-test"].staticTexts["偏好设置"]
                let remote = app.buttons["ai-remote-conversation-mac:drawer-font-mac:drawer-remote"].staticTexts["偏好设置"]
                XCTAssertTrue(local.waitForExistence(timeout: 5))
                XCTAssertTrue(remote.waitForExistence(timeout: 5))
                capture(app, "drawer-\(family)-\(preset)")
                XCTAssertEqual(local.frame.height, bodyHeight, accuracy: 0.4, "本机会话标题应使用正文基准")
                XCTAssertEqual(remote.frame.height, bodyHeight, accuracy: 0.4, "远端会话标题应使用正文基准")
                // 抽屉展开时，返回遮罩的大部分区域在屏幕外；点可见聊天卡片，
                // 再等待实际收回，避免把遮罩点击误当成底栏“我的”点击。
                app.coordinate(withNormalizedOffset: CGVector(dx: 0.96, dy: 0.5)).tap()
                XCTAssertEqual(XCTWaiter.wait(for: [XCTNSPredicateExpectation(
                    predicate: NSPredicate(format: "hittable == true"), object: app.buttons["对话列表"])],
                    timeout: 6), .completed)
                XCTAssertFalse(app.buttons["ai-session-return"].exists)
                app.buttons["我的"].firstMatch.tap()
                XCTAssertTrue(app.buttons["my-preferences-entry"].waitForExistence(timeout: 5))
            }
        }
        app.buttons["my-preferences-entry"].tap()
        app.buttons["preferences-font-size"].tap()
        app.buttons["font-size-standard"].tap()
        app.navigationBars.buttons.firstMatch.tap()
        app.buttons["preferences-font-family"].tap()
        app.buttons["font-family-system"].tap()
    }

    // 从正常联系人卡片进入资料页，对照同一个昵称，补上字体统一验收遗漏的导航路径。
    @MainActor func testContactProfileUsesTheSameBodyAsNicknameRow() {
        continueAfterFailure = false
        let app = XCUIApplication()
        app.launchArguments = ["--ai-ui-test", "--my-settings-ui-test", "--typography-ui-test", "--contacts-profile-ui-test"]
        app.launch()
        defer { app.terminate() }
        XCTAssertTrue(app.buttons["my-preferences-entry"].waitForExistence(timeout: 15))
        for family in ["system", "notoSans", "notoSerif"] {
            app.buttons["my-theme-entry"].tap()
            app.buttons[family == "notoSerif" ? "theme-dark" : "theme-light"].tap()
            app.navigationBars.buttons.firstMatch.tap()
            app.buttons["my-preferences-entry"].tap()
            app.buttons["preferences-font-family"].tap()
            app.buttons["font-family-\(family)"].tap()
            app.navigationBars.buttons.firstMatch.tap()
            app.navigationBars.buttons.firstMatch.tap()
            for preset in ["standard", "extraLarge"] {
                app.buttons["my-preferences-entry"].tap()
                app.buttons["preferences-font-size"].tap()
                app.buttons["font-size-\(preset)"].tap()
                app.navigationBars.buttons.firstMatch.tap()
                app.navigationBars.buttons.firstMatch.tap()
                app.buttons["好友列表"].firstMatch.tap()
                let row = app.buttons["friend-profile-bars-peer-0"]
                XCTAssertTrue(row.waitForExistence(timeout: 10))
                let rowHeight = app.staticTexts["边缘测试联系人 0"].firstMatch.frame.height
                XCTAssertFalse(app.staticTexts["sample-account-bars-peer-0"].exists)
                capture(app, "contacts-\(family)-\(preset)")
                row.tap()
                let nickname = app.staticTexts["friend-profile-nickname"]
                XCTAssertTrue(nickname.waitForExistence(timeout: 10))
                capture(app, "contact-profile-\(family)-\(preset)")
                XCTAssertEqual(nickname.frame.height, rowHeight, accuracy: 0.4,
                               "资料页昵称与联系人昵称应使用同一正文基准")
                XCTAssertEqual(app.staticTexts["friend-profile-account-id"].label, "sample-account-bars-peer-0")
                XCTAssertTrue(app.buttons["friend-profile-copy"].exists)
                XCTAssertTrue(app.buttons["friend-profile-message"].exists)
                app.navigationBars.buttons.firstMatch.tap()
                app.buttons["我的"].firstMatch.tap()
            }
        }
        app.buttons["好友列表"].firstMatch.tap()
        app.buttons["friend-profile-bars-peer-1"].tap()
        let longNickname = app.staticTexts["friend-profile-nickname"]
        XCTAssertTrue(longNickname.waitForExistence(timeout: 10))
        XCTAssertEqual(longNickname.label, "测试-Nickname-中英混合的很长昵称-LongContactName-完整资料")
        capture(app, "contact-profile-long-nickname-dark")
        app.buttons["friend-profile-copy"].tap()
        app.buttons["friend-profile-message"].tap()
        XCTAssertTrue(app.textViews["message-composer-text-view"].waitForExistence(timeout: 10))
        capture(app, "contact-profile-open-message")
        app.buttons["返回会话列表"].tap()
        app.buttons["我的"].firstMatch.tap()
        app.buttons["my-preferences-entry"].tap()
        app.buttons["preferences-font-size"].tap()
        app.buttons["font-size-standard"].tap()
        app.navigationBars.buttons.firstMatch.tap()
        app.buttons["preferences-font-family"].tap()
        app.buttons["font-family-system"].tap()
        app.navigationBars.buttons.firstMatch.tap()
        app.navigationBars.buttons.firstMatch.tap()
        app.buttons["my-theme-entry"].tap()
        app.buttons["theme-light"].tap()
    }

    @MainActor func testMenuUsesTheSameBodyMetricsAsCreationText() {
        continueAfterFailure = false
        let app = XCUIApplication()
        app.launchArguments = ["--ai-ui-test", "--my-settings-ui-test", "--menu-font-ui-test"]
        app.launch()
        defer { app.terminate() }
        XCTAssertTrue(app.buttons["my-preferences-entry"].waitForExistence(timeout: 15))
        for family in ["system", "notoSans", "notoSerif"] {
            app.buttons["my-preferences-entry"].tap()
            app.buttons["preferences-font-family"].tap()
            let choice = app.buttons["font-family-\(family)"]
            XCTAssertTrue(choice.waitForExistence(timeout: 5))
            choice.tap()
            XCTAssertTrue(choice.isSelected)
            capture(app, "family-\(family)")
            app.navigationBars.buttons.firstMatch.tap()
            app.navigationBars.buttons.firstMatch.tap()
            for preset in ["standard", "extraLarge"] {
                app.buttons["my-preferences-entry"].tap()
                app.buttons["preferences-font-size"].tap()
                app.buttons["font-size-\(preset)"].tap()
                app.navigationBars.buttons.firstMatch.tap()
                app.navigationBars.buttons.firstMatch.tap()
                let menu = app.staticTexts["偏好设置"].firstMatch
                XCTAssertTrue(menu.exists)
                let menuFrame = menu.frame
                capture(app, "menu-\(family)-\(preset)")
                app.buttons["创作中心"].firstMatch.tap()
                let body = app.staticTexts["偏好设置"].firstMatch
                XCTAssertTrue(body.waitForExistence(timeout: 10))
                let bodyFrame = body.frame
                capture(app, "body-\(family)-\(preset)")
                let metrics = XCTAttachment(string: "\(family)/\(preset): menu=\(menuFrame), body=\(bodyFrame)")
                metrics.name = "menu-body-font-metrics-\(family)-\(preset)"; metrics.lifetime = .keepAlways; add(metrics)
                XCTAssertEqual(menuFrame.height, bodyFrame.height, accuracy: 0.4, "同一字体正文不能因 List/NavigationLink 改变字号")
                app.buttons["我的"].firstMatch.tap()
            }
        }
        app.buttons["my-preferences-entry"].tap()
        app.buttons["preferences-font-size"].tap()
        app.buttons["font-size-standard"].tap()
        app.navigationBars.buttons.firstMatch.tap()
        app.buttons["preferences-font-family"].tap()
        app.buttons["font-family-system"].tap()
    }

    @MainActor func testUnifiedFontsAndDarkThemeAcrossNormalTabs() {
        continueAfterFailure = false
        let app = XCUIApplication()
        app.launchArguments = ["--ai-ui-test", "--my-settings-ui-test", "--markdown-breaks-ui-test", "--typography-ui-test"]
        app.launch()
        defer { app.terminate() }
        XCTAssertTrue(app.buttons["my-theme-entry"].waitForExistence(timeout: 15))
        app.buttons["my-theme-entry"].tap()
        app.buttons["theme-dark"].tap()
        app.navigationBars.buttons.firstMatch.tap()
        capture(app, "dark-my-profile")
        for tab in ["消息", "好友列表", "云盘", "创作中心"] {
            app.buttons[tab].firstMatch.tap()
            if tab == "消息" {
                let peer = app.buttons["im-conversation-bars-peer-34"]
                XCTAssertTrue(peer.waitForExistence(timeout: 10))
                capture(app, "dark-messages")
                peer.tap()
                XCTAssertTrue(app.staticTexts.matching(NSPredicate(format: "label CONTAINS %@", "Expected:")).firstMatch.waitForExistence(timeout: 10))
                capture(app, "dark-im-markdown")
                app.buttons["返回会话列表"].tap()
            } else {
                if tab == "好友列表" {
                    XCTAssertTrue(app.staticTexts["边缘测试联系人 0"].waitForExistence(timeout: 5))
                } else if tab == "云盘" {
                    XCTAssertTrue(app.staticTexts["示例文件夹 Typography"].waitForExistence(timeout: 5))
                }
                capture(app, "dark-\(tab)")
            }
        }
        app.buttons["我的"].firstMatch.tap()
        app.buttons["my-preferences-entry"].tap()
        app.buttons["preferences-font-size"].tap()
        let preview = app.staticTexts.matching(NSPredicate(format: "label CONTAINS %@", "你好，Hello MaiChat")).firstMatch
        var heights: [CGFloat] = []
        for preset in ["standard", "large", "extraLarge"] {
            app.buttons["font-size-\(preset)"].tap()
            XCTAssertTrue(preview.waitForExistence(timeout: 5))
            heights.append(preview.frame.height)
            capture(app, "font-\(preset)-markdown")
        }
        XCTAssertGreaterThan(heights[1], heights[0])
        XCTAssertGreaterThan(heights[2], heights[1])
        app.buttons["font-size-standard"].tap()
        app.navigationBars.buttons.firstMatch.tap()
        app.navigationBars.buttons.firstMatch.tap()
        app.buttons["my-theme-entry"].tap()
        app.buttons["theme-light"].tap()
        app.navigationBars.buttons.firstMatch.tap()
        capture(app, "light-my-profile")
        app.buttons["创作中心"].firstMatch.tap()
        capture(app, "light-creation-markdown")
    }

    @MainActor func testMarkdownTableBreaksInIMAndCreationCenter() {
        continueAfterFailure = false
        let app = XCUIApplication()
        app.launchArguments = ["--ai-ui-test", "--markdown-breaks-ui-test"]
        app.launch()
        defer { app.terminate() }
        let peer = app.buttons["im-conversation-bars-peer-34"]
        XCTAssertTrue(peer.waitForExistence(timeout: 15))
        peer.tap()
        let cell = app.staticTexts.matching(NSPredicate(format: "label == %@",
            "Expected:\n320x180\n\ndetails")).firstMatch
        XCTAssertTrue(cell.waitForExistence(timeout: 15))
        let literal = app.staticTexts.matching(NSPredicate(format: "label CONTAINS %@", "literal <br>")).firstMatch
        XCTAssertTrue(literal.exists)
        capture(app, "markdown-im-table-breaks")
        app.buttons["返回会话列表"].tap()
        app.buttons["创作中心"].tap()
        XCTAssertTrue(cell.waitForExistence(timeout: 15))
        XCTAssertTrue(literal.exists)
        capture(app, "markdown-creation-table-breaks")
    }

    @MainActor func testMyPagesKeepWorkingPreferencesAndSafeCleanupConfirmation() {
        continueAfterFailure = false
        let app = XCUIApplication()
        app.launchArguments = ["--ai-ui-test", "--my-settings-ui-test"]
        app.launch()
        defer { app.terminate() }
        XCTAssertTrue(app.buttons["personal-profile-entry"].waitForExistence(timeout: 15))
        capture(app, "my-page")
        XCTAssertFalse(app.staticTexts["500"].exists)
        XCTAssertFalse(app.buttons["升级套餐"].exists)
        app.buttons["my-theme-entry"].tap()
        XCTAssertTrue(app.buttons["theme-system"].waitForExistence(timeout: 5))
        let previousTheme = ["system", "light", "dark"].first { app.buttons["theme-\($0)"].isSelected } ?? "system"
        app.buttons["theme-dark"].tap()
        XCTAssertTrue(app.buttons["theme-dark"].isSelected)
        capture(app, "my-theme-dark")
        app.buttons["theme-system"].tap()
        app.buttons["theme-\(previousTheme)"].tap()
        app.navigationBars.buttons.firstMatch.tap()
        app.buttons["my-preferences-entry"].tap()
        XCTAssertTrue(app.buttons["preferences-font-size"].waitForExistence(timeout: 5))
        capture(app, "my-preferences")
        app.buttons["preferences-font-size"].tap()
        let previousSize = ["system", "standard", "large", "extraLarge"].first {
            app.buttons["font-size-\($0)"].isSelected
        } ?? "system"
        app.buttons["font-size-large"].tap()
        XCTAssertTrue(app.staticTexts["font-size-preview"].exists)
        capture(app, "my-font-large")
        app.buttons["font-size-\(previousSize)"].tap()
        app.navigationBars.buttons.firstMatch.tap()
        app.navigationBars.buttons.firstMatch.tap()

        app.buttons["my-storage-entry"].tap()
        XCTAssertTrue(app.staticTexts["storage-total-size"].waitForExistence(timeout: 15))
        capture(app, "my-storage")
        let preview = app.buttons["storage-preview-all"]
        let ready = XCTNSPredicateExpectation(predicate: NSPredicate(format: "enabled == true"), object: preview)
        XCTAssertEqual(XCTWaiter.wait(for: [ready], timeout: 15), .completed)
        preview.tap()
        // QA 只查看精确预览并取消，不执行共享临时目录清理。
        let noFiles = app.buttons["好"]
        XCTAssertTrue(app.sheets.firstMatch.waitForExistence(timeout: 10) || noFiles.waitForExistence(timeout: 3))
        capture(app, "my-storage-preview-or-empty")
        if noFiles.exists { noFiles.tap() } else { dismissSystemPrompt(app) }
        app.navigationBars.buttons.firstMatch.tap()

        let about = app.buttons["my-about-entry"]
        if !about.isHittable { app.swipeUp() }
        about.tap()
        XCTAssertTrue(app.buttons["about-export-logs"].waitForExistence(timeout: 5))
        capture(app, "my-about")
        app.buttons["about-export-logs"].tap()
        XCTAssertTrue(app.buttons["最近 1 小时"].waitForExistence(timeout: 5))
        dismissSystemPrompt(app)
        app.buttons["about-clear-logs"].tap()
        XCTAssertTrue(app.sheets.firstMatch.waitForExistence(timeout: 5))
        capture(app, "my-clear-logs-confirmation")
        dismissSystemPrompt(app)
        app.navigationBars.buttons.firstMatch.tap()
        let logout = app.buttons["my-logout"]
        if !logout.isHittable { app.swipeUp() }
        logout.tap()
        XCTAssertTrue(app.sheets.firstMatch.waitForExistence(timeout: 5))
        capture(app, "my-logout-confirmation")
        dismissSystemPrompt(app)
        XCTAssertTrue(app.navigationBars["我的"].exists)
    }

    @MainActor private func dismissSystemPrompt(_ app: XCUIApplication) {
        if app.buttons["取消"].exists { app.buttons["取消"].tap(); return }
        // 当前系统会把确认框适配成 popover，取消由点击外部完成，不渲染取消行。
        let dismiss = app.otherElements["PopoverDismissRegion"]
        XCTAssertTrue(dismiss.exists)
        dismiss.coordinate(withNormalizedOffset: CGVector(dx: 0.03, dy: 0.5)).tap()
    }

    @MainActor private func capture(_ app: XCUIApplication, _ name: String) {
        let image = XCTAttachment(screenshot: app.screenshot())
        image.name = name
        image.lifetime = .keepAlways
        add(image)
    }
}
