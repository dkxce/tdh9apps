/* APRS extras for the loader stub (shipped with --aprs-mute, build.py).
 *
 * Two stock hooks, both retargeted by build.py (app offsets; CPU = app + 0x01E00100):
 *   0x86324  call 0x85682  -> ml_popup_beep    (stock APRS popup screen 0x01E85782)
 *   0x87494  call 0x82EB2  -> ml_beacon_tick   (stock timed-beacon check 0x01E82FB2)
 *
 * 1. Holding V/M (scan byte 0xC3AF = 0x10) sends one beacon with stock's own
 *    sender, 0x01E5C3D4, from the APRS tick context. The trigger is 30 stock
 *    hold ticks (h[0xC3B2]); afterwards every tick keeps the stock hold counter
 *    at zero, so stock never reaches its own long-press threshold and the V/M
 *    action (VFO/Memory) does not fire. Immediately after the sender, the
 *    stock tail of the timed-beacon path runs (0x01E83008..0x01E8301E):
 *    unless b[0x72400+78] == 1 or the sender returned 0, call 0x01E5CDEC(0),
 *    which finishes the transmission - without it PTT stays keyed.
 *    State: 0x2BEF2 (0 idle / 1 armed / 2 sent) and 0x2BEF3 (u8 baseline);
 *    they are the tail of the run-next request's name[16], which a module
 *    rewrites when it posts one.
 *
 * 2. Before the APRS popup is shown, call stock's own key-beep wrapper
 *    0x01E52BD0(42, 20, 1) twice - the path stock uses from 0x01E75998. Tone 42
 *    is "storage/res_nor/C/tone/d_sound.*": a sound file on the resource chip,
 *    so it needs the stock prompt player and its audio routing (the wrapper does
 *    0x01E50820 / 0x01E521CE itself; the bare 0x01E5241A stayed silent).
 *    The popup arguments (r0-r2) are preserved and passed to 0x01E85782.
 *
 * Addresses were found in app.bin v1.0.38 and cross-checked with h9toolbox's
 * evidence (aprs-timed-beacon.asm) and far-hello/audio.c ("stock app+0x5231A").
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
    if (r1 > 2) goto bt_idle     /* stale/garbage: start over */
    r2 = 0x0000c3b2
    r3 = h[r2+0] (u)
    r3 = r3 & 0xff
    if (r1 == 0) goto bt_arm
    if (r1 == 2) goto bt_hold    /* already sent: keep stock from seeing a long press */
    r6 = b[r5+1] (u)
    r3 -= r6
    if (r3 < 30) goto bt_done
    r1 = 2
    b[r5+0] = r1                 /* one beacon per hold */
    r3 = 0x01e5c3d4
    call r3
    /* Stock's timed-beacon path ends the transmission right after the sender
     * (0x01E83008..0x01E8301E): unless b[0x72400+78] == 1 or the sender
     * returned 0, it calls 0x01E5CDEC(0). Without this PTT stays keyed. */
    r6 = 0x00072400
    r6 = b[r6+78] (u)
    if (r6 == 1) goto bt_hold
    if (r0 == 0) goto bt_hold
    r0 = 0
    r3 = 0x01e5cdec
    call r3
bt_hold:
    r1 = 0
    h[r2+0] = r1                 /* stock's hold counter stays short: no V/M action */
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
    [--sp] = {rets, r6-r4}
    r4 = r0                      /* popup arguments */
    r5 = r1
    r6 = r2
    r0 = 42
    r1 = 20
    r2 = 1
    r3 = 0x01e52bd0              /* stock key-beep wrapper: routing + gate + tone */
    call r3
    r0 = 42
    r1 = 20
    r2 = 1
    r3 = 0x01e52bd0
    call r3
    r0 = r4
    r1 = r5
    r2 = r6
    {rets, r6-r4} = [sp++]
    r3 = 0x01e85782
    goto r3