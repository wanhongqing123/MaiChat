import XCTest

final class ScreenFrameArchiveUITests: XCTestCase {
    /// Consume the persisted snapshot from an actual native local-model Agent
    /// turn. The App must show its real delivery receipt in the latest reply,
    /// independently of the older capture card, without opening fullscreen.
    @MainActor
    func testNativeAgentPhotoDisplayAppearsInCurrentReply() async throws {
        #if DEBUG && targetEnvironment(simulator)
        guard let rawID = ProcessInfo.processInfo.environment["MAICHAT_NATIVE_PHOTO_REPLY_TEST_ID"],
              let token = UUID(uuidString: rawID) else {
            throw XCTSkip("Requires the explicitly staged real native Agent snapshot and public JPEG")
        }
        let caption = try XCTUnwrap(ProcessInfo.processInfo.environment["MAICHAT_NATIVE_PHOTO_REPLY_CAPTION"])
        let capturePrompt = try XCTUnwrap(ProcessInfo.processInfo.environment["MAICHAT_NATIVE_PHOTO_REPLY_CAPTURE_PROMPT"])
        XCTAssertFalse(caption.isEmpty)
        XCTAssertFalse(capturePrompt.isEmpty)
        continueAfterFailure = false
        XCUIDevice.shared.orientation = .portrait
        let app = XCUIApplication()
        app.launchArguments = ["--ai-ui-test", "--ai-image-fullscreen-ui-test",
            "--ai-native-photo-reply-ui-test", "-maichat.notification-preprompt-dismissed", "YES"]
        app.launchEnvironment["MAICHAT_AI_TEST_ID"] = token.uuidString
        app.launch()
        defer { app.terminate() }

        func currentCard() -> XCUIElement {
            let cards = app.buttons.matching(identifier: "agent-image-bubble")
            return cards.element(boundBy: max(0, cards.count - 1))
        }
        func openAssistant() {
            let tab = app.buttons["创作中心"]
            XCTAssertTrue(tab.waitForExistence(timeout: 12))
            tab.tap()
            XCTAssertTrue(app.textViews["ai-composer"].waitForExistence(timeout: 8))
        }
        func assertInlineReply() {
            let card = currentCard()
            XCTAssertTrue(card.waitForExistence(timeout: 15), "当前真实交付回执应显示图片卡片")
            XCTAssertTrue(card.isHittable, "当前回复的图片应直接可见，无需用户向上寻找旧卡片")
            XCTAssertTrue(app.staticTexts[caption].isHittable, "当前真实交付回执的说明应直接可见")
            XCTAssertFalse(app.images["agent-image-preview"].exists, "普通展示不得自动弹出全屏")
            XCTAssertTrue(app.textViews["ai-composer"].exists)
            let draft = app.textViews["ai-composer"].value as? String ?? ""
            XCTAssertTrue(draft.isEmpty || draft == "可按住转文字", "展示结果不能填入未发送草稿")
            XCTAssertFalse(app.buttons.matching(NSPredicate(format: "identifier BEGINSWITH %@",
                "ai-remove-attachment-")).firstMatch.exists, "工具交付不能追加到用户输入草稿")
        }
        openAssistant()
        assertInlineReply()
        let inline = XCTAttachment(screenshot: app.screenshot())
        inline.name = "native-agent-current-reply-photo-inline"
        inline.lifetime = .keepAlways
        add(inline)

        try await Task.sleep(for: .milliseconds(500))
        currentCard().tap()
        let preview = app.images["agent-image-preview"]
        XCTAssertTrue(preview.waitForExistence(timeout: 8))
        let expanded = XCTAttachment(screenshot: app.screenshot())
        expanded.name = "native-agent-current-reply-photo-explicit-enlargement"
        expanded.lifetime = .keepAlways
        add(expanded)
        preview.tap()
        XCTAssertEqual(XCTWaiter.wait(for: [XCTNSPredicateExpectation(
            predicate: NSPredicate(format: "exists == false"), object: preview)], timeout: 8), .completed)
        assertInlineReply()

        // Lazy history rows may leave the accessibility tree when offscreen.
        // Visit the older real capture separately so both messages are checked
        // without requiring all history thumbnails to remain allocated at once.
        let scroll = try XCTUnwrap(app.scrollViews.allElementsBoundByIndex.first {
            $0.isHittable && $0.identifier != "ai-session-drawer" &&
                $0.frame.width > 100 && $0.frame.height > 100
        }, "使用当前可交互的消息视口，不能选中保留在视图树中的隐藏会话抽屉")
        for _ in 0..<8 where !app.staticTexts[capturePrompt].isHittable { scroll.swipeDown() }
        XCTAssertTrue(app.staticTexts[capturePrompt].isHittable)
        XCTAssertTrue(app.buttons.matching(identifier: "agent-image-bubble").firstMatch.isHittable,
                      "当前再次展示不能移除旧拍照消息中的图片卡片")
        XCTAssertFalse(app.images["agent-image-preview"].exists)
        let older = XCTAttachment(screenshot: app.screenshot())
        older.name = "native-agent-earlier-capture-card-preserved"
        older.lifetime = .keepAlways
        add(older)
        for _ in 0..<8 where !app.staticTexts[caption].isHittable { scroll.swipeUp() }
        assertInlineReply()

        // Relaunch re-decodes the same unmodified persisted C ABI snapshot. No
        // completed part or assistant answer is synthesized by this fixture.
        app.terminate()
        app.launch()
        openAssistant()
        assertInlineReply()
        let restored = XCTAttachment(screenshot: app.screenshot())
        restored.name = "native-agent-current-reply-photo-restored-snapshot"
        restored.lifetime = .keepAlways
        add(restored)
        #else
        throw XCTSkip("Only the explicitly staged Debug Simulator fixture may import this snapshot")
        #endif
    }

