/*
 * Copyright (c) 2012-2015 Chun-Ying Huang
 *
 * This file is part of GamingAnywhere (GA).
 *
 * GA is free software; you can redistribute it and/or modify it
 * under the terms of the 3-clause BSD License as published by the
 * Free Software Foundation: http://directory.fsf.org/wiki/License:BSD_3Clause
 *
 * GA is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 *
 * You should have received a copy of the 3-clause BSD License along with GA;
 * if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include <stdio.h>

#include "ga-common.h"
#include "ga-avcodec.h"
#include "rtspconf.h"
#include "asource.h"
#include "ga-hook-common.h"
#include "ga-hook-coreaudio.h"

#define __DEBUG 0
#if __DEBUG
#define __DEBUG_HOOK_COREAUDIO 0
#define __DEBUG_HOOK_GETMIXFORMAT 0
#define __DEBUG_HOOK_GETBUFFER 0
#define __DEBUG_HOOK_RELEASEBUFFER 0
#define __DEBUG_HOOK_RELEASEBUFFER_0 0
#define __DEBUG_TRY_AUDIO_ON_SERVER 0
#endif

#define CA_MAX_SAMPLES	32768

static int ca_bytes_per_sample = 0;
static int ca_samplerate = 0;
static int ga_samplerate = 0;
static int ga_channels = 0;
static struct SwrContext *swrctx = NULL;
static unsigned char *audio_buf = NULL;

static t_GetBuffer		old_GetBuffer = NULL;
static t_ReleaseBuffer		old_ReleaseBuffer = NULL;
static t_GetMixFormat		old_GetMixFormat = NULL;
#if __DEBUG_TRY_AUDIO_ON_SERVER
static struct SwrContext *swrctx_reverse = NULL;
#endif
#if __DEBUG_HOOK_RELEASEBUFFER
static AVSampleFormat __debug_in_sample_fmt;
static AVSampleFormat __debug_out_sample_fmt;
#endif

#define	CA_DO_HOOK(name)	ga_hook_function(#name, old_##name, hook_##name)

static enum AVSampleFormat
CA2SWR_format(WAVEFORMATEX *w) {
	WAVEFORMATEXTENSIBLE *wex = (WAVEFORMATEXTENSIBLE*) w;
	switch(w->wFormatTag) {
	case WAVE_FORMAT_PCM:
pcm:
		if(w->wBitsPerSample == 8)
			return AV_SAMPLE_FMT_U8;
		if(w->wBitsPerSample == 16)
			return AV_SAMPLE_FMT_S16;
		if(w->wBitsPerSample == 32)
			return AV_SAMPLE_FMT_S32;
		break;
	case WAVE_FORMAT_IEEE_FLOAT:
ieee_float:
		if(w->wBitsPerSample == 32)
			return AV_SAMPLE_FMT_FLT;
		if(w->wBitsPerSample == 64)
			return AV_SAMPLE_FMT_DBL;
		break;
	case WAVE_FORMAT_EXTENSIBLE:
		if(wex->SubFormat == KSDATAFORMAT_SUBTYPE_PCM)
			goto pcm;
		if(wex->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT)
			goto ieee_float;
		ga_error("CA2SWR: format %08lX-%04hX-%04hX-%02hhX%02hhX-%02hhX%02hhX%02hhX%02hhX%02hhX%02hhX is not supported.\n",
			wex->SubFormat.Data1,
			wex->SubFormat.Data2,
			wex->SubFormat.Data3,
			wex->SubFormat.Data4[0],
			wex->SubFormat.Data4[1],
			wex->SubFormat.Data4[2],
			wex->SubFormat.Data4[3],
			wex->SubFormat.Data4[4],
			wex->SubFormat.Data4[5],
			wex->SubFormat.Data4[6],
			wex->SubFormat.Data4[7]);
		exit(-1);
		break;
	default:
		ga_error("CA2SWR: format %x is not supported.\n", w->wFormatTag);
		exit(-1);
	}
	return AV_SAMPLE_FMT_NONE;
}

static WORD
format_to_BitsPerSample(enum AVSampleFormat sample_fmt) {
	switch (sample_fmt) {
	case AV_SAMPLE_FMT_U8:
		return 8;
	case AV_SAMPLE_FMT_S16:
		return 16;
	case AV_SAMPLE_FMT_S32:
	case AV_SAMPLE_FMT_FLT:
		return 32;
	case AV_SAMPLE_FMT_DBL:
		return 64;
	default:
		ga_error("format_to_BitsPerSample: format %d is not supported.\n", sample_fmt);
		exit(-1);
	}
	return -1;
}

static int64_t
CA2SWR_chlayout(int channels) {
	if(channels == 1)
		return AV_CH_LAYOUT_MONO;
	if(channels == 2)
		return AV_CH_LAYOUT_STEREO;
	ga_error("CA2SWR: channel layout (%d) is not supported.\n", channels);
	exit(-1);
	return -1;
}

static int
ca_create_swrctx(WAVEFORMATEX *w) {
	struct RTSPConf *rtspconf = rtspconf_global();
	int bufreq, samples;
	//
	if(swrctx != NULL)
		swr_free(&swrctx);
	if(audio_buf != NULL)
		free(audio_buf);
	//
	ga_error("CoreAudio: create swr context - format[%x] freq[%d] channels[%d]\n",
		w->wFormatTag, w->nSamplesPerSec, w->nChannels);
#if __DEBUG_HOOK_GETMIXFORMAT
	if (w->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
		WAVEFORMATEXTENSIBLE *wex = (WAVEFORMATEXTENSIBLE*)w;
		ga_error("[hook_GetMixFormat => ca_create_swrctx]: DEBUG: resample context (source)\n"
			"  wFormatTag: WAVE_FORMAT_EXTENSIBLE\n"
			"  SubFormat:");
		if (wex->SubFormat == KSDATAFORMAT_SUBTYPE_PCM) {
			ga_error(" KSDATAFORMAT_SUBTYPE_PCM\n");
		}
		if (wex->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT) {
			ga_error(" KSDATAFORMAT_SUBTYPE_IEEE_FLOAT\n");
		}
	}
#endif
	//
	swrctx = swr_alloc_set_opts(NULL,
		rtspconf->audio_device_channel_layout,
		rtspconf->audio_device_format,
		rtspconf->audio_samplerate,
		CA2SWR_chlayout(w->nChannels),
		CA2SWR_format(w),
		w->nSamplesPerSec, 0, NULL);
#if __DEBUG_HOOK_RELEASEBUFFER
	__debug_in_sample_fmt = CA2SWR_format(w);
	__debug_out_sample_fmt = rtspconf->audio_device_format;
#endif
#if __DEBUG_TRY_AUDIO_ON_SERVER
	swrctx_reverse = swr_alloc_set_opts(NULL,
		CA2SWR_chlayout(w->nChannels),         CA2SWR_format(w),              w->nSamplesPerSec, 
		rtspconf->audio_device_channel_layout, rtspconf->audio_device_format, rtspconf->audio_samplerate,
		0, NULL);
#endif
	if(swrctx == NULL) {
		ga_error("CoreAudio: cannot create resample context.\n");
		return -1;
	} else {
		ga_error("CoreAudio: resample context (%x,%d,%d) -> (%x,%d,%d)\n",
			(int) CA2SWR_chlayout(w->nChannels),
			(int) CA2SWR_format(w), // AV_SAMPLE_FMT_FLT (3) for torchlight2
			(int) w->nSamplesPerSec,
			(int) rtspconf->audio_device_channel_layout,
			(int) rtspconf->audio_device_format,
			(int) rtspconf->audio_samplerate);
	}
	if(swr_init(swrctx) < 0) {
		swr_free(&swrctx);
		swrctx = NULL;
		ga_error("CoreAudio: resample context init failed.\n");
		return -1;
	}
	// allocate buffer?
	ga_samplerate = rtspconf->audio_samplerate;
	ga_channels = av_get_channel_layout_nb_channels(rtspconf->audio_device_channel_layout);
	ca_samplerate = w->nSamplesPerSec;
	ca_bytes_per_sample = w->wBitsPerSample/8;
	samples = av_rescale_rnd(CA_MAX_SAMPLES,
			rtspconf->audio_samplerate, w->nSamplesPerSec, AV_ROUND_UP);
	bufreq = av_samples_get_buffer_size(NULL,
			rtspconf->audio_channels, samples*2,
			rtspconf->audio_device_format,
			1/*no-alignment*/); // no-alignment => actual size of audio data
	if((audio_buf = (unsigned char *) malloc(bufreq)) == NULL) {
		ga_error("CoreAudio: cannot allocate resample memory.\n");
		return -1;
	}
	if(audio_source_setup(bufreq, rtspconf->audio_samplerate,
				// 16/* depends on format */,
				format_to_BitsPerSample(rtspconf->audio_device_format),
				rtspconf->audio_channels) < 0) {
		ga_error("CoreAudio: audio source setup failed.\n");
		return -1;
	}
