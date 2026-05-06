/**
 * @file collect.c
 * @brief v2 inventory and metrics collectors built on /proc, /sys, syscalls.
 *
 * Output schema lives in docs/payload-schema.md (mirrored by the engine).
 * Stateless: cumulative `/proc` counters are emitted as-is; the engine
 * computes deltas, rates, and percentages from two consecutive snapshots.
 */

#define _POSIX_C_SOURCE 200809L
#define _GNU_SOURCE

#include "collect.h"
#include "util.h"

#include <cjson/cJSON.h>

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <ifaddrs.h>
#include <limits.h>
#include <net/if.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/statvfs.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>

#define AGENT_VERSION_FALLBACK "0.0.0-dev"

/* ============================================================
 * Common metadata
 * ============================================================ */

/**
 * @brief Add common metadata fields to @p obj.
 *
 * Fields: message_type, machine_id, agent_version, collected_at,
 * hostname, message_id.
 */
static void add_common_metadata(cJSON *obj,
                                const char *message_type,
                                const char *machine_id,
                                const char *agent_version)
{
	cJSON_AddStringToObject(obj, "message_type", message_type);
	cJSON_AddStringToObject(obj, "machine_id",
	                        machine_id && *machine_id ? machine_id : "");
	cJSON_AddStringToObject(obj, "agent_version",
	                        agent_version && *agent_version ? agent_version
	                                                        : AGENT_VERSION_FALLBACK);

	struct timespec ts;
	clock_gettime(CLOCK_REALTIME, &ts);
	char ts_buf[32];
	iso8601_utc(ts.tv_sec, ts_buf, sizeof ts_buf);
	cJSON_AddStringToObject(obj, "collected_at", ts_buf);

	const char *override = getenv("AGENT_HOSTNAME_OVERRIDE");
	char host_buf[HOST_NAME_MAX + 1];
	if (override && *override) {
		cJSON_AddStringToObject(obj, "hostname", override);
	} else if (gethostname(host_buf, sizeof host_buf) == 0) {
		host_buf[sizeof host_buf - 1] = '\0';
		cJSON_AddStringToObject(obj, "hostname", host_buf);
	} else {
		cJSON_AddStringToObject(obj, "hostname", "unknown");
	}

	char uuid_buf[64];
	uuid_v4(uuid_buf, sizeof uuid_buf);
	cJSON_AddStringToObject(obj, "message_id", uuid_buf);
}

/* ============================================================
 * Small helpers
 * ============================================================ */

/**
 * @brief Return @p arr if non-NULL, else a freshly created empty array.
 *
 * Guards JSON output against array fields becoming missing on
 * cJSON_CreateArray() failure inside a collector.
 */
static cJSON *or_empty_array(cJSON *arr)
{
	return arr ? arr : cJSON_CreateArray();
}

/**
 * @brief Add @p key as a kB number, or null if @p val is negative.
 *
 * Negative inputs come from meminfo_get_kb() when a line is missing or
 * unparseable. "Couldn't read" is encoded as JSON null rather than 0
 * so it can be distinguished from a real zero reading downstream.
 */
static void add_kb_or_null(cJSON *root, const char *key, long val)
{
	if (val < 0) cJSON_AddNullToObject(root, key);
	else         cJSON_AddNumberToObject(root, key, (double)val);
}

/**
 * @brief Block device names the agent always excludes from disks[] / disk_io[].
 *
 * Universal noise across all target OSes:
 *   - loop  : snap (Ubuntu), flatpak, losetup-mounted images
 *   - ram   : ramdisk (always present, never analytical interest)
 *   - sr    : optical drives / virtual CDROMs (cloud-init seed iso)
 *   - fd    : floppy (legacy, but still enumerated by some kernels)
 * Centralized here so lsblk / sysfs / diskstats apply identical policy.
 */
static int is_excluded_block_dev(const char *name)
{
	if (!name || !*name) return 1;
	return strncmp(name, "loop", 4) == 0
	    || strncmp(name, "ram",  3) == 0
	    || strncmp(name, "sr",   2) == 0
	    || strncmp(name, "fd",   2) == 0;
}

/**
 * @brief Parse "MAJOR:MINOR" into two ints.
 *
 * Used by both lsblk JSON (`MAJ:MIN` column) and `/sys/block/<dev>/dev`
 * sysfs file. Returns 1 on success and writes both ints; on failure both
 * stay -1 so the caller can choose to omit the JSON fields.
 */
static int parse_major_minor(const char *s, int *major, int *minor)
{
	*major = -1;
	*minor = -1;
	if (!s) return 0;
	const char *colon = strchr(s, ':');
	if (!colon || colon == s) return 0;       /* require non-empty major */
	char *end = NULL;
	long mj = strtol(s, &end, 10);
	if (end != colon) return 0;
	long mn = strtol(colon + 1, &end, 10);
	if (end == colon + 1) return 0;            /* require non-empty minor */
	*major = (int)mj;
	*minor = (int)mn;
	return 1;
}

/**
 * @brief Add `major`/`minor` int fields to @p obj, or skip when negative.
 */
static void add_major_minor(cJSON *obj, int major, int minor)
{
	if (major >= 0) cJSON_AddNumberToObject(obj, "major", (double)major);
	if (minor >= 0) cJSON_AddNumberToObject(obj, "minor", (double)minor);
}

/* ============================================================
 * machine_id resolution
 * ============================================================ */

/**
 * @brief 32-char hex check (machine-id format).
 */
static int is_machine_id(const char *s)
{
	size_t n = 0;
	for (const char *p = s; *p; p++) {
		if (!isxdigit((unsigned char)*p))
			return 0;
		n++;
	}
	return n == 32;
}

