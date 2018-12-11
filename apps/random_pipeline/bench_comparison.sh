#!/bin/bash

# Start a watchdog to kill any compilations that take too long
bash ./watchdog.sh &
WATCHDOG_PID=$!

function finish {
    kill $WATCHDOG_PID
}
trap finish EXIT

mkdir -p results

PIPELINES=100
SCHEDULES=1

RANDOM_DROPOUT=100
BEAM_SIZE=50

# Build the shared things by building one pipeline
HL_TARGET=host HL_SEED=root PIPELINE_SEED=0 HL_RANDOM_DROPOUT=${RANDOM_DROPOUT} HL_BEAM_SIZE=${BEAM_SIZE} make build

for ((b=1;b<2;b++)); do
    echo Batch $b
    rm -f results/files_*.txt
    
    # Build lots of pipelines
    for ((p=0;p<$PIPELINES;p++)); do
        P=$((b * $PIPELINES + p))
        STAGES=$(((P % 30) + 10))
        echo echo Building pipeline $P
        #echo "HL_TARGET=host HL_SEED=root PIPELINE_SEED=$P PIPELINE_STAGES=$STAGES HL_RANDOM_DROPOUT=${RANDOM_DROPOUT} HL_BEAM_SIZE=${BEAM_SIZE} make build 2>&1 | grep -v Nothing.to.be.done"
        echo "HL_TARGET=host HL_SEED=0 PIPELINE_SEED=$P PIPELINE_STAGES=$STAGES HL_RANDOM_DROPOUT=${RANDOM_DROPOUT} HL_BEAM_SIZE=${BEAM_SIZE} make build 2>&1 | grep -v Nothing.to.be.done"        
        for prof in ""; do
            for ((m=0;m<2;m++)); do 
                for ((s=0;s<$SCHEDULES;s++)); do
                    echo "NEW_AUTOSCHEDULER=1 HL_TARGET=host${prof} HL_SEED=$s PIPELINE_SEED=$P PIPELINE_STAGES=$STAGES HL_RANDOM_DROPOUT=${RANDOM_DROPOUT} HL_BEAM_SIZE=${BEAM_SIZE} HL_USE_MANUAL_COST_MODEL=${m} make build 2>&1 | grep -v Nothing.to.be.done"
                done
            done
        done
    done | xargs -P48 -I{} bash -c "{}"

    # Benchmark them
    for ((p=0;p<$PIPELINES;p++)); do
        P=$((b * $PIPELINES + p))
        STAGES=$(((P % 30) + 10))
        echo Benchmarking pipeline $P
        #F=bin/host/pipeline_${P}_${STAGES}/schedule_root_${RANDOM_DROPOUT}_${BEAM_SIZE}_0/times.txt
        #if [ ! -f $F ]; then HL_TARGET=host HL_SEED=root PIPELINE_SEED=$P PIPELINE_STAGES=$STAGES HL_RANDOM_DROPOUT=${RANDOM_DROPOUT} HL_BEAM_SIZE=${BEAM_SIZE} HL_NUM_THREADS=32 make bench 2>&1 | grep -v "Nothing to be done"; fi
        #grep '^Time' $F > /dev/null && echo $F >> results/files_root_${b}.txt

        F=bin/host/pipeline_${P}_${STAGES}/schedule_0_${RANDOM_DROPOUT}_${BEAM_SIZE}_0/times.txt
        if [ ! -f $F ]; then HL_TARGET=host HL_SEED=0 PIPELINE_SEED=$P PIPELINE_STAGES=$STAGES HL_RANDOM_DROPOUT=${RANDOM_DROPOUT} HL_BEAM_SIZE=${BEAM_SIZE} HL_USE_MANUAL_COST_MODEL=0 HL_NUM_THREADS=32 make bench 2>&1 | grep -v "Nothing to be done"; fi
        
        grep '^Time' $F > /dev/null && echo $F >> results/files_master_${b}.txt
        for prof in ""; do
            for ((m=0;m<2;m++)); do
                for ((s=0;s<$SCHEDULES;s++)); do
                    F=bin/host${prof}-new_autoscheduler/pipeline_${P}_${STAGES}/schedule_${s}_${RANDOM_DROPOUT}_${BEAM_SIZE}_${m}/times.txt
                    if [ ! -f $F ]; then PLUGIN="-p bin/auto_schedule.so" HL_TARGET=host${prof} HL_SEED=$s PIPELINE_SEED=$P PIPELINE_STAGES=$STAGES HL_RANDOM_DROPOUT=${RANDOM_DROPOUT} HL_BEAM_SIZE=${BEAM_SIZE} HL_USE_MANUAL_COST_MODEL=${m} HL_NUM_THREADS=32 make bench 2>&1 | grep -v "Nothing to be done"; fi

                    grep '^Time' $F > /dev/null && echo $F >> results/files_${b}_${m}${prof}.txt
                done
            done
        done
    done

    # Generate the success cases by taking the intersection of the results from the learned model and the manual model
    cat results/files_${b}_0.txt results/files_${b}_1.txt | sed "s/_..times.txt/_X\/times.txt/" | sort | uniq -d > results/files_common.txt
    
    # Extract the runtimes
    echo "Extracting runtimes..."
    cat results/files_common.txt | sed "s/_X/_0/" | while read F; do grep '^Time' $F | cut -d: -f2 | cut -b2-; done > results/learned_runtimes_${b}.txt
    cat results/files_common.txt | sed "s/_X/_1/" | while read F; do grep '^Time' $F | cut -d: -f2 | cut -b2-; done > results/manual_runtimes_${b}.txt    

    # Extract the compute_root runtimes
    echo "Extracting compute_root runtimes..."
    cat results/files_common.txt | sed "s/_X/_0/" | sed "s/schedule_[0-9]*/schedule_root/" | while read F; do grep '^Time' $F | cut -d: -f2 | cut -b2-; done > results/root_runtimes_${b}.txt

    echo "Extracting master runtimes..."
    cat results/files_common.txt | sed "s/_X/_0/" | sed "s///" | while read F; do grep '^Time' $F | cut -d: -f2 | cut -b2-; done > results/master_runtimes_${b}.txt
    
    # Extract the features
    echo "Extracting features..."
    cat results/files_common.txt | sed "s/_X/_0/" | while read F; do echo $(grep '^YYY' ${F/times/stderr} | cut -d' ' -f3-); done > results/features_${b}.txt

    # Extract failed proofs
    echo "Extracting any failed proofs..."
    cat results/files_${b}_*.txt | while read F; do grep -A1 'Failed to prove' ${F/times/stderr}; done > results/failed_proofs_${b}.txt    

    # Extract the cost according to the hand-designed model (just the sum of a few of the features)
    echo "Extracting costs..."
    cat results/files_common.txt | sed "s/_X/_0/" | while read F; do echo $(grep '^State with cost' ${F/times/stderr} | cut -d' ' -f4 | cut -d: -f1); done  > results/learned_costs_${b}.txt
    cat results/files_common.txt | sed "s/_X/_1/" | while read F; do echo $(grep '^State with cost' ${F/times/stderr} | cut -d' ' -f4 | cut -d: -f1); done  > results/manual_costs_${b}.txt    
done