    /// A real completed host receipt becomes an inline card immediately. Loading
    /// the same persisted part must not launch a viewer or pollute the draft.
    @MainActor
    func testManualPhotoResultShowsInlineAndRestoredHistoryOnlyOpensOnTap() async throws {
        #if DEBUG && targetEnvironment(simulator)
        let app = launchManualCameraToolFixture()
        defer { app.terminate() }
        XCTAssertFalse(app.buttons["agent-image-bubble"].exists)
        app.buttons["ai-manual-camera-tool-open"].tap()
        let shutter = app.buttons["ai-manual-camera-shutter"]
        XCTAssertTrue(shutter.waitForExistence(timeout: 8))
        XCTAssertFalse(app.buttons["agent-image-bubble"].exists)
        shutter.tap()
        assertManualToolState(app, result: "succeeded", files: 1, pending: false, verified: true)
        assertManualToolDidNotAttachToComposer(app)

        let bubble = app.buttons["agent-image-bubble"].firstMatch
        XCTAssertTrue(bubble.waitForExistence(timeout: 8), "拍照回执必须直接显示缩略图，不依赖模型再调用工具")
        XCTAssertEqual(app.buttons.matching(identifier: "agent-image-bubble").count, 1)
        XCTAssertFalse(app.images["agent-image-preview"].exists, "完成拍照不能自动打开全屏")
        let inline = XCTAttachment(screenshot: app.screenshot())
        inline.name = "manual-photo-completed-receipt-inline-card"
        inline.lifetime = .keepAlways
        add(inline)

        app.buttons["ai-manual-camera-tool-restore"].tap()
        XCTAssertTrue(bubble.waitForExistence(timeout: 5))
        XCTAssertFalse(app.images["agent-image-preview"].exists, "历史回执重载也不能自动打开全屏")
        try await Task.sleep(for: .milliseconds(500))
        bubble.tap()
        let preview = app.images["agent-image-preview"]
        XCTAssertTrue(preview.waitForExistence(timeout: 8))
        let expanded = XCTAttachment(screenshot: app.screenshot())
        expanded.name = "manual-photo-result-viewer-after-explicit-tap"
        expanded.lifetime = .keepAlways
        add(expanded)
        preview.tap()
        XCTAssertEqual(XCTWaiter.wait(for: [XCTNSPredicateExpectation(
            predicate: NSPredicate(format: "exists == false"), object: preview)], timeout: 8), .completed)
        XCTAssertTrue(bubble.isHittable)
        assertManualToolDidNotAttachToComposer(app)
        let returned = XCTAttachment(screenshot: app.screenshot())
        returned.name = "manual-photo-result-returned-to-inline-card"
        returned.lifetime = .keepAlways
        add(returned)
        #else
        throw XCTSkip("仅 Debug 模拟器使用独立工作区与合成手动快门")
        #endif
    }

