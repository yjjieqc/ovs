/*
 * Copyright (c) 2007-2015 Nicira, Inc.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of version 2 of the GNU General Public
 * License as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301, USA
 */
 
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt
 
#include <linux/init.h>
#include <linux/module.h>
#include <linux/if_arp.h>
#include <linux/if_vlan.h>
#include <linux/in.h>
#include <linux/ip.h>
#include <linux/jhash.h>
#include <linux/delay.h>
#include <linux/time.h>
#include <linux/etherdevice.h>
#include <linux/genetlink.h>
#include <linux/kernel.h>
#include <linux/kthread.h>
#include <linux/mutex.h>
#include <linux/percpu.h>
#include <linux/rcupdate.h>
#include <linux/tcp.h>
#include <linux/udp.h>
#include <linux/version.h>
#include <linux/ethtool.h>
#include <linux/wait.h>
#include <asm/div64.h>
#include <linux/highmem.h>
#include <linux/netfilter_bridge.h>
#include <linux/netfilter_ipv4.h>
#include <linux/inetdevice.h>
#include <linux/list.h>
#include <linux/openvswitch.h>
#include <linux/rculist.h>
#include <linux/dmi.h>
#include <net/genetlink.h>
#include <net/net_namespace.h>
#include <net/netns/generic.h>
 
#include "datapath.h"
#include "conntrack.h"
#include "flow.h"
#include "flow_table.h"
#include "flow_netlink.h"
#include "gso.h"
#include "vport-internal_dev.h"
#include "vport-netdev.h"
/*************************qiwei's logic************************/
extern unsigned int queuelength;
#define ecn_mark_threshold 10000;
/**************************************************************/
 
/*************************************keqiang's logic**************************/
//keqiang's logic
#include <linux/hashtable.h>
#include <net/tcp.h>
 
//keqiang's logic
#define BRIDGE_NAME "br0" //help determine the direction of a packet, when we move to container, we only compare first 2 char
#define OVS_PACK_HEADROOM 32
#define MSS_DEFAULT (9000U - 14U - 20U -20U)  //in bytes
#define TBL_SIZE 15U
#define RWND_INIT 10U*MSS
 
#define RWND_MIN (MSS)
//#define RWND_STEP (MSS/2 + MSS/4)
#define RWND_STEP (MSS)
 
/*RWND / RTT = Tput
so RWND = Tput*RTT
the max Tput possible we allow is 10Gbps and
the max RTT we anticipate is 4 msec
so RWND_CLAMP is defined as following
the RWND_SSTHRESH is "RWND_CLAMP >> 1"
*/
#define RWND_CLAMP (10*1000*1000*4/8) //4 means the maximal latency expected (4 msec), in bytes
#define RWND_SSTHRESH_INIT (RWND_CLAMP >> 1)
#define DCTCP_ALPHA_INIT 1024U
#define DCTCP_MAX_ALPHA  1024U
/* customer logic added by keqiang */
/*pass a kernel parameter to initialized threshold*/
static unsigned int MSS = MSS_DEFAULT;
module_param(MSS, uint, 0644);
MODULE_PARM_DESC(MSS, "An unsigned int to initlize the MSS");
 
static unsigned int ECE_CLEAR = 0;
module_param(ECE_CLEAR, uint, 0644);
MODULE_PARM_DESC(ECE_CLEAR, "An unsigned int to initlize the ECE_CLEAR--whether clear the ECE bit in ACK");
 
static unsigned int dctcp_shift_g __read_mostly = 4; /* g = 1/2^4 */
module_param(dctcp_shift_g, uint, 0644);
MODULE_PARM_DESC(dctcp_shift_g, "parameter g for updating dctcp_alpha");
 
/*besides flow table, we need to add two new hash tables
RCU enabled hash table, high performant hash table
benchmark data: a 4K size table, insertion takes 83ns, deletion 125ns, lookup 7ns
*/
static DEFINE_SPINLOCK(datalock);
static DEFINE_SPINLOCK(acklock);
 
/*TODO for production code, use resizable hashtable
A technique related to IBM, https://lwn.net/Articles/612021/
*/
static DEFINE_HASHTABLE(rcv_data_hashtbl, TBL_SIZE);
static DEFINE_HASHTABLE(rcv_ack_hashtbl, TBL_SIZE);
 
struct rcv_data {
    u64 key; //key of a flow, {LOW16(srcip), LOW16(dstip), tcpsrc, tcpdst}
    u32 ecn_bytes_per_ack; //32 bit should be sufficient
    u32 total_bytes_per_ack; //32 bit should be sufficient
    spinlock_t lock; //lock for read/write, write/write conflicts
    struct hlist_node hash;
    struct rcu_head rcu;
};
 
struct rcv_ack {
    u64 key;
    u32 rwnd;
    u32 rwnd_ssthresh;
    u32 rwnd_clamp;
    u32 alpha;
    u32 snd_una;
    u32 snd_nxt;
    u32 next_seq;
    u8 snd_wscale;
    u32 snd_rwnd_cnt;
    u16 prior_real_rcv_wnd;
    u32 dupack_cnt;
    bool loss_detected_this_win;
    bool reduced_this_win;
    u32 ecn_bytes;
    u32 total_bytes;
    spinlock_t lock;
    struct hlist_node hash;
    struct rcu_head rcu;
};
 
/*rcv_data_hashtbl functions*/
 
static u16 ovs_hash_min(u64 key, int size) {
    u16 low16;
    u32 low32;
 
    low16 = key & ((1UL << 16) - 1);
    low32 = key & ((1UL << 32) - 1);
 
    low32 = low32 >> 16;
    return (low16 + low32) % (1 << size);
}
 
//insert a new entry
static void rcv_data_hashtbl_insert(u64 key, struct rcv_data *value)
{
    u32 bucket_hash;
    bucket_hash = ovs_hash_min(key, HASH_BITS(rcv_data_hashtbl)); //hash_min is the same as hash_long if key is 64bit
    //lock the table
    spin_lock(&datalock);
    hlist_add_head_rcu(&value->hash, &rcv_data_hashtbl[bucket_hash]);
    spin_unlock(&datalock);
}
 
static void free_rcv_data_rcu(struct rcu_head *rp)
{
    struct rcv_data * tofree = container_of(rp, struct rcv_data, rcu);
    kfree(tofree);
}
 
static void rcv_data_hashtbl_delete(struct rcv_data *value)
{
    //lock the table
    spin_lock(&datalock);
    hlist_del_init_rcu(&value->hash);
    spin_unlock(&datalock);
    call_rcu(&value->rcu, free_rcv_data_rcu);
}
 
//caller must use "rcu_read_lock()" to guard it
static struct rcv_data * rcv_data_hashtbl_lookup(u64 key)
{
    int j = 0;
    struct rcv_data * v_iter = NULL;
 
 
    j = ovs_hash_min(key, HASH_BITS(rcv_data_hashtbl));
    hlist_for_each_entry_rcu(v_iter, &rcv_data_hashtbl[j], hash)
    if (v_iter->key == key) /* iterm found*/
        return v_iter;
    return NULL; /*return NULL if can not find it */
}
 
//delete all entries in the hashtable
static void rcv_data_hashtbl_destroy(void)
{
    struct rcv_data * v_iter;
    struct hlist_node * tmp;
    int j = 0;
 
    rcu_barrier(); //wait until all rcu_call are finished
 
    spin_lock(&datalock); //no new insertion or deletion !
    hash_for_each_safe(rcv_data_hashtbl, j, tmp, v_iter, hash) {
        hash_del(&v_iter->hash);
        kfree(v_iter);
        pr_info("delete one entry from rcv_data_hashtbl table\n");
    }
    spin_unlock(&datalock);
}
 
/*functions for rcv_ack_hashtbl*/
//insert a new entru
static void rcv_ack_hashtbl_insert(u64 key, struct rcv_ack *value)
{
    u32 bucket_hash;
    bucket_hash = ovs_hash_min(key, HASH_BITS(rcv_ack_hashtbl)); //hash_min is the same as hash_long if key is 64bit
    //lock the table
    spin_lock(&acklock);
    hlist_add_head_rcu(&value->hash, &rcv_ack_hashtbl[bucket_hash]);
    spin_unlock(&acklock);
}
 
static void free_rcv_ack_rcu(struct rcu_head *rp)
{
    struct rcv_ack * tofree = container_of(rp, struct rcv_ack, rcu);
    kfree(tofree);
}
 
static void rcv_ack_hashtbl_delete(struct rcv_ack *value)
{
    //lock the table
    spin_lock(&acklock);
    hlist_del_init_rcu(&value->hash);
    spin_unlock(&acklock);
    call_rcu(&value->rcu, free_rcv_ack_rcu);
}
 
//caller must use "rcu_read_lock()" to guard it
static struct rcv_ack * rcv_ack_hashtbl_lookup(u64 key)
{
    int j = 0;
    struct rcv_ack * v_iter = NULL;
 
 
    j = ovs_hash_min(key, HASH_BITS(rcv_ack_hashtbl));
    hlist_for_each_entry_rcu(v_iter, &rcv_ack_hashtbl[j], hash)
    if (v_iter->key == key) /* iterm found*/
        return v_iter;
    return NULL; /*return NULL if can not find it */
}
 
//delete all entries in the hashtable
static void rcv_ack_hashtbl_destroy(void)
{
    struct rcv_ack * v_iter;
    struct hlist_node * tmp;
    int j = 0;
 
    rcu_barrier(); //wait until all rcu_call are finished
 
    spin_lock(&acklock); //no new insertion or deletion !
    hash_for_each_safe(rcv_ack_hashtbl, j, tmp, v_iter, hash) {
        hash_del(&v_iter->hash);
        kfree(v_iter);
        pr_info("delete one entry from rcv_ack_hashtbl table\n");
    }
    spin_unlock(&acklock);
}
 
//clear the 2 hash tables we added, used in module exit function
static void __hashtbl_exit(void) {
    rcv_data_hashtbl_destroy();
    rcv_ack_hashtbl_destroy();
}
//rcv_data and rcv_ack hash table tests*/
 
static void __hashtable_test(void) {
    u64 i;
    u64 j;
    struct timeval tstart;
    struct timeval tend;
 
    printk(KERN_INFO "start rcv_data/ack_hashtbl tests.\n");
    do_gettimeofday(&tstart);
    for (i = 0; i < (1 << TBL_SIZE); i ++) {
        struct rcv_data * value = NULL;
        value = kzalloc(sizeof(*value), GFP_KERNEL);
        value->key = i;
        rcv_data_hashtbl_insert(i, value);
    }
 
    do_gettimeofday(&tend);
    printk("rcv_data_hashtbl insert time taken: %ld microseconds\n", 1000000 * (tend.tv_sec - tstart.tv_sec) +
           (tend.tv_usec - tstart.tv_usec) );
 
    //Lookup performance
    do_gettimeofday(&tstart);
    for (i = 0; i < (1 << TBL_SIZE); i ++) {
        for (j = 0; j < (1 << TBL_SIZE); j ++) {
            struct rcv_data * value = NULL;
            rcu_read_lock();
            value = rcv_data_hashtbl_lookup(j);
            rcu_read_unlock();
        }
    }
    do_gettimeofday(&tend);
    printk("rcv_data_hashtbl lookup time taken: %ld microseconds\n", 1000000 * (tend.tv_sec - tstart.tv_sec) +
           (tend.tv_usec - tstart.tv_usec) );
 
    //correctness check of lookup
    for (i = 0; i < (1 << TBL_SIZE); i ++) {
        struct rcv_data * value = NULL;
        rcu_read_lock();
        value = rcv_data_hashtbl_lookup(i);
        if (value)
            ;
        //printk("lookup okay, value->key:%lu\n", value->key);
        else
            printk("rcv_data_hashtbl lookup bad!\n");
        rcu_read_unlock();
    }
 
    //delete performacne
    do_gettimeofday(&tstart);
    for (i = 0; i < (1 << TBL_SIZE); i ++) {
        struct rcv_data * value = NULL;
        rcu_read_lock();
        value = rcv_data_hashtbl_lookup(i);
        rcu_read_unlock();
        rcv_data_hashtbl_delete(value);
    }
    do_gettimeofday(&tend);
    printk("rcv_data_hashtbl deletion time taken: %ld microseconds\n", 1000000 * (tend.tv_sec - tstart.tv_sec) +
           (tend.tv_usec - tstart.tv_usec) );
 
    /*rcv_ack_hashtbl performance*/
    do_gettimeofday(&tstart);
    for (i = 0; i < (1 << TBL_SIZE); i ++) {
        struct rcv_ack * value = NULL;
        value = kzalloc(sizeof(*value), GFP_KERNEL);
        value->key = i;
        rcv_ack_hashtbl_insert(i, value);
    }
 
    do_gettimeofday(&tend);
    printk("rcv_ack_hashtbl insert time taken: %ld microseconds\n", 1000000 * (tend.tv_sec - tstart.tv_sec) +
           (tend.tv_usec - tstart.tv_usec) );
 
    //Lookup performance
    do_gettimeofday(&tstart);
    for (i = 0; i < (1 << TBL_SIZE); i ++) {
        for (j = 0; j < (1 << TBL_SIZE); j ++) {
            struct rcv_ack * value = NULL;
            rcu_read_lock();
            value = rcv_ack_hashtbl_lookup(j);
            rcu_read_unlock();
        }
    }
    do_gettimeofday(&tend);
    printk("rcv_ack_hashtbl lookup time taken: %ld microseconds\n", 1000000 * (tend.tv_sec - tstart.tv_sec) +
           (tend.tv_usec - tstart.tv_usec) );
 
    //correctness check of lookup
    for (i = 0; i < (1 << TBL_SIZE); i ++) {
        struct rcv_ack * value = NULL;
        rcu_read_lock();
        value = rcv_ack_hashtbl_lookup(i);
        if (value)
            ;
        //printk("lookup okay, value->key:%lu\n", value->key);
        else
            printk("rcv_ack_hashtbl lookup bad!\n");
        rcu_read_unlock();
    }
 
    //delete performacne
    do_gettimeofday(&tstart);
    for (i = 0; i < (1 << TBL_SIZE); i ++) {
        struct rcv_ack * value = NULL;
        rcu_read_lock();
        value = rcv_ack_hashtbl_lookup(i);
        rcu_read_unlock();
        rcv_ack_hashtbl_delete(value);
    }
    do_gettimeofday(&tend);
    printk("rcv_ack_hashtbl deletion time taken: %ld microseconds\n", 1000000 * (tend.tv_sec - tstart.tv_sec) +
           (tend.tv_usec - tstart.tv_usec) );
 
    printk(KERN_INFO "end rcv_data/ack_hashtbl tests.\n");
}
/* end of kqiang's logic*/
/******************************************************************************/
 
int ovs_net_id __read_mostly;
EXPORT_SYMBOL_GPL(ovs_net_id);
 
static struct genl_family dp_packet_genl_family;
static struct genl_family dp_flow_genl_family;
static struct genl_family dp_datapath_genl_family;
 
static const struct nla_policy flow_policy[];
 
static struct genl_multicast_group ovs_dp_flow_multicast_group = {
    .name = OVS_FLOW_MCGROUP
};
 
static struct genl_multicast_group ovs_dp_datapath_multicast_group = {
    .name = OVS_DATAPATH_MCGROUP
};
 
struct genl_multicast_group ovs_dp_vport_multicast_group = {
    .name = OVS_VPORT_MCGROUP
};
 
/* Check if need to build a reply message.
 * OVS userspace sets the NLM_F_ECHO flag if it needs the reply.
 */
static bool ovs_must_notify(struct genl_family *family, struct genl_info *info,
                unsigned int group)
{
    return info->nlhdr->nlmsg_flags & NLM_F_ECHO ||
           genl_has_listeners(family, genl_info_net(info), group);
}
 
static void ovs_notify(struct genl_family *family, struct genl_multicast_group *grp,
               struct sk_buff *skb, struct genl_info *info)
{
    genl_notify(family, skb, info, GROUP_ID(grp), GFP_KERNEL);
}
 
/**
 * DOC: Locking:
 *
 * All writes e.g. Writes to device state (add/remove datapath, port, set
 * operations on vports, etc.), Writes to other state (flow table
 * modifications, set miscellaneous datapath parameters, etc.) are protected
 * by ovs_lock.
 *
 * Reads are protected by RCU.
 *
 * There are a few special cases (mostly stats) that have their own
 * synchronization but they nest under all of above and don't interact with
 * each other.
 *
 * The RTNL lock nests inside ovs_mutex.
 */
 
static DEFINE_MUTEX(ovs_mutex);
 
void ovs_lock(void)
{
    mutex_lock(&ovs_mutex);
}
 
void ovs_unlock(void)
{
    mutex_unlock(&ovs_mutex);
}
 
#ifdef CONFIG_LOCKDEP
int lockdep_ovsl_is_held(void)
{
    if (debug_locks)
        return lockdep_is_held(&ovs_mutex);
    else
        return 1;
}
EXPORT_SYMBOL_GPL(lockdep_ovsl_is_held);
#endif
 
static int queue_gso_packets(struct datapath *dp, struct sk_buff *,
                 const struct sw_flow_key *,
                 const struct dp_upcall_info *,
                 uint32_t cutlen);
static int queue_userspace_packet(struct datapath *dp, struct sk_buff *,
                  const struct sw_flow_key *,
                  const struct dp_upcall_info *,
                  uint32_t cutlen);
 
