#!/bin/sh
#
# This script glues symbol parts together and turns the sym_S_N
# into a '#define S value' construct.
#

NM=nm
AWK=awk

if [ "$1x" == "x" ]; then
	echo "usage: $0 filename"
	exit
fi

${NM} $1 | ${AWK} '
	BEGIN {
		print "/* This file is automatically generated - do not edit */\n"
	}
	/ C sym_.*_0$/ {
		part1 = substr($1, length($1) - 3);
	}
	/ C sym_.*_1$/ {
		part2 = substr($1, length($1) - 3);
	}
	/ C sym_.*_2$/ {
		part3 = substr($1, length($1) - 3);
	}
	/ C sym_.*_3$/ {
		symname = substr($0, index($0, "sym_") + 4)
		sub(/_[0-9]+$/, "", symname)
		part4 = substr($1, length($1) - 3);
		total = part4 part3 part2 part1
		sub("^0*", "", total)
		if (total == "")
			total = "0"
		printf("#define %s 0x%s\n", symname, total)
	}
'