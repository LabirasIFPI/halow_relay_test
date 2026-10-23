#ifndef MONITOR_ENLACE_H
#define MONITOR_ENLACE_H

#include "base.h"

/**
 * @brief Contagem de quadros e sinal de um enlace filho -> pai.
 *
 
 *  - ulTx/ulRx: quadros do filho enviados / decodificados no pai.
 *  - dlTx/dlRx: quadros do pai enviados / decodificados no filho.
 *  - ul: sinal no pai, de quadros do filho.
 *  - dl: sinal no filho, de quadros do pai (inclui beacons).
 */
struct LinkFrameStats {
    uint64_t ulTx = 0, ulRx = 0;
    uint64_t dlTx = 0, dlRx = 0;
    RadioDirectionStats ul;
    RadioDirectionStats dl;
};
typedef std::pair<uint32_t, uint32_t> LinkKey;
std::map<LinkKey, LinkFrameStats> g_linkFrames;

std::map<uint32_t, RadioDirectionStats> g_apRxFromNode;
std::map<uint32_t, RadioDirectionStats> g_nodeRxFromAp;

/**
 * @brief Registra o sinal de cada quadro recebido (trace MonitorSnifferRx do PHY).
 *
 * 
 */
class RssiSniffer
{
public:
    RssiSniffer(uint32_t nodeId, std::string iface)
        : m_nodeId(nodeId), m_iface(iface) {}

    void operator()(Ptr<const Packet> packet,
                    uint16_t channelFreqMhz,
                    uint16_t channelNumber,
                    uint32_t rate,
                    bool isShortPreamble,
                    WifiTxVector txVector,
                    double signalDbm,
                    double noiseDbm) const
    {
        uint32_t sender = 0xFFFFFFFF;
        uint32_t receiver = 0xFFFFFFFF;
        Mac48Address addr1, addr2;
        if (packet) {
            Ptr<Packet> copy = packet->Copy();
            WifiMacHeader hdr;
            if (copy->GetSize() >= 10) {
                copy->PeekHeader(hdr);
                if (!(hdr.IsAck() || hdr.IsCts())) {
                    addr1 = hdr.GetAddr1();
                    addr2 = hdr.GetAddr2();
                    auto it = g_macToNode.find(addr2);
                    if (it != g_macToNode.end()) sender = it->second;
                    auto rt = g_macToNode.find(addr1);
                    if (rt != g_macToNode.end()) receiver = rt->second;
                }
            }
        }
        const double snr = signalDbm - noiseDbm;

        if (sender != 0xFFFFFFFFu) {
            const bool txIsAp = MacEhAp(addr2);
            if (receiver == m_nodeId) {
                const bool rxIsAp = MacEhAp(addr1);
                if (rxIsAp && !txIsAp) {
                    LinkFrameStats& l = g_linkFrames[LinkKey(sender, m_nodeId)];
                    l.ulRx++;
                    AddRadioDirectionSample(l.ul, signalDbm, noiseDbm, snr);
                } else if (!rxIsAp && txIsAp) {
                    LinkFrameStats& l = g_linkFrames[LinkKey(m_nodeId, sender)];
                    l.dlRx++;
                    AddRadioDirectionSample(l.dl, signalDbm, noiseDbm, snr);
                }
            } else if (addr1.IsGroup() && txIsAp && sender == PaiDe(m_nodeId)) {
                LinkFrameStats& l = g_linkFrames[LinkKey(m_nodeId, sender)];
                AddRadioDirectionSample(l.dl, signalDbm, noiseDbm, snr);
            }
        }

        if (m_nodeId == g_centralApId && sender != 0xFFFFFFFFu && sender != g_centralApId)
            AddRadioDirectionSample(g_apRxFromNode[sender], signalDbm, noiseDbm, snr);
        else if (sender == g_centralApId && m_nodeId != g_centralApId)
            AddRadioDirectionSample(g_nodeRxFromAp[m_nodeId], signalDbm, noiseDbm, snr);

        if (g_verbose)
            NS_LOG_UNCOND(Simulator::Now().GetSeconds() << "s | Nó " << m_nodeId << " ("
                          << RoleToString(g_nodeRole[m_nodeId]) << "/" << m_iface
                          << ") RSSI=" << signalDbm << " dBm SNR=" << snr
                          << " dB de nó " << (int)sender);
    }

private:
    uint32_t    m_nodeId;
    std::string m_iface;
};

