// 文件作用：在账号私有 SQLite 中保存修图反馈，以成对排序损失训练小型个人模型。
// 每个属性独立九个权重；图像特征和强度的交互项允许不同照片使用不同程度，
// 二次项提供平滑强度偏好。不是把一次调整写成永久固定默认值。
#include "MaiPortraitPersonalization.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <stdexcept>
#include <utility>

#include <json.hpp>
#include <sqlite3.h>

#include "MaiFilePath.h"
#include "MaiFileSystem.h"
#include "MaiIdGenerator.h"

namespace {
using Json = nlohmann::json;
using Weights = std::array<std::array<double, 9>, MaiPortraitParameters::kCount>;
Weights priorWeights() {
    Weights weights{};
    const MaiPortraitParameters defaults;
    for (int attribute = 0; attribute < MaiPortraitParameters::kCount; ++attribute) {
        weights[attribute][0] = defaults.values[attribute] * 2;
        weights[attribute][8] = 1;
    }
    return weights;
}
MaiPortraitParameters::Values storedParameters(const std::string& encoded) {
    const auto data = Json::parse(encoded);
    if (!data.is_array() || (data.size() != 7 && data.size() != MaiPortraitParameters::kCount))
        throw std::runtime_error("Portrait result contains invalid parameters");
    // 美白追加到末尾；旧七属性记录补零，不改动已收集的参数和评价索引。
    MaiPortraitParameters::Values values{};
    for (size_t index = 0; index < data.size(); ++index) values[index] = data[index].get<float>();
    return values;
}
Weights storedWeights(const std::string& encoded) {
    const auto data = Json::parse(encoded);
    if (!data.is_array() || (data.size() != 7 && data.size() != MaiPortraitParameters::kCount))
        throw std::runtime_error("Personal model has an unsupported attribute count");
    auto weights = priorWeights();
    // 旧模型保留原有 63 个权重，新美白头使用默认关闭的先验；后续版本统一保存八属性。
    for (size_t attribute = 0; attribute < data.size(); ++attribute)
        weights[attribute] = data[attribute].get<std::array<double, 9>>();
    for (const auto& row : weights)
        for (double value : row)
            if (!std::isfinite(value)) throw std::runtime_error("Personal model contains invalid weights");
    return weights;
}
struct Sample {
    std::string source;
    int attribute;
    std::array<double, 9> difference;
};
double scoreDifference(const Weights& weights, const Sample& sample) {
    double result = 0;
    for (int index = 0; index < 9; ++index) result += weights[sample.attribute][index] * sample.difference[index];
    return result;
}
double loss(const Weights& weights, const std::vector<Sample>& samples) {
    if (samples.empty()) return -1;
    double total = 0;
    for (const auto& sample : samples) {
        const double difference = -scoreDifference(weights, sample);
        total += std::max(0.0, difference) + std::log1p(std::exp(-std::abs(difference)));
    }
    return total / samples.size();
}
class Statement {
public:
    Statement(sqlite3* database, const char* sql) {
        if (sqlite3_prepare_v2(database, sql, -1, &statement, nullptr) != SQLITE_OK)
            throw std::runtime_error("Cannot prepare portrait storage operation");
    }
    ~Statement() {
        sqlite3_finalize(statement);
    }
    void text(int index, const std::string& value) {
        sqlite3_bind_text(statement, index, value.c_str(), static_cast<int>(value.size()), SQLITE_TRANSIENT);
    }
    void integer(int index, int value) {
        sqlite3_bind_int(statement, index, value);
    }
    std::string string(int column) {
        const auto* value = sqlite3_column_text(statement, column);
        return value ? reinterpret_cast<const char*>(value) : "";
    }
    int step() {
        int code = sqlite3_step(statement);
        if (code != SQLITE_ROW && code != SQLITE_DONE) throw std::runtime_error("Portrait storage operation failed");
        return code;
    }
    sqlite3_stmt* statement = nullptr;
};
}  // namespace

