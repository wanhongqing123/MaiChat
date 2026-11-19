# iOS 231 缓存路由安装包与连接边界

源码为 `2dd34dc0f218eccb26154792ae1c558a2abcac54`。本端核对十件供件与发布后逐 SHA 一致，备份原自有 WIP 后 FF，产品工作区 clean 开始正常真机构建。

## 正常产物

- 原 workspace/scheme、iphoneos Release 构建 exit 0，51.342 秒；版本 3.0.1(231)，bundle `com.kongshang.maichat`。
- 本地候选 deep/strict 签名验证 0，签名 Team 与 230 一致。exe 95551632 字节，SHA256 `bcaa3c078a00208fedc3335b14df9975a01f53feab01ce6fdab7fd8df23397d1`；未作设备 exe 读回。
- 物理目标 CMake 源目录正确。76 个实际依赖新增 Request Header 的对象均在新 Header 后重编，9 项关键 Core/CAPI 对象到 native 与 App 合并静态库逐字节核等，没有旧请求布局混链；未修改旧 cache home 或 SDK stamp。

## 安装前外部阻塞

安装前仅查询目标 bundle 的 metadata 时，CoreDevice 返回 4016：请求所需的 trusted connectivity、core device services、power assertion 状态未就绪。因此本轮没有派发 install 或 launch，不能把 ready 安装包计为已安装、已运行 231。

手机最近一次独立确认安装版本仍为 230，其普通启动曾被 Locked 拒绝。新的连接并解锁请求已由本端提出；用户确认后只继续同 bundle 保数据安装、普通无参数启动及独立版本/进程回读，无需为此重编、重新配对或重复验证探针。

本轮未读取手机数据、密钥或真实供应商响应，付费模型验证请求为 0；没有改动模型、思考、输出预算或请求资料顺序。供应商真实缓存命中率和节省金额尚未验证。
