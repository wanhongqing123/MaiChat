import XCTest

final class AccountToolsUITests: XCTestCase {
    @MainActor func testIMDatabaseRejectsRetiredNativeContextAfterAccountSwitch() {
        let app = XCUIApplication()
        app.launchArguments = ["--ai-ui-test", "--ai-account-tools-ui-test", "--im-group-ui-test",
                               "--im-query-contract-ui-test"]
        app.launchEnvironment["MAICHAT_IM_QUERY_CASE"] = "old-context"
        app.launch()
        defer { app.terminate() }
        let status = app.staticTexts["account-tools-test-status"]
        XCTAssertTrue(status.waitForExistence(timeout: 15))
        let finished = XCTNSPredicateExpectation(
            predicate: NSPredicate(format: "label BEGINSWITH 'PASS IM QUERY' OR label BEGINSWITH 'FAIL IM QUERY'"), object: status)
        XCTAssertEqual(XCTWaiter.wait(for: [finished], timeout: 15), .completed)
        XCTAssertTrue(status.label.hasPrefix("PASS IM QUERY"), status.label)
    }

    @MainActor func testIMDatabaseNativeAdmissionRejectsQueuedPreviousAccount() {
        let app = XCUIApplication()
        app.launchArguments = ["--ai-ui-test", "--ai-account-tools-ui-test", "--im-group-ui-test",
                               "--im-query-contract-ui-test"]
        app.launchEnvironment["MAICHAT_IM_QUERY_CASE"] = "switch-admission"
        app.launch()
        defer { app.terminate() }
        let status = app.staticTexts["account-tools-test-status"]
        XCTAssertTrue(status.waitForExistence(timeout: 15))
        let finished = XCTNSPredicateExpectation(
            predicate: NSPredicate(format: "label BEGINSWITH 'PASS IM QUERY' OR label BEGINSWITH 'FAIL IM QUERY'"), object: status)
        XCTAssertEqual(XCTWaiter.wait(for: [finished], timeout: 15), .completed)
        XCTAssertTrue(status.label.hasPrefix("PASS IM QUERY"), status.label)
    }

    @MainActor func testIMDatabaseCallbackDeadlineStartsBeforeMainActorAdmission() {
        let app = XCUIApplication()
        app.launchArguments = ["--ai-ui-test", "--ai-account-tools-ui-test", "--im-group-ui-test",
                               "--im-query-contract-ui-test"]
        app.launchEnvironment["MAICHAT_IM_QUERY_CASE"] = "main-budget"
        app.launch()
        defer { app.terminate() }
        let status = app.staticTexts["account-tools-test-status"]
        XCTAssertTrue(status.waitForExistence(timeout: 15))
        let finished = XCTNSPredicateExpectation(
            predicate: NSPredicate(format: "label BEGINSWITH 'PASS IM QUERY' OR label BEGINSWITH 'FAIL IM QUERY'"), object: status)
        XCTAssertEqual(XCTWaiter.wait(for: [finished], timeout: 15), .completed)
        XCTAssertTrue(status.label.hasPrefix("PASS IM QUERY"), status.label)
    }

    @MainActor func testPersistedIMToolsDoNotDependOnLoadedWindow() {
        for mode in ["peer", "page", "closed", "get", "owner", "validation"] {
            let app = XCUIApplication()
            app.launchArguments = ["--ai-ui-test", "--ai-account-tools-ui-test", "--im-group-ui-test",
                                   "--im-query-contract-ui-test"]
            app.launchEnvironment["MAICHAT_IM_QUERY_CASE"] = mode
            app.launch()
            let status = app.staticTexts["account-tools-test-status"]
            XCTAssertTrue(status.waitForExistence(timeout: 15))
            let finished = XCTNSPredicateExpectation(
                predicate: NSPredicate(format: "label BEGINSWITH 'PASS IM QUERY' OR label BEGINSWITH 'FAIL IM QUERY'"),
                object: status)
            XCTAssertEqual(XCTWaiter.wait(for: [finished], timeout: 15), .completed)
            XCTAssertTrue(status.label.hasPrefix("PASS IM QUERY"), status.label)
            app.terminate()
        }
    }

    @MainActor func testCurrentAccountAndAvatarRespectConfirmationAndAccountBoundaries() {
        continueAfterFailure = false
        let app = XCUIApplication()
        app.launchArguments = ["--ai-ui-test", "--ai-account-tools-ui-test", "--im-group-ui-test"]
        app.launch()
        defer { app.terminate() }
        let status = app.staticTexts["account-tools-test-status"]
        XCTAssertTrue(status.waitForExistence(timeout: 20))
        let finished = NSPredicate(format: "label BEGINSWITH 'PASS' OR label BEGINSWITH 'FAIL'")
        expectation(for: finished, evaluatedWith: status)
        waitForExpectations(timeout: 25)
        XCTAssertTrue(status.label.hasPrefix("PASS"), status.label)
        let capture = XCTAttachment(screenshot: app.screenshot())
        capture.name = "account-and-avatar-confirmation-boundaries"
        capture.lifetime = .keepAlways
        add(capture)

        // 通过正常主导航和个人资料入口查看最终头像，不能只把测试 PASS 当作界面验收。
        let me = app.buttons["我的"]
        XCTAssertTrue(me.waitForExistence(timeout: 5))
        me.tap()
        let profile = app.buttons["personal-profile-entry"]
        XCTAssertTrue(profile.waitForExistence(timeout: 5))
        profile.tap()
        XCTAssertTrue(app.navigationBars["个人资料"].waitForExistence(timeout: 5))
        let personalCapture = XCTAttachment(screenshot: app.screenshot())
        personalCapture.name = "account-normal-personal-profile-with-normalized-avatar"
        personalCapture.lifetime = .keepAlways
        add(personalCapture)
        let hierarchy = XCTAttachment(string: app.debugDescription)
        hierarchy.name = "account-normal-personal-profile-accessibility"
        hierarchy.lifetime = .keepAlways
        add(hierarchy)
        // 实际录屏已确认账号可见。LabeledContent 可以把标题与值合为一个 AX 元素，
        // 其值不一定属于 StaticText；保留精确账号匹配，定位当前可见的 label/value。
        let account = app.descendants(matching: .any).matching(NSPredicate(
            format: "label == %@ OR value == %@ OR label == %@",
            "sim-group-owner", "sim-group-owner", "账号, sim-group-owner"))
            .allElementsBoundByIndex.first { $0.isHittable }
        XCTAssertNotNil(account, "正常个人资料页没有显示精确的当前账号 sim-group-owner")
        let nickname = app.buttons["account-nickname-entry"]
        XCTAssertTrue(nickname.exists)
        XCTAssertTrue(nickname.label.contains("模拟器测试账号") ||
                      (nickname.value as? String ?? "").contains("模拟器测试账号") ||
                      app.staticTexts["模拟器测试账号"].isHittable,
                      "正常个人资料页没有显示当前账号昵称")
        XCTAssertTrue(app.buttons.matching(NSPredicate(format: "label BEGINSWITH %@", "头像")).firstMatch.isHittable)
    }
}
