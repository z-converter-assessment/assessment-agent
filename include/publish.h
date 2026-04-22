/**
 * @file publish.h
 * @brief RabbitMQ(AMQP) 단일 메시지 발행 인터페이스.
 *
 * Python 프로토타입과 동일한 큐 설계를 사용한다:
 *   - Exchange : `<cfg.exchange>` (direct, durable)
 *   - Queue    : `<cfg.queue>` (durable)
 *   - Binding  : queue ← exchange via `<cfg.routing_key>`
 *   - Message  : content-type `application/json`, delivery_mode=2 (persistent)
 */

#ifndef ASSESSMENT_AGENT_PUBLISH_H
#define ASSESSMENT_AGENT_PUBLISH_H

#include <stddef.h>

/**
 * @brief 브로커 연결/발행에 필요한 설정 묶음.
 *
 * 모든 문자열 필드는 호출자 소유이며, @ref publish_message
 * 호출이 반환할 때까지 유효해야 한다.
 */
typedef struct {
	const char *host;        /**< 브로커 호스트/IP (내부망 경로). */
	int         port;        /**< AMQP 포트. 일반적으로 5672. */
	const char *user;        /**< SASL PLAIN 사용자. */
	const char *password;    /**< SASL PLAIN 비밀번호. */
	const char *exchange;    /**< direct exchange 이름. */
	const char *queue;       /**< 대상 queue 이름. */
	const char *routing_key; /**< exchange→queue 바인딩에 쓰이는 라우팅 키. */
} publish_config_t;

/**
 * @brief 메시지 1건을 동기 publish 한다.
 *
 * 내부적으로 connect → login → channel.open → exchange/queue/binding
 * declare → basic.publish → 채널/연결 닫기까지 1 사이클을 수행한다.
 * 현재 구현은 매 호출마다 새 연결을 연다(테스트 용이성 우선).
 *
 * @param cfg      브로커 연결 및 큐 설정.
 * @param body     발행할 메시지 바이트 버퍼(예: JSON 직렬화 결과).
 * @param body_len @p body 의 길이(바이트).
 * @return 0: 성공, 음수: 실패. 실패 이유는 stderr에 출력된다.
 */
int publish_message(const publish_config_t *cfg, const char *body, size_t body_len);

#endif
