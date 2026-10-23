#ifndef TREE_LQI_H
#define TREE_LQI_H

#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

/**
 * @brief Lê a curva de LQI gerada pelo calibrar_lqi.py (curva_lqi.csv).
 *
 * Colunas do arquivo: snr_uplink_db, pdr, lqi_0_100. A primeira linha é o cabeçalho.
 * @param arquivo Caminho do curva_lqi.csv.
 * @return Pares (SNR em dB, LQI de 0 a 100); vazio se o arquivo não for informado
 *         ou não abrir, e nesse caso os enlaces passam a ser avaliados pelo SNR.
 */
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
    std::getline(in, linha);
    while (std::getline(in, linha)) {
        std::stringstream ss(linha);
        std::string snr, pdr, lqi;
        if (std::getline(ss, snr, ',') && std::getline(ss, pdr, ',') && std::getline(ss, lqi, ','))
            curva.push_back(std::make_pair(std::stod(snr), std::stod(lqi)));
    }
    return curva;
}

/**
 * @brief Converte um SNR em LQI pela curva, com interpolação linear entre os pontos.
 *
 * Fora da faixa da curva, repete o valor da ponta mais próxima. É a mesma
 * conta da função avaliar() do calibrar_lqi.py.
 * @param curva Curva lida por LerCurvaLqi().
 * @param snr SNR de uplink (dB).
 * @return LQI de 0 a 100, ou -1 se a curva estiver vazia.
 */
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

#endif // TREE_LQI_H
