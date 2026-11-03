// 测试宿主与素材仅供 Debug 使用，正式页面不在本文件实现。
#if DEBUG
import Foundation
import QuartzCore

#if DEBUG
/// 定点性能记录仅由环境变量显式启用；只记录计数/时间，不记录会话、消息或媒体内容。
@MainActor
enum AISessionDrawerDiagnostics {
    private static let enabled = ProcessInfo.processInfo.environment["MAICHAT_DRAWER_PERF_TRACE"] == "1"
    private static var phase: String?
    private static var started: CFTimeInterval = 0
    private static var updates = 0
    private static var rows = 0
    private static var lists = 0
    private static var focusDismissals = 0

    static func begin(_ name: String) {
        guard enabled else { return }
        finish()
        phase = name
        started = CACurrentMediaTime()
        updates = 0
        rows = 0
        lists = 0
        focusDismissals = 0
    }

    static func finish() {
        guard enabled, let phase else { return }
        NSLog("[SessionDrawerPerf] phase=%@ elapsed_ms=%.1f updates=%d rows=%d lists=%d focus_dismissals=%d",
              phase, (CACurrentMediaTime() - started) * 1000,
              updates, rows, lists, focusDismissals)
        self.phase = nil
    }

    static func update() { if enabled && phase != nil { updates += 1 } }
    static func rowBody() { if enabled && phase != nil { rows += 1 } }
    static func listBody() { if enabled && phase != nil { lists += 1 } }
    static func dismissFocus() { if enabled && phase != nil { focusDismissals += 1 } }
    static func menuTap(blocked: Bool, shown: Bool) {
        if enabled {
            NSLog("[SessionDrawerPerf] menu-tap blocked=%d shown=%d", blocked ? 1 : 0, shown ? 1 : 0)
        }
    }
}
#endif

#endif
