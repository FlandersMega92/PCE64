#!/bin/sh
# n64run.sh GAME.pce FRAMES [make vars...] : build stats ROM, convert, run in ares, print debug lines
set -e
G="$1"; F="$2"; shift 2
export N64_INST=/opt/libdragon PATH=/opt/libdragon/bin:$PATH
D=${N64DIR:-/home/claude/scratch/n64b}
rm -rf $D; mkdir -p $D; cp /home/claude/pce64/n64/main.c /home/claude/pce64/n64/Makefile $D/; [ -f /home/claude/pce64/n64/script_input.h ] && cp /home/claude/pce64/n64/script_input.h $D/ || true; 
sed -i "s|^CORE = .*|CORE = /home/claude/pce64/core|; s|\$(BUILD_DIR)/ext/%.o: ../%.c|\$(BUILD_DIR)/ext/%.o: /home/claude/pce64/%.c|; s|\$(subst ../,ext/,|\$(subst /home/claude/pce64/,ext/,|" $D/Makefile
if [ -n "$SCRIPT" ]; then python3 /home/claude/pce64/tools/script_to_header.py "$SCRIPT" $D/script_input.h; set -- "$@" PCE_SCRIPT=1; sed -i "s|-I\$(BUILD_DIR)|-I.|" $D/Makefile; fi
(cd $D && make -j2 PCE_LIBDIR=/home/claude/pce64/n64/librel PCE_STATS=${STATS:-1} "$@" >build.log 2>&1) || { tail -20 $D/build.log; exit 1; }
python3 /home/claude/pce64/tools/pce64_convert.py $D/pce64.z64 "$G" $D/game.z64 >/dev/null
mkdir -p $D/shots
timeout ${TMO:-1500} /home/claude/pce64/tools/ares.sh $D/game.z64 --frames $F --quiet ${HB:+--homebrew} --out $D/shots/ ${SHOTS:+--shots $SHOTS} 2>&1 | grep -v Stalled
