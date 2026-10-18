#!/bin/sh
# Product journey only: no kernel changes, no ISO required after prepare.
set -eu
repo=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
usage() {
    echo 'usage: scripts/demo-user-journey.sh prepare|qemu|linux-on|linux-off|status|check-qemu|check-vbox LAB' >&2
    echo 'LAB is a fresh demonstration directory; prepare creates only LAB/system.img.' >&2
    exit 2
}
[ "$#" = 2 ] || usage
action=$1
mkdir -p "$2"
lab=$(CDPATH= cd -- "$2" && pwd)
cd "$repo"
case "$action" in
 prepare)
    [ ! -e "$lab/system.img" ] && [ ! -e "$lab/.capmix-user-journey" ] || {
        echo 'refused: choose a fresh lab (existing image/marker retained)' >&2; exit 1;
    }
    make kernel LINUX_ABI=1 build/linux-package
    python3 scripts/install-disk.py "$lab/system.img"
    printf 'CapMix disposable product journey v1\n' > "$lab/.capmix-user-journey"
    echo "prepared: $lab/system.img; Linux OFF; boot without ISO"
    ;;
 qemu|linux-on|linux-off|status|check-qemu|check-vbox)
    [ -f "$lab/.capmix-user-journey" ] && [ -f "$lab/system.img" ] || {
        echo 'prepare a fresh lab first' >&2; exit 1;
    }
    case "$action" in
     qemu)
        # Type in the GTK virtual screen (PS/2), not the serial terminal.
        exec qemu-system-x86_64 -enable-kvm -m 512 -cpu host -smp 4 \
          -drive "id=root,file=$lab/system.img,if=none,format=raw" \
          -device ich9-ahci,id=ahci -device ide-hd,drive=root,bus=ahci.0 \
          -netdev "user,id=n0,hostfwd=tcp:127.0.0.1:${CAPMIX_HTTP_PORT:-18080}-:8080" \
          -device e1000,netdev=n0 -serial stdio -display "${CAPMIX_DISPLAY:-gtk}" -no-reboot
        ;;
     linux-on) exec python3 scripts/linux_package.py enable "$lab/system.img" ;;
     linux-off) exec python3 scripts/linux_package.py disable "$lab/system.img" ;;
     status) exec python3 scripts/linux_package.py status "$lab/system.img" ;;
     check-qemu|check-vbox)
        exec python3 tests/check_user_journey.py --backend "${action#check-}" --lab "$lab" --port "${CAPMIX_HTTP_PORT:-18080}"
        ;;
    esac
    ;;
 *) usage ;;
esac
