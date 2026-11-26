# Credits

BA-55 by swtdr34mz. All of its own code (the voice, the sequencer, the screens) is written for this project; the sound
core grew out of swtdr34mz's MONSUN. All sounds are synthesized in the firmware: no samples. The concept and the look
follow the Erica Synths Bassline DB-01; no code, sound or artwork of theirs is used. The filter is a state-variable
filter after Vadim Zavalishin's published topology-preserving form, with saturating integrators.

## Built on

| Project | Author | Licence | Used for |
| --- | --- | --- | --- |
| [fm1-base](https://github.com/charlesvestal/fm1-base) | Charles Vestal | MPL-2.0 | the platform: screen, panel, audio, MIDI, storage, simulator, build tools, the Terminus pixel fonts it carries (SIL OFL 1.1) |
| [felucca-bsp](https://github.com/hugelton/felucca-bsp) | Leo Kuroshita (Hügelton Instruments) and contributors | MPL-2.0 | the hardware layer, update loader, packaging |
| JieLi's AC79 SDK | JieLi Technology | Apache-2.0 | three files in every `.fwsc` package (fetched at build time, never copied here) |
