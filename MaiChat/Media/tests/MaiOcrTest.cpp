// 文件作用：以普通调用和真实模型验证本地 OCR 契约、坐标、取消和识别。
#include "MaiOcr.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#endif

#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>

#include "MaiFilePath.h"
#include "MaiFileSystem.h"
#include "MaiOcrInternal.h"

namespace {
int failures = 0;
#define CHECK(condition)                                                    \
    do {                                                                    \
        if (!(condition)) {                                                 \
            std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #condition); \
            ++failures;                                                     \
        }                                                                   \
    } while (0)

std::string readBytes(const std::string& path) {
    std::string bytes;
    CHECK(!MaiFileSystem::readFile(MaiFilePath::fromUtf8(path), bytes, 64 * 1024 * 1024));
    return bytes;
}

MaiOcrImage imageView(const cv::Mat& rgba) {
    return {rgba.data, rgba.step * static_cast<size_t>(rgba.rows), rgba.cols, rgba.rows, static_cast<int>(rgba.step)};
}

void checkGeometry(const MaiOcrResult& result) {
    for (size_t i = 0; i < result.count; ++i) {
        CHECK(result.regions[i].text != nullptr);
        CHECK(std::isfinite(result.regions[i].confidence));
        CHECK(result.regions[i].confidence >= 0 && result.regions[i].confidence <= 1);
        for (const auto& point : result.regions[i].quad) {
            CHECK(std::isfinite(point.x) && std::isfinite(point.y));
            CHECK(point.x >= 0 && point.x <= result.width);
            CHECK(point.y >= 0 && point.y <= result.height);
        }
    }
}

std::string collectText(const MaiOcrResult& result) {
    std::string text;
    for (size_t i = 0; i < result.count; ++i) {
        if (result.regions[i].text) text += result.regions[i].text;
    }
    return text;
}

cv::Mat ownedFixture() {
    cv::Mat rgba(300, 720, CV_8UC4, cv::Scalar(255, 255, 255, 255));
#if defined(_WIN32)
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = rgba.cols;
    info.bmiHeader.biHeight = -rgba.rows;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* pixels = nullptr;
    HDC dc = CreateCompatibleDC(nullptr);
    HBITMAP bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
    CHECK(dc && bitmap && pixels);
    if (dc && bitmap && pixels) {
        HGDIOBJ oldBitmap = SelectObject(dc, bitmap);
        RECT background{0, 0, rgba.cols, rgba.rows};
        FillRect(dc, &background, static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));
        HFONT font = CreateFontW(-36, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                 CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH, L"Microsoft YaHei");
        HGDIOBJ oldFont = SelectObject(dc, font);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(24, 24, 24));
        const wchar_t* title = L"MaiChat OCR Test";
        const wchar_t* labels = L"\u53d1\u9001  \u53d6\u6d88  \u8bbe\u7f6e";
        const wchar_t* line = L"Local text recognition 2026";
        TextOutW(dc, 36, 30, title, static_cast<int>(wcslen(title)));
        TextOutW(dc, 36, 110, labels, static_cast<int>(wcslen(labels)));
        TextOutW(dc, 36, 190, line, static_cast<int>(wcslen(line)));
        GdiFlush();
        std::memcpy(rgba.data, pixels, rgba.total() * rgba.elemSize());
        for (int y = 0; y < rgba.rows; ++y)
            for (int x = 0; x < rgba.cols; ++x) rgba.at<cv::Vec4b>(y, x)[3] = 255;
        SelectObject(dc, oldFont);
        SelectObject(dc, oldBitmap);
        DeleteObject(font);
    }
    if (bitmap) DeleteObject(bitmap);
    if (dc) DeleteDC(dc);
#else
    cv::putText(rgba, "MaiChat OCR Test", {36, 70}, cv::FONT_HERSHEY_SIMPLEX, 1.2, {24, 24, 24, 255}, 2, cv::LINE_AA);
