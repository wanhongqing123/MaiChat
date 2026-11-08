// 文件作用：用普通 C 接口验证本地音视频时长和失败回执；不链接 Qt 或启动播放器。
#include "MaiMediaProbe.h"

#include <cstdint>
#include <cstdio>
#include <string>

#include "MaiFilePath.h"
#include "MaiFileSystem.h"
#include "MaiIdGenerator.h"

static int failures = 0;
#define CHECK(condition)                                                             \
    do {                                                                             \
        if (!(condition)) {                                                          \
            std::fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #condition); \
            ++failures;                                                              \
        }                                                                            \
    } while (false)

class MaiProbeTestDirectory {
public:
    MaiProbeTestDirectory()
        : path(MaiFileSystem::temporaryDirectory().append(
              MaiFilePath::fromUtf8(MaiIdGenerator::generate("mai-media-probe-")))) {
        CHECK(!MaiFileSystem::createDirectories(path));
    }
    ~MaiProbeTestDirectory() {
        MaiFileSystem::removeRecursively(path);
    }

    MaiFilePath path;
};

static void appendLittleEndian(std::string& bytes, std::uint32_t value, unsigned count) {
    for (unsigned index = 0; index < count; ++index) bytes.push_back(static_cast<char>((value >> (index * 8)) & 0xff));
}

static std::string silentWave() {
    constexpr std::uint32_t sampleRate = 8000;
    constexpr std::uint32_t dataBytes = sampleRate * 3 * 2;
    std::string bytes("RIFF");
    appendLittleEndian(bytes, 36 + dataBytes, 4);
    bytes += "WAVEfmt ";
    appendLittleEndian(bytes, 16, 4);
    appendLittleEndian(bytes, 1, 2);
    appendLittleEndian(bytes, 1, 2);
    appendLittleEndian(bytes, sampleRate, 4);
    appendLittleEndian(bytes, sampleRate * 2, 4);
    appendLittleEndian(bytes, 2, 2);
    appendLittleEndian(bytes, 16, 2);
    bytes += "data";
    appendLittleEndian(bytes, dataBytes, 4);
    bytes.append(dataBytes, '\0');
    return bytes;
}

static void checkAudio(const MaiFilePath& file) {
    const std::string before = silentWave();
    CHECK(!MaiFileSystem::writeFile(file, before));
    const auto result = maiMediaProbeFile(file.toUtf8().c_str());
    CHECK(result.error_code == 0);
    CHECK(result.has_audio == 1);
    CHECK(result.has_video == 0);
    CHECK(result.width == 0 && result.height == 0);
    CHECK(result.duration_us >= 2999000 && result.duration_us <= 3001000);
    std::string after;
    CHECK(!MaiFileSystem::readFile(file, after));
    CHECK(after == before);
}

static void checkFailure(const char* path) {
    const auto result = maiMediaProbeFile(path);
    CHECK(result.error_code < 0);
    CHECK(result.duration_us == 0);
    CHECK(result.has_audio == 0 && result.has_video == 0);
    CHECK(result.width == 0 && result.height == 0);
}

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: MaiMediaProbeTests path-to-ffplay-av-subtitles.mkv\n");
        return 2;
    }
    MaiProbeTestDirectory directory;
    checkAudio(directory.path.append(MaiFilePath::fromUtf8("silence.wav")));
    // 中文与空格路径不经过 ANSI 文件 API；代码字面量用转义以兼容所有编译器输入编码。
    checkAudio(directory.path.append(MaiFilePath::fromUtf8(u8"\u97f3\u9891 original.wav")));
    const auto video = maiMediaProbeFile(argv[1]);
    CHECK(video.error_code == 0);
    CHECK(video.has_audio == 1 && video.has_video == 1);
    CHECK(video.width > 0 && video.height > 0);
    CHECK(video.duration_us > 0);
    checkFailure(nullptr);
    checkFailure("");
    checkFailure(directory.path.append(MaiFilePath::fromUtf8("missing.wav")).toUtf8().c_str());
    const auto invalid = directory.path.append(MaiFilePath::fromUtf8("invalid.mp4"));
    CHECK(!MaiFileSystem::writeFile(invalid, "not a media container"));
    checkFailure(invalid.toUtf8().c_str());
    const auto truncated = directory.path.append(MaiFilePath::fromUtf8("truncated.wav"));
    CHECK(!MaiFileSystem::writeFile(truncated, silentWave().substr(0, 12)));
    checkFailure(truncated.toUtf8().c_str());
    if (!failures) std::printf("ordinary media probe tests passed\n");
    return failures ? 1 : 0;
}
