//**************************************************************************
//**
//** $Id: i_sound.c,v 1.7 2008-06-17 18:00:21 sezero Exp $
//**
//**************************************************************************


#include "h2stdinc.h"
#include <math.h>       // pow()
#include <pthread.h>
#include "h2def.h"
#include "sounds.h"
#include "i_sound.h"
#include "audio_plugin.h"


#define SAMPLE_FORMAT   FMT_S16_LE
#define SAMPLE_ZERO     0
#define SAMPLE_RATE     11025   // Hz
#define SAMPLE_CHANNELS 2

#if 0
#define SAMPLE_TYPE     char
#else
#define SAMPLE_TYPE     short
#endif


/*
 *
 *                           SOUND HEADER & DATA
 *
 *
 */


int tsm_ID = -1;

const char snd_prefixen[] = { 'P', 'P', 'A', 'S', 'S', 'S', 'M',
  'M', 'M', 'S' };

int snd_Channels;
int snd_DesiredMusicDevice, snd_DesiredSfxDevice;
int snd_MusicDevice,    // current music card # (index to dmxCodes)
	snd_SfxDevice,      // current sfx card # (index to dmxCodes)
	snd_MaxVolume,      // maximum volume for sound
	snd_MusicVolume;    // maximum volume for music
int dmxCodes[NUM_SCARDS]; // the dmx code for a given card

int     snd_SBport, snd_SBirq, snd_SBdma;       // sound blaster variables
int     snd_Mport;                              // midi variables

extern boolean  snd_MusicAvail, // whether music is available
		snd_SfxAvail;   // whether sfx are available

void I_PauseSong(int handle)
{
}

void I_ResumeSong(int handle)
{
}

void I_SetMusicVolume(int volume)
{
}

void I_SetSfxVolume(int volume)
{
}

/*
 *
 *                              SONG API
 *
 */

int I_RegisterSong(void *data)
{
  return 0;
}

void I_UnRegisterSong(int handle)
{
}

int I_QrySongPlaying(int handle)
{
  return 0;
}

// Stops a song.  MUST be called before I_UnregisterSong().

void I_StopSong(int handle)
{
}

void I_PlaySong(int handle, boolean looping)
{
}

/*
 *
 *                                 SOUND FX API
 *
 */


typedef struct
{
    unsigned char* begin;           // pointers into Sample.firstSample
    unsigned char* end;

    SAMPLE_TYPE* lvol_table;               // point into vol_lookup
    SAMPLE_TYPE* rvol_table;

    unsigned int pitch_step;
    unsigned int step_remainder;    // 0.16 bit remainder of last step.
    
    int pri;
    unsigned int time;
} Channel;


typedef struct
{
    short a;            // always 3
    short freq;         // always 11025
    int32_t length;     // sample length
    unsigned char firstSample;
} Sample;


extern OutputPlugin* get_oplugin_info();
static OutputPlugin* audioPI = NULL;
static int audio_exit_thread = 1;
static pthread_t audio_thread;


#define CHAN_COUNT        8
Channel channel[ CHAN_COUNT ];

#define MAX_VOL           64        // 64 keeps our table down to 16Kb
SAMPLE_TYPE vol_lookup[ MAX_VOL * 256 ];

int steptable[ 256 ];               // Pitch to stepping lookup


