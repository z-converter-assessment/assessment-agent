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
	if (strstr(vendor, "Amazon") || strstr(vendor, "Xen"))
		result = "aws";
	else if (strstr(vendor, "Microsoft"))
		result = "azure";
	else if (strstr(vendor, "Google"))
		result = "gcp";

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
	cJSON_AddNumberToObject(root, "swap_total_kb",
	                        swap_total < 0 ? 0.0 : (double)swap_total);
	return 1;
}

/**
 * @brief Collect disk list via `lsblk -dn -b -o NAME,SIZE,TYPE -J`.
 *
 * Empty array on failure (not treated as fatal — mounts[] still informs).
 */
static cJSON *collect_disks(void)
{
	cJSON *arr = cJSON_CreateArray();
	if (!arr)
		return NULL;

	char *out = run_cmd("lsblk -dn -b -o NAME,SIZE,TYPE -J 2>/dev/null");
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
		cJSON *size = cJSON_GetObjectItemCaseSensitive(dev, "size");
		cJSON *type = cJSON_GetObjectItemCaseSensitive(dev, "type");
		if (!cJSON_IsString(name))
			continue;

		cJSON *item = cJSON_CreateObject();
		cJSON_AddStringToObject(item, "name", name->valuestring);

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
 * @brief List mountpoints from /proc/mounts, skip pseudo filesystems.
 *
 * @return malloc'd array of malloc'd `mount<TAB>fstype` strings, NULL-terminated.
 *         Caller frees each entry then the array.
 */
static char **list_real_mounts(void)
{
	char *content = read_file_all("/proc/mounts");
	if (!content)
		return NULL;

	size_t cap = 16, count = 0;
	char **arr = malloc(sizeof *arr * cap);
	if (!arr) {
		free(content);
		return NULL;
	}

	const char *skip_fs[] = {
		"proc", "sysfs", "cgroup", "cgroup2", "devpts", "tmpfs",
		"devtmpfs", "mqueue", "hugetlbfs", "fusectl", "debugfs",
		"tracefs", "securityfs", "pstore", "autofs", "rpc_pipefs",
		"binfmt_misc", "configfs", "bpf", "ramfs", "overlay", "squashfs",
		NULL,
	};

	char *save = NULL;
	for (char *line = strtok_r(content, "\n", &save); line;
	     line = strtok_r(NULL, "\n", &save)) {
		char *tok_save = NULL;
		char *src   = strtok_r(line, " ", &tok_save);
		char *mnt   = strtok_r(NULL, " ", &tok_save);
		char *fstype = strtok_r(NULL, " ", &tok_save);
		if (!src || !mnt || !fstype)
			continue;

		int skip = 0;
		for (int i = 0; skip_fs[i]; i++) {
			if (strcmp(fstype, skip_fs[i]) == 0) {
				skip = 1;
				break;
			}
		}
		if (skip)
			continue;

		size_t need = strlen(mnt) + 1 + strlen(fstype) + 1;
		char *entry = malloc(need);
		if (!entry)
			continue;
		snprintf(entry, need, "%s\t%s", mnt, fstype);

		if (count + 2 > cap) {
			cap *= 2;
			char **nr = realloc(arr, sizeof *arr * cap);
			if (!nr) {
				free(entry);
				break;
			}
			arr = nr;
		}
		arr[count++] = entry;
	}
	arr[count] = NULL;
	free(content);
	return arr;
}

/**
 * @brief Build inventory `mounts[]` (raw bytes + fstype).
 */
static cJSON *collect_mounts_inventory(void)
{
	cJSON *arr = cJSON_CreateArray();
	if (!arr)
		return NULL;

	char **mounts = list_real_mounts();
	if (!mounts)
		return arr;

	for (size_t i = 0; mounts[i]; i++) {
		char *tab = strchr(mounts[i], '\t');
		if (!tab) {
			free(mounts[i]);
			continue;
		}
		*tab = '\0';
		const char *mnt = mounts[i];
		const char *fstype = tab + 1;

		struct statvfs st;
		if (statvfs(mnt, &st) != 0) {
			free(mounts[i]);
			continue;
		}
		double total = (double)st.f_blocks * (double)st.f_frsize;
		double freeb = (double)st.f_bfree  * (double)st.f_frsize;
		double avail = (double)st.f_bavail * (double)st.f_frsize;

		cJSON *item = cJSON_CreateObject();
		cJSON_AddStringToObject(item, "mount", mnt);
		cJSON_AddNumberToObject(item, "total_bytes", total);
		cJSON_AddNumberToObject(item, "free_bytes", freeb);
		cJSON_AddNumberToObject(item, "avail_bytes", avail);
		cJSON_AddStringToObject(item, "fstype", fstype);
		cJSON_AddItemToArray(arr, item);
		free(mounts[i]);
	}
	free(mounts);
	return arr;
}

/**
 * @brief Build metrics `mounts[]` (raw bytes only — no fstype, no usage_pct).
 */
static cJSON *collect_mounts_metrics(void)
{
	cJSON *arr = cJSON_CreateArray();
	if (!arr)
		return NULL;

	char **mounts = list_real_mounts();
	if (!mounts)
		return arr;

	for (size_t i = 0; mounts[i]; i++) {
		char *tab = strchr(mounts[i], '\t');
		if (tab) *tab = '\0';
		const char *mnt = mounts[i];

		struct statvfs st;
		if (statvfs(mnt, &st) != 0) {
			free(mounts[i]);
			continue;
		}
		double total = (double)st.f_blocks * (double)st.f_frsize;
		double freeb = (double)st.f_bfree  * (double)st.f_frsize;
		double avail = (double)st.f_bavail * (double)st.f_frsize;

		cJSON *item = cJSON_CreateObject();
		cJSON_AddStringToObject(item, "mount", mnt);
		cJSON_AddNumberToObject(item, "total_bytes", total);
		cJSON_AddNumberToObject(item, "free_bytes", freeb);
		cJSON_AddNumberToObject(item, "avail_bytes", avail);
		cJSON_AddItemToArray(arr, item);
		free(mounts[i]);
	}
	free(mounts);
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

	cJSON_AddItemToObject(root, "disks",       collect_disks());
	cJSON_AddItemToObject(root, "mounts",      collect_mounts_inventory());
	cJSON_AddItemToObject(root, "ip_internal", collect_internal_ips());
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
	cJSON_AddNumberToObject(root, "mem_free_kb",  mem_free  < 0 ? 0.0 : (double)mem_free);
	if (mem_available < 0) {
		/* CentOS 7.0~7.1 fallback: MemFree + Buffers + Cached. */
		if (mem_free >= 0 && mem_buffers >= 0 && mem_cached >= 0) {
			cJSON_AddNumberToObject(root, "mem_available_kb",
			                        (double)(mem_free + mem_buffers + mem_cached));
		} else {
			cJSON_AddNullToObject(root, "mem_available_kb");
		}
	} else {
		cJSON_AddNumberToObject(root, "mem_available_kb", (double)mem_available);
	}
	cJSON_AddNumberToObject(root, "mem_buffers_kb", mem_buffers < 0 ? 0.0 : (double)mem_buffers);
	cJSON_AddNumberToObject(root, "mem_cached_kb",  mem_cached  < 0 ? 0.0 : (double)mem_cached);
	cJSON_AddNumberToObject(root, "swap_total_kb",  swap_total  < 0 ? 0.0 : (double)swap_total);
	cJSON_AddNumberToObject(root, "swap_free_kb",   swap_free   < 0 ? 0.0 : (double)swap_free);
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
 * Skips loop / ram devices and partitions of common parents (heuristic:
 * keep entries that match a real /sys/block/<name>).
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
		if (strncmp(dev, "loop", 4) == 0 || strncmp(dev, "ram", 3) == 0)
			continue;

		/* Only keep top-level block devices: /sys/block/<dev> exists. */
		char path[256];
		snprintf(path, sizeof path, "/sys/block/%s", dev);
		if (access(path, F_OK) != 0)
			continue;

		cJSON *item = cJSON_CreateObject();
		cJSON_AddStringToObject(item, "device", dev);
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
		/* /proc/net/dev: rx: bytes packets errs drop fifo frame compressed multicast | tx: bytes packets errs drop fifo colls carrier compressed */
		long unused;
		int n = sscanf(colon + 1,
		               "%ld %ld %ld %ld %ld %ld %ld %ld "
		               "%ld %ld %ld %ld %ld %ld %ld %ld",
		               &rx_bytes, &rx_packets, &rx_errors, &unused,
		               &unused, &unused, &unused, &unused,
		               &tx_bytes, &tx_packets, &tx_errors, &unused,
		               &unused, &unused, &unused, &unused);
		if (n < 11)
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

	cJSON_AddItemToObject(root, "disk_io", collect_disk_io());
	cJSON_AddItemToObject(root, "mounts",  collect_mounts_metrics());
	cJSON_AddItemToObject(root, "net_io",  collect_net_io());

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
