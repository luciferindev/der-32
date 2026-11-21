#!/bin/sh
#
# Build the DER/32 live + install ISO (out/images/der32-<release>-amd64.iso).
#
# The ISO carries the same root tree as the USB installer image, release
# sets in /der32/sets included (it reuses the work tree that "make
# installer" leaves in out/obj).  It boots from BIOS via El Torito
# (bootxx_cd9660 -> /boot -> /netbsd) and from UEFI (efiboot.img with
# EFI/boot/bootx64.efi).  Console: serial if a terminal answers on com0,
# else the screen; the boot menu can force either.  The root is read-only; the
# rc.d script der32live (ordered right after rc.d/root) copies into tmpfs
# /etc, /var, /root, /usr/local and /usr/pkg, so the stock BSD rc boots the live
# system to multi-user and upkg can install packages for the session.
#
set -eu

ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT=${DER32_OUT:-$ROOT/out}
[ -f "$ROOT/src/sys/conf/der32.release" ] || sh "$ROOT/scripts/patches.sh" apply
. "$ROOT/src/sys/conf/der32.release"
TOOLS=$OUT/tools/bin
IMGOBJ=$OUT/obj/distrib/amd64/der32image
TREE=$IMGOBJ/work
W=$OUT/iso-work
ISO=$OUT/images/der32-$DER32_RELEASE-amd64.iso
VOL=DER32_$(echo "$DER32_RELEASE" | tr . _)

die() { echo "mkiso.sh: $*" >&2; exit 1; }
[ -d "$TREE/etc" ] && [ -f "$IMGOBJ/work.spec" ] ||
    die "installer work tree missing ($TREE): run 'make installer' first"
[ -d "$TREE/der32/sets" ] ||
    die "$TREE is not an installer tree (no der32/sets): run 'make installer' first"
[ -x "$TOOLS/nbmakefs" ] || die "toolchain missing: run 'make tools'"

echo "==> staging live root"
rm -rf "$W"
mkdir -p "$W"
cp -a "$TREE" "$W/root"
R=$W/root
mkdir -p "$R/usr/pkg"	# mount point for the pkgsrc tmpfs layer

cat > "$R/etc/fstab" <<'EOF'
# DER/32 live ISO
/dev/cd0a	/		cd9660	ro			0 0
ptyfs		/dev/pts	ptyfs	rw			0 0
kernfs		/kern		kernfs	rw,noauto		0 0
procfs		/proc		procfs	rw,noauto		0 0
EOF

cat > "$R/etc/rc.d/der32live" <<'EOF'
#!/bin/sh
#
# PROVIDE: der32live
# REQUIRE: root
# BEFORE:  sysctl devpubd lvm ipfilter ip6addrctl ipsec mountcritremote network npf_boot npf pf_boot
#
# DER/32 live ISO: the root filesystem is a read-only CD.  Right after
# rc.d/root (which runs "umount -a"), give the system writable tmpfs
# layers: /tmp, and in-memory copies of /etc, /var, /root, /usr/local
# and /usr/pkg.
# (MNT_UNION only merges lookups -- existing files would stay read-only --
# so each directory is copied into its own tmpfs.)

$_rc_subr_loaded . /etc/rc.subr

name="der32live"
start_cmd="der32live_start"
stop_cmd=":"

der32live_start()
{
	mount_tmpfs -m 1777 tmpfs /tmp
	for d in /etc /var /root /usr/local /usr/pkg; do
		s=/tmp/.der32live$d
		mkdir -p "$s"
		(cd "$d" && pax -rw -pe . "$s")
		mount_tmpfs tmpfs "$d"
		(cd "$s" && pax -rw -pe . "$d")
		rm -rf "$s"
	done
	rm -rf /tmp/.der32live
	echo "DER/32 live: writable tmpfs layers on /tmp /etc /var /root /usr/local /usr/pkg"
}

load_rc_config $name
run_rc_command "$1"
EOF

cat > "$R/boot.cfg" <<EOF
banner=DER/32 $DER32_RELEASE "$DER32_CODENAME" live and install
banner=================================================
banner=Log in as root (password der32). Run der32-install
banner=to install, or just look around.
timeout=5
menu=Boot DER/32:boot netbsd
menu=Boot DER/32 on the screen:consdev pc;boot netbsd
menu=Boot DER/32 on the serial port (9600 baud):consdev com0;boot netbsd
menu=Boot DER/32 single user:boot netbsd -s
menu=Drop to boot prompt:prompt
default=1
EOF

{
	cat "$IMGOBJ/work.spec"
	echo "./etc/rc.d/der32live type=file uname=root gname=wheel mode=0555"
	grep -q '^\./boot\.cfg ' "$IMGOBJ/work.spec" ||
	    echo "./boot.cfg type=file uname=root gname=wheel mode=0644"
	echo "./usr/pkg type=dir uname=root gname=wheel mode=0755"
} > "$W/iso.spec"

echo "==> El Torito boot block"
cp "$R/usr/mdec/bootxx_cd9660" "$W/bootxx.amd64"
chmod u+w "$W/bootxx.amd64"
"$TOOLS/nbinstallboot" -m amd64 -e -o console=auto,speed=9600 "$W/bootxx.amd64"

echo "==> EFI boot image"
rm -rf "$W/efiboot" "$W/efiboot.img"
mkdir -p "$W/efiboot/EFI/boot"
cp "$R/usr/mdec/bootx64.efi" "$R/usr/mdec/bootia32.efi" "$W/efiboot/EFI/boot/"
"$TOOLS/nbmakefs" -M 1m -m 1m -B le -t msdos -o F=12,c=1 "$W/efiboot.img" "$W/efiboot"

echo "==> makefs -t cd9660"
mkdir -p "$OUT/images"
(cd "$W" && "$TOOLS/nbmakefs" -t cd9660 -F iso.spec -N root/etc \
    -o "rockridge,label=$VOL,bootimage=i386;bootxx.amd64,no-emul-boot,allow-multidot,bootimage=i386;efiboot.img,platformid=efi,no-emul-boot" \
    "$ISO.tmp" root)
mv "$ISO.tmp" "$ISO"
rm -rf "$W"	# the staging copy is as big as the ISO
(cd "$OUT/images" && sha256sum "$(basename "$ISO")" > "$(basename "$ISO").sha256")
echo "==> iso: ${ISO#$ROOT/} ($(du -h "$ISO" | cut -f1))"
