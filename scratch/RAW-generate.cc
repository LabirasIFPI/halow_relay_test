/* -*-  Mode: C++; c-file-style: "gnu"; indent-tabs-mode:nil; -*- */
/*
 * Copyright (c) 2009 MIRKO BANCHI
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as 
 * published by the Free Software Foundation;
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
*/

/*
beacon interval=0.1s
slotformat  rawslotcount slotnum
1             829              1
1             412              2 
1             204              4
*/


/*
Output current RawConfig information into file RawConfig.txt
Add current RawConfig information into file RawConfiglog.txt
*/



#include "ns3/core-module.h"
#include "ns3/network-module.h"
#include "ns3/applications-module.h"
#include "ns3/wifi-module.h"
#include "ns3/mobility-module.h"
#include "ns3/ipv4-global-routing-helper.h"
#include "ns3/internet-module.h"
#include <iostream>
#include <fstream>
#include <stdio.h>
#include <stdlib.h>
#include <ctime>
#include <fstream>
#include <sys/stat.h>
#include "ns3/rps.h"
#include <utility> // std::pair
#include <map>
#include <iostream>
#include <vector>
#include <algorithm>
#include <stdexcept>
#include <cstdint>



using namespace std;
using namespace ns3;

void RAWGroupping (uint32_t Numsta, uint32_t NGroups, uint32_t NumSlot,
                   uint32_t trial, string RAWConfigPath,
                   uint32_t beaconinterval, uint32_t pageSliceCount,
                   uint32_t pageSliceLen)
{
    (void) trial;
    (void) pageSliceLen;

    // AID 0 is reserved. The usable associated AIDs are 1..8191.
    // Each RAW assignment carries a 2-bit page and 11-bit page-local AID range.
    if (Numsta == 0 || Numsta > 8191)
    {
        throw std::runtime_error("NRawSta must be between 1 and 8191 (AID 0 is reserved).");
    }
    if (NGroups == 0 || NumSlot == 0)
    {
        throw std::runtime_error("NGroup and NumSlot must be greater than zero.");
    }
    if (pageSliceCount != 0)
    {
        throw std::runtime_error("This multi-page generator requires pageSliceCount=0. TIM page slicing is a separate configuration.");
    }

    const uint32_t AID_PAGE_SIZE = 2048;
    const uint32_t MAX_PAGE = 3;
    const uint32_t firstPage = 0;
    const uint32_t lastPage = (Numsta + AID_PAGE_SIZE) / AID_PAGE_SIZE;
    const uint32_t SlotCrossBoundary = 1;
    const uint32_t RawControl = 0;
    const uint32_t SlotFormat = (NumSlot < 8) ? 1 : 0;

    if ((SlotFormat == 1 && NumSlot >= 8) || (SlotFormat == 0 && NumSlot >= 64))
    {
        throw std::runtime_error("NumSlot is outside the encoding range supported by this RAW format.");
    }

    struct RawRange { uint32_t page, start, end; };
    std::vector<RawRange> ranges;
    uint32_t totalGroups = 0;

    for (uint32_t page = firstPage; page < lastPage && page <= MAX_PAGE; ++page)
    {
        const uint32_t globalPageStart = (page == 0) ? 1 : page * AID_PAGE_SIZE;
        const uint32_t globalPageEnd = std::min(Numsta, page * AID_PAGE_SIZE + AID_PAGE_SIZE - 1);
        if (globalPageStart > globalPageEnd)
        {
            continue;
        }

        const uint32_t pageStaCount = globalPageEnd - globalPageStart + 1;
        const uint32_t groupsThisPage = std::min(NGroups, pageStaCount);
        const uint32_t base = pageStaCount / groupsThisPage;
        const uint32_t remainder = pageStaCount % groupsThisPage;
        uint32_t globalStart = globalPageStart;

        for (uint32_t group = 0; group < groupsThisPage; ++group)
        {
            const uint32_t count = base + (group < remainder ? 1 : 0);
            const uint32_t globalEnd = globalStart + count - 1;
            ranges.push_back({page, globalStart - page * AID_PAGE_SIZE,
                              globalEnd - page * AID_PAGE_SIZE});
            globalStart = globalEnd + 1;
            ++totalGroups;
        }
    }

    if (totalGroups == 0 || totalGroups > 42)
    {
        throw std::runtime_error("The generated RPS must contain 1..42 RAW assignments; reduce NGroup (maximum 42 assignments per RPS IE).");
    }

    // The total duration of all RAW groups must fit in the beacon interval.
    const uint64_t slotBudget = beaconinterval / (uint64_t(totalGroups) * NumSlot);
    if (slotBudget <= 500)
    {
        throw std::runtime_error("beaconinterval is too short for the requested number of RAW groups and slots.");
    }
    const uint64_t durationCount = (slotBudget - 500) / 120;
    const uint64_t maxDurationCount = (SlotFormat == 1) ? 2047 : 255;
    if (durationCount > maxDurationCount)
    {
        throw std::runtime_error("Calculated SlotDurationCount exceeds the selected RAW SlotFormat range.");
    }

    ofstream newfile(RAWConfigPath.c_str(), ios::out | ios::trunc);
    if (!newfile.is_open())
    {
        throw std::runtime_error("Cannot open RAWConfigPath: " + RAWConfigPath);
    }

    // Config format: number of RPS entries, RAW assignments per RPS, then rows.
    // One RPS advertises groups for all pages, so all 8191 valid AIDs are covered
    // in the same beacon rather than rotating through only page 0.
    newfile << 1 << "\n" << totalGroups << "\n";
    for (const auto &r : ranges)
    {
        newfile << RawControl << "\t" << SlotCrossBoundary << "\t"
                << SlotFormat << "\t" << durationCount << "\t" << NumSlot
                << "\t" << r.page << "\t" << r.start << "\t" << r.end << "\n";
    }
    newfile.close();

    std::cout << "RAW config generated: stations=" << Numsta
              << ", pages=" << lastPage << ", RAW groups=" << totalGroups
              << ", slots/group=" << NumSlot << ", SlotDurationCount="
              << durationCount << std::endl;
}


