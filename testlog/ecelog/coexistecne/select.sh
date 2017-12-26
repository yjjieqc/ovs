#!/bin/sh
echo 99.9% latency
for i in $(seq 1 5)
do
sed -n '13p' tcpece-"$i"G-sockperf.log| tr -s ' '| cut -d ' ' -f 6
done
echo 99% latency
for j in $(seq 1 5)
do
sed -n '14p' tcpece-"$j"G-sockperf.log| tr -s ' '| cut -d ' ' -f 6
done
echo 90% latency
for k in $(seq 1 5)
do
sed -n '15p' tcpece-"$k"G-sockperf.log| tr -s ' '| cut -d ' ' -f 6
done
