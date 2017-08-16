libwrclient is a library that allows you to easily connect to a webradio stream
and decode it.
The project's website is https://github.com/MasterbrainBytes/libwrclient.  
It currently supports mp3 streams (using libmpg123 for decoding) and ogg/vorbis
streams (using libogg and libvorbis) and uses libcurl for the http connection.

libwrclient has a simple API that allows you to set callbacks for the
decoded audio (int16_t samples in the samplerate and channel count used by the
stream), radio station info, current title etc.

src/webradioclient.h is the header you should include to use libwrclient
and documents the API.

```c
#include "libwrclient.h"

// called whenever new data was received from the webradio server
static void my_playbackCB(void* userdata, int16_t* samples, size_t numSamples)
{
	// TODO: play numSamples samples..
}

// called whenever the audio format changes (at least once when streaming starts)
static int my_initAudioCB(void* userdata, int sampleRate, int numChannels)
{
	// TODO: do whatever you have to to to play back audio with the given
	//       samplerate and number of channels.
	
	return 1; // return 0; if there was an error, like you can't support the format
}

// this will be called right after connecting to the server with the metadata
// from the server. arguments might be NULL if that information wasn't given
static void my_stationInfoCB(void* userdata, const char* name, const char* genre,
                             const char* description, const char* url)
{
	printf("Station Info: \n");
	printf("  Name: %s\n", name);
	printf("  Genre: %s\n", genre);
	printf("  Description: %s\n", description);
	printf("  URL: %s\n", url);
}

// called whenever the stream announces a changed title
static void my_currentTitleCB(void* userdata, const char* currentTitle)
{
	printf("Updated Title: %s\n", currentTitle);
}

// called whenever an unrecoverable error happens.
// errorCode is WRC_ERR_* from webradioclient.h
static void my_reportErrorCB(void* userdata, int errorCode, const char* errormsg)
{
	printf("ERROR %d: %s\n", errorCode, errormsg);
}


// ...

	WRC_Init(); // call this once before using the lib
	
	void* userdata = whatever; // this is passed to all your callbacks

	WRC_Stream* stream = WRC_CreateStream(url, my_playbackCB, my_initAudioCB, userdata);

	if(stream != NULL)
	{
		WRC_SetMetadataCallbacks(stream, my_stationInfoCB, my_currentTitleCB);
		WRC_SetErrorReportingCallback(stream, my_reportErrorCB);

		// the following will block until you call WRC_StopStreaming(stream);
		// (from another thread) or until an error happens
		WRC_StartStreaming(stream);
	}

	WRC_Shutdown(); // call this once you're not using the lib anymore

// ...
```

src/sdl2client.c is a simple commandline webradio stream player that uses
SDL2 for sound output and serves as an example on how to use the API.

libwrclient itself is released under the MIT license, but it uses libmpg123
for mp3 decoding which is released under LGPL v2.1, so unless you disable mp3
support that is the license you'll have to follow.  
See LICENSE.txt for more details.
