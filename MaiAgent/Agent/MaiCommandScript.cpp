// 文件作用：先完整解析受限脚本，再通过宿主执行命令与有界字符串管道，不启动系统 shell。
#include "MaiCommandScript.h"

#include <algorithm>
#include <optional>
#include <utility>

namespace {
enum class MaiScriptTokenKind { Word, Separator, And, Or, Pipe, Input, Output, Append };
enum class MaiScriptPartKind { Literal, Variable, Status };
enum class MaiScriptCondition { Always, Success, Failure };

struct MaiScriptPart {
    MaiScriptPartKind kind = MaiScriptPartKind::Literal;
    std::string text;
    bool quoted = false;
};

struct MaiScriptWord {
    std::vector<MaiScriptPart> parts;
    std::string assignmentName;
    bool plain = true;
    std::string literal;
};

struct MaiScriptToken {
    MaiScriptTokenKind kind = MaiScriptTokenKind::Word;
    MaiScriptWord word;
    std::size_t begin = 0;
    std::size_t end = 0;
    bool newline = false;
};

struct MaiScriptAssignment {
    std::string name;
    MaiScriptWord value;
};

struct MaiScriptRedirect {
    MaiScriptWord path;
    bool append = false;
};

struct MaiScriptCommand {
    std::vector<MaiScriptAssignment> assignments;
    std::vector<MaiScriptWord> arguments;
    std::optional<MaiScriptRedirect> input;
    std::optional<MaiScriptRedirect> output;
};

struct MaiScriptPipeline {
    std::vector<MaiScriptCommand> commands;
    MaiScriptCondition condition = MaiScriptCondition::Always;
};

bool isNameStart(unsigned char character) {
    return (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
           character == '_';
}

bool isNameCharacter(unsigned char character) {
    return isNameStart(character) || (character >= '0' && character <= '9');
}

bool isName(const std::string& name) {
    return !name.empty() && isNameStart(name.front()) &&
           std::all_of(name.begin(), name.end(), isNameCharacter);
}

bool isSpace(unsigned char character) {
    return character == ' ' || character == '\t' || character == '\n' || character == '\r' ||
           character == '\f' || character == '\v';
}

bool isCanceled(const MaiCommandScriptOptions& options) {
    return options.isCanceled && options.isCanceled();
}

MaiError canceledError() {
    return {MaiErrorCode::Canceled, "The command script was canceled."};
}

MaiError syntaxError(const std::string& message, std::size_t position) {
    return {MaiErrorCode::InvalidInput, message + " at byte " + std::to_string(position) + "."};
}

bool validScriptText(const std::string& text) {
    // 脚本是 UTF-8 文本；拒绝 NUL、非法编码与隐藏控制字符，参数/文件内容仍按字节保留。
    for (std::size_t index = 0; index < text.size();) {
        const auto first = static_cast<unsigned char>(text[index++]);
        if (first < 0x80) {
            if (first == 0 || (first < 0x20 && first != '\t' && first != '\r' && first != '\n'))
                return false;
            continue;
        }
        const int remaining = first >= 0xc2 && first <= 0xdf   ? 1
                              : first >= 0xe0 && first <= 0xef ? 2
                              : first >= 0xf0 && first <= 0xf4 ? 3
                                                               : -1;
        if (remaining < 0 || text.size() - index < static_cast<std::size_t>(remaining))
            return false;
        unsigned int value = first & (0x7f >> (remaining + 1));
        for (int count = 0; count < remaining; ++count) {
            const auto next = static_cast<unsigned char>(text[index++]);
            if ((next & 0xc0) != 0x80) return false;
            value = (value << 6) | (next & 0x3f);
        }
        if ((remaining == 1 && value < 0x80) || (remaining == 2 && value < 0x800) ||
            (remaining == 3 && value < 0x10000) || value > 0x10ffff ||
            (value >= 0xd800 && value <= 0xdfff))
            return false;
    }
    return true;
}

void appendLiteral(MaiScriptWord& word, const std::string& text, bool quoted) {
    if (!word.parts.empty() && word.parts.back().kind == MaiScriptPartKind::Literal &&
        word.parts.back().quoted == quoted) {
        word.parts.back().text += text;
    } else {
        word.parts.push_back({MaiScriptPartKind::Literal, text, quoted});
    }
}

MaiError parseVariable(const std::string& script, std::size_t& position, MaiScriptWord& word,
                       bool quoted) {
    const auto start = position++;
    word.plain = false;
    if (position >= script.size()) {
        appendLiteral(word, "$", quoted);
        return {};
    }
    if (script[position] == '(')
        return syntaxError("Command and arithmetic substitution are unsupported", start);
    if (script[position] == '?') {
        ++position;
        word.parts.push_back({MaiScriptPartKind::Status, {}, quoted});
        return {};
    }
    std::string name;
    if (script[position] == '{') {
        const auto end = script.find('}', ++position);
        if (end == std::string::npos) return syntaxError("Unclosed variable expansion", start);
        name = script.substr(position, end - position);
        position = end + 1;
        if (!isName(name)) return syntaxError("Unsupported variable expansion", start);
    } else if (isNameStart(script[position])) {
        while (position < script.size() && isNameCharacter(script[position]))
            name += script[position++];
    } else {
        // 特殊参数、位置参数和本地化/ANSI 引号不能被悄悄当成已支持的展开。
        if (std::string("0123456789*@#!-").find(script[position]) != std::string::npos ||
            (!quoted && (script[position] == '\'' || script[position] == '"')))
            return syntaxError("Unsupported special variable or quoting syntax", start);
        appendLiteral(word, "$", quoted);
        return {};
    }
    word.parts.push_back({MaiScriptPartKind::Variable, std::move(name), quoted});
    return {};
}

MaiResult<std::vector<MaiScriptToken>> tokenize(const std::string& script,
                                                const MaiCommandScriptOptions& options) {
    std::vector<MaiScriptToken> tokens;
    std::size_t position = 0;
    while (position < script.size()) {
        if (isCanceled(options)) return canceledError();
        const char character = script[position];
        if (character == ' ' || character == '\t') {
            ++position;
            continue;
        }
        if (character == '#') {
            while (position < script.size() && script[position] != '\n' && script[position] != '\r')
                ++position;
            continue;
        }
        MaiScriptToken token;
        token.begin = position;
        if (character == ';' || character == '\n' || character == '\r') {
            token.kind = MaiScriptTokenKind::Separator;
            token.newline = character != ';';
            ++position;
            if (character == '\r' && position < script.size() && script[position] == '\n')
                ++position;
        } else if (character == '&' || character == '|') {
            const bool doubled = position + 1 < script.size() && script[position + 1] == character;
            if (character == '&' && !doubled)
                return syntaxError("Background commands and descriptor redirection are unsupported",
                                   position);
            token.kind = character == '&' ? MaiScriptTokenKind::And
                         : doubled        ? MaiScriptTokenKind::Or
                                          : MaiScriptTokenKind::Pipe;
            position += doubled ? 2 : 1;
        } else if (character == '<' || character == '>') {
            ++position;
            if (position < script.size() && script[position] == '<')
                return syntaxError("Here-documents and here-strings are unsupported", token.begin);
            if (character == '<' && position < script.size() && script[position] == '>')
                return syntaxError("Read-write redirection is unsupported", token.begin);
            const bool append =
                character == '>' && position < script.size() && script[position] == '>';
            if (append) ++position;
            token.kind = character == '<' ? MaiScriptTokenKind::Input
                         : append         ? MaiScriptTokenKind::Append
                                          : MaiScriptTokenKind::Output;
        } else {
            token.kind = MaiScriptTokenKind::Word;
            auto& word = token.word;
            std::string assignmentPrefix;
            bool assignmentCandidate = true;
            bool anyContent = false;
            while (position < script.size()) {
                const char next = script[position];
                if (isSpace(next) || std::string(";&|<>").find(next) != std::string::npos) break;
                anyContent = true;
                if (next == '\\') {
                    word.plain = false;
                    assignmentCandidate = false;
                    if (++position == script.size())
                        return syntaxError("Trailing escape", position - 1);
                    if (script[position] == '\n') {
                        ++position;
                    } else if (script[position] == '\r' && position + 1 < script.size() &&
                               script[position + 1] == '\n') {
                        position += 2;
                    } else {
                        appendLiteral(word, script.substr(position++, 1), true);
                    }
                } else if (next == '\'' || next == '"') {
                    word.plain = false;
                    assignmentCandidate = false;
                    const char quote = script[position++];
                    appendLiteral(word, {}, true);
                    bool closed = false;
                    while (position < script.size()) {
                        const char inner = script[position];
                        if (inner == quote) {
                            ++position;
                            closed = true;
                            break;
                        }
                        if (quote == '"' && inner == '$') {
                            if (const auto error = parseVariable(script, position, word, true))
                                return error;
                        } else if (quote == '"' && inner == '`') {
                            return syntaxError("Command substitution is unsupported", position);
                        } else if (quote == '"' && inner == '\\') {
                            if (++position == script.size())
                                return syntaxError("Unclosed quoted escape", position - 1);
                            if (script[position] == '\n') {
                                ++position;
                            } else if (std::string("$`\"\\").find(script[position]) !=
                                       std::string::npos) {
                                appendLiteral(word, script.substr(position++, 1), true);
                            } else {
                                appendLiteral(word, "\\", true);
                            }
                        } else {
                            appendLiteral(word, script.substr(position++, 1), true);
                        }
                    }
                    if (!closed) return syntaxError("Unclosed quote", token.begin);
                } else if (next == '$') {
                    assignmentCandidate = false;
                    if (const auto error = parseVariable(script, position, word, false))
                        return error;
                } else {
                    if (std::string("`(){}*?[]~").find(next) != std::string::npos)
                        return syntaxError(
                            "Unsupported substitution, grouping, glob, or tilde syntax", position);
                    if (assignmentCandidate) {
                        if (next == '=') {
                            if (isName(assignmentPrefix)) word.assignmentName = assignmentPrefix;
                            assignmentCandidate = false;
                        } else {
                            assignmentPrefix += next;
                        }
                    }
                    word.literal += next;
                    appendLiteral(word, script.substr(position++, 1), false);
                }
            }
            // 只有行续接的 token 不应凭空生成一个空 argv。
            if (word.parts.empty() && anyContent) continue;
        }
        token.end = position;
        tokens.push_back(std::move(token));
    }
    return tokens;
}

bool isReservedWord(const MaiScriptWord& word) {
    if (!word.plain) return false;
    static const std::vector<std::string> words{
        "if",    "then", "else", "elif",     "fi",     "for", "in",   "do", "done", "while",
        "until", "case", "esac", "function", "select", "!",   "time", "[[", "]]"};
    return std::find(words.begin(), words.end(), word.literal) != words.end();
}

MaiResult<std::vector<MaiScriptPipeline>> parse(const std::vector<MaiScriptToken>& tokens,
                                                const MaiCommandScriptOptions& options) {
    std::vector<MaiScriptPipeline> pipelines;
    MaiScriptCondition condition = MaiScriptCondition::Always;
    std::size_t position = 0;
    std::size_t commandCount = 0;
    const auto skipNewlines = [&] {
        while (position < tokens.size() && tokens[position].kind == MaiScriptTokenKind::Separator &&
               tokens[position].newline)
            ++position;
    };
    skipNewlines();
    while (position < tokens.size()) {
        MaiScriptPipeline pipeline;
        pipeline.condition = condition;
        while (true) {
            MaiScriptCommand command;
            std::size_t wordCount = 0;
            bool present = false;
            while (position < tokens.size()) {
                const auto& token = tokens[position];
                if (token.kind != MaiScriptTokenKind::Word &&
                    token.kind != MaiScriptTokenKind::Input &&
                    token.kind != MaiScriptTokenKind::Output &&
                    token.kind != MaiScriptTokenKind::Append)
                    break;
                present = true;
                if (++wordCount > options.maxArguments)
                    return syntaxError("Command word budget exceeded", token.begin);
                if (token.kind == MaiScriptTokenKind::Word) {
                    if (position + 1 < tokens.size() && token.word.plain &&
                        !token.word.literal.empty() && token.end == tokens[position + 1].begin &&
                        (tokens[position + 1].kind == MaiScriptTokenKind::Input ||
                         tokens[position + 1].kind == MaiScriptTokenKind::Output ||
                         tokens[position + 1].kind == MaiScriptTokenKind::Append) &&
                        std::all_of(
                            token.word.literal.begin(), token.word.literal.end(),
                            [](unsigned char value) { return value >= '0' && value <= '9'; }))
                        return syntaxError("File descriptor redirection is unsupported",
                                           token.begin);
                    if (command.arguments.empty() && !token.word.assignmentName.empty()) {
                        auto value = token.word;
                        value.parts.front().text.erase(0, value.assignmentName.size() + 1);
                        command.assignments.push_back({value.assignmentName, std::move(value)});
                    } else {
                        if (command.arguments.empty() && isReservedWord(token.word))
                            return syntaxError(
                                "Shell control flow and reserved words are unsupported",
                                token.begin);
                        command.arguments.push_back(token.word);
                    }
                    ++position;
                } else {
                    const bool input = token.kind == MaiScriptTokenKind::Input;
                    if ((input && command.input) || (!input && command.output))
                        return syntaxError("Repeated redirection is unsupported", token.begin);
                    if (++position >= tokens.size() ||
                        tokens[position].kind != MaiScriptTokenKind::Word)
                        return syntaxError("Redirection requires a file word", token.begin);
                    MaiScriptRedirect redirect{tokens[position++].word,
                                               token.kind == MaiScriptTokenKind::Append};
                    if (input)
                        command.input = std::move(redirect);
                    else
                        command.output = std::move(redirect);
                }
            }
            if (!present)
                return syntaxError("A command is required",
                                   position < tokens.size() ? tokens[position].begin : 0);
            if (++commandCount > options.maxCommands)
                return syntaxError("Script command budget exceeded",
                                   position < tokens.size() ? tokens[position].begin : 0);
            pipeline.commands.push_back(std::move(command));
            if (pipeline.commands.size() > options.maxPipelineCommands)
                return syntaxError("Pipeline command budget exceeded",
                                   position < tokens.size() ? tokens[position].begin : 0);
            if (position == tokens.size() || tokens[position].kind != MaiScriptTokenKind::Pipe)
                break;
            ++position;
            skipNewlines();
        }
        pipelines.push_back(std::move(pipeline));
        if (position == tokens.size()) break;
        const auto& operation = tokens[position++];
        if (operation.kind == MaiScriptTokenKind::And || operation.kind == MaiScriptTokenKind::Or) {
            condition = operation.kind == MaiScriptTokenKind::And ? MaiScriptCondition::Success
                                                                  : MaiScriptCondition::Failure;
            skipNewlines();
            if (position == tokens.size())
                return syntaxError("A conditional command is required", operation.begin);
        } else if (operation.kind == MaiScriptTokenKind::Separator) {
            condition = MaiScriptCondition::Always;
            skipNewlines();
        } else {
            return syntaxError("Unexpected command operator", operation.begin);
        }
    }
    return pipelines;
}

MaiResult<std::vector<std::string>> expand(const MaiScriptWord& word,
                                           const std::map<std::string, std::string>& variables,
                                           int status, const MaiCommandScriptOptions& options,
                                           bool split) {
    std::vector<std::string> fields;
    std::string current;
    bool present = !split;
    std::size_t total = 0;
    for (const auto& part : word.parts) {
        std::string value;
        if (part.kind == MaiScriptPartKind::Literal)
            value = part.text;
        else if (part.kind == MaiScriptPartKind::Status)
            value = std::to_string(status);
        else {
            const auto found = variables.find(part.text);
            if (found != variables.end()) value = found->second;
        }
        if (value.size() > options.maxExpandedBytes - total)
            return {MaiErrorCode::InvalidInput, "Command expansion byte budget exceeded."};
        total += value.size();
        if (part.kind == MaiScriptPartKind::Literal || part.quoted || !split) {
            current += value;
            present = present || part.quoted || !value.empty();
        } else {
            for (const unsigned char character : value) {
                if (isSpace(character)) {
                    if (present) {
                        fields.push_back(std::move(current));
                        current.clear();
                        present = false;
                    }
                } else {
                    current += static_cast<char>(character);
                    present = true;
                }
                if (fields.size() > options.maxArguments)
                    return {MaiErrorCode::InvalidInput, "Expanded argument budget exceeded."};
            }
        }
    }
    if (present) fields.push_back(std::move(current));
    if (fields.size() > options.maxArguments)
        return {MaiErrorCode::InvalidInput, "Expanded argument budget exceeded."};
    return fields;
}

MaiResult<MaiFilePath> redirectPath(const MaiScriptWord& word,
                                    const std::map<std::string, std::string>& variables, int status,
                                    const MaiFilePath& directory,
                                    const MaiCommandScriptOptions& options) {
    const auto fields = expand(word, variables, status, options, true);
    if (!fields) return fields.error();
    if (fields.value().size() != 1 || fields.value().front().empty() ||
        fields.value().front().find('\0') != std::string::npos)
        return {MaiErrorCode::InvalidInput, "Redirection requires one nonempty file path."};
    return directory.append(MaiFilePath::fromUtf8(fields.value().front()));
}

bool variablesFit(const std::map<std::string, std::string>& variables, std::size_t limit) {
    std::size_t bytes = 0;
    for (const auto& value : variables) {
        if (value.first.size() > limit - bytes) return false;
        bytes += value.first.size();
        if (value.second.size() > limit - bytes) return false;
        bytes += value.second.size();
    }
    return true;
}

void appendOutput(MaiCommandExecution& total, std::string& target, const std::string& value,
                  const MaiCommandScriptOptions& options) {
    const auto remaining =
        options.maxOutputBytes - total.standardOutput.size() - total.standardError.size();
    auto kept = std::min(remaining, value.size());
    if (kept < value.size() && (static_cast<unsigned char>(value[kept]) & 0xc0) == 0x80) {
        // 输出仍是原字节前缀；只在确认切断了完整 UTF-8 字符时退回字符起点。
        auto start = kept;
        while (start > 0 && (static_cast<unsigned char>(value[start]) & 0xc0) == 0x80) --start;
        const auto lead = static_cast<unsigned char>(value[start]);
        const std::size_t length = lead >= 0xc2 && lead <= 0xdf   ? 2
                                   : lead >= 0xe0 && lead <= 0xef ? 3
                                   : lead >= 0xf0 && lead <= 0xf4 ? 4
                                                                  : 0;
        if (length > kept - start && length <= value.size() - start &&
            std::all_of(value.begin() + start + 1, value.begin() + start + length,
                        [](unsigned char character) { return (character & 0xc0) == 0x80; }))
            kept = start;
    }
    target.append(value, 0, kept);
    if (value.size() > remaining) total.outputTruncated = true;
}

MaiError validateHost(const std::vector<MaiScriptPipeline>& pipelines,
                      const MaiCommandScriptHost& host) {
    // 回调缺失是配置问题，在任何前缀命令执行前报告；动态命令名仍在实际展开后检查。
    for (const auto& pipeline : pipelines)
        for (const auto& command : pipeline.commands) {
            if (command.input && !host.read)
                return {MaiErrorCode::NotConfigured, "Script input reader is unavailable."};
            if (command.output && !host.write)
                return {MaiErrorCode::NotConfigured, "Script output writer is unavailable."};
            if (command.arguments.empty()) continue;
            const auto& name = command.arguments.front();
            if (name.plain && name.literal == "pwd") continue;
            if (name.plain && name.literal == "cd") {
                if (!host.changeDirectory)
                    return {MaiErrorCode::NotConfigured,
                            "Script directory resolver is unavailable."};
            } else if (!host.execute) {
                return {MaiErrorCode::NotConfigured, "Script command executor is unavailable."};
            }
        }
    return {};
}

MaiResult<MaiCommandExecution> executeCommand(const MaiScriptCommand& command,
                                              std::map<std::string, std::string>& variables,
                                              MaiFilePath& directory, int status,
                                              const std::string& input,
                                              const MaiCommandScriptOptions& options,
                                              const MaiCommandScriptHost& host) {
    if (isCanceled(options)) return canceledError();
    MaiCommandInvocation invocation;
    invocation.workingDirectory = directory;
    invocation.standardInput = input;
    invocation.variables = variables;
    for (const auto& assignment : command.assignments) {
        const auto value = expand(assignment.value, invocation.variables, status, options, false);
        if (!value) return value.error();
        invocation.variables[assignment.name] = value.value().front();
        if (!variablesFit(invocation.variables, options.maxExpandedBytes))
            return {MaiErrorCode::InvalidInput, "Script variable byte budget exceeded."};
    }
    std::size_t argumentBytes = 0;
    for (const auto& argument : command.arguments) {
        const auto fields = expand(argument, variables, status, options, true);
        if (!fields) return fields.error();
        for (const auto& field : fields.value()) {
            if (field.size() > options.maxExpandedBytes - argumentBytes)
                return {MaiErrorCode::InvalidInput, "Command argument byte budget exceeded."};
            argumentBytes += field.size();
            invocation.arguments.push_back(field);
            if (invocation.arguments.size() > options.maxArguments)
                return {MaiErrorCode::InvalidInput, "Expanded argument budget exceeded."};
        }
    }
    if (!invocation.arguments.empty() && invocation.arguments.front().empty())
        return {MaiErrorCode::InvalidInput, "The expanded command name is empty."};
    std::optional<MaiFilePath> outputPath;
    if (command.input) {
        const auto path = redirectPath(command.input->path, variables, status, directory, options);
        if (!path) return path.error();
        if (isCanceled(options)) return canceledError();
        const auto contents = host.read(path.value());
        if (!contents) return contents.error();
        if (contents.value().size() > options.maxOutputBytes)
            return {MaiErrorCode::InvalidInput, "Script input byte budget exceeded."};
        invocation.standardInput = contents.value();
    }
    if (command.output) {
        const auto path = redirectPath(command.output->path, variables, status, directory, options);
        if (!path) return path.error();
        outputPath = path.value();
    }
    if (isCanceled(options)) return canceledError();
    MaiCommandExecution execution;
    if (invocation.arguments.empty()) {
        variables = std::move(invocation.variables);
    } else if (invocation.arguments.front() == "pwd") {
        if (invocation.arguments.size() == 1)
            execution.standardOutput = directory.toGenericUtf8() + "\n";
        else {
            execution.exitCode = 2;
            execution.standardError = "pwd accepts no arguments.\n";
        }
    } else if (invocation.arguments.front() == "cd") {
        if (invocation.arguments.size() != 2) {
            execution.exitCode = 2;
            execution.standardError = "cd requires exactly one directory argument.\n";
        } else {
            if (!host.changeDirectory)
                return {MaiErrorCode::NotConfigured, "Script directory resolver is unavailable."};
            const auto changed = host.changeDirectory(
                directory.append(MaiFilePath::fromUtf8(invocation.arguments[1])));
            if (!changed) return changed.error();
            if (changed.value().isEmpty() || !changed.value().isAbsolute())
                return {MaiErrorCode::Internal, "The host returned an invalid working directory."};
            variables["OLDPWD"] = directory.toGenericUtf8();
            directory = changed.value();
            variables["PWD"] = directory.toGenericUtf8();
            if (!variablesFit(variables, options.maxExpandedBytes))
                return {MaiErrorCode::InvalidInput, "Script variable byte budget exceeded."};
        }
    } else {
        if (!host.execute)
            return {MaiErrorCode::NotConfigured, "Script command executor is unavailable."};
        const auto result = host.execute(invocation);
        if (!result) return result.error();
        execution = result.value();
    }
    if (isCanceled(options)) return canceledError();
    if (outputPath) {
        if (execution.outputTruncated || execution.standardOutput.size() > options.maxOutputBytes)
            return {MaiErrorCode::InvalidInput,
                    "Cannot redirect incomplete or oversized command output."};
        if (const auto error =
                host.write(*outputPath, execution.standardOutput, command.output->append))
            return error;
        execution.standardOutput.clear();
    }
    return execution;
}
}  // namespace

MaiResult<MaiCommandExecution> maiRunCommandScript(const std::string& script,
                                                   const MaiCommandScriptOptions& options,
                                                   const MaiCommandScriptHost& host) {
    if (options.maxScriptBytes == 0 || options.maxCommands == 0 || options.maxArguments == 0 ||
        options.maxPipelineCommands == 0 || options.maxOutputBytes == 0 ||
        options.maxExpandedBytes == 0 || script.size() > options.maxScriptBytes ||
        !validScriptText(script) || options.workingDirectory.isEmpty() ||
        !options.workingDirectory.isAbsolute())
        return {MaiErrorCode::InvalidInput,
                "Invalid script text, working directory, or command budgets."};
    if (isCanceled(options)) return canceledError();
    const auto tokens = tokenize(script, options);
    if (!tokens) return tokens.error();
    const auto pipelines = parse(tokens.value(), options);
    if (!pipelines) return pipelines.error();
    if (const auto error = validateHost(pipelines.value(), host)) return error;
    MaiCommandExecution execution;
    MaiFilePath directory = options.workingDirectory;
    std::map<std::string, std::string> variables{{"PWD", directory.toGenericUtf8()}};
    if (!variablesFit(variables, options.maxExpandedBytes))
        return {MaiErrorCode::InvalidInput, "Initial script variable byte budget exceeded."};
    for (const auto& pipeline : pipelines.value()) {
        if (isCanceled(options)) return canceledError();
        if ((pipeline.condition == MaiScriptCondition::Success && execution.exitCode != 0) ||
            (pipeline.condition == MaiScriptCondition::Failure && execution.exitCode == 0))
            continue;
        std::string pipeInput;
        const int previousStatus = execution.exitCode;
        for (std::size_t index = 0; index < pipeline.commands.size(); ++index) {
            auto localVariables = variables;
            auto localDirectory = directory;
            const bool isolated = pipeline.commands.size() > 1;
            const auto result = executeCommand(
                pipeline.commands[index], isolated ? localVariables : variables,
                isolated ? localDirectory : directory, previousStatus, pipeInput, options, host);
            if (!result) return result.error();
            execution.exitCode = result.value().exitCode;
            execution.outputTruncated = execution.outputTruncated || result.value().outputTruncated;
            appendOutput(execution, execution.standardError, result.value().standardError, options);
            const bool last = index + 1 == pipeline.commands.size();
            if (!last) {
                if (result.value().outputTruncated ||
                    result.value().standardOutput.size() > options.maxOutputBytes)
                    return {MaiErrorCode::InvalidInput,
                            "Cannot pipe incomplete or oversized command output."};
                pipeInput = result.value().standardOutput;
            } else {
                appendOutput(execution, execution.standardOutput, result.value().standardOutput,
                             options);
            }
        }
    }
    if (isCanceled(options)) return canceledError();
    return execution;
}
