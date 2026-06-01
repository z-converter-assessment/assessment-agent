/**
 * @file installer.c
 * @brief Self-installer subcommands (install / uninstall / prep-image).
 *
 * Replaces deploy/install.ps1 + deploy/env-setup.ps1 + scripts/image-prep.ps1
 * with Win32 API calls so operators only need assessment-agent.exe — no
 * PowerShell scripts on the target host.
 *
 * Behaviour mirrors the prior ps1 trio one-to-one; see header for surface.
 */

#include "installer.h"
#include "service.h"
#include "util.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include <windows.h>
#include <winreg.h>
#include <winsvc.h>
#include <accctrl.h>
#include <aclapi.h>
#include <sddl.h>
#include <shlobj.h>
#include <bcrypt.h>

/* ====================================================================
 *  Fixed paths — match the prior install.ps1 layout 1:1 so an upgrade
 *  from the script-based installer finds the same files in place.
 * ==================================================================== */
#define PROG_DIR     L"C:\\Program Files\\assessment-agent"
#define DATA_DIR     L"C:\\ProgramData\\assessment-agent"
#define EXE_TARGET   PROG_DIR L"\\assessment-agent.exe"
#define ENV_FILE     DATA_DIR L"\\agent.env"
#define ENV_LOCAL    DATA_DIR L"\\agent.env.local"
#define WORKER_DIR   DATA_DIR L"\\worker"

#define SERVICE_DISPLAY L"Assessment Agent"
#define SERVICE_DESC    L"Resource assessment collector — publishes inventory/metrics/error to RabbitMQ."

/* env-setup.ps1 의 PromptKeys / SecretKeys 와 1:1 매핑. */
static const char *const PROMPT_KEYS[] = {
    "RABBITMQ_HOST",
    "WORKER_DOWNLOAD_ALLOWED_HOSTS",
    NULL,
};
static const char *const SECRET_KEYS[] = {
    "RABBITMQ_PASS",
    "RABBITMQ_WORKER_PASS",
    NULL,
};

static int str_in_list(const char *needle, const char *const *list)
{
    for (; *list; list++)
        if (strcmp(*list, needle) == 0) return 1;
    return 0;
}

/* ====================================================================
 *  Win admin / version gates
 * ==================================================================== */
static int is_elevated(void)
{
    HANDLE tok = NULL;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) return 0;
    TOKEN_ELEVATION el = {0};
    DWORD sz = 0;
    BOOL ok = GetTokenInformation(tok, TokenElevation, &el, sizeof el, &sz);
    CloseHandle(tok);
    return ok && el.TokenIsElevated;
}

/* Use RtlGetVersion — GetVersionExW lies without an app manifest entry. */
static int check_windows_version(void)
{
    typedef LONG (WINAPI *RtlGetVersion_t)(PRTL_OSVERSIONINFOEXW);
    HMODULE nt = GetModuleHandleW(L"ntdll.dll");
    if (!nt) return 0;
    RtlGetVersion_t fn = (RtlGetVersion_t)(void *)GetProcAddress(nt, "RtlGetVersion");
    if (!fn) return 0;
    RTL_OSVERSIONINFOEXW v = { .dwOSVersionInfoSize = sizeof v };
    if (fn(&v) != 0) return 0;
    if (v.dwMajorVersion < 10) {
        fprintf(stderr, "[install] unsupported Windows %lu.%lu — requires 10 / Server 2016+\n",
                (unsigned long)v.dwMajorVersion, (unsigned long)v.dwMinorVersion);
        return 0;
    }
    fprintf(stderr, "[install] OS         : Windows %lu.%lu build %lu\n",
            (unsigned long)v.dwMajorVersion, (unsigned long)v.dwMinorVersion,
            (unsigned long)v.dwBuildNumber);
    return 1;
}

/* ====================================================================
 *  Embedded RT_RCDATA — agent.env.example bytes shipped inside the exe.
 *  Returned buffer is the resource lock (do NOT free).
 * ==================================================================== */
