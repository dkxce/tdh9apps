/* APRS extras for the loader stub (shipped with --aprs-mute, build.py).
 *
 * Two stock hooks, retargeted by build.py (app offsets; CPU = app + 0x01E00100):
 *   0x862C6  call 0x55EC2  -> ml_popup_beep    (popup request 0x01E55FC2)
 *   0x7E21C  call 0x52472  -> ml_pf_action     (PF action dispatch, TEST hook)
 *
 * When the APRS RX popup is requested (b[0xC0F0+1095] == 1, the "RX Oto Popup"
 * setting), stock posts UI request 9 and calls 0x01E55FC2. The hook sits on
 * that call, so the beep happens only when the popup is really requested.
 * Note 0x1E85782 is NOT used in this configuration: with the setting on, the
 * packet handler jumps past it.
 *
 * The beep is stock's own key-beep wrapper 0x01E52BD0(42, 20, 1), the path
 * stock uses from 0x01E75998. Tone 42 is
 * "storage/res_nor/C/tone/d_sound.*": a sound file on the resource chip, so it
 * needs the prompt player and its audio routing (the wrapper does 0x01E50820 /
 * 0x01E521CE itself; the bare 0x01E5241A stays silent). Two calls give
 * beep-beep. The request arguments (r0=0, r1=3) are preserved and passed on.
 *
 * The beep follows the APRS "Report Sats" setting: b[0xC0F0+1056] must be
 * non-zero (the same byte the beacon builder tests at 0x01E5C04C before adding
 * satellites). The popup request is passed through either way.
 *
 * Verified on hardware (TD-H9, 2026-11-23): beep-beep plays with the popup.
 */
    .section .text,"ax",@progbits

/* ---- popup request -> beep-beep ----------------------------------------- */
    .globl ml_popup_beep
ml_popup_beep:
    [--sp] = {rets, r6-r4}
    r4 = r0                      /* request arguments (0, 3) */
    r5 = r1
    r6 = r2
    r3 = 49392                   /* settings cache; 1056 = "Report Sats" */
    r1 = 1056
    r1 = b[r3+r1] (u)
    if (r1 == 0) goto beep_done
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
beep_done:
    r0 = r4
    r1 = r5
    r2 = r6
    {rets, r6-r4} = [sp++]
    r3 = 0x01e55fc2
    goto r3

/* ---- PF action "Weather" -> beacon instead of the weather screen --------
 *
 * The side-button action list is dispatcher 0x01E7E318: it takes the action
 * number in r0, does `r0 = r0 - 1; if (r0 > 7) return`, and switches over the
 * tbb table at 0x1E7E336. The range fits the menu
 *   None, FM Radio, GNSS SW, Tone, Alarm, Weather, PTT2, OD PTT
 * so Weather = 6 (6th item, index 5 from zero as seen in the radio menu).
 *
 * The hook sits on `call 0x1E52572` at CPU 0x01E7E31C (app offset 0x7E21C) -
 * the call the dispatcher makes right after keeping the action number in r2.
 * The callee ignores its arguments, so only rets has to survive the beacon.
 *
 * For Weather: send the beacon, then force r2 = 0. The dispatcher computes
 * `r0 = r2 - 1` = -1, fails its own `if (r0 > 7)` bound and returns from the
 * epilogue - no weather screen, the radio stays on the current one.
 *
 * Verified on hardware (TD-H9, 2026-11-24): beacon goes out on the Weather
 * item. The screen cancellation below is not tested yet.
 */
    .globl ml_pf_action
ml_pf_action:
    [--sp] = {rets, r5, r4}
    r4 = r0                      /* action number, 1-based */
    r1 = 6                       /* Weather */
    if (r4 != r1) goto pf_done
    r4 = 72400
    r5 = b[r4+78] (u)            /* stock's timed-beacon path skips the stop */
    r3 = 0x01e5c3d4              /* stock beacon sender */
    call r3
    if (r5 == 1) goto pf_void
    if (r0 == 0) goto pf_void
    r0 = 0
    r3 = 0x01e5cdec              /* end of transmission, else PTT stays keyed */
    call r3
pf_void:
    r2 = 0                       /* invalid action -> dispatcher returns */
pf_done:
    {rets, r5, r4} = [sp++]
    r3 = 0x1e52572               /* original callee of the hooked call */
    goto r3

/* ---- Reference: sending an APRS beacon from stock code (NOT WIRED) -------
 *
 * The V/M hold trigger was removed on 2026-11-23 (it also left the radio in
 * TX and fought stock's V/M action). The pieces below are the verified stock
 * path, kept here so the feature can be re-wired without another reverse.
 *
 * Sender: 0x01E5C3D4, no arguments, call it from the APRS task context
 * (inside the tick 0x01E8752E, or a hook on a call inside it).
 * Before sending it checks, and silently returns when:
 *   b[0xC0F0+218] == 1   APRS/TX enabled          (app+0x5224C family config)
 *   b[0x72400+2688] != 0 APRS state ready
 *   [0x72400+2704] and [0x72400+2708] not both 0   fixed position / GPS fix
 *   b[0xC0F0+1091] <= 4  beacon frame type
 * then switches on the frame type (Mic-E / position / ...) via tbb/tbh.
 *
 * End of transmission: stock's timed-beacon path (0x01E83008..0x01E8301E)
 * does it right after the sender:
 *   r4 = b[0x72400+78]
 *   call 0x01E5C3D4
 *   if (r4 == 1) done
 *   if (r0 == 0) done
 *   r0 = 0; call 0x01E5CDEC        <- without this PTT stays keyed
 *
 * Where the timed beacon runs: 0x01E82FB2, called from the APRS tick at the
 * call site app+0x87494 (call app+0x82EB2); it reads b[0xC0F0+1059] (timed
 * beacon on) and h[0xC0F0+1060] (interval, seconds), counts ticks in
 * [0x72400+540] and fires every interval*100 ticks.
 *
 * To wire a new trigger: retarget app+0x87494 to a stub that calls
 * 0x01E5C3D4 + 0x01E5CDEC as above, then tail-jumps to 0x01E82FB2; keep the
 * sender in the APRS task context, never in the main loop (airtime ~2 s).
 * Free RAM bytes used by the removed trigger: 0x2BEF2 (state), 0x2BEF3
 * (u8 hold baseline) - the tail of the run-next request's name[16].
 */

