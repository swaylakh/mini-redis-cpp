#!/bin/bash
# Run every test script and summarise. Exits non-zero if any suite failed.
cd "$(dirname "$0")"
overall=0
for t in commands.sh info.sh protocol.sh psync.sh propagation.sh replication.sh rdb_roundtrip.sh; do
	echo "###############################"
	echo "# $t"
	echo "###############################"
	bash "$t" || overall=1
	echo ""
done
if [ $overall -eq 0 ]; then echo "ALL SUITES PASSED"; else echo "SOME SUITES FAILED"; fi
exit $overall
