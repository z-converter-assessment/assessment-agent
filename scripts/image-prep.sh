#!/bin/sh
# image-prep.sh
#
# Run this on the GOLDEN VM IMAGE before snapshotting / sealing.
#
# Without this step, every VM cloned from the image inherits the same
# /etc/machine-id. The agent uses /etc/machine-id as `machine_id`; cloned
# VMs all publish under the same identifier and the engine overwrites
# records on every receive.
#
# This is the standard cloud-init / Packer "generalize" pattern.

set -eu

if [ "$(id -u)" -ne 0 ]; then
	echo "[image-prep] must run as root (try: sudo $0)" >&2
	exit 1
fi

printf '\n=== Assessment Agent — image preparation ===\n'
printf 'Run this once on the GOLDEN TEMPLATE before snapshotting.\n\n'

# --- 1. Stop agent if running (it should be stopped already; defensive).
if systemctl is-active --quiet assessment-agent.service 2>/dev/null; then
	echo "[image-prep] stopping assessment-agent.service..."
	systemctl stop assessment-agent.service
fi

# --- 2. Clear /etc/machine-id.
#
# systemd-machine-id-setup runs on every boot and regenerates this file if
# it is empty (zero bytes — not deleted). The dbus copy under
# /var/lib/dbus/machine-id is replaced with a symlink to /etc/machine-id so
# the two stay in sync after regeneration.
echo "[image-prep] clearing /etc/machine-id (systemd will regenerate on next boot)"
: > /etc/machine-id
if [ -e /var/lib/dbus/machine-id ] && [ ! -L /var/lib/dbus/machine-id ]; then
	rm -f /var/lib/dbus/machine-id
	ln -s /etc/machine-id /var/lib/dbus/machine-id
fi

# --- 3. Clear systemd random seed (regenerated on boot).
rm -f /var/lib/systemd/random-seed

# --- 4. Clear cloud-init instance cache so each clone fetches its own
#        metadata on first boot (otherwise instance-id, ssh keys, etc.
#        are stale).
if [ -d /var/lib/cloud ]; then
	rm -rf /var/lib/cloud/instance \
	       /var/lib/cloud/instances/* \
	       /var/lib/cloud/data/* 2>/dev/null || true
	echo "[image-prep] cleared cloud-init cache"
fi

# --- 5. We deliberately KEEP /etc/assessment-agent/agent.env.local.
#
# agent.env.local holds broker credentials, which are per-tenant, not
# per-machine. Operators who want per-machine credentials should delete
# the file manually before this script.
echo "[image-prep] keeping /etc/assessment-agent/agent.env{,.local} (per-tenant secrets)"

printf '\n[image-prep] done — VM is ready to snapshot.\n'
printf '[image-prep] reminder: assessment-agent.service should already be enabled\n'
printf '[image-prep]           (install.sh IMAGE_PREP=1 enables without starting).\n'
