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

#ifndef MADE_DATABASE_H
#define MADE_DATABASE_H

#include "common/array.h"
#include "common/endian.h"
#include "common/hashmap.h"

namespace Common {
class SeekableReadStream;
class WriteStream;
class String;
}

namespace Made {

class MadeEngine;

/* Object */

class Object {
public:
	Object();
	virtual ~Object();

	virtual int load(Common::SeekableReadStream &source) = 0;
	virtual int load(byte *source) = 0;
	virtual int save(Common::WriteStream &dest) const;

	virtual uint16 getFlags() const;
	virtual uint16 getClass() const = 0;
	virtual uint16 getCount() const = 0;
	virtual byte getKeyCount() const = 0;
	virtual byte getValueCount() const = 0;
	virtual byte *getData() const = 0;

	bool isByteArray() const;
	bool isWordArray() const;
	bool isObject() const;

	bool isWritable() const;

	const char *getString() const;
	void setString(const char *str);

	int16 getVectorItem(int16 index) const;
	void setVectorItem(int16 index, int16 value);

	void dump(const Common::String &filename) const;

protected:
	bool _freeData;
	uint16 _objSize;
	byte *_objData;

	virtual inline uint16 getWord(const void *ptr) const { return READ_LE_UINT16(ptr); }
	virtual inline void setWord(void *ptr, uint16 value) { WRITE_LE_UINT16(ptr, value); }
};

class ObjectV1 : public Object {
public:
	int load(Common::SeekableReadStream &source) override;
	int load(byte *source) override;
	int save(Common::WriteStream &dest) const override;

	uint16 getClass() const override;
	uint16 getCount() const override;
	byte getKeyCount() const override;
	byte getValueCount() const override;
	byte *getData() const override;

protected:
	uint16 calcSize(const byte *source) const;
};

class ObjectV2 : public ObjectV1 {
public:
	byte getValueCount() const override;
	byte getKeyCount() const override;
};

class ObjectV3 : public Object {
public:
	int load(Common::SeekableReadStream &source) override;
	int load(byte *source) override;

	uint16 getFlags() const override;
	uint16 getClass() const override;
	uint16 getCount() const override;
	byte getKeyCount() const override;
	byte getValueCount() const override;
	byte *getData() const override;

protected:
	virtual uint16 calcSize(const byte *source) const;
};

class ObjectV3_1 : public ObjectV3 {
public:
	ObjectV3_1(bool bigEndian = false) : ObjectV3(), _bigEndian(bigEndian) {}

protected:
	uint16 calcSize(const byte *source) const override;

private:
	// V3_1 objects may be either endianness depending on source platform
	bool _bigEndian;
	inline uint16 getWord(const void *ptr) const override { return _bigEndian ? READ_BE_UINT16(ptr) : READ_LE_UINT16(ptr); }
	inline void setWord(void *ptr, uint16 value) override { _bigEndian ? WRITE_BE_UINT16(ptr, value) : WRITE_LE_UINT16(ptr, value); }
};


/* GameDatabase */

class GameDatabase {
public:
	GameDatabase();
	virtual ~GameDatabase();

	void open(const char *filename);
	void openFromRed(const char *redFilename, const char *filename);

	void reload();

	int16 getVar(int16 index) const;
	void setVar(int16 index, int16 value);

	Object *getObject(int16 index) const;
	uint getObjectCount() const { return _objects.size(); }

	int16 getMainCodeObjectIndex() const { return _mainCodeObjectIndex; }

	const char *getObjectString(int16 index) const;
	void setObjectString(int16 index, const char *str);

	virtual int16 *findObjectProperty(int16 objectIndex, int16 propertyId, int16 &propertyFlag) = 0;
	int16 *findObjectPropertyCached(int16 objectIndex, int16 propertyId, int16 &writableFlag);
	virtual const char *getString(uint16 offset) = 0;
	virtual bool getSavegameDescription(const char *filename, Common::String &description, int16 version) = 0;
	virtual int16 savegame(const char *filename, const char *description, int16 version) = 0;
	virtual int16 loadgame(const char *filename, int16 version) = 0;

	int16 getObjectProperty(int16 objectIndex, int16 propertyId);
	int16 setObjectProperty(int16 objectIndex, int16 propertyId, int16 value);

	void dumpObject(int16 index) const;

protected:
	virtual void load(Common::SeekableReadStream &sourceS) = 0;
	virtual void reloadFromStream(Common::SeekableReadStream &sourceS) = 0;

	virtual inline uint16 getWord(const void *ptr) const { return READ_LE_UINT16(ptr); }
	virtual inline void setWord(void *ptr, uint16 value) { WRITE_LE_UINT16(ptr, value); }

	Common::String _filename, _redFilename;

	typedef Common::HashMap<uint32, int16 *> ObjectPropertyCacheMap;
	Common::Array<Object *> _objects;
	ObjectPropertyCacheMap _objectPropertyCache;

	Common::Array<byte> _gameState;
	int16 _mainCodeObjectIndex;
};

class GameDatabaseV1 : public GameDatabase {
public:
	int16 *findObjectProperty(int16 objectIndex, int16 propertyId, int16 &propertyFlag) override;
	const char *getString(uint16 offset) override;
	bool getSavegameDescription(const char *filename, Common::String &description, int16 version) override;
	int16 savegame(const char *filename, const char *description, int16 version) override;
	int16 loadgame(const char *filename, int16 version) override;

protected:
	void load(Common::SeekableReadStream &sourceS) override;
	void reloadFromStream(Common::SeekableReadStream &sourceS) override;

	Common::Array<char> _gameText;
};

class GameDatabaseV2 : public GameDatabaseV1 {
protected:
	void load(Common::SeekableReadStream &sourceS) override;
};

class GameDatabaseV3 : public GameDatabase {
public:
	GameDatabaseV3();
	virtual int16 *findObjectProperty(int16 objectIndex, int16 propertyId, int16 &propertyFlag) override;
	const char *getString(uint16 offset) override;
	bool getSavegameDescription(const char *filename, Common::String &description, int16 version) override;
	int16 savegame(const char *filename, const char *description, int16 version) override;
	int16 loadgame(const char *filename, int16 version) override;

protected:
	bool _bigEndian;
	inline uint16 getWord(const void *ptr) const override { return _bigEndian ? READ_BE_UINT16(ptr) : READ_LE_UINT16(ptr); }
	inline void setWord(void *ptr, uint16 value) override { _bigEndian ? WRITE_BE_UINT16(ptr, value) : WRITE_LE_UINT16(ptr, value); }

	uint32 _gameStateOffs;
	void load(Common::SeekableReadStream &sourceS) override;
	void reloadFromStream(Common::SeekableReadStream &sourceS) override;
};

class GameDatabaseV3_1 : public GameDatabaseV3 {
public:
	int16 *findObjectProperty(int16 objectIndex, int16 propertyId, int16 &propertyFlag) override;
};

} // End of namespace Made

#endif /* MADE_DATABASE_H */