/*
 * Copyright (C) 2009-2012 the libgit2 contributors
 *
 * This file is part of libgit2, distributed under the GNU GPL v2 with
 * a Linking Exception. For full terms see the included COPYING file.
 */

#include "pack-objects.h"

#include "compress.h"
#include "delta.h"
#include "iterator.h"
#include "netops.h"
#include "pack.h"
#include "thread-utils.h"
#include "tree.h"

#include "git2/pack.h"
#include "git2/commit.h"
#include "git2/tag.h"
#include "git2/indexer.h"
#include "git2/config.h"

GIT__USE_OIDMAP;

struct unpacked {
	git_pobject *object;
	void *data;
	struct git_delta_index *index;
	unsigned int depth;
};

static unsigned name_hash(const char *name)
{
	unsigned c, hash = 0;

	if (!name)
		return 0;

	/*
	 * This effectively just creates a sortable number from the
	 * last sixteen non-whitespace characters. Last characters
	 * count "most", so things that end in ".c" sort together.
	 */
	while ((c = *name++) != 0) {
		if (git__isspace(c))
			continue;
		hash = (hash >> 2) + (c << 24);
	}
	return hash;
}

static int packbuilder_config(git_packbuilder *pb)
{
	git_config *config;
	int ret;

	if (git_repository_config__weakptr(&config, pb->repo) < 0)
		return -1;

#define config_get(key, dst, default) \
	ret = git_config_get_int64((int64_t *)&dst, config, key); \
	if (ret == GIT_ENOTFOUND) \
		dst = default; \
	else if (ret < 0) \
		return -1;

	config_get("pack.deltaCacheSize", pb->max_delta_cache_size,
		   GIT_PACK_DELTA_CACHE_SIZE);
	config_get("pack.deltaCacheLimit", pb->cache_max_small_delta_size,
		   GIT_PACK_DELTA_CACHE_LIMIT);
	config_get("pack.deltaCacheSize", pb->big_file_threshold,
		   GIT_PACK_BIG_FILE_THRESHOLD);
	config_get("pack.windowMemory", pb->window_memory_limit, 0);

#undef config_get

	return 0;
}

int git_packbuilder_new(git_packbuilder **out, git_repository *repo)
{
	git_packbuilder *pb;

	*out = NULL;

	pb = git__malloc(sizeof(*pb));
	GITERR_CHECK_ALLOC(pb);

	memset(pb, 0x0, sizeof(*pb));

	pb->object_ix = git_oidmap_alloc();
	GITERR_CHECK_ALLOC(pb->object_ix);

	pb->repo = repo;
	pb->nr_threads = 1; /* do not spawn any thread by default */
	pb->ctx = git_hash_new_ctx();

	if (git_repository_odb(&pb->odb, repo) < 0)
		goto on_error;

	if (packbuilder_config(pb) < 0)
		goto on_error;

	*out = pb;
	return 0;

on_error:
	git__free(pb);
	return -1;
}

void git_packbuilder_set_threads(git_packbuilder *pb, unsigned int n)
{
	assert(pb);
	pb->nr_threads = n;
}

static void rehash(git_packbuilder *pb)
{
	git_pobject *po;
	khiter_t pos;
	unsigned int i;
	int ret;

	kh_clear(oid, pb->object_ix);
	for (i = 0, po = pb->object_list; i < pb->nr_objects; i++, po++) {
		pos = kh_put(oid, pb->object_ix, &po->id, &ret);
		kh_value(pb->object_ix, pos) = po;
	}
}

int git_packbuilder_insert(git_packbuilder *pb, const git_oid *oid,
			   const char *name)
{
	git_pobject *po;
	git_odb_object *obj;
	khiter_t pos;
	int ret;

	assert(pb && oid);

	pos = kh_get(oid, pb->object_ix, oid);
	if (pos != kh_end(pb->object_ix))
		return 0;

	if (pb->nr_objects >= pb->nr_alloc) {
		pb->nr_alloc = (pb->nr_alloc + 1024) * 3 / 2;
		pb->object_list = git__realloc(pb->object_list,
					       pb->nr_alloc * sizeof(*po));
		GITERR_CHECK_ALLOC(pb->object_list);
		rehash(pb);
	}

	if (git_odb_read(&obj, pb->odb, oid) < 0)
		return -1;

	po = pb->object_list + pb->nr_objects++;
	memset(po, 0x0, sizeof(*po));

	git_oid_cpy(&po->id, oid);
	po->type = git_odb_object_type(obj);
	po->size = git_odb_object_size(obj);
	git_odb_object_free(obj);
	po->hash = name_hash(name);

	pos = kh_put(oid, pb->object_ix, &po->id, &ret);
	assert(ret != 0);
	kh_value(pb->object_ix, pos) = po;

	pb->done = false;
	return 0;
}

