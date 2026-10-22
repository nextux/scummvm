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
#include "made/database.h"
#include "made/made.h"
#include "made/resource.h"
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
#include "graphics/cursorman.h"
#include "graphics/paletteman.h"
#include "graphics/pixelformat.h"
#include "graphics/surface.h"
#include "video/mpegps_decoder.h"

namespace Made {

static void drawCharToRGBSurface(Graphics::Surface *surf, FontResource *font, uint c, int16 x, int16 y, uint32 pixel, uint8 bytesPerPixel) {
	if (!surf || !font)
		return;

	uint width = font->getCharWidth(c);
	uint height = font->getHeight();
	byte *charData = font->getChar(c);
	if (!charData)
		return;

	for (uint yc = 0; yc < height; ++yc) {
		int16 sy = y + (int16)yc;
		if (sy < 0 || sy >= surf->h)
			continue;
		byte p = charData[yc];
		for (uint xc = 0; xc < width; ++xc) {
			int16 sx = x + (int16)xc;
			if ((p & 0x80) && sx >= 0 && sx < surf->w) {
				if (bytesPerPixel == 2)
					*((uint16 *)surf->getBasePtr(sx, sy)) = (uint16)pixel;
				else if (bytesPerPixel == 4)
					*((uint32 *)surf->getBasePtr(sx, sy)) = pixel;
			}
			p <<= 1;
		}
	}
}

MpgPlayer::MpgPlayer(MadeEngine *vm, Audio::Mixer *mixer) : _vm(vm), _mixer(mixer) {
}

MpgPlayer::~MpgPlayer() {
}

void MpgPlayer::addFaceIcon(int16 picId, int16 x, int16 y) {
	for (uint i = 0; i < _faceIcons.size(); ++i) {
		if (_faceIcons[i].x == x && _faceIcons[i].y == y) {
			_faceIcons[i].picId = picId;
			return;
		}
	}
	FaceIconOverlay icon;
	icon.picId = picId;
	icon.x = x;
	icon.y = y;
	_faceIcons.push_back(icon);
}

void MpgPlayer::clearFaceIcons() {
	_faceIcons.clear();
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

bool MpgPlayer::play(const char *filename, int16 callbackScriptObj, int16 maxFrames) {
	Common::Path mpgPath = resolveMpgPath(filename);

	Common::File probeFile;
	if (!probeFile.open(mpgPath)) {
		warning("MpgPlayer::play(): Failed to open MPEG file '%s' (from '%s')",
				mpgPath.toString().c_str(), filename);
		clearFaceIcons();
		return false;
	}

	MpgStreamType streamType = inspectHeader(probeFile);
	probeFile.close();

	if (streamType == kMpgStreamUnknown) {
		warning("MpgPlayer::play(): Unrecognized MPEG header in '%s'", mpgPath.toString().c_str());
		clearFaceIcons();
		return false;
	}

	if (streamType == kMpgStreamReelMagic) {
		warning("MpgPlayer::play(): ReelMagic MPEG format in '%s' is not yet supported", mpgPath.toString().c_str());
		clearFaceIcons();
		return false;
	}

	Common::String baseCheck(filename);
	if (baseCheck.equalsIgnoreCase("FWIZ01X1.PMV") || baseCheck.equalsIgnoreCase("FWIZ01X1.MPG")) {
		_vm->_openingCreditsOpen = false;
	}

	Video::MPEGPSDecoder decoder;
	if (!decoder.loadFile(mpgPath)) {
		warning("MpgPlayer::play(): Video::MPEGPSDecoder failed to load '%s'", mpgPath.toString().c_str());
		clearFaceIcons();
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

	// Keep the 8-bit mouse cursor visible when switching to a true-color screen format
	CursorMan.replaceCursorPalette(savedPalette, 0, 256);

	_vm->_system->fillScreen(0);
	_vm->_system->updateScreen();

	ScriptInterpreter si(_vm);
	int16 frameNumber = 0;
	bool aborted = false;
	
	// In Return to Zork (Macintosh MPEG), FMPVFO00.MPG contains both the fly-off and
 	// the monitor-shattering attack. When the rock is thrown (location 4152 / VultureFlys1),
 	// the video should exit at frame 47 so the vulture flies past without cracking the screen.
 	if (_vm->getGameID() == GID_RTZ && maxFrames <= 0) {
 	Common::Path resolved = resolveMpgPath(filename);
 	Common::String baseName = resolved.getLastComponent().toString();
	
	if (baseName.equalsIgnoreCase("FMPVFO00.MPG") && _vm->_dat->getVar(9) == 4152) {
	maxFrames = 47;
	}
	}
	decoder.start();

	while (!_vm->shouldQuit() && !aborted && !decoder.endOfVideo()) {
		if (maxFrames > 0 && frameNumber >= maxFrames) {
		break;
		}
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

				Graphics::Surface *scaled = nullptr;
				Graphics::Surface *drawSurf = nullptr;

				if (outFrame->w != screenW || outFrame->h != screenH) {
					scaled = outFrame->scale(screenW, screenH);
					drawSurf = scaled;
				} else if (converted) {
					drawSurf = converted;
				} else {
					converted = new Graphics::Surface();
					converted->copyFrom(*outFrame);
					drawSurf = converted;
				}

				// Apply Classic Mac OS gamma curve to the decoded MPEG video frame
				if (_vm->getPlatform() == Common::kPlatformMacintosh && drawSurf) {
					static byte macGammaLUT[256];
					static bool lutInitialized = false;
					if (!lutInitialized) {
						for (int i = 0; i < 256; i++) {
							macGammaLUT[i] = (byte)(pow(i / 255.0, 0.70) * 255.0 + 0.5);
						}
						lutInitialized = true;
					}

					for (int16 y = 0; y < drawSurf->h; y++) {
						if (screenFormat.bytesPerPixel == 4) {
							uint32 *ptr = (uint32 *)drawSurf->getBasePtr(0, y);
							for (int16 x = 0; x < drawSurf->w; x++) {
								uint8 r, g, b;
								screenFormat.colorToRGB(ptr[x], r, g, b);
								ptr[x] = screenFormat.RGBToColor(macGammaLUT[r], macGammaLUT[g], macGammaLUT[b]);
							}
						} else if (screenFormat.bytesPerPixel == 2) {
							uint16 *ptr = (uint16 *)drawSurf->getBasePtr(0, y);
							for (int16 x = 0; x < drawSurf->w; x++) {
								uint8 r, g, b;
								screenFormat.colorToRGB(ptr[x], r, g, b);
								ptr[x] = screenFormat.RGBToColor(macGammaLUT[r], macGammaLUT[g], macGammaLUT[b]);
							}
						}
					}
				}

				// Draw any active NPC mood/body icons (from exFACEICON) over the video frame
				if (!_faceIcons.empty() && drawSurf) {
					int16 hoveredPicId = -1;
					const int16 mouseX = _vm->_eventMouseX;
					const int16 mouseY = _vm->_eventMouseY;

					for (uint i = 0; i < _faceIcons.size(); ++i) {
						PictureResource *flex = _vm->_res->getPicture(_faceIcons[i].picId);
						if (!flex)
							continue;

						Graphics::Surface *iconSurf = flex->getPicture();
						if (iconSurf && iconSurf->getPixels()) {
							const byte *pal = flex->hasPalette() ? flex->getPalette() : savedPalette;
							// Note: Mac scripts pass x in 0..640 range (e.g. 24 for left, 594 for right).
							// Divide by 2 if needed to fit the 320x200 screen.
							int16 destX = _faceIcons[i].x;
							if (destX >= screenW)
								destX /= 2;
							int16 destY = _faceIcons[i].y;

							if (mouseX >= destX && mouseX < destX + iconSurf->w &&
								mouseY >= destY && mouseY < destY + iconSurf->h) {
								hoveredPicId = _faceIcons[i].picId;
							}

							for (int16 iy = 0; iy < iconSurf->h; ++iy) {
								int16 sy = destY + iy;
								if (sy < 0 || sy >= drawSurf->h)
									continue;
								const byte *srcRow = (const byte *)iconSurf->getBasePtr(0, iy);
								for (int16 ix = 0; ix < iconSurf->w; ++ix) {
									int16 sx = destX + ix;
									if (sx < 0 || sx >= drawSurf->w)
										continue;
									byte colorIdx = srcRow[ix];
									if (colorIdx == 0)
										continue; // color 0 is transparent
									byte r = pal[colorIdx * 3 + 0];
									byte g = pal[colorIdx * 3 + 1];
									byte b = pal[colorIdx * 3 + 2];
									uint32 pixel = screenFormat.RGBToColor(r, g, b);
									if (screenFormat.bytesPerPixel == 2)
										*((uint16 *)drawSurf->getBasePtr(sx, sy)) = (uint16)pixel;
									else if (screenFormat.bytesPerPixel == 4)
										*((uint32 *)drawSurf->getBasePtr(sx, sy)) = pixel;
								}
							}
						}
						_vm->_res->freeResource(flex);
					}

					// Draw hover tooltip for the hovered mood/body icon (matching DOS script 133 -> object 91)
					if (hoveredPicId != -1 && _vm->_dat->getVar(55) != 0) {
						static const int16 kTalkIconObjs[] = {
							2499, 2496, 2495, 2494, 2493, 2492, 2491, 2490, 2489, 2488
						};
						int16 matchedObj = 0;
						for (uint j = 0; j < ARRAYSIZE(kTalkIconObjs); ++j) {
							if (_vm->_dat->getObjectProperty(kTalkIconObjs[j], 506) == hoveredPicId) {
								matchedObj = kTalkIconObjs[j];
								break;
							}
						}

						if (matchedObj != 0) {
							int16 menuIndex = _vm->_dat->getObjectProperty(matchedObj, 59);
							int16 textIndex = _vm->_dat->getObjectProperty(matchedObj, 52);
							if (menuIndex <= 0)
								menuIndex = 5;

							MenuResource *menu = _vm->_res->getMenu(menuIndex);
							if (menu) {
								const char *rawText = menu->getString(textIndex);
								if (rawText && rawText[0] != '\0') {
									Common::String tooltip(rawText);
									if (tooltip[0] >= 'a' && tooltip[0] <= 'z')
										tooltip.setChar(tooltip[0] - 32, 0);

									int16 fontNum = _vm->_dat->getObjectProperty(91, 99);
									if (fontNum <= 0)
										fontNum = 22;

									FontResource *font = _vm->_res->getFont(fontNum);
									if (font) {
										int16 textW = font->getTextWidth(tooltip.c_str());
										int16 textH = font->getHeight();

										int16 tx = mouseX + 5 - (textW / 2);
										if (tx + textW >= drawSurf->w - 1)
											tx = drawSurf->w - textW - 2;
										if (tx < 1)
											tx = 1;

										int16 ty = mouseY - 15;
										if (ty < 1)
											ty = mouseY + 15;
										if (ty + textH >= drawSurf->h - 1)
											ty = drawSurf->h - textH - 2;

										int16 textColorIdx = _vm->_dat->getObjectProperty(91, 100);
										int16 outlineColorIdx = _vm->_dat->getObjectProperty(91, 101);
										if (textColorIdx < 0 || textColorIdx > 255)
											textColorIdx = 5;
										if (outlineColorIdx < 0 || outlineColorIdx > 255)
											outlineColorIdx = 15;

										uint32 textPixel = screenFormat.RGBToColor(
											savedPalette[textColorIdx * 3 + 0],
											savedPalette[textColorIdx * 3 + 1],
											savedPalette[textColorIdx * 3 + 2]);
										uint32 outlinePixel = screenFormat.RGBToColor(
											savedPalette[outlineColorIdx * 3 + 0],
											savedPalette[outlineColorIdx * 3 + 1],
											savedPalette[outlineColorIdx * 3 + 2]);

										int16 curX = tx;
										for (uint cIdx = 0; cIdx < tooltip.size(); ++cIdx) {
											uint ch = (byte)tooltip[cIdx];
											if (ch >= 28 && ch <= 255) {
												for (int16 dy = -1; dy <= 1; ++dy) {
													for (int16 dx = -1; dx <= 1; ++dx) {
														if (dx != 0 || dy != 0)
															drawCharToRGBSurface(drawSurf, font, ch, curX + dx, ty + dy, outlinePixel, screenFormat.bytesPerPixel);
													}
												}
												drawCharToRGBSurface(drawSurf, font, ch, curX, ty, textPixel, screenFormat.bytesPerPixel);
												curX += font->getCharWidth(ch);
											}
										}
										_vm->_res->freeResource(font);
									}
								}
								_vm->_res->freeResource(menu);
							}
						}
					}
				}

				_vm->_system->copyRectToScreen(drawSurf->getPixels(), drawSurf->pitch,
											   0, 0, drawSurf->w, drawSurf->h);

				if (scaled) {
					scaled->free();
					delete scaled;
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
	clearFaceIcons();

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