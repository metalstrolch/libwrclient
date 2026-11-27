/*
 * libwrclient - webradio client library
 *
 * Copyright (C) 2015-2017 Masterbrain Bytes GmbH & Co. KG
 *
 * Released under MIT license, see LICENSE.txt
 */

// ogg/vorbis decoder, using libogg+libvorbis

#include "internal.h"

#ifdef WRC_OGG

static void shutdownOGG(WRC_Stream* ctx)
{
	enum WRC__OGG_DECODE_STATE state = ctx->ogg.state;
	if(state == WRC_OGGDEC_PREINIT)
	{
		// ogg support wasn't initialized, nothing to do here
		return;
	}

	if(ctx->ogg.opusDec != NULL)
	{
		opus_decoder_destroy(ctx->ogg.opusDec);
		ctx->ogg.opusDec = NULL;
		ogg_stream_clear(&ctx->ogg.os);
		ogg_sync_clear(&ctx->ogg.oy);
		memset(&ctx->ogg, 0, sizeof(struct WRC__oggVorbisContext));
		return;
	}

	if(state == WRC_OGGDEC_STREAMDEC)
	{
		ogg_stream_clear(&ctx->ogg.os);
	}
	if(state > WRC_OGGDEC_COMMENT)
	{
		vorbis_comment_clear(&ctx->ogg.vc);
	}
	if(state > WRC_OGGDEC_SETUP)
	{
		vorbis_info_clear(&ctx->ogg.vi);
	}
	if(state > WRC_OGGDEC_PREINIT)
	{
		ogg_sync_clear(&ctx->ogg.oy);
	}

	memset(&ctx->ogg, 0, sizeof(struct WRC__oggVorbisContext));
}

static bool initOGG(WRC_Stream* ctx)
{
	ogg_sync_init(&ctx->ogg.oy);

	ctx->ogg.state = WRC_OGGDEC_VORBISINFO;

	ctx->shutdown = shutdownOGG;

	// these must be set by WRC__decodeOGG()
	ctx->sampleRate = 0;
	ctx->numChannels = 0;

	return true;
}

static void sendCurrentTitleToUser(WRC_Stream* ctx, const char* artist, const char* title)
{
	if(ctx->currentTitleCB != NULL)
	{
		if(artist == NULL)
		{
			if(title == NULL)
			{
				ctx->currentTitleCB(ctx->userdata, "???");
			}
			else
			{
				ctx->currentTitleCB(ctx->userdata, title);
			}
		}
		else if(title == NULL)
		{
			title = "Unknown Title";
			ctx->currentTitleCB(ctx->userdata, artist);
		}
		else
		{
			char buf[512];
			WRC__snprintf(buf, sizeof(buf), "%s - %s", artist, title);
			ctx->currentTitleCB(ctx->userdata, buf);
		}
	}
}


static uint32_t readLE32(const unsigned char* p)
{
	return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24);
}

// looks up "NAME=value" in an OpusTags packet; returns malloc'd value or NULL
static char* opusTagsQuery(const ogg_packet* op, const char* name)
{
	const unsigned char* d = op->packet;
	size_t size = op->bytes;
	size_t nameLen = strlen(name);

	if(size < 12) return NULL;
	size_t pos = 8;
	uint32_t vendorLen = readLE32(d + pos);
	pos += 4;
	if(vendorLen > size - pos) return NULL;
	pos += vendorLen;
	if(size - pos < 4) return NULL;
	uint32_t count = readLE32(d + pos);
	pos += 4;

	for(uint32_t i = 0; i < count && size - pos >= 4; ++i)
	{
		uint32_t len = readLE32(d + pos);
		pos += 4;
		if(len > size - pos) return NULL;

		if(len > nameLen && d[pos + nameLen] == '=' &&
			strncasecmp((const char*)d + pos, name, nameLen) == 0)
		{
			size_t valLen = len - nameLen - 1;
			char* res = malloc(valLen + 1);
			if(res != NULL)
			{
				memcpy(res, d + pos + nameLen + 1, valLen);
				res[valLen] = 0;
			}
			return res;
		}
		pos += len;
	}
	return NULL;
}

static bool isOpusHead(const ogg_packet* op)
{
	return op->bytes >= 19 && memcmp(op->packet, "OpusHead", 8) == 0;
}

// resets the opus state at end of a logical stream
static void endOpusStream(WRC_Stream* ctx)
{
	opus_decoder_destroy(ctx->ogg.opusDec);
	ctx->ogg.opusDec = NULL;
	ogg_stream_clear(&ctx->ogg.os);
	ctx->ogg.state = WRC_OGGDEC_VORBISINFO;
}

