#!/bin/sh

logpath="../testlog/multiple/"
for i in $(seq 10000 10000 30000)
do
t=$((t=$i+10000))
for j in 2000 
do

for k in 819 1638 2458 3278 4096
do
cd /users/Junjieqc/openvswitch-2.6.0
tc qdisc del dev eth2 root
./remove_ovs.sh
insmod datapath/linux/openvswitch.ko threshold1=$i threshold2=$t ALPHA=$k
./ovs_config.sh
./ovs_port.sh
./rate_config.sh "$j"mbit
cd /users/Junjieqc/ovs
iperf -c 10.10.1.2 -t 20 -i 5 -P 4 > iperf.txt &
(sleep 5s
sockperf pp -i 10.10.1.2 -t 10 -p 8899 --tcp --pps=100 )


sleep 3s
echo finish delta $k
done
echo finish $j
done
echo finish threshold $i $j mbit
done