static const char *load_env_example(size_t *out_len)
{
    HRSRC h = FindResourceW(NULL, L"AGENT_ENV_EXAMPLE", RT_RCDATA);
    if (!h) return NULL;
    HGLOBAL g = LoadResource(NULL, h);
    if (!g) return NULL;
    DWORD sz = SizeofResource(NULL, h);
    const char *p = (const char *)LockResource(g);
    if (!p) return NULL;
    *out_len = sz;
    return p;
}

/* ====================================================================
 *  Filesystem helpers
 * ==================================================================== */
static int ensure_dir(const wchar_t *path)
{
    int rc = SHCreateDirectoryExW(NULL, path, NULL);
    if (rc == ERROR_SUCCESS || rc == ERROR_ALREADY_EXISTS ||
        rc == ERROR_FILE_EXISTS) return 0;
    fprintf(stderr, "[install] SHCreateDirectoryEx failed (%d) on %ls\n", rc, path);
    return -1;
}

/* SYSTEM + Administrators full control, no inheritance — equivalent to
 * `icacls <path> /inheritance:r /grant:r SYSTEM:F Administrators:F`. */
static int apply_strict_acl(const wchar_t *path, int container)
{
    PSECURITY_DESCRIPTOR sd = NULL;
    /* SDDL: D(ACL)P(rotected)(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)
     *   P    — break inheritance (matches /inheritance:r)
     *   A;OICI;FA;;;SY  — SYSTEM full, inherited by child files+dirs
     *   A;OICI;FA;;;BA  — BUILTIN\\Administrators full
     * OICI is harmless on files; only takes effect on directories. */
    const wchar_t *sddl = container
        ? L"D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)"
        : L"D:P(A;;FA;;;SY)(A;;FA;;;BA)";
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            sddl, SDDL_REVISION_1, &sd, NULL)) {
        fprintf(stderr, "[install] ACL build failed: %lu\n",
                (unsigned long)GetLastError());
        return -1;
    }
    BOOL dacl_present = FALSE, dacl_default = FALSE;
    PACL dacl = NULL;
    if (!GetSecurityDescriptorDacl(sd, &dacl_present, &dacl, &dacl_default) ||
        !dacl_present) {
        LocalFree(sd);
        return -1;
    }
    DWORD rc = SetNamedSecurityInfoW((LPWSTR)path, SE_FILE_OBJECT,
                                     DACL_SECURITY_INFORMATION |
                                     PROTECTED_DACL_SECURITY_INFORMATION,
                                     NULL, NULL, dacl, NULL);
    LocalFree(sd);
    if (rc != ERROR_SUCCESS) {
        fprintf(stderr, "[install] SetNamedSecurityInfo failed (%lu) on %ls\n",
                (unsigned long)rc, path);
        return -1;
    }
    return 0;
}

static int copy_self_to(const wchar_t *target)
{
    wchar_t self[MAX_PATH] = {0};
    DWORD n = GetModuleFileNameW(NULL, self, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) {
        fprintf(stderr, "[install] could not resolve own executable path\n");
        return -1;
    }
    if (_wcsicmp(self, target) == 0) {
        /* Already in place (re-running from the install dir) — skip. */
        return 0;
    }
    if (!CopyFileW(self, target, FALSE)) {
        fprintf(stderr, "[install] CopyFile %ls -> %ls failed: %lu\n",
                self, target, (unsigned long)GetLastError());
        return -1;
    }
    return 0;
}

/* ====================================================================
 *  Service control helpers
 * ==================================================================== */
static int service_exists(SC_HANDLE scm)
{
    SC_HANDLE s = OpenServiceW(scm, ASSESSMENT_AGENT_SERVICE_NAME,
                               SERVICE_QUERY_STATUS);
    if (!s) return 0;
    CloseServiceHandle(s);
    return 1;
}

