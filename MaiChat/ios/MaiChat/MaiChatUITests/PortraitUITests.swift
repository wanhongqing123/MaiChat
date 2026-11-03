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
    @MainActor func testLargePhotoWhiteningReturnsOriginalSizeAndSkinMask() throws {
        continueAfterFailure = false
        let app = XCUIApplication()
        app.launchArguments = ["--ai-ui-test", "--ai-portrait-ui-test"]
        app.launchEnvironment["MAICHAT_PORTRAIT_LARGE_TEST"] = "1"
        app.launch()
        let status = app.staticTexts["portrait-fixture-status"]
        XCTAssertTrue(status.waitForExistence(timeout: 25))
        expectation(for: NSPredicate(format: "label BEGINSWITH 'PASS' OR label BEGINSWITH 'FAIL'"), evaluatedWith: status)
        waitForExpectations(timeout: 120)
        XCTAssertTrue(status.label.hasPrefix("PASS：24MP 5712×4284 image+L8 mask"), status.label)
        let qaElement = app.staticTexts["portrait-fixture-qa"]
        XCTAssertTrue(qaElement.exists)
        let qaText = qaElement.label
        let qa = try XCTUnwrap(try JSONSerialization.jsonObject(with: Data(qaText.utf8)) as? [String: Any])
        XCTAssertEqual(qa["width"] as? Int, 5712)
        XCTAssertEqual(qa["height"] as? Int, 4284)
        XCTAssertEqual(qa["mask_width"] as? Int, 5712)
        XCTAssertEqual(qa["mask_height"] as? Int, 4284)
        XCTAssertEqual(qa["mask_format"] as? String, "L8")
        XCTAssertEqual(qa["face_count"] as? Int, 1)
        XCTAssertTrue(qa["person_count"] is NSNull)
        for field in ["source_unchanged", "background_blocks_equal", "unknown_parameter", "file_not_found", "input_not_ready"] {
            XCTAssertEqual(qa[field] as? Bool, true, field)
        }
        XCTAssertEqual(qa["source_sha_before"] as? String, qa["source_sha_after"] as? String)
        XCTAssertGreaterThan(try XCTUnwrap(qa["skin_L_after"] as? Double), try XCTUnwrap(qa["skin_L_before"] as? Double))
        XCTAssertLessThan(try XCTUnwrap(qa["skin_C_after"] as? Double), try XCTUnwrap(qa["skin_C_before"] as? Double))
        let report = XCTAttachment(string: qaText)
        report.name = "portrait-large-photo-qa.json"
        report.lifetime = .keepAlways
        add(report)
        func attach(_ name: String) {
            let attachment = XCTAttachment(screenshot: app.screenshot())
            attachment.name = name; attachment.lifetime = .keepAlways; add(attachment)
        }
        let thumbnail = app.descendants(matching: .any)["agent-image-bubble"].firstMatch
        XCTAssertTrue(thumbnail.waitForExistence(timeout: 10))
        attach("portrait-24mp-original-size-status-and-thumbnail")
        thumbnail.tap()
        let preview = app.images["agent-image-preview"]
        XCTAssertTrue(preview.waitForExistence(timeout: 15))
        attach("portrait-24mp-output-fullscreen")
        preview.tap()
        XCTAssertEqual(XCTWaiter.wait(for: [XCTNSPredicateExpectation(
            predicate: NSPredicate(format: "exists == false"), object: preview)], timeout: 10), .completed)
        XCTAssertTrue(thumbnail.exists)
        attach("portrait-24mp-returned-to-message")
    }

}
