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

#include "common/array.h"
#include "common/debug.h"
#include "common/endian.h"
#include "common/events.h"
#include "common/file.h"
#include "common/list.h"
#include "common/str.h"
#include "common/system.h"

#include "engines/util.h"

#include "graphics/paletteman.h"
#include "graphics/surface.h"

#include "video/mpegps_decoder.h"

namespace Made {

MpgPlayer::MpgPlayer(MadeEngine *vm, Audio::Mixer *mixer)
	: _vm(vm), _mixer(mixer), _mpegDecoder(nullptr), _scaledSurface(nullptr),
	  _streamType(kMpgStreamUnknown), _paletteSaved(false),
	  _frameNumber(0), _frameDelay(33), _width(320), _height(240) {
	memset(_savedPalette, 0, sizeof(_savedPalette));
}

MpgPlayer::~MpgPlayer() {
	close();
}

bool MpgPlayer::resolveMpgPath(const char *filename, Common::Path &outPath) const {
	Common::String normalized(filename);
	for (uint i = 0; i < normalized.size(); ++i) {
		if (normalized[i] == '\\' || normalized[i] == ':')
			normalized.setChar('/', i);
	}
	while (normalized.hasPrefix("/"))
		normalized.deleteChar(0);

	Common::String baseNoExt = normalized;
	if (baseNoExt.hasSuffixIgnoreCase(".pmv") || baseNoExt.hasSuffixIgnoreCase(".mpg")) {
		baseNoExt = Common::String(baseNoExt.c_str(), baseNoExt.size() - 4);
	}

	Common::Array<Common::String> candidates;
	candidates.push_back(baseNoExt + ".mpg");

	uint32 slashPos = baseNoExt.findLastOf('/');
	if (slashPos != Common::String::npos) {
		Common::String fileOnly = baseNoExt.substr(slashPos + 1);
		candidates.push_back(fileOnly + ".mpg");
	}

	for (uint i = 0; i < candidates.size(); ++i) {
		Common::Path candidatePath(candidates[i], '/');
		if (Common::File::exists(candidatePath)) {
			outPath = candidatePath;
			return true;
		}
	}

	return false;
}

bool MpgPlayer::hasFile(const char *filename) const {
	Common::Path resolvedPath;
	return resolveMpgPath(filename, resolvedPath);
}

MpgStreamType MpgPlayer::inspectHeader(const Common::Path &path, uint16 &width, uint16 &height, uint16 &frameDelay) const {
	Common::File fd;
	if (!fd.open(path))
		return kMpgStreamUnknown;

	byte headerBuf[4096];
	uint32 bytesRead = fd.read(headerBuf, sizeof(headerBuf));
	fd.close();

	if (bytesRead < 12)
		return kMpgStreamUnknown;

	uint32 firstCode = READ_BE_UINT32(headerBuf);
	// Standard MPEG-1 files start with Pack Header (0x000001BA) or Sequence Header (0x000001B3)
	if (firstCode != 0x000001BA && firstCode != 0x000001B3 && (firstCode & 0xFFFFFF00) != 0x00000100)
		return kMpgStreamUnknown;

	width = 320;
	height = 240;
	frameDelay = 33;

	// Scan for the MPEG-1 Sequence Header (0x000001B3)
	for (uint32 i = 0; i <= bytesRead - 12; ++i) {
		if (READ_BE_UINT32(headerBuf + i) == 0x000001B3) {
			uint32 seqInfo = READ_BE_UINT32(headerBuf + i + 4);
			uint16 bitRateFlags = READ_BE_UINT16(headerBuf + i + 8);

			width = (seqInfo >> 20) & 0x0FFF;
			height = (seqInfo >> 8) & 0x0FFF;
			byte frameRateCode = seqInfo & 0x0F;

			static const uint16 kMpegFrameDelays[9] = {
				33, // 0: fallback
				42, // 1: 23.976 fps
				42, // 2: 24 fps
				40, // 3: 25 fps
				33, // 4: 29.97 fps
				33, // 5: 30 fps
				20, // 6: 50 fps
				17, // 7: 59.94 fps
				17  // 8: 60 fps
			};

			if (frameRateCode < ARRAYSIZE(kMpegFrameDelays))
				frameDelay = kMpegFrameDelays[frameRateCode];

			debug(2, "MpgPlayer::inspectHeader(%s): %dx%d, rateCode=%d (%d ms), flags=%04X",
				  path.toString().c_str(), width, height, frameRateCode, frameDelay, bitRateFlags);
			break;
		}
	}

	return kMpgStreamStandard;
}

bool MpgPlayer::load(const char *filename) {
	close();

	Common::Path mpgPath;
	if (!resolveMpgPath(filename, mpgPath)) {
		warning("MpgPlayer::load() Failed to find MPEG file for '%s'", filename);
		return false;
	}

	_streamType = inspectHeader(mpgPath, _width, _height, _frameDelay);
	if (_streamType == kMpgStreamUnknown) {
		warning("MpgPlayer::load() Unrecognized MPEG header in '%s'", mpgPath.toString().c_str());
		return false;
	}

	if (_streamType == kMpgStreamReelMagic) {
		warning("MpgPlayer::load() ReelMagic MPEG format is not yet implemented for '%s'", mpgPath.toString().c_str());
		return false;
	}

	// First cutscene after the opening credits finish
	Common::String upperName(filename);
	upperName.toUppercase();
	if (upperName.contains("FWIZ01X1")) {
		_vm->_openingCreditsOpen = false;
	}

	// Save the current 8-bit CLUT8 palette before switching screen to RGB
	if (_vm->_system->getScreenFormat().isCLUT8()) {
		_vm->_system->getPaletteManager()->grabPalette(_savedPalette, 0, 256);
		_paletteSaved = true;
	}

	Common::List<Graphics::PixelFormat> supportedFormats = _vm->_system->getSupportedFormats();
	for (Common::List<Graphics::PixelFormat>::iterator it = supportedFormats.begin(); it != supportedFormats.end(); ) {
		if (it->isCLUT8())
			it = supportedFormats.erase(it);
		else
			++it;
	}

	if (!supportedFormats.empty()) {
		initGraphics(_vm->_screen->getWidth(), _vm->_screen->getHeight(), supportedFormats);
	}

	_mpegDecoder = new Video::MPEGPSDecoder();
	if (!_mpegDecoder->loadFile(mpgPath)) {
		warning("MpgPlayer::load() Failed to initialize MPEG decoder for '%s'", mpgPath.toString().c_str());
		delete _mpegDecoder;
		_mpegDecoder = nullptr;

		if (_paletteSaved) {
			initGraphics(_vm->_screen->getWidth(), _vm->_screen->getHeight());
			_vm->_system->getPaletteManager()->setPalette(_savedPalette, 0, 256);
			_paletteSaved = false;
		}
		return false;
	}

	_frameNumber = 0;

	Graphics::Surface *screenSurf = _vm->_system->lockScreen();
	if (screenSurf) {
		screenSurf->fillRect(Common::Rect(0, 0, screenSurf->w, screenSurf->h), 0);
		_vm->_system->unlockScreen();
		_vm->_system->updateScreen();
	}

	_mpegDecoder->start();
	return true;
}

bool MpgPlayer::isPlaying() const {
	return _mpegDecoder != nullptr && _mpegDecoder->isPlaying() && !_mpegDecoder->endOfVideo();
}

bool MpgPlayer::decodeFrame() {
	if (!_mpegDecoder || _mpegDecoder->endOfVideo())
		return false;

	const Graphics::Surface *frame = _mpegDecoder->decodeNextFrame();
	if (!frame)
		return !_mpegDecoder->endOfVideo();

	Graphics::PixelFormat screenFormat = _vm->_system->getScreenFormat();
	const Graphics::Surface *blitSurf = frame;
	Graphics::Surface *convertedSurf = nullptr;

	if (frame->format != screenFormat) {
		convertedSurf = frame->convertTo(screenFormat);
		blitSurf = convertedSurf;
	}

	uint16 screenW = _vm->_screen->getWidth();
	uint16 screenH = _vm->_screen->getHeight();

	// Scale MPEG frame (e.g. 352x240 SIF) to fill the screen resolution (640x480 on Mac)
	if ((blitSurf->w != screenW || blitSurf->h != screenH) &&
		(screenFormat.bytesPerPixel == 2 || screenFormat.bytesPerPixel == 4)) {

		if (!_scaledSurface || _scaledSurface->w != screenW || _scaledSurface->h != screenH ||
			_scaledSurface->format != screenFormat) {
			if (_scaledSurface) {
				_scaledSurface->free();
				delete _scaledSurface;
			}
			_scaledSurface = new Graphics::Surface();
			_scaledSurface->create(screenW, screenH, screenFormat);
		}

		uint16 srcW = blitSurf->w;
		uint16 srcH = blitSurf->h;

		if (screenFormat.bytesPerPixel == 2) {
			for (uint16 y = 0; y < screenH; ++y) {
				uint16 srcY = (uint32)(y * srcH) / screenH;
				const uint16 *srcRow = (const uint16 *)blitSurf->getBasePtr(0, srcY);
				uint16 *dstRow = (uint16 *)_scaledSurface->getBasePtr(0, y);
				for (uint16 x = 0; x < screenW; ++x) {
					dstRow[x] = srcRow[(uint32)(x * srcW) / screenW];
				}
			}
		} else if (screenFormat.bytesPerPixel == 4) {
			for (uint16 y = 0; y < screenH; ++y) {
				uint16 srcY = (uint32)(y * srcH) / screenH;
				const uint32 *srcRow = (const uint32 *)blitSurf->getBasePtr(0, srcY);
				uint32 *dstRow = (uint32 *)_scaledSurface->getBasePtr(0, y);
				for (uint16 x = 0; x < screenW; ++x) {
					dstRow[x] = srcRow[(uint32)(x * srcW) / screenW];
				}
			}
		}

		blitSurf = _scaledSurface;
	}

	int16 dstX = MAX<int16>(0, (screenW - blitSurf->w) / 2);
	int16 dstY = MAX<int16>(0, (screenH - blitSurf->h) / 2);
	uint16 copyW = MIN<uint16>(blitSurf->w, screenW - dstX);
	uint16 copyH = MIN<uint16>(blitSurf->h, screenH - dstY);

	_vm->_system->copyRectToScreen(blitSurf->getPixels(), blitSurf->pitch,
								   dstX, dstY, copyW, copyH);

	if (convertedSurf) {
		convertedSurf->free();
		delete convertedSurf;
	}

	return true;
}

void MpgPlayer::waitForNextFrame() {
	if (_mpegDecoder) {
		uint32 waitTime = _mpegDecoder->getTimeToNextFrame();
		if (waitTime > 0)
			g_system->delayMillis(MIN<uint32>(waitTime, 100));
	}
}

void MpgPlayer::close() {
	if (_mpegDecoder) {
		_mpegDecoder->close();
		delete _mpegDecoder;
		_mpegDecoder = nullptr;
	}

	if (_scaledSurface) {
		_scaledSurface->free();
		delete _scaledSurface;
		_scaledSurface = nullptr;
	}

	if (_paletteSaved) {
		initGraphics(_vm->_screen->getWidth(), _vm->_screen->getHeight());
		_vm->_system->getPaletteManager()->setPalette(_savedPalette, 0, 256);
		_vm->_screen->showWorkScreen();
		_vm->_system->updateScreen();
		_paletteSaved = false;
	}

	_streamType = kMpgStreamUnknown;
}

bool MpgPlayer::handleEvents() {
	bool aborted = false;
	Common::Event event;
	while (_vm->_system->getEventManager()->pollEvent(event)) {
		switch (event.type) {
		case Common::EVENT_KEYDOWN:
			if (event.kbd.keycode == Common::KEYCODE_ESCAPE) {
				aborted = true;
				_vm->stopTextToSpeech();
			}
			break;
		default:
			break;
		}
	}
	return aborted;
}

bool MpgPlayer::play(const char *filename) {
	bool aborted = false;

	if (load(filename)) {
		while (!_vm->shouldQuit() && !aborted && isPlaying()) {
			if (!decodeFrame())
				break;

			waitForNextFrame();
			_vm->_system->updateScreen();

			_frameNumber++;
			aborted = handleEvents();
		}

		close();
	}

	return !aborted;
}

} // End of namespace Made