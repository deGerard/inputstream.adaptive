/*
 *  Copyright (C) 2023 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "HLSTree.h"

#include "../aes_decrypter.h"
#include "../utils/Base64Utils.h"
#include "../utils/StringUtils.h"
#include "../utils/UrlUtils.h"
#include "../utils/Utils.h"
#include "../utils/log.h"
#include "kodi/tools/StringUtils.h"

#include <optional>
#include <sstream>

using namespace PLAYLIST;
using namespace UTILS;
using namespace kodi::tools;

namespace
{
// \brief Parse a tag (e.g. #EXT-X-VERSION:1) to extract name and value
void ParseTagNameValue(const std::string& line, std::string& tagName, std::string& tagValue)
{
  tagName.clear();
  tagValue.clear();

  if (line[0] != '#')
    return;

  size_t charPos = line.find(':');
  tagName = line.substr(0, charPos);
  if (charPos != std::string::npos)
    tagValue = line.substr(charPos + 1);
}

// \brief Parse a tag value of attributes, double accent characters will be removed
//        e.g. TYPE=AUDIO,GROUP-ID="audio" the output will be TYPE -> AUDIO and GROUP-ID -> audio
std::map<std::string, std::string> ParseTagAttributes(const std::string& tagValue)
{
  std::map<std::string, std::string> tagAttribs;
  size_t offset{0};
  size_t value;
  size_t end;

  while (offset < tagValue.size() && (value = tagValue.find('=', offset)) != std::string::npos)
  {
    while (offset < tagValue.size() && tagValue[offset] == ' ')
    {
      ++offset;
    }
    end = value;
    uint8_t inValue(0);
    while (++end < tagValue.size() && ((inValue & 1) || tagValue[end] != ','))
    {
      if (tagValue[end] == '\"')
        ++inValue;
    }

    std::string attribName = tagValue.substr(offset, value - offset);
    StringUtils::TrimRight(attribName);

    std::string attribValue =
        tagValue.substr(value + (inValue ? 2 : 1), end - value - (inValue ? 3 : 1));
    StringUtils::Trim(attribValue);

    tagAttribs[attribName] = attribValue;
    offset = end + 1;
  }
  return tagAttribs;
}

void ParseResolution(int& width, int& height, std::string_view val)
{
  size_t pos = val.find('x');
  if (pos != std::string::npos)
  {
    width = STRING::ToInt32(val.substr(0, pos));
    height = STRING::ToInt32(val.substr(pos + 1));
  }
}

// \brief Detect container type from file extension
ContainerType DetectContainerTypeFromExt(std::string_view extension)
{
  if (STRING::CompareNoCase(extension, "ts"))
    return ContainerType::TS;
  else if (STRING::CompareNoCase(extension, "aac"))
    return ContainerType::ADTS;
  else if (STRING::CompareNoCase(extension, "mp4"))
    return ContainerType::MP4;
  else if (STRING::CompareNoCase(extension, "vtt") || STRING::CompareNoCase(extension, "webvtt"))
    return ContainerType::TEXT;
  else
    return ContainerType::INVALID;
}

// \brief Workaround to get audio codec from CODECS attribute list
std::string GetAudioCodec(std::string_view codecs)
{
  //! @todo: this way to get the audio codec is inappropriate and can lead to bad playback
  //! this because CODECS attribute its optionals and not guarantee to provide full codec list
  //! the codec format should be provided by MP4 demuxer

  // The codec search must follow exactly the following order, this is currently the best workaround
  // to make multi-channel audio formats work, but since CODECS attribute is unreliable
  // this workaround can still cause playback problems
  if (codecs.find("ec-3") != std::string::npos)
    return "ec-3";
  else if (codecs.find("ac-3") != std::string::npos)
    return "ac-3";
  else
    return "aac";
}
// \brief Workaround to get audio codec from representation codecs list
std::string GetAudioCodec(const PLAYLIST::CRepresentation* repr)
{
  if (repr->ContainsCodec("ec-3"))
    return "ec-3";
  else if (repr->ContainsCodec("ac-3"))
    return "ac-3";
  else
    return "aac";
}

} // unnamed namespace

adaptive::CHLSTree::CHLSTree(const CHLSTree& left) : AdaptiveTree(left)
{
  m_decrypter = std::make_unique<AESDecrypter>(left.m_decrypter->getLicenseKey());
}

void adaptive::CHLSTree::Configure(const UTILS::PROPERTIES::KodiProperties& kodiProps)
{
  AdaptiveTree::Configure(kodiProps);
  m_decrypter = std::make_unique<AESDecrypter>(kodiProps.m_licenseKey);
}

bool adaptive::CHLSTree::open(const std::string& url)
{
  return open(url, {});
}

bool adaptive::CHLSTree::open(const std::string& url,
                              std::map<std::string, std::string> additionalHeaders)
{
  std::string data;
  HTTPRespHeaders respHeaders;
  if (!DownloadManifest(url, additionalHeaders, data, respHeaders))
    return false;

  SaveManifest(nullptr, data, url);

  if (!PreparePaths(respHeaders.m_effectiveUrl))
    return false;

  if (!ParseManifest(data))
  {
    LOG::LogF(LOGERROR, "Failed to parse the manifest file");
    return false;
  }

  if (m_periods.empty())
  {
    LOG::Log(LOGWARNING, "No periods in the manifest");
    return false;
  }

  m_currentPeriod = m_periods[0].get();

  SortTree();

  return true;
}

PLAYLIST::PrepareRepStatus adaptive::CHLSTree::prepareRepresentation(PLAYLIST::CPeriod* period,
                                                                     PLAYLIST::CAdaptationSet* adp,
                                                                     PLAYLIST::CRepresentation* rep,
                                                                     bool update)
{
  if (rep->GetSourceUrl().empty())
    return PrepareRepStatus::FAILURE;

  CRepresentation* entryRep = rep;
  uint64_t currentRepSegNumber = rep->getCurrentSegmentNumber();

  size_t adpSetPos = GetPtrPosition(period->GetAdaptationSets(), adp);
  size_t reprPos = GetPtrPosition(adp->GetRepresentations(), rep);

  std::unique_ptr<CPeriod> periodLost;

  PrepareRepStatus prepareStatus = PrepareRepStatus::OK;
  std::string data;
  HTTPRespHeaders respHeaders;

  if (rep->m_isDownloaded)
  {
    // do nothing
  }
  else if (DownloadManifest(rep->GetSourceUrl(), {}, data, respHeaders))
  {
    // Parse child playlist

    SaveManifest(adp, data, rep->GetSourceUrl());

    std::string baseUrl = URL::RemoveParameters(respHeaders.m_effectiveUrl);

    EncryptionType currentEncryptionType = EncryptionType::CLEAR;

    uint64_t currentSegStartPts{0};
    uint64_t newStartNumber{0};

    CSpinCache<CSegment> newSegments;
    std::optional<CSegment> newSegment;
    bool segmentHasByteRange{false};
    // Pssh set used between segments
    uint16_t psshSetPos = PSSHSET_POS_DEFAULT;

    CSegment segInit; // Initialization segment
    std::string segInitUrl; // Initialization segment URL
    bool hasSegmentInit{false};

    uint32_t discontCount{0};

    bool isExtM3Uformat{false};

    std::stringstream streamData{data};

    for (std::string line; STRING::GetLine(streamData, line);)
    {
      // Keep track of current line pos, can be used to go back to previous line
      // if we move forward within the loop code
      std::streampos currentStreamPos = streamData.tellg();

      std::string tagName;
      std::string tagValue;
      ParseTagNameValue(line, tagName, tagValue);

      // Find the extended M3U file initialization tag
      if (!isExtM3Uformat)
      {
        if (tagName == "#EXTM3U")
          isExtM3Uformat = true;
        continue;
      }

      if (tagName == "#EXT-X-KEY")
      {
        auto attribs = ParseTagAttributes(tagValue);

        switch (ProcessEncryption(base_url_, attribs))
        {
          case EncryptionType::NOT_SUPPORTED:
            period->SetEncryptionState(EncryptionState::ENCRYPTED);
            return PrepareRepStatus::FAILURE;
          case EncryptionType::AES128:
            currentEncryptionType = EncryptionType::AES128;
            psshSetPos = PSSHSET_POS_DEFAULT;
            break;
          case EncryptionType::WIDEVINE:
            currentEncryptionType = EncryptionType::WIDEVINE;
            period->SetEncryptionState(EncryptionState::ENCRYPTED_SUPPORTED);

            rep->m_psshSetPos = InsertPsshSet(adp->GetStreamType(), period, adp, m_currentPssh,
                                              m_currentDefaultKID, m_currentIV);
            if (period->GetPSSHSets()[rep->GetPsshSetPos()].m_usageCount == 1 ||
                prepareStatus == PrepareRepStatus::DRMCHANGED)
            {
              prepareStatus = PrepareRepStatus::DRMCHANGED;
            }
            else
              prepareStatus = PrepareRepStatus::DRMUNCHANGED;
            break;
          case EncryptionType::UNKNOWN:
            LOG::LogF(LOGWARNING, "Unknown encryption type");
            break;
          default:
            break;
        }
      }
      else if (tagName == "#EXT-X-MAP")
      {
        auto attribs = ParseTagAttributes(tagValue);

        if (STRING::KeyExists(attribs, "URI"))
        {
          std::string uri = attribs["URI"];

          if (URL::IsUrlRelative(uri))
            segInitUrl = URL::Join(baseUrl, uri);
          else
            segInitUrl = uri;

          segInit.url = segInitUrl;
          segInit.startPTS_ = NO_PTS_VALUE;
          segInit.pssh_set_ = PSSHSET_POS_DEFAULT;
          rep->SetHasInitialization(true);
          rep->SetContainerType(ContainerType::MP4);
          hasSegmentInit = true;
        }

        if (STRING::KeyExists(attribs, "BYTERANGE"))
        {
          if (ParseRangeValues(attribs["BYTERANGE"], segInit.range_end_, segInit.range_begin_))
          {
            segInit.range_end_ = segInit.range_begin_ + segInit.range_end_ - 1;
          }
        }
        else
          segInit.range_begin_ = CSegment::NO_RANGE_VALUE;
      }
      else if (tagName == "#EXT-X-MEDIA-SEQUENCE")
      {
        newStartNumber = STRING::ToUint64(tagValue);
      }
      else if (tagName == "#EXT-X-PLAYLIST-TYPE")
      {
        if (STRING::CompareNoCase(tagValue, "VOD"))
        {
          m_refreshPlayList = false;
          has_timeshift_buffer_ = false;
        }
      }
      else if (tagName == "#EXT-X-TARGETDURATION")
      {
        // Set update interval for manifest LIVE update
        // to maximum segment duration * 1500 secs (25 minuts)
        uint32_t newIntervalSecs = STRING::ToUint32(tagValue) * 1500;
        if (newIntervalSecs < m_updateInterval)
          m_updateInterval = newIntervalSecs;
      }
      else if (tagName == "#EXTINF")
      {
        // Make a new segment
        newSegment = CSegment();
        newSegment->startPTS_ = currentSegStartPts;

        uint64_t duration = static_cast<uint64_t>(STRING::ToFloat(tagValue) * rep->GetTimescale());
        newSegment->m_duration = duration;
        newSegment->pssh_set_ = psshSetPos;

        currentSegStartPts += duration;
      }
      else if (tagName == "#EXT-X-BYTERANGE" && newSegment.has_value())
      {
        ParseRangeValues(tagValue, newSegment->range_end_, newSegment->range_begin_);

        if (newSegment->range_begin_ == CSegment::NO_RANGE_VALUE)
        {
          if (newSegments.GetSize() > 0)
            newSegment->range_begin_ = newSegments.Get(newSegments.GetSize() - 1)->range_end_ + 1;
          else
            newSegment->range_begin_ = 0;
        }

        newSegment->range_end_ += newSegment->range_begin_;
        segmentHasByteRange = true;
      }
      else if (newSegment.has_value() && !line.empty() && line[0] != '#')
      {
        // We fall here after a EXTINF (and possible EXT-X-BYTERANGE in the middle)

        if (rep->GetContainerType() == ContainerType::NOTYPE)
        {
          // Try find the container type on the representation according to the file extension
          std::string url = URL::RemoveParameters(line, false);
          url = url.substr(URL::GetDomainUrl(url).size());
          std::string extension;
          size_t extPos = line.rfind('.');
          if (extPos != std::string::npos)
            extension = line.substr(extPos);

          ContainerType containerType = ContainerType::INVALID;

          if (!extension.empty())
          {
            containerType = DetectContainerTypeFromExt(extension);

            // Streams that have a media url encoded as a parameter of the url itself
            // e.g. https://cdn-prod.tv/beacon?streamId=1&rp=https%3A%2F%2Ftest.com%2F167037ac3%2Findex_4_0.ts&sessionId=abc&assetId=OD
            // cannot be detected in safe way, so we try fallback to common containers
          }

          if (containerType == ContainerType::INVALID)
          {
            switch (adp->GetStreamType())
            {
              case StreamType::VIDEO:
                LOG::LogF(LOGWARNING,
                          "Cannot detect container type from media url, fallback to TS");
                containerType = ContainerType::TS;
                break;
              case StreamType::AUDIO:
                LOG::LogF(LOGWARNING,
                          "Cannot detect container type from media url, fallback to ADTS");
                containerType = ContainerType::ADTS;
                break;
              case StreamType::SUBTITLE:
                LOG::LogF(LOGWARNING,
                          "Cannot detect container type from media url, fallback to TEXT");
                containerType = ContainerType::TEXT;
                break;
              default:
                break;
            }
          }
          rep->SetContainerType(containerType);
        }
        else if (rep->GetContainerType() == ContainerType::INVALID)
        {
          // Skip EXTINF segment
          newSegment.reset();
          continue;
        }

        if (!segmentHasByteRange || rep->GetUrl().empty())
        {
          std::string url;
          if (URL::IsUrlRelative(line))
            url = URL::Join(baseUrl, line);
          else
            url = line;

          if (!segmentHasByteRange)
          {
            newSegment->url = url;
          }
          else
            rep->SetUrl(url);
        }

        if (currentEncryptionType == EncryptionType::AES128)
        {
          if (psshSetPos == PSSHSET_POS_DEFAULT)
          {
            psshSetPos = InsertPsshSet(StreamType::NOTYPE, period, adp, m_currentPssh,
                                       m_currentDefaultKID, m_currentIV);
            newSegment->pssh_set_ = psshSetPos;
          }
          else
            period->InsertPSSHSet(newSegment->pssh_set_);
        }

        newSegments.GetData().emplace_back(*newSegment);
        newSegment.reset();
      }
      else if (tagName == "#EXT-X-DISCONTINUITY-SEQUENCE")
      {
        m_discontSeq = STRING::ToUint32(tagValue);
        if (!initial_sequence_.has_value())
          initial_sequence_ = m_discontSeq;

        m_hasDiscontSeq = true;
        // make sure first period has a sequence on initial prepare
        if (!update && m_discontSeq > 0 && m_periods.back()->GetSequence() == 0)
          m_periods[0]->SetSequence(m_discontSeq);

        for (auto itPeriod = m_periods.begin(); itPeriod != m_periods.end();)
        {
          if (itPeriod->get()->GetSequence() < m_discontSeq)
          {
            if ((*itPeriod).get() != m_currentPeriod)
            {
              itPeriod = m_periods.erase(itPeriod);
            }
            else
            {
              // we end up here after pausing for some time
              // remove from m_periods for now and reattach later
              periodLost = std::move(*itPeriod); // Move out the period unique ptr
              itPeriod = m_periods.erase(itPeriod); // Remove empty unique ptr state
            }
          }
          else
            itPeriod++;
        }

        period = m_periods[0].get();
        adp = period->GetAdaptationSets()[adpSetPos].get();
        rep = adp->GetRepresentations()[reprPos].get();
      }
      else if (tagName == "#EXT-X-DISCONTINUITY")
      {
        if (!newSegments.Get(0))
        {
          LOG::LogF(LOGERROR, "Segment at position 0 not found");
          continue;
        }

        period->SetSequence(m_discontSeq + discontCount);
        if (!segmentHasByteRange)
          rep->SetHasSegmentsUrl(true);

        uint64_t duration{0};
        if (!newSegments.IsEmpty())
          duration = currentSegStartPts - newSegments.Get(0)->startPTS_;
        rep->SetDuration(duration);

        if (adp->GetStreamType() != StreamType::SUBTITLE)
        {
          uint64_t periodDuration =
              (rep->GetDuration() * m_periods[discontCount]->GetTimescale()) / rep->GetTimescale();
          period->SetDuration(periodDuration);
        }

        FreeSegments(period, rep);
        rep->SegmentTimeline().Swap(newSegments);
        rep->SetStartNumber(newStartNumber);

        if (hasSegmentInit)
        {
          std::swap(rep->initialization_, segInit);
          // EXT-X-MAP init url must persist to next period until overrided by new tag
          segInit.url = segInitUrl;
        }
        if (m_periods.size() == ++discontCount)
        {
          auto newPeriod = CPeriod::MakeUniquePtr();
          newPeriod->CopyHLSData(m_currentPeriod);
          period = newPeriod.get();
          m_periods.push_back(std::move(newPeriod));
        }
        else
          period = m_periods[discontCount].get();

        newStartNumber += rep->SegmentTimeline().GetSize();
        adp = period->GetAdaptationSets()[adpSetPos].get();
        rep = adp->GetRepresentations()[reprPos].get();

        currentSegStartPts = 0;

        if (currentEncryptionType == EncryptionType::WIDEVINE)
        {
          rep->m_psshSetPos = InsertPsshSet(adp->GetStreamType(), period, adp, m_currentPssh,
                                            m_currentDefaultKID, m_currentIV);
          period->SetEncryptionState(EncryptionState::ENCRYPTED_SUPPORTED);
        }

        if (hasSegmentInit && !segInitUrl.empty())
        {
          rep->SetHasInitialization(true);
          rep->SetContainerType(ContainerType::MP4);
        }
      }
      else if (tagName == "#EXT-X-ENDLIST")
      {
        m_refreshPlayList = false;
        has_timeshift_buffer_ = false;
      }
    }

    if (!isExtM3Uformat)
    {
      LOG::LogF(LOGERROR, "Non-compliant HLS manifest, #EXTM3U tag not found.");
      return PrepareRepStatus::FAILURE;
    }

    if (!segmentHasByteRange)
      rep->SetHasSegmentsUrl(true);

    FreeSegments(period, rep);

    if (newSegments.IsEmpty())
    {
      LOG::LogF(LOGERROR, "No segments parsed.");
      return PrepareRepStatus::FAILURE;
    }

    rep->SegmentTimeline().Swap(newSegments);
    rep->SetStartNumber(newStartNumber);

    if (hasSegmentInit)
      std::swap(rep->initialization_, segInit);

    uint64_t reprDuration{0};
    if (rep->SegmentTimeline().Get(0))
      reprDuration = currentSegStartPts - rep->SegmentTimeline().Get(0)->startPTS_;

    rep->SetDuration(reprDuration);
    period->SetSequence(m_discontSeq + discontCount);

    uint64_t totalTimeSecs = 0;
    if (discontCount > 0 || m_hasDiscontSeq)
    {
      if (adp->GetStreamType() != StreamType::SUBTITLE)
      {
        uint64_t periodDuration =
            (rep->GetDuration() * m_periods[discontCount]->GetTimescale()) / rep->GetTimescale();
        m_periods[discontCount]->SetDuration(periodDuration);
      }

      for (auto& p : m_periods)
      {
        totalTimeSecs += p->GetDuration() / p->GetTimescale();
        if (!has_timeshift_buffer_ && !m_refreshPlayList)
        {
          auto& adpSet = p->GetAdaptationSets()[adpSetPos];
          adpSet->GetRepresentations()[reprPos]->m_isDownloaded = true;
        }
      }
    }
    else
    {
      totalTimeSecs = rep->GetDuration() / rep->GetTimescale();
      if (!has_timeshift_buffer_ && !m_refreshPlayList)
      {
        rep->m_isDownloaded = true;
      }
    }

    if (adp->GetStreamType() != StreamType::SUBTITLE)
      m_totalTimeSecs = totalTimeSecs;
  }

  if (update)
  {
    if (currentRepSegNumber == 0 || currentRepSegNumber < entryRep->GetStartNumber() ||
        currentRepSegNumber == SEGMENT_NO_NUMBER)
    {
      entryRep->current_segment_ = nullptr;
    }
    else
    {
      if (currentRepSegNumber >= entryRep->GetStartNumber() + entryRep->SegmentTimeline().GetSize())
      {
        currentRepSegNumber =
            entryRep->GetStartNumber() + entryRep->SegmentTimeline().GetSize() - 1;
      }

      entryRep->current_segment_ = entryRep->get_segment(
          static_cast<size_t>(currentRepSegNumber - entryRep->GetStartNumber()));
    }
    if (entryRep->IsWaitForSegment() && (entryRep->get_next_segment(entryRep->current_segment_) ||
                                         m_currentPeriod != m_periods.back().get()))
    {
      entryRep->SetIsWaitForSegment(false);
    }
  }
  else
    StartUpdateThread();

  if (periodLost)
    m_periods.insert(m_periods.begin(), std::move(periodLost));

  return prepareStatus;
}

void adaptive::CHLSTree::OnDataArrived(uint64_t segNum,
                                       uint16_t psshSet,
                                       uint8_t iv[16],
                                       const char* srcData,
                                       size_t srcDataSize,
                                       std::string& segBuffer,
                                       size_t segBufferSize,
                                       bool isLastChunk)
{
  if (psshSet && m_currentPeriod->GetEncryptionState() != EncryptionState::ENCRYPTED_SUPPORTED)
  {
    std::lock_guard<TreeUpdateThread> lckUpdTree(GetTreeUpdMutex());

    std::vector<CPeriod::PSSHSet>& psshSets = m_currentPeriod->GetPSSHSets();

    if (psshSet >= psshSets.size())
    {
      LOG::LogF(LOGERROR, "Cannot get PSSHSet at position %u", psshSet);
      return;
    }

    CPeriod::PSSHSet& pssh = psshSets[psshSet];

    //Encrypted media, decrypt it
    if (pssh.defaultKID_.empty())
    {
      //First look if we already have this URL resolved
      for (auto itPsshSet = m_currentPeriod->GetPSSHSets().begin();
           itPsshSet != m_currentPeriod->GetPSSHSets().end(); itPsshSet++)
      {
        if (itPsshSet->pssh_ == pssh.pssh_ && !itPsshSet->defaultKID_.empty())
        {
          pssh.defaultKID_ = itPsshSet->defaultKID_;
          break;
        }
      }

      if (pssh.defaultKID_.empty())
      {
      RETRY:
        std::map<std::string, std::string> headers;
        std::vector<std::string> keyParts{StringUtils::Split(m_decrypter->getLicenseKey(), '|')};
        std::string url = pssh.pssh_.c_str();

        if (keyParts.size() > 0)
        {
          URL::AppendParameters(url, keyParts[0]);
        }
        if (keyParts.size() > 1)
          ParseHeaderString(headers, keyParts[1]);

        std::string data;
        HTTPRespHeaders respHeaders;

        if (Download(url, headers, data, respHeaders))
        {
          pssh.defaultKID_ = data;
        }
        else if (pssh.defaultKID_ != "0")
        {
          pssh.defaultKID_ = "0";
          if (keyParts.size() >= 5 && !keyParts[4].empty() &&
              m_decrypter->RenewLicense(keyParts[4]))
            goto RETRY;
        }
      }
    }
    if (pssh.defaultKID_ == "0")
    {
      segBuffer.insert(segBufferSize, srcDataSize, 0);
      return;
    }
    else if (!segBufferSize)
    {
      if (pssh.iv.empty())
        m_decrypter->ivFromSequence(iv, segNum);
      else
      {
        memset(iv, 0, 16);
        memcpy(iv, pssh.iv.data(), pssh.iv.size() < 16 ? pssh.iv.size() : 16);
      }
    }

    // Decrypter needs preallocated string data
    segBuffer.resize(segBufferSize + srcDataSize);

    m_decrypter->decrypt(reinterpret_cast<const uint8_t*>(pssh.defaultKID_.data()), iv,
                         reinterpret_cast<const AP4_UI08*>(srcData), segBuffer, segBufferSize,
                         srcDataSize, isLastChunk);
    if (srcDataSize >= 16)
      memcpy(iv, srcData + (srcDataSize - 16), 16);
  }
  else
    AdaptiveTree::OnDataArrived(segNum, psshSet, iv, srcData, srcDataSize, segBuffer, segBufferSize,
                                isLastChunk);
}

//Called each time before we switch to a new segment
void adaptive::CHLSTree::RefreshSegments(PLAYLIST::CPeriod* period,
                                         PLAYLIST::CAdaptationSet* adp,
                                         PLAYLIST::CRepresentation* rep,
                                         PLAYLIST::StreamType type)
{
  if (m_refreshPlayList)
  {
    if (rep->IsIncludedStream())
      return;

    m_updThread.ResetStartTime();
    prepareRepresentation(period, adp, rep, true);
  }
}

// Can be called form update-thread!
//! @todo: check updated variables that are not thread safe
void adaptive::CHLSTree::RefreshLiveSegments()
{
  lastUpdated_ = std::chrono::system_clock::now();

  if (!m_refreshPlayList)
    return;

  std::vector<std::tuple<CAdaptationSet*, CRepresentation*>> refreshList;

  for (auto& adpSet : m_currentPeriod->GetAdaptationSets())
  {
    for (auto& repr : adpSet->GetRepresentations())
    {
      if (repr->IsEnabled())
        refreshList.emplace_back(std::make_tuple(adpSet.get(), repr.get()));
    }
  }
  for (auto& itemList : refreshList)
  {
    prepareRepresentation(m_currentPeriod, std::get<0>(itemList), std::get<1>(itemList), true);
  }
}

bool adaptive::CHLSTree::ParseManifest(const std::string& data)
{
  // Parse master playlist

  bool isExtM3Uformat{false};

  // Determine if is needed create a dummy audio representation for audio stream embedded on video stream
  bool createDummyAudioRepr{false};

  std::unique_ptr<CPeriod> period = CPeriod::MakeUniquePtr();
  period->SetTimescale(1000000);

  std::stringstream streamData{data};

  for (std::string line; STRING::GetLine(streamData, line);)
  {
    // Keep track of current line pos, can be used to go back to previous line
    // if we move forward within the loop code
    std::streampos currentStreamPos = streamData.tellg();

    std::string tagName;
    std::string tagValue;
    ParseTagNameValue(line, tagName, tagValue);

    // Find the extended M3U file initialization tag
    if (!isExtM3Uformat)
    {
      if (tagName == "#EXTM3U")
        isExtM3Uformat = true;
      continue;
    }

    if (tagName == "#EXT-X-MEDIA")
    {
      auto attribs = ParseTagAttributes(tagValue);

      StreamType streamType = StreamType::NOTYPE;
      if (attribs["TYPE"] == "AUDIO")
        streamType = StreamType::AUDIO;
      else if (attribs["TYPE"] == "SUBTITLES")
        streamType = StreamType::SUBTITLE;
      else
        continue;

      // Create or get existing group id
      ExtGroup& group = m_extGroups[attribs["GROUP-ID"]];

      auto adpSet = CAdaptationSet::MakeUniquePtr(period.get());
      auto repr = CRepresentation::MakeUniquePtr(adpSet.get());

      adpSet->SetStreamType(streamType);
      adpSet->SetLanguage(attribs["LANGUAGE"].empty() ? "unk" : attribs["LANGUAGE"]);
      adpSet->SetName(attribs["NAME"]);
      adpSet->SetIsDefault(attribs["DEFAULT"] == "YES");
      adpSet->SetIsForced(attribs["FORCED"] == "YES");

      repr->AddCodecs(group.m_codecs);
      repr->SetTimescale(1000000);

      if (STRING::KeyExists(attribs, "URI"))
      {
        repr->SetSourceUrl(BuildDownloadUrl(attribs["URI"]));

        if (streamType == StreamType::SUBTITLE)
        {
          // default to WebVTT
          repr->AddCodecs("wvtt");
        }
      }
      else
      {
        repr->SetIsIncludedStream(true);
        period->m_includedStreamType |= 1U << static_cast<int>(streamType);
      }

      if (streamType == StreamType::AUDIO)
      {
        repr->SetAudioChannels(STRING::ToUint32(attribs["CHANNELS"], 2));
      }

      repr->assured_buffer_duration_ = m_settings.m_bufferAssuredDuration;
      repr->max_buffer_duration_ = m_settings.m_bufferMaxDuration;

      repr->SetScaling();

      // Add the representation/adaptation set to the group
      adpSet->AddRepresentation(repr);
      group.m_adpSets.push_back(std::move(adpSet));
    }
    else if (tagName == "#EXT-X-STREAM-INF")
    {
      //! @todo: If CODECS value is not present, get StreamReps from stream program section
      // #EXT-X-STREAM-INF:BANDWIDTH=263851,CODECS="mp4a.40.2, avc1.4d400d",RESOLUTION=416x234,AUDIO="bipbop_audio",SUBTITLES="subs"

      auto attribs = ParseTagAttributes(tagValue);

      if (!STRING::KeyExists(attribs, "BANDWIDTH"))
      {
        LOG::LogF(LOGERROR, "Skipped EXT-X-STREAM-INF due to to missing bandwidth attribute (%s)",
                  tagValue.c_str());
        continue;
      }

      if (period->GetAdaptationSets().empty())
      {
        auto newAdpSet = CAdaptationSet::MakeUniquePtr(period.get());
        newAdpSet->SetStreamType(StreamType::VIDEO);
        period->AddAdaptationSet(newAdpSet);
      }

      CAdaptationSet* adpSet = period->GetAdaptationSets()[0].get();

      auto repr = CRepresentation::MakeUniquePtr(adpSet);
      repr->SetTimescale(1000000);

      if (STRING::KeyExists(attribs, "CODECS"))
        repr->AddCodecs(attribs["CODECS"]);
      else
      {
        LOG::LogF(LOGDEBUG, "Missing CODECS attribute, fallback to h264");
        repr->AddCodecs("h264");
      }

      repr->SetBandwidth(STRING::ToUint32(attribs["BANDWIDTH"]));

      if (STRING::KeyExists(attribs, "RESOLUTION"))
      {
        int resWidth{0};
        int resHeight{0};
        ParseResolution(resWidth, resHeight, attribs["RESOLUTION"]);
        repr->SetResWidth(resWidth);
        repr->SetResHeight(resHeight);
      }

      if (STRING::KeyExists(attribs, "AUDIO"))
      {
        // Set codecs to the representations of audio group
        m_extGroups[attribs["AUDIO"]].SetCodecs(GetAudioCodec(attribs["CODECS"]));
      }
      else
      {
        // We assume audio is included
        period->m_includedStreamType |= 1U << static_cast<int>(StreamType::AUDIO);
        createDummyAudioRepr = true;
      }

      if (STRING::KeyExists(attribs, "FRAME-RATE"))
      {
        double frameRate = STRING::ToFloat(attribs["FRAME-RATE"]);
        if (frameRate == 0)
        {
          LOG::LogF(LOGWARNING, "Wrong FRAME-RATE attribute, fallback to 60 fps");
          frameRate = 60.0f;
        }
        repr->SetFrameRate(static_cast<uint32_t>(frameRate * 1000));
        repr->SetFrameRateScale(1000);
      }

      repr->assured_buffer_duration_ = m_settings.m_bufferAssuredDuration;
      repr->max_buffer_duration_ = m_settings.m_bufferMaxDuration;

      repr->SetScaling();

      // Try read on the next stream line, to get the playlist URL address
      if (STRING::GetLine(streamData, line) && !line.empty() && line[0] != '#')
      {
        std::string sourceUrl = BuildDownloadUrl(line);

        // Ensure that we do not add duplicate URLs / representations
        auto itRepr =
            std::find_if(adpSet->GetRepresentations().begin(), adpSet->GetRepresentations().end(),
                         [&sourceUrl](const std::unique_ptr<CRepresentation>& r)
                         { return r->GetSourceUrl() == sourceUrl; });

        if (itRepr == adpSet->GetRepresentations().end())
        {
          repr->SetSourceUrl(sourceUrl);
          adpSet->AddRepresentation(repr);
        }
      }
      else
      {
        // Malformed line, rollback stream to previous line position
        streamData.seekg(currentStreamPos);
      }
    }
    else if (tagName == "#EXTINF")
    {
      // This is not a multi - bitrate playlist

      auto newAdpSet = CAdaptationSet::MakeUniquePtr(period.get());
      newAdpSet->SetStreamType(StreamType::VIDEO);

      auto repr = CRepresentation::MakeUniquePtr(newAdpSet.get());
      repr->SetTimescale(1000000);
      repr->SetSourceUrl(manifest_url_);

      repr->assured_buffer_duration_ = m_settings.m_bufferAssuredDuration;
      repr->max_buffer_duration_ = m_settings.m_bufferMaxDuration;

      repr->SetScaling();

      newAdpSet->AddRepresentation(repr);
      period->AddAdaptationSet(newAdpSet);

      // We assume audio is included
      period->m_includedStreamType |= 1U << static_cast<int>(StreamType::AUDIO);
      createDummyAudioRepr = true;
      break;
    }
    else if (tagName == "#EXT-X-SESSION-KEY")
    {
      auto attribs = ParseTagAttributes(tagValue);

      switch (ProcessEncryption(base_url_, attribs))
      {
        case EncryptionType::NOT_SUPPORTED:
          return false;
        case EncryptionType::AES128:
        case EncryptionType::WIDEVINE:
          // #EXT-X-SESSION-KEY is meant for preparing DRM without
          // loading sub-playlist. As long our workflow is serial, we
          // don't profite and therefore do not any action.
          break;
        case EncryptionType::UNKNOWN:
          LOG::LogF(LOGWARNING, "Unknown encryption type");
          break;
        default:
          break;
      }
    }
  }

  if (!isExtM3Uformat)
  {
    LOG::LogF(LOGERROR, "Non-compliant HLS manifest, #EXTM3U tag not found.");
    return false;
  }

  if (createDummyAudioRepr)
  {
    // We may need to create the Default / Dummy audio representation

    auto newAdpSet = CAdaptationSet::MakeUniquePtr(period.get());
    newAdpSet->SetStreamType(StreamType::AUDIO);
    newAdpSet->SetContainerType(ContainerType::MP4);
    newAdpSet->SetLanguage("unk"); // Unknown

    auto repr = CRepresentation::MakeUniquePtr(newAdpSet.get());
    repr->SetTimescale(1000000);

    // Try to get the codecs from first representation
    std::string codec = "aac";
    auto& adpSets = period->GetAdaptationSets();
    if (!adpSets.empty())
    {
      auto& reprs = adpSets[0]->GetRepresentations();
      if (!reprs.empty())
        codec = GetAudioCodec(reprs[0].get());
    }
    repr->AddCodecs(codec);
    repr->SetAudioChannels(2);
    repr->SetIsIncludedStream(true);

    repr->assured_buffer_duration_ = m_settings.m_bufferAssuredDuration;
    repr->max_buffer_duration_ = m_settings.m_bufferMaxDuration;

    repr->SetScaling();

    newAdpSet->AddRepresentation(repr);
    period->AddAdaptationSet(newAdpSet);
  }

  // Add adaptation sets from groups
  for (auto& group : m_extGroups)
  {
    for (auto& adpSet : group.second.m_adpSets)
    {
      period->AddAdaptationSet(adpSet);
    }
  }
  m_extGroups.clear();

  // Set Live as default
  has_timeshift_buffer_ = true;
  m_manifestUpdateParam = "full";

  m_periods.push_back(std::move(period));

  return true;
}

PLAYLIST::EncryptionType adaptive::CHLSTree::ProcessEncryption(
    std::string_view baseUrl, std::map<std::string, std::string>& attribs)
{
  std::string encryptMethod = attribs["METHOD"];

  // NO ENCRYPTION
  if (encryptMethod == "NONE")
  {
    m_currentPssh.clear();

    return EncryptionType::CLEAR;
  }

  // AES-128
  if (encryptMethod == "AES-128" && !attribs["URI"].empty())
  {
    m_currentPssh = attribs["URI"];
    if (URL::IsUrlRelative(m_currentPssh))
      m_currentPssh = URL::Join(baseUrl.data(), m_currentPssh);

    m_currentIV = m_decrypter->convertIV(attribs["IV"]);

    return EncryptionType::AES128;
  }

  // WIDEVINE
  if (STRING::CompareNoCase(attribs["KEYFORMAT"],
                            "urn:uuid:edef8ba9-79d6-4ace-a3c8-27dcd51d21ed") &&
      !attribs["URI"].empty())
  {
    if (!attribs["KEYID"].empty())
    {
      std::string keyid = attribs["KEYID"].substr(2);
      const char* defaultKID = keyid.c_str();
      m_currentDefaultKID.resize(16);
      for (unsigned int i(0); i < 16; ++i)
      {
        m_currentDefaultKID[i] = STRING::ToHexNibble(*defaultKID) << 4;
        ++defaultKID;
        m_currentDefaultKID[i] |= STRING::ToHexNibble(*defaultKID);
        ++defaultKID;
      }
    }

    m_currentPssh = attribs["URI"].substr(23);
    // Try to get KID from pssh, we assume len+'pssh'+version(0)+systemid+lenkid+kid
    if (m_currentDefaultKID.empty() && m_currentPssh.size() == 68)
    {
      std::string decPssh{BASE64::Decode(m_currentPssh)};
      if (decPssh.size() == 50)
        m_currentDefaultKID = decPssh.substr(34, 16);
    }
    if (encryptMethod == "SAMPLE-AES-CTR")
      m_cryptoMode = CryptoMode::AES_CTR;
    else if (encryptMethod == "SAMPLE-AES")
      m_cryptoMode = CryptoMode::AES_CBC;

    return EncryptionType::WIDEVINE;
  }

  // KNOWN UNSUPPORTED
  if (STRING::CompareNoCase(attribs["KEYFORMAT"], "com.apple.streamingkeydelivery"))
  {
    LOG::LogF(LOGDEBUG, "Keyformat %s not supported", attribs["KEYFORMAT"].c_str());
    return EncryptionType::NOT_SUPPORTED;
  }

  return EncryptionType::UNKNOWN;
}

void adaptive::CHLSTree::SaveManifest(PLAYLIST::CAdaptationSet* adpSet,
                                      std::string_view data,
                                      std::string_view info)
{
  if (m_pathSaveManifest.empty())
    return;

  std::string fileNameSuffix = "master";
  if (adpSet)
  {
    fileNameSuffix = "child-";
    fileNameSuffix += StreamTypeToString(adpSet->GetStreamType());
  }

  AdaptiveTree::SaveManifest(fileNameSuffix, data, info);
}
