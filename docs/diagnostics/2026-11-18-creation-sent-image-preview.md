# 创作中心发送方图片点击放大

用户报告发送方缩略图无法像其他图片一样点击放大。先按自己发出的图片检查并询问平台；桌面普通本机会话路径已有决定性缺口。base3d5f9c0aa2ef057f3a96617af811ca30478b9761，协调端授予AgentChatPanel的appendUserImages/必要私有连接和一个对应测试hunk，本轮不改传输、解码、存储根、输入核心或字体。

## 复现和原因

普通MIME拖入自有960×720 PNG，发送给无网络的CapturingModel，缩略图正常出现。appendUserImages原来仅创建QLabel，既没有点击/键盘激活，也没有接MessageImageLoader和ImagePreviewDialog。assistant产物及草稿图片已有该预览入口。实时受理和冷历史都调用同一个appendUserImages，因此两者同时受影响。

新增普通面板用例通过窗口坐标点击实际缩略图像素，而不是只检查新增对象名；旧生产对象实际RED，live.opened=false。基线最后同例执行5.649秒，Qt记录1个业务失败；init/cleanup不算业务用例。

## 最窄修复

发送方仍以相同尺寸/比例在阅读列右侧显示；仅把图片放进原生QPushButton点击区，子QLabel继续绘制原缩略图并让鼠标命中按钮。保留文本、引用、消息id和图像顺序。按钮有键盘焦点和只含文件名的无障碍名称。

私有连接复用同一个有界异步MessageImageLoader和现有ImagePreviewDialog；从当前屏幕目标大小请求更大显示数据，不把小缩略图当原图，不新增解码器或下载。结果持有panel/session和实际消息item绑定，页面或消息已失效的迟到结果不弹入别的会话；不把滚动导致的暂时隐藏误当消息失效。对话框由panel拥有并在关闭时回收；打开、关闭不删除消息持久引用的文件。源丢失或损坏则在原会话显示明确提示，消息和缩略图仍保留。

## 实际验证

同一用例候选转绿，覆盖：普通发送后点击；Esc关闭及回到原页面；删掉原始导入文件后，仍使用保留的工作区副本；销毁并重建Controller/Panel后从实际自有SQLite历史点击；Space打开及Esc关闭；明确删除本测试保留副本后提示失败且不打开空预览。大图sourceImage为960×720且像素同原样本，缩略图仅280×210，防止直接把小图拉伸冒充来源。完整页面、大图、历史/键盘以及失败截图已亲看。

最终候选生产对象编译0/6.73秒；测试以最终未改动的类声明重新编译0/7.292秒，正常消费者链接参数下链接0/1.033秒，最终执行0/3.642秒。Qt总3/0包含init/cleanup，实际业务用例1项，内部覆盖上述流程。之前一次候选测试关闭时序超时75秒被保留；诊断中加日志会通过，不因此宣称原测试稳定。最终测试等待QThread的modal loop层级真正增加后才发送Esc，避免Cocoa show过程中抢先accept，最后同源负对照仍失败、候选通过。

生产只改AgentChatPanel.cpp；类声明最终不变。原私有CMake缓存的Qt源码Git元数据已在前任务证明不可再生。本轮只复用该已验SDK的普通编译/链接参数，测试MOC来自当前完整源；目标未构建时缺失的空外部autogen对象不参与私有链接，测试自己的included MOC完整生成。private UI消费者只替换复制归档中的AgentChatPanel.cpp.o，其余归档成员逐SHA不变。未改SDK/stamp，也不称完整App打包或现装验证。

## 边界与证据

私有证据 `/private/tmp/maichat-creation-sent-image-preview-20261118` 包括精确前像、normal-commands、baseline-final-click、accepted-native-ui、archive-scope和所有截图。使用自有图和临时数据库，经普通MIME事件而非系统剪贴板，不读用户媒体/消息/凭据，不触发付费模型、不装App。iOS相应图片已有AIWorkspaceImage的Button/openImage路径；本轮只验证桌面共享界面，不把本机结果称为iOS/Android或跨设备显示验收。

后继DesktopComputer新ABI头只验接收SHA后排队，未混入本轮旧normal消费者。主线由协调端串行聚合，发布、安装和后台服务不在本轮范围。
