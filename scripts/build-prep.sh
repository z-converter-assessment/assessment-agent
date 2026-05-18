#!/bin/sh
# build-prep.sh — install vendor-build prerequisites on the native Linux host.
#
# Auto-detects apt-get (Debian / Ubuntu) or yum / dnf (RHEL / CentOS / Rocky /
# AlmaLinux / Oracle Linux / Amazon Linux) and installs the system packages
# needed by `make vendor-build` (mostly: compiler toolchain, cmake, git, and
# Perl + IPC::Cmd which OpenSSL 3.0+ Configure requires).
#
# Two ways the project consumes this:
#   - Native amd64 build host: operator runs `sudo bash scripts/build-prep.sh`
#     once, then `make vendor-fetch && make vendor-build && make USE_VENDORED=1 release`
#   - Containerized build: `scripts/build-linux.sh` calls this from inside
#     the manylinux2014 container automatically (yum branch).

set -eu

if [ "$(id -u)" -ne 0 ]; then
	echo "[build-prep] must run as root (try: sudo $0)" >&2
	exit 1
fi

# Package lists — kept tight (only what vendor-build actually needs)
APT_PKGS='build-essential cmake git pkg-config perl libipc-cmd-perl ca-certificates'
RPM_PKGS='gcc make cmake git pkgconfig perl perl-IPC-Cmd perl-Data-Dumper perl-Test-Simple perl-Pod-Html perl-Module-Load-Conditional ca-certificates'

if command -v apt-get >/dev/null 2>&1; then
	echo "[build-prep] detected apt — installing: $APT_PKGS"
	export DEBIAN_FRONTEND=noninteractive
	apt-get update -qq
	# shellcheck disable=SC2086
	apt-get install -y -qq $APT_PKGS
elif command -v dnf >/dev/null 2>&1; then
	echo "[build-prep] detected dnf — installing: $RPM_PKGS"
	# shellcheck disable=SC2086
	dnf install -y -q $RPM_PKGS
elif command -v yum >/dev/null 2>&1; then
	echo "[build-prep] detected yum — installing: $RPM_PKGS"
	# shellcheck disable=SC2086
	yum install -y -q $RPM_PKGS
else
	echo "[build-prep] no supported package manager found (apt-get / yum / dnf)" >&2
	exit 1
fi

echo "[build-prep] OK — vendor-build prerequisites installed"
