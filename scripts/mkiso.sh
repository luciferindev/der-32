#!/bin/sh
#
# Build the DER/32 live ISO (out/images/der32-<release>-amd64.iso).
#
# The ISO carries the same root tree as the disk image (it reuses the work
# tree that "make image" leaves in out/obj), booted from cd9660 via El
# Torito (bootxx_cd9660 -> /boot -> /netbsd).  The root is read-only; a
# small /etc/rc front end union-mounts tmpfs over /etc, /var, /root and
# /usr/local, then runs the normal BSD rc, so the live system boots to
# multi-user and upkg can install packages for the session.
#
set -eu

ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT=${DER32_OUT:-$ROOT/out}
. "$ROOT/src/sys/conf/der32.release"
TOOLS=$OUT/tools/bin
IMGOBJ=$OUT/obj/distrib/amd64/der32image
TREE=$IMGOBJ/work
W=$OUT/iso-work
ISO=$OUT/images/der32-$DER32_RELEASE-amd64.iso
VOL=DER32_$(echo "$DER32_RELEASE" | tr . _)

die() { echo "mkiso.sh: $*" >&2; exit 1; }
[ -d "$TREE/etc" ] && [ -f "$IMGOBJ/work.spec" ] ||
    die "image work tree missing ($TREE): run 'make image' first"
[ -x "$TOOLS/nbmakefs" ] || die "toolchain missing: run 'make tools'"

echo "==> staging live root"
rm -rf "$W"
mkdir -p "$W"
cp -a "$TREE" "$W/root"
R=$W/root

cat > "$R/etc/fstab" <<'EOF'
# DER/32 live ISO
/dev/cd0a	/		cd9660	ro			0 0
ptyfs		/dev/pts	ptyfs	rw			0 0
kernfs		/kern		kernfs	rw,noauto		0 0
procfs		/proc		procfs	rw,noauto		0 0
EOF

mv "$R/etc/rc" "$R/etc/rc.multiuser"
cat > "$R/etc/rc" <<'EOF'
#!/bin/sh
#
# DER/32 live ISO front end for /etc/rc.  The root filesystem is a
# read-only CD: give the system writable tmpfs layers, then continue with
# the normal BSD rc (now /etc/rc.multiuser).
#
PATH=/sbin:/bin:/usr/sbin:/usr/bin
export PATH
mount -t tmpfs tmpfs /tmp
for d in /etc /var /root /usr/local; do
	mount -t tmpfs -o union tmpfs "$d"
done
exec /bin/sh /etc/rc.multiuser "$@"
EOF

cat > "$R/boot.cfg" <<EOF
banner=DER/32 $DER32_RELEASE $DER32_CODENAME (live ISO)
banner=================================
consdev=com0
timeout=3
menu=Boot DER/32:boot netbsd
menu=Boot DER/32 single user:boot netbsd -s
EOF

{
	cat "$IMGOBJ/work.spec"
	echo "./etc/rc.multiuser type=file uname=root gname=wheel mode=0644"
	echo "./boot.cfg type=file uname=root gname=wheel mode=0644"
} > "$W/iso.spec"

echo "==> El Torito boot block"
cp "$R/usr/mdec/bootxx_cd9660" "$W/bootxx.amd64"
chmod u+w "$W/bootxx.amd64"
"$TOOLS/nbinstallboot" -m amd64 -e -o console=com0,speed=9600 "$W/bootxx.amd64"

echo "==> makefs -t cd9660"
mkdir -p "$OUT/images"
(cd "$W" && "$TOOLS/nbmakefs" -t cd9660 -F iso.spec -N root/etc \
    -o "rockridge,label=$VOL,bootimage=i386;bootxx.amd64,no-emul-boot,allow-multidot" \
    "$ISO.tmp" root)
mv "$ISO.tmp" "$ISO"
(cd "$OUT/images" && sha256sum "$(basename "$ISO")" > "$(basename "$ISO").sha256")
echo "==> iso: ${ISO#$ROOT/} ($(du -h "$ISO" | cut -f1))"
