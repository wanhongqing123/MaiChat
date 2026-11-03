// 文件作用：实现 iOS 视频播放器界面及 FFplay 控制桥接，供用户和 Agent 共用播放状态。
// 阅读边界：视频解码在底层 FFplay；本文件负责播放控制、进度、音频会话和手势反馈。
// 持有播放器视图、进度、声音和分屏控件，并接收 Agent 播放命令。
import AVFoundation
import QuartzCore
import SwiftUI
import UIKit

enum MaiMediaKind: Equatable {
    case image
    case video
}

struct MaiMediaItem: Equatable {
    let kind: MaiMediaKind
    let path: String
}

struct MaiMediaSequence: Equatable {
    let items: [MaiMediaItem]
    let index: Int
    let groupID: UUID

    var current: MaiMediaItem { items[index] }

    init(selected: MaiMediaItem, items: [MaiMediaItem], groupID: UUID = UUID()) {
        self.groupID = groupID
        var seen = Set<String>()
        var available = items.filter { item in
            FileManager.default.fileExists(atPath: item.path) &&
                seen.insert("\(item.kind):\(item.path)").inserted
        }
        if !available.contains(selected) { available.insert(selected, at: 0) }
        self.items = available
        index = available.firstIndex(of: selected) ?? 0
    }

    func adjacent(_ direction: Int) -> MaiMediaSequence? {
        var next = index + direction
        while items.indices.contains(next) {
            let candidate = items[next]
            if FileManager.default.fileExists(atPath: candidate.path) {
                return MaiMediaSequence(selected: candidate, items: items, groupID: groupID)
            }
            next += direction
        }
        return nil
    }

    func updatingItems(_ next: [MaiMediaItem]) -> MaiMediaSequence {
        MaiMediaSequence(selected: current, items: next, groupID: groupID)
    }
}

struct MaiFfplayPresentation: Identifiable {
    let id: UUID
    let startSeconds: Double
    let sequence: MaiMediaSequence
    let entryDirection: Int
    let vod: MaiCloudVodSession?

    var groupID: UUID { sequence.groupID }
    var path: String { sequence.current.path }
    var playlist: [String] { sequence.items.map(\.path) }
    var index: Int { sequence.index }

    init(path: String, startSeconds: Double = 0, playlist: [String] = [],
         id: UUID = UUID(),
         groupID: UUID = UUID(), entryDirection: Int = 0,
         vod: MaiCloudVodSession? = nil) {
        self.id = id
        self.startSeconds = startSeconds
        self.entryDirection = entryDirection
        self.vod = vod
        sequence = MaiMediaSequence(selected: MaiMediaItem(kind: .video, path: path),
                                    items: playlist.map { MaiMediaItem(kind: .video, path: $0) },
                                    groupID: groupID)
    }

    init(sequence: MaiMediaSequence, startSeconds: Double = 0, id: UUID = UUID(),
         entryDirection: Int = 0, vod: MaiCloudVodSession? = nil) {
        self.id = id
        self.startSeconds = startSeconds
        self.sequence = sequence
        self.entryDirection = entryDirection
        self.vod = vod
    }

    func updatingPlaylist(_ paths: [String]) -> MaiFfplayPresentation {
        // 保持播放器视图身份不变，后台补齐历史视频时不能重播当前文件。
        updatingMediaItems(paths.map { MaiMediaItem(kind: .video, path: $0) })
    }

    func updatingMediaItems(_ items: [MaiMediaItem]) -> MaiFfplayPresentation {
        MaiFfplayPresentation(sequence: sequence.updatingItems(items),
                              startSeconds: startSeconds, id: id,
                              entryDirection: entryDirection, vod: vod)
    }

    func adjacent(_ direction: Int) -> MaiFfplayPresentation? {
        guard let next = sequence.adjacent(direction), next.current.kind == .video else { return nil }
        return MaiFfplayPresentation(sequence: next, entryDirection: direction)
    }
}

@MainActor
enum MaiFfplayMobilePlayer {
    static var active: MaiFfplayVideoController?

    static var unavailableReason: String {
        guard let active else { return "no video popup is active" }
        return active.playbackUnavailableReason
    }

    static func command(_ action: String, percent: Double?, speed: Double?, seconds: Double?) -> Bool {
        guard let active else { return false }
        if action == "close" {
            active.close()
            return true
        }
        // 语音输入会暂时切换全局 AVAudioSession。Agent 指令到达时先恢复视频输出，
        // 否则 seek 成功了，后续画面在播，扬声器却仍停留在录音模式。
        let restartsPlayback = action == "replay" || action == "set_speed" ||
            (action == "seek_seconds" && seconds == 0)
        if !restartsPlayback && !active.restoreAudioForAgentCommand() { return false }
        if action == "seek_percent" {
            guard let percent, (0...100).contains(percent) else { return false }
            return active.seekPercent(percent / 100)
        }
        if action == "seek_seconds" {
            guard let seconds else { return false }
            return seconds == 0 ? active.replay() : active.seekSeconds(seconds)
        }
        if action == "replay" {
            return active.replay()
        }
        if action == "set_speed" {
            guard let speed else { return false }
            return active.setSpeed(speed)
        }
        return active.sendCommand(action)
    }
}

