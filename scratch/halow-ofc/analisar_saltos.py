#!/usr/bin/env python3
"""
Entrega e atraso por número de saltos numa rodada do halow-ofc com relays, e
onde se perdem as respostas (descida) do gateway.

Cada resposta UDP echo é seguida pelos eventos IP (resultado_ip_packet_events.csv):
TX no gateway -> RX/FWD em cada relay do caminho -> RX/DELIVER no nó. A perda é
atribuída ao enlace logo depois do último nó que recebeu o pacote.

Uso: python analisar_saltos.py <pasta da rodada> [<pasta> ...]
Requer resultado_topologia.csv, resultado_flow_summary.csv,
resultado_ip_packet_events.csv e sim.log na pasta.
"""
import csv
import os
import re
import sys
from collections import Counter, defaultdict

JANELA_S = 9.0  # respostas do mesmo nó saem a cada 10 s


def carregar_topologia(pasta):
    topo = {}
    for t in csv.DictReader(open(os.path.join(pasta, "resultado_topologia.csv"), encoding="utf-8")):
        pai = t["pai"]
        p = -1 if pai == "gateway" else (int(pai.split("_")[-1]) if pai.startswith("relay") else -2)
        topo[int(t["sta_index"])] = dict(pai=p, saltos=int(t["saltos_ate_gateway"] or 0),
                                         relay=t["papel"] == "RELAY")
    return topo


def caminho_descida(topo, no, gateway_id):
    """[gateway, relay..., nó] na ordem em que a resposta percorre."""
    sobe = [no]
    p = topo[no]["pai"]
    while p >= 0:
        sobe.append(p)
        p = topo[p]["pai"]
    sobe.append(gateway_id)
    return list(reversed(sobe))


def analisar(pasta):
    topo = carregar_topologia(pasta)
    gateway_id = len(topo)  # o AP é criado depois das STAs
    nome = lambda n: "gateway" if n == gateway_id else ("R" if topo[n]["relay"] else "") + str(n)

    log = open(os.path.join(pasta, "sim.log"), encoding="latin-1").read()
    env, sub, eco = Counter(), Counter(), Counter()
    for ip in re.findall(r"client=(\S+) sent", log): env[ip] += 1
    for ip in re.findall(r"server=\S+ received \d+ bytes from (\S+) port", log): sub[ip] += 1
    for ip in re.findall(r"client=(\S+) received", log): eco[ip] += 1

    ip_do_no, atr_sub, atr_desc = {}, defaultdict(list), defaultdict(list)
    for f in csv.DictReader(open(os.path.join(pasta, "resultado_flow_summary.csv"))):
        if f["destination_port"] == "9":
            n = int(f["source_node_id"]); ip_do_no[n] = f["source_ip"]
            if f["mean_one_way_delay_s"]: atr_sub[topo[n]["saltos"]].append(float(f["mean_one_way_delay_s"]) * 1000)
        elif f["source_port"] == "9" and f["mean_one_way_delay_s"]:
            n = int(f["destination_node_id"])
            atr_desc[topo[n]["saltos"]].append(float(f["mean_one_way_delay_s"]) * 1000)

    # --- entrega por saltos
    por_saltos = defaultdict(lambda: [0, 0, 0, 0])
    for n, ip in ip_do_no.items():
        a = por_saltos[topo[n]["saltos"]]
        a[0] += 1; a[1] += env[ip]; a[2] += sub[ip]; a[3] += eco[ip]
    media = lambda v: sum(v) / len(v) if v else float("nan")
    print(f"\n=== {os.path.basename(os.path.normpath(pasta))}")
    t = [sum(a[i] for a in por_saltos.values()) for i in range(4)]
    print(f"nós com tráfego {t[0]}, enviados {t[1]}, chegaram ao gateway {100*t[2]/t[1]:.1f}%, "
          f"ida e volta {100*t[3]/t[1]:.1f}%")
    print("saltos  nós  subida   ida+volta  perda_descida  atraso_sub  atraso_desc")
    for h in sorted(por_saltos):
        a = por_saltos[h]
        perda_desc = 100 * (a[2] - a[3]) / a[2] if a[2] else float("nan")
        print(f"{h:6d} {a[0]:4d}  {100*a[2]/a[1]:6.1f}%  {100*a[3]/a[1]:8.1f}%  {perda_desc:11.1f}%"
              f"  {media(atr_sub[h]):8.0f} ms  {media(atr_desc[h]):8.0f} ms")

    # --- onde as respostas se perdem
    eventos = defaultdict(list)  # (destino ip) -> [(t, nó, evento)] das respostas
    arq = os.path.join(pasta, "resultado_ip_packet_events.csv")
    if not os.path.exists(arq):
        print("(sem resultado_ip_packet_events.csv: rode sem --MonitorPacketRecords=false para localizar perdas)")
        return
    for e in csv.DictReader(open(arq)):
        if e["source_port"] == "9" and e["ip_protocol"] == "17":
            eventos[e["destination_ip"]].append((float(e["time_s"]), int(e["node_id"]), e["event"]))
    perdas_por_salto = Counter()     # índice do enlace no caminho (1 = gateway -> 1º nó)
    perdas_por_enlace = Counter()    # (de, para)
    respostas = 0
    for n, ip in ip_do_no.items():
        ev = sorted(eventos.get(ip, []))
        cam = caminho_descida(topo, n, gateway_id)
        saidas = [x for x in ev if x[1] == gateway_id and x[2] == "TX"]
        for t0, _, _ in saidas:
            respostas += 1
            janela = [x for x in ev if t0 <= x[0] < t0 + JANELA_S]
            if any(x[1] == n and x[2] == "DELIVER" for x in janela):
                continue
            alcancados = {x[1] for x in janela if x[2] in ("RX", "FWD", "DELIVER")} | {gateway_id}
            ultimo = max(i for i, no in enumerate(cam) if no in alcancados)
            if ultimo + 1 < len(cam):
                perdas_por_salto[ultimo + 1] += 1
                perdas_por_enlace[(cam[ultimo], cam[ultimo + 1])] += 1
    total = sum(perdas_por_salto.values())
    print(f"respostas do gateway rastreadas: {respostas}, perdidas: {total}")
    if total:
        print("perdas pela posição do enlace no caminho (1 = gateway -> primeiro nó):",
              ", ".join(f"{k}: {v}" for k, v in sorted(perdas_por_salto.items())))
        destino_relay = sum(v for (a, b), v in perdas_por_enlace.items() if topo.get(b, {}).get("relay"))
        print(f"perdidas em enlace AP -> relay: {destino_relay}; AP -> nó final: {total - destino_relay}")
        print("enlaces com mais perdas:", ", ".join(f"{nome(a)}->{nome(b)}: {v}"
                                                  for (a, b), v in perdas_por_enlace.most_common(8)))


if __name__ == "__main__":
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    for p in sys.argv[1:]:
        analisar(p)
