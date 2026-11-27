// 文件作用：以真实 SQLite 检查元数据捕获、事务边界和安全重放。
#include "MaiSqliteProjectionMutation.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <new>
#include <string>
#include <vector>

#include <sqlite3.h>

// 只注入 C++ 行复制分配失败；SQLite 自己的 malloc 不受影响。
static bool failNextAllocation = false;
void* operator new(std::size_t size) {
    if (failNextAllocation) {
        failNextAllocation = false;
        throw std::bad_alloc();
    }
    if (auto* memory = std::malloc(size ? size : 1)) return memory;
    throw std::bad_alloc();
}
void operator delete(void* memory) noexcept {
    std::free(memory);
}
void operator delete(void* memory, std::size_t) noexcept {
    std::free(memory);
}

namespace {
int failures = 0;
#define CHECK(condition)                                                     \
    do {                                                                     \
        if (!(condition)) {                                                  \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
            ++failures;                                                      \
        }                                                                    \
    } while (false)

struct Database {
    sqlite3* connection = nullptr;
    Database() {
        CHECK(sqlite3_open(":memory:", &connection) == SQLITE_OK);
    }
    ~Database() {
        CHECK(sqlite3_close(connection) == SQLITE_OK);
    }
};

bool execute(sqlite3* connection, const char* sql) {
    char* error = nullptr;
    const auto result = sqlite3_exec(connection, sql, nullptr, nullptr, &error);
    if (result != SQLITE_OK) std::printf("SQL %d: %s\n", result, error ? error : "");
    sqlite3_free(error);
    return result == SQLITE_OK;
}

std::string scalar(sqlite3* connection, const char* sql) {
    sqlite3_stmt* statement = nullptr;
    CHECK(sqlite3_prepare_v2(connection, sql, -1, &statement, nullptr) == SQLITE_OK);
    std::string result;
    if (statement && sqlite3_step(statement) == SQLITE_ROW) {
        const auto* value = sqlite3_column_text(statement, 0);
        if (value)
            result.assign(reinterpret_cast<const char*>(value), sqlite3_column_bytes(statement, 0));
    }
    sqlite3_finalize(statement);
    return result;
}

const char* schema = R"SQL(
PRAGMA foreign_keys=ON;
CREATE TABLE parent(a TEXT NOT NULL,b INTEGER NOT NULL, note, payload,
    virtual_body TEXT GENERATED ALWAYS AS (printf('body:%s',a)) VIRTUAL,
    stored_body TEXT GENERATED ALWAYS AS (printf('stored:%s',a)) STORED,
    PRIMARY KEY(a,b)) WITHOUT ROWID;
CREATE TABLE child(id INTEGER PRIMARY KEY, a TEXT,b INTEGER,
    FOREIGN KEY(a,b) REFERENCES parent(a,b) ON DELETE CASCADE ON UPDATE CASCADE);
CREATE TABLE audit(id INTEGER PRIMARY KEY, note);
CREATE TRIGGER audit_parent AFTER INSERT ON parent BEGIN
    INSERT INTO audit(note) VALUES(new.a);
END;
)SQL";

MaiSqliteProjectionMutationOptions options() {
    return {{"parent", "child", "audit"}};
}

