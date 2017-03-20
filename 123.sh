#!/bin/sh

logpath="../testlog/multiple/"
a=5

i=20000
t=$((t=$i+10000))
j=1000
for k in 819 1638 2458 3278 4096
do
        cd /users/Junjieqc/openvswitch-2.6.0
        tc qdisc del dev eth2 root
        ./remove_ovs.sh
        insmod datapath/linux/openvswitch.ko threshold1=$i threshold2=$t ALPHA=$k
        ./ovs_config.sh
        ./ovs_port.sh
        ./rate_config.sh "$j"mbit
        cd /users/Junjieqc/ovs
        if [ ! -d ""$logpath"multiple16/th-"$i"/"$j"mbit" ] ; then
        mkdir -p ""$logpath"multiple16/th-"$i"/"$j"mbit"
        echo make folder """$logpath"multiple16/th-"$i"/"$j"mbit""
        else
        echo folder th-"$i"/"$j"mbit"" already exist
        fi
        iperf -c 10.10.1.2 -t 20 -i 5 -P 16 > """$logpath"multiple16/th-"$i"/"$j"mbit/iperf-$k.txt
        (sleep 5s
        sockperf pp -i 10.10.1.2 -t 10 -p 8899 --tcp --pps=100 --full-log """$logpath"multiple16/th-"$i"/"$j"mbit/sockperf-$k.log)
        a=`expr $a - 1`
        sleep 5s
        echo  ---------finish threshold $i $j mbit STILL "\033[31m $a \033[0m" REMAIN ------
done

echo "<===================================" all finished "====================================>"

