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
 * v3 — collect.c — is_excluded_block_dev
 * ============================================================ */

static void test_is_excluded_block_dev_loop(void)
{
	ASSERT_EQ(is_excluded_block_dev("loop0"),  1);
	ASSERT_EQ(is_excluded_block_dev("loop42"), 1);
}

static void test_is_excluded_block_dev_ram_sr_fd(void)
{
	ASSERT_EQ(is_excluded_block_dev("ram0"), 1);
	ASSERT_EQ(is_excluded_block_dev("sr0"),  1);
	ASSERT_EQ(is_excluded_block_dev("sr15"), 1);
	ASSERT_EQ(is_excluded_block_dev("fd0"),  1);
}

static void test_is_excluded_block_dev_real_disks_kept(void)
{
	ASSERT_EQ(is_excluded_block_dev("sda"),   0);
	ASSERT_EQ(is_excluded_block_dev("vda"),   0);
	ASSERT_EQ(is_excluded_block_dev("nvme0n1"), 0);
	ASSERT_EQ(is_excluded_block_dev("xvda"),  0);
	ASSERT_EQ(is_excluded_block_dev("dm-0"),  0);
}

static void test_is_excluded_block_dev_null_empty(void)
{
	ASSERT_EQ(is_excluded_block_dev(NULL), 1);
	ASSERT_EQ(is_excluded_block_dev(""),   1);
}

/* "ramen" is not a real device but proves the prefix policy fires on
 * 3 chars; documents that the helper is intentionally a prefix match,
 * not a wholename match. */
static void test_is_excluded_block_dev_prefix_policy(void)
{
	ASSERT_EQ(is_excluded_block_dev("ramen"), 1);
	ASSERT_EQ(is_excluded_block_dev("loopxx"), 1);
}

/* ============================================================
 * v3 — collect.c — parse_major_minor
 * ============================================================ */

static void test_parse_major_minor_basic(void)
{
	int mj = -1, mn = -1;
	ASSERT_EQ(parse_major_minor("252:0", &mj, &mn), 1);
	ASSERT_EQ(mj, 252);
	ASSERT_EQ(mn, 0);
}

static void test_parse_major_minor_two_digit(void)
{
	int mj = -1, mn = -1;
	ASSERT_EQ(parse_major_minor("8:16", &mj, &mn), 1);
	ASSERT_EQ(mj, 8);
	ASSERT_EQ(mn, 16);
}

static void test_parse_major_minor_invalid(void)
{
	int mj = 99, mn = 99;
	ASSERT_EQ(parse_major_minor("bad", &mj, &mn), 0);
	ASSERT_EQ(mj, -1);
	ASSERT_EQ(mn, -1);
	mj = mn = 99;
	ASSERT_EQ(parse_major_minor(":5", &mj, &mn), 0);
	mj = mn = 99;
	ASSERT_EQ(parse_major_minor("5:", &mj, &mn), 0);
	mj = mn = 99;
	ASSERT_EQ(parse_major_minor(NULL, &mj, &mn), 0);
}

/* ============================================================
 * v3 — collect.c — is_excluded_fstype
 * ============================================================ */

static void test_is_excluded_fstype_pseudo(void)
{
	ASSERT_EQ(is_excluded_fstype("proc"),     1);
	ASSERT_EQ(is_excluded_fstype("sysfs"),    1);
	ASSERT_EQ(is_excluded_fstype("cgroup2"),  1);
	ASSERT_EQ(is_excluded_fstype("tmpfs"),    1);
	ASSERT_EQ(is_excluded_fstype("squashfs"), 1);
	ASSERT_EQ(is_excluded_fstype("overlay"),  1);
	ASSERT_EQ(is_excluded_fstype("nsfs"),     1);
}

static void test_is_excluded_fstype_real(void)
{
	ASSERT_EQ(is_excluded_fstype("ext4"), 0);
	ASSERT_EQ(is_excluded_fstype("xfs"),  0);
	ASSERT_EQ(is_excluded_fstype("btrfs"), 0);
	ASSERT_EQ(is_excluded_fstype("zfs"),  0);
}

static void test_is_excluded_fstype_null(void)
{
	ASSERT_EQ(is_excluded_fstype(NULL), 1);
}

/* ============================================================
 * v3 — collect.c — parse_mountinfo_line
 * ============================================================ */

static void test_parse_mountinfo_line_basic(void)
{
	const char *line =
		"36 35 252:1 / /mnt/data rw,relatime - ext4 /dev/vda1 rw,errors=remount-ro";
	int mj = -1, mn = -1;
	char *mnt = NULL, *fst = NULL;
	ASSERT_EQ(parse_mountinfo_line(line, &mj, &mn, &mnt, &fst), 1);
	ASSERT_EQ(mj, 252);
	ASSERT_EQ(mn, 1);
	ASSERT_STR_EQ(mnt, "/mnt/data");
	ASSERT_STR_EQ(fst, "ext4");
	free(mnt); free(fst);
}