private func maiFfplayRetainLayer(_ pointer: UnsafeMutableRawPointer?) {
    if let pointer { _ = Unmanaged<AnyObject>.fromOpaque(pointer).retain() }
}

private func maiFfplayReleaseLayer(_ pointer: UnsafeMutableRawPointer?) {
    if let pointer { Unmanaged<AnyObject>.fromOpaque(pointer).release() }
}

@MainActor
private final class MaiFfplayMetalView: UIView {
    override class var layerClass: AnyClass { CAMetalLayer.self }

    var videoPath = ""
    var vod: MaiCloudVodSession?
    var startPositionUs: Int64 = 0
    var onError: ((String) -> Void)?
    private var viewID: UInt64 = 0
    private var session: OpaquePointer?
    private var playbackSpeed = 1.0
    private var lastSize: CGSize = .zero
    private var audioStartUptime = 0.0
    private var audioStartBufferCount: UInt64 = 0
    private var didLogAudioOutput = false

    override func didMoveToWindow() {
        super.didMoveToWindow()
        if window != nil {
            updateDisplayScale()
            attach()
        }
        else { stop() }
    }

    override func layoutSubviews() {
        super.layoutSubviews()
        updateDisplayScale()
        guard viewID != 0 else { attach(); return }
        let size = CGSize(width: max(1, bounds.width * contentScaleFactor),
                          height: max(1, bounds.height * contentScaleFactor))
        guard size != lastSize else { return }
        lastSize = size
        #if DEBUG
            NSLog(
                "[FFplayDisplay] resize %@",
                """
                view=\(Int(bounds.width))x\(Int(bounds.height)) scale=\(contentScaleFactor) \
                pixels=\(Int(size.width))x\(Int(size.height))
                """
            )
        #endif
        maiGraphicsPresenterResize(viewID, UInt32(size.width), UInt32(size.height))
    }

    private func updateDisplayScale() {
        guard let scale = window?.screen.scale else { return }
        if contentScaleFactor != scale { contentScaleFactor = scale }
        if let metalLayer = layer as? CAMetalLayer, metalLayer.contentsScale != scale {
            metalLayer.contentsScale = scale
        }
    }

    private func attach() {
        guard viewID == 0, window != nil, bounds.width > 0, bounds.height > 0,
              !videoPath.isEmpty, maiGraphicsPresenterEnsureStarted() else { return }
        guard activatePlaybackAudioSession() else { return }
        lastSize = CGSize(width: max(1, bounds.width * contentScaleFactor),
                          height: max(1, bounds.height * contentScaleFactor))
        #if DEBUG
            NSLog(
                "[FFplayDisplay] attach %@",
                """
                view=\(Int(bounds.width))x\(Int(bounds.height)) contentScale=\(contentScaleFactor) \
                screenScale=\(window?.screen.scale ?? 0) pixels=\(Int(lastSize.width))x\(Int(lastSize.height))
                """
            )
        #endif
        viewID = maiGraphicsPresenterAttach(
            Unmanaged.passUnretained(layer).toOpaque(), UInt32(lastSize.width),
            UInt32(lastSize.height), maiFfplayRetainLayer, maiFfplayReleaseLayer
        )
        guard viewID != 0 else {
            onError?("Metal 视频视图无法创建")
            return
        }
        resetAudioObservation()
        session = startPlayback(at: startPositionUs)
        if session == nil {
            maiGraphicsPresenterDetach(viewID)
            viewID = 0
            onError?("FFplay 无法启动")
        }
    }

    private func activatePlaybackAudioSession() -> Bool {
        do {
            let audioSession = AVAudioSession.sharedInstance()
            try audioSession.setCategory(.playback, mode: .moviePlayback)
            try audioSession.setActive(true)
            AppDiagnosticLog.shared.record(level: .info, category: "ffplay",
                event: "audio-session-active", fields: [
                    "category": audioSession.category.rawValue,
                    "route": audioSession.currentRoute.outputs.map { $0.portType.rawValue }
                        .joined(separator: ","),
                    "volume": String(format: "%.2f", audioSession.outputVolume)
                ])
            return true
        } catch {
            onError?("无法启动视频声音：\(error.localizedDescription)")
            return false
        }
    }

    func restorePlaybackAudioSession() -> Bool {
        activatePlaybackAudioSession()
    }

    private func resetAudioObservation() {
        audioStartUptime = ProcessInfo.processInfo.systemUptime
        audioStartBufferCount = maiFfplayAppleNonSilentBufferCount()
        didLogAudioOutput = false
    }

    private func startPlayback(at position: Int64) -> OpaquePointer? {
        if let vod {
            return maiFfplayIosStartVodAtSpeed(viewID, vod.pointer,
                                               playbackSpeed, position)
        }
        return videoPath.withCString {
            maiFfplayIosStartAtSpeed(viewID, $0, playbackSpeed, position)
        }
    }

    func configureInitialSpeed(_ speed: Double) {
        guard session == nil, viewID == 0 else { return }
        playbackSpeed = speed
    }

