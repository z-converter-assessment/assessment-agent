/**
 * @file worker.c
 * @brief task.install consumer + fork/reap + idempotency state machine.
 *
 * State (per ctx):
 *   IDLE  — no child in flight. tick polls basic_get on the per-machine queue.
 *           If empty → return. If message → fork child → enter BUSY.
 *   BUSY  — child PID set. tick reaps non-blocking; on reap → read result
 *           file (or synthesize on abnormal exit) → publish task.result →
 *           ack broker → move result file to `/done` → enter IDLE.
 *   DRAIN — SIGTERM observed. No new basic_get. tick reaps the in-flight
 *           child if any, then waits idle.
 *
 * Idempotency:
 *   /var/lib/agent-worker/done/<task_id>.json exists  ⇒  this task already
 *     completed in a prior run. Worker emits a synthesized result with
 *     failure_reason="already_done" and acks immediately (no fork).
 *
 * Durability:
 *   The child writes its result JSON to /var/lib/agent-worker/results/
 *   <task_id>.json *before* exiting. The parent — and only the parent —
 *   reads, publishes, acks, then moves the file to /done. A parent
 *   crash between child completion and broker ack leaves the file in
 *   /results; on next startup the worker_init scan replays it.
 */

#define _POSIX_C_SOURCE 200809L

#include "worker.h"
#include "download.h"
#include "extract.h"
#include "exec.h"
#include "util.h"

#include <cjson/cJSON.h>

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifndef AGENT_VERSION
#define AGENT_VERSION "0.0.0"
#endif

/* ============================================================
 * Context
 * ============================================================ */

struct worker_ctx_s {
	worker_config_t cfg;
	publish_conn_t *conn;

	int  drain;                    /* set by worker_begin_drain */
	int  conn_dead;                /* CRITICAL #2: set on transport failure; reconnect on next tick */

	/* IDLE when child_pid == 0, BUSY otherwise. */
	pid_t    child_pid;
	uint64_t inflight_delivery_tag;
	char     inflight_task_id[128];

	/* Cached dir paths so we don't re-derive on every tick. */
	char results_dir[512];
	char done_dir[512];
	char running_dir[512];   /* CRITICAL #10: in-flight task markers */
};

/* ============================================================
 * Small filesystem helpers
 * ============================================================ */

static int mkdir_p(const char *path, mode_t mode)
{
	if (!path || !*path) return -1;
	char tmp[1024];
	size_t n = strnlen(path, sizeof tmp);
	if (n >= sizeof tmp) return -1;
	memcpy(tmp, path, n + 1);

	for (size_t i = 1; i < n; i++) {
		if (tmp[i] == '/') {
			tmp[i] = '\0';
			if (mkdir(tmp, mode) != 0 && errno != EEXIST) return -1;
			tmp[i] = '/';
		}
	}
	if (mkdir(tmp, mode) != 0 && errno != EEXIST) return -1;
	return 0;
}

