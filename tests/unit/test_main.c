/**
 * @file test_main.c
 * @brief Unit tests for util.c (public API) and collect.c (statics).
 *
 * collect.c is compile-included so we can exercise its `static` parsers
 * without changing their visibility in production builds. util.c is
 * linked separately (its public symbols are used by both collect.c and
 * the tests). See Makefile target `test-unit`.
 */

/* collect.c sets _POSIX_C_SOURCE / _GNU_SOURCE for the whole TU. */
#include "../../src/collect.c"

#include "tinytest.h"

#include <fcntl.h>
#include <unistd.h>

/* ============================================================
 * util.c — trim_inplace
 * ============================================================ */

static void test_trim_inplace_basic(void)
{
	char buf[] = "   hello   ";
	trim_inplace(buf);
	ASSERT_STR_EQ(buf, "hello");
}

static void test_trim_inplace_no_trim(void)
{
	char buf[] = "hello";
	trim_inplace(buf);
	ASSERT_STR_EQ(buf, "hello");
}

static void test_trim_inplace_all_whitespace(void)
{
	char buf[] = "   \t\n  ";
	trim_inplace(buf);
	ASSERT_STR_EQ(buf, "");
}

static void test_trim_inplace_null(void)
{
	char *r = trim_inplace(NULL);
	ASSERT_NULL(r);
}

static void test_trim_inplace_internal_whitespace_kept(void)
{
	char buf[] = "  a b c  ";
	trim_inplace(buf);
	ASSERT_STR_EQ(buf, "a b c");
}

/* ============================================================
 * util.c — getenv_default
 * ============================================================ */

static void test_getenv_default_unset(void)
{
	unsetenv("TT_GD_VAR");
	ASSERT_STR_EQ(getenv_default("TT_GD_VAR", "fallback"), "fallback");
}

static void test_getenv_default_empty(void)
{
	setenv("TT_GD_VAR", "", 1);
	ASSERT_STR_EQ(getenv_default("TT_GD_VAR", "fallback"), "fallback");
	unsetenv("TT_GD_VAR");
}

static void test_getenv_default_set(void)
{
	setenv("TT_GD_VAR", "actual", 1);
	ASSERT_STR_EQ(getenv_default("TT_GD_VAR", "fallback"), "actual");
	unsetenv("TT_GD_VAR");
}

/* ============================================================
 * util.c — iso8601_utc / iso8601_utc_ms
 * ============================================================ */

static void test_iso8601_utc_epoch(void)
{
	char buf[32] = { 0 };
	iso8601_utc(0, buf, sizeof buf);
	ASSERT_STR_EQ(buf, "1970-01-01T00:00:00Z");
}

static void test_iso8601_utc_known(void)
{
	/* 2000-01-01T00:00:00Z */
	char buf[32] = { 0 };
	iso8601_utc(946684800L, buf, sizeof buf);
	ASSERT_STR_EQ(buf, "2000-01-01T00:00:00Z");
}

static void test_iso8601_utc_ms_format(void)
{
	char buf[32] = { 0 };
	struct timespec ts = { .tv_sec = 0, .tv_nsec = 123000000L };
	iso8601_utc_ms(ts, buf, sizeof buf);
	ASSERT_STR_EQ(buf, "1970-01-01T00:00:00.123Z");
}

static void test_iso8601_utc_ms_zero_ms(void)
{
	char buf[32] = { 0 };
	struct timespec ts = { .tv_sec = 946684800L, .tv_nsec = 0 };
	iso8601_utc_ms(ts, buf, sizeof buf);
	ASSERT_STR_EQ(buf, "2000-01-01T00:00:00.000Z");
}

/* ============================================================
 * util.c — read_file_all
 * ============================================================ */

