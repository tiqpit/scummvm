/* ScummVM - Scumm Interpreter
 * Copyright (C) 2001  Ludvig Strigeus
 * Copyright (C) 2001-2003 The ScummVM project
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.

 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.

 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
 *
 * $Header$
 *
 */

#include "stdafx.h"
#include "mixer.h"
#include "common/engine.h"	// for warning/error/debug
#include "common/file.h"
#include "common/util.h"

#define SOX_HACK

#ifdef SOX_HACK
//#define BUGGY_NEW_MP3_PLAYER
#include "rate.h"
#endif

class Channel {
protected:
	SoundMixer *_mixer;
	PlayingSoundHandle *_handle;
public:
	int _id;
	Channel(SoundMixer *mixer, PlayingSoundHandle *handle)
		: _mixer(mixer), _handle(handle), _id(-1) {
		assert(mixer);
	}
	virtual ~Channel() {
		if (_handle)
			*_handle = 0;
	}
	
	/* len indicates the number of sample *pairs*. So a value of
	   10 means that the buffer contains twice 10 sample, each
	   16 bits, for a total of 40 bytes.
	 */
	virtual void mix(int16 *data, uint len) = 0;
	void destroy() {
		for (int i = 0; i != SoundMixer::NUM_CHANNELS; i++)
			if (_mixer->_channels[i] == this)
				_mixer->_channels[i] = 0;
		delete this;
	}
	virtual bool isMusicChannel() = 0;
};

class ChannelRaw : public Channel {
	byte *_ptr;
	byte _flags;
#ifdef SOX_HACK
	RateConverter *_converter;
	AudioInputStream *_input;
#else
	uint32 _pos;
	uint32 _size;
	uint32 _fpSpeed;
	uint32 _fpPos;
	uint32 _realSize, _rate;
	byte *_loop_ptr;
	uint32 _loop_size;
#endif

public:
	ChannelRaw(SoundMixer *mixer, PlayingSoundHandle *handle, void *sound, uint32 size, uint rate, byte flags, int id);
	~ChannelRaw();

	void mix(int16 *data, uint len);
	bool isMusicChannel() {
		return false; // TODO: Is this correct? Or does anything use ChannelRaw for music?
	}
};

class ChannelStream : public Channel {
#ifdef SOX_HACK
	RateConverter *_converter;
	WrappedAudioInputStream *_input;
#else
	byte *_ptr;
	byte *_endOfData;
	byte *_endOfBuffer;
	byte *_pos;
	uint32 _fpSpeed;
	uint32 _fpPos;
	uint32 _bufferSize;
	uint32 _rate;
	byte _flags;
#endif
	bool _finished;

public:
	ChannelStream(SoundMixer *mixer, PlayingSoundHandle *handle, void *sound, uint32 size, uint rate, byte flags, uint32 buffer_size);
	~ChannelStream();

	void mix(int16 *data, uint len);
	void append(void *sound, uint32 size);
	bool isMusicChannel() {
		return true;
	}
	void finish() { _finished = true; }
};

#ifdef USE_MAD

class ChannelMP3Common : public Channel {
protected:
	byte *_ptr;
	struct mad_stream _stream;
	struct mad_frame _frame;
	struct mad_synth _synth;
	uint32 _posInFrame;
	uint32 _size;
	bool _initialized;

public:
	ChannelMP3Common(SoundMixer *mixer, PlayingSoundHandle *handle);
	~ChannelMP3Common();
};

class ChannelMP3 : public ChannelMP3Common {
	uint32 _position;

public:
	ChannelMP3(SoundMixer *mixer, PlayingSoundHandle *handle, File *file, uint size);

	void mix(int16 *data, uint len);
	bool isMusicChannel() { return false; }
};

#ifdef SOX_HACK
class ChannelMP3CDMusic : public Channel {
	RateConverter *_converter;
	MP3InputStream *_input;
#else
class ChannelMP3CDMusic : public ChannelMP3Common {
	uint32 _bufferSize;
	mad_timer_t _duration;
	File *_file;
#endif

public:
	ChannelMP3CDMusic(SoundMixer *mixer, PlayingSoundHandle *handle, File *file, mad_timer_t duration);
	~ChannelMP3CDMusic();

	void mix(int16 *data, uint len);
	bool isMusicChannel() { return true; }
};

#endif

#ifdef USE_VORBIS
class ChannelVorbis : public Channel {
#ifdef SOX_HACK
	RateConverter *_converter;
	AudioInputStream *_input;
#else
	OggVorbis_File *_ov_file;
	int _end_pos;
#endif
	bool _is_cd_track;

public:
	ChannelVorbis(SoundMixer *mixer, PlayingSoundHandle *handle, OggVorbis_File *ov_file, int duration, bool is_cd_track);
	~ChannelVorbis();

