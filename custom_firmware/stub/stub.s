/* H9 Toolbox module loader: the main-flash stub (offline, not yet flashed).
 *
 * Replaces Brick Breaker in H9 Toolbox Firmware. It lives in the cave the
 * game used (app+0xA77F0, before the S2 tables at app+0xA7E50) and is entered
 * from the same my_app loop hook (app+0x088906) Brick Breaker's launcher used.
 *
 * Hold # about 0.4 s (40 stock hold ticks; Back hold stays stock SATS INFO):
 *   1. ml_check validates the module bundle on the resource chip while the
 *      stock my_app frame is still intact. No valid bundle: the stock loop
 *      continues as if nothing happened (the radio stays usable).
 *   2. Otherwise drop my_app's frame (as Brick Breaker did) and enter
 *      ml_main, which never returns to the stock UI:
 *        run module 0 (the menu) -> it returns the index to run, or 0
 *        run that module         -> when it returns, back to the menu
 *        menu returns 0, or the menu cannot load -> reset (stock cpu_reset)
 * Every other pass, rec_tick saves stock's GPS fix to the resource chip
 * when it has moved (background recorder, below).
 *
 * Storage: the "modules" upload slot (dist/doom/upload.mjs, H9DATA01
 * directory at 0xCC0000, one segment at 0xCC1000, COMPLETE marker), holding
 * one bundle:
 *   +0  "H9MODS01"  +8 u16 version 1  +10 u16 count  +12 u32 bundle bytes
 *   +16 + 32*i: name[16], u32 offset (from bundle start), u32 size,
 *              u32 bss, u16 entry (from the load address), u16 CRC-16/XMODEM
 * Each module body is linked for, and copied to, RAM 0x29F00-0x2BEFF (8 KiB
 * taken from the stock heap: app+0x05FA9E, 0x2BF00 -> 0x29F00). The stub
 * zeroes bss, checks the body CRC in RAM, then calls
 *   entry(r0 = &ml_api, r1 = own index)
 * with the JieLi clang ABI (r4-r14 preserved by the module).
 */
    .section .text,"ax",@progbits

/* ---- my_app loop hook (Brick Breaker's launcher, retargeted) ----------
 * sp+68 in the original my_app frame holds an initialization-only temporary
 * (see brick-breaker/src/launcher.s). After saving rets/r4 it is at sp+76:
 * magic upper half 0xA5A5, stock hold count at arming below it. */
    .globl ml_hotkey
ml_hotkey:
    [--sp] = {rets, r4}
    /* Owner-lock flag setter: on the first pass record skip_check = 2.
     * No 0x716FF8 read here. Then, if skip_check is 2 (locked and not yet
     * unlocked), open the toolbox (menu) at once; the menu at skip_check==2
     * runs PASSCODE, which verifies the password and clears the flag to 1.
     * Otherwise fall through to the normal hold-# entry. */
    call mlk_check
    r5 = 0x0002b8df
    r0 = b[r5+0] (u)
    if (r0 != 2) goto ml_no_auto_open   /* only auto-open while locked */

    /* Auto-open because locked: need a valid bundle, then run the menu.
     * ml_main keeps the task frame and returns, so after PASSCODE clears the
     * flag the stock loop carries on (no reset/reboot). */
    call ml_check
    if (r0 == 0) goto ml_no_auto_open
    call ml_main
    goto ml_back_released

ml_no_auto_open:
    r3 = 0x0000c3af
    r0 = b[r3+0] (u)
	 /* Проверяем клавишу: # (20), * (19)  */
    // if (r0 == 20) goto ml_holded   /* # (0x14) */
    if (r0 == 19) goto ml_holded   /* * (0x13) */
    goto ml_back_released

