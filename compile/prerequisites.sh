#!/bin/sh

#install kernel
#cd /dev/
sudo apt-get update
sudo apt-get install git build-essential kernel-package fakeroot libncurses5-dev libssl-dev ccache xz-utils vim -y
sudo apt-get install dpkg-dev -y
