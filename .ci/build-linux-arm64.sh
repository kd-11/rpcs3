#!/bin/sh -ex

if [ -z "$CIRRUS_CI" ]; then
   cd rpcs3 || exit 1
fi

apt -y update && \
apt -y install clang build-essential libasound2-dev libpulse-dev libopenal-dev libglew-dev zlib1g-dev libedit-dev libvulkan-dev libudev-dev git libevdev-dev libsdl2-dev libjack-dev libsndio-dev
apt -y install qt6-base-dev qt6-declarative-dev qt6-multimedia-dev qt6-svg-dev

sh -ex .ci/build-linux.sh