ml_holded:
    r2 = [sp+76]
    r1 = r2 >> 16
    r3 = 0xa5a5
    if (r1 != r3) goto ml_arm_back_hold
    r3 = 0x0000c3b2
    r0 = h[r3+0] (u)
    r2 = r2.l (u)
    if (r0 < r2) goto ml_arm_back_hold
    r0 -= r2
    if (r0 < 40) goto ml_stock_loop_call

    /* Held long enough. With no valid bundle, re-arm: the stock UI keeps
     * running and another 0.4 s of holding tries again. */
    call ml_check
    if (r0 == 0) goto ml_arm_back_hold

    /* Keep my_app's 124-byte task frame: call ml_main as a subroutine, it
     * returns when the menu is done (back to the radio), and we continue the
     * stock loop. No drop-frame / reset, so the radio does not reboot. */
    call ml_main
    goto ml_back_released

ml_arm_back_hold:
    r3 = 0x0000c3b2
    r0 = h[r3+0] (u)
    r3 = 0xa5a50000
    r0 |= r3
    [sp+76] = r0
    goto ml_stock_loop_call

ml_back_released:
    r0 = 0
    [sp+76] = r0

ml_stock_loop_call:
    call res_tick
    call rec_tick
    r3 = 0x01e58ee4
    call r3
    {pc, r4} = [sp++]

/* ---- owner-lock flag setter (mlk_*) ----------------------------------
 * skip_check lives at 0x2B8DF (spare byte, last of the satshare struct).
 *   0  fresh / unset  1  unlocked  2  locked (needs verification)
 * On the first pass (skip_check 0 or 0xFF) read the 8 lock bytes at 0x716FF8
 * and record skip_check:
 *   - all bytes are digits '0'-'9' or spaces (0x20) AND at least one is a
 *     digit  -> locked = 2
 *   - otherwise (incl. all 0xFF erased) -> unlocked = 1
 * On later passes (skip_check already 1 or 2) it just returns. ml_hotkey
 * then auto-opens the menu when skip_check == 2 (the menu at skip_check==2
 * runs PASSCODE, which verifies the password and clears the flag to 1).
 * mlk_check preserves r7-r4; it calls ml_read which preserves r14-r4. */
mlk_check:
    [--sp] = {rets, r7-r4}
    r4 = 0x0002b8df
    r0 = b[r4+0] (u)
    if (r0 == 1) goto mlk_done       /* already unlocked this session */
    if (r0 == 2) goto mlk_done       /* already marked locked */
    /* fresh: read the 8 lock bytes */
    sp += -8
    r0 = sp + 0
    r1 = 0x00716ff8
    r2 = 8
    call ml_read
    r5 = [sp+0]                      /* word 0 */
    r6 = [sp+4]                      /* word 1 */
    sp += 8
    /* all 0xFF (erased) -> no password */
    r0 = r5
    r0 |= r6
    if (r0 == 0xffffffff) goto mlk_no
    /* check every byte is digit or space, and at least one digit */
    r11 = 0                          /* anyDigit = 0 */
    r9  = 0                          /* byte counter 0..7 */
    r8  = r5                         /* start with word 0 */
mlkl_byte:
    r3 = r8 & 0xff
    if (r3 == 0x20) goto mlkl_next   /* space: ok, not a digit */
    if (r3 < 0x30) goto mlk_no       /* not digit/space -> not locked */
    if (r3 > 0x39) goto mlk_no
    r11 = 1                          /* a digit */
mlkl_next:
    r8 = r8 >> 8
    r9 += 1
    if (r9 != 4) goto mlkl_cnt       /* after 4 bytes switch to word 1 */
    r8 = r6
mlkl_cnt:
    if (r9 != 8) goto mlkl_byte
    if (r11 == 0) goto mlk_no        /* all spaces, no digit -> not locked */
    /* locked */
    r4 = 0x0002b8df
    r0 = 2
    b[r4+0] = r0
    goto mlk_done
mlk_no:
    r4 = 0x0002b8df
    r0 = 1
    b[r4+0] = r0
mlk_done:
    {pc, r7-r4} = [sp++]

