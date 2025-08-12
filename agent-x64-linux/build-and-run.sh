#!/bin/sh

set -e # Exit early if any commands fail

(
  cd "$(dirname "$0")" # Ensure compile steps are run within the repository directory
  # cmake -B build -S . -DCMAKE_TOOLCHAIN_FILE=<PATH-TO-VCPKG>/scripts/buildsystems/vcpkg.cmake
  cmake -B build -S . \
    -DCMAKE_TOOLCHAIN_FILE=/home/$USER/.vcpkg/scripts/buildsystems/vcpkg.cmake \
    -DVCPKG_TARGET_TRIPLET=x64-linux \
  #
  cmake --build ./build
)

exec $(dirname $0)/build/c2-agent "$@"
