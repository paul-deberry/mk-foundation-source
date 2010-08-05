/*
 * $Id$
 * Copyright (c) 2010, Matroska Foundation
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in the
 *       documentation and/or other materials provided with the distribution.
 *     * Neither the name of the Matroska Foundation nor the
 *       names of its contributors may be used to endorse or promote products
 *       derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY The Matroska Foundation ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL The Matroska Foundation BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */
#include "mkvalidator_stdafx.h"
#include "mkvalidator_project.h"
#ifndef CONFIG_EBML_UNICODE
#define CONFIG_EBML_UNICODE
#endif
#include "matroska/matroska.h"

/*!
 * \todo verify that the size of frames inside a lace is legit (ie the remaining size for the last must be > 0)
 * \todo verify that items with a limited set of values don't use other values
 * \todo verify the CRC-32 is valid when it exists
 * \todo verify that timecodes for each track are increasing (for keyframes and p frames)
 * \todo optionally show the use of deprecated elements
 */

static textwriter *StdErr = NULL;
static ebml_element *RSegmentInfo = NULL, *RTrackInfo = NULL, *RChapters = NULL, *RTags = NULL, *RCues = NULL, *RAttachments = NULL, *RSeekHead = NULL, *RSeekHead2 = NULL;
static array RClusters;
static array Tracks;
static size_t TrackMax=0;
static bool_t Warnings = 1;
static bool_t Live = 0;
static bool_t Details = 0;
static bool_t DivX = 0;
static timecode_t MinTime = INVALID_TIMECODE_T, MaxTime = INVALID_TIMECODE_T;
static timecode_t ClusterTime = INVALID_TIMECODE_T;

typedef struct track_info
{
    int Num;
    int Kind;
    ebml_string *CodecID;
    filepos_t DataLength;

} track_info;

#ifdef TARGET_WIN
#include <windows.h>
void DebugMessage(const tchar_t* Msg,...)
{
#if !defined(NDEBUG) || defined(LOGFILE) || defined(LOGTIME)
	va_list Args;
	tchar_t Buffer[1024],*s=Buffer;

	va_start(Args,Msg);
	vstprintf_s(Buffer,TSIZEOF(Buffer), Msg, Args);
	va_end(Args);
	tcscat_s(Buffer,TSIZEOF(Buffer),T("\r\n"));
#endif

#ifdef LOGTIME
    {
        tchar_t timed[1024];
        SysTickToString(timed,TSIZEOF(timed),GetTimeTick(),1,1,0);
        stcatprintf_s(timed,TSIZEOF(timed),T(" %s"),s);
        s = timed;
    }
#endif

#if !defined(NDEBUG)
	OutputDebugString(s);
#endif

#if defined(LOGFILE)
{
    static FILE* f=NULL;
    static char s8[1024];
    size_t i;
    if (!f)
#if defined(TARGET_WINCE)
    {
        tchar_t DocPath[MAXPATH];
        char LogPath[MAXPATH];
        charconv *ToStr = CharConvOpen(NULL,CHARSET_DEFAULT);
        GetDocumentPath(NULL,DocPath,TSIZEOF(DocPath),FTYPE_LOG); // more visible via ActiveSync
        if (!DocPath[0])
            tcscpy_s(DocPath,TSIZEOF(DocPath),T("\\My Documents"));
        if (!PathIsFolder(NULL,DocPath))
            FolderCreate(NULL,DocPath);
        tcscat_s(DocPath,TSIZEOF(DocPath),T("\\corelog.txt"));
        CharConvST(ToStr,LogPath,sizeof(LogPath),DocPath);
        CharConvClose(ToStr);
        f=fopen(LogPath,"a+b");
        if (!f)
            f=fopen("\\corelog.txt","a+b");
    }
#else
        f=fopen("\\corelog.txt","a+b");
#endif
    for (i=0;s[i];++i)
        s8[i]=(char)s[i];
    s8[i]=0;
    fputs(s8,f);
    fflush(f);
}
#endif
}
#endif

static const tchar_t *GetProfileName(size_t ProfileNum)
{
static const tchar_t *Profile[7] = {T("unknown"), T("matroska v1"), T("matroska v2"), T("webm v1"), T("webm v2"), T("divx v1"), T("divx v2") };
	switch (ProfileNum)
	{
	case PROFILE_MATROSKA_V1: return Profile[1];
	case PROFILE_MATROSKA_V2: return Profile[2];
	case PROFILE_WEBM_V1:     return Profile[3];
	case PROFILE_WEBM_V2:     return Profile[4];
	case PROFILE_DIVX_V1:     return Profile[5];
	case PROFILE_DIVX_V2:     return Profile[6];
	default:                  return Profile[0];
	}
}

static int OutputError(int ErrCode, const tchar_t *ErrString, ...)
{
	tchar_t Buffer[MAXLINE];
	va_list Args;
	va_start(Args,ErrString);
	vstprintf_s(Buffer,TSIZEOF(Buffer), ErrString, Args);
	va_end(Args);
	TextPrintf(StdErr,T("\rERR%03X: %s\r\n"),ErrCode,Buffer);
	return -ErrCode;
}

static int OutputWarning(int ErrCode, const tchar_t *ErrString, ...)
{
    if (!Warnings)
        return 0;
    else
    {
	    tchar_t Buffer[MAXLINE];
	    va_list Args;
	    va_start(Args,ErrString);
	    vstprintf_s(Buffer,TSIZEOF(Buffer), ErrString, Args);
	    va_end(Args);
	    TextPrintf(StdErr,T("\rWRN%03X: %s\r\n"),ErrCode,Buffer);
	    return -ErrCode;
    }
}

static filepos_t CheckUnknownElements(ebml_element *Elt)
{
	tchar_t IdStr[32], String[MAXPATH];
	ebml_element *SubElt;
	filepos_t VoidAmount = 0;
	for (SubElt = EBML_MasterChildren(Elt); SubElt; SubElt = EBML_MasterNext(SubElt))
	{
		if (Node_IsPartOf(SubElt,EBML_DUMMY_ID))
		{
			Node_FromStr(Elt,String,TSIZEOF(String),Elt->Context->ElementName);
			EBML_IdToString(IdStr,TSIZEOF(IdStr),SubElt->Context->Id);
			OutputError(12,T("Unknown element in %s %s at %") TPRId64 T(" (size %") TPRId64 T(")"),String,IdStr,SubElt->ElementPosition,SubElt->DataSize);
		}
		else if (Node_IsPartOf(SubElt,EBML_VOID_CLASS))
		{
			VoidAmount = EBML_ElementFullSize(SubElt,0);
		}
		else if (Node_IsPartOf(SubElt,EBML_MASTER_CLASS))
		{
			VoidAmount += CheckUnknownElements(SubElt);
		}
	}
	return VoidAmount;
}

static int64_t gcd(int64_t a, int64_t b)
{
    for (;;)
    {
        int64_t c = a % b;
        if(!c) return b;
        a = b;
        b = c;
    }
}

