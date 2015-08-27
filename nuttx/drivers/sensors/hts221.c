/****************************************************************************
 * drivers/sensors/hts221.c
 *
 *   Copyright (C) 2014 Haltian Ltd. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 * 3. Neither the name NuttX nor the names of its contributors may be
 *    used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 * ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 ****************************************************************************/

#include <stdio.h>
#include <debug.h>
#include <stdlib.h>
#include <nuttx/config.h>
#include <nuttx/arch.h>
#include <nuttx/i2c.h>
#include <sys/types.h>
#include <string.h>
#include <fcntl.h>
#include <poll.h>
#include <errno.h>
#include <nuttx/kmalloc.h>
#include <nuttx/random.h>

#include <nuttx/sensors/hts221.h>

#ifdef CONFIG_TRACES_HTS221_HUMIDITY
#  define hts221_dbg(x, ...)    dbg(x, ##__VA_ARGS__)
#  define hts221_lldbg(x, ...)  lldbg(x, ##__VA_ARGS__)
#else
#  define hts221_dbg(x, ...)
#  define hts221_lldbg(x, ...)
#endif

/****************************************************************************
 * Pre-Processor Definitions
 ****************************************************************************/

#define HTS221_WHO_AM_I             0x0F
#define HTS221_AV_CONF              0x10
#define HTS221_CTRL_REG1            0x20
#define HTS221_CTRL_REG2            0x21
#define HTS221_CTRL_REG3            0x22
#define HTS221_STATUS_REG           0x27
#define HTS221_HUM_OUT_L            0x28
#define HTS221_HUM_OUT_H            0x29
#define HTS221_TEMP_OUT_L           0x2A
#define HTS221_TEMP_OUT_H           0x2B

/* Calibration registers */

#define HTS221_CALIB_H0_RH_X2       0x30
#define HTS221_CALIB_H1_RH_X2       0x31
#define HTS221_CALIB_T0_DEGC_X8     0x32
#define HTS221_CALIB_T1_DEGC_X8     0x33
#define HTS221_CALIB_T1_T0_MSB      0x35
#define HTS221_CALIB_H0T0_OUT_L     0x36
#define HTS221_CALIB_H0T0_OUT_H     0x37
#define HTS221_CALIB_H1T0_OUT_L     0x3A
#define HTS221_CALIB_H1T0_OUT_H     0x3B
#define HTS221_CALIB_T0_OUT_L       0x3C
#define HTS221_CALIB_T0_OUT_H       0x3D
#define HTS221_CALIB_T1_OUT_L       0x3E
#define HTS221_CALIB_T1_OUT_H       0x3F

/* HTS221_CTRL_REG1 */

#define HTS221_CTRL_REG1_PD         (1 << 7)
#define HTS221_CTRL_REG1_BDU        (1 << 2)

/* HTS221_CTRL_REG2 */

#define HTS221_CTRL_REG2_BOOT       (1 << 7)
#define HTS221_CTRL_REG2_ONE_SHOT   (1 << 0)

/* HTS221_CTRL_REG3 */

#define HTS221_CTRL_REG3_DRDY_L_H   (1 << 7)
#define HTS221_CTRL_REG3_PP_OD      (1 << 6)
#define HTS221_CTRL_REG3_DRDY_EN    (1 << 2)

/* HTS221_STATUS_REG */

#define HTS221_STATUS_REG_H_DA      (1 << 1)
#define HTS221_STATUS_REG_T_DA      (1 << 0)

#define HTS221_I2C_RETRIES          10

/****************************************************************************
* Private Function Prototypes
*****************************************************************************/

static int hts221_open(FAR struct file *filep);
static int hts221_close(FAR struct file *filep);
static ssize_t hts221_read(FAR struct file *filep, FAR char *buffer,
                           size_t buflen);
static ssize_t hts221_write(FAR struct file *filep, FAR const char *buffer,
                            size_t buflen);
