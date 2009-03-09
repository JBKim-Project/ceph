
#include <asm/div64.h>

#include "super.h"
#include "osdmap.h"
#include "crush/hash.h"
#include "decode.h"

#include "ceph_debug.h"

int ceph_debug_osdmap __read_mostly = -1;
#define DOUT_MASK DOUT_MASK_OSDMAP
#define DOUT_VAR ceph_debug_osdmap

/* maps */

static int calc_bits_of(unsigned t)
{
	int b = 0;
	while (t) {
		t = t >> 1;
		b++;
	}
	return b;
}

/*
 * the foo_mask is the smallest value 2^n-1 that is >= foo.
 */
static void calc_pg_masks(struct ceph_osdmap *map)
{
	map->pg_num_mask = (1 << calc_bits_of(map->pg_num-1)) - 1;
	map->pgp_num_mask = (1 << calc_bits_of(map->pgp_num-1)) - 1;
	map->lpg_num_mask = (1 << calc_bits_of(map->lpg_num-1)) - 1;
	map->lpgp_num_mask = (1 << calc_bits_of(map->lpgp_num-1)) - 1;
}

/*
 * decode crush map
 */
static int crush_decode_uniform_bucket(void **p, void *end,
				       struct crush_bucket_uniform *b)
{
	int j;
	dout(30, "crush_decode_uniform_bucket %p to %p\n", *p, end);
	b->primes = kmalloc(b->h.size * sizeof(u32), GFP_NOFS);
	if (b->primes == NULL)
		return -ENOMEM;
	ceph_decode_need(p, end, (1+b->h.size) * sizeof(u32), bad);
	for (j = 0; j < b->h.size; j++)
		ceph_decode_32(p, b->primes[j]);
	ceph_decode_32(p, b->item_weight);
	return 0;
bad:
	return -EINVAL;
}

static int crush_decode_list_bucket(void **p, void *end,
				    struct crush_bucket_list *b)
{
	int j;
	dout(30, "crush_decode_list_bucket %p to %p\n", *p, end);
	b->item_weights = kmalloc(b->h.size * sizeof(u32), GFP_NOFS);
	if (b->item_weights == NULL)
		return -ENOMEM;
	b->sum_weights = kmalloc(b->h.size * sizeof(u32), GFP_NOFS);
	if (b->sum_weights == NULL)
		return -ENOMEM;
	ceph_decode_need(p, end, 2 * b->h.size * sizeof(u32), bad);
	for (j = 0; j < b->h.size; j++) {
		ceph_decode_32(p, b->item_weights[j]);
		ceph_decode_32(p, b->sum_weights[j]);
	}
	return 0;
bad:
	return -EINVAL;
}

static int crush_decode_tree_bucket(void **p, void *end,
				    struct crush_bucket_tree *b)
{
	int j;
	dout(30, "crush_decode_tree_bucket %p to %p\n", *p, end);
	b->node_weights = kmalloc(b->h.size * sizeof(u32), GFP_NOFS);
	if (b->node_weights == NULL)
		return -ENOMEM;
	ceph_decode_need(p, end, b->h.size * sizeof(u32), bad);
	for (j = 0; j < b->h.size; j++)
		ceph_decode_32(p, b->node_weights[j]);
	return 0;
bad:
	return -EINVAL;
}

static int crush_decode_straw_bucket(void **p, void *end,
				     struct crush_bucket_straw *b)
{
	int j;
	dout(30, "crush_decode_straw_bucket %p to %p\n", *p, end);
	b->item_weights = kmalloc(b->h.size * sizeof(u32), GFP_NOFS);
	if (b->item_weights == NULL)
		return -ENOMEM;
	b->straws = kmalloc(b->h.size * sizeof(u32), GFP_NOFS);
	if (b->straws == NULL)
		return -ENOMEM;
	ceph_decode_need(p, end, 2 * b->h.size * sizeof(u32), bad);
	for (j = 0; j < b->h.size; j++) {
		ceph_decode_32(p, b->item_weights[j]);
		ceph_decode_32(p, b->straws[j]);
	}
	return 0;
bad:
	return -EINVAL;
}

static struct crush_map *crush_decode(void *pbyval, void *end)
{
	struct crush_map *c;
	int err = -EINVAL;
	int i, j;
	void **p = &pbyval;
	void *start = pbyval;
	u32 magic;

	dout(30, "crush_decode %p to %p len %d\n", *p, end, (int)(end - *p));

