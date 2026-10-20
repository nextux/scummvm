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

#include "made/mpgplayer.h"
#include "made/made.h"
#include "made/screen.h"
#include "made/script.h"

#include "common/archive.h"
#include "common/debug.h"
#include "common/endian.h"
#include "common/events.h"
#include "common/file.h"
#include "common/str.h"
#include "common/system.h"
#include "common/util.h"

#include "audio/mixer.h"
#include "engines/util.h"
#include "graphics/paletteman.h"
#include "graphics/pixelformat.h"
#include "graphics/surface.h"
#include "video/mpegps_decoder.h"

namespace Made {

MpgPlayer::MpgPlayer(MadeEngine *vm, Audio::Mixer *mixer) : _vm(vm), _mixer(mixer) {
}

MpgPlayer::~MpgPlayer() {
}

Common::Path MpgPlayer::resolveMpgPath(const char *filename) const {
	Common::String name(filename);
	for (uint i = 0; i < name.size(); ++i) {
		if (name[i] == '\\')
			name.setChar('/', i);
	}

	if (name.size() >= 4 && name.substr(name.size() - 4).equalsIgnoreCase(".pmv")) {
		name = name.substr(0, name.size() - 4) + ".MPG";
	}

	Common::Path candidates[3];
	candidates[0] = Common::Path(name, '/');

	Common::String upperName = name;
	upperName.toUppercase();
	candidates[1] = Common::Path(upperName, '/');

	Common::String lowerName = name;
	lowerName.toLowercase();
	candidates[2] = Common::Path(lowerName, '/');

	for (int i = 0; i < 3; ++i) {
		if (SearchMan.hasFile(candidates[i]))
			return candidates[i];
	}

	int32 lastSlash = -1;
	for (int32 i = (int32)name.size() - 1; i >= 0; --i) {
		if (name[i] == '/') {
			lastSlash = i;
			break;
		}
	}
	if (lastSlash >= 0) {
		Common::String baseName = name.substr(lastSlash + 1);
		Common::Path basePath(baseName, '/');
		if (SearchMan.hasFile(basePath))
			return basePath;
	}

	return candidates[0];
}

MpgStreamType MpgPlayer::inspectHeader(Common::SeekableReadStream &stream) const {
	int64 origPos = stream.pos();
	stream.seek(0);

	if (stream.size() < 4) {
		stream.seek(origPos);
		return kMpgStreamUnknown;
	}

	uint32 startCode = stream.readUint32BE();
	stream.seek(origPos);

	if (startCode == 0x000001BA || startCode == 0x000001B3) {
		return kMpgStreamSystemOrVideo;
	}

	if ((startCode & 0xFFFFFF00) == 0x00000100) {
		return kMpgStreamReelMagic;
	}

	return kMpgStreamUnknown;
}

bool MpgPlayer::play(const char *filename, int16 callbackScriptObj) {
	Common::Path mpgPath = resolveMpgPath(filename);

	Common::File probeFile;
	if (!probeFile.open(mpgPath)) {
		warning("MpgPlayer::play(): Failed to open MPEG file '%s' (from '%s')",
				mpgPath.toString().c_str(), filename);
		return false;
	}

	MpgStreamType streamType = inspectHeader(probeFile);
	probeFile.close();

	if (streamType == kMpgStreamUnknown) {
		warning("MpgPlayer::play(): Unrecognized MPEG header in '%s'", mpgPath.toString().c_str());
		return false;
	}

	if (streamType == kMpgStreamReelMagic) {
		warning("MpgPlayer::play(): ReelMagic MPEG format in '%s' is not yet supported", mpgPath.toString().c_str());
		return false;
	}

	Common::String baseCheck(filename);
	if (baseCheck.equalsIgnoreCase("FWIZ01X1.PMV") || baseCheck.equalsIgnoreCase("FWIZ01X1.MPG")) {
		_vm->_openingCreditsOpen = false;
	}

	Video::MPEGPSDecoder decoder;
	if (!decoder.loadFile(mpgPath)) {
		warning("MpgPlayer::play(): Video::MPEGPSDecoder failed to load '%s'", mpgPath.toString().c_str());
		return false;
	}

	const uint16 screenW = _vm->_screen->getWidth();
	const uint16 screenH = _vm->_screen->getHeight();

	byte savedPalette[256 * 3];
	_vm->_system->getPaletteManager()->grabPalette(savedPalette, 0, 256);

	Common::List<Graphics::PixelFormat> supportedFormats = _vm->_system->getSupportedFormats();
	Graphics::PixelFormat videoFormat = decoder.getPixelFormat();
	if (videoFormat.isCLUT8() && !supportedFormats.empty()) {
		for (Common::List<Graphics::PixelFormat>::const_iterator it = supportedFormats.begin();
			 it != supportedFormats.end(); ++it) {
			if (!it->isCLUT8()) {
				videoFormat = *it;
				break;
			}
		}
	}

	initGraphics(screenW, screenH, &videoFormat);
	Graphics::PixelFormat screenFormat = _vm->_system->getScreenFormat();

	_vm->_system->fillScreen(0);
	_vm->_system->updateScreen();

	ScriptInterpreter si(_vm);
	int16 frameNumber = 0;
	bool aborted = false;

	decoder.start();

	while (!_vm->shouldQuit() && !aborted && !decoder.endOfVideo()) {
		if (decoder.needsUpdate()) {
			// If called via sfMovieCall, execute the per-frame script callback
			if (callbackScriptObj != 0) {
				int16 playing = si.runScript(callbackScriptObj, 1, &frameNumber);
				debug(3, "MpgPlayer::play(): callback script %04X frame %d returned %d",
					  callbackScriptObj, frameNumber, playing);
				if (!playing) {
					aborted = true;
					break;
				}
			}

			const Graphics::Surface *frame = decoder.decodeNextFrame();
			if (frame) {
				if (frameNumber == 0) {
					debug(1, "MpgPlayer::play(): '%s' video %dx%d -> screen %dx%d (callback=%04X)",
						  mpgPath.toString().c_str(), frame->w, frame->h, screenW, screenH, callbackScriptObj);
				}

				Graphics::Surface *converted = nullptr;
				const Graphics::Surface *outFrame = frame;

				if (frame->format != screenFormat) {
					converted = frame->convertTo(screenFormat);
					outFrame = converted;
				}

				if (outFrame->w != screenW || outFrame->h != screenH) {
					Graphics::Surface *scaled = outFrame->scale(screenW, screenH);
					_vm->_system->copyRectToScreen(scaled->getPixels(), scaled->pitch,
												   0, 0, scaled->w, scaled->h);
					scaled->free();
					delete scaled;
				} else {
					_vm->_system->copyRectToScreen(outFrame->getPixels(), outFrame->pitch,
												   0, 0, outFrame->w, outFrame->h);
				}

				if (converted) {
					converted->free();
					delete converted;
				}

				_vm->_system->updateScreen();
				frameNumber++;
			}
		}

		// When playing via sfPlayMovie (no script callback), check ESC directly
		if (callbackScriptObj == 0) {
			Common::Event event;
			while (_vm->_system->getEventManager()->pollEvent(event)) {
				if (event.type == Common::EVENT_KEYDOWN && event.kbd.keycode == Common::KEYCODE_ESCAPE) {
					aborted = true;
					_vm->stopTextToSpeech();
					break;
				}
			}
		}

		if (!aborted) {
			uint32 waitMs = MIN<uint32>(decoder.getTimeToNextFrame(), 10);
			if (waitMs > 0)
				_vm->_system->delayMillis(waitMs);
		}
	}

	decoder.close();

	// Restore 8-bit CLUT8 mode and palette for the MADE engine
	Graphics::PixelFormat clut8 = Graphics::PixelFormat::createFormatCLUT8();
	initGraphics(screenW, screenH, &clut8);
	_vm->_system->getPaletteManager()->setPalette(savedPalette, 0, 256);
	_vm->_screen->showWorkScreen();
	_vm->_system->updateScreen();

	if (callbackScriptObj != 0)
		return true;

	return !aborted;
}

} // End of namespace Made