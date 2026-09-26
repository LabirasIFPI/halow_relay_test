/*
 * relay-selection-helper.h
 *
 * Seleção de Relay para o IEEE-802.11ah-ns-3 (imec-IDLab, base ns-3.23).
 *
 * O RSSI NÃO é fictício: ele é calculado com os MESMOS objetos e a MESMA
 * sequência de operações que o simulador usa para entregar um quadro:
 *
 *   YansWifiPhy::SendPacket      -> txPower = GetPowerDbm(nível) + TxGain
 *   YansWifiChannel::Send        -> rxPower = m_loss->CalcRxPower(txPower, mobTx, mobRx)
 *   YansWifiPhy::StartReceive... -> rxPower += RxGain
 *
 * Para isso, o PropagationLossModel instalado no YansWifiChannel é criado
 * explicitamente no cenário e registrado aqui (SetPropagationLossModel).
 *
 * Header-only para poder ficar em scratch/ ao lado do programa principal.
 */
#ifndef RELAY_SELECTION_HELPER_H
#define RELAY_SELECTION_HELPER_H

#include "ns3/core-module.h"
#include "ns3/network-module.h"
#include "ns3/mobility-module.h"
#include "ns3/wifi-module.h"
#include "ns3/propagation-module.h"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

namespace ns3 {

// Nome legível de um nó (usa ns3::Names; cai para "Node#id" se não houver nome)
inline std::string
RelayNodeName (Ptr<Node> node)
{
  if (node == 0)
    {
      return "(nenhum)";
    }
  std::string name = Names::FindName (node);
  if (!name.empty ())
    {
      return name;
    }
  std::ostringstream oss;
  oss << "Node#" << node->GetId ();
  return oss.str ();
}

// Métricas suportadas. RSSI_SUM é a pedida; as outras já estão prontas
// porque usam exatamente os mesmos dados (sem nova dependência de API).
enum RelayMetric
{
  RELAY_METRIC_RSSI_SUM = 0,  // RSSI(s,r) + RSSI(r,d)            [dBm + dBm]
  RELAY_METRIC_RSSI_MIN,      // min(RSSI(s,r), RSSI(r,d))  (enlace gargalo)
  RELAY_METRIC_SNR_SUM,       // SNR(s,r) + SNR(r,d)   (ruído térmico + NF do PHY)
  RELAY_METRIC_DISTANCE       // -(d(s,r) + d(r,d))
};

struct LinkQuality
{
  double rssiDbm;
  double snrDb;
  double distanceM;
};

class RelaySelectionHelper
{
public:
  static void SetPropagationLossModel (Ptr<PropagationLossModel> loss) { LossModelRef () = loss; }
  static void SetMetric (RelayMetric metric) { MetricRef () = metric; }
  static void SetVerbose (bool verbose) { VerboseRef () = verbose; }
  static void Reset (void) { LossModelRef () = 0; }

  // Assinatura pedida: todos os relays são candidatos.
  static Ptr<Node>
  SelectBestRelay (NodeContainer relays, Ptr<Node> source, Ptr<Node> destination)
  {
    std::vector<bool> eligible (relays.GetN (), true);
    return SelectBestRelay (relays, source, destination, eligible);
  }

  // Variante com filtro de elegibilidade (usada na construção da árvore
  // para respeitar a capacidade de 2 filhos por relay).
  static Ptr<Node>
  SelectBestRelay (NodeContainer relays, Ptr<Node> source, Ptr<Node> destination,
                   const std::vector<bool> &eligible)
  {
    const bool verbose = VerboseRef ();
    std::cout << std::fixed << std::setprecision (2);
    if (verbose)
      {
        std::cout << "\n[RelaySelection]\n"
                  << "Source: " << RelayNodeName (source) << "\n"
                  << "Destination: " << RelayNodeName (destination) << "\n"
                  << "Metric: " << MetricName () << "\n\n";
      }

    Ptr<Node> best = 0;
    double bestScore = -std::numeric_limits<double>::infinity ();

    for (uint32_t i = 0; i < relays.GetN (); ++i)
      {
        Ptr<Node> relay = relays.Get (i);
        LinkQuality sr = GetLinkQuality (source, relay);
        LinkQuality rd = GetLinkQuality (relay, destination);
        double score = Score (sr, rd);
        bool ok = (i < eligible.size ()) && eligible[i];

        if (verbose)
          {
            std::cout << RelayNodeName (relay) << ":\n"
                      << "  RSSI source-relay      = " << sr.rssiDbm << " dBm"
                      << "  (d = " << sr.distanceM << " m, SNR = " << sr.snrDb << " dB)\n"
                      << "  RSSI relay-destination = " << rd.rssiDbm << " dBm"
                      << "  (d = " << rd.distanceM << " m, SNR = " << rd.snrDb << " dB)\n"
                      << "  Score = " << score
                      << (ok ? "" : "   [sem capacidade: ignorado]") << "\n\n";
          }

        if (ok && score > bestScore)
          {
            bestScore = score;
            best = relay;
          }
      }

    if (verbose)
      {
        std::cout << "Best Relay: " << RelayNodeName (best)
                  << "  (Score = " << bestScore << ")\n";
      }
    return best;
  }

