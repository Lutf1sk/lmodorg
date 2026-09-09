#pragma once

#include <lt/lt.h>
#include <lt/mem.h>

#define arr_t(T) T*

typedef struct arr_header {
	usz count;
	u64 pad;
} arr_header_t;

#define ARR_BLOCKSIZE 128
#define ARR_BLOCKMASK (ARR_BLOCKSIZE-1)

static LT_INLINE
arr_header_t* arr_header(void* arr) {
	return (arr_header_t*)arr - 1;
}

static LT_INLINE
void* arr_body(arr_header_t* head) {
	return head + 1;
}

static LT_INLINE
usz arr_count(void* arr) {
	return arr_header(arr)->count;
}

static LT_INLINE
void* arr_alloc(usz elem_size) {
	arr_header_t* head = malloc(sizeof(arr_header_t) + ARR_BLOCKSIZE * elem_size);
	*head = (arr_header_t){ 0 };
	return arr_body(head);
}

#define arr_alloc(T) \
	((T*)arr_alloc(sizeof(T)))

static LT_INLINE
void arr_free(void* arr) {
	free(arr_header(arr));
}

static
void* arr_push(void* arr, usz elem_size) {
	arr_header_t* head = arr_header(arr);
	if LT_UNLIKELY (!((head->count + 1) & ARR_BLOCKMASK)) {
		head = realloc(head, sizeof(arr_header_t) + (head->count + 1 + ARR_BLOCKSIZE) * elem_size);
		if (!head)
			return NULL;
	}
	++head->count;
	return arr_body(head);
}

#define arr_push(arr, ...) \
	(((arr)[arr_count((arr))] = (__VA_ARGS__)), (typeof(arr))arr_push((arr), sizeof(*(arr))))

static LT_INLINE
void* arr_pop(void* arr, usz elem_size) {
	return (u8*)arr + --arr_header(arr)->count * elem_size;
}

#define arr_pop(arr) \
	((typeof(arr))arr_pop((arr), sizeof(*(arr))))

static LT_INLINE
void arr_erase(void* arr, usz index, usz elem_size) {
	usz offset = index * elem_size;
	memmove((u8*)arr + offset, (u8*)arr + offset + elem_size, (--arr_header(arr)->count - index) * elem_size);
}

#define arr_erase(arr, index) \
	arr_erase((arr), (index), sizeof(*(arr)))

static
void* arr_insert(void* arr, usz index, usz elem_size, const void* data) {
	arr_header_t* head = arr_header(arr);
	if LT_UNLIKELY (!((head->count + 1) & ARR_BLOCKMASK)) {
		head = realloc(head, sizeof(arr_header_t) + (head->count + 1 + ARR_BLOCKSIZE) * elem_size);
		if (!head)
			return NULL;
		arr = arr_body(head);
	}
	usz offset = index * elem_size;
	memmove((u8*)arr + offset + elem_size, (u8*)arr + offset, (head->count - index) * elem_size);
	memcpy((u8*)arr + offset, data, elem_size);
	++head->count;
	return arr;
}

#define arr_insert(arr, index, ...) \
	((typeof(arr))arr_insert((arr), (index), sizeof(*(arr)), (typeof(*(arr))[]){ __VA_ARGS__ }))

static
void* arr_push_zeroed(void* arr, usz count, usz elem_size) {
	arr_header_t* head = arr_header(arr);
	usz new_count = head->count + count;
	if LT_UNLIKELY ((head->count ^ (new_count + 1)) & ~ARR_BLOCKMASK) {
		head = realloc(head, sizeof(arr_header_t) + lt_align_fwd(new_count + 1, ARR_BLOCKSIZE) * elem_size);
		if (!head)
			return NULL;
		arr = arr_body(head);
	}
	memset((u8*)arr + head->count * elem_size, 0, count * elem_size);
	head->count = new_count;
	return arr;
}

#define arr_push_zeroed(arr, count) \
	((typeof(arr))arr_push_zeroed((arr), (count), sizeof(*(arr))))

static LT_INLINE
void arr_clear(void* arr) {
	arr_header(arr)->count = 0;
}

#define arr_foreach(arr, it_name) \
	for (typeof(arr) it_name = (arr), end__ = arr_count(it); it_name < end__; ++it_name)