    /// 真正调用 mobile_take_photo；超过采样间隔仍等待快门，返回单张文件而不改 composer。
    @MainActor
    func testManualCameraToolWaitsForShutterAndReturnsExactlyOnePhoto() async throws {
        #if DEBUG && targetEnvironment(simulator)
        let app = launchManualCameraToolFixture()
        defer { app.terminate() }
        assertManualToolState(app, result: "idle", files: 0, pending: false, verified: false)
        app.buttons["ai-manual-camera-tool-open"].tap()
        let shutter = app.buttons["ai-manual-camera-shutter"]
        XCTAssertTrue(shutter.waitForExistence(timeout: 8))
        assertManualToolState(app, preview: true, result: "pending", files: 0, pending: true, verified: false)

        try await Task.sleep(for: .milliseconds(2200))
        app.buttons["ai-manual-camera-tool-preview-refresh"].tap()
        XCTAssertTrue(shutter.isHittable, "工具请求必须继续等待显式快门")
        assertManualToolState(app, preview: true, result: "pending", files: 0, pending: true, verified: false)
        XCTAssertEqual(app.staticTexts["ai-manual-camera-timed-state"].value as? String,
                       "running:false;request:false")
        XCTAssertFalse(app.buttons["ai-camera-stop"].exists)
        let waiting = XCTAttachment(screenshot: app.screenshot())
        waiting.name = "manual-camera-tool-still-waiting-after-two-seconds"
        waiting.lifetime = .keepAlways
        add(waiting)

        shutter.tap()
        assertManualToolState(app, result: "succeeded", files: 1, pending: false, verified: true)
        let path = try XCTUnwrap(app.staticTexts["ai-manual-camera-tool-path"].value as? String)
        XCTAssertTrue(path.hasPrefix("photo-") && path.hasSuffix(".jpg"),
                      "正式宿主应返回工作区中的单张 JPEG 相对路径")
        XCTAssertEqual(app.staticTexts["ai-manual-camera-tool-mime"].value as? String, "image/jpeg")
        assertManualToolDidNotAttachToComposer(app)
        app.buttons["ai-manual-camera-tool-refresh"].tap()
        assertManualToolState(app, result: "succeeded", files: 1, pending: false, verified: true)
        #else
        throw XCTSkip("手动工具验收仅在 Debug 模拟器使用合成画面和独立工作区")
        #endif
    }

    /// 用户取消必须结束正式工具等待，重试后的快门只产生新的一张文件。
    @MainActor
    func testManualCameraToolCancelExitsAndAllowsRetry() async throws {
        #if DEBUG && targetEnvironment(simulator)
        let app = launchManualCameraToolFixture()
        defer { app.terminate() }
        app.buttons["ai-manual-camera-tool-open"].tap()
        XCTAssertTrue(app.buttons["ai-manual-camera-cancel"].waitForExistence(timeout: 8))
        app.buttons["ai-manual-camera-cancel"].tap()
        assertManualToolState(app, result: "canceled", files: 0, pending: false, verified: false)
        assertManualToolDidNotAttachToComposer(app)

        app.buttons["ai-manual-camera-tool-open"].tap()
        XCTAssertTrue(app.buttons["ai-manual-camera-shutter"].waitForExistence(timeout: 8))
        XCTAssertEqual(app.staticTexts["ai-manual-camera-timed-state"].value as? String,
                       "running:false;request:false")
        app.buttons["ai-manual-camera-shutter"].tap()
        assertManualToolState(app, result: "succeeded", files: 1, pending: false, verified: true)
        assertManualToolDidNotAttachToComposer(app)
        #else
        throw XCTSkip("手动工具验收仅在 Debug 模拟器使用合成画面和独立工作区")
        #endif
    }

