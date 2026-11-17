# 8.148.230.58 运行服务只读检查

本页记录最初只读检查时的状态；用户随后授权的实际清理及清理后状态见 [业务与磁盘清理](aliyun-8-148-230-58-cleanup-20261112.md)。

用户询问空闲阿里云服务器正在运行什么服务，并给出控制台 CPU 3.12%、内存 20.86%、云盘 48.99%。本次预期为识别当前进程、服务、监听及资源占用；不停止、重启、安装、部署、改配置或读取业务数据库。

观察时间：2026-11-12 00:33–00:35（Asia/Shanghai；远端 UTC 2026-11-11 16:33–16:35）。执行端仓库 HEAD 为 `dabe95b854b6345dae512dbd4eb77607a3973494`，不据此推断远端业务版本。SSH 使用既有主机配置及严格 host key 校验；只输出筛选后的进程/配置元数据，不输出环境、完整命令行、密钥或请求内容。

## 实际运行的业务

- `knowhowmath.com`：Node.js 项目 `knowhowmath-platform-app`，package version `2.0.0`。运行入口 `/www/wwwroot/knowhowmath.com/app/server.js`，PID1052，用户 `teachci`，RSS75948KiB（约74MiB）。由 `pm2-teachci.service` 管理，PM2 PID925，RSS77308KiB（约75MiB）。Nginx `00-khm-ip.conf` 将 HTTP80 对应的 `knowhowmath.com` / `www.knowhowmath.com` / 本机 IP 请求转发到3000；Node 实际监听 `*:3000`。
- TeachSoftware 课堂测试服务：`teachsoftware.service`，描述 `TeachSoftware Go classroom test service`，PID675，RSS18112KiB（约18MiB）。实际 `/proc/675/exe` 为 `/opt/teachsoftware/releases/ci-27173de33fef/classroom-server`，工作目录为同一 release；unit 工作路径 `/opt/teachsoftware/current`。监听 `127.0.0.1:3080`，Nginx8080反向代理到此端口。release 目录名仅作部署身份，不冒称完整 Git SHA。

Nginx master PID1048、worker PID1050/1051；另有 SSH22。云端辅助进程包括 Aegis/AliYunDun、cloudmonitor/argusagent、LoongCollector、Aliyun Assist，以及系统日志、时钟、网络和调度服务。未发现正在运行的独立 Docker、MySQL 或 Redis 进程；这不代表业务没有使用嵌入式或外部数据库。

## 当前资源与响应

- Alibaba Cloud Linux 3.2104 U13.3，2个逻辑CPU，系统可见内存约1.8GiB（约2GiB规格）。
- 2026-11-12 00:33观测：连续运行11天11小时，load average `0.00/0.00/0.00`；`/proc/stat` 一秒采样忙碌约1.51%。这是一段短时观测，不与云控制台不同采样窗口的3.12%强行比较。
- `free -h`：used402MiB、available1.4GiB，buff/cache1.1GiB，未配置swap。缓存不等于业务进程独占内存。
- 根卷 `/dev/vda3` ext4：40G、used19G、available19G、49%。未扫描目录占用或归因这19G的具体来源。
- 00:35使用本机连接、正确Host的 `HEAD /`：HTTP80（Host `knowhowmath.com`）200；HTTP8080（Host `8.148.230.58`）200，均返回HTML类型。只验证本机HTTP响应，不代表外网安全组、DNS、所有功能或近期业务活跃度已验收。
- 00:34的80/8080/3000/3080端口 established 连接计数为空；这是瞬时快照，不能据此认定没有用户或服务可停用。

## 边界

检查仅使用系统状态、服务属性、进程 exe/cwd/cgroup、筛选后的Nginx入口配置和package公开元数据。没有读业务数据库、用户聊天、环境文件或私钥内容，也未发任何写业务请求。正常SSH及HTTP请求可能产生系统/访问日志。首个只读脚本因远端Python3.6不支持 `subprocess.run(text=...)` 退出，随后仅改为兼容的 `universal_newlines=True` 重读成功；不是服务故障。