static int stop_service_if_running(SC_HANDLE scm)
{
    SC_HANDLE s = OpenServiceW(scm, ASSESSMENT_AGENT_SERVICE_NAME,
                               SERVICE_STOP | SERVICE_QUERY_STATUS);
    if (!s) return 0;  /* not registered → nothing to do */
    SERVICE_STATUS st = {0};
    QueryServiceStatus(s, &st);
    if (st.dwCurrentState == SERVICE_STOPPED) {
        CloseServiceHandle(s);
        return 0;
    }
    fprintf(stderr, "[install] stopping existing service for upgrade...\n");
    ControlService(s, SERVICE_CONTROL_STOP, &st);
    /* SCM stop is async; wait up to 30s for SERVICE_STOPPED so the exe
     * file isn't held open when CopyFile overwrites it. */
    for (int i = 0; i < 60; i++) {
        QueryServiceStatus(s, &st);
        if (st.dwCurrentState == SERVICE_STOPPED) break;
        Sleep(500);
    }
    CloseServiceHandle(s);
    return st.dwCurrentState == SERVICE_STOPPED ? 0 : -1;
}

static int register_service(SC_HANDLE scm)
{
    /* Binary path must be quoted — the path itself contains a space
     * ("C:\\Program Files\\..."). SCM treats lpBinaryPathName as a command
     * line, so without quotes the first space splits the argv. */
    const wchar_t *bin_path = L"\"" EXE_TARGET L"\"";

    SC_HANDLE svc = CreateServiceW(scm,
        ASSESSMENT_AGENT_SERVICE_NAME, SERVICE_DISPLAY,
        SERVICE_ALL_ACCESS,
        SERVICE_WIN32_OWN_PROCESS,
        SERVICE_AUTO_START,
        SERVICE_ERROR_NORMAL,
        bin_path,
        NULL, NULL, NULL,
        NULL,  /* LocalSystem */
        NULL);
    if (!svc) {
        DWORD err = GetLastError();
        fprintf(stderr, "[install] CreateService failed: %lu\n", (unsigned long)err);
        return -1;
    }

    SERVICE_DESCRIPTIONW desc = { .lpDescription = (LPWSTR)SERVICE_DESC };
    ChangeServiceConfig2W(svc, SERVICE_CONFIG_DESCRIPTION, &desc);

    /* Failure recovery — restart after 5s / 10s / 30s. Reset counter after
     * 1 clean day. Matches `sc.exe failure ... reset= 86400 actions= ...`
     * from the previous install.ps1. */
    SC_ACTION actions[3] = {
        { SC_ACTION_RESTART,  5000 },
        { SC_ACTION_RESTART, 10000 },
        { SC_ACTION_RESTART, 30000 },
    };
    SERVICE_FAILURE_ACTIONSW fa = {
        .dwResetPeriod = 86400,
        .lpRebootMsg   = NULL,
        .lpCommand     = NULL,
        .cActions      = 3,
        .lpsaActions   = actions,
    };
    ChangeServiceConfig2W(svc, SERVICE_CONFIG_FAILURE_ACTIONS, &fa);

    CloseServiceHandle(svc);
    return 0;
}

static int start_service_and_wait(SC_HANDLE scm)
{
    SC_HANDLE svc = OpenServiceW(scm, ASSESSMENT_AGENT_SERVICE_NAME,
                                 SERVICE_START | SERVICE_QUERY_STATUS);
    if (!svc) return -1;
    if (!StartServiceW(svc, 0, NULL)) {
        DWORD err = GetLastError();
        if (err != ERROR_SERVICE_ALREADY_RUNNING) {
            fprintf(stderr, "[install] StartService failed: %lu\n",
                    (unsigned long)err);
            CloseServiceHandle(svc);
            return -1;
        }
    }
    SERVICE_STATUS st = {0};
    for (int i = 0; i < 30; i++) {
        QueryServiceStatus(svc, &st);
        if (st.dwCurrentState == SERVICE_RUNNING) break;
        Sleep(500);
    }
    CloseServiceHandle(svc);
    return st.dwCurrentState == SERVICE_RUNNING ? 0 : -1;
}

/* ====================================================================
 *  env-setup — embedded example drives the prompt loop.
 *
 *  Key list: parsed from the embedded agent.env.example. Both bare
 *  (KEY=value) and commented-out (# KEY=value) lines feed the canonical
 *  key list; commented secrets are tracked separately via SECRET_KEYS.
 * ==================================================================== */
typedef struct kv {
    char *key;
    char *value;
    int   commented;  /* from a "# KEY=value" line in the example */
} kv_t;

