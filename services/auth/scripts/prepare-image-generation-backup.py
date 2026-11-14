"""供明确批准的后续执行器调用的 SQLite online backup 源码。

没有 CLI 或服务入口：直接执行仅报 source-only，不连接数据库。
调用器负责已批准模式、精确源路径/角色、0700 私有目录及基线核验；
本模块不读环境、业务表或凭据，不联网、不部署、不重启、不恢复生产库。
"""

from contextlib import closing
from datetime import datetime, timezone
import hashlib
import math
import os
from pathlib import Path
import sqlite3
import stat
import time


def backup_database(source, private_directory, basename, max_seconds):
    """备份一个已明确批准的源库；只返回安全元数据，失败文件保留为 incomplete。

    不宣称多个调用是跨库事务快照。不得用于自动覆盖或回滚生产数据库。
    """
    source = Path(source).resolve(strict=True)
    private_directory = Path(private_directory)
    directory = private_directory.resolve(strict=True)
    info = directory.stat()
    if (directory != private_directory.absolute() or not stat.S_ISDIR(info.st_mode)
            or info.st_uid != os.geteuid() or stat.S_IMODE(info.st_mode) != 0o700):
        raise ValueError("PRIVATE_DIRECTORY_REQUIRED")
    if (not source.is_file() or not basename or basename != Path(basename).name
            or basename in {".", ".."} or not basename.endswith(".sqlite")
            or not isinstance(max_seconds, (int, float)) or isinstance(max_seconds, bool)
            or max_seconds <= 0 or not math.isfinite(max_seconds)):
        raise ValueError("APPROVED_BACKUP_INPUT_REQUIRED")
    destination = directory / basename
    # O_EXCL/O_NOFOLLOW 确保不能覆盖已有备份、链接或原库。
    fd = os.open(destination, os.O_CREAT | os.O_EXCL | os.O_WRONLY | os.O_NOFOLLOW, 0o600)
    os.close(fd)
    started = time.monotonic()
    deadline = started + max_seconds

    def progress(_status, _remaining, _total):
        if time.monotonic() > deadline:
            raise TimeoutError("BACKUP_DEADLINE")

    try:
        # mode=ro 保留普通 WAL 可见性；immutable=1 会漏掉活 WAL，禁止使用。
        with closing(sqlite3.connect(source.as_uri() + "?mode=ro", uri=True, timeout=5)) as src:
            with closing(sqlite3.connect(destination)) as dst:
                src.backup(dst, pages=128, progress=progress, sleep=0.05)
                if dst.execute("PRAGMA quick_check").fetchone() != ("ok",):
                    raise RuntimeError("BACKUP_INTEGRITY_FAILED")
        os.chmod(destination, 0o600)
        digest = hashlib.sha256()
        with destination.open("rb") as data:
            for chunk in iter(lambda: data.read(1024 * 1024), b""):
                digest.update(chunk)
        return {"ok": True, "backup": str(destination), "bytes": destination.stat().st_size,
                "sha256": digest.hexdigest(), "seconds": round(time.monotonic() - started, 3),
                "at": datetime.now(timezone.utc).isoformat()}
    except Exception:
        # 不打印数据库异常内容；不删除失败证据，也不把不完整文件冒充成功快照。
        return {"ok": False, "backup": str(destination), "incomplete": True,
                "code": "BACKUP_FAILED_INCOMPLETE", "seconds": round(time.monotonic() - started, 3)}


if __name__ == "__main__":
    raise SystemExit("SOURCE_ONLY: no database or deployment action was executed")
