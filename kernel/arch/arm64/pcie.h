/* pcie.h — ARM64 PCIe stubs. ECAM discovery not yet ported. */
#ifndef PCIE_H
#define PCIE_H

#include <stdint.h>

#define PCIE_MAX_DEVICES 64

typedef struct {
    uint16_t vendor_id;
    uint16_t device_id;
    uint8_t  class_code;
    uint8_t  subclass;
    uint8_t  progif;
    uint8_t  bus, dev, fn;
    uint64_t bar[6];
} pcie_device_t;

static inline void pcie_init(void) {}

static inline uint8_t  pcie_read8 (uint8_t b, uint8_t d, uint8_t f, uint16_t o)
    { (void)b;(void)d;(void)f;(void)o; return 0xFF; }
static inline uint16_t pcie_read16(uint8_t b, uint8_t d, uint8_t f, uint16_t o)
    { (void)b;(void)d;(void)f;(void)o; return 0xFFFF; }
static inline uint32_t pcie_read32(uint8_t b, uint8_t d, uint8_t f, uint16_t o)
    { (void)b;(void)d;(void)f;(void)o; return 0xFFFFFFFF; }
static inline void pcie_write32(uint8_t b, uint8_t d, uint8_t f, uint16_t o, uint32_t v)
    { (void)b;(void)d;(void)f;(void)o;(void)v; }

static inline int pcie_device_count(void) { return 0; }
static inline const pcie_device_t *pcie_get_devices(void)
    { return (const pcie_device_t *)0; }
static inline const pcie_device_t *pcie_find_device(uint8_t c, uint8_t s, uint8_t p)
    { (void)c;(void)s;(void)p; return (const pcie_device_t *)0; }

#endif /* PCIE_H */