static char *try_machine_id_file(const char *path)
{
	char *raw = read_file_all(path);
	if (!raw)
		return NULL;
	trim_inplace(raw);
	if (!is_machine_id(raw)) {
		free(raw);
		return NULL;
	}
	return raw;
}

static char *try_dbus_uuidgen(void)
{
	char *raw = run_cmd("dbus-uuidgen --get 2>/dev/null");
	if (!raw)
		return NULL;
	trim_inplace(raw);
	if (!*raw || !is_machine_id(raw)) {
		free(raw);
		return NULL;
	}
	return raw;
}

/**
 * @brief Detect cloud vendor by /sys/class/dmi/id/sys_vendor.
 *
 * Returns "aws" / "azure" / "gcp" / NULL. Result is statically allocated.
 */
static const char *detect_cloud_vendor(void)
{
	char *vendor = read_file_all("/sys/class/dmi/id/sys_vendor");
	if (!vendor)
		return NULL;
	trim_inplace(vendor);

	const char *result = NULL;
	if (strstr(vendor, "Amazon"))
		result = "aws";
	else if (strstr(vendor, "Microsoft"))
		result = "azure";
	else if (strstr(vendor, "Google"))
		result = "gcp";
	/* Legacy AWS Xen instances had sys_vendor="Xen". Modern Nitro is
	 * "Amazon EC2", and non-AWS Xen IaaS exists, so we no longer treat
	 * a bare "Xen" vendor as AWS. Hosts that lose vendor detection
	 * fall back to /etc/machine-id and dbus-uuidgen. */

	free(vendor);
	return result;
}

/**
 * @brief Fetch instance-id from cloud metadata service.
 *
 * Best-effort: any failure returns NULL. The agent then falls back to
 * other strategies. 1-second timeout to avoid blocking on non-cloud hosts.
 */
static char *try_cloud_instance_id(void)
{
	const char *vendor = detect_cloud_vendor();
	if (!vendor)
		return NULL;

	char *out = NULL;
	if (strcmp(vendor, "aws") == 0) {
		/* IMDSv2 — token first, then fetch. */
		char *token = run_cmd(
			"curl -fsS -m 1 -X PUT "
			"-H 'X-aws-ec2-metadata-token-ttl-seconds: 60' "
			"http://169.254.169.254/latest/api/token 2>/dev/null");
		if (token && *token) {
			trim_inplace(token);
			char cmd[512];
			snprintf(cmd, sizeof cmd,
			         "curl -fsS -m 1 -H 'X-aws-ec2-metadata-token: %s' "
			         "http://169.254.169.254/latest/meta-data/instance-id 2>/dev/null",
			         token);
			out = run_cmd(cmd);
		}
		free(token);
	} else if (strcmp(vendor, "azure") == 0) {
		out = run_cmd(
			"curl -fsS -m 1 -H 'Metadata: true' "
			"'http://169.254.169.254/metadata/instance/compute/vmId"
			"?api-version=2021-02-01&format=text' 2>/dev/null");
	} else if (strcmp(vendor, "gcp") == 0) {
		out = run_cmd(
			"curl -fsS -m 1 -H 'Metadata-Flavor: Google' "
			"http://metadata.google.internal/computeMetadata/v1/instance/id 2>/dev/null");
	}

	if (out)
		trim_inplace(out);
	if (!out || !*out) {
		free(out);
		return NULL;
	}
	return out;
}

char *resolve_machine_id(void)
{
	char *id = try_machine_id_file("/etc/machine-id");
	if (id)
		return id;
	id = try_machine_id_file("/var/lib/dbus/machine-id");
	if (id)
		return id;
	id = try_dbus_uuidgen();
	if (id)
		return id;
	return try_cloud_instance_id();
}

/* ============================================================
 * Inventory collectors
 * ============================================================ */

/**
 * @brief Parse `KEY=VALUE` line where VALUE may be quoted.
 *
 * Writes a malloc'd string to *@p out and returns 1 if found, 0 otherwise.
 */
static int read_os_release_field(const char *content, const char *key, char **out)
{
	*out = NULL;
	size_t key_len = strlen(key);
	const char *p = content;
	while (*p) {
		const char *eol = strchr(p, '\n');
		size_t len = eol ? (size_t)(eol - p) : strlen(p);
		if (len > key_len && strncmp(p, key, key_len) == 0 && p[key_len] == '=') {
			const char *v = p + key_len + 1;
			size_t vlen = len - key_len - 1;
			if (vlen >= 2 && v[0] == '"' && v[vlen - 1] == '"') {
				v++;
				vlen -= 2;
			}
			char *r = malloc(vlen + 1);
			if (!r)
				return 0;
			memcpy(r, v, vlen);
			r[vlen] = '\0';
			*out = r;
			return 1;
		}
		if (!eol)
			break;
		p = eol + 1;
	}
	return 0;
}

/**
 * @brief Add `os_id`, `os_version`, `os_codename` to @p root.
 */
static int add_os_release(cJSON *root)
{
	char *content = read_file_all("/etc/os-release");
	if (!content) {
		cJSON_AddNullToObject(root, "os_id");
		cJSON_AddNullToObject(root, "os_version");
		cJSON_AddNullToObject(root, "os_codename");
		return 0;
	}

	char *id = NULL, *ver = NULL, *code = NULL;
	read_os_release_field(content, "ID", &id);
	read_os_release_field(content, "VERSION_ID", &ver);
	read_os_release_field(content, "VERSION_CODENAME", &code);

	id   ? cJSON_AddStringToObject(root, "os_id", id)         : cJSON_AddNullToObject(root, "os_id");
	ver  ? cJSON_AddStringToObject(root, "os_version", ver)   : cJSON_AddNullToObject(root, "os_version");
	code ? cJSON_AddStringToObject(root, "os_codename", code) : cJSON_AddNullToObject(root, "os_codename");

	free(id); free(ver); free(code); free(content);
	return 1;
}

