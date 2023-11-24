#!/bin/bash

SCRIPTDIR=$(dirname $(realpath $0))

function dump_instruction_counts() {
    objdump --disassemble --no-show-raw-insn "$1" |
    grep '^ '                                     |
    cut --fields=2                                |
    cut --delimiter=' ' --fields=1                |
    sort                                          |
    uniq --count
}

awk --assign filename1="$(basename $1)"   \
    --assign filename2="$(basename $2)"   \
    --file "$SCRIPTDIR"/dump-instdist.awk \
    <(dump_instruction_counts "$1")       \
    <(dump_instruction_counts "$2")       |
gnuplot -p -e "
set key autotitle columnheader noenhanced;
set style data histogram;
set style data linespoints;
set pointsize 2;
set ylabel 'percentage';
set xtics rotate by 292;
plot '-' using 1:xticlabels(2), '' using 1:xticlabels(2)
"
