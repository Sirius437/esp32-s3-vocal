# esp32-s3-vocal

An ESP32-S3 that speaks. The chip runs the [MBROLA](https://github.com/numediart/MBROLA) diphone engine: recorded speech segments, repitched with TD-PSOLA to a note and a duration you choose. Synthesis is 16 kHz into PSRAM. The audio task upsamples 3× to 48 kHz, runs a small character chain, and writes 16-bit stereo I2S with the same sample on both channels.

The chip does not turn text into phonemes. It plays an MBROLA `.pho` line. One line is baked in and plays at boot. Another line can be sent as phoneme frames over USB-MIDI.

The engine sources under `third_party/MBROLA/` are the unmodified one-channel amalgamation. The port is `firmware/voice_mbrola/mbrola_esp.c`: allocations go to PSRAM, Xtensa is treated as little-endian, and a fatal engine error holds silence instead of exiting.

## Hardware

The wiring sketch is [ESP-VOICE-WIRING.svg](ESP-VOICE-WIRING.svg). It shows a Freenove ESP32-S3 WROOM Lite, a GY-PCM5102A, the SCK-to-GND jumper on the DAC, and the stereo line out. On that drawing the line out lands on a mixer’s channels 5 and 6. Any line input can take the same pair.

Any ESP32-S3 with **16 MB flash** and **8 MB OPI PSRAM** matches the sketch. A module without PSRAM will not load the voice.

Audio out is a **GY-PCM5102A** (or any PCM5102A I2S board). The connections on the drawing are:

| PCM5102A | ESP32-S3 |
| --- | --- |
| BCK | GPIO 5 |
| WS / LCK | GPIO 6 |
| DIN | GPIO 7 |
| VIN | 5 V |
| GND | GND |

On the DAC board, jumper **SCK to GND**. Tie **XSMT to 3V3** so the output is unmuted. Leave FLT, DEMP, and FMT at I2S, normal latency, and de-emphasis off.

The output is ground-centered line level, about 2.1 Vrms. Connect it to an amplifier, powered speakers, or a mixer line input. It is not a headphone output.

The module has two USB sockets. The **UART** socket is the one that talks to the ROM bootloader. Use it for the first upload. After that flash, the **native USB** socket enumerates as `esp32-s3-vocal`: a serial log and a USB-MIDI port. Confirm the chip before writing:

```bash
esptool.py --chip esp32s3 flash-id
```

The report should say ESP32-S3 and 16 MB.

## Dependencies

- [arduino-cli](https://arduino.github.io/arduino-cli/)
- the ESP32 Arduino core (`arduino-cli core install esp32:esp32`)
- Python 3, standard library only, for the voice image
- `esptool.py`, which ships with the ESP32 core
- a host `mbrola` binary, from the [MBROLA](https://github.com/numediart/MBROLA) project, used only to turn a voice into a ROM image

## Voice

The chip speaks with one diphone database loaded into PSRAM at boot. This tree is set up for **us1**, the American English male voice, at **16 kHz**. The phoneme names in `firmware/voice_mbrola/sampa.h`, the baked boot lines, and the duration and accent tables are that voice’s alphabet. us1 is not in the repository. MBROLA voices are free to use and are not redistributable. Download it from [MBROLA-voices](https://github.com/numediart/MBROLA-voices) (`data/us1/us1`) and make the ROM image:

```bash
mbrola -W /path/to/us1
```

That writes `us1.rom` next to the voice file. The firmware and `tools/make_ffat.py` both expect that file to be **7,282,628** bytes.

Another language is another MBROLA voice, prepared the same way (`mbrola -W /path/to/fr1`, and so on). Four things in this tree are us1-specific and have to follow the new voice:

- **Sample rate.** Synthesis is fixed at 16 kHz, then a 3× upsample to 48 kHz. Use a 16 kHz database. A voice recorded at another rate plays at the wrong speed.
- **Size.** The whole ROM is copied into the 8 MB PSRAM. us1 is about 6.9 MB. A much larger database will fail that allocation. `ROM_BYTES` in `firmware/voice_mbrola/voice_mbrola.ino` and in `tools/make_ffat.py` must both be the new file’s length. The script refuses any other size.
- **Phonemes.** A `.pho` line has to use the new voice’s own symbols. Edit the names in `sampa.h`, and the baked `HELD` and `FUNK` strings in `voice_mbrola.ino`, so the indexes and the boot line match that alphabet. `floor.h` and `accent.h` are parallel arrays on the same indexes: vowel and diphthong floors are 130 ms and 170 ms.
- **Redistribution.** Keep the new voice out of the repository, the same as us1.

## Build and load

From this directory:

```bash
arduino-cli compile --fqbn \
  esp32:esp32:esp32s3:USBMode=default,CDCOnBoot=cdc,PSRAM=opi,FlashSize=16M,FlashMode=qio,PartitionScheme=app3M_fat9M_16MB \
  firmware/voice_mbrola
```

`USBMode=default` is USB-OTG (TinyUSB). Hardware CDC mode compiles a sketch that does nothing. `PartitionScheme=app3M_fat9M_16MB` leaves a 9 MB `ffat` partition at `0x610000` for the voice. The application itself does not contain the voice.

The `.h` files in the sketch folder that point at `third_party/MBROLA/` are symlinks. The engine includes those headers by name, and this version of arduino-cli passes `build_opt.h` through without expanding `{build.source.path}`, so the sketch directory is the include path that travels with the tree. Leave the links in place.

Upload the application through the UART socket (the port name changes when the cable moves):

```bash
arduino-cli upload -p /dev/ttyACM0 --fqbn \
  esp32:esp32:esp32s3:USBMode=default,CDCOnBoot=cdc,PSRAM=opi,FlashSize=16M,FlashMode=qio,PartitionScheme=app3M_fat9M_16MB \
  firmware/voice_mbrola
```

Pack the ROM into the FAT partition image and write it at the `ffat` offset. `tools/make_ffat.py` checks the size, the `MBRO` header, and that the file bytes land at `0x9200`, which is where the firmware reads them.

```bash
python3 tools/make_ffat.py /path/to/us1.rom
esptool.py --chip esp32s3 -p /dev/ttyACM0 write_flash 0x610000 ffat.img
```

Open the native USB serial port at 115200. The boot log is `esp32-s3-vocal`, then the ROM size and the free PSRAM. The chip says a held “ah” at 130 Hz, then the line “funk it up brother funk to the beat”, and then waits. A block at 16 kHz is 20 ms of audio. The log prints how long each block took and an `underrun` count while a phrase is in the engine.

## Sending a line

USB-MIDI SysEx, manufacturer id `7D`:

```
F0 7D 01 <bar lo> <bar hi> <count> <phoneme> <dur> <note> ... F7
```

Every data byte is 7-bit. `count` is at most 48. Each frame is three bytes:

- `phoneme` — index in the table below. 0 is a rest.
- `dur` — length in sixteenth notes, minimum 1. A vowel is comfortable at 2.
- `note` — MIDI note number. 0, or phoneme 0, is unpitched. One pitch point is placed at 50% of the phoneme. Hz is `440 * 2^((note - 69) / 12)`.

`bar` 0 means the next bar. The phrase is spoken on MIDI Start (`FA`), or on the next bar while a MIDI clock is running. MIDI Stop (`FC`) ends the phrase. Until a clock has been heard, a sixteenth is 125 ms (120 BPM).

Phoneme ids are the us1 SAMPA names in `firmware/voice_mbrola/sampa.h`. Ids 7 and 17 are both `O`.

| id | name | id | name | id | name |
| --- | --- | --- | --- | --- | --- |
| 0 | `_` | 14 | `@U` | 28 | `d` |
| 1 | `i` | 15 | `aU` | 29 | `g` |
| 2 | `I` | 16 | `OI` | 30 | `dZ` |
| 3 | `E` | 17 | `O` | 31 | `v` |
| 4 | `{` | 18 | `p` | 32 | `D` |
| 5 | `A` | 19 | `t` | 33 | `z` |
| 6 | `V` | 20 | `k` | 34 | `Z` |
| 7 | `O` | 21 | `tS` | 35 | `m` |
| 8 | `U` | 22 | `f` | 36 | `n` |
| 9 | `u` | 23 | `T` | 37 | `N` |
| 10 | `r=` | 24 | `s` | 38 | `l` |
| 11 | `@` | 25 | `S` | 39 | `r` |
| 12 | `EI` | 26 | `h` | 40 | `w` |
| 13 | `AI` | 27 | `b` | 41 | `j` |

A short “ah” (phoneme `A`, eight sixteenths, MIDI note 48) is:

```
F0 7D 01 00 00 01 05 08 30 F7
```

Follow it with Start.

The Speed knob scales those sixteenths in the millisecond domain. Vowels do not go under 130 ms, and diphthongs do not go under 170 ms, so a faster setting shortens consonants and gaps first.

## Knobs

SysEx `F0 7D 03 <target> <knob> <value> F7`. Values are 0..127.

| Control | target, knob | Boot | What it does |
| --- | --- | --- | --- |
| Drive | 0, 0 | 48 | Soft clip. Higher is grit. |
| Crush | 0, 1 | 0 | Sample-hold. 0 is bypassed. |
| Ring | 0, 2 | 0 | Ring mod at 90 Hz. 0 is bypassed. |
| Telephone | 0, 3 | 0 | 300 Hz–3.4 kHz band. 0 is bypassed. |
| Formant | 0, 4 | 64 | One peaking filter, about 350 Hz–2.5 kHz. |
| Level | 1, 3 | 100 | Output level. |
| Speed | 2, 0 | 64 | 0 is as written, 64 is about 0.75, 127 is 0.50. |
| Accent | 2, 1 | 48 | 0 is flat loudness, 127 is the full per-phoneme table. |
| Air | 2, 3 | 0 | Harmonic exciter. 0 is bypassed. |

Presets are `F0 7D 02 <index> F7`:

| index | Name | Change |
| --- | --- | --- |
| 0 | Robot | Drive 80 |
| 1 | Deep | Formant toward the low end |
| 2 | Nasal | Formant toward the high end |
| 3 | Telephone | Telephone full, drive 30 |

A preset resets drive, crush, ring, telephone, and formant. It leaves level, speed, accent, and air where they are.

## License

The MBROLA engine is [AGPL-3.0](LICENSE). The firmware links that engine in, so the combined program is AGPL-3.0 as well. `third_party/MBROLA/LICENSE` is the same text.

The diphone database is not included. Download us1, or whichever voice you build, yourself, and leave it out of this tree.