static int CheckVideoTrack(ebml_element *Track, int TrackNum, int ProfileNum)
{
	int Result = 0;
	ebml_element *Unit, *Elt, *Video, *PixelW, *PixelH;
	Video = EBML_MasterFindFirstElt(Track,&MATROSKA_ContextTrackVideo,0,0);
	if (!Video)
		Result = OutputWarning(0xE0,T("Video track at %") TPRId64 T(" is missing a Video element"),Track->ElementPosition);
	// check the DisplayWidth and DisplayHeight are correct
	else
	{
		int64_t DisplayW,DisplayH;
		PixelW = EBML_MasterFindFirstElt(Video,&MATROSKA_ContextTrackVideoPixelWidth,1,1);
		if (!PixelW)
			Result |= OutputError(0xE1,T("Video track #%d at %") TPRId64 T(" has no pixel width"),TrackNum,Track->ElementPosition);
		PixelH = EBML_MasterFindFirstElt(Video,&MATROSKA_ContextTrackVideoPixelHeight,1,1);
		if (!PixelH)
			Result |= OutputError(0xE2,T("Video track #%d at %") TPRId64 T(" has no pixel height"),TrackNum,Track->ElementPosition);

		Elt = EBML_MasterFindFirstElt(Video,&MATROSKA_ContextTrackVideoDisplayWidth,0,0);
		if (Elt)
			DisplayW = EBML_IntegerValue(Elt);
		else
			DisplayW = EBML_IntegerValue(PixelW);
		Elt = EBML_MasterFindFirstElt(Video,&MATROSKA_ContextTrackVideoDisplayHeight,0,0);
		if (Elt)
			DisplayH = EBML_IntegerValue(Elt);
		else
			DisplayH = EBML_IntegerValue(PixelH);

		if (DisplayH==0)
			Result |= OutputError(0xE7,T("Video track #%d at %") TPRId64 T(" has a null height"),TrackNum,Track->ElementPosition);
		if (DisplayW==0)
			Result |= OutputError(0xE7,T("Video track #%d at %") TPRId64 T(" has a null width"),TrackNum,Track->ElementPosition);

        Unit = EBML_MasterFindFirstElt(Video,&MATROSKA_ContextTrackVideoDisplayUnit,1,1);
		assert(Unit!=NULL);
		if (EBML_IntegerValue(Unit)==MATROSKA_DISPLAY_UNIT_PIXEL && PixelW && PixelH)
		{
			// check if the pixel sizes appear valid
			if (DisplayW < EBML_IntegerValue(PixelW) && DisplayH < EBML_IntegerValue(PixelH))
			{
                int Serious = gcd(DisplayW,DisplayH)==1; // the DAR values were reduced as much as possible
                if (DisplayW*EBML_IntegerValue(PixelH) == DisplayH*EBML_IntegerValue(PixelW))
                    Serious++; // same aspect ratio as the source
                if (8*DisplayW <= EBML_IntegerValue(PixelW) && 8*DisplayH <= EBML_IntegerValue(PixelH))
                    Serious+=2; // too much shrinking compared to the original pixels
                if (ProfileNum!=PROFILE_WEBM_V2 && ProfileNum!=PROFILE_WEBM_V1)
                    --Serious; // in Matroska it's tolerated as it's been operating like that for a while

				if (Serious>2)
					Result |= OutputError(0xE3,T("The output pixels for Video track #%d seem wrong %") TPRId64 T("x%") TPRId64 T("px from %") TPRId64 T("x%") TPRId64,TrackNum,DisplayW,DisplayH,EBML_IntegerValue(PixelW),EBML_IntegerValue(PixelH));
				else if (Serious)
					Result |= OutputWarning(0xE3,T("The output pixels for Video track #%d seem wrong %") TPRId64 T("x%") TPRId64 T("px from %") TPRId64 T("x%") TPRId64,TrackNum,DisplayW,DisplayH,EBML_IntegerValue(PixelW),EBML_IntegerValue(PixelH));
			}
		}

        if (EBML_IntegerValue(Unit)==MATROSKA_DISPLAY_UNIT_DAR)
        {
            // crop values should never exist
            Elt = EBML_MasterFindFirstElt(Video,&MATROSKA_ContextTrackVideoPixelCropTop,0,0);
            if (Elt)
                Result |= OutputError(0xE4,T("Video track #%d is using unconstrained aspect ratio and has top crop at %") TPRId64,TrackNum,Elt->ElementPosition);
            Elt = EBML_MasterFindFirstElt(Video,&MATROSKA_ContextTrackVideoPixelCropBottom,0,0);
            if (Elt)
                Result |= OutputError(0xE4,T("Video track #%d is using unconstrained aspect ratio and has bottom crop at %") TPRId64,TrackNum,Elt->ElementPosition);
            Elt = EBML_MasterFindFirstElt(Video,&MATROSKA_ContextTrackVideoPixelCropLeft,0,0);
            if (Elt)
                Result |= OutputError(0xE4,T("Video track #%d is using unconstrained aspect ratio and has left crop at %") TPRId64,TrackNum,Elt->ElementPosition);
            Elt = EBML_MasterFindFirstElt(Video,&MATROSKA_ContextTrackVideoPixelCropRight,0,0);
            if (Elt)
                Result |= OutputError(0xE4,T("Video track #%d is using unconstrained aspect ratio and has right crop at %") TPRId64,TrackNum,Elt->ElementPosition);
        }
        else
        {
            // crop values should be less than the extended value
            PixelW = EBML_MasterFindFirstElt(Video,&MATROSKA_ContextTrackVideoPixelCropTop,1,1);
            PixelH = EBML_MasterFindFirstElt(Video,&MATROSKA_ContextTrackVideoPixelCropBottom,1,1);
            if (EBML_IntegerValue(PixelW) + EBML_IntegerValue(PixelH) >= DisplayH)
                Result |= OutputError(0xE5,T("Video track #%d is cropping too many vertical pixels %") TPRId64 T(" vs %") TPRId64 T(" + %") TPRId64,TrackNum, DisplayH, EBML_IntegerValue(PixelW), EBML_IntegerValue(PixelH));

            PixelW = EBML_MasterFindFirstElt(Video,&MATROSKA_ContextTrackVideoPixelCropLeft,1,1);
            PixelH = EBML_MasterFindFirstElt(Video,&MATROSKA_ContextTrackVideoPixelCropRight,1,1);
            if (EBML_IntegerValue(PixelW) + EBML_IntegerValue(PixelH) >= DisplayW)
                Result |= OutputError(0xE6,T("Video track #%d is cropping too many horizontal pixels %") TPRId64 T(" vs %") TPRId64 T(" + %") TPRId64,TrackNum, DisplayW, EBML_IntegerValue(PixelW), EBML_IntegerValue(PixelH));
        }
	}
	return Result;
}

static int CheckTracks(ebml_element *Tracks, int ProfileNum)
{
	ebml_element *Track, *TrackType, *TrackNum, *Elt, *Elt2;
	ebml_string *CodecID;
	tchar_t CodecName[MAXPATH],String[MAXPATH];
	int Result = 0;
	Track = EBML_MasterFindFirstElt(Tracks, &MATROSKA_ContextTrackEntry, 0, 0);
	while (Track)
	{
        // check if the codec is valid for the profile
		TrackNum = EBML_MasterFindFirstElt(Track, &MATROSKA_ContextTrackNumber, 1, 1);
		if (TrackNum)
		{
			TrackType = EBML_MasterFindFirstElt(Track, &MATROSKA_ContextTrackType, 1, 1);
			CodecID = (ebml_string*)EBML_MasterFindFirstElt(Track, &MATROSKA_ContextTrackCodecID, 1, 1);
			if (!CodecID)
				Result |= OutputError(0x300,T("Track #%d has no CodecID defined"),(int)EBML_IntegerValue(TrackNum));
			else if (!TrackType)
				Result |= OutputError(0x301,T("Track #%d has no type defined"),(int)EBML_IntegerValue(TrackNum));
			else
			{
				if (ProfileNum==PROFILE_WEBM_V1 || ProfileNum==PROFILE_WEBM_V2)
				{
					if (EBML_IntegerValue(TrackType) != TRACK_TYPE_AUDIO && EBML_IntegerValue(TrackType) != TRACK_TYPE_VIDEO)
						Result |= OutputError(0x302,T("Track #%d type %d not supported for profile '%s'"),(int)EBML_IntegerValue(TrackNum),(int)EBML_IntegerValue(TrackType),GetProfileName(ProfileNum));
					if (CodecID)
					{
						EBML_StringGet(CodecID,CodecName,TSIZEOF(CodecName));
						tcscpy_s(String,TSIZEOF(String),CodecName);
						if (tcscmp(tcsupr(String),CodecName)!=0)
							Result |= OutputWarning(0x307,T("Track #%d codec %s should be uppercase"),(int)EBML_IntegerValue(TrackNum),CodecName);

						if (EBML_IntegerValue(TrackType) == TRACK_TYPE_AUDIO)
						{
							if (!tcsisame_ascii(CodecName,T("A_VORBIS")))
								Result |= OutputError(0x303,T("Track #%d codec %s not supported for profile '%s'"),(int)EBML_IntegerValue(TrackNum),CodecName,GetProfileName(ProfileNum));
						}
						else if (EBML_IntegerValue(TrackType) == TRACK_TYPE_VIDEO)
						{
							if (!tcsisame_ascii(CodecName,T("V_VP8")))
								Result |= OutputError(0x304,T("Track #%d codec %s not supported for profile '%s'"),(int)EBML_IntegerValue(TrackNum),CodecName,GetProfileName(ProfileNum));
						}
					}
				}
			}
		}

        // check if the AttachmentLink values match existing attachments
		TrackType = EBML_MasterFindFirstElt(Track, &MATROSKA_ContextTrackAttachmentLink, 0, 0);
        while (TrackType)
        {
            if (!RAttachments)
            {
                if (TrackNum)
				    Result |= OutputError(0x305,T("Track #%d has attachment links but not attachments in the file"),(int)EBML_IntegerValue(TrackNum));
                else
                    Result |= OutputError(0x305,T("Track at %") TPRId64 T(" has attachment links but not attachments in the file"),Track->ElementPosition);
                break;
            }

            for (Elt=EBML_MasterChildren(RAttachments);Elt;Elt=EBML_MasterNext(Elt))
            {
                if (Elt->Context->Id == MATROSKA_ContextAttachedFile.Id)
                {
                    Elt2 = EBML_MasterFindFirstElt(Elt, &MATROSKA_ContextAttachedFileUID, 0, 0);
                    if (Elt2 && EBML_IntegerValue(Elt2) == EBML_IntegerValue(TrackType))
                        break;
                }
            }
            if (!Elt)
            {
                if (TrackNum)
				    Result |= OutputError(0x306,T("Track #%d attachment link UID 0x%") TPRIx64 T(" not found in attachments"),(int)EBML_IntegerValue(TrackNum),EBML_IntegerValue(TrackType));
                else
                    Result |= OutputError(0x306,T("Track at %") TPRId64 T(" attachment link UID 0x%") TPRIx64 T(" not found in attachments"),Track->ElementPosition,EBML_IntegerValue(TrackType));
            }

            TrackType = EBML_MasterFindNextElt(Track, TrackType, 0, 0);
        }

		Track = EBML_MasterFindNextElt(Tracks, Track, 0, 0);
	}
	return Result;
}