class MaiPortraitPersonalization::Implementation {
public:
    explicit Implementation(const std::string& directory) {
        const auto root = MaiFilePath::fromUtf8(directory);
        if (directory.empty() || MaiFileSystem::createDirectories(root))
            throw std::runtime_error("Cannot create personal portrait directory");
        const auto path = root.append(MaiFilePath::fromUtf8("portrait.sqlite3"));
        if (sqlite3_open_v2(path.toUtf8().c_str(), &database,
                            SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, nullptr) != SQLITE_OK) {
            if (database) sqlite3_close(database);
            database = nullptr;
            throw std::runtime_error("Cannot open personal portrait storage");
        }
        sqlite3_busy_timeout(database, 2000);
        execute(
            "PRAGMA journal_mode=WAL; PRAGMA foreign_keys=ON; CREATE TABLE IF NOT EXISTS portrait_results(id TEXT "
            "PRIMARY KEY, source TEXT NOT NULL, source_path TEXT NOT NULL, output_path TEXT NOT NULL, parameters TEXT "
            "NOT NULL, features TEXT NOT NULL); CREATE INDEX IF NOT EXISTS portrait_output ON "
            "portrait_results(output_path); CREATE TABLE IF NOT EXISTS portrait_feedback(preferred TEXT NOT NULL "
            "REFERENCES portrait_results(id), rejected TEXT NOT NULL REFERENCES portrait_results(id), attribute "
            "INTEGER NOT NULL, UNIQUE(preferred,rejected,attribute)); CREATE TABLE IF NOT EXISTS "
            "portrait_versions(version INTEGER PRIMARY KEY AUTOINCREMENT,weights TEXT NOT NULL,active INTEGER NOT NULL "
            "DEFAULT 0,validated INTEGER NOT NULL DEFAULT 0);");
    }
    ~Implementation() {
        if (database) sqlite3_close(database);
    }
    void execute(const char* sql) {
        if (sqlite3_exec(database, sql, nullptr, nullptr, nullptr) != SQLITE_OK)
            throw std::runtime_error("Portrait storage transaction failed");
    }
    Weights activeWeights() {
        Statement query(database, "SELECT weights FROM portrait_versions WHERE active=1 ORDER BY version DESC LIMIT 1");
        if (query.step() == SQLITE_DONE) return priorWeights();
        return storedWeights(query.string(0));
    }
    sqlite3* database = nullptr;
};

MaiPortraitPersonalization::MaiPortraitPersonalization(const std::string& directory)
    : mImplementation(std::make_unique<Implementation>(directory)) {}
MaiPortraitPersonalization::~MaiPortraitPersonalization() = default;

