## O que é?

O `test++` é uma cópia da `test` — pasta essa que é a base de todo o projeto — com as modificações necessárias para a criação de uma topologia árvore ( 1 AP + 1 RELAY +  1 STA).

## Quais foram as modificações realizadas?

Para adotar a topologia de árvore foi modificada apenas os arquivos `sg1-test-tim-raw.cc`  e `sg1-test-tim-raw.h` , citarei apenas as modificações menos intuitivas, de início foi alterado a parte de criação de nós.

```cpp
  // criação dos conteineres que guardaram os nós
	NodeContainer wifiRelayNode;
	NodeContainer wifiLeafNode;

	// criação dos nós
	wifiRelayNode.Create(1); // relay --> nó 0
	wifiLeafNode.Create(2); // STAs --> nós 1 e 2
	wifiApNode.Create(1); // AP --> nó 3

	// Mescla os nós relays + STAs
	wifiStaNode.Add(wifiRelayNode);
	wifiStaNode.Add(wifiLeafNode);
```

A mesclagem dos nós serve para que seja utilizado a criação de aplicações já presentes no código não necessitando novas implementações.

Seguindo é necessário a criação de dois SSIDs para permitir o fluxo STAs → Relay → AP e vice-versa.

```cpp
Ssid ssid = Ssid("ns380211ah"); // ssid do AP central
Ssid relaySsid = Ssid("ns380211ah-relay"); // ssid do relay
```

De forma consecutiva temos a atribuição dos nós aos SSIDs corretos, também vale observar que temos alterações no canal entre cada nó.

```cpp
  // ATENÇÃO: este é um código simplificado
  // channel1 -> salto AP raiz <-> STA do relay
  //channel2 -> salto AP do relay <-> leaves
  
  // conexão sta: relay --> AP
  phy.SetChannel(channel1); // ATENÇÃO
  mac.SetType("ns3::StaWifiMac", "Ssid", SsidValue(ssid));

	NetDeviceContainer staRelayDevice;
	staRelayDevice = wifi.Install(phy, mac, wifiRelayNode);

	// conexão sta: STAs --> relay
	mac.SetType("ns3::StaWifiMac", "Ssid", SsidValue(relaySsid));
	
	phy.SetChannel(channel2); // ATENÇÃO
	NetDeviceContainer leafDevice;
	leafDevice = wifi.Install(phy, mac, wifiLeafNode);

	// mesclagem das portas STAs
	NetDeviceContainer staDevice;
	staDevice.Add(staRelayDevice);
	staDevice.Add(leafDevice);

	// conexão AP: AP --> Relay
	mac.SetType ("ns3::ApWifiMac",
	                 "Ssid", SsidValue (ssid)
	               );

	phy.SetChannel(channel1); // ATENÇÃO
	apDevice = wifi.Install(phy, mac, wifiApNode);

	// conexão AP: relay --> STAs
	mac.SetType ("ns3::ApWifiMac",
	                 "Ssid", SsidValue (relaySsid)
	               );

	phy.SetChannel(channel2); // ATENÇÃO
	NetDeviceContainer apRelayDevice;
	apRelayDevice = wifi.Install(phy, mac, wifiRelayNode);
```

Procedendo temos uma pequena alteração na mobilidade que antes de os nós STAs sejam posicionados é capturado a posição do relay para que os mesmos possam ser posicionados ao seu redor.

```cpp
  // captura da posição do relay
	Ptr<MobilityModel> relayMob = wifiRelayNode.Get(0)->GetObject<MobilityModel>();
	Vector relayPos = relayMob->GetPosition();

	MobilityHelper staMobility;
	staMobility.SetPositionAllocator("ns3::UniformDiscPositionAllocator",
									"X", DoubleValue(relayPos.x),
									"Y", DoubleValue(relayPos.y),
									"rho", DoubleValue(20.0));  // raio em metros
	staMobility.SetMobilityModel("ns3::ConstantPositionMobilityModel");
	staMobility.Install(wifiLeafNode);
```

Por fim, a última alteração foi na parte de endereçamento IP.

```cpp
  // rede AP <-> Relay
	address.SetBase("192.168.1.0", "255.255.255.0");

	staNodeInterface = address.Assign(staRelayDevice);
	apNodeInterface = address.Assign(apDevice);

	// rede Relay <-> STAs
	address.SetBase("192.168.2.0", "255.255.255.0");
```

## Próximos objetivos

Os próximos objetivos estão em passos sequenciais ou seja um depende a conclusão do outro.

1. Extrair o máximo de dados possíveis através de sniffers
2. Tornar a topologia estática escalável ex: quero 2 relays com 4 filhos cada
3. Torna-lá dinâmica ex: endereçamento dinâmico + mobilidade nos nós