    func sendCommand(_ action: String) -> Bool {
        guard let session else { return false }
        return action.withCString { maiFfplayIosCommand(session, $0) == 1 }
    }

    func seekPercent(_ fraction: Double) -> Bool {
        guard let session else { return false }
        return maiFfplayIosSeekPercent(session, fraction) == 1
    }

    func seekAbsoluteUs(_ position: Int64) -> Bool {
        guard let session else { return false }
        return maiFfplayIosSeekAbsoluteUs(session, position) == 1
    }

    func replayFromBeginning() -> Bool {
        guard viewID != 0 else { return false }
        if let previous = session {
            session = nil
            maiFfplayIosStop(previous)
        }
        guard activatePlaybackAudioSession() else { return false }
        startPositionUs = 0
        resetAudioObservation()
        session = startPlayback(at: 0)
        return session != nil
    }

    func setSpeed(_ speed: Double) -> Bool {
        guard [0.25, 0.5, 1.0, 1.5, 2.0].contains(speed), let previous = session,
              viewID != 0 else { return false }
        if speed == playbackSpeed { return true }
        let position = max(0, playbackStatus()?.position_us ?? 0)
        session = nil
        maiFfplayIosStop(previous)
        guard activatePlaybackAudioSession() else { return false }
        playbackSpeed = speed
        resetAudioObservation()
        session = startPlayback(at: position)
        return session != nil
    }

    func checkPlayback() {
        guard let session, maiFfplayIosHasFinished(session) == 1,
              maiFfplayIosExitCode(session) != 0 else { return }
        onError?("FFplay 无法播放此视频")
    }

    func playbackStatus() -> MaiFfplayPlaybackStatus? {
        guard session != nil else { return nil }
        var status = MaiFfplayPlaybackStatus()
        guard maiFfplayGetPlaybackStatus(&status) == 1 else { return nil }
        if !didLogAudioOutput &&
            ProcessInfo.processInfo.systemUptime - audioStartUptime >= 3 {
            didLogAudioOutput = true
            let audioSession = AVAudioSession.sharedInstance()
            AppDiagnosticLog.shared.record(level: .info, category: "ffplay",
                event: "audio-output-observed", fields: [
                    "non_silent_buffers": String(
                        maiFfplayAppleNonSilentBufferCount() - audioStartBufferCount),
                    "category": audioSession.category.rawValue,
                    "route": audioSession.currentRoute.outputs.map { $0.portType.rawValue }
                        .joined(separator: ","),
                    "volume": String(format: "%.2f", audioSession.outputVolume)
                ])
        }
        return status
    }

    var hasFirstFrame: Bool {
        guard let session else { return false }
        return maiFfplayIosHasFirstFrame(session) == 1
    }

    var unavailableReason: String {
        guard let session else { return "the video popup has no playback session" }
        if maiFfplayIosHasFinished(session) == 1 {
            return "FFplay exited with status \(maiFfplayIosExitCode(session))"
        }
        return "FFplay is running but did not accept the command"
    }

    func stop() {
        freezeForPaging()
        if viewID != 0 {
            maiGraphicsPresenterDetach(viewID)
            viewID = 0
        }
    }

    // 分页提交时只停止解码，保留已提交的 Metal 画面。整页滑出后再 detach，
    // 避免唯一 FFplay 实例交接期间把仍在屏幕里的旧视频清成黑色。
    func freezeForPaging() {
        if let session {
            self.session = nil
            maiFfplayIosStop(session)
        }
    }

}

@MainActor
final class MaiFfplayVideoController: UIViewController {
    private let path: String
    private let vod: MaiCloudVodSession?
    private let startSeconds: Double
    private let embeddedInMediaPager: Bool
    private let onClose: () -> Void
    private var onPlaybackReady: (() -> Void)?
    var onPlaybackFailure: ((String) -> Void)?
    var controlsInteractionChanged: ((Bool) -> Void)?
    private let metalView = MaiFfplayMetalView()
    private let pauseButton = UIButton(type: .system)
    private let seekSlider = UISlider()
    private let timeLabel = UILabel()
    private let errorLabel = UILabel()
    private let saveButton = UIButton(type: .system)
    private let speedButton = UIButton(type: .system)
    private let speedChoices = UIStackView()
    private let saveStatusLabel = UILabel()
    private var playbackTimer: Timer?
    private var playing = true
    private var playbackSpeed = 1.0
    private var saving = false
    private var didReportPlaybackReady = false
    private var controlViews: [UIView] = []
    private var controlsVisible = true
    private var controlsPlaying = true
    private var controlsSeeking = false
    private var controlTouches = 0
    private var hostActive = false
    private var controlsHideTask: Task<Void, Never>?

    init(path: String, startSeconds: Double = 0, initialSpeed: Double = 1,
         onPlaybackReady: (() -> Void)? = nil,
         vod: MaiCloudVodSession? = nil,
         embeddedInMediaPager: Bool = false,
         close: @escaping () -> Void) {
        self.path = path
        self.vod = vod
        self.startSeconds = startSeconds
        playbackSpeed = initialSpeed
        self.embeddedInMediaPager = embeddedInMediaPager
        self.onPlaybackReady = onPlaybackReady
        onClose = close
        super.init(nibName: nil, bundle: nil)
    }

