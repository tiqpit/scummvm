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

#define BS_LOG_PREFIX "GRAPHICENGINE"

#include "sword25/gfx/image/image.h"
#include "sword25/gfx/screenshot.h"
#include "sword25/kernel/inputpersistenceblock.h"
#include "sword25/kernel/outputpersistenceblock.h"
#include "sword25/gfx/graphicengine.h"

namespace Lua {
extern "C"
{
#include "sword25/util/lua/lua.h"
#include "sword25/util/lua/lauxlib.h"
}

}

namespace Sword25 {

using namespace Lua;

static const uint FRAMETIME_SAMPLE_COUNT = 5;       // Anzahl der Framezeiten �ber die, die Framezeit gemittelt wird

GraphicEngine::GraphicEngine(Kernel *pKernel) :
	m_Width(0),
	m_Height(0),
	m_BitDepth(0),
	m_Windowed(0),
	m_LastTimeStamp((uint64) - 1), // max. BS_INT64 um beim ersten Aufruf von _UpdateLastFrameDuration() einen Reset zu erzwingen
	m_LastFrameDuration(0),
	m_TimerActive(true),
	m_FrameTimeSampleSlot(0),
	m_RepaintedPixels(0),
	ResourceService(pKernel) {
	m_FrameTimeSamples.resize(FRAMETIME_SAMPLE_COUNT);

	if (!RegisterScriptBindings())
		BS_LOG_ERRORLN("Script bindings could not be registered.");
	else
		BS_LOGLN("Script bindings registered.");
}

// -----------------------------------------------------------------------------

void  GraphicEngine::UpdateLastFrameDuration() {
	// Aktuelle Zeit holen
	uint64_t CurrentTime = Kernel::GetInstance()->GetMicroTicks();

	// Verstrichene Zeit seit letztem Frame berechnen und zu gro�e Zeitspr�nge ( > 250 msek.) unterbinden
	// (kann vorkommen bei geladenen Spielst�nden, w�hrend des Debuggings oder Hardwareungenauigkeiten)
	m_FrameTimeSamples[m_FrameTimeSampleSlot] = static_cast<uint>(CurrentTime - m_LastTimeStamp);
	if (m_FrameTimeSamples[m_FrameTimeSampleSlot] > 250000) m_FrameTimeSamples[m_FrameTimeSampleSlot] = 250000;
	m_FrameTimeSampleSlot = (m_FrameTimeSampleSlot + 1) % FRAMETIME_SAMPLE_COUNT;

	// Die Framezeit wird �ber mehrere Frames gemittelt um Ausreisser zu eliminieren
	Common::Array<uint>::const_iterator it = m_FrameTimeSamples.begin();
	uint Sum = *it;
	for (it++; it != m_FrameTimeSamples.end(); it++) Sum += *it;
	m_LastFrameDuration = Sum / FRAMETIME_SAMPLE_COUNT;

	// _LastTimeStamp auf die Zeit des aktuellen Frames setzen
	m_LastTimeStamp = CurrentTime;
}

// -----------------------------------------------------------------------------

namespace {
bool DoSaveScreenshot(GraphicEngine &GraphicEngine, const Common::String &Filename, bool Thumbnail) {
	uint Width;
	uint Height;
	byte *Data;
	if (!GraphicEngine.GetScreenshot(Width, Height, &Data)) {
		BS_LOG_ERRORLN("Call to GetScreenshot() failed. Cannot save screenshot.");
		return false;
	}

	if (Thumbnail)
		return Screenshot::SaveThumbnailToFile(Width, Height, Data, Filename);
	else
		return Screenshot::SaveToFile(Width, Height, Data, Filename);
}
}

// -----------------------------------------------------------------------------

bool GraphicEngine::SaveScreenshot(const Common::String &Filename) {
	return DoSaveScreenshot(*this, Filename, false);
}

// -----------------------------------------------------------------------------

bool GraphicEngine::SaveThumbnailScreenshot(const Common::String &Filename) {
	return DoSaveScreenshot(*this, Filename, true);
}

// -----------------------------------------------------------------------------

void GraphicEngine::ARGBColorToLuaColor(lua_State *L, uint Color) {
	lua_Number Components[4] = {
		(Color >> 16) & 0xff,   // Rot
		(Color >> 8) & 0xff,    // Gr�n
		Color & 0xff,          // Blau
		Color >> 24,           // Alpha
	};

	lua_newtable(L);

	for (uint i = 1; i <= 4; i++) {
		lua_pushnumber(L, i);
		lua_pushnumber(L, Components[i - 1]);
		lua_settable(L, -3);
	}
}

// -----------------------------------------------------------------------------

uint GraphicEngine::LuaColorToARGBColor(lua_State *L, int StackIndex) {
#ifdef DEBUG
	int __startStackDepth = lua_gettop(L);
#endif

	// Sicherstellen, dass wir wirklich eine Tabelle betrachten
	luaL_checktype(L, StackIndex, LUA_TTABLE);
	// Gr��e der Tabelle auslesen
	uint n = luaL_getn(L, StackIndex);
	// RGB oder RGBA Farben werden unterst�tzt und sonst keine
	if (n != 3 && n != 4) luaL_argcheck(L, 0, StackIndex, "at least 3 of the 4 color components have to be specified");

	// Rote Farbkomponente auslesen
	lua_rawgeti(L, StackIndex, 1);
	uint Red = static_cast<uint>(lua_tonumber(L, -1));
	if (!lua_isnumber(L, -1) || Red >= 256) luaL_argcheck(L, 0, StackIndex, "red color component must be an integer between 0 and 255");
	lua_pop(L, 1);

	// Gr�ne Farbkomponente auslesen
	lua_rawgeti(L, StackIndex, 2);
	uint Green = static_cast<uint>(lua_tonumber(L, -1));
	if (!lua_isnumber(L, -1) || Green >= 256) luaL_argcheck(L, 0, StackIndex, "green color component must be an integer between 0 and 255");
	lua_pop(L, 1);

	// Blaue Farbkomponente auslesen
	lua_rawgeti(L, StackIndex, 3);
	uint Blue = static_cast<uint>(lua_tonumber(L, -1));
	if (!lua_isnumber(L, -1) || Blue >= 256) luaL_argcheck(L, 0, StackIndex, "blue color component must be an integer between 0 and 255");
	lua_pop(L, 1);

	// Alpha Farbkomponente auslesen
	uint Alpha = 0xff;
	if (n == 4) {
		lua_rawgeti(L, StackIndex, 4);
		Alpha = static_cast<uint>(lua_tonumber(L, -1));
		if (!lua_isnumber(L, -1) || Alpha >= 256) luaL_argcheck(L, 0, StackIndex, "alpha color component must be an integer between 0 and 255");
		lua_pop(L, 1);
	}

#ifdef DEBUG
	BS_ASSERT(__startStackDepth == lua_gettop(L));
#endif

	return (Alpha << 24) | (Red << 16) | (Green << 8) | Blue;
}

// -----------------------------------------------------------------------------

bool GraphicEngine::persist(OutputPersistenceBlock &writer) {
	writer.write(m_TimerActive);
	return true;
}

// -----------------------------------------------------------------------------

bool GraphicEngine::unpersist(InputPersistenceBlock &reader) {
	reader.read(m_TimerActive);
	return reader.isGood();
}

} // End of namespace Sword25