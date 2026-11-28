// 文件作用：验证桌面端内嵌 FFmpeg 工具的参数处理、执行和取消路径。
// 运行边界：由测试进程驱动实际组件或替身，不作为正式 App 的用户入口。
#include "MaiFfmpegTools.h"
#include "MaiVideoGeometryTool.h"

#include <atomic>
#include <algorithm>
#include <mutex>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

#include <json.hpp>

#include "MaiFilePath.h"
#include "MaiFileSystem.h"
#include "MaiIdGenerator.h"
#include "MaiThread.h"
#include "mai_fftools_embed.h"

extern "C" {
#include "../../../MaiAgent/third_party/ffmpeg/libavutil/log.h"
}

namespace {

int (*sFakeCancelCheck)(void *) = nullptr;
void *sFakeCancelOpaque = nullptr;
std::string sCapturedProbeOutput;
std::string sCapturedProbeInput;
std::string sCapturedConvertInput;
std::string sCapturedConvertOutput;

void setFakeCancelCheck(int (*check)(void *), void *opaque) {
  sFakeCancelCheck = check;
  sFakeCancelOpaque = opaque;
}

int runUntilCanceled(int, char **) {
  while (!sFakeCancelCheck || !sFakeCancelCheck(sFakeCancelOpaque))
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  return 255;
}

int captureProbePaths(int argc, char **argv) {
  for (int index = 0; index + 1 < argc; ++index)
    if (std::string(argv[index]) == "-o")
      sCapturedProbeOutput = argv[index + 1];
  sCapturedProbeInput = argv[argc - 1];
  return MaiFileSystem::writeFile(MaiFilePath::fromUtf8(sCapturedProbeOutput),
                                  R"({"streams":[{"codec_type":"video"}]})")
                 .hasError()
             ? 1
             : 0;
}

int captureConvertPaths(int argc, char **argv) {
  for (int index = 0; index + 1 < argc; ++index)
    if (std::string(argv[index]) == "-i")
      sCapturedConvertInput = argv[index + 1];
  sCapturedConvertOutput = argv[argc - 1];
  return 0;
}

MaiToolResult runFilterGraph(MaiTool &convert, const MaiToolContext &context,
                             const std::vector<std::string> &sources,
                             const std::string &graph) {
  std::vector<std::string> arguments;
  for (const std::string &source : sources)
    arguments.insert(arguments.end(), {"-f", "lavfi", "-i", source});
  arguments.insert(arguments.end(), {"-filter_complex", graph, "-map", "[out]",
                                     "-frames:v", "1", "-f", "null", "-"});
  return convert.execute(nlohmann::json{{"arguments", arguments}}.dump(),
                         context);
}

int helpFailures = 0;
void helpCheck(bool value, const char* label) {
    if (!value) {
        ++helpFailures;
        std::fprintf(stderr, "OWNED_HELP_FAIL: %s\n", label);
    }
}

MaiFfmpegEngine realHelpEngine() {
    return {mai_ffmpeg_execute,           mai_ffmpeg_set_cancel_check, mai_ffprobe_execute,
            mai_ffprobe_set_cancel_check, mai_fftools_set_log_sink,    mai_fftools_error_string,
            mai_fftools_set_text_sink};
}

nlohmann::json helpResult(MaiTool& tool, const std::vector<std::string>& args, const MaiToolContext& context) {
    const auto result = tool.execute(nlohmann::json{{"arguments", args}}.dump(), context);
    helpCheck(!result.hasError(), "information query has no tool error");
    const auto parsed = nlohmann::json::parse(result.output(), nullptr, false);
    helpCheck(parsed.is_object() && parsed.contains("text") && parsed["text"].is_string(),
              "automatic text body without capture_log");
    if (!parsed.is_object()) return nlohmann::json::object();
    helpCheck(parsed.value("text", "").size() <= 16 * 1024, "bounded information text");
    helpCheck(result.isTruncated() == parsed.value("text_truncated", false), "structured and outer truncation agree");
    return parsed;
}

int argumentVectorContract(int argc, char** argv) {
    // argc excludes the mandatory null slot, for both ordinary and tool callers.
    const bool ordinary = argc == 2;
    const bool model = argc == 5;
    if ((!ordinary && !model) || !argv || !argv[0] || !argv[argc - 1] || std::strcmp(argv[0], "ffmpeg") ||
        std::strcmp(argv[argc - 1], "-h") || argv[argc] != nullptr)
        return -44;
    mai_fftools_text_printf("OWNED_ARGV_CONTRACT_OK");
    return 0;
}

int largeFormatting(int, char**) {
    const std::string large(20000, 'x');
    mai_fftools_text_printf("OWNED_FORMAT_PREFIX:%s:OWNED_FORMAT_SUFFIX", large.c_str());
    return 0;
}
int utf8Formatting(int, char**) {
    // Repeated UTF-8 bytes cross the aggregate16KiB boundary. No source encoding dependency.
    const char* word = "\xe4\xb8\xad\xe6\x96\x87";
    for (int i = 0; i < 4000; ++i) mai_fftools_text_printf("%s", word);
    return 0;
}
int byteLengthFormatting(int, char**) {
    mai_fftools_text_printf("A%cB", 0);
    return 0;
}
int partialFailure(int, char**) {
    mai_fftools_text_printf("OWNED_PARTIAL_FAILURE");
    return -22;
}
std::atomic<bool>* helpCancellation = nullptr;
int partialCancellation(int, char**) {
    mai_fftools_text_printf("OWNED_PARTIAL_CANCEL");
    if (helpCancellation) helpCancellation->store(true);
    return 255;
}

// Only command help, synthesized output and generated lavfi conversion are used.
// Parent captures stdout/stderr separately to verify callback reset, without
// redirecting process-global file descriptors inside the App/library.
int runOwnedHelpChecks() {
    helpFailures = 0;
    auto engine = realHelpEngine();
    auto tool = makeMaiFfmpegTool(engine);
    MaiToolContext context;
    context.root = ".";
    const int originalLevel = av_log_get_level();
    av_log_set_level(AV_LOG_WARNING);
    const auto direct = maiRunFfmpegSerialized(engine, {"ffmpeg", "-hide_banner", "-h"}, nullptr);
    helpCheck(direct.exitStatus == 0 && !direct.canceled && !direct.textCaptureFailed,
              "ordinary API returns successful real help");
    helpCheck(
        direct.text.find("Getting help:") != std::string::npos && direct.text.find("-h type=name") != std::string::npos,
        "ordinary API contains original help heading and topic route");
    helpCheck(av_log_get_level() == AV_LOG_WARNING, "ordinary help restores caller log level");
    av_log(nullptr, AV_LOG_ERROR, "OWNED_AVLOG_STDERR_AFTER_HELP\n");
    mai_fftools_text_printf("OWNED_STDOUT_AFTER_CLEAR\n");

    auto contractEngine = engine;
    contractEngine.runFfmpeg = argumentVectorContract;
    const auto contract = maiRunFfmpegSerialized(contractEngine, {"ffmpeg", "-h"}, nullptr);
    helpCheck(contract.exitStatus == 0 && contract.text == "OWNED_ARGV_CONTRACT_OK",
              "ordinary argv is null-terminated and argc excludes its sentinel");
    auto contractTool = makeMaiFfmpegTool(contractEngine);
    const auto contractText = helpResult(*contractTool, {"-h"}, context);
    helpCheck(contractText.value("text", "") == "OWNED_ARGV_CONTRACT_OK",
              "model argv preserves injected defaults and null terminator contract");

    for (const std::string& flag : {"-h", "-?", "-help", "--help"}) {
        const auto result = helpResult(*tool, {flag}, context);
        helpCheck(
            result.value("status", -1) == 0 && result.value("text", "").find("Getting help:") != std::string::npos,
            "real help alias returns heading");
        helpCheck(av_log_get_level() == AV_LOG_WARNING, "tool help restores caller log level");
    }
    for (const int level : {AV_LOG_ERROR, AV_LOG_QUIET}) {
        av_log_set_level(level);
        for (const std::string& flag : {"-version", "-buildconf"}) {
            const auto result = helpResult(*tool, {flag}, context);
            helpCheck(!result.value("text", "").empty(), "version/buildconf body at ERROR/QUIET");
            helpCheck(av_log_get_level() == level, "version/buildconf restores low caller level");
        }
    }
    av_log_set_level(AV_LOG_WARNING);
    auto filter = helpResult(*tool, {"-h", "filter=scale"}, context);
    helpCheck(filter.value("text", "").find("Filter scale") != std::string::npos &&
                  filter.value("text", "").find("width") != std::string::npos,
              "filter topic unchanged and AVOption body captured despite prior warning level");
    auto encoder = helpResult(*tool, {"-h", "encoder=libx264"}, context);
    helpCheck(encoder.value("text", "").find("Encoder libx264") != std::string::npos,
              "encoder topic is not rewritten to an output path");
    const auto unknown = helpResult(*tool, {"-h", "filter=owned_filter_does_not_exist"}, context);
    const auto unknownDirect =
        maiRunFfmpegSerialized(engine, {"ffmpeg", "-hide_banner", "-h", "filter=owned_filter_does_not_exist"}, nullptr);
    helpCheck(unknown.value("status", -999) == unknownDirect.exitStatus &&
                  unknown.value("text", "") == unknownDirect.text &&
                  unknownDirect.text.find("owned_filter_does_not_exist") != std::string::npos,
              "unknown topic preserves upstream status and body without invented success fact");

    for (const std::string& flag :
         {"-L",        "-license", "-version",     "-buildconf",    "-formats", "-muxers",    "-demuxers",
          "-devices",  "-codecs",  "-decoders",    "-encoders",     "-bsfs",    "-protocols", "-filters",
          "-pix_fmts", "-layouts", "-sample_fmts", "-dispositions", "-colors",  "-hwaccels"}) {
        const auto result = helpResult(*tool, {flag}, context);
        helpCheck(result.value("status", -1) == 0 && !result.value("text", "").empty(),
                  "whitelisted native information option returns text");
    }
    const auto full = helpResult(*tool, {"-h", "full"}, context);
    helpCheck(full.value("text_truncated", false) && full.value("text", "").find("Getting help:") != std::string::npos,
              "full help is bounded prefix with real truncation");
    for (const std::vector<std::string>& args : {std::vector<std::string>{"-h", "filter=scale", "-i", "outside.mp4"},
                                                 {"-i", "outside.mp4", "-h"},
                                                 {"-h", "full", "output.mp4"},
                                                 {"-filters", "output.mp4"},
                                                 {"-h", "-report"},
                                                 {"-h", "../outside"},
                                                 {"-hide_banner", "-h", "full", "-y"}}) {
        const auto result = tool->execute(nlohmann::json{{"arguments", args}}.dump(), context);
        helpCheck(result.hasError() && result.error().code() == MaiErrorCode::InvalidInput,
                  "mixed/invalid information argv rejects before executing");
    }
    auto legacyEngine = engine;
    legacyEngine.setTextSink = nullptr;
    const auto unavailable = makeMaiFfmpegTool(legacyEngine)->execute(R"({"arguments":["-h"]})", context);
    helpCheck(unavailable.hasError() && unavailable.error().code() == MaiErrorCode::NotSupported,
              "old aggregate without text writer does not report empty help success");

    auto injected = engine;
    injected.runFfmpeg = largeFormatting;
    auto stress = makeMaiFfmpegTool(injected);
    const auto formatted = helpResult(*stress, {"-h"}, context);
    helpCheck(formatted.value("text_truncated", false) &&
                  formatted.value("text", "").find("OWNED_FORMAT_PREFIX:") == 0 &&
                  formatted.value("text", "").size() <= 4095,
              "formatter bounded before host buffer and reports formatter truncation");
    injected.runFfmpeg = utf8Formatting;
    stress = makeMaiFfmpegTool(injected);
    const auto unicode = helpResult(*stress, {"-h"}, context);
    helpCheck(unicode.value("text_truncated", false) && unicode.value("text", "").size() % 3 == 0,
              "aggregate prefix ends at complete UTF-8 code point");
    injected.runFfmpeg = byteLengthFormatting;
    stress = makeMaiFfmpegTool(injected);
    const auto counted = helpResult(*stress, {"-h"}, context);
    helpCheck(counted.value("text", "") == std::string("A\0B", 3), "text callback uses explicit length not strlen");
    injected.runFfmpeg = partialFailure;
    stress = makeMaiFfmpegTool(injected);
    const auto failed = stress->execute(R"({"arguments":["-h"]})", context);
    helpCheck(failed.hasError() && failed.error().message().find("OWNED_PARTIAL_FAILURE") != std::string::npos &&
                  failed.error().message().find("status -22") != std::string::npos,
              "failure retains captured text");
    std::atomic<bool> canceled{false};
    helpCancellation = &canceled;
    context.cancel = &canceled;
    injected.runFfmpeg = partialCancellation;
    stress = makeMaiFfmpegTool(injected);
    const auto stopped = stress->execute(R"({"arguments":["-h"]})", context);
    helpCheck(stopped.hasError() && stopped.error().code() == MaiErrorCode::Canceled &&
                  stopped.error().message().find("OWNED_PARTIAL_CANCEL") != std::string::npos &&
                  stopped.error().message().find("status 255") != std::string::npos,
              "cancellation retains partial captured text");
    helpCancellation = nullptr;
    context.cancel = nullptr;
    const auto after = helpResult(*tool, {"-h", "filter=scale"}, context);
    helpCheck(after.value("text", "").find("OWNED_PARTIAL") == std::string::npos,
              "failure/cancellation does not leak into next call");

    std::string concurrentFilter, concurrentEncoder;
    std::thread first([&] {
        const auto result = tool->execute(R"({"arguments":["-h","filter=scale"]})", context);
        concurrentFilter = result.output();
    });
    std::thread second([&] {
        const auto result = tool->execute(R"({"arguments":["-h","encoder=libx264"]})", context);
        concurrentEncoder = result.output();
    });
    std::thread unrelated([] { std::printf("OWNED_UNRELATED_STDOUT\n"); });
    first.join();
    second.join();
    unrelated.join();
    helpCheck(concurrentFilter.find("Filter scale") != std::string::npos &&
                  concurrentFilter.find("Encoder libx264") == std::string::npos &&
                  concurrentEncoder.find("Encoder libx264") != std::string::npos &&
                  concurrentEncoder.find("Filter scale") == std::string::npos &&
                  concurrentFilter.find("OWNED_UNRELATED_STDOUT") == std::string::npos &&
                  concurrentEncoder.find("OWNED_UNRELATED_STDOUT") == std::string::npos,
              "concurrent command text is isolated and unrelated stdout is not redirected");
    const auto converted = tool->execute(
        R"({"arguments":["-f","lavfi","-i","color=c=red:s=16x16:r=1","-frames:v","1","-f","null","-"],"capture_log":true})",
        context);
    helpCheck(!converted.hasError(), "help-failure-help-normal conversion survives");
    if (!converted.hasError()) {
        const auto logs = nlohmann::json::parse(converted.output(), nullptr, false);
        helpCheck(logs.is_object() && logs.contains("log_tail") && !logs.contains("text"),
                  "ordinary conversion capture_log output contract unchanged");
    }
    helpCheck(av_log_get_level() == AV_LOG_WARNING, "log level restored after full sequence");
    av_log(nullptr, AV_LOG_ERROR, "OWNED_AVLOG_STDERR_AT_END\n");
    av_log_set_level(originalLevel);
    std::printf("Owned embedded FFmpeg help: %d failures\n", helpFailures);
    return helpFailures ? 90 : 0;
}

} // namespace

