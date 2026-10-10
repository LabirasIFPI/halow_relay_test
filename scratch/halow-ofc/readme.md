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
| `monitor.h` | Implementa a coleta de métricas de rádio e pacotes e a exportação dos resultados em CSV. |
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

O arquivo `monitor.h` implementa funções para acompanhar o comportamento da comunicação e exportar os resultados ao final da execução.

As principais métricas incluem:

- **RSSI:** potência do sinal recebido, em dBm.
- **Ruído e SNR:** nível de ruído e relação sinal-ruído.
- **Qualidade estimada do enlace:** valores normalizados derivados de RSSI e SNR.
- **Distância até o AP central:** distância geométrica calculada a partir dos modelos de mobilidade.
- **Pacotes IP:** eventos de transmissão, encaminhamento, recepção e entrega à aplicação.
- **Estatísticas de fluxo:** pacotes enviados e recebidos, bytes, atrasos e saltos, quando aplicáveis.
- **Energia estimada:** estimativas de energia associadas aos eventos de transmissão e recepção IP.

**Observação:** os valores de qualidade de enlace são aproximações heurísticas, não uma medição padronizada de LQI do IEEE 802.11ah. Da mesma forma, o consumo energético calculado pelo monitor é uma estimativa baseada em potência e tempo de transmissão, não uma medição realizada pelo framework de energia do NS-3.

## Como executar?

Os comandos abaixo pressupõem que o NS-3 já esteja configurado e compilado no ambiente utilizado pelo projeto.

A partir da raiz do repositório, compile o projeto:

```bash
./waf
```

Para executar o cenário com dez estações e tráfego UDP Echo:

```bash
./waf --run "halow-ofc --Nsta=10 --NRawSta=10 --TrafficType=udpecho"
```

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

Os arquivos gerados pelo monitor são:

| Arquivo | Conteúdo |
|---|---|
| `resultado_radio_rx_samples.csv` | Amostras de recepção na camada física, incluindo RSSI, ruído e SNR. |
| `resultado_link_ap_summary.csv` | Resumo dos enlaces em relação ao AP central, incluindo distância e estatísticas de sinal. |
| `resultado_ip_packet_events.csv` | Eventos de pacotes observados na camada IP. |
| `resultado_flow_summary.csv` | Estatísticas agregadas dos fluxos de tráfego. |
| `resultado_node_summary.csv` | Estatísticas agregadas por nó. |
| `resultado_simulation_summary.csv` | Resumo geral da simulação. |

A localização efetiva dos CSVs depende do diretório de trabalho a partir do qual a simulação é executada.

O programa também escreve estatísticas no terminal, como quantidade de pacotes enviados, entregues e ecoados, além da perda de pacotes. Outros arquivos auxiliares podem ser gerados pelos componentes de estatísticas e visualização.

## Próximos objetivos

Os próximos passos podem incluir:

1. Validar as métricas exportadas comparando os resultados dos CSVs com os logs e as estatísticas nativas do NS-3.
2. Executar experimentos com diferentes quantidades de estações, modos MCS, durações de simulação e configurações RAW.
3. Avaliar a escalabilidade do cenário, verificando o tempo de execução, o volume de dados coletados e o comportamento da associação das estações.
4. Documentar os formatos dos arquivos de saída para facilitar a análise experimental.
5. Investigar a implementação de topologias com relays e múltiplos saltos, caso esse seja um objetivo futuro do projeto.