static int hts221_ioctl(FAR struct file *filep, int cmd, unsigned long arg);
#ifndef CONFIG_DISABLE_POLL
static int hts221_poll(FAR struct file *filep, FAR struct pollfd *fds,
                       bool setup);
#endif

/****************************************************************************
* Private Data
****************************************************************************/

struct hts221_dev_t
  {
    struct i2c_dev_s *i2c;
    uint8_t addr;
    hts221_config_t *config;
    sem_t devsem;
    volatile bool int_pending;
#ifndef CONFIG_DISABLE_POLL
    struct pollfd *fds[CONFIG_HTS221_NPOLLWAITERS];
#endif
    struct
      {
        int16_t t0_out;
        int16_t t1_out;
        int16_t h0_t0_out;
        int16_t h1_t0_out;
        unsigned int t0_x8:10;
        unsigned int t1_x8:10;
        uint8_t h0_x2;
        uint8_t h1_x2;
      } calib;
  };

static const struct file_operations g_humidityops = {
  hts221_open,
  hts221_close,
  hts221_read,
  hts221_write,
  0,
  hts221_ioctl,
#ifndef CONFIG_DISABLE_POLL
  hts221_poll
#endif
};

static struct hts221_dev_t *g_humid_data;

static int hts221_do_transfer(FAR struct hts221_dev_t *dev,
                              FAR struct i2c_msg_s *msgv,
                              size_t nmsg)
{
  int ret = -EIO;
  int retries;

  for (retries = 0; retries < HTS221_I2C_RETRIES; retries++)
    {
      ret = I2C_TRANSFER(dev->i2c, msgv, nmsg);
      if (ret >= 0)
        {
          return 0;
        }
      else
        {
          /* Some error. Try to reset I2C bus and keep trying. */
#ifdef CONFIG_I2C_RESET
          ret = up_i2creset(dev->i2c);
          if (ret < 0)
            {
              hts221_dbg("up_i2creset failed: %d\n", ret);
              return ret;
            }
#endif
          continue;
        }
    }

  hts221_dbg("xfer failed: %d\n", ret);
  return ret;
}

static int32_t hts221_write_reg8(FAR struct hts221_dev_t *dev,
                                 const uint8_t * command)
{
  struct i2c_msg_s msgv[2] =
  { {
    .addr   = dev->addr,
    .flags  = 0,
    .buffer = (void *)&command[0],
    .length = 1
  }, {
    .addr   = dev->addr,
    .flags  = I2C_M_NORESTART,
    .buffer = (void *)&command[1],
    .length = 1
  } };

  return hts221_do_transfer(dev, msgv, 2);
}

static int hts221_read_reg(struct hts221_dev_t *dev, const uint8_t * command,
                           uint8_t * value)
{
  struct i2c_msg_s msgv[2] =
  { {
    .addr   = dev->addr,
    .flags  = 0,
    .buffer = (void *)command,
    .length = 1
  }, {
    .addr   = dev->addr,
    .flags  = I2C_M_READ,
    .buffer = value,
    .length = 1
  } };

  return hts221_do_transfer(dev, msgv, 2);
}

static int hts221_get_id(FAR struct hts221_dev_t *priv, uint8_t * value)
{
  int ret = OK;
  uint8_t cmd = HTS221_WHO_AM_I;

  ret = hts221_read_reg(priv, &cmd, value);

  hts221_lldbg("Who am I request: 0x%02X\n", *value);

  return ret;
}

static int hts221_cfgr_resolution(FAR struct hts221_dev_t *priv,
                                  hts221_settings_t * settings)
{
  int ret;
  uint8_t value;
  const uint8_t addr = HTS221_AV_CONF;
  uint8_t regval = 0;
  uint8_t cmd[2] = { 0 };
  const uint8_t mask = 0x3F;

  ret = hts221_read_reg(priv, &addr, &regval);
  hts221_lldbg("Default resolution: 0x%02X\n", regval);
  if (ret < 0)
    {
      return ERROR;
    }

  value = (uint8_t)settings->humid_resol | ((uint8_t)settings->temp_resol << 3);
  regval &= ~mask;
  cmd[0] = addr;
  cmd[1] = regval | value;
  hts221_lldbg("New resolution: 0x%02X\n", cmd[1]);

  ret = hts221_write_reg8(priv, cmd);

  hts221_lldbg("Resolution changed: temp=%d humid=%d ret=%d\n",
               settings->temp_resol, settings->humid_resol, ret);

  return ret;
}

