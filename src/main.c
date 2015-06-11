
// build with gcc -std=c99 -ggdb -Wall -o wrclient  `/opt/sdl2/bin/sdl2-config --cflags` main.c \
// `/opt/sdl2/bin/sdl2-config --libs` -lmpg123 -lcurl -lm -lvorbis -logg


#include <string.h>
#include <stdio.h>
#include <stdbool.h>
#include <assert.h>

#include <curl/curl.h>
#include <mpg123.h>
#include <vorbis/codec.h>

#include <SDL.h>

#define eprintf(...) fprintf(stderr, __VA_ARGS__)

enum CONTENT_TYPE {
	WRC_CONTENT_UNKNOWN = 0,
	WRC_CONTENT_MP3,
	WRC_CONTENT_OGGVORBIS,
	// TODO: other formats?
};

enum ICY_HEADER_STATE {
	WRC_ICY_UNDECIDED = 0,
	WRC_ICY_HEADER_IN_BODY,
	WRC_ICY_HEADER_DONE
};

enum OGG_DECODE_STATE {
	WRC_OGGDEC_PREINIT = 0,
	WRC_OGGDEC_VORBISINFO,
	WRC_OGGDEC_COMMENT,
	WRC_OGGDEC_SETUP,
	WRC_OGGDEC_STREAMDEC
};

static const int oggBufSize = 4096;

struct WRC__oggVorbisContext
{
	ogg_sync_state   oy; // handles incoming data (bitstream)
	ogg_page         og; // one Ogg page. ogg_packets containing the vorbis data are retrieved from this
	ogg_stream_state os; // tracks decoding pages into packets
	ogg_packet       op; // a single raw packet of data, passed to the (vorbis) codec

	vorbis_info      vi; // contains basic information about the audio, from the vorbis headers
	vorbis_comment   vc; // contains the info from the comment header (artist, title etc)
	vorbis_dsp_state vd; // contains the state of the vorbis (packet to PCM) decoder
	vorbis_block     vb; // local working space for packet->PCM decode

	enum OGG_DECODE_STATE state;
	int maxBufSamplesPerChan;
};

typedef struct _musicStreamContext
{
	CURL* curl;
	struct curl_slist* headers;
	enum CONTENT_TYPE contentType;
	char* contentTypeHeaderVal;
	enum ICY_HEADER_STATE icyHeaderDone;

	char headerBuf[8192];
	int headerBufAfterEndIdx;

	int  icyMetaInt;
	// the metadata sent periodically, cannot be more than this
	// two buffers to swap between them (metadata could be split up into multiple packets)
	char icyMetadata[2][256*16];
	char icyMetadataIdx; // 0 or 1 - the currently valid index, will write to the other one, then swap
	int  icyMetaBytesMissing;
	int  icyMetaBytesWritten;
	int  dataReadSinceLastIcyMeta;

	int sampleRate;
	int numChannels;
	// let's assume format is always 16bit int - but signed or unsigned, little or big endian?

	char* icyName;
	char* icyGenre;
	char* icyURL;
	char* icyDescription;

	mpg123_handle* handle;

	struct WRC__oggVorbisContext ogg;

	SDL_AudioDeviceID sdlAudioDevId;

	// function pointers for the implementation of the current format

	// decode returns true if decoding was successful or it just needs more data
	//    and false, if there was a non-recoverable error
	bool (*decode)(struct _musicStreamContext* ctx, void* data, size_t size);
	void (*shutdown)(struct _musicStreamContext* ctx);

	// function pointers for callbacks to user-code, so the user can play the decoded music
	// and display (changed) metadata etc

	void* userdata; // this will be passed to the user-specified callbacks

	// called whenever fresh samples are available
	void (*playbackCB)(void* userdata, int16_t* samples, size_t numSamples);
	// called on start and whenever samplerate or number of channels change
	bool (*initAudioCB)(void* userdata, int sampleRate, int numChannels);

	// TODO: callback for errors, callback(s?) for metadata

} musicStreamContext;


