/*
 * region-allocator.c -- region based memory allocator.
 *
 * Erik Rozendaal, <erik@nlnetlabs.nl>
 *
 * Copyright (c) 2003, NLnet Labs. All rights reserved.
 *
 * This software is an open source.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * Redistributions of source code must retain the above copyright notice,
 * this list of conditions and the following disclaimer.
 *
 * Redistributions in binary form must reproduce the above copyright notice,
 * this list of conditions and the following disclaimer in the documentation
 * and/or other materials provided with the distribution.
 *
 * Neither the name of the NLNET LABS nor the names of its contributors may
 * be used to endorse or promote products derived from this software without
 * specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 */

#include <config.h>

#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include "region-allocator.h"

#define ALIGN_UP(x, s)     (((x) + s - 1) & (~(s - 1)))

#define CHUNK_SIZE         4096
#define LARGE_OBJECT_SIZE  (CHUNK_SIZE / 8)
#define ALIGNMENT          (sizeof(void *))

typedef struct cleanup cleanup_type;
struct cleanup
{
	void (*action)(void *);
	void *data;
};

struct region
{
	size_t        total_allocated;
	size_t        small_objects;
	size_t        large_objects;
	size_t        chunk_count;
	size_t        unused_space; /* Unused space due to alignment, etc. */
	
	size_t        allocated;
	char         *initial_data;
	char         *data;

	void         *(*allocator)(size_t);
	void          (*deallocator)(void *);
    
	size_t        maximum_cleanup_count;
	size_t        cleanup_count;
	cleanup_type *cleanups;
};

static region_type *current_region = NULL;

region_type *
region_create(void *(*allocator)(size_t size),
	      void (*deallocator)(void *))
{
	region_type *result = allocator(sizeof(region_type));
	if (!result) return NULL;

	result->total_allocated = 0;
	result->small_objects = 0;
	result->large_objects = 0;
	result->chunk_count = 1;
	result->unused_space = 0;
	
	result->allocated = 0;
	result->data = allocator(CHUNK_SIZE);
	if (!result->data) {
		deallocator(result);
		return NULL;
	}
	result->initial_data = result->data;
	
	result->allocator = allocator;
	result->deallocator = deallocator;

	result->maximum_cleanup_count = 16;
	result->cleanup_count = 0;
	result->cleanups = allocator(result->maximum_cleanup_count * sizeof(cleanup_type));
	if (!result->cleanups) {
		deallocator(result->data);
		deallocator(result);
		return NULL;
	}
    
	return result;
}

void
region_destroy(region_type *region)
{
	void (*deallocator)(void *);
	if (!region)
		return;

	deallocator = region->deallocator;

	region_free_all(region);
	deallocator(region->cleanups);
	deallocator(region->initial_data);
	deallocator(region);
}

void
region_set_current(region_type *region)
{
	current_region = region;
}

region_type *
region_get_current(void)
{
	return current_region;
}

size_t
region_add_cleanup(region_type *region, void (*action)(void *), void *data)
{
	assert(action);
    
	if (region->cleanup_count >= region->maximum_cleanup_count) {
		cleanup_type *cleanups = region->allocator(
			2 * region->maximum_cleanup_count * sizeof(cleanup_type));
		if (!cleanups) return 0;

		memcpy(cleanups, region->cleanups,
		       region->cleanup_count * sizeof(cleanup_type));
		region->deallocator(region->cleanups);

		region->cleanups = cleanups;
		region->maximum_cleanup_count *= 2;
	}

	region->cleanups[region->cleanup_count].action = action;
	region->cleanups[region->cleanup_count].data = data;

	++region->cleanup_count;
	return region->cleanup_count;
}

void *
region_alloc(region_type *region, size_t size)
{
	size_t aligned_size;
	void *result;

	if (size == 0) {
		size = 1;
	}
	aligned_size = ALIGN_UP(size, ALIGNMENT);

	if (aligned_size >= LARGE_OBJECT_SIZE) {
		result = region->allocator(size);
		if (!result) return NULL;
        
		if (!region_add_cleanup(region, region->deallocator, result)) {
			region->deallocator(result);
			return NULL;
		}
        
		region->total_allocated += size;
		++region->large_objects;
		
		return result;
	}
    
	if (region->allocated + aligned_size > CHUNK_SIZE) {
		void *chunk = region->allocator(CHUNK_SIZE);
		if (!chunk) return NULL;

		++region->chunk_count;
		region->unused_space += CHUNK_SIZE - region->allocated;
		
		region_add_cleanup(region, region->deallocator, chunk);
		region->allocated = 0;
		region->data = chunk;
	}

	result = region->data + region->allocated;
	region->allocated += aligned_size;

	region->total_allocated += aligned_size;
	region->unused_space += aligned_size - size;
	++region->small_objects;
	
	return result;
}

