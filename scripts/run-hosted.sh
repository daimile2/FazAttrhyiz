#!/bin/sh
set -e
cd "$(dirname "$0")/.."
make capmix-hosted
bin=capmix-hosted
if [ -x "$bin" ]; then
    exec ./$bin
fi
cp "$bin" /tmp/capmix-hosted
chmod +x /tmp/capmix-hosted
exec /tmp/capmix-hosted