static void test_read_file_all_basic(void)
{
	char path[] = "/tmp/tt_rf_XXXXXX";
	int fd = mkstemp(path);
	ASSERT_TRUE(fd >= 0);
	const char *content = "hello\nworld\n";
	ASSERT_EQ((long)write(fd, content, strlen(content)), (long)strlen(content));
	close(fd);

	char *out = read_file_all(path);
	unlink(path);
	ASSERT_NOT_NULL(out);
	ASSERT_STR_EQ(out, content);
	free(out);
}

static void test_read_file_all_missing(void)
{
	char *out = read_file_all("/tmp/tt_does_not_exist_anywhere_zz");
	ASSERT_NULL(out);
}

static void test_read_file_all_empty(void)
{
	char path[] = "/tmp/tt_rfe_XXXXXX";
	int fd = mkstemp(path);
	ASSERT_TRUE(fd >= 0);
	close(fd);

	char *out = read_file_all(path);
	unlink(path);
	ASSERT_NOT_NULL(out);
	ASSERT_STR_EQ(out, "");
	free(out);
}

/* ============================================================
 * util.c — load_env_file
 * ============================================================ */

static void test_load_env_file_basic(void)
{
	char path[] = "/tmp/tt_env_XXXXXX";
	int fd = mkstemp(path);
	ASSERT_TRUE(fd >= 0);
	const char *body =
		"# comment\n"
		"TT_K1=value1\n"
		"TT_K2=\"quoted value\"\n"
		"TT_K3='single quoted'\n"
		"  TT_K4 =trimmed_key\n";
	ASSERT_EQ((long)write(fd, body, strlen(body)), (long)strlen(body));
	close(fd);

	unsetenv("TT_K1"); unsetenv("TT_K2"); unsetenv("TT_K3"); unsetenv("TT_K4");
	load_env_file(path);
	unlink(path);

	ASSERT_STR_EQ(getenv("TT_K1"), "value1");
	ASSERT_STR_EQ(getenv("TT_K2"), "quoted value");
	ASSERT_STR_EQ(getenv("TT_K3"), "single quoted");
	ASSERT_STR_EQ(getenv("TT_K4"), "trimmed_key");

	unsetenv("TT_K1"); unsetenv("TT_K2"); unsetenv("TT_K3"); unsetenv("TT_K4");
}

static void test_load_env_file_does_not_overwrite(void)
{
	char path[] = "/tmp/tt_envov_XXXXXX";
	int fd = mkstemp(path);
	ASSERT_TRUE(fd >= 0);
	const char *body = "TT_PRESET=from_file\n";
	ASSERT_EQ((long)write(fd, body, strlen(body)), (long)strlen(body));
	close(fd);

	setenv("TT_PRESET", "from_shell", 1);
	load_env_file(path);
	unlink(path);

	ASSERT_STR_EQ(getenv("TT_PRESET"), "from_shell");
	unsetenv("TT_PRESET");
}

static void test_load_env_file_missing_silent(void)
{
	/* must not crash */
	load_env_file("/tmp/tt_does_not_exist_anywhere_zz");
}

/* ============================================================
 * collect.c — is_machine_id
 * ============================================================ */

static void test_is_machine_id_valid(void)
{
	ASSERT_EQ(is_machine_id("a1b2c3d4e5f6a1b2c3d4e5f6a1b2c3d4"), 1);
	ASSERT_EQ(is_machine_id("00000000000000000000000000000000"), 1);
	ASSERT_EQ(is_machine_id("ABCDEFabcdef0123456789aaaabbbbbb"), 1);
}

static void test_is_machine_id_invalid_length(void)
{
	ASSERT_EQ(is_machine_id(""), 0);
	/* 31 chars */
	ASSERT_EQ(is_machine_id("a1b2c3d4e5f6a1b2c3d4e5f6a1b2c3d"), 0);
	/* 33 chars */
	ASSERT_EQ(is_machine_id("a1b2c3d4e5f6a1b2c3d4e5f6a1b2c3d4a"), 0);
}

