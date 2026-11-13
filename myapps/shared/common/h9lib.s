/* Shared helpers for H9 Toolbox modules (linked into each module that needs
 * them, at the module load address 0x29F00). The LCD helpers are copied
 * unchanged from c-hello/entry.s (the hardware-proven stage-23 path); the
 * panel is already set up by stock my_app when a module runs.
 *
 * C API (JieLi Pi32v2 clang ABI: arguments r0-r3, result r0, r4-r14
 * preserved; each wrapper saves r4-r14). Declared in h9mod.h. */
    .section .text,"ax",@progbits

/* void h9_lcd_window(x0, y0, x1, y1): open a pixel window for writes. */
    .globl h9_lcd_window
h9_lcd_window:
    [--sp] = {rets, r14-r4}
    call hello_set_window
    {pc, r14-r4} = [sp++]

/* void h9_lcd_write(buffer, length): send big-endian RGB565 bytes (keep
 * length <= 128, as the stage-23 path does). */
    .globl h9_lcd_write
h9_lcd_write:
    [--sp] = {rets, r14-r4}
    r4 = r0
    r5 = r1
    r3 = 0x02001634
    call r3
    r0 = 1
    r1 = r4
    r2 = r5
    call hello_transfer_submit
    r3 = 0x02001650
    call r3
    {pc, r14-r4} = [sp++]

/* unsigned h9_key(void): run the stock key scan, return the key byte at
 * 0xC3AF (0 none, digit d -> d+2, 12 Menu, 13 Up, 14 Down, 15 Back). */
    .globl h9_key
h9_key:
    [--sp] = {rets, r14-r4}
    r3 = 0x01e515ac
    call r3
    r0 = 0x0000c3af
    r0 = b[r0+0] (u)
    {pc, r14-r4} = [sp++]

/* void h9_delay(ms): the stock delay, in milliseconds (h9_delay(100) took
 * 10 ticks of the 100 Hz timer on hardware, 2026-11-09); yields. */
    .globl h9_delay
h9_delay:
    [--sp] = {rets, r14-r4}
    r3 = 0x01e516c6
    call r3
    {pc, r14-r4} = [sp++]

/* void h9_led(on): the GPIO 9 LED. */
    .globl h9_led
h9_led:
    r1 = 0x001e5000
    r2 = [r1+0]
    r2 = r2 & ~0x200
    if (r0 == 0) goto h9_led_store
    r2 = r2 | 0x200
h9_led_store:
    [r1+0] = r2
    rts

/* void h9_usb_loader(void): ROM 0x02000D68, the BR23 USB loader (proven
 * from our code 2026-11-09); does not return. */
    .globl h9_usb_loader
h9_usb_loader:
    r3 = 0x02000d68
    call r3
h9_usb_loader_returned:
    goto h9_usb_loader_returned

/* void h9_reset(void): restart the chip the way stock does. Stock's two
 * reset paths (app+0xC6EC8 / app+0xC6EF4: CPU 0x01EC6FC8 / 0x01EC6FF4)
 * end with PWR_CON (0x1E0000) |= bit 4, then spin; that is the SDK's
 * cpu_reset. Interrupts off first (ROM 0x02001634). Does not return.
 * (ROM 0x0200000A, the heap-failure target, froze when called directly.) */
    .globl h9_reset
h9_reset:
    r3 = 0x02001634
    call r3
    r0 = 0x001e0000
    [r0+0] |= 0x10
h9_reset_wait:
    goto h9_reset_wait

/* 0x2A column range, 0x2B row range, 0x2C pixel-memory write.
 * The stock window routine at app+0x04ECDE uses this same coordinate order. */
hello_set_window:
    [--sp] = {rets, r7-r4}
    r4 = r3
    r5 = r2
    r6 = r1
    r7 = r0
    r0 = 42
    call hello_command
    r0 = r7
    call hello_data_word
    r0 = r5
    call hello_data_word
    r0 = 43
    call hello_command
    r0 = r6
    call hello_data_word
    r0 = r4
    call hello_data_word
    r0 = 44
    call hello_command
    {pc, r7-r4} = [sp++]

/* Stock app+0x04EC8A selects pin 18 and clears bit 4 at its GPIO bank
 * while sending a command byte. The sync register reads bracket pin changes.
 * Keep the command byte in r2 through the reads. */
hello_command:
    [--sp] = {rets, r5, r4}
    r2 = r0
    r0 = 18
    r3 = 0x01e001ba
    call r3
    r4 = r0
    if (r4 == 0) goto hello_command_done
    r5 = 0x001e0200
    r3 = [r5+0]
    r3 = [r4+0]
    r3 = r3 & ~0x4
    [r4+0] = r3
    r3 = [r5+0]
    r0 = r2
    call hello_data_byte
    r3 = [r5+0]
    r3 = [r4+0]
    r3 = r3 | 0x4
    [r4+0] = r3
    r3 = [r5+0]
hello_command_done:
    {pc, r5, r4} = [sp++]

/* Stock app+0x04ECBA sends one big-endian coordinate word through the
 * same transfer-submit path used by the stage-four byte wrapper. */
hello_data_word:
    [--sp] = rets
    sp += -4
    r1 = r0 >> 8
    b[sp] = r1
    b[sp+1] = r0
    r3 = 0x02001634
    call r3
    r1 = sp + 0
    r0 = 1
    r2 = 2
    r3 = hello_transfer_submit
    call r3
    r3 = 0x02001650
    call r3
    sp += 4
    pc = [sp++]

/* Port of the behavior at stock app+0x04EC6E. */
hello_data_byte:
    [--sp] = rets
    sp += -4
    [sp] = r0
    r3 = 0x02001634
    call r3
    r1 = sp + 0
    r0 = 1
    r2 = 1
    r3 = hello_transfer_submit
    call r3
    r3 = 0x02001650
    call r3
    sp += 4
    pc = [sp++]

/* Port of stock app+0x04BDC4: normalize controller index, select its
 * register block, submit buffer pointer and length, then invoke the stock
 * completion path at 0x01E4BD0E. */
hello_transfer_submit:
    [--sp] = {rets, r6-r4}
    r4 = r2
    r5 = r1
    r6 = r0
    r3 = 0x01e4bc8e
    call r3
    r1 = 0x01eb2144
    r0 = [r1+r0<<2]
    [r0+0] &= 0xffffefff
    [r0+12] = r5
    [r0+16] = r4
    csync
    r0 = r6
    r1 = r4
    r3 = 0x01e4bd0e
    call r3
    {pc, r6-r4} = [sp++]
