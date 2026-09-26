#include "modulesDataset.h"
#include <fstream>
#include <iostream>

/**
 * @brief Construtor da classe HalowExperiment.
 * 
 * @param distance Distância entre os nós na grade (em metros).
 * @param packetSize Tamanho do payload do pacote UDP (em bytes).
 * @param simTime Tempo total de simulação (em segundos).
 * @param config Objeto de configuração contendo os parâmetros RAW/RPS e IEs extraídos do Configuration.h e .cc.
 */
HalowExperiment::HalowExperiment (double distance, uint32_t packetSize, double simTime, Configuration config)
    : m_distance (distance), 
      m_packetSize (packetSize), 
      m_simTime (simTime),
      m_config (config) {} 

/**
 * @brief Configura as camadas Física (PHY) e de Enlace (MAC) para o padrão IEEE 802.11ah (HaLow).
 * 
 * @details Configura o canal de rádio em 2 MHz. Utiliza o gerenciador dinâmico 
 * MinstrelWifiManager e herda a configuração S1g1MfieldEnabled para evitar erros de estouro 
 * de buffer (SIGIOT) durante a leitura do cabeçalho.
 */
void HalowExperiment::ConfigureWifi() {
    // Configuração PHY
    YansWifiChannelHelper channel = YansWifiChannelHelper::Default ();
    YansWifiPhyHelper phy = YansWifiPhyHelper::Default ();
    phy.SetChannel (channel.Create ());
    phy.Set ("ChannelWidth", UintegerValue (2)); 
    phy.Set ("ChannelNumber", UintegerValue (1));
    
    phy.Set ("S1g1MfieldEnabled", BooleanValue (m_config.S1g1MfieldEnabled));

    WifiHelper wifi;
    wifi.SetStandard (WIFI_PHY_STANDARD_80211ah);
    
    wifi.SetRemoteStationManager ("ns3::MinstrelWifiManager");
    

    S1gWifiMacHelper mac = S1gWifiMacHelper::Default (); 
    Ssid ssid = Ssid ("HaLow-Network");

    // Instala o Gateway com adequado ao RAW
    mac.SetType ("ns3::ApWifiMac",
                 "Ssid", SsidValue (ssid),
                 "BeaconInterval", TimeValue (MicroSeconds (m_config.BeaconInterval)),
                 "NRawStations", UintegerValue (m_config.NRawSta),
                 "RPSsetup", RPSVectorValue (m_config.rps),
                 "PageSliceSet", pageSliceValue (m_config.pageS),
                 "TIMSet", TIMValue (m_config.tim));
    m_apDevices = wifi.Install (phy, mac, m_apNode);

    // Instala os Nós
    mac.SetType ("ns3::StaWifiMac",
                 "Ssid", SsidValue (ssid),
                 "ActiveProbing", BooleanValue (false));
    m_staDevices = wifi.Install (phy, mac, m_staNodes);

    // Ativa captura de pacotes
    phy.EnablePcap ("dataset-ap", m_apDevices.Get (0));
    phy.EnablePcap ("dataset-sta", m_staDevices.Get (0));
}

/**
 * @brief Define o modelo espacial e o posicionamento físico dos nós.
 * 
 * @details Distribui o Gateway (AP) e os Sensores (STAs) em um formato 
 * de grade unificada (GridPositionAllocator) com espaçamento definido pela variável m_distance. 
 * Todos os nós permanecem estáticos
 */
void HalowExperiment::ConfigureMobility() {
    MobilityHelper mobility;
    mobility.SetPositionAllocator ("ns3::GridPositionAllocator",
                                   "MinX", DoubleValue (0.0),
                                   "MinY", DoubleValue (0.0),
                                   "DeltaX", DoubleValue (m_distance),
                                   "DeltaY", DoubleValue (m_distance),
                                   "GridWidth", UintegerValue (3),
                                   "LayoutType", StringValue ("RowFirst"));
    
    mobility.SetMobilityModel ("ns3::ConstantPositionMobilityModel");

    // Unifica os nós na grade e afasta-os com m_distance
    NodeContainer allNodes;
    allNodes.Add(m_apNode);
    allNodes.Add(m_staNodes);
    mobility.Install (allNodes);
}

/**
 * @brief Instala a pilha de protocolos de Internet (TCP/IP) e o roteamento global.
 * 
 */
