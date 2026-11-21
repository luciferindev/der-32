#!/bin/sh
#	$DER32: der32-install.sh,v 0.1 2026/11/20 $
#
# der32-install -- install DER/32 onto a disk.
#
# Runs on a booted DER/32 install medium (live ISO or USB installer).
# It partitions the chosen disk with GPT (EFI system partition, swap,
# FFSv2 root), makes it bootable from both BIOS and UEFI, extracts the
# release sets, and configures the new system: hostname, time zone,
# root password, a user, console, network, and optionally the X11 sets
# plus the XFCE desktop from NetBSD's binary packages.
#
# Interactive by default.  "der32-install -f answers" reads the same
# settings from a file of sh assignments instead (see der32-install(8)).
#
# Copyright (c) 2026 The DER/32 Project.  BSD-2-Clause, see LICENSES/.

PATH=/sbin:/usr/sbin:/bin:/usr/bin:/usr/pkg/sbin:/usr/pkg/bin
export PATH
umask 022

PROG=der32-install
TARGET=/targetroot	# falls back to /mnt on a read-only root (the ISO)
SETSDIR=${DER32_SETS:-/der32/sets}
LOG=/tmp/der32-install.log
DER32_NAME=$(sysctl -n kern.der32.name 2>/dev/null || echo DER/32)
DER32_RELEASE=$(sysctl -n kern.der32.release 2>/dev/null || echo 0.1)
DER32_CODENAME=$(sysctl -n kern.der32.codename 2>/dev/null || echo Genesis)

BASE_SETS="base etc comp man misc text kern-DER32"
X11_SETS="xbase xcomp xetc xfont xserver"

# ------------------------------------------------------------------ helpers

die()  { echo "$PROG: $*" >&2; cleanup; exit 1; }
say()  { printf '%s\n' "$*"; }
step() { printf '\n==> %s\n' "$*"; echo "==> $*" >> "$LOG"; }
run()  { echo "+ $*" >> "$LOG"; "$@" >> "$LOG" 2>&1 || die "failed: $* (see $LOG)"; }

# ask VAR "question" [default]: read an answer unless VAR is already set
# (from an answer file).  Empty input takes the default.
ask() {
	eval "_cur=\${$1-}"
	if [ -n "$_cur" ] || [ -n "$UNATTENDED" ]; then
		[ -n "$_cur" ] || eval "$1=\${3-}"
		return
	fi
	if [ -n "${3-}" ]; then
		printf '%s [%s]: ' "$2" "$3"
	else
		printf '%s: ' "$2"
	fi
	read -r _ans || die "no input"
	[ -n "$_ans" ] || _ans=${3-}
	eval "$1=\$_ans"
}

yesno() {	# yesno VAR "question" default(yes|no)
	while :; do
		ask "$1" "$2 (yes/no)" "$3"
		eval "_v=\$$1"
		case "$_v" in
		[yY]|[yY][eE][sS]) eval "$1=yes"; return ;;
		[nN]|[nN][oO])     eval "$1=no"; return ;;
		esac
		[ -n "$UNATTENDED" ] && die "$1 must be yes or no"
		eval "$1="
	done
}

