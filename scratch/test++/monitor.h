#ifndef MONITOR_H
#define MONITOR_H

// ============================================================
// monitor.h - Sniffers de RSSI + pacotes IP para simulação 802.11ah
//   - Papel do nó: STA / AP / RELAY (relay = 2 devices: AP + STA)
//   - RSSI de cada nó em relação ao AP central
//   - Por pacote: fonteIP, destinoIP, delay, hops, tamanho, flowID
//   - Por fluxo/nó: vazão (throughput), perdas, delay médio, hops médio
// Uso: chamar InstalarSniffers() DEPOIS de instalar a pilha IP e
//      atribuir os endereços; ExportarTudo() ao final da simulação.
// ============================================================

#include "ns3/core-module.h"
#include "ns3/network-module.h"
#include "ns3/wifi-module.h"
#include "ns3/internet-module.h"
#include "ns3/sta-wifi-mac.h"
#include "ns3/ap-wifi-mac.h"
#include "ns3/wifi-mac-header.h"
#include "ns3/ipv4-l3-protocol.h"
#include "ns3/ipv4-header.h"
#include "ns3/udp-header.h"
#include "ns3/tcp-header.h"
#include "ns3/mac48-address.h"

#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <map>
#include <tuple>
#include <limits>

using namespace ns3;

// ------------------------------------------------------------
// 1. Papéis e estruturas de registro
// ------------------------------------------------------------
enum class NodeRole { STA, AP, RELAY, UNKNOWN };

inline std::string RoleToString(NodeRole r)
{
    switch (r) {
        case NodeRole::STA:   return "STA";
        case NodeRole::AP:    return "AP";
        case NodeRole::RELAY: return "RELAY";
        default:              return "UNKNOWN";
    }
}

struct RssiRecord {
    double      time;
    uint32_t    nodeId;        // quem recebeu
    uint32_t    deviceIndex;
    std::string role;          // STA / AP / RELAY
    std::string ifaceType;     // interface que recebeu (AP ou STA)
    double      rssi;
    double      noise;
    double      snr;
    uint32_t    senderNodeId;  // 0xFFFFFFFF = desconhecido (ex.: ACK/CTS)
    bool        fromCentralAp; // quadro veio do AP central?
    uint32_t    packetSize;
};

struct PacketRecord {
    double       time;
    uint32_t     nodeId;
    std::string  role;
    std::string  ifaceType;    // para RELAY mostra por qual interface passou
    std::string  event;        // TX | FWD | RX | DELIVER
    Ipv4Address  srcIp;
    Ipv4Address  dstIp;
    uint8_t      proto;
    uint16_t     srcPort;
    uint16_t     dstPort;
    uint32_t     flowId;
    uint32_t     packetSize;   // bytes (cabeçalho IP + payload)
    uint8_t      ttl;
    double       delay;        // só em DELIVER (-1 = n/d)
    int          hops;         // só em DELIVER (-1 = n/d)
};

struct FlowStats {
    Ipv4Address srcIp, dstIp;
    uint8_t     proto = 0;
    uint16_t    srcPort = 0, dstPort = 0;
    uint64_t    txPackets = 0, rxPackets = 0;
    uint64_t    txBytes = 0,   rxBytes = 0;
    double      firstTx = -1, lastRx = -1;
    double      delaySum = 0, hopsSum = 0;
};

struct NodeStats {
    uint64_t txPackets = 0, txBytes = 0;          // originados no nó
    uint64_t rxPackets = 0, rxBytes = 0;          // recebidos no L3 (inclui encaminhados)
    uint64_t fwdPackets = 0, fwdBytes = 0;        // encaminhados (relay/roteador)
    uint64_t deliveredPackets = 0, deliveredBytes = 0; // entregues à aplicação
};

struct PendingTx { double time; uint8_t ttl; };