	void mix(int16 *data, uint len);
	bool isMusicChannel() {
		return _is_cd_track;
	}
};
#endif


SoundMixer::SoundMixer() {
	_syst = 0;
	_mutex = 0;

	_premixParam = 0;
	_premixProc = 0;
	int i = 0;

	_outputRate = 0;

	_globalVolume = 0;
	_musicVolume = 0;

	_paused = false;

	for (i = 0; i != NUM_CHANNELS; i++)
		_channels[i] = NULL;
}

SoundMixer::~SoundMixer() {
	_syst->clear_sound_proc();
	for (int i = 0; i != NUM_CHANNELS; i++) {
		delete _channels[i];
	}
	_syst->delete_mutex(_mutex);
}

int SoundMixer::newStream(void *sound, uint32 size, uint rate, byte flags, uint32 buffer_size) {
	StackLock lock(_mutex);
	return insertChannel(NULL, new ChannelStream(this, 0, sound, size, rate, flags, buffer_size));
}

void SoundMixer::appendStream(int index, void *sound, uint32 size) {
	StackLock lock(_mutex);

	ChannelStream *chan;
#if !defined(_WIN32_WCE) && !defined(__PALM_OS__)
	chan = dynamic_cast<ChannelStream *>(_channels[index]);
#else
	chan = (ChannelStream*)_channels[index];
#endif
	if (!chan) {
		error("Trying to append to a nonexistant stream %d", index);
	} else {
		chan->append(sound, size);
	}
}

void SoundMixer::endStream(int index) {
	StackLock lock(_mutex);

	ChannelStream *chan;
#if !defined(_WIN32_WCE) && !defined(__PALM_OS__)
	chan = dynamic_cast<ChannelStream *>(_channels[index]);
#else
	chan = (ChannelStream*)_channels[index];
#endif
	if (!chan) {
		error("Trying to end a nonexistant streamer : %d", index);
	} else {
		chan->finish();
	}
}

int SoundMixer::insertChannel(PlayingSoundHandle *handle, Channel *chan) {
	int index = -1;
	for (int i = 0; i != NUM_CHANNELS; i++) {
		if (_channels[i] == NULL) {
			index = i;
			break;
		}
	}
	if(index == -1) {
		warning("SoundMixer::out of mixer slots");
		delete chan;
		return -1;
	}

	_channels[index] = chan;
	if (handle)
		*handle = index + 1;
	return index;
}

int SoundMixer::playRaw(PlayingSoundHandle *handle, void *sound, uint32 size, uint rate, byte flags, int id) {
	StackLock lock(_mutex);

	// Prevent duplicate sounds
	if (id != -1) {
		for (int i = 0; i != NUM_CHANNELS; i++)
			if (_channels[i] != NULL && _channels[i]->_id == id)
				return -1;
	}

	return insertChannel(handle, new ChannelRaw(this, handle, sound, size, rate, flags, id));
}

#ifdef USE_MAD
int SoundMixer::playMP3(PlayingSoundHandle *handle, File *file, uint32 size) {
	StackLock lock(_mutex);
	return insertChannel(handle, new ChannelMP3(this, handle, file, size));
}
int SoundMixer::playMP3CDTrack(PlayingSoundHandle *handle, File *file, mad_timer_t duration) {
	StackLock lock(_mutex);
	return insertChannel(handle, new ChannelMP3CDMusic(this, handle, file, duration));
}
#endif

#ifdef USE_VORBIS
int SoundMixer::playVorbis(PlayingSoundHandle *handle, OggVorbis_File *ov_file, int duration, bool is_cd_track) {
	StackLock lock(_mutex);
	return insertChannel(handle, new ChannelVorbis(this, handle, ov_file, duration, is_cd_track));
}
#endif

void SoundMixer::mix(int16 *buf, uint len) {
	StackLock lock(_mutex);

	if (_premixProc && !_paused) {
		int i;
		_premixProc(_premixParam, buf, len);
		for (i = (len - 1); i >= 0; i--) {
			buf[2 * i] = buf[2 * i + 1] = buf[i];
		}
	} else {
		//  zero the buf out
		memset(buf, 0, 2 * len * sizeof(int16));
	}

	if (!_paused) {
		// now mix all channels
		for (int i = 0; i != NUM_CHANNELS; i++)
			if (_channels[i])
				_channels[i]->mix(buf, len);
	}
}

void SoundMixer::mixCallback(void *s, byte *samples, int len) {
	assert(s);
	assert(samples);
	// Len is the number of bytes in the buffer; we divide it by
	// four to get the number of samples (stereo 16 bit).
	((SoundMixer *)s)->mix((int16 *)samples, len >> 2);
}

bool SoundMixer::bindToSystem(OSystem *syst) {
	uint rate = (uint) syst->property(OSystem::PROP_GET_SAMPLE_RATE, 0);
	_outputRate = rate;
	_syst = syst;
	_mutex = _syst->create_mutex();

	if (rate == 0)
		error("OSystem returned invalid sample rate");

	return syst->set_sound_proc(mixCallback, this, OSystem::SOUND_16BIT);
}

void SoundMixer::stopAll() {
	StackLock lock(_mutex);
	for (int i = 0; i != NUM_CHANNELS; i++)
		if (_channels[i])
			_channels[i]->destroy();
}

void SoundMixer::stop(int index) {
	if ((index < 0) || (index >= NUM_CHANNELS)) {
		warning("soundMixer::stop has invalid index %d", index);
		return;
	}

	StackLock lock(_mutex);
	if (_channels[index])
		_channels[index]->destroy();
}

void SoundMixer::stopID(int id) {
	StackLock lock(_mutex);
	for (int i = 0; i != NUM_CHANNELS; i++) {
		if (_channels[i] != NULL && _channels[i]->_id == id) {
			_channels[i]->destroy();
			return;
		}
	}
}

void SoundMixer::stopHandle(PlayingSoundHandle handle) {
	StackLock lock(_mutex);
	
	// Simply ignore stop requests for handles of sounds that already terminated
	if (handle == 0)
		return;

	int index = handle - 1;
	if ((index < 0) || (index >= NUM_CHANNELS)) {
		warning("soundMixer::stopHandle has invalid index %d", index);
		return;
	}

	if (_channels[index])
		_channels[index]->destroy();
}


void SoundMixer::pause(bool paused) {
	_paused = paused;
}

bool SoundMixer::hasActiveSFXChannel() {
	// FIXME/TODO: We need to distinguish between SFX and music channels
	// (and maybe also voice) here to work properly in iMuseDigital
	// games. In the past that was achieve using the _beginSlots hack.
	// Since we don't have that anymore, it's not that simple anymore.
	StackLock lock(_mutex);
	for (int i = 0; i != NUM_CHANNELS; i++)
		if (_channels[i] && !_channels[i]->isMusicChannel())
			return true;
	return false;
}

void SoundMixer::setupPremix(void *param, PremixProc *proc) {
	StackLock lock(_mutex);
	_premixParam = param;
	_premixProc = proc;
}

void SoundMixer::setVolume(int volume) {
	// Check range
	if (volume > 256)
		volume = 256;
	else if (volume < 0)
		volume = 0;

	_globalVolume = volume;
}

void SoundMixer::setMusicVolume(int volume) {
	// Check range
	if (volume > 256)
		volume = 256;
	else if (volume < 0)
		volume = 0;

	_musicVolume = volume;
}

#ifdef SOX_HACK
#define clamped_add_16(a, b)	clampedAdd(a, b)
#else
/*
 * Class that performs cubic interpolation on integer data.
 * It is expected that the data is equidistant, i.e. all have the same
 * horizontal distance. This is obviously the case for sampled audio.
 */
class CubicInterpolator {
protected:
	int x0, x1, x2, x3;
	int a, b, c, d;

public:
	CubicInterpolator(int8 a0, int8 b0, int8 c0) : x0(2 * a0 - b0), x1(a0), x2(b0), x3(c0) {
		// We use a simple linear interpolation for x0
		updateCoefficients();
	}

