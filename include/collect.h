/**
 * @file collect.h
 * @brief 고객 서버 인벤토리 수집기.
 *
 * Python 프로토타입과 동일한 스키마의 payload를 만든다:
 * @code{.json}
 * {
 *   "hostname": "<str>",
 *   "nproc":    "<str>",
 *   "free":     { "mem_total_mb": <int> },
 *   "lsblk_raw": [ { "name": "<str>", "size": "<str>" }, ... ],
 *   "ip_raw":    { "internal": [<str>, ...], "external": [<str>, ...] }
 * }
 * @endcode
 *
 * 본 모듈은 수집만 담당하며, 브로커 전송은 publish 모듈에서 수행한다.
 */

#ifndef ASSESSMENT_AGENT_COLLECT_H
#define ASSESSMENT_AGENT_COLLECT_H

#include <cjson/cJSON.h>

/**
 * @brief 인벤토리 payload를 수집해 cJSON 객체로 반환한다.
 *
 * 필수 필드(hostname/nproc/free/lsblk/ip.internal) 중 하나라도
 * 수집에 실패하면 전체 실패로 간주하고 NULL을 돌려준다.
 * ip.external은 네트워크 장애가 흔하므로 내부적으로 빈 배열
 * fallback을 수행한다.
 *
 * @return 새로 할당된 cJSON 객체. 호출자가 @ref cJSON_Delete 로 해제해야 한다.
 *         실패 시 NULL.
 */
cJSON *collect_inventory(void);

#endif
