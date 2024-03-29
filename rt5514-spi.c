/*
 * rt5514-spi.c  --  RT5514 SPI driver
 *
 * Copyright 2015 Realtek Semiconductor Corp.
 * Author: Oder Chiou <oder_chiou@realtek.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/module.h>
#include <linux/input.h>
#include <linux/spi/spi.h>
#include <linux/device.h>
#include <linux/init.h>
#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/regulator/consumer.h>
#include <linux/pm_wakeirq.h>
#include <linux/sysfs.h>
#include <linux/clk.h>
#include <linux/of_irq.h>
#include <sound/core.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>

#include <linux/wakelock.h>
#include <linux/math64.h>
#include "rt5514.h"
#include "rt5514-spi.h"

#define RECORD_SHIFT  16000
#define KEY_WAKEUP    143   //define KEY_WAKEUP for lower power on linkportable

static atomic_t is_spi_ready = ATOMIC_INIT(0);

static struct wake_lock dsp_lock;
extern int dsp_idle_mode_on;

static struct spi_device *rt5514_spi;
static int gpio_hotword;
static int ns_per_tic,ns_per_sample,tic_per_sample;
static unsigned long ps_tic;

static bool hot_en = 1;
module_param(hot_en, bool, 0664);
MODULE_PARM_DESC(hot_en, "Hotword detection switch");

static unsigned long long timestamp = 0;

struct rt5514_dsp {
	struct device *dev;
	struct delayed_work start_work, copy_work, get_dsp_tic, watchdog_work;
	struct mutex dma_lock;
	struct snd_pcm_substream *substream;
	unsigned int buf_base, buf_limit, buf_rp, time_syncing;
	size_t buf_size, get_size, dma_offset;
	Params_AEC AEC1, AEC2,AEC_hotword;
	u64 ts1, ts2, ts_buf_start, ts_wp_soc;
	struct input_dev *input_dev;
	struct delayed_work wake_work;
};

static inline int cal_copy_delay(struct rt5514_dsp *rt5514_dsp, int remain_data)
{
	if (rt5514_dsp->get_size < rt5514_dsp->buf_size) {
		pr_info_once("speed up man~~%d %d\n", rt5514_dsp->get_size, rt5514_dsp->buf_size);
		return 0;
	} else {
		struct snd_pcm_runtime *runtime = rt5514_dsp->substream->runtime;
		size_t period_bytes = snd_pcm_lib_period_bytes(rt5514_dsp->substream);
		ssize_t need_sample = bytes_to_samples(runtime, ((period_bytes - remain_data)/runtime->channels));
		int sample_per_msec = runtime->rate/1000;
		int need_msec = ((need_sample + sample_per_msec) / sample_per_msec);

		pr_info_ratelimited("slow down man~~ %d - %d, %d, %d ch=%d rt=%d\n",
			period_bytes, remain_data, need_sample, need_msec, runtime->channels, runtime->rate);
		return msecs_to_jiffies(need_msec);
	}
}

int rt5514_spi_read_addr(unsigned int addr, unsigned int *val)
{
	struct spi_device *spi = rt5514_spi;
	struct spi_message message;
	struct spi_transfer x[3];
	u8 spi_cmd = RT5514_SPI_CMD_32_READ;
	int status;
	u8 write_buf[5];
	u8 read_buf[4];

	write_buf[0] = spi_cmd;
	write_buf[1] = (addr & 0xff000000) >> 24;
	write_buf[2] = (addr & 0x00ff0000) >> 16;
	write_buf[3] = (addr & 0x0000ff00) >> 8;
	write_buf[4] = (addr & 0x000000ff) >> 0;

	spi_message_init(&message);
	memset(x, 0, sizeof(x));

	x[0].len = 5;
	x[0].tx_buf = write_buf;
	spi_message_add_tail(&x[0], &message);

	x[1].len = 4;
	x[1].tx_buf = write_buf;
	spi_message_add_tail(&x[1], &message);

	x[2].len = 4;
	x[2].rx_buf = read_buf;
	spi_message_add_tail(&x[2], &message);

	status = spi_sync(spi, &message);

	*val = read_buf[3] | read_buf[2] << 8 | read_buf[1] << 16 |
		read_buf[0] << 24;

	return status;
}

int rt5514_spi_write_addr(unsigned int addr, unsigned int val)
{
	struct spi_device *spi = rt5514_spi;
	u8 spi_cmd = RT5514_SPI_CMD_32_WRITE;
	int status;
	u8 write_buf[10];

	write_buf[0] = spi_cmd;
	write_buf[1] = (addr & 0xff000000) >> 24;
	write_buf[2] = (addr & 0x00ff0000) >> 16;
	write_buf[3] = (addr & 0x0000ff00) >> 8;
	write_buf[4] = (addr & 0x000000ff) >> 0;
	write_buf[5] = (val & 0xff000000) >> 24;
	write_buf[6] = (val & 0x00ff0000) >> 16;
	write_buf[7] = (val & 0x0000ff00) >> 8;
	write_buf[8] = (val & 0x000000ff) >> 0;
	write_buf[9] = spi_cmd;

	status = spi_write(spi, write_buf, sizeof(write_buf));

	if (status)
		dev_err(&spi->dev, "%s error %d\n", __func__, status);

	return status;
}

void rt5514_set_irq_low(void)
{
	rt5514_spi_write_addr(0x18002e04, 0x0);
}

void rt5514_hotdet(bool en)
{
	if (hot_en) {
		pr_info("%s: %s hotword detection\n", __func__, (en?"Enable":"Disable"));

		if (en) {
			/* Set 0x18002fac to 1 to enable Google engine detection */
			rt5514_spi_write_addr(0x18002fac, 0x1);
		} else {
			/* Set 0x18002fac to 0 to disable Google engine detection */
			rt5514_spi_write_addr(0x18002fac, 0x0);
		}
	}
}

