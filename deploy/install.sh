#!/bin/sh
# Assessment Agent (Linux) — installer.
#
# Single entry point. Run as root from the repo root:
#
#     sudo ./deploy/install.sh
#
# Re-runnable (idempotent). Each step short-circuits when already done:
#   - user/group already created → skipped
#   - service already enabled    → re-enabled (no-op)
#   - env keys already filled    → skipped (env-setup.sh only asks for empties)
#
# Flags (set via env, not argv — POSIX sh has no getopt without external help):
#   IMAGE_PREP=1     register service but do NOT start. Use before sealing a
#                    golden VM image; then run scripts/image-prep.sh to clear
#                    /etc/machine-id before snapshot.
#   SKIP_SHA256=1    skip dist/SHA256SUMS verification (only when intentional).
#
# Server-side dependencies (must be present — sh + coreutils + systemd are
# guaranteed on every supported OS, others may not be):
#   sh, awk, sed, grep, tr, mktemp, sha256sum, install, chmod, chown,
#   useradd, groupadd, getent, id, systemctl

set -eu

# --- 0. Banner — image-clone caveat is the single biggest gotcha for
#        fleet operators, so it gets first billing.
printf '\n'
printf '=== Assessment Agent installer ===\n'
printf 'NOTE: If this server was cloned from a VM image, the agent will inherit\n'
printf '      the source machine'\''s /etc/machine-id and the engine will overwrite\n'
printf '      its records. Before cloning, run scripts/image-prep.sh on the\n'
printf '      golden image (or use cloud-init image-cleanup) to clear machine-id.\n'
printf '\n'

# --- 1. Resolve paths
SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
AGENT_ROOT=$(cd "$SCRIPT_DIR/.." && pwd)

# INSTALLER_SELF_PATH lets the self-installer subcommand (assessment-agent
# install) point DIST_BIN at /proc/self/exe so install.sh runs against the
# single-binary bundle without needing a dist/ directory on disk.
DIST_BIN="${INSTALLER_SELF_PATH:-$AGENT_ROOT/dist/assessment-agent-linux-x86_64}"
SHA_FILE="$AGENT_ROOT/dist/SHA256SUMS"
ENV_EXAMPLE="$SCRIPT_DIR/systemd/agent.env.example"
SERVICE_FILE="$SCRIPT_DIR/systemd/assessment-agent.service"

CFG_DIR=/etc/assessment-agent
STATE_DIR=/var/lib/agent-worker
ENV_FILE="$CFG_DIR/agent.env"
ENV_LOCAL="$CFG_DIR/agent.env.local"

printf '[install] repo root : %s\n' "$AGENT_ROOT"

# --- 2. Root check
if [ "$(id -u)" -ne 0 ]; then
	echo "[install] must run as root (try: sudo $0)" >&2
	exit 1
fi

# --- 3. Required commands (fail-fast with package hint)
for cmd in sha256sum useradd groupadd id systemctl install chmod chown awk grep sed tr getent mktemp; do
	if ! command -v "$cmd" >/dev/null 2>&1; then
		echo "[install] missing required command: $cmd" >&2
		echo "[install]   apt:  apt-get install coreutils util-linux passwd systemd" >&2
		echo "[install]   dnf:  dnf install coreutils util-linux shadow-utils systemd" >&2
		exit 1
	fi
done

# --- 4. OS gate (lib/detect-os.sh)
# shellcheck disable=SC1091
. "$SCRIPT_DIR/lib/detect-os.sh"
status=$(detect_os || true)
if [ "$status" != "ok" ]; then
	echo "[install] $status" >&2
	exit 1
fi
. /etc/os-release
printf '[install] OS         : %s-%s — supported\n' "$ID" "$VERSION_ID"

# --- 5. Binary present
if [ ! -f "$DIST_BIN" ]; then
	echo "[install] binary missing: $DIST_BIN" >&2
	echo "[install] run 'make release' on a manylinux2014 build host first" >&2
	exit 1
fi

