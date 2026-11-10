import XCTest
import Foundation
@preconcurrency import Network

/// Owned HTTP/1.1 fixtures use real native Chat/Responses requests. No provider account is used.
private final class SessionModelHTTPFixture: @unchecked Sendable {
    private let listener: NWListener
    private let queue = DispatchQueue(label: "maichat.session-model-http")
    private let lock = NSLock()
    private var captured: [[String: Any]] = []
    private(set) var port: UInt16 = 0
    private let glm: Bool

    init(glm: Bool) throws {
        self.glm = glm
        listener = try NWListener(using: .tcp, on: .any)
        let ready = DispatchSemaphore(value: 0)
        listener.stateUpdateHandler = { [weak self] state in
            if case .ready = state, let self { self.port = self.listener.port?.rawValue ?? 0; ready.signal() }
            if case .failed = state { ready.signal() }
        }
        listener.newConnectionHandler = { [weak self] connection in
            guard let self else { connection.cancel(); return }
            connection.start(queue: self.queue)
            self.receive(connection, buffer: Data())
        }
        listener.start(queue: queue)
        guard ready.wait(timeout: .now() + 5) == .success, port != 0 else {
            throw NSError(domain: "OwnedHTTPFixture", code: 1)
        }
    }
    deinit { listener.cancel() }

    private func receive(_ connection: NWConnection, buffer: Data) {
        connection.receive(minimumIncompleteLength: 1, maximumLength: 128 * 1024) { [weak self] data, _, complete, error in
            guard let self else { connection.cancel(); return }
            var buffer = buffer
            if let data { buffer.append(data) }
            guard buffer.count <= 2 * 1024 * 1024 else { connection.cancel(); return }
            if let range = buffer.range(of: Data("\r\n\r\n".utf8)),
               let header = String(data: buffer[..<range.lowerBound], encoding: .utf8) {
                let length = header.components(separatedBy: "\r\n").first(where: {
                    $0.lowercased().hasPrefix("content-length:")
                }).flatMap { Int($0.split(separator: ":", maxSplits: 1)[1].trimmingCharacters(in: .whitespaces)) } ?? 0
                let body = buffer[range.upperBound...]
                if body.count >= length {
                    self.respond(connection, header: header, body: Data(body.prefix(length)))
                    return
                }
            }
            if complete || error != nil { connection.cancel(); return }
            self.receive(connection, buffer: buffer)
        }
    }

    private func respond(_ connection: NWConnection, header: String, body: Data) {
        let object = (try? JSONSerialization.jsonObject(with: body)) as? [String: Any] ?? [:]
        lock.lock(); captured.append(object); lock.unlock()
        let expected = glm ? "/chat/completions" : "/responses"
        let key = glm ? "synthetic-session-glm-key" : "synthetic-session-deepseek-key"
        let authorized = header.contains("POST " + expected + " ") && header.lowercased().contains("authorization: bearer " + key)
        let reply: String
        if glm {
            reply = "data: {\"choices\":[{\"delta\":{\"content\":\"Fixture GLM reply\"},\"finish_reason\":\"stop\"}]}\n\ndata: [DONE]\n\n"
        } else {
            reply = "data: {\"type\":\"response.output_text.delta\",\"output_index\":0,\"delta\":\"Fixture DeepSeek reply\"}\n\ndata: {\"type\":\"response.completed\",\"response\":{\"status\":\"completed\",\"output\":[]}}\n\n"
        }
        let bytes = Data((authorized ? reply : "{}").utf8)
        let encoded = Data("HTTP/1.1 \(authorized ? "200 OK" : "401 Unauthorized")\r\nContent-Type: text/event-stream\r\nContent-Length: \(bytes.count)\r\nConnection: close\r\n\r\n".utf8) + bytes
        connection.send(content: encoded, completion: .contentProcessed { _ in connection.cancel() })
    }
    func requests() -> [[String: Any]] { lock.lock(); defer { lock.unlock() }; return captured }
}

