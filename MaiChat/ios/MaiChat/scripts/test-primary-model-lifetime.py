#!/usr/bin/env python3
"""Deterministic actor/file regression using actual backend open/save method bodies.

Native configuration is replaced by the same targeted workspace/file side effects;
no App, account service, model, or user database runs. This is a lifecycle test,
not an iOS/native integration acceptance test.
"""
import argparse
import hashlib
import subprocess
from pathlib import Path


def definition(text, marker):
    start = text.index(marker)
    opening = text.index("{", start)
    depth = 0
    for index in range(opening, len(text)):
        depth += (text[index] == "{") - (text[index] == "}")
        if depth == 0:
            return text[start:index + 1]
    raise RuntimeError("Unclosed source definition: " + marker)


def main():
    parser = argparse.ArgumentParser()
    repo = Path(__file__).resolve().parents[4]
    parser.add_argument("--source", type=Path, default=repo / "MaiChat/ios/MaiChat/MaiChat/Assistant/AIAssistantModel.swift")
    parser.add_argument("--work-dir", type=Path, required=True)
    args = parser.parse_args()
    text = args.source.read_text()
    backend = text[text.index("actor AIAssistantBackend {"):text.index("\n@MainActor\nfinal class AIAssistantModel:")]
    settings = definition(text, "struct AIModelSettings:")
    opened = definition(text, "struct AIAssistantOpenResult:")
    error = definition(text, "struct AIBackendError:")
    open_body = definition(backend, "    func open(accountUserID:")
    save_body = definition(backend, "    func save(_ config:")
    invalidation = definition(backend, "    func invalidatePermanently()") if "    func invalidatePermanently()" in backend else "func invalidatePermanently() { permanentlyInvalidated = true }"
    check = definition(backend, "    private func checkLifecycle()") if "    private func checkLifecycle()" in backend else "private func checkLifecycle() throws {}"
    configure_source = definition(backend, "    private func configure(")
    configure_guard = "try checkLifecycle()" if "try checkLifecycle()" in configure_source else ""
    template = r'''
import Foundation
__SETTINGS__
__OPENED__
__ERROR__
struct AILocalModelSelection { let modelURL: URL }
enum AICloudCredentialSync {
 static func refreshAccountModelKeysIfAvailable() async throws -> Bool { false }
 static func refreshSeedanceIfNeeded(force: Bool = false) async throws {}
 static func refreshBailianIfNeeded(force: Bool = false) async throws {}
}
enum AIAssistantWorkspaceLocator {
 static func directory(for id: String) throws -> URL { URL(fileURLWithPath: id, isDirectory: true) }
}
actor PauseAfterResolution {
 private var entered = false
 private var released = false
 private var ready: [CheckedContinuation<Void, Never>] = []
 private var release: CheckedContinuation<Void, Never>?
 func pause() async {
  if released { return }
  entered = true; for item in ready { item.resume() }; ready = []
  await withCheckedContinuation { release = $0 }
 }
 func waitForEntry() async {
  if entered { return }; await withCheckedContinuation { ready.append($0) }
 }
 func resume() { released = true; release?.resume(); release = nil }
}
actor BackendUnderTest {
 private var root: URL?
 private var settings: AIModelSettings
 private var initialized = false
 private var configurationInProgress = false
 private var permanentlyInvalidated = false
 private let gate: PauseAfterResolution
 init(root: URL, gate: PauseAfterResolution) {
  self.root = root; self.gate = gate
  var local = AIModelSettings(); local.modelProvider = "llama"; local.localModelID = "fixture"; local.model = "local:fixture"
  settings = local
 }
 __CHECK__
 __INVALIDATION__
 private func storedKey(for model: String) -> String { "" }
 private func resolveLocalConfiguration(_ config: AIModelSettings) async throws -> AILocalModelSelection? {
  if !config.isLocalModel { return nil }
  let resolved = AILocalModelSelection(modelURL: root!.appendingPathComponent("fixture.gguf"))
  await gate.pause()
  return resolved
 }
 private func configure(_ config: AIModelSettings, key: String, local: AILocalModelSelection? = nil) throws {
  __CONFIGURE_GUARD__
  guard let root else { throw CancellationError() }
  try FileManager.default.createDirectory(at: root.appendingPathComponent("Workspace"), withIntermediateDirectories: true)
  try Data([1]).write(to: root.appendingPathComponent("sessions.sqlite"))
 }
 __OPEN__
 __SAVE__
}
@main struct Regression {
 static func main() async throws {
  let base = FileManager.default.temporaryDirectory.appendingPathComponent("primary-lifetime-" + UUID().uuidString)
  defer { try? FileManager.default.removeItem(at: base) }
  var failures = 0
  for operation in ["open", "save", "normal_save"] {
   let directory = base.appendingPathComponent(operation)
   try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
   var local = AIModelSettings(); local.modelProvider = "llama"; local.localModelID = "fixture"; local.model = "local:fixture"
   try JSONEncoder().encode(local).write(to: directory.appendingPathComponent("settings.json"))
   let gate = PauseAfterResolution(), backend = BackendUnderTest(root: directory, gate: gate)
   let captured = local
   let task = Task {
    do {
     if operation == "open" { _ = try await backend.open(accountUserID: directory.path) }
     else { _ = try await backend.save(captured) }
     return true
    } catch { return false }
   }
   await gate.waitForEntry()
   if operation != "normal_save" {
    await backend.invalidatePermanently()
    try FileManager.default.removeItem(at: directory)
   }
   await gate.resume()
   let succeeded = await task.value
   let recreated = FileManager.default.fileExists(atPath: directory.path)
   let valid = operation == "normal_save" ? succeeded && recreated : !succeeded && !recreated
   print("\(valid ? "PASS" : "FAIL") \(operation): succeeded=\(succeeded) account_directory_exists=\(recreated)")
   if !valid { failures += 1 }
  }
  if failures != 0 { exit(1) }
 }
}
'''
    replacements = {"__SETTINGS__": settings, "__OPENED__": opened, "__ERROR__": error,
                    "__CHECK__": check, "__INVALIDATION__": invalidation,
                    "__CONFIGURE_GUARD__": configure_guard, "__OPEN__": open_body, "__SAVE__": save_body}
    for marker, value in replacements.items():
        template = template.replace(marker, value)
    args.work_dir.mkdir(parents=True, exist_ok=True)
    swift = args.work_dir / "LifecycleRegression.swift"
    executable = args.work_dir / "lifecycle-regression"
    swift.write_text(template)
    print("source_sha256=" + hashlib.sha256(text.encode()).hexdigest(), flush=True)
    subprocess.run(["xcrun", "swiftc", "-swift-version", "5", "-parse-as-library", str(swift), "-o", str(executable)], check=True)
    result = subprocess.run([str(executable)])
    raise SystemExit(result.returncode)


if __name__ == "__main__":
    main()
