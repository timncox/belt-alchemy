# TETHER end to end

PROTECTOR (CLOCK OUT = PARTS) in each keyboard's simulator, its USB MIDI fed
into the Lab emulator running TETHER, and every jack checked against what the
MIDI says it should do.

1. FM-1: `sh fm1gen.sh 120 > f.txt; FM1B_LOG_CLOCKS=1 protector_host f.txt out;`
   `python3 tolab.py out/midi_out.txt fm1_120.mid` (the FM-1 host logs F8 only
   with that variable, a local patch to host/plat_host.c).
2. SMK-37: `sh mk.sh 120 0 > s.txt; smkhost s.txt out; python3 smk2lab.py out.log.txt smk_120.mid`
3. `./runlab.sh fm1_120 120` (EMU= the emulator's path), or with jobs and a
   prescript: `./runlab.sh x 120 gateA,gateB,clk16,section,trig1,run pre_section.emu`.
   Results go to NAME.res; "FAILS 0" is a pass.

Needs the emulator's `midifile` and `cvlog` commands (alchemy-lab, branch
claude/emu-midi-in).