static int add_kernel_version(cJSON *root)
{
	struct utsname u;
	if (uname(&u) != 0)
		return 0;
	cJSON_AddStringToObject(root, "kernel_version", u.release);
	return 1;
}

static int add_cpu_cores(cJSON *root)
{
	long n = sysconf(_SC_NPROCESSORS_ONLN);
	if (n <= 0)
		return 0;
	cJSON_AddNumberToObject(root, "cpu_cores", (double)n);
	return 1;
}

static void add_cpu_model(cJSON *root)
{
	char *content = read_file_all("/proc/cpuinfo");
	if (!content) {
		cJSON_AddNullToObject(root, "cpu_model");
		return;
	}
	const char *p = content;
	while (*p) {
		if (strncmp(p, "model name", 10) == 0) {
			const char *colon = strchr(p, ':');
			if (colon) {
				const char *v = colon + 1;
				while (*v == ' ' || *v == '\t')
					v++;
				const char *eol = strchr(v, '\n');
				size_t vlen = eol ? (size_t)(eol - v) : strlen(v);
				char *r = malloc(vlen + 1);
				if (r) {
					memcpy(r, v, vlen);
					r[vlen] = '\0';
					trim_inplace(r);
					cJSON_AddStringToObject(root, "cpu_model", r);
					free(r);
					free(content);
					return;
				}
			}
		}
		const char *eol = strchr(p, '\n');
		if (!eol) break;
		p = eol + 1;
	}
	cJSON_AddNullToObject(root, "cpu_model");
	free(content);
}

/**
 * @brief Read a single kB-suffixed value from /proc/meminfo.
 *
 * @return >= 0 on success, -1 on missing key or parse failure.
 */
static long meminfo_get_kb(const char *content, const char *key)
{
	size_t key_len = strlen(key);
	const char *p = content;
	while (*p) {
		const char *eol = strchr(p, '\n');
		size_t len = eol ? (size_t)(eol - p) : strlen(p);
		if (len > key_len && strncmp(p, key, key_len) == 0 && p[key_len] == ':') {
			const char *v = p + key_len + 1;
			while (*v == ' ' || *v == '\t')
				v++;
			char *end;
			long val = strtol(v, &end, 10);
			if (end != v)
				return val;
			return -1;
		}
		if (!eol) break;
		p = eol + 1;
	}
	return -1;
}

static int add_mem_total_swap_total(cJSON *root)
{
	char *content = read_file_all("/proc/meminfo");
	if (!content)
		return 0;
	long mem_total = meminfo_get_kb(content, "MemTotal");
	long swap_total = meminfo_get_kb(content, "SwapTotal");
	free(content);
	if (mem_total < 0)
		return 0;
	cJSON_AddNumberToObject(root, "mem_total_kb", (double)mem_total);
	add_kb_or_null(root, "swap_total_kb", swap_total);
	return 1;
}

/**
 * @brief Collect disk list via lsblk JSON output.
 *
 * Command: `lsblk -dn -b -e 7,11 -o NAME,MAJ:MIN,SIZE,TYPE -J`
 *   -e 7,11 excludes loop (major 7) and sr/cdrom (major 11) at source.
 *   MAJ:MIN is added so disks[] can be joined with mounts[] / disk_io[].
 *
 * Defense-in-depth: even with `-e`, the name prefix filter
 * (`is_excluded_block_dev`) drops anything that slipped through.
 *
 * Returns an empty array if lsblk is missing or output is unparsable
 * (older util-linux predates `-J`, e.g. CentOS 7's 2.23 — see
 * collect_disks for the sysfs fallback path).
 */
static cJSON *collect_disks_via_lsblk(void)
{
	cJSON *arr = cJSON_CreateArray();
	if (!arr)
		return NULL;

	char *out = run_cmd("lsblk -dn -b -e 7,11 -o NAME,MAJ:MIN,SIZE,TYPE -J 2>/dev/null");
	if (!out)
		return arr;

	cJSON *parsed = cJSON_Parse(out);
	free(out);
	if (!parsed)
		return arr;

	cJSON *devices = cJSON_GetObjectItemCaseSensitive(parsed, "blockdevices");
	cJSON *dev = NULL;
	cJSON_ArrayForEach(dev, devices) {
		cJSON *name = cJSON_GetObjectItemCaseSensitive(dev, "name");
		/* util-linux pre-2.33 emits the JSON key as "MAJ:MIN" (uppercase);
		 * 2.33+ emits "maj:min". Try lowercase first, then uppercase. */
		cJSON *majmin = cJSON_GetObjectItemCaseSensitive(dev, "maj:min");
		if (!majmin)
			majmin = cJSON_GetObjectItemCaseSensitive(dev, "MAJ:MIN");
		cJSON *size = cJSON_GetObjectItemCaseSensitive(dev, "size");
		cJSON *type = cJSON_GetObjectItemCaseSensitive(dev, "type");
		if (!cJSON_IsString(name))
			continue;
		if (is_excluded_block_dev(name->valuestring))
			continue;

		int major = -1, minor = -1;
		if (cJSON_IsString(majmin))
			parse_major_minor(majmin->valuestring, &major, &minor);

		cJSON *item = cJSON_CreateObject();
		cJSON_AddStringToObject(item, "name", name->valuestring);
		add_major_minor(item, major, minor);

		double size_bytes = 0;
		if (cJSON_IsNumber(size))
			size_bytes = size->valuedouble;
		else if (cJSON_IsString(size))
			size_bytes = strtod(size->valuestring, NULL);
		cJSON_AddNumberToObject(item, "size_bytes", size_bytes);

		cJSON_AddStringToObject(item, "type",
		                        cJSON_IsString(type) ? type->valuestring : "disk");
		cJSON_AddItemToArray(arr, item);
	}
	cJSON_Delete(parsed);
	return arr;
}

