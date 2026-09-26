/*
 * halow-relay-tree.cc
 *
 * Evolução do cenário "WifiHalowStarTopology" para uma árvore de retransmissão:
 *
 *                      Gateway
 *                    /   |   |   \
 *                  R1   R2   R3   R4
 *                 / \  / \  / \  / \
 *               1a 1b 2a 2b 3a 3b 4a 4b
 *
 * Cada Relay possui 2 WifiNetDevices S1G (mesma config PHY/MAC do original):
 *   - StaWifiMac associado ao BSS do Gateway  (enlace "para cima")
 *   - ApWifiMac com BSS e RAW/RPS próprios    (enlace "para baixo")
 * O encaminhamento é feito na camada IP (Ipv4StaticRouting), com rotas
 * geradas a partir da árvore (RelayTree). O relay pai de cada nó final é
 * escolhido por RelaySelectionHelper (RSSI real do canal).
 */
#include "ns3/core-module.h"
#include "ns3/network-module.h"
#include "ns3/applications-module.h"
#include "ns3/wifi-module.h"
#include "ns3/mobility-module.h"
#include "ns3/propagation-module.h"
#include "ns3/internet-module.h"
#include "ns3/flow-monitor-module.h"
#include "../rca/Configuration.h"   // mantido como no cenário original
#include "../rca/Configuration.cc"  // mantido como no cenário original

#include "relay-selection-helper.h"
#include "relay-tree.h"
#include "relay-path-tracer.h"

#include "meupau.cc"

#include <fstream>
#include <string>
#include <iostream>
#include <sstream>
#include <cmath>

using namespace ns3;

NS_LOG_COMPONENT_DEFINE ("WifiHalowRelayTree");

static const uint32_t N_RELAYS = 4;
static const uint32_t CHILDREN_PER_RELAY = 2;

static std::map<Mac48Address, std::string> g_apName;

static void
OnAssoc (std::string sta, Mac48Address ap)
{
  std::map<Mac48Address, std::string>::const_iterator it = g_apName.find (ap);
  std::cout << "[Assoc] t=" << Simulator::Now ().GetSeconds () << "s  " << sta << " -> "
            << (it != g_apName.end () ? it->second : "?") << " (" << ap << ")\n";
}

static void
OnDeAssoc (std::string sta, Mac48Address ap)
{
  std::cout << "[DeAssoc] t=" << Simulator::Now ().GetSeconds () << "s  " << sta << " perdeu " << ap << "\n";
}

