/* Optional normal-radio APRS speaker gate. Original implementation from the
 * observed M11 decision tree, not copied M11 code. No modem/RF writes, RAM
 * reservation or persistent-settings writes. Enabled by --aprs-mute only.
 * Stock 1.0.38: config 0xC0F0+944 = APRS enabled, +1090 = RX side (2=B).
 * app_core +444=current frequency, +464=A, +472=B. A wins on equal A/B.
 * These meanings and hardware/dual-watch behavior still require bench tests.
 */
    .section .text,"ax",@progbits
    .globl ml_mute_squelch
ml_mute_squelch:
    [--sp] = rets
    /* Never suppress a close request. */
    if (r1 == 0) goto mute_squelch_pass
    call mute_block
    if (r2 != 0) goto mute_done
mute_squelch_pass:
    {rets} = [sp++]
    r3 = 0x01e5234c
    goto r3
    .globl ml_mute_volume
ml_mute_volume:
    [--sp] = rets
    if (r0 == 0) goto mute_volume_pass
    call mute_block
    if (r2 != 0) goto mute_done
mute_volume_pass:
    {rets} = [sp++]
    r3 = 0x01e52328
    goto r3
mute_done:
    pc = [sp++]
/* r2=result; r3 scratch; r0/r1 intact for the stock tail call. */
mute_block:
    .ifdef ML_RUNTIME_BOOT
    r3 = 0x29a00
    r2 = b[r3+4] (u)
    if (r2 == 0) goto mute_allow
    .endif
    r3 = 0x0000c0f0
    r2 = 944
    r2 = b[r3+r2] (u)
    if (r2 != 1) goto mute_allow
    r2 = 1090
    r2 = b[r3+r2] (u)
    if (r2 != 2) goto mute_allow
    r3 = 0x00011ad0
    r2 = [r3+444]
    r3 = [r3+464]
    if (r2 == r3) goto mute_allow
    r3 = 0x00011ad0
    r3 = [r3+472]
    if (r2 != r3) goto mute_allow
    r2 = 1
    rts
mute_allow:
    r2 = 0
    rts
