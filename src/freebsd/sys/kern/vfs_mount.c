/*-
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Copyright (c) 1999-2004 Poul-Henning Kamp
 * Copyright (c) 1999 Michael Smith
 * Copyright (c) 1989, 1993
 *	The Regents of the University of California.  All rights reserved.
 * (c) UNIX System Laboratories, Inc.
 * All or some portions of this file are derived from material licensed
 * to the University of California by American Telephone and Telegraph
 * Co. or Unix System Laboratories, Inc. and are reproduced herein with
 * the permission of UNIX System Laboratories, Inc.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <sys/cdefs.h>
__FBSDID("$FreeBSD$");

#include <sys/param.h>
#include <sys/conf.h>
#include <sys/smp.h>
#include <sys/devctl.h>
#include <sys/eventhandler.h>
#include <sys/fcntl.h>
#include <sys/jail.h>
#include <sys/kernel.h>
#include <sys/ktr.h>
#include <sys/libkern.h>
#include <sys/malloc.h>
#include <sys/mount.h>
#include <sys/mutex.h>
#include <sys/namei.h>
#include <sys/priv.h>
#include <sys/proc.h>
#include <sys/filedesc.h>
#include <sys/reboot.h>
#include <sys/sbuf.h>
#include <sys/syscallsubr.h>
#include <sys/sysproto.h>
#include <sys/sx.h>
#include <sys/sysctl.h>
#include <sys/sysent.h>
#include <sys/systm.h>
#include <sys/vnode.h>
#include <vm/uma.h>


#include <machine/stdarg.h>

#include <security/audit/audit.h>
#include <security/mac/mac_framework.h>

#define	VFS_MOUNTARG_SIZE_MAX	(1024 * 64)

static int	vfs_domount(struct thread *td, const char *fstype, char *fspath,
		    uint64_t fsflags, struct vfsoptlist **optlist);
static void	free_mntarg(struct mntarg *ma);

static int	usermount = 0;
SYSCTL_INT(_vfs, OID_AUTO, usermount, CTLFLAG_RW, &usermount, 0,
    "Unprivileged users may mount and unmount file systems");

static bool	default_autoro = false;
SYSCTL_BOOL(_vfs, OID_AUTO, default_autoro, CTLFLAG_RW, &default_autoro, 0,
    "Retry failed r/w mount as r/o if no explicit ro/rw option is specified");

MALLOC_DEFINE(M_MOUNT, "mount", "vfs mount structure");
MALLOC_DEFINE(M_STATFS, "statfs", "statfs structure");
static uma_zone_t mount_zone;

/* List of mounted filesystems. */
struct mntlist mountlist = TAILQ_HEAD_INITIALIZER(mountlist);

/* For any iteration/modification of mountlist */
struct __begin_exclusive_cache_line mtx_padalign __exclusive_cache_line mountlist_mtx;
MTX_SYSINIT(mountlist, &mountlist_mtx, "mountlist", MTX_DEF);

EVENTHANDLER_LIST_DEFINE(vfs_mounted);
EVENTHANDLER_LIST_DEFINE(vfs_unmounted);

static void mount_devctl_event(const char *type, struct mount *mp, bool donew);

/*
 * Global opts, taken by all filesystems
 */
static const char *global_opts[] = {
	"errmsg",
	"fstype",
	"fspath",
	"ro",
	"rw",
	"nosuid",
	"noexec",
	NULL
};

static int
mount_init(void *mem, int size, int flags)
{
	struct mount *mp;

	mp = (struct mount *)mem;
	mtx_init(&mp->mnt_mtx, "struct mount mtx", NULL, MTX_DEF);
	mtx_init(&mp->mnt_listmtx, "struct mount vlist mtx", NULL, MTX_DEF);
	lockinit(&mp->mnt_explock, PVFS, "explock", 0, 0);
	mp->mnt_pcpu = uma_zalloc_pcpu(pcpu_zone_16, M_WAITOK | M_ZERO);
	mp->mnt_ref = 0;
	mp->mnt_vfs_ops = 1;
	mp->mnt_rootvnode = NULL;
	return (0);
}

static void
mount_fini(void *mem, int size)
{
	struct mount *mp;

	mp = (struct mount *)mem;
	uma_zfree_pcpu(pcpu_zone_16, mp->mnt_pcpu);
	lockdestroy(&mp->mnt_explock);
	mtx_destroy(&mp->mnt_listmtx);
	mtx_destroy(&mp->mnt_mtx);
}

static void
vfs_mount_init(void *dummy __unused)
{

	mount_zone = uma_zcreate("Mountpoints", sizeof(struct mount), NULL,
	    NULL, mount_init, mount_fini, UMA_ALIGN_CACHE, UMA_ZONE_NOFREE);
}
SYSINIT(vfs_mount, SI_SUB_VFS, SI_ORDER_ANY, vfs_mount_init, NULL);

/*
 * ---------------------------------------------------------------------
 * Functions for building and sanitizing the mount options
 */

/* Remove one mount option. */
static void
vfs_freeopt(struct vfsoptlist *opts, struct vfsopt *opt)
{

	TAILQ_REMOVE(opts, opt, link);
	vos_free(opt->name, M_MOUNT);
	if (opt->value != NULL)
		vos_free(opt->value, M_MOUNT);
	vos_free(opt, M_MOUNT);
}

/* Release all resources related to the mount options. */
void
vfs_freeopts(struct vfsoptlist *opts)
{
	struct vfsopt *opt;

	while (!TAILQ_EMPTY(opts)) {
		opt = TAILQ_FIRST(opts);
		vfs_freeopt(opts, opt);
	}
	vos_free(opts, M_MOUNT);
}

void
vfs_deleteopt(struct vfsoptlist *opts, const char *name)
{
	struct vfsopt *opt, *temp;

	if (opts == NULL)
		return;
	TAILQ_FOREACH_SAFE(opt, opts, link, temp)  {
		if (strcmp(opt->name, name) == 0)
			vfs_freeopt(opts, opt);
	}
}

static int
vfs_isopt_ro(const char *opt)
{

	if (strcmp(opt, "ro") == 0 || strcmp(opt, "rdonly") == 0 ||
	    strcmp(opt, "norw") == 0)
		return (1);
	return (0);
}

static int
vfs_isopt_rw(const char *opt)
{

	if (strcmp(opt, "rw") == 0 || strcmp(opt, "noro") == 0)
		return (1);
	return (0);
}

/*
 * Check if options are equal (with or without the "no" prefix).
 */
static int
vfs_equalopts(const char *opt1, const char *opt2)
{
	char *p;

	/* "opt" vs. "opt" or "noopt" vs. "noopt" */
	if (strcmp(opt1, opt2) == 0)
		return (1);
	/* "noopt" vs. "opt" */
	if (vos_strncmp(opt1, "no", 2) == 0 && strcmp(opt1 + 2, opt2) == 0)
		return (1);
	/* "opt" vs. "noopt" */
	if (vos_strncmp(opt2, "no", 2) == 0 && strcmp(opt1, opt2 + 2) == 0)
		return (1);
	while ((p = strchr(opt1, '.')) != NULL &&
	    !vos_strncmp(opt1, opt2, ++p - opt1)) {
		opt2 += p - opt1;
		opt1 = p;
		/* "foo.noopt" vs. "foo.opt" */
		if (vos_strncmp(opt1, "no", 2) == 0 && strcmp(opt1 + 2, opt2) == 0)
			return (1);
		/* "foo.opt" vs. "foo.noopt" */
		if (vos_strncmp(opt2, "no", 2) == 0 && strcmp(opt1, opt2 + 2) == 0)
			return (1);
	}
	/* "ro" / "rdonly" / "norw" / "rw" / "noro" */
	if ((vfs_isopt_ro(opt1) || vfs_isopt_rw(opt1)) &&
	    (vfs_isopt_ro(opt2) || vfs_isopt_rw(opt2)))
		return (1);
	return (0);
}

/*
 * If a mount option is specified several times,
 * (with or without the "no" prefix) only keep
 * the last occurrence of it.
 */
static void
vfs_sanitizeopts(struct vfsoptlist *opts)
{
	struct vfsopt *opt, *opt2, *tmp;

	TAILQ_FOREACH_REVERSE(opt, opts, vfsoptlist, link) {
		opt2 = TAILQ_PREV(opt, vfsoptlist, link);
		while (opt2 != NULL) {
			if (vfs_equalopts(opt->name, opt2->name)) {
				tmp = TAILQ_PREV(opt2, vfsoptlist, link);
				vfs_freeopt(opts, opt2);
				opt2 = tmp;
			} else {
				opt2 = TAILQ_PREV(opt2, vfsoptlist, link);
			}
		}
	}
}