	c = kzalloc(sizeof(*c), GFP_NOFS);
	if (c == NULL)
		return ERR_PTR(-ENOMEM);

	ceph_decode_need(p, end, 4*sizeof(u32), bad);
	ceph_decode_32(p, magic);
	if (magic != CRUSH_MAGIC) {
		derr(0, "crush_decode magic %x != current %x\n",
		     (unsigned)magic, (unsigned)CRUSH_MAGIC);
		goto bad;
	}
	ceph_decode_32(p, c->max_buckets);
	ceph_decode_32(p, c->max_rules);
	ceph_decode_32(p, c->max_devices);

	c->device_parents = kmalloc(c->max_devices * sizeof(u32), GFP_NOFS);
	if (c->device_parents == NULL)
		goto badmem;
	c->bucket_parents = kmalloc(c->max_buckets * sizeof(u32), GFP_NOFS);
	if (c->bucket_parents == NULL)
		goto badmem;

	c->buckets = kmalloc(c->max_buckets * sizeof(*c->buckets), GFP_NOFS);
	if (c->buckets == NULL)
		goto badmem;
	c->rules = kmalloc(c->max_rules * sizeof(*c->rules), GFP_NOFS);
	if (c->rules == NULL)
		goto badmem;

	/* buckets */
	for (i = 0; i < c->max_buckets; i++) {
		int size = 0;
		u32 alg;
		struct crush_bucket *b;

		ceph_decode_32_safe(p, end, alg, bad);
		if (alg == 0) {
			c->buckets[i] = NULL;
			continue;
		}
		dout(30, "crush_decode bucket %d off %x %p to %p\n",
		     i, (int)(*p-start), *p, end);

		switch (alg) {
		case CRUSH_BUCKET_UNIFORM:
			size = sizeof(struct crush_bucket_uniform);
			break;
		case CRUSH_BUCKET_LIST:
			size = sizeof(struct crush_bucket_list);
			break;
		case CRUSH_BUCKET_TREE:
			size = sizeof(struct crush_bucket_tree);
			break;
		case CRUSH_BUCKET_STRAW:
			size = sizeof(struct crush_bucket_straw);
			break;
		default:
			goto bad;
		}
		BUG_ON(size == 0);
		b = c->buckets[i] = kzalloc(size, GFP_NOFS);
		if (b == NULL)
			goto badmem;

		ceph_decode_need(p, end, 4*sizeof(u32), bad);
		ceph_decode_32(p, b->id);
		ceph_decode_16(p, b->type);
		ceph_decode_16(p, b->alg);
		ceph_decode_32(p, b->weight);
		ceph_decode_32(p, b->size);

		dout(30, "crush_decode bucket size %d off %x %p to %p\n",
		     b->size, (int)(*p-start), *p, end);

		b->items = kmalloc(b->size * sizeof(__s32), GFP_NOFS);
		if (b->items == NULL)
			goto badmem;

		ceph_decode_need(p, end, b->size*sizeof(u32), bad);
		for (j = 0; j < b->size; j++)
			ceph_decode_32(p, b->items[j]);

		switch (b->alg) {
		case CRUSH_BUCKET_UNIFORM:
			err = crush_decode_uniform_bucket(p, end,
				  (struct crush_bucket_uniform *)b);
			if (err < 0)
				goto bad;
			break;
		case CRUSH_BUCKET_LIST:
			err = crush_decode_list_bucket(p, end,
			       (struct crush_bucket_list *)b);
			if (err < 0)
				goto bad;
			break;
		case CRUSH_BUCKET_TREE:
			err = crush_decode_tree_bucket(p, end,
				(struct crush_bucket_tree *)b);
			if (err < 0)
				goto bad;
			break;
		case CRUSH_BUCKET_STRAW:
			err = crush_decode_straw_bucket(p, end,
				(struct crush_bucket_straw *)b);
			if (err < 0)
				goto bad;
			break;
		}
	}

