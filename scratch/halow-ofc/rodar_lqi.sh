#!/bin/bash
# Roda uma simulação halow-ofc isolada em results/lqi/<nome>/ (permite rodadas em paralelo).
# Uso: scratch/halow-ofc/rodar_lqi.sh <nome> <Nsta> <Spacing> <NGroup> <NumSlot> <simTime> <seed> [args extras]
# Rodar da raiz do projeto (onde fica o ./waf), depois de compilar.

NOME="$1"; NSTA="$2"; SPACING="$3"; NGROUP="$4"; NSLOT="$5"; SIMTIME="$6"; SEED="$7"; shift 7
ROOT="$(pwd)"
DIR="${ROOT}/results/lqi/${NOME}"
mkdir -p "${DIR}/results" "${DIR}/OptimalRawGroup"
RAW="${DIR}/RawConfig.txt"
LD_LIBRARY_PATH="${ROOT}/build" ./build/scratch/RAW-generate --NRawSta="${NSTA}" --NGroup="${NGROUP}" --NumSlot="${NSLOT}" \
    --beaconinterval=102400 --RAWConfigPath="${RAW}" --pageSliceCount=0 --pageSliceLen=1 > "${DIR}/rawgen.log" 2>&1
START=$(date +%s)
( cd "${DIR}" && LD_LIBRARY_PATH="${ROOT}/build" "${ROOT}/build/scratch/halow-ofc/halow-ofc" \
    --Nsta="${NSTA}" --NRawSta="${NSTA}" --Spacing="${SPACING}" --NGroup="${NGROUP}" --NRawSlotNum="${NSLOT}" \
    --RAWConfigFile="${RAW}" --BeaconInterval=102400 --pageSliceCount=0 --pageSliceLength=31 \
    --TrafficType=udpecho --simulationTime="${SIMTIME}" --seed="${SEED}" "$@" > "${DIR}/sim.log" 2>&1 )
STATUS=$?
END=$(date +%s)
echo "nome=${NOME} nsta=${NSTA} spacing=${SPACING} ngroup=${NGROUP} nslot=${NSLOT} simtime=${SIMTIME} seed=${SEED} extras=$* status=${STATUS} wall_s=$((END-START)) associadas=$(grep -a -c 'is associated and has aid' "${DIR}/sim.log")" | tee "${DIR}/info.txt"
