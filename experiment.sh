#!/bin/sh

for i in $(seq 10000 10000 90000)
do
t=$((t=$i+10000))
for j in 500 1000 2000
do
for k in 1024 2048 4096
do
cd /users/Junjieqc/openvswitch-2.6.0
tc qdisc del dev eth2 root
./remove_ovs.sh
insmod datapath/linux/openvswitch.ko threshold1=$i threshold2=$t ALPHA=$k
./ovs_config.sh
./ovs_port.sh
./rate_config.sh "$j"mbit
cd /users/Junjieqc/ovs
iperf -c 10.10.1.2 -t 20 -i 1 > iperf-$j-$i.txt &
(sleep 5s
sockperf pp -i 10.10.1.2 -t 10 -p 8899 --tcp --pps=100 --full-log sockperf-$j-$i.log)
sleep 3s
echo finish delta $k
done
echo finish $j
done
echo finish threshold $i $j mbit
done