static int hts221_config_ctrl_reg3(FAR struct hts221_dev_t *priv,
                                   hts221_settings_t * settings)
{
  int ret = OK;
  uint8_t regval = 0;
  uint8_t addr = HTS221_CTRL_REG3;
  const uint8_t mask = 0xC4;
  uint8_t data_to_write[2] = { 0 };

  ret = hts221_read_reg(priv, &addr, &regval);
  hts221_lldbg("CTRL_REG3: 0x%02X\n", regval);
  if (ret < 0)
    {
      return ERROR;
    }

  regval &= ~mask;
  regval |= (uint8_t) (settings->is_high_edge ? 0 : HTS221_CTRL_REG3_DRDY_L_H);
  regval |= (uint8_t) (settings->is_open_drain ? HTS221_CTRL_REG3_PP_OD : 0);
  regval |= (uint8_t) (settings->is_data_rdy ? HTS221_CTRL_REG3_DRDY_EN : 0);

  data_to_write[0] = addr;
  data_to_write[1] = regval;

  ret = hts221_write_reg8(priv, data_to_write);

  return ret;
}

static int hts221_config_ctrl_reg2(FAR struct hts221_dev_t *priv,
                                   hts221_settings_t * settings)
{
  int ret = OK;
  uint8_t regval = 0;
  uint8_t addr = HTS221_CTRL_REG2;
  const uint8_t mask = 0x80;
  uint8_t data_to_write[2] = { 0 };

  ret = hts221_read_reg(priv, &addr, &regval);
  hts221_lldbg("CTRL_REG2: 0x%02X\n", regval);
  if (ret < 0)
    {
      return ERROR;
    }

  regval &= ~mask;
  regval |= (uint8_t) (settings->is_boot ? HTS221_CTRL_REG2_BOOT : 0);

  data_to_write[0] = addr;
  data_to_write[1] = regval;

  ret = hts221_write_reg8(priv, data_to_write);

  return ret;
}

static int hts221_config_ctrl_reg1(FAR struct hts221_dev_t *priv,
                                   hts221_settings_t * settings)
{
  int ret = OK;
  uint8_t regval = 0;
  uint8_t addr = HTS221_CTRL_REG1;
  const uint8_t mask = 0x87;
  uint8_t data_to_write[2] = { 0 };

  ret = hts221_read_reg(priv, &addr, &regval);
  hts221_lldbg("CTRL_REG1: 0x%02X\n", regval);
  if (ret < 0)
    {
      return ERROR;
    }

  regval &= ~mask;
  regval |= (uint8_t) (settings->odr & 0xFF);
  regval |= (uint8_t) (settings->is_bdu ? HTS221_CTRL_REG1_BDU : 0);
  //regval |= (uint8_t) HTS221_CTRL_REG1_PD;

  data_to_write[0] = addr;
  data_to_write[1] = regval;

  ret = hts221_write_reg8(priv, data_to_write);

  return ret;
}

static int hts221_power_on_off(FAR struct hts221_dev_t *priv, bool on)
{
  int ret = OK;
  uint8_t regval = 0;
  uint8_t addr = HTS221_CTRL_REG1;
  uint8_t data_to_write[2];

  ret = hts221_read_reg(priv, &addr, &regval);
  hts221_lldbg("CTRL_REG1: 0x%02X\n", regval);
  if (ret < 0)
    {
      return ret;
    }

  if (on)
    {
      regval |= HTS221_CTRL_REG1_PD;
    }
  else
    {
      regval &= ~HTS221_CTRL_REG1_PD;
    }
  data_to_write[0] = addr;
  data_to_write[1] = regval;

  ret = hts221_write_reg8(priv, data_to_write);

  return ret;
}

