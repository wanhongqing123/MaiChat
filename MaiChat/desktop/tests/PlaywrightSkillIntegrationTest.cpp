// 文件作用：用官方 Playwright 技能和真实浏览器验证技能上下文到普通 shell 工具的完整路径。
// 模型按脚本确定性调用工具；本用例验证接线和执行，不声称验证真实大模型的自主技能选择。
#include <atomic>
#include <chrono>
#include <cstdio>
#include <functional>
#include <memory>
#include <mutex>
#include <regex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <QApplication>
#include <QEventLoop>
#include <QTimer>

#include <httplib.h>
#include <json.hpp>

#include "MaiAgent.h"
#include "MaiFileSystem.h"
#include "MaiIdGenerator.h"
#include "MaiThread.h"
#include "agent/AgentController.h"

using nlohmann::json;

static std::atomic<int> failures{0};
#define CHECK(value)                                                    \
    do {                                                                \
        if (!(value)) {                                                 \
            std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #value); \
            ++failures;                                                 \
        }                                                               \
    } while (0)

static MaiFilePath child(const MaiFilePath& root, const std::string& name) {
    return root.append(MaiFilePath::fromUtf8(name));
}

// PowerShell 单引号只通过双写转义，不把路径或表单文本当作表达式执行。
static std::string quote(const std::string& value) {
    std::string result = "'";
    for (const char character : value) {
        result += character;
        if (character == '\'') result += '\'';
    }
    return result + "'";
}

static bool copySkill(const MaiFilePath& source, const MaiFilePath& destination) {
    bool success = !MaiFileSystem::createDirectories(destination);
    MaiFileSystem::walk(
        source,
        [&](const MaiFileEntry& entry) {
            const auto relative = entry.path.toGenericUtf8().substr(source.toGenericUtf8().size() + 1);
            const auto target = child(destination, relative);
            const auto error = entry.isDirectory ? MaiFileSystem::createDirectories(target)
                                                 : MaiFileSystem::copyFile(entry.path, target);
            if (error) {
                std::printf("Copy failed: %s\n", error.message().c_str());
                success = false;
            }
            return MaiWalkAction::Continue;
        },
        [&](const MaiFilePath&, const MaiError& error) {
            std::printf("Walk failed: %s\n", error.message().c_str());
            success = false;
        });
    return success;
}

class MaiPlaywrightSmokePage {
public:
    MaiPlaywrightSmokePage() {
        mServer.Get("/", [](const httplib::Request&, httplib::Response& response) {
            response.set_content(R"HTML(<!doctype html><html><head><meta charset="utf-8">
<title>MaiChat Skills smoke</title></head><body><h1>Owned skills test</h1>
<form id="tasks"><label>Task <input name="task" id="task"></label>
<button type="submit">Add</button></form><output id="result"></output>
<script>document.querySelector('#tasks').addEventListener('submit',async event=>{
event.preventDefault();const task=document.querySelector('#task').value;
const response=await fetch('/record',{method:'POST',headers:{'Content-Type':'application/json'},
body:JSON.stringify({task})});if(response.ok){document.querySelector('#result').textContent=task;
document.title='MAICHAT_SKILL_PASS_1';}});</script></body></html>)HTML",
                                 "text/html");
        });
        mServer.Post("/record", [this](const httplib::Request& request, httplib::Response& response) {
            const auto body = json::parse(request.body, nullptr, false);
            if (!body.is_object() || !body.contains("task") || !body["task"].is_string()) {
                response.status = 400;
                return;
            }
            std::lock_guard<std::mutex> lock(mMutex);
            mTasks.push_back(body["task"].get<std::string>());
            response.set_content("{}", "application/json");
        });
        mPort = mServer.bind_to_any_port("127.0.0.1");
        if (mPort > 0)
            mThread = std::thread([this] {
                MaiThread::setCurrentName("mai-skill-page");
                mServer.listen_after_bind();
            });
    }
    ~MaiPlaywrightSmokePage() {
        mServer.stop();
        if (mThread.joinable()) mThread.join();
    }
    std::string url() const {
        return "http://127.0.0.1:" + std::to_string(mPort) + "/";
    }
    bool isValid() const {
        return mPort > 0;
    }
    std::vector<std::string> tasks() {
        std::lock_guard<std::mutex> lock(mMutex);
        return mTasks;
    }