static const struct snd_pcm_hardware rt5514_spi_pcm_hardware = {
	.info			= SNDRV_PCM_INFO_MMAP |
				  SNDRV_PCM_INFO_MMAP_VALID |
				  SNDRV_PCM_INFO_INTERLEAVED,
	.formats		= SNDRV_PCM_FMTBIT_S32_LE,
	.period_bytes_min	= PAGE_SIZE,
	.period_bytes_max	= 0x20000 / 8,
	.periods_min		= 8,
	.periods_max		= 8,
	.channels_min		= 2,
	.channels_max		= 2,
	.buffer_bytes_max	= 0x20000,
};

static struct snd_soc_dai_driver rt5514_spi_dai = {
	.name = "rt5514-dsp-cpu-dai",
	.id = 0,
	.capture = {
		.stream_name = "DSP Capture",
		.channels_min = 2,
		.channels_max = 2,
		.rates = SNDRV_PCM_RATE_16000,
		.formats = SNDRV_PCM_FMTBIT_S32_LE,
	},
};

static int timestamp_get(char *val, const struct kernel_param *kp);
static const struct kernel_param_ops timestamp_ops = {
	.set = NULL,
	.get = timestamp_get,
};

module_param_cb(timestamp, &timestamp_ops, &timestamp, S_IRUGO | S_IWUSR);

static int timestamp_get(char *val, const struct kernel_param *kp)
{
	int ret = 0;
	struct rt5514_dsp *rt5514_dsp = (struct rt5514_dsp *)spi_get_drvdata(rt5514_spi);

	return param_get_ullong(val, kp);
}

static int raw_counter_read_handler(char *buf, const struct kernel_param *kp)
{
	/*
	 * 1. Read monotonic timestamp
	 * 2. Read 0x4ff60000
	 * 3. Read monotonic timestamp
	 */

	unsigned int cnt = 0;
	u64 t1 = 0;
	u64 t2 = 0;
	u64 t_dsp = 0;
	u64 cur_tick = 0;
	Params_AEC aec;
	u8 buf_sche_copy[8] = {0x00, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00};

	/* don’t trigger IRQ to host */
	rt5514_spi_write_addr(0x18001028, 0x0);

	/* Record current system time */
	t1 = ktime_get_ns();

	/* Ask DSP to store the current tick to Params_AEC */
	rt5514_spi_burst_write(0x18001014, buf_sche_copy, 8);

	/* Record current system time */
	t2 = ktime_get_ns();

	/* enable irq*/
	rt5514_spi_write_addr(0x18001028, 0x1);

	/* Fetch the Params_AEC to get the counter */
	rt5514_spi_burst_read(0x4ff60000, (u8 *)&aec, sizeof(Params_AEC));

	/* current tick = high 32bit + low 32bit*/
	cur_tick = (u64)(((u64)aec.RTC_Cur_Upper << 32) | ((u64)aec.RTC_Current));

	/* Time of DSP (ns) = current tick * ps/tick * ns/ps */
	t_dsp = div_u64((ps_tic * cur_tick), 1000);

	pr_info("up=%lu, low=%lu, --> %llu t_dsp=%llu, ps_tic=%lu\n", aec.RTC_Cur_Upper, aec.RTC_Current, cur_tick, t_dsp, ps_tic);

	memset(buf, 0x00, 128);
	sprintf(buf, "%llu, %llu, %llu", t1, t_dsp, t2);

	return strlen(buf);
}

static const struct kernel_param_ops raw_counter_op_ops = {
	.set = NULL,
	.get = raw_counter_read_handler
};
module_param_cb(raw_counter, &raw_counter_op_ops, NULL, S_IRUGO);
MODULE_PARM_DESC(raw_counter, "monotonic timestamp (us or ns) before I2C transaction, DSP counter content, monotonic timestamp (us or ns) after I2C transaction");

