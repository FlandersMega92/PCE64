#!/bin/sh
D=/home/claude/scratch/n64f; rm -rf $D; mkdir -p $D; cp /home/claude/pce64/n64/main.c /home/claude/pce64/n64/fuzz_jit.c /home/claude/pce64/n64/Makefile $D/
sed -i "s|^CORE = .*|CORE = /home/claude/pce64/core|; s|\$(BUILD_DIR)/ext/%.o: ../%.c|\$(BUILD_DIR)/ext/%.o: /home/claude/pce64/%.c|; s|\$(subst ../,ext/,|\$(subst /home/claude/pce64/,ext/,|" $D/Makefile
export N64_INST=/opt/libdragon PATH=/opt/libdragon/bin:$PATH
(cd $D && make -j2 PCE_LIBDIR=/home/claude/pce64/n64/librel MAIN=fuzz_jit.c "$@" > build.log 2>&1) || { tail -20 $D/build.log; exit 1; }
timeout ${TMO:-900} stdbuf -oL /home/claude/pce64/tools/ares.sh $D/pce64.z64 --frames ${FR:-8000000} --until "FUZZ DONE" > $D/out.txt 2>&1; grep -v "Stalled\|flushing\|Vulkan" $D/out.txt | tail -${TAIL:-6}
