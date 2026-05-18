/**
 * @file main.c
 * @brief Agent entry point (Windows).
 *
 * Modes (selected by argv):
 *   default     : run_as_service()  — SCM dispatcher (used by `sc.exe start`)
 *   --console   : run_as_console()  — foreground, Ctrl+C exits
 *
 * Loop structure mirrors the Linux agent (assessment-agent/src/main.c §lifecycle):
 *   1. parse env (agent.env then shell — shell wins)
 *   2. resolve machine_id once
 *   3. publish initial `inventory` (retry with backoff)
 *   4. loop: `metrics` every AGENT_INTERVAL_SEC, `inventory` republish every
 *      AGENT_INVENTORY_REFRESH_SEC ±15% jitter
 *
 * v1 한정: worker (task.install) 없음.
 */

#include "collect.h"
#include "publish.h"
#include "service.h"
#include "util.h"
#include "cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <winsock2.h>
#include <windows.h>

#ifndef AGENT_VERSION
#define AGENT_VERSION "1.0.0"
#endif

/* ============================================================
 *  publish_config_t builder (env-driven)
 * ============================================================ */
static publish_config_t make_publish_config(void)
{
	/* TLS / verify 플래그는 parse_bool 사용 — atoi 면 "true" 가 0이 되어
	 * production AMQPS 가 silently plain 으로 떨어지는 보안 회귀를 막는다. */
	publish_config_t cfg;
	memset(&cfg, 0, sizeof cfg);
	cfg.host                = getenv_default("RABBITMQ_HOST", "localhost");
	cfg.port                = getenv_int_or ("RABBITMQ_PORT", 0);
	cfg.vhost               = getenv_default("RABBITMQ_VHOST", "/");
	cfg.user                = getenv_default("RABBITMQ_USER", "admin");
	cfg.password            = getenv_default("RABBITMQ_PASS", "admin");
	cfg.exchange            = getenv_default("RABBITMQ_EXCHANGE", "assessment");
	cfg.heartbeat_sec       = getenv_int_or ("RABBITMQ_HEARTBEAT_SEC", 60);
	cfg.tls_enabled         = getenv_bool   ("RABBITMQ_TLS_ENABLED", 0);
	cfg.tls_ca_path         = getenv_default("RABBITMQ_TLS_CA_PATH", "");
	cfg.tls_verify_peer     = getenv_bool   ("RABBITMQ_TLS_VERIFY_PEER", 1);
	cfg.tls_verify_hostname = getenv_bool   ("RABBITMQ_TLS_VERIFY_HOSTNAME", 1);
	cfg.tls_cert_path       = getenv_default("RABBITMQ_TLS_CERT_PATH", "");
	cfg.tls_key_path        = getenv_default("RABBITMQ_TLS_KEY_PATH", "");
	if (cfg.port <= 0)
		cfg.port = cfg.tls_enabled ? 5671 : 5672;
	return cfg;
}

/* ============================================================
 *  Helpers
 * ============================================================ */
static int serialize_and_publish(const publish_config_t *cfg,
                                 const char *routing_key, cJSON *msg)
{
	if (!msg) return -1;
	char *body = cJSON_PrintUnformatted(msg);
	int rc = -1;
	if (body) rc = publish_message(cfg, routing_key, body, strlen(body));
	free(body);
	cJSON_Delete(msg);
	return rc;
}

static void emit_error(const publish_config_t *cfg, const char *machine_id,
                       const char *code, const char *msg, const char *comp,
                       int retry_count, const char *first_at,
                       const char *recovered_at)
{
	const char *rk = getenv_default("RABBITMQ_ROUTING_KEY_ERROR", "server.error");
	cJSON *e = build_error_payload(machine_id ? machine_id : "",
	                               AGENT_VERSION,
	                               code, msg, comp,
	                               retry_count, first_at, recovered_at);
	int rc = serialize_and_publish(cfg, rk, e);
	if (rc != 0)
		fprintf(stderr, "[agent] failed to publish error %s\n",
		        code ? code : "?");
}

/* Sleep in 1-second slices so SCM stop response is bounded. */
static void interruptible_sleep(int seconds)
{
	for (int i = 0; i < seconds && !stop_requested(); i++)
		Sleep(1000);
}

static time_t next_inventory_deadline(time_t now, int refresh_sec)
{
	return now + (time_t)jitter_seconds(refresh_sec, 0.15);
}

static int publish_with_retry(const publish_config_t *cfg, const char *rk,
                              cJSON *msg, const char *machine_id,
                              int max_backoff)
{
	if (!msg) return -1;
	char *body = cJSON_PrintUnformatted(msg);
	cJSON_Delete(msg);
	if (!body) return -1;

	unsigned int backoff = 1;
	int retry_count = 0;
	char first_failed_at[32] = {0};

	for (;;) {
		int rc = publish_message(cfg, rk, body, strlen(body));
		if (rc == 0) {
			free(body);
			if (retry_count > 0) {
				time_t now; time(&now);
				char now_buf[32];
				iso8601_utc(now, now_buf, sizeof now_buf);
				char detail[160];
				snprintf(detail, sizeof detail,
				         "broker reconnected after %d retries", retry_count);
				emit_error(cfg, machine_id, "PUBLISH_RECOVERED",
				           detail, "publish",
				           retry_count, first_failed_at, now_buf);
			}
			return 0;
		}
		if (stop_requested()) { free(body); return -1; }

		if (retry_count == 0) {
			time_t now; time(&now);
			iso8601_utc(now, first_failed_at, sizeof first_failed_at);
		}
		retry_count++;
		if (backoff > (unsigned int)max_backoff)
			backoff = (unsigned int)max_backoff;
		fprintf(stderr, "[agent] publish failed, retry %d in %us\n",
		        retry_count, backoff);
		interruptible_sleep((int)backoff);
		if (stop_requested()) { free(body); return -1; }
		backoff *= 2;
	}
}