static int rt5514_spi_time_sync(int num,int type)
{
	struct snd_soc_platform *platform =
		snd_soc_lookup_platform(&rt5514_spi->dev);
	struct rt5514_dsp *rt5514_dsp =
		snd_soc_platform_get_drvdata(platform);

	u8 buf_sche_copy[8] = {0x00, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00};

	pr_info("%s -- num:%d,dsp_idle_mode_on:%d\n", __func__,num,dsp_idle_mode_on);
	if(!dsp_idle_mode_on){
		rt5514_dsp->time_syncing = num;
		rt5514_spi_burst_write(0x18001014, buf_sche_copy, 8);
	} else
		pr_info("%s -- Stop time syncing when dsp is in idle mode.\n", __func__);

	return 0;
}

static void rt5514_spi_copy_work(struct work_struct *work)
{
	struct rt5514_dsp *rt5514_dsp =
		container_of(work, struct rt5514_dsp, copy_work.work);
	struct snd_pcm_runtime *runtime;
	size_t period_bytes, truncated_bytes = 0;
	unsigned int cur_wp, remain_data = 0;
	u8 buf[8];

	mutex_lock(&rt5514_dsp->dma_lock);
	if (!rt5514_dsp->substream) {
		dev_err(rt5514_dsp->dev, "No pcm substream\n");
		goto done;
	}

	runtime = rt5514_dsp->substream->runtime;
	period_bytes = snd_pcm_lib_period_bytes(rt5514_dsp->substream);

	if (rt5514_dsp->get_size >= rt5514_dsp->buf_size) {
		rt5514_spi_burst_read(RT5514_BUFFER_VOICE_WP, (u8 *)&buf,
			sizeof(buf));
		cur_wp = buf[0] | buf[1] << 8 | buf[2] << 16 |
					buf[3] << 24;

		if (cur_wp >= rt5514_dsp->buf_rp)
			remain_data = (cur_wp - rt5514_dsp->buf_rp);
		else
			remain_data =
				(rt5514_dsp->buf_limit - rt5514_dsp->buf_rp) +
				(cur_wp - rt5514_dsp->buf_base);

		if (remain_data < period_bytes) {
			schedule_delayed_work(&rt5514_dsp->copy_work,
				cal_copy_delay(rt5514_dsp, remain_data));
			goto done;
		}
	}

	if (rt5514_dsp->buf_rp + period_bytes <= rt5514_dsp->buf_limit) {
		rt5514_spi_burst_read(rt5514_dsp->buf_rp,
			runtime->dma_area + rt5514_dsp->dma_offset,
			period_bytes);

		if (rt5514_dsp->buf_rp + period_bytes == rt5514_dsp->buf_limit)
			rt5514_dsp->buf_rp = rt5514_dsp->buf_base;
		else
			rt5514_dsp->buf_rp += period_bytes;
	} else {
		truncated_bytes = rt5514_dsp->buf_limit - rt5514_dsp->buf_rp;
		rt5514_spi_burst_read(rt5514_dsp->buf_rp,
			runtime->dma_area + rt5514_dsp->dma_offset,
			truncated_bytes);

		rt5514_spi_burst_read(rt5514_dsp->buf_base,
			runtime->dma_area + rt5514_dsp->dma_offset +
			truncated_bytes, period_bytes - truncated_bytes);

		rt5514_dsp->buf_rp = rt5514_dsp->buf_base + period_bytes -
			truncated_bytes;
	}

	rt5514_dsp->get_size += period_bytes;
	rt5514_dsp->dma_offset += period_bytes;
	if (rt5514_dsp->dma_offset >= runtime->dma_bytes)
		rt5514_dsp->dma_offset = 0;

	snd_pcm_period_elapsed(rt5514_dsp->substream);

	schedule_delayed_work(&rt5514_dsp->copy_work,
		cal_copy_delay(rt5514_dsp, period_bytes));

done:
	mutex_unlock(&rt5514_dsp->dma_lock);
}

