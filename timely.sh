#!/bin/sh

usernow="Junjieqc"
ovspath="/users/Junjieqc/openvswitch-2.6.0"
scriptpath="/users/Junjieqc/ovs"

sudo sysctl -w net.ipv4.tcp_congestion_control=cubic
sudo sysctl -w net.ipv4.tcp_ecn=0
sudo tc qdisc del dev eth2 root

if [ ! -e /users/$usernow/timelylog ]; then
	mkdir /users/$usernow/timelylog
	echo mkdir /users/$usernow/timelylog
fi

for rate in 2000 1000 500 #by mbps
do
	if [ ! -e /users/$usernow/timelylog/timely"$rate"mbps ]; then
		mkdir /users/$usernow/timelylog/timely"$rate"mbps
		echo mkdir /users/$usernow/timelylog/timely"$rate"mbps
	fi
	for alpha in 820 1638 2458 3277 4096 
	#choose alpha from 0.1(820) to 0.5(4096)    scaled by 2^13=8192
	do
		if [ ! -e /users/$usernow/timelylog/timely"$rate"mbps/alpha"$alpha" ]; then
			mkdir /users/$usernow/timelylog/timely"$rate"mbps/alpha"$alpha"
			echo mkdir /users/$usernow/timelylog/timely"$rate"mbps/alpha"$alpha"
		fi
		echo start alpha=$alpha
		for beta in 820 1638 2458 3277 4096 4915 5734
		#from 0.1 to 0.7
		do
			if [ ! -e /users/$usernow/timelylog/timely"$rate"mbps/alpha"$alpha"/beta"$beta" ]; then
				mkdir /users/$usernow/timelylog/timely"$rate"mbps/alpha"$alpha"/beta"$beta"
				echo mkdir /users/$usernow/timelylog/timely"$rate"mbps/alpha"$alpha"/beta"$beta"
			fi
			echo start beta=$beta
			for qlow in 3000 5000 7000 10000 15000 20000 25000 30000 40000 50000 60000 70000 80000 90000 100000
			do
				for diff in 40000 50000 80000 # 3000 5000 7000 10000 15000 20000 25000 30000
				do
					if [ ! -e /users/$usernow/timelylog/timely"$rate"mbps/alpha"$alpha"/beta"$beta"/difference"$diff" ]; then
						mkdir /users/$usernow/timelylog/timely"$rate"mbps/alpha"$alpha"/beta"$beta"/difference"$diff"
						echo mkdir /users/$usernow/timelylog/timely"$rate"mbps/alpha"$alpha"/beta"$beta"/difference"$diff"
					fi
					qhigh=`expr $qlow + $diff`
					echo start Qlow=$qlow Qhigh=$qhigh alpha=$alpha beta=$beta test
					
					#remove_ovs
				echo remove ovs
				cd $ovspath
				./remove_ovs.sh
					
					#insmod openvswitch module
					echo insmod openvswitch module
					sudo insmod datapath/linux/openvswitch.ko TIMELY_Qlow=$qlow TIMELY_Qhigh=$qhigh TIMELY_ALPHA=$alpha TIMELY_BETA=$beta
					#eg. sudo insmod /users/$usernow/openvswitch-2.6.0/datapath/linux/openvswitch.ko TIMELY_Qlow=20000 TIMELY_Qhigh=30000 TIMELY_ALPHA=819 TIMELY_BETA=819
					
					#ovs_config
				./ovs_config.sh	

					#ovs_port
				./ovs_port.sh
					
					#rate_config
					echo rate_config
					sudo tc qdisc del dev eth2 root
					echo after del htb root
					sudo tc qdisc add dev eth2 root handle 1: htb default 1
					sudo tc class add dev eth2 parent 1:0 classid 1:1 htb rate "$rate"mbit
					sudo tc qdisc show dev eth2
					sudo tc class show dev eth2 root
				cd $scriptpath
					#start test
					echo start iperf and sockperf
					(iperf -c 10.10.1.2 -i 1 -t 20 > /users/$usernow/timelylog/timely"$rate"mbps/alpha"$alpha"/beta"$beta"/difference"$diff"/iperf"$qlow"_"$qhigh".log) &
					sleep 7s
					sockperf ping-pong -p 8899 -i 10.10.1.2 -t 10 --tcp --pps=100 --full-log /users/$usernow/timelylog/timely"$rate"mbps/alpha"$alpha"/beta"$beta"/difference"$diff"/sockperf"$qlow"_"$qhigh".log
					sleep 4s
					
					echo finish Qlow=$qlow Qhigh=$qhigh alpha=$alpha beta=$beta test
				done
				echo finish Qlow=$qlow
			done
			echo finish beta=$beta
		done
		echo finish alpha=$alpha
	done
	echo finish rate="$rate"mbps
done
mkdir /users/$usernow/finishtimely
