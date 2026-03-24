# virtio-gpu Reference

Reference for the full virtio-gpu 2D driver (Aegis Phase 29 follow-up).

The current Aegis framebuffer driver (`kernel/drivers/fb.c`) uses the linear
framebuffer that GRUB sets up via VBE/GOP. No virtio-gpu protocol is needed
for that approach.

A full virtio-gpu driver (Phase 29+) would:
1. Use the virtio-gpu PCI device (vendor 0x1AF4, device 0x1050)
2. Create a 2D resource via VIRTIO_GPU_CMD_RESOURCE_CREATE_2D
3. Attach backing memory via VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING
4. Set the scanout via VIRTIO_GPU_CMD_SET_SCANOUT
5. Transfer and flush via VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D +
   VIRTIO_GPU_CMD_RESOURCE_FLUSH

This allows pixel-perfect display updates and works under KVM (where direct
framebuffer writes may not update the display without explicit flushes).

## References
- Virtio spec §5.7: https://docs.oasis-open.org/virtio/virtio/v1.1/virtio-v1.1.pdf
- Linux driver: drivers/gpu/drm/virtio/
- QEMU device: hw/display/virtio-gpu.c