#define BUF_LEN 256*2
 
 
void* audio_loop( void* arg )
{
    Channel* chan;
    Channel* cend;
    char buf[ BUF_LEN ];
    SAMPLE_TYPE* begin;
    SAMPLE_TYPE* end;
//    int remain;
    unsigned int sample;
    register int dl;
    register int dr;
 
    end = (SAMPLE_TYPE*) (buf + BUF_LEN);
    cend = channel + CHAN_COUNT;

    while( ! audio_exit_thread )
    {
        begin = (SAMPLE_TYPE*) buf;
        while( begin < end )
        {
            // Mix all the channels together.

            dl = SAMPLE_ZERO;
            dr = SAMPLE_ZERO;

            chan = channel;
            for( ; chan < cend; chan++ )
            {
                // Check channel, if active.
                if( chan->begin )
                {
                    // Get the sample from the channel. 
                    sample = *chan->begin;

                    // Adjust volume accordingly.
                    dl += chan->lvol_table[ sample ];
                    dr += chan->rvol_table[ sample ];

                    // Increment sample pointer with pitch adjustment.
                    chan->step_remainder += chan->pitch_step;
                    chan->begin += chan->step_remainder >> 16;
                    chan->step_remainder &= 65535;

                    // Check whether we are done.
                    if( chan->begin >= chan->end )
                    {
                        chan->begin = 0;
                        //printf( "  channel done %d\n", chan );
                    }
                }
            }
            
#if 0   //SAMPLE_FORMAT
            if( dl > 127 ) dl = 127;
            else if( dl < -128 ) dl = -128;

            if( dr > 127 ) dr = 127;
            else if( dr < -128 ) dr = -128;
#else
            if( dl > 0x7fff ) dl = 0x7fff;
            else if( dl < -0x8000 ) dl = -0x8000;

            if( dr > 0x7fff ) dr = 0x7fff;
            else if( dr < -0x8000 ) dr = -0x8000;
#endif

            *begin++ = dl;
            *begin++ = dr;
        }

        // This write is expected to block.
        audioPI->write_audio( buf, BUF_LEN );
    }
 
    pthread_exit(NULL);
}


// Gets lump nums of the named sound.  Returns pointer which will be
// passed to I_StartSound() when you want to start an SFX.  Must be
// sure to pass this to UngetSoundEffect() so that they can be
// freed!


int I_GetSfxLumpNum(sfxinfo_t *sound)
{
  return W_GetNumForName(sound->lumpname);

}


// Id is unused.
// Data is a pointer to a Sample structure.
// Volume ranges from 0 to 127.
// Separation (orientation/stereo) ranges from 0 to 255.  128 is balanced.
// Pitch ranges from 0 to 255.  Normal is 128.
// Priority looks to be unused (always 0).

int I_StartSound( int id, void* data, int vol, int sep, int pitch, int priority)
{
    // Relative time order to find oldest sound.
    static unsigned int soundTime = 0;
    int chanId;
    Sample* sample;
    Channel* chan;
    int oldest;
    int i;
    

    // Find an empty channel, the oldest playing channel, or default to 0.
    // Currently ignoring priority.

    chanId = 0;
    oldest = soundTime;
    for( i = 0; i < CHAN_COUNT; i++ )
    {
        if( ! channel[ i ].begin )
        {
            chanId = i;
            break;
        }
        if( channel[ i ].time < oldest )
        {
            chanId = i;
            oldest = channel[ i ].time;
        }
    }

    sample = (Sample*) data;
    chan = &channel[ chanId ];

    I_UpdateSoundParams( chanId + 1, vol, sep, pitch );

    // begin must be set last because the audio thread will access the channel
    // once it is non-zero.  Perhaps this should be protected by a mutex.

    chan->pri = priority;
    chan->time = soundTime;
    chan->end = &sample->firstSample + sample->length;
    chan->begin = &sample->firstSample;

    soundTime++;

#if 0
    printf( "I_StartSound %d: v:%d s:%d p:%d pri:%d | %d %d %d %d\n",
            id, vol, sep, pitch, priority,
            chanId, chan->pitch_step, sample->a, sample->freq );
#endif

    return chanId + 1;
}

void I_StopSound(int handle)
{
    handle--;
    handle &= 7;
    channel[ handle ].begin = 0;
}

int I_SoundIsPlaying(int handle)
{
    handle--;
    handle &= 7;
    return( channel[ handle ].begin != 0 );
}

