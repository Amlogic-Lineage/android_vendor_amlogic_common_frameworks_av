/*
 * Copyright (C) 2010 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

//#define LOG_NDEBUG 0
#define LOG_TAG "NU-AmNuPlayer"
#include <utils/Log.h>

#include <stdio.h>
#include <assert.h>
#include <limits.h>
#include <unistd.h>
#include <fcntl.h>
#include <sched.h>
#include <sys/types.h>
#include <sys/stat.h>

#include <libavcodec/avcodec.h>


#include "AmNuPlayer.h"

#include "AmHTTPLiveSource.h"
#include "AmNuPlayerCCDecoder.h"
#include "AmNuPlayerDecoder.h"
#include "AmNuPlayerDecoderBase.h"
#include "AmNuPlayerDecoderPassThrough.h"
#include "AmNuPlayerDriver.h"
#include "AmNuPlayerRenderer.h"
#include "AmNuPlayerSource.h"
#include "AmRTSPSource.h"
#include "AmStreamingSource.h"
#include "AmGenericSource.h"
#include "TextDescriptions.h"

#include "AmATSParser.h"

#include <cutils/properties.h>

#include <media/stagefright/foundation/hexdump.h>
#include <media/stagefright/foundation/ABuffer.h>
#include <media/stagefright/foundation/ADebug.h>
#include <media/stagefright/foundation/AMessage.h>
#include <media/stagefright/MediaBuffer.h>
#include <media/stagefright/MediaDefs.h>
#include <media/stagefright/MediaErrors.h>
#include <media/stagefright/MetaData.h>
#include <gui/IGraphicBufferProducer.h>


#include "avc_utils.h"

#include "ESDS.h"
#include <media/stagefright/Utils.h>
#include <Amavutils.h>

namespace android {

struct AmNuPlayer::Action : public RefBase {
    Action() {}

    virtual void execute(AmNuPlayer *player) = 0;

private:
    DISALLOW_EVIL_CONSTRUCTORS(Action);
};

struct AmNuPlayer::SeekAction : public Action {
    SeekAction(int64_t seekTimeUs, bool needNotify)
        : mSeekTimeUs(seekTimeUs),
          mNeedNotify(needNotify) {
    }

    virtual void execute(AmNuPlayer *player) {
        player->performSeek(mSeekTimeUs, mNeedNotify);
    }

private:
    int64_t mSeekTimeUs;
    bool mNeedNotify;

    DISALLOW_EVIL_CONSTRUCTORS(SeekAction);
};

struct AmNuPlayer::ResumeDecoderAction : public Action {
    ResumeDecoderAction(bool needNotify)
        : mNeedNotify(needNotify) {
    }

    virtual void execute(AmNuPlayer *player) {
        player->performResumeDecoders(mNeedNotify);
    }

private:
    bool mNeedNotify;

    DISALLOW_EVIL_CONSTRUCTORS(ResumeDecoderAction);
};

struct AmNuPlayer::SetSurfaceAction : public Action {
    SetSurfaceAction(const sp<NativeWindowWrapper> &wrapper)
        : mWrapper(wrapper) {
    }

    virtual void execute(AmNuPlayer *player) {
        player->performSetSurface(mWrapper);
    }

private:
    sp<NativeWindowWrapper> mWrapper;

    DISALLOW_EVIL_CONSTRUCTORS(SetSurfaceAction);
};

struct AmNuPlayer::FlushDecoderAction : public Action {
    FlushDecoderAction(FlushCommand audio, FlushCommand video)
        : mAudio(audio),
          mVideo(video) {
    }

    virtual void execute(AmNuPlayer *player) {
        player->performDecoderFlush(mAudio, mVideo);
    }

private:
    FlushCommand mAudio;
    FlushCommand mVideo;

    DISALLOW_EVIL_CONSTRUCTORS(FlushDecoderAction);
};

struct AmNuPlayer::PostMessageAction : public Action {
    PostMessageAction(const sp<AMessage> &msg)
        : mMessage(msg) {
    }

    virtual void execute(AmNuPlayer *) {
        mMessage->post();
    }

private:
    sp<AMessage> mMessage;

    DISALLOW_EVIL_CONSTRUCTORS(PostMessageAction);
};

// Use this if there's no state necessary to save in order to execute
// the action.
struct AmNuPlayer::SimpleAction : public Action {
    typedef void (AmNuPlayer::*ActionFunc)();

    SimpleAction(ActionFunc func)
        : mFunc(func) {
    }

    virtual void execute(AmNuPlayer *player) {
        (player->*mFunc)();
    }

private:
    ActionFunc mFunc;

    DISALLOW_EVIL_CONSTRUCTORS(SimpleAction);
};

////////////////////////////////////////////////////////////////////////////////

// static
Mutex AmNuPlayer::mThreadLock;
Vector<android_thread_id_t> AmNuPlayer::mThreadId;

AmNuPlayer::AmNuPlayer()
    : mUIDValid(false),
      mSourceFlags(0),
      mOffloadAudio(false),
      mAudioDecoderGeneration(0),
      mVideoDecoderGeneration(0),
      mRendererGeneration(0),
      mAudioEOS(false),
      mVideoEOS(false),
      mScanSourcesPending(false),
      mScanSourcesGeneration(0),
      mPollDurationGeneration(0),
      mTimedTextGeneration(0),
      mFlushingAudio(NONE),
      mFlushingVideo(NONE),
      mResumePending(false),
      mVideoScalingMode(NATIVE_WINDOW_SCALING_MODE_SCALE_TO_WINDOW),
      mNewSurface(NULL),
      mEnableFrameRate(false),
      mFrameRate(-1.0),
      mWaitSeconds(10),
      mStarted(false),
      mPaused(false),
      mPausedByClient(false),
      mAutoSwitch(-1) {
    clearFlushComplete();
    DtshdApreTotal=0;
    DtsHdStreamType=0;
    DtsHdMulAssetHint=0;
    DtsHdHpsHint=0;
    mStrCurrentAudioCodec = NULL;
    char value[PROPERTY_VALUE_MAX];
    if (property_get("media.hls.wait-seconds", value, NULL)) {
        mWaitSeconds = atoi(value);
    }
}

AmNuPlayer::~AmNuPlayer() {
    // restore the state of auto frame-rate if needed
    if (mEnableFrameRate && mAutoSwitch > 0) {
        amsysfs_set_sysfs_int("/sys/class/tv/policy_fr_auto_switch", mAutoSwitch);
    }
    if (mSource != NULL) {
        mSource->release();
    }
}

// static
void AmNuPlayer::thread_interrupt() {
    android_thread_id_t thread_id = androidGetThreadId();

    Mutex::Autolock autoLock(mThreadLock);

    if (mThreadId.isEmpty()) {
        mThreadId.push_back(thread_id);
    } else {
        size_t size = mThreadId.size();
        size_t i;
        for (i = 0; i < size; i++) {
            if (mThreadId.itemAt(i) == thread_id) {
                break;
            }
        }
        if (i == size) {
            mThreadId.push_back(thread_id);
        }
    }
}

// static
void AmNuPlayer::thread_uninterrupt() {
    android_thread_id_t thread_id = androidGetThreadId();

    Mutex::Autolock autoLock(mThreadLock);

    size_t size = mThreadId.size();
    for (size_t i = 0; i < size; i++) {
        if (mThreadId.itemAt(i) == thread_id) {
            mThreadId.removeAt(i);
            break;
        }
    }
}

// static
int32_t AmNuPlayer::interrupt_callback(android_thread_id_t thread_id) {
    Mutex::Autolock autoLock(mThreadLock);
    if (mThreadId.isEmpty()) {
        return 0;
    } else {
        size_t size = mThreadId.size();
        for (size_t i = 0; i < size; i++) {
            if (mThreadId.itemAt(i) == thread_id) {
                return 1;
            }
        }
    }
    return 0;
}
int AmNuPlayer::getintfromString8(String8 &s, const char*pre){
    int off;
    int val = 0;
    if ((off = s.find(pre, 0)) >= 0) {
        sscanf(s.string() + off + strlen(pre), "%d", &val);
    }
    return val;
}


#define DTSM6_EXCHANGE_INFO_NODE "/sys/class/amaudio/debug"
static void dtsm6_get_exchange_info(int *streamtype,int *APreCnt,int *APreSel,int *ApreAssetSel,int32_t *ApresAssetsArray,int *MulAssetHint,int *HPs_hint){
    int fd=open(DTSM6_EXCHANGE_INFO_NODE,  O_RDWR | O_TRUNC, 0644);
    int bytes=0,i=0;

    if (fd >= 0) {
        uint8_t ubuf8[256]={0};
        bytes=read(fd,ubuf8,256);

        if (streamtype != NULL) {
            uint8_t *pStreamType=(uint8_t *)strstr((const char*)ubuf8,"StreamType");
            if (pStreamType != NULL) {
               pStreamType+=10;
               *streamtype=atoi((const char*)pStreamType);
            }
        }

        if (APreCnt != NULL) {
            uint8_t *pApreCnt=(uint8_t *)strstr((const char*)ubuf8,"ApreCnt");
            if (pApreCnt != NULL) {
               pApreCnt+=7;
               *APreCnt=atoi((const char*)pApreCnt);
            }
        }

        if (APreSel != NULL) {
            uint8_t *pApreSel=(uint8_t *)strstr((const char*)ubuf8,"ApreSel");
            if (pApreSel != NULL) {
               pApreSel+=7;
               *APreSel=atoi((const char*)pApreSel);
            }
        }

        if (ApreAssetSel != NULL) {
            uint8_t *pApreAssetSel=(uint8_t *)strstr((const char*)ubuf8,"ApreAssetSel");
            if (pApreAssetSel != NULL) {
                pApreAssetSel+=12;
                *ApreAssetSel=atoi((const char*)pApreAssetSel);
            }
        }

        if (ApresAssetsArray != NULL && APreCnt != NULL) {
            uint8_t *pApresAssetsArray=(uint8_t *)strstr((const char*)ubuf8,"ApresAssetsArray");
            if (pApresAssetsArray != NULL) {
               pApresAssetsArray+=16;
               for (i=0;i<*APreCnt;i++) {
                 ApresAssetsArray[i]=pApresAssetsArray[i];
                 ALOGI("[%s %d]ApresAssetsArray[%d]/%d",__FUNCTION__,__LINE__,i,ApresAssetsArray[i]);
               }
            }
        }
        if (MulAssetHint != NULL) {
            uint8_t *pMulAssetHint=(uint8_t *)strstr((const char*)ubuf8,"MulAssetHint");
            if (pMulAssetHint != NULL) {
               pMulAssetHint+=12;
               *MulAssetHint=atoi((const char*)pMulAssetHint);
            }
        }
        if (HPs_hint != NULL) {
            uint8_t *phps_hint=(uint8_t *)strstr((const char*)ubuf8,"HPSHint");
            if (phps_hint != NULL) {
               phps_hint +=7;
               *HPs_hint=atoi((const char*)phps_hint);
            }
        }
        close(fd);
    }else{
        ALOGI("[%s %d]open %s failed!\n",__FUNCTION__,__LINE__,DTSM6_EXCHANGE_INFO_NODE);
       if (streamtype != NULL)  *streamtype=0;
       if (APreCnt != NULL)     *APreCnt=0;
       if (APreSel != NULL)     *APreSel=0;
       if (ApreAssetSel != NULL)*ApreAssetSel=0;
       if (HPs_hint != NULL)    *HPs_hint=0;
       if (ApresAssetsArray != NULL&& APreCnt != NULL) memset(ApresAssetsArray,0,*APreCnt);
    }
}

static void dtsm6_set_exchange_info(int *APreSel,int *ApreAssetSel)
{
    int fd=open(DTSM6_EXCHANGE_INFO_NODE,  O_RDWR | O_TRUNC, 0644);
    int bytes,pos=0;
    if (fd >= 0) {
       char ubuf8[128]={0};
       if (APreSel != NULL) {
           bytes=sprintf(ubuf8,"dtsm6_apre_sel_set%d",*APreSel);
           write(fd, ubuf8, bytes);
       }
       if (ApreAssetSel != NULL) {
           bytes=sprintf(ubuf8,"dtsm6_apre_assets_sel_set%d",*ApreAssetSel);
           write(fd, ubuf8, bytes);
       }
       close(fd);
    }else{
       ALOGI("[%s %d]open %s failed!\n",__FUNCTION__,__LINE__,DTSM6_EXCHANGE_INFO_NODE);
    }
}
static aformat_t audioTypeConvert(enum CodecID id)
{
    aformat_t format = (aformat_t)-1;
    switch (id) {
    case CODEC_ID_PCM_MULAW:
        //format = AFORMAT_MULAW;
        format = AFORMAT_ADPCM;
        break;

    case CODEC_ID_PCM_ALAW:
        //format = AFORMAT_ALAW;
        format = AFORMAT_ADPCM;
        break;


    case CODEC_ID_MP1:
    case CODEC_ID_MP2:
    case CODEC_ID_MP3:
        format = AFORMAT_MPEG;
        break;

    case CODEC_ID_AAC_LATM:
        format = AFORMAT_AAC_LATM;
        break;


    case CODEC_ID_AAC:
        format = AFORMAT_AAC;
        break;

    case CODEC_ID_AC3:
        format = AFORMAT_AC3;
        break;
    case CODEC_ID_EAC3:
        format = AFORMAT_EAC3;
        break;
    case CODEC_ID_DTS:
        format = AFORMAT_DTS;
        break;

    case CODEC_ID_PCM_S16BE:
        format = AFORMAT_PCM_S16BE;
        break;

    case CODEC_ID_PCM_S16LE:
        format = AFORMAT_PCM_S16LE;
        break;

    case CODEC_ID_PCM_U8:
        format = AFORMAT_PCM_U8;
        break;

    case CODEC_ID_COOK:
        format = AFORMAT_COOK;
        break;

    case CODEC_ID_ADPCM_IMA_WAV:
    case CODEC_ID_ADPCM_MS:
        format = AFORMAT_ADPCM;
        break;
    case CODEC_ID_AMR_NB:
    case CODEC_ID_AMR_WB:
        format =  AFORMAT_AMR;
        break;
    case CODEC_ID_WMAV1:
    case CODEC_ID_WMAV2:
        format =  AFORMAT_WMA;
        break;
    case CODEC_ID_FLAC:
        format = AFORMAT_FLAC;
        break;

    case CODEC_ID_WMAPRO:
        format = AFORMAT_WMAPRO;
        break;

    case CODEC_ID_PCM_BLURAY:
        format = AFORMAT_PCM_BLURAY;
        break;
    case CODEC_ID_ALAC:
        format = AFORMAT_ALAC;
        break;
    case CODEC_ID_VORBIS:
        format =    AFORMAT_VORBIS;
        break;
    case CODEC_ID_APE:
        format =    AFORMAT_APE;
        break;
    case CODEC_ID_PCM_WIFIDISPLAY:
        format = AFORMAT_PCM_WIFIDISPLAY;
        break;
    default:
        format = AFORMAT_UNSUPPORT;
        ALOGV("audio codec_id=0x%x\n", id);
    }
    ALOGV("[audioTypeConvert]audio codec_id=0x%x format=%d\n", id, format);

    return format;
}

status_t AmNuPlayer::updateMediaInfo(void) {
    ALOGI("updateMediaInfo");
    maudio_info_t *ainfo;
    mvideo_info_t *vinfo;
    mStreamInfo.stream_info.total_video_num = 0;
    mStreamInfo.stream_info.total_audio_num = 0;
    sp<AMessage> aformat= mSource->getFormat(true);  //audio
    sp<AMessage> vformat= mSource->getFormat(false);   // video
    if (vformat != NULL) {
        vinfo = (mvideo_info_t *)malloc(sizeof(mvideo_info_t));
        memset(vinfo, 0, sizeof(mvideo_info_t));
        int32_t codecid=-1,width=-1,height=-1,bitrate=-1;
        int64_t duration = -1;
        vinfo->index = 0;
        if (vformat->findInt32("code-id", &codecid)) {
            vinfo->id = codecid;
        }
        if (vformat->findInt32("width", &width)) {
            vinfo->width = width;
        }
        if (vformat->findInt32("height", &height)) {
            vinfo->height = height;
        }
        if (vformat->findInt64("durationUs", &duration)) {
             vinfo->duartion = duration;
        }
        if (vformat->findInt32("bit-rate", &bitrate)) {
            vinfo->bit_rate = bitrate;
        }
        vinfo->format  = (vformat_t)0;
        vinfo->aspect_ratio_num = 0;
        vinfo->aspect_ratio_den = 0;
        vinfo->frame_rate_num   = 0;
        vinfo->frame_rate_den   = 0;
        vinfo->video_rotation_degree = 0;
        mStreamInfo.video_info[mStreamInfo.stream_info.total_video_num] = vinfo;
        mStreamInfo.stream_info.total_video_num++;
    }
    if (aformat != NULL) {
        ainfo = (maudio_info_t *)malloc(sizeof(maudio_info_t));
        memset(ainfo, 0, sizeof(maudio_info_t));
        int32_t codecid=-1, bitrate=-1, samplerate=-1, channelcount=-1;
        int64_t duration=-1;
        ainfo->index = 0;
        AString mime;
        if (aformat->findInt32("code-id", &codecid)) {
            ainfo->id = codecid;
        }
        if (aformat->findString("mime", &mime)) {
            if (mime == MEDIA_MIMETYPE_AUDIO_DTSHD) {
                ALOGI("mime:%s",MEDIA_MIMETYPE_AUDIO_DTSHD);
                mStrCurrentAudioCodec = "DTSHD";
                ainfo->id = CODEC_ID_DTS;
            }
        }
        if (aformat->findInt32("bit-rate", &bitrate)) {
            ainfo->bit_rate = bitrate;
        }
        if (aformat->findInt32("channel-count", &channelcount)) {
            ainfo->channel = channelcount;
        }
        if (aformat->findInt32("sample-rate", &samplerate)) {
            ainfo->sample_rate = samplerate;
        }
        if (vformat->findInt64("durationUs", &duration)) {
            ainfo->duration = duration;
        }
        if (ainfo->id  > 0)
            ainfo->aformat      = audioTypeConvert((enum CodecID)ainfo->id);
        ALOGI("aformat %d",ainfo->aformat);
        mStreamInfo.audio_info[mStreamInfo.stream_info.total_audio_num] = ainfo;
        mStreamInfo.stream_info.total_audio_num++;

    }
    mStreamInfo.stream_info.cur_video_index = 0;
    mStreamInfo.stream_info.cur_audio_index = 0;
    mStreamInfo.stream_info.cur_sub_index   = -1;

    return OK;

}

status_t AmNuPlayer::getMediaInfo(Parcel* reply){
    //Mutex::Autolock autoLock(mLock);
    ALOGI("AmNuPlayer::getMediaInfo");
    int datapos=reply->dataPosition();
    updateMediaInfo();
    //filename
    reply->writeString16(String16("-1"));
    //duration
    if (mStreamInfo.stream_info.duration > 0)
        reply->writeInt32(mStreamInfo.stream_info.duration);
    else
        reply->writeInt32(-1);

    reply->writeString16(String16("null"));

    //bitrate
      if (mStreamInfo.stream_info.bitrate > 0)
        reply->writeInt32(mStreamInfo.stream_info.bitrate);
    else
        reply->writeInt32(-1);

    //filetype
    reply->writeInt32(mStreamInfo.stream_info.type);

    /*select info*/
    reply->writeInt32(mStreamInfo.stream_info.cur_video_index);
    reply->writeInt32(mStreamInfo.stream_info.cur_audio_index);
    reply->writeInt32(mStreamInfo.stream_info.cur_sub_index);
    ALOGI("--cur video:%d cur audio:%d cur sub:%d \n",mStreamInfo.stream_info.cur_video_index,mStreamInfo.stream_info.cur_audio_index,mStreamInfo.stream_info.cur_sub_index);
    /*build video info*/
    reply->writeInt32(mStreamInfo.stream_info.total_video_num);
    for (int i = 0;i < mStreamInfo.stream_info.total_video_num; i ++) {
        reply->writeInt32(mStreamInfo.video_info[i]->index);
        reply->writeInt32(mStreamInfo.video_info[i]->id);
        reply->writeString16(String16("unknow"));
        reply->writeInt32(mStreamInfo.video_info[i]->width);
        reply->writeInt32(mStreamInfo.video_info[i]->height);
        ALOGI("--video index:%d id:%d totlanum:%d width:%d height:%d \n",mStreamInfo.video_info[i]->index,mStreamInfo.video_info[i]->id,mStreamInfo.stream_info.total_video_num,mStreamInfo.video_info[i]->width,mStreamInfo.video_info[i]->height);
    }

    /*build audio info*/
    reply->writeInt32(mStreamInfo.stream_info.total_audio_num);
    for (int i = 0; i < mStreamInfo.stream_info.total_audio_num; i ++) {
        reply->writeInt32(mStreamInfo.audio_info[i]->index);
        reply->writeInt32(mStreamInfo.audio_info[i]->id);
        reply->writeInt32(mStreamInfo.audio_info[i]->aformat);
        reply->writeInt32(mStreamInfo.audio_info[i]->channel);
        reply->writeInt32(mStreamInfo.audio_info[i]->sample_rate);
        ALOGI("--audio index:%d id:%d totlanum:%d channel:%d samplerate:%d aformat=%d\n",mStreamInfo.audio_info[i]->index,mStreamInfo.audio_info[i]->id,mStreamInfo.stream_info.total_audio_num,mStreamInfo.audio_info[i]->channel,mStreamInfo.audio_info[i]->sample_rate,mStreamInfo.audio_info[i]->aformat);
    }

    /*build subtitle info*/
    reply->writeInt32(0);
    reply->writeInt32(0);
    reply->setDataPosition(datapos);
    return OK;
}