// ------------------------------------------------------------
// 2. Variáveis globais
// ------------------------------------------------------------
typedef std::tuple<uint32_t, uint32_t, uint8_t, uint16_t, uint16_t> FlowKey; // src,dst,proto,sport,dport
typedef std::tuple<uint32_t, uint32_t, uint8_t, uint16_t>           PktKey;  // src,dst,proto,ipId

std::vector<RssiRecord>                    g_rssiHistory;
std::vector<PacketRecord>                  g_pktHistory;
std::map<uint32_t, NodeRole>               g_nodeRole;
std::map<Mac48Address, uint32_t>           g_macToNode;
std::map<uint32_t, uint32_t>               g_ipToNode;     // Ipv4Address::Get() -> nodeId
std::map<std::pair<uint32_t,uint32_t>, std::string> g_devIface; // (nodeId, ifIndex) -> "AP"/"STA"
std::map<uint32_t, std::pair<double,uint64_t>> g_rssiNodeAp;   // nodeId -> (soma, n) do link com o AP central
std::map<FlowKey, uint32_t>                g_flowIds;
std::map<uint32_t, FlowStats>              g_flows;
std::map<uint32_t, NodeStats>              g_nodeStats;
std::map<PktKey, PendingTx>                g_pending;

uint32_t g_centralApId     = 0;
bool     g_logPacketRecords = true;   // false = só estatísticas agregadas (usa menos memória)
bool     g_verbose          = false;

// ------------------------------------------------------------
// 3. Sniffer de RSSI (MonitorSnifferRx na PHY)
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
        // Identifica o transmissor pelo Addr2 do cabeçalho MAC
        uint32_t sender = 0xFFFFFFFF;
        if (packet) {
            Ptr<Packet> copy = packet->Copy();
            WifiMacHeader hdr;
            if (copy->GetSize() >= 10) {
                copy->PeekHeader(hdr);
                if (!(hdr.IsAck() || hdr.IsCts())) {
                    auto it = g_macToNode.find(hdr.GetAddr2());
                    if (it != g_macToNode.end()) sender = it->second;
                }
            }
        }

        RssiRecord rec;
        rec.time          = Simulator::Now().GetSeconds();
        rec.nodeId        = m_nodeId;
        rec.deviceIndex   = m_devIdx;
        rec.role          = RoleToString(g_nodeRole[m_nodeId]);
        rec.ifaceType     = m_iface;
        rec.rssi          = signalDbm;
        rec.noise         = noiseDbm;
        rec.snr           = signalDbm - noiseDbm;
        rec.senderNodeId  = sender;
        rec.fromCentralAp = (sender == g_centralApId);
        rec.packetSize    = packet ? packet->GetSize() : 0;
        g_rssiHistory.push_back(rec);

        // RSSI do enlace nó <-> AP central
        uint32_t key = 0xFFFFFFFF;
        if (m_nodeId == g_centralApId && sender != 0xFFFFFFFF) key = sender;   // AP ouvindo o nó
        else if (g_nodeRole[sender] == NodeRole::AP || g_nodeRole[sender] == NodeRole::RELAY)                      key = m_nodeId; // nó ouvindo o AP
        if (key != 0xFFFFFFFF) {
            g_rssiNodeAp[key].first  += signalDbm;
            g_rssiNodeAp[key].second += 1;
        }

        if (g_verbose)
            NS_LOG_UNCOND(rec.time << "s | Nó " << m_nodeId << " (" << rec.role << "/" << m_iface
                          << ") RSSI=" << signalDbm << " dBm de nó " << (int)sender);
    }

private:
    uint32_t    m_nodeId;
    uint32_t    m_devIdx;
    std::string m_iface;
};

// ------------------------------------------------------------
// 4. Sniffer de pacotes IP (Ipv4L3Protocol Tx / Rx / LocalDeliver)
// ------------------------------------------------------------
inline uint32_t NodeFromIp(Ipv4Address a)
{
    auto it = g_ipToNode.find(a.Get());
    return it == g_ipToNode.end() ? 0xFFFFFFFF : it->second;
}

