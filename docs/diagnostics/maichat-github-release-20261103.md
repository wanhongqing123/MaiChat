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
