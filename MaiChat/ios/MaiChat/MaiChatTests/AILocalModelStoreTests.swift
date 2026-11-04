// 文件作用：用真实临时文件验证流式导入、固定模型安装、取消与目录边界；不调用推理或网络。
import Foundation
import XCTest
import CryptoKit
@testable import MaiChatCore

final class AILocalModelStoreTests: XCTestCase {
    func testFormerBundledSelectionMigratesWithoutChangingImportsOrMappedFiles() async throws {
        let root = try temporaryDirectory()
        defer { try? FileManager.default.removeItem(at: root) }
        let fixture = try bundledFixture(root: root)
        let storage = root.appendingPathComponent("models")
        let initial = AILocalModelStore(directory: storage, bundledPrimaryDirectory: fixture.directory,
            descriptor: fixture.descriptor)
        let imported = try await initial.importModel(from: fixture.source, role: .projector)
        let oldModel = storage.appendingPathComponent("old-content-version.gguf")
        let oldBytes = gguf(payloadBytes: 1024, fill: 19)
        try oldBytes.write(to: oldModel)
        let catalogURL = storage.appendingPathComponent("catalog.json")
        var legacy = try JSONSerialization.jsonObject(with: Data(contentsOf: catalogURL)) as! [String: Any]
        legacy["selectedModelID"] = "builtin-qwen2.5-0.5b-instruct-q4_k_m"
        legacy["selectedProjectorID"] = imported.id
        let legacyData = try JSONSerialization.data(withJSONObject: legacy, options: [.sortedKeys])
        try legacyData.write(to: catalogURL)

        let reopened = AILocalModelStore(directory: storage, bundledPrimaryDirectory: fixture.directory,
            descriptor: fixture.descriptor)
        let snapshot = try await reopened.snapshot()
        XCTAssertEqual(snapshot.selectedModelID, AILocalModelStore.bundledPrimaryID)
        XCTAssertNil(snapshot.selectedProjectorID)
        XCTAssertTrue(snapshot.entries.contains(imported))
        let selection = try await reopened.selection()
        let resolved = try await reopened.resolve(modelID: "builtin-qwen2.5-0.5b-instruct-q4_k_m", projectorID: nil)
        XCTAssertEqual(selection.modelURL, resolved.modelURL)
        XCTAssertTrue(selection.isBuiltin)
        XCTAssertNil(selection.projectorURL)
        XCTAssertEqual(try Data(contentsOf: oldModel), oldBytes)
        XCTAssertEqual(try Data(contentsOf: catalogURL), legacyData)
    }

    func testBundledPrimaryMissingResourcesReportPackagingFailureWithoutPublishing() async throws {
        let root = try temporaryDirectory()
        defer { try? FileManager.default.removeItem(at: root) }
        let fixture = try bundledFixture(root: root)
        let storage = root.appendingPathComponent("models")
        let missingBundle = AILocalModelStore(directory: storage, bundledPrimaryDirectory: nil,
            descriptor: fixture.descriptor)
        do { _ = try await missingBundle.bundledPrimarySelection(); XCTFail("Missing bundle accepted") }
        catch AILocalModelStoreError.bundledResourceMissing { }
        XCTAssertFalse(FileManager.default.fileExists(atPath: storage.path))

        try FileManager.default.removeItem(at: fixture.source)
        let missingWeights = AILocalModelStore(directory: storage, bundledPrimaryDirectory: fixture.directory,
            descriptor: fixture.descriptor)
        do { _ = try await missingWeights.bundledPrimarySelection(); XCTFail("Missing weights accepted") }
        catch AILocalModelStoreError.bundledResourceMissing { }
        XCTAssertFalse(FileManager.default.fileExists(atPath: storage.path))
    }

