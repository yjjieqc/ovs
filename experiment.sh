#!/bin/sh

for i in $(seq 10000 10000 90000)
do
t=$((t=$i+10000))
for j in 500 1000 2000
do

for k in 819 1638 2458 3278 4096 4915 5734 6553 7372 8191
do
cd /users/Junjieqc/openvswitch-2.6.0
tc qdisc del dev eth2 root
./remove_ovs.sh
insmod datapath/linux/openvswitch.ko threshold1=$i threshold2=$t ALPHA=$k
./ovs_config.sh
./ovs_port.sh
./rate_config.sh "$j"mbit
cd /users/Junjieqc/ovs
iperf -c 10.10.1.2 -t 20 -i 1 > /users/Junjieqc/testlog/th"$i"-th"$t"/"$j"mbit/iperf-$k.txt &
(sleep 5s
sockperf pp -i 10.10.1.2 -t 10 -p 8899 --tcp --pps=100 --full-log /users/Junjieqc/testlog/th"$i"-th"$t"/"$j"mbit/sockperf-$j-$k.log)
sleep 3s
echo finish threshold1=$i htb ratelimiter="$j"mbit delta=$k
done
echo finish $j mbit
done
echo finish threshold1=$i
done
