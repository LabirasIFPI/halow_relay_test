#ifndef MONITOR_TRAFEGO_H
#define MONITOR_TRAFEGO_H

#include "base.h"
#include "monitor_enlace.h"

/**
 * @brief Totais de um fluxo (origem, destino, protocolo e portas).
 */
struct FlowStats {
    Ipv4Address srcIp, dstIp;
    uint8_t     proto = 0;
    uint16_t    srcPort = 0, dstPort = 0;
    uint64_t    txPackets = 0, rxPackets = 0;
    uint64_t    txBytes = 0,   rxBytes = 0;
    double      firstTx = -1, lastRx = -1;
    double      delaySum = 0, hopsSum = 0;
};

/**
 * @brief Totais de pacotes IP de um nó.
 *
 * tx: originados no nó; rx: recebidos no L3 (inclui os encaminhados);
 * fwd: encaminhados (relay); delivered: entregues à pilha local.
 */
struct NodeStats {
    uint64_t txPackets = 0, txBytes = 0;
    uint64_t rxPackets = 0, rxBytes = 0;
    uint64_t fwdPackets = 0, fwdBytes = 0;
    uint64_t deliveredPackets = 0, deliveredBytes = 0;
};

/**
 * @brief Pacote originado que ainda espera a entrega, para medir atraso e saltos.
 */
struct PendingTx { double time; uint8_t ttl; };

/**
 * @brief RAW a que um nó pertence, registrado quando ele associa.
 *
 * Índices em base 1, como no NodeEntry. valid = false indica nó sem RAW
 * registrada (o AP ou nós que nunca associaram).
 */
struct NodeRawInfo {
    bool     valid    = false;
    uint32_t aid      = 0;
    int      rpsIndex = -1;
    int      rawGroup = -1;
    int      rawSlot  = -1;
};

typedef std::tuple<uint32_t, uint32_t, uint8_t, uint16_t, uint16_t> FlowKey;
typedef std::tuple<uint32_t, uint32_t, uint8_t, uint16_t>           PktKey;

std::map<FlowKey, uint32_t>                g_flowIds;
std::map<uint32_t, FlowStats>              g_flows;
std::map<uint32_t, NodeStats>              g_nodeStats;
std::map<PktKey, PendingTx>                g_pending;
std::map<uint32_t, NodeRawInfo>            g_nodeRaw;
std::map<uint32_t, double>                 g_energyTxJ;
std::map<uint32_t, double>                 g_energyRxJ;

/**
 * @brief Registra a RAW de um nó. Chamar em onSTAAssociated.
 *
 * Se o nó reassociar, o último registro prevalece.
 * @param nodeId Id do nó.
 * @param aid AID recebido na associação.
 * @param rpsIndex Índice do RPS (base 1).
 * @param rawGroup Grupo RAW (base 1).
 * @param rawSlot Slot RAW (base 1).
 */
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

double g_monitorTxPowerW       = 0.8;
double g_monitorRxPowerW       = 0.5;
double g_monitorNominalRateBps = 150000.0;

/**
 * @brief Estima a energia de um evento IP. Modelo simplificado, não é medição de rádio.
 *
 * Usa o tempo no ar do pacote na taxa nominal g_monitorNominalRateBps e a
 * potência média assumida g_monitorTxPowerW ou g_monitorRxPowerW. Não
 * substitui o Energy Framework do ns-3.
 * @param bytes Tamanho do pacote IP.
 * @param tx true para transmissão, false para recepção.
 * @return Energia estimada (J).
 */
inline double EstimarEnergiaJ(uint32_t bytes, bool tx)
{
    double rate = g_monitorNominalRateBps > 0 ? g_monitorNominalRateBps : 150000.0;
    double airtime = (static_cast<double>(bytes) * 8.0) / rate;
    return airtime * (tx ? g_monitorTxPowerW : g_monitorRxPowerW);
}

/**
 * @brief Encontra o nó dono de um endereço IP.
 * @param a Endereço IP.
 * @return Id do nó, ou 0xFFFFFFFF se o endereço não for conhecido.
 */
inline uint32_t NodeFromIp(Ipv4Address a)
{
    auto it = g_ipToNode.find(a.Get());
    return it == g_ipToNode.end() ? 0xFFFFFFFF : it->second;
}

/**
 * @brief Lê as portas de origem e destino de um pacote UDP ou TCP.
 *
 * Fragmentos (exceto o primeiro) e outros protocolos ficam com portas 0.
 * @param h Cabeçalho IP.
 * @param payload Conteúdo após o cabeçalho IP.
 * @param sp Porta de origem (saída).
 * @param dp Porta de destino (saída).
 */
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

