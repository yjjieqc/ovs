#!/bin/sh

#modinfo datapath/linux/openvswitch.ko
#insmod datapath/linux/openvswitch.ko MSS=8946 ECE_CLEAR=1
#insmod datapath/linux/openvswitch.ko MSS=1446 ECE_CLEAR=0

kill `cd /usr/local/var/run/openvswitch && cat ovsdb-server.pid ovs-vswitchd.pid`
rm -rf /usr/local/etc/openvswitch
mkdir -p /usr/local/etc/openvswitch
ovsdb-tool create /usr/local/etc/openvswitch/conf.db vswitchd/vswitch.ovsschema
ovsdb-server --remote=punix:/usr/local/var/run/openvswitch/db.sock \
                    --remote=db:Open_vSwitch,Open_vSwitch,manager_options \
        --pidfile --detach
ovs-vsctl --no-wait init
ovs-vswitchd --pidfile --detach
