/*
* Copyright (C) 2008-2016 TrinityCore <http://www.trinitycore.org/>
*
* This program is free software; you can redistribute it and/or modify it
* under the terms of the GNU General Public License as published by the
* Free Software Foundation; either version 3 of the License, or (at your
* option) any later version.
*
* This program is distributed in the hope that it will be useful, but WITHOUT
* ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
* FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
* more details.
*
* You should have received a copy of the GNU General Public License along
* with this program. If not, see <http://www.gnu.org/licenses/>.
*/

#include "MMapManager.h"
#include "Log.h"
#include "World.h"

namespace MMAP
{
	// ######################## MMapManager ########################
	MMapManager::~MMapManager()
	{
		for (MMapDataSet::iterator i = loadedMMaps.begin(); i != loadedMMaps.end(); ++i)
			delete i->second;

		// By now we should not have maps loaded.
		// If we had, tiles in MMapData->mmapLoadedTiles, their actual data is lost!
	}

	bool MMapManager::loadMapData(uint32 mapId)
	{
		// We already have this map loaded?
		if (loadedMMaps.find(mapId) != loadedMMaps.end())
			return true;

		// Load and init dtNavMesh - read parameters from file
		uint32 pathLen = sWorld->GetDataPath().length() + strlen("mmaps/%03i.mmap") + 1;
		char *fileName = new char[pathLen];
		snprintf(fileName, pathLen, (sWorld->GetDataPath() + "mmaps/%03i.mmap").c_str(), mapId);

		FILE* file = fopen(fileName, "rb");
		if (!file)
		{
			sLog->outInfo(LOG_FILTER_MAPS, "MMAP:loadMapData: Error: Could not open mmap file '%s'", fileName);
			delete[] fileName;
			return false;
		}

		dtNavMeshParams params;
		int count = fread(&params, sizeof(dtNavMeshParams), 1, file);
		fclose(file);
		if (count != 1)
		{
			sLog->outInfo(LOG_FILTER_MAPS, "MMAP:loadMapData: Error: Could not read params from file '%s'", fileName);
			delete[] fileName;
			return false;
		}

		dtNavMesh* mesh = dtAllocNavMesh();
		ASSERT(mesh);
		if (dtStatusFailed(mesh->init(&params)))
		{
			dtFreeNavMesh(mesh);
			sLog->outInfo(LOG_FILTER_MAPS, "MMAP:loadMapData: Failed to initialize dtNavMesh for mmap %03u from file %s", mapId, fileName);
			delete[] fileName;
			return false;
		}

		delete[] fileName;

		sLog->outInfo(LOG_FILTER_MAPS, "MMAP:loadMapData: Loaded %03i.mmap", mapId);

		// Store inside our map list
		MMapData* mmap_data = new MMapData(mesh);
		mmap_data->mmapLoadedTiles.clear();

		loadedMMaps.insert(std::pair<uint32, MMapData*>(mapId, mmap_data));
		return true;
	}

	uint32 MMapManager::packTileID(int32 x, int32 y)
	{
		return uint32(x << 16 | y);
	}

