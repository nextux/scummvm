/* ScummVM - Graphic Adventure Engine
 *
 * ScummVM is the legal property of its developers, whose names
 * are too numerous to list here. Please refer to the COPYRIGHT
 * file distributed with this source distribution.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#ifndef MADE_MPGPLAYER_H
#define MADE_MPGPLAYER_H

#include "audio/mixer.h"
#include "common/path.h"
#include "common/scummsys.h"

namespace Graphics {
struct Surface;
}

namespace Video {
class MPEGPSDecoder;
}

namespace Made {

class MadeEngine;

enum MpgStreamType {
	kMpgStreamUnknown = 0,
	kMpgStreamStandard = 1,  // Standard ISO/IEC 11172-2 MPEG-1 (Macintosh MPEG release)
	kMpgStreamReelMagic = 2  // Future: DOS ReelMagic MPEG variant
};

class MpgPlayer {
public:
	MpgPlayer(MadeEngine *vm, Audio::Mixer *mixer);
	~MpgPlayer();

	bool hasFile(const char *filename) const;

	bool load(const char *filename);
	bool decodeFrame();
	void waitForNextFrame();
	void close();

	bool play(const char *filename);
	bool isPlaying() const;

	uint32 getFrameNumber() const { return _frameNumber; }
	void incrementFrameNumber() { _frameNumber++; }
	uint16 getFrameDelay() const { return _frameDelay; }

protected:
	MadeEngine *_vm;
	Audio::Mixer *_mixer;

	Video::MPEGPSDecoder *_mpegDecoder;
	Graphics::Surface *_scaledSurface;
	MpgStreamType _streamType;
	bool _paletteSaved;
	byte _savedPalette[768];

	uint32 _frameNumber;
	uint16 _frameDelay;
	uint16 _width;
	uint16 _height;

	bool resolveMpgPath(const char *filename, Common::Path &outPath) const;
	MpgStreamType inspectHeader(const Common::Path &path, uint16 &width, uint16 &height, uint16 &frameDelay) const;
	bool handleEvents();
};

} // End of namespace Made

#endif /* MADE_MPGPLAYER_H */