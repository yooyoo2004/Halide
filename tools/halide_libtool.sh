#!/bin/bash

# This is a script to allow combining a list of .a or .o
# files into a single .a file; input .a are decomposed
# into their component .o files. Ordering is maintained.
# It is assumed there are no duplicates. (It's basically the subset
# of libtool that we'd use, if libtool was reliably available everwhere.)
#
# $1 = Output .a file
# $2...$N = Input .a or .o files
#
# It is OK to have the same file as an input and output
# (it will of course be overwritten).

set -eux

# Output to temp file in case it's also in inputs
OUTPUT=`mktemp`

for INPUT in ${@:2}; do
    EXT="${INPUT##*.}"
    if [[ ${EXT} == "o" ]]; then
        Adding Input ${INPUT}
        ar q ${OUTPUT} ${INPUT}
    elif [[ ${EXT} == "a" ]]; then
        AR_TEMP=`mktemp -d`
        cd AR_TEMP
        ar x ${INPUT}
        # Insert in the same order, don't rely on glob
        for OBJ in `ar p ${INPUT}`; do
            echo Adding Input ${INPUT} -> ${OBJ}
            ar q ${OUTPUT} ${OBJ}
        done
        rm -rf ${AR_TEMP}
    else
        echo File ${INPUT} is neither .o nor .a
        exit 1
    fi
done

ranlib ${OUTPUT}
mv -f ${OUTPUT} $1

