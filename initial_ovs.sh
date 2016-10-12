#!/bin/sh


#ovs-dpctl del-dp system@ovs-system
modprobe nf_conntrack
modprobe nf_defrag_ipv4
modprobe nf_defrag_ipv6
modprobe libcrc32c
modprobe vxlan
modprobe gre
insmod datapath/linux/openvswitch.ko
