#!/bin/sh -ex

if [ -z "$CIRRUS_CI" ]; then
   cd rpcs3 || exit 1
fi

apt -y update && \
apt -y install clang cmake ninja-build llvm libasound2-dev libpulse-dev libopenal-dev libglew-dev zlib1g-dev libedit-dev libvulkan-dev libudev-dev git libevdev-dev libsdl2-dev libjack-dev libsndio-dev libcurl4-openssl-dev
apt -y install qt6-base-dev qt6-declarative-dev qt6-multimedia-dev qt6-svg-dev

git config --global --add safe.directory '*'

# Pull all the submodules except llvm
# shellcheck disable=SC2046
git submodule -q update --init $(awk '/path/ && !/llvm/ && !/SPIRV/ { print $3 }' .gitmodules)

mkdir build && cd build || exit 1

export CC=clang
export CXX=clang++

cmake ..                                               \
    -DCMAKE_INSTALL_PREFIX=/usr                        \
    -DUSE_NATIVE_INSTRUCTIONS=OFF                      \
    -DUSE_PRECOMPILED_HEADERS=OFF                      \
    -DCMAKE_C_FLAGS="$CFLAGS"                          \
    -DCMAKE_CXX_FLAGS="$CFLAGS"                        \
    -DUSE_SYSTEM_CURL=ON                               \
    -DUSE_SDL=ON                                       \
    -DUSE_SYSTEM_SDL=ON                                \
    -DUSE_SYSTEM_FFMPEG=OFF                            \
    -DUSE_DISCORD_RPC=ON                               \
    -DOpenGL_GL_PREFERENCE=LEGACY                      \
    -DSTATIC_LINK_LLVM=ON                              \
    -DBUILD_LLVM=OFF                                   \
    -G Ninja

ninja; build_status=$?;

cd ..

shellcheck .ci/*.sh

# If it compiled succesfully let's deploy.
# Azure and Cirrus publish PRs as artifacts only.
{   [ "$CI_HAS_ARTIFACTS" = "true" ];
} && SHOULD_DEPLOY="true" || SHOULD_DEPLOY="false"

if [ "$build_status" -eq 0 ] && [ "$SHOULD_DEPLOY" = "true" ]; then
    .ci/deploy-linux.sh
fi
