# halow-ofc

## O que é?

O `halow-ofc` é um cenário de simulação de redes Wi-Fi HaLow (IEEE 802.11ah) desenvolvido sobre o NS-3. Ele permite configurar uma rede com um ponto de acesso (AP) e múltiplas estações (STAs), executar diferentes tipos de tráfego e coletar métricas para analisar o comportamento da comunicação sem fio.

A implementação combina recursos de configuração de **RAW (Restricted Access Window)** e **TIM (Traffic Indication Map)** com ferramentas de monitoramento para registrar informações sobre sinal de rádio, pacotes IP, fluxos de tráfego e estimativas de consumo energético.

## Quais foram as modificações realizadas?

O diretório `halow-ofc` reúne arquivos próprios para configuração da simulação, gerenciamento de eventos, coleta de estatísticas e monitoramento da rede.

Entre os principais arquivos estão:

| Arquivo | Responsabilidade |
|---|---|
| `s1g-test-tim-raw.cc` | Arquivo principal da simulação: criação dos nós, configuração da rede, execução e coleta dos resultados. |
| `s1g-test-tim-raw.h` | Declarações e funções auxiliares relacionadas ao cenário de simulação. |
| `Configuration.h` | Define os parâmetros de configuração da simulação. |
| `Configuration.cc` | Processa os argumentos recebidos pela linha de comando. |
| `monitor/` | Coleta de métricas e exportação em CSV. `monitor.h` é o ponto de entrada; `base.h` tem o comum; `monitor_enlace.h` mede cada enlace filho → pai (`link_frame_summary`); `monitor_trafego.h` mede pacotes e fluxos (`flow_summary`, `node_summary`). |
| `tree/` | Árvore de relays. `topologia.h` decide a árvore antes da simulação (grade, relays e pai de cada nó); `radio.h` prevê o SNR de um enlace; `lqi.h` lê a curva de LQI e converte SNR em LQI; `instalar_relays.h` constrói essa árvore no ns-3 (canais, rádios dos dois lados de cada relay, filas, posições, endereços IP e rotas). |
| `NodeEntry.h` / `NodeEntry.cc` | Mantêm informações e callbacks associados às estações. |
| `NodeStatistics.h` / `NodeStatistics.cc` | Armazenam estatísticas de cada nó. |
| `Statistics.h` / `Statistics.cc` | Organizam as estatísticas gerais da simulação. |
| `SimulationEventManager.h` / `SimulationEventManager.cc` | Gerenciam eventos e atualizações enviados ao visualizador. |
| `signal.cc` | Contém funcionalidades auxiliares relacionadas a sinais. |
| `cleaning-helper.h` | Reúne funcionalidades auxiliares de limpeza. |

O diretório também possui implementações de aplicações cliente-servidor para diferentes tipos de tráfego, incluindo UDP Echo, TCP Echo, ping-pong, câmeras IP, atualização de firmware e sensores.

### Configuração da rede Wi-Fi HaLow

O cenário cria um contêiner com `Nsta` estações e outro com um único AP:

```cpp
wifiStaNode.Create(config.Nsta);
wifiApNode.Create(1);
```

A comunicação utiliza o padrão `WIFI_PHY_STANDARD_80211ah`. O código configura o canal, o modelo de propagação, os parâmetros da camada física e a taxa de transmissão.

O SSID utilizado na rede é `ns380211ah`.

As estações são instaladas como dispositivos `StaWifiMac`, enquanto o ponto de acesso utiliza `ApWifiMac`. O código também configura parâmetros relacionados ao Beacon Interval, RAW, TIM e RPS (Restricted Access Window Parameter Set).

### Posicionamento dos nós

O AP é posicionado de acordo com o parâmetro `rho`. As estações são organizadas em uma disposição bidimensional, com espaçamento de 100 metros entre as posições da matriz.

A quantidade de colunas é calculada a partir do número de estações, utilizando a raiz quadrada de `Nsta + 1`. As posições são atribuídas aos nós antes da execução da simulação.

