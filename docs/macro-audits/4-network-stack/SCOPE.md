# Audit 4: Network Stack

## Priority: HIGH

The network stack handles untrusted external input (packets from the wire).
Outbound TCP is currently broken. Shared static buffers make concurrency fragile.

## Files to review

| File | LOC | Focus |
|------|-----|-------|
| `kernel/net/tcp.c` | 642 | State machine, retransmit, outbound connect bug |
| `kernel/net/unix_socket.c` | 673 | AF_UNIX ring buffer, peer lifetime, fd passing |
| `kernel/net/ip.c` | 294 | IP fragmentation (or lack thereof), header validation |
| `kernel/net/eth.c` | 243 | Ethernet framing, ARP resolve from ISR |
| `kernel/net/udp.c` | 171 | UDP RX delivery, port binding |
| `kernel/net/epoll.c` | 239 | epoll correctness, lost wakeups |
| `kernel/net/socket.c` | 218 | Socket allocation, type dispatch |
| `kernel/drivers/virtio_net.c` | 505 | DMA ring handling, buffer ownership |

## Checklist

### Packet parsing (untrusted input)
- [ ] IP header length validated before accessing payload
- [ ] TCP header offset validated (data_off * 4 >= 20, <= packet length)
- [ ] No buffer overread on short packets
- [ ] ARP response validation (sender IP/MAC plausibility)
- [ ] UDP length field cross-checked against IP total length

### TCP state machine
- [ ] All state transitions match RFC 793
- [ ] RST handling correct in every state
- [ ] SYN flood doesn't exhaust connection table (32 slots)
- [ ] TIME_WAIT implemented (or documented why not)
- [ ] Outbound connect race: diagnose why curl gets ECONNREFUSED

### Shared state
- [ ] Static TX buffers (eth_send, ip_send) not corrupted by concurrent callers
- [ ] tcp_lock/sock_lock ordering consistent
- [ ] ISR-driven RX doesn't corrupt syscall-driven TX state
- [ ] Unix socket ring buffer lifetime correct after Phase 45 fix

### Resource exhaustion
- [ ] Socket table has bounded size and returns EMFILE/ENFILE
- [ ] epoll interest list bounded
- [ ] Unix socket connection slots freed on close

## Output format

Same as Audit 1.
