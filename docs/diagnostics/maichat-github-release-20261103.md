# MaiChat GitHub Release（2026-11-03）

用户要求把当前 MaiChat 发布到 **wanhongqing123/MaiChat** 的 GitHub Releases，
不能沿用旧 multi-ai-code 仓库的发布地址。

操作与预期：完成当前源码提交推送，制作可下载的安装包，校验包内版本、签名、依赖与
源码对应关系；在 MaiChat 仓库创建指向该提交的 tag 与 Release，上传安装包及 SHA-256，
随后从 GitHub 回读目标提交、发布状态、附件大小与校验值。

## 发布前证据

- GitHub API 确认仓库 `full_name=wanhongqing123/MaiChat`、公开仓库、默认分支 main，
  当前账号有发布权限；发布前 Releases、tags、Actions workflows 均为空。
- 账号资料与头像工具已提交推送 `fd67e6c20b0ff8f653f85220a882cb27f533a976`；
  三端版本、测试及实机边界见 `maichat-account-profile-tools-20261103.md`。
- macOS 2.2.34 来自 Release 构建；打包 App 与已安装 App 主程序 SHA-256 相同：
  `5a28eeafa81d90f2d10bdc87adef666f16508127b3a4df902141fc19ed1bce0a`。
  包含运行依赖，严格验签与 smoke 通过。使用 ad-hoc 签名，未做 Developer ID 或公证，
  不宣称 Gatekeeper 已接受。`vtool -show-build` 确认主程序最低系统为 macOS 27.2，
  与本机 27.2 一致；附件说明明确此要求，不宣称支持更早 macOS。
- Android 2.2.6 (246) 是已验证的 Debug APK，两个 ABI 为 arm64-v8a、armeabi-v7a，
  231137408 字节，SHA-256 为
  `aad66d4869e5a3a6460ee24b1caa60e247c2006843779670a2ad839f9a35ea0e`。
  签名与 ZIP 完整性通过，正常账号服务配置；明确命名为 debug 测试包。
  本机未配置四项正式 Release 签名环境变量，已有 release APK 是旧版，不能误发布。
- iOS 2.2.4 (172) 已安装启动，但当前产物是受设备限制的开发签名；本次不上传开发
  IPA、描述文件、设备清单、签名材料或原始日志。无本轮 Windows 分发产物。

## 打包迁移缺口

`desktop/README.md` 仍指向旧仓库，并引用拆仓后不存在的 npm 发布命令。
`package-macos.sh` 仍依赖不存在的 `node_modules/dmg-builder` Finder 布局工具，
普通 DMG 流程会在挂载临时镜像后失败。修复应保留已有 hdiutil 和 Applications 拖拽
安装布局，把布局写入改为独立打包机 Python 工具，并在修改 staging 前检查依赖。
已改用固定版本 ds_store 1.3.3 / mac_alias 2.2.3 写入和回读验证 Finder 布局；
独立 venv 的依赖检查、布局往返与 --verify-only、bash 语法检查均通过；
缺依赖的负例退出 1，原 staging sentinel 保留，未先删除已有文件。
打包工具依赖不随 App 分发，不引入对旧工作区的运行时依赖。

发布验收使用最终 DMG 挂载目录及其 App，不替换正在运行的用户 App，不修改用户资料。
安装包不提交到 Git，只上传明确命名的附件与校验文件。

## 最终产物验证

使用提交 `ca4e3938` 的脚本从已验证 Release 构建生成
`MaiChat-macos-arm64-v2.2.34-20261103-ca4e3938.dmg`，143429508 字节。
镜像 SHA-256：`db29a142c1a461cbbb24bd71b213fbcc691386f17c98b6edc44d2993c2e6d8b9`。
`hdiutil verify` 通过；以只读方式挂载后，版本 2.2.34、严格验签、Finder 布局回读、
QtWebSockets 存在检查及主程序哈希均通过。原生 cocoa 后端 `--smoke` 退出 0。
最初验证脚本误指定未分发的 offscreen 插件，因插件不存在退出，不能据此判定包损坏；
改用实际随包的 cocoa 进行验证。只关闭本次临时挂载路径启动的测试副本后正常卸载，
未停止 `/Applications/MaiChat.app` 的用户进程。回执和校验清单在
`/private/tmp/maichat-github-release-20261103/`，原始本机日志不上传。

独立复查发现：输入 App 位于 staging 内时，原脚本会先删除输入再复制失败。
本次打包使用分离路径，镜像未受影响；另增加真实路径解析及双向重叠拒绝，覆盖符号
链接别名。三个实际 shell 用例（输入在输出内、输出在输入内、符号链接别名）都在
删除前退出 1，原输入 sentinel 保留；语法与 diff 检查通过。该保护不改变 App 二进制。

## 发布结果

2026-11-03 11:40:49（Asia/Shanghai）已发布
[MaiChat v2.2.34](https://github.com/wanhongqing123/MaiChat/releases/tag/v2.2.34)，
tag 指向 `3c7a29272dc3c97cacb4d1ba0678801a9cf36b46`。
先上传草稿，逐项核对 GitHub 返回的大小和 SHA-256 后再公开；最终回读确认
draft=false、prerelease=false，且为 MaiChat 仓库 Latest Release。三个附件为上述 DMG、
Android Debug APK 和 SHA256SUMS，远端摘要全部与本地一致；未向其他仓库发布。
