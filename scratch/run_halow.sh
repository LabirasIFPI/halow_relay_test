#!/bin/bash
# ============================================================
# run_halow.sh — Bateria automatizada HaLow (ns-3)
#
# Uso:  ./scratch/run_halow.sh {groups|2500|n|slot|seed|custom|show|reset}
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
# Bateria focada em 2500 STAs: cruza grupos RAW (1..4) com slots por grupo (1..4).
N2500_GROUP_LIST="1 2 3 4"
N2500_SLOT_LIST="1 2 3 4"

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
    RAWGENLOG="${RAWDIR}/rawgen-N${nsta}-G${ngroup}-S${slotnum}.log"
    local genlog="$RAWGENLOG"

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


# Verifica que o RawConfig cobre exatamente os AIDs esperados, sem intervalos
# inválidos ou repetidos, antes de iniciar uma simulação cara.
validate_rawconfig() {
    local path="$1" expected="$2" logpath="$3"
    if ! awk -v expected="$expected" '
        NR == 1 { rps = $1; next }
        NR == 2 { declaredGroups = $1; next }
        NR >= 3 {
            if (NF < 8) { bad = 1; next }
            page = $6; start = $7; finish = $8
            if (page < 0 || page > 3 || start < 0 || finish > 2047 || start > finish) bad = 1
            if (page == 0 && start < 1) bad = 1
            count++
            total += finish - start + 1
            for (localAid = start; localAid <= finish; localAid++) {
                globalAid = page * 2048 + localAid
                if (globalAid < 1 || globalAid > 8191 || seen[globalAid]++) bad = 1
            }
        }
        END {
            if (rps != 1 || count != declaredGroups || count < 1 || count > 42 || total != expected || bad) exit 1
        }
    ' "$path"; then
        echo "     x RawConfig inválido: cobertura RAW não corresponde a ${expected} STAs (ver $logpath)"
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
    local tag="$1" requested_nsta="$2" slotnum="$3" ngroup="$4" extra_args="$5"
    local nsta="$requested_nsta" remainder=0 adjustment=""
    local id folder safe_tag status="ok"

    # Garante divisibilidade: aumenta a quantidade solicitada até o próximo
    # múltiplo de NGroup conforme solicitado; o gerador multi-página também distribui restos por página.
    if [ "$ngroup" -le 0 ]; then
        echo "     x NGroup precisa ser maior que zero (recebido: $ngroup)"
        return 1
    fi
    remainder=$((nsta % ngroup))
    if [ "$remainder" -ne 0 ]; then
        nsta=$((nsta + ngroup - remainder))
        adjustment="Nsta solicitado=${requested_nsta}; ajustado para ${nsta} para ser divisível por NGroup=${ngroup}."
        echo "     ! $adjustment"
    fi
    if [ "$nsta" -lt 1 ] || [ "$nsta" -gt 8191 ]; then
        echo "     x Nsta usado=${nsta} fora do intervalo suportado (1..8191 AIDs válidos)."
        return 1
    fi
    if [ "$ngroup" -gt 42 ]; then
        echo "     x NGroup=$ngroup pode exceder o limite de 42 atribuições RAW por RPS."
        return 1
    fi
    id=$(next_id)
    if [ "$nsta" != "$requested_nsta" ]; then
        tag="${tag}-usadoN${nsta}"
    fi
    safe_tag=$(printf '%s' "$tag" | sed 's/[^A-Za-z0-9_.-]/_/g')
    folder="${RESULTS_DIR}/$(printf '%02d' "$id")_${safe_tag}"
    if [ -e "$folder" ]; then
        folder="${folder}_$(date +%H%M%S)"   # nao sobrescreve baterias anteriores
    fi
    mkdir -p "$folder"

    printf "[%02d] %-22s Nsta=%-5s NGroup=%s NumSlot=%s\n" \
           "$id" "$tag" "$nsta" "$ngroup" "$slotnum"

    # ---- avisos informativos ----
    local warns="${adjustment}"
    if [ "$nsta" -gt 2047 ]; then
        warns="${warns}AIDs distribuídos por múltiplas páginas (0..3); exige gerador e AP com suporte multi-página. "
    fi
    [ -n "$warns" ] && echo "     ! $warns"

    if ! gen_rawconfig "$nsta" "$slotnum" "$ngroup" "$tag"; then
        status="falha_raw"
        echo "${id},${BATCH_ID},\"${tag}\",${nsta},${ngroup},${slotnum},${status},\"\",${folder}" >> "$MANIFEST"
        return
    fi
    if ! validate_rawconfig "$RAWPATH" "$nsta" "$RAWGENLOG"; then
        status="falha_validacao_raw"
        cp "$RAWPATH" "${folder}/RawConfig.txt"
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
        echo "nsta_requested=${requested_nsta}"
        echo "nsta_used=${nsta}"
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
    elif grep -q "cannot send since not associated" "${folder}/sim.log"; then
        status="associacao_incompleta"
        echo "     ! houve tentativa de envio por STA não associada; confira ${folder}/sim.log"
    fi

    # A mensagem atual do simulador informa a quantidade observada no momento
    # em que iniciou o tráfego; se for menor que Nsta, registra o caso como incompleto.
    if [ "$status" = "ok" ]; then
        local assoc_count
        assoc_count=$(grep -Eo 'All [0-9]+ stations associated' "${folder}/sim.log" | tail -1 | awk '{print $2}')
        if [ -n "$assoc_count" ] && [ "$assoc_count" -lt "$nsta" ]; then
            status="associacao_incompleta"
            echo "     ! simulador iniciou o tráfego com ${assoc_count}/${nsta} STAs associadas (ver ${folder}/sim.log)"
        fi
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

# Bateria solicitada: 2500 STAs em várias configurações RAW.
# Para NGroup=3, ajusta automaticamente 2500 -> 2502 para manter divisibilidade.
sweep_2500() {
    local G SN
    for G in $N2500_GROUP_LIST; do
        for SN in $N2500_SLOT_LIST; do
            run_test "N2500-G${G}-S${SN}" "2500" "$SN" "$G" ""
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
    2500|n2500) start_batch; sweep_2500 ;;
    n)        start_batch; sweep_n ;;
    slot|raw) start_batch; sweep_slotnum ;;
    seed)     start_batch; sweep_seed ;;
    custom)   start_batch; sweep_custom ;;
    show)     show_manifest ;;
    reset)    reset_all ;;
    *)
        echo "Uso: $0 {groups|2500|n|slot|raw|seed|custom|show|reset}"
        echo "  groups  Nsta {${NSTA_LIST}} x grupos RAW {${GROUP_LIST}}  (principal)"
        echo "  2500    2500 STAs x NGroup {${N2500_GROUP_LIST}} x slots {${N2500_SLOT_LIST}} (16 simulações)"
        ;;
esac