static int CheckProfileViolation(ebml_element *Elt, int ProfileMask)
{
	int Result = 0;
	tchar_t String[MAXPATH],Invalid[MAXPATH];
	ebml_element *SubElt;

	Node_FromStr(Elt,String,TSIZEOF(String),Elt->Context->ElementName);
	if (Node_IsPartOf(Elt,EBML_MASTER_CLASS))
	{
		for (SubElt = EBML_MasterChildren(Elt); SubElt; SubElt = EBML_MasterNext(SubElt))
		{
			if (!Node_IsPartOf(SubElt,EBML_DUMMY_ID))
			{
				const ebml_semantic *i;
				for (i=Elt->Context->Semantic;i->eClass;++i)
				{
					if (i->eClass->Id==SubElt->Context->Id)
					{
						if ((i->DisabledProfile & ProfileMask)!=0)
						{
							Node_FromStr(Elt,Invalid,TSIZEOF(Invalid),i->eClass->ElementName);
							Result |= OutputError(0x201,T("Invalid %s for profile '%s' at %") TPRId64 T(" in %s"),Invalid,GetProfileName(ProfileMask),SubElt->ElementPosition,String);
						}
						break;
					}
				}
				if (Node_IsPartOf(SubElt,EBML_MASTER_CLASS))
					Result |= CheckProfileViolation(SubElt, ProfileMask);
			}
		}
	}

	return Result;
}

static int CheckMandatory(ebml_element *Elt, int ProfileMask)
{
	int Result = 0;
	tchar_t String[MAXPATH],Missing[MAXPATH];
	ebml_element *SubElt;

	Node_FromStr(Elt,String,TSIZEOF(String),Elt->Context->ElementName);
	if (Node_IsPartOf(Elt,EBML_MASTER_CLASS))
	{
		const ebml_semantic *i;
		for (i=Elt->Context->Semantic;i->eClass;++i)
		{
			if ((i->DisabledProfile & ProfileMask)==0 && i->Mandatory && !i->eClass->HasDefault && !EBML_MasterFindChild(Elt,i->eClass))
			{
				Node_FromStr(Elt,Missing,TSIZEOF(Missing),i->eClass->ElementName);
				Result |= OutputError(0x200,T("Missing element %s in %s at %") TPRId64 T(""),Missing,String,Elt->ElementPosition);
			}
            if ((i->DisabledProfile & ProfileMask)==0 && i->Unique && (SubElt=EBML_MasterFindChild(Elt,i->eClass)) && EBML_MasterFindNextElt(Elt,SubElt,0,0))
            {
				Node_FromStr(Elt,Missing,TSIZEOF(Missing),i->eClass->ElementName);
				Result |= OutputError(0x202,T("Unique element %s in %s at %") TPRId64 T(" found more than once"),Missing,String,Elt->ElementPosition);
            }
		}

		for (SubElt = EBML_MasterChildren(Elt); SubElt; SubElt = EBML_MasterNext(SubElt))
		{
			if (Node_IsPartOf(SubElt,EBML_MASTER_CLASS))
				Result |= CheckMandatory(SubElt, ProfileMask);
		}
	}

	return Result;
}

static int CheckSeekHead(ebml_element *SeekHead)
{
	int Result = 0;
	ebml_element *RLevel1 = EBML_MasterFindFirstElt(SeekHead, &MATROSKA_ContextSeek, 0, 0);
    bool_t BSegmentInfo = 0, BTrackInfo = 0, BCues = 0, BTags = 0, BChapters = 0, BAttachments = 0, BSecondSeek = 0;
	while (RLevel1)
	{
		filepos_t Pos = MATROSKA_MetaSeekAbsolutePos((matroska_seekpoint*)RLevel1);
		fourcc_t SeekId = MATROSKA_MetaSeekID((matroska_seekpoint*)RLevel1);
		tchar_t IdString[32];

		EBML_IdToString(IdString,TSIZEOF(IdString),SeekId);
		if (Pos == INVALID_FILEPOS_T)
			Result |= OutputError(0x60,T("The SeekPoint at %") TPRId64 T(" has an unknown position (ID %s)"),RLevel1->ElementPosition,IdString);
		else if (SeekId==0)
			Result |= OutputError(0x61,T("The SeekPoint at %") TPRId64 T(" has no ID defined (position %") TPRId64 T(")"),RLevel1->ElementPosition,Pos);
		else if (SeekId == MATROSKA_ContextSegmentInfo.Id)
		{
			if (!RSegmentInfo)
				Result |= OutputError(0x62,T("The SeekPoint at %") TPRId64 T(" references an unknown SegmentInfo at %") TPRId64 T(""),RLevel1->ElementPosition,Pos);
			else if (RSegmentInfo->ElementPosition != Pos)
				Result |= OutputError(0x63,T("The SeekPoint at %") TPRId64 T(" references a SegmentInfo at wrong position %") TPRId64 T(" (real %") TPRId64 T(")"),RLevel1->ElementPosition,Pos,RSegmentInfo->ElementPosition);
            BSegmentInfo = 1;
		}
		else if (SeekId == MATROSKA_ContextTracks.Id)
		{
			if (!RTrackInfo)
				Result |= OutputError(0x64,T("The SeekPoint at %") TPRId64 T(" references an unknown TrackInfo at %") TPRId64 T(""),RLevel1->ElementPosition,Pos);
			else if (RTrackInfo->ElementPosition != Pos)
				Result |= OutputError(0x65,T("The SeekPoint at %") TPRId64 T(" references a TrackInfo at wrong position %") TPRId64 T(" (real %") TPRId64 T(")"),RLevel1->ElementPosition,Pos,RTrackInfo->ElementPosition);
            BTrackInfo = 1;
		}
		else if (SeekId == MATROSKA_ContextCues.Id)
		{
			if (!RCues)
				Result |= OutputError(0x66,T("The SeekPoint at %") TPRId64 T(" references an unknown Cues at %") TPRId64 T(""),RLevel1->ElementPosition,Pos);
			else if (RCues->ElementPosition != Pos)
				Result |= OutputError(0x67,T("The SeekPoint at %") TPRId64 T(" references a Cues at wrong position %") TPRId64 T(" (real %") TPRId64 T(")"),RLevel1->ElementPosition,Pos,RCues->ElementPosition);
            BCues = 1;
		}
		else if (SeekId == MATROSKA_ContextTags.Id)
		{
			if (!RTags)
				Result |= OutputError(0x68,T("The SeekPoint at %") TPRId64 T(" references an unknown Tags at %") TPRId64 T(""),RLevel1->ElementPosition,Pos);
			else if (RTags->ElementPosition != Pos)
				Result |= OutputError(0x69,T("The SeekPoint at %") TPRId64 T(" references a Tags at wrong position %") TPRId64 T(" (real %") TPRId64 T(")"),RLevel1->ElementPosition,Pos,RTags->ElementPosition);
            BTags = 1;
		}
		else if (SeekId == MATROSKA_ContextChapters.Id)
		{
			if (!RChapters)
				Result |= OutputError(0x6A,T("The SeekPoint at %") TPRId64 T(" references an unknown Chapters at %") TPRId64 T(""),RLevel1->ElementPosition,Pos);
			else if (RChapters->ElementPosition != Pos)
				Result |= OutputError(0x6B,T("The SeekPoint at %") TPRId64 T(" references a Chapters at wrong position %") TPRId64 T(" (real %") TPRId64 T(")"),RLevel1->ElementPosition,Pos,RChapters->ElementPosition);
            BChapters = 1;
		}
		else if (SeekId == MATROSKA_ContextAttachments.Id)
		{
			if (!RAttachments)
				Result |= OutputError(0x6C,T("The SeekPoint at %") TPRId64 T(" references an unknown Attachments at %") TPRId64 T(""),RLevel1->ElementPosition,Pos);
			else if (RAttachments->ElementPosition != Pos)
				Result |= OutputError(0x6D,T("The SeekPoint at %") TPRId64 T(" references a Attachments at wrong position %") TPRId64 T(" (real %") TPRId64 T(")"),RLevel1->ElementPosition,Pos,RAttachments->ElementPosition);
            BAttachments = 1;
		}
		else if (SeekId == MATROSKA_ContextSeekHead.Id)
		{
			if (SeekHead->ElementPosition == Pos)
				Result |= OutputError(0x6E,T("The SeekPoint at %") TPRId64 T(" references references its own SeekHead"),RLevel1->ElementPosition);
			else if (SeekHead == RSeekHead)
            {
                if (!RSeekHead2)
				    Result |= OutputError(0x6F,T("The SeekPoint at %") TPRId64 T(" references an unknown secondary SeekHead at %") TPRId64 T(""),RLevel1->ElementPosition,Pos);
                BSecondSeek = 1;
            }
			else if (SeekHead == RSeekHead2 && Pos!=RSeekHead->ElementPosition)
			    Result |= OutputError(0x70,T("The SeekPoint at %") TPRId64 T(" references an unknown extra SeekHead at %") TPRId64 T(""),RLevel1->ElementPosition,Pos);
		}
		else if (SeekId == MATROSKA_ContextCluster.Id)
		{
			ebml_element **Cluster;
			for (Cluster = ARRAYBEGIN(RClusters,ebml_element*);Cluster != ARRAYEND(RClusters,ebml_element*); ++Cluster)
			{
				if ((*Cluster)->ElementPosition == Pos)
					break;
			}
			if (Cluster == ARRAYEND(RClusters,ebml_element*) && Cluster != ARRAYBEGIN(RClusters,ebml_element*))
				Result |= OutputError(0x71,T("The SeekPoint at %") TPRId64 T(" references a Cluster not found at %") TPRId64 T(""),RLevel1->ElementPosition,Pos);
		}
		else
			Result |= OutputWarning(0x860,T("The SeekPoint at %") TPRId64 T(" references an element that is not a known level 1 ID %s at %") TPRId64 T(")"),RLevel1->ElementPosition,IdString,Pos);
		RLevel1 = EBML_MasterFindNextElt(SeekHead, RLevel1, 0, 0);
	}
    if (SeekHead == RSeekHead)
    {
        if (!BSegmentInfo && RSegmentInfo)
            Result |= OutputWarning(0x861,T("The SegmentInfo is not referenced in the main SeekHead"));
        if (!BTrackInfo && RTrackInfo)
            Result |= OutputWarning(0x861,T("The TrackInfo is not referenced in the main SeekHead"));
        if (!BCues && RCues)
            Result |= OutputWarning(0x861,T("The Cues is not referenced in the main SeekHead"));
        if (!BTags && RTags)
            Result |= OutputWarning(0x861,T("The Tags is not referenced in the main SeekHead"));
        if (!BChapters && RChapters)
            Result |= OutputWarning(0x861,T("The Chapters is not referenced in the main SeekHead"));
        if (!BAttachments && RAttachments)
            Result |= OutputWarning(0x861,T("The Attachments is not referenced in the main SeekHead"));
        if (!BSecondSeek && RSeekHead2)
            Result |= OutputWarning(0x861,T("The secondary SeekHead is not referenced in the main SeekHead"));
    }
	return Result;
}