/* Must be called with rcu_read_lock. */
static struct datapath *get_dp_rcu(struct net *net, int dp_ifindex)
{
    struct net_device *dev = dev_get_by_index_rcu(net, dp_ifindex);
 
    if (dev) {
        struct vport *vport = ovs_internal_dev_get_vport(dev);
        if (vport)
            return vport->dp;
    }
 
    return NULL;
}
 
/* The caller must hold either ovs_mutex or rcu_read_lock to keep the
 * returned dp pointer valid.
 */
static inline struct datapath *get_dp(struct net *net, int dp_ifindex)
{
    struct datapath *dp;
 
    WARN_ON_ONCE(!rcu_read_lock_held() && !lockdep_ovsl_is_held());
    rcu_read_lock();
    dp = get_dp_rcu(net, dp_ifindex);
    rcu_read_unlock();
 
    return dp;
}
 
/* Must be called with rcu_read_lock or ovs_mutex. */
const char *ovs_dp_name(const struct datapath *dp)
{
    struct vport *vport = ovs_vport_ovsl_rcu(dp, OVSP_LOCAL);
    return ovs_vport_name(vport);
}
 
static int get_dpifindex(const struct datapath *dp)
{
    struct vport *local;
    int ifindex;
 
    rcu_read_lock();
 
    local = ovs_vport_rcu(dp, OVSP_LOCAL);
    if (local)
        ifindex = local->dev->ifindex;
    else
        ifindex = 0;
 
    rcu_read_unlock();
 
    return ifindex;
}
 
static void destroy_dp_rcu(struct rcu_head *rcu)
{
    struct datapath *dp = container_of(rcu, struct datapath, rcu);
 
    ovs_flow_tbl_destroy(&dp->table);
    free_percpu(dp->stats_percpu);
    kfree(dp->ports);
    kfree(dp);
}
 
static struct hlist_head *vport_hash_bucket(const struct datapath *dp,
                        u16 port_no)
{
    return &dp->ports[port_no & (DP_VPORT_HASH_BUCKETS - 1)];
}
 
/* Called with ovs_mutex or RCU read lock. */
struct vport *ovs_lookup_vport(const struct datapath *dp, u16 port_no)
{
    struct vport *vport;
    struct hlist_head *head;
 
    head = vport_hash_bucket(dp, port_no);
    hlist_for_each_entry_rcu(vport, head, dp_hash_node) {
        if (vport->port_no == port_no)
            return vport;
    }
    return NULL;
}
 
/* Called with ovs_mutex. */
static struct vport *new_vport(const struct vport_parms *parms)
{
    struct vport *vport;
 
    vport = ovs_vport_add(parms);
    if (!IS_ERR(vport)) {
        struct datapath *dp = parms->dp;
        struct hlist_head *head = vport_hash_bucket(dp, vport->port_no);
 
        hlist_add_head_rcu(&vport->dp_hash_node, head);
    }
    return vport;
}
 
void ovs_dp_detach_port(struct vport *p)
{
    ASSERT_OVSL();
 
    /* First drop references to device. */
    hlist_del_rcu(&p->dp_hash_node);
 
    /* Then destroy it. */
    ovs_vport_del(p);
 
}
/****************************************keqiang's logic**************************************/
/* keqiang's logic */
enum {
    OVS_PKT_IN = 1U, //packets come to the host
    OVS_PKT_OUT = 3U, //packets go to the network (switch), see "ip_summed_*"
};
 
enum {
    OVS_ECN_MASK = 3U,
    OVS_ECN_ZERO = 0U,
    OVS_ECN_ONE = 1U,
    OVS_ECN_FAKE = 4U, //set the second highest bit of 3 reserved bits in TCP header
    OVS_ECN_FAKE_CLEAR = 11U, // 1011 (binary) = 11 (decimal)
    OVS_ACK_SPEC_SET = 8U, //set the highest bit of 3 reserved bits in TCP header
    OVS_ACK_PACK_SET = 2U, //set the third highest bit of 3 reserved bits in TCP header
    OVS_ACK_PACK_CLEAR = 13U, // 1101 (binary) = 13 (decimal)
};
 
/*the following functions are used to copy part of an SKB, it is used to generate the ECN info ACKs
we do not want to copy data of an skb, we just need the necessary MAC IP AND TCP headers
This optimization is useful when DATA packets carry ACKs, for pure ACKs, it copies the whole ACK
*/
 
//need several functions from  <linux/net/core/skbuff.c>
static inline int skb_alloc_rx_flag(const struct sk_buff *skb)
{
    if (skb_pfmemalloc(skb))
        return SKB_ALLOC_RX;
    return 0;
}
 
static void __copy_skb_header(struct sk_buff *new, const struct sk_buff *old)
{
    new->tstamp             = old->tstamp;
    new->dev                = old->dev;
    new->transport_header   = old->transport_header;
    new->network_header     = old->network_header;
    new->mac_header         = old->mac_header;
    new->inner_protocol     = old->inner_protocol;
    new->inner_transport_header = old->inner_transport_header;
    new->inner_network_header = old->inner_network_header;
    new->inner_mac_header = old->inner_mac_header;
    skb_dst_copy(new, old);
    skb_copy_hash(new, old);
    new->ooo_okay           = old->ooo_okay;
    new->no_fcs             = old->no_fcs;
    new->encapsulation      = old->encapsulation;
    memcpy(new->cb, old->cb, sizeof(old->cb));
    new->csum               = old->csum;
    //new->local_df           = old->local_df;
    new->pkt_type           = old->pkt_type;
    new->ip_summed          = old->ip_summed;
    skb_copy_queue_mapping(new, old);
    new->priority           = old->priority;
    new->pfmemalloc         = old->pfmemalloc;
    new->protocol           = old->protocol;
    new->mark               = old->mark;
    new->skb_iif            = old->skb_iif;
    //__nf_copy(new, old);
 
    new->vlan_proto         = old->vlan_proto;
    new->vlan_tci           = old->vlan_tci;
 
    skb_copy_secmark(new, old);
 
}
 
static void copy_skb_header(struct sk_buff *new, const struct sk_buff *old)
{
    __copy_skb_header(new, old);
 
    skb_shinfo(new)->gso_size = skb_shinfo(old)->gso_size;
    skb_shinfo(new)->gso_segs = skb_shinfo(old)->gso_segs;
    skb_shinfo(new)->gso_type = skb_shinfo(old)->gso_type;
}
 
static struct sk_buff *skb_copy_ecn_ack(const struct sk_buff *skb, gfp_t gfp_mask, u32 nondata_len) {
    int headerlen = skb_headroom(skb);
    unsigned int size = skb_end_offset(skb) + 0;
    struct sk_buff *n = __alloc_skb(size, gfp_mask,
                                    skb_alloc_rx_flag(skb), NUMA_NO_NODE);
    if (!n)
        return NULL;
    /* Set the data pointer */
    skb_reserve(n, headerlen);
    /* Set the tail pointer and length */
    skb_put(n, nondata_len);
    if (skb_copy_bits(skb, -headerlen, n->head, headerlen + nondata_len))
        BUG();
    copy_skb_header(n, skb);
    return n;
}
 
/*help function get a u64 key for a TCP flow */
static u64 get_tcp_key64(u32 ip1, u32 ip2, u16 tp1, u16 tp2) {
    u64 key = 0;
    u64 part1, part2, part3, part4;
 
    part1 = ip1 & ((1 << 16) - 1); // get the lower 16 bits of u32
    part1 = part1 << 48; //the highest 16 bits of the result
 
    part2 = ip2 & ((1 << 16) - 1);
    part2 = part2 << 32;
 
    part3 = tp1 << 16;
 
    part4 = tp2;
 
    key = part1 + part2 + part3 + part4;
    return key;
 
}
 
/*helper function, determine the direction of the traffic (packet), i.e., go to the net or come to the host?*/
static bool ovs_packet_to_net(struct sk_buff *skb) {
    if (strncmp(skb->dev->name, BRIDGE_NAME, 2) == 0)
        return 1;
    else
        return 0;
}
 
/*extract window scaling factor, Normally only called on SYN and SYNACK packets.
see http://packetlife.net/blog/2010/aug/4/tcp-windows-and-window-scaling/
TODO: we do not consider the case that one side uses scaling while the other does
not support it (in this case, both should not use scaling factor).
This should be handled in production code
*/
 
static u8 ovs_tcp_parse_options(const struct sk_buff *skb) {
    u8 snd_wscale = 0;
 
    const unsigned char *ptr;
    const struct tcphdr *th = tcp_hdr(skb);
    int length = (th->doff * 4) - sizeof(struct tcphdr);
 
    ptr = (const unsigned char *)(th + 1);
 
    while (length > 0) {
        int opcode = *ptr++;
        int opsize;
        switch (opcode) {
        case TCPOPT_EOL:
            return 0;
        case TCPOPT_NOP:        /* Ref: RFC 793 section 3.1 */
            length--;
            continue;
        default:
            opsize = *ptr++;
            if (opsize < 2) /* "silly options" */
                return 0;
            if (opsize > length)
                return 0; /* don't parse partial options */
            switch (opcode) {
            case TCPOPT_WINDOW:
                if (opsize == TCPOLEN_WINDOW && th->syn) {
                    snd_wscale = *(__u8 *)ptr;
                    if (snd_wscale > 14) {
                        printk("Illegal window scaling: %u\n", snd_wscale);
                        snd_wscale = 14;
                    }
                }
                break;
            default:
                break;
            }
            ptr += opsize - 2;
            length -= opsize;
        }
    }
    return snd_wscale;
}
 
/*keqiang's congestion control logic */
 
static void ovs_tcp_slow_start(struct rcv_ack * rack, u32 acked) {
//”acked” means the number of bytes acked by an ACK
    u32 rwnd = rack->rwnd + acked;
 
    if (rwnd > rack->rwnd_ssthresh)
        rwnd = rack->rwnd_ssthresh + RWND_STEP;
 
    rack->rwnd = min(rwnd, rack->rwnd_clamp);
}
 
/* In theory this is tp->snd_cwnd += 1 / tp->snd_cwnd (or alternative w) */
/* In theory this is tp->rwnd += MSS / tp->rwnd (or alternative w) */
static void ovs_tcp_cong_avoid_ai(struct rcv_ack * rack, u32 w, u32 acked) {
    if (rack->snd_rwnd_cnt >= w) {
        if (rack->rwnd < rack->rwnd_clamp)
            rack->rwnd += RWND_STEP;
        rack->snd_rwnd_cnt = 0;
    } else {
        rack->snd_rwnd_cnt += acked;
    }
 
    rack->rwnd = min(rack->rwnd, rack->rwnd_clamp);
}
 
/*
* TCP Reno congestion control
* This is special case used for fallback as well.
*/
/* This is Jacobson's slow start and congestion avoidance.
* SIGCOMM '88, p. 328.
*/
static void ovs_tcp_reno_cong_avoid(struct rcv_ack * rack, u32 acked) {
    /* In "safe" area, increase. */
    if (rack->rwnd <= rack->rwnd_ssthresh)
        ovs_tcp_slow_start(rack, acked);
    /* In dangerous area, increase slowly. */
    else
        ovs_tcp_cong_avoid_ai(rack, rack->rwnd, acked);
}
 
/* Slow start threshold is half the congestion window (min 2) */
static u32 ovs_tcp_reno_ssthresh(struct rcv_ack* rack) {
    return max(rack->rwnd >> 1U, 2U);
}
 
static void ovs_dctcp_reset(struct rcv_ack * rack) {
    rack->next_seq = rack->snd_nxt;
 
    rack->ecn_bytes = 0;
    rack->total_bytes = 0;
 
    rack->reduced_this_win = false;
    rack->loss_detected_this_win = false;
}
 
static u32 ovs_dctcp_ssthresh(struct rcv_ack * rack) {
    //cwnd = cwnd* (1 - alpha/2)
    //rwnd = rwnd* (1 - alpha/2)
    return max(rack->rwnd - ((rack->rwnd * rack->alpha) >> 11U), RWND_MIN);
 
}
 
static void ovs_dctcp_update_alpha(struct rcv_ack * rack) {
    /* Expired RTT */
    /* update alpha once per window of data, roughly once per RTT
     * rack->total_bytes should be larger than 0
     */
    if (!before(rack->snd_una, rack->next_seq)) {
 
        /*printk(KERN_INFO "ecn_bytes:%u, total_bytes:%u, alpha:%u, snd_una:%u, next_seq:%u, snd_nxt:%u \n",
                *         rack->ecn_bytes, rack->total_bytes, rack->alpha, rack->snd_una, rack->next_seq, rack->snd_nxt);
        */
        /* keep alpha the same if total_bytes is zero */
        if (rack->total_bytes > 0) {
 
            if (rack->ecn_bytes > rack->total_bytes)
                rack->ecn_bytes = rack->total_bytes;
 
            /* alpha = (1 - g) * alpha + g * F */
            rack->alpha = rack->alpha -
                          (rack->alpha >> dctcp_shift_g) +
                          (rack->ecn_bytes << (10U - dctcp_shift_g)) /
                          rack->total_bytes;
            if (rack->alpha > DCTCP_MAX_ALPHA)
                rack->alpha = DCTCP_MAX_ALPHA;
        }
 
        ovs_dctcp_reset(rack);
        /*printk(KERN_INFO "ecn_bytes:%u, total_bytes:%u, alpha:%u\n",
                        rack->ecn_bytes, rack->total_bytes, rack->alpha);
        */
 
    }
}
 
static bool ovs_may_raise_rwnd(struct rcv_ack * rack) {
    /*return ture if there is no ECN feedback received in this window yet &&
    * no packet loss is detected in this window yet
    */
    if (rack->ecn_bytes > 0 || rack->loss_detected_this_win == true)
        return false;
    else
        return true;
}
 
static bool ovs_may_reduce_rwnd(struct rcv_ack * rack) {
    if (rack->reduced_this_win == false)
        return true;
    else
        return false;
}
static int ovs_pack_ecn_info(struct sk_buff * skb, u32 ecn_bytes, u32 total_bytes) {
    struct iphdr *nh;
    struct tcphdr * tcp;
 
    u16 header_len;
    u16 old_total_len;
    u16 old_tcp_len;
 
    u8 ECN_INFO_LEN = 8;
    /*the caller makes sure this is a TCP packet*/
    nh = ip_hdr(skb);
    tcp = tcp_hdr(skb);
 
    header_len = skb->mac_len + (nh->ihl << 2) + 20;
    old_total_len = ntohs(nh->tot_len);
    old_tcp_len = tcp->doff << 2;
 
    if (skb_cow_head(skb, ECN_INFO_LEN) < 0)
        return -ENOMEM;
 
    skb_push(skb, ECN_INFO_LEN);
    memmove(skb_mac_header(skb) - ECN_INFO_LEN, skb_mac_header(skb), header_len);
    skb_reset_mac_header(skb);
    skb_set_network_header(skb, skb->mac_len);
    skb_set_transport_header(skb, skb->mac_len + (ip_hdr(skb)->ihl << 2));
 
    ecn_bytes = htonl(ecn_bytes);
    total_bytes = htonl(total_bytes);
    memcpy(skb_mac_header(skb) + header_len, &ecn_bytes, (ECN_INFO_LEN >> 1));
    memcpy(skb_mac_header(skb) + header_len + (ECN_INFO_LEN >> 1), &total_bytes, (ECN_INFO_LEN >> 1));
    /*we believe that the NIC will re-calculate checksums for us*/
    nh = ip_hdr(skb);
    tcp = tcp_hdr(skb);
 
    nh->tot_len = htons(old_total_len + ECN_INFO_LEN);
    tcp->doff = ((old_tcp_len + ECN_INFO_LEN) >> 2);
    /*printk("before maring pack, tcp->src:%u, tcp->dst:%u, tcp->res1:%u\n",
    * ntohs(tcp->source), ntohs(tcp->dest), tcp->res1);
    */
    tcp->res1 |= OVS_ACK_PACK_SET;
    /*printk("before maring pack, tcp->src:%u, tcp->dst:%u, tcp->res1:%u\n",
    * ntohs(tcp->source), ntohs(tcp->dest), tcp->res1);
    */
    return 0;
}
 
