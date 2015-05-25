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

	char mp3Buf[65536];
	int mp3BufAfterEndIdx;

	int sampleRate;
	int numChannels;
	// let's assume format is always 16bit int - but signed or unsigned, little or big endian?

	mpg123_handle* handle;

	SDL_AudioDeviceID sdlAudioDevId;

	size_t dataWritten; // TODO: remove

	FILE* out;
} mp3streamContext;

static void parseInBodyIcyHeader(mp3streamContext* ctx)
{

	printf("## header: %s\n", ctx->headerBuf);

	// TODO: parse the header, at least get "content-type:audio/mpeg"
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

	mpg123_open_feed(h);

	SDL_PauseAudioDevice(ctx->sdlAudioDevId, 0);

	return true;
}

static void playMP3(mp3streamContext* ctx, void* data, size_t size)
{
#if 0

	const int bufSize = sizeof(ctx->mp3Buf);
	// TODO: feed into mpg123 and call SDL with result

	int copySize = min(bufSize - ctx->mp3BufAfterEndIdx, size);

	memcpy(ctx->mp3Buf + ctx->mp3BufAfterEndIdx, data, copySize);
	ctx->mp3BufAfterEndIdx += copySize;
#endif // 0

	byte decBuf[32768];

	if(ctx->handle == NULL) initMP3(ctx);

	assert(ctx->handle != NULL);

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
	size_t remDataSize = freshDataSize;
	mp3streamContext* ctx = (mp3streamContext*)context;

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
				// header is not in body, just play the stream-
				// TODO: the header data is in the http header, we could extract
				//       it from there and save info to ctx
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

	if(remDataSize > 0)
	{
		//fwrite(remData, remDataSize, 1, ctx->out);
		playMP3(ctx, remData, remDataSize);
	}

	ctx->dataWritten += remDataSize;

	return freshDataSize;
}

static bool prepareCURL(const char* url, mp3streamContext* ctx)
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

	headers = curl_slist_append(headers, "Icy-MetaData:0"); // TODO: set to 1 for stream info

	curl_easy_setopt(curl, CURLOPT_URL, url);
	curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1 );
	curl_easy_setopt(curl, CURLOPT_PROTOCOLS, CURLPROTO_HTTP | CURLPROTO_HTTPS);
	curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS, CURLPROTO_HTTP | CURLPROTO_HTTPS);
	curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1); // follow http 3xx redirects
	curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 10);
	curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curlWriteFun);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, ctx);

	ctx->curl = curl;
	ctx->headers = headers;
	//ctx->out = f;

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

int main(int argc, char** argv)
{
	if(argc < 2)
	{
		eprintf("Usage: %s <mp3 stream URL>\n",  argv[0]);
		return 1;
	}
	const char* url = argv[1];

	mp3streamContext ctx = {0};

	mpg123_init();

	SDL_Init(SDL_INIT_AUDIO);
	if(!initSDLaudio(&ctx)) return 1;

	if(!prepareCURL(url, &ctx)) return 1;

	execCurlRequest(&ctx);

	//fclose(ctx.out);

	SDL_CloseAudioDevice(ctx.sdlAudioDevId);
	SDL_Quit();

	return 0;
}
