#ifndef MONITOR_H
#define MONITOR_H

// ============================================================
// monitor.h - Sniffers de RSSI/SNR + pacotes IP para simulação 802.11ah
//   - Papel do nó: STA / AP / RELAY (relay = 2 devices: AP + STA)
//   - RSSI e SNR por enlace nó <-> AP central (uplink e downlink separados)
//   - Por pacote: fonteIP, destinoIP, delay, hops, tamanho, flowID
//   - Por fluxo/nó: vazão (goodput), perdas, PDR, delay médio, hops médio
//   - RAW de cada nó (AID, RPS, grupo e slot) no resumo por nó
//   - Energia: estimativa simplificada em camada IP (não é medição de PHY)
// Uso: chamar InstalarSniffers() DEPOIS de instalar a pilha IP e
//      atribuir os endereços; ExportarTudo() ao final da simulação.
//      Chamar RegistrarRawDoNo() quando cada STA associar (ver onSTAAssociated).
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
#include "ns3/mobility-module.h"

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
    double       energyJ;      // estimativa por evento IP (J); -1 = n/d
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
    uint64_t deliveredPackets = 0, deliveredBytes = 0; // entregues à pilha local
};

struct PendingTx { double time; uint8_t ttl; };

// RAW a que o nó pertence (preenchido pelo código principal ao associar).
// Índices em base 1, como no NodeEntry. valid=false => nó sem RAW registrada.
struct NodeRawInfo {
    bool     valid    = false;
    uint32_t aid      = 0;
    int      rpsIndex = -1;
    int      rawGroup = -1;
    int      rawSlot  = -1;
};

// Estatísticas direcionais: RSSI é medido no receptor, por isso uplink e
// downlink não devem ser misturados na mesma média.
struct RadioDirectionStats {
    uint64_t n = 0;
    double rssiSum = 0.0, noiseSum = 0.0, snrSum = 0.0;
    double rssiMin = std::numeric_limits<double>::infinity();
    double rssiMax = -std::numeric_limits<double>::infinity();
};

inline void AddRadioDirectionSample(RadioDirectionStats& s, double rssi,
                                    double noise, double snr)
{
    ++s.n;
    s.rssiSum += rssi; s.noiseSum += noise; s.snrSum += snr;
    if (rssi < s.rssiMin) s.rssiMin = rssi;
    if (rssi > s.rssiMax) s.rssiMax = rssi;
}

// ------------------------------------------------------------
// 2. Variáveis globais
// ------------------------------------------------------------
typedef std::tuple<uint32_t, uint32_t, uint8_t, uint16_t, uint16_t> FlowKey; // src,dst,proto,sport,dport
typedef std::tuple<uint32_t, uint32_t, uint8_t, uint16_t>           PktKey;  // src,dst,proto,ipId

uint32_t g_centralApId = 0;

std::vector<RssiRecord>                    g_rssiHistory;
std::vector<PacketRecord>                  g_pktHistory;
std::map<uint32_t, NodeRole>               g_nodeRole;
std::map<Mac48Address, uint32_t>           g_macToNode;
std::map<uint32_t, uint32_t>               g_ipToNode;     // Ipv4Address::Get() -> nodeId
std::map<std::pair<uint32_t,uint32_t>, std::string> g_devIface; // (nodeId, ifIndex) -> "AP"/"STA"
std::map<uint32_t, RadioDirectionStats>    g_apRxFromNode; // uplink: AP central recebe do nó
std::map<uint32_t, RadioDirectionStats>    g_nodeRxFromAp; // downlink: nó recebe do AP central
std::map<uint32_t, double>                 g_energyTxJ;    // energia TX estimada por nó
std::map<uint32_t, double>                 g_energyRxJ;    // energia RX estimada por nó
std::map<FlowKey, uint32_t>                g_flowIds;
std::map<uint32_t, FlowStats>              g_flows;
std::map<uint32_t, NodeStats>              g_nodeStats;
std::map<PktKey, PendingTx>                g_pending;
std::map<uint32_t, NodeRawInfo>            g_nodeRaw;      // nodeId -> RAW (AID/RPS/grupo/slot)

