/* -*-  Mode: C++; c-file-style: "gnu"; indent-tabs-mode:nil; -*- */
/*
 * Copyright (c) 2009 MIRKO BANCHI
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation;
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "s1g-test-tim-raw.h"
#include "monitor.h"
#include "topologia.h"

// Árvore (Topology=area): pai de cada STA (PAI_GATEWAY ou índice do relay pai)
// e quais STAs são relays.
std::vector<int> g_paiDe;
std::vector<bool> g_ehRelay;
std::vector<bool> g_clienteIniciado;    // cliente UDP echo já instalado?

// Diagnóstico de acesso ao canal de uma STA (--TraceAccessNode).
// Códigos: 0/1 = acesso bloqueado/liberado pelo RAW; 10/20/40 = pedido de acesso;
// 41 = há pacote e acesso liberado, mas já existe pedido pendente; 30/66 = início de RAW;
// 98 = acesso concedido enquanto bloqueado (desperdiçado); 99 = acesso concedido;
// 77 = ACK recebido; 50 = início de transmissão de dados; 51 = outra transmissão;
// 60+estado = mudança de estado do rádio (60 IDLE, 61 CCA_BUSY, 62 TX, 63 RX, 64 SWITCHING, 65 SLEEP).
std::ofstream g_traceAcesso;
void TraceAcessoEdca(double t, double codigo) { g_traceAcesso << t << "," << codigo << std::endl; }
void TraceAcessoTx(Ptr<const Packet> p) {
	WifiMacHeader h;
	bool dados = p->GetSize() >= 10 && p->Copy()->PeekHeader(h) && h.IsData();
	g_traceAcesso << Simulator::Now().GetMicroSeconds() << "," << (dados ? 50 : 51) << std::endl;
}
void TraceAcessoEstado(Time inicio, Time duracao, WifiPhy::State estado) {
	g_traceAcesso << inicio.GetMicroSeconds() << "," << 60 + (int) estado << std::endl;
}

NS_LOG_COMPONENT_DEFINE("s1g-wifi-network-tim-raw");

uint32_t AssocNum = 0;
int64_t AssocTime = 0;
uint32_t StaNum = 0;
NetDeviceContainer staDeviceCont;
const int MaxSta = 8000;

Configuration config;
Statistics stats;
SimulationEventManager eventManager;

class assoc_record {
public:
	assoc_record();
	bool GetAssoc();
	void SetAssoc(std::string context, Mac48Address address);
	void UnsetAssoc(std::string context, Mac48Address address);
	void setstaid(uint16_t id);
private:
	bool assoc;
	uint16_t staid;
};

assoc_record::assoc_record() {
	assoc = false;
	staid = 65535;
}

void assoc_record::setstaid(uint16_t id) {
	staid = id;
}

void assoc_record::SetAssoc(std::string context, Mac48Address address) {
	assoc = true;
}

void assoc_record::UnsetAssoc(std::string context, Mac48Address address) {
	assoc = false;
}

bool assoc_record::GetAssoc() {
	return assoc;
}

typedef std::vector<assoc_record *> assoc_recordVector;
assoc_recordVector assoc_vector;

uint32_t GetAssocNum() {
	AssocNum = 0;
	for (assoc_recordVector::const_iterator index = assoc_vector.begin();
			index != assoc_vector.end(); index++) {
		if ((*index)->GetAssoc()) {
			AssocNum++;
		}
	}
	return AssocNum;
}

void PopulateArpCache() {
	Ptr<ArpCache> arp = CreateObject<ArpCache>();
	arp->SetAliveTimeout(Seconds(3600 * 24 * 365));
	for (NodeList::Iterator i = NodeList::Begin(); i != NodeList::End(); ++i) {
		Ptr<Ipv4L3Protocol> ip = (*i)->GetObject<Ipv4L3Protocol>();
		NS_ASSERT(ip != 0);
		ObjectVectorValue interfaces;
		ip->GetAttribute("InterfaceList", interfaces);
		for (ObjectVectorValue::Iterator j = interfaces.Begin();
				j != interfaces.End(); j++) {
			Ptr<Ipv4Interface> ipIface =
					(j->second)->GetObject<Ipv4Interface>();
			NS_ASSERT(ipIface != 0);
			Ptr<NetDevice> device = ipIface->GetDevice();
			NS_ASSERT(device != 0);
			Mac48Address addr = Mac48Address::ConvertFrom(device->GetAddress());
			for (uint32_t k = 0; k < ipIface->GetNAddresses(); k++) {
				Ipv4Address ipAddr = ipIface->GetAddress(k).GetLocal();
				if (ipAddr == Ipv4Address::GetLoopback())
					continue;
				ArpCache::Entry * entry = arp->Add(ipAddr);
				entry->MarkWaitReply(0);
				entry->MarkAlive(addr);
				std::cout << "Arp Cache: Adding the pair (" << addr << ","
						<< ipAddr << ")" << std::endl;
			}
		}
	}
	for (NodeList::Iterator i = NodeList::Begin(); i != NodeList::End(); ++i) {
		Ptr<Ipv4L3Protocol> ip = (*i)->GetObject<Ipv4L3Protocol>();
		NS_ASSERT(ip != 0);
		ObjectVectorValue interfaces;
		ip->GetAttribute("InterfaceList", interfaces);
		for (ObjectVectorValue::Iterator j = interfaces.Begin();
				j != interfaces.End(); j++) {
			Ptr<Ipv4Interface> ipIface =
					(j->second)->GetObject<Ipv4Interface>();
			ipIface->SetAttribute("ArpCache", PointerValue(arp));
		}
	}
}

uint16_t ngroup;
uint16_t nslot;
void configureRAW(RPSVector &rpslist, string &RAWConfigFile) {
	uint16_t NRPS = 0;
	uint16_t NRAWPERBEACON = 0;
	uint16_t Value = 0;
	uint32_t page = 0;
	uint32_t aid_start = 0;
	uint32_t aid_end = 0;
	uint32_t rawinfo = 0;

	ifstream myfile(RAWConfigFile);
	//1. get info from config file

	//2. define RPS
	if (myfile.is_open()) {
		myfile >> NRPS;
		int totalNumSta = 0;
		for (uint16_t kk = 0; kk < NRPS; kk++) // number of beacons covering all raw groups
		{
			RPS *m_rps = new RPS;
			myfile >> NRAWPERBEACON;
			ngroup = NRAWPERBEACON;
			for (uint16_t i = 0; i < NRAWPERBEACON; i++) // raw groups in one beacon
			{
				//RPS *m_rps = new RPS;
				RPS::RawAssignment *m_raw = new RPS::RawAssignment;

				myfile >> Value;
				m_raw->SetRawControl(Value);  //support paged STA or not
				myfile >> Value;
				m_raw->SetSlotCrossBoundary(Value);
				myfile >> Value;
				m_raw->SetSlotFormat(Value);
				myfile >> Value;
				m_raw->SetSlotDurationCount(Value);
				myfile >> Value;
				nslot = Value;
				m_raw->SetSlotNum(Value);
				myfile >> page;
				myfile >> aid_start;
				myfile >> aid_end;
				rawinfo = (aid_end << 13) | (aid_start << 2) | page;
				m_raw->SetRawGroup(rawinfo);
				totalNumSta += aid_end - aid_start + 1;
				m_rps->SetRawAssignment(*m_raw);
				delete m_raw;
			}
			rpslist.rpsset.push_back(m_rps);
			//config.nRawGroupsPerRpsList.push_back(NRAWPERBEACON);
		}
		myfile.close();
		config.NRawSta = totalNumSta;
				/*rpslist.rpsset[rpslist.rpsset.size() - 1]->GetRawAssigmentObj(
						NRAWPERBEACON - 1).GetRawGroupAIDEnd();*/
	} else
		cout << "Unable to open RAW configuration file \n";
}

/*
pageslice element and TIM(DTIM) together accomplish page slicing.

Prior knowledge:
802.11ah support up to 8192 stations, they are constructed into: page, block,
 subblock, sta.
there are 13 bit represent the AID of stations.
 AID[11-12] represent page.
 AID[6-10] represent block.
 AID[3-5] represent subblock.
 AID[0-2] represent sta.

A TIM(DTIM) element only support one page
A Page slice element only support one page

 Concept of page slicing:
 Between two DTIM beacon, there are many TIM beacons, only allow a TIM beacon include some blocks of one page is called page slice. One TIM beacon is called a page slice.
 Page slcie element specify number of page slice between two DTIM, number of blocks in each
 page slice.
 Page slice element only appears together with DTIM.

 Details:
 Page slice element also indicates AP has buffered data for which block, if a station is in that block, the station should first sleep, then wake up at coresponding page slice(TIM beacon) which includes that block.

 When station wake up at that block, it check whether AP has data for itself. If has, keep awake to receive packets and go to sleep in the next beacon.
 */

void configurePageSlice (void)
{
    config.pageS.SetPageindex (config.pageIndex);
    config.pageS.SetPagePeriod (config.pagePeriod); //2 TIM groups between DTIMs
    config.pageS.SetPageSliceLen (config.pageSliceLength); //each TIM group has 1 block (2 blocks in 2 TIM groups)
    config.pageS.SetPageSliceCount (config.pageSliceCount);
    config.pageS.SetBlockOffset (config.blockOffset);
    config.pageS.SetTIMOffset (config.timOffset);
    //std::cout << "pageIndex=" << (int)config.pageIndex << ", pagePeriod=" << (int)config.pagePeriod << ", pageSliceLength=" << (int)config.pageSliceLength << ", pageSliceCount=" << (int)config.pageSliceCount << ", blockOffset=" << (int)config.blockOffset << ", timOffset=" << (int)config.timOffset << std::endl;
    // page 0
    // 8 TIM(page slice) for one page
    // 4 block (each page)
    // 8 page slice
    // both offset are 0
}

void configureTIM (void)
{
    config.tim.SetPageIndex (config.pageIndex);
    if (config.pageSliceCount)
    	config.tim.SetDTIMPeriod (config.pageSliceCount); // not necessarily the same
    else
    	config.tim.SetDTIMPeriod (1);

    //std::cout << "DTIM period=" << (int)config.pagePeriod << std::endl;
}