#endif
    return rgba;
}

#if defined(_WIN32)
struct MaiOcrSmallUiFixture {
    cv::Mat rgba;
    std::array<cv::Rect, 2> textInk;
};

MaiOcrSmallUiFixture ownedSmallUiFixture(int fontHeight) {
    MaiOcrSmallUiFixture fixture;
    fixture.rgba = cv::Mat(1080, 1920, CV_8UC4, cv::Scalar(247, 247, 247, 255));
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = fixture.rgba.cols;
    info.bmiHeader.biHeight = -fixture.rgba.rows;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* pixels = nullptr;
    HDC dc = CreateCompatibleDC(nullptr);
    HBITMAP bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
    CHECK(dc && bitmap && pixels);
    if (dc && bitmap && pixels) {
        HGDIOBJ oldBitmap = SelectObject(dc, bitmap);
        RECT background{0, 0, 1920, 1080};
        HBRUSH backgroundBrush = CreateSolidBrush(RGB(247, 247, 247));
        FillRect(dc, &background, backgroundBrush);
        DeleteObject(backgroundBrush);
        HFONT font =
            CreateFontW(-fontHeight, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                        CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH, L"Microsoft YaHei");
        HGDIOBJ oldFont = SelectObject(dc, font);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(24, 24, 24));
        const wchar_t* unrelated[] = {L"MaiChat Documents", L"Report.md", L"Holiday.jpg", L"Notes.txt", L"Save file"};
        for (int index = 0; index < 5; ++index) {
            const int x = index == 4 ? 1400 : 60;
            const int y = index == 4 ? 860 : 60 + index * 60;
            TextOutW(dc, x, y, unrelated[index], static_cast<int>(wcslen(unrelated[index])));
        }
        const wchar_t* labels[] = {L"\u65b0\u589e\u6587\u4ef6", L"\u8986\u76d6\u6587\u4ef6"};
        for (int index = 0; index < 2; ++index) {
            RECT button{1400 + index * 150, 920, 1533 + index * 150, 962};
            FillRect(dc, &button, static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));
            HBRUSH border = CreateSolidBrush(RGB(180, 180, 180));
            FrameRect(dc, &button, border);
            DeleteObject(border);
            SIZE extent{};
            GetTextExtentPoint32W(dc, labels[index], 4, &extent);
            const int x = button.left + (133 - extent.cx) / 2;
            const int y = button.top + (42 - extent.cy) / 2;
            TextOutW(dc, x, y, labels[index], 4);
        }
        GdiFlush();
        std::memcpy(fixture.rgba.data, pixels, fixture.rgba.total() * fixture.rgba.elemSize());
        for (int y = 0; y < fixture.rgba.rows; ++y)
            for (int x = 0; x < fixture.rgba.cols; ++x) fixture.rgba.at<cv::Vec4b>(y, x)[3] = 255;
        SelectObject(dc, oldFont);
        SelectObject(dc, oldBitmap);
        DeleteObject(font);
    }
    if (bitmap) DeleteObject(bitmap);
    if (dc) DeleteDC(dc);
    // 真值来自独立的 GDI 文字墨迹像素，既不读取 OCR 框，也不照抄它的坐标变换。
    for (int index = 0; index < 2; ++index) {
        std::vector<cv::Point> ink;
        for (int y = 926; y < 956; ++y)
            for (int x = 1406 + index * 150; x < 1527 + index * 150; ++x) {
                const auto value = fixture.rgba.at<cv::Vec4b>(y, x);
                if (value[0] < 80 && value[1] < 80 && value[2] < 80) ink.emplace_back(x, y);
            }
        CHECK(!ink.empty());
        if (!ink.empty()) fixture.textInk[index] = cv::boundingRect(ink);
    }
    return fixture;
}

