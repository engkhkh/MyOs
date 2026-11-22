/* net.c - MyOS network stack: Ethernet, ARP, IPv4, ICMP, UDP, DHCP, DNS, TCP.
 *
 * How a packet travels out:  data -> TCP/UDP header -> IP header -> Ethernet
 * header -> network card. ARP finds the Ethernet (MAC) address of the next
 * machine (our router, or the target if it's on our local network).
 * Incoming packets are peeled the other way round.
 *
 * Everything here runs inside the kernel's main loop (net_poll / net_timer) or
 * inside a system call, never both at once, so no locking is needed. Nothing in
 * here ever waits: lookups and connections are started, then checked later.
 *
 * All addresses are kept in normal ("host") order: 10.0.2.15 = 0x0A00020F.
 */
#include <stdint.h>
#include <stddef.h>
#include "kernel.h"

/* ---------- byte helpers (network order is big-endian) ---------- */
static uint16_t g16(const uint8_t *p) { return p[0] << 8 | p[1]; }
static uint32_t g32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
static void p16(uint8_t *p, uint16_t v) { p[0] = v >> 8; p[1] = v; }
static void p32(uint8_t *p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }
static void mcpy(void *d, const void *s, uint32_t n) { uint8_t *a = d; const uint8_t *b = s; while (n--) *a++ = *b++; }
static void mset(void *d, int c, uint32_t n) { uint8_t *a = d; while (n--) *a++ = (uint8_t)c; }
static int meq(const void *a, const void *b, uint32_t n) { const uint8_t *x = a, *y = b; while (n--) if (*x++ != *y++) return 0; return 1; }

static uint32_t csum_add(uint32_t s, const uint8_t *p, uint32_t n) {
    for (uint32_t i = 0; i + 1 < n; i += 2) s += g16(p + i);
    if (n & 1) s += p[n - 1] << 8;
    return s;
}
static uint16_t csum_fin(uint32_t s) { while (s >> 16) s = (s & 0xFFFF) + (s >> 16); return (uint16_t)~s; }

