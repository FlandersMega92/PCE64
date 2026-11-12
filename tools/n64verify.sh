#!/bin/sh
# n64verify.sh GAME.pce INPUTS.txt
D=/home/claude/scratch/n64v2; rm -rf $D; mkdir -p $D; cp /home/claude/pce64/n64/main.c /home/claude/pce64/n64/verify.c /home/claude/pce64/n64/Makefile $D/
sed -i "s|^CORE = .*|CORE = /home/claude/pce64/core|; s|\$(BUILD_DIR)/ext/%.o: ../%.c|\$(BUILD_DIR)/ext/%.o: /home/claude/pce64/%.c|; s|\$(subst ../,ext/,|\$(subst /home/claude/pce64/,ext/,|" $D/Makefile
python3 /home/claude/pce64/tools/script_to_header.py "$2" $D/script_input.h
export N64_INST=/opt/libdragon PATH=/opt/libdragon/bin:$PATH
(cd $D && make -j2 PCE_LIBDIR=/home/claude/pce64/n64/librel MAIN=verify.c "PCE_EXTRA=-DPCE_VERIFY -I. $VEXTRA" > build.log 2>&1) || { grep -E "error" $D/build.log | head; exit 1; }
python3 /home/claude/pce64/tools/pce64_convert.py $D/pce64.z64 "$1" $D/game.z64 > /dev/null
timeout ${TMO:-1500} /home/claude/pce64/tools/ares.sh $D/game.z64 --frames ${FR:-20000} --until "VERIFY DONE" 2>&1 | grep -v -E "Stalled|flushing"