/**
 * @brief Fallback disk list from /sys/block (no lsblk required).
 *
 * Used on hosts where lsblk is missing or too old for `-J` (e.g. CentOS 7
 * util-linux 2.23). Applies the same exclusion policy as the lsblk path
 * via `is_excluded_block_dev`. Skips entries without `/sys/block/<name>/device`
 * (synthetic devices). `major`/`minor` come from `/sys/block/<name>/dev`.
 * `type` is always `"disk"` since sysfs does not classify rotational vs SSD
 * vs LVM at this layer.
 */
static cJSON *collect_disks_via_sysfs(void)
{
	cJSON *arr = cJSON_CreateArray();
	if (!arr)
		return NULL;

	DIR *d = opendir("/sys/block");
	if (!d)
		return arr;

	struct dirent *e;
	while ((e = readdir(d)) != NULL) {
		if (e->d_name[0] == '.')
			continue;
		if (is_excluded_block_dev(e->d_name))
			continue;

		char path[512];
		snprintf(path, sizeof path, "/sys/block/%s/device", e->d_name);
		if (access(path, F_OK) != 0)
			continue;

		snprintf(path, sizeof path, "/sys/block/%s/size", e->d_name);
		char *content = read_file_all(path);
		if (!content)
			continue;
		long sectors = strtol(content, NULL, 10);
		free(content);
		if (sectors <= 0)
			continue;

		int major = -1, minor = -1;
		snprintf(path, sizeof path, "/sys/block/%s/dev", e->d_name);
		char *dev_str = read_file_all(path);
		if (dev_str) {
			trim_inplace(dev_str);
			parse_major_minor(dev_str, &major, &minor);
			free(dev_str);
		}

		cJSON *item = cJSON_CreateObject();
		cJSON_AddStringToObject(item, "name", e->d_name);
		add_major_minor(item, major, minor);
		cJSON_AddNumberToObject(item, "size_bytes", (double)sectors * 512.0);
		cJSON_AddStringToObject(item, "type", "disk");
		cJSON_AddItemToArray(arr, item);
	}
	closedir(d);
	return arr;
}

/**
 * @brief Disk list with lsblk-first, /sys/block fallback.
 *
 * lsblk knows partition vs disk vs LVM types and reports byte-accurate
 * sizes, so it is preferred. If lsblk is unavailable (missing binary,
 * pre-2.27 util-linux without `-J`) or returns no entries, we fall through
 * to a minimal /sys/block scan that always works on Linux 2.6+.
 */
static cJSON *collect_disks(void)
{
	cJSON *arr = collect_disks_via_lsblk();
	if (arr && cJSON_GetArraySize(arr) > 0)
		return arr;
	cJSON_Delete(arr);
	return collect_disks_via_sysfs();
}

/**
 * @brief Pseudo / virtual filesystems excluded from mounts[].
 *
 * These exist for kernel/snap/k8s interfaces, not user data storage.
 * Centralised so inventory.mounts[] and metrics.mounts[] always agree.
 *
 *   nsfs     — k8s / container namespaces
 *   squashfs — snap (Ubuntu) and other read-only image mounts
 *   overlay  — container layered fs (docker/podman/k8s)
 *   tmpfs/devtmpfs/proc/sysfs/cgroup* etc — kernel pseudo-fs
 */
static int is_excluded_fstype(const char *fstype)
{
	static const char *skip_fs[] = {
		"proc", "sysfs", "cgroup", "cgroup2", "devpts", "tmpfs",
		"devtmpfs", "mqueue", "hugetlbfs", "fusectl", "debugfs",
		"tracefs", "securityfs", "pstore", "autofs", "rpc_pipefs",
		"binfmt_misc", "configfs", "bpf", "ramfs", "overlay", "squashfs",
		"nsfs",
		/* container / VM virtual fs */
		"9p", "virtiofs", "fuse.lxcfs", "fuse.gvfsd-fuse",
		NULL,
	};
	if (!fstype) return 1;
	for (int i = 0; skip_fs[i]; i++)
		if (strcmp(fstype, skip_fs[i]) == 0)
			return 1;
	return 0;
}

/**
 * @brief Single mount entry produced by list_real_mounts().
 *
 * `mount` and `fstype` are owned malloc'd strings. (major,minor) is the
 * device id parsed from /proc/self/mountinfo field 3. Used as the dedup
 * key so bind mounts (same device, multiple mountpoints) only count once.
 */
struct mount_entry {
	int   major;
	int   minor;
	char *mount;
	char *fstype;
};

static void free_mount_entries(struct mount_entry *arr, size_t n)
{
	if (!arr) return;
	for (size_t i = 0; i < n; i++) {
		free(arr[i].mount);
		free(arr[i].fstype);
	}
	free(arr);
}

