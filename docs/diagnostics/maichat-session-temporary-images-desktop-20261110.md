# Desktop Session 临时图片发布与读取验证

## 操作与预期

同账号设备打开 Desktop 所属 Session。Desktop 新增 user 图片，或 assistant 工具返回图片时，保留原消息的 text / image / text 顺序；首次发布图片先发送 `remoteImage.state=pending`，服务端确认后才执行临时 OSS 上传。完成后只更新原图片 part 的固定引用。启动历史同步和翻页不把旧图片加入上传队列。

图片读取使用完整的原 owner login / slot / Session / message / part / attachment 身份；所属设备离线或重新登录不影响有效引用。服务端从首次预留起固定保留 24 小时。完整本地缓存可在过期后继续显示；没有缓存的过期引用直接显示过期，不能发送 HTTP / WS 图片请求，也不能重新上传。

## 环境与边界

- 时间：2026-11-10，Asia/Shanghai。
- 源码基线：`28977e7a42e5bbb680505e5aa7c8aef9ebe0b240`，附带当前工作区改动。
- 实际测试平台：Windows 11、Qt 6.12.0、MSVC 2022。
- Desktop 构建：`MaiChat/desktop/.cache/build-qt6-ui-20261109/bin/desktop_session_sync_host_test.exe`，通过仓库 `scripts/run-tests-windows.ps1` 提供 Qt DLL PATH 和非模态错误处理。
- HTTP / OSS 均为临时 localhost TLS fixture；测试凭据为 fixture 固定值。未读取用户账号、模型凭据或用户数据库，没有付费模型、真实 OSS、线上部署或生命周期规则修改。
- 本记录验证共享传输实现与实际 Qt 测试构建中的读取界面。尚未完成真实账号跨设备和真实 OSS 的正常 App 验收，不能将其描述为实机业务验收通过。

## 实现约束

`DesktopSessionAttachments` 在账号独立的目录保存原 owner 身份及 terminal receipt，使用 `QSaveFile` 原子提交。重复历史序列化只读取 receipt。`DesktopSessionSyncCoordinator` 只对观察到的 live message IDs enrollment，并在已有的二十条上限精读处再次检查延后追加的工具图片；catalog 和 backfill 不 enrollment。在 pending publish 的成功回调后调用普通上传 API；失败、过期和已完成身份不重新创建。共享 `MaiCloudDriveClient` 使用已有 Multipart 路径，添加 temporary purpose 和 message / part 绑定；遇到 temporary 410 保留 checkpoint，不进入普通附件的重新初始化分支。

`MAI_SESSION_IMAGE_DOWNLOAD` 先校验固定期限，再请求完整身份的 signed read，核对所有绑定和精确字节数，最后通过 `mai_download_new_file` 发布完整文件。平台缓存包含账号、原 owner login、slot、Session、message、part 和 attachment ID；文件路径、signed URL、OSS key、token、图片字节不进入同步消息。Desktop 界面在同一图片 widget 上更新状态，读取和普通图片解码在 worker 上执行，账号、房间和 receipt 变化会取消并拒绝旧回调。

## 已保留的证据

- 共享 Network 行为 RED：`.cache/remote-session-temp-images-20261110/network-temporary-red.txt`。相同 focused 测试链接 `git show HEAD` 的旧实现时因未发送 temporary purpose 失败；编译和链接成功，退出码 1。
- 共享 Network focused GREEN：`network-temporary-green.txt`，退出码 0。覆盖 purpose / 全身份、原 login / part checkpoint 隔离、本地过期零请求、错误 part / login / room / expiry / size 的拒绝、短文件不提交、取消、服务端 410 不重建。
- 原有持久云盘与 Session 附件 TLS 回归：`network-persistent-regression.txt`，退出码 0。
- Desktop 原渲染行为 RED：`desktop-render-red.txt`。原实现把 text / image / text 合为一行 legacy 占位；目标断言要求三行，实际一行。
- Desktop 历史与状态回归：`desktop-host-green-full-run.txt`，注册的 Host 测试通过，23.60 秒。新增测试覆盖账号 journal terminal 状态、pending ACK 前不启动上传、启动前的历史图片不 enrollment，以及稳定图片 widget。
- 后续完整 Host 回归：`desktop-image-final-full.txt`，79 通过 / 0 失败 / 1 跳过，21516 ms。跳过的 localhost TLS 用例通过专用 runner 单独执行。新增文件写入使用 Qt test mode；只清理由本次 UUID 账号生成且位于测试根目录内的 journal 和缓存目录。
- Desktop 实际 TLS 读取：`desktop-local-tls-run.txt`、`desktop-remote-image-local-tls.qtest.txt`；首次结果 3 通过 / 0 失败 / 0 跳过，5272 ms。runner 校验恰好一次初始化和一次 signed read，覆盖 owner 离线与重新登录、实际短期限过后重开界面仍读本地缓存、另一账号无缓存时过期零请求。fixture 的 5 秒期限仅用于测试，不修改产品固定 24 小时策略。

