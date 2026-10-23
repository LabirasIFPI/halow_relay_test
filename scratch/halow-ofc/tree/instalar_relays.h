#ifndef INSTALAR_RELAYS_H
#define INSTALAR_RELAYS_H

#include "ns3/core-module.h"
#include "ns3/network-module.h"
#include "ns3/wifi-module.h"
#include "ns3/internet-module.h"
#include "ns3/mobility-module.h"
#include "ns3/edca-txop-n.h"
#include "ns3/wifi-mac-queue.h"
#include "ns3/ipv4-static-routing-helper.h"

#include "../s1g-test-tim-raw.h"
#include "../monitor/monitor.h"
#include "topologia.h"

#include <map>
#include <string>
#include <vector>

using namespace ns3;

std::vector<int> g_paiDe;
std::vector<bool> g_ehRelay;

/**
 * @brief Peças da árvore instaladas no ns-3 que passam de uma etapa para a outra.
 *
 *  - canalGateway: canal do BSS do gateway (o salto compartilhado com os relays).
 *  - canalRelay: canal do BSS de cada relay; canalRelay[k] é o do relay topo.relays[k].
 *    Cada relay tem um canal próprio (como canais HaLow diferentes), então os
 *    BSSs não interferem entre si.
 *  - staDevice: interface STA de cada nó, na ordem dos índices. No relay, é a
 *    STA ligada ao pai (DeviceList/0).
 *  - relayApDevice: lado AP de cada relay (DeviceList/1); relayApDevice.Get(k)
 *    é o AP do relay topo.relays[k].
 */
struct RedeArvore
{
    Ptr<YansWifiChannel> canalGateway;
    std::vector<Ptr<YansWifiChannel> > canalRelay;
    NetDeviceContainer staDevice;
    NetDeviceContainer relayApDevice;

    /**
     * @brief Canal do BSS de um AP (gateway ou relay).
     * @param ap PAI_GATEWAY ou índice da STA que é relay.
     * @param topo Topologia montada.
     */
    Ptr<YansWifiChannel> CanalDoAp(int ap, const Topologia& topo) const
    {
        return ap == PAI_GATEWAY ? canalGateway : canalRelay[topo.IndiceRelay(ap)];
    }
};

/**
 * @brief SSID do BSS de um AP: o do gateway ou "ns380211ah-relay-<k>" para o relay k.
 * @param ap PAI_GATEWAY ou índice da STA que é relay.
 * @param topo Topologia montada.
 * @param ssidGateway SSID do gateway.
 */
inline Ssid SsidDoAp(int ap, const Topologia& topo, const Ssid& ssidGateway)
{
    return ap == PAI_GATEWAY ? ssidGateway : Ssid("ns380211ah-relay-" + std::to_string(topo.IndiceRelay(ap)));
}

/**
 * @brief Monta a topologia e decide a árvore, antes de criar qualquer nó do ns-3.
 *
 * Topology=area: grade da área com os relays nas posições dadas (config.Nsta
 * passa a ser o número de pontos da grade). Topology=grid: matriz a partir do
 * gateway, sem relays. Em seguida lê a curva de LQI e decide o pai de cada nó
 * com MontarArvore(), preenche g_paiDe e g_ehRelay (usados para liberar o
 * tráfego só quando o caminho até o gateway está associado) e grava o
 * resultado_topologia.csv.
 * @param config Parâmetros da simulação; config.Nsta é ajustado na topologia "area".
 * @param larguraMhz Largura de banda do rádio (MHz), usada na previsão do SNR.
 * @return Topologia com posições, relays e o pai de cada nó.
 */
inline Topologia DecidirArvore(Configuration& config, double larguraMhz)
{
    Topologia topo;
    if (config.topology == "area") {
        topo = MontarTopologiaArea(config.areaSize, config.gridSpacing, config.numRelays, config.relayPositions);
        std::cout << "Topology=area: " << config.areaSize << " x " << config.areaSize << " m, espaçamento "
                  << config.gridSpacing << " m -> Nsta=" << topo.sta.size() << " (inclui os relays)" << std::endl;
        config.Nsta = topo.sta.size();
    } else {
        double xpos0 = std::stoi(config.rho, nullptr, 0);
        topo = MontarTopologiaGrid(config.Nsta, xpos0, xpos0, config.gridSpacing);
    }
    CriterioEnlace criterio;
    criterio.curva = LerCurvaLqi(config.lqiCurveFile);
    criterio.lqiMinimo = config.minLqiParent;
    criterio.snrMinimo = config.minSnrParent;
    criterio.maxFilhos = config.maxChildren;
    NS_ABORT_MSG_IF(config.parentPreference != "saltos" && config.parentPreference != "sinal",
            "ParentPreference deve ser saltos ou sinal");
    criterio.preferirMenosSaltos = (config.parentPreference == "saltos");
    criterio.larguraMhz = larguraMhz;
    MontarArvore(topo, criterio);
    if (!topo.relays.empty()) ImprimirResumoArvore(topo);
    g_paiDe = topo.paiInstalacao;
    g_ehRelay.assign(topo.sta.size(), false);
    for (int r : topo.relays) g_ehRelay[r] = true;
    ExportarTopologiaCsv("resultado_topologia.csv", topo);
    return topo;
}

