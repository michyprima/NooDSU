# NooDSU
A DS and GBA emulator for the Wii U.

### Overview
NooDSU is a Wii U-only fork of [NooDS](https://github.com/Hydr8gon/NooDS) by Hydr8gon, focused on running well on real
hardware. It adds a JIT for the Wii U's CPU, a 3D renderer that uses the Wii U's GPU, and faster threading and ROM loading.
It's licensed under the GPL version 3, like NooDS.

### Usage
Put `noodsu.wuhb` in `sd:/wiiu/apps/` on your SD card. Settings, states and cheats are kept in `sd:/wiiu/apps/noodsu/`, and saves next to the ROM.

NooDSU should be able to run most things without any setup. DS BIOS and firmware files must be provided to boot the
system menu, and can be dumped from a DS with [DSBF Dumper](https://archive.org/details/dsbf-dumper). The firmware must
come from an original DS; DSi and 3DS dumps don't contain any boot code. A GBA BIOS file can be optionally provided, and
can be dumped from many systems with [this dumper](https://github.com/mgba-emu/bios-dump). File paths can be configured
in the `noodsu.ini` file. Save types are automatically detected, but this may not always be accurate. If you run something
and it has issues with saving, the save type can be changed in the pause menu.

### Building
Install [devkitPro](https://devkitpro.org/wiki/Getting_Started) and its `wiiu-dev` package. Run `make -j$(nproc)` in the
project root directory to start building. `./build-wiiu-debug.sh` builds variants for debugging and measuring.

### References
* [GBATEK](https://problemkaputt.de/gbatek.htm) - The main information source for all things DS and GBA
* [GBATEK Addendum](https://melonds.kuribo64.net/board/thread.php?id=13) - A thread that aims to fill the gaps in GBATEK
* [melonDS Blog](https://melonds.kuribo64.net) - Contains posts that detail various 3D quirks
* [DraStic BIOS](https://drive.google.com/file/d/1dl6xgOXc892r43RzkIJKI6nikYIipzoN/view) - Reference for the DS HLE BIOS
functions
* [Cult of GBA BIOS](https://github.com/Cult-of-GBA/BIOS) - Reference for the GBA HLE BIOS functions
