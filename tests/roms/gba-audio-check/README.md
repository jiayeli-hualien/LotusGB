# LotusGB GBA Audio Check

These are original, MIT-licensed GBA homebrew ROMs generated from
[`generate.py`](generate.py). `audio-check.gba` enables the GBA PSG, routes
sound to both channels, and plays a steady square-wave tone at approximately
440 Hz. `direct-sound-check.gba` alternates positive and negative samples
through Direct Sound FIFO A and B at 4096 samples per second using
timer-triggered repeat DMA. The alternating samples produce an approximately
2048 Hz signal, with A routed right and B left. The ROMs contain no Nintendo
logo, commercial game code, or third-party assets.

Together, the ROMs provide repeatable PSG and Direct Sound output baselines for
LotusGBA. They do not establish accurate analog sound or hardware timing. The
local `gba_audio_test` runner checks deterministic PSG output and verifies the
tone remains between 420 and 460 Hz, and verifies the Direct Sound signal
remains between 1950 and 2150 Hz, with separate stereo routing, timer-triggered
DMA playback, the 50%/100% Direct Sound volume selection, and SOUNDBIAS cadence,
clipping and 9–6-bit PWM amplitude resolution. It also exercises GBA Sound 3's
inactive-bank CPU access, 32-step and 64-step playback, and force-75%-volume
mode. A high-rate PSG check confirms that an 8.192 kHz tone passes while a
32.768 kHz tone is
attenuated before conversion to the 48 kHz frontend stream.

Regenerate the binary with:

```sh
python3 tests/roms/gba-audio-check/generate.py
```

License: MIT; copyright 2026 LotusGB contributors.
