#ifndef TOPOLOGIA_H
#define TOPOLOGIA_H

// ============================================================
// topologia.h - Posições dos nós e montagem da árvore (gateway + relays)
//
//  "grid": matriz a partir do gateway (comportamento original do halow-ofc).
//  "area": matriz que preenche uma área quadrada; gateway no canto inferior
//          direito e relays em posições fixas (ou um relay no centro).
//
// Os relays são DADOS (posições fixas); a escolha de quais nós viram relays
// fica para um algoritmo próprio. Aqui só se decide a qual AP cada nó se liga,
// antes da simulação, pelo enlace de uplink previsto (mesmo modelo de
// propagação do canal):
//   - enlace válido: LQI previsto >= LQI mínimo (curva calibrada do
//     calibrar_lqi.py) ou, sem curva, SNR previsto >= SNR mínimo;
//   - preferência "saltos" (padrão): pai = AP válido com MENOS saltos até o
//     gateway; empate -> maior LQI; empate -> maior SNR;
//   - preferência "sinal": pai = AP válido com maior LQI; empate -> maior SNR
//     (ignora saltos; foi a regra do primeiro teste da Krisna);
//   - opcionalmente respeitando um máximo de filhos por AP.
// Relays se ligam primeiro (por nível, a partir do gateway), depois as STAs.
// ============================================================

#include "ns3/core-module.h"
#include "ns3/mobility-module.h"

#include <cmath>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

using namespace ns3;

// Mesmos parâmetros do canal (LogDistancePropagationLossModel no main)
const double PROP_EXPOENTE = 3.76;
const double PROP_PERDA_REFERENCIA_DB = 8.0;
const double PROP_DISTANCIA_REFERENCIA_M = 1.0;

const int PAI_GATEWAY = -1;     // pai[i] == PAI_GATEWAY: ligado ao gateway
const int SEM_COBERTURA = -2;   // nenhum AP com enlace válido

struct Topologia
{
    Vector gateway;
    std::vector<Vector> sta;          // posição de cada STA (índice = nó)
    std::vector<int> relays;          // índices das STAs que também são relays
    std::vector<int> pai;             // PAI_GATEWAY, índice do relay pai ou SEM_COBERTURA
    std::vector<int> saltos;          // saltos até o gateway (0 = sem cobertura)
    std::vector<double> snrPai;       // SNR de uplink previsto até o pai (dB)
    std::vector<double> lqiPai;       // LQI previsto até o pai (-1 = sem curva)
    std::vector<int> paiInstalacao;   // AP cujo SSID/canal o nó usa: o pai ou, sem
                                      // cobertura, o AP conectado mais próximo

    bool EhRelay(int i) const
    {
        for (int r : relays) if (r == i) return true;
        return false;
    }
    int IndiceRelay(int i) const      // posição do relay na lista (-1 se não é relay)
    {
        for (size_t k = 0; k < relays.size(); k++) if (relays[k] == i) return (int) k;
        return -1;
    }
    bool Conectado(int i) const { return pai[i] != SEM_COBERTURA; }
};

// Matriz a partir do gateway em (x0, y0), preenchida linha a linha (original).
inline Topologia MontarTopologiaGrid(uint32_t nsta, double x0, double y0, double spacing)
{
    Topologia t;
    t.gateway = Vector(x0, y0, 0.0);
    uint32_t cols = (uint32_t) std::ceil(std::sqrt((double) nsta + 1));
    for (uint32_t linha = 0; t.sta.size() < nsta; linha++)
        for (uint32_t coluna = 0; coluna < cols && t.sta.size() < nsta; coluna++) {
            if (linha == 0 && coluna == 0) continue;   // célula do gateway
            t.sta.push_back(Vector(x0 + coluna * spacing, y0 + linha * spacing, 0.0));
        }
    return t;
}

// "x1,y1;x2,y2;..." -> lista de posições
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