static void* WRC__memmem(const void* haystack, size_t haystacklen, const void* needle, size_t needlelen)
{
	assert((haystack != NULL || haystacklen == 0)
			&& (needle != NULL || needlelen == 0)
			&& "Don't pass NULL into DG_memmem(), unless the corresponding len is 0!");

	unsigned char* h = (unsigned char*)haystack;
	unsigned char* n = (unsigned char*)needle;

	if(needlelen == 0) return (void*)haystack; // this is what glibc does..
	if(haystacklen < needlelen) return NULL; // also handles haystacklen == 0

	if(needlelen == 1) return (void*)memchr(haystack, n[0], haystacklen);

	// the byte after the last byte needle could start at so it'd still fit into haystack
	unsigned char* afterlast = h + haystacklen - needlelen + 1;
	// haystack length up to afterlast
	size_t hlen_for_needle_start = afterlast - h;
	int n0 = n[0];
	unsigned char* n0candidate = (unsigned char*)memchr(h, n0, hlen_for_needle_start);

	while(n0candidate != NULL)
	{
		if(memcmp(n0candidate+1, n+1, needlelen-1) == 0)
		{
			return (void*)n0candidate;
		}

		++n0candidate; // go on searching one byte after the last n0candidate
		hlen_for_needle_start = afterlast - n0candidate;
		n0candidate = (unsigned char*)memchr(n0candidate, n0, hlen_for_needle_start);
	}

	return NULL; // not found
}

static const char* skipBlanc(const char* str)
{
	while(*str == ' ' || *str == '\t')
	{
		++str;
	}
	return str;
}

// if the line starts with startsWith, returns pointer to the rest of line
//    after startsWith and any spaces or tabs after that
// if it doesn't start with that, returns NULL
static const char* remLineIfStartsWith(const char* line, const char* startsWith)
{
	int prefixLen = strlen(startsWith);
	if(strncasecmp(line, startsWith, prefixLen) == 0)
	{
		return skipBlanc(line + prefixLen);
	}
	return NULL;
}

// copies str to (excluding) strAfterEnd, but skips \0, \r and \n chars at the end
static char* dupStr(const char* str, const char* strAfterEnd)
{
	const char* strEnd = strAfterEnd-1;
	// skip \r and \n at the end of the string
	while(*strEnd == '\r' || *strEnd == '\n') --strEnd;

	size_t len = strEnd+1 - str; // strEnd+1 so strEnd itself is also copied

	char* ret = (char*)malloc(len+1); // +1 for terminating \0
	if(ret != NULL)
	{
		memcpy(ret, str, len);
		ret[len] = '\0';
	}
	return ret;
}

// compare strings until next \r or \n (or \0) in str
static int strCmpToNL(const char* str, const char* cmpStr)
{
	size_t len = strlen(cmpStr);
	int startCmp = strncasecmp(str, cmpStr, len);

	if(startCmp != 0) return startCmp;
	// so the first len chars were equal.. if this is str's end, that's ok.
	char nextC = str[len];
	if(nextC == '\r' || nextC == '\n' || nextC == '\0')
		return 0;
	else
		return 1;
}

static bool playMP3(musicStreamContext* ctx, void* data, size_t size);
static bool playOGG(musicStreamContext* ctx, void* data, size_t size);

static void handleHeaderLine(musicStreamContext* ctx, const char* line, size_t len)
{
	const char* lineAfterEnd = line+len;
	const char* str;
	if((str = remLineIfStartsWith(line, "content-type:")))
	{
		ctx->contentTypeHeaderVal = dupStr(str, lineAfterEnd);
		if(strCmpToNL(str, "audio/mpeg") == 0)
		{
			ctx->contentType = WRC_CONTENT_MP3;
			ctx->decode = playMP3;
		}
		else if(strCmpToNL(str, "application/ogg") == 0)
		{
			ctx->contentType = WRC_CONTENT_OGGVORBIS;
			ctx->decode = playOGG;
		}
		else
		{
			ctx->contentType = WRC_CONTENT_UNKNOWN;
			eprintf("## stream with unknown content type: %s\n", str);
		}
	}
	else if((str = remLineIfStartsWith(line, "icy-name:")))
	{
		ctx->icyName = dupStr(str, lineAfterEnd);
		printf("## icy-name:%s\n", ctx->icyName);
	}
	else if((str = remLineIfStartsWith(line, "icy-genre:")))
	{
		ctx->icyGenre = dupStr(str, lineAfterEnd);
		printf("## icy-genre:%s\n", ctx->icyGenre);
	}
	else if((str = remLineIfStartsWith(line, "icy-url:")))
	{
		ctx->icyURL = dupStr(str, lineAfterEnd);
		printf("## icy-url: \"%s\"\n", ctx->icyURL);
	}
	else if((str = remLineIfStartsWith(line, "icy-description:")))
	{
		ctx->icyDescription = dupStr(str, lineAfterEnd);
		printf("## icy-description: %s\n", ctx->icyDescription);
	}
	else if((str = remLineIfStartsWith(line, "icy-metaint:")))
	{
		ctx->icyMetaInt = atoi(str);
		printf("### metaint: %d\n", ctx->icyMetaInt);
	}
}

