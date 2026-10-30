/* SPDX-License-Identifier: GPL-2.0 */
/* Included only for CONFIG_MTK_MIR3DA_A25_VIRTUAL_PS.
 * Sole compensated producer: work scheduled by hrtimer; getters cache-only.
 * All mutable fields and I2C are under mir_lock. Never cancel_sync with it held.
 */
#define MIR_DEFAULT_NS 20000000ULL
#define MIR_MIN_NS 5000000ULL
#define MIR_MAX_NS 2000000000ULL

static u64 mir_period(bool acc, bool ps, u64 an, u64 pn)
{
	if (!acc)
		return ps ? pn : MIR_DEFAULT_NS;
	return ps && pn < an ? pn : an;
}
static enum hrtimer_restart mir_sample_timer(struct hrtimer *timer)
{
	struct mir3da_state *s = container_of(timer, struct mir3da_state,
		sample_timer);

	/* IRQ context: queue only, no bus or sensor event operation. */
	schedule_work(&s->sample_work);
	return HRTIMER_NORESTART;
}
static void mir_arm_locked(u64 delay)
{
	hrtimer_start(&mir->sample_timer, ns_to_ktime(delay), HRTIMER_MODE_REL);
}
static int mir_hw_config_locked(bool acc, bool ps, u64 an, u64 pn)
{
	int ret;

	if (acc || ps) {
		ret = mir3da_set_odr(mir->client,
			div_u64(mir_period(acc, ps, an, pn), 1000000));
		if (ret)
			return ret;
	}
	return mir3da_set_enable(mir->client, acc || ps);
}
static int mir_config_locked(bool acc, bool ps, u64 an, u64 pn)
{
	u64 period;
	int ret = mir_available();

	if (ret)
		return ret;
	ret = mir_hw_config_locked(acc, ps, an, pn);
	if (ret) {
		/* A failed bus write may have reached the chip. Restore old desired
		 * state; if restoration fails, retry without processing samples.
		 */
		mir->recovering = !!mir_hw_config_locked(mir->acc_on, mir->ps_on,
			mir->acc_ns, mir->ps_ns);
		if (mir->recovering)
			mir_arm_locked(MIR_DEFAULT_NS);
		return ret;
	}
	if (ps && !mir->ps_on)
		__pm_stay_awake(mir->ps_wake);
	else if (!ps && mir->ps_on)
		__pm_relax(mir->ps_wake);
	/* ACC report phase is owned by the HAL data request, not factory. */
	mir->acc_on = acc;
	mir->ps_on = ps;
	mir->acc_ns = an;
	mir->ps_ns = pn;
	mir->enabled = acc || ps;
	mir->recovering = false;
	period = mir_period(acc, ps, an, pn);
	if (mir->enabled) {
		/* Reconfiguration may accelerate, never postpone an earlier sample. */
		u64 now = ktime_get_boot_ns();
		u64 due = now + period;

		if (!mir->sample_due || due < mir->sample_due)
			mir->sample_due = due;
		mir_arm_locked(mir->sample_due > now ? mir->sample_due - now : 1);
	} else {
		hrtimer_cancel(&mir->sample_timer);
		mir->sample_due = 0;
		mir->cache_valid = false;
	}
	return 0;
}
/* HAL data uses batch/set_delay. Nodata has no separate rate ABI and
 * retains its 20ms default; factory owns its own requested period. */
