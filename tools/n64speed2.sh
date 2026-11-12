#!/bin/sh
# n64speed2.sh NAME ROM INPUTS NFRAMES [make vars]: like n64speed.sh with extra build flags
N=$1; R=$2; I=$3; F=$4; shift 4
D=/home/claude/scratch/n64_spd_$N
HB=1 SCRIPT=$I N64DIR=$D sh /home/claude/pce64/tools/n64run.sh "$R" $F "$@" > $D.log 2>&1 || true
echo "$N: $(awk -f /home/claude/pce64/tools/busy.awk $D.log)"
