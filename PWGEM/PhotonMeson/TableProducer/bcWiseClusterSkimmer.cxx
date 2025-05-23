// Copyright 2019-2020 CERN and copyright holders of ALICE O2.
// See https://alice-o2.web.cern.ch/copyright for details of the copyright holders.
// All rights not expressly granted are reserved.
//
// This software is distributed under the terms of the GNU General Public
// License v3 (GPL Version 3), copied verbatim in the file "COPYING".
//
// In applying this license CERN does not waive the privileges and immunities
// granted to it by virtue of its status as an Intergovernmental Organization
// or submit itself to any jurisdiction.
///
/// \file bcWiseClusterSkimmer.cxx
///
/// \brief This task creates minimalistic skimmed tables containing EMC clusters and centrality information
///
/// \author Nicolas Strangmann (nicolas.strangmann@cern.ch) - Goethe University Frankfurt
///

#include <limits>
#include <vector>
#include <map>

#include "Framework/runDataProcessing.h"
#include "Framework/AnalysisTask.h"

#include "DetectorsBase/GeometryManager.h"
#include "EMCALBase/Geometry.h"

#include "Common/DataModel/EventSelection.h"
#include "Common/DataModel/Centrality.h"
#include "PWGJE/DataModel/EMCALClusters.h"
#include "PWGEM/PhotonMeson/Utils/MCUtilities.h"
#include "PWGEM/PhotonMeson/DataModel/bcWiseTables.h"

using namespace o2;
using namespace o2::aod::emdownscaling;
using namespace o2::framework;

using MyCollisions = soa::Join<aod::Collisions, aod::EvSels, aod::CentFT0Ms>;
using MyMCCollisions = soa::Join<aod::Collisions, aod::EvSels, aod::CentFT0Ms, aod::McCollisionLabels>;
using MyBCs = soa::Join<aod::BCs, aod::BcSels>;

using SelectedUniqueClusters = soa::Filtered<aod::EMCALClusters>;                                                         // Clusters from collisions with only one collision in the BC
using SelectedUniqueMCClusters = soa::Filtered<soa::Join<aod::EMCALClusters, aod::EMCALMCClusters>>;                      // Clusters from collisions with only one collision in the BC
using SelectedAmbiguousClusters = soa::Filtered<aod::EMCALAmbiguousClusters>;                                             // Clusters from BCs with multiple collisions (no vertex assignment possible)
using SelectedAmbiguousMCClusters = soa::Filtered<soa::Join<aod::EMCALAmbiguousClusters, aod::EMCALAmbiguousMCClusters>>; // Clusters from BCs with multiple collisions (no vertex assignment possible)
using SelectedCells = o2::soa::Filtered<aod::Calos>;

struct bcWiseClusterSkimmer {
  Produces<aod::BCWiseBCs> bcTable;
  Produces<aod::BCWiseClusters> clusterTable;
  Produces<aod::BCWiseCollisions> collisionTable;
  Produces<aod::BCWiseMCPi0s> mcpi0Table;
  Produces<aod::BCWiseMCClusters> mcclusterTable;

  PresliceUnsorted<MyCollisions> perFoundBC = aod::evsel::foundBCId;
  Preslice<SelectedUniqueClusters> perCol = aod::emcalcluster::collisionId;
  Preslice<SelectedAmbiguousClusters> perBC = aod::emcalcluster::bcId;
  Preslice<SelectedCells> cellsPerBC = aod::calo::bcId;

  Configurable<float> cfgMinClusterEnergy{"cfgMinClusterEnergy", 0.5, "Minimum energy of selected clusters (GeV)"};
  Configurable<float> cfgMinM02{"cfgMinM02", -1., "Minimum M02 of selected clusters"};
  Configurable<float> cfgMaxM02{"cfgMaxM02", 5., "Maximum M02 of selected clusters"};
  Configurable<float> cfgMinTime{"cfgMinTime", -25, "Minimum time of selected clusters (ns)"};
  Configurable<float> cfgMaxTime{"cfgMaxTime", 25, "Maximum time of selected clusters (ns)"};
  Configurable<float> cfgRapidityCut{"cfgRapidityCut", 0.8f, "Maximum absolute rapidity of counted generated particles"};
  // Configurable<float> cfgMinPtGenPi0{"cfgMinPtGenPi0", 0., "Minimum pT for stored generated pi0s (reduce disk space of derived data)"};