    /// 停止工具 Task 与切换原会话均取消等待；旧请求不能写文件或占用后续拍摄。
    @MainActor
    func testManualCameraToolTaskCancellationAndSessionSwitchDiscardPendingPhoto() async throws {
        #if DEBUG && targetEnvironment(simulator)
        let app = launchManualCameraToolFixture()
        defer { app.terminate() }
        for cancellation in ["ai-manual-camera-tool-cancel-task", "ai-manual-camera-tool-switch-session"] {
            app.buttons["ai-manual-camera-tool-open"].tap()
            XCTAssertTrue(app.buttons["ai-manual-camera-shutter"].waitForExistence(timeout: 8))
            assertManualToolState(app, preview: true, result: "pending", files: 0, pending: true, verified: false)
            app.buttons[cancellation].tap()
            assertManualToolState(app, result: "canceled", files: 0, pending: false, verified: false)
            assertManualToolDidNotAttachToComposer(app)
            XCTAssertFalse(app.buttons["ai-manual-camera-shutter"].exists)
        }
        XCTAssertEqual(app.staticTexts["ai-manual-camera-tool-session"].value as? String, "switch-B")
        app.buttons["ai-manual-camera-tool-open"].tap()
        XCTAssertTrue(app.buttons["ai-manual-camera-shutter"].waitForExistence(timeout: 8))
        app.buttons["ai-manual-camera-shutter"].tap()
        assertManualToolState(app, result: "succeeded", files: 1, pending: false, verified: true)
        assertManualToolDidNotAttachToComposer(app)
        #else
        throw XCTSkip("手动工具验收仅在 Debug 模拟器使用合成画面和独立工作区")
        #endif
    }

    #if DEBUG && targetEnvironment(simulator)
    @MainActor
    private func launchManualCameraToolFixture() -> XCUIApplication {
        continueAfterFailure = false
        let app = XCUIApplication()
        app.launchArguments = ["--ai-ui-test", "--ai-session-reveal-ui-test",
            "--ai-manual-camera-tool-ui-test", "-maichat.notification-preprompt-dismissed", "YES"]
        app.launchEnvironment["MAICHAT_AI_TEST_ID"] = UUID().uuidString
        app.launch()
        let tab = app.buttons["创作中心"]
        XCTAssertTrue(tab.waitForExistence(timeout: 12))
        tab.tap()
        let open = app.buttons["ai-manual-camera-tool-open"]
        XCTAssertTrue(open.waitForExistence(timeout: 8))
        let ready = XCTNSPredicateExpectation(predicate: NSPredicate(format: "enabled == true"), object: open)
        XCTAssertEqual(XCTWaiter.wait(for: [ready], timeout: 8), .completed)
        return app
    }

    @MainActor
    private func assertManualToolState(_ app: XCUIApplication, preview: Bool = false,
                                       result: String, files: Int, pending: Bool, verified: Bool,
                                       file: StaticString = #filePath, line: UInt = #line) {
        let prefix = preview ? "ai-manual-camera-tool-preview" : "ai-manual-camera-tool"
        let state = app.staticTexts[prefix + "-state"]
        XCTAssertTrue(state.waitForExistence(timeout: 8), file: file, line: line)
        let expected = "result:\(result);files:\(files);attachments:0;request:\(pending);verified:\(verified)"
        let settled = XCTNSPredicateExpectation(predicate: NSPredicate(format: "value == %@", expected), object: state)
        XCTAssertEqual(XCTWaiter.wait(for: [settled], timeout: 8), .completed,
                       "宿主工具状态应为 \(expected)，实际 \(String(describing: state.value))", file: file, line: line)
    }

    @MainActor
    private func assertManualToolDidNotAttachToComposer(_ app: XCUIApplication,
                                                      file: StaticString = #filePath, line: UInt = #line) {
        XCTAssertTrue(app.textViews["ai-composer"].exists, file: file, line: line)
        XCTAssertFalse(app.buttons.matching(NSPredicate(format: "identifier BEGINSWITH %@",
            "ai-remove-attachment-")).firstMatch.exists, file: file, line: line)
        XCTAssertFalse(app.buttons["ai-camera-stop"].exists, file: file, line: line)
    }
    #endif

