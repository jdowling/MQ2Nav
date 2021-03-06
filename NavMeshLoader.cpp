//
// NavMeshLoader.cpp
//

#include "NavMeshLoader.h"
#include "MQ2Nav_Util.h"
#include "MQ2Navigation.h"

// nav mesh definitions
#include "DetourNavMesh.h"
#include "DetourCommon.h"

#include <ctime>

//----------------------------------------------------------------------------

void NavMeshLoader::Initialize()
{
}

void NavMeshLoader::Shutdown()
{
}

//----------------------------------------------------------------------------

void NavMeshLoader::SetZoneId(int zoneId)
{
	if (m_zoneId == zoneId)
		return;

	m_zoneId = zoneId;

	Reset();

	if (m_zoneId != -1)
	{
		// set or clear zone name
		PCHAR zoneName = GetShortZone(zoneId);
		m_zoneShortName = zoneName ? zoneName : std::string();

		if (m_zoneShortName == "UNKNOWN_ZONE")
		{
			// invalid / unsupported zone id
			DebugSpewAlways("[MQ2Nav] Unrecognized zone id: %d", zoneId);
			Reset();
		}
		else
		{
			DebugSpewAlways("[MQ2Nav] Zone changed to: %s", m_zoneShortName.c_str());

			if (m_autoLoad)
			{
				LoadNavMesh();
			}
		}
	}
}

void NavMeshLoader::SetAutoLoad(bool autoLoad)
{
	m_autoLoad = autoLoad;
}

// Given a filename, return a buffer of data. Returns nullptr if
// the file does not exist.
static std::pair<std::shared_ptr<char>, DWORD> ReadFile(const std::string& filename)
{
	FILE* file = 0;
	errno_t err = fopen_s(&file, filename.c_str(), "rb");
	if (!err)
	{
		scope_guard g = [file]() { fclose(file); };

		// find the file size
		DWORD size = 0;
		fseek(file, 0, SEEK_END);
		size = ftell(file);
		rewind(file);

		// allocate memory to contain the whole file
		std::shared_ptr<char> buffer(new char[size], [](char* p) { delete[] p; });

		// copy the file into the buffer
		size_t result = fread(buffer.get(), 1, size, file);
		if (result == size)
		{
			return std::make_pair(buffer, size);
		}
	}

	return std::make_pair(nullptr, 0);
}

static std::pair<std::shared_ptr<char>, DWORD> ReadRarFile(const std::string& filename, const std::string& contentsFile)
{
	char* outbuf = nullptr;
	DWORD outsize = 0;

	char outfile[MAX_PATH];
	strcpy_s(outfile, contentsFile.c_str());

	char rarfile[MAX_PATH];
	strcpy_s(rarfile, filename.c_str());

	return std::make_pair(std::shared_ptr<char>(outbuf, free), outsize);
}

std::string NavMeshLoader::GetMeshDirectory() const
{
	// the root path is where we look for all of our mesh files
	return std::string(gszINIPath) + "\\MQ2Nav";
}

bool NavMeshLoader::LoadNavMesh()
{
	// At this point, we expect the zone short name to be set, so we know
	// which map file we need to load.

	// Upon successful completion of this load, we will have set the dtNavMesh
	// and the filename that we loaded.

	if (m_zoneShortName.empty())
		return false;

	// the root path is where we look for all of our mesh files
	std::string root_path = GetMeshDirectory() + "\\";

	// The mesh filename
	std::string mesh_filename = m_zoneShortName + ".bin";
	std::string data_file = root_path + mesh_filename;

	// this is what we do after we successfully read from a file. We are given the
	// data, its length, and the file that provided the data. If the load succeeds,
	// we'll store the filename and process the mesh. If the load fails, we'll move
	// onto the next file.

	// The code is broken up this way to maintain consistency between loading from a
	// rar file, and loading from a plain .bin file
	auto next_load_stage = [&](const auto& load_ret_val, const std::string& load_filename) -> bool
	{
		std::shared_ptr<char> data = load_ret_val.first;
		DWORD dataLen = load_ret_val.second;

		LoadResult result = LoadZoneMeshData(data, dataLen);
		if (result == SUCCESS)
		{
			WriteChatf(PLUGIN_MSG "\agSuccessfully loaded mesh for \am%s\ax", m_zoneShortName.c_str()); 
			m_loadedDataFile = load_filename;

			// Get filetime
			HANDLE hFile = CreateFile(m_loadedDataFile.c_str(), GENERIC_READ, FILE_SHARE_READ,
				NULL, OPEN_EXISTING, 0, NULL);
			if (hFile != INVALID_HANDLE_VALUE)
			{
				GetFileTime(hFile, NULL, NULL, &m_fileTime);
			}
			CloseHandle(hFile);

			return true;
		}
		if (result == CORRUPT)
		{
			WriteChatf(PLUGIN_MSG "\arFailed to load mesh file, the file is corrupt (%s)", load_filename.c_str());
		}
		else if (result == VERSION_MISMATCH)
		{
			WriteChatf(PLUGIN_MSG "\arCouldn't load mesh file due to version mismatch (%s)", load_filename.c_str());
		}

		return false;
	};

	// load raw mesh file first
	auto file_data = ReadFile(data_file);
	if (file_data.first)
	{
		if (next_load_stage(file_data, data_file))
			return true;
	}

	WriteChatf(PLUGIN_MSG "\ayNo nav mesh available for \am%s\ax", m_zoneShortName.c_str());
	return false;
}

