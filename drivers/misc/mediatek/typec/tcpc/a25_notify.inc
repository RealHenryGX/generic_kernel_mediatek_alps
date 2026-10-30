/* SPDX-License-Identifier: GPL-2.0-only */
/* Included by tcpci_core.c. A25-only subscriptions and publication lifecycle.
 * No hardware access. Ordered, per-subscriber delivery: a late subscriber
 * cannot consume pre-registration backlog; replay precedes subsequent events.
 * No callback on this controller's evt_wq may unregister ANY subscription on
 * this controller or destroy it: both operations flush that same queue.
 * The owner serializes unregister/re-register/free of an nb, and controller
 * destruction against all subscription operations. Not a hot-unplug API.
 */
struct a25_subscription;
struct a25_notifications;
struct a25_delivery {
 struct list_head node;
 struct work_struct work;
 struct a25_subscription *sub;
 struct tcp_notify data;
 unsigned long event;
 bool reserved;
};
struct a25_subscription {
 struct list_head node;
 struct notifier_block *nb;
 struct a25_notifications *owner;
 uint8_t flags;
 /* Registered consumers always own one allocation-free safety detach. */
 struct a25_delivery detach;
 bool detach_queued;
};
struct a25_notifications {
 struct mutex lock;
 struct list_head subscribers;
 struct tcp_notify snapshot;
 unsigned int detach_pending;
 bool valid;
 bool closing;
};
static void a25_deliver(struct work_struct *work)
{
 struct a25_delivery *d = container_of(work, struct a25_delivery, work);
 struct a25_subscription *s = d->sub;
 struct tcp_notify data;
 if (!d->reserved) {
  s->nb->notifier_call(s->nb, d->event, &d->data);
  kfree(d);
  return;
 }
 /* Release the reserve before invoking the consumer. A later publication may
  * requeue this running work, but this ordered queue cannot run it twice at
  * once. Its callback data must therefore be a local, immutable copy. */
 mutex_lock(&s->owner->lock);
 data = d->data;
 s->detach_queued = false;
 s->owner->detach_pending--;
 mutex_unlock(&s->owner->lock);
 s->nb->notifier_call(s->nb, TCP_NOTIFY_TYPEC_STATE, &data);
}
static struct a25_delivery *a25_delivery_alloc(struct a25_subscription *sub,
 const struct tcp_notify *data, unsigned long event)
{
 struct a25_delivery *d = kzalloc(sizeof(*d), GFP_KERNEL);
 if (!d) return NULL;
 INIT_WORK(&d->work, a25_deliver);
 d->sub = sub; d->data = *data; d->event = event;
 return d;
}
int tcpc_a25_notify(struct tcpc_device *tcpc, const struct tcp_notify *data,
 uint8_t type, uint8_t event)
{
 struct a25_notifications *n = tcpc->a25_notifications;
 struct a25_subscription *s;
 struct a25_delivery *d, *tmp;
 LIST_HEAD(pending);
 bool state = type == TCP_NOTIFY_IDX_USB && event == TCP_NOTIFY_TYPEC_STATE;
 bool detach = state && data->typec_state.new_state == TYPEC_UNATTACHED;
 int ret = 0;
 if (!n || type >= TCP_NOTIFY_IDX_NR) return -EINVAL;
 mutex_lock(&n->lock);
 if (n->closing) { ret = -ESHUTDOWN; goto out; }
 /* Do not accept an attach between two coalesced reserved detaches. The
  * normal producer can retry after the software-only queue drains; neither
  * the PM-failure detach nor its delivery needs another bus transaction. */
 if (state && !detach && n->detach_pending) { ret = -EAGAIN; goto out; }
 if (state && data->typec_state.old_state == data->typec_state.new_state) {
  /* Snapshot freshness and register/replay share the same ordering lock.
   * Do not invent an edge for existing subscribers. */
  n->snapshot = *data;
  n->valid = true;
  goto out;
 }
 if (detach && n->detach_pending) goto safety_detach;
 list_for_each_entry(s, &n->subscribers, node) {
  if (!(s->flags & (1U << type))) continue;
  d = a25_delivery_alloc(s, data, event);
  if (!d) { ret = -ENOMEM; goto free_pending; }
  list_add_tail(&d->node, &pending);
 }
 if (state) {
  n->snapshot = *data;
  n->valid = true;
 }
 list_for_each_entry_safe(d, tmp, &pending, node) {
  list_del(&d->node);
  queue_work(tcpc->evt_wq, &d->work);
 }
 goto out;
free_pending:
 /* No prefix was submitted, so falling back cannot duplicate that prefix. */
 list_for_each_entry_safe(d, tmp, &pending, node) {
  list_del(&d->node); kfree(d);
 }
 if (!detach) goto out;
safety_detach:
 /* Commit actual safe state AND guaranteed delivery ownership atomically.
  * Continuous OOM cannot keep an old attach replayable. There is no timer,
  * retry allocation, I2C, or busy loop in this path. */
 n->snapshot = *data;
 n->valid = true;
 list_for_each_entry(s, &n->subscribers, node) {
  if (!(s->flags & TCP_NOTIFY_TYPE_USB)) continue;
  s->detach.data = *data;
  if (s->detach_queued) continue;
  s->detach_queued = true;
  n->detach_pending++;
  queue_work(tcpc->evt_wq, &s->detach.work);
 }
 ret = 0;
out:
 mutex_unlock(&n->lock);
 return ret;
}
static int a25_register_notifier(struct tcpc_device *tcpc,
 struct notifier_block *nb, uint8_t flags)
{
 struct a25_notifications *n = tcpc->a25_notifications;
 struct a25_subscription *s, *it;
 struct a25_delivery *d = NULL;
 struct tcp_notify replay;
 int ret = 0;
 if (!nb || !nb->notifier_call || !flags ||
     (flags & ~((1U << TCP_NOTIFY_IDX_NR) - 1))) return -EINVAL;
 s = kzalloc(sizeof(*s), GFP_KERNEL);
 if (!s) return -ENOMEM;
 s->nb = nb; s->flags = flags; s->owner = n;
 INIT_WORK(&s->detach.work, a25_deliver);
 s->detach.sub = s; s->detach.reserved = true;
 mutex_lock(&n->lock);
 if (n->closing) { ret = -ESHUTDOWN; goto out; }
 list_for_each_entry(it, &n->subscribers, node)
  if (it->nb == nb) { ret = -EEXIST; goto out; }
 if (n->valid && (flags & TCP_NOTIFY_TYPE_USB)) {
  replay = n->snapshot;
  /* Existing extcon/charger consumers key on a transition, not SNK->SNK.
   * Only this new subscriber receives the synthetic initial transition. */
  replay.typec_state.old_state = replay.typec_state.new_state ==
   TYPEC_UNATTACHED ? TYPEC_ATTACHED_SNK : TYPEC_UNATTACHED;
  d = a25_delivery_alloc(s, &replay, TCP_NOTIFY_TYPEC_STATE);
  if (!d) { ret = -ENOMEM; goto out; }
 }
 list_add_tail(&s->node, &n->subscribers);
 if (d) queue_work(tcpc->evt_wq, &d->work);
out:
 mutex_unlock(&n->lock);
 if (ret) kfree(s);
 return ret;
}
static int a25_unregister_notifier(struct tcpc_device *tcpc,
 struct notifier_block *nb, uint8_t flags)
{
 struct a25_notifications *n = tcpc->a25_notifications;
 struct a25_subscription *s, *found = NULL;
 mutex_lock(&n->lock);
 list_for_each_entry(s, &n->subscribers, node) {
  if (s->nb != nb) continue;
  /* Reject partial unregister rather than silently discarding other types. */
  if (s->flags != flags) { mutex_unlock(&n->lock); return -EINVAL; }
  list_del(&s->node); found = s; break;
 }
 mutex_unlock(&n->lock);
 if (!found) return -ENOENT;
 flush_workqueue(tcpc->evt_wq);
 kfree(found);
 return 0;
}
static void a25_device_release(struct device *dev)
{
 struct tcpc_device *tcpc = to_tcpc_device(dev);
 kfree(tcpc->a25_notifications);
 kfree(tcpc);
}
static struct tcpc_device *a25_device_register(struct device *parent,
 struct tcpc_desc *desc, struct tcpc_ops *ops, void *data)
{
 struct tcpc_device *tcpc;
 struct a25_notifications *n;
 int ret;
 tcpc = kzalloc(sizeof(*tcpc), GFP_KERNEL);
 if (!tcpc) return ERR_PTR(-ENOMEM);
 n = kzalloc(sizeof(*n), GFP_KERNEL);
 if (!n) { kfree(tcpc); return ERR_PTR(-ENOMEM); }
 tcpc->a25_notifications = n;
 mutex_init(&n->lock); INIT_LIST_HEAD(&n->subscribers);
 mutex_init(&tcpc->access_lock); mutex_init(&tcpc->typec_lock);
 mutex_init(&tcpc->timer_lock); mutex_init(&tcpc->mr_lock);
 tcpc->ops = ops; tcpc->drv_data = data; tcpc->desc = *desc;
 tcpc->tcpc_flags = TCPC_FLAGS_A25_SINK_ONLY;
 tcpc->typec_role = tcpc->typec_role_new = TYPEC_ROLE_SNK;
 tcpc->typec_local_rp_level = desc->rp_lvl;
 /* No generic timer/PD state-machine resources are initialized. */
 device_initialize(&tcpc->dev);
 tcpc->dev.class = tcpc_class;
 tcpc->dev.type = &tcpc_dev_type;
 tcpc->dev.parent = parent;
 tcpc->dev.release = a25_device_release;
 dev_set_drvdata(&tcpc->dev, tcpc);
 device_set_of_node_from_dev(&tcpc->dev, parent);
 ret = dev_set_name(&tcpc->dev, "%s", desc->name);
 if (ret) goto put;
 tcpc->evt_wq = alloc_ordered_workqueue("%s", 0, desc->name);
 if (!tcpc->evt_wq) { ret = -ENOMEM; goto put; }
 /* Sink-only notification path does not use generic attach/detach wakelocks. */
 ret = device_add(&tcpc->dev);
 if (ret) { destroy_workqueue(tcpc->evt_wq); goto put; }
 WRITE_ONCE(tcpc->a25_live, true);
 return tcpc;
put:
 put_device(&tcpc->dev);
 return ERR_PTR(ret);
}
static void a25_device_unregister(struct tcpc_device *tcpc)
{
 struct a25_notifications *n = tcpc->a25_notifications;
 struct a25_subscription *s, *tmp;
 /* Caller must have quiesced the driver producer. General hot-unplug remains
  * unsupported: existing consumers do not own a balanced tcpc reference. */
 WRITE_ONCE(tcpc->a25_live, false);
 mutex_lock(&n->lock); n->closing = true; mutex_unlock(&n->lock);
 device_del(&tcpc->dev);
 destroy_workqueue(tcpc->evt_wq);
 list_for_each_entry_safe(s, tmp, &n->subscribers, node) {
  list_del(&s->node); kfree(s);
 }
 put_device(&tcpc->dev);
}
