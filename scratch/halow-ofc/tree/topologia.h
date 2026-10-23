#ifndef TOPOLOGIA_H
#define TOPOLOGIA_H

#include "ns3/core-module.h"
#include "ns3/mobility-module.h"

#include "radio.h"
#include "lqi.h"

#include <cmath>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

using namespace ns3;

const int PAI_GATEWAY = -1;
const int SEM_COBERTURA = -2;

/**
 * @brief Posições dos nós e árvore de ligação até o gateway, decididas antes da simulação.
 *
 */
struct Topologia
{
    Vector gateway;
    std::vector<Vector> sta;
    std::vector<int> relays;
    std::vector<int> pai;
    std::vector<int> saltos;
    std::vector<double> snrPai;
    std::vector<double> lqiPai;
    std::vector<int> paiInstalacao;

    /**
     * @brief Indica se a STA i é um relay.
     */
    bool EhRelay(int i) const
    {
        for (int r : relays) if (r == i) return true;
        return false;
    }
    /**
     * @brief Posição da STA i na lista de relays (-1 se não for relay).
     */
    int IndiceRelay(int i) const
    {
        for (size_t k = 0; k < relays.size(); k++) if (relays[k] == i) return (int) k;
        return -1;
    }
    /**
     * @brief Indica se a STA i tem caminho até o gateway.
     */
    bool Conectado(int i) const { return pai[i] != SEM_COBERTURA; }
};

/**
 * @brief Topologia "grid": matriz a partir do gateway, preenchida linha a linha
 *        (comportamento original do halow-ofc).
 *
 * A célula (0, 0) da matriz é do gateway e fica sem STA.
 * @param nsta Número de STAs.
 * @param x0 Posição x do gateway (m).
 * @param y0 Posição y do gateway (m).
 * @param spacing Distância entre nós vizinhos (m).
 * @return Topologia com o gateway e as STAs, sem relays.
 */
inline Topologia MontarTopologiaGrid(uint32_t nsta, double x0, double y0, double spacing)
{
    Topologia t;
    t.gateway = Vector(x0, y0, 0.0);
    uint32_t cols = (uint32_t) std::ceil(std::sqrt((double) nsta + 1));
    for (uint32_t linha = 0; t.sta.size() < nsta; linha++)
        for (uint32_t coluna = 0; coluna < cols && t.sta.size() < nsta; coluna++) {
            if (linha == 0 && coluna == 0) continue;
            t.sta.push_back(Vector(x0 + coluna * spacing, y0 + linha * spacing, 0.0));
        }
    return t;
}

/**
 * @brief Converte o texto "x1,y1;x2,y2;..." em uma lista de posições.
 * @param texto Posições em metros, separadas por ponto e vírgula.
 * @return Posições lidas; pares sem vírgula são ignorados.
 */
inline std::vector<Vector> LerPosicoes(const std::string& texto)
{
    std::vector<Vector> pos;
    std::stringstream ss(texto);
    std::string par;
    while (std::getline(ss, par, ';')) {
        size_t v = par.find(',');
        if (v == std::string::npos) continue;
        pos.push_back(Vector(std::stod(par.substr(0, v)), std::stod(par.substr(v + 1)), 0.0));
    }
    return pos;
}

/**
 * @brief Encontra o ponto mais próximo de uma posição.
 * @param pts Pontos candidatos.
 * @param alvo Posição de referência.
 * @return Índice do ponto mais próximo, ou -1 se a lista estiver vazia.
 */
inline int PontoMaisProximo(const std::vector<Vector>& pts, const Vector& alvo)
{
    int melhor = -1;
    double dmin = 1e18;
    for (size_t i = 0; i < pts.size(); i++) {
        double d = CalculateDistance(pts[i], alvo);
        if (d < dmin) { dmin = d; melhor = (int) i; }
    }
    return melhor;
}

/**
 * @brief Topologia "area": matriz que cobre [0, area] x [0, area], com o gateway
 *        no canto inferior direito (area, 0).
 *
 * Os relays são dados, não escolhidos: cada posição fixa vira o nó da matriz
 * mais próximo. Sem posições e com numRelays >= 1, põe um relay no centro.
 * @param area Lado da área quadrada (m).
 * @param spacing Distância entre nós vizinhos (m).
 * @param numRelays Usado só sem posições: 0 = só o gateway, >= 1 = um relay no centro.
 * @param posicoesRelays Posições fixas dos relays, "x1,y1;x2,y2;..." (m).
 * @return Topologia com o gateway, as STAs e os relays (ainda sem pais).
 */
