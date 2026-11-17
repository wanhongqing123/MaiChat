import XCTest
import UIKit

final class AccountToolsUITests: XCTestCase {
    @MainActor func testAgentFileCancellationAfterCopyCleansUnsubmittedFile() {
        checkOwnedFileCancellation(point: "copy")
    }
    @MainActor func testAgentFileCancellationBeforeSDKCleansUnsubmittedFile() {
        checkOwnedFileCancellation(point: "prepared")
    }
    @MainActor private func checkOwnedFileCancellation(point: String) {
        let app = XCUIApplication()
        app.launchArguments = ["--ai-ui-test", "--ai-account-tools-ui-test", "--im-group-ui-test"]
        app.launchEnvironment["MAICHAT_ACCOUNT_TOOLS_CASE"] = "file-send"
        app.launchEnvironment["MAICHAT_FILE_CANCEL_POINT"] = point
        app.launch()
        defer { app.terminate() }
        let status = app.staticTexts["account-tools-test-status"]
        XCTAssertTrue(status.waitForExistence(timeout: 20))
        let finished = XCTNSPredicateExpectation(predicate: NSPredicate(
            format: "label BEGINSWITH 'PASS FILE CANCEL' OR label BEGINSWITH 'FAIL FILE CANCEL'"), object: status)
        XCTAssertEqual(XCTWaiter.wait(for: [finished], timeout: 20), .completed)
        XCTAssertTrue(status.label.hasPrefix("PASS FILE CANCEL"), status.label)
    }

    @MainActor func testCurrentAIFileCardsCanPreviewPDF() {
        let app = XCUIApplication()
        app.launchArguments = ["--ai-ui-test", "--ai-file-artifact-ui-test"]
        app.launch()
        defer { app.terminate() }
        let pdf = app.buttons["agent-file-artifact-owned-file-pdf-part"]
        let archive = app.buttons["agent-file-artifact-owned-file-zip-part"]
        XCTAssertTrue(pdf.waitForExistence(timeout: 20))
        XCTAssertTrue(archive.exists)
        let pdfThumbnail = app.images["agent-document-thumbnail-owned-file-pdf-part"]
        let mdThumbnail = app.images["agent-document-thumbnail-owned-file-md-part"]
        XCTAssertTrue(pdfThumbnail.waitForExistence(timeout: 10), "PDF卡片需要真实第一页缩略图")
        XCTAssertTrue(mdThumbnail.waitForExistence(timeout: 10), "Markdown卡片需要真实正文缩略图")
        // 普通历史区默认跟随末尾；滚到第一页完整可见，避免只证明树中存在离屏图。
        let history = app.scrollViews.containing(.button,
            identifier: "agent-file-artifact-owned-file-pdf-part").firstMatch
        for _ in 0..<4 where pdfThumbnail.frame.minY < 130 { history.swipeDown() }
        XCTAssertGreaterThan(pdfThumbnail.frame.minY, 120)
        XCTAssertGreaterThan(documentPixelCount(pdfThumbnail.screenshot().image, red: true), 200,
                             "PDF第一页的红色页眉必须实际绘制到卡片")
        let cardImage = XCTAttachment(screenshot: app.screenshot())
        cardImage.name = "owned-current-AI-file-cards"
        cardImage.lifetime = .keepAlways
        add(cardImage)
        pdf.tap()
        XCTAssertTrue(app.buttons["ai-file-preview-close"].waitForExistence(timeout: 10))
        let previewImage = XCTAttachment(screenshot: app.screenshot())
        previewImage.name = "owned-current-AI-PDF-preview"
        previewImage.lifetime = .keepAlways
        add(previewImage)
        app.buttons["ai-file-preview-close"].tap()
        XCTAssertTrue(pdf.waitForExistence(timeout: 5))
        let markdown = app.buttons["agent-file-artifact-owned-file-md-part"]
        for _ in 0..<3 where mdThumbnail.frame.maxY > app.frame.maxY - 130 { history.swipeUp() }
        XCTAssertGreaterThan(documentPixelCount(mdThumbnail.screenshot().image, red: false), 200,
                             "Markdown缩略图必须包含真实正文像素")
        markdown.tap()
        XCTAssertTrue(app.buttons["ai-file-preview-close"].waitForExistence(timeout: 10))
        let markdownPreview = XCTAttachment(screenshot: app.screenshot())
        markdownPreview.name = "owned-current-AI-Markdown-preview"
        markdownPreview.lifetime = .keepAlways
        add(markdownPreview)
        app.buttons["ai-file-preview-close"].tap()
        XCTAssertTrue(mdThumbnail.waitForExistence(timeout: 5))
        markdown.tap()
        XCTAssertTrue(app.buttons["ai-file-preview-close"].waitForExistence(timeout: 5))
        app.buttons["ai-file-preview-close"].tap()
        XCTAssertTrue(mdThumbnail.waitForExistence(timeout: 5))
    }