void HalowExperiment::ConfigureNetwork() {
    InternetStackHelper stack;
    stack.Install (m_apNode);
    stack.Install (m_staNodes);

    Ipv4AddressHelper address;
    address.SetBase ("192.168.1.0", "255.255.255.0");
    m_apInterface = address.Assign (m_apDevices);
    m_staInterfaces = address.Assign (m_staDevices);
    
    Ipv4GlobalRoutingHelper::PopulateRoutingTables ();
}

/**
 * @brief Responsável pela injeção e orquestração do tráfego na rede (Camada de Aplicação).
 * 
 * @details Instala um servidor UDP Echo no Gateway (AP) escutando na porta 9 de forma contínua. 
 * Instala clientes UDP nos Sensores (STAs) configurados para disparar um volume fixo de pacotes 
 * em direção ao AP. Os tempos de início dos clientes são levemente defasados entre si para 
 * mitigar colisões perfeitas de CSMA/CA no instante inicial da transmissão.
 */
void HalowExperiment::ConfigureApplications() {
    UdpEchoServerHelper echoServer (9);
    ApplicationContainer serverApps = echoServer.Install (m_apNode.Get (0));
    serverApps.Start (Seconds (1.0));
    serverApps.Stop (Seconds (m_simTime));

    UdpEchoClientHelper echoClient (m_apInterface.GetAddress (0), 9);
    echoClient.SetAttribute ("MaxPackets", UintegerValue (1000));
    echoClient.SetAttribute ("Interval", TimeValue (Seconds (0.1)));
    echoClient.SetAttribute ("PacketSize", UintegerValue (m_packetSize));

   for (uint32_t i = 0; i < m_staNodes.GetN (); ++i) {
        ApplicationContainer client = echoClient.Install (m_staNodes.Get (i));
        client.Start (Seconds (60.0 + i * 0.05));   // 60.00, 60.05, 60.10, ...
        client.Stop  (Seconds (m_simTime));
   }
}

void HalowExperiment::Run() {
    m_apNode.Create (1);
    m_staNodes.Create (m_config.Nsta);

    ConfigureMobility();
    ConfigureWifi();
    ConfigureNetwork();
    ConfigureApplications();

    m_monitor = m_flowmonHelper.InstallAll();

    Simulator::Stop (Seconds (m_simTime));
    Simulator::Run ();
}

void HalowExperiment::ExportDataset(std::string filename) {
    m_monitor->CheckForLostPackets ();
    Ptr<Ipv4FlowClassifier> classifier = DynamicCast<Ipv4FlowClassifier> (m_flowmonHelper.GetClassifier ());
    std::map<FlowId, FlowMonitor::FlowStats> stats = m_monitor->GetFlowStats ();

    std::ofstream datasetFile;
    datasetFile.open(filename, std::ios_base::app); 
    
    datasetFile.seekp(0, std::ios::end);
    if (datasetFile.tellp() == 0) {
        datasetFile << "Distance(m),PacketSize(B),FlowID,SourceIP,DestIP,TxPackets,RxPackets,LostPackets,Throughput(kbps),Delay(ms)\n";
    }

    for (std::map<FlowId, FlowMonitor::FlowStats>::const_iterator i = stats.begin (); i != stats.end (); ++i) {
        Ipv4FlowClassifier::FiveTuple t = classifier->FindFlow (i->first);
        
        double throughput = 0;
        if (i->second.timeLastRxPacket.GetSeconds() > i->second.timeFirstTxPacket.GetSeconds()) {
            throughput = i->second.rxBytes * 8.0 / (i->second.timeLastRxPacket.GetSeconds() - i->second.timeFirstTxPacket.GetSeconds()) / 1024;
        }

        double delay = 0;
        if (i->second.rxPackets > 0) {
            delay = i->second.delaySum.GetSeconds() / i->second.rxPackets * 1000;
        }

        datasetFile << m_distance << ","
                    << m_packetSize << ","
                    << i->first << ","
                    << t.sourceAddress << ","
                    << t.destinationAddress << ","
                    << i->second.txPackets << ","
                    << i->second.rxPackets << ","
                    << i->second.lostPackets << ","
                    << throughput << ","
                    << delay << "\n";
    }

    datasetFile.close();
}