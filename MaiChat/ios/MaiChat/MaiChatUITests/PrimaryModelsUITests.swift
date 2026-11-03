import XCTest

final class PrimaryModelsUITests: XCTestCase {
    /// 只给正常导入按钮提供公共 GGUF 文件；两轮回复和重启恢复均来自实际 native Agent。
    @MainActor
    func testImportedLocalPrimaryModelChatsTwiceAndRestoresAfterRelaunch() throws {
        #if DEBUG && targetEnvironment(simulator)
        continueAfterFailure = false
        guard let source = ProcessInfo.processInfo.environment["MAICHAT_PRIMARY_MODEL_FIXTURE_PATH"],
              FileManager.default.fileExists(atPath: source) else {
            throw XCTSkip("需显式提供已校验的本地 Qwen GGUF；没有权重时不伪造推理通过")
        }
        let app = XCUIApplication()
        app.launchArguments = ["--ai-ui-test", "--ai-session-reveal-ui-test", "--ai-primary-model-ui-test",
            "-maichat.notification-preprompt-dismissed", "YES"]
        app.launchEnvironment["MAICHAT_AI_TEST_ID"] = UUID().uuidString
        app.launchEnvironment["MAICHAT_PRIMARY_MODEL_FIXTURE_PATH"] = source
        app.launch()
        defer { app.terminate() }
        let state = app.staticTexts["primary-ui-model-state"]
        XCTAssertTrue(state.waitForExistence(timeout: 20))
        XCTAssertTrue(app.textViews["ai-composer"].waitForExistence(timeout: 20))
        app.buttons["更多"].tap()
        let settings = app.buttons["ai-primary-model-settings"]
        XCTAssertTrue(settings.waitForExistence(timeout: 4)); settings.tap()
        let local = app.buttons["本地模型"]
        XCTAssertTrue(local.waitForExistence(timeout: 5)); local.tap()
        let management = app.buttons["primary-model-management"]
        XCTAssertTrue(management.waitForExistence(timeout: 5)); management.tap()
        let form = app.collectionViews.allElementsBoundByIndex.first(where: { $0.isHittable })
            ?? app.collectionViews.firstMatch
        func reveal(_ element: XCUIElement) {
            for _ in 0..<8 where !element.isHittable { form.swipeUp() }
            XCTAssertTrue(element.isHittable)
        }
        let importButton = app.buttons["primary-local-model-import"]
        reveal(importButton); importButton.tap()
        let imported = app.staticTexts["primary-model-notice"]
        for _ in 0..<4 where !imported.isHittable { form.swipeUp() }
        XCTAssertTrue(imported.waitForExistence(timeout: 60))
        XCTAssertEqual(imported.label, "模型已导入，点击“使用此模型”后生效。")

        let apply = app.buttons["primary-model-apply"]
        reveal(apply); XCTAssertTrue(apply.isEnabled); apply.tap()
        let selected = XCTNSPredicateExpectation(predicate: NSPredicate(format:
            "value CONTAINS %@ AND value CONTAINS %@ AND value CONTAINS %@", "provider:llama;", "configured:true;", ";native-wire:local;"), object: state)
        XCTAssertEqual(XCTWaiter.wait(for: [selected], timeout: 30), .completed)
        let supportsGPU = (state.value as? String ?? "").contains("qwen-gpu-ops-supported:true")
        // 真实 Sim archive/probe 已确认 Apple2 设备拒绝 Q4_K matmul 与 F16 FA，
        // 完整 119 工具首字 CPU 实测 190.70 秒。这里明确验证 CPU 功能，不能虚报 GPU。
        // 真机相同 client 的 GPU/耗时另由正常 App 隔离设备探针测量；不更改产品超时。
        let replyTimeout: TimeInterval = supportsGPU ? 180 : 360
        let capability = XCTAttachment(string: supportsGPU
            ? "Device supports required Qwen GPU operations; require actual GPU buffers."
            : "Simulator lacks required SIMD operations; validate actual CPU replies. Baseline: 190.70s first token for 119 tools. Physical GPU is validated separately.")
        capability.name = "actual-qwen-compute-capability"
        capability.lifetime = .keepAlways
        add(capability)

        func send(_ text: String, atLeast count: Int) {
            let composer = app.textViews["ai-composer"]
            XCTAssertTrue(composer.waitForExistence(timeout: 10)); composer.tap(); composer.typeText(text)
            app.buttons["ai-send-draft"].tap()
            let countPattern = count == 1 ? "[1-9][0-9]*" : "(?:[2-9]|[1-9][0-9]+)"
            let gpu = supportsGPU
                ? "gpu-selected:true;gpu-bytes:[1-9][0-9]*;gpu-backend:[^;]+;"
                : "gpu-selected:(?:true|false);gpu-bytes:[0-9]+;gpu-backend:[^;]*;"
            let reply = XCTNSPredicateExpectation(predicate: NSPredicate(format: "value MATCHES %@",
                ".*provider:llama;configured:true;busy:false;assistant-text-count:" + countPattern + ";session:[^;]+;native-wire:local;native-model:local:[^;]+;" + gpu + "error-empty:true;qwen-gpu-ops-supported:(?:true|false)"), object: state)
            XCTAssertEqual(XCTWaiter.wait(for: [reply], timeout: replyTimeout), .completed,
                           "真实本地主模型必须生成非空、无错误的回复并完成轮次；实际状态：\(state.value ?? "missing")")
            XCTAssertTrue(app.buttons["复制回复"].firstMatch.exists, "回复应实际显示在聊天消息区")
        }
        send("Reply with one short sentence: Hello from Qwen.", atLeast: 1)
        send("What exact phrase did you just say? Reply briefly.", atLeast: 2)
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
        let current = app.staticTexts["primary-model-current"]
        XCTAssertTrue(current.waitForExistence(timeout: 8))
        XCTAssertEqual(current.label, "当前：本地模型")
        let persisted = XCTAttachment(screenshot: app.screenshot())
        persisted.name = "local-primary-selection-restored"; persisted.lifetime = .keepAlways; add(persisted)
        #else
        throw XCTSkip("仅在 Debug 模拟器使用显式公共 GGUF 和真实本地后端")
        #endif
    }
}
