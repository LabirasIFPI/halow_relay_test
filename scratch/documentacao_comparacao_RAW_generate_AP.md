# Comparação do `RAW-generate.cc` original com a versão multi-página e explicação do AP

## Resumo executivo

A versão corrigida **não produz exatamente a mesma configuração efetiva em todos os casos abaixo de 2048 STAs**.

- Com `NGroup=1`, nos casos testados até 2047 STAs, os campos numéricos da única atribuição RAW são iguais entre as versões (a formatação textual muda ligeiramente).
- Com `NGroup>1`, a versão original escrevia `1` como quantidade de atribuições no RPS, mesmo quando imprimia 2, 3 ou 4 linhas de grupos. O parser do simulador lê essa segunda linha como o número de atribuições e, portanto, só carregava a primeira linha RAW. A versão corrigida escreve a quantidade real de atribuições e faz o parser ler todos os grupos.
- Quando `Nsta` não é divisível por `NGroup`, a versão original usa divisão inteira e pode deixar os AIDs finais fora de qualquer grupo. A versão corrigida distribui o resto entre os grupos e cobre todos os AIDs válidos da faixa configurada.
- O gerador original só considera a página 0 e contém uma asserção que impede intervalos que ultrapassem 2047. A versão corrigida divide os AIDs entre as páginas 0 a 3, até 8191.

## O que foi alterado em `RAW-generate.cc`

### 1. Tipos e validação dos parâmetros

**Original:** os parâmetros de quantidade de estações, grupos e slots eram `uint16_t`; algumas variáveis não eram inicializadas antes de o parser receber os argumentos. A função dependia principalmente de `NS_ASSERT` para validar intervalos.

**Corrigido:** a função usa `uint32_t` para os parâmetros de contagem, inicializa os parâmetros no `main` e retorna uma mensagem de erro quando:

- `NRawSta` está fora de 1 a 8191;
- `NGroup` ou `NumSlot` é zero;
- `pageSliceCount` é diferente de zero;
- a configuração pede mais de 42 atribuições RAW no mesmo RPS;
- o beacon interval não comporta a duração calculada;
- os parâmetros de slot ou o caminho do arquivo são inválidos.

A faixa máxima 1–8191 representa AIDs globais utilizáveis neste modelo; AID 0 permanece reservado. `pageSliceCount>0` continua não suportado por esta implementação.

### 2. Distribuição dos AIDs por página

**Original:** `pageIndex` era fixo em zero. O código exigia que o fim do grupo fosse menor que 2048, por meio de `NS_ASSERT`. Portanto, o gerador não implementava distribuição multi-página.

**Corrigido:** cada atribuição é calculada dentro de uma página de 2048 posições e contém:

- página (`0` a `3`);
- AID inicial local à página;
- AID final local à página.

Os intervalos globais cobertos são:

| Página | AIDs globais usados | AIDs locais codificados |
|---|---:|---:|
| 0 | 1–2047 | 1–2047 |
| 1 | 2048–4095 | 0–2047 |
| 2 | 4096–6143 | 0–2047 |
| 3 | 6144–8191 | 0–2047 |

A página 0 começa no AID 1 porque o AID 0 é reservado. Nas páginas seguintes, o AID local 0 corresponde ao primeiro AID global daquela página (por exemplo, AID global 2048 na página 1).

### 3. Número de atribuições escrito no arquivo

**Original:** para `pageSliceCount=0`, escrevia sempre:

```text
1
1
```

A primeira linha significa um RPS. A segunda deveria informar quantas atribuições RAW há nesse RPS, mas o código escrevia `1` independentemente de `NGroup`. Apesar disso, em seguida imprimia uma linha para cada grupo.

**Corrigido:** escreve `1` como número de RPS e, na segunda linha, o número real de atribuições que foram geradas. Assim, o parser `configureRAW()` pode ler todas as linhas RAW previstas.

### 4. Resto da divisão entre grupos

**Original:** calculava `NstaPerRaw = NRawSta / NRawGroups` com divisão inteira e repetia esse tamanho em todos os grupos. Quando havia resto, alguns AIDs ficavam de fora. Por exemplo, 625 STAs divididas em 4 grupos geravam intervalos até 624, deixando o AID 625 sem atribuição.

**Corrigido:** distribui o resto entre os primeiros grupos, de modo que os tamanhos variam no máximo uma estação e todos os AIDs da página sejam cobertos. Para 625 STAs e 4 grupos, os intervalos passam a ser 1–157, 158–313, 314–469 e 470–625.

### 5. Duração dos slots

O gerador original calculava a duração com base em `NGroup × NumSlot`. A versão corrigida usa o número total de atribuições realmente geradas vezes `NumSlot`. Para cenários de uma única página com `NRawSta >= NGroup`, isso conserva a mesma base de cálculo. Em cenários multi-página, há mais atribuições no RPS e a duração é recalculada para caber no beacon interval.

## Comparação executada para valores abaixo do limite antigo

Foram compilados e executados os dois arquivos C++ em um harness isolado, com implementações substitutas mínimas para `CommandLine` e `NS_ASSERT`. **Isso testa a lógica do gerador, mas não é uma compilação do ns-3 nem uma simulação de rede.** Os parâmetros usados foram `NumSlot=2`, `beaconinterval=102400` e `pageSliceCount=0`; testaram-se `Nsta=100, 625, 1250, 2047` e `NGroup=1, 2, 3, 4`.

