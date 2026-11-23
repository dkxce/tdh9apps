/* APRS extras for the loader stub (shipped with --aprs-mute, build.py).
 *
 * Two stock hooks, both retargeted by build.py (app offsets; CPU = app + 0x01E00100):
 *   0x86324  call 0x85682  -> ml_popup_beep    (stock APRS popup screen 0x01E85782)
 *   0x87494  call 0x82EB2  -> ml_beacon_tick   (stock timed-beacon check 0x01E82FB2)
 *
 * 1. Holding V/M (key byte 0xC3AF = 0x10) for 40 stock hold ticks sends one
 *    beacon with stock's own sender, 0x01E5C3D4, from the APRS tick context.
 *    State: 0x2BEF2 (0 idle / 1 armed / 2 sent for this hold), 0x2BEF3 (u8
 *    baseline of h[0xC3B2]). Those two bytes are the tail of the run-next
 *    request's name[16]; a module that posts a request rewrites the whole
 *    name, so nothing stale survives, and the state byte is validated.
 *
 * 2. When the APRS popup is about to be shown, play the stock key beep twice:
 *    0x01E5241A(42) (stock tone id 42 = the keypad beep, app+0x5231A) with
 *    an 80 ms gap (0x01E516C6 takes milliseconds). While the radio is muted by
 *    aprs_mute (APRS on, RX side B, listening on B), the RF audio DAC is turned
 *    off for those ~200 ms (write 0x30 = 0xBDF1, app+0x5060A) and put back
 *    afterwards (0xBFF5), and the speaker gate is closed again (0x01E5234C),
 *    so only the beep is heard. The popup arguments (r0-r2) are passed on.
 *
 * All stock addresses were found in app.bin v1.0.38 and cross-checked against
 * h9toolbox's evidence (aprs-timed-beacon.asm) and far-hello/audio.c
 * ("stock app+0x5231A", key beeps on hardware). Not yet tested on the radio.
 */
    .section .text,"ax",@progbits

/* ---- V/M hold -> beacon (runs inside the stock APRS tick) ---------------- */
    .globl ml_beacon_tick
ml_beacon_tick:
    [--sp] = {rets, r7-r4}
    r4 = 0x0000c3af
    r0 = b[r4+0] (u)
    r5 = 0x0002bef2
    if (r0 != 16) goto bt_idle
    r1 = b[r5+0] (u)
    if (r1 > 2) goto bt_idle   /* stale/garbage: start over */
    if (r1 == 2) goto bt_done  /* already sent during this hold */
    r2 = 0x0000c3b2
    r3 = h[r2+0] (u)
    r3 = r3 & 0xff
    if (r1 == 0) goto bt_arm
    r6 = b[r5+1] (u)
    r3 -= r6
    if (r3 < 40) goto bt_done
    r1 = 2
    b[r5+0] = r1              /* one beacon per hold */
    r3 = 0x01e5c3d4
    call r3
    goto bt_done
bt_arm:
    b[r5+1] = r3
    r1 = 1
    b[r5+0] = r1
    goto bt_done
bt_idle:
    r1 = 0
    b[r5+0] = r1
bt_done:
    {rets, r7-r4} = [sp++]
    r3 = 0x01e82fb2
    goto r3

/* ---- popup -> beep-beep ------------------------------------------------- */
    .globl ml_popup_beep
ml_popup_beep:
    [--sp] = {rets, r9-r4}
    r4 = r0                   /* popup arguments */
    r5 = r1
    r6 = r2
    call mute_block          /* stub/aprs_mute.s: r2 = 1 when this channel is muted */
    r8 = r2
    if (r8 == 0) goto pb_beep
    r0 = 0x30                 /* RF audio DAC off: no packet audio under the beep */
    r1 = 0xbdf1
    r3 = 0x01e5070a
    call r3
pb_beep:
    r0 = 42
    r3 = 0x01e5241a
    call r3
    r0 = 80
    r3 = 0x01e516c6
    call r3
    r0 = 42
    r3 = 0x01e5241a
    call r3
    r0 = 120                  /* let the second tone finish before the gate closes */
    r3 = 0x01e516c6
    call r3
    if (r8 == 0) goto pb_tail
    r0 = 0x30                 /* back to what an open squelch uses */
    r1 = 0xbff5
    r3 = 0x01e5070a
    call r3
    r0 = 1                    /* and keep the amplifier closed (mute holds) */
    r1 = 0
    r3 = 0x01e5234c
    call r3
pb_tail:
    r0 = r4
    r1 = r5
    r2 = r6
    {rets, r9-r4} = [sp++]
    r3 = 0x01e85782
    goto r3

