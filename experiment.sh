#!/bin/sh

logpath="../testlog/"
for i in $(seq 10000 10000 90000)
do
t=$((t=$i+10000))
for j in 500 1000 2000
do
for k in 1024 2048 4096
do
cd /root/Junjie/openvswitch-2.6.0
tc qdisc del dev eth0 root
./remove_ovs.sh
insmod datapath/linux/openvswitch.ko threshold1=$i threshold2=$t ALPHA=$k
./ovs_config.sh
./ovs_port.sh
./rate_config.sh "$j"mbit
cd /root/Junjie/ovs
if [! -d ""$logpath"th-"$i"/"$j"mbit" ] ; then
mkdir -p """$logpath"th-"$i"/"$j"mbit""
echo make folder """$logpath"th-"$i"/"$j"mbit""
else
echo folder """$logpath"th-"$i"/"$j"mbit"" already exist
fi
iperf -c 192.168.0.1 -t 20 -i 1 > """$logpath"th-"$i"/"$j"mbit""/iperf-$j-$i.txt &
(sleep 5s
sockperf pp -i 192.168.0.1 -t 10 -p 8899 --tcp --pps=100 --full-log """$logpath"th-"$i"/"$j"mbit""/sockperf-$j-$i.log)

sleep 3s
echo finish delta $k
done
echo finish $j
done
echo finish threshold $i $j mbit
done
