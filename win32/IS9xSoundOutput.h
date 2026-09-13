/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

#ifndef IS9XSOUNDOUTPUT_H
#define IS9XSOUNDOUTPUT_H
#include "../port.h"
#include <vector>
#include <string>

/* IS9xSoundOutput
	Interface for the sound output.
*/

class IS9xSoundOutput {
public:
	// InitSoundOutput should initialize the sound output but not start playback
	virtual bool InitSoundOutput(void)=0;

	// DeInitSoundOutput should stop playback and uninitialize the output
	virtual void DeInitSoundOutput(void)=0;

	// SetupSound should apply the current sound settings for outputbuffers/devices and
	// (re)start playback
	virtual bool SetupSound()=0;

	// FlushSoundOutput should drop everything queued and leave the output ready
	// to take new samples, WITHOUT closing the device. Callers use it in place
	// of a close/open pair when the output format has not changed: an output
	// that is closed and reopened, or simply left with an empty queue for more
	// than ~125ms, has its stream restarted by the audio engine, and the next
	// write then blocks for about a third of a second. Backends should re-prime
	// the queue with silence so the stream stays alive across the caller's work.
	// Returning false means "I can't be reused" (device lost, never opened) and
	// the caller must fall back to a full open.
	virtual bool FlushSoundOutput() { return false; }

	// SetVolume should set a new volume level (between 0.0 and 1.0)
	virtual void SetVolume(double volume) = 0;

	// ProcessSound should queue new available samples into the Host sound 
    // system. If the sound system is callback based, ProcessSound should move
    // all samples into a buffer that the callback can read, using a critical 
    // section while accessing that buffer for thread-safety.
	virtual void ProcessSound()=0;

    // GetDeviceList should return a list of device strings that can be displayed in a dropdown
    virtual std::vector<std::wstring> GetDeviceList()
    {
        return std::vector<std::wstring>();
    }

    // FindDeviceIndex should try to find a matching index in the device list for a particular device string
    virtual int FindDeviceIndex(TCHAR *audio_device)
    {
        return 0;
    }

    // Pause/resume hooks. Pause is called just before the main loop stops feeding
    // samples (window move, menu open, focus loss); the backend can use it to push
    // a fade-out tail so the buffer queue drains to silence smoothly instead of
    // cutting off mid-waveform. Resume primes a fade-in for the next produced
    // buffer so audio doesn't kick back in on a non-zero sample.
    virtual void OnPauseRequested() {}
    virtual void OnResumeRequested() {}

};

#endif
