#ifndef TREE_RADIO_H
#define TREE_RADIO_H

#include "ns3/core-module.h"
#include "ns3/mobility-module.h"

#include <cmath>

using namespace ns3;

const double PROP_EXPOENTE = 3.76;
const double PROP_PERDA_REFERENCIA_DB = 8.0;
const double PROP_DISTANCIA_REFERENCIA_M = 1.0;

/**
 * @brief Prevê o SNR de uplink entre dois pontos, antes da simulação.
 *
 * Usa a mesma propagação do canal (LogDistancePropagationLossModel com
 * PROP_EXPOENTE, PROP_PERDA_REFERENCIA_DB e PROP_DISTANCIA_REFERENCIA_M, os
 * mesmos valores passados ao canal no main) e o mesmo ruído que o PHY usa para
 * decidir a recepção: ruído térmico da banda mais a figura de ruído.
 * @param a Posição do transmissor (filho).
 * @param b Posição do receptor (pai).
 * @param txDbm Potência de transmissão (dBm).
 * @param txGainDb Ganho de transmissão (dB).
 * @param rxGainDb Ganho de recepção (dB).
 * @param larguraMhz Largura de banda (MHz).
 * @param figuraRuidoDb Figura de ruído do receptor (dB).
 * @return SNR previsto (dB).
 */
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

#endif // TREE_RADIO_H
