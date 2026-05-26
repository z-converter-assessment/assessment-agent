/**
 * @file download.h
 * @brief HTTPS / HTTP package download with sha256 streaming + host whitelist + size cap (Windows).
 *
 * Linux download.c 의 Windows 포팅. backend / 보안 경계 동일:
 *   - HTTP / HTTPS scheme. HTTPS 는 TLS peer + hostname verify, HTTP 는 sha256
 *     streaming 검증으로 무결성 보장 (사내망 ZDM / mirror 채널 가정)
 *   - 30x redirect 비활성 (RD2). redirect 응답은 download_failed 로 처리
 *   - Host 는 WORKER_DOWNLOAD_ALLOWED_HOSTS 와 case-insensitive 정확 매치
 *   - sha256 streaming via OpenSSL EVP_DigestUpdate, 종료 시 비교
 *   - 다운로드 중 expected_size_bytes 초과 시 abort
 *   - WORKER_TMP_DIR 의 자유 공간 < (size + WORKER_DISK_RESERVE_MB) 면 pre-flight fail
 *     (Windows: GetDiskFreeSpaceExW)
 */

#ifndef ASSESSMENT_AGENT_WIN_DOWNLOAD_H
#define ASSESSMENT_AGENT_WIN_DOWNLOAD_H

#include <stddef.h>
#include <stdint.h>

typedef enum {
	DOWNLOAD_OK = 0,
	DOWNLOAD_ERR_URL_NOT_ALLOWED,
	DOWNLOAD_ERR_INSUFFICIENT_DISK,
	DOWNLOAD_ERR_DOWNLOAD_FAILED,
	DOWNLOAD_ERR_SHA256_MISMATCH,
	DOWNLOAD_ERR_INTERNAL,
} download_status_t;

/**
 * @brief Download @p url → @p out_path, verifying sha256 + size + host.
 */
download_status_t download_package(const char *url,
                                   const char *expected_sha256_hex,
                                   int64_t     expected_size_bytes,
                                   const char *allowed_hosts_csv,
                                   const char *tmp_dir,
                                   int         disk_reserve_mb,
                                   const char *out_path);

int download_host_allowed(const char *host, const char *allowed_hosts_csv);
int download_url_extract_host(const char *url, char *out, size_t out_sz);
int download_url_is_https(const char *url);

#endif
