/* Key-Value Database
 *
 * Author: Jernej Turnsek <jernej.turnsek@gmail.com>
 * Based on KISSDB by Adam Ierymenko <adam.ierymenko@zerotier.com>
 * KVDB is in the public domain and is distributed with NO WARRANTY.
 *
 */

/* Note: big-endian systems will need changes to implement byte swapping
 * on hash table file I/O. Or you could just use it as-is if you don't care
 * that your database files will be unreadable on little-endian systems. */

#define _FILE_OFFSET_BITS 64

#include "kvdb.h"

#include <string.h>
#include <stdlib.h>
#include <stdint.h>

#define KVDB_HEADER_SIZE ((sizeof(uint64_t) * 3) + 4)

/* djb2 hash function */
static uint64_t KVDB_hash(const void *b, unsigned long len)
{
	unsigned long i;
	uint64_t hash = 5381;
	for(i = 0; i < len; ++i)
		hash = ((hash << 5) + hash) + (uint64_t)(((const uint8_t *)b)[i]);
	return hash;
}

int KVDB_open(
	KVDB *db,
	const char *path,
	int mode,
	unsigned long hash_table_size,
	unsigned long key_size,
	unsigned long value_size)
{
	uint64_t tmp;
	uint8_t tmp2[4];
	uint64_t *httmp;
	uint64_t *hash_tables_rea;

	db->f = fopen(path, ((mode == KVDB_OPEN_MODE_RWREPLACE) ? "w+b" : (((mode == KVDB_OPEN_MODE_RDWR) || (mode == KVDB_OPEN_MODE_RWCREAT)) ? "r+b" : "rb")));

	if (!db->f) {
		if (mode == KVDB_OPEN_MODE_RWCREAT) {
			db->f = fopen(path, "w+b");
		}
		if (!db->f)
			return KVDB_ERROR_IO;
	}

	if (fseeko(db->f, 0, SEEK_END)) {
		fclose(db->f);
		return KVDB_ERROR_IO;
	}
	if (ftello(db->f) < KVDB_HEADER_SIZE) {
		/* write header if not already present */
		if ((hash_table_size) && (key_size) && (value_size)) {
			if (fseeko(db->f, 0, SEEK_SET)) { 
				fclose(db->f); 
				return KVDB_ERROR_IO; 
			}
			tmp2[0] = 'K'; tmp2[1] = 'V'; tmp2[2] = 'B'; tmp2[3] = KVDB_VERSION;
			if (fwrite(tmp2, 4, 1, db->f) != 1) { 
				fclose(db->f); 
				return KVDB_ERROR_IO; 
			}
			tmp = hash_table_size;
			if (fwrite(&tmp, sizeof(uint64_t), 1, db->f) != 1) { 
				fclose(db->f); 
				return KVDB_ERROR_IO; 
			}
			tmp = key_size;
			if (fwrite(&tmp, sizeof(uint64_t), 1, db->f) != 1) { 
				fclose(db->f); 
				return KVDB_ERROR_IO; 
			}
			tmp = value_size;
			if (fwrite(&tmp, sizeof(uint64_t), 1, db->f) != 1) { 
				fclose(db->f); 
				return KVDB_ERROR_IO; 
			}
			fflush(db->f);
		} 
		else {
			fclose(db->f);
			return KVDB_ERROR_INVALID_PARAMETERS;
		}
	} 
	else {
		if (fseeko(db->f, 0, SEEK_SET)) { 
			fclose(db->f); 
			return KVDB_ERROR_IO; 
		}
		if (fread(tmp2, 4, 1, db->f) != 1) { 
			fclose(db->f); 
			return KVDB_ERROR_IO; 
		}
		if ((tmp2[0] != 'K') || (tmp2[1] != 'V') || (tmp2[2] != 'B') || (tmp2[3] != KVDB_VERSION)) {
			fclose(db->f);
			return KVDB_ERROR_CORRUPT_KVDBFILE;
		}
		if (fread(&tmp, sizeof(uint64_t), 1, db->f) != 1) { 
			fclose(db->f); 
			return KVDB_ERROR_IO; 
		}
		if (!tmp) {
			fclose(db->f);
			return KVDB_ERROR_CORRUPT_KVDBFILE;
		}
		hash_table_size = (unsigned long)tmp;
		if (fread(&tmp, sizeof(uint64_t), 1, db->f) != 1) { 
			fclose(db->f); 
			return KVDB_ERROR_IO; 
		}
		if (!tmp) {
			fclose(db->f);
			return KVDB_ERROR_CORRUPT_KVDBFILE;
		}
		key_size = (unsigned long)tmp;
		if (fread(&tmp, sizeof(uint64_t), 1, db->f) != 1) { 
			fclose(db->f); 
			return KVDB_ERROR_IO; }
		if (!tmp) {
			fclose(db->f);
			return KVDB_ERROR_CORRUPT_KVDBFILE;
		}
		value_size = (unsigned long)tmp;
	}

	db->hash_table_size = hash_table_size;
	db->key_size = key_size;
	db->value_size = value_size;
	db->hash_table_size_bytes = sizeof(uint64_t) * (hash_table_size + 1); /* [hash_table_size] == next table */

	httmp = malloc(db->hash_table_size_bytes);
	if (!httmp) {
		fclose(db->f);
		return KVDB_ERROR_MALLOC;
	}
	db->num_hash_tables = 0;
	db->hash_tables = (uint64_t *)0;
	while (fread(httmp, db->hash_table_size_bytes, 1, db->f) == 1) {
		hash_tables_rea = realloc(db->hash_tables, db->hash_table_size_bytes * (db->num_hash_tables + 1));
		if (!hash_tables_rea) {
			KVDB_close(db);
			free(httmp);
			return KVDB_ERROR_MALLOC;
		}
		db->hash_tables = hash_tables_rea;

		memcpy(((uint8_t *)db->hash_tables) + (db->hash_table_size_bytes * db->num_hash_tables), httmp, db->hash_table_size_bytes);
		++db->num_hash_tables;
		if (httmp[db->hash_table_size]) {
			if (fseeko(db->f, httmp[db->hash_table_size], SEEK_SET)) {
				KVDB_close(db);
				free(httmp);
				return KVDB_ERROR_IO;
			}
		} 
		else 
			break;
	}
	free(httmp);

	return 0;
}