	/* rules */
	dout(30, "rule vec is %p\n", c->rules);
	for (i = 0; i < c->max_rules; i++) {
		u32 yes;
		struct crush_rule *r;

		ceph_decode_32_safe(p, end, yes, bad);
		if (!yes) {
			dout(30, "crush_decode NO rule %d off %x %p to %p\n",
			     i, (int)(*p-start), *p, end);
			c->rules[i] = NULL;
			continue;
		}

		dout(30, "crush_decode rule %d off %x %p to %p\n",
		     i, (int)(*p-start), *p, end);

		/* len */
		ceph_decode_32_safe(p, end, yes, bad);

		r = c->rules[i] = kmalloc(sizeof(*r) +
					  yes*sizeof(struct crush_rule_step),
					  GFP_NOFS);
		if (r == NULL)
			goto badmem;
		dout(30, " rule %d is at %p\n", i, r);
		r->len = yes;
		ceph_decode_copy_safe(p, end, &r->mask, 4, bad); /* 4 u8's */
		ceph_decode_need(p, end, r->len*3*sizeof(u32), bad);
		for (j = 0; j < r->len; j++) {
			ceph_decode_32(p, r->steps[j].op);
			ceph_decode_32(p, r->steps[j].arg1);
			ceph_decode_32(p, r->steps[j].arg2);
		}
	}

	/* ignore trailing name maps. */

	dout(30, "crush_decode success\n");
	return c;

badmem:
	err = -ENOMEM;
bad:
	dout(30, "crush_decode fail %d\n", err);
	crush_destroy(c);
	return ERR_PTR(err);
}


/*
 * osd map
 */
void osdmap_destroy(struct ceph_osdmap *map)
{
	dout(10, "osdmap_destroy %p\n", map);
	if (map->crush)
		crush_destroy(map->crush);
	kfree(map->osd_state);
	kfree(map->osd_weight);
	kfree(map->osd_addr);
	kfree(map);
}

/*
 * adjust max osd value.  reallocate arrays.
 */
static int osdmap_set_max_osd(struct ceph_osdmap *map, int max)
{
	u8 *state;
	struct ceph_entity_addr *addr;
	u32 *weight;

	state = kzalloc(max * sizeof(*state), GFP_NOFS);
	addr = kzalloc(max * sizeof(*addr), GFP_NOFS);
	weight = kzalloc(max * sizeof(*weight), GFP_NOFS);
	if (state == NULL || addr == NULL || weight == NULL) {
		kfree(state);
		kfree(addr);
		kfree(weight);
		return -ENOMEM;
	}

	/* copy old? */
	if (map->osd_state) {
		memcpy(state, map->osd_state, map->max_osd*sizeof(*state));
		memcpy(addr, map->osd_addr, map->max_osd*sizeof(*addr));
		memcpy(weight, map->osd_weight, map->max_osd*sizeof(*weight));
		kfree(map->osd_state);
		kfree(map->osd_addr);
		kfree(map->osd_weight);
	}

	map->osd_state = state;
	map->osd_weight = weight;
	map->osd_addr = addr;
	map->max_osd = max;
	return 0;
}

/*
 * decode a full map.
 */
struct ceph_osdmap *osdmap_decode(void **p, void *end)
{
	struct ceph_osdmap *map;
	u32 len, max, i;
	int err = -EINVAL;
	void *start = *p;
	__le64 major, minor;

	dout(30, "osdmap_decode %p to %p len %d\n", *p, end, (int)(end - *p));

	map = kzalloc(sizeof(*map), GFP_NOFS);
	if (map == NULL)
		return ERR_PTR(-ENOMEM);

	ceph_decode_need(p, end, 2*sizeof(u64)+11*sizeof(u32), bad);
	ceph_decode_64_le(p, major);
	__ceph_fsid_set_major(&map->fsid, major);
	ceph_decode_64_le(p, minor);
	__ceph_fsid_set_minor(&map->fsid, minor);
	ceph_decode_32(p, map->epoch);
	ceph_decode_32_le(p, map->ctime.tv_sec);
	ceph_decode_32_le(p, map->ctime.tv_nsec);
	ceph_decode_32_le(p, map->mtime.tv_sec);
	ceph_decode_32_le(p, map->mtime.tv_nsec);
	ceph_decode_32(p, map->pg_num);
	ceph_decode_32(p, map->pgp_num);
	ceph_decode_32(p, map->lpg_num);
	ceph_decode_32(p, map->lpgp_num);
	ceph_decode_32(p, map->last_pg_change);
	ceph_decode_32(p, map->flags);

	calc_pg_masks(map);

	ceph_decode_32(p, max);

	/* (re)alloc osd arrays */
	err = osdmap_set_max_osd(map, max);
	if (err < 0)
		goto bad;
	dout(30, "osdmap_decode max_osd = %d\n", map->max_osd);

	/* osds */
	err = -EINVAL;
	ceph_decode_need(p, end, 3*sizeof(u32) +
			 map->max_osd*(1 + sizeof(*map->osd_weight) +
				       sizeof(*map->osd_addr)), bad);
	*p += 4; /* skip length field (should match max) */
	ceph_decode_copy(p, map->osd_state, map->max_osd);

