# macOS 设置页复选框与文字对齐

用户截图中，免打扰的方框明显高于“开启”；同页“允许远程控制”也有同类偏差。操作是打开设置页，检查两项的未勾选、勾选、标签点击、空格切换与设置恢复。当前本地main为10fe0c64，已fetch b3fbdb1c；相关MainWindow和资源前像与协调端最新源一致。

## 定位与最窄修正

复用普通MainWindow的设置页消费者，原图确认偏移。独立Cocoa对照同时包含原控件、仅调整indicator位置、layout-widget-rect、Fusion、等价point字体和QSS绘制。前三项及point字体保留偏移；Fusion和QSS绘制居中。这不是整行布局没有垂直居中，不能靠整体移动开关或改变字体基值处理。

只在Q_OS_MACOS为settingsDoNotDisturb、remoteControlToggle提供同一内容框内的indicator绘制。保留原QCheckBox、文本、可点击区域、键盘事件、无障碍属性、连接和持久化。方框仍为16逻辑像素，选中使用现有蓝色视觉和白色矢量勾；资源只加settings-check.svg及其qrc别名。不改Windows控件样式，不调整UiZoom或字体基值。

## 实际验证

相同普通页面私有像素消费者链接旧MainWindow对象时，实际RED：indicator相对中文墨迹中心偏高4.5逻辑像素。链接候选MainWindow对象后，免打扰偏差-0.5、远程控制偏差-1.0逻辑像素，均在1.5以内。未勾选与蓝色白勾截图已亲看。

最终生产对象编译0/8.030秒，消费者对象编译0/9.181秒，正常消费者链接参数下链接0/0.441秒。三项普通消费者通过：个人/设置页导航及两个方框像素、免打扰切换持久化/恢复、最新偏好下的通知/未读保留；另在页面内实际点击“开启”文字并按空格，分别确认toggled及状态恢复。Qt总计5/0包含init/cleanup，实际业务用例3项，执行进程3.713秒、Qt计时2.135秒。

完整CMake候选重建在旧private Qt源码Git元数据检查处失败（git rev-parse报not a git repository），尚未到产品编译；未改SDK源码、Git元数据或sourceState stamp。后继验证复用既有已验Qt消费者的真实编译/链接参数，只在private复制的UI归档中替换MainWindow.cpp.o，新图标用同qrc别名的私有资源注册；不是完整App打包成功。跟踪测试源不变，原SDK库与公开发布资产不变。后续正常包仍需在有效SDK构建环境完成；本轮没有替换用户现装。

## 标准字体数据

同一个Cocoa设置页fixture中，UiZoom=1、preset=standard；请求pixelSize=16、pointSize=-1，屏幕logicalDPI=72、DPR=2。请求族为.AppleSystemUIFont/Noto Sans SC/Microsoft YaHei UI/PingFang SC；实际中文glyph采用PingFang SC 16px，ascent16.9531、descent5.4375，QTextLine高23。这是该fixture的字体选择，不把它称为Windows实际字体、重复DPI或两平台主观大小结论。

## 证据与范围

私有目录 `/private/tmp/maichat-settings-checkbox-align-20261118` 保留before、final、六种对照、源码指纹、编译/链接参数、初始错误目标名/缺私有probe头文件等检查失败和最终结果。所有窗口为自有假账号/临时偏好数据，未读取用户消息、凭据或设置，不新开TCC授权，不动后台服务、播放器色彩或系统亮度。主线由协调端串行聚合本轮三生产文件和本记录。

### 补充：复用普通 main.cpp 字体初始化

私有同页面consumer按原main.cpp注册同14项字体，并设置PreferNoHinting、同UiZoom字体栈与16px标准正文。编译0/8.689s、链接0/0.597s、普通页面/像素/标签点击/Space用例0/2.757s（Qt业务1项，init/cleanup后总3/0）。未见字体注册失败。

此时实际中文glyph变为Noto Sans SC 16px，ascent18.5469、descent4.59375、QTextLine高24，screen72 logicalDPI/DPR2；两indicator相对墨迹中心均-1逻辑px，仍满足原1.5阈值。之前PingFang数据仅为未执行字体注册的QtTest fixture，不作为现装App fallback。

生产3源、原4件packet和57b17b44未改；本次源已核等57b并FF同步，Mac工作区clean，原4件stash备份保留。