/*
 * The per-object header is a pretty dense thing, which is
 *  - first byte: low four bits are "size",
 *    then three bits of "type",
 *    with the high bit being "size continues".
 *  - each byte afterwards: low seven bits are size continuation,
 *    with the high bit being "size continues"
 */
static int gen_pack_object_header(
		unsigned char *hdr,
		unsigned long size,
		git_otype type)
{
	unsigned char *hdr_base;
	unsigned char c;

	assert(type >= GIT_OBJ_COMMIT && type <= GIT_OBJ_REF_DELTA);

	/* TODO: add support for chunked objects; see git.git 6c0d19b1 */

	c = (unsigned char)((type << 4) | (size & 15));
	size >>= 4;
	hdr_base = hdr;

	while (size) {
		*hdr++ = c | 0x80;
		c = size & 0x7f;
		size >>= 7;
	}
	*hdr++ = c;

	return hdr - hdr_base;
}

static int get_delta(void **out, git_odb *odb, git_pobject *po)
{
	git_odb_object *src = NULL, *trg = NULL;
	unsigned long delta_size;
	void *delta_buf;

	*out = NULL;

	if (git_odb_read(&src, odb, &po->delta->id) < 0 ||
	    git_odb_read(&trg, odb, &po->id) < 0)
		goto on_error;

	delta_buf = git_delta(git_odb_object_data(src), git_odb_object_size(src),
			      git_odb_object_data(trg), git_odb_object_size(trg),
			      &delta_size, 0);

	if (!delta_buf || delta_size != po->delta_size) {
		giterr_set(GITERR_INVALID, "Delta size changed");
		goto on_error;
	}

	*out = delta_buf;

	git_odb_object_free(src);
	git_odb_object_free(trg);
	return 0;

on_error:
	git_odb_object_free(src);
	git_odb_object_free(trg);
	return -1;
}

static int write_object(git_buf *buf, git_packbuilder *pb, git_pobject *po)
{
	git_odb_object *obj = NULL;
	git_buf zbuf = GIT_BUF_INIT;
	git_otype type;
	unsigned char hdr[10];
	unsigned int hdr_len;
	unsigned long size;
	void *data;

	if (po->delta) {
		if (po->delta_data)
			data = po->delta_data;
		else if (get_delta(&data, pb->odb, po) < 0)
				goto on_error;
		size = po->delta_size;
		type = GIT_OBJ_REF_DELTA;
	} else {
		if (git_odb_read(&obj, pb->odb, &po->id))
			goto on_error;

		data = (void *)git_odb_object_data(obj);
		size = git_odb_object_size(obj);
		type = git_odb_object_type(obj);
	}

	/* Write header */
	hdr_len = gen_pack_object_header(hdr, size, type);

	if (git_buf_put(buf, (char *)hdr, hdr_len) < 0)
		goto on_error;

	git_hash_update(pb->ctx, hdr, hdr_len);

	if (type == GIT_OBJ_REF_DELTA) {
		if (git_buf_put(buf, (char *)po->delta->id.id,
				GIT_OID_RAWSZ) < 0)
			goto on_error;

		git_hash_update(pb->ctx, po->delta->id.id, GIT_OID_RAWSZ);
	}

	/* Write data */
	if (po->z_delta_size)
		size = po->z_delta_size;
	else if (git__compress(&zbuf, data, size) < 0)
		goto on_error;
	else {
		if (po->delta)
			git__free(data);
		data = zbuf.ptr;
		size = zbuf.size;
	}

	if (git_buf_put(buf, data, size) < 0)
		goto on_error;

	git_hash_update(pb->ctx, data, size);

	if (po->delta_data)
		git__free(po->delta_data);

	git_odb_object_free(obj);
	git_buf_free(&zbuf);

	pb->nr_written++;
	return 0;

on_error:
	git_odb_object_free(obj);
	git_buf_free(&zbuf);
	return -1;
}

enum write_one_status {
	WRITE_ONE_SKIP = -1, /* already written */
	WRITE_ONE_BREAK = 0, /* writing this will bust the limit; not written */
	WRITE_ONE_WRITTEN = 1, /* normal */
	WRITE_ONE_RECURSIVE = 2 /* already scheduled to be written */
};

