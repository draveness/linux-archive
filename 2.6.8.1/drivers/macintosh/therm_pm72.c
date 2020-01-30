/*
 * Device driver for the thermostats & fan controller of  the
 * Apple G5 "PowerMac7,2" desktop machines.
 *
 * (c) Copyright IBM Corp. 2003-2004
 *
 * Maintained by: Benjamin Herrenschmidt
 *                <benh@kernel.crashing.org>
 * 
 *
 * The algorithm used is the PID control algorithm, used the same
 * way the published Darwin code does, using the same values that
 * are present in the Darwin 7.0 snapshot property lists.
 *
 * As far as the CPUs control loops are concerned, I use the
 * calibration & PID constants provided by the EEPROM,
 * I do _not_ embed any value from the property lists, as the ones
 * provided by Darwin 7.0 seem to always have an older version that
 * what I've seen on the actual computers.
 * It would be interesting to verify that though. Darwin has a
 * version code of 1.0.0d11 for all control loops it seems, while
 * so far, the machines EEPROMs contain a dataset versioned 1.0.0f
 *
 * Darwin doesn't provide source to all parts, some missing
 * bits like the AppleFCU driver or the actual scale of some
 * of the values returned by sensors had to be "guessed" some
 * way... or based on what Open Firmware does.
 *
 * I didn't yet figure out how to get the slots power consumption
 * out of the FCU, so that part has not been implemented yet and
 * the slots fan is set to a fixed 50% PWM, hoping this value is
 * safe enough ...
 *
 * Note: I have observed strange oscillations of the CPU control
 * loop on a dual G5 here. When idle, the CPU exhaust fan tend to
 * oscillates slowly (over several minutes) between the minimum
 * of 300RPMs and approx. 1000 RPMs. I don't know what is causing
 * this, it could be some incorrect constant or an error in the
 * way I ported the algorithm, or it could be just normal. I
 * don't have full understanding on the way Apple tweaked the PID
 * algorithm for the CPU control, it is definitely not a standard
 * implementation...
 *
 * TODO:  - Check MPU structure version/signature
 *        - Add things like /sbin/overtemp for non-critical
 *          overtemp conditions so userland can take some policy
 *          decisions, like slewing down CPUs
 *	  - Deal with fan and i2c failures in a better way
 *
 * History:
 *
 *  Nov. 13, 2003 : 0.5
 *	- First release
 *
 *  Nov. 14, 2003 : 0.6
 *	- Read fan speed from FCU, low level fan routines now deal
 *	  with errors & check fan status, though higher level don't
 *	  do much.
 *	- Move a bunch of definitions to .h file
 *
 *  Nov. 18, 2003 : 0.7
 *	- Fix build on ppc64 kernel
 *	- Move back statics definitions to .c file
 *	- Avoid calling schedule_timeout with a negative number
 *
 *  Dec. 18, 2003 : 0.8
 *	- Fix typo when reading back fan speed on 2 CPU machines
 *
 *  Mar. 11, 2004 : 0.9
 *	- Rework code accessing the ADC chips, make it more robust and
 *	  closer to the chip spec. Also make sure it is configured properly,
 *        I've seen yet unexplained cases where on startup, I would have stale
 *        values in the configuration register
 *	- Switch back to use of target fan speed for PID, thus lowering
 *        pressure on i2c
 */

#include <linux/config.h>
#include <linux/types.h>
#include <linux/module.h>
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/delay.h>
#include <linux/sched.h>
#include <linux/i2c.h>
#include <linux/slab.h>
#include <linux/init.h>
#include <linux/spinlock.h>
#include <linux/smp_lock.h>
#include <linux/wait.h>
#include <linux/reboot.h>
#include <linux/kmod.h>
#include <linux/i2c.h>
#include <linux/i2c-dev.h>
#include <asm/prom.h>
#include <asm/machdep.h>
#include <asm/io.h>
#include <asm/system.h>
#include <asm/sections.h>
#include <asm/of_device.h>

#include "therm_pm72.h"

#define VERSION "0.9"

#undef DEBUG

#ifdef DEBUG
#define DBG(args...)	printk(args)
#else
#define DBG(args...)	do { } while(0)
#endif


/*
 * Driver statics
 */

static struct of_device *		of_dev;
static struct i2c_adapter *		u3_0;
static struct i2c_adapter *		u3_1;
static struct i2c_client *		fcu;
static struct cpu_pid_state		cpu_state[2];
static struct backside_pid_state	backside_state;
static struct drives_pid_state		drives_state;
static int				state;
static int				cpu_count;
static pid_t				ctrl_task;
static struct completion		ctrl_complete;
static int				critical_state;
static DECLARE_MUTEX(driver_lock);

/*
 * i2c_driver structure to attach to the host i2c controller
 */

static int therm_pm72_attach(struct i2c_adapter *adapter);
static int therm_pm72_detach(struct i2c_adapter *adapter);

static struct i2c_driver therm_pm72_driver =
{
	.name		= "therm_pm72",
	.id		= 0xDEADBEEF,
	.flags		= I2C_DF_NOTIFY,
	.attach_adapter	= therm_pm72_attach,
	.detach_adapter	= therm_pm72_detach,
};

/*
 * Utility function to create an i2c_client structure and
 * attach it to one of u3 adapters
 */
static struct i2c_client *attach_i2c_chip(int id, const char *name)
{
	struct i2c_client *clt;
	struct i2c_adapter *adap;

	if (id & 0x100)
		adap = u3_1;
	else
		adap = u3_0;
	if (adap == NULL)
		return NULL;

	clt = kmalloc(sizeof(struct i2c_client), GFP_KERNEL);
	if (clt == NULL)
		return NULL;
	memset(clt, 0, sizeof(struct i2c_client));

	clt->addr = (id >> 1) & 0x7f;
	clt->adapter = adap;
	clt->driver = &therm_pm72_driver;
	clt->id = 0xDEADBEEF;
	strncpy(clt->name, name, I2C_NAME_SIZE-1);

