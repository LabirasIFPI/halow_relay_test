/*
 * relay-path-tracer.h
 *
 * Prova que os pacotes passam pelos Relays, usando trace sources do
 * Ipv4L3Protocol (existentes no ns-3.23):
 *   "SendOutgoing"   (origem)      -> início do caminho
 *   "Rx"             (cada salto)  -> imprime  "anterior → atual"
 *   "UnicastForward" (relay/GW)    -> confirma que o nó ENCAMINHOU
 *   "LocalDeliver"   (destino)     -> fecha e registra o caminho completo
 *
 * Se algum nome de trace não existir no seu fork, TraceConnect retorna false
 * e o programa aborta com mensagem, em vez de falhar silenciosamente.
 */
#ifndef RELAY_PATH_TRACER_H
#define RELAY_PATH_TRACER_H

#include "relay-selection-helper.h"
#include "ns3/internet-module.h"

#include <map>

namespace ns3 {

class RelayPathTracer
{
public:
  explicit RelayPathTracer (uint32_t maxPacketsPerFlow = 3) : m_maxPerFlow (maxPacketsPerFlow) {}

  void
  Install (NodeContainer nodes)
  {
    for (uint32_t i = 0; i < nodes.GetN (); ++i)
      {
        Ptr<Node> node = nodes.Get (i);
        std::string name = RelayNodeName (node);
        Ptr<Ipv4L3Protocol> ip = node->GetObject<Ipv4L3Protocol> ();
        NS_ABORT_MSG_IF (ip == 0, "RelayPathTracer: " << name << " sem pilha IPv4");

        for (uint32_t j = 1; j < ip->GetNInterfaces (); ++j)    // 0 = loopback
          {
            for (uint32_t a = 0; a < ip->GetNAddresses (j); ++a)
              {
                m_nameOf[ip->GetAddress (j, a).GetLocal ()] = name;
              }
          }

        bool ok = true;
        ok = ip->TraceConnect ("SendOutgoing", name, MakeCallback (&RelayPathTracer::OnSend, this)) && ok;
        ok = ip->TraceConnect ("Rx", name, MakeCallback (&RelayPathTracer::OnRx, this)) && ok;
        ok = ip->TraceConnect ("UnicastForward", name, MakeCallback (&RelayPathTracer::OnForward, this)) && ok;
        ok = ip->TraceConnect ("LocalDeliver", name, MakeCallback (&RelayPathTracer::OnDeliver, this)) && ok;
        NS_ABORT_MSG_IF (!ok, "RelayPathTracer: trace source IPv4 ausente nesta versão do ns-3");
      }
  }

  std::string
  NameOf (Ipv4Address a) const
  {
    std::map<Ipv4Address, std::string>::const_iterator it = m_nameOf.find (a);
    if (it != m_nameOf.end ()) return it->second;
    std::ostringstream oss;
    oss << a;
    return oss.str ();
  }

  void
  PrintSummary (void) const
  {
    std::cout << "\n================ RESUMO DO ENCAMINHAMENTO ================\n"
              << "Pacotes UDP encaminhados (UnicastForward) por nó:\n";
    for (std::map<std::string, uint32_t>::const_iterator it = m_forwarded.begin ();
         it != m_forwarded.end (); ++it)
      {
        std::cout << "  " << it->first << ": " << it->second << "\n";
      }
    std::cout << "\nCaminhos observados (amostra de " << m_maxPerFlow << " pacotes por fluxo):\n";
    for (std::map<std::string, std::map<std::string, uint32_t> >::const_iterator f = m_observed.begin ();
         f != m_observed.end (); ++f)
      {
        std::cout << "  " << f->first << "\n";
        for (std::map<std::string, uint32_t>::const_iterator p = f->second.begin ();
             p != f->second.end (); ++p)
          {
            std::cout << "      " << p->first << "   (x" << p->second << ")\n";
          }
      }
  }

private:
  struct Track
  {
    std::vector<std::string> path;
  };

  static bool
  IsDataPacket (const Ipv4Header &h)
  {
    return h.GetProtocol () == 17 /* UDP */ &&
           !h.GetDestination ().IsBroadcast () && !h.GetDestination ().IsMulticast ();
  }

  std::string
  FlowName (const Ipv4Header &h) const
  {
    return NameOf (h.GetSource ()) + " → " + NameOf (h.GetDestination ());
  }

  // O eco UDP reaproveita o mesmo objeto Packet (mesmo uid) na resposta,
  // por isso a chave inclui origem e destino.
  static std::string
  PacketKey (uint64_t uid, const Ipv4Header &h)
  {
    std::ostringstream oss;
    oss << uid << "|" << h.GetSource () << "|" << h.GetDestination ();
    return oss.str ();
  }

  void
  OnSend (std::string node, const Ipv4Header &h, Ptr<const Packet> p, uint32_t)
  {
    if (!IsDataPacket (h)) return;
    std::string flow = FlowName (h);
    if (m_flowCount[flow] >= m_maxPerFlow) return;
    m_flowCount[flow]++;

    Track &t = m_tracks[PacketKey (p->GetUid (), h)];
    t.path.clear ();
    t.path.push_back (node);
    std::cout << "\n[Path] t=" << std::setprecision (4) << Simulator::Now ().GetSeconds ()
              << "s uid=" << p->GetUid () << "  " << flow
              << "  (" << h.GetSource () << " → " << h.GetDestination () << ")\n";
  }

  void
  OnRx (std::string node, Ptr<const Packet> p, Ptr<Ipv4>, uint32_t)
  {
    Ipv4Header h;
    if (p->PeekHeader (h) == 0 || !IsDataPacket (h)) return;
    std::map<std::string, Track>::iterator it = m_tracks.find (PacketKey (p->GetUid (), h));
    if (it == m_tracks.end ()) return;
    const std::string prev = it->second.path.back ();
    if (prev == node) return;                       // duplicata de MAC
    std::cout << "  " << prev << " → " << node << "\n";
    it->second.path.push_back (node);
  }

  void
  OnForward (std::string node, const Ipv4Header &h, Ptr<const Packet> p, uint32_t)
  {
    if (!IsDataPacket (h)) return;
    m_forwarded[node]++;
    if (m_tracks.count (PacketKey (p->GetUid (), h)))
      {
        std::cout << "      (" << node << " encaminhou: UnicastForward)\n";
      }
  }

  void
  OnDeliver (std::string node, const Ipv4Header &h, Ptr<const Packet> p, uint32_t)
  {
    if (!IsDataPacket (h)) return;
    std::map<std::string, Track>::iterator it = m_tracks.find (PacketKey (p->GetUid (), h));
    if (it == m_tracks.end ()) return;
    std::ostringstream path;
    for (uint32_t i = 0; i < it->second.path.size (); ++i)
      {
        path << (i ? " → " : "") << it->second.path[i];
      }
    std::cout << "  ✓ entregue em " << node << "  | Caminho: " << path.str () << "\n";
    m_observed[FlowName (h)][path.str ()]++;
    m_tracks.erase (it);
  }

  uint32_t m_maxPerFlow;
  std::map<Ipv4Address, std::string> m_nameOf;
  std::map<std::string, uint32_t> m_flowCount;
  std::map<std::string, Track> m_tracks;
  std::map<std::string, uint32_t> m_forwarded;
  std::map<std::string, std::map<std::string, uint32_t> > m_observed;
};

} // namespace ns3

#endif /* RELAY_PATH_TRACER_H */