    required init?(coder: NSCoder) { nil }


    /// 初始化播放器承载视图、播放控件与进度观察。
    /// 视图仅创建一次；重播和跳转复用同一播放实例，避免重建导致音频状态丢失。
    override func viewDidLoad() {
        super.viewDidLoad()
        view.backgroundColor = .black
        // 媒体身份交给无障碍画面描述，供读屏和回归识别，不绘制文件名标题。
        metalView.isAccessibilityElement = true
        metalView.accessibilityTraits = .image
        metalView.accessibilityLabel = "视频画面"
        metalView.accessibilityValue = URL(fileURLWithPath: path).lastPathComponent
        metalView.accessibilityIdentifier = "ffplay-video-surface"
        metalView.videoPath = path
        metalView.configureInitialSpeed(playbackSpeed)
        metalView.vod = vod
        metalView.startPositionUs = Int64((startSeconds * 1_000_000).rounded())
        metalView.onError = { [weak self] message in
            self?.errorLabel.text = message
            self?.errorLabel.isHidden = false
            self?.onPlaybackFailure?(message)
            self?.revealControls()
        }
        metalView.translatesAutoresizingMaskIntoConstraints = false
        view.addSubview(metalView)
        metalView.addGestureRecognizer(UITapGestureRecognizer(target: self, action: #selector(videoTapped)))

        errorLabel.textColor = .white
        errorLabel.accessibilityIdentifier = "ffplay-error"
        errorLabel.textAlignment = .center
        errorLabel.isHidden = true
        errorLabel.translatesAutoresizingMaskIntoConstraints = false
        view.addSubview(errorLabel)

        pauseButton.setImage(UIImage(systemName: "pause.fill"), for: .normal)
        pauseButton.accessibilityLabel = "暂停视频"
        pauseButton.accessibilityIdentifier = "ffplay-pause"
        pauseButton.tintColor = .white
        pauseButton.contentEdgeInsets = UIEdgeInsets(top: 14, left: 0, bottom: 0, right: 0)
        pauseButton.addTarget(self, action: #selector(togglePause), for: .touchUpInside)
        pauseButton.translatesAutoresizingMaskIntoConstraints = false
        pauseButton.widthAnchor.constraint(equalToConstant: 54).isActive = true
        pauseButton.heightAnchor.constraint(equalToConstant: 54).isActive = true
        seekSlider.minimumTrackTintColor = .white
        seekSlider.maximumTrackTintColor = UIColor.white.withAlphaComponent(0.35)
        seekSlider.thumbTintColor = .white
        seekSlider.addTarget(self, action: #selector(seekReleased), for: .touchUpInside)
        seekSlider.accessibilityIdentifier = "ffplay-seek"
        seekSlider.addTarget(self, action: #selector(seekReleased), for: .touchUpOutside)
        timeLabel.text = "0:00 / 0:00"
        timeLabel.accessibilityIdentifier = "ffplay-time"
        timeLabel.textColor = .white
        timeLabel.font = .monospacedDigitSystemFont(ofSize: 15, weight: .medium)
        let timeline = UIStackView(arrangedSubviews: [timeLabel, seekSlider])
        timeline.axis = .vertical
        timeline.spacing = 1
        let controls = UIStackView(arrangedSubviews: [pauseButton, timeline])
        controls.axis = .horizontal
        controls.alignment = .center
        controls.spacing = 12
        controls.translatesAutoresizingMaskIntoConstraints = false
        view.addSubview(controls)

        let shareButton = actionButton("square.and.arrow.up", label: "转发或分享",
                                       identifier: "ffplay-share", action: #selector(shareTapped))
        saveButton.setImage(UIImage(systemName: "square.and.arrow.down"), for: .normal)
        saveButton.accessibilityLabel = "保存到相册"
        saveButton.accessibilityIdentifier = "ffplay-save"
        saveButton.tintColor = .white
        saveButton.backgroundColor = UIColor.white.withAlphaComponent(0.18)
        saveButton.layer.cornerRadius = 25
        saveButton.addTarget(self, action: #selector(saveTapped), for: .touchUpInside)
        saveButton.translatesAutoresizingMaskIntoConstraints = false
        saveButton.widthAnchor.constraint(equalToConstant: 50).isActive = true
        saveButton.heightAnchor.constraint(equalToConstant: 50).isActive = true
        speedButton.setTitle("1×", for: .normal)
        speedButton.setTitleColor(.white, for: .normal)
        speedButton.titleLabel?.font = .systemFont(ofSize: 16, weight: .semibold)
        speedButton.accessibilityLabel = "播放倍速 1.0×"
        speedButton.accessibilityIdentifier = "ffplay-speed"
        speedButton.backgroundColor = UIColor.white.withAlphaComponent(0.18)
        speedButton.layer.cornerRadius = 25
        speedButton.addTarget(self, action: #selector(toggleSpeedChoices), for: .touchUpInside)
        speedButton.translatesAutoresizingMaskIntoConstraints = false
        speedButton.widthAnchor.constraint(equalToConstant: 56).isActive = true
        speedButton.heightAnchor.constraint(equalToConstant: 50).isActive = true

        let copyButton = actionButton("doc.on.doc", label: "复制文件路径",
                                      identifier: "ffplay-copy-path", action: #selector(copyPathTapped))
        if vod != nil {
            shareButton.isHidden = true
            saveButton.isHidden = true
            copyButton.isHidden = true
        }
        let actions = UIStackView(arrangedSubviews: [shareButton, saveButton, speedButton, copyButton])
        actions.axis = .horizontal
        actions.spacing = 12
        actions.translatesAutoresizingMaskIntoConstraints = false
        view.addSubview(actions)

        // 倍速直接在播放画面内选择，避免两层系统弹窗遮住视频。
        speedChoices.axis = .horizontal
        speedChoices.alignment = .fill
        speedChoices.distribution = .fillEqually
        speedChoices.spacing = 4
        // 使用统一实色底，跨越视频画面与黑边时仍能看清完整菜单边界。
        speedChoices.backgroundColor = UIColor(white: 0.15, alpha: 1)
        speedChoices.layer.cornerRadius = 18
        speedChoices.layer.borderWidth = 1
        speedChoices.layer.borderColor = UIColor.white.withAlphaComponent(0.16).cgColor
        speedChoices.accessibilityIdentifier = "ffplay-speed-options"
        speedChoices.insetsLayoutMarginsFromSafeArea = false
        speedChoices.layoutMargins = UIEdgeInsets(top: 6, left: 6, bottom: 6, right: 6)
        speedChoices.isLayoutMarginsRelativeArrangement = true
        speedChoices.isHidden = true
        speedChoices.translatesAutoresizingMaskIntoConstraints = false
        for rate in [0.25, 0.5, 1.0, 1.5, 2.0] {
            let choice = UIButton(type: .system)
            choice.setTitle("\(rate)×", for: .normal)
            choice.setTitleColor(.white, for: .normal)
            choice.titleLabel?.font = .systemFont(ofSize: 14, weight: .semibold)
            choice.accessibilityIdentifier = "ffplay-speed-\(rate)"
            choice.tag = Int(rate * 100)
            choice.addTarget(self, action: #selector(speedChoiceTapped(_:)), for: .touchUpInside)
            speedChoices.addArrangedSubview(choice)
        }
        view.addSubview(speedChoices)

        saveStatusLabel.textColor = .white
        saveStatusLabel.font = .systemFont(ofSize: 14, weight: .medium)
        saveStatusLabel.textAlignment = .center
        saveStatusLabel.numberOfLines = 2
        saveStatusLabel.accessibilityIdentifier = "ffplay-save-status"
        saveStatusLabel.backgroundColor = UIColor.black.withAlphaComponent(0.7)
        saveStatusLabel.layer.cornerRadius = 10
        saveStatusLabel.clipsToBounds = true
        saveStatusLabel.isHidden = true
        saveStatusLabel.translatesAutoresizingMaskIntoConstraints = false
        view.addSubview(saveStatusLabel)

        let closeButton = UIButton(type: .system)
        closeButton.setImage(UIImage(systemName: "xmark"), for: .normal)
        closeButton.accessibilityIdentifier = "ffplay-close"
        closeButton.accessibilityLabel = "关闭视频"
        closeButton.isHidden = embeddedInMediaPager
        closeButton.tintColor = .white
        closeButton.backgroundColor = UIColor.white.withAlphaComponent(0.2)
        closeButton.layer.cornerRadius = 25
        closeButton.addTarget(self, action: #selector(closeTapped), for: .touchUpInside)
        closeButton.translatesAutoresizingMaskIntoConstraints = false
        view.addSubview(closeButton)

        NSLayoutConstraint.activate([
            metalView.leadingAnchor.constraint(equalTo: view.leadingAnchor),
            metalView.trailingAnchor.constraint(equalTo: view.trailingAnchor),
            metalView.topAnchor.constraint(equalTo: view.topAnchor),
            metalView.bottomAnchor.constraint(equalTo: view.bottomAnchor),
            errorLabel.centerXAnchor.constraint(equalTo: view.centerXAnchor),
            errorLabel.centerYAnchor.constraint(equalTo: view.centerYAnchor),
            controls.leadingAnchor.constraint(equalTo: view.leadingAnchor, constant: 16),
            controls.trailingAnchor.constraint(equalTo: view.trailingAnchor, constant: -16),
            controls.bottomAnchor.constraint(equalTo: actions.topAnchor, constant: -4),
            actions.trailingAnchor.constraint(equalTo: view.trailingAnchor, constant: -18),
            actions.bottomAnchor.constraint(equalTo: view.safeAreaLayoutGuide.bottomAnchor,
                                           constant: -4),
            speedChoices.leadingAnchor.constraint(equalTo: view.safeAreaLayoutGuide.leadingAnchor, constant: 16),
            speedChoices.trailingAnchor.constraint(equalTo: view.safeAreaLayoutGuide.trailingAnchor, constant: -18),
            // 60 pt 面板减去上下各 6 pt 后，选项保持 48 pt 高度。
            speedChoices.heightAnchor.constraint(equalToConstant: 60),
            speedChoices.bottomAnchor.constraint(equalTo: controls.topAnchor, constant: -10),
            saveStatusLabel.centerXAnchor.constraint(equalTo: view.centerXAnchor),
            saveStatusLabel.bottomAnchor.constraint(equalTo: controls.topAnchor, constant: -18),
            saveStatusLabel.widthAnchor.constraint(greaterThanOrEqualToConstant: 160),
            saveStatusLabel.widthAnchor.constraint(lessThanOrEqualTo: view.widthAnchor,
                                                  constant: -32),
            saveStatusLabel.heightAnchor.constraint(equalToConstant: 40),
            closeButton.leadingAnchor.constraint(equalTo: view.leadingAnchor, constant: 18),
            closeButton.topAnchor.constraint(equalTo: view.safeAreaLayoutGuide.topAnchor,
                                             constant: 12),
            closeButton.widthAnchor.constraint(equalToConstant: 50),
            closeButton.heightAnchor.constraint(equalToConstant: 50),
        ])
        controlViews = [controls, actions]
        // 所有宿主复用触摸互锁；拖动及菜单操作期间不得把用户正在操作的控件隐藏。
        observeControlTouches(in: view)
    }

    override func viewDidAppear(_ animated: Bool) {
        super.viewDidAppear(animated)
        activateForVisibleHost()
    }

    // A selected page can embed this controller after its parent appeared.
    // Match the standalone SwiftUI host's ownership without starting neighbors.
    func activateForVisibleHost() {
        hostActive = true
        MaiLocalMusicPlayer.shared.stop()
        if let previous = MaiFfplayMobilePlayer.active, previous !== self {
            previous.stop()
        }
        MaiFfplayMobilePlayer.active = self
        if playbackTimer == nil {
            playbackTimer = Timer.scheduledTimer(withTimeInterval: 0.5, repeats: true) {
                [weak self] _ in
                Task { @MainActor [weak self] in
                    self?.refreshPlayback()
                }
            }
        }
        refreshPlayback()
    }

    private func refreshPlayback() {
        metalView.checkPlayback()
        if !didReportPlaybackReady && metalView.hasFirstFrame {
            didReportPlaybackReady = true
            onPlaybackReady?()
            revealControls()
        }
        guard let status = metalView.playbackStatus(), status.duration_us > 0 else { return }
        // 手指松开后，云盘读取/解码尚未完成时沿用共享核心发布的目标反馈。
        // 等本次新队列时钟生效再跟随真实进度，避免旧时钟把滑块拉回去。
        let position = min(max(status.seeking != 0 ? status.seek_target_us : status.position_us, 0),
                           status.duration_us)
        if !seekSlider.isTracking {
            seekSlider.value = Float(Double(position) / Double(status.duration_us))
        }
        timeLabel.text = "\(Self.timestamp(position)) / \(Self.timestamp(status.duration_us))"
        if status.seeking == 0 { playing = status.paused == 0 }
        if controlsSeeking != (status.seeking != 0) {
            controlsSeeking = status.seeking != 0
            revealControls()
        }
        playbackSpeed = Double(status.speed_percent) / 100.0
        speedButton.setTitle("\(playbackSpeed)×", for: .normal)
        speedButton.accessibilityLabel = "播放倍速 \(playbackSpeed)×"
        refreshPlayButton()
    }

    private static func timestamp(_ microseconds: Int64) -> String {
        let seconds = max(0, microseconds / 1_000_000)
        return String(format: "%lld:%02lld", seconds / 60, seconds % 60)
    }

    func stop() {
        hostActive = false
        controlTouches = 0
        controlsHideTask?.cancel()
        controlsHideTask = nil
        playbackTimer?.invalidate()
        playbackTimer = nil
        metalView.stop()
        controlsInteractionChanged?(false)
        if MaiFfplayMobilePlayer.active === self { MaiFfplayMobilePlayer.active = nil }
    }

    func freezeForPaging() {
        hostActive = false
        controlTouches = 0
        controlsHideTask?.cancel()
        controlsHideTask = nil
        playbackTimer?.invalidate()
        playbackTimer = nil
        metalView.freezeForPaging()
        controlsInteractionChanged?(false)
        if MaiFfplayMobilePlayer.active === self { MaiFfplayMobilePlayer.active = nil }
    }

    var speedForPaging: Double { playbackSpeed }

    var playbackUnavailableReason: String { metalView.unavailableReason }

    func sendCommand(_ action: String) -> Bool {
        let command: String
        if action == "play" {
            if playing { return true }
            command = "pause"
        } else if action == "pause" {
            if !playing { return true }
            command = "pause"
        } else {
            command = action == "toggle_pause" ? "pause" : action
        }
        let accepted = metalView.sendCommand(command)
        if accepted && command == "pause" {
            playing.toggle()
            refreshPlayButton()
        }
        return accepted
    }

    func seekPercent(_ fraction: Double) -> Bool {
        let accepted = metalView.seekPercent(fraction)
        if accepted { revealControls() }
        return accepted
    }

    func seekSeconds(_ seconds: Double) -> Bool {
        guard seconds.isFinite, seconds >= 0,
              let status = metalView.playbackStatus(), status.duration_us > 0,
              seconds * 1_000_000 <= Double(status.duration_us) else { return false }
        let accepted = metalView.seekAbsoluteUs(Int64((seconds * 1_000_000).rounded()))
        if accepted { revealControls() }
        return accepted
    }

    func replay() -> Bool {
        guard metalView.replayFromBeginning() else { return false }
        playing = true
        seekSlider.value = 0
        refreshPlayButton()
        return true
    }

    func restoreAudioForAgentCommand() -> Bool {
        metalView.restorePlaybackAudioSession()
    }

    func setSpeed(_ speed: Double) -> Bool {
        guard metalView.setSpeed(speed) else { return false }
        playbackSpeed = speed
        speedButton.setTitle("\(speed)×", for: .normal)
        speedButton.accessibilityLabel = "播放倍速 \(speed)×"
        playing = true
        refreshPlayButton()
        revealControls()
        return true
    }

    func close() { onClose() }

    private func observeControlTouches(in parent: UIView) {
        if let control = parent as? UIControl {
            control.addTarget(self, action: #selector(controlTouchBegan), for: .touchDown)
            control.addTarget(self, action: #selector(controlTouchEnded),
                              for: [.touchUpInside, .touchUpOutside, .touchCancel])
        }
        parent.subviews.forEach { observeControlTouches(in: $0) }
    }

    @objc private func controlTouchBegan() {
        controlTouches += 1
        controlsInteractionChanged?(true)
        revealControls()
    }
    @objc private func controlTouchEnded() {
        controlTouches = max(0, controlTouches - 1)
        controlsInteractionChanged?(controlTouches > 0)
        scheduleControlsHide()
    }

    @objc private func videoTapped() {
        if !speedChoices.isHidden {
            speedChoices.isHidden = true
            revealControls()
        } else if controlsVisible && playing {
            controlsHideTask?.cancel()
            setControlsVisible(false)
        } else { revealControls() }
    }

    private func revealControls() {
        setControlsVisible(true)
        scheduleControlsHide()
    }

    private func setControlsVisible(_ visible: Bool) {
        controlsVisible = visible
        for control in controlViews {
            if visible { control.isHidden = false }
            control.isUserInteractionEnabled = visible
            control.accessibilityElementsHidden = !visible
        }
        UIView.animate(withDuration: 0.18, delay: 0, options: [.beginFromCurrentState, .allowUserInteraction]) {
            self.controlViews.forEach { $0.alpha = visible ? 1 : 0 }
        } completion: { [weak self] _ in
            guard let self, !self.controlsVisible else { return }
            self.controlViews.forEach { $0.isHidden = true }
        }
    }

    private func scheduleControlsHide() {
        controlsHideTask?.cancel()
        controlsHideTask = nil
        guard hostActive, playing, !controlsSeeking, controlTouches == 0, speedChoices.isHidden, !saving,
              presentedViewController == nil, errorLabel.isHidden else { return }
        // 每次操作更新同一个任务；停止或翻页取消，旧播放器不能影响新一页的控件。
        controlsHideTask = Task { @MainActor [weak self] in
            do { try await Task.sleep(nanoseconds: 2_500_000_000) }
            catch { return }
            guard let self, !Task.isCancelled, self.hostActive, self.playing, !self.controlsSeeking,
                  self.controlTouches == 0, self.speedChoices.isHidden, !self.saving,
                  self.presentedViewController == nil, self.errorLabel.isHidden else { return }
            self.setControlsVisible(false)
        }
    }

    @objc private func togglePause() { _ = sendCommand("toggle_pause") }
    @objc private func seekReleased() { _ = seekPercent(Double(seekSlider.value)) }
    @objc private func closeTapped() { close() }

    private func refreshPlayButton() {
        if controlsPlaying != playing {
            controlsPlaying = playing
            revealControls()
        }
        pauseButton.setImage(UIImage(systemName: playing ? "pause.fill" : "play.fill"), for: .normal)
        pauseButton.accessibilityLabel = playing ? "暂停视频" : "播放视频"
    }

    private func actionButton(_ symbol: String, label: String, identifier: String,
                              action: Selector) -> UIButton {
        let button = UIButton(type: .system)
        button.setImage(UIImage(systemName: symbol), for: .normal)
        button.accessibilityLabel = label
        button.accessibilityIdentifier = identifier
        button.tintColor = .white
        button.backgroundColor = UIColor.white.withAlphaComponent(0.18)
        button.layer.cornerRadius = 25
        button.addTarget(self, action: action, for: .touchUpInside)
        button.translatesAutoresizingMaskIntoConstraints = false
        button.widthAnchor.constraint(equalToConstant: 50).isActive = true
        button.heightAnchor.constraint(equalToConstant: 50).isActive = true
        return button
    }

    private func showStatus(_ message: String) {
        saveStatusLabel.text = "  \(message)  "
        saveStatusLabel.accessibilityLabel = message
        saveStatusLabel.isHidden = false
        DispatchQueue.main.asyncAfter(deadline: .now() + 2.5) { [weak self] in
            self?.saveStatusLabel.isHidden = true
        }
    }

    @objc private func shareTapped() {
        if vod != nil { showStatus("云盘视频正在点播，无法直接转发") ; return }
        let sheet = UIActivityViewController(activityItems: [URL(fileURLWithPath: path)],
                                             applicationActivities: nil)
        sheet.completionWithItemsHandler = { [weak self] _, _, _, _ in
            Task { @MainActor [weak self] in self?.revealControls() }
        }
        sheet.popoverPresentationController?.sourceView = view
        sheet.popoverPresentationController?.sourceRect = CGRect(
            x: view.bounds.midX, y: view.bounds.maxY - 50, width: 1, height: 1)
        present(sheet, animated: true)
    }

    @objc private func saveTapped() {
        if vod != nil { showStatus("云盘视频正在点播，未下载到相册") ; return }
        guard !saving else { return }
        saving = true
        saveButton.isEnabled = false
        let requestedAt = Date()
        let fileURL = URL(fileURLWithPath: path)
        Task { @MainActor in
            defer {
                saving = false
                saveButton.isEnabled = true
                revealControls()
            }
            do {
                // 先检查真实文件和视频轨道；无效文件不应进入 Photos 的变更块。
                let values = try fileURL.resourceValues(forKeys: [.isRegularFileKey, .fileSizeKey])
                guard values.isRegularFile == true, (values.fileSize ?? 0) > 0,
                      !(try await AVURLAsset(url: fileURL).loadTracks(withMediaType: .video)).isEmpty else {
                    showStatus("视频文件不可用，无法保存")
                    return
                }
                // 保存只需要向相册添加作品，无需为播放器请求整个图库的读取权限。
                guard await MaiPhotoLibraryMediaWriter.requestAddAccess() else {
                    showStatus("请允许添加视频到相册")
                    return
                }
                try await MaiPhotoLibraryMediaWriter.saveVideo(at: fileURL, createdAt: requestedAt)
                showStatus("已保存到相册")
            } catch {
                showStatus("保存失败：\(error.localizedDescription)")
            }
        }
    }

    @objc private func toggleSpeedChoices() {
        speedChoices.isHidden.toggle()
        revealControls()
        #if targetEnvironment(simulator)
        if !speedChoices.isHidden && ProcessInfo.processInfo.arguments.contains("--ai-ui-test") {
            DispatchQueue.main.async { [weak self] in
                guard let self else { return }
                NSLog("[PlaybackSpeedLayout] host=%@ safe=%@ menu=%@ menuSafe=%@ margins=%@",
                    String(describing: self.view.bounds), String(describing: self.view.safeAreaInsets),
                    String(describing: self.speedChoices.frame), String(describing: self.speedChoices.safeAreaInsets),
                    String(describing: self.speedChoices.layoutMargins))
                for case let button as UIButton in self.speedChoices.arrangedSubviews {
                    NSLog("[PlaybackSpeedLayout] choice=%@ frame=%@ title=%@ lineHeight=%.2f",
                        button.currentTitle ?? "", String(describing: button.frame),
                        String(describing: button.titleLabel?.frame), button.titleLabel?.font.lineHeight ?? 0)
                }
            }
        }
        #endif
    }

    @objc private func speedChoiceTapped(_ sender: UIButton) {
        speedChoices.isHidden = true
        if !setSpeed(Double(sender.tag) / 100) { showStatus("倍速切换失败") }
    }

    @objc private func copyPathTapped() {
        if vod != nil { showStatus("云盘视频没有本地文件路径") ; return }
        UIPasteboard.general.string = path
        showStatus("已复制文件路径")
    }
}

struct MaiFfplayVideoScreen: UIViewControllerRepresentable {
    let path: String
    let vod: MaiCloudVodSession?
    let startSeconds: Double
    let onPlaybackReady: (() -> Void)?
    let close: () -> Void

    init(path: String, startSeconds: Double = 0,
         onPlaybackReady: (() -> Void)? = nil,
         vod: MaiCloudVodSession? = nil,
         close: @escaping () -> Void) {
        self.path = path
        self.vod = vod
        self.startSeconds = startSeconds
        self.onPlaybackReady = onPlaybackReady
        self.close = close
    }

    func makeUIViewController(context _: Context) -> MaiFfplayVideoController {
        MaiLocalMusicPlayer.shared.stop()
        let controller = MaiFfplayVideoController(
            path: path, startSeconds: startSeconds,
            onPlaybackReady: onPlaybackReady, vod: vod, close: close)
        // 嵌入式播放器不一定触发全屏控制器的 viewDidAppear；创建时先绑定 Agent 工具。
        if let previous = MaiFfplayMobilePlayer.active, previous !== controller {
            previous.stop()
        }
        MaiFfplayMobilePlayer.active = controller
        return controller
    }

    func updateUIViewController(_ controller: MaiFfplayVideoController, context _: Context) {
    }

    static func dismantleUIViewController(_ controller: MaiFfplayVideoController,
                                          coordinator _: Void) {
        controller.stop()
    }
}
