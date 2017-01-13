#!/bin/sh

ovs-vsctl -- \
       add-br br0 -- \
       add-port br0 eth1 -- \
       add-port br0 vport1 -- set interface vport1 ofport_request=5 -- \
       add-port br0 vport2 -- set interface vport2 ofport_request=6 -- \
       set port eth1 qos=@newqos -- \
       --id=@newqos create qos type=linux-htb \
           other-config:max-rate=10000000000 \
           queues:123=@vif10queue \
           queues:234=@vif20queue -- \
       --id=@vif10queue create queue other-config:min-rate=4000000000 other-config:max-rate=10000000000 -- \
       --id=@vif20queue create queue other-config:min-rate=4000000000 other-config:max-rate=10000000000

