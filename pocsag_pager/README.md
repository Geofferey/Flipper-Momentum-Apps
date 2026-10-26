# Flipper Pager

POCSAG and FLEX pager receiver for the Flipper Zero CC1101 radio.

Protocol implementation made by @Shmuma, 512 and 2400 speeds support by @htotoo.
Plugin based on [Weather Station](https://github.com/flipperdevices/flipperzero-firmware/tree/dev/applications/plugins/weather_station) from OFW.
Icons by @Svaarich.

## Disclaimer

This app is provided as is, without warranty, for education, research and
receiving traffic you are authorized to receive. You are responsible for how
you use it.

**Frequency range.** The CC1101 is rated for 300-348, 387-464 and
779-928 MHz. The US FLEX paging bands (929-932 MHz) are outside that range
and only work with the firmware's extended frequency range enabled. Operation
there is out of specification: sensitivity and stability are not
guaranteed, and the extended range is unlocked at your own risk. This app
only receives, but the extended range applies to the whole device;
transmitting outside the bands the Flipper is certified and you are licensed
for is illegal in most countries.

**Intercepting pager traffic.** Pager messages are communications between
other people. In the US, intercepting the contents of display pager messages
is restricted by the Electronic Communications Privacy Act (18 U.S.C. 2511),
and divulging or using intercepted radio communications by 47 U.S.C. 605;
other countries have similar laws. Check what applies where you are before
decoding anything.

**Sensitive personal information.** Decoded messages and logs can contain
personal and sensitive information, including names, phone numbers,
addresses and medical or emergency details. Do not share, publish or upload
received messages, screenshots or log files. Prefer Temp logging, which keeps
nothing after the app exits. If you use Save, protect the SD card and delete
logs you no longer need. Deleting a file does not reliably erase it from flash
storage.

## Supported signals

- POCSAG 512, 1200 and 2400
- FLEX 1600/2, 3200/2, 3200/4 and 6400/4, with short and long (two-word) addresses
- FLEX alphanumeric, numeric, tone and binary pages; secure pages are shown as alphanumeric

Built-in radio presets:

- `POCSAG` (default) for POCSAG: 58 kHz filter, no carrier-sense squelch
- `POCSAG-W` for POCSAG at 900 MHz, with a 116 kHz filter for crystal offset
- `FM95`, the original POCSAG preset (270 kHz filter with carrier sense)
- `FLEX` for two-level FLEX (1600/2 and 3200/2), about +/-4.76 kHz deviation
- `FLEX4-3200` and `FLEX4-6400` for four-level FLEX. Both decode every FLEX
  rate; they differ only in the CC1101 data-rate setting.

The `FLEX4` presets read the CC1101 4-FSK hard decisions (RX_HARD_DATA) from
the GDO0 and GDO1 pins, sampled at 25.6 kHz. GDO1 shares the SPI MISO line, so
RSSI display and frequency hopping are off while they are active. External
modules must expose GDO1 on MISO, as standard CC1101 modules do.

## Using the app

- **Receive messages** lists messages as they arrive; OK opens one (Up/Down
  scroll long messages). Left opens the receiver Config.
- **Review messages** opens a saved session log from `/ext/pocsag/logs`.
- **Settings** is the same configuration, available before receiving:
  frequency, hopping, modulation, FLEX page types, binary pages, duplicates,
  encrypted messages, logging and a capcode filter. Settings are saved to
  `/ext/apps_data/pocsag_pager/settings.txt` and restored at startup.

FLEX decoding features:

- Every page in every phase of a frame is decoded.
- Long messages that the network splits into fragments are joined into one
  entry (`S<parts>J` in the header); missing parts are marked `[...]`.
- The alphanumeric header checksum is verified; a page that fails it is
  marked `?`.
- Pages with uncorrectable words are repaired from later copies when the
  network retransmits them (marked `R`).
- Duplicates (same capcode and text within 5 minutes, ignoring a trailing
  `[NN]` counter) can be hidden.
- Messages that look like encrypted payloads (FLEX secure pages, or one
  unbroken base64 run) can be hidden.

## Logging

Messages are written to the SD card as they arrive and the list keeps only a
small index in RAM, so long sessions do not run out of memory.

- **Temp** (default): one hidden file, `/ext/pocsag/.session.tmp`, overwritten
  each receive session. It is overwritten with zeros and deleted when a new
  session starts, when the app exits, and at the next start after a crash.
  This is best effort: SD card wear levelling can keep old data in cells the
  file system no longer uses.
- **Save**: one log per receive session in `/ext/pocsag/logs`, named by start
  time and frequency, e.g. `2026-10-25_04-48-13_929.6125.txt`. One line per
  message: time, frequency, protocol, header and message, tab-separated.

Logs can contain other people's personal information; see the
[Disclaimer](#disclaimer).

## Frequencies

On first start the app creates `/ext/pocsag/settings.txt` (a standard Flipper
Sub-GHz setting file) with the standard Sub-GHz frequencies plus these paging
channels:

POCSAG: 439.9875 (DAPNET), 448.425, 465.970 and 466.075 MHz (German networks),
from [Wikipedia](https://en.wikipedia.org/wiki/POCSAG) and
[DAPNET](https://en.wikipedia.org/wiki/DAPNET).

US FLEX: 929.3625, 929.3875, 929.4625 (test channel), 929.5375, 929.5875, 929.6125,
929.6625, 929.9375, 931.0625, 931.2125, 931.3375, 931.8625 and 931.9375 MHz,
from the [RadioReference paging wiki](https://wiki.radioreference.com/index.php/Paging)
and the [Signal Identification Wiki](https://sigidwiki.com/wiki/FLEX).

They are above the CC1101's 928 MHz rating and only appear if the extended
frequency range is enabled (see the [Disclaimer](#disclaimer)). An existing `settings.txt` is never overwritten;
edit it to add your own `Frequency:` lines (in Hz, e.g. `Frequency: 929612500`),
or delete it to get the defaults back. Custom frequencies appear at the end of
the frequency list. See the Flipper
[setting_user.example](https://github.com/flipperdevices/flipperzero-firmware/blob/dev/applications/main/subghz/resources/subghz/assets/setting_user.example)
for the file format. The DAPNET POCSAG frequency 439.9875 MHz is the default.

## References for the FLEX decoder

The FLEX decoder was written for this app, using these projects as references:

- **[multimon-ng](https://github.com/EliasOenal/multimon-ng)**, `demod_flex.c`
  and `bch.c` (GPL-3.0-or-later). The protocol structure follows it: sync
  words and the mode table, frame information word decoding, block
  deinterleaving into phases A-D, BCH(31,21) error correction (derived from
  its BCH implementation), block information, address and vector word
  parsing, page types, alphanumeric parsing including the signature slot
  and fragment flags, BCD numeric and tone pages, the long-address range
  test and the extra data word that long numeric and tone pages carry, and
  binary pages shown as hex like its `parse_unknown`. The long-address
  capcode formula is PDW's, as quoted in multimon-ng. Its FLEX decoder
  credits Craig Shelley, Jason Lingohr, Bruce Quinton, Ramon Smits, Rob0101,
  bierviltje and bertinholland, building on GNU Radio code (Free Software
  Foundation).
- **[PortaPack Mayhem](https://github.com/portapack-mayhem/mayhem-firmware)**.
  Its FLEX receiver (`proc_flex.cpp`) informed the four-level approach of
  tracking symbol phase and taking the majority of samples near each symbol
  centre. Its FLEX transmitter (`flex_encode_alpha` in the `flex_tx` app) was
  the reference for the alphanumeric header word: the checksum, message
  number and retrieval flag (which marks retransmitted copies), and the
  placement of the header in the second vector slot for long addresses.
  Mayhem FLEX transmissions were also used for testing. Mayhem's FLEX
  receiver is by @timelf123 (Tim Elfelt) and @VasylSamoilov, and its FLEX
  transmitter by @VasylSamoilov (from the project's git history).

FLEX decoding for this app by @Geofferey.

Written for this app: the CC1101 GDO sampling, the symbol clock recovery
(a fractional-phase DPLL that follows the 1600 to 3200 baud switch),
repair from multiple copies (including a two-copy BCH search),
fragment joining, duplicate and encrypted detection, and SD logging.

All contributions are welcome!
