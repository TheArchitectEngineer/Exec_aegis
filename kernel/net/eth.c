/* kernel/net/eth.c — Ethernet framing, ARP table, ARP send/resolve */
#include "eth.h"
#include "ip.h"     /* ip_rx(), net_get_config() */
#include "arch.h"   /* arch_get_ticks() */
#include "printk.h"
#include "spinlock.h"
#include <stddef.h>

/* Local memory helpers — kernel does not link against libc. */
static void
_eth_memset(void *dst, int val, uint32_t n)
{
    uint8_t *p = (uint8_t *)dst;
    while (n--) *p++ = (uint8_t)val;
}

static void
_eth_memcpy(void *dst, const void *src, uint32_t n)
{
    const uint8_t *s = (const uint8_t *)src;
    uint8_t       *d = (uint8_t *)dst;
    while (n--) *d++ = *s++;
}

/* ---- ARP table --------------------------------------------------------- */

#define ARP_TABLE_SIZE 16

typedef struct {
    ip4_addr_t ip;
    mac_addr_t mac;
    uint32_t   age;    /* PIT ticks since last use; evict oldest */
    uint8_t    valid;
    uint8_t    resolved; /* S5: 0 = pending request, 1 = reply received */
} arp_entry_t;

static arp_entry_t s_arp_table[ARP_TABLE_SIZE];

/* Shared static TX buffer — callers are sequential (no concurrent sends). */
static uint8_t s_tx_buf[1514];
static spinlock_t arp_lock = SPINLOCK_INIT;

void eth_init(void)
{
    _eth_memset(s_arp_table, 0, sizeof(s_arp_table));
}

/* ---- ARP helpers ------------------------------------------------------- */

static arp_entry_t *arp_find(ip4_addr_t ip)
{
    int i;
    for (i = 0; i < ARP_TABLE_SIZE; i++) {
        if (s_arp_table[i].valid && s_arp_table[i].ip == ip)
            return &s_arp_table[i];
    }
    return NULL;
}

/* S5: Insert a pending (unresolved) ARP entry so that arp_rx_pkt only
 * updates entries we actually requested.  If an entry already exists for
 * this IP, leave it alone. */
static void arp_insert_pending(ip4_addr_t ip)
{
    int i;
    /* Already have an entry? Don't overwrite. */
    for (i = 0; i < ARP_TABLE_SIZE; i++) {
        if (s_arp_table[i].valid && s_arp_table[i].ip == ip)
            return;
    }
    /* Find a free slot (prefer free over evicting). */
    for (i = 0; i < ARP_TABLE_SIZE; i++) {
        if (!s_arp_table[i].valid) {
            s_arp_table[i].ip       = ip;
            _eth_memset(&s_arp_table[i].mac, 0, sizeof(mac_addr_t));
            s_arp_table[i].age      = 0;
            s_arp_table[i].valid    = 1;
            s_arp_table[i].resolved = 0;
            return;
        }
    }
    /* Table full — evict oldest. */
    uint32_t oldest = 0;
    int oldest_idx = 0;
    for (i = 0; i < ARP_TABLE_SIZE; i++) {
        if (s_arp_table[i].age >= oldest) {
            oldest = s_arp_table[i].age;
            oldest_idx = i;
        }
    }
    s_arp_table[oldest_idx].ip       = ip;
    _eth_memset(&s_arp_table[oldest_idx].mac, 0, sizeof(mac_addr_t));
    s_arp_table[oldest_idx].age      = 0;
    s_arp_table[oldest_idx].valid    = 1;
    s_arp_table[oldest_idx].resolved = 0;
}

static void arp_send_request(netdev_t *dev, ip4_addr_t target_ip)
{
    ip4_addr_t  my_ip;
    mac_addr_t  my_mac;
    arp_pkt_t   pkt;
    mac_addr_t  bcast;

    /* S5: Create a pending entry before sending the request so that
     * arp_rx_pkt will only update entries we actually requested. */
    arp_insert_pending(target_ip);

    net_get_config(&my_ip, NULL, NULL);  /* only need local IP; mask/gw unused here */
    /* dev->mac is uint8_t[6]; copy into mac_addr_t (same layout). */
    _eth_memcpy(my_mac.b, dev->mac, 6);

    pkt.htype = htons(1);
    pkt.ptype = htons(ETHERTYPE_IP);
    pkt.hlen  = 6;
    pkt.plen  = 4;
    pkt.oper  = htons(1);       /* REQUEST */
    pkt.sha   = my_mac;
    pkt.spa   = my_ip;
    _eth_memset(&pkt.tha, 0, sizeof(pkt.tha));
    pkt.tpa   = target_ip;

    _eth_memset(bcast.b, 0xff, 6);
    eth_send(dev, &bcast, ETHERTYPE_ARP, &pkt, sizeof(pkt));
}

