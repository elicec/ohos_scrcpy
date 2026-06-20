/*
 * OHOS Scrcpy - 日志工具
 */

#ifndef OHOS_SCRCPY_LOG_H
#define OHOS_SCRCPY_LOG_H

#include <stdio.h>
#include <stdarg.h>
#include <time.h>

typedef enum {
    LOG_DEBUG = 0,
    LOG_INFO,
    LOG_WARN,
    LOG_ERROR,
} LogLevel;

#ifdef __cplusplus
extern "C" {
#endif

static inline void log_print(LogLevel level, const char *tag, const char *fmt, ...) {
    const char *level_str[] = {"DEBUG", "INFO", "WARN", "ERROR"};

    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    char time_buf[32];
    strftime(time_buf, sizeof(time_buf), "%H:%M:%S", t);

    fprintf(stderr, "[%s][%s][%s] ", time_buf, level_str[level], tag);

    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);

    fprintf(stderr, "\n");
}

#define LOG_TAG "OHScrcpy"

#define LOGD(fmt, ...) log_print(LOG_DEBUG, LOG_TAG, fmt, ##__VA_ARGS__)
#define LOGI(fmt, ...) log_print(LOG_INFO,  LOG_TAG, fmt, ##__VA_ARGS__)
#define LOGW(fmt, ...) log_print(LOG_WARN,  LOG_TAG, fmt, ##__VA_ARGS__)
#define LOGE(fmt, ...) log_print(LOG_ERROR, LOG_TAG, fmt, ##__VA_ARGS__)

#define LOG_TAG_D(tag, fmt, ...) log_print(LOG_DEBUG, tag, fmt, ##__VA_ARGS__)
#define LOG_TAG_I(tag, fmt, ...) log_print(LOG_INFO,  tag, fmt, ##__VA_ARGS__)
#define LOG_TAG_W(tag, fmt, ...) log_print(LOG_WARN,  tag, fmt, ##__VA_ARGS__)
#define LOG_TAG_E(tag, fmt, ...) log_print(LOG_ERROR, tag, fmt, ##__VA_ARGS__)

#ifdef __cplusplus
}
#endif

#endif /* OHOS_SCRCPY_LOG_H */