void captureTypedGeneratedColumns() {
    Database source;
    CHECK(execute(source.connection, schema));
    MaiSqliteProjectionMutationRecorder recorder(source.connection, options());
    CHECK(execute(source.connection, "BEGIN"));
    CHECK(recorder.beginCapture());
    CHECK(execute(source.connection,
                  "INSERT INTO parent(a,b,note,payload) VALUES('k',9223372036854775806,"
                  "CAST(x'ff0061' AS TEXT),x'00ff6100')"));
    CHECK(recorder.endCapture());
    CHECK(!recorder.hasError());
    const auto changes = recorder.capturedChanges();
    CHECK(changes.size() == 2);
    if (changes.empty()) return;
    const auto& change = changes.front();
    CHECK(change.table == "parent");
    CHECK(change.columns == std::vector<std::string>({"a", "b", "note", "payload"}));
    CHECK(change.primaryKeyColumns == std::vector<std::string>({"a", "b"}));
    CHECK(change.oldValues.empty());
    CHECK(change.newValues.size() == 4);
    if (change.newValues.size() != 4) return;
    CHECK(change.newValues[1].type == MaiSqliteProjectionValueType::Integer);
    CHECK(change.newValues[1].integer == INT64_C(9223372036854775806));
    CHECK(change.newValues[2].type == MaiSqliteProjectionValueType::Text);
    CHECK(change.newValues[2].bytes == std::string("\xff\0a", 3));
    CHECK(change.newValues[3].type == MaiSqliteProjectionValueType::Blob);
    CHECK(change.newValues[3].bytes == std::string("\0\xff"
                                                   "a\0",
                                                   4));
    CHECK(changes.back().depth == 1);

    std::string encoded;
    CHECK(maiEncodeSqliteProjectionMutations(changes, encoded));
    CHECK(encoded.find("body:") == std::string::npos);
    std::vector<MaiSqliteProjectionMutation> decoded;
    CHECK(maiDecodeSqliteProjectionMutations(encoded, decoded));
    CHECK(decoded.size() == changes.size());
    if (!decoded.empty()) CHECK(decoded.front().newValues[2].bytes == change.newValues[2].bytes);

    Database target;
    CHECK(execute(target.connection, schema));
    std::string error;
    CHECK(!maiApplySqliteProjectionMutations(target.connection, decoded, options(), &error));
    CHECK(execute(target.connection, "BEGIN"));
    CHECK(maiApplySqliteProjectionMutations(target.connection, decoded, options(), &error));
    CHECK(scalar(target.connection, "SELECT hex(note)||':'||hex(payload) FROM parent") ==
          "FF0061:00FF6100");
    CHECK(scalar(target.connection, "SELECT count(*) FROM audit") == "1");
    CHECK(execute(target.connection, "ROLLBACK"));
    CHECK(scalar(target.connection, "SELECT count(*) FROM parent") == "0");
    CHECK(execute(source.connection, "ROLLBACK"));
}

void updateDeleteCascadeAndRollback() {
    Database source;
    Database target;
    CHECK(execute(source.connection, schema));
    CHECK(execute(target.connection, schema));
    const char* initial =
        "INSERT INTO parent(a,b,note) VALUES('old',1,'before');"
        "INSERT INTO child VALUES(5,'old',1);";
    CHECK(execute(source.connection, initial));
    CHECK(execute(target.connection, initial));
    MaiSqliteProjectionMutationRecorder recorder(source.connection, options());
    CHECK(execute(source.connection, "BEGIN"));
    CHECK(recorder.beginCapture());
    CHECK(execute(source.connection, "UPDATE parent SET a='new',b=2,note=3.125 WHERE a='old'"));
    CHECK(recorder.endCapture());
    auto changes = recorder.capturedChanges();
    CHECK(changes.size() == 2);
    if (changes.size() != 2) return;
    CHECK(changes[0].type == MaiSqliteProjectionMutationType::Update);
    CHECK(changes[0].oldValues[0].bytes == "old");
    CHECK(changes[0].newValues[0].bytes == "new");
    CHECK(changes[0].newValues[2].type == MaiSqliteProjectionValueType::Real);
    CHECK(changes[1].table == "child");
    CHECK(changes[1].depth == 1);
    CHECK(execute(target.connection, "BEGIN"));
    std::string error;
    CHECK(maiApplySqliteProjectionMutations(target.connection, changes, options(), &error));
    CHECK(scalar(target.connection, "SELECT a||':'||b FROM child") == "new:2");
    CHECK(execute(source.connection, "COMMIT"));
    CHECK(execute(target.connection, "COMMIT"));

    CHECK(execute(source.connection, "BEGIN"));
    CHECK(recorder.beginCapture());
    CHECK(execute(source.connection, "DELETE FROM parent WHERE a='new'"));
    CHECK(recorder.endCapture());
    changes = recorder.capturedChanges();
    CHECK(changes.size() == 2);
    CHECK(execute(target.connection, "BEGIN"));
    CHECK(maiApplySqliteProjectionMutations(target.connection, changes, options(), &error));
    CHECK(scalar(target.connection, "SELECT count(*) FROM child") == "0");
    CHECK(execute(source.connection, "ROLLBACK"));
    CHECK(execute(target.connection, "ROLLBACK"));
    CHECK(scalar(target.connection, "SELECT a FROM parent") == "new");
    // 回滚后仍由调用方丢弃上一批，下一次 begin 必须清空结果。
    CHECK(recorder.beginCapture());
    CHECK(recorder.capturedChanges().empty());
    CHECK(recorder.endCapture());
}

