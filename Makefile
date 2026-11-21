# DER/32 top-level Makefile.  The real work is done by scripts/build.sh,
# which drives NetBSD's build.sh; see docs/building.md.

SH = /bin/sh

all: release

check:
	@$(SH) scripts/build.sh check

patches:
	@$(SH) scripts/build.sh patches

tools:
	@$(SH) scripts/build.sh tools

kernel:
	@$(SH) scripts/build.sh kernel

world:
	@$(SH) scripts/build.sh world

sets:
	@$(SH) scripts/build.sh sets

repo:
	@$(SH) scripts/build.sh repo

image:
	@$(SH) scripts/build.sh image

image-pc:
	@$(SH) scripts/build.sh image-pc

install-image:
	@$(SH) scripts/install-image.sh "$(IMAGE)" "$(DEVICE)"

release:
	@$(SH) scripts/build.sh release

iso:
	@$(SH) scripts/build.sh iso

qemu:
	@$(SH) scripts/run-qemu.sh

qemu-pc:
	@QEMU_CONSOLE=pc $(SH) scripts/run-qemu.sh

upkg:
	@$(MAKE) -C upkg

test:
	@$(SH) scripts/test.sh

test-upkg:
	@$(SH) scripts/test.sh upkg

test-qemu:
	@$(SH) scripts/test.sh qemu

clean:
	rm -rf out/obj out/destdir out/release out/images out/repo-work out/repos
	@$(MAKE) -C upkg clean

distclean: clean
	rm -rf out

.PHONY: all check patches tools kernel world sets repo image release iso \
	qemu qemu-pc install-image image-pc upkg test test-upkg test-qemu clean distclean
