/**
 * @file util.h
 * @brief Process / environment utilities shared by collect and publish.
 *
 * Domain logic (AMQP, payload schema) does not belong here.
 */

#ifndef ASSESSMENT_AGENT_UTIL_H
#define ASSESSMENT_AGENT_UTIL_H

#include <stddef.h>
#include <time.h>

/**
 * @brief Run a shell command and capture stdout into a malloc'd string.
 *
 * Returns NULL if popen fails, the child exits abnormally, exit status != 0,
 * or memory cannot be allocated. Caller frees with free().
 */
char *run_cmd(const char *cmd);

/**
 * @brief Read an entire file into a malloc'd null-terminated string.
 *
 * Returns NULL on open failure or OOM. Caller frees with free().
 */
char *read_file_all(const char *path);

/**
 * @brief Load a .env-style file into the process environment.
 *
 * Existing variables are not overwritten (shell env wins). Missing files are
 * ignored silently. Supports `KEY=VALUE`, single-quoted, double-quoted, and
 * `# comment` lines.
 */
void load_env_file(const char *path);

/**
 * @brief getenv() wrapper. Returns @p fallback if the variable is unset or empty.
 */
const char *getenv_default(const char *name, const char *fallback);

/**
 * @brief Trim ASCII whitespace from both ends of @p s in place.
 */
char *trim_inplace(char *s);

/**
 * @brief Format @p t as ISO 8601 UTC into @p buf (size >= 21).
 *
 * Output: `YYYY-MM-DDTHH:MM:SSZ`. Returns @p buf.
 */
char *iso8601_utc(time_t t, char *buf, size_t len);

/**
 * @brief Format @p ts as ISO 8601 UTC with millisecond precision into @p buf
 *        (size >= 25).
 *
 * Output: `YYYY-MM-DDTHH:MM:SS.sssZ`. Returns @p buf.
 */
char *iso8601_utc_ms(struct timespec ts, char *buf, size_t len);

/**
 * @brief Generate a UUID v4 string into @p buf (size >= 37).
 *
 * Reads /proc/sys/kernel/random/uuid first; if that fails, builds a synthetic
 * value from hostname/pid/timestamp.
 */
char *uuid_v4(char *buf, size_t len);

#endif
