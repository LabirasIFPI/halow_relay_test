#ifndef MONITOR_BASE_H
#define MONITOR_BASE_H

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
#include <cmath>

using namespace ns3;

/**
 * @brief Papel de um nó na rede.
 *
 * Um relay tem dois devices Wi-Fi: um AP (para os filhos) e uma STA (ligada ao pai).
 */
enum class NodeRole { STA, AP, RELAY, UNKNOWN };

/**
 * @brief Converte o papel de um nó em texto para os CSVs.
 * @param r Papel do nó.
 * @return "STA", "AP", "RELAY" ou "UNKNOWN".
 */
inline std::string RoleToString(NodeRole r)
{
    switch (r) {
        case NodeRole::STA:   return "STA";
        case NodeRole::AP:    return "AP";
        case NodeRole::RELAY: return "RELAY";
        default:              return "UNKNOWN";
    }
}

/**
 * @brief Soma de amostras de sinal em uma direção de um enlace.
 *
 * O RSSI é medido no receptor, por isso uplink e downlink ficam em
 * estatísticas separadas e nunca são misturados na mesma média.
 */
struct RadioDirectionStats {
    uint64_t n = 0;
    double rssiSum = 0.0, noiseSum = 0.0, snrSum = 0.0;
    double rssiMin = std::numeric_limits<double>::infinity();
    double rssiMax = -std::numeric_limits<double>::infinity();
};

/**
 * @brief Acrescenta uma amostra de recepção às estatísticas de uma direção.
 * @param s Estatísticas a atualizar.
 * @param rssi Potência recebida (dBm).
 * @param noise Ruído informado pelo sniffer (dBm).
 * @param snr SNR do quadro (dB).
 */
inline void AddRadioDirectionSample(RadioDirectionStats& s, double rssi,
                                    double noise, double snr)
{
    ++s.n;
    s.rssiSum += rssi; s.noiseSum += noise; s.snrSum += snr;
    if (rssi < s.rssiMin) s.rssiMin = rssi;
    if (rssi > s.rssiMax) s.rssiMax = rssi;
}

uint32_t g_centralApId = 0;

std::map<uint32_t, NodeRole>               g_nodeRole;
std::map<Mac48Address, uint32_t>           g_macToNode;
std::map<uint32_t, uint32_t>               g_ipToNode;
std::map<std::pair<uint32_t,uint32_t>, std::string> g_devIface;
std::map<Mac48Address, bool>               g_macIsAp;
std::map<uint32_t, Ptr<YansWifiPhy> >      g_staPhyDoNo;
std::map<uint32_t, Ptr<YansWifiPhy> >      g_apPhyDoNo;
std::map<uint32_t, uint32_t>               g_paiNo;

bool g_verbose = false;

/**
 * @brief Retorna o pai de um nó na árvore.
 * @param no Id do nó filho.
 * @return Id do pai registrado com DefinirPai(); sem registro, o AP central.
 */
inline uint32_t PaiDe(uint32_t no)
{
    auto it = g_paiNo.find(no);
    return it == g_paiNo.end() ? g_centralApId : it->second;
}

/**
 * @brief Registra o pai de um nó (ex.: uma STA atrás de um relay).
 *
 * Nós sem pai registrado são tratados como filhos do AP central.
 * @param filho Id do nó filho.
 * @param pai Id do nó pai (gateway ou relay).
 */
inline void DefinirPai(uint32_t filho, uint32_t pai) { g_paiNo[filho] = pai; }

/**
 * @brief Indica se um endereço MAC é a interface AP de algum nó.
 * @param mac Endereço MAC.
 * @return true se for a interface AP de um gateway ou relay.
 */
inline bool MacEhAp(const Mac48Address& mac)
{
    auto it = g_macIsAp.find(mac);
    return it != g_macIsAp.end() && it->second;
}

/**
 * @brief Distância entre um nó e o AP central.
 * @param nodeId Id do nó.
 * @return Distância em metros, ou -1 se algum dos nós não tiver posição.
 */
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

/**
 * @brief Distância entre dois nós quaisquer.
 * @param a Id do primeiro nó.
 * @param b Id do segundo nó.
 * @return Distância em metros, ou -1 se algum dos nós não tiver posição.
 */
inline double DistanciaEntreNos(uint32_t a, uint32_t b)
{
    Ptr<MobilityModel> ma = NodeList::GetNode(a)->GetObject<MobilityModel>();
    Ptr<MobilityModel> mb = NodeList::GetNode(b)->GetObject<MobilityModel>();
    return (ma && mb) ? ma->GetDistanceFrom(mb) : -1.0;
}

/**
 * @brief Descobre o papel de cada nó e monta as tabelas usadas pelos sniffers.
 *
 * Preenche: papel (STA, AP ou RELAY), MAC -> nó, IP -> nó, interface de cada
 * device ("AP"/"STA"), se cada MAC é de uma interface AP e o PHY das
 * interfaces STA e AP de cada nó.
 * @param nodes Todos os nós da simulação.
 */
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
            Ptr<YansWifiPhy> phy = DynamicCast<YansWifiPhy>(w->GetPhy());
            if (DynamicCast<StaWifiMac>(mac))     { hasSta = true; t = "STA"; g_staPhyDoNo[id] = phy; }
            else if (DynamicCast<ApWifiMac>(mac)) { hasAp  = true; t = "AP";  g_apPhyDoNo[id] = phy; }
            g_devIface[{id, w->GetIfIndex()}] = t;
            g_macToNode[Mac48Address::ConvertFrom(w->GetAddress())] = id;
            g_macIsAp[Mac48Address::ConvertFrom(w->GetAddress())] = (t == "AP");
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

#endif // MONITOR_BASE_H