/*
 * Build a linked list of mount options from a struct uio.
 */
int
vfs_buildopts(struct uio *auio, struct vfsoptlist **options)
{
	struct vfsoptlist *opts;
	struct vfsopt *opt;
	size_t memused, namelen, optlen;
	unsigned int i, iovcnt;
	int error;

	opts = vos_malloc(sizeof(struct vfsoptlist), M_MOUNT, M_WAITOK);
	TAILQ_INIT(opts);
	memused = 0;
	iovcnt = auio->uio_iovcnt;
	for (i = 0; i < iovcnt; i += 2) {
		namelen = auio->uio_iov[i].iov_len;
		optlen = auio->uio_iov[i + 1].iov_len;
		memused += sizeof(struct vfsopt) + optlen + namelen;
		/*
		 * Avoid consuming too much memory, and attempts to overflow
		 * memused.
		 */
		if (memused > VFS_MOUNTARG_SIZE_MAX ||
		    optlen > VFS_MOUNTARG_SIZE_MAX ||
		    namelen > VFS_MOUNTARG_SIZE_MAX) {
			error = VOS_EINVAL;
			goto bad;
		}

		opt = vos_malloc(sizeof(struct vfsopt), M_MOUNT, M_WAITOK);
		opt->name = vos_malloc(namelen, M_MOUNT, M_WAITOK);
		opt->value = NULL;
		opt->len = 0;
		opt->pos = i / 2;
		opt->seen = 0;

		/*
		 * Do this early, so jumps to "bad" will free the current
		 * option.
		 */
		TAILQ_INSERT_TAIL(opts, opt, link);

		if (auio->uio_segflg == UIO_SYSSPACE) {
			bcopy(auio->uio_iov[i].iov_base, opt->name, namelen);
		} else {
			error = copyin(auio->uio_iov[i].iov_base, opt->name,
			    namelen);
			if (error)
				goto bad;
		}
		/* Ensure names are null-terminated strings. */
		if (namelen == 0 || opt->name[namelen - 1] != '\0') {
			error = VOS_EINVAL;
			goto bad;
		}
		if (optlen != 0) {
			opt->len = optlen;
			opt->value = vos_malloc(optlen, M_MOUNT, M_WAITOK);
			if (auio->uio_segflg == UIO_SYSSPACE) {
				bcopy(auio->uio_iov[i + 1].iov_base, opt->value,
				    optlen);
			} else {
				error = copyin(auio->uio_iov[i + 1].iov_base,
				    opt->value, optlen);
				if (error)
					goto bad;
			}
		}
	}
	vfs_sanitizeopts(opts);
	*options = opts;
	return (0);
bad:
	vfs_freeopts(opts);
	return (error);
}

/*
 * Merge the old mount options with the new ones passed
 * in the MNT_UPDATE case.
 *
 * XXX: This function will keep a "nofoo" option in the new
 * options.  E.g, if the option's canonical name is "foo",
 * "nofoo" ends up in the mount point's active options.
 */
static void
vfs_mergeopts(struct vfsoptlist *toopts, struct vfsoptlist *oldopts)
{
	struct vfsopt *opt, *new;

	TAILQ_FOREACH(opt, oldopts, link) {
		new = vos_malloc(sizeof(struct vfsopt), M_MOUNT, M_WAITOK);
		new->name = vos_strdup(opt->name, M_MOUNT);
		if (opt->len != 0) {
			new->value = vos_malloc(opt->len, M_MOUNT, M_WAITOK);
			bcopy(opt->value, new->value, opt->len);
		} else
			new->value = NULL;
		new->len = opt->len;
		new->seen = opt->seen;
		TAILQ_INSERT_HEAD(toopts, new, link);
	}
	vfs_sanitizeopts(toopts);
}

/*
 * Mount a filesystem.
 */
#ifndef _SYS_SYSPROTO_H_
struct nmount_args {
	struct iovec *iovp;
	unsigned int iovcnt;
	int flags;
};
#endif
int
sys_nmount(struct thread *td, struct nmount_args *uap)
{
	struct uio *auio;
	int error;
	u_int iovcnt;
	uint64_t flags;

	/*
	 * Mount flags are now 64-bits. On 32-bit archtectures only
	 * 32-bits are passed in, but from here on everything handles
	 * 64-bit flags correctly.
	 */
	flags = uap->flags;

	AUDIT_ARG_FFLAGS(flags);
	CTR4(KTR_VFS, "%s: iovp %p with iovcnt %d and flags %d", __func__,
	    uap->iovp, uap->iovcnt, flags);

	/*
	 * Filter out MNT_ROOTFS.  We do not want clients of nmount() in
	 * userspace to set this flag, but we must filter it out if we want
	 * MNT_UPDATE on the root file system to work.
	 * MNT_ROOTFS should only be set by the kernel when mounting its
	 * root file system.
	 */
	flags &= ~MNT_ROOTFS;

	iovcnt = uap->iovcnt;
	/*
	 * Check that we have an even number of iovec's
	 * and that we have at least two options.
	 */
	if ((iovcnt & 1) || (iovcnt < 4)) {
		CTR2(KTR_VFS, "%s: failed for invalid iovcnt %d", __func__,
		    uap->iovcnt);
		return (VOS_EINVAL);
	}

	error = copyinuio(uap->iovp, iovcnt, &auio);
	if (error) {
		CTR2(KTR_VFS, "%s: failed for invalid uio op with %d errno",
		    __func__, error);
		return (error);
	}
	error = vfs_donmount(td, flags, auio);

	vos_free(auio, M_IOV);
	return (error);
}

/*
 * ---------------------------------------------------------------------
 * Various utility functions
 */

void
vfs_ref(struct mount *mp)
{
	struct mount_pcpu *mpcpu;

	CTR2(KTR_VFS, "%s: mp %p", __func__, mp);
	if (vfs_op_thread_enter(mp, mpcpu)) {
		vfs_mp_count_add_pcpu(mpcpu, ref, 1);
		vfs_op_thread_exit(mp, mpcpu);
		return;
	}

	MNT_ILOCK(mp);
	MNT_REF(mp);
	MNT_IUNLOCK(mp);
}

void
vfs_rel(struct mount *mp)
{
	struct mount_pcpu *mpcpu;

	CTR2(KTR_VFS, "%s: mp %p", __func__, mp);
	if (vfs_op_thread_enter(mp, mpcpu)) {
		vfs_mp_count_sub_pcpu(mpcpu, ref, 1);
		vfs_op_thread_exit(mp, mpcpu);
		return;
	}

	MNT_ILOCK(mp);
	MNT_REL(mp);
	MNT_IUNLOCK(mp);
}

/*
 * Allocate and initialize the mount point struct.
 */
struct mount *
vfs_mount_alloc(struct vnode *vp, struct vfsconf *vfsp, const char *fspath,
    struct ucred *cred)
{
	struct mount *mp;

	mp = uma_zalloc(mount_zone, M_WAITOK);
	bzero(&mp->mnt_startzero,
	    __rangeof(struct mount, mnt_startzero, mnt_endzero));
	mp->mnt_kern_flag = 0;
	mp->mnt_flag = 0;
	mp->mnt_rootvnode = NULL;
	mp->mnt_vnodecovered = NULL;
	mp->mnt_op = NULL;
	mp->mnt_vfc = NULL;
	TAILQ_INIT(&mp->mnt_nvnodelist);
	mp->mnt_nvnodelistsize = 0;
	TAILQ_INIT(&mp->mnt_lazyvnodelist);
	mp->mnt_lazyvnodelistsize = 0;
	if (mp->mnt_ref != 0 || mp->mnt_lockref != 0 ||
	    mp->mnt_writeopcount != 0)
		panic("%s: non-zero counters on new mp %p\n", __func__, mp);
	if (mp->mnt_vfs_ops != 1)
		panic("%s: vfs_ops should be 1 but %d found\n", __func__,
		    mp->mnt_vfs_ops);
	(void) vfs_busy(mp, MBF_NOWAIT);
	atomic_add_acq_int(&vfsp->vfc_refcount, 1);
	mp->mnt_op = vfsp->vfc_vfsops;
	mp->mnt_vfc = vfsp;
	mp->mnt_stat.f_type = vfsp->vfc_typenum;
	mp->mnt_gen++;
	vos_strlcpy(mp->mnt_stat.f_fstypename, vfsp->vfc_name, MFSNAMELEN);
	mp->mnt_vnodecovered = vp;
	mp->mnt_cred = crdup(cred);
	mp->mnt_stat.f_owner = cred->cr_uid;
	vos_strlcpy(mp->mnt_stat.f_mntonname, fspath, MNAMELEN);
	mp->mnt_iosize_max = DFLTPHYS;
#ifdef MAC
	mac_mount_init(mp);
	mac_mount_create(cred, mp);
#endif
	arc4rand(&mp->mnt_hashseed, sizeof mp->mnt_hashseed, 0);
	TAILQ_INIT(&mp->mnt_uppers);
	return (mp);
}

