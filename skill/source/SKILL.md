---
name: realtime-audio-hook
description: Methodology for inserting or modifying real-time audio DSP inside a host process you do not own - games, DAWs, drivers - by hooking an existing audio interface. Covers building an offline harness before touching the live path, realtime-callback safety, lock-free control from a GUI, avoiding double-processing when wrappers nest, and how to tell a real DSP bug from a broken measurement. Use this whenever the work involves audio callbacks, ASIO or WASAPI, IAudioCaptureClient or IAudioRenderClient, pitch or time manipulation, buffer interception, a mod or plugin that sits in another program's audio path, or any latency-sensitive signal processing where a mistake is heard rather than thrown. Reach for it even when the request sounds simple, like "shift the pitch of the mic input" or "tap the game's audio" - the failure modes here are quiet and cost far more to find later.
---

# Real-time audio DSP inside someone else's process

The defining property of this work is that **mistakes are inaudible as errors**. Nothing
throws. The host keeps running. The signal is just subtly wrong, and you find out days
later when someone says "it sounds a bit off." Everything below exists to convert that
silent failure mode into something you can see.

## Build the harness before you touch the live path

Get the DSP running offline, driven by synthetic or recorded input, with numeric checks,
*before* it ever runs inside the host. This is not extra work; it is the cheapest place to
find bugs and usually the only place you can find them at all.

An in-host test loop is: rebuild, relaunch the host, navigate to the right screen, play,
listen, guess. An offline loop is a second and prints numbers. You will run the loop
hundreds of times.

Make the harness assert on things that are objectively checkable:

- Output pitch equals input pitch times the intended ratio, measured in cents
- Bypass is bit-exact when the effect is off, so you can prove the path is transparent
- No NaN, no infinity, no samples outside a sane range
- Every partial of a chord moved to where it should be, and nothing was left behind

Measure with a method **independent of the one inside your DSP**. If your shifter has a
pitch tracker, do not verify with that same tracker; a bug there would then hide itself.

### Vary what comes *before* the event, not just the event

This is the test-design mistake most likely to leave a bug in shipped code. DSP carries
state - delay lines, envelopes, parameter changes waiting for a safe moment to apply - so
the same input can behave completely differently depending on what preceded it.

A suite built by dropping each test case into a fresh, silent buffer exercises exactly one
path through that state, and it is usually the easy one. In one real case, every chord
test started from silence, which happened to be the single condition in which a pending
grain change was guaranteed to be installed. Played that way the output was correct; a
chord struck over still-ringing notes played on a stale grain and came out badly combed.
The suite was green the whole time.

So for anything stateful, write cases that arrive mid-stream: a note during another note's
decay, a parameter change with no gap around it, a loud passage after a quiet one. If a
result differs depending on what preceded it, that difference is the test.

## Rules for the callback thread

The audio callback runs under a hard deadline on a thread you do not own. Missing it
produces a click, and clicks are what users report as "it's broken."

- Preallocate everything at initialization, where the buffer size and format are known
- No allocation, no locks, no file or console I/O, no system calls in the callback
- Bound every loop by a size you fixed at init
- Degrade to pass-through rather than fail: a packet you cannot handle should be handed
  onward untouched, never dropped or half-processed

That last point is the difference between "the effect didn't apply to one buffer" and
"the host's audio glitched."

## Never write into the host's buffer

When an API hands you a pointer to read, it usually belongs to the driver. Copy into
memory you own, process the copy, and return your pointer instead. `IAudioCaptureClient::GetBuffer`
is the classic case: the buffer is read-only and shared, and writing to it corrupts state
you cannot see.

Handle the awkward cases explicitly, because they are where the crashes live:

- A packet larger than what you sized for at init: pass through, do not allocate
- A silence flag: the memory may be uninitialized, so synthesise the silence yourself
  rather than reading it, and still run it through your DSP so its delay line stays
  continuous
- Zero frames, or a success code that means "nothing available"

## Do not process twice when wrappers nest

