import XCTest

final class PortraitUITests: XCTestCase {
    @MainActor func testWelcomeDescribesPortraitToolsWithoutActions() throws {
        continueAfterFailure = false
        let app = XCUIApplication()
        app.launchArguments = ["--ai-ui-test", "--ai-welcome-ui-test"]
        app.launch()
        XCTAssertTrue(app.staticTexts["从这里开始创作"].waitForExistence(timeout: 15))
        for title in ["人像美颜", "美体塑形", "个人模型训练"] {
            let feature = app.staticTexts[title].firstMatch
            XCTAssertTrue(feature.exists, title)
            XCTAssertFalse(app.buttons[title].exists, title)
            for _ in 0..<4 where !feature.isHittable { app.swipeUp() }
            feature.tap()
            XCTAssertTrue(app.staticTexts["从这里开始创作"].exists)
            XCTAssertEqual(app.keyboards.count, 0)
        }
        let screenshot = XCTAttachment(screenshot: app.screenshot())
        screenshot.name = "portrait-tools-visible-on-welcome"
        screenshot.lifetime = .keepAlways
        add(screenshot)
        let editor = app.textViews.firstMatch
        if editor.exists {
            XCTAssertFalse((editor.value as? String ?? "").contains("个人美颜美体模型"))
        }
    }

    @MainActor func testLocalPortraitPreviewAndTrainingCandidate() throws {
        continueAfterFailure = false
        let app = XCUIApplication()
        app.launchArguments = ["--ai-ui-test", "--ai-portrait-ui-test"]
        app.launch()
        let status = app.staticTexts["portrait-fixture-status"]
        XCTAssertTrue(status.waitForExistence(timeout: 25))
        let processed = NSPredicate(format: "label BEGINSWITH 'PASS' OR label BEGINSWITH 'FAIL'")
        expectation(for: processed, evaluatedWith: status)
        waitForExpectations(timeout: 40)
        XCTAssertTrue(status.label.contains("PASS：本地美白"), status.label)
        let thumbnail = app.descendants(matching: .any)["agent-image-bubble"].firstMatch
        XCTAssertTrue(thumbnail.waitForExistence(timeout: 5))
        thumbnail.tap()
        let preview = XCTAttachment(screenshot: app.screenshot())
        preview.name = "portrait-local-output-fullscreen"
        preview.lifetime = .keepAlways
        add(preview)
        // 正常媒体预览单击收起；使用图片区域中心，避免测试状态浮层。
        app.coordinate(withNormalizedOffset: CGVector(dx: 0.5, dy: 0.42)).tap()
        XCTAssertTrue(app.buttons["测试本地训练"].waitForExistence(timeout: 5))
        app.buttons["测试本地训练"].tap()
        expectation(for: NSPredicate(format: "label CONTAINS %@", "PASS：权重已训练"), evaluatedWith: status)
        waitForExpectations(timeout: 40)
        let training = XCTAttachment(screenshot: app.screenshot())
        training.name = "portrait-local-training-candidate-status"
        training.lifetime = .keepAlways
        add(training)
    }
}
