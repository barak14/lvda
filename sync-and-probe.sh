#!/usr/bin/env bash
# sync-and-probe.sh — push the lvda driver to a VM, build + load it, and run a
# viability probe, all over SSH.
#
#   ./sync-and-probe.sh <ssh-alias> [mode] [--stop-dm]
#     mode = gbm      (default) software GBM/EGL renders on the render-less card
#            scanout  lvda-ctl up -> atomic modeset at the exact mode ->
#                     GetFB2 + PRIME capture -> lvda-ctl down
#            all      gbm then scanout
#     --stop-dm   (scanout/all only) free DRM master before the probe by dropping
#                 the graphical stack to text mode (isolate multi-user.target) and
#                 terminating the seat0 session, then restoring it afterward. The
#                 atomic modeset needs DRM master, which a live compositor holds.
#                 On a multi-GPU box just stopping the greeter is not enough — the
#                 session respawns and re-grabs master on the secondary card right
#                 as lvda hotplugs, so we isolate to text mode (no respawn) AND
#                 terminate the live seat0 scope. SSH survives (sshd is in
#                 multi-user.target; SSH sessions have no seat). Restore runs on
#                 exit even if the probe fails.
#
#   ./sync-and-probe.sh fedora-vm
#   ./sync-and-probe.sh cachyos-vm scanout --stop-dm
#
# The VM needs sshd reachable, key auth, and a sudo-capable login. With
# passwordless sudo it runs fully non-interactively; run it from a terminal and
# it allocates a tty so sudo can prompt instead.
#
# Exit code mirrors the probe (0 = PASS).

set -euo pipefail

VM="${1:-}"
MODE="${2:-gbm}"
STOP_DM=0
for arg in "${@:3}"; do
	case "$arg" in
	--stop-dm) STOP_DM=1 ;;
	*) echo "unknown option '$arg'" >&2; exit 2 ;;
	esac
done
if [ -z "$VM" ]; then
	echo "usage: $0 <ssh-alias> [gbm|scanout|all] [--stop-dm]" >&2
	exit 2
fi
case "$MODE" in gbm|scanout|all) ;; *) echo "bad mode '$MODE'" >&2; exit 2;; esac

SRC_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REMOTE="lvda-c"
# Non-1080p on purpose: proves the client's *exact* requested mode lands.
TEST_W=2560; TEST_H=1440; TEST_FPS=120

echo ">> [$VM] checking connectivity"
if ! ssh -o BatchMode=yes -o ConnectTimeout=8 "$VM" true 2>/dev/null; then
	echo "!! cannot ssh to '$VM' (BatchMode). Fix the alias / key first." >&2
	exit 3
fi

# Exclude prebuilt host binaries so the VM rebuilds them for its own ABI. The
# binary-only excludes are anchored to their full path (*/dir/bin) so they don't
# also drop the same-named *directory* — a bare --exclude='lvda-ctl' matches the
# tools/lvda-ctl/ dir component too and silently omits the whole tool.
echo ">> [$VM] syncing $SRC_DIR -> ~/$REMOTE (tar over ssh)"
tar -C "$SRC_DIR" \
	--exclude='.git' \
	--exclude='*.o' --exclude='*.ko' --exclude='*.mod' --exclude='*.mod.c' \
	--exclude='.*.cmd' --exclude='Module.symvers' --exclude='modules.order' \
	--exclude='.tmp_versions' \
	--exclude='*/tests/host/gbm_probe' \
	--exclude='*/tests/host/test_edid' \
	--exclude='*/tests/host/kms_scanout_probe' \
	--exclude='*/tools/lvda-ctl/lvda-ctl' \
	-czf - . \
	| ssh "$VM" "rm -rf ~/$REMOTE && mkdir -p ~/$REMOTE && tar -C ~/$REMOTE -xzf -"

TTYFLAG=""
[ -t 0 ] && TTYFLAG="-t"

echo ">> [$VM] build + load + probe (mode=$MODE stop_dm=$STOP_DM)"
# shellcheck disable=SC2029
ssh $TTYFLAG "$VM" "REMOTE=$REMOTE MODE=$MODE STOP_DM=$STOP_DM TEST_W=$TEST_W TEST_H=$TEST_H TEST_FPS=$TEST_FPS bash -s" <<'REMOTE_EOF'
set -euo pipefail
cd "$HOME/$REMOTE"
. /etc/os-release
echo "== distro: ${PRETTY_NAME:-$ID}  kernel: $(uname -r)"

apt_any() { for s in "$@"; do if sudo apt-get install -y $s; then return 0; fi; done; return 1; }

case "$ID" in
fedora|rhel|centos)
	sudo dnf install -y "kernel-devel-$(uname -r)" gcc make pkgconf-pkg-config \
		libdrm-devel mesa-libgbm-devel mesa-libEGL-devel mesa-libGLES-devel ;;