	*p += 4; /* skip length field (should match max) */
	for (i = 0; i < map->max_osd; i++)
		ceph_decode_32(p, map->osd_weight[i]);

	*p += 4; /* skip length field (should match max) */
	ceph_decode_copy(p, map->osd_addr, map->max_osd*sizeof(*map->osd_addr));

	/* crush */
	ceph_decode_32_safe(p, end, len, bad);
	dout(30, "osdmap_decode crush len %d from off 0x%x\n", len,
	     (int)(*p - start));
	ceph_decode_need(p, end, len, bad);
	map->crush = crush_decode(*p, end);
	*p += len;
	if (IS_ERR(map->crush)) {
		err = PTR_ERR(map->crush);
		map->crush = NULL;
		goto bad;
	}

	/* ignore the rest of the map */
	*p = end;

	dout(30, "osdmap_decode done %p %p\n", *p, end);
	return map;

bad:
	dout(30, "osdmap_decode fail\n");
	osdmap_destroy(map);
	return ERR_PTR(err);
}

/*
 * decode and apply an incremental map update.
 */
struct ceph_osdmap *apply_incremental(void **p, void *end,
				      struct ceph_osdmap *map,
				      struct ceph_messenger *msgr)
{
	struct ceph_osdmap *newmap = map;
	struct crush_map *newcrush = NULL;
	ceph_fsid_t fsid;
	u32 epoch = 0;
	struct ceph_timespec ctime;
	u32 len, x;
	__s32 new_flags, max;
	void *start = *p;
	int err = -EINVAL;
	__le64 major, minor;

	ceph_decode_need(p, end, sizeof(fsid)+sizeof(ctime)+2*sizeof(u32),
			 bad);
	ceph_decode_64_le(p, major);
	__ceph_fsid_set_major(&fsid, major);
	ceph_decode_64_le(p, minor);
	__ceph_fsid_set_minor(&fsid, minor);
	ceph_decode_32(p, epoch);
	BUG_ON(epoch != map->epoch+1);
	ceph_decode_32_le(p, ctime.tv_sec);
	ceph_decode_32_le(p, ctime.tv_nsec);
	ceph_decode_32(p, new_flags);

	/* full map? */
	ceph_decode_32_safe(p, end, len, bad);
	if (len > 0) {
		dout(20, "apply_incremental full map len %d, %p to %p\n",
		     len, *p, end);
		newmap = osdmap_decode(p, min(*p+len, end));
		return newmap;  /* error or not */
	}

	/* new crush? */
	ceph_decode_32_safe(p, end, len, bad);
	if (len > 0) {
		dout(20, "apply_incremental new crush map len %d, %p to %p\n",
		     len, *p, end);
		newcrush = crush_decode(*p, min(*p+len, end));
		if (IS_ERR(newcrush))
			return ERR_PTR(PTR_ERR(newcrush));
	}

	/* new flags? */
	if (new_flags >= 0)
		map->flags = new_flags;

	ceph_decode_need(p, end, 5*sizeof(u32), bad);

	/* new max? */
	ceph_decode_32(p, max);
	if (max >= 0) {
		err = osdmap_set_max_osd(map, max);
		if (err < 0)
			goto bad;
	}
	ceph_decode_32(p, x);
	if (x)
		map->pg_num = x;
	ceph_decode_32(p, x);
	if (x)
		map->pgp_num = x;
	ceph_decode_32(p, x);
	if (x)
		map->lpg_num = x;
	ceph_decode_32(p, x);
	if (x)
		map->lpgp_num = x;

	map->epoch++;
	map->ctime = map->ctime;
	if (newcrush) {
		if (map->crush)
			crush_destroy(map->crush);
		map->crush = newcrush;
		newcrush = NULL;
	}

	/* new_up */
	err = -EINVAL;
	ceph_decode_32_safe(p, end, len, bad);
	while (len--) {
		u32 osd;
		struct ceph_entity_addr addr;
		ceph_decode_32_safe(p, end, osd, bad);
		ceph_decode_copy_safe(p, end, &addr, sizeof(addr), bad);
		dout(1, "osd%d up\n", osd);
		BUG_ON(osd >= map->max_osd);
		map->osd_state[osd] |= CEPH_OSD_UP;
		map->osd_addr[osd] = addr;
	}

