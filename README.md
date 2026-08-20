# RSTune

Built on [RS_ASIO](https://github.com/mdias/rs_asio) by Micael Dias (MIT). The upstream
project adds ASIO support to Rocksmith 2014; this fork keeps all of that and adds a pitch
shifter to the guitar input path.

Real-time tuning shifter for Rocksmith 2014. Pick the tuning your guitar is actually in
and the tuning the song is written in; RSTune pitch shifts your guitar signal on its way
into the game. You hear the shifted tuning, and the game's own tuner and note detection
both agree with it, so you can play a D Standard song with your guitar in E Standard
without touching a peg.

## How to use it

1. Start `RSTune.exe` from the Rocksmith folder.
2. Set "My guitar is tuned to" and "Play songs written in".
3. Tick "Shift my guitar to the target tuning".
4. Launch the game (there is a button for it) and leave RSTune open while you play.

Settings are remembered in `RSTune.ini` next to the exe.

## What it can and cannot do

RSTune only performs a **uniform** shift, moving all six strings by the same number of
semitones. That covers every standard-to-standard conversion (E, Eb, D, C#, C, B, Bb, A)
and every drop-to-drop conversion (Drop D, C#, C, B, Bb, A, G).

Converting *between* the families, such as E Standard to Drop D, needs the low string
moved on its own while the others stay put. That is a different and much harder problem,
and doing it badly wrecks both the sound and the game's note recognition, so the target
list simply hides those combinations rather than faking them.

## Latency

Pitch shifting costs latency and there is no way around it: a clean shift needs a grain
at least one period of the note long, and a low E period is 12 ms. Typical added latency
is 6 ms on the high strings, 12 ms on the low ones, and up to about 25 ms on low power
chords where the grain has to span the whole chord period. The live figure is shown in
the window. If timing feels late, raise Rocksmith's own calibration to compensate.

The Quality setting caps how much latency the shifter may spend. Tight keeps it lowest
but can leave some roughness on low chords; Smooth gives the cleanest chords.

## How it works

`RS_ASIO.dll` is a fork of RS_ASIO with a pitch shifter inserted into the input path, in
`RSAsioAudioClient::OnAsioBufferSwitch` - after the ASIO input has been laid into the
game's buffer format and before the game's own volume stage. Only instrument inputs are
touched; the microphone client is left alone.

The shifter is a two-head granular delay line with sin/cos crossfade windows. Its one
critical detail is that the grain length is locked to a whole number of periods of the
detected fundamental. Without that the two heads comb-filter and the pitch the game sees
drifts tens of cents flat, which was measured and fixed during development.

`RSTune.exe` never touches audio. It publishes settings into a small shared memory block
through a seqlock, which the audio thread reads without ever blocking, so the GUI cannot
glitch or stall the audio even if it hangs.

## Layout

    RS_ASIO/RSTune/     the pitch shifter, shared memory, tuning table
    RSTuneApp/          the control panel
    RSTuneTest/         offline harness, 117 checks
    backup-original/    your original RS_ASIO.dll, avrt.dll and RS_ASIO.ini
    build_app.cmd       builds RSTune.exe
    build_test.cmd      builds and is run to validate the DSP

Build the DLL with:

    msbuild RS_ASIO.sln /p:Configuration=Release /p:Platform=x86

## Reverting

Copy `backup-original\RS_ASIO.dll` back over the one in the Rocksmith folder and delete
`RSTune.exe`. `avrt.dll` was never modified.

## Not included in v1

A cents fine-tune field was prototyped and removed. The value reached the audio engine
correctly but the on-screen field did not reliably display it, and a control that
disagrees with what is actually applied is worse than no control. The audio path still
carries a cents parameter for a later build.

## Compatibility

**ASIO interfaces:** hardware agnostic. The shifter works on the game's buffer format,
converting through RS_ASIO's existing routines, which cover 16, 24 and 32 bit integer
plus 32 and 64 bit float in any combination. Every time constant is derived from the
stream, so 44.1, 48, 88.2, 96 and 192 kHz all behave the same, and buffer size is
whatever the driver gives.

**WASAPI inputs:** supported. When RS_ASIO hands the game the real system devices
(`EnableWasapiInputs=1`), the shifter runs inside `DebugWrapperCaptureClient::GetBuffer`,
which already sits between the driver and the game. WASAPI hands out packets of varying
size and its buffer belongs to the driver, so a processed copy is made in memory RSTune
owns and that pointer is returned instead. Packets larger than the client's buffer size
are passed through untouched rather than allocating on a realtime thread.

**Real Tone cable:** works, because it is an ordinary WASAPI capture device. It also
works through ASIO4ALL if you prefer that route.

**Not shifted twice:** the ASIO devices are themselves wrapped by the same
`DebugWrapperAudioClient` that the WASAPI hook lives in. RSAsioAudioClient answers to a
private marker interface so the WASAPI side recognises it and stands down.

**Microphones:** every capture stream the game opens is listed in the window with a
checkbox, so you can untick anything that is not your guitar. There is no reliable way
to tell a guitar interface from a headset automatically, so RSTune does not pretend
there is; it only pre-unticks a stream that looks like a microphone the first time it
sees one, and only when the game has microphone support switched on.

**What will defeat it:** `ForceWDM=1` or `ForceDirectXSink=1` in Rocksmith.ini route the
game's audio around the interfaces RSTune hooks. The window says so explicitly rather
than leaving you to guess.

### Is anything specific to one interface?

No. There are no device or vendor names anywhere in the code, and nothing assumes a
particular channel count, buffer size or sample rate. Everything comes from the stream
the game negotiated:

- **Sample rate** is read from the stream and every filter coefficient, grain bound and
  the pitch tracker's decimation are derived from it. Verified at 44.1, 48, 88.2, 96 and
  192 kHz.
- **Sample format** goes through RS_ASIO's conversion routines: 16, 24 and 32 bit
  integer and 32 and 64 bit float, in any combination. Verified for 16/24/32 bit PCM and
  32 bit float, mono and stereo.
- **Channel count** is whatever the device reports; each channel gets its own shifter.
- **Buffer size** is whatever the driver gives, fixed or ragged.

The one machine-specific thing left is `RS_ASIO.ini`, which is RS_ASIO's own config and
is where you name your ASIO driver. That is not shipped with RSTune.

### Output driver, exclusive mode, and what actually breaks it

**The output driver is irrelevant.** RSTune only ever touches the capture path. There is
no RSTune code in either render client, and on the ASIO side the hook sits inside the
input branch only. Whatever the game plays back through, and whether that is ASIO or
WASAPI, shared or exclusive, RSTune never sees it and cannot affect it.

**Exclusive and shared mode both work on the input.** Exclusive gives fixed size packets
and shared gives variable ones; the hook handles either, and ragged packet sizes are part
of the test suite. `ExclusiveMode` in Rocksmith.ini changes what the game asks for, not
whether RSTune can sit in the path.

**Unsupported formats degrade to a pass-through**, they do not fail loudly or produce
garbage: 8 bit, 20 bit and 16 bit float are all declined cleanly and the driver's buffer
is handed to the game untouched.

Things that genuinely stop it working:

- `ForceWDM=1` or `ForceDirectXSink=1` in Rocksmith.ini. The game routes around the
  interfaces RSTune hooks. The window says so.
- The game never opening a capture stream at all. `RealToneCableOnly=1` with no cable and
  no ASIO device is one way to land here.
- Windows having "allow applications to take exclusive control" switched off for the
  input device while the game asks for exclusive mode. Worth knowing that this stops
  Rocksmith getting any input at all, with or without RSTune.

In other words the remaining failure modes are ones where the game itself has no working
guitar input; there is no known configuration where Rocksmith gets audio and RSTune
cannot reach it, other than the two Rocksmith.ini switches above.

### Bass

A uniform shift applies to bass exactly as it does to guitar, and the pitch tracker
reaches down to about 30 Hz so a 5 string low B is covered. One caveat measured rather
than assumed: a 30.9 Hz low B needs a grain longer than the Balanced preset's latency
ceiling allows, so it lands 18 cents flat there and 0.3 cents flat on Smooth, for 3 ms
more latency. **On a 5 string bass, use Smooth.** Every other bass note is identical on
both presets.

## Reading the game's configuration

RSTune reads `Rocksmith.ini` from inside the game process, once per game launch, which
is the cadence that matters: those settings only take effect when the game starts, so
re-reading more often would tell you nothing new. The GUI receives the result through
telemetry, so it refreshes on its own whenever you start the game.

It is used for two things: deciding whether to bother hinting that a stream might be a
microphone, and telling you plainly when the audio path cannot reach RSTune at all.
The authority on whether shifting is happening is always the live telemetry, never the
ini; the ini only explains why when the answer is no.