static int write_one(git_buf *buf, git_packbuilder *pb, git_pobject *po,
		     enum write_one_status *status)
{
	if (po->recursing) {
		*status = WRITE_ONE_RECURSIVE;
		return 0;
	} else if (po->written) {
		*status = WRITE_ONE_SKIP;
		return 0;
	}

	if (po->delta) {
		po->recursing = 1;
		if (write_one(buf, pb, po->delta, status) < 0)
			return -1;
		switch (*status) {
		case WRITE_ONE_RECURSIVE:
			/* we cannot depend on this one */
			po->delta = NULL;
			break;
		default:
			break;
		}
	}

	po->written = 1;
	po->recursing = 0;
	return write_object(buf, pb, po);
}

GIT_INLINE(void) add_to_write_order(git_pobject **wo, unsigned int *endp,
				    git_pobject *po)
{
	if (po->filled)
		return;
	wo[(*endp)++] = po;
	po->filled = 1;
}

static void add_descendants_to_write_order(git_pobject **wo, unsigned int *endp,
					   git_pobject *po)
{
	int add_to_order = 1;
	while (po) {
		if (add_to_order) {
			git_pobject *s;
			/* add this node... */
			add_to_write_order(wo, endp, po);
			/* all its siblings... */
			for (s = po->delta_sibling; s; s = s->delta_sibling) {
				add_to_write_order(wo, endp, s);
			}
		}
		/* drop down a level to add left subtree nodes if possible */
		if (po->delta_child) {
			add_to_order = 1;
			po = po->delta_child;
		} else {
			add_to_order = 0;
			/* our sibling might have some children, it is next */
			if (po->delta_sibling) {
				po = po->delta_sibling;
				continue;
			}
			/* go back to our parent node */
			po = po->delta;
			while (po && !po->delta_sibling) {
				/* we're on the right side of a subtree, keep
				 * going up until we can go right again */
				po = po->delta;
			}
			if (!po) {
				/* done- we hit our original root node */
				return;
			}
			/* pass it off to sibling at this level */
			po = po->delta_sibling;
		}
	};
}

static void add_family_to_write_order(git_pobject **wo, unsigned int *endp,
				      git_pobject *po)
{
	git_pobject *root;

	for (root = po; root->delta; root = root->delta)
		; /* nothing */
	add_descendants_to_write_order(wo, endp, root);
}

static int cb_tag_foreach(const char *name, git_oid *oid, void *data)
{
	git_packbuilder *pb = data;
	git_pobject *po;
	khiter_t pos;

	GIT_UNUSED(name);

	pos = kh_get(oid, pb->object_ix, oid);
	if (pos == kh_end(pb->object_ix))
		return 0;

	po = kh_value(pb->object_ix, pos);
	po->tagged = 1;

	/* TODO: peel objects */

	return 0;
}

static git_pobject **compute_write_order(git_packbuilder *pb)
{
	unsigned int i, wo_end, last_untagged;

	git_pobject **wo = git__malloc(sizeof(*wo) * pb->nr_objects);

	for (i = 0; i < pb->nr_objects; i++) {
		git_pobject *po = pb->object_list + i;
		po->tagged = 0;
		po->filled = 0;
		po->delta_child = NULL;
		po->delta_sibling = NULL;
	}

	/*
	 * Fully connect delta_child/delta_sibling network.
	 * Make sure delta_sibling is sorted in the original
	 * recency order.
	 */
	for (i = pb->nr_objects; i > 0;) {
		git_pobject *po = &pb->object_list[--i];
		if (!po->delta)
			continue;
		/* Mark me as the first child */
		po->delta_sibling = po->delta->delta_child;
		po->delta->delta_child = po;
	}

	/*
	 * Mark objects that are at the tip of tags.
	 */
	if (git_tag_foreach(pb->repo, &cb_tag_foreach, pb) < 0)
		return NULL;

	/*
	 * Give the objects in the original recency order until
	 * we see a tagged tip.
	 */
	for (i = wo_end = 0; i < pb->nr_objects; i++) {
		git_pobject *po = pb->object_list + i;
		if (po->tagged)
			break;
		add_to_write_order(wo, &wo_end, po);
	}
	last_untagged = i;

	/*
	 * Then fill all the tagged tips.
	 */
	for (; i < pb->nr_objects; i++) {
		git_pobject *po = pb->object_list + i;
		if (po->tagged)
			add_to_write_order(wo, &wo_end, po);
	}

	/*
	 * And then all remaining commits and tags.
	 */
	for (i = last_untagged; i < pb->nr_objects; i++) {
		git_pobject *po = pb->object_list + i;
		if (po->type != GIT_OBJ_COMMIT &&
		    po->type != GIT_OBJ_TAG)
			continue;
		add_to_write_order(wo, &wo_end, po);
	}

	/*
	 * And then all the trees.
	 */
	for (i = last_untagged; i < pb->nr_objects; i++) {
		git_pobject *po = pb->object_list + i;
		if (po->type != GIT_OBJ_TREE)
			continue;
		add_to_write_order(wo, &wo_end, po);
	}

	/*
	 * Finally all the rest in really tight order
	 */
	for (i = last_untagged; i < pb->nr_objects; i++) {
		git_pobject *po = pb->object_list + i;
		if (!po->filled)
			add_family_to_write_order(wo, &wo_end, po);
	}

	if (wo_end != pb->nr_objects) {
		giterr_set(GITERR_INVALID, "invalid write order");
		return NULL;
	}

	return wo;
}