ubuntu|debian|linuxmint|pop)
	sudo apt-get update -qq
	sudo apt-get install -y "linux-headers-$(uname -r)" build-essential pkg-config \
		libdrm-dev libgbm-dev
	apt_any "libegl-dev libgles-dev" "libegl1-mesa-dev libgles2-mesa-dev" ;;
arch|cachyos|endeavouros|manjaro)
	sudo pacman -Sy --needed --noconfirm base-devel libdrm mesa pkgconf
	hk="linux-headers"; case "$(uname -r)" in *cachyos*) hk="linux-cachyos-headers";; esac
	sudo pacman -S --needed --noconfirm "$hk" || echo "!! install '$hk' manually" ;;
*) echo "!! unknown distro '$ID'"; exit 4 ;;
esac

[ -d "/lib/modules/$(uname -r)/build" ] || { echo "!! no kernel headers"; exit 5; }

echo "== build + load module"
make -C module clean >/dev/null 2>&1 || true
make -C module
sudo rmmod lvda 2>/dev/null || true
sudo make -C module modules_install >/dev/null
sudo depmod -a 2>/dev/null || true
sudo modprobe lvda || sudo insmod module/lvda.ko
sudo dmesg | grep -i 'Initialized lvda' | tail -n 1 || true

run_gbm() {
	echo "== [gbm] build + run probe"
	make -C tests/host gbm_probe
	echo "-- gbm_probe --"
	sudo tests/host/gbm_probe
}

SESSION_DOWN=0
restore_session() {
	if [ "$SESSION_DOWN" = 1 ]; then
		echo "== restore graphical session (isolate graphical.target)"
		sudo systemctl isolate graphical.target 2>/dev/null \
			|| sudo systemctl start display-manager 2>/dev/null || true
		SESSION_DOWN=0
	fi
}

# Free DRM master. Stopping the greeter alone is not enough on a multi-GPU box:
# the session respawns and re-grabs master on the secondary card exactly when
# lvda hotplugs. isolate multi-user.target drops to text mode and prevents the
# respawn; terminate-seat kills the still-live seat0 session scope.
free_drm_master() {
	echo "== free DRM master (isolate multi-user.target + terminate seat0)"
	sudo systemctl isolate multi-user.target 2>/dev/null \
		|| sudo systemctl stop display-manager 2>/dev/null || true
	sudo loginctl terminate-seat seat0 2>/dev/null || true
	SESSION_DOWN=1
	trap restore_session EXIT
	sleep 1
}

# Poll until nothing holds the card, up to ~10s. Returns 0 if free.
wait_card_free() {
	local card="$1"
	for _ in $(seq 1 50); do
		if ! sudo fuser "$card" >/dev/null 2>&1; then
			return 0
		fi
		sleep 0.2
	done
	return 1
}

run_scanout() {
	echo "== [scanout] build lvda-ctl + probe"
	make -C tools/lvda-ctl >/dev/null
	make -C tests/host kms_scanout_probe

	[ "$STOP_DM" = 1 ] && free_drm_master

	pid=/tmp/lvda.pid; cardf=/tmp/lvda.card
	rm -f "$pid" "$cardf"

	# `up` CREATEs the display, forks a daemon that holds the fd alive, writes
	# "<minor>\n<connector>" to --card-out, and returns. No backgrounding here.
	echo "-- lvda-ctl up ${TEST_W}x${TEST_H}@${TEST_FPS} --"
	sudo tools/lvda-ctl/lvda-ctl up --width "$TEST_W" --height "$TEST_H" \
		--fps "$TEST_FPS" --pidfile "$pid" --card-out "$cardf"

	minor=$(sed -n 1p "$cardf" 2>/dev/null || true)
	conn=$(sed -n 2p "$cardf" 2>/dev/null || true)
	if [ -z "$minor" ]; then
		echo "!! lvda-ctl up did not publish a card minor"
		restore_session; return 1
	fi
	card="/dev/dri/card${minor}"
	echo "lvda up: $card connector=${conn:-?}"

	if sudo fuser "$card" >/dev/null 2>&1; then
		if [ "$STOP_DM" = 1 ]; then
			echo "-- waiting for $card master to release --"
			wait_card_free "$card" || echo "!! $card still held after teardown"
		else
			echo "!! note: $card is held by another process — atomic modeset needs"
			echo "!! DRM master. Re-run with --stop-dm to free it."
		fi
	fi

	rc=0
	echo "-- kms_scanout_probe ${TEST_W}x${TEST_H} --"
	sudo tests/host/kms_scanout_probe "$card" "${TEST_W}x${TEST_H}" || rc=$?

	sudo tools/lvda-ctl/lvda-ctl down --pidfile "$pid" 2>/dev/null || true
	restore_session
	return $rc
}

case "$MODE" in
	gbm)     run_gbm ;;
	scanout) run_scanout ;;
	all)     run_gbm; run_scanout ;;
esac
REMOTE_EOF

rc=$?
echo ">> [$VM] finished (exit $rc)"
exit $rc
