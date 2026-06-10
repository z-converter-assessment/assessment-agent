#!/bin/sh
# Assessment Agent (Linux) — uninstaller.
#
# Invoked by the self-installer subcommand:
#     sudo ./assessment-agent-linux-x86_64 uninstall
#
# Symmetric to install.sh:
#   - stop + disable service
#   - remove systemd unit + binary
#   - leave /etc/assessment-agent/ and /var/lib/agent-worker/ in place by
#     default (preserves env + worker state across reinstalls). To wipe the
#     state directories too, set PURGE=1.
#   - user/group removal is gated on PURGE=1 as well — touching system
#     accounts mid-fleet on a re-install would break ownership of the
#     preserved env files.
#
# Required commands: sh, systemctl, rm, userdel, groupdel (when PURGE=1).

set -eu

CFG_DIR=/etc/assessment-agent
STATE_DIR=/var/lib/agent-worker
BIN_TARGET=/usr/local/bin/assessment-agent
UNIT=/etc/systemd/system/assessment-agent.service

if [ "$(id -u)" -ne 0 ]; then
	echo "[uninstall] must run as root (try: sudo $0)" >&2
	exit 1
fi

if ! command -v systemctl >/dev/null 2>&1; then
	echo "[uninstall] systemctl missing — non-systemd host?" >&2
	exit 1
fi

if systemctl is-active --quiet assessment-agent.service 2>/dev/null; then
	echo "[uninstall] stopping assessment-agent.service..."
	systemctl stop assessment-agent.service || true
fi

if systemctl is-enabled --quiet assessment-agent.service 2>/dev/null; then
	systemctl disable assessment-agent.service >/dev/null 2>&1 || true
fi

if [ -f "$UNIT" ]; then
	rm -f "$UNIT"
	echo "[uninstall] removed $UNIT"
fi

systemctl daemon-reload

if [ -f "$BIN_TARGET" ]; then
	rm -f "$BIN_TARGET"
	echo "[uninstall] removed $BIN_TARGET"
fi

if [ "${PURGE:-0}" = "1" ]; then
	rm -rf "$CFG_DIR" "$STATE_DIR"
	echo "[uninstall] purged $CFG_DIR and $STATE_DIR"
	if id assessment-agent >/dev/null 2>&1; then
		userdel assessment-agent 2>/dev/null || true
	fi
	if getent group assessment-agent >/dev/null 2>&1; then
		groupdel assessment-agent 2>/dev/null || true
	fi
	echo "[uninstall] removed assessment-agent user/group"
else
	echo "[uninstall] preserved $CFG_DIR and $STATE_DIR (PURGE=1 to wipe)"
fi

echo "[uninstall] done."