static int file_exists(const char *path)
{
	struct stat st;
	return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

/*
 * Atomic write with full durability:
 *   1. write tmp file, fsync the file fd
 *   2. rename(tmp, path) — atomic on POSIX filesystems
 *   3. fsync the parent directory so the rename is durable across power loss
 *
 * Without the dir fsync, a rename can be lost on crash even though the file
 * data is durable. /done idempotency markers must survive crash so we don't
 * silently double-execute redelivered tasks (CRITICAL #13).
 */
static int fsync_parent_dir(const char *path)
{
	char dir[1024];
	size_t n = strnlen(path, sizeof dir);
	if (n >= sizeof dir) return -1;
	memcpy(dir, path, n + 1);
	char *slash = strrchr(dir, '/');
	if (!slash) { dir[0] = '.'; dir[1] = '\0'; }
	else if (slash == dir) { dir[1] = '\0'; }    /* path is `/foo`, dir is `/` */
	else *slash = '\0';

	int dfd = open(dir, O_RDONLY | O_DIRECTORY);
	if (dfd < 0) return -1;
	int rc = fsync(dfd);
	close(dfd);
	return rc;
}

static int write_file_atomic(const char *path, const char *content)
{
	char tmp[1024];
	if ((size_t)snprintf(tmp, sizeof tmp, "%s.tmp", path) >= sizeof tmp) return -1;
	FILE *f = fopen(tmp, "wb");
	if (!f) return -1;
	size_t len = strlen(content);
	if (fwrite(content, 1, len, f) != len) { fclose(f); unlink(tmp); return -1; }
	if (fflush(f) != 0)                    { fclose(f); unlink(tmp); return -1; }
	int fd = fileno(f);
	if (fd >= 0 && fsync(fd) != 0)         { fclose(f); unlink(tmp); return -1; }
	if (fclose(f) != 0) { unlink(tmp); return -1; }
	if (rename(tmp, path) != 0) { unlink(tmp); return -1; }
	(void)fsync_parent_dir(path);
	return 0;
}

static int rmrf(const char *path)
{
	struct stat st;
	if (lstat(path, &st) != 0) return errno == ENOENT ? 0 : -1;
	if (!S_ISDIR(st.st_mode)) return unlink(path);

	DIR *d = opendir(path);
	if (!d) return -1;
	struct dirent *e;
	int rc = 0;
	while ((e = readdir(d)) != NULL) {
		if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
		char sub[1024];
		if ((size_t)snprintf(sub, sizeof sub, "%s/%s", path, e->d_name) >= sizeof sub) {
			rc = -1; continue;
		}
		if (rmrf(sub) != 0) rc = -1;
	}
	closedir(d);
	if (rmdir(path) != 0) rc = -1;
	return rc;
}

/* ============================================================
 * task_id validation
 * ============================================================ */

/*
 * Reject any task_id that could escape the state directory or contain
 * shell metacharacters. We accept only the UUID v4 grammar (or its
 * unhyphenated 32-char form) which is what the portal always emits.
 */
static int task_id_valid(const char *id)
{
	if (!id) return 0;
	size_t n = strlen(id);
	if (n != 36 && n != 32) return 0;
	for (size_t i = 0; i < n; i++) {
		char c = id[i];
		if (n == 36 && (i == 8 || i == 13 || i == 18 || i == 23)) {
			if (c != '-') return 0;
		} else if (!((c >= '0' && c <= '9') ||
		             (c >= 'a' && c <= 'f') ||
		             (c >= 'A' && c <= 'F'))) {
			return 0;
		}
	}
	return 1;
}

/* ============================================================
 * Result JSON build
 * ============================================================ */

static char *build_result_json(const worker_ctx_t *ctx,
                               const char *task_id,
                               const char *status,
                               const char *failure_reason,
                               int   has_exit_code, int exit_code,
                               long  duration_ms,
                               const char *stdout_tail,
                               const char *stderr_tail)
{
	cJSON *root = cJSON_CreateObject();
	if (!root) return NULL;

	struct timespec ts;
	clock_gettime(CLOCK_REALTIME, &ts);
	char now_buf[32];
	iso8601_utc(ts.tv_sec, now_buf, sizeof now_buf);

	cJSON_AddStringToObject(root, "message_type",     "task.result");
	cJSON_AddStringToObject(root, "machine_id",       ctx->cfg.machine_id ? ctx->cfg.machine_id : "");
	cJSON_AddStringToObject(root, "agent_version",    ctx->cfg.agent_version ? ctx->cfg.agent_version : AGENT_VERSION);
	cJSON_AddStringToObject(root, "collected_at",     now_buf);

	char host[256];
	if (gethostname(host, sizeof host) == 0) {
		host[sizeof host - 1] = '\0';
		cJSON_AddStringToObject(root, "hostname", host);
	} else {
		cJSON_AddStringToObject(root, "hostname", "unknown");
	}

	char msg_id[64];
	uuid_v4(msg_id, sizeof msg_id);
	cJSON_AddStringToObject(root, "message_id", msg_id);

	/*
	 * NOTE: boot_time / agent_started_at are intentionally omitted here.
	 * The collector's add_common_metadata() in collect.c owns those fields
	 * and caches them at process start. The worker module does not link
	 * against collect.c, and ducking the cross-module access here keeps
	 * the worker independently testable. Engine-side schema treats them
	 * as nullable on task.result (the routing-key path is portal-only).
	 */

	cJSON_AddStringToObject(root, "task_id", task_id ? task_id : "");
	cJSON_AddStringToObject(root, "status",  status);
	if (failure_reason && *failure_reason)
		cJSON_AddStringToObject(root, "failure_reason", failure_reason);
	else
		cJSON_AddNullToObject(root, "failure_reason");
	if (has_exit_code) cJSON_AddNumberToObject(root, "exit_code", exit_code);
	else               cJSON_AddNullToObject(root, "exit_code");
	cJSON_AddNumberToObject(root, "duration_ms", (double)duration_ms);
	cJSON_AddStringToObject(root, "stdout_tail",  stdout_tail ? stdout_tail : "");
	cJSON_AddStringToObject(root, "stderr_tail",  stderr_tail ? stderr_tail : "");
	cJSON_AddStringToObject(root, "completed_at", now_buf);

	char *json = cJSON_PrintUnformatted(root);
	cJSON_Delete(root);
	return json;
}

static const char *reason_for_status(download_status_t  ds,
                                     extract_status_t   es,
                                     exec_status_t      xs)
{
	if (ds == DOWNLOAD_ERR_URL_NOT_ALLOWED)     return "url_not_allowed";
	if (ds == DOWNLOAD_ERR_INSUFFICIENT_DISK)   return "insufficient_disk";
	if (ds == DOWNLOAD_ERR_DOWNLOAD_FAILED)     return "download_failed";
	if (ds == DOWNLOAD_ERR_SHA256_MISMATCH)     return "sha256_mismatch";
	if (ds == DOWNLOAD_ERR_INTERNAL)            return "internal_error";

	if (es == EXTRACT_ERR_OPEN)                 return "extract_failed";
	if (es == EXTRACT_ERR_FORBIDDEN_TYPE)       return "extract_failed";
	if (es == EXTRACT_ERR_PATH_TRAVERSAL)       return "extract_failed";
	if (es == EXTRACT_ERR_WRITE)                return "extract_failed";
	if (es == EXTRACT_ERR_INTERNAL)             return "internal_error";

	if (xs == EXEC_ERR_SCRIPT_NOT_FOUND)        return "script_not_found";
	if (xs == EXEC_ERR_SCRIPT_FAILED)           return "script_failed";
	if (xs == EXEC_ERR_SCRIPT_TIMEOUT)          return "script_timeout";
	if (xs == EXEC_ERR_INTERNAL)                return "internal_error";
	return "internal_error";
}

/* ============================================================
 * Child entry — one task lifecycle
 * ============================================================ */

static void child_write_result_file(const worker_ctx_t *ctx,
                                    const char *task_id,
                                    const char *json)
{
	char path[1024];
	snprintf(path, sizeof path, "%s/%s.json", ctx->results_dir, task_id);
	write_file_atomic(path, json);
}

static void child_run_task(worker_ctx_t *ctx, cJSON *task)
{
	const cJSON *jid       = cJSON_GetObjectItemCaseSensitive(task, "task_id");
	const cJSON *jmachine  = cJSON_GetObjectItemCaseSensitive(task, "machine_id");
	const cJSON *jdownload = cJSON_GetObjectItemCaseSensitive(task, "download");
	const cJSON *jinstall  = cJSON_GetObjectItemCaseSensitive(task, "install");

	const char *task_id = cJSON_IsString(jid) ? jid->valuestring : NULL;
	if (!task_id_valid(task_id)) {
		_exit(2);  /* Parent will synthesize internal_error with no file. */
	}

	/* Machine_id mismatch: produce already_done-style noop so portal isn't blind. */
	if (cJSON_IsString(jmachine) && ctx->cfg.machine_id &&
	    strcmp(jmachine->valuestring, ctx->cfg.machine_id) != 0) {
		char *res = build_result_json(ctx, task_id, "failure", "internal_error",
		                              0, 0, 0, "", "machine_id mismatch — task routed in error\n");
		if (res) { child_write_result_file(ctx, task_id, res); free(res); }
		_exit(0);
	}

	const char *url        = NULL;
	const char *sha256     = NULL;
	int64_t     size_bytes = 0;
	if (cJSON_IsObject(jdownload)) {
		const cJSON *ju = cJSON_GetObjectItemCaseSensitive(jdownload, "url");
		const cJSON *jh = cJSON_GetObjectItemCaseSensitive(jdownload, "sha256");
		const cJSON *jb = cJSON_GetObjectItemCaseSensitive(jdownload, "size_bytes");
		if (cJSON_IsString(ju)) url    = ju->valuestring;
		if (cJSON_IsString(jh)) sha256 = jh->valuestring;
		if (cJSON_IsNumber(jb)) size_bytes = (int64_t)jb->valuedouble;
	}

	const char *script  = "install.sh";
	int timeout_sec     = 600;
	const char **iargs  = NULL;
	if (cJSON_IsObject(jinstall)) {
		const cJSON *js = cJSON_GetObjectItemCaseSensitive(jinstall, "script");
		const cJSON *jt = cJSON_GetObjectItemCaseSensitive(jinstall, "timeout_sec");
		const cJSON *ja = cJSON_GetObjectItemCaseSensitive(jinstall, "args");
		if (cJSON_IsString(js)) script      = js->valuestring;
		if (cJSON_IsNumber(jt)) timeout_sec = (int)jt->valuedouble;
		if (cJSON_IsArray(ja)) {
			int n = cJSON_GetArraySize(ja);
			iargs = (const char **)calloc((size_t)n + 1, sizeof(char *));
			if (iargs) {
				for (int i = 0; i < n; i++) {
					const cJSON *e = cJSON_GetArrayItem(ja, i);
					if (cJSON_IsString(e)) iargs[i] = e->valuestring;
				}
			}
		}
	}

	/* Workspace directory. */
	char work[1024];
	snprintf(work, sizeof work, "%s/agent-task-%s", ctx->cfg.tmp_dir, task_id);
	rmrf(work);
	if (mkdir(work, 0700) != 0) {
		char *res = build_result_json(ctx, task_id, "failure", "internal_error",
		                              0, 0, 0, "", "workspace mkdir failed\n");
		if (res) { child_write_result_file(ctx, task_id, res); free(res); }
		free(iargs);
		_exit(0);
	}

	struct timespec ts0;
	clock_gettime(CLOCK_MONOTONIC, &ts0);

	/* 1. Download. */
	char tar_path[1100];
	snprintf(tar_path, sizeof tar_path, "%s/package.tar", work);
	download_status_t ds = (url && sha256 && size_bytes > 0)
		? download_package(url, sha256, size_bytes,
		                   ctx->cfg.allowed_hosts_csv,
		                   ctx->cfg.tmp_dir,
		                   ctx->cfg.disk_reserve_mb,
		                   tar_path)
		: DOWNLOAD_ERR_INTERNAL;

	extract_status_t es = EXTRACT_OK;
	exec_status_t    xs = EXEC_OK;
	exec_result_t    er = { .exit_code = -1 };

	if (ds == DOWNLOAD_OK) {
		es = extract_tarball(tar_path, work);
		if (es == EXTRACT_OK) {
			xs = exec_install_script(work, script, iargs,
			                         timeout_sec,
			                         ctx->cfg.mem_limit_mb,
			                         ctx->cfg.fsize_limit_mb,
			                         task_id, ctx->cfg.machine_id, &er);
		}
	}

	struct timespec ts1;
	clock_gettime(CLOCK_MONOTONIC, &ts1);
	long duration_ms = (ts1.tv_sec - ts0.tv_sec) * 1000L
	                 + (ts1.tv_nsec - ts0.tv_nsec) / 1000000L;

	int success = (ds == DOWNLOAD_OK && es == EXTRACT_OK && xs == EXEC_OK);
	const char *reason = success ? "" : reason_for_status(ds, es, xs);
	int has_exit = (xs != EXEC_ERR_SCRIPT_NOT_FOUND && xs != EXEC_ERR_INTERNAL &&
	                ds == DOWNLOAD_OK && es == EXTRACT_OK);

	char *res = build_result_json(ctx, task_id,
	                              success ? "success" : "failure",
	                              success ? "" : reason,
	                              has_exit, er.exit_code,
	                              duration_ms,
	                              er.stdout_tail, er.stderr_tail);
	if (res) {
		child_write_result_file(ctx, task_id, res);
		free(res);
	}

	rmrf(work);
	free(iargs);
	_exit(0);
}

/* ============================================================
 * Parent — publish + ack + move to /done
 * ============================================================ */

static int read_result_file(const char *path, char **out_body)
{
	*out_body = read_file_all(path);
	return *out_body ? 0 : -1;
}

static int move_to_done(const worker_ctx_t *ctx,
                        const char *task_id)
{
	char src[1024], dst[1024];
	snprintf(src, sizeof src, "%s/%s.json", ctx->results_dir, task_id);
	snprintf(dst, sizeof dst, "%s/%s.json", ctx->done_dir,    task_id);
	return rename(src, dst);
}

static int publish_result_and_ack(worker_ctx_t *ctx,
                                  const char *task_id,
                                  uint64_t delivery_tag,
                                  const char *body)
{
	int rc = publish_conn_publish(ctx->conn,
	                              ctx->cfg.amqp.exchange,
	                              ctx->cfg.result_routing_key,
	                              body, strlen(body));
	if (rc != 0) {
		fprintf(stderr, "[worker] task.result publish failed for %s — file kept in results/ for retry\n",
		        task_id);
		return -1;
	}
	if (publish_conn_ack(ctx->conn, delivery_tag) != 0) {
		fprintf(stderr, "[worker] basic.ack failed for %s — broker will redeliver, idempotency marker will gate it\n",
		        task_id);
		/* still proceed to mark done — duplicate publish handled by I1 */
	}
	if (move_to_done(ctx, task_id) != 0)
		fprintf(stderr, "[worker] move to /done failed for %s: %s\n",
		        task_id, strerror(errno));
	return 0;
}

static int publish_synth_failure(worker_ctx_t *ctx,
                                 const char *task_id,
                                 uint64_t delivery_tag,
                                 const char *reason,
                                 const char *err_tail)
{
	char *body = build_result_json(ctx, task_id, "failure", reason,
	                               0, 0, 0, "", err_tail ? err_tail : "");
	if (!body) return -1;

	/*
	 * Order matters for crash safety (CRITICAL #13):
	 *   1. write /done marker durably first so any redelivery is gated by I1
	 *   2. publish to broker
	 *   3. ack
	 * If we crash between 1 and 2, the redelivered task hits /done and
	 * publishes a synth-failure here again — duplicate but bounded. If we
	 * crash before 1, broker simply redelivers and we re-do the work.
	 */
	char dst[1024];
	if ((size_t)snprintf(dst, sizeof dst, "%s/%s.json", ctx->done_dir, task_id) < sizeof dst)
		write_file_atomic(dst, body);

	int rc = publish_conn_publish(ctx->conn,
	                              ctx->cfg.amqp.exchange,
	                              ctx->cfg.result_routing_key,
	                              body, strlen(body));
	free(body);
	if (rc != 0) return -1;
	publish_conn_ack(ctx->conn, delivery_tag);
	return 0;
}

/* ============================================================
 * Startup cleanup (D3)
 * ============================================================ */

static void purge_expired_done(const worker_ctx_t *ctx)
{
	DIR *d = opendir(ctx->done_dir);
	if (!d) return;
	time_t now = time(NULL);
	struct dirent *e;
	while ((e = readdir(d)) != NULL) {
		if (e->d_name[0] == '.') continue;
		char path[1024];
		if ((size_t)snprintf(path, sizeof path, "%s/%s", ctx->done_dir, e->d_name) >= sizeof path) continue;
		struct stat st;
		if (stat(path, &st) != 0) continue;
		if (now - st.st_mtime > (time_t)ctx->cfg.done_retention_sec) unlink(path);
	}
	closedir(d);
}

/*
 * Filename must look like `<task_id>.json` where task_id passes the same
 * validator used for incoming messages. Anything else (write_file_atomic
 * leftovers like `*.tmp`, hand-dropped files, files with embedded
 * separators) is left untouched.
 */
static int replay_filename_to_task_id(const char *fname, char *out, size_t out_sz)
{
	size_t n = strlen(fname);
	const char *suffix = ".json";
	size_t slen = strlen(suffix);
	if (n <= slen) return 0;
	if (strcmp(fname + n - slen, suffix) != 0) return 0;
	size_t base = n - slen;
	if (base >= out_sz) return 0;
	memcpy(out, fname, base);
	out[base] = '\0';
	return task_id_valid(out);
}

static void replay_pending_results(worker_ctx_t *ctx)
{
	DIR *d = opendir(ctx->results_dir);
	if (!d) return;
	struct dirent *e;
	while ((e = readdir(d)) != NULL) {
		if (e->d_name[0] == '.') continue;

		char task_id[64];
		if (!replay_filename_to_task_id(e->d_name, task_id, sizeof task_id)) {
			fprintf(stderr, "[worker] replay: skipping unrecognized file '%s'\n", e->d_name);
			continue;
		}

		char path[1024];
		if ((size_t)snprintf(path, sizeof path, "%s/%s", ctx->results_dir, e->d_name) >= sizeof path) continue;

		char *body = NULL;
		if (read_result_file(path, &body) != 0 || !body) continue;
		int rc = publish_conn_publish(ctx->conn,
		                              ctx->cfg.amqp.exchange,
		                              ctx->cfg.result_routing_key,
		                              body, strlen(body));
		free(body);
		if (rc != 0) {
			fprintf(stderr, "[worker] replay publish failed for %s — left for next startup\n", e->d_name);
			continue;
		}
		/* Move /results → /done. The original broker delivery_tag is
		 * long gone after our crash; the redelivered task (if any) hits
		 * the /done marker and is acked + skipped in worker_tick. */
		char done_path[1024];
		snprintf(done_path, sizeof done_path, "%s/%s.json", ctx->done_dir, task_id);
		rename(path, done_path);
	}
	closedir(d);
}

static void purge_stale_workspaces(const worker_ctx_t *ctx)
{
	DIR *d = opendir(ctx->cfg.tmp_dir);
	if (!d) return;
	struct dirent *e;
	while ((e = readdir(d)) != NULL) {
		if (strncmp(e->d_name, "agent-task-", 11) != 0) continue;
		char path[1024];
		if ((size_t)snprintf(path, sizeof path, "%s/%s", ctx->cfg.tmp_dir, e->d_name) >= sizeof path) continue;
		rmrf(path);
	}
	closedir(d);
}

/*
 * CRITICAL #10: any /running/<task_id> marker present at startup means a
 * previous agent instance died mid-install. We can't know whether the
 * child finished or not; treat as crashed-during-install. Publish a synth
 * failure (best-effort — connection may not be up yet on first init), and
 * move marker to /done so future redeliveries are gated by I1.
 */
static void recover_stale_running(worker_ctx_t *ctx)
{
	DIR *d = opendir(ctx->running_dir);
	if (!d) return;
	struct dirent *e;
	while ((e = readdir(d)) != NULL) {
		if (e->d_name[0] == '.') continue;
		char task_id[64];
		if (!replay_filename_to_task_id(e->d_name, task_id, sizeof task_id)) continue;

		char *body = build_result_json(ctx, task_id, "failure", "internal_error",
		                               0, 0, 0, "",
		                               "agent terminated mid-install; recovered on restart\n");
		if (!body) continue;

		/* Best-effort publish; don't ack (no live delivery_tag here). */
		(void)publish_conn_publish(ctx->conn,
		                           ctx->cfg.amqp.exchange,
		                           ctx->cfg.result_routing_key,
		                           body, strlen(body));

		/* Always promote /running -> /done so future redeliveries are gated. */
		char done_path[1024];
		if ((size_t)snprintf(done_path, sizeof done_path, "%s/%s.json", ctx->done_dir, task_id) < sizeof done_path)
			write_file_atomic(done_path, body);

		char src_path[1024];
		if ((size_t)snprintf(src_path, sizeof src_path, "%s/%s", ctx->running_dir, e->d_name) < sizeof src_path)
			unlink(src_path);

		free(body);
	}
	closedir(d);
}

/*
 * Returns 1 if a /running/<task_id> marker currently exists. Used by
 * try_pick_new_task to detect mid-install redeliveries (broker
 * consumer_timeout fired while child was still running). The current
 * parent will publish the result when the child reaps, so the redelivery
 * just acks and drops with no extra publish.
 */
static int running_marker_present(const worker_ctx_t *ctx, const char *task_id)
{
	char path[1024];
	if ((size_t)snprintf(path, sizeof path, "%s/%s.json", ctx->running_dir, task_id) >= sizeof path)
		return 0;
	return file_exists(path);
}

static void write_running_marker(const worker_ctx_t *ctx, const char *task_id)
{
	char path[1024];
	if ((size_t)snprintf(path, sizeof path, "%s/%s.json", ctx->running_dir, task_id) >= sizeof path) return;

	struct timespec ts;
	clock_gettime(CLOCK_REALTIME, &ts);
	char now_buf[32];
	iso8601_utc(ts.tv_sec, now_buf, sizeof now_buf);

	char body[256];
	snprintf(body, sizeof body,
	         "{\"task_id\":\"%s\",\"started_at\":\"%s\"}\n", task_id, now_buf);
	write_file_atomic(path, body);
}

static void clear_running_marker(const worker_ctx_t *ctx, const char *task_id)
{
	char path[1024];
	if ((size_t)snprintf(path, sizeof path, "%s/%s.json", ctx->running_dir, task_id) >= sizeof path) return;
	(void)unlink(path);
}

/* ============================================================
 * Public API
 * ============================================================ */

worker_ctx_t *worker_init(const worker_config_t *cfg)
{
	if (!cfg || !cfg->machine_id || !cfg->queue_name) return NULL;
	worker_ctx_t *ctx = (worker_ctx_t *)calloc(1, sizeof *ctx);
	if (!ctx) return NULL;
	ctx->cfg = *cfg;

	snprintf(ctx->results_dir, sizeof ctx->results_dir, "%s/results", ctx->cfg.state_dir);
	snprintf(ctx->done_dir,    sizeof ctx->done_dir,    "%s/done",    ctx->cfg.state_dir);
	snprintf(ctx->running_dir, sizeof ctx->running_dir, "%s/running", ctx->cfg.state_dir);

	if (mkdir_p(ctx->results_dir, 0700) != 0 ||
	    mkdir_p(ctx->done_dir,    0700) != 0 ||
	    mkdir_p(ctx->running_dir, 0700) != 0) {
		fprintf(stderr, "[worker] state directory init failed: %s\n", strerror(errno));
		free(ctx);
		return NULL;
	}

	ctx->conn = publish_conn_open(&ctx->cfg.amqp);
	if (!ctx->conn) {
		fprintf(stderr, "[worker] AMQP connection (agent-worker) failed\n");
		free(ctx);
		return NULL;
	}

	purge_stale_workspaces(ctx);
	replay_pending_results(ctx);
	recover_stale_running(ctx);
	purge_expired_done(ctx);

	fprintf(stderr, "[worker] initialized — queue=%s exchange=%s\n",
	        ctx->cfg.queue_name, ctx->cfg.amqp.exchange);
	return ctx;
}

void worker_begin_drain(worker_ctx_t *ctx)
{
	if (ctx) ctx->drain = 1;
}

void worker_force_child_term(worker_ctx_t *ctx, int hard)
{
	if (!ctx || ctx->child_pid == 0) return;
	int sig = hard ? SIGKILL : SIGTERM;
	if (kill(-ctx->child_pid, sig) != 0 && errno == ESRCH) {
		/* process group already gone; treat as success */
	}
	fprintf(stderr, "[worker] forced %s to child pgid=%d\n",
	        hard ? "SIGKILL" : "SIGTERM", (int)ctx->child_pid);
}

int worker_idle(const worker_ctx_t *ctx)
{
	return ctx && ctx->child_pid == 0;
}

void worker_shutdown(worker_ctx_t *ctx)
{
	if (!ctx) return;
	if (ctx->conn) publish_conn_close(ctx->conn);
	free(ctx);
}

/* ============================================================
 * worker_tick — one main-loop step
 * ============================================================ */

static int try_reap_child(worker_ctx_t *ctx)
{
	if (ctx->child_pid == 0) return 0;

	int status = 0;
	pid_t rc = waitpid(ctx->child_pid, &status, WNOHANG);
	if (rc == 0) return 0;        /* still running */
	if (rc < 0)  return -1;

	const char *task_id = ctx->inflight_task_id;
	uint64_t    tag     = ctx->inflight_delivery_tag;

	char path[1024];
	snprintf(path, sizeof path, "%s/%s.json", ctx->results_dir, task_id);

	if (file_exists(path)) {
		char *body = NULL;
		if (read_result_file(path, &body) == 0 && body) {
			publish_result_and_ack(ctx, task_id, tag, body);
			free(body);
		} else {
			publish_synth_failure(ctx, task_id, tag, "internal_error",
			                      "result file unreadable after child exit\n");
		}
	} else {
		/* Child died before writing the file (signal / OOM / abort). */
		publish_synth_failure(ctx, task_id, tag, "internal_error",
		                      "worker child exited without writing result\n");
	}

	/* Reaped — clear the in-flight running marker (CRITICAL #10). */
	clear_running_marker(ctx, task_id);

	ctx->child_pid               = 0;
	ctx->inflight_delivery_tag   = 0;
	ctx->inflight_task_id[0]     = '\0';
	return 0;
}

static int try_pick_new_task(worker_ctx_t *ctx)
{
	if (ctx->drain || ctx->child_pid != 0) return 0;

	char    *body = NULL;
	size_t   blen = 0;
	uint64_t tag  = 0;
	int rc = publish_conn_get(ctx->conn, ctx->cfg.queue_name,
	                          &body, &blen, &tag);
	if (rc == 1) return 0;         /* queue empty */
	if (rc < 0)  return -1;        /* connection-level failure */

	cJSON *task = cJSON_Parse(body);
	if (!task) {
		fprintf(stderr, "[worker] malformed task — dropping (ack)\n");
		publish_conn_ack(ctx->conn, tag);
		free(body);
		return 0;
	}

	const cJSON *jid = cJSON_GetObjectItemCaseSensitive(task, "task_id");
	const char *task_id = cJSON_IsString(jid) ? jid->valuestring : NULL;
	if (!task_id_valid(task_id)) {
		fprintf(stderr, "[worker] invalid task_id — dropping (ack)\n");
		publish_conn_ack(ctx->conn, tag);
		cJSON_Delete(task);
		free(body);
		return 0;
	}

	/* Idempotency: /done marker present → skip install. */
	char done_path[1024];
	snprintf(done_path, sizeof done_path, "%s/%s.json", ctx->done_dir, task_id);
	if (file_exists(done_path)) {
		fprintf(stderr, "[worker] task %s already_done — synthesizing result\n", task_id);
		char *res = build_result_json(ctx, task_id, "failure", "already_done",
		                              0, 0, 0, "", "");
		if (res) {
			publish_conn_publish(ctx->conn,
			                     ctx->cfg.amqp.exchange,
			                     ctx->cfg.result_routing_key,
			                     res, strlen(res));
			free(res);
		}
		publish_conn_ack(ctx->conn, tag);
		cJSON_Delete(task);
		free(body);
		return 0;
	}

	/*
	 * CRITICAL #10: redelivery while a previous attempt is still running.
	 * The /running marker says some agent (this one or a previous instance
	 * before crash) is mid-install. The original parent will publish the
	 * result; this redelivery should just be acked and dropped without
	 * publishing anything to avoid double results. If the original parent
	 * is dead, recover_stale_running on next startup will publish the synth
	 * failure for that abandoned task.
	 */
	if (running_marker_present(ctx, task_id)) {
		fprintf(stderr, "[worker] task %s already in-flight — redelivery dropped\n", task_id);
		publish_conn_ack(ctx->conn, tag);
		cJSON_Delete(task);
		free(body);
		return 0;
	}

	/* Write /running marker BEFORE fork so a redelivery during this run
	 * (broker consumer_timeout, agent restart) can detect the in-flight
	 * state and not double-execute. */
	write_running_marker(ctx, task_id);

	/* Fork. */
	pid_t pid = fork();
	if (pid < 0) {
		fprintf(stderr, "[worker] fork failed: %s\n", strerror(errno));
		publish_synth_failure(ctx, task_id, tag, "internal_error",
		                      "agent could not fork worker child\n");
		cJSON_Delete(task);
		free(body);
		return -1;
	}
	if (pid == 0) {
		/* Child: close inherited AMQP fd is handled by us not touching
		 * the connection. cJSON tree owned by child. */
		publish_conn_t *inherited = ctx->conn;
		ctx->conn = NULL;                       /* don't double-close on _exit */
		(void)inherited;                        /* fd closes at _exit */
		child_run_task(ctx, task);
		_exit(0);                               /* unreachable */
	}

	/* Parent: remember inflight. */
	ctx->child_pid             = pid;
	ctx->inflight_delivery_tag = tag;
	strncpy(ctx->inflight_task_id, task_id, sizeof ctx->inflight_task_id - 1);
	ctx->inflight_task_id[sizeof ctx->inflight_task_id - 1] = '\0';

	fprintf(stderr, "[worker] task %s started — pid=%d\n", task_id, (int)pid);

	cJSON_Delete(task);
	free(body);
	return 0;
}

/*
 * CRITICAL #2 + #3: reconnect after transport / channel failures.
 * Closing and re-opening the connection is the simplest way to recover
 * from any of: heartbeat-driven socket close, broker-side channel
 * exception (e.g. NOT_FOUND from basic_get on a queue the portal hasn't
 * declared yet), or TLS-level read/write errors. Outstanding delivery
 * tags are dropped — this is fine because we have not yet acked them and
 * the broker will redeliver after timeout / requeue.
 */
static int reconnect_if_dead(worker_ctx_t *ctx)
{
	if (!ctx->conn_dead && ctx->conn) return 0;

	if (ctx->conn) { publish_conn_close(ctx->conn); ctx->conn = NULL; }
	ctx->conn = publish_conn_open(&ctx->cfg.amqp);
	if (!ctx->conn) {
		fprintf(stderr, "[worker] reconnect failed — will retry next tick\n");
		return -1;
	}
	ctx->conn_dead = 0;
	fprintf(stderr, "[worker] AMQP connection re-established\n");

	/*
	 * After a reconnect the in-flight delivery_tag is stale — that channel
	 * is gone. The broker still has the message unacked from the old
	 * channel and will redeliver after timeout. /running marker keeps I1
	 * gating intact for the redelivery.
	 */
	ctx->inflight_delivery_tag = 0;
	return 0;
}

void worker_keepalive(worker_ctx_t *ctx)
{
	if (ctx && ctx->conn && !ctx->conn_dead)
		publish_conn_pump(ctx->conn);
}

int worker_tick(worker_ctx_t *ctx)
{
	if (!ctx) return -1;

	if (reconnect_if_dead(ctx) < 0) return -1;
	publish_conn_pump(ctx->conn);   /* CRITICAL #1: heartbeat keepalive */

	if (try_reap_child(ctx) < 0)    { ctx->conn_dead = 1; return -1; }
	if (try_pick_new_task(ctx) < 0) { ctx->conn_dead = 1; return -1; }

	return 0;
}