void checkRawAndTimConfiguration (void)
{
	std::cout << "Checking RAW and TIM configuration..." << std::endl;
	bool configIsCorrect = true;
	NS_ASSERT (config.rps.rpsset.size());
	// Number of page slices in a single page has to equal number of different RPS elements because
	// If #PS > #RPS, the same RPS will be used in more than 1 PS and that is wrong because
	// each PS can accommodate different AIDs (same RPS means same stations in RAWs)
    if(config.pageSliceCount)
    {
	//NS_ASSERT (config.pagePeriod == config.rps.rpsset.size());
    }
	for (uint32_t j = 0; j < config.rps.rpsset.size(); j++)
	{
		uint32_t totalRawTime = 0;
		for (uint32_t i = 0; i < config.rps.rpsset[j]->GetNumberOfRawGroups(); i++)
		{
			totalRawTime += (120 * config.rps.rpsset[j]->GetRawAssigmentObj(i).GetSlotDurationCount() + 500) * config.rps.rpsset[j]->GetRawAssigmentObj(i).GetSlotNum();
			auto aidStart = config.rps.rpsset[j]->GetRawAssigmentObj(i).GetRawGroupAIDStart();
			auto aidEnd = config.rps.rpsset[j]->GetRawAssigmentObj(i).GetRawGroupAIDEnd();
			configIsCorrect = check (aidStart, j) && check (aidEnd, j);
			// AIDs in each RPS must comply with TIM in the following way:
			// TIM0: 1-63; TIM1: 64-127; TIM2: 128-191; ...; TIM32: 1983-2047
			// If RPS that belongs to TIM0 includes other AIDs (other than range [1-63]) configuration is incorrect
			NS_ASSERT (configIsCorrect);
		}
		NS_ASSERT (totalRawTime <= config.BeaconInterval);
	}
}
// assumes each TIM has its own beacon - doesn't need to be the case as there has to be only PageSliceCount beacons between DTIMs
bool check (uint16_t aid, uint32_t index)
{
	uint8_t block = (aid >> 6 ) & 0x001f;
	NS_ASSERT (config.pageS.GetPageSliceLen() > 0);
	//uint8_t toTim = (block - config.pageS.GetBlockOffset()) % config.pageS.GetPageSliceLen();
	if (index == config.pageS.GetPageSliceCount() - 1 && config.pageS.GetPageSliceCount() != 0)
	{
		// the last page slice has 32 - the rest blocks
		return (block <= 31) && (block >= index * config.pageS.GetPageSliceLen());
	}
	else if (config.pageS.GetPageSliceCount() == 0)
		return true;

	return (block >= index * config.pageS.GetPageSliceLen()) && (block < (index + 1) * config.pageS.GetPageSliceLen());
}


void sendStatistics(bool schedule) {
	eventManager.onUpdateStatistics(stats);
	eventManager.onUpdateSlotStatistics(
			transmissionsPerTIMGroupAndSlotFromAPSinceLastInterval,
			transmissionsPerTIMGroupAndSlotFromSTASinceLastInterval);
	// reset
	std::fill(transmissionsPerTIMGroupAndSlotFromAPSinceLastInterval.begin(),
			transmissionsPerTIMGroupAndSlotFromAPSinceLastInterval.end(), 0);
	std::fill(transmissionsPerTIMGroupAndSlotFromSTASinceLastInterval.begin(),
			transmissionsPerTIMGroupAndSlotFromSTASinceLastInterval.end(), 0);

	if (schedule)
		Simulator::Schedule(Seconds(config.visualizerSamplingInterval),	&sendStatistics, true);
}

void onSTADeassociated(int i) {
	eventManager.onNodeDeassociated(*nodes[i]);
}

void updateNodesQueueLength() {
	for (uint32_t i = 0; i < config.Nsta; i++) {
		nodes[i]->UpdateQueueLength();
		stats.get(i).EDCAQueueLength = nodes[i]->queueLength;
	}
	Simulator::Schedule(Seconds(0.5), &updateNodesQueueLength);
}

void onSTAAssociated(int i) {
	cout << "Node " << std::to_string(i) << " is associated and has aid "
			<< nodes[i]->aId << endl;

	// RAW do nó: o MAC escolhe o grupo por (AID & 0x7ff) dentro de [AIDStart, AIDEnd]
	// e o slot por (AID & 0x7ff) % slotNum (ver StaWifiMac). Usa o AID real, não o índice.
	const int aid = nodes[i]->aId;
	for (int k = 0; k < (int) config.rps.rpsset.size(); k++) {
		for (int j = 0; j < (int) config.rps.rpsset[k]->GetNumberOfRawGroups(); j++) {
			auto raw = config.rps.rpsset[k]->GetRawAssigmentObj(j);
			if (raw.GetRawGroupAIDStart() <= (aid & 0x07ff)
					&& (aid & 0x07ff) <= raw.GetRawGroupAIDEnd()) {
				nodes[i]->rpsIndex = k + 1;
				nodes[i]->rawGroupNumber = j + 1;
				nodes[i]->rawSlotIndex = (aid & 0x07ff) % raw.GetSlotNum() + 1;
				RegistrarRawDoNo(wifiStaNode.Get(i)->GetId(), aid,
						k + 1, j + 1, nodes[i]->rawSlotIndex);
			}
		}
	}

	eventManager.onNodeAssociated(*nodes[i]);

	// UDP echo: cada STA começa a transmitir quando ELA associa e todos os relays
	// do caminho até o gateway também estão associados.
	// Assim um nó que nunca associa não trava os outros, e pacotes enviados
	// antes da associação não entram como perda.
	if (config.trafficType == "udpecho") {
		static bool serverConfigured = false;
		if (!serverConfigured) {
			configureUDPEchoServer();
			stats.TimeWhenEverySTAIsAssociated = Simulator::Now();
			updateNodesQueueLength();
			serverConfigured = true;
		}
		startUDPEchoClientIfReady(i);
		// um relay que associa pode completar o caminho de nós abaixo dele
		if (i < (int) g_ehRelay.size() && g_ehRelay[i])
			for (uint32_t j = 0; j < g_paiDe.size(); j++)
				startUDPEchoClientIfReady(j);
		return;
	}

	// RPS, Raw group and RAW slot assignment
	static bool trafficConfigured = false;

	if (GetAssocNum() <= config.Nsta && !trafficConfigured) {
		cout << "All " << AssocNum << " stations associated at " << Simulator::Now ().GetMicroSeconds () <<", configuring clients & server" << endl;

		// association complete, start sending packets
		stats.TimeWhenEverySTAIsAssociated = Simulator::Now();

		if (config.trafficType == "udp") {
			std::cout << "UDP" << std::endl;
			configureUDPServer();
			configureUDPClients();
		} else if (config.trafficType == "udpecho") {
			configureUDPEchoServer();
			configureUDPEchoClients();
		} else if (config.trafficType == "tcpecho") {
			configureTCPEchoServer();
			configureTCPEchoClients();
		} else if (config.trafficType == "tcppingpong") {
			configureTCPPingPongServer();
			configureTCPPingPongClients();
		} else if (config.trafficType == "tcpipcamera") {
			configureTCPIPCameraServer();
			configureTCPIPCameraClients();
		} else if (config.trafficType == "tcpfirmware") {
			configureTCPFirmwareServer();
			configureTCPFirmwareClients();
		} else if (config.trafficType == "tcpsensor") {
			configureTCPSensorServer();
			configureTCPSensorClients();
		}
		updateNodesQueueLength();
	}
	trafficConfigured = true;
}

void RpsIndexTrace(uint16_t oldValue, uint16_t newValue) {
	currentRps = newValue;
	//cout << "RPS: " << newValue << " at " << Simulator::Now().GetMicroSeconds() << endl;
}

void RawGroupTrace(uint8_t oldValue, uint8_t newValue) {
	currentRawGroup = newValue;
	//cout << "	group " << std::to_string(newValue) << " at " << Simulator::Now().GetMicroSeconds() << endl;
}

void RawSlotTrace(uint8_t oldValue, uint8_t newValue) {
	currentRawSlot = newValue;
	//cout << "		slot " << std::to_string(newValue) << " at " << Simulator::Now().GetMicroSeconds() << endl;
}

void configureNodes(NodeContainer& wifiStaNode, NetDeviceContainer& staDevice) {
	cout << "Configuring STA Node trace sources..." << endl;

	for (uint32_t i = 0; i < config.Nsta; i++) {

		cout << "Hooking up trace sources for STA " << i << endl;

		NodeEntry* n = new NodeEntry(i, &stats, wifiStaNode.Get(i),
				staDevice.Get(i));

		n->SetAssociatedCallback([ = ] {onSTAAssociated(i);});
		n->SetDeassociatedCallback([ = ] {onSTADeassociated(i);});

		nodes.push_back(n);
		// hook up Associated and Deassociated events
		Config::Connect(
				"/NodeList/" + std::to_string(i)
						+ "/DeviceList/0/$ns3::WifiNetDevice/Mac/$ns3::RegularWifiMac/$ns3::StaWifiMac/Assoc",
				MakeCallback(&NodeEntry::SetAssociation, n));
		Config::Connect(
				"/NodeList/" + std::to_string(i)
						+ "/DeviceList/0/$ns3::WifiNetDevice/Mac/$ns3::RegularWifiMac/$ns3::StaWifiMac/DeAssoc",
				MakeCallback(&NodeEntry::UnsetAssociation, n));
		Config::Connect(
				"/NodeList/" + std::to_string(i)
						+ "/DeviceList/0/$ns3::WifiNetDevice/Mac/$ns3::RegularWifiMac/$ns3::StaWifiMac/NrOfTransmissionsDuringRAWSlot",
				MakeCallback(
						&NodeEntry::OnNrOfTransmissionsDuringRAWSlotChanged,
						n));	//not implem

		//Config::Connect("/NodeList/" + std::to_string(i) + "/DeviceList/0/$ns3::WifiNetDevice/Mac/$ns3::RegularWifiMac/$ns3::StaWifiMac/S1gBeaconMissed", MakeCallback(&NodeEntry::OnS1gBeaconMissed, n));

		Config::Connect(
				"/NodeList/" + std::to_string(i)
						+ "/DeviceList/0/$ns3::WifiNetDevice/Mac/$ns3::RegularWifiMac/$ns3::StaWifiMac/PacketDropped",
				MakeCallback(&NodeEntry::OnMacPacketDropped, n));
		Config::Connect(
				"/NodeList/" + std::to_string(i)
						+ "/DeviceList/0/$ns3::WifiNetDevice/Mac/$ns3::RegularWifiMac/$ns3::StaWifiMac/Collision",
				MakeCallback(&NodeEntry::OnCollision, n));
		Config::Connect(
				"/NodeList/" + std::to_string(i)
						+ "/DeviceList/0/$ns3::WifiNetDevice/Mac/$ns3::RegularWifiMac/$ns3::StaWifiMac/TransmissionWillCrossRAWBoundary",
				MakeCallback(&NodeEntry::OnTransmissionWillCrossRAWBoundary,
						n)); //?

		// hook up TX
		Config::Connect(
				"/NodeList/" + std::to_string(i)
						+ "/DeviceList/0/$ns3::WifiNetDevice/Phy/PhyTxBegin",
				MakeCallback(&NodeEntry::OnPhyTxBegin, n));
		Config::Connect(
				"/NodeList/" + std::to_string(i)
						+ "/DeviceList/0/$ns3::WifiNetDevice/Phy/PhyTxEnd",
				MakeCallback(&NodeEntry::OnPhyTxEnd, n));
		Config::Connect(
				"/NodeList/" + std::to_string(i)
						+ "/DeviceList/0/$ns3::WifiNetDevice/Phy/PhyTxDropWithReason",
				MakeCallback(&NodeEntry::OnPhyTxDrop, n)); //?

		// hook up RX
		Config::Connect(
				"/NodeList/" + std::to_string(i)
						+ "/DeviceList/0/$ns3::WifiNetDevice/Phy/PhyRxBegin",
				MakeCallback(&NodeEntry::OnPhyRxBegin, n));
		Config::Connect(
				"/NodeList/" + std::to_string(i)
						+ "/DeviceList/0/$ns3::WifiNetDevice/Phy/PhyRxEnd",
				MakeCallback(&NodeEntry::OnPhyRxEnd, n));
		Config::Connect(
				"/NodeList/" + std::to_string(i)
						+ "/DeviceList/0/$ns3::WifiNetDevice/Phy/PhyRxDropWithReason",
				MakeCallback(&NodeEntry::OnPhyRxDrop, n));

		// hook up MAC traces
		Config::Connect(
				"/NodeList/" + std::to_string(i)
						+ "/DeviceList/0/$ns3::WifiNetDevice/RemoteStationManager/MacTxRtsFailed",
				MakeCallback(&NodeEntry::OnMacTxRtsFailed, n)); //?
		Config::Connect(
				"/NodeList/" + std::to_string(i)
						+ "/DeviceList/0/$ns3::WifiNetDevice/RemoteStationManager/MacTxDataFailed",
				MakeCallback(&NodeEntry::OnMacTxDataFailed, n));
		Config::Connect(
				"/NodeList/" + std::to_string(i)
						+ "/DeviceList/0/$ns3::WifiNetDevice/RemoteStationManager/MacTxFinalRtsFailed",
				MakeCallback(&NodeEntry::OnMacTxFinalRtsFailed, n)); //?
		Config::Connect(
				"/NodeList/" + std::to_string(i)
						+ "/DeviceList/0/$ns3::WifiNetDevice/RemoteStationManager/MacTxFinalDataFailed",
				MakeCallback(&NodeEntry::OnMacTxFinalDataFailed, n)); //?

		// hook up PHY State change
		Config::Connect(
				"/NodeList/" + std::to_string(i)
						+ "/DeviceList/0/$ns3::WifiNetDevice/Phy/State/State",
				MakeCallback(&NodeEntry::OnPhyStateChange, n));

	}
}

