# Changelog

Releases prior to v1.12 predate this changelog — see the GitHub releases page
for their history.

---

## v1.12 — the hardening release

This is a stability and correctness release: ~90 defect fixes (123 commits)
on top of v1.11, plus a large expansion of the developer test infrastructure.
Nearly all of the fixed defects exist in upstream preenfm3 code — they were
found by a systematic campaign (static analysis, address/UB sanitizers, an
expanded host test suite with golden-master render comparison, and a
~30-session on-device validation program covering MIDI, audio, display, SD
persistence, and electrical interfaces), and every fix was verified on
hardware against the v1.11 firmware it replaces.
**No regression introduced since v1.11 was found in the entire A/B program.**

### Highlights

- **No more sequencer freezes.** Two independent freeze classes fixed: a
  threading race that corrupted the sequencer's action list when MIDI arrived
  during playback (froze the UI mid-recording), and unguarded list walks that
  turned one corrupted saved sequence into a boot-time or play-time hang
  (corrupted slots are now detected and repaired on load).
- **No more display corruption.** The operator-page encoder storm garbage
  (random pixels around intact text) is root-caused and fixed, the
  MENU + "reinit" shortcut now fully heals a desynced screen, and several
  drawing functions were hardened against out-of-range data.
- **No more hard faults on the edit page.** Selecting middle-column operators
  or turning their MIX encoder could crash the unit (alignment UB exposed by
  the current toolchain); fixed.
- **Synth correctness.** MONO mode now recalls the most recent held note on
  key release; CC14 (unison spread) no longer starts the step sequencer; a
  bug that faded a timbre to silence after ~20–25 retriggered notes is fixed;
  stuck-voice classes on order-dependent voice reuse are fixed.
- **SD data safety.** Crash-safe saves for the controller config (write to
  temp → verify → atomic rename, with automatic backup promotion), truncation
  of the default mixer file so stale tails can't leak, and validation of
  essentially everything the firmware reads from SD: scala files, sequence
  banks, mixer banks, user waveforms/env curves, controller config. Rejected
  user-waveform slots now render silence instead of noise; malformed scala
  files fall back to diatonic instead of undefined behavior.
- **Screenshots are color-correct.** PPM screenshot-to-SD (MENU +
  NEXT_INSTRUMENT) now expands 16-bit to 24-bit color with low-bit
  replication — full-scale channels stay full-scale. Verified pixel-exact
  against the live framebuffer.
- **Step editor fixes.** "Clear all"/"clear part" now clear the final step
  (255) too, and clear-part boundaries/cursor advance are exact.

### Fixed (grouped)

**Sequencer & MIDI**
- Sequencer freeze during recording/playback under MIDI traffic — decode-context
  action-list race (mutations now queued to the main loop)
- Freeze/corruption from damaged sequence files — bounded walks, load-time
  validation and auto-repair, insert guards, transactional loads
- Step 255 not cleared by clear-all / clear-part
- Step-note loops bounded at 5 slots; step notes fully initialized
- CC14 no longer starts (or stops) the step sequencer — missing `break` in the
  CC dispatch table
- Out-of-range NRPN float values clamped before conversion
- Sysex framing safety; DX7 LFO-AMD table index bounded on malformed sysex

**Synth engine**
- MONO note-off now recalls the most recent held note
- MAIN_GATE stale-target freeze (timbre fading to silence after ~20–25 notes)
- Stuck voices from order-dependent voice lifecycle state; playing-flag clear
  on the pending-tail path
- Synced-LFO phase wrapped into range; non-finite/corrupt step-seq BPM guarded
- Matrix sources/destinations zero-initialized at construction

**Display / UI**
- Operator-page encoder-storm corruption (oscillo-background out-of-bounds
  writes into the color table) — clamped, plus bin-loader validation
- Hard fault on middle-column operator highlight (unaligned stores)
- MENU + reinit now forces a full repaint (heals a desynced panel)
- Power-poll and TFT-action-queue race hardening; SPI ownership window closed

**Filesystem / SD**
- Crash-safe controller-config save with backup promotion; corrupt or
  truncated configs load defaults or the backup instead of garbage
- Truncated mix.dfl no longer leaves stale tails (default mixer save truncates)
- Sequence banks: exact version header required, payload reads validated
  before any state mutation, slot seeks checked, write-stall-bounded creation
- Scala: every interval validated (not just the octave), truncated .scl
  rejected, frequencies validated at computation time
- User waveforms/env curves: rejected slots render silence, NaN curves fixed,
  interpolation bounded, header counts validated
- Mixer banks: reads validated, name copies bounded, unknown versions reset
  safely
- PPM screenshots: color expansion fixed + feature flag initialized

**MIDI controller mode**
- Corrupt button/encoder types in a saved config coerce to safe defaults and
  emit nothing for unknown types
- Persisted MIDI channels validated; ring capacity reserved for whole CC
  messages (no torn output under load)
- Encoder deltas accumulate in 64-bit (no signed overflow)

**Build & platform (developer-facing)**
- `-Wall -Wextra` now enabled and clean on the firmware target
- Host test suite expanded to 656 tests (32 test files, from the v1.11
  scaffold), with golden-master render comparison, ASAN+UBSAN clean, order-
  and shuffle-stable, ~90% coverage of the changed areas
- Undefined-behavior cleanup across DSP/render/filesystem paths surfaced by
  sanitizers (uninitialized reads, out-of-bounds indices)

### Known issues (inherited from upstream, documented — not regressions)

- **Changing the FX1 type while a voice is sounding mutes that voice.**
  Deterministic, present in v1.11 (verified by A/B on both firmwares); fresh
  notes are unaffected. Workaround: change FX type while silent.
- **USB enumeration is one-shot at boot.** If the host misses the connect
  event (rapid replugs, host bus churn), the synth runs fine but never
  appears on USB until replug/reset. Hardening is sketched but deferred.
- **USB-MIDI message loss is possible on a heavily loaded host** when
  recording the synth's audio while streaming dense MIDI (observed only on a
  degraded host; clean at 800 msg/s on a healthy one). All-notes-off clears
  any stuck voice.
- **DX7 sysex content is not validated.** A correctly-sized file of garbage
  loads as a noisy-but-valid patch (upstream behavior).
- **No MIDI clock output** — the synth cannot act as clock master (never
  existed upstream; re-scoped as a feature request).
- Pre-existing mixer banks saved by much older firmware keep their legacy
  master-FX byte order until re-saved (read side deliberately frozen).

### Installing

- Flash the application at `0x08020000` via DFU (e.g.
  `dfu-util -a 0 -s 0x08020000:leave -d 0x0483:0xdf11 -D preenfm3.bin`).
  **The bootloader is not touched.**
- Settings, mixer banks, presets, sequences, scala files, and user
  waveforms on the SD card are compatible; no migration needed.
- First boot after flashing is a normal boot; nothing to reconfigure.