  // Qualidade de um enlace tx -> rx, a partir do estado real do simulador
  static LinkQuality
  GetLinkQuality (Ptr<Node> tx, Ptr<Node> rx)
  {
    LinkQuality q;
    q.rssiDbm = GetRssi (tx, rx);
    q.snrDb = q.rssiDbm - GetNoiseFloorDbm (rx);
    q.distanceM = GetMobility (tx)->GetDistanceFrom (GetMobility (rx));
    return q;
  }

  static double
  Score (const LinkQuality &sr, const LinkQuality &rd)
  {
    switch (MetricRef ())
      {
      case RELAY_METRIC_RSSI_MIN:
        return std::min (sr.rssiDbm, rd.rssiDbm);
      case RELAY_METRIC_SNR_SUM:
        return sr.snrDb + rd.snrDb;
      case RELAY_METRIC_DISTANCE:
        return -(sr.distanceM + rd.distanceM);
      case RELAY_METRIC_RSSI_SUM:
      default:
        return sr.rssiDbm + rd.rssiDbm;
      }
  }

  static std::string
  MetricName (void)
  {
    switch (MetricRef ())
      {
      case RELAY_METRIC_RSSI_MIN: return "min(RSSI(s,r), RSSI(r,d))";
      case RELAY_METRIC_SNR_SUM:  return "SNR(s,r) + SNR(r,d)";
      case RELAY_METRIC_DISTANCE: return "-(d(s,r) + d(r,d))";
      default:                    return "RSSI(s,r) + RSSI(r,d)";
      }
  }

private:
  // RSSI (dBm) que o PHY de node2 mediria para um quadro transmitido por node1.
  static double
  GetRssi (Ptr<Node> node1, Ptr<Node> node2)
  {
    Ptr<PropagationLossModel> loss = LossModelRef ();
    NS_ABORT_MSG_IF (loss == 0,
                     "RelaySelectionHelper: chame SetPropagationLossModel() com o MESMO "
                     "modelo instalado no YansWifiChannel");

    Ptr<YansWifiPhy> txPhy = GetYansPhy (node1);
    Ptr<YansWifiPhy> rxPhy = GetYansPhy (node2);

    // Nível de potência 0 (= TxPowerStart), que é o nível padrão usado pelo
    // MinstrelWifiManager. Se usar NTxPower > 1 com controle de potência,
    // trocar por TxPowerStart + nível * (End-Start)/(N-1).
    double txPowerDbm = txPhy->GetTxPowerStart () + txPhy->GetTxGain ();
    double rxPowerDbm = loss->CalcRxPower (txPowerDbm, GetMobility (node1), GetMobility (node2));
    return rxPowerDbm + rxPhy->GetRxGain ();
  }

  // Ruído térmico (290 K) na largura de canal S1G configurada + figura de ruído do PHY.
  // Não inclui interferência: é uma SNR de enlace, não SINR instantânea.
  static double
  GetNoiseFloorDbm (Ptr<Node> node)
  {
    Ptr<YansWifiPhy> phy = GetYansPhy (node);
    UintegerValue widthMhz;
    phy->GetAttribute ("ChannelWidth", widthMhz);   // atributo que o cenário já usa (=2)
    double bwHz = static_cast<double> (widthMhz.Get ()) * 1e6;
    return -174.0 + 10.0 * std::log10 (bwHz) + phy->GetRxNoiseFigure ();
  }

  static Ptr<YansWifiPhy>
  GetYansPhy (Ptr<Node> node)
  {
    // Relays possuem 2 WifiNetDevices (STA + AP) com a mesma configuração de PHY;
    // o primeiro encontrado é representativo.
    for (uint32_t i = 0; i < node->GetNDevices (); ++i)
      {
        Ptr<WifiNetDevice> dev = DynamicCast<WifiNetDevice> (node->GetDevice (i));
        if (dev != 0)
          {
            Ptr<YansWifiPhy> phy = DynamicCast<YansWifiPhy> (dev->GetPhy ());
            if (phy != 0)
              {
                return phy;
              }
          }
      }
    NS_FATAL_ERROR ("RelaySelectionHelper: " << RelayNodeName (node)
                    << " não possui WifiNetDevice com YansWifiPhy");
    return 0;
  }

  static Ptr<MobilityModel>
  GetMobility (Ptr<Node> node)
  {
    Ptr<MobilityModel> m = node->GetObject<MobilityModel> ();
    NS_ABORT_MSG_IF (m == 0, "RelaySelectionHelper: " << RelayNodeName (node)
                     << " sem MobilityModel (instale a mobilidade antes da seleção)");
    return m;
  }

  static Ptr<PropagationLossModel> &LossModelRef (void) { static Ptr<PropagationLossModel> s; return s; }
  static RelayMetric &MetricRef (void) { static RelayMetric s = RELAY_METRIC_RSSI_SUM; return s; }
  static bool &VerboseRef (void) { static bool s = true; return s; }
};

} // namespace ns3

#endif /* RELAY_SELECTION_HELPER_H */