	if (i2c_attach_client(clt)) {
		printk(KERN_ERR "therm_pm72: Failed to attach to i2c ID 0x%x\n", id);
		kfree(clt);
		return NULL;
	}
	return clt;
}

/*
 * Utility function to get rid of the i2c_client structure
 * (will also detach from the adapter hopepfully)
 */
static void detach_i2c_chip(struct i2c_client *clt)
{
	i2c_detach_client(clt);
	kfree(clt);
}

/*
 * Here are the i2c chip access wrappers
 */

static void initialize_adc(struct cpu_pid_state *state)
{
	int rc;
	u8 buf[2];

	/* Read ADC the configuration register and cache it. We
	 * also make sure Config2 contains proper values, I've seen
	 * cases where we got stale grabage in there, thus preventing
	 * proper reading of conv. values
	 */

	/* Clear Config2 */
	buf[0] = 5;
	buf[1] = 0;
	i2c_master_send(state->monitor, buf, 2);

	/* Read & cache Config1 */
	buf[0] = 1;
	rc = i2c_master_send(state->monitor, buf, 1);
	if (rc > 0) {
		rc = i2c_master_recv(state->monitor, buf, 1);
		if (rc > 0) {
			state->adc_config = buf[0];
			DBG("ADC config reg: %02x\n", state->adc_config);
			/* Disable shutdown mode */
		       	state->adc_config &= 0xfe;
			buf[0] = 1;
			buf[1] = state->adc_config;
			rc = i2c_master_send(state->monitor, buf, 2);
		}
	}
	if (rc <= 0)
		printk(KERN_ERR "therm_pm72: Error reading ADC config"
		       " register !\n");
}

static int read_smon_adc(struct cpu_pid_state *state, int chan)
{
	int rc, data, tries = 0;
	u8 buf[2];

	for (;;) {
		/* Set channel */
		buf[0] = 1;
		buf[1] = (state->adc_config & 0x1f) | (chan << 5);
		rc = i2c_master_send(state->monitor, buf, 2);
		if (rc <= 0)
			goto error;
		/* Wait for convertion */
		msleep(1);
		/* Switch to data register */
		buf[0] = 4;
		rc = i2c_master_send(state->monitor, buf, 1);
		if (rc <= 0)
			goto error;
		/* Read result */
		rc = i2c_master_recv(state->monitor, buf, 2);
		if (rc < 0)
			goto error;
		data = ((u16)buf[0]) << 8 | (u16)buf[1];
		return data >> 6;
	error:
		DBG("Error reading ADC, retrying...\n");
		if (++tries > 10) {
			printk(KERN_ERR "therm_pm72: Error reading ADC !\n");
			return -1;
		}
		msleep(10);
	}
}

static int fan_read_reg(int reg, unsigned char *buf, int nb)
{
	int tries, nr, nw;

	buf[0] = reg;
	tries = 0;
	for (;;) {
		nw = i2c_master_send(fcu, buf, 1);
		if (nw > 0 || (nw < 0 && nw != -EIO) || tries >= 100)
			break;
		msleep(10);
		++tries;
	}
	if (nw <= 0) {
		printk(KERN_ERR "Failure writing address to FCU: %d", nw);
		return -EIO;
	}
	tries = 0;
	for (;;) {
		nr = i2c_master_recv(fcu, buf, nb);
		if (nr > 0 || (nr < 0 && nr != ENODEV) || tries >= 100)
			break;
		msleep(10);
		++tries;
	}
	if (nr <= 0)
		printk(KERN_ERR "Failure reading data from FCU: %d", nw);
	return nr;
}

static int fan_write_reg(int reg, const unsigned char *ptr, int nb)
{
	int tries, nw;
	unsigned char buf[16];

	buf[0] = reg;
	memcpy(buf+1, ptr, nb);
	++nb;
	tries = 0;
	for (;;) {
		nw = i2c_master_send(fcu, buf, nb);
		if (nw > 0 || (nw < 0 && nw != EIO) || tries >= 100)
			break;
		msleep(10);
		++tries;
	}
	if (nw < 0)
		printk(KERN_ERR "Failure writing to FCU: %d", nw);
	return nw;
}

static int start_fcu(void)
{
	unsigned char buf = 0xff;
	int rc;

	rc = fan_write_reg(0xe, &buf, 1);
	if (rc < 0)
		return -EIO;
	rc = fan_write_reg(0x2e, &buf, 1);
	if (rc < 0)
		return -EIO;
	return 0;
}

static int set_rpm_fan(int fan, int rpm)
{
	unsigned char buf[2];
	int rc;

	if (rpm < 300)
		rpm = 300;
	else if (rpm > 8191)
		rpm = 8191;
	buf[0] = rpm >> 5;
	buf[1] = rpm << 3;
	rc = fan_write_reg(0x10 + (fan * 2), buf, 2);
	if (rc < 0)
		return -EIO;
	return 0;
}

static int get_rpm_fan(int fan, int programmed)
{
	unsigned char failure;
	unsigned char active;
	unsigned char buf[2];
	int rc, reg_base;

	rc = fan_read_reg(0xb, &failure, 1);
	if (rc != 1)
		return -EIO;
	if ((failure & (1 << fan)) != 0)
		return -EFAULT;
	rc = fan_read_reg(0xd, &active, 1);
	if (rc != 1)
		return -EIO;
	if ((active & (1 << fan)) == 0)
		return -ENXIO;

	/* Programmed value or real current speed */
	reg_base = programmed ? 0x10 : 0x11;
	rc = fan_read_reg(reg_base + (fan * 2), buf, 2);
	if (rc != 2)
		return -EIO;

	return (buf[0] << 5) | buf[1] >> 3;
}

