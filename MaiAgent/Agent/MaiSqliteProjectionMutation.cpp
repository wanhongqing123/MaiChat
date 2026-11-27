// 文件作用：捕获 SQLite 原生行事件并在相同 schema 上重放，不持有正文或磁盘日志。
#include "MaiSqliteProjectionMutation.h"

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstring>
#include <map>
#include <stdexcept>
#include <utility>

#include <json.hpp>
#include <sqlite3.h>

namespace {
using Value = MaiSqliteProjectionValue;
using ValueType = MaiSqliteProjectionValueType;
using Mutation = MaiSqliteProjectionMutation;
using MutationType = MaiSqliteProjectionMutationType;
using Json = nlohmann::json;

struct Column {
    int index = 0;
    std::string name;
    bool realAffinity = false;
};
struct Table {
    std::vector<Column> columns;
    std::vector<std::string> names;
    std::vector<std::string> primaryKeys;
    std::vector<std::size_t> keyIndexes;
    std::vector<std::string> keyCollations;
    int totalColumns = 0;
};
using Schema = std::map<std::string, Table>;
using Statement = std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)>;

// 只在本线程的 apply 作用域中抑制捕获，不改变连接上的 hook 所有权。
thread_local sqlite3* applyingConnection = nullptr;
struct ApplyScope {
    sqlite3* previous = applyingConnection;
    explicit ApplyScope(sqlite3* connection) {
        applyingConnection = connection;
    }
    ~ApplyScope() {
        applyingConnection = previous;
    }
};

