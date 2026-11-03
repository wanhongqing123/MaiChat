// 文件作用：验证真实静态库、mtmd 公共 ABI 及后端生命周期；不加载或下载模型。
#include "llama.h"
#include "mtmd-helper.h"
#include "mtmd.h"

#include <cstdio>
#include <cstring>

int main() {
    llama_backend_init();
    const auto language = llama_context_default_params();
    const auto vision = mtmd_context_params_default();
    // A one-pixel PNG pulls the real helper/STB/hash code into each target link.
    const unsigned char png[] = {
        0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x48,
        0x44, 0x52, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x08, 0x04, 0x00, 0x00,
        0x00, 0xb5, 0x1c, 0x0c, 0x02, 0x00, 0x00, 0x00, 0x0b, 0x49, 0x44, 0x41, 0x54, 0x78,
        0xda, 0x63, 0x64, 0xf8, 0x0f, 0x00, 0x01, 0x05, 0x01, 0x01, 0x27, 0x18, 0xe3, 0x66,
        0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4e, 0x44, 0xae, 0x42, 0x60, 0x82};
    auto image = mtmd_helper_bitmap_init_from_buf(nullptr, png, sizeof(png), false,
                                                  mtmd_helper_init_opt_default());
    const char* commit = ggml_commit();
    const bool valid = language.n_seq_max > 0 && vision.n_threads > 0 && commit &&
                       std::strcmp(commit, MAI_LLAMA_SOURCE_COMMIT) == 0 && image.bitmap &&
                       !image.video_ctx && mtmd_bitmap_get_nx(image.bitmap) == 1 &&
                       mtmd_bitmap_get_ny(image.bitmap) == 1;
    std::printf("llama.cpp b12065 source=%s mtmd_threads=%d context_sequences=%u\n",
                commit ? commit : "unknown", vision.n_threads, language.n_seq_max);
    if (image.bitmap) mtmd_bitmap_free(image.bitmap);
    llama_backend_free();
    return valid ? 0 : 1;
}
