#ifndef MONITOR_H
#define MONITOR_H

#include "base.h"
#include "monitor_enlace.h"
#include "monitor_trafego.h"

std::vector<RssiSniffer*>    g_rssiSniffers;
std::vector<FrameTxSniffer*> g_txSniffers;
std::vector<PacketSniffer*>  g_pktSniffers;

/**
 * @brief Detecta os papéis dos nós e liga os sniffers de enlace e de tráfego.
 *
 * Chamar DEPOIS de instalar a pilha IP e atribuir os endereços. Depois dela,
 * chamar DefinirPai() para cada nó atrás de um relay.
 *  - Em cada device Wi-Fi (o relay tem dois): sniffers de enlace (sinal dos
 *    quadros recebidos e quadros enviados).
 *  - Em cada nó com IP: sniffer de tráfego (pacotes IP).
 * @param nodes Todos os nós da simulação.
 * @param centralApId Id do nó do AP central (gateway).
 */
void InstalarSniffers(NodeContainer nodes, uint32_t centralApId = 0)
{
    g_centralApId = centralApId;
    DetectarPapeis(nodes);

    for (uint32_t i = 0; i < nodes.GetN(); ++i) {
        Ptr<Node> node = nodes.Get(i);
        uint32_t nodeId = node->GetId();

        for (uint32_t d = 0; d < node->GetNDevices(); ++d) {
            Ptr<WifiNetDevice> w = DynamicCast<WifiNetDevice>(node->GetDevice(d));
            if (!w || !w->GetPhy()) continue;

            std::string t = g_devIface[{nodeId, w->GetIfIndex()}];
            const Mac48Address mac = Mac48Address::ConvertFrom(w->GetAddress());

            RssiSniffer* s = new RssiSniffer(nodeId, t);
            g_rssiSniffers.push_back(s);
            w->GetPhy()->TraceConnectWithoutContext(
                "MonitorSnifferRx", MakeCallback(&RssiSniffer::operator(), s));

            FrameTxSniffer* tx = new FrameTxSniffer(nodeId, mac);
            g_txSniffers.push_back(tx);
            w->GetPhy()->TraceConnectWithoutContext(
                "PhyTxBegin", MakeCallback(&FrameTxSniffer::operator(), tx));
        }

        Ptr<Ipv4L3Protocol> l3 = node->GetObject<Ipv4L3Protocol>();
        if (!l3) continue;
        PacketSniffer* ps = new PacketSniffer(nodeId);
        g_pktSniffers.push_back(ps);
        l3->TraceConnectWithoutContext("Tx",           MakeCallback(&PacketSniffer::OnTx, ps));
        l3->TraceConnectWithoutContext("Rx",           MakeCallback(&PacketSniffer::OnRx, ps));
        l3->TraceConnectWithoutContext("LocalDeliver", MakeCallback(&PacketSniffer::OnLocalDeliver, ps));
    }
}

/**
 * @brief Grava os CSVs do monitor ao final da simulação, com o prefixo dado.
 *
 * Enlace: link_frame_summary. Tráfego: flow_summary e node_summary.
 * (O resultado_topologia.csv é gravado antes da simulação, por tree/topologia.h.)
 * As taxas usam Simulator::Now() quando os eventos continuam além do tempo
 * nominal; por isso, chamar antes de Simulator::Destroy().
 * @param prefixo Início do nome de cada arquivo (ex.: "resultado").
 * @param simTime Tempo nominal da simulação (s), usado se Now() for zero.
 */
void ExportarTudo(const std::string& prefixo, double simTime)
{
    const double observedSimTime = Simulator::Now().GetSeconds();
    const double durationForRates = observedSimTime > 0.0 ? observedSimTime : simTime;

    ExportarResumoEnlaceQuadrosCsv(prefixo + "_link_frame_summary.csv");
    ExportarResumoFluxosCsv(prefixo + "_flow_summary.csv");
    ExportarResumoNosCsv(prefixo + "_node_summary.csv", durationForRates);

    NS_LOG_UNCOND("CSVs exportados com prefixo '" << prefixo << "' ("
                  << g_linkFrames.size() << " enlaces, " << g_flows.size() << " fluxos)");
}

/**
 * @brief Libera os sniffers criados por InstalarSniffers(). Chamar no fim da simulação.
 */
void LimparSniffers()
{
    for (auto* s : g_rssiSniffers) delete s;
    for (auto* s : g_txSniffers)   delete s;
    for (auto* s : g_pktSniffers)  delete s;
    g_rssiSniffers.clear();
    g_txSniffers.clear();
    g_pktSniffers.clear();
}

#endif // MONITOR_H