static int hts221_config(FAR struct hts221_dev_t *priv,
                         hts221_settings_t * cfgr)
{
  int ret = OK;

  ret = hts221_cfgr_resolution(priv, cfgr);
  if (ret < 0)
    {
      return ERROR;
    }

  ret = hts221_config_ctrl_reg3(priv, cfgr);
  if (ret < 0)
    {
      return ERROR;
    }

  ret = hts221_config_ctrl_reg2(priv, cfgr);
  if (ret < 0)
    {
      return ERROR;
    }

  ret = hts221_config_ctrl_reg1(priv, cfgr);
  if (ret < 0)
    {
      return ERROR;
    }

  return ret;
}

static int hts221_start_conversion(FAR struct hts221_dev_t *priv)
{
  int ret;
  uint8_t addr = HTS221_CTRL_REG2;
  uint8_t data_to_write[2];

  ret = hts221_power_on_off(priv, true);
  if (ret < 0)
    {
      return ERROR;
    }

  data_to_write[0] = addr;
  data_to_write[1] = (uint8_t) HTS221_CTRL_REG2_ONE_SHOT;

  ret = hts221_write_reg8(priv, data_to_write);
  if (ret < 0)
    {
      hts221_lldbg("Cannot start conversion\n");
      ret = ERROR;
    }

  return ret;
}

static int hts221_check_status(FAR struct hts221_dev_t *priv,
                               hts221_status_t * status)
{
  int ret = OK;
  uint8_t addr = HTS221_STATUS_REG;
  const uint8_t humid_mask = 0x02;
  const uint8_t temp_mask = 0x01;
  uint8_t regval = 0;

  ret = hts221_read_reg(priv, &addr, &regval);
  if (ret < 0)
    {
      return ERROR;
    }

  status->is_humid_ready = ((regval & humid_mask) ? true : false);
  status->is_temp_ready = ((regval & temp_mask) ? true : false);

  return ret;
}

static int hts221_read_raw_data(FAR struct hts221_dev_t *priv,
                                hts221_raw_data_t * data)
{
  int ret = OK;
  uint8_t addr_humid_low = HTS221_HUM_OUT_L;
  uint8_t addr_humid_high = HTS221_HUM_OUT_H;
  uint8_t addr_temp_low = HTS221_TEMP_OUT_L;
  uint8_t addr_temp_high = HTS221_TEMP_OUT_H;
  uint32_t flags;

  ret = hts221_read_reg(priv, &addr_humid_low, &data->humid_low_bits);
  if (ret < 0)
    {
      return ERROR;
    }

  ret = hts221_read_reg(priv, &addr_humid_high, &data->humid_high_bits);
  if (ret < 0)
    {
      return ERROR;
    }

  ret = hts221_read_reg(priv, &addr_temp_low, &data->temp_low_bits);
  if (ret < 0)
    {
      return ERROR;
    }

  ret = hts221_read_reg(priv, &addr_temp_high, &data->temp_high_bits);
  if (ret < 0)
    {
      return ERROR;
    }

  /* Add low-order bytes to entropy pool. */

  add_sensor_randomness(((uint32_t)data->humid_low_bits << 8) | data->temp_low_bits);

  flags = irqsave();
  priv->int_pending = false;
  irqrestore(flags);

  hts221_lldbg("Humid: 0x%02X, 0x%02X Temper: 0x%02X 0x%02X\n",
               data->humid_high_bits, data->humid_low_bits,
               data->temp_high_bits, data->temp_low_bits);

  return ret;
}

