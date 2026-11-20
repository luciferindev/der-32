#!/bin/sh
# DER/32 cautious raw-image installer for Linux hosts.
# Copies a prepared image to an entire, unmounted disk; never partitions
# or modifies the host until the operator confirms the exact device path.
set -eu

usage() {
    echo "usage: $0 IMAGE /dev/DISK" >&2
    echo "WARNING: every existing partition on /dev/DISK will be destroyed." >&2
    exit 2
}
[ "$#" -eq 2 ] || usage
img=$1
dev=$2

die() { echo "install-image: $*" >&2; exit 1; }
for command in readlink lsblk blockdev stat dd od sha256sum; do
    command -v "$command" >/dev/null 2>&1 || die "missing tool: $command"
done
[ "$(uname -s)" = Linux ] || die "this installer currently supports Linux hosts only"
[ -f "$img" ] || die "image not found: $img"
img=$(readlink -f "$img") || die "cannot resolve image"
dev=$(readlink -f "$dev") || die "cannot resolve target disk"
case $dev in /dev/*) ;; *) die "target must be a /dev/ block disk" ;; esac
[ -b "$dev" ] || die "target is not a block device: $dev"
[ "$(lsblk -dn -o TYPE "$dev" | tr -d ' ')" = disk ] ||
    die "target must be a whole disk, not a partition or loop device"

# All descendants (disk/partitions) must be completely unmounted.
if lsblk -nr -o MOUNTPOINT "$dev" | grep -q '[^[:space:]]'; then
    die "target or one of its partitions is mounted; unmount everything first"
fi
# Reject disks backing active swap even when not mounted.
if lsblk -nrpo NAME "$dev" | while IFS= read -r name; do
    [ -z "$name" ] && continue
    if awk -v path="$name" 'NR > 1 && $1 == path { found=1 } END { exit !found }' /proc/swaps; then
        exit 0
    fi
    false
done; then
    die "target or one of its partitions is active swap"
fi

[ "$(od -An -tx1 -j510 -N2 "$img" | tr -d '[:space:]')" = 55aa ] ||
    die "image does not have an MBR boot signature"
dir=$(dirname "$img")
base=$(basename "$img")
if [ -f "$img.sha256" ]; then
    (cd "$dir" && sha256sum -c "$base.sha256") ||
        die "image checksum verification failed"
else
    echo "WARNING: no image checksum sidecar: $img.sha256" >&2
fi
bytes=$(stat -c %s "$img")
capacity=$(blockdev --getsize64 "$dev")
[ "$bytes" -le "$capacity" ] || die "image ($bytes bytes) exceeds disk ($capacity bytes)"

echo
echo "DER/32 raw disk installation"
echo "  Image:  $img ($bytes bytes)"
echo "  Target: $dev ($capacity bytes)"
lsblk -dn -o NAME,SIZE,MODEL,TRAN "$dev"
echo
echo "ALL PARTITIONS AND DATA ON $dev WILL BE LOST."
printf 'To proceed, type exactly: ERASE %s\n> ' "$dev"
IFS= read -r confirm || die "cancelled"
[ "$confirm" = "ERASE $dev" ] || die "confirmation did not match; cancelled"

[ "$(id -u)" -eq 0 ] || die "re-run as root after reviewing target"
# Re-check mounts immediately before the destructive operation.
if lsblk -nr -o MOUNTPOINT "$dev" | grep -q '[^[:space:]]'; then
    die "target became mounted; cancelled"
fi
echo "Writing image..."
dd if="$img" of="$dev" bs=4M conv=fsync status=progress
sync
echo "Image written to $dev. Remove the media safely before booting."
echo "DER/32 Genesis uses BIOS/legacy boot and a DEVELOPMENT root password."
echo "Change the root password using passwd immediately after first login."
