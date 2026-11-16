#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "MaiChatTools/DesktopComputerInputTypes.h"

// 模型协议只负责无损转换，范围与组合规则仍由普通 DTO 的统一校验执行。
// 接口不暴露 JSON 类型，普通调用者可直接构造相同的 events/options。
struct DesktopComputerInputBatchArguments {
    std::vector<DesktopInputEvent> events;
    DesktopInputOptions options;
};

std::string maiDesktopComputerInputSchema(const std::string& legacySchema);
MaiResult<DesktopComputerInputBatchArguments> maiDesktopParseInputBatch(const std::string& eventsJson,
                                                                        const std::string& optionsJson = {});
MaiResult<std::uint64_t> maiDesktopInputSequence(const std::string& decimal);
std::string maiDesktopInputCapabilitiesJson(const DesktopInputCapabilities& capabilities);
std::string maiDesktopInputFeedbackJson(const DesktopInputFeedbackBatch& batch);