	/* new_down */
	ceph_decode_32_safe(p, end, len, bad);
	while (len--) {
		u32 osd;
		ceph_decode_32_safe(p, end, osd, bad);
		(*p)++;  /* clean flag */
		dout(1, "osd%d down\n", osd);
		if (osd < map->max_osd) {
			map->osd_state[osd] &= ~CEPH_OSD_UP;
			ceph_messenger_mark_down(msgr, &map->osd_addr[osd]);
		}
	}

	/* new_weight */
	ceph_decode_32_safe(p, end, len, bad);
	while (len--) {
		u32 osd, off;
		ceph_decode_need(p, end, sizeof(u32)*2, bad);
		ceph_decode_32(p, osd);
		ceph_decode_32(p, off);
		dout(1, "osd%d weight 0x%x %s\n", osd, off,
		     off == CEPH_OSD_IN ? "(in)" :
		     (off == CEPH_OSD_OUT ? "(out)" : ""));
		if (osd < map->max_osd)
			map->osd_weight[osd] = off;
	}

	/* ignore the rest */
	*p = end;
	return map;

bad:
	derr(10, "corrupt incremental osdmap epoch %d off %d (%p of %p-%p)\n",
	     epoch, (int)(*p - start), *p, start, end);
	if (newcrush)
		crush_destroy(newcrush);
	return ERR_PTR(err);
}




/*
 * calculate file layout from given offset, length.
 * fill in correct oid, logical length, and object extent
 * offset, length.
 *
 * for now, we write only a single su, until we can
 * pass a stride back to the caller.
 */
void calc_file_object_mapping(struct ceph_file_layout *layout,
			      u64 off, u64 *plen,
			      struct ceph_object *oid,
			      u64 *oxoff, u64 *oxlen)
{
	u32 osize = le32_to_cpu(layout->fl_object_size);
	u32 su = le32_to_cpu(layout->fl_stripe_unit);
	u32 sc = le32_to_cpu(layout->fl_stripe_count);
	u32 bl, stripeno, stripepos, objsetno;
	u32 su_per_object;
	u64 t;

	dout(80, "mapping %llu~%llu  osize %u fl_su %u\n", off, *plen,
	     osize, su);
	su_per_object = osize / le32_to_cpu(layout->fl_stripe_unit);
	dout(80, "osize %u / su %u = su_per_object %u\n", osize, su,
	     su_per_object);

	BUG_ON((su & ~PAGE_MASK) != 0);
	/* bl = *off / su; */
	t = off;
	do_div(t, su);
	bl = t;
	dout(80, "off %llu / su %u = bl %u\n", off, su, bl);

	stripeno = bl / sc;
	stripepos = bl % sc;
	objsetno = stripeno / su_per_object;

	oid->bno = cpu_to_le32(objsetno * sc + stripepos);
	dout(80, "objset %u * sc %u = bno %u\n", objsetno, sc, oid->bno);
	/* *oxoff = *off / layout->fl_stripe_unit; */
	t = off;
	*oxoff = do_div(t, su);
	*oxlen = min_t(u64, *plen, su - *oxoff);
	*plen = *oxlen;

	dout(80, " obj extent %llu~%llu\n", *oxoff, *oxlen);
}

/*
 * calculate an object layout (i.e. pgid) from an oid,
 * file_layout, and osdmap
 */
void calc_object_layout(struct ceph_object_layout *ol,
			struct ceph_object *oid,
			struct ceph_file_layout *fl,
			struct ceph_osdmap *osdmap)
{
	unsigned num, num_mask;
	union ceph_pg pgid;
	u64 ino = le64_to_cpu(oid->ino);
	unsigned bno = le32_to_cpu(oid->bno);
	s32 preferred = (s32)le32_to_cpu(fl->fl_pg_preferred);

	if (preferred >= 0) {
		num = osdmap->lpg_num;
		num_mask = osdmap->lpg_num_mask;
	} else {
		num = osdmap->pg_num;
		num_mask = osdmap->pg_num_mask;
	}

	pgid.pg64 = 0;   /* start with it zeroed out */
	pgid.pg.ps = bno + crush_hash32_2(ino, ino>>32);
	pgid.pg.preferred = preferred;
	pgid.pg.type = fl->fl_pg_type;
	pgid.pg.size = fl->fl_pg_size;
	pgid.pg.pool = fl->fl_pg_pool;

	ol->ol_pgid = cpu_to_le64(pgid.pg64);
	ol->ol_stripe_unit = fl->fl_object_stripe_unit;
}