    func testBundledPrimaryRejectsChangedManifestAndHashBeforePublishing() async throws {
        let root = try temporaryDirectory()
        defer { try? FileManager.default.removeItem(at: root) }
        let fixture = try bundledFixture(root: root)
        let storage = root.appendingPathComponent("models")
        let store = AILocalModelStore(directory: storage, bundledPrimaryDirectory: fixture.directory,
            descriptor: fixture.descriptor)
        let manifest = fixture.directory.appendingPathComponent("manifest.json")
        let originalManifest = try Data(contentsOf: manifest)
        var changed = try JSONSerialization.jsonObject(with: originalManifest) as! [String: Any]
        changed["filename"] = "../outside.gguf"
        try JSONSerialization.data(withJSONObject: changed).write(to: manifest)
        do { _ = try await store.bundledPrimarySelection(); XCTFail("Changed manifest accepted") }
        catch AILocalModelStoreError.bundledResourceInvalid { }
        XCTAssertFalse(FileManager.default.fileExists(atPath: storage.path))

        try originalManifest.write(to: manifest)
        try gguf(payloadBytes: 1024, fill: 99).write(to: fixture.source)
        do { _ = try await store.bundledPrimarySelection(); XCTFail("Wrong hash accepted") }
        catch AILocalModelStoreError.bundledResourceInvalid { }
        XCTAssertTrue(try FileManager.default.contentsOfDirectory(atPath: storage.path).isEmpty)
    }

    func testBundledPrimaryFirstUseAndRestartResolveVerifiedIdentityWithoutChangingImports() async throws {
        let root = try temporaryDirectory()
        defer { try? FileManager.default.removeItem(at: root) }
        let fixture = try bundledFixture(root: root)
        let storage = root.appendingPathComponent("models")
        let store = AILocalModelStore(directory: storage, bundledPrimaryDirectory: fixture.directory,
            descriptor: fixture.descriptor)
        let imported = try await store.importModel(from: fixture.source, role: .model)
        let importedURL = try await store.selection().modelURL
        let catalogBefore = try Data(contentsOf: storage.appendingPathComponent("catalog.json"))
        let selection = try await store.bundledPrimarySelection()
        let entry = try await store.bundledPrimaryEntry()
        XCTAssertEqual(entry.id, AILocalModelStore.bundledPrimaryID)
        XCTAssertEqual(entry.sha256, fixture.descriptor.sha256)
        XCTAssertEqual(entry.bytes, fixture.descriptor.bytes)
        XCTAssertEqual(selection.modelURL.lastPathComponent, fixture.descriptor.installedFilename)
        XCTAssertNotEqual(selection.modelURL, importedURL)
        XCTAssertEqual(try Data(contentsOf: selection.modelURL), try Data(contentsOf: fixture.source))
        XCTAssertTrue(selection.isBuiltin)
        XCTAssertNil(selection.projectorURL)
        XCTAssertEqual(try Data(contentsOf: storage.appendingPathComponent("catalog.json")), catalogBefore)

        let reopened = AILocalModelStore(directory: storage, bundledPrimaryDirectory: fixture.directory,
            descriptor: fixture.descriptor)
        let resolved = try await reopened.resolve(modelID: AILocalModelStore.bundledPrimaryID, projectorID: nil)
        XCTAssertEqual(resolved.modelURL, selection.modelURL)
        let catalog = try await reopened.snapshot()
        XCTAssertEqual(catalog.selectedModelID, imported.id)
        XCTAssertEqual(try Data(contentsOf: importedURL), try Data(contentsOf: fixture.source))
        do {
            _ = try await reopened.resolve(modelID: AILocalModelStore.bundledPrimaryID, projectorID: imported.id)
            XCTFail("Text-only primary accepted a projector")
        } catch AILocalModelStoreError.unavailable { }
    }

