#include "mai_fftools_embed.h"

#include <stdarg.h>
#include <stdio.h>

#include "libavutil/error.h"
#include "libavutil/log.h"

static void (*mai_log_sink)(void *opaque, const char *line);
static void *mai_log_opaque;
static void (*mai_text_sink)(void *opaque, const char *bytes, size_t length, unsigned flags);
static void *mai_text_opaque;
static int mai_saved_log_level;
static int mai_has_saved_log_level;

static void mai_log_callback(void *context, int level, const char *format, va_list arguments)
{
    if (!mai_log_sink || level > AV_LOG_INFO)
        return;

    char line[1024];
    int print_prefix = 1;
    av_log_format_line2(context, level, format, arguments, line, sizeof(line), &print_prefix);
    mai_log_sink(mai_log_opaque, line);
}

void mai_fftools_set_log_sink(void (*sink)(void *opaque, const char *line), void *opaque)
{
    if (sink && !mai_log_sink) {
        mai_saved_log_level = av_log_get_level();
        mai_has_saved_log_level = 1;
    }
    mai_log_opaque = opaque;
    mai_log_sink = sink;
    av_log_set_callback(sink ? mai_log_callback : av_log_default_callback);
    if (!sink && mai_has_saved_log_level) {
        av_log_set_level(mai_saved_log_level);
        mai_has_saved_log_level = 0;
    }
}

void mai_fftools_set_text_sink(void (*sink)(void *opaque, const char *bytes,
                                          size_t length, unsigned flags), void *opaque)
{
    mai_text_opaque = opaque;
    mai_text_sink = sink;
}

int mai_fftools_text_vprintf(const char *format, va_list arguments)
{
    /* Bound formatting itself, not just the host's final response buffer. */
    char text[4096];
    int needed;
    size_t length;
    unsigned flags = 0;
    if (!mai_text_sink)
        return vfprintf(stdout, format, arguments);
    needed = vsnprintf(text, sizeof(text), format, arguments);
    if (needed < 0) {
        mai_text_sink(mai_text_opaque, NULL, 0, MAI_FFTOOLS_TEXT_ERROR);
        return needed;
    }
    length = (size_t)needed;
    if (length >= sizeof(text)) {
        length = sizeof(text) - 1;
        flags |= MAI_FFTOOLS_TEXT_TRUNCATED;
    }
    mai_text_sink(mai_text_opaque, text, length, flags);
    return needed;
}

int mai_fftools_text_printf(const char *format, ...)
{
    int result;
    va_list arguments;
    va_start(arguments, format);
    result = mai_fftools_text_vprintf(format, arguments);
    va_end(arguments);
    return result;
}

int mai_fftools_error_string(int code, char *buffer, size_t size)
{
    return av_strerror(code, buffer, size);
}