void *
region_alloc_init(region_type *region, const void *init, size_t size)
{
	void *result = region_alloc(region, size);
	if (!result) return NULL;
	memcpy(result, init, size);
	return result;
}

void *
region_alloc_zero(region_type *region, size_t size)
{
	void *result = region_alloc(region, size);
	if (!result) return NULL;
	memset(result, 0, size);
	return result;
}

void *
region_alloc_current(size_t size)
{
	return region_alloc(current_region, size);
}

void
region_free_all(region_type *region)
{
	size_t i;
	assert(region);
	assert(region->cleanups);
    
	i = region->cleanup_count;
	while (i > 0) {
		--i;
		assert(region->cleanups[i].action);
		region->cleanups[i].action(region->cleanups[i].data);
	}

	region->data = region->initial_data;
	region->cleanup_count = 0;
	region->allocated = 0;

	region->total_allocated = 0;
	region->small_objects = 0;
	region->large_objects = 0;
	region->chunk_count = 1;
	region->unused_space = 0;
}


char *
region_strdup(region_type *region, const char *string)
{
	size_t len = strlen(string);
	char *result = region_alloc(region, len + 1);
	if (!result) return NULL;
	memcpy(result, string, len + 1);
	return result;
}

void
region_dump_stats(region_type *region, FILE *out)
{
	fprintf(out, "%lu objects (%lu small/%lu large), %lu bytes allocated (%lu wasted) in %lu chunks, %lu cleanups",
		(unsigned long) (region->small_objects + region->large_objects),
		(unsigned long) region->small_objects,
		(unsigned long) region->large_objects,
		(unsigned long) region->total_allocated,
		(unsigned long) region->unused_space,
		(unsigned long) region->chunk_count,
		(unsigned long) region->cleanup_count);
}


#ifdef TEST

#include <stdio.h>

#define ALLOCS 128

void *
xalloc(size_t size)
{
	void *result = malloc(size);
	assert(result);
	return result;
}

void region_loop(void);
void malloc_loop(void);

int
main(void)
{
	char buf[LARGE_OBJECT_SIZE];
	size_t i;
	void *a;
	void *b;
	void *c;
	region_type *r = region_create(xalloc, free);
	assert(r);

	assert(r->cleanup_count == 0);

	region_alloc(r, 3);
	assert((intptr_t) region_alloc(r, 1) % ALIGNMENT == 0);
    
	a = region_alloc(r, 41);
	assert(a);
	assert(r->cleanup_count == 0);

	assert((intptr_t) a % ALIGNMENT == 0);
    
	b = region_alloc(r, LARGE_OBJECT_SIZE);
	assert(b);
	assert(r->cleanup_count == 1);

	c = region_alloc(r, 100);
	assert(c);
	assert(r->cleanup_count == 1);

	assert((intptr_t) c % ALIGNMENT == 0);

	assert(a != b);
	assert(a != c);
	assert(b != c);

	memset(a, 'a', 40);
	memset(b, 'b', LARGE_OBJECT_SIZE);
	memset(c, 'c', 100);
    
	memset(buf, 'a', sizeof(buf));
	assert(memcmp(a, buf, 40) == 0);
    
	memset(buf, 'b', sizeof(buf));
	assert(memcmp(b, buf, LARGE_OBJECT_SIZE) == 0);
    
	memset(buf, 'c', sizeof(buf));
	assert(memcmp(c, buf, 100) == 0);
    
	region_free_all(r);
	assert(r->cleanup_count == 0);

	for (i = 0; i < CHUNK_SIZE / 128; ++i) {
		region_alloc(r, 128);
	}

	assert(r->cleanup_count == 0);
	assert(r->allocated = CHUNK_SIZE);

	region_free_all(r);
	assert(r->cleanup_count == 0);
	assert(r->allocated == 0);

	region_destroy(r);
    
	region_loop();
	malloc_loop();
    
	return 0;
}

void
region_loop(void)
{
	int i;
	region_type *r = region_create(xalloc, free);
    
	for (i = 0; i < 100000 * ALLOCS; ++i) {
		region_alloc(r, i % 50 * 15);
		if (i % ALLOCS == ALLOCS - 1)
			region_free_all(r);
	}
	region_destroy(r);
}

void
malloc_loop(void)
{
	int i;
	void *ptrs[ALLOCS];
    
	for (i = 0; i < 100000 * ALLOCS; ++i) {
		ptrs[i % ALLOCS] = xalloc(i % 50 * 15);
		if (i % ALLOCS == ALLOCS - 1) {
			int j;
			for (j = 0; j < ALLOCS; ++j) {
				free(ptrs[j]);
			}
		}
	}
}

#endif /* TEST */
