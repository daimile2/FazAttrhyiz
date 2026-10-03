#!/bin/sh
set -e
cd "$(dirname "$0")/.."
ISO="${1:-capmix.iso}"
if [ ! -f "$ISO" ]; then
    echo "missing $ISO — run: ./scripts/fetch-limine.sh && make iso" >&2
    exit 1
fi
QEMU="${QEMU:-qemu-system-x86_64}"
exec "$QEMU" \
    -m "${QEMU_MEM:-256M}" \
    -smp "${QEMU_SMP:-2}" \
    -cdrom "$ISO" \
    -serial stdio \
    -display none \
    -no-reboot \
    ${QEMU_EXTRA:-}
