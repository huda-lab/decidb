#!/bin/bash
# usage: tree7_probes.sh <tag>   (tag "before" saves the references for probes 4 and 5)
S=/private/tmp/claude-501/-Users-hatim-Code-decidb/cc1ee130-38ff-4d84-8eae-28eb6a7ea8cb/scratchpad
cd /Users/hatim/Code/decidb
tag=$1
for pair in "probe.sql probe_new" "probe2.sql probe2_before" "probe3.sql probe3_c3f" "probe4.sql probe4_before" "probe5.sql probe5_before"; do
  set -- $pair
  d=$S/t7_${2}_$tag.dump; o=$S/t7_${2}_$tag.out
  rm -f "$d"
  DECIDB_DUMP_MODEL=$d DECIDB_NATIVE_CONSTRUCTS=on build/release/decidb < $S/$1 > $o 2>&1
  if [ "$tag" = before ] && [ ! -f $S/$2.dump ]; then cp "$d" $S/$2.dump; cp "$o" $S/$2.out; echo "$1: saved"; continue; fi
  cmp "$d" $S/$2.dump && cmp "$o" $S/$2.out && echo "$1: identical (dump+out)"
done