Essa organização permite experimentar diferentes quantidades de estações e observar como a rede se comporta conforme o cenário aumenta.

### Endereçamento IP

Após a instalação da pilha de Internet nos nós, os dispositivos do AP e das estações recebem endereços IP a partir da mesma sub-rede:

```cpp
address.SetBase("10.0.0.0", "255.0.0.0");
```

Portanto, a implementação atual configura uma rede IP compartilhada entre o AP e as STAs, e não sub-redes distintas para cada salto de uma topologia em árvore.

### Monitoramento da rede

Os arquivos da pasta `monitor/` implementam funções para acompanhar o comportamento da comunicação e exportar os resultados ao final da execução.

As principais métricas incluem:

- **RSSI:** potência do sinal recebido, em dBm.
- **Ruído e SNR:** nível de ruído e relação sinal-ruído.
- **Entrega por enlace:** quadros enviados e decodificados em cada enlace filho → pai (base do LQI calibrado por PDR).
- **Distância até o AP central:** distância geométrica calculada a partir dos modelos de mobilidade.
- **Estatísticas de fluxo:** pacotes enviados e recebidos, bytes, atrasos e saltos, quando aplicáveis.
- **Energia estimada:** estimativas de energia associadas aos eventos de transmissão e recepção IP.

**Observação:** o consumo energético calculado pelo monitor é uma estimativa baseada em potência e tempo de transmissão, não uma medição realizada pelo framework de energia do NS-3.

## Como executar?

O ns-3.23 precisa de um Linux antigo (Python 2 e GCC antigo), por isso a simulação roda dentro de um container Docker. Pré-requisitos: Docker Desktop aberto e, para o visualizador, Node.js.

**1. Preparar (uma vez).** Na raiz do repositório, pelo Git Bash, WSL ou Linux:

```bash
./setup_ns3_halow.sh
```

Ele cria a imagem `ns3-halow-env` (a partir do `Dockerfile` da raiz), configura e compila este repositório dentro do container e instala as dependências do `ahVisualizer`.

**2. Abrir um terminal no container**, com a pasta do repositório montada em `/workspace/relay-halow-tree`:

```bash
docker run -it --name ns3-relay-tree -v "$(pwd):/workspace/relay-halow-tree" -w /workspace/relay-halow-tree ns3-halow-env
```

No Git Bash, prefixe o comando com `MSYS_NO_PATHCONV=1`; no PowerShell, troque `$(pwd)` por `${PWD}`. Nas próximas vezes, use `docker start -ai ns3-relay-tree`.

**3. Dentro do container**, compile depois de cada mudança no código, gere o arquivo de RAW e rode:

```bash
./waf build
./waf --run "RAW-generate --NRawSta=10 --NGroup=1 --NumSlot=1 --beaconinterval=102400 --pageSliceCount=0 --pageSliceLen=1 --RAWConfigPath=OptimalRawGroup/RawConfig-10.txt"
./waf --run "halow-ofc --Nsta=10 --NRawSta=10 --TrafficType=udpecho --RAWConfigFile=OptimalRawGroup/RawConfig-10.txt --VisualizerIP=none"
```

O arquivo de RAW diz quantos grupos e slots RAW existem e quais AIDs ficam em cada um. A pasta `OptimalRawGroup/` está no `.gitignore`, então o arquivo precisa ser gerado em cada máquina, com `--NRawSta` igual ao número de nós da simulação. Sem ele, o simulador para com a mensagem "Unable to open RAW configuration file".

O `--VisualizerIP=none` desliga o envio ao visualizador. Sem ele, o simulador tenta o `localhost` do container e repete "Connecting to visualizer" / "Connection refused" durante toda a rodada (não afeta os resultados).