    /// 正常 Root 的 +/拍摄入口必须先出现手动快门；显式 Agent 定时结束后也不能复用定时模式。
    @MainActor
    func testManualCameraHasShutterAndDoesNotReuseTimedCapture() async throws {
        #if DEBUG && targetEnvironment(simulator)
        continueAfterFailure = false
        let app = XCUIApplication()
        app.launchArguments = ["--ai-ui-test", "--ai-session-reveal-ui-test",
            "--ai-manual-camera-ui-test", "--agent-camera-ui-test",
            "-maichat.notification-preprompt-dismissed", "YES"]
        app.launchEnvironment["MAICHAT_AI_TEST_ID"] = UUID().uuidString
        app.launch()
        defer { app.terminate() }
        let tab = app.buttons["创作中心"]
        XCTAssertTrue(tab.waitForExistence(timeout: 12))
        tab.tap()
        XCTAssertTrue(app.textViews["ai-composer"].waitForExistence(timeout: 8))

        func openManual(_ name: String) {
            app.buttons["展开更多功能"].tap()
            let entry = app.buttons["拍摄"]
            XCTAssertTrue(entry.waitForExistence(timeout: 3))
            entry.tap()
            let shutter = app.buttons["ai-manual-camera-shutter"]
            _ = shutter.waitForExistence(timeout: 4)
            let shot = XCTAttachment(screenshot: app.screenshot())
            shot.name = name; shot.lifetime = .keepAlways; add(shot)
            XCTAssertTrue(shutter.isHittable, "手动拍摄必须呈现真实手动分支，不能将 nil target 当成定时模式")
            XCTAssertFalse(app.buttons["ai-camera-stop"].exists)
            XCTAssertFalse(app.staticTexts.containing(NSPredicate(format: "label CONTAINS %@", "每 2 秒拍摄")).firstMatch.exists)
            XCTAssertEqual(app.staticTexts["ai-manual-camera-timed-state"].value as? String,
                           "running:false;request:false")
        }

        openManual("manual-camera-first-open")
        try await Task.sleep(for: .milliseconds(2200))
        XCTAssertEqual(app.staticTexts["ai-manual-camera-timed-state"].value as? String,
                       "running:false;request:false", "越过两秒后仍不能自动进入采样")
        app.buttons["ai-manual-camera-cancel"].tap()
        XCTAssertTrue(app.textViews["ai-composer"].waitForExistence(timeout: 4))
        openManual("manual-camera-reopened-after-cancel")
        app.buttons["ai-manual-camera-cancel"].tap()

        // 这一操作才显式请求定时模式，保留原 Agent 能力；帧来自已 opt-in 的合成 sampler。
        let timed = app.buttons["ai-camera-fixture-open"]
        XCTAssertTrue(timed.waitForExistence(timeout: 4)); timed.tap()
        let stop = app.buttons["ai-camera-stop"]
        XCTAssertTrue(stop.waitForExistence(timeout: 8))
        let sampled = app.staticTexts.matching(NSPredicate(format: "label MATCHES %@", ".*已拍摄 [1-9][0-9]* 张")).firstMatch
        XCTAssertTrue(sampled.waitForExistence(timeout: 8))
        stop.tap()
        let close = app.buttons["ai-screen-frames-close"]
        XCTAssertTrue(close.waitForExistence(timeout: 8)); close.tap()
        openManual("manual-camera-after-timed-stop")
        // 合成手动快门只添加一次照片到原草稿，不再次启动定时采样，也不接触真实相机。
        app.buttons["ai-manual-camera-shutter"].tap()
        let attachments = app.scrollViews["ai-pending-attachments"]
        XCTAssertTrue(attachments.waitForExistence(timeout: 8))
        let singlePhoto = XCTNSPredicateExpectation(predicate: NSPredicate(format: "count == 1"),
            object: app.buttons.matching(NSPredicate(format: "identifier BEGINSWITH %@", "ai-remove-attachment-")))
        XCTAssertEqual(XCTWaiter.wait(for: [singlePhoto], timeout: 8), .completed,
                       "手动快门必须将且仅将一张照片加到原会话草稿")
        XCTAssertTrue(app.textViews["ai-composer"].exists)
        XCTAssertFalse(app.buttons["ai-camera-stop"].exists)
        let imported = XCTAttachment(screenshot: app.screenshot())
        imported.name = "manual-camera-single-photo-in-original-draft"
        imported.lifetime = .keepAlways
        add(imported)
        #else
        throw XCTSkip("仅在 Debug 模拟器使用合成相机和独立内存会话")
        #endif
    }

