#!/bin/sh

for i in 25 30 35 40 
do

j=$((j = $i + 4))
echo start k1 = $i k2 = $j test
sudo tc qdisc del dev eth2 root
sudo rmmod sch_htb
sudo insmod /lib/modules/4.8.0-rc2-microburst/kernel/net/sched/sch_htb.ko k_1=$i k_2=$j
cat /sys/module/sch_htb/parameters/k_1
cat /sys/module/sch_htb/parameters/k_2
sudo tc qdisc add dev eth2 root handle 1: htb default 1
sudo tc class add dev eth2 parent 1:0 classid 1:1 htb rate 500mbit
(iperf -c 10.10.1.2 -i 2 -t 30 >./iperf-k1"$i"-k2"$j".log) &
sleep 5s
sockperf pp -p 8899 -i 10.10.1.2 -t 20 --tcp --pps=200 --full-log ./sockperf-k1"$i"-k2"$j".log
sleep 5s
echo finish $i

done
