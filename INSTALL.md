# Installing RSTune

RSTune is a fork of [RS_ASIO](https://github.com/mdias/rs_asio), so the download **is**
RS_ASIO with the tuning shifter built in. You do not install both.

## What is in the download

| File | What it does |
|------|--------------|
| `avrt.dll` | The loader. Rocksmith loads this on startup, and it pulls in `RS_ASIO.dll`. **Without this file nothing happens at all.** |
| `RS_ASIO.dll` | RS_ASIO plus the pitch shifter |
| `RSTune.exe` | The control panel |
| `RS_ASIO.ini` | Audio configuration template |
| `LICENSE` | MIT, covering upstream RS_ASIO and this fork |

## Install

1. Find your Rocksmith 2014 folder. In Steam: right click the game, Manage, Browse local
   files. It is the folder containing `Rocksmith2014.exe`.
2. **If you already use RS_ASIO**, back up your existing `RS_ASIO.dll` and `avrt.dll`
   first, and keep your current `RS_ASIO.ini` rather than overwriting it. Your audio
   settings are already correct and this release does not change how they work.
3. Copy the files from the download into that folder, next to `Rocksmith2014.exe`.
4. Run `RSTune.exe` from the same folder. It remembers its settings in `RSTune.ini`
   beside itself, so keep it there rather than moving it to the desktop; make a shortcut
   if you want one.

Windows will likely warn that `RSTune.exe` is unrecognised, because it is not code
signed. That is expected for a small free tool. More info, then Run anyway, or check the
file on VirusTotal first if you would rather.

## Audio setup

If you are new to RS_ASIO, its own [documentation](https://github.com/mdias/rs_asio) and
its `docs` folder cover per interface setup and are the right place to start; RSTune does
not change any of it. The short version is that you edit `RS_ASIO.ini` and put your ASIO
driver's name in the `Driver=` fields.

RSTune works on either input path:

- **ASIO** (`EnableAsio=1`). Best latency. This is the tested path.
- **WASAPI** (`EnableWasapiInputs=1`), which is also how a Rocksmith Real Tone cable is
  normally seen.

It never touches audio output, so whatever you play back through is up to you.

## Using it

1. Start `RSTune.exe`.
2. Set **My guitar is tuned to** and **Play songs written in**.
3. Tick **Shift my guitar to the target tuning**.
4. Start the game. There is a button for it.

Leave the window open while you play. It shows which inputs it is reaching, the live added
latency, and your input level.

Rocksmith's own tuner will read the target tuning, so tune your guitar normally to the
tuning you told RSTune you are in, and let it do the rest.

## If nothing happens

The window itself tells you most of what you need:

- **"Game: not running"** while the game is open means no audio is reaching RSTune. Check
  `EnableAsio` or `EnableWasapiInputs` in `RS_ASIO.ini`.
- **A message about `ForceWDM` or `ForceDirectXSink`** means those are set in
  `Rocksmith.ini` and route the game's audio around RSTune entirely. Set them to `0`.
- **No inputs listed** means the game never opened a capture stream. `RealToneCableOnly=1`
  in `Rocksmith.ini` with no cable and no ASIO device is one way to end up here.

`RS_ASIO-log.txt` appears next to the game and lines containing `RSTune` show what it saw.
Please include it in any bug report, along with your interface, sample rate, and whether
you are on ASIO or WASAPI.

## Uninstalling

Delete `RSTune.exe` and `RSTune.ini`, and put your original `RS_ASIO.dll` and `avrt.dll`
back. If you did not use RS_ASIO before, deleting `avrt.dll`, `RS_ASIO.dll` and
`RS_ASIO.ini` returns the game to stock.
