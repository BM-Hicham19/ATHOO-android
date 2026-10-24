#!/usr/bin/env python3
"""Convert the game's OGG/MP3 sound effects (inside your APK) to WAV files the Vita app can play.

    python3 tools/extract_audio.py game.apk [output_dir]

Needs ffmpeg on your PATH (https://ffmpeg.org).  Then copy the output folder (default ./audio) to the Vita as
    ux0:data/AdventureTimeVita/audio/
The MIDI music is played by the app itself and needs no conversion."""
import os, shutil, subprocess, sys, tempfile, zipfile

EXTS = ('.ogg', '.mp3', '.m4a', '.aac', '.amr', '.flac', '.wav')

def main():
    if len(sys.argv) < 2:
        print(__doc__); return 1
    apk, out = sys.argv[1], (sys.argv[2] if len(sys.argv) > 2 else 'audio')
    if not shutil.which('ffmpeg'):
        print('ffmpeg not found on PATH - install it first (e.g. "winget install ffmpeg", "brew install ffmpeg", "apt install ffmpeg").'); return 1
    os.makedirs(out, exist_ok=True)
    done = 0
    with zipfile.ZipFile(apk) as z, tempfile.TemporaryDirectory() as tmp:
        for name in sorted(z.namelist()):
            base, ext = os.path.splitext(os.path.basename(name))
            if not name.startswith('res/raw/') or ext.lower() not in EXTS:
                continue
            src = os.path.join(tmp, base + ext)
            with open(src, 'wb') as f: f.write(z.read(name))
            dst = os.path.join(out, base + '.wav')
            r = subprocess.run(['ffmpeg', '-y', '-loglevel', 'error', '-i', src, '-ac', '1', '-c:a', 'pcm_s16le', dst])
            print(('ok    ' if r.returncode == 0 else 'FAILED'), name, '->', dst)
            done += r.returncode == 0
    print(f'\n{done} file(s) converted. Copy "{out}" to ux0:data/AdventureTimeVita/audio/ on your Vita.')
    return 0

if __name__ == '__main__':
    sys.exit(main())