/*note, after this unpack function, tcp and ip points should be refreshed*/
static int ovs_unpack_ecn_info(struct sk_buff* skb, u32 * this_ecn, u32 * this_total) {
    struct iphdr *nh;
    struct tcphdr * tcp;
 
    u16 header_len;
    u16 old_total_len;
    u16 old_tcp_len;
    int err;
 
    u8 ECN_INFO_LEN = 8;
    /*the caller makes sure this is a TCP packet*/
    nh = ip_hdr(skb);
    tcp = tcp_hdr(skb);
 
    header_len = skb->mac_len + (nh->ihl << 2) + 20;
    old_total_len = ntohs(nh->tot_len);
    old_tcp_len = tcp->doff << 2;
 
    err = skb_ensure_writable(skb, header_len);
    if (unlikely(err))
        return err;
 
    memset(this_ecn, 0, sizeof(*this_ecn));
    memset(this_total, 0, sizeof(*this_total));
 
    memcpy(this_ecn, skb_mac_header(skb) + header_len, (ECN_INFO_LEN >> 1));
    memcpy(this_total, skb_mac_header(skb) + header_len + (ECN_INFO_LEN >> 1), (ECN_INFO_LEN >> 1));
 
    *this_ecn = ntohl(*this_ecn);
    *this_total = ntohl(*this_total);
 
    //printk("we are unpack (check ip_summed):%u, ip_fast_csum:%u\n", skb->ip_summed, ip_fast_csum((u8 *)nh, nh->ihl));
    skb_postpull_rcsum(skb, skb_mac_header(skb) + header_len, ECN_INFO_LEN);
 
    memmove(skb_mac_header(skb) + ECN_INFO_LEN, skb_mac_header(skb), header_len);
    __skb_pull(skb, ECN_INFO_LEN);
    skb_reset_mac_header(skb);
    skb_set_network_header(skb, skb->mac_len);
    skb_set_transport_header(skb, skb->mac_len + (ip_hdr(skb)->ihl << 2));
 
    nh = ip_hdr(skb);
    tcp = tcp_hdr(skb);
 
    /*printk("we are unpack (before), tcp->src:%u, tcp->dst:%u, tcp->seq:%u, tcp->ack_seq:%u, tcp->res1:%u, nh->tot_len:%u, tcp->doff:%u, this_ecn:%u, this_total:%u, skb->ip_summed:%u\n",
        * ntohs(tcp->source), ntohs(tcp->dest), ntohl(tcp->seq), ntohl(tcp->ack_seq), tcp->res1, ntohs(nh->tot_len), tcp->doff, *this_ecn, *this_total, skb->ip_summed);
    */
    nh->tot_len = htons(old_total_len - ECN_INFO_LEN);
    csum_replace2(&nh->check, htons(old_total_len), nh->tot_len);
 
    tcp->doff = ((old_tcp_len - ECN_INFO_LEN) >> 2);
    tcp->res1 &= OVS_ACK_PACK_CLEAR;
 
    /*printk("we are unpack (after), tcp->src:%u, tcp->dst:%u, tcp->seq:%u, tcp->ack_seq:%u, tcp->res1:%u, nh->tot_len:%u, tcp->doff:%u, this_ecn:%u, this_total:%u, skb->ip_summed:%u, ip_fast_csum:%u\n",
        * ntohs(tcp->source), ntohs(tcp->dest), ntohl(tcp->seq), ntohl(tcp->ack_seq), tcp->res1, ntohs(nh->tot_len), tcp->doff, *this_ecn, *this_total, skb->ip_summed, ip_fast_csum((u8 *)nh, nh->ihl));
    */
    return 0;
}
/***********************************************************************************/
 static bool ovs_packet_to_net(struct sk_buff *skb) {
        if (strncmp(skb->dev->name, BRIDGE_NAME, 2) == 0)
                return 1;
        else
                return 0;
}
/* Must be called with rcu_read_lock. */
void ovs_dp_process_packet(struct sk_buff *skb, struct sw_flow_key *key)
{
    const struct vport *p = OVS_CB(skb)->input_vport;
    struct datapath *dp = p->dp;
    struct sw_flow *flow;
    struct sw_flow_actions *sf_acts;
    struct dp_stats_percpu *stats;
    u64 *stats_counter;
    u32 n_mask_hit;
 
    stats = this_cpu_ptr(dp->stats_percpu);
    /**********************************qiwei's logic*********************/
    printk(KERN_ALERT "qiwei_ovs_queue_len==========%d\n",queuelength);
    /********************************************************************/
    /*******************************************************************************/
    printk(KERN_ALERT "the ac/dctcp logic");
    struct iphdr *nh;
    struct tcphdr * tcp;
    struct sk_buff * ecn_ack = NULL;
    /*keqiang's marking, speically for SYN*/
    /*here we assume that user won't use TOS field, in other words,
    it is always 0. For producation code, this can be solved by adding
    a few more lines of code
    */
    if (ntohs(skb->protocol) == ETH_P_IP) { //this is an IP packet
        nh = ip_hdr(skb);
        printk(KERN_ALERT "this is an IP packet\n");
        if (nh->protocol == IPPROTO_TCP) { //this is an TCP packet
            tcp = tcp_hdr(skb);
            printk(KERN_ALERT "this is an TCP packet\n");
            if ( ovs_packet_to_net(skb) && (nh->tos & OVS_ECN_MASK) == OVS_ECN_ZERO) {//get the last 2 bits 
                if (queuelength>ecn_mark_threshold){//queue_length_ovsrflow
                    printk(KERN_ALERT "ovs_queue_len>ecn_mark_threshold,where ecn_mark_threshold=%d\n",ecn_mark_threshold);
                    ipv4_change_dsfield(nh, 0, OVS_ECN_MASK);   //mark IP ecn notify congestion
                    printk(KERN_ALERT "ipv4_change_dsfield\n");
                    //set second highest bit in TCP reserve fields to 1
                    //tcp->res1 |= OVS_ECN_FAKE;
                    }
                }
        }//it was an TCP packet
    }//it was an IP packet
    /*keqiang's logic,
    TODO: we assume that TCP three-way hand-shake is always successful
    while in production code, this should be taken care, broken handshake waste entries in the hashtables
    this piece of logic should be before "upcall" because we need to examine SYN
    */
 
 
    /*******************************************************************************/
  
    /**************************************keqiang's logic-one***********************/
    /*keqiang's marking, speically for SYN*/
    /*here we assume that user won't use TOS field, in other words,
    it is always 0. For producation code, this can be solved by adding
    a few more lines of code
    */
    printk(KERN_ALERT "now in keqiang's logic");
 
    if (ntohs(skb->protocol) == ETH_P_IP) { //this is an IP packet
        nh = ip_hdr(skb);
        if (nh->protocol == IPPROTO_TCP) { //this is an TCP packet
            tcp = tcp_hdr(skb);
            if (tcp->syn && ovs_packet_to_net(skb)) {// outgoing to the NIC
                if ( (nh->tos & OVS_ECN_MASK) == OVS_ECN_ZERO) {//get the last 2 bits
                    ipv4_change_dsfield(nh, 0, OVS_ECN_ONE);
                    //set second highest bit in TCP reserve fields to 1
                    tcp->res1 |= OVS_ECN_FAKE;
                }
 
            }
        }//it was an TCP packet
    }//it was an IP packet
    /*keqiang's logic,
    TODO: we assume that TCP three-way hand-shake is always successful
    while in production code, this should be taken care, broken handshake waste entries in the hashtables
    this piece of logic should be before "upcall" because we need to examine SYN
    */
 
    if (ntohs(skb->protocol) == ETH_P_IP) { //this is an IP packet
        nh = ip_hdr(skb);
        if (nh->protocol == IPPROTO_TCP) { //this is an TCP packet
            u32 srcip;
            u32 dstip;
            u16 srcport;
            u16 dstport;
            u64 tcp_key64;
 
            tcp = tcp_hdr(skb);
 
            srcip = ntohl(nh->saddr);
            dstip = ntohl(nh->daddr);
            srcport = ntohs(tcp->source);
            dstport = ntohs(tcp->dest);
 
            //outgoing SYN or SYN/ACK or FIN, insert/delete entry in rcv_ack_hashtbl
            if (ovs_packet_to_net(skb)) {
                if (unlikely(tcp->syn)) {//insert an entry to rcv_ack_hashtbl
                    struct rcv_ack * new_entry = NULL;
                    //pay attention to the parameter order
                    tcp_key64 = get_tcp_key64(dstip, srcip, dstport, srcport);
 
                    rcu_read_lock();
                    new_entry = rcv_ack_hashtbl_lookup(tcp_key64);
                    rcu_read_unlock();
                    if (likely(!new_entry)) {
                        new_entry = kzalloc(sizeof(*new_entry), GFP_KERNEL);
                        new_entry->key = tcp_key64;
                        rcv_ack_hashtbl_insert(tcp_key64, new_entry);
                    }
 
                    new_entry->rwnd = RWND_INIT;
                    new_entry->rwnd_ssthresh = RWND_SSTHRESH_INIT;
                    new_entry->rwnd_clamp = RWND_CLAMP;
                    new_entry->alpha = DCTCP_ALPHA_INIT;
                    new_entry->snd_una = ntohl(tcp->seq);
                    new_entry->snd_nxt = ntohl(tcp->seq) + 1;//SYN takes 1 byte
                    new_entry->next_seq = new_entry->snd_nxt;
                    //keqiang, Sep 19, 2015, Window Scaling factor logic was wrong
                    //new_entry->snd_wscale = ovs_tcp_parse_options(skb);
                    new_entry->snd_rwnd_cnt = 0;
                    new_entry->reduced_this_win = false;
                    new_entry->loss_detected_this_win = false;
                    new_entry->prior_real_rcv_wnd = ntohs(tcp->window);
                    new_entry->dupack_cnt = 0;
                    new_entry->ecn_bytes = 0;
                    new_entry->total_bytes = 0;
                    spin_lock_init(&new_entry->lock);
 
                    /*printk(KERN_INFO "rcv_ack_hashtbl new entry inserted. %d --> %d, snd_rwnd_cnt:%u, rwnd:%u \n",
                                srcport, dstport,
                                new_entry->snd_rwnd_cnt, new_entry->rwnd);
                    */
                }
                /*TODO: we may also need to consider RST */
                if (unlikely(tcp->fin)) {
                    struct rcv_ack * new_entry = NULL;
                    //pay attention to the parameter order
                    tcp_key64 = get_tcp_key64(dstip, srcip, dstport, srcport);
 
                    rcu_read_lock();
                    new_entry = rcv_ack_hashtbl_lookup(tcp_key64);
                    rcu_read_unlock();
                    if (likely(new_entry)) {
                        rcv_ack_hashtbl_delete(new_entry);
                        /*printk(KERN_INFO "rcv_ack_hashtbl new entry deleted. %d --> %d\n",
                            srcport, dstport);
                        */
                    }
 
                }
 
            }//it was outgoing
            else { //incoming traffic
                if (unlikely(tcp->syn)) {
                    struct rcv_data * new_entry = NULL;
                    struct rcv_ack * ack_entry2 = NULL;
                    //pay attention to the parameter order
                    tcp_key64 = get_tcp_key64(srcip, dstip, srcport, dstport);
 
                    rcu_read_lock();
                    new_entry = rcv_data_hashtbl_lookup(tcp_key64);
                    rcu_read_unlock();
                    if (likely(!new_entry)) {
                        new_entry = kzalloc(sizeof(*new_entry), GFP_KERNEL);
 
                        new_entry->key = tcp_key64;
                        new_entry->ecn_bytes_per_ack = 0;
                        new_entry->total_bytes_per_ack = 0;
                        spin_lock_init(&new_entry->lock);
 
                        rcv_data_hashtbl_insert(tcp_key64, new_entry);
                        /*printk(KERN_INFO "rcv_data_hashtbl new entry inserted.
                                %d --> %d\n", srcport, dstport);
                        */
                    }
 
                    //correct window scaling factor parser, see RFC 1323
                    /* see also http://www.networksorcery.com/enp/protocol/tcp/option003.htm */
                    tcp_key64 = get_tcp_key64(srcip, dstip, srcport, dstport);
                    rcu_read_lock();
                    ack_entry2 = rcv_ack_hashtbl_lookup(tcp_key64);
                    rcu_read_unlock();
                    if (!ack_entry2) {
                        ack_entry2 = kzalloc(sizeof(*ack_entry2), GFP_KERNEL);
                        ack_entry2->key = tcp_key64;
                        rcv_ack_hashtbl_insert(tcp_key64, ack_entry2);
                    }
                    ack_entry2->snd_wscale = ovs_tcp_parse_options(skb);
                    /*printk("incoming SYN, window scaling factor is %u, src %u-->dest %u, MSS is %u\n",
                        ack_entry2->snd_wscale, ntohs(tcp->source), ntohs(tcp->dest), MSS);
                    */
                }
                /*TODO: we may also need to consider RST */
                if (unlikely(tcp->fin)) {
                    struct rcv_data * new_entry = NULL;
                    //pay attention to the parameter order
                    tcp_key64 = get_tcp_key64(srcip, dstip, srcport, dstport);
 
                    rcu_read_lock();
                    new_entry = rcv_data_hashtbl_lookup(tcp_key64);
                    rcu_read_unlock();
                    if (likely(new_entry)) {
                        rcv_data_hashtbl_delete(new_entry);
                        /*printk(KERN_INFO "rcv_data_hashtbl new entry deleted. %d --> %d\n",
                                srcport, dstport);
                        */
                    }
 
                    /*else {
                        printk(KERN_INFO "rcv_data_hashtbl try to delete but entry not found.
                            %d --> %d\n", srcport, dstport);
                    }*/
                }
            }//incoming to the host traffic
        }//is TCP packet
    }//is IP packet
    /*********************************************************************************/
    /* Look up flow. */
    flow = ovs_flow_tbl_lookup_stats(&dp->table, key, skb_get_hash(skb),
                     &n_mask_hit);
    if (unlikely(!flow)) {
        struct dp_upcall_info upcall;
        int error;
 
        memset(&upcall, 0, sizeof(upcall));
        upcall.cmd = OVS_PACKET_CMD_MISS;
        upcall.portid = ovs_vport_find_upcall_portid(p, skb);
        upcall.mru = OVS_CB(skb)->mru;
        error = ovs_dp_upcall(dp, skb, key, &upcall, 0);
        if (unlikely(error))
            kfree_skb(skb);
        else
            consume_skb(skb);
        stats_counter = &stats->n_missed;
        goto out;
    }
    /**********************************keqiang's logic********************************/
    /*keqiang's logic, update rcv_data and rcv_ack table entries,
    this piece of logic is better to implement after upcall -
    SYN never carry data, so we won't update any bytes
    TODO: it is better to disable OVS's TCP flag matching feature
    */
    if (ntohs(skb->protocol) == ETH_P_IP) { //this is an IP packet
        nh = ip_hdr(skb);
        if (nh->protocol == IPPROTO_TCP) { //this is an TCP packet
            u32 srcip;
            u32 dstip;
            u16 srcport;
            u16 dstport;
            u64 tcp_key64;
 
            tcp = tcp_hdr(skb);
 
            srcip = ntohl(nh->saddr);
            dstip = ntohl(nh->daddr);
            srcport = ntohs(tcp->source);
            dstport = ntohs(tcp->dest);
 
            if (likely(!(tcp->syn || tcp->fin))) {//do not process SYN, SYN/ACK and FIN here
                //process outgoing traffic
                if (ovs_packet_to_net(skb)) {
                    int tcp_data_len;
                    u32 end_seq;
                    struct rcv_ack * the_entry = NULL;
 
                    //first task, update "snd_nxt" in "rcv_ack"
                    tcp_data_len = ntohs(nh->tot_len) - (nh->ihl << 2) - (tcp->doff << 2);
                    end_seq = ntohl(tcp->seq) + tcp_data_len;
 
                    tcp_key64 = get_tcp_key64(dstip, srcip, dstport, srcport);
 
                    rcu_read_lock();
                    the_entry = rcv_ack_hashtbl_lookup(tcp_key64);
                    if (likely(the_entry)) {
                        spin_lock(&the_entry->lock);
                        if (tcp_data_len > 0 &&
                                after(end_seq, the_entry->snd_nxt)) {
                            the_entry->snd_nxt = end_seq;
                            /*printk(KERN_INFO "tcp_data_len:%d, snd_nxt updated: %u (%d --> %d)\n",
                                tcp_data_len, end_seq, srcport, dstport);
                            */
 
                        }
                        spin_unlock(&the_entry->lock);
                    }
                    rcu_read_unlock();
 
                    /*second task, may 1)pack ecn info into a RACK or 2)generate faked ECN ACK based on "rcv_data"
                    * for small packet (smaller than MSS - OVS_PACK_HEADROOM), we pack the "ecn_bytes" and "total_bytes"
                    * for large segment (for TSO), we generated faked ACK
                    */
                    if (tcp->ack) {
                        u32 ecn_bytes_this_ack = 0;
                        u32 total_bytes_this_ack = 0;
 
                        struct rcv_data * byte_entry = NULL;
                        //tcp_key64 calculated above
                        rcu_read_lock();
                        byte_entry = rcv_data_hashtbl_lookup(tcp_key64);
                        if (likely(byte_entry)) {
                            spin_lock(&byte_entry->lock);
                            if (byte_entry->total_bytes_per_ack > 0) {
                                ecn_bytes_this_ack = byte_entry->ecn_bytes_per_ack;
                                total_bytes_this_ack = byte_entry->total_bytes_per_ack;
 
                                byte_entry->ecn_bytes_per_ack = 0;
                                byte_entry->total_bytes_per_ack = 0;
                            }
                            spin_unlock(&byte_entry->lock);
                        }
                        rcu_read_unlock();
 
                        if (total_bytes_this_ack > 0) {
                            //if (false) {
                            if (tcp_data_len < MSS - OVS_PACK_HEADROOM) { //pack ECN info
                                int err;
                                err = ovs_pack_ecn_info(skb, ecn_bytes_this_ack, total_bytes_this_ack);
                                if (err)
                                    printk(KERN_INFO "warning, packing packets error\n");
                            } //end of packing
                            else {//the ACK is big, generated FACK
                                struct tcphdr * ecn_ack_tcp = NULL;
                                struct iphdr *ecn_ack_nh = NULL;
 
                                u32 copy_len = skb->mac_len +
                                               (nh->ihl << 2) + (tcp->doff << 2);
                                //ecn_ack = skb_copy(skb, GFP_ATOMIC);
                                ecn_ack = skb_copy_ecn_ack(skb, GFP_ATOMIC, copy_len);
                                if (ecn_ack != NULL) {
                                    ecn_ack_tcp = tcp_hdr(ecn_ack);
                                    ecn_ack_nh = ip_hdr(ecn_ack);
 
                                    //modify the IP total len
                                    ecn_ack_nh->tot_len =
                                        htons((nh->ihl << 2) + (tcp->doff << 2));
                                    //let tcp seq carry ecn_bytes_per_ack
                                    //set tcp ack_seq to 0
                                    ecn_ack_tcp->seq = htonl(ecn_bytes_this_ack);
                                    ecn_ack_tcp->ack_seq = htonl(total_bytes_this_ack);
                                    //set highest bit in TCP reserve fields to 1
                                    ecn_ack_tcp->res1 |= OVS_ACK_SPEC_SET;
 
                                    //critical!!!!, set the ECT for faked ECN info ACKs, otherwise, they can be dropped
                                    if ( (ecn_ack_nh->tos & OVS_ECN_MASK) == OVS_ECN_ZERO) //get the last 2 bits
                                        ipv4_change_dsfield(ecn_ack_nh, 0, OVS_ECN_ONE);
                                    //test purpose
                                    /*printk("generated an faked ECN ACK, ecn_bytes: %u,
                                        total_bytes:%u\n",
                                        ecn_bytes_this_ack, total_bytes_this_ack);
                                    */
                                }
                            }// end of generating FACK
                        }//end of if (total_bytes > 0)
                    }
 
                }//end processing outgoing skb
                else {// processing incoming (to the end host) skbs
                    int tcp_data_len;
                    struct rcv_data * the_entry = NULL;
 
                    //first task, update "*_bytes_per_ack" in "rcv_data"
                    tcp_data_len = ntohs(nh->tot_len) - (nh->ihl << 2) - (tcp->doff << 2);
                    tcp_key64 = get_tcp_key64(srcip, dstip, srcport, dstport);
 
                    //test purpose
                    /*
                    if (tcp_data_len > 0)
                                            printk("this skb length is: %u\n", tcp_data_len);
                    */
                    if (tcp_data_len > 0) {
                        rcu_read_lock();
                        the_entry = rcv_data_hashtbl_lookup(tcp_key64);
                        if (likely(the_entry)) {
                            spin_lock(&the_entry->lock);
                            the_entry->total_bytes_per_ack += tcp_data_len;
                            if ( (nh->tos & OVS_ECN_MASK) == OVS_ECN_MASK)
                                the_entry->ecn_bytes_per_ack += tcp_data_len;
                            spin_unlock(&the_entry->lock);
                        }
                        rcu_read_unlock();
                    }
                    //second, if it is an ACK,
                    //   i) update "snd_una" in "rcv_ack"
                    //  ii) run VJ congestion control algorithm and DCTCP logic
                    // iii) drop the ECN info ACKs, no need to pushup
                    if (tcp->ack) {
                        struct rcv_ack * ack_entry = NULL;
                        bool is_pack = false;
                        bool is_fack = false;
                        bool is_rack = false;
                        u32 this_ecn_bytes = 0;
                        u32 this_total_bytes = 0;
                        u32 acked = 0;
                        //this ACK is a PACK?
                        if ( (tcp->res1 & OVS_ACK_PACK_SET) == OVS_ACK_PACK_SET) {
                            int err;
 
                            is_pack = true;
                            err = ovs_unpack_ecn_info(skb, &this_ecn_bytes, &this_total_bytes);
                            if (err)
                                printk(KERN_INFO "warning, unpack packet error\n");
 
                            nh = ip_hdr(skb);
                            tcp = tcp_hdr(skb);
                        }
                        //this ACK is a FACK?
                        else if ( (tcp->res1 & OVS_ACK_SPEC_SET) == OVS_ACK_SPEC_SET) {
                            is_fack = true;
 
                            this_ecn_bytes = ntohl(tcp->seq);
                            this_total_bytes = ntohl(tcp->ack_seq);
                        }
                        else {
                            is_rack = true;
                        }
 
                        rcu_read_lock();
                        ack_entry = rcv_ack_hashtbl_lookup(tcp_key64);
                        if (likely(ack_entry)) {
                            spin_lock(&ack_entry->lock);
                            if (is_pack || is_fack) {
                                ack_entry->ecn_bytes += this_ecn_bytes;
                                ack_entry->total_bytes += this_total_bytes;
                            }
                            if (is_pack || is_rack) {
                                //snd_una means first byte we want an ack for
                                if (before(ntohl(tcp->ack_seq), ack_entry->snd_una))
                                    printk("STALE ACKS FOUND!!!!!!!!!!!!!!!!!!!!\n");
                                acked = ntohl(tcp->ack_seq) - ack_entry->snd_una;
                                /*printk(KERN_INFO "real ack acked bytes:%u, (%d --> %d)\n",
                                        acked, srcport, dstport);
                                */
                                ack_entry->snd_una = ntohl(tcp->ack_seq);
                                /*theory behind: When a TCP sender receives 3 duplicate acknowledgements
                                * for the same piece of data (i.e. 4 ACKs for the same segment,
                                * which is not the most recently sent piece of data), then most likely,
                                * packet was lost in the netowrk. DUP-ACK is faster than RTO*/
                                if (acked == 0 && before(ack_entry->snd_una, ack_entry->snd_nxt) && (tcp_data_len == 0)
                                        && ack_entry->prior_real_rcv_wnd == ntohs(tcp->window))
                                    ack_entry->dupack_cnt ++;
                                ack_entry->prior_real_rcv_wnd = ntohs(tcp->window);
                                /*
                                                            *printk(KERN_INFO "ntohl(tcp->ack_seq): %u, snd_una: %u, dupack: %u (%d --> %d)\n",
                                *    ntohl(tcp->ack_seq), ack_entry->snd_una, ack_entry->dupack_cnt, srcport, dstport);
                                */
                                //DCTCP update alpha
                                ovs_dctcp_update_alpha(ack_entry);
                                //we may want to grow the window
                                if (acked > 0) {
                                    //TCP new Reno CC
                                    if (ovs_may_raise_rwnd(ack_entry))
                                        ovs_tcp_reno_cong_avoid(ack_entry, acked);
                                    /*
                                    printk(KERN_INFO "current RWND: %u, SSHTRESH:%u\n",
                                                ack_entry->rwnd, ack_entry->rwnd_ssthresh);
                                    */
                                }
                            }
                            //we may want to decrease the window
                            if (ack_entry->dupack_cnt >= 3 ||
                                    ( (is_fack || is_pack) && this_ecn_bytes > 0) ) {
                                /* if this_ecn_bytes is non-zero, that means
                                                             * congesiton happened (happens) in the network
                                                             * if we may need to reduce ssthersh and rwnd (if we have not done
                                                             * this in the current window yet)
                                                             *  i) ssthresh = dctcp_sshtresh()
                                                             * ii) rwnd = max(MSS, ssthresh - MSS)
                                                            */
                                if (ovs_may_reduce_rwnd(ack_entry)) {
                                    if (ack_entry->dupack_cnt >= 3) {
                                        ack_entry->alpha = DCTCP_MAX_ALPHA;
                                        ack_entry->dupack_cnt = 0;
                                        ack_entry->loss_detected_this_win = true;
                                    }
                                    ack_entry->rwnd_ssthresh = ovs_dctcp_ssthresh(ack_entry);
                                    //ack_entry->rwnd = max(MSS, (ack_entry->rwnd_ssthresh - MSS));
                                    ack_entry->rwnd = max(RWND_MIN, (ack_entry->rwnd_ssthresh));
                                    ack_entry->snd_rwnd_cnt = 0;
 
                                    ack_entry->reduced_this_win = true;
                                }
                            }
 
                            if (is_pack || is_rack) {
                                //rwnd enforce tput here
                                //printk("incoming ACK, win scale is %u\n", ack_entry->snd_wscale);
                                if ( (ntohs(tcp->window) << ack_entry->snd_wscale) > ack_entry->rwnd) {
                                    u16 enforce_win = ack_entry->rwnd >> ack_entry->snd_wscale;
                                    tcp->window = htons(enforce_win);
                                }
 
                            }
                            spin_unlock(&ack_entry->lock);
                        }
                        rcu_read_unlock();
 
                        //if it is faked ACK, drop it
                        if (is_fack) {
                            consume_skb(skb);
                            //printk("received an ECN ACK, tcp->res1 = %d, nh->tos = %d\n", tcp->res1, nh->tos);
                            stats_counter = &stats->n_hit;
                            goto out;
                        }
                    }
                }//end processing incoming SKB
 
            }//it was an TCP skb
        }//it was an IP skb
    }//end of updating "rcv_data", "rcv_ack"
 
 
 
    /*keqiang's logic: marking*/
    /*here we assume that user won't use TOS field, in other words,
    it is always 0. For producation code, this can be solved by adding
    a few more lines of code
    */
    if (ntohs(skb->protocol) == ETH_P_IP) { //this is an IP packet
        nh = ip_hdr(skb);
        if (nh->protocol == IPPROTO_TCP) { //this is an TCP packet
            tcp = tcp_hdr(skb);
            if (ovs_packet_to_net(skb)) {// outgoing to the NIC
                if ( (nh->tos & OVS_ECN_MASK) == OVS_ECN_ZERO) {//get the last 2 bits
                    ipv4_change_dsfield(nh, 0, OVS_ECN_ONE);
                    //set second highest bit in TCP reserve fields to 1
                    tcp->res1 |= OVS_ECN_FAKE;
                }
 
            }
            else {
                if ( (nh->tos & OVS_ECN_MASK) != OVS_ECN_ZERO &&
                        (tcp->res1 & OVS_ECN_FAKE) == OVS_ECN_FAKE ) {
 
                    ipv4_change_dsfield(nh, 0, OVS_ECN_ZERO);
                    //clear the second highest bit in TCP reserve fields to 0
                    tcp->res1 &= OVS_ECN_FAKE_CLEAR;
                }
                if (ECE_CLEAR && tcp->ece)
                    tcp->ece = 0;
            }
        }//it was an TCP packet
    }//it was an IP packet
    /***********************************************************************************************************************/
    ovs_flow_stats_update(flow, key->tp.flags, skb);
    sf_acts = rcu_dereference(flow->sf_acts);
    /************************************************keqiang's logic-third**************************************************/
    //keqiang's logic
    if (ecn_ack != NULL) //this shoud be executed first
        ovs_execute_actions(dp, ecn_ack, sf_acts, key);
    /***********************************************************************************************************************/
    ovs_execute_actions(dp, skb, sf_acts, key);
 
    stats_counter = &stats->n_hit;
 
out:
    /* Update datapath statistics. */
    u64_stats_update_begin(&stats->syncp);
    (*stats_counter)++;
    stats->n_mask_hit += n_mask_hit;
    u64_stats_update_end(&stats->syncp);
}
 
