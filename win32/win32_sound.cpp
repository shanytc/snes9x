/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

#include "IS9xSoundOutput.h"
#include "../snes9x.h"
#include "../apu/apu.h"
#include "../sgb/sgb.h"
#include "wsnes9x.h"
#include "CXAudio2.h"
#include "CWaveOut.h"
#include "win32_sound.h"
#include "win32_display.h"

#define CLAMP(x, low, high) (((x) > (high)) ? (high) : (((x) < (low)) ? (low) : (x)))

// available sound output methods
CXAudio2 S9xXAudio2;
CWaveOut S9xWaveOut;

// Interface used to access the sound output
IS9xSoundOutput *S9xSoundOutput = &S9xXAudio2;

static double last_volume = 1.0;

/*  The format of the output device is fixed when it is opened, and opening it
    is expensive in a way that is not obvious: the audio engine starts a stream
    for it, and the first write afterwards blocks for about a third of a second.
    A ROM load calls ReInitSound and changes none of the format, so remember
    what the open device was set up with and flush it instead of reopening.
*/
static struct
{
    bool         open;
    int          driver;
    unsigned int rate;
    int          buffer_ms;
    TCHAR        device[MAX_AUDIO_NAME_LENGTH];
} s_open_device = { false, -1, 0, 0, { 0 } };

static bool OpenDeviceMatchesSettings()
{
    return s_open_device.open && S9xSoundOutput &&
           s_open_device.driver    == GUI.SoundDriver &&
           s_open_device.rate      == (unsigned int)Settings.SoundPlaybackRate &&
           s_open_device.buffer_ms == GUI.SoundBufferSize &&
           lstrcmp(s_open_device.device, GUI.AudioDevice) == 0;
}

// Record what the device ended up with, not what was asked for: a backend may
// snap the rate (XAudio2 rejects rates that aren't a whole number of samples
// per its 10ms quantum).
static void RememberOpenDevice()
{
    s_open_device.driver    = GUI.SoundDriver;
    s_open_device.rate      = (unsigned int)Settings.SoundPlaybackRate;
    s_open_device.buffer_ms = GUI.SoundBufferSize;
    lstrcpyn(s_open_device.device, GUI.AudioDevice, MAX_AUDIO_NAME_LENGTH);
    s_open_device.open      = true;
}

void S9xForgetOpenSoundDevice()
{
    s_open_device.open = false;
}

/*  ReInitSound
reinitializes the sound core with current settings
IN:
mode		-	0 disables sound output, 1 enables
-----
returns true if successful, false otherwise
*/
/*  ApplyLiveSoundSettings
applies every sound setting that does NOT define the device or its format:
input rate, mute, dynamic rate control, resampler kernel. Re-opening the
device costs a noticeable pause (CreateMasteringVoice has to acquire the
endpoint), so callers that only changed these must use this instead.
*/
void ApplyLiveSoundSettings()
{
	if (GUI.AutomaticInputRate)
	{
		int rate = WinGetAutomaticInputRate();
		if (rate)
			Settings.SoundInputRate = rate;
		else
		{
			GUI.AutomaticInputRate = false;
			Settings.SoundInputRate = 32040;
		}
	}

	Settings.SoundInputRate = CLAMP(Settings.SoundInputRate,31700, 32300);
	S9xSetSoundMute(GUI.Mute);
	// Re-derives the resampler ratio from the input/playback rates and
	// re-applies Settings.AudioFidelity. (1, 2) leaves the rate multiplier at
	// unity, matching the other non-audio-thread callers.
	S9xUpdateDynamicRate(1, 2);
}

bool ReInitSound()
{
	if (GUI.AVIOut)
		return false;

	ApplyLiveSoundSettings();
	Settings.SoundPlaybackRate = CLAMP(Settings.SoundPlaybackRate,8000, 48000);

	// Only close the device when the format it was opened with is changing.
	// S9xOpenSoundDevice below reuses it otherwise.
	if(S9xSoundOutput && !OpenDeviceMatchesSettings())
	{
		S9xSoundOutput->DeInitSoundOutput();
		S9xForgetOpenSoundDevice();
	}

    last_volume = 1.0;
    return S9xInitSound(25);
}

