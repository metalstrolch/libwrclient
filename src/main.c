#define _GNU_SOURCE
#include <string.h>
#include <stdio.h>
#include <stdbool.h>
#include <assert.h>

#include <curl/curl.h>
#include <mpg123.h>

#include <SDL.h>

typedef unsigned char byte;

#define eprintf(...) fprintf(stderr, __VA_ARGS__)

enum CONTENT_TYPE {
	WRC_CONTENT_UNKNOWN,
	WRC_CONTENT_MP3,
	// TODO: ogg etc
};

enum ICY_HEADER_STATE {
	WRC_ICY_UNDECIDED,
	WRC_ICY_HEADER_IN_BODY,
	WRC_ICY_HEADER_DONE
};

typedef struct _mp3streamContext
{
	CURL* curl;
	struct curl_slist* headers;
	enum CONTENT_TYPE contentType;
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

	SDL_AudioDeviceID sdlAudioDevId;
} mp3streamContext;

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
	int startCmp = strncmp(str, cmpStr, len);

	if(startCmp != 0) return startCmp;
	// so the first len chars were equal.. if this is str's end, that's ok.
	char nextC = str[len];
	if(nextC == '\r' || nextC == '\n' || nextC == '\0')
		return 0;
	else
		return 1;
}



static void handleHeaderLine(mp3streamContext* ctx, const char* line, size_t len)
{
	const char* lineAfterEnd = line+len;
	const char* str;
	if((str = remLineIfStartsWith(line, "content-type:")))
	{
		if(strCmpToNL(str, "audio/mpeg") == 0)
		{
			ctx->contentType = WRC_CONTENT_MP3;
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

static void parseInBodyIcyHeader(mp3streamContext* ctx)
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

static bool initMP3(mp3streamContext* ctx)
{
	mpg123_handle* h = mpg123_new(NULL, NULL);
	ctx->handle = h;

	if(h == NULL)
	{
		eprintf("WTF, mpg123_new() failed!\n");
		return false;
	}

	mpg123_format_none(h);
	if(mpg123_format(h, 44100, MPG123_STEREO, MPG123_ENC_UNSIGNED_16) != MPG123_OK)
	{
		eprintf("setting ouput format for mpg123 failed!\n");
		return false;
	}

	ctx->numChannels = 2;
	ctx->sampleRate = 44100;

	mpg123_open_feed(h);

	SDL_PauseAudioDevice(ctx->sdlAudioDevId, 0);

	return true;
}

static void playMP3(mp3streamContext* ctx, void* data, size_t size)
{
	byte decBuf[32768];

	if(ctx->handle == NULL) initMP3(ctx);

	assert(ctx->handle != NULL);

	if(size == 0) return;

	size_t decSize=0;
	int mRet = mpg123_decode(ctx->handle, data, size, decBuf, sizeof(decBuf), &decSize);
	if(mRet == MPG123_ERR)
	{
		eprintf("mpg123_decode failed: %s\n", mpg123_strerror(ctx->handle));
		return;
	}

	if(decSize != 0 && SDL_QueueAudio(ctx->sdlAudioDevId, decBuf, decSize) != 0)
	{
		eprintf("SDL_QueueAudio(%d, buf, %zd) failed: %s\n", ctx->sdlAudioDevId, decSize, SDL_GetError());
	}

	while(mRet != MPG123_ERR && mRet != MPG123_NEED_MORE)
	{
		// get as much decoded audio as available from last feed
		mRet = mpg123_decode(ctx->handle, NULL, 0, decBuf, sizeof(decBuf), &decSize);
		if(decSize != 0)
		{
			if(SDL_QueueAudio(ctx->sdlAudioDevId, decBuf, decSize) != 0)
			{
				eprintf("SDL_QueueAudio(%d, buf, %zd) failed: %s\n", ctx->sdlAudioDevId, decSize, SDL_GetError());
			}
		}
	}
}

static size_t curlWriteFun(void* freshData, size_t size, size_t nmemb, void* context)
{
	const size_t freshDataSize = size*nmemb;
	mp3streamContext* ctx = (mp3streamContext*)context;

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
			char* headerEnd = memmem(remData, remDataSize, "\r\n\r\n", 4);
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

	// TODO: metaint
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
			playMP3(ctx, remData, dataToNextMeta);
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


	playMP3(ctx, remData, remDataSize);


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
	mp3streamContext* ctx = (mp3streamContext*)userdata;
	handleHeaderLine(ctx, buffer, dataSize);
	return dataSize;
}

static bool prepareCURL(mp3streamContext* ctx, const char* url)
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

	headers = curl_slist_append(headers, "Icy-MetaData:1"); // TODO: set to 1 for stream info

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

static bool execCurlRequest(mp3streamContext* ctx)
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

static bool initSDLaudio(mp3streamContext* ctx)
{
	SDL_Init(SDL_INIT_AUDIO);
	SDL_AudioSpec want, have;
	SDL_AudioDeviceID dev;

	SDL_zero(want);
	want.freq = 44100;
	want.format = AUDIO_U16;
	want.channels = 2;
	want.samples = 4096;
	want.callback = NULL; // I wanna use SDL_QueueAudio()
	dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);

	if(dev == 0)
	{
		eprintf("SDL_OpenAudioDevice() failed: %s\n", SDL_GetError());
		return false;
	}

	ctx->sdlAudioDevId = dev;

	return true;
}

static bool initContext(mp3streamContext* ctx, const char* url)
{
	if(mpg123_init() != MPG123_OK)
	{
		eprintf("Initializing libmpg123 failed!\n");
		return false;
	}

	if(!initSDLaudio(ctx)) return false;

	if(!prepareCURL(ctx, url)) return false;

	return true;
}

static void closeContext(mp3streamContext* ctx)
{
	if(ctx->handle != NULL) mpg123_close(ctx->handle);

	curl_easy_cleanup(ctx->curl);

	free(ctx->icyName);
	free(ctx->icyGenre);
	free(ctx->icyURL);
	free(ctx->icyDescription);

	if(ctx->sdlAudioDevId != 0) SDL_CloseAudioDevice(ctx->sdlAudioDevId);

	memset(ctx, 0 , sizeof(*ctx));

	SDL_Quit();
}

int main(int argc, char** argv)
{
	if(argc < 2)
	{
		eprintf("Usage: %s <mp3 stream URL>\n",  argv[0]);
		return 1;
	}
	const char* url = argv[1];

	mp3streamContext ctx = {0};

	if(!initContext(&ctx, url)) return 1;

	execCurlRequest(&ctx);

	closeContext(&ctx);

	return 0;
}
