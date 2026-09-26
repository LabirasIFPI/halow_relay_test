#pragma once
#include "ns3/core-module.h"
#include "ns3/network-module.h"
#include "ns3/applications-module.h"
#include "ns3/wifi-module.h"
#include "ns3/mobility-module.h"
#include "ns3/ipv4-global-routing-helper.h"
#include "ns3/internet-module.h"
#include "ns3/flow-monitor-module.h"
#include "../rca/Configuration.h" 

using namespace ns3;

class HalowExperiment {
public:
    HalowExperiment(double distance, uint32_t packetSize, double simTime, Configuration config);
    void Run();
    void ExportDataset(std::string filename);

private:
    void ConfigureMobility();
    void ConfigureWifi();
    void ConfigureNetwork();
    void ConfigureApplications();

    
    double m_distance; // Distância entre nós e gaetways na grade
    uint32_t m_packetSize; // Tamanho do pacote UDP em bytes
    double m_simTime; // Tempo em que a simulação fica ativa coletando dados
    Configuration m_config; // Importa o arquivo "Configuration" para configuração do RAW

    NodeContainer m_apNode; // Nó vazio do gateway
    NodeContainer m_staNodes; // Nós vazios a se comunicar com o gateway
    NetDeviceContainer m_apDevices; // Placas de rede do gateway
    NetDeviceContainer m_staDevices; // Placas de rede dos nós
    Ipv4InterfaceContainer m_apInterface; // Endereço IP do gateway para roteamento
    Ipv4InterfaceContainer m_staInterfaces; // Endereços IP dos nós para roteamento
    
    FlowMonitorHelper m_flowmonHelper;
    Ptr<FlowMonitor> m_monitor;
};