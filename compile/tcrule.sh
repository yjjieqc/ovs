#!/bin/sh
echo "set ratelimit 2000mbps"
tc qdisc del dev eth2 root
 tc qdisc add dev eth2 root handle 1: htb default 10
 tc class add dev eth2 parent 1: classid 1:1 htb rate 10000mbit burst 15k
 tc class add dev eth2 parent 1:1 classid 1:10 htb rate 5000mbit
 tc class add dev eth2 parent 1:1 classid 1:20 htb rate 500mbit
 tc class add dev eth2 parent 1:1 classid 1:30 htb rate 1000kbit
sudo tc filter add dev eth2 parent 1:0 protocol ip prio 1 u32 match ip dst 10.10.1.2/24 flowid 1:20