#if __DEBUG_HOOK_GETMIXFORMAT
	ga_error("[hook_GetMixFormat => ca_create_swrctx]: DEBUG: BitsPerSample@audio_source_setup: %d\n",
		format_to_BitsPerSample(rtspconf->audio_device_format));
#endif
	ga_error("CoreAudio: max %d samples with %d byte(s) resample buffer allocated.\n",
		samples, bufreq);
	//
	return 0;
}

DllExport HRESULT __stdcall
hook_GetMixFormat( 
		IAudioClient *thiz,
		WAVEFORMATEX **ppDeviceFormat)
{
#if __DEBUG_HOOK_GETMIXFORMAT
	ga_error("[hook_GetMixFormat]: DEBUG: Function called.\n");
#endif
	HRESULT hr;
	hr = old_GetMixFormat(thiz, ppDeviceFormat);
	// init audio here
	if(hr != S_OK)
		return hr;
	if(ca_create_swrctx(*ppDeviceFormat) < 0) {
		ga_error("CoreAudio: GetMixFormat returns an unsupported format\n");
		exit(-1);
		return E_INVALIDARG;
	}
	//
	return hr;
}

// address is set when hook_GetBuffer() is called.
static char *buffer_data = NULL;
static unsigned int buffer_frames = 0;