	bool MMapManager::loadMap(const std::string& /*basePath*/, uint32 mapId, int32 x, int32 y)
	{
		// Make sure the mmap is loaded and ready to load tiles
		if (!loadMapData(mapId))
			return false;

		// Get this mmap data
		MMapData* mmap = loadedMMaps[mapId];
		ASSERT(mmap->navMesh);

		// Check if we already have this tile loaded
		uint32 packedGridPos = packTileID(x, y);
		if (mmap->mmapLoadedTiles.find(packedGridPos) != mmap->mmapLoadedTiles.end())
			return false;

		// Load this tile :: mmaps/MMMXXYY.mmtile
		uint32 pathLen = sWorld->GetDataPath().length() + strlen("mmaps/%03i%02i%02i.mmtile") + 1;
		char *fileName = new char[pathLen];

		snprintf(fileName, pathLen, (sWorld->GetDataPath() + "mmaps/%03i%02i%02i.mmtile").c_str(), mapId, x, y);

		FILE* file = fopen(fileName, "rb");
		if (!file)
		{
			sLog->outInfo(LOG_FILTER_MAPS, "MMAP:loadMap: Could not open mmtile file '%s'", fileName);
			delete[] fileName;
			return false;
		}
		delete[] fileName;

		// Read header
		MmapTileHeader fileHeader;
		if (fread(&fileHeader, sizeof(MmapTileHeader), 1, file) != 1 || fileHeader.mmapMagic != MMAP_MAGIC)
		{
			sLog->outInfo(LOG_FILTER_MAPS, "MMAP:loadMap: Bad header in mmap %03u%02i%02i.mmtile", mapId, x, y);
			fclose(file);
			return false;
		}

		if (fileHeader.mmapVersion != MMAP_VERSION)
		{
			sLog->outInfo(LOG_FILTER_MAPS, "MMAP:loadMap: %03u%02i%02i.mmtile was built with generator v%i, expected v%i",
				mapId, x, y, fileHeader.mmapVersion, MMAP_VERSION);
			fclose(file);
			return false;
		}

		unsigned char* data = (unsigned char*)dtAlloc(fileHeader.size, DT_ALLOC_PERM);
		ASSERT(data);

		size_t result = fread(data, fileHeader.size, 1, file);
		if (!result)
		{
			sLog->outInfo(LOG_FILTER_MAPS, "MMAP:loadMap: Bad header or data in mmap %03u%02i%02i.mmtile", mapId, x, y);
			fclose(file);
			return false;
		}

		fclose(file);

		dtMeshHeader* header = (dtMeshHeader*)data;
		dtTileRef tileRef = 0;

		// Memory allocated for data is now managed by detour, and will be deallocated when the tile is removed
		if (dtStatusSucceed(mmap->navMesh->addTile(data, fileHeader.size, DT_TILE_FREE_DATA, 0, &tileRef)))
		{
			mmap->mmapLoadedTiles.insert(std::pair<uint32, dtTileRef>(packedGridPos, tileRef));
			++loadedTiles;
			sLog->outInfo(LOG_FILTER_MAPS, "MMAP:loadMap: Loaded mmtile %03i[%02i, %02i] into %03i[%02i, %02i]", mapId, x, y, mapId, header->x, header->y);
			return true;
		}
		else
		{
			sLog->outInfo(LOG_FILTER_MAPS, "MMAP:loadMap: Could not load %03u%02i%02i.mmtile into navmesh", mapId, x, y);
			dtFree(data);
			return false;
		}

		return false;
	}

	bool MMapManager::unloadMap(uint32 mapId, int32 x, int32 y)
	{
		// Check if we have this map loaded
		if (loadedMMaps.find(mapId) == loadedMMaps.end())
		{
			// File may not exist, therefore not loaded
			sLog->outInfo(LOG_FILTER_MAPS, "MMAP:unloadMap: Asked to unload not loaded navmesh map. %03u%02i%02i.mmtile", mapId, x, y);
			return false;
		}

		MMapData* mmap = loadedMMaps[mapId];

		// Check if we have this tile loaded
		uint32 packedGridPos = packTileID(x, y);
		if (mmap->mmapLoadedTiles.find(packedGridPos) == mmap->mmapLoadedTiles.end())
		{
			// File may not exist, therefore not loaded
			sLog->outInfo(LOG_FILTER_MAPS, "MMAP:unloadMap: Asked to unload not loaded navmesh tile. %03u%02i%02i.mmtile", mapId, x, y);
			return false;
		}

		dtTileRef tileRef = mmap->mmapLoadedTiles[packedGridPos];

		// Unload, and mark as non loaded
		if (dtStatusFailed(mmap->navMesh->removeTile(tileRef, NULL, NULL)))
		{
			// This is technically a memory leak
			// If the grid is later reloaded, dtNavMesh::addTile will return error but no extra memory is used
			// We cannot recover from this error - assert out
			sLog->outInfo(LOG_FILTER_MAPS, "MMAP:unloadMap: Could not unload %03u%02i%02i.mmtile from navmesh", mapId, x, y);
			ASSERT(false);
		}
		else
		{
			mmap->mmapLoadedTiles.erase(packedGridPos);
			--loadedTiles;
			sLog->outInfo(LOG_FILTER_MAPS, "MMAP:unloadMap: Unloaded mmtile %03i[%02i, %02i] from %03i", mapId, x, y, mapId);
			return true;
		}

		return false;
	}