    @MainActor func testIMReportMarkdownShowsThumbnailAndSingleFileName() {
        let name = "2026-11-16-maichat-directory-plan-readable.md"
        for hasCaption in [false, true] {
            let app = XCUIApplication()
            app.launchArguments = ["--ai-ui-test", "--im-document-cards-ui-test"]
            app.launchEnvironment["MAICHAT_DOCUMENT_REPORT"] = "1"
            if hasCaption { app.launchEnvironment["MAICHAT_DOCUMENT_REAL_CAPTION"] = "1" }
            app.launch()
            defer { app.terminate() }
            if app.buttons["稍后"].waitForExistence(timeout: 3) { app.buttons["稍后"].tap() }
            let peer = app.buttons["im-conversation-owned-document-peer"]
            XCTAssertTrue(peer.waitForExistence(timeout: 20)); peer.tap()
            let image = app.images.matching(NSPredicate(format: "identifier BEGINSWITH %@", "im-document-thumbnail-md-")).firstMatch
            XCTAssertTrue(image.waitForExistence(timeout: 15), "实际95321字节报告需要内容缩略图")
            XCTAssertEqual(app.staticTexts.matching(NSPredicate(format: "label CONTAINS %@", name)).count, 1,
                "文件名只在元数据中显示一次，不能再把系统占位正文显示出来")
            if hasCaption { XCTAssertTrue(app.staticTexts["请查看这份目录方案。"].exists, "真实配文必须保留") }
            if image.exists { XCTAssertGreaterThan(documentPixelCount(image.screenshot().image, red: false), 200) }
            let capture = XCTAttachment(screenshot: app.screenshot())
            capture.name = hasCaption ? "report95KB-real-caption" : "report95KB-single-file-name"
            capture.lifetime = .keepAlways
            add(capture)
            app.terminate()
        }
    }

