#!/usr/bin/env python3
"""
Calibra um LQI (0-100) a partir do PDR por enlace medido no ns-3.


"""
import argparse
import csv
import os

SNR_PATAMAR_DB = 3.0
SNR_ZONA_RELEVANTE_DB = -3.0


def distancia(r):
    """Distância do enlace filho -> pai, em metros.

    Args:
        r: linha do link_frame_summary.
    """
    return float(r.get("distance_to_parent_m") or r["distance_to_central_ap_m"])


def ler(caminho):
    """Lê um link_frame_summary, só com enlaces que têm SNR e quadros enviados.

    Args:
        caminho: caminho do CSV.

    Returns:
        Lista de linhas do CSV.
    """
    with open(caminho, newline="") as f:
        return [r for r in csv.DictReader(f) if r["ul_snr_phy_db"] and r["ul_frames_tx"] != "0"]


def agrupar(linhas):
    """Soma os quadros dos enlaces com o mesmo SNR.

    Args:
        linhas: enlaces lidos por ler().

    Returns:
        SNR -> (quadros enviados, quadros recebidos).
    """
    grupos = {}
    for r in linhas:
        snr = round(float(r["ul_snr_phy_db"]), 3)
        tx, rx = grupos.get(snr, (0, 0))
        grupos[snr] = (tx + int(r["ul_frames_tx"]), rx + int(r["ul_frames_rx_at_ap"]))
    return grupos


def patamar_de(grupos):
    """Entrega média dos enlaces com SNR >= SNR_PATAMAR_DB (o 100% do LQI).

    Args:
        grupos: resultado de agrupar().

    Returns:
        PDR do patamar, ou None se não houver enlaces bons.
    """
    tx = sum(t for s, (t, _) in grupos.items() if s >= SNR_PATAMAR_DB)
    rx = sum(r for s, (_, r) in grupos.items() if s >= SNR_PATAMAR_DB)
    return rx / tx if tx else None


def montar_curva(grupos):
    """Monta a curva SNR -> PDR que nunca desce (regressão isotônica).

    Args:
        grupos: resultado de agrupar().

    Returns:
        Pontos (SNR, PDR) em ordem crescente de SNR.
    """
    pontos = sorted((snr, rx / tx, tx) for snr, (tx, rx) in grupos.items())
    blocos = []
    for snr, pdr, peso in pontos:
        blocos.append([snr, snr, pdr, peso])
        while len(blocos) > 1 and blocos[-2][2] > blocos[-1][2]:
            b = blocos.pop()
            a = blocos[-1]
            a[2] = (a[2] * a[3] + b[2] * b[3]) / (a[3] + b[3])
            a[1], a[3] = b[1], a[3] + b[3]
    curva = []
    for smin, smax, pdr, _ in blocos:
        curva.append((smin, pdr))
        if smax != smin:
            curva.append((smax, pdr))
    return curva


def avaliar(curva, snr):
    """PDR da curva para um SNR, com interpolação linear (igual ao tree/lqi.h).

    Args:
        curva: pontos de montar_curva().
        snr: SNR de uplink (dB).
    """
    if snr <= curva[0][0]:
        return curva[0][1]
    if snr >= curva[-1][0]:
        return curva[-1][1]
    for (s0, p0), (s1, p1) in zip(curva, curva[1:]):
        if s0 <= snr <= s1:
            return p0 if s1 == s0 else p0 + (p1 - p0) * (snr - s0) / (s1 - s0)


def erro_medio(resultado, chave, filtro=lambda r: True):
    """Erro médio absoluto entre uma coluna de LQI e o PDR medido normalizado.

    Args:
        resultado: linhas de comparar().
        chave: coluna de LQI.
        filtro: quais linhas entram.

    Returns:
        (erro médio, número de enlaces).
    """
    pares = [(r[chave], r["pdr_medido_normalizado_0_100"]) for r in resultado
             if r[chave] != "" and filtro(r)]
    if not pares:
        return None, 0
    return sum(abs(a - b) for a, b in pares) / len(pares), len(pares)