int getBandwidth(string dataMode) {
	if (dataMode == "MCS1_0" || dataMode == "MCS1_1" || dataMode == "MCS1_2"
			|| dataMode == "MCS1_3" || dataMode == "MCS1_4"
			|| dataMode == "MCS1_5" || dataMode == "MCS1_6"
			|| dataMode == "MCS1_7" || dataMode == "MCS1_8"
			|| dataMode == "MCS1_9" || dataMode == "MCS1_10")
		return 1;

	else if (dataMode == "MCS2_0" || dataMode == "MCS2_1"
			|| dataMode == "MCS2_2" || dataMode == "MCS2_3"
			|| dataMode == "MCS2_4" || dataMode == "MCS2_5"
			|| dataMode == "MCS2_6" || dataMode == "MCS2_7"
			|| dataMode == "MCS2_8")
		return 2;

	return 0;
}

string getWifiMode(string dataMode) {
	if (dataMode == "MCS1_0")
		return "OfdmRate300KbpsBW1MHz";
	else if (dataMode == "MCS1_1")
		return "OfdmRate600KbpsBW1MHz";
	else if (dataMode == "MCS1_2")
		return "OfdmRate900KbpsBW1MHz";
	else if (dataMode == "MCS1_3")
		return "OfdmRate1_2MbpsBW1MHz";
	else if (dataMode == "MCS1_4")
		return "OfdmRate1_8MbpsBW1MHz";
	else if (dataMode == "MCS1_5")
		return "OfdmRate2_4MbpsBW1MHz";
	else if (dataMode == "MCS1_6")
		return "OfdmRate2_7MbpsBW1MHz";
	else if (dataMode == "MCS1_7")
		return "OfdmRate3MbpsBW1MHz";
	else if (dataMode == "MCS1_8")
		return "OfdmRate3_6MbpsBW1MHz";
	else if (dataMode == "MCS1_9")
		return "OfdmRate4MbpsBW1MHz";
	else if (dataMode == "MCS1_10")
		return "OfdmRate150KbpsBW1MHz";

	else if (dataMode == "MCS2_0")
		return "OfdmRate650KbpsBW2MHz";
	else if (dataMode == "MCS2_1")
		return "OfdmRate1_3MbpsBW2MHz";
	else if (dataMode == "MCS2_2")
		return "OfdmRate1_95MbpsBW2MHz";
	else if (dataMode == "MCS2_3")
		return "OfdmRate2_6MbpsBW2MHz";
	else if (dataMode == "MCS2_4")
		return "OfdmRate3_9MbpsBW2MHz";
	else if (dataMode == "MCS2_5")
		return "OfdmRate5_2MbpsBW2MHz";
	else if (dataMode == "MCS2_6")
		return "OfdmRate5_85MbpsBW2MHz";
	else if (dataMode == "MCS2_7")
		return "OfdmRate6_5MbpsBW2MHz";
	else if (dataMode == "MCS2_8")
		return "OfdmRate7_8MbpsBW2MHz";
	return "";
}

void OnAPPhyRxDrop(std::string context, Ptr<const Packet> packet,
		DropReason reason) {
	// THIS REQUIRES PACKET METADATA ENABLE!
	auto pCopy = packet->Copy();
	auto it = pCopy->BeginItem();
	while (it.HasNext()) {

		auto item = it.Next();
		Callback<ObjectBase *> constructor = item.tid.GetConstructor();

		ObjectBase *instance = constructor();
		Chunk *chunk = dynamic_cast<Chunk *>(instance);
		chunk->Deserialize(item.current);

		if (dynamic_cast<WifiMacHeader*>(chunk)) {
			WifiMacHeader* hdr = (WifiMacHeader*) chunk;

			int staId = -1;
			if (!config.useV6) {
				for (uint32_t i = 0; i < staNodeInterface.GetN(); i++) {
					if (wifiStaNode.Get(i)->GetDevice(0)->GetAddress()
							== hdr->GetAddr2()) {
						staId = i;
						break;
					}
				}
			} else {
				for (uint32_t i = 0; i < staNodeInterface6.GetN(); i++) {
					if (wifiStaNode.Get(i)->GetDevice(0)->GetAddress()
							== hdr->GetAddr2()) {
						staId = i;
						break;
					}
				}
			}
			if (staId != -1) {
				stats.get(staId).NumberOfDropsByReasonAtAP[reason]++;
			}
			delete chunk;
			break;
		} else
			delete chunk;
	}

}

void OnAPPacketToTransmitReceived(string context, Ptr<const Packet> packet,
		Mac48Address to, bool isScheduled, bool isDuringSlotOfSTA,
		Time timeLeftInSlot) {
	int staId = -1;
	if (!config.useV6) {
		for (uint32_t i = 0; i < staNodeInterface.GetN(); i++) {
			if (wifiStaNode.Get(i)->GetDevice(0)->GetAddress() == to) {
				staId = i;
				break;
			}
		}
	} else {
		for (uint32_t i = 0; i < staNodeInterface6.GetN(); i++) {
			if (wifiStaNode.Get(i)->GetDevice(0)->GetAddress() == to) {
				staId = i;
				break;
			}
		}
	}
	if (staId != -1) {
		if (isScheduled)
			stats.get(staId).NumberOfAPScheduledPacketForNodeInNextSlot++;
		else {
			stats.get(staId).NumberOfAPSentPacketForNodeImmediately++;
			stats.get(staId).APTotalTimeRemainingWhenSendingPacketInSameSlot +=
					timeLeftInSlot;
		}
	}
}

void onChannelTransmission(Ptr<NetDevice> senderDevice, Ptr<Packet> packet) {
	int rpsIndex = currentRps - 1;
	int rawGroup = currentRawGroup - 1;
	int slotIndex = currentRawSlot - 1;
	//cout << rpsIndex << "		" << rawGroup << "		" << slotIndex << "		" << endl;

	uint64_t iSlot = slotIndex;
	if (rpsIndex > 0)
		for (int r = rpsIndex - 1; r >= 0; r--)
			for (int g = 0; g < config.rps.rpsset[r]->GetNumberOfRawGroups(); g++)
				iSlot += config.rps.rpsset[r]->GetRawAssigmentObj(g).GetSlotNum();

	if (rawGroup > 0)
		for (int i = rawGroup - 1; i >= 0; i--)
			iSlot += config.rps.rpsset[rpsIndex]->GetRawAssigmentObj(i).GetSlotNum();

	if (rpsIndex >= 0 && rawGroup >= 0 && slotIndex >= 0)
	{
		if (senderDevice->GetAddress() == apDevice.Get(0)->GetAddress())
		{
			// from AP
			transmissionsPerTIMGroupAndSlotFromAPSinceLastInterval[iSlot] += packet->GetSerializedSize();
		}
		else
		{
			// from STA
			transmissionsPerTIMGroupAndSlotFromSTASinceLastInterval[iSlot] += packet->GetSerializedSize();

		}
	}
	//std::cout << "------------- packetSerializedSize = " << packet->GetSerializedSize() << std::endl;
	//std::cout << "------------- txAP[" << iSlot <<"] = " << transmissionsPerTIMGroupAndSlotFromAPSinceLastInterval[iSlot] << std::endl;
	//std::cout << "------------- txSTA[" << iSlot <<"] = " << transmissionsPerTIMGroupAndSlotFromSTASinceLastInterval[iSlot] << std::endl;

}

// int getSTAIdFromAddress(Ipv4Address from) {
// 	int staId = -1;
// 	for (int i = 0; i < staNodeInterface.GetN(); i++) {
// 		if (staNodeInterface.GetAddress(i) == from) {
// 			staId = i;
// 			break;
// 		}
// 	}
// 	return staId;
// }

int getSTAIdFromAddress(Ipv4Address from)
{
    // Primeiro tenta no container original
    for (int i = 0; i < staNodeInterface.GetN(); i++) {
        if (staNodeInterface.GetAddress(i) == from)
            return i;
    }

    // Depois tenta no container do outro lado (se existir)
    for (int i = 0; i < staLeafInterface.GetN(); i++) {
        if (staLeafInterface.GetAddress(i) == from)
            return i;   // ← cuidado: o índice é do outro container!
    }

    return -1;
}

void udpPacketReceivedAtServer(Ptr<const Packet> packet, Address from) { //works
	//cout << "+++++++++++udpPacketReceivedAtServer" << endl;
	int staId = getSTAIdFromAddress(
			InetSocketAddress::ConvertFrom(from).GetIpv4());
	if (staId != -1)
		nodes[staId]->OnUdpPacketReceivedAtAP(packet);
	else
		cout << "*** Node could not be determined from received packet at AP "
				<< endl;
}

