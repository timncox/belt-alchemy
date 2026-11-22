# runlab.sh NAME BPM [jobs] [prescript]  -> NAME.cv, NAME.res ; NAME.mid must exist
N=$1; BPM=$2; JOBS=${3:-gateA,gateB,clk16,reset,trig1,trig2}; PRE=${4:-}
D=$(cd $(dirname $0) && pwd)
EMU=${EMU:-/home/claude/alchemy-lab/emu/build/custom/tether_alchemy-400458/emu}
DUR=$(awk 'END{print int($1)+1500}' $D/$N.mid)
{ echo "midi plug"; echo "wait 3000"; echo "expect booted"; [ -n "$PRE" ] && cat $PRE; echo "cvlog $D/$N.cv"; echo "midifile $D/$N.mid"; echo "wait $DUR"; echo "cvlog off"; echo "expect finite"; } > $D/$N.emu
BASE=$($EMU --headless --script $D/$N.emu --flash none --nomic 2>&1 | tee $D/$N.emulog | sed -n 's/midifile from frame \([0-9]*\).*/\1/p')
python3 $D/analyze.py $D/$N.mid $D/$N.cv $BASE $BPM $JOBS > $D/$N.res