# askpw VAR "who": read a password twice without echo, store its hash.
askpw() {
	eval "_cur=\${$1-}"
	[ -n "$_cur" ] && return
	[ -n "$UNATTENDED" ] && die "$1 is required in the answer file"
	while :; do
		stty -echo
		printf 'Password for %s: ' "$2"; read -r _p1; echo
		printf 'Again: '; read -r _p2; echo
		stty echo
		if [ -z "$_p1" ]; then
			say "Empty passwords are not allowed."
		elif [ "$_p1" != "$_p2" ]; then
			say "They don't match. Try again."
		elif [ ${#_p1} -lt 6 ]; then
			say "Use at least 6 characters."
		else
			break
		fi
	done
	_h=$(pwhash -- "$_p1") || die "pwhash failed"
	unset _p1 _p2
	eval "$1=\$_h"
}

cleanup() {
	stty echo 2>/dev/null
	[ -n "$MOUNTED_ESP" ] && umount "$TARGET/mnt.esp" 2>/dev/null
	if [ -n "$MOUNTED_ROOT" ]; then
		umount "$TARGET/dev/pts" 2>/dev/null
		umount "$TARGET" 2>/dev/null
	fi
	MOUNTED_ESP= MOUNTED_ROOT=
}
trap 'echo; cleanup; exit 130' INT TERM

# Size of a disk in MiB.
disk_mb() {
	disklabel "$1" 2>/dev/null |
	    awk '/^bytes\/sector:/ { b = $2 } /^total sectors:/ { s = $3 }
		 END { if (b && s) printf "%d\n", s * b / 1048576 }'
}

# The disk the running system booted from, so we never offer it.
boot_disk() {
	r=$(sysctl -n kern.root_device 2>/dev/null)
	case "$r" in
	dk*)	dkctl "$r" getwedgeinfo 2>/dev/null |
		    sed -n '1s/^dk[0-9]* at \([a-z]*[0-9]*\):.*/\1/p' ;;
	*)	echo "$r" | sed 's/[a-p]$//' ;;
	esac
}

candidate_disks() {
	bd=$(boot_disk)
	for d in $(sysctl -n hw.disknames); do
		case "$d" in
		wd[0-9]*|sd[0-9]*|ld[0-9]*) ;;
		*) continue ;;
		esac
		[ "$d" = "$bd" ] && continue
		mb=$(disk_mb "$d")
		[ -n "$mb" ] && [ "$mb" -gt 0 ] && echo "$d $mb"
	done
}

wedge_for() {	# wedge_for disk label -> dkN
	dkctl "$1" listwedges 2>/dev/null |
	    awk -v l="$2" -F'[ :]+' '$0 ~ "^dk[0-9]+: " l "," { print $1; exit }'
}

# ------------------------------------------------------------------ start

UNATTENDED=
while getopts f:s: opt; do
	case "$opt" in
	f)	[ -r "$OPTARG" ] || die "cannot read $OPTARG"
		. "$OPTARG"
		UNATTENDED=1 ;;
	s)	SETSDIR=$OPTARG ;;
	*)	echo "usage: $PROG [-f answer-file] [-s sets-dir]" >&2; exit 2 ;;
	esac
done

[ "$(id -u)" = 0 ] || die "run me as root"
: > "$LOG"

cat <<EOF

  ___
_|___|_     $DER32_NAME $DER32_RELEASE "$DER32_CODENAME" installer
 .---.
/     \\     This puts DER/32 on a disk, replacing everything on it.
|  -- '-.   Press Ctrl-C at any question to stop. Nothing is written
'-.____)    to any disk until you confirm the disk.

EOF

for s in $BASE_SETS; do
	[ -f "$SETSDIR/$s.tar.xz" ] ||
	    die "set $s.tar.xz not found in $SETSDIR. Boot the DER/32 install medium (ISO or USB installer), or use -s DIR."
done

# ------------------------------------------------------------------ questions

step "Disk"
disks=$(candidate_disks)
[ -n "$disks" ] || die "no disks found to install on"
if [ -z "$DISK" ]; then
	say "Disks found:"
	echo "$disks" | while read -r d mb; do
		printf '  %-6s %8s MiB\n' "$d" "$mb"
	done
fi
first=$(echo "$disks" | awk 'NR == 1 { print $1 }')
while :; do
	ask DISK "Install on which disk" "$first"
	DISK_MB=$(echo "$disks" | awk -v d="$DISK" '$1 == d { print $2 }')
	[ -n "$DISK_MB" ] && break
	[ -n "$UNATTENDED" ] && die "no such disk: $DISK"
	say "Not one of the disks above."
	DISK=
done
[ "$DISK_MB" -ge 4096 ] || die "$DISK is too small (${DISK_MB} MiB; at least 4096 MiB)"

step "System"
ask HOSTNAME "Host name" "der32"
while :; do
	ask TIMEZONE "Time zone (like America/Boise, Europe/Berlin, UTC)" "UTC"
	[ -f "/usr/share/zoneinfo/$TIMEZONE" ] && break
	[ -n "$UNATTENDED" ] && die "unknown time zone: $TIMEZONE"
	say "Unknown time zone. Names are under /usr/share/zoneinfo."
	TIMEZONE=