static int write_pack(git_packbuilder *pb,
		      int (*cb)(void *buf, size_t size, void *data),
		      void *data)
{
	git_pobject **write_order;
	git_pobject *po;
	git_buf buf = GIT_BUF_INIT;
	enum write_one_status status;
	struct git_pack_header ph;
	unsigned int i = 0;

	write_order = compute_write_order(pb);
	if (write_order == NULL)
		goto on_error;

	/* Write pack header */
	ph.hdr_signature = htonl(PACK_SIGNATURE);
	ph.hdr_version = htonl(PACK_VERSION);
	ph.hdr_entries = htonl(pb->nr_objects);

	if (cb(&ph, sizeof(ph), data) < 0)
		goto on_error;

	git_hash_update(pb->ctx, &ph, sizeof(ph));

	pb->nr_remaining = pb->nr_objects;
	do {
		pb->nr_written = 0;
		for ( ; i < pb->nr_objects; ++i) {
			po = write_order[i];
			if (write_one(&buf, pb, po, &status) < 0)
				goto on_error;
			if (cb(buf.ptr, buf.size, data) < 0)
				goto on_error;
			git_buf_clear(&buf);
		}

		pb->nr_remaining -= pb->nr_written;
	} while (pb->nr_remaining && i < pb->nr_objects);

	git__free(write_order);
	git_buf_free(&buf);
	git_hash_final(&pb->pack_oid, pb->ctx);

	return cb(pb->pack_oid.id, GIT_OID_RAWSZ, data);

on_error:
	git__free(write_order);
	git_buf_free(&buf);
	return -1;
}

static int send_pack_file(void *buf, size_t size, void *data)
{
	git_transport *t = (git_transport *)data;
	return gitno_send(t, buf, size, 0);
}

static int write_pack_buf(void *buf, size_t size, void *data)
{
	git_buf *b = (git_buf *)data;
	return git_buf_put(b, buf, size);
}

static int write_pack_to_file(void *buf, size_t size, void *data)
{
	git_filebuf *file = (git_filebuf *)data;
	return git_filebuf_write(file, buf, size);
}

static int write_pack_file(git_packbuilder *pb, const char *path)
{
	git_filebuf file = GIT_FILEBUF_INIT;

	if (git_filebuf_open(&file, path, 0) < 0 ||
	    write_pack(pb, &write_pack_to_file, &file) < 0 ||
	    git_filebuf_commit(&file, GIT_PACK_FILE_MODE) < 0) {
		git_filebuf_cleanup(&file);
		return -1;
	}

	return 0;
}

static int type_size_sort(const void *_a, const void *_b)
{
	const git_pobject *a = (git_pobject *)_a;
	const git_pobject *b = (git_pobject *)_b;

	if (a->type > b->type)
		return -1;
	if (a->type < b->type)
		return 1;
	if (a->hash > b->hash)
		return -1;
	if (a->hash < b->hash)
		return 1;
	/*
	 * TODO
	 *
	if (a->preferred_base > b->preferred_base)
		return -1;
	if (a->preferred_base < b->preferred_base)
		return 1;
	*/
	if (a->size > b->size)
		return -1;
	if (a->size < b->size)
		return 1;
	return a < b ? -1 : (a > b); /* newest first */
}

static int delta_cacheable(git_packbuilder *pb, unsigned long src_size,
			   unsigned long trg_size, unsigned long delta_size)
{
	if (pb->max_delta_cache_size &&
		pb->delta_cache_size + delta_size > pb->max_delta_cache_size)
		return 0;

	if (delta_size < pb->cache_max_small_delta_size)
		return 1;

	/* cache delta, if objects are large enough compared to delta size */
	if ((src_size >> 20) + (trg_size >> 21) > (delta_size >> 10))
		return 1;

	return 0;
}

