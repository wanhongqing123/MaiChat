# Mac 本地改动提交与 main 同步

2026-11-17，用户明确要求将应提交的本地改动提交并推送远端。

## 现场与保全

原工作区在 main/c3ea0bbbbb63f09b67329f57b31fd5e6b92c554c，47项 tracked 修改和49项 untracked 文件；最新远端为 ed13d88c25737f15348a307aa36fc85b85866161。先保存逐文件SHA、完整原文件归档、tracked binary patch，再用 include-untracked stash保全并快进main。私有备份在 `/private/tmp/maichat-submit-local-20261117`，保全stash为4da40b3a1bd3ceb1d1961814f5d187966b4f1690，未自动删除。

31项本地改动已被远端覆盖或完整包含，随快进消除重复，不另造重复功能提交。其余内容按c3ea共同基线三方合并，保留所有远端后继修复。Android MessagePagesTest的唯一文本冲突是远端补充测试；保留该补充后整文件与远端相等，未新增一次重复测试修改。本轮iOS原有本地改动均保留最终已验版本，其他文件随远端更新；Android Controller/MessagePages/Test采用远端包含原改动的后继完整版本。

两份双方独立新增的同名诊断保留远端正文，将本机详细历史另存 `-mac-evidence.md`，互相链接，避免覆盖Windows证据。历史记录中“候选/未安装/等待”等状态按对应时点解释，后继安装、上线和发布以各最终回执小节为准。目录归整仍仅方案，未因提交文档而实施迁移。

## 代码提交与实际检查

- Android：5481f4917a9e651b1eac989210d44e74f93588a1，5件横屏生产/测试源。普通Dialog JVM测试6/0、errors0，Gradle28.545秒、用例2.570秒。仓库gradlew没有可执行位，首调用在运行前PermissionError；改用bash执行同一脚本后通过，未修改权限或安装依赖。既有API37真实解码和窗口截图证据见移动播放器记录，本次没有重建完整APK或安装Android真机。
- iOS：11595dcf0f11d062f234de1f27405479e4d5db13，15件共用文档卡、HTML完整预览、手动横屏、分页方向生命周期、切页取消边界及原UI测试。当前合并源正常模拟器build+test退出0，218.782秒，四项UI全部通过：手动横屏暂停/恢复23.411秒，创作HTML首屏/完整预览9.710秒，IM收发两侧PDF/MD/HTML56.716秒，快速切页取消19.632秒。
- 已导出并亲看整屏横屏、IM Markdown/HTML及完整HTML截图。素材为自有fixture，未读取用户消息或触发付费模型；隔离模拟器bundle没有替换真机现装227。
- 工程plutil lint和git diff检查通过。对所有新增内容扫描私钥、云AccessKey、GitHub token、JWT和带凭据URL；唯一扩展规则命中为原UI测试明确命名fakePassword的本地假样本，无真实凭据。构建产物、数据库、环境文件和私有执行日志不入提交。

## 推送范围与边界

代码按Android和iOS两笔提交，诊断、设计、工具清单、发布与部署验收记录另作一笔提交。远端main推进在本轮由Mac独占，协调端暂停并行commit/push并保留其本机其他WIP；没有消费其旧iOS完整文件，也没有提交另一台机器的未审源。

本次只更新main，不修改已公开v3.0.1 tag及八个资产，不重装App、不重新部署服务，不另改SDK或第三方源码。最终main SHA、远端回读和工作区状态写入私有任务 `completed-receipt.json`；原始失败、备份和stash均保留。
