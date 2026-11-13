// 文件作用：在原始字节位置上流式解码文本，避免把文件前缀上限误报为真实 EOF。
#include "MaiTextFileReader.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <limits>
#include <string_view>
#include <utility>

#if defined(_WIN32)
#include <windows.h>
#elif !defined(__ANDROID__)
#include <iconv.h>
#endif
#if defined(__APPLE__)
#include <CoreFoundation/CFString.h>
#include <CoreFoundation/CFStringEncodingExt.h>
#endif

namespace {

constexpr std::size_t kBufferBytes = 64 * 1024;

bool sameSnapshot(const MaiRegularFileInfo& left, const MaiRegularFileInfo& right) {
    return left.bytes == right.bytes && left.modified == right.modified &&
           left.identity == right.identity;
}

MaiError changedSource() {
    return {MaiErrorCode::InvalidInput,
            "file changed during or since the previous read; restart without the cursor"};
}

bool isCanceled(const MaiTextFileReadOptions& options) {
    return options.isCanceled && options.isCanceled();
}

std::string normalizeEncoding(const std::string& name) {
    std::string encoding = name.empty() ? "auto" : name;
    std::transform(encoding.begin(), encoding.end(), encoding.begin(), [](unsigned char character) {
        return character == '_' ? '-' : static_cast<char>(std::tolower(character));
    });
    if (encoding == "utf8") return "utf-8";
    if (encoding == "utf16le") return "utf-16le";
    if (encoding == "utf16be") return "utf-16be";
    if (encoding == "cp936") return "gbk";
    if (encoding == "iso-8859-1") return "latin1";
    if (encoding == "shift-jis-2004") return "shift-jis";
    return encoding;
}

std::string systemEncoding() {
#if defined(_WIN32)
    switch (::GetACP()) {
        case CP_UTF8: return "utf-8";
        case 936: return "gbk";
        case 54936: return "gb18030";
        case 932: return "shift-jis";
        case 1252: return "windows-1252";
        case 28591: return "latin1";
        default: return {};
    }
#else
    // POSIX 的进程 locale 不保证无状态，也不保证能从任意游标位置独立解码。
    // 不把它猜成另一种编码；调用方需明确给出支持的源编码。
    return {};
#endif
}

bool isSupportedEncoding(const std::string& encoding) {
    return encoding == "utf-8" || encoding == "utf-16le" || encoding == "utf-16be" ||
           encoding == "gb18030" || encoding == "gbk" || encoding == "shift-jis" ||
           encoding == "windows-1252" || encoding == "latin1";
}

void appendUtf8(std::string& output, std::uint32_t codePoint) {
    if (codePoint <= 0x7f) {
        output.push_back(static_cast<char>(codePoint));
    } else if (codePoint <= 0x7ff) {
        output.push_back(static_cast<char>(0xc0 | (codePoint >> 6)));
        output.push_back(static_cast<char>(0x80 | (codePoint & 0x3f)));
    } else if (codePoint <= 0xffff) {
        output.push_back(static_cast<char>(0xe0 | (codePoint >> 12)));
        output.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3f)));
        output.push_back(static_cast<char>(0x80 | (codePoint & 0x3f)));
    } else {
        output.push_back(static_cast<char>(0xf0 | (codePoint >> 18)));
        output.push_back(static_cast<char>(0x80 | ((codePoint >> 12) & 0x3f)));
        output.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3f)));
        output.push_back(static_cast<char>(0x80 | (codePoint & 0x3f)));
    }
}

// 返回首个 UTF-8 字符宽度；前缀不足与非法编码分别返回 0 和 -1。
// auto 探测只允许最后一个字符不足，实际读取直到真实 EOF 时仍严格报错。
int utf8Width(const std::string& bytes, std::size_t offset) {
    const auto first = static_cast<unsigned char>(bytes[offset]);
    int width = 0;
    if (first <= 0x7f)
        width = 1;
    else if (first >= 0xc2 && first <= 0xdf)
        width = 2;
    else if (first >= 0xe0 && first <= 0xef)
        width = 3;
    else if (first >= 0xf0 && first <= 0xf4)
        width = 4;
    else
        return -1;
    if (bytes.size() - offset < static_cast<std::size_t>(width)) return 0;
    for (int index = 1; index < width; ++index) {
        const auto next = static_cast<unsigned char>(bytes[offset + index]);
        if (next < 0x80 || next > 0xbf) return -1;
    }
    if (width >= 3) {
        const auto second = static_cast<unsigned char>(bytes[offset + 1]);
        if ((first == 0xe0 && second < 0xa0) || (first == 0xed && second > 0x9f) ||
            (first == 0xf0 && second < 0x90) || (first == 0xf4 && second > 0x8f))
            return -1;
    }
    return width;
}