void upsertDoesNotReplaceCascadeChildren() {
    Database source;
    Database target;
    CHECK(execute(source.connection, schema));
    CHECK(execute(target.connection, schema));
    MaiSqliteProjectionMutationRecorder recorder(source.connection, options());
    CHECK(recorder.beginCapture());
    CHECK(execute(source.connection, "INSERT INTO parent(a,b,note) VALUES('same',1,'new')"));
    CHECK(recorder.endCapture());
    CHECK(execute(target.connection,
                  "INSERT INTO parent(a,b,note) VALUES('same',1,'old');"
                  "INSERT INTO child VALUES(2,'same',1); BEGIN"));
    std::string error;
    CHECK(maiApplySqliteProjectionMutations(target.connection, recorder.capturedChanges(),
                                            options(), &error));
    CHECK(scalar(target.connection, "SELECT count(*) FROM child") == "1");
    CHECK(scalar(target.connection, "SELECT note FROM parent") == "new");
    CHECK(scalar(target.connection, "SELECT count(*) FROM audit") == "1");
    CHECK(execute(target.connection, "ROLLBACK"));
}

void typeFidelityAndCodecFailures() {
    Database source;
    Database target;
    const char* sql = "CREATE TABLE typed(id INTEGER PRIMARY KEY,n,t,b,r,real_affinity REAL);";
    CHECK(execute(source.connection, sql));
    CHECK(execute(target.connection, sql));
    const MaiSqliteProjectionMutationOptions typedOptions{{"typed"}};
    MaiSqliteProjectionMutationRecorder recorder(source.connection, typedOptions);
    CHECK(recorder.beginCapture());
    CHECK(execute(source.connection,
                  "INSERT INTO typed VALUES(1,NULL,'',x'',-0.0,1.0);"
                  "INSERT INTO typed VALUES(2,-9223372036854775808,123,"
                  "CAST(x'fe00' AS TEXT),1e999,2.25)"));
    CHECK(recorder.endCapture());
    const auto& changes = recorder.capturedChanges();
    CHECK(changes.size() == 2);
    if (changes.size() != 2) return;
    CHECK(changes[0].newValues[1].type == MaiSqliteProjectionValueType::Null);
    CHECK(changes[0].newValues[2].type == MaiSqliteProjectionValueType::Text);
    CHECK(changes[0].newValues[3].type == MaiSqliteProjectionValueType::Blob);
    CHECK(changes[0].newValues[4].type == MaiSqliteProjectionValueType::Real);
    CHECK(changes[1].newValues[1].integer == std::numeric_limits<std::int64_t>::min());
    std::string encoded;
    CHECK(maiEncodeSqliteProjectionMutations(changes, encoded));
    std::vector<MaiSqliteProjectionMutation> decoded;
    CHECK(maiDecodeSqliteProjectionMutations(encoded, decoded));
    CHECK(decoded.size() == 2);
    if (decoded.size() == 2) {
        CHECK(std::memcmp(&decoded[0].newValues[4].real, &changes[0].newValues[4].real,
                          sizeof(double)) == 0);
        CHECK(decoded[1].newValues[4].real == std::numeric_limits<double>::infinity());
    }
    const auto before = decoded.size();
    CHECK(!maiDecodeSqliteProjectionMutations("{\"version\":2,\"changes\":[]}", decoded));
    CHECK(!maiDecodeSqliteProjectionMutations(
        "{\"version\":1,\"changes\":[[0,\"AB==\",[],[],[],[],0]]}", decoded));
    CHECK(!maiDecodeSqliteProjectionMutations(
        "{\"version\":1,\"changes\":[[0,\"AA=A\",[],[],[],[],0]]}", decoded));
    CHECK(!maiDecodeSqliteProjectionMutations(
        "{\"version\":1,\"changes\":[[0,\"\",[],[],[[1,18446744073709551615]],[],0]]}", decoded));
    CHECK(decoded.size() == before);
    CHECK(execute(target.connection, "BEGIN"));
    std::string error;
    const bool applied =
        maiApplySqliteProjectionMutations(target.connection, decoded, typedOptions, &error);
    if (!applied) std::printf("typed replay error: %s\n", error.c_str());
    CHECK(applied);
    CHECK(scalar(target.connection,
                 "SELECT typeof(n)||':'||typeof(t)||':'||typeof(b)||':'||typeof(r) FROM typed "
                 "WHERE id=1") == "null:text:blob:real");
    CHECK(execute(target.connection, "ROLLBACK"));
}

