#include <SDL.h>
#include "webradioclient.h"

// build with: clang -ggdb -Wall -o wrclient `/opt/sdl2/bin/sdl2-config --cflags` -I src/ src/sdl2client.c \
//             `/opt/sdl2/bin/sdl2-config --libs` -L./ -lwrclient -Wl,-rpath,'$ORIGIN'

#define eprintf(...) fprintf(stderr, __VA_ARGS__)

static void playbackCB_SDL(void* userdata, int16_t* samples, size_t numSamples)
{
	SDL_AudioDeviceID* dev = userdata;

	size_t decSize = numSamples*sizeof(int16_t);

	if(SDL_QueueAudio(*dev, samples, decSize) != 0)
	{
		eprintf("SDL_QueueAudio(%d, buf, %zd) failed: %s\n", *dev, decSize, SDL_GetError());
	}
}

static int initAudioCB_SDL(void* userdata, int sampleRate, int numChannels)
{
	SDL_AudioDeviceID* oldDev = userdata;
	if(*oldDev != 0)
	{
		SDL_CloseAudioDevice(*oldDev);
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
		return 0;
	}

	*oldDev = dev;

	SDL_PauseAudioDevice(dev, 0);

	return 1;
}

static void stationInfoCB_SDL(void* userdata, const char* name, const char* genre, const char* description, const char* url)
{
	printf("Station Info: \n");
	printf("  Name: %s\n", name);
	printf("  Genre: %s\n", genre);
	printf("  Description: %s\n", description);
	printf("  URL: %s\n", url);
}

static void currentTitleCB_SDL(void* userdata, const char* currentTitle)
{
	printf("Updated Title: %s\n", currentTitle);
}

static void reportErrorCB_SDL(void* userdata, const char* errormsg)
{
	printf("ERROR: %s\n", errormsg);
}

int main(int argc, char** argv)
{
	if(argc < 2)
	{
		eprintf("Usage: %s <mp3 stream URL>\n",  argv[0]);
		return 1;
	}
	const char* url = argv[1];

	SDL_Init(SDL_INIT_AUDIO);

	WRC_Init();

	SDL_AudioDeviceID audioDev = 0;
	WRC_Stream* stream = WRC_CreateStream(url, playbackCB_SDL, initAudioCB_SDL, &audioDev);

	WRC_SetMetadataCallbacks(stream, stationInfoCB_SDL,  currentTitleCB_SDL);

	WRC_SetErrorReportingCallback(stream, reportErrorCB_SDL);

	if(stream != NULL)
	{
		WRC_StartStreaming(stream);

		if(audioDev != 0) SDL_CloseAudioDevice(audioDev);
	}

	WRC_Shutdown();

	SDL_Quit();

	return 0;
}