status_t AmNuPlayer::setParameter(int key , const Parcel &  request ) {
    if (KEY_PARAMETER_AML_PLAYER_SET_DTS_ASSET == key) {
        int ApreID =0,ApreAssetSel;
        const String16 uri16 = request.readString16();
        String8 keyStr = String8(uri16);
        ALOGI("setParameter %d=[%s]\n", key, keyStr.string());
        ApreID = getintfromString8(keyStr, "dtsApre:");
        ApreAssetSel=getintfromString8(keyStr, "dtsAsset:");
        if (ApreID >= 0 && ApreAssetSel >= 0) {
            dtsm6_set_exchange_info(&ApreID,&ApreAssetSel);
        }
    }else{
        ALOGI("unsupport setParameter value!=%d\n", key);
    }
    return OK;
}

status_t AmNuPlayer::getParameter(int key, Parcel *reply){
    if (key == KEY_PARAMETER_AML_PLAYER_GET_DTS_ASSET_TOTAL) {
        if (mSource == NULL)
            return INVALID_OPERATION;
        int32_t codecid;
        if (mStrCurrentAudioCodec != NULL && !strncmp(mStrCurrentAudioCodec,"DTS",3)) {
            int32_t ApresAssetsArray[32]={0};
            dtsm6_get_exchange_info(NULL,&DtshdApreTotal,NULL,NULL,ApresAssetsArray,NULL,NULL);
            reply->writeInt32(DtshdApreTotal);
            reply->writeInt32Array(32,ApresAssetsArray);
        }else{
            int32_t ApresAssetsArray[32]={0};
            reply->writeInt32(0);
            reply->writeInt32Array(32,ApresAssetsArray);
        }
    }else if (KEY_PARAMETER_AML_PLAYER_GET_MEDIA_INFO == key) {
        getMediaInfo(reply);
    }else {
        ALOGI("unsupport setParameter value!=%d\n", key);
    }
    return  OK;
}