#ifdef GIT_THREADS
static git_mutex cache_mutex;
#define cache_lock()		git_mutex_lock(&cache_mutex);
#define cache_unlock()		git_mutex_unlock(&cache_mutex);

static git_mutex progress_mutex;
#define progress_lock()		git_mutex_lock(&progress_mutex);
#define progress_unlock()	git_mutex_unlock(&progress_mutex);

static git_cond progress_cond;
#else

#define cache_lock()		(void)0;
#define cache_unlock()		(void)0;
#define progress_lock()		(void)0;
#define progress_unlock()	(void)0;
#endif

static int try_delta(git_packbuilder *pb, struct unpacked *trg,
		     struct unpacked *src, unsigned int max_depth,
		     unsigned long *mem_usage, int *ret)
{
	git_pobject *trg_object = trg->object;
	git_pobject *src_object = src->object;
	git_odb_object *obj;
	unsigned long trg_size, src_size, delta_size,
		      sizediff, max_size, sz;
	unsigned int ref_depth;
	void *delta_buf;

	/* Don't bother doing diffs between different types */
	if (trg_object->type != src_object->type) {
		*ret = -1;
		return 0;
	}

	*ret = 0;

	/* TODO: support reuse-delta */

	/* Let's not bust the allowed depth. */
	if (src->depth >= max_depth)
		return 0;

	/* Now some size filtering heuristics. */
	trg_size = trg_object->size;
	if (!trg_object->delta) {
		max_size = trg_size/2 - 20;
		ref_depth = 1;
	} else {
		max_size = trg_object->delta_size;
		ref_depth = trg->depth;
	}

	max_size = (uint64_t)max_size * (max_depth - src->depth) /
					(max_depth - ref_depth + 1);
	if (max_size == 0)
		return 0;

	src_size = src_object->size;
	sizediff = src_size < trg_size ? trg_size - src_size : 0;
	if (sizediff >= max_size)
		return 0;
	if (trg_size < src_size / 32)
		return 0;

	/* Load data if not already done */
	if (!trg->data) {
		if (git_odb_read(&obj, pb->odb, &trg_object->id) < 0)
			return -1;

		sz = git_odb_object_size(obj);
		trg->data = git__malloc(sz);
		GITERR_CHECK_ALLOC(trg->data);
		memcpy(trg->data, git_odb_object_data(obj), sz);

		git_odb_object_free(obj);

		if (sz != trg_size) {
			giterr_set(GITERR_INVALID,
				   "Inconsistent target object length");
			return -1;
		}

		*mem_usage += sz;
	}
	if (!src->data) {
		if (git_odb_read(&obj, pb->odb, &src_object->id) < 0)
			return -1;

		sz = git_odb_object_size(obj);
		src->data = git__malloc(sz);
		GITERR_CHECK_ALLOC(src->data);
		memcpy(src->data, git_odb_object_data(obj), sz);

		git_odb_object_free(obj);

		if (sz != src_size) {
			giterr_set(GITERR_INVALID,
				   "Inconsistent source object length");
			return -1;
		}

		*mem_usage += sz;
	}
	if (!src->index) {
		src->index = git_delta_create_index(src->data, src_size);
		if (!src->index)
			return 0; /* suboptimal pack - out of memory */

		*mem_usage += git_delta_sizeof_index(src->index);
	}

	delta_buf = git_delta_create(src->index, trg->data, trg_size,
				     &delta_size, max_size);
	if (!delta_buf)
		return 0;

	if (trg_object->delta) {
		/* Prefer only shallower same-sized deltas. */
		if (delta_size == trg_object->delta_size &&
		    src->depth + 1 >= trg->depth) {
			git__free(delta_buf);
			return 0;
		}
	}

	cache_lock();
	if (trg_object->delta_data) {
		git__free(trg_object->delta_data);
		pb->delta_cache_size -= trg_object->delta_size;
		trg_object->delta_data = NULL;
	}
	if (delta_cacheable(pb, src_size, trg_size, delta_size)) {
		pb->delta_cache_size += delta_size;
		cache_unlock();

		trg_object->delta_data = git__realloc(delta_buf, delta_size);
		GITERR_CHECK_ALLOC(trg_object->delta_data);
	} else {
		/* create delta when writing the pack */
		cache_unlock();
		git__free(delta_buf);
	}

	trg_object->delta = src_object;
	trg_object->delta_size = delta_size;
	trg->depth = src->depth + 1;

	*ret = 1;
	return 0;
}