void tcpPacketReceivedAtServer(Ptr<const Packet> packet, Address from) {
	int staId = getSTAIdFromAddress(
			InetSocketAddress::ConvertFrom(from).GetIpv4());
	if (staId != -1)
		nodes[staId]->OnTcpPacketReceivedAtAP(packet);
	else
		cout << "*** Node could not be determined from received packet at AP "
				<< endl;
}

void tcpRetransmissionAtServer(Address to) {
	int staId = getSTAIdFromAddress(Ipv4Address::ConvertFrom(to));
	if (staId != -1)
		nodes[staId]->OnTcpRetransmissionAtAP();
	else
		cout << "*** Node could not be determined from received packet at AP "
				<< endl;
}

void tcpPacketDroppedAtServer(Address to, Ptr<Packet> packet,
		DropReason reason) {
	int staId = getSTAIdFromAddress(Ipv4Address::ConvertFrom(to));
	if (staId != -1) {
		stats.get(staId).NumberOfDropsByReasonAtAP[reason]++;
	}
}

void tcpStateChangeAtServer(TcpSocket::TcpStates_t oldState,
		TcpSocket::TcpStates_t newState, Address to) {

	int staId = getSTAIdFromAddress(
			InetSocketAddress::ConvertFrom(to).GetIpv4());
	if (staId != -1)
		nodes[staId]->OnTcpStateChangedAtAP(oldState, newState);
	else
		cout << "*** Node could not be determined from received packet at AP "
				<< endl;

	//cout << Simulator::Now().GetMicroSeconds() << " ********** TCP SERVER SOCKET STATE CHANGED FROM " << oldState << " TO " << newState << endl;
}

void tcpIPCameraDataReceivedAtServer(Address from, uint16_t nrOfBytes) {
	int staId = getSTAIdFromAddress(
			InetSocketAddress::ConvertFrom(from).GetIpv4());
	if (staId != -1)
		nodes[staId]->OnTcpIPCameraDataReceivedAtAP(nrOfBytes);
	else
		cout << "*** Node could not be determined from received packet at AP "
				<< endl;
}

void configureUDPServer() {
	UdpServerHelper myServer(9);
	serverApp = myServer.Install(wifiApNode);
	serverApp.Get(0)->TraceConnectWithoutContext("Rx",
			MakeCallback(&udpPacketReceivedAtServer));
	serverApp.Start(Seconds(0));

}

void configureUDPEchoServer() {
	UdpEchoServerHelper myServer(9);
	serverApp = myServer.Install(wifiApNode);
	serverApp.Get(0)->TraceConnectWithoutContext("Rx",
			MakeCallback(&udpPacketReceivedAtServer));
	serverApp.Start(Seconds(0));
}

void configureTCPEchoServer() {
	TcpEchoServerHelper myServer(80);
	serverApp = myServer.Install(wifiApNode);
	wireTCPServer(serverApp);
	serverApp.Start(Seconds(0));
}

void configureTCPPingPongServer() {
	// TCP ping pong is a test for the new base tcp-client and tcp-server applications
	ObjectFactory factory;
	factory.SetTypeId(TCPPingPongServer::GetTypeId());
	factory.Set("Port", UintegerValue(81));

	Ptr<Application> tcpServer = factory.Create<TCPPingPongServer>();
	wifiApNode.Get(0)->AddApplication(tcpServer);

	auto serverApp = ApplicationContainer(tcpServer);
	wireTCPServer(serverApp);
	serverApp.Start(Seconds(0));
}

void configureTCPPingPongClients() {

	ObjectFactory factory;
	factory.SetTypeId(TCPPingPongClient::GetTypeId());
	factory.Set("Interval", TimeValue(MilliSeconds(config.trafficInterval)));
	factory.Set("PacketSize", UintegerValue(config.payloadSize));

	factory.Set("RemoteAddress",
			Ipv4AddressValue(apNodeInterface.GetAddress(0)));
	factory.Set("RemotePort", UintegerValue(81));

	Ptr<UniformRandomVariable> m_rv = CreateObject<UniformRandomVariable>();

	for (uint16_t i = 0; i < config.Nsta; i++) {

		Ptr<Application> tcpClient = factory.Create<TCPPingPongClient>();
		wifiStaNode.Get(i)->AddApplication(tcpClient);
		auto clientApp = ApplicationContainer(tcpClient);
		wireTCPClient(clientApp, i);

		double random = m_rv->GetValue(0, config.trafficInterval);
		clientApp.Start(MilliSeconds(0 + random));
		//clientApp.Stop(Seconds(simulationTime + 1));
	}
}

void configureTCPIPCameraServer() {
	ObjectFactory factory;
	factory.SetTypeId(TCPIPCameraServer::GetTypeId());
	factory.Set("Port", UintegerValue(82));

	Ptr<Application> tcpServer = factory.Create<TCPIPCameraServer>();
	wifiApNode.Get(0)->AddApplication(tcpServer);

	auto serverApp = ApplicationContainer(tcpServer);
	wireTCPServer(serverApp);
	serverApp.Start(Seconds(0));
//	serverApp.Stop(Seconds(config.simulationTime));
}

void configureTCPIPCameraClients() {

	ObjectFactory factory;
	factory.SetTypeId(TCPIPCameraClient::GetTypeId());
	factory.Set("MotionPercentage",
			DoubleValue(config.ipcameraMotionPercentage));
	factory.Set("MotionDuration",
			TimeValue(Seconds(config.ipcameraMotionDuration)));
	factory.Set("DataRate", UintegerValue(config.ipcameraDataRate));

	factory.Set("PacketSize", UintegerValue(config.payloadSize));

	factory.Set("RemoteAddress",
			Ipv4AddressValue(apNodeInterface.GetAddress(0)));
	factory.Set("RemotePort", UintegerValue(82));

	Ptr<UniformRandomVariable> m_rv = CreateObject<UniformRandomVariable>();

	for (uint16_t i = 0; i < config.Nsta; i++) {

		Ptr<Application> tcpClient = factory.Create<TCPIPCameraClient>();
		wifiStaNode.Get(i)->AddApplication(tcpClient);
		auto clientApp = ApplicationContainer(tcpClient);
		wireTCPClient(clientApp, i);

		clientApp.Start(MilliSeconds(0));
		//clientApp.Stop(Seconds(config.simulationTime));
	}
}

void configureTCPFirmwareServer() {
	ObjectFactory factory;
	factory.SetTypeId(TCPFirmwareServer::GetTypeId());
	factory.Set("Port", UintegerValue(83));

	factory.Set("FirmwareSize", UintegerValue(config.firmwareSize));
	factory.Set("BlockSize", UintegerValue(config.firmwareBlockSize));
	factory.Set("NewUpdateProbability",
			DoubleValue(config.firmwareNewUpdateProbability));

	Ptr<Application> tcpServer = factory.Create<TCPFirmwareServer>();
	wifiApNode.Get(0)->AddApplication(tcpServer);

	auto serverApp = ApplicationContainer(tcpServer);
	wireTCPServer(serverApp);
	serverApp.Start(Seconds(0));
//	serverApp.Stop(Seconds(config.simulationTime));
}

void configureTCPFirmwareClients() {

	ObjectFactory factory;
	factory.SetTypeId(TCPFirmwareClient::GetTypeId());
	factory.Set("CorruptionProbability",
			DoubleValue(config.firmwareCorruptionProbability));
	factory.Set("VersionCheckInterval",
			TimeValue(MilliSeconds(config.firmwareVersionCheckInterval)));
	factory.Set("PacketSize", UintegerValue(config.payloadSize));

	factory.Set("RemoteAddress",
			Ipv4AddressValue(apNodeInterface.GetAddress(0)));
	factory.Set("RemotePort", UintegerValue(83));

	Ptr<UniformRandomVariable> m_rv = CreateObject<UniformRandomVariable>();

	for (uint16_t i = 0; i < config.Nsta; i++) {

		Ptr<Application> tcpClient = factory.Create<TCPFirmwareClient>();
		wifiStaNode.Get(i)->AddApplication(tcpClient);
		auto clientApp = ApplicationContainer(tcpClient);
		wireTCPClient(clientApp, i);

		double random = m_rv->GetValue(0, config.trafficInterval);
		clientApp.Start(MilliSeconds(0 + random));
		clientApp.Stop(Seconds(config.simulationTime));
	}
}

void configureTCPSensorServer() {
	ObjectFactory factory;
	factory.SetTypeId(TCPSensorServer::GetTypeId());
	factory.Set("Port", UintegerValue(84));

	Ptr<Application> tcpServer = factory.Create<TCPSensorServer>();
	wifiApNode.Get(0)->AddApplication(tcpServer);

	auto serverApp = ApplicationContainer(tcpServer);
	wireTCPServer(serverApp);
	serverApp.Start(Seconds(0));
//	serverApp.Stop(Seconds(config.simulationTime));
}

void configureTCPSensorClients() {

	ObjectFactory factory;
	factory.SetTypeId(TCPSensorClient::GetTypeId());

	factory.Set("Interval", TimeValue(MilliSeconds(config.trafficInterval)));
	factory.Set("PacketSize", UintegerValue(config.payloadSize));
	factory.Set("MeasurementSize", UintegerValue(config.sensorMeasurementSize));

	factory.Set("RemoteAddress",
			Ipv4AddressValue(apNodeInterface.GetAddress(0)));
	factory.Set("RemotePort", UintegerValue(84));

	Ptr<UniformRandomVariable> m_rv = CreateObject<UniformRandomVariable>();

	for (uint16_t i = 0; i < config.Nsta; i++) {

		Ptr<Application> tcpClient = factory.Create<TCPSensorClient>();
		wifiStaNode.Get(i)->AddApplication(tcpClient);
		auto clientApp = ApplicationContainer(tcpClient);
		wireTCPClient(clientApp, i);

		double random = m_rv->GetValue(0, config.trafficInterval);
		clientApp.Start(MilliSeconds(0 + random));
		clientApp.Stop(Seconds(config.simulationTime));
	}
}

void wireTCPServer(ApplicationContainer serverApp) {
	serverApp.Get(0)->TraceConnectWithoutContext("Rx",
			MakeCallback(&tcpPacketReceivedAtServer));
	serverApp.Get(0)->TraceConnectWithoutContext("Retransmission",
			MakeCallback(&tcpRetransmissionAtServer));
	serverApp.Get(0)->TraceConnectWithoutContext("PacketDropped",
			MakeCallback(&tcpPacketDroppedAtServer));
	serverApp.Get(0)->TraceConnectWithoutContext("TCPStateChanged",
			MakeCallback(&tcpStateChangeAtServer));

	if (config.trafficType == "tcpipcamera") {
		serverApp.Get(0)->TraceConnectWithoutContext("DataReceived",
				MakeCallback(&tcpIPCameraDataReceivedAtServer));
	}
}

