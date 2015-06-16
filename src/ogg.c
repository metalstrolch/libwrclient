#include "internal.h"

#ifdef WRC_OGG

static const int oggBufSize = 4096;

static void shutdownOGG(WRC_Stream* ctx)
{
	enum WRC__OGG_DECODE_STATE state = ctx->ogg.state;
	if(state == WRC_OGGDEC_PREINIT)
	{
		// ogg support wasn't initialized, nothing to do here
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

		if(ogg_stream_packetout(os, op) != 1 || vorbis_synthesis_headerin(vi, vc, op) < 0)
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
				ctx->ogg.maxBufSamplesPerChan = oggBufSize/vi->channels;

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
		ogg_int16_t decBuf[oggBufSize];

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
