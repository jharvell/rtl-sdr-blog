/*
 * rtl-sdr, turns your Realtek RTL2832 based DVB dongle into a SDR receiver
 * Copyright (C) 2012 by Steve Markgraf <steve@steve-m.de>
 * Copyright (C) 2012 by Dimitri Stolnikov <horiz0n@gmx.net>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <errno.h>
#include <signal.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#ifndef _WIN32
#include <unistd.h>
#endif

#include <libusb.h>

/*
 * All libusb callback functions should be marked with the LIBUSB_CALL macro
 * to ensure that they are compiled with the same calling convention as libusb.
 *
 * If the macro isn't available in older libusb versions, we simply define it.
 */
#ifndef LIBUSB_CALL
#define LIBUSB_CALL
#endif

#include <rtl-sdr.h>
#include "tuner_e4000.h"
#include "tuner_fc0012.h"
#include "tuner_fc0013.h"
#include "tuner_fc2580.h"

typedef struct rtlsdr_tuner {
	int(*init)(void *);
	int(*exit)(void *);
	int(*tune)(void *, int freq /* Hz */);
	int(*set_bw)(void *, int bw /* Hz */);
	int(*set_gain)(void *, int gain /* dB */);
	int freq; /* Hz */
	int corr; /* ppm */
	int gain; /* dB */
} rtlsdr_tuner_t;

void rtlsdr_set_gpio_bit(rtlsdr_dev_t *dev, uint8_t gpio, int val);

/* generic tuner interface functions, shall be moved to the tuner implementations */
int e4k_init(void *dev) { return e4000_Initialize(dev); }
int e4k_exit(void *dev) { return 0; }
int e4k_tune(void *dev, int freq) { return e4000_SetRfFreqHz(dev, freq); }
int e4k_set_bw(void *dev, int bw) { return e4000_SetBandwidthHz(dev, 8000000); }
int e4k_set_gain(void *dev, int gain) { return 0; }

int fc0012_init(void *dev) { return FC0012_Open(dev); }
int fc0012_exit(void *dev) { return 0; }
int fc0012_tune(void *dev, int freq) {
	unsigned int bw = 6;
	/* select V-band/U-band filter */
	rtlsdr_set_gpio_bit(dev, 6, (freq > 300000000) ? 1 : 0);
	return FC0012_SetFrequency(dev, freq/1000, bw & 0xff);
}
int fc0012_set_bw(void *dev, int bw) {
	unsigned long freq = ((rtlsdr_tuner_t *)dev)->freq;
	return FC0013_SetFrequency(dev, freq/1000, bw/1000000);
}
int fc0012_set_gain(void *dev, int gain) { return 0; }

int fc0013_init(void *dev) { return FC0013_Open(dev); }
int fc0013_exit(void *dev) { return 0; }
int fc0013_tune(void *dev, int freq) {
	unsigned int bw = 6;
	return FC0013_SetFrequency(dev, freq/1000, bw & 0xff);
}
int fc0013_set_bw(void *dev, int bw) {
	unsigned long freq = ((rtlsdr_tuner_t *)dev)->freq;
	return FC0013_SetFrequency(dev, freq/1000, bw/1000000);
}
int fc0013_set_gain(void *dev, int gain) { return 0; }

int fc2580_init(void *dev) { return fc2580_Initialize(dev); }
int fc2580_exit(void *dev) { return 0; }
int fc2580_tune(void *dev, int freq) { return fc2580_SetRfFreqHz(dev, freq); }
int fc2580_set_bw(void *dev, int bw) { return fc2580_SetBandwidthMode(dev, 1); }
int fc2580_set_gain(void *dev, int gain) { return 0; }

enum rtlsdr_tuners {
	RTLSDR_TUNER_E4000,
	RTLSDR_TUNER_FC0012,
	RTLSDR_TUNER_FC0013,
	RTLSDR_TUNER_FC2580
};