DllExport HRESULT __stdcall
hook_ReleaseBuffer( 
		IAudioRenderClient *thiz,
		UINT32 NumFramesWritten,
		DWORD dwFlags)
{
#if __DEBUG_HOOK_RELEASEBUFFER_0
	ga_error("[hook_ReleaseBuffer]: DEBUG: Function called.\n");
#endif
	const unsigned char *srcplanes[SWR_CH_MAX];
	unsigned char *dstplanes[SWR_CH_MAX];
	int samples;
	// capture audio here
#if __DEBUG_HOOK_RELEASEBUFFER_0
	if (swrctx == NULL || buffer_data == NULL || audio_buf == NULL) {
		ga_error("[hook_ReleaseBuffer]: DEBUG: No Capture:\n"
			"  &swrctx: %p\n  &buffer_data: %p\n  &audio_buf: %p\n",
			swrctx, buffer_data, audio_buf);
	}
#endif
	if(swrctx == NULL || buffer_data == NULL || audio_buf == NULL)
		goto no_capture;
	srcplanes[0] = (unsigned char*) buffer_data;
	srcplanes[1] = NULL;
	dstplanes[0] = audio_buf;
	dstplanes[1] = NULL;
	samples = av_rescale_rnd(NumFramesWritten,
			ga_samplerate,
			ca_samplerate, AV_ROUND_UP);
#if __DEBUG_HOOK_RELEASEBUFFER
	if (hr = swr_convert(swrctx,
			dstplanes, samples,
			srcplanes, NumFramesWritten) < 0) {
		ga_error("[hook_ReleaseBuffer]: DEBUG: Error: swr_convert: return %d\n", hr);
	}
#else
	swr_convert(swrctx,
			dstplanes, samples,
			srcplanes, NumFramesWritten);
#endif
#if __DEBUG_HOOK_RELEASEBUFFER_0
	do {
		ga_error("[hook_ReleaseBuffer]: DEBUG: swr_convert\n"
			"  samples: %d\n  NumFramesWritten: %d\n"
			"  ga_samplerate: %d\n  ca_samplerate: %d\n"
			"  in_sample_fmt: %d\n  out_sample_fmt: %d\n",
			samples, NumFramesWritten,
			ga_samplerate, ca_samplerate,
			__debug_in_sample_fmt, __debug_out_sample_fmt);
	} while (0);
#endif
	audio_source_buffer_fill(audio_buf, samples);
#if __DEBUG_TRY_AUDIO_ON_SERVER
	do {
		// play true sound successfully.
		const unsigned char *srcplanes_reverse[SWR_CH_MAX];
		unsigned char *dstplanes_reverse[SWR_CH_MAX];

		srcplanes_reverse[0] = audio_buf;
		srcplanes_reverse[1] = NULL;
		dstplanes_reverse[0] = (unsigned char*)buffer_data;
		dstplanes_reverse[1] = NULL;
		int samples_reverse = av_rescale_rnd(samples,
			ca_samplerate,
			ga_samplerate, AV_ROUND_UP);
		swr_convert(swrctx_reverse,
			dstplanes_reverse, samples_reverse,
			srcplanes_reverse, samples);
		//
		// int frames = samples;
		// int framesize = frames\
					  * audio_source_channels()/*ab->channels*/\
					  * audio_source_bitspersample()/*ab->bitspersample*/ / 8;
		// ga_error("[hook_ReleaseBuffer]: DEBUG: not clearing buffer.\n");
		// fill in data to buffer_data here to play on the server.
		// directly copy would fail to play true sound but only noise
		// as audio_buf saved PCM INT 16 but buffer_data was IEEE FLOAT 32
		// bcopy(audio_buf, buffer_data, framesize);
	} while (0);
#else
	bzero(buffer_data, NumFramesWritten * ca_bytes_per_sample);
	dwFlags |= AUDCLNT_BUFFERFLAGS_SILENT;
#endif
	//
no_capture:
	buffer_data = NULL;
	return old_ReleaseBuffer(thiz, NumFramesWritten, dwFlags);
}


