#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
: "${DEVKITPRO:=/opt/devkitpro}"
: "${DEVKITARM:=${DEVKITPRO}/devkitARM}"
export DEVKITPRO DEVKITARM
make
echo "Built ImpossibleGame3DS.3dsx"