/* ============================================================
 *  agent_run — the actual collection loop.
 *
 *  Called by service.c::service_main (SCM mode) or service.c::run_as_console
 *  (foreground mode). Returns the process exit code.
 * ============================================================ */
int agent_run(void)
{
	WSADATA wsa;
	WSAStartup(MAKEWORD(2, 2), &wsa);

	/* Try local .env first (dev), then production paths. */
	load_env_file(".env");
	load_env_file("C:\\ProgramData\\assessment-agent\\agent.env");
	load_env_file("C:\\ProgramData\\assessment-agent\\agent.env.local");

	srand((unsigned int)(time(NULL) ^ GetCurrentProcessId()));

	int interval    = getenv_int_or("AGENT_INTERVAL_SEC", 60);
	int inv_refresh = getenv_int_or("AGENT_INVENTORY_REFRESH_SEC", 3600);
	publish_config_t cfg = make_publish_config();

	const char *rk_inv = getenv_default("RABBITMQ_ROUTING_KEY_INVENTORY", "server.inventory");
	const char *rk_met = getenv_default("RABBITMQ_ROUTING_KEY_METRICS",   "server.metrics");

	char *machine_id = resolve_machine_id();
	if (!machine_id) {
		fprintf(stderr, "[agent] machine_id resolution failed "
		                "(HKLM\\SOFTWARE\\Microsoft\\Cryptography\\MachineGuid missing)\n");
		emit_error(&cfg, NULL, "MACHINE_ID_UNRESOLVED",
		           "registry MachineGuid missing", "collect",
		           -1, NULL, NULL);
		WSACleanup();
		return 2;
	}
	fprintf(stderr, "[agent] machine_id=%s\n", machine_id);

	/* --- Initial inventory --- */
	cJSON *inv = collect_inventory_payload(machine_id, AGENT_VERSION);
	if (!inv) {
		emit_error(&cfg, machine_id, "COLLECT_INVENTORY_FAILED",
		           "core inventory source unreadable", "collect",
		           -1, NULL, NULL);
		fprintf(stderr, "[agent] inventory collect failed; continuing with metrics only\n");
	} else {
		int max_backoff = interval > 0 ? interval : 60;
		if (publish_with_retry(&cfg, rk_inv, inv, machine_id, max_backoff) == 0)
			fprintf(stderr, "[agent] published inventory\n");
	}

	/* --- One-shot mode --- */
	if (interval <= 0) {
		cJSON *m = collect_metrics_payload(machine_id, AGENT_VERSION);
		if (!m) {
			emit_error(&cfg, machine_id, "COLLECT_METRICS_FAILED",
			           "core metrics source unreadable", "collect",
			           -1, NULL, NULL);
			free(machine_id); WSACleanup();
			return 1;
		}
		int rc = serialize_and_publish(&cfg, rk_met, m);
		free(machine_id); WSACleanup();
		return rc == 0 ? 0 : 1;
	}

	/* --- Loop mode --- */
	fprintf(stderr, "[agent] loop mode: interval=%ds, inventory_refresh=%ds\n",
	        interval, inv_refresh);

	time_t inv_next = (inv_refresh > 0)
		? next_inventory_deadline(time(NULL), inv_refresh) : 0;

	while (!stop_requested()) {
		cJSON *m = collect_metrics_payload(machine_id, AGENT_VERSION);
		if (!m) {
			emit_error(&cfg, machine_id, "COLLECT_METRICS_FAILED",
			           "core metrics source unreadable", "collect",
			           -1, NULL, NULL);
		} else {
			publish_with_retry(&cfg, rk_met, m, machine_id, interval);
		}

		if (inv_refresh > 0 && time(NULL) >= inv_next) {
			cJSON *iv = collect_inventory_payload(machine_id, AGENT_VERSION);
			if (!iv) {
				emit_error(&cfg, machine_id, "COLLECT_INVENTORY_FAILED",
				           "core inventory source unreadable", "collect",
				           -1, NULL, NULL);
			} else if (publish_with_retry(&cfg, rk_inv, iv, machine_id, interval) == 0) {
				fprintf(stderr, "[agent] republished inventory (periodic)\n");
			}
			inv_next = next_inventory_deadline(time(NULL), inv_refresh);
		}

		if (stop_requested()) break;
		interruptible_sleep(interval);
	}

	fprintf(stderr, "[agent] stopping (machine_id=%s)\n", machine_id);
	free(machine_id);
	WSACleanup();
	return 0;
}

/* ============================================================
 *  main
 * ============================================================ */
int main(int argc, char **argv)
{
	int console_mode = 0;
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--console") == 0 ||
		    strcmp(argv[i], "-c") == 0)
			console_mode = 1;
	}
	if (console_mode)
		return run_as_console();
	return run_as_service();
}
