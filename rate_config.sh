sudo tc qdisc add dev eth2 root handle 1: htb default 1
sudo tc class add dev eth2 parent 1:0 classid 1:1 htb rate 2000mbit
