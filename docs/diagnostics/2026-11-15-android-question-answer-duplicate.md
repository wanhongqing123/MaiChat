# Android 问题答案重复显示与图片恢复提示

## 现场和复现要求

用户提供 Android 创作中心照片，明确两项：同一句用户回答显示两次；去掉图片卡片
“从所属设备重新加载”提示。预期：一次回答只出现一个用户气泡，真实重复发送仍分别保留；
图片继续正常同步和预览，不暴露来源设备重发机制的额外按钮。

基线为 Android 2.2.42 / versionCode 284，正常安装进程观察为 PID 15489。
工程基线 `e2482c54`；原 MobileAgentAdapter 未提交工作不进入构建。
本次没有把照片内聊天气泡中的“可以”当成 Windows 更新或服务部署授权。

## 现场证据

只读取已展示的 Android controller/page 字段，输出匿名消息 ID、正文摘要、长度和角色，
不调用目标方法、不访问用户数据库、不输出正文、令牌或来源 URL。

当前页原始 messages 共 16 条，messageDisplayUpdates、questionMessages、questions、
questionAnswerEchoes 为空，outgoing 队列也为空；原始消息没有相同 ID 或相同普通正文。
其中第 7 条 assistant 包含已完成的 question 工具，output 长度 24；紧接第 8 条真实 user
消息的正文摘要与此 output 完全相等。由此定位到显示投影，而不是相同消息被存储两份。

`AIAssistantQuestionConversation.parts()` 原先无条件将已完成 question.output 生成为
`question_answer` 用户气泡；后面的真实 user message 又画一次。照片中先在 assistant
消息内部出现答案、随后在其 footer 下方再次出现同文，符合这个路径。

第一次深层只读 JDI 遍历暂停 VM 耗时 18.315 秒，超出预期；finally 已 resume/dispose，
端口转发已移除。后续改为读取不可变 page 编码字符串，不再暂停 VM，定点问题摘要探针
763ms 完成并断开。不得把第一次耗时描述为短暂停顿，也不再用冻结 VM 的广泛字段遍历。

随后在正常 284 手机 UI 滚到照片同处，再次实际复现两条气泡。hierarchy 中相同 24 字摘要
的 TextView 为 2，bounds 分别为 `[104,375][672,503]` 与 `[128,652][648,740]`；截图已人工查看。
`before-question-10.png/xml/-bounds.json` 保存在任务证据目录，不把当前滚动窗口外的计数 0 当成修复。

## 同例失败与修复

普通 JUnit 两个新基线用例实际 2/2 失败：一次问题回答应显示 1 个气泡，实际为 2；
两次真实用户发送相同答案应保留 2 个气泡，基线连同问题回声显示了 3 个。
日志 `question-answer-duplicate-red-*`，没有用模型重复发消息来复现。

修复仅位于显示合流：同 session、紧邻的真实纯文本 user message 与前一片段最后一个
可见 question_answer 精确等文时，在复制的显示 part 上标记答案已有真实用户消息承载。
不改原 messages、question.output、消息 ID、分页或模型历史，不按正文对真实消息去重。
跨 session、中间有 assistant 回应、两个真实同文用户消息、ACK 到完成快照的过渡、
同 assistant 的后续分段均有独立边界用例。另有真实 Panel/ListView 布局断言，要求可见
答案 TextView 只有一个，不仅检查中间 JSONArray。

独立静态 review 补出尾 reasoning 元数据边界：Panel 不显示 reasoning，原候选却把它作为
最后可见 part，仍会留重复答案。新普通用例实际 1/1 RED；同步按 Panel 可见性跳过 reasoning
后，该边界及原组全绿。没有把推测直接当实跑失败或跳过该边界。

## 图片提示调整

原按钮缺陷也有普通 Robolectric 基线失败：会话中仍能找到“从所属设备重新加载”按钮。
现在去掉该按钮和“正在从所属设备重新加载…”文案。已有图片下载/缓存/预览链不改；
恢复请求保留在图片占位卡点击上，同一个请求进行时不可重复触发，完成后清除旧重试监听器，
保留正常图片预览。离线时不显示设备实现细节，不自动发新的源图片发布请求。

原图片生命周期测试同步改为点击占位卡，继续验证单 part 请求、同容器、缓存、离线、
重复点击合并、失败后再次点击、换 selection 后的迟回调与草稿/抽屉保持。

## 验证与安装边界

普通 JUnit/Robolectric 在既有标准 OpenJDK 17 下验证，不初始化 JNI/模型或真实账号传输。
最终受影响 8 组 125/0，31.507 秒：QuestionConversation 26、Drawer 26、RemoteImage 21、
RemoteSessionSync 21、MessagePages 14、MediaCardLayout 9、ImageMessageRow 6、ImageThumbnail 2。
一轮批量选择器误写 MediaCard 类名而只覆盖其余 115 条，随后单独补实际 Layout 9 条；最后已用
正确 8 个类名同跑 125 条，最终 XML 逐组归档，未把未执行类当作已通过。
正常 APK 与手机验收按后续实际结果补记。
目标安装为同 applicationId / buildType / 签名的正常 285 / 2.2.43 覆盖更新，不卸载或清数据。
用户此前明确授予的 Android 正常更新授权继续适用，不再次请求手机安装确认。
Windows 播放器替换及服务端 R2 部署仍是各自独立的待授权动作。