int ovs_dp_upcall(struct datapath *dp, struct sk_buff *skb,
          const struct sw_flow_key *key,
          const struct dp_upcall_info *upcall_info,
          uint32_t cutlen)
{
    struct dp_stats_percpu *stats;
    int err;
 
    if (upcall_info->portid == 0) {
        err = -ENOTCONN;
        goto err;
    }
 
    if (!skb_is_gso(skb))
        err = queue_userspace_packet(dp, skb, key, upcall_info, cutlen);
    else
        err = queue_gso_packets(dp, skb, key, upcall_info, cutlen);
    if (err)
        goto err;
 
    return 0;
 
err:
    stats = this_cpu_ptr(dp->stats_percpu);
 
    u64_stats_update_begin(&stats->syncp);
    stats->n_lost++;
    u64_stats_update_end(&stats->syncp);
 
    return err;
}
 
static int queue_gso_packets(struct datapath *dp, struct sk_buff *skb,
                 const struct sw_flow_key *key,
                 const struct dp_upcall_info *upcall_info,
                 uint32_t cutlen)
{
    unsigned short gso_type = skb_shinfo(skb)->gso_type;
    struct sw_flow_key later_key;
    struct sk_buff *segs, *nskb;
    struct ovs_skb_cb ovs_cb;
    int err;
 
    ovs_cb = *OVS_CB(skb);
    segs = __skb_gso_segment(skb, NETIF_F_SG, false);
    *OVS_CB(skb) = ovs_cb;
    if (IS_ERR(segs))
        return PTR_ERR(segs);
    if (segs == NULL)
        return -EINVAL;
 
    if (gso_type & SKB_GSO_UDP) {
        /* The initial flow key extracted by ovs_flow_key_extract()
         * in this case is for a first fragment, so we need to
         * properly mark later fragments.
         */
        later_key = *key;
        later_key.ip.frag = OVS_FRAG_TYPE_LATER;
    }
 
    /* Queue all of the segments. */
    skb = segs;
    do {
        *OVS_CB(skb) = ovs_cb;
        if (gso_type & SKB_GSO_UDP && skb != segs)
            key = &later_key;
 
        err = queue_userspace_packet(dp, skb, key, upcall_info, cutlen);
        if (err)
            break;
 
    } while ((skb = skb->next));
 
    /* Free all of the segments. */
    skb = segs;
    do {
        nskb = skb->next;
        if (err)
            kfree_skb(skb);
        else
            consume_skb(skb);
    } while ((skb = nskb));
    return err;
}
 
static size_t upcall_msg_size(const struct dp_upcall_info *upcall_info,
                  unsigned int hdrlen)
{
    size_t size = NLMSG_ALIGN(sizeof(struct ovs_header))
        + nla_total_size(hdrlen) /* OVS_PACKET_ATTR_PACKET */
        + nla_total_size(ovs_key_attr_size()) /* OVS_PACKET_ATTR_KEY */
        + nla_total_size(sizeof(unsigned int)); /* OVS_PACKET_ATTR_LEN */
 
    /* OVS_PACKET_ATTR_USERDATA */
    if (upcall_info->userdata)
        size += NLA_ALIGN(upcall_info->userdata->nla_len);
 
    /* OVS_PACKET_ATTR_EGRESS_TUN_KEY */
    if (upcall_info->egress_tun_info)
        size += nla_total_size(ovs_tun_key_attr_size());
 
    /* OVS_PACKET_ATTR_ACTIONS */
    if (upcall_info->actions_len)
        size += nla_total_size(upcall_info->actions_len);
 
    /* OVS_PACKET_ATTR_MRU */
    if (upcall_info->mru)
        size += nla_total_size(sizeof(upcall_info->mru));
 
    return size;
}
 
static void pad_packet(struct datapath *dp, struct sk_buff *skb)
{
    if (!(dp->user_features & OVS_DP_F_UNALIGNED)) {
        size_t plen = NLA_ALIGN(skb->len) - skb->len;
 
        if (plen > 0)
            memset(skb_put(skb, plen), 0, plen);
    }
}
 
static int queue_userspace_packet(struct datapath *dp, struct sk_buff *skb,
                  const struct sw_flow_key *key,
                  const struct dp_upcall_info *upcall_info,
                  uint32_t cutlen)
{
    struct ovs_header *upcall;
    struct sk_buff *nskb = NULL;
    struct sk_buff *user_skb = NULL; /* to be queued to userspace */
    struct nlattr *nla;
    size_t len;
    unsigned int hlen;
    int err, dp_ifindex;
 
    dp_ifindex = get_dpifindex(dp);
    if (!dp_ifindex)
        return -ENODEV;
 
    if (skb_vlan_tag_present(skb)) {
        nskb = skb_clone(skb, GFP_ATOMIC);
        if (!nskb)
            return -ENOMEM;
 
        nskb = __vlan_hwaccel_push_inside(nskb);
        if (!nskb)
            return -ENOMEM;
 
        skb = nskb;
    }
 
    if (nla_attr_size(skb->len) > USHRT_MAX) {
        err = -EFBIG;
        goto out;
    }
 
    /* Complete checksum if needed */
    if (skb->ip_summed == CHECKSUM_PARTIAL &&
        (err = skb_checksum_help(skb)))
        goto out;
 
    /* Older versions of OVS user space enforce alignment of the last
     * Netlink attribute to NLA_ALIGNTO which would require extensive
     * padding logic. Only perform zerocopy if padding is not required.
     */
    if (dp->user_features & OVS_DP_F_UNALIGNED)
        hlen = skb_zerocopy_headlen(skb);
    else
        hlen = skb->len;
 
    len = upcall_msg_size(upcall_info, hlen - cutlen);
    user_skb = genlmsg_new(len, GFP_ATOMIC);
    if (!user_skb) {
        err = -ENOMEM;
        goto out;
    }
 
    upcall = genlmsg_put(user_skb, 0, 0, &dp_packet_genl_family,
                 0, upcall_info->cmd);
    upcall->dp_ifindex = dp_ifindex;
 
    err = ovs_nla_put_key(key, key, OVS_PACKET_ATTR_KEY, false, user_skb);
    BUG_ON(err);
 
    if (upcall_info->userdata)
        __nla_put(user_skb, OVS_PACKET_ATTR_USERDATA,
              nla_len(upcall_info->userdata),
              nla_data(upcall_info->userdata));
 
 
    if (upcall_info->egress_tun_info) {
        nla = nla_nest_start(user_skb, OVS_PACKET_ATTR_EGRESS_TUN_KEY);
        err = ovs_nla_put_tunnel_info(user_skb,
                          upcall_info->egress_tun_info);
        BUG_ON(err);
        nla_nest_end(user_skb, nla);
    }
 
    if (upcall_info->actions_len) {
        nla = nla_nest_start(user_skb, OVS_PACKET_ATTR_ACTIONS);
        err = ovs_nla_put_actions(upcall_info->actions,
                      upcall_info->actions_len,
                      user_skb);
        if (!err)
            nla_nest_end(user_skb, nla);
        else
            nla_nest_cancel(user_skb, nla);
    }
 
    /* Add OVS_PACKET_ATTR_MRU */
    if (upcall_info->mru) {
        if (nla_put_u16(user_skb, OVS_PACKET_ATTR_MRU,
                upcall_info->mru)) {
            err = -ENOBUFS;
            goto out;
        }
        pad_packet(dp, user_skb);
    }
 
    /* Add OVS_PACKET_ATTR_LEN when packet is truncated */
    if (cutlen > 0) {
        if (nla_put_u32(user_skb, OVS_PACKET_ATTR_LEN,
                skb->len)) {
            err = -ENOBUFS;
            goto out;
        }
        pad_packet(dp, user_skb);
    }
 
    /* Only reserve room for attribute header, packet data is added
     * in skb_zerocopy()
     */
    if (!(nla = nla_reserve(user_skb, OVS_PACKET_ATTR_PACKET, 0))) {
        err = -ENOBUFS;
        goto out;
    }
    nla->nla_len = nla_attr_size(skb->len - cutlen);
 
    err = skb_zerocopy(user_skb, skb, skb->len - cutlen, hlen);
    if (err)
        goto out;
 
    /* Pad OVS_PACKET_ATTR_PACKET if linear copy was performed */
    pad_packet(dp, user_skb);
 
    ((struct nlmsghdr *) user_skb->data)->nlmsg_len = user_skb->len;
 
    err = genlmsg_unicast(ovs_dp_get_net(dp), user_skb, upcall_info->portid);
    user_skb = NULL;
out:
    if (err)
        skb_tx_error(skb);
    kfree_skb(user_skb);
    kfree_skb(nskb);
    return err;
}
 