inline Topologia MontarTopologiaArea(double area, double spacing, uint32_t numRelays,
                                     const std::string& posicoesRelays)
{
    Topologia t;
    t.gateway = Vector(area, 0.0, 0.0);
    uint32_t n = (uint32_t) std::floor(area / spacing + 1e-9);
    for (uint32_t linha = 0; linha <= n; linha++)
        for (uint32_t coluna = 0; coluna <= n; coluna++) {
            Vector p(coluna * spacing, linha * spacing, 0.0);
            if (std::fabs(p.x - t.gateway.x) < 1e-6 && std::fabs(p.y - t.gateway.y) < 1e-6) continue;
            t.sta.push_back(p);
        }

    std::vector<Vector> alvos = LerPosicoes(posicoesRelays);
    if (alvos.empty() && numRelays > 0)
        alvos.push_back(Vector(area / 2.0, area / 2.0, 0.0));
    for (const Vector& a : alvos) {
        int idx = PontoMaisProximo(t.sta, a);
        if (idx >= 0 && !t.EhRelay(idx)) t.relays.push_back(idx);
    }
    return t;
}

/**
 * @brief Regras para aceitar um enlace filho -> pai e escolher o pai.
 *
 *  - curva: curva de LQI do calibrar_lqi.py; vazia = avalia pelo SNR.
 *  - lqiMinimo: LQI previsto mínimo para o enlace valer (com curva).
 *  - snrMinimo: SNR previsto mínimo (dB), usado só sem curva.
 *  - maxFilhos: máximo de filhos por AP (0 = sem limite).
 *  - preferirMenosSaltos: true = regra "saltos"; false = regra "sinal".
 *  - Rádio usado na previsão: STA a staTxDbm com ganho staTxGainDb; AP
 *    (gateway e relay) com ganho de recepção apRxGainDb; banda larguraMhz e
 *    figura de ruído figuraRuidoDb.
 */
struct CriterioEnlace
{
    std::vector<std::pair<double, double> > curva;
    double lqiMinimo = 80.0;
    double snrMinimo = 1.0;
    uint32_t maxFilhos = 0;
    bool preferirMenosSaltos = true;
    double staTxDbm = 0.0, staTxGainDb = 0.0, apRxGainDb = 3.0;
    double larguraMhz = 2.0, figuraRuidoDb = 6.8;
};

/**
 * @brief Decide o pai de cada nó, antes da simulação, pelo enlace de uplink previsto.
 *
 * @param t Topologia com posições e relays; recebe pai, saltos, snrPai,
 *          lqiPai e paiInstalacao.
 * @param c Critério de enlace.
 */
inline void MontarArvore(Topologia& t, const CriterioEnlace& c)
{
    const int n = (int) t.sta.size();
    t.pai.assign(n, SEM_COBERTURA);
    t.saltos.assign(n, 0);
    t.snrPai.assign(n, 0.0);
    t.lqiPai.assign(n, -1.0);
    std::vector<uint32_t> filhosGateway(1, 0), filhosRelay(n, 0);

    auto posicaoAp = [&](int ap) { return ap == PAI_GATEWAY ? t.gateway : t.sta[ap]; };
    auto saltosAp = [&](int ap) { return ap == PAI_GATEWAY ? 0 : t.saltos[ap]; };
    auto cheio = [&](int ap) {
        if (c.maxFilhos == 0) return false;
        return (ap == PAI_GATEWAY ? filhosGateway[0] : filhosRelay[ap]) >= c.maxFilhos;
    };
    auto escolher = [&](int i, const std::vector<int>& aps) {
        int melhor = SEM_COBERTURA;
        double melhorLqi = -1e9, melhorSnr = -1e9;
        int melhorSaltos = 1 << 30;
        for (int ap : aps) {
            if (ap == i || cheio(ap)) continue;
            double snr = PreverSnrUplinkDb(t.sta[i], posicaoAp(ap), c.staTxDbm, c.staTxGainDb,
                                           c.apRxGainDb, c.larguraMhz, c.figuraRuidoDb);
            double lqi = AvaliarCurvaLqi(c.curva, snr);
            bool valido = c.curva.empty() ? snr >= c.snrMinimo : lqi >= c.lqiMinimo;
            if (!valido) continue;
            int s = c.preferirMenosSaltos ? saltosAp(ap) : 0;
            bool ganha = s < melhorSaltos
                      || (s == melhorSaltos && lqi > melhorLqi + 1e-9)
                      || (s == melhorSaltos && std::fabs(lqi - melhorLqi) <= 1e-9 && snr > melhorSnr);
            if (ganha) { melhor = ap; melhorSaltos = s; melhorLqi = lqi; melhorSnr = snr; }
        }
        if (melhor != SEM_COBERTURA) {
            t.pai[i] = melhor;
            t.saltos[i] = saltosAp(melhor) + 1;
            t.snrPai[i] = melhorSnr;
            t.lqiPai[i] = melhorLqi;
            if (melhor == PAI_GATEWAY) filhosGateway[0]++; else filhosRelay[melhor]++;
        }
    };

    std::vector<int> conectados(1, PAI_GATEWAY);
    std::vector<int> pendentes = t.relays;
    bool progrediu = true;
    while (!pendentes.empty() && progrediu) {
        progrediu = false;
        std::vector<int> novos, resto;
        for (int r : pendentes) {
            escolher(r, conectados);
            if (t.Conectado(r)) novos.push_back(r); else resto.push_back(r);
        }
        for (int r : novos) conectados.push_back(r);
        progrediu = !novos.empty();
        pendentes = resto;
    }

    for (int i = 0; i < n; i++)
        if (!t.EhRelay(i)) escolher(i, conectados);

    t.paiInstalacao = t.pai;
    for (int i = 0; i < n; i++) {
        if (t.Conectado(i)) continue;
        double dmin = 1e18;
        for (int ap : conectados) {
            if (ap == i) continue;
            double d = CalculateDistance(t.sta[i], posicaoAp(ap));
            if (d < dmin) { dmin = d; t.paiInstalacao[i] = ap; }
        }
    }
}