void KVDB_close(KVDB *db)
{
	if (db->hash_tables)
		free(db->hash_tables);
	if (db->f)
		fclose(db->f);
	memset(db, 0, sizeof(KVDB));
}

int KVDB_get(KVDB *db,const void *key, void *vbuf)
{
	uint8_t tmp[256];
	const uint8_t *kptr;
	unsigned long klen,i;
	uint64_t hash = KVDB_hash(key, db->key_size) % (uint64_t)db->hash_table_size;
	uint64_t offset;
	uint64_t *cur_hash_table;
	long n;

	cur_hash_table = db->hash_tables;
	for(i = 0; i < db->num_hash_tables; ++i) {
		offset = cur_hash_table[hash];
		if (offset) {
			if (fseeko(db->f, offset, SEEK_SET))
				return KVDB_ERROR_IO;
			if (fread(&tmp[0], 1, 1, db->f) != 1) 
				return KVDB_ERROR_IO;
			kptr = (const uint8_t *)key;
			klen = db->key_size;
			while (klen) {
				n = (long)fread(&tmp[1], 1, (klen > (sizeof(tmp) - 1)) ? (sizeof(tmp) - 1) : klen, db->f);
				if (n > 0) {
					if (memcmp(kptr, &tmp[1], n))
						goto get_no_match_next_hash_table;
					kptr += n;
					klen -= (unsigned long)n;
				} 
				else 
					return 1; /* not found */
			}

			if (tmp[0] == 0) {
				/* deleted entry */
				return 1; /* not found */
			}

			if (fread(vbuf, db->value_size, 1, db->f) == 1)
				return 0; /* success */
			else 
				return KVDB_ERROR_IO;
		} 
		else 
			return 1; /* not found */
get_no_match_next_hash_table:
		cur_hash_table += db->hash_table_size + 1;
	}

	return 1; /* not found */
}