/* ---- resident tracker (res_tick runs on every my_app loop) --------------
 * A module can stay armed after the radio restarts: SAT TRACK's "start VFO
 * tracking" fills the satshare struct (modules/satpass/satshare.h, 0x2B880) and
 * resets the radio. While res_magic ("CSRT") is set, this calls the module's
 * entry in the RAM slot twice a second as entry(&ml_api, 0xFFFF): it must be
 * quick, draw nothing and keep the radio's own UI alive (the stock loop goes
 * on). Layout used here: +52 res_magic, +56 res_entry, +60 res_lencrc (length
 * << 16 | CRC-16 of the body), +64 res_tick, +80 st_flags (bit 0: slot
 * checked). The slot is checked (CRC) once after arming; anything wrong
 * disarms (res_magic = 0). The menu disarms it too (ml_main). Nothing here
 * runs unless the module armed it, so a radio that never used it is unchanged.
 * (RAM is assumed to survive the restart: if not, the magic is gone and
 * nothing happens.) */
res_tick:
    [--sp] = {rets, r7-r4}
    r4 = 0x0002b880
    r0 = [r4+52]
    r1 = 0x54525343
    if (r0 != r1) goto rest_done
    r5 = 0x0001d8b0
    r5 = [r5+0]
    r0 = [r4+64]
    r6 = r5
    r6 -= r0
    r0 = 50
    if (r6 < r0) goto rest_done
    [r4+64] = r5
    r6 = [r4+56]
    r0 = 0x00029f00
    if (r6 < r0) goto rest_bad
    r0 = 0x0002bee0
    if (r6 < r0) goto rest_range_ok
    goto rest_bad
rest_range_ok:
    r0 = [r4+80]
    r1 = r0 << 31
    if (r1 != 0) goto rest_run
    r0 = [r4+60]
    r1 = r0 >> 16
    r7 = r0 << 16
    r7 = r7 >> 16
    r0 = 0x2000
    if (r0 < r1) goto rest_bad
    if (r1 == 0) goto rest_bad
    r0 = 0x00029f00
    call ml_crc
    if (r0 != r7) goto rest_bad
    r0 = [r4+80]
    r1 = 1
    r0 |= r1
    [r4+80] = r0
rest_run:
    r3 = r6
    r0 = ml_api
    r1 = 0xffff
    call r3
    goto rest_done
rest_bad:
    r0 = 0
    [r4+52] = r0
rest_done:
    {pc, r7-r4} = [sp++]

/* ---- background GPS recorder (rec_tick runs on every my_app loop) ------
 * Stock's NMEA parser (CPU 0x01E86780...) keeps the fix in its state block:
 *   0x12550 (0x11AD0+0xA80) byte: 1 = valid fix (GGA quality > 0, RMC 'A')
 *   0x12558 (+0xA88) i32 latitude, 0x12560 (+0xA90) i32 longitude:
 *           signed microdegrees (0x01E54450: deg*1e6 + min*1e6/60, S/W < 0)
 * When the fix is valid and more than 100,000 udeg (about 0.1 degree) from
 * the last saved one, append a 16-byte record to the resource chip's sector
 * 0x3FF000 (modules/common/fix_store.h: "GFX1", lat, lon, u16 day = 0,
 * u16 check), erasing it only when all 256 records are used.
 * RAM: the run-next mailbox's spare words (0x2BEF4: magic "GFCR", the last
 * saved lat, lon). They describe the flash, so they stay true across
 * cpu_reset; with no magic, rec_scan reads them back from the chip. */
rec_tick:
    [--sp] = {rets, r7-r4}
    r0 = 0x00012550
    r0 = b[r0+0] (u)
    if (r0 == 0) goto rect_done
    r0 = 0x00012558
    r4 = [r0+0]
    r5 = [r0+8]
    r0 = r4
    r0 |= r5
    if (r0 == 0) goto rect_done
    r6 = 0x0002bef4
    r0 = [r6+0]
    r1 = 0x52434647
    if (r0 == r1) goto rect_have
    call rec_scan
    r1 = 0x52434647
    [r6+0] = r1