inline void ExtrairPortas(const Ipv4Header& h, Ptr<const Packet> payload,
                          uint16_t& sp, uint16_t& dp)
{
    sp = dp = 0;
    if (h.GetFragmentOffset() != 0) return;
    Ptr<Packet> c = payload->Copy();
    if (h.GetProtocol() == 17 && c->GetSize() >= 8) {
        UdpHeader u; c->PeekHeader(u);
        sp = u.GetSourcePort(); dp = u.GetDestinationPort();
    } else if (h.GetProtocol() == 6 && c->GetSize() >= 20) {
        TcpHeader t; c->PeekHeader(t);
        sp = t.GetSourcePort(); dp = t.GetDestinationPort();
    }
}

inline uint32_t ObterFlowId(const Ipv4Header& h, uint16_t sp, uint16_t dp)
{
    FlowKey k(h.GetSource().Get(), h.GetDestination().Get(), h.GetProtocol(), sp, dp);
    auto it = g_flowIds.find(k);
    if (it != g_flowIds.end()) return it->second;

    uint32_t id = g_flowIds.size() + 1;
    g_flowIds[k] = id;
    FlowStats fs;
    fs.srcIp = h.GetSource(); fs.dstIp = h.GetDestination();
    fs.proto = h.GetProtocol(); fs.srcPort = sp; fs.dstPort = dp;
    g_flows[id] = fs;
    return id;
}

class PacketSniffer
{
public:
    explicit PacketSniffer(uint32_t nodeId) : m_nodeId(nodeId) {}

    // Pacote já com cabeçalho IP
    void OnTx(Ptr<const Packet> p, Ptr<Ipv4> ipv4, uint32_t iface) { WithHeader("TX", p, ipv4, iface); }
    void OnRx(Ptr<const Packet> p, Ptr<Ipv4> ipv4, uint32_t iface) { WithHeader("RX", p, ipv4, iface); }

    // Entrega local: cabeçalho vem separado
    void OnLocalDeliver(const Ipv4Header& h, Ptr<const Packet> p, uint32_t iface)
    {
        Process("DELIVER", h, p, iface, nullptr);
    }

private:
    void WithHeader(const std::string& evt, Ptr<const Packet> p, Ptr<Ipv4> ipv4, uint32_t iface)
    {
        Ptr<Packet> c = p->Copy();
        Ipv4Header h;
        c->RemoveHeader(h);
        Process(evt, h, c, iface, ipv4);
    }

