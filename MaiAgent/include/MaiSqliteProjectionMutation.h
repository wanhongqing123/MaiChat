// 文件作用：记录和重放 SQLite 元数据行变更，不承担事务提交或日志持久化。
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

struct sqlite3;

// 保留 SQLite 存储类型；bytes 只用于 Text/Blob，允许 NUL 和非法 UTF-8。
// 不做数值或文本规范化，避免投影恢复改变原始元数据。
enum class MaiSqliteProjectionValueType { Null, Integer, Real, Text, Blob };

struct MaiSqliteProjectionValue {
    MaiSqliteProjectionValueType type = MaiSqliteProjectionValueType::Null;
    std::int64_t integer = 0;
    double real = 0;
    std::string bytes;
};

enum class MaiSqliteProjectionMutationType { Insert, Update, Delete };

// columns 是 table_xinfo 顺序的普通列；生成列永不进入记录。
// primaryKeyColumns 按声明的复合主键顺序排列，不推测 rowid。
// Insert 的 oldValues、Delete 的 newValues 为空；其余值与 columns 一一对应。
// depth 保留 SQLite 的触发器深度，以免重放时重复执行级联派生的行变更。
struct MaiSqliteProjectionMutation {
    MaiSqliteProjectionMutationType type = MaiSqliteProjectionMutationType::Insert;
    std::string table;
    std::vector<std::string> columns;
    std::vector<std::string> primaryKeyColumns;
    std::vector<MaiSqliteProjectionValue> oldValues;
    std::vector<MaiSqliteProjectionValue> newValues;
    int depth = 0;
};

struct MaiSqliteProjectionMutationOptions {
    // 仅 main 中显式允许的普通表；内部表、虚表和没有声明主键的表不受支持。
    // 捕获期间遇到其他表写入会标记失败并请求中断，不能静默丢失元数据。
    std::vector<std::string> allowedTables;
};

// 借用 connection，必须比 recorder 长寿；调用方独占连接及 preupdate hook。
// 调用方必须用同一把锁串行化连接和 recorder 操作，不得另装 hook/session 扩展。
// 一次 beginCapture/endCapture 及其中的 SQL/hook 在同一线程执行；静止的事务间可交接线程。
// 不绑定构造线程；析构前必须确认连接没有并发执行或仍在途的 hook。
// beginCapture 在 SQL 语句执行前缓存 schema；捕获期间禁止 DDL、附加库和 blob_write。
// hook 只复制内存，不执行 SQL、文件操作、日志或回调；异常被转换为失败并请求 interrupt。
// SQLite 在语句接近结束时可能仍返回 SQLITE_OK，因此必须检查 endCapture 并在失败时回滚。
// 调用方必须在语句/事务失败或回滚后丢弃 capturedChanges，检查 endCapture 的结果，
// 只有 SQL 操作和捕获都成功的记录才能写入权威日志；刷日志与提交的顺序由调用方决定。
// 本类不观察或控制 commit/rollback。
// 析构只卸载 hook，不关闭连接。禁止移动和复制，以保持 C 回调上下文地址稳定。
class MaiSqliteProjectionMutationRecorder {
public:
    MaiSqliteProjectionMutationRecorder(sqlite3* connection,
                                        MaiSqliteProjectionMutationOptions options);
    ~MaiSqliteProjectionMutationRecorder();
    MaiSqliteProjectionMutationRecorder(const MaiSqliteProjectionMutationRecorder&) = delete;
    MaiSqliteProjectionMutationRecorder& operator=(const MaiSqliteProjectionMutationRecorder&) =
        delete;

    // 清除上次结果；返回 false 时没有开始捕获。分配失败也通过 error 返回。
    bool beginCapture();
    // 停止捕获；失败时结果不可使用，调用方必须回滚受影响的事务。
    bool endCapture();
    const std::vector<MaiSqliteProjectionMutation>& capturedChanges() const;
    bool hasError() const;
    std::string error() const;

private:
    struct State;
    std::unique_ptr<State> mState;
};

// 在调用方已开启的事务中应用类型化行变更；不提交、不回滚、不写日志。
// 失败时调用方必须回滚整批；error 可空。必须先停止该连接上的 recorder。
// 验证 schema 中的表、列和声明主键，标识符引用后拼接，值全部绑定。
// 仅执行 depth==0 的变更，匹配 schema 的触发器和外键负责重建派生变更；
// 因而恢复连接必须使用与捕获连接相同的触发器和 foreign_keys/recursive_triggers 设置。
// Insert 使用主键 UPSERT，不用 REPLACE；Update/Delete 必须精确匹配一行旧主键。
// 记录不是任意 SQL 执行入口，也不允许 schema 变更或 rowid 回退。
bool maiApplySqliteProjectionMutations(sqlite3* connection,
                                       const std::vector<MaiSqliteProjectionMutation>& changes,
                                       const MaiSqliteProjectionMutationOptions& options,
                                       std::string* error = nullptr);

// 版本化的 ASCII JSON 载荷；所有名字及 Text/Blob 字节用 base64，Real 用 IEEE 位编码。
// 不包含 SQL 或正文生成列。失败时不修改 output；调用方负责日志封装、持久化及大小上限。
bool maiEncodeSqliteProjectionMutations(const std::vector<MaiSqliteProjectionMutation>& changes,
                                        std::string& output, std::string* error = nullptr);
bool maiDecodeSqliteProjectionMutations(const std::string& payload,
                                        std::vector<MaiSqliteProjectionMutation>& output,
                                        std::string* error = nullptr);