bool fail(std::string* error, const std::string& message) {
    if (error) *error = message;
    return false;
}
bool validIdentifier(const std::string& name) {
    return !name.empty() && name.find('\0') == std::string::npos;
}
std::string quote(const std::string& name) {
    if (!validIdentifier(name)) throw std::runtime_error("Invalid SQLite identifier.");
    std::string result = "\"";
    for (const char value : name) {
        if (value == '"') result += '"';
        result += value;
    }
    return result + '"';
}
Statement prepare(sqlite3* connection, const std::string& sql) {
    sqlite3_stmt* raw = nullptr;
    const auto result = sqlite3_prepare_v2(connection, sql.c_str(), -1, &raw, nullptr);
    Statement statement(raw, sqlite3_finalize);
    if (result != SQLITE_OK) throw std::runtime_error(sqlite3_errmsg(connection));
    return statement;
}
void bind(sqlite3_stmt* statement, int index, const Value& value) {
    int result = SQLITE_MISUSE;
    switch (value.type) {
        case ValueType::Null: result = sqlite3_bind_null(statement, index); break;
        case ValueType::Integer:
            result = sqlite3_bind_int64(statement, index, value.integer);
            break;
        case ValueType::Real: result = sqlite3_bind_double(statement, index, value.real); break;
        case ValueType::Text:
            result = sqlite3_bind_text64(statement, index, value.bytes.data(), value.bytes.size(),
                                         SQLITE_TRANSIENT, SQLITE_UTF8);
            break;
        case ValueType::Blob:
            result = sqlite3_bind_blob64(statement, index, value.bytes.data(), value.bytes.size(),
                                         SQLITE_TRANSIENT);
            break;
    }
    if (result != SQLITE_OK) throw std::runtime_error("Cannot bind SQLite mutation value.");
}
void bindText(sqlite3_stmt* statement, int index, const std::string& bytes) {
    Value value;
    value.type = ValueType::Text;
    value.bytes = bytes;
    bind(statement, index, value);
}
std::string columnText(sqlite3_stmt* statement, int index) {
    const auto* text = sqlite3_column_text(statement, index);
    const auto size = sqlite3_column_bytes(statement, index);
    if (!text && sqlite3_column_type(statement, index) != SQLITE_NULL)
        throw std::runtime_error("Cannot read SQLite schema text.");
    return text ? std::string(reinterpret_cast<const char*>(text), size) : std::string();
}
bool hasRealAffinity(std::string type) {
    for (auto& character : type) {
        if (character >= 'a' && character <= 'z') character -= 'a' - 'A';
    }
    // SQLite 的 affinity 规则有固定优先级，不能仅搜索 REAL 子串。
    if (type.find("INT") != std::string::npos || type.find("CHAR") != std::string::npos ||
        type.find("CLOB") != std::string::npos || type.find("TEXT") != std::string::npos ||
        type.find("BLOB") != std::string::npos)
        return false;
    return type.find("REAL") != std::string::npos || type.find("FLOA") != std::string::npos ||
           type.find("DOUB") != std::string::npos;
}
sqlite3_int64 schemaVersion(sqlite3* connection) {
    auto statement = prepare(connection, "PRAGMA main.schema_version");
    if (sqlite3_step(statement.get()) != SQLITE_ROW)
        throw std::runtime_error("Cannot inspect mutation schema version.");
    return sqlite3_column_int64(statement.get(), 0);
}
Schema loadSchema(sqlite3* connection, const MaiSqliteProjectionMutationOptions& options) {
    if (!connection || options.allowedTables.empty())
        throw std::runtime_error("A connection and explicit table allowlist are required.");
    Schema schema;
    for (const auto& name : options.allowedTables) {
        if (!validIdentifier(name) || sqlite3_strnicmp(name.c_str(), "sqlite_", 7) == 0)
            throw std::runtime_error("Internal or invalid mutation table.");
        if (schema.count(name)) throw std::runtime_error("Duplicate mutation table.");
        auto kind = prepare(connection,
                            "SELECT type FROM pragma_table_list WHERE schema='main' AND name=?1");
        bindText(kind.get(), 1, name);
        if (sqlite3_step(kind.get()) != SQLITE_ROW || columnText(kind.get(), 0) != "table")
            throw std::runtime_error("Mutation table is missing or is not an ordinary main table.");
        auto statement = prepare(connection, "PRAGMA main.table_xinfo(" + quote(name) + ")");
        Table table;
        std::map<int, std::string> orderedKeys;
        int result;
        while ((result = sqlite3_step(statement.get())) == SQLITE_ROW) {
            const auto index = sqlite3_column_int(statement.get(), 0);
            const auto column = columnText(statement.get(), 1);
            const auto primaryKey = sqlite3_column_int(statement.get(), 5);
            const auto hidden = sqlite3_column_int(statement.get(), 6);
            if (index != table.totalColumns++ || !validIdentifier(column))
                throw std::runtime_error("Invalid mutation table column metadata.");
            // hidden=2/3 分别是 VIRTUAL/STORED；不读取生成值，避免正文或标量回调进入记录。
            if (hidden != 0) {
                if (primaryKey) throw std::runtime_error("Generated primary keys are unsupported.");
                continue;
            }
            table.columns.push_back(
                {index, column, hasRealAffinity(columnText(statement.get(), 2))});
            table.names.push_back(column);
            if (primaryKey) orderedKeys.emplace(primaryKey, column);
        }
        if (result != SQLITE_DONE) throw std::runtime_error(sqlite3_errmsg(connection));
        if (table.columns.empty() || orderedKeys.empty())
            throw std::runtime_error("Mutation tables require ordinary columns and a declared PK.");
        for (const auto& entry : orderedKeys) {
            if (entry.first != static_cast<int>(table.primaryKeys.size() + 1))
                throw std::runtime_error("Invalid declared primary key order.");
            table.primaryKeys.push_back(entry.second);
            table.keyIndexes.push_back(
                std::find(table.names.begin(), table.names.end(), entry.second) -
                table.names.begin());
            const char* collation = nullptr;
            if (sqlite3_table_column_metadata(connection, "main", name.c_str(),
                                              entry.second.c_str(), nullptr, &collation, nullptr,
                                              nullptr, nullptr) != SQLITE_OK ||
                !collation)
                throw std::runtime_error("Cannot inspect primary key collation.");
            table.keyCollations.emplace_back(collation);
        }
        // PRIMARY KEY(id COLLATE ...) 可以覆盖列声明的 collation，必须以实际主键索引为准。
        // INTEGER PRIMARY KEY 没有独立索引，保留上面列元信息中的 BINARY 即可。
        auto indexes =
            prepare(connection, "SELECT name FROM pragma_index_list(?1,'main') WHERE origin='pk'");
        bindText(indexes.get(), 1, name);
        const auto indexResult = sqlite3_step(indexes.get());
        if (indexResult == SQLITE_ROW) {
            auto keys = prepare(connection,
                                "SELECT name,coll FROM pragma_index_xinfo(?1,'main') "
                                "WHERE key=1 ORDER BY seqno");
            bindText(keys.get(), 1, columnText(indexes.get(), 0));
            std::size_t keyIndex = 0;
            while ((result = sqlite3_step(keys.get())) == SQLITE_ROW) {
                if (keyIndex >= table.primaryKeys.size() ||
                    columnText(keys.get(), 0) != table.primaryKeys[keyIndex])
                    throw std::runtime_error("Mutation primary key index identity mismatch.");
                table.keyCollations[keyIndex++] = columnText(keys.get(), 1);
            }
            if (result != SQLITE_DONE || keyIndex != table.primaryKeys.size())
                throw std::runtime_error("Cannot inspect mutation primary key index.");
        } else if (indexResult != SQLITE_DONE) {
            throw std::runtime_error(sqlite3_errmsg(connection));
        }
        schema.emplace(name, std::move(table));
    }
    return schema;
}
Value readValue(sqlite3_value* source) {
    if (!source) throw std::runtime_error("Missing SQLite row value.");
    Value value;
    switch (sqlite3_value_type(source)) {
        case SQLITE_NULL: value.type = ValueType::Null; break;
        case SQLITE_INTEGER:
            value.type = ValueType::Integer;
            value.integer = sqlite3_value_int64(source);
            break;
        case SQLITE_FLOAT:
            value.type = ValueType::Real;
            value.real = sqlite3_value_double(source);
            break;
        case SQLITE_TEXT:
        case SQLITE_BLOB: {
            const bool isText = sqlite3_value_type(source) == SQLITE_TEXT;
            value.type = isText ? ValueType::Text : ValueType::Blob;
            const void* bytes = isText ? static_cast<const void*>(sqlite3_value_text(source))
                                       : sqlite3_value_blob(source);
            const auto size = sqlite3_value_bytes(source);
            if (!bytes && (size || isText)) throw std::bad_alloc();
            if (size) value.bytes.assign(static_cast<const char*>(bytes), size);
            break;
        }
        default: throw std::runtime_error("Unknown SQLite value type.");
    }
    return value;
}
void validateValues(const std::vector<Value>& values, const Table& table) {
    if (values.size() != table.names.size())
        throw std::runtime_error("Mutation row width mismatch.");
    for (const auto key : table.keyIndexes) {
        if (values[key].type == ValueType::Null)
            throw std::runtime_error("Nullable primary key values cannot identify a unique row.");
    }
    for (const auto& value : values) {
        if (value.type < ValueType::Null || value.type > ValueType::Blob)
            throw std::runtime_error("Unknown mutation value type.");
    }
}
void validate(const Mutation& change, const Schema& schema) {
    const auto entry = schema.find(change.table);
    if (entry == schema.end()) throw std::runtime_error("Mutation table is not allowed.");
    const auto& table = entry->second;
    if (change.columns != table.names || change.primaryKeyColumns != table.primaryKeys ||
        change.depth < 0)
        throw std::runtime_error("Mutation schema identity mismatch.");
    switch (change.type) {
        case MutationType::Insert:
            if (!change.oldValues.empty()) throw std::runtime_error("Insert has an old row.");
            validateValues(change.newValues, table);
            break;
        case MutationType::Update:
            validateValues(change.oldValues, table);
            validateValues(change.newValues, table);
            break;
        case MutationType::Delete:
            validateValues(change.oldValues, table);
            if (!change.newValues.empty()) throw std::runtime_error("Delete has a new row.");
            break;
        default: throw std::runtime_error("Unknown mutation operation.");
    }
}
std::string listColumns(const std::vector<std::string>& names) {
    std::string result;
    for (const auto& name : names) {
        if (!result.empty()) result += ',';
        result += quote(name);
    }
    return result;
}
std::string keyPredicate(const Table& table) {
    std::string result;
    for (std::size_t index = 0; index != table.primaryKeys.size(); ++index) {
        if (!result.empty()) result += " AND ";
        result += quote(table.primaryKeys[index]) + " COLLATE " +
                  quote(table.keyCollations[index]) + " IS ?";
    }
    return result;
}
void bindKey(sqlite3_stmt* statement, int first, const Table& table,
             const std::vector<Value>& values) {
    for (const auto index : table.keyIndexes) bind(statement, first++, values[index]);
}
bool sameValue(const Value& left, const Value& right) {
    if (left.type != right.type) return false;
    switch (left.type) {
        case ValueType::Null: return true;
        case ValueType::Integer: return left.integer == right.integer;
        case ValueType::Real: return std::memcmp(&left.real, &right.real, sizeof(double)) == 0;
        case ValueType::Text:
        case ValueType::Blob: return left.bytes == right.bytes;
    }
    return false;
}
void verifyRow(sqlite3* connection, const std::string& name, const Table& table,
               const std::vector<Value>& values, bool present) {
    auto statement = prepare(connection, "SELECT " + listColumns(table.names) + " FROM main." +
                                             quote(name) + " WHERE " + keyPredicate(table));
    bindKey(statement.get(), 1, table, values);
    auto result = sqlite3_step(statement.get());
    if (!present) {
        if (result != SQLITE_DONE)
            throw std::runtime_error("Deleted mutation row is still present.");
        return;
    }
    if (result != SQLITE_ROW)
        throw std::runtime_error("Mutation expected exactly one existing row.");
    for (std::size_t index = 0; index != values.size(); ++index) {
        const auto actual =
            readValue(sqlite3_column_value(statement.get(), static_cast<int>(index)));
        const auto& expected = values[index];
        // SQLite 的 REAL 列可能在 preupdate_new 中用整数存储优化，SELECT 会施加 REAL affinity。
        // 记录仍保留 hook 原始类型，只在验证相同 schema 的查询结果时允许这一个已知转换。
        const bool affinityMatch =
            table.columns[index].realAffinity && actual.type == ValueType::Real &&
            ((expected.type == ValueType::Integer &&
              actual.real == static_cast<double>(expected.integer)) ||
             (expected.type == ValueType::Real && actual.real == expected.real));
        if (!sameValue(actual, expected) && !affinityMatch)
            throw std::runtime_error("Mutation row values do not match the captured column: " +
                                     table.names[index]);
    }
    if (sqlite3_step(statement.get()) != SQLITE_DONE)
        throw std::runtime_error("Mutation primary key did not identify exactly one row.");
}
void applyOne(sqlite3* connection, const Mutation& change, const Table& table) {
    if (change.type != MutationType::Insert)
        verifyRow(connection, change.table, table, change.oldValues, true);
    std::string sql;
    if (change.type == MutationType::Insert) {
        sql = "INSERT INTO main." + quote(change.table) + "(" + listColumns(table.names) +
              ") VALUES(";
        for (std::size_t index = 0; index != table.names.size(); ++index) sql += index ? ",?" : "?";
        sql += ") ON CONFLICT(" + listColumns(table.primaryKeys) + ") DO UPDATE SET ";
        for (std::size_t index = 0; index != table.names.size(); ++index) {
            if (index) sql += ',';
            sql += quote(table.names[index]) + "=excluded." + quote(table.names[index]);
        }
    } else if (change.type == MutationType::Update) {
        sql = "UPDATE main." + quote(change.table) + " SET ";
        for (std::size_t index = 0; index != table.names.size(); ++index) {
            if (index) sql += ',';
            sql += quote(table.names[index]) + "=?";
        }
        sql += " WHERE " + keyPredicate(table);
    } else {
        sql = "DELETE FROM main." + quote(change.table) + " WHERE " + keyPredicate(table);
    }
    auto statement = prepare(connection, sql);
    int index = 1;
    if (change.type != MutationType::Delete) {
        for (const auto& value : change.newValues) bind(statement.get(), index++, value);
    }
    if (change.type != MutationType::Insert)
        bindKey(statement.get(), index, table, change.oldValues);
    if (sqlite3_step(statement.get()) != SQLITE_DONE)
        throw std::runtime_error(sqlite3_errmsg(connection));
    if (sqlite3_changes64(connection) != 1)
        throw std::runtime_error("Mutation did not affect exactly one row.");
}
// 使用 SQLite 自己的主键比较和 collation；不能用字符串 key 推测 NOCASE/数值键的身份。
bool sameKey(sqlite3* connection, const Table& table, const std::vector<Value>& left,
             const std::vector<Value>& right) {
    std::string sql = "SELECT ";
    for (std::size_t index = 0; index != table.keyIndexes.size(); ++index) {
        if (index) sql += " AND ";
        sql += "? COLLATE " + quote(table.keyCollations[index]) + " IS ?";
    }
    auto statement = prepare(connection, sql);
    int bindIndex = 1;
    for (const auto index : table.keyIndexes) {
        bind(statement.get(), bindIndex++, left[index]);
        bind(statement.get(), bindIndex++, right[index]);
    }
    if (sqlite3_step(statement.get()) != SQLITE_ROW)
        throw std::runtime_error("Cannot compare mutation primary keys.");
    return sqlite3_column_int(statement.get(), 0) != 0;
}
struct ExpectedRow {
    const Mutation* change = nullptr;
    const std::vector<Value>* values = nullptr;
    bool present = false;
};
void appendKeyInteger(std::string& output, std::uint64_t value) {
    for (int shift = 56; shift >= 0; shift -= 8) output += static_cast<char>(value >> shift);
}
std::string binaryKey(const Table& table, const std::vector<Value>& values) {
    std::string result;
    for (const auto index : table.keyIndexes) {
        const auto& value = values[index];
        auto type = value.type;
        auto integer = value.integer;
        // SQLite 的主键比较将可精确表示的整数浮点数视为同一数值；保留大整数全部 64 位。
        if (type == ValueType::Real && std::isfinite(value.real) &&
            value.real >= -9223372036854775808.0 && value.real < 9223372036854775808.0 &&
            std::trunc(value.real) == value.real) {
            type = ValueType::Integer;
            integer = static_cast<std::int64_t>(value.real);
        }
        result += static_cast<char>(type);
        switch (type) {
            case ValueType::Integer:
                appendKeyInteger(result, static_cast<std::uint64_t>(integer));
                break;
            case ValueType::Real: {
                std::uint64_t bits;
                std::memcpy(&bits, &value.real, sizeof(bits));
                appendKeyInteger(result, bits);
                break;
            }
            case ValueType::Text:
            case ValueType::Blob:
                appendKeyInteger(result, value.bytes.size());
                result += value.bytes;
                break;
            case ValueType::Null: throw std::runtime_error("Nullable mutation primary key.");
        }
    }
    return result;
}
void verifyFinalRows(sqlite3* connection, const std::vector<Mutation>& changes,
                     const Schema& schema) {
    // 常规 BINARY 主键按值聚合，批量删除/导入不做平方级 SQLite prepare。
    // 自定义 collation 不能由 C++ 猜测，仍交给 SQLite 比较，保证少量特殊表的正确性。
    std::map<std::pair<std::string, std::string>, ExpectedRow> binaryRows;
    std::vector<ExpectedRow> expected;
    const auto remember = [&](const Mutation& change, const std::vector<Value>& values,
                              bool present) {
        const auto& table = schema.at(change.table);
        const bool binary =
            std::all_of(table.keyCollations.begin(), table.keyCollations.end(),
                        [](const std::string& collation) {
                            return sqlite3_stricmp(collation.c_str(), "BINARY") == 0;
                        });
        if (binary) {
            binaryRows[{change.table, binaryKey(table, values)}] = {&change, &values, present};
            return;
        }
        for (auto& row : expected) {
            if (row.change->table == change.table &&
                sameKey(connection, table, *row.values, values)) {
                row = {&change, &values, present};
                return;
            }
        }
        expected.push_back({&change, &values, present});
    };
    for (const auto& change : changes) {
        if (!change.oldValues.empty()) remember(change, change.oldValues, false);
        if (!change.newValues.empty()) remember(change, change.newValues, true);
    }
    for (const auto& row : expected)
        verifyRow(connection, row.change->table, schema.at(row.change->table), *row.values,
                  row.present);
    for (const auto& entry : binaryRows) {
        const auto& row = entry.second;
        verifyRow(connection, row.change->table, schema.at(row.change->table), *row.values,
                  row.present);
    }
}

