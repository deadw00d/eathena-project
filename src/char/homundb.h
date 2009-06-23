// Copyright (c) Athena Dev Teams - Licensed under GNU GPL
// For more information, see LICENCE in the main folder

#ifndef _HOMUNDB_H_
#define _HOMUNDB_H_

#include "../common/mmo.h" // struct s_homunculus

typedef struct HomunDB HomunDB;
typedef struct HomunDBIterator HomunDBIterator;


struct HomunDBIterator
{
	/// Destroys this iterator, releasing all allocated memory (including itself).
	///
	/// @param self Iterator
	void (*destroy)(HomunDBIterator* self);

	/// Fetches the next homun data and stores it in 'data'.
	/// @param self Iterator
	/// @param data a homun's data
	/// @return true if successful
	bool (*next)(HomunDBIterator* self, struct s_homunculus* data);
};


struct HomunDB
{
	bool (*init)(HomunDB* self);

	void (*destroy)(HomunDB* self);

	bool (*sync)(HomunDB* self);

	bool (*create)(HomunDB* self, struct s_homunculus* p);

	bool (*remove)(HomunDB* self, int homun_id);

	bool (*save)(HomunDB* self, const struct s_homunculus* p);

	bool (*load)(HomunDB* self, struct s_homunculus* p, int homun_id);

	/// Returns an iterator over all homunculi.
	///
	/// @param self Database
	/// @return Iterator
	HomunDBIterator* (*iterator)(HomunDB* self);
};


#endif /* _HOMUNDB_H_ */