bool isUtf8Prefix(const std::string& bytes, bool sourceEnds) {
    for (std::size_t offset = 0; offset < bytes.size();) {
        const int width = utf8Width(bytes, offset);
        if (width == 0) return !sourceEnds;
        if (width < 0) return false;
        offset += static_cast<std::size_t>(width);
    }
    return true;
}

bool isBinaryCharacter(const std::string& character) {
    if (character.size() != 1) return false;
    const auto byte = static_cast<unsigned char>(character.front());
    return byte < 0x09 || (byte > 0x0d && byte < 0x20);
}

#if !defined(__ANDROID__)
bool hasInvalidGb18030Range(const std::string& bytes) {
    for (std::size_t position = 0; position < bytes.size();) {
        const auto first = static_cast<unsigned char>(bytes[position]);
        if (first < 0x81 || first > 0xfe) {
            ++position;
            continue;
        }
        if (bytes.size() - position < 2) return true;
        const auto second = static_cast<unsigned char>(bytes[position + 1]);
        if (second >= 0x30 && second <= 0x39) {
            if (bytes.size() - position < 4) return true;
            const auto third = static_cast<unsigned char>(bytes[position + 2]);
            const auto fourth = static_cast<unsigned char>(bytes[position + 3]);
            if (third < 0x81 || third > 0xfe || fourth < 0x30 || fourth > 0x39) return true;
            const std::uint32_t pointer =
                (((first - 0x81) * 10 + (second - 0x30)) * 126 + (third - 0x81)) * 10 +
                (fourth - 0x30);
            // WHATWG Encoding 的 GB18030 ranges code point 明确排除两个无标量区间：
            // https://encoding.spec.whatwg.org/#index-gb18030-ranges-code-point
            // 部分原生转换器仍接受 >U+10FFFF 的四字节输入，因此不能只依赖其成功返回值。
            if ((pointer > 39419 && pointer < 189000) || pointer > 1237575) return true;
            position += 4;
        } else {
            position += 2;
        }
    }
    return false;
}
#endif

#if defined(__APPLE__)
MaiResult<std::string> decodeAppleGb18030(const std::string& bytes) {
    // 部分 macOS libiconv 拒绝合法的 GB18030 非 BMP 序列（如 90 30 81 30）。
    // 仅在其失败后使用公开的 CoreFoundation 转换器；完整往返验证拒绝任何替代或丢失字节。
    const CFStringRef text = ::CFStringCreateWithBytes(
        kCFAllocatorDefault, reinterpret_cast<const UInt8*>(bytes.data()),
        static_cast<CFIndex>(bytes.size()), kCFStringEncodingGB_18030_2000, false);
    if (!text) return {MaiErrorCode::InvalidInput, "invalid source character in gb18030"};
    const CFIndex length = ::CFStringGetLength(text);
    const CFRange range = CFRangeMake(0, length);
    std::string utf8(bytes.size() * 4, '\0');
    CFIndex written = 0;
    const CFIndex converted = ::CFStringGetBytes(text, range, kCFStringEncodingUTF8, 0, false,
                                                 reinterpret_cast<UInt8*>(utf8.data()),
                                                 static_cast<CFIndex>(utf8.size()), &written);
    if (converted != length) {
        ::CFRelease(text);
        return {MaiErrorCode::InvalidInput, "GB18030 character could not be converted to UTF-8"};
    }
    std::string roundTrip(bytes.size(), '\0');
    CFIndex roundTripBytes = 0;
    const CFIndex restored =
        ::CFStringGetBytes(text, range, kCFStringEncodingGB_18030_2000, 0, false,
                           reinterpret_cast<UInt8*>(roundTrip.data()),
                           static_cast<CFIndex>(roundTrip.size()), &roundTripBytes);
    ::CFRelease(text);
    if (restored != length || roundTripBytes != static_cast<CFIndex>(bytes.size()) ||
        roundTrip != bytes)
        return {MaiErrorCode::InvalidInput, "GB18030 source could not be decoded without loss"};
    utf8.resize(static_cast<std::size_t>(written));
    return utf8;
}
#endif

