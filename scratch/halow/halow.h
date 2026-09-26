#pragma once
#include "ns3/core-module.h"
#include "ns3/network-module.h"
#include "ns3/applications-module.h"
#include "ns3/wifi-module.h"
#include "ns3/mobility-module.h"
#include "ns3/internet-module.h"
#include "ns3/flow-monitor-module.h"
#include "Configuration.h"

using namespace ns3;

class HalowExperiment {
public:
    HalowExperiment (Configuration &config);

    void Run ();
    void ExportDataset (std::string filename);

private:
    void ConfigureMobility ();
    void ConfigureWifi ();
    void ConfigureNetwork ();
    void ConfigureApplications ();

    double        m_distance;
    uint32_t      m_packetSize;
    double        m_simTime;
    Configuration m_config;   // cópia local (evita efeito colateral)

    NodeContainer          m_apNode;
    NodeContainer          m_staNodes;
    NetDeviceContainer     m_apDevices;
    NetDeviceContainer     m_staDevices;
    Ipv4InterfaceContainer m_apInterface;
    Ipv4InterfaceContainer m_staInterfaces;

    FlowMonitorHelper m_flowmonHelper;
    Ptr<FlowMonitor>  m_monitor;
};