/*
 * Destroy the mount struct previously allocated by vfs_mount_alloc().
 */
void
vfs_mount_destroy(struct mount *mp)
{

	if (mp->mnt_vfs_ops == 0)
		panic("%s: entered with zero vfs_ops\n", __func__);

	vfs_assert_mount_counters(mp);

	MNT_ILOCK(mp);
	mp->mnt_kern_flag |= MNTK_REFEXPIRE;
	if (mp->mnt_kern_flag & MNTK_MWAIT) {
		mp->mnt_kern_flag &= ~MNTK_MWAIT;
		wakeup(mp);
	}
	while (mp->mnt_ref)
		msleep(mp, MNT_MTX(mp), PVFS, "mntref", 0);
	KASSERT(mp->mnt_ref == 0,
	    ("%s: invalid refcount in the drain path @ %s:%d", __func__,
	    __FILE__, __LINE__));
	if (mp->mnt_writeopcount != 0)
		panic("vfs_mount_destroy: nonzero writeopcount");
	if (mp->mnt_secondary_writes != 0)
		panic("vfs_mount_destroy: nonzero secondary_writes");
	atomic_subtract_rel_int(&mp->mnt_vfc->vfc_refcount, 1);
	if (!TAILQ_EMPTY(&mp->mnt_nvnodelist)) {
		struct vnode *vp;

		TAILQ_FOREACH(vp, &mp->mnt_nvnodelist, v_nmntvnodes)
			vn_printf(vp, "dangling vnode ");
		panic("unmount: dangling vnode");
	}
	KASSERT(TAILQ_EMPTY(&mp->mnt_uppers), ("mnt_uppers"));
	if (mp->mnt_nvnodelistsize != 0)
		panic("vfs_mount_destroy: nonzero nvnodelistsize");
	if (mp->mnt_lazyvnodelistsize != 0)
		panic("vfs_mount_destroy: nonzero lazyvnodelistsize");
	if (mp->mnt_lockref != 0)
		panic("vfs_mount_destroy: nonzero lock refcount");
	MNT_IUNLOCK(mp);

	if (mp->mnt_vfs_ops != 1)
		panic("%s: vfs_ops should be 1 but %d found\n", __func__,
		    mp->mnt_vfs_ops);

	if (mp->mnt_rootvnode != NULL)
		panic("%s: mount point still has a root vnode %p\n", __func__,
		    mp->mnt_rootvnode);

	if (mp->mnt_vnodecovered != NULL)
		vrele(mp->mnt_vnodecovered);
#ifdef MAC
	mac_mount_destroy(mp);
#endif
	if (mp->mnt_opt != NULL)
		vfs_freeopts(mp->mnt_opt);
	crfree(mp->mnt_cred);
	uma_zfree(mount_zone, mp);
}

static bool
vfs_should_downgrade_to_ro_mount(uint64_t fsflags, int error)
{
	/* This is an upgrade of an exisiting mount. */
	if ((fsflags & MNT_UPDATE) != 0)
		return (false);
	/* This is already an R/O mount. */
	if ((fsflags & MNT_RDONLY) != 0)
		return (false);

	switch (error) {
	case VOS_ENODEV:	/* generic, geom, ... */
	case VOS_EACCES:	/* cam/scsi, ... */
	case VOS_EROFS:	/* md, mmcsd, ... */
		/*
		 * These errors can be returned by the storage layer to signal
		 * that the media is read-only.  No harm in the R/O mount
		 * attempt if the error was returned for some other reason.
		 */
		return (true);
	default:
		return (false);
	}
}

int
vfs_donmount(struct thread *td, uint64_t fsflags, struct uio *fsoptions)
{
	struct vfsoptlist *optlist;
	struct vfsopt *opt, *tmp_opt;
	char *fstype, *fspath, *errmsg;
	int error, fstypelen, fspathlen, errmsg_len, errmsg_pos;
	bool autoro;

	errmsg = fspath = NULL;
	errmsg_len = fspathlen = 0;
	errmsg_pos = -1;
	autoro = default_autoro;

	error = vfs_buildopts(fsoptions, &optlist);
	if (error)
		return (error);

	if (vfs_getopt(optlist, "errmsg", (void **)&errmsg, &errmsg_len) == 0)
		errmsg_pos = vfs_getopt_pos(optlist, "errmsg");

	/*
	 * We need these two options before the others,
	 * and they are mandatory for any filesystem.
	 * Ensure they are NUL terminated as well.
	 */
	fstypelen = 0;
	error = vfs_getopt(optlist, "fstype", (void **)&fstype, &fstypelen);
	if (error || fstypelen <= 0 || fstype[fstypelen - 1] != '\0') {
		error = VOS_EINVAL;
		if (errmsg != NULL)
			vos_strncpy(errmsg, "Invalid fstype", errmsg_len);
		goto bail;
	}
	fspathlen = 0;
	error = vfs_getopt(optlist, "fspath", (void **)&fspath, &fspathlen);
	if (error || fspathlen <= 0 || fspath[fspathlen - 1] != '\0') {
		error = VOS_EINVAL;
		if (errmsg != NULL)
			vos_strncpy(errmsg, "Invalid fspath", errmsg_len);
		goto bail;
	}

	/*
	 * We need to see if we have the "update" option
	 * before we call vfs_domount(), since vfs_domount() has special
	 * logic based on MNT_UPDATE.  This is very important
	 * when we want to update the root filesystem.
	 */
	TAILQ_FOREACH_SAFE(opt, optlist, link, tmp_opt) {
		int do_freeopt = 0;

		if (strcmp(opt->name, "update") == 0) {
			fsflags |= MNT_UPDATE;
			do_freeopt = 1;
		}
		else if (strcmp(opt->name, "async") == 0)
			fsflags |= MNT_ASYNC;
		else if (strcmp(opt->name, "force") == 0) {
			fsflags |= MNT_FORCE;
			do_freeopt = 1;
		}
		else if (strcmp(opt->name, "reload") == 0) {
			fsflags |= MNT_RELOAD;
			do_freeopt = 1;
		}
		else if (strcmp(opt->name, "multilabel") == 0)
			fsflags |= MNT_MULTILABEL;
		else if (strcmp(opt->name, "noasync") == 0)
			fsflags &= ~MNT_ASYNC;
		else if (strcmp(opt->name, "noatime") == 0)
			fsflags |= MNT_NOATIME;
		else if (strcmp(opt->name, "atime") == 0) {
			vos_free(opt->name, M_MOUNT);
			opt->name = vos_strdup("nonoatime", M_MOUNT);
		}
		else if (strcmp(opt->name, "noclusterr") == 0)
			fsflags |= MNT_NOCLUSTERR;
		else if (strcmp(opt->name, "clusterr") == 0) {
			vos_free(opt->name, M_MOUNT);
			opt->name = vos_strdup("nonoclusterr", M_MOUNT);
		}
		else if (strcmp(opt->name, "noclusterw") == 0)
			fsflags |= MNT_NOCLUSTERW;
		else if (strcmp(opt->name, "clusterw") == 0) {
			vos_free(opt->name, M_MOUNT);
			opt->name = vos_strdup("nonoclusterw", M_MOUNT);
		}
		else if (strcmp(opt->name, "noexec") == 0)
			fsflags |= MNT_NOEXEC;
		else if (strcmp(opt->name, "exec") == 0) {
			vos_free(opt->name, M_MOUNT);
			opt->name = vos_strdup("nonoexec", M_MOUNT);
		}
		else if (strcmp(opt->name, "nosuid") == 0)
			fsflags |= MNT_NOSUID;
		else if (strcmp(opt->name, "suid") == 0) {
			vos_free(opt->name, M_MOUNT);
			opt->name = vos_strdup("nonosuid", M_MOUNT);
		}
		else if (strcmp(opt->name, "nosymfollow") == 0)
			fsflags |= MNT_NOSYMFOLLOW;
		else if (strcmp(opt->name, "symfollow") == 0) {
			vos_free(opt->name, M_MOUNT);
			opt->name = vos_strdup("nonosymfollow", M_MOUNT);
		}
		else if (strcmp(opt->name, "noro") == 0) {
			fsflags &= ~MNT_RDONLY;
			autoro = false;
		}
		else if (strcmp(opt->name, "rw") == 0) {
			fsflags &= ~MNT_RDONLY;
			autoro = false;
		}
		else if (strcmp(opt->name, "ro") == 0) {
			fsflags |= MNT_RDONLY;
			autoro = false;
		}
		else if (strcmp(opt->name, "rdonly") == 0) {
			vos_free(opt->name, M_MOUNT);
			opt->name = vos_strdup("ro", M_MOUNT);
			fsflags |= MNT_RDONLY;
			autoro = false;
		}
		else if (strcmp(opt->name, "autoro") == 0) {
			do_freeopt = 1;
			autoro = true;
		}
		else if (strcmp(opt->name, "suiddir") == 0)
			fsflags |= MNT_SUIDDIR;
		else if (strcmp(opt->name, "sync") == 0)
			fsflags |= MNT_SYNCHRONOUS;
		else if (strcmp(opt->name, "union") == 0)
			fsflags |= MNT_UNION;
		else if (strcmp(opt->name, "automounted") == 0) {
			fsflags |= MNT_AUTOMOUNTED;
			do_freeopt = 1;
		} else if (strcmp(opt->name, "nocover") == 0) {
			fsflags |= MNT_NOCOVER;
			do_freeopt = 1;
		} else if (strcmp(opt->name, "cover") == 0) {
			fsflags &= ~MNT_NOCOVER;
			do_freeopt = 1;
		} else if (strcmp(opt->name, "emptydir") == 0) {
			fsflags |= MNT_EMPTYDIR;
			do_freeopt = 1;
		} else if (strcmp(opt->name, "noemptydir") == 0) {
			fsflags &= ~MNT_EMPTYDIR;
			do_freeopt = 1;
		}
		if (do_freeopt)
			vfs_freeopt(optlist, opt);
	}

	/*
	 * Be ultra-paranoid about making sure the type and fspath
	 * variables will fit in our mp buffers, including the
	 * terminating NUL.
	 */
	if (fstypelen > MFSNAMELEN || fspathlen > MNAMELEN) {
		error = VOS_ENAMETOOLONG;
		goto bail;
	}

	error = vfs_domount(td, fstype, fspath, fsflags, &optlist);

	/*
	 * See if we can mount in the read-only mode if the error code suggests
	 * that it could be possible and the mount options allow for that.
	 * Never try it if "[no]{ro|rw}" has been explicitly requested and not
	 * overridden by "autoro".
	 */
	if (autoro && vfs_should_downgrade_to_ro_mount(fsflags, error)) {
		printf("%s: R/W mount failed, possibly R/O media,"
		    " trying R/O mount\n", __func__);
		fsflags |= MNT_RDONLY;
		error = vfs_domount(td, fstype, fspath, fsflags, &optlist);
	}
bail:
	/* copyout the errmsg */
	if (errmsg_pos != -1 && ((2 * errmsg_pos + 1) < fsoptions->uio_iovcnt)
	    && errmsg_len > 0 && errmsg != NULL) {
		if (fsoptions->uio_segflg == UIO_SYSSPACE) {
			bcopy(errmsg,
			    fsoptions->uio_iov[2 * errmsg_pos + 1].iov_base,
			    fsoptions->uio_iov[2 * errmsg_pos + 1].iov_len);
		} else {
			copyout(errmsg,
			    fsoptions->uio_iov[2 * errmsg_pos + 1].iov_base,
			    fsoptions->uio_iov[2 * errmsg_pos + 1].iov_len);
		}
	}

	if (optlist != NULL)
		vfs_freeopts(optlist);
	return (error);
}

