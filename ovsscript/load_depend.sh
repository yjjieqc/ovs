sudo insmod /lib/modules/4.6.0-custom/kernel/lib/libcrc32c.ko
sudo insmod /lib/modules/4.6.0-custom/kernel/net/ipv4/udp_tunnel.ko
sudo insmod /lib/modules/4.6.0-custom/kernel/net/netfilter/nf_conntrack.ko
sudo insmod /lib/modules/4.6.0-custom/kernel/net/netfilter/nf_nat.ko
sudo insmod /lib/modules/4.6.0-custom/kernel/net/ipv6/netfilter/nf_defrag_ipv6.ko
sudo insmod /lib/modules/4.6.0-custom/kernel/net/ipv4/netfilter/nf_nat_ipv4.ko
sudo insmod /lib/modules/4.6.0-custom/kernel/net/ipv6/netfilter/nf_nat_ipv6.ko
sudo insmod /lib/modules/4.6.0-custom/kernel/net/sched/sch_htb.ko printfrequency=10000
