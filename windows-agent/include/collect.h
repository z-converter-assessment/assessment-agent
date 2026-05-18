/**
 * @file collect.h
 * @brief Inventory and metrics collectors (Windows). Builds v2 payloads.
 *
 * /proc 대신 Win32 API (Registry, GlobalMemoryStatusEx, GetSystemTimes,
 * GetAdaptersAddresses, EnumServicesStatusExW, IOCTL_DISK_PERFORMANCE, ...)
 * 을 사용해 Linux 에이전트와 동일한 페이로드 스키마를 emit한다.
 *
 * 정식 계약: assessment-agent/docs/payload-schema.md
 *
 * Windows에서 1:1 대응이 없어 항상 null로 emit 되는 필드:
 *   - metrics.load_1m / load_5m / load_15m   (Windows에 loadavg 개념 없음)
 *   - cpu_stat.{nice, iowait, irq, softirq, steal} (0으로 emit)
 *
 * 카운터 단위 환산:
 *   - cpu_stat: GetSystemTimes의 100ns FILETIME → 10ms tick (jiffy 호환)
 *   - disk_io.sectors_*: BytesRead/Written ÷ 512 (Linux diskstats 호환)
 */

#ifndef ASSESSMENT_AGENT_COLLECT_H
#define ASSESSMENT_AGENT_COLLECT_H

#include "cJSON.h"

/**
 * @brief Resolve the immutable server identifier.
 *
 * Resolution order on Windows:
 *   1. Registry HKLM\SOFTWARE\Microsoft\Cryptography\MachineGuid
 *   2. Cloud metadata instance-id (AWS IMDSv2 / Azure / GCP) — best effort
 *
 * `inventory.mac_addresses[]`는 별도 신호로 emit되어 엔진이
 * machine_id 충돌(이미지 클론) 감지에 활용한다.
 *
 * Returns malloc'd string the caller must free, or NULL on total failure.
 */
char *resolve_machine_id(void);

/**
 * @brief Produce an `inventory` payload conforming to docs/payload-schema.md §1.
 *
 * Common metadata: message_type, machine_id, agent_version, collected_at,
 * hostname, message_id, boot_time, agent_started_at.
 *
 * Windows-specific value mapping:
 *   - os_id        : "windows"
 *   - os_version   : Registry CurrentVersion\ProductName + DisplayVersion
 *   - os_codename  : null
 *   - kernel_version : Registry CurrentBuildNumber.UBR (e.g. "17763.5458")
 *   - cpu_model    : CPUID brand string (EAX=0x80000002..04)
 *   - mac_addresses: GetAdaptersAddresses → PhysicalAddress, sorted, dedup
 *
 * @return cJSON object on success (caller deletes), NULL on critical failure.
 */
cJSON *collect_inventory_payload(const char *machine_id, const char *agent_version);

/**
 * @brief Produce a `metrics` payload conforming to docs/payload-schema.md §2.
 *
 * @return cJSON object on success (caller deletes), NULL on critical failure.
 */
cJSON *collect_metrics_payload(const char *machine_id, const char *agent_version);

/**
 * @brief Produce an `error` payload conforming to docs/payload-schema.md §3.
 */
cJSON *build_error_payload(const char *machine_id,
                           const char *agent_version,
                           const char *error_code,
                           const char *error_message,
                           const char *failed_component,
                           int         retry_count,
                           const char *first_failed_at,
                           const char *recovered_at);

#endif
