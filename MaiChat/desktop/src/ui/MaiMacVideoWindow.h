// 文件作用：观察 AppKit 全屏转场完成通知，不替换 Qt 自己的 NSWindow delegate。
#pragma once

class QWidget;
enum class MaiMacFullscreenPhase { WillEnter, DidEnter, WillExit, DidExit };
using MaiMacFullscreenCallback = void (*)(void*, MaiMacFullscreenPhase);

void* maiObserveMacVideoFullscreen(QWidget* widget, void* context, MaiMacFullscreenCallback callback);
// 与创建同在 GUI 线程；同时取消尚未消费的完成回调，调用方随后可释放 context。
void maiReleaseMacVideoFullscreenObserver(void* observer);

// 显示前将普通播放器留在全屏宿主的Space中；显式进入全屏前撤销辅助窗口资格。
// 已处于原生全屏/退出转场时保持Primary，直到DidExit再恢复普通窗口策略。
void maiPrepareMacVideoWindowState(QWidget* widget, bool fullscreen);