  Configurable<bool> cfgRequirekTVXinEMC{"cfgRequirekTVXinEMC", false, "Only store kTVXinEMC triggered BCs"};
  Configurable<bool> cfgRequireGoodRCTQuality{"cfgRequireGoodRCTQuality", false, "Only store BCs with good quality of T0 and EMC in RCT"};

  aod::rctsel::RCTFlagsChecker isFT0EMCGoodRCTChecker{aod::rctsel::kFT0Bad, aod::rctsel::kEMCBad};

  expressions::Filter energyFilter = aod::emcalcluster::energy > cfgMinClusterEnergy;
  expressions::Filter m02Filter = (aod::emcalcluster::nCells == 1 || (aod::emcalcluster::m02 > cfgMinM02 && aod::emcalcluster::m02 < cfgMaxM02));
  expressions::Filter timeFilter = (aod::emcalcluster::time > cfgMinTime && aod::emcalcluster::time < cfgMaxTime);
  expressions::Filter emccellfilter = aod::calo::caloType == 1;

  HistogramRegistry mHistManager{"output", {}, OutputObjHandlingPolicy::AnalysisObject, false, false};

  std::map<int32_t, int32_t> fMapPi0Index; // Map to connect the MC index of the pi0 to the one saved in the derived table

  void init(framework::InitContext&)
  {
    const int nEventBins = 6;
    mHistManager.add("nBCs", "Number of BCs;;#bf{#it{N}_{BCs}}", HistType::kTH1F, {{nEventBins, -0.5, 5.5}});
    const TString binLabels[nEventBins] = {"All", "FT0", "TVX", "kTVXinEMC", "Cell", "NoBorder"};
    for (int iBin = 0; iBin < nEventBins; iBin++)
      mHistManager.get<TH1>(HIST("nBCs"))->GetXaxis()->SetBinLabel(iBin + 1, binLabels[iBin]);

    LOG(info) << "| Timing cut: " << cfgMinTime << " < t < " << cfgMaxTime;
    LOG(info) << "| M02 cut: " << cfgMinM02 << " < M02 < " << cfgMaxM02;
    LOG(info) << "| E cut: E > " << cfgMinClusterEnergy;

    o2::emcal::Geometry::GetInstanceFromRunNumber(300000);
    if (cfgRequireGoodRCTQuality)
      isFT0EMCGoodRCTChecker.init({aod::rctsel::kFT0Bad, aod::rctsel::kEMCBad});
  }

  /// \brief Process EMCAL clusters (either ambigous or unique)
  template <typename OutputType, typename InputType>
  OutputType convertForStorage(InputType const& valueIn, Observables observable)
  {
    double valueToBeChecked = valueIn * downscalingFactors[observable];
    if (valueToBeChecked < std::numeric_limits<OutputType>::lowest()) {
      LOG(warning) << "Value " << valueToBeChecked << " of observable " << observable << " below lowest possible value of " << typeid(OutputType).name() << ": " << static_cast<float>(std::numeric_limits<OutputType>::lowest());
      valueToBeChecked = static_cast<float>(std::numeric_limits<OutputType>::lowest());
    }
    if (valueToBeChecked > std::numeric_limits<OutputType>::max()) {
      LOG(warning) << "Value " << valueToBeChecked << " of observable " << observable << " obove highest possible value of " << typeid(OutputType).name() << ": " << static_cast<float>(std::numeric_limits<OutputType>::max());
      valueToBeChecked = static_cast<float>(std::numeric_limits<OutputType>::max());
    }

    return static_cast<OutputType>(valueToBeChecked);
  }

  /// \brief Process EMCAL clusters (either ambigous or unique)
  template <typename Clusters>
  void processClusters(Clusters const& clusters, const int bcID)
  {
    for (const auto& cluster : clusters) {
      clusterTable(bcID,
                   convertForStorage<uint8_t>(cluster.definition(), kDefinition),
                   convertForStorage<uint16_t>(cluster.energy(), kEnergy),
                   convertForStorage<int16_t>(cluster.eta(), kEta),
                   convertForStorage<uint16_t>(cluster.phi(), kPhi),
                   convertForStorage<uint8_t>(cluster.nCells(), kNCells),
                   convertForStorage<uint16_t>(cluster.m02(), kM02),
                   convertForStorage<int16_t>(cluster.time(), kTime),
                   cluster.isExotic());
    }
  }