done
case "$(tty 2>/dev/null)" in
*tty0[0-9]|*dty0[0-9]) defcons=serial ;;
*) defcons=screen ;;
esac
while :; do
	ask CONSOLE "Console: screen and keyboard, or serial port (screen/serial)" "$defcons"
	case "$CONSOLE" in screen|serial) break ;; esac
	[ -n "$UNATTENDED" ] && die "CONSOLE must be screen or serial"
	CONSOLE=
done
mem_mb=$(( $(sysctl -n hw.physmem64) / 1048576 ))
defswap=$mem_mb
[ "$defswap" -gt 4096 ] && defswap=4096
[ "$DISK_MB" -lt 16384 ] && defswap=512
while :; do
	ask SWAP_MB "Swap size in MiB (0 for none)" "$defswap"
	case "$SWAP_MB" in *[!0-9]*|'') ;; *) [ "$SWAP_MB" -lt $((DISK_MB / 4)) ] && break ;; esac
	[ -n "$UNATTENDED" ] && die "bad SWAP_MB: $SWAP_MB"
	say "Give a number of MiB, smaller than a quarter of the disk."
	SWAP_MB=
done

step "Users"
askpw ROOT_PWHASH root
ask USERNAME "Create a user (leave empty to skip)" ""
if [ -n "$USERNAME" ]; then
	case "$USERNAME" in
	[a-z_]*) ;;
	*) die "user names start with a lowercase letter" ;;
	esac
	askpw USER_PWHASH "$USERNAME"
fi

step "Network and extras"
yesno DHCP "Configure the network with DHCP at boot" yes
# SSH is not offered in 0.1: loading SSH private keys hangs on current
# DER/32 builds (see docs/roadmap.md), and sshd would hang the boot.
SSHD=no
yesno XFCE "Install the XFCE desktop (X11 sets + packages from cdn.NetBSD.org, needs internet)" no
if [ "$XFCE" = yes ]; then
	for s in $X11_SETS; do
		[ -f "$SETSDIR/$s.tar.xz" ] || die "X11 set $s.tar.xz missing from $SETSDIR"
	done
fi

cat <<EOF

  Disk:        $DISK (${DISK_MB} MiB), GPT: EFI 256 MiB, swap ${SWAP_MB} MiB, root the rest
  Host name:   $HOSTNAME
  Time zone:   $TIMEZONE
  Console:     $CONSOLE
  User:        ${USERNAME:-none}
  DHCP:        $DHCP    XFCE: $XFCE
EOF
CONFIRM_WANT="erase $DISK"
if [ -z "$UNATTENDED" ] || [ "$CONFIRM" != "$CONFIRM_WANT" ]; then
	[ -n "$UNATTENDED" ] && die "answer file must set CONFIRM=\"$CONFIRM_WANT\""
	printf '\nEverything on %s will be destroyed. Type "%s" to go on: ' "$DISK" "$CONFIRM_WANT"
	read -r CONFIRM
	[ "$CONFIRM" = "$CONFIRM_WANT" ] || die "not confirmed; nothing was changed"
fi

# ------------------------------------------------------------------ disk

step "Partitioning $DISK (GPT)"
# Remove wedges on the disk and any old partition table.
for w in $(dkctl "$DISK" listwedges 2>/dev/null | sed -n 's/^\(dk[0-9]*\):.*/\1/p'); do
	dkctl "$DISK" delwedge "$w" >> "$LOG" 2>&1
done
gpt destroy "$DISK" >> "$LOG" 2>&1
dd if=/dev/zero of="/dev/r${DISK}d" bs=1m count=4 >> "$LOG" 2>&1 ||
    dd if=/dev/zero of="/dev/r${DISK}" bs=1m count=4 >> "$LOG" 2>&1
