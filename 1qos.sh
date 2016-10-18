#!/bin/sh

ovs-vsctl set port eth1 qos=@newqos -- \
       --id=@newqos create qos type=linux-htb \
           other-config:max-rate=10000000000 \
           queues:111=@q0 \
           queues:222=@q1 -- \
       --id=@q0 create queue other-config:min-rate=2000000000 other-config:max-rate=10000000000 -- \
       --id=@q1 create queue other-config:min-rate=2000000000 other-config:max-rate=10000000000



ovs-ofctl add-flow br0 "nw_dst=10.10.1.2 actions=set_queue:111,normal"
ovs-ofctl add-flow br0 "nw_dst=10.10.1.3 actions=set_queue:222,normal"
