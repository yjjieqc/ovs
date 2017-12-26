#!/bin/sh
 
iperf -c 10.103.90.27 -n 2000000000 -i 10 &
(sleep 25s 
sockperf pp -i 10.103.90.27 -t 5 -p 8899 --tcp --pps=100  --full-log ./sockperf.log) 
