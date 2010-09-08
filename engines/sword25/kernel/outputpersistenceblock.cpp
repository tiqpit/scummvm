/* ScummVM - Graphic Adventure Engine
 *
 * ScummVM is the legal property of its developers, whose names
 * are too numerous to list here. Please refer to the COPYRIGHT
 * file distributed with this source distribution.
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
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 * $URL$
 * $Id$
 *
 */

/*
 * This code is based on Broken Sword 2.5 engine
 *
 * Copyright (c) Malte Thiesen, Daniel Queteschiner and Michael Elsdoerfer
 *
 * Licensed under GNU GPL v2
 *
 */

#define BS_LOG_PREFIX "OUTPUTPERSISTENCEBLOCK"

// -----------------------------------------------------------------------------
// Includes
// -----------------------------------------------------------------------------

#include "sword25/kernel/outputpersistenceblock.h"

// -----------------------------------------------------------------------------
// Constants
// -----------------------------------------------------------------------------

namespace {
const uint INITIAL_BUFFER_SIZE = 1024 * 64;
}

namespace Sword25 {

// -----------------------------------------------------------------------------
// Construction / Destruction
// -----------------------------------------------------------------------------

OutputPersistenceBlock::OutputPersistenceBlock() {
	m_Data.reserve(INITIAL_BUFFER_SIZE);
}

// -----------------------------------------------------------------------------
// Writing
// -----------------------------------------------------------------------------

void OutputPersistenceBlock::write(signed int Value) {
	WriteMarker(SINT_MARKER);
	Value = ConvertEndianessFromSystemToStorage(Value);
	RawWrite(&Value, sizeof(Value));
}

// -----------------------------------------------------------------------------

void OutputPersistenceBlock::write(uint Value) {
	WriteMarker(UINT_MARKER);
	Value = ConvertEndianessFromSystemToStorage(Value);
	RawWrite(&Value, sizeof(Value));
}

// -----------------------------------------------------------------------------

void OutputPersistenceBlock::write(float Value) {
	WriteMarker(FLOAT_MARKER);
	Value = ConvertEndianessFromSystemToStorage(Value);
	RawWrite(&Value, sizeof(Value));
}

// -----------------------------------------------------------------------------

void OutputPersistenceBlock::write(bool Value) {
	WriteMarker(BOOL_MARKER);

	uint UIntBool = Value ? 1 : 0;
	UIntBool = ConvertEndianessFromSystemToStorage(UIntBool);
	RawWrite(&UIntBool, sizeof(UIntBool));
}

// -----------------------------------------------------------------------------

void OutputPersistenceBlock::write(const Common::String &String) {
	WriteMarker(STRING_MARKER);

	write(String.size());
	RawWrite(String.c_str(), String.size());
}

// -----------------------------------------------------------------------------

void OutputPersistenceBlock::write(const void *BufferPtr, size_t Size) {
	WriteMarker(BLOCK_MARKER);

	write(Size);
	RawWrite(BufferPtr, Size);
}

// -----------------------------------------------------------------------------

void OutputPersistenceBlock::WriteMarker(byte Marker) {
	m_Data.push_back(Marker);
}

// -----------------------------------------------------------------------------

void OutputPersistenceBlock::RawWrite(const void *DataPtr, size_t Size) {
	if (Size > 0) {
		uint OldSize = m_Data.size();
		m_Data.resize(OldSize + Size);
		memcpy(&m_Data[OldSize], DataPtr, Size);
	}
}

} // End of namespace Sword25