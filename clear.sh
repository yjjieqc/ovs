#!/bin/sh


ovs-vsctl -- destroy qos eth1 -- clear port eth1 qos
ovs-vsctl --all destroy queue
echo "clean qos successfully"
