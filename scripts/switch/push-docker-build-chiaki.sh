#!/bin/bash
# Usage: Open netload via hbmenu (press Y), then
# jingkai@jkwin:~/chiaki-ng/scripts/switch$ ./push-podman-build-chiaki.sh -a 192.168.1.35
# Sending chiaki-ng.nro, 22095181 bytes
# 10289946 sent (46.57%), 918 blocks
# starting server
# server active ...
# initNxLink
# [I] Parse config file chiaki.conf
#...
#[INFO] Gamepad detected: Nintendo Switch Controller

cd "`dirname $(readlink -f ${0})`/../.."

# A container from a previous run can still be holding port 28771 (e.g. this
# script was Ctrl-C'd, or Docker Desktop is slow to release the port after
# the container exited) - clean it up first instead of failing with "port is
# already allocated".
stale="`docker ps -aq --filter publish=28771`"
if [ -n "$stale" ]; then
    docker rm -f $stale >/dev/null 2>&1
fi

docker run --rm \
    -v "`pwd`:/build/chiaki" \
    -w "/build/chiaki" \
    -ti -p 28771:28771 \
    --entrypoint /opt/devkitpro/tools/bin/nxlink \
    docker.io/xlanor/chiaki-ng-switch-builder:latest \
    "$@" -s /build/chiaki/build_switch/switch/chiaki-ng.nro