DllExport HRESULT __stdcall
hook_GetBuffer( 
		IAudioRenderClient *thiz,
		UINT32 NumFramesRequested,
		BYTE **ppData)
{	
#if __DEBUG_HOOK_GETBUFFER
	ga_error("[hook_GetBuffer]: DEBUG: Function called.\n");
#endif
	HRESULT hr;
	hr = old_GetBuffer(thiz, NumFramesRequested, ppData);
	if(hr == S_OK) {
		buffer_data = (char*) *ppData;
		buffer_frames = NumFramesRequested;
	}
#if __DEBUG_HOOK_GETBUFFER
	else {
		ga_error("[hook_getBuffer]: DEBUG: old_GetBuffer Failed with hr: %ld", hr);
	}
#endif
	return S_OK;
}


int
hook_coreaudio() {
	int ret = -1;

	HRESULT hr;
	IMMDeviceEnumerator *deviceEnumerator = NULL;
	IMMDevice *device = NULL;
	IAudioClient *audioClient = NULL;
	IAudioRenderClient *renderClient = NULL;
	WAVEFORMATEX *pwfx = NULL;

	// obtain core-audio objects and functions
#define	RET_ON_ERROR(hr, prefix) if(hr!=S_OK) { ga_error("[core-audio] %s failed (%08x).\n", prefix, hr); goto hook_ca_quit; }
	hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), NULL, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**) &deviceEnumerator);
	RET_ON_ERROR(hr, "CoCreateInstance");

	hr = deviceEnumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
	RET_ON_ERROR(hr, "GetDefaultAudioEndpoint");

	hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, NULL, (void**) &audioClient);
	RET_ON_ERROR(hr, "Activate");

	hr = audioClient->GetMixFormat(&pwfx);
	RET_ON_ERROR(hr, "GetMixFormat");
#if __DEBUG_HOOK_COREAUDIO
	ga_error("[hook_coreaudio] DEBUG: pwfx:\n"
		"  wFormatTag: %d\n  nChannels: %d\n  nSamplesPerSec: %d\n"
		"  nAvgBytesPerSec: %d\n  nBlockAlign: %d\n  wBitsPerSample: %d\n"
		"  cbSize: %d\n",
		pwfx->wFormatTag, pwfx->nChannels, pwfx->nSamplesPerSec,
		pwfx->nAvgBytesPerSec, pwfx->nBlockAlign, pwfx->wBitsPerSample,
		pwfx->cbSize);
#endif

	hr = audioClient->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, 10000000/*REFTIME_PER_SEC*/, 0, pwfx, NULL);
	RET_ON_ERROR(hr, "Initialize");

	hr = audioClient->GetService(__uuidof(IAudioRenderClient), (void**) &renderClient);
	RET_ON_ERROR(hr, "GetService[IAudioRenderClient]");
#undef	RET_ON_ERROR

	// do hook stuff
	old_GetMixFormat = (t_GetMixFormat) ((comobj_t*) audioClient)->vtbl->func[8];
	CA_DO_HOOK(GetMixFormat);

	old_GetBuffer = (t_GetBuffer) ((comobj_t*) renderClient)->vtbl->func[3];
	// GetBuffer will be called each time *before* source fill in data.
	CA_DO_HOOK(GetBuffer);

	old_ReleaseBuffer = (t_ReleaseBuffer) ((comobj_t*) renderClient)->vtbl->func[4];
	// ReleaseBuffer will be called each time *after* source fill in data.
	CA_DO_HOOK(ReleaseBuffer);

	ret = 0;

	ga_error("hook_coreaudio: done\n");

hook_ca_quit:
	if(renderClient)	{ renderClient->Release();	renderClient = NULL;		}
	if(pwfx)		{ CoTaskMemFree(pwfx);		pwfx= NULL;			}
	if(audioClient)		{ audioClient->Release();	audioClient = NULL;		}
	if(device)		{ device->Release();		device = NULL;			}
	if(deviceEnumerator)	{ deviceEnumerator->Release();	deviceEnumerator = NULL;	}

	return ret;
}

#if __DEBUG
#undef __DEBUG_HOOK_COREAUDIO
#undef __DEBUG_HOOK_GETMIXFORMAT
#undef __DEBUG_HOOK_GETBUFFER
#undef __DEBUG_HOOK_RELEASEBUFFER
#undef __DEBUG_HOOK_RELEASEBUFFER_0
#undef __DEBUG_TRY_AUDIO_ON_SERVER
#endif
#undef __DEBUG