run gpt create -f "$DISK"
run gpt add -a 1m -s 256m -t efi -l der32efi "$DISK"
[ "$SWAP_MB" -gt 0 ] && run gpt add -a 1m -s "${SWAP_MB}m" -t swap -l der32swap "$DISK"
run gpt add -a 1m -t ffs -l der32root "$DISK"
run gpt biosboot -c /usr/mdec/gptmbr.bin -L der32root "$DISK"
dkctl "$DISK" makewedges >> "$LOG" 2>&1
ESPDK=$(wedge_for "$DISK" der32efi)
ROOTDK=$(wedge_for "$DISK" der32root)
[ -n "$ESPDK" ] && [ -n "$ROOTDK" ] || die "partitions did not show up as wedges"
say "  EFI: $ESPDK   root: $ROOTDK"

step "Making file systems"
run newfs -O 2 -V 0 "/dev/r$ROOTDK"
run newfs_msdos -F 32 -L DER32EFI "/dev/r$ESPDK"
if ! mkdir -p "$TARGET" 2>/dev/null; then
	TARGET=/mnt
	[ -z "$(ls -A /mnt 2>/dev/null)" ] || die "/mnt is in use; unmount it first"
fi
run mount -o async "/dev/$ROOTDK" "$TARGET"
MOUNTED_ROOT=1

# ------------------------------------------------------------------ sets

extract() {
	printf '  %-12s' "$1"
	( cd "$TARGET" && xz -dc "$SETSDIR/$1.tar.xz" | tar -xpf - ) >> "$LOG" 2>&1 ||
	    die "extracting $1 failed (see $LOG)"
	echo "done"
}
step "Installing the release sets"
for s in $BASE_SETS; do extract "$s"; done
if [ "$XFCE" = yes ]; then
	for s in $X11_SETS; do extract "$s"; done
fi
# DER/32's own pieces that come with the medium rather than a set.
[ -d /etc/upkg ] && (cd / && pax -rw -pe etc/upkg "$TARGET") >> "$LOG" 2>&1
[ -d /usr/share/upkg ] && (cd / && pax -rw -pe usr/share/upkg "$TARGET") >> "$LOG" 2>&1

step "Device nodes"
(cd "$TARGET/dev" && sh ./MAKEDEV all) >> "$LOG" 2>&1 || die "MAKEDEV failed"

# ------------------------------------------------------------------ boot

step "Boot loaders (BIOS and UEFI)"
if [ "$CONSOLE" = serial ]; then cons=com0; else cons=pc; fi
run cp -p /usr/mdec/boot "$TARGET/boot"
run installboot -v -o "console=$cons,timeout=5" "/dev/r$ROOTDK" /usr/mdec/bootxx_ffsv2
cat > "$TARGET/boot.cfg" <<EOF
banner=Welcome to $DER32_NAME $DER32_RELEASE "$DER32_CODENAME"
banner================================================
consdev=$cons
timeout=5
menu=Boot DER/32:boot netbsd
menu=Boot DER/32 single user:boot netbsd -s
menu=Drop to boot prompt:prompt
default=1
EOF
mkdir -p "$TARGET/mnt.esp"
run mount_msdos "/dev/$ESPDK" "$TARGET/mnt.esp"
MOUNTED_ESP=1
mkdir -p "$TARGET/mnt.esp/EFI/BOOT"
run cp /usr/mdec/bootx64.efi "$TARGET/mnt.esp/EFI/BOOT/BOOTX64.EFI"
[ -f /usr/mdec/bootia32.efi ] && cp /usr/mdec/bootia32.efi "$TARGET/mnt.esp/EFI/BOOT/BOOTIA32.EFI"
umount "$TARGET/mnt.esp"
MOUNTED_ESP=
rmdir "$TARGET/mnt.esp"

# ------------------------------------------------------------------ configure

step "Configuring the new system"
{
	echo "# DER/32 installed system"
	echo "NAME=der32root	/		ffs	rw,log		1 1"
	[ "$SWAP_MB" -gt 0 ] && echo "NAME=der32swap	none		swap	sw,dp		0 0"
	echo "tmpfs		/tmp		tmpfs	rw,-m=1777,-s=ram%25	0 0"
	echo "ptyfs		/dev/pts	ptyfs	rw		0 0"
	echo "kernfs		/kern		kernfs	rw,noauto	0 0"
	echo "procfs		/proc		procfs	rw,noauto	0 0"
} > "$TARGET/etc/fstab"
mkdir -p "$TARGET/kern" "$TARGET/proc"

