/**
 * @file main.c
 * @brief Agent entry point — orchestrates inventory + metrics + error publish.
 *
 * Lifecycle:
 *   1. Parse env (.env then shell — shell wins).
 *   2. Resolve machine_id once (cached for the process).
 *   3. Publish `inventory` once. Failure here is fatal in one-shot mode but
 *      not in loop mode (we keep trying with backoff so a slow startup
 *      doesn't kill the agent).
 *   4. Loop: publish `metrics` every AGENT_INTERVAL_SEC seconds.
 *
 * Error policy (matches docs/payload-schema.md §3):
 *   - Optional source unavailable → field is null/empty, no error message.
 *   - Critical collect failure (`collect_*_payload` returned NULL) → emit
 *     a single `server.error` and continue.
 *   - Publish failure → exponential backoff retry. Inside the retry loop we
 *     do NOT publish errors (we'd be publishing into a broken broker). On
 *     recovery, emit a single `PUBLISH_RECOVERED` summary message including
 *     `retry_count`, `first_failed_at`, `recovered_at`.
 */

#define _POSIX_C_SOURCE 200809L

#include "collect.h"
#include "publish.h"
#include "util.h"

#include <cjson/cJSON.h>

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#ifndef AGENT_VERSION
#define AGENT_VERSION "1.0.0"
#endif

static volatile sig_atomic_t g_stop = 0;
static void on_signal(int sig) { (void)sig; g_stop = 1; }

/**
 * @brief Build a publish_config_t from current environment.
 */
static publish_config_t make_publish_config(void)
{
	publish_config_t cfg = {
		.host        = getenv_default("RABBITMQ_HOST", "localhost"),
		.port        = atoi(getenv_default("RABBITMQ_PORT", "5672")),
		.vhost       = getenv_default("RABBITMQ_VHOST", "/"),
		.user        = getenv_default("RABBITMQ_USER", "admin"),
		.password    = getenv_default("RABBITMQ_PASS", "admin"),
		.exchange    = getenv_default("RABBITMQ_EXCHANGE", "assessment"),

		/* RabbitMQ recommends 60s heartbeat. */
		.heartbeat_sec = atoi(getenv_default("RABBITMQ_HEARTBEAT_SEC", "60")),

		.tls_enabled = atoi(getenv_default("RABBITMQ_TLS_ENABLED", "0")),
		.tls_ca_path = getenv_default("RABBITMQ_TLS_CA_PATH", ""),
		.tls_verify_peer     = atoi(getenv_default("RABBITMQ_TLS_VERIFY_PEER", "1")),
		.tls_verify_hostname = atoi(getenv_default("RABBITMQ_TLS_VERIFY_HOSTNAME", "1")),
		.tls_cert_path = getenv_default("RABBITMQ_TLS_CERT_PATH", ""),
		.tls_key_path  = getenv_default("RABBITMQ_TLS_KEY_PATH", ""),
	};
	if (cfg.port <= 0)
		cfg.port = cfg.tls_enabled ? 5671 : 5672;
	return cfg;
}

/**
 * @brief Serialize @p msg, publish via @p routing_key, free both.
 */
static int serialize_and_publish(const publish_config_t *cfg,
                                 const char *routing_key,
                                 cJSON *msg)
{
	if (!msg)
		return -1;
	char *body = cJSON_PrintUnformatted(msg);
	int rc = -1;
	if (body)
		rc = publish_message(cfg, routing_key, body, strlen(body));
	free(body);
	cJSON_Delete(msg);
	return rc;
}

/**
 * @brief Publish a `server.error` message (best-effort, never recurses).
 */
static void emit_error(const publish_config_t *cfg,
                       const char *machine_id,
                       const char *error_code,
                       const char *error_message,
                       const char *failed_component,
                       int retry_count,
                       const char *first_failed_at,
                       const char *recovered_at)
{
	const char *rk = getenv_default("RABBITMQ_ROUTING_KEY_ERROR", "server.error");
	cJSON *msg = build_error_payload(machine_id ? machine_id : "",
	                                 AGENT_VERSION,
	                                 error_code, error_message,
	                                 failed_component,
	                                 retry_count,
	                                 first_failed_at, recovered_at);
	int rc = serialize_and_publish(cfg, rk, msg);
	if (rc != 0) {
		fprintf(stderr, "[agent] failed to publish error message %s\n",
		        error_code ? error_code : "?");
	}
}

/**
 * @brief Publish a payload with exponential-backoff retry.
 *
 * On retry, the function records `first_failed_at` once and increments
 * `retry_count`. On final success after >0 retries, emits a
 * `PUBLISH_RECOVERED` error summary so operators can observe the outage.
 *
 * @return 0 on success, -1 if signal-stopped.
 */
