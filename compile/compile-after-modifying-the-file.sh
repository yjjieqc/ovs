#!/bin/sh

#compile
cd /mnt/shared/linux/
sudo cp /boot/config-`uname -r` .config
sudo yes '' | make oldconfig
sudo make clean
sudo make -j `getconf _NPROCESSORS_ONLN` deb-pkg LOCALVERSION=-custom