static int ovs_packet_cmd_execute(struct sk_buff *skb, struct genl_info *info)
{
    struct ovs_header *ovs_header = info->userhdr;
    struct net *net = sock_net(skb->sk);
    struct nlattr **a = info->attrs;
    struct sw_flow_actions *acts;
    struct sk_buff *packet;
    struct sw_flow *flow;
    struct sw_flow_actions *sf_acts;
    struct datapath *dp;
    struct ethhdr *eth;
    struct vport *input_vport;
    u16 mru = 0;
    int len;
    int err;
    bool log = !a[OVS_PACKET_ATTR_PROBE];
 
    err = -EINVAL;
    if (!a[OVS_PACKET_ATTR_PACKET] || !a[OVS_PACKET_ATTR_KEY] ||
        !a[OVS_PACKET_ATTR_ACTIONS])
        goto err;
 
    len = nla_len(a[OVS_PACKET_ATTR_PACKET]);
    packet = __dev_alloc_skb(NET_IP_ALIGN + len, GFP_KERNEL);
    err = -ENOMEM;
    if (!packet)
        goto err;
    skb_reserve(packet, NET_IP_ALIGN);
 
    nla_memcpy(__skb_put(packet, len), a[OVS_PACKET_ATTR_PACKET], len);
 
    skb_reset_mac_header(packet);
    eth = eth_hdr(packet);
 
    /* Normally, setting the skb 'protocol' field would be handled by a
     * call to eth_type_trans(), but it assumes there's a sending
     * device, which we may not have.
     */
    if (eth_proto_is_802_3(eth->h_proto))
        packet->protocol = eth->h_proto;
    else
        packet->protocol = htons(ETH_P_802_2);
 
    /* Set packet's mru */
    if (a[OVS_PACKET_ATTR_MRU]) {
        mru = nla_get_u16(a[OVS_PACKET_ATTR_MRU]);
        packet->ignore_df = 1;
    }
    OVS_CB(packet)->mru = mru;
 
    /* Build an sw_flow for sending this packet. */
    flow = ovs_flow_alloc();
    err = PTR_ERR(flow);
    if (IS_ERR(flow))
        goto err_kfree_skb;
 
    err = ovs_flow_key_extract_userspace(net, a[OVS_PACKET_ATTR_KEY],
                         packet, &flow->key, log);
    if (err)
        goto err_flow_free;
 
    err = ovs_nla_copy_actions(net, a[OVS_PACKET_ATTR_ACTIONS],
                   &flow->key, &acts, log);
    if (err)
        goto err_flow_free;
 
    rcu_assign_pointer(flow->sf_acts, acts);
    packet->priority = flow->key.phy.priority;
    packet->mark = flow->key.phy.skb_mark;
 
    rcu_read_lock();
    dp = get_dp_rcu(net, ovs_header->dp_ifindex);
    err = -ENODEV;
    if (!dp)
        goto err_unlock;
 
    input_vport = ovs_vport_rcu(dp, flow->key.phy.in_port);
    if (!input_vport)
        input_vport = ovs_vport_rcu(dp, OVSP_LOCAL);
 
    if (!input_vport)
        goto err_unlock;
 
    packet->dev = input_vport->dev;
    OVS_CB(packet)->input_vport = input_vport;
    sf_acts = rcu_dereference(flow->sf_acts);
 
    local_bh_disable();
    err = ovs_execute_actions(dp, packet, sf_acts, &flow->key);
    local_bh_enable();
    rcu_read_unlock();
 
    ovs_flow_free(flow, false);
    return err;
 
err_unlock:
    rcu_read_unlock();
err_flow_free:
    ovs_flow_free(flow, false);
err_kfree_skb:
    kfree_skb(packet);
err:
    return err;
}
 
static const struct nla_policy packet_policy[OVS_PACKET_ATTR_MAX + 1] = {
    [OVS_PACKET_ATTR_PACKET] = { .len = ETH_HLEN },
    [OVS_PACKET_ATTR_KEY] = { .type = NLA_NESTED },
    [OVS_PACKET_ATTR_ACTIONS] = { .type = NLA_NESTED },
    [OVS_PACKET_ATTR_PROBE] = { .type = NLA_FLAG },
    [OVS_PACKET_ATTR_MRU] = { .type = NLA_U16 },
};
 
static struct genl_ops dp_packet_genl_ops[] = {
    { .cmd = OVS_PACKET_CMD_EXECUTE,
      .flags = GENL_UNS_ADMIN_PERM, /* Requires CAP_NET_ADMIN privilege. */
      .policy = packet_policy,
      .doit = ovs_packet_cmd_execute
    }
};
 
static struct genl_family dp_packet_genl_family = {
    .id = GENL_ID_GENERATE,
    .hdrsize = sizeof(struct ovs_header),
    .name = OVS_PACKET_FAMILY,
    .version = OVS_PACKET_VERSION,
    .maxattr = OVS_PACKET_ATTR_MAX,
    .netnsok = true,
    .parallel_ops = true,
    .ops = dp_packet_genl_ops,
    .n_ops = ARRAY_SIZE(dp_packet_genl_ops),
};
 
static void get_dp_stats(const struct datapath *dp, struct ovs_dp_stats *stats,
             struct ovs_dp_megaflow_stats *mega_stats)
{
    int i;
 
    memset(mega_stats, 0, sizeof(*mega_stats));
 
    stats->n_flows = ovs_flow_tbl_count(&dp->table);
    mega_stats->n_masks = ovs_flow_tbl_num_masks(&dp->table);
 
    stats->n_hit = stats->n_missed = stats->n_lost = 0;
 
    for_each_possible_cpu(i) {
        const struct dp_stats_percpu *percpu_stats;
        struct dp_stats_percpu local_stats;
        unsigned int start;
 
        percpu_stats = per_cpu_ptr(dp->stats_percpu, i);
 
        do {
            start = u64_stats_fetch_begin_irq(&percpu_stats->syncp);
            local_stats = *percpu_stats;
        } while (u64_stats_fetch_retry_irq(&percpu_stats->syncp, start));
 
        stats->n_hit += local_stats.n_hit;
        stats->n_missed += local_stats.n_missed;
        stats->n_lost += local_stats.n_lost;
        mega_stats->n_mask_hit += local_stats.n_mask_hit;
    }
}
 
static bool should_fill_key(const struct sw_flow_id *sfid, uint32_t ufid_flags)
{
    return ovs_identifier_is_ufid(sfid) &&
           !(ufid_flags & OVS_UFID_F_OMIT_KEY);
}
 
static bool should_fill_mask(uint32_t ufid_flags)
{
    return !(ufid_flags & OVS_UFID_F_OMIT_MASK);
}
 
static bool should_fill_actions(uint32_t ufid_flags)
{
    return !(ufid_flags & OVS_UFID_F_OMIT_ACTIONS);
}
 
static size_t ovs_flow_cmd_msg_size(const struct sw_flow_actions *acts,
                    const struct sw_flow_id *sfid,
                    uint32_t ufid_flags)
{
    size_t len = NLMSG_ALIGN(sizeof(struct ovs_header));
 
    /* OVS_FLOW_ATTR_UFID */
    if (sfid && ovs_identifier_is_ufid(sfid))
        len += nla_total_size(sfid->ufid_len);
 
    /* OVS_FLOW_ATTR_KEY */
    if (!sfid || should_fill_key(sfid, ufid_flags))
        len += nla_total_size(ovs_key_attr_size());
 
    /* OVS_FLOW_ATTR_MASK */
    if (should_fill_mask(ufid_flags))
        len += nla_total_size(ovs_key_attr_size());
 
    /* OVS_FLOW_ATTR_ACTIONS */
    if (should_fill_actions(ufid_flags))
        len += nla_total_size(acts->orig_len);
 
    return len
        + nla_total_size_64bit(sizeof(struct ovs_flow_stats)) /* OVS_FLOW_ATTR_STATS */
        + nla_total_size(1) /* OVS_FLOW_ATTR_TCP_FLAGS */
        + nla_total_size_64bit(8); /* OVS_FLOW_ATTR_USED */
}
 
/* Called with ovs_mutex or RCU read lock. */
static int ovs_flow_cmd_fill_stats(const struct sw_flow *flow,
                   struct sk_buff *skb)
{
    struct ovs_flow_stats stats;
    __be16 tcp_flags;
    unsigned long used;
 
    ovs_flow_stats_get(flow, &stats, &used, &tcp_flags);
 
    if (used &&
        nla_put_u64_64bit(skb, OVS_FLOW_ATTR_USED, ovs_flow_used_time(used),
                  OVS_FLOW_ATTR_PAD))
        return -EMSGSIZE;
 
    if (stats.n_packets &&
        nla_put_64bit(skb, OVS_FLOW_ATTR_STATS,
              sizeof(struct ovs_flow_stats), &stats,
              OVS_FLOW_ATTR_PAD))
        return -EMSGSIZE;
 
    if ((u8)ntohs(tcp_flags) &&
         nla_put_u8(skb, OVS_FLOW_ATTR_TCP_FLAGS, (u8)ntohs(tcp_flags)))
        return -EMSGSIZE;
 
    return 0;
}
 
/* Called with ovs_mutex or RCU read lock. */
static int ovs_flow_cmd_fill_actions(const struct sw_flow *flow,
                     struct sk_buff *skb, int skb_orig_len)
{
    struct nlattr *start;
    int err;
 
    /* If OVS_FLOW_ATTR_ACTIONS doesn't fit, skip dumping the actions if
     * this is the first flow to be dumped into 'skb'.  This is unusual for
     * Netlink but individual action lists can be longer than
     * NLMSG_GOODSIZE and thus entirely undumpable if we didn't do this.
     * The userspace caller can always fetch the actions separately if it
     * really wants them.  (Most userspace callers in fact don't care.)
     *
     * This can only fail for dump operations because the skb is always
     * properly sized for single flows.
     */
    start = nla_nest_start(skb, OVS_FLOW_ATTR_ACTIONS);
    if (start) {
        const struct sw_flow_actions *sf_acts;
 
        sf_acts = rcu_dereference_ovsl(flow->sf_acts);
        err = ovs_nla_put_actions(sf_acts->actions,
                      sf_acts->actions_len, skb);
 
        if (!err)
            nla_nest_end(skb, start);
        else {
            if (skb_orig_len)
                return err;
 
            nla_nest_cancel(skb, start);
        }
    } else if (skb_orig_len) {
        return -EMSGSIZE;
    }
 
    return 0;
}
 
/* Called with ovs_mutex or RCU read lock. */
static int ovs_flow_cmd_fill_info(const struct sw_flow *flow, int dp_ifindex,
                  struct sk_buff *skb, u32 portid,
                  u32 seq, u32 flags, u8 cmd, u32 ufid_flags)
{
    const int skb_orig_len = skb->len;
    struct ovs_header *ovs_header;
    int err;
 
    ovs_header = genlmsg_put(skb, portid, seq, &dp_flow_genl_family,
                 flags, cmd);
    if (!ovs_header)
        return -EMSGSIZE;
 
    ovs_header->dp_ifindex = dp_ifindex;
 
    err = ovs_nla_put_identifier(flow, skb);
    if (err)
        goto error;
 
    if (should_fill_key(&flow->id, ufid_flags)) {
        err = ovs_nla_put_masked_key(flow, skb);
        if (err)
            goto error;
    }
 
    if (should_fill_mask(ufid_flags)) {
        err = ovs_nla_put_mask(flow, skb);
        if (err)
            goto error;
    }
 
    err = ovs_flow_cmd_fill_stats(flow, skb);
    if (err)
        goto error;
 
    if (should_fill_actions(ufid_flags)) {
        err = ovs_flow_cmd_fill_actions(flow, skb, skb_orig_len);
        if (err)
            goto error;
    }
 
    genlmsg_end(skb, ovs_header);
    return 0;
 
error:
    genlmsg_cancel(skb, ovs_header);
    return err;
}
 
/* May not be called with RCU read lock. */
static struct sk_buff *ovs_flow_cmd_alloc_info(const struct sw_flow_actions *acts,
                           const struct sw_flow_id *sfid,
                           struct genl_info *info,
                           bool always,
                           uint32_t ufid_flags)
{
    struct sk_buff *skb;
    size_t len;
 
    if (!always && !ovs_must_notify(&dp_flow_genl_family, info,
                    GROUP_ID(&ovs_dp_flow_multicast_group)))
        return NULL;
 
    len = ovs_flow_cmd_msg_size(acts, sfid, ufid_flags);
    skb = genlmsg_new(len, GFP_KERNEL);
    if (!skb)
        return ERR_PTR(-ENOMEM);
 
    return skb;
}
 
/* Called with ovs_mutex. */
static struct sk_buff *ovs_flow_cmd_build_info(const struct sw_flow *flow,
                           int dp_ifindex,
                           struct genl_info *info, u8 cmd,
                           bool always, u32 ufid_flags)
{
    struct sk_buff *skb;
    int retval;
 
    skb = ovs_flow_cmd_alloc_info(ovsl_dereference(flow->sf_acts),
                      &flow->id, info, always, ufid_flags);
    if (IS_ERR_OR_NULL(skb))
        return skb;
 
    retval = ovs_flow_cmd_fill_info(flow, dp_ifindex, skb,
                    info->snd_portid, info->snd_seq, 0,
                    cmd, ufid_flags);
    BUG_ON(retval < 0);
    return skb;
}
 