As rodadas do estudo de LQI e relays usam o `scratch/halow-ofc/rodar_lqi.sh`, que gera o `RawConfig.txt` e grava tudo em `results/lqi/<nome>/` (exemplo: `scratch/halow-ofc/rodar_lqi.sh teste 24 50 2 2 40 1 --Topology=area --AreaSize=200 --Relays=1 --VisualizerIP=none`).

**4. Visualizador (opcional).** No Windows, fora do container:

```bash
cd ahVisualizer/forwardsocketdata
node index.js
```

Abra http://localhost:8080 e, no container, rode a simulação trocando `--VisualizerIP=none` por `--VisualizerIP=host.docker.internal` (dentro do container, `localhost` é o próprio container):

```bash
./waf --run "halow-ofc --Nsta=10 --NRawSta=10 --TrafficType=udpecho --RAWConfigFile=OptimalRawGroup/RawConfig-10.txt --VisualizerIP=host.docker.internal"
```

Cada execução também grava um arquivo `.nss` em `results/`, que o visualizador reproduz depois.

O parâmetro `Nsta` define a quantidade de estações. `NRawSta` configura a quantidade de estações consideradas pelo AP para a configuração RAW. `TrafficType` seleciona o tipo de tráfego.

Entre os parâmetros reconhecidos pelo arquivo `Configuration.cc` estão:

| Parâmetro | Finalidade |
|---|---|
| `Nsta` | Quantidade de estações no cenário. |
| `NRawSta` | Quantidade de estações que suportam RAW. |
| `simulationTime` | Duração da simulação, em segundos. |
| `seed` | Semente de aleatoriedade. |
| `payloadSize` | Tamanho do payload, em bytes. |
| `BeaconInterval` | Intervalo entre beacons, em microssegundos. |
| `DataMode` | Modo de transmissão MCS. |
| `rho` | Parâmetro usado para posicionar o AP. |
| `TrafficType` | Tipo de tráfego da aplicação. |
| `RAWConfigFile` | Caminho do arquivo de configuração RAW. |
| `Outputpath` | Caminho configurado para arquivos auxiliares de saída. |

O código também depende de arquivos de configuração RAW e de diretórios de saída existentes nos caminhos definidos. Verifique esses caminhos antes de executar em outro ambiente.

## Resultados gerados

Ao final da simulação, o código chama `ExportarTudo("resultado", config.simulationTime)`, que exporta arquivos CSV com o prefixo `resultado`.

Os arquivos gerados são:

| Arquivo | Conteúdo |
|---|---|
| `resultado_topologia.csv` | Árvore montada antes da simulação (`tree/topologia.h`): posição, papel, pai, saltos, SNR e LQI previstos de cada STA. |
| `resultado_link_frame_summary.csv` | Quadros enviados e decodificados, PDR e SNR de cada enlace filho → pai. É a entrada do `calibrar_lqi.py`. |
| `resultado_flow_summary.csv` | Estatísticas agregadas dos fluxos de tráfego. |
| `resultado_node_summary.csv` | Estatísticas agregadas por nó, incluindo a RAW de cada nó. |

A localização efetiva dos CSVs depende do diretório de trabalho a partir do qual a simulação é executada.

O programa também escreve estatísticas no terminal, como quantidade de pacotes enviados, entregues e ecoados, além da perda de pacotes. Outros arquivos auxiliares podem ser gerados pelos componentes de estatísticas e visualização.

## Próximos objetivos

Os próximos passos podem incluir:

1. Validar as métricas exportadas comparando os resultados dos CSVs com os logs e as estatísticas nativas do NS-3.
2. Executar experimentos com diferentes quantidades de estações, modos MCS, durações de simulação e configurações RAW.
3. Avaliar a escalabilidade do cenário, verificando o tempo de execução, o volume de dados coletados e o comportamento da associação das estações.
4. Documentar os formatos dos arquivos de saída para facilitar a análise experimental.
5. Investigar a implementação de topologias com relays e múltiplos saltos, caso esse seja um objetivo futuro do projeto.

