import XCTest

final class LocalInferenceUITests: XCTestCase {
    @MainActor func testOrdinaryLocalInferenceMissingModelKeepsRuntimeAndAppAlive() throws {
        continueAfterFailure = false
        XCUIDevice.shared.orientation = .portrait
        let app = XCUIApplication()
        app.launchArguments = ["--ai-ui-test", "--ai-local-inference-ui-test"]
        // 即使本轮 runner 同时 opt in 正向模型测试，此例也必须保留无权重错误路径。
        app.launchEnvironment["MAICHAT_LOCAL_INFERENCE_TEXT_MODEL_RELATIVE"] = ""
        app.launch()
        defer { app.terminate() }
        dismissNotificationOffer(app)
        let output = app.staticTexts["local-inference-result"]
        XCTAssertTrue(output.waitForExistence(timeout: 15))
        let completed = XCTNSPredicateExpectation(
            predicate: NSPredicate(format: "value CONTAINS %@", "\"status\":\"passed\""), object: output)
        XCTAssertEqual(XCTWaiter.wait(for: [completed], timeout: 15), .completed)
        try assertReceipt(output, attempt: 1)
        let recheck = app.buttons["local-inference-recheck"]
        XCTAssertTrue(recheck.isEnabled)
        recheck.tap()
        let repeated = XCTNSPredicateExpectation(
            predicate: NSPredicate(format: "value CONTAINS %@", "\"attempt\":2"), object: output)
        XCTAssertEqual(XCTWaiter.wait(for: [repeated], timeout: 10), .completed)
        try assertReceipt(output, attempt: 2)
        let screenshot = XCTAttachment(screenshot: app.screenshot())
        screenshot.name = "ios-linked-local-inference-missing-model-alive"
        screenshot.lifetime = .keepAlways
        add(screenshot)
    }

    @MainActor func testOrdinaryLocalTextInferenceReusesResidentModel() throws {
        let key = "MAICHAT_LOCAL_INFERENCE_TEXT_MODEL_RELATIVE"
        let allowed = "qa-local-inference/tiny.gguf"
        guard ProcessInfo.processInfo.environment[key] == allowed else {
            throw XCTSkip("Explicit isolated Documents TinyStories fixture is not enabled")
        }
        continueAfterFailure = false
        XCUIDevice.shared.orientation = .portrait
        let app = XCUIApplication()
        app.launchArguments = ["--ai-ui-test", "--ai-local-inference-ui-test"]
        app.launchEnvironment[key] = allowed
        app.launch()
        defer { app.terminate() }
        dismissNotificationOffer(app)
        let output = app.staticTexts["local-inference-result"]
        XCTAssertTrue(output.waitForExistence(timeout: 15))
        let completed = XCTNSPredicateExpectation(
            predicate: NSPredicate(format: "value CONTAINS %@", "\"status\":\"passed\""), object: output)
        XCTAssertEqual(XCTWaiter.wait(for: [completed], timeout: 30), .completed)
        let raw = try XCTUnwrap(output.value as? String)
        let data = try XCTUnwrap(raw.data(using: .utf8))
        let result = try XCTUnwrap(JSONSerialization.jsonObject(with: data) as? [String: Any])
        XCTAssertEqual(result["mode"] as? String, "text")
        XCTAssertEqual(result["ordinary_api"] as? Bool, true)
        XCTAssertEqual(result["generation_count"] as? Int, 2)
        for name in ["first_prompt_tokens", "first_generated_tokens", "second_prompt_tokens", "second_generated_tokens"] {
            XCTAssertGreaterThan(try XCTUnwrap(result[name] as? Int), 0)
        }
        XCTAssertEqual(result["first_text_nonempty"] as? Bool, true)
        XCTAssertEqual(result["second_text_nonempty"] as? Bool, true)
        XCTAssertEqual(result["busy"] as? Bool, false)
        XCTAssertEqual(result["has_model"] as? Bool, true)
        XCTAssertEqual(result["has_projector"] as? Bool, false)
        XCTAssertEqual(result["context_size"] as? Int, 256)
        XCTAssertEqual(result["resident_generation_unchanged"] as? Bool, true)
        XCTAssertGreaterThan(try XCTUnwrap(result["model_generation"] as? Int), 0)
        XCTAssertEqual(result["unloaded"] as? Bool, true)
        XCTAssertEqual(result["has_model_after_unload"] as? Bool, false)
        XCTAssertEqual(result["context_size_after_unload"] as? Int, 0)
        XCTAssertEqual(result["generation_unchanged_after_unload"] as? Bool, true)
        XCTAssertFalse(raw.contains("tiny.gguf"), "Receipt must not contain file paths or generated content")
        XCTAssertTrue(app.buttons["local-inference-recheck"].isEnabled)
        let screenshot = XCTAttachment(screenshot: app.screenshot())
        screenshot.name = "ios-linked-local-text-inference-resident-model"
        screenshot.lifetime = .keepAlways
        add(screenshot)
    }

    // 首次正常 Root 会展示应用内通知引导；选择稍后以观察真实页面，不授予系统通知权限。
    @MainActor private func dismissNotificationOffer(_ app: XCUIApplication) {
        let later = app.buttons["稍后"]
        if later.waitForExistence(timeout: 2) { later.tap() }
    }

    @MainActor private func assertReceipt(_ element: XCUIElement, attempt: Int) throws {
        let raw = try XCTUnwrap(element.value as? String)
        let data = try XCTUnwrap(raw.data(using: .utf8))
        let result = try XCTUnwrap(JSONSerialization.jsonObject(with: data) as? [String: Any])
        XCTAssertEqual(result["status"] as? String, "passed")
        XCTAssertEqual(result["attempt"] as? Int, attempt)
        XCTAssertEqual(result["not_found_count"] as? Int, 2)
        XCTAssertEqual(result["ordinary_api"] as? Bool, true)
        XCTAssertEqual(result["status_after_failure"] as? Bool, true)
        XCTAssertEqual(result["busy"] as? Bool, false)
        XCTAssertEqual(result["has_model"] as? Bool, false)
        XCTAssertEqual(result["has_projector"] as? Bool, false)
        XCTAssertEqual(result["generation_unchanged"] as? Bool, true)
        XCTAssertFalse(raw.contains("model.gguf"), "QA receipt must not contain paths or prompt text")
    }
}