MaiResult<std::string> decodeLegacy(
    const std::string& bytes, const std::string& encoding,
    const std::function<MaiResult<std::string>(const std::string&, const std::string&)>&
        decodeText) {
    if (decodeText) return decodeText(bytes, encoding);
#if defined(_WIN32)
    UINT codePage = 0;
    if (encoding == "auto" || encoding == "system")
        codePage = CP_ACP;
    else if (encoding == "gb18030")
        codePage = 54936;
    else if (encoding == "gbk" || encoding == "cp936")
        codePage = 936;
    else if (encoding == "shift-jis" || encoding == "shift-jis-2004")
        codePage = 932;
    else if (encoding == "windows-1252")
        codePage = 1252;
    else if (encoding == "latin1" || encoding == "iso-8859-1")
        codePage = 28591;
    else
        return {MaiErrorCode::NotSupported, "unsupported source encoding: " + encoding};
    if (codePage == 54936 && hasInvalidGb18030Range(bytes))
        return {MaiErrorCode::InvalidInput, "invalid source character in gb18030"};
    if (bytes.empty()) return std::string{};
    std::wstring wide(bytes.size(), L'\0');
    const int count = ::MultiByteToWideChar(codePage, MB_ERR_INVALID_CHARS, bytes.data(),
                                            static_cast<int>(bytes.size()), wide.data(),
                                            static_cast<int>(wide.size()));
    if (count <= 0) return {MaiErrorCode::InvalidInput, "invalid source character in " + encoding};
    std::string utf8(bytes.size() * 4, '\0');
    const int length =
        ::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(), count, utf8.data(),
                              static_cast<int>(utf8.size()), nullptr, nullptr);
    if (length <= 0) return {MaiErrorCode::InvalidInput, "source character conversion failed"};
    utf8.resize(static_cast<std::size_t>(length));
    return utf8;
#elif !defined(__ANDROID__)
    const std::string source = encoding == "auto" ? "gb18030" : encoding;
    if (source == "gb18030" && hasInvalidGb18030Range(bytes))
        return {MaiErrorCode::InvalidInput, "invalid source character in gb18030"};
    iconv_t converter = ::iconv_open("UTF-8", source.c_str());
    if (converter == reinterpret_cast<iconv_t>(-1)) {
#if defined(__APPLE__)
        if (source == "gb18030") return decodeAppleGb18030(bytes);
#endif
        return {MaiErrorCode::NotSupported, "unsupported source encoding: " + encoding};
    }
    char* input = const_cast<char*>(bytes.data());
    std::size_t remaining = bytes.size();
    std::string utf8(bytes.size() * 4, '\0');
    char* output = utf8.data();
    std::size_t capacity = utf8.size();
    const auto converted = ::iconv(converter, &input, &remaining, &output, &capacity);
    ::iconv_close(converter);
    if (converted == static_cast<std::size_t>(-1) || remaining != 0) {
#if defined(__APPLE__)
        if (source == "gb18030") return decodeAppleGb18030(bytes);
#endif
        return {MaiErrorCode::InvalidInput, "invalid source character in " + encoding};
    }
    utf8.resize(utf8.size() - capacity);
    return utf8;
#else
    return {MaiErrorCode::NotConfigured, "this Android host has no legacy text decoder configured"};
#endif
}

struct MaiDecodedFileCharacter {
    std::string text;
    std::size_t sourceBytes = 0;
};