static rtlsdr_tuner_t tuners[] = {
	{ e4k_init, e4k_exit, e4k_tune, e4k_set_bw, e4k_set_gain, 0, 0, 0 },
	{ fc0012_init, fc0012_exit, fc0012_tune, fc0012_set_bw, fc0012_set_gain, 0, 0, 0 },
	{ fc0013_init, fc0013_exit, fc0013_tune, fc0013_set_bw, fc0013_set_gain, 0, 0, 0 },
	{ fc2580_init, fc2580_exit, fc2580_tune, fc2580_set_bw, fc2580_set_gain, 0, 0, 0 },
};

typedef struct rtlsdr_device {
	uint16_t vid;
	uint16_t pid;
	const char *name;
} rtlsdr_device_t;

static rtlsdr_device_t devices[] = {
	{ 0x0bda, 0x2832, "Generic RTL2832U (e.g. hama nano)" },
	{ 0x0bda, 0x2838, "ezcap USB 2.0 DVB-T/DAB/FM dongle" },
	{ 0x0ccd, 0x00a9, "Terratec Cinergy T Stick Black (rev 1)" },
	{ 0x0ccd, 0x00b3, "Terratec NOXON DAB/DAB+ USB dongle (rev 1)" },
	{ 0x0ccd, 0x00e0, "Terratec NOXON DAB/DAB+ USB dongle (rev 2)" },
	{ 0x1f4d, 0xb803, "GTek T803" },
	{ 0x1f4d, 0xc803, "Lifeview LV5TDeluxe" },
	{ 0x1b80, 0xd3a4, "Twintech UT-40" },
	{ 0x1d19, 0x1101, "Dexatek DK DVB-T Dongle (Logilink VG0002A)" },
	{ 0x1d19, 0x1102, "Dexatek DK DVB-T Dongle (MSI DigiVox mini II V3.0)" },
	{ 0x0458, 0x707f, "Genius TVGo DVB-T03 USB dongle (Ver. B)" },
	{ 0x1b80, 0xd393, "GIGABYTE GT-U7300" },
	{ 0x1b80, 0xd395, "Peak 102569AGPK" },
	{ 0x1b80, 0xd39d, "SVEON STV20 DVB-T USB & FM" },
};

#define BUF_COUNT	32
#define BUF_LENGTH	(16 * 16384)

typedef struct rtlsdr_dev {
	libusb_context *ctx;
	struct libusb_device_handle *devh;
	struct libusb_transfer *xfer[BUF_COUNT];
	unsigned char *xfer_buf[BUF_COUNT];
	rtlsdr_async_read_cb_t cb;
	void *cb_ctx;
	int run_async;
	rtlsdr_tuner_t *tuner;
	int rate; /* Hz */
} rtlsdr_dev_t;

#define CRYSTAL_FREQ	28800000
#define MAX_SAMP_RATE	3200000

#define CTRL_IN		(LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_ENDPOINT_IN)
#define CTRL_OUT	(LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_ENDPOINT_OUT)

enum usb_reg {
	USB_SYSCTL		= 0x2000,
	USB_CTRL		= 0x2010,
	USB_STAT		= 0x2014,
	USB_EPA_CFG		= 0x2144,
	USB_EPA_CTL		= 0x2148,
	USB_EPA_MAXPKT		= 0x2158,
	USB_EPA_MAXPKT_2	= 0x215a,
	USB_EPA_FIFO_CFG	= 0x2160,
};

enum sys_reg {
	DEMOD_CTL		= 0x3000,
	GPO			= 0x3001,
	GPI			= 0x3002,
	GPOE			= 0x3003,
	GPD			= 0x3004,
	SYSINTE			= 0x3005,
	SYSINTS			= 0x3006,
	GP_CFG0			= 0x3007,
	GP_CFG1			= 0x3008,
	SYSINTE_1		= 0x3009,
	SYSINTS_1		= 0x300a,
	DEMOD_CTL_1		= 0x300b,
	IR_SUSPEND		= 0x300c,
};

enum blocks {
	DEMODB			= 0,
	USBB			= 1,
	SYSB			= 2,
	TUNB			= 3,
	ROMB			= 4,
	IRB			= 5,
	IICB			= 6,
};