void wireTCPClient(ApplicationContainer clientApp, int i) {

	clientApp.Get(0)->TraceConnectWithoutContext("Tx",
			MakeCallback(&NodeEntry::OnTcpPacketSent, nodes[i]));
	clientApp.Get(0)->TraceConnectWithoutContext("Rx",
			MakeCallback(&NodeEntry::OnTcpEchoPacketReceived, nodes[i]));

	clientApp.Get(0)->TraceConnectWithoutContext("CongestionWindow",
			MakeCallback(&NodeEntry::OnTcpCongestionWindowChanged, nodes[i]));
	clientApp.Get(0)->TraceConnectWithoutContext("RTO",
			MakeCallback(&NodeEntry::OnTcpRTOChanged, nodes[i]));
	clientApp.Get(0)->TraceConnectWithoutContext("RTT",
			MakeCallback(&NodeEntry::OnTcpRTTChanged, nodes[i]));
	clientApp.Get(0)->TraceConnectWithoutContext("SlowStartThreshold",
			MakeCallback(&NodeEntry::OnTcpSlowStartThresholdChanged, nodes[i]));
	clientApp.Get(0)->TraceConnectWithoutContext("EstimatedBW",
			MakeCallback(&NodeEntry::OnTcpEstimatedBWChanged, nodes[i]));

	clientApp.Get(0)->TraceConnectWithoutContext("TCPStateChanged",
			MakeCallback(&NodeEntry::OnTcpStateChanged, nodes[i]));
	clientApp.Get(0)->TraceConnectWithoutContext("Retransmission",
			MakeCallback(&NodeEntry::OnTcpRetransmission, nodes[i]));

	clientApp.Get(0)->TraceConnectWithoutContext("PacketDropped",
			MakeCallback(&NodeEntry::OnTcpPacketDropped, nodes[i]));

	if (config.trafficType == "tcpfirmware") {
		clientApp.Get(0)->TraceConnectWithoutContext("FirmwareUpdated",
				MakeCallback(&NodeEntry::OnTcpFirmwareUpdated, nodes[i]));
	} else if (config.trafficType == "tcpipcamera") {
		clientApp.Get(0)->TraceConnectWithoutContext("DataSent",
				MakeCallback(&NodeEntry::OnTcpIPCameraDataSent, nodes[i]));
		clientApp.Get(0)->TraceConnectWithoutContext("StreamStateChanged",
				MakeCallback(&NodeEntry::OnTcpIPCameraStreamStateChanged,
						nodes[i]));
	}
}

void configureTCPEchoClients() {
	TcpEchoClientHelper clientHelper(apNodeInterface.GetAddress(0), 80); //address of remote node
	clientHelper.SetAttribute("MaxPackets", UintegerValue(4294967295u));
	clientHelper.SetAttribute("Interval",
			TimeValue(MilliSeconds(config.trafficInterval)));
	//clientHelper.SetAttribute("IntervalDeviation", TimeValue(MilliSeconds(config.trafficIntervalDeviation)));
	clientHelper.SetAttribute("PacketSize", UintegerValue(config.payloadSize));

	Ptr<UniformRandomVariable> m_rv = CreateObject<UniformRandomVariable>();

	for (uint16_t i = 0; i < config.Nsta; i++) {
		ApplicationContainer clientApp = clientHelper.Install(
				wifiStaNode.Get(i));
		wireTCPClient(clientApp, i);

		double random = m_rv->GetValue(0, config.trafficInterval);
		clientApp.Start(MilliSeconds(0 + random));
		//clientApp.Stop(Seconds(simulationTime + 1));
	}
}

void configureUDPClients() {
	//Application start time
	Ptr<UniformRandomVariable> m_rv = CreateObject<UniformRandomVariable>();

	UdpClientHelper myClient(apNodeInterface.GetAddress(0), 9); //address of remote node
	myClient.SetAttribute("MaxPackets", config.maxNumberOfPackets);
	myClient.SetAttribute("PacketSize", UintegerValue(config.payloadSize));
	traffic_sta.clear();
	ifstream trafficfile(config.TrafficPath);
	if (trafficfile.is_open()) {
		uint16_t sta_id;
		float sta_traffic;
		for (uint16_t kk = 0; kk < config.Nsta; kk++) {
			trafficfile >> sta_id;
			trafficfile >> sta_traffic;
			traffic_sta.insert(std::make_pair(sta_id, sta_traffic)); //insert data
			//cout << "sta_id = " << sta_id << " sta_traffic = " << sta_traffic << "\n";
		}
		trafficfile.close();
	} else
		cout << "Unable to open traffic file \n";

	double randomStart = 0.0;
	for (std::map<uint16_t, float>::iterator it = traffic_sta.begin();
			it != traffic_sta.end(); ++it) {
		std::ostringstream intervalstr;
		intervalstr << (config.payloadSize * 8) / (it->second * 1000000);
		std::string intervalsta = intervalstr.str();

		//config.trafficInterval = UintegerValue (Time (intervalsta));

		myClient.SetAttribute("Interval", TimeValue(Time(intervalsta))); // TODO add to nodeEntry and visualize
		randomStart = m_rv->GetValue(0,
				(config.payloadSize * 8) / (it->second * 1000000));
		ApplicationContainer clientApp = myClient.Install(
				wifiStaNode.Get(it->first));
		clientApp.Get(0)->TraceConnectWithoutContext("Tx",
				MakeCallback(&NodeEntry::OnUdpPacketSent, nodes[it->first]));
		clientApp.Start(Seconds(1 + randomStart));
		//clientApp.Stop (Seconds (config.simulationTime+1)); //with this throughput is smaller
	}
	AppStartTime = Simulator::Now().GetSeconds() + 1;
	//Simulator::Stop (Seconds (config.simulationTime+1));
}

void startUDPEchoClientIfReady(int i) {
	if (g_clienteIniciado.size() < config.Nsta) g_clienteIniciado.resize(config.Nsta, false);
	if (g_clienteIniciado[i] || !nodes[i]->isAssociated) return;
	// todos os relays do caminho até o gateway precisam estar associados
	int pai = (i < (int) g_paiDe.size()) ? g_paiDe[i] : PAI_GATEWAY;
	for (uint32_t guarda = 0; pai >= 0 && guarda < g_paiDe.size(); guarda++) {
		if (!nodes[pai]->isAssociated) return;
		pai = g_paiDe[pai];
	}
	g_clienteIniciado[i] = true;

	UdpEchoClientHelper clientHelper(apNodeInterface.GetAddress(0), 9);
	clientHelper.SetAttribute("MaxPackets", UintegerValue(4294967295u));
	clientHelper.SetAttribute("Interval", TimeValue(MilliSeconds(config.trafficInterval)));
	clientHelper.SetAttribute("PacketSize", UintegerValue(config.payloadSize));

	ApplicationContainer clientApp = clientHelper.Install(wifiStaNode.Get(i));
	clientApp.Get(0)->TraceConnectWithoutContext("Tx",
			MakeCallback(&NodeEntry::OnUdpPacketSent, nodes[i]));
	clientApp.Get(0)->TraceConnectWithoutContext("Rx",
			MakeCallback(&NodeEntry::OnUdpEchoPacketReceived, nodes[i]));

	Ptr<UniformRandomVariable> m_rv = CreateObject<UniformRandomVariable>();
	double random = m_rv->GetValue(0, config.trafficInterval);
	clientApp.Start(MilliSeconds(random));
}

void configureUDPEchoClients() {
	UdpEchoClientHelper clientHelper(apNodeInterface.GetAddress(0), 9); //address of remote node
	clientHelper.SetAttribute("MaxPackets", UintegerValue(4294967295u));
	clientHelper.SetAttribute("Interval", TimeValue(MilliSeconds(config.trafficInterval)));
	//clientHelper.SetAttribute("IntervalDeviation", TimeValue(MilliSeconds(config.trafficIntervalDeviation)));
	clientHelper.SetAttribute("PacketSize", UintegerValue(config.payloadSize));

	Ptr<UniformRandomVariable> m_rv = CreateObject<UniformRandomVariable>();

	for (uint16_t i = 0; i < config.Nsta; i++) {
		ApplicationContainer clientApp = clientHelper.Install(
				wifiStaNode.Get(i));
		clientApp.Get(0)->TraceConnectWithoutContext("Tx",
				MakeCallback(&NodeEntry::OnUdpPacketSent, nodes[i]));
		clientApp.Get(0)->TraceConnectWithoutContext("Rx",
				MakeCallback(&NodeEntry::OnUdpEchoPacketReceived, nodes[i]));

		double random = m_rv->GetValue(0, config.trafficInterval);
		clientApp.Start(MilliSeconds(0 + random));
		//clientApp.Stop(Seconds(simulationTime + 1));
	}
}

Time timeIdleArray[MaxSta];
Time timeRxArray[MaxSta];
Time timeTxArray[MaxSta];
Time timeSleepArray[MaxSta];
Time timeCollisionArray[MaxSta];

Time timeIdleNotAssociated[MaxSta];
Time timeRxNotAssociated[MaxSta];
Time timeTxNotAssociated[MaxSta];
Time timeSleepNotAssociated[MaxSta];
Time timeCollisionNotAssociated[MaxSta];

double dist[MaxSta];

