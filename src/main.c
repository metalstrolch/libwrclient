
#include "internal.h"
#include <stdarg.h>
#include <assert.h>

// TODO: some buffering before starting to decode?

/*	Takes input from <src> and decodes into <dest>, which should be a buffer
	large enough to hold <strlen(src) + 1> characters.

	If <src> is <NULL>, input will be taken from <dest>, decoding
	the entities in-place.

	The function returns the length of the decoded string.
*/
size_t WRC__decode_html_entities_utf8(char *dest, const char *src);

#ifdef _WIN32
int WRC__vsnprintf(char *dst, size_t size, const char *format, va_list ap)
{
	int ret = -1;
	if(dst != NULL && size > 0)
	{
#if defined(_MSC_VER) && _MSC_VER >= 1400
		// I think MSVC2005 introduced _vsnprintf_s().
		// this shuts up _vsnprintf() security/deprecation warnings.
		ret = _vsnprintf_s(dst, size, _TRUNCATE, format, ap);
#else
		ret = _vsnprintf(dst, size, format, ap);
		dst[size-1] = '\0'; // ensure '\0'-termination
#endif
	}

	if(ret == -1)
	{
		// _vsnprintf() returns -1 if the output is truncated, real vsnprintf()
		// returns the number of characters that would've been written..
		// fortunately _vscprintf() calculates that.
		ret = _vscprintf(format, ap);
	}

	return ret;
}

int WRC__snprintf(char *dst, size_t size, const char *format, ...)
{
	int ret = 0;

	va_list argptr;
	va_start( argptr, format );

	ret = WRC__vsnprintf(dst, size, format, argptr);

	va_end( argptr );

	return ret;
}
#endif // _WIN32