static int set_pwm_fan(int fan, int pwm)
{
	unsigned char buf[2];
	int rc;

	if (pwm < 10)
		pwm = 10;
	else if (pwm > 100)
		pwm = 100;
	pwm = (pwm * 2559) / 1000;
	buf[0] = pwm;
	rc = fan_write_reg(0x30 + (fan * 2), buf, 1);
	if (rc < 0)
		return rc;
	return 0;
}

static int get_pwm_fan(int fan)
{
	unsigned char failure;
	unsigned char active;
	unsigned char buf[2];
	int rc;

	rc = fan_read_reg(0x2b, &failure, 1);
	if (rc != 1)
		return -EIO;
	if ((failure & (1 << fan)) != 0)
		return -EFAULT;
	rc = fan_read_reg(0x2d, &active, 1);
	if (rc != 1)
		return -EIO;
	if ((active & (1 << fan)) == 0)
		return -ENXIO;

	/* Programmed value or real current speed */
	rc = fan_read_reg(0x30 + (fan * 2), buf, 1);
	if (rc != 1)
		return -EIO;

	return (buf[0] * 1000) / 2559;
}

/*
 * Utility routine to read the CPU calibration EEPROM data
 * from the device-tree
 */
static int read_eeprom(int cpu, struct mpu_data *out)
{
	struct device_node *np;
	char nodename[64];
	u8 *data;
	int len;

	/* prom.c routine for finding a node by path is a bit brain dead
	 * and requires exact @xxx unit numbers. This is a bit ugly but
	 * will work for these machines
	 */
	sprintf(nodename, "/u3@0,f8000000/i2c@f8001000/cpuid@a%d", cpu ? 2 : 0);
	np = of_find_node_by_path(nodename);
	if (np == NULL) {
		printk(KERN_ERR "therm_pm72: Failed to retreive cpuid node from device-tree\n");
		return -ENODEV;
	}
	data = (u8 *)get_property(np, "cpuid", &len);
	if (data == NULL) {
		printk(KERN_ERR "therm_pm72: Failed to retreive cpuid property from device-tree\n");
		of_node_put(np);
		return -ENODEV;
	}
	memcpy(out, data, sizeof(struct mpu_data));
	of_node_put(np);
	
	return 0;
}

/* 
 * Now, unfortunately, sysfs doesn't give us a nice void * we could
 * pass around to the attribute functions, so we don't really have
 * choice but implement a bunch of them...
 *
 * That sucks a bit, we take the lock because FIX32TOPRINT evaluates
 * the input twice... I accept patches :)
 */
#define BUILD_SHOW_FUNC_FIX(name, data)				\
static ssize_t show_##name(struct device *dev, char *buf)	\
{								\
	ssize_t r;						\
	down(&driver_lock);					\
	r = sprintf(buf, "%d.%03d", FIX32TOPRINT(data));	\
	up(&driver_lock);					\
	return r;						\
}
#define BUILD_SHOW_FUNC_INT(name, data)				\
static ssize_t show_##name(struct device *dev, char *buf)	\
{								\
	return sprintf(buf, "%d", data);			\
}

BUILD_SHOW_FUNC_FIX(cpu0_temperature, cpu_state[0].last_temp)
BUILD_SHOW_FUNC_FIX(cpu0_voltage, cpu_state[0].voltage)
BUILD_SHOW_FUNC_FIX(cpu0_current, cpu_state[0].current_a)
BUILD_SHOW_FUNC_INT(cpu0_exhaust_fan_rpm, cpu_state[0].rpm)
BUILD_SHOW_FUNC_INT(cpu0_intake_fan_rpm, cpu_state[0].intake_rpm)

BUILD_SHOW_FUNC_FIX(cpu1_temperature, cpu_state[1].last_temp)
BUILD_SHOW_FUNC_FIX(cpu1_voltage, cpu_state[1].voltage)
BUILD_SHOW_FUNC_FIX(cpu1_current, cpu_state[1].current_a)
BUILD_SHOW_FUNC_INT(cpu1_exhaust_fan_rpm, cpu_state[1].rpm)
BUILD_SHOW_FUNC_INT(cpu1_intake_fan_rpm, cpu_state[1].intake_rpm)

BUILD_SHOW_FUNC_FIX(backside_temperature, backside_state.last_temp)
BUILD_SHOW_FUNC_INT(backside_fan_pwm, backside_state.pwm)

BUILD_SHOW_FUNC_FIX(drives_temperature, drives_state.last_temp)
BUILD_SHOW_FUNC_INT(drives_fan_rpm, drives_state.rpm)

static DEVICE_ATTR(cpu0_temperature,S_IRUGO,show_cpu0_temperature,NULL);
static DEVICE_ATTR(cpu0_voltage,S_IRUGO,show_cpu0_voltage,NULL);
static DEVICE_ATTR(cpu0_current,S_IRUGO,show_cpu0_current,NULL);
static DEVICE_ATTR(cpu0_exhaust_fan_rpm,S_IRUGO,show_cpu0_exhaust_fan_rpm,NULL);
static DEVICE_ATTR(cpu0_intake_fan_rpm,S_IRUGO,show_cpu0_intake_fan_rpm,NULL);

static DEVICE_ATTR(cpu1_temperature,S_IRUGO,show_cpu1_temperature,NULL);
static DEVICE_ATTR(cpu1_voltage,S_IRUGO,show_cpu1_voltage,NULL);
static DEVICE_ATTR(cpu1_current,S_IRUGO,show_cpu1_current,NULL);
static DEVICE_ATTR(cpu1_exhaust_fan_rpm,S_IRUGO,show_cpu1_exhaust_fan_rpm,NULL);
static DEVICE_ATTR(cpu1_intake_fan_rpm,S_IRUGO,show_cpu1_intake_fan_rpm,NULL);

static DEVICE_ATTR(backside_temperature,S_IRUGO,show_backside_temperature,NULL);
static DEVICE_ATTR(backside_fan_pwm,S_IRUGO,show_backside_fan_pwm,NULL);