    func testBundledPrimaryConcurrentResolutionNeverOverwritesCorruptedMappedFile() async throws {
        let root = try temporaryDirectory()
        defer { try? FileManager.default.removeItem(at: root) }
        let fixture = try bundledFixture(root: root)
        let storage = root.appendingPathComponent("models")
        let descriptor = fixture.descriptor
        let sourceDirectory = fixture.directory
        let urls = try await withThrowingTaskGroup(of: URL.self, returning: [URL].self) { group in
            for _ in 0..<4 {
                group.addTask {
                    let store = AILocalModelStore(directory: storage, bundledPrimaryDirectory: sourceDirectory,
                        descriptor: descriptor)
                    return try await store.bundledPrimarySelection().modelURL
                }
            }
            var result: [URL] = []
            for try await url in group { result.append(url) }
            return result
        }
        XCTAssertEqual(Set(urls).count, 1)
        XCTAssertEqual(try FileManager.default.contentsOfDirectory(atPath: storage.path), [descriptor.installedFilename])
        let installed = try XCTUnwrap(urls.first)
        let sentinel = gguf(payloadBytes: 1024, fill: 88)
        try sentinel.write(to: installed)
        let reopened = AILocalModelStore(directory: storage, bundledPrimaryDirectory: fixture.directory,
            descriptor: descriptor)
        do { _ = try await reopened.bundledPrimarySelection(); XCTFail("Changed installed file accepted") }
        catch AILocalModelStoreError.bundledResourceInvalid { }
        XCTAssertEqual(try Data(contentsOf: installed), sentinel)
    }

    func testBundledPrimaryContentVersionUsesANewPathAndPreservesOldModel() async throws {
        let root = try temporaryDirectory()
        defer { try? FileManager.default.removeItem(at: root) }
        let oldFixture = try bundledFixture(root: root, name: "old", fill: 16)
        let newFixture = try bundledFixture(root: root, name: "new", fill: 17)
        let storage = root.appendingPathComponent("models")
        let oldStore = AILocalModelStore(directory: storage, bundledPrimaryDirectory: oldFixture.directory,
            descriptor: oldFixture.descriptor)
        let oldURL = try await oldStore.bundledPrimarySelection().modelURL
        let newStore = AILocalModelStore(directory: storage, bundledPrimaryDirectory: newFixture.directory,
            descriptor: newFixture.descriptor)
        let newURL = try await newStore.bundledPrimarySelection().modelURL
        XCTAssertNotEqual(oldURL, newURL)
        XCTAssertEqual(try Data(contentsOf: oldURL), try Data(contentsOf: oldFixture.source))
        XCTAssertEqual(try Data(contentsOf: newURL), try Data(contentsOf: newFixture.source))
        // 损坏的旧导入目录不应该阻止正常 App 解析内置模型，也不能被偷偷重写。
        let badCatalog = Data("preserve unrelated legacy catalog".utf8)
        try badCatalog.write(to: storage.appendingPathComponent("catalog.json"))
        let reopened = AILocalModelStore(directory: storage, bundledPrimaryDirectory: newFixture.directory,
            descriptor: newFixture.descriptor)
        let resolved = try await reopened.bundledPrimarySelection()
        XCTAssertEqual(resolved.modelURL, newURL)
        XCTAssertEqual(try Data(contentsOf: storage.appendingPathComponent("catalog.json")), badCatalog)
    }

    func testImportsIndependentCopiesPersistsSelectionAndPreservesSource() async throws {
        let root = try temporaryDirectory()
        defer { try? FileManager.default.removeItem(at: root) }
        let source = root.appendingPathComponent("source.gguf")
        let original = gguf(payloadBytes: 2 * 1024 * 1024, fill: 43)
        try original.write(to: source)
        let storage = root.appendingPathComponent("models")
        let store = AILocalModelStore(directory: storage)
        let first = try await store.importModel(from: source, role: .model)
        let firstURL = try await store.selection().modelURL
        XCTAssertNotEqual(firstURL, source)
        XCTAssertEqual(try Data(contentsOf: firstURL), original)
        XCTAssertEqual(try Data(contentsOf: source), original)
        XCTAssertEqual(first.sha256, SHA256.hash(data: original).map { String(format: "%02x", $0) }.joined())

        let changed = gguf(payloadBytes: 3 * 64 * 1024, fill: 71)
        try changed.write(to: source)
        let second = try await store.importModel(from: source, role: .model)
        XCTAssertNotEqual(first.id, second.id)
        XCTAssertEqual(try Data(contentsOf: firstURL), original)
        let reopened = AILocalModelStore(directory: storage)
        let catalog = try await reopened.snapshot()
        XCTAssertEqual(catalog.entries.map(\.id), [first.id, second.id])
        XCTAssertEqual(catalog.selectedModelID, second.id)
        let reopenedSelection = try await reopened.selection()
        XCTAssertEqual(try Data(contentsOf: reopenedSelection.modelURL), changed)
        // 主模型保存的是明确 ID；自测选择切到第二份文件后仍应解析原来的第一份。
        let pinnedPrimary = try await reopened.resolve(modelID: first.id, projectorID: nil)
        XCTAssertEqual(pinnedPrimary.modelURL, firstURL)
        XCTAssertEqual(try Data(contentsOf: pinnedPrimary.modelURL), original)
        let afterResolve = try await reopened.snapshot()
        XCTAssertEqual(afterResolve.selectedModelID, second.id)
        let encoded = try String(contentsOf: storage.appendingPathComponent("catalog.json"), encoding: .utf8)
        XCTAssertFalse(encoded.contains(source.path))
        XCTAssertFalse(try FileManager.default.contentsOfDirectory(atPath: storage.path).contains { $0.hasPrefix(".import-") })
    }

