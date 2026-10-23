#!/bin/bash
# Prepara o ambiente para rodar o halow-ofc pela linha de comando.
# Rodar a partir da raiz do repositório (Git Bash, WSL ou Linux), com o Docker aberto.
#   1. cria a imagem ns3-halow-env (Ubuntu 16.04 com o compilador do ns-3.23);
#   2. configura e compila ESTE repositório dentro do container;
#   3. instala as dependências do ahVisualizer (precisa do Node.js no computador).
# Diferente do setup antigo do halow-relay-tree, não clona outra cópia do ns-3:
# o simulador compilado é o código desta pasta.
set -e
RAIZ="$(cd "$(dirname "$0")" && pwd)"
IMAGEM="ns3-halow-env"

echo "== 1. Imagem Docker ${IMAGEM}"
docker build -t "${IMAGEM}" "${RAIZ}"

echo "== 2. Configuração e compilação do ns-3 (demora alguns minutos na primeira vez)"
MSYS_NO_PATHCONV=1 docker run --rm -v "${RAIZ}:/workspace/relay-halow-tree" -w /workspace/relay-halow-tree "${IMAGEM}" \
    bash -c 'CXXFLAGS="-std=c++11" ./waf configure --disable-examples --disable-tests && ./waf build'

echo "== 3. ahVisualizer"
if command -v npm >/dev/null 2>&1; then
    (cd "${RAIZ}/ahVisualizer/forwardsocketdata" && npm install)
else
    echo "npm não encontrado: instale o Node.js para usar o ahVisualizer (opcional)."
fi

echo "Pronto. Veja scratch/halow-ofc/readme.md, seção \"Como executar\"."
