T=$1; SONG=${2:-0}
cat <<X
master 4096
load $SONG
wait 300
page 13
tap SEL
wait 50
turn K2 2
wait 50
expect clockout 2
page 0
sg 0 $T
sg 1 0
wait 200
tap PLAY
wait 8000
midi USB B0 55 01
wait 8000
tap PLAY
wait 500
X
