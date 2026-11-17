# Mac Desktop 3.0.1 发布准备与兼容边界

2026-11-17；本机 macOS 27.2 / arm64。用户授权与 house-maichat 恢复合作并统一发布最新 Desktop；版本由准备阶段 2.2.66 改为 3.0.1。Windows、tag、draft、统一上传窗口与最终发布归协调端；Mac 负责干净构建、目标平台验证及四件资产，尚未上传或发布。Mobile 不参与此次 Desktop 发布。

## 源码与实际构建

原工作区 main c3ea0bbbbb63f09b67329f57b31fd5e6b92c554c 及所有既有 WIP 保留；/Applications/MaiChat.app 2.2.64 / PID 96313 保留，没有终止或替换用户程序。

私有 clean main 副本 `/private/tmp/maichat-release-2.2.66-20261117/source` 经主项目 GitHub 非递归 fetch，先冻结 a5d81db3e00ace0ad8d78b914a7873c3828f86bc，随后 fast-forward 到 71a81e33c5d75588103ae2a701a14448bc4c3ee4。后者全部 freeze trees、3.0.1、CMake blob、190B header 及 clean 状态均逐等。原路径中的准备版本仅为私有容器名；正式构建和输出目录为 `/private/tmp/maichat-release-3.0.1-20261117`，CMAKE_HOME_DIRECTORY 指向该干净源码的 MaiChat/desktop，没有从旧父目录取源码 SHA。

Qt 6.12 两个锁定模块由现有 clean 源码离线复制；没有递归第三方 Git 或网络更新。因新 builder 增加来源状态指纹，未伪造旧 stamp，而在私有副本全新构建 SDK。ORT 既有公开 runtime 和四个忽略的人像模型均逐 SHA 复用，没有读取用户数据库、消息、凭据或调用供应商付费任务。

## 干净构建发现并修复的缺项

第一轮配置缺四个 gitignored portrait 模型；逐固定 SHA 补齐后配置成功，旧失败证据保留。随后实际 App 编译 87.695s exit1：SoundTouch AAFilter.cpp/BPMDetect.cpp 在 STTypes.h:53 找不到 soundtouch_config.h。原工作区恰好残留此文件，源码仓库被 vendor 的 `*config*` 规则漏跟踪。

直接下载原 soundtouch-2.3.3.tar.gz 只作只读证据：606780B，SHA256 43b23dfac2f64a3aff55d64be096ffc7b73842c3f5665caff44975633a975a99；member soundtouch/include/soundtouch_config.h 原始190B、SHA256 2356e64a66fc519f4968ed0b692143fa8047c8824b403868c2e546b5524591d1，与已有 comments-only non-autotools stub 完全相同。只补该原件，SoundTouch 16步/1.620s exit0，完整 App overlay 870步/332.496s exit0，无其它 clean 缺项。协调端独立核 archive/member 后以新 freeze 跟踪该原件和来源说明，未改 CMake 或平台宏。

71a81e33 最终 normal configure 9.573s / App build 4.989s / 必要消费者编译12.311s，均 exit0。未把 a5+overlay 诊断包当最终发布资产。

## 本机真实消费者

- AgentDocumentThumbnailTest：128 checks / 0 failures，22.700s；没有套用 Windows132计数。
- MarkdownDocumentTest：18 passed / 0 failed / 0 skipped。
- AgentFileDeliveryUiTest：8 passed / 0 failed / 0 skipped，真实普通文件卡、完整预览、loading/失败恢复、会话/工作根切换；Cocoa 进程12.860s。
- MainWindowLayoutTest 窄导航槽：13 passed / 0 failed / 0 skipped，7.767s；六项同蓝、空心未选/实心选中，Cloud/My 实心及并列个人/设置页面。

消费者在私有 CFFIXED_USER_HOME 下运行，sandbox 拒绝外网及真实用户目录读写；无需 TCC。已亲看本轮 Markdown 内容卡、损坏 PDF 回原卡、PDF 真首屏、Cloud/My 导航截图。PDF 卡顶部在当前滚动视口外是页面滚动位置，不误判为卡片自身几何裁切；独立 loading 首Paint几何断言通过。

## 发布期间发现的最低 macOS 版本问题

不把 bundle 标注14.4当旧系统验收。按源 App、正式插件全集和延迟加载 IM SDK 的 LC_LOAD/RPATH 递归静态闭包，共26个 arm64 Mach-O，unresolved0；动态 image 实际最低版本最大26.0。

