#!/bin/sh

iperf -c 10.10.1.2 -t 60 -P 2 1>./twoiperf/iperf1_$1.txt &
iperf -c 10.10.1.3 -t 60 -P 2 1>./twoiperf/iperf2_$1.txt &
(sleep 25s 
sockperf pp -i 10.10.1.2 -t 30 -p 8899 --tcp --pps=100 --full-log /users/Junjieqc/ovs/skpf1_$1.log) &
(sleep 25s 
sockperf pp -i 10.10.1.3 -t 30 -p 8899 --tcp --pps=100 --full-log /users/Junjieqc/ovs/skpf2_$1.log)
