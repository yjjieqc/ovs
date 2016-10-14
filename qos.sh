#!/bin/sh

ovs-vsctl set port eth1 qos=@newqos -- \
       --id=@newqos create qos type=linux-htb \
           other-config:max-rate=10000000000 \
           queues:111=@q0 \
           queues:222=@q1 -- \
       --id=@q0 create queue other-config:min-rate=1000000000 other-config:max-rate=1000000000 -- \
       --id=@q1 create queue other-config:min-rate=1000000000 other-config:max-rate=1000000000


ovs-ofctl add-flow br0 in_port=5,actions=set_queue:111,normal
ovs-ofctl add-flow br0 in_port=6,actions=set_queue:222,normal

ovs-vsctl set interface vport1 ingress_policing_rate=100000
ovs-vsctl set interface vport2 ingress_policing_rate=100000