    void Process(std::string evt, const Ipv4Header& h, Ptr<const Packet> payload,
                 uint32_t iface, Ptr<Ipv4> ipv4)
    {
        if (iface == 0) return; // loopback

        double   now  = Simulator::Now().GetSeconds();
        uint16_t sp, dp;
        ExtrairPortas(h, payload, sp, dp);
        uint32_t flowId = ObterFlowId(h, sp, dp);
        uint32_t size   = payload->GetSize() + h.GetSerializedSize();
        FlowStats& fs   = g_flows[flowId];
        NodeStats& ns   = g_nodeStats[m_nodeId];

        if (evt == "TX" && NodeFromIp(h.GetSource()) != m_nodeId) evt = "FWD";

        double delay = -1;
        int    hops  = -1;

        if (evt == "TX") {
            ns.txPackets++; ns.txBytes += size;
            fs.txPackets++; fs.txBytes += size;
            if (fs.firstTx < 0) fs.firstTx = now;
            PktKey k(h.GetSource().Get(), h.GetDestination().Get(), h.GetProtocol(), h.GetIdentification());
            g_pending[k] = PendingTx{now, h.GetTtl()};
        }
        else if (evt == "FWD") {
            ns.fwdPackets++; ns.fwdBytes += size;
        }
        else if (evt == "RX") {
            ns.rxPackets++; ns.rxBytes += size;
        }
        else if (evt == "DELIVER") {
            ns.deliveredPackets++; ns.deliveredBytes += size;
            PktKey k(h.GetSource().Get(), h.GetDestination().Get(), h.GetProtocol(), h.GetIdentification());
            auto it = g_pending.find(k);
            if (it != g_pending.end()) {
                delay = now - it->second.time;
                hops  = (int)it->second.ttl - (int)h.GetTtl() + 1; // nº de enlaces percorridos
                g_pending.erase(it);
                fs.rxPackets++; fs.rxBytes += size;
                fs.delaySum += delay; fs.hopsSum += hops;
                fs.lastRx = now;
            }
        }

        if (g_logPacketRecords) {
            PacketRecord r;
            r.time = now; r.nodeId = m_nodeId;
            r.role = RoleToString(g_nodeRole[m_nodeId]);
            r.ifaceType = "OTHER";
            if (ipv4) {
                Ptr<NetDevice> dev = ipv4->GetNetDevice(iface);
                if (dev) {
                    auto it = g_devIface.find({m_nodeId, dev->GetIfIndex()});
                    if (it != g_devIface.end()) r.ifaceType = it->second;
                }
            }
            r.event = evt; r.srcIp = h.GetSource(); r.dstIp = h.GetDestination();
            r.proto = h.GetProtocol(); r.srcPort = sp; r.dstPort = dp;
            r.flowId = flowId; r.packetSize = size; r.ttl = h.GetTtl();
            r.delay = delay; r.hops = hops;
            g_pktHistory.push_back(r);
        }

        if (g_verbose)
            NS_LOG_UNCOND(now << "s | Nó " << m_nodeId << " " << evt << " flow " << flowId
                          << " " << h.GetSource() << "->" << h.GetDestination()
                          << " " << size << "B");
    }

    uint32_t m_nodeId;
};

// ------------------------------------------------------------
// 5. Guarda os sniffers vivos durante a simulação
// ------------------------------------------------------------
std::vector<RssiSniffer*>   g_rssiSniffers;
std::vector<PacketSniffer*> g_pktSniffers;

// ------------------------------------------------------------
// 6. Detecta papéis (STA / AP / RELAY) e monta tabelas MAC/IP
// ------------------------------------------------------------
void DetectarPapeis(NodeContainer nodes)
{
    for (uint32_t i = 0; i < nodes.GetN(); ++i) {
        Ptr<Node> node = nodes.Get(i);
        uint32_t id = node->GetId();
        bool hasAp = false, hasSta = false;

        for (uint32_t d = 0; d < node->GetNDevices(); ++d) {
            Ptr<WifiNetDevice> w = DynamicCast<WifiNetDevice>(node->GetDevice(d));
            if (!w) continue;
            Ptr<WifiMac> mac = w->GetMac();
            std::string t = "UNKNOWN";
            if (DynamicCast<StaWifiMac>(mac))     { hasSta = true; t = "STA"; }
            else if (DynamicCast<ApWifiMac>(mac)) { hasAp  = true; t = "AP";  }
            g_devIface[{id, w->GetIfIndex()}] = t;
            g_macToNode[Mac48Address::ConvertFrom(w->GetAddress())] = id;
        }

        if (hasAp && hasSta) g_nodeRole[id] = NodeRole::RELAY;
        else if (hasAp)      g_nodeRole[id] = NodeRole::AP;
        else if (hasSta)     g_nodeRole[id] = NodeRole::STA;
        else                 g_nodeRole[id] = NodeRole::UNKNOWN;

        Ptr<Ipv4> ipv4 = node->GetObject<Ipv4>();
        if (ipv4) {
            for (uint32_t k = 1; k < ipv4->GetNInterfaces(); ++k)
                for (uint32_t a = 0; a < ipv4->GetNAddresses(k); ++a)
                    g_ipToNode[ipv4->GetAddress(k, a).GetLocal().Get()] = id;
        }

        NS_LOG_UNCOND("Nó " << id << " -> " << RoleToString(g_nodeRole[id]));
    }
}

