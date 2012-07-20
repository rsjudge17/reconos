/*
 * Copyright 2012 Daniel Borkmann <dborkma@tik.ee.ethz.ch>
 * Copyright 2012 Ariane Keller <ariane.keller@tik.ee.ethz.ch>
 */

#include <linux/init.h>
#include <linux/module.h>
#include <linux/errno.h>
#include <linux/slab.h>
#include <linux/wait.h>
#include <linux/kernel.h>
#include <linux/kthread.h>

#include "xt_engine.h"
#include "reconos.h"
#include "mbox.h"

enum noc_hw_slots {
	C_HWT_SLOT_NR = 0,
#define C_HWT_SLOT_NR		0
	B_HWT_SLOT_NR,
#define B_HWT_SLOT_NR		1
	A_HWT_SLOT_NR,
#define A_HWT_SLOT_NR		2
	E_HWT_SLOT_NR,
#define E_HWT_SLOT_NR		3
	__MAX_HWT_SLOT_NR
#define __MAX_HWT_SLOT_NR	4
};

#define HW_TO_SW_SLOT		B_HWT_SLOT_NR
#define SW_TO_HW_SLOT		C_HWT_SLOT_NR

struct noc_slot {
	struct reconos_hwt hwt;
	struct reconos_resource res[2];
	struct mbox mb_put, mb_get;
};

static struct noc_slot noc[__MAX_HWT_SLOT_NR];

struct noc_pkt {
	u8 hw_addr_block;	/* local hardware address */
	u8 hw_addr_switch;	/* global hardware address */
	u8 priority:2,		/* range [0..3] */
	   direction:1,		/* 1 for ingress, 0 for egress */
	   latency_critical:1;	/* 1 lat. critical, 0 not latency critical */
	u32 src_idp;		/* src IDP of the packet */
	u32 dst_idp;		/* dst IDP of the packet */
	u32 payload_len;	/* payload length in bytes */
	u8* payload;		/* pointer to the actual payload */
};

static struct task_struct *thread;
static u8 *shared_mem_h2s, *shared_mem_s2h;
static int order = 0;

static struct noc_pkt *skb_to_noc_pkt(struct sk_buff *skb)
{
	struct noc_pkt *npkt;

	return npkt;
}

static struct sk_buff *noc_pkt_to_skb(struct noc_pkt *npkt)
{
	/* stub */
	return NULL;
}

static int reconos_noc_sendpkt(struct noc_pkt *npkt)
{
	/* stub */
	return 0;
}

void packet_sw_to_hw(struct sk_buff *skb, enum path_type dir)
{
	struct noc_pkt *npkt;

	npkt = skb_to_noc_pkt(skb);
	if (npkt) {
		reconos_noc_sendpkt(npkt);
	}
}

static void packet_hw_to_sw(struct noc_pkt *npkt)
{
	struct sk_buff *skb;
	enum path_type dir;

	skb = noc_pkt_to_skb(npkt);
	if (skb) {
		dir = read_path_from_skb(skb);
		engine_backlog_tail(skb, dir);
	}
}

static int hwif_worker_thread(void *arg)
{
	u32 pkt_len;
	struct noc_pkt npkt;

	while (likely(!kthread_should_stop())) {
		memset(&npkt, 0, sizeof(npkt));

		/* sleeps here */
		mbox_get(&noc[HW_TO_SW_SLOT].mb_get);
		pkt_len = *(u32 *) shared_mem_h2s;

		/* FIXME: fill this out */
		npkt.priority = 0;
		npkt.direction = 0;
		npkt.latency_critical = 0;
		npkt.src_idp = 0;
		npkt.dst_idp = 0;
		npkt.payload_len = pkt_len;
		npkt.payload = shared_mem_h2s + sizeof(u32);

		packet_hw_to_sw(&npkt);
	}

	return 0;
}

static int reconos_noc_init(void)
{
	int i;

	printk(KERN_INFO "[lana] bootstraping noc (%d slots)!\n",
	       __MAX_HWT_SLOT_NR);

	reconos_init_autodetect();
	memset(noc, 0, sizeof(noc));

	reconos_proc_control_selftest();

	for (i = 0; i < ARRAY_SIZE(noc); ++i) {
		mbox_init(&noc[i].mb_put, 2);
		mbox_init(&noc[i].mb_get, 2);

		noc[i].res[0].type = RECONOS_TYPE_MBOX;
		noc[i].res[0].ptr  = &noc[i].mb_put;	  	
	    	noc[i].res[1].type = RECONOS_TYPE_MBOX;
		noc[i].res[1].ptr  = &noc[i].mb_get;

		reconos_hwt_setresources(&noc[i].hwt, noc[i].res,
					 ARRAY_SIZE(noc[i].res));
		reconos_hwt_create(&noc[i].hwt, i, NULL);

		printk(KERN_INFO "[lana] noc hwt slot%d up and running!\n", i);
	}

	/* FIXME: might break in some cases on 64 bit */
	shared_mem_h2s = (u8 *) __get_free_pages(GFP_KERNEL | __GFP_NOWARN,
						 order);
	mbox_put(&noc[HW_TO_SW_SLOT].mb_put, (uint32_t) shared_mem_h2s);
	printk(KERN_INFO "[lana] noc hw->sw memory %p (order%d, slot%d)\n",
	       shared_mem_h2s, order, HW_TO_SW_SLOT);

	/* FIXME: might break in some cases on 64 bit */
	shared_mem_s2h = (u8 *) __get_free_pages(GFP_KERNEL | __GFP_NOWARN,
						 order);
	mbox_put(&noc[SW_TO_HW_SLOT].mb_put, (uint32_t) shared_mem_s2h);
	printk(KERN_INFO "[lana] noc sw->hw memory %p (order%d, slot%d)\n",
	       shared_mem_s2h, order, SW_TO_HW_SLOT);

	return 0;
}

static void reconos_noc_cleanup(void)
{
	reconos_cleanup();
	free_pages((unsigned long) shared_mem_h2s, order);
	free_pages((unsigned long) shared_mem_s2h, order);

	printk(KERN_INFO "[lana] noc halted!\n");
}

int init_hwif(void)
{
	int ret;

	thread = kthread_create(hwif_worker_thread, NULL, "lana_hwif");
	if (IS_ERR(thread)) {
		printk(KERN_ERR "Cannot create hwif thread!\n");
		return -EIO;
	}

	ret = reconos_noc_init();
	if (ret) {
		printk(KERN_ERR "Cannot init reconos noc!\n");
		kthread_stop(thread);
		return -EIO;
	}

	return 0;
}

void cleanup_hwif(void)
{
	kthread_stop(thread);
	reconos_noc_cleanup();
}