  template <typename Clusters>
  void processClusterMCInfo(Clusters const& clusters, const int bcID, aod::McParticles const& mcParticles)
  {
    for (const auto& cluster : clusters) {
      float clusterInducerEnergy = 0.;
      int32_t pi0MCIndex = -1;
      if (cluster.amplitudeA().size() > 0) {
        int clusterInducerId = cluster.mcParticleIds()[0];
        auto clusterInducer = mcParticles.iteratorAt(clusterInducerId);
        clusterInducerEnergy = clusterInducer.e();
        int daughterId = aod::pwgem::photonmeson::utils::mcutil::FindMotherInChain(clusterInducer, mcParticles, std::vector<int>{111});
        if (daughterId > 0)
          pi0MCIndex = mcParticles.iteratorAt(daughterId).mothersIds()[0];
      }
      if (pi0MCIndex > 0)
        pi0MCIndex = fMapPi0Index[pi0MCIndex];
      mcclusterTable(bcID, pi0MCIndex, convertForStorage<uint16_t>(clusterInducerEnergy, kEnergy));
    }
  }

  bool isBCSelected(const auto& bc)
  {
    if (cfgRequirekTVXinEMC && !bc.selection_bit(aod::evsel::kIsTriggerTVX))
      return false;
    if (cfgRequireGoodRCTQuality && !isFT0EMCGoodRCTChecker(bc))
      return false;
    return true;
  }

  void processEventProperties(const auto& bc, const auto& collisionsInBC, const auto& cellsInBC)
  {
    bool hasFT0 = bc.has_foundFT0();
    bool hasTVX = bc.selection_bit(aod::evsel::kIsTriggerTVX);
    bool haskTVXinEMC = bc.alias_bit(kTVXinEMC);
    bool hasEMCCell = cellsInBC.size() > 0;
    bool hasNoTFROFBorder = bc.selection_bit(aod::evsel::kNoTimeFrameBorder) && bc.selection_bit(aod::evsel::kNoITSROFrameBorder);
    mHistManager.fill(HIST("nBCs"), 0);
    if (hasFT0)
      mHistManager.fill(HIST("nBCs"), 1);
    if (hasTVX)
      mHistManager.fill(HIST("nBCs"), 2);
    if (haskTVXinEMC)
      mHistManager.fill(HIST("nBCs"), 3);
    if (hasEMCCell)
      mHistManager.fill(HIST("nBCs"), 4);
    if (hasNoTFROFBorder)
      mHistManager.fill(HIST("nBCs"), 5);

    float ft0Amp = hasFT0 ? bc.foundFT0().sumAmpA() + bc.foundFT0().sumAmpC() : 0.;

    bcTable(hasFT0, hasTVX, haskTVXinEMC, hasEMCCell, hasNoTFROFBorder, convertForStorage<uint16_t>(ft0Amp, kFT0Amp));

    for (const auto& collision : collisionsInBC) {
      collisionTable(bcTable.lastIndex(), convertForStorage<uint8_t>(collision.centFT0M(), kFT0MCent), convertForStorage<int16_t>(collision.posZ(), kZVtx));
    }
  }

  template <typename TMCParticle, typename TMCParticles>
  bool isGammaGammaDecay(TMCParticle mcParticle, TMCParticles mcParticles)
  {
    auto daughtersIds = mcParticle.daughtersIds();
    if (daughtersIds.size() != 2)
      return false;
    for (const auto& daughterId : daughtersIds) {
      if (mcParticles.iteratorAt(daughterId).pdgCode() != 22)
        return false;
    }
    return true;
  }

  template <typename TMCParticle, typename TMCParticles>
  bool isAccepted(TMCParticle mcParticle, TMCParticles mcParticles)
  {
    auto daughtersIds = mcParticle.daughtersIds();
    if (daughtersIds.size() != 2)
      return false;
    for (const auto& daughterId : daughtersIds) {
      if (mcParticles.iteratorAt(daughterId).pdgCode() != 22)
        return false;
      int iCellID = -1;
      try {
        iCellID = emcal::Geometry::GetInstance()->GetAbsCellIdFromEtaPhi(mcParticles.iteratorAt(daughterId).eta(), mcParticles.iteratorAt(daughterId).phi());
      } catch (emcal::InvalidPositionException& e) {
        iCellID = -1;
      }
      if (iCellID == -1)
        return false;
    }
    return true;
  }

