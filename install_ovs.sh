#!/bin/sh

wget http://openvswitch.org/releases/openvswitch-2.5.0.tar.gz
tar xzvf openvswitch-2.5.0.tar.gz 
cd openvswitch-2.5.0/
apt-get install -y git automake autoconf gcc uml-utilities libtool build-essential pkg-config linux-headers-`uname -r`
apt-get -y install libnl-dev
./boot.sh
./configure --with-linux=/lib/modules/`uname -r`/build
make
make install
echo "--------------------finished-----------------"
