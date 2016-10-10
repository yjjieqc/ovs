#!/bin/sh

echo "installing OVS"
git clone https://github.com/Mellanox/sockperf.git
cd sockperf
./autogen.sh 
./configure 
make
sudo make install

echo "-------------install finished---------------------"
