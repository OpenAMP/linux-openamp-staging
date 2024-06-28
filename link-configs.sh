#!/bin/bash

S=$1
: ${S:=~/w/proj/openamp/ci-builds}

for d in $S/layers/meta-openamp/recipes-kernel/linux/openamp-kmeta/cfg $S/layers/meta-openamp-bsp/recipes-kernel/linux/linux-openamp/openamp-bsp-kmeta/cfg/; do
    for f in $d/*.cfg; do
        for a in arm64 arm; do
            F=$(basename $f)
            F=${F/.cfg/.config}
            ln -sf $f arch/$a/configs/${F}
        done
    done
done

build-compound-config() {
    a=$1; shift
    O=arch/$a/configs/openamp-ci.config

    echo "# Openamp CI config built from the following fragments:" >$O
    echo "#     $@" >>$O
    echo "" >>$O

    for f in "$@"; do
        F=$(basename $f)
        F=${F/.cfg/.config}
        echo "############### $f" >>$O
        cat arch/$a/configs/$F >>$O
        echo "" >>$O
        echo "" >>$O
    done
}

build-compound-config arm64 openamp.cfg remoteproc-arm64.cfg  zynqmp-still-needed-def.cfg zynqmp-still-needed-com.cfg
build-compound-config arm   openamp.cfg remoteproc-armv7a.cfg multi_v7_extras.cfg multi_v7_stmp1_extras.cfg