static void rt5514_schedule_copy(struct rt5514_dsp *rt5514_dsp)
{
	size_t period_bytes;
	u8 buf[8] = {0};
	int buf_diff_sample,truncated_bytes;
	s64 ts_AEC1_wp;

	if (!rt5514_dsp->substream) {
		rt5514_hotdet(1);
		return;
	}

	ts_AEC1_wp = rt5514_dsp->ts_wp_soc;

	period_bytes = snd_pcm_lib_period_bytes(rt5514_dsp->substream);
	rt5514_dsp->get_size = 0;

	/**
	 * The address area x1800XXXX is the register address, and it cannot
	 * support spi burst read perfectly. So we use the spi burst read
	 * individually to make sure the data correctly.
	 */
	rt5514_spi_burst_read(RT5514_BUFFER_VOICE_BASE, (u8 *)&buf,
		sizeof(buf));
	rt5514_dsp->buf_base = buf[0] | buf[1] << 8 | buf[2] << 16 |
				buf[3] << 24;

	rt5514_spi_burst_read(RT5514_BUFFER_VOICE_LIMIT, (u8 *)&buf,
		sizeof(buf));
	rt5514_dsp->buf_limit = buf[0] | buf[1] << 8 | buf[2] << 16 |
				buf[3] << 24;

	rt5514_spi_burst_read(RT5514_BUFFER_VOICE_WP, (u8 *)&buf,
		sizeof(buf));
	rt5514_dsp->buf_rp = buf[0] | buf[1] << 8 | buf[2] << 16 |
				buf[3] << 24;

	/* To avoid rp out of valid memory region */
	if (rt5514_dsp->buf_rp + RECORD_SHIFT <= rt5514_dsp->buf_limit) {
		rt5514_dsp->buf_rp = rt5514_dsp->buf_rp + RECORD_SHIFT;
	} else {
		truncated_bytes = rt5514_dsp->buf_limit - rt5514_dsp->buf_rp;
		rt5514_dsp->buf_rp = rt5514_dsp->buf_base + (RECORD_SHIFT-truncated_bytes);
	}
	/* To avoid rp out of valid memory region */

	if (rt5514_dsp->buf_rp % 8)
		rt5514_dsp->buf_rp = (rt5514_dsp->buf_rp / 8) * 8;

	rt5514_dsp->buf_size = rt5514_dsp->buf_limit - rt5514_dsp->buf_base;

	if (likely(tic_per_sample)) {
		buf_diff_sample = (rt5514_dsp->AEC_hotword.RTC_Current - rt5514_dsp->AEC_hotword.RTC_BufferWP)/
			tic_per_sample;

		rt5514_dsp->ts_buf_start = ts_AEC1_wp -
			((((s64)(rt5514_dsp->buf_size) / 8) + buf_diff_sample) * ns_per_sample)+
			((RECORD_SHIFT/8) * ns_per_sample);

		timestamp = rt5514_dsp->ts_buf_start;

		pr_info("%s(%d): buf_diff_sample:%d\n", __func__,__LINE__,buf_diff_sample);
		pr_info("%s(%d): ts_buf_start:%llu\n", __func__,__LINE__,rt5514_dsp->ts_buf_start);
	} else {
		timestamp = 0;
		pr_err("%s(%d): No parameters for time sync\n", __func__,__LINE__);
	}

	if (rt5514_dsp->buf_size % period_bytes)
		rt5514_dsp->buf_size = (rt5514_dsp->buf_size / period_bytes) *
			period_bytes;

	if (rt5514_dsp->buf_base && rt5514_dsp->buf_limit &&
		rt5514_dsp->buf_rp && rt5514_dsp->buf_size)
		schedule_delayed_work(&rt5514_dsp->copy_work,
			msecs_to_jiffies(0));
}

static unsigned long rt5514_cal_ps_per_tick(struct rt5514_dsp *rt5514_dsp)
{
	u32 tick_diff = (rt5514_dsp->AEC2.RTC_Current - rt5514_dsp->AEC1.RTC_Current);
	u64 ns_diff = (rt5514_dsp->ts2 - rt5514_dsp->ts1);
	u64 result = div_u64(ns_diff * 1000, tick_diff);

	if (1) {
		pr_info("%s Dsp Counter %lu - %lu = %lu\n", __func__, rt5514_dsp->AEC2.RTC_Current, rt5514_dsp->AEC1.RTC_Current, tick_diff);
		pr_info("%s Sys Time %llu - %llu = %llu\n", __func__, rt5514_dsp->ts2, rt5514_dsp->ts1, ns_diff);
		pr_info("%s Result = %llu\n", __func__, result);
	}

	return (u32) result;
}

static void rt5514_schedule_get_dsp_tic_ns(struct rt5514_dsp *rt5514_dsp)
{
	int tic_per_byte, dmic_l_val,dmic_r_val,dmic_l_mute,dmic_r_mute;

	rt5514_set_irq_low();

	/* Mute dmic during calculate dsp ti ns */
	rt5514_spi_read_addr(0x18002190,&dmic_l_val);
	rt5514_spi_read_addr(0x18002194,&dmic_r_val);
	dmic_l_mute = dmic_l_val | 0x0800;
	dmic_r_mute = dmic_r_val | 0x0800;
	rt5514_spi_write_addr(0x18002190, dmic_l_mute);
	rt5514_spi_write_addr(0x18002194, dmic_r_mute);
	/* Mute dmic during calculate dsp ti ns */

	/* enable IRQ to host */
	rt5514_spi_write_addr(0x18001028, 0x1);

	rt5514_spi_time_sync(1,RT5514_GET_TIC_NS);
	msleep(100);

	rt5514_spi_time_sync(2,RT5514_GET_TIC_NS);
	msleep(20);

	rt5514_dsp->time_syncing = 0;

	ns_per_tic = (int)(rt5514_dsp->ts2 - rt5514_dsp->ts1) /
		(rt5514_dsp->AEC2.RTC_Current - rt5514_dsp->AEC1.RTC_Current);

	ps_tic = rt5514_cal_ps_per_tick(rt5514_dsp);

	ns_per_sample = (ns_per_tic *
		(rt5514_dsp->AEC1.Diff_T + rt5514_dsp->AEC2.Diff_T)) /
		((rt5514_dsp->AEC1.Diff_WP + rt5514_dsp->AEC2.Diff_WP) / 4);

	tic_per_byte = (rt5514_dsp->AEC1.Diff_T + rt5514_dsp->AEC2.Diff_T) /
		(rt5514_dsp->AEC1.Diff_WP + rt5514_dsp->AEC2.Diff_WP);

	tic_per_sample = tic_per_byte * 4;

	pr_info("%s(): tic_per_byte:%d,tic_per_sample:%d\n", __func__, tic_per_byte, tic_per_sample);
	pr_info("%s(): ns_per_tic:%d,ns_per_sample:%d, ps_tic:%lu\n", __func__,ns_per_tic, ns_per_sample, ps_tic);

	/* Unmute dmic */
	rt5514_spi_write_addr(0x18002190, dmic_l_val);
	rt5514_spi_write_addr(0x18002194, dmic_r_val);
}