static DEVICE_ATTR(drives_temperature,S_IRUGO,show_drives_temperature,NULL);
static DEVICE_ATTR(drives_fan_rpm,S_IRUGO,show_drives_fan_rpm,NULL);

/*
 * CPUs fans control loop
 */
static void do_monitor_cpu(struct cpu_pid_state *state)
{
	s32 temp, voltage, current_a, power, power_target;
	s32 integral, derivative, proportional, adj_in_target, sval;
	s64 integ_p, deriv_p, prop_p, sum; 
	int i, intake, rc;

	DBG("cpu %d:\n", state->index);

	/* Read current fan status */
	if (state->index == 0)
		rc = get_rpm_fan(CPUA_EXHAUST_FAN_RPM_ID, !RPM_PID_USE_ACTUAL_SPEED);
	else
		rc = get_rpm_fan(CPUB_EXHAUST_FAN_RPM_ID, !RPM_PID_USE_ACTUAL_SPEED);
	if (rc < 0) {
		printk(KERN_WARNING "Error %d reading CPU %d exhaust fan !\n",
		       rc, state->index);
		/* XXX What do we do now ? */
	} else
		state->rpm = rc;
	DBG("  current rpm: %d\n", state->rpm);

	/* Get some sensor readings and scale it */
	temp = read_smon_adc(state, 1);
	if (temp == -1) {
		state->overtemp++;
		return;
	}
	voltage = read_smon_adc(state, 3);
	current_a = read_smon_adc(state, 4);

	/* Fixup temperature according to diode calibration
	 */
	DBG("  temp raw: %04x, m_diode: %04x, b_diode: %04x\n",
	    temp, state->mpu.mdiode, state->mpu.bdiode);
	temp = ((s32)temp * (s32)state->mpu.mdiode + ((s32)state->mpu.bdiode << 12)) >> 2;
	state->last_temp = temp;
	DBG("  temp: %d.%03d\n", FIX32TOPRINT(temp));

	/* Check tmax, increment overtemp if we are there. At tmax+8, we go
	 * full blown immediately and try to trigger a shutdown
	 */
	if (temp >= ((state->mpu.tmax + 8) << 16)) {
		printk(KERN_WARNING "Warning ! CPU %d temperature way above maximum"
		       " (%d) !\n",
		       state->index, temp >> 16);
		state->overtemp = CPU_MAX_OVERTEMP;
	} else if (temp > (state->mpu.tmax << 16))
		state->overtemp++;
	else
		state->overtemp = 0;
	if (state->overtemp >= CPU_MAX_OVERTEMP)
		critical_state = 1;
	if (state->overtemp > 0) {
		state->rpm = state->mpu.rmaxn_exhaust_fan;
		state->intake_rpm = intake = state->mpu.rmaxn_intake_fan;
		goto do_set_fans;
	}
	
	/* Scale other sensor values according to fixed scales
	 * obtained in Darwin and calculate power from I and V
	 */
	state->voltage = voltage *= ADC_CPU_VOLTAGE_SCALE;
	state->current_a = current_a *= ADC_CPU_CURRENT_SCALE;
	power = (((u64)current_a) * ((u64)voltage)) >> 16;

	/* Calculate power target value (could be done once for all)
	 * and convert to a 16.16 fp number
	 */
	power_target = ((u32)(state->mpu.pmaxh - state->mpu.padjmax)) << 16;

	DBG("  current: %d.%03d, voltage: %d.%03d\n",
	    FIX32TOPRINT(current_a), FIX32TOPRINT(voltage));
	DBG("  power: %d.%03d W, target: %d.%03d, error: %d.%03d\n", FIX32TOPRINT(power),
	    FIX32TOPRINT(power_target), FIX32TOPRINT(power_target - power));

	/* Store temperature and power in history array */
	state->cur_temp = (state->cur_temp + 1) % CPU_TEMP_HISTORY_SIZE;
	state->temp_history[state->cur_temp] = temp;
	state->cur_power = (state->cur_power + 1) % state->count_power;
	state->power_history[state->cur_power] = power;
	state->error_history[state->cur_power] = power_target - power;
	
	/* If first loop, fill the history table */
	if (state->first) {
		for (i = 0; i < (state->count_power - 1); i++) {
			state->cur_power = (state->cur_power + 1) % state->count_power;
			state->power_history[state->cur_power] = power;
			state->error_history[state->cur_power] = power_target - power;
		}
		for (i = 0; i < (CPU_TEMP_HISTORY_SIZE - 1); i++) {
			state->cur_temp = (state->cur_temp + 1) % CPU_TEMP_HISTORY_SIZE;
			state->temp_history[state->cur_temp] = temp;			
		}
		state->first = 0;
	}

	/* Calculate the integral term normally based on the "power" values */
	sum = 0;
	integral = 0;
	for (i = 0; i < state->count_power; i++)
		integral += state->error_history[i];
	integral *= CPU_PID_INTERVAL;
	DBG("  integral: %08x\n", integral);

	/* Calculate the adjusted input (sense value).
	 *   G_r is 12.20
	 *   integ is 16.16
	 *   so the result is 28.36
	 *
	 * input target is mpu.ttarget, input max is mpu.tmax
	 */
	integ_p = ((s64)state->mpu.pid_gr) * (s64)integral;
	DBG("   integ_p: %d\n", (int)(deriv_p >> 36));
	sval = (state->mpu.tmax << 16) - ((integ_p >> 20) & 0xffffffff);
	adj_in_target = (state->mpu.ttarget << 16);
	if (adj_in_target > sval)
		adj_in_target = sval;
	DBG("   adj_in_target: %d.%03d, ttarget: %d\n", FIX32TOPRINT(adj_in_target),
	    state->mpu.ttarget);

	/* Calculate the derivative term */
	derivative = state->temp_history[state->cur_temp] -
		state->temp_history[(state->cur_temp + CPU_TEMP_HISTORY_SIZE - 1)
				    % CPU_TEMP_HISTORY_SIZE];
	derivative /= CPU_PID_INTERVAL;
	deriv_p = ((s64)state->mpu.pid_gd) * (s64)derivative;
	DBG("   deriv_p: %d\n", (int)(deriv_p >> 36));
	sum += deriv_p;

	/* Calculate the proportional term */
	proportional = temp - adj_in_target;
	prop_p = ((s64)state->mpu.pid_gp) * (s64)proportional;
	DBG("   prop_p: %d\n", (int)(prop_p >> 36));
	sum += prop_p;

	/* Scale sum */
	sum >>= 36;

	DBG("   sum: %d\n", (int)sum);
	state->rpm += (s32)sum;

	if (state->rpm < state->mpu.rminn_exhaust_fan)
		state->rpm = state->mpu.rminn_exhaust_fan;
	if (state->rpm > state->mpu.rmaxn_exhaust_fan)
		state->rpm = state->mpu.rmaxn_exhaust_fan;

	intake = (state->rpm * CPU_INTAKE_SCALE) >> 16;
	if (intake < state->mpu.rminn_intake_fan)
		intake = state->mpu.rminn_intake_fan;
	if (intake > state->mpu.rmaxn_intake_fan)
		intake = state->mpu.rmaxn_intake_fan;
	state->intake_rpm = intake;

 do_set_fans:
	DBG("** CPU %d RPM: %d Ex, %d In, overtemp: %d\n",
	    state->index, (int)state->rpm, intake, state->overtemp);

	/* We should check for errors, shouldn't we ? But then, what
	 * do we do once the error occurs ? For FCU notified fan
	 * failures (-EFAULT) we probably want to notify userland
	 * some way...
	 */
	if (state->index == 0) {
		set_rpm_fan(CPUA_INTAKE_FAN_RPM_ID, intake);
		set_rpm_fan(CPUA_EXHAUST_FAN_RPM_ID, state->rpm);
	} else {
		set_rpm_fan(CPUB_INTAKE_FAN_RPM_ID, intake);
		set_rpm_fan(CPUB_EXHAUST_FAN_RPM_ID, state->rpm);
	}
}