//it prints the information regarding the state of the device
void PhyStateTrace(std::string context, Time start, Time duration,
		enum WifiPhy::State state) {

	/*Get the number of the node from the context*/
	/*context = "/NodeList/"+strSTA+"/DeviceList/'*'/Phy/$ns3::YansWifiPhy/State/State"*/
	unsigned first = context.find("t/");
	unsigned last = context.find("/D");
	string strNew = context.substr((first + 2), (last - first - 2));

	int node = std::stoi(strNew);

	if (nodes[node]->isAssociated)
	{
		switch (state)
		{
		case WifiPhy::State::SLEEP: //Sleep
			timeSleepArray[node] = timeSleepArray[node] + duration;
			//NS_LOG_UNCOND(to_string(node + 1) + ",SLEEP," + to_string(start.GetMicroSeconds()) + " " + to_string(duration.GetMicroSeconds()));
			break;
		case WifiPhy::State::IDLE: //Idle
			timeIdleArray[node] = timeIdleArray[node] + duration;
			//NS_LOG_UNCOND(to_string(node + 1) + ",IDLE," + to_string(start.GetMicroSeconds()) + " " + to_string(duration.GetMicroSeconds()));
			break;
		case WifiPhy::State::TX: //Tx
			timeTxArray[node] = timeTxArray[node] + duration;
			//NS_LOG_UNCOND (to_string(node+1) + ",TX," + to_string(start.GetMicroSeconds()) + " " + to_string(duration.GetMicroSeconds()));
			break;
		case WifiPhy::State::RX: //Rx
			timeRxArray[node] = timeRxArray[node] + duration;
			//NS_LOG_UNCOND (to_string(node+1) + ",RX," + to_string(start.GetMicroSeconds()) + " " + to_string(duration.GetMicroSeconds()));
			break;
		case WifiPhy::State::CCA_BUSY: //CCA_BUSY
			timeCollisionArray[node] = timeCollisionArray[node] + duration;
			//NS_LOG_UNCOND (to_string(node+1) + ",CCA_BUSY," + to_string(start.GetMicroSeconds()) + " " + to_string(duration.GetMicroSeconds()));
			break;
		}
	}
	else
	{
		switch (state)
		{
		case WifiPhy::State::SLEEP: //Sleep
			timeSleepNotAssociated[node] = timeSleepNotAssociated[node] + duration;
			//NS_LOG_UNCOND(to_string(node + 1) + ",SLEEP," + to_string(start.GetMicroSeconds()) + " " + to_string(duration.GetMicroSeconds()));
			break;
		case WifiPhy::State::IDLE: //Idle
			timeIdleNotAssociated[node] = timeIdleNotAssociated[node] + duration;
			//NS_LOG_UNCOND(to_string(node + 1) + ",IDLE," + to_string(start.GetMicroSeconds()) + " " + to_string(duration.GetMicroSeconds()));
			break;
		case WifiPhy::State::TX: //Tx
			timeTxNotAssociated[node] = timeTxNotAssociated[node] + duration;
			//NS_LOG_UNCOND (to_string(node+1) + ",TX," + to_string(start.GetMicroSeconds()) + " " + to_string(duration.GetMicroSeconds()));
			break;
		case WifiPhy::State::RX: //Rx
			timeRxNotAssociated[node] = timeRxNotAssociated[node] + duration;
			//NS_LOG_UNCOND (to_string(node+1) + ",RX," + to_string(start.GetMicroSeconds()) + " " + to_string(duration.GetMicroSeconds()));
			break;
		case WifiPhy::State::CCA_BUSY: //CCA_BUSY
			timeCollisionNotAssociated[node] = timeCollisionNotAssociated[node] + duration;
			//NS_LOG_UNCOND (to_string(node+1) + ",CCA_BUSY," + to_string(start.GetMicroSeconds()) + " " + to_string(duration.GetMicroSeconds()));
			break;
		}
	}
}