void AmNuPlayer::setUID(uid_t uid) {
    mUIDValid = true;
    mUID = uid;
}

void AmNuPlayer::setDriver(const wp<AmNuPlayerDriver> &driver) {
    mDriver = driver;
}

void AmNuPlayer::setDataSourceAsync(const sp<IStreamSource> &source) {
    sp<AMessage> msg = new AMessage(kWhatSetDataSource, this);

    sp<AMessage> notify = new AMessage(kWhatSourceNotify, this);

    msg->setObject("source", new StreamingSource(notify, source));
    msg->post();
}

static bool IsHTTPLiveURL(const char *url) {
    if (!strncasecmp("http://", url, 7)
        || !strncasecmp("https://", url, 8)) {
        return true;
    }

    return false;
}

void AmNuPlayer::setDataSourceAsync(
        const sp<IMediaHTTPService> &httpService,
        const char *url,
        const KeyedVector<String8, String8> *headers) {

    sp<AMessage> msg = new AMessage(kWhatSetDataSource, this);
    size_t len = strlen(url);

    sp<AMessage> notify = new AMessage(kWhatSourceNotify, this);

    sp<Source> source;
    mEnableFrameRate = false;
    if (IsHTTPLiveURL(url)) {
        source = new HTTPLiveSource(notify, httpService, url, headers, interrupt_callback);

        // only enable auto frame-rate for HLS
        char value[PROPERTY_VALUE_MAX];
        if (property_get("media.hls.frame-rate", value, NULL)) {
            mEnableFrameRate = atoi(value);
        }
    } else if (!strncasecmp(url, "rtsp://", 7)) {
        source = new RTSPSource(
                notify, httpService, url, headers, mUIDValid, mUID);
    } else if ((!strncasecmp(url, "http://", 7)
                || !strncasecmp(url, "https://", 8))
                    && ((len >= 4 && !strcasecmp(".sdp", &url[len - 4]))
                    || strstr(url, ".sdp?"))) {
        source = new RTSPSource(
                notify, httpService, url, headers, mUIDValid, mUID, true);
    } else {
        sp<GenericSource> genericSource =
                new GenericSource(notify, mUIDValid, mUID);
        // Don't set FLAG_SECURE on mSourceFlags here for widevine.
        // The correct flags will be updated in Source::kWhatFlagsChanged
        // handler when  GenericSource is prepared.

        status_t err = genericSource->setDataSource(httpService, url, headers);

        if (err == OK) {
            source = genericSource;
        } else {
            ALOGE("Failed to set data source!");
        }
    }
    msg->setObject("source", source);
    msg->post();
}

void AmNuPlayer::setDataSourceAsync(int fd, int64_t offset, int64_t length) {
    sp<AMessage> msg = new AMessage(kWhatSetDataSource, this);

    sp<AMessage> notify = new AMessage(kWhatSourceNotify, this);

    sp<GenericSource> source =
            new GenericSource(notify, mUIDValid, mUID);

    status_t err = source->setDataSource(fd, offset, length);

    if (err != OK) {
        ALOGE("Failed to set data source!");
        source = NULL;
    }

    msg->setObject("source", source);
    msg->post();
}

void AmNuPlayer::prepareAsync() {
    (new AMessage(kWhatPrepare, this))->post();
}

void AmNuPlayer::setVideoSurfaceTextureAsync(
        const sp<IGraphicBufferProducer> &bufferProducer) {
    sp<AMessage> msg = new AMessage(kWhatSetVideoNativeWindow, this);

    if (bufferProducer == NULL) {
        msg->setObject("native-window", NULL);
    } else {
        sp<Surface> tmpSurface = new Surface(bufferProducer, true /* controlledByApp */);
        if (mNativeWindow != NULL) {
            mNewSurface = tmpSurface;
        }
        msg->setObject(
                "native-window",
                new NativeWindowWrapper(tmpSurface));
    }

    msg->post();
}

void AmNuPlayer::setAudioSink(const sp<MediaPlayerBase::AudioSink> &sink) {
    sp<AMessage> msg = new AMessage(kWhatSetAudioSink, this);
    msg->setObject("sink", sink);
    msg->post();
}

void AmNuPlayer::start() {
    (new AMessage(kWhatStart, this))->post();
}

void AmNuPlayer::pause() {
    (new AMessage(kWhatPause, this))->post();
}

void AmNuPlayer::resetAsync() {
    if (mSource != NULL) {
        // During a reset, the data source might be unresponsive already, we need to
        // disconnect explicitly so that reads exit promptly.
        // We can't queue the disconnect request to the looper, as it might be
        // queued behind a stuck read and never gets processed.
        // Doing a disconnect outside the looper to allows the pending reads to exit
        // (either successfully or with error).
        mSource->disconnect();
    }

    (new AMessage(kWhatReset, this))->post();
}

void AmNuPlayer::seekToAsync(int64_t seekTimeUs, bool needNotify) {
    sp<AMessage> msg = new AMessage(kWhatSeek, this);
    msg->setInt64("seekTimeUs", seekTimeUs);
    msg->setInt32("needNotify", needNotify);
    msg->post();
}


void AmNuPlayer::writeTrackInfo(
        Parcel* reply, const sp<AMessage> format) const {
    int32_t trackType;
    CHECK(format->findInt32("type", &trackType));

    AString mime;
    if (!format->findString("mime", &mime)) {
        // Java MediaPlayer only uses mimetype for subtitle and timedtext tracks.
        // If we can't find the mimetype here it means that we wouldn't be needing
        // the mimetype on the Java end. We still write a placeholder mime to keep the
        // (de)serialization logic simple.
        if (trackType == MEDIA_TRACK_TYPE_AUDIO) {
            mime = "audio/";
        } else if (trackType == MEDIA_TRACK_TYPE_VIDEO) {
            mime = "video/";
        } else {
            TRESPASS();
        }
    }

    AString lang;
    CHECK(format->findString("language", &lang));

    reply->writeInt32(2); // write something non-zero
    reply->writeInt32(trackType);
    reply->writeString16(String16(mime.c_str()));
    reply->writeString16(String16(lang.c_str()));

    if (trackType == MEDIA_TRACK_TYPE_SUBTITLE) {
        int32_t isAuto, isDefault, isForced;
        CHECK(format->findInt32("auto", &isAuto));
        CHECK(format->findInt32("default", &isDefault));
        CHECK(format->findInt32("forced", &isForced));

        reply->writeInt32(isAuto);
        reply->writeInt32(isDefault);
        reply->writeInt32(isForced);
    }
}