std::string MaiPortraitPersonalization::originalSource(const std::string& path) {
    Statement query(mImplementation->database,
                    "SELECT source_path FROM portrait_results WHERE output_path=? ORDER BY rowid DESC LIMIT 1");
    const auto resolved = MaiFileSystem::resolve(MaiFilePath::fromUtf8(path)).toUtf8();
    query.text(1, resolved);
    return query.step() == SQLITE_ROW ? query.string(0) : path;
}
std::string MaiPortraitPersonalization::recordResult(const std::string& fingerprint, const std::string& sourcePath,
                                                     const std::string& outputPath,
                                                     const MaiPortraitParameters& parameters,
                                                     const std::array<float, 8>& features) {
    if (fingerprint.empty() || sourcePath.empty() || outputPath.empty() || sourcePath == outputPath)
        throw std::runtime_error("Invalid portrait result metadata");
    for (float value : features)
        if (!std::isfinite(value)) throw std::runtime_error("Invalid portrait features");
    const std::string identifier = MaiIdGenerator::generate("portrait_");
    Statement insert(mImplementation->database, "INSERT INTO portrait_results VALUES(?,?,?,?,?,?)");
    insert.text(1, identifier);
    insert.text(2, fingerprint);
    // Android 的 /data/data 与 /data/user/0 等沙盒路径可能指向同一文件。
    // 保存和查询统一真实路径，避免第二次修图把已生成图片误当作新的原图。
    insert.text(3, MaiFileSystem::resolve(MaiFilePath::fromUtf8(sourcePath)).toUtf8());
    insert.text(4, MaiFileSystem::resolve(MaiFilePath::fromUtf8(outputPath)).toUtf8());
    insert.text(5, Json(parameters.values).dump());
    insert.text(6, Json(features).dump());
    insert.step();
    return identifier;
}
int MaiPortraitPersonalization::recordPreference(const std::string& preferred, const std::string& rejected,
                                                 const std::vector<int>& attributes) {
    if (preferred == rejected || attributes.empty() || attributes.size() > MaiPortraitParameters::kCount)
        throw std::runtime_error("Select distinct preferred and rejected results and evaluated attributes");
    std::array<std::string, 2> sources;
    std::array<MaiPortraitParameters::Values, 2> parameters;
    for (int index = 0; index < 2; ++index) {
        Statement query(mImplementation->database, "SELECT source,parameters FROM portrait_results WHERE id=?");
        query.text(1, index ? rejected : preferred);
        if (query.step() != SQLITE_ROW)
            throw std::runtime_error("Portrait result does not belong to the current profile");
        sources[index] = query.string(0);
        parameters[index] = storedParameters(query.string(1));
    }
    if (sources[0] != sources[1]) throw std::runtime_error("Compare results generated from the same original photo");
    std::set<int> unique(attributes.begin(), attributes.end());
    for (int attribute : unique)
        if (attribute < 0 || attribute >= MaiPortraitParameters::kCount ||
            std::abs(parameters[0][attribute] - parameters[1][attribute]) < 0.0001f)
            throw std::runtime_error("Feedback attribute is invalid or has no difference between the results");
    int inserted = 0;
    mImplementation->execute("BEGIN IMMEDIATE");
    try {
        for (int attribute : unique) {
            Statement conflict(mImplementation->database,
                               "SELECT 1 FROM portrait_feedback WHERE preferred=? AND rejected=? AND attribute=?");
            conflict.text(1, rejected);
            conflict.text(2, preferred);
            conflict.integer(3, attribute);
            if (conflict.step() == SQLITE_ROW)
                throw std::runtime_error("Conflicting feedback exists for this result pair");
            Statement insert(mImplementation->database, "INSERT OR IGNORE INTO portrait_feedback VALUES(?,?,?)");
            insert.text(1, preferred);
            insert.text(2, rejected);
            insert.integer(3, attribute);
            insert.step();
            inserted += sqlite3_changes(mImplementation->database);
        }
        mImplementation->execute("COMMIT");
    } catch (...) {
        mImplementation->execute("ROLLBACK");
        throw;
    }
    return inserted;
}
MaiPortraitParameters MaiPortraitPersonalization::recommend(const std::array<float, 8>& features) {
    const auto weights = mImplementation->activeWeights();
    MaiPortraitParameters result;
    for (int attribute = 0; attribute < MaiPortraitParameters::kCount; ++attribute) {
        double linear = 0;
        for (int index = 0; index < 8; ++index) linear += weights[attribute][index] * features[index];
        result.values[attribute] = static_cast<float>(
            std::clamp(linear / (2 * std::max(0.1, weights[attribute][8])), attribute == 2 ? -1.0 : 0.0, 1.0));
    }
    return result;
}
MaiPortraitProfileStatus MaiPortraitPersonalization::status() {
    MaiPortraitProfileStatus result;
    Statement count(mImplementation->database, "SELECT count(*) FROM portrait_feedback");
    if (count.step() == SQLITE_ROW) result.feedbackCount = sqlite3_column_int(count.statement, 0);
    Statement versions(mImplementation->database,
                       "SELECT version,active FROM portrait_versions ORDER BY version DESC LIMIT 100");
    while (versions.step() == SQLITE_ROW) {
        int version = sqlite3_column_int(versions.statement, 0);
        result.versions.push_back(version);
        if (sqlite3_column_int(versions.statement, 1)) result.activeVersion = version;
    }
    return result;
}
MaiPortraitTrainingSummary MaiPortraitPersonalization::train() {
    std::vector<Sample> samples;
    std::set<std::string> sources;
    Statement query(mImplementation->database,
                    "SELECT p.source,p.parameters,r.parameters,p.features,f.attribute FROM portrait_feedback f JOIN "
                    "portrait_results p ON p.id=f.preferred JOIN portrait_results r ON r.id=f.rejected ORDER BY "
                    "f.rowid DESC LIMIT 512");
    while (query.step() == SQLITE_ROW) {
        auto preferred = storedParameters(query.string(1));
        auto rejected = storedParameters(query.string(2));
        auto features = Json::parse(query.string(3)).get<std::array<float, 8>>();
        Sample sample;
        sample.source = query.string(0);
        sample.attribute = sqlite3_column_int(query.statement, 4);
        if (sample.attribute < 0 || sample.attribute >= MaiPortraitParameters::kCount)
            throw std::runtime_error("Portrait feedback contains an invalid attribute");
        const double first = preferred[sample.attribute], second = rejected[sample.attribute];
        for (int index = 0; index < 8; ++index) sample.difference[index] = features[index] * (first - second);
        sample.difference[8] = -(first * first - second * second);
        sources.insert(sample.source);
        samples.push_back(sample);
    }
    if (samples.empty()) throw std::runtime_error("No explicit portrait preferences are available for training");
    std::vector<Sample> training, validation;
    std::set<std::string> heldOut;
    if (sources.size() >= 3) {
        int index = 0;
        for (const auto& source : sources) {
            if (index++ % 5 == 0) heldOut.insert(source);
        }
    }
    for (const auto& sample : samples) (heldOut.count(sample.source) ? validation : training).push_back(sample);
    if (training.empty()) throw std::runtime_error("Not enough independent training photos");
    auto weights = mImplementation->activeWeights();
    const auto before = weights;
    for (int epoch = 0; epoch < 120; ++epoch)
        for (const auto& sample : training) {
            const double margin = std::clamp(scoreDifference(weights, sample), -40.0, 40.0);
            const double gradient = -1.0 / (1.0 + std::exp(margin));
            for (int index = 0; index < 9; ++index)
                weights[sample.attribute][index] -=
                    0.04 * (gradient * sample.difference[index] +
                            0.002 * (weights[sample.attribute][index] - before[sample.attribute][index]));
            weights[sample.attribute][8] = std::clamp(weights[sample.attribute][8], 0.1, 8.0);
        }
    MaiPortraitTrainingSummary result;
    result.feedbackCount = static_cast<int>(samples.size());
    result.sourceCount = static_cast<int>(sources.size());
    result.trainingLoss = loss(weights, training);
    result.validationLoss = loss(weights, validation);
    result.activated = !validation.empty() && result.validationLoss + 0.00001 < loss(before, validation) &&
                       result.trainingLoss <= loss(before, training);
    // 不以肤质的验证改善掩盖只有一张图的瘦脸或美体反馈。
    // 本次涉及的各项属性均具备独立图像和验证改善后，才整体启用新版本。
    for (int attribute = 0; attribute < MaiPortraitParameters::kCount; ++attribute) {
        std::set<std::string> attributeSources;
        std::vector<Sample> attributeTraining, attributeValidation;
        for (const auto& sample : samples)
            if (sample.attribute == attribute) attributeSources.insert(sample.source);
        if (attributeSources.empty()) continue;
        for (const auto& sample : training)
            if (sample.attribute == attribute) attributeTraining.push_back(sample);
        for (const auto& sample : validation)
            if (sample.attribute == attribute) attributeValidation.push_back(sample);
        if (attributeSources.size() < 3 || attributeTraining.empty() || attributeValidation.empty() ||
            loss(weights, attributeValidation) + 0.00001 >= loss(before, attributeValidation) ||
            loss(weights, attributeTraining) > loss(before, attributeTraining))
            result.activated = false;
    }
    mImplementation->execute("BEGIN IMMEDIATE");
    try {
        if (result.activated) mImplementation->execute("UPDATE portrait_versions SET active=0");
        Statement insert(mImplementation->database,
                         "INSERT INTO portrait_versions(weights,active,validated) VALUES(?,?,?)");
        insert.text(1, Json(weights).dump());
        insert.integer(2, result.activated ? 1 : 0);
        insert.integer(3, result.activated ? 1 : 0);
        insert.step();
        result.version = static_cast<int>(sqlite3_last_insert_rowid(mImplementation->database));
        mImplementation->execute("COMMIT");
    } catch (...) {
        mImplementation->execute("ROLLBACK");
        throw;
    }
    result.activeVersion = status().activeVersion;
    return result;
}
void MaiPortraitPersonalization::rollback(int version) {
    if (version < 0) throw std::runtime_error("Invalid portrait model version");
    if (version > 0) {
        Statement query(mImplementation->database, "SELECT 1 FROM portrait_versions WHERE version=? AND validated=1");
        query.integer(1, version);
        if (query.step() != SQLITE_ROW) throw std::runtime_error("Personal portrait model version was not found");
    }
    mImplementation->execute("BEGIN IMMEDIATE");
    try {
        mImplementation->execute("UPDATE portrait_versions SET active=0");
        if (version > 0) {
            Statement update(mImplementation->database, "UPDATE portrait_versions SET active=1 WHERE version=?");
            update.integer(1, version);
            update.step();
        }
        mImplementation->execute("COMMIT");
    } catch (...) {
        mImplementation->execute("ROLLBACK");
        throw;
    }
}