// handles the OpusHead packet (first packet of the stream)
static bool initOpusStream(WRC_Stream* ctx, const ogg_packet* op)
{
	const unsigned char* d = op->packet;
	int channels = d[9];

	if((d[8] >> 4) != 0 || d[18] != 0 || channels < 1 || channels > 2)
	{
		WRC__errorReset(ctx, WRC_ERR_UNSUPPORTED_FORMAT, "Unsupported Opus stream (only version 0, mono/stereo supported)");
		return false;
	}

	int err = 0;
	ctx->ogg.opusDec = opus_decoder_create(48000, channels, &err);
	if(ctx->ogg.opusDec == NULL)
	{
		WRC__errorReset(ctx, WRC_ERR_CORRUPT_STREAM, "opus_decoder_create() failed: %s", opus_strerror(err));
		return false;
	}

	ctx->ogg.opusPreSkip = d[10] | (d[11] << 8);

	// output gain is a signed Q7.8 value in dB
	int gain = (int16_t)(d[16] | (d[17] << 8));
	opus_decoder_ctl(ctx->ogg.opusDec, OPUS_SET_GAIN(gain));

	if(ctx->sampleRate != 48000 || ctx->numChannels != channels)
	{
		ctx->sampleRate = 48000;
		ctx->numChannels = channels;

		if(!ctx->initAudioCB(ctx->userdata, ctx->sampleRate, ctx->numChannels))
		{
			WRC__errorReset(ctx, WRC_ERR_INIT_AUDIO_FAILED,
					"calling initAudioCB(userdata, %d, %d) failed - samplerate/numchannels not supported?!",
					ctx->sampleRate, ctx->numChannels);
			return false;
		}
	}

	ctx->ogg.state = WRC_OGGDEC_OPUSTAGS;
	return true;
}

// decodes all pages available in the sync state while in an opus stream
static bool decodeOpus(WRC_Stream* ctx)
{
	ogg_sync_state*   oy = &ctx->ogg.oy;
	ogg_page*         og = &ctx->ogg.og;
	ogg_stream_state* os = &ctx->ogg.os;
	ogg_packet*       op = &ctx->ogg.op;

	// max opus frame is 120ms = 5760 samples per channel at 48kHz
	opus_int16 decBuf[5760 * 2];

	for(;;)
	{
		int res = ogg_sync_pageout(oy, og);
		if(res == 0) return true; // need more data
		if(res < 0) continue;     // corrupt/missing data, try next page

		ogg_stream_pagein(os, og);

		while((res = ogg_stream_packetout(os, op)) != 0)
		{
			if(res < 0) continue;

			if(ctx->ogg.state == WRC_OGGDEC_OPUSTAGS)
			{
				if(op->bytes < 8 || memcmp(op->packet, "OpusTags", 8) != 0)
				{
					WRC__errorReset(ctx, WRC_ERR_CORRUPT_STREAM, "Corrupt OpusTags header!");
					return false;
				}
				char* artist = opusTagsQuery(op, "ARTIST");
				char* title = opusTagsQuery(op, "TITLE");
				sendCurrentTitleToUser(ctx, artist, title);
				free(artist);
				free(title);

				ctx->ogg.state = WRC_OGGDEC_STREAMDEC;
				continue;
			}

			int numChannels = ctx->numChannels;
			int frames = opus_decode(ctx->ogg.opusDec, op->packet, op->bytes, decBuf, 5760, 0);
			if(frames < 0)
			{
				continue; // skip undecodable packet
			}

			int start = WRC__min(frames, ctx->ogg.opusPreSkip);
			ctx->ogg.opusPreSkip -= start;

			opus_int16* out = decBuf + start * numChannels;
			int remaining = (frames - start) * numChannels;

			while(remaining > 0 && ctx->playbackCB != NULL)
			{
				int n = WRC__min(remaining, WRC__decBufSize - WRC__decBufSize % numChannels);
				ctx->playbackCB(ctx->userdata, out, n);
				out += n;
				remaining -= n;
			}
		}

		if(ogg_page_eos(og))
		{
			endOpusStream(ctx);
			return true;
		}
	}
}