final class SessionModelsUITests: XCTestCase {
    @MainActor
    func testRemoteOwnerConfigurationAndScopedReceipts() throws {
        continueAfterFailure = false
        let glm = try SessionModelHTTPFixture(glm: true)
        let deepseek = try SessionModelHTTPFixture(glm: false)
        XCUIDevice.shared.orientation = .portrait
        let app = XCUIApplication()
        app.launchArguments = ["--ai-ui-test", "--ai-session-reveal-ui-test", "--ai-remote-session-model-ui-test", "--ai-session-model-ui-test",
                               "-maichat.notification-preprompt-dismissed", "YES"]
        app.launchEnvironment["MAICHAT_AI_TEST_ID"] = UUID().uuidString
        app.launchEnvironment["MAICHAT_SESSION_MODELS_GLM_PORT"] = String(glm.port)
        app.launchEnvironment["MAICHAT_SESSION_MODELS_DEEPSEEK_PORT"] = String(deepseek.port)
        app.launch(); defer { app.terminate() }
        let result = app.staticTexts["remote-session-model-checks"]
        XCTAssertTrue(result.waitForExistence(timeout: 30))
        XCTAssertEqual(XCTWaiter.wait(for: [XCTNSPredicateExpectation(predicate: NSPredicate(format: "value BEGINSWITH %@", "PASS"), object: result)], timeout: 15), .completed,
                       "Actual protocol checks: \(result.value ?? "missing")")
        let capture = XCTAttachment(screenshot: app.screenshot()); capture.name = "remote-owner-configuration"; capture.lifetime = .keepAlways; add(capture)
    }

    @MainActor
    func testModelMenuFitsLandscape() throws {
        continueAfterFailure = false
        let glm = try SessionModelHTTPFixture(glm: true)
        let deepseek = try SessionModelHTTPFixture(glm: false)
        XCUIDevice.shared.orientation = .portrait
        let app = XCUIApplication()
        app.launchArguments = ["--ai-ui-test", "--ai-session-reveal-ui-test", "--ai-session-model-ui-test",
                               "-maichat.notification-preprompt-dismissed", "YES"]
        app.launchEnvironment["MAICHAT_AI_TEST_ID"] = UUID().uuidString
        app.launchEnvironment["MAICHAT_SESSION_MODELS_GLM_PORT"] = String(glm.port)
        app.launchEnvironment["MAICHAT_SESSION_MODELS_DEEPSEEK_PORT"] = String(deepseek.port)
        app.launch(); defer { XCUIDevice.shared.orientation = .portrait; app.terminate() }
        XCTAssertTrue(app.textViews["ai-composer"].waitForExistence(timeout: 30))
        let configured = app.staticTexts["primary-ui-session-configuration"]
        XCTAssertEqual(XCTWaiter.wait(for: [XCTNSPredicateExpectation(predicate: NSPredicate(format: "value CONTAINS %@", "model:deepseek-flash;"), object: configured)], timeout: 15), .completed)
        XCUIDevice.shared.orientation = .landscapeRight
        XCTAssertEqual(XCTWaiter.wait(for: [XCTNSPredicateExpectation(predicate: NSPredicate { _, _ in app.frame.width > app.frame.height }, object: app)], timeout: 10), .completed)
        let more = app.buttons["更多"]
        XCTAssertTrue(more.waitForExistence(timeout: 10)); more.tap()
        let settings = app.buttons["ai-primary-model-settings"]
        XCTAssertTrue(settings.waitForExistence(timeout: 5)); settings.tap()
        let panel = app.descendants(matching: .any)["ai-action-panel"]
        XCTAssertTrue(panel.waitForExistence(timeout: 5))
        XCTAssertLessThanOrEqual(panel.frame.maxY, app.frame.maxY + 1)
        XCTAssertGreaterThanOrEqual(panel.frame.minY + 1, app.frame.minY)
        let local = app.buttons["ai-primary-model-local"]
        if !local.isHittable { panel.swipeUp() }
        XCTAssertTrue(local.isHittable, "Last model must be reachable by scrolling inside the viewport")
        let capture = XCTAttachment(screenshot: XCUIScreen.main.screenshot()); capture.name = "landscape-model-menu-scroll-last-option"; capture.lifetime = .keepAlways; add(capture)
        let clear = app.buttons["清空当前对话"]
        if !clear.isHittable { panel.swipeUp() }
        XCTAssertTrue(clear.isHittable, "Bottom action must be reachable inside the panel")
        XCTAssertLessThanOrEqual(clear.frame.maxY, app.frame.maxY + 1)
        let bottom = XCTAttachment(screenshot: XCUIScreen.main.screenshot()); bottom.name = "landscape-model-menu-bottom"; bottom.lifetime = .keepAlways; add(bottom)
        clear.tap()
        XCTAssertTrue(app.buttons["清空消息"].waitForExistence(timeout: 5), "Bottom action did not receive the actual tap")
        let confirmation = XCTAttachment(screenshot: XCUIScreen.main.screenshot())
        confirmation.name = "landscape-bottom-action-received-confirmation"
        confirmation.lifetime = .keepAlways; add(confirmation)
        // Do not execute the destructive action; the owned app terminates below.
    }