//----------------------------------------------------------------------------

static const int NAVMESHSET_MAGIC = 'M' << 24 | 'S' << 16 | 'E' << 8 | 'T';

static const int NAVMESHSET_VERSION = 2;

// header (the same across all versions)
struct NavMeshSetHeader
{
	int magic;
	int version;
	int numTiles;
	dtNavMeshParams params;
};

struct NavMeshTileHeader
{
	dtTileRef tileRef;
	int dataSize;
};

struct ReadCursor {
	char* data;
	DWORD length;
};

template <typename T>
bool FillStructure(ReadCursor& cursor, T& data)
{
	DWORD l = sizeof(T);
	if (l > cursor.length)
		return false;

	memcpy(&data, cursor.data, l);
	cursor.data += l;
	cursor.length -= l;
	return true;
}

NavMeshLoader::LoadResult NavMeshLoader::LoadZoneMeshData(const std::shared_ptr<char>& data, DWORD length)
{
	ReadCursor cursor{ data.get(), length };

	//------------------------------------------------------------------------
	// Read the header
	NavMeshSetHeader header;
	if (!FillStructure(cursor, header)) {
		DebugSpewAlways("Failed to read header from mesh file!");
		return CORRUPT;
	}

	if (header.magic != NAVMESHSET_MAGIC) {
		DebugSpewAlways("Header Magic value mismatch!");
		return CORRUPT;
	}
	if (header.version != NAVMESHSET_VERSION) {
		DebugSpewAlways("Header version mismatch!");
		return VERSION_MISMATCH;
	}

	std::unique_ptr<dtNavMesh> navMesh(new dtNavMesh);
	if (!navMesh->init(&header.params)) {
		DebugSpewAlways("Header params are bad!");
		return CORRUPT;
	}

	//------------------------------------------------------------------------
	// Read the tiles
	int passtile = 0, failtile = 0;

	for (int i = 0; i < header.numTiles; ++i)
	{
		NavMeshTileHeader tileHeader;
		if (!FillStructure(cursor, tileHeader)) {
			DebugSpewAlways("Failed to read tile header for tile %d!", i);
			return CORRUPT;
		}

		if (!tileHeader.tileRef || !tileHeader.dataSize) {
			continue;
		}

		// validate the size. For sure it should not exceed the remainder of the
		// file size.
		if (tileHeader.dataSize < 0 || tileHeader.dataSize > cursor.length) {
			DebugSpewAlways("tileHeader's dataSize has invalid size %d for tile %d",
				tileHeader.dataSize, i);
			return CORRUPT;
		}

		unsigned char* tileData = (unsigned char*)dtAlloc(tileHeader.dataSize, DT_ALLOC_PERM);
		memcpy(tileData, cursor.data, tileHeader.dataSize);
		cursor.length -= tileHeader.dataSize;
		cursor.data += tileHeader.dataSize;

		if (navMesh->addTile(tileData, tileHeader.dataSize, DT_TILE_FREE_DATA, tileHeader.tileRef, 0))
			passtile++;
		else
		{
			DebugSpewAlways("Failed to load tile %d", i);
			failtile++;
		}
	}

	// these values shouldn't be set until we have a successful load
	m_loadedTiles = passtile;
	m_mesh = std::move(navMesh);
	OnNavMeshChanged(m_mesh.get());

	return SUCCESS;
}

void NavMeshLoader::Reset()
{
	OnNavMeshChanged(nullptr);
	m_mesh.reset();
	m_zoneShortName.clear();
	m_loadedDataFile.clear();
	m_loadedTiles = 0;
}

void NavMeshLoader::OnPulse()
{
	if (m_autoReload)
	{
		clock::time_point now = clock::now();
		if (now - m_lastUpdate > std::chrono::seconds(1))
		{
			m_lastUpdate = now;

			if (!m_loadedDataFile.empty())
			{
				// Get the current filetime
				FILETIME currentFileTime;

				HANDLE hFile = CreateFile(m_loadedDataFile.c_str(), GENERIC_READ, FILE_SHARE_READ,
					NULL, OPEN_EXISTING, 0, NULL);
				if (hFile != INVALID_HANDLE_VALUE)
				{
					GetFileTime(hFile, NULL, NULL, &currentFileTime);

					// Reload file *if* it is 5 seconds old AND newer than existing
					if (CompareFileTime(&currentFileTime, &m_fileTime))
					{
						DebugSpewAlways("Current file time is newer than old file time, refreshing");
						LoadNavMesh();
					}
				}
				CloseHandle(hFile);
			}
		}
	}
}

void NavMeshLoader::SetGameState(int GameState)
{
	if (GameState != GAMESTATE_INGAME) {
		// don't unload the mesh until we finish zoning. We might zone
		// into the same zone (succor), so no use unload the mesh until
		// after loading completes.
		if (GameState != GAMESTATE_ZONING && GameState != GAMESTATE_LOGGINGIN) {
			Reset();
		}
	}
}

//----------------------------------------------------------------------------

void NavMeshLoader::SetAutoReload(bool autoReload)
{
	if (m_autoReload != autoReload)
	{
		m_autoReload = autoReload;
	}
}