static int _kvdb_put(KVDB *db, const void *key, const void *value, bool delete)
{
	uint8_t tmp[256];
	const uint8_t *kptr;
	unsigned long klen,i;
	uint64_t hash = KVDB_hash(key, db->key_size) % (uint64_t)db->hash_table_size;
	uint64_t offset;
	uint64_t htoffset,lasthtoffset;
	uint64_t endoffset;
	uint64_t *cur_hash_table;
	uint64_t *hash_tables_rea;
	long n;

	lasthtoffset = htoffset = KVDB_HEADER_SIZE;
	cur_hash_table = db->hash_tables;
	for(i = 0; i < db->num_hash_tables; ++i) {
		offset = cur_hash_table[hash];
		if (offset) {
			/* rewrite if already exists */
			if (fseeko(db->f, offset, SEEK_SET))
				return KVDB_ERROR_IO;
			kptr = (const uint8_t *)key;
			klen = db->key_size;
			if (fread(&tmp[0], 1, 1, db->f) != 1) 
				return KVDB_ERROR_IO;
			while (tmp[0] && klen) {
				n = (long)fread(&tmp[1], 1, (klen > (sizeof(tmp) - 1)) ? (sizeof(tmp) - 1) : klen, db->f);
				if (n > 0) {
					if (memcmp(kptr, &tmp[1], n))
						goto put_no_match_next_hash_table;
					kptr += n;
					klen -= (unsigned long)n;
				}
			}

			if (delete) {
				if (fseeko(db->f, offset, SEEK_SET))
					return KVDB_ERROR_IO;
				tmp[0] = 0;
				if (fwrite(&tmp[0], 1, 1, db->f) != 1)
					return KVDB_ERROR_IO;
				fflush(db->f);
				return 0; /* success */
			}
			else {
				if (tmp[0] == 0) {
					/* deleted entry found */
					if (fseeko(db->f, offset, SEEK_SET))
						return KVDB_ERROR_IO;
					tmp[0] = 1;
					if (fwrite(&tmp[0], 1, 1, db->f) != 1)
						return KVDB_ERROR_IO;
					if (fwrite(key, db->key_size, 1, db->f) != 1)
						return KVDB_ERROR_IO;
				}
				else {
					/* C99 spec demands seek after fread(), required for Windows */
					fseeko(db->f, 0, SEEK_CUR);
				}
				if (fwrite(value, db->value_size, 1, db->f) == 1) {
					fflush(db->f);
					return 0; /* success */
				} 
				else 
					return KVDB_ERROR_IO;
			}
		} 
		else {
			if (delete) {
				/* entry not present */
				return KVDB_ERROR_IO;	
			}
			/* add if an empty hash table slot is discovered */
			if (fseeko(db->f, 0, SEEK_END))
				return KVDB_ERROR_IO;
			endoffset = ftello(db->f);
			
			tmp[0] = 1;
			if (fwrite(&tmp[0], 1, 1, db->f) != 1)
				return KVDB_ERROR_IO;
			if (fwrite(key, db->key_size, 1, db->f) != 1)
				return KVDB_ERROR_IO;
			if (fwrite(value, db->value_size, 1, db->f) != 1)
				return KVDB_ERROR_IO;

			if (fseeko(db->f, htoffset + (sizeof(uint64_t) * hash), SEEK_SET))
				return KVDB_ERROR_IO;
			if (fwrite(&endoffset, sizeof(uint64_t), 1, db->f) != 1)
				return KVDB_ERROR_IO;
			cur_hash_table[hash] = endoffset;

			fflush(db->f);
			return 0; /* success */
		}
put_no_match_next_hash_table:
		lasthtoffset = htoffset;
		htoffset = cur_hash_table[db->hash_table_size];
		cur_hash_table += (db->hash_table_size + 1);
	}

	if (delete) {
		/* entry not present */
		return KVDB_ERROR_IO;	
	}
	/* if no existing slots, add a new page of hash table entries */
	if (fseeko(db->f, 0, SEEK_END))
		return KVDB_ERROR_IO;
	endoffset = ftello(db->f);

	hash_tables_rea = realloc(db->hash_tables, db->hash_table_size_bytes * (db->num_hash_tables + 1));
	if (!hash_tables_rea)
		return KVDB_ERROR_MALLOC;
	db->hash_tables = hash_tables_rea;
	cur_hash_table = &(db->hash_tables[(db->hash_table_size + 1) * db->num_hash_tables]);
	memset(cur_hash_table, 0, db->hash_table_size_bytes);

	cur_hash_table[hash] = endoffset + db->hash_table_size_bytes; /* where new entry will go */

	if (fwrite(cur_hash_table, db->hash_table_size_bytes, 1, db->f) != 1)
		return KVDB_ERROR_IO;
	
	tmp[0] = 1;
	if (fwrite(&tmp[0], 1, 1, db->f) != 1)
		return KVDB_ERROR_IO;
	if (fwrite(key, db->key_size, 1, db->f) != 1)
		return KVDB_ERROR_IO;
	if (fwrite(value, db->value_size, 1, db->f) != 1)
		return KVDB_ERROR_IO;

	if (db->num_hash_tables) {
		if (fseeko(db->f, lasthtoffset + (sizeof(uint64_t) * db->hash_table_size), SEEK_SET))
			return KVDB_ERROR_IO;
		if (fwrite(&endoffset, sizeof(uint64_t), 1, db->f) != 1)
			return KVDB_ERROR_IO;
		db->hash_tables[((db->hash_table_size + 1) * (db->num_hash_tables - 1)) + db->hash_table_size] = endoffset;
	}

	++db->num_hash_tables;

	fflush(db->f);
	return 0; /* success */
}

