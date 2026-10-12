# `run_halow.sh` — Guia de uso e bateria de 2500 STAs

Script Bash para automatizar simulações Wi-Fi HaLow (IEEE 802.11ah) no ns-3. Para cada caso, gera um `RawConfig`, executa o simulador e organiza os CSVs e logs em uma pasta própria.

## 1. Pré-requisitos

- Executar a partir da raiz do repositório, na pasta que contém `./waf`.
- Projeto compilado e executável com `./waf`.
- Programas em `scratch/`:
  - gerador RAW `RAW-generate` (nome diferencia maiúsculas/minúsculas);
  - simulador `halow-ofc` (padrão) ou outro escolhido por `SIM_PROJ`.
- Para suportar mais de 2047 STAs, usar **a versão multi-página corrigida tanto de `scratch/RAW-generate.cc` quanto de `src/wifi/model/ap-wifi-mac.cc`**. Atualizar só o gerador não basta.

```bash
chmod +x scratch/run_halow.sh
```

## 2. Executar a bateria específica de 2500 STAs

```bash
./scratch/run_halow.sh 2500
```

O comando faz o cruzamento de **4 valores de `NGroup` × 4 valores de `NumSlot` = 16 simulações**:

- `NGroup`: 1, 2, 3 e 4;
- `NumSlot`: 1, 2, 3 e 4;
- quantidade solicitada: 2500 STAs em todos os casos.

Como 2500 não é divisível por 3, os casos com `NGroup=3` usam **2502 STAs**. O nome da pasta e o arquivo `params.txt` registram a quantidade efetivamente usada. Nos demais casos, o total permanece 2500.

Por padrão, o simulador usa o tempo definido no próprio programa (nesta configuração, normalmente 60 s). Para uma rodada exploratória mais curta:

```bash
SIM_TIME=2 ./scratch/run_halow.sh 2500
```

A simulação curta é útil para verificar inicialização e associação, mas pode ser insuficiente para recolher métricas estáveis. Para resultados experimentais, repita com um tempo adequado e registre os parâmetros.

### O que cada combinação testa

| `NGroup` | `NumSlot` | STAs usadas | Finalidade |
|---:|---:|---:|---|
| 1 | 1, 2, 3, 4 | 2500 | Poucos grupos RAW; varia slots |
| 2 | 1, 2, 3, 4 | 2500 | Dois grupos por página; varia slots |
| 3 | 1, 2, 3, 4 | 2502 | Ajuste para divisibilidade; varia slots |
| 4 | 1, 2, 3, 4 | 2500 | Quatro grupos por página; varia slots |

O `NGroup` é passado ao gerador e ao simulador. Na geração multi-página, os grupos são distribuídos dentro de cada página de AIDs, portanto o número total de atribuições RAW no arquivo pode ser maior que `NGroup`.

## 3. Outros comandos

```bash
./scratch/run_halow.sh groups   # Nsta 625, 1250 e 2500 × NGroup 1, 2, 3 e 4
./scratch/run_halow.sh n        # uma configuração por tamanho, conforme NGROUP_MAP
./scratch/run_halow.sh slot     # varia NumSlot de 1 a 4 para cada tamanho
./scratch/run_halow.sh seed     # repetições com sementes diferentes
./scratch/run_halow.sh show     # exibe o manifest.csv
./scratch/run_halow.sh reset    # zera contador e manifest; mantém as pastas de resultados
```

`groups`, `n`, `slot`, `raw`, `seed` e `custom` também aplicam o ajuste de divisibilidade explicado abaixo.

## 4. Ajuste automático da quantidade de STAs

Antes de gerar a configuração RAW, o script verifica se a quantidade solicitada é divisível por `NGroup`. Se não for, **arredonda para cima até o próximo múltiplo**, conforme solicitado para padronizar os cenários comparados. Exemplos:

| Solicitado | `NGroup` | Quantidade usada |
|---:|---:|---:|
| 625 | 2 | 626 |
| 625 | 3 | 627 |
| 625 | 4 | 628 |
| 1250 | 3 | 1251 |
| 1250 | 4 | 1252 |
| 2500 | 3 | 2502 |

O ajuste é informado no terminal, em `params.txt` e no nome da pasta. Na versão multi-página corrigida, os restos dentro de cada página também são distribuídos entre os grupos, sem descartar STAs; o ajuste global serve para manter os cenários divisíveis conforme solicitado. A quantidade efetiva nunca pode ultrapassar 8191, pois o gerador considera AID 0 reservado e suporta AIDs válidos de 1 a 8191.

**Importante:** a divisibilidade total por `NGroup` não garante grupos de tamanho idêntico em cada página. A página 0 possui 2047 AIDs válidos (1–2047), enquanto as páginas 1–3 possuem 2048 AIDs cada (0–2047 localmente). O gerador distribui os AIDs restantes entre grupos dentro de cada página para não deixar estações de fora.

## 5. Configuração e variáveis de ambiente

| Variável | Padrão | Uso |
|---|---|---|
| `SIM_PROJ` | `halow-ofc` | Programa do simulador em `scratch/` |
| `RAW_PROJ` | `RAW-generate` | Programa gerador RAW |
| `RESULTS_DIR` | `results` | Pasta dos resultados |
| `SIM_TIME` | vazio | Tempo de simulação em segundos; vazio mantém o padrão do programa |

Exemplo:

```bash
SIM_PROJ=halow-tree SIM_TIME=30 ./scratch/run_halow.sh 2500
```