// search for needle in haystack (like strstr() for for data that might not be '\0'-terminated)
static void* WRC__memmem(const void* haystack, size_t haystacklen, const void* needle, size_t needlelen)
{
	assert((haystack != NULL || haystacklen == 0)
			&& (needle != NULL || needlelen == 0)
			&& "Don't pass NULL into WRC__memmem(), unless the corresponding len is 0!");

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
// also replaces html-encoding with proper UTF-8 (for umlauts etc)
static char* copyDecodeHeaderStr(const char* str, const char* strAfterEnd)
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
		WRC__decode_html_entities_utf8(ret, NULL);
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

static bool decodeDummyFail(WRC_Stream* ctx, void* data, size_t size)
{
	return false;
}

static void handleHeaderLine(WRC_Stream* ctx, const char* line, size_t len)
{
	const char* lineAfterEnd = line+len;
	const char* str;
	if((str = remLineIfStartsWith(line, "content-type:")))
	{
		ctx->contentTypeHeaderVal = copyDecodeHeaderStr(str, lineAfterEnd);
#ifdef WRC_MP3
		if(strCmpToNL(str, "audio/mpeg") == 0)
		{
			ctx->contentType = WRC_CONTENT_MP3;
			ctx->decode = WRC__decodeMP3;
		}
		else
#endif // WRC_MP3
#ifdef WRC_OGG
		if(strCmpToNL(str, "application/ogg") == 0 || strCmpToNL(str, "audio/ogg") == 0)
		{
			ctx->contentType = WRC_CONTENT_OGGVORBIS;
			ctx->decode = WRC__decodeOGG;
		}
		else
#endif // WRC_OGG
		{
			ctx->contentType = WRC_CONTENT_UNKNOWN;
			ctx->decode = decodeDummyFail; // this returns false so curlWriteFun() will abort
			WRC__errorReset(ctx, WRC_ERR_UNSUPPORTED_FORMAT, "unknown content type: %s", str);
		}
	}
	else if((str = remLineIfStartsWith(line, "icy-name:")))
	{
		ctx->icyName = copyDecodeHeaderStr(str, lineAfterEnd);
	}
	else if((str = remLineIfStartsWith(line, "icy-genre:")))
	{
		ctx->icyGenre = copyDecodeHeaderStr(str, lineAfterEnd);
	}
	else if((str = remLineIfStartsWith(line, "icy-url:")))
	{
		ctx->icyURL = copyDecodeHeaderStr(str, lineAfterEnd);
	}
	else if((str = remLineIfStartsWith(line, "icy-description:")))
	{
		ctx->icyDescription = copyDecodeHeaderStr(str, lineAfterEnd);
	}
	else if((str = remLineIfStartsWith(line, "icy-metaint:")))
	{
		ctx->icyMetaInt = atoi(str);
	}
}

static void parseInBodyIcyHeader(WRC_Stream* ctx)
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

void WRC__errorReset(WRC_Stream* ctx, int errCode, const char* format, ...)
{
	if(ctx->reportErrorCB != NULL)
	{
		char msgBuf[512];
		msgBuf[0] = '\0';

		va_list argptr;
		va_start( argptr, format );

		WRC__vsnprintf(msgBuf, sizeof(msgBuf), format, argptr);

		va_end(argptr);

		ctx->reportErrorCB(ctx->userdata, errCode, msgBuf);
	}

	if(ctx->shutdown != NULL)
		ctx->shutdown(ctx);

	ctx->streamState = WRC__STREAM_ABORT_ERROR;
}

static bool decodeMusic(WRC_Stream* ctx, void* data, size_t size)
{
	if(ctx->decode != NULL)
	{
		return ctx->decode(ctx, data, size);
	}

	return false;
}

static void sendStationInfo(WRC_Stream* ctx)
{
	if(ctx->stationInfoCB != NULL)
	{
		ctx->stationInfoCB(ctx->userdata, ctx->icyName, ctx->icyGenre, ctx->icyDescription, ctx->icyURL);
	}
}

// strips crap from the icy metadata string from the periodic updates,
// which looks like:
// "StreamTitle='Norma Jean - Opposite Of Left And Wrong | WackenRadio.com';StreamUrl='';"
// and then calls ctx->currentTitleCB(), if any
static void stripIcyMetaBufAndTellUser(WRC_Stream* ctx, char* str)
{
	static const int bufLen = 256*16;

	str[bufLen-1] = '\0'; // make sure it's terminated.

	char* streamTitleStart = strstr(str, "StreamTitle=\'");
	char* streamTitleEnd = NULL;
	if(streamTitleStart == NULL)
	{
		streamTitleStart = str;
	}
	else
	{
		streamTitleStart += strlen("StreamTitle=\'");
	}

	streamTitleEnd = strstr(streamTitleStart, "\';");
	if(streamTitleEnd == NULL)
	{
		streamTitleEnd = str + strlen(str);
	}

	*streamTitleEnd = '\0'; // cut off "';"

	if(ctx->currentTitleCB != NULL)
	{
		ctx->currentTitleCB(ctx->userdata, streamTitleStart);
	}
}

static size_t curlWriteFun(void* freshData, size_t size, size_t nmemb, void* context)
{
	const size_t freshDataSize = size*nmemb;
	WRC_Stream* ctx = (WRC_Stream*)context;

	size_t remDataSize = freshDataSize;

	char* remData = freshData;

	if(ctx->streamState >= WRC__STREAM_ABORT_GRACEFULLY)
	{
		// if this function doesn't return size*nmemb,
		// cURL will assume an error and abort.
		return 0;
	}
	else if(ctx->streamState < WRC__STREAM_MUSIC)
	{
		if(ctx->streamState == WRC__STREAM_FRESH)
		{
			// this should only happen with the very first data received
			if(memcmp(freshData, "ICY 200 OK", strlen("ICY 200 OK")) == 0)
			{
				ctx->streamState = WRC__STREAM_ICY_HEADER_IN_BODY;
			}
			else
			{
				// header is not in body, just play the stream.
				// the metadata from the headers has been received via the curlHeaderFun() callback.
				ctx->streamState = WRC__STREAM_MUSIC;
			}
		}

		if(ctx->streamState == WRC__STREAM_ICY_HEADER_IN_BODY)
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
			int numBytes = WRC__min(headerDataSize, bufSize - ctx->headerBufAfterEndIdx - 1);
			memcpy(ctx->headerBuf+ctx->headerBufAfterEndIdx, remData, numBytes);
			ctx->headerBufAfterEndIdx += numBytes;

			if(headerEnd == NULL)
			{
				// part of the header is still missing.. return now and wait for more data
				return freshDataSize;
			}

			// ok, headerEnd is not NULL, i.e. the whole header has been received.
			ctx->streamState = WRC__STREAM_MUSIC;
			ctx->headerBuf[ctx->headerBufAfterEndIdx] = '\0';
			parseInBodyIcyHeader(ctx);

			remData = headerEnd; // \r\n\r\n

			remDataSize -= headerDataSize;
		}

		// tell the user about the station info via his callback
		sendStationInfo(ctx);

		if(ctx->playbackCB == NULL)
		{
			// abort stream gracefully - probably the user only wanted the metadata
			ctx->streamState = WRC__STREAM_ABORT_GRACEFULLY;
			return 0;
		}
	}

	if(ctx->icyMetaInt != 0)
	{
		int dataToNextMeta = ctx->icyMetaInt - ctx->dataReadSinceLastIcyMeta;
		if(ctx->icyMetaBytesMissing > 0)
		{
			char* mdBuf = ctx->icyMetadata[!ctx->icyMetadataIdx];
			int writeMdBytes = WRC__min(remDataSize, ctx->icyMetaBytesMissing);
			memcpy(mdBuf + ctx->icyMetaBytesWritten, remData, writeMdBytes);

			remDataSize -= writeMdBytes;
			remData += writeMdBytes;
			ctx->icyMetaBytesMissing -= writeMdBytes;
			ctx->icyMetaBytesWritten += writeMdBytes;
			if(ctx->icyMetaBytesMissing == 0)
			{
				stripIcyMetaBufAndTellUser(ctx, mdBuf);
				ctx->icyMetadataIdx = !ctx->icyMetadataIdx;
				ctx->dataReadSinceLastIcyMeta = 0;
				ctx->icyMetaBytesWritten = 0;
			}
		}
		else if(dataToNextMeta < remDataSize)
		{
			if(!decodeMusic(ctx, remData, dataToNextMeta))
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
				int writeMdBytes = WRC__min(remDataSize, numBytes);
				memcpy(mdBuf, remData, writeMdBytes);

				remDataSize -= writeMdBytes;
				remData += writeMdBytes;
				ctx->icyMetaBytesMissing = numBytes - writeMdBytes;
				ctx->icyMetaBytesWritten = writeMdBytes;
				if(ctx->icyMetaBytesMissing == 0)
				{
					stripIcyMetaBufAndTellUser(ctx, mdBuf);
					ctx->icyMetadataIdx = !ctx->icyMetadataIdx;
					ctx->dataReadSinceLastIcyMeta = 0;
					ctx->icyMetaBytesWritten = 0;
				}
			}
		}

		ctx->dataReadSinceLastIcyMeta += remDataSize;
	}


	if(!decodeMusic(ctx, remData, remDataSize))
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
	WRC_Stream* ctx = (WRC_Stream*)userdata;
	long respCode = 0;
	curl_easy_getinfo(ctx->curl, CURLINFO_RESPONSE_CODE, &respCode);
	if(respCode >= 200 && respCode < 300)
	{
		// don't parse headers of forwardings, errorpages etc
		handleHeaderLine(ctx, buffer, dataSize);
	}
	return dataSize;
}