/*
 * Old mount API.
 */
#ifndef _SYS_SYSPROTO_H_
struct mount_args {
	char	*type;
	char	*path;
	int	flags;
	caddr_t	data;
};
#endif
/* ARGSUSED */
int
sys_mount(struct thread *td, struct mount_args *uap)
{
	char *fstype;
	struct vfsconf *vfsp = NULL;
	struct mntarg *ma = NULL;
	uint64_t flags;
	int error;

	/*
	 * Mount flags are now 64-bits. On 32-bit architectures only
	 * 32-bits are passed in, but from here on everything handles
	 * 64-bit flags correctly.
	 */
	flags = uap->flags;

	AUDIT_ARG_FFLAGS(flags);

	/*
	 * Filter out MNT_ROOTFS.  We do not want clients of mount() in
	 * userspace to set this flag, but we must filter it out if we want
	 * MNT_UPDATE on the root file system to work.
	 * MNT_ROOTFS should only be set by the kernel when mounting its
	 * root file system.
	 */
	flags &= ~MNT_ROOTFS;

	fstype = vos_malloc(MFSNAMELEN, M_TEMP, M_WAITOK);
	error = copyinstr(uap->type, fstype, MFSNAMELEN, NULL);
	if (error) {
		vos_free(fstype, M_TEMP);
		return (error);
	}

	AUDIT_ARG_TEXT(fstype);
	vfsp = vfs_byname_kld(fstype, td, &error);
	vos_free(fstype, M_TEMP);
	if (vfsp == NULL)
		return (VOS_ENOENT);
	if (((vfsp->vfc_flags & VFCF_SBDRY) != 0 &&
	    vfsp->vfc_vfsops_sd->vfs_cmount == NULL) ||
	    ((vfsp->vfc_flags & VFCF_SBDRY) == 0 &&
	    vfsp->vfc_vfsops->vfs_cmount == NULL))
		return (VOS_EOPNOTSUPP);

	ma = mount_argsu(ma, "fstype", uap->type, MFSNAMELEN);
	ma = mount_argsu(ma, "fspath", uap->path, MNAMELEN);
	ma = mount_argb(ma, flags & MNT_RDONLY, "noro");
	ma = mount_argb(ma, !(flags & MNT_NOSUID), "nosuid");
	ma = mount_argb(ma, !(flags & MNT_NOEXEC), "noexec");

	if ((vfsp->vfc_flags & VFCF_SBDRY) != 0)
		return (vfsp->vfc_vfsops_sd->vfs_cmount(ma, uap->data, flags));
	return (vfsp->vfc_vfsops->vfs_cmount(ma, uap->data, flags));
}

/*
 * vfs_domount_first(): first file system mount (not update)
 */
static int
vfs_domount_first(
	struct thread *td,		/* Calling thread. */
	struct vfsconf *vfsp,		/* File system type. */
	char *fspath,			/* Mount path. */
	struct vnode *vp,		/* Vnode to be covered. */
	uint64_t fsflags,		/* Flags common to all filesystems. */
	struct vfsoptlist **optlist	/* Options local to the filesystem. */
	)
{
	return (0);
}

/*
 * vfs_domount_update(): update of mounted file system
 */
static int
vfs_domount_update(
	struct thread *td,		/* Calling thread. */
	struct vnode *vp,		/* Mount point vnode. */
	uint64_t fsflags,		/* Flags common to all filesystems. */
	struct vfsoptlist **optlist	/* Options local to the filesystem. */
	)
{
	return -1;
}

/*
 * vfs_domount(): actually attempt a filesystem mount.
 */
