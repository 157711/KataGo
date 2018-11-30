#!/bin/bash -eu

if [[ $# -ne 2 ]]
then
    echo "Usage: $0 BASEDIR NTHREADS"
    echo "BASEDIR containing selfplay data and models and related directories"
    echo "NTHREADS number of parallel threads/processes to use in shuffle"
    exit 0
fi
BASEDIRRAW=$1
shift
NTHREADS=$1
shift

basedir=$(realpath $BASEDIRRAW)

mkdir -p $basedir/scripts
cp ./*.py ./selfplay/*.sh $basedir/scripts

(
    cd $basedir/scripts
    while true
    do
        ./shuffle.sh $basedir $NTHREADS
        sleep 300
    done
) 2>&1 | tee outshuffle.txt &

(
    cd $basedir/scripts
    while true
    do
        ./export_modelv3_for_selfplay.sh $basedir
        sleep 30
    done
) 2>&1 | tee outexport.txt &