static void parseInBodyIcyHeader(musicStreamContext* ctx)
{
	/*
	ICY 200 OK
	icy-notice1:<BR>This stream requires <a href="http://www.winamp.com/">Winamp</a><BR>
	icy-notice2:SHOUTcast Distributed Network Audio Server/Linux v1.9.8<BR>
	icy-name:WackenRadio.com - Official Wacken Radio by RauteMusik.FM
	icy-genre:Metal Rock Alternative
	icy-url:http://www.WackenRadio.com
	content-type:audio/mpeg
	icy-pub:1
	icy-br:192
	*/

	//printf("## header: %s\n", ctx->headerBuf);

	size_t remsize = ctx->headerBufAfterEndIdx;
	const char* headerEnd = ctx->headerBuf + remsize;

	char* line = ctx->headerBuf;
	char* lineEnd = memchr(line, '\r', remsize);

	while(lineEnd && lineEnd <= headerEnd)
	{
		handleHeaderLine(ctx, line, lineEnd - line);

		line = (lineEnd[1] == '\n') ? (lineEnd+2) : (lineEnd+1);
		remsize = headerEnd - line;
		lineEnd = memchr(line, '\r', remsize);
	}
}

static int min(int a, int b) { return a < b ? a : b; }

static void shutdownMP3(musicStreamContext* ctx)
{
	if(ctx->handle != NULL)
	{
		mpg123_close(ctx->handle);
	}
}

static bool initMP3(musicStreamContext* ctx)
{
	mpg123_handle* h = mpg123_new(NULL, NULL);
	ctx->handle = h;

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

	ctx->shutdown = shutdownMP3;

	mpg123_open_feed(h);

	return true;
}

