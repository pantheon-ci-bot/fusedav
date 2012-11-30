#ifndef foostatcachehfoo
#define foostatcachehfoo

/***
  This file is part of fusedav.

  fusedav is free software; you can redistribute it and/or modify it
  under the terms of the GNU General Public License as published by
  the Free Software Foundation; either version 2 of the License, or
  (at your option) any later version.
  
  fusedav is distributed in the hope that it will be useful, but WITHOUT
  ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
  or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public
  License for more details.
  
  You should have received a copy of the GNU General Public License
  along with fusedav; if not, write to the Free Software Foundation,
  Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
***/

#include <sys/stat.h>
#include <leveldb/c.h>

typedef leveldb_t stat_cache_t;

struct stat_cache_iterator {
    leveldb_iterator_t *ldb_iter;
    char *key_prefix;
    size_t key_prefix_len;
};

struct stat_cache_value {
    struct stat st;
    struct timespec local_generation;
    char *remote_generation;
};

int stat_cache_open(stat_cache_t **cache, char *storage_path);
int stat_cache_close(stat_cache_t *cache);

struct timespec stat_cache_now(void);

struct stat_cache_value *stat_cache_value_get(stat_cache_t *cache, const char *key);
int stat_cache_value_set(stat_cache_t *cache, const char *key, struct stat_cache_value *value);
void stat_cache_value_free(struct stat_cache_value *value);

int stat_cache_delete(stat_cache_t *cache, const char* key);
int stat_cache_delete_parent(stat_cache_t *cache, const char *key);
int stat_cache_delete_older(stat_cache_t *cache, const char *key_prefix, struct timespec min_time);

int stat_cache_enumerate(stat_cache_t *cache, const char *key_prefix, void (*f) (const char *key, const char *child_key, void *user), void *user);

#endif