    @MainActor func testIMDocumentContentThumbnailsIncomingAndOutgoing() {
        for side in ["incoming", "outgoing"] {
            let app = XCUIApplication()
            app.launchArguments = ["--ai-ui-test", "--im-document-cards-ui-test", "--document-html-ui-test"]
            app.launchEnvironment["MAICHAT_DOCUMENT_SIDE"] = side
            app.launch()
            defer { app.terminate() }
            if app.buttons["稍后"].waitForExistence(timeout: 3) { app.buttons["稍后"].tap() }
            let peer = app.buttons["im-conversation-owned-document-peer"]
            XCTAssertTrue(peer.waitForExistence(timeout: 20))
            peer.tap()
            for ext in ["pdf", "md", "html"] {
                let image = app.images.matching(NSPredicate(format: "identifier BEGINSWITH %@", "im-document-thumbnail-" + ext + "-")).firstMatch
                XCTAssertTrue(image.waitForExistence(timeout: 12), side + " " + ext + "需要内容缩略图")
                let card = app.buttons.containing(.image, identifier: image.identifier).firstMatch
                let history = app.scrollViews.containing(.button, identifier: card.identifier).firstMatch
                for _ in 0..<6 {
                    if image.frame.minY < 135 { history.swipeDown() }
                    else if image.frame.maxY > app.frame.maxY - 155 { history.swipeUp() }
                    else { break }
                }
                if ext == "pdf" { XCTAssertGreaterThan(documentPixelCount(image.screenshot().image, red: true), 200) }
                if ext == "md" { XCTAssertGreaterThan(documentPixelCount(image.screenshot().image, red: false), 200) }
                if ext == "html" {
                    XCTAssertGreaterThan(documentPixelCount(image.screenshot().image, red: false, green: true), 500)
                    XCTAssertLessThan(documentPixelCount(image.screenshot().image, red: true), 50, "HTML脚本不得运行")
                }
                let capture = XCTAttachment(screenshot: app.screenshot())
                capture.name = "IM-document-" + side + "-" + ext
                capture.lifetime = .keepAlways
                add(capture)
                card.tap()
                XCTAssertTrue(app.buttons["document-file-preview-close"].waitForExistence(timeout: 10))
                if ext == "md" { XCTAssertTrue(app.staticTexts["文件交付预览"].waitForExistence(timeout: 10)) }
                app.buttons["document-file-preview-close"].tap()
                XCTAssertTrue(image.waitForExistence(timeout: 5))
            }
            app.terminate()
        }
    }

    @MainActor func testCreationHTMLContentThumbnail() {
        let app = XCUIApplication()
        app.launchArguments = ["--ai-ui-test", "--ai-file-artifact-ui-test", "--document-html-ui-test"]
        app.launch()
        defer { app.terminate() }
        let image = app.images["agent-document-thumbnail-owned-file-html-part"]
        XCTAssertTrue(image.waitForExistence(timeout: 15))
        XCTAssertGreaterThan(documentPixelCount(image.screenshot().image, red: false, green: true), 500)
        XCTAssertLessThan(documentPixelCount(image.screenshot().image, red: true), 50)
        let capture = XCTAttachment(screenshot: app.screenshot())
        capture.name = "creation-HTML-document"
        capture.lifetime = .keepAlways
        add(capture)
        app.buttons["agent-file-artifact-owned-file-html-part"].tap()
        XCTAssertTrue(app.buttons["ai-file-preview-close"].waitForExistence(timeout: 10))
        XCTAssertTrue(app.staticTexts["HTML 内容预览"].waitForExistence(timeout: 6), "完整HTML预览不能是空白页")
        let preview = XCTAttachment(screenshot: app.screenshot())
        preview.name = "creation-HTML-full-preview"
        preview.lifetime = .keepAlways
        add(preview)
        app.buttons["ai-file-preview-close"].tap()
    }

    @MainActor func testInvalidIMHTMLDiffDoesNotGenerateThumbnail() {
        let app = XCUIApplication()
        app.launchArguments = ["--ai-ui-test", "--im-document-cards-ui-test", "--document-html-ui-test"]
        app.launchEnvironment["MAICHAT_DOCUMENT_INVALID_DIFF"] = "1"
        app.launch()
        defer { app.terminate() }
        if app.buttons["稍后"].waitForExistence(timeout: 3) { app.buttons["稍后"].tap() }
        let peer = app.buttons["im-conversation-owned-document-peer"]
        XCTAssertTrue(peer.waitForExistence(timeout: 20)); peer.tap()
        let unavailable = app.images.matching(NSPredicate(format: "identifier BEGINSWITH %@", "im-document-thumbnail-unavailable-html-")).firstMatch
        XCTAssertTrue(unavailable.waitForExistence(timeout: 10))
        XCTAssertFalse(app.images.matching(NSPredicate(format: "identifier BEGINSWITH %@", "im-document-thumbnail-html-")).firstMatch.exists)
        let card = app.buttons.containing(.image, identifier: unavailable.identifier).firstMatch
        card.tap()
        XCTAssertTrue(app.staticTexts["Diff 校验失败"].waitForExistence(timeout: 10))
        app.buttons["document-file-preview-close"].tap()
    }