    /// 本机假账号和模拟器合成帧验证 Agent 相机流程，不请求真实相机或模型服务。
    @MainActor
    func testAgentCameraAutomaticallySamplesAndAddsSelectionToOriginalDraft() async throws {
        #if DEBUG && targetEnvironment(simulator)
        continueAfterFailure = false
        let base = "http://127.0.0.1:18592"
        let (status, response) = try await URLSession.shared.data(from: URL(string: base + "/test/status")!)
        XCTAssertEqual((response as? HTTPURLResponse)?.statusCode, 200)
        let fixture = try XCTUnwrap(JSONSerialization.jsonObject(with: status) as? [String: Any])
        XCTAssertTrue((fixture["macSessions"] as? [String] ?? []).contains("ses_mock_mac"))
        let (certificate, _) = try await URLSession.shared.data(from: URL(string: base + "/test/cert")!)
        let app = XCUIApplication()
        app.launchArguments = ["--agent-camera-ui-test"]
        app.launchEnvironment["MAICHAT_LOCAL_UI_PHONE"] = "sync-ui-test"
        app.launchEnvironment["MAICHAT_LOCAL_UI_PASSWORD"] = "local sync UI test passphrase"
        app.launchEnvironment["MAICHAT_LOCAL_UI_AUTH_PORT"] = "18592"
        app.launchEnvironment["MAICHAT_LOCAL_UI_SOCKET_PORT"] = "18593"
        app.launchEnvironment["MAICHAT_LOCAL_UI_MODEL_PORT"] = "18594"
        app.launchEnvironment["MAICHAT_LOCAL_UI_CA_BASE64"] = certificate.base64EncodedString()
        app.launch()
        defer { app.terminate() }
        let assistant = app.buttons["创作中心"]
        XCTAssertTrue(assistant.waitForExistence(timeout: 35))
        if app.buttons["稍后"].exists { app.buttons["稍后"].tap() }
        assistant.tap()
        let race = app.buttons["ai-camera-fixture-race"]
        XCTAssertTrue(race.waitForExistence(timeout: 35))
        race.tap()
        let raceResult = app.staticTexts["ai-camera-fixture-race-result"]
        XCTAssertTrue(raceResult.waitForExistence(timeout: 5))
        XCTAssertEqual(raceResult.label, "相机请求代次通过")
        let open = app.buttons["ai-camera-fixture-open"]
        XCTAssertTrue(open.waitForExistence(timeout: 35))
        open.tap()
        let stop = app.buttons["ai-camera-stop"]
        XCTAssertTrue(stop.waitForExistence(timeout: 15))
        let threeFrames = app.staticTexts.matching(NSPredicate(
            format: "label MATCHES %@", ".*已拍摄 ([3-9]|[1-9][0-9]+) 张"
        )).firstMatch
        XCTAssertTrue(threeFrames.waitForExistence(timeout: 15), "两秒采样应连续保存至少三张图片")
        let recording = XCTAttachment(screenshot: app.screenshot())
        recording.name = "agent-camera-three-frames"
        recording.lifetime = .keepAlways
        add(recording)
        stop.tap()
        XCTAssertTrue(app.buttons["ai-screen-frame-select-1"].waitForExistence(timeout: 15))
        let grid = XCTAttachment(screenshot: app.screenshot())
        grid.name = "agent-camera-stopped-grid"
        grid.lifetime = .keepAlways
        add(grid)
        app.buttons["ai-screen-frame-select-0"].tap()
        app.buttons["ai-screen-frames-add"].tap()
        XCTAssertTrue(app.scrollViews["ai-pending-attachments"].waitForExistence(timeout: 15),
                      "选择图片后应回到原会话草稿")
        let archivedAttachment = app.buttons.matching(NSPredicate(format: "identifier BEGINSWITH %@",
            "ai-remove-attachment-.maichat/screen-frame-archive/")).firstMatch
        XCTAssertTrue(archivedAttachment.exists, "草稿应引用归档原图")
        XCTAssertTrue(open.exists)
        #else
        throw XCTSkip("相机合成帧验收只在 Debug 模拟器运行")
        #endif
    }

