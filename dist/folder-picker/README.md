# Firmware picker with folders (Mutable in one folder)

Built Nov 22 on belt-alchemy `claude/iphone-screen` @ e9fd86f plus `belt-alchemy-folder-picker.patch`. Not pushed anywhere.

## What changes on the module
- The card's `/alchemy` folder can hold folders one level deep, e.g. `/alchemy/mutable/clouds_alchemy.bin`.
- **Knob (Firmware page, top-left pot):** a folder is one **purple** dot. Pick it and do the Flash gesture (top-right pot down, then all the way up): it opens the folder, nothing is written. Inside, the first dot is **white** = back up. Files stay green. The page always opens at the top.
- **App (Firmware tab):** lists every file on both levels by path, e.g. `mutable/plaits_alchemy.bin`. Tap one to switch, same as before.
- Limits: 40 entries in all, 6 folders, names up to 47 characters including the folder.

## Which firmwares have it
Only firmwares built with this picker: **Kerf 0.2.1** (`comb-filter/build/kerf_alchemy-phone-0.2.1.bin`) and **Pulse 0.3.1** (`pulse-clock/phone/pulse_alchemy-phone-0.3.1.bin`). Belt also builds with it here, but no Belt file is shipped.
Every other firmware (Belt as installed, TETHER, Smack, the six Mutable ports from mi-alchemy, …) keeps the old picker: it only sees the top level of `/alchemy`, at most 16 files. So from those, hop to Kerf or Pulse first to reach the folder. Coming back out is fine: the Mutable ports still see every top-level file.

## Card layout after the move
Top level: 10 + kerf + pulse = 12 files (under the old 16 limit), plus `mutable/` with clouds, elements, marbles, meld, plaits, warps.

## Mac steps (zsh), Lab in Mac mode (hold B1 at power-up, B3 blue), plugged into the Mac
`hostlink-fs.mjs` here adds `mv` (uses the firmware's rename; nothing is deleted). Copy it to `~/Downloads/hostlink-fs.mjs` first; the checked-in tool in ~/tim-os/belt-alchemy is left alone.

```
cd ~/tim-os/belt-alchemy
node ~/Downloads/hostlink-fs.mjs ls /alchemy
node ~/Downloads/hostlink-fs.mjs mkdir /alchemy/mutable
for f in clouds elements marbles meld plaits warps; do node ~/Downloads/hostlink-fs.mjs mv /alchemy/${f}_alchemy.bin /alchemy/mutable/${f}_alchemy.bin; done
node ~/Downloads/hostlink-fs.mjs put ~/Downloads/kerf_alchemy-phone-0.2.1.bin /alchemy/kerf_alchemy.bin
node ~/Downloads/hostlink-fs.mjs put ~/Downloads/pulse_alchemy-phone-0.3.1.bin /alchemy/pulse_alchemy.bin
node ~/Downloads/hostlink-fs.mjs ls /alchemy
node ~/Downloads/hostlink-fs.mjs ls /alchemy/mutable
```
Check the real Mutable file names in the first `ls` and adjust the loop if they differ. To undo: `mv` each back to `/alchemy/`.

## Tests
- `c++ -std=c++17 -I src tests/picker_list_test.cpp && ./a.out` → 31/31 (folder dot, open, back, app paths, sorting, caps, AppleDouble/hidden skipped, folder removed).
- Belt, Kerf 0.2.1, Pulse 0.3.1 build clean with arm-none-eabi-gcc 13.2.
- Not run on a module. The emulator stubs the picker, so the knob gesture on a folder is untested until it's on the Lab.

## SHA-256
- kerf_alchemy-phone-0.2.1.bin  e26598c2e97126fdfbbf768a3fd6b8d053e0e0f5f8bda38d2a0250a0723ff0b1
- pulse_alchemy-phone-0.3.1.bin 1f1ef0f1a4622cdbb1e26340cad7ee95bd9e206fad8c9481bfc5f41ea194a769
