// 测试宿主与素材仅供 Debug 使用，正式页面不在本文件实现。
#if DEBUG
import AVFoundation
import QuartzCore
import SwiftUI
import UIKit

#if targetEnvironment(simulator)
struct MaiFfplayUITestRoot: View {
    @StateObject private var appState = RemoteIMAppState(loadedSettings: .empty)
    @State private var showingVideo = false

    var body: some View {
        AIAssistantView()
            .environmentObject(appState)
            .fullScreenCover(isPresented: $showingVideo) {
                if let file = Bundle.main.url(forResource: "ffplay-sample", withExtension: "mp4") {
                    MaiFfplayVideoScreen(path: file.path) { showingVideo = false }
                        .ignoresSafeArea()
                        .statusBarHidden()
                }
            }
            .task { showingVideo = true }
    }
}

// 验收同一正式容器中的连续视频及图片/视频切换，不用测试专用导航或标题。
struct MaiFfplaySwipeUITestRoot: View {
    @State private var sequence: MaiMediaSequence?
    var body: some View {
        Color.black.ignoresSafeArea()
            .overlay {
                if let sequence { MaiLocalMediaPager(sequence: sequence) { self.sequence = nil } }
            }
            .statusBarHidden()
            .task {
                guard let first = Bundle.main.url(forResource: "ffplay-sample", withExtension: "mp4"),
                      let second = Bundle.main.url(forResource: "ffplay-seek-sample", withExtension: "mp4")
                else { return }
                let selected = MaiMediaItem(kind: .video, path: first.path)
                sequence = MaiMediaSequence(selected: selected, items: [selected,
                    MaiMediaItem(kind: .video, path: second.path)])
            }
    }
}

struct MaiMixedGalleryUITestRoot: View {
    @State private var sequence: MaiMediaSequence?
    var body: some View {
        Color.black.ignoresSafeArea()
            .overlay {
                if let sequence { MaiLocalMediaPager(sequence: sequence) { self.sequence = nil } }
            }
            .statusBarHidden()
            .task {
                guard let first = Bundle.main.url(forResource: "ffplay-seek-sample", withExtension: "mp4"),
                      let last = Bundle.main.url(forResource: "ffplay-sample", withExtension: "mp4")
                else { return }
                let sample = UIGraphicsImageRenderer(size: CGSize(width: 300, height: 500))
                    .image { context in
                        UIColor.systemOrange.setFill()
                        context.fill(CGRect(x: 0, y: 0, width: 300, height: 500))
                    }
                let file = FileManager.default.temporaryDirectory.appendingPathComponent("mixed-gallery-sample.png")
                guard let data = sample.pngData(), (try? data.write(to: file)) != nil else { return }
                let missing = ProcessInfo.processInfo.environment["MAICHAT_MEDIA_MISSING_VIDEO"] == "1"
                let path = missing ? FileManager.default.temporaryDirectory
                    .appendingPathComponent("missing-video-\(UUID().uuidString).mp4").path : first.path
                let selected = MaiMediaItem(kind: .video, path: path)
                sequence = MaiMediaSequence(selected: selected, items: [selected,
                    MaiMediaItem(kind: .image, path: file.path),
                    MaiMediaItem(kind: .video, path: last.path)])
            }
    }
}

// FFplay 在工作线程回调，不能让 SwiftUI .task 的 MainActor 隔离传入 C 函数指针。
private nonisolated func maiFfplaySeekDelayForUITest(
    _ context: UnsafeMutableRawPointer?, _ level: Int32, _ line: UnsafePointer<CChar>?
) {
    guard let line, String(cString: line).hasPrefix("Seek to "),
          let text = ProcessInfo.processInfo.environment["MAICHAT_FFPLAY_TEST_SEEK_DELAY_MS"],
          let milliseconds = Int(text), milliseconds > 0, milliseconds <= 5_000 else { return }
    Thread.sleep(forTimeInterval: Double(milliseconds) / 1_000)
}

struct MaiFfplaySeekUITestRoot: View {
    let startSeconds: Double
    @StateObject private var appState = RemoteIMAppState(loadedSettings: .empty)
    @State private var showingVideo = false
    @State private var commandResult = "尚未定位"

