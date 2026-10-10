#!/bin/bash
# ============================================================
# run_halow.sh — Bateria automatizada HaLow (ns-3)
#
# Uso:  ./scratch/run_halow.sh {groups|n|slot|seed|custom|show|reset}
#       (rode da raiz do projeto, onde fica o ./waf)
#
# Cada simulacao gera uma pasta propria em results/:
#     results/01_N625-G1/
#         resultado_*.csv   (rssi, rssi_resumo, pacotes, fluxos, nos)
#         *.nss             (se o simulador gerar)
#         RawConfig.txt     (RAW usada nessa simulacao)
#         sim.log           (saida da simulacao)
#         params.txt        (parametros e avisos)
#
# O contador de ids e zerado no inicio de cada bateria.
# ============================================================
set -u

# ---------------- CONFIG ----------------
SIM_PROJ="${SIM_PROJ:-halow-ofc}"   # programa do simulador (nome da pasta em scratch/)
RAW_PROJ="${RAW_PROJ:-RAW-generate}"          # gerador de RawConfig (nome e case-sensitive)

RESULTS_DIR="${RESULTS_DIR:-results}"
RAWDIR="${RESULTS_DIR}/rawconfigs"
MANIFEST="${RESULTS_DIR}/manifest.csv"
COUNTER="${RESULTS_DIR}/.counter"

CSV_PREFIX="resultado"          # prefixo usado em ExportarTudo("resultado", ...)
CSV_DIRS=". scratch"            # onde o simulador pode deixar os CSVs (cwd do waf)

BEACON="102400"
PAGESLICE_COUNT="0"
PAGESLICE_LEN="1"
NUMSLOT="2"
SIM_TIME="${SIM_TIME:-}"      # vazio = usa o default do simulador (ex.: SIM_TIME=30 ./run_halow.sh groups)

# --- Bateria principal: Nsta x numero de grupos RAW ---
NSTA_LIST="625 1250 2500"
GROUP_LIST="1 2 3 4"

# --- Outras baterias (mantidas) ---
SEED_LIST="1 2 3"
SLOTNUM_LIST="1 2 3 4"
NGROUP_MAP="1:1 2:1 4:1 8:2 16:2 32:4 64:4"

CUSTOM_LIST=""
# formato: tag|nsta|slots|ngroup|args extras   (uma simulacao por linha)

BATCH_ID="$(date +%Y%m%d-%H%M%S)"

mkdir -p "$RESULTS_DIR" "$RAWDIR"

# ---------------- HELPERS ----------------
init() {
    [ -f "$COUNTER" ]  || echo 0 > "$COUNTER"
    [ -f "$MANIFEST" ] || echo "id,batch,tag,nsta,ngroup,numslot,status,args,pasta" > "$MANIFEST"
}

reset_counter() {
    echo 0 > "$COUNTER"
}

next_id() {
    local id
    id=$(cat "$COUNTER")
    id=$((id + 1))
    echo "$id" > "$COUNTER"
    echo "$id"
}

ngroup_for() {
    local n="$1" pair
    for pair in $NGROUP_MAP; do
        if [ "${pair%%:*}" = "$n" ]; then echo "${pair##*:}"; return; fi
    done
    echo 1
}

# Gera o RawConfig em $RAWDIR e deixa o caminho em RAWPATH
gen_rawconfig() {
    local nsta="$1" slotnum="$2" ngroup="$3" tag="$4"
    RAWPATH="${RAWDIR}/RawConfig-N${nsta}-G${ngroup}-S${slotnum}.txt"
    local genlog="${RAWDIR}/rawgen-N${nsta}-G${ngroup}-S${slotnum}.log"

    rm -f "$RAWPATH"

    ./waf --run "$RAW_PROJ \
                 --NRawSta=${nsta} \
                 --NGroup=${ngroup} \
                 --NumSlot=${slotnum} \
                 --beaconinterval=${BEACON} \
                 --RAWConfigPath=${RAWPATH} \
                 --pageSliceCount=${PAGESLICE_COUNT} \
                 --pageSliceLen=${PAGESLICE_LEN}" \
        > "$genlog" 2>&1

    if [ ! -s "$RAWPATH" ]; then
        echo "     x raw-generate falhou (ver $genlog)"
        return 1
    fi
    return 0
}