const char base64Digits[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
std::string encodeBytes(const std::string& bytes) {
    std::string result;
    for (std::size_t index = 0; index < bytes.size(); index += 3) {
        const auto first = static_cast<unsigned char>(bytes[index]);
        const auto second =
            index + 1 < bytes.size() ? static_cast<unsigned char>(bytes[index + 1]) : 0;
        const auto third =
            index + 2 < bytes.size() ? static_cast<unsigned char>(bytes[index + 2]) : 0;
        result += base64Digits[first >> 2];
        result += base64Digits[((first & 3) << 4) | (second >> 4)];
        result +=
            index + 1 < bytes.size() ? base64Digits[((second & 15) << 2) | (third >> 6)] : '=';
        result += index + 2 < bytes.size() ? base64Digits[third & 63] : '=';
    }
    return result;
}
std::string decodeBytes(const Json& encoded) {
    if (!encoded.is_string()) throw std::runtime_error("Mutation bytes must be base64 strings.");
    const auto& text = encoded.get_ref<const std::string&>();
    if (text.size() % 4) throw std::runtime_error("Invalid mutation base64 length.");
    std::string result;
    for (std::size_t index = 0; index < text.size(); index += 4) {
        unsigned int bits = 0;
        int padding = 0;
        for (std::size_t offset = 0; offset != 4; ++offset) {
            const auto value = text[index + offset];
            if (value == '=') {
                if (offset < 2 || index + 4 != text.size())
                    throw std::runtime_error("Invalid mutation base64 padding.");
                ++padding;
                bits <<= 6;
            } else {
                const char* digit = value ? std::strchr(base64Digits, value) : nullptr;
                if (!digit || padding)
                    throw std::runtime_error("Invalid mutation base64 alphabet.");
                bits = (bits << 6) | static_cast<unsigned int>(digit - base64Digits);
            }
        }
        if (padding > 2 || (padding == 1 && (bits & 255)) || (padding == 2 && (bits & 65535)))
            throw std::runtime_error("Non-canonical mutation base64 padding.");
        result += static_cast<char>(bits >> 16);
        if (padding < 2) result += static_cast<char>(bits >> 8);
        if (padding < 1) result += static_cast<char>(bits);
    }
    return result;
}
Json encodeValues(const std::vector<Value>& values) {
    Json result = Json::array();
    for (const auto& value : values) {
        Json entry = Json::array();
        entry.push_back(static_cast<int>(value.type));
        switch (value.type) {
            case ValueType::Null: break;
            case ValueType::Integer: entry.push_back(value.integer); break;
            case ValueType::Real: {
                static_assert(sizeof(double) == sizeof(std::uint64_t), "64-bit doubles required");
                std::uint64_t bits;
                std::memcpy(&bits, &value.real, sizeof(bits));
                // 显式高位在前，不把宿主端字节序写入日志格式。
                std::string bytes;
                for (int shift = 56; shift >= 0; shift -= 8)
                    bytes += static_cast<char>(bits >> shift);
                entry.push_back(encodeBytes(bytes));
                break;
            }
            case ValueType::Text:
            case ValueType::Blob: entry.push_back(encodeBytes(value.bytes)); break;
            default: throw std::runtime_error("Unknown mutation value type.");
        }
        result.push_back(std::move(entry));
    }
    return result;
}
std::int64_t signedInteger(const Json& value) {
    if (!value.is_number_integer() ||
        (value.is_number_unsigned() &&
         value.get<std::uint64_t>() > static_cast<std::uint64_t>(INT64_MAX)))
        throw std::runtime_error("Mutation integer is outside int64 range.");
    return value.get<std::int64_t>();
}
std::vector<Value> decodeValues(const Json& encoded) {
    if (!encoded.is_array()) throw std::runtime_error("Mutation row must be an array.");
    std::vector<Value> result;
    for (const auto& entry : encoded) {
        if (!entry.is_array() || entry.empty())
            throw std::runtime_error("Invalid typed mutation value.");
        const auto type = signedInteger(entry.at(0));
        if (type < 0 || type > static_cast<int>(ValueType::Blob) || entry.size() != (type ? 2 : 1))
            throw std::runtime_error("Invalid typed mutation value shape.");
        Value value;
        value.type = static_cast<ValueType>(type);
        switch (value.type) {
            case ValueType::Null: break;
            case ValueType::Integer: value.integer = signedInteger(entry.at(1)); break;
            case ValueType::Real: {
                const auto bytes = decodeBytes(entry.at(1));
                if (bytes.size() != 8) throw std::runtime_error("Invalid mutation double width.");
                std::uint64_t bits = 0;
                for (const unsigned char byte : bytes) bits = (bits << 8) | byte;
                std::memcpy(&value.real, &bits, sizeof(bits));
                break;
            }
            case ValueType::Text:
            case ValueType::Blob: value.bytes = decodeBytes(entry.at(1)); break;
        }
        result.push_back(std::move(value));
    }
    return result;
}
Json encodeNames(const std::vector<std::string>& names) {
    Json result = Json::array();
    for (const auto& name : names) result.push_back(encodeBytes(name));
    return result;
}
std::vector<std::string> decodeNames(const Json& encoded) {
    if (!encoded.is_array()) throw std::runtime_error("Mutation names must be an array.");
    std::vector<std::string> result;
    for (const auto& entry : encoded) result.push_back(decodeBytes(entry));
    return result;
}
}  // namespace

