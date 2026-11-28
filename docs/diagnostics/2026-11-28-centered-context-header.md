# 创作中心上下文居中

用户要求去掉顶部固定“创作中心”标题，将进度条和用量文字作为整组居中，
三点菜单保留在右侧，可选视频圈保留在左侧。验证初始进入、切换会话、清空后仍符合布局，
没有修改容量计算或会话标题保存规则。

Mac 供件基于 `6adfe4c02`，ZIP 314648 字节，SHA256
`12c76cb4f3508cca35a824756099a03b3a4ba8bd208dfcce414c5ca83cc07840`。
Root 独立核两路径前后大小/SHA、before 与 HEAD 原字节相等，
只 apply `header-only.patch` 到已有工具结果显示 WIP；未使用整份 post 覆盖。
另更新一条切换会话时遗留的固定标题注释。

布局用常驻等宽两翼及等 stretch 居中摘要；视频圈隐藏也不回收左翼宽度。
Mac 正常 Qt 组件构建及四个定点 slot 通过；1100/780 宽、视频圈显隐四组合中心误差
不超过 1px，菜单无重叠。Root 看过原始 owned header PNG，长数字完整无裁切。
原 context 夹具硬编码旧日志名，现用 `maiCurrentSessionDatabasePath` 选权威日志，
保留原损坏与缓存失效断言，不修改 Core。

原始包、验证摘要及四张 PNG 在 `.cache/steer-input-20261128/mac-header-delta/`。
此项与工具 receipt 的新显示共同进入 Root 桌面增量构建和定点回归，
尚未替换用户现装的正式 4.0.2。

Root 合并后正常 App/组件构建通过，Windows 同一测试包装器下 Header 四个 slot
及 receipt/懒加载/连续工具回归合计 13 passed、0 failed；四种几何组合原断言保留。
Root 再次查看 Windows 780+circle 和 receipt 原 PNG，中心与菜单边界、输出换行正常。
实际截图在 `images-final-header/`，测试输出在 `final-header-ui.log` 及正常构建目录的 QtTest 文件。
没有声称覆盖更窄窗口、所有缩放比例或移动平台的同样布局。
