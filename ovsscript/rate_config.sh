sudo tc qdisc add dev eth0 root handle 1: htb default 1
sudo tc class add dev eth0 parent 1:0 classid 1:1 htb rate $1 
