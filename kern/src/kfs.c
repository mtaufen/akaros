/*
 * Copyright (c) 2009 The Regents of the University of California
 * Barret Rhoden <brho@cs.berkeley.edu>
 * See LICENSE for details.
 */

#include <kfs.h>
#include <string.h>
#include <assert.h>
#include <ros/error.h>

#define DECL_PROG(x) extern uint8_t _binary_obj_user_apps_##x##_start[], _binary_obj_user_apps_##x##_size[];      

#define KFS_ENTRY(x) {#x, _binary_obj_user_apps_##x##_start, (size_t) _binary_obj_user_apps_##x##_size},

/*
 * Hardcode the files included in the KFS.  This needs to be in sync with the
 * userapps in kern/src/Makefrag.
 * Make sure to declare it, and add an entry.  Keep MAX_KFS_FILES big enough too
 */
DECL_PROG(roslib_proctests);
DECL_PROG(roslib_fptest);
DECL_PROG(roslib_null);
DECL_PROG(roslib_spawn);
DECL_PROG(roslib_hello);

#ifdef __i386__
DECL_PROG(parlib_channel_test_client);
DECL_PROG(parlib_channel_test_server);
DECL_PROG(roslib_measurements);
#endif

struct kfs_entry kfs[MAX_KFS_FILES] = {
	KFS_ENTRY(roslib_proctests)
	KFS_ENTRY(roslib_fptest)
	KFS_ENTRY(roslib_null)
	KFS_ENTRY(roslib_spawn)
	KFS_ENTRY(roslib_hello)
	#ifdef __i386__
	KFS_ENTRY(parlib_channel_test_client)
	KFS_ENTRY(parlib_channel_test_server)
	KFS_ENTRY(roslib_measurements)
	#endif
};

ssize_t kfs_lookup_path(char* path)
{
	for (int i = 0; i < MAX_KFS_FILES; i++)
		// need to think about how to copy-in something of unknown length
		if (!strncmp(kfs[i].name, path, strlen(path)))
			return i;
	return -EINVAL;
}

/*
 * Creates a process from the file pointed to by the KFS inode (index)
 * This should take a real inode or something to point to the real location,
 * and env_create shouldn't assume everything is contiguous
 */
struct proc *kfs_proc_create(size_t kfs_inode)
{
	if (kfs_inode < 0 || kfs_inode >= MAX_KFS_FILES)
		panic("Invalid kfs_inode.  Check you error codes!");
	return env_create(kfs[kfs_inode].start, kfs[kfs_inode].size);
}
