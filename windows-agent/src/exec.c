/**
 * @file exec.c
 * @brief Install execution (Windows) — stub.
 *
 * Step 2 (이 파일): EXEC_ERR_INTERNAL 반환. 실제 구현은 후속 commit
 * (CreateProcessW + Job Object 컨테이너 + minimal env block + anonymous
 * pipe stdout/stderr 4KB tail + wall-clock timeout via WaitForSingleObject).
 */

#include "exec.h"

#include <string.h>

exec_status_t exec_install(exec_install_type_t  type,
                           const char          *work_dir,
                           const char          *target_file,
                           const char         **argv_extra,
                           int                  timeout_sec,
                           int                  mem_limit_mb,
                           int                  fsize_limit_mb,
                           int                  active_proc_limit,
                           const char          *task_id,
                           const char          *machine_id,
                           exec_result_t       *out)
{
	(void)type; (void)work_dir; (void)target_file; (void)argv_extra;
	(void)timeout_sec; (void)mem_limit_mb; (void)fsize_limit_mb;
	(void)active_proc_limit; (void)task_id; (void)machine_id;
	if (out) memset(out, 0, sizeof *out);
	return EXEC_ERR_INTERNAL;
}