/**
 * @brief Parse a single /proc/self/mountinfo line.
 *
 * Format (man 5 proc):
 *   ID PARENT MAJOR:MINOR ROOT MOUNTPOINT MOUNT_OPTS [optional...] - FSTYPE SOURCE SUPER_OPTS
 *
 * Single-pass token scan — only positions 3 (maj:min), 5 (mount), and
 * the field after "-" (fstype) are captured. Avoids any fixed-size
 * fields[] cap so hosts with many optional fields (mount propagation
 * tags: shared:N master:M propagate_from:K unbindable, …) parse safely.
 *
 * Mountpoints with whitespace appear as `\040`-escaped — left as-is
 * (rare in our target environments; engine can decode if it matters).
 *
 * @return 1 on full success and writes @p major, @p minor, @p mount_out
 *         (malloc'd), @p fstype_out (malloc'd). 0 on parse failure
 *         (out-pointers untouched).
 */
static int parse_mountinfo_line(const char *line,
                                int *major, int *minor,
                                char **mount_out, char **fstype_out)
{
	char *copy = strdup(line);
	if (!copy) return 0;

	int mj = -1, mn = -1;
	char *mnt = NULL, *fst = NULL;
	int seen_dash = 0;
	int idx = 0;

	char *save = NULL;
	for (char *tok = strtok_r(copy, " ", &save); tok;
	     tok = strtok_r(NULL, " ", &save)) {
		idx++;
		if (idx == 3) {
			if (!parse_major_minor(tok, &mj, &mn)) goto fail;
		} else if (idx == 5) {
			mnt = strdup(tok);
			if (!mnt) goto fail;
		} else if (!seen_dash && idx >= 7
		           && tok[0] == '-' && tok[1] == '\0') {
			seen_dash = 1;
		} else if (seen_dash && !fst) {
			fst = strdup(tok);
			if (!fst) goto fail;
			break; /* fstype is the only post-dash field we need */
		}
	}
	free(copy);

	if (mj < 0 || !mnt || !fst) {
		free(mnt); free(fst);
		return 0;
	}

	*major = mj;
	*minor = mn;
	*mount_out = mnt;
	*fstype_out = fst;
	return 1;

fail:
	free(copy);
	free(mnt);
	free(fst);
	return 0;
}

/**
 * @brief Dedup by (major, minor). Keeps the first occurrence (typically the
 *        canonical mountpoint; bind mounts are skipped).
 *
 * In-place: rewrites @p arr and updates @p count. Frees freed-out entries'
 * strings. Stable order — first-seen wins.
 */
static void dedup_mounts(struct mount_entry *arr, size_t *count)
{
	size_t out = 0;
	for (size_t i = 0; i < *count; i++) {
		int seen = 0;
		for (size_t j = 0; j < out; j++) {
			if (arr[j].major == arr[i].major &&
			    arr[j].minor == arr[i].minor) {
				seen = 1;
				break;
			}
		}
		if (seen) {
			free(arr[i].mount);
			free(arr[i].fstype);
			continue;
		}
		if (out != i) arr[out] = arr[i];
		out++;
	}
	*count = out;
}

/**
 * @brief Build the canonical mount list from /proc/self/mountinfo.
 *
 * Excludes pseudo filesystems (`is_excluded_fstype`) and dedups by
 * (major,minor) so bind mounts of the same device only appear once.
 *
 * @param out_count  receives entry count.
 * @return malloc'd array (caller frees via free_mount_entries) or NULL on
 *         OOM / mountinfo unreadable. *out_count is 0 in that case.
 */
static struct mount_entry *list_real_mounts(size_t *out_count)
{
	*out_count = 0;
	char *content = read_file_all("/proc/self/mountinfo");
	if (!content) return NULL;

	size_t cap = 16, count = 0;
	struct mount_entry *arr = malloc(sizeof *arr * cap);
	if (!arr) { free(content); return NULL; }

	char *save = NULL;
	for (char *line = strtok_r(content, "\n", &save); line;
	     line = strtok_r(NULL, "\n", &save)) {
		int mj = -1, mn = -1;
		char *mnt = NULL, *fst = NULL;
		if (!parse_mountinfo_line(line, &mj, &mn, &mnt, &fst))
			continue;
		if (is_excluded_fstype(fst)) {
			free(mnt); free(fst);
			continue;
		}
		if (count + 1 > cap) {
			cap *= 2;
			struct mount_entry *nr = realloc(arr, sizeof *arr * cap);
			if (!nr) { free(mnt); free(fst); break; }
			arr = nr;
		}
		arr[count].major = mj;
		arr[count].minor = mn;
		arr[count].mount = mnt;
		arr[count].fstype = fst;
		count++;
	}
	free(content);

	dedup_mounts(arr, &count);
	*out_count = count;
	return arr;
}

/**
 * @brief Build inventory `mounts[]` (raw bytes + fstype + major/minor).
 */
static cJSON *collect_mounts_inventory(void)
{
	cJSON *arr = cJSON_CreateArray();
	if (!arr)
		return NULL;

	size_t n = 0;
	struct mount_entry *mounts = list_real_mounts(&n);
	for (size_t i = 0; i < n; i++) {
		struct statvfs st;
		if (statvfs(mounts[i].mount, &st) != 0)
			continue;
		double total = (double)st.f_blocks * (double)st.f_frsize;
		double freeb = (double)st.f_bfree  * (double)st.f_frsize;
		double avail = (double)st.f_bavail * (double)st.f_frsize;

		cJSON *item = cJSON_CreateObject();
		cJSON_AddStringToObject(item, "mount", mounts[i].mount);
		add_major_minor(item, mounts[i].major, mounts[i].minor);
		cJSON_AddNumberToObject(item, "total_bytes", total);
		cJSON_AddNumberToObject(item, "free_bytes", freeb);
		cJSON_AddNumberToObject(item, "avail_bytes", avail);
		cJSON_AddStringToObject(item, "fstype", mounts[i].fstype);
		cJSON_AddItemToArray(arr, item);
	}
	free_mount_entries(mounts, n);
	return arr;
}