	bool MMapManager::unloadMap(uint32 mapId)
	{
		if (loadedMMaps.find(mapId) == loadedMMaps.end())
		{
			// File may not exist, therefore not loaded
			sLog->outInfo(LOG_FILTER_MAPS, "MMAP:unloadMap: Asked to unload not loaded navmesh map %03u", mapId);
			return false;
		}

		// Unload all tiles from given map
		MMapData* mmap = loadedMMaps[mapId];
		for (MMapTileSet::iterator i = mmap->mmapLoadedTiles.begin(); i != mmap->mmapLoadedTiles.end(); ++i)
		{
			uint32 x = (i->first >> 16);
			uint32 y = (i->first & 0x0000FFFF);
			if (dtStatusFailed(mmap->navMesh->removeTile(i->second, NULL, NULL)))
				sLog->outInfo(LOG_FILTER_MAPS, "MMAP:unloadMap: Could not unload %03u%02i%02i.mmtile from navmesh", mapId, x, y);
			else
			{
				--loadedTiles;
				sLog->outInfo(LOG_FILTER_MAPS, "MMAP:unloadMap: Unloaded mmtile %03i[%02i, %02i] from %03i", mapId, x, y, mapId);
			}
		}

		delete mmap;
		loadedMMaps.erase(mapId);
		sLog->outInfo(LOG_FILTER_MAPS, "MMAP:unloadMap: Unloaded %03i.mmap", mapId);

		return true;
	}

	bool MMapManager::unloadMapInstance(uint32 mapId, uint32 instanceId)
	{
		// Check if we have this map loaded
		if (loadedMMaps.find(mapId) == loadedMMaps.end())
		{
			// File may not exist, therefore not loaded
			sLog->outInfo(LOG_FILTER_MAPS, "MMAP:unloadMapInstance: Asked to unload not loaded navmesh map %03u", mapId);
			return false;
		}

		MMapData* mmap = loadedMMaps[mapId];
		if (mmap->navMeshQueries.find(instanceId) == mmap->navMeshQueries.end())
		{
			sLog->outInfo(LOG_FILTER_MAPS, "MMAP:unloadMapInstance: Asked to unload not loaded dtNavMeshQuery mapId %03u instanceId %u", mapId, instanceId);
			return false;
		}

		dtNavMeshQuery* query = mmap->navMeshQueries[instanceId];

		dtFreeNavMeshQuery(query);
		mmap->navMeshQueries.erase(instanceId);
		sLog->outInfo(LOG_FILTER_MAPS, "MMAP:unloadMapInstance: Unloaded mapId %03u instanceId %u", mapId, instanceId);

		return true;
	}

	dtNavMesh const* MMapManager::GetNavMesh(uint32 mapId)
	{
		if (loadedMMaps.find(mapId) == loadedMMaps.end())
			return NULL;

		return loadedMMaps[mapId]->navMesh;
	}

	dtNavMeshQuery const* MMapManager::GetNavMeshQuery(uint32 mapId, uint32 instanceId)
	{
		if (loadedMMaps.find(mapId) == loadedMMaps.end())
			return NULL;

		MMapData* mmap = loadedMMaps[mapId];
		if (mmap->navMeshQueries.find(instanceId) == mmap->navMeshQueries.end())
		{
			// Allocate mesh query
			dtNavMeshQuery* query = dtAllocNavMeshQuery();
			ASSERT(query);
			if (dtStatusFailed(query->init(mmap->navMesh, 1024)))
			{
				dtFreeNavMeshQuery(query);
				sLog->outInfo(LOG_FILTER_MAPS, "MMAP:GetNavMeshQuery: Failed to initialize dtNavMeshQuery for mapId %03u instanceId %u", mapId, instanceId);
				return NULL;
			}

			sLog->outInfo(LOG_FILTER_MAPS, "MMAP:GetNavMeshQuery: created dtNavMeshQuery for mapId %03u instanceId %u", mapId, instanceId);
			mmap->navMeshQueries.insert(std::pair<uint32, dtNavMeshQuery*>(instanceId, query));
		}

		return mmap->navMeshQueries[instanceId];
	}
}