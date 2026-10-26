# Flipper Pager

POCSAG and FLEX pager receiver, plus a POCSAG transmitter, for the Flipper Zero CC1101 radio.

Protocol implementation made by @Shmuma, 512 and 2400 speeds support by @htotoo.
Plugin based on [Weather Station](https://github.com/flipperdevices/flipperzero-firmware/tree/dev/applications/plugins/weather_station) from OFW.
Icons by @Svaarich.

## Disclaimer

This app is provided as is, without warranty, for education, research,
receiving traffic you are authorized to receive, and sending pages to systems
you own or are authorized to use. You are responsible for how you use it.

**Frequency range.** The CC1101 is rated for 300-348, 387-464 and
779-928 MHz. The US FLEX paging bands (929-932 MHz) are outside that range
and only work with the firmware's extended frequency range enabled. Operation
there is out of specification: sensitivity and stability are not
guaranteed, and the extended range is unlocked at your own risk. Transmit only
on frequencies where your device is permitted and you are licensed or otherwise
authorized. The app keeps the firmware's regional transmit checks in place.

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
- **Send messages** composes an alphanumeric POCSAG page for a destination RIC.
  It can also send an alphanumeric FLEX page to a short capcode. Select the
  protocol, frequency and polarity, enter up to 80 characters, review the
  complete page, then explicitly press Send. POCSAG supports functions 0-3 and
  512/1200/2400 baud. FLEX transmission currently supports 1600/2FSK and short
  capcodes 1-1933312; the CC1101 asynchronous data input cannot directly emit
  the four frequency levels required by FLEX 3200/4FSK or 6400/4FSK. FLEX sends
  acquisition bursts followed by a new page and a retransmitted copy. Use
  Standard polarity unless the target requires an inverted signal. Optional TX
  correction offsets the carrier from -10.0 to +10.0 kHz in 0.5 kHz steps and
  defaults to Off. This is intended for licensed operators testing their own
  pager infrastructure; the app does not bypass firmware region restrictions.
- **Settings** is the same configuration, available before receiving:
  frequency, hopping, modulation, FLEX page types, binary pages, duplicates,
  encrypted messages, logging and a capcode filter. Settings are saved to
  `/ext/apps_data/pocsag_pager/settings.txt` and restored at startup.

Receiver settings and the transmitter protocol, frequency, address, function,
baud, polarity and correction are saved on a clean app exit. Message text is
deliberately not saved.

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

## References for the FLEX transmitter

The 1600/2FSK transmitter uses the FLEX word layouts already exercised by the
receiver and was cross-checked against PortaPack Mayhem's
[`flex_tx` encoder](https://github.com/portapack-mayhem/mayhem-firmware/blob/next/firmware/application/external/flex_tx/ui_flex_tx.cpp).
That reference covers Sync 1/2, FIW and BIW fields, short addresses,
alphanumeric vectors and checksums, BCH(31,21), block interleaving, ERS and the
retrieval-flag change between original and repeated pages. The radio deviation
and bitstream polarity use the same Mayhem
[`proc_fsk` implementation](https://github.com/portapack-mayhem/mayhem-firmware/blob/next/firmware/baseband/proc_fsk.cpp)
already referenced by the POCSAG transmitter. No four-level FLEX transmit mode
is claimed by this implementation.

## References for the POCSAG transmitter

The encoder was independently implemented for this app from
[ITU-R M.584-2](https://www.itu.int/rec/R-REC-M.584-2-199711-I/en), including
the 576-bit alternating preamble, 8-frame/16-codeword batch layout,
BCH(31,21), even parity, address frame placement and LSB-first 7-bit message
packing. Its output structure and padding conventions were cross-checked
against [DAPNET UniPager](https://github.com/DAPNET/UniPager) and
[MMDVMHost](https://github.com/g4klx/MMDVMHost/blob/master/POCSAGControl.cpp),
then checked against the independently written
[faithanalog POCSAG encoder](https://github.com/faithanalog/pocsag-encoder/blob/master/pocsag.c).
The RF polarity, 576-bit preamble and +/-4.5 kHz reference were compared with
[PortaPack Mayhem's POCSAG transmitter](https://github.com/portapack-mayhem/mayhem-firmware/blob/next/firmware/application/external/pocsag_tx/ui_pocsag_tx.cpp),
its [POCSAG encoder](https://github.com/portapack-mayhem/mayhem-firmware/blob/next/firmware/common/pocsag.cpp),
and its [FSK baseband implementation](https://github.com/portapack-mayhem/mayhem-firmware/blob/next/firmware/baseband/proc_fsk.cpp).
No source code was copied. The fixed 38.4 kBaud asynchronous sampler and the
CC1101 deviation step use the programming rules from the
[Texas Instruments CC1101 data sheet](https://www.ti.com/lit/ds/symlink/cc1101.pdf);
the waveform timer independently supplies the selected 512/1200/2400 baud.

All contributions are welcome!