# --- 6. SHA256 verify (this is the build-host integrity gate the prod box
#        trusts — GLIBC / ldd whitelist / forbidden-API checks happened on
#        the build host's `make verify`, not here, because objdump/readelf
#        may not be installed on the target server)
if [ "${SKIP_SHA256:-0}" = "1" ]; then
	echo "[install] SKIP_SHA256=1 — skipping integrity check"
elif [ -f "$SHA_FILE" ]; then
	(cd "$AGENT_ROOT/dist" && sha256sum -c SHA256SUMS) >/dev/null 2>&1 || {
		echo "[install] SHA256 mismatch — binary may be corrupt" >&2
		exit 1
	}
	echo "[install] SHA256 OK"
else
	echo "[install] WARNING: $SHA_FILE missing — proceeding without integrity check" >&2
fi

# --- 7. User/group
if ! getent group assessment-agent >/dev/null 2>&1; then
	groupadd --system assessment-agent
	echo "[install] created group assessment-agent"
fi
if ! id assessment-agent >/dev/null 2>&1; then
	useradd --system --gid assessment-agent --home "$STATE_DIR" \
	        --shell /usr/sbin/nologin assessment-agent
	echo "[install] created user assessment-agent"
fi

# --- 8. Directories
install -d -o root             -g root             -m 0755 "$CFG_DIR"
install -d -o assessment-agent -g assessment-agent -m 0700 "$STATE_DIR"

# --- 9. Stop service if running (upgrade path)
if systemctl is-active --quiet assessment-agent.service 2>/dev/null; then
	echo "[install] stopping running service for upgrade..."
	systemctl stop assessment-agent.service
	# wait briefly so the binary can be overwritten
	for _ in 1 2 3 4 5 6 7 8 9 10; do
		systemctl is-active --quiet assessment-agent.service 2>/dev/null || break
		sleep 1
	done
fi

# --- 10. Binary install
install -o root -g root -m 0755 "$DIST_BIN" /usr/local/bin/assessment-agent
echo "[install] binary     : /usr/local/bin/assessment-agent"

# --- 11. systemd unit
install -o root -g root -m 0644 "$SERVICE_FILE" /etc/systemd/system/assessment-agent.service

# --- 12. env file (seed from example on first run, then env-setup fills empties)
if [ ! -f "$ENV_FILE" ]; then
	install -o root -g assessment-agent -m 0640 "$ENV_EXAMPLE" "$ENV_FILE"
	echo "[install] seeded env : $ENV_FILE (from agent.env.example)"
fi

# --- 13. env-setup (idempotent — only prompts for empty keys + missing secrets)
sh "$SCRIPT_DIR/lib/env-setup.sh" "$ENV_EXAMPLE" "$ENV_FILE" "$ENV_LOCAL"

# --- 14. systemd reload + enable
systemctl daemon-reload

# --- 15. image-prep mode bail-out
if [ "${IMAGE_PREP:-0}" = "1" ]; then
	systemctl enable assessment-agent.service >/dev/null 2>&1
	printf '\n'
	printf '[install] IMAGE_PREP=1 — service enabled but NOT started.\n'
	printf '[install] before sealing this VM into an image, run:\n'
	printf '[install]     sudo ./scripts/image-prep.sh\n'
	printf '\n'
	exit 0
fi

# --- 16. Start
systemctl enable --now assessment-agent.service

# --- 17. Verify it actually came up
sleep 5
if systemctl is-active --quiet assessment-agent.service; then
	printf '\n'
	echo "[install] OK — assessment-agent is active"
	echo "[install] logs:       journalctl -u assessment-agent -f"
	echo "[install] stop:       systemctl stop assessment-agent"
	echo "[install] uninstall:  systemctl disable --now assessment-agent && rm /etc/systemd/system/assessment-agent.service && systemctl daemon-reload"
else
	echo "[install] WARNING: service is not active. Last 30 log lines:" >&2
	journalctl -u assessment-agent.service -n 30 --no-pager || true
	exit 1
fi
