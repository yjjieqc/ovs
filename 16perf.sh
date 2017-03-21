#!/bin/sh

(iperf -c 10.10.1.11 -t 20 -i 10 -P 4 > iperf1to11.txt & 
iperf -c 10.10.1.12 -t 20 -i 10 -P 4 > iperf1to12.txt) & 
(sleep 5s  && 
(sockperf pp -i 10.10.1.11 -t 10 -p 8899 --tcp --pps=100 --full-log sockperf1to11.log & 
sockperf pp -i 10.10.1.12 -t 10 -p 9988 --tcp --pps=100 --full-log sockperf1to12.log))


