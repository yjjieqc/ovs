#!/bin/sh

ovs-vsctl add-br br0
ovs-vsctl add-port br0 $1
ifconfig $1 up
ifconfig $1 0
ifconfig br0 10.10.1.1 netmask 255.255.255.0
ifconfig $1 mtu 9000
ifconfig br0 mtu 9000