void injectionSchemaAndFailureGuards() {
    Database source;
    Database target;
    CHECK(execute(source.connection, schema));
    CHECK(execute(target.connection, schema));
    MaiSqliteProjectionMutationRecorder recorder(source.connection, options());
    CHECK(recorder.beginCapture());
    CHECK(execute(source.connection, "INSERT INTO parent(a,b,note) VALUES('safe',1,'value')"));
    CHECK(recorder.endCapture());
    auto changes = recorder.capturedChanges();
    CHECK(execute(target.connection, "BEGIN"));
    auto invalid = changes;
    invalid.front().table = "parent\";DROP TABLE child;--";
    CHECK(!maiApplySqliteProjectionMutations(target.connection, invalid, options()));
    invalid = changes;
    invalid.front().columns[0] = "a\"=?;DROP TABLE child;--";
    CHECK(!maiApplySqliteProjectionMutations(target.connection, invalid, options()));
    invalid = changes;
    invalid.front().primaryKeyColumns = {"b", "a"};
    CHECK(!maiApplySqliteProjectionMutations(target.connection, invalid, options()));
    invalid = changes;
    invalid.front().newValues.pop_back();
    CHECK(!maiApplySqliteProjectionMutations(target.connection, invalid, options()));
    CHECK(scalar(target.connection, "SELECT count(*) FROM parent") == "0");
    CHECK(execute(target.connection, "ROLLBACK"));
    // 触发器缺失不能靠跳过派生行得到成功回执。
    CHECK(execute(target.connection, "DROP TRIGGER audit_parent; BEGIN"));
    CHECK(!maiApplySqliteProjectionMutations(target.connection, changes, options()));
    CHECK(execute(target.connection, "ROLLBACK"));
    CHECK(execute(source.connection, "CREATE TABLE other(id INTEGER PRIMARY KEY)"));
    CHECK(recorder.beginCapture());
    char* error = nullptr;
    sqlite3_exec(source.connection, "BEGIN; INSERT INTO other VALUES(1)", nullptr, nullptr, &error);
    sqlite3_free(error);
    CHECK(!recorder.endCapture());
    CHECK(recorder.hasError());
    if (!sqlite3_get_autocommit(source.connection)) CHECK(execute(source.connection, "ROLLBACK"));
    CHECK(scalar(source.connection, "SELECT count(*) FROM other") == "0");
    {
        Database empty;
        MaiSqliteProjectionMutationRecorder missing(empty.connection, {{"missing"}});
        CHECK(!missing.beginCapture());
    }
}

void schemaChangesAndUnsupportedKeysFailClosed() {
    Database source;
    CHECK(execute(source.connection,
                  "CREATE TABLE t(id TEXT PRIMARY KEY,note); CREATE TABLE no_key(note)"));
    {
        MaiSqliteProjectionMutationRecorder noKey(source.connection, {{"no_key"}});
        CHECK(!noKey.beginCapture());
        CHECK(!noKey.error().empty());
    }
    {
        MaiSqliteProjectionMutationRecorder recorder(source.connection, {{"t"}});
        CHECK(execute(source.connection, "BEGIN"));
        CHECK(recorder.beginCapture());
        CHECK(execute(source.connection,
                      "ALTER TABLE t RENAME COLUMN note TO changed;"
                      "INSERT INTO t VALUES('id','value')"));
        CHECK(!recorder.endCapture());
        CHECK(execute(source.connection, "ROLLBACK"));
        CHECK(recorder.beginCapture());
        sqlite3_exec(source.connection, "INSERT INTO t VALUES(NULL,'invalid')", nullptr, nullptr,
                     nullptr);
        CHECK(!recorder.endCapture());
        CHECK(recorder.hasError());
    }
    // 析构后不留下指向已销毁 C++ 对象的 C 回调上下文。
    CHECK(sqlite3_preupdate_hook(source.connection, nullptr, nullptr) == nullptr);
}

