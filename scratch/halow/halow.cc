#include "halow.h"
#include <fstream>

HalowExperiment::HalowExperiment (Configuration &config)
    : m_config (config) {}

void HalowExperiment::ConfigureWifi () {
    YansWifiChannelHelper channel = YansWifiChannelHelper::Default ();
    YansWifiPhyHelper phy = YansWifiPhyHelper::Default ();
    phy.SetChannel (channel.Create ());
    phy.Set ("ChannelWidth",       UintegerValue (2));
    phy.Set ("ChannelNumber",      UintegerValue (1));
    phy.Set ("S1g1MfieldEnabled",  BooleanValue (m_config.S1g1MfieldEnabled));

    WifiHelper wifi;
    wifi.SetStandard (WIFI_PHY_STANDARD_80211ah);
    wifi.SetRemoteStationManager ("ns3::MinstrelWifiManager");

    S1gWifiMacHelper mac = S1gWifiMacHelper::Default ();
    Ssid ssid = Ssid ("HaLow-Network");

    // AP — RAW + PageSlice + TIM
    mac.SetType ("ns3::ApWifiMac",
                 "Ssid",           SsidValue (ssid),
                 "BeaconInterval", TimeValue (MicroSeconds (m_config.BeaconInterval)),
                 "NRawStations",   UintegerValue (m_config.NRawSta),
                 "RPSsetup",       RPSVectorValue (m_config.rps),
                 "PageSliceSet",   pageSliceValue (m_config.pageS),
                 "TIMSet",         TIMValue (m_config.tim));
    m_apDevices = wifi.Install (phy, mac, m_apNode);

    // STAs
    mac.SetType ("ns3::StaWifiMac",
                 "Ssid",          SsidValue (ssid),
                 "ActiveProbing", BooleanValue (false));
    m_staDevices = wifi.Install (phy, mac, m_staNodes);
}

void HalowExperiment::ConfigureMobility () {
    MobilityHelper mobility;
    mobility.SetPositionAllocator ("ns3::GridPositionAllocator",
                                   "MinX",       DoubleValue (0.0),
                                   "MinY",       DoubleValue (0.0),
                                   "DeltaX",     DoubleValue (m_config.distance),
                                   "DeltaY",     DoubleValue (m_config.distance),
                                   "GridWidth",  UintegerValue (m_staNodes.GetN () + 1),
                                   "LayoutType", StringValue ("RowFirst"));
    mobility.SetMobilityModel ("ns3::ConstantPositionMobilityModel");

    NodeContainer all;
    all.Add (m_apNode);
    all.Add (m_staNodes);
    mobility.Install (all);
}

void HalowExperiment::ConfigureNetwork () {
    InternetStackHelper stack;
    stack.Install (m_apNode);
    stack.Install (m_staNodes);

    Ipv4AddressHelper address;
    address.SetBase ("192.168.0.0", "255.255.0.0");   // /16, cabe N grande
    m_staInterfaces = address.Assign (m_staDevices);
    m_apInterface   = address.Assign (m_apDevices);

    Ipv4GlobalRoutingHelper::PopulateRoutingTables ();
}

void HalowExperiment::ConfigureApplications () {
    UdpEchoServerHelper echoServer (9);
    ApplicationContainer serverApps = echoServer.Install (m_apNode.Get (0));
    serverApps.Start (Seconds (1.0));
    serverApps.Stop  (Seconds (m_simTime));

    UdpEchoClientHelper echoClient (m_apInterface.GetAddress (0), 9);
    echoClient.SetAttribute ("MaxPackets", UintegerValue (m_config.maxNumberOfPackets));
    echoClient.SetAttribute ("Interval",   TimeValue (Seconds (m_config.interval)));
    echoClient.SetAttribute ("PacketSize", UintegerValue (m_config.payloadSize));

    Ptr<UniformRandomVariable> rv = CreateObject<UniformRandomVariable>();
    double spread = 10.0;

    for (uint32_t i = 0; i < m_staNodes.GetN (); ++i) {
        ApplicationContainer c = echoClient.Install (m_staNodes.Get (i));
        double randomStart = 60.0 + rv->GetValue (0.0, spread);
        c.Start (Seconds (randomStart));
        c.Stop  (Seconds (m_config.simulationTime));
    }
}

void HalowExperiment::Run () {
    m_apNode.Create (1);
    m_staNodes.Create (m_config.Nsta);

    ConfigureMobility ();
    ConfigureWifi ();
    ConfigureNetwork ();
    ConfigureApplications ();

    m_monitor = m_flowmonHelper.InstallAll ();

    Simulator::Stop (Seconds (m_config.simulationTime + 5.0));
    Simulator::Run ();
}

void HalowExperiment::ExportDataset (std::string filename) {
    m_monitor->CheckForLostPackets ();
    Ptr<Ipv4FlowClassifier> classifier =
        DynamicCast<Ipv4FlowClassifier> (m_flowmonHelper.GetClassifier ());
    auto stats = m_monitor->GetFlowStats ();

    std::ofstream out (filename, std::ios_base::trunc);
    out << "Distance(m),PacketSize(B),FlowID,SourceIP,DestIP,"
           "TxPackets,RxPackets,LostPackets,Throughput(kbps),Delay(ms)\n";

    for (auto &kv : stats) {
        Ipv4FlowClassifier::FiveTuple t = classifier->FindFlow (kv.first);
        double thr = 0, delay = 0;
        double dt = kv.second.timeLastRxPacket.GetSeconds ()
                  - kv.second.timeFirstTxPacket.GetSeconds ();
        if (dt > 0) thr = kv.second.rxBytes * 8.0 / dt / 1024;
        if (kv.second.rxPackets > 0)
            delay = kv.second.delaySum.GetSeconds () / kv.second.rxPackets * 1000;

        out << m_config.distance << "," << m_config.payloadSize << "," << kv.first << ","
            << t.sourceAddress << "," << t.destinationAddress << ","
            << kv.second.txPackets << "," << kv.second.rxPackets << ","
            << kv.second.lostPackets << "," << thr << "," << delay << "\n";
    }
    out.close ();

    std::cerr << "[DATASET] " << filename
              << " (" << stats.size () << " flows)" << std::endl;
}