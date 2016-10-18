#!/bin/sh

/sbin/modprobe openvswitch

rmmod openvswitch
#ovs-dpctl del-dp system@ovs-system
modprobe nf_conntrack
modprobe nf_defrag_ipv4
modprobe nf_defrag_ipv6
modprobe libcrc32c
modprobe vxlan
modprobe gre
insmod /users/Junjieqc/openvswitch-2.5.0/datapath/linux/openvswitch.ko

kill `cd /usr/local/var/run/openvswitch && cat ovsdb-server.pid ovs-vswitchd.pid`