void AmNuPlayer::onMessageReceived(const sp<AMessage> &msg) {
    switch (msg->what()) {
        case kWhatSetDataSource:
        {
            ALOGV("kWhatSetDataSource");

            CHECK(mSource == NULL);

            status_t err = OK;
            sp<RefBase> obj;
            CHECK(msg->findObject("source", &obj));
            if (obj != NULL) {
                mSource = static_cast<Source *>(obj.get());
                mSelfThreadId = androidGetThreadId();
                mSource->setParentThreadId(mSelfThreadId);
            } else {
                err = UNKNOWN_ERROR;
            }

            CHECK(mDriver != NULL);
            sp<AmNuPlayerDriver> driver = mDriver.promote();
            if (driver != NULL) {
                driver->notifySetDataSourceCompleted(err);
            }
            break;
        }

        case kWhatPrepare:
        {
            mSource->prepareAsync();
            break;
        }

        case kWhatGetTrackInfo:
        {
            sp<AReplyToken> replyID;
            CHECK(msg->senderAwaitsResponse(&replyID));

            Parcel* reply;
            CHECK(msg->findPointer("reply", (void**)&reply));

            size_t inbandTracks = 0;
            if (mSource != NULL) {
                inbandTracks = mSource->getTrackCount();
            }

            size_t ccTracks = 0;
            if (mCCDecoder != NULL) {
                ccTracks = mCCDecoder->getTrackCount();
            }

            ALOGI("Got inband tracks(%d), cctracks(%d)", inbandTracks, ccTracks);
            // total track count
            reply->writeInt32(inbandTracks + ccTracks);

            // write inband tracks
            for (size_t i = 0; i < inbandTracks; ++i) {
                writeTrackInfo(reply, mSource->getTrackInfo(i));
            }

            // write CC track
            for (size_t i = 0; i < ccTracks; ++i) {
                writeTrackInfo(reply, mCCDecoder->getTrackInfo(i));
            }

            sp<AMessage> response = new AMessage;
            response->postReply(replyID);
            break;
        }

        case kWhatGetSelectedTrack:
        {
            status_t err = INVALID_OPERATION;
            if (mSource != NULL) {
                err = OK;

                int32_t type32;
                CHECK(msg->findInt32("type", (int32_t*)&type32));
                media_track_type type = (media_track_type)type32;
                ssize_t selectedTrack = mSource->getSelectedTrack(type);

                Parcel* reply;
                CHECK(msg->findPointer("reply", (void**)&reply));
                reply->writeInt32(selectedTrack);
            }

            sp<AMessage> response = new AMessage;
            response->setInt32("err", err);

            sp<AReplyToken> replyID;
            CHECK(msg->senderAwaitsResponse(&replyID));
            response->postReply(replyID);
            break;
        }

        case kWhatSelectTrack:
        {
            sp<AReplyToken> replyID;
            CHECK(msg->senderAwaitsResponse(&replyID));

            size_t trackIndex;
            int32_t select;
            int64_t timeUs;
            CHECK(msg->findSize("trackIndex", &trackIndex));
            CHECK(msg->findInt32("select", &select));
            CHECK(msg->findInt64("timeUs", &timeUs));

            status_t err = INVALID_OPERATION;

            size_t inbandTracks = 0;
            if (mSource != NULL) {
                inbandTracks = mSource->getTrackCount();
            }
            size_t ccTracks = 0;
            if (mCCDecoder != NULL) {
                ccTracks = mCCDecoder->getTrackCount();
            }

            if (trackIndex < inbandTracks) {
                err = mSource->selectTrack(trackIndex, select, timeUs);

                if (!select && err == OK) {
                    int32_t type;
                    sp<AMessage> info = mSource->getTrackInfo(trackIndex);
                    if (info != NULL
                            && info->findInt32("type", &type)
                            && type == MEDIA_TRACK_TYPE_TIMEDTEXT) {
                        ++mTimedTextGeneration;
                    }
                }
            } else {
                trackIndex -= inbandTracks;

                if (trackIndex < ccTracks) {
                    err = mCCDecoder->selectTrack(trackIndex, select);
                }
            }

            sp<AMessage> response = new AMessage;
            response->setInt32("err", err);

            response->postReply(replyID);
            break;
        }

        case kWhatPollDuration:
        {
            int32_t generation;
            CHECK(msg->findInt32("generation", &generation));

            if (generation != mPollDurationGeneration) {
                // stale
                break;
            }

            int64_t durationUs;
            if (mDriver != NULL && mSource != NULL && mSource->getDuration(&durationUs) == OK) {
                sp<AmNuPlayerDriver> driver = mDriver.promote();
                if (driver != NULL) {
                    driver->notifyDuration(durationUs);
                }
            }

            msg->post(1000000ll);  // poll again in a second.
            break;
        }

        case kWhatSetVideoNativeWindow:
        {
            ALOGV("kWhatSetVideoNativeWindow");

            sp<RefBase> obj;
            CHECK(msg->findObject("native-window", &obj));

            if (mSource == NULL || mSource->getFormat(false /* audio */) == NULL) {
                performSetSurface(static_cast<NativeWindowWrapper *>(obj.get()));
                break;
            }

            mDeferredActions.push_back(
                    new FlushDecoderAction(FLUSH_CMD_FLUSH /* audio */,
                                           FLUSH_CMD_SHUTDOWN /* video */));

            mDeferredActions.push_back(
                    new SetSurfaceAction(
                        static_cast<NativeWindowWrapper *>(obj.get())));

            if (obj != NULL) {
                if (mStarted) {
                    // Issue a seek to refresh the video screen only if started otherwise
                    // the extractor may not yet be started and will assert.
                    // If the video decoder is not set (perhaps audio only in this case)
                    // do not perform a seek as it is not needed.
                    int64_t currentPositionUs = 0;
                    if (getCurrentPosition(&currentPositionUs) == OK) {
                        mDeferredActions.push_back(
                                new SeekAction(currentPositionUs, false /* needNotify */));
                    }
                }

                // If there is a new surface texture, instantiate decoders
                // again if possible.
                mDeferredActions.push_back(
                        new SimpleAction(&AmNuPlayer::performScanSources));
            }

            // After a flush without shutdown, decoder is paused.
            // Don't resume it until source seek is done, otherwise it could
            // start pulling stale data too soon.
            mDeferredActions.push_back(
                    new ResumeDecoderAction(false /* needNotify */));

            processDeferredActions();
            break;
        }

        case kWhatSetAudioSink:
        {
            ALOGV("kWhatSetAudioSink");

            sp<RefBase> obj;
            CHECK(msg->findObject("sink", &obj));

            mAudioSink = static_cast<MediaPlayerBase::AudioSink *>(obj.get());
            break;
        }

        case kWhatStart:
        {
            ALOGV("kWhatStart");
            if (mStarted) {
                onResume();
            } else {
                onStart();
            }
            mPausedByClient = false;
            break;
        }

        case kWhatScanSources:
        {
            int32_t generation;
            CHECK(msg->findInt32("generation", &generation));
            if (generation != mScanSourcesGeneration) {
                // Drop obsolete msg.
                break;
            }

            if (mEnableFrameRate && mFrameRate < 0.0) {
                int32_t num = 0;
                msg->findInt32("scan-num", &num);
                if (num < mWaitSeconds * 100) {     // wait up to 10 seconds
                    ALOGI("scan sources wait %d", num);
                    ++num;
                    msg->setInt32("scan-num", num);
                    msg->post(10 * 1000);
                    break;
                }
            }

            mScanSourcesPending = false;

            ALOGV("scanning sources haveAudio=%d, haveVideo=%d",
                 mAudioDecoder != NULL, mVideoDecoder != NULL);

            bool mHadAnySourcesBefore =
                (mAudioDecoder != NULL) || (mVideoDecoder != NULL);

            // initialize video before audio because successful initialization of
            // video may change deep buffer mode of audio.
            if (mNativeWindow != NULL) {
                instantiateDecoder(false, &mVideoDecoder);
            }

            // Don't try to re-open audio sink if there's an existing decoder.
            if (mAudioSink != NULL && mAudioDecoder == NULL) {
                sp<MetaData> audioMeta = mSource->getFormatMeta(true /* audio */);
                sp<AMessage> videoFormat = mSource->getFormat(false /* audio */);
                audio_stream_type_t streamType = mAudioSink->getAudioStreamType();
                const bool hasVideo = (videoFormat != NULL);
                const bool canOffload = canOffloadStream(
                        audioMeta, hasVideo, true /* is_streaming */, streamType);
                if (canOffload) {
                    if (!mOffloadAudio) {
                        mRenderer->signalEnableOffloadAudio();
                    }
                    // open audio sink early under offload mode.
                    sp<AMessage> format = mSource->getFormat(true /*audio*/);
                    tryOpenAudioSinkForOffload(format, hasVideo);
                }
                instantiateDecoder(true, &mAudioDecoder);
            }

            if (!mHadAnySourcesBefore
                    && (mAudioDecoder != NULL || mVideoDecoder != NULL)) {
                // This is the first time we've found anything playable.

                if (mSourceFlags & Source::FLAG_DYNAMIC_DURATION) {
                    schedulePollDuration();
                }
            }

            status_t err;
            if ((err = mSource->feedMoreTSData()) != OK) {
                if (mAudioDecoder == NULL && mVideoDecoder == NULL) {
                    // We're not currently decoding anything (no audio or
                    // video tracks found) and we just ran out of input data.

                    if (err == ERROR_END_OF_STREAM) {
                        notifyListener(MEDIA_PLAYBACK_COMPLETE, 0, 0);
                    } else {
                        notifyListener(MEDIA_ERROR, MEDIA_ERROR_UNKNOWN, err);
                    }
                }
                break;
            }

            if ((mAudioDecoder == NULL && mAudioSink != NULL)
                    || (mVideoDecoder == NULL && mNativeWindow != NULL)) {
                msg->post(100000ll);
                mScanSourcesPending = true;
            }
            break;
        }

        case kWhatVideoNotify:
        case kWhatAudioNotify:
        {
            bool audio = msg->what() == kWhatAudioNotify;

            int32_t currentDecoderGeneration =
                (audio? mAudioDecoderGeneration : mVideoDecoderGeneration);
            int32_t requesterGeneration = currentDecoderGeneration - 1;
            CHECK(msg->findInt32("generation", &requesterGeneration));

            if (requesterGeneration != currentDecoderGeneration) {
                ALOGV("got message from old %s decoder, generation(%d:%d)",
                        audio ? "audio" : "video", requesterGeneration,
                        currentDecoderGeneration);
                sp<AMessage> reply;
                if (!(msg->findMessage("reply", &reply))) {
                    return;
                }

                reply->setInt32("err", INFO_DISCONTINUITY);
                reply->post();
                return;
            }

            // restore the state of auto frame-rate after seek
            if (mEnableFrameRate && !audio && mAutoSwitch > 0) {
                amsysfs_set_sysfs_int("/sys/class/tv/policy_fr_auto_switch", mAutoSwitch);
                mAutoSwitch = -1;
            }

            int32_t what;
            CHECK(msg->findInt32("what", &what));

            if (what == DecoderBase::kWhatInputDiscontinuity) {
                int32_t formatChange;
                CHECK(msg->findInt32("formatChange", &formatChange));

                ALOGV("%s discontinuity: formatChange %d",
                        audio ? "audio" : "video", formatChange);

                if (formatChange) {
                    mDeferredActions.push_back(
                            new FlushDecoderAction(
                                audio ? FLUSH_CMD_SHUTDOWN : FLUSH_CMD_NONE,
                                audio ? FLUSH_CMD_NONE : FLUSH_CMD_SHUTDOWN));
                }

                mDeferredActions.push_back(
                        new SimpleAction(
                                &AmNuPlayer::performScanSources));

                processDeferredActions();
            } else if (what == DecoderBase::kWhatEOS) {
                int32_t err;
                CHECK(msg->findInt32("err", &err));

                if (err == ERROR_END_OF_STREAM) {
                    ALOGV("got %s decoder EOS", audio ? "audio" : "video");
                } else {
                    ALOGV("got %s decoder EOS w/ error %d",
                         audio ? "audio" : "video",
                         err);
                }

                mRenderer->queueEOS(audio, err);
            } else if (what == DecoderBase::kWhatFlushCompleted) {
                ALOGV("decoder %s flush completed", audio ? "audio" : "video");

                handleFlushComplete(audio, true /* isDecoder */);
                finishFlushIfPossible();
            } else if (what == DecoderBase::kWhatVideoSizeChanged) {
                sp<AMessage> format;
                CHECK(msg->findMessage("format", &format));

                sp<AMessage> inputFormat =
                        mSource->getFormat(false /* audio */);

                updateVideoSize(inputFormat, format);
            } else if (what == DecoderBase::kWhatShutdownCompleted) {
                ALOGV("%s shutdown completed", audio ? "audio" : "video");
                if (audio) {
                    mAudioDecoder.clear();
                    ++mAudioDecoderGeneration;

                    CHECK_EQ((int)mFlushingAudio, (int)SHUTTING_DOWN_DECODER);
                    mFlushingAudio = SHUT_DOWN;
                } else {
                    mVideoDecoder.clear();
                    ++mVideoDecoderGeneration;

                    CHECK_EQ((int)mFlushingVideo, (int)SHUTTING_DOWN_DECODER);
                    mFlushingVideo = SHUT_DOWN;
                }

                finishFlushIfPossible();
            } else if (what == DecoderBase::kWhatResumeCompleted) {
                finishResume();
            } else if (what == DecoderBase::kWhatError) {
                status_t err;
                if (!msg->findInt32("err", &err) || err == OK) {
                    err = UNKNOWN_ERROR;
                }

                // when the two surface is not the same, means the first surface has abandoned
                // ignore native_window_api_connect error: -19, No such device
                // AmNuPlayer will use the second surface to configure the decoder
                if (mNativeWindow != NULL && mNativeWindow->getSurfaceTextureClient() != mNewSurface) {
                    if (err == -19) {
                        break;
                    }
                }

                // Decoder errors can be due to Source (e.g. from streaming),
                // or from decoding corrupted bitstreams, or from other decoder
                // MediaCodec operations (e.g. from an ongoing reset or seek).
                // They may also be due to openAudioSink failure at
                // decoder start or after a format change.
                //
                // We try to gracefully shut down the affected decoder if possible,
                // rather than trying to force the shutdown with something
                // similar to performReset(). This method can lead to a hang
                // if MediaCodec functions block after an error, but they should
                // typically return INVALID_OPERATION instead of blocking.

                FlushStatus *flushing = audio ? &mFlushingAudio : &mFlushingVideo;
                ALOGE("received error(%#x) from %s decoder, flushing(%d), now shutting down",
                        err, audio ? "audio" : "video", *flushing);

                switch (*flushing) {
                    case NONE:
                        mDeferredActions.push_back(
                                new FlushDecoderAction(
                                    audio ? FLUSH_CMD_SHUTDOWN : FLUSH_CMD_NONE,
                                    audio ? FLUSH_CMD_NONE : FLUSH_CMD_SHUTDOWN));
                        processDeferredActions();
                        break;
                    case FLUSHING_DECODER:
                        *flushing = FLUSHING_DECODER_SHUTDOWN; // initiate shutdown after flush.
                        break; // Wait for flush to complete.
                    case FLUSHING_DECODER_SHUTDOWN:
                        break; // Wait for flush to complete.
                    case SHUTTING_DOWN_DECODER:
                        break; // Wait for shutdown to complete.
                    case FLUSHED:
                        // Widevine source reads must stop before releasing the video decoder.
                        if (!audio && mSource != NULL && mSourceFlags & Source::FLAG_SECURE) {
                            mSource->stop();
                        }
                        getDecoder(audio)->initiateShutdown(); // In the middle of a seek.
                        *flushing = SHUTTING_DOWN_DECODER;     // Shut down.
                        break;
                    case SHUT_DOWN:
                        finishFlushIfPossible();  // Should not occur.
                        break;                    // Finish anyways.
                }
                notifyListener(MEDIA_ERROR, MEDIA_ERROR_UNKNOWN, err);
            } else {
                ALOGV("Unhandled decoder notification %d '%c%c%c%c'.",
                      what,
                      what >> 24,
                      (what >> 16) & 0xff,
                      (what >> 8) & 0xff,
                      what & 0xff);
            }

            break;
        }

        case kWhatRendererNotify:
        {
            int32_t requesterGeneration = mRendererGeneration - 1;
            CHECK(msg->findInt32("generation", &requesterGeneration));
            if (requesterGeneration != mRendererGeneration) {
                ALOGV("got message from old renderer, generation(%d:%d)",
                        requesterGeneration, mRendererGeneration);
                return;
            }

            int32_t what;
            CHECK(msg->findInt32("what", &what));

            if (what == Renderer::kWhatEOS) {
                int32_t audio;
                CHECK(msg->findInt32("audio", &audio));

                int32_t finalResult;
                CHECK(msg->findInt32("finalResult", &finalResult));

                if (audio) {
                    mAudioEOS = true;
                } else {
                    mVideoEOS = true;
                }

                if (finalResult == ERROR_END_OF_STREAM) {
                    ALOGV("reached %s EOS", audio ? "audio" : "video");
                } else {
                    ALOGE("%s track encountered an error (%d)",
                         audio ? "audio" : "video", finalResult);

                    notifyListener(
                            MEDIA_ERROR, MEDIA_ERROR_UNKNOWN, finalResult);
                }

                if ((mAudioEOS || mAudioDecoder == NULL)
                        && (mVideoEOS || mVideoDecoder == NULL)) {
                    notifyListener(MEDIA_PLAYBACK_COMPLETE, 0, 0);
                }
            } else if (what == Renderer::kWhatFlushComplete) {
                int32_t audio;
                CHECK(msg->findInt32("audio", &audio));

                ALOGV("renderer %s flush completed.", audio ? "audio" : "video");
                handleFlushComplete(audio, false /* isDecoder */);
                finishFlushIfPossible();
            } else if (what == Renderer::kWhatVideoRenderingStart) {
                notifyListener(MEDIA_INFO, MEDIA_INFO_RENDERING_START, 0);
            } else if (what == Renderer::kWhatMediaRenderingStart) {
                ALOGV("media rendering started");
                notifyListener(MEDIA_STARTED, 0, 0);
            } else if (what == Renderer::kWhatAudioOffloadTearDown) {
                ALOGV("Tear down audio offload, fall back to s/w path if due to error.");
                int64_t positionUs;
                CHECK(msg->findInt64("positionUs", &positionUs));
                int32_t reason;
                CHECK(msg->findInt32("reason", &reason));
                closeAudioSink();
                mAudioDecoder.clear();
                ++mAudioDecoderGeneration;
                mRenderer->flush(
                        true /* audio */, false /* notifyComplete */);
                if (mVideoDecoder != NULL) {
                    mRenderer->flush(
                            false /* audio */, false /* notifyComplete */);
                }

                performSeek(positionUs, false /* needNotify */);
                if (reason == Renderer::kDueToError) {
                    mRenderer->signalDisableOffloadAudio();
                    mOffloadAudio = false;
                    instantiateDecoder(true /* audio */, &mAudioDecoder);
                }
            }
            break;
        }

        case kWhatMoreDataQueued:
        {
            break;
        }

        case kWhatReset:
        {
            ALOGV("kWhatReset");

            mDeferredActions.push_back(
                    new FlushDecoderAction(
                        FLUSH_CMD_SHUTDOWN /* audio */,
                        FLUSH_CMD_SHUTDOWN /* video */));

            mDeferredActions.push_back(
                    new SimpleAction(&AmNuPlayer::performReset));

            processDeferredActions();
            break;
        }

        case kWhatSeek:
        {
            int64_t seekTimeUs;
            int32_t needNotify;
            CHECK(msg->findInt64("seekTimeUs", &seekTimeUs));
            CHECK(msg->findInt32("needNotify", &needNotify));

            ALOGV("kWhatSeek seekTimeUs=%lld us, needNotify=%d",
                    seekTimeUs, needNotify);

            // temporarily close auto frame-rate to avoid black srceen when seek
            if (mEnableFrameRate && mFrameRate > 0.0) {
                int64_t nowUs = ALooper::GetNowUs();
                int64_t timeSinceStart = nowUs - mStartTimeUs;
                if (timeSinceStart > 100000) {
                    mAutoSwitch = amsysfs_get_sysfs_int("/sys/class/tv/policy_fr_auto_switch");
                    amsysfs_set_sysfs_int("/sys/class/tv/policy_fr_auto_switch", 0);
                }
            }

            mDeferredActions.push_back(
                    new FlushDecoderAction(FLUSH_CMD_FLUSH /* audio */,
                                           FLUSH_CMD_FLUSH /* video */));

            mDeferredActions.push_back(
                    new SeekAction(seekTimeUs, needNotify));

            // After a flush without shutdown, decoder is paused.
            // Don't resume it until source seek is done, otherwise it could
            // start pulling stale data too soon.
            mDeferredActions.push_back(
                    new ResumeDecoderAction(needNotify));

            processDeferredActions();
            break;
        }

        case kWhatPause:
        {
            onPause();
            mPausedByClient = true;
            break;
        }

        case kWhatSourceNotify:
        {
            onSourceNotify(msg);
            break;
        }

        case kWhatClosedCaptionNotify:
        {
            onClosedCaptionNotify(msg);
            break;
        }

        default:
            TRESPASS();
            break;
    }
}

