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
