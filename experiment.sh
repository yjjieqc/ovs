#!/bin/sh

for i in 1 2 3 4 5
do 
./sp1.sh $1-"$i"
sleep 3s
echo finish $i

done
