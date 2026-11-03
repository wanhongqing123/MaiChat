#!/usr/bin/env python3
"""汇集当前工作区真实构建命令，供 clangd 和 VS Code C/C++ 共用。

输入为 CMake 导出的数据库以及 Clang -gen-cdb-fragment-path 生成的 Xcode 片段。
不猜测平台宏、SDK 或头文件路径；同一源文件优先采用最先提供的数据库。
本机输入位置保存在被忽略的 .cache/cpp-index/inputs.json，后续可直接再次运行。
"""

import argparse
import json
import os
import subprocess
import tempfile
from pathlib import Path


WORKSPACE = Path(__file__).resolve().parents[2]
LOCAL_CONFIG = WORKSPACE / ".cache/cpp-index/inputs.json"


def atomic_json(path: Path, value: object) -> None:
    """语言服务器可能正在读取数据库，完整写入后再原子替换。"""
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(mode="w", encoding="utf-8", dir=path.parent,
                                         prefix=".compile_commands.", suffix=".tmp",
                                         delete=False) as stream:
            temporary = Path(stream.name)
            json.dump(value, stream, ensure_ascii=False, indent=2)
            stream.write("\n")
        os.replace(temporary, path)
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)


def read_commands(path: Path, fragment: bool = False) -> list[dict]:
    """Clang 片段末尾允许逗号，完整 CMake 数据库必须为 JSON 数组。"""
    raw = path.read_text(encoding="utf-8").strip()
    value = json.loads(raw.rstrip(",") if fragment else raw)
    if fragment and isinstance(value, dict):
        return [value]
    if not isinstance(value, list):
        raise ValueError(f"Expected a compilation database array: {path}")
    return value


def refresh_cmake(database: Path) -> None:
    """只重新配置已有构建；不改变工具链或构建选项，不编译/启动产品。"""
    cache = database.parent / "CMakeCache.txt"
    for line in cache.read_text(encoding="utf-8").splitlines():
        if line.startswith("CMAKE_HOME_DIRECTORY:INTERNAL="):
            source = Path(line.partition("=")[2]).resolve()
            if not source.is_relative_to(WORKSPACE):
                raise ValueError(f"Build belongs to another checkout: {database.parent}")
            subprocess.run(["cmake", "-S", str(source), "-B", str(database.parent),
                            "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON"], check=True)
            return
    raise ValueError(f"Missing CMake source directory: {cache}")


def own_source(path: Path) -> bool:
    """保留产品与自有测试源码；依赖头仍由真实 -I/-F 参数解析，避免索引整个第三方构建。"""
    if not path.is_relative_to(WORKSPACE):
        return False
    parts = path.relative_to(WORKSPACE).parts
    return (parts[0] in {"MaiChat", "MaiAgent"} and
            not any(part in {"third_party", "vendor", "build", ".cache", ".cxx", ".gradle"}
                    or part.startswith("build-") for part in parts))


def install_alias(path: Path, destination: Path) -> None:
    """旧的子目录数据库会遮蔽根目录数据库，统一为相对链接，避免保留失效的旧仓库路径。"""
    relative = os.path.relpath(destination, path.parent)
    if path.is_symlink() and os.readlink(path) == relative:
        return
    temporary = path.with_name(".compile_commands.alias.tmp")
    temporary.unlink(missing_ok=True)
    temporary.symlink_to(relative)
    os.replace(temporary, path)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--database", type=Path, action="append", default=[])
    parser.add_argument("--ios-fragments", type=Path)
    parser.add_argument("--refresh", action="store_true", help="先刷新各 CMake 输入数据库")
    args = parser.parse_args()
    previous = json.loads(LOCAL_CONFIG.read_text()) if LOCAL_CONFIG.exists() else {}
    databases = args.database or [Path(p) for p in previous.get("databases", [])]
    fragments = args.ios_fragments or (
        Path(previous["ios_fragments"]) if previous.get("ios_fragments") else None)
    if not databases:
        parser.error("请用 --database 提供当前 CMake 构建导出的 compile_commands.json")
    databases = [p.resolve() for p in databases]
    if args.refresh:
        for database in databases:
            refresh_cmake(database)
    inputs = [(p, False) for p in databases]
    if fragments is not None:
        fragments = fragments.resolve()
        inputs += [(p, True) for p in sorted(fragments.glob("*.json"))]
    commands = {}
    for database, fragment in inputs:
        for entry in read_commands(database, fragment):
            directory = Path(entry["directory"]).resolve()
            source = Path(entry["file"])
            source = (directory / source).resolve() if not source.is_absolute() else source.resolve()
            if not own_source(source) or source in commands:
                continue
            if not source.is_file() or not directory.is_dir():
                raise ValueError(f"Stale compilation entry: {source} ({directory})")
            if not (entry.get("arguments") or entry.get("command")):
                raise ValueError(f"Missing compiler command: {source}")
            commands[source] = {**entry, "file": str(source), "directory": str(directory)}
    if not commands:
        parser.error("输入中没有当前工作区源码的真实编译命令")
    output = WORKSPACE / "compile_commands.json"
    atomic_json(output, [commands[p] for p in sorted(commands)])
    for alias in [WORKSPACE / "MaiChat/compile_commands.json",
                  WORKSPACE / "MaiChat/ios/MaiChat/compile_commands.json",
                  WORKSPACE / "MaiAgent/compile_commands.json"]:
        install_alias(alias, output)
    atomic_json(LOCAL_CONFIG, {"databases": [str(p) for p in databases],
                              "ios_fragments": str(fragments) if fragments else None})
    print(f"{output}: {len(commands)} compilation commands")


if __name__ == "__main__":
    main()
