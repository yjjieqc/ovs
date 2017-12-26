#!/bin/sh
cd /proj/networklatency-PG0/yjjie/linux/net/sched
sudo tc qdisc del dev eth2 root
sudo rmmod sch_htb
sudo insmod sch_htb.ko threshold=$1
cat /sys/module/sch_htb/parameters/threshold
cd -
