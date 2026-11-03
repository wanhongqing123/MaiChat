// 本地图片和视频共用一个持续存在的分页容器。拖动时保留相邻两页，
// 提交后以整页位移完成切换；视频只在提交时交接 FFplay，取消拖动不打断播放。
import AVFoundation
import SwiftUI
import UIKit

// 只在模拟器界面回归记录宿主生命周期，不记录文件路径、账号或媒体内容。
@MainActor
func maiLocalMediaTrace(_ event: String) {
    #if targetEnvironment(simulator)
    if ProcessInfo.processInfo.arguments.contains("--ai-ui-test") { NSLog("[MediaPaging] %@", event) }
    #endif
}

@MainActor
struct MaiLocalMediaPager: UIViewControllerRepresentable {
    let sequence: MaiMediaSequence
    var startSeconds: Double = 0
    var vod: MaiCloudVodSession? = nil
    var imageIdentifier = "gallery-image-preview"
    var showsImageActions = true
    var isModal = true
    var sourceFrame: ((UIView) -> CGRect?)? = nil
    var thumbnail: UIImage? = nil
    var selectionChanged: ((MaiMediaSequence) -> Void)? = nil
    let close: () -> Void

    func makeUIViewController(context: Context) -> MaiLocalMediaPagerController {
        MaiLocalMediaPagerController(sequence: sequence, startSeconds: startSeconds, vod: vod,
            imageIdentifier: imageIdentifier, showsImageActions: showsImageActions,
            sourceFrame: sourceFrame, thumbnail: thumbnail, isModal: isModal, selectionChanged: selectionChanged,
            close: close)
    }

    func updateUIViewController(_ controller: MaiLocalMediaPagerController, context: Context) {
        controller.update(sequence: sequence, sourceFrame: sourceFrame,
                          selectionChanged: selectionChanged, close: close)
        controller.isModal = isModal
    }

    static func dismantleUIViewController(_ controller: MaiLocalMediaPagerController,
                                          coordinator: ()) {
        controller.dispose()
    }
}