/**
 * @brief Build metrics `mounts[]` (raw bytes + major/minor; no fstype).
 *
 * fstype lives in inventory only — it doesn't change per metric tick and
 * keeping it out of the 60-second message reduces wire traffic.
 */
static cJSON *collect_mounts_metrics(void)
{
	cJSON *arr = cJSON_CreateArray();
	if (!arr)
		return NULL;

	size_t n = 0;
	struct mount_entry *mounts = list_real_mounts(&n);
	for (size_t i = 0; i < n; i++) {
		struct statvfs st;
		if (statvfs(mounts[i].mount, &st) != 0)
			continue;
		double total = (double)st.f_blocks * (double)st.f_frsize;
		double freeb = (double)st.f_bfree  * (double)st.f_frsize;
		double avail = (double)st.f_bavail * (double)st.f_frsize;

		cJSON *item = cJSON_CreateObject();
		cJSON_AddStringToObject(item, "mount", mounts[i].mount);
		add_major_minor(item, mounts[i].major, mounts[i].minor);
		cJSON_AddNumberToObject(item, "total_bytes", total);
		cJSON_AddNumberToObject(item, "free_bytes", freeb);
		cJSON_AddNumberToObject(item, "avail_bytes", avail);
		cJSON_AddItemToArray(arr, item);
	}
	free_mount_entries(mounts, n);
	return arr;
}

static cJSON *collect_internal_ips(void)
{
	cJSON *arr = cJSON_CreateArray();
	struct ifaddrs *ifap = NULL;
	if (getifaddrs(&ifap) != 0)
		return arr;

	for (struct ifaddrs *ifa = ifap; ifa; ifa = ifa->ifa_next) {
		if (!ifa->ifa_addr)
			continue;
		if (ifa->ifa_flags & IFF_LOOPBACK)
			continue;
		if (ifa->ifa_addr->sa_family != AF_INET)
			continue;
		char ip[INET_ADDRSTRLEN];
		struct sockaddr_in *sin = (struct sockaddr_in *)ifa->ifa_addr;
		if (!inet_ntop(AF_INET, &sin->sin_addr, ip, sizeof ip))
			continue;
		cJSON_AddItemToArray(arr, cJSON_CreateString(ip));
	}
	freeifaddrs(ifap);
	return arr;
}

/**
 * @brief Resolve external IP via env override → cloud metadata.
 *
 * @return cJSON array (empty allowed) or null literal if both fail.
 */
static cJSON *collect_external_ip(void)
{
	const char *override = getenv("AGENT_EXTERNAL_IP");
	if (override && *override) {
		cJSON *arr = cJSON_CreateArray();
		char *copy = strdup(override);
		if (!copy)
			return arr;
		char *save = NULL;
		for (char *tok = strtok_r(copy, ",", &save); tok;
		     tok = strtok_r(NULL, ",", &save)) {
			trim_inplace(tok);
			if (*tok)
				cJSON_AddItemToArray(arr, cJSON_CreateString(tok));
		}
		free(copy);
		return arr;
	}

	const char *vendor = detect_cloud_vendor();
	if (!vendor)
		return cJSON_CreateNull();

	char *out = NULL;
	if (strcmp(vendor, "aws") == 0) {
		char *token = run_cmd(
			"curl -fsS -m 1 -X PUT "
			"-H 'X-aws-ec2-metadata-token-ttl-seconds: 60' "
			"http://169.254.169.254/latest/api/token 2>/dev/null");
		if (token && *token) {
			trim_inplace(token);
			char cmd[512];
			snprintf(cmd, sizeof cmd,
			         "curl -fsS -m 1 -H 'X-aws-ec2-metadata-token: %s' "
			         "http://169.254.169.254/latest/meta-data/public-ipv4 2>/dev/null",
			         token);
			out = run_cmd(cmd);
		}
		free(token);
	} else if (strcmp(vendor, "azure") == 0) {
		out = run_cmd(
			"curl -fsS -m 1 -H 'Metadata: true' "
			"'http://169.254.169.254/metadata/instance/network/interface/0/"
			"ipv4/ipAddress/0/publicIpAddress?api-version=2021-02-01&format=text' 2>/dev/null");
	} else if (strcmp(vendor, "gcp") == 0) {
		out = run_cmd(
			"curl -fsS -m 1 -H 'Metadata-Flavor: Google' "
			"http://metadata.google.internal/computeMetadata/v1/instance/"
			"network-interfaces/0/access-configs/0/external-ip 2>/dev/null");
	}

	if (!out)
		return cJSON_CreateNull();
	trim_inplace(out);
	if (!*out) {
		free(out);
		return cJSON_CreateArray();
	}
	cJSON *arr = cJSON_CreateArray();
	cJSON_AddItemToArray(arr, cJSON_CreateString(out));
	free(out);
	return arr;
}

static int add_boot_time(cJSON *root)
{
	char *content = read_file_all("/proc/uptime");
	if (!content)
		return 0;
	double uptime = strtod(content, NULL);
	free(content);
	if (uptime <= 0)
		return 0;

	struct timespec now;
	clock_gettime(CLOCK_REALTIME, &now);
	time_t boot = now.tv_sec - (time_t)uptime;
	char buf[32];
	iso8601_utc(boot, buf, sizeof buf);
	cJSON_AddStringToObject(root, "boot_time", buf);
	return 1;
}

