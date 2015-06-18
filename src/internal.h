
// internal declarations shared between .c files
#ifndef SRC_INTERNAL_H_
#define SRC_INTERNAL_H_

#include "webradioclient.h"

#include <string.h>
#include <stdio.h>
#include <stdbool.h>

#include <curl/curl.h>

#define WRC_OGG 1
#define WRC_MP3 1

#define eprintf(...) fprintf(stderr, __VA_ARGS__) // TODO: remove

enum WRC__CONTENT_TYPE {
	WRC_CONTENT_UNKNOWN = 0,
	WRC_CONTENT_MP3,
	WRC_CONTENT_OGGVORBIS,
	// TODO: other formats?
};

enum WRC__STREAM_STATE {
	WRC__STREAM_FRESH = 0,
	WRC__STREAM_ICY_HEADER_IN_BODY,
	WRC__STREAM_MUSIC,
	WRC__STREAM_ABORT_GRACEFULLY,
	WRC__STREAM_ABORT_ERROR
};

enum WRC__OGG_DECODE_STATE {
	WRC_OGGDEC_PREINIT = 0,
	WRC_OGGDEC_VORBISINFO,
	WRC_OGGDEC_COMMENT,
	WRC_OGGDEC_SETUP,
	WRC_OGGDEC_STREAMDEC
};

// number of samples in the temporary decoding buffer that is then passed to WRC_playbackCB()
// => not more than WRC__decBufSize samples will be sent to the user at once
static const int WRC__decBufSize = 4096;

#ifdef WRC_MP3
#include <mpg123.h>

bool WRC__decodeMP3(WRC_Stream* ctx, void* data, size_t size);
#endif // WRC_MP3

#ifdef WRC_OGG
#include <vorbis/codec.h>

bool WRC__decodeOGG(WRC_Stream* ctx, void* data, size_t size);

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

	enum WRC__OGG_DECODE_STATE state;
	int maxBufSamplesPerChan;
};

#endif // WRC_OGG

static inline int WRC__min(int a, int b) { return a < b ? a : b; }

void WRC__errorReset(WRC_Stream* ctx, int errorCode, const char* format, ...);

#ifdef _WIN32
int WRC__vsnprintf(char *dst, size_t size, const char *format, va_list ap);
int WRC__snprintf(char *dst, size_t size, const char *format, ...);
#else // a platform that has a working (v)snprintf..
#define WRC__vsnprintf vsnprintf
#define WRC__snprintf snprintf
#endif // _WIN32

struct WRC__Stream
{
	char url[2048];

	CURL* curl;
	struct curl_slist* headers;
	enum WRC__CONTENT_TYPE contentType;
	char* contentTypeHeaderVal;

	enum WRC__STREAM_STATE streamState;

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

	char* icyName;
	char* icyGenre;
	char* icyURL;
	char* icyDescription;

#ifdef WRC_MP3
	mpg123_handle* handle;
#endif

#ifdef WRC_OGG
	struct WRC__oggVorbisContext ogg;
#endif

	// function pointers for the implementation of the current format

	// decode returns true if decoding was successful or if it just needs more data
	//    and false, if there was a non-recoverable error
	bool (*decode)(struct WRC__Stream* ctx, void* data, size_t size);
	void (*shutdown)(struct WRC__Stream* ctx);

	// function pointers for callbacks to user-code, so the user can play the decoded music
	// and display (changed) metadata etc

	void* userdata; // this will be passed to the user-specified callbacks

	WRC_playbackCB playbackCB;
	WRC_initAudioCB initAudioCB;

	WRC_stationInfoCB stationInfoCB;
	WRC_currentTitleCB currentTitleCB;
	WRC_reportErrorCB reportErrorCB;
};


#endif /* SRC_INTERNAL_H_ */