/**
 * @brief Retorna o id do fluxo de um pacote, criando o fluxo na primeira vez.
 * @param h Cabeçalho IP.
 * @param sp Porta de origem.
 * @param dp Porta de destino.
 * @return Id do fluxo (começa em 1).
 */
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

/**
 * @brief Acompanha os pacotes IP de um nó (traces Tx, Rx e LocalDeliver do Ipv4L3Protocol).
 *
 * Classifica cada evento (TX, FWD, RX, DELIVER), atualiza os totais do nó e do
 * fluxo e, na entrega, calcula o atraso de ponta a ponta e os saltos IP pela
 * diferença de TTL. O TTL só conta saltos de roteamento IP; relays de camada 2
 * não aparecem nele.
 */
class PacketSniffer
{
public:
    explicit PacketSniffer(uint32_t nodeId) : m_nodeId(nodeId) {}

    void OnTx(Ptr<const Packet> p, Ptr<Ipv4> ipv4, uint32_t iface) { WithHeader("TX", p, iface); }
    void OnRx(Ptr<const Packet> p, Ptr<Ipv4> ipv4, uint32_t iface) { WithHeader("RX", p, iface); }

    void OnLocalDeliver(const Ipv4Header& h, Ptr<const Packet> p, uint32_t iface)
    {
        Process("DELIVER", h, p, iface);
    }

private:
    void WithHeader(const std::string& evt, Ptr<const Packet> p, uint32_t iface)
    {
        Ptr<Packet> c = p->Copy();
        Ipv4Header h;
        c->RemoveHeader(h);
        Process(evt, h, c, iface);
    }

    void Process(std::string evt, const Ipv4Header& h, Ptr<const Packet> payload, uint32_t iface)
    {
        if (iface == 0) return;

        double   now = Simulator::Now().GetSeconds();
        uint16_t sp, dp;
        ExtrairPortas(h, payload, sp, dp);
        uint32_t flowId = ObterFlowId(h, sp, dp);
        uint32_t size   = payload->GetSize() + h.GetSerializedSize();
        FlowStats& fs   = g_flows[flowId];
        NodeStats& ns   = g_nodeStats[m_nodeId];

        if (evt == "TX" && NodeFromIp(h.GetSource()) != m_nodeId) evt = "FWD";

        if (evt == "TX") {
            ns.txPackets++; ns.txBytes += size;
            g_energyTxJ[m_nodeId] += EstimarEnergiaJ(size, true);
            fs.txPackets++; fs.txBytes += size;
            if (fs.firstTx < 0) fs.firstTx = now;
            PktKey k(h.GetSource().Get(), h.GetDestination().Get(), h.GetProtocol(), h.GetIdentification());
            g_pending[k] = PendingTx{now, h.GetTtl()};
        }
        else if (evt == "FWD") {
            ns.fwdPackets++; ns.fwdBytes += size;
            g_energyTxJ[m_nodeId] += EstimarEnergiaJ(size, true);
        }
        else if (evt == "RX") {
            ns.rxPackets++; ns.rxBytes += size;
            g_energyRxJ[m_nodeId] += EstimarEnergiaJ(size, false);
        }
        else if (evt == "DELIVER") {
            ns.deliveredPackets++; ns.deliveredBytes += size;
            PktKey k(h.GetSource().Get(), h.GetDestination().Get(), h.GetProtocol(), h.GetIdentification());
            auto it = g_pending.find(k);
            if (it != g_pending.end()) {
                const double delay = now - it->second.time;
                const int    hops  = (int)it->second.ttl - (int)h.GetTtl();
                g_pending.erase(it);
                fs.rxPackets++; fs.rxBytes += size;
                fs.delaySum += delay; fs.hopsSum += hops;
                fs.lastRx = now;
            }
        }

        if (g_verbose)
            NS_LOG_UNCOND(now << "s | Nó " << m_nodeId << " " << evt << " flow " << flowId
                          << " " << h.GetSource() << "->" << h.GetDestination()
                          << " " << size << "B");
    }

    uint32_t m_nodeId;
};

/**
 * @brief Grava os totais de cada fluxo (flow_summary.csv).
 *
 * PDR = pacotes entregues / pacotes originados, limitado a 1 para que
 * duplicatas ou inconsistências de contagem nunca gerem PDR > 1.
 * @param filename Caminho do CSV.
 */
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

/**
 * @brief Grava os totais de cada nó (node_summary.csv).
 *
 * Inclui a RAW do nó (vazia para o AP e para nós que nunca associaram), os
 * pacotes IP, o sinal do enlace direto com o AP central e a energia estimada.
 * @param filename Caminho do CSV.
 * @param simTime Duração usada para calcular as taxas (s).
 */
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

#endif // MONITOR_TRAFEGO_H