static int ovs_flow_cmd_new(struct sk_buff *skb, struct genl_info *info)
{
    struct net *net = sock_net(skb->sk);
    struct nlattr **a = info->attrs;
    struct ovs_header *ovs_header = info->userhdr;
    struct sw_flow *flow = NULL, *new_flow;
    struct sw_flow_mask mask;
    struct sk_buff *reply;
    struct datapath *dp;
    struct sw_flow_key key;
    struct sw_flow_actions *acts;
    struct sw_flow_match match;
    u32 ufid_flags = ovs_nla_get_ufid_flags(a[OVS_FLOW_ATTR_UFID_FLAGS]);
    int error;
    bool log = !a[OVS_FLOW_ATTR_PROBE];
 
    /* Must have key and actions. */
    error = -EINVAL;
    if (!a[OVS_FLOW_ATTR_KEY]) {
        OVS_NLERR(log, "Flow key attr not present in new flow.");
        goto error;
    }
    if (!a[OVS_FLOW_ATTR_ACTIONS]) {
        OVS_NLERR(log, "Flow actions attr not present in new flow.");
        goto error;
    }
 
    /* Most of the time we need to allocate a new flow, do it before
     * locking.
     */
    new_flow = ovs_flow_alloc();
    if (IS_ERR(new_flow)) {
        error = PTR_ERR(new_flow);
        goto error;
    }
 
    /* Extract key. */
    ovs_match_init(&match, &key, &mask);
    error = ovs_nla_get_match(net, &match, a[OVS_FLOW_ATTR_KEY],
                  a[OVS_FLOW_ATTR_MASK], log);
    if (error)
        goto err_kfree_flow;
 
    ovs_flow_mask_key(&new_flow->key, &key, true, &mask);
 
    /* Extract flow identifier. */
    error = ovs_nla_get_identifier(&new_flow->id, a[OVS_FLOW_ATTR_UFID],
                       &key, log);
    if (error)
        goto err_kfree_flow;
 
    /* Validate actions. */
    error = ovs_nla_copy_actions(net, a[OVS_FLOW_ATTR_ACTIONS],
                     &new_flow->key, &acts, log);
    if (error) {
        OVS_NLERR(log, "Flow actions may not be safe on all matching packets.");
        goto err_kfree_flow;
    }
 
    reply = ovs_flow_cmd_alloc_info(acts, &new_flow->id, info, false,
                    ufid_flags);
    if (IS_ERR(reply)) {
        error = PTR_ERR(reply);
        goto err_kfree_acts;
    }
 
    ovs_lock();
    dp = get_dp(net, ovs_header->dp_ifindex);
    if (unlikely(!dp)) {
        error = -ENODEV;
        goto err_unlock_ovs;
    }
 
    /* Check if this is a duplicate flow */
    if (ovs_identifier_is_ufid(&new_flow->id))
        flow = ovs_flow_tbl_lookup_ufid(&dp->table, &new_flow->id);
    if (!flow)
        flow = ovs_flow_tbl_lookup(&dp->table, &key);
    if (likely(!flow)) {
        rcu_assign_pointer(new_flow->sf_acts, acts);
 
        /* Put flow in bucket. */
        error = ovs_flow_tbl_insert(&dp->table, new_flow, &mask);
        if (unlikely(error)) {
            acts = NULL;
            goto err_unlock_ovs;
        }
 
        if (unlikely(reply)) {
            error = ovs_flow_cmd_fill_info(new_flow,
                               ovs_header->dp_ifindex,
                               reply, info->snd_portid,
                               info->snd_seq, 0,
                               OVS_FLOW_CMD_NEW,
                               ufid_flags);
            BUG_ON(error < 0);
        }
        ovs_unlock();
    } else {
        struct sw_flow_actions *old_acts;
 
        /* Bail out if we're not allowed to modify an existing flow.
         * We accept NLM_F_CREATE in place of the intended NLM_F_EXCL
         * because Generic Netlink treats the latter as a dump
         * request.  We also accept NLM_F_EXCL in case that bug ever
         * gets fixed.
         */
        if (unlikely(info->nlhdr->nlmsg_flags & (NLM_F_CREATE
                             | NLM_F_EXCL))) {
            error = -EEXIST;
            goto err_unlock_ovs;
        }
        /* The flow identifier has to be the same for flow updates.
         * Look for any overlapping flow.
         */
        if (unlikely(!ovs_flow_cmp(flow, &match))) {
            if (ovs_identifier_is_key(&flow->id))
                flow = ovs_flow_tbl_lookup_exact(&dp->table,
                                 &match);
            else /* UFID matches but key is different */
                flow = NULL;
            if (!flow) {
                error = -ENOENT;
                goto err_unlock_ovs;
            }
        }
        /* Update actions. */
        old_acts = ovsl_dereference(flow->sf_acts);
        rcu_assign_pointer(flow->sf_acts, acts);
 
        if (unlikely(reply)) {
            error = ovs_flow_cmd_fill_info(flow,
                               ovs_header->dp_ifindex,
                               reply, info->snd_portid,
                               info->snd_seq, 0,
                               OVS_FLOW_CMD_NEW,
                               ufid_flags);
            BUG_ON(error < 0);
        }
        ovs_unlock();
 
        ovs_nla_free_flow_actions_rcu(old_acts);
        ovs_flow_free(new_flow, false);
    }
 
    if (reply)
        ovs_notify(&dp_flow_genl_family, &ovs_dp_flow_multicast_group, reply, info);
    return 0;
 
err_unlock_ovs:
    ovs_unlock();
    kfree_skb(reply);
err_kfree_acts:
    ovs_nla_free_flow_actions(acts);
err_kfree_flow:
    ovs_flow_free(new_flow, false);
error:
    return error;
}
 
/* Factor out action copy to avoid "Wframe-larger-than=1024" warning. */
static struct sw_flow_actions *get_flow_actions(struct net *net,
                        const struct nlattr *a,
                        const struct sw_flow_key *key,
                        const struct sw_flow_mask *mask,
                        bool log)
{
    struct sw_flow_actions *acts;
    struct sw_flow_key masked_key;
    int error;
 
    ovs_flow_mask_key(&masked_key, key, true, mask);
    error = ovs_nla_copy_actions(net, a, &masked_key, &acts, log);
    if (error) {
        OVS_NLERR(log,
              "Actions may not be safe on all matching packets");
        return ERR_PTR(error);
    }
 
    return acts;
}
 
static int ovs_flow_cmd_set(struct sk_buff *skb, struct genl_info *info)
{
    struct net *net = sock_net(skb->sk);
    struct nlattr **a = info->attrs;
    struct ovs_header *ovs_header = info->userhdr;
    struct sw_flow_key key;
    struct sw_flow *flow;
    struct sw_flow_mask mask;
    struct sk_buff *reply = NULL;
    struct datapath *dp;
    struct sw_flow_actions *old_acts = NULL, *acts = NULL;
    struct sw_flow_match match;
    struct sw_flow_id sfid;
    u32 ufid_flags = ovs_nla_get_ufid_flags(a[OVS_FLOW_ATTR_UFID_FLAGS]);
    int error = 0;
    bool log = !a[OVS_FLOW_ATTR_PROBE];
    bool ufid_present;
 
    ufid_present = ovs_nla_get_ufid(&sfid, a[OVS_FLOW_ATTR_UFID], log);
    if (a[OVS_FLOW_ATTR_KEY]) {
        ovs_match_init(&match, &key, &mask);
        error = ovs_nla_get_match(net, &match, a[OVS_FLOW_ATTR_KEY],
                      a[OVS_FLOW_ATTR_MASK], log);
    } else if (!ufid_present) {
        OVS_NLERR(log,
              "Flow set message rejected, Key attribute missing.");
        error = -EINVAL;
    }
    if (error)
        goto error;
 
    /* Validate actions. */
    if (a[OVS_FLOW_ATTR_ACTIONS]) {
        if (!a[OVS_FLOW_ATTR_KEY]) {
            OVS_NLERR(log,
                  "Flow key attribute not present in set flow.");
            error = -EINVAL;
            goto error;
        }
 
        acts = get_flow_actions(net, a[OVS_FLOW_ATTR_ACTIONS], &key,
                    &mask, log);
        if (IS_ERR(acts)) {
            error = PTR_ERR(acts);
            goto error;
        }
 
        /* Can allocate before locking if have acts. */
        reply = ovs_flow_cmd_alloc_info(acts, &sfid, info, false,
                        ufid_flags);
        if (IS_ERR(reply)) {
            error = PTR_ERR(reply);
            goto err_kfree_acts;
        }
    }
 
    ovs_lock();
    dp = get_dp(net, ovs_header->dp_ifindex);
    if (unlikely(!dp)) {
        error = -ENODEV;
        goto err_unlock_ovs;
    }
    /* Check that the flow exists. */
    if (ufid_present)
        flow = ovs_flow_tbl_lookup_ufid(&dp->table, &sfid);
    else
        flow = ovs_flow_tbl_lookup_exact(&dp->table, &match);
    if (unlikely(!flow)) {
        error = -ENOENT;
        goto err_unlock_ovs;
    }
 
    /* Update actions, if present. */
    if (likely(acts)) {
        old_acts = ovsl_dereference(flow->sf_acts);
        rcu_assign_pointer(flow->sf_acts, acts);
 
        if (unlikely(reply)) {
            error = ovs_flow_cmd_fill_info(flow,
                               ovs_header->dp_ifindex,
                               reply, info->snd_portid,
                               info->snd_seq, 0,
                               OVS_FLOW_CMD_NEW,
                               ufid_flags);
            BUG_ON(error < 0);
        }
    } else {
        /* Could not alloc without acts before locking. */
        reply = ovs_flow_cmd_build_info(flow, ovs_header->dp_ifindex,
                        info, OVS_FLOW_CMD_NEW, false,
                        ufid_flags);
 
        if (unlikely(IS_ERR(reply))) {
            error = PTR_ERR(reply);
            goto err_unlock_ovs;
        }
    }
 
    /* Clear stats. */
    if (a[OVS_FLOW_ATTR_CLEAR])
        ovs_flow_stats_clear(flow);
    ovs_unlock();
 
    if (reply)
        ovs_notify(&dp_flow_genl_family, &ovs_dp_flow_multicast_group, reply, info);
    if (old_acts)
        ovs_nla_free_flow_actions_rcu(old_acts);
 
    return 0;
 
err_unlock_ovs:
    ovs_unlock();
    kfree_skb(reply);
err_kfree_acts:
    ovs_nla_free_flow_actions(acts);
error:
    return error;
}
 
static int ovs_flow_cmd_get(struct sk_buff *skb, struct genl_info *info)
{
    struct nlattr **a = info->attrs;
    struct ovs_header *ovs_header = info->userhdr;
    struct net *net = sock_net(skb->sk);
    struct sw_flow_key key;
    struct sk_buff *reply;
    struct sw_flow *flow;
    struct datapath *dp;
    struct sw_flow_match match;
    struct sw_flow_id ufid;
    u32 ufid_flags = ovs_nla_get_ufid_flags(a[OVS_FLOW_ATTR_UFID_FLAGS]);
    int err = 0;
    bool log = !a[OVS_FLOW_ATTR_PROBE];
    bool ufid_present;
 
    ufid_present = ovs_nla_get_ufid(&ufid, a[OVS_FLOW_ATTR_UFID], log);
    if (a[OVS_FLOW_ATTR_KEY]) {
        ovs_match_init(&match, &key, NULL);
        err = ovs_nla_get_match(net, &match, a[OVS_FLOW_ATTR_KEY], NULL,
                    log);
    } else if (!ufid_present) {
        OVS_NLERR(log,
              "Flow get message rejected, Key attribute missing.");
        err = -EINVAL;
    }
    if (err)
        return err;
 
    ovs_lock();
    dp = get_dp(sock_net(skb->sk), ovs_header->dp_ifindex);
    if (!dp) {
        err = -ENODEV;
        goto unlock;
    }
 
    if (ufid_present)
        flow = ovs_flow_tbl_lookup_ufid(&dp->table, &ufid);
    else
        flow = ovs_flow_tbl_lookup_exact(&dp->table, &match);
    if (!flow) {
        err = -ENOENT;
        goto unlock;
    }
 
    reply = ovs_flow_cmd_build_info(flow, ovs_header->dp_ifindex, info,
                    OVS_FLOW_CMD_NEW, true, ufid_flags);
    if (IS_ERR(reply)) {
        err = PTR_ERR(reply);
        goto unlock;
    }
 
    ovs_unlock();
    return genlmsg_reply(reply, info);
unlock:
    ovs_unlock();
    return err;
}
 
static int ovs_flow_cmd_del(struct sk_buff *skb, struct genl_info *info)
{
    struct nlattr **a = info->attrs;
    struct ovs_header *ovs_header = info->userhdr;
    struct net *net = sock_net(skb->sk);
    struct sw_flow_key key;
    struct sk_buff *reply;
    struct sw_flow *flow = NULL;
    struct datapath *dp;
    struct sw_flow_match match;
    struct sw_flow_id ufid;
    u32 ufid_flags = ovs_nla_get_ufid_flags(a[OVS_FLOW_ATTR_UFID_FLAGS]);
    int err;
    bool log = !a[OVS_FLOW_ATTR_PROBE];
    bool ufid_present;
 
    ufid_present = ovs_nla_get_ufid(&ufid, a[OVS_FLOW_ATTR_UFID], log);
    if (a[OVS_FLOW_ATTR_KEY]) {
        ovs_match_init(&match, &key, NULL);
        err = ovs_nla_get_match(net, &match, a[OVS_FLOW_ATTR_KEY],
                    NULL, log);
        if (unlikely(err))
            return err;
    }
 
    ovs_lock();
    dp = get_dp(sock_net(skb->sk), ovs_header->dp_ifindex);
    if (unlikely(!dp)) {
        err = -ENODEV;
        goto unlock;
    }
 
    if (unlikely(!a[OVS_FLOW_ATTR_KEY] && !ufid_present)) {
        err = ovs_flow_tbl_flush(&dp->table);
        goto unlock;
    }
 
    if (ufid_present)
        flow = ovs_flow_tbl_lookup_ufid(&dp->table, &ufid);
    else
        flow = ovs_flow_tbl_lookup_exact(&dp->table, &match);
    if (unlikely(!flow)) {
        err = -ENOENT;
        goto unlock;
    }
 
    ovs_flow_tbl_remove(&dp->table, flow);
    ovs_unlock();
 
    reply = ovs_flow_cmd_alloc_info(rcu_dereference_raw(flow->sf_acts),
                    &flow->id, info, false, ufid_flags);
 
    if (likely(reply)) {
        if (likely(!IS_ERR(reply))) {
            rcu_read_lock();    /*To keep RCU checker happy. */
            err = ovs_flow_cmd_fill_info(flow, ovs_header->dp_ifindex,
                             reply, info->snd_portid,
                             info->snd_seq, 0,
                             OVS_FLOW_CMD_DEL,
                             ufid_flags);
            rcu_read_unlock();
            BUG_ON(err < 0);
            ovs_notify(&dp_flow_genl_family, &ovs_dp_flow_multicast_group, reply, info);
        } else {
            genl_set_err(&dp_flow_genl_family, sock_net(skb->sk), 0,
                     GROUP_ID(&ovs_dp_flow_multicast_group), PTR_ERR(reply));
 
        }
    }
 
    ovs_flow_free(flow, true);
    return 0;
unlock:
    ovs_unlock();
    return err;
}
 
static int ovs_flow_cmd_dump(struct sk_buff *skb, struct netlink_callback *cb)
{
    struct nlattr *a[__OVS_FLOW_ATTR_MAX];
    struct ovs_header *ovs_header = genlmsg_data(nlmsg_data(cb->nlh));
    struct table_instance *ti;
    struct datapath *dp;
    u32 ufid_flags;
    int err;
 
    err = genlmsg_parse(cb->nlh, &dp_flow_genl_family, a,
                OVS_FLOW_ATTR_MAX, flow_policy);
    if (err)
        return err;
    ufid_flags = ovs_nla_get_ufid_flags(a[OVS_FLOW_ATTR_UFID_FLAGS]);
 
    rcu_read_lock();
    dp = get_dp_rcu(sock_net(skb->sk), ovs_header->dp_ifindex);
    if (!dp) {
        rcu_read_unlock();
        return -ENODEV;
    }
 
    ti = rcu_dereference(dp->table.ti);
    for (;;) {
        struct sw_flow *flow;
        u32 bucket, obj;
 
        bucket = cb->args[0];
        obj = cb->args[1];
        flow = ovs_flow_tbl_dump_next(ti, &bucket, &obj);
        if (!flow)
            break;
 
        if (ovs_flow_cmd_fill_info(flow, ovs_header->dp_ifindex, skb,
                       NETLINK_CB(cb->skb).portid,
                       cb->nlh->nlmsg_seq, NLM_F_MULTI,
                       OVS_FLOW_CMD_NEW, ufid_flags) < 0)
            break;
 
        cb->args[0] = bucket;
        cb->args[1] = obj;
    }
    rcu_read_unlock();
    return skb->len;
}
 
static const struct nla_policy flow_policy[OVS_FLOW_ATTR_MAX + 1] = {
    [OVS_FLOW_ATTR_KEY] = { .type = NLA_NESTED },
    [OVS_FLOW_ATTR_MASK] = { .type = NLA_NESTED },
    [OVS_FLOW_ATTR_ACTIONS] = { .type = NLA_NESTED },
    [OVS_FLOW_ATTR_CLEAR] = { .type = NLA_FLAG },
    [OVS_FLOW_ATTR_PROBE] = { .type = NLA_FLAG },
    [OVS_FLOW_ATTR_UFID] = { .type = NLA_UNSPEC, .len = 1 },
    [OVS_FLOW_ATTR_UFID_FLAGS] = { .type = NLA_U32 },
};
 
static struct genl_ops dp_flow_genl_ops[] = {
    { .cmd = OVS_FLOW_CMD_NEW,
      .flags = GENL_UNS_ADMIN_PERM, /* Requires CAP_NET_ADMIN privilege. */
      .policy = flow_policy,
      .doit = ovs_flow_cmd_new
    },
    { .cmd = OVS_FLOW_CMD_DEL,
      .flags = GENL_UNS_ADMIN_PERM, /* Requires CAP_NET_ADMIN privilege. */
      .policy = flow_policy,
      .doit = ovs_flow_cmd_del
    },
    { .cmd = OVS_FLOW_CMD_GET,
      .flags = 0,           /* OK for unprivileged users. */
      .policy = flow_policy,
      .doit = ovs_flow_cmd_get,
      .dumpit = ovs_flow_cmd_dump
    },
    { .cmd = OVS_FLOW_CMD_SET,
      .flags = GENL_UNS_ADMIN_PERM, /* Requires CAP_NET_ADMIN privilege. */
      .policy = flow_policy,
      .doit = ovs_flow_cmd_set,
    },
};
 