void I_UpdateSoundParams(int handle, int vol, int sep, int pitch)
{
    int lvol, rvol;
    Channel* chan;

    // Set left/right channel volume based on seperation.

    sep += 1;       // range 1 - 256
    lvol = vol - ((vol * sep * sep) >> 16); // (256*256);
    sep = sep - 257;
    rvol = vol - ((vol * sep * sep) >> 16);    


    // Sanity check, clamp volume.
    if( rvol < 0 )
    {
        //printf( "rvol out of bounds %d, id %d\n", rvol, handle );
        rvol = 0;
    }
    else if( rvol > 127 )
    {
        //printf( "rvol out of bounds %d, id %d\n", rvol, handle );
        rvol = 127;
    }
    
    if( lvol < 0 )
    {
        //printf( "lvol out of bounds %d, id %d\n", lvol, handle );
        lvol = 0;
    }
    else if( lvol > 127 )
    {
        //printf( "lvol out of bounds %d, id %d\n", lvol, handle );
        lvol = 127;
    }

    // Limit to MAX_VOL (64)
    lvol >>= 1;
    rvol >>= 1;

    handle--;
    handle &= 7;
    chan = &channel[ handle ];
    chan->pitch_step = steptable[ pitch ];
    chan->step_remainder = 0;
    chan->lvol_table = &vol_lookup[ lvol * 256 ];
    chan->rvol_table = &vol_lookup[ rvol * 256 ];
}

/*
 *
 *                                                      SOUND STARTUP STUFF
 *
 *
 */

// inits all sound stuff

void I_StartupSound (void)
{
    int ok;

    snd_MusicDevice = snd_SfxDevice = 0;

    if( M_CheckParm("--nosound") || M_CheckParm("-s") || M_CheckParm("-nosound") )
    {
        ST_Message("I_StartupSound: Sound Disabled.\n");
        return;
    }

    if (debugmode)
        ST_Message("I_StartupSound: Hope you hear a pop.\n");

    /* Using get_oplugin_info() from oss.c.  In the future this could
     load from a real shared library plugin. */
    audioPI = get_oplugin_info();
    if (!audioPI)
	return;
    audioPI->init();
    audioPI->about();
    
    ok = audioPI->open_audio( SAMPLE_FORMAT, SAMPLE_RATE, SAMPLE_CHANNELS );
    if( ok )
    {
        audio_exit_thread = 0;
        pthread_create( &audio_thread, NULL, audio_loop, NULL);
    }
    else
    {
        fprintf( stderr, "I_StartupSound: failed\n" );
    }
}

// shuts down all sound stuff

void I_ShutdownSound (void)
{
    if( audioPI )
    {
        if( ! audio_exit_thread )
        {
            audio_exit_thread = 1;
            pthread_join( audio_thread, NULL );
        }
        audioPI->close_audio();
    }
    audioPI = NULL;
}

void I_SetChannels(int channels)
{
    int v, j;
    int* steptablemid;

    // We always have CHAN_COUNT channels.

    for( j = 0; j < CHAN_COUNT; j++ )
    {
        channel[ j ].begin = 0;
        channel[ j ].end = 0;
        channel[ j ].time = 0;
    }

    // This table provides step widths for pitch parameters.
    steptablemid = steptable + 128;
    for( j = -128; j < 128; j++ )
    {
        steptablemid[ j ] = (int) (pow( 2.0, (j/64.0) ) * 65536.0);
    }

    // Generate the volume lookup tables.
    for( v = 0 ; v < MAX_VOL ; v++ )
    {
        for( j = 0; j < 256; j++ )
        {
            //vol_lookup[v*256+j] = 128 + ((v * (j-128)) / (MAX_VOL-1));

            // Turn the unsigned samples into signed samples.
#if 0   // SAMPLE_FORMAT
            vol_lookup[v*256+j] = (v * (j-128)) / (MAX_VOL-1);
#else
            vol_lookup[v*256+j] = (v * (j-128) * 256) / (MAX_VOL-1);
#endif

            //printf("vol_lookup[%d*256+%d] = %d\n", v, j, vol_lookup[v*256+j]);
        }
    }
}


/* EOF */

