#!/bin/sh
# n64speed.sh NAME ROM INPUTS NFRAMES : busy time per frame (60-frame windows)
D=/home/claude/scratch/n64_spd_$1
HB=1 SCRIPT=$3 N64DIR=$D sh /home/claude/pce64/tools/n64run.sh "$2" $4 > $D.log 2>&1 || true
awk -f /home/claude/pce64/tools/busy.awk $D.log