    var body: some View {
        Color.black
            .fullScreenCover(isPresented: $showingVideo) {
                if let file = Bundle.main.url(forResource: "ffplay-seek-sample", withExtension: "mp4") {
                    MaiFfplayVideoScreen(path: file.path, startSeconds: startSeconds) {
                        showingVideo = false
                    }
                    .ignoresSafeArea()
                    .statusBarHidden()
                    .overlay(alignment: .topTrailing) {
                        Button(commandResult) {
                            Task { @MainActor in
                                let result = await AIMobileHostToolProvider.shared.execute(
                                    name: "maichat_video_command",
                                    argumentsJSON: #"{"action":"seek_seconds","seconds":6}"#)
                                commandResult = result.errorCode == nil ? "定位已受理" : "定位失败"
                            }
                        }
                        .accessibilityIdentifier("agent-seek-six-seconds")
                        .padding(.top, 72)
                        .padding(.trailing, 16)
                    }
                }
            }
            .task {
                // 仅此模拟器验收根视图启用诊断延迟，确定性覆盖手指已松开、
                // 解码尚未执行 seek 的窗口；正常播放器和真机不安装此回调。
                if ProcessInfo.processInfo.environment["MAICHAT_FFPLAY_TEST_SEEK_DELAY_MS"] != nil {
                    maiFfplaySetDiagnosticSink(maiFfplaySeekDelayForUITest, nil)
                }
                AIMobileHostToolProvider.shared.appState = appState
                showingVideo = true
            }
    }
}
#endif

#if DEBUG
struct MaiFfplayAgentDockUITestRoot: View {
    @StateObject private var appState = RemoteIMAppState(loadedSettings: .empty)
    @State private var commandResult = "准备打开视频"

    var body: some View {
        GeometryReader { geometry in
            VStack(spacing: 16) {
                if let presentation = appState.agentVideoPresentation {
                    MaiFfplayVideoScreen(path: presentation.path,
                                         startSeconds: presentation.startSeconds) {
                        appState.agentVideoPresentation = nil
                    }
                    .frame(height: geometry.size.height * 0.57)
                }
                HStack {
                    Button("Agent 重播") {
                        Task { await command(#"{"action":"replay"}"#) }
                    }
                    .accessibilityIdentifier("agent-replay")
                    Button("Agent 从开头播放") {
                        Task { await command(#"{"action":"seek_seconds","seconds":0}"#) }
                    }
                    .accessibilityIdentifier("agent-start-over")
                }
                HStack {
                    Button("Agent 0.5×") {
                        Task { await command(#"{"action":"set_speed","speed":0.5}"#) }
                    }
                    .accessibilityIdentifier("agent-speed-half")
                    Button("Agent 2×") {
                        Task { await command(#"{"action":"set_speed","speed":2}"#) }
                    }
                    .accessibilityIdentifier("agent-speed-double")
                }
                Text(commandResult).accessibilityIdentifier("agent-command-result")
                Spacer()
            }
            .frame(maxWidth: .infinity)
        }
        .background(.black)
        .foregroundStyle(.white)
        .task { await openVideoThroughAgentTool() }
    }

    private func command(_ arguments: String) async {
        let result = await AIMobileHostToolProvider.shared.execute(
            name: "maichat_video_command", argumentsJSON: arguments)
        commandResult = result.errorCode == nil ? "Agent 指令已执行" :
            "Agent 指令失败：\(result.errorCode ?? "unknown")"
    }

    private func openVideoThroughAgentTool() async {
        AppDiagnosticLog.shared.install()
        AIMobileHostToolProvider.shared.appState = appState
        if ProcessInfo.processInfo.arguments.contains("--ffplay-agent-real-video-test") {
            let support = FileManager.default.urls(for: .applicationSupportDirectory,
                                                   in: .userDomainMask)[0]
            let workspace = support.appendingPathComponent("AIAssistant/Workspace",
                                                           isDirectory: true)
            let video = workspace.appendingPathComponent("out5-nowm.mp4")
            guard FileManager.default.fileExists(atPath: video.path) else {
                commandResult = "实际测试视频不存在"
                return
            }
            AIAssistantModel.shared.installVideoToolTestWorkspace(workspace.path)
            let result = await AIMobileHostToolProvider.shared.execute(
                name: "maichat_play_video", argumentsJSON: #"{"path":"out5-nowm.mp4"}"#)
            commandResult = result.errorCode == nil ? "Agent 已打开实际视频" :
                "Agent 打开失败：\(result.errorCode ?? "unknown")"
            return
        }
        guard let sample = Bundle.main.url(forResource: "ffplay-seek-sample",
                                            withExtension: "mp4") else {
            commandResult = "测试视频不存在"
            return
        }
        let workspace = FileManager.default.temporaryDirectory
            .appendingPathComponent("ffplay-agent-dock-test", isDirectory: true)
        let destination = workspace.appendingPathComponent(sample.lastPathComponent)
        do {
            try FileManager.default.createDirectory(at: workspace,
                                                    withIntermediateDirectories: true)
            if !FileManager.default.fileExists(atPath: destination.path) {
                try FileManager.default.copyItem(at: sample, to: destination)
            }
        } catch {
            commandResult = "无法准备测试视频：\(error.localizedDescription)"
            return
        }
        AIAssistantModel.shared.installVideoToolTestWorkspace(workspace.path)
        let result = await AIMobileHostToolProvider.shared.execute(
            name: "maichat_play_video",
            argumentsJSON: #"{"path":"ffplay-seek-sample.mp4"}"#)
        commandResult = result.errorCode == nil ? "Agent 已打开视频" :
            "Agent 打开失败：\(result.errorCode ?? "unknown")"
    }
}
#endif
#endif