static unsigned int check_delta_limit(git_pobject *me, unsigned int n)
{
	git_pobject *child = me->delta_child;
	unsigned int m = n;

	while (child) {
		unsigned int c = check_delta_limit(child, n + 1);
		if (m < c)
			m = c;
		child = child->delta_sibling;
	}
	return m;
}

static unsigned long free_unpacked(struct unpacked *n)
{
	unsigned long freed_mem = git_delta_sizeof_index(n->index);
	git_delta_free_index(n->index);
	n->index = NULL;
	if (n->data) {
		freed_mem += n->object->size;
		git__free(n->data);
		n->data = NULL;
	}
	n->object = NULL;
	n->depth = 0;
	return freed_mem;
}

static int find_deltas(git_packbuilder *pb, git_pobject **list,
		       unsigned int *list_size, unsigned int window,
		       unsigned int depth)
{
	git_pobject *po;
	git_buf zbuf = GIT_BUF_INIT;
	struct unpacked *array;
	uint32_t idx = 0, count = 0;
	unsigned long mem_usage = 0;
	unsigned int i;
	int error = -1;

	array = git__calloc(window, sizeof(struct unpacked));
	GITERR_CHECK_ALLOC(array);

	for (;;) {
		struct unpacked *n = array + idx;
		unsigned int max_depth;
		int j, best_base = -1;

		progress_lock();
		if (!*list_size) {
			progress_unlock();
			break;
		}

		po = *list++;
		(*list_size)--;
		progress_unlock();

		mem_usage -= free_unpacked(n);
		n->object = po;

		while (pb->window_memory_limit &&
		       mem_usage > pb->window_memory_limit &&
		       count > 1) {
			uint32_t tail = (idx + window - count) % window;
			mem_usage -= free_unpacked(array + tail);
			count--;
		}

		/*
		 * If the current object is at pack edge, take the depth the
		 * objects that depend on the current object into account
		 * otherwise they would become too deep.
		 */
		max_depth = depth;
		if (po->delta_child) {
			max_depth -= check_delta_limit(po, 0);
			if (max_depth <= 0)
				goto next;
		}

		j = window;
		while (--j > 0) {
			int ret;
			uint32_t other_idx = idx + j;
			struct unpacked *m;

			if (other_idx >= window)
				other_idx -= window;

			m = array + other_idx;
			if (!m->object)
				break;

			if (try_delta(pb, n, m, max_depth, &mem_usage, &ret) < 0)
				goto on_error;
			if (ret < 0)
				break;
			else if (ret > 0)
				best_base = other_idx;
		}

		/*
		 * If we decided to cache the delta data, then it is best
		 * to compress it right away.  First because we have to do
		 * it anyway, and doing it here while we're threaded will
		 * save a lot of time in the non threaded write phase,
		 * as well as allow for caching more deltas within
		 * the same cache size limit.
		 * ...
		 * But only if not writing to stdout, since in that case
		 * the network is most likely throttling writes anyway,
		 * and therefore it is best to go to the write phase ASAP
		 * instead, as we can afford spending more time compressing
		 * between writes at that moment.
		 */
		if (po->delta_data) {
			if (git__compress(&zbuf, po->delta_data, po->delta_size) < 0)
				goto on_error;

			git__free(po->delta_data);
			po->delta_data = git__malloc(zbuf.size);
			GITERR_CHECK_ALLOC(po->delta_data);

			memcpy(po->delta_data, zbuf.ptr, zbuf.size);
			po->z_delta_size = zbuf.size;
			git_buf_clear(&zbuf);

			cache_lock();
			pb->delta_cache_size -= po->delta_size;
			pb->delta_cache_size += po->z_delta_size;
			cache_unlock();
		}

		/*
		 * If we made n a delta, and if n is already at max
		 * depth, leaving it in the window is pointless.  we
		 * should evict it first.
		 */
		if (po->delta && max_depth <= n->depth)
			continue;

		/*
		 * Move the best delta base up in the window, after the
		 * currently deltified object, to keep it longer.  It will
		 * be the first base object to be attempted next.
		 */
		if (po->delta) {
			struct unpacked swap = array[best_base];
			int dist = (window + idx - best_base) % window;
			int dst = best_base;
			while (dist--) {
				int src = (dst + 1) % window;
				array[dst] = array[src];
				dst = src;
			}
			array[dst] = swap;
		}

		next:
		idx++;
		if (count + 1 < window)
			count++;
		if (idx >= window)
			idx = 0;
	}
	error = 0;

on_error:
	for (i = 0; i < window; ++i) {
		git__free(array[i].index);
		git__free(array[i].data);
	}
	git__free(array);
	git_buf_free(&zbuf);

	return error;
}

