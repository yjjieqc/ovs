#!/bin/sh

for i in $(seq 10000 10000 90000)
do
j=$((j=$i+10000))
echo $i $j
done