/**
 * @brief Mostra no terminal o pai de cada relay e quantos nós ficaram em cada salto.
 * @param t Topologia já montada por MontarArvore().
 */
inline void ImprimirResumoArvore(const Topologia& t)
{
    std::map<int, uint32_t> porSaltos;
    uint32_t sem = 0;
    for (size_t i = 0; i < t.sta.size(); i++) {
        if (!t.Conectado(i)) { sem++; continue; }
        porSaltos[t.saltos[i]]++;
    }
    std::cout << "Árvore: " << t.relays.size() << " relay(s), " << t.sta.size() << " STAs (inclui relays)" << std::endl;
    for (int r : t.relays)
        std::cout << "  relay STA " << r << " em (" << t.sta[r].x << ", " << t.sta[r].y << "): "
                  << (t.Conectado(r) ? (t.pai[r] == PAI_GATEWAY ? std::string("pai = gateway")
                                                                 : "pai = relay STA " + std::to_string(t.pai[r]))
                                     : std::string("SEM CAMINHO ATÉ O GATEWAY"))
                  << (t.Conectado(r) ? ", salto " + std::to_string(t.saltos[r]) : "") << std::endl;
    std::cout << "  nós por saltos até o gateway:";
    for (const auto& kv : porSaltos) std::cout << " " << kv.first << "->" << kv.second;
    std::cout << " | sem cobertura: " << sem << std::endl;
}

/**
 * @brief Grava a árvore montada, uma linha por STA (resultado_topologia.csv).
 *
 * Usado pelo analisar_saltos.py e pelas páginas.
 * @param arquivo Caminho do CSV.
 * @param t Topologia já montada por MontarArvore().
 */
inline void ExportarTopologiaCsv(const std::string& arquivo, const Topologia& t)
{
    std::ofstream out(arquivo.c_str());
    out << "sta_index,x_m,y_m,papel,pai,saltos_ate_gateway,dist_pai_m,snr_prev_pai_db,lqi_prev_pai,dist_gateway_m\n";
    for (size_t i = 0; i < t.sta.size(); i++) {
        out << i << "," << t.sta[i].x << "," << t.sta[i].y << ","
            << (t.EhRelay(i) ? "RELAY" : "STA") << ",";
        if (!t.Conectado(i)) out << "sem_cobertura,,,,";
        else {
            Vector pp = t.pai[i] == PAI_GATEWAY ? t.gateway : t.sta[t.pai[i]];
            out << (t.pai[i] == PAI_GATEWAY ? std::string("gateway") : "relay_sta_" + std::to_string(t.pai[i]))
                << "," << t.saltos[i] << "," << CalculateDistance(t.sta[i], pp) << "," << t.snrPai[i] << ",";
            if (t.lqiPai[i] >= 0) out << t.lqiPai[i];
            out << ",";
        }
        out << CalculateDistance(t.sta[i], t.gateway) << "\n";
    }
}

#endif // TOPOLOGIA_H
