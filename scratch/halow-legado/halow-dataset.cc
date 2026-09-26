#include "ns3/core-module.h"
#include "modulesDataset.h"
#include "../rca/Configuration.cc" 
#include <fstream>
#include <string>

using namespace ns3;

NS_LOG_COMPONENT_DEFINE ("WifiHalowDatasetExample");

/**
 * @brief Lê e traduz as regras de escalonamento RAW a partir de um arquivo de texto.
 * 
 * @details Funciona como o "tradutor" das regras de negócio do projeto. Ele abre o 
 * arquivo de configuração (ex: RawConfig-rca.txt), extrai a distribuição matemática 
 * dos nós (AIDs), grupos e slots de tempo  e os converte nas estruturas C++ exigidas pela placa de rede do protocolo 802.11ah.
 * 
 * @param rpslist Estrutura base de conjuntos de parâmetros RAW (RPS) a ser preenchida.
 * @param RAWConfigFile Caminho para o arquivo de texto contendo a matriz de configuração.
 * @param config Referência ao objeto de configuração global para atualizar o número total de nós.
 * @return RPSVector O vetor estruturado com todos os grupos e regras de acesso restrito.
 */
RPSVector configureRAW (RPSVector rpslist, std::string RAWConfigFile, Configuration &config) {
    RPS *m_rps = new RPS;

    // ============================================================
    //  Parâmetros extraídos dos arquivos oficiais do autor
    //  (OptimalRawGroup/RawConfig-*.txt)
    //
    //  - SlotFormat=1 → bidirecional (exige slotNum < 8)
    //  - slotNum=2    → valor padrão do autor
    //  - slotDur=102  → valor do RawConfig-32-2-2-51200-1-0.txt
    //  - 16 STAs/grupo → agrupamento do autor (evita truncamento
    //                    do beacon com N assignments)
    // ============================================================
    const uint32_t SLOT_FORMAT      = 1;
    const uint32_t SLOT_NUM         = 2;
    const uint32_t SLOT_DUR         = 102;
    const uint32_t STAsPorAssignment = 16;

    uint32_t N = config.Nsta;
    uint32_t aidAtual = 1;

    while (aidAtual <= N) {
        uint32_t aidFim = std::min<uint32_t>(aidAtual + STAsPorAssignment - 1, N);

        RPS::RawAssignment *m_raw = new RPS::RawAssignment;
        m_raw->SetRawControl (0);
        m_raw->SetSlotCrossBoundary (1);
        m_raw->SetSlotFormat (SLOT_FORMAT);
        m_raw->SetSlotDurationCount (SLOT_DUR);
        m_raw->SetSlotNum (SLOT_NUM);

        uint32_t page    = 0;
        uint32_t rawinfo = (aidFim << 13) | (aidAtual << 2) | page;
        m_raw->SetRawGroup (rawinfo);

        m_rps->SetRawAssignment (*m_raw);
        delete m_raw;

        aidAtual = aidFim + 1;
    }

    rpslist.rpsset.push_back (m_rps);
    config.NRawSta = N;

    std::cerr << "[RAW] N=" << N
              << " → " << ((N + STAsPorAssignment - 1) / STAsPorAssignment)
              << " grupos de " << STAsPorAssignment
              << ", slotNum=" << SLOT_NUM
              << ", slotDur=" << SLOT_DUR << std::endl;

    return rpslist;
}

int main (int argc, char *argv[])
{
    // Carrega as variáveis básicas
    Configuration config(argc, argv);

    // Lê as regras de janela estrita e joga na memória da config
    config.rps = configureRAW(config.rps, config.RAWConfigFile, config);
    
    // Estrutura as páginas (Slicing) exigidas pela placa HaLow
    config.pageS.SetPageindex (config.pageIndex);
    config.pageS.SetPagePeriod (config.pagePeriod);
    config.pageS.SetPageSliceLen (config.pageSliceLength);
    config.pageS.SetPageSliceCount (config.pageSliceCount);
    config.pageS.SetBlockOffset (config.blockOffset);
    config.pageS.SetTIMOffset (config.timOffset);
    config.tim.SetPageIndex (config.pageIndex);
    config.tim.SetDTIMPeriod (config.pageSliceCount ? config.pageSliceCount : 1);

    // parâmetros estáticos do projeto
    double distance = 2.0;
    uint32_t packetSize = 512;
    std::string outputFile = "scratch/dataset-halow.txt";

    LogComponentEnable ("UdpEchoClientApplication", LOG_LEVEL_INFO);
    LogComponentEnable ("UdpEchoServerApplication", LOG_LEVEL_INFO);
    
    HalowExperiment experiment (distance, packetSize, 200.0, config);
    
    experiment.Run ();
    experiment.ExportDataset (outputFile);
    

    Simulator::Destroy ();
    return 0;
}