static struct genl_family dp_flow_genl_family = {
    .id = GENL_ID_GENERATE,
    .hdrsize = sizeof(struct ovs_header),
    .name = OVS_FLOW_FAMILY,
    .version = OVS_FLOW_VERSION,
    .maxattr = OVS_FLOW_ATTR_MAX,
    .netnsok = true,
    .parallel_ops = true,
    .ops = dp_flow_genl_ops,
    .n_ops = ARRAY_SIZE(dp_flow_genl_ops),
    .mcgrps = &ovs_dp_flow_multicast_group,
    .n_mcgrps = 1,
};
 
static size_t ovs_dp_cmd_msg_size(void)
{
    size_t msgsize = NLMSG_ALIGN(sizeof(struct ovs_header));
 
    msgsize += nla_total_size(IFNAMSIZ);
    msgsize += nla_total_size_64bit(sizeof(struct ovs_dp_stats));
    msgsize += nla_total_size_64bit(sizeof(struct ovs_dp_megaflow_stats));
    msgsize += nla_total_size(sizeof(u32)); /* OVS_DP_ATTR_USER_FEATURES */
 
    return msgsize;
}
 
/* Called with ovs_mutex. */
static int ovs_dp_cmd_fill_info(struct datapath *dp, struct sk_buff *skb,
                u32 portid, u32 seq, u32 flags, u8 cmd)
{
    struct ovs_header *ovs_header;
    struct ovs_dp_stats dp_stats;
    struct ovs_dp_megaflow_stats dp_megaflow_stats;
    int err;
 
    ovs_header = genlmsg_put(skb, portid, seq, &dp_datapath_genl_family,
                   flags, cmd);
    if (!ovs_header)
        goto error;
 
    ovs_header->dp_ifindex = get_dpifindex(dp);
 
    err = nla_put_string(skb, OVS_DP_ATTR_NAME, ovs_dp_name(dp));
    if (err)
        goto nla_put_failure;
 
    get_dp_stats(dp, &dp_stats, &dp_megaflow_stats);
    if (nla_put_64bit(skb, OVS_DP_ATTR_STATS, sizeof(struct ovs_dp_stats),
              &dp_stats, OVS_DP_ATTR_PAD))
        goto nla_put_failure;
 
    if (nla_put_64bit(skb, OVS_DP_ATTR_MEGAFLOW_STATS,
              sizeof(struct ovs_dp_megaflow_stats),
              &dp_megaflow_stats, OVS_DP_ATTR_PAD))
        goto nla_put_failure;
 
    if (nla_put_u32(skb, OVS_DP_ATTR_USER_FEATURES, dp->user_features))
        goto nla_put_failure;
 
    genlmsg_end(skb, ovs_header);
    return 0;
 
nla_put_failure:
    genlmsg_cancel(skb, ovs_header);
error:
    return -EMSGSIZE;
}
 
static struct sk_buff *ovs_dp_cmd_alloc_info(void)
{
    return genlmsg_new(ovs_dp_cmd_msg_size(), GFP_KERNEL);
}
 
/* Called with rcu_read_lock or ovs_mutex. */
static struct datapath *lookup_datapath(struct net *net,
                    const struct ovs_header *ovs_header,
                    struct nlattr *a[OVS_DP_ATTR_MAX + 1])
{
    struct datapath *dp;
 
    if (!a[OVS_DP_ATTR_NAME])
        dp = get_dp(net, ovs_header->dp_ifindex);
    else {
        struct vport *vport;
 
        vport = ovs_vport_locate(net, nla_data(a[OVS_DP_ATTR_NAME]));
        dp = vport && vport->port_no == OVSP_LOCAL ? vport->dp : NULL;
    }
    return dp ? dp : ERR_PTR(-ENODEV);
}
 
static void ovs_dp_reset_user_features(struct sk_buff *skb, struct genl_info *info)
{
    struct datapath *dp;
 
    dp = lookup_datapath(sock_net(skb->sk), info->userhdr, info->attrs);
    if (IS_ERR(dp))
        return;
 
    WARN(dp->user_features, "Dropping previously announced user features\n");
    dp->user_features = 0;
}
 
static void ovs_dp_change(struct datapath *dp, struct nlattr *a[])
{
    if (a[OVS_DP_ATTR_USER_FEATURES])
        dp->user_features = nla_get_u32(a[OVS_DP_ATTR_USER_FEATURES]);
}
 
static int ovs_dp_cmd_new(struct sk_buff *skb, struct genl_info *info)
{
    struct nlattr **a = info->attrs;
    struct vport_parms parms;
    struct sk_buff *reply;
    struct datapath *dp;
    struct vport *vport;
    struct ovs_net *ovs_net;
    int err, i;
 
    err = -EINVAL;
    if (!a[OVS_DP_ATTR_NAME] || !a[OVS_DP_ATTR_UPCALL_PID])
        goto err;
 
    reply = ovs_dp_cmd_alloc_info();
    if (!reply)
        return -ENOMEM;
 
    err = -ENOMEM;
    dp = kzalloc(sizeof(*dp), GFP_KERNEL);
    if (dp == NULL)
        goto err_free_reply;
 
    ovs_dp_set_net(dp, sock_net(skb->sk));
 
    /* Allocate table. */
    err = ovs_flow_tbl_init(&dp->table);
    if (err)
        goto err_free_dp;
 
    dp->stats_percpu = netdev_alloc_pcpu_stats(struct dp_stats_percpu);
    if (!dp->stats_percpu) {
        err = -ENOMEM;
        goto err_destroy_table;
    }
 
    dp->ports = kmalloc(DP_VPORT_HASH_BUCKETS * sizeof(struct hlist_head),
                GFP_KERNEL);
    if (!dp->ports) {
        err = -ENOMEM;
        goto err_destroy_percpu;
    }
 
    for (i = 0; i < DP_VPORT_HASH_BUCKETS; i++)
        INIT_HLIST_HEAD(&dp->ports[i]);
 
    /* Set up our datapath device. */
    parms.name = nla_data(a[OVS_DP_ATTR_NAME]);
    parms.type = OVS_VPORT_TYPE_INTERNAL;
    parms.options = NULL;
    parms.dp = dp;
    parms.port_no = OVSP_LOCAL;
    parms.upcall_portids = a[OVS_DP_ATTR_UPCALL_PID];
 
    ovs_dp_change(dp, a);
 
    /* So far only local changes have been made, now need the lock. */
    ovs_lock();
 
    vport = new_vport(&parms);
    if (IS_ERR(vport)) {
        err = PTR_ERR(vport);
        if (err == -EBUSY)
            err = -EEXIST;
 
        if (err == -EEXIST) {
            /* An outdated user space instance that does not understand
             * the concept of user_features has attempted to create a new
             * datapath and is likely to reuse it. Drop all user features.
             */
            if (info->genlhdr->version < OVS_DP_VER_FEATURES)
                ovs_dp_reset_user_features(skb, info);
        }
 
        goto err_destroy_ports_array;
    }
 
    err = ovs_dp_cmd_fill_info(dp, reply, info->snd_portid,
                   info->snd_seq, 0, OVS_DP_CMD_NEW);
    BUG_ON(err < 0);
 
    ovs_net = net_generic(ovs_dp_get_net(dp), ovs_net_id);
    list_add_tail_rcu(&dp->list_node, &ovs_net->dps);
 
    ovs_unlock();
 
    ovs_notify(&dp_datapath_genl_family, &ovs_dp_datapath_multicast_group, reply, info);
    return 0;
 
err_destroy_ports_array:
    ovs_unlock();
    kfree(dp->ports);
err_destroy_percpu:
    free_percpu(dp->stats_percpu);
err_destroy_table:
    ovs_flow_tbl_destroy(&dp->table);
err_free_dp:
    kfree(dp);
err_free_reply:
    kfree_skb(reply);
err:
    return err;
}
 
/* Called with ovs_mutex. */
static void __dp_destroy(struct datapath *dp)
{
    int i;
 
    for (i = 0; i < DP_VPORT_HASH_BUCKETS; i++) {
        struct vport *vport;
        struct hlist_node *n;
 
        hlist_for_each_entry_safe(vport, n, &dp->ports[i], dp_hash_node)
            if (vport->port_no != OVSP_LOCAL)
                ovs_dp_detach_port(vport);
    }
 
    list_del_rcu(&dp->list_node);
 
    /* OVSP_LOCAL is datapath internal port. We need to make sure that
     * all ports in datapath are destroyed first before freeing datapath.
     */
    ovs_dp_detach_port(ovs_vport_ovsl(dp, OVSP_LOCAL));
 
    /* RCU destroy the flow table */
    call_rcu(&dp->rcu, destroy_dp_rcu);
}
 
static int ovs_dp_cmd_del(struct sk_buff *skb, struct genl_info *info)
{
    struct sk_buff *reply;
    struct datapath *dp;
    int err;
 
    reply = ovs_dp_cmd_alloc_info();
    if (!reply)
        return -ENOMEM;
 
    ovs_lock();
    dp = lookup_datapath(sock_net(skb->sk), info->userhdr, info->attrs);
    err = PTR_ERR(dp);
    if (IS_ERR(dp))
        goto err_unlock_free;
 
    err = ovs_dp_cmd_fill_info(dp, reply, info->snd_portid,
                   info->snd_seq, 0, OVS_DP_CMD_DEL);
    BUG_ON(err < 0);
 
    __dp_destroy(dp);
    ovs_unlock();
 
    ovs_notify(&dp_datapath_genl_family, &ovs_dp_datapath_multicast_group, reply, info);
    return 0;
 
err_unlock_free:
    ovs_unlock();
    kfree_skb(reply);
    return err;
}
 
static int ovs_dp_cmd_set(struct sk_buff *skb, struct genl_info *info)
{
    struct sk_buff *reply;
    struct datapath *dp;
    int err;
 
    reply = ovs_dp_cmd_alloc_info();
    if (!reply)
        return -ENOMEM;
 
    ovs_lock();
    dp = lookup_datapath(sock_net(skb->sk), info->userhdr, info->attrs);
    err = PTR_ERR(dp);
    if (IS_ERR(dp))
        goto err_unlock_free;
 
    ovs_dp_change(dp, info->attrs);
 
    err = ovs_dp_cmd_fill_info(dp, reply, info->snd_portid,
                   info->snd_seq, 0, OVS_DP_CMD_NEW);
    BUG_ON(err < 0);
 
    ovs_unlock();
 
    ovs_notify(&dp_datapath_genl_family, &ovs_dp_datapath_multicast_group, reply, info);
    return 0;
 
err_unlock_free:
    ovs_unlock();
    kfree_skb(reply);
    return err;
}
 
static int ovs_dp_cmd_get(struct sk_buff *skb, struct genl_info *info)
{
    struct sk_buff *reply;
    struct datapath *dp;
    int err;
 
    reply = ovs_dp_cmd_alloc_info();
    if (!reply)
        return -ENOMEM;
 
    ovs_lock();
    dp = lookup_datapath(sock_net(skb->sk), info->userhdr, info->attrs);
    if (IS_ERR(dp)) {
        err = PTR_ERR(dp);
        goto err_unlock_free;
    }
    err = ovs_dp_cmd_fill_info(dp, reply, info->snd_portid,
                   info->snd_seq, 0, OVS_DP_CMD_NEW);
    BUG_ON(err < 0);
    ovs_unlock();
 
    return genlmsg_reply(reply, info);
 
err_unlock_free:
    ovs_unlock();
    kfree_skb(reply);
    return err;
}
 
static int ovs_dp_cmd_dump(struct sk_buff *skb, struct netlink_callback *cb)
{
    struct ovs_net *ovs_net = net_generic(sock_net(skb->sk), ovs_net_id);
    struct datapath *dp;
    int skip = cb->args[0];
    int i = 0;
 
    ovs_lock();
    list_for_each_entry(dp, &ovs_net->dps, list_node) {
        if (i >= skip &&
            ovs_dp_cmd_fill_info(dp, skb, NETLINK_CB(cb->skb).portid,
                     cb->nlh->nlmsg_seq, NLM_F_MULTI,
                     OVS_DP_CMD_NEW) < 0)
            break;
        i++;
    }
    ovs_unlock();
 
    cb->args[0] = i;
 
    return skb->len;
}
 
static const struct nla_policy datapath_policy[OVS_DP_ATTR_MAX + 1] = {
    [OVS_DP_ATTR_NAME] = { .type = NLA_NUL_STRING, .len = IFNAMSIZ - 1 },
    [OVS_DP_ATTR_UPCALL_PID] = { .type = NLA_U32 },
    [OVS_DP_ATTR_USER_FEATURES] = { .type = NLA_U32 },
};
 
static struct genl_ops dp_datapath_genl_ops[] = {
    { .cmd = OVS_DP_CMD_NEW,
      .flags = GENL_UNS_ADMIN_PERM, /* Requires CAP_NET_ADMIN privilege. */
      .policy = datapath_policy,
      .doit = ovs_dp_cmd_new
    },
    { .cmd = OVS_DP_CMD_DEL,
      .flags = GENL_UNS_ADMIN_PERM, /* Requires CAP_NET_ADMIN privilege. */
      .policy = datapath_policy,
      .doit = ovs_dp_cmd_del
    },
    { .cmd = OVS_DP_CMD_GET,
      .flags = 0,           /* OK for unprivileged users. */
      .policy = datapath_policy,
      .doit = ovs_dp_cmd_get,
      .dumpit = ovs_dp_cmd_dump
    },
    { .cmd = OVS_DP_CMD_SET,
      .flags = GENL_UNS_ADMIN_PERM, /* Requires CAP_NET_ADMIN privilege. */
      .policy = datapath_policy,
      .doit = ovs_dp_cmd_set,
    },
};
 
static struct genl_family dp_datapath_genl_family = {
    .id = GENL_ID_GENERATE,
    .hdrsize = sizeof(struct ovs_header),
    .name = OVS_DATAPATH_FAMILY,
    .version = OVS_DATAPATH_VERSION,
    .maxattr = OVS_DP_ATTR_MAX,
    .netnsok = true,
    .parallel_ops = true,
    .ops = dp_datapath_genl_ops,
    .n_ops = ARRAY_SIZE(dp_datapath_genl_ops),
    .mcgrps = &ovs_dp_datapath_multicast_group,
    .n_mcgrps = 1,
};
 
/* Called with ovs_mutex or RCU read lock. */
static int ovs_vport_cmd_fill_info(struct vport *vport, struct sk_buff *skb,
                   u32 portid, u32 seq, u32 flags, u8 cmd)
{
    struct ovs_header *ovs_header;
    struct ovs_vport_stats vport_stats;
    int err;
 
    ovs_header = genlmsg_put(skb, portid, seq, &dp_vport_genl_family,
                 flags, cmd);
    if (!ovs_header)
        return -EMSGSIZE;
 
    ovs_header->dp_ifindex = get_dpifindex(vport->dp);
 
    if (nla_put_u32(skb, OVS_VPORT_ATTR_PORT_NO, vport->port_no) ||
        nla_put_u32(skb, OVS_VPORT_ATTR_TYPE, vport->ops->type) ||
        nla_put_string(skb, OVS_VPORT_ATTR_NAME,
               ovs_vport_name(vport)))
        goto nla_put_failure;
 
    ovs_vport_get_stats(vport, &vport_stats);
    if (nla_put_64bit(skb, OVS_VPORT_ATTR_STATS,
              sizeof(struct ovs_vport_stats), &vport_stats,
              OVS_VPORT_ATTR_PAD))
        goto nla_put_failure;
 
    if (ovs_vport_get_upcall_portids(vport, skb))
        goto nla_put_failure;
 
    err = ovs_vport_get_options(vport, skb);
    if (err == -EMSGSIZE)
        goto error;
 
    genlmsg_end(skb, ovs_header);
    return 0;
 
nla_put_failure:
    err = -EMSGSIZE;
error:
    genlmsg_cancel(skb, ovs_header);
    return err;
}
 
static struct sk_buff *ovs_vport_cmd_alloc_info(void)
{
    return nlmsg_new(NLMSG_DEFAULT_SIZE, GFP_KERNEL);
}
 
/* Called with ovs_mutex, only via ovs_dp_notify_wq(). */
struct sk_buff *ovs_vport_cmd_build_info(struct vport *vport, u32 portid,
                     u32 seq, u8 cmd)
{
    struct sk_buff *skb;
    int retval;
 
    skb = nlmsg_new(NLMSG_DEFAULT_SIZE, GFP_ATOMIC);
    if (!skb)
        return ERR_PTR(-ENOMEM);
 
    retval = ovs_vport_cmd_fill_info(vport, skb, portid, seq, 0, cmd);
    BUG_ON(retval < 0);
 
    return skb;
}
 
/* Called with ovs_mutex or RCU read lock. */
static struct vport *lookup_vport(struct net *net,
                  const struct ovs_header *ovs_header,
                  struct nlattr *a[OVS_VPORT_ATTR_MAX + 1])
{
    struct datapath *dp;
    struct vport *vport;
 
    if (a[OVS_VPORT_ATTR_NAME]) {
        vport = ovs_vport_locate(net, nla_data(a[OVS_VPORT_ATTR_NAME]));
        if (!vport)
            return ERR_PTR(-ENODEV);
        if (ovs_header->dp_ifindex &&
            ovs_header->dp_ifindex != get_dpifindex(vport->dp))
            return ERR_PTR(-ENODEV);
        return vport;
    } else if (a[OVS_VPORT_ATTR_PORT_NO]) {
        u32 port_no = nla_get_u32(a[OVS_VPORT_ATTR_PORT_NO]);
 
        if (port_no >= DP_MAX_PORTS)
            return ERR_PTR(-EFBIG);
 
        dp = get_dp(net, ovs_header->dp_ifindex);
        if (!dp)
            return ERR_PTR(-ENODEV);
 
        vport = ovs_vport_ovsl_rcu(dp, port_no);
        if (!vport)
            return ERR_PTR(-ENODEV);
        return vport;
    } else
        return ERR_PTR(-EINVAL);
}
 