static void LinkClusterBlocks()
{
	matroska_cluster **Cluster;
	for (Cluster=ARRAYBEGIN(RClusters,matroska_cluster*);Cluster!=ARRAYEND(RClusters,matroska_cluster*);++Cluster)
		MATROSKA_LinkClusterBlocks(*Cluster, RSegmentInfo, RTrackInfo, 1);
}

static bool_t TrackIsLaced(int16_t TrackNum)
{
    ebml_element *TrackData, *Track = EBML_MasterFindFirstElt(RTrackInfo, &MATROSKA_ContextTrackEntry, 0, 0);
    while (Track)
    {
        TrackData = EBML_MasterFindFirstElt(Track, &MATROSKA_ContextTrackNumber, 1, 1);
        if (EBML_IntegerValue(TrackData) == TrackNum)
        {
            TrackData = EBML_MasterFindFirstElt(Track, &MATROSKA_ContextTrackLacing, 1, 1);
            return EBML_IntegerValue(TrackData) != 0;
        }
        Track = EBML_MasterFindNextElt(RTrackInfo, Track, 0, 0);
    }
    return 1;
}

static bool_t TrackIsVideo(int16_t TrackNum)
{
    ebml_element *TrackData, *Track = EBML_MasterFindFirstElt(RTrackInfo, &MATROSKA_ContextTrackEntry, 0, 0);
    while (Track)
    {
        TrackData = EBML_MasterFindFirstElt(Track, &MATROSKA_ContextTrackNumber, 1, 1);
        if (EBML_IntegerValue(TrackData) == TrackNum)
        {
            TrackData = EBML_MasterFindFirstElt(Track, &MATROSKA_ContextTrackType, 1, 1);
            return EBML_IntegerValue(TrackData) == TRACK_TYPE_VIDEO;
        }
        Track = EBML_MasterFindNextElt(RTrackInfo, Track, 0, 0);
    }
    return 0;
}

static int CheckVideoStart()
{
	int Result = 0;
	ebml_element **Cluster;
    ebml_element *Block, *GBlock;
    int16_t BlockNum;
    timecode_t ClusterTimecode;
    array TrackKeyframe;

	for (Cluster=ARRAYBEGIN(RClusters,ebml_element*);Cluster!=ARRAYEND(RClusters,ebml_element*);++Cluster)
    {
        ArrayInit(&TrackKeyframe);
        ArrayResize(&TrackKeyframe,sizeof(bool_t)*(TrackMax+1),256);
        ArrayZero(&TrackKeyframe);

        ClusterTimecode = MATROSKA_ClusterTimecode((matroska_cluster*)*Cluster);
        if (ClusterTimecode==INVALID_TIMECODE_T)
            Result |= OutputError(0xC1,T("The Cluster at %") TPRId64 T(" has no timecode"),(*Cluster)->ElementPosition);
        else if (ClusterTime!=INVALID_TIMECODE_T && ClusterTime >= ClusterTimecode)
            Result |= OutputError(0xC2,T("The timecode of the Cluster at %") TPRId64 T(" is not incrementing"),(*Cluster)->ElementPosition);
        ClusterTime = ClusterTimecode;

	    for (Block = EBML_MasterChildren(*Cluster);Block;Block=EBML_MasterNext(Block))
	    {
		    if (Block->Context->Id == MATROSKA_ContextClusterBlockGroup.Id)
		    {
			    for (GBlock = EBML_MasterChildren(Block);GBlock;GBlock=EBML_MasterNext(GBlock))
			    {
				    if (GBlock->Context->Id == MATROSKA_ContextClusterBlock.Id)
				    {
                        BlockNum = MATROSKA_BlockTrackNum((matroska_block*)GBlock);
                        if (MATROSKA_BlockKeyframe((matroska_block*)GBlock))
                            ARRAYBEGIN(TrackKeyframe,bool_t)[BlockNum] = 1;
                        else if (!ARRAYBEGIN(TrackKeyframe,bool_t)[BlockNum] && TrackIsVideo(BlockNum))
                        {
                            OutputWarning(0xC0,T("First Block for video track #%d in Cluster at %") TPRId64 T(" is not a keyframe"),(int)BlockNum,(*Cluster)->ElementPosition);
                            ARRAYBEGIN(TrackKeyframe,bool_t)[BlockNum] = 1;
                        }
					    break;
				    }
			    }
		    }
		    else if (Block->Context->Id == MATROSKA_ContextClusterSimpleBlock.Id)
		    {
                BlockNum = MATROSKA_BlockTrackNum((matroska_block*)Block);
                if (MATROSKA_BlockKeyframe((matroska_block*)Block))
                    ARRAYBEGIN(TrackKeyframe,bool_t)[BlockNum] = 1;
                else if (!ARRAYBEGIN(TrackKeyframe,bool_t)[BlockNum] && TrackIsVideo(BlockNum))
                {
                    OutputWarning(0xC0,T("First Block for video track #%d in Cluster at %") TPRId64 T(" is not a keyframe"),(int)BlockNum,(*Cluster)->ElementPosition);
                    ARRAYBEGIN(TrackKeyframe,bool_t)[BlockNum] = 1;
                }
		    }
	    }
        ArrayClear(&TrackKeyframe);
    }
	return Result;
}