typedef struct kv_arr {
    kv_t  *items;
    size_t len, cap;
} kv_arr_t;

static void kv_arr_free(kv_arr_t *a)
{
    for (size_t i = 0; i < a->len; i++) {
        free(a->items[i].key);
        free(a->items[i].value);
    }
    free(a->items);
    a->items = NULL; a->len = a->cap = 0;
}

static int kv_arr_push(kv_arr_t *a, const char *k, const char *v, int commented)
{
    if (a->len == a->cap) {
        size_t nc = a->cap ? a->cap * 2 : 16;
        kv_t *ni = realloc(a->items, nc * sizeof *ni);
        if (!ni) return -1;
        a->items = ni; a->cap = nc;
    }
    a->items[a->len].key       = _strdup(k);
    a->items[a->len].value     = _strdup(v ? v : "");
    a->items[a->len].commented = commented;
    if (!a->items[a->len].key || !a->items[a->len].value) return -1;
    a->len++;
    return 0;
}

static kv_t *kv_arr_find(kv_arr_t *a, const char *k)
{
    for (size_t i = 0; i < a->len; i++)
        if (strcmp(a->items[i].key, k) == 0) return &a->items[i];
    return NULL;
}

static int kv_arr_set(kv_arr_t *a, const char *k, const char *v)
{
    kv_t *e = kv_arr_find(a, k);
    if (e) {
        char *nv = _strdup(v ? v : "");
        if (!nv) return -1;
        free(e->value);
        e->value = nv;
        e->commented = 0;
        return 0;
    }
    return kv_arr_push(a, k, v, 0);
}

static void trim_inplace(char *s)
{
    char *p = s;
    while (*p && isspace((unsigned char)*p)) p++;
    if (p != s) memmove(s, p, strlen(p) + 1);
    size_t n = strlen(s);
    while (n > 0 && isspace((unsigned char)s[n - 1])) s[--n] = '\0';
}

static int is_key_char(int c, int first)
{
    if (first) return c == '_' || (c >= 'A' && c <= 'Z');
    return c == '_' || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
}

/* Parse a single "KEY=value" line.
 *   leading_hash != NULL → also accept a single leading `#` (commented-out
 *                          template entry). *leading_hash set to 1 if seen.
 * Returns 0 on success (key/value written to out), -1 if not a kv line. */
static int parse_kv_line(const char *line, char *key, size_t kcap,
                         char *val, size_t vcap, int *leading_hash)
{
    const char *p = line;
    while (*p && isspace((unsigned char)*p)) p++;
    int hash = 0;
    if (*p == '#') {
        if (!leading_hash) return -1;
        hash = 1;
        p++;
        while (*p && isspace((unsigned char)*p)) p++;
    }
    if (!is_key_char(*p, 1)) return -1;

    size_t kl = 0;
    while (is_key_char(*p, 0)) {
        if (kl + 1 >= kcap) return -1;
        key[kl++] = *p++;
    }
    key[kl] = '\0';

    while (*p && isspace((unsigned char)*p)) p++;
    if (*p != '=') return -1;
    p++;
    while (*p && isspace((unsigned char)*p)) p++;

    size_t vl = 0;
    while (*p && *p != '\n' && *p != '\r') {
        if (vl + 1 >= vcap) return -1;
        val[vl++] = *p++;
    }
    val[vl] = '\0';
    trim_inplace(val);
    /* Strip matching surrounding quotes. */
    size_t vlen = strlen(val);
    if (vlen >= 2 &&
        ((val[0] == '"'  && val[vlen - 1] == '"') ||
         (val[0] == '\'' && val[vlen - 1] == '\''))) {
        memmove(val, val + 1, vlen - 2);
        val[vlen - 2] = '\0';
    }
    if (leading_hash) *leading_hash = hash;
    return 0;
}

/* Parse a byte buffer or file by splitting on newlines, feeding each line
 * into a callback. Used for both the embedded example and on-disk files. */
typedef void (*line_cb_t)(const char *line, void *ctx);