// 与 Chromium File::Read(offset, ...) 相同，缓冲位置独立于文件句柄当前偏移。
// 每块从 MaiFileSystem 取得，不使用 C 运行时窄字符路径或全文件内存映射。
class MaiTextFileBuffer {
public:
    MaiTextFileBuffer(MaiFileReadHandle& handle, const MaiTextFileReadOptions& options);
    MaiResult<std::string> prefix();
    MaiResult<MaiDecodedFileCharacter> character(std::uint64_t offset, const std::string& encoding);
    bool isEofAt(std::uint64_t offset) const;
    MaiResult<std::string_view> utf8Span(std::uint64_t offset, std::size_t maxBytes);

private:
    MaiError ensure(std::uint64_t offset);
    MaiError prepareLegacy(std::size_t index, const std::string& encoding);
    MaiFileReadHandle& mHandle;
    const MaiTextFileReadOptions& mOptions;
    std::optional<std::uint64_t> mEofOffset;
    bool mBufferEof = false;
    std::uint64_t mBufferOffset = 0;
    std::string mBytes;
    std::string mLegacyText;
    std::vector<unsigned char> mLegacySourceWidths;
    std::vector<std::uint32_t> mLegacyUtf8Offsets;
};

MaiTextFileBuffer::MaiTextFileBuffer(MaiFileReadHandle& handle,
                                     const MaiTextFileReadOptions& options)
    : mHandle(handle), mOptions(options) {}

bool MaiTextFileBuffer::isEofAt(std::uint64_t offset) const {
    return mEofOffset && offset >= *mEofOffset;
}

MaiError MaiTextFileBuffer::ensure(std::uint64_t offset) {
    if (isCanceled(mOptions)) return {MaiErrorCode::Canceled, "file read canceled"};
    if (isEofAt(offset)) return {};
    if (offset >= mBufferOffset && offset - mBufferOffset < mLegacySourceWidths.size() &&
        mLegacySourceWidths[static_cast<std::size_t>(offset - mBufferOffset)] != 0)
        return {};
    if (!mBytes.empty() && offset >= mBufferOffset && offset - mBufferOffset < mBytes.size() &&
        (mBytes.size() - static_cast<std::size_t>(offset - mBufferOffset) >= 4 || mBufferEof))
        return {};
    mBufferOffset = offset;
    mLegacyText.clear();
    mLegacySourceWidths.clear();
    mLegacyUtf8Offsets.clear();
    const auto error = mHandle.readAt(offset, kBufferBytes, mBytes, &mBufferEof);
    if (error) return error;
    if (mBufferEof) mEofOffset = offset + mBytes.size();
    return {};
}

MaiResult<std::string> MaiTextFileBuffer::prefix() {
    const auto error = ensure(0);
    if (error) return error;
    return mBytes;
}

MaiResult<std::string_view> MaiTextFileBuffer::utf8Span(std::uint64_t offset,
                                                        std::size_t maxBytes) {
    const auto error = ensure(offset);
    if (error) return error;
    if (isEofAt(offset) || maxBytes == 0) return std::string_view{};
    const auto index = static_cast<std::size_t>(offset - mBufferOffset);
    const auto available = std::min(maxBytes, mBytes.size() - index);
    const auto end = index + available;
    std::size_t position = index;
    // 以连续片段验证和消费 UTF-8，避免为扫描到的每个 ASCII 字节构造字符对象。
    // 换行由统一慢路径消费，因而 CRLF、行数及行内游标规则只有一份实现。
    while (position < end) {
        const auto first = static_cast<unsigned char>(mBytes[position]);
        if (first == '\r' || first == '\n') break;
        if (first < 0x80) {
            if (first < 0x09 || (first > 0x0d && first < 0x20)) {
                if (position == index)
                    return {MaiErrorCode::InvalidInput, "file contains binary control bytes"};
                break;
            }
            ++position;
        } else {
            const int width = utf8Width(mBytes, position);
            if (width <= 0) {
                if (position == index && (width < 0 || mBufferEof))
                    return {MaiErrorCode::InvalidInput, "file is not valid UTF-8"};
                break;
            }
            if (static_cast<std::size_t>(width) > end - position) break;
            position += static_cast<std::size_t>(width);
        }
    }
    return std::string_view(mBytes.data() + index, position - index);
}