// ============================================================
// RAW (mesma lógica do original, agora parametrizada por AP)
// ============================================================
RPSVector
configureRAW (uint32_t nSta, const std::string &apName)
{
  RPSVector rpslist;
  RPS *m_rps = new RPS;

  const uint32_t SLOT_FORMAT       = 1;
  const uint32_t SLOT_NUM          = 2;
  const uint32_t SLOT_DUR          = 102;
  const uint32_t STAsPorAssignment = 16;

  uint32_t N        = nSta;
  uint32_t aidAtual = 1;

  while (aidAtual <= N)
    {
      uint32_t aidFim = std::min<uint32_t> (aidAtual + STAsPorAssignment - 1, N);

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
  std::cerr << "[RAW] " << apName << ": N=" << N
            << " → " << ((N + STAsPorAssignment - 1) / STAsPorAssignment)
            << " grupo(s) de " << STAsPorAssignment
            << ", slotNum=" << SLOT_NUM
            << ", slotDur=" << SLOT_DUR << std::endl;
  return rpslist;
}

// ============================================================
// CLASSE DE EXPERIMENTO
// ============================================================
class HalowRelayTreeExperiment
{
public:
  HalowRelayTreeExperiment (double relayDistance, double leafDistance, uint32_t packetSize,
                            double simTime, Configuration config);
  void Run ();
  void ExportDataset (std::string filename);

private:
  void CreateNodes ();
  void ConfigureMobility ();
  void ConfigureWifi ();
  void BuildRelayTree ();
  void ConfigureNetwork ();
  void ConfigureApplications ();
  void DemonstrateRelaySelection ();
  void TraceAssociations ();

  NetDeviceContainer InstallAp (WifiHelper &wifi, YansWifiPhyHelper &phy, S1gWifiMacHelper &mac,
                                Ptr<Node> node, Ssid ssid, uint32_t nSta);
  Ptr<NetDevice> GetEndDevice (Ptr<Node> endNode) const;
  static Ssid RelaySsid (uint32_t k);

  double m_relayDistance;   // Gateway ↔ Relay
  double m_leafDistance;    // Relay ↔ nó final
  uint32_t m_packetSize;
  double m_simTime;
  Configuration m_config;
  

  NodeContainer m_gateway;
  NodeContainer m_relays;
  NodeContainer m_endDevices;

  NetDeviceContainer m_gatewayDevice;     // ApWifiMac raiz
  NetDeviceContainer m_relayUpDevices;    // StaWifiMac dos relays
  NetDeviceContainer m_relayDownDevices;  // ApWifiMac dos relays
  NetDeviceContainer m_endNetDevices;     // StaWifiMac dos nós finais

  Ptr<LogDistancePropagationLossModel> m_loss;
  RelayTree m_tree;
  RelayPathTracer m_tracer;

  FlowMonitorHelper m_flowmonHelper;
  Ptr<FlowMonitor> m_monitor;
};

HalowRelayTreeExperiment::HalowRelayTreeExperiment (double relayDistance, double leafDistance,
                                                    uint32_t packetSize, double simTime,
                                                    Configuration config)
  : m_relayDistance (relayDistance),
    m_leafDistance (leafDistance),
    m_packetSize (packetSize),
    m_simTime (simTime),
    m_config (config),
    m_tracer (3)
{
}

Ssid
HalowRelayTreeExperiment::RelaySsid (uint32_t k)
{
  std::ostringstream oss;
  oss << "HaLow-Relay-" << (k + 1);
  return Ssid (oss.str ());
}

void
HalowRelayTreeExperiment::CreateNodes ()
{
  m_gateway.Create (1);
  m_relays.Create (N_RELAYS);
  m_endDevices.Create (N_RELAYS * CHILDREN_PER_RELAY);

  Names::Add ("Gateway", m_gateway.Get (0));
  for (uint32_t k = 0; k < N_RELAYS; ++k)
    {
      std::ostringstream r;
      r << "Relay" << (k + 1);
      Names::Add (r.str (), m_relays.Get (k));
      for (uint32_t c = 0; c < CHILDREN_PER_RELAY; ++c)
        {
          std::ostringstream n;
          n << "Node" << (k + 1) << static_cast<char> ('a' + c);
          Names::Add (n.str (), m_endDevices.Get (k * CHILDREN_PER_RELAY + c));
        }
    }
}

void
HalowRelayTreeExperiment::ConfigureMobility ()
{
  MobilityHelper mobility;
  Ptr<ListPositionAllocator> pos = CreateObject<ListPositionAllocator> ();
  const double PI = 3.14159265358979323846;

  // 1. Gateway no centro (como no original)
  pos->Add (Vector (0.0, 0.0, 0.0));

  // 2. Relays em círculo ao redor do Gateway (a "estrela" original, agora de relays)
  std::vector<Vector> relayPos;
  for (uint32_t k = 0; k < N_RELAYS; ++k)
    {
      double a = 2.0 * PI * k / N_RELAYS;
      Vector p (m_relayDistance * std::cos (a), m_relayDistance * std::sin (a), 0.0);
      relayPos.push_back (p);
      pos->Add (p);
    }

  // 3. Nós finais "atrás" de cada relay, abertos ±30°.
  //    O nome (Node1a...) indica o grupo geométrico; o PAI real é decidido
  //    por RelaySelectionHelper em BuildRelayTree().
  const double spread = PI / 6.0;
  for (uint32_t k = 0; k < N_RELAYS; ++k)
    {
      for (uint32_t c = 0; c < CHILDREN_PER_RELAY; ++c)
        {
          double a = 2.0 * PI * k / N_RELAYS + (c == 0 ? spread : -spread);
          pos->Add (Vector (relayPos[k].x + m_leafDistance * std::cos (a),
                            relayPos[k].y + m_leafDistance * std::sin (a), 0.0));
        }
    }

  mobility.SetPositionAllocator (pos);
  mobility.SetMobilityModel ("ns3::ConstantPositionMobilityModel");
  NodeContainer allNodes (m_gateway, m_relays, m_endDevices);   // mesma ordem do alocador
  mobility.Install (allNodes);
}

NetDeviceContainer
HalowRelayTreeExperiment::InstallAp (WifiHelper &wifi, YansWifiPhyHelper &phy,
                                     S1gWifiMacHelper &mac, Ptr<Node> node, Ssid ssid,
                                     uint32_t nSta)
{
  RPSVector rps = configureRAW (nSta, RelayNodeName (node));
  std::cout << "----->" << rps.rpsset.size() << endl;
  mac.SetType ("ns3::ApWifiMac",
               "Ssid", SsidValue (ssid),
               "BeaconInterval", TimeValue (MicroSeconds (m_config.BeaconInterval)),
               "NRawStations", UintegerValue (nSta),
               "RPSsetup", RPSVectorValue (rps),
               "PageSliceSet", pageSliceValue (m_config.pageS),
               "TIMSet", TIMValue (m_config.tim));
  return wifi.Install (phy, mac, node);
}

void
HalowRelayTreeExperiment::ConfigureWifi ()
{
  // Canal: MESMOS modelos que YansWifiChannelHelper::Default() instala
  // (LogDistance com atributos padrão + ConstantSpeed), porém criados
  // explicitamente para que o RelaySelectionHelper use o mesmo objeto.
  m_loss = CreateObject<LogDistancePropagationLossModel> ();
  Ptr<ConstantSpeedPropagationDelayModel> delay = CreateObject<ConstantSpeedPropagationDelayModel> ();

  Ptr<YansWifiChannel> backhaul = CreateObject<YansWifiChannel> ();  // Gateway <-> Relays
  backhaul->SetPropagationLossModel (m_loss);
  backhaul->SetPropagationDelayModel (delay);

  Ptr<YansWifiChannel> access = CreateObject<YansWifiChannel> ();    // Relays <-> nós finais
  access->SetPropagationLossModel (m_loss);
  access->SetPropagationDelayModel (delay);

  YansWifiPhyHelper phy = YansWifiPhyHelper::Default ();
  phy.SetChannel (backhaul);
  phy.Set ("ChannelWidth", UintegerValue (2));
  phy.Set ("ChannelNumber", UintegerValue (1));
  phy.Set ("S1g1MfieldEnabled", BooleanValue (m_config.S1g1MfieldEnabled));

  WifiHelper wifi;
  wifi.SetStandard (WIFI_PHY_STANDARD_80211ah);
  wifi.SetRemoteStationManager ("ns3::MinstrelWifiManager");

  S1gWifiMacHelper mac = S1gWifiMacHelper::Default ();
  Ssid rootSsid = Ssid ("HaLow-Star-Network");   // SSID original = BSS do Gateway

  // 1) Gateway (AP raiz): seus STAs são as interfaces "up" dos relays
  m_gatewayDevice = InstallAp (wifi, phy, mac, m_gateway.Get (0), rootSsid, N_RELAYS);

  // 2) Relay → Gateway (StaWifiMac)
  mac.SetType ("ns3::StaWifiMac",
               "Ssid", SsidValue (rootSsid),
               "ActiveProbing", BooleanValue (false));
  m_relayUpDevices = wifi.Install (phy, mac, m_relays);
  phy.SetChannel (access);   // APs dos relays e nós finais vão para o canal de acesso
  // 3) Relay → filhos (ApWifiMac com BSS e RAW próprios)
  for (uint32_t k = 0; k < N_RELAYS; ++k)
    {
      m_relayDownDevices.Add (InstallAp (wifi, phy, mac, m_relays.Get (k), RelaySsid (k),
                                         CHILDREN_PER_RELAY));
    }

  // 4) Nós finais (StaWifiMac). O SSID definitivo é aplicado em BuildRelayTree(),
  //    depois da seleção do relay pai (antes de Simulator::Run()).
  mac.SetType ("ns3::StaWifiMac",
               "Ssid", SsidValue (Ssid ("HaLow-NaoAssociado")),
               "ActiveProbing", BooleanValue (false));
  m_endNetDevices = wifi.Install (phy, mac, m_endDevices);

  phy.EnablePcapAll ("dataset-arvore");
}

Ptr<NetDevice>
HalowRelayTreeExperiment::GetEndDevice (Ptr<Node> endNode) const
{
  for (uint32_t i = 0; i < m_endDevices.GetN (); ++i)
    {
      if (m_endDevices.Get (i) == endNode)
        {
          return m_endNetDevices.Get (i);
        }
    }
  NS_FATAL_ERROR ("Nó final desconhecido: " << RelayNodeName (endNode));
  return 0;
}

void
HalowRelayTreeExperiment::BuildRelayTree ()
{
  m_tree.SetGateway (m_gateway.Get (0));
  for (uint32_t k = 0; k < N_RELAYS; ++k)
    {
      m_tree.AddRelay (m_relays.Get (k), RelaySsid (k));
    }

  RelaySelectionHelper::SetPropagationLossModel (m_loss);
  RelaySelectionHelper::SetMetric (RELAY_METRIC_RSSI_SUM);
  RelaySelectionHelper::SetVerbose (true);

  m_tree.Build (m_endDevices, CHILDREN_PER_RELAY);

  // Aplica a árvore na camada MAC: o nó final passa a associar-se ao BSS do relay escolhido
  for (uint32_t k = 0; k < m_tree.GetNRelays (); ++k)
    {
      const RelayNode &r = m_tree.GetRelay (k);
      for (uint32_t c = 0; c < r.children.size (); ++c)
        {
          Ptr<WifiNetDevice> dev = DynamicCast<WifiNetDevice> (GetEndDevice (r.children[c]));
          dev->GetMac ()->SetSsid (r.ssid);
        }
    }
}

void
HalowRelayTreeExperiment::ConfigureNetwork ()
{
  InternetStackHelper stack;
  stack.Install (m_gateway);
  stack.Install (m_relays);
  stack.Install (m_endDevices);

  Ipv4AddressHelper address;

  // BSS do Gateway: 192.168.0.0/24  (Gateway .1, Relays .2–.5)
  address.SetBase ("192.168.0.0", "255.255.255.0");
  Ipv4InterfaceContainer gwIf = address.Assign (m_gatewayDevice);
  Ipv4InterfaceContainer relayUpIf = address.Assign (m_relayUpDevices);
  m_tree.SetGatewayAddress (gwIf.GetAddress (0));

  // BSS do Relay k: 192.168.k.0/24  (Relay .1, filhos .2 e .3)
  for (uint32_t k = 0; k < m_tree.GetNRelays (); ++k)
    {
      RelayNode &r = m_tree.GetRelay (k);
      r.upAddress = relayUpIf.GetAddress (k);

      std::ostringstream net;
      net << "192.168." << (k + 1) << ".0";
      r.subnet = Ipv4Address (net.str ().c_str ());
      r.mask = Ipv4Mask ("255.255.255.0");
      address.SetBase (r.subnet, r.mask);

      Ipv4InterfaceContainer down = address.Assign (NetDeviceContainer (m_relayDownDevices.Get (k)));
      r.downAddress = down.GetAddress (0);

      for (uint32_t c = 0; c < r.children.size (); ++c)
        {
          Ipv4InterfaceContainer ci = address.Assign (NetDeviceContainer (GetEndDevice (r.children[c])));
          m_tree.SetEndNodeAddress (r.children[c], ci.GetAddress (0));
        }
    }

  // Encaminhamento: rotas estáticas derivadas da árvore
  // (PopulateRoutingTables() do original foi removido de propósito: o
  //  roteamento global não conhece a hierarquia Gateway→Relay→Node.)
  m_tree.InstallStaticRoutes ();

  Ipv4RoutingHelper::PrintRoutingTableAllAt (
      Seconds (59.0), Create<OutputStreamWrapper> ("scratch/arvore-rotas.txt", std::ios::out));
}

void
HalowRelayTreeExperiment::ConfigureApplications ()
{
  Ipv4Address gwAddr = m_tree.GetGatewayAddress ();

  // (a) Aplicações originais: servidor UDP no Gateway, clientes em todos os nós finais.
  //     Requisição: Node → Relay → Gateway ; resposta (eco): Gateway → Relay → Node
  UdpEchoServerHelper echoServer (9);
  ApplicationContainer serverApps = echoServer.Install (m_gateway.Get (0));
  serverApps.Start (Seconds (1.0));
  serverApps.Stop (Seconds (m_simTime));

  UdpEchoClientHelper echoClient (gwAddr, 9);
  echoClient.SetAttribute ("MaxPackets", UintegerValue (1000));
  echoClient.SetAttribute ("Interval", TimeValue (Seconds (0.1)));
  echoClient.SetAttribute ("PacketSize", UintegerValue (m_packetSize));

  for (uint32_t i = 0; i < m_endDevices.GetN (); ++i)
    {
      ApplicationContainer clientApp = echoClient.Install (m_endDevices.Get (i));
      clientApp.Start (Seconds (60.0 + i * 0.05));
      clientApp.Stop (Seconds (m_simTime));
    }

  // (b) Teste entre ramos: Node1a → Relay1 → Gateway → Relay3 → Node3a (e o eco de volta)
  Ptr<Node> src = Names::Find<Node> ("Node1a");
  Ptr<Node> dst = Names::Find<Node> ("Node3a");

  UdpEchoServerHelper interServer (10);
  ApplicationContainer interSrv = interServer.Install (dst);
  interSrv.Start (Seconds (1.0));
  interSrv.Stop (Seconds (m_simTime));

  UdpEchoClientHelper interClient (m_tree.GetEndNodeAddress (dst), 10);
  interClient.SetAttribute ("MaxPackets", UintegerValue (100));
  interClient.SetAttribute ("Interval", TimeValue (Seconds (1.0)));
  interClient.SetAttribute ("PacketSize", UintegerValue (m_packetSize));
  ApplicationContainer interCli = interClient.Install (src);
  interCli.Start (Seconds (61.0));
  interCli.Stop (Seconds (m_simTime));
}

void
HalowRelayTreeExperiment::DemonstrateRelaySelection ()
{
  m_tree.Print ();

  Ptr<Node> gw = m_gateway.Get (0);
  Ptr<Node> n1a = Names::Find<Node> ("Node1a");
  Ptr<Node> n3a = Names::Find<Node> ("Node3a");

  std::cout << "\n================ SELEÇÃO POR TRANSMISSÃO (diagnóstico) ================\n";
  RelaySelectionHelper::SelectBestRelay (m_relays, n1a, gw);   // subida do ramo de origem
  RelaySelectionHelper::SelectBestRelay (m_relays, gw, n3a);   // descida do ramo de destino

  std::cout << "\n================ ROTAS ESPERADAS (RelayTree::NextHop) ================\n"
            << "  " << m_tree.PathToString (n1a, gw) << "\n"
            << "  " << m_tree.PathToString (gw, n1a) << "\n"
            << "  " << m_tree.PathToString (n1a, n3a) << "\n"
            << "  " << m_tree.PathToString (n3a, n1a) << "\n";
}

void
HalowRelayTreeExperiment::Run ()
{
  CreateNodes ();

  ConfigureMobility ();

  ConfigureWifi ();

  BuildRelayTree ();

  ConfigureNetwork ();

  ConfigureApplications ();

  DemonstrateRelaySelection ();

  TraceAssociations ();


  m_monitor = m_flowmonHelper.InstallAll ();


  NodeContainer allNodes (m_gateway, m_relays, m_endDevices);
  m_tracer.Install (allNodes);


  //B.O no simulator
  Simulator::Stop (Seconds (m_simTime));
  Simulator::Run ();

  m_tracer.PrintSummary ();

}

void
HalowRelayTreeExperiment::ExportDataset (std::string filename)
{
  m_monitor->CheckForLostPackets ();
  Ptr<Ipv4FlowClassifier> classifier = DynamicCast<Ipv4FlowClassifier> (m_flowmonHelper.GetClassifier ());
  std::map<FlowId, FlowMonitor::FlowStats> stats = m_monitor->GetFlowStats ();

  std::ofstream datasetFile;
  datasetFile.open (filename.c_str (), std::ios_base::app);
  datasetFile.seekp (0, std::ios::end);
  if (datasetFile.tellp () == 0)
    {
      datasetFile << "RelayDistance(m),LeafDistance(m),PacketSize(B),FlowID,Source,Destination,"
                     "SourceIP,DestIP,TxPackets,RxPackets,LostPackets,PDR(%),TimesForwarded,"
                     "AvgHops,Throughput(kbps),Delay(ms),Jitter(ms)\n";
    }

  std::cout << "\n================ FLOWMONITOR ================\n";
  for (std::map<FlowId, FlowMonitor::FlowStats>::const_iterator i = stats.begin (); i != stats.end (); ++i)
    {
      Ipv4FlowClassifier::FiveTuple t = classifier->FindFlow (i->first);
      const FlowMonitor::FlowStats &s = i->second;

      double throughput = 0;
      if (s.timeLastRxPacket.GetSeconds () > s.timeFirstTxPacket.GetSeconds ())
        {
          throughput = s.rxBytes * 8.0 /
                       (s.timeLastRxPacket.GetSeconds () - s.timeFirstTxPacket.GetSeconds ()) / 1024;
        }
      double delay = (s.rxPackets > 0) ? s.delaySum.GetSeconds () / s.rxPackets * 1000 : 0;
      double jitter = (s.rxPackets > 1) ? s.jitterSum.GetSeconds () / (s.rxPackets - 1) * 1000 : 0;
      double pdr = (s.txPackets > 0) ? 100.0 * s.rxPackets / s.txPackets : 0;
      // saltos = 1 + encaminhamentos por pacote recebido (aprox.: inclui encaminhamentos de perdidos)
      double hops = (s.rxPackets > 0) ? 1.0 + static_cast<double> (s.timesForwarded) / s.rxPackets : 0;

      std::string srcName = m_tracer.NameOf (t.sourceAddress);
      std::string dstName = m_tracer.NameOf (t.destinationAddress);

      datasetFile << m_relayDistance << "," << m_leafDistance << "," << m_packetSize << ","
                  << i->first << "," << srcName << "," << dstName << ","
                  << t.sourceAddress << "," << t.destinationAddress << ","
                  << s.txPackets << "," << s.rxPackets << "," << s.lostPackets << ","
                  << pdr << "," << s.timesForwarded << "," << hops << ","
                  << throughput << "," << delay << "," << jitter << "\n";

      std::cout << "Flow " << i->first << ": " << srcName << " → " << dstName
                << "  Tx=" << s.txPackets << " Rx=" << s.rxPackets
                << " PDR=" << pdr << "%  saltos≈" << hops
                << "  atraso=" << delay << " ms\n";
    }
  datasetFile.close ();
}

void
HalowRelayTreeExperiment::TraceAssociations ()
{
  g_apName[Mac48Address::ConvertFrom (m_gatewayDevice.Get (0)->GetAddress ())] = "Gateway";
  for (uint32_t k = 0; k < N_RELAYS; ++k)
    {
      g_apName[Mac48Address::ConvertFrom (m_relayDownDevices.Get (k)->GetAddress ())] =
          "AP do " + RelayNodeName (m_relays.Get (k));
    }
  NetDeviceContainer stas (m_relayUpDevices, m_endNetDevices);
  for (uint32_t i = 0; i < stas.GetN (); ++i)
    {
      Ptr<WifiNetDevice> d = DynamicCast<WifiNetDevice> (stas.Get (i));
      std::string name = RelayNodeName (d->GetNode ());
      if (!d->GetMac ()->TraceConnect ("Assoc", name, MakeCallback (&OnAssoc)))
        std::cout << "[Aviso] trace 'Assoc' indisponível neste fork\n";
      d->GetMac ()->TraceConnect ("DeAssoc", name, MakeCallback (&OnDeAssoc));
    }
}

// ============================================================
// FUNÇÃO PRINCIPAL
// ============================================================
int main (int argc, char *argv[])
{
  // LogComponentEnable("StaWifiMac", LOG_LEVEL_ALL);
  // LogComponentEnable("RegularWifiMac", LOG_LEVEL_ALL);
  // LogComponentEnable("MacRxMiddle", LOG_LEVEL_ALL);
  // LogComponentEnable("MacLow", LOG_LEVEL_ALL);
  signal(SIGFPE, sigHandler); 
  Configuration config (argc, argv);

  // 8 nós finais (4 relays + 1 gateway completam os 13 nós)
  config.Nsta = N_RELAYS * CHILDREN_PER_RELAY;

  // Page slicing / TIM exatamente como no original (aplicados a TODOS os APs)
  config.pageS.SetPageindex (config.pageIndex);
  config.pageS.SetPagePeriod (config.pagePeriod);
  config.pageS.SetPageSliceLen (config.pageSliceLength);
  config.pageS.SetPageSliceCount (config.pageSliceCount);
  config.pageS.SetBlockOffset (config.blockOffset);
  config.pageS.SetTIMOffset (config.timOffset);
  config.tim.SetPageIndex (config.pageIndex);
  config.tim.SetDTIMPeriod (config.pageSliceCount ? config.pageSliceCount : 1);

  // Parâmetros estáticos (o CommandLine pertence a Configuration; para torná-los
  // configuráveis, adicione-os em rca/Configuration.cc)
  double relayDistance = 60.0;
  double leafDistance = 25.0;
  uint32_t packetSize = 512;
  double simTime = 200.0;
  std::string outputFile = "scratch/dataset-halow-arvore.txt";

  bool appLogs = false;   // true = logs do UdpEcho do original (muito verbosos com 9 clientes)
  if (appLogs)
    {
      LogComponentEnable ("UdpEchoClientApplication", LOG_LEVEL_INFO);
      LogComponentEnable ("UdpEchoServerApplication", LOG_LEVEL_INFO);
    }

  HalowRelayTreeExperiment experiment (relayDistance, leafDistance, packetSize, simTime, config);
  experiment.Run ();
  experiment.ExportDataset (outputFile);
  RelaySelectionHelper::Reset ();
  Simulator::Destroy ();
  return 0;
}