static bool prepareCURL(WRC_Stream* ctx)
{
	CURL* curl = curl_easy_init();

	if(curl == NULL)
	{
		eprintf("Initializing cURL failed!\n");
		return false;
	}

	struct curl_slist* headers = NULL;

	headers = curl_slist_append(headers, "Icy-MetaData:1");

	curl_easy_setopt(curl, CURLOPT_URL, ctx->url);
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

static bool execCurlRequest(WRC_Stream* ctx)
{
	CURLcode res = curl_easy_perform(ctx->curl);

	if(ctx->headers != NULL)
	{
		curl_slist_free_all(ctx->headers);
		ctx->headers = NULL;
	}

	if(res != CURLE_OK)
	{
		if(ctx->streamState < WRC__STREAM_ABORT_GRACEFULLY)
		{
			// probably an error while connecting, might be worth reporting
			// TODO: we could possibly use res to report different kinds of errors
			//       (404, 403, domain not found, no server at that port, ...)
			WRC__errorReset(ctx, WRC_ERR_UNAVAILABLE, "Downloading failed, cURL error: %s\n", curl_easy_strerror(res));
		}
		return false;
	}

	return true;
}

#define WRC_CTX_FREE(x) free(ctx->x); ctx->x = NULL;

static void resetStream(WRC_Stream* ctx)
{
	// basically, we wanna clear everything except for the URL and the user supplied callbacks
	if(ctx->curl != NULL)
	{
		curl_easy_cleanup(ctx->curl);
		ctx->curl = NULL;

		// the header are already taken care of in execCurlRequest()
	}
	ctx->contentType = WRC_CONTENT_UNKNOWN;
	WRC_CTX_FREE(contentTypeHeaderVal);

	if(ctx->shutdown != NULL)
	{
		ctx->shutdown(ctx);
		ctx->shutdown = NULL;
	}

	ctx->streamState = WRC__STREAM_FRESH;

	memset(ctx->headerBuf, 0, sizeof(ctx->headerBuf));
	ctx->headerBufAfterEndIdx = 0;

	ctx->icyMetaInt = 0;
	memset(ctx->icyMetadata, 0, sizeof(ctx->icyMetadata));
	ctx->icyMetadataIdx = 0;
	ctx->icyMetaBytesMissing = 0;
	ctx->icyMetaBytesWritten = 0;
	ctx->dataReadSinceLastIcyMeta = 0;

	ctx->sampleRate = 44100;
	ctx->numChannels = 2;

	WRC_CTX_FREE(icyName);
	WRC_CTX_FREE(icyGenre);
	WRC_CTX_FREE(icyURL);
	WRC_CTX_FREE(icyDescription);

	ctx->decode = NULL;

	// the rest is userdata, which can remain as it is
}

#undef WRC_CTX_FREE



// Sets up global internal stuff - call this *once* before using the library
// (after loading it or on startup of your application or whatever)
// Returns 1 on success, 0 on error
int WRC_Init()
{
#ifdef WRC_MP3
	if(mpg123_init() != MPG123_OK)
	{
		eprintf("Initializing libmpg123 failed!\n");
		return 0;
	}
#endif // WRC_MP3
	return 1;
}

// Clears global internal stuff - call this *once* before unloading the library
// or before shutting down your application
void WRC_Shutdown()
{
#ifdef WRC_MP3
	mpg123_exit();
#endif // WRC_MP3
}

// Creates/prepares a new stream for the given URL, but doesn't start streaming.
// Actual streaming will be started when you call WRC_StartStreaming() with the returned stream.
// * url: The URL to connect to (*not* a playlist, but the actual http stream,
//        usually *from* a playlist, i.e. you need to parse playlists yourself!)
// * playbackFn: This callback will be called to send you decoded audio samples from the stream,
//               in the format last told you through the initAudioFn callback
//               Set this to NULL if you only want the station info metadata, but no actual music
// * initAudioFn: This callback will be called to tell you about changes in the
//                streams sameplerate or number of channels.
//                Yes, this can actually happen during playback (e.g. when a new song starts)
//                Only if you set playbackFn to NULL, this may be NULL, too.
// * userdata: Will be passed to all your callbacks (including the metadata and error reporting ones!),
//             so you can do with it whatever you want. May be NULL.
// Returns NULL on error, otherwise a WRC_Stream object.
WRC_Stream* WRC_CreateStream(const char* url, WRC_playbackCB playbackFn,
                             WRC_initAudioCB initAudioFn, void* userdata)
{
	WRC_Stream* ret = calloc(1, sizeof(struct WRC__Stream));
	if(ret == NULL)
	{
		eprintf("WRC_CreateStream(): Out of Memory!\n");
		return NULL;
	}

	if(initAudioFn == NULL)
	{
		playbackFn = NULL;
	}

	int urlLen = strlen(url);
	if(urlLen > sizeof(ret->url)-1)
	{
		eprintf("WRC_CreateStream(): URL too long!\n");
		free(ret);
		return NULL;
	}

	memcpy(ret->url, url, urlLen+1); // +1 to also copy terminating '\0'

	ret->playbackCB = playbackFn;
	ret->initAudioCB = initAudioFn;
	ret->userdata = userdata;

	// set default samplerate + number of channels
	ret->sampleRate = 44100;
	ret->numChannels = 2;

	return ret;
}

// Set the callbacks for receiving metadata information for the given stream.
// * stationInfoFn:  Will be called once after connecting to give you infos about
//                   the web radio station, obtained from HTTP/ICY headers.
// * currentTitleFn: Will be called whenever the streaming server sends updated
//                   metadata for the currently played title
// You probably want to set these before calling WRC_StartStreaming() - stationInfoFn
// won't be called otherwise.
// Either or both arguments may be NULL if you don't care about that kind of metadata.
void WRC_SetMetadataCallbacks(WRC_Stream* stream, WRC_stationInfoCB stationInfoFn,
                              WRC_currentTitleCB currentTitleFn)
{
	stream->stationInfoCB = stationInfoFn;
	stream->currentTitleCB = currentTitleFn;
}

// If you set this callback, unrecoverable errors will be reported to you via reportErrorFn.
void WRC_SetErrorReportingCallback(WRC_Stream* stream, WRC_reportErrorCB reportErrorFn)
{
	stream->reportErrorCB = reportErrorFn;
}

// Start streaming. Streams until you call WRC_StopStreaming(), so you probably
// want to call this in a thread.
// (Special case: If you passed NULL as playbackFn in WRC_CreateStream(), it returns
//   right after connecting and calling the stationInfoFn() metadata callback)
// Returns 0 when an error occurs (server not reachable, unsupported format, ...)
// Returns 1 if there was no error and streaming stopped only because you called
//   WRC_StopStreaming(). After that you may call WRC_StartStreaming() with the same
//   stream again to connect to the same server again and start streaming again.
int WRC_StartStreaming(WRC_Stream* stream)
{
	if(!prepareCURL(stream))
	{
		return 0;
	}

	int ret = execCurlRequest(stream);

	if(stream->streamState == WRC__STREAM_ABORT_GRACEFULLY)
	{
		// ret might be 0 even if we tried to shut down gracefully - after all,
		// curl assumes an error when the callback returns 0..
		return 1;
	}

	resetStream(stream);

	return ret;
}

// Stop the current stream by disconnecting from the server.
// WRC_StartStreaming() will return shortly after you called this.
void WRC_StopStreaming(WRC_Stream* stream)
{
	if(stream->streamState < WRC__STREAM_ABORT_GRACEFULLY)
	{
		stream->streamState = WRC__STREAM_ABORT_GRACEFULLY;
	}
}

// free()s all resources hold by the stream and the stream object itself.
void WRC_CleanupStream(WRC_Stream* stream)
{
	if(stream != NULL)
	{
		resetStream(stream);
		free(stream);
	}
}
