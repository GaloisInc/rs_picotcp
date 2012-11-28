/*********************************************************************
PicoTCP. Copyright (c) 2012 TASS Belgium NV. Some rights reserved.
See LICENSE and COPYING for usage.
Do not redistribute without a written permission by the Copyright
holders.

Authors: Daniele Lacamera
*********************************************************************/


#include "pico_config.h"
#include "pico_arp.h"
#include "rb.h"
#include "pico_ipv4.h"
#include "pico_device.h"
#include "pico_stack.h"
const uint8_t PICO_ETHADDR_ANY[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
#define PICO_ARP_TIMEOUT 600000


struct
__attribute__ ((__packed__)) 
pico_arp_hdr
{
  uint16_t htype;
  uint16_t ptype;
  uint8_t hsize;
  uint8_t psize;
  uint16_t opcode;
  uint8_t s_mac[PICO_SIZE_ETH];
  struct pico_ip4 src;
  uint8_t d_mac[PICO_SIZE_ETH];
  struct pico_ip4 dst;
};


#define PICO_SIZE_ARPHDR ((sizeof(struct pico_arp_hdr)))



/*****************/
/**  ARP TREE **/
/*****************/

/* Routing destination */

RB_HEAD(arp_tree, pico_arp);
RB_PROTOTYPE_STATIC(arp_tree, pico_arp, node, arp_compare);


static int arp_compare(struct pico_arp *a, struct pico_arp *b)
{

  if (a->ipv4.addr < b->ipv4.addr)
    return -1;
  else if (a->ipv4.addr > b->ipv4.addr)
    return 1;
  return 0;
}

RB_GENERATE_STATIC(arp_tree, pico_arp, node, arp_compare);

static struct arp_tree Arp_table;

/*********************/
/**  END ARP TREE **/
/*********************/

struct pico_arp *pico_arp_get(struct pico_ip4 *dst)
{
  struct pico_arp search, *found;
  search.ipv4.addr = dst->addr;
  found = RB_FIND(arp_tree, &Arp_table, &search);
  if (found && (found->arp_status != PICO_ARP_STATUS_STALE))
    return found;
  return NULL;
}

void dbg_arp(void)
{
  struct pico_arp *a;
  RB_FOREACH(a, arp_tree, &Arp_table) {
    dbg("ARP to  %08x, mac: %02x:%02x:%02x:%02x:%02x:%02x\n", a->ipv4.addr,a->eth.addr[0],a->eth.addr[1],a->eth.addr[2],a->eth.addr[3],a->eth.addr[4],a->eth.addr[5] );
  }
}

void arp_expire(unsigned long now, void *_stale)
{
  struct pico_arp *stale = (struct pico_arp *) _stale;
  stale->arp_status = PICO_ARP_STATUS_STALE;
  dbg("ARP: Setting arp_status to STALE\n");
  pico_arp_query(stale->dev, &stale->ipv4);

}

int pico_arp_receive(struct pico_frame *f)
{
  struct pico_arp_hdr *hdr;
  struct pico_arp search, *found, *new = NULL;
  int ret = -1;
  hdr = (struct pico_arp_hdr *) f->net_hdr;

  if (!hdr)
    goto end;


  /* Populate a new arp entry */
  search.ipv4.addr = hdr->src.addr;
  memcpy(search.eth.addr, hdr->s_mac, PICO_SIZE_ETH);

  /* Search for already existing entry */
  found = RB_FIND(arp_tree, &Arp_table, &search);
  if (!found) {
    new = pico_zalloc(sizeof(struct pico_arp));
    if (!new)
      goto end;
    new->ipv4.addr = hdr->src.addr;
  }
  else if (found->arp_status == PICO_ARP_STATUS_STALE) {
    /* Replace if stale */
    new = found;
    RB_REMOVE(arp_tree, &Arp_table, new);
  }

  ret = 0;

  if (new) {
    memcpy(new->eth.addr, hdr->s_mac, PICO_SIZE_ETH);
    new->arp_status = PICO_ARP_STATUS_REACHABLE;
    new->timestamp  = PICO_TIME();
    new->dev = f->dev;
    RB_INSERT(arp_tree, &Arp_table, new);
    dbg("ARP ## reachable.\n");
    pico_timer_add(PICO_ARP_TIMEOUT, arp_expire, new);
  }

  if (hdr->opcode == PICO_ARP_REQUEST) {
    struct pico_ip4 me;
    struct pico_eth_hdr *eh = (struct pico_eth_hdr *)f->datalink_hdr;
    struct pico_device *link_dev;
    me.addr = hdr->dst.addr;

    link_dev = pico_ipv4_link_find(&me);
    if (link_dev != f->dev)
      goto end;

    hdr->opcode = PICO_ARP_REPLY;
    memcpy(hdr->d_mac, hdr->s_mac, PICO_SIZE_ETH);
    memcpy(hdr->s_mac, f->dev->eth->mac.addr, PICO_SIZE_ETH);
    hdr->dst.addr = hdr->src.addr;
    hdr->src.addr = me.addr;

    /* Prepare eth header for arp reply */
    memcpy(eh->daddr, eh->saddr, PICO_SIZE_ETH);
    memcpy(eh->saddr, f->dev->eth->mac.addr, PICO_SIZE_ETH);
    f->start = f->datalink_hdr;
    f->len = PICO_SIZE_ETHHDR + PICO_SIZE_ARPHDR;
    f->dev->send(f->dev, f->start, f->len);
  }

  dbg_arp();

end:
  pico_frame_discard(f);
  return ret;
}

int pico_arp_query(struct pico_device *dev, struct pico_ip4 *dst)
{
  struct pico_frame *q = pico_frame_alloc(PICO_SIZE_ETHHDR + PICO_SIZE_ARPHDR);
  struct pico_eth_hdr *eh;
  struct pico_arp_hdr *ah;
  struct pico_ip4 *src;
  int ret;

  src = pico_ipv4_source_find(dst);
  if (!src)
    return -1;

  dbg("QUERY: %08x\n", dst->addr);

  if (!q)
    return -1;
  eh = (struct pico_eth_hdr *)q->start;
  ah = (struct pico_arp_hdr *) (q->start + PICO_SIZE_ETHHDR);

  /* Fill eth header */
  memcpy(eh->saddr, dev->eth->mac.addr, PICO_SIZE_ETH);
  memcpy(eh->daddr, PICO_ETHADDR_ANY, PICO_SIZE_ETH);
  eh->proto = PICO_IDETH_ARP;

  /* Fill arp header */
  ah->htype  = PICO_ARP_HTYPE_ETH;
  ah->ptype  = PICO_IDETH_IPV4;
  ah->hsize  = PICO_SIZE_ETH;
  ah->psize  = PICO_SIZE_IP4;
  ah->opcode = PICO_ARP_REQUEST;
  memcpy(ah->s_mac, dev->eth->mac.addr, PICO_SIZE_ETH);
  ah->src.addr = src->addr;
  ah->dst.addr = dst->addr;
  dbg("Sending arp query.\n");
  ret = dev->send(dev, q->start, q->len);
  pico_frame_discard(q);
  return ret;
}

#ifdef UNIT_ARPTABLE

int main(void)
{
  struct pico_arp test1, test2, test3;
  struct pico_arp *found, *notfound;
  struct pico_frame *f = pico_frame_alloc(40);
  struct pico_ipv4_hdr *iphdr;
  f->net_hdr = f->start;
  iphdr = (struct pico_ipv4_hdr *) f->net_hdr;
  iphdr->dst.addr = 3;

  memset(test1.eth.addr, 1, 6);
  memset(test2.eth.addr, 2, 6);
  memset(test3.eth.addr, 3, 6);

  test1.ipv4.addr = 0x1;
  test2.ipv4.addr = 0x2;
  test3.ipv4.addr = 0x3; 

  RB_INSERT(arp_tree, &Arp_table, &test1);
  RB_INSERT(arp_tree, &Arp_table, &test2);
  RB_INSERT(arp_tree, &Arp_table, &test3);

  found = pico_arp_get(&f->dst);
  iphdr->dst.addr = 4;
  notfound = pico_arp_get(&f->dst);

  if (found && !notfound)
    return 0;
  return 5;
}

#endif