void saveOwnedBitmap(const cv::Mat& rgba, const std::string& name) {
    const char* directory = std::getenv("MAI_OCR_TEST_ARTIFACT_DIR");
    if (!directory || !*directory) return;
    CHECK(!MaiFileSystem::createDirectories(MaiFilePath::fromUtf8(directory)));
    cv::Mat bgra;
    cv::cvtColor(rgba, bgra, cv::COLOR_RGBA2BGRA);
    BITMAPFILEHEADER header{};
    header.bfType = 0x4d42;
    header.bfOffBits = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);
    header.bfSize = header.bfOffBits + static_cast<DWORD>(bgra.total() * bgra.elemSize());
    BITMAPINFOHEADER info{};
    info.biSize = sizeof(info);
    info.biWidth = bgra.cols;
    info.biHeight = -bgra.rows;
    info.biPlanes = 1;
    info.biBitCount = 32;
    info.biCompression = BI_RGB;
    std::string bytes(header.bfSize, '\0');
    std::memcpy(bytes.data(), &header, sizeof(header));
    std::memcpy(bytes.data() + sizeof(header), &info, sizeof(info));
    std::memcpy(bytes.data() + header.bfOffBits, bgra.data, bgra.total() * bgra.elemSize());
    CHECK(!MaiFileSystem::writeFile(MaiFilePath::fromUtf8(std::string(directory) + "/" + name), bytes));
}
#endif
}  // namespace

