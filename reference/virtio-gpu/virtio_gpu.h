/* reference/virtio-gpu/virtio_gpu.h
 *
 * Key virtio-gpu struct definitions from the VIRTIO 1.1 spec §5.7.
 * For reference only — not compiled into the kernel.
 *
 * Aegis currently uses the simpler GRUB linear framebuffer approach.
 * This header documents the protocol for a future virtio-gpu driver.
 */
#ifndef VIRTIO_GPU_H
#define VIRTIO_GPU_H

#include <stdint.h>

/* PCI identity */
#define VIRTIO_GPU_VENDOR_ID    0x1AF4u
#define VIRTIO_GPU_DEVICE_ID    0x1050u  /* virtio-gpu (modern 1.0) */
#define VIRTIO_GPU_DEVICE_LEGACY 0x1040u /* transitional (also 1.0 capable) */

/* Control queue command types (virtio spec §5.7.6) */
#define VIRTIO_GPU_CMD_GET_DISPLAY_INFO          0x0100u
#define VIRTIO_GPU_CMD_RESOURCE_CREATE_2D        0x0101u
#define VIRTIO_GPU_CMD_RESOURCE_UNREF            0x0102u
#define VIRTIO_GPU_CMD_SET_SCANOUT               0x0103u
#define VIRTIO_GPU_CMD_RESOURCE_FLUSH            0x0104u
#define VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D       0x0105u
#define VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING   0x0106u
#define VIRTIO_GPU_CMD_RESOURCE_DETACH_BACKING   0x0107u
#define VIRTIO_GPU_CMD_GET_CAPSET_INFO           0x0108u
#define VIRTIO_GPU_CMD_GET_CAPSET               0x0109u

/* Response types */
#define VIRTIO_GPU_RESP_OK_NODATA               0x1100u
#define VIRTIO_GPU_RESP_OK_DISPLAY_INFO         0x1101u
#define VIRTIO_GPU_RESP_ERR_UNSPEC              0x1200u
#define VIRTIO_GPU_RESP_ERR_OUT_OF_MEMORY       0x1201u
#define VIRTIO_GPU_RESP_ERR_INVALID_SCANOUT_ID  0x1202u
#define VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID 0x1203u
#define VIRTIO_GPU_RESP_ERR_INVALID_CONTEXT_ID  0x1204u

/* Pixel format */
#define VIRTIO_GPU_FORMAT_B8G8R8A8_UNORM 1u
#define VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM 2u
#define VIRTIO_GPU_FORMAT_A8R8G8B8_UNORM 3u
#define VIRTIO_GPU_FORMAT_X8R8G8B8_UNORM 4u

/* Common header for all commands */
typedef struct __attribute__((packed)) {
    uint32_t type;
    uint32_t flags;
    uint64_t fence_id;
    uint32_t ctx_id;
    uint32_t padding;
} virtio_gpu_ctrl_hdr_t;

/* Rectangular region */
typedef struct __attribute__((packed)) {
    uint32_t x;
    uint32_t y;
    uint32_t width;
    uint32_t height;
} virtio_gpu_rect_t;

/* CMD_RESOURCE_CREATE_2D */
typedef struct __attribute__((packed)) {
    virtio_gpu_ctrl_hdr_t hdr;
    uint32_t resource_id;   /* host-assigned (e.g. 1) */
    uint32_t format;        /* VIRTIO_GPU_FORMAT_* */
    uint32_t width;
    uint32_t height;
} virtio_gpu_resource_create_2d_t;

/* CMD_RESOURCE_ATTACH_BACKING: followed by virtio_gpu_mem_entry_t[] */
typedef struct __attribute__((packed)) {
    virtio_gpu_ctrl_hdr_t hdr;
    uint32_t resource_id;
    uint32_t nr_entries;
} virtio_gpu_resource_attach_backing_t;

typedef struct __attribute__((packed)) {
    uint64_t addr;    /* physical address */
    uint32_t length;
    uint32_t padding;
} virtio_gpu_mem_entry_t;

/* CMD_SET_SCANOUT */
typedef struct __attribute__((packed)) {
    virtio_gpu_ctrl_hdr_t hdr;
    virtio_gpu_rect_t     r;
    uint32_t              scanout_id;    /* 0 = primary display */
    uint32_t              resource_id;
} virtio_gpu_set_scanout_t;

/* CMD_TRANSFER_TO_HOST_2D */
typedef struct __attribute__((packed)) {
    virtio_gpu_ctrl_hdr_t hdr;
    virtio_gpu_rect_t     r;
    uint64_t              offset;    /* byte offset into resource backing */
    uint32_t              resource_id;
    uint32_t              padding;
} virtio_gpu_transfer_to_host_2d_t;

/* CMD_RESOURCE_FLUSH */
typedef struct __attribute__((packed)) {
    virtio_gpu_ctrl_hdr_t hdr;
    virtio_gpu_rect_t     r;
    uint32_t              resource_id;
    uint32_t              padding;
} virtio_gpu_resource_flush_t;

#endif /* VIRTIO_GPU_H */