rect_have:
    /* |new - saved| > 100000 in either axis: (d + 100000) > 200000 unsigned */
    r7 = 0x000186a0
    r3 = 0x00030d40
    r0 = [r6+4]
    r1 = r4
    r1 -= r0
    r1 = r1 + r7
    if (r3 < r1) goto rect_save
    r0 = [r6+8]
    r1 = r5
    r1 -= r0
    r1 = r1 + r7
    if (r3 < r1) goto rect_save
    goto rect_done
rect_save:
    r0 = r4
    r1 = r5
    call rec_save
rect_done:
    {pc, r7-r4} = [sp++]

/* unsigned rec_scan(void): the first erased record (256 if full); stores the
 * newest valid record's lat/lon at 0x2BEF8/0x2BEFC (0x7FFFFFFF: none). */
rec_scan:
    [--sp] = {rets, r9-r4}
    sp += -16
    r7 = 0x7fffffff
    r8 = r7
    r6 = 0
recs_loop:
    r0 = 0x00000100
    if (r6 == r0) goto recs_done
    r0 = sp + 0
    r1 = r6 << 4
    r2 = 0x003ff000
    r1 = r1 + r2
    r2 = 16
    call ml_read
    r0 = [sp+0]
    r1 = 0xffffffff
    if (r0 == r1) goto recs_done
    r1 = 0x31584647
    if (r0 != r1) goto recs_next
    r0 = [sp+4]
    r1 = [sp+8]
    r3 = sp + 0
    r2 = h[r3+12] (u)
    r9 = r0
    r9 ^= r1
    r9 ^= r2
    r2 = 0x5a5a
    r9 ^= r2
    r2 = r9 >> 16
    r9 ^= r2
    r9 = r9 << 16
    r9 = r9 >> 16
    r2 = h[r3+14] (u)
    if (r9 != r2) goto recs_next
    r7 = r0
    r8 = r1
recs_next:
    r6 += 1
    goto recs_loop
recs_done:
    r0 = 0x0002bef4
    [r0+4] = r7
    [r0+8] = r8
    r0 = r6
    sp += 16
    {pc, r9-r4} = [sp++]

/* void rec_save(lat, lon): append one record (erase first if full). */
rec_save:
    [--sp] = {rets, r7-r4}
    sp += -16
    r4 = r0
    r5 = r1
    call rec_scan
    r6 = r0
    r0 = 0x00000100
    if (r6 != r0) goto recsv_have
    r0 = 0x000003ff
    r3 = 0x01e50000
    call rec_stock
    r6 = 0
recsv_have:
    r0 = 0x31584647
    [sp+0] = r0
    [sp+4] = r4
    [sp+8] = r5
    r0 = r4
    r0 ^= r5
    r1 = 0x5a5a
    r0 ^= r1
    r1 = r0 >> 16
    r0 ^= r1
    r0 = r0 << 16
    [sp+12] = r0
    r0 = sp + 0
    r1 = r6 << 4
    r2 = 0x003ff000
    r1 = r1 + r2
    r2 = 16
    r3 = 0x01e5006e
    call rec_stock
    r0 = 0x0002bef4
    [r0+4] = r4
    [r0+8] = r5
    sp += 16
    {pc, r7-r4} = [sp++]

/* rec_stock: call the stock routine in r3 with r0-r2, keeping r4-r14 (as
 * ml_read). 0x01E50000 = sector erase (sector number; SPI NOR 0x20),
 * 0x01E5006E = program (src, chip address, length): the routines H9
 * Toolbox's Bluetooth upload uses (resource-service/src/resource_write.s). */
rec_stock:
    [--sp] = {rets, r14-r4}
    call r3
    {pc, r14-r4} = [sp++]

/* ---- run loop (returns when the menu asks for the radio) ----------------
 * Kept as a real subroutine: saved registers, and ml_main_return restores
 * them so it can be called with `call` from ml_hotkey and return to the
 * stock loop (no reset/reboot). The menu (index 0) returns the module index
 * to run, or 0 to leave for the radio; a module returns 0xFFFF to leave too. */