If the interface you are hooking can wrap *itself* - a debug or compatibility layer that
wraps both the real device and the alternate implementation you also hook - your DSP will
run twice on the same signal, and the result sounds plausible but wrong.

Resolve it with identity, not heuristics. Give your own implementation a private marker
interface and have the hook query for it and stand down when it answers. Matching on
device names or string prefixes will break the moment someone renames something.

## Control it from a GUI without ever blocking audio

A separate process for the interface is worth it: the DSP stays in the host, the UI stays
out of the callback. Shared memory with a seqlock is the right primitive - the writer
bumps a counter to odd, writes, bumps to even; the reader retries if it saw odd or the
counter moved. Readers never block writers, so a hung or crashed GUI cannot glitch the
audio.

Publish telemetry back the same way: a heartbeat that increments every block is what tells
the UI the difference between "connected" and "the host isn't running." See
`references/patterns.md` for the seqlock and the buffer-ownership pattern in full.

Version the shared structure and refuse a mismatch rather than misreading it, since the
two binaries ship and update independently.

## Know which rung of the ladder you are on

Verification here is a ladder, and it is tempting to claim a higher rung than you reached:

1. The DSP is numerically correct offline
2. The code loads in the host and initializes
3. It processes live audio without glitching
4. It processes *real input* - an instrument, a microphone - not silence
5. **The host's own logic accepts the result**

Rung 5 is the one that decides whether the feature works, and it is invisible from the
others. A pitch shift can be mathematically perfect and still be rejected by the host's
detector because of artifacts. Say plainly which rung you have reached, especially when
handing the work to someone else.

## When a test fails, suspect the instrument first

Expect this, because it is common enough to be the default hypothesis: a failing test
often means the *test* is wrong, not the code.

Real examples of the shape:

- A pitch measurement with a lower search limit above the notes being tested reported zero
  and looked like total failure
- A latency readout sampled after the note had decayed reported the idle value, not the
  one in use while it mattered
- A test probing "is energy left at the old frequency?" caught a *shifted harmonic* of
  another note that happened to land there, and read it as leftover energy

Before changing the DSP, confirm the harness can see what you are asking about: check its
range, its timing, and whether the thing it measures could be produced by something other
than the bug you suspect. When you find the instrument was wrong, say so - a "bug" that
was never real should not be recorded as a fix.

The same discipline applies to reasoning. When an explanation and a measurement disagree,
get more data rather than a better theory: dump a spectrum, print the internal state per
block, compare a working case against a broken one. Theories about why audio sounds wrong
are cheap and usually incorrect.

## Separate physics from policy

Some limits are arithmetic and cannot be traded away. In granular pitch shifting, a grain
must span at least one period of the note or the read heads comb and the pitch drifts; a
low E period is about 12 ms, so 12 ms of latency is the price, not a tuning choice.

Do not expose a setting that lets a user violate a physical limit. A "low latency" option
that caps the grain below one period does not give lower latency with slightly worse
sound - it gives *wrong notes*, silently. Bound such settings by physics and let them vary
only across the range where the trade is real.

Test every preset against every case. A preset the suite never exercises is a preset that
is broken; the one that was only ever tested on the default setting is where this bites.

## Name settings for what you measured

If two options measure identically on everything you can check, do not name one "best" or
"high quality." Name them for the property you can demonstrate - "lower latency" against
"smoother", where smoother follows from a longer grain splicing less often.

If one option were genuinely better on every axis it would not be a setting; it would just
be what the software does. A knob earns its place only when the trade is real, and a label
that promises quality you never measured turns into support questions later.

## Derive everything from the stream

Read sample rate, channel count, bit depth and buffer size from the format the host
negotiated, and derive every filter coefficient and window length from them. Hard-coding
48 kHz or stereo works on the machine you built it on and fails quietly elsewhere - often
by degrading rather than erroring, which is the hardest kind of failure to notice.

Watch for constants that are secretly rate-dependent. A decimation factor of 4 gives one
analysis rate at 48 kHz and a completely different one at 96 kHz, which can silently move
a detector's usable range past the notes it needs to see.