static bool playMP3(musicStreamContext* ctx, void* data, size_t size)
{
	unsigned char decBuf[32768];

	if(ctx->handle == NULL && !initMP3(ctx))
	{
		return false;
	}

	if(size == 0) return true;

	size_t decSize=0;
	int mRet = mpg123_decode(ctx->handle, data, size, decBuf, sizeof(decBuf), &decSize);
	if(mRet == MPG123_ERR)
	{
		eprintf("mpg123_decode failed: %s\n", mpg123_strerror(ctx->handle));
		return true; // TODO: is there any chance the next try will succeed?
	}

	if(decSize != 0 && ctx->playbackCB != NULL)
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

static void shutdownOGG(musicStreamContext* ctx)
{
	enum OGG_DECODE_STATE state = ctx->ogg.state;
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

	ctx->ogg.state = WRC_OGGDEC_PREINIT;
}

static bool initOGG(musicStreamContext* ctx)
{
	ogg_sync_init(&ctx->ogg.oy);

	ctx->ogg.state = WRC_OGGDEC_VORBISINFO;

	ctx->shutdown = shutdownOGG;

	return true;
}

// TODO: a reason enum? (errno-like)
void WRC__errorReset(musicStreamContext* ctx, const char* format, ...)
{
	va_list argptr;
	va_start( argptr, format );
	// TODO: call some error/log callback
	vfprintf(stderr, format, argptr);
	va_end(argptr);

	ctx->shutdown(ctx);

	exit(1); // TODO: do this properly, set some flag in ctx to abort download etc
}

static bool playOGG(musicStreamContext* ctx, void* data, size_t size)
{
	if(size == 0) return true;

	if(ctx->ogg.state == WRC_OGGDEC_PREINIT) initOGG(ctx);

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
			WRC__errorReset(ctx, "Error while reading first OGG page");
			return false;
		}

		if(ogg_stream_packetout(os, op) != 1 || vorbis_synthesis_headerin(vi, vc, op) < 0)
		{
			WRC__errorReset(ctx, "Error while reading first OGG packet or vorbis header.");
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
				WRC__errorReset(ctx, "Corrupt Vorbis comment or info header!");
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
				WRC__errorReset(ctx, "Corrupt Vorbis comment or info header!");
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
				WRC__errorReset(ctx, "vorbis_synthesis_init() failed!\n");
				return false;
			}

			ctx->sampleRate = vi->rate;
			ctx->numChannels = vi->channels;

			ctx->ogg.maxBufSamplesPerChan = oggBufSize/vi->channels;

			// TODO: replace this by something that makes sense
			printf("# ogg vendor: %s\n## ogg comments:\n", (vc->vendor == NULL) ? "<NULL>" : vc->vendor);

			for(int i=0; i<vc->comments; ++i)
			{
				if(vc->comment_lengths[i] > 0)
				{
					printf("### %s\n", vc->user_comments[i]);
				}
			}
			// TODO: save other info from vi and vc somewhere

			// re-initialize audio backend for new sampleRate/numChannels
			ctx->initAudioCB(ctx->userdata, ctx->sampleRate, ctx->numChannels);

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
				eprintf("Corrupt or missing data in bitstream; continuing...\n"); // TODO: I probably don't care much..
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
					int numOutSamples = min(samples, ctx->ogg.maxBufSamplesPerChan);
					int numChannels = vi->channels;

					// convert floats to 16bit signed ints and interleave
					for(int chanIdx=0; chanIdx < numChannels; ++chanIdx)
					{
						float* curChan = pcm[chanIdx];
						ogg_int16_t* curOutSample = decBuf + chanIdx;

						for(int sampleIdx=0; sampleIdx < numOutSamples; ++sampleIdx)
						{
							int sample = floor(curChan[sampleIdx]*32767.0f + 0.5f);

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

static bool playMusic(musicStreamContext* ctx, void* data, size_t size)
{
	if(ctx->decode != NULL)
	{
		return ctx->decode(ctx, data, size);
	}

	return false;
}

static size_t curlWriteFun(void* freshData, size_t size, size_t nmemb, void* context)
{
	const size_t freshDataSize = size*nmemb;
	musicStreamContext* ctx = (musicStreamContext*)context;

	size_t remDataSize = freshDataSize;

	char* remData = freshData;

	if(ctx->icyHeaderDone != WRC_ICY_HEADER_DONE)
	{
		if(ctx->icyHeaderDone == WRC_ICY_UNDECIDED)
		{
			// this should only happen with the very first data received
			if(memcmp(freshData, "ICY 200 OK", strlen("ICY 200 OK")) == 0)
			{
				ctx->icyHeaderDone = WRC_ICY_HEADER_IN_BODY;
			}
			else
			{
				// header is not in body, just play the stream.
				// the metadata from the headers is received via the curlHeaderFun() callback.
				ctx->icyHeaderDone = WRC_ICY_HEADER_DONE;
			}
		}
		if(ctx->icyHeaderDone == WRC_ICY_HEADER_IN_BODY)
		{
			char* headerEnd = WRC__memmem(remData, remDataSize, "\r\n\r\n", 4);
			const int bufSize = sizeof(ctx->headerBuf);
			size_t headerDataSize = remDataSize;

			if(headerEnd != NULL)
			{
				headerEnd += 4; // \r\n\r\n
				headerDataSize = headerEnd - remData;
			}

			// -1 because I wanna leave one byte for \0-termination
			int numBytes = min(headerDataSize, bufSize - ctx->headerBufAfterEndIdx - 1);
			memcpy(ctx->headerBuf+ctx->headerBufAfterEndIdx, remData, numBytes);
			ctx->headerBufAfterEndIdx += numBytes;

			if(headerEnd == NULL)
			{
				return freshDataSize;
			}

			// ok, headerEnd is not NULL, i.e. the whole header has been received.
			ctx->icyHeaderDone = WRC_ICY_HEADER_DONE;
			ctx->headerBuf[ctx->headerBufAfterEndIdx] = '\0';
			parseInBodyIcyHeader(ctx);

			remData = headerEnd; // \r\n\r\n

			remDataSize -= headerDataSize;
		}
	}

	if(ctx->icyMetaInt)
	{
		int dataToNextMeta = ctx->icyMetaInt - ctx->dataReadSinceLastIcyMeta;
		if(ctx->icyMetaBytesMissing > 0)
		{
			char* mdBuf = ctx->icyMetadata[!ctx->icyMetadataIdx];
			int writeMdBytes = min(remDataSize, ctx->icyMetaBytesMissing);
			memcpy(mdBuf + ctx->icyMetaBytesWritten, remData, writeMdBytes);

			remDataSize -= writeMdBytes;
			remData += writeMdBytes;
			ctx->icyMetaBytesMissing -= writeMdBytes;
			ctx->icyMetaBytesWritten += writeMdBytes;
			if(ctx->icyMetaBytesMissing == 0)
			{
				ctx->icyMetadataIdx = !ctx->icyMetadataIdx;
				ctx->dataReadSinceLastIcyMeta = 0;
				ctx->icyMetaBytesWritten = 0;
				printf("## MD update: %s\n", mdBuf);
			}
		}
		else if(dataToNextMeta < remDataSize)
		{
			if(!playMusic(ctx, remData, dataToNextMeta))
			{
				// there was an error, abort downloading stream
				return 0;
			}

			remData += dataToNextMeta;
			remDataSize -= dataToNextMeta;
			int numBytes = ((unsigned char*)remData)[0] * 16;

			// skip the metadata length byte
			++remData;
			--remDataSize;

			if(numBytes == 0)
			{
				// no new metadata this time, start counting again
				ctx->dataReadSinceLastIcyMeta = 0;
			}
			else
			{
				char* mdBuf = ctx->icyMetadata[!ctx->icyMetadataIdx];
				int writeMdBytes = min(remDataSize, numBytes);
				memcpy(mdBuf, remData, writeMdBytes);

				remDataSize -= writeMdBytes;
				remData += writeMdBytes;
				ctx->icyMetaBytesMissing = numBytes - writeMdBytes;
				ctx->icyMetaBytesWritten = writeMdBytes;
				if(ctx->icyMetaBytesMissing == 0)
				{
					ctx->icyMetadataIdx = !ctx->icyMetadataIdx;
					ctx->dataReadSinceLastIcyMeta = 0;
					ctx->icyMetaBytesWritten = 0;
					printf("## MD update2: %s\n", mdBuf);
				}
			}
		}

		ctx->dataReadSinceLastIcyMeta += remDataSize;
	}


	if(!playMusic(ctx, remData, remDataSize))
	{
		// there was an error, abort download
		return 0;
	}


	return freshDataSize;
}

static size_t curlHeaderFun(char *buffer, size_t size, size_t nitems, void *userdata)
{
	/*
	icy-br:128
	ice-audio-info: bitrate=128;samplerate=44100;channels=2
	icy-br:128
	icy-description:Thrash Zone Radio la seule (...)
	icy-genre:thrash
	icy-name:ThrashZoneRadio
	icy-pub:1
	icy-url:http://www.thrash-zone.pcriot.com/
	icy-metaint:16000
	 */

	size_t dataSize = size*nitems;
	musicStreamContext* ctx = (musicStreamContext*)userdata;
	handleHeaderLine(ctx, buffer, dataSize);
	return dataSize;
}

static bool prepareCURL(musicStreamContext* ctx, const char* url)
{
	CURL* curl = curl_easy_init();

	if(curl == NULL)
	{
		eprintf("Initializing cURL failed!\n");
		return false;
	}
#if 0
	FILE* f = fopen("/tmp/test.dat", "w");
	if(f == NULL)
	{
		eprintf("Couldn't open test file\n");
		return false;
	}
#endif // 0

	struct curl_slist* headers = NULL;

	headers = curl_slist_append(headers, "Icy-MetaData:1");

	curl_easy_setopt(curl, CURLOPT_URL, url);
	curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1 );
	curl_easy_setopt(curl, CURLOPT_PROTOCOLS, CURLPROTO_HTTP | CURLPROTO_HTTPS);
	curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS, CURLPROTO_HTTP | CURLPROTO_HTTPS);
	curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1); // follow http 3xx redirects
	curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 10);
	curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curlWriteFun);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, ctx);
	curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, curlHeaderFun);
	curl_easy_setopt(curl, CURLOPT_WRITEHEADER, ctx);

	ctx->curl = curl;
	ctx->headers = headers;

	return true;
}

