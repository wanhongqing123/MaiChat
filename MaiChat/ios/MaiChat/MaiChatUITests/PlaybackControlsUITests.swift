import XCTest

final class PlaybackControlsUITests: XCTestCase {
    @MainActor func testControlsHideWakeAndRemainDuringPauseAndSpeedChoice() throws {
        continueAfterFailure = false
        let app = XCUIApplication()
        app.launchArguments = ["--ai-ui-test", "--ffplay-seek-ui-test"]
        app.launch()
        defer { app.terminate() }
        let video = app.images["ffplay-video-surface"]
        XCTAssertTrue(video.waitForExistence(timeout: 20))
        let pause = app.buttons["ffplay-pause"]
        if !pause.exists || !pause.isHittable { video.tap() }
        app.buttons["ffplay-speed"].tap()
        app.buttons["ffplay-speed-0.5"].tap()
        waitForHidden(pause)
        capture(app, "ios-playback-controls-auto-hidden")
        video.tap()
        XCTAssertTrue(pause.waitForExistence(timeout: 3))
        pause.tap()
        XCTAssertEqual(pause.label, "播放视频")
        Thread.sleep(forTimeInterval: 3)
        XCTAssertTrue(pause.isHittable, "暂停时保留控制条")
        pause.tap()
        app.buttons["ffplay-speed"].tap()
        Thread.sleep(forTimeInterval: 3)
        XCTAssertTrue(app.buttons["ffplay-speed-0.5"].isHittable, "操作倍速时不能收起菜单")
        capture(app, "ios-playback-speed-keeps-controls")
        app.buttons["ffplay-speed-0.5"].tap()
        waitForHidden(pause)
        video.tap()
        XCTAssertTrue(pause.waitForExistence(timeout: 3))
        app.buttons["ffplay-close"].tap()
    }

    @MainActor func testFullScreenNormalAppPlayerHidesControlsAndWakesWithoutClosing() throws {
        continueAfterFailure = false
        let app = XCUIApplication()
        app.launchArguments = ["--ai-ui-test", "--ai-image-mixed-ui-test"]
        app.launchEnvironment["MAICHAT_MEDIA_VIDEO_FIRST"] = "1"
        app.launch()
        defer { app.terminate() }
        let video = app.images["ffplay-video-surface"]
        XCTAssertTrue(video.waitForExistence(timeout: 20))
        XCTAssertFalse(app.buttons["ffplay-expand"].exists)
        XCTAssertFalse(app.buttons["创作中心"].isHittable)
        waitForHidden(app.buttons["ffplay-pause"])
        XCTAssertTrue(video.exists, "隐藏控件不能关闭或替换当前视频")
        capture(app, "ios-normal-app-fullscreen-controls-hidden")
        video.tap()
        XCTAssertTrue(app.buttons["ffplay-pause"].waitForExistence(timeout: 3))
        XCTAssertTrue(video.exists)
        app.buttons["ffplay-close"].tap()
        XCTAssertEqual(XCTWaiter.wait(for: [XCTNSPredicateExpectation(
            predicate: NSPredicate(format: "exists == false"), object: video)], timeout: 5), .completed)
    }

    @MainActor private func waitForHidden(_ control: XCUIElement) {
        let hidden = XCTNSPredicateExpectation(predicate: NSPredicate(format: "exists == false"), object: control)
        XCTAssertEqual(XCTWaiter.wait(for: [hidden], timeout: 7), .completed)
    }

    @MainActor private func capture(_ app: XCUIApplication, _ name: String) {
        let attachment = XCTAttachment(screenshot: app.screenshot())
        attachment.name = name
        attachment.lifetime = .keepAlways
        add(attachment)
    }
}
