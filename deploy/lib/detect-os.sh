#!/bin/sh
# detect-os.sh — sourced by install.sh.
#
# Source then call `detect_os` — it parses /etc/os-release, prints "ok" on
# stdout and returns 0 on supported targets, or prints a one-line reason and
# returns non-zero on unsupported.
#
# Supported matrix (must stay in sync with deploy/SUPPORTED_OS.md and the
# manylinux2014 ABI ceiling = glibc 2.17 = CentOS 7):
#   - Ubuntu 18.04 / 20.04 / 22.04 / 24.04
#   - Debian 10 / 11 / 12 / 13
#   - RHEL / CentOS / Rocky / AlmaLinux / Oracle Linux 7 / 8 / 9
#     (incl. CentOS Stream 8 + 9)
#   - Amazon Linux 2, 2023
#   - SUSE Linux Enterprise / openSUSE Leap 12 / 15 (any SP)
#   - Tencent OS 4.x

detect_os() {
	if [ ! -r /etc/os-release ]; then
		echo "unsupported: /etc/os-release missing — CentOS 6 / pre-systemd?"
		return 1
	fi
	# shellcheck disable=SC1091
	. /etc/os-release
	id="${ID:-unknown}"
	ver="${VERSION_ID:-unknown}"
	major="${ver%%.*}"

	case "$id" in
		ubuntu)
			case "$ver" in
				18.04|20.04|22.04|24.04) echo ok; return 0 ;;
			esac
			;;
		debian)
			case "$major" in
				10|11|12|13) echo ok; return 0 ;;
			esac
			;;
		rhel|centos|rocky|almalinux|ol|oracle)
			case "$major" in
				7|8|9) echo ok; return 0 ;;
			esac
			;;
		amzn)
			case "$ver" in
				2|2023) echo ok; return 0 ;;
			esac
			;;
		sles|opensuse-leap|opensuse)
			case "$major" in
				12|15) echo ok; return 0 ;;
			esac
			;;
		tencentos)
			case "$major" in
				4) echo ok; return 0 ;;
			esac
			;;
	esac
	echo "unsupported: $id-$ver — see deploy/SUPPORTED_OS.md"
	return 1
}