/*
 * Initialize the state structure for one CPU control loop
 */
static int init_cpu_state(struct cpu_pid_state *state, int index)
{
	state->index = index;
	state->first = 1;
	state->rpm = 1000;
	state->overtemp = 0;
	state->adc_config = 0x00;

	if (index == 0)
		state->monitor = attach_i2c_chip(SUPPLY_MONITOR_ID, "CPU0_monitor");
	else if (index == 1)
		state->monitor = attach_i2c_chip(SUPPLY_MONITORB_ID, "CPU1_monitor");
	if (state->monitor == NULL)
		goto fail;

	if (read_eeprom(index, &state->mpu))
		goto fail;

	state->count_power = state->mpu.tguardband;
	if (state->count_power > CPU_POWER_HISTORY_SIZE) {
		printk(KERN_WARNING "Warning ! too many power history slots\n");
		state->count_power = CPU_POWER_HISTORY_SIZE;
	}
	DBG("CPU %d Using %d power history entries\n", index, state->count_power);

	if (index == 0) {
		device_create_file(&of_dev->dev, &dev_attr_cpu0_temperature);
		device_create_file(&of_dev->dev, &dev_attr_cpu0_voltage);
		device_create_file(&of_dev->dev, &dev_attr_cpu0_current);
		device_create_file(&of_dev->dev, &dev_attr_cpu0_exhaust_fan_rpm);
		device_create_file(&of_dev->dev, &dev_attr_cpu0_intake_fan_rpm);
	} else {
		device_create_file(&of_dev->dev, &dev_attr_cpu1_temperature);
		device_create_file(&of_dev->dev, &dev_attr_cpu1_voltage);
		device_create_file(&of_dev->dev, &dev_attr_cpu1_current);
		device_create_file(&of_dev->dev, &dev_attr_cpu1_exhaust_fan_rpm);
		device_create_file(&of_dev->dev, &dev_attr_cpu1_intake_fan_rpm);
	}

	return 0;
 fail:
	if (state->monitor)
		detach_i2c_chip(state->monitor);
	state->monitor = NULL;
	
	return -ENODEV;
}

/*
 * Dispose of the state data for one CPU control loop
 */
static void dispose_cpu_state(struct cpu_pid_state *state)
{
	if (state->monitor == NULL)
		return;

	if (state->index == 0) {
		device_remove_file(&of_dev->dev, &dev_attr_cpu0_temperature);
		device_remove_file(&of_dev->dev, &dev_attr_cpu0_voltage);
		device_remove_file(&of_dev->dev, &dev_attr_cpu0_current);
		device_remove_file(&of_dev->dev, &dev_attr_cpu0_exhaust_fan_rpm);
		device_remove_file(&of_dev->dev, &dev_attr_cpu0_intake_fan_rpm);
	} else {
		device_remove_file(&of_dev->dev, &dev_attr_cpu1_temperature);
		device_remove_file(&of_dev->dev, &dev_attr_cpu1_voltage);
		device_remove_file(&of_dev->dev, &dev_attr_cpu1_current);
		device_remove_file(&of_dev->dev, &dev_attr_cpu1_exhaust_fan_rpm);
		device_remove_file(&of_dev->dev, &dev_attr_cpu1_intake_fan_rpm);
	}

	detach_i2c_chip(state->monitor);
	state->monitor = NULL;
}

/*
 * Motherboard backside & U3 heatsink fan control loop
 */
