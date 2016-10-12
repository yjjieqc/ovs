#!/bin/sh

iperf -c 10.10.1.11 -t 10 -i 1 &
iperf -c 10.10.1.12 -t 10 -i 1