#ifdef GIT_THREADS

struct thread_params {
	git_thread thread;
	git_packbuilder *pb;

	git_pobject **list;

	git_cond cond;
	git_mutex mutex;

	unsigned int list_size;
	unsigned int remaining;

	int window;
	int depth;
	int working;
	int data_ready;
};

static void init_threaded_search(void)
{
	git_mutex_init(&cache_mutex);
	git_mutex_init(&progress_mutex);

	git_cond_init(&progress_cond);
}

static void cleanup_threaded_search(void)
{
	git_cond_free(&progress_cond);

	git_mutex_free(&cache_mutex);
	git_mutex_free(&progress_mutex);
}

static void *threaded_find_deltas(void *arg)
{
	struct thread_params *me = arg;

	while (me->remaining) {
		if (find_deltas(me->pb, me->list, &me->remaining,
				me->window, me->depth) < 0) {
			; /* TODO */
		}

		progress_lock();
		me->working = 0;
		git_cond_signal(&progress_cond);
		progress_unlock();

		/*
		 * We must not set ->data_ready before we wait on the
		 * condition because the main thread may have set it to 1
		 * before we get here. In order to be sure that new
		 * work is available if we see 1 in ->data_ready, it
		 * was initialized to 0 before this thread was spawned
		 * and we reset it to 0 right away.
		 */
		git_mutex_lock(&me->mutex);
		while (!me->data_ready)
			git_cond_wait(&me->cond, &me->mutex);
		me->data_ready = 0;
		git_mutex_unlock(&me->mutex);
	}
	/* leave ->working 1 so that this doesn't get more work assigned */
	return NULL;
}

static int ll_find_deltas(git_packbuilder *pb, git_pobject **list,
			  unsigned int list_size, unsigned int window,
			  unsigned int depth)
{
	struct thread_params *p;
	int i, ret, active_threads = 0;

	init_threaded_search();

	if (!pb->nr_threads)
		pb->nr_threads = git_online_cpus();
	if (pb->nr_threads <= 1) {
		find_deltas(pb, list, &list_size, window, depth);
		cleanup_threaded_search();
		return 0;
	}

	p = git__malloc(pb->nr_threads * sizeof(*p));
	GITERR_CHECK_ALLOC(p);

	/* Partition the work among the threads */
	for (i = 0; i < pb->nr_threads; ++i) {
		unsigned sub_size = list_size / (pb->nr_threads - i);

		/* don't use too small segments or no deltas will be found */
		if (sub_size < 2*window && i+1 < pb->nr_threads)
			sub_size = 0;

		p[i].pb = pb;
		p[i].window = window;
		p[i].depth = depth;
		p[i].working = 1;
		p[i].data_ready = 0;

		/* try to split chunks on "path" boundaries */
		while (sub_size && sub_size < list_size &&
		       list[sub_size]->hash &&
		       list[sub_size]->hash == list[sub_size-1]->hash)
			sub_size++;

		p[i].list = list;
		p[i].list_size = sub_size;
		p[i].remaining = sub_size;

		list += sub_size;
		list_size -= sub_size;
	}

	/* Start work threads */
	for (i = 0; i < pb->nr_threads; ++i) {
		if (!p[i].list_size)
			continue;

		git_mutex_init(&p[i].mutex);
		git_cond_init(&p[i].cond);

		ret = git_thread_create(&p[i].thread, NULL,
					threaded_find_deltas, &p[i]);
		if (ret) {
			giterr_set(GITERR_THREAD, "unable to create thread");
			return -1;
		}
		active_threads++;
	}

	/*
	 * Now let's wait for work completion.  Each time a thread is done
	 * with its work, we steal half of the remaining work from the
	 * thread with the largest number of unprocessed objects and give
	 * it to that newly idle thread.  This ensure good load balancing
	 * until the remaining object list segments are simply too short
	 * to be worth splitting anymore.
	 */
	while (active_threads) {
		struct thread_params *target = NULL;
		struct thread_params *victim = NULL;
		unsigned sub_size = 0;

		progress_lock();
		for (;;) {
			for (i = 0; !target && i < pb->nr_threads; i++)
				if (!p[i].working)
					target = &p[i];
			if (target)
				break;
			git_cond_wait(&progress_cond, &progress_mutex);
		}

		for (i = 0; i < pb->nr_threads; i++)
			if (p[i].remaining > 2*window &&
			    (!victim || victim->remaining < p[i].remaining))
				victim = &p[i];
		if (victim) {
			sub_size = victim->remaining / 2;
			list = victim->list + victim->list_size - sub_size;
			while (sub_size && list[0]->hash &&
			       list[0]->hash == list[-1]->hash) {
				list++;
				sub_size--;
			}
			if (!sub_size) {
				/*
				 * It is possible for some "paths" to have
				 * so many objects that no hash boundary
				 * might be found.  Let's just steal the
				 * exact half in that case.
				 */
				sub_size = victim->remaining / 2;
				list -= sub_size;
			}
			target->list = list;
			victim->list_size -= sub_size;
			victim->remaining -= sub_size;
		}
		target->list_size = sub_size;
		target->remaining = sub_size;
		target->working = 1;
		progress_unlock();

		git_mutex_lock(&target->mutex);
		target->data_ready = 1;
		git_cond_signal(&target->cond);
		git_mutex_unlock(&target->mutex);

		if (!sub_size) {
			git_thread_join(target->thread, NULL);
			git_cond_free(&target->cond);
			git_mutex_free(&target->mutex);
			active_threads--;
		}
	}

	cleanup_threaded_search();
	git__free(p);
	return 0;
}