static void rt5514_spi_start_work(struct work_struct *work)
{
	struct rt5514_dsp *rt5514_dsp =
		container_of(work, struct rt5514_dsp, start_work.work);

	rt5514_schedule_copy(rt5514_dsp);
}

static void rt5514_get_dsp_tic_ns(struct work_struct *work)
{
	struct rt5514_dsp *rt5514_dsp =
		container_of(work, struct rt5514_dsp, get_dsp_tic.work);

	rt5514_schedule_get_dsp_tic_ns(rt5514_dsp);
}

static void rt5514_watchdog_work(struct work_struct *work)
{
	pr_info("%s -- watchdog work!\n", __func__);
	rt5514_dsp_reload_fw(0);
	rt5514_dsp_reload_fw(1);

	enable_irq(gpio_hotword);
}

static void rt5514_helper(struct rt5514_dsp *rt5514_dsp)
{
	unsigned int device_id,wdg_status;
	u64 timestamp;
	Params_AEC AEC;
	u8 ret_dev_id[8] = {0}, ret_wdg[8] = {0};

	int time_sync;

	timestamp = ktime_get_ns();
	rt5514_spi_burst_read(0x18002ff4, (u8 *)ret_dev_id, sizeof(ret_dev_id));
	rt5514_spi_burst_read(0x18002f04, (u8 *)ret_wdg, sizeof(ret_wdg));
	rt5514_spi_read_addr(0x18002fa8, &time_sync);

	device_id = ret_dev_id[0] | ret_dev_id[1] << 8 | ret_dev_id[2] << 16 | ret_dev_id[3] << 24;
	wdg_status = ret_wdg[0] | ret_wdg[1] << 8 | ret_wdg[2] << 16 | ret_wdg[3] << 24;
	wdg_status = wdg_status & (0x2);
	pr_info("%s -- timestamp:%llu\n", __func__,timestamp);
	pr_info("%s -- device id:0x%x,wdg_status:0x%x,time_sync:%d\n", __func__,device_id,wdg_status,time_sync);
	if ((device_id != RT5514_DEVICE_ID) || (wdg_status)) {
		schedule_delayed_work(&rt5514_dsp->watchdog_work,
				msecs_to_jiffies(0));
	} else {
		rt5514_set_irq_low();

		enable_irq(gpio_hotword);

		if (time_sync) {
			if (rt5514_dsp->time_syncing) {

				rt5514_spi_burst_read(0x4ff60000, (u8 *)&AEC, sizeof(Params_AEC));

				if (rt5514_dsp->time_syncing == 1) {
					rt5514_dsp->ts1 = timestamp;
					rt5514_dsp->AEC1 = AEC;
				} else {
					rt5514_dsp->ts2 = timestamp;
					rt5514_dsp->AEC2 = AEC;
					rt5514_spi_write_addr(0x18002fa8, 0x0);
				}
				pr_info("%s(%d) -- 1\n", __func__, __LINE__);
			} else {
				pr_info("%s(%d) -- 2\n", __func__, __LINE__);
				schedule_delayed_work(&rt5514_dsp->get_dsp_tic,
				msecs_to_jiffies(0));
			}
		} else {
			pr_info("%s(%d) -- 3\n", __func__, __LINE__);

			rt5514_hotdet(0);

			rt5514_spi_burst_read(0x4ff60000, (u8 *)&AEC, sizeof(Params_AEC));
			rt5514_dsp->ts_wp_soc = timestamp;
			rt5514_dsp->AEC_hotword = AEC;

			schedule_delayed_work(&rt5514_dsp->wake_work, 0);

			schedule_delayed_work(&rt5514_dsp->start_work,
			msecs_to_jiffies(0));
		}
	}
}