  void processData(MyBCs const& bcs, MyCollisions const& collisions, aod::FT0s const&, SelectedCells const& cells, SelectedUniqueClusters const& uClusters, SelectedAmbiguousClusters const& aClusters)
  {
    for (const auto& bc : bcs) {
      if (!isBCSelected(bc))
        continue;
      auto collisionsInBC = collisions.sliceBy(perFoundBC, bc.globalIndex());
      auto cellsInBC = cells.sliceBy(cellsPerBC, bc.globalIndex());

      processEventProperties(bc, collisionsInBC, cellsInBC);

      if (collisionsInBC.size() == 1) {
        auto clustersInBC = uClusters.sliceBy(perCol, collisionsInBC.begin().globalIndex());
        processClusters(clustersInBC, bcTable.lastIndex());
      } else {
        auto clustersInBC = aClusters.sliceBy(perBC, bc.globalIndex());
        processClusters(clustersInBC, bcTable.lastIndex());
      }
    }
  }
  PROCESS_SWITCH(bcWiseClusterSkimmer, processData, "Run skimming for data", true);

  Preslice<aod::McCollisions> mcCollperBC = aod::mccollision::bcId;
  Preslice<aod::McParticles> perMcCollision = aod::mcparticle::mcCollisionId;

  void processMC(MyBCs const& bcs, MyMCCollisions const& collisions, aod::McCollisions const& mcCollisions, aod::FT0s const&, SelectedCells const& cells, SelectedUniqueMCClusters const& uClusters, SelectedAmbiguousMCClusters const& aClusters, aod::McParticles const& mcParticles)
  {
    for (const auto& bc : bcs) {
      if (!isBCSelected(bc))
        continue;
      auto collisionsInBC = collisions.sliceBy(perFoundBC, bc.globalIndex());
      auto cellsInBC = cells.sliceBy(cellsPerBC, bc.globalIndex());

      processEventProperties(bc, collisionsInBC, cellsInBC);

      auto mcCollisionsBC = mcCollisions.sliceBy(mcCollperBC, bc.globalIndex());
      for (const auto& mcCollision : mcCollisionsBC) {
        auto mcParticlesInColl = mcParticles.sliceBy(perMcCollision, mcCollision.globalIndex());
        for (const auto& mcParticle : mcParticlesInColl) {
          if (mcParticle.pdgCode() != 111 || std::abs(mcParticle.y()) > cfgRapidityCut || !isGammaGammaDecay(mcParticle, mcParticles))
            continue;
          bool isPrimary = mcParticle.isPhysicalPrimary() || mcParticle.producedByGenerator();
          bool isFromWD = (aod::pwgem::photonmeson::utils::mcutil::IsFromWD(mcCollision, mcParticle, mcParticles)) > 0;
          mcpi0Table(bc.globalIndex(), convertForStorage<uint16_t>(mcParticle.pt(), kpT), isAccepted(mcParticle, mcParticles), isPrimary, isFromWD);
          fMapPi0Index[mcParticle.globalIndex()] = static_cast<int32_t>(mcpi0Table.lastIndex());
        }
      }

      if (collisionsInBC.size() == 1) {
        auto clustersInBC = uClusters.sliceBy(perCol, collisionsInBC.begin().globalIndex());
        processClusters(clustersInBC, bcTable.lastIndex());
        processClusterMCInfo(clustersInBC, bc.globalIndex(), mcParticles);
      } else {
        auto clustersInBC = aClusters.sliceBy(perBC, bc.globalIndex());
        processClusters(clustersInBC, bcTable.lastIndex());
        processClusterMCInfo(clustersInBC, bc.globalIndex(), mcParticles);
      }
      fMapPi0Index.clear();
    }
  }
  PROCESS_SWITCH(bcWiseClusterSkimmer, processMC, "Run skimming for MC", false);
};

WorkflowSpec defineDataProcessing(ConfigContext const& cfgc) { return WorkflowSpec{adaptAnalysisTask<bcWiseClusterSkimmer>(cfgc)}; }