void AmNuPlayer::onResume() {
    if (!mPaused) {
        return;
    }
    mPaused = false;
    if (mSource != NULL) {
        mSource->resume();
    } else {
        ALOGW("resume called when source is gone or not set");
    }
    // |mAudioDecoder| may have been released due to the pause timeout, so re-create it if
    // needed.
    if (audioDecoderStillNeeded() && mAudioDecoder == NULL) {
        instantiateDecoder(true /* audio */, &mAudioDecoder);
    }
    if (mRenderer != NULL) {
        mRenderer->resume();
    } else {
        ALOGW("resume called when renderer is gone or not set");
    }
}

status_t AmNuPlayer::onInstantiateSecureDecoders() {
    status_t err;
    if (!(mSourceFlags & Source::FLAG_SECURE)) {
        return BAD_TYPE;
    }

    if (mRenderer != NULL) {
        ALOGE("renderer should not be set when instantiating secure decoders");
        return UNKNOWN_ERROR;
    }

    // TRICKY: We rely on mRenderer being null, so that decoder does not start requesting
    // data on instantiation.
    if (mNativeWindow != NULL) {
        err = instantiateDecoder(false, &mVideoDecoder);
        if (err != OK) {
            return err;
        }
    }

    if (mAudioSink != NULL) {
        err = instantiateDecoder(true, &mAudioDecoder);
        if (err != OK) {
            return err;
        }
    }
    return OK;
}

void AmNuPlayer::onStart() {
    mOffloadAudio = false;
    mAudioEOS = false;
    mVideoEOS = false;
    mStarted = true;

    mSource->start();

    uint32_t flags = 0;

    if (mSource->isRealTime()) {
        flags |= Renderer::FLAG_REAL_TIME;
    }

    sp<MetaData> audioMeta = mSource->getFormatMeta(true /* audio */);
    audio_stream_type_t streamType = AUDIO_STREAM_MUSIC;
    if (mAudioSink != NULL) {
        streamType = mAudioSink->getAudioStreamType();
    }

    sp<AMessage> videoFormat = mSource->getFormat(false /* audio */);

    mOffloadAudio =
        canOffloadStream(audioMeta, (videoFormat != NULL),
                         true /* is_streaming */, streamType);
    if (mOffloadAudio) {
        flags |= Renderer::FLAG_OFFLOAD_AUDIO;
    }

    sp<AMessage> notify = new AMessage(kWhatRendererNotify, this);
    ++mRendererGeneration;
    notify->setInt32("generation", mRendererGeneration);
    mRenderer = new Renderer(mAudioSink, notify, flags);

    mRendererLooper = new ALooper;
    mRendererLooper->setName("NuPlayerRenderer");
    mRendererLooper->start(false, false, ANDROID_PRIORITY_AUDIO);
    mRendererLooper->registerHandler(mRenderer);

    sp<MetaData> meta = getFileMeta();
    int32_t rate;
    if (meta != NULL
            && meta->findInt32(kKeyFrameRate, &rate) && rate > 0) {
        mRenderer->setVideoFrameRate(rate);
    }

    if (mVideoDecoder != NULL) {
        mVideoDecoder->setRenderer(mRenderer);
        mRenderer->setHasMedia(false);
    }
    if (mAudioDecoder != NULL) {
        mAudioDecoder->setRenderer(mRenderer);
        mRenderer->setHasMedia(true);
    }

    mStartTimeUs = ALooper::GetNowUs();

    postScanSources();
}