bool g_logPacketRecords = true;   // false = só estatísticas agregadas (usa menos memória)
bool g_verbose          = false;

// Registra a RAW de um nó. Chamar em onSTAAssociated (índices em base 1).
// Se o nó reassociar, o último registro prevalece.
inline void RegistrarRawDoNo(uint32_t nodeId, uint32_t aid,
                             int rpsIndex, int rawGroup, int rawSlot)
{
    NodeRawInfo info;
    info.valid    = true;
    info.aid      = aid;
    info.rpsIndex = rpsIndex;
    info.rawGroup = rawGroup;
    info.rawSlot  = rawSlot;
    g_nodeRaw[nodeId] = info;
}

// MODELO SIMPLIFICADO, configurável. Não substitui o ns-3 Energy Framework.
// Potência média assumida do rádio e taxa nominal usados para estimar airtime.
double g_monitorTxPowerW       = 0.8;
double g_monitorRxPowerW       = 0.5;
double g_monitorNominalRateBps = 150000.0; // ajuste para a taxa do cenário

inline double EstimarEnergiaJ(uint32_t bytes, bool tx)
{
    double rate = g_monitorNominalRateBps > 0 ? g_monitorNominalRateBps : 150000.0;
    double airtime = (static_cast<double>(bytes) * 8.0) / rate;
    return airtime * (tx ? g_monitorTxPowerW : g_monitorRxPowerW);
}

inline double DistanciaAteAp(uint32_t nodeId)
{
    Ptr<Node> node = NodeList::GetNode(nodeId);
    Ptr<Node> ap   = NodeList::GetNode(g_centralApId);
    if (!node || !ap) return -1.0;
    Ptr<MobilityModel> nm = node->GetObject<MobilityModel>();
    Ptr<MobilityModel> am = ap->GetObject<MobilityModel>();
    if (!nm || !am) return -1.0;
    return nm->GetDistanceFrom(am);
}