void finalRowsFollowSqliteKeyCollation() {
    Database source;
    Database target;
    const char* sql = "CREATE TABLE t(id TEXT PRIMARY KEY COLLATE NOCASE,note);";
    CHECK(execute(source.connection, sql));
    CHECK(execute(target.connection, sql));
    const MaiSqliteProjectionMutationOptions collationOptions{{"t"}};
    MaiSqliteProjectionMutationRecorder recorder(source.connection, collationOptions);
    CHECK(recorder.beginCapture());
    CHECK(execute(source.connection,
                  "INSERT INTO t VALUES('A','first');"
                  "UPDATE t SET id='a',note='second' WHERE id='A'"));
    CHECK(recorder.endCapture());
    CHECK(execute(target.connection, "BEGIN"));
    CHECK(maiApplySqliteProjectionMutations(target.connection, recorder.capturedChanges(),
                                            collationOptions));
    CHECK(scalar(target.connection, "SELECT id||':'||note FROM t") == "a:second");
    CHECK(execute(target.connection, "ROLLBACK"));
}

void bulkRowsAndNumericKeys() {
    Database source;
    Database target;
    const char* sql = "CREATE TABLE t(id PRIMARY KEY,note);";
    CHECK(execute(source.connection, sql));
    CHECK(execute(target.connection, sql));
    const MaiSqliteProjectionMutationOptions bulkOptions{{"t"}};
    MaiSqliteProjectionMutationRecorder recorder(source.connection, bulkOptions);
    CHECK(recorder.beginCapture());
    CHECK(execute(source.connection,
                  "WITH RECURSIVE n(x) AS (VALUES(1) UNION ALL SELECT x+1 FROM n WHERE x<1000) "
                  "INSERT INTO t SELECT x,'value' FROM n;"
                  "UPDATE t SET id=1.0,note='numeric' WHERE id=1;"
                  "DELETE FROM t WHERE id=1000"));
    CHECK(recorder.endCapture());
    CHECK(recorder.capturedChanges().size() == 1002);
    CHECK(execute(target.connection, "BEGIN"));
    CHECK(maiApplySqliteProjectionMutations(target.connection, recorder.capturedChanges(),
                                            bulkOptions));
    CHECK(scalar(target.connection, "SELECT count(*) FROM t") == "999");
    CHECK(scalar(target.connection, "SELECT typeof(id)||':'||note FROM t WHERE id=1") ==
          "real:numeric");
    CHECK(execute(target.connection, "ROLLBACK"));
}

void generatedColumnsBetweenOrdinaryColumns() {
    Database source;
    Database target;
    const char* sql =
        "CREATE TABLE t(id TEXT,note,virtual_body AS (note||'body') VIRTUAL,"
        "tail BLOB,stored_body AS (note||'stored') STORED,"
        "PRIMARY KEY(id COLLATE NOCASE));";
    CHECK(execute(source.connection, sql));
    CHECK(execute(target.connection, sql));
    const MaiSqliteProjectionMutationOptions interleaved{{"t"}};
    MaiSqliteProjectionMutationRecorder recorder(source.connection, interleaved);
    CHECK(recorder.beginCapture());
    CHECK(execute(source.connection,
                  "INSERT INTO t(id,note,tail) VALUES('A','first',x'00fe');"
                  "UPDATE t SET id='a',note='second' WHERE id='A';"
                  "UPDATE t SET note='third',tail=x'fe00' WHERE id='a'"));
    CHECK(recorder.endCapture());
    const auto& changes = recorder.capturedChanges();
    CHECK(changes.size() == 3);
    if (changes.size() != 3) return;
    CHECK(changes[0].columns == std::vector<std::string>({"id", "note", "tail"}));
    CHECK(changes[0].newValues[2].bytes == std::string("\0\xfe", 2));
    CHECK(execute(target.connection, "BEGIN"));
    std::string error;
    const bool applied =
        maiApplySqliteProjectionMutations(target.connection, changes, interleaved, &error);
    if (!applied) std::printf("interleaved replay error: %s\n", error.c_str());
    CHECK(applied);
    CHECK(scalar(target.connection, "SELECT id||':'||hex(tail) FROM t") == "a:FE00");
    CHECK(execute(target.connection, "ROLLBACK"));
}