    @MainActor func testMarkdownThumbnailSurvivesPreparedCacheEviction() {
        let app = XCUIApplication()
        app.launchArguments = ["--ai-ui-test", "--ai-file-artifact-ui-test"]
        app.launchEnvironment["MAICHAT_DOCUMENT_EVICT"] = "1"
        app.launch()
        defer { app.terminate() }
        let image = app.images["agent-document-thumbnail-owned-file-md-part"]
        XCTAssertTrue(image.waitForExistence(timeout: 20))
        XCTAssertGreaterThan(documentPixelCount(image.screenshot().image, red: false), 200,
            "语义缓存被驱逐后，缩略图仍应画正文，不能缓存ProgressView")
    }

    @MainActor func testOversizedMarkdownRetainsFileCardWithoutMisleadingThumbnail() {
        let app = XCUIApplication()
        app.launchArguments = ["--ai-ui-test", "--ai-file-artifact-ui-test"]
        app.launchEnvironment["MAICHAT_DOCUMENT_OVERSIZE"] = "1"
        app.launch()
        defer { app.terminate() }
        let card = app.buttons["agent-file-artifact-owned-file-md-part"]
        XCTAssertTrue(card.waitForExistence(timeout: 20))
        XCTAssertTrue(app.images["agent-document-thumbnail-unavailable-owned-file-md-part"]
            .waitForExistence(timeout: 15), "超限整表不能把前缀解析成真实首屏")
        XCTAssertFalse(app.images["agent-document-thumbnail-owned-file-md-part"].exists)
        card.tap()
        XCTAssertTrue(app.buttons["ai-file-preview-close"].waitForExistence(timeout: 10))
        app.buttons["ai-file-preview-close"].tap()
        XCTAssertTrue(card.waitForExistence(timeout: 5))
    }

    // 固定RGBA画布，检查自有fixture内容，不能用系统文件图标/占位白纸冒充首屏。
    private func documentPixelCount(_ image: UIImage, red: Bool, green: Bool = false) -> Int {
        guard let cg = image.cgImage else { return 0 }
        let width = cg.width, height = cg.height
        var pixels = [UInt8](repeating: 0, count: width * height * 4)
        return pixels.withUnsafeMutableBytes { bytes in
            guard let context = CGContext(data: bytes.baseAddress, width: width, height: height,
                bitsPerComponent: 8, bytesPerRow: width * 4, space: CGColorSpaceCreateDeviceRGB(),
                bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue) else { return 0 }
            context.draw(cg, in: CGRect(x: 0, y: 0, width: width, height: height))
            let data = bytes.bindMemory(to: UInt8.self)
            return stride(from: 0, to: data.count, by: 4).reduce(0) { count, offset in
                let r = data[offset], g = data[offset + 1], b = data[offset + 2]
                let matches = green ? g > 140 && r < 80 && b < 150 :
                    red ? r > 200 && g < 70 && b < 70 : r < 100 && g < 100 && b < 100
                return count + (matches ? 1 : 0)
            }
        }
    }

    @MainActor func testAgentSendsOwnedGenericFilesThroughNormalHost() {
        let app = XCUIApplication()
        app.launchArguments = ["--ai-ui-test", "--ai-account-tools-ui-test", "--im-group-ui-test"]
        app.launchEnvironment["MAICHAT_ACCOUNT_TOOLS_CASE"] = "file-send"
        app.launch()
        defer { app.terminate() }
        let status = app.staticTexts["account-tools-test-status"]
        XCTAssertTrue(status.waitForExistence(timeout: 20))
        let finished = XCTNSPredicateExpectation(
            predicate: NSPredicate(format: "label BEGINSWITH 'PASS FILE SEND' OR label BEGINSWITH 'FAIL FILE SEND'"),
            object: status)
        XCTAssertEqual(XCTWaiter.wait(for: [finished], timeout: 20), .completed)
        XCTAssertTrue(status.label.hasPrefix("PASS FILE SEND"), status.label)
    }

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