static void rt5514_wake_work(struct work_struct *work)
{
	struct rt5514_dsp *rt5514_dsp =
		container_of(work, struct rt5514_dsp, wake_work.work);

	dev_info(&rt5514_spi->dev, "%s Send key event\n", __func__);
	wake_lock_timeout(&dsp_lock, msecs_to_jiffies(10*HZ));
	input_report_key(rt5514_dsp->input_dev, KEY_WAKEUP, 1);
	input_sync(rt5514_dsp->input_dev);

	msleep(500);

	input_report_key(rt5514_dsp->input_dev, KEY_WAKEUP, 0);
	input_sync(rt5514_dsp->input_dev);
	dev_info(&rt5514_spi->dev, "%s Send key event\n", __func__);
}

static irqreturn_t rt5514_spi_hotword_irq(int irq, void *data)
{
	struct rt5514_dsp *rt5514_dsp = data;

	disable_irq_nosync(gpio_hotword);

	pm_wakeup_event(rt5514_dsp->dev, 500);

	if (atomic_read(&is_spi_ready) == 0) {
		pr_info("%s Skip, wait spi driver resume\n", __func__);
	} else {
		rt5514_helper(rt5514_dsp);
	}

	return IRQ_HANDLED;
}

/* PCM for streaming audio from the DSP buffer */
static int rt5514_spi_pcm_open(struct snd_pcm_substream *substream)
{
	snd_soc_set_runtime_hwparams(substream, &rt5514_spi_pcm_hardware);

	return 0;
}

static int rt5514_spi_hw_params(struct snd_pcm_substream *substream,
			       struct snd_pcm_hw_params *hw_params)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct rt5514_dsp *rt5514_dsp =
			snd_soc_platform_get_drvdata(rtd->platform);
	int ret;
	u8 buf[8];

	mutex_lock(&rt5514_dsp->dma_lock);
	ret = snd_pcm_lib_alloc_vmalloc_buffer(substream,
			params_buffer_bytes(hw_params));
	rt5514_dsp->substream = substream;
	rt5514_dsp->dma_offset = 0;

	/* Read IRQ status and schedule copy accordingly. */
	rt5514_spi_burst_read(RT5514_IRQ_CTRL, (u8 *)&buf, sizeof(buf));
	if (buf[0] & RT5514_IRQ_STATUS_BIT)
		rt5514_schedule_copy(rt5514_dsp);

	mutex_unlock(&rt5514_dsp->dma_lock);

	return ret;
}

static int rt5514_spi_hw_free(struct snd_pcm_substream *substream)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct rt5514_dsp *rt5514_dsp =
			snd_soc_platform_get_drvdata(rtd->platform);
	u8 buf[8] = {0};

	mutex_lock(&rt5514_dsp->dma_lock);
	rt5514_dsp->substream = NULL;
	mutex_unlock(&rt5514_dsp->dma_lock);

	cancel_delayed_work_sync(&rt5514_dsp->copy_work);

	rt5514_hotdet(1);

	rt5514_set_irq_low();

	return snd_pcm_lib_free_vmalloc_buffer(substream);
}

static snd_pcm_uframes_t rt5514_spi_pcm_pointer(
		struct snd_pcm_substream *substream)
{
	struct snd_pcm_runtime *runtime = substream->runtime;
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct rt5514_dsp *rt5514_dsp =
		snd_soc_platform_get_drvdata(rtd->platform);

	return bytes_to_frames(runtime, rt5514_dsp->dma_offset);
}

static const struct snd_pcm_ops rt5514_spi_pcm_ops = {
	.open		= rt5514_spi_pcm_open,
	.hw_params	= rt5514_spi_hw_params,
	.hw_free	= rt5514_spi_hw_free,
	.pointer	= rt5514_spi_pcm_pointer,
	.mmap		= snd_pcm_lib_mmap_vmalloc,
	.page		= snd_pcm_lib_get_vmalloc_page,
};

static int rt5514_parse_irq(struct device_node *np)
{
	gpio_hotword = irq_of_parse_and_map(np, 0);
	if (gpio_hotword <= 0){
		pr_info("%s No gpio_hotword number found\n", __func__);
		return -1;
	}
	return 0;
}

