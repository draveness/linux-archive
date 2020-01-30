/* DVB USB compliant Linux driver for the
 *  - GENPIX 8pks/qpsk USB2.0 DVB-S module
 *
 * Copyright (C) 2006 Alan Nisota (alannisota@gmail.com)
 *
 * Thanks to GENPIX for the sample code used to implement this module.
 *
 * This module is based off the vp7045 and vp702x modules
 *
 *	This program is free software; you can redistribute it and/or modify it
 *	under the terms of the GNU General Public License as published by the Free
 *	Software Foundation, version 2.
 *
 * see Documentation/dvb/README.dvb-usb for more information
 */
#include "gp8psk.h"

struct gp8psk_fe_state {
	struct dvb_frontend fe;

	struct dvb_usb_device *d;

	u16 snr;

	unsigned long next_snr_check;
};

static int gp8psk_fe_read_status(struct dvb_frontend* fe, fe_status_t *status)
{
	struct gp8psk_fe_state *st = fe->demodulator_priv;
	u8 lock;

	if (gp8psk_usb_in_op(st->d, GET_SIGNAL_LOCK, 0, 0, &lock,1))
		return -EINVAL;

	if (lock)
		*status = FE_HAS_LOCK | FE_HAS_SYNC | FE_HAS_VITERBI | FE_HAS_SIGNAL | FE_HAS_CARRIER;
	else
		*status = 0;

	return 0;
}

/* not supported by this Frontend */
static int gp8psk_fe_read_ber(struct dvb_frontend* fe, u32 *ber)
{
	(void) fe;
	*ber = 0;
	return 0;
}

/* not supported by this Frontend */
static int gp8psk_fe_read_unc_blocks(struct dvb_frontend* fe, u32 *unc)
{
	(void) fe;
	*unc = 0;
	return 0;
}

static int gp8psk_fe_read_snr(struct dvb_frontend* fe, u16 *snr)
{
	struct gp8psk_fe_state *st = fe->demodulator_priv;
	u8 buf[2];

	if (time_after(jiffies,st->next_snr_check)) {
		gp8psk_usb_in_op(st->d,GET_SIGNAL_STRENGTH,0,0,buf,2);
		*snr = (int)(buf[1]) << 8 | buf[0];
		/* snr is reported in dBu*256 */
		/* snr / 38.4 ~= 100% strength */
		/* snr * 17 returns 100% strength as 65535 */
		if (*snr <= 3855)
			*snr = (*snr<<4) + *snr; // snr * 17
		else
			*snr = 65535;
		st->next_snr_check = jiffies + (10*HZ)/1000;
	} else {
		*snr = st->snr;
	}
	return 0;
}

static int gp8psk_fe_read_signal_strength(struct dvb_frontend* fe, u16 *strength)
{
	return gp8psk_fe_read_snr(fe, strength);
}

static int gp8psk_fe_get_tune_settings(struct dvb_frontend* fe, struct dvb_frontend_tune_settings *tune)
{
	tune->min_delay_ms = 800;
	return 0;
}

static int gp8psk_fe_set_frontend(struct dvb_frontend* fe,
				  struct dvb_frontend_parameters *fep)
{
	struct gp8psk_fe_state *state = fe->demodulator_priv;
	u8 cmd[10];
	u32 freq = fep->frequency * 1000;

	cmd[4] = freq         & 0xff;
	cmd[5] = (freq >> 8)  & 0xff;
	cmd[6] = (freq >> 16) & 0xff;
	cmd[7] = (freq >> 24) & 0xff;

	switch(fe->ops.info.type) {
	case FE_QPSK:
		cmd[0] =  fep->u.qpsk.symbol_rate        & 0xff;
		cmd[1] = (fep->u.qpsk.symbol_rate >>  8) & 0xff;
		cmd[2] = (fep->u.qpsk.symbol_rate >> 16) & 0xff;
		cmd[3] = (fep->u.qpsk.symbol_rate >> 24) & 0xff;
		cmd[8] = ADV_MOD_DVB_QPSK;
		cmd[9] = 0x03; /*ADV_MOD_FEC_XXX*/
		break;
	default:
		// other modes are unsuported right now
		cmd[0] = 0;
		cmd[1] = 0;
		cmd[2] = 0;
		cmd[3] = 0;
		cmd[8] = 0;
		cmd[9] = 0;
		break;
	}

	gp8psk_usb_out_op(state->d,TUNE_8PSK,0,0,cmd,10);

	state->next_snr_check = jiffies;

	return 0;
}

static int gp8psk_fe_get_frontend(struct dvb_frontend* fe,
				  struct dvb_frontend_parameters *fep)
{
	return 0;
}


static int gp8psk_fe_send_diseqc_msg (struct dvb_frontend* fe,
				    struct dvb_diseqc_master_cmd *m)
{
	struct gp8psk_fe_state *st = fe->demodulator_priv;