static int publish_with_retry(const publish_config_t *cfg,
                              const char *routing_key,
                              cJSON *msg,
                              const char *machine_id,
                              int max_backoff)
{
	if (!msg)
		return -1;
	char *body = cJSON_PrintUnformatted(msg);
	cJSON_Delete(msg);
	if (!body)
		return -1;

	unsigned int backoff = 1;
	int retry_count = 0;
	char first_failed_at[32] = { 0 };

	for (;;) {
		int rc = publish_message(cfg, routing_key, body, strlen(body));
		if (rc == 0) {
			free(body);
			if (retry_count > 0) {
				char now_buf[32];
				struct timespec ts;
				clock_gettime(CLOCK_REALTIME, &ts);
				iso8601_utc(ts.tv_sec, now_buf, sizeof now_buf);
				char detail[160];
				snprintf(detail, sizeof detail,
				         "broker reconnected after %d retries", retry_count);
				emit_error(cfg, machine_id,
				           "PUBLISH_RECOVERED", detail, "publish",
				           retry_count, first_failed_at, now_buf);
			}
			return 0;
		}

		if (g_stop) {
			free(body);
			return -1;
		}

		if (retry_count == 0) {
			struct timespec ts;
			clock_gettime(CLOCK_REALTIME, &ts);
			iso8601_utc(ts.tv_sec, first_failed_at, sizeof first_failed_at);
		}
		retry_count++;

		if (backoff > (unsigned int)max_backoff)
			backoff = (unsigned int)max_backoff;
		fprintf(stderr, "[agent] publish failed, retry %d in %us\n",
		        retry_count, backoff);
		sleep(backoff);
		backoff *= 2;
	}
}

/**
 * @brief Probe optional shell-out commands and log their availability.
 *
 * The agent has silent fallbacks (or `null`) for missing tools, so a
 * field becoming empty does not necessarily indicate a collect failure.
 * Logging command presence at startup lets operators distinguish
 * "no disks found" from "lsblk missing".
 */
static void log_optional_cmds(void)
{
	const char *cmds[] = { "lsblk", "curl", "dbus-uuidgen", NULL };
	for (int i = 0; cmds[i]; i++) {
		char buf[96];
		snprintf(buf, sizeof buf,
		         "command -v %s >/dev/null 2>&1", cmds[i]);
		int rc = system(buf);
		fprintf(stderr, "[agent] cmd %-13s %s\n",
		        cmds[i],
		        (rc == 0) ? "available"
		                  : "MISSING (silent fallback / null)");
	}
}

int main(void)
{
	signal(SIGINT,  on_signal);
	signal(SIGTERM, on_signal);

	load_env_file(".env");
	log_optional_cmds();

	int interval = atoi(getenv_default("AGENT_INTERVAL_SEC", "60"));
	publish_config_t cfg = make_publish_config();

	const char *rk_inv = getenv_default("RABBITMQ_ROUTING_KEY_INVENTORY", "server.inventory");
	const char *rk_met = getenv_default("RABBITMQ_ROUTING_KEY_METRICS",   "server.metrics");

	char *machine_id = resolve_machine_id();
	if (!machine_id) {
		fprintf(stderr, "[agent] machine_id resolution failed (no /etc/machine-id, "
		                "no dbus-uuidgen, no IMDS). Cannot identify host.\n");
		emit_error(&cfg, NULL,
		           "MACHINE_ID_UNRESOLVED",
		           "no /etc/machine-id, no dbus-uuidgen, no IMDS instance-id",
		           "collect", -1, NULL, NULL);
		return 2;
	}
	fprintf(stderr, "[agent] machine_id=%s\n", machine_id);

	/* --- Initial inventory --- */
	cJSON *inv = collect_inventory_payload(machine_id, AGENT_VERSION);
	if (!inv) {
		emit_error(&cfg, machine_id,
		           "COLLECT_INVENTORY_FAILED",
		           "core inventory source unreadable", "collect",
		           -1, NULL, NULL);
		fprintf(stderr, "[agent] inventory collect failed; continuing with metrics only\n");
	} else {
		int max_backoff = interval > 0 ? interval : 60;
		if (publish_with_retry(&cfg, rk_inv, inv, machine_id, max_backoff) == 0)
			fprintf(stderr, "[agent] published inventory\n");
	}

	/* --- One-shot mode: collect & publish metrics once and exit --- */
	if (interval <= 0) {
		cJSON *m = collect_metrics_payload(machine_id, AGENT_VERSION);
		if (!m) {
			emit_error(&cfg, machine_id,
			           "COLLECT_METRICS_FAILED",
			           "core metrics source unreadable", "collect",
			           -1, NULL, NULL);
			free(machine_id);
			return 1;
		}
		int rc = serialize_and_publish(&cfg, rk_met, m);
		free(machine_id);
		return rc == 0 ? 0 : 1;
	}

	/* --- Loop mode --- */
	fprintf(stderr, "[agent] loop mode: interval=%ds (Ctrl+C to exit)\n", interval);
	while (!g_stop) {
		cJSON *m = collect_metrics_payload(machine_id, AGENT_VERSION);
		if (!m) {
			emit_error(&cfg, machine_id,
			           "COLLECT_METRICS_FAILED",
			           "core metrics source unreadable", "collect",
			           -1, NULL, NULL);
		} else {
			publish_with_retry(&cfg, rk_met, m, machine_id, interval);
		}
		if (g_stop) break;
		sleep((unsigned int)interval);
	}

	free(machine_id);
	return 0;
}