static int rt5514_spi_pcm_probe(struct snd_soc_platform *platform)
{
	struct rt5514_dsp *rt5514_dsp;
	struct device_node *np = NULL;
	int ret;

	rt5514_dsp = devm_kzalloc(platform->dev, sizeof(*rt5514_dsp),
			GFP_KERNEL);

	rt5514_dsp->dev = &rt5514_spi->dev;
	mutex_init(&rt5514_dsp->dma_lock);
	INIT_DELAYED_WORK(&rt5514_dsp->copy_work, rt5514_spi_copy_work);
	INIT_DELAYED_WORK(&rt5514_dsp->start_work, rt5514_spi_start_work);
	INIT_DELAYED_WORK(&rt5514_dsp->get_dsp_tic, rt5514_get_dsp_tic_ns);
	INIT_DELAYED_WORK(&rt5514_dsp->watchdog_work, rt5514_watchdog_work);
	INIT_DELAYED_WORK(&rt5514_dsp->wake_work, rt5514_wake_work);
	snd_soc_platform_set_drvdata(platform, rt5514_dsp);

	np = of_find_compatible_node(NULL, NULL, "realtek,rt5514-spi");
	if (!np){
		dev_err(&rt5514_spi->dev,
				"%s DTS compatible node not found!\n", __func__);
		return -1;
	}

	ret = rt5514_parse_irq(np);
	if(ret){
		dev_err(&rt5514_spi->dev,
				"%s Fail to parse irq number!\n", __func__);
		return -1;
	}

	if (gpio_hotword) {
		ret = devm_request_threaded_irq(&rt5514_spi->dev,
			gpio_hotword, NULL, rt5514_spi_hotword_irq,
			IRQF_TRIGGER_RISING | IRQF_ONESHOT, "rt5514-spi-hot",
			rt5514_dsp);
		if (ret) {
			dev_err(&rt5514_spi->dev,
				"%s Failed to reguest IRQ: %d\n", __func__,
				ret);
		} else {
			device_init_wakeup(&rt5514_spi->dev, true);
			dev_pm_set_wake_irq(&rt5514_spi->dev, gpio_hotword);
		}
	}

	rt5514_dsp->input_dev = input_allocate_device();
	if (rt5514_dsp->input_dev) {
		struct input_dev *input = rt5514_dsp->input_dev;
		wake_lock_init(&dsp_lock, WAKE_LOCK_SUSPEND, "dsptrigger");
		input->name = "hotword-trigger-key";
		input->id.bustype = BUS_HOST;
		input->id.vendor = 0x0001;
		input->id.product = 0x0001;
		input->id.version = 0x0001;
		__set_bit(143, input->keybit);
		__set_bit(EV_KEY, input->evbit);

		ret = input_register_device(input);
		if(ret) {
			dev_err(&rt5514_spi->dev, "input allocate device fail.\n");
			input_free_device(input);
		}
	}

	atomic_set(&is_spi_ready, 1);

	spi_set_drvdata(rt5514_spi, rt5514_dsp);

	return 0;
}

static struct snd_soc_platform_driver rt5514_spi_platform = {
	.probe = rt5514_spi_pcm_probe,
	.ops = &rt5514_spi_pcm_ops,
};

static const struct snd_soc_component_driver rt5514_spi_dai_component = {
	.name		= "rt5514-spi-dai",
};

/**
 * rt5514_spi_burst_read - Read data from SPI by rt5514 address.
 * @addr: Start address.
 * @rxbuf: Data Buffer for reading.
 * @len: Data length, it must be a multiple of 8.
 *
 *
 * Returns true for success.
 */
int rt5514_spi_burst_read(unsigned int addr, u8 *rxbuf, size_t len)
{
	u8 spi_cmd = RT5514_SPI_CMD_BURST_READ;
	int status;
	u8 write_buf[8];
	unsigned int i, end, offset = 0;

	struct spi_message message;
	struct spi_transfer x[3];

	while (offset < len) {
		if (offset + RT5514_SPI_BUF_LEN <= len)
			end = RT5514_SPI_BUF_LEN;
		else
			end = len % RT5514_SPI_BUF_LEN;

		write_buf[0] = spi_cmd;
		write_buf[1] = ((addr + offset) & 0xff000000) >> 24;
		write_buf[2] = ((addr + offset) & 0x00ff0000) >> 16;
		write_buf[3] = ((addr + offset) & 0x0000ff00) >> 8;
		write_buf[4] = ((addr + offset) & 0x000000ff) >> 0;

		spi_message_init(&message);
		memset(x, 0, sizeof(x));

		x[0].len = 5;
		x[0].tx_buf = write_buf;
		spi_message_add_tail(&x[0], &message);

		x[1].len = 4;
		x[1].tx_buf = write_buf;
		spi_message_add_tail(&x[1], &message);

		x[2].len = end;
		x[2].rx_buf = rxbuf + offset;
		spi_message_add_tail(&x[2], &message);

		status = spi_sync(rt5514_spi, &message);

		if (status)
			return false;

		offset += RT5514_SPI_BUF_LEN;
	}

	for (i = 0; i < len; i += 8) {
		write_buf[0] = rxbuf[i + 0];
		write_buf[1] = rxbuf[i + 1];
		write_buf[2] = rxbuf[i + 2];
		write_buf[3] = rxbuf[i + 3];
		write_buf[4] = rxbuf[i + 4];
		write_buf[5] = rxbuf[i + 5];
		write_buf[6] = rxbuf[i + 6];
		write_buf[7] = rxbuf[i + 7];

		rxbuf[i + 0] = write_buf[7];
		rxbuf[i + 1] = write_buf[6];
		rxbuf[i + 2] = write_buf[5];
		rxbuf[i + 3] = write_buf[4];
		rxbuf[i + 4] = write_buf[3];
		rxbuf[i + 5] = write_buf[2];
		rxbuf[i + 6] = write_buf[1];
		rxbuf[i + 7] = write_buf[0];
	}

	return true;
}
EXPORT_SYMBOL_GPL(rt5514_spi_burst_read);