/* Called from eth_rx for ARP frames (inside and outside busy-poll). */
static void arp_rx_pkt(const arp_pkt_t *pkt)
{
    if (ntohs(pkt->htype) != 1)      return;
    if (ntohs(pkt->ptype) != 0x0800) return;
    if (pkt->hlen != 6 || pkt->plen != 4) return;
    if (ntohs(pkt->oper) != 2)       return;  /* only cache REPLY */

    /* S5: Only update ARP cache for entries with pending requests.
     * Reject unsolicited ARP replies to prevent cache poisoning. */
    arp_entry_t *e = arp_find(pkt->spa);
    if (!e) return;  /* no pending entry — unsolicited reply, drop */
    e->mac      = pkt->sha;
    e->age      = 0;
    e->resolved = 1;
}

/* ---- eth_rx / eth_send ------------------------------------------------- */

void eth_rx(netdev_t *dev, const void *frame, uint16_t len)
{
    const eth_hdr_t *hdr;
    const void      *payload;
    uint16_t         payload_len;
    uint16_t         et;

    if (len < (uint16_t)sizeof(eth_hdr_t)) return;
    hdr         = (const eth_hdr_t *)frame;
    payload     = (const uint8_t *)frame + sizeof(eth_hdr_t);
    payload_len = (uint16_t)(len - sizeof(eth_hdr_t));
    et          = ntohs(hdr->ethertype);

    if (et == ETHERTYPE_ARP && payload_len >= (uint16_t)sizeof(arp_pkt_t)) {
        arp_rx_pkt((const arp_pkt_t *)payload);
    } else if (et == ETHERTYPE_IP) {
        ip_rx(dev, frame, payload, payload_len);
    }
    /* Other ethertypes: drop silently */
}

int eth_send(netdev_t *dev, const mac_addr_t *dst_mac,
             uint16_t ethertype, const void *payload, uint16_t len)
{
    eth_hdr_t *hdr;
    uint16_t   total;
    mac_addr_t src_mac;

    if (!dev || len > 1500) return -1;

    irqflags_t fl = spin_lock_irqsave(&arp_lock);
    hdr = (eth_hdr_t *)s_tx_buf;
    hdr->dst = *dst_mac;
    /* dev->mac is uint8_t[6]; copy into mac_addr_t src field. */
    _eth_memcpy(src_mac.b, dev->mac, 6);
    hdr->src       = src_mac;
    hdr->ethertype = htons(ethertype);
    _eth_memcpy(s_tx_buf + sizeof(eth_hdr_t), payload, len);

    total = (uint16_t)(sizeof(eth_hdr_t) + len);
    dev->send(dev, s_tx_buf, total);
    spin_unlock_irqrestore(&arp_lock, fl);
    return 0;
}

/* ---- arp_resolve ------------------------------------------------------- */

/* Set by netdev_poll_all — when 1, we're inside the PIT ISR RX path
 * and arp_resolve must not block (would deadlock on netdev_lock). */
extern volatile int g_in_netdev_poll;

int arp_resolve(netdev_t *dev, ip4_addr_t ip, mac_addr_t *mac_out)
{
    arp_entry_t *e = arp_find(ip);
    if (e && e->resolved) {
        e->age = 0;
        *mac_out = e->mac;
        return 0;
    }

    /* Send ARP request regardless of context. */
    arp_send_request(dev, ip);

    /* If called from the PIT ISR RX path (netdev_poll_all → virtio_net_poll
     * → tcp_rx → tcp_send_segment → ip_send), we CANNOT block.  Blocking
     * with arch_wait_for_irq() while holding netdev_lock + tcp_lock causes
     * a permanent deadlock: the next PIT tick tries to acquire those locks.
     * Return -1 so the TCP retransmit timer re-sends on the next tick.
     *
     * We check g_in_netdev_poll (not IF flag) because syscall-context callers
     * like tcp_connect also hold spinlocks with IRQs disabled — those callers
     * CAN safely block here since they're not in the ISR chain. */
    if (g_in_netdev_poll) {
        return -1;  /* caller should retry later */
    }

    /* Syscall/task context — safe to block and wait for ARP reply.
     *
     * On TCG QEMU, the guest vCPU and SLIRP share a thread.  "sti; hlt; cli"
     * yields to QEMU so SLIRP can generate the ARP reply.  The PIT ISR
     * calls netdev_poll_all() which delivers pending RX frames. */
    {
        uint32_t n;
        for (n = 0; n < 500u; n++) {
            arch_wait_for_irq();
            if (dev->poll)
                dev->poll(dev);
            e = arp_find(ip);
            if (e && e->resolved) {
                arch_enable_irq();
                *mac_out = e->mac;
                return 0;
            }
        }
    }
    arch_enable_irq();
    return -1;
}