@MainActor
final class MaiLocalMediaPagerController: UIViewController, UIGestureRecognizerDelegate {
    private var sequence: MaiMediaSequence
    private let initialItem: MaiMediaItem
    private let startSeconds: Double
    private let vod: MaiCloudVodSession?
    private let imageIdentifier: String
    private let showsImageActions: Bool
    var isModal: Bool {
        didSet { viewIfLoaded?.accessibilityViewIsModal = isModal }
    }
    private var sourceFrame: ((UIView) -> CGRect?)?
    private var selectionChanged: ((MaiMediaSequence) -> Void)?
    private var close: () -> Void
    private var pages: [String: MaiLocalMediaPageController] = [:]
    private var current: MaiLocalMediaPageController!
    private var incoming: MaiLocalMediaPageController?
    private var target: MaiMediaSequence?
    private var direction = 0
    private var offset: CGFloat = 0
    private var animator: UIViewPropertyAnimator?
    private var waitingToCommit = false
    private var closing = false
    private var disposed = false
    private var appeared = false
    private var opened = false
    private var opening = false
    private var speed = 1.0
    private lazy var pan = UIPanGestureRecognizer(target: self, action: #selector(dragged(_:)))
    private let closeButton = UIButton(type: .system)

    init(sequence: MaiMediaSequence, startSeconds: Double = 0, vod: MaiCloudVodSession? = nil,
         imageIdentifier: String = "gallery-image-preview", showsImageActions: Bool = true,
         sourceFrame: ((UIView) -> CGRect?)? = nil, thumbnail: UIImage? = nil, isModal: Bool = true,
         selectionChanged: ((MaiMediaSequence) -> Void)? = nil, close: @escaping () -> Void) {
        self.sequence = sequence
        initialItem = sequence.current
        self.startSeconds = startSeconds
        self.vod = vod
        self.imageIdentifier = imageIdentifier
        self.showsImageActions = showsImageActions
        self.isModal = isModal
        self.sourceFrame = sourceFrame
        self.selectionChanged = selectionChanged
        self.close = close
        super.init(nibName: nil, bundle: nil)
        current = page(for: sequence.current, thumbnail: thumbnail)
    }

    required init?(coder: NSCoder) { fatalError("init(coder:) has not been implemented") }
    override var prefersStatusBarHidden: Bool { true }
    override var prefersHomeIndicatorAutoHidden: Bool { true }
    override func accessibilityPerformEscape() -> Bool { requestClose(); return true }

    override func viewDidLoad() {
        super.viewDidLoad()
        view.backgroundColor = .clear
        view.clipsToBounds = true
        view.accessibilityIdentifier = "local-media-pager"
        // 全屏宿主同时隔离无障碍焦点；分屏宿主仍允许操作下面的聊天输入和导航。
        view.accessibilityViewIsModal = isModal
        attach(current)
        current.view.frame = view.bounds
        pan.delegate = self
        pan.maximumNumberOfTouches = 1
        view.addGestureRecognizer(pan)
        closeButton.setImage(UIImage(systemName: "xmark"), for: .normal)
        closeButton.tintColor = .white
        closeButton.backgroundColor = UIColor.white.withAlphaComponent(0.2)
        closeButton.layer.cornerRadius = 25
        closeButton.accessibilityIdentifier = "ffplay-close"
        closeButton.accessibilityLabel = "关闭视频"
        closeButton.addTarget(self, action: #selector(closeTapped), for: .touchUpInside)
        view.addSubview(closeButton)
        refreshControls()
    }

    override func viewDidAppear(_ animated: Bool) {
        super.viewDidAppear(animated)
        appeared = true
        if current.item.kind == .video {
            opened = true
            current.activateVideo(startSeconds: startSeconds, vod: vod)
        } else { openImageIfReady() }
        prewarmNeighbors()
    }

    override func viewDidLayoutSubviews() {
        super.viewDidLayoutSubviews()
        guard animator == nil else { return }
        layoutPages()
        closeButton.frame = CGRect(x: view.safeAreaInsets.left + 18,
            y: view.safeAreaInsets.top + 12, width: 50, height: 50)
    }

    override func viewWillTransition(to size: CGSize,
                                    with coordinator: UIViewControllerTransitionCoordinator) {
        // 旋转只结束正在进行的手势，不重建当前页或重播视频。
        if let animator { animator.stopAnimation(false); animator.finishAnimation(at: .end) }
        else { cancelDragImmediately() }
        super.viewWillTransition(to: size, with: coordinator)
    }

    func update(sequence new: MaiMediaSequence, sourceFrame: ((UIView) -> CGRect?)?,
                selectionChanged: ((MaiMediaSequence) -> Void)?, close: @escaping () -> Void) {
        guard !disposed else { return }
        self.sourceFrame = sourceFrame
        self.selectionChanged = selectionChanged
        self.close = close
        // 历史媒体异步补齐时保留用户当前项和控制器，不能回到初始选择或重新播放。
        sequence = MaiMediaSequence(selected: sequence.current, items: new.items,
                                    groupID: sequence.groupID)
        if target == nil { prewarmNeighbors() }
    }

    private func key(_ item: MaiMediaItem) -> String { "\(item.kind):\(item.path)" }

    private func page(for item: MaiMediaItem, thumbnail: UIImage? = nil) -> MaiLocalMediaPageController {
        if let page = pages[key(item)] { return page }
        let page = MaiLocalMediaPageController(item: item, imageIdentifier: imageIdentifier,
                                              showsImageActions: showsImageActions, thumbnail: thumbnail)
        page.close = { [weak self] in self?.requestClose() }
        page.readyChanged = { [weak self, weak page] in
            guard let self, let page, !self.disposed else { return }
            if page === self.current { self.openImageIfReady() }
            if page === self.incoming && self.waitingToCommit { self.finishDrag(commit: true) }
        }
        pages[key(item)] = page
        page.loadViewIfNeeded()
        return page
    }

    private func attach(_ page: MaiLocalMediaPageController) {
        guard page.parent == nil else { return }
        addChild(page)
        view.insertSubview(page.view, at: 0)
        page.didMove(toParent: self)
    }

    private func detach(_ page: MaiLocalMediaPageController) {
        guard page.parent === self else { return }
        page.willMove(toParent: nil)
        page.view.removeFromSuperview()
        page.removeFromParent()
    }

    private func prewarmNeighbors() {
        guard isViewLoaded, !disposed else { return }
        var retained = Set([key(sequence.current)])
        for delta in [-1, 1] {
            if let next = sequence.adjacent(delta) {
                retained.insert(key(next.current))
                _ = page(for: next.current)
            }
        }
        // 只保留当前页及前后各一页；邻居视频只有封面，不创建后台播放器或占用音频。
        for itemKey in Array(pages.keys) where !retained.contains(itemKey) {
            if let page = pages.removeValue(forKey: itemKey) { detach(page); page.dispose() }
        }
    }

    private func layoutPages() {
        let bounds = view.bounds
        current.view.frame = bounds.offsetBy(dx: offset, dy: 0)
        incoming?.view.frame = bounds.offsetBy(dx: offset + CGFloat(direction) * bounds.width, dy: 0)
    }

    func gestureRecognizerShouldBegin(_ gestureRecognizer: UIGestureRecognizer) -> Bool {
        guard gestureRecognizer === pan, animator == nil, !waitingToCommit,
              !closing, !opening, !current.isZoomed, !current.controlsInteracting else { return false }
        let velocity = pan.velocity(in: view)
        return abs(velocity.x) > abs(velocity.y) * 1.25
    }

    func gestureRecognizer(_ gestureRecognizer: UIGestureRecognizer, shouldReceive touch: UITouch) -> Bool {
        var touched = touch.view
        while let node = touched, node !== view {
            if node is UIControl { return false }
            touched = node.superview
        }
        return true
    }

    @objc private func dragged(_ gesture: UIPanGestureRecognizer) {
        switch gesture.state {
        case .began, .changed:
            let translation = gesture.translation(in: view).x
            let requested = translation < 0 ? 1 : -1
            if requested != direction {
                if let incoming { detach(incoming) }
                incoming = nil
                direction = requested
                target = sequence.adjacent(requested)
                if let target {
                    let next = page(for: target.current)
                    next.view.accessibilityElementsHidden = true
                    incoming = next
                    attach(next)
                }
            }
            offset = target == nil ? translation * 0.24 :
                max(-view.bounds.width, min(view.bounds.width, translation))
            layoutPages()
        case .ended:
            let velocity = gesture.velocity(in: view).x * CGFloat(-direction)
            let enoughDistance = abs(offset) >= view.bounds.width * 0.23
            let flick = velocity > 420 && abs(offset) > 24
            finishDrag(commit: target != nil && (enoughDistance || flick))
        case .cancelled, .failed: finishDrag(commit: false)
        default: break
        }
    }

    private func finishDrag(commit: Bool) {
        guard animator == nil, !disposed else { return }
        if commit, let incoming, !incoming.readyForTransition {
            // 冷图像仍在同一邻页中加载，原页保持可见；准备完成后再提交，避免整屏空白。
            waitingToCommit = true
            return
        }
        waitingToCommit = false
        let previous = current!
        let next = incoming
        let selected = target
        if commit, let next {
            if let previousSpeed = previous.videoSpeed { speed = previousSpeed }
            // 停止旧视频解码但保留 Metal 最后一帧和视图，直到它完整滑出。
            previous.freezeVideo()
            if next.item.kind == .video { next.activateVideo(startSeconds: 0, vod: nil, speed: speed) }
        }
        let distance = CGFloat(direction) * view.bounds.width
        let animation = UIViewPropertyAnimator(duration: commit ? 0.50 : 0.40,
            controlPoint1: CGPoint(x: 0.22, y: 0.72), controlPoint2: CGPoint(x: 0.30, y: 1)) {
            previous.view.frame.origin.x = commit ? -distance : 0
            next?.view.frame.origin.x = commit ? 0 : distance
        }
        animator = animation
        animation.addCompletion { [weak self] _ in
            guard let self, !self.disposed else { return }
            self.animator = nil
            if commit, let next, let selected {
                self.detach(previous)
                previous.stopVideo()
                self.current = next
                self.sequence = selected
                previous.view.accessibilityElementsHidden = true
                next.view.accessibilityElementsHidden = false
            } else if let next { self.detach(next) }
            self.incoming = nil
            self.target = nil
            self.direction = 0
            self.offset = 0
            self.layoutPages()
            self.refreshControls()
            self.prewarmNeighbors()
            if commit { self.selectionChanged?(self.sequence) }
        }
        animation.startAnimation()
    }

    private func cancelDragImmediately() {
        if let incoming { detach(incoming) }
        incoming = nil
        target = nil
        waitingToCommit = false
        direction = 0
        offset = 0
        if isViewLoaded { layoutPages() }
    }

    private func refreshControls() {
        closeButton.isHidden = sequence.current.kind != .video
    }

    private func openImageIfReady() {
        guard appeared, !opened, current.item.kind == .image,
              let image = current.image else { return }
        opened = true
        guard let source = sourceFrame?(view), !source.isEmpty else { return }
        opening = true
        current.setImageHidden(true)
        current.view.alpha = 0
        let floating = UIImageView(image: image)
        floating.contentMode = .scaleAspectFit
        floating.clipsToBounds = true
        floating.layer.cornerRadius = 8
        floating.frame = source
        view.addSubview(floating)
        UIView.animate(withDuration: 0.38, delay: 0, options: [.curveEaseInOut]) {
            self.current.view.alpha = 1
            floating.frame = Self.fitted(image.size, in: self.view.bounds)
            floating.layer.cornerRadius = 0
        } completion: { [weak self] _ in
            floating.removeFromSuperview()
            self?.current.setImageHidden(false)
            self?.opening = false
        }
    }

    @objc private func closeTapped() { requestClose() }

    func requestClose() {
        guard !closing, animator == nil, !opening else { return }
        maiLocalMediaTrace("close-start")
        closing = true
        cancelDragImmediately()
        if sequence.current == initialItem, current.item.kind == .image,
           let image = current.image, let source = sourceFrame?(view), !source.isEmpty {
            current.setImageHidden(true)
            let floating = UIImageView(image: image)
            floating.contentMode = .scaleAspectFit
            floating.clipsToBounds = true
            floating.frame = Self.fitted(image.size, in: view.bounds)
            view.addSubview(floating)
            UIView.animate(withDuration: 0.38, delay: 0, options: [.curveEaseInOut]) {
                self.current.view.alpha = 0
                floating.frame = source
                floating.layer.cornerRadius = 8
            } completion: { [weak self] _ in
                maiLocalMediaTrace("close-animation-complete")
                self?.close()
            }
        } else {
            UIView.animate(withDuration: 0.22, animations: { self.view.alpha = 0 }) {
                [weak self] _ in
                maiLocalMediaTrace("close-fade-complete")
                self?.close()
            }
        }
    }

    static func fitted(_ size: CGSize, in bounds: CGRect) -> CGRect {
        guard size.width > 0, size.height > 0 else { return bounds }
        let scale = min(bounds.width / size.width, bounds.height / size.height)
        let fitted = CGSize(width: size.width * scale, height: size.height * scale)
        return CGRect(x: bounds.midX - fitted.width / 2, y: bounds.midY - fitted.height / 2,
                      width: fitted.width, height: fitted.height)
    }

    func dispose() {
        guard !disposed else { return }
        disposed = true
        maiLocalMediaTrace("disposed")
        // SwiftUI 可能在关闭后暂时保留 UIViewController，立即释放模态焦点和触摸隔离。
        viewIfLoaded?.accessibilityViewIsModal = false
        viewIfLoaded?.isUserInteractionEnabled = false
        animator?.stopAnimation(true)
        animator = nil
        for page in pages.values { detach(page); page.dispose() }
        pages.removeAll()
    }
}

// 原图适配屏幕时将横向拖动交给外层分页；放大后拖动只平移当前图片。
private final class MaiLocalMediaZoomScrollView: UIScrollView {
    override func gestureRecognizerShouldBegin(_ gestureRecognizer: UIGestureRecognizer) -> Bool {
        if gestureRecognizer === panGestureRecognizer && zoomScale <= minimumZoomScale + 0.01 {
            return false
        }
        return super.gestureRecognizerShouldBegin(gestureRecognizer)
    }
}

@MainActor
private final class MaiLocalMediaPageController: UIViewController, UIScrollViewDelegate {
    let item: MaiMediaItem
    private let imageIdentifier: String
    private let showsImageActions: Bool
    var close: (() -> Void)?
    var readyChanged: (() -> Void)?
    private let scroll = MaiLocalMediaZoomScrollView()
    private let imageView = UIImageView()
    private let backdrop = UIImageView()
    private let blur = UIVisualEffectView(effect: UIBlurEffect(style: .dark))
    private let spinner = UIActivityIndicatorView(style: .large)
    private let message = UILabel()
    private let retry = UIButton(type: .system)
    private let poster = UIView()
    private let actions = UIStackView()
    private var loading: Task<Void, Never>?
    private var player: MaiFfplayVideoController?
    private var lastSize = CGSize.zero
    private(set) var image: UIImage?
    private(set) var readyForTransition = false
    private(set) var controlsInteracting = false
    var isZoomed: Bool { scroll.zoomScale > 1.01 || scroll.isZooming }

    init(item: MaiMediaItem, imageIdentifier: String, showsImageActions: Bool, thumbnail: UIImage?) {
        self.item = item
        self.imageIdentifier = imageIdentifier
        self.showsImageActions = showsImageActions
        image = thumbnail
        super.init(nibName: nil, bundle: nil)
    }

    required init?(coder: NSCoder) { fatalError("init(coder:) has not been implemented") }

    override func viewDidLoad() {
        super.viewDidLoad()
        view.backgroundColor = .black
        view.clipsToBounds = true
        backdrop.contentMode = .scaleAspectFill
        backdrop.clipsToBounds = true
        view.addSubview(backdrop)
        view.addSubview(blur)
        scroll.delegate = self
        scroll.minimumZoomScale = 1
        scroll.maximumZoomScale = 4
        scroll.bouncesZoom = false
        scroll.showsHorizontalScrollIndicator = false
        scroll.showsVerticalScrollIndicator = false
        scroll.contentInsetAdjustmentBehavior = .never
        scroll.accessibilityIdentifier = "local-media-image-zoom"
        view.addSubview(scroll)
        imageView.contentMode = .scaleAspectFit
        imageView.isAccessibilityElement = item.kind == .image
        imageView.accessibilityLabel = "图片预览"
        imageView.accessibilityValue = URL(fileURLWithPath: item.path).lastPathComponent
        imageView.accessibilityIdentifier = imageIdentifier
        scroll.addSubview(imageView)
        let tap = UITapGestureRecognizer(target: self, action: #selector(imageTapped))
        scroll.addGestureRecognizer(tap)
        let doubleTap = UITapGestureRecognizer(target: self, action: #selector(toggleZoom(_:)))
        doubleTap.numberOfTapsRequired = 2
        scroll.addGestureRecognizer(doubleTap)
        tap.require(toFail: doubleTap)
        poster.backgroundColor = .black
        poster.isHidden = item.kind == .image
        view.addSubview(poster)
        if item.kind == .video { imageView.removeFromSuperview(); poster.addSubview(imageView) }
        spinner.color = .white
        spinner.startAnimating()
        view.addSubview(spinner)
        message.textColor = .white
        message.textAlignment = .center
        message.font = .preferredFont(forTextStyle: .body)
        message.numberOfLines = 3
        view.addSubview(message)
        retry.setTitle("重试", for: .normal)
        retry.tintColor = .white
        retry.isHidden = true
        retry.addTarget(self, action: #selector(loadMedia), for: .touchUpInside)
        view.addSubview(retry)
        if item.kind == .image && showsImageActions { buildActions() }
        if let image { show(image) }
        loadMedia()
    }

    override func viewDidLayoutSubviews() {
        super.viewDidLayoutSubviews()
        backdrop.frame = view.bounds
        blur.frame = view.bounds
        poster.frame = view.bounds
        player?.view.frame = view.bounds
        spinner.center = CGPoint(x: view.bounds.midX, y: view.bounds.midY)
        message.frame = CGRect(x: 24, y: view.bounds.midY - 50, width: view.bounds.width - 48, height: 76)
        retry.frame = CGRect(x: view.bounds.midX - 50, y: view.bounds.midY + 30, width: 100, height: 44)
        actions.frame = CGRect(x: view.bounds.width - view.safeAreaInsets.right - 132,
            y: view.bounds.height - view.safeAreaInsets.bottom - 66, width: 112, height: 48)
        guard lastSize != view.bounds.size else { return }
        lastSize = view.bounds.size
        scroll.frame = view.bounds
        fitImage()
    }

    @objc private func loadMedia() {
        loading?.cancel()
        retry.isHidden = true
        message.text = nil
        loading = Task { @MainActor [weak self] in
            guard let self else { return }
            let url = URL(fileURLWithPath: self.item.path)
            let decoded: UIImage?
            if self.item.kind == .image { decoded = await MaiPreviewImageLoader.image(at: url) }
            else {
                // VOD 显示路径没有本地文件；保持加载反馈，交给真实播放器报告首帧或失败。
                guard FileManager.default.fileExists(atPath: self.item.path) else {
                    self.readyForTransition = true
                    self.readyChanged?()
                    return
                }
                let generator = AVAssetImageGenerator(asset: AVURLAsset(url: url))
                generator.appliesPreferredTrackTransform = true
                generator.maximumSize = CGSize(width: 1536, height: 1536)
                let frame = try? await generator.image(at: .zero)
                decoded = frame.map { UIImage(cgImage: $0.image) }
            }
            guard !Task.isCancelled else { return }
            if let decoded { self.show(decoded) }
            else {
                self.spinner.stopAnimating()
                self.message.text = self.item.kind == .image ? "图片无法显示" : "视频封面暂不可用"
                self.retry.isHidden = false
            }
            // 错误页也是明确状态，允许滑入并重试或滑回，不无限等待媒体准备。
            self.readyForTransition = true
            self.readyChanged?()
        }
    }

    private func show(_ image: UIImage) {
        self.image = image
        imageView.image = image
        backdrop.image = image
        spinner.stopAnimating()
        readyForTransition = true
        fitImage()
    }

    private func fitImage() {
        guard let image else { return }
        if item.kind == .video { imageView.frame = view.bounds; return }
        scroll.setZoomScale(1, animated: false)
        let frame = MaiLocalMediaPagerController.fitted(image.size, in: view.bounds)
        imageView.frame = CGRect(origin: .zero, size: frame.size)
        scroll.contentSize = frame.size
        centerImage()
    }

    private func centerImage() {
        scroll.contentInset = UIEdgeInsets(
            top: max(0, (scroll.bounds.height - scroll.contentSize.height) / 2),
            left: max(0, (scroll.bounds.width - scroll.contentSize.width) / 2), bottom: 0, right: 0)
    }

    func viewForZooming(in scrollView: UIScrollView) -> UIView? { imageView }
    func scrollViewDidZoom(_ scrollView: UIScrollView) {
        centerImage()
        scroll.accessibilityValue = String(format: "%.3f", scroll.zoomScale)
    }
    func setImageHidden(_ hidden: Bool) { scroll.alpha = hidden ? 0 : 1 }
    @objc private func imageTapped() { close?() }

    @objc private func toggleZoom(_ gesture: UITapGestureRecognizer) {
        if isZoomed { scroll.setZoomScale(1, animated: true) }
        else {
            let point = gesture.location(in: imageView)
            let size = CGSize(width: scroll.bounds.width / 2.5, height: scroll.bounds.height / 2.5)
            scroll.zoom(to: CGRect(x: point.x - size.width / 2, y: point.y - size.height / 2,
                                   width: size.width, height: size.height), animated: true)
        }
    }

    var videoSpeed: Double? { player?.speedForPaging }

    func activateVideo(startSeconds: Double, vod: MaiCloudVodSession?, speed: Double = 1) {
        guard item.kind == .video, player == nil else { return }
        let controller = MaiFfplayVideoController(path: item.path, startSeconds: startSeconds, initialSpeed: speed,
            onPlaybackReady: { [weak self] in
                guard let self, self.player != nil else { return }
                self.poster.isHidden = true
                self.spinner.stopAnimating()
                self.message.text = nil
                self.retry.isHidden = true
            }, vod: vod, embeddedInMediaPager: true, close: { [weak self] in self?.close?() })
        controller.controlsInteractionChanged = { [weak self] in self?.controlsInteracting = $0 }
        controller.onPlaybackFailure = { [weak self] _ in
            // 封面不能盖住正式播放器的错误反馈；保持关闭及左右浏览可用。
            self?.poster.isHidden = true
            self?.spinner.stopAnimating()
            self?.message.text = nil
            self?.retry.isHidden = true
        }
        player = controller
        controller.activateForVisibleHost()
        addChild(controller)
        controller.view.frame = view.bounds
        view.insertSubview(controller.view, belowSubview: poster)
        controller.didMove(toParent: self)
    }

    func freezeVideo() { player?.freezeForPaging() }

    func stopVideo() {
        if let player {
            player.stop()
            player.willMove(toParent: nil)
            player.view.removeFromSuperview()
            player.removeFromParent()
        }
        player = nil
        poster.isHidden = item.kind == .image
        controlsInteracting = false
    }

    private func buildActions() {
        actions.axis = .horizontal
        actions.spacing = 12
        actions.distribution = .fillEqually
        for (symbol, label, selector) in [
            ("square.and.arrow.up", "分享图片", #selector(shareImage)),
            ("square.and.arrow.down", "保存图片", #selector(saveImage))
        ] {
            let button = UIButton(type: .system)
            button.setImage(UIImage(systemName: symbol), for: .normal)
            button.tintColor = .white
            button.backgroundColor = UIColor.black.withAlphaComponent(0.56)
            button.layer.cornerRadius = 24
            button.accessibilityLabel = label
            button.addTarget(self, action: selector, for: .touchUpInside)
            actions.addArrangedSubview(button)
        }
        view.addSubview(actions)
    }

    @objc private func shareImage() {
        let sheet = UIActivityViewController(activityItems: [URL(fileURLWithPath: item.path)],
                                             applicationActivities: nil)
        sheet.popoverPresentationController?.sourceView = actions
        sheet.popoverPresentationController?.sourceRect = actions.bounds
        present(sheet, animated: true)
    }

    @objc private func saveImage() {
        Task { @MainActor [weak self] in
            guard let self else { return }
            do {
                guard await MaiPhotoLibraryMediaWriter.requestAddAccess() else {
                    self.message.text = "请允许保存到相册"; return
                }
                try await MaiPhotoLibraryMediaWriter.saveImage(
                    at: URL(fileURLWithPath: self.item.path), createdAt: Date())
                self.message.text = "已保存到相册"
            } catch { self.message.text = error.localizedDescription }
        }
    }

    func dispose() { loading?.cancel(); loading = nil; stopVideo() }
}