static void do_monitor_backside(struct backside_pid_state *state)
{
	s32 temp, integral, derivative;
	s64 integ_p, deriv_p, prop_p, sum; 
	int i, rc;

	if (--state->ticks != 0)
		return;
	state->ticks = BACKSIDE_PID_INTERVAL;

	DBG("backside:\n");

	/* Check fan status */
	rc = get_pwm_fan(BACKSIDE_FAN_PWM_ID);
	if (rc < 0) {
		printk(KERN_WARNING "Error %d reading backside fan !\n", rc);
		/* XXX What do we do now ? */
	} else
		state->pwm = rc;
	DBG("  current pwm: %d\n", state->pwm);

	/* Get some sensor readings */
	temp = i2c_smbus_read_byte_data(state->monitor, MAX6690_EXT_TEMP) << 16;
	state->last_temp = temp;
	DBG("  temp: %d.%03d, target: %d.%03d\n", FIX32TOPRINT(temp),
	    FIX32TOPRINT(BACKSIDE_PID_INPUT_TARGET));

	/* Store temperature and error in history array */
	state->cur_sample = (state->cur_sample + 1) % BACKSIDE_PID_HISTORY_SIZE;
	state->sample_history[state->cur_sample] = temp;
	state->error_history[state->cur_sample] = temp - BACKSIDE_PID_INPUT_TARGET;
	
	/* If first loop, fill the history table */
	if (state->first) {
		for (i = 0; i < (BACKSIDE_PID_HISTORY_SIZE - 1); i++) {
			state->cur_sample = (state->cur_sample + 1) %
				BACKSIDE_PID_HISTORY_SIZE;
			state->sample_history[state->cur_sample] = temp;
			state->error_history[state->cur_sample] =
				temp - BACKSIDE_PID_INPUT_TARGET;
		}
		state->first = 0;
	}

	/* Calculate the integral term */
	sum = 0;
	integral = 0;
	for (i = 0; i < BACKSIDE_PID_HISTORY_SIZE; i++)
		integral += state->error_history[i];
	integral *= BACKSIDE_PID_INTERVAL;
	DBG("  integral: %08x\n", integral);
	integ_p = ((s64)BACKSIDE_PID_G_r) * (s64)integral;
	DBG("   integ_p: %d\n", (int)(integ_p >> 36));
	sum += integ_p;

	/* Calculate the derivative term */
	derivative = state->error_history[state->cur_sample] -
		state->error_history[(state->cur_sample + BACKSIDE_PID_HISTORY_SIZE - 1)
				    % BACKSIDE_PID_HISTORY_SIZE];
	derivative /= BACKSIDE_PID_INTERVAL;
	deriv_p = ((s64)BACKSIDE_PID_G_d) * (s64)derivative;
	DBG("   deriv_p: %d\n", (int)(deriv_p >> 36));
	sum += deriv_p;

	/* Calculate the proportional term */
	prop_p = ((s64)BACKSIDE_PID_G_p) * (s64)(state->error_history[state->cur_sample]);
	DBG("   prop_p: %d\n", (int)(prop_p >> 36));
	sum += prop_p;

	/* Scale sum */
	sum >>= 36;

	DBG("   sum: %d\n", (int)sum);
	state->pwm += (s32)sum;
	if (state->pwm < BACKSIDE_PID_OUTPUT_MIN)
		state->pwm = BACKSIDE_PID_OUTPUT_MIN;
	if (state->pwm > BACKSIDE_PID_OUTPUT_MAX)
		state->pwm = BACKSIDE_PID_OUTPUT_MAX;

	DBG("** BACKSIDE PWM: %d\n", (int)state->pwm);
	set_pwm_fan(BACKSIDE_FAN_PWM_ID, state->pwm);
}

/*
 * Initialize the state structure for the backside fan control loop
 */
static int init_backside_state(struct backside_pid_state *state)
{
	state->ticks = 1;
	state->first = 1;
	state->pwm = 50;

	state->monitor = attach_i2c_chip(BACKSIDE_MAX_ID, "backside_temp");
	if (state->monitor == NULL)
		return -ENODEV;

	device_create_file(&of_dev->dev, &dev_attr_backside_temperature);
	device_create_file(&of_dev->dev, &dev_attr_backside_fan_pwm);

	return 0;
}

/*
 * Dispose of the state data for the backside control loop
 */
static void dispose_backside_state(struct backside_pid_state *state)
{
	if (state->monitor == NULL)
		return;

	device_remove_file(&of_dev->dev, &dev_attr_backside_temperature);
	device_remove_file(&of_dev->dev, &dev_attr_backside_fan_pwm);

	detach_i2c_chip(state->monitor);
	state->monitor = NULL;
}
 
/*
 * Drives bay fan control loop
 */