// Matriz que cobre [0, area] x [0, area]; gateway em (area, 0).
// Relays: posições fixas (cada uma vira o nó da matriz mais próximo) ou,
// sem posições e com numRelays >= 1, um relay no centro.
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

// SNR de uplink na mesma escala que o PHY usa (ruído térmico + figura de ruído).
inline double PreverSnrUplinkDb(const Vector& a, const Vector& b, double txDbm, double txGainDb,
                                double rxGainDb, double larguraMhz, double figuraRuidoDb)
{
    double d = CalculateDistance(a, b);
    double perda = PROP_PERDA_REFERENCIA_DB;
    if (d > PROP_DISTANCIA_REFERENCIA_M)
        perda += 10.0 * PROP_EXPOENTE * std::log10(d / PROP_DISTANCIA_REFERENCIA_M);
    double rx = txDbm + txGainDb + rxGainDb - perda;
    double ruido = -174.0 + 10.0 * std::log10(larguraMhz * 1e6) + figuraRuidoDb;
    return rx - ruido;
}

// Lê curva_lqi.csv (snr_uplink_db,pdr,lqi_0_100). Vazia se o arquivo não abrir.
inline std::vector<std::pair<double, double> > LerCurvaLqi(const std::string& arquivo)
{
    std::vector<std::pair<double, double> > curva;
    if (arquivo.empty()) return curva;
    std::ifstream in(arquivo.c_str());
    if (!in.is_open()) {
        std::cout << "AVISO: não foi possível abrir a curva de LQI " << arquivo
                  << "; os enlaces serão avaliados pelo SNR." << std::endl;
        return curva;
    }
    std::string linha;
    std::getline(in, linha); // cabeçalho
    while (std::getline(in, linha)) {
        std::stringstream ss(linha);
        std::string snr, pdr, lqi;
        if (std::getline(ss, snr, ',') && std::getline(ss, pdr, ',') && std::getline(ss, lqi, ','))
            curva.push_back(std::make_pair(std::stod(snr), std::stod(lqi)));
    }
    return curva;
}

inline double AvaliarCurvaLqi(const std::vector<std::pair<double, double> >& curva, double snr)
{
    if (curva.empty()) return -1.0;
    if (snr <= curva.front().first) return curva.front().second;
    if (snr >= curva.back().first) return curva.back().second;
    for (size_t i = 1; i < curva.size(); i++)
        if (snr <= curva[i].first) {
            double s0 = curva[i - 1].first, s1 = curva[i].first;
            double l0 = curva[i - 1].second, l1 = curva[i].second;
            return s1 == s0 ? l0 : l0 + (l1 - l0) * (snr - s0) / (s1 - s0);
        }
    return curva.back().second;
}

struct CriterioEnlace
{
    std::vector<std::pair<double, double> > curva; // vazia = avalia pelo SNR
    double lqiMinimo = 80.0;     // com curva
    double snrMinimo = 1.0;      // sem curva (dB)
    uint32_t maxFilhos = 0;      // 0 = sem limite
    bool preferirMenosSaltos = true; // false = preferência "sinal"
    // rádio: STA 0 dBm / ganho TX 0 dB; AP (gateway e relay) ganho RX 3 dB
    double staTxDbm = 0.0, staTxGainDb = 0.0, apRxGainDb = 3.0;
    double larguraMhz = 2.0, figuraRuidoDb = 6.8;
};

// Monta a árvore: relays por nível a partir do gateway, depois as STAs.
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
    // Escolhe o melhor pai para o nó i entre os APs disponíveis
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

    // 1) Backbone: relays ligados nível a nível (BFS a partir do gateway)
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

    // 2) STAs comuns se ligam ao melhor AP conectado (gateway ou relay)
    for (int i = 0; i < n; i++)
        if (!t.EhRelay(i)) escolher(i, conectados);

    // 3) Nós sem cobertura ficam no SSID do AP conectado mais próximo (não vão
    //    associar: o limiar de beacon os impede de insistir)
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
