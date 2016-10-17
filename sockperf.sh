#!/bin/sh
iperf -c 10.10.1.11 -t 60 1> iperf1_$1.txt &
iperf -c 10.10.1.12 -t 60 1> iperf2_$1.txt &
(sleep 25s &&
sockperf pp -i 10.10.1.11 -t 30 -p 8899 --tcp 1>sockperf1.txt --full-log /users/Junjieqc/ovs/sockperf1_$1.log) &
(sleep 25s &&
sockperf pp -i 10.10.1.12 -t 30 -p 8899 --tcp 1>sockperf2.txt --full-log /users/Junjieqc/ovs/sockperf2_$1.log)
 
