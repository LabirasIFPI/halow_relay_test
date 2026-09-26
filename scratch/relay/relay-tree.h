/*
 * relay-tree.h
 *
 * Representação explícita da árvore Gateway -> Relay -> Node,
 * cálculo do próximo salto e geração das rotas IP estáticas
 * que fazem os Relays encaminharem os pacotes de fato.
 */
#ifndef RELAY_TREE_H
#define RELAY_TREE_H

#include "relay-selection-helper.h"
#include "ns3/internet-module.h"

#include <map>

namespace ns3 {

struct RelayNode
{
  Ptr<Node> relay;
  Ptr<Node> parent;                  // sempre o Gateway nesta topologia
  std::vector<Ptr<Node> > children;  // nós finais associados

  Ssid ssid;                         // SSID do BSS criado pelo ApWifiMac do relay
  Ipv4Address upAddress;             // IP do StaWifiMac (BSS do Gateway)
  Ipv4Address downAddress;           // IP do ApWifiMac (gateway padrão dos filhos)
  Ipv4Address subnet;                // sub-rede do BSS do relay
  Ipv4Mask mask;
};

class RelayTree
{
public:
  void SetGateway (Ptr<Node> gateway) { m_gateway = gateway; }
  Ptr<Node> GetGateway (void) const { return m_gateway; }
  void SetGatewayAddress (Ipv4Address a) { m_gatewayAddress = a; }
  Ipv4Address GetGatewayAddress (void) const { return m_gatewayAddress; }

  uint32_t
  AddRelay (Ptr<Node> relay, Ssid ssid)
  {
    RelayNode r;
    r.relay = relay;
    r.parent = m_gateway;
    r.ssid = ssid;
    m_relays.push_back (r);
    m_relayNodes.Add (relay);
    return m_relays.size () - 1;
  }

  uint32_t GetNRelays (void) const { return m_relays.size (); }
  RelayNode &GetRelay (uint32_t i) { return m_relays.at (i); }
  const RelayNode &GetRelay (uint32_t i) const { return m_relays.at (i); }
  NodeContainer GetRelayNodes (void) const { return m_relayNodes; }

  int
  FindRelay (Ptr<Node> node) const
  {
    for (uint32_t i = 0; i < m_relays.size (); ++i)
      {
        if (m_relays[i].relay == node)
          {
            return static_cast<int> (i);
          }
      }
    return -1;
  }

  int
  GetParentRelay (Ptr<Node> endNode) const
  {
    std::map<uint32_t, int>::const_iterator it = m_parentOf.find (endNode->GetId ());
    return (it == m_parentOf.end ()) ? -1 : it->second;
  }

  void SetEndNodeAddress (Ptr<Node> n, Ipv4Address a) { m_addressOf[n->GetId ()] = a; }

  Ipv4Address
  GetEndNodeAddress (Ptr<Node> n) const
  {
    std::map<uint32_t, Ipv4Address>::const_iterator it = m_addressOf.find (n->GetId ());
    NS_ABORT_MSG_IF (it == m_addressOf.end (), "RelayTree: " << RelayNodeName (n) << " sem IP");
    return it->second;
  }

  // Constrói a árvore: para cada nó final escolhe o relay pai com
  // SelectBestRelay(source = nó final, destination = Gateway),
  // considerando apenas relays que ainda têm capacidade (< maxChildren).
  void
  Build (NodeContainer endNodes, uint32_t maxChildren)
  {
    NS_ABORT_MSG_IF (endNodes.GetN () > maxChildren * m_relays.size (),
                     "RelayTree: capacidade insuficiente para todos os nós finais");

    std::cout << "\n================ CONSTRUÇÃO DA ÁRVORE (SelectBestRelay) ================\n";
    for (uint32_t i = 0; i < endNodes.GetN (); ++i)
      {
        Ptr<Node> e = endNodes.Get (i);
        std::vector<bool> eligible (m_relays.size ());
        for (uint32_t r = 0; r < m_relays.size (); ++r)
          {
            eligible[r] = m_relays[r].children.size () < maxChildren;
          }

        Ptr<Node> best = RelaySelectionHelper::SelectBestRelay (m_relayNodes, e, m_gateway, eligible);
        int idx = FindRelay (best);
        NS_ABORT_MSG_IF (idx < 0, "RelayTree: nenhum relay elegível para " << RelayNodeName (e));

        m_relays[idx].children.push_back (e);
        m_parentOf[e->GetId ()] = idx;
        std::cout << "=> " << RelayNodeName (e) << " associado a " << RelayNodeName (best) << "\n";
      }
  }

