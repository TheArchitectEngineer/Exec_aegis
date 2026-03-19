#![no_std]

extern "C" {
    // C declaration: void serial_write_string(const char *s)
    // On x86-64 with GCC, `char` is signed 8-bit and `u8` is unsigned 8-bit.
    // Both are 1 byte with identical ABI representation — safe to call with
    // a Rust byte-string literal (*const u8) on this target.
    fn serial_write_string(s: *const u8);
}

/// Initialize the capability subsystem.
///
/// Phase 1: stub only. Prints status line and returns immediately.
///
/// Note: writes directly to serial rather than through printk because no
/// `printk` Rust FFI wrapper exists yet. This means CAP output does not
/// appear on VGA in Phase 1. Revisit when a printk wrapper is designed
/// (post-PMM/VMM phase).
#[no_mangle]
pub extern "C" fn cap_init() {
    // SAFETY: serial_init() is called in arch_init() before cap_init() is
    // called in kernel_main, so the serial port is fully initialized.
    // serial_write_string is a simple polling write with no shared mutable
    // state and no re-entrancy concerns at this point in boot.
    // The pointer is to a valid C string literal (null-terminated) in read-only data.
    // `char` and `u8` have identical 8-bit ABI representation on x86-64/GCC.
    unsafe {
        serial_write_string(c"[CAP] OK: capability subsystem reserved\n".as_ptr() as *const u8);
    }
}

#[panic_handler]
fn panic(_info: &core::panic::PanicInfo) -> ! {
    loop {}
}