static int hts221_load_calibration_data(FAR struct hts221_dev_t *priv)
{
  int ret;
  uint8_t addr;
  uint8_t t0_degc_x8 = 0;
  uint8_t t1_degc_x8 = 0;
  uint8_t t1_t0_msb = 0;
  uint8_t t0_out_lsb = 0;
  uint8_t t0_out_msb = 0;
  uint8_t t1_out_lsb = 0;
  uint8_t t1_out_msb = 0;
  uint8_t h0_rh_x2 = 0;
  uint8_t h1_rh_x2 = 0;
  uint8_t h0t0_out_lsb = 0;
  uint8_t h0t0_out_msb = 0;
  uint8_t h1t0_out_lsb = 0;
  uint8_t h1t0_out_msb = 0;

  addr = HTS221_CALIB_T0_DEGC_X8;
  ret = hts221_read_reg(priv, &addr, &t0_degc_x8);
  if (ret < 0)
    return ret;

  addr = HTS221_CALIB_T1_DEGC_X8;
  ret = hts221_read_reg(priv, &addr, &t1_degc_x8);
  if (ret < 0)
    return ret;

  addr = HTS221_CALIB_T1_T0_MSB;
  ret = hts221_read_reg(priv, &addr, &t1_t0_msb);
  if (ret < 0)
    return ret;

  addr = HTS221_CALIB_T0_OUT_L;
  ret = hts221_read_reg(priv, &addr, &t0_out_lsb);
  if (ret < 0)
    return ret;

  addr = HTS221_CALIB_T0_OUT_H;
  ret = hts221_read_reg(priv, &addr, &t0_out_msb);
  if (ret < 0)
    return ret;

  addr = HTS221_CALIB_T1_OUT_L;
  ret = hts221_read_reg(priv, &addr, &t1_out_lsb);
  if (ret < 0)
    return ret;

  addr = HTS221_CALIB_T1_OUT_H;
  ret = hts221_read_reg(priv, &addr, &t1_out_msb);
  if (ret < 0)
    return ret;

  addr = HTS221_CALIB_H0_RH_X2;
  ret = hts221_read_reg(priv, &addr, &h0_rh_x2);
  if (ret < 0)
    return ret;

  addr = HTS221_CALIB_H1_RH_X2;
  ret = hts221_read_reg(priv, &addr, &h1_rh_x2);
  if (ret < 0)
    return ret;

  addr = HTS221_CALIB_H0T0_OUT_L;
  ret = hts221_read_reg(priv, &addr, &h0t0_out_lsb);
  if (ret < 0)
    return ret;

  addr = HTS221_CALIB_H0T0_OUT_H;
  ret = hts221_read_reg(priv, &addr, &h0t0_out_msb);
  if (ret < 0)
    return ret;

  addr = HTS221_CALIB_H1T0_OUT_L;
  ret = hts221_read_reg(priv, &addr, &h1t0_out_lsb);
  if (ret < 0)
    return ret;

  addr = HTS221_CALIB_H1T0_OUT_H;
  ret = hts221_read_reg(priv, &addr, &h1t0_out_msb);
  if (ret < 0)
    return ret;

  priv->calib.t0_x8 = t0_degc_x8 | ((t1_t0_msb & 0x3) << 8);
  priv->calib.t1_x8 = t1_degc_x8 | ((t1_t0_msb & (0x3 << 2)) << (8 - 2));
  priv->calib.t0_out = (uint16_t) (t0_out_lsb | (t0_out_msb << 8));
  priv->calib.t1_out = (uint16_t) (t1_out_lsb | (t1_out_msb << 8));
  priv->calib.h0_x2 = h0_rh_x2;
  priv->calib.h1_x2 = h1_rh_x2;
  priv->calib.h0_t0_out = (uint16_t) (h0t0_out_lsb | (h0t0_out_msb << 8));
  priv->calib.h1_t0_out = (uint16_t) (h1t0_out_lsb | (h1t0_out_msb << 8));

  hts221_lldbg("calib.t0_x8: %d\n", priv->calib.t0_x8);
  hts221_lldbg("calib.t1_x8: %d\n", priv->calib.t1_x8);
  hts221_lldbg("calib.t0_out: %d\n", priv->calib.t0_out);
  hts221_lldbg("calib.t1_out: %d\n", priv->calib.t1_out);
  hts221_lldbg("calib.h0_x2: %d\n", priv->calib.h0_x2);
  hts221_lldbg("calib.h1_x2: %d\n", priv->calib.h1_x2);
  hts221_lldbg("calib.h0_t0_out: %d\n", priv->calib.h0_t0_out);
  hts221_lldbg("calib.h1_t0_out: %d\n", priv->calib.h1_t0_out);

  /* As calibration coefficients are unique to each sensor device,
   * they are a good candidate to be added to entropy pool.
   */

  up_rngaddentropy((uint32_t *)&priv->calib, sizeof(priv->calib) / sizeof(uint32_t));

  return OK;
}