  // Próximo salto lógico de 'current' em direção a 'destination'
  Ptr<Node>
  NextHop (Ptr<Node> current, Ptr<Node> destination) const
  {
    if (current == destination)
      {
        return 0;
      }
    int dstRelay = FindRelay (destination);
    int dstParent = GetParentRelay (destination);

    if (current == m_gateway)                           // raiz: desce
      {
        if (dstRelay >= 0) return m_relays[dstRelay].relay;
        if (dstParent >= 0) return m_relays[dstParent].relay;
        return 0;
      }

    int curRelay = FindRelay (current);
    if (curRelay >= 0)                                   // relay
      {
        if (dstParent == curRelay) return destination;   // filho direto
        return m_gateway;                                // qualquer outro: sobe
      }

    int curParent = GetParentRelay (current);            // nó final: sempre ao pai
    return (curParent >= 0) ? m_relays[curParent].relay : Ptr<Node> (0);
  }

  std::vector<Ptr<Node> >
  GetPath (Ptr<Node> source, Ptr<Node> destination) const
  {
    std::vector<Ptr<Node> > path;
    Ptr<Node> cur = source;
    path.push_back (cur);
    for (uint32_t guard = 0; guard < 16 && cur != destination; ++guard)
      {
        cur = NextHop (cur, destination);
        if (cur == 0) break;
        path.push_back (cur);
      }
    return path;
  }

  std::string
  PathToString (Ptr<Node> source, Ptr<Node> destination) const
  {
    std::vector<Ptr<Node> > p = GetPath (source, destination);
    std::ostringstream oss;
    for (uint32_t i = 0; i < p.size (); ++i)
      {
        oss << (i ? " → " : "") << RelayNodeName (p[i]);
      }
    return oss.str ();
  }

  // Traduz a árvore em rotas IP estáticas (é isto que faz o relay encaminhar):
  //   nó final : default via IP do ApWifiMac do seu relay
  //   relay    : default via IP do Gateway (pela interface StaWifiMac)
  //   gateway  : 192.168.k.0/24 via IP "de cima" do relay k
  void
  InstallStaticRoutes (void) const
  {
    Ipv4StaticRoutingHelper helper;

    Ptr<Ipv4> gwIp = m_gateway->GetObject<Ipv4> ();
    int32_t gwIf = gwIp->GetInterfaceForAddress (m_gatewayAddress);
    NS_ABORT_MSG_IF (gwIf < 0, "RelayTree: IP do Gateway não encontrado");
    Ptr<Ipv4StaticRouting> gwRt = helper.GetStaticRouting (gwIp);

    for (uint32_t k = 0; k < m_relays.size (); ++k)
      {
        const RelayNode &r = m_relays[k];

        gwRt->AddNetworkRouteTo (r.subnet, r.mask, r.upAddress, gwIf);

        Ptr<Ipv4> rIp = r.relay->GetObject<Ipv4> ();
        int32_t upIf = rIp->GetInterfaceForAddress (r.upAddress);
        NS_ABORT_MSG_IF (upIf < 0, "RelayTree: interface 'up' ausente em " << RelayNodeName (r.relay));
        helper.GetStaticRouting (rIp)->SetDefaultRoute (m_gatewayAddress, upIf);

        for (uint32_t c = 0; c < r.children.size (); ++c)
          {
            Ptr<Node> child = r.children[c];
            Ptr<Ipv4> cIp = child->GetObject<Ipv4> ();
            int32_t cIf = cIp->GetInterfaceForAddress (GetEndNodeAddress (child));
            NS_ABORT_MSG_IF (cIf < 0, "RelayTree: interface ausente em " << RelayNodeName (child));
            helper.GetStaticRouting (cIp)->SetDefaultRoute (r.downAddress, cIf);
          }
      }
  }

  void
  Print (void) const
  {
    std::cout << "\n================ ÁRVORE DE RETRANSMISSÃO ================\n"
              << RelayNodeName (m_gateway) << "  (" << m_gatewayAddress << ")\n";
    for (uint32_t k = 0; k < m_relays.size (); ++k)
      {
        const RelayNode &r = m_relays[k];
        bool lastRelay = (k + 1 == m_relays.size ());
        std::cout << (lastRelay ? "└── " : "├── ") << RelayNodeName (r.relay)
                  << "  [SSID " << r.ssid << "]  up=" << r.upAddress
                  << "  down=" << r.downAddress << "\n";
        for (uint32_t c = 0; c < r.children.size (); ++c)
          {
            bool lastChild = (c + 1 == r.children.size ());
            std::map<uint32_t, Ipv4Address>::const_iterator it = m_addressOf.find (r.children[c]->GetId ());
            std::cout << (lastRelay ? "    " : "│   ") << (lastChild ? "└── " : "├── ")
                      << RelayNodeName (r.children[c]);
            if (it != m_addressOf.end ()) std::cout << "  (" << it->second << ")";
            std::cout << "\n";
          }
      }
  }

private:
  Ptr<Node> m_gateway;
  Ipv4Address m_gatewayAddress;
  std::vector<RelayNode> m_relays;
  NodeContainer m_relayNodes;
  std::map<uint32_t, int> m_parentOf;           // nodeId (final) -> índice do relay
  std::map<uint32_t, Ipv4Address> m_addressOf;  // nodeId (final) -> IP
};

} // namespace ns3

#endif /* RELAY_TREE_H */