MaiError MaiTextFileBuffer::prepareLegacy(std::size_t index, const std::string& encoding) {
    // 一次桥接完整有界块，并建立源字符到 UTF-8 标量的映射。游标按源字节前进，
    // 输出按 UTF-8 字节计量，二者不能用同一个偏移，也不能为每个汉字往返宿主 JNI。
    mLegacySourceWidths.assign(mBytes.size() + 1, 0);
    mLegacyUtf8Offsets.assign(mBytes.size() + 1, 0);
    std::size_t sourceEnd = index;
    while (sourceEnd < mBytes.size()) {
        if (isCanceled(mOptions)) return {MaiErrorCode::Canceled, "file read canceled"};
        const auto first = static_cast<unsigned char>(mBytes[sourceEnd]);
        std::size_t width = 1;
        if ((encoding == "gbk" || encoding == "gb18030") && first >= 0x81 && first <= 0xfe) {
            width = 2;
            if (encoding == "gb18030" && mBytes.size() - sourceEnd >= 2) {
                const auto second = static_cast<unsigned char>(mBytes[sourceEnd + 1]);
                if (second >= 0x30 && second <= 0x39) width = 4;
            }
        } else if (encoding == "shift-jis" &&
                   ((first >= 0x81 && first <= 0x9f) || (first >= 0xe0 && first <= 0xfc))) {
            width = 2;
        }
        if (mBytes.size() - sourceEnd < width) {
            if (mBufferEof)
                return {MaiErrorCode::InvalidInput,
                        "file ends with an incomplete encoded character"};
            break;
        }
        mLegacySourceWidths[sourceEnd] = static_cast<unsigned char>(width);
        sourceEnd += width;
    }
    const auto decoded = MaiTextFileReader::decodeLegacyText(
        mBytes.substr(index, sourceEnd - index), encoding, mOptions.decodeText);
    if (!decoded) return decoded.error();
    mLegacyText = decoded.value();
    if (mLegacyText.size() > 4 * (sourceEnd - index))
        return {MaiErrorCode::InvalidInput, "legacy decoder exceeded the bounded UTF-8 expansion"};
    std::size_t textOffset = 0;
    for (std::size_t sourceOffset = index; sourceOffset < sourceEnd;) {
        if (isCanceled(mOptions)) return {MaiErrorCode::Canceled, "file read canceled"};
        if (textOffset >= mLegacyText.size())
            return {MaiErrorCode::InvalidInput,
                    "legacy decoder changed the source character count"};
        const int textWidth = utf8Width(mLegacyText, textOffset);
        if (textWidth <= 0)
            return {MaiErrorCode::InvalidInput, "legacy decoder returned invalid UTF-8"};
        const auto first = static_cast<unsigned char>(mBytes[sourceOffset]);
        if ((first < 0x80 && (textWidth != 1 || mLegacyText[textOffset] != mBytes[sourceOffset])) ||
            (first >= 0x80 && textWidth == 1 &&
             (mLegacyText[textOffset] == '\r' || mLegacyText[textOffset] == '\n')))
            return {MaiErrorCode::InvalidInput, "legacy decoder changed source line boundaries"};
        mLegacyUtf8Offsets[sourceOffset] = static_cast<std::uint32_t>(textOffset);
        textOffset += static_cast<std::size_t>(textWidth);
        sourceOffset += mLegacySourceWidths[sourceOffset];
        mLegacyUtf8Offsets[sourceOffset] = static_cast<std::uint32_t>(textOffset);
    }
    if (textOffset != mLegacyText.size())
        return {MaiErrorCode::InvalidInput, "legacy decoder changed the source character count"};
    return {};
}