static void iterate_lines(const char *buf, size_t len, line_cb_t cb, void *ctx)
{
    size_t i = 0;
    char line[4096];
    while (i < len) {
        size_t j = i;
        while (j < len && buf[j] != '\n') j++;
        size_t n = j - i;
        if (n >= sizeof line) n = sizeof line - 1;
        memcpy(line, buf + i, n);
        line[n] = '\0';
        cb(line, ctx);
        i = j + 1;
    }
}

static void example_line_cb(const char *line, void *ctx)
{
    kv_arr_t *a = (kv_arr_t *)ctx;
    char k[128], v[2048];
    int hash = 0;
    if (parse_kv_line(line, k, sizeof k, v, sizeof v, &hash) == 0) {
        if (!kv_arr_find(a, k)) kv_arr_push(a, k, v, hash);
    }
}

static void existing_line_cb(const char *line, void *ctx)
{
    kv_arr_t *a = (kv_arr_t *)ctx;
    char k[128], v[2048];
    if (parse_kv_line(line, k, sizeof k, v, sizeof v, NULL) == 0)
        kv_arr_set(a, k, v);
}

static int read_file_all(const wchar_t *path, char **out, size_t *out_len)
{
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) {
        *out = NULL; *out_len = 0;
        return -1;
    }
    LARGE_INTEGER sz; sz.QuadPart = 0;
    GetFileSizeEx(f, &sz);
    char *buf = malloc((size_t)sz.QuadPart + 1);
    if (!buf) { CloseHandle(f); return -1; }
    DWORD rd = 0;
    ReadFile(f, buf, (DWORD)sz.QuadPart, &rd, NULL);
    buf[rd] = '\0';
    CloseHandle(f);
    *out = buf; *out_len = rd;
    return 0;
}

/* Atomic write — UTF-8, no BOM (matches env-setup.ps1 behaviour and the
 * Linux env file format read by load_env_file). */
