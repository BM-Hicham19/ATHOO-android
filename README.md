# Adventure Time (Android 1.0.8) → PS Vita & Android ("ATHOO")

This is **not a rewrite of the game**. It is a small Dalvik virtual machine written in C (`src/vm.c`,
~800 lines) that runs the game's *own* `classes.dex` on the Vita, plus native implementations of the
~250 Android / Java calls the game makes (`src/jlib.c`, `src/droid.c`): a software canvas, bitmaps,
PNG decoding, streams, strings, input, file storage. Game code, art and levels come from **your own APK**;
nothing from the game is in this repository or in the VPK.

## Status (what has and hasn't been verified)

Verified on a Linux PC, running the real `09617-adventuretime-v1_0_8.apk` headless:
language select → title → main menu → New Game → overworld with Finn & Jake, dialogue, HUD,
walking with the virtual stick, screen transitions, save files; 5,000+ frames under AddressSanitizer/UBSan
with no errors, and the garbage collector stress-tested with a tiny heap threshold.
About 0.8 ms/frame on a desktop CPU; peak memory ~55 MB.

**Not verified:** anything on real Vita hardware. `src/main_vita.c` (display, pad, touch, threads) was only
syntax-checked against stand-in headers, so expect to fix small SDK-signature mistakes on first build.
Vita performance is an estimate (the Cortex-A9 should manage 30 fps, but I haven't seen it).

**Audio:** music (the game's 8 MIDI tracks) is played by a small built-in synthesizer, so it sounds chiptune-like
rather than like a real soundfont. Sound effects are bundled as WAV. Verified on
the PC by capturing the audio: the right MIDI notes at the right times, sample-exact effect playback, correct
restart/overlap/stop behaviour. **Not verified by ear or on Vita hardware.**

**Not implemented:** `Canvas.drawText` (the game draws its text with its own bitmap fonts, so this should rarely matter),
ads / analytics / "More Games" / "Invite" (switched off on purpose).

## Build without installing anything (GitHub Actions)

Create a new GitHub repository, upload the *contents* of this folder (not your APK), and open the
**Actions** tab: the "Build VPK" workflow compiles it inside the official `vitasdk/vitasdk` image and
attaches `AdventureTimeVita.vpk` as a downloadable artifact. (Written from memory, not run by me.)

## Build locally (needs vitasdk, see vitasdk.org for installing it)

    export VITASDK=/usr/local/vitasdk        # wherever yours is
    mkdir build && cd build && cmake .. && make
    # -> AdventureTimeVita.vpk

Install the VPK with VitaShell, then copy your APK to the Vita as

    ux0:data/AdventureTimeVita/game.apk

(Tested with v1.0.8, package `com.globalfun.adventuretime.free`.) Save data is written next to it.

## Android version (any Android 8 - 17 phone, including the Galaxy S10)

`android/` is the Android app, named **ATHOO**: a NativeActivity app that wraps the same game runtime (no Dalvik, no ad SDKs, no old-API problems),
so it runs on current Android. Targets API 35 (works on Android 16 / 17), arm64 + 32-bit ARM + x86_64,
16 KB-page-size compatible.

1. In your **private** GitHub repo, put your game APK at `android/app/src/main/assets/game.apk`.
2. Push. The "Build Android APK" workflow builds `app-release.apk` (artifact `AdventureTime-android-apk`; unzip it).
3. Install the APK on the phone (allow "install unknown apps").

Controls: touch works as in the original game (landscape only); a gamepad also works (d-pad / left stick = move,
A/B/R1 = action, X/L1 = switch character, Y = skip dialogue, Start = pause). The Back button closes the app.
Saves and `log.txt` are in the app's private storage; `adb logcat -s avm` shows the same log.
The APK is signed with the personal key `android/atvm.jks` (committed on purpose so later builds install over
earlier ones), so keep the repo private. **Not yet built or run on a device by me**: the Android SDK isn't available
in my environment, so the first CI run may need a small fix; paste me the error log.

## Controls (PS Vita)

| Vita                      | Game                                   |
|---------------------------|----------------------------------------|
| D-pad / left stick        | move (drives the game's virtual stick) |
| Cross / Circle / R        | action button                          |
| Square / L                | switch character                       |
| Triangle                  | skip dialogue                          |
| Start                     | pause                                  |
| Start + Select            | quit                                   |
| Front touch screen        | works as on the phone (menus, etc.)    |

Button positions are in `src/buttons.c` if you want to move them.

## Sound

Music: the game's 8 MIDI tracks are played by the built-in synthesizer (read straight from your APK).
Sound effects: the 7 effects were converted from the APK's OGG/MP3 files to WAV and are bundled in `audio/`;
the build puts them inside the VPK (they are read from `app0:audio/`). Nothing to copy by hand.
`tools/extract_audio.py` (needs ffmpeg) regenerates them from a different APK version if you ever need to.

Note: the game itself targets about 14 logic frames per second (it sleeps 70 ms per frame), so motion will
look like the original phone version, not 60 fps.

## Icon and LiveArea artwork

`sce_sys/` holds the bubble icon (`icon0.png`, 128x128) and LiveArea background / start image (840x500 and
280x158, all 8-bit indexed PNGs) made from the title art and the Finn icon. An earlier icon I generated made one
Vita refuse the install (error 0x8010113D), so the GitHub build produces **two** VPKs:

* `AdventureTimeVita-with-artwork`: try this one first;
* `AdventureTimeVita-no-artwork`: identical game, no icon/LiveArea assets; install it if the first one fails.

Locally: `cmake -DWITH_ARTWORK=OFF ..` builds the plain version.

**Keep your GitHub repository private**: `audio/` and `sce_sys/` contain Cartoon Network / game assets you may
keep for your own use but shouldn't publish.

## When something goes wrong

* A red screen for 5 s means the game failed to start; read `ux0:data/AdventureTimeVita/log.txt`.
* Create an empty file `ux0:data/AdventureTimeVita/debug` to also log the game's own messages.
* The log lists any Android call that isn't implemented (`unimplemented native: ...`).

## Desktop test harness

    make -f Makefile.linux
    ./atvm_test game.apk 1800 300 "130:960:256;400:960:540;700:960:316" "1300-1500:1:0:0"
    #           apk       frames dump  taps (frame:x:y, DISP coords 1920x1080)   pad (f0-f1:dx:dy:act)

Frames are written to `/tmp/atframes/*.ppm`. `AVM_TRACE=2` prints every method call, `AVM_LOG=1` the game's
log output, `AVM_THROWLOG=1` every exception, `AVM_GC_KB=2048` forces frequent garbage collection.

## Code map

    src/vm.c        dex loader, class model, GC, interpreter
    src/jlib.c      java.lang / java.util / java.io natives
    src/droid.c     android.* natives, software canvas, boot sequence
    src/res.c       zip + inflate, PNG decoder, resources.arsc parser
    src/input.c     multitouch -> Android MotionEvents      src/buttons.c  gamepad -> touch UI
    src/main_vita.c Vita frontend                           src/main_linux.c  test frontend

The game believes it runs on a 1920x1080 phone (so it picks its 640x360 asset set); the canvas renders at
960x540 and the Vita's 1920x1088 touch panel maps straight onto the game's coordinates.