    func testProjectorSelectionIsSeparateAndSurvivesRestart() async throws {
        let root = try temporaryDirectory()
        defer { try? FileManager.default.removeItem(at: root) }
        let source = root.appendingPathComponent("example.gguf")
        try gguf().write(to: source)
        let storage = root.appendingPathComponent("models")
        let store = AILocalModelStore(directory: storage)
        let model = try await store.importModel(from: source, role: .model)
        let projector = try await store.importModel(from: source, role: .projector)
        let reopened = AILocalModelStore(directory: storage)
        let catalog = try await reopened.snapshot()
        XCTAssertEqual(catalog.selectedModelID, model.id)
        XCTAssertEqual(catalog.selectedProjectorID, projector.id)
        let selection = try await reopened.selection()
        XCTAssertNotEqual(selection.modelURL, selection.projectorURL)
        try await reopened.selectProjector(id: nil)
        let cleared = try await reopened.selection()
        XCTAssertNil(cleared.projectorURL)
        do { try await reopened.selectModel(id: projector.id); XCTFail("Projector cannot become text model") }
        catch { XCTAssertTrue(error is AILocalModelStoreError) }
    }

    func testRejectsBadMagicTruncationAndMetadataBombWithoutPublishing() async throws {
        let root = try temporaryDirectory()
        defer { try? FileManager.default.removeItem(at: root) }
        let storage = root.appendingPathComponent("models")
        let store = AILocalModelStore(directory: storage)
        var badMagic = gguf(); badMagic.replaceSubrange(0..<4, with: Data("NOPE".utf8))
        var bomb = Data("GGUF".utf8)
        append(3, bytes: 4, to: &bomb); append(1, bytes: 8, to: &bomb); append(1, bytes: 8, to: &bomb)
        append(UInt64.max, bytes: 8, to: &bomb)
        for (index, data) in [badMagic, Data(gguf().prefix(40)), bomb].enumerated() {
            let source = root.appendingPathComponent("bad-\(index).gguf")
            try data.write(to: source)
            do { _ = try await store.importModel(from: source, role: .model); XCTFail("Malformed GGUF accepted") }
            catch { XCTAssertTrue(error is AILocalModelStoreError) }
        }
        let catalog = try await store.snapshot()
        XCTAssertTrue(catalog.entries.isEmpty)
        XCTAssertTrue(try FileManager.default.contentsOfDirectory(atPath: storage.path).isEmpty)
    }

