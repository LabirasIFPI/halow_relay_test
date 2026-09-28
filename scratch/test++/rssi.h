#ifndef RSSI_H
#define RSSI_H

// ============================================================
// rssi.h - Sniffer de RSSI para simulação 802.11ah
// ============================================================

#include "ns3/core-module.h"
#include "ns3/network-module.h"
#include "ns3/wifi-module.h"
#include "ns3/sta-wifi-mac.h"
#include "ns3/ap-wifi-mac.h"

#include <fstream>
#include <string>
#include <vector>
#include <map>
#include <iostream>

using namespace ns3;

// ------------------------------------------------------------
// 1. Estrutura de registro de RSSI
// ------------------------------------------------------------
struct RssiRecord {
    double      time;
    uint32_t    nodeId;
    uint32_t    deviceIndex;
    std::string ifaceType;
    double      rssi;
    double      noise;
    double      snr;
    uint32_t    senderNodeId;
    uint32_t    packetSize;
};

// ------------------------------------------------------------
// 2. Variáveis globais
// ------------------------------------------------------------
std::vector<RssiRecord>    g_rssiHistory;
std::map<uint32_t, double> g_lastRssi;

// ------------------------------------------------------------
// 3. Functor que captura o RSSI
// ------------------------------------------------------------
class RssiSniffer
{
public:
    RssiSniffer(uint32_t nodeId, uint32_t devIdx, std::string iface)
        : m_nodeId(nodeId), m_devIdx(devIdx), m_iface(iface) {}

    void operator()(Ptr<const Packet> packet,
                    uint16_t channelFreqMhz,
                    uint16_t channelNumber,
                    uint32_t rate,
                    bool isShortPreamble,
                    WifiTxVector txVector,
                    double signalDbm,
                    double noiseDbm) const
    {
        RssiRecord rec;
        rec.time         = Simulator::Now().GetSeconds();
        rec.nodeId       = m_nodeId;
        rec.deviceIndex  = m_devIdx;
        rec.ifaceType    = m_iface;
        rec.rssi         = signalDbm;
        rec.noise        = noiseDbm;
        rec.snr          = signalDbm - noiseDbm;
        rec.senderNodeId = 0;
        rec.packetSize   = packet ? packet->GetSize() : 0;

        g_rssiHistory.push_back(rec);
        g_lastRssi[m_nodeId] = signalDbm;

        NS_LOG_UNCOND(Simulator::Now().GetSeconds() << "s | Nó " << m_nodeId
                      << " (" << m_iface << ") RSSI = " << signalDbm
                      << " dBm | Noise = " << noiseDbm << " dBm");
    }

private:
    uint32_t    m_nodeId;
    uint32_t    m_devIdx;
    std::string m_iface;
};

// ------------------------------------------------------------
// 4. Guarda os sniffers vivos durante a simulação
// ------------------------------------------------------------
std::vector<RssiSniffer*> g_sniffers;

// ------------------------------------------------------------
// 5. Instala o sniffer em todos os nós
// ------------------------------------------------------------
void InstalarSnifferRssiEmTodos(NodeContainer nodes)
{
    for (uint32_t i = 0; i < nodes.GetN(); ++i) {
        Ptr<Node> node = nodes.Get(i);
        uint32_t nodeId = node->GetId();

        for (uint32_t d = 0; d < node->GetNDevices(); ++d) {
            Ptr<NetDevice> dev = node->GetDevice(d);
            Ptr<WifiNetDevice> wifiDev = DynamicCast<WifiNetDevice>(dev);
            if (!wifiDev) continue;

            Ptr<WifiPhy> phy = wifiDev->GetPhy();
            if (!phy) continue;

            Ptr<WifiMac> mac = wifiDev->GetMac();
            std::string ifaceType = "UNKNOWN";
            if (DynamicCast<StaWifiMac>(mac))      ifaceType = "STA";
            else if (DynamicCast<ApWifiMac>(mac))  ifaceType = "AP";

            RssiSniffer* sniffer = new RssiSniffer(nodeId, d, ifaceType);
            g_sniffers.push_back(sniffer);

            phy->TraceConnectWithoutContext(
                "MonitorSnifferRx",
                MakeCallback(&RssiSniffer::operator(), sniffer)
            );

            NS_LOG_UNCOND("Sniffer instalado: nó " << nodeId
                          << " / dev " << d << " (" << ifaceType << ")");
        }
    }
}

// ------------------------------------------------------------
// 6. Exporta CSV completo
// ------------------------------------------------------------
void ExportarRssiCsv(const std::string& filename)
{
    std::ofstream out(filename);
    if (!out.is_open()) {
        NS_LOG_UNCOND("Erro ao abrir " << filename);
        return;
    }

    out << "timestamp_s,node_id,device_index,interface_type,"
           "rssi_dbm,noise_dbm,snr_db,sender_node_id,packet_size\n";

    for (const auto& r : g_rssiHistory) {
        out << r.time << ","
            << r.nodeId << ","
            << r.deviceIndex << ","
            << r.ifaceType << ","
            << r.rssi << ","
            << r.noise << ","
            << r.snr << ","
            << r.senderNodeId << ","
            << r.packetSize << "\n";
    }
    out.close();
    NS_LOG_UNCOND("CSV exportado: " << filename
                  << " (" << g_rssiHistory.size() << " registros)");
}

// ------------------------------------------------------------
// 7. Libera a memória dos sniffers (chamar no fim da simulação)
// ------------------------------------------------------------
void LimparSniffers()
{
    for (auto* s : g_sniffers) delete s;
    g_sniffers.clear();
}

#endif // RSSI_H