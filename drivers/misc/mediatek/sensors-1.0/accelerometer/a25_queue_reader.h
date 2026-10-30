/* SPDX-License-Identifier: GPL-2.0 */
#ifndef A25_QUEUE_READER_H
#define A25_QUEUE_READER_H
/* One transaction lock per publication, never taken by producers or remove.
 * refs are protected by the endpoint's metadata lock (not readers).
 * Owner holds one reference; each open holds another, including blocked read.
 */
struct a25_read_epoch {
	struct mutex readers;
	unsigned int refs;
	u64 generation;
};
static struct a25_read_epoch *a25_read_epoch_new(void)
{
	struct a25_read_epoch *e = kzalloc(sizeof(*e), GFP_KERNEL);

	if (e) {
		mutex_init(&e->readers);
		e->refs = 1;
	}
	return e;
}
static void a25_read_epoch_put(struct a25_read_epoch *e)
{
	if (e && !--e->refs)
		kfree(e);
}
#endif
