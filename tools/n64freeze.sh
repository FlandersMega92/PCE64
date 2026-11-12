#!/bin/sh
# n64freeze.sh GAME.pce INPUTS FRAME OUT.png : N64 screenshot of PCE frame FRAME
D=/home/claude/scratch/n64_frz
export SCRIPT=$2 N64DIR=$D SHOTS=$(( $3 * 14 / 10 + 200 ))
sh /home/claude/pce64/tools/n64run.sh "$1" $(( $3 * 14 / 10 + 210 )) "PCE_EXTRA=-DFREEZE_AT=$3" > /dev/null 2>&1
cp $D/shots/frame_*.png "$4"