void AmNuPlayer::onPause() {
    if (mPaused) {
        return;
    }
    mPaused = true;
    if (mSource != NULL) {
        mSource->pause();
    } else {
        ALOGW("pause called when source is gone or not set");
    }
    if (mRenderer != NULL) {
        mRenderer->pause();
    } else {
        ALOGW("pause called when renderer is gone or not set");
    }
}

bool AmNuPlayer::audioDecoderStillNeeded() {
    // Audio decoder is no longer needed if it's in shut/shutting down status.
    return ((mFlushingAudio != SHUT_DOWN) && (mFlushingAudio != SHUTTING_DOWN_DECODER));
}

void AmNuPlayer::handleFlushComplete(bool audio, bool isDecoder) {
    // We wait for both the decoder flush and the renderer flush to complete
    // before entering either the FLUSHED or the SHUTTING_DOWN_DECODER state.

    mFlushComplete[audio][isDecoder] = true;
    if (!mFlushComplete[audio][!isDecoder]) {
        return;
    }

    FlushStatus *state = audio ? &mFlushingAudio : &mFlushingVideo;
    switch (*state) {
        case FLUSHING_DECODER:
        {
            *state = FLUSHED;
            break;
        }

        case FLUSHING_DECODER_SHUTDOWN:
        {
            *state = SHUTTING_DOWN_DECODER;

            ALOGV("initiating %s decoder shutdown", audio ? "audio" : "video");
            if (!audio) {
                // Widevine source reads must stop before releasing the video decoder.
                if (mSource != NULL && mSourceFlags & Source::FLAG_SECURE) {
                    mSource->stop();
                }
            }
            getDecoder(audio)->initiateShutdown();
            break;
        }

        default:
            // decoder flush completes only occur in a flushing state.
            LOG_ALWAYS_FATAL_IF(isDecoder, "decoder flush in invalid state %d", *state);
            break;
    }
}

void AmNuPlayer::finishFlushIfPossible() {
    if (mFlushingAudio != NONE && mFlushingAudio != FLUSHED
            && mFlushingAudio != SHUT_DOWN) {
        return;
    }

    if (mFlushingVideo != NONE && mFlushingVideo != FLUSHED
            && mFlushingVideo != SHUT_DOWN) {
        return;
    }

    ALOGV("both audio and video are flushed now.");

    mFlushingAudio = NONE;
    mFlushingVideo = NONE;

    clearFlushComplete();

    processDeferredActions();
}

void AmNuPlayer::postScanSources() {
    if (mScanSourcesPending) {
        return;
    }

    sp<AMessage> msg = new AMessage(kWhatScanSources, this);
    msg->setInt32("generation", mScanSourcesGeneration);
    msg->post();

    mScanSourcesPending = true;
}

void AmNuPlayer::tryOpenAudioSinkForOffload(const sp<AMessage> &format, bool hasVideo) {
    // Note: This is called early in AmNuPlayer to determine whether offloading
    // is possible; otherwise the decoders call the renderer openAudioSink directly.

    status_t err = mRenderer->openAudioSink(
            format, true /* offloadOnly */, hasVideo, AUDIO_OUTPUT_FLAG_NONE, &mOffloadAudio);
    if (err != OK) {
        // Any failure we turn off mOffloadAudio.
        mOffloadAudio = false;
    } else if (mOffloadAudio) {
        sp<MetaData> audioMeta =
                mSource->getFormatMeta(true /* audio */);
        sendMetaDataToHal(mAudioSink, audioMeta);
    }
}

void AmNuPlayer::closeAudioSink() {
    mRenderer->closeAudioSink();
}

status_t AmNuPlayer::instantiateDecoder(bool audio, sp<DecoderBase> *decoder) {
    if (*decoder != NULL) {
        return OK;
    }

    sp<AMessage> format = mSource->getFormat(audio);

    if (format == NULL) {
        return -EWOULDBLOCK;
    }

    if (!audio) {
        AString mime;
        CHECK(format->findString("mime", &mime));

        sp<AMessage> ccNotify = new AMessage(kWhatClosedCaptionNotify, this);
        if (mCCDecoder == NULL) {
            mCCDecoder = new CCDecoder(ccNotify);
        }

        if (mSourceFlags & Source::FLAG_SECURE) {
            format->setInt32("secure", true);
        }

        if (mSourceFlags & Source::FLAG_PROTECTED) {
            format->setInt32("protected", true);
        }
    }

    if (audio) {
        sp<AMessage> notify = new AMessage(kWhatAudioNotify, this);
        ++mAudioDecoderGeneration;
        notify->setInt32("generation", mAudioDecoderGeneration);

        if (mOffloadAudio) {
            *decoder = new DecoderPassThrough(notify, mSource, mRenderer);
        } else {
            *decoder = new Decoder(notify, mSource, mRenderer);
        }
        mRenderer->setHasMedia(true);
    } else {
        sp<AMessage> notify = new AMessage(kWhatVideoNotify, this);
        ++mVideoDecoderGeneration;
        notify->setInt32("generation", mVideoDecoderGeneration);
        format->setFloat("frame-rate", mFrameRate);

        *decoder = new Decoder(
                notify, mSource, mRenderer, mNativeWindow, mCCDecoder);
        mRenderer->setHasMedia(false);

        // enable FRC if high-quality AV sync is requested, even if not
        // queuing to native window, as this will even improve textureview
        // playback.
        {
            char value[PROPERTY_VALUE_MAX];
            if (property_get("persist.sys.media.avsync", value, NULL) &&
                    (!strcmp("1", value) || !strcasecmp("true", value))) {
                format->setInt32("auto-frc", 1);
            }
        }
    }
    (*decoder)->init();
    (*decoder)->configure(format);

    // allocate buffers to decrypt widevine source buffers
    if (!audio && (mSourceFlags & Source::FLAG_SECURE)) {
        Vector<sp<ABuffer> > inputBufs;
        CHECK_EQ((*decoder)->getInputBuffers(&inputBufs), (status_t)OK);

        Vector<MediaBuffer *> mediaBufs;
        for (size_t i = 0; i < inputBufs.size(); i++) {
            const sp<ABuffer> &buffer = inputBufs[i];
            MediaBuffer *mbuf = new MediaBuffer(buffer->data(), buffer->size());
            mediaBufs.push(mbuf);
        }

        status_t err = mSource->setBuffers(audio, mediaBufs);
        if (err != OK) {
            for (size_t i = 0; i < mediaBufs.size(); ++i) {
                mediaBufs[i]->release();
            }
            mediaBufs.clear();
            ALOGE("Secure source didn't support secure mediaBufs.");
            return err;
        }
    }
    return OK;
}

void AmNuPlayer::updateVideoSize(
        const sp<AMessage> &inputFormat,
        const sp<AMessage> &outputFormat) {
    if (inputFormat == NULL) {
        ALOGW("Unknown video size, reporting 0x0!");
        notifyListener(MEDIA_SET_VIDEO_SIZE, 0, 0);
        return;
    }

    int32_t displayWidth, displayHeight;
    int32_t cropLeft, cropTop, cropRight, cropBottom;

    if (outputFormat != NULL) {
        int32_t width, height;
        CHECK(outputFormat->findInt32("width", &width));
        CHECK(outputFormat->findInt32("height", &height));

        int32_t cropLeft, cropTop, cropRight, cropBottom;
        CHECK(outputFormat->findRect(
                    "crop",
                    &cropLeft, &cropTop, &cropRight, &cropBottom));

        displayWidth = cropRight - cropLeft + 1;
        displayHeight = cropBottom - cropTop + 1;

        ALOGV("Video output format changed to %d x %d "
             "(crop: %d x %d @ (%d, %d))",
             width, height,
             displayWidth,
             displayHeight,
             cropLeft, cropTop);
    } else {
        CHECK(inputFormat->findInt32("width", &displayWidth));
        CHECK(inputFormat->findInt32("height", &displayHeight));

        ALOGV("Video input format %d x %d", displayWidth, displayHeight);
    }

    // Take into account sample aspect ratio if necessary:
    int32_t sarWidth, sarHeight;
    if (inputFormat->findInt32("sar-width", &sarWidth)
            && inputFormat->findInt32("sar-height", &sarHeight)) {
        ALOGV("Sample aspect ratio %d : %d", sarWidth, sarHeight);

        displayWidth = (displayWidth * sarWidth) / sarHeight;

        ALOGV("display dimensions %d x %d", displayWidth, displayHeight);
    }

    int32_t rotationDegrees;
    if (!inputFormat->findInt32("rotation-degrees", &rotationDegrees)) {
        rotationDegrees = 0;
    }

    if (rotationDegrees == 90 || rotationDegrees == 270) {
        int32_t tmp = displayWidth;
        displayWidth = displayHeight;
        displayHeight = tmp;
    }

    notifyListener(
            MEDIA_SET_VIDEO_SIZE,
            displayWidth,
            displayHeight);
}

void AmNuPlayer::notifyListener(int msg, int ext1, int ext2, const Parcel *in) {
    if (mDriver == NULL) {
        return;
    }

    sp<AmNuPlayerDriver> driver = mDriver.promote();

    if (driver == NULL) {
        return;
    }

    driver->notifyListener(msg, ext1, ext2, in);
}

void AmNuPlayer::flushDecoder(bool audio, bool needShutdown) {
    ALOGI("[%s] flushDecoder needShutdown=%d",
          audio ? "audio" : "video", needShutdown);

    const sp<DecoderBase> &decoder = getDecoder(audio);
    if (decoder == NULL) {
        ALOGI("flushDecoder %s without decoder present",
             audio ? "audio" : "video");
        return;
    }

    // Make sure we don't continue to scan sources until we finish flushing.
    ++mScanSourcesGeneration;
    mScanSourcesPending = false;

    decoder->signalFlush();

    FlushStatus newStatus =
        needShutdown ? FLUSHING_DECODER_SHUTDOWN : FLUSHING_DECODER;

    mFlushComplete[audio][false /* isDecoder */] = (mRenderer == NULL);
    mFlushComplete[audio][true /* isDecoder */] = false;
    if (audio) {
        ALOGE_IF(mFlushingAudio != NONE,
                "audio flushDecoder() is called in state %d", mFlushingAudio);
        mFlushingAudio = newStatus;
    } else {
        ALOGE_IF(mFlushingVideo != NONE,
                "video flushDecoder() is called in state %d", mFlushingVideo);
        mFlushingVideo = newStatus;
    }
}