/**
 * rt5514_spi_burst_write - Write data to SPI by rt5514 address.
 * @addr: Start address.
 * @txbuf: Data Buffer for writng.
 * @len: Data length, it must be a multiple of 8.
 *
 *
 * Returns true for success.
 */
int rt5514_spi_burst_write(u32 addr, const u8 *txbuf, size_t len)
{
	u8 spi_cmd = RT5514_SPI_CMD_BURST_WRITE;
	u8 *write_buf;
	unsigned int i, end, offset = 0;

	write_buf = kmalloc(RT5514_SPI_BUF_LEN + 6, GFP_KERNEL);

	if (write_buf == NULL)
		return -ENOMEM;

	while (offset < len) {
		if (offset + RT5514_SPI_BUF_LEN <= len)
			end = RT5514_SPI_BUF_LEN;
		else
			end = len % RT5514_SPI_BUF_LEN;

		write_buf[0] = spi_cmd;
		write_buf[1] = ((addr + offset) & 0xff000000) >> 24;
		write_buf[2] = ((addr + offset) & 0x00ff0000) >> 16;
		write_buf[3] = ((addr + offset) & 0x0000ff00) >> 8;
		write_buf[4] = ((addr + offset) & 0x000000ff) >> 0;

		for (i = 0; i < end; i += 8) {
			write_buf[i + 12] = txbuf[offset + i + 0];
			write_buf[i + 11] = txbuf[offset + i + 1];
			write_buf[i + 10] = txbuf[offset + i + 2];
			write_buf[i +  9] = txbuf[offset + i + 3];
			write_buf[i +  8] = txbuf[offset + i + 4];
			write_buf[i +  7] = txbuf[offset + i + 5];
			write_buf[i +  6] = txbuf[offset + i + 6];
			write_buf[i +  5] = txbuf[offset + i + 7];
		}

		write_buf[end + 5] = spi_cmd;

		spi_write(rt5514_spi, write_buf, end + 6);

		offset += RT5514_SPI_BUF_LEN;
	}

	kfree(write_buf);

	return 0;
}
EXPORT_SYMBOL_GPL(rt5514_spi_burst_write);

static int rt5514_spi_probe(struct spi_device *spi)
{
	int ret;

	rt5514_spi = spi;

	ret = devm_snd_soc_register_platform(&spi->dev, &rt5514_spi_platform);
	if (ret < 0) {
		dev_err(&spi->dev, "Failed to register platform.\n");
		return ret;
	}

	ret = devm_snd_soc_register_component(&spi->dev,
					      &rt5514_spi_dai_component,
					      &rt5514_spi_dai, 1);
	if (ret < 0) {
		dev_err(&spi->dev, "Failed to register component.\n");
		return ret;
	}

	return 0;
}

static int rt5514_suspend(struct device *dev)
{
	struct snd_soc_platform *platform = snd_soc_lookup_platform(dev);
	struct rt5514_dsp *rt5514_dsp =
		snd_soc_platform_get_drvdata(platform);

	if (rt5514_dsp)
		atomic_set(&is_spi_ready, 0);

	return 0;
}

static int rt5514_resume(struct device *dev)
{
	struct snd_soc_platform *platform = snd_soc_lookup_platform(dev);
	struct rt5514_dsp *rt5514_dsp =
		snd_soc_platform_get_drvdata(platform);
	u8 buf[8];

	if (rt5514_dsp) {
		atomic_set(&is_spi_ready, 1);

		if (rt5514_dsp->substream) {
			rt5514_spi_burst_read(RT5514_IRQ_CTRL, (u8 *)&buf,
				sizeof(buf));
			if (buf[0] & RT5514_IRQ_STATUS_BIT)
				rt5514_helper(rt5514_dsp);
		}
	}

	return 0;
}

static SIMPLE_DEV_PM_OPS(rt5514_pm_ops, rt5514_suspend, rt5514_resume);

static const struct of_device_id rt5514_of_match[] = {
	{ .compatible = "realtek,rt5514-spi", },
	{},
};
MODULE_DEVICE_TABLE(of, rt5514_of_match);

static struct spi_driver rt5514_spi_driver = {
	.driver = {
		.name = "rt5514",
		.pm = &rt5514_pm_ops,
		.of_match_table = of_match_ptr(rt5514_of_match),
	},
	.probe = rt5514_spi_probe,
};
module_spi_driver(rt5514_spi_driver);

MODULE_DESCRIPTION("RT5514 SPI driver");
MODULE_AUTHOR("Oder Chiou <oder_chiou@realtek.com>");
MODULE_LICENSE("GPL v2");