    @MainActor
    func testModelsAndEffortBelongToSessionsAndSurviveRelaunch() throws {
        continueAfterFailure = false
        let glm = try SessionModelHTTPFixture(glm: true)
        let deepseek = try SessionModelHTTPFixture(glm: false)
        XCUIDevice.shared.orientation = .portrait
        let app = XCUIApplication()
        app.launchArguments = ["--ai-ui-test", "--ai-session-reveal-ui-test", "--ai-session-model-ui-test",
                               "-maichat.notification-preprompt-dismissed", "YES"]
        app.launchEnvironment["MAICHAT_AI_TEST_ID"] = UUID().uuidString
        app.launchEnvironment["NO_PROXY"] = "*"
        app.launchEnvironment["no_proxy"] = "*"
        app.launchEnvironment["MAICHAT_SESSION_MODELS_GLM_PORT"] = String(glm.port)
        app.launchEnvironment["MAICHAT_SESSION_MODELS_DEEPSEEK_PORT"] = String(deepseek.port)
        app.launch(); defer { app.terminate() }
        XCUIDevice.shared.orientation = .portrait
        XCTAssertEqual(XCTWaiter.wait(for: [XCTNSPredicateExpectation(predicate: NSPredicate { _, _ in app.frame.height > app.frame.width }, object: app)], timeout: 10), .completed)
        let config = app.staticTexts["primary-ui-session-configuration"]
        XCTAssertTrue(config.waitForExistence(timeout: 30))
        let composer = app.textViews["ai-composer"]
        XCTAssertTrue(composer.waitForExistence(timeout: 20))
        func require(_ model: String, _ effort: String) {
            XCTAssertEqual(XCTWaiter.wait(for: [XCTNSPredicateExpectation(
                predicate: NSPredicate(format: "value CONTAINS %@ AND value CONTAINS %@", "model:\(model);", "effort:\(effort);"), object: config)], timeout: 12), .completed)
        }
        func sessionID() throws -> String {
            let value = try XCTUnwrap(config.value as? String)
            return try XCTUnwrap(value.split(separator: ";").first(where: { $0.hasPrefix("session:") })).dropFirst(8).description
        }
        func openPanel() {
            if !app.descendants(matching: .any)["ai-action-panel"].exists { app.buttons["更多"].tap() }
        }
        func chooseModel(_ model: String) {
            openPanel()
            let item = app.buttons["ai-primary-model-" + model]
            if !item.exists { app.buttons["ai-primary-model-settings"].tap() }
            XCTAssertTrue(item.waitForExistence(timeout: 5)); XCTAssertTrue(item.isHittable); item.tap()
        }
        func chooseEffort(_ label: String) {
            openPanel(); app.buttons["思考强度"].tap()
            let button = app.buttons[label]
            XCTAssertTrue(button.waitForExistence(timeout: 5)); button.tap()
        }
        func closePanel() {
            let panel = app.descendants(matching: .any)["ai-action-panel"]
            if panel.exists {
                app.coordinate(withNormalizedOffset: .zero).withOffset(CGVector(dx: 8, dy: panel.frame.midY)).tap()
            }
        }
        func selectSession(_ id: String) {
            closePanel(); app.buttons["对话列表"].tap()
            let row = app.buttons["ai-conversation-" + id]
            XCTAssertTrue(row.waitForExistence(timeout: 8)); row.tap()
        }
        require("deepseek-flash", "high")
        composer.tap(); composer.typeText("draft-A")
        let a = try sessionID()
        chooseModel("glm-5.3"); chooseEffort("简单"); require("glm-5.3", "low")
        closePanel(); XCTAssertEqual(composer.value as? String, "draft-A")
        let selected = XCTAttachment(screenshot: app.screenshot()); selected.name = "session-A-glm-low-with-draft"; selected.lifetime = .keepAlways; add(selected)
        app.buttons["对话列表"].tap()
        // safeAreaInset header inherits the drawer identifier on this supported runtime.
        // Distinguish it from the real untitled conversation row; both are labelled New conversation.
        let create = app.buttons.matching(identifier: "ai-session-drawer")
            .matching(NSPredicate(format: "label == %@", "新对话")).firstMatch
        XCTAssertTrue(create.waitForExistence(timeout: 5))
        XCTAssertEqual(XCTWaiter.wait(for: [XCTNSPredicateExpectation(predicate: NSPredicate(format: "enabled == true"), object: create)], timeout: 5), .completed)
        create.tap()
        XCTAssertEqual(XCTWaiter.wait(for: [XCTNSPredicateExpectation(predicate: NSPredicate(format: "NOT (value CONTAINS %@)", "session:\(a);"), object: config)], timeout: 10), .completed)
        let b = try sessionID()
        require("deepseek-flash", "high")
        chooseModel("deepseek-v4-pro"); chooseEffort("深度"); require("deepseek-v4-pro", "max")
        closePanel(); composer.tap(); composer.typeText("Reply in session B\n")
        XCTAssertEqual(XCTWaiter.wait(for: [XCTNSPredicateExpectation(predicate: NSPredicate(format: "value CONTAINS %@", "Fixture DeepSeek reply"), object: app.staticTexts["primary-ui-model-output"])], timeout: 15), .completed)
        selectSession(a); require("glm-5.3", "low")
        XCTAssertEqual(composer.value as? String, "draft-A")
        composer.tap(); composer.typeText("\n")
        XCTAssertEqual(XCTWaiter.wait(for: [XCTNSPredicateExpectation(predicate: NSPredicate(format: "value CONTAINS %@", "Fixture GLM reply"), object: app.staticTexts["primary-ui-model-output"])], timeout: 15), .completed)
        chooseModel("glm-5.3-flash"); chooseEffort("标准"); require("glm-5.3-flash", "high")
        closePanel(); selectSession(b); require("deepseek-v4-pro", "max")
        let separate = XCTAttachment(screenshot: app.screenshot()); separate.name = "session-B-pro-max-isolated"; separate.lifetime = .keepAlways; add(separate)
        app.terminate(); app.launch()
        XCTAssertTrue(config.waitForExistence(timeout: 30))
        selectSession(a); require("glm-5.3-flash", "high")
        selectSession(b); require("deepseek-v4-pro", "max")
        XCTAssertTrue(glm.requests().contains { $0["model"] as? String == "glm-5.3" && $0["reasoning_effort"] as? String == "low" })
        XCTAssertTrue(deepseek.requests().contains { $0["model"] as? String == "deepseek-v4-pro" && ($0["reasoning"] as? [String: Any])?["effort"] as? String == "max" })
    }
}