// ------------------------------------------------------------
// 3. Sniffer de RSSI/SNR (MonitorSnifferRx na PHY)
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

        // Enlace DIRETO com o AP central; uplink e downlink separados.
        if (m_nodeId == g_centralApId && sender != 0xFFFFFFFFu && sender != g_centralApId)
            AddRadioDirectionSample(g_apRxFromNode[sender], signalDbm, noiseDbm, rec.snr);
        else if (sender == g_centralApId && m_nodeId != g_centralApId)
            AddRadioDirectionSample(g_nodeRxFromAp[m_nodeId], signalDbm, noiseDbm, rec.snr);

        if (g_verbose)
            NS_LOG_UNCOND(rec.time << "s | Nó " << m_nodeId << " (" << rec.role << "/" << m_iface
                          << ") RSSI=" << signalDbm << " dBm SNR=" << rec.snr
                          << " dB de nó " << (int)sender);
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

        double   now = Simulator::Now().GetSeconds();
        uint16_t sp, dp;
        ExtrairPortas(h, payload, sp, dp);
        uint32_t flowId = ObterFlowId(h, sp, dp);
        uint32_t size   = payload->GetSize() + h.GetSerializedSize();
        FlowStats& fs   = g_flows[flowId];
        NodeStats& ns   = g_nodeStats[m_nodeId];

        if (evt == "TX" && NodeFromIp(h.GetSource()) != m_nodeId) evt = "FWD";

        double delay   = -1;
        double energyJ = -1.0; // -1 = não estimado neste evento (ex.: DELIVER)
        int    hops    = -1;

        if (evt == "TX") {
            ns.txPackets++; ns.txBytes += size;
            energyJ = EstimarEnergiaJ(size, true);
            g_energyTxJ[m_nodeId] += energyJ;
            fs.txPackets++; fs.txBytes += size;
            if (fs.firstTx < 0) fs.firstTx = now;
            PktKey k(h.GetSource().Get(), h.GetDestination().Get(), h.GetProtocol(), h.GetIdentification());
            g_pending[k] = PendingTx{now, h.GetTtl()};
        }
        else if (evt == "FWD") {
            ns.fwdPackets++; ns.fwdBytes += size;
            energyJ = EstimarEnergiaJ(size, true);
            g_energyTxJ[m_nodeId] += energyJ;
        }
        else if (evt == "RX") {
            ns.rxPackets++; ns.rxBytes += size;
            energyJ = EstimarEnergiaJ(size, false);
            g_energyRxJ[m_nodeId] += energyJ;
        }
        else if (evt == "DELIVER") {
            ns.deliveredPackets++; ns.deliveredBytes += size;
            PktKey k(h.GetSource().Get(), h.GetDestination().Get(), h.GetProtocol(), h.GetIdentification());
            auto it = g_pending.find(k);
            if (it != g_pending.end()) {
                delay = now - it->second.time;
                // TTL só permite estimar saltos IP/roteamento; não conta relays L2.
                hops  = (int)it->second.ttl - (int)h.GetTtl();
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
            r.delay = delay; r.energyJ = energyJ; r.hops = hops;
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

        // --- RSSI/SNR: um sniffer por device Wi-Fi (relay terá dois) ---
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
// 8. Exportação dos CSVs
// ------------------------------------------------------------
void ExportarAmostrasRadioCsv(const std::string& filename)
{
    std::ofstream out(filename.c_str());
    out << "time_s,receiver_node_id,receiver_role,receiver_device_index,receiver_interface,"
           "sender_node_id,sender_is_central_ap,rssi_dbm,noise_dbm,snr_db,frame_size_bytes\n";
    for (const auto& r : g_rssiHistory) {
        out << r.time << "," << r.nodeId << "," << r.role << "," << r.deviceIndex << ","
            << r.ifaceType << ",";
        if (r.senderNodeId != 0xFFFFFFFFu) out << r.senderNodeId;
        out << "," << (r.fromCentralAp ? 1 : 0) << "," << r.rssi << "," << r.noise << ","
            << r.snr << "," << r.packetSize << "\n";
    }
}

static void EscreverStatsDirecao(std::ofstream& out, const RadioDirectionStats& s)
{
    // 6 campos: n, rssi_mean, rssi_min, rssi_max, noise_mean, snr_mean
    if (s.n)
        out << s.n << "," << s.rssiSum / s.n << "," << s.rssiMin << "," << s.rssiMax << ","
            << s.noiseSum / s.n << "," << s.snrSum / s.n;
    else
        out << ",,,,,";
}

void ExportarResumoEnlaceApCsv(const std::string& filename)
{
    std::ofstream out(filename.c_str());
    out << "node_id,role,distance_to_central_ap_m,"
           "ap_rx_samples_from_node,ap_rx_rssi_mean_dbm,ap_rx_rssi_min_dbm,ap_rx_rssi_max_dbm,"
           "ap_rx_noise_mean_dbm,ap_rx_snr_mean_db,"
           "node_rx_samples_from_ap,node_rx_rssi_mean_dbm,node_rx_rssi_min_dbm,node_rx_rssi_max_dbm,"
           "node_rx_noise_mean_dbm,node_rx_snr_mean_db\n";
    for (const auto& role : g_nodeRole) {
        const uint32_t id = role.first;
        if (id == g_centralApId) continue;
        out << id << "," << RoleToString(role.second) << "," << DistanciaAteAp(id) << ",";
        auto up = g_apRxFromNode.find(id);
        EscreverStatsDirecao(out, up != g_apRxFromNode.end() ? up->second : RadioDirectionStats());
        out << ",";
        auto down = g_nodeRxFromAp.find(id);
        EscreverStatsDirecao(out, down != g_nodeRxFromAp.end() ? down->second : RadioDirectionStats());
        out << "\n";
    }
}

void ExportarEventosIpCsv(const std::string& filename)
{
    std::ofstream out(filename.c_str());
    out << "time_s,node_id,node_role,interface_type,event,source_ip,destination_ip,ip_protocol,"
           "source_port,destination_port,flow_id,ip_packet_size_bytes,ttl,one_way_delay_s,"
           "ip_router_hops_from_ttl_estimate,ip_layer_energy_estimate_j\n";
    for (const auto& r : g_pktHistory) {
        out << r.time << "," << r.nodeId << "," << r.role << "," << r.ifaceType << ","
            << r.event << "," << r.srcIp << "," << r.dstIp << "," << static_cast<unsigned>(r.proto) << ","
            << r.srcPort << "," << r.dstPort << "," << r.flowId << "," << r.packetSize << ","
            << static_cast<unsigned>(r.ttl) << ",";
        if (r.delay >= 0) out << r.delay;
        out << ",";
        if (r.hops >= 0) out << r.hops;
        out << ",";
        if (r.energyJ >= 0.0) out << r.energyJ;
        out << "\n";
    }
}

void ExportarResumoFluxosCsv(const std::string& filename)
{
    std::ofstream out(filename.c_str());
    out << "flow_id,source_ip,destination_ip,source_node_id,destination_node_id,ip_protocol,"
           "source_port,destination_port,origin_tx_packets,destination_delivered_packets,"
           "origin_tx_bytes,destination_delivered_bytes,estimated_loss_pct,pdr_ratio,"
           "mean_one_way_delay_s,mean_ip_router_hops_from_ttl,observed_duration_s,"
           "goodput_kbit_s,first_tx_s,last_delivery_s\n";
    for (const auto& kv : g_flows) {
        const FlowStats& f = kv.second;
        // PDR = pacotes entregues / pacotes originados, limitado a 1.0 para que
        // duplicatas/inconsistências de contagem nunca gerem PDR > 1.
        const uint64_t delivered = f.rxPackets > f.txPackets ? f.txPackets : f.rxPackets;
        const double pdr   = f.txPackets ? static_cast<double>(delivered) / f.txPackets : -1.0;
        const double loss  = pdr >= 0.0 ? 100.0 * (1.0 - pdr) : -1.0;
        const double mdel  = f.rxPackets ? f.delaySum / f.rxPackets : -1.0;
        const double mhops = f.rxPackets ? f.hopsSum / f.rxPackets : -1.0;
        const double dur   = (f.rxPackets && f.lastRx > f.firstTx && f.firstTx >= 0) ? f.lastRx - f.firstTx : -1.0;
        const double goodput = dur > 0 ? f.rxBytes * 8.0 / dur / 1000.0 : -1.0;
        out << kv.first << "," << f.srcIp << "," << f.dstIp << ","
            << NodeFromIp(f.srcIp) << "," << NodeFromIp(f.dstIp) << ","
            << static_cast<unsigned>(f.proto) << "," << f.srcPort << "," << f.dstPort << ","
            << f.txPackets << "," << f.rxPackets << "," << f.txBytes << "," << f.rxBytes << ",";
        if (loss >= 0)    out << loss;
        out << ","; if (pdr >= 0)     out << pdr;
        out << ","; if (mdel >= 0)    out << mdel;
        out << ","; if (mhops >= 0)   out << mhops;
        out << ","; if (dur >= 0)     out << dur;
        out << ","; if (goodput >= 0) out << goodput;
        out << ","; if (f.firstTx >= 0) out << f.firstTx;
        out << ","; if (f.rxPackets && f.lastRx >= 0) out << f.lastRx;
        out << "\n";
    }
}

void ExportarResumoNosCsv(const std::string& filename, double simTime)
{
    std::ofstream out(filename.c_str());
    out << "node_id,node_role,distance_to_central_ap_m,aid,raw_rps_index,raw_group,raw_slot,"
           "ip_originated_tx_packets,ip_originated_tx_bytes,"
           "ip_rx_packets,ip_rx_bytes,ip_forwarded_packets,ip_forwarded_bytes,"
           "ip_delivered_to_local_stack_packets,ip_delivered_to_local_stack_bytes,delivery_rate_kbit_s,"
           "ap_rx_samples_from_node,ap_rx_rssi_mean_dbm,ap_rx_snr_mean_db,"
           "node_rx_samples_from_ap,node_rx_rssi_mean_dbm,node_rx_snr_mean_db,"
           "estimated_ip_tx_energy_j,estimated_ip_rx_energy_j,estimated_ip_total_energy_j,"
           "estimated_tx_energy_per_originated_or_forwarded_packet_j\n";
    for (const auto& kv : g_nodeRole) {
        const uint32_t id = kv.first;
        const NodeStats& s = g_nodeStats[id];
        const double txJ = g_energyTxJ[id], rxJ = g_energyRxJ[id];
        out << id << "," << RoleToString(kv.second) << "," << DistanciaAteAp(id) << ",";

        // RAW do nó: vazio para o AP ou para nós que nunca associaram
        auto rawIt = g_nodeRaw.find(id);
        if (rawIt != g_nodeRaw.end() && rawIt->second.valid)
            out << rawIt->second.aid << "," << rawIt->second.rpsIndex << ","
                << rawIt->second.rawGroup << "," << rawIt->second.rawSlot << ",";
        else
            out << ",,,,";

        out << s.txPackets << "," << s.txBytes << "," << s.rxPackets << "," << s.rxBytes << ","
            << s.fwdPackets << "," << s.fwdBytes << "," << s.deliveredPackets << "," << s.deliveredBytes << ","
            << (simTime > 0 ? s.deliveredBytes * 8.0 / simTime / 1000.0 : 0.0) << ",";
        auto up = g_apRxFromNode.find(id);
        if (up != g_apRxFromNode.end() && up->second.n)
            out << up->second.n << "," << up->second.rssiSum / up->second.n << ","
                << up->second.snrSum / up->second.n;
        else out << ",,";
        out << ",";
        auto down = g_nodeRxFromAp.find(id);
        if (down != g_nodeRxFromAp.end() && down->second.n)
            out << down->second.n << "," << down->second.rssiSum / down->second.n << ","
                << down->second.snrSum / down->second.n;
        else out << ",,";
        out << "," << txJ << "," << rxJ << "," << (txJ + rxJ) << ",";
        if (s.txPackets + s.fwdPackets > 0) out << txJ / static_cast<double>(s.txPackets + s.fwdPackets);
        out << "\n";
    }
}

void ExportarResumoSimulacaoCsv(const std::string& filename, double simTime)
{
    std::ofstream out(filename.c_str());
    uint64_t txPackets = 0, txBytes = 0, deliveredPackets = 0, deliveredBytes = 0;
    uint64_t rxPackets = 0, fwdPackets = 0;
    uint64_t flowTxPackets = 0, flowDeliveredPackets = 0;
    uint64_t flowTxBytes = 0, flowDeliveredBytes = 0;
    double txEnergy = 0.0, rxEnergy = 0.0, lastIpEvent = 0.0;

    for (const auto& kv : g_nodeStats) {
        txPackets += kv.second.txPackets; txBytes += kv.second.txBytes;
        rxPackets += kv.second.rxPackets; fwdPackets += kv.second.fwdPackets;
        deliveredPackets += kv.second.deliveredPackets; deliveredBytes += kv.second.deliveredBytes;
    }
    for (const auto& kv : g_flows) {
        flowTxPackets += kv.second.txPackets; flowDeliveredPackets += kv.second.rxPackets;
        flowTxBytes += kv.second.txBytes; flowDeliveredBytes += kv.second.rxBytes;
    }
    for (const auto& kv : g_energyTxJ) txEnergy += kv.second;
    for (const auto& kv : g_energyRxJ) rxEnergy += kv.second;
    for (const auto& r : g_pktHistory) if (r.time > lastIpEvent) lastIpEvent = r.time;

    const double appRate = simTime > 0 ? deliveredBytes * 8.0 / simTime / 1000.0 : 0.0;
    const uint64_t deliveredForPdr = flowDeliveredPackets > flowTxPackets ? flowTxPackets : flowDeliveredPackets;
    const double flowPdr  = flowTxPackets ? static_cast<double>(deliveredForPdr) / flowTxPackets : -1.0;
    const double flowLoss = flowPdr >= 0.0 ? 100.0 * (1.0 - flowPdr) : -1.0;

    out << "metric,value,unit,definition\n";
    out << "simulation_duration_used_for_rates," << simTime << ",s,Duration used as denominator for node delivery rates\n";
    out << "last_observed_ip_event_time," << lastIpEvent << ",s,Latest timestamp in recorded IP events; not necessarily simulator stop time\n";
    out << "nodes_observed," << g_nodeRole.size() << ",nodes,Nodes detected by monitor\n";
    out << "flows_observed," << g_flows.size() << ",flows,Unique source-destination-protocol-port tuples seen by monitor\n";
    out << "radio_rx_samples," << g_rssiHistory.size() << ",samples,MonitorSnifferRx samples; not unique application packets\n";
    out << "ip_packet_events," << g_pktHistory.size() << ",events,IP Tx Rx and LocalDeliver events; one packet may create multiple events\n";
    out << "ip_originated_tx_packets," << txPackets << ",events,Packets classified as originating IPv4 TX\n";
    out << "ip_originated_tx_bytes," << txBytes << ",bytes,Bytes including IP header counted at originating TX\n";
    out << "ip_rx_packets," << rxPackets << ",events,Packets observed at IPv4 Rx trace; not unique packets\n";
    out << "ip_forwarded_packets," << fwdPackets << ",events,Packets classified as forwarded by source-IP versus current node\n";
    out << "ip_local_deliver_events," << deliveredPackets << ",events,Packets observed at IPv4 LocalDeliver; not necessarily matched to an origin TX\n";
    out << "ip_local_deliver_bytes," << deliveredBytes << ",bytes,Bytes observed at IPv4 LocalDeliver\n";
    out << "delivery_rate_from_local_deliver_events," << appRate << ",kbit/s,LocalDeliver IP bytes divided by supplied duration\n";
    out << "flow_matched_tx_packets," << flowTxPackets << ",packets,Origin TX packets represented in per-flow statistics\n";
    out << "flow_matched_delivered_packets," << flowDeliveredPackets << ",packets,Deliveries matched to an origin TX by source destination protocol and IPv4 identification\n";
    out << "estimated_flow_packet_loss_pct,";
    if (flowLoss >= 0) out << flowLoss;
    out << ",percent,Approximation from per-flow origin TX versus matched deliveries\n";
    out << "flow_pdr_ratio,";
    if (flowPdr >= 0) out << flowPdr;
    out << ",ratio,Matched deliveries divided by origin TX packets across observed flows\n";
    out << "estimated_flow_tx_bytes," << flowTxBytes << ",bytes,Origin TX bytes counted per flow\n";
    out << "estimated_flow_delivered_bytes," << flowDeliveredBytes << ",bytes,Matched delivery bytes counted per flow\n";
    out << "estimated_ip_tx_energy," << txEnergy << ",J,Model estimate using IP bytes nominal rate and assumed TX power; not PHY energy\n";
    out << "estimated_ip_rx_energy," << rxEnergy << ",J,Model estimate at IPv4 Rx events; not PHY energy\n";
    out << "estimated_ip_total_energy," << txEnergy + rxEnergy << ",J,Sum of the monitor's IP-layer energy estimates\n";
}

void ExportarTudo(const std::string& prefixo, double simTime)
{
    // Simulator::Now() é preferível ao tempo nominal quando os eventos continuam
    // além dele. Requer chamar antes de Simulator::Destroy().
    const double observedSimTime = Simulator::Now().GetSeconds();
    const double durationForRates = observedSimTime > 0.0 ? observedSimTime : simTime;
    ExportarAmostrasRadioCsv(prefixo + "_radio_rx_samples.csv");
    ExportarResumoEnlaceApCsv(prefixo + "_link_ap_summary.csv");
    ExportarEventosIpCsv(prefixo + "_ip_packet_events.csv");
    ExportarResumoFluxosCsv(prefixo + "_flow_summary.csv");
    ExportarResumoNosCsv(prefixo + "_node_summary.csv", durationForRates);
    ExportarResumoSimulacaoCsv(prefixo + "_simulation_summary.csv", durationForRates);
    NS_LOG_UNCOND("CSVs exportados com prefixo '" << prefixo << "' ("
                  << g_rssiHistory.size() << " amostras de recepção PHY, "
                  << g_pktHistory.size() << " eventos IP, " << g_flows.size() << " fluxos)");
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