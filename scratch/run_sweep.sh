#!/bin/bash
# ============================================================
# run_sweep.sh — Bateria automatizada HaLow (ns-3)
# ============================================================
set -u

# ---------------- CONFIG ----------------
SIM_PROJ="halow"
RAW_PROJ="raw-generate"
DATASET="scratch/halow/dataset-halow.txt"
RAWDIR="scratch/halow"
RESULTS_DIR="results"
MANIFEST="${RESULTS_DIR}/manifest.csv"
COUNTER="${RESULTS_DIR}/.counter"

BEACON="102400"
PAGESLICE_COUNT="0"
PAGESLICE_LEN="1"
NUMSLOT="2"

mkdir -p "$RESULTS_DIR" "$RAWDIR"

NSTA_LIST="1 2 4 8 16 32"
SEED_LIST="1 2 3"
SLOTNUM_LIST="1 2 4"

NGROUP_MAP="1:1 2:1 4:1 8:2 16:2 32:4 64:4"
NUMSLOT_MAP="1:4 2:4 4:1 8:2 16:2 32:2"

CUSTOM_LIST=""

# ---------------- HELPERS ----------------
init() {
    [ -f "$COUNTER" ]  || echo 0 > "$COUNTER"
    [ -f "$MANIFEST" ] || echo "id,tag,nsta,args,arquivo" > "$MANIFEST"
}

next_id() {
    local id=$(cat "$COUNTER")
    id=$((id + 1))
    echo "$id" > "$COUNTER"
    echo "$id"
}

ngroup_for() {
    local n="$1"
    for pair in $NGROUP_MAP; do
        local key="${pair%%:*}"
        local val="${pair##*:}"
        if [ "$key" = "$n" ]; then echo "$val"; return; fi
    done
    echo 1
}

numslot_for() {
    local n="$1"
    for pair in $NUMSLOT_MAP; do
        local key="${pair%%:*}"
        local val="${pair##*:}"
        if [ "$key" = "$n" ]; then echo "$val"; return; fi
    done
    echo "$NUMSLOT"
}

gen_rawconfig() {
    local nsta="$1"
    local slotnum="$2"
    local ngroup="$3"
    local rawpath="${RAWDIR}/RawConfig-N${nsta}.txt"

    rm -f "$rawpath"

    ./waf --run "$RAW_PROJ \
                 --NRawSta=${nsta} \
                 --NGroup=${ngroup} \
                 --NumSlot=${slotnum} \
                 --beaconinterval=${BEACON} \
                 --RAWConfigPath=${rawpath} \
                 --pageSliceCount=${PAGESLICE_COUNT} \
                 --pageSliceLen=${PAGESLICE_LEN}" \
        > "${RESULTS_DIR}/.rawgen-N${nsta}.log" 2>&1

    if [ ! -f "$rawpath" ]; then
        echo "     x raw-generate falhou (ver .rawgen-N${nsta}.log)"
        return 1
    fi
    return 0
}

run_test() {
    local tag="$1"
    local nsta="$2"
    local slotnum="$3"
    local ngroup="$4"
    local extra_args="$5"

    local id=$(next_id)
    local out="${RESULTS_DIR}/dataset.${id}.csv"
    local logf="${RESULTS_DIR}/dataset.${id}.log"

    local sim_args="--Nsta=${nsta}"
    if [ -n "$extra_args" ]; then
        sim_args="${sim_args} ${extra_args}"
    fi

    printf "[%02d] %-30s Nsta=%-3s NGroup=%s NumSlot=%s\n" \
           "$id" "$tag" "$nsta" "$ngroup" "$slotnum"

    if ! gen_rawconfig "$nsta" "$slotnum" "$ngroup"; then
        return
    fi

    rm -f "$DATASET"

    if ./waf --run "$SIM_PROJ $sim_args" > "$logf" 2>&1; then
        if [ -f "$DATASET" ]; then
            cp "$DATASET" "$out"
            echo "${id},\"${tag}\",${nsta},\"${sim_args}\",${out}" >> "$MANIFEST"
            echo "     -> $out"
        else
            echo "     x dataset nao gerado (ver $logf)"
        fi
    else
        echo "     x simulacao falhou (ver $logf)"
    fi
}

# ---------------- SWEEPS ----------------
sweep_n() {
    for N in $NSTA_LIST; do
        local ng=$(ngroup_for "$N")
        run_test "N${N}" "$N" "$NUMSLOT" "$ng" ""
    done
}

sweep_n_opt() {
    for N in $NSTA_LIST; do
        local ng=$(ngroup_for "$N")
        local sn=$(numslot_for "$N")
        run_test "N${N}-opt" "$N" "$sn" "$ng" ""
    done
}

sweep_slotnum() {
    for N in $NSTA_LIST; do
        local ng=$(ngroup_for "$N")
        for SN in $SLOTNUM_LIST; do
            run_test "N${N}-slot${SN}" "$N" "$SN" "$ng" ""
        done
    done
}

sweep_seed() {
    for S in $SEED_LIST; do
        run_test "seed${S}" "4" "$NUMSLOT" "1" "--seed=${S}"
    done
}

# ============================================================
#  LOAD — 3 cenarios de carga × 6 Ns
# ============================================================
sweep_load() {
    local CENARIOS="leve:5:10 medio:100:1 estressante:1000:0.1"

    for cen in $CENARIOS; do
        local tag="${cen%%:*}"
        local rest="${cen#*:}"
        local maxpkt="${rest%%:*}"
        local interval="${rest##*:}"

        echo ""
        echo "======================================================="
        echo "  CENARIO: $tag   (MaxPackets=$maxpkt, Interval=${interval}s)"
        echo "======================================================="

        for N in $NSTA_LIST; do
            local ng=$(ngroup_for "$N")
            run_test "load-${tag}-N${N}" "$N" "$NUMSLOT" "$ng" \
                     "--MaxPackets=$maxpkt --interval=$interval"
        done
    done
}

sweep_custom() {
    echo "$CUSTOM_LIST" | while IFS='|' read -r tag nsta slot ngroup args; do
        [ -z "$tag" ] && continue
        run_test "$tag" "$nsta" "$slot" "$ngroup" "$args"
    done
}

# ---------------- UTILITARIOS ----------------
show_manifest() {
    if [ -f "$MANIFEST" ]; then
        column -s, -t < "$MANIFEST"
    else
        echo "(manifest ainda vazio)"
    fi
}

reset_all() {
    echo 0 > "$COUNTER"
    echo "id,tag,nsta,args,arquivo" > "$MANIFEST"
    rm -f "${RESULTS_DIR}"/dataset.*.csv "${RESULTS_DIR}"/dataset.*.log
    rm -f "${RESULTS_DIR}"/.rawgen-N*.log
    echo "Contador, manifest e datasets resetados."
}

# ---------------- DISPATCHER ----------------
init

case "${1:-help}" in
    n)      sweep_n ;;
    nopt)   sweep_n_opt ;;
    load)   sweep_load ;;
    slot)   sweep_slotnum ;;
    seed)   sweep_seed ;;
    custom) sweep_custom ;;
    show)   show_manifest ;;
    reset)  reset_all ;;
    *)
        echo "Uso: $0 {n|nopt|load|slot|seed|custom|show|reset}"
        ;;
esac