cJSON *collect_inventory_payload(const char *machine_id, const char *agent_version)
{
	cJSON *root = cJSON_CreateObject();
	if (!root)
		return NULL;
	add_common_metadata(root, "inventory", machine_id, agent_version);

	int ok = 1;
	add_os_release(root);
	if (!add_kernel_version(root))     ok = 0;
	if (!add_cpu_cores(root))          ok = 0;
	add_cpu_model(root);
	if (!add_mem_total_swap_total(root)) ok = 0;

	cJSON_AddItemToObject(root, "disks",       or_empty_array(collect_disks()));
	cJSON_AddItemToObject(root, "mounts",      or_empty_array(collect_mounts_inventory()));
	cJSON_AddItemToObject(root, "ip_internal", or_empty_array(collect_internal_ips()));
	/* ip_external may be null literal by design (cloud metadata unreachable). */
	cJSON_AddItemToObject(root, "ip_external", collect_external_ip());

	if (!add_boot_time(root)) ok = 0;

	if (!ok) {
		cJSON_Delete(root);
		return NULL;
	}
	return root;
}

/* ============================================================
 * Metrics collectors
 * ============================================================ */

static int add_cpu_stat(cJSON *root)
{
	char *content = read_file_all("/proc/stat");
	if (!content)
		return 0;
	long u = 0, n = 0, s = 0, i = 0, w = 0, q = 0, sq = 0, st = 0;
	int got = sscanf(content, "cpu  %ld %ld %ld %ld %ld %ld %ld %ld",
	                 &u, &n, &s, &i, &w, &q, &sq, &st);
	free(content);
	if (got < 4)
		return 0;
	cJSON *obj = cJSON_CreateObject();
	cJSON_AddNumberToObject(obj, "user",    (double)u);
	cJSON_AddNumberToObject(obj, "nice",    (double)n);
	cJSON_AddNumberToObject(obj, "system",  (double)s);
	cJSON_AddNumberToObject(obj, "idle",    (double)i);
	cJSON_AddNumberToObject(obj, "iowait",  (double)w);
	cJSON_AddNumberToObject(obj, "irq",     (double)q);
	cJSON_AddNumberToObject(obj, "softirq", (double)sq);
	cJSON_AddNumberToObject(obj, "steal",   (double)st);
	cJSON_AddItemToObject(root, "cpu_stat", obj);
	return 1;
}

static int add_meminfo_full(cJSON *root)
{
	char *content = read_file_all("/proc/meminfo");
	if (!content)
		return 0;
	long mem_total     = meminfo_get_kb(content, "MemTotal");
	long mem_free      = meminfo_get_kb(content, "MemFree");
	long mem_available = meminfo_get_kb(content, "MemAvailable");
	long mem_buffers   = meminfo_get_kb(content, "Buffers");
	long mem_cached    = meminfo_get_kb(content, "Cached");
	long swap_total    = meminfo_get_kb(content, "SwapTotal");
	long swap_free     = meminfo_get_kb(content, "SwapFree");
	free(content);

	if (mem_total < 0)
		return 0;

	cJSON_AddNumberToObject(root, "mem_total_kb", (double)mem_total);
	add_kb_or_null(root, "mem_free_kb",    mem_free);
	add_kb_or_null(root, "mem_buffers_kb", mem_buffers);
	add_kb_or_null(root, "mem_cached_kb",  mem_cached);
	add_kb_or_null(root, "swap_total_kb",  swap_total);
	add_kb_or_null(root, "swap_free_kb",   swap_free);

	if (mem_available < 0) {
		/* CentOS 7.0~7.1 fallback: MemFree + Buffers + Cached. */
		if (mem_free >= 0 && mem_buffers >= 0 && mem_cached >= 0)
			cJSON_AddNumberToObject(root, "mem_available_kb",
			                        (double)(mem_free + mem_buffers + mem_cached));
		else
			cJSON_AddNullToObject(root, "mem_available_kb");
	} else {
		cJSON_AddNumberToObject(root, "mem_available_kb", (double)mem_available);
	}
	return 1;
}

static int add_loadavg(cJSON *root)
{
	char *content = read_file_all("/proc/loadavg");
	if (!content)
		return 0;
	double l1 = 0, l5 = 0, l15 = 0;
	int got = sscanf(content, "%lf %lf %lf", &l1, &l5, &l15);
	free(content);
	if (got != 3)
		return 0;
	cJSON_AddNumberToObject(root, "load_1m",  l1);
	cJSON_AddNumberToObject(root, "load_5m",  l5);
	cJSON_AddNumberToObject(root, "load_15m", l15);
	return 1;
}

/**
 * @brief Parse /proc/diskstats first 14 columns per device.
 *
 * Filtering policy (matches inventory.disks[]):
 *   - Excludes loop / ram / sr / fd via is_excluded_block_dev.
 *   - Keeps only top-level block devices: /sys/block/<dev> must exist
 *     (drops partitions like vda1 by leaving them out of the parent's row).
 *
 * Emits major/minor so the engine can join with inventory.disks[] /
 * mounts[] without doing string-level device-name matching.
 */
