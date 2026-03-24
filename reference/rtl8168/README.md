# RTL8168 / RTL8111 Driver Reference

Linux kernel r8169 driver for Realtek RTL8111/8168/8211/8411 (PCI 10ec:8168).

## Source files
- `r8169_main.c` — main driver (5800+ lines, Linux 6.x)
- `r8169.h` — private structs, mac_version enum
- `r8169_phy_config.c` — PHY initialization per mac_version

## Key register offsets (MMIO base + offset)
| Register            | Offset | Width | Notes                                 |
|---------------------|--------|-------|---------------------------------------|
| MAC0                | 0x00   | 32    | MAC address bytes 0-3                 |
| MAC4                | 0x04   | 32    | MAC address bytes 4-5                 |
| TxDescStartAddrLow  | 0x20   | 32    | TX descriptor ring base (low 32)      |
| TxDescStartAddrHigh | 0x24   | 32    | TX descriptor ring base (high 32)     |
| ChipCmd             | 0x37   | 8     | Reset(0x10), RxEn(0x08), TxEn(0x04)  |
| IntrMask            | 0x3C   | 16    | Interrupt mask                        |
| IntrStatus          | 0x3E   | 16    | Interrupt status                      |
| TxConfig            | 0x40   | 32    | TX DMA burst, IFG                     |
| RxConfig            | 0x44   | 32    | RX filter, DMA burst                  |
| CPlusCmd            | 0xE0   | 16    | RxChkSum(0x20), RxVlan(0x40)          |
| RxDescAddrLow       | 0xE4   | 32    | RX descriptor ring base (low 32)      |
| RxDescAddrHigh      | 0xE8   | 32    | RX descriptor ring base (high 32)     |
| MaxTxPacketSize     | 0xEC   | 16    | Set to 0x3f (or 0x27ce for jumbo off) |

## TxConfig value
`(7 << 8) | (0x03 << 24)` = TX_DMA_BURST=7 (unlimited), IFG=3

## RxConfig value (RTL8168, VER_40+)
`RX128_INT_EN(1<<15) | RX_MULTI_EN(1<<14) | RX_DMA_BURST(7<<8) | RX_EARLY_OFF(1<<11)`
Plus accept filter: `AcceptBroadcast(0x08) | AcceptMyPhys(0x02)`

## Descriptor format (TX and RX identical layout)
```c
struct rtl_desc {
    uint32_t opts1;   // bit31=DescOwn, bit30=RingEnd, bit29=FirstFrag, bit28=LastFrag, bits0-12=length
    uint32_t opts2;   // VLAN / checksum (set to 0 for basic operation)
    uint64_t addr;    // physical DMA address of buffer
};
```
256 descriptors per ring, ring must be 256-byte aligned.

## Init sequence (simplified, no PHY tuning)
1. PCI Bus Master Enable (pci_set_master)
2. Map MMIO BAR (first memory BAR — BAR2 on RTL8168)
3. Reset: write 0x10 to ChipCmd, poll until clear
4. Read MAC from offset 0x00-0x05
5. Disable interrupts: write 0x0000 to IntrMask
6. Write TxDescStartAddrHigh then Low
7. Write RxDescAddrHigh then Low
8. Write TxConfig
9. Write RxConfig (with accept filter)
10. Write CPlusCmd (RxChkSum | existing bits)
11. Write ChipCmd = CmdRxEnb | CmdTxEnb (0x0C)

## BAR selection
Linux uses `pci_select_bars(pdev, IORESOURCE_MEM)` — picks first memory BAR.
For RTL8168: BAR0 is I/O space, BAR2 is MMIO. Use BAR2.

## RTL8168 vs RTL8125 key differences
| Feature         | RTL8168 (10ec:8168) | RTL8125 (10ec:8125) |
|-----------------|---------------------|---------------------|
| IntrMask offset | 0x3C (16-bit)       | 0x38 (32-bit)       |
| Speed           | 1 Gbps              | 2.5 Gbps            |
| mac_version     | VER_02..VER_52      | VER_61+             |
| RxConfig        | see above           | different bits      |
| PHY init        | complex per-version | different           |

Shared concepts: descriptor format, TxConfig/RxConfig registers, ring management,
ChipCmd reset sequence, MAC address registers.
