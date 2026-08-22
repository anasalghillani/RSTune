# realtime-audio-hook

A skill capturing the method used to build RSTune: how to add real-time audio DSP inside
a host process you do not own, without shipping bugs that are inaudible as errors.

`realtime-audio-hook.skill` is the shareable package. `source/` is the same content as
plain files, if you would rather read or edit it directly.

## Installing it

**Claude Code / desktop:** copy `source/realtime-audio-hook/` into `~/.claude/skills/`
(on Windows, `C:\Users\<you>\.claude\skills\`). It is picked up in new sessions.

**Sharing it:** send the `.skill` file. Where skill creation is allowed, the file card
offers a Save button that installs it.

**Another agent:** point it at `SKILL.md`. It is plain markdown with no tooling
dependencies; `references/patterns.md` holds the code.

## What is in it

The parts that were earned rather than assumed, each traceable to a bug that was found
by measurement during RSTune's development:

- Build the offline harness before touching the live path, and vary what comes *before*
  the event under test, not just the event
- Realtime callback rules, and degrading to pass-through rather than failing
- Never write into the host's buffer; return your own
- Avoid double-processing when a wrapper can wrap itself, using identity not name matching
- Lock-free control from a GUI that a crashed GUI cannot glitch
- The five-rung verification ladder, and saying which rung you actually reached
- When a test fails, suspect the instrument first
- Separate physics from policy, and name settings for what you measured

## Provenance

Written from RSTune, a tuning shifter built into a fork of RS_ASIO for Rocksmith 2014.
Every lesson corresponds to something that actually went wrong and was caught by
measurement. Evaluated against a no-skill baseline on three tasks; the clearest result
was a run that built and ran its own harness and used it to find both a real bug and a
broken test.