MaiResult<MaiDecodedFileCharacter> MaiTextFileBuffer::character(std::uint64_t offset,
                                                                const std::string& encoding) {
    const auto error = ensure(offset);
    if (error) return error;
    if (isEofAt(offset)) return MaiDecodedFileCharacter{};
    const auto index = static_cast<std::size_t>(offset - mBufferOffset);
    const auto first = static_cast<unsigned char>(mBytes[index]);
    MaiDecodedFileCharacter result;
    if (encoding == "utf-8") {
        const int width = utf8Width(mBytes, index);
        if (width <= 0)
            return {MaiErrorCode::InvalidInput,
                    "file is not valid UTF-8; retry with an explicit encoding"};
        result.sourceBytes = static_cast<std::size_t>(width);
        result.text = mBytes.substr(index, result.sourceBytes);
    } else if (encoding == "utf-16le" || encoding == "utf-16be") {
        if (mBytes.size() - index < 2)
            return {MaiErrorCode::InvalidInput, "UTF-16 file ends with an incomplete code unit"};
        const bool little = encoding == "utf-16le";
        const auto unitAt = [&](std::size_t at) {
            const auto left = static_cast<unsigned char>(mBytes[at]);
            const auto right = static_cast<unsigned char>(mBytes[at + 1]);
            return static_cast<std::uint32_t>(little ? left | (right << 8) : (left << 8) | right);
        };
        std::uint32_t codePoint = unitAt(index);
        result.sourceBytes = 2;
        if (codePoint >= 0xd800 && codePoint <= 0xdbff) {
            if (mBytes.size() - index < 4)
                return {MaiErrorCode::InvalidInput, "UTF-16 file ends with a high surrogate"};
            const auto low = unitAt(index + 2);
            if (low < 0xdc00 || low > 0xdfff)
                return {MaiErrorCode::InvalidInput, "UTF-16 file has an invalid surrogate pair"};
            codePoint = 0x10000 + ((codePoint - 0xd800) << 10) + (low - 0xdc00);
            result.sourceBytes = 4;
        } else if (codePoint >= 0xdc00 && codePoint <= 0xdfff) {
            return {MaiErrorCode::InvalidInput, "UTF-16 file has an unmatched low surrogate"};
        }
        appendUtf8(result.text, codePoint);
    } else {
        if (first < 0x80) {
            result.sourceBytes = 1;
            result.text.assign(1, static_cast<char>(first));
        } else {
            if (index >= mLegacySourceWidths.size() || mLegacySourceWidths[index] == 0) {
                const auto prepared = prepareLegacy(index, encoding);
                if (prepared) return prepared;
            }
            result.sourceBytes = mLegacySourceWidths[index];
            const auto textOffset = mLegacyUtf8Offsets[index];
            const auto textEnd = mLegacyUtf8Offsets[index + result.sourceBytes];
            result.text = mLegacyText.substr(textOffset, textEnd - textOffset);
        }
    }
    if (isBinaryCharacter(result.text))
        return {MaiErrorCode::InvalidInput, "file contains binary control bytes"};
    return result;
}

MaiResult<MaiTextFileCursor> initialCursor(const MaiFilePath& path,
                                           const MaiRegularFileInfo& snapshot,
                                           const MaiTextFileReadOptions& options,
                                           MaiTextFileBuffer& buffer) {
    MaiTextFileCursor cursor;
    cursor.sourcePath = path;
    cursor.snapshot = snapshot;
    cursor.targetLine = options.offset;
    cursor.encoding = normalizeEncoding(options.encoding);
    const auto prefix = buffer.prefix();
    if (!prefix) return prefix.error();
    const auto& bytes = prefix.value();
    const bool littleBom = bytes.size() >= 2 && bytes.compare(0, 2, "\xff\xfe", 2) == 0;
    const bool bigBom = bytes.size() >= 2 && bytes.compare(0, 2, "\xfe\xff", 2) == 0;
    const bool utf8Bom = bytes.size() >= 3 && bytes.compare(0, 3, "\xef\xbb\xbf", 3) == 0;
    if (cursor.encoding == "auto") {
        if (littleBom)
            cursor.encoding = "utf-16le";
        else if (bigBom)
            cursor.encoding = "utf-16be";
        else if (utf8Bom || isUtf8Prefix(bytes, buffer.isEofAt(bytes.size())))
            cursor.encoding = "utf-8";
        else {
#if defined(_WIN32)
            cursor.encoding = systemEncoding();
#else
            cursor.encoding = "gb18030";
#endif
        }
    } else if (cursor.encoding == "system") {
        cursor.encoding = systemEncoding();
    }
    if (!isSupportedEncoding(cursor.encoding))
        return {MaiErrorCode::NotSupported,
                "unsupported or stateful source encoding; specify utf-8, utf-16le, utf-16be, "
                "gb18030, gbk, shift-jis, windows-1252, or latin1"};
    if ((cursor.encoding == "utf-16le" && bigBom) || (cursor.encoding == "utf-16be" && littleBom))
        return {MaiErrorCode::InvalidInput, "UTF-16 BOM conflicts with the requested encoding"};
    if ((cursor.encoding == "utf-16le" && littleBom) || (cursor.encoding == "utf-16be" && bigBom))
        cursor.byteOffset = 2;
    if (cursor.encoding == "utf-8" && utf8Bom) cursor.byteOffset = 3;
    return cursor;
}

}  // namespace