static int write_env_file(const wchar_t *path, const kv_arr_t *a, int skip_commented)
{
    wchar_t tmp[MAX_PATH];
    _snwprintf(tmp, MAX_PATH, L"%ls.tmp", path);
    HANDLE f = CreateFileW(tmp, GENERIC_WRITE, 0, NULL,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return -1;
    for (size_t i = 0; i < a->len; i++) {
        if (skip_commented && a->items[i].commented) continue;
        char line[2560];
        int n = _snprintf(line, sizeof line, "%s=%s\r\n",
                          a->items[i].key, a->items[i].value);
        if (n > 0) {
            DWORD w = 0;
            WriteFile(f, line, (DWORD)n, &w, NULL);
        }
    }
    CloseHandle(f);
    if (!MoveFileExW(tmp, path, MOVEFILE_REPLACE_EXISTING)) return -1;
    return 0;
}

/* Console input with optional echo suppression. Returns the trimmed line.
 * Wide is not needed — values are ASCII / UTF-8 bytes. */
static int read_console_line(char *buf, size_t cap, int echo)
{
    HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
    DWORD prev = 0;
    int restored = 0;
    if (!echo) {
        if (GetConsoleMode(in, &prev)) {
            SetConsoleMode(in, prev & ~ENABLE_ECHO_INPUT);
            restored = 1;
        }
    }
    int rc = -1;
    if (fgets(buf, (int)cap, stdin)) {
        trim_inplace(buf);
        rc = 0;
    }
    if (restored) {
        SetConsoleMode(in, prev);
        fputc('\n', stdout);  /* echo-off swallowed the user's Enter */
    }
    return rc;
}

static int env_setup_run(const char *example, size_t example_len)
{
    kv_arr_t schema = {0};  /* canonical key list from the example */
    iterate_lines(example, example_len, example_line_cb, &schema);

    /* --- agent.env (non-secret) --- */
    kv_arr_t env_state = {0};
    /* Seed from example so the file is complete even if every non-prompt key
     * silently takes the default (preserves Linux env_setup behaviour). */
    for (size_t i = 0; i < schema.len; i++) {
        if (schema.items[i].commented) continue;
        if (str_in_list(schema.items[i].key, SECRET_KEYS)) continue;
        kv_arr_push(&env_state, schema.items[i].key,
                    schema.items[i].value, 0);
    }
    {
        char *existing = NULL; size_t elen = 0;
        if (read_file_all(ENV_FILE, &existing, &elen) == 0 && existing) {
            iterate_lines(existing, elen, existing_line_cb, &env_state);
            free(existing);
        }
    }
    for (size_t i = 0; i < schema.len; i++) {
        const char *k = schema.items[i].key;
        if (schema.items[i].commented) continue;
        if (str_in_list(k, SECRET_KEYS)) continue;
        if (!str_in_list(k, PROMPT_KEYS)) continue;
        kv_t *cur = kv_arr_find(&env_state, k);
        if (cur && cur->value && *cur->value &&
            strcmp(cur->value, schema.items[i].value) != 0) {
            /* already customized — never overwrite */
            continue;
        }
        const char *def = schema.items[i].value;
        char line[1024];
        if (def && *def) {
            fprintf(stdout, "%s [%s]: ", k, def);
        } else {
            fprintf(stdout, "%s: ", k);
        }
        fflush(stdout);
        char input[1024] = {0};
        if (read_console_line(input, sizeof input, 1) == 0 && *input) {
            kv_arr_set(&env_state, k, input);
        } else if (def && *def) {
            kv_arr_set(&env_state, k, def);
        }
        (void)line;
    }
    if (write_env_file(ENV_FILE, &env_state, 1) != 0) {
        fprintf(stderr, "[install] writing %ls failed\n", ENV_FILE);
        kv_arr_free(&schema); kv_arr_free(&env_state);
        return -1;
    }
    fprintf(stdout, "[install] wrote %ls\n", ENV_FILE);

    /* --- agent.env.local (secret) --- */
    kv_arr_t local_state = {0};
    {
        char *existing = NULL; size_t elen = 0;
        if (read_file_all(ENV_LOCAL, &existing, &elen) == 0 && existing) {
            iterate_lines(existing, elen, existing_line_cb, &local_state);
            free(existing);
        }
    }
    for (const char *const *k = SECRET_KEYS; *k; k++) {
        kv_t *cur = kv_arr_find(&local_state, *k);
        if (cur && cur->value && *cur->value) continue;
        fprintf(stdout, "%s (input hidden): ", *k);
        fflush(stdout);
        char input[1024] = {0};
        if (read_console_line(input, sizeof input, 0) == 0 && *input)
            kv_arr_set(&local_state, *k, input);
    }
    if (write_env_file(ENV_LOCAL, &local_state, 0) != 0) {
        fprintf(stderr, "[install] writing %ls failed\n", ENV_LOCAL);
        kv_arr_free(&schema); kv_arr_free(&env_state); kv_arr_free(&local_state);
        return -1;
    }
    /* Strict ACL on the secret file — SYSTEM + Administrators only. */
    apply_strict_acl(ENV_LOCAL, 0);
    fprintf(stdout, "[install] wrote %ls (ACL: SYSTEM + Administrators)\n", ENV_LOCAL);

    kv_arr_free(&schema);
    kv_arr_free(&env_state);
    kv_arr_free(&local_state);
    return 0;
}

/* ====================================================================
 *  install
 * ==================================================================== */
int installer_run_install(int image_prep)
{
    fprintf(stdout, "\n=== Assessment Agent installer ===\n");
    fprintf(stdout, "NOTE: cloned VMs inherit MachineGuid — run `prep-image`\n"
                    "      on the golden template before snapshotting.\n\n");

    if (!is_elevated()) {
        fprintf(stderr, "[install] must run from an elevated (Administrator) shell\n");
        return 1;
    }
    if (!check_windows_version()) return 1;

    size_t ex_len = 0;
    const char *ex = load_env_example(&ex_len);
    if (!ex) {
        fprintf(stderr, "[install] embedded env example missing — corrupt binary\n");
        return 1;
    }

    SC_HANDLE scm = OpenSCManagerW(NULL, NULL,
                                   SC_MANAGER_CREATE_SERVICE |
                                   SC_MANAGER_CONNECT);
    if (!scm) {
        fprintf(stderr, "[install] OpenSCManager failed: %lu\n",
                (unsigned long)GetLastError());
        return 1;
    }

    if (stop_service_if_running(scm) != 0)
        fprintf(stderr, "[install] warning: existing service did not stop cleanly\n");

    if (ensure_dir(PROG_DIR) != 0 ||
        ensure_dir(DATA_DIR) != 0 ||
        ensure_dir(WORKER_DIR) != 0 ||
        ensure_dir(WORKER_DIR L"\\results") != 0 ||
        ensure_dir(WORKER_DIR L"\\done")    != 0 ||
        ensure_dir(WORKER_DIR L"\\running") != 0) {
        CloseServiceHandle(scm);
        return 1;
    }

    if (copy_self_to(EXE_TARGET) != 0) {
        CloseServiceHandle(scm);
        return 1;
    }
    fprintf(stdout, "[install] binary     : %ls\n", EXE_TARGET);

    if (env_setup_run(ex, ex_len) != 0) {
        CloseServiceHandle(scm);
        return 1;
    }

    /* Tighten DATA_DIR ACL last so the env-setup writes (which inherit the
     * default ACL on first creation) get re-secured. */
    apply_strict_acl(DATA_DIR, 1);

    if (!service_exists(scm)) {
        fprintf(stdout, "[install] registering service '%ls'...\n",
                ASSESSMENT_AGENT_SERVICE_NAME);
        if (register_service(scm) != 0) {
            CloseServiceHandle(scm);
            return 1;
        }
    }

    if (image_prep) {
        fprintf(stdout, "\n[install] --image-prep — service registered, NOT started.\n");
        fprintf(stdout, "[install] before sealing the VM image, run:\n");
        fprintf(stdout, "[install]     assessment-agent.exe prep-image\n\n");
        CloseServiceHandle(scm);
        return 0;
    }

    fprintf(stdout, "[install] starting service...\n");
    if (start_service_and_wait(scm) != 0) {
        fprintf(stderr, "[install] service failed to reach RUNNING — check "
                        "Application event log (Source: assessment-agent)\n");
        CloseServiceHandle(scm);
        return 1;
    }
    CloseServiceHandle(scm);

    fprintf(stdout, "\n[install] OK — assessment-agent is running.\n");
    fprintf(stdout, "[install] logs:      Get-WinEvent -LogName Application -MaxEvents 50 |\n"
                    "[install]              ?{ $_.ProviderName -match 'assessment-agent' }\n");
    fprintf(stdout, "[install] stop:      Stop-Service assessment-agent\n");
    fprintf(stdout, "[install] uninstall: assessment-agent.exe uninstall\n");
    return 0;
}

/* ====================================================================
 *  uninstall — stop + delete service; leave on-disk state alone.
 * ==================================================================== */
int installer_run_uninstall(void)
{
    if (!is_elevated()) {
        fprintf(stderr, "[uninstall] must run from an elevated shell\n");
        return 1;
    }
    SC_HANDLE scm = OpenSCManagerW(NULL, NULL, SC_MANAGER_CONNECT);
    if (!scm) return 1;
    SC_HANDLE svc = OpenServiceW(scm, ASSESSMENT_AGENT_SERVICE_NAME,
                                 SERVICE_STOP | SERVICE_QUERY_STATUS | DELETE);
    if (!svc) {
        fprintf(stdout, "[uninstall] service not registered — nothing to do\n");
        CloseServiceHandle(scm);
        return 0;
    }
    SERVICE_STATUS st = {0};
    QueryServiceStatus(svc, &st);
    if (st.dwCurrentState != SERVICE_STOPPED) {
        fprintf(stdout, "[uninstall] stopping service...\n");
        ControlService(svc, SERVICE_CONTROL_STOP, &st);
        for (int i = 0; i < 60; i++) {
            QueryServiceStatus(svc, &st);
            if (st.dwCurrentState == SERVICE_STOPPED) break;
            Sleep(500);
        }
    }
    if (!DeleteService(svc)) {
        fprintf(stderr, "[uninstall] DeleteService failed: %lu\n",
                (unsigned long)GetLastError());
        CloseServiceHandle(svc); CloseServiceHandle(scm);
        return 1;
    }
    CloseServiceHandle(svc);
    CloseServiceHandle(scm);
    fprintf(stdout, "[uninstall] service removed. on-disk state preserved at:\n");
    fprintf(stdout, "[uninstall]   %ls\n", DATA_DIR);
    fprintf(stdout, "[uninstall]   %ls\n", PROG_DIR);
    return 0;
}

/* ====================================================================
 *  prep-image — regenerate HKLM MachineGuid (+ optional sysprep).
 * ==================================================================== */
static int regenerate_machine_guid(void)
{
    /* Build a UUID v4 string via BCryptGenRandom (same path util.h::uuid_v4
     * uses for message_id). 36-char canonical form, lowercased and wrapped
     * in braces to match the format Windows stores under MachineGuid. */
    unsigned char r[16];
    NTSTATUS s = BCryptGenRandom(NULL, r, sizeof r,
                                 BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    if (s != 0) return -1;
    r[6] = (r[6] & 0x0f) | 0x40;
    r[8] = (r[8] & 0x3f) | 0x80;

    wchar_t guid[40];
    _snwprintf(guid, 40,
               L"%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
               r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[7],
               r[8], r[9], r[10], r[11], r[12], r[13], r[14], r[15]);

    HKEY k;
    LSTATUS rc = RegOpenKeyExW(HKEY_LOCAL_MACHINE,
                               L"SOFTWARE\\Microsoft\\Cryptography",
                               0, KEY_SET_VALUE | KEY_QUERY_VALUE, &k);
    if (rc != ERROR_SUCCESS) {
        fprintf(stderr, "[prep-image] RegOpenKey failed: %ld\n", rc);
        return -1;
    }
    /* Old value, for the operator's audit trail. */
    wchar_t prev[64] = {0};
    DWORD plen = sizeof prev;
    DWORD type = REG_SZ;
    RegQueryValueExW(k, L"MachineGuid", NULL, &type, (LPBYTE)prev, &plen);

    DWORD vlen = (DWORD)((wcslen(guid) + 1) * sizeof(wchar_t));
    rc = RegSetValueExW(k, L"MachineGuid", 0, REG_SZ, (const BYTE *)guid, vlen);
    RegCloseKey(k);
    if (rc != ERROR_SUCCESS) {
        fprintf(stderr, "[prep-image] RegSetValueEx failed: %ld\n", rc);
        return -1;
    }
    fprintf(stdout, "[prep-image] MachineGuid\n");
    if (*prev) fprintf(stdout, "[prep-image]   was : %ls\n", prev);
    fprintf(stdout, "[prep-image]   now : %ls\n", guid);
    return 0;
}

int installer_run_prep_image(int run_sysprep)
{
    if (!is_elevated()) {
        fprintf(stderr, "[prep-image] must run from an elevated shell\n");
        return 1;
    }
    fprintf(stdout, "\n=== Assessment Agent — image preparation ===\n");
    fprintf(stdout, "Run once on the GOLDEN TEMPLATE before snapshotting.\n\n");

    SC_HANDLE scm = OpenSCManagerW(NULL, NULL, SC_MANAGER_CONNECT);
    if (scm) {
        stop_service_if_running(scm);
        CloseServiceHandle(scm);
    }
    if (regenerate_machine_guid() != 0) return 1;

    if (run_sysprep) {
        const wchar_t sys[] = L"C:\\Windows\\System32\\Sysprep\\Sysprep.exe";
        fprintf(stdout, "[prep-image] launching sysprep /generalize /oobe /shutdown...\n");
        HINSTANCE rc = ShellExecuteW(NULL, L"open", sys,
                                     L"/generalize /oobe /shutdown",
                                     NULL, SW_SHOWNORMAL);
        if ((INT_PTR)rc <= 32) {
            fprintf(stderr, "[prep-image] ShellExecute failed: %lld\n", (long long)(INT_PTR)rc);
            return 1;
        }
    } else {
        fprintf(stdout, "\n[prep-image] recommended next step (full generalization):\n");
        fprintf(stdout, "    C:\\Windows\\System32\\Sysprep\\Sysprep.exe /generalize /oobe /shutdown\n");
        fprintf(stdout, "  or rerun with --sysprep\n\n");
    }
    fprintf(stdout, "[prep-image] done.\n");
    return 0;
}