高于14.4的五件：libonnxruntime.1.26.0.dylib=26.0、libnghttp2.14.dylib=26.0、libbrotlidec.1.2.0.dylib=15.0、libbrotlicommon.1.2.0.dylib=15.0、libzstd.1.5.7.dylib=15.0。其余<=14.4。此前 FFmpeg 静态对象另出现27.2链接警告，因此不能只根据最终 executable 的14.4 load command 推断全部编译输入兼容。

已用相同冻结源码脚本及 MACOSX_DEPLOYMENT_TARGET=14.4，在独立 runtime-14.4-probe 子目录完成全新 FFmpeg/codec 构建，135.503s exit0。11个静态档案中2393个声明最低版本的对象全部<=14.4；未改 load command，未安装到系统或正式 App。这证明静态 FFmpeg 边界可以正常重编解决，不能证明其余五件或14.4系统实际运行。已查私有 vendor/Homebrew 下载缓存没有可直接复用的同版本14.4动态产物；iOS ORT xcframework 无macOS slice。

协调端随后收到用户明确授权按14.4重编现有依赖。保持版本和providers，不改共享源码或Mach-O最低版本声明，没有系统/Homebrew安装。以下追加实际重编与最终验包结果，前述26.0/27.2数据是修复前边界。

## 当前状态

Mac四件正式资产已生成并完成本机验包；由协调端建立v3.0.1 draft，Mac仅按授权上传自身四件，不创建tag/release或发布。签名为ad-hoc，非Developer ID、未公证；实际运行平台为macOS27.2 arm64，14.4为构建目标和最低版本门禁，没有旧系统或Intel Mac运行验收声明。

完整私有证据：`/private/tmp/maichat-release-3.0.1-20261117`，含 source-freeze.json、source-checkout-r2.json、sdk-inputs.json、normal-app-build.json、consumers.json、anticipated-package-minos.json、runtime-14.4-metadata.json 及截图/原始日志。

## Windows r2 清单交叉核对

收到协调端71a81e33版Windows manifest/SUMS，manifest实际SHA256 f27250a940616974ae9294ada0da44a6675a13f5a45149fa6a61c2f5cfb0a375 与SUMS一致；3.0.1、完整sourceCommit、三主要源码树、五模型bytes/SHA及两资产条目均一致。仅核收到的清单，未在Mac下载或执行Windows二进制，也未将此当Mac兼容范围或上传窗口批准。回执 windows-r2-cross-check.json。

## 同版本14.4依赖重编与最终供件

当前冻结源始终71a81e33。官方归档先逐SHA核验：ORT1.26.0=2a90eb9a306c1eeb29213f5b165a55008ac5cb7d27e0935c4458c51a49ef091d；nghttp21.69.0=c866b7477cbb7512ab6863a685027adbb1bb8da8fc3bab7429ed43d3281d5aa9；Brotli1.2.0=816c96e8e8f193b40151dad7e8ff37b1221d019dbcb9c35cd3fadbfe6477dfec；zstd1.5.7=37d7284556b20954e56e1ca85b80226768902e2edabd3b649e9e72c0c9012ee3。

ORT使用冻结vendor源，CoreML+XNNPACK+CPU不变；20份既有上游依赖归档逐deps.txt SHA1和附加SHA256核验，按原URL布局镜像供给。没有使用准备过的SOURCE_DIR覆盖，没有跳过上游PATCH_COMMAND，没有vcpkg或第三方Git下载。显式arm64、CMAKE_OSX_DEPLOYMENT_TARGET/MACOSX_DEPLOYMENT_TARGET=14.4、skip_submodule_sync、TRY_FIND_PACKAGE_MODE=NEVER；2153步/547.188s构建exit0。

nghttp2最终采用官方release configure/make，保持旧Darwin compatibility44.0.0/current44.4.0约定；早期CMake14.29.4产物仅历史候选。四个实际使用的网络dylib旧导出181/14/11/596全部保留。Brotli/Zstd实际压缩回读、nghttp2 settings帧和版本检查exit0，dladdr仅指向private prefix。Qt五个框架LC_LOAD无Homebrew/Brotli/Zstd，最终curl通过清理旧pkg缓存和精确include/library参数固定到新prefix，HTTP2、Brotli、Zstd均开启。