    @MainActor
    func testRealSamplerSelectionUsesOriginalFramesAndCleanupKeepsReferences() throws {
        #if targetEnvironment(simulator)
        continueAfterFailure = false
        let app = XCUIApplication()
        app.launchArguments = ["--ai-ui-test", "--ai-screen-frames-ui-test"]
        app.launch()
        defer { app.terminate() }
        let prepare = app.buttons["screen-frames-fixture-prepare"]
        XCTAssertTrue(prepare.waitForExistence(timeout: 15))
        prepare.tap()
        XCTAssertTrue(app.staticTexts["采样已准备"].waitForExistence(timeout: 60))
        app.buttons["screen-frames-fixture-account"].tap()
        XCTAssertTrue(app.staticTexts["账号隔离通过"].waitForExistence(timeout: 15))

        app.buttons["screen-frames-fixture-open"].tap()
        XCTAssertTrue(app.staticTexts["ai-screen-frames-summary"].waitForExistence(timeout: 15))
        app.buttons["ai-screen-frame-preview-0"].tap()
        XCTAssertTrue(app.images["ai-screen-frame-large-image"].waitForExistence(timeout: 15),
                      "大图必须经过真实PNG验证和解码")
        let screen = app.frame
        let portrait = app.images["ai-screen-frame-large-image"].frame
        XCTAssertEqual(portrait.minX, screen.minX, accuracy: 1.5)
        XCTAssertEqual(portrait.maxX, screen.maxX, accuracy: 1.5)
        XCTAssertEqual(portrait.minY, screen.minY, accuracy: 1.5)
        XCTAssertEqual(portrait.maxY, screen.maxY, accuracy: 1.5,
                       "屏幕同纵横比截图应适配完整viewport，不缩进safeArea形成四边黑框")
        XCTAssertFalse(app.statusBars.firstMatch.exists, "原图已有状态栏，预览不应叠加第二个系统状态栏")
        XCTAssertTrue(app.buttons["ai-screen-frame-large-close"].isHittable)
        let fullImage = XCTAttachment(screenshot: app.screenshot())
        fullImage.name = "screen-archive-decoded-preview"
        fullImage.lifetime = .keepAlways
        add(fullImage)
        app.buttons["ai-screen-frame-large-close"].tap()
        app.buttons["ai-screen-frame-preview-2"].tap()
        XCTAssertTrue(app.images["ai-screen-frame-large-image"].waitForExistence(timeout: 15))
        let landscape = app.images["ai-screen-frame-large-image"].frame
        XCTAssertEqual(landscape.width, screen.width, accuracy: 1.5)
        XCTAssertEqual(landscape.width / landscape.height, 320.0 / 180.0, accuracy: 0.02,
                       "横图保留完整比例，不能裁图或拉伸以填满竖屏")
        let landscapeImage = XCTAttachment(screenshot: app.screenshot())
        landscapeImage.name = "screen-archive-landscape-fit-preview"
        landscapeImage.lifetime = .keepAlways
        add(landscapeImage)
        app.buttons["ai-screen-frame-large-close"].tap()
        app.buttons["ai-screen-frame-select-0"].tap()
        app.buttons["ai-screen-frames-actions"].tap()
        XCTAssertTrue(app.buttons["清理未选图片"].exists)
        XCTAssertFalse(app.buttons["清理未选图片"].isEnabled,
                       "尚未加入草稿的勾选图片不能被清理操作悄悄删除")
        app.scrollViews["ai-screen-frames-grid"]
            .coordinate(withNormalizedOffset: CGVector(dx: 0.05, dy: 0.9)).tap()
        app.buttons["ai-screen-frames-close"].tap()
        XCTAssertTrue(app.buttons["screen-frames-fixture-open-from-b"].waitForExistence(timeout: 10))

        // 关闭选择界面不丢档案，且从B打开仍只会写启动时的A草稿。
        app.buttons["screen-frames-fixture-open-from-b"].tap()
        XCTAssertTrue(app.buttons["ai-screen-frame-select-1"].waitForExistence(timeout: 15))
        app.buttons["ai-screen-frame-select-0"].tap()
        app.buttons["ai-screen-frame-select-1"].tap()
        XCTAssertEqual(app.staticTexts["ai-screen-frames-selection-count"].label, "已选 2 张")
        let grid = XCTAttachment(screenshot: app.screenshot())
        grid.name = "screen-archive-multiple-selection"
        grid.lifetime = .keepAlways
        add(grid)
        app.buttons["ai-screen-frames-add"].tap()
        XCTAssertTrue(app.staticTexts["A=2 B=0"].waitForExistence(timeout: 15))

        app.buttons["screen-frames-fixture-open"].tap()
        XCTAssertTrue(app.buttons["ai-screen-frames-actions"].waitForExistence(timeout: 10))
        app.buttons["ai-screen-frames-actions"].tap()
        app.buttons["清理未选图片"].tap()
        XCTAssertTrue(app.buttons["screen-frames-fixture-verify"].waitForExistence(timeout: 10))
        app.buttons["screen-frames-fixture-verify"].tap()
        XCTAssertTrue(app.staticTexts["PASS: 原图无重复、独立引用安全、未选清理及会话回收通过"]
            .waitForExistence(timeout: 15))
        #else
        throw XCTSkip("仅使用隔离模拟器和生成的安全画面，不操作真机")
        #endif
    }
}
