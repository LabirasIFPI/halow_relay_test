#!/usr/bin/env python3
"""
Entrega e atraso por número de saltos numa rodada do halow-ofc com relays.

Uso: python analisar_saltos.py <pasta da rodada> [<pasta> ...]
Requer resultado_topologia.csv, resultado_flow_summary.csv e sim.log na pasta.
"""
import csv
import os
import re
import sys
from collections import Counter, defaultdict


def carregar_topologia(pasta):
    """Lê o resultado_topologia.csv da rodada.

    Args:
        pasta: pasta da rodada.

    Returns:
        Dicionário índice da STA -> {"pai", "saltos", "relay"}. O pai vale -1
        para o gateway, o índice do relay pai, ou -2 sem cobertura.
    """
    topo = {}
    for t in csv.DictReader(open(os.path.join(pasta, "resultado_topologia.csv"), encoding="utf-8")):
        pai = t["pai"]
        p = -1 if pai == "gateway" else (int(pai.split("_")[-1]) if pai.startswith("relay") else -2)
        topo[int(t["sta_index"])] = dict(pai=p, saltos=int(t["saltos_ate_gateway"] or 0),
                                         relay=t["papel"] == "RELAY")
    return topo


def analisar(pasta):
    """Mostra a entrega e o atraso por número de saltos.

    Entrega: cruza os envios, chegadas ao gateway e ecos do sim.log com o
    número de saltos de cada nó. Atraso: média do flow_summary, por saltos.

    Args:
        pasta: pasta da rodada.
    """
    topo = carregar_topologia(pasta)

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


if __name__ == "__main__":
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    for p in sys.argv[1:]:
        analisar(p)
