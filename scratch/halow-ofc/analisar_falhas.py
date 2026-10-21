#!/usr/bin/env python3
"""
Por que quadros de dados não são entregues nos enlaces filho <-> pai.

Lê resultado_link_failures.csv (monitor.h) e resultado_topologia.csv e agrupa,
por sentido e tipo de enlace (gateway->STA, gateway->relay, relay->relay,
relay->STA), as tentativas de transmissão e os motivos de falha:
  - mac_gave_up: quadros abandonados pelo transmissor após esgotar as retransmissões;
  - phy_*: tentativas que o rádio do receptor descartou (dormindo, ocupado
    recebendo outro quadro, transmitindo, erro de decodificação, sinal fraco).

Uso: python analisar_falhas.py <pasta da rodada> [<pasta> ...]
"""
import csv
import os
import sys
from collections import defaultdict

MOTIVOS = [("phy_drop_sleeping", "dormindo"), ("phy_drop_busy_receiving", "recebendo outro"),
           ("phy_drop_busy_transmitting", "transmitindo"), ("phy_drop_decode_error", "erro decodif."),
           ("phy_drop_weak_signal", "sinal fraco"), ("phy_drop_other", "outros")]


def analisar(pasta):
    relays = set()
    for t in csv.DictReader(open(os.path.join(pasta, "resultado_topologia.csv"), encoding="utf-8")):
        if t["papel"] == "RELAY":
            relays.add(int(t["sta_index"]))
    agg = defaultdict(lambda: defaultdict(int))
    for r in csv.DictReader(open(os.path.join(pasta, "resultado_link_failures.csv"))):
        filho, pai = int(r["node_id"]), int(r["parent_node_id"])
        tipo_pai = "relay" if pai in relays else "gateway"
        tipo_filho = "relay" if filho in relays else "STA"
        if r["direction"] == "downlink":
            chave = ("descida", f"{tipo_pai} -> {tipo_filho}")
        else:
            chave = ("subida", f"{tipo_filho} -> {tipo_pai}")
        a = agg[chave]
        a["enlaces"] += 1
        for c in ("frames_tx", "frames_rx", "mac_gave_up") + tuple(m for m, _ in MOTIVOS):
            a[c] += int(r[c])

    print(f"\n=== {os.path.basename(os.path.normpath(pasta))}")
    cab = f"{'sentido':<8}{'enlace':<17}{'enl.':>5}{'tentativas':>11}{'entregues':>10}{'abandonados':>12}  " + \
          "  ".join(f"{n:>14}" for _, n in MOTIVOS)
    print(cab)
    for (sentido, tipo), a in sorted(agg.items()):
        tx = a["frames_tx"]
        p = lambda v: f"{v:6d} ({100*v/tx:4.1f}%)" if tx else f"{v:6d}"
        print(f"{sentido:<8}{tipo:<17}{a['enlaces']:>5}{tx:>11}{p(a['frames_rx']):>10}  {p(a['mac_gave_up']):>12}  " +
              "  ".join(f"{p(a[m]):>14}" for m, _ in MOTIVOS))


if __name__ == "__main__":
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    for p in sys.argv[1:]:
        analisar(p)
