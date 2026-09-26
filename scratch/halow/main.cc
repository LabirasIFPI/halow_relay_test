#include "ns3/core-module.h"
#include "ns3/rng-seed-manager.h"
#include "halow.h"
#include <fstream>
#include <iostream>
#include <sstream>

using namespace ns3;
using namespace std;

NS_LOG_COMPONENT_DEFINE ("HalowProject");

// ============================================================
//  configureRAW — CÓPIA EXATA do s1g-test-tim-raw.cc
//  Nenhuma modificação. Lê do arquivo .txt gerado por raw-generate.
// ============================================================
RPSVector configureRAW (RPSVector rpslist, string RAWConfigFile, Configuration &config) {
    uint16_t NRPS = 0, NRAWPERBEACON = 0, Value = 0;
    uint32_t page = 0, aid_start = 0, aid_end = 0, rawinfo = 0;

    ifstream myfile (RAWConfigFile);

    if (myfile.is_open ()) {
        myfile >> NRPS;
        int totalNumSta = 0;

        for (uint16_t kk = 0; kk < NRPS; kk++) {
            RPS *m_rps = new RPS;
            myfile >> NRAWPERBEACON;

            for (uint16_t i = 0; i < NRAWPERBEACON; i++) {
                RPS::RawAssignment *m_raw = new RPS::RawAssignment;

                myfile >> Value; m_raw->SetRawControl (Value);
                myfile >> Value; m_raw->SetSlotCrossBoundary (Value);
                myfile >> Value; m_raw->SetSlotFormat (Value);
                myfile >> Value; m_raw->SetSlotDurationCount (Value);
                myfile >> Value; m_raw->SetSlotNum (Value);
                myfile >> page;
                myfile >> aid_start;
                myfile >> aid_end;

                rawinfo = (aid_end << 13) | (aid_start << 2) | page;
                m_raw->SetRawGroup (rawinfo);
                totalNumSta += aid_end - aid_start + 1;
                m_rps->SetRawAssignment (*m_raw);
                delete m_raw;
            }
            rpslist.rpsset.push_back (m_rps);
        }
        myfile.close ();
        config.NRawSta = totalNumSta;
    } else {
        cerr << "[ERRO] Não abriu: " << RAWConfigFile << endl;
    }

    return rpslist;
}

// ============================================================
//  configurePageSlice — CÓPIA EXATA do s1g-test-tim-raw.cc
// ============================================================
void configurePageSlice (Configuration &config) {
    config.pageS.SetPageindex    (config.pageIndex);
    config.pageS.SetPagePeriod   (config.pagePeriod);
    config.pageS.SetPageSliceLen (config.pageSliceLength);
    config.pageS.SetPageSliceCount (config.pageSliceCount);
    config.pageS.SetBlockOffset  (config.blockOffset);
    config.pageS.SetTIMOffset    (config.timOffset);
}

// ============================================================
//  configureTIM — CÓPIA EXATA do s1g-test-tim-raw.cc
// ============================================================
void configureTIM (Configuration &config) {
    config.tim.SetPageIndex (config.pageIndex);
    if (config.pageSliceCount)
        config.tim.SetDTIMPeriod (config.pageSliceCount);
    else
        config.tim.SetDTIMPeriod (1);
}

// ============================================================
//  main
// ============================================================
int main (int argc, char *argv[]) {
    Configuration config (argc, argv);

    // Arquivo RAW baseado no Nsta (gerado previamente por raw-generate)
    std::ostringstream rawFile;
    rawFile << "scratch/halow/RawConfig-N" << config.Nsta << ".txt";
    config.RAWConfigFile = rawFile.str ();

    // Lê o RAW
    config.rps = configureRAW (config.rps, config.RAWConfigFile, config);

    // Configura PageSlicing e TIM (funções oficiais do autor)
    configurePageSlice (config);
    configureTIM (config);

    // Log
    cerr << "[MAIN] Nsta=" << config.Nsta
         << " NRawSta=" << config.NRawSta
         << " beacon=" << config.BeaconInterval << "us"
         << " file=" << config.RAWConfigFile << endl;

    std::string outputFile = "scratch/halow/dataset-halow.txt";

    RngSeedManager::SetSeed (config.seed);
    RngSeedManager::SetRun  (config.seed);

    LogComponentEnable ("ApWifiMac", LOG_LEVEL_DEBUG);
    LogComponentEnable ("TIM", LOG_LEVEL_DEBUG);
    LogComponentEnable ("StaWifiMac", LOG_LEVEL_DEBUG);

    HalowExperiment experiment (config);
    experiment.Run ();
    experiment.ExportDataset (outputFile);

    Simulator::Destroy ();
    return 0;
}