int rtlsdr_read_array(rtlsdr_dev_t *dev, uint8_t block, uint16_t addr, uint8_t *array, uint8_t len)
{
	int r;
	uint16_t index = (block << 8);

	r = libusb_control_transfer(dev->devh, CTRL_IN, 0, addr, index, array, len, 0);

	return r;
}

int rtlsdr_write_array(rtlsdr_dev_t *dev, uint8_t block, uint16_t addr, uint8_t *array, uint8_t len)
{
	int r;
	uint16_t index = (block << 8) | 0x10;

	r = libusb_control_transfer(dev->devh, CTRL_OUT, 0, addr, index, array, len, 0);

	return r;
}

int rtlsdr_i2c_write_reg(rtlsdr_dev_t *dev, uint8_t i2c_addr, uint8_t reg, uint8_t val)
{
	uint16_t addr = i2c_addr;
	uint8_t data[2];

	data[0] = reg;
	data[1] = val;
	return rtlsdr_write_array(dev, IICB, addr, (uint8_t *)&data, 2);
}

uint8_t rtlsdr_i2c_read_reg(rtlsdr_dev_t *dev, uint8_t i2c_addr, uint8_t reg)
{
	uint16_t addr = i2c_addr;
	uint8_t data;

	rtlsdr_write_array(dev, IICB, addr, &reg, 1);
	rtlsdr_read_array(dev, IICB, addr, &data, 1);

	return data;
}

int rtlsdr_i2c_write(rtlsdr_dev_t *dev, uint8_t i2c_addr, uint8_t *buffer, int len)
{
	uint16_t addr = i2c_addr;

	if (!dev)
		return -1;

	return rtlsdr_write_array(dev, IICB, addr, buffer, len);
}

int rtlsdr_i2c_read(rtlsdr_dev_t *dev, uint8_t i2c_addr, uint8_t *buffer, int len)
{
	uint16_t addr = i2c_addr;

	if (!dev)
		return -1;

	return rtlsdr_read_array(dev, IICB, addr, buffer, len);
}

uint16_t rtlsdr_read_reg(rtlsdr_dev_t *dev, uint8_t block, uint16_t addr, uint8_t len)
{
	int r;
	unsigned char data[2];
	uint16_t index = (block << 8);
	uint16_t reg;

	r = libusb_control_transfer(dev->devh, CTRL_IN, 0, addr, index, data, len, 0);

	if (r < 0)
		fprintf(stderr, "%s failed\n", __FUNCTION__);

	reg = (data[1] << 8) | data[0];

	return reg;
}

void rtlsdr_write_reg(rtlsdr_dev_t *dev, uint8_t block, uint16_t addr, uint16_t val, uint8_t len)
{
	int r;
	unsigned char data[2];

	uint16_t index = (block << 8) | 0x10;

	if (len == 1)
		data[0] = val & 0xff;
	else
		data[0] = val >> 8;

	data[1] = val & 0xff;

	r = libusb_control_transfer(dev->devh, CTRL_OUT, 0, addr, index, data, len, 0);

	if (r < 0)
		fprintf(stderr, "%s failed\n", __FUNCTION__);
}

uint16_t rtlsdr_demod_read_reg(rtlsdr_dev_t *dev, uint8_t page, uint8_t addr, uint8_t len)
{
	int r;
	unsigned char data[2];

	uint16_t index = page;
	uint16_t reg;
	addr = (addr << 8) | 0x20;

	r = libusb_control_transfer(dev->devh, CTRL_IN, 0, addr, index, data, len, 0);

	if (r < 0)
		fprintf(stderr, "%s failed\n", __FUNCTION__);

	reg = (data[1] << 8) | data[0];

	return reg;
}

void rtlsdr_demod_write_reg(rtlsdr_dev_t *dev, uint8_t page, uint16_t addr, uint16_t val, uint8_t len)
{
	int r;
	unsigned char data[2];
	uint16_t index = 0x10 | page;
	addr = (addr << 8) | 0x20;

	if (len == 1)
		data[0] = val & 0xff;
	else
		data[0] = val >> 8;

	data[1] = val & 0xff;

	r = libusb_control_transfer(dev->devh, CTRL_OUT, 0, addr, index, data, len, 0);

	if (r < 0)
		fprintf(stderr, "%s failed\n", __FUNCTION__);

	rtlsdr_demod_read_reg(dev, 0x0a, 0x01, 1);
}