int main(int argc, char** argv) {
    // 重复标签在 blank 之后必须重新输出，不能把合法双字母错误合并。
    const std::vector<std::string> symbols{"", "A", "B", " "};
    std::vector<float> probabilities{.05f, .9f, .03f, .02f, .05f, .9f,  .03f, .02f, .9f,  .05f, .03f, .02f,
                                     .05f, .9f, .03f, .02f, .05f, .03f, .9f,  .02f, .05f, .03f, .9f,  .02f};
    MaiOcrDecodedLine decoded;
    CHECK(maiOcrDecodeCtc(probabilities.data(), 6, 4, symbols, decoded) == MAI_OCR_OK);
    CHECK(decoded.text == "AAB");
    CHECK(std::abs(decoded.confidence - .9f) < .0001f);
    probabilities.back() = std::numeric_limits<float>::quiet_NaN();
    CHECK(maiOcrDecodeCtc(probabilities.data(), 6, 4, symbols, decoded) == MAI_OCR_MODEL_ERROR);
    CHECK(decoded.text.empty());
    CHECK(maiOcrDecodeCtc(probabilities.data(), 6, 3, symbols, decoded) == MAI_OCR_MODEL_ERROR);
    if (argc != 3) {
        std::puts("usage: mai_ocr_test <runtime-library> <model-directory>");
        return 1;
    }
    char error[512]{};
    MaiOcrEngine* engine = nullptr;
    CHECK(maiOcrCreate(nullptr, &engine, error, sizeof(error)) == MAI_OCR_INVALID_INPUT);
    CHECK(engine == nullptr && error[0] != '\0');
    const std::string directory = argv[2];
    const std::string detection = readBytes(directory + "/det.onnx");
    const std::string recognition = readBytes(directory + "/rec.onnx");
    const std::string dictionary = readBytes(directory + "/dictionary.txt");
    MaiOcrModelConfig config{argv[1],
                             reinterpret_cast<const unsigned char*>(detection.data()),
                             detection.size(),
                             reinterpret_cast<const unsigned char*>(recognition.data()),
                             recognition.size(),
                             dictionary.data(),
                             dictionary.size()};
    MaiOcrModelConfig bad = config;
    bad.runtime_path = "missing-ocr-runtime.dll";
    CHECK(maiOcrCreate(&bad, &engine, error, sizeof(error)) == MAI_OCR_NOT_CONFIGURED);
    CHECK(engine == nullptr);
    bad = config;
    bad.dictionary_utf8 = "\xc0\xaf\n";
    bad.dictionary_size = 3;
    CHECK(maiOcrCreate(&bad, &engine, error, sizeof(error)) == MAI_OCR_INVALID_INPUT);
    CHECK(engine == nullptr);
    bad = config;
    bad.detection_model = reinterpret_cast<const unsigned char*>("bad-model");
    bad.detection_model_size = 9;
    CHECK(maiOcrCreate(&bad, &engine, error, sizeof(error)) == MAI_OCR_MODEL_ERROR);
    CHECK(engine == nullptr);
    bad = config;
    const std::string incompleteDictionary = dictionary.substr(dictionary.find('\n') + 1);
    bad.dictionary_utf8 = incompleteDictionary.data();
    bad.dictionary_size = incompleteDictionary.size();
    CHECK(maiOcrCreate(&bad, &engine, error, sizeof(error)) == MAI_OCR_MODEL_ERROR);
    CHECK(engine == nullptr);
    const MaiOcrStatus created = maiOcrCreate(&config, &engine, error, sizeof(error));
    if (created != MAI_OCR_OK) std::printf("create: %d %s\n", created, error);
    CHECK(created == MAI_OCR_OK && engine != nullptr);
    if (!engine) return 1;

    MaiOcrResult result{};
    cv::Mat blank(240, 400, CV_8UC4, cv::Scalar(255, 255, 255, 255));
    MaiOcrImage view = imageView(blank);
    CHECK(maiOcrRecognize(engine, &view, nullptr, &result, error, sizeof(error)) == MAI_OCR_OK);
    CHECK(result.width == 400 && result.height == 240 && result.count == 0);
    maiOcrResultFree(&result);
    CHECK(result.regions == nullptr && result.count == 0 && result.width == 0);

    MaiOcrImage invalid = view;
    invalid.size = 4;
    CHECK(maiOcrRecognize(engine, &invalid, nullptr, &result, error, sizeof(error)) == MAI_OCR_INVALID_INPUT);
    CHECK(result.count == 0 && result.width == 0);
    invalid = view;
    invalid.width = std::numeric_limits<int>::max();
    CHECK(maiOcrRecognize(engine, &invalid, nullptr, &result, error, sizeof(error)) == MAI_OCR_LIMIT_EXCEEDED);
    MaiOcrOptions canceled{[](void*) { return 1; }, nullptr};
    CHECK(maiOcrRecognize(engine, &view, &canceled, &result, error, sizeof(error)) == MAI_OCR_CANCELED);
    CHECK(result.count == 0 && result.width == 0);

    const cv::Mat fixture = ownedFixture();
    for (int variant = 0; variant != 3; ++variant) {
        cv::Mat image;
        if (variant == 0) image = fixture;
        if (variant == 1) cv::resize(fixture, image, {}, 1.5, 1.5);
        if (variant == 2) cv::rotate(fixture, image, cv::ROTATE_90_CLOCKWISE);
        view = imageView(image);
        const MaiOcrStatus status = maiOcrRecognize(engine, &view, nullptr, &result, error, sizeof(error));
        if (status != MAI_OCR_OK) std::printf("recognize: %d %s\n", status, error);
        CHECK(status == MAI_OCR_OK);
        CHECK(result.width == image.cols && result.height == image.rows);
        checkGeometry(result);
        const std::string text = collectText(result);
        std::printf("variant %d: %zu regions, %s\n", variant, result.count, text.c_str());
        CHECK(text.find("MaiChat") != std::string::npos);
        bool titlePositionChecked = false;
        for (size_t i = 0; i < result.count; ++i) {
            if (std::string(result.regions[i].text).find("MaiChat") == std::string::npos) continue;
            float x = 0;
            float y = 0;
            for (const auto& point : result.regions[i].quad) {
                x += point.x / 4;
                y += point.y / 4;
            }
            if (variant == 1) {
                x /= 1.5f;
                y /= 1.5f;
            }
            if (variant == 2) {
                const float originalX = y;
                y = fixture.rows - x;
                x = originalX;
            }
            // 对照自有 fixture 的实际文字位置，能抓住误返回 1,024 检测坐标的回归。
            CHECK(x > 30 && x < 450 && y > 20 && y < 95);
            titlePositionChecked = true;
        }
        CHECK(titlePositionChecked);
#if defined(_WIN32)
        CHECK(text.find(u8"\u53d1\u9001") != std::string::npos);
        CHECK(text.find(u8"\u53d6\u6d88") != std::string::npos);
#endif
        maiOcrResultFree(&result);
    }
    view = imageView(fixture);
    int polls = 0;
    MaiOcrOptions midFlight{[](void* context) -> int { return ++*static_cast<int*>(context) >= 4; }, &polls};
    CHECK(maiOcrRecognize(engine, &view, &midFlight, &result, error, sizeof(error)) == MAI_OCR_CANCELED);
    CHECK(polls >= 4 && result.regions == nullptr && result.count == 0 && result.width == 0);
#if defined(_WIN32)
    const std::array<std::string, 2> labels{u8"\u65b0\u589e\u6587\u4ef6", u8"\u8986\u76d6\u6587\u4ef6"};
    for (const int fontHeight : {18, 20}) {
        const MaiOcrSmallUiFixture small = ownedSmallUiFixture(fontHeight);
        view = imageView(small.rgba);
        const MaiOcrStatus status = maiOcrRecognize(engine, &view, nullptr, &result, error, sizeof(error));
        if (status != MAI_OCR_OK) std::printf("small UI: %d %s\n", status, error);
        CHECK(status == MAI_OCR_OK && result.width == 1920 && result.height == 1080);
        checkGeometry(result);
        std::printf("1920x1080 font %d: %zu regions, %s\n", fontHeight, result.count, collectText(result).c_str());
        cv::Mat annotated = small.rgba.clone();
        std::array<bool, 2> matched{};
        for (size_t index = 0; index < result.count; ++index) {
            const MaiOcrRegion& region = result.regions[index];
            std::vector<cv::Point2f> polygon;
            std::vector<cv::Point> drawing;
            for (const auto& point : region.quad) {
                polygon.emplace_back(point.x, point.y);
                drawing.emplace_back(cvRound(point.x), cvRound(point.y));
            }
            cv::polylines(annotated, drawing, true, cv::Scalar(255, 0, 0, 255), 2);
            for (size_t label = 0; label < labels.size(); ++label) {
                if (labels[label] != region.text) continue;
                CHECK(!matched[label]);
                matched[label] = true;
                const cv::Rect& ink = small.textInk[label];
                const std::array<cv::Point2f, 4> corners{cv::Point2f(ink.x, ink.y),
                                                         cv::Point2f(ink.x + ink.width - 1, ink.y),
                                                         cv::Point2f(ink.x + ink.width - 1, ink.y + ink.height - 1),
                                                         cv::Point2f(ink.x, ink.y + ink.height - 1)};
                for (const auto& corner : corners) CHECK(cv::pointPolygonTest(polygon, corner, true) >= -1.5);
            }
        }
        CHECK(matched[0] && matched[1]);
        for (const auto& ink : small.textInk) cv::rectangle(annotated, ink, cv::Scalar(0, 180, 0, 255), 1);
        const std::string prefix = "ocr-small-ui-" + std::to_string(fontHeight);
        saveOwnedBitmap(small.rgba, prefix + ".bmp");
        saveOwnedBitmap(annotated, prefix + "-annotated.bmp");
        maiOcrResultFree(&result);
    }
#endif
    maiOcrDestroy(engine);
    maiOcrResultFree(nullptr);
    maiOcrDestroy(nullptr);
    std::printf("MaiOcrTest: %d failures\n", failures);
    return failures ? 1 : 0;
}
