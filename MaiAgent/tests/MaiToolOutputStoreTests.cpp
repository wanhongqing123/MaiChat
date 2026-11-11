// 文件作用：用同一契约验证内存/SQLite 工具原始输出、归属和保留边界。
#include <cstdio>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include <sqlite3.h>

#include "MaiFilePath.h"
#include "MaiFileSystem.h"
#include "MaiIdGenerator.h"
#include "MaiMemoryStore.h"
#include "MaiSessionStore.h"
#include "MaiSqliteStore.h"

static int failures = 0;
#define CHECK(condition)                                                    \
    do {                                                                    \
        if (!(condition)) {                                                 \
            std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #condition); \
            ++failures;                                                     \
        }                                                                   \
    } while (0)

namespace {

struct TempDatabase {
    MaiFilePath root = MaiFileSystem::temporaryDirectory().append(
        MaiFilePath::fromUtf8("mai-tool-output-" + MaiIdGenerator::newSessionId()));
    ~TempDatabase() {
        MaiFileSystem::removeRecursively(root);
    }
    std::string path() const {
        return root.append(MaiFilePath::fromUtf8("output.db")).toUtf8();
    }
};

MaiToolOutputRecord seedOutput(MaiSessionStore& store, const std::string& sessionId,
                               const std::string& id, const std::string& text,
                               MaiMillis created = 1) {
    MaiSession session;
    session.id = sessionId;
    store.putSession(session);
    MaiMessage message;
    message.id = "msg_" + id;
    message.role = MaiRole::Assistant;
    MaiMessagePart part;
    part.id = "prt_" + id;
    MaiToolPart tool;
    tool.tool = "shell";
    tool.callId = "call_" + id;
    tool.state = MaiToolState::Pending;
    part.body = tool;
    message.parts.push_back(part);
    store.putMessage(sessionId, message);
    MaiToolOutputRecord output;
    output.id = id;
    output.sessionId = sessionId;
    output.messageId = message.id;
    output.partId = part.id;
    output.callId = tool.callId;
    output.text = text;
    output.created = created;
    return output;
}

void contractOwnershipAndPaging(MaiSessionStore& store) {
    const std::string sessionId = "ses_output";
    const std::string otherId = "ses_other";
    // NUL 和非法字节必须原样保留，不能走 SQLite TEXT 的净化入口。
    const std::string bytes("first\0last\xFF!", 12);
    auto output = seedOutput(store, sessionId, "out_bytes", bytes);
    CHECK(!store.putToolOutput(output));
    auto page = store.readToolOutput(sessionId, {output.id, 0, 6});
    CHECK(page.isOk());
    if (page) {
        CHECK(page.value().text == bytes.substr(0, 6));
        CHECK(page.value().offsetBytes == 0);
        CHECK(page.value().sourceBytesRead == 6);
        CHECK(page.value().nextOffsetBytes == 6);
        CHECK(page.value().totalBytes == bytes.size());
        CHECK(page.value().outputId == output.id);
        CHECK(page.value().messageId == output.messageId);
        CHECK(page.value().partId == output.partId);
        CHECK(page.value().callId == output.callId);
        CHECK(page.value().textIsValidUtf8);
    }
    page = store.readToolOutput(sessionId, {output.id, 6, 6});
    CHECK(page.isOk());
    if (page) {
        CHECK(page.value().text == bytes.substr(6));
        CHECK(!page.value().textIsValidUtf8);
        CHECK(page.value().nextOffsetBytes == 0);
    }
    page = store.readToolOutput(sessionId, {output.id, bytes.size(), 1});
    CHECK(page.isOk());
    if (page) CHECK(page.value().text.empty() && page.value().nextOffsetBytes == 0);
    CHECK(store.readToolOutput(sessionId, {output.id, bytes.size() + 1, 1}).error().code() ==
          MaiErrorCode::InvalidInput);
    CHECK(store.readToolOutput(sessionId, {output.id, 0, 0}).error().code() ==
          MaiErrorCode::InvalidInput);
    CHECK(store.readToolOutput(sessionId, {output.id, 0, 4097}).error().code() ==
          MaiErrorCode::InvalidInput);
    CHECK(store.readToolOutput(sessionId, {output.id, std::numeric_limits<std::size_t>::max(), 1})
              .error()
              .code() == MaiErrorCode::InvalidInput);
    CHECK(store.readToolOutput(sessionId, {std::string(129, 'x')}).error().code() ==
          MaiErrorCode::InvalidInput);
    CHECK(store.readToolOutput(otherId, {output.id}).error().code() == MaiErrorCode::NotFound);

    // 重写句柄必须失败，包括完全相同的数据；原内容保持不变。
    CHECK(store.putToolOutput(output).code() == MaiErrorCode::InvalidInput);
    output.text = "replaced";
    CHECK(store.putToolOutput(output).code() == MaiErrorCode::InvalidInput);
    page = store.readToolOutput(sessionId, {output.id});
    CHECK(page && page.value().text == bytes);

    auto invalid = seedOutput(store, otherId, "out_invalid", "foreign");
    invalid.sessionId = sessionId;
    CHECK(store.putToolOutput(invalid).code() == MaiErrorCode::InvalidInput);
    invalid.sessionId = otherId;
    invalid.messageId = "msg_missing";
    CHECK(store.putToolOutput(invalid).code() == MaiErrorCode::InvalidInput);
    invalid.messageId = "msg_out_invalid";
    invalid.partId = output.partId;
    CHECK(store.putToolOutput(invalid).code() == MaiErrorCode::InvalidInput);
    invalid.partId = "prt_out_invalid";
    invalid.callId = "call_wrong";
    CHECK(store.putToolOutput(invalid).code() == MaiErrorCode::InvalidInput);
    invalid.callId = "call_out_invalid";
    invalid.id = std::string(129, 'x');
    CHECK(store.putToolOutput(invalid).code() == MaiErrorCode::InvalidInput);
    invalid.id.clear();
    CHECK(store.putToolOutput(invalid).code() == MaiErrorCode::InvalidInput);

    // 原片段归属变化后旧句柄失效，不能靠 retained row 绕过当前源片段核验。
    MaiMessage message;
    CHECK(store.getMessage(sessionId, output.messageId, message));
    std::get<MaiToolPart>(message.parts.front().body).callId = "call_changed";
    store.putMessage(sessionId, message);
    CHECK(store.readToolOutput(sessionId, {output.id}).error().code() == MaiErrorCode::NotFound);
    CHECK(store.clearMessages(sessionId));
    CHECK(store.readToolOutput(sessionId, {output.id}).error().code() == MaiErrorCode::NotFound);
    auto afterClear = seedOutput(store, sessionId, output.id, "after clear");
    CHECK(!store.putToolOutput(afterClear));
    CHECK(store.removeSession(sessionId));
    CHECK(store.readToolOutput(sessionId, {output.id}).error().code() == MaiErrorCode::NotFound);
    CHECK(store.removeSession(otherId));

    auto empty = seedOutput(store, "ses_empty", "out_empty", "");
    CHECK(!store.putToolOutput(empty));
    page = store.readToolOutput(empty.sessionId, {empty.id});
    CHECK(page && page.value().text.empty() && page.value().totalBytes == 0 &&
          page.value().sourceBytesRead == 0 && page.value().nextOffsetBytes == 0);
    CHECK(store.removeSession(empty.sessionId));
}

void contractUtf8(MaiSessionStore& store) {
    const std::string text =
        "a"
        "\xE4\xB8\xAD"
        "\xF0\x9F\x98\x80"
        "z";
    auto output = seedOutput(store, "ses_utf8", "out_utf8", text);
    output.truncated = true;
    CHECK(!store.putToolOutput(output));
    auto page = store.readToolOutput(output.sessionId, {output.id, 0, 3});
    CHECK(page && page.value().text == "a" && page.value().nextOffsetBytes == 1);
    page = store.readToolOutput(output.sessionId, {output.id, 1, 3});
    CHECK(page && page.value().text == text.substr(1, 3) && page.value().nextOffsetBytes == 4 &&
          page.value().truncated);
    page = store.readToolOutput(output.sessionId, {output.id, 4, 4});
    CHECK(page && page.value().text == text.substr(4, 4) && page.value().textIsValidUtf8);
    CHECK(store.readToolOutput(output.sessionId, {output.id, 2, 4}).error().code() ==
          MaiErrorCode::InvalidInput);
    CHECK(store.readToolOutput(output.sessionId, {output.id, 4, 3}).error().code() ==
          MaiErrorCode::InvalidInput);
    // 孤立续字节、过长编码和代理项都逐字节推进，绝不伪称合法 UTF-8。
    const std::string bad = "\x80\xC0\xAF\xED\xA0\x80";
    auto invalid = seedOutput(store, "ses_utf8", "out_bad_utf8", bad);
    CHECK(!store.putToolOutput(invalid));
    for (std::size_t offset = 0; offset < bad.size(); ++offset) {
        page = store.readToolOutput(invalid.sessionId, {invalid.id, offset, 1});
        CHECK(page && page.value().text == bad.substr(offset, 1) &&
              page.value().sourceBytesRead == 1 && !page.value().textIsValidUtf8);
    }
    CHECK(store.removeSession(output.sessionId));
    CHECK(store.readToolOutput(output.sessionId, {output.id}).error().code() ==
          MaiErrorCode::NotFound);
}

void contractQuotas(MaiSessionStore& store) {
    // 对上限本身操作，保证测试观察到真正的字节配额，不能只验证内部常量。
    const std::string block(16 * 1024 * 1024, 'q');
    auto oversized = seedOutput(store, "ses_quota", "out_too_large", block + "x");
    CHECK(store.putToolOutput(oversized).code() == MaiErrorCode::InvalidInput);
    oversized.text.clear();
    for (int index = 0; index < 5; ++index) {
        auto output = seedOutput(store, "ses_quota", "out_quota_" + std::to_string(index), block,
                                 index == 0 ? 2 : 1);
        CHECK(!store.putToolOutput(output));
    }
    // created 相同按 id 淘汰；out_quota_0 虽先写入，其 created 较新，仍须保留。
    CHECK(store.readToolOutput("ses_quota", {"out_quota_1", 0, 1}).error().code() ==
          MaiErrorCode::NotFound);
    CHECK(store.readToolOutput("ses_quota", {"out_quota_0", 0, 1}).isOk());
    auto reused = seedOutput(store, "ses_quota", "out_quota_1", "replacement", 9);
    CHECK(store.putToolOutput(reused).code() == MaiErrorCode::InvalidInput);
    CHECK(store.removeSession("ses_quota"));

    for (int index = 0; index < 17; ++index) {
        const auto label = std::to_string(index);
        auto output =
            seedOutput(store, "ses_global_" + label, "out_global_" + label, block, 100 + index);
        CHECK(!store.putToolOutput(output));
    }
    CHECK(store.readToolOutput("ses_global_0", {"out_global_0", 0, 1}).error().code() ==
          MaiErrorCode::NotFound);
    CHECK(store.readToolOutput("ses_global_16", {"out_global_16", 0, 1}).isOk());
    for (int index = 0; index < 17; ++index)
        CHECK(store.removeSession("ses_global_" + std::to_string(index)));
}

void contractExecutionRecovery(MaiSessionStore& store) {
    auto pending = seedOutput(store, "ses_recovery", "out_pending", "pending partial");
    auto running = seedOutput(store, "ses_recovery", "out_running", "running partial");
    auto completed = seedOutput(store, "ses_recovery", "out_completed", "completed original");
    for (const auto& output : {pending, running, completed}) {
        MaiMessage message;
        CHECK(store.getMessage(output.sessionId, output.messageId, message));
        auto& tool = std::get<MaiToolPart>(message.parts.front().body);
        tool.output = output.text;
        tool.state = output.id == pending.id   ? MaiToolState::Pending
                     : output.id == running.id ? MaiToolState::Running
                                               : MaiToolState::Completed;
        MaiToolExecution execution;
        execution.status = MaiToolExecutionStatus::Running;
        execution.effectStatus = MaiToolEffectStatus::Unknown;
        execution.nextAction = MaiToolNextAction::QueryStatus;
        execution.started = true;
        execution.exitCode = 17;
        execution.timedOut = true;
        execution.canceled = true;
        execution.outputTruncated = true;
        execution.outputId = output.id;
        execution.taskId = "accepted_task";
        execution.recoveryTool = "task_status";
        tool.execution = execution;
        store.putMessage(output.sessionId, message);
        CHECK(!store.putToolOutput(output));
        MaiMessage loaded;
        CHECK(store.getMessage(output.sessionId, output.messageId, loaded));
        const auto& saved = std::get<MaiToolPart>(loaded.parts.front().body).execution;
        CHECK(saved.has_value());
        if (saved) {
            CHECK(saved->status == MaiToolExecutionStatus::Running);
            CHECK(saved->nextAction == MaiToolNextAction::QueryStatus);
            CHECK(saved->started && *saved->started);
            CHECK(saved->exitCode && *saved->exitCode == 17);
            CHECK(saved->timedOut && saved->canceled && saved->outputTruncated);
            CHECK(saved->outputId == output.id && saved->taskId == "accepted_task" &&
                  saved->recoveryTool == "task_status");
        }
    }
    store.recoverInterruptedTools("Host interrupted execution.", 44);
    for (const auto& output : {pending, running, completed}) {
        MaiMessage loaded;
        CHECK(store.getMessage(output.sessionId, output.messageId, loaded));
        const auto& tool = std::get<MaiToolPart>(loaded.parts.front().body);
        CHECK(tool.output == output.text);
        CHECK(tool.execution.has_value());
        if (!tool.execution) continue;
        const auto& execution = *tool.execution;
        CHECK(execution.outputId == output.id && execution.taskId == "accepted_task");
        if (output.id == completed.id) {
            CHECK(tool.state == MaiToolState::Completed);
            CHECK(loaded.completed == 0);
            CHECK(execution.status == MaiToolExecutionStatus::Running);
        } else {
            CHECK(tool.state == MaiToolState::Error && tool.error == "Host interrupted execution.");
            CHECK(loaded.completed == 44);
            if (output.id == pending.id) {
                CHECK(execution.status == MaiToolExecutionStatus::NotStarted);
                CHECK(execution.effectStatus == MaiToolEffectStatus::None);
                CHECK(execution.started && !*execution.started);
                CHECK(execution.nextAction == MaiToolNextAction::None);
                CHECK(!execution.exitCode && !execution.timedOut && !execution.canceled);
            } else {
                CHECK(execution.status == MaiToolExecutionStatus::Unknown);
                CHECK(execution.effectStatus == MaiToolEffectStatus::Unknown);
                CHECK(execution.nextAction == MaiToolNextAction::QueryStatus);
                CHECK(execution.recoveryTool == "task_status");
                CHECK(execution.started && *execution.started && !execution.exitCode);
            }
        }
        const auto page = store.readToolOutput(output.sessionId, {output.id});
        CHECK(page && page.value().text == output.text);
    }
    auto unknown = seedOutput(store, "ses_recovery", "out_unknown", "unknown partial");
    MaiMessage message;
    CHECK(store.getMessage(unknown.sessionId, unknown.messageId, message));
    auto& tool = std::get<MaiToolPart>(message.parts.front().body);
    tool.state = MaiToolState::Running;
    tool.output = unknown.text;
    store.putMessage(unknown.sessionId, message);
    store.recoverInterruptedTools("Host interrupted again.", 55);
    CHECK(store.getMessage(unknown.sessionId, unknown.messageId, message));
    const auto& recovered = std::get<MaiToolPart>(message.parts.front().body);
    CHECK(recovered.output == unknown.text && recovered.execution);
    if (recovered.execution) {
        CHECK(recovered.execution->status == MaiToolExecutionStatus::Unknown);
        CHECK(recovered.execution->effectStatus == MaiToolEffectStatus::Unknown);
        CHECK(recovered.execution->nextAction == MaiToolNextAction::InspectState);
        CHECK(!recovered.execution->started);
    }
    CHECK(store.removeSession(pending.sessionId));
}

void testSqliteReopen() {
    TempDatabase temporary;
    auto opened = makeMaiSqliteStore(temporary.path());
    CHECK(opened.isOk());
    if (!opened) return;
    auto store = std::move(opened.value());
    const std::string text = std::string(120 * 1024, 'r') + "FINAL_MARKER";
    auto output = seedOutput(*store, "ses_reopen", "out_reopen", text);
    CHECK(!store->putToolOutput(output));
    MaiMessage message;
    CHECK(store->getMessage(output.sessionId, output.messageId, message));
    auto& tool = std::get<MaiToolPart>(message.parts.front().body);
    tool.execution = MaiToolExecution{};
    tool.execution->status = MaiToolExecutionStatus::Completed;
    tool.execution->started = true;
    tool.execution->exitCode = 0;
    tool.execution->outputId = output.id;
    store->putMessage(output.sessionId, message);
    store.reset();
    opened = makeMaiSqliteStore(temporary.path());
    CHECK(opened.isOk());
    if (!opened) return;
    store = std::move(opened.value());
    auto page = store->readToolOutput(output.sessionId, {output.id, text.size() - 12, 12});
    CHECK(page && page.value().text == "FINAL_MARKER" && page.value().nextOffsetBytes == 0);
    CHECK(store->getMessage(output.sessionId, output.messageId, message));
    const auto& execution = std::get<MaiToolPart>(message.parts.front().body).execution;
    CHECK(execution && execution->status == MaiToolExecutionStatus::Completed &&
          execution->started && *execution->started && execution->exitCode &&
          *execution->exitCode == 0 && execution->outputId == output.id);
    CHECK(store->clearMessages(output.sessionId));
    store.reset();
    opened = makeMaiSqliteStore(temporary.path());
    CHECK(opened.isOk());
    if (!opened) return;
    store = std::move(opened.value());
    CHECK(store->readToolOutput(output.sessionId, {output.id}).error().code() ==
          MaiErrorCode::NotFound);
}

void testSqliteMigrationAndAtomicCleanup() {
    TempDatabase temporary;
    auto opened = makeMaiSqliteStore(temporary.path());
    CHECK(opened.isOk());
    if (!opened) return;
    auto store = std::move(opened.value());
    auto output = seedOutput(*store, "ses_migrate", "out_migrate", "original preview");
    store.reset();
    sqlite3* database = nullptr;
    CHECK(sqlite3_open(temporary.path().c_str(), &database) == SQLITE_OK);
    CHECK(sqlite3_exec(database,
                       "ALTER TABLE parts DROP COLUMN execution;"
                       "DROP TABLE tool_outputs;PRAGMA user_version=10;",
                       nullptr, nullptr, nullptr) == SQLITE_OK);
    sqlite3_close(database);
    opened = makeMaiSqliteStore(temporary.path());
    CHECK(opened.isOk());
    if (!opened) return;
    store = std::move(opened.value());
    MaiMessage message;
    CHECK(store->getMessage(output.sessionId, output.messageId, message));
    CHECK(!std::get<MaiToolPart>(message.parts.front().body).execution);
    CHECK(!store->putToolOutput(output));

    // 清理中途失败必须回滚已删除的 BLOB/墓碑，不能让消息和保留输出只剩一半。
    CHECK(sqlite3_open(temporary.path().c_str(), &database) == SQLITE_OK);
    CHECK(sqlite3_exec(database,
                       "CREATE TRIGGER block_cleanup BEFORE DELETE ON parts BEGIN "
                       "SELECT RAISE(ABORT,'test cleanup failure');END;",
                       nullptr, nullptr, nullptr) == SQLITE_OK);
    CHECK(!store->clearMessages(output.sessionId));
    CHECK(store->readToolOutput(output.sessionId, {output.id}).isOk());
    CHECK(!store->removeSession(output.sessionId));
    CHECK(store->readToolOutput(output.sessionId, {output.id}).isOk());
    CHECK(sqlite3_exec(database, "DROP TRIGGER block_cleanup", nullptr, nullptr, nullptr) ==
          SQLITE_OK);
    sqlite3_close(database);
    CHECK(store->removeSession(output.sessionId));
    CHECK(store->readToolOutput(output.sessionId, {output.id}).error().code() ==
          MaiErrorCode::NotFound);
}

}  // namespace

int main() {
    auto memory = makeMaiMemoryStore();
    std::printf("memory retained output contract\n");
    contractOwnershipAndPaging(*memory);
    contractUtf8(*memory);
    contractQuotas(*memory);
    contractExecutionRecovery(*memory);
    auto sqlite = makeMaiSqliteStore(":memory:");
    CHECK(sqlite.isOk());
    if (sqlite) {
        std::printf("sqlite retained output contract\n");
        contractOwnershipAndPaging(*sqlite.value());
        contractUtf8(*sqlite.value());
        contractQuotas(*sqlite.value());
        contractExecutionRecovery(*sqlite.value());
    }
    testSqliteReopen();
    testSqliteMigrationAndAtomicCleanup();
    std::printf("%d failures\n", failures);
    return failures == 0 ? 0 : 1;
}