static bool execCurlRequest(musicStreamContext* ctx)
{
	CURLcode res = curl_easy_perform(ctx->curl);

	if(ctx->headers != NULL)
	{
		curl_slist_free_all(ctx->headers);
		ctx->headers = NULL;
	}

	if(res != CURLE_OK)
	{
		eprintf("Downloading failed, cURL error: %s\n", curl_easy_strerror(res));
		return false;
	}

	return true;
}

static void playbackCB_SDL(void* userdata, int16_t* samples, size_t numSamples)
{
	musicStreamContext* ctx = userdata;

	size_t decSize = numSamples*sizeof(int16_t);

	if(SDL_QueueAudio(ctx->sdlAudioDevId, samples, decSize) != 0)
	{
		eprintf("SDL_QueueAudio(%d, buf, %zd) failed: %s\n", ctx->sdlAudioDevId, decSize, SDL_GetError());
	}
}

static bool initAudioCB_SDL(void* userdata, int sampleRate, int numChannels)
{
	musicStreamContext* ctx = userdata;
	if(ctx->sdlAudioDevId != 0)
	{
		SDL_CloseAudioDevice(ctx->sdlAudioDevId);
	}

	SDL_AudioSpec want, have;
	SDL_AudioDeviceID dev;

	SDL_zero(want);
	want.freq = sampleRate;
	want.format = AUDIO_S16;
	want.channels = numChannels;
	want.samples = 4096;
	want.callback = NULL; // I wanna use SDL_QueueAudio()
	dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);

	if(dev == 0)
	{
		eprintf("SDL_OpenAudioDevice() failed: %s\n", SDL_GetError());
		return false;
	}

	ctx->sdlAudioDevId = dev;

	SDL_PauseAudioDevice(dev, 0);

	return true;
}

