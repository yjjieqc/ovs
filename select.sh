#!/bin/sh
echo th10-20 delta 1024
echo latecy of a 1000mbit bandwidth
for i in $(seq 1 5)
do
sed -n '7p' sockperf-1000-"$i".log| cut -d ' ' -f 4
done