#else
#define ll_find_deltas(pb, l, ls, w, d) find_deltas(pb, l, &ls, w, d)
#endif

static void get_object_details(git_packbuilder *pb)
{
	git_pobject *po;
	unsigned int i;

	for (i = 0; i < pb->nr_objects; ++i) {
		po = &pb->object_list[i];
		if (pb->big_file_threshold < po->size)
			po->no_try_delta = 1;
	}
}

static int prepare_pack(git_packbuilder *pb)
{
	git_pobject **delta_list;
	unsigned int i, n = 0;

	if (pb->nr_objects == 0 || pb->done)
		return 0; /* nothing to do */

	get_object_details(pb);

	delta_list = git__malloc(pb->nr_objects * sizeof(*delta_list));
	GITERR_CHECK_ALLOC(delta_list);

	for (i = 0; i < pb->nr_objects; ++i) {
		git_pobject *po = pb->object_list + i;

		if (po->size < 50)
			continue;

		if (po->no_try_delta)
			continue;

		delta_list[n++] = po;
	}

	if (n > 1) {
		git__tsort((void **)delta_list, n, type_size_sort);
		if (ll_find_deltas(pb, delta_list, n,
				   GIT_PACK_WINDOW + 1,
				   GIT_PACK_DEPTH) < 0) {
			git__free(delta_list);
			return -1;
		}
	}

	pb->done = true;
	git__free(delta_list);
	return 0;
}

#define PREPARE_PACK if (prepare_pack(pb) < 0) { return -1; }

int git_packbuilder_send(git_packbuilder *pb, git_transport *t)
{
	PREPARE_PACK;
	return write_pack(pb, &send_pack_file, t);
}

int git_packbuilder_write_buf(git_buf *buf, git_packbuilder *pb)
{
	PREPARE_PACK;
	return write_pack(pb, &write_pack_buf, buf);
}

int git_packbuilder_write(git_packbuilder *pb, const char *path)
{
	PREPARE_PACK;
	return write_pack_file(pb, path);
}

#undef PREPARE_PACK

static int cb_tree_walk(const char *root, const git_tree_entry *entry, void *payload)
{
	git_packbuilder *pb = payload;
	git_buf buf = GIT_BUF_INIT;

	git_buf_puts(&buf, root);
	git_buf_puts(&buf, git_tree_entry_name(entry));

	if (git_packbuilder_insert(pb, git_tree_entry_id(entry),
				   git_buf_cstr(&buf)) < 0) {
		git_buf_free(&buf);
		return -1;
	}

	git_buf_free(&buf);
	return 0;
}

int git_packbuilder_insert_tree(git_packbuilder *pb, const git_oid *oid)
{
	git_tree *tree;

	if (git_tree_lookup(&tree, pb->repo, oid) < 0 ||
	    git_packbuilder_insert(pb, oid, NULL) < 0)
		return -1;

	if (git_tree_walk(tree, cb_tree_walk, GIT_TREEWALK_PRE, pb) < 0) {
		git_tree_free(tree);
		return -1;
	}

	git_tree_free(tree);
	return 0;
}

void git_packbuilder_free(git_packbuilder *pb)
{
	if (pb == NULL)
		return;

	git_odb_free(pb->odb);
	git_hash_free_ctx(pb->ctx);
	git_oidmap_free(pb->object_ix);
	git__free(pb->object_list);
	git__free(pb);
}