struct MaiSqliteProjectionMutationRecorder::State {
    sqlite3* connection;
    MaiSqliteProjectionMutationOptions options;
    Schema schema;
    std::vector<Mutation> changes;
    std::string detail;
    const char* failure = nullptr;
    bool active = false;
    sqlite3_int64 version = 0;
    State(sqlite3* borrowed, MaiSqliteProjectionMutationOptions supplied)
        : connection(borrowed), options(std::move(supplied)) {}
    void reject(const char* message) noexcept {
        if (!failure) failure = message;
        sqlite3_interrupt(connection);
    }
    static void hook(void* context, sqlite3* connection, int operation, const char* database,
                     const char* name, sqlite3_int64, sqlite3_int64) noexcept {
        auto& state = *static_cast<State*>(context);
        if (!state.active || applyingConnection == connection) return;
        if (state.failure) {
            sqlite3_interrupt(connection);
            return;
        }
        try {
            if (!database || std::strcmp(database, "main") || !name) {
                state.reject("Mutation capture permits only main tables.");
                return;
            }
            const auto found = state.schema.find(name);
            if (found == state.schema.end()) {
                state.reject("A written mutation table was not allowed.");
                return;
            }
            const auto& table = found->second;
            if (sqlite3_preupdate_count(connection) != table.totalColumns ||
                sqlite3_preupdate_blobwrite(connection) >= 0) {
                state.reject("Mutation schema changed or incremental blob writes were used.");
                return;
            }
            Mutation change;
            change.table = name;
            change.columns = table.names;
            change.primaryKeyColumns = table.primaryKeys;
            change.depth = sqlite3_preupdate_depth(connection);
            switch (operation) {
                case SQLITE_INSERT: change.type = MutationType::Insert; break;
                case SQLITE_UPDATE: change.type = MutationType::Update; break;
                case SQLITE_DELETE: change.type = MutationType::Delete; break;
                default: state.reject("Unknown SQLite mutation operation."); return;
            }
            for (const auto& column : table.columns) {
                sqlite3_value* value = nullptr;
                if (operation != SQLITE_INSERT) {
                    if (sqlite3_preupdate_old(connection, column.index, &value) != SQLITE_OK)
                        throw std::runtime_error("Cannot read old SQLite mutation value.");
                    change.oldValues.push_back(readValue(value));
                }
                if (operation != SQLITE_DELETE) {
                    if (sqlite3_preupdate_new(connection, column.index, &value) != SQLITE_OK)
                        throw std::runtime_error("Cannot read new SQLite mutation value.");
                    change.newValues.push_back(readValue(value));
                }
            }
            validate(change, state.schema);
            state.changes.push_back(std::move(change));
        } catch (...) {
            // 不能在失败分支分配错误字符串；C 边界不允许异常逃逸。
            state.reject("Mutation capture failed while copying or validating row values.");
        }
    }
};
MaiSqliteProjectionMutationRecorder::MaiSqliteProjectionMutationRecorder(
    sqlite3* connection, MaiSqliteProjectionMutationOptions options)
    : mState(std::make_unique<State>(connection, std::move(options))) {
    if (connection) sqlite3_preupdate_hook(connection, State::hook, mState.get());
}
MaiSqliteProjectionMutationRecorder::~MaiSqliteProjectionMutationRecorder() {
    if (mState->connection) sqlite3_preupdate_hook(mState->connection, nullptr, nullptr);
}
bool MaiSqliteProjectionMutationRecorder::beginCapture() {
    if (mState->active) {
        mState->failure = "Mutation capture is already active.";
        return false;
    }
    mState->changes.clear();
    mState->detail.clear();
    mState->failure = nullptr;
    try {
        mState->schema = loadSchema(mState->connection, mState->options);
        mState->version = schemaVersion(mState->connection);
        mState->active = true;
        return true;
    } catch (const std::exception& error) {
        mState->failure = "Cannot prepare mutation schema.";
        try {
            mState->detail = error.what();
        } catch (...) {
        }
    } catch (...) {
        mState->failure = "Cannot prepare mutation schema.";
    }
    return false;
}
bool MaiSqliteProjectionMutationRecorder::endCapture() {
    if (!mState->active && !mState->failure) mState->failure = "Mutation capture is not active.";
    mState->active = false;
    if (!mState->failure) {
        try {
            if (schemaVersion(mState->connection) != mState->version)
                mState->failure = "Mutation schema changed during capture.";
        } catch (...) {
            mState->failure = "Cannot validate mutation schema after capture.";
        }
    }
    return !mState->failure;
}
const std::vector<MaiSqliteProjectionMutation>&
MaiSqliteProjectionMutationRecorder::capturedChanges() const {
    return mState->changes;
}
bool MaiSqliteProjectionMutationRecorder::hasError() const {
    return mState->failure != nullptr;
}
std::string MaiSqliteProjectionMutationRecorder::error() const {
    return mState->detail.empty() ? (mState->failure ? mState->failure : "") : mState->detail;
}
bool maiApplySqliteProjectionMutations(sqlite3* connection, const std::vector<Mutation>& changes,
                                       const MaiSqliteProjectionMutationOptions& options,
                                       std::string* error) {
    if (error) error->clear();
    if (!connection || sqlite3_get_autocommit(connection))
        return fail(error, "Mutation apply requires a caller-owned active transaction.");
    try {
        ApplyScope scope(connection);
        const auto schema = loadSchema(connection, options);
        // 先验证整批，避免格式错误造成任何部分执行；SQL/FK 错误仍由调用方整批回滚。
        for (const auto& change : changes) validate(change, schema);
        for (const auto& change : changes) {
            if (!change.depth) applyOne(connection, change, schema.at(change.table));
        }
        verifyFinalRows(connection, changes, schema);
        return true;
    } catch (const std::exception& exception) {
        return fail(error, exception.what());
    } catch (...) {
        return fail(error, "Cannot apply SQLite mutation batch.");
    }
}
bool maiEncodeSqliteProjectionMutations(const std::vector<Mutation>& changes, std::string& output,
                                        std::string* error) {
    if (error) error->clear();
    try {
        Json entries = Json::array();
        for (const auto& change : changes) {
            entries.push_back(Json::array(
                {static_cast<int>(change.type), encodeBytes(change.table),
                 encodeNames(change.columns), encodeNames(change.primaryKeyColumns),
                 encodeValues(change.oldValues), encodeValues(change.newValues), change.depth}));
        }
        auto encoded = Json{{"version", 1}, {"changes", std::move(entries)}}.dump();
        output = std::move(encoded);
        return true;
    } catch (const std::exception& exception) {
        return fail(error, exception.what());
    }
}
bool maiDecodeSqliteProjectionMutations(const std::string& payload, std::vector<Mutation>& output,
                                        std::string* error) {
    if (error) error->clear();
    try {
        const auto document = Json::parse(payload);
        if (!document.is_object() || document.size() != 2 ||
            signedInteger(document.at("version")) != 1 || !document.at("changes").is_array())
            throw std::runtime_error("Unsupported mutation payload envelope.");
        std::vector<Mutation> changes;
        for (const auto& entry : document.at("changes")) {
            if (!entry.is_array() || entry.size() != 7)
                throw std::runtime_error("Invalid mutation payload entry.");
            const auto type = signedInteger(entry.at(0));
            const auto depth = signedInteger(entry.at(6));
            if (type < 0 || type > static_cast<int>(MutationType::Delete) || depth < 0 ||
                depth > INT_MAX)
                throw std::runtime_error("Invalid mutation operation or depth.");
            Mutation change;
            change.type = static_cast<MutationType>(type);
            change.table = decodeBytes(entry.at(1));
            change.columns = decodeNames(entry.at(2));
            change.primaryKeyColumns = decodeNames(entry.at(3));
            change.oldValues = decodeValues(entry.at(4));
            change.newValues = decodeValues(entry.at(5));
            change.depth = static_cast<int>(depth);
            changes.push_back(std::move(change));
        }
        output = std::move(changes);
        return true;
    } catch (const std::exception& exception) {
        return fail(error, exception.what());
    }
}
