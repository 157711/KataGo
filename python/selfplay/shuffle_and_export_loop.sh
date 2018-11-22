#!/bin/bash -eu

basedir=$(realpath $1)

mkdir -p $basedir/scripts
cp ./*.py ./selfplay/*.sh $basedir/scripts

(
    cd $basedir/scripts
    while true
    do
        ./shuffle.sh $basedir
        sleep 400
    done
) 2>&1 | tee outshuffle.txt &

(
    cd $basedir/scripts
    while true
    do
        ./export_modelv3_for_selfplay.sh $basedir
        sleep 120
    done
) 2>&1 | tee outexport.txt &