/**
 * @brief Cria o canal do gateway e um canal para cada relay.
 * @param rede Recebe canalGateway e canalRelay.
 * @param builder Modelo de canal (propagação e atraso) já configurado.
 * @param topo Topologia montada.
 * @param aoTransmitir Função ligada ao trace "Transmission" de cada canal.
 */
inline void CriarCanais(RedeArvore& rede, YansWifiChannelHelper& builder, const Topologia& topo,
                        void (*aoTransmitir)(Ptr<NetDevice>, Ptr<Packet>))
{
    rede.canalGateway = builder.Create();
    rede.canalGateway->TraceConnectWithoutContext("Transmission", MakeCallback(aoTransmitir));
    for (size_t k = 0; k < topo.relays.size(); k++) {
        rede.canalRelay.push_back(builder.Create());
        rede.canalRelay.back()->TraceConnectWithoutContext("Transmission", MakeCallback(aoTransmitir));
    }
}

/**
 * @brief Instala a interface STA de cada nó no BSS do seu pai.
 *
 * Sem relays, instala todas de uma vez no BSS do gateway. Com relays, instala
 * nó a nó, na ordem dos índices: assim o MAC (e portanto o AID) da STA i
 * continua sendo i+1, dentro da faixa coberta pelo RAW de todos os APs. No
 * relay, esta é a interface STA (DeviceList/0), ligada ao BSS do pai. Ao final,
 * o phy volta para o canal do gateway.
 * @param rede Recebe staDevice; usa os canais criados por CriarCanais().
 * @param topo Topologia montada.
 * @param config Parâmetros da simulação.
 * @param wifi Helper Wi-Fi já configurado.
 * @param phy Helper do PHY com os parâmetros das STAs.
 * @param mac Helper do MAC já com o tipo StaWifiMac e o SSID do gateway.
 * @param ssidGateway SSID do gateway.
 */
inline void InstalarStas(RedeArvore& rede, const Topologia& topo, const Configuration& config,
                         WifiHelper& wifi, YansWifiPhyHelper& phy, S1gWifiMacHelper& mac,
                         const Ssid& ssidGateway)
{
    if (topo.relays.empty()) {
        rede.staDevice = wifi.Install(phy, mac, wifiStaNode);
    } else {
        for (uint32_t i = 0; i < config.Nsta; i++) {
            int ap = topo.paiInstalacao[i];
            mac.SetType("ns3::StaWifiMac", "Ssid", SsidValue(SsidDoAp(ap, topo, ssidGateway)),
                    "ActiveProbing", BooleanValue(false));
            phy.SetChannel(rede.CanalDoAp(ap, topo));
            rede.staDevice.Add(wifi.Install(phy, mac, wifiStaNode.Get(i)));
        }
        phy.SetChannel(rede.canalGateway);
    }
}

/**
 * @brief Instala o lado AP de cada relay: mesmo PHY, RAW e TIM do gateway, com SSID e canal próprios.
 *
 * Fica como DeviceList/1 do nó relay (DeviceList/0 é a STA ligada ao pai).
 * Chamar depois de instalar o AP do gateway, com o phy já nos parâmetros de AP.
 * Ao final, o phy volta para o canal do gateway.
 * @param rede Recebe relayApDevice; usa canalRelay.
 * @param topo Topologia montada.
 * @param config Parâmetros da simulação (beacon, RAW, TIM, páginas).
 * @param wifi Helper Wi-Fi já configurado.
 * @param phy Helper do PHY com os parâmetros de AP.
 * @param mac Helper do MAC.
 * @param ssidGateway SSID do gateway.
 */
