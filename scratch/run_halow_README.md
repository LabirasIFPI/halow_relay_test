# run_halow.sh — Guia de uso

Script que automatiza baterias de simulações HaLow (IEEE 802.11ah) no ns-3: gera o `RawConfig`, roda o simulador e organiza os CSVs de cada simulação em uma pasta própria.

## 1. Pré-requisitos

- Rodar **da raiz do projeto** (a pasta onde está o `./waf`).
- Projeto compilável com `./waf`, contendo dois programas em `scratch/`:
  - o gerador de RAW: `RAW-generate` (nome é *case-sensitive*);
  - o simulador (por padrão `halow-ofc`).
- Permissão de execução:

```bash
chmod +x scratch/run_halow.sh
```

## 2. Uso rápido

```bash
./scratch/run_halow.sh groups
```

Roda a bateria principal: **Nsta `625 1250 2500` × `1 2 3 4` grupos RAW** (12 simulações).

Teste rápido com simulação curta:

```bash
SIM_TIME=10 ./scratch/run_halow.sh groups
```

## 3. Comandos

| Comando | O que faz |
|---|---|
| `groups` | **Principal.** Cada Nsta de `NSTA_LIST` em cada nº de grupos de `GROUP_LIST`. |
| `n` | Uma simulação por Nsta, com nº de grupos tirado de `NGROUP_MAP` (padrão 1). |
| `slot` / `raw` | Para cada Nsta, varia o nº de slots por RAW (`SLOTNUM_LIST`). |
| `seed` | 4 STAs, 1 grupo, variando a semente (`SEED_LIST`). |
| `custom` | Roda as simulações listadas em `CUSTOM_LIST` (ver seção 6). |
| `show` | Mostra o `manifest.csv` em tabela. |
| `reset` | Zera contador e manifest. **Não apaga** as pastas de resultados. |

Qualquer outro argumento mostra a ajuda.

## 4. O que o script faz em cada simulação

1. Incrementa o contador e cria a pasta `results/NN_<tag>/`.
2. Emite avisos de configuração (ver seção 8).
3. Chama `RAW-generate` e grava `results/rawconfigs/RawConfig-N<nsta>-G<grupos>-S<slots>.txt`.
4. Roda o simulador passando `--Nsta`, `--NGroup`, `--NRawSlotNum`, `--RAWConfigFile`, `--BeaconInterval`, `--pageSliceCount`, `--pageSliceLength` (e `--simulationTime` se `SIM_TIME` estiver definido).
5. Move os `resultado_*.csv` (e `.nss`, se houver) para a pasta da simulação.
6. Registra uma linha no `manifest.csv`.

## 5. Estrutura de saída

```
results/
├── manifest.csv
├── .counter
├── rawconfigs/
│   ├── RawConfig-N625-G1-S2.txt
│   └── rawgen-N625-G1-S2.log
├── 01_N625-G1/
│   ├── resultado_rssi.csv
│   ├── resultado_rssi_resumo.csv
│   ├── resultado_pacotes.csv
│   ├── resultado_fluxos.csv
│   ├── resultado_nos.csv
│   ├── RawConfig.txt        # RAW usada nesta simulação
│   ├── sim.log              # saída do simulador
│   └── params.txt           # comando exato e avisos
├── 02_N625-G2/
└── ...
```

**Contador:** volta a 1 no início de cada bateria. Se já existir uma pasta com o mesmo nome de uma bateria anterior, o script **não sobrescreve** e acrescenta um sufixo de hora (ex.: `01_N625-G1_232850`).

### manifest.csv

Colunas: `id, batch, tag, nsta, ngroup, numslot, status, args, pasta`.

| status | Significado |
|---|---|
| `ok` | Simulação terminou e gerou CSVs. |
| `falha_raw` | `RAW-generate` não produziu o arquivo (ver `results/rawconfigs/rawgen-*.log`). |
| `falha_sim` | O `waf --run` do simulador retornou erro (ver `sim.log`). |
| `sem_csv` | Rodou sem erro, mas nenhum `resultado_*.csv` foi encontrado. |

O `batch` (data-hora) identifica a execução, já que o `id` reinicia a cada bateria.

## 6. Configuração

### Variáveis de ambiente (sem editar o script)

