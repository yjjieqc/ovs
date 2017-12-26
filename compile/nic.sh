#!/bin/sh

cat /proc/sys/net/ipv4/tcp_ecn  
sudo sysctl -w net.ipv4.tcp_ecn=2
cat /proc/sys/net/ipv4/tcp_congestion_control 
sudo sysctl -w net.ipv4.tcp_congestion_control=dctcp
cat /proc/sys/net/ipv4/tcp_congestion_control

ethtool -k eth2 
ethtool -K eth2 gso off   
ethtool -K eth2 tso off 
