#!/bin/bash
# Kernel bundle producer for an explicit product DTB.
# Usage: build-y4-bundle.sh <kernel.git> <O=builddir> <bundle-out> [pgdrv-source-dir] [Y4|Y12]
# Produces: Image, the selected product DTB, System.map-6.6.89-cix-build-generic,
#           config-6.6.89-cix-build-generic,
#           modules-6.6.89-cix-build-generic.tar.zst (rooted at the release
#           directory and carrying extra/y4/pgdrv.ko compiled against this
#           exact kernel - the factory NIC flow loads it from that path),
#           kernel.release, SHA256SUMS, RELEASE.md.
# kernel.release names the UTS release exactly as the modules tarball root
# does; build.py reads it as the single version source (kernel-ota-design
# 4.4 build gate) so the bundle and the packed rootfs cannot diverge.
# The pgdrv source ships in tools.git inputs/realtek-pg-cix-20261026/source
# and inside the y4-board deb (usr/share/doc/y4-board/pgdrv-source); pass it
# as the 4th argument. Skip the step with PGSRC="" if unwanted.
set -euo pipefail
KSRC=${1:?kernel.git checkout}
ODIR=${2:?build dir}
BOUT=${3:?bundle output dir}
PGSRC=${4:-}
if [[ -e "$BOUT" || -L "$BOUT" ]]; then
    printf "Bundle output must be a new directory: %s\n" "$BOUT" >&2
    exit 2
fi
case "${5-Y4}" in
    Y4) DTB=sky1-y4.dtb ;;
    Y12) DTB=sky1-y12.dtb ;;
    *) printf "Unknown kernel bundle board: %s\n" "$5" >&2; exit 2 ;;
esac
DTB_SOURCE="$KSRC/arch/arm64/boot/dts/cix"
if ! test -f "$DTB_SOURCE/${DTB%.dtb}.dts" ||
   ! grep -Fqw "$DTB" "$DTB_SOURCE/Makefile"; then
    printf "Board DTB is not wired into this kernel tree: %s\n" "$DTB" >&2
    exit 2
fi
VER=6.6.89-cix-build-generic
TC=/Y4/repo/rootfs/dependencies/kernel-toolchain-12.3/bin
export PATH=$TC:$PATH
STAGE=$(mktemp -d /Y4/builds/modules-stage-XXXXXX)

cd "$KSRC"
# Both NAS products prioritize first-transfer latency for installed M50 cards.
test -f "$ODIR/.config"
scripts/config --file "$ODIR/.config" --enable XH2A_KEEP_AWAKE_BY_DEFAULT
make O="$ODIR" ARCH=arm64 CROSS_COMPILE=aarch64-none-linux-gnu- LOCALVERSION=-generic \
     KBUILD_BUILD_USER=y4-debian KBUILD_BUILD_HOST=y4-build KBUILD_BUILD_VERSION=1 \
     -j"$(nproc)" Image dtbs modules
make O="$ODIR" ARCH=arm64 CROSS_COMPILE=aarch64-none-linux-gnu- \
     INSTALL_MOD_PATH="$STAGE" INSTALL_MOD_STRIP=1 modules_install >/dev/null 2>&1
MDIR="$STAGE/lib/modules/$VER"
find "$MDIR" -name build -o -name source | xargs -r rm -f

if [ -n "$PGSRC" ] && [ -d "$PGSRC" ]; then
    make -C "$ODIR" ARCH=arm64 CROSS_COMPILE=aarch64-none-linux-gnu- \
         M="$PGSRC" modules
    install -Dm644 "$PGSRC/pgdrv.ko" "$MDIR/extra/y4/pgdrv.ko"
fi

(cd "$STAGE/lib/modules" && tar --zstd -cf "/Y4/builds/modules-$(basename "$BOUT").tar.zst" "$VER")
rm -rf "$STAGE"

test -s "$ODIR/arch/arm64/boot/dts/cix/$DTB"
mkdir "$BOUT"
cp "$ODIR/arch/arm64/boot/Image" "$BOUT/Image"
cp "$ODIR/arch/arm64/boot/dts/cix/$DTB" "$BOUT/$DTB"
cp "$ODIR/System.map" "$BOUT/System.map-$VER"
cp "$ODIR/.config" "$BOUT/config-$VER"
cp "/Y4/builds/modules-$(basename "$BOUT").tar.zst" "$BOUT/modules-$VER.tar.zst"
printf '%s\n' "$VER" > "$BOUT/kernel.release"
cd "$BOUT"
sha256sum Image "$DTB" "System.map-$VER" "config-$VER" "modules-$VER.tar.zst" kernel.release > SHA256SUMS
sha256sum -c SHA256SUMS
# build.py kernel_from_bundle() hashes RELEASE.md as the handoff
# document; emit it here so every bundle ships one.
SRCHEAD=$(git -C "$KSRC" rev-parse HEAD)
SRCDESC=$(git -C "$KSRC" describe --always --dirty 2>/dev/null || echo "$SRCHEAD")
cat > "$BOUT/RELEASE.md" <<RELEASE
# kernel bundle: $VER

Produced by scripts/build-y4-bundle.sh from kernel.git $SRCHEAD
(describe: $SRCDESC), O=$ODIR, toolchain $TC. Artifact digests:
SHA256SUMS. Configuration: config-$VER, byte-comparable via
olddefconfig against the reference bundle it was derived from.
RELEASE

echo "BUNDLE READY: $BOUT"