static int hts221_calculate_temperature(FAR struct hts221_dev_t *priv,
                                        int *temperature,
                                        hts221_raw_data_t * raw_data)
{
  int16_t t_out = (raw_data->temp_high_bits << 8) | raw_data->temp_low_bits;
  int x0 = priv->calib.t0_out;
  int x1 = priv->calib.t1_out;
  int y0 = priv->calib.t0_x8;
  int y1 = priv->calib.t1_x8;
  int x = t_out;
  int64_t y;
  int x1_x0_diff;

  x1_x0_diff = x1 - x0;

  y = (y0 * x1_x0_diff + (y1 - y0) * (x - x0));
  y *= HTS221_TEMPERATURE_PRECISION;
  y /= x1_x0_diff * 8;

  *temperature = (int)y;

  hts221_lldbg("Interpolation data temper: %d\n", *temperature);

  return OK;
}

static int hts221_calculate_humidity(FAR struct hts221_dev_t *priv,
                                     unsigned int *humidity,
                                     hts221_raw_data_t * raw_data)
{
  int16_t h_out = (raw_data->humid_high_bits << 8) | raw_data->humid_low_bits;
  int x0 = priv->calib.h0_t0_out;
  int x1 = priv->calib.h1_t0_out;
  int y0 = priv->calib.h0_x2;
  int y1 = priv->calib.h1_x2;
  int x = h_out;
  int64_t y;
  int x1_x0_diff;

  x1_x0_diff = x1 - x0;

  y = (y0 * x1_x0_diff + (y1 - y0) * (x - x0));
  y *= HTS221_HUMIDITY_PRECISION;
  y /= x1_x0_diff * 2;

  *humidity = (int)y;

  hts221_lldbg("Interpolation data humidity: %d\n", *humidity);

  return OK;
}

static int hts221_read_convert_data(FAR struct hts221_dev_t *priv,
                                    hts221_conv_data_t * data)
{
  int ret = OK;
  hts221_raw_data_t raw_data;

  ret = hts221_read_raw_data(priv, &raw_data);
  if (ret < 0)
    {
      return ERROR;
    }

  ret = hts221_calculate_temperature(priv, &data->temperature, &raw_data);
  if (ret < 0)
    {
      return ERROR;
    }

  hts221_lldbg("Temperature calculated\n");

  ret = hts221_calculate_humidity(priv, &data->humidity, &raw_data);
  if (ret < 0)
    {
      return ERROR;
    }

  hts221_lldbg("Humidity calculated\n");

  return ret;
}

#if CONFIG_TRACES_HTS221_HUMIDITY
static int hts221_dump_registers(FAR struct hts221_dev_t *priv)
{
  int ret = OK;
  uint8_t av_addr = HTS221_AV_CONF;
  uint8_t ctrl_reg1_addr = HTS221_CTRL_REG1;
  uint8_t ctrl_reg2_addr = HTS221_CTRL_REG2;
  uint8_t ctrl_reg3_addr = HTS221_CTRL_REG3;
  uint8_t regval = 0;

  ret = hts221_read_reg(priv, &av_addr, &regval);
  if (ret < 0)
    {
      return ERROR;
    }
  hts221_lldbg("AV_CONF_REG: 0x%02X\n", regval);

  ret = hts221_read_reg(priv, &ctrl_reg1_addr, &regval);
  if (ret < 0)
    {
      return ERROR;
    }
  hts221_lldbg("CTRL_REG_1: 0x%02X\n", regval);

  ret = hts221_read_reg(priv, &ctrl_reg2_addr, &regval);
  if (ret < 0)
    {
      return ERROR;
    }
  hts221_lldbg("CTRL_REG_2: 0x%02X\n", regval);

  ret = hts221_read_reg(priv, &ctrl_reg3_addr, &regval);
  if (ret < 0)
    {
      return ERROR;
    }
  hts221_lldbg("CTRL_REG_3: 0x%02X\n", regval);

  return ret;
}
#endif