ml_main:
    [--sp] = {rets, r14-r4}
    /* The menu stops the resident tracker: its code is about to be replaced. */
    r0 = 0x0002b8b4
    r1 = 0
    [r0+0] = r1
    call ml_check
    r10 = r0
    if (r10 == 0) goto ml_main_return
    r11 = 0
ml_run:
    r0 = r11
    r1 = r10
    call ml_load
    if (r0 == 0) goto ml_load_failed
    r3 = r0
    r0 = ml_api
    r1 = r11
    call r3
    if (r11 != 0) goto ml_back_to_menu
    /* The menu returned: 0 = back to the radio, else the module to run.
     * Return to the stock loop (no reset) so a locked/verified radio keeps
     * running instead of rebooting. */
    if (r0 == 0) goto ml_main_return
    r11 = r0
    goto ml_run
ml_load_failed:
    if (r11 == 0) goto ml_main_return
ml_back_to_menu:
    r11 = 0
    goto ml_run
ml_main_return:
    {pc, r14-r4} = [sp++]

/* Restart the radio the way stock's cpu_reset does (app+0xC6EC8/0xC6EF4):
 * interrupts off (ROM 0x02001634), PWR_CON 0x1E0000 |= bit 4, spin. Same
 * sequence as h9_reset in modules/common/h9lib.s, proven on hardware by the
 * menu's RADIO item 2026-11-10. (ROM 0x0200000A froze when jumped to.) */
ml_reset:
    r3 = 0x02001634
    call r3
    r0 = 0x001e0000
    [r0+0] |= 0x10
ml_reset_wait:
    goto ml_reset_wait

/* ---- unsigned ml_check(void): module count of a valid bundle, else 0 --- */
ml_check:
    [--sp] = {rets, r7-r4}
    sp += -128
    r0 = 0x00012234
    r0 = h[r0+0] (u)
    r1 = 0x5aa5
    if (r0 != r1) goto mlc_fail

    /* Directory: H9DATA01, one segment at the slot's data start, CRC-16. */
    r0 = sp + 0
    r1 = 0x00cc0000
    r2 = 128
    call ml_read
    r0 = [sp+0]
    r1 = 0x41443948
    if (r0 != r1) goto mlc_fail
    r0 = [sp+4]
    r1 = 0x31304154
    if (r0 != r1) goto mlc_fail
    r0 = [sp+80]
    if (r0 != 1) goto mlc_fail
    r0 = [sp+84]
    r1 = 0x00cc1000
    if (r0 != r1) goto mlc_fail
    r4 = [sp+88]
    r0 = r4 >> 17
    if (r0 != 0) goto mlc_fail
    r0 = sp + 0
    r1 = 92
    call ml_crc
    r1 = sp + 0
    r1 = h[r1+92] (u)
    if (r0 != r1) goto mlc_fail

    /* Upload finished and verified: "H9DATA-COMPL..." in the last block. */
    r0 = sp + 0
    r1 = 0x00cc0fe0
    r2 = 16
    call ml_read
    r0 = [sp+0]
    r1 = 0x41443948
    if (r0 != r1) goto mlc_fail
    r0 = [sp+4]
    r1 = 0x432d4154
    if (r0 != r1) goto mlc_fail
    r0 = [sp+8]
    r1 = 0x4c504d4f
    if (r0 != r1) goto mlc_fail

    /* Bundle header: H9MODS01, version 1, count 1-255, length = upload. */
    r0 = sp + 0
    r1 = 0x00cc1000
    r2 = 16
    call ml_read
    r0 = [sp+0]
    r1 = 0x4f4d3948
    if (r0 != r1) goto mlc_fail
    r0 = [sp+4]
    r1 = 0x31305344
    if (r0 != r1) goto mlc_fail
    r1 = sp + 0
    r0 = h[r1+8] (u)
    if (r0 != 1) goto mlc_fail
    r5 = h[r1+10] (u)
    if (r5 == 0) goto mlc_fail
    r0 = r5 >> 8
    if (r0 != 0) goto mlc_fail
    r0 = [sp+12]
    if (r0 != r4) goto mlc_fail
    r0 = r5
    goto mlc_done
