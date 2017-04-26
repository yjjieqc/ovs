#!/bin/sh

wget http://openvswitch.org/releases/openvswitch-2.6.0.tar.gz
tar xzvf openvswitch-2.6.0.tar.gz 
cd openvswitch-2.6.0/
apt-get install -y git libnl-dev automake autoconf gcc uml-utilities libtool build-essential pkg-config linux-headers-`uname -r`
./boot.sh
./configure --with-linux=/lib/modules/`uname -r`/build
make
make install
echo "--------------------finished-----------------"