| Variável | Padrão | Uso |
|---|---|---|
| `SIM_PROJ` | `halow-matrices-test` | Nome do programa do simulador em `scratch/`. |
| `RAW_PROJ` | `RAW-generate` | Nome do gerador de RAW. |
| `RESULTS_DIR` | `results` | Pasta de saída. |
| `SIM_TIME` | *(vazio)* | Tempo de simulação em segundos; vazio usa o padrão do simulador. |

Exemplo:

```bash
SIM_PROJ=halow-ofc SIM_TIME=30 ./scratch/run_halow.sh groups
```

### Variáveis no topo do script (editar o arquivo)

| Variável | Padrão | Significado |
|---|---|---|
| `NSTA_LIST` | `625 1250 2500` | Quantidades de STAs. |
| `GROUP_LIST` | `1 2 3 4` | Nº de grupos RAW da bateria `groups`. |
| `NUMSLOT` | `2` | Slots por RAW. |
| `BEACON` | `102400` | Beacon interval em µs (usado no gerador **e** no simulador). |
| `PAGESLICE_COUNT` / `PAGESLICE_LEN` | `0` / `1` | Page slicing. |
| `SEED_LIST` | `1 2 3` | Sementes da bateria `seed`. |
| `SLOTNUM_LIST` | `1 2 3 4` | Slots da bateria `slot`. |
| `NGROUP_MAP` | `1:1 2:1 4:1 ...` | Mapa Nsta→grupos usado por `n` e `slot` (sem entrada, usa 1). |
| `CSV_PREFIX` | `resultado` | Prefixo dos CSVs (deve bater com `ExportarTudo("resultado", ...)`). |
| `CSV_DIRS` | `. scratch` | Onde procurar os CSVs gerados. |

### Bateria personalizada (`custom`)

Preencha `CUSTOM_LIST`, uma simulação por linha, no formato `tag|nsta|slots|ngroup|args extras`:

```bash
CUSTOM_LIST="carga-alta|625|2|1|--TrafficInterval=100
carga-baixa|625|2|1|--TrafficInterval=10000"
```

Para variar a carga use `--TrafficInterval` (em ms).

## 7. Exemplos

```bash
./scratch/run_halow.sh groups                      # bateria principal
SIM_TIME=10 ./scratch/run_halow.sh groups          # versão curta, para testar
./scratch/run_halow.sh show                        # ver o manifest
./scratch/run_halow.sh reset                       # zerar contador e manifest
nohup ./scratch/run_halow.sh groups > batch.log 2>&1 &   # rodar em segundo plano
```

## 8. Avisos e limitações

- **Nsta não divisível por NGroup:** o `RAW-generate` faz divisão inteira, então algumas STAs ficam **fora dos grupos RAW** (ex.: 625 com 2, 3 ou 4 grupos; 1250 com 3 ou 4). O script avisa no terminal e no `params.txt`, mas não corrige.
- **Nsta > 2047:** o gerador usa só a página 0 (AIDs até 2047) e tem um `NS_ASSERT` nisso. Em build *debug* ele aborta; em build otimizado gera AIDs inválidos. Isso afeta os casos com 2500 STAs.
- **Tempo:** são 12 simulações com até 2500 nós; pode levar horas. Use `SIM_TIME` para testes e `nohup`/`tmux` para a bateria completa.
- **Associação:** se as STAs não associarem (por exemplo, por distância na grade), o tráfego não inicia, pois só começa quando todas associam. Veja o `sim.log`.

## 9. Problemas comuns

| Sintoma | O que verificar |
|---|---|
| `x raw-generate falhou` | `results/rawconfigs/rawgen-*.log`; confirme que `RAW_PROJ` é `RAW-generate`. |
| `x simulacao falhou` | `sim.log` da pasta; confirme `SIM_PROJ` e se o projeto compila (`./waf`). |
| `nenhum resultado_*.csv encontrado` | Confira onde o simulador grava os CSVs e ajuste `CSV_DIRS` / `CSV_PREFIX`. |
| Pasta com sufixo de hora | Normal: já existia uma pasta com o mesmo nome de bateria anterior. |
| `column: command not found` no `show` | O script cai para `cat` automaticamente; a mensagem não deve aparecer. |