static void test_is_machine_id_non_hex(void)
{
	ASSERT_EQ(is_machine_id("g1b2c3d4e5f6a1b2c3d4e5f6a1b2c3d4"), 0);
	ASSERT_EQ(is_machine_id("01234567890123456789012345678 12"), 0);
}

/* ============================================================
 * collect.c — meminfo_get_kb
 * ============================================================ */

static void test_meminfo_get_kb_basic(void)
{
	const char *content =
		"MemTotal:       16384000 kB\n"
		"MemFree:         4000000 kB\n"
		"MemAvailable:   12000000 kB\n";
	ASSERT_EQ(meminfo_get_kb(content, "MemTotal"),     16384000);
	ASSERT_EQ(meminfo_get_kb(content, "MemFree"),       4000000);
	ASSERT_EQ(meminfo_get_kb(content, "MemAvailable"), 12000000);
}

static void test_meminfo_get_kb_missing(void)
{
	const char *content = "MemTotal: 1000 kB\n";
	ASSERT_EQ(meminfo_get_kb(content, "MemFree"), -1);
	ASSERT_EQ(meminfo_get_kb(content, "Foo"), -1);
}

static void test_meminfo_get_kb_zero(void)
{
	const char *content = "SwapTotal:       0 kB\n";
	ASSERT_EQ(meminfo_get_kb(content, "SwapTotal"), 0);
}

static void test_meminfo_get_kb_substring_not_match(void)
{
	/* "Mem" must NOT match "MemTotal:..." — exact key+: required */
	const char *content = "MemTotal: 1000 kB\n";
	ASSERT_EQ(meminfo_get_kb(content, "Mem"), -1);
}

/* ============================================================
 * collect.c — read_os_release_field
 * ============================================================ */

static void test_read_os_release_field_quoted_and_unquoted(void)
{
	const char *content =
		"NAME=\"Ubuntu\"\n"
		"VERSION_ID=\"22.04\"\n"
		"ID=ubuntu\n"
		"VERSION_CODENAME=jammy\n";
	char *out = NULL;

	ASSERT_EQ(read_os_release_field(content, "ID", &out), 1);
	ASSERT_STR_EQ(out, "ubuntu");
	free(out);

	out = NULL;
	ASSERT_EQ(read_os_release_field(content, "VERSION_ID", &out), 1);
	ASSERT_STR_EQ(out, "22.04");
	free(out);

	out = NULL;
	ASSERT_EQ(read_os_release_field(content, "VERSION_CODENAME", &out), 1);
	ASSERT_STR_EQ(out, "jammy");
	free(out);

	out = NULL;
	ASSERT_EQ(read_os_release_field(content, "NAME", &out), 1);
	ASSERT_STR_EQ(out, "Ubuntu");
	free(out);
}

static void test_read_os_release_field_missing(void)
{
	const char *content = "ID=ubuntu\n";
	char *out = NULL;
	ASSERT_EQ(read_os_release_field(content, "PRETTY_NAME", &out), 0);
	ASSERT_NULL(out);
}

static void test_read_os_release_field_substring_not_match(void)
{
	/* key "VERSION" must NOT match "VERSION_ID=..." — needs trailing '=' */
	const char *content = "VERSION_ID=22.04\n";
	char *out = NULL;
	ASSERT_EQ(read_os_release_field(content, "VERSION", &out), 0);
	ASSERT_NULL(out);
}

/* ============================================================
 * collect.c — or_empty_array / add_kb_or_null helpers
 * ============================================================ */

static void test_or_empty_array_passthrough(void)
{
	cJSON *arr = cJSON_CreateArray();
	cJSON_AddItemToArray(arr, cJSON_CreateString("x"));
	cJSON *r = or_empty_array(arr);
	ASSERT_TRUE(r == arr);
	ASSERT_EQ(cJSON_GetArraySize(r), 1);
	cJSON_Delete(r);
}

static void test_or_empty_array_null_replaces(void)
{
	cJSON *r = or_empty_array(NULL);
	ASSERT_NOT_NULL(r);
	ASSERT_EQ((long)cJSON_IsArray(r), 1L);
	ASSERT_EQ(cJSON_GetArraySize(r), 0);
	cJSON_Delete(r);
}

