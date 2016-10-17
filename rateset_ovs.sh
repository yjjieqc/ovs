#!/bin/sh

sudo ovs-vsctl -- set port $1 qos=@newqos -- --id=@newqos create qos type=linux-htb  other-config:max-rate=$2 queues=0=@q0 -- --id=@q0 create queue other-config:min-rate=$2 other-config:max-rate=$2
