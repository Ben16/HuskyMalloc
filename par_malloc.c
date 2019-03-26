#include <stdlib.h>
#include <sys/mman.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>

#include "xmalloc.h"

#define PAGE_SIZE 8192
#define BUCKET_COUNT 16

const int bucket_sizes[BUCKET_COUNT] = {16, 24, 32, 48, 64, 96, 128, 192, 256, 384, 512, 768, 1024, 1536, 2048, 3072};
__thread void* buckets[BUCKET_COUNT] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
__thread void** free_list[BUCKET_COUNT] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};

typedef long tag_t;

int
get_bucket_index(size_t bytes)
{
	for(int i = 0; i < BUCKET_COUNT; ++i)
	{
		if(bytes <= bucket_sizes[i])
			return i;
	}
	return -1;
}

void
fill_space(void* loc, size_t remaining)
{
	int bucket_index = get_bucket_index(remaining) - 1;
	if(bucket_index < 0)
		return;
	int bucket_size = bucket_sizes[bucket_index];
	*((void**)loc) = (void*)free_list[bucket_index];
	free_list[bucket_index] = (void**)loc;
	fill_space(loc + bucket_size, remaining - bucket_size);

	/*if(remaining < bucket_sizes[0])
		return;

	int index = get_bucket_index(remaining) - 1;
	int bucket_size;
	while(index > 0)
	{
		bucket_size = bucket_sizes[index];
		if(!free_list[index])
		{
			*((void**)loc) = 0;
			free_list[index] = (void**)loc;
			fill_space(loc + bucket_size, remaining - bucket_size);
			return;
		}
		--index;
	}*/
	
	// all buckets have free list entries; dump remaining into 16 bucket
	/*bucket_size = bucket_sizes[index];
	int entries = remaining / bucket_size;
	void** working = (void**)loc;
	for(int i = 0; i < entries; ++i, working = (void**)((void*)working + bucket_size))
	{
		*working = free_list[index];
		free_list[index] = working;
	}*/
}

void*
xmalloc(size_t bytes)
{
	bytes += sizeof(tag_t);

	int bucket_index = get_bucket_index(bytes);
	int bucket_size = bucket_sizes[bucket_index];
	if(bucket_index < 0) //allocations > 1 page
	{
		bytes = (bytes / PAGE_SIZE + ((bytes % PAGE_SIZE) ? 1 : 0)) * PAGE_SIZE;
		tag_t* mem = (tag_t*)mmap(0, bytes, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
		*mem = bytes;
		return mem + 1;
	}
	else if(free_list[bucket_index])
	{
		void** entry = free_list[bucket_index];
		free_list[bucket_index] = (void**)(*entry);
		tag_t* tag = (tag_t*)entry;
		*tag = bucket_index;
		return tag + 1;
	}
	else
	{
		for(int i = bucket_index + 1; i < BUCKET_COUNT; ++i)
		{
			if(free_list[i])
			{
				void** entry = free_list[i];
				free_list[i] = (void**)(*entry);
				tag_t* tag = (tag_t*)entry;
				*tag = bucket_index;
				fill_space((void*)entry + bucket_size, bucket_sizes[i] - bucket_size);
				return tag + 1;
			}
		}

		void* new_page = mmap(0, PAGE_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);

		void* previous = 0;
		void* page = buckets[bucket_index];
		while(page)
		{
			previous = page;
			page = *((void**)page);
		}
		if(!previous)
			buckets[bucket_index] = new_page;
		else
			*((void**)previous) = new_page;

		*((void**)new_page) = 0;
		tag_t* base = new_page + sizeof(void*);
		*base = bucket_index;
		
		int entries = (PAGE_SIZE - sizeof(void*)) / bucket_size - 1;
		void** working = (void**)((void*)base + bucket_size);
		for(int i = 0; i < entries; ++i, working = (void**)((void*)working + bucket_size))
		{
			*working = free_list[bucket_index];
			free_list[bucket_index] = working;
		}
		fill_space((void*)working, PAGE_SIZE - (entries + 1) * bucket_size - sizeof(void*));
		return (base + 1);
	}
}

void
xfree(void* ptr)
{
	long identifier = *((tag_t*)(ptr - sizeof(tag_t)));
	if(identifier >= BUCKET_COUNT)
		munmap(ptr - sizeof(tag_t), identifier);
	else
	{
		void** base = (void**)(ptr - sizeof(tag_t));
		*base = free_list[identifier];
		free_list[identifier] = base;
	}
}

void*
xrealloc(void* prev, size_t bytes)
{
	//printf("Realloc to %ld bytes\n", bytes);
	void* new_loc = xmalloc(bytes);
	memcpy(new_loc, prev, bytes);
	xfree(prev);
	return new_loc;
}