static int
vfs_domount(
	struct thread *td,		/* Calling thread. */
	const char *fstype,		/* Filesystem type. */
	char *fspath,			/* Mount path. */
	uint64_t fsflags,		/* Flags common to all filesystems. */
	struct vfsoptlist **optlist	/* Options local to the filesystem. */
	)
{
	struct vfsconf *vfsp;
	struct nameidata nd;
	struct vnode *vp;
	char *pathbuf;
	int error;

	/*
	 * Be ultra-paranoid about making sure the type and fspath
	 * variables will fit in our mp buffers, including the
	 * terminating NUL.
	 */
	if (vos_strlen(fstype) >= MFSNAMELEN || vos_strlen(fspath) >= MNAMELEN)
		return (VOS_ENAMETOOLONG);

	if (jailed(td->td_ucred) || usermount == 0) {
		if ((error = priv_check(td, PRIV_VFS_MOUNT)) != 0)
			return (error);
	}

	/*
	 * Do not allow NFS export or MNT_SUIDDIR by unprivileged users.
	 */
	if (fsflags & MNT_EXPORTED) {
		error = priv_check(td, PRIV_VFS_MOUNT_EXPORTED);
		if (error)
			return (error);
	}
	if (fsflags & MNT_SUIDDIR) {
		error = priv_check(td, PRIV_VFS_MOUNT_SUIDDIR);
		if (error)
			return (error);
	}
	/*
	 * Silently enforce MNT_NOSUID and MNT_USER for unprivileged users.
	 */
	if ((fsflags & (MNT_NOSUID | MNT_USER)) != (MNT_NOSUID | MNT_USER)) {
		if (priv_check(td, PRIV_VFS_MOUNT_NONUSER) != 0)
			fsflags |= MNT_NOSUID | MNT_USER;
	}

	/* Load KLDs before we lock the covered vnode to avoid reversals. */
	vfsp = NULL;
	if ((fsflags & MNT_UPDATE) == 0) {
		/* Don't try to load KLDs if we're mounting the root. */
		if (fsflags & MNT_ROOTFS)
			vfsp = vfs_byname(fstype);
		else
			vfsp = vfs_byname_kld(fstype, td, &error);
		if (vfsp == NULL)
			return (VOS_ENODEV);
	}

	/*
	 * Get vnode to be covered or mount point's vnode in case of MNT_UPDATE.
	 */
	NDINIT(&nd, LOOKUP, FOLLOW | LOCKLEAF | AUDITVNODE1,
	    UIO_SYSSPACE, fspath, td);
	error = namei(&nd);
	if (error != 0)
		return (error);
	NDFREE(&nd, NDF_ONLY_PNBUF);
	vp = nd.ni_vp;
	if ((fsflags & MNT_UPDATE) == 0) {
		if ((vp->v_vflag & VV_ROOT) != 0 &&
		    (fsflags & MNT_NOCOVER) != 0) {
			vput(vp);
			return (VOS_EBUSY);
		}
		pathbuf = vos_malloc(MNAMELEN, M_TEMP, M_WAITOK);
		vos_strcpy(pathbuf, fspath);
		error = vn_path_to_global_path(td, vp, pathbuf, MNAMELEN);
		if (error == 0) {
			error = vfs_domount_first(td, vfsp, pathbuf, vp,
			    fsflags, optlist);
		}
		vos_free(pathbuf, M_TEMP);
	} else
		error = vfs_domount_update(td, vp, fsflags, optlist);

	return (error);
}

/*
 * Unmount a filesystem.
 *
 * Note: unmount takes a path to the vnode mounted on as argument, not
 * special file (as before).
 */
#ifndef _SYS_SYSPROTO_H_
struct unmount_args {
	char	*path;
	int	flags;
};
#endif
/* ARGSUSED */
int
sys_unmount(struct thread *td, struct unmount_args *uap)
{

	return (kern_unmount(td, uap->path, uap->flags));
}

int
kern_unmount(struct thread *td, const char *path, int flags)
{
	struct nameidata nd;
	struct mount *mp;
	char *pathbuf;
	int error, id0, id1;

	AUDIT_ARG_VALUE(flags);
	if (jailed(td->td_ucred) || usermount == 0) {
		error = priv_check(td, PRIV_VFS_UNMOUNT);
		if (error)
			return (error);
	}

	pathbuf = vos_malloc(MNAMELEN, M_TEMP, M_WAITOK);
	error = copyinstr(path, pathbuf, MNAMELEN, NULL);
	if (error) {
		vos_free(pathbuf, M_TEMP);
		return (error);
	}
	if (flags & MNT_BYFSID) {
		AUDIT_ARG_TEXT(pathbuf);
		/* Decode the filesystem ID. */
		if (sscanf(pathbuf, "FSID:%d:%d", &id0, &id1) != 2) {
			vos_free(pathbuf, M_TEMP);
			return (VOS_EINVAL);
		}

		mtx_lock(&mountlist_mtx);
		TAILQ_FOREACH_REVERSE(mp, &mountlist, mntlist, mnt_list) {
			if (mp->mnt_stat.f_fsid.val[0] == id0 &&
			    mp->mnt_stat.f_fsid.val[1] == id1) {
				vfs_ref(mp);
				break;
			}
		}
		mtx_unlock(&mountlist_mtx);
	} else {
		/*
		 * Try to find global path for path argument.
		 */
		NDINIT(&nd, LOOKUP, FOLLOW | LOCKLEAF | AUDITVNODE1,
		    UIO_SYSSPACE, pathbuf, td);
		if (namei(&nd) == 0) {
			NDFREE(&nd, NDF_ONLY_PNBUF);
			error = vn_path_to_global_path(td, nd.ni_vp, pathbuf,
			    MNAMELEN);
			if (error == 0)
				vput(nd.ni_vp);
		}
		mtx_lock(&mountlist_mtx);
		TAILQ_FOREACH_REVERSE(mp, &mountlist, mntlist, mnt_list) {
			if (strcmp(mp->mnt_stat.f_mntonname, pathbuf) == 0) {
				vfs_ref(mp);
				break;
			}
		}
		mtx_unlock(&mountlist_mtx);
	}
	vos_free(pathbuf, M_TEMP);
	if (mp == NULL) {
		/*
		 * Previously we returned VOS_ENOENT for a nonexistent path and
		 * VOS_EINVAL for a non-mountpoint.  We cannot tell these apart
		 * now, so in the !MNT_BYFSID case return the more likely
		 * VOS_EINVAL for compatibility.
		 */
		return ((flags & MNT_BYFSID) ? VOS_ENOENT : VOS_EINVAL);
	}

	/*
	 * Don't allow unmounting the root filesystem.
	 */
	if (mp->mnt_flag & MNT_ROOTFS) {
		vfs_rel(mp);
		return (VOS_EINVAL);
	}
	error = dounmount(mp, flags, td);
	return (error);
}

/*
 * Return error if any of the vnodes, ignoring the root vnode
 * and the syncer vnode, have non-zero usecount.
 *
 * This function is purely advisory - it can return false positives
 * and negatives.
 */
static int
vfs_check_usecounts(struct mount *mp)
{
	struct vnode *vp, *mvp;

	MNT_VNODE_FOREACH_ALL(vp, mp, mvp) {
		if ((vp->v_vflag & VV_ROOT) == 0 && vp->v_type != VNON &&
		    vp->v_usecount != 0) {
			VI_UNLOCK(vp);
			MNT_VNODE_FOREACH_ALL_ABORT(mp, mvp);
			return (VOS_EBUSY);
		}
		VI_UNLOCK(vp);
	}

	return (0);
}

static void
dounmount_cleanup(struct mount *mp, struct vnode *coveredvp, int mntkflags)
{

	mtx_assert(MNT_MTX(mp), MA_OWNED);
	mp->mnt_kern_flag &= ~mntkflags;
	if ((mp->mnt_kern_flag & MNTK_MWAIT) != 0) {
		mp->mnt_kern_flag &= ~MNTK_MWAIT;
		wakeup(mp);
	}
	vfs_op_exit_locked(mp);
	MNT_IUNLOCK(mp);
	if (coveredvp != NULL) {
		VOP_UNLOCK(coveredvp);
		vdrop(coveredvp);
	}
	vn_finished_write(mp);
}

/*
 * There are various reference counters associated with the mount point.
 * Normally it is permitted to modify them without taking the mnt ilock,
 * but this behavior can be temporarily disabled if stable value is needed
 * or callers are expected to block (e.g. to not allow new users during
 * forced unmount).
 */
void
vfs_op_enter(struct mount *mp)
{
	struct mount_pcpu *mpcpu;
	int cpu;

	MNT_ILOCK(mp);
	mp->mnt_vfs_ops++;
	if (mp->mnt_vfs_ops > 1) {
		MNT_IUNLOCK(mp);
		return;
	}
	vfs_op_barrier_wait(mp);
	CPU_FOREACH(cpu) {
		mpcpu = vfs_mount_pcpu_remote(mp, cpu);

		mp->mnt_ref += mpcpu->mntp_ref;
		mpcpu->mntp_ref = 0;

		mp->mnt_lockref += mpcpu->mntp_lockref;
		mpcpu->mntp_lockref = 0;

		mp->mnt_writeopcount += mpcpu->mntp_writeopcount;
		mpcpu->mntp_writeopcount = 0;
	}
	if (mp->mnt_ref <= 0 || mp->mnt_lockref < 0 || mp->mnt_writeopcount < 0)
		panic("%s: invalid count(s) on mp %p: ref %d lockref %d writeopcount %d\n",
		    __func__, mp, mp->mnt_ref, mp->mnt_lockref, mp->mnt_writeopcount);
	MNT_IUNLOCK(mp);
	vfs_assert_mount_counters(mp);
}

void
vfs_op_exit_locked(struct mount *mp)
{

	mtx_assert(MNT_MTX(mp), MA_OWNED);

	if (mp->mnt_vfs_ops <= 0)
		panic("%s: invalid vfs_ops count %d for mp %p\n",
		    __func__, mp->mnt_vfs_ops, mp);
	mp->mnt_vfs_ops--;
}

