#ifndef MAI_FFTOOLS_EMBED_H
#define MAI_FFTOOLS_EMBED_H

#include <stddef.h>
#include <stdarg.h>

#ifdef __cplusplus
extern "C" {
#endif

int mai_ffmpeg_execute(int argc, char **argv);
void mai_ffmpeg_set_cancel_check(int (*check)(void *opaque), void *opaque);
int mai_ffprobe_execute(int argc, char **argv);
void mai_ffprobe_set_cancel_check(int (*check)(void *opaque), void *opaque);
void mai_ffprobe_show_help_default(const char *opt, const char *arg);
void mai_fftools_set_log_sink(void (*sink)(void *opaque, const char *line), void *opaque);
/* Human-readable command output only; never media packets or ffprobe JSON.
 * bytes is borrowed for this synchronous callback and is not NUL-dependent.
 * The host serializes calls, joins command workers, then clears both sinks.
 * A callback must not throw across this C boundary. */
enum {
    MAI_FFTOOLS_TEXT_TRUNCATED = 1u,
    MAI_FFTOOLS_TEXT_ERROR = 2u
};
void mai_fftools_set_text_sink(void (*sink)(void *opaque, const char *bytes,
                                          size_t length, unsigned flags), void *opaque);
int mai_fftools_text_printf(const char *format, ...);
int mai_fftools_text_vprintf(const char *format, va_list arguments);
int mai_fftools_error_string(int code, char *buffer, size_t size);

#ifdef __cplusplus
}
#endif

#endif