void rtlsdr_set_gpio_bit(rtlsdr_dev_t *dev, uint8_t gpio, int val)
{
	uint8_t r;

	gpio = 1 << gpio;
	r = rtlsdr_read_reg(dev, SYSB, GPO, 1);
	r = val ? (r | gpio) : (r & ~gpio);
	rtlsdr_write_reg(dev, SYSB, GPO, r, 1);
}

void rtlsdr_set_gpio_output(rtlsdr_dev_t *dev, uint8_t gpio)
{
	int r;
	gpio = 1 << gpio;

	r = rtlsdr_read_reg(dev, SYSB, GPD, 1);
	rtlsdr_write_reg(dev, SYSB, GPO, r & ~gpio, 1);
	r = rtlsdr_read_reg(dev, SYSB, GPOE, 1);
	rtlsdr_write_reg(dev, SYSB, GPOE, r | gpio, 1);
}

void rtlsdr_set_i2c_repeater(rtlsdr_dev_t *dev, int on)
{
	rtlsdr_demod_write_reg(dev, 1, 0x01, on ? 0x18 : 0x10, 1);
}

void rtlsdr_init_baseband(rtlsdr_dev_t *dev)
{
	unsigned int i;

	/* default FIR coefficients used for DAB/FM by the Windows driver,
	 * the DVB driver uses different ones */
	uint8_t fir_coeff[] = {
		0xca, 0xdc, 0xd7, 0xd8, 0xe0, 0xf2, 0x0e, 0x35, 0x06, 0x50,
		0x9c, 0x0d, 0x71, 0x11, 0x14, 0x71, 0x74, 0x19, 0x41, 0x00,
	};

	/* initialize USB */
	rtlsdr_write_reg(dev, USBB, USB_SYSCTL, 0x09, 1);
	rtlsdr_write_reg(dev, USBB, USB_EPA_MAXPKT, 0x0002, 2);
	rtlsdr_write_reg(dev, USBB, USB_EPA_CTL, 0x1002, 2);

	/* poweron demod */
	rtlsdr_write_reg(dev, SYSB, DEMOD_CTL_1, 0x22, 1);
	rtlsdr_write_reg(dev, SYSB, DEMOD_CTL, 0xe8, 1);

	/* reset demod (bit 3, soft_rst) */
	rtlsdr_demod_write_reg(dev, 1, 0x01, 0x14, 1);
	rtlsdr_demod_write_reg(dev, 1, 0x01, 0x10, 1);

	/* disable spectrum inversion and adjacent channel rejection */
	rtlsdr_demod_write_reg(dev, 1, 0x15, 0x00, 1);
	rtlsdr_demod_write_reg(dev, 1, 0x16, 0x0000, 2);

	/* set IF-frequency to 0 Hz */
	rtlsdr_demod_write_reg(dev, 1, 0x19, 0x0000, 2);

	/* set FIR coefficients */
	for (i = 0; i < sizeof (fir_coeff); i++)
		rtlsdr_demod_write_reg(dev, 1, 0x1c + i, fir_coeff[i], 1);

	rtlsdr_demod_write_reg(dev, 0, 0x19, 0x25, 1);

	/* init FSM state-holding register */
	rtlsdr_demod_write_reg(dev, 1, 0x93, 0xf0, 1);

	/* disable AGC (en_dagc, bit 0) */
	rtlsdr_demod_write_reg(dev, 1, 0x11, 0x00, 1);

	/* disable PID filter (enable_PID = 0) */
	rtlsdr_demod_write_reg(dev, 0, 0x61, 0x60, 1);

	/* opt_adc_iq = 0, default ADC_I/ADC_Q datapath */
	rtlsdr_demod_write_reg(dev, 0, 0x06, 0x80, 1);

	/* Enable Zero-IF mode (en_bbin bit), DC cancellation (en_dc_est),
	 * IQ estimation/compensation (en_iq_comp, en_iq_est) */
	rtlsdr_demod_write_reg(dev, 1, 0xb1, 0x1b, 1);
}

