#!/bin/sh

iperf -c 10.10.1.11 -t 10 -i 1 &
iperf -c 10.10.1.11 -t 10 -i 1 &
sockperf pp -i 10.10.1.11 -p 8899 --tcp &
sockperf pp -i 10.10.1.12 -p 8899 --tcp