// ------------------------------------------------------------
// 7. Instala todos os sniffers
// ------------------------------------------------------------
void InstalarSniffers(NodeContainer nodes, uint32_t centralApId = 0)
{
    g_centralApId = centralApId;
    DetectarPapeis(nodes);

    for (uint32_t i = 0; i < nodes.GetN(); ++i) {
        Ptr<Node> node = nodes.Get(i);
        uint32_t nodeId = node->GetId();

        // --- RSSI: um sniffer por device Wi-Fi (relay terá dois) ---
        for (uint32_t d = 0; d < node->GetNDevices(); ++d) {
            Ptr<WifiNetDevice> w = DynamicCast<WifiNetDevice>(node->GetDevice(d));
            if (!w || !w->GetPhy()) continue;

            std::string t = g_devIface[{nodeId, w->GetIfIndex()}];
            RssiSniffer* s = new RssiSniffer(nodeId, d, t);
            g_rssiSniffers.push_back(s);
            w->GetPhy()->TraceConnectWithoutContext(
                "MonitorSnifferRx", MakeCallback(&RssiSniffer::operator(), s));
        }

        // --- Pacotes IP: um sniffer por nó ---
        Ptr<Ipv4L3Protocol> l3 = node->GetObject<Ipv4L3Protocol>();
        if (!l3) continue;
        PacketSniffer* ps = new PacketSniffer(nodeId);
        g_pktSniffers.push_back(ps);
        l3->TraceConnectWithoutContext("Tx",           MakeCallback(&PacketSniffer::OnTx, ps));
        l3->TraceConnectWithoutContext("Rx",           MakeCallback(&PacketSniffer::OnRx, ps));
        l3->TraceConnectWithoutContext("LocalDeliver", MakeCallback(&PacketSniffer::OnLocalDeliver, ps));
    }
}

// ------------------------------------------------------------
// 8. Exportação de CSVs
// ------------------------------------------------------------
void ExportarRssiCsv(const std::string& filename)
{
    std::ofstream out(filename);
    out << "timestamp_s,node_id,role,device_index,interface_type,rssi_dbm,noise_dbm,"
           "snr_db,sender_node_id,from_central_ap,packet_size\n";
    for (const auto& r : g_rssiHistory)
        out << r.time << "," << r.nodeId << "," << r.role << "," << r.deviceIndex << ","
            << r.ifaceType << "," << r.rssi << "," << r.noise << "," << r.snr << ","
            << (int)r.senderNodeId << "," << r.fromCentralAp << "," << r.packetSize << "\n";
}

void ExportarRssiResumoCsv(const std::string& filename)
{
    std::ofstream out(filename);
    out << "node_id,role,rssi_medio_ate_ap_dbm,amostras\n";
    for (const auto& kv : g_rssiNodeAp)
        out << kv.first << "," << RoleToString(g_nodeRole[kv.first]) << ","
            << kv.second.first / kv.second.second << "," << kv.second.second << "\n";
}

void ExportarPacotesCsv(const std::string& filename)
{
    std::ofstream out(filename);
    out << "timestamp_s,node_id,role,interface_type,event,src_ip,dst_ip,proto,src_port,"
           "dst_port,flow_id,packet_size,ttl,delay_s,hops\n";
    for (const auto& r : g_pktHistory) {
        out << r.time << "," << r.nodeId << "," << r.role << "," << r.ifaceType << ","
            << r.event << "," << r.srcIp << "," << r.dstIp << "," << (int)r.proto << ","
            << r.srcPort << "," << r.dstPort << "," << r.flowId << "," << r.packetSize << ","
            << (int)r.ttl << ",";
        if (r.delay >= 0) out << r.delay; out << ",";
        if (r.hops  >= 0) out << r.hops;  out << "\n";
    }
}