static int CheckPosSize(const ebml_element *RSegment)
{
	int Result = 0;
	ebml_element **Cluster,*PrevCluster=NULL;
    ebml_element *Elt;

	for (Cluster=ARRAYBEGIN(RClusters,ebml_element*);Cluster!=ARRAYEND(RClusters,ebml_element*);++Cluster)
    {
        Elt = EBML_MasterFindFirstElt(*Cluster,&MATROSKA_ContextClusterPrevSize,0,0);
        if (Elt)
        {
            if (PrevCluster==NULL)
                Result |= OutputError(0xA0,T("The PrevSize %") TPRId64 T(" was set on the first Cluster at %") TPRId64 T(""),EBML_IntegerValue(Elt),Elt->ElementPosition);
            else if (EBML_IntegerValue(Elt) != (*Cluster)->ElementPosition - PrevCluster->ElementPosition)
                Result |= OutputError(0xA1,T("The Cluster PrevSize %") TPRId64 T(" at %") TPRId64 T(" should be %") TPRId64 T(""),EBML_IntegerValue(Elt),Elt->ElementPosition,((*Cluster)->ElementPosition - PrevCluster->ElementPosition));
        }
        Elt = EBML_MasterFindFirstElt(*Cluster,&MATROSKA_ContextClusterPosition,0,0);
        if (Elt)
        {
            if (EBML_IntegerValue(Elt) != (*Cluster)->ElementPosition - EBML_ElementPositionData(RSegment))
                Result |= OutputError(0xA2,T("The Cluster position %") TPRId64 T(" at %") TPRId64 T(" should be %") TPRId64 T(""),EBML_IntegerValue(Elt),Elt->ElementPosition,((*Cluster)->ElementPosition - EBML_ElementPositionData(RSegment)));
        }
        PrevCluster = *Cluster;
    }
	return Result;
}

static int CheckLacingKeyframe()
{
	int Result = 0;
	matroska_cluster **Cluster;
    ebml_element *Block, *GBlock;
    int16_t BlockNum;
    timecode_t BlockTime;
    size_t Frame,TrackIdx;

	for (Cluster=ARRAYBEGIN(RClusters,matroska_cluster*);Cluster!=ARRAYEND(RClusters,matroska_cluster*);++Cluster)
    {
	    for (Block = EBML_MasterChildren(*Cluster);Block;Block=EBML_MasterNext(Block))
	    {
		    if (Block->Context->Id == MATROSKA_ContextClusterBlockGroup.Id)
		    {
			    for (GBlock = EBML_MasterChildren(Block);GBlock;GBlock=EBML_MasterNext(GBlock))
			    {
				    if (GBlock->Context->Id == MATROSKA_ContextClusterBlock.Id)
				    {
                        //MATROSKA_ContextTrackLacing
                        BlockNum = MATROSKA_BlockTrackNum((matroska_block*)GBlock);
                        for (TrackIdx=0; TrackIdx<ARRAYCOUNT(Tracks,track_info); ++TrackIdx)
                            if (ARRAYBEGIN(Tracks,track_info)[TrackIdx].Num == BlockNum)
                                break;
                        
                        if (TrackIdx==ARRAYCOUNT(Tracks,track_info))
                            Result |= OutputError(0xB2,T("Block at %") TPRId64 T(" is using an unknown track #%d"),GBlock->ElementPosition,(int)BlockNum);
                        else
                        {
                            if (MATROSKA_BlockLaced((matroska_block*)GBlock) && !TrackIsLaced(BlockNum))
                                Result |= OutputError(0xB0,T("Block at %") TPRId64 T(" track #%d is laced but the track is not"),GBlock->ElementPosition,(int)BlockNum);
                            if (!MATROSKA_BlockKeyframe((matroska_block*)GBlock) && !TrackIsVideo(BlockNum))
                                Result |= OutputError(0xB1,T("Block at %") TPRId64 T(" track #%d is not a keyframe"),GBlock->ElementPosition,(int)BlockNum);

                            for (Frame=0; Frame<MATROSKA_BlockGetFrameCount((matroska_block*)GBlock); ++Frame)
                                ARRAYBEGIN(Tracks,track_info)[TrackIdx].DataLength += MATROSKA_BlockGetLength((matroska_block*)GBlock,Frame);
                            if (Details)
                            {
                                BlockTime = MATROSKA_BlockTimecode((matroska_block*)GBlock);
                                if (MinTime==INVALID_TIMECODE_T || MinTime>BlockTime)
                                    MinTime = BlockTime;
                                if (MaxTime==INVALID_TIMECODE_T || MaxTime<BlockTime)
                                    MaxTime = BlockTime;
                            }
                        }
					    break;
				    }
			    }
		    }
		    else if (Block->Context->Id == MATROSKA_ContextClusterSimpleBlock.Id)
		    {
                BlockNum = MATROSKA_BlockTrackNum((matroska_block*)Block);
                for (TrackIdx=0; TrackIdx<ARRAYCOUNT(Tracks,track_info); ++TrackIdx)
                    if (ARRAYBEGIN(Tracks,track_info)[TrackIdx].Num == BlockNum)
                        break;
                
                if (TrackIdx==ARRAYCOUNT(Tracks,track_info))
                    Result |= OutputError(0xB2,T("Block at %") TPRId64 T(" is using an unknown track #%d"),Block->ElementPosition,(int)BlockNum);
                else
                {
                    if (MATROSKA_BlockLaced((matroska_block*)Block) && !TrackIsLaced(BlockNum))
                        Result |= OutputError(0xB0,T("SimpleBlock at %") TPRId64 T(" track #%d is laced but the track is not"),Block->ElementPosition,(int)BlockNum);
                    if (!MATROSKA_BlockKeyframe((matroska_block*)Block) && !TrackIsVideo(BlockNum))
                        Result |= OutputError(0xB1,T("SimpleBlock at %") TPRId64 T(" track #%d is not a keyframe"),Block->ElementPosition,(int)BlockNum);
                    for (Frame=0; Frame<MATROSKA_BlockGetFrameCount((matroska_block*)Block); ++Frame)
                        ARRAYBEGIN(Tracks,track_info)[TrackIdx].DataLength += MATROSKA_BlockGetLength((matroska_block*)Block,Frame);
                    if (Details)
                    {
                        BlockTime = MATROSKA_BlockTimecode((matroska_block*)Block);
                        if (MinTime==INVALID_TIMECODE_T || MinTime>BlockTime)
                            MinTime = BlockTime;
                        if (MaxTime==INVALID_TIMECODE_T || MaxTime<BlockTime)
                            MaxTime = BlockTime;
                    }
                }
		    }
	    }
    }
	return Result;
}

static int CheckCueEntries(ebml_element *Cues)
{
	int Result = 0;
	timecode_t TimecodeEntry, PrevTimecode = INVALID_TIMECODE_T;
	int16_t TrackNumEntry;
	matroska_cluster **Cluster;
	matroska_block *Block;
    int ClustNum = 0;

	if (!RSegmentInfo)
		Result |= OutputError(0x310,T("A Cues (index) is defined but no SegmentInfo was found"));
	else if (ARRAYCOUNT(RClusters,matroska_cluster*))
	{
		matroska_cuepoint *CuePoint = (matroska_cuepoint*)EBML_MasterFindFirstElt(Cues, &MATROSKA_ContextCuePoint, 0, 0);
		while (CuePoint)
		{
            if (ClustNum++ % 24 == 0)
                TextWrite(StdErr,T("."));
			MATROSKA_LinkCueSegmentInfo(CuePoint,RSegmentInfo);
			TimecodeEntry = MATROSKA_CueTimecode(CuePoint);
			TrackNumEntry = MATROSKA_CueTrackNum(CuePoint);

			if (TimecodeEntry < PrevTimecode && PrevTimecode != INVALID_TIMECODE_T)
				Result |= OutputError(0x311,T("The Cues entry for timecode %") TPRId64 T(" ms is listed after entry %") TPRId64 T(" ms"),Scale64(TimecodeEntry,1,1000000),Scale64(PrevTimecode,1,1000000));

			// find a matching Block
			for (Cluster = ARRAYBEGIN(RClusters,matroska_cluster*);Cluster != ARRAYEND(RClusters,matroska_cluster*); ++Cluster)
			{
				Block = MATROSKA_GetBlockForTimecode(*Cluster, TimecodeEntry, TrackNumEntry);
				if (Block)
					break;
			}
			if (Cluster == ARRAYEND(RClusters,matroska_cluster*))
				Result |= OutputError(0x312,T("CueEntry Track #%d and timecode %") TPRId64 T(" ms not found"),(int)TrackNumEntry,Scale64(TimecodeEntry,1,1000000));
			PrevTimecode = TimecodeEntry;
			CuePoint = (matroska_cuepoint*)EBML_MasterFindNextElt(Cues, (ebml_element*)CuePoint, 0, 0);
		}
	}
	return Result;
}