bool WRC__decodeOGG(WRC_Stream* ctx, void* data, size_t size)
{
	if(size == 0) return true;

	if(ctx->ogg.state == WRC_OGGDEC_PREINIT)
	{
		initOGG(ctx);
	}

	// these variable names are not very descriptive..
	// but at least consistent with documentation (API docs + examples)
	ogg_sync_state*   oy = &ctx->ogg.oy;
	ogg_page*         og = &ctx->ogg.og;
	ogg_stream_state* os = &ctx->ogg.os;
	ogg_packet*       op = &ctx->ogg.op;

	vorbis_info*      vi = &ctx->ogg.vi;
	vorbis_comment*   vc = &ctx->ogg.vc;
	vorbis_dsp_state* vd = &ctx->ogg.vd;
	vorbis_block*     vb = &ctx->ogg.vb;


	// whatever OGG_DECODE_STATE we're in, first add the current data to the internal buffer
	char* buffer = ogg_sync_buffer(oy, size);
	memcpy(buffer, data, size);
	ogg_sync_wrote(oy, size);

	if(ctx->ogg.opusDec != NULL)
	{
		if(!decodeOpus(ctx)) return false;
		// stream ended => a new one may already be in the buffer
		if(ctx->ogg.opusDec != NULL) return true;
	}

	// decode the first ogg-vorbis header: "vorbis stream initial header"
	// from the first page that's transmitted
	if(ctx->ogg.state == WRC_OGGDEC_VORBISINFO)
	{
		// get the first page, should contain the "vorbis stream initial header"
		if(ogg_sync_pageout(oy, og) != 1)
		{
			// data for page missing, try again with more data later
			return true;
		}

		// set up a logical stream with the serialno
		ogg_stream_init(os, ogg_page_serialno(og));

		vorbis_info_init(vi);
		vorbis_comment_init(vc);

		if(ogg_stream_pagein(os, og) < 0)
		{
			WRC__errorReset(ctx, WRC_ERR_UNSUPPORTED_FORMAT, "Error while reading first OGG page (probably not ogg/vorbis)");
			return false;
		}

		int firstRes = ogg_stream_packetout(os, op);
		if(firstRes == 1 && isOpusHead(op))
		{
			vorbis_info_clear(vi);
			vorbis_comment_clear(vc);
			if(!initOpusStream(ctx, op)) return false;
			return decodeOpus(ctx);
		}

		if(firstRes != 1 || vorbis_synthesis_headerin(vi, vc, op) < 0)
		{
			WRC__errorReset(ctx, WRC_ERR_UNSUPPORTED_FORMAT, "Error while reading first OGG packet or vorbis header (probably not ogg/vorbis)");
			return false;
		}

		// ok, now we're sure it's vorbis and the next packets are
		// the comment- and the setup/codeboock-header

		ctx->ogg.state = WRC_OGGDEC_COMMENT;
	}

	// the code to decode the next two headers - comment and setup/codebook - is identical
	// so I put it in a loop, which basically loops over the pages from ogg_sync_pageout()
	// until we're out of data or both headers are read.
	while(ctx->ogg.state >= WRC_OGGDEC_COMMENT && ctx->ogg.state <= WRC_OGGDEC_SETUP)
	{
		int pageOutRes = ogg_sync_pageout(oy, og);
		if(pageOutRes == 0)
		{
			// data for page missing, try again with more data later
			return true;
		}
		else if(pageOutRes < 0)
		{
			// decoder_example.c jumps back to ogg_sync_pageout() again if this happens,
			// so let's do that, too
			continue;
		}
		// ok, pageOutRes is 1 at this point => was successful

		ogg_stream_pagein(os, og);

		// this loop iterates over packets from the current page of the outer loop
		do {
			int result = ogg_stream_packetout(os, op);
			if(result < 0)
			{
				// data corrupted or missing - this mustn't happen while reading headers!
				WRC__errorReset(ctx, WRC_ERR_CORRUPT_STREAM, "Corrupt Vorbis comment or info header!");
				return false;
			}
			else if(result == 0)
			{
				// get next page (at beginning of outer loop)
				break;
			}

			result = vorbis_synthesis_headerin(vi, vc, op);
			if(result < 0)
			{
				WRC__errorReset(ctx, WRC_ERR_CORRUPT_STREAM, "Corrupt Vorbis comment or info header!");
				return false;
			}

			ctx->ogg.state++; // go to next state

		} while(ctx->ogg.state <= WRC_OGGDEC_SETUP);

		if(ctx->ogg.state > WRC_OGGDEC_SETUP)
		{
			// we've parsed all three headers, so the actual vorbis stream
			// decoder can be initialized
			if(vorbis_synthesis_init(vd, vi) != 0)
			{
				WRC__errorReset(ctx, WRC_ERR_CORRUPT_STREAM, "vorbis_synthesis_init() failed!\n");
				return false;
			}

			const char* artist = vorbis_comment_query(vc, "ARTIST", 0);
			const char* title = vorbis_comment_query(vc, "TITLE", 0);

			sendCurrentTitleToUser(ctx, artist, title);

			if(vi->rate != ctx->sampleRate || vi->channels != ctx->numChannels)
			{
				ctx->sampleRate = vi->rate;
				ctx->numChannels = vi->channels;
				ctx->ogg.maxBufSamplesPerChan = WRC__decBufSize/vi->channels;

				// re-initialize audio backend for new sampleRate/numChannels
				if(!ctx->initAudioCB(ctx->userdata, ctx->sampleRate, ctx->numChannels))
				{
					WRC__errorReset(ctx, WRC_ERR_INIT_AUDIO_FAILED,
							"calling initAudioCB(userdata, %d, %d) failed - samplerate/numchannels not supported?!",
							ctx->sampleRate, ctx->numChannels);

					return false;
				}
			}

			vorbis_block_init(vd, vb);
		}
	}

	if(ctx->ogg.state == WRC_OGGDEC_STREAMDEC)
	{
		bool eos = false;
		ogg_int16_t decBuf[WRC__decBufSize];

		// this loop iterates the pages in the current buffer/ogg_sync_state (oy)
		while(!eos)
		{
			int res = ogg_sync_pageout(oy, og);

			if(res == 0)
			{
				return true; // more data is needed for the page, try again later
			}

			if(res < 0)
			{
				// missing or corrupt data at this page position.. try next page
				continue;
			}

			ogg_stream_pagein(os, og);

			// this loop iterates the packets of the current page
			for(;;)
			{
				res = ogg_stream_packetout(os, op);

				if(res == 0)
				{
					// no more packets => break into outer loop to get next page
					break;
				}
				else if(res < 0)
				{
					// we're out of sync, calling ogg_stream_packetout() again
					// should fix it - so just continue
					continue;
				}

				// we have a proper packet..

				if(vorbis_synthesis(vb, op) == 0)
				{
					vorbis_synthesis_blockin(vd, vb);
				}

				// pcm contains one array per channel (=> vi->channels),
				// so pcm[0] is the first channel, pcm[1] the second etc
				// the samples are float values between -1.0f and 1.0f
				float** pcm;
				// the number of samples per channel in pcm
				int samples;

				// this loop iterates the samples in a packet
				while( (samples = vorbis_synthesis_pcmout(vd, &pcm)) > 0 )
				{
					int numOutSamples = WRC__min(samples, ctx->ogg.maxBufSamplesPerChan);
					int numChannels = ctx->numChannels;

					// convert floats to 16bit signed ints and interleave
					for(int chanIdx=0; chanIdx < numChannels; ++chanIdx)
					{
						float* curChan = pcm[chanIdx];
						ogg_int16_t* curOutSample = decBuf + chanIdx;

						for(int sampleIdx=0; sampleIdx < numOutSamples; ++sampleIdx)
						{
							//int sample = floorf(curChan[sampleIdx]*32767.0f + 0.5f);
							int sample = (curChan[sampleIdx]*32767.0f + 0.5f);

							// prevent int overflows aka clipping
							if(sample < -32768) // INT16_MIN
							{
								sample = -32768;
							}
							else if(sample > 32767) // INT16_MAX
							{
								sample = 32767;
							}

							*curOutSample = sample;
							curOutSample += numChannels;
						}
					}

					if(ctx->playbackCB != NULL)
					{
						ctx->playbackCB(ctx->userdata, decBuf, numOutSamples*numChannels);
					}

					// inform the vorbis decoder how many samples from last
					// vorbis_synthesis_pcmout() were used
					vorbis_synthesis_read(vd, numOutSamples);

				} // end of samples-loop

			} // end of packetout-loop

			if(ogg_page_eos(og))
			{
				// this stream (probably: music track) is over.
				// a second (next music track) may follow with its own headers
				// (containing artist, title etc) and possibly different
				// samplerate and channel count
				eos=true;
				// so we'll be back at the "receive first ogg header" state.
				ctx->ogg.state = WRC_OGGDEC_VORBISINFO;

				// clear the current stream and metadata, it'll be replaced
				ogg_stream_clear(os);
				vorbis_comment_clear(vc);
				vorbis_info_clear(vi);  // decoder_example.c says, this must be called last
			}

		} // end of pageout-loop
	}

	return true;
}

#endif // WRC_OGG