	inline void feedData() {
		x0 = x1;
		x1 = x2;
		x2 = x3;
		x3 = 2 * x2 - x1;	// Simple linear interpolation
		updateCoefficients();
	}

	inline void feedData(int8 xNew) {
		x0 = x1;
		x1 = x2;
		x2 = x3;
		x3 = xNew;
		updateCoefficients();
	}

	/* t must be a 16.16 fixed point number between 0 and 1 */
	inline int interpolate(uint32 fpPos) {
		int result = 0;
		int t = fpPos >> 8;
		result = (a * t + b) >> 8;
		result = (result * t + c) >> 8;
		result = (result * t + d) >> 8;
		result = (result / 3 + 1) >> 1;

		return result;
	}

protected:
	inline void updateCoefficients() {
		a = ((-x0 * 2) + (x1 * 5) - (x2 * 4) + x3);
		b = ((x0 + x2 - (2 * x1)) * 6) << 8;
		c = ((-4 * x0) + x1 + (x2 * 4) - x3) << 8;
		d = (x1 * 6) << 8;
	}
};

static inline void clamped_add_16(int16& a, int b) {
	int val = a + b;

	if (val > 32767)
		a = 32767;
	else if (val < -32768)
		a = -32768;
	else
		a = val;
}

static void mix_signed_mono_8(int16 *data, uint &len, byte *&s, uint32 &fp_pos,
								int fp_speed, int volume, byte *s_end, bool reverse_stereo) {
	int inc = 1, result;
	CubicInterpolator interp(*s, *(s + 1), *(s + 2));

	do {
		do {
			result = interp.interpolate(fp_pos) * volume;

			clamped_add_16(*data++, result);
			clamped_add_16(*data++, result);

			fp_pos += fp_speed;
			inc = fp_pos >> 16;
			s += inc;
			len--;
			fp_pos &= 0x0000FFFF;
		} while (!inc && len && (s < s_end));

		if (s + 2 < s_end)
			interp.feedData(*(s + 2));
		else
			interp.feedData();

	} while (len && (s < s_end));
}

static void mix_unsigned_mono_8(int16 *data, uint &len, byte *&s, uint32 &fp_pos,
											int fp_speed, int volume, byte *s_end, bool reverse_stereo) {
	int inc = 1, result;
	CubicInterpolator interp(*s ^ 0x80, *(s + 1) ^ 0x80, *(s + 2) ^ 0x80);

	do {
		do {
			result = interp.interpolate(fp_pos) * volume;

			clamped_add_16(*data++, result);
			clamped_add_16(*data++, result);

			fp_pos += fp_speed;
			inc = fp_pos >> 16;
			s += inc;
			len--;
			fp_pos &= 0x0000FFFF;
		} while (!inc && len && (s < s_end));

		if (s + 2 < s_end)
			interp.feedData(*(s + 2) ^ 0x80);
		else
			interp.feedData();

	} while (len && (s < s_end));
}

static void mix_signed_stereo_8(int16 *data, uint &len, byte *&s, uint32 &fp_pos,
										int fp_speed, int volume, byte *s_end, bool reverse_stereo) {
	warning("Mixing stereo signed 8 bit is not supported yet ");
}
static void mix_unsigned_stereo_8(int16 *data, uint &len, byte *&s, uint32 &fp_pos,
										int fp_speed, int volume, byte *s_end, bool reverse_stereo) {
	int inc = 1;
	CubicInterpolator	left(*s ^ 0x80, *(s + 2) ^ 0x80, *(s + 4) ^ 0x80);
	CubicInterpolator	right(*(s + 1) ^ 0x80, *(s + 3) ^ 0x80, *(s + 5) ^ 0x80);

	do {
		do {
			if (!reverse_stereo) {
				clamped_add_16(*data++, left.interpolate(fp_pos) * volume);
				clamped_add_16(*data++, right.interpolate(fp_pos) * volume);
			} else {
				clamped_add_16(*data++, right.interpolate(fp_pos) * volume);
				clamped_add_16(*data++, left.interpolate(fp_pos) * volume);
			}

			fp_pos += fp_speed;
			inc = (fp_pos >> 16) << 1;
			s += inc;
			len--;
			fp_pos &= 0x0000FFFF;
		} while (!inc && len && (s < s_end));

		if (s + 5 < s_end) {
			left.feedData(*(s + 4) ^ 0x80);
			right.feedData(*(s + 5) ^ 0x80);
		} else {
			left.feedData();
			right.feedData();
		}

	} while (len && (s < s_end));
}
static void mix_signed_mono_16(int16 *data, uint &len, byte *&s, uint32 &fp_pos,
										 int fp_speed, int volume, byte *s_end, bool reverse_stereo) {
	do {
		int16 sample = ((int16)READ_BE_UINT16(s) * volume) / 256;
		fp_pos += fp_speed;

		clamped_add_16(*data++, sample);
		clamped_add_16(*data++, sample);

		s += (fp_pos >> 16) << 1;
		fp_pos &= 0x0000FFFF;
	} while ((--len) && (s < s_end));
}
static void mix_unsigned_mono_16(int16 *data, uint &len, byte *&s, uint32 &fp_pos,
										 int fp_speed, int volume, byte *s_end, bool reverse_stereo) {
	warning("Mixing mono unsigned 16 bit is not supported yet ");
}
static void mix_signed_stereo_16(int16 *data, uint &len, byte *&s, uint32 &fp_pos,
										 int fp_speed, int volume, byte *s_end, bool reverse_stereo) {
	do {
		int16 leftS = ((int16)READ_BE_UINT16(s) * volume) / 256;
		int16 rightS = ((int16)READ_BE_UINT16(s+2) * volume) / 256;
		fp_pos += fp_speed;

		if (!reverse_stereo) {
			clamped_add_16(*data++, leftS);
			clamped_add_16(*data++, rightS);
		} else {
			clamped_add_16(*data++, rightS);
			clamped_add_16(*data++, leftS);
		}
		s += (fp_pos >> 16) << 2;
		fp_pos &= 0x0000FFFF;
	} while ((--len) && (s < s_end));
}
static void mix_unsigned_stereo_16(int16 *data, uint &len, byte *&s, uint32 &fp_pos,
											 int fp_speed, int volume, byte *s_end, bool reverse_stereo) {
	warning("Mixing stereo unsigned 16 bit is not supported yet ");
}

typedef void MixProc(int16 *data, uint &len, byte *&s,
                      uint32 &fp_pos, int fp_speed, int volume,
                      byte *s_end, bool reverse_stereo);

static MixProc *mixer_helper_table[8] = {
	mix_signed_mono_8, mix_unsigned_mono_8,
	mix_signed_stereo_8, mix_unsigned_stereo_8,
	mix_signed_mono_16, mix_unsigned_mono_16,
	mix_signed_stereo_16, mix_unsigned_stereo_16
};

static int16 mixer_element_size[] = {
	1, 1,
	2, 2,
	2, 2,
	4, 4
};
#endif

/* RAW mixer */
ChannelRaw::ChannelRaw(SoundMixer *mixer, PlayingSoundHandle *handle, void *sound, uint32 size, uint rate, byte flags, int id)
	: Channel(mixer, handle) {
	_id = id;
	_ptr = (byte *)sound;
	_flags = flags;

#ifdef SOX_HACK
	
	// Create the input stream
	_input = makeLinearInputStream(flags, _ptr, size);
	// TODO: add support for SoundMixer::FLAG_REVERSE_STEREO

	// Get a rate converter instance
	_converter = makeRateConverter(rate, mixer->getOutputRate(), _input->isStereo());
#else
	_pos = 0;
	_fpPos = 0;
	_fpSpeed = (1 << 16) * rate / mixer->getOutputRate();
	_realSize = size;

	// adjust the magnitude to prevent division error
	while (size & 0xFFFF0000)
		size >>= 1, rate = (rate >> 1) + 1;

	_rate = rate;
	_size = size * mixer->getOutputRate() / rate;
	if (_flags & SoundMixer::FLAG_16BITS)
		_size = _size >> 1;
	if (_flags & SoundMixer::FLAG_STEREO)
		_size = _size >> 1;

	if (flags & SoundMixer::FLAG_LOOP) {
		_loop_ptr = _ptr;
		_loop_size = _size;
	}
#endif
}

ChannelRaw::~ChannelRaw() {
#ifdef SOX_HACK
	delete _converter;
	delete _input;
#endif
	if (_flags & SoundMixer::FLAG_AUTOFREE)
		free(_ptr);
}

void ChannelRaw::mix(int16 *data, uint len) {
#ifdef SOX_HACK
	assert(_input);
	assert(_converter);

	if (_input->eof()) {
		// TODO: call drain method
		// TODO: Looping
		destroy();
		return;
	}

	const int volume = _mixer->getVolume();
	uint tmpLen = len;
	_converter->flow(*_input, data, (st_size_t *) &tmpLen, volume);
#else
	byte *s, *end;

	if (len > _size)
		len = _size;
	_size -= len;

	s = _ptr + _pos;
	end = _ptr + _realSize;

	mixer_helper_table[_flags & 0x07] (data, len, s, _fpPos, _fpSpeed, _mixer->getVolume(), end, (_flags & SoundMixer::FLAG_REVERSE_STEREO) ? true : false);

	_pos = s - _ptr;

	if (_size <= 0) {
		if (_flags & SoundMixer::FLAG_LOOP) {
			_ptr = _loop_ptr;
			_size = _loop_size;
			_pos = 0;
			_fpPos = 0;
		} else {
			destroy();
		}
	}
#endif
}

#define WARP_WORKAROUND 50000

ChannelStream::ChannelStream(SoundMixer *mixer, PlayingSoundHandle *handle, void *sound, uint32 size, uint rate,
										 byte flags, uint32 buffer_size)
	: Channel(mixer, handle) {
	assert(size <= buffer_size);

#ifdef SOX_HACK
	
	// Create the input stream
	_input = makeWrappedInputStream(flags, buffer_size);
	_input->append((const byte *)sound, size);
	// TODO: add support for SoundMixer::FLAG_REVERSE_STEREO

	// Get a rate converter instance
	_converter = makeRateConverter(rate, mixer->getOutputRate(), _input->isStereo());
#else
	_flags = flags;
	_bufferSize = buffer_size;
	_ptr = (byte *)malloc(_bufferSize + WARP_WORKAROUND);
	memcpy(_ptr, sound, size);
	_endOfData = _ptr + size;
	_endOfBuffer = _ptr + _bufferSize;
	_pos = _ptr;
	_fpPos = 0;
	_fpSpeed = (1 << 16) * rate / mixer->getOutputRate();

	// adjust the magnitude to prevent division error
	while (size & 0xFFFF0000)
		size >>= 1, rate = (rate >> 1) + 1;

	_rate = rate;
#endif
	_finished = false;
}

ChannelStream::~ChannelStream() {
#ifdef SOX_HACK
	delete _converter;
	delete _input;
#else
	free(_ptr);
#endif
}

void ChannelStream::append(void *data, uint32 len) {
#ifdef SOX_HACK
	_input->append((const byte *)data, len);
#else
	if (_endOfData + len > _endOfBuffer) {
		/* Wrap-around case */
		uint32 size_to_end_of_buffer = _endOfBuffer - _endOfData;
		uint32 new_size = len - size_to_end_of_buffer;
		if ((_endOfData < _pos) || (_ptr + new_size >= _pos)) {
			debug(2, "Mixer full... Trying to not break too much (A)");
			return;
		}
		memcpy(_endOfData, (byte*)data, size_to_end_of_buffer);
		memcpy(_ptr, (byte *)data + size_to_end_of_buffer, new_size);
		_endOfData = _ptr + new_size;
	} else {
		if ((_endOfData < _pos) && (_endOfData + len >= _pos)) {
			debug(2, "Mixer full... Trying to not break too much (B)");
			return;
		}
		memcpy(_endOfData, data, len);
		_endOfData += len;
	}
#endif
}

void ChannelStream::mix(int16 *data, uint len) {
#ifdef SOX_HACK
	assert(_input);
	assert(_converter);

	if (_input->eof()) {
		// TODO: call drain method

		// Normally, the stream stays around even if all its data is used up.
		// This is in case more data is streamed into it. To make the stream
		// go away, one can either stop() it (which takes effect immediately,
		// ignoring any remaining sound data), or finish() it, which means
		// it will finish playing before it terminates itself.
		if (_finished) {
			destroy();
		}

		return;
	}

	const int volume = _mixer->getVolume();
	uint tmpLen = len;
	_converter->flow(*_input, data, (st_size_t *) &tmpLen, volume);
#else
	if (_pos == _endOfData) {
		// Normally, the stream stays around even if all its data is used up.
		// This is in case more data is streamed into it. To make the stream
		// go away, one can either stop() it (which takes effect immediately,
		// ignoring any remaining sound data), or finish() it, which means
		// it will finish playing before it terminates itself.
		if (_finished) {
			destroy();
		} else {
			// Since the buffer is empty now, reset the position to the start
			_pos = _endOfData = _ptr;
			_fpPos = 0;
		}

		return;
	}

	MixProc *mixProc = mixer_helper_table[_flags & 0x07];

	if (_pos < _endOfData) {
		mixProc(data, len, _pos, _fpPos, _fpSpeed, _mixer->getVolume(), _endOfData, (_flags & SoundMixer::FLAG_REVERSE_STEREO) ? true : false);
	} else {
		int wrapOffset = 0;
		const uint32 outLen = mixer_element_size[_flags & 0x07] * len;

		// see if we will wrap
		if (_pos + outLen > _endOfBuffer) {
			wrapOffset = _pos + outLen - _endOfBuffer;
			debug(2, "using wrap workaround for %d bytes", wrapOffset);
			assert(wrapOffset <= WARP_WORKAROUND);
			memcpy(_endOfBuffer, _ptr, wrapOffset);
		}

		mixProc(data, len, _pos, _fpPos, _fpSpeed, _mixer->getVolume(), _endOfBuffer + wrapOffset, (_flags & SoundMixer::FLAG_REVERSE_STEREO) ? true : false);

		// recover from wrap
		if (wrapOffset)
			_pos = _ptr + wrapOffset;

		// shouldn't happen anymore
		if (len != 0) {
			//FIXME: what is wrong ?
			warning("bad play sound in stream (wrap around)");
			_pos = _ptr;
			mixProc(data, len, _pos, _fpPos, _fpSpeed, _mixer->getVolume(), _endOfData, (_flags & SoundMixer::FLAG_REVERSE_STEREO) ? true : false);
		}
	}
#endif
}

#ifdef USE_MAD

ChannelMP3Common::ChannelMP3Common(SoundMixer *mixer, PlayingSoundHandle *handle)
	: Channel(mixer, handle) {
	mad_stream_init(&_stream);
#ifdef _WIN32_WCE
	// Lower sample rate to 11 kHz on WinCE if necessary
	if (_syst->property(OSystem::PROP_GET_SAMPLE_RATE, 0) != 22050)
		mad_stream_options(&_stream, MAD_OPTION_HALFSAMPLERATE);
#endif
	mad_frame_init(&_frame);
	mad_synth_init(&_synth);

	_initialized = false;
}

ChannelMP3Common::~ChannelMP3Common() {
	free(_ptr);
	mad_synth_finish(&_synth);
	mad_frame_finish(&_frame);
	mad_stream_finish(&_stream);
}

static inline int scale_sample(mad_fixed_t sample) {
	/* round */
	sample += (1L << (MAD_F_FRACBITS - 16));

	/* clip */
	if (sample > MAD_F_ONE - 1)
		sample = MAD_F_ONE - 1;
	else if (sample < -MAD_F_ONE)
		sample = -MAD_F_ONE;

	/* quantize and scale to not saturate when mixing a lot of channels */
	return sample >> (MAD_F_FRACBITS + 1 - 16);
}

ChannelMP3::ChannelMP3(SoundMixer *mixer, PlayingSoundHandle *handle, File *file, uint size)
	: ChannelMP3Common(mixer, handle) {
	_posInFrame = 0xFFFFFFFF;
	_position = 0;
	_ptr = (byte *)malloc(size + MAD_BUFFER_GUARD);

	_size = file->read(_ptr, size);
}

void ChannelMP3::mix(int16 *data, uint len) {
	const int volume = _mixer->getVolume();

	// Exit if all data is used up (this also covers the case were reading from the file failed).
	if (_position >= _size) {
		destroy();
		return;
	}

	while (1) {

		int16 sample;
		while ((_posInFrame < _synth.pcm.length) && (len > 0)) {
			sample = (int16)((scale_sample(_synth.pcm.samples[0][_posInFrame]) * volume) / 256);
			clamped_add_16(*data++, sample);
			if (_synth.pcm.channels > 1)
				sample = (int16)((scale_sample(_synth.pcm.samples[1][_posInFrame]) * volume) / 256);
			clamped_add_16(*data++, sample);
			len--;
			_posInFrame++;
		}
		if (len == 0)
			return;

		if (_position >= _size) {
			destroy();
			return;
		}

		mad_stream_buffer(&_stream, _ptr + _position,
											_size + MAD_BUFFER_GUARD - _position);

		if (mad_frame_decode(&_frame, &_stream) == -1) {
			/* End of audio... */
			if (_stream.error == MAD_ERROR_BUFLEN) {
				destroy();
				return;
			} else if (!MAD_RECOVERABLE(_stream.error)) {
				error("MAD frame decode error !");
			}
		}
		mad_synth_frame(&_synth, &_frame);
		_posInFrame = 0;
		_position = _stream.next_frame - _ptr;
	}
}

#define MP3CD_BUFFERING_SIZE 131072

#ifdef SOX_HACK
ChannelMP3CDMusic::ChannelMP3CDMusic(SoundMixer *mixer, PlayingSoundHandle *handle, File *file, mad_timer_t duration) 
	: Channel(mixer, handle) {
	// Create the input stream
	_input = new MP3InputStream(file, duration);

	// Get a rate converter instance
printf("ChannelMP3CDMusic: inrate %d, outrate %d, stereo %d\n", _input->getRate(), mixer->getOutputRate(), _input->isStereo());
	_converter = makeRateConverter(_input->getRate(), mixer->getOutputRate(), _input->isStereo());
}
#else
ChannelMP3CDMusic::ChannelMP3CDMusic(SoundMixer *mixer, PlayingSoundHandle *handle, File *file, mad_timer_t duration)
	: ChannelMP3Common(mixer, handle) {
	_file = file;
	_duration = duration;
	_bufferSize = MP3CD_BUFFERING_SIZE;
	_ptr = (byte *)malloc(MP3CD_BUFFERING_SIZE);
}
#endif

ChannelMP3CDMusic::~ChannelMP3CDMusic() {
#ifdef SOX_HACK
	delete _converter;
	delete _input;
#endif
}

void ChannelMP3CDMusic::mix(int16 *data, uint len) {
#ifdef SOX_HACK
	assert(_input);
	assert(_converter);

	if (_input->eof()) {
		// TODO: call drain method
		destroy();
		return;
	}

	const int volume = _mixer->getVolume();
	uint tmpLen = len;
	_converter->flow(*_input, data, &tmpLen, volume);
#else
	mad_timer_t frame_duration;
	const int volume = _mixer->getMusicVolume();

	if (!_initialized) {
		// just skipped
		memset(_ptr, 0, _bufferSize);
		_size = _file->read(_ptr, _bufferSize);
		if (_size <= 0) {
			debug(1, "Failed to read MP3 data during channel initialisation !");
			destroy();
			return;
		}
		// Resync
		mad_stream_buffer(&_stream, _ptr, _size);
		
		// Skip the first two frames (see ChannelMP3::ChannelMP3 for an explanation)
		int skip_loop = 2;
		while (skip_loop != 0) {
			if (mad_frame_decode(&_frame, &_stream) == 0) {
				/* Do not decrease duration - see if it's a problem */
				skip_loop--;
			} else {
				if (!MAD_RECOVERABLE(_stream.error)) {
					debug(1, "Unrecoverable error while skipping !");
					destroy();
					return;
				}
			}
		}
		
		// FIXME: Fingolfin asks: why is this call to mad_synth_frame
		// necessary? Or rather, *is* it actually necessary?
		mad_synth_frame(&_synth, &_frame);

		// We are supposed to be in synch
		mad_frame_mute(&_frame);
		mad_synth_mute(&_synth);
		// Resume decoding
		if (mad_frame_decode(&_frame, &_stream) == 0) {
			_posInFrame = 0;
			_initialized = true;
		} else {
			debug(1, "Cannot resume decoding");
			destroy();
			return;
		}
	}

	while (1) {
	
		// TODO: Check _synth.pcm.samplerate and perform rate conversion of appropriate
		// TODO: Check _synth.pcm.channels to support stereo

		// Get samples, play samples ...
		int16 sample;
		while ((_posInFrame < _synth.pcm.length) && (len > 0)) {
			sample = (int16)((scale_sample(_synth.pcm.samples[0][_posInFrame]) * volume) / 256);
			clamped_add_16(*data++, sample);
			if (_synth.pcm.channels > 1)
				sample = (int16)((scale_sample(_synth.pcm.samples[1][_posInFrame]) * volume) / 256);
			clamped_add_16(*data++, sample);
			len--;
			_posInFrame++;
		}
		if (len == 0)
			return;

		// See if we have finished
		// May be incorrect to check the size at the end of a frame but I suppose
		// they are short enough :)
		frame_duration = _frame.header.duration;
		mad_timer_negate(&frame_duration);
		mad_timer_add(&_duration, frame_duration);
		
		if (mad_timer_compare(_duration, mad_timer_zero) <= 0) {
			destroy();
			return;
		}
		if (mad_frame_decode(&_frame, &_stream) == -1) {
			if (_stream.error == MAD_ERROR_BUFLEN) {
				int not_decoded;

				if (!_stream.next_frame) {
					not_decoded = 0;
					memset(_ptr, 0, _bufferSize + MAD_BUFFER_GUARD);
				} else {
					not_decoded = _stream.bufend - _stream.next_frame;
					memcpy(_ptr, _stream.next_frame, not_decoded);
				}
				_size = _file->read(_ptr + not_decoded, _bufferSize - not_decoded);
				if (_size <= 0) {
					return;
				}
				_stream.error = (enum mad_error)0;
				// Restream
				mad_stream_buffer(&_stream, _ptr, _size + not_decoded);
				if (mad_frame_decode(&_frame, &_stream) == -1) {
					debug(1, "Error %d decoding after restream !", _stream.error);
				}
			} else if (!MAD_RECOVERABLE(_stream.error)) {
				error("MAD frame decode error in MP3 CDMUSIC !");
			}
		}
		mad_synth_frame(&_synth, &_frame);
		_posInFrame = 0;
	}
#endif
}

#endif

#ifdef USE_VORBIS
ChannelVorbis::ChannelVorbis(SoundMixer *mixer, PlayingSoundHandle *handle, OggVorbis_File *ov_file, int duration, bool is_cd_track)
	: Channel(mixer, handle) {
#ifdef SOX_HACK
	vorbis_info *vi;
	
	// Create the input stream
	_input = new VorbisInputStream(ov_file, duration);

	// Get a rate converter instance
	vi = ov_info(ov_file, -1);
	assert(vi->channels == 1 || vi->channels == 2);
	_converter = makeRateConverter(vi->rate, mixer->getOutputRate(), _input->isStereo());
#else
	_ov_file = ov_file;

	if (duration)
		_end_pos = ov_pcm_tell(ov_file) + duration;
	else
		_end_pos = 0;
#endif
	_is_cd_track = is_cd_track;
}

ChannelVorbis::~ChannelVorbis() {
#ifdef SOX_HACK
	delete _converter;
	delete _input;
#endif
}

#ifdef CHUNKSIZE
#define VORBIS_TREMOR
#endif

void ChannelVorbis::mix(int16 *data, uint len) {

#ifdef SOX_HACK
	assert(_input);
	assert(_converter);

	if (_input->eof()) {
		// TODO: call drain method
		destroy();
		return;
	}

	const int volume = _mixer->getVolume();
	uint tmpLen = len;
	_converter->flow(*_input, data, &tmpLen, volume);
#else
	if (_end_pos > 0 && ov_pcm_tell(_ov_file) >= _end_pos) {
		destroy();
		return;
	}

	int channels = ov_info(_ov_file, -1)->channels;
	uint len_left = len * channels * 2;
	int16 *samples = new int16[len_left / 2];
	char *read_pos = (char *) samples;
	bool eof_flag = false;
	int volume = isMusicChannel() ? _mixer->getMusicVolume() : _mixer->getVolume();

	// Read the samples
	while (len_left > 0) {
		long result = ov_read(_ov_file, read_pos, len_left,
#ifndef VORBIS_TREMOR
#ifdef SCUMM_BIG_ENDIAN
				      1,
#else
				      0,
#endif
				      2, 1,
#endif
					  NULL);
		if (result == 0) {
			eof_flag = true;
			memset(read_pos, 0, len_left);
			break;
		} else if (result == OV_HOLE) {
			// Possibly recoverable, just warn about it
			warning("Corrupted data in Vorbis file");
		} else if (result < 0) {
			debug(1, "Decode error %d in Vorbis file", result);
			eof_flag = true;
			memset(read_pos, 0, len_left);
			break;
		} else {
			len_left -= result;
			read_pos += result;
		}
	}

	// Mix the samples in
	for (uint i = 0; i < len; i++) {
		int16 sample = (int16)(samples[i * channels] * volume / 256);
		clamped_add_16(*data++, sample);
		if (channels > 1)
			sample = (int16)(samples[i * channels + 1] * volume / 256);
		clamped_add_16(*data++, sample);
	}

	delete [] samples;

	if (eof_flag)
		destroy();
#endif
}

#endif
