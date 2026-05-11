/**
 * @file exec.h
 * @brief Sandboxed execve of `install.sh` (EC1 environment).
 *
 * Performs fork + execve with a hardened child setup:
 *   - clearenv() then whitelist-rebuild (PATH, HOME, USER, LANG, TASK_ID,
 *     MACHINE_ID). Parent's RABBITMQ_* / .env contents are NEVER inherited.
 *   - chdir(extraction_dir) → install.sh's CWD is the unpacked package.
 *   - setsid() → child becomes its own process-group leader so wall-clock
 *     timeout / SIGTERM cleanup can be targeted via killpg().
 *   - stdin redirected to /dev/null; stdout + stderr piped to ring buffers
 *     that retain only the **last** 4096 bytes each (the tails surfaced in
 *     task.result).
 *   - setrlimit(NOFILE/AS/FSIZE/CPU) caps. CPU cap mirrors the wall-clock
 *     timeout so a CPU-bound runaway is also bounded.
 *   - Wall-clock timeout enforced by parent via timed waitpid loop: at
 *     timeout_sec → SIGTERM(pgid), +5s → SIGKILL(pgid).
 */

#ifndef ASSESSMENT_AGENT_EXEC_H
#define ASSESSMENT_AGENT_EXEC_H

#include <stddef.h>
#include <sys/types.h>

typedef enum {
	EXEC_OK = 0,                 /* install.sh exit_code == 0 */
	EXEC_ERR_SCRIPT_FAILED,      /* install.sh exit_code != 0 */
	EXEC_ERR_SCRIPT_TIMEOUT,     /* wall-clock timeout — killed */
	EXEC_ERR_SCRIPT_NOT_FOUND,   /* extract_dir/script does not exist or not regular */
	EXEC_ERR_INTERNAL,           /* fork / pipe / setrlimit failure */
} exec_status_t;

/**
 * @brief Per-invocation result, populated for every non-INTERNAL outcome.
 *
 * `stdout_tail` and `stderr_tail` always hold up to 4096 bytes (the most
 * recent slice if the script wrote more). Output is sanitized: bytes
 * outside ASCII printable + tab/newline range are replaced with '?' so
 * downstream JSON encoding stays valid without per-character escaping.
 */
typedef struct {
	int   exit_code;             /* -1 if killed by signal */
	int   signal_no;             /* signal that killed the process, else 0 */
	long  duration_ms;
	char  stdout_tail[4096];
	char  stderr_tail[4096];
} exec_result_t;

/**
 * @brief Fork + execve `extract_dir/script` with the EC1 environment.
 *
 * @param extract_dir       Sandbox directory. Must exist; becomes child CWD.
 * @param script            Path within @p extract_dir to execute.
 * @param argv_extra        Additional argv entries (NULL-terminated). May be NULL.
 * @param timeout_sec       Wall-clock timeout. <= 0 disables.
 * @param mem_limit_mb      RLIMIT_AS in MB. <= 0 leaves the system default.
 * @param fsize_limit_mb    RLIMIT_FSIZE in MB. <= 0 leaves the system default.
 * @param task_id           Set as TASK_ID env in the child.
 * @param machine_id        Set as MACHINE_ID env in the child.
 * @param out               Populated on every non-INTERNAL return.
 */
exec_status_t exec_install_script(const char  *extract_dir,
                                  const char  *script,
                                  const char **argv_extra,
                                  int          timeout_sec,
                                  int          mem_limit_mb,
                                  int          fsize_limit_mb,
                                  const char  *task_id,
                                  const char  *machine_id,
                                  exec_result_t *out);

#endif
