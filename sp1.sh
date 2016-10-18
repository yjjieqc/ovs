#!/bin/sh

iperf -c 10.10.1.2 -t 60 1>./oneiperf/iperf1_$1.txt &
iperf -c 10.10.1.3 -t 60 1>./oneiperf/iperf2_$1.txt &
(sleep 25s 
sockperf pp -i 10.10.1.2 -t 30 -p 8899 --tcp --pps=100 --full-log /users/Junjieqc/ovs/sockperf1_$1.log) &
(sleep 25s 
sockperf pp -i 10.10.1.3 -t 30 -p 8899 --tcp --pps=100 --full-log /users/Junjieqc/ovs/sockperf2_$1.log)
