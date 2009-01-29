// Copyright (c) Athena Dev Teams - Licensed under GNU GPL
// For more information, see LICENCE in the main folder

#include "../common/cbasetypes.h"
#include "../common/db.h"
#include "../common/lock.h"
#include "../common/malloc.h"
#include "../common/mmo.h"
#include "../common/showmsg.h"
#include "../common/strlib.h"
#include "castledb.h"
#include "charserverdb_txt.h"
#include <stdio.h>
#include <string.h>

#define START_CASTLE_NUM 1

/// internal structure
typedef struct CastleDB_TXT
{
	CastleDB vtable;      // public interface

	CharServerDB_TXT* owner;
	DBMap* castles;       // in-memory castle storage
	int next_castle_id;   // auto_increment

	const char* castle_db; // castle data storage file

} CastleDB_TXT;

typedef struct CastleDBIterator_TXT
{
	CastleDBIterator vtable;      // public interface

	DBIterator* iter;

} CastleDBIterator_TXT;

/// internal functions
static bool castle_db_txt_init(CastleDB* self);
static void castle_db_txt_destroy(CastleDB* self);
static bool castle_db_txt_sync(CastleDB* self);
static bool castle_db_txt_create(CastleDB* self, struct guild_castle* gc);
static bool castle_db_txt_remove(CastleDB* self, const int castle_id);
static bool castle_db_txt_save(CastleDB* self, const struct guild_castle* gc);
static bool castle_db_txt_load_num(CastleDB* self, struct guild_castle* gc, int castle_id);
static CastleDBIterator* castle_db_txt_iterator(CastleDB* self);
static void castle_db_txt_iter_destroy(CastleDBIterator* self);
static bool castle_db_txt_iter_next(CastleDBIterator* self, struct guild_castle* gc);

static bool mmo_castle_fromstr(struct guild_castle* gc, char* str);
static bool mmo_castle_tostr(const struct guild_castle* gc, char* str);
static bool mmo_castle_sync(CastleDB_TXT* db);

/// public constructor
CastleDB* castle_db_txt(CharServerDB_TXT* owner)
{
	CastleDB_TXT* db = (CastleDB_TXT*)aCalloc(1, sizeof(CastleDB_TXT));

	// set up the vtable
	db->vtable.init      = &castle_db_txt_init;
	db->vtable.destroy   = &castle_db_txt_destroy;
	db->vtable.sync      = &castle_db_txt_sync;
	db->vtable.create    = &castle_db_txt_create;
	db->vtable.remove    = &castle_db_txt_remove;
	db->vtable.save      = &castle_db_txt_save;
	db->vtable.load_num  = &castle_db_txt_load_num;
	db->vtable.iterator  = &castle_db_txt_iterator;

	// initialize to default values
	db->owner = owner;
	db->castles = NULL;
	db->next_castle_id = START_CASTLE_NUM;

	// other settings
	db->castle_db = db->owner->file_castles;

	return &db->vtable;
}


/* ------------------------------------------------------------------------- */


static bool castle_db_txt_init(CastleDB* self)
{
	CastleDB_TXT* db = (CastleDB_TXT*)self;
	DBMap* castles;

	char line[16384];
	FILE* fp;
	int count = 0;

	// create castle database
	db->castles = idb_alloc(DB_OPT_RELEASE_DATA);
	castles = db->castles;

	// open data file
	fp = fopen(db->castle_db, "r");
	if( fp == NULL )
		return 1;

	// load data file
	while( fgets(line, sizeof(line), fp) )
	{
		struct guild_castle* gc = (struct guild_castle *) aCalloc(sizeof(struct guild_castle), 1);

		if( !mmo_castle_fromstr(gc, line) )
		{
			ShowError("castle_db_txt_init: skipping invalid data: %s", line);
			aFree(gc);
			continue;
		}

		// record entry in db
		idb_put(castles, gc->castle_id, gc);

		if( gc->castle_id >= db->next_castle_id )
			db->next_castle_id = gc->castle_id + 1;

		count++;
	}

	// close data file
	fclose(fp);

	if( count == 0 )
	{// empty castles file, set up a default layout
		int i;
		ShowStatus(" %s - making Default Data...\n", db->castle_db);
		for( i = 0; i < MAX_GUILDCASTLE; ++i )
		{
			struct guild_castle* gc = (struct guild_castle *) aCalloc(sizeof(struct guild_castle), 1);
			gc->castle_id = i;
			idb_put(castles, gc->castle_id, gc);
		}
		ShowStatus(" %s - making done\n", db->castle_db);
	}

	return true;
}

static void castle_db_txt_destroy(CastleDB* self)
{
	CastleDB_TXT* db = (CastleDB_TXT*)self;
	DBMap* castles = db->castles;

	// write data
	mmo_castle_sync(db);

	// delete castle database
	castles->destroy(castles, NULL);
	db->castles = NULL;

	// delete entire structure
	aFree(db);
}

static bool castle_db_txt_sync(CastleDB* self)
{
	CastleDB_TXT* db = (CastleDB_TXT*)self;
	return mmo_castle_sync(db);
}

static bool castle_db_txt_create(CastleDB* self, struct guild_castle* gc)
{
/*
*/
}

static bool castle_db_txt_remove(CastleDB* self, const int castle_id)
{
/*
*/
}