/* mountinfo with one optional field (`shared:1`). The parser must skip
 * variable-length optional region until the literal `-` separator. */
static void test_parse_mountinfo_line_one_optional(void)
{
	const char *line =
		"22 28 0:21 / /sys rw,nosuid shared:7 - sysfs sysfs rw";
	int mj = -1, mn = -1;
	char *mnt = NULL, *fst = NULL;
	ASSERT_EQ(parse_mountinfo_line(line, &mj, &mn, &mnt, &fst), 1);
	ASSERT_EQ(mj, 0);
	ASSERT_EQ(mn, 21);
	ASSERT_STR_EQ(mnt, "/sys");
	ASSERT_STR_EQ(fst, "sysfs");
	free(mnt); free(fst);
}

/* Multiple optional fields. Real-world mountinfo can have 3-4+ tags such
 * as `shared:N master:M propagate_from:K unbindable`. The parser must
 * still correctly locate the `-` separator. */
static void test_parse_mountinfo_line_many_optionals(void)
{
	const char *line =
		"99 1 8:1 /sub /mnt/x rw,relatime "
		"shared:1 master:2 propagate_from:3 unbindable "
		"- xfs /dev/sda1 rw,attr2,inode64";
	int mj = -1, mn = -1;
	char *mnt = NULL, *fst = NULL;
	ASSERT_EQ(parse_mountinfo_line(line, &mj, &mn, &mnt, &fst), 1);
	ASSERT_EQ(mj, 8);
	ASSERT_EQ(mn, 1);
	ASSERT_STR_EQ(mnt, "/mnt/x");
	ASSERT_STR_EQ(fst, "xfs");
	free(mnt); free(fst);
}

static void test_parse_mountinfo_line_missing_dash(void)
{
	/* No `-` separator → must reject. */
	const char *line = "1 2 3:4 / /mnt rw shared:1 only_optionals_no_dash";
	int mj = 99, mn = 99;
	char *mnt = NULL, *fst = NULL;
	ASSERT_EQ(parse_mountinfo_line(line, &mj, &mn, &mnt, &fst), 0);
	ASSERT_NULL(mnt);
	ASSERT_NULL(fst);
}

static void test_parse_mountinfo_line_too_few_fields(void)
{
	const char *line = "1 2 3:4";
	int mj = 99, mn = 99;
	char *mnt = NULL, *fst = NULL;
	ASSERT_EQ(parse_mountinfo_line(line, &mj, &mn, &mnt, &fst), 0);
	ASSERT_NULL(mnt);
	ASSERT_NULL(fst);
}

static void test_parse_mountinfo_line_bad_majmin(void)
{
	const char *line = "1 2 BAD / /mnt rw - ext4 /dev/sda rw";
	int mj = 99, mn = 99;
	char *mnt = NULL, *fst = NULL;
	ASSERT_EQ(parse_mountinfo_line(line, &mj, &mn, &mnt, &fst), 0);
	ASSERT_NULL(mnt);
	ASSERT_NULL(fst);
}

/* ============================================================
 * v3 — collect.c — dedup_mounts
 * ============================================================ */

static void test_dedup_mounts_keeps_first(void)
{
	struct mount_entry arr[3] = {
		{ 252, 1, strdup("/"),     strdup("ext4") },
		{ 252, 1, strdup("/bind"), strdup("ext4") }, /* dup */
		{ 8,   1, strdup("/data"), strdup("xfs")  },
	};
	size_t n = 3;
	dedup_mounts(arr, &n);
	ASSERT_EQ((long)n, 2L);
	ASSERT_STR_EQ(arr[0].mount, "/");
	ASSERT_EQ(arr[0].major, 252);
	ASSERT_STR_EQ(arr[1].mount, "/data");
	ASSERT_EQ(arr[1].major, 8);
	free(arr[0].mount); free(arr[0].fstype);
	free(arr[1].mount); free(arr[1].fstype);
}

static void test_dedup_mounts_no_dups_passthrough(void)
{
	struct mount_entry arr[2] = {
		{ 252, 1, strdup("/"),     strdup("ext4") },
		{ 8,   1, strdup("/data"), strdup("xfs")  },
	};
	size_t n = 2;
	dedup_mounts(arr, &n);
	ASSERT_EQ((long)n, 2L);
	free(arr[0].mount); free(arr[0].fstype);
	free(arr[1].mount); free(arr[1].fstype);
}

