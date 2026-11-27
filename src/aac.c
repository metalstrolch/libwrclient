/*
 * libwrclient - webradio client library
 *
 * Released under MIT license, see LICENSE.txt
 */

// AAC (ADTS) decoder, using libfaad

#include "internal.h"

#ifdef WRC_AAC

static void shutdownAAC(WRC_Stream* ctx)
{
	if(ctx->aac.dec != NULL)
	{
		NeAACDecClose(ctx->aac.dec);
		ctx->aac.dec = NULL;
	}
	free(ctx->aac.buf);
	ctx->aac.buf = NULL;
	ctx->aac.bufSize = ctx->aac.bufCap = 0;
	ctx->aac.initialized = false;
}

static bool appendInput(struct WRC__aacContext* a, const void* data, size_t size)
{
	if(a->bufSize + size > a->bufCap)
	{
		size_t cap = a->bufSize + size + 4096;
		unsigned char* nb = realloc(a->buf, cap);
		if(nb == NULL)
			return false;
		a->buf = nb;
		a->bufCap = cap;
	}
	memcpy(a->buf + a->bufSize, data, size);
	a->bufSize += size;
	return true;
}

static void consume(struct WRC__aacContext* a, size_t n)
{
	a->bufSize -= n;
	memmove(a->buf, a->buf + n, a->bufSize);
}

// returns length of the ADTS frame at p, or 0 if p is not a valid header
static size_t adtsFrameLen(const unsigned char* p)
{
	if(p[0] != 0xFF || (p[1] & 0xF6) != 0xF0)
		return 0;
	size_t len = ((p[3] & 0x03) << 11) | (p[4] << 3) | (p[5] >> 5);
	return len >= 7 ? len : 0;
}

static bool initAAC(WRC_Stream* ctx)
{
	ctx->aac.dec = NeAACDecOpen();
	ctx->shutdown = shutdownAAC;
	if(ctx->aac.dec == NULL)
	{
		eprintf("NeAACDecOpen() failed!\n");
		return false;
	}

	NeAACDecConfigurationPtr cfg = NeAACDecGetCurrentConfiguration(ctx->aac.dec);
	cfg->outputFormat = FAAD_FMT_16BIT;
	cfg->downMatrix = 1; // at most stereo output
	NeAACDecSetConfiguration(ctx->aac.dec, cfg);
	return true;
}

bool WRC__decodeAAC(WRC_Stream* ctx, void* data, size_t size)
{
	struct WRC__aacContext* a = &ctx->aac;

	if(a->dec == NULL && !initAAC(ctx))
		return false;

	if(size == 0)
		return true;

	if(!appendInput(a, data, size))
	{
		WRC__errorReset(ctx, WRC_ERR_INIT_AUDIO_FAILED, "out of memory");
		return false;
	}

	size_t pos = 0;
	while(a->bufSize - pos >= 7)
	{
		unsigned char* p = a->buf + pos;
		size_t len = adtsFrameLen(p);
		if(len == 0)
		{
			pos++; // resync
			continue;
		}
		if(a->bufSize - pos < len)
			break; // need more data

		if(!a->initialized)
		{
			unsigned long rate;
			unsigned char ch;
			if(NeAACDecInit(a->dec, p, a->bufSize - pos, &rate, &ch) < 0)
			{
				pos += len;
				continue;
			}
			a->initialized = true;
		}

		NeAACDecFrameInfo fi;
		int16_t* pcm = NeAACDecDecode(a->dec, &fi, p, len);
		pos += len;

		if(fi.error != 0 || pcm == NULL)
		{
			eprintf("NeAACDecDecode failed: %s\n", NeAACDecGetErrorMessage(fi.error));
			continue;
		}
		if(fi.samples == 0)
			continue;

		if((int)fi.samplerate != ctx->sampleRate || (int)fi.channels != ctx->numChannels)
		{
			ctx->sampleRate = fi.samplerate;
			ctx->numChannels = fi.channels;
			if(!ctx->initAudioCB(ctx->userdata, ctx->sampleRate, ctx->numChannels))
			{
				WRC__errorReset(ctx, WRC_ERR_INIT_AUDIO_FAILED,
						"calling initAudioCB(userdata, %d, %d) failed - samplerate/numchannels not supported?!",
						ctx->sampleRate, ctx->numChannels);
				return false;
			}
		}

		size_t done = 0;
		while(done < fi.samples)
		{
			size_t n = fi.samples - done;
			if(n > WRC__decBufSize)
				n = WRC__decBufSize;
			ctx->playbackCB(ctx->userdata, pcm + done, n);
			done += n;
		}
	}

	consume(a, pos);
	return true;
}

#endif // WRC_AAC