static bool castle_db_txt_save(CastleDB* self, const struct guild_castle* gc)
{
	CastleDB_TXT* db = (CastleDB_TXT*)self;
	DBMap* castles = db->castles;
	int castle_id = gc->castle_id;

	// retrieve previous data
	struct guild_castle* tmp = idb_get(castles, castle_id);
	if( tmp == NULL )
	{// error condition - entry not found
		return false;
	}
	
	// overwrite with new data
	memcpy(tmp, gc, sizeof(struct guild_castle));

	return true;
}

static bool castle_db_txt_load_num(CastleDB* self, struct guild_castle* gc, int castle_id)
{
	CastleDB_TXT* db = (CastleDB_TXT*)self;
	DBMap* castles = db->castles;

	// retrieve data
	struct guild_castle* tmp = idb_get(castles, castle_id);
	if( tmp == NULL )
	{// entry not found
		return false;
	}

	// store it
	memcpy(gc, tmp, sizeof(struct guild_castle));

	return true;
}

/// Returns an iterator over all the characters.
static CastleDBIterator* castle_db_txt_iterator(CastleDB* self)
{
	CastleDB_TXT* db = (CastleDB_TXT*)self;
	DBMap* castles = db->castles;
	CastleDBIterator_TXT* iter = (CastleDBIterator_TXT*)aCalloc(1, sizeof(CastleDBIterator_TXT));

	// set up the vtable
	iter->vtable.destroy = &castle_db_txt_iter_destroy;
	iter->vtable.next    = &castle_db_txt_iter_next;

	// fill data
	iter->iter = db_iterator(castles);

	return &iter->vtable;
}

/// Destroys this iterator, releasing all allocated memory (including itself).
static void castle_db_txt_iter_destroy(CastleDBIterator* self)
{
	CastleDBIterator_TXT* iter = (CastleDBIterator_TXT*)self;
	dbi_destroy(iter->iter);
	aFree(iter);
}

/// Fetches the next castle.
static bool castle_db_txt_iter_next(CastleDBIterator* self, struct guild_castle* gc)
{
	CastleDBIterator_TXT* iter = (CastleDBIterator_TXT*)self;
	struct guild_castle* tmp;

	while( true )
	{
		tmp = (struct guild_castle*)dbi_next(iter->iter);
		if( tmp == NULL )
			return false;// not found

		memcpy(gc, tmp, sizeof(struct guild_castle));
		return true;
	}
}


/// parses the castle data string into a castle data structure
static bool mmo_castle_fromstr(struct guild_castle* gc, char* str)
{
	int dummy;

	memset(gc, 0, sizeof(struct guild_castle));

	// structure of guild castle with the guardian hp included (old one)
	if( sscanf(str, "%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d",
		&gc->castle_id, &gc->guild_id, &gc->economy, &gc->defense,
		&gc->triggerE, &gc->triggerD, &gc->nextTime, &gc->payTime, &gc->createTime, &gc->visibleC,
		&gc->guardian[0].visible, &gc->guardian[1].visible, &gc->guardian[2].visible, &gc->guardian[3].visible,
		&gc->guardian[4].visible, &gc->guardian[5].visible, &gc->guardian[6].visible, &gc->guardian[7].visible,
		&dummy, &dummy, &dummy, &dummy, &dummy, &dummy, &dummy, &dummy) != 26 )
	// structure of guild castle without the hps (current one)
	if( sscanf(str, "%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d",
		&gc->castle_id, &gc->guild_id, &gc->economy, &gc->defense,
		&gc->triggerE, &gc->triggerD, &gc->nextTime, &gc->payTime, &gc->createTime, &gc->visibleC,
		&gc->guardian[0].visible, &gc->guardian[1].visible, &gc->guardian[2].visible, &gc->guardian[3].visible,
		&gc->guardian[4].visible, &gc->guardian[5].visible, &gc->guardian[6].visible, &gc->guardian[7].visible) != 18 )
		return false;

	return true;
}

/// serializes the castle data structure into the provided string
static bool mmo_castle_tostr(const struct guild_castle* gc, char* str)
{
	int len;

	len = sprintf(str, "%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d",
	              gc->castle_id, gc->guild_id, gc->economy, gc->defense, gc->triggerE,
	              gc->triggerD, gc->nextTime, gc->payTime, gc->createTime, gc->visibleC,
	              gc->guardian[0].visible, gc->guardian[1].visible, gc->guardian[2].visible, gc->guardian[3].visible,
	              gc->guardian[4].visible, gc->guardian[5].visible, gc->guardian[6].visible, gc->guardian[7].visible);

	return true;
}

static bool mmo_castle_sync(CastleDB_TXT* db)
{
	DBIterator* iter;
	void* data;
	FILE *fp;
	int lock;

	// save castle data
	fp = lock_fopen(db->castle_db, &lock);
	if( fp == NULL )
	{
		ShowError("mmo_castle_sync: can't write [%s] !!! data is lost !!!\n", db->castle_db);
		return false;
	}

	iter = db->castles->iterator(db->castles);
	for( data = iter->first(iter,NULL); iter->exists(iter); data = iter->next(iter,NULL) )
	{
		struct guild_castle* gc = (struct guild_castle*) data;
		char line[16384];

		mmo_castle_tostr(gc, line);
		fprintf(fp, "%s\n", line);
	}
	iter->destroy(iter);

	lock_fclose(fp, db->castle_db, &lock);

	return true;
}