int KVDB_put(KVDB *db, const void *key, const void *value)
{
	return _kvdb_put(db, key, value, false);
}

int KVDB_delete(KVDB *db, const void *key)
{
	return _kvdb_put(db, key, NULL, true);
}

void KVDB_iterator_init(KVDB *db, KVDB_ITERATOR *dbi)
{
	dbi->db = db;
	dbi->h_no = 0;
	dbi->h_idx = 0;
}

int KVDB_iterator_next(KVDB_ITERATOR *dbi, void *kbuf, void *vbuf)
{
	uint64_t offset;
	uint8_t tmp;

	while (1) {
		if ((dbi->h_no < dbi->db->num_hash_tables) && (dbi->h_idx < dbi->db->hash_table_size)) {
			while (!(offset = dbi->db->hash_tables[((dbi->db->hash_table_size + 1) * dbi->h_no) + dbi->h_idx])) {
				if (++dbi->h_idx >= dbi->db->hash_table_size) {
					dbi->h_idx = 0;
					if (++dbi->h_no >= dbi->db->num_hash_tables)
						return 0;
				}
			}
			if (fseeko(dbi->db->f, offset, SEEK_SET))
				return KVDB_ERROR_IO;
			if (fread(&tmp, 1, 1, dbi->db->f) != 1)
				return KVDB_ERROR_IO;
			if (fread(kbuf, dbi->db->key_size, 1, dbi->db->f) != 1)
				return KVDB_ERROR_IO;
			if (fread(vbuf, dbi->db->value_size, 1, dbi->db->f) != 1)
				return KVDB_ERROR_IO;
			if (++dbi->h_idx >= dbi->db->hash_table_size) {
				dbi->h_idx = 0;
				++dbi->h_no;
			}
			if (!tmp)
				continue;
			return 1;
		}
		break;
	}

	return 0;
}
