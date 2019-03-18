#include <stdlib.h>
#include <sys/mman.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>

#include "hmalloc.h"

/*
  typedef struct hm_stats {
  long pages_mapped;
  long pages_unmapped;
  long chunks_allocated;
  long chunks_freed;
  long free_length;
  } hm_stats;
*/

const size_t PAGE_SIZE = 4096;
static hm_stats stats; // This initializes the stats to 0.

typedef struct node Node;
typedef struct node
{
	size_t size; //INCLUDES the size of the Node
	Node* next;
} Node;

Node* free_list_head = 0;
pthread_once_t init_flag = PTHREAD_ONCE_INIT;
pthread_mutex_t mutex;

void init_mutex()
{
	pthread_mutex_init(&mutex, 0);
}

void push_free_list(Node* node)
{
	Node* previous = 0;
	Node* current = free_list_head;
	while(current)
	{
		if(current > node)
		{
			if(previous)
				previous->next = node;
			else
				free_list_head = node;
			node->next = current;
			return;
		}
		previous = current;
		current = current->next;
	}

	if(!previous)
		free_list_head = node;
	else
		previous->next = node;
	node->next = 0;
}

long free_list_length()
{
	pthread_once(&init_flag, init_mutex);
	pthread_mutex_lock(&mutex);

	long sum = 0;
	Node* current = free_list_head;
	while(current)
	{
		++sum;
		current = current->next;
	}
	pthread_mutex_unlock(&mutex);
	return sum;
}

hm_stats* hgetstats()
{
    stats.free_length = free_list_length();
    return &stats;
}

void hprintstats()
{
    stats.free_length = free_list_length();
    fprintf(stderr, "\n== husky malloc stats ==\n");
    fprintf(stderr, "Mapped:   %ld\n", stats.pages_mapped);
    fprintf(stderr, "Unmapped: %ld\n", stats.pages_unmapped);
    fprintf(stderr, "Allocs:   %ld\n", stats.chunks_allocated);
    fprintf(stderr, "Frees:    %ld\n", stats.chunks_freed);
    fprintf(stderr, "Freelen:  %ld\n", stats.free_length);
}

static size_t div_up(size_t xx, size_t yy)
{
    // This is useful to calculate # of pages
    // for large allocations.
    size_t zz = xx / yy;

    if (zz * yy == xx)
        return zz;
    else
        return zz + 1;
}

void coalesce()
{
	Node* current = free_list_head;
	while(current)
	{
		if((void*)current + current->size == current->next)
		{
			current->size += current->next->size;
			current->next = current->next->next;
		}
		else
			current = current->next;
	}
}

void* hmalloc(size_t size)
{
	pthread_once(&init_flag, init_mutex);
	pthread_mutex_lock(&mutex);

	stats.chunks_allocated += 1;
	// the chunk size is emplaced before the memory
	// note that this value does NOT include the size of the marker itself
	size += sizeof(size_t);
	if(size % 16)
		size += 16 - (size % 16);

	size_t pages = div_up(size, PAGE_SIZE);
	if(pages == 1)
	{
		Node* previous = 0;
		Node* current = free_list_head;
		while(current)
		{
			if(current->size >= size)
			{
				if(current->size - size >= sizeof(Node))
				{
					Node* new_node = (Node*)((void*)current + size);
					new_node->size = current->size - size;
					new_node->next = current->next;
					if(previous)
						previous->next = new_node;
					else
						free_list_head = new_node;
				}
				else
				{
					if(previous)
						previous->next = current->next;
					else
						free_list_head = current->next;
				}
				*((size_t*)current) = size - sizeof(size_t); 
				pthread_mutex_unlock(&mutex);
				return (void*)current + sizeof(size_t);
			}
			previous = current;
			current = current->next;
		}
	}

	// if we got here, we either need multiple pages, or our free list doesn't have space
	void* mem = mmap(0, pages * PAGE_SIZE, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
	stats.pages_mapped += pages;
	*((size_t*)mem) = size - sizeof(size_t);
	if(pages * PAGE_SIZE - size >= sizeof(Node))
	{
		Node* node = mem + size;
		node->size = pages * PAGE_SIZE - size;
		push_free_list(node);
	}
	pthread_mutex_unlock(&mutex);
	return (void*)(mem + sizeof(size_t));
}

void hfree(void* item)
{
	pthread_once(&init_flag, init_mutex);
	pthread_mutex_lock(&mutex);

	stats.chunks_freed += 1;

	void* base = item - sizeof(size_t);
	size_t size = *((long*)base) + sizeof(size_t);
	if(size >= PAGE_SIZE)
	{
		size_t pages = div_up(size, PAGE_SIZE);
		Node* previous = 0;
		Node* current = free_list_head;
		while(current)
		{
			// since this is sorted, we could probably flatten to a single removal
			if((void*)current >= base && (void*)current <= base + pages * PAGE_SIZE)
			{
				if(previous)
					previous->next = current->next;
				else
					free_list_head = current->next;
			}
			previous = current;
			current = current->next;
		}
		munmap(base, pages * PAGE_SIZE);
		stats.pages_unmapped += pages;
	}
	else
	{
		Node* node = (Node*)base;
		node->size = size;
		push_free_list(node);
	}

	coalesce();
	pthread_mutex_unlock(&mutex);
}

void* hrealloc(void* item, size_t size)
{
	pthread_once(&init_flag, init_mutex);
	pthread_mutex_lock(&mutex);

	size += sizeof(size_t);
	void* base = item - sizeof(size_t);
	size_t old_size = *((long*)base) + sizeof(size_t);

	if(old_size >= size)
	{
		stats.chunks_freed += 1;
		stats.chunks_allocated += 1;
		*((long*)base) = (long)(size - sizeof(size_t));
		if(old_size - size > sizeof(Node))
		{
			Node* node = base + size;
			node->size = old_size - size;
			node->next = 0;
			push_free_list(node);
			coalesce();
		}
		pthread_mutex_unlock(&mutex);
		return item;
	}
	else
	{
		pthread_mutex_unlock(&mutex);
		void* new_item = hmalloc(size - sizeof(size_t));
		pthread_mutex_lock(&mutex);

		memcpy(new_item, item, old_size - sizeof(size_t));

		pthread_mutex_unlock(&mutex);
		hfree(item);

		return new_item;
	}
}