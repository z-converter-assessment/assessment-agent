/**
 * @file download.c
 * @brief HTTPS package download (Windows) — stub.
 *
 * Step 2 (이 파일): 모든 함수가 fail 반환. libcurl 의존성을 Makefile 에 반영해
 * 빌드 검증. 실제 구현은 후속 commit (libcurl HTTPS + sha256 streaming +
 * size cap + host whitelist + statvfs equivalent (GetDiskFreeSpaceExW)).
 */

#include "download.h"

#include <string.h>

download_status_t download_package(const char *url,
                                   const char *expected_sha256_hex,
                                   int64_t     expected_size_bytes,
                                   const char *allowed_hosts_csv,
                                   const char *tmp_dir,
                                   int         disk_reserve_mb,
                                   const char *out_path)
{
	(void)url; (void)expected_sha256_hex; (void)expected_size_bytes;
	(void)allowed_hosts_csv; (void)tmp_dir; (void)disk_reserve_mb;
	(void)out_path;
	return DOWNLOAD_ERR_INTERNAL;
}

int download_host_allowed(const char *host, const char *allowed_hosts_csv)
{
	(void)host; (void)allowed_hosts_csv;
	return 0;
}

int download_url_extract_host(const char *url, char *out, size_t out_sz)
{
	(void)url;
	if (out && out_sz) out[0] = '\0';
	return 0;
}

int download_url_is_https(const char *url)
{
	(void)url;
	return 0;
}
