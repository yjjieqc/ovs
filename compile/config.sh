#!/bin/sh
#cp /boot/config-`uname -r` .config
yes '' | make oldconfig
#make menuconfig
make clean
make -j `getconf _NPROCESSORS_ONLN` deb-pkg LOCALVERSION=-custom