{
	echo
	echo "# --- written by der32-install $(date -u '+%Y-%m-%d %H:%M UTC') ---"
	echo "rc_configured=YES"
	echo "hostname=$HOSTNAME"
	echo "update_motd=YES"
	echo "certctl_init=YES"
	echo "postfix=NO		# DER/32 builds without Postfix"
	[ "$DHCP" = yes ] && echo "dhcpcd=YES" && echo "dhcpcd_flags=\"-q\""
	[ "$CONSOLE" = screen ] && echo "wscons=YES"
	[ "$SWAP_MB" -gt 0 ] && echo "savecore=YES"
} >> "$TARGET/etc/rc.conf"

ln -sf "/usr/share/zoneinfo/$TIMEZONE" "$TARGET/etc/localtime"
echo "$HOSTNAME" > "$TARGET/etc/myname"

# chroot helper: the new system's own tools, with its own /dev.
inroot() { chroot "$TARGET" /usr/bin/env -i PATH=/sbin:/usr/sbin:/bin:/usr/bin:/usr/pkg/sbin:/usr/pkg/bin HOME=/root TERM="${TERM:-vt100}" "$@"; }

run inroot usermod -p "$ROOT_PWHASH" root
if [ -n "$USERNAME" ]; then
	run inroot useradd -m -G wheel -s /bin/sh "$USERNAME"
	run inroot usermod -p "$USER_PWHASH" "$USERNAME"
fi
run inroot certctl rehash

# ------------------------------------------------------------------ XFCE

if [ "$XFCE" = yes ]; then
	step "XFCE desktop (from cdn.NetBSD.org; this takes a while)"
	if ! route -n get default >/dev/null 2>&1; then
		say "  bringing up the network with dhcpcd"
		dhcpcd -w -t 30 >> "$LOG" 2>&1
	fi
	cp /etc/resolv.conf "$TARGET/etc/resolv.conf" 2>/dev/null
	mount -t ptyfs ptyfs "$TARGET/dev/pts" 2>/dev/null
	if inroot pkg_add pkgin >> "$LOG" 2>&1 &&
	   inroot pkgin -y update >> "$LOG" 2>&1 &&
	   inroot pkgin -y install xfce4 >> "$LOG" 2>&1; then
		# pkgsrc keeps rc.d scripts as examples; XFCE needs dbus.
		for s in dbus; do
			[ -f "$TARGET/usr/pkg/share/examples/rc.d/$s" ] &&
			    cp "$TARGET/usr/pkg/share/examples/rc.d/$s" "$TARGET/etc/rc.d/$s"
		done
		echo "dbus=YES" >> "$TARGET/etc/rc.conf"
		for h in "$TARGET/root" ${USERNAME:+"$TARGET/home/$USERNAME"} "$TARGET/etc/skel"; do
			[ -d "$h" ] || continue
			printf 'exec startxfce4\n' > "$h/.xinitrc"
			printf '#!/bin/sh\nexec startxfce4\n' > "$h/.xsession"
			chmod 755 "$h/.xsession"
		done
		[ -n "$USERNAME" ] && inroot chown "$USERNAME" "/home/$USERNAME/.xinitrc" "/home/$USERNAME/.xsession"
		XFCE_DONE=yes
		say "  XFCE installed. Log in and run startx."
	else
		XFCE_DONE=no
		say "  XFCE could not be installed (no network?). The X11 sets are in;"
		say "  after booting, run: pkg_add pkgin && pkgin install xfce4"
	fi
	umount "$TARGET/dev/pts" 2>/dev/null
fi

# ------------------------------------------------------------------ done

step "Finishing"
cp "$LOG" "$TARGET/var/log/der32-install.log" 2>/dev/null
sync
umount "$TARGET" || die "cannot unmount $TARGET"
MOUNTED_ROOT=

cat <<EOF

  DER/32 is installed on $DISK.

  Remove the install medium and reboot:   shutdown -r now
  It boots from BIOS and from UEFI. Log in as root${USERNAME:+ or $USERNAME}.
EOF
[ "$XFCE_DONE" = yes ] && echo "  For the desktop, log in and run:   startx"
echo
exit 0