/* Called with ovs_mutex */
static void update_headroom(struct datapath *dp)
{
    unsigned dev_headroom, max_headroom = 0;
    struct net_device *dev;
    struct vport *vport;
    int i;
 
    for (i = 0; i < DP_VPORT_HASH_BUCKETS; i++) {
        hlist_for_each_entry_rcu(vport, &dp->ports[i], dp_hash_node) {
            dev = vport->dev;
            dev_headroom = netdev_get_fwd_headroom(dev);
            if (dev_headroom > max_headroom)
                max_headroom = dev_headroom;
        }
    }
 
    dp->max_headroom = max_headroom;
    for (i = 0; i < DP_VPORT_HASH_BUCKETS; i++)
        hlist_for_each_entry_rcu(vport, &dp->ports[i], dp_hash_node)
            netdev_set_rx_headroom(vport->dev, max_headroom);
}
 
static int ovs_vport_cmd_new(struct sk_buff *skb, struct genl_info *info)
{
    struct nlattr **a = info->attrs;
    struct ovs_header *ovs_header = info->userhdr;
    struct vport_parms parms;
    struct sk_buff *reply;
    struct vport *vport;
    struct datapath *dp;
    u32 port_no;
    int err;
 
    if (!a[OVS_VPORT_ATTR_NAME] || !a[OVS_VPORT_ATTR_TYPE] ||
        !a[OVS_VPORT_ATTR_UPCALL_PID])
        return -EINVAL;
 
    port_no = a[OVS_VPORT_ATTR_PORT_NO]
        ? nla_get_u32(a[OVS_VPORT_ATTR_PORT_NO]) : 0;
    if (port_no >= DP_MAX_PORTS)
        return -EFBIG;
 
    reply = ovs_vport_cmd_alloc_info();
    if (!reply)
        return -ENOMEM;
 
    ovs_lock();
restart:
    dp = get_dp(sock_net(skb->sk), ovs_header->dp_ifindex);
    err = -ENODEV;
    if (!dp)
        goto exit_unlock_free;
 
    if (port_no) {
        vport = ovs_vport_ovsl(dp, port_no);
        err = -EBUSY;
        if (vport)
            goto exit_unlock_free;
    } else {
        for (port_no = 1; ; port_no++) {
            if (port_no >= DP_MAX_PORTS) {
                err = -EFBIG;
                goto exit_unlock_free;
            }
            vport = ovs_vport_ovsl(dp, port_no);
            if (!vport)
                break;
        }
    }
 
    parms.name = nla_data(a[OVS_VPORT_ATTR_NAME]);
    parms.type = nla_get_u32(a[OVS_VPORT_ATTR_TYPE]);
    parms.options = a[OVS_VPORT_ATTR_OPTIONS];
    parms.dp = dp;
    parms.port_no = port_no;
    parms.upcall_portids = a[OVS_VPORT_ATTR_UPCALL_PID];
 
    vport = new_vport(&parms);
    err = PTR_ERR(vport);
    if (IS_ERR(vport)) {
        if (err == -EAGAIN)
            goto restart;
        goto exit_unlock_free;
    }
 
    err = ovs_vport_cmd_fill_info(vport, reply, info->snd_portid,
                      info->snd_seq, 0, OVS_VPORT_CMD_NEW);
    BUG_ON(err < 0);
 
    if (netdev_get_fwd_headroom(vport->dev) > dp->max_headroom)
        update_headroom(dp);
    else
        netdev_set_rx_headroom(vport->dev, dp->max_headroom);
 
    ovs_unlock();
 
    ovs_notify(&dp_vport_genl_family, &ovs_dp_vport_multicast_group, reply, info);
    return 0;
 
exit_unlock_free:
    ovs_unlock();
    kfree_skb(reply);
    return err;
}
 
static int ovs_vport_cmd_set(struct sk_buff *skb, struct genl_info *info)
{
    struct nlattr **a = info->attrs;
    struct sk_buff *reply;
    struct vport *vport;
    int err;
 
    reply = ovs_vport_cmd_alloc_info();
    if (!reply)
        return -ENOMEM;
 
    ovs_lock();
    vport = lookup_vport(sock_net(skb->sk), info->userhdr, a);
    err = PTR_ERR(vport);
    if (IS_ERR(vport))
        goto exit_unlock_free;
 
    if (a[OVS_VPORT_ATTR_TYPE] &&
        nla_get_u32(a[OVS_VPORT_ATTR_TYPE]) != vport->ops->type) {
        err = -EINVAL;
        goto exit_unlock_free;
    }
 
    if (a[OVS_VPORT_ATTR_OPTIONS]) {
        err = ovs_vport_set_options(vport, a[OVS_VPORT_ATTR_OPTIONS]);
        if (err)
            goto exit_unlock_free;
    }
 
    if (a[OVS_VPORT_ATTR_UPCALL_PID]) {
        struct nlattr *ids = a[OVS_VPORT_ATTR_UPCALL_PID];
 
        err = ovs_vport_set_upcall_portids(vport, ids);
        if (err)
            goto exit_unlock_free;
    }
 
    err = ovs_vport_cmd_fill_info(vport, reply, info->snd_portid,
                      info->snd_seq, 0, OVS_VPORT_CMD_NEW);
    BUG_ON(err < 0);
    ovs_unlock();
 
    ovs_notify(&dp_vport_genl_family, &ovs_dp_vport_multicast_group, reply, info);
    return 0;
 
exit_unlock_free:
    ovs_unlock();
    kfree_skb(reply);
    return err;
}
 
static int ovs_vport_cmd_del(struct sk_buff *skb, struct genl_info *info)
{
    bool must_update_headroom = false;
    struct nlattr **a = info->attrs;
    struct sk_buff *reply;
    struct datapath *dp;
    struct vport *vport;
    int err;
 
    reply = ovs_vport_cmd_alloc_info();
    if (!reply)
        return -ENOMEM;
 
    ovs_lock();
    vport = lookup_vport(sock_net(skb->sk), info->userhdr, a);
    err = PTR_ERR(vport);
    if (IS_ERR(vport))
        goto exit_unlock_free;
 
    if (vport->port_no == OVSP_LOCAL) {
        err = -EINVAL;
        goto exit_unlock_free;
    }
 
    err = ovs_vport_cmd_fill_info(vport, reply, info->snd_portid,
                      info->snd_seq, 0, OVS_VPORT_CMD_DEL);
    BUG_ON(err < 0);
 
    /* the vport deletion may trigger dp headroom update */
    dp = vport->dp;
    if (netdev_get_fwd_headroom(vport->dev) == dp->max_headroom)
        must_update_headroom = true;
    netdev_reset_rx_headroom(vport->dev);
    ovs_dp_detach_port(vport);
 
    if (must_update_headroom)
        update_headroom(dp);
 
    ovs_unlock();
 
    ovs_notify(&dp_vport_genl_family, &ovs_dp_vport_multicast_group, reply, info);
    return 0;
 
exit_unlock_free:
    ovs_unlock();
    kfree_skb(reply);
    return err;
}
 
static int ovs_vport_cmd_get(struct sk_buff *skb, struct genl_info *info)
{
    struct nlattr **a = info->attrs;
    struct ovs_header *ovs_header = info->userhdr;
    struct sk_buff *reply;
    struct vport *vport;
    int err;
 
    reply = ovs_vport_cmd_alloc_info();
    if (!reply)
        return -ENOMEM;
 
    rcu_read_lock();
    vport = lookup_vport(sock_net(skb->sk), ovs_header, a);
    err = PTR_ERR(vport);
    if (IS_ERR(vport))
        goto exit_unlock_free;
    err = ovs_vport_cmd_fill_info(vport, reply, info->snd_portid,
                      info->snd_seq, 0, OVS_VPORT_CMD_NEW);
    BUG_ON(err < 0);
    rcu_read_unlock();
 
    return genlmsg_reply(reply, info);
 
exit_unlock_free:
    rcu_read_unlock();
    kfree_skb(reply);
    return err;
}
 
static int ovs_vport_cmd_dump(struct sk_buff *skb, struct netlink_callback *cb)
{
    struct ovs_header *ovs_header = genlmsg_data(nlmsg_data(cb->nlh));
    struct datapath *dp;
    int bucket = cb->args[0], skip = cb->args[1];
    int i, j = 0;
 
    rcu_read_lock();
    dp = get_dp_rcu(sock_net(skb->sk), ovs_header->dp_ifindex);
    if (!dp) {
        rcu_read_unlock();
        return -ENODEV;
    }
    for (i = bucket; i < DP_VPORT_HASH_BUCKETS; i++) {
        struct vport *vport;
 
        j = 0;
        hlist_for_each_entry_rcu(vport, &dp->ports[i], dp_hash_node) {
            if (j >= skip &&
                ovs_vport_cmd_fill_info(vport, skb,
                            NETLINK_CB(cb->skb).portid,
                            cb->nlh->nlmsg_seq,
                            NLM_F_MULTI,
                            OVS_VPORT_CMD_NEW) < 0)
                goto out;
 
            j++;
        }
        skip = 0;
    }
out:
    rcu_read_unlock();
 
    cb->args[0] = i;
    cb->args[1] = j;
 
    return skb->len;
}
 
static const struct nla_policy vport_policy[OVS_VPORT_ATTR_MAX + 1] = {
    [OVS_VPORT_ATTR_NAME] = { .type = NLA_NUL_STRING, .len = IFNAMSIZ - 1 },
    [OVS_VPORT_ATTR_STATS] = { .len = sizeof(struct ovs_vport_stats) },
    [OVS_VPORT_ATTR_PORT_NO] = { .type = NLA_U32 },
    [OVS_VPORT_ATTR_TYPE] = { .type = NLA_U32 },
    [OVS_VPORT_ATTR_UPCALL_PID] = { .type = NLA_U32 },
    [OVS_VPORT_ATTR_OPTIONS] = { .type = NLA_NESTED },
};
 
static struct genl_ops dp_vport_genl_ops[] = {
    { .cmd = OVS_VPORT_CMD_NEW,
      .flags = GENL_UNS_ADMIN_PERM, /* Requires CAP_NET_ADMIN privilege. */
      .policy = vport_policy,
      .doit = ovs_vport_cmd_new
    },
    { .cmd = OVS_VPORT_CMD_DEL,
      .flags = GENL_UNS_ADMIN_PERM, /* Requires CAP_NET_ADMIN privilege. */
      .policy = vport_policy,
      .doit = ovs_vport_cmd_del
    },
    { .cmd = OVS_VPORT_CMD_GET,
      .flags = 0,           /* OK for unprivileged users. */
      .policy = vport_policy,
      .doit = ovs_vport_cmd_get,
      .dumpit = ovs_vport_cmd_dump
    },
    { .cmd = OVS_VPORT_CMD_SET,
      .flags = GENL_UNS_ADMIN_PERM, /* Requires CAP_NET_ADMIN privilege. */
      .policy = vport_policy,
      .doit = ovs_vport_cmd_set,
    },
};
 
struct genl_family dp_vport_genl_family = {
    .id = GENL_ID_GENERATE,
    .hdrsize = sizeof(struct ovs_header),
    .name = OVS_VPORT_FAMILY,
    .version = OVS_VPORT_VERSION,
    .maxattr = OVS_VPORT_ATTR_MAX,
    .netnsok = true,
    .parallel_ops = true,
    .ops = dp_vport_genl_ops,
    .n_ops = ARRAY_SIZE(dp_vport_genl_ops),
    .mcgrps = &ovs_dp_vport_multicast_group,
    .n_mcgrps = 1,
};
 
static struct genl_family *dp_genl_families[] = {
    &dp_datapath_genl_family,
    &dp_vport_genl_family,
    &dp_flow_genl_family,
    &dp_packet_genl_family,
};
 
static void dp_unregister_genl(int n_families)
{
    int i;
 
    for (i = 0; i < n_families; i++)
        genl_unregister_family(dp_genl_families[i]);
}
 
static int dp_register_genl(void)
{
    int err;
    int i;
 
    for (i = 0; i < ARRAY_SIZE(dp_genl_families); i++) {
 
        err = genl_register_family(dp_genl_families[i]);
        if (err)
            goto error;
    }
 
    return 0;
 
error:
    dp_unregister_genl(i);
    return err;
}
 
static int __net_init ovs_init_net(struct net *net)
{
    struct ovs_net *ovs_net = net_generic(net, ovs_net_id);
 
    INIT_LIST_HEAD(&ovs_net->dps);
    INIT_WORK(&ovs_net->dp_notify_work, ovs_dp_notify_wq);
    ovs_ct_init(net);
    return 0;
}
 
static void __net_exit list_vports_from_net(struct net *net, struct net *dnet,
                        struct list_head *head)
{
    struct ovs_net *ovs_net = net_generic(net, ovs_net_id);
    struct datapath *dp;
 
    list_for_each_entry(dp, &ovs_net->dps, list_node) {
        int i;
 
        for (i = 0; i < DP_VPORT_HASH_BUCKETS; i++) {
            struct vport *vport;
 
            hlist_for_each_entry(vport, &dp->ports[i], dp_hash_node) {
 
                if (vport->ops->type != OVS_VPORT_TYPE_INTERNAL)
                    continue;
 
                if (dev_net(vport->dev) == dnet)
                    list_add(&vport->detach_list, head);
            }
        }
    }
}
 
static void __net_exit ovs_exit_net(struct net *dnet)
{
    struct datapath *dp, *dp_next;
    struct ovs_net *ovs_net = net_generic(dnet, ovs_net_id);
    struct vport *vport, *vport_next;
    struct net *net;
    LIST_HEAD(head);
 
    ovs_ct_exit(dnet);
    ovs_lock();
    list_for_each_entry_safe(dp, dp_next, &ovs_net->dps, list_node)
        __dp_destroy(dp);
 
    rtnl_lock();
    for_each_net(net)
        list_vports_from_net(net, dnet, &head);
    rtnl_unlock();
 
    /* Detach all vports from given namespace. */
    list_for_each_entry_safe(vport, vport_next, &head, detach_list) {
        list_del(&vport->detach_list);
        ovs_dp_detach_port(vport);
    }
 
    ovs_unlock();
 
    cancel_work_sync(&ovs_net->dp_notify_work);
}
 
static struct pernet_operations ovs_net_ops = {
    .init = ovs_init_net,
    .exit = ovs_exit_net,
    .id   = &ovs_net_id,
    .size = sizeof(struct ovs_net),
};
 
static int __init dp_init(void)
{
    int err;
 
    BUILD_BUG_ON(sizeof(struct ovs_skb_cb) > FIELD_SIZEOF(struct sk_buff, cb));
 
    pr_info("Open vSwitch switching datapath %s\n", VERSION);
 
    err = compat_init();
    if (err)
        goto error;
 
    err = action_fifos_init();
    if (err)
        goto error_compat_exit;
 
    err = ovs_internal_dev_rtnl_link_register();
    if (err)
        goto error_action_fifos_exit;
 
    err = ovs_flow_init();
    if (err)
        goto error_unreg_rtnl_link;
 
    err = ovs_vport_init();
    if (err)
        goto error_flow_exit;
 
    err = register_pernet_device(&ovs_net_ops);
    if (err)
        goto error_vport_exit;
 
    err = register_netdevice_notifier(&ovs_dp_device_notifier);
    if (err)
        goto error_netns_exit;
 
    err = ovs_netdev_init();
    if (err)
        goto error_unreg_notifier;
 
    err = dp_register_genl();
    if (err < 0)
        goto error_unreg_netdev;
 
    return 0;
 
error_unreg_netdev:
    ovs_netdev_exit();
error_unreg_notifier:
    unregister_netdevice_notifier(&ovs_dp_device_notifier);
error_netns_exit:
    unregister_pernet_device(&ovs_net_ops);
error_vport_exit:
    ovs_vport_exit();
error_flow_exit:
    ovs_flow_exit();
error_unreg_rtnl_link:
    ovs_internal_dev_rtnl_link_unregister();
error_action_fifos_exit:
    action_fifos_exit();
error_compat_exit:
    compat_exit();
error:
    return err;
}
 
static void dp_cleanup(void)
{
    dp_unregister_genl(ARRAY_SIZE(dp_genl_families));
    ovs_netdev_exit();
    unregister_netdevice_notifier(&ovs_dp_device_notifier);
    unregister_pernet_device(&ovs_net_ops);
    rcu_barrier();
    ovs_vport_exit();
    ovs_flow_exit();
    ovs_internal_dev_rtnl_link_unregister();
    action_fifos_exit();
    compat_exit();
    /**************************************keqiang's logic*******************************************/
    //keqiang's logic
    __hashtbl_exit();
    /************************************************************************************************/
}
 
module_init(dp_init);
module_exit(dp_cleanup);
 
MODULE_DESCRIPTION("Open vSwitch switching datapath");
MODULE_LICENSE("GPL");
MODULE_VERSION(VERSION);