int main(int argc, char *argv[]) {
	//LogComponentEnable ("UdpServer", LOG_INFO);
	 LogComponentEnable ("UdpEchoServerApplication", LOG_INFO);
	 LogComponentEnable ("UdpEchoClientApplication", LOG_INFO);

	//LogComponentEnable ("ApWifiMac", LOG_DEBUG);
	//LogComponentEnable ("StaWifiMac", LOG_DEBUG);
	//LogComponentEnable ("EdcaTxopN", LOG_DEBUG);

	bool OutputPosition = true;
	config = Configuration(argc, argv);
	Config::SetDefault("ns3::StaWifiMac::MinBeaconRssiForAssoc", DoubleValue(config.minBeaconRssiForAssoc));

	// Topologia: posições e pai de cada STA (definidos antes da simulação)
	Topologia topo;
	if (config.topology == "area") {
		topo = MontarTopologiaArea(config.areaSize, config.gridSpacing, config.numRelays, config.relayPositions);
		cout << "Topology=area: " << config.areaSize << " x " << config.areaSize << " m, espaçamento "
				<< config.gridSpacing << " m -> Nsta=" << topo.sta.size() << " (inclui os relays)" << endl;
		config.Nsta = topo.sta.size();
	} else {
		double xpos0 = std::stoi(config.rho, nullptr, 0);
		topo = MontarTopologiaGrid(config.Nsta, xpos0, xpos0, config.gridSpacing);
	}
	// Árvore: a qual AP (gateway ou relay) cada nó se liga
	CriterioEnlace criterio;
	criterio.curva = LerCurvaLqi(config.lqiCurveFile);
	criterio.lqiMinimo = config.minLqiParent;
	criterio.snrMinimo = config.minSnrParent;
	criterio.maxFilhos = config.maxChildren;
	NS_ABORT_MSG_IF(config.parentPreference != "saltos" && config.parentPreference != "sinal",
			"ParentPreference deve ser saltos ou sinal");
	criterio.preferirMenosSaltos = (config.parentPreference == "saltos");
	criterio.larguraMhz = getBandwidth(config.DataMode);
	MontarArvore(topo, criterio);
	if (!topo.relays.empty()) ImprimirResumoArvore(topo);
	g_paiDe = topo.paiInstalacao;
	g_ehRelay.assign(topo.sta.size(), false);
	for (int r : topo.relays) g_ehRelay[r] = true;
	ExportarTopologiaCsv("resultado_topologia.csv", topo);

    configureRAW(config.rps, config.RAWConfigFile);
	// config.Nsta = config.NRawSta;

	configurePageSlice ();
	configureTIM ();
	//checkRawAndTimConfiguration ();

	config.NSSFile = "results/" + config.trafficType + "_" + std::to_string(config.Nsta)
			+ "sta_" + std::to_string(config.NGroup) + "Group_"
			+ std::to_string(config.NRawSlotNum) + "slots_"
			+ std::to_string(config.payloadSize) + "payload_"
			+ std::to_string(config.totaltraffic) + "Mbps_"
			+ std::to_string(config.BeaconInterval) + "BI" + ".nss";

	stats = Statistics(config.Nsta);
	eventManager = SimulationEventManager(config.visualizerIP,
			config.visualizerPort, config.NSSFile);
	uint32_t totalRawGroups(0);
	for (int i = 0; i < config.rps.rpsset.size(); i++) {
		int nRaw = config.rps.rpsset[i]->GetNumberOfRawGroups();
		totalRawGroups += nRaw;
		//cout << "Total raw groups after rps " << i << " is " << totalRawGroups << endl;
		for (int j = 0; j < nRaw; j++) {
			config.totalRawSlots += config.rps.rpsset[i]->GetRawAssigmentObj(j).GetSlotNum();
			//cout << "Total slots after group " << j << " is " << totalRawSlots << endl;
		}

	}
	transmissionsPerTIMGroupAndSlotFromAPSinceLastInterval = vector<long>(
			config.totalRawSlots, 0);
	transmissionsPerTIMGroupAndSlotFromSTASinceLastInterval = vector<long>(
			config.totalRawSlots, 0);

	RngSeedManager::SetSeed(config.seed);
	RngSeedManager::SetRun(config.seed);

	// criação dos nós
	wifiStaNode.Create(config.Nsta); // Stas

	wifiApNode.Create(1); // AP

	YansWifiChannelHelper channelBuilder = YansWifiChannelHelper();
	channelBuilder.AddPropagationLoss("ns3::LogDistancePropagationLossModel",
			"Exponent", DoubleValue(PROP_EXPOENTE), "ReferenceLoss", DoubleValue(PROP_PERDA_REFERENCIA_DB),
			"ReferenceDistance", DoubleValue(PROP_DISTANCIA_REFERENCIA_M));
	channelBuilder.SetPropagationDelay("ns3::ConstantSpeedPropagationDelayModel");

	Ptr<YansWifiChannel> channel1 = channelBuilder.Create(); // salto compartilhado: AP raiz <-> STA dos relays

	channel1->TraceConnectWithoutContext("Transmission", MakeCallback(&onChannelTransmission));

	// Cada relay tem seu BSS em um canal próprio (como canais HaLow diferentes):
	// os BSSs não interferem entre si. canalRelay[k] = canal do relay topo.relays[k].
	std::vector<Ptr<YansWifiChannel> > canalRelay;
	for (size_t k = 0; k < topo.relays.size(); k++) {
		canalRelay.push_back(channelBuilder.Create());
		canalRelay.back()->TraceConnectWithoutContext("Transmission", MakeCallback(&onChannelTransmission));
	}
	auto canalDoAp = [&](int ap) {
		return ap == PAI_GATEWAY ? channel1 : canalRelay[topo.IndiceRelay(ap)];
	};

	YansWifiPhyHelper phy = YansWifiPhyHelper::Default();

	phy.SetErrorRateModel("ns3::YansErrorRateModel");
	phy.SetChannel(channel1);
	phy.Set("ShortGuardEnabled", BooleanValue(false));
	phy.Set("ChannelWidth", UintegerValue(getBandwidth(config.DataMode))); // changed
	phy.Set("EnergyDetectionThreshold", DoubleValue(-110.0));
	phy.Set("CcaMode1Threshold", DoubleValue(-113.0));
	phy.Set("TxGain", DoubleValue(0.0));
	phy.Set("RxGain", DoubleValue(0.0));
	phy.Set("TxPowerLevels", UintegerValue(1));
	phy.Set("TxPowerEnd", DoubleValue(0.0));
	phy.Set("TxPowerStart", DoubleValue(0.0));
	phy.Set("RxNoiseFigure", DoubleValue(6.8));
	phy.Set("LdpcEnabled", BooleanValue(true));
	phy.Set("S1g1MfieldEnabled", BooleanValue(config.S1g1MfieldEnabled));

	WifiHelper wifi = WifiHelper::Default();
	wifi.SetStandard(WIFI_PHY_STANDARD_80211ah);
	S1gWifiMacHelper mac = S1gWifiMacHelper::Default();

	Ssid ssid = Ssid("ns380211ah"); // ssid do AP central
	StringValue DataRate;
	DataRate = StringValue(getWifiMode(config.DataMode)); // changed

	wifi.SetRemoteStationManager("ns3::ConstantRateWifiManager", "DataMode",DataRate, "ControlMode", DataRate);
    //wifi.SetRemoteStationManager("ns3::ArfWifiManager");

	// conexão sta: relay --> AP
	mac.SetType("ns3::StaWifiMac", "Ssid", SsidValue(ssid), "ActiveProbing",
			BooleanValue(false));

	NetDeviceContainer staDevice;
	auto ssidDoAp = [&](int ap) {
		return ap == PAI_GATEWAY ? ssid : Ssid("ns380211ah-relay-" + std::to_string(topo.IndiceRelay(ap)));
	};
	if (topo.relays.empty()) {
		staDevice = wifi.Install(phy, mac, wifiStaNode);
	} else {
		// Instala nó a nó, na ordem dos índices: o MAC (e portanto o AID) da STA i
		// continua sendo i+1, dentro da faixa coberta pelo RAW de todos os APs.
		// No relay, esta é a interface STA (DeviceList/0), ligada ao BSS do pai.
		for (uint32_t i = 0; i < config.Nsta; i++) {
			int ap = topo.paiInstalacao[i];
			mac.SetType("ns3::StaWifiMac", "Ssid", SsidValue(ssidDoAp(ap)),
					"ActiveProbing", BooleanValue(false));
			phy.SetChannel(canalDoAp(ap));
			staDevice.Add(wifi.Install(phy, mac, wifiStaNode.Get(i)));
		}
		phy.SetChannel(channel1);
	}

	// conexão AP: AP --> Relay
	mac.SetType ("ns3::ApWifiMac",
	                 "Ssid", SsidValue (ssid),
	                 "BeaconInterval", TimeValue (MicroSeconds(config.BeaconInterval)),
	                 "NRawStations", UintegerValue (config.NRawSta),
	                 "RPSsetup", RPSVectorValue (config.rps),
	                 "PageSliceSet", pageSliceValue (config.pageS),
	                 "TIMSet", TIMValue (config.tim)
	               );

	phy.Set("TxGain", DoubleValue(3.0));
	phy.Set("RxGain", DoubleValue(3.0));
	phy.Set("TxPowerLevels", UintegerValue(1));
	phy.Set("TxPowerEnd", DoubleValue(config.apTxPowerDbm));
	phy.Set("TxPowerStart", DoubleValue(config.apTxPowerDbm));
	phy.Set("RxNoiseFigure", DoubleValue(6.8));

	apDevice = wifi.Install(phy, mac, wifiApNode);

	// Lado AP de cada relay: mesmo PHY/RAW/TIM do gateway, SSID e canal próprios.
	// Fica como DeviceList/1 do nó relay (DeviceList/0 é a STA ligada ao pai).
	// relayApDevice.Get(k) = AP do relay topo.relays[k].
	NetDeviceContainer relayApDevice;
	for (size_t k = 0; k < topo.relays.size(); k++) {
		mac.SetType ("ns3::ApWifiMac",
		                 "Ssid", SsidValue (ssidDoAp(topo.relays[k])),
		                 "BeaconInterval", TimeValue (MicroSeconds(config.BeaconInterval)),
		                 "NRawStations", UintegerValue (config.NRawSta),
		                 "RPSsetup", RPSVectorValue (config.rps),
		                 "PageSliceSet", pageSliceValue (config.pageS),
		                 "TIMSet", TIMValue (config.tim)
		               );
		phy.SetChannel(canalRelay[k]);
		relayApDevice.Add(wifi.Install(phy, mac, wifiStaNode.Get(topo.relays[k])));
	}
	phy.SetChannel(channel1);
	// Lado STA dos relays: dorme ou não (padrão do módulo: dorme como qualquer STA)
	for (int r : topo.relays)
		DynamicCast<WifiNetDevice>(staDevice.Get(r))->GetMac()->SetAttribute(
				"PowerSaveEnabled", BooleanValue(config.relayStaPowerSave));
	for (int r : topo.relays)
		DynamicCast<WifiNetDevice>(staDevice.Get(r))->GetMac()->SetAttribute(
				"RawExempt", BooleanValue(config.relayStaRawExempt));
	for (uint32_t k = 0; k < relayApDevice.GetN(); k++) {
		Ptr<WifiNetDevice> d = DynamicCast<WifiNetDevice>(relayApDevice.Get(k));
		PointerValue edca;
		d->GetMac()->GetAttribute("BE_EdcaTxopN", edca);
		Ptr<WifiMacQueue> fila = edca.Get<EdcaTxopN>()->GetEdcaQueue();
		fila->SetAttribute("MaxDelay", TimeValue(MilliSeconds(config.relayApQueueMaxDelayMs)));
		fila->SetAttribute("MaxPacketNumber", UintegerValue(config.relayApQueueMaxPackets));
	}

	Config::Set(
			"/NodeList/*/DeviceList/0/$ns3::WifiNetDevice/Mac/$ns3::RegularWifiMac/BE_EdcaTxopN/Queue/MaxPacketNumber",
			UintegerValue(10));
	Config::Set(
			"/NodeList/*/DeviceList/0/$ns3::WifiNetDevice/Mac/$ns3::RegularWifiMac/BE_EdcaTxopN/Queue/MaxDelay",
			TimeValue(NanoSeconds(6000000000000)));
	{
		// Fila de dados do gateway (por padrão os mesmos 10 pacotes de todo DeviceList/0)
		PointerValue edca;
		DynamicCast<WifiNetDevice>(apDevice.Get(0))->GetMac()->GetAttribute("BE_EdcaTxopN", edca);
		edca.Get<EdcaTxopN>()->GetEdcaQueue()->SetAttribute("MaxPacketNumber",
				UintegerValue(config.gatewayApQueueMaxPackets));
	}
	// Fila de subida do lado STA dos relays (DeviceList/0 do nó relay)
	for (int r : topo.relays) {
		PointerValue edca;
		DynamicCast<WifiNetDevice>(staDevice.Get(r))->GetMac()->GetAttribute("BE_EdcaTxopN", edca);
		edca.Get<EdcaTxopN>()->GetEdcaQueue()->SetAttribute("MaxPacketNumber",
				UintegerValue(config.relayStaQueueMaxPackets));
	}
/*
	string DataModeCamera = "OfdmRate650KbpsBW2MHz";
	StringValue aa = StringValue(DataModeCamera);
	for (uint16_t k = 0; k < config.Nsta; k++) {
		std::ostringstream APSTA;
		APSTA << k;
		std::string strAP = APSTA.str();
		Config::Set(
				"/NodeList/" + strAP
						+ "/DeviceList/0/$ns3::WifiNetDevice/RemoteStationManager/$ns3::ConstantRateWifiManager/DataMode",
				aa);
		Config::Set(
				"/NodeList/" + strAP
						+ "/DeviceList/0/$ns3::WifiNetDevice/RemoteStationManager/$ns3::ConstantRateWifiManager/ControlMode",
				aa);
		Config::Set(
				"/NodeList/" + strAP
						+ "/DeviceList/0/$ns3::WifiNetDevice/Phy/$ns3::YansWifiPhy/ChannelWidth",
				UintegerValue(2));
	}
*/
	std::ostringstream oss;
	oss << "/NodeList/" << wifiApNode.Get(0)->GetId()
			<< "/DeviceList/0/$ns3::WifiNetDevice/Mac/$ns3::RegularWifiMac/$ns3::ApWifiMac/";
	Config::ConnectWithoutContext(oss.str() + "RpsIndex", MakeCallback(&RpsIndexTrace));
	Config::ConnectWithoutContext(oss.str() + "RawGroup", MakeCallback(&RawGroupTrace));
	Config::ConnectWithoutContext(oss.str() + "RawSlot", MakeCallback(&RawSlotTrace));

	// Posições vindas da topologia (grid: gateway na célula (0,0) da matriz;
	// area: gateway no canto inferior direito da área)
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
	/*

	 MobilityHelper mobilityApCamera;
	 Ptr<ListPositionAllocator> positionAllocAp = CreateObject<ListPositionAllocator> ();
	 positionAllocAp->Add (Vector (xpos, ypos, 0.0));
	 mobilityApCamera.SetPositionAllocator (positionAllocAp);
	 mobilityApCamera.SetMobilityModel ("ns3::ConstantPositionMobilityModel");
	 mobilityApCamera.Install (wifiApNode);

	 float deltaAngle = 2* M_PI / (config.tcpipcameraEnd - config.tcpipcameraStart +1);
	 float angle = 0.0;
	 double x = 0.0;
	 double y = 0.0;

	 double Distance = 50.0;


	 Ptr<UniformRandomVariable> m_rv = CreateObject<UniformRandomVariable> ();


	 for (int i = config.tcpipcameraStart; i <= config.tcpipcameraEnd; i++)
	 {
	 x = cos(angle) * Distance + xpos;
	 y = sin(angle) * Distance + ypos;

	 MobilityHelper mobilityCamera;
	 Ptr<ListPositionAllocator> positionAllocSta = CreateObject<ListPositionAllocator> ();
	 positionAllocSta->Add(Vector(x, y, 0.0));
	 mobilityCamera.SetPositionAllocator(positionAllocSta);
	 mobilityCamera.SetMobilityModel("ns3::ConstantPositionMobilityModel");
	 mobilityCamera.Install(wifiStaNode.Get(i));
	 angle += deltaAngle;
	 }

	 */

	/* Internet stack*/
	InternetStackHelper stack;
	stack.Install(wifiApNode);
	stack.Install(wifiStaNode);

	Ipv4AddressHelper address;

	if (topo.relays.empty()) {
		address.SetBase("10.0.0.0", "255.0.0.0"); // uplink, único, compartilhado
		staNodeInterface = address.Assign(staDevice);
		apNodeInterface  = address.Assign(apDevice);
	} else {
		// Uma sub-rede por BSS: 10.1.0.0/16 (gateway) e 10.(2+k).0.0/16 (relay k).
		// staNodeInterface fica na ordem dos índices das STAs (usado em getSTAIdFromAddress).
		NS_ABORT_MSG_IF(topo.relays.size() > 250, "Endereçamento suporta até 250 relays");
		auto redeDoAp = [&](int ap) {
			int octeto = (ap == PAI_GATEWAY) ? 1 : 2 + topo.IndiceRelay(ap);
			return "10." + std::to_string(octeto) + ".0.0";
		};
		std::map<int, Ipv4AddressHelper> addrDoAp;
		addrDoAp[PAI_GATEWAY].SetBase(redeDoAp(PAI_GATEWAY).c_str(), "255.255.0.0");
		for (int r : topo.relays) addrDoAp[r].SetBase(redeDoAp(r).c_str(), "255.255.0.0");

		for (uint32_t i = 0; i < config.Nsta; i++)
			staNodeInterface.Add(addrDoAp[topo.paiInstalacao[i]].Assign(NetDeviceContainer(staDevice.Get(i))));
		apNodeInterface = addrDoAp[PAI_GATEWAY].Assign(apDevice);
		std::map<int, Ipv4Address> ipDoAp;   // endereço da interface AP de cada AP
		ipDoAp[PAI_GATEWAY] = apNodeInterface.GetAddress(0);
		for (size_t k = 0; k < topo.relays.size(); k++)
			ipDoAp[topo.relays[k]] = addrDoAp[topo.relays[k]].Assign(NetDeviceContainer(relayApDevice.Get(k))).GetAddress(0);

		// Rotas estáticas. Interface 1 = primeiro rádio Wi-Fi do nó (no relay, a STA);
		// interface 2 do relay = seu AP; interface 1 do gateway = seu AP.
		Ipv4StaticRoutingHelper srh;
		auto rotasDoAp = [&](int ap) {
			Ptr<Node> no = (ap == PAI_GATEWAY) ? wifiApNode.Get(0) : wifiStaNode.Get(ap);
			return srh.GetStaticRouting(no->GetObject<Ipv4>());
		};
		// subida: todo nó (inclusive relay) usa o AP do pai como rota padrão
		for (uint32_t i = 0; i < config.Nsta; i++)
			srh.GetStaticRouting(wifiStaNode.Get(i)->GetObject<Ipv4>())
				->SetDefaultRoute(ipDoAp[topo.paiInstalacao[i]], 1);
		// descida: cada AP no caminho até o gateway aprende a sub-rede de cada relay
		// abaixo dele, com próximo salto = o filho (relay) que leva até ela
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

	allNodes.Add(wifiStaNode);
	allNodes.Add(wifiApNode);

	if (config.traceAccessNode >= 0 && config.traceAccessNode < (int) config.Nsta) {
		g_traceAcesso.open("resultado_access_trace.csv");
		g_traceAcesso << "time_us,code" << std::endl;
		Ptr<WifiNetDevice> d = DynamicCast<WifiNetDevice>(staDevice.Get(config.traceAccessNode));
		PointerValue edca;
		d->GetMac()->GetAttribute("BE_EdcaTxopN", edca);
		edca.Get<EdcaTxopN>()->TraceConnectWithoutContext("AccessQuest_record", MakeCallback(&TraceAcessoEdca));
		d->GetPhy()->TraceConnectWithoutContext("PhyTxBegin", MakeCallback(&TraceAcessoTx));
		PointerValue estado;
		d->GetPhy()->GetAttribute("State", estado);
		estado.Get<WifiPhyStateHelper>()->TraceConnectWithoutContext("State", MakeCallback(&TraceAcessoEstado));
	}
	g_logRadioSamples = config.monitorRadioSamples;
	g_logPacketRecords = config.monitorPacketRecords;
	InstalarSniffers(allNodes, wifiApNode.Get(0)->GetId());
	for (uint32_t i = 0; i < config.Nsta; i++)
		if (topo.paiInstalacao[i] >= 0)
			DefinirPai(wifiStaNode.Get(i)->GetId(), wifiStaNode.Get(topo.paiInstalacao[i])->GetId());

	//trace association
	std::cout << "Configuring trace sources..." << std::endl;
	for (uint16_t kk = 0; kk < wifiApNode.Get(0)->GetId(); kk++) {
		std::ostringstream STA;
		STA << kk;
		std::string strSTA = STA.str();

		assoc_record *m_assocrecord = new assoc_record;
		m_assocrecord->setstaid(kk);
		Config::Connect(
				"/NodeList/" + strSTA
						+ "/DeviceList/0/$ns3::WifiNetDevice/Mac/$ns3::RegularWifiMac/$ns3::StaWifiMac/Assoc",
				MakeCallback(&assoc_record::SetAssoc, m_assocrecord));
		Config::Connect(
				"/NodeList/" + strSTA
						+ "/DeviceList/0/$ns3::WifiNetDevice/Mac/$ns3::RegularWifiMac/$ns3::StaWifiMac/DeAssoc",
				MakeCallback(&assoc_record::UnsetAssoc, m_assocrecord));
		assoc_vector.push_back(m_assocrecord);
	}

	std::cout << "Populating routing tables..." << std::endl;
	// Ipv4GlobalRoutingHelper::PopulateRoutingTables();
	std::cout << "Populating ARP cache..." << std::endl;
	PopulateArpCache();

	// configure tracing for associations & other metrics
	std::cout << "Configuring trace sinks for nodes..." << std::endl;
	configureNodes(wifiStaNode, staDevice);

	Config::Connect(
			"/NodeList/" + std::to_string(wifiStaNode.GetN())
					+ "/DeviceList/0/$ns3::WifiNetDevice/Phy/PhyRxDropWithReason",
			MakeCallback(&OnAPPhyRxDrop));
	Config::Connect(
			"/NodeList/" + std::to_string(wifiStaNode.GetN())
					+ "/DeviceList/0/$ns3::WifiNetDevice/Mac/$ns3::ApWifiMac/PacketToTransmitReceivedFromUpperLayer",
			MakeCallback(&OnAPPacketToTransmitReceived));

	Ptr<MobilityModel> mobility1 =
			wifiApNode.Get(0)->GetObject<MobilityModel>();
	Vector apposition = mobility1->GetPosition();
	if (OutputPosition) {
		uint32_t i = 0;
		while (i < wifiStaNode.GetN()) {
			Ptr<MobilityModel> mobility = wifiStaNode.Get(i)->GetObject<
					MobilityModel>();
			Vector position = mobility->GetPosition();
			nodes[i]->x = position.x;
			nodes[i]->y = position.y;
			std::cout << "Sta node#" << i << ", " << "position = " << position
					<< std::endl;
			dist[i] = mobility->GetDistanceFrom(
					wifiApNode.Get(0)->GetObject<MobilityModel>());
			i++;
		}
		std::cout << "AP node, position = " << apposition << std::endl;
	}

	/*Print of the state of the stations*/
	for (uint32_t i = 0; i < wifiStaNode.GetN(); i++) {
		std::ostringstream STA;
		STA << i;
		std::string strSTA = STA.str();

		Config::Connect(
				"/NodeList/" + strSTA
						+ "/DeviceList/*/Phy/$ns3::YansWifiPhy/State/State",
				MakeCallback(&PhyStateTrace));
	}

	eventManager.onStartHeader();
	eventManager.onStart(config);
	if (config.rps.rpsset.size() > 0)
		for (uint32_t i = 0; i < config.rps.rpsset.size(); i++)
			for (uint32_t j = 0;
					j < config.rps.rpsset[i]->GetNumberOfRawGroups(); j++)
				eventManager.onRawConfig(i, j,
						config.rps.rpsset[i]->GetRawAssigmentObj(j));

	for (uint32_t i = 0; i < wifiStaNode.GetN(); i++)
		eventManager.onSTANodeCreated(*nodes[i]);

	eventManager.onAPNodeCreated(apposition.x, apposition.y);
	eventManager.onStatisticsHeader();

	sendStatistics(true);

	Simulator::Stop(Seconds(config.simulationTime + config.CoolDownPeriod)); // allow up to a minute after the client & server apps are finished to process the queue
	Simulator::Run();

	ExportarTudo("resultado", config.simulationTime);
	LimparSniffers();

	// Visualizer throughput
	int pay = 0, totalSuccessfulPackets = 0, totalSentPackets = 0, totalPacketsEchoed = 0;
	for (int i = 0; i < wifiStaNode.GetN(); i++)
	{
		totalSuccessfulPackets += stats.get(i).NumberOfSuccessfulPackets;
		totalSentPackets += stats.get(i).NumberOfSentPackets;
		totalPacketsEchoed += stats.get(i).NumberOfSuccessfulRoundtripPackets;
		pay += stats.get(i).TotalPacketPayloadSize;
		cout << i << " sent: " << stats.get(i).NumberOfSentPackets
				<< " ; delivered: " << stats.get(i).NumberOfSuccessfulPackets
				<< " ; echoed: " << stats.get(i).NumberOfSuccessfulRoundtripPackets
				<< "; packetloss: "
				<< stats.get(i).GetPacketLoss(config.trafficType) << endl;
	}

	if (config.trafficType == "udp")
	{
		double throughput = 0;
		uint32_t totalPacketsThrough =
				DynamicCast<UdpServer>(serverApp.Get(0))->GetReceived();
		throughput = totalPacketsThrough * config.payloadSize * 8
				/ (config.simulationTime * 1000000.0);
		cout << "totalPacketsThrough " << totalPacketsThrough << " ++my "
				<< totalSuccessfulPackets << endl;
		cout << "throughput " << throughput << " ++my "
				<< pay * 8. / (config.simulationTime * 1000000.0) << endl;
		std::cout << "datarate" << "\t" << "throughput" << std::endl;
		std::cout << config.datarate << "\t" << throughput << " Mbit/s"
				<< std::endl;

	}
	else if (config.trafficType == "udpecho")
	{
		double ulThroughput = 0, dlThroughput = 0;
		ulThroughput = totalSuccessfulPackets * config.payloadSize * 8 / (config.simulationTime * 1000000.0);
		dlThroughput = totalPacketsEchoed * config.payloadSize * 8 / (config.simulationTime * 1000000.0);
		cout << "totalPacketsSent " << totalSentPackets << endl;
		cout << "totalPacketsDelivered " << totalSuccessfulPackets << endl;
		cout << "totalPacketsEchoed " << totalPacketsEchoed << endl;
		cout << "UL packets lost " << totalSentPackets - totalSuccessfulPackets << endl;
		cout << "DL packets lost " << totalSuccessfulPackets - totalPacketsEchoed << endl;
		cout << "Total packets lost " << totalSentPackets - totalPacketsEchoed << endl;

		/*cout << "uplink throughput Mbit/s " << ulThroughput << endl;
		cout << "downlink throughput Mbit/s " << dlThroughput << endl;*/

		double throughput = (totalSuccessfulPackets + totalPacketsEchoed) * config.payloadSize * 8 / (config.simulationTime * 1000000.0);
		cout << "total throughput Kbit/s " << throughput * 1000 << endl;

		std::cout << "datarate" << "\t" << "throughput" << std::endl;
		std::cout << config.datarate << "\t" << throughput * 1000 << " Kbit/s" << std::endl;
	}
	cout << "total packet loss % "
			<< 100 - 100. * totalPacketsEchoed / totalSentPackets << endl;
	Simulator::Destroy();

	ofstream risultati;
	string addressresults = config.OutputPath + "moreinfo.txt";
	risultati.open(addressresults.c_str(), ios::out | ios::trunc);

    risultati << "Sta node#,distance,timerx(notassociated),timeidle(notassociated),timetx(notassociated),timesleep(notassociated),timecollision(notassociated)" << std::endl;
    int i = 0;
    string spazio = ",";
    
    while (i < wifiStaNode.GetN()) {
        
        risultati << i << spazio << dist[i] << spazio << timeRxArray[i].GetSeconds() << ",(" << timeRxNotAssociated[i].GetSeconds() << ")," << timeIdleArray[i].GetSeconds() << ",(" << timeIdleNotAssociated[i].GetSeconds() << ")," << timeTxArray[i].GetSeconds() << ",(" << timeTxNotAssociated[i].GetSeconds() << ")," << timeSleepArray[i].GetSeconds() << ",(" << timeSleepNotAssociated[i].GetSeconds() << ")," << timeCollisionArray[i].GetSeconds() << ",(" << timeCollisionNotAssociated[i].GetSeconds() << ")" << std::endl;
        /*
         cout << "================== Sleep " << stats.get(i).TotalSleepTime.GetSeconds() << endl;
         cout << "================== Tx " << stats.get(i).TotalTxTime.GetSeconds() << endl;
         cout << "================== Rx " << stats.get(i).TotalRxTime.GetSeconds() << endl;
         cout << "+++++++++++++++++++IDLE " << stats.get(i).TotalIdleTime.GetSeconds() << endl;
         cout << "ooooooooooooooooooo TOTENERGY " <<  stats.get(i).GetTotalEnergyConsumption() << " mW" << endl;
         cout << "Rx+Idle ENERGY " <<  stats.get(i).EnergyRxIdle << " mW" << endl;
         cout << "Tx ENERGY " <<  stats.get(i).EnergyTx << " mW" << endl;*/
        
        i++;
    }

	risultati.close();
	return 0;
}