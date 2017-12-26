#!/bin/sh

cat /proc/sys/net/ipv4/tcp_ecn  
sudo sysctl -w net.ipv4.tcp_ecn=2
cat /proc/sys/net/ipv4/tcp_congestion_control 
sudo sysctl -w net.ipv4.tcp_congestion_control=dctcp
cat /proc/sys/net/ipv4/tcp_congestion_control

ethtool -k eth2 
ethtool -K eth2 gso off   
ethtool -K eth2 tso off 


for i in 0 1000 2000 3000 4000 5000 6000 7000 8000 9000 10000 
do
        echo start thershold=$i test
        sudo tc qdisc del dev eth2 root
        sudo rmmod sch_htb
        sudo insmod /proj/networklatency-PG0/hunter/4.6/linux/net/sched/sch_htb.ko threshold="$i"
        sudo tc qdisc add dev eth2 root handle 1: htb default 1
        sudo tc class add dev eth2 parent 1:0 classid 1:1 htb rate 500mbit
        (iperf -c 10.10.1.1 -i 1 -t 20 > /users/wenfeiwu/log_500/iperf"$i".log) &
        sleep 5s
        sockperf ping-pong -p 8899 -i 10.10.1.1 -t 10 --tcp --pps=1000 --full-log /users/wenfeiwu/log_500/sockperf"$i".log
        sleep 10s
        echo finish threshold=$i

done
