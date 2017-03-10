#!/bin/sh

for j in 500 1000 2000
do
cd /users/Junjieqc/openvswitch-2.6.0
tc qdisc del dev eth2 root
./remove_ovs.sh
insmod datapath/linux/openvswitch.ko threshold1=$1 threshold2=$2 ALPHA=$3
./ovs_config.sh
./ovs_port.sh
./rate_config.sh "$j"mbit
cd /users/Junjieqc/ovs
for i in 1 2 3 4 5
do 
iperf -c 10.10.1.2 -t 20 -i 1 > iperf-$j-$i.txt &
(sleep 5s
sockperf pp -i 10.10.1.2 -t 10 -p 8899 --tcp --pps=100 --full-log sockperf-$j-$i.log)
sleep 3s
echo finish $i

done
echo finish $j mbit
done
