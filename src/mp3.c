#include "internal.h"

#ifdef WRC_MP3

static void shutdownMP3(WRC_Stream* ctx)
{
	if(ctx->handle != NULL)
	{
		mpg123_close(ctx->handle);
		ctx->handle = NULL;
	}
}

static bool initMP3(WRC_Stream* ctx)
{
	mpg123_handle* h = mpg123_new(NULL, NULL);
	ctx->handle = h;
	ctx->shutdown = shutdownMP3;

	if(h == NULL)
	{
		eprintf("WTF, mpg123_new() failed!\n");
		return false;
	}

	mpg123_format_none(h);
	if(mpg123_format(h, 44100, MPG123_STEREO, MPG123_ENC_SIGNED_16) != MPG123_OK)
	{
		eprintf("setting ouput format for mpg123 failed!\n");
		return false;
	}

	ctx->numChannels = 2;
	ctx->sampleRate = 44100;

	if(!ctx->initAudioCB(ctx->userdata, ctx->sampleRate, ctx->numChannels))
	{
		WRC__errorReset(ctx, WRC_ERR_INIT_AUDIO_FAILED,
				"calling initAudioCB(userdata, %d, %d) failed - samplerate/numchannels not supported?!",
				ctx->sampleRate, ctx->numChannels);

		return false;
	}

	mpg123_open_feed(h);

	return true;
}

bool WRC__decodeMP3(WRC_Stream* ctx, void* data, size_t size)
{
	unsigned char decBuf[WRC__decBufSize*sizeof(int16_t)];

	if(ctx->handle == NULL && !initMP3(ctx))
	{
		return false;
	}

	if(size == 0)
	{
		return true;
	}

	size_t decSize=0;
	int mRet = mpg123_decode(ctx->handle, data, size, decBuf, sizeof(decBuf), &decSize);
	if(mRet == MPG123_ERR)
	{
		eprintf("mpg123_decode failed: %s\n", mpg123_strerror(ctx->handle)); // TODO: remove
		return true; // TODO: is there any chance the next try will succeed?
	}

	if(decSize != 0)
	{
		ctx->playbackCB(ctx->userdata, (int16_t*)decBuf, decSize/sizeof(int16_t));
	}

	while(mRet != MPG123_ERR && mRet != MPG123_NEED_MORE)
	{
		// get as much decoded audio as available from last feed
		mRet = mpg123_decode(ctx->handle, NULL, 0, decBuf, sizeof(decBuf), &decSize);
		if(decSize != 0)
		{
			ctx->playbackCB(ctx->userdata, (int16_t*)decBuf, decSize/sizeof(int16_t));
		}
	}

	/*
	struct mpg123_frameinfo fi;
	mpg123_info(ctx->handle, &fi);
	printf("# bitrate: %d vbr: %d\n", fi.bitrate, fi.vbr);
	*/
	return true;
}

#endif // WRC_MP3