    func testRejectsDirectoriesAndSymlinksWithoutWritingTheirTargets() async throws {
        let root = try temporaryDirectory()
        defer { try? FileManager.default.removeItem(at: root) }
        let source = root.appendingPathComponent("source.gguf")
        let original = gguf()
        try original.write(to: source)
        let alias = root.appendingPathComponent("alias.gguf")
        try FileManager.default.createSymbolicLink(at: alias, withDestinationURL: source)
        let store = AILocalModelStore(directory: root.appendingPathComponent("models"))
        for invalid in [root, alias] {
            do { _ = try await store.importModel(from: invalid, role: .model); XCTFail("Non-regular source accepted") }
            catch { }
        }
        let outside = root.appendingPathComponent("outside")
        try FileManager.default.createDirectory(at: outside, withIntermediateDirectories: false)
        let linkedStorage = root.appendingPathComponent("linked-models")
        try FileManager.default.createSymbolicLink(at: linkedStorage, withDestinationURL: outside)
        do {
            _ = try await AILocalModelStore(directory: linkedStorage).importModel(from: source, role: .model)
            XCTFail("Linked storage accepted")
        } catch { }
        XCTAssertEqual(try Data(contentsOf: source), original)
        XCTAssertTrue(try FileManager.default.contentsOfDirectory(atPath: outside.path).isEmpty)
    }

    func testRejectsCatalogTraversalAndLeavesOutsideFileUntouched() async throws {
        let root = try temporaryDirectory()
        defer { try? FileManager.default.removeItem(at: root) }
        let outside = root.appendingPathComponent("outside.gguf")
        let contents = gguf()
        try contents.write(to: outside)
        let storage = root.appendingPathComponent("models")
        try FileManager.default.createDirectory(at: storage, withIntermediateDirectories: false)
        let id = UUID().uuidString
        let malicious: [String: Any] = ["version": 1, "selectedModelID": id, "entries": [[
            "id": id, "name": "outside", "filename": "../outside.gguf", "bytes": contents.count,
            "sha256": String(repeating: "0", count: 64), "role": "model", "isBuiltin": false]]]
        try JSONSerialization.data(withJSONObject: malicious).write(to: storage.appendingPathComponent("catalog.json"))
        do { _ = try await AILocalModelStore(directory: storage).snapshot(); XCTFail("Traversal catalog accepted") }
        catch { XCTAssertTrue(error is AILocalModelStoreError) }
        XCTAssertEqual(try Data(contentsOf: outside), contents)
    }

    func testOversizedSparseFileAndPreCancelledImportLeaveNoPublishedModel() async throws {
        let root = try temporaryDirectory()
        defer { try? FileManager.default.removeItem(at: root) }
        let source = root.appendingPathComponent("large.gguf")
        try gguf().write(to: source)
        let handle = try FileHandle(forWritingTo: source)
        try handle.truncate(atOffset: AILocalModelStore.maximumFileBytes + 1)
        try handle.close()
        let storage = root.appendingPathComponent("models")
        let store = AILocalModelStore(directory: storage)
        do { _ = try await store.importModel(from: source, role: .model); XCTFail("Oversized file accepted") }
        catch AILocalModelStoreError.tooLarge { }
        try gguf(payloadBytes: 2 * 1024 * 1024).write(to: source)
        let task = Task {
            withUnsafeCurrentTask { $0?.cancel() }
            return try await store.importModel(from: source, role: .model)
        }
        do { _ = try await task.value; XCTFail("Precancelled import accepted") }
        catch is CancellationError { }
        let catalog = try await store.snapshot()
        XCTAssertTrue(catalog.entries.isEmpty)
        XCTAssertTrue(try FileManager.default.contentsOfDirectory(atPath: storage.path).isEmpty)
    }