O parser `configureRAW()` do projeto foi então aplicado logicamente ao formato produzido: ele lê a primeira linha como número de RPS, a segunda como número de atribuições naquele RPS e, em seguida, lê somente esse número de linhas.

| Nsta | NGroup | Grupos impressos pela versão original | Cobertura efetivamente lida pelo parser original | Grupos corrigidos | Cobertura lida pelo parser corrigido |
|---:|---:|---:|---:|---:|---:|
| 100 | 1 | 1 | 100 | 1 | 100 |
| 100 | 2 | 2 | 50 | 2 | 100 |
| 100 | 3 | 3 | 33 | 3 | 100 |
| 100 | 4 | 4 | 25 | 4 | 100 |
| 625 | 1 | 1 | 625 | 1 | 625 |
| 625 | 2 | 2 | 312 | 2 | 625 |
| 625 | 3 | 3 | 208 | 3 | 625 |
| 625 | 4 | 4 | 156 | 4 | 625 |
| 1250 | 1 | 1 | 1250 | 1 | 1250 |
| 1250 | 2 | 2 | 625 | 2 | 1250 |
| 1250 | 3 | 3 | 416 | 3 | 1250 |
| 1250 | 4 | 4 | 312 | 4 | 1250 |
| 2047 | 1 | 1 | 2047 | 1 | 2047 |
| 2047 | 2 | 2 | 1023 | 2 | 2047 |
| 2047 | 3 | 3 | 682 | 3 | 2047 |
| 2047 | 4 | 4 | 511 | 4 | 2047 |

A coluna de cobertura corresponde ao número de AIDs contidos nas atribuições que o parser efetivamente lê, e não à soma de todas as linhas que aparecem no arquivo original. Por isso, mesmo quando todas as linhas originais somadas parecem cobrir quase todos os AIDs, o parser só carrega a primeira atribuição por causa do cabeçalho incorreto.

**Conclusão da comparação:** a versão corrigida preserva os campos numéricos da atribuição para os casos testados com `NGroup=1`, mas muda intencionalmente a configuração efetiva quando `NGroup>1`. A mudança corrige a contagem de atribuições e a cobertura dos grupos; não é uma equivalência byte a byte com a saída antiga.

## Por que também foi corrigido `ap-wifi-mac.cc`?

O gerador produz um arquivo de configuração. O AP é o componente que interpreta essa configuração durante a simulação e decide em qual grupo/slot RAW uma estação pode acessar o meio. Alterar só o gerador não basta para suportar AIDs de páginas 1, 2 e 3.

### A. Seleção do grupo em `GetSlotStartTimeFromAid()`

**Antes:** o AP comparava o AID global diretamente com `GetRawGroupAIDStart()` e `GetRawGroupAIDEnd()`. Porém, em uma atribuição multi-página, esses campos são AIDs locais de 11 bits. Um intervalo local `0–2047` da página 1 não significa os AIDs globais `0–2047`; significa os AIDs globais `2048–4095`.

**Depois:** o AP separa o AID global em:

- página: `(aid >> 11) & 0x03`;
- AID local: `aid & 0x07ff`.

Ele só considera uma atribuição quando a página coincide e o AID local está dentro do intervalo. Sem isso, as atribuições das páginas superiores poderiam ser confundidas com as da página 0.

### B. Lista de acesso em `SendOneBeacon()`

**Antes:** o AP usava os valores inicial/final da atribuição diretamente como índices globais para procurar a estação em `m_AidToMacAddr`. Isso só faz sentido para a página 0.

**Depois:** calcula `pageOffset = page * 2048` e converte o intervalo local em global:

`AID global = pageOffset + AID local`.

Por exemplo, uma atribuição da página 1 com intervalo local `0–452` vira o intervalo global `2048–2500`. Assim o AP procura as estações corretas no mapa AID→MAC.

### C. AIDs ainda não presentes no mapa

O AP antigo percorria todos os AIDs esperados e usava `find(k)->second` sem verificar se `find(k)` realmente encontrou uma entrada. Durante a associação, o mapa pode ainda não conter todos os AIDs; dereferenciar `end()` pode provocar comportamento indefinido ou falha.

A versão corrigida percorre as entradas que já existem no mapa e verifica o resultado de `find()` antes de acessar o valor. Isso é uma proteção durante a associação e não, por si só, uma garantia de que todas as estações conseguirão se associar.

## Limites e validação pendente

- A configuração corrigida exige `pageSliceCount=0`; combinar TIM page slicing com esta distribuição RAW ainda não foi implementado.
- O gerador permite no máximo 42 atribuições RAW no mesmo RPS. `NGroup` passa a significar grupos por página; com várias páginas, o total de atribuições é a soma dos grupos gerados em cada página.
- A comparação acima foi feita com o gerador compilado em harness isolado e com o parser interpretado segundo seu código-fonte. Não comprova a compilação completa do projeto ns-3 nem o comportamento de rádio, associação ou tráfego.
- A confirmação final requer compilar o projeto e executar simulações reais, observando associação, cobertura dos AIDs por grupo, eventos de slot e pacotes transmitidos/recebidos.