Variáveis configuradas no início do script:

| Variável | Padrão | Significado |
|---|---|---|
| `NSTA_LIST` | `625 1250 2500` | Tamanhos da bateria geral |
| `GROUP_LIST` | `1 2 3 4` | Valores de `NGroup` para `groups` |
| `N2500_GROUP_LIST` | `1 2 3 4` | Valores de `NGroup` da bateria específica de 2500 |
| `N2500_SLOT_LIST` | `1 2 3 4` | Valores de `NumSlot` da bateria específica de 2500 |
| `NUMSLOT` | `2` | Slots por RAW para baterias que não variam slots |
| `BEACON` | `102400` | Beacon interval em microssegundos |
| `PAGESLICE_COUNT` | `0` | Esta versão do gerador multi-página exige `0` |
| `PAGESLICE_LEN` | `1` | Comprimento de page slice passado ao gerador |
| `SEED_LIST` | `1 2 3` | Sementes da bateria `seed` |
| `SLOTNUM_LIST` | `1 2 3 4` | Slots da bateria `slot` |
| `NGROUP_MAP` | `1:1 2:1 4:1 8:2 16:2 32:4 64:4` | Mapa usado por `n` e `slot` |
| `CSV_PREFIX` | `resultado` | Prefixo dos CSVs exportados pelo simulador |
| `CSV_DIRS` | `. scratch` | Diretórios em que o script procura CSVs |

### Bateria personalizada

Defina `CUSTOM_LIST` no próprio script, uma simulação por linha, no formato `tag|nsta|slots|ngroup|args extras`:

```bash
CUSTOM_LIST="carga-alta|2500|2|2|--TrafficInterval=100
carga-baixa|2500|2|2|--TrafficInterval=10000"
./scratch/run_halow.sh custom
```

A quantidade pode ser ajustada para cima se não for divisível por `NGroup`.

## 6. Saída e rastreabilidade

Cada simulação tem sua pasta em `results/` contendo, quando gerados:

- `RawConfig.txt`: configuração RAW efetivamente usada;
- `sim.log`: saída completa do simulador;
- `params.txt`: comando, quantidade solicitada e quantidade usada, grupos, slots e avisos;
- `resultado_*.csv`: métricas exportadas;
- arquivos `.nss`, se o simulador os gerar.

O diretório `results/rawconfigs/` guarda as configurações RAW e os logs do gerador. `results/manifest.csv` registra cada execução. O contador volta a 1 no início de cada bateria, mas pastas já existentes não são sobrescritas: é acrescentado um sufixo de horário.

Estados registrados no manifest:

| Status | Significado |
|---|---|
| `ok` | O simulador terminou e foram encontrados CSVs |
| `falha_raw` | O gerador não produziu `RawConfig` |
| `falha_sim` | O comando de simulação retornou erro |
| `sem_csv` | O comando terminou, mas não foram encontrados CSVs |
| `falha_validacao_raw` | O arquivo gerado não cobre exatamente todos os AIDs esperados |
| `associacao_incompleta` | O log indica envio por STAs não associadas ou início de tráfego antes de todas associarem |

## 7. Limites conhecidos e interpretação

- O gerador corrigido aceita de 1 a 8191 STAs/AIDs válidos.
- O RPS gerado pode conter no máximo 42 atribuições RAW. Se o número de páginas × grupos exceder esse limite, reduza `NGroup`.
- O gerador multi-página exige `pageSliceCount=0`; o uso combinado com TIM page slicing não foi implementado nesta versão.
- Para mais de 2047 STAs, **gerador e AP precisam ser da versão multi-página corrigida**. Caso contrário, o gerador pode falhar ou o AP pode interpretar incorretamente AIDs de páginas superiores.
- O ajuste de divisibilidade impede sobras pela divisão inteira global, mas não faz todas as páginas terem exatamente o mesmo número de STAs por grupo, pois a página 0 tem um AID válido a menos.
- **Problema detectado durante o teste de 2500 STAs:** o código de `scratch/halow-ofc/s1g-test-tim-raw.cc` usa `if (GetAssocNum() <= config.Nsta && !trafficConfigured)` em `onSTAAssociated()`. Essa condição é verdadeira já na primeira associação; o log pode dizer `All 1 stations associated` e os clientes passam a registrar `cannot send since not associated`. Isso precisa ser corrigido no simulador (a condição deve ser revisada para só iniciar o tráfego quando a contagem atingir `config.Nsta`) antes de considerar válidos os resultados de throughput/perda para todos os nós.
- A bateria `2500` executa 16 simulações e pode consumir bastante memória/tempo. Use `SIM_TIME` curto para validar a execução inicial e depois rode com o tempo necessário para a medição.

## 8. Diagnóstico rápido

| Sintoma | Verificação |
|---|---|
| `falha_raw` | Veja `results/rawconfigs/rawgen-*.log`; confirme `RAW_PROJ` e `pageSliceCount=0` |
| `falha_sim` | Veja o `sim.log` correspondente e confirme `SIM_PROJ` |
| `sem_csv` | Confira o caminho de exportação; ajuste `CSV_DIRS` e `CSV_PREFIX` se necessário |
| Nem todas as STAs associam | Procure mensagens de associação no `sim.log`; verifique o suporte multi-página do AP |
| Nenhum pacote é transmitido | Confirme associação completa, configuração RAW e início das aplicações de tráfego |
| `NGroup` acima do suportado | Reduza grupos para que o RPS tenha no máximo 42 atribuições |
