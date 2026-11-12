# Flipper Pager v2.0

Receive POCSAG and FLEX pager messages — and now transmit them.

## ⚠️ Upgrading from 1.x — one manual step

Copy **`ABOUT.txt`** from the release to **`/ext/pocsag/ABOUT.txt`** on your SD card, alongside `settings.txt`. The About screen reads its text from there instead of carrying it in RAM. Without the file the app still works; the About screen just shows a short credit line and a link to this repo.

## Transmit (new)

Send to your own node, from **Transmit** in the main menu.

- **POCSAG** alphanumeric at 512 / 1200 / 2400 baud, function bits 0–3, normal or inverted polarity
- **FLEX** alphanumeric at 1600/2FSK
- Per-shot TX frequency correction, ±15 kHz in 0.5 kHz steps
- A review screen before anything goes out

Transmit only where you are licensed and authorised.

## Decoding

- Complete blocks are now recovered from **interrupted frames** rather than discarding the whole frame
- The **BIW checksum is validated** before a phase is parsed, so corrupt phases no longer produce junk pages
- **Weak Decode** — opt-in soft-decision (Chase) recovery of FLEX4 words that fail BCH. Off by default; costs nothing when off. Pages it recovered are marked `W`
- Hardened FLEX **fragment reassembly** and multi-part joining
- **Inverted POCSAG** is decoded automatically
- POCSAG produces **one entry per message** instead of one per address

## Frequency accuracy

- **Crystal correction in ppm**, applied to both RX and TX tuning, adjusted with left/right. One value works on every band; `+` corrects a carrier that runs low

## Filtering

- Independent **Hide/Show per FLEX page kind** — Alphanumeric, Numbers, Tone only, Binary — replacing the old combined type filter, which overlapped the binary toggle
- **Duplicates**: Hide, Show, or **CAPS** (show copies to different capcodes, hide same-cap retransmits)
- **Phase** (A–D) and **frame** filters
- **Capcode** and POCSAG **RIC** filters, each off / exact / range, with a shortcut to filter on the address of the selected message
- Hide **encrypted or opaque** payloads (FLEX secure pages, wholly opaque Base64/hex)
- Hide **partial** messages that could not be fully decoded
- Filtered pages are still written to the session log

## Alerts and indicators

- **Alert**: sound, vibration, both, or off
- **Alert Tone**: Default, or **Classic** — a rate-limited Motorola-style tone
- **Signal Sound** while receiving: Raw (the data line), Tone (FLEX4 levels as pitches, squelched between frames), or Synth (from the decoder's recovered symbols, clean)
- Configurable **Signal Vol / Pitch / Spread**
- **Receive** and **Decode LED** indicators, each off or one of 14 colours
- Receive alerts no longer wake the backlight while the keyboard is locked

## Clock

**Time sync** from the FLEX network: minute and second, or the full clock once the network hour is known, kept continuously in step with the RTC.

## Interface

- Hold **Back** to leave the receiver; double-tap Back to jump to the newest message
- In the message viewer, press **Down past the end** to load the next message, or **Up past the start** for the previous one (opening at its end, so scrolling back reads continuously)
- Saved logs in **Review Messages** share the live list's layout and navigation
- Long messages cache their layout while being viewed

## Presets and frequencies

- **POCSAG** and **POCSAG-W** presets (POCSAG-W widens the channel filter for the crystal offset at 900 MHz)
- **FLEX4-3200** and **FLEX4-6400** four-level presets
- More default POCSAG and FLEX channels; custom frequencies from `/ext/pocsag/settings.txt` appear at the end of the list

929–932 MHz is outside the CC1101's rated range — use at your own risk.

## Fixes

- **NFC no longer stops working after using the app.** FLEX4 sampling left the CC1101 driving GDO1, which shares its pin with the SPI MISO line the NFC chip uses, so the radio contended with it on every NFC transfer
- **Fixed a crash when changing any filter mode.** The config screen cached row pointers that the list invalidated when it grew, so writing a row's text went through freed memory — which is why it came and went unpredictably
- Saved-log entries are sized from the next entry's offset rather than the end of the file
- Capcode and RIC filter controls behave correctly in range mode

## Memory

Long receive sessions are bounded by a rolling history window, and receive-time buffers — the Weak Decode reliability table, FLEX partial-repair slots, part-joining text and the BCH syndrome table — are released when you leave the receiver, so Review and Transmit are not starved after a long session.

The resident image is **51.4 KB**, down from 55.7 KB.

## Diagnostics

**Raw Capture** records the FLEX4 sample stream to SD, and **Stats Log** writes periodic counter snapshots to `/ext/pocsag/stats.txt`, for untethered reception testing.

## Credits

Built on work by **@xMasterX & @Shmuma**, with protocol improvements by **@htotoo** and FLEX decoding by **@Geofferey**. FLEX decoding is based on **multimon-ng** (Craig Shelley, Bruce Quinton, Jason Lingohr and contributors) and **PortaPack Mayhem** FLEX RX/TX (@timelf123, @VasylSamoilov). Icons by **@Svaarich**.