void ExportarFluxosCsv(const std::string& filename)
{
    std::ofstream out(filename);
    out << "flow_id,src_ip,dst_ip,src_node,dst_node,proto,src_port,dst_port,tx_packets,"
           "rx_packets,tx_bytes,rx_bytes,loss_pct,mean_delay_s,mean_hops,throughput_kbps\n";
    for (const auto& kv : g_flows) {
        const FlowStats& f = kv.second;
        double loss  = f.txPackets ? 100.0 * (f.txPackets - f.rxPackets) / f.txPackets : 0;
        double mdel  = f.rxPackets ? f.delaySum / f.rxPackets : 0;
        double mhops = f.rxPackets ? f.hopsSum  / f.rxPackets : 0;
        double dur   = (f.lastRx > f.firstTx && f.firstTx >= 0) ? f.lastRx - f.firstTx : 0;
        double thr   = dur > 0 ? f.rxBytes * 8.0 / dur / 1000.0 : 0;
        out << kv.first << "," << f.srcIp << "," << f.dstIp << ","
            << (int)NodeFromIp(f.srcIp) << "," << (int)NodeFromIp(f.dstIp) << ","
            << (int)f.proto << "," << f.srcPort << "," << f.dstPort << ","
            << f.txPackets << "," << f.rxPackets << "," << f.txBytes << "," << f.rxBytes << ","
            << loss << "," << mdel << "," << mhops << "," << thr << "\n";
    }
}

void ExportarNosCsv(const std::string& filename, double simTime)
{
    std::ofstream out(filename);
    out << "node_id,role,tx_packets,tx_bytes,rx_packets,rx_bytes,fwd_packets,fwd_bytes,"
           "delivered_packets,delivered_bytes,throughput_kbps,rssi_medio_ate_ap_dbm\n";
    for (const auto& kv : g_nodeRole) {
        if (kv.first == 0xFFFFFFFFu || kv.first == static_cast<uint32_t>(-1))
        continue;
        
        const NodeStats& s = g_nodeStats[kv.first];
        auto r = g_rssiNodeAp.find(kv.first);
        out << kv.first << "," << RoleToString(kv.second) << ","
            << s.txPackets << "," << s.txBytes << "," << s.rxPackets << "," << s.rxBytes << ","
            << s.fwdPackets << "," << s.fwdBytes << ","
            << s.deliveredPackets << "," << s.deliveredBytes << ","
            << (simTime > 0 ? s.deliveredBytes * 8.0 / simTime / 1000.0 : 0) << ",";
        if (r != g_rssiNodeAp.end()) out << r->second.first / r->second.second;
        out << "\n";
    }
}

void ExportarTudo(const std::string& prefixo, double simTime)
{
    ExportarRssiCsv(prefixo + "_rssi.csv");
    ExportarRssiResumoCsv(prefixo + "_rssi_resumo.csv");
    ExportarPacotesCsv(prefixo + "_pacotes.csv");
    ExportarFluxosCsv(prefixo + "_fluxos.csv");
    ExportarNosCsv(prefixo + "_nos.csv", simTime);
    NS_LOG_UNCOND("CSVs exportados com prefixo '" << prefixo << "' ("
                  << g_rssiHistory.size() << " RSSI, " << g_pktHistory.size() << " pacotes, "
                  << g_flows.size() << " fluxos)");
}

// ------------------------------------------------------------
// 9. Libera memória (chamar no fim da simulação)
// ------------------------------------------------------------
void LimparSniffers()
{
    for (auto* s : g_rssiSniffers) delete s;
    for (auto* s : g_pktSniffers)  delete s;
    g_rssiSniffers.clear();
    g_pktSniffers.clear();
}

#endif // MONITOR_H