void AmNuPlayer::queueDecoderShutdown(
        bool audio, bool video, const sp<AMessage> &reply) {
    ALOGI("queueDecoderShutdown audio=%d, video=%d", audio, video);

    mDeferredActions.push_back(
            new FlushDecoderAction(
                audio ? FLUSH_CMD_SHUTDOWN : FLUSH_CMD_NONE,
                video ? FLUSH_CMD_SHUTDOWN : FLUSH_CMD_NONE));

    mDeferredActions.push_back(
            new SimpleAction(&AmNuPlayer::performScanSources));

    mDeferredActions.push_back(new PostMessageAction(reply));

    processDeferredActions();
}

status_t AmNuPlayer::setVideoScalingMode(int32_t mode) {
    mVideoScalingMode = mode;
    if (mNativeWindow != NULL) {
        status_t ret = native_window_set_scaling_mode(
                mNativeWindow->getNativeWindow().get(), mVideoScalingMode);
        if (ret != OK) {
            ALOGE("Failed to set scaling mode (%d): %s",
                -ret, strerror(-ret));
            return ret;
        }
    }
    return OK;
}

status_t AmNuPlayer::getTrackInfo(Parcel* reply) const {
    sp<AMessage> msg = new AMessage(kWhatGetTrackInfo, this);
    msg->setPointer("reply", reply);

    sp<AMessage> response;
    status_t err = msg->postAndAwaitResponse(&response);
    return err;
}

status_t AmNuPlayer::getSelectedTrack(int32_t type, Parcel* reply) const {
    sp<AMessage> msg = new AMessage(kWhatGetSelectedTrack, this);
    msg->setPointer("reply", reply);
    msg->setInt32("type", type);

    sp<AMessage> response;
    status_t err = msg->postAndAwaitResponse(&response);
    if (err == OK && response != NULL) {
        CHECK(response->findInt32("err", &err));
    }
    return err;
}

status_t AmNuPlayer::selectTrack(size_t trackIndex, bool select, int64_t timeUs) {
    sp<AMessage> msg = new AMessage(kWhatSelectTrack, this);
    msg->setSize("trackIndex", trackIndex);
    msg->setInt32("select", select);
    msg->setInt64("timeUs", timeUs);

    sp<AMessage> response;
    status_t err = msg->postAndAwaitResponse(&response);

    if (err != OK) {
        return err;
    }

    if (!response->findInt32("err", &err)) {
        err = OK;
    }

    return err;
}

status_t AmNuPlayer::getCurrentPosition(int64_t *mediaUs) {
    sp<Renderer> renderer = mRenderer;
    if (renderer == NULL) {
        return NO_INIT;
    }
    //ALOGI("getcurrent:%s",(mStrCurrentAudioCodec != NULL) ? "dts" : "null" );
    if ( mStrCurrentAudioCodec != NULL &&!strncmp(mStrCurrentAudioCodec,"DTS",3)) {
        int stream_type=0;
        int TotalApre=0;
        int MulAssetHint=0;
        int HPS_hint=0;
        dtsm6_get_exchange_info(&stream_type,&TotalApre,NULL,NULL,NULL,&MulAssetHint,&HPS_hint);
        ALOGI("%d:%d:%d:%d",stream_type,TotalApre,MulAssetHint,HPS_hint);
        if (TotalApre != DtshdApreTotal && TotalApre>0 ) {
            ALOGI("[%s %d]TotalApre changed:%d-->%d\n",__FUNCTION__,__LINE__,DtshdApreTotal,TotalApre);
            DtshdApreTotal=TotalApre;
            notifyListener(MEDIA_INFO, MEDIA_INFO_AMLOGIC_SHOW_DTS_ASSET,0,NULL);
        }
        if (stream_type != DtsHdStreamType) {
            ALOGI("[%s %d]DtsHdStreamType changed:%d-->%d\n",__FUNCTION__,__LINE__,DtsHdStreamType,stream_type);
             DtsHdStreamType=stream_type;
             if (DtsHdStreamType == 0x0)
                notifyListener(MEDIA_INFO, MEDIA_INFO_AMLOGIC_SHOW_DTS_ASSET,0,NULL);
             if (DtsHdStreamType == 0x1)
                notifyListener(MEDIA_INFO, MEDIA_INFO_AMLOGIC_SHOW_DTS_EXPRESS,0,NULL);
             else if (DtsHdStreamType==0x2)
                notifyListener(MEDIA_INFO, MEDIA_INFO_AMLOGIC_SHOW_DTS_HD_MASTER_AUDIO,0,NULL);
        }
        if (DtsHdMulAssetHint != MulAssetHint && MulAssetHint) {//TOTO:xiangliang.wang
            ALOGI("[%s %d]MulAssetHint event send\n",__FUNCTION__,__LINE__);
            notifyListener(MEDIA_INFO, MEDIA_INFO_AMLOGIC_SHOW_DTS_MULASSETHINT,0,NULL);
            DtsHdMulAssetHint=MulAssetHint;
        }

        if (HPS_hint && DtsHdHpsHint == 0) {
            notifyListener(MEDIA_INFO,MEDIA_INFO_AMLOGIC_SHOW_DTS_HPS_NOTSUPPORT,0,NULL);
            DtsHdHpsHint=1;
        }
    }
    return renderer->getCurrentPosition(mediaUs);
}

void AmNuPlayer::getStats(int64_t *numFramesTotal, int64_t *numFramesDropped) {
    sp<DecoderBase> decoder = getDecoder(false /* audio */);
    if (decoder != NULL) {
        decoder->getStats(numFramesTotal, numFramesDropped);
    } else {
        *numFramesTotal = 0;
        *numFramesDropped = 0;
    }
}

sp<MetaData> AmNuPlayer::getFileMeta() {
    return mSource->getFileFormatMeta();
}

void AmNuPlayer::schedulePollDuration() {
    sp<AMessage> msg = new AMessage(kWhatPollDuration, this);
    msg->setInt32("generation", mPollDurationGeneration);
    msg->post();
}

void AmNuPlayer::cancelPollDuration() {
    ++mPollDurationGeneration;
}

void AmNuPlayer::processDeferredActions() {
    while (!mDeferredActions.empty()) {
        // We won't execute any deferred actions until we're no longer in
        // an intermediate state, i.e. one more more decoders are currently
        // flushing or shutting down.

        if (mFlushingAudio != NONE || mFlushingVideo != NONE) {
            // We're currently flushing, postpone the reset until that's
            // completed.

            ALOGV("postponing action mFlushingAudio=%d, mFlushingVideo=%d",
                  mFlushingAudio, mFlushingVideo);

            break;
        }

        sp<Action> action = *mDeferredActions.begin();
        mDeferredActions.erase(mDeferredActions.begin());

        action->execute(this);
    }
}

void AmNuPlayer::performSeek(int64_t seekTimeUs, bool needNotify) {
    ALOGV("performSeek seekTimeUs=%lld us (%.2f secs), needNotify(%d)",
          seekTimeUs,
          seekTimeUs / 1E6,
          needNotify);

    if (mSource == NULL) {
        // This happens when reset occurs right before the loop mode
        // asynchronously seeks to the start of the stream.
        LOG_ALWAYS_FATAL_IF(mAudioDecoder != NULL || mVideoDecoder != NULL,
                "mSource is NULL and decoders not NULL audio(%p) video(%p)",
                mAudioDecoder.get(), mVideoDecoder.get());
        return;
    }

    thread_interrupt();
    mSource->seekTo(seekTimeUs);
    thread_uninterrupt();

    ++mTimedTextGeneration;

    // everything's flushed, continue playback.
}

void AmNuPlayer::performDecoderFlush(FlushCommand audio, FlushCommand video) {
    ALOGI("performDecoderFlush audio=%d, video=%d", audio, video);

    if ((audio == FLUSH_CMD_NONE || mAudioDecoder == NULL)
            && (video == FLUSH_CMD_NONE || mVideoDecoder == NULL)) {
        return;
    }

    if (audio != FLUSH_CMD_NONE && mAudioDecoder != NULL) {
        flushDecoder(true /* audio */, (audio == FLUSH_CMD_SHUTDOWN));
    }

    if (video != FLUSH_CMD_NONE && mVideoDecoder != NULL) {
        flushDecoder(false /* audio */, (video == FLUSH_CMD_SHUTDOWN));
    }
    ALOGI("performDecoderFlush end");
}

void AmNuPlayer::performReset() {
    ALOGI("performReset");

    CHECK(mAudioDecoder == NULL);
    CHECK(mVideoDecoder == NULL);

    cancelPollDuration();

    ++mScanSourcesGeneration;
    mScanSourcesPending = false;

    if (mRendererLooper != NULL) {
        if (mRenderer != NULL) {
            //mRendererLooper->unregisterHandler(mRenderer->this);
        }
        mRendererLooper->stop();
        mRendererLooper.clear();
    }
    mRenderer.clear();
    ++mRendererGeneration;

    if (mSource != NULL) {
        thread_interrupt();
        mSource->stop();

        mSource.clear();
        thread_uninterrupt();
    }

    if (mDriver != NULL) {
        sp<AmNuPlayerDriver> driver = mDriver.promote();
        if (driver != NULL) {
            driver->notifyResetComplete();
        }
    }

    mStarted = false;
}

void AmNuPlayer::performScanSources() {
    ALOGV("performScanSources");

    if (!mStarted) {
        return;
    }

    if (mAudioDecoder == NULL || mVideoDecoder == NULL) {
        postScanSources();
    }
}

void AmNuPlayer::performSetSurface(const sp<NativeWindowWrapper> &wrapper) {
    ALOGV("performSetSurface");

    mNativeWindow = wrapper;

    // XXX - ignore error from setVideoScalingMode for now
    setVideoScalingMode(mVideoScalingMode);

    if (mDriver != NULL) {
        sp<AmNuPlayerDriver> driver = mDriver.promote();
        if (driver != NULL) {
            driver->notifySetSurfaceComplete();
        }
    }
}

void AmNuPlayer::performResumeDecoders(bool needNotify) {
    if (needNotify) {
        mResumePending = true;
        if (mVideoDecoder == NULL) {
            // if audio-only, we can notify seek complete now,
            // as the resume operation will be relatively fast.
            finishResume();
        }
    }

    if (mVideoDecoder != NULL) {
        // When there is continuous seek, MediaPlayer will cache the seek
        // position, and send down new seek request when previous seek is
        // complete. Let's wait for at least one video output frame before
        // notifying seek complete, so that the video thumbnail gets updated
        // when seekbar is dragged.
        mVideoDecoder->signalResume(needNotify);
    }

    if (mAudioDecoder != NULL) {
        mAudioDecoder->signalResume(false /* needNotify */);
    }
}

void AmNuPlayer::finishResume() {
    if (mResumePending) {
        mResumePending = false;
        if (mDriver != NULL) {
            sp<AmNuPlayerDriver> driver = mDriver.promote();
            if (driver != NULL) {
                driver->notifySeekComplete();
            }
        }
    }
}

