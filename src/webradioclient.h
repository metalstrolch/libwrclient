/*
 * libwrclient - webradio client library
 *
 * Copyright (C) 2015-2017 Masterbrain Bytes GmbH & Co. KG
 *
 * Released under MIT license, see LICENSE.txt
 */


// the public interface of libwebradioclient

#ifndef SRC_WEBRADIOCLIENT_H_
#define SRC_WEBRADIOCLIENT_H_

#include <stddef.h> // size_t
#include <stdint.h> // int16_t

#ifdef _WIN32
	#ifdef WRC__COMPILING_LIB
		#define WRC_EXTERN __declspec(dllexport)
	#else // not compiling libwrclient => normal include of the header
		#define WRC_EXTERN __declspec(dllimport)
	#endif // WRC__COMPILING_LIB
#else // not _WIN32 - nothing to do here
	#define WRC_EXTERN
#endif

#ifdef __cplusplus
extern "C" {
#endif

// the details of the struct are private, it'll only be passed around by pointer
struct WRC__Stream;
typedef struct WRC__Stream WRC_Stream;

enum
{
	WRC_ERR_NOERROR = 0,
	WRC_ERR_UNAVAILABLE = 1, // can't connect to stream
	WRC_ERR_UNSUPPORTED_FORMAT = 2, // not ogg or mp3
	WRC_ERR_CORRUPT_STREAM = 3, // I received garbage
	WRC_ERR_INIT_AUDIO_FAILED = 4, // the initAudio callback returned 0

	WRC_ERR_GENERIC = 255 // some other error
};

// the following types are for callbacks provided by the user
// void* userdata is the userdata provided to WRC_CreateStream()

// called whenever fresh samples are available
typedef void (*WRC_playbackCB)(void* userdata, int16_t* samples, size_t numSamples);

// called on start and whenever samplerate and/or number of channels change.
// return 0 if you can't support that format to abort further decoding of the current stream
typedef int (*WRC_initAudioCB)(void* userdata, int sampleRate, int numChannels);

// called on connection with the data from the icy headers about the station
typedef void (*WRC_stationInfoCB)(void* userdata, const char* name, const char* genre,
                                  const char* description, const char* url);

// called whenever the the stream metadata about the currently playing title is updated
// the title usually consists of the band and the track title and sometimes also the station name
// it should just be displayed like it is.
typedef void (*WRC_currentTitleCB)(void* userdata, const char* currentTitle);

// called when an unrecoverable error happens (server not reachable, stream in unsupported format, ...)
// errorCode will be one of WRC_ERR_* from above
typedef void (*WRC_reportErrorCB)(void* userdata, int errorCode, const char* errormsg);



// Sets up global internal stuff - call this *once* before using the library
// (after loading it or on startup of your application or whatever)
// Returns 1 on success, 0 on error
WRC_EXTERN int WRC_Init();

// Clears global internal stuff - call this *once* before unloading the library
// or before shutting down your application
WRC_EXTERN void WRC_Shutdown();

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
WRC_EXTERN WRC_Stream* WRC_CreateStream(const char* url, WRC_playbackCB playbackFn,
                                        WRC_initAudioCB initAudioFn, void* userdata);

// Set the callbacks for receiving metadata information for the given stream.
// * stationInfoFn:  Will be called once after connecting to give you infos about
//                   the web radio station, obtained from HTTP/ICY headers.
// * currentTitleFn: Will be called whenever the streaming server sends updated
//                   metadata for the currently played title
// You probably want to set these before calling WRC_StartStreaming() - stationInfoFn
// won't be called otherwise.
// Either or both arguments may be NULL if you don't care about that kind of metadata.
WRC_EXTERN void WRC_SetMetadataCallbacks(WRC_Stream* stream, WRC_stationInfoCB stationInfoFn,
                                         WRC_currentTitleCB currentTitleFn);

// If you set this callback, unrecoverable errors will be reported to you via reportErrorFn.
WRC_EXTERN void WRC_SetErrorReportingCallback(WRC_Stream* stream, WRC_reportErrorCB reportErrorFn);

// Start streaming. Streams until you call WRC_StopStreaming(), so you probably
// want to call this in a thread.
// (Special case: If you passed NULL as playbackFn in WRC_CreateStream(), it returns
//   right after connecting and calling the stationInfoFn() metadata callback)
// Returns 0 when an error occurs (server not reachable, unsupported format, ...)
// Returns 1 if there was no error and streaming stopped only because you called
//   WRC_StopStreaming(). After that you may call WRC_StartStreaming() with the same
//   stream to connect to the same server again and start streaming again.
WRC_EXTERN int WRC_StartStreaming(WRC_Stream* stream);

// Stop the current stream by disconnecting from the server.
// WRC_StartStreaming() will return shortly after you called this.
WRC_EXTERN void WRC_StopStreaming(WRC_Stream* stream);

// free()s all resources hold by the stream and the stream object itself.
WRC_EXTERN void WRC_CleanupStream(WRC_Stream* stream);

#ifdef __cplusplus
} // extern "C"
#endif

#endif /* SRC_WEBRADIOCLIENT_H_ */