static cJSON *collect_disk_io(void)
{
	cJSON *arr = cJSON_CreateArray();
	if (!arr)
		return NULL;
	char *content = read_file_all("/proc/diskstats");
	if (!content)
		return arr;

	char *save = NULL;
	for (char *line = strtok_r(content, "\n", &save); line;
	     line = strtok_r(NULL, "\n", &save)) {
		long major = 0, minor = 0;
		char dev[64] = { 0 };
		long reads_completed = 0, reads_merged = 0;
		long sectors_read = 0, time_reading = 0;
		long writes_completed = 0, writes_merged = 0;
		long sectors_written = 0, time_writing = 0;
		long ios_in_progress = 0, time_io = 0, weighted_time = 0;

		int n = sscanf(line, "%ld %ld %63s %ld %ld %ld %ld %ld %ld %ld %ld %ld %ld %ld",
		               &major, &minor, dev,
		               &reads_completed, &reads_merged, &sectors_read, &time_reading,
		               &writes_completed, &writes_merged, &sectors_written, &time_writing,
		               &ios_in_progress, &time_io, &weighted_time);
		if (n < 7)
			continue;
		if (is_excluded_block_dev(dev))
			continue;

		/* Only keep top-level block devices: /sys/block/<dev> exists. */
		char path[256];
		snprintf(path, sizeof path, "/sys/block/%s", dev);
		if (access(path, F_OK) != 0)
			continue;

		cJSON *item = cJSON_CreateObject();
		cJSON_AddStringToObject(item, "device", dev);
		add_major_minor(item, (int)major, (int)minor);
		cJSON_AddNumberToObject(item, "reads_completed",  (double)reads_completed);
		cJSON_AddNumberToObject(item, "writes_completed", (double)writes_completed);
		cJSON_AddNumberToObject(item, "sectors_read",     (double)sectors_read);
		cJSON_AddNumberToObject(item, "sectors_written",  (double)sectors_written);
		cJSON_AddItemToArray(arr, item);
	}
	free(content);
	return arr;
}

static cJSON *collect_net_io(void)
{
	cJSON *arr = cJSON_CreateArray();
	if (!arr)
		return NULL;
	char *content = read_file_all("/proc/net/dev");
	if (!content)
		return arr;

	int line_no = 0;
	char *save = NULL;
	for (char *line = strtok_r(content, "\n", &save); line;
	     line = strtok_r(NULL, "\n", &save)) {
		line_no++;
		if (line_no <= 2)
			continue; /* header rows */

		char *colon = strchr(line, ':');
		if (!colon)
			continue;
		*colon = '\0';
		char *iface = line;
		while (*iface == ' ' || *iface == '\t')
			iface++;
		if (strcmp(iface, "lo") == 0)
			continue;

		long rx_bytes = 0, rx_packets = 0, rx_errors = 0;
		long tx_bytes = 0, tx_packets = 0, tx_errors = 0;
		/* /proc/net/dev rx: bytes packets errs drop fifo frame compressed multicast
		 *               tx: bytes packets errs drop fifo colls carrier compressed
		 * We only keep bytes/packets/errors per direction; the rest are skipped
		 * with assignment-suppression (%*d) so the matched-count reflects the
		 * fields we actually need. */
		int n = sscanf(colon + 1,
		               "%ld %ld %ld %*d %*d %*d %*d %*d "
		               "%ld %ld %ld",
		               &rx_bytes, &rx_packets, &rx_errors,
		               &tx_bytes, &tx_packets, &tx_errors);
		if (n < 6)
			continue;

		cJSON *item = cJSON_CreateObject();
		cJSON_AddStringToObject(item, "interface", iface);
		cJSON_AddNumberToObject(item, "rx_bytes",   (double)rx_bytes);
		cJSON_AddNumberToObject(item, "tx_bytes",   (double)tx_bytes);
		cJSON_AddNumberToObject(item, "rx_packets", (double)rx_packets);
		cJSON_AddNumberToObject(item, "tx_packets", (double)tx_packets);
		cJSON_AddNumberToObject(item, "rx_errors",  (double)rx_errors);
		cJSON_AddNumberToObject(item, "tx_errors",  (double)tx_errors);
		cJSON_AddItemToArray(arr, item);
	}
	free(content);
	return arr;
}

cJSON *collect_metrics_payload(const char *machine_id, const char *agent_version)
{
	cJSON *root = cJSON_CreateObject();
	if (!root)
		return NULL;
	add_common_metadata(root, "metrics", machine_id, agent_version);

	int ok = 1;
	if (!add_cpu_stat(root))     ok = 0;
	if (!add_meminfo_full(root)) ok = 0;
	if (!add_loadavg(root))      ok = 0;

	cJSON_AddItemToObject(root, "disk_io", or_empty_array(collect_disk_io()));
	cJSON_AddItemToObject(root, "mounts",  or_empty_array(collect_mounts_metrics()));
	cJSON_AddItemToObject(root, "net_io",  or_empty_array(collect_net_io()));

	if (!ok) {
		cJSON_Delete(root);
		return NULL;
	}
	return root;
}

/* ============================================================
 * Error payload
 * ============================================================ */

cJSON *build_error_payload(const char *machine_id,
                           const char *agent_version,
                           const char *error_code,
                           const char *error_message,
                           const char *failed_component,
                           int         retry_count,
                           const char *first_failed_at,
                           const char *recovered_at)
{
	cJSON *root = cJSON_CreateObject();
	if (!root)
		return NULL;
	add_common_metadata(root, "error", machine_id, agent_version);

	/* Error-specific: collected_at carries millisecond precision. */
	struct timespec ts;
	clock_gettime(CLOCK_REALTIME, &ts);
	char ms_buf[32];
	iso8601_utc_ms(ts, ms_buf, sizeof ms_buf);
	cJSON_DeleteItemFromObject(root, "collected_at");
	cJSON_AddStringToObject(root, "collected_at", ms_buf);

	cJSON_AddStringToObject(root, "error_code",       error_code       ? error_code       : "UNKNOWN");
	cJSON_AddStringToObject(root, "error_message",    error_message    ? error_message    : "");
	cJSON_AddStringToObject(root, "failed_component", failed_component ? failed_component : "collect");

	if (retry_count >= 0)
		cJSON_AddNumberToObject(root, "retry_count", (double)retry_count);
	if (first_failed_at)
		cJSON_AddStringToObject(root, "first_failed_at", first_failed_at);
	if (recovered_at)
		cJSON_AddStringToObject(root, "recovered_at", recovered_at);

	return root;
}