static void do_monitor_drives(struct drives_pid_state *state)
{
	s32 temp, integral, derivative;
	s64 integ_p, deriv_p, prop_p, sum; 
	int i, rc;

	if (--state->ticks != 0)
		return;
	state->ticks = DRIVES_PID_INTERVAL;

	DBG("drives:\n");

	/* Check fan status */
	rc = get_rpm_fan(DRIVES_FAN_RPM_ID, !RPM_PID_USE_ACTUAL_SPEED);
	if (rc < 0) {
		printk(KERN_WARNING "Error %d reading drives fan !\n", rc);
		/* XXX What do we do now ? */
	} else
		state->rpm = rc;
	DBG("  current rpm: %d\n", state->rpm);

	/* Get some sensor readings */
	temp = le16_to_cpu(i2c_smbus_read_word_data(state->monitor, DS1775_TEMP)) << 8;
	state->last_temp = temp;
	DBG("  temp: %d.%03d, target: %d.%03d\n", FIX32TOPRINT(temp),
	    FIX32TOPRINT(DRIVES_PID_INPUT_TARGET));

	/* Store temperature and error in history array */
	state->cur_sample = (state->cur_sample + 1) % DRIVES_PID_HISTORY_SIZE;
	state->sample_history[state->cur_sample] = temp;
	state->error_history[state->cur_sample] = temp - DRIVES_PID_INPUT_TARGET;
	
	/* If first loop, fill the history table */
	if (state->first) {
		for (i = 0; i < (DRIVES_PID_HISTORY_SIZE - 1); i++) {
			state->cur_sample = (state->cur_sample + 1) %
				DRIVES_PID_HISTORY_SIZE;
			state->sample_history[state->cur_sample] = temp;
			state->error_history[state->cur_sample] =
				temp - DRIVES_PID_INPUT_TARGET;
		}
		state->first = 0;
	}

	/* Calculate the integral term */
	sum = 0;
	integral = 0;
	for (i = 0; i < DRIVES_PID_HISTORY_SIZE; i++)
		integral += state->error_history[i];
	integral *= DRIVES_PID_INTERVAL;
	DBG("  integral: %08x\n", integral);
	integ_p = ((s64)DRIVES_PID_G_r) * (s64)integral;
	DBG("   integ_p: %d\n", (int)(integ_p >> 36));
	sum += integ_p;

	/* Calculate the derivative term */
	derivative = state->error_history[state->cur_sample] -
		state->error_history[(state->cur_sample + DRIVES_PID_HISTORY_SIZE - 1)
				    % DRIVES_PID_HISTORY_SIZE];
	derivative /= DRIVES_PID_INTERVAL;
	deriv_p = ((s64)DRIVES_PID_G_d) * (s64)derivative;
	DBG("   deriv_p: %d\n", (int)(deriv_p >> 36));
	sum += deriv_p;

	/* Calculate the proportional term */
	prop_p = ((s64)DRIVES_PID_G_p) * (s64)(state->error_history[state->cur_sample]);
	DBG("   prop_p: %d\n", (int)(prop_p >> 36));
	sum += prop_p;

	/* Scale sum */
	sum >>= 36;

	DBG("   sum: %d\n", (int)sum);
	state->rpm += (s32)sum;
	if (state->rpm < DRIVES_PID_OUTPUT_MIN)
		state->rpm = DRIVES_PID_OUTPUT_MIN;
	if (state->rpm > DRIVES_PID_OUTPUT_MAX)
		state->rpm = DRIVES_PID_OUTPUT_MAX;

	DBG("** DRIVES RPM: %d\n", (int)state->rpm);
	set_rpm_fan(DRIVES_FAN_RPM_ID, state->rpm);
}

/*
 * Initialize the state structure for the drives bay fan control loop
 */
static int init_drives_state(struct drives_pid_state *state)
{
	state->ticks = 1;
	state->first = 1;
	state->rpm = 1000;

	state->monitor = attach_i2c_chip(DRIVES_DALLAS_ID, "drives_temp");
	if (state->monitor == NULL)
		return -ENODEV;

	device_create_file(&of_dev->dev, &dev_attr_drives_temperature);
	device_create_file(&of_dev->dev, &dev_attr_drives_fan_rpm);

	return 0;
}

/*
 * Dispose of the state data for the drives control loop
 */
static void dispose_drives_state(struct drives_pid_state *state)
{
	if (state->monitor == NULL)
		return;

	device_remove_file(&of_dev->dev, &dev_attr_drives_temperature);
	device_remove_file(&of_dev->dev, &dev_attr_drives_fan_rpm);

	detach_i2c_chip(state->monitor);
	state->monitor = NULL;
}

static int call_critical_overtemp(void)
{
	char *argv[] = { critical_overtemp_path, NULL };
	static char *envp[] = { "HOME=/",
				"TERM=linux",
				"PATH=/sbin:/usr/sbin:/bin:/usr/bin",
				NULL };

	return call_usermodehelper(critical_overtemp_path, argv, envp, 0);
}


/*
 * Here's the kernel thread that calls the various control loops
 */
static int main_control_loop(void *x)
{
	daemonize("kfand");

	DBG("main_control_loop started\n");

	down(&driver_lock);

	if (start_fcu() < 0) {
		printk(KERN_ERR "kfand: failed to start FCU\n");
		up(&driver_lock);
		goto out;
	}

	/* Set the PCI fan once for now */
	set_pwm_fan(SLOTS_FAN_PWM_ID, SLOTS_FAN_DEFAULT_PWM);

	/* Initialize ADCs */
	initialize_adc(&cpu_state[0]);
	if (cpu_state[1].monitor != NULL)
		initialize_adc(&cpu_state[1]);

	up(&driver_lock);

	while (state == state_attached) {
		unsigned long elapsed, start;

		start = jiffies;

		down(&driver_lock);
		do_monitor_cpu(&cpu_state[0]);
		if (cpu_state[1].monitor != NULL)
			do_monitor_cpu(&cpu_state[1]);
		do_monitor_backside(&backside_state);
		do_monitor_drives(&drives_state);
		up(&driver_lock);

		if (critical_state == 1) {
			printk(KERN_WARNING "Temperature control detected a critical condition\n");
			printk(KERN_WARNING "Attempting to shut down...\n");
			if (call_critical_overtemp()) {
				printk(KERN_WARNING "Can't call %s, power off now!\n",
				       critical_overtemp_path);
				machine_power_off();
			}
		}
		if (critical_state > 0)
			critical_state++;
		if (critical_state > MAX_CRITICAL_STATE) {
			printk(KERN_WARNING "Shutdown timed out, power off now !\n");
			machine_power_off();
		}

		// FIXME: Deal with signals
		set_current_state(TASK_INTERRUPTIBLE);
		elapsed = jiffies - start;
		if (elapsed < HZ)
			schedule_timeout(HZ - elapsed);
	}

 out:
	DBG("main_control_loop ended\n");

	ctrl_task = 0;
	complete_and_exit(&ctrl_complete, 0);
}

/*
 * Dispose the control loops when tearing down
 */
static void dispose_control_loops(void)
{
	dispose_cpu_state(&cpu_state[0]);
	dispose_cpu_state(&cpu_state[1]);

	dispose_backside_state(&backside_state);
	dispose_drives_state(&drives_state);
}

/*
 * Create the control loops. U3-0 i2c bus is up, so we can now
 * get to the various sensors
 */
