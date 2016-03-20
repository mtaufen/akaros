#!/bin/ash
set -v

max_vcores
MAXVC=$?

# pthread_test exists to hog the machine
# pthread_test num_threads num_loops [num_vcores]
pthread_test 100 999999999 $MAXVC >> tmpfile 2>&1 &
PID_PTH=$!

prov -tc -p$PID_PTH -m
prov -s

eslt_a > eslt_a.out &
PID_A=$!
eslt_b > eslt_b.out &
PID_B=$!

prov -tc -p$PID_A -v1
prov -tc -p$PID_B -v2
prov -s

usleep 1000000
prov -s

cat eslt_a.out
cat eslt_b.out

prov -tc -p$PID_PTH -m
prov -s
prov -tc -p $PID_A -v2
prov -tc -p $PID_B -v1
prov -s

usleep 1000000
prov -s

cat eslt_a.out
cat eslt_b.out

prov -tc -p$PID_PTH -m
prov -s
prov -tc -p$PID_A -v1
prov -tc -p$PID_B -v1
prov -s

usleep 1000000
prov -s

cat eslt_a.out
cat eslt_b.out

kill $PID_PTH
kill $PID_A
kill $PID_B