static void test_add_kb_or_null_positive(void)
{
	cJSON *obj = cJSON_CreateObject();
	add_kb_or_null(obj, "x", 12345L);
	cJSON *v = cJSON_GetObjectItem(obj, "x");
	ASSERT_NOT_NULL(v);
	ASSERT_EQ((long)cJSON_IsNumber(v), 1L);
	ASSERT_EQ((long)v->valuedouble, 12345L);
	cJSON_Delete(obj);
}

static void test_add_kb_or_null_zero(void)
{
	cJSON *obj = cJSON_CreateObject();
	add_kb_or_null(obj, "x", 0L);
	cJSON *v = cJSON_GetObjectItem(obj, "x");
	ASSERT_NOT_NULL(v);
	ASSERT_EQ((long)cJSON_IsNumber(v), 1L);
	ASSERT_EQ((long)v->valuedouble, 0L);
	cJSON_Delete(obj);
}

static void test_add_kb_or_null_negative(void)
{
	cJSON *obj = cJSON_CreateObject();
	add_kb_or_null(obj, "x", -1L);
	cJSON *v = cJSON_GetObjectItem(obj, "x");
	ASSERT_NOT_NULL(v);
	ASSERT_EQ((long)cJSON_IsNull(v), 1L);
	cJSON_Delete(obj);
}

/* ============================================================
 * runner
 * ============================================================ */

int main(void)
{
	/* util.c — trim_inplace */
	RUN_TEST(test_trim_inplace_basic);
	RUN_TEST(test_trim_inplace_no_trim);
	RUN_TEST(test_trim_inplace_all_whitespace);
	RUN_TEST(test_trim_inplace_null);
	RUN_TEST(test_trim_inplace_internal_whitespace_kept);

	/* util.c — getenv_default */
	RUN_TEST(test_getenv_default_unset);
	RUN_TEST(test_getenv_default_empty);
	RUN_TEST(test_getenv_default_set);

	/* util.c — iso8601 */
	RUN_TEST(test_iso8601_utc_epoch);
	RUN_TEST(test_iso8601_utc_known);
	RUN_TEST(test_iso8601_utc_ms_format);
	RUN_TEST(test_iso8601_utc_ms_zero_ms);

	/* util.c — read_file_all */
	RUN_TEST(test_read_file_all_basic);
	RUN_TEST(test_read_file_all_missing);
	RUN_TEST(test_read_file_all_empty);

	/* util.c — load_env_file */
	RUN_TEST(test_load_env_file_basic);
	RUN_TEST(test_load_env_file_does_not_overwrite);
	RUN_TEST(test_load_env_file_missing_silent);

	/* collect.c — is_machine_id */
	RUN_TEST(test_is_machine_id_valid);
	RUN_TEST(test_is_machine_id_invalid_length);
	RUN_TEST(test_is_machine_id_non_hex);

	/* collect.c — meminfo_get_kb */
	RUN_TEST(test_meminfo_get_kb_basic);
	RUN_TEST(test_meminfo_get_kb_missing);
	RUN_TEST(test_meminfo_get_kb_zero);
	RUN_TEST(test_meminfo_get_kb_substring_not_match);

	/* collect.c — read_os_release_field */
	RUN_TEST(test_read_os_release_field_quoted_and_unquoted);
	RUN_TEST(test_read_os_release_field_missing);
	RUN_TEST(test_read_os_release_field_substring_not_match);

	/* collect.c — helpers */
	RUN_TEST(test_or_empty_array_passthrough);
	RUN_TEST(test_or_empty_array_null_replaces);
	RUN_TEST(test_add_kb_or_null_positive);
	RUN_TEST(test_add_kb_or_null_zero);
	RUN_TEST(test_add_kb_or_null_negative);

	return tt_summary();
}
