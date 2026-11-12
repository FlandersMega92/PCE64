#!/bin/sh
# headless Ares (stock settings) on lavapipe software Vulkan
export VK_ICD_FILENAMES=/opt/lavapipe/lvp_icd.json
export LP_NUM_THREADS=2
exec /home/claude/ares-emulator/ares/build/headless/ares-headless "$@"