void CloseSoundDevice() {
	S9xSoundOutput->DeInitSoundOutput();
	S9xForgetOpenSoundDevice();
	S9xSetSamplesAvailableCallback(NULL,NULL);
}

/*  S9xOpenSoundDevice
called by S9xInitSound - initializes the currently selected sound output and
applies the current sound settings
-----
returns true if successful, false otherwise
*/
bool8 S9xOpenSoundDevice ()
{
	S9xSetSamplesAvailableCallback (NULL, NULL);

	// Driver, device, rate and buffer size all unchanged: the device that is
	// already open can serve, so flush it rather than closing and reopening.
	// Reopening restarts the audio engine's stream and the first write after
	// that blocks for ~1/3 s, which on a ROM load is a third of a second of
	// the new game not appearing. A backend that can't be reused says so and
	// we fall through to the full open (SetupSound tears down first).
	if (OpenDeviceMatchesSettings() && S9xSoundOutput->FlushSoundOutput())
	{
		S9xSetSamplesAvailableCallback (S9xSoundCallback, NULL);
		return TRUE;
	}
	S9xForgetOpenSoundDevice();

	// point the interface to the correct output object
	switch(GUI.SoundDriver) {
		case WIN_WAVEOUT_DRIVER:
			S9xSoundOutput = &S9xWaveOut;
			break;
		case WIN_XAUDIO2_SOUND_DRIVER:
			S9xSoundOutput = &S9xXAudio2;
			break;
		default:	// we default to WaveOut
			GUI.SoundDriver = WIN_WAVEOUT_DRIVER;
			S9xSoundOutput = &S9xWaveOut;
	}
	if(!S9xSoundOutput->InitSoundOutput())
		return false;
	
	if(!S9xSoundOutput->SetupSound())
		return false;

	RememberOpenDevice();
	S9xSetSamplesAvailableCallback (S9xSoundCallback, NULL);
	return true;
}

/*  S9xSoundCallback
called by the sound core to process generated samples
*/
void S9xSoundCallback(void *data)
{
	// Fast-forward release in GB/SGB modes: turbo banks the GB ring and the
	// SPC resampler full and winds up their rate controllers, which then
	// play back sped-up ("screechy") while they re-converge. Drop the
	// backlog and reset the controllers so playback resumes at normal
	// speed immediately.
	static bool wasTurbo = false;
	if (wasTurbo && !Settings.TurboMode &&
	    (Settings.SuperGameBoy || Settings.SGB_BIOSModeActive))
	{
		S9xSGBClearSamples();
		S9xClearSamples();
		S9xSpcResetDrc();
	}
	wasTurbo = Settings.TurboMode != FALSE;

	// only try to change volume if we actually need to switch it
	double current_volume = ((Settings.TurboMode || Settings.Rewinding) ? GUI.VolumeTurbo : GUI.VolumeRegular) / 100.;
	// master pre-amp rides on top, so it can push the level past unity
	current_volume *= S9xGainLinear(GUI.GainRegular);
	if (last_volume != current_volume) {
		S9xSoundOutput->SetVolume(current_volume);
		last_volume = current_volume;
	}

	S9xSoundOutput->ProcessSound();
}

/*  GetAvailableSoundDevices
returns a list of output devices available for the current output driver
*/
std::vector<std::wstring> GetAvailableSoundDevices()
{
    return S9xSoundOutput->GetDeviceList();
}

/*  FindAudioDeviceIndex
find an audio device that matches the currently configured audio device string
*/
int FindAudioDeviceIndex(TCHAR *audio_device)
{
    return S9xSoundOutput->FindDeviceIndex(audio_device);
}
