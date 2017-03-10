#!/bin/sh

iperf -c 10.10.1.2 -t 20 -i 1 > iperf-$1.txt &
(sleep 5s 
sockperf pp -i 10.10.1.2 -t 10 -p 8899 --tcp --pps=100 --full-log sockperf-$1.log) 
