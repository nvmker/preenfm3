# pi-lens ast-grep rules (project-local)

pi-lens auto-loads every `.yml` under `rules/ast-grep-rules/rules/` alongside
its built-ins — no config required. See the pi-lens `custom-rules` doc for the
schema. These are the repo's Tier-3 rules: domain-specific bug patterns the
generic analyzers (cppcheck, clang-tidy, gcc) don't catch precisely.

## Shipped

### `cpp/table-index-sysex-buffer.yml`

Flags any lookup indexed by a raw sysex/patch buffer read —
`TABLE[patch[N]]` / `TABLE[eb_op[N]]` / `TABLE[(packed[N])]` — that isn't
wrapped in `limit()`. This is the DX7-sysex OOB class: a malformed patch byte
`>= table size` reads past the global (the original crash-class bug,
`dx7_voice_amd_to_ol_adjustment[(patch[140])]`, fixed in `63650f4` and guarded
by a unit test). Scoped to the sysex buffer names so trusted internal nested
lookups (`voices_[voiceNumber_[k]]`, `actions[nextActionIndex_[i]]`, …) are not
flagged.

Calibration: **0 findings on the current tree** (every table access is already
`limit()`-wrapped) — pure regression prevention. Verified to catch the original
parenthesized bug form and to fire on an injected regression.

## Analyzed, NOT shipped

Two further rules from the original Tier-3 plan were investigated and
deliberately deferred — with the evidence, so the decision is auditable.

### switch-without-break / `[[fallthrough]]`

**Covered by the compiler.** GCC `-Wimplicit-fallthrough` (on via `-Wextra` on
the `preenfm3` target) already flags unannotated fallthrough — 12 sites in the
last build. An ast-grep duplicate would either report the same 12 (redundant)
or, to find anything new, have to second-guess gcc's annotation handling
(`/* fall through */`, `[[fallthrough]]`) and risk false positives. Not worth
the maintenance overlap. If `-Wimplicit-fallthrough` ever proves insufficient,
revisit.

### channel-match-inconsistency

**No concrete instance to calibrate against.** No past bug in the triage
history matches this shape, and "inconsistent MIDI-channel comparison" has no
clean structural signature (it would need to correlate two comparisons on
different channel expressions across distant code — not expressible as a local
ast-grep pattern without a prohibitive false-positive rate). Deferred until a
real instance surfaces to calibrate against; until then it's a code-review
checklist item, not a rule.