// 使用本地媒体样本集中验证 FFmpeg 工具入口、参数限制和产物，不向付费服务提交任务。
int main(int argc, char **argv) {
    if (argc == 2 && std::string(argv[1]) == "--help-output-smoke") return runOwnedHelpChecks();
    if (argc != 2) return 2;
    const MaiFilePath input = MaiFilePath::fromUtf8(argv[1]);
    if (!MaiFileSystem::exists(input)) return 3;

    MaiFfmpegEngine engine{mai_ffmpeg_execute,           mai_ffmpeg_set_cancel_check, mai_ffprobe_execute,
                           mai_ffprobe_set_cancel_check, mai_fftools_set_log_sink,    mai_fftools_error_string,
                           mai_fftools_set_text_sink};
    auto probe = makeMaiFfprobeTool(engine);
    auto convert = makeMaiFfmpegTool(engine);
    MaiToolContext context;
    context.root = input.dirName().toUtf8();
    context.allowOutsideWorkingDirectory = true;
    context.fileAccessRoot = MaiFileSystem::temporaryDirectory().toUtf8();

    const MaiToolResult missing = probe->execute(R"({"path":"no-such-video.mp4"})", context);
    if (!missing.hasError()) return 4;
    const MaiToolResult invalidConversion =
        convert->execute(nlohmann::json{{"arguments", {"-i", "no-such-video.mp4", "-f", "null", "-"}}}.dump(), context);
    if (!invalidConversion.hasError()) return 9;
    for (int attempt = 0; attempt < 2; ++attempt) {
        const MaiToolResult metadata = probe->execute(nlohmann::json{{"path", input.toUtf8()}}.dump(), context);
        if (metadata.hasError()) {
            std::fprintf(stderr, "probe failed: %s\n", metadata.error().message().c_str());
            return 5;
        }
        const nlohmann::json parsed = nlohmann::json::parse(metadata.output(), nullptr, false);
        if (parsed.is_discarded() || !parsed.contains("streams") || !parsed["streams"].is_array() ||
            parsed["streams"].empty())
            return 6;
    }
  const MaiToolResult counted =
      probe->execute(nlohmann::json{{"path", input.toUtf8()},
                                    {"count_frames", true},
                                    {"include_chapters", true}}
                         .dump(),
                     context);
  if (counted.hasError()) {
    std::fprintf(stderr, "counted probe failed: %s\n",
                 counted.error().message().c_str());
    return 51;
  }
  const nlohmann::json countedJson =
      nlohmann::json::parse(counted.output(), nullptr, false);
  if (countedJson.is_discarded() || !countedJson.contains("streams"))
    return 52;
  const MaiToolResult keyframes =
      probe->execute(nlohmann::json{{"path", input.toUtf8()},
                                    {"mode", "keyframes"},
                                    {"start_s", 0},
                                    {"duration_s", 2}}
                         .dump(),
                     context);
  if (keyframes.hasError()) {
    std::fprintf(stderr, "keyframe probe failed: %s\n",
                 keyframes.error().message().c_str());
    return 53;
  }
  const nlohmann::json frameJson =
      nlohmann::json::parse(keyframes.output(), nullptr, false);
  if (frameJson.is_discarded() || !frameJson.contains("frames") ||
      !frameJson["frames"].is_array() || frameJson["frames"].empty())
    return 54;
  for (const char *mode : {"frames", "packets"}) {
    const MaiToolResult timing =
        probe->execute(nlohmann::json{{"path", input.toUtf8()},
                                      {"mode", mode},
                                      {"start_s", 0},
                                      {"duration_s", 1}}
                           .dump(),
                       context);
    if (timing.hasError()) {
      std::fprintf(stderr, "%s probe failed: %s\n", mode,
                   timing.error().message().c_str());
      return 57;
    }
    const auto parsedTiming =
        nlohmann::json::parse(timing.output(), nullptr, false);
    const char *list = std::string(mode) == "frames" ? "frames" : "packets";
    if (parsedTiming.is_discarded() || !parsedTiming.contains(list) ||
        !parsedTiming[list].is_array() || parsedTiming[list].empty())
      return 58;
  }
  if (!probe
           ->execute(R"({"path":"gallery.mp4","select_streams":"v:0;bad"})",
                     context)
           .hasError() ||
      !probe
           ->execute(
               R"({"path":"gallery.mp4","mode":"keyframes","duration_s":121})",
               context)
           .hasError())
    return 55;
  const MaiToolResult volumeAnalysis = convert->execute(
      nlohmann::json{
          {"arguments",
           {"-f", "lavfi", "-i", "sine=frequency=440:sample_rate=44100", "-t",
            "0.25", "-af", "volumedetect", "-f", "null", "-"}},
          {"capture_log", true}}
          .dump(),
      context);
  if (volumeAnalysis.hasError() ||
      nlohmann::json::parse(volumeAnalysis.output(), nullptr, false)
              .value("log_tail", std::string{})
              .find("mean_volume") == std::string::npos) {
    std::fprintf(stderr, "FFmpeg analysis log missing: %s\n",
                 volumeAnalysis.hasError()
                     ? volumeAnalysis.error().message().c_str()
                     : volumeAnalysis.output().c_str());
    return 56;
  }

  const MaiFilePath workspace = MaiFileSystem::temporaryDirectory().append(
      MaiFilePath::fromUtf8(MaiIdGenerator::generate("mai_media_workspace_")));
  if (MaiFileSystem::createDirectories(workspace).hasError())
    return 16;
  const MaiFilePath workspaceInput =
      workspace.append(MaiFilePath::fromUtf8("gallery.mp4"));
  if (MaiFileSystem::writeFile(workspaceInput, "media").hasError())
    return 17;
  MaiToolContext workspaceContext = context;
  workspaceContext.root = workspace.toUtf8();
  workspaceContext.fileAccessRoot = workspace.dirName().toUtf8();
  workspaceContext.allowOutsideWorkingDirectory = false;
  MaiFfmpegEngine pathEngine{captureConvertPaths, nullptr, captureProbePaths,
                             nullptr};
  const MaiToolResult workspaceProbe =
      makeMaiFfprobeTool(pathEngine)
          ->execute(R"({"path":"gallery.mp4"})", workspaceContext);
  const MaiToolResult workspaceConvert =
      makeMaiFfmpegTool(pathEngine)
          ->execute(
              R"({"arguments":["-i","gallery.mp4","-frames:v","1","frame.png"]})",
              workspaceContext);
  const std::string expectedInput =
      MaiFileSystem::resolve(workspaceInput).toUtf8();
  const std::string expectedOutput =
      MaiFileSystem::resolve(
          workspace.append(MaiFilePath::fromUtf8("frame.png")))
          .toUtf8();
  const std::string outputDirectory =
      MaiFileSystem::resolve(MaiFilePath::fromUtf8(sCapturedProbeOutput))
          .dirName()
          .toUtf8();
  const bool workspacePathsCorrect =
      !workspaceProbe.hasError() && !workspaceConvert.hasError() &&
      sCapturedProbeInput == expectedInput &&
      outputDirectory == MaiFileSystem::resolve(workspace).toUtf8() &&
      sCapturedConvertInput == expectedInput &&
      sCapturedConvertOutput == expectedOutput;
  MaiFileSystem::removeRecursively(workspace);
  if (!workspacePathsCorrect) {
    std::fprintf(
        stderr,
        "workspace probe error=%s convert error=%s\n"
        "probe input=%s output=%s\nconvert input=%s output=%s\n",
        workspaceProbe.hasError() ? workspaceProbe.error().message().c_str()
                                  : "none",
        workspaceConvert.hasError() ? workspaceConvert.error().message().c_str()
                                    : "none",
        sCapturedProbeInput.c_str(), sCapturedProbeOutput.c_str(),
        sCapturedConvertInput.c_str(), sCapturedConvertOutput.c_str());
    return 18;
  }

#ifdef _WIN32
  std::string fixtureBytes;
  if (MaiFileSystem::readFile(input, fixtureBytes).hasError())
    return 12;
  const MaiFilePath utf8Input =
      MaiFileSystem::temporaryDirectory().append(MaiFilePath::fromUtf8(
          MaiIdGenerator::generate("mai_") + "_\xE6\xB5\x8B\xE8\xAF\x95.mp4"));
  if (MaiFileSystem::writeFile(utf8Input, fixtureBytes).hasError())
    return 13;
  const MaiToolResult utf8Metadata = probe->execute(
      nlohmann::json{{"path", utf8Input.toUtf8()}}.dump(), context);
  if (utf8Metadata.hasError()) {
    std::fprintf(stderr, "UTF-8 path probe failed: %s\n",
                 utf8Metadata.error().message().c_str());
    MaiFileSystem::removeFile(utf8Input);
    return 14;
  }
  const MaiFilePath utf8Output =
      MaiFileSystem::temporaryDirectory().append(MaiFilePath::fromUtf8(
          MaiIdGenerator::generate("mai_") + "_\xE6\xB5\x8B\xE8\xAF\x95.ppm"));
  const nlohmann::json utf8Request = {
      {"arguments",
       {"-i", utf8Input.toUtf8(), "-frames:v", "1", "-f", "image2",
        utf8Output.toUtf8()}}};
  const MaiToolResult utf8Conversion =
      convert->execute(utf8Request.dump(), context);
  std::string utf8OutputBytes;
  const MaiError utf8ReadError =
      MaiFileSystem::readFile(utf8Output, utf8OutputBytes, 16);
  MaiFileSystem::removeFile(utf8Input);
  MaiFileSystem::removeFile(utf8Output);
  if (utf8Conversion.hasError() || utf8ReadError.hasError() ||
      utf8OutputBytes.compare(0, 2, "P6") != 0)
    return 15;
#endif

  for (int attempt = 0; attempt < 2; ++attempt) {
    const MaiFilePath output =
        MaiFileSystem::temporaryDirectory().append(MaiFilePath::fromUtf8(
            MaiIdGenerator::generate("mai_ffmpeg_test_") + ".ppm"));
    const nlohmann::json request = {{"arguments",
                                     {"-i", input.toUtf8(), "-frames:v", "1",
                                      "-f", "image2", output.toUtf8()}}};
    const MaiToolResult result = convert->execute(request.dump(), context);
    if (result.hasError()) {
      std::fprintf(stderr, "conversion failed: %s\n",
                   result.error().message().c_str());
      return 7;
    }
    std::string bytes;
    const MaiError readError = MaiFileSystem::readFile(output, bytes, 16);
    MaiFileSystem::removeFile(output);
    if (readError.hasError() || bytes.compare(0, 2, "P6") != 0)
      return 8;
  }

  const MaiFilePath av1Input =
      input.dirName().append(MaiFilePath::fromUtf8("tiny-red-av1.webm"));
  if (!MaiFileSystem::exists(av1Input))
    return 19;
  const MaiFilePath av1Output =
      MaiFileSystem::temporaryDirectory().append(MaiFilePath::fromUtf8(
          MaiIdGenerator::generate("mai_av1_test_") + ".ppm"));
  const MaiToolResult av1Result = convert->execute(
      nlohmann::json{{"arguments",
                      {"-i", av1Input.toUtf8(), "-frames:v", "1", "-f",
                       "image2", av1Output.toUtf8()}}}
          .dump(),
      context);
  std::string av1Bytes;
  const MaiError av1Read = MaiFileSystem::readFile(av1Output, av1Bytes, 16);
  MaiFileSystem::removeFile(av1Output);
  if (av1Result.hasError() || av1Read.hasError() ||
      av1Bytes.compare(0, 2, "P6") != 0) {
    std::fprintf(stderr, "AV1 software decode failed: %s\n",
                 av1Result.hasError() ? av1Result.error().message().c_str()
                                      : "no PPM output");
    return 20;
  }

  const MaiFilePath lavfiOutput =
      MaiFileSystem::temporaryDirectory().append(MaiFilePath::fromUtf8(
          MaiIdGenerator::generate("mai_lavfi_test_") + ".ppm"));
  const MaiToolResult lavfiResult = convert->execute(
      nlohmann::json{{"arguments",
                      {"-f", "lavfi", "-i", "testsrc=size=16x16:rate=1",
                       "-frames:v", "1", "-f", "image2", lavfiOutput.toUtf8()}}}
          .dump(),
      context);
  std::string lavfiBytes;
  const MaiError lavfiRead =
      MaiFileSystem::readFile(lavfiOutput, lavfiBytes, 16);
  MaiFileSystem::removeFile(lavfiOutput);
  if (lavfiResult.hasError() || lavfiRead.hasError() ||
      lavfiBytes.compare(0, 2, "P6") != 0) {
    std::fprintf(stderr, "lavfi source failed: %s\n",
                 lavfiResult.hasError() ? lavfiResult.error().message().c_str()
                                        : "no PPM output");
    return 21;
  }

  const std::vector<std::string> fourSources = {
      "color=c=red:s=64x64:d=0.1:r=1", "color=c=white:s=64x64:d=0.1:r=1",
      "color=c=gray:s=64x64:d=0.1:r=1", "color=c=blue:s=64x64:d=0.1:r=1"};
  const std::string maskedMergeGraph =
      "[0:v]format=rgb24[fg];[1:v]format=gray[pm];"
      "[2:v]format=gray[dm];[pm][dm]blend=all_mode=lighten[cm];"
      "[3:v]format=rgb24[bg];[bg][fg][cm]maskedmerge[out]";
  const MaiToolResult mixedFormats =
      runFilterGraph(*convert, context, fourSources, maskedMergeGraph);
  if (mixedFormats.hasError()) {
    std::fprintf(stderr, "mixed-format maskedmerge failed: %s\n",
                 mixedFormats.error().message().c_str());
    return 29;
  }
  const MaiToolResult missingFourthInput = runFilterGraph(
      *convert, context, {fourSources[0], fourSources[1], fourSources[2]},
      maskedMergeGraph);
  if (!missingFourthInput.hasError() ||
      missingFourthInput.error().message().find("Invalid file index 3") ==
          std::string::npos ||
      missingFourthInput.error().message().find("needs a fourth -i input") ==
          std::string::npos) {
    std::fprintf(stderr, "fourth-input diagnostic missing: %s\n",
                 missingFourthInput.hasError()
                     ? missingFourthInput.error().message().c_str()
                     : "no failure");
    return 30;
  }
  const MaiToolResult extraOutputLabel = runFilterGraph(
      *convert, context, {fourSources[0], fourSources[1], fourSources[2]},
      "[0:v][1:v][2:v]maskedmerge[out][extra]");
  if (!extraOutputLabel.hasError() ||
      extraOutputLabel.error().message().find(
          "More output link labels specified") == std::string::npos ||
      extraOutputLabel.error().message().find(
          "Pixel formats are negotiated later") == std::string::npos ||
      extraOutputLabel.error().message().find("filter_complex as received") ==
          std::string::npos) {
    std::fprintf(stderr, "maskedmerge parser diagnostic missing: %s\n",
                 extraOutputLabel.hasError()
                     ? extraOutputLabel.error().message().c_str()
                     : "no failure");
    return 31;
  }
  const MaiToolResult nestedExpression = runFilterGraph(
      *convert, context, {"testsrc2=s=128x128:d=0.1:r=1"},
      R"([0:v]format=rgb24,split[t1][t2];[t1]boxblur=8[norm];[t2][norm]blend=all_mode=difference,)"
          R"(geq=lum='255-100*pow(p(X\,Y)/255\,2)'[out])");
  if (nestedExpression.hasError()) {
    std::fprintf(stderr, "nested geq expression failed: %s\n",
                 nestedExpression.error().message().c_str());
    return 32;
  }
  const MaiFilePath unsupportedOutput =
      MaiFileSystem::temporaryDirectory().append(MaiFilePath::fromUtf8(
          MaiIdGenerator::generate("mai_encoder_test_") + ".mp4"));
  const MaiToolResult unsupportedEncoder = convert->execute(
      nlohmann::json{{"arguments",
                      {"-i", input.toUtf8(), "-c:v", "maichat_missing_encoder",
                       unsupportedOutput.toUtf8()}}}
          .dump(),
      context);
  MaiFileSystem::removeFile(unsupportedOutput);
  if (!unsupportedEncoder.hasError() ||
      unsupportedEncoder.error().message().find("Encoder not found") ==
          std::string::npos) {
    std::fprintf(stderr, "encoder diagnostic missing: %s\n",
                 unsupportedEncoder.hasError()
                     ? unsupportedEncoder.error().message().c_str()
                     : "no failure");
    return 22;
  }

  const MaiFilePath x264Output =
      MaiFileSystem::temporaryDirectory().append(MaiFilePath::fromUtf8(
          MaiIdGenerator::generate("mai_x264_test_") + ".mp4"));
  const MaiToolResult x264Encode = convert->execute(
      nlohmann::json{{"arguments",
                      {"-i", input.toUtf8(), "-an", "-vf",
                       "hqdn3d=1.5:1.5:6:6,eq=contrast=1.05:brightness=0.02,"
                       "unsharp=3:3:0.5",
                       "-c:v", "libx264", "-preset", "ultrafast", "-crf", "28",
                       x264Output.toUtf8()}}}
          .dump(),
      context);
  const MaiToolResult x264Metadata =
      x264Encode.hasError()
          ? x264Encode
          : probe->execute(nlohmann::json{{"path", x264Output.toUtf8()}}.dump(),
                           context);
  MaiFileSystem::removeFile(x264Output);
  if (x264Metadata.hasError() ||
      x264Metadata.output().find("\"codec_name\": \"h264\"") ==
          std::string::npos) {
    std::fprintf(stderr, "libx264/filter encode failed: %s\n",
                 x264Metadata.hasError()
                     ? x264Metadata.error().message().c_str()
                     : x264Metadata.output().c_str());
    return 24;
  }

  const MaiFilePath stretchedOutput =
      MaiFileSystem::temporaryDirectory().append(MaiFilePath::fromUtf8(
          MaiIdGenerator::generate("mai_stretch_test_") + ".mp4"));
  MaiStretchLowerOptions stretchOptions;
  stretchOptions.inputPath = input.toUtf8();
  stretchOptions.outputPath = stretchedOutput.toUtf8();
  stretchOptions.splitY = 32;
  stretchOptions.factor = 1.2;
  const auto stretched = maiStretchLowerVideo(stretchOptions, engine, context);
  const MaiToolResult stretchedMetadata =
      stretched ? probe->execute(
                      nlohmann::json{{"path", stretchedOutput.toUtf8()}}.dump(),
                      context)
                : MaiToolResult::failure(stretched.error().code(),
                                         stretched.error().message());
  MaiFileSystem::removeFile(stretchedOutput);
  if (!stretched || stretched.value().outputHeight != 70 ||
      stretchedMetadata.hasError() ||
      stretchedMetadata.output().find("\"height\": 70") == std::string::npos ||
      stretchedMetadata.output().find("\"color_primaries\": \"bt709\"") ==
          std::string::npos) {
    std::fprintf(stderr, "lower-region stretch failed: %s\n",
                 stretchedMetadata.hasError()
                     ? stretchedMetadata.error().message().c_str()
                     : stretchedMetadata.output().c_str());
    return 61;
  }

  const MaiFilePath mp3Output =
      MaiFileSystem::temporaryDirectory().append(MaiFilePath::fromUtf8(
          MaiIdGenerator::generate("mai_lame_test_") + ".mp3"));
  const MaiToolResult mp3Encode = convert->execute(
      nlohmann::json{
          {"arguments",
           {"-f", "lavfi", "-i", "sine=frequency=440:sample_rate=44100", "-t",
            "1", "-c:a", "libmp3lame", mp3Output.toUtf8()}}}
          .dump(),
      context);
  const MaiToolResult mp3Metadata =
      mp3Encode.hasError()
          ? mp3Encode
          : probe->execute(nlohmann::json{{"path", mp3Output.toUtf8()}}.dump(),
                           context);
  MaiFileSystem::removeFile(mp3Output);
  if (mp3Metadata.hasError() ||
      mp3Metadata.output().find("\"codec_name\": \"mp3\"") ==
          std::string::npos) {
    std::fprintf(stderr, "libmp3lame encode failed: %s\n",
                 mp3Metadata.hasError() ? mp3Metadata.error().message().c_str()
                                        : mp3Metadata.output().c_str());
    return 25;
  }

  const MaiFilePath previewWorkspace =
      MaiFileSystem::temporaryDirectory().append(
          MaiFilePath::fromUtf8(MaiIdGenerator::generate("mai_preview_test_")));
  if (MaiFileSystem::createDirectories(previewWorkspace).hasError())
    return 26;
  MaiToolContext previewContext = context;
  previewContext.root = previewWorkspace.toUtf8();
  previewContext.allowOutsideWorkingDirectory = false;
  const MaiFilePath largeImage =
      previewWorkspace.append(MaiFilePath::fromUtf8("large.png"));
  const MaiToolResult generatedImage = convert->execute(
      nlohmann::json{{"arguments",
                      {"-f", "lavfi", "-i", "testsrc=size=1920x1080:rate=1",
                       "-frames:v", "1", largeImage.toUtf8()}}}
          .dump(),
      previewContext);
  if (generatedImage.hasError()) {
    std::fprintf(stderr, "image fixture generation failed: %s\n",
                 generatedImage.error().message().c_str());
    MaiFileSystem::removeRecursively(previewWorkspace);
    return 27;
  }
  const MaiToolResult preview =
      makeMaiViewImageTool(makeMaiFfmpegImagePreview(engine))
          ->execute(R"({"path":"large.png"})", previewContext);
  bool previewValid = !preview.hasError() && preview.images().size() == 1;
  if (previewValid) {
    const auto &image = preview.images().front();
    std::uint64_t previewBytes = 0;
    previewValid = image.mimeType == "image/jpeg" &&
                   image.path != largeImage.toUtf8() &&
                   MaiFileSystem::fileSize(MaiFilePath::fromUtf8(image.path),
                                           previewBytes) &&
                   previewBytes > 0 && previewBytes <= 2u * 1024u * 1024u;
    if (previewValid) {
      const MaiToolResult dimensions = probe->execute(
          nlohmann::json{{"path", image.path}}.dump(), previewContext);
      const nlohmann::json metadata = nlohmann::json::parse(
          dimensions.hasError() ? "" : dimensions.output(), nullptr, false);
      previewValid = !metadata.is_discarded() && metadata.contains("streams") &&
                     !metadata["streams"].empty() &&
                     metadata["streams"][0].value("width", 0) <= 1280 &&
                     metadata["streams"][0].value("height", 0) <= 1280;
    }
  }
  MaiFileSystem::removeRecursively(previewWorkspace);
  if (!previewValid) {
    std::fprintf(stderr, "bounded image preview failed: %s\n",
                 preview.hasError() ? preview.error().message().c_str()
                                    : "invalid preview");
    return 28;
  }

#if defined(__APPLE__) && !TARGET_OS_SIMULATOR
  const MaiFilePath hardwareOutput =
      MaiFileSystem::temporaryDirectory().append(MaiFilePath::fromUtf8(
          MaiIdGenerator::generate("mai_videotoolbox_test_") + ".mp4"));
  const MaiToolResult hardwareEncode = convert->execute(
      nlohmann::json{{"arguments",
                      {"-i", input.toUtf8(), "-an", "-c:v", "h264_videotoolbox",
                       "-b:v", "200k", hardwareOutput.toUtf8()}}}
          .dump(),
      context);
  const MaiToolResult hardwareMetadata =
      hardwareEncode.hasError()
          ? hardwareEncode
          : probe->execute(
                nlohmann::json{{"path", hardwareOutput.toUtf8()}}.dump(),
                context);
  MaiFileSystem::removeFile(hardwareOutput);
  if (hardwareMetadata.hasError() ||
      hardwareMetadata.output().find("\"codec_name\": \"h264\"") ==
          std::string::npos) {
    std::fprintf(stderr, "VideoToolbox encode failed: %s\n",
                 hardwareMetadata.hasError()
                     ? hardwareMetadata.error().message().c_str()
                     : hardwareMetadata.output().c_str());
    return 23;
  }
#endif

  std::atomic<bool> canceled{false};
  // The cancellation fixture uses -version as its dummy command; that is now
  // an information query and must explicitly provide the text capability.
  MaiFfmpegEngine fakeEngine{runUntilCanceled, setFakeCancelCheck,       nullptr, nullptr, nullptr,
                             nullptr,          mai_fftools_set_text_sink};
  auto fakeTool = makeMaiFfmpegTool(fakeEngine);
  context.cancel = &canceled;
  std::thread requestCancel([&] {
    MaiThread::setCurrentName("mai-test-cancel");
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    canceled.store(true, std::memory_order_release);
  });
  const MaiToolResult canceledResult =
      fakeTool->execute(R"({"arguments":["-version"]})", context);
  requestCancel.join();
  if (!canceledResult.hasError() ||
      canceledResult.error().code() != MaiErrorCode::Canceled)
    return 10;

  canceled.store(false, std::memory_order_release);
  std::thread cancelRealRun([&] {
    MaiThread::setCurrentName("mai-test-cancel");
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    canceled.store(true, std::memory_order_release);
  });
  const nlohmann::json longRun = {
      {"arguments",
       {"-re", "-stream_loop", "-1", "-i", input.toUtf8(), "-t", "10", "-f",
        "null", "-"}}};
  const MaiToolResult interrupted = convert->execute(longRun.dump(), context);
  cancelRealRun.join();
  if (!interrupted.hasError() ||
      interrupted.error().code() != MaiErrorCode::Canceled)
    return 11;
  return 0;
}