void AmNuPlayer::onSourceNotify(const sp<AMessage> &msg) {
    int32_t what;
    CHECK(msg->findInt32("what", &what));

    switch (what) {
        case Source::kWhatInstantiateSecureDecoders:
        {
            if (mSource == NULL) {
                // This is a stale notification from a source that was
                // asynchronously preparing when the client called reset().
                // We handled the reset, the source is gone.
                break;
            }

            sp<AMessage> reply;
            CHECK(msg->findMessage("reply", &reply));
            status_t err = onInstantiateSecureDecoders();
            reply->setInt32("err", err);
            reply->post();
            break;
        }

        case Source::kWhatPrepared:
        {
            if (mSource == NULL) {
                // This is a stale notification from a source that was
                // asynchronously preparing when the client called reset().
                // We handled the reset, the source is gone.
                break;
            }

            int32_t err;
            CHECK(msg->findInt32("err", &err));

            if (err != OK) {
                // shut down potential secure codecs in case client never calls reset
                mDeferredActions.push_back(
                        new FlushDecoderAction(FLUSH_CMD_SHUTDOWN /* audio */,
                                               FLUSH_CMD_SHUTDOWN /* video */));
                processDeferredActions();
            }

            sp<AmNuPlayerDriver> driver = mDriver.promote();
            if (driver != NULL) {
                // notify duration first, so that it's definitely set when
                // the app received the "prepare complete" callback.
                int64_t durationUs;
                if (mSource != NULL && mSource->getDuration(&durationUs) == OK) {
                    driver->notifyDuration(durationUs);
                }
                driver->notifyPrepareCompleted(err);
            }

            break;
        }

        case Source::kWhatFlagsChanged:
        {
            uint32_t flags;
            CHECK(msg->findInt32("flags", (int32_t *)&flags));

            sp<AmNuPlayerDriver> driver = mDriver.promote();
            if (driver != NULL) {
                if ((flags & AmNuPlayer::Source::FLAG_CAN_SEEK) == 0) {
                    driver->notifyListener(
                            MEDIA_INFO, MEDIA_INFO_NOT_SEEKABLE, 0);
                }
                driver->notifyFlagsChanged(flags);
            }

            if ((mSourceFlags & Source::FLAG_DYNAMIC_DURATION)
                    && (!(flags & Source::FLAG_DYNAMIC_DURATION))) {
                cancelPollDuration();
            } else if (!(mSourceFlags & Source::FLAG_DYNAMIC_DURATION)
                    && (flags & Source::FLAG_DYNAMIC_DURATION)
                    && (mAudioDecoder != NULL || mVideoDecoder != NULL)) {
                schedulePollDuration();
            }

            mSourceFlags = flags;
            break;
        }

        case Source::kWhatVideoSizeChanged:
        {
            sp<AMessage> format;
            CHECK(msg->findMessage("format", &format));

            updateVideoSize(format);
            break;
        }

        case Source::kWhatBufferingUpdate:
        {
            int32_t percentage;
            CHECK(msg->findInt32("percentage", &percentage));

            notifyListener(MEDIA_BUFFERING_UPDATE, percentage, 0);
            break;
        }

        case Source::kWhatPauseOnBufferingStart:
        {
            // ignore if not playing
            if (mStarted && !mPausedByClient) {
                ALOGI("buffer low, pausing...");

                onPause();
            }
            // fall-thru
        }

        case Source::kWhatBufferingStart:
        {
            notifyListener(MEDIA_INFO, MEDIA_INFO_BUFFERING_START, 0);
            break;
        }

        case Source::kWhatResumeOnBufferingEnd:
        {
            // ignore if not playing
            if (mStarted && !mPausedByClient) {
                ALOGI("buffer ready, resuming...");

                onResume();
            }
            // fall-thru
        }

        case Source::kWhatBufferingEnd:
        {
            notifyListener(MEDIA_INFO, MEDIA_INFO_BUFFERING_END, 0);
            break;
        }

        case Source::kWhatCacheStats:
        {
            int32_t kbps;
            CHECK(msg->findInt32("bandwidth", &kbps));

            notifyListener(MEDIA_INFO, MEDIA_INFO_NETWORK_BANDWIDTH, kbps);
            break;
        }

        case Source::kWhatSubtitleData:
        {
            sp<ABuffer> buffer;
            CHECK(msg->findBuffer("buffer", &buffer));

            sendSubtitleData(buffer, 0 /* baseIndex */);
            break;
        }

        case Source::kWhatTimedMetaData:
        {
            sp<ABuffer> buffer;
            if (!msg->findBuffer("buffer", &buffer)) {
                ALOGI("[timed_id3] update metadata info!");
                notifyListener(MEDIA_INFO, MEDIA_INFO_METADATA_UPDATE, 0);
            } else {
                sendTimedMetaData(buffer);
            }
            break;
        }

        case Source::kWhatTimedTextData:
        {
            int32_t generation;
            if (msg->findInt32("generation", &generation)
                    && generation != mTimedTextGeneration) {
                break;
            }

            sp<ABuffer> buffer;
            CHECK(msg->findBuffer("buffer", &buffer));

            sp<AmNuPlayerDriver> driver = mDriver.promote();
            if (driver == NULL) {
                break;
            }

            int posMs;
            int64_t timeUs, posUs;
            driver->getCurrentPosition(&posMs);
            posUs = posMs * 1000;
            CHECK(buffer->meta()->findInt64("timeUs", &timeUs));

            if (posUs < timeUs) {
                if (!msg->findInt32("generation", &generation)) {
                    msg->setInt32("generation", mTimedTextGeneration);
                }
                msg->post(timeUs - posUs);
            } else {
                sendTimedTextData(buffer);
            }
            break;
        }

        case Source::kWhatQueueDecoderShutdown:
        {
            int32_t audio, video;
            CHECK(msg->findInt32("audio", &audio));
            CHECK(msg->findInt32("video", &video));

            sp<AMessage> reply;
            CHECK(msg->findMessage("reply", &reply));

            queueDecoderShutdown(audio, video, reply);
            break;
        }

        case Source::kWhatDrmNoLicense:
        {
            notifyListener(MEDIA_ERROR, MEDIA_ERROR_UNKNOWN, ERROR_DRM_NO_LICENSE);
            break;
        }

        case Source::kWhatSourceReady:
        {
            int32_t err;
            CHECK(msg->findInt32("err", &err));
            notifyListener(0xffff, err, 0);
            break;
        }

        case Source::kWhatFrameRate:
        {
            CHECK(msg->findFloat("frame-rate", &mFrameRate));
            break;
        }

        default:
            TRESPASS();
    }
}

void AmNuPlayer::onClosedCaptionNotify(const sp<AMessage> &msg) {
    int32_t what;
    CHECK(msg->findInt32("what", &what));

    switch (what) {
        case AmNuPlayer::CCDecoder::kWhatClosedCaptionData:
        {
            sp<ABuffer> buffer;
            CHECK(msg->findBuffer("buffer", &buffer));

            size_t inbandTracks = 0;
            if (mSource != NULL) {
                inbandTracks = mSource->getTrackCount();
            }

            sendSubtitleData(buffer, inbandTracks);
            break;
        }

        case AmNuPlayer::CCDecoder::kWhatTrackAdded:
        {
            notifyListener(MEDIA_INFO, MEDIA_INFO_METADATA_UPDATE, 0);

            break;
        }

        default:
            TRESPASS();
    }


}

void AmNuPlayer::sendSubtitleData(const sp<ABuffer> &buffer, int32_t baseIndex) {
    int32_t trackIndex;
    int64_t timeUs, durationUs;
    CHECK(buffer->meta()->findInt32("trackIndex", &trackIndex));
    CHECK(buffer->meta()->findInt64("timeUs", &timeUs));
    CHECK(buffer->meta()->findInt64("durationUs", &durationUs));

    Parcel in;
    in.writeInt32(trackIndex + baseIndex);
    in.writeInt64(timeUs);
    in.writeInt64(durationUs);
    in.writeInt32(buffer->size());
    in.writeInt32(buffer->size());
    in.write(buffer->data(), buffer->size());

    notifyListener(MEDIA_SUBTITLE_DATA, 0, 0, &in);
}

void AmNuPlayer::sendTimedMetaData(const sp<ABuffer> &buffer) {
    int64_t timeUs;
    CHECK(buffer->meta()->findInt64("timeUs", &timeUs));

    Parcel in;
    in.writeInt64(timeUs);
    in.writeInt32(buffer->size());
    in.writeInt32(buffer->size());
    in.write(buffer->data(), buffer->size());

    notifyListener(MEDIA_META_DATA, 0, 0, &in);
}

void AmNuPlayer::sendTimedTextData(const sp<ABuffer> &buffer) {
    const void *data;
    size_t size = 0;
    int64_t timeUs;
    int32_t flag = TextDescriptions::LOCAL_DESCRIPTIONS;

    AString mime;
    CHECK(buffer->meta()->findString("mime", &mime));
    CHECK(strcasecmp(mime.c_str(), MEDIA_MIMETYPE_TEXT_3GPP) == 0);

    data = buffer->data();
    size = buffer->size();

    Parcel parcel;
    if (size > 0) {
        CHECK(buffer->meta()->findInt64("timeUs", &timeUs));
        flag |= TextDescriptions::IN_BAND_TEXT_3GPP;
        //TextDescriptions::getParcelOfDescriptions(
        //        (const uint8_t *)data, size, flag, timeUs / 1000, &parcel);
    }

    if ((parcel.dataSize() > 0)) {
        notifyListener(MEDIA_TIMED_TEXT, 0, 0, &parcel);
    } else {  // send an empty timed text
        notifyListener(MEDIA_TIMED_TEXT, 0, 0);
    }
}
////////////////////////////////////////////////////////////////////////////////

sp<AMessage> AmNuPlayer::Source::getFormat(bool audio) {
    sp<MetaData> meta = getFormatMeta(audio);

    if (meta == NULL) {
        return NULL;
    }

    sp<AMessage> msg = new AMessage;

    if (convertMetaDataToMessage(meta, &msg) == OK) {
        return msg;
    }
    return NULL;
}

void AmNuPlayer::Source::notifyFlagsChanged(uint32_t flags) {
    sp<AMessage> notify = dupNotify();
    notify->setInt32("what", kWhatFlagsChanged);
    notify->setInt32("flags", flags);
    notify->post();
}

void AmNuPlayer::Source::notifyVideoSizeChanged(const sp<AMessage> &format) {
    sp<AMessage> notify = dupNotify();
    notify->setInt32("what", kWhatVideoSizeChanged);
    notify->setMessage("format", format);
    notify->post();
}

void AmNuPlayer::Source::notifyPrepared(status_t err) {
    sp<AMessage> notify = dupNotify();
    notify->setInt32("what", kWhatPrepared);
    notify->setInt32("err", err);
    notify->post();
}

void AmNuPlayer::Source::notifyInstantiateSecureDecoders(const sp<AMessage> &reply) {
    sp<AMessage> notify = dupNotify();
    notify->setInt32("what", kWhatInstantiateSecureDecoders);
    notify->setMessage("reply", reply);
    notify->post();
}

void AmNuPlayer::Source::onMessageReceived(const sp<AMessage> & /* msg */) {
    TRESPASS();
}

}  // namespace android