新ORT预签名SHA256=06d76fe8c3d8647b5a8362c011b95aa527e9731d21087bef526735184894777c；new-build/vendor/源App逐字节一致。正式重签后ORT SHA256=894a4bc0e50cec2cc81a01afc2b0028b6be78531b07808ba90ea14ccc29c5858，明确区分签名前后字节。两个阶段均用独立进程dladdr确认实际库路径、1.26.0、同三providers及3个相同公开导出。原MaiRvmMattingTests未改源：显式新库、真实15MB模型首帧/递推/reset检查，签前0.359s、签后0.065s均exit0；dyld日志确认实际目标库，拒绝unset skip。

FFmpeg导入目录指向真实runtime-14.4-probe独立树，旧host27对象移至历史目录；最终App重新链接。实际Ninja应用/ORT链接命令收集164个静态档案及66个直接对象，所有声明最低版本<=14.4；没有把旧历史目录算作最终输入。

## 最终实际包验收

官方打包门禁/严格签名/依赖架构检查均0，再对真实arm64切片执行最低版本硬门禁。26个Mach-O总资源中25个实际arm64 image全部<=14.4；既有未加载TRTC Intel辅助audio driver仅作为SDK资源单列，没有安装驱动。SDK版本27.2没有误算作minos。

实际DMG只读挂载、实际ZIP提取、staging三份App的338文件SHA和37符号链接全部一致，严格签名均0；六次裸系统PATH、隔离资料/外网/SDK/Homebrew/真实用户目录smoke均exit0，确认五个Qt框架及Cocoa插件从对应bundle加载。运行PID33972、34420、35261、35564、36228、36620。DMG布局/完整性、ZIP完整性也通过。五模型与Windows共同清单逐bytes/SHA一致。

Mac源码exe SHA256=c261e53fd1aa916deb9a819057d010e5069557b313f4235decf5f484d6c1c71c；签后exe SHA256=a9c0191f65e110e2e13ed53b0d5d0e6be0d8367005d301199d0195717b9b0f83。完整命令、SDK、providers、归档镜像SHA和签名链保存在本轮private receipts及发布manifest。没有重复执行Windows已过的通用逻辑测试。

最终大包：MaiChat-macos-arm64-v3.0.1-20261117-71a81e33.dmg，214884887B，SHA256 dc3e11bcb87a94f85c0d40e8223ccbaf9a98719e4509bc3e04cdb40048949d88；同名zip，215423423B，SHA256 ed4032eb5f5e9c031cced63b2f7643ee8fb78c1a5a947a3c40069b05839a3425。Release448375145 draft上传按四件唯一名称执行，不用clobber，完成后逐GitHub服务器digest核对。最终publish归house-maichat。

## Mac四件上传完成

既有gh按448375145/v3.0.1授权窗口串行上传，四件均uploaded且GitHub服务器digest逐SHA一致，无clobber或重传。

- SHA256SUMS-macos-arm64.txt：330B，SHA256 df048039710cb81c5b9326999f29b01693a19f77839708b72a93fd03b00d5562，assetID 758383934。
- release-manifest-macos-arm64.json：47600B，SHA256 a4670b36ea20d49fe5eb15802e7003cb6853dbda8927902ee36fb5f5c0839fcd，assetID 758384230。
- MaiChat-macos-arm64-v3.0.1-20261117-71a81e33.dmg：214884887B，SHA256 dc3e11bcb87a94f85c0d40e8223ccbaf9a98719e4509bc3e04cdb40048949d88，assetID 758385030。
- MaiChat-macos-arm64-v3.0.1-20261117-71a81e33.zip：215423423B，SHA256 ed4032eb5f5e9c031cced63b2f7643ee8fb78c1a5a947a3c40069b05839a3425，assetID 758399094。

最终小manifest/SUMS和safe receipt已交协调端；Mac未修改Release body/tag或publish。当前等待协调端统一8资产元数据核对后公开发布。

## 正式发布完成

协调端已于2026-11-17T09:26:31Z公开发布v3.0.1并设Latest，Release448375145/source71a81e33。Mac独立实时REST读回draft=false/prerelease=false及8件uploaded状态，四件Mac服务器digest与本地逐等。公共页：https://github.com/wanhongqing123/MaiChat/releases/tag/v3.0.1 。Mac没有执行publish/tag/body修改，现装App保持不动。本轮Desktop发布无剩余动作；完整归档public-completion.json。
