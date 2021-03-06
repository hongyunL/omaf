
/** 
 * This file is part of Nokia OMAF implementation
 *
 * Copyright (c) 2018 Nokia Corporation and/or its subsidiary(-ies). All rights reserved.
 *
 * Contact: omaf@nokia.com
 *
 * This software, including documentation, is protected by copyright controlled by Nokia Corporation and/ or its
 * subsidiaries. All rights are reserved.
 *
 * Copying, including reproducing, storing, adapting or translating, any or all of this material requires the prior
 * written consent of Nokia.
 */
#include "DashProvider/NVRDashAdaptationSetExtractor.h"
#include "VideoDecoder/NVRVideoDecoderManager.h"
#include "DashProvider/NVRMPDAttributes.h"
#include "DashProvider/NVRMPDExtension.h"
#include "Foundation/NVRLogger.h"
#include "Media/NVRMediaType.h"
#include "DashProvider/NVRDashRepresentationExtractor.h"
#include "DashProvider/NVRDashRepresentationTile.h"

OMAF_NS_BEGIN
    OMAF_LOG_ZONE(DashAdaptationSetExtractor)

    DashAdaptationSetExtractor::DashAdaptationSetExtractor(DashAdaptationSetObserver& observer)
    : DashAdaptationSetTile(observer)
        , mNextSegmentToBeConcatenated(1)
    {

    }

    DashAdaptationSetExtractor::~DashAdaptationSetExtractor()
    {
    }

    bool_t DashAdaptationSetExtractor::isExtractor(DashComponents aDashComponents)
    {
        for (size_t codecIndex = 0; codecIndex < aDashComponents.adaptationSet->GetCodecs().size(); codecIndex++)
        {
            const std::string& codec = aDashComponents.adaptationSet->GetCodecs().at(codecIndex);
            if (codec.find("hvc2") != std::string::npos)
            {
                return true;
            }
        }
        return false;
    }

    uint32_t DashAdaptationSetExtractor::parseId(DashComponents aDashComponents)
    {
        return aDashComponents.adaptationSet->GetId();
    }

    AdaptationSetBundleIds DashAdaptationSetExtractor::hasPreselection(DashComponents aDashComponents, uint32_t aAdaptationSetId)
    {
        AdaptationSetBundleIds supportingSetIds;
        // In OMAF, Preselection links adaptation sets. Not relevant with the representations
        DashOmafAttributes::getOMAFPreselection(aDashComponents, aAdaptationSetId, supportingSetIds);
        return supportingSetIds;
    }

    RepresentationDependencies DashAdaptationSetExtractor::hasDependencies(DashComponents aDashComponents)
    {
        RepresentationDependencies dependingRepresentationIds;
        // check for dependencies too. We add all representation ids that all representations of this set depends on into the same list, but grouped
        DashOmafAttributes::getOMAFRepresentationDependencies(aDashComponents, dependingRepresentationIds);
        return dependingRepresentationIds;
    }

    bool_t DashAdaptationSetExtractor::hasMultiResolution(DashComponents aDashComponents)
    {
        bool_t multiResolution;
        dash::mpd::IRepresentation *representation = aDashComponents.adaptationSet->GetRepresentation().at(0);
        Error::Enum result = DashOmafAttributes::getOMAFQualityRankingType(representation->GetAdditionalSubNodes(), multiResolution);
        if (result == Error::ITEM_NOT_FOUND)
        {
            // try adaptation set level - TODO should this be under the DashOmafAttributes
            result = DashOmafAttributes::getOMAFQualityRankingType(aDashComponents.adaptationSet->GetAdditionalSubNodes(), multiResolution);
        }

        if (result != Error::OK)
        {
            return false;
        }
        return multiResolution;
    }


    AdaptationSetType::Enum DashAdaptationSetExtractor::getType() const
    {
        return AdaptationSetType::EXTRACTOR;
    }

    Error::Enum DashAdaptationSetExtractor::initialize(DashComponents aDashComponents, uint32_t& aInitializationSegmentId)
    {
        // In OMAF, Preselection links adaptation sets. Not relevant with the representations
        DashOmafAttributes::getOMAFPreselection(aDashComponents, mAdaptationSetId, mSupportingSetIds);
        return doInitialize(aDashComponents, aInitializationSegmentId);
    }

    Error::Enum DashAdaptationSetExtractor::doInitialize(DashComponents aDashComponents, uint32_t& aInitializationSegmentId)
    {
        //TODO anything special here? At least select the mCurrentRepresentation
        return DashAdaptationSet::doInitialize(aDashComponents, aInitializationSegmentId);
    }

    DashRepresentation* DashAdaptationSetExtractor::createRepresentation(DashComponents aDashComponents, uint32_t aInitializationSegmentId, uint32_t aBandwidth)
    {
        DashRepresentationExtractor* newrepresentation = OMAF_NEW(mMemoryAllocator, DashRepresentationExtractor);
        if (newrepresentation->initialize(aDashComponents, aBandwidth, mContent, aInitializationSegmentId, this) != Error::OK)
        {
            OMAF_LOG_W("Non-supported representation skipped");
            OMAF_DELETE(mMemoryAllocator, newrepresentation);
            return OMAF_NULL;
        }
        aInitializationSegmentId++;

        if (mCoveredViewport == OMAF_NULL)
        {
            // extractor is always video
            parseVideoViewport(aDashComponents);
        }

        // set cache mode: auto
        newrepresentation->setCacheFillMode(true);

        return newrepresentation;
    }

    uint32_t DashAdaptationSetExtractor::getCurrentBandwidth() const
    {
        if (mCurrentRepresentation != OMAF_NULL)
        {
            uint32_t bitrate = mCurrentRepresentation->getBitrate();
            if (mContent.matches(MediaContent::Type::VIDEO_EXTRACTOR) && !mSupportingSets.isEmpty())
            {
                for (SupportingAdaptationSets::ConstIterator it = mSupportingSets.begin(); it != mSupportingSets.end(); ++it)
                {
                    bitrate += (*it)->getCurrentBandwidth();
                }
            }
            return bitrate;
        }
        else
        {
            return 0;
        }
    }

    Error::Enum DashAdaptationSetExtractor::startDownload(time_t startTime)
    {
        mTargetNextSegmentId = 0;
        return DashAdaptationSet::startDownload(startTime);
    }

    Error::Enum DashAdaptationSetExtractor::startDownload(uint64_t overridePTSUs, uint32_t overrideSegmentId, VideoStreamMode::Enum aMode, uint32_t aExpectedPingTimeMs)
    {
        mExpectedPingTimeMs = aExpectedPingTimeMs;
        mTargetNextSegmentId = overrideSegmentId;
        return DashAdaptationSet::startDownload(overridePTSUs, overrideSegmentId, aMode, aExpectedPingTimeMs);
    }

    Error::Enum DashAdaptationSetExtractor::stopDownload()
    {
        if (mSupportingSets.getSize() > 0)
        {
            for (SupportingAdaptationSets::Iterator it = mSupportingSets.begin(); it != mSupportingSets.end(); ++it)
            {
                (*it)->stopDownload();
            }
        }
        return DashAdaptationSet::stopDownload();
    }

    Error::Enum DashAdaptationSetExtractor::stopDownloadAsync(bool_t aReset)
    {
        if (mSupportingSets.getSize() > 0)
        {
            for (SupportingAdaptationSets::Iterator it = mSupportingSets.begin(); it != mSupportingSets.end(); ++it)
            {
                (*it)->stopDownloadAsync(aReset);
            }
        }
        return DashAdaptationSet::stopDownloadAsync(aReset);
    }

    void_t DashAdaptationSetExtractor::clearDownloadedContent()
    {
        DashAdaptationSet::clearDownloadedContent();
        if (mSupportingSets.getSize() > 0)
        {
            for (SupportingAdaptationSets::Iterator it = mSupportingSets.begin(); it != mSupportingSets.end(); ++it)
            {
                (*it)->clearDownloadedContent();
            }
        }
    }

    bool_t DashAdaptationSetExtractor::isBuffering()
    {
        bool_t isBuffering = DashAdaptationSet::isBuffering();
        if (!isBuffering && !mSupportingSets.isEmpty())
        {
            for (SupportingAdaptationSets::Iterator it = mSupportingSets.begin(); it != mSupportingSets.end(); ++it)
            {
                if ((*it)->isBuffering())
                {
                    OMAF_LOG_V("%s is buffering", (*it)->getCurrentRepresentation()->getId());
                    return true;
                }
            }
        }
        return isBuffering;
    }

    Error::Enum DashAdaptationSetExtractor::seekToMs(uint64_t& aSeekToTargetMs, uint64_t& aSeekToResultMs)
    {
        Error::Enum result = mCurrentRepresentation->seekToMs(aSeekToTargetMs, aSeekToResultMs);
        // other representations should get the indices right when doing quality / ABR switch

        if (mSupportingSets.getSize() > 0)
        {
            // this is an OMAF extractor track
            uint64_t resultMs;
            for (SupportingAdaptationSets::Iterator it = mSupportingSets.begin(); it != mSupportingSets.end(); ++it)
            {
                (*it)->seekToMs(aSeekToTargetMs, resultMs);
            }
        }
        return result;
    }

    bool_t DashAdaptationSetExtractor::processSegmentDownload()
    {
        mCurrentRepresentation->processSegmentDownload();

        if (mSupportingSets.getSize() > 0)
        {
            // this is an OMAF extractor track
            uint32_t nextSegmentId = peekNextSegmentId();
            if (getIsInitialized())
            {
                for (SupportingAdaptationSets::Iterator it = mSupportingSets.begin(); it != mSupportingSets.end(); ++it)
                {
                    if ((*it)->isActive())
                    {
                        (*it)->processSegmentDownloadTile(nextSegmentId, true);
                    }
                    else
                    {
                        if (mTargetNextSegmentId > 0)
                        {
                            (*it)->startDownload(OMAF_UINT64_MAX, mTargetNextSegmentId, VideoStreamMode::BASE, mExpectedPingTimeMs);
                        }
                        else
                        {
                            (*it)->startDownload(mDownloadStartTime);
                        }
                    }
                }
            }

            if (mCurrentRepresentation->readyForSegment(nextSegmentId))
            {
                concatenateAndParseSegments();
            }
            else
            {
                //OMAF_LOG_V("skip concatenate %d", nextSegmentId);
            }
        }

        // this class supports only extractors with single representation
        return false;
    }



    void_t DashAdaptationSetExtractor::parseVideoViewport(DashComponents& aNextComponents)
    {
        DashAttributes::Coverage coverage;

        Error::Enum result = DashOmafAttributes::getOMAFVideoViewport(aNextComponents, coverage, mSourceType);
        if (result == Error::OK_NO_CHANGE)
        {
            mContent.addType(MediaContent::Type::VIDEO_BASE);
        }
        else if (result == Error::OK)
        {
            if (mSourceType == SourceType::EQUIRECTANGULAR_PANORAMA)
            {
                if (mCoveredViewport == OMAF_NULL)
                {
                    mCoveredViewport = OMAF_NEW(mMemoryAllocator, VASTileViewport)();
                }
                mCoveredViewport->set(coverage.azimuthCenter, coverage.elevationCenter, coverage.azimuthRange, coverage.elevationRange, VASLongitudeDirection::COUNTER_CLOCKWISE);
            }
            else if (mSourceType == SourceType::CUBEMAP)
            {
                if (mCoveredViewport == OMAF_NULL)
                {
                    mCoveredViewport = OMAF_NEW(mMemoryAllocator, VASTileViewportCube)();
                }
                mCoveredViewport->set(coverage.azimuthCenter, coverage.elevationCenter, coverage.azimuthRange, coverage.elevationRange, VASLongitudeDirection::COUNTER_CLOCKWISE);
                OMAF_LOG_V("Created cubemap tile at (%f, %f), range (%f, %f)", coverage.azimuthCenter, coverage.elevationCenter, coverage.azimuthRange, coverage.elevationRange);
            }
        }
        else
        {
            return;
        }
    }

    const AdaptationSetBundleIds& DashAdaptationSetExtractor::getSupportingSets() const
    {
        return mSupportingSetIds;
    }

    void_t DashAdaptationSetExtractor::addSupportingSet(DashAdaptationSetTile* aSupportingSet)
    {
        mSupportingSets.add(aSupportingSet);
        //OMAF_LOG_V("addSupportingSet %d for %d", aSupportingSet->getId(), mAdaptationSetId);
    }

    void_t DashAdaptationSetExtractor::concatenateAndParseSegments()
    {
        // check if we have segments to concatenate
        DashSegment* segment = mCurrentRepresentation->peekSegment();
        if (segment != OMAF_NULL)
        {
            if (segment->getSegmentId() < mNextSegmentToBeConcatenated)
            {
                OMAF_LOG_V("Too old segment!");
                mCurrentRepresentation->cleanUpOldSegments(mNextSegmentToBeConcatenated);
                segment = mCurrentRepresentation->peekSegment();
                if (segment == OMAF_NULL)
                {
                    return;
                }
            }
            OMAF_LOG_V("concatenateAndParseSegments %d, repr %s", segment->getSegmentId(), mCurrentRepresentation->getId());
            size_t totalSize = segment->getDataBuffer()->getSize();
            uint32_t segmentId = segment->getSegmentId();
            bool_t available = true;
            for (SupportingAdaptationSets::Iterator it = mSupportingSets.begin(); it != mSupportingSets.end(); ++it)
            {
                size_t segmentSize = 0;
                if ((*it)->getCurrentRepresentation()->hasSegment(segmentId, segmentSize))
                {
                    totalSize += segmentSize;
                }
                else
                {
                    available = false;
                }
            }
            if (available)
            {
                // concatenate and feed to the mp4 parser. Note: the ownership of the segment is given to the parser who frees the memory
                DashSegment* concatenatedSegment = OMAF_NEW_HEAP(DashSegment)(totalSize, segment->getSegmentId(), segment->getInitSegmentId(), false, OMAF_NULL);
                segment = mCurrentRepresentation->getSegment();
                uint8_t* ptr = concatenatedSegment->getDataBuffer()->getDataPtr();
                size_t index = 0;
                memcpy(ptr, segment->getDataBuffer()->getDataPtr(), segment->getDataBuffer()->getSize());
                index += segment->getDataBuffer()->getSize();
                concatenatedSegment->addSrcSegment(segment);
                for (SupportingAdaptationSets::Iterator it = mSupportingSets.begin(); it != mSupportingSets.end(); ++it)
                {
                    segment = (*it)->getCurrentRepresentation()->getSegment();
                    // id was already checked above
                    memcpy(ptr + index, segment->getDataBuffer()->getDataPtr(), segment->getDataBuffer()->getSize());
                    index += segment->getDataBuffer()->getSize();
                    concatenatedSegment->addSrcSegment(segment);
                    OMAF_LOG_V("Concatenate segment %d from %s", segmentId, (*it)->getCurrentRepresentationId().getData());
                }
                concatenatedSegment->getDataBuffer()->setSize(index);
                mNextSegmentToBeConcatenated = segmentId + 1;

                mCurrentRepresentation->parseConcatenatedMediaSegment(concatenatedSegment);
            }
        }

    }

    bool_t DashAdaptationSetExtractor::updateBitrate(size_t aNrForegroundTiles, uint8_t aQualityForeground, uint8_t aQualityBackground, uint8_t aNrLevels)
    {
        if (mSupportingSets.isEmpty())
        {
            return false;
        }

        uint32_t bitrate = getRepresentationForQuality(aQualityForeground, aNrLevels)->getBitrate();
        uint8_t quality = aQualityForeground;
        // TODO this is now just taking the first aNrForegroundTiles tiles for foreground, ignoring their position in sphere. Might be better to check they are around horizon?
        for (size_t j = 0; j < mSupportingSets.getSize(); j++)
        {
            if (j == aNrForegroundTiles)
            {
                quality = aQualityBackground;
            }
            bitrate += mSupportingSets[j]->getRepresentationForQuality(quality, aNrLevels)->getBitrate();
        }
        mBitrates.add(bitrate);
        return true;
    }


OMAF_NS_END