inline void InstalarApsDosRelays(RedeArvore& rede, const Topologia& topo, Configuration& config,
                                 WifiHelper& wifi, YansWifiPhyHelper& phy, S1gWifiMacHelper& mac,
                                 const Ssid& ssidGateway)
{
    for (size_t k = 0; k < topo.relays.size(); k++) {
        mac.SetType ("ns3::ApWifiMac",
                         "Ssid", SsidValue (SsidDoAp(topo.relays[k], topo, ssidGateway)),
                         "BeaconInterval", TimeValue (MicroSeconds(config.BeaconInterval)),
                         "NRawStations", UintegerValue (config.NRawSta),
                         "RPSsetup", RPSVectorValue (config.rps),
                         "PageSliceSet", pageSliceValue (config.pageS),
                         "TIMSet", TIMValue (config.tim)
                       );
        phy.SetChannel(rede.canalRelay[k]);
        rede.relayApDevice.Add(wifi.Install(phy, mac, wifiStaNode.Get(topo.relays[k])));
    }
    phy.SetChannel(rede.canalGateway);
}

/**
 * @brief Ajusta o comportamento dos dois lados de cada relay.
 *
 *  - Lado STA: dorme ou não (config.relayStaPowerSave; o padrão do módulo é
 *    dormir como qualquer STA), isenção de RAW (config.relayStaRawExempt) e
 *    tamanho da fila de subida (config.relayStaQueueMaxPackets).
 *  - Lado AP: tamanho e tempo máximo da fila de dados (config.relayApQueueMaxPackets
 *    e config.relayApQueueMaxDelayMs).
 * Chamar depois do Config::Set que fixa as filas de todo DeviceList/0, senão a
 * fila do lado STA dos relays volta ao valor geral.
 * @param rede Devices instalados.
 * @param topo Topologia montada.
 * @param config Parâmetros da simulação.
 */
inline void ConfigurarRelays(const RedeArvore& rede, const Topologia& topo, const Configuration& config)
{
    for (int r : topo.relays)
        DynamicCast<WifiNetDevice>(rede.staDevice.Get(r))->GetMac()->SetAttribute(
                "PowerSaveEnabled", BooleanValue(config.relayStaPowerSave));
    for (int r : topo.relays)
        DynamicCast<WifiNetDevice>(rede.staDevice.Get(r))->GetMac()->SetAttribute(
                "RawExempt", BooleanValue(config.relayStaRawExempt));
    for (uint32_t k = 0; k < rede.relayApDevice.GetN(); k++) {
        Ptr<WifiNetDevice> d = DynamicCast<WifiNetDevice>(rede.relayApDevice.Get(k));
        PointerValue edca;
        d->GetMac()->GetAttribute("BE_EdcaTxopN", edca);
        Ptr<WifiMacQueue> fila = edca.Get<EdcaTxopN>()->GetEdcaQueue();
        fila->SetAttribute("MaxDelay", TimeValue(MilliSeconds(config.relayApQueueMaxDelayMs)));
        fila->SetAttribute("MaxPacketNumber", UintegerValue(config.relayApQueueMaxPackets));
    }
    for (int r : topo.relays) {
        PointerValue edca;
        DynamicCast<WifiNetDevice>(rede.staDevice.Get(r))->GetMac()->GetAttribute("BE_EdcaTxopN", edca);
        edca.Get<EdcaTxopN>()->GetEdcaQueue()->SetAttribute("MaxPacketNumber",
                UintegerValue(config.relayStaQueueMaxPackets));
    }
}

/**
 * @brief Põe o gateway e cada STA nas posições calculadas pela topologia.
 *
 * Grid: gateway na célula (0, 0) da matriz. Area: gateway no canto inferior
 * direito da área.
 * @param topo Topologia montada.
 */
inline void PosicionarNos(const Topologia& topo)
{
    MobilityHelper mobilityAp;
    Ptr<ListPositionAllocator> apAlloc = CreateObject<ListPositionAllocator>();
    apAlloc->Add(topo.gateway);
    mobilityAp.SetPositionAllocator(apAlloc);
    mobilityAp.SetMobilityModel("ns3::ConstantPositionMobilityModel");
    mobilityAp.Install(wifiApNode);

    Ptr<ListPositionAllocator> staAlloc = CreateObject<ListPositionAllocator>();
    for (const Vector& p : topo.sta)
        staAlloc->Add(p);

    MobilityHelper mobilitySta;
    mobilitySta.SetPositionAllocator(staAlloc);
    mobilitySta.SetMobilityModel("ns3::ConstantPositionMobilityModel");
    mobilitySta.Install(wifiStaNode);
}