void primaryKeyIndexOverridesColumnCollation() {
    Database source;
    Database target;
    const char* sql = "CREATE TABLE t(id TEXT COLLATE NOCASE,note,PRIMARY KEY(id COLLATE BINARY));";
    CHECK(execute(source.connection, sql));
    CHECK(execute(target.connection, sql));
    const MaiSqliteProjectionMutationOptions pkOptions{{"t"}};
    MaiSqliteProjectionMutationRecorder recorder(source.connection, pkOptions);
    CHECK(recorder.beginCapture());
    CHECK(execute(source.connection, "INSERT INTO t VALUES('A','upper'),('a','lower')"));
    CHECK(recorder.endCapture());
    CHECK(execute(target.connection, "BEGIN"));
    CHECK(maiApplySqliteProjectionMutations(target.connection, recorder.capturedChanges(),
                                            pkOptions));
    CHECK(scalar(target.connection, "SELECT count(*) FROM t") == "2");
    CHECK(execute(target.connection, "ROLLBACK"));
}

void allocationFailureCannotEscapeHook() {
    Database source;
    CHECK(execute(source.connection, "CREATE TABLE t(id INTEGER PRIMARY KEY,note); BEGIN"));
    MaiSqliteProjectionMutationRecorder recorder(source.connection, {{"t"}});
    CHECK(recorder.beginCapture());
    failNextAllocation = true;
    const int result = sqlite3_exec(source.connection, "INSERT INTO t VALUES(1,'value')", nullptr,
                                    nullptr, nullptr);
    failNextAllocation = false;
    // SQLite 官方 interrupt 契约允许接近完成的语句继续返回成功。
    CHECK(result == SQLITE_INTERRUPT || result == SQLITE_OK);
    CHECK(!recorder.endCapture());
    CHECK(recorder.hasError());
    CHECK(recorder.capturedChanges().empty());
    if (!sqlite3_get_autocommit(source.connection)) CHECK(execute(source.connection, "ROLLBACK"));
    CHECK(scalar(source.connection, "SELECT count(*) FROM t") == "0");
    CHECK(recorder.beginCapture());
    CHECK(execute(source.connection, "INSERT INTO t VALUES(2,'next')"));
    CHECK(recorder.endCapture());
}

void applyGuardAndQuotedIdentifiers() {
    Database source;
    Database target;
    const char* sql =
        "CREATE TABLE \"t\"\";literal\"(\"id\"\"\" TEXT PRIMARY KEY,\"value;--\" BLOB)";
    CHECK(execute(source.connection, sql));
    CHECK(execute(target.connection, sql));
    const MaiSqliteProjectionMutationOptions quoted{{"t\";literal"}};
    MaiSqliteProjectionMutationRecorder recorder(source.connection, quoted);
    CHECK(recorder.beginCapture());
    CHECK(execute(source.connection, "INSERT INTO \"t\"\";literal\" VALUES('x',x'00fe')"));
    CHECK(recorder.endCapture());
    MaiSqliteProjectionMutationRecorder targetRecorder(target.connection, quoted);
    CHECK(execute(target.connection, "BEGIN"));
    CHECK(targetRecorder.beginCapture());
    CHECK(maiApplySqliteProjectionMutations(target.connection, recorder.capturedChanges(), quoted));
    CHECK(targetRecorder.endCapture());
    CHECK(targetRecorder.capturedChanges().empty());
    CHECK(scalar(target.connection, "SELECT hex(\"value;--\") FROM \"t\"\";literal\"") == "00FE");
    CHECK(execute(target.connection, "ROLLBACK"));
}
}  // namespace

int main() {
    captureTypedGeneratedColumns();
    updateDeleteCascadeAndRollback();
    upsertDoesNotReplaceCascadeChildren();
    typeFidelityAndCodecFailures();
    injectionSchemaAndFailureGuards();
    schemaChangesAndUnsupportedKeysFailClosed();
    finalRowsFollowSqliteKeyCollation();
    bulkRowsAndNumericKeys();
    generatedColumnsBetweenOrdinaryColumns();
    primaryKeyIndexOverridesColumnCollation();
    allocationFailureCannotEscapeHook();
    applyGuardAndQuotedIdentifiers();
    std::printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