void rtlsdr_deinit_baseband(rtlsdr_dev_t *dev)
{
	/* deinitialize tuner */
	rtlsdr_set_i2c_repeater(dev, 1);
	dev->tuner->exit(dev);
	rtlsdr_set_i2c_repeater(dev, 0);

	/* poweroff demodulator and ADCs */
	rtlsdr_write_reg(dev, SYSB, DEMOD_CTL, 0x20, 1);
}

int rtlsdr_set_center_freq(rtlsdr_dev_t *dev, uint32_t freq)
{
	int r;
	double f = (double) freq;

	if (!dev || !dev->tuner)
		return -1;

	rtlsdr_set_i2c_repeater(dev, 1);

	f *= 1.0 + dev->tuner->corr / 1e6;
	r = dev->tuner->tune((void *)dev, (int) f);

	rtlsdr_set_i2c_repeater(dev, 0);

	if (!r)
		dev->tuner->freq = freq;

	return r;
}

int rtlsdr_get_center_freq(rtlsdr_dev_t *dev)
{
	if (!dev || !dev->tuner)
		return -1;

	return dev->tuner->freq;
}

int rtlsdr_set_freq_correction(rtlsdr_dev_t *dev, int ppm)
{
	int r;

	if (!dev || !dev->tuner)
		return -1;

	if (dev->tuner->corr == ppm)
		return -1;

	dev->tuner->corr = ppm;

	/* retune to apply new correction value */
	r = rtlsdr_set_center_freq(dev, dev->tuner->freq);

	return r;
}

int rtlsdr_get_freq_correction(rtlsdr_dev_t *dev)
{
	if (!dev || !dev->tuner)
		return -1;

	return dev->tuner->corr;
}

int rtlsdr_set_tuner_gain(rtlsdr_dev_t *dev, int gain)
{
	int r;

	if (!dev || !dev->tuner)
		return -1;

	r = dev->tuner->set_gain((void *)dev, gain);

	if (!r)
		dev->tuner->gain = gain;

	return r;
}

int rtlsdr_get_tuner_gain(rtlsdr_dev_t *dev)
{
	if (!dev || !dev->tuner)
		return -1;

	return dev->tuner->gain;
}

int rtlsdr_set_sample_rate(rtlsdr_dev_t *dev, uint32_t samp_rate)
{
	uint16_t tmp;
	uint32_t rsamp_ratio;
	double real_rate;

	if (!dev)
		return -1;

	/* check for the maximum rate the resampler supports */
	if (samp_rate > MAX_SAMP_RATE)
		samp_rate = MAX_SAMP_RATE;

	rsamp_ratio = (CRYSTAL_FREQ * pow(2, 22)) / samp_rate;
	rsamp_ratio &= ~3;

	real_rate = (CRYSTAL_FREQ * pow(2, 22)) / rsamp_ratio;
	fprintf(stderr, "Setting sample rate: %.3f Hz\n", real_rate);

	if (dev->tuner)
		dev->tuner->set_bw((void *)dev, real_rate);

	dev->rate = samp_rate;

	tmp = (rsamp_ratio >> 16);
	rtlsdr_demod_write_reg(dev, 1, 0x9f, tmp, 2);
	tmp = rsamp_ratio & 0xffff;
	rtlsdr_demod_write_reg(dev, 1, 0xa1, tmp, 2);

	return 0;
}

int rtlsdr_get_sample_rate(rtlsdr_dev_t *dev)
{
	if (!dev)
		return -1;

	return dev->rate;
}

rtlsdr_device_t *find_known_device(uint16_t vid, uint16_t pid)
{
	int i;
	rtlsdr_device_t *device = NULL;

	for (i = 0; i < sizeof(devices)/sizeof(rtlsdr_device_t); i++ ) {
		if (devices[i].vid == vid && devices[i].pid == pid) {
			device = &devices[i];
			break;
		}
	}

	return device;
}