MaiResult<std::string> MaiTextFileReader::decodeLegacyText(
    const std::string& bytes, const std::string& encoding,
    const std::function<MaiResult<std::string>(const std::string&, const std::string&)>&
        decodeText) {
    std::string source = encoding.empty() ? "auto" : encoding;
    std::transform(source.begin(), source.end(), source.begin(), [](unsigned char character) {
        return character == '_' ? '-' : static_cast<char>(std::tolower(character));
    });
    auto decoded = decodeLegacy(bytes, source, decodeText);
    if (decoded && !isUtf8Prefix(decoded.value(), true))
        return {MaiErrorCode::InvalidInput, "legacy decoder returned invalid UTF-8"};
    return decoded;
}

MaiResult<MaiTextFilePage> MaiTextFileReader::read(const MaiFilePath& path,
                                                   const MaiTextFileReadOptions& options) {
    if (path.isEmpty() || options.offset == 0 || options.limit == 0 || options.limit > 10000 ||
        options.maxOutputBytes < 4 || options.maxOutputBytes > 1024 * 1024 ||
        options.maxScanBytes < 4 || options.maxDuration.count() <= 0 ||
        (options.cursor && options.offset != 1))
        return {MaiErrorCode::InvalidInput, "invalid text file page options"};
    if (isCanceled(options)) return {MaiErrorCode::Canceled, "file read canceled"};
    const auto started = std::chrono::steady_clock::now();
    auto opened = MaiFileReadHandle::open(path);
    if (!opened) return opened.error();
    const auto before = opened.value()->snapshot();
    if (!before) return before.error();
    const auto snapshot = before.value();
    MaiTextFileBuffer buffer(*opened.value(), options);
    MaiTextFileCursor cursor;
    if (options.cursor) {
        cursor = *options.cursor;
        const auto requested = normalizeEncoding(options.encoding);
        if (cursor.sourcePath != path || !sameSnapshot(cursor.snapshot, snapshot))
            return changedSource();
        if ((snapshot.bytes != 0 && cursor.byteOffset > snapshot.bytes) || cursor.lineNumber == 0 ||
            cursor.targetLine == 0 || !isSupportedEncoding(cursor.encoding) ||
            (requested != "auto" && requested != "system" && requested != cursor.encoding) ||
            ((cursor.encoding == "utf-16le" || cursor.encoding == "utf-16be") &&
             cursor.byteOffset % 2 != 0))
            return {MaiErrorCode::InvalidInput, "invalid text file continuation cursor"};
    } else {
        auto initial = initialCursor(path, snapshot, options, buffer);
        if (!initial) return initial.error();
        cursor = std::move(initial.value());
    }
    MaiTextFilePage page;
    page.snapshot = snapshot;
    page.encoding = cursor.encoding;
    const std::uint64_t startOffset = cursor.byteOffset;
    std::size_t outputBytes = 0;
    std::optional<MaiTextFileLine> current;
    while (true) {
        if (isCanceled(options)) return {MaiErrorCode::Canceled, "file read canceled"};
        if (std::chrono::steady_clock::now() - started >= options.maxDuration) {
            page.stopReason = MaiTextFileStopReason::TimeLimit;
            break;
        }
        if (cursor.byteOffset - startOffset >= options.maxScanBytes) {
            page.stopReason = MaiTextFileStopReason::ScanLimit;
            break;
        }
        const bool selected = cursor.lineNumber >= cursor.targetLine;
        if (cursor.encoding == "utf-8") {
            const auto scanRemaining = options.maxScanBytes - (cursor.byteOffset - startOffset);
            const auto outputRemaining =
                selected ? options.maxOutputBytes - outputBytes : kBufferBytes;
            const auto span = buffer.utf8Span(
                cursor.byteOffset,
                static_cast<std::size_t>(std::min<std::uint64_t>(
                    scanRemaining, std::min<std::size_t>(kBufferBytes, outputRemaining))));
            if (!span) return span.error();
            if (!span.value().empty()) {
                if (selected && !current) {
                    current.emplace();
                    current->number = cursor.lineNumber;
                    current->byteOffset = cursor.lineUtf8Offset;
                }
                if (current) {
                    current->text.append(span.value().data(), span.value().size());
                    outputBytes += span.value().size();
                }
                cursor.byteOffset += span.value().size();
                if (cursor.lineUtf8Offset >
                    std::numeric_limits<std::uint64_t>::max() - span.value().size())
                    return {MaiErrorCode::InvalidInput, "text line byte offset overflow"};
                cursor.lineUtf8Offset += span.value().size();
                continue;
            }
        }
        const auto decoded = buffer.character(cursor.byteOffset, cursor.encoding);
        if (!decoded) return decoded.error();
        auto character = decoded.value();
        if (character.sourceBytes == 0) break;
        bool newline = character.text == "\n";
        // CRLF 必须作为整体消费；否则在两页之间切开时会残留 CR 或凭空增加空行。
        if (character.text == "\r") {
            const auto next =
                buffer.character(cursor.byteOffset + character.sourceBytes, cursor.encoding);
            if (!next) return next.error();
            if (next.value().text == "\n") {
                newline = true;
                character.sourceBytes += next.value().sourceBytes;
            }
        }
        if (selected && !newline && character.text.size() > options.maxOutputBytes - outputBytes) {
            page.stopReason = MaiTextFileStopReason::OutputLimit;
            break;
        }
        if (selected && !current) {
            current.emplace();
            current->number = cursor.lineNumber;
            current->byteOffset = cursor.lineUtf8Offset;
        }
        cursor.byteOffset += character.sourceBytes;
        if (newline) {
            if (cursor.lineNumber == std::numeric_limits<std::uint64_t>::max())
                return {MaiErrorCode::InvalidInput, "text line number overflow"};
            ++cursor.lineNumber;
            cursor.lineUtf8Offset = 0;
            if (current) {
                current->complete = true;
                page.lines.push_back(std::move(*current));
                current.reset();
                if (page.lines.size() == options.limit) {
                    page.stopReason = MaiTextFileStopReason::LineLimit;
                    break;
                }
            }
        } else {
            if (cursor.lineUtf8Offset >
                std::numeric_limits<std::uint64_t>::max() - character.text.size())
                return {MaiErrorCode::InvalidInput, "text line byte offset overflow"};
            cursor.lineUtf8Offset += character.text.size();
            if (current) {
                current->text += character.text;
                outputBytes += character.text.size();
            }
        }
    }
    page.eof = buffer.isEofAt(cursor.byteOffset);
    if (current) {
        current->complete = page.eof;
        page.lines.push_back(std::move(*current));
    }
    if (page.eof)
        page.stopReason = MaiTextFileStopReason::EndOfFile;
    else
        page.nextCursor = cursor;
    // procfs 等虚拟文件可返回非空内容却始终报告 size=0，并且内容变化不更新快照元数据。
    // 允许本次句柄读到真实 EOF 的有界单页，不能承诺跨次续读仍属于同一内容版本。
    if (snapshot.bytes == 0 && !page.eof)
        return {
            MaiErrorCode::NotSupported,
            "this size-zero virtual file needs continuation but cannot provide a stable snapshot"};
    if (isCanceled(options)) return {MaiErrorCode::Canceled, "file read canceled"};
    const auto validated = opened.value()->validate(snapshot);
    if (validated) return validated;
    page.io = opened.value()->stats();
    return page;
}