static bool initContext(musicStreamContext* ctx, const char* url)
{
	if(mpg123_init() != MPG123_OK)
	{
		eprintf("Initializing libmpg123 failed!\n");
		return false;
	}

	// set default samplerate + number of channels
	ctx->sampleRate = 44100;
	ctx->numChannels = 2;

	// TODO: remove this later (or get arguments from function arguments or something)
	ctx->userdata = ctx;
	ctx->playbackCB = playbackCB_SDL;
	ctx->initAudioCB = initAudioCB_SDL;

	ctx->initAudioCB(ctx->userdata, ctx->sampleRate, ctx->numChannels);

	if(!prepareCURL(ctx, url)) return false;

	return true;
}

static void closeContext(musicStreamContext* ctx)
{
	if(ctx->shutdown != NULL)
	{
		ctx->shutdown(ctx);
	}

	curl_easy_cleanup(ctx->curl);

	free(ctx->icyName);
	free(ctx->icyGenre);
	free(ctx->icyURL);
	free(ctx->icyDescription);

	if(ctx->sdlAudioDevId != 0) SDL_CloseAudioDevice(ctx->sdlAudioDevId);

	memset(ctx, 0 , sizeof(*ctx));
}

int main(int argc, char** argv)
{
	if(argc < 2)
	{
		eprintf("Usage: %s <mp3 stream URL>\n",  argv[0]);
		return 1;
	}
	const char* url = argv[1];

	musicStreamContext ctx = {0};
	SDL_Init(SDL_INIT_AUDIO);

	if(!initContext(&ctx, url)) return 1;

	execCurlRequest(&ctx);

	closeContext(&ctx);

	SDL_Quit();

	return 0;
}