## 渲染排障

人工检查 `desktop-remote-image-local-tls.png` 发现 80×40 的 fixture 画成 105×33，原始 pixmap 尺寸断言不能发现布局变形。增加对实际 widget 截图中蓝色像素边界的 2:1 比例断言后，`desktop-aspect-red-run.txt` 记录真实失败：卡片仍是 loading 时的 90×42。

根因是 `MarkdownView` 自行管理嵌入 widget 的几何；`QLabel::updateGeometry()` 只通知 viewport，图片完成时没有触发该行的高度重算。完成回调在保持原 widget 的前提下显式发送 `LayoutRequest`，让已有过滤器按图片比例重新测量。`desktop-aspect-green.qtest.txt` 为 3 通过 / 0 失败 / 0 跳过，5277 ms；人工查看并统计 `desktop-aspect-green.png` 的蓝色区域为 120×60，保持 2:1。

## Assistant 工具图片信号

源码审查确认 `MaiTurnRunner` 在工具完成事件之后追加 `resultImages`，不能只按工具 part ID 查找图片。使用真实 Agent / Controller 信号的 scripted model 与 `successWithImages` 工具验证：第二次模型响应持续运行时，图片也必须首先发布 pending。`assistant-signal-red.txt` 为 2 通过 / 1 失败 / 0 跳过，9260 ms，失败是持续运行期间未出现 pending 图片。修复全消息 live 检查和 deferred point read 后，该用例在 `desktop-image-final-full.txt` 中通过；没有修改 Core 事件或扫描 catalog / backfill。

## Signed read 临近到期的传输延迟

独立服务端对接复核发现：服务端返回的 `read_expires_in` 从签发时起算，不能再与 HTTP 回包到达后的剩余时间直接比较。此前有效的临近到期回包会被误判为 `INVALID_RESPONSE`。以固定 4 秒期限和 1.2 秒控制面延迟复现，`network-latency-red.txt` 记录退出码 1。

修复后相对 TTL 与请求开始时的剩余期限核对，回包时仍要求原始绝对 `expires_at` 精确匹配且未过期。`network-latency-green.txt` 退出码 0；其余 focused 临时图片负例再次通过。最终共享头文件 SHA256 为 `0BAEA84A65E806A508E3E435F7BB9F7E8362BDF76D712C1924CBBFEBFBB3877B`，实现为 `BF18159818F58DD565D5EDF9B28C5CEFC99C01686552F9F74992DAEE9B2F8D3B`。

## 图片预览

Desktop 缩略图点击必须打开已有 `ImagePreviewDialog`。`desktop-preview-red.qtest.txt` 记录真实点击后未出现预览，2 通过 / 1 失败 / 0 跳过，5037 ms。

实现通过已提交的本地完整缓存路径在 worker 上调用普通图片解码器，并创建 panel 持有的非模态 `ImagePreviewDialog`。预览不签名、不下载、不上传；账号、房间、receipt 或所属行变化会关闭预览并取消旧解码。

`desktop-preview-green.qtest.txt` 为 3 通过 / 0 失败 / 0 跳过，5279 ms；实际 Qt 点击后检查预览已显示、非模态且保留 80×40 原图。runner 再次确认只有一次对象初始化、一次 signed read。人工查看 `desktop-preview-green.png` 的缩略图比例正常。`desktop-preview-green-build.txt` 记录正常 App 与 Host 测试构建成功、114 个 PE 依赖检查通过。该 Host 测试产物 SHA256 为 `58787AAD8AE8602259D4004C4E8B5743894046496DB17C04FA613D66505412EB`。