/* ---------- configuration & stats ---------- */
struct net_info netcfg;
static uint8_t mac[6];
static const uint8_t BCAST[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
static uint16_t ip_id = 1;
static uint8_t out[1600];                 /* frame being built */

static uint32_t now_ms(void) { return ticks * (1000 / TIMER_HZ); }

static void send_frame(const uint8_t *dst, uint16_t type, uint32_t payload_len) {
    mcpy(out, dst, 6); mcpy(out + 6, mac, 6); p16(out + 12, type);
    uint32_t len = 14 + payload_len;
    if (len < 60) { mset(out + len, 0, 60 - len); len = 60; }
    if (e1000_send(out, len)) netcfg.tx_packets++;
}

/* ================= ARP ================= */
struct arp_entry { uint32_t ip; uint8_t mac[6]; uint32_t when; int used; };
static struct arp_entry arp[16];
struct pending { uint32_t hop; uint32_t len, when; uint16_t type; uint8_t data[1600]; int used; };
static struct pending pend[4];            /* packets waiting for an ARP answer */

static struct arp_entry *arp_find(uint32_t ip) {
    for (int i = 0; i < 16; i++) if (arp[i].used && arp[i].ip == ip) return &arp[i];
    return 0;
}
static void arp_learn(uint32_t ip, const uint8_t *m) {
    if (!ip) return;
    struct arp_entry *e = arp_find(ip);
    if (!e) { int old = 0; for (int i = 0; i < 16; i++) { if (!arp[i].used) { old = i; break; } if (arp[i].when < arp[old].when) old = i; } e = &arp[old]; }
    e->ip = ip; mcpy(e->mac, m, 6); e->when = now_ms(); e->used = 1;
    for (int i = 0; i < 4; i++)                       /* send anything that was waiting for it */
        if (pend[i].used && pend[i].hop == ip) {
            mcpy(out + 14, pend[i].data, pend[i].len);
            send_frame(m, pend[i].type, pend[i].len);
            pend[i].used = 0;
        }
}
static void arp_send(uint16_t op, const uint8_t *tmac, uint32_t tip, const uint8_t *dst) {
    uint8_t *a = out + 14;
    p16(a, 1); p16(a + 2, 0x0800); a[4] = 6; a[5] = 4; p16(a + 6, op);
    mcpy(a + 8, mac, 6); p32(a + 14, netcfg.ip);
    mcpy(a + 18, tmac, 6); p32(a + 24, tip);
    send_frame(dst, 0x0806, 28);
}
static void arp_rx(const uint8_t *a, int len) {
    if (len < 28 || g16(a) != 1 || g16(a + 2) != 0x0800) return;
    uint32_t sip = g32(a + 14), tip = g32(a + 24);
    arp_learn(sip, a + 8);
    if (g16(a + 6) == 1 && netcfg.ip && tip == netcfg.ip) arp_send(2, a + 8, sip, a + 8);   /* "that's me" */
}

/* ================= IPv4 ================= */
/* Build an IP packet around payload (already at out+14+20) and send it. */
static void ip_send(uint32_t dst, uint8_t proto, uint32_t plen) {
    uint8_t *h = out + 14;
    h[0] = 0x45; h[1] = 0; p16(h + 2, 20 + plen); p16(h + 4, ip_id++); p16(h + 6, 0x4000);
    h[8] = 64; h[9] = proto; p16(h + 10, 0); p32(h + 12, netcfg.ip); p32(h + 16, dst);
    p16(h + 10, csum_fin(csum_add(0, h, 20)));
    if (dst == 0xFFFFFFFF) { send_frame(BCAST, 0x0800, 20 + plen); return; }
    uint32_t hop = (netcfg.mask && (dst & netcfg.mask) == (netcfg.ip & netcfg.mask)) ? dst : netcfg.gateway;
    if (!hop) return;
    struct arp_entry *e = arp_find(hop);
    if (e) { send_frame(e->mac, 0x0800, 20 + plen); return; }
    int slot = 0;                                      /* park it until ARP answers */
    for (int i = 0; i < 4; i++) { if (!pend[i].used) { slot = i; break; } if (pend[i].when < pend[slot].when) slot = i; }
    pend[slot].used = 1; pend[slot].hop = hop; pend[slot].len = 20 + plen; pend[slot].when = now_ms(); pend[slot].type = 0x0800;
    mcpy(pend[slot].data, out + 14, 20 + plen);
    static const uint8_t zero[6] = { 0 };
    arp_send(1, zero, hop, BCAST);
}

/* ================= ICMP (ping) ================= */
static uint16_t ping_id = 0x4D59;                      /* "MY" */
static uint16_t ping_seq;
static uint32_t ping_sent_at[16], ping_rtt[16];
static int ping_got[16];

static void ping_send(uint32_t dst) {
    uint8_t *m = out + 14 + 20;
    int seq = ping_seq++;
    m[0] = 8; m[1] = 0; p16(m + 2, 0); p16(m + 4, ping_id); p16(m + 6, seq);
    for (int i = 0; i < 32; i++) m[8 + i] = 'a' + i % 26;
    p16(m + 2, csum_fin(csum_add(0, m, 40)));
    ping_sent_at[seq % 16] = now_ms(); ping_got[seq % 16] = 0;
    ip_send(dst, 1, 40);
}
static void icmp_rx(uint32_t src, const uint8_t *m, int len) {
    if (len < 8) return;
    if (m[0] == 8) {                                   /* someone pings us: answer */
        if (len > 1400) return;
        uint8_t *r = out + 14 + 20;
        mcpy(r, m, len); r[0] = 0; p16(r + 2, 0);
        p16(r + 2, csum_fin(csum_add(0, r, len)));
        ip_send(src, 1, len);
    } else if (m[0] == 0 && g16(m + 4) == ping_id) {   /* answer to our ping */
        int s = g16(m + 6) % 16;
        ping_got[s] = 1; ping_rtt[s] = now_ms() - ping_sent_at[s];
    }
}

/* ================= UDP ================= */
static void udp_send(uint32_t dst, uint16_t sport, uint16_t dport, uint32_t len) {
    uint8_t *u = out + 14 + 20;
    p16(u, sport); p16(u + 2, dport); p16(u + 4, 8 + len); p16(u + 6, 0);   /* checksum optional in IPv4 */
    ip_send(dst, 17, 8 + len);
}

/* ================= DHCP (getting an address automatically) ================= */
static uint32_t dhcp_xid, dhcp_offer, dhcp_server, dhcp_last;
static int dhcp_state;                                 /* 0 start, 1 sent discover, 2 sent request, 3 bound */

static void dhcp_send(int type) {
    uint8_t *d = out + 14 + 20 + 8;
    mset(d, 0, 300);
    d[0] = 1; d[1] = 1; d[2] = 6; p32(d + 4, dhcp_xid); p16(d + 10, 0x8000);   /* please answer by broadcast */
    mcpy(d + 28, mac, 6);
    p32(d + 236, 0x63825363);                          /* "magic cookie" */
    uint8_t *o = d + 240;
    *o++ = 53; *o++ = 1; *o++ = (uint8_t)type;          /* message type */
    *o++ = 61; *o++ = 7; *o++ = 1; mcpy(o, mac, 6); o += 6;
    *o++ = 12; *o++ = 4; mcpy(o, "MyOS", 4); o += 4;   /* host name */
    if (type == 3) { *o++ = 50; *o++ = 4; p32(o, dhcp_offer); o += 4; *o++ = 54; *o++ = 4; p32(o, dhcp_server); o += 4; }
    *o++ = 55; *o++ = 3; *o++ = 1; *o++ = 3; *o++ = 6;   /* ask for: mask, router, DNS */
    *o++ = 255;
    uint32_t saved = netcfg.ip; netcfg.ip = 0;          /* DHCP is sent from 0.0.0.0 */
    udp_send(0xFFFFFFFF, 68, 67, (uint32_t)(o - d) < 300 ? 300 : (uint32_t)(o - d));
    netcfg.ip = saved;
    dhcp_last = now_ms();
}

static void dhcp_rx(const uint8_t *d, int len) {
    if (len < 240 || d[0] != 2 || g32(d + 4) != dhcp_xid || g32(d + 236) != 0x63825363) return;
    int type = 0; uint32_t mask = 0, router = 0, dns = 0, server = 0;
    for (int i = 240; i < len && d[i] != 255;) {
        if (d[i] == 0) { i++; continue; }
        int code = d[i], l = d[i + 1];
        const uint8_t *v = d + i + 2;
        if (i + 2 + l > len) break;
        if (code == 53) type = v[0];
        if (code == 1 && l >= 4) mask = g32(v);
        if (code == 3 && l >= 4) router = g32(v);
        if (code == 6 && l >= 4) dns = g32(v);
        if (code == 54 && l >= 4) server = g32(v);
        i += 2 + l;
    }
    if (type == 2 && dhcp_state == 1) {                /* OFFER: ask for it */
        dhcp_offer = g32(d + 16); dhcp_server = server;
        dhcp_state = 2; dhcp_send(3);
    } else if (type == 5 && dhcp_state == 2) {         /* ACK: it's ours */
        netcfg.ip = g32(d + 16); netcfg.mask = mask; netcfg.gateway = router; netcfg.dns = dns ? dns : router;
        dhcp_state = 3;
    } else if (type == 6) {                            /* NAK: start over */
        dhcp_state = 0;
    }
}

/* ================= DNS (names -> addresses) ================= */
struct dns_entry { char name[64]; uint32_t ip, when, sent; int state, tries; };   /* state 1 asking, 2 known, 3 failed */
static struct dns_entry dns[8];

static int parse_ip(const char *s, uint32_t *ip) {
    uint32_t v = 0; int parts = 0, n = -1;
    for (;; s++) {
        if (*s >= '0' && *s <= '9') { n = (n < 0 ? 0 : n) * 10 + *s - '0'; if (n > 255) return 0; }
        else if (*s == '.' || !*s) { if (n < 0) return 0; v = v << 8 | n; parts++; n = -1; if (!*s) break; }
        else return 0;
    }
    if (parts != 4) return 0;
    *ip = v; return 1;
}

static void dns_query(int i) {
    struct dns_entry *e = &dns[i];
    uint8_t *q = out + 14 + 20 + 8;
    p16(q, 0x4D00 + i); p16(q + 2, 0x0100); p16(q + 4, 1); p16(q + 6, 0); p16(q + 8, 0); p16(q + 10, 0);
    uint8_t *p = q + 12;
    const char *s = e->name;
    while (*s) {                                       /* "www.example.com" -> 3www7example3com0 */
        uint8_t *lenp = p++; int n = 0;
        while (*s && *s != '.') { *p++ = *s++; n++; }
        *lenp = (uint8_t)n;
        if (*s == '.') s++;
    }
    *p++ = 0;
    p16(p, 1); p16(p + 2, 1); p += 4;                  /* type A, class IN */
    e->sent = now_ms(); e->tries++;
    udp_send(netcfg.dns, 0xC000 + i, 53, p - q);
}

static int skip_name(const uint8_t *m, int len, int i) {
    while (i < len) {
        if (m[i] == 0) return i + 1;
        if ((m[i] & 0xC0) == 0xC0) return i + 2;      /* compressed: a pointer */
        i += m[i] + 1;
    }
    return len;
}
static void dns_rx(uint16_t port, const uint8_t *m, int len) {
    int i = port - 0xC000;
    if (i < 0 || i >= 8 || len < 12 || g16(m) != 0x4D00 + i || dns[i].state != 1) return;
    int an = g16(m + 6), rcode = m[3] & 15;
    if (rcode) { dns[i].state = 3; dns[i].when = now_ms(); return; }
    int p = 12;
    for (int q = g16(m + 4); q > 0; q--) p = skip_name(m, len, p) + 4;
    for (; an > 0 && p < len; an--) {
        p = skip_name(m, len, p);
        if (p + 10 > len) break;
        int type = g16(m + p), rdlen = g16(m + p + 8);
        if (type == 1 && rdlen == 4 && p + 14 <= len) { dns[i].ip = g32(m + p + 10); dns[i].state = 2; dns[i].when = now_ms(); return; }
        p += 10 + rdlen;
    }
    dns[i].state = 3; dns[i].when = now_ms();
}

/* Look a name up. Returns 1 (found, *ip set), 0 (still asking) or -1 (failed). */
int net_resolve(const char *name, uint32_t *ip) {
    if (parse_ip(name, ip)) return 1;
    if (!netcfg.ip || !netcfg.dns) return -1;
    int k = 0; while (name[k]) k++;
    if (!k || k > 63) return -1;
    int free_i = -1, oldest = 0;
    for (int i = 0; i < 8; i++) {
        struct dns_entry *e = &dns[i];
        if (e->state && !kstrcmp(e->name, name)) {
            if (e->state == 2) { *ip = e->ip; return 1; }
            if (e->state == 3) { if (now_ms() - e->when < 5000) return -1; e->state = 0; free_i = i; break; }
            if (now_ms() - e->sent > 1000) { if (e->tries >= 4) { e->state = 3; e->when = now_ms(); return -1; } dns_query(i); }
            return 0;
        }
        if (!e->state && free_i < 0) free_i = i;
        if (e->when < dns[oldest].when) oldest = i;
    }
    if (free_i < 0) free_i = oldest;
    struct dns_entry *e = &dns[free_i];
    mset(e, 0, sizeof *e);
    for (int j = 0; j <= k; j++) e->name[j] = name[j];
    e->state = 1;
    dns_query(free_i);
    return 0;
}

/* ================= TCP ================= */
#define NSOCK 8
#define RXBUF 65536
#define TXBUF 16384
enum { S_FREE, S_SYN_SENT, S_OPEN, S_PEER_CLOSED, S_FAILED };
struct sock {
    int state, owner, fin_sent, tries;
    uint32_t rip; uint16_t rport, lport, peer_win;
    uint32_t iss, snd_una, snd_nxt, rcv_nxt, last_send;
    uint8_t *rx; uint32_t rx_head, rx_len;        /* received data waiting for the program */
    uint8_t *tx; uint32_t tx_len;                 /* data from snd_una onward (sent or not) */
};
static struct sock socks[NSOCK];
static uint16_t next_port = 49152;

static void tcp_segment(struct sock *s, uint8_t flags, uint32_t seq, const uint8_t *data, uint32_t len) {
    uint8_t *t = out + 14 + 20;
    int hl = (flags & 0x02) ? 24 : 20;               /* SYN carries the MSS option */
    p16(t, s->lport); p16(t + 2, s->rport); p32(t + 4, seq); p32(t + 8, s->rcv_nxt);
    t[12] = (uint8_t)((hl / 4) << 4); t[13] = flags;
    uint32_t space = RXBUF - s->rx_len;
    p16(t + 14, space > 65535 ? 65535 : space); p16(t + 16, 0); p16(t + 18, 0);
    if (hl == 24) { t[20] = 2; t[21] = 4; p16(t + 22, 1460); }
    if (len) mcpy(t + hl, data, len);
    uint8_t ph[12];                                   /* checksum covers a "pseudo header" too */
    p32(ph, netcfg.ip); p32(ph + 4, s->rip); ph[8] = 0; ph[9] = 6; p16(ph + 10, hl + len);
    p16(t + 16, csum_fin(csum_add(csum_add(0, ph, 12), t, hl + len)));
    ip_send(s->rip, 6, hl + len);
    s->last_send = now_ms();
}
#define F_FIN 0x01
#define F_SYN 0x02
#define F_RST 0x04
#define F_PSH 0x08
#define F_ACK 0x10

/* Send whatever is queued and allowed by the receiver's window. */
static void tcp_output(struct sock *s) {
    if (s->state != S_OPEN && s->state != S_PEER_CLOSED) return;
    for (;;) {
        uint32_t inflight = s->snd_nxt - s->snd_una;
        if (inflight >= s->tx_len) break;
        uint32_t win = s->peer_win ? s->peer_win : 1;
        if (inflight >= win) break;
        uint32_t n = s->tx_len - inflight;
        if (n > 1460) n = 1460;
        if (n > win - inflight) n = win - inflight;
        tcp_segment(s, F_ACK | F_PSH, s->snd_nxt, s->tx + inflight, n);
        s->snd_nxt += n;
    }
}

static void tcp_rx(uint32_t src, const uint8_t *t, int len) {
    if (len < 20) return;
    uint16_t sport = g16(t), dport = g16(t + 2);
    struct sock *s = 0;
    for (int i = 0; i < NSOCK; i++)
        if (socks[i].state != S_FREE && socks[i].rip == src && socks[i].rport == sport && socks[i].lport == dport) s = &socks[i];
    if (!s) return;
    uint32_t seq = g32(t + 4), ack = g32(t + 8);
    int hl = (t[12] >> 4) * 4; uint8_t fl = t[13];
    if (hl < 20 || hl > len) return;
    const uint8_t *data = t + hl; uint32_t dlen = len - hl;
    s->peer_win = g16(t + 14);
    if (fl & F_RST) { s->state = S_FAILED; return; }

    if (s->state == S_SYN_SENT) {
        if ((fl & (F_SYN | F_ACK)) == (F_SYN | F_ACK) && ack == s->iss + 1) {
            s->rcv_nxt = seq + 1; s->snd_una = s->snd_nxt = ack;
            s->state = S_OPEN; s->tries = 0;
            tcp_segment(s, F_ACK, s->snd_nxt, 0, 0);
            tcp_output(s);
        }
        return;
    }
    if (s->state == S_FAILED) return;
    if (fl & F_ACK) {                                 /* the other side got our data up to "ack" */
        uint32_t acked = ack - s->snd_una;
        uint32_t limit = s->snd_nxt - s->snd_una + (s->fin_sent ? 1 : 0);
        if (acked && acked <= limit) {
            uint32_t drop = acked > s->tx_len ? s->tx_len : acked;
            for (uint32_t i = drop; i < s->tx_len; i++) s->tx[i - drop] = s->tx[i];
            s->tx_len -= drop;
            s->snd_una = ack;
            if (s->snd_nxt - s->snd_una > 0x80000000u) s->snd_nxt = s->snd_una;
            s->tries = 0;
        }
    }
    int need_ack = 0;
    if (dlen) {
        if (seq == s->rcv_nxt) {                      /* the next bytes in order: keep them */
            uint32_t room = RXBUF - s->rx_len, n = dlen < room ? dlen : room;
            for (uint32_t i = 0; i < n; i++) s->rx[(s->rx_head + s->rx_len + i) % RXBUF] = data[i];
            s->rx_len += n; s->rcv_nxt += n;
            if (n == dlen && (fl & F_FIN)) { s->rcv_nxt++; s->state = S_PEER_CLOSED; }
        }
        need_ack = 1;                                 /* (out of order: re-ack what we have) */
    } else if ((fl & F_FIN) && seq == s->rcv_nxt) {
        s->rcv_nxt++; s->state = S_PEER_CLOSED; need_ack = 1;
    }
    if (need_ack) tcp_segment(s, F_ACK, s->snd_nxt, 0, 0);
    tcp_output(s);
}

int tcp_open(uint32_t ip, uint16_t port, int owner) {
    if (!netcfg.ip) return -1;
    for (int i = 0; i < NSOCK; i++) {
        struct sock *s = &socks[i];
        if (s->state != S_FREE) continue;
        uint8_t *rx = s->rx ? s->rx : kmalloc(RXBUF), *tx = s->tx ? s->tx : kmalloc(TXBUF);
        if (!rx || !tx) return -1;
        mset(s, 0, sizeof *s);
        s->rx = rx; s->tx = tx;
        s->rip = ip; s->rport = port; s->lport = next_port++; if (next_port > 65000) next_port = 49152;
        s->owner = owner; s->peer_win = 1460;
        s->iss = ticks * 2654435761u ^ (i << 20);
        s->snd_una = s->snd_nxt = s->iss + 1;
        s->state = S_SYN_SENT;
        tcp_segment(s, F_SYN, s->iss, 0, 0);
        s->tries = 1;
        return i;
    }
    return -1;
}

static struct sock *sock_of(int i, int owner) {
    if (i < 0 || i >= NSOCK || socks[i].state == S_FREE || socks[i].owner != owner) return 0;
    return &socks[i];
}
/* 0 connecting, 1 open, 2 the other side finished sending, -1 failed */
int tcp_status(int i, int owner) {
    struct sock *s = sock_of(i, owner);
    if (!s) return -1;
    return s->state == S_SYN_SENT ? 0 : s->state == S_OPEN ? 1 : s->state == S_PEER_CLOSED ? 2 : -1;
}
int tcp_send(int i, int owner, const void *data, uint32_t len) {
    struct sock *s = sock_of(i, owner);
    if (!s || (s->state != S_OPEN && s->state != S_PEER_CLOSED)) return -1;
    uint32_t room = TXBUF - s->tx_len, n = len < room ? len : room;
    mcpy(s->tx + s->tx_len, data, n);
    s->tx_len += n;
    tcp_output(s);
    return (int)n;
}
/* Bytes received, 0 = nothing yet, -1 = connection over and nothing left. */
int tcp_recv(int i, int owner, void *buf, uint32_t max) {
    struct sock *s = sock_of(i, owner);
    if (!s) return -1;
    if (!s->rx_len) return (s->state == S_PEER_CLOSED || s->state == S_FAILED) ? -1 : 0;
    uint32_t n = s->rx_len < max ? s->rx_len : max;
    uint8_t *o = buf;
    int was_full = s->rx_len > RXBUF / 2;
    for (uint32_t k = 0; k < n; k++) o[k] = s->rx[(s->rx_head + k) % RXBUF];
    s->rx_head = (s->rx_head + n) % RXBUF; s->rx_len -= n;
    if (was_full && s->state == S_OPEN) tcp_segment(s, F_ACK, s->snd_nxt, 0, 0);   /* window opened up again */
    return (int)n;
}
void tcp_close(int i, int owner) {
    struct sock *s = sock_of(i, owner);
    if (!s) return;
    if (s->state == S_OPEN || s->state == S_PEER_CLOSED) tcp_segment(s, F_FIN | F_ACK, s->snd_nxt, 0, 0);
    else if (s->state == S_SYN_SENT) tcp_segment(s, F_RST, s->snd_nxt, 0, 0);
    s->state = S_FREE;                                /* buffers are kept for reuse */
}
void net_close_owner(int owner) { for (int i = 0; i < NSOCK; i++) if (socks[i].state != S_FREE && socks[i].owner == owner) tcp_close(i, owner); }

/* ================= receive path & timers ================= */
static uint8_t in[2048];

static void ip_rx(const uint8_t *h, int len) {
    if (len < 20 || (h[0] >> 4) != 4) return;
    int hl = (h[0] & 15) * 4, tot = g16(h + 2);
    if (hl < 20 || tot > len || tot < hl) return;
    if (g16(h + 6) & 0x3FFF) return;                 /* fragments: not supported */
    uint32_t src = g32(h + 12), dst = g32(h + 16);
    if (netcfg.ip && dst != netcfg.ip && dst != 0xFFFFFFFF) return;
    const uint8_t *p = h + hl; int plen = tot - hl;
    if (h[9] == 1) icmp_rx(src, p, plen);
    else if (h[9] == 6) tcp_rx(src, p, plen);
    else if (h[9] == 17 && plen >= 8) {
        uint16_t dport = g16(p + 2);
        int ulen = g16(p + 4) - 8; if (ulen > plen - 8) ulen = plen - 8;
        if (dport == 68) dhcp_rx(p + 8, ulen);
        else if (dport >= 0xC000 && dport < 0xC008) dns_rx(dport, p + 8, ulen);
    }
}

/* Handle every packet that has arrived. Called from the kernel's main loop. */
void net_poll(void) {
    int n;
    while ((n = e1000_receive(in)) > 0) {
        netcfg.rx_packets++;
        if (n < 14) continue;
        if (!meq(in, mac, 6) && !meq(in, BCAST, 6)) continue;
        uint16_t type = g16(in + 12);
        if (type == 0x0806) arp_rx(in + 14, n - 14);
        else if (type == 0x0800) ip_rx(in + 14, n - 14);
    }
}

/* Retries and time-outs, about ten times a second. */
void net_timer(void) {
    static uint32_t last;
    uint32_t t = now_ms();
    if (t - last < 100) return;
    last = t;
    if (!e1000_present()) return;
    if (dhcp_state != 3) {
        uint32_t wait = dhcp_state == 0 ? 0 : 2000;
        if (t - dhcp_last >= wait) { dhcp_xid = t * 2654435761u ^ 0x4D794F53; dhcp_state = 1; dhcp_send(1); }
    }
    for (int i = 0; i < 4; i++) if (pend[i].used && t - pend[i].when > 3000) pend[i].used = 0;
    for (int i = 0; i < NSOCK; i++) {
        struct sock *s = &socks[i];
        if (s->state == S_SYN_SENT && t - s->last_send > 1000) {
            if (s->tries >= 6) { s->state = S_FAILED; continue; }
            tcp_segment(s, F_SYN, s->iss, 0, 0); s->tries++;
        } else if ((s->state == S_OPEN || s->state == S_PEER_CLOSED) && s->snd_nxt != s->snd_una && t - s->last_send > 1000) {
            if (s->tries >= 8) { s->state = S_FAILED; continue; }
            s->snd_nxt = s->snd_una; s->tries++;      /* resend everything not yet acknowledged */
            tcp_output(s);
        }
    }
}

int net_init(void) {
    if (!e1000_init()) return 0;
    e1000_mac(mac);
    for (int i = 0; i < 6; i++) netcfg.mac[i] = mac[i];
    netcfg.present = 1;
    return 1;
}

/* ---------- ping & nslookup for the Terminal (run in the background) ---------- */
static struct { int active, kind, sent, replies, reported; char host[64]; uint32_t ip, next; } job;   /* kind 1 ping, 2 nslookup */

static void ip_text(uint32_t ip, char *b) {
    int k = 0;
    for (int i = 3; i >= 0; i--) {
        int v = (ip >> (i * 8)) & 255; char t[4]; int n = 0;
        do t[n++] = '0' + v % 10; while (v /= 10);
        while (n) b[k++] = t[--n];
        if (i) b[k++] = '.';
    }
    b[k] = 0;
}
void net_ip_text(uint32_t ip, char *b) { ip_text(ip, b); }

int net_job_start(int kind, const char *host) {
    if (!netcfg.ip) return 0;
    mset(&job, 0, sizeof job);
    int k = 0; for (; host[k] && k < 63; k++) job.host[k] = host[k]; job.host[k] = 0;
    job.kind = kind; job.active = 1;
    return 1;
}

/* Advances a running ping/nslookup; prints into the Terminal. */
void net_job_tick(void) {
    if (!job.active) return;
    char b[20];
    if (!job.ip) {
        int r = net_resolve(job.host, &job.ip);
        if (r == 0) return;
        if (r < 0) { con_print("Could not find host '"); con_print(job.host); con_print("'.\n"); job.active = 0; shell_job_done(); return; }
        ip_text(job.ip, b);
        if (job.kind == 2) {
            con_print("Name:    "); con_print(job.host); con_print("\nAddress: "); con_print(b); con_print("\n");
            job.active = 0; shell_job_done(); return;
        }
        con_print("Pinging "); con_print(job.host); con_print(" ["); con_print(b); con_print("] with 32 bytes of data:\n");
        ping_seq = 0;
    }
    uint32_t t = now_ms();
    if (job.sent > 0 && !job.reported) {              /* report the last ping: reply or time-out */
        int s = (job.sent - 1) % 16;
        if (ping_got[s]) {
            ip_text(job.ip, b);
            con_print("Reply from "); con_print(b); con_print(": time");
            if (ping_rtt[s] < 10) con_print("<10ms\n"); else { con_print("="); con_num(ping_rtt[s]); con_print("ms\n"); }
            job.replies++; job.reported = 1;
        } else if (t - ping_sent_at[s] >= 1000) { con_print("Request timed out.\n"); job.reported = 1; }
        else return;
    }
    if (t < job.next) return;
    if (job.sent == 4) {
        con_print("Sent 4, received "); con_num(job.replies); con_print(", lost "); con_num(4 - job.replies); con_print(".\n");
        job.active = 0; shell_job_done(); return;
    }
    ping_send(job.ip); job.sent++; job.reported = 0; job.next = t + 1000;
}
void net_job_cancel(void) { if (job.active) { job.active = 0; con_print("^C\n"); shell_job_done(); } }