static int hts221_open(FAR struct file *filep)
{
  FAR struct inode *inode = filep->f_inode;
  FAR struct hts221_dev_t *priv = inode->i_private;

  while (sem_wait(&priv->devsem) != 0)
    {
      assert(errno == EINTR);
    }

  priv->config->irq_enable(priv->config, true);

  sem_post(&priv->devsem);
  hts221_dbg("Sensor is powered on\n");

  return OK;
}

static int hts221_close(FAR struct file *filep)
{
  FAR struct inode *inode = filep->f_inode;
  FAR struct hts221_dev_t *priv = inode->i_private;
  int ret = OK;

  while (sem_wait(&priv->devsem) != 0)
    {
      assert(errno == EINTR);
    }

  priv->config->irq_enable(priv->config, false);
  ret = hts221_power_on_off(priv, false);

  sem_post(&priv->devsem);
  hts221_dbg("CLOSED\n");
  return ret;
}

static ssize_t hts221_read(FAR struct file *filep, FAR char *buffer,
                           size_t buflen)
{
  ssize_t length = 0;
  return length;
}

static ssize_t hts221_write(FAR struct file *filep, FAR const char *buffer,
                            size_t buflen)
{
  ssize_t length = 0;
  return length;
}

static int hts221_ioctl(FAR struct file *filep, int cmd, unsigned long arg)
{
  FAR struct inode *inode = filep->f_inode;
  FAR struct hts221_dev_t *priv = inode->i_private;
  int32_t ret = 0;

  while (sem_wait(&priv->devsem) != 0)
    {
      assert(errno == EINTR);
    }

  switch (cmd)
    {
    case HTS221_IOC_GET_DEV_ID:
      ret = hts221_get_id(priv, (uint8_t *) arg);
      break;
    case HTS221_IOC_CFGR:
      ret = hts221_config(priv, (hts221_settings_t *) arg);
      break;
    case HTS221_IOC_START_CONVERSION:
      ret = hts221_start_conversion(priv);
      break;
    case HTS221_IOC_CHECK_STATUS_REG:
      ret = hts221_check_status(priv, (hts221_status_t *) arg);
      break;
    case HTS221_IOC_READ_RAW_DATA:
      ret = hts221_read_raw_data(priv, (hts221_raw_data_t *) arg);
      break;
#if CONFIG_TRACES_HTS221_HUMIDITY
    case HTS221_IOC_DUMP_REGS:
      ret = hts221_dump_registers(priv);
      break;
#endif
    case HTS221_IOC_READ_CONVERT_DATA:
      ret = hts221_read_convert_data(priv, (hts221_conv_data_t *) arg);
      break;
    default:
      ret = -EINVAL;
      break;
    }

  sem_post(&priv->devsem);
  return ret;
}

#ifndef CONFIG_DISABLE_POLL
static bool hts221_sample(FAR struct hts221_dev_t *priv)
{
  int ret;
  hts221_status_t status = {
    .is_humid_ready = false,
    .is_temp_ready = false
  };

  ret = hts221_check_status(priv, &status);
  if (ret < 0)
  {
    return false;
  }

  return status.is_humid_ready || status.is_temp_ready;
}

