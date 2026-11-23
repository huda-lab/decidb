#!/bin/bash
# usage: run_probes.sh <tag>
S=/private/tmp/claude-501/-Users-hatim-Code-decidb/cc1ee130-38ff-4d84-8eae-28eb6a7ea8cb/scratchpad
cd /Users/hatim/Code/decidb
tag=$1
i=0
for pair in "probe.sql probe_new" "probe2.sql probe2_before" "probe3.sql probe3_c3f"; do
  set -- $pair
  d=$S/${2}_$tag.dump; o=$S/${2}_$tag.out
  rm -f "$d"
  DECIDB_DUMP_MODEL=$d DECIDB_NATIVE_CONSTRUCTS=on build/release/decidb < $S/$1 > $o 2>&1
  cmp "$d" $S/$2.dump && cmp "$o" $S/$2.out && echo "$1: identical (dump+out)"
done
