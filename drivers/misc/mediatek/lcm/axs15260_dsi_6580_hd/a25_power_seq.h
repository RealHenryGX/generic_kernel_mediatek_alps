/* SPDX-License-Identifier: GPL-2.0 */
#ifndef A25_POWER_SEQ_H
#define A25_POWER_SEQ_H
/* Pure sequencing core; production and host fault tests compile this file. */
enum a25_panel_state { A25_UNKNOWN, A25_OFF, A25_ON, A25_FAULT };
struct a25_seq_ops {
	int (*enp)(int high);
	int (*write)(unsigned char reg, unsigned char value);
	int (*touch_reset)(void);
	void (*panel_reset)(unsigned int high);
	void (*delay_ms)(unsigned int ms);
	void (*table)(int suspend);
};
static int a25_seq_off(enum a25_panel_state *state,
		       const struct a25_seq_ops *ops)
{
	int ret = ops->enp(0);

	ops->delay_ms(20);
	*state = ret ? A25_FAULT : A25_OFF;
	return ret;
}
static int a25_seq_init(enum a25_panel_state *state,
			const struct a25_seq_ops *ops)
{
	int ret;

	/* UNKNOWN also covers a bootloader-lit panel. Do not invent rail values. */
	if (*state == A25_FAULT)
		return -EIO; /* Require an explicit successful OFF before retry. */
	ret = ops->enp(1);
	if (ret)
		goto fail;
	ops->delay_ms(20);
	ret = ops->write(0, 0x12);
	if (ret)
		goto fail;
	ret = ops->write(1, 0x12);
	if (ret)
		goto fail;
	ret = ops->touch_reset();
	if (ret)
		goto fail;
	ops->panel_reset(1);
	ops->delay_ms(20);
	ops->panel_reset(0);
	ops->delay_ms(20);
	ops->panel_reset(1);
	ops->delay_ms(100);
	ops->table(0);
	*state = A25_ON;
	return 0;
fail:
	/* Best effort rollback; a failed OFF is latched, never called safe. */
	a25_seq_off(state, ops);
	return ret;
}
static int a25_seq_suspend(enum a25_panel_state *state,
			   const struct a25_seq_ops *ops)
{
	if (*state == A25_OFF)
		return 0;
	if (*state != A25_FAULT) {
		ops->table(1);
		ops->delay_ms(120);
	}
	return a25_seq_off(state, ops);
}
#endif