static int create_control_loops(void)
{
	struct device_node *np;

	/* Count CPUs from the device-tree, we don't care how many are
	 * actually used by Linux
	 */
	cpu_count = 0;
	for (np = NULL; NULL != (np = of_find_node_by_type(np, "cpu"));)
		cpu_count++;

	DBG("counted %d CPUs in the device-tree\n", cpu_count);

	/* Create control loops for everything. If any fail, everything
	 * fails
	 */
	if (init_cpu_state(&cpu_state[0], 0))
		goto fail;
	if (cpu_count > 1 && init_cpu_state(&cpu_state[1], 1))
		goto fail;
	if (init_backside_state(&backside_state))
		goto fail;
	if (init_drives_state(&drives_state))
		goto fail;

	DBG("all control loops up !\n");

	return 0;
	
 fail:
	DBG("failure creating control loops, disposing\n");

	dispose_control_loops();

	return -ENODEV;
}

/*
 * Start the control loops after everything is up, that is create
 * the thread that will make them run
 */
static void start_control_loops(void)
{
	init_completion(&ctrl_complete);

	ctrl_task = kernel_thread(main_control_loop, NULL, SIGCHLD | CLONE_KERNEL);
}

/*
 * Stop the control loops when tearing down
 */
static void stop_control_loops(void)
{
	if (ctrl_task != 0)
		wait_for_completion(&ctrl_complete);
}

/*
 * Attach to the i2c FCU after detecting U3-1 bus
 */
static int attach_fcu(void)
{
	fcu = attach_i2c_chip(FAN_CTRLER_ID, "fcu");
	if (fcu == NULL)
		return -ENODEV;

	DBG("FCU attached\n");

	return 0;
}

/*
 * Detach from the i2c FCU when tearing down
 */
static void detach_fcu(void)
{
	if (fcu)
		detach_i2c_chip(fcu);
	fcu = NULL;
}

/*
 * Attach to the i2c controller. We probe the various chips based
 * on the device-tree nodes and build everything for the driver to
 * run, we then kick the driver monitoring thread
 */
static int therm_pm72_attach(struct i2c_adapter *adapter)
{
	down(&driver_lock);

	/* Check state */
	if (state == state_detached)
		state = state_attaching;
	if (state != state_attaching) {
		up(&driver_lock);
		return 0;
	}

	/* Check if we are looking for one of these */
	if (u3_0 == NULL && !strcmp(adapter->name, "u3 0")) {
		u3_0 = adapter;
		DBG("found U3-0, creating control loops\n");
		if (create_control_loops())
			u3_0 = NULL;
	} else if (u3_1 == NULL && !strcmp(adapter->name, "u3 1")) {
		u3_1 = adapter;
		DBG("found U3-1, attaching FCU\n");
		if (attach_fcu())
			u3_1 = NULL;
	}
	/* We got all we need, start control loops */
	if (u3_0 != NULL && u3_1 != NULL) {
		DBG("everything up, starting control loops\n");
		state = state_attached;
		start_control_loops();
	}
	up(&driver_lock);

	return 0;
}

/*
 * Called on every adapter when the driver or the i2c controller
 * is going away.
 */
static int therm_pm72_detach(struct i2c_adapter *adapter)
{
	down(&driver_lock);

	if (state != state_detached)
		state = state_detaching;

	/* Stop control loops if any */
	DBG("stopping control loops\n");
	up(&driver_lock);
	stop_control_loops();
	down(&driver_lock);

	if (u3_0 != NULL && !strcmp(adapter->name, "u3 0")) {
		DBG("lost U3-0, disposing control loops\n");
		dispose_control_loops();
		u3_0 = NULL;
	}
	
	if (u3_1 != NULL && !strcmp(adapter->name, "u3 1")) {
		DBG("lost U3-1, detaching FCU\n");
		detach_fcu();
		u3_1 = NULL;
	}
	if (u3_0 == NULL && u3_1 == NULL)
		state = state_detached;

	up(&driver_lock);

	return 0;
}

static int fcu_of_probe(struct of_device* dev, const struct of_match *match)
{
	int rc;

	state = state_detached;

	rc = i2c_add_driver(&therm_pm72_driver);
	if (rc < 0)
		return rc;
	return 0;
}

static int fcu_of_remove(struct of_device* dev)
{
	i2c_del_driver(&therm_pm72_driver);

	return 0;
}

static struct of_match fcu_of_match[] = 
{
	{
	.name 		= OF_ANY_MATCH,
	.type		= "fcu",
	.compatible	= OF_ANY_MATCH
	},
	{},
};

static struct of_platform_driver fcu_of_platform_driver = 
{
	.name 		= "temperature",
	.match_table	= fcu_of_match,
	.probe		= fcu_of_probe,
	.remove		= fcu_of_remove
};

/*
 * Check machine type, attach to i2c controller
 */
static int __init therm_pm72_init(void)
{
	struct device_node *np;

	if (!machine_is_compatible("PowerMac7,2"))
	    	return -ENODEV;

	printk(KERN_INFO "PowerMac G5 Thermal control driver %s\n", VERSION);

	np = of_find_node_by_type(NULL, "fcu");
	if (np == NULL) {
		printk(KERN_ERR "Can't find FCU in device-tree !\n");
		return -ENODEV;
	}
	of_dev = of_platform_device_create(np, "temperature");
	if (of_dev == NULL) {
		printk(KERN_ERR "Can't register FCU platform device !\n");
		return -ENODEV;
	}

	of_register_driver(&fcu_of_platform_driver);
	
	return 0;
}

static void __exit therm_pm72_exit(void)
{
	of_unregister_driver(&fcu_of_platform_driver);

	if (of_dev)
		of_device_unregister(of_dev);
}

module_init(therm_pm72_init);
module_exit(therm_pm72_exit);

MODULE_AUTHOR("Benjamin Herrenschmidt <benh@kernel.crashing.org>");
MODULE_DESCRIPTION("Driver for Apple's PowerMac7,2 G5 thermal control");
MODULE_LICENSE("GPL");