private:
    httplib::Server mServer;
    std::thread mThread;
    int mPort = -1;
    std::mutex mMutex;
    std::vector<std::string> mTasks;
};

class MaiPlaywrightScriptModel final : public MaiModelClient {
public:
    std::function<MaiError(const MaiModelRequest&, const MaiStreamSink&)> respond;
    MaiError stream(const MaiModelRequest& request, const MaiStreamSink& sink,
                    const std::atomic<bool>& cancel) override {
        if (cancel.load()) return {MaiErrorCode::Canceled, "Smoke test canceled"};
        return respond(request, sink);
    }
    MaiWireApi wireApi() const override {
        return MaiWireApi::Responses;
    }
};

struct MaiPlaywrightSmokeOptions {
    MaiFilePath skillSource;
    MaiFilePath cli;
    MaiFilePath evidence;
    std::string browser;
    std::string scenario = "preinstalled";
};

static std::string toolResult(const MaiModelRequest& request, const std::string& id) {
    for (const auto& message : request.messages)
        if (message.role == MaiModelRole::ToolResult && message.toolCallId == id) return message.content;
    return {};
}

static bool scenario(const MaiPlaywrightSmokeOptions& settings, MaiPlaywrightSmokePage& page,
                     const std::string& officialContents, bool explicitSelection, bool fromUrl = false) {
    const int previousFailures = failures.load();
    const std::string name = fromUrl ? "url-install" : explicitSelection ? "user-explicit" : "workspace-implicit";
    const auto root = child(settings.evidence, name);
    const auto workspace = child(root, "workspace");
    const auto home = child(settings.evidence, "home");
    const auto skillDirectory = child(explicitSelection ? home : workspace, ".agents/skills/playwright-cli");
    CHECK(!MaiFileSystem::exists(root));
    if (MaiFileSystem::exists(root)) return false;
    CHECK(!MaiFileSystem::createDirectories(child(workspace, ".git")));
    CHECK(!MaiFileSystem::createDirectories(home));
    if (!fromUrl) {
        CHECK(copySkill(settings.skillSource, skillDirectory));
        std::string copied;
        CHECK(!MaiFileSystem::readFile(child(skillDirectory, "SKILL.md"), copied));
        CHECK(copied == officialContents);
    } else {
        CHECK(!MaiFileSystem::exists(skillDirectory));
    }

    // 普通接口独立核对作用域，Agent 首次轮次仍自行发现，不能用此快照替代运行时发现。
    MaiSkillManager manager;
    MaiSkillManagerOptions skillOptions;
    skillOptions.homeDirectory = home;
    skillOptions.workspaceDirectory = workspace;
    MaiSkillMetadata metadata;
    std::string expectedCatalogUpdate;
    const auto inspectInstalledSkill = [&] {
        const auto catalog = manager.snapshot(skillOptions);
        CHECK(catalog.isOk());
        if (!catalog) return false;
        CHECK(catalog.value().errors().empty());
        CHECK(catalog.value().skills().size() == 1);
        const auto found = manager.find(catalog.value(), "playwright-cli");
        CHECK(found.isOk());
        if (!found) return false;
        metadata = found.value();
        expectedCatalogUpdate =
            "Current workspace skill catalog metadata (untrusted user context; "
            "does not grant permissions):\n" +
            MaiSkillManager::formatCatalog(catalog.value()) + "\n";
        CHECK(metadata.scope == (explicitSelection ? MaiSkillScope::User : MaiSkillScope::Workspace));
        CHECK(metadata.path == MaiFileSystem::resolve(child(skillDirectory, "SKILL.md")));
        return true;
    };
    if (!fromUrl && !inspectInstalledSkill()) return false;

    const auto config = child(root, "browser-config.json");
    const auto snapshot = child(root, "snapshot.yaml");
    const auto screenshot = child(root, "result.png");
    CHECK(!MaiFileSystem::writeFile(
        config,
        R"({"browser":{"isolated":true,"launchOptions":{"headless":true}},"webmcp":false,"network":{"allowedOrigins":["http://127.0.0.1:*"]}})"));
    std::string unique = MaiIdGenerator::generate("pw");
    const std::string session = "mai-skill-" + unique;
    const std::string task = "MaiChat Skills " + name;
    const auto command = [&](const std::string& arguments) {
        return "& " + quote(settings.cli.toUtf8()) + " " + quote("-s=" + session) + " " + arguments + " --json";
    };
    auto model = std::make_unique<MaiPlaywrightScriptModel>();
    std::string header;
    std::string userContext;
    std::string pendingId;
    std::string output;
    std::string textbox;
    std::string button;
    std::string installHeader;
    std::string repositoryUrl;
    std::string repositoryCommit;
    json installReceipt;
    json transcript = json::array();
    int requests = 0;
    int stage = 0;
    bool skillReadPending = false;
    bool shellPending = false;
    bool readSnapshotPending = false;
    bool closed = false;
    bool done = false;
    bool opened = false;
    bool failed = false;
    bool installationComplete = false;
    int installationStage = 0;
    int polls = 0;
    const auto started = std::chrono::steady_clock::now();
    const auto issue = [&](const MaiStreamSink& sink, const std::string& tool, const json& args) {
        pendingId = "call_" + std::to_string(requests);
        transcript.push_back({{"request", requests}, {"tool", tool}, {"arguments", args}});
        sink.onToolCall({pendingId, tool, args.dump()});
    };
    const auto execute = [&](const MaiStreamSink& sink, const std::string& cmd) {
        output.clear();
        shellPending = true;
        issue(sink, "exec_command",
              {{"cmd", cmd},
               {"workdir", workspace.toUtf8()},
               {"shell", "C:/Windows/System32/WindowsPowerShell/v1.0/powershell.exe"},
               {"login", false},
               {"yield_time_ms", 10000},
               {"max_output_tokens", 12000}});
    };
    model->respond = [&](const MaiModelRequest& request, const MaiStreamSink& sink) -> MaiError {
        ++requests;
        const auto reject = [&](const std::string& reason) {
            std::printf("FAIL %s: %s\n", name.c_str(), reason.c_str());
            transcript.push_back({{"failure", reason}});
            ++failures;
            failed = true;
        };
        if (requests > 100 || std::chrono::steady_clock::now() - started > std::chrono::seconds(fromUrl ? 350 : 95)) {
            reject("Bounded model request/time budget exceeded");
            return {MaiErrorCode::Canceled, "Smoke test budget exceeded"};
        }
        // URL 路径先让产品工具执行真实远端发现和安装。只使用返回的仓库链接、路径及提交，
        // 不把测试夹具复制到目标，也不把框架仓库内的维护技能当作浏览器自动化技能。
        if (fromUrl && !installationComplete) {
            if (installationStage == 0) {
                installHeader = request.baseInstructions;
                CHECK(!installHeader.empty());
                CHECK(installHeader.find("# Browser Automation with playwright-cli") == std::string::npos);
                bool declared = false;
                for (const auto& spec : request.tools) declared = declared || spec.name == "skill_install";
                CHECK(declared);
                CHECK(!MaiFileSystem::exists(skillDirectory));
                issue(sink, "skill_install",
                      {{"mode", "discover"}, {"url", "https://github.com/microsoft/playwright"}});
                ++installationStage;
                return {};
            }
            CHECK(request.baseInstructions == installHeader);
            const auto result = toolResult(request, pendingId);
            transcript.push_back({{"request", requests}, {"result", result}});
            const auto reply = json::parse(result, nullptr, false);
            if (!reply.is_object()) {
                reject("skill_install returned no JSON object: " + result);
                return {MaiErrorCode::Protocol, "Missing installer response"};
            }
            if (installationStage == 1) {
                CHECK(reply.value("source_url", "") == "https://github.com/microsoft/playwright");
                CHECK(reply.contains("candidates") && reply["candidates"].is_array());
                CHECK(reply.contains("readme_links") && reply["readme_links"].is_array());
                if (reply.contains("readme_links") && reply["readme_links"].is_array()) {
                    for (const auto& link : reply["readme_links"]) {
                        const auto url = link.value("url", "");
                        if (url == "https://github.com/microsoft/playwright-cli") repositoryUrl = url;
                    }
                }
                if (repositoryUrl.empty()) {
                    reject("Framework README did not return the official CLI skill repository link");
                    return {MaiErrorCode::Protocol, "Official skill repository link missing"};
                }
                issue(sink, "skill_install", {{"mode", "discover"}, {"url", repositoryUrl}});
            } else if (installationStage == 2) {
                CHECK(reply.value("source_url", "") == repositoryUrl);
                repositoryCommit = reply.value("commit", "");
                CHECK(std::regex_match(repositoryCommit, std::regex("[0-9a-f]{40}")));
                bool intendedPath = false;
                if (reply.contains("candidates") && reply["candidates"].is_array())
                    for (const auto& candidate : reply["candidates"])
                        intendedPath = intendedPath || candidate.value("path", "") == "skills/playwright-cli";
                if (!intendedPath || repositoryCommit.empty()) {
                    reject("Official CLI repository did not return the intended skill path and pinned commit");
                    return {MaiErrorCode::Protocol, "Official skill candidate missing"};
                }
                issue(sink, "skill_install",
                      {{"mode", "install"},
                       {"url", repositoryUrl},
                       {"path", "skills/playwright-cli"},
                       {"ref", repositoryCommit}});
            } else {
                installReceipt = reply;
                CHECK(reply.value("installed", false));
                CHECK(reply.value("name", "") == "playwright-cli");
                CHECK(reply.value("scope", "") == "user");
                CHECK(reply.value("source_url", "") == repositoryUrl);
                CHECK(reply.value("commit", "") == repositoryCommit);
                CHECK(reply.value("source_path", "") == "skills/playwright-cli");
                CHECK(reply.contains("dependencies_installed") && reply["dependencies_installed"] == false);
                CHECK(reply.contains("scripts_executed") && reply["scripts_executed"] == false);
                CHECK(reply.value("available_on", "") == "next_user_input");
                CHECK(reply.value("file_count", 0) == 11);
                if (!inspectInstalledSkill()) return {MaiErrorCode::Protocol, "Installed skill is undiscoverable"};
                CHECK(reply.value("skill_id", "") == metadata.id);
                std::size_t comparedFiles = 0;
                MaiFileSystem::walk(
                    settings.skillSource,
                    [&](const MaiFileEntry& entry) {
                        if (entry.isDirectory) return MaiWalkAction::Continue;
                        const auto relative =
                            entry.path.toGenericUtf8().substr(settings.skillSource.toGenericUtf8().size() + 1);
                        std::string expected, actual;
                        CHECK(!MaiFileSystem::readFile(entry.path, expected));
                        CHECK(!MaiFileSystem::readFile(child(skillDirectory, relative), actual));
                        CHECK(actual == expected);
                        ++comparedFiles;
                        return MaiWalkAction::Continue;
                    },
                    [&](const MaiFilePath&, const MaiError&) { CHECK(false); });
                CHECK(comparedFiles == 11);
                std::size_t installedFiles = 0;
                MaiFileSystem::walk(
                    skillDirectory,
                    [&](const MaiFileEntry& entry) {
                        if (!entry.isDirectory) ++installedFiles;
                        return MaiWalkAction::Continue;
                    },
                    [&](const MaiFilePath&, const MaiError&) { CHECK(false); });
                CHECK(installedFiles == 12);
                std::string provenanceBytes;
                CHECK(!MaiFileSystem::readFile(child(skillDirectory, ".mai-skill-source.json"), provenanceBytes));
                const auto provenance = json::parse(provenanceBytes, nullptr, false);
                CHECK(provenance.is_object());
                if (provenance.is_object()) {
                    CHECK(provenance.value("source_url", "") == repositoryUrl);
                    CHECK(provenance.value("commit", "") == repositoryCommit);
                    CHECK(provenance.value("path", "") == "skills/playwright-cli");
                    CHECK(provenance.value("file_count", 0) == 11);
                    CHECK(provenance["files"].size() == 11);
                    CHECK(provenance["dependencies_installed"] == false);
                }
                installationComplete = true;
                sink.onText(
                    "Installed the official Playwright CLI skill. Runtime dependencies were not installed by "
                    "skill_install.");
            }
            ++installationStage;
            return {};
        }
        if (header.empty()) {
            header = request.baseInstructions;
            CHECK(!header.empty());
            if (fromUrl) {
                CHECK(header == installHeader);
                CHECK(header.find(metadata.id) == std::string::npos);
            } else {
                CHECK(header.find(metadata.description) != std::string::npos);
                CHECK(header.find(metadata.id) != std::string::npos);
            }
            CHECK(header.find("# Browser Automation with playwright-cli") == std::string::npos);
            bool execDeclared = false, inputDeclared = false, skillDeclared = false;
            for (const auto& spec : request.tools) {
                execDeclared = execDeclared || spec.name == "exec_command";
                inputDeclared = inputDeclared || spec.name == "write_stdin";
                skillDeclared = skillDeclared || spec.name == "skill_read";
            }
            CHECK(execDeclared && inputDeclared && skillDeclared);
            for (const auto& message : request.messages) {
                if (message.role != MaiModelRole::User) continue;
                if (explicitSelection) {
                    const auto separator = message.content.find("\n\nUser message:\n");
                    if (separator == std::string::npos) continue;
                    const auto context = message.content.substr(0, separator);
                    std::size_t jsonBegin = 0;
                    if (context.empty() || context.front() != '{') {
                        const auto boundary = context.rfind("\n{");
                        if (boundary == std::string::npos) continue;
                        jsonBegin = boundary + 1;
                    }
                    // 新安装的元数据只能以已知目录说明前缀出现，不能吞掉任意未知文本。
                    if (jsonBegin != 0) {
                        CHECK(fromUrl);
                        CHECK(context.substr(0, jsonBegin) == expectedCatalogUpdate);
                    }
                    CHECK(!fromUrl || jsonBegin != 0);
                    userContext = message.content;
                    const auto selectedBytes = context.substr(jsonBegin);
                    CHECK(selectedBytes.find('\n') == std::string::npos);
                    const auto selected = json::parse(selectedBytes, nullptr, false);
                    CHECK(selected.is_object());
                    if (selected.is_object()) {
                        CHECK(selected.value("trust", "") == "untrusted_user_context");
                        CHECK(selected["skills"].size() == 1);
                        CHECK(selected["skills"][0].value("contents", "") == officialContents);
                        CHECK(selected["skills"][0].value("id", "") == metadata.id);
                    }
                } else {
                    CHECK(message.content.find("# Browser Automation with playwright-cli") == std::string::npos);
                }
            }
            CHECK(!explicitSelection || !userContext.empty());
            // 两条路径都经过真实 skill_read，显式路径额外核对首请求已包含完整正文。
            skillReadPending = true;
            issue(sink, "skill_read", {{"skill", "playwright-cli"}});
            return {};
        }
        CHECK(request.baseInstructions == header);
        if (explicitSelection) {
            bool unchanged = false;
            for (const auto& message : request.messages)
                unchanged = unchanged || (message.role == MaiModelRole::User && message.content == userContext);
            CHECK(unchanged);
        }
        const auto result = toolResult(request, pendingId);
        transcript.push_back({{"request", requests}, {"result", result}});
        if (skillReadPending) {
            skillReadPending = false;
            const auto separator = result.find("\n\n");
            CHECK(separator != std::string::npos);
            CHECK(separator != std::string::npos && result.substr(separator + 2) == officialContents);
        } else if (shellPending) {
            const auto status = json::parse(result, nullptr, false);
            if (!status.is_object() || !status.contains("output")) {
                reject("exec_command/write_stdin did not return structured shell output: " + result);
            } else {
                output += status.value("output", "");
                if (status.contains("session_id")) {
                    if (++polls > 40) {
                        reject("Too many shell continuation polls");
                    } else {
                        issue(sink, "write_stdin",
                              {{"session_id", status["session_id"]},
                               {"chars", ""},
                               {"yield_time_ms", 10000},
                               {"max_output_tokens", 12000}});
                        return {};
                    }
                } else if (!status.contains("exit_code") || status["exit_code"] != 0 ||
                           status.value("capture_truncated", false) || status.contains("error")) {
                    reject("Shell command failed: " + status.dump());
                }
            }
            shellPending = false;
            if (!failed && readSnapshotPending) {
                readSnapshotPending = false;
                std::smatch match;
                if (std::regex_search(output, match, std::regex("textbox[^\\r\\n]*\\[ref=(e[0-9]+)\\]")))
                    textbox = match[1].str();
                if (std::regex_search(output, match, std::regex("button[^\\r\\n]*\\[ref=(e[0-9]+)\\]")))
                    button = match[1].str();
                if (textbox.empty() || button.empty()) reject("Snapshot contains no textbox/button refs");
            } else if (!failed) {
                const auto cliReply = json::parse(output, nullptr, false);
                if (!cliReply.is_object() || cliReply.value("isError", false))
                    reject("Playwright CLI JSON response failed: " + output);
                if (stage == 6 && output.find("MAICHAT_SKILL_PASS_1") == std::string::npos)
                    reject("Real document.title did not contain the success title: " + output);
                if (stage == 8) closed = true;
            }
        }
        // 失败后仍只关闭本用例的命名会话；不调用 close-all、kill-all 或用户浏览器连接命令。
        if (failed) {
            if (opened && !closed) {
                closed = true;
                execute(sink, command("close"));
            } else {
                sink.onText("Smoke test failed; inspect owned evidence.");
            }
            return {};
        }
        switch (stage++) {
            case 0:
                opened = true;
                execute(sink, command("open " + quote(page.url()) + " --browser=" + settings.browser + " " +
                                      quote("--config=" + config.toUtf8())));
                break;
            case 1: execute(sink, command("snapshot " + quote("--filename=" + snapshot.toUtf8()))); break;
            case 2:
                readSnapshotPending = true;
                execute(sink,
                        "[Console]::OutputEncoding=[System.Text.Encoding]::UTF8; "
                        "Get-Content -Raw -Encoding UTF8 -LiteralPath " +
                            quote(snapshot.toUtf8()));
                break;
            case 3: execute(sink, command("fill " + textbox + " " + quote(task))); break;
            case 4: execute(sink, command("click " + button)); break;
            case 5: execute(sink, command("eval " + quote("document.title"))); break;
            case 6: execute(sink, command("screenshot " + quote("--filename=" + screenshot.toUtf8()))); break;
            case 7: execute(sink, command("close")); break;
            default:
                done = true;
                sink.onText("Completed the real browser form through the installed official skill.");
                break;
        }
        return {};
    };
    std::atomic<int> errors{0};
    std::string lastError;
    // 使用产品宿主的默认技能配置和原生主目录发现，不在测试中手动启用 skillsEnabled。
    AgentController controller(std::move(model), QString());
    CHECK(controller.openError().isEmpty());
    CHECK(controller.setApprovalPolicy(MaiApprovalPolicy::Never));
    auto& agent = controller.agent();
    const auto token = agent.eventBus().subscribe([&](const MaiEvent& event) {
        if (event.type == MaiEventType::SessionError) {
            ++errors;
            lastError = event.detail;
        }
    });
    const auto created =
        controller.createSession(QString::fromUtf8(workspace.toUtf8().c_str()), QString::fromUtf8(name.c_str()));
    CHECK(!created.isEmpty());
    bool acknowledged = false;
    int acknowledgedTurns = 0;
    if (!created.isEmpty()) {
        QEventLoop eventLoop;
        QTimer timeout;
        timeout.setSingleShot(true);
        QObject::connect(&timeout, &QTimer::timeout, &eventLoop, &QEventLoop::quit);
        QObject::connect(&controller, &AgentController::turnFinished, &eventLoop, [&](const QString& finishedSession) {
            if (finishedSession != created) return;
            acknowledged = true;
            eventLoop.quit();
        });
        const std::string text =
            (explicitSelection ? "Use $playwright-cli to " : "Use the available browser automation skill to ") +
            std::string("open the owned local test form, enter the task, click Add and capture a screenshot.");
        const auto sendAndWait = [&](const std::string& input) {
            acknowledged = false;
            if (!controller.sendPrompt(created, QString::fromUtf8(input.c_str()))) {
                CHECK(false);
                return false;
            }
            timeout.start(fromUrl ? 180000 : 100000);
            eventLoop.exec();
            timeout.stop();
            if (!acknowledged) {
                CHECK(false);
                controller.interrupt(created);
            } else {
                ++acknowledgedTurns;
            }
            agent.waitIdle();
            return acknowledged;
        };
        if (fromUrl) {
            if (sendAndWait("Install https://github.com/microsoft/playwright skill") && installationComplete && !failed)
                sendAndWait(text);
        } else {
            sendAndWait(text);
        }
    }
    agent.eventBus().unsubscribe(token);
    CHECK(errors.load() == 0);
    if (!lastError.empty()) std::printf("Session error: %s\n", lastError.c_str());
    CHECK(done && closed && !failed && acknowledged);
    CHECK(acknowledgedTurns == (fromUrl ? 2 : 1));
    std::string png;
    CHECK(!MaiFileSystem::readFile(screenshot, png));
    CHECK(png.size() > 100 && png.compare(0, 8, "\x89PNG\r\n\x1a\n", 8) == 0);
    const auto records = page.tasks();
    int matching = 0;
    for (const auto& record : records)
        if (record == task) ++matching;
    CHECK(matching == 1);
    json evidence{{"scenario", name},
                  {"scripted_model", true},
                  {"requests", requests},
                  {"shell_polls", polls},
                  {"done", done},
                  {"closed", closed},
                  {"desktop_turn_finished", acknowledged},
                  {"acknowledged_turns", acknowledgedTurns},
                  {"session_error", lastError},
                  {"server_matching_posts", matching},
                  {"screenshot_bytes", png.size()},
                  {"skill_id", metadata.id},
                  {"installation_complete", installationComplete},
                  {"install_receipt", installReceipt},
                  {"base_instructions", header},
                  {"transcript", transcript}};
    CHECK(!MaiFileSystem::writeFile(child(root, "evidence.json"), evidence.dump(2)));
    std::printf("%s: requests=%d polls=%d posts=%d screenshot=%zu failures=%d\n", name.c_str(), requests, polls,
                matching, png.size(), failures.load() - previousFailures);
    return failures.load() == previousFailures;
}

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    MaiPlaywrightSmokeOptions settings;
    for (int index = 1; index + 1 < argc; index += 2) {
        const std::string option = argv[index];
        const std::string value = argv[index + 1];
        if (option == "--skill-source")
            settings.skillSource = MaiFilePath::fromUtf8(value);
        else if (option == "--cli")
            settings.cli = MaiFilePath::fromUtf8(value);
        else if (option == "--browser")
            settings.browser = value;
        else if (option == "--evidence")
            settings.evidence = MaiFilePath::fromUtf8(value);
        else if (option == "--scenario")
            settings.scenario = value;
        else
            return 2;
    }
    if ((argc != 9 && argc != 11) || !settings.skillSource.isAbsolute() || !settings.cli.isAbsolute() ||
        !settings.evidence.isAbsolute() || (settings.scenario != "preinstalled" && settings.scenario != "url") ||
        (settings.browser != "chrome" && settings.browser != "msedge" && settings.browser != "chromium")) {
        std::printf(
            "usage: MaiPlaywrightSkillSmoke --skill-source <absolute-folder> "
            "--cli <absolute-launcher> --browser <chrome|msedge|chromium> --evidence <new-owned-folder> "
            "[--scenario <preinstalled|url>]\n");
        return 2;
    }
    // 进程启动前由调用者设置隔离主目录，宿主构造和 Qt 配置都只能看到本次运行的目录。
    const auto expectedHome = MaiFileSystem::resolve(child(settings.evidence, "home"));
    const auto actualHome = MaiSkillManager::defaultHomeDirectory();
    if (!actualHome || MaiFileSystem::resolve(actualHome.value()) != expectedHome ||
        MaiFileSystem::exists(child(expectedHome, ".agents/skills/playwright-cli"))) {
        std::printf("USERPROFILE must be the owned evidence/home with no preexisting skill.\n");
        return 2;
    }
    QApplication application(argc, argv);
    settings.skillSource = MaiFileSystem::resolve(settings.skillSource);
    std::string officialContents;
    const auto read = MaiFileSystem::readFile(child(settings.skillSource, "SKILL.md"), officialContents);
    if (read) {
        std::printf("Cannot read official skill: %s\n", read.message().c_str());
        return 2;
    }
    MaiPlaywrightSmokePage page;
    CHECK(page.isValid());
    if (page.isValid()) {
        if (settings.scenario == "url") {
            scenario(settings, page, officialContents, true, true);
        } else {
            scenario(settings, page, officialContents, false);
            scenario(settings, page, officialContents, true);
        }
    }
    std::printf("MaiPlaywrightSkillSmoke: %d failures (scripted model, real tools and browser)\n", failures.load());
    return failures.load() == 0 ? 0 : 1;
}