void
vfs_op_exit(struct mount *mp)
{

	MNT_ILOCK(mp);
	vfs_op_exit_locked(mp);
	MNT_IUNLOCK(mp);
}

struct vfs_op_barrier_ipi {
	struct mount *mp;
	struct smp_rendezvous_cpus_retry_arg srcra;
};

static void
vfs_op_action_func(void *arg)
{
}

static void
vfs_op_wait_func(void *arg, int cpu)
{
	struct vfs_op_barrier_ipi *vfsopipi;
	struct mount *mp;
	struct mount_pcpu *mpcpu;

	vfsopipi = __containerof(arg, struct vfs_op_barrier_ipi, srcra);
	mp = vfsopipi->mp;

	mpcpu = vfs_mount_pcpu_remote(mp, cpu);
	while (atomic_load_int(&mpcpu->mntp_thread_in_ops))
		cpu_spinwait();
}

void
vfs_op_barrier_wait(struct mount *mp)
{
	struct vfs_op_barrier_ipi vfsopipi;

	vfsopipi.mp = mp;

	smp_rendezvous_cpus_retry(all_cpus,
	    smp_no_rendezvous_barrier,
	    vfs_op_action_func,
	    smp_no_rendezvous_barrier,
	    vfs_op_wait_func,
	    &vfsopipi.srcra);
}

#ifdef DIAGNOSTIC
void
vfs_assert_mount_counters(struct mount *mp)
{
	struct mount_pcpu *mpcpu;
	int cpu;

	if (mp->mnt_vfs_ops == 0)
		return;

	CPU_FOREACH(cpu) {
		mpcpu = vfs_mount_pcpu_remote(mp, cpu);
		if (mpcpu->mntp_ref != 0 ||
		    mpcpu->mntp_lockref != 0 ||
		    mpcpu->mntp_writeopcount != 0)
			vfs_dump_mount_counters(mp);
	}
}

void
vfs_dump_mount_counters(struct mount *mp)
{
	struct mount_pcpu *mpcpu;
	int ref, lockref, writeopcount;
	int cpu;

	printf("%s: mp %p vfs_ops %d\n", __func__, mp, mp->mnt_vfs_ops);

	printf("        ref : ");
	ref = mp->mnt_ref;
	CPU_FOREACH(cpu) {
		mpcpu = vfs_mount_pcpu_remote(mp, cpu);
		printf("%d ", mpcpu->mntp_ref);
		ref += mpcpu->mntp_ref;
	}
	printf("\n");
	printf("    lockref : ");
	lockref = mp->mnt_lockref;
	CPU_FOREACH(cpu) {
		mpcpu = vfs_mount_pcpu_remote(mp, cpu);
		printf("%d ", mpcpu->mntp_lockref);
		lockref += mpcpu->mntp_lockref;
	}
	printf("\n");
	printf("writeopcount: ");
	writeopcount = mp->mnt_writeopcount;
	CPU_FOREACH(cpu) {
		mpcpu = vfs_mount_pcpu_remote(mp, cpu);
		printf("%d ", mpcpu->mntp_writeopcount);
		writeopcount += mpcpu->mntp_writeopcount;
	}
	printf("\n");

	printf("counter       struct total\n");
	printf("ref             %-5d  %-5d\n", mp->mnt_ref, ref);
	printf("lockref         %-5d  %-5d\n", mp->mnt_lockref, lockref);
	printf("writeopcount    %-5d  %-5d\n", mp->mnt_writeopcount, writeopcount);

	panic("invalid counts on struct mount");
}
#endif

int
vfs_mount_fetch_counter(struct mount *mp, enum mount_counter which)
{
	struct mount_pcpu *mpcpu;
	int cpu, sum;

	switch (which) {
	case MNT_COUNT_REF:
		sum = mp->mnt_ref;
		break;
	case MNT_COUNT_LOCKREF:
		sum = mp->mnt_lockref;
		break;
	case MNT_COUNT_WRITEOPCOUNT:
		sum = mp->mnt_writeopcount;
		break;
	}

	CPU_FOREACH(cpu) {
		mpcpu = vfs_mount_pcpu_remote(mp, cpu);
		switch (which) {
		case MNT_COUNT_REF:
			sum += mpcpu->mntp_ref;
			break;
		case MNT_COUNT_LOCKREF:
			sum += mpcpu->mntp_lockref;
			break;
		case MNT_COUNT_WRITEOPCOUNT:
			sum += mpcpu->mntp_writeopcount;
			break;
		}
	}
	return (sum);
}

/*
 * Do the actual filesystem unmount.
 */
int
dounmount(struct mount *mp, int flags, struct thread *td)
{
	return (0);
}

/*
 * Report errors during filesystem mounting.
 */
void
vfs_mount_error(struct mount *mp, const char *fmt, ...)
{
	struct vfsoptlist *moptlist = mp->mnt_optnew;
	va_list ap;
	int error, len;
	char *errmsg;

	error = vfs_getopt(moptlist, "errmsg", (void **)&errmsg, &len);
	if (error || errmsg == NULL || len <= 0)
		return;

	va_start(ap, fmt);
	vsnprintf(errmsg, (size_t)len, fmt, ap);
	va_end(ap);
}

void
vfs_opterror(struct vfsoptlist *opts, const char *fmt, ...)
{
	va_list ap;
	int error, len;
	char *errmsg;

	error = vfs_getopt(opts, "errmsg", (void **)&errmsg, &len);
	if (error || errmsg == NULL || len <= 0)
		return;

	va_start(ap, fmt);
	vsnprintf(errmsg, (size_t)len, fmt, ap);
	va_end(ap);
}

/*
 * ---------------------------------------------------------------------
 * Functions for querying mount options/arguments from filesystems.
 */

/*
 * Check that no unknown options are given
 */
int
vfs_filteropt(struct vfsoptlist *opts, const char **legal)
{
	struct vfsopt *opt;
	char errmsg[255];
	const char **t, *p, *q;
	int ret = 0;

	TAILQ_FOREACH(opt, opts, link) {
		p = opt->name;
		q = NULL;
		if (p[0] == 'n' && p[1] == 'o')
			q = p + 2;
		for(t = global_opts; *t != NULL; t++) {
			if (strcmp(*t, p) == 0)
				break;
			if (q != NULL) {
				if (strcmp(*t, q) == 0)
					break;
			}
		}
		if (*t != NULL)
			continue;
		for(t = legal; *t != NULL; t++) {
			if (strcmp(*t, p) == 0)
				break;
			if (q != NULL) {
				if (strcmp(*t, q) == 0)
					break;
			}
		}
		if (*t != NULL)
			continue;
		snprintf(errmsg, sizeof(errmsg),
		    "mount option <%s> is unknown", p);
		ret = VOS_EINVAL;
	}
	if (ret != 0) {
		TAILQ_FOREACH(opt, opts, link) {
			if (strcmp(opt->name, "errmsg") == 0) {
				vos_strncpy((char *)opt->value, errmsg, opt->len);
				break;
			}
		}
		if (opt == NULL)
			printf("%s\n", errmsg);
	}
	return (ret);
}

/*
 * Get a mount option by its name.
 *
 * Return 0 if the option was found, VOS_ENOENT otherwise.
 * If len is non-NULL it will be filled with the length
 * of the option. If buf is non-NULL, it will be filled
 * with the address of the option.
 */
int
vfs_getopt(struct vfsoptlist *opts, const char *name, void **buf, int *len)
{
	struct vfsopt *opt;

	KASSERT(opts != NULL, ("vfs_getopt: caller passed 'opts' as NULL"));

	TAILQ_FOREACH(opt, opts, link) {
		if (strcmp(name, opt->name) == 0) {
			opt->seen = 1;
			if (len != NULL)
				*len = opt->len;
			if (buf != NULL)
				*buf = opt->value;
			return (0);
		}
	}
	return (VOS_ENOENT);
}

int
vfs_getopt_pos(struct vfsoptlist *opts, const char *name)
{
	struct vfsopt *opt;

	if (opts == NULL)
		return (-1);

	TAILQ_FOREACH(opt, opts, link) {
		if (strcmp(name, opt->name) == 0) {
			opt->seen = 1;
			return (opt->pos);
		}
	}
	return (-1);
}