int main (int argc, char *argv[])
{
  uint32_t NRawSta = 0;
  uint32_t NGroup = 0;
  uint32_t trial=0;
  uint32_t NumSlot = 0;
  uint32_t beaconinterval = 0;
  string RAWConfigPath;
  uint32_t pageSliceLen = 1, pageSliceCount = 0;
  CommandLine cmd;
  cmd.AddValue ("NRawSta", "number of stations supporting RAW", NRawSta);
  cmd.AddValue ("NGroup", "number of RAW groups", NGroup);
  cmd.AddValue ("NumSlot", "number of slot per Raw", NumSlot);
  cmd.AddValue ("beaconinterval", "beaconinterval (us)", beaconinterval);
//   cmd.AddValue ("trial", "times that RAW group has been balanced", trial);
  cmd.AddValue ("RAWConfigPath", "RAW Config file Path", RAWConfigPath);
  cmd.AddValue ("pageSliceCount", "number of TIMs in a single page period", pageSliceCount);
  cmd.AddValue ("pageSliceLen", "Number of blocks in each TIM for the associated page except the last TIM", pageSliceLen);
  cmd.Parse (argc,argv);
  
  try
  {
    std::cout << "NRawSta=" << NRawSta << " NGroup=" << NGroup
              << " NumSlot=" << NumSlot << " beaconinterval=" << beaconinterval
              << " RAWConfigPath=" << RAWConfigPath << "\n";
    RAWGroupping (NRawSta, NGroup, NumSlot, trial, RAWConfigPath,
                  beaconinterval, pageSliceCount, pageSliceLen);
  }
  catch (const std::exception &e)
  {
    std::cerr << "RAW-generate error: " << e.what() << std::endl;
    return 1;
  }
    
  return 0;
}












