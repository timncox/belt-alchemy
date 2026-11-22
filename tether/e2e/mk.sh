T=$1; SONG=${2:-0}
cat <<X
end 17500
100 cmd load $SONG
150 cmd clockout 2
160 cmd sg 0 $T
170 cmd sg 1 0
500 btn 16
520 btnup 16
8500 pad 9 100
8600 pad 9 0
16500 btn 16
16520 btnup 16
17000 cmd expect playing 0
X
