# Flipper POCSAG/FLEX Receiver plugin

## Protocol implementation made by @Shmuma + 512, 2400 speeds support by @htotoo
Plugin based on [Weather Station](https://github.com/flipperdevices/flipperzero-firmware/tree/dev/applications/plugins/weather_station) from OFW

Icons by @Svaarich

Includes two FM presets built into the app:

- `FM95` for POCSAG reception
- `FLEX` for asynchronous two-level FLEX reception with approximately +/-4.76 kHz deviation

The app supports POCSAG 512, POCSAG 1200, POCSAG 2400, FLEX 1600/2,
and FLEX 3200/2 decoding on CC1101-supported frequencies.

FLEX 3200/4 and FLEX 6400/4 are not supported because the current CC1101
asynchronous receive path exposes a two-level digital stream rather than all
four discriminator levels.

The FLEX framing, BCH correction, deinterleaving, and message parsing are based
on the GPL-licensed [multimon-ng FLEX decoder](https://github.com/EliasOenal/multimon-ng/blob/master/demod_flex.c).

Check datasheet and add required frequency in config file (see details below)

Default frequency is set to DAPNET - "439987500"

To add new presets and frequencies create file "yourMicroSD/pocsag/settings.txt"

And put [THIS](https://github.com/flipperdevices/flipperzero-firmware/blob/dev/applications/main/subghz/resources/subghz/assets/setting_user.example) file contents into it, and edit this example for yourself, add needed frequencies

Warning!!! This file is EXAMPLE! It contains frequencies that are commented with "#"

"#" Is a comment symbol, if that symbol is present at the beggining of the line, that means this line will be ignored! 

To add custom frequency you need to uncomment the line and edit numbers keeping its size (zeros at the end) "Frequency: 433000000" 

To edit default freqency follow same procedure 

All custom frequencies added from this file will be at the end of the list in the frequency selector in the app! Scroll frequencies to the end to find your custom freq. 

Default list > Custom list



All contributions are welcome!
