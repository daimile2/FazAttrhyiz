#!/bin/sh
# Fetch a released Limine tree into third_party/limine.
set -e
cd "$(dirname "$0")/.."
mkdir -p third_party
if [ -x third_party/limine/limine ] || [ -f third_party/limine/limine-bios-cd.bin ]; then
    echo "limine already present under third_party/limine"
    exit 0
fi
VER="${LIMINE_VERSION:-v9.2.3}"
echo "cloning Limine ${VER} (binary branch preferred)..."
if command -v git >/dev/null 2>&1; then
    git clone --depth 1 -b "${VER}-binary" \
        https://github.com/limine-bootloader/limine.git third_party/limine \
        || git clone --depth 1 https://github.com/limine-bootloader/limine.git third_party/limine
else
    echo "need git to fetch limine" >&2
    exit 1
fi
echo "ok: third_party/limine"