# Move os CSVs gerados pelo simulador para a pasta da simulacao; imprime quantos moveu
collect_csvs() {
    local dest="$1" dir f n=0
    for dir in $CSV_DIRS; do
        for f in "$dir"/${CSV_PREFIX}_*.csv; do
            if [ -f "$f" ]; then
                mv "$f" "$dest/"
                n=$((n + 1))
            fi
        done
    done
    echo "$n"
}

clean_stale_csvs() {
    local dir
    for dir in $CSV_DIRS; do
        rm -f "$dir"/${CSV_PREFIX}_*.csv
    done
}

run_test() {
    local tag="$1" nsta="$2" slotnum="$3" ngroup="$4" extra_args="$5"

    local id folder safe_tag status="ok"
    id=$(next_id)
    safe_tag=$(printf '%s' "$tag" | sed 's/[^A-Za-z0-9_.-]/_/g')
    folder="${RESULTS_DIR}/$(printf '%02d' "$id")_${safe_tag}"
    if [ -e "$folder" ]; then
        folder="${folder}_$(date +%H%M%S)"   # nao sobrescreve baterias anteriores
    fi
    mkdir -p "$folder"

    printf "[%02d] %-22s Nsta=%-5s NGroup=%s NumSlot=%s\n" \
           "$id" "$tag" "$nsta" "$ngroup" "$slotnum"

    # ---- avisos de configuracao (nao interrompem) ----
    local warns=""
    if [ $((nsta % ngroup)) -ne 0 ]; then
        warns="${warns}Nsta=${nsta} nao e divisivel por NGroup=${ngroup}: $((nsta % ngroup)) STAs ficam fora dos grupos RAW. "
    fi
    if [ "$nsta" -gt 2047 ]; then
        warns="${warns}Nsta=${nsta} > 2047: AIDs passam do limite da pagina 0 (o RAW-generate tem NS_ASSERT nisso em build debug). "
    fi
    [ -n "$warns" ] && echo "     ! $warns"

    if ! gen_rawconfig "$nsta" "$slotnum" "$ngroup" "$tag"; then
        status="falha_raw"
        echo "${id},${BATCH_ID},\"${tag}\",${nsta},${ngroup},${slotnum},${status},\"\",${folder}" >> "$MANIFEST"
        return
    fi
    cp "$RAWPATH" "${folder}/RawConfig.txt"

    local sim_args="--Nsta=${nsta} --NGroup=${ngroup} --NRawSlotNum=${slotnum}"
    sim_args="${sim_args} --RAWConfigFile=${RAWPATH} --BeaconInterval=${BEACON}"
    sim_args="${sim_args} --pageSliceCount=${PAGESLICE_COUNT} --pageSliceLength=${PAGESLICE_LEN}"
    [ -n "$SIM_TIME" ]    && sim_args="${sim_args} --simulationTime=${SIM_TIME}"
    [ -n "$extra_args" ]  && sim_args="${sim_args} ${extra_args}"

    {
        echo "id=${id}"
        echo "batch=${BATCH_ID}"
        echo "tag=${tag}"
        echo "nsta=${nsta}"
        echo "ngroup=${ngroup}"
        echo "numslot=${slotnum}"
        echo "comando=./waf --run \"${SIM_PROJ} ${sim_args}\""
        echo "avisos=${warns}"
    } > "${folder}/params.txt"

    clean_stale_csvs
    local marker="${folder}/.start"
    touch "$marker"

    if ! ./waf --run "$SIM_PROJ $sim_args" > "${folder}/sim.log" 2>&1; then
        status="falha_sim"
        echo "     x simulacao falhou (ver ${folder}/sim.log)"
    fi

    # CSVs (mesmo em falha parcial, guarda o que existir)
    local ncsv
    ncsv=$(collect_csvs "$folder")

    # arquivos .nss novos (o simulador escreve em results/)
    local f
    while IFS= read -r f; do
        [ -n "$f" ] && mv "$f" "$folder/"
    done < <(find "$RESULTS_DIR" -maxdepth 1 -name '*.nss' -newer "$marker" 2>/dev/null)
    rm -f "$marker"

    if [ "$ncsv" -eq 0 ] && [ "$status" = "ok" ]; then
        status="sem_csv"
        echo "     x nenhum ${CSV_PREFIX}_*.csv encontrado em: ${CSV_DIRS} (ver ${folder}/sim.log)"
    elif [ "$status" = "ok" ]; then
        echo "     -> ${folder}  (${ncsv} CSVs)"
    fi

    echo "${id},${BATCH_ID},\"${tag}\",${nsta},${ngroup},${slotnum},${status},\"${sim_args}\",${folder}" >> "$MANIFEST"
}

