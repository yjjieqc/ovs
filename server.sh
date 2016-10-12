#!/bin/sh

iperf -s &
sockperf server -p  8899 --tcp
