#!/usr/bin/env bash
# Run a command inside the devkitPro docker container with the project mounted.
# Usage:
#   tools/dkp.sh make
#   tools/dkp.sh make clean
#   tools/dkp.sh bash
#   tools/dkp.sh ./build-libssh2.sh

set -euo pipefail

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
IMAGE="devkitpro/devkitarm:latest"

if [ $# -eq 0 ]; then
  echo "usage: $0 <command...>"
  exit 2
fi

# -t (tty) only when stdin is a tty, otherwise CI/non-interactive use breaks.
TTY_FLAG=()
[ -t 0 ] && TTY_FLAG=(-t)

# Use sudo for docker only if user not in docker group.
DOCKER_CMD="docker"
if ! docker info >/dev/null 2>&1; then
  DOCKER_CMD="sudo docker"
fi

exec $DOCKER_CMD run --rm -i "${TTY_FLAG[@]}" \
  -v "$PROJECT_ROOT":/workspace \
  -w /workspace \
  --network host \
  -u "$(id -u):$(id -g)" \
  "$IMAGE" \
  bash -c "$*"