static u64 mir_acc_period(bool data, bool nodata, bool factory,
	u64 data_ns, u64 nodata_ns, u64 factory_ns)
{
	u64 ns = data ? data_ns : MIR_MAX_NS;

	if (nodata && nodata_ns < ns)
		ns = nodata_ns;
	if (factory && factory_ns < ns)
		ns = factory_ns;
	return data || nodata || factory ? ns : data_ns;
}
static int mir_acc_config_locked(bool data, bool nodata, bool factory,
	u64 hal_ns, u64 factory_ns)
{
	int ret = mir_config_locked(data || nodata || factory, mir->ps_on,
		mir_acc_period(data, nodata, factory, hal_ns,
			mir->nodata_ns, factory_ns),
		mir->ps_ns);

	if (!ret) {
		if (data && (!mir->acc_data_on || hal_ns != mir->hal_ns))
			mir->acc_due = 0;
		mir->acc_data_on = data;
		mir->acc_nodata_on = nodata;
		mir->factory_on = factory;
		mir->hal_ns = hal_ns;
		mir->factory_ns = factory_ns;
	}
	return ret;
}
static int mir_acc_enable(bool data, int en)
{
	int ret;

	if (en != 0 && en != 1)
		return -EINVAL;
	mutex_lock(&mir_lock);
	ret = mir_available();
	if (!ret)
		ret = mir_acc_config_locked(data ? en : mir->acc_data_on,
			data ? mir->acc_nodata_on : en, mir->factory_on,
			mir->hal_ns, mir->factory_ns);
	mutex_unlock(&mir_lock);
	return ret;
}
static int mir_acc_open_report_data(int en)
{
	return mir_acc_enable(true, en);
}
static int mir_runtime_enable(bool ps, int en)
{
	int ret;

	if (!ps)
		return mir_acc_enable(false, en);
	if (en != 0 && en != 1)
		return -EINVAL;
	mutex_lock(&mir_lock);
	ret = mir_available();
	if (!ret)
		ret = mir_config_locked(mir->acc_on, en,
			mir->acc_ns, mir->ps_ns);
	mutex_unlock(&mir_lock);
	return ret;
}
static int mir_runtime_delay(bool ps, u64 ns)
{
	int ret;

	if (!ns)
		return -EINVAL;
	if (ns < MIR_MIN_NS || ns > MIR_MAX_NS)
		return -ERANGE;
	mutex_lock(&mir_lock);
	ret = mir_available();
	if (!ret) {
		if (ps)
			ret = mir_config_locked(mir->acc_on, mir->ps_on,
				mir->acc_ns, ns);
		else
			ret = mir_acc_config_locked(mir->acc_data_on,
				mir->acc_nodata_on, mir->factory_on,
				ns, mir->factory_ns);
	}
	mutex_unlock(&mir_lock);
	return ret;
}
static int mir_factory_enable_locked(bool enable, s64 ms)
{
	if (enable && (ms < 5 || ms > 2000))
		return -ERANGE;
	return mir_acc_config_locked(mir->acc_data_on, mir->acc_nodata_on,
		enable, mir->hal_ns, enable ? ms * 1000000ULL : mir->factory_ns);
}
static int mir_ps_enable(int en)
{
	return mir_runtime_enable(true, en);
}
static int mir_ps_batch(int flag, s64 ns, s64 latency)
{
	if (flag || latency)
		return -EOPNOTSUPP;
	return ns <= 0 ? -EINVAL : mir_runtime_delay(true, ns);
}
static int mir_runtime_flush_locked(bool ps)
{
	int *error = ps ? &mir->ps_error : &mir->acc_error;
	int ret;

	/* A dropped sample must not become a successful flush. Report and
	 * acknowledge the asynchronous error once; user may retry flush.
	 */
	if (*error) {
		ret = *error;
		*error = 0;
		return ret;
	}
	return ps ? a25_ps_event(0, ktime_get_boot_ns(), true) :
		acc_flush_report();
}
static int mir_ps_flush(void)
{
	int ret;

	mutex_lock(&mir_lock);
	ret = mir_available();
	if (!ret)
		ret = mir_runtime_flush_locked(true);
	mutex_unlock(&mir_lock);
	return ret;
}
static const struct a25_ps_ops mir_ps_ops = {
	.enable = mir_ps_enable, .batch = mir_ps_batch,
	.flush = mir_ps_flush, .cali = NULL,
};
static void mir_sample_work(struct work_struct *work)
{
	short raw[3];
	int counts[3], ret, i, ps;
	u64 now, period;
	struct acc_data data = {0};

	mutex_lock(&mir_lock);
	if (mir_available() || &mir->sample_work != work)
		goto out;
	if (mir->recovering) {
		ret = mir_hw_config_locked(mir->acc_on, mir->ps_on,
			mir->acc_ns, mir->ps_ns);
		if (ret) {
			mir_arm_locked(MIR_DEFAULT_NS);
			goto out;
		}
		mir->recovering = false;
	}
	if (!mir->enabled)
		goto out;
	now = ktime_get_boot_ns();
	if (now < mir->sample_due) {
		mir_arm_locked(mir->sample_due - now);
		goto out;
	}
	period = mir_period(mir->acc_on, mir->ps_on, mir->acc_ns, mir->ps_ns);
	ret = mir3da_read_raw_data(mir->client, &raw[0], &raw[1], &raw[2]);
	if (ret) {
		mir->cache_valid = false;
		if (mir->acc_data_on)
			mir->acc_error = ret;
		if (mir->ps_on)
			mir->ps_error = ret;
		goto rearm;
	}
	/* One successful read -> one 30-history update and one PS step.
	 * Calibration is explicitly unsupported: cali_sw remains zero.
	 */
	mir3da_stock_compensate(raw);
	for (i = 0; i < 3; i++)
		counts[mir->cvt.map[i]] = mir->cvt.sign[i] * (int)raw[i];
	for (i = 0; i < 3; i++)
		mir->cache[i] = counts[i] * GRAVITY_EARTH_1000 / 1024;
	mir->cache_valid = true;
	mir->sequence++;
	ps = stock_ps_step(&mir->ps_state, mir->ps_on,
		counts[0], counts[1], counts[2]);
	if (ps >= 0) {
		ret = a25_ps_event(ps, now, false);
		if (ret)
			mir->ps_error = ret;
	}
	if (mir->acc_data_on && now >= mir->acc_due) {
		data.x = mir->cache[0]; data.y = mir->cache[1];
		data.z = mir->cache[2]; data.timestamp = now;
		data.status = SENSOR_STATUS_UNRELIABLE;
		ret = acc_data_report(&data);
		if (ret)
			mir->acc_error = ret;
		/* Keep phase for non-multiple consumer rates; never synthesize
		 * missed events after a stall. Jitter is bounded by shared period.
		 */
		if (!mir->acc_due)
			mir->acc_due = now;
		mir->acc_due += (div64_u64(now - mir->acc_due, mir->hal_ns) + 1) *
			mir->hal_ns;
	}
rearm:
	/* No catch-up replay of the same physical sample after a stall. */
	mir->sample_due = ktime_get_boot_ns() + period;
	mir_arm_locked(period);
out:
	mutex_unlock(&mir_lock);
}
