#include <stdlib.h>
#include <sys/mman.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>

#include "xmalloc.h"

#define PAGE_SIZE 4096
#define BUCKET_COUNT 10

const int bucket_sizes[BUCKET_COUNT] = {4, 8, 16, 32, 64, 128, 256, 512, 1024, 2048};
const int bitfield_sizes[BUCKET_COUNT] = {124, 63, 32, 16, 8, 4, 2, 1, 1, 1};
const unsigned char full_segment = 255;
__thread void* buckets[BUCKET_COUNT] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
unsigned char mask = 1;

int
bitfield_size(int size)
{
	int bit_count = (PAGE_SIZE - sizeof(void*)) * 8 / (size * 8 + 1);
	return bit_count / 8 + ((bit_count % 8) ? 1 : 0);
}

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

void*
xmalloc(size_t bytes)
{
	bytes += 2 * sizeof(short);
	// bucket_index | bitfield offset (bits)

	int bucket_index = get_bucket_index(bytes);
	void* page = buckets[bucket_index];
	void* previous = 0;
	int bucket_size = bucket_sizes[bucket_index];
	int bitfield_size = bitfield_sizes[bucket_index];
	while(page)
	{
		void* next = *((void**)page);
		unsigned char* bitfield = page + sizeof(void*);
		for(int i = 0; i < bitfield_size; ++i)
		{
			if(*bitfield != full_segment)
			{
				for(int j = 0; j < 8; ++j)
				{
					if(~bitfield[i] & (mask << (7 - j)))
					{
						short bit = i*8 + j;
						bitfield[i] = bitfield[i] | (mask << (7 - j));
						short* base = (short*)(page + sizeof(void*) + bitfield_sizes[bucket_index] + bit * bucket_size);
						*base = bucket_index;
						*(base + 1) = bit;
						return (base + 2);
					}
				}
			}
			++bitfield;
		}

		previous = page;
		page = next;
	}

	void* new_page = mmap(0, PAGE_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
	if(!previous)
		buckets[bucket_index] = new_page;
	else
		*((void**)previous) = new_page;

	*((void**)new_page) = 0;
	char* bitfield = (char*)(new_page + sizeof(void*));
	memset(bitfield, 0, bitfield_size);
	bitfield[0] = 1 << 7;
	short* base = (short*)((void*)bitfield + bitfield_size);
	*base = bucket_index;
	*(base + 1) = 0;
	return (base + 2);
}

void
xfree(void* ptr)
{
	short index = *((short*)(ptr - 2*sizeof(short)));
	short offset = *((short*)(ptr - sizeof(short)));
	int bucket_size = bucket_sizes[index];
	char* bitfield = ptr - bucket_size * offset - bitfield_sizes[index];
	bitfield[offset / 8] = bitfield[offset / 8] & ~(mask << (offset % 8));
}

void*
xrealloc(void* prev, size_t bytes)
{
	// NAIVE
	void* new_loc = xmalloc(bytes);
	memcpy(prev, new_loc, bytes);
	xfree(prev);
	return new_loc;
}