def comparar(curva, patamar, caminho_teste, saida):
    """Compara o LQI da curva com o PDR medido numa rodada de teste.

    Args:
        curva: pontos de montar_curva().
        patamar: patamar da calibração.
        caminho_teste: link_frame_summary da rodada de teste.
        saida: CSV de comparação a gravar.

    Returns:
        (resumo com os erros, linhas por enlace).
    """
    teste = ler(caminho_teste)
    patamar_teste = patamar_de(agrupar(teste)) or 1.0
    resultado = []
    for r in sorted(teste, key=distancia):
        snr = float(r["ul_snr_phy_db"])
        tx, rx = int(r["ul_frames_tx"]), int(r["ul_frames_rx_at_ap"])
        pdr = rx / tx
        resultado.append({
            "node_id": r["node_id"],
            "distancia_m": round(distancia(r), 1),
            "pai": r.get("parent_role", "AP"),
            "snr_uplink_db": round(snr, 2),
            "origem_snr": r["ul_snr_source"],
            "quadros_enviados": tx,
            "quadros_recebidos_ap": rx,
            "pdr_medido": round(pdr, 3),
            "pdr_medido_normalizado_0_100": round(100 * min(1.0, pdr / patamar_teste), 1),
            "lqi_calibrado_pdr_0_100": round(100 * min(1.0, avaliar(curva, snr) / patamar), 1),
        })
    with open(saida, "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=list(resultado[0].keys()))
        w.writeheader()
        w.writerows(resultado)

    relevante = lambda r: r["snr_uplink_db"] >= SNR_ZONA_RELEVANTE_DB
    resumo = {"teste": caminho_teste, "enlaces": len(resultado),
              "enlaces_zona_relevante": sum(1 for r in resultado if relevante(r)),
              "patamar_pdr_teste": round(patamar_teste, 3),
              "quadros_enviados_total": sum(r["quadros_enviados"] for r in resultado)}
    e_all, n_all = erro_medio(resultado, "lqi_calibrado_pdr_0_100")
    e_rel, n_rel = erro_medio(resultado, "lqi_calibrado_pdr_0_100", relevante)
    resumo["erro_calibrado_todos"] = round(e_all, 1) if e_all is not None else ""
    resumo["n_calibrado_todos"] = n_all
    resumo["erro_calibrado_zona_relevante"] = round(e_rel, 1) if e_rel is not None else ""
    resumo["n_calibrado_zona_relevante"] = n_rel
    return resumo, resultado


def main():
    """Grava a curva_lqi.csv e compara a curva com cada rodada de teste."""
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--calibracao", nargs="+", required=True)
    ap.add_argument("--teste", nargs="+", required=True)
    ap.add_argument("--saida", default=".")
    args = ap.parse_args()
    os.makedirs(args.saida, exist_ok=True)

    linhas = []
    for c in args.calibracao:
        linhas += ler(c)
    grupos = agrupar(linhas)
    curva = montar_curva(grupos)
    patamar = patamar_de(grupos) or 1.0

    print(f"Calibração: {len(args.calibracao)} rodada(s), {len(linhas)} enlaces, "
          f"{sum(t for t, _ in grupos.values())} quadros; patamar de PDR = {patamar:.3f}")
    with open(os.path.join(args.saida, "curva_lqi.csv"), "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["snr_uplink_db", "pdr", "lqi_0_100"])
        for snr, pdr in curva:
            w.writerow([snr, round(pdr, 4), round(100 * min(1.0, pdr / patamar), 1)])
    print("Curva na zona de transição (SNR dB -> LQI):")
    for snr, pdr in curva:
        if -3.0 <= snr <= 3.0:
            print(f"  {snr:7.2f} -> {100 * min(1.0, pdr / patamar):5.1f}")

    resumos = []
    for t in args.teste:
        nome = os.path.basename(os.path.dirname(os.path.abspath(t))) or "teste"
        resumo, _ = comparar(curva, patamar, t, os.path.join(args.saida, f"comparacao_{nome}.csv"))
        resumo["teste"] = nome
        resumos.append(resumo)

    with open(os.path.join(args.saida, "resumo_comparacao.csv"), "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=list(resumos[0].keys()))
        w.writeheader()
        w.writerows(resumos)
    print("\nErro médio absoluto vs PDR medido normalizado (pontos de 0 a 100):")
    print(f"{'teste':<28}{'enlaces':>8}{'relev.':>8}{'calib(relev.)':>15}{'calib(todos)':>14}")
    for r in resumos:
        print(f"{r['teste']:<28}{r['enlaces']:>8}{r['enlaces_zona_relevante']:>8}"
              f"{str(r['erro_calibrado_zona_relevante']):>15}{str(r['erro_calibrado_todos']):>14}")
    print(f"\nArquivos salvos em {args.saida}")


if __name__ == "__main__":
    main()