/**
 * @brief Conta os quadros unicast transmitidos nos enlaces filho <-> pai (trace PhyTxBegin).
 *
 * Broadcast (beacons), ACK/CTS e destinos desconhecidos são ignorados.
 */
class FrameTxSniffer
{
public:
    FrameTxSniffer(uint32_t nodeId, Mac48Address mac) : m_nodeId(nodeId), m_mac(mac) {}

    void operator()(Ptr<const Packet> packet) const
    {
        if (!packet || packet->GetSize() < 10) return;
        Ptr<Packet> copy = packet->Copy();
        WifiMacHeader hdr;
        copy->PeekHeader(hdr);
        if (hdr.IsAck() || hdr.IsCts()) return;
        auto rt = g_macToNode.find(hdr.GetAddr1());
        if (rt == g_macToNode.end()) return;

        const bool txIsAp = MacEhAp(m_mac), rxIsAp = MacEhAp(hdr.GetAddr1());
        if (!txIsAp && rxIsAp)      g_linkFrames[LinkKey(m_nodeId, rt->second)].ulTx++;
        else if (txIsAp && !rxIsAp) g_linkFrames[LinkKey(rt->second, m_nodeId)].dlTx++;
    }

private:
    uint32_t m_nodeId;
    Mac48Address m_mac;
};

/**
 * @brief Grava o PDR e o SNR de cada enlace filho -> pai (link_frame_summary.csv).
 *
 * É a entrada do calibrar_lqi.py. Uma linha por enlace (STA -> gateway,
 * STA -> relay, relay -> gateway), com o PDR em quadros MAC e o SNR de uplink
 * na mesma escala que o PHY usa para decidir a recepção.
 *  
 * @param filename Caminho do CSV.
 */
void ExportarResumoEnlaceQuadrosCsv(const std::string& filename)
{
    std::ofstream out(filename.c_str());
    out << "node_id,parent_node_id,parent_role,distance_to_parent_m,distance_to_central_ap_m,"
           "ul_frames_tx,ul_frames_rx_at_ap,ul_frame_pdr,"
           "dl_frames_tx,dl_frames_rx_at_node,dl_frame_pdr,"
           "ul_rssi_measured_dbm,ul_rssi_reciprocity_dbm,ul_snr_phy_db,ul_snr_source\n";

    for (const auto& role : g_nodeRole) {
        const uint32_t id = role.first;
        if (role.second != NodeRole::STA && role.second != NodeRole::RELAY) continue;
        const uint32_t pai = PaiDe(id);
        Ptr<YansWifiPhy> apPhy = g_apPhyDoNo[pai];
        Ptr<YansWifiPhy> staPhy = g_staPhyDoNo[id];
        if (!apPhy || !staPhy) continue;
        const double paiNoiseFloorDbm = -174.0 + 10.0 * std::log10(apPhy->GetChannelWidth() * 1e6)
                                        + apPhy->GetRxNoiseFigure();
        const LinkFrameStats& f = g_linkFrames[LinkKey(id, pai)];

        out << id << "," << pai << "," << RoleToString(g_nodeRole[pai]) << ","
            << DistanciaEntreNos(id, pai) << "," << DistanciaAteAp(id) << ","
            << f.ulTx << "," << f.ulRx << ",";
        if (f.ulTx) out << static_cast<double>(f.ulRx) / f.ulTx;
        out << "," << f.dlTx << "," << f.dlRx << ",";
        if (f.dlTx) out << static_cast<double>(f.dlRx) / f.dlTx;
        out << ",";

        const bool hasMeasured = f.ul.n > 0;
        const bool hasRecip = f.dl.n > 0;
        const double ulRssiMeasured = hasMeasured ? f.ul.rssiSum / f.ul.n : 0.0;
        double ulRssiRecip = 0.0;
        if (hasRecip) {
            const double pathLossDb = apPhy->GetTxPowerStart() + apPhy->GetTxGain() + staPhy->GetRxGain()
                                      - f.dl.rssiSum / f.dl.n;
            ulRssiRecip = staPhy->GetTxPowerStart() + staPhy->GetTxGain() + apPhy->GetRxGain() - pathLossDb;
        }

        if (hasMeasured) out << ulRssiMeasured;
        out << ",";
        if (hasRecip) out << ulRssiRecip;
        out << ",";
        if (hasMeasured)   out << ulRssiMeasured - paiNoiseFloorDbm << ",measured";
        else if (hasRecip) out << ulRssiRecip - paiNoiseFloorDbm << ",reciprocity";
        else               out << ",";
        out << "\n";
    }
}

#endif // MONITOR_ENLACE_H
