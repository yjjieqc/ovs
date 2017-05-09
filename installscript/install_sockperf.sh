#!/bin/sh

echo "installing iperf and sockperf"
sudo apt-get update
sudo apt-get install iperf

git clone http://git.killua.wang/yjjieqc/sockperf.git 
cd sockperf
./autogen.sh 
./configure 
make
sudo make install

echo "-------------install finished---------------------"
