// 文件作用：复用普通文件核心发现本机 shell；不运行探测命令、不采集环境或启动脚本内容。
#include "MaiShellEnvironment.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "MaiBlockingCheck.h"
#include "MaiFilePath.h"
#include "MaiFileSystem.h"

#if defined(_WIN32)
#include <windows.h>
#else
#include <cerrno>
#include <pwd.h>
#include <unistd.h>
#endif

namespace {
constexpr std::size_t kMaximumEnvironmentBytes = 1024 * 1024;

std::optional<MaiShellType> typeOf(const std::string& value) {
    const auto slash = value.find_last_of("/\\");
    auto name = value.substr(slash == std::string::npos ? 0 : slash + 1);
    const auto dot = name.find_last_of('.');
    if (dot != std::string::npos) name.resize(dot);
    std::transform(name.begin(), name.end(), name.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    if (name == "zsh") return MaiShellType::Zsh;
    if (name == "bash") return MaiShellType::Bash;
    if (name == "sh") return MaiShellType::Sh;
    if (name == "cmd") return MaiShellType::Cmd;
    if (name == "pwsh" || name == "powershell") return MaiShellType::PowerShell;
    return {};
}

std::string shellName(MaiShellType type) {
    switch (type) {
        case MaiShellType::Zsh: return "zsh";
        case MaiShellType::Bash: return "bash";
        case MaiShellType::PowerShell: return "powershell";
        case MaiShellType::Sh: return "sh";
        case MaiShellType::Cmd: return "cmd";
    }
    return {};
}

std::string environmentValue(const char* name) {
#if defined(_WIN32)
    const auto wideName = MaiFilePath::fromUtf8(name).value();
    DWORD size = ::GetEnvironmentVariableW(wideName.c_str(), nullptr, 0);
    if (!size || size > kMaximumEnvironmentBytes) return {};
    std::wstring value(size, L'\0');
    const auto copied = ::GetEnvironmentVariableW(wideName.c_str(), value.data(), size);
    if (!copied || copied >= size) return {};
    value.resize(copied);
    return MaiFilePath(std::move(value)).toUtf8();
#else
    const char* value = std::getenv(name);
    if (!value) return {};
    const std::string owned(value);
    return owned.size() <= kMaximumEnvironmentBytes ? owned : std::string{};
#endif
}

std::string executableAt(const MaiFilePath& path) {
    const auto resolved = MaiFileSystem::resolve(path);
    if (resolved.isEmpty() || !resolved.isAbsolute()) return {};
    MaiRegularFileInfo info;
    if (!MaiFileSystem::regularFileInfo(resolved, info)) return {};
#if !defined(_WIN32)
    if (::access(resolved.value().c_str(), X_OK) != 0) return {};
    // 最后一级名字决定 sh/BusyBox applet 模式；不能把 /bin/sh 的 argv[0] 改成
    // realpath 后的 bash/busybox。仅规范化父目录，保留已验证的解释器入口名。
    const auto parent =
        MaiFileSystem::resolve(path.dirName().isEmpty() ? MaiFilePath(".") : path.dirName());
    return parent.isEmpty() ? std::string{} : parent.append(path.baseName()).toUtf8();
#else
    return resolved.toUtf8();
#endif
}

std::string onPath(const std::string& binary) {
    const auto value = environmentValue("PATH");
#if defined(_WIN32)
    constexpr char separator = ';';
    const std::string executable = binary + ".exe";
#else
    constexpr char separator = ':';
    const std::string executable = binary;
#endif
    std::size_t offset = 0;
    while (offset <= value.size()) {
        const auto end = value.find(separator, offset);
        auto directory = value.substr(offset, end == std::string::npos ? end : end - offset);
        if (directory.size() >= 2 && directory.front() == '"' && directory.back() == '"')
            directory = directory.substr(1, directory.size() - 2);
        if (!directory.empty()) {
            auto found = executableAt(
                MaiFilePath::fromUtf8(directory).append(MaiFilePath::fromUtf8(executable)));
            if (!found.empty()) return found;
        }
        if (end == std::string::npos) break;
        offset = end + 1;
    }
    return {};
}

std::string accountShell() {
#if defined(_WIN32)
    return {};
#else
    // getpwuid 的共享静态缓冲在并行会话中不安全；与 Codex 一样使用调用者拥有的缓冲。
    const long suggested = ::sysconf(_SC_GETPW_R_SIZE_MAX);
    std::size_t size =
        suggested > 0 ? std::min<std::size_t>(suggested, kMaximumEnvironmentBytes) : 1024;
    std::vector<char> buffer(size);
    for (;;) {
        struct passwd entry{};
        struct passwd* result = nullptr;
        const auto status = ::getpwuid_r(::getuid(), &entry, buffer.data(), buffer.size(), &result);
        if (status == 0)
            return result && result->pw_shell ? std::string(result->pw_shell) : std::string{};
        if (status != ERANGE || buffer.size() >= kMaximumEnvironmentBytes) return {};
        buffer.resize(std::min(kMaximumEnvironmentBytes, buffer.size() * 2));
    }
#endif
}

std::string systemWindowsPath(const std::string& relative) {
#if defined(_WIN32)
    std::wstring directory(32768, L'\0');
    const auto size = ::GetSystemDirectoryW(directory.data(), static_cast<UINT>(directory.size()));
    if (!size || size >= directory.size()) return {};
    directory.resize(size);
    return MaiFilePath(std::move(directory)).append(MaiFilePath::fromUtf8(relative)).toUtf8();
#else
    (void)relative;
    return {};
#endif
}

std::string findExecutable(MaiShellType type, const std::string& account) {
    if (!account.empty() && typeOf(account) == type) {
        const auto found = executableAt(MaiFilePath::fromUtf8(account));
        if (!found.empty()) return found;
    }
    std::vector<std::pair<std::string, std::vector<std::string>>> candidates;
    switch (type) {
        case MaiShellType::Zsh: candidates = {{"zsh", {"/bin/zsh"}}}; break;
        case MaiShellType::Bash: candidates = {{"bash", {"/bin/bash", "/usr/bin/bash"}}}; break;
        case MaiShellType::Sh: candidates = {{"sh", {"/bin/sh"}}}; break;
        case MaiShellType::PowerShell:
#if defined(_WIN32)
        {
            const auto programFiles = environmentValue("ProgramFiles");
            candidates = {
                {"pwsh",
                 {programFiles.empty() ? std::string{}
                                       : MaiFilePath::fromUtf8(programFiles)
                                             .append(MaiFilePath::fromUtf8("PowerShell/7/pwsh.exe"))
                                             .toUtf8(),
                  "C:/Program Files/PowerShell/7/pwsh.exe"}},
                {"powershell", {systemWindowsPath("WindowsPowerShell/v1.0/powershell.exe")}}};
        }
#else
            candidates = {{"pwsh", {"/usr/local/bin/pwsh"}}, {"powershell", {}}};
#endif
        break;
        case MaiShellType::Cmd: candidates = {{"cmd", {systemWindowsPath("cmd.exe")}}}; break;
    }
    for (const auto& candidate : candidates) {
        const auto found = onPath(candidate.first);
        if (!found.empty()) return found;
        for (const auto& fallback : candidate.second) {
            if (fallback.empty()) continue;
            const auto fallbackFound = executableAt(MaiFilePath::fromUtf8(fallback));
            if (!fallbackFound.empty()) return fallbackFound;
        }
    }
    return {};
}

const char* shEnvironmentStartup() {
    // Adapted from OpenAI Codex, Copyright 2025 OpenAI, licensed under Apache-2.0.
    // Frozen source 553c764e8b99fa519e9c6eb9b657875ef0f5e1c1; see the diagnostic attribution.
    // frozen shell-command/src/shell_snapshot.rs 的非求值 ENV 路径展开；仅在子 shell 中运行。
    // 命令替换用于接收 helper 的返回值，不解释 ENV 里的命令替换或其他任意表达式。
    return R"MAIENV(__mai_expand_env_20261121() (
  set +u
  __mai_get_env_20261121() {
    __mai_expanded_20261121=$(
      if command -v printenv >/dev/null 2>&1; then
        printenv "$1"
      elif [ "$1" = PATH ]; then
        [ "${PATH+x}" = x ] && printf '%s\n' "$PATH"
      else
        command -p printenv "$1"
      fi && printf '.'
    ) || return 1
    __mai_expanded_20261121=${__mai_expanded_20261121%?}
    __mai_expanded_20261121=${__mai_expanded_20261121%?}
  }
  __mai_env_file_20261121=$1
  case "$__mai_env_file_20261121" in
    '~/'*) __mai_env_file_20261121="${HOME-}/${__mai_env_file_20261121#*/}" ;;
    '${PATH%%:*}') __mai_env_file_20261121="${PATH%%:*}" ;;
    '${PATH%%:*}/'*) __mai_env_file_20261121="${PATH%%:*}/${__mai_env_file_20261121#*/}" ;;
    '${'*)
      __mai_env_body_20261121=${__mai_env_file_20261121#\$\{}
      case "$__mai_env_body_20261121" in
        *\}*)
          __mai_env_name_20261121=${__mai_env_body_20261121%%\}*}
          __mai_env_suffix_20261121=${__mai_env_body_20261121#*\}}
          case "$__mai_env_name_20261121" in
            *:-*)
              __mai_env_default_20261121=${__mai_env_name_20261121#*:-}
              __mai_env_name_20261121=${__mai_env_name_20261121%%:-*}
              __mai_env_has_default_20261121=1
              ;;
            *) __mai_env_has_default_20261121= ;;
          esac
          case "$__mai_env_name_20261121" in
            ''|[0-9]*|*[!A-Za-z0-9_]*) ;;
            *)
              case "$__mai_env_suffix_20261121" in
                ''|/*)
                  if __mai_get_env_20261121 "$__mai_env_name_20261121" 2>/dev/null &&
                    { [ -n "$__mai_expanded_20261121" ] || [ -z "$__mai_env_has_default_20261121" ]; }; then
                    __mai_env_file_20261121="$__mai_expanded_20261121$__mai_env_suffix_20261121"
                  elif [ -n "$__mai_env_has_default_20261121" ]; then
                    __mai_env_default_20261121=$(__mai_expand_env_20261121 "$__mai_env_default_20261121")
                    __mai_env_file_20261121="$__mai_env_default_20261121$__mai_env_suffix_20261121"
                  fi
                  ;;
              esac
              ;;
          esac
          ;;
      esac
      ;;
    '$'*)
      __mai_env_name_20261121=${__mai_env_file_20261121%%/*}
      __mai_env_name_20261121=${__mai_env_name_20261121#\$}
      case "$__mai_env_name_20261121" in
        ''|[0-9]*|*[!A-Za-z0-9_]*) ;;
        *)
          if __mai_get_env_20261121 "$__mai_env_name_20261121" 2>/dev/null; then
            if [ "$__mai_env_file_20261121" = "\$$__mai_env_name_20261121" ]; then
              __mai_env_file_20261121="$__mai_expanded_20261121"
            else
              __mai_env_file_20261121="$__mai_expanded_20261121/${__mai_env_file_20261121#*/}"
            fi
          fi
          ;;
      esac
      ;;
  esac
  printf '%s' "$__mai_env_file_20261121"
)
if [ -n "${ENV-}" ]; then
  __mai_env_file_20261121=$(__mai_expand_env_20261121 "$ENV")
  if [ -r "$__mai_env_file_20261121" ] && [ ! -d "$__mai_env_file_20261121" ]; then
    case "$__mai_env_file_20261121" in
      /*|[A-Za-z]:/*|[A-Za-z]:\\*) . "$__mai_env_file_20261121" ;;
      *) . "./$__mai_env_file_20261121" ;;
    esac
  fi
  command unset __mai_env_file_20261121
fi
command unset -f __mai_expand_env_20261121
)MAIENV";
}
}  // namespace

MaiResult<MaiShellEnvironment> maiResolveShellEnvironment(const std::string& requestedShell) {
    maiAssertBlockingAllowed("maiResolveShellEnvironment");
    if (requestedShell.size() > 8192 ||
        std::any_of(requestedShell.begin(), requestedShell.end(),
                    [](unsigned char ch) { return ch < 32 || ch == 127; }))
        return {MaiErrorCode::InvalidInput,
                "shell selection contains controls or exceeds its size limit"};
    const auto account = accountShell();
    std::vector<MaiShellType> types;
    if (!requestedShell.empty()) {
        const auto type = typeOf(requestedShell);
        if (!type)
            return {MaiErrorCode::InvalidInput,
                    "shell must select zsh, bash, sh, pwsh, powershell or cmd"};
        types.push_back(*type);
    } else {
#if defined(_WIN32)
        types = {MaiShellType::PowerShell, MaiShellType::Cmd};
#else
        if (const auto type = typeOf(account)) types.push_back(*type);
#if defined(__APPLE__)
        types.insert(types.end(), {MaiShellType::Zsh, MaiShellType::Bash, MaiShellType::Sh});
#else
        types.insert(types.end(), {MaiShellType::Bash, MaiShellType::Zsh, MaiShellType::Sh});
#endif
#endif
    }
    for (const auto type : types) {
        const auto path = findExecutable(type, account);
        if (!path.empty()) return MaiShellEnvironment{type, shellName(type), path};
    }
    return {MaiErrorCode::NotFound, "the selected shell interpreter is unavailable"};
}

std::vector<std::string> maiShellCommandArguments(const MaiShellEnvironment& shell,
                                                  const std::string& command, bool login) {
    switch (shell.type) {
        case MaiShellType::Zsh:
        case MaiShellType::Bash:
        case MaiShellType::Sh: return {shell.path, login ? "-lc" : "-c", command};
        case MaiShellType::PowerShell:
            return login ? std::vector<std::string>{shell.path, "-Command", command}
                         : std::vector<std::string>{shell.path, "-NoProfile", "-Command", command};
        case MaiShellType::Cmd: return {shell.path, "/d", "/s", "/c", command};
    }
    return {};
}

std::string maiShellStartupCommand(const MaiShellEnvironment& shell, const std::string& command,
                                   bool login, const std::string& workdir) {
    // 与 Codex 一致，不注入 pipefail/-e 或改写最终状态；管道选项由解释器、profile 和命令决定。
    // 非登录模式不能额外加载 rc/profile；PowerShell 仍统一输出编码，避免运行中回执乱码。
    if (shell.type == MaiShellType::PowerShell) {
        // 对齐 Codex 0.172 shell-command/src/powershell.rs 的 UTF8_OUTPUT_PREFIX。
        // manager 的未完成输出是原始字节，必须在解释器写入管道前统一，不能等待退出后转换。
        const std::string prefix =
            "try { [Console]::OutputEncoding=[System.Text.Encoding]::UTF8 } catch {}\n";
        if (!login) return prefix + command;
        std::string literal = "'";
        for (char ch : workdir) literal += ch == '\'' ? "''" : std::string(1, ch);
        literal += "'";
        // 登录 profile 可能改变 provider cwd；使用模块限定的真实 cmdlet，避免同名 alias/function，
        // 并让失败终止脚本，不能在错误目录中继续用户命令。这是明确的工作目录保护。
        // profile 的全局 trap { continue } 能吞掉普通终止错误；本地 catch 显式 exit，
        // 目录恢复失败时不能落入原命令。诊断只含固定文本，不输出异常或环境值。
        return prefix + "try {\n  Microsoft.PowerShell.Management\\Set-Location -LiteralPath " +
               literal +
               " -ErrorAction Stop\n} catch {\n  try { [Console]::Error.WriteLine('Could not "
               "restore requested shell workdir.') } catch {}\n  exit 1\n}\n" +
               command;
    }
    if (!login || shell.type == MaiShellType::Cmd) return command;
    std::string startup;
    if (shell.type == MaiShellType::Zsh) {
        // 与 frozen Codex startup.rs 一致，但使用独立变量避免覆盖用户自己的 rc 变量。
        startup = R"(if [[ -n "${ZDOTDIR-}" ]]; then
  __maichat_shell_startup_rc_20261121="$ZDOTDIR/.zshrc"
elif [[ -n "${HOME-}" ]]; then
  __maichat_shell_startup_rc_20261121="$HOME/.zshrc"
else
  __maichat_shell_startup_rc_20261121=
fi
if [[ -r "$__maichat_shell_startup_rc_20261121" ]]; then
  . "$__maichat_shell_startup_rc_20261121"
fi
unset __maichat_shell_startup_rc_20261121
)";
    } else if (shell.type == MaiShellType::Bash) {
        // 非空 BASH_ENV 由 Bash 自己加载；不在此重复加载 HOME/.bashrc。
        startup = R"(if [ -z "${BASH_ENV-}" ] && [ -n "${HOME-}" ] && [ -r "$HOME/.bashrc" ]; then
  . "$HOME/.bashrc"
fi
)";
    } else if (shell.type == MaiShellType::Sh) {
        startup = shEnvironmentStartup();
    }
    // Windows MSYS 接受 C:/ 路径；POSIX 原生路径中的反斜杠必须保持原字节。
#if defined(_WIN32)
    const auto directory = MaiFilePath::fromUtf8(workdir).toGenericUtf8();
#else
    const auto& directory = workdir;
#endif
    std::string quoted = "'";
    for (char ch : directory) quoted += ch == '\'' ? "'\\''" : std::string(1, ch);
    quoted += "'";
    // 独立输入行让 rc 定义的 alias 在解析原命令时生效；不通过 eval 再解释命令文本。
    startup += shell.type == MaiShellType::Sh ? "command cd -- " : "builtin cd -- ";
    startup += quoted + " || exit $?\n";
    return startup + command;
}