    func testFixedBuiltinIsReusableAndNeverOverwritesChangedFile() async throws {
        guard let path = ProcessInfo.processInfo.environment["MAICHAT_LOCAL_MODEL_FIXTURE"] else {
            throw XCTSkip("Native Foundation harness supplies the pinned Debug-only fixture path")
        }
        let root = try temporaryDirectory()
        defer { try? FileManager.default.removeItem(at: root) }
        let fixture = URL(fileURLWithPath: path)
        let concurrentDirectory = root.appendingPathComponent("concurrent")
        let concurrent = try await withThrowingTaskGroup(of: URL.self, returning: [URL].self) { group in
            for _ in 0..<4 {
                group.addTask {
                    try AILocalModelStore.installBundledTestModel(source: fixture, directory: concurrentDirectory)
                }
            }
            var urls: [URL] = []
            for try await url in group { urls.append(url) }
            return urls
        }
        XCTAssertEqual(Set(concurrent).count, 1)
        XCTAssertEqual(try FileManager.default.contentsOfDirectory(atPath: concurrentDirectory.path), ["stories260K.gguf"])
        let installed = try AILocalModelStore.installBundledTestModel(source: fixture, directory: root)
        XCTAssertEqual(try Data(contentsOf: installed), try Data(contentsOf: fixture))
        let attributes = try FileManager.default.attributesOfItem(atPath: installed.path)
        let second = try AILocalModelStore.installBundledTestModel(source: fixture, directory: root)
        XCTAssertEqual(second, installed)
        XCTAssertEqual(try FileManager.default.attributesOfItem(atPath: installed.path)[.systemFileNumber] as? NSNumber,
                       attributes[.systemFileNumber] as? NSNumber)
        let store = AILocalModelStore(directory: root, builtinSource: fixture)
        let imported = try await store.importModel(from: fixture, role: .model)
        let catalog = try await store.snapshot()
        XCTAssertTrue(catalog.builtinAvailable)
        XCTAssertEqual(catalog.entries.map(\.id), [AILocalModelStore.builtinID, imported.id])
        try await store.selectModel(id: AILocalModelStore.builtinID)
        let selected = try await store.selection()
        XCTAssertTrue(selected.isBuiltin)
        XCTAssertEqual(selected.modelURL, installed)
        let sentinel = Data("do not overwrite an existing mapped file".utf8)
        try sentinel.write(to: installed)
        XCTAssertThrowsError(try AILocalModelStore.installBundledTestModel(source: fixture, directory: root))
        XCTAssertEqual(try Data(contentsOf: installed), sentinel)
    }

    private func temporaryDirectory() throws -> URL {
        let url = FileManager.default.temporaryDirectory.appendingPathComponent("maichat-local-model-store-\(UUID().uuidString)")
        try FileManager.default.createDirectory(at: url, withIntermediateDirectories: false)
        return url
    }

    // 固定生产 SHA 不可由公开 API 覆盖；这里只用内部注入验证同一安装逻辑和磁盘边界。
    private func bundledFixture(root: URL, name: String = "bundle", fill: UInt8 = 17) throws ->
        (directory: URL, source: URL, descriptor: AILocalModelStore.BundledPrimaryDescriptor) {
        let directory = root.appendingPathComponent(name)
        try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: false)
        let bytes = gguf(payloadBytes: 1024, fill: fill)
        let descriptor = AILocalModelStore.BundledPrimaryDescriptor(version: 1,
            id: AILocalModelStore.bundledPrimaryID, name: "Small container fixture",
            filename: "fixture.gguf", bytes: UInt64(bytes.count),
            sha256: SHA256.hash(data: bytes).map { String(format: "%02x", $0) }.joined(),
            revision: "fixture-only")
        let source = directory.appendingPathComponent(descriptor.filename)
        try bytes.write(to: source)
        try JSONEncoder().encode(descriptor).write(to: directory.appendingPathComponent("manifest.json"))
        return (directory, source, descriptor)
    }

    // 最小合法容器不是可运行权重；这些测试检查宿主复制/目录边界，推理由独立真实权重测试负责。
    private func gguf(payloadBytes: Int = 64, fill: UInt8 = 17) -> Data {
        var data = Data("GGUF".utf8)
        append(3, bytes: 4, to: &data); append(1, bytes: 8, to: &data); append(0, bytes: 8, to: &data)
        append(6, bytes: 8, to: &data); data.append(Data("weight".utf8))
        append(1, bytes: 4, to: &data); append(1, bytes: 8, to: &data)
        append(0, bytes: 4, to: &data); append(0, bytes: 8, to: &data)
        while data.count % 32 != 0 { data.append(0) }
        data.append(Data(repeating: fill, count: payloadBytes))
        return data
    }

    private func append(_ value: UInt64, bytes: Int, to data: inout Data) {
        for byte in 0..<bytes { data.append(UInt8(truncatingIfNeeded: value >> (byte * 8))) }
    }
}