int main(int argc, const char *argv[])
{
    int Result = 0;
    int ShowUsage = 0;
    int ShowVersion = 0;
    parsercontext p;
    textwriter _StdErr;
    stream *Input = NULL;
    tchar_t Path[MAXPATHFULL];
    tchar_t String[MAXLINE];
    ebml_element *EbmlHead = NULL, *RSegment = NULL, *RLevel1 = NULL, *Prev, **Cluster;
	ebml_element *EbmlDocVer, *EbmlReadDocVer;
    ebml_string *LibName, *AppName;
    ebml_parser_context RContext;
    ebml_parser_context RSegmentContext;
    int i,UpperElement;
	int MatroskaProfile = 0;
    bool_t HasVideo = 0;
	int DotCount;
    track_info *TI;
	filepos_t VoidAmount = 0;

    // Core-C init phase
    ParserContext_Init(&p,NULL,NULL,NULL);
	StdAfx_Init((nodemodule*)&p);
    ProjectSettings((nodecontext*)&p);

    // EBML & Matroska Init
    MATROSKA_Init((nodecontext*)&p);

    ArrayInit(&RClusters);
    ArrayInit(&Tracks);

    StdErr = &_StdErr;
    memset(StdErr,0,sizeof(_StdErr));
    StdErr->Stream = (stream*)NodeSingleton(&p,STDERR_ID);
    assert(StdErr->Stream!=NULL);

	for (i=1;i<argc;++i)
	{
	    Node_FromStr(&p,Path,TSIZEOF(Path),argv[i]);
		if (tcsisame_ascii(Path,T("--no-warn"))) Warnings = 0;
		else if (tcsisame_ascii(Path,T("--live"))) Live = 1;
		else if (tcsisame_ascii(Path,T("--details"))) Details = 1;
		else if (tcsisame_ascii(Path,T("--divx"))) DivX = 1;
		else if (tcsisame_ascii(Path,T("--version"))) ShowVersion = 1;
        else if (tcsisame_ascii(Path,T("--help"))) {ShowVersion = 1; ShowUsage = 1;}
		else if (i<argc-1) TextPrintf(StdErr,T("Unknown parameter '%s'\r\n"),Path);
	}

    if (argc < 2 || ShowVersion)
    {
        TextWrite(StdErr,T("mkvalidator v") PROJECT_VERSION T(", Copyright (c) 2010 Matroska Foundation\r\n"));
        if (argc < 2 || ShowUsage)
        {
            Result = OutputError(1,T("Usage: mkvalidator [options] <matroska_src>"));
		    TextWrite(StdErr,T("Options:\r\n"));
		    TextWrite(StdErr,T("  --no-warn   only output errors, no warnings\r\n"));
            TextWrite(StdErr,T("  --live      only output errors/warnings relevant to live streams\r\n"));
            TextWrite(StdErr,T("  --details   show details for valid files\r\n"));
            TextWrite(StdErr,T("  --divx      assume the file is using DivX specific extensions\r\n"));
            TextWrite(StdErr,T("  --version   show the version of mkvalidator\r\n"));
            TextWrite(StdErr,T("  --help      show this screen\r\n"));
        }
        goto exit;
    }

    Node_FromStr(&p,Path,TSIZEOF(Path),argv[argc-1]);
    Input = StreamOpen(&p,Path,SFLAG_RDONLY/*|SFLAG_BUFFERED*/);
    if (!Input)
    {
        TextPrintf(StdErr,T("Could not open file \"%s\" for reading\r\n"),Path);
        Result = -2;
        goto exit;
    }

    // parse the source file to determine if it's a Matroska file and determine the location of the key parts
    RContext.Context = &MATROSKA_ContextStream;
    RContext.EndPosition = INVALID_FILEPOS_T;
    RContext.UpContext = NULL;
    EbmlHead = EBML_FindNextElement(Input, &RContext, &UpperElement, 0);
	if (!EbmlHead || EbmlHead->Context->Id != EBML_ContextHead.Id)
    {
        Result = OutputError(3,T("EBML head not found! Are you sure it's a matroska/webm file?"));
        goto exit;
    }

    TextWrite(StdErr,T("."));

	if (EBML_ElementReadData(EbmlHead,Input,&RContext,0,SCOPE_ALL_DATA)!=ERR_NONE)
    {
        Result = OutputError(4,T("Could not read the EBML head"));
        goto exit;
    }

	VoidAmount += CheckUnknownElements(EbmlHead);

	RLevel1 = EBML_MasterFindFirstElt(EbmlHead,&EBML_ContextReadVersion,1,1);
	if (EBML_IntegerValue(RLevel1) > EBML_MAX_VERSION)
		OutputError(5,T("The EBML read version is not supported: %d"),(int)EBML_IntegerValue(RLevel1));

	RLevel1 = EBML_MasterFindFirstElt(EbmlHead,&EBML_ContextMaxIdLength,1,1);
	if (EBML_IntegerValue(RLevel1) > EBML_MAX_ID)
		OutputError(6,T("The EBML max ID length is not supported: %d"),(int)EBML_IntegerValue(RLevel1));

	RLevel1 = EBML_MasterFindFirstElt(EbmlHead,&EBML_ContextMaxSizeLength,1,1);
	if (EBML_IntegerValue(RLevel1) > EBML_MAX_SIZE)
		OutputError(7,T("The EBML max size length is not supported: %d"),(int)EBML_IntegerValue(RLevel1));

	RLevel1 = EBML_MasterFindFirstElt(EbmlHead,&EBML_ContextDocType,1,1);
    EBML_StringGet((ebml_string*)RLevel1,String,TSIZEOF(String));
    if (tcscmp(String,T("matroska"))!=0 && tcscmp(String,T("webm"))!=0)
	{
		Result = OutputError(8,T("The EBML doctype is not supported: %s"),String);
		goto exit;
	}

	EbmlDocVer = EBML_MasterFindFirstElt(EbmlHead,&EBML_ContextDocTypeVersion,1,1);
	EbmlReadDocVer = EBML_MasterFindFirstElt(EbmlHead,&EBML_ContextDocTypeReadVersion,1,1);

	if (EBML_IntegerValue(EbmlDocVer) > EBML_IntegerValue(EbmlReadDocVer))
		OutputError(9,T("The read DocType version %d is higher than the Doctype version %d"),(int)EBML_IntegerValue(EbmlReadDocVer),(int)EBML_IntegerValue(EbmlDocVer));

	if (tcscmp(String,T("matroska"))==0)
	{
		if (EBML_IntegerValue(EbmlReadDocVer)==2)
        {
            if (DivX)
    			MatroskaProfile = PROFILE_DIVX_V2;
            else
			    MatroskaProfile = PROFILE_MATROSKA_V2;
        }
		else if (EBML_IntegerValue(EbmlReadDocVer)==1)
        {
            if (DivX)
    			MatroskaProfile = PROFILE_DIVX_V1;
            else
		    	MatroskaProfile = PROFILE_MATROSKA_V1;
        }
		else
			Result |= OutputError(10,T("Unknown Matroska profile %d/%d"),(int)EBML_IntegerValue(EbmlDocVer),(int)EBML_IntegerValue(EbmlReadDocVer));
	}
	else if (EBML_IntegerValue(EbmlReadDocVer)==1)
		MatroskaProfile = PROFILE_WEBM_V1;
	else if (EBML_IntegerValue(EbmlReadDocVer)==2)
		MatroskaProfile = PROFILE_WEBM_V2;

	if (MatroskaProfile==0)
		Result |= OutputError(11,T("Matroska profile not supported"));

    TextWrite(StdErr,T("."));

	// find the segment
	RSegment = EBML_FindNextElement(Input, &RContext, &UpperElement, 1);
    RSegmentContext.Context = &MATROSKA_ContextSegment;
    RSegmentContext.EndPosition = EBML_ElementPositionEnd(RSegment);
    RSegmentContext.UpContext = &RContext;
	UpperElement = 0;
	DotCount = 0;
//TextPrintf(StdErr,T("Loading the level1 elements in memory\r\n"));
	Prev = NULL;
    RLevel1 = EBML_FindNextElement(Input, &RSegmentContext, &UpperElement, 1);
    while (RLevel1)
	{
        if (RLevel1->Context->Id == MATROSKA_ContextCluster.Id)
        {
            if (EBML_ElementReadData(RLevel1,Input,&RSegmentContext,0,SCOPE_PARTIAL_DATA)==ERR_NONE)
			{
                ArrayAppend(&RClusters,&RLevel1,sizeof(RLevel1),256);
				NodeTree_SetParent(RLevel1, RSegment, NULL);
				VoidAmount += CheckUnknownElements(RLevel1);
				Result |= CheckProfileViolation(RLevel1, MatroskaProfile);
				Result |= CheckMandatory(RLevel1, MatroskaProfile);
			}
			else
			{
				Result = OutputError(0x180,T("Failed to read the Cluster at %") TPRId64 T(" size %") TPRId64 T(""),RLevel1->ElementPosition,RLevel1->DataSize);
				goto exit;
			}
        }
        else if (RLevel1->Context->Id == MATROSKA_ContextSeekHead.Id)
        {
            if (Live)
            {
                Result |= OutputError(0x170,T("The live stream has a SeekHead at %") TPRId64 T(""),RLevel1->ElementPosition);
			    EBML_ElementSkipData(RLevel1, Input, &RSegmentContext, NULL, 1);
                NodeDelete((node*)RLevel1);
            }
            else if (EBML_ElementReadData(RLevel1,Input,&RSegmentContext,1,SCOPE_ALL_DATA)==ERR_NONE)
			{
				if (!RSeekHead)
					RSeekHead = RLevel1;
				else if (!RSeekHead2)
                {
					OutputWarning(0x103,T("Unnecessary secondary SeekHead was found at %") TPRId64 T(""),RLevel1->ElementPosition);
					RSeekHead2 = RLevel1;
                }
				else
					Result |= OutputError(0x101,T("Extra SeekHead found at %") TPRId64 T(" (size %") TPRId64 T(")"),RLevel1->ElementPosition,RLevel1->DataSize);
				NodeTree_SetParent(RLevel1, RSegment, NULL);
				VoidAmount += CheckUnknownElements(RLevel1);
				Result |= CheckProfileViolation(RLevel1, MatroskaProfile);
				Result |= CheckMandatory(RLevel1, MatroskaProfile);
			}
			else
			{
				Result = OutputError(0x100,T("Failed to read the SeekHead at %") TPRId64 T(" size %") TPRId64 T(""),RLevel1->ElementPosition,RLevel1->DataSize);
				goto exit;
			}
		}
        else if (RLevel1->Context->Id == MATROSKA_ContextSegmentInfo.Id)
        {
            if (EBML_ElementReadData(RLevel1,Input,&RSegmentContext,1,SCOPE_ALL_DATA)==ERR_NONE)
			{
				if (RSegmentInfo != NULL)
					Result |= OutputError(0x110,T("Extra SegmentInfo found at %") TPRId64 T(" (size %") TPRId64 T(")"),RLevel1->ElementPosition,RLevel1->DataSize);
				else
				{
					RSegmentInfo = RLevel1;
					NodeTree_SetParent(RLevel1, RSegment, NULL);
					VoidAmount += CheckUnknownElements(RLevel1);
					Result |= CheckProfileViolation(RLevel1, MatroskaProfile);
					Result |= CheckMandatory(RLevel1, MatroskaProfile);

                    if (Live)
                    {
                        EbmlHead = EBML_MasterFindFirstElt(RLevel1,&MATROSKA_ContextDuration,0,0);
                        if (EbmlHead)
                            Result |= OutputError(0x112,T("The live Segment has a duration set at %") TPRId64 T(""),EbmlHead->ElementPosition);
                    }
                }
			}
			else
			{
				Result = OutputError(0x111,T("Failed to read the SegmentInfo at %") TPRId64 T(" size %") TPRId64 T(""),RLevel1->ElementPosition,RLevel1->DataSize);
				goto exit;
			}
		}
        else if (RLevel1->Context->Id == MATROSKA_ContextTracks.Id)
        {
            if (EBML_ElementReadData(RLevel1,Input,&RSegmentContext,1,SCOPE_ALL_DATA)==ERR_NONE)
			{
				if (RTrackInfo != NULL)
					Result |= OutputError(0x120,T("Extra TrackInfo found at %") TPRId64 T(" (size %") TPRId64 T(")"),RLevel1->ElementPosition,RLevel1->DataSize);
				else
				{
                    size_t TrackCount;

                    RTrackInfo = RLevel1;
					NodeTree_SetParent(RLevel1, RSegment, NULL);
					VoidAmount += CheckUnknownElements(RLevel1);
					Result |= CheckProfileViolation(RLevel1, MatroskaProfile);
					Result |= CheckMandatory(RLevel1, MatroskaProfile);

                    EbmlHead = EBML_MasterFindFirstElt(RTrackInfo,&MATROSKA_ContextTrackEntry,0,0);
                    TrackCount = 0;
                    while (EbmlHead)
                    {
                        EbmlHead = EBML_MasterFindNextElt(RTrackInfo,EbmlHead,0,0);
                        ++TrackCount;
                    }

                    ArrayResize(&Tracks,TrackCount*sizeof(track_info),256);
                    ArrayZero(&Tracks);

                    EbmlHead = EBML_MasterFindFirstElt(RTrackInfo,&MATROSKA_ContextTrackEntry,0,0);
                    TrackCount = 0;
                    while (EbmlHead)
                    {
                        EbmlDocVer = EBML_MasterFindFirstElt(EbmlHead,&MATROSKA_ContextTrackNumber,0,0);
                        assert(EbmlDocVer!=NULL);
                        if (EbmlDocVer)
                        {
                            TrackMax = max(TrackMax,(size_t)EBML_IntegerValue(EbmlDocVer));
                            ARRAYBEGIN(Tracks,track_info)[TrackCount].Num = (int)EBML_IntegerValue(EbmlDocVer);
                        }
                        EbmlDocVer = EBML_MasterFindFirstElt(EbmlHead,&MATROSKA_ContextTrackType,0,0);
                        assert(EbmlDocVer!=NULL);
                        if (EbmlDocVer)
                        {
                            if (EBML_IntegerValue(EbmlDocVer)==TRACK_TYPE_VIDEO)
							{
								Result |= CheckVideoTrack(EbmlHead, ARRAYBEGIN(Tracks,track_info)[TrackCount].Num, MatroskaProfile);
                                HasVideo = 1;
							}
                            ARRAYBEGIN(Tracks,track_info)[TrackCount].Kind = (int)EBML_IntegerValue(EbmlDocVer);
                        }
                        ARRAYBEGIN(Tracks,track_info)[TrackCount].CodecID = (ebml_string*)EBML_MasterFindFirstElt(EbmlHead,&MATROSKA_ContextTrackCodecID,0,0);
                        EbmlHead = EBML_MasterFindNextElt(RTrackInfo,EbmlHead,0,0);
                        ++TrackCount;
                    }
                    EbmlDocVer = NULL;
                    EbmlHead = NULL;
                }
			}
			else
			{
				Result = OutputError(0x121,T("Failed to read the TrackInfo at %") TPRId64 T(" size %") TPRId64 T(""),RLevel1->ElementPosition,RLevel1->DataSize);
				goto exit;
			}
		}
        else if (RLevel1->Context->Id == MATROSKA_ContextCues.Id)
        {
            if (Live)
            {
                Result |= OutputError(0x171,T("The live stream has Cues at %") TPRId64 T(""),RLevel1->ElementPosition);
			    EBML_ElementSkipData(RLevel1, Input, &RSegmentContext, NULL, 1);
                NodeDelete((node*)RLevel1);
            }
            else if (EBML_ElementReadData(RLevel1,Input,&RSegmentContext,1,SCOPE_ALL_DATA)==ERR_NONE)
			{
				if (RCues != NULL)
					Result |= OutputError(0x130,T("Extra Cues found at %") TPRId64 T(" (size %") TPRId64 T(")"),RLevel1->ElementPosition,RLevel1->DataSize);
				else
				{
					RCues = RLevel1;
					NodeTree_SetParent(RLevel1, RSegment, NULL);
					VoidAmount += CheckUnknownElements(RLevel1);
					Result |= CheckProfileViolation(RLevel1, MatroskaProfile);
					Result |= CheckMandatory(RLevel1, MatroskaProfile);
				}
			}
			else
			{
				Result = OutputError(0x131,T("Failed to read the Cues at %") TPRId64 T(" size %") TPRId64 T(""),RLevel1->ElementPosition,RLevel1->DataSize);
				goto exit;
			}
		}
        else if (RLevel1->Context->Id == MATROSKA_ContextChapters.Id)
        {
            if (Live)
            {
                Result |= OutputError(0x172,T("The live stream has Chapters at %") TPRId64 T(""),RLevel1->ElementPosition);
			    EBML_ElementSkipData(RLevel1, Input, &RSegmentContext, NULL, 1);
                NodeDelete((node*)RLevel1);
            }
            else if (EBML_ElementReadData(RLevel1,Input,&RSegmentContext,1,SCOPE_ALL_DATA)==ERR_NONE)
			{
				if (RChapters != NULL)
					Result |= OutputError(0x140,T("Extra Chapters found at %") TPRId64 T(" (size %") TPRId64 T(")"),RLevel1->ElementPosition,RLevel1->DataSize);
				else
				{
					RChapters = RLevel1;
					NodeTree_SetParent(RLevel1, RSegment, NULL);
					VoidAmount += CheckUnknownElements(RLevel1);
					Result |= CheckProfileViolation(RLevel1, MatroskaProfile);
					Result |= CheckMandatory(RLevel1, MatroskaProfile);
				}
			}
			else
			{
				Result = OutputError(0x141,T("Failed to read the Chapters at %") TPRId64 T(" size %") TPRId64 T(""),RLevel1->ElementPosition,RLevel1->DataSize);
				goto exit;
			}
		}
        else if (RLevel1->Context->Id == MATROSKA_ContextTags.Id)
        {
            if (EBML_ElementReadData(RLevel1,Input,&RSegmentContext,1,SCOPE_ALL_DATA)==ERR_NONE)
			{
				if (RTags != NULL)
					Result |= OutputError(0x150,T("Extra Tags found at %") TPRId64 T(" (size %") TPRId64 T(")"),RLevel1->ElementPosition,RLevel1->DataSize);
				else
				{
					RTags = RLevel1;
					NodeTree_SetParent(RLevel1, RSegment, NULL);
					VoidAmount += CheckUnknownElements(RLevel1);
					Result |= CheckProfileViolation(RLevel1, MatroskaProfile);
					Result |= CheckMandatory(RLevel1, MatroskaProfile);
				}
			}
			else
			{
				Result = OutputError(0x151,T("Failed to read the Tags at %") TPRId64 T(" size %") TPRId64 T(""),RLevel1->ElementPosition,RLevel1->DataSize);
				goto exit;
			}
		}
        else if (RLevel1->Context->Id == MATROSKA_ContextAttachments.Id)
        {
            if (Live)
            {
                Result |= OutputError(0x173,T("The live stream has a Attachments at %") TPRId64 T(""),RLevel1->ElementPosition);
			    EBML_ElementSkipData(RLevel1, Input, &RSegmentContext, NULL, 1);
                NodeDelete((node*)RLevel1);
            }
            else if (EBML_ElementReadData(RLevel1,Input,&RSegmentContext,1,SCOPE_ALL_DATA)==ERR_NONE)
			{
				if (RAttachments != NULL)
					Result |= OutputError(0x160,T("Extra Attachments found at %") TPRId64 T(" (size %") TPRId64 T(")"),RLevel1->ElementPosition,RLevel1->DataSize);
				else
				{
					RAttachments = RLevel1;
					NodeTree_SetParent(RLevel1, RSegment, NULL);
					VoidAmount += CheckUnknownElements(RLevel1);
					Result |= CheckProfileViolation(RLevel1, MatroskaProfile);
					Result |= CheckMandatory(RLevel1, MatroskaProfile);
				}
			}
			else
			{
				Result = OutputError(0x161,T("Failed to read the Attachments at %") TPRId64 T(" size %") TPRId64 T(""),RLevel1->ElementPosition,RLevel1->DataSize);
				goto exit;
			}
		}
		else
		{
			if (Node_IsPartOf(RLevel1,EBML_DUMMY_ID))
			{
				tchar_t Id[32];
				EBML_IdToString(Id,TSIZEOF(Id),RLevel1->Context->Id);
				Result |= OutputError(0x80,T("Unknown element %s at %") TPRId64 T(" size %") TPRId64 T(""),Id,RLevel1->ElementPosition,RLevel1->DataSize);
			}
			if (Node_IsPartOf(RLevel1,EBML_VOID_CLASS))
			{
				VoidAmount += EBML_ElementFullSize(RLevel1,0);
			}
			EBML_ElementSkipData(RLevel1, Input, &RSegmentContext, NULL, 1);
            NodeDelete((node*)RLevel1);
		}
        TextWrite(StdErr,T(".")); ++DotCount;
		if (!(DotCount % 60))
			TextWrite(StdErr,T("\r                                                              \r"));

		Prev = RLevel1;
		RLevel1 = EBML_FindNextElement(Input, &RSegmentContext, &UpperElement, 1);
	}

	if (!RSegmentInfo)
	{
		Result = OutputError(0x40,T("The segment is missing a SegmentInfo"));
		goto exit;
	}

	if (Prev)
	{
		if (EBML_ElementPositionEnd(RSegment)!=INVALID_FILEPOS_T && EBML_ElementPositionEnd(RSegment)!=EBML_ElementPositionEnd(Prev))
			Result |= OutputError(0x42,T("The segment's size %") TPRId64 T(" doesn't match the position where it ends %") TPRId64,EBML_ElementPositionEnd(RSegment),EBML_ElementPositionEnd(Prev));
	}

	if (!RSeekHead)
    {
        if (!Live)
		    OutputWarning(0x801,T("The segment has no SeekHead section"));
    }
	else
		Result |= CheckSeekHead(RSeekHead);
	if (RSeekHead2)
		Result |= CheckSeekHead(RSeekHead2);

	if (ARRAYCOUNT(RClusters,ebml_element*))
	{
        TextWrite(StdErr,T("."));
		LinkClusterBlocks();

        if (HasVideo)
            Result |= CheckVideoStart();
        Result |= CheckLacingKeyframe();
        Result |= CheckPosSize(RSegment);
		if (!RCues)
        {
            if (!Live)
			    OutputWarning(0x800,T("The segment has Clusters but no Cues section (bad for seeking)"));
        }
		else
			CheckCueEntries(RCues);
		if (!RTrackInfo)
		{
			Result = OutputError(0x41,T("The segment has Clusters but no TrackInfo section"));
			goto exit;
		}
	}

    TextWrite(StdErr,T("."));
	if (RTrackInfo)
		CheckTracks(RTrackInfo, MatroskaProfile);

    for (TI=ARRAYBEGIN(Tracks,track_info); TI!=ARRAYEND(Tracks,track_info); ++TI)
    {
        if (TI->DataLength==0)
            OutputWarning(0xB8,T("Track #%d is defined but has no frame"),TI->Num);
    }

	if (VoidAmount > 4*1024)
		OutputWarning(0xD0,T("There are %") TPRId64 T(" bytes of void data\r\n"),VoidAmount);

	if (Result==0)
    {
        TextPrintf(StdErr,T("\r%s %s: the file appears to be valid\r\n"),PROJECT_NAME,PROJECT_VERSION);
        if (Details)
        {
            track_info *TI;
            for (TI=ARRAYBEGIN(Tracks,track_info); TI!=ARRAYEND(Tracks,track_info); ++TI)
            {
                EBML_StringGet(TI->CodecID,String,TSIZEOF(String));
                TextPrintf(StdErr,T("Track #%d %18s %") TPRId64 T(" bits/s\r\n"),TI->Num,String,Scale64(TI->DataLength,8000000000,MaxTime-MinTime));
            }
        }
    }

exit:
	if (RSegmentInfo)
	{
		tchar_t App[MAXPATH];
		App[0] = 0;
		LibName = (ebml_string*)EBML_MasterFindFirstElt(RSegmentInfo,&MATROSKA_ContextMuxingApp,0,0);
		AppName = (ebml_string*)EBML_MasterFindFirstElt(RSegmentInfo,&MATROSKA_ContextWritingApp,0,0);
		if (LibName)
		{
			EBML_StringGet(LibName,String,TSIZEOF(String));
			tcscat_s(App,TSIZEOF(App),String);
		}
		if (AppName)
		{
			EBML_StringGet(AppName,String,TSIZEOF(String));
			if (App[0])
				tcscat_s(App,TSIZEOF(App),T(" / "));
			tcscat_s(App,TSIZEOF(App),String);
		}
		if (App[0]==0)
			tcscat_s(App,TSIZEOF(App),T("<unknown>"));
		TextPrintf(StdErr,T("\r\tfile created with %s\r\n"),App);
	}

    for (Cluster = ARRAYBEGIN(RClusters,ebml_element*);Cluster != ARRAYEND(RClusters,ebml_element*); ++Cluster)
        NodeDelete((node*)*Cluster);
    ArrayClear(&RClusters);
    if (RAttachments)
        NodeDelete((node*)RAttachments);
    if (RTags)
        NodeDelete((node*)RTags);
    if (RCues)
        NodeDelete((node*)RCues);
    if (RChapters)
        NodeDelete((node*)RChapters);
    if (RTrackInfo)
        NodeDelete((node*)RTrackInfo);
    if (RSegmentInfo)
        NodeDelete((node*)RSegmentInfo);
    if (RLevel1)
        NodeDelete((node*)RLevel1);
    if (RSegment)
        NodeDelete((node*)RSegment);
    if (EbmlHead)
        NodeDelete((node*)EbmlHead);
    ArrayClear(&Tracks);
    if (Input)
        StreamClose(Input);

    // EBML & Matroska ending
    MATROSKA_Done((nodecontext*)&p);

    // Core-C ending
	StdAfx_Done((nodemodule*)&p);
    ParserContext_Done(&p);

    return Result;
}
