// 文件作用：把图片文件解码为供移动端或桌面端绘制的像素缓冲。
// 调用边界：由 MaiChat 媒体或 Agent 宿主调用；平台资源在停止或失败时须正确释放。
#include "MaiImageDecode.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <climits>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavcodec/exif.h>
#include <libavformat/avformat.h>
#include <libavutil/crc.h>
#include <libavutil/display.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/intreadwrite.h>
#include <libswscale/swscale.h>
}

namespace {

constexpr long long kMaximumPixels = 64'000'000;
constexpr long long kMaximumGridPixels = 32'000'000;
constexpr uint64_t kDefaultEncodedBytes = 128ull * 1024 * 1024;
constexpr uint64_t kMaximumEncodedBytes = 512ull * 1024 * 1024;
constexpr uint64_t kMaximumSourcePixels = 128'000'000;

MaiImageInfo failedInfo(int error) {
    MaiImageInfo info{};
    info.error_code = error;
    return info;
}

int readOptions(const MaiImageReadOptions* supplied, MaiImageReadOptions& options) {
    options = supplied ? *supplied : MaiImageReadOptions{};
    if (!options.max_encoded_bytes) options.max_encoded_bytes = kDefaultEncodedBytes;
    if (!options.max_source_pixels) options.max_source_pixels = kMaximumPixels;
    if (options.max_encoded_bytes > kMaximumEncodedBytes || options.max_source_pixels > kMaximumSourcePixels ||
        options.max_source_width < 0 || options.max_source_height < 0 || options.max_output_width < 0 ||
        options.max_output_height < 0)
        return AVERROR(EINVAL);
    return 0;
}

// These polls are made only by the synchronous entry/conversion loops. Native
// allocator and I/O callbacks must never invoke application callbacks.
bool isCanceled(const MaiImageReadOptions& options) {
    return options.is_canceled && options.is_canceled(options.context);
}

int checkDimensions(int width, int height, const MaiImageReadOptions& options) {
    if (width <= 0 || height <= 0) return AVERROR_INVALIDDATA;
    if (static_cast<uint64_t>(width) * height > options.max_source_pixels ||
        (options.max_source_width && width > options.max_source_width) ||
        (options.max_source_height && height > options.max_source_height))
        return AVERROR(EFBIG);
    return 0;
}

struct DecodeBudget {
    const MaiImageReadOptions* options;
    std::atomic<int> error{0};
};

int boundedFrameBuffer(AVCodecContext* codec, AVFrame* frame, int flags) {
    auto* budget = static_cast<DecodeBudget*>(codec->opaque);
    const int error = checkDimensions(frame->width > 0 ? frame->width : codec->width,
                                      frame->height > 0 ? frame->height : codec->height, *budget->options);
    if (error < 0) {
        budget->error = error;
        return error;
    }
    return avcodec_default_get_buffer2(codec, frame, flags);
}

void constrainCodec(AVCodecContext* codec, DecodeBudget& budget) {
    codec->opaque = &budget;
    codec->get_buffer2 = boundedFrameBuffer;
    codec->thread_count = 1;
    codec->thread_type = 0;
    // DR1 codecs use our exact allocator guard; their internal check also counts
    // aligned scanlines. Self-allocating codecs (notably libdav1d) never call it:
    // give those the exact limit, and inspect coded dimensions before opening.
    const bool directRendering = codec->codec && (codec->codec->capabilities & AV_CODEC_CAP_DR1);
    codec->max_pixels = static_cast<int64_t>(budget.options->max_source_pixels) * (directRendering ? 2 : 1);
    codec->err_recognition = AV_EF_CRCCHECK | AV_EF_BITSTREAM | AV_EF_BUFFER | AV_EF_EXPLODE;
}

MaiImageDecodeResult failed(int error) {
    return {nullptr, 0, 0, 0, error, nullptr, 0, 0, 0};
}

int orientationFromMatrix(const uint8_t* data, size_t size) {
    if (!data || size < 9 * sizeof(int32_t)) return 1;
    int32_t matrix[9];
    std::memcpy(matrix, data, sizeof(matrix));
    const double horizontal = std::hypot(double(matrix[0]), double(matrix[1]));
    const double vertical = std::hypot(double(matrix[3]), double(matrix[4]));
    if (!horizontal || !vertical) return 1;
    // Match all eight orthogonal transforms, including negative angles and
    // mirrors. Rotation alone loses EXIF 2/4/5/7 and HEIF imir.
    for (int orientation = 1; orientation <= 8; ++orientation) {
        int32_t reference[9];
        av_exif_orientation_to_matrix(reference, orientation);
        bool matches = true;
        for (int index : {0, 1, 3, 4}) {
            const double scale = index < 3 ? horizontal : vertical;
            if (std::abs(matrix[index] / scale - reference[index] / 65536.0) > 0.001) matches = false;
        }
        if (matches) return orientation;
    }
    return 1;
}

int displayRotation(const AVPacketSideData* sideData, int count) {
    for (int index = 0; index < count; ++index) {
        const AVPacketSideData& entry = sideData[index];
        if (entry.type != AV_PKT_DATA_DISPLAYMATRIX) continue;
        return orientationFromMatrix(entry.data, entry.size);
    }
    return 1;
}

AVPixelFormat scalePixelFormat(AVPixelFormat format) {
    switch (format) {
        case AV_PIX_FMT_YUVJ420P: return AV_PIX_FMT_YUV420P;
        case AV_PIX_FMT_YUVJ422P: return AV_PIX_FMT_YUV422P;
        case AV_PIX_FMT_YUVJ444P: return AV_PIX_FMT_YUV444P;
        case AV_PIX_FMT_YUVJ440P: return AV_PIX_FMT_YUV440P;
        default: return format;
    }
}

int configureColor(SwsContext* scale, const AVFrame* frame) {
    int matrix = SWS_CS_ITU601;
    switch (frame->colorspace) {
        case AVCOL_SPC_BT709: matrix = SWS_CS_ITU709; break;
        case AVCOL_SPC_BT2020_NCL:
        case AVCOL_SPC_BT2020_CL: matrix = SWS_CS_BT2020; break;
        default: break;
    }
    const int* coefficients = sws_getCoefficients(matrix);
    const bool fullRange = frame->color_range == AVCOL_RANGE_JPEG || frame->format == AV_PIX_FMT_YUVJ420P ||
                           frame->format == AV_PIX_FMT_YUVJ422P || frame->format == AV_PIX_FMT_YUVJ444P ||
                           frame->format == AV_PIX_FMT_YUVJ440P;
    return sws_setColorspaceDetails(scale, coefficients, fullRange ? 1 : 0, coefficients, 1, 0, 1 << 16, 1 << 16);
}

MaiImageDecodeResult orientRgba(MaiImageDecodeResult image, int orientation, const MaiImageReadOptions& options) {
    if (!image.rgba || orientation == 1) return image;
    const int width = orientation < 5 ? image.width : image.height;
    const int height = orientation < 5 ? image.height : image.width;
    auto output = std::unique_ptr<unsigned char, decltype(&std::free)>(
        static_cast<unsigned char*>(std::malloc(static_cast<size_t>(width) * height * 4)), &std::free);
    if (!output) {
        maiImageDecodeFree(image.rgba);
        return failed(AVERROR(ENOMEM));
    }
    for (int y = 0; y < height; ++y) {
        if (y % 64 == 0 && isCanceled(options)) {
            maiImageDecodeFree(image.rgba);
            return failed(AVERROR_EXIT);
        }
        for (int x = 0; x < width; ++x) {
            int sourceX = x;
            int sourceY = y;
            if (orientation == 2) {
                sourceX = image.width - 1 - x;
            } else if (orientation == 3) {
                sourceX = image.width - 1 - x;
                sourceY = image.height - 1 - y;
            } else if (orientation == 4) {
                sourceY = image.height - 1 - y;
            } else if (orientation == 5) {
                sourceX = y;
                sourceY = x;
            } else if (orientation == 6) {
                sourceX = y;
                sourceY = image.height - 1 - x;
            } else if (orientation == 7) {
                sourceX = image.width - 1 - y;
                sourceY = image.height - 1 - x;
            } else if (orientation == 8) {
                sourceX = image.width - 1 - y;
                sourceY = x;
            }
            std::memcpy(output.get() + (static_cast<size_t>(y) * width + x) * 4,
                        image.rgba + static_cast<size_t>(sourceY) * image.stride + sourceX * 4, 4);
        }
    }
    maiImageDecodeFree(image.rgba);
    return {output.release(), width, height, width * 4, 0, nullptr, 0, 0, 0};
}

MaiImageDecodeResult attachColorMetadata(MaiImageDecodeResult image, const unsigned char* profile, size_t profileSize,
                                         int primaries, int transfer) {
    if (!image.rgba) return image;
    image.color_primaries = primaries;
    image.color_transfer = transfer;
    if (!image.rgba || !profile || !profileSize) return image;
    if (profileSize > 1024 * 1024) {
        maiImageDecodeFree(image.rgba);
        return failed(AVERROR(EINVAL));
    }
    const size_t pixelBytes = static_cast<size_t>(image.stride) * image.height;
    auto* combined = static_cast<unsigned char*>(std::malloc(pixelBytes + profileSize));
    if (!combined) {
        maiImageDecodeFree(image.rgba);
        return failed(AVERROR(ENOMEM));
    }
    std::memcpy(combined, image.rgba, pixelBytes);
    std::memcpy(combined + pixelBytes, profile, profileSize);
    maiImageDecodeFree(image.rgba);
    image.rgba = combined;
    image.icc_profile = combined + pixelBytes;
    image.icc_size = static_cast<int>(profileSize);
    return image;
}

const AVPacketSideData* packetIcc(const AVPacketSideData* sideData, int count) {
    for (int index = 0; index < count; ++index)
        if (sideData[index].type == AV_PKT_DATA_ICC_PROFILE) return &sideData[index];
    return nullptr;
}

MaiImageDecodeResult resizeRgba(const unsigned char* pixels, int width, int height, int maxWidth, int maxHeight,
                                int orientation, const MaiImageReadOptions& options) {
    const int displayWidth = orientation >= 5 ? height : width;
    const int displayHeight = orientation >= 5 ? width : height;
    double ratio = 1.0;
    if (maxWidth > 0) ratio = std::min(ratio, static_cast<double>(maxWidth) / displayWidth);
    if (maxHeight > 0) ratio = std::min(ratio, static_cast<double>(maxHeight) / displayHeight);
    const int outputWidth = std::max(1, static_cast<int>(std::floor(width * ratio)));
    const int outputHeight = std::max(1, static_cast<int>(std::floor(height * ratio)));
    if (outputWidth > std::numeric_limits<int>::max() / 4 ||
        static_cast<long long>(outputWidth) * outputHeight > kMaximumPixels)
        return failed(AVERROR(EINVAL));
    auto output = std::unique_ptr<unsigned char, decltype(&std::free)>(
        static_cast<unsigned char*>(std::malloc(static_cast<size_t>(outputWidth) * outputHeight * 4)), &std::free);
    if (!output) return failed(AVERROR(ENOMEM));
    if (outputWidth == width && outputHeight == height) {
        std::memcpy(output.get(), pixels, static_cast<size_t>(width) * height * 4);
    } else {
        auto freeScale = [](SwsContext* value) { sws_freeContext(value); };
        // Accurate rounding preserves uniform tones at extreme SIMD reduction ratios.
        std::unique_ptr<SwsContext, decltype(freeScale)> scale(
            sws_getContext(width, height, AV_PIX_FMT_RGBA, outputWidth, outputHeight, AV_PIX_FMT_RGBA,
                           SWS_LANCZOS | SWS_ACCURATE_RND, nullptr, nullptr, nullptr),
            freeScale);
        if (!scale) return failed(AVERROR(ENOSYS));
        const uint8_t* source[] = {pixels, nullptr, nullptr, nullptr};
        const int sourceLines[] = {width * 4, 0, 0, 0};
        uint8_t* target[] = {output.get(), nullptr, nullptr, nullptr};
        const int targetLines[] = {outputWidth * 4, 0, 0, 0};
        const int scaled = sws_scale(scale.get(), source, sourceLines, 0, height, target, targetLines);
        if (scaled != outputHeight) return failed(scaled < 0 ? scaled : AVERROR_INVALIDDATA);
    }
    return orientRgba({output.release(), outputWidth, outputHeight, outputWidth * 4, 0, nullptr, 0, 0, 0}, orientation,
                      options);
}

// 网格图片需按瓦片次序拼接后缩放；任一瓦片失败都释放已分配像素，避免交付局部图像。
MaiImageDecodeResult decodeTileGrid(AVFormatContext* format, const AVStreamGroup* group, int maxWidth, int maxHeight,
                                    const MaiImageReadOptions& options) {
    const AVStreamGroupTileGrid* grid = group->params.tile_grid;
    if (!grid || !grid->nb_tiles || grid->nb_tiles > 128 || !group->nb_streams || group->nb_streams > 128 ||
        grid->coded_width < 1 || grid->coded_height < 1 || grid->width < 1 || grid->height < 1 ||
        grid->horizontal_offset < 0 || grid->vertical_offset < 0 ||
        grid->horizontal_offset + grid->width > grid->coded_width ||
        grid->vertical_offset + grid->height > grid->coded_height ||
        static_cast<long long>(grid->coded_width) * grid->coded_height > kMaximumGridPixels)
        return failed(AVERROR(EINVAL));
    const int dimensionsError = checkDimensions(grid->width, grid->height, options);
    if (dimensionsError < 0) return failed(dimensionsError);
    DecodeBudget budget{&options};

    auto freeCodec = [](AVCodecContext* value) { avcodec_free_context(&value); };
    auto freeFrame = [](AVFrame* value) { av_frame_free(&value); };
    using Codec = std::unique_ptr<AVCodecContext, decltype(freeCodec)>;
    using Frame = std::unique_ptr<AVFrame, decltype(freeFrame)>;
    std::vector<Codec> decoders;
    std::vector<Frame> frames;
    std::vector<bool> ready(group->nb_streams, false);
    std::vector<int> position(format->nb_streams, -1);
    decoders.reserve(group->nb_streams);
    frames.reserve(group->nb_streams);
    for (unsigned int index = 0; index < group->nb_streams; ++index) {
        const AVStream* stream = group->streams[index];
        if (!stream || stream->index < 0 || stream->index >= format->nb_streams) return failed(AVERROR_INVALIDDATA);
        const AVCodec* decoder = avcodec_find_decoder(stream->codecpar->codec_id);
        Codec codec(avcodec_alloc_context3(decoder), freeCodec);
        Frame frame(av_frame_alloc(), freeFrame);
        if (!decoder || !codec || !frame) return failed(AVERROR(ENOMEM));
        if (avcodec_parameters_to_context(codec.get(), stream->codecpar) < 0) return failed(AVERROR_INVALIDDATA);
        constrainCodec(codec.get(), budget);
        if (avcodec_open2(codec.get(), decoder, nullptr) < 0) return failed(AVERROR_DECODER_NOT_FOUND);
        position[stream->index] = static_cast<int>(index);
        decoders.push_back(std::move(codec));
        frames.push_back(std::move(frame));
    }

    auto freePacket = [](AVPacket* value) { av_packet_free(&value); };
    std::unique_ptr<AVPacket, decltype(freePacket)> packet(av_packet_alloc(), freePacket);
    if (!packet) return failed(AVERROR(ENOMEM));
    unsigned int remaining = group->nb_streams;
    int status = 0;
    while (remaining && (status = av_read_frame(format, packet.get())) >= 0) {
        if (isCanceled(options)) return failed(AVERROR_EXIT);
        const int stream = packet->stream_index;
        const int slot = stream >= 0 && stream < position.size() ? position[stream] : -1;
        if (slot >= 0 && !ready[slot]) {
            status = avcodec_send_packet(decoders[slot].get(), packet.get());
            if (status >= 0) status = avcodec_receive_frame(decoders[slot].get(), frames[slot].get());
            if (status >= 0) {
                ready[slot] = true;
                --remaining;
            } else if (status != AVERROR(EAGAIN)) {
                av_packet_unref(packet.get());
                return failed(budget.error < 0 ? budget.error.load() : status);
            }
        }
        av_packet_unref(packet.get());
    }
    for (unsigned int index = 0; index < group->nb_streams && remaining; ++index) {
        if (ready[index]) continue;
        status = avcodec_send_packet(decoders[index].get(), nullptr);
        if (status >= 0) status = avcodec_receive_frame(decoders[index].get(), frames[index].get());
        if (status < 0) return failed(budget.error < 0 ? budget.error.load() : status);
        ready[index] = true;
        --remaining;
    }

    const size_t canvasBytes = static_cast<size_t>(grid->coded_width) * grid->coded_height * 4;
    auto canvas = std::unique_ptr<unsigned char, decltype(&std::free)>(
        static_cast<unsigned char*>(std::malloc(canvasBytes)), &std::free);
    if (!canvas) return failed(AVERROR(ENOMEM));
    for (size_t offset = 0; offset < canvasBytes; offset += 4) std::memcpy(canvas.get() + offset, grid->background, 4);
    auto freeScale = [](SwsContext* value) { sws_freeContext(value); };
    std::unique_ptr<SwsContext, decltype(freeScale)> scale(nullptr, freeScale);
    for (unsigned int tile = 0; tile < grid->nb_tiles; ++tile) {
        if (isCanceled(options)) return failed(AVERROR_EXIT);
        const auto& offset = grid->offsets[tile];
        if (offset.idx >= frames.size() || offset.horizontal < 0 || offset.vertical < 0)
            return failed(AVERROR_INVALIDDATA);
        const AVFrame* frame = frames[offset.idx].get();
        if (frame->decode_error_flags) return failed(AVERROR_INVALIDDATA);
        if (offset.horizontal + frame->width > grid->coded_width ||
            offset.vertical + frame->height > grid->coded_height)
            return failed(AVERROR_INVALIDDATA);
        scale.reset(sws_getCachedContext(scale.release(), frame->width, frame->height,
                                         scalePixelFormat(static_cast<AVPixelFormat>(frame->format)), frame->width,
                                         frame->height, AV_PIX_FMT_RGBA, SWS_BICUBIC, nullptr, nullptr, nullptr));
        if (!scale) return failed(AVERROR(ENOSYS));
        if (configureColor(scale.get(), frame) < 0) return failed(AVERROR(EINVAL));
        uint8_t* target[] = {
            canvas.get() + (static_cast<size_t>(offset.vertical) * grid->coded_width + offset.horizontal) * 4, nullptr,
            nullptr, nullptr};
        const int lines[] = {grid->coded_width * 4, 0, 0, 0};
        status = sws_scale(scale.get(), frame->data, frame->linesize, 0, frame->height, target, lines);
        if (status != frame->height) return failed(status < 0 ? status : AVERROR_INVALIDDATA);
    }

    const size_t croppedBytes = static_cast<size_t>(grid->width) * grid->height * 4;
    auto cropped = std::unique_ptr<unsigned char, decltype(&std::free)>(
        static_cast<unsigned char*>(std::malloc(croppedBytes)), &std::free);
    if (!cropped) return failed(AVERROR(ENOMEM));
    for (int row = 0; row < grid->height; ++row)
        std::memcpy(
            cropped.get() + static_cast<size_t>(row) * grid->width * 4,
            canvas.get() +
                (static_cast<size_t>(row + grid->vertical_offset) * grid->coded_width + grid->horizontal_offset) * 4,
            static_cast<size_t>(grid->width) * 4);
    const int rotation = displayRotation(grid->coded_side_data, grid->nb_coded_side_data);
    MaiImageDecodeResult image =
        resizeRgba(cropped.get(), grid->width, grid->height, maxWidth, maxHeight, rotation, options);
    const AVPacketSideData* icc = packetIcc(grid->coded_side_data, grid->nb_coded_side_data);
    const AVCodecParameters* codec = group->streams[0]->codecpar;
    return attachColorMetadata(image, icc ? icc->data : nullptr, icc ? icc->size : 0, codec->color_primaries,
                               codec->color_trc);
}

struct RasterHeader {
    MaiImageInfo info{};
    const char* demuxer = nullptr;
    AVCodecID codec = AV_CODEC_ID_NONE;
    int stream = -1;
};

int exifOrientation(const unsigned char* bytes, size_t size) {
    if (size > 1024 * 1024) return 1;
    if (size >= 6 && std::memcmp(bytes, "Exif\0\0", 6) == 0) {
        bytes += 6;
        size -= 6;
    }
    AVExifMetadata metadata{};
    if (av_exif_parse_buffer(nullptr, bytes, size, &metadata, AV_EXIF_TIFF_HEADER) < 0) return 1;
    AVExifEntry* entry = nullptr;
    const int found = av_exif_get_entry(nullptr, &metadata, 0x112, 0, &entry);
    int orientation = 1;
    if (found > 0 && entry && entry->type == AV_TIFF_SHORT && entry->count == 1 && entry->value.uint[0] >= 1 &&
        entry->value.uint[0] <= 8)
        orientation = static_cast<int>(entry->value.uint[0]);
    av_exif_free(&metadata);
    return orientation;
}

int pngHeader(const unsigned char* bytes, size_t size, bool complete, RasterHeader& header,
              const MaiImageReadOptions& options) {
    if (size < 33 || AV_RB32(bytes + 8) != 13 || std::memcmp(bytes + 12, "IHDR", 4)) return AVERROR_INVALIDDATA;
    const uint32_t width = AV_RB32(bytes + 16), height = AV_RB32(bytes + 20);
    if (width > INT_MAX || height > INT_MAX) return AVERROR_INVALIDDATA;
    header.info.width = int(width);
    header.info.height = int(height);
    bool pixels = false, end = false;
    for (size_t offset = 8; offset + 12 <= size;) {
        if (isCanceled(options)) return AVERROR_EXIT;
        const uint32_t count = AV_RB32(bytes + offset);
        if (count > size - offset - 12) {
            if (complete) return AVERROR_INVALIDDATA;
            break;
        }
        const unsigned char* type = bytes + offset + 4;
        if (complete) {
            const uint32_t crc =
                av_crc(av_crc_get_table(AV_CRC_32_IEEE_LE), UINT32_MAX, type, size_t(count) + 4) ^ UINT32_MAX;
            if (crc != AV_RB32(bytes + offset + 8 + count)) return AVERROR_INVALIDDATA;
        }
        if (!std::memcmp(type, "eXIf", 4)) header.info.orientation = exifOrientation(type + 4, count);
        if (!std::memcmp(type, "acTL", 4)) {
            header.demuxer = "apng";
            header.codec = AV_CODEC_ID_APNG;
        }
        if (!std::memcmp(type, "IDAT", 4)) pixels = true;
        if (!std::memcmp(type, "IEND", 4)) {
            if (count) return AVERROR_INVALIDDATA;
            end = true;
            break;
        }
        offset += size_t(count) + 12;
    }
    return complete && (!pixels || !end) ? AVERROR_INVALIDDATA : 0;
}

int jpegHeader(const unsigned char* bytes, size_t size, bool complete, RasterHeader& header,
               const MaiImageReadOptions& options) {
    bool entropy = false;
    size_t offset = 2;
    while (offset < size) {
        if (isCanceled(options)) return AVERROR_EXIT;
        if (entropy) {
            while (offset < size && bytes[offset] != 0xff) ++offset;
        }
        if (offset >= size || bytes[offset++] != 0xff) return AVERROR_INVALIDDATA;
        while (offset < size && bytes[offset] == 0xff) ++offset;
        if (offset >= size) return AVERROR_INVALIDDATA;
        const int marker = bytes[offset++];
        if (entropy && (marker == 0 || (marker >= 0xd0 && marker <= 0xd7))) continue;
        if (marker == 0xd9) {
            // A concatenated MJPEG elementary stream must not become an image
            // merely because its first packet is a valid JPEG.
            while (offset < size &&
                   (bytes[offset] == 0 || bytes[offset] == '\n' || bytes[offset] == '\r' || bytes[offset] == ' '))
                ++offset;
            return offset == size && header.info.width > 0 ? 0 : AVERROR_INVALIDDATA;
        }
        if (marker == 0xd8 || marker == 0) return AVERROR_INVALIDDATA;
        if (marker == 0x01) continue;
        if (size - offset < 2) return AVERROR_INVALIDDATA;
        const size_t length = AV_RB16(bytes + offset);
        if (length < 2 || length > size - offset) return AVERROR_INVALIDDATA;
        if (marker >= 0xc0 && marker <= 0xcf && marker != 0xc4 && marker != 0xc8 && marker != 0xcc) {
            if (length < 8) return AVERROR_INVALIDDATA;
            header.info.height = AV_RB16(bytes + offset + 3);
            header.info.width = AV_RB16(bytes + offset + 5);
        }
        if (marker == 0xe1 && length >= 8 && !std::memcmp(bytes + offset + 2, "Exif\0\0", 6))
            header.info.orientation = exifOrientation(bytes + offset + 2, length - 2);
        offset += length;
        if (marker == 0xda) {
            entropy = true;
        }
    }
    return AVERROR_INVALIDDATA;
}

int webpHeader(const unsigned char* bytes, size_t size, bool complete, RasterHeader& header,
               const MaiImageReadOptions& options) {
    const uint64_t declared = uint64_t(AV_RL32(bytes + 4)) + 8;
    if (declared < 20 || (complete && declared != size)) return AVERROR_INVALIDDATA;
    const size_t limit = static_cast<size_t>(std::min<uint64_t>(declared, size));
    bool pixels = false;
    for (size_t offset = 12; offset + 8 <= limit;) {
        if (isCanceled(options)) return AVERROR_EXIT;
        const uint32_t count = AV_RL32(bytes + offset + 4);
        if (count > limit - offset - 8) {
            if (complete) return AVERROR_INVALIDDATA;
            break;
        }
        const unsigned char *chunk = bytes + offset, *data = chunk + 8;
        if (!std::memcmp(chunk, "VP8X", 4) && count >= 10) {
            header.info.width = int(AV_RL24(data + 4)) + 1;
            header.info.height = int(AV_RL24(data + 7)) + 1;
            if (data[0] & 2) {
                header.demuxer = "webp_anim";
                header.codec = AV_CODEC_ID_WEBP_ANIM;
            }
        } else if (!std::memcmp(chunk, "VP8L", 4) && count >= 5 && data[0] == 0x2f) {
            if (!header.info.width) {
                const uint32_t dimensions = AV_RL32(data + 1);
                header.info.width = int(dimensions & 0x3fff) + 1;
                header.info.height = int((dimensions >> 14) & 0x3fff) + 1;
            }
            pixels = true;
        } else if (!std::memcmp(chunk, "VP8 ", 4) && count >= 10 && !(data[0] & 1) &&
                   !std::memcmp(data + 3, "\x9d\x01\x2a", 3)) {
            if (!header.info.width) {
                header.info.width = AV_RL16(data + 6) & 0x3fff;
                header.info.height = AV_RL16(data + 8) & 0x3fff;
            }
            pixels = true;
        } else if (!std::memcmp(chunk, "ANMF", 4)) {
            pixels = count >= 16;
        } else if (!std::memcmp(chunk, "EXIF", 4)) {
            header.info.orientation = exifOrientation(data, count);
        }
        const uint64_t next = uint64_t(offset) + 8 + count + (count & 1);
        if (next > limit) return complete ? AVERROR_INVALIDDATA : 0;
        offset = static_cast<size_t>(next);
    }
    return header.info.width > 0 && (!complete || pixels) ? 0 : AVERROR_INVALIDDATA;
}

// Bounded box traversal recognizes only item-based still HEIF/AVIF. Normal MOV/
// MP4 tracks and image sequences must retain their video classification.
bool heifHeader(const unsigned char* bytes, size_t size, RasterHeader& header) {
    bool still = false, primary = false, avif = false;
    for (size_t offset = 0; offset + 8 <= size;) {
        uint64_t count = AV_RB32(bytes + offset);
        size_t prefix = 8;
        if (count == 1) {
            if (size - offset < 16) return false;
            count = AV_RB64(bytes + offset + 8);
            prefix = 16;
        } else if (!count)
            count = size - offset;
        if (count < prefix || count > size - offset) return false;
        const auto* type = bytes + offset + 4;
        const size_t end = offset + size_t(count);
        if (!std::memcmp(type, "moov", 4)) return false;
        if (!std::memcmp(type, "ftyp", 4)) {
            if (count < prefix + 8 || (count - prefix) % 4) return false;
            for (size_t index = offset + prefix; index + 4 <= end; index += 4) {
                if (index == offset + prefix + 4) continue;
                const auto* brand = bytes + index;
                if (!std::memcmp(brand, "avif", 4)) avif = true;
                if (!std::memcmp(brand, "avif", 4) || !std::memcmp(brand, "mif1", 4) ||
                    !std::memcmp(brand, "heic", 4) || !std::memcmp(brand, "heix", 4) ||
                    !std::memcmp(brand, "heim", 4) || !std::memcmp(brand, "heis", 4))
                    still = true;
                if (index == offset + prefix && (!std::memcmp(brand, "avis", 4) || !std::memcmp(brand, "msf1", 4) ||
                                                 !std::memcmp(brand, "hevc", 4) || !std::memcmp(brand, "hevx", 4)))
                    return false;
            }
        } else if (!std::memcmp(type, "meta", 4)) {
            if (count < prefix + 4) return false;
            for (size_t child = offset + prefix + 4; child + 8 <= end;) {
                const uint32_t length = AV_RB32(bytes + child);
                if (length < 8 || length > end - child) return false;
                if (!std::memcmp(bytes + child + 4, "pitm", 4) && length >= 14)
                    primary = bytes[child + 8] == 0 ? AV_RB16(bytes + child + 12) != 0
                                                    : length >= 16 && AV_RB32(bytes + child + 12) != 0;
                child += length;
            }
        }
        offset = end;
    }
    if (!still || !primary) return false;
    header.info.format = avif ? MAI_IMAGE_FORMAT_AVIF : MAI_IMAGE_FORMAT_HEIF;
    header.demuxer = "mov";
    return true;
}

int parseRasterHeader(const unsigned char* bytes, size_t size, bool complete, const MaiImageReadOptions& options,
                      RasterHeader& header) {
    header.info.orientation = 1;
    int status = 0;
    if (size >= 8 && !std::memcmp(bytes, "\x89PNG\r\n\x1a\n", 8)) {
        header.info.format = MAI_IMAGE_FORMAT_PNG;
        header.demuxer = "png_pipe";
        header.codec = AV_CODEC_ID_PNG;
        status = pngHeader(bytes, size, complete, header, options);
    } else if (size >= 3 && bytes[0] == 0xff && bytes[1] == 0xd8 && bytes[2] == 0xff) {
        header.info.format = MAI_IMAGE_FORMAT_JPEG;
        header.demuxer = "jpeg_pipe";
        header.codec = AV_CODEC_ID_MJPEG;
        status = jpegHeader(bytes, size, complete, header, options);
    } else if (size >= 12 && !std::memcmp(bytes, "RIFF", 4) && !std::memcmp(bytes + 8, "WEBP", 4)) {
        header.info.format = MAI_IMAGE_FORMAT_WEBP;
        header.demuxer = "webp_pipe";
        header.codec = AV_CODEC_ID_WEBP;
        status = webpHeader(bytes, size, complete, header, options);
    } else if (size >= 26 && !std::memcmp(bytes, "BM", 2)) {
        header.info.format = MAI_IMAGE_FORMAT_BMP;
        header.demuxer = "bmp_pipe";
        header.codec = AV_CODEC_ID_BMP;
        const uint32_t dib = AV_RL32(bytes + 14);
        if (dib == 12) {
            header.info.width = AV_RL16(bytes + 18);
            header.info.height = AV_RL16(bytes + 20);
        } else if (size >= 54 && dib >= 40 && dib <= size - 14) {
            const int32_t width = int32_t(AV_RL32(bytes + 18));
            const int32_t height = int32_t(AV_RL32(bytes + 22));
            if (width <= 0 || height == INT_MIN) return AVERROR_INVALIDDATA;
            header.info.width = width;
            header.info.height = std::abs(height);
        } else
            return AVERROR_INVALIDDATA;
    } else if (size >= 13 && (!std::memcmp(bytes, "GIF87a", 6) || !std::memcmp(bytes, "GIF89a", 6))) {
        header.info.format = MAI_IMAGE_FORMAT_GIF;
        header.demuxer = "gif";
        header.codec = AV_CODEC_ID_GIF;
        header.info.width = AV_RL16(bytes + 6);
        header.info.height = AV_RL16(bytes + 8);
    } else if (size >= 22 && AV_RL16(bytes) == 0 && AV_RL16(bytes + 2) == 1 && AV_RL16(bytes + 4) > 0) {
        header.info.format = MAI_IMAGE_FORMAT_ICO;
        header.demuxer = "ico";
        const unsigned int count = AV_RL16(bytes + 4);
        if (count > (size - 6) / 16) return AVERROR_INVALIDDATA;
        for (unsigned int index = 0; index < count; ++index) {
            const auto* entry = bytes + 6 + index * 16;
            const int width = entry[0] ? entry[0] : 256;
            const int height = entry[1] ? entry[1] : 256;
            if (width * height > header.info.width * header.info.height) {
                header.info.width = width;
                header.info.height = height;
                header.stream = static_cast<int>(index);
            }
        }
    } else if (size >= 8 && (!std::memcmp(bytes, "II\x2a\0", 4) || !std::memcmp(bytes, "MM\0\x2a", 4))) {
        header.info.format = MAI_IMAGE_FORMAT_TIFF;
        header.demuxer = "tiff_pipe";
        header.codec = AV_CODEC_ID_TIFF;
        const bool little = bytes[0] == 'I';
        const auto get16 = [little](const unsigned char* p) { return little ? AV_RL16(p) : AV_RB16(p); };
        const auto get32 = [little](const unsigned char* p) { return little ? AV_RL32(p) : AV_RB32(p); };
        const uint32_t first = get32(bytes + 4);
        if (first > size - 2) return AVERROR_INVALIDDATA;
        const unsigned int count = get16(bytes + first);
        if (count > (size - first - 2) / 12) return AVERROR_INVALIDDATA;
        for (unsigned int index = 0; index < count; ++index) {
            const auto* entry = bytes + first + 2 + index * 12;
            if (get32(entry + 4) != 1) continue;
            const unsigned int kind = get16(entry + 2);
            const uint32_t value = kind == 3 ? get16(entry + 8) : kind == 4 ? get32(entry + 8) : 0;
            if (value > INT_MAX) return AVERROR_INVALIDDATA;
            if (get16(entry) == 0x100) header.info.width = int(value);
            if (get16(entry) == 0x101) header.info.height = int(value);
            if (get16(entry) == 0x112 && value >= 1 && value <= 8) header.info.orientation = int(value);
        }
    } else if (size >= 3 && bytes[0] == 'P' && bytes[1] >= '1' && bytes[1] <= '6' &&
               (bytes[2] == ' ' || bytes[2] == '\n' || bytes[2] == '\r' || bytes[2] == '\t')) {
        header.info.format = MAI_IMAGE_FORMAT_PNM;
        const int kind = bytes[1] - '0';
        header.demuxer = kind == 1 || kind == 4 ? "pbm_pipe" : kind == 2 || kind == 5 ? "pgm_pipe" : "ppm_pipe";
        header.codec = kind == 1 || kind == 4   ? AV_CODEC_ID_PBM
                       : kind == 2 || kind == 5 ? AV_CODEC_ID_PGM
                                                : AV_CODEC_ID_PPM;
        size_t offset = 2;
        for (int axis = 0; axis < 2; ++axis) {
            while (offset < size) {
                if (bytes[offset] == '#') {
                    while (offset < size && bytes[offset] != '\n') ++offset;
                } else if (bytes[offset] == ' ' || bytes[offset] == '\t' || bytes[offset] == '\r' ||
                           bytes[offset] == '\n')
                    ++offset;
                else
                    break;
            }
            uint64_t value = 0;
            bool digit = false;
            while (offset < size && bytes[offset] >= '0' && bytes[offset] <= '9') {
                digit = true;
                value = value * 10 + bytes[offset++] - '0';
                if (value > INT_MAX) return AVERROR_INVALIDDATA;
            }
            if (!digit) return AVERROR_INVALIDDATA;
            (axis == 0 ? header.info.width : header.info.height) = int(value);
        }
    } else if (size >= 42 && AV_RB32(bytes) == 0xff4fff51) {
        header.info.format = MAI_IMAGE_FORMAT_JPEG2000;
        header.demuxer = "j2k_pipe";
        header.codec = AV_CODEC_ID_JPEG2000;
        const int64_t width = int64_t(AV_RB32(bytes + 8)) - AV_RB32(bytes + 16);
        const int64_t height = int64_t(AV_RB32(bytes + 12)) - AV_RB32(bytes + 20);
        if (width <= 0 || width > INT_MAX || height <= 0 || height > INT_MAX) return AVERROR_INVALIDDATA;
        header.info.width = int(width);
        header.info.height = int(height);
    } else if (size >= 12 && AV_RB32(bytes) == 12 && !std::memcmp(bytes + 4, "jP  \r\n\x87\n", 8)) {
        header.info.format = MAI_IMAGE_FORMAT_JPEG2000;
        header.demuxer = "j2k_pipe";
        header.codec = AV_CODEC_ID_JPEG2000;
        // The JP2 image header has fixed fields inside jp2h, before codestream bytes.
        for (size_t offset = 12; offset + 8 <= size;) {
            const uint32_t count = AV_RB32(bytes + offset);
            if (count < 8 || count > size - offset) return AVERROR_INVALIDDATA;
            if (!std::memcmp(bytes + offset + 4, "jp2h", 4)) {
                for (size_t child = offset + 8; child + 22 <= offset + count;) {
                    const uint32_t length = AV_RB32(bytes + child);
                    if (length < 8 || length > offset + count - child) return AVERROR_INVALIDDATA;
                    if (!std::memcmp(bytes + child + 4, "ihdr", 4)) {
                        const uint32_t height = AV_RB32(bytes + child + 8), width = AV_RB32(bytes + child + 12);
                        if (width > INT_MAX || height > INT_MAX) return AVERROR_INVALIDDATA;
                        header.info.width = int(width);
                        header.info.height = int(height);
                        break;
                    }
                    child += length;
                }
                break;
            }
            offset += count;
        }
    } else if (size >= 16 && heifHeader(bytes, size, header)) {
        return 0;  // Dimensions and authoritative irot/imir come from the item metadata.
    } else
        return AVERROR_INVALIDDATA;
    if (status < 0) return status;
    status = checkDimensions(header.info.width, header.info.height, options);
    if (status < 0) return status;
    header.info.display_width = header.info.orientation >= 5 ? header.info.height : header.info.width;
    header.info.display_height = header.info.orientation >= 5 ? header.info.width : header.info.height;
    return 0;
}

int localFileBytes(const char* path, const MaiImageReadOptions& options, std::vector<unsigned char>& bytes) {
    if (!path || !*path) return AVERROR(EINVAL);
    // Resolve only native files. No FFmpeg protocol parser sees this path.
    const char* colon = std::strchr(path, ':');
    const char* slash = std::strpbrk(path, "/\\");
    if (colon && (!slash || colon < slash) && colon != path + 1) return AVERROR(EINVAL);
    if (isCanceled(options)) return AVERROR_EXIT;
#ifdef _WIN32
    const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, nullptr, 0);
    if (!length) return AVERROR(EINVAL);
    std::vector<wchar_t> wide(static_cast<size_t>(length));
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, wide.data(), length)) return AVERROR(EINVAL);
    const HANDLE file = CreateFileW(wide.data(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                    nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return AVERROR(ENOENT);
    struct Close {
        HANDLE file;
        ~Close() {
            CloseHandle(file);
        }
    } close{file};
    LARGE_INTEGER size{};
    if (GetFileType(file) != FILE_TYPE_DISK || !GetFileSizeEx(file, &size) || size.QuadPart <= 0)
        return AVERROR(EINVAL);
    const uint64_t count = static_cast<uint64_t>(size.QuadPart);
#else
    const int file = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (file < 0) return AVERROR(errno);
    struct Close {
        int file;
        ~Close() {
            close(file);
        }
    } closer{file};
    struct stat state{};
    if (fstat(file, &state) || !S_ISREG(state.st_mode) || state.st_size <= 0) return AVERROR(EINVAL);
    const uint64_t count = static_cast<uint64_t>(state.st_size);
#endif
    if (count > options.max_encoded_bytes) return AVERROR(EFBIG);
    bytes.resize(static_cast<size_t>(count));
    size_t offset = 0;
    while (offset < bytes.size()) {
        if (isCanceled(options)) return AVERROR_EXIT;
        const size_t block = std::min<size_t>(64 * 1024, bytes.size() - offset);
#ifdef _WIN32
        DWORD received = 0;
        if (!ReadFile(file, bytes.data() + offset, static_cast<DWORD>(block), &received, nullptr)) return AVERROR(EIO);
#else
        const ssize_t received = read(file, bytes.data() + offset, block);
        if (received < 0) {
            if (errno == EINTR) continue;
            return AVERROR(errno);
        }
#endif
        if (!received) return AVERROR_INVALIDDATA;
        offset += static_cast<size_t>(received);
    }
    unsigned char extra;
#ifdef _WIN32
    DWORD received = 0;
    if (!ReadFile(file, &extra, 1, &received, nullptr)) return AVERROR(EIO);
#else
    const ssize_t received = read(file, &extra, 1);
    if (received < 0) return AVERROR(errno);
#endif
    return received ? AVERROR(EFBIG) : 0;
}

struct RasterInput {
    const unsigned char* bytes;
    size_t size;
    size_t offset = 0;
    AVIOContext* io = nullptr;
    AVFormatContext* format = nullptr;
    ~RasterInput() {
        if (format) avformat_close_input(&format);
        if (io) {
            av_freep(&io->buffer);
            avio_context_free(&io);
        }
    }
};

int memoryRead(void* opaque, uint8_t* target, int count) {
    auto* input = static_cast<RasterInput*>(opaque);
    if (count <= 0) return AVERROR(EINVAL);
    const size_t available = std::min<size_t>(static_cast<size_t>(count), input->size - input->offset);
    if (!available) return AVERROR_EOF;
    std::memcpy(target, input->bytes + input->offset, available);
    input->offset += available;
    return static_cast<int>(available);
}

int64_t memorySeek(void* opaque, int64_t offset, int origin) {
    auto* input = static_cast<RasterInput*>(opaque);
    if (origin == AVSEEK_SIZE) return static_cast<int64_t>(input->size);
    origin &= ~AVSEEK_FORCE;
    const int64_t base = origin == SEEK_SET   ? 0
                         : origin == SEEK_CUR ? static_cast<int64_t>(input->offset)
                         : origin == SEEK_END ? static_cast<int64_t>(input->size)
                                              : -1;
    if (base < 0 || offset < -base || offset > static_cast<int64_t>(input->size) - base) return AVERROR(EINVAL);
    input->offset = static_cast<size_t>(base + offset);
    return base + offset;
}

int denyExternalInput(AVFormatContext*, AVIOContext**, const char*, int, AVDictionary**) {
    return AVERROR(EACCES);
}

int openRasterInput(RasterInput& input, const RasterHeader& header) {
    const AVInputFormat* demuxer = av_find_input_format(header.demuxer);
    if (!demuxer) return AVERROR_DEMUXER_NOT_FOUND;
    auto* buffer = static_cast<unsigned char*>(av_malloc(32768));
    if (!buffer) return AVERROR(ENOMEM);
    input.io = avio_alloc_context(buffer, 32768, 0, &input, memoryRead, nullptr, memorySeek);
    if (!input.io) {
        av_free(buffer);
        return AVERROR(ENOMEM);
    }
    input.format = avformat_alloc_context();
    if (!input.format) return AVERROR(ENOMEM);
    input.format->pb = input.io;
    input.format->flags |= AVFMT_FLAG_CUSTOM_IO;
    input.format->io_open = denyExternalInput;
    input.format->max_streams = 256;
    input.format->probesize = std::min<int64_t>(static_cast<int64_t>(input.size), 1024 * 1024);
    // A forced, signature-checked demuxer cannot dispatch playlists or protocols.
    // Header parsing is enough for image streams; find_stream_info could decode
    // pixels before the allocator's exact source/axis guards are installed.
    return avformat_open_input(&input.format, nullptr, demuxer, nullptr);
}

const AVStreamGroup* primaryGrid(const AVFormatContext* format) {
    for (unsigned int index = 0; index < format->nb_stream_groups; ++index) {
        const AVStreamGroup* group = format->stream_groups[index];
        if (group->type == AV_STREAM_GROUP_PARAMS_TILE_GRID && group->params.tile_grid &&
            (group->disposition & AV_DISPOSITION_DEFAULT))
            return group;
    }
    return nullptr;
}

bool singleAv1Frame(const AVPacket* packet) {
    // Only inspect OBU framing here; all AV1 header/pixel grammar stays in the
    // public FFmpeg parser/decoder. Count hidden/spatial frames too: the parser
    // reports dimensions only for a visible frame in spatial layer zero.
    size_t offset = 0;
    int frames = 0;
    const size_t size = static_cast<size_t>(packet->size);
    while (offset < size) {
        const unsigned int header = packet->data[offset++];
        if (header & 0x81) return false;
        const unsigned int type = (header >> 3) & 15;
        if (!type || type == 7 || type == 8) return false;
        if (type == 3 || type == 6) ++frames;
        if (frames > 1) return false;
        if (header & 4) {
            if (offset >= size) return false;
            const unsigned int extension = packet->data[offset++];
            if (extension & 0x1f) return false;  // No nonzero spatial layer or reserved bits.
        }
        uint64_t bytes = size - offset;
        if (header & 2) {
            bytes = 0;
            bool complete = false;
            for (int index = 0; index < 8 && offset < size; ++index) {
                const unsigned int value = packet->data[offset++];
                bytes |= uint64_t(value & 0x7f) << (index * 7);
                if (!(value & 0x80)) {
                    complete = true;
                    break;
                }
            }
            if (!complete) return false;
        }
        if (bytes > size - offset) return false;
        offset += static_cast<size_t>(bytes);
    }
    return frames == 1;
}

int inspectCodedPacket(const AVCodecParameters* parameters, const AVPacket* packet,
                       const MaiImageReadOptions& options) {
    if (parameters->codec_id != AV_CODEC_ID_AV1 && parameters->codec_id != AV_CODEC_ID_HEVC) return AVERROR_INVALIDDATA;
    if (parameters->codec_id == AV_CODEC_ID_AV1 && !singleAv1Frame(packet)) return AVERROR_INVALIDDATA;
    auto closeParser = [](AVCodecParserContext* value) { av_parser_close(value); };
    auto freeCodec = [](AVCodecContext* value) { avcodec_free_context(&value); };
    std::unique_ptr<AVCodecParserContext, decltype(closeParser)> parser(av_parser_init(parameters->codec_id),
                                                                        closeParser);
    std::unique_ptr<AVCodecContext, decltype(freeCodec)> context(avcodec_alloc_context3(nullptr), freeCodec);
    if (!parser) return AVERROR(ENOSYS);
    if (!context) return AVERROR(ENOMEM);
    int status = avcodec_parameters_to_context(context.get(), parameters);
    if (status < 0) return status;
    parser->flags |= PARSER_FLAG_COMPLETE_FRAMES;
    uint8_t* output = nullptr;
    int outputSize = 0;
    status = av_parser_parse2(parser.get(), context.get(), &output, &outputSize, packet->data, packet->size,
                              AV_NOPTS_VALUE, AV_NOPTS_VALUE, packet->pos);
    if (status < 0) return status;
    // A nonnegative parse return is not success. AV1 does not fill coded_width/
    // height, and avctx can still contain forged ispe dimensions. Use fresh
    // parser dimensions; no decoder or native picture allocator has run yet.
    return checkDimensions(std::max(parser->width, parser->coded_width), std::max(parser->height, parser->coded_height),
                           options);
}

int inspectCodedItems(AVFormatContext* format, const MaiImageReadOptions& options) {
    std::vector<bool> pending(format->nb_streams, false);
    const AVStreamGroup* grid = primaryGrid(format);
    if (grid) {
        for (unsigned int index = 0; index < grid->nb_streams; ++index) {
            const int stream = grid->streams[index]->index;
            if (stream < 0 || stream >= static_cast<int>(pending.size())) return AVERROR_INVALIDDATA;
            pending[stream] = true;
        }
    } else {
        const int stream = av_find_best_stream(format, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
        if (stream < 0) return stream;
        pending[stream] = true;
    }
    size_t remaining = static_cast<size_t>(std::count(pending.begin(), pending.end(), true));
    auto freePacket = [](AVPacket* value) { av_packet_free(&value); };
    std::unique_ptr<AVPacket, decltype(freePacket)> packet(av_packet_alloc(), freePacket);
    if (!packet) return AVERROR(ENOMEM);
    int status;
    while (remaining && (status = av_read_frame(format, packet.get())) >= 0) {
        if (isCanceled(options)) return AVERROR_EXIT;
        const int stream = packet->stream_index;
        if (stream >= 0 && stream < static_cast<int>(pending.size()) && pending[stream]) {
            status = inspectCodedPacket(format->streams[stream]->codecpar, packet.get(), options);
            if (status < 0) return status;
            pending[stream] = false;
            --remaining;
        }
        av_packet_unref(packet.get());
    }
    return remaining ? AVERROR_INVALIDDATA : 0;
}

int inspectItemMetadata(RasterInput& input, RasterHeader& header, const MaiImageReadOptions& options) {
    const AVStreamGroup* grid = primaryGrid(input.format);
    if (grid) {
        header.info.width = grid->params.tile_grid->width;
        header.info.height = grid->params.tile_grid->height;
        header.info.orientation =
            displayRotation(grid->params.tile_grid->coded_side_data, grid->params.tile_grid->nb_coded_side_data);
    } else {
        const AVStream* primary = nullptr;
        for (unsigned int index = 0; index < input.format->nb_streams; ++index) {
            const AVStream* stream = input.format->streams[index];
            if (stream->disposition & AV_DISPOSITION_DEFAULT) {
                primary = stream;
                break;
            }
        }
        if (!primary) return AVERROR_INVALIDDATA;
        header.info.width = primary->codecpar->width;
        header.info.height = primary->codecpar->height;
        header.info.orientation =
            displayRotation(primary->codecpar->coded_side_data, primary->codecpar->nb_coded_side_data);
    }
    const int status = checkDimensions(header.info.width, header.info.height, options);
    header.info.display_width = header.info.orientation >= 5 ? header.info.height : header.info.width;
    header.info.display_height = header.info.orientation >= 5 ? header.info.width : header.info.height;
    return status;
}

int tiffPacketView(AVPacket* packet, int& orientation) {
    if (packet->size < 8) return AVERROR_INVALIDDATA;
    const bool little = packet->data[0] == 'I';
    const auto get16 = [little](const unsigned char* p) { return little ? AV_RL16(p) : AV_RB16(p); };
    const auto get32 = [little](const unsigned char* p) { return little ? AV_RL32(p) : AV_RB32(p); };
    const uint32_t first = get32(packet->data + 4);
    if (first > static_cast<unsigned int>(packet->size - 2)) return AVERROR_INVALIDDATA;
    const unsigned int count = get16(packet->data + first);
    if (packet->size - first - 2 < 4 || count > (packet->size - first - 6) / 12) return AVERROR_INVALIDDATA;
    unsigned int removed = 0;
    for (unsigned int index = 0; index < count; ++index) {
        const auto* entry = packet->data + first + 2 + index * 12;
        if (get16(entry) != 0x112) continue;
        if (get16(entry + 2) != 3 || get32(entry + 4) != 1 || get16(entry + 8) < 1 || get16(entry + 8) > 8)
            return AVERROR_INVALIDDATA;
        orientation = get16(entry + 8);
        ++removed;
    }
    if (!removed) return 0;
    // The pinned strict TIFF decoder rejects the valid Orientation tag. Keep
    // strict pixel/strip errors enabled. Relocate only the first IFD without
    // that display-only tag; original value/pixel offsets remain valid in the
    // private packet, and the shared RGBA stage applies the original EXIF.
    const int originalSize = packet->size;
    const unsigned int kept = count - removed;
    const int status = av_grow_packet(packet, int(kept * 12 + 6));
    if (status < 0) return status;
    auto* table = packet->data + originalSize;
    if (little) {
        AV_WL32(packet->data + 4, originalSize);
        AV_WL16(table, kept);
    } else {
        AV_WB32(packet->data + 4, originalSize);
        AV_WB16(table, kept);
    }
    unsigned int output = 0;
    for (unsigned int index = 0; index < count; ++index) {
        const auto* entry = packet->data + first + 2 + index * 12;
        if (get16(entry) != 0x112) std::memcpy(table + 2 + output++ * 12, entry, 12);
    }
    std::memcpy(table + 2 + kept * 12, packet->data + first + 2 + count * 12, 4);
    return 0;
}

int cropItemFrame(AVFrame* frame, const MaiImageInfo& info) {
    if (info.width < 1 || info.height < 1 || info.width > frame->width || info.height > frame->height)
        return AVERROR_INVALIDDATA;
    // ispe describes the displayed item; codec blocks can contain padded rows.
    frame->crop_right = static_cast<size_t>(frame->width - info.width);
    frame->crop_bottom = static_cast<size_t>(frame->height - info.height);
    return av_frame_apply_cropping(frame, AV_FRAME_CROP_UNALIGNED);
}

// The legacy video-cover entry and strict raster reads use the same frame/grid,
// RGBA, orientation and color-metadata implementation.
MaiImageDecodeResult decodeFormat(AVFormatContext* format, const MaiImageReadOptions& options, MaiImageInfo* info,
                                  int wantedStream = -1) {
    const int max_width = options.max_output_width;
    const int max_height = options.max_output_height;
    int status = 0;
    const AVStreamGroup* largestGrid = nullptr;
    for (unsigned int index = 0; index < format->nb_stream_groups; ++index) {
        const AVStreamGroup* group = format->stream_groups[index];
        if (group->type != AV_STREAM_GROUP_PARAMS_TILE_GRID || !group->params.tile_grid ||
            group->params.tile_grid->nb_tiles <= 1)
            continue;
        if (!largestGrid ||
            static_cast<long long>(group->params.tile_grid->width) * group->params.tile_grid->height >
                static_cast<long long>(largestGrid->params.tile_grid->width) * largestGrid->params.tile_grid->height)
            largestGrid = group;
    }
    if (info && (info->format == MAI_IMAGE_FORMAT_HEIF || info->format == MAI_IMAGE_FORMAT_AVIF))
        largestGrid = primaryGrid(format);
    if (largestGrid) {
        if (info) {
            info->width = largestGrid->params.tile_grid->width;
            info->height = largestGrid->params.tile_grid->height;
            info->orientation = displayRotation(largestGrid->params.tile_grid->coded_side_data,
                                                largestGrid->params.tile_grid->nb_coded_side_data);
            info->display_width = info->orientation >= 5 ? info->height : info->width;
            info->display_height = info->orientation >= 5 ? info->width : info->height;
        }
        return decodeTileGrid(format, largestGrid, max_width, max_height, options);
    }
    const AVCodec* decoder = nullptr;
    const int stream = av_find_best_stream(format, AVMEDIA_TYPE_VIDEO, wantedStream, -1, &decoder, 0);
    if (stream < 0 || !decoder) return failed(stream < 0 ? stream : AVERROR_DECODER_NOT_FOUND);

    DecodeBudget budget{&options};
    auto freeCodec = [](AVCodecContext* value) { avcodec_free_context(&value); };
    std::unique_ptr<AVCodecContext, decltype(freeCodec)> codec(avcodec_alloc_context3(decoder), freeCodec);
    if (!codec) return failed(AVERROR(ENOMEM));
    status = avcodec_parameters_to_context(codec.get(), format->streams[stream]->codecpar);
    if (status < 0) return failed(status);
    constrainCodec(codec.get(), budget);
    status = avcodec_open2(codec.get(), decoder, nullptr);
    if (status < 0) return failed(budget.error < 0 ? budget.error.load() : status);

    auto freePacket = [](AVPacket* value) { av_packet_free(&value); };
    auto freeFrame = [](AVFrame* value) { av_frame_free(&value); };
    std::unique_ptr<AVPacket, decltype(freePacket)> packet(av_packet_alloc(), freePacket);
    std::unique_ptr<AVFrame, decltype(freeFrame)> frame(av_frame_alloc(), freeFrame);
    if (!packet || !frame) return failed(AVERROR(ENOMEM));

    bool decoded = false;
    int orientationHint = info ? info->orientation : 1;
    while ((status = av_read_frame(format, packet.get())) >= 0) {
        if (isCanceled(options)) return failed(AVERROR_EXIT);
        if (packet->stream_index != stream) {
            av_packet_unref(packet.get());
            continue;
        }
        if (codec->codec_id == AV_CODEC_ID_TIFF) {
            status = tiffPacketView(packet.get(), orientationHint);
            if (status < 0) return failed(status);
        }
        status = avcodec_send_packet(codec.get(), packet.get());
        av_packet_unref(packet.get());
        if (status < 0) return failed(budget.error < 0 ? budget.error.load() : status);
        status = avcodec_receive_frame(codec.get(), frame.get());
        if (status == AVERROR(EAGAIN)) continue;
        if (status < 0) return failed(budget.error < 0 ? budget.error.load() : status);
        decoded = true;
        break;
    }
    if (!decoded) {
        status = avcodec_send_packet(codec.get(), nullptr);
        if (status < 0) return failed(budget.error < 0 ? budget.error.load() : status);
        status = avcodec_receive_frame(codec.get(), frame.get());
        if (status < 0) return failed(budget.error < 0 ? budget.error.load() : status);
    }

    const int codedError = checkDimensions(frame->width, frame->height, options);
    if (codedError < 0) return failed(codedError);
    if (info && (info->format == MAI_IMAGE_FORMAT_HEIF || info->format == MAI_IMAGE_FORMAT_AVIF)) {
        status = cropItemFrame(frame.get(), *info);
        if (status < 0) return failed(status);
    }
    const int sourceWidth = frame->width;
    const int sourceHeight = frame->height;
    if (isCanceled(options)) return failed(AVERROR_EXIT);
    const int dimensionsError = checkDimensions(sourceWidth, sourceHeight, options);
    if (dimensionsError < 0) return failed(dimensionsError);
    if (frame->decode_error_flags) return failed(AVERROR_INVALIDDATA);

    const AVFrameSideData* frameMatrix = av_frame_get_side_data(frame.get(), AV_FRAME_DATA_DISPLAYMATRIX);
    int rotation = frameMatrix ? orientationFromMatrix(frameMatrix->data, frameMatrix->size)
                               : displayRotation(format->streams[stream]->codecpar->coded_side_data,
                                                 format->streams[stream]->codecpar->nb_coded_side_data);
    if (!frameMatrix && rotation == 1 &&
        (!info || (info->format != MAI_IMAGE_FORMAT_HEIF && info->format != MAI_IMAGE_FORMAT_AVIF)))
        rotation = orientationHint;
    const int displayWidth = rotation >= 5 ? sourceHeight : sourceWidth;
    const int displayHeight = rotation >= 5 ? sourceWidth : sourceHeight;
    if (info) {
        info->width = sourceWidth;
        info->height = sourceHeight;
        info->display_width = displayWidth;
        info->display_height = displayHeight;
        info->orientation = rotation;
    }
    double ratio = 1.0;
    if (max_width > 0) ratio = std::min(ratio, static_cast<double>(max_width) / displayWidth);
    if (max_height > 0) ratio = std::min(ratio, static_cast<double>(max_height) / displayHeight);
    const int width = std::max(1, static_cast<int>(std::floor(sourceWidth * ratio)));
    const int height = std::max(1, static_cast<int>(std::floor(sourceHeight * ratio)));
    if (static_cast<long long>(width) * height > kMaximumPixels || width > std::numeric_limits<int>::max() / 4)
        return failed(AVERROR(EINVAL));

    auto freeScale = [](SwsContext* value) { sws_freeContext(value); };
    // Keep constant gray/RGB and alpha stable when reducing a very large raster.
    std::unique_ptr<SwsContext, decltype(freeScale)> scale(
        sws_getContext(sourceWidth, sourceHeight, scalePixelFormat(static_cast<AVPixelFormat>(frame->format)), width,
                       height, AV_PIX_FMT_RGBA, SWS_LANCZOS | SWS_ACCURATE_RND, nullptr, nullptr, nullptr),
        freeScale);
    if (!scale) return failed(AVERROR(ENOSYS));
    if (configureColor(scale.get(), frame.get()) < 0) return failed(AVERROR(EINVAL));

    const int stride = width * 4;
    auto pixels = std::unique_ptr<unsigned char, decltype(&std::free)>(
        static_cast<unsigned char*>(std::malloc(static_cast<size_t>(stride) * height)), &std::free);
    if (!pixels) return failed(AVERROR(ENOMEM));
    unsigned char* output[] = {pixels.get(), nullptr, nullptr, nullptr};
    const int outputLines[] = {stride, 0, 0, 0};
    status = sws_scale(scale.get(), frame->data, frame->linesize, 0, sourceHeight, output, outputLines);
    if (status != height) return failed(status < 0 ? status : AVERROR_INVALIDDATA);
    MaiImageDecodeResult image =
        orientRgba({pixels.release(), width, height, stride, 0, nullptr, 0, 0, 0}, rotation, options);
    const AVFrameSideData* frameIcc = av_frame_get_side_data(frame.get(), AV_FRAME_DATA_ICC_PROFILE);
    const AVPacketSideData* codedIcc = packetIcc(format->streams[stream]->codecpar->coded_side_data,
                                                 format->streams[stream]->codecpar->nb_coded_side_data);
    const unsigned char* profile = frameIcc ? frameIcc->data : codedIcc ? codedIcc->data : nullptr;
    const size_t profileSize = frameIcc ? frameIcc->size : codedIcc ? codedIcc->size : 0;
    const int primaries = frame->color_primaries != AVCOL_PRI_UNSPECIFIED
                              ? frame->color_primaries
                              : format->streams[stream]->codecpar->color_primaries;
    const int transfer =
        frame->color_trc != AVCOL_TRC_UNSPECIFIED ? frame->color_trc : format->streams[stream]->codecpar->color_trc;
    return attachColorMetadata(image, profile, profileSize, primaries, transfer);
}

int prepareRaster(const unsigned char* bytes, size_t size, bool complete, const MaiImageReadOptions& options,
                  RasterHeader& header) {
    if (!bytes || !size) return AVERROR_INVALIDDATA;
    if (size > options.max_encoded_bytes) return AVERROR(EFBIG);
    if (isCanceled(options)) return AVERROR_EXIT;
    return parseRasterHeader(bytes, size, complete, options, header);
}

MaiImageInfo inspectRaster(const unsigned char* bytes, size_t size, const MaiImageReadOptions& options) {
    RasterHeader header;
    int status = prepareRaster(bytes, size, false, options, header);
    if (status < 0) return failedInfo(status);
    if (header.info.format == MAI_IMAGE_FORMAT_HEIF || header.info.format == MAI_IMAGE_FORMAT_AVIF) {
        RasterInput input{bytes, size};
        status = openRasterInput(input, header);
        if (status >= 0) status = inspectItemMetadata(input, header, options);
        if (status >= 0) status = inspectCodedItems(input.format, options);
        if (status < 0) return failedInfo(status);
    }
    if (isCanceled(options)) return failedInfo(AVERROR_EXIT);
    return header.info;
}

MaiImageDecodeResult readRaster(const unsigned char* bytes, size_t size, const MaiImageReadOptions& options,
                                MaiImageInfo* outInfo) {
    RasterHeader header;
    int status = prepareRaster(bytes, size, true, options, header);
    if (status < 0) return failed(status);
    RasterInput input{bytes, size};
    status = openRasterInput(input, header);
    if (status < 0) return failed(status);
    if (header.info.format == MAI_IMAGE_FORMAT_HEIF || header.info.format == MAI_IMAGE_FORMAT_AVIF) {
        status = inspectItemMetadata(input, header, options);
        if (status < 0) return failed(status);
        status = inspectCodedItems(input.format, options);
        if (status < 0) return failed(status);
    }
    // Signature, container and codec must agree. No best-video-stream success
    // can turn an arbitrary media container into an accepted user bitmap.
    for (unsigned int index = 0; index < input.format->nb_streams; ++index) {
        const AVCodecID codec = input.format->streams[index]->codecpar->codec_id;
        if (header.codec != AV_CODEC_ID_NONE && codec != header.codec) return failed(AVERROR_INVALIDDATA);
        if (header.info.format == MAI_IMAGE_FORMAT_ICO && codec != AV_CODEC_ID_PNG && codec != AV_CODEC_ID_BMP)
            return failed(AVERROR_INVALIDDATA);
        if ((header.info.format == MAI_IMAGE_FORMAT_HEIF || header.info.format == MAI_IMAGE_FORMAT_AVIF) &&
            codec != AV_CODEC_ID_HEVC && codec != AV_CODEC_ID_AV1)
            return failed(AVERROR_INVALIDDATA);
    }
    if (isCanceled(options)) return failed(AVERROR_EXIT);
    const MaiImageInfo inspected = header.info;
    // Item inspection consumes packets, so open a fresh view of the same owning
    // snapshot for decode. No second file open or mutable metadata is trusted.
    const bool itemImage = header.info.format == MAI_IMAGE_FORMAT_HEIF || header.info.format == MAI_IMAGE_FORMAT_AVIF;
    RasterInput decodeInput{bytes, size};
    if (itemImage) {
        status = openRasterInput(decodeInput, header);
        if (status < 0) return failed(status);
    }
    MaiImageDecodeResult image =
        decodeFormat(itemImage ? decodeInput.format : input.format, options, &header.info, header.stream);
    if (image.error_code == 0 && (header.info.width != inspected.width || header.info.height != inspected.height)) {
        maiImageDecodeFree(image.rgba);
        return failed(AVERROR_INVALIDDATA);
    }
    if (image.error_code == 0 && isCanceled(options)) {
        maiImageDecodeFree(image.rgba);
        return failed(AVERROR_EXIT);
    }
    if (image.error_code == 0 && outInfo) *outInfo = header.info;
    return image;
}

}  // namespace

extern "C" MaiImageInfo maiImageInspectMemory(const unsigned char* bytes, size_t size,
                                              const MaiImageReadOptions* supplied) {
    try {
        MaiImageReadOptions options{};
        const int status = readOptions(supplied, options);
        return status < 0 ? failedInfo(status) : inspectRaster(bytes, size, options);
    } catch (const std::bad_alloc&) {
        return failedInfo(AVERROR(ENOMEM));
    }
}

extern "C" MaiImageInfo maiImageInspectFile(const char* path, const MaiImageReadOptions* supplied) {
    try {
        MaiImageReadOptions options{};
        int status = readOptions(supplied, options);
        if (status < 0) return failedInfo(status);
        std::vector<unsigned char> bytes;
        status = localFileBytes(path, options, bytes);
        return status < 0 ? failedInfo(status) : inspectRaster(bytes.data(), bytes.size(), options);
    } catch (const std::bad_alloc&) {
        return failedInfo(AVERROR(ENOMEM));
    }
}

extern "C" MaiImageDecodeResult maiImageReadMemory(const unsigned char* bytes, size_t size,
                                                   const MaiImageReadOptions* supplied, MaiImageInfo* outInfo) {
    MaiImageDecodeResult image;
    try {
        MaiImageReadOptions options{};
        const int status = readOptions(supplied, options);
        image = status < 0 ? failed(status) : readRaster(bytes, size, options, outInfo);
    } catch (const std::bad_alloc&) {
        image = failed(AVERROR(ENOMEM));
    }
    if (image.error_code < 0 && outInfo) *outInfo = failedInfo(image.error_code);
    return image;
}

extern "C" MaiImageDecodeResult maiImageReadFile(const char* path, const MaiImageReadOptions* supplied,
                                                 MaiImageInfo* outInfo) {
    MaiImageDecodeResult image;
    try {
        MaiImageReadOptions options{};
        int status = readOptions(supplied, options);
        std::vector<unsigned char> bytes;
        if (status >= 0) status = localFileBytes(path, options, bytes);
        image = status < 0 ? failed(status) : readRaster(bytes.data(), bytes.size(), options, outInfo);
    } catch (const std::bad_alloc&) {
        image = failed(AVERROR(ENOMEM));
    }
    if (image.error_code < 0 && outInfo) *outInfo = failedInfo(image.error_code);
    return image;
}

extern "C" MaiImageDecodeResult maiImageDecodeFile(const char* path, int max_width, int max_height) {
    if (!path || !*path || max_width < 0 || max_height < 0) return failed(AVERROR(EINVAL));
    MaiImageReadOptions options{};
    options.max_source_pixels = kMaximumPixels;
    options.max_output_width = max_width;
    options.max_output_height = max_height;
    AVFormatContext* raw = nullptr;
    int status = avformat_open_input(&raw, path, nullptr, nullptr);
    if (status < 0) return failed(status);
    auto close = [](AVFormatContext* value) { avformat_close_input(&value); };
    std::unique_ptr<AVFormatContext, decltype(close)> format(raw, close);
    // Stream-info may itself decode. Give every probe decoder the same bounded
    // pixel budget before it is opened, rather than checking only its output.
    std::vector<AVDictionary*> dictionaries(format->nb_streams, nullptr);
    for (auto& dictionary : dictionaries) {
        av_dict_set_int(&dictionary, "max_pixels", kMaximumPixels, 0);
        av_dict_set(&dictionary, "threads", "1", 0);
    }
    status = avformat_find_stream_info(format.get(), dictionaries.data());
    for (auto& dictionary : dictionaries) av_dict_free(&dictionary);
    if (status < 0) return failed(status);
    return decodeFormat(format.get(), options, nullptr);
}

extern "C" void maiImageDecodeFree(void* pixels) {
    std::free(pixels);
}