int
vfs_getopt_size(struct vfsoptlist *opts, const char *name, off_t *value)
{
	char *opt_value, *vtp;
	quad_t iv;
	int error, opt_len;

	error = vfs_getopt(opts, name, (void **)&opt_value, &opt_len);
	if (error != 0)
		return (error);
	if (opt_len == 0 || opt_value == NULL)
		return (VOS_EINVAL);
	if (opt_value[0] == '\0' || opt_value[opt_len - 1] != '\0')
		return (VOS_EINVAL);
	iv = strtoq(opt_value, &vtp, 0);
	if (vtp == opt_value || (vtp[0] != '\0' && vtp[1] != '\0'))
		return (VOS_EINVAL);
	if (iv < 0)
		return (VOS_EINVAL);
	switch (vtp[0]) {
	case 't': case 'T':
		iv *= 1024;
		/* FALLTHROUGH */
	case 'g': case 'G':
		iv *= 1024;
		/* FALLTHROUGH */
	case 'm': case 'M':
		iv *= 1024;
		/* FALLTHROUGH */
	case 'k': case 'K':
		iv *= 1024;
	case '\0':
		break;
	default:
		return (VOS_EINVAL);
	}
	*value = iv;

	return (0);
}

char *
vfs_getopts(struct vfsoptlist *opts, const char *name, int *error)
{
	struct vfsopt *opt;

	*error = 0;
	TAILQ_FOREACH(opt, opts, link) {
		if (strcmp(name, opt->name) != 0)
			continue;
		opt->seen = 1;
		if (opt->len == 0 ||
		    ((char *)opt->value)[opt->len - 1] != '\0') {
			*error = VOS_EINVAL;
			return (NULL);
		}
		return (opt->value);
	}
	*error = VOS_ENOENT;
	return (NULL);
}

int
vfs_flagopt(struct vfsoptlist *opts, const char *name, uint64_t *w,
	uint64_t val)
{
	struct vfsopt *opt;

	TAILQ_FOREACH(opt, opts, link) {
		if (strcmp(name, opt->name) == 0) {
			opt->seen = 1;
			if (w != NULL)
				*w |= val;
			return (1);
		}
	}
	if (w != NULL)
		*w &= ~val;
	return (0);
}

int
vfs_scanopt(struct vfsoptlist *opts, const char *name, const char *fmt, ...)
{
	va_list ap;
	struct vfsopt *opt;
	int ret;

	KASSERT(opts != NULL, ("vfs_getopt: caller passed 'opts' as NULL"));

	TAILQ_FOREACH(opt, opts, link) {
		if (strcmp(name, opt->name) != 0)
			continue;
		opt->seen = 1;
		if (opt->len == 0 || opt->value == NULL)
			return (0);
		if (((char *)opt->value)[opt->len - 1] != '\0')
			return (0);
		va_start(ap, fmt);
		ret = vsscanf(opt->value, fmt, ap);
		va_end(ap);
		return (ret);
	}
	return (0);
}

int
vfs_setopt(struct vfsoptlist *opts, const char *name, void *value, int len)
{
	struct vfsopt *opt;

	TAILQ_FOREACH(opt, opts, link) {
		if (strcmp(name, opt->name) != 0)
			continue;
		opt->seen = 1;
		if (opt->value == NULL)
			opt->len = len;
		else {
			if (opt->len != len)
				return (VOS_EINVAL);
			bcopy(value, opt->value, len);
		}
		return (0);
	}
	return (VOS_ENOENT);
}

int
vfs_setopt_part(struct vfsoptlist *opts, const char *name, void *value, int len)
{
	struct vfsopt *opt;

	TAILQ_FOREACH(opt, opts, link) {
		if (strcmp(name, opt->name) != 0)
			continue;
		opt->seen = 1;
		if (opt->value == NULL)
			opt->len = len;
		else {
			if (opt->len < len)
				return (VOS_EINVAL);
			opt->len = len;
			bcopy(value, opt->value, len);
		}
		return (0);
	}
	return (VOS_ENOENT);
}

int
vfs_setopts(struct vfsoptlist *opts, const char *name, const char *value)
{
	struct vfsopt *opt;

	TAILQ_FOREACH(opt, opts, link) {
		if (strcmp(name, opt->name) != 0)
			continue;
		opt->seen = 1;
		if (opt->value == NULL)
			opt->len = vos_strlen(value) + 1;
		else if (vos_strlcpy(opt->value, value, opt->len) >= opt->len)
			return (VOS_EINVAL);
		return (0);
	}
	return (VOS_ENOENT);
}

/*
 * Find and copy a mount option.
 *
 * The size of the buffer has to be specified
 * in len, if it is not the same length as the
 * mount option, VOS_EINVAL is returned.
 * Returns VOS_ENOENT if the option is not found.
 */
int
vfs_copyopt(struct vfsoptlist *opts, const char *name, void *dest, int len)
{
	struct vfsopt *opt;

	KASSERT(opts != NULL, ("vfs_copyopt: caller passed 'opts' as NULL"));

	TAILQ_FOREACH(opt, opts, link) {
		if (strcmp(name, opt->name) == 0) {
			opt->seen = 1;
			if (len != opt->len)
				return (VOS_EINVAL);
			bcopy(opt->value, dest, opt->len);
			return (0);
		}
	}
	return (VOS_ENOENT);
}

int
__vfs_statfs(struct mount *mp, struct statfs *sbp)
{

	/*
	 * Filesystems only fill in part of the structure for updates, we
	 * have to read the entirety first to get all content.
	 */
	if (sbp != &mp->mnt_stat)
		memcpy(sbp, &mp->mnt_stat, sizeof(*sbp));

	/*
	 * Set these in case the underlying filesystem fails to do so.
	 */
	sbp->f_version = STATFS_VERSION;
	sbp->f_namemax = NAME_MAX;
	sbp->f_flags = mp->mnt_flag & MNT_VISFLAGMASK;

	return (mp->mnt_op->vfs_statfs(mp, sbp));
}

void
vfs_mountedfrom(struct mount *mp, const char *from)
{

	bzero(mp->mnt_stat.f_mntfromname, sizeof mp->mnt_stat.f_mntfromname);
	vos_strlcpy(mp->mnt_stat.f_mntfromname, from,
	    sizeof mp->mnt_stat.f_mntfromname);
}

/*
 * ---------------------------------------------------------------------
 * This is the api for building mount args and mounting filesystems from
 * inside the kernel.
 *
 * The API works by accumulation of individual args.  First error is
 * latched.
 *
 * XXX: should be documented in new manpage kernel_mount(9)
 */

/* A memory allocation which must be freed when we are done */
struct mntaarg {
	SLIST_ENTRY(mntaarg)	next;
};

/* The header for the mount arguments */
struct mntarg {
	struct iovec *v;
	int len;
	int error;
	SLIST_HEAD(, mntaarg)	list;
};

/*
 * Add a boolean argument.
 *
 * flag is the boolean value.
 * name must start with "no".
 */
struct mntarg *
mount_argb(struct mntarg *ma, int flag, const char *name)
{

	KASSERT(name[0] == 'n' && name[1] == 'o',
	    ("mount_argb(...,%s): name must start with 'no'", name));

	return (mount_arg(ma, name + (flag ? 2 : 0), NULL, 0));
}

/*
 * Add an argument printf style
 */
struct mntarg *
mount_argf(struct mntarg *ma, const char *name, const char *fmt, ...)
{
	va_list ap;
	struct mntaarg *maa;
	struct sbuf *sb;
	int len;

	if (ma == NULL) {
		ma = vos_malloc(sizeof *ma, M_MOUNT, M_WAITOK | M_ZERO);
		SLIST_INIT(&ma->list);
	}
	if (ma->error)
		return (ma);

	ma->v = vos_realloc(ma->v, sizeof *ma->v * (ma->len + 2),
	    M_MOUNT, M_WAITOK);
	ma->v[ma->len].iov_base = (void *)(uintptr_t)name;
	ma->v[ma->len].iov_len = vos_strlen(name) + 1;
	ma->len++;

	sb = sbuf_new_auto();
	va_start(ap, fmt);
	sbuf_vprintf(sb, fmt, ap);
	va_end(ap);
	sbuf_finish(sb);
	len = sbuf_len(sb) + 1;
	maa = vos_malloc(sizeof *maa + len, M_MOUNT, M_WAITOK | M_ZERO);
	SLIST_INSERT_HEAD(&ma->list, maa, next);
	bcopy(sbuf_data(sb), maa + 1, len);
	sbuf_delete(sb);

	ma->v[ma->len].iov_base = maa + 1;
	ma->v[ma->len].iov_len = len;
	ma->len++;

	return (ma);
}

/*
 * Add an argument which is a userland string.
 */