uint32_t rtlsdr_get_device_count(void)
{
	int i;
	libusb_device **list;
	uint32_t device_count = 0;
	struct libusb_device_descriptor dd;
	ssize_t cnt;

	libusb_init(NULL);

	cnt = libusb_get_device_list(NULL, &list);

	for (i = 0; i < cnt; i++) {
		libusb_get_device_descriptor(list[i], &dd);

		if (find_known_device(dd.idVendor, dd.idProduct))
			device_count++;
	}

	libusb_free_device_list(list, 0);

	libusb_exit(NULL);

	return device_count;
}

const char *rtlsdr_get_device_name(uint32_t index)
{
	int i;
	libusb_device **list;
	struct libusb_device_descriptor dd;
	rtlsdr_device_t *device = NULL;
	uint32_t device_count = 0;
	ssize_t cnt;

	libusb_init(NULL);

	cnt = libusb_get_device_list(NULL, &list);

	for (i = 0; i < cnt; i++) {
		libusb_get_device_descriptor(list[i], &dd);

		device = find_known_device(dd.idVendor, dd.idProduct);

		if (device) {
			device_count++;

			if (index == device_count - 1)
				break;
		}
	}

	libusb_free_device_list(list, 0);

	libusb_exit(NULL);

	if (device)
		return device->name;
	else
		return "";
}

int rtlsdr_open(rtlsdr_dev_t **out_dev, uint32_t index)
{
	int r;
	int i;
	libusb_device **list;
	rtlsdr_dev_t *dev = NULL;
	libusb_device *device = NULL;
	uint32_t device_count = 0;
	struct libusb_device_descriptor dd;
	uint8_t reg;
	ssize_t cnt;

	dev = malloc(sizeof(rtlsdr_dev_t));
	memset(dev, 0, sizeof(rtlsdr_dev_t));

	libusb_init(&dev->ctx);

	cnt = libusb_get_device_list(dev->ctx, &list);

	for (i = 0; i < cnt; i++) {
		device = list[i];

		libusb_get_device_descriptor(list[i], &dd);

		if (find_known_device(dd.idVendor, dd.idProduct)) {
			device_count++;
		}

		if (index == device_count - 1)
			break;

		device = NULL;
	}

	if (!device) {
		r = -1;
		goto err;
	}

	r = libusb_open(device, &dev->devh);
	if (r < 0) {
		libusb_free_device_list(list, 0);
		fprintf(stderr, "usb_open error %d\n", r);
		goto err;
	}

	libusb_free_device_list(list, 0);

	r = libusb_claim_interface(dev->devh, 0);
	if (r < 0) {
		fprintf(stderr, "usb_claim_interface error %d\n", r);
		goto err;
	}

	rtlsdr_init_baseband(dev);

	/* Probe tuners */
	rtlsdr_set_i2c_repeater(dev, 1);

	reg = rtlsdr_i2c_read_reg(dev, E4K_I2C_ADDR, E4K_CHECK_ADDR);
	if (reg == E4K_CHECK_VAL) {
		fprintf(stderr, "Found Elonics E4000 tuner\n");
		dev->tuner = &tuners[RTLSDR_TUNER_E4000];
		goto found;
	}

	reg = rtlsdr_i2c_read_reg(dev, FC0013_I2C_ADDR, FC0013_CHECK_ADDR);
	if (reg == FC0013_CHECK_VAL) {
		fprintf(stderr, "Found Fitipower FC0013 tuner\n");
		dev->tuner = &tuners[RTLSDR_TUNER_FC0013];
		goto found;
	}

	/* initialise GPIOs */
	rtlsdr_set_gpio_output(dev, 5);

	/* reset tuner before probing */
	rtlsdr_set_gpio_bit(dev, 5, 1);
	rtlsdr_set_gpio_bit(dev, 5, 0);

	reg = rtlsdr_i2c_read_reg(dev, FC2580_I2C_ADDR, FC2580_CHECK_ADDR);
	if ((reg & 0x7f) == FC2580_CHECK_VAL) {
		fprintf(stderr, "Found FCI 2580 tuner\n");
		dev->tuner = &tuners[RTLSDR_TUNER_FC2580];
		goto found;
	}

	reg = rtlsdr_i2c_read_reg(dev, FC0012_I2C_ADDR, FC0012_CHECK_ADDR);
	if (reg == FC0012_CHECK_VAL) {
		fprintf(stderr, "Found Fitipower FC0012 tuner\n");
		rtlsdr_set_gpio_output(dev, 6);
		dev->tuner = &tuners[RTLSDR_TUNER_FC0012];
		goto found;
	}

found:
	if (dev->tuner)
		r =dev->tuner->init(dev);

	rtlsdr_set_i2c_repeater(dev, 0);

	*out_dev = dev;

	return 0;
err:
	if (dev) {
		if (dev->ctx)
			libusb_exit(dev->ctx);

		free(dev);
	}

	return r;
}