/**
 * @brief Atribui os endereços IP e cria as rotas estáticas da árvore.
 *
 * Sem relays: uma sub-rede única 10.0.0.0/8. Com relays: uma sub-rede por BSS,
 * 10.1.0.0/16 para o gateway e 10.(2+k).0.0/16 para o relay k (até 250 relays).
 * staNodeInterface fica na ordem dos índices das STAs (usado em
 * getSTAIdFromAddress). Rotas (interface 1 = primeiro rádio do nó, que no relay
 * é a STA; interface 2 do relay = seu AP; interface 1 do gateway = seu AP):
 *  - subida: todo nó, inclusive relay, usa o AP do pai como rota padrão;
 *  - descida: cada AP no caminho até o gateway aprende a sub-rede de cada relay
 *    abaixo dele, com próximo salto = o filho (relay) que leva até ela.
 * Chamar depois de instalar a pilha de Internet.
 * @param rede Devices instalados.
 * @param topo Topologia montada.
 * @param config Parâmetros da simulação.
 */
inline void EnderecarERotear(const RedeArvore& rede, const Topologia& topo, const Configuration& config)
{
    Ipv4AddressHelper address;

    if (topo.relays.empty()) {
        address.SetBase("10.0.0.0", "255.0.0.0");
        staNodeInterface = address.Assign(rede.staDevice);
        apNodeInterface  = address.Assign(apDevice);
        return;
    }

    NS_ABORT_MSG_IF(topo.relays.size() > 250, "Endereçamento suporta até 250 relays");
    auto redeDoAp = [&](int ap) {
        int octeto = (ap == PAI_GATEWAY) ? 1 : 2 + topo.IndiceRelay(ap);
        return "10." + std::to_string(octeto) + ".0.0";
    };
    std::map<int, Ipv4AddressHelper> addrDoAp;
    addrDoAp[PAI_GATEWAY].SetBase(redeDoAp(PAI_GATEWAY).c_str(), "255.255.0.0");
    for (int r : topo.relays) addrDoAp[r].SetBase(redeDoAp(r).c_str(), "255.255.0.0");

    for (uint32_t i = 0; i < config.Nsta; i++)
        staNodeInterface.Add(addrDoAp[topo.paiInstalacao[i]].Assign(NetDeviceContainer(rede.staDevice.Get(i))));
    apNodeInterface = addrDoAp[PAI_GATEWAY].Assign(apDevice);
    std::map<int, Ipv4Address> ipDoAp;
    ipDoAp[PAI_GATEWAY] = apNodeInterface.GetAddress(0);
    for (size_t k = 0; k < topo.relays.size(); k++)
        ipDoAp[topo.relays[k]] = addrDoAp[topo.relays[k]].Assign(NetDeviceContainer(rede.relayApDevice.Get(k))).GetAddress(0);

    Ipv4StaticRoutingHelper srh;
    auto rotasDoAp = [&](int ap) {
        Ptr<Node> no = (ap == PAI_GATEWAY) ? wifiApNode.Get(0) : wifiStaNode.Get(ap);
        return srh.GetStaticRouting(no->GetObject<Ipv4>());
    };
    for (uint32_t i = 0; i < config.Nsta; i++)
        srh.GetStaticRouting(wifiStaNode.Get(i)->GetObject<Ipv4>())
            ->SetDefaultRoute(ipDoAp[topo.paiInstalacao[i]], 1);
    for (int r : topo.relays) {
        int filho = r, ap = topo.paiInstalacao[r];
        for (uint32_t guarda = 0; guarda <= topo.relays.size(); guarda++) {
            rotasDoAp(ap)->AddNetworkRouteTo(Ipv4Address(redeDoAp(r).c_str()), Ipv4Mask("255.255.0.0"),
                    staNodeInterface.GetAddress(filho), ap == PAI_GATEWAY ? 1 : 2);
            if (ap == PAI_GATEWAY) break;
            filho = ap;
            ap = topo.paiInstalacao[ap];
        }
    }
}

/**
 * @brief Informa ao monitor o pai de cada nó, para medir cada enlace filho -> pai.
 *
 * Chamar depois de InstalarSniffers(). Nós ligados ao gateway não precisam de
 * registro (o monitor assume o AP central).
 * @param topo Topologia montada.
 * @param config Parâmetros da simulação.
 */
inline void RegistrarPaisNoMonitor(const Topologia& topo, const Configuration& config)
{
    for (uint32_t i = 0; i < config.Nsta; i++)
        if (topo.paiInstalacao[i] >= 0)
            DefinirPai(wifiStaNode.Get(i)->GetId(), wifiStaNode.Get(topo.paiInstalacao[i])->GetId());
}

#endif // INSTALAR_RELAYS_H
