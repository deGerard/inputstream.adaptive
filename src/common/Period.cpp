/*
 *  Copyright (C) 2023 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "Period.h"
#include "AdaptationSet.h"
#include "AdaptiveUtils.h"
#include "Representation.h"

using namespace PLAYLIST;

PLAYLIST::CPeriod::CPeriod() : CCommonSegAttribs()
{
  m_psshSets.emplace_back(PSSHSet());
}

PLAYLIST::CPeriod::~CPeriod()
{
}

void PLAYLIST::CPeriod::CopyHLSData(const CPeriod* other)
{
  m_adaptationSets.reserve(other->m_adaptationSets.size());
  for (const auto& otherAdp : other->m_adaptationSets)
  {
    std::unique_ptr<CAdaptationSet> adp = CAdaptationSet::MakeUniquePtr(this);
    adp->CopyHLSData(otherAdp.get());
    m_adaptationSets.push_back(std::move(adp));
  }

  m_baseUrl = other->m_baseUrl;
  m_id = other->m_id;
  m_timescale = other->m_timescale;
  m_start = other->m_start;
  m_startPts = other->m_startPts;
  m_duration = other->m_duration;
  m_encryptionState = other->m_encryptionState;
  m_includedStreamType = other->m_includedStreamType;
  m_isSecureDecoderNeeded = other->m_isSecureDecoderNeeded;
}

uint16_t PLAYLIST::CPeriod::InsertPSSHSet(PSSHSet* psshSet)
{
  if (psshSet)
  {
    // Find the psshSet by skipping the first one of the list (empty)
    // note that PSSHSet struct has a custom comparator for std::find
    auto itPssh = std::find(m_psshSets.begin() + 1, m_psshSets.end(), *psshSet);

    if (itPssh == m_psshSets.end())
      itPssh = m_psshSets.insert(m_psshSets.end(), *psshSet);
    else // Found
    {
      // If the existing is not used replace it with current one
      if (itPssh->m_usageCount == 0)
        *itPssh = *psshSet;
    }

    itPssh->m_usageCount++;

    return static_cast<uint16_t>(itPssh - m_psshSets.begin());
  }
  else
  {
    // Increase the usage of first empty psshSet
    m_psshSets[0].m_usageCount++;
    return PSSHSET_POS_DEFAULT;
  }
}

void PLAYLIST::CPeriod::RemovePSSHSet(uint16_t pssh_set)
{
  for (std::unique_ptr<PLAYLIST::CAdaptationSet>& adpSet : m_adaptationSets)
  {
    auto& reps = adpSet->GetRepresentations();

    for (auto itRepr = reps.begin(); itRepr != reps.end();)
    {
      if ((*itRepr)->m_psshSetPos == pssh_set)
        itRepr = reps.erase(itRepr);
      else
        itRepr++;
    }
  }
}

void PLAYLIST::CPeriod::AddAdaptationSet(std::unique_ptr<CAdaptationSet>& adaptationSet)
{
  m_adaptationSets.push_back(std::move(adaptationSet));
}