int rtlsdr_close(rtlsdr_dev_t *dev)
{
	int i;

	if (!dev)
		return -1;

	rtlsdr_deinit_baseband(dev);

	libusb_release_interface(dev->devh, 0);
	libusb_close(dev->devh);

	for(i = 0; i < BUF_COUNT; ++i) {
		if (dev->xfer[i])
			libusb_free_transfer(dev->xfer[i]);
		if (dev->xfer_buf[i])
			free(dev->xfer_buf[i]);
	}

	libusb_exit(dev->ctx);

	free(dev);

	return 0;
}

int rtlsdr_reset_buffer(rtlsdr_dev_t *dev)
{
	if (!dev)
		return -1;

	rtlsdr_write_reg(dev, USBB, USB_EPA_CTL, 0x1002, 2);
	rtlsdr_write_reg(dev, USBB, USB_EPA_CTL, 0x0000, 2);

	return 0;
}

int rtlsdr_read_sync(rtlsdr_dev_t *dev, void *buf, int len, int *n_read)
{
	if (!dev)
		return -1;

	return libusb_bulk_transfer(dev->devh, 0x81, buf, len, n_read, 3000);
}

static void LIBUSB_CALL _libusb_callback(struct libusb_transfer *transfer)
{
	if (LIBUSB_TRANSFER_COMPLETED == transfer->status) {
		rtlsdr_dev_t *dev = (rtlsdr_dev_t *)transfer->user_data;

		dev->cb(transfer->buffer, transfer->actual_length, dev->cb_ctx);

		libusb_submit_transfer(transfer); /* resubmit transfer */
	} else {
		/*fprintf(stderr, "transfer %d\n", transfer->status);*/
	}
}

int rtlsdr_wait_async(rtlsdr_dev_t *dev, rtlsdr_async_read_cb_t cb, void *ctx)
{
	int i, r;

	if (!dev)
		return -1;

	dev->cb = cb;
	dev->cb_ctx = ctx;

	for(i = 0; i < BUF_COUNT; ++i) {
		if (dev->xfer[i])
			continue;

		dev->xfer[i] = libusb_alloc_transfer(0);
	}

	for(i = 0; i < BUF_COUNT; ++i) {
		if (dev->xfer_buf[i])
			continue;

		dev->xfer_buf[i] = (unsigned char *)malloc(BUF_LENGTH);
	}

	for(i = 0; i < BUF_COUNT; ++i) {
		libusb_fill_bulk_transfer(dev->xfer[i],
					  dev->devh,
					  0x81,
					  dev->xfer_buf[i], BUF_LENGTH,
					  _libusb_callback,
					  (void *)dev, 0);

		libusb_submit_transfer(dev->xfer[i]);
	}

	dev->run_async = 1;

	while (dev->run_async) {
		struct timeval tv = { 1, 0 };
		r = libusb_handle_events_timeout(dev->ctx, &tv);
		if (r < 0) {
			/*fprintf(stderr, "handle_events %d\n", r);*/
			break;
		}
	}

	return r;
}

int rtlsdr_cancel_async(rtlsdr_dev_t *dev)
{
	if (!dev)
		return -1;

	if (dev->run_async) {
		dev->run_async = 0;
		return 0;
	}

	return -2;
}
