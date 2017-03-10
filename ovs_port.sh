sudo ovs-vsctl del-br br0
sudo ovs-vsctl add-br br0
sudo ovs-vsctl add-port br0 eth2
sudo ifconfig eth2 0
sudo ifconfig br0 up
sudo ifconfig br0 10.10.1.1 netmask 255.255.255.0
sudo ifconfig eth2 mtu 1500
sudo ifconfig br0 mtu 1500