# ---------------- BATERIAS ----------------

# Principal: cada Nsta em cada numero de grupos RAW (3 x 4 = 12 simulacoes)
sweep_groups() {
    local N G
    for N in $NSTA_LIST; do
        for G in $GROUP_LIST; do
            run_test "N${N}-G${G}" "$N" "$NUMSLOT" "$G" ""
        done
    done
}

sweep_n() {
    local N ng
    for N in $NSTA_LIST; do
        ng=$(ngroup_for "$N")
        run_test "N${N}" "$N" "$NUMSLOT" "$ng" ""
    done
}

sweep_slotnum() {
    local N SN ng
    for N in $NSTA_LIST; do
        ng=$(ngroup_for "$N")
        for SN in $SLOTNUM_LIST; do
            run_test "N${N}-RAW${SN}" "$N" "$SN" "$ng" ""
        done
    done
}

sweep_seed() {
    local S
    for S in $SEED_LIST; do
        run_test "seed${S}" "4" "$NUMSLOT" "1" "--seed=${S}"
    done
}

sweep_custom() {
    local tag nsta slot ngroup args
    while IFS='|' read -r tag nsta slot ngroup args; do
        [ -z "$tag" ] && continue
        run_test "$tag" "$nsta" "$slot" "$ngroup" "$args"
    done <<< "$CUSTOM_LIST"
}

# ---------------- UTILITARIOS ----------------
show_manifest() {
    if [ -f "$MANIFEST" ]; then
        if command -v column >/dev/null 2>&1; then
            column -s, -t < "$MANIFEST"
        else
            cat "$MANIFEST"
        fi
    else
        echo "(manifest ainda vazio)"
    fi
}

# Zera contador e manifest. NAO apaga as pastas de resultados.
reset_all() {
    reset_counter
    echo "id,batch,tag,nsta,ngroup,numslot,status,args,pasta" > "$MANIFEST"
    echo "Contador e manifest zerados (as pastas em ${RESULTS_DIR}/ foram mantidas)."
}

# ---------------- DISPATCHER ----------------
init

# Baterias zeram o contador ao comecar
start_batch() {
    reset_counter
    echo "Bateria ${BATCH_ID} (contador zerado)"
}

case "${1:-help}" in
    groups)   start_batch; sweep_groups ;;
    n)        start_batch; sweep_n ;;
    slot|raw) start_batch; sweep_slotnum ;;
    seed)     start_batch; sweep_seed ;;
    custom)   start_batch; sweep_custom ;;
    show)     show_manifest ;;
    reset)    reset_all ;;
    *)
        echo "Uso: $0 {groups|n|slot|raw|seed|custom|show|reset}"
        echo "  groups  Nsta {${NSTA_LIST}} x grupos RAW {${GROUP_LIST}}  (principal)"
        ;;
esac