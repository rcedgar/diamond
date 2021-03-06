/****
DIAMOND protein aligner
Copyright (C) 2013-2017 Benjamin Buchfink <buchfink@gmail.com>

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>.
****/

#pragma once
#include <exception>
#include <stdint.h>
#include <memory>
#include <algorithm>
#include <string.h>
#include "hash_function.h"
#include "simd.h"

struct hash_table_overflow_exception : public std::exception
{
	virtual const char* what() const throw()
	{
		return "Hash table overflow";
	}
};

template<typename T, T value> struct value_compare
{
bool operator()(T x) const
{
	return x == value;
}
};

template<typename _V> class PHash_table
{

public:

	struct entry
	{
		uint16_t finger_print;
		_V value;
	}; // __attribute__((packed));

	PHash_table() :
		size_(0)
	{}

	PHash_table(size_t size) :
		table(new entry[size]),
		size_(size)
	{
		memset(table.get(), 0, size_ * sizeof(entry));
	}

	PHash_table(size_t size, double factor) :
		size_(std::max(size_t((double)size * factor), (size_t)1llu)),
		table(new entry[size_])
	{
		memset(table.get(), 0, size_ * sizeof(entry));
	}
	
	entry* operator[](uint64_t hash) const
	{
		entry *entry = get_entry(hash);
		if (entry->value == 0u)
			return NULL;
		return entry;
	}

	entry* insert(uint64_t hash)
	{
		entry *entry = get_entry(hash);
		if (entry->value == 0u)
			entry->finger_print = finger_print(hash);
		return entry;
	}

	size_t size() const
	{
		return size_;
	}

	size_t count() const
	{
		size_t n(0);
		for (size_t i = 0; i<size_; ++i)
			if (table[i].value != 0u)
				++n;
		return n;
	}

private:

	static uint16_t finger_print(uint64_t hash)
	{
		return (uint16_t)(hash & 0xffffllu);
	}

	entry* get_entry(uint64_t hash) const
	{
		entry *p = table.get() + ((hash >> 16) % size_);
		const uint16_t fp = finger_print(hash);
		bool wrapped = false;
		while (p->finger_print != fp && p->value != 0u) {
			++p;
			if (p == table.get() + size_) {
				if (wrapped)
					throw hash_table_overflow_exception();
				p = table.get();
				wrapped = true;
			}
		}
		return p;
	}

	size_t size_;
	std::unique_ptr<entry[]> table;	

};

struct Modulo2 {};

template<typename _t>
inline uint64_t modulo(uint64_t x, uint64_t y)
{
	return x % y;
}

template<>
inline uint64_t modulo<Modulo2>(uint64_t x, uint64_t y)
{
	return x & (y - 1);
}

struct No_hash
{
	uint64_t operator()(uint64_t x) const
	{
		return x;
	}
};

template<typename _mod, typename _hash>
struct PHash_set
{

public:

	typedef uint8_t fp;

	PHash_set() :
		size_(0)
	{}

	PHash_set(size_t size) :
		table(new fp[size]),
		size_(size)
	{
		memset(table, 0, size_ * sizeof(fp));
	}

	PHash_set(fp* data, size_t size) :
		table(data),
		size_(size)
	{}

	~PHash_set() {
		delete[] table;
	}

	bool contains(uint64_t key) const
	{
#ifdef USE_AVX
		const uint64_t hash = _hash()(key);
		fp* p = table.get() + modulo<_mod>(hash >> (sizeof(fp) * 8), size_);
		__m256i r = _mm256_loadu_si256((const __m256i*)p);
		__m256i z = _mm256_setzero_si256();
		const int zm = _mm256_movemask_epi8(_mm256_cmpeq_epi8(r, z));
		if (zm == 0)
			return true;

		const fp f = finger_print(hash);
		__m256i fr = _mm256_set1_epi8(f);
		const int fm = _mm256_movemask_epi8(_mm256_cmpeq_epi8(r, fr));
		return fm != 0;
#elif defined(__SSE2__)
		const uint64_t hash = _hash()(key);
		fp* p = table + modulo<_mod>(hash >> (sizeof(fp) * 8), size_);
		__m128i r = _mm_loadu_si128((const __m128i*)p);
		__m128i z = _mm_setzero_si128();
		const int zm = _mm_movemask_epi8(_mm_cmpeq_epi8(r, z));
		if (zm == 0)
			return true;

		const fp f = finger_print(hash);
		__m128i fr = _mm_set1_epi8(f);
		const int fm = _mm_movemask_epi8(_mm_cmpeq_epi8(r, fr));
		return fm != 0;
#else
		fp *p;
		return get_entry(key, p);
#endif
	}

	void insert(uint64_t key)
	{
		fp *entry;
		get_entry(key, entry);
		if (*entry == (fp)0)
			*entry = finger_print(_hash()(key));
	}

	size_t size() const
	{
		return size_;
	}

	size_t load() const
	{
		size_t n = 0;
		for (size_t i = 0; i < size_; ++i)
			if (*(table + i) != 0)
				++n;
		return n;
	}

	const fp* data() const {
		return table;
	}

	fp* table;
	size_t size_;

private:

	static fp finger_print(uint64_t hash)
	{
		const fp x = (fp)(hash & ((1llu << (sizeof(fp) * 8)) - 1llu));
		return std::max(x, (fp)1);
	}

	bool get_entry(uint64_t key, fp*& p) const
	{
		const uint64_t hash = _hash()(key);
		const fp f = finger_print(hash);
		p = table + modulo<_mod>(hash >> (sizeof(fp) * 8), size_);
		bool wrapped = false;
		while (true) {
			if (*p == f)
				return true;
			if (*p == (fp)0)
				return false;
			++p;
			if (p == table + size_) {
				if (wrapped)
					throw hash_table_overflow_exception();
				p = table;
				wrapped = true;
			}
		}
	}

};