mlc_fail:
    r0 = 0
mlc_done:
    sp += 128
    {pc, r7-r4} = [sp++]

/* ---- entry ml_load(index, count): load module `index`, return its entry
 * address, or 0 if the index, the entry or the body CRC is wrong. -------- */
ml_load:
    [--sp] = {rets, r9-r4}
    sp += -32
    r2 = r0 >> 8
    if (r2 != 0) goto mll_fail
    if (r0 < r1) goto mll_index_ok
    goto mll_fail
mll_index_ok:
    r1 = r0 << 5
    r0 = 0x00cc1010
    r1 = r1 + r0
    r0 = sp + 0
    r2 = 32
    call ml_read
    r5 = [sp+16]
    r6 = [sp+20]
    r7 = [sp+24]
    r1 = sp + 0
    r8 = h[r1+28] (u)
    r9 = h[r1+30] (u)

    /* size 1..8 KiB, bss < 8 KiB, size + bss <= 8 KiB, entry < size,
     * offset + size inside the slot's 124 KiB. */
    if (r6 == 0) goto mll_fail
    r0 = r6 >> 13
    if (r0 != 0) goto mll_fail
    r0 = r7 >> 13
    if (r0 != 0) goto mll_fail
    r0 = r6 + r7
    r1 = 0x2000
    if (r1 < r0) goto mll_fail
    if (r8 < r6) goto mll_entry_ok
    goto mll_fail
mll_entry_ok:
    r0 = r5 >> 17
    if (r0 != 0) goto mll_fail
    r0 = r5 + r6
    r1 = 0x1f000
    if (r1 < r0) goto mll_fail

    r0 = 0x00029f00
    r1 = 0x00cc1000
    r1 = r1 + r5
    r2 = r6
    call ml_read

    r0 = 0x00029f00
    r0 = r0 + r6
    r1 = 0
mll_zero:
    if (r7 == 0) goto mll_zero_done
    b[r0+0] = r1
    r0 += 1
    r7 += -1
    goto mll_zero
mll_zero_done:
    r0 = 0x00029f00
    r1 = r6
    call ml_crc
    if (r0 != r9) goto mll_fail
    csync
    r0 = 0x00029f00
    r0 = r0 + r8
    goto mll_done
mll_fail:
    r0 = 0
mll_done:
    sp += 32
    {pc, r9-r4} = [sp++]

/* ---- void ml_read(dst, chip_address, length): stock SPI2 NOR read
 * 0x01E4FF22 (ROM lock, CS GPIO 23, command 03; DMA when dst is 4-byte
 * aligned and length >= 16). Also given to modules in ml_api. ---------- */
ml_read:
    [--sp] = {rets, r14-r4}
    r3 = 0x01e4ff22
    call r3
    {pc, r14-r4} = [sp++]

/* ---- unsigned ml_crc(buffer, length): CRC-16/XMODEM in RAM ----------- */
ml_crc:
    [--sp] = {rets, r5-r4}
    r2 = 0
mlcrc_byte:
    if (r1 == 0) goto mlcrc_done
    r3 = b[r0+0] (u)
    r0 += 1
    r1 += -1
    r3 = r3 << 8
    r2 ^= r3
    r4 = 8
mlcrc_bit:
    r2 = r2 << 1
    r3 = r2 >> 16
    if (r3 == 0) goto mlcrc_next_bit
    r3 = 0x11021
    r2 ^= r3
mlcrc_next_bit:
    r4 += -1
    if (r4 != 0) goto mlcrc_bit
    goto mlcrc_byte
mlcrc_done:
    r0 = r2
    {pc, r5-r4} = [sp++]

/* ---- what a module gets in r0 ---------------------------------------- */
    .balign 4
ml_api:
    .long 0x50413948        /* "H9AP" */
    .long 1                 /* API version */
    .long 0x00cc1000        /* bundle chip address */
    .long 0x00029f00        /* load address */
    .long 0x2000            /* slot bytes */
    .long ml_read           /* void read(dst, chip_address, length) */
