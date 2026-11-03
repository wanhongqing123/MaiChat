// 测试宿主与素材仅供 Debug 使用，正式页面不在本文件实现。
#if DEBUG
import SwiftUI
import UIKit

#if targetEnvironment(simulator)
@MainActor
private final class MaiCloudImagePagerFixture: ObservableObject {
    @Published var loadedCount = ProcessInfo.processInfo.environment[
        "MAICHAT_CLOUD_PAGER_APPEND_TEST"] == "YES" ? 2 : 3
    private var appending = false
    let images = (0..<3).map { index in
        MaiCloudDriveVideo(id: "cloud-image-\(index)", name: "图片\(index + 1).png",
            sizeBytes: 1024, contentType: "image/png", mediaType: "image",
            createdAt: 1_793_181_600_000, folderId: nil)
    }
    var files: [URL] = []
    var failSecond = true

    func loadMore() {
        guard loadedCount == 2, !appending else { return }
        appending = true
        Task { @MainActor in
            try? await Task.sleep(for: .seconds(5))
            loadedCount = 3
        }
    }

    func load(_ item: MaiCloudDriveVideo) async throws -> URL {
        if item.id == images[1].id && failSecond {
            failSecond = false
            throw CocoaError(.fileReadNoPermission)
        }
        let index = images.firstIndex(where: { $0.id == item.id }) ?? 0
        let colors: [UIColor] = [.systemRed, .systemGreen, .systemBlue]
        let pixels = UIGraphicsImageRenderer(size: CGSize(width: 900, height: 1200)).image { context in
            colors[index].setFill()
            context.fill(CGRect(x: 0, y: 0, width: 900, height: 1200))
            ("CLOUD IMAGE \(index + 1)" as NSString).draw(at: CGPoint(x: 100, y: 550),
                withAttributes: [.font: UIFont.boldSystemFont(ofSize: 65),
                                 .foregroundColor: UIColor.white])
        }
        let file = FileManager.default.temporaryDirectory
            .appendingPathComponent("cloud-image-pager-fixture-" + UUID().uuidString + ".png")
        try pixels.pngData()!.write(to: file)
        files.append(file)
        return file
    }
}

struct MaiCloudImagePagerUITestRoot: View {
    @StateObject private var fixture = MaiCloudImagePagerFixture()
    @State private var showing = false
    @State private var cleaned = false

    var body: some View {
        VStack(spacing: 20) {
            Button("打开云盘图片测试") { showing = true }
                .accessibilityIdentifier("cloud-drive-pager-fixture-open")
            if cleaned { Text("云盘图片缓存已清理") }
        }
        .fullScreenCover(isPresented: $showing) {
            MaiCloudDriveMediaPager(items: Array(fixture.images.prefix(fixture.loadedCount)),
                initialIndex: 0, loadImage: { try await fixture.load($0) },
                loadVideo: { _ in throw CancellationError() },
                close: { showing = false }, hasMore: fixture.loadedCount < 3,
                loadMore: { fixture.loadMore() })
                .ignoresSafeArea()
        }
        .onChange(of: showing) { open in
            guard !open else { return }
            Task { @MainActor in
                try? await Task.sleep(for: .milliseconds(500))
                cleaned = !fixture.files.isEmpty && fixture.files.allSatisfy {
                    !FileManager.default.fileExists(atPath: $0.path)
                }
            }
        }
    }
}
#endif
#endif