struct mntarg *
mount_argsu(struct mntarg *ma, const char *name, const void *val, int len)
{
	struct mntaarg *maa;
	char *tbuf;

	if (val == NULL)
		return (ma);
	if (ma == NULL) {
		ma = vos_malloc(sizeof *ma, M_MOUNT, M_WAITOK | M_ZERO);
		SLIST_INIT(&ma->list);
	}
	if (ma->error)
		return (ma);
	maa = vos_malloc(sizeof *maa + len, M_MOUNT, M_WAITOK | M_ZERO);
	SLIST_INSERT_HEAD(&ma->list, maa, next);
	tbuf = (void *)(maa + 1);
	ma->error = copyinstr(val, tbuf, len, NULL);
	return (mount_arg(ma, name, tbuf, -1));
}

/*
 * Plain argument.
 *
 * If length is -1, treat value as a C string.
 */
struct mntarg *
mount_arg(struct mntarg *ma, const char *name, const void *val, int len)
{

	if (ma == NULL) {
		ma = vos_malloc(sizeof *ma, M_MOUNT, M_WAITOK | M_ZERO);
		SLIST_INIT(&ma->list);
	}
	if (ma->error)
		return (ma);

	ma->v = vos_realloc(ma->v, sizeof *ma->v * (ma->len + 2),
	    M_MOUNT, M_WAITOK);
	ma->v[ma->len].iov_base = (void *)(uintptr_t)name;
	ma->v[ma->len].iov_len = vos_strlen(name) + 1;
	ma->len++;

	ma->v[ma->len].iov_base = (void *)(uintptr_t)val;
	if (len < 0)
		ma->v[ma->len].iov_len = vos_strlen(val) + 1;
	else
		ma->v[ma->len].iov_len = len;
	ma->len++;
	return (ma);
}

/*
 * Free a mntarg structure
 */
static void
free_mntarg(struct mntarg *ma)
{
	struct mntaarg *maa;

	while (!SLIST_EMPTY(&ma->list)) {
		maa = SLIST_FIRST(&ma->list);
		SLIST_REMOVE_HEAD(&ma->list, next);
		vos_free(maa, M_MOUNT);
	}
	vos_free(ma->v, M_MOUNT);
	vos_free(ma, M_MOUNT);
}

/*
 * Mount a filesystem
 */
int
kernel_mount(struct mntarg *ma, uint64_t flags)
{
	struct uio auio;
	int error;

	KASSERT(ma != NULL, ("kernel_mount NULL ma"));
	KASSERT(ma->v != NULL, ("kernel_mount NULL ma->v"));
	KASSERT(!(ma->len & 1), ("kernel_mount odd ma->len (%d)", ma->len));

	auio.uio_iov = ma->v;
	auio.uio_iovcnt = ma->len;
	auio.uio_segflg = UIO_SYSSPACE;

	error = ma->error;
	if (!error)
		error = vfs_donmount(curthread, flags, &auio);
	free_mntarg(ma);
	return (error);
}

/*
 * A printflike function to mount a filesystem.
 */
int
kernel_vmount(int flags, ...)
{
	struct mntarg *ma = NULL;
	va_list ap;
	const char *cp;
	const void *vp;
	int error;

	va_start(ap, flags);
	for (;;) {
		cp = va_arg(ap, const char *);
		if (cp == NULL)
			break;
		vp = va_arg(ap, const void *);
		ma = mount_arg(ma, cp, vp, (vp != NULL ? -1 : 0));
	}
	va_end(ap);

	error = kernel_mount(ma, flags);
	return (error);
}

/* Map from mount options to printable formats. */
static struct mntoptnames optnames[] = {
	MNTOPT_NAMES
};

static void
mount_devctl_event_mntopt(struct sbuf *sb, const char *what, struct vfsoptlist *opts)
{
	struct vfsopt *opt;

	if (opts == NULL || TAILQ_EMPTY(opts))
		return;
	sbuf_printf(sb, " %s=\"", what);
	TAILQ_FOREACH(opt, opts, link) {
		if (opt->name[0] == '\0' || (opt->len > 0 && *(char *)opt->value == '\0'))
			continue;
		devctl_safe_quote_sb(sb, opt->name);
		if (opt->len > 0) {
			sbuf_putc(sb, '=');
			devctl_safe_quote_sb(sb, opt->value);
		}
		sbuf_putc(sb, ';');
	}
	sbuf_putc(sb, '"');
}

#define DEVCTL_LEN 1024
static void
mount_devctl_event(const char *type, struct mount *mp, bool donew)
{
	const uint8_t *cp;
	struct mntoptnames *fp;
	struct sbuf sb;
	struct statfs *sfp = &mp->mnt_stat;
	char *buf;

	buf = vos_malloc(DEVCTL_LEN, M_MOUNT, M_NOWAIT);
	if (buf == NULL)
		return;
	sbuf_new(&sb, buf, DEVCTL_LEN, SBUF_FIXEDLEN);
	sbuf_cpy(&sb, "mount-point=\"");
	devctl_safe_quote_sb(&sb, sfp->f_mntonname);
	sbuf_cat(&sb, "\" mount-dev=\"");
	devctl_safe_quote_sb(&sb, sfp->f_mntfromname);
	sbuf_cat(&sb, "\" mount-type=\"");
	devctl_safe_quote_sb(&sb, sfp->f_fstypename);
	sbuf_cat(&sb, "\" fsid=0x");
	cp = (const uint8_t *)&sfp->f_fsid.val[0];
	for (int i = 0; i < sizeof(sfp->f_fsid); i++)
		sbuf_printf(&sb, "%02x", cp[i]);
	sbuf_printf(&sb, " owner=%u flags=\"", sfp->f_owner);
	for (fp = optnames; fp->o_opt != 0; fp++) {
		if ((mp->mnt_flag & fp->o_opt) != 0) {
			sbuf_cat(&sb, fp->o_name);
			sbuf_putc(&sb, ';');
		}
	}
	sbuf_putc(&sb, '"');
	mount_devctl_event_mntopt(&sb, "opt", mp->mnt_opt);
	if (donew)
		mount_devctl_event_mntopt(&sb, "optnew", mp->mnt_optnew);
	sbuf_finish(&sb);

	if (sbuf_error(&sb) == 0)
		devctl_notify("VFS", "FS", type, sbuf_data(&sb));
	sbuf_delete(&sb);
	vos_free(buf, M_MOUNT);
}

/*
 * Suspend write operations on all local writeable filesystems.  Does
 * full sync of them in the process.
 *
 * Iterate over the mount points in reverse order, suspending most
 * recently mounted filesystems first.  It handles a case where a
 * filesystem mounted from a md(4) vnode-backed device should be
 * suspended before the filesystem that owns the vnode.
 */
void
suspend_all_fs(void)
{
	struct mount *mp;
	int error;

	mtx_lock(&mountlist_mtx);
	TAILQ_FOREACH_REVERSE(mp, &mountlist, mntlist, mnt_list) {
		error = vfs_busy(mp, MBF_MNTLSTLOCK | MBF_NOWAIT);
		if (error != 0)
			continue;
		if ((mp->mnt_flag & (MNT_RDONLY | MNT_LOCAL)) != MNT_LOCAL ||
		    (mp->mnt_kern_flag & MNTK_SUSPEND) != 0) {
			mtx_lock(&mountlist_mtx);
			vfs_unbusy(mp);
			continue;
		}
		error = vfs_write_suspend(mp, 0);
		if (error == 0) {
			MNT_ILOCK(mp);
			MPASS((mp->mnt_kern_flag & MNTK_SUSPEND_ALL) == 0);
			mp->mnt_kern_flag |= MNTK_SUSPEND_ALL;
			MNT_IUNLOCK(mp);
			mtx_lock(&mountlist_mtx);
		} else {
			printf("suspend of %s failed, error %d\n",
			    mp->mnt_stat.f_mntonname, error);
			mtx_lock(&mountlist_mtx);
			vfs_unbusy(mp);
		}
	}
	mtx_unlock(&mountlist_mtx);
}

void
resume_all_fs(void)
{
	struct mount *mp;

	mtx_lock(&mountlist_mtx);
	TAILQ_FOREACH(mp, &mountlist, mnt_list) {
		if ((mp->mnt_kern_flag & MNTK_SUSPEND_ALL) == 0)
			continue;
		mtx_unlock(&mountlist_mtx);
		MNT_ILOCK(mp);
		MPASS((mp->mnt_kern_flag & MNTK_SUSPEND) != 0);
		mp->mnt_kern_flag &= ~MNTK_SUSPEND_ALL;
		MNT_IUNLOCK(mp);
		vfs_write_resume(mp, 0);
		mtx_lock(&mountlist_mtx);
		vfs_unbusy(mp);
	}
	mtx_unlock(&mountlist_mtx);
}