	deb_fe("%s\n",__FUNCTION__);

	if (gp8psk_usb_out_op(st->d,SEND_DISEQC_COMMAND, m->msg[0], 0,
			m->msg, m->msg_len)) {
		return -EINVAL;
	}
	return 0;
}

static int gp8psk_fe_send_diseqc_burst (struct dvb_frontend* fe,
				    fe_sec_mini_cmd_t burst)
{
	struct gp8psk_fe_state *st = fe->demodulator_priv;
	u8 cmd;

	deb_fe("%s\n",__FUNCTION__);

	/* These commands are certainly wrong */
	cmd = (burst == SEC_MINI_A) ? 0x00 : 0x01;

	if (gp8psk_usb_out_op(st->d,SEND_DISEQC_COMMAND, cmd, 0,
			&cmd, 0)) {
		return -EINVAL;
	}
	return 0;
}

static int gp8psk_fe_set_tone (struct dvb_frontend* fe, fe_sec_tone_mode_t tone)
{
	struct gp8psk_fe_state* state = fe->demodulator_priv;

	if (gp8psk_usb_out_op(state->d,SET_22KHZ_TONE,
		 (tone == SEC_TONE_ON), 0, NULL, 0)) {
		return -EINVAL;
	}
	return 0;
}

static int gp8psk_fe_set_voltage (struct dvb_frontend* fe, fe_sec_voltage_t voltage)
{
	struct gp8psk_fe_state* state = fe->demodulator_priv;

	if (gp8psk_usb_out_op(state->d,SET_LNB_VOLTAGE,
			 voltage == SEC_VOLTAGE_18, 0, NULL, 0)) {
		return -EINVAL;
	}
	return 0;
}

static int gp8psk_fe_send_legacy_dish_cmd (struct dvb_frontend* fe, unsigned long sw_cmd)
{
	struct gp8psk_fe_state* state = fe->demodulator_priv;
	u8 cmd = sw_cmd & 0x7f;

	if (gp8psk_usb_out_op(state->d,SET_DN_SWITCH, cmd, 0,
			NULL, 0)) {
		return -EINVAL;
	}
	if (gp8psk_usb_out_op(state->d,SET_LNB_VOLTAGE, !!(sw_cmd & 0x80),
			0, NULL, 0)) {
		return -EINVAL;
	}

	return 0;
}

static void gp8psk_fe_release(struct dvb_frontend* fe)
{
	struct gp8psk_fe_state *state = fe->demodulator_priv;
	kfree(state);
}

static struct dvb_frontend_ops gp8psk_fe_ops;

struct dvb_frontend * gp8psk_fe_attach(struct dvb_usb_device *d)
{
	struct gp8psk_fe_state *s = kzalloc(sizeof(struct gp8psk_fe_state), GFP_KERNEL);
	if (s == NULL)
		goto error;

	s->d = d;
	memcpy(&s->fe.ops, &gp8psk_fe_ops, sizeof(struct dvb_frontend_ops));
	s->fe.demodulator_priv = s;

	goto success;
error:
	return NULL;
success:
	return &s->fe;
}


static struct dvb_frontend_ops gp8psk_fe_ops = {
	.info = {
		.name			= "Genpix 8psk-USB DVB-S",
		.type			= FE_QPSK,
		.frequency_min		= 950000,
		.frequency_max		= 2150000,
		.frequency_stepsize	= 100,
		.symbol_rate_min        = 1000000,
		.symbol_rate_max        = 45000000,
		.symbol_rate_tolerance  = 500,  /* ppm */
		.caps = FE_CAN_INVERSION_AUTO |
				FE_CAN_FEC_1_2 | FE_CAN_FEC_2_3 | FE_CAN_FEC_3_4 |
				FE_CAN_FEC_5_6 | FE_CAN_FEC_7_8 | FE_CAN_FEC_AUTO |
				FE_CAN_QPSK
	},

	.release = gp8psk_fe_release,

	.init = NULL,
	.sleep = NULL,

	.set_frontend = gp8psk_fe_set_frontend,
	.get_frontend = gp8psk_fe_get_frontend,
	.get_tune_settings = gp8psk_fe_get_tune_settings,

	.read_status = gp8psk_fe_read_status,
	.read_ber = gp8psk_fe_read_ber,
	.read_signal_strength = gp8psk_fe_read_signal_strength,
	.read_snr = gp8psk_fe_read_snr,
	.read_ucblocks = gp8psk_fe_read_unc_blocks,

	.diseqc_send_master_cmd = gp8psk_fe_send_diseqc_msg,
	.diseqc_send_burst = gp8psk_fe_send_diseqc_burst,
	.set_tone = gp8psk_fe_set_tone,
	.set_voltage = gp8psk_fe_set_voltage,
	.dishnetwork_send_legacy_command = gp8psk_fe_send_legacy_dish_cmd,
};