static void hts221_notify(FAR struct hts221_dev_t *priv)
{
  DEBUGASSERT(priv != NULL);

  int i;

  /* If there are threads waiting on poll() for data to become available,
   * then wake them up now.  NOTE: we wake up all waiting threads because we
   * do not know that they are going to do.  If they all try to read the data,
   * then some make end up blocking after all. */
  for (i = 0; i < CONFIG_HTS221_NPOLLWAITERS; i++)
    {
      struct pollfd *fds = priv->fds[i];
      if (fds)
        {
          fds->revents |= POLLIN;
          hts221_lldbg("Report events: %02x\n", fds->revents);
          sem_post(fds->sem);
        }
    }
}

static int hts221_poll(FAR struct file *filep, FAR struct pollfd *fds,
                       bool setup)
{
  FAR struct inode *inode;
  FAR struct hts221_dev_t *priv;
  int ret = OK;
  int i;
  uint32_t flags;

  DEBUGASSERT(filep && fds);
  inode = filep->f_inode;

  DEBUGASSERT(inode && inode->i_private);
  priv = (FAR struct hts221_dev_t *)inode->i_private;

  while (sem_wait(&priv->devsem) != 0)
    {
      assert(errno == EINTR);
    }

  if (setup)
    {
      /* Ignore waits that do not include POLLIN */

      if ((fds->events & POLLIN) == 0)
        {
          ret = -EDEADLK;
          goto out;
        }

      /* This is a request to set up the poll.  Find an available slot for the
       * poll structure reference */

      for (i = 0; i < CONFIG_HTS221_NPOLLWAITERS; i++)
        {
          /* Find an available slot */

          if (!priv->fds[i])
            {
              /* Bind the poll structure and this slot */

              priv->fds[i] = fds;
              fds->priv = &priv->fds[i];
              break;
            }
        }

      if (i >= CONFIG_HTS221_NPOLLWAITERS)
        {
          fds->priv = NULL;
          ret = -EBUSY;
          goto out;
        }
      flags = irqsave();
      if (priv->int_pending || hts221_sample(priv))
        {
          hts221_notify(priv);
        }
      irqrestore(flags);
    }
  else if (fds->priv)
    {
      /* This is a request to tear down the poll. */

      struct pollfd **slot = (struct pollfd **)fds->priv;
      DEBUGASSERT(slot != NULL);

      /* Remove all memory of the poll setup */

      *slot = NULL;
      fds->priv = NULL;
    }
out:
  sem_post(&priv->devsem);
  return ret;
}
#endif /* !CONFIG_DISABLE_POLL */

static int hts221_int_handler(int irq, FAR void *context)
{
  if (!g_humid_data)
    return OK;

  g_humid_data->int_pending = true;
  hts221_lldbg("Hts221 interrupt\n");
#ifndef CONFIG_DISABLE_POLL
  hts221_notify(g_humid_data);
#endif

  return OK;
}

int hts221_register(FAR const char *devpath, FAR struct i2c_dev_s *i2c,
                    uint8_t addr, hts221_config_t * config)
{
  int ret = 0;
  struct hts221_dev_t *priv;

  priv = (struct hts221_dev_t *)kmm_zalloc(sizeof(struct hts221_dev_t));

  if (!priv)
    {
      hts221_dbg("Memory cannot be allocated for hts221 sensor");
      return -ENOMEM;
    }

  g_humid_data = priv;
  priv->addr = addr;
  priv->i2c = i2c;
  priv->config = config;
  sem_init(&priv->devsem, 0, 1);

  ret = hts221_load_calibration_data(priv);
  if (ret < 0)
    {
      kmm_free(priv);
      hts221_dbg("Cannot calibrate hts221 sensor\n");
      return -EAGAIN;
    }

  ret = register_driver(devpath, &g_humidityops, 0666, priv);

  hts221_dbg("Registered with %d\n", ret);

  if (ret < 0)
    {
      kmm_free(priv);
      hts221_dbg("Error occurred during the driver registering\n");
      return ret;
    }

  priv->config->irq_clear(priv->config);
  priv->config->irq_attach(priv->config, hts221_int_handler);
  priv->config->irq_enable(priv->config, false);

  return OK;
}