static void test_dedup_mounts_empty(void)
{
	struct mount_entry *arr = NULL;
	size_t n = 0;
	dedup_mounts(arr, &n);
	ASSERT_EQ((long)n, 0L);
}

/* ============================================================
 * v3 — collect.c — parse_tcp_v4_hex_addr
 * ============================================================ */

static void test_parse_tcp_v4_hex_addr_localhost(void)
{
	/* 127.0.0.1 in network order = 0x7F000001 → printed in host order
	 * on x86_64 little-endian as "0100007F". */
	char buf[INET_ADDRSTRLEN] = { 0 };
	parse_tcp_v4_hex_addr("0100007F", buf, sizeof buf);
	ASSERT_STR_EQ(buf, "127.0.0.1");
}

static void test_parse_tcp_v4_hex_addr_any(void)
{
	char buf[INET_ADDRSTRLEN] = { 0 };
	parse_tcp_v4_hex_addr("00000000", buf, sizeof buf);
	ASSERT_STR_EQ(buf, "0.0.0.0");
}

/* RFC 1918 "192.168.1.10" → network 0xC0A8010A → host LE "0A01A8C0". */
static void test_parse_tcp_v4_hex_addr_rfc1918(void)
{
	char buf[INET_ADDRSTRLEN] = { 0 };
	parse_tcp_v4_hex_addr("0A01A8C0", buf, sizeof buf);
	ASSERT_STR_EQ(buf, "192.168.1.10");
}

/* ============================================================
 * v3 — collect.c — parse_tcp_v6_hex_addr
 * ============================================================ */

static void test_parse_tcp_v6_hex_addr_unspecified(void)
{
	char buf[INET6_ADDRSTRLEN] = { 0 };
	parse_tcp_v6_hex_addr(
		"00000000000000000000000000000000", buf, sizeof buf);
	ASSERT_STR_EQ(buf, "::");
}

static void test_parse_tcp_v6_hex_addr_loopback(void)
{
	/* ::1 in network bytes = 00..00 01. Stored as 4 LE u32:
	 * dwords = 0, 0, 0, 0x01000000 → hex chars: 8x'0' 8x'0' 8x'0' "01000000". */
	char buf[INET6_ADDRSTRLEN] = { 0 };
	parse_tcp_v6_hex_addr(
		"00000000000000000000000001000000", buf, sizeof buf);
	ASSERT_STR_EQ(buf, "::1");
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

	/* v3 — collect.c — is_excluded_block_dev */
	RUN_TEST(test_is_excluded_block_dev_loop);
	RUN_TEST(test_is_excluded_block_dev_ram_sr_fd);
	RUN_TEST(test_is_excluded_block_dev_real_disks_kept);
	RUN_TEST(test_is_excluded_block_dev_null_empty);
	RUN_TEST(test_is_excluded_block_dev_prefix_policy);

	/* v3 — collect.c — parse_major_minor */
	RUN_TEST(test_parse_major_minor_basic);
	RUN_TEST(test_parse_major_minor_two_digit);
	RUN_TEST(test_parse_major_minor_invalid);

	/* v3 — collect.c — is_excluded_fstype */
	RUN_TEST(test_is_excluded_fstype_pseudo);
	RUN_TEST(test_is_excluded_fstype_real);
	RUN_TEST(test_is_excluded_fstype_null);

	/* v3 — collect.c — parse_mountinfo_line */
	RUN_TEST(test_parse_mountinfo_line_basic);
	RUN_TEST(test_parse_mountinfo_line_one_optional);
	RUN_TEST(test_parse_mountinfo_line_many_optionals);
	RUN_TEST(test_parse_mountinfo_line_missing_dash);
	RUN_TEST(test_parse_mountinfo_line_too_few_fields);
	RUN_TEST(test_parse_mountinfo_line_bad_majmin);

	/* v3 — collect.c — dedup_mounts */
	RUN_TEST(test_dedup_mounts_keeps_first);
	RUN_TEST(test_dedup_mounts_no_dups_passthrough);
	RUN_TEST(test_dedup_mounts_empty);

	/* v3 — collect.c — parse_tcp_v4_hex_addr */
	RUN_TEST(test_parse_tcp_v4_hex_addr_localhost);
	RUN_TEST(test_parse_tcp_v4_hex_addr_any);
	RUN_TEST(test_parse_tcp_v4_hex_addr_rfc1918);

	/* v3 — collect.c — parse_tcp_v6_hex_addr */
	RUN_TEST(test_parse_tcp_v6_hex_addr_unspecified);
	RUN_TEST(test_parse_tcp_v6_hex_addr_loopback);

	return tt_summary();
}
