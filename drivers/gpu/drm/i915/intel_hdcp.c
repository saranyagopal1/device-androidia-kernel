/* SPDX-License-Identifier: MIT */
/*
 * Copyright (C) 2017 Google, Inc.
 *
 * Authors:
 * Sean Paul <seanpaul@chromium.org>
 */

#include <drm/drmP.h>
#include <drm/drm_hdcp.h>
#include <linux/i2c.h>
#include <linux/random.h>
#include <uapi/linux/swab.h>

#include "intel_drv.h"
#include "i915_reg.h"

#define KEY_LOAD_TRIES	5

static int intel_hdcp_poll_ksv_fifo(struct intel_digital_port *intel_dig_port,
				    const struct intel_hdcp_shim *shim)
{
	int ret, read_ret;
	bool ksv_ready;

	/* Poll for ksv list ready (spec says max time allowed is 5s) */
	ret = __wait_for(read_ret = shim->read_ksv_ready(intel_dig_port,
							 &ksv_ready),
			 read_ret || ksv_ready, 5 * 1000 * 1000, 1000,
			 100 * 1000);
	if (ret)
		return ret;
	if (read_ret)
		return read_ret;
	if (!ksv_ready)
		return -ETIMEDOUT;

	return 0;
}

static bool hdcp_key_loadable(struct drm_i915_private *dev_priv)
{
	struct i915_power_domains *power_domains = &dev_priv->power_domains;
	struct i915_power_well *power_well;
	enum i915_power_well_id id;
	bool enabled = false;

	/*
	 * On HSW and BDW, Display HW loads the Key as soon as Display resumes.
	 * On all BXT+, SW can load the keys only when the PW#1 is turned on.
	 */
	if (IS_HASWELL(dev_priv) || IS_BROADWELL(dev_priv))
		id = HSW_DISP_PW_GLOBAL;
	else
		id = SKL_DISP_PW_1;

	mutex_lock(&power_domains->lock);

	/* PG1 (power well #1) needs to be enabled */
	for_each_power_well(dev_priv, power_well) {
		if (power_well->id == id) {
			enabled = power_well->ops->is_enabled(dev_priv,
							      power_well);
			break;
		}
	}
	mutex_unlock(&power_domains->lock);

	/*
	 * Another req for hdcp key loadability is enabled state of pll for
	 * cdclk. Without active crtc we wont land here. So we are assuming that
	 * cdclk is already on.
	 */

	return enabled;
}

static void intel_hdcp_clear_keys(struct drm_i915_private *dev_priv)
{
	I915_WRITE(HDCP_KEY_CONF, HDCP_CLEAR_KEYS_TRIGGER);
	I915_WRITE(HDCP_KEY_STATUS, HDCP_KEY_LOAD_DONE | HDCP_KEY_LOAD_STATUS |
		   HDCP_FUSE_IN_PROGRESS | HDCP_FUSE_ERROR | HDCP_FUSE_DONE);
}

static int intel_hdcp_load_keys(struct drm_i915_private *dev_priv)
{
	int ret;
	u32 val;

	val = I915_READ(HDCP_KEY_STATUS);
	if ((val & HDCP_KEY_LOAD_DONE) && (val & HDCP_KEY_LOAD_STATUS))
		return 0;

	/*
	 * On HSW and BDW HW loads the HDCP1.4 Key when Display comes
	 * out of reset. So if Key is not already loaded, its an error state.
	 */
	if (IS_HASWELL(dev_priv) || IS_BROADWELL(dev_priv))
		if (!(I915_READ(HDCP_KEY_STATUS) & HDCP_KEY_LOAD_DONE))
			return -ENXIO;

	/*
	 * Initiate loading the HDCP key from fuses.
	 *
	 * BXT+ platforms, HDCP key needs to be loaded by SW. Only SKL and KBL
	 * differ in the key load trigger process from other platforms.
	 */
	if (IS_SKYLAKE(dev_priv) || IS_KABYLAKE(dev_priv)) {
		mutex_lock(&dev_priv->pcu_lock);
		ret = sandybridge_pcode_write(dev_priv,
					      SKL_PCODE_LOAD_HDCP_KEYS, 1);
		mutex_unlock(&dev_priv->pcu_lock);
		if (ret) {
			DRM_ERROR("Failed to initiate HDCP key load (%d)\n",
			          ret);
			return ret;
		}
	} else {
		I915_WRITE(HDCP_KEY_CONF, HDCP_KEY_LOAD_TRIGGER);
	}

	/* Wait for the keys to load (500us) */
	ret = __intel_wait_for_register(dev_priv, HDCP_KEY_STATUS,
					HDCP_KEY_LOAD_DONE, HDCP_KEY_LOAD_DONE,
					10, 1, &val);
	if (ret)
		return ret;
	else if (!(val & HDCP_KEY_LOAD_STATUS))
		return -ENXIO;

	/* Send Aksv over to PCH display for use in authentication */
	I915_WRITE(HDCP_KEY_CONF, HDCP_AKSV_SEND_TRIGGER);

	return 0;
}

/* Returns updated SHA-1 index */
static int intel_write_sha_text(struct drm_i915_private *dev_priv, u32 sha_text)
{
	I915_WRITE(HDCP_SHA_TEXT, sha_text);
	if (intel_wait_for_register(dev_priv, HDCP_REP_CTL,
				    HDCP_SHA1_READY, HDCP_SHA1_READY, 1)) {
		DRM_ERROR("Timed out waiting for SHA1 ready\n");
		return -ETIMEDOUT;
	}
	return 0;
}

static
u32 intel_hdcp_get_repeater_ctl(struct intel_digital_port *intel_dig_port)
{
	enum port port = intel_dig_port->base.port;
	switch (port) {
	case PORT_A:
		return HDCP_DDIA_REP_PRESENT | HDCP_DDIA_SHA1_M0;
	case PORT_B:
		return HDCP_DDIB_REP_PRESENT | HDCP_DDIB_SHA1_M0;
	case PORT_C:
		return HDCP_DDIC_REP_PRESENT | HDCP_DDIC_SHA1_M0;
	case PORT_D:
		return HDCP_DDID_REP_PRESENT | HDCP_DDID_SHA1_M0;
	case PORT_E:
		return HDCP_DDIE_REP_PRESENT | HDCP_DDIE_SHA1_M0;
	default:
		break;
	}
	DRM_ERROR("Unknown port %d\n", port);
	return -EINVAL;
}

static
bool intel_hdcp_is_ksv_valid(u8 *ksv)
{
	int i, ones = 0;
	/* KSV has 20 1's and 20 0's */
	for (i = 0; i < DRM_HDCP_KSV_LEN; i++)
		ones += hweight8(ksv[i]);
	if (ones != 20)
		return false;
	return true;
}

struct intel_digital_port *conn_to_dig_port(struct intel_connector *connector)
{
	return enc_to_dig_port(&intel_attached_encoder(&connector->base)->base);
}

static inline void intel_hdcp_print_ksv(u8 *ksv)
{
	DRM_DEBUG_KMS("\t%#04x, %#04x, %#04x, %#04x, %#04x\n", *ksv,
		      *(ksv + 1), *(ksv + 2), *(ksv + 3), *(ksv + 4));
}

/* Check if any of the KSV is revocated by DCP LLC through SRM table */
static inline bool intel_hdcp_ksvs_revocated(struct intel_connector *connector,
					     u8 *ksvs, u32 ksv_count)
{
	u32 rev_ksv_cnt = connector->revocated_ksv_cnt;
	u8 *rev_ksv_list = connector->revocated_ksv_list;
	u32 cnt, i, j;

	/* If the Revocated ksv list is empty */
	if (!rev_ksv_cnt || !rev_ksv_list)
		return false;

	for  (cnt = 0; cnt < ksv_count; cnt++) {
		rev_ksv_list = connector->revocated_ksv_list;
		for (i = 0; i < rev_ksv_cnt; i++) {
			for (j = 0; j < DRM_HDCP_KSV_LEN; j++)
				if (*(ksvs + j) != *(rev_ksv_list + j)) {
					break;
				} else if (j == (DRM_HDCP_KSV_LEN - 1)) {
					DRM_DEBUG_KMS("Revocated KSV is ");
					intel_hdcp_print_ksv(ksvs);
					return true;
				}
			/* Move the offset to next KSV in the revocated list */
			rev_ksv_list += DRM_HDCP_KSV_LEN;
		}

		/* Iterate to next ksv_offset */
		ksvs += DRM_HDCP_KSV_LEN;
	}
	return false;
}

/* Implements Part 2 of the HDCP authorization procedure */
static
int intel_hdcp_auth_downstream(struct intel_connector *connector)
{
	struct intel_digital_port *intel_dig_port =
						conn_to_dig_port(connector);
	const struct intel_hdcp_shim *shim = connector->hdcp_shim;
	struct drm_i915_private *dev_priv = to_i915(connector->base.dev);
	u32 vprime, sha_text, sha_leftovers, rep_ctl;
	u8 bstatus[2], num_downstream, *ksv_fifo;
	int ret, i, j, sha_idx;

	if(intel_dig_port == NULL)
		return EINVAL;

	ret = intel_hdcp_poll_ksv_fifo(intel_dig_port, shim);
	if (ret) {
		DRM_ERROR("KSV list failed to become ready (%d)\n", ret);
		return ret;
	}

	ret = shim->read_bstatus(intel_dig_port, bstatus);
	if (ret)
		return ret;

	if (DRM_HDCP_MAX_DEVICE_EXCEEDED(bstatus[0]) ||
	    DRM_HDCP_MAX_CASCADE_EXCEEDED(bstatus[1])) {
		DRM_ERROR("Max Topology Limit Exceeded\n");
		return -EPERM;
	}

	/*
	 * When repeater reports 0 device count, HDCP1.4 spec allows disabling
	 * the HDCP encryption. That implies that repeater can't have its own
	 * display. As there is no consumption of encrypted content in the
	 * repeater with 0 downstream devices, we are failing the
	 * authentication.
	 */
	num_downstream = DRM_HDCP_NUM_DOWNSTREAM(bstatus[0]);
	if (num_downstream == 0)
		return -EINVAL;

	connector->downstream_info->device_count = num_downstream;
	connector->downstream_info->depth = DRM_HDCP_DEPTH(bstatus[1]);

	ksv_fifo = kzalloc(num_downstream * DRM_HDCP_KSV_LEN, GFP_KERNEL);
	if (!ksv_fifo)
		return -ENOMEM;

	ret = shim->read_ksv_fifo(intel_dig_port, num_downstream, ksv_fifo);
	if (ret)
		return ret;

	if (intel_hdcp_ksvs_revocated(connector, ksv_fifo, num_downstream)) {
		DRM_ERROR("Revocated Ksv(s) in ksv_fifo\n");
		return -EPERM;
	}

	memcpy(connector->downstream_info->ksv_list, ksv_fifo,
	       num_downstream * DRM_HDCP_KSV_LEN);

	/* Process V' values from the receiver */
	for (i = 0; i < DRM_HDCP_V_PRIME_NUM_PARTS; i++) {
		ret = shim->read_v_prime_part(intel_dig_port, i, &vprime);
		if (ret)
			return ret;
		I915_WRITE(HDCP_SHA_V_PRIME(i), vprime);
	}

	/*
	 * We need to write the concatenation of all device KSVs, BINFO (DP) ||
	 * BSTATUS (HDMI), and M0 (which is added via HDCP_REP_CTL). This byte
	 * stream is written via the HDCP_SHA_TEXT register in 32-bit
	 * increments. Every 64 bytes, we need to write HDCP_REP_CTL again. This
	 * index will keep track of our progress through the 64 bytes as well as
	 * helping us work the 40-bit KSVs through our 32-bit register.
	 *
	 * NOTE: data passed via HDCP_SHA_TEXT should be big-endian
	 */
	sha_idx = 0;
	sha_text = 0;
	sha_leftovers = 0;
	rep_ctl = intel_hdcp_get_repeater_ctl(intel_dig_port);
	I915_WRITE(HDCP_REP_CTL, rep_ctl | HDCP_SHA1_TEXT_32);
	for (i = 0; i < num_downstream; i++) {
		unsigned int sha_empty;
		u8 *ksv = &ksv_fifo[i * DRM_HDCP_KSV_LEN];

		/* Fill up the empty slots in sha_text and write it out */
		sha_empty = sizeof(sha_text) - sha_leftovers;
		for (j = 0; j < sha_empty; j++)
			sha_text |= ksv[j] << ((sizeof(sha_text) - j - 1) * 8);

		ret = intel_write_sha_text(dev_priv, sha_text);
		if (ret < 0)
			return ret;

		/* Programming guide writes this every 64 bytes */
		sha_idx += sizeof(sha_text);
		if (!(sha_idx % 64))
			I915_WRITE(HDCP_REP_CTL, rep_ctl | HDCP_SHA1_TEXT_32);

		/* Store the leftover bytes from the ksv in sha_text */
		sha_leftovers = DRM_HDCP_KSV_LEN - sha_empty;
		sha_text = 0;
		for (j = 0; j < sha_leftovers; j++)
			sha_text |= ksv[sha_empty + j] <<
					((sizeof(sha_text) - j - 1) * 8);

		/*
		 * If we still have room in sha_text for more data, continue.
		 * Otherwise, write it out immediately.
		 */
		if (sizeof(sha_text) > sha_leftovers)
			continue;

		ret = intel_write_sha_text(dev_priv, sha_text);
		if (ret < 0)
			return ret;
		sha_leftovers = 0;
		sha_text = 0;
		sha_idx += sizeof(sha_text);
	}

	/*
	 * We need to write BINFO/BSTATUS, and M0 now. Depending on how many
	 * bytes are leftover from the last ksv, we might be able to fit them
	 * all in sha_text (first 2 cases), or we might need to split them up
	 * into 2 writes (last 2 cases).
	 */
	if (sha_leftovers == 0) {
		/* Write 16 bits of text, 16 bits of M0 */
		I915_WRITE(HDCP_REP_CTL, rep_ctl | HDCP_SHA1_TEXT_16);
		ret = intel_write_sha_text(dev_priv,
					   bstatus[0] << 8 | bstatus[1]);
		if (ret < 0)
			return ret;
		sha_idx += sizeof(sha_text);

		/* Write 32 bits of M0 */
		I915_WRITE(HDCP_REP_CTL, rep_ctl | HDCP_SHA1_TEXT_0);
		ret = intel_write_sha_text(dev_priv, 0);
		if (ret < 0)
			return ret;
		sha_idx += sizeof(sha_text);

		/* Write 16 bits of M0 */
		I915_WRITE(HDCP_REP_CTL, rep_ctl | HDCP_SHA1_TEXT_16);
		ret = intel_write_sha_text(dev_priv, 0);
		if (ret < 0)
			return ret;
		sha_idx += sizeof(sha_text);

	} else if (sha_leftovers == 1) {
		/* Write 24 bits of text, 8 bits of M0 */
		I915_WRITE(HDCP_REP_CTL, rep_ctl | HDCP_SHA1_TEXT_24);
		sha_text |= bstatus[0] << 16 | bstatus[1] << 8;
		/* Only 24-bits of data, must be in the LSB */
		sha_text = (sha_text & 0xffffff00) >> 8;
		ret = intel_write_sha_text(dev_priv, sha_text);
		if (ret < 0)
			return ret;
		sha_idx += sizeof(sha_text);

		/* Write 32 bits of M0 */
		I915_WRITE(HDCP_REP_CTL, rep_ctl | HDCP_SHA1_TEXT_0);
		ret = intel_write_sha_text(dev_priv, 0);
		if (ret < 0)
			return ret;
		sha_idx += sizeof(sha_text);

		/* Write 24 bits of M0 */
		I915_WRITE(HDCP_REP_CTL, rep_ctl | HDCP_SHA1_TEXT_8);
		ret = intel_write_sha_text(dev_priv, 0);
		if (ret < 0)
			return ret;
		sha_idx += sizeof(sha_text);

	} else if (sha_leftovers == 2) {
		/* Write 32 bits of text */
		I915_WRITE(HDCP_REP_CTL, rep_ctl | HDCP_SHA1_TEXT_32);
		sha_text |= bstatus[0] << 24 | bstatus[1] << 16;
		ret = intel_write_sha_text(dev_priv, sha_text);
		if (ret < 0)
			return ret;
		sha_idx += sizeof(sha_text);

		/* Write 64 bits of M0 */
		I915_WRITE(HDCP_REP_CTL, rep_ctl | HDCP_SHA1_TEXT_0);
		for (i = 0; i < 2; i++) {
			ret = intel_write_sha_text(dev_priv, 0);
			if (ret < 0)
				return ret;
			sha_idx += sizeof(sha_text);
		}
	} else if (sha_leftovers == 3) {
		/* Write 32 bits of text */
		I915_WRITE(HDCP_REP_CTL, rep_ctl | HDCP_SHA1_TEXT_32);
		sha_text |= bstatus[0] << 24;
		ret = intel_write_sha_text(dev_priv, sha_text);
		if (ret < 0)
			return ret;
		sha_idx += sizeof(sha_text);

		/* Write 8 bits of text, 24 bits of M0 */
		I915_WRITE(HDCP_REP_CTL, rep_ctl | HDCP_SHA1_TEXT_8);
		ret = intel_write_sha_text(dev_priv, bstatus[1]);
		if (ret < 0)
			return ret;
		sha_idx += sizeof(sha_text);

		/* Write 32 bits of M0 */
		I915_WRITE(HDCP_REP_CTL, rep_ctl | HDCP_SHA1_TEXT_0);
		ret = intel_write_sha_text(dev_priv, 0);
		if (ret < 0)
			return ret;
		sha_idx += sizeof(sha_text);

		/* Write 8 bits of M0 */
		I915_WRITE(HDCP_REP_CTL, rep_ctl | HDCP_SHA1_TEXT_24);
		ret = intel_write_sha_text(dev_priv, 0);
		if (ret < 0)
			return ret;
		sha_idx += sizeof(sha_text);
	} else {
		DRM_DEBUG_KMS("Invalid number of leftovers %d\n",
			      sha_leftovers);
		return -EINVAL;
	}

	I915_WRITE(HDCP_REP_CTL, rep_ctl | HDCP_SHA1_TEXT_32);
	/* Fill up to 64-4 bytes with zeros (leave the last write for length) */
	while ((sha_idx % 64) < (64 - sizeof(sha_text))) {
		ret = intel_write_sha_text(dev_priv, 0);
		if (ret < 0)
			return ret;
		sha_idx += sizeof(sha_text);
	}

	/*
	 * Last write gets the length of the concatenation in bits. That is:
	 *  - 5 bytes per device
	 *  - 10 bytes for BINFO/BSTATUS(2), M0(8)
	 */
	sha_text = (num_downstream * 5 + 10) * 8;
	ret = intel_write_sha_text(dev_priv, sha_text);
	if (ret < 0)
		return ret;

	/* Tell the HW we're done with the hash and wait for it to ACK */
	I915_WRITE(HDCP_REP_CTL, rep_ctl | HDCP_SHA1_COMPLETE_HASH);
	if (intel_wait_for_register(dev_priv, HDCP_REP_CTL,
				    HDCP_SHA1_COMPLETE,
				    HDCP_SHA1_COMPLETE, 1)) {
		DRM_DEBUG_KMS("Timed out waiting for SHA1 complete\n");
		return -ETIMEDOUT;
	}
	if (!(I915_READ(HDCP_REP_CTL) & HDCP_SHA1_V_MATCH)) {
		DRM_DEBUG_KMS("SHA-1 mismatch, HDCP failed\n");
		return -ENXIO;
	}

	return 0;
}

/* Implements Part 1 of the HDCP authorization procedure */
static int intel_hdcp_auth(struct intel_connector *connector)
{
	struct intel_digital_port *intel_dig_port = conn_to_dig_port(connector);
	const struct intel_hdcp_shim *shim = connector->hdcp_shim;
	struct drm_i915_private *dev_priv = to_i915(connector->base.dev);
	enum port port;
	unsigned long r0_prime_gen_start;
	int ret, i, tries = 2;
	union {
		u32 reg[2];
		u8 shim[DRM_HDCP_AN_LEN];
	} an;
	union {
		u32 reg[2];
		u8 shim[DRM_HDCP_KSV_LEN];
	} bksv;
	union {
		u32 reg;
		u8 shim[DRM_HDCP_RI_LEN];
	} ri;
	bool repeater_present, hdcp_capable;

        if(intel_dig_port == NULL)
                return EINVAL;

	port = intel_dig_port->base.port;

	/*
	 * Detects whether the display is HDCP capable. Although we check for
	 * valid Bksv below, the HDCP over DP spec requires that we check
	 * whether the display supports HDCP before we write An. For HDMI
	 * displays, this is not necessary.
	 */
	if (shim->hdcp_capable) {
		ret = shim->hdcp_capable(intel_dig_port, &hdcp_capable);
		if (ret)
			return ret;
		if (!hdcp_capable) {
			DRM_ERROR("Panel is not HDCP capable\n");
			return -EINVAL;
		}
	}

	/* Initialize An with 2 random values and acquire it */
	for (i = 0; i < 2; i++)
		I915_WRITE(PORT_HDCP_ANINIT(port), get_random_u32());
	I915_WRITE(PORT_HDCP_CONF(port), HDCP_CONF_CAPTURE_AN);

	/* Wait for An to be acquired */
	if (intel_wait_for_register(dev_priv, PORT_HDCP_STATUS(port),
				    HDCP_STATUS_AN_READY,
				    HDCP_STATUS_AN_READY, 1)) {
		DRM_ERROR("Timed out waiting for An\n");
		return -ETIMEDOUT;
	}

	an.reg[0] = I915_READ(PORT_HDCP_ANLO(port));
	an.reg[1] = I915_READ(PORT_HDCP_ANHI(port));
	ret = shim->write_an_aksv(intel_dig_port, an.shim);
	if (ret)
		return ret;

	r0_prime_gen_start = jiffies;

	memset(&bksv, 0, sizeof(bksv));

	/* HDCP spec states that we must retry the bksv if it is invalid */
	for (i = 0; i < tries; i++) {
		ret = shim->read_bksv(intel_dig_port, bksv.shim);
		if (ret)
			return ret;
		if (intel_hdcp_is_ksv_valid(bksv.shim))
			break;
	}
	if (i == tries) {
		DRM_ERROR("HDCP failed, Bksv is invalid\n");
		return -ENODEV;
	}

	if (intel_hdcp_ksvs_revocated(connector, bksv.shim, 1)) {
		DRM_ERROR("BKSV is revocated\n");
		return -EPERM;
	}

	memcpy(connector->downstream_info->bksv, bksv.shim,
	       DRM_MODE_HDCP_KSV_LEN);

	I915_WRITE(PORT_HDCP_BKSVLO(port), bksv.reg[0]);
	I915_WRITE(PORT_HDCP_BKSVHI(port), bksv.reg[1]);

	ret = shim->repeater_present(intel_dig_port, &repeater_present);
	if (ret)
		return ret;
	if (repeater_present) {
		I915_WRITE(HDCP_REP_CTL,
			   intel_hdcp_get_repeater_ctl(intel_dig_port));
		connector->downstream_info->is_repeater = true;
	}

	ret = shim->toggle_signalling(intel_dig_port, true);
	if (ret)
		return ret;

	I915_WRITE(PORT_HDCP_CONF(port), HDCP_CONF_AUTH_AND_ENC);

	/* Wait for R0 ready */
	if (wait_for(I915_READ(PORT_HDCP_STATUS(port)) &
		     (HDCP_STATUS_R0_READY | HDCP_STATUS_ENC), 1)) {
		DRM_ERROR("Timed out waiting for R0 ready\n");
		return -ETIMEDOUT;
	}

	/*
	 * Wait for R0' to become available. The spec says 100ms from Aksv, but
	 * some monitors can take longer than this. We'll set the timeout at
	 * 300ms just to be sure.
	 *
	 * On DP, there's an R0_READY bit available but no such bit
	 * exists on HDMI. Since the upper-bound is the same, we'll just do
	 * the stupid thing instead of polling on one and not the other.
	 */
	wait_remaining_ms_from_jiffies(r0_prime_gen_start, 300);

	tries = 3;

	/*
	 * DP HDCP Spec mandates the two more reattempt to read R0, incase
	 * of R0 mismatch.
	 */
	for (i = 0; i < tries; i++) {
		ri.reg = 0;
		ret = shim->read_ri_prime(intel_dig_port, ri.shim);
		if (ret)
			return ret;
		I915_WRITE(PORT_HDCP_RPRIME(port), ri.reg);

		/* Wait for Ri prime match */
		if (!wait_for(I915_READ(PORT_HDCP_STATUS(port)) &
		    (HDCP_STATUS_RI_MATCH | HDCP_STATUS_ENC), 1))
			break;
	}

	if (i == tries) {
		DRM_ERROR("Timed out waiting for Ri prime match (%x)\n",
			  I915_READ(PORT_HDCP_STATUS(port)));
		return -ETIMEDOUT;
	}

	/* Wait for encryption confirmation */
	if (intel_wait_for_register(dev_priv, PORT_HDCP_STATUS(port),
				    HDCP_STATUS_ENC, HDCP_STATUS_ENC, 20)) {
		DRM_ERROR("Timed out waiting for encryption\n");
		return -ETIMEDOUT;
	}

	/*
	 * XXX: If we have MST-connected devices, we need to enable encryption
	 * on those as well.
	 */

	if (repeater_present)
		return intel_hdcp_auth_downstream(connector);

	DRM_DEBUG_KMS("HDCP is enabled (no repeater present)\n");
	return 0;
}

static int _intel_hdcp_disable(struct intel_connector *connector)
{
	struct drm_i915_private *dev_priv = connector->base.dev->dev_private;
	struct intel_digital_port *intel_dig_port = conn_to_dig_port(connector);
	enum port port = intel_dig_port->base.port;
	int ret;

	DRM_DEBUG_KMS("[%s:%d] HDCP is being disabled...\n",
		      connector->base.name, connector->base.base.id);

	I915_WRITE(PORT_HDCP_CONF(port), 0);
	if (intel_wait_for_register(dev_priv, PORT_HDCP_STATUS(port), ~0, 0,
				    20)) {
		DRM_ERROR("Failed to disable HDCP, timeout clearing status\n");
		return -ETIMEDOUT;
	}

	ret = connector->hdcp_shim->toggle_signalling(intel_dig_port, false);
	if (ret) {
		DRM_ERROR("Failed to disable HDCP signalling\n");
		return ret;
	}

	memset(connector->downstream_info, 0,
	       sizeof(struct cp_downstream_info));

	DRM_DEBUG_KMS("HDCP is disabled\n");
	return 0;
}

static int _intel_hdcp_enable(struct intel_connector *connector)
{
	struct drm_i915_private *dev_priv = connector->base.dev->dev_private;
	int i, ret, tries = 3;

	DRM_DEBUG_KMS("[%s:%d] HDCP is being enabled...\n",
		      connector->base.name, connector->base.base.id);

	if (!hdcp_key_loadable(dev_priv)) {
		DRM_ERROR("HDCP key Load is not possible\n");
		return -ENXIO;
	}

	for (i = 0; i < KEY_LOAD_TRIES; i++) {
		ret = intel_hdcp_load_keys(dev_priv);
		if (!ret)
			break;
		intel_hdcp_clear_keys(dev_priv);
	}
	if (ret) {
		DRM_ERROR("Could not load HDCP keys, (%d)\n", ret);
		return ret;
	}

	/* Incase of authentication failures, HDCP spec expects reauth. */
	for (i = 0; i < tries; i++) {
		ret = intel_hdcp_auth(connector);
		if (!ret) {
			connector->hdcp_value =
					DRM_MODE_CONTENT_PROTECTION_ENABLED;
			schedule_work(&connector->hdcp_prop_work);
			schedule_delayed_work(&connector->hdcp_check_work,
					DRM_HDCP_CHECK_PERIOD_MS);
			return 0;
		}

		DRM_DEBUG_KMS("HDCP Auth failure (%d)\n", ret);

		/* Ensuring HDCP encryption and signalling are stopped. */
		_intel_hdcp_disable(connector);
	}

	memset(connector->downstream_info, 0,
	       sizeof(struct cp_downstream_info));

	DRM_ERROR("HDCP authentication failed (%d tries/%d)\n", tries, ret);
	return ret;
}

static void intel_hdcp_enable_work(struct work_struct *work)
{
	struct intel_connector *connector = container_of(work,
							 struct intel_connector,
							 hdcp_enable_work);
	int ret;

	mutex_lock(&connector->hdcp_mutex);
	ret = _intel_hdcp_enable(connector);
	if (!ret) {
		ret = drm_mode_connector_update_cp_downstream_property(
						&connector->base,
						connector->downstream_info);
		if (ret)
			DRM_ERROR("Downstream_property update failed.%d\n",
				  ret);
	}
	mutex_unlock(&connector->hdcp_mutex);
}

static void intel_hdcp_check_work(struct work_struct *work)
{
	struct intel_connector *connector = container_of(to_delayed_work(work),
							 struct intel_connector,
							 hdcp_check_work);
	if (!intel_hdcp_check_link(connector))
		schedule_delayed_work(&connector->hdcp_check_work,
				      DRM_HDCP_CHECK_PERIOD_MS);
}

static void intel_hdcp_prop_work(struct work_struct *work)
{
	struct intel_connector *connector = container_of(work,
							 struct intel_connector,
							 hdcp_prop_work);
	struct drm_device *dev = connector->base.dev;
	struct drm_connector_state *state;

	drm_modeset_lock(&dev->mode_config.connection_mutex, NULL);
	mutex_lock(&connector->hdcp_mutex);

	/*
	 * This worker is only used to flip between ENABLED/DESIRED. Either of
	 * those to UNDESIRED is handled by core. If hdcp_value == UNDESIRED,
	 * we're running just after hdcp has been disabled, so just exit
	 */
	if (connector->hdcp_value != DRM_MODE_CONTENT_PROTECTION_UNDESIRED) {
		state = connector->base.state;
		state->content_protection = connector->hdcp_value;
	}

	mutex_unlock(&connector->hdcp_mutex);
	drm_modeset_unlock(&dev->mode_config.connection_mutex);
}

bool is_hdcp_supported(struct drm_i915_private *dev_priv, enum port port)
{
	/* PORT E doesn't have HDCP, and PORT F is disabled */
	return ((INTEL_GEN(dev_priv) >= 8 || IS_HASWELL(dev_priv)) &&
		!IS_CHERRYVIEW(dev_priv) && port < PORT_E);
}

int intel_hdcp_init(struct intel_connector *connector,
		    const struct intel_hdcp_shim *hdcp_shim)
{
	int ret;

	ret = drm_connector_attach_content_protection_property(
			&connector->base);
	if (ret)
		return ret;

	ret = drm_connector_attach_cp_srm_property(&connector->base);
	if (ret)
		return ret;

	ret = drm_connector_attach_cp_downstream_property(&connector->base);
	if (ret)
		return ret;

	connector->downstream_info = kzalloc(sizeof(struct cp_downstream_info),
					     GFP_KERNEL);
	if (!connector->downstream_info)
		return -ENOMEM;

	connector->hdcp_shim = hdcp_shim;
	mutex_init(&connector->hdcp_mutex);
	INIT_DELAYED_WORK(&connector->hdcp_check_work, intel_hdcp_check_work);
	INIT_WORK(&connector->hdcp_prop_work, intel_hdcp_prop_work);
	INIT_WORK(&connector->hdcp_enable_work, intel_hdcp_enable_work);
	return 0;
}

int intel_hdcp_enable(struct intel_connector *connector)
{
	if (!connector->hdcp_shim)
		return -ENOENT;

	mutex_lock(&connector->hdcp_mutex);
	schedule_work(&connector->hdcp_enable_work);
	mutex_unlock(&connector->hdcp_mutex);

	return 0;
}

int intel_hdcp_disable(struct intel_connector *connector)
{
	int ret = 0;

	if (!connector->hdcp_shim)
		return -ENOENT;

	mutex_lock(&connector->hdcp_mutex);

	if (connector->hdcp_value != DRM_MODE_CONTENT_PROTECTION_UNDESIRED) {
		connector->hdcp_value = DRM_MODE_CONTENT_PROTECTION_UNDESIRED;
		ret = _intel_hdcp_disable(connector);
	}

	mutex_unlock(&connector->hdcp_mutex);
	cancel_delayed_work_sync(&connector->hdcp_check_work);
	return ret;
}

void intel_hdcp_atomic_check(struct drm_connector *connector,
			     struct drm_connector_state *old_state,
			     struct drm_connector_state *new_state)
{
	uint64_t old_cp = old_state->content_protection;
	uint64_t new_cp = new_state->content_protection;

	if (!new_state->crtc) {
		/*
		 * If the connector is being disabled with CP enabled, mark it
		 * desired so it's re-enabled when the connector is brought back
		 */
		if (old_cp == DRM_MODE_CONTENT_PROTECTION_ENABLED)
			new_state->content_protection =
				DRM_MODE_CONTENT_PROTECTION_DESIRED;
		return;
	}

	/*
	 * Nothing to do if the state didn't change, or HDCP was activated since
	 * the last commit
	 */
	if (old_cp == new_cp ||
	    (old_cp == DRM_MODE_CONTENT_PROTECTION_DESIRED &&
	     new_cp == DRM_MODE_CONTENT_PROTECTION_ENABLED))
		return;
}

void intel_hdcp_atomic_pre_commit(struct drm_connector *connector,
				  struct drm_connector_state *old_state,
				  struct drm_connector_state *new_state)
{
	uint64_t old_cp = old_state->content_protection;
	uint64_t new_cp = new_state->content_protection;

	/*
	 * Disable HDCP if the connector is becoming disabled, or if requested
	 * via the property.
	 */
	if ((!new_state->crtc &&
	    old_cp != DRM_MODE_CONTENT_PROTECTION_UNDESIRED) ||
	    (new_state->crtc &&
	    old_cp != DRM_MODE_CONTENT_PROTECTION_UNDESIRED &&
	    new_cp == DRM_MODE_CONTENT_PROTECTION_UNDESIRED))
		intel_hdcp_disable(to_intel_connector(connector));
}

static u32 intel_hdcp_get_revocated_ksv_count(u8 *buf, u32 vrls_length)
{
	u32 parsed_bytes = 0, ksv_count = 0, vrl_ksv_cnt, vrl_sz;

	do {
		vrl_ksv_cnt = *buf;
		ksv_count += vrl_ksv_cnt;

		vrl_sz = (vrl_ksv_cnt * DRM_HDCP_KSV_LEN) + 1;
		buf += vrl_sz;
		parsed_bytes += vrl_sz;
	} while (parsed_bytes < vrls_length);

	return ksv_count;
}

static u32 intel_hdcp_get_revocated_ksvs(u8 *ksv_list, const u8 *buf,
					u32 vrls_length)
{
	u32 parsed_bytes = 0, ksv_count = 0;
	u32 vrl_ksv_cnt, vrl_ksv_sz, vrl_idx = 0;

	do {
		vrl_ksv_cnt = *buf;
		vrl_ksv_sz = vrl_ksv_cnt * DRM_HDCP_KSV_LEN;

		buf++;

		DRM_INFO("vrl: %d, Revoked KSVs: %d\n", vrl_idx++,
							vrl_ksv_cnt);
		memcpy(ksv_list, buf, vrl_ksv_sz);

		ksv_count += vrl_ksv_cnt;
		ksv_list += vrl_ksv_sz;
		buf += vrl_ksv_sz;

		parsed_bytes += (vrl_ksv_sz + 1);
	} while (parsed_bytes < vrls_length);

	return ksv_count;
}

static int intel_hdcp_parse_srm(struct drm_connector *connector,
				struct drm_property_blob *blob)
{
	struct intel_connector *intel_connector = to_intel_connector(connector);
	struct cp_srm_header *header;
	u32 vrl_length, ksv_count;
	u8 *buf;

	if (blob->length < (sizeof(struct cp_srm_header) +
			    DRM_HDCP_1_4_VRL_LENGTH_SIZE +
			    DRM_HDCP_1_4_DCP_SIG_SIZE)) {
		DRM_ERROR("Invalid blob length\n");
		return -EINVAL;
	}

	header = (struct cp_srm_header *)blob->data;

	DRM_INFO("SRM ID: 0x%x, SRM Ver: 0x%x, SRM Gen No: 0x%x\n",
				header->spec_indicator.srm_id,
				__swab16(header->srm_version),
				header->srm_gen_no);

	WARN_ON(header->spec_indicator.reserved_hi ||
			header->spec_indicator.reserved_lo);

	if (header->spec_indicator.srm_id != DRM_HDCP_1_4_SRM_ID) {
		DRM_ERROR("Invalid srm_id\n");
		return -EINVAL;
	}

	buf = blob->data + sizeof(*header);

	vrl_length = (*buf << 16 | *(buf + 1) << 8 | *(buf + 2));

	if (blob->length < (sizeof(struct cp_srm_header) + vrl_length) ||
		vrl_length < (DRM_HDCP_1_4_VRL_LENGTH_SIZE +
			      DRM_HDCP_1_4_DCP_SIG_SIZE)) {
		DRM_ERROR("Invalid blob length or vrl length\n");
		return -EINVAL;
	}

	/* Length of the all vrls combined */
	vrl_length -= (DRM_HDCP_1_4_VRL_LENGTH_SIZE +
		       DRM_HDCP_1_4_DCP_SIG_SIZE);

	if (!vrl_length) {
		DRM_DEBUG("No vrl found\n");
		return -EINVAL;
	}

	buf += DRM_HDCP_1_4_VRL_LENGTH_SIZE;


	ksv_count = intel_hdcp_get_revocated_ksv_count(buf, vrl_length);
	if (!ksv_count) {
		DRM_INFO("Revocated KSV count is 0\n");
		return 0;
	}

	kfree(intel_connector->revocated_ksv_list);
	intel_connector->revocated_ksv_list = kzalloc(ksv_count *
						DRM_HDCP_KSV_LEN, GFP_KERNEL);
	if (!intel_connector->revocated_ksv_list) {
		DRM_ERROR("Out of Memory\n");
		return -ENOMEM;
	}

	if (intel_hdcp_get_revocated_ksvs(intel_connector->revocated_ksv_list,
				      buf, vrl_length) != ksv_count) {
		intel_connector->revocated_ksv_cnt = 0;
		kfree(intel_connector->revocated_ksv_list);
		return -EINVAL;
	}

	intel_connector->revocated_ksv_cnt = ksv_count;
	return 0;
}

static void intel_hdcp_update_srm(struct drm_connector *connector,
				  u32 srm_blob_id)
{
	struct intel_connector *intel_connector = to_intel_connector(connector);
	struct drm_property_blob *blob;

	blob = drm_property_lookup_blob(connector->dev, srm_blob_id);
	if (!blob || !blob->data)
		return;

	if (!intel_hdcp_parse_srm(connector, blob))
		intel_connector->srm_blob_id = srm_blob_id;

	drm_property_blob_put(blob);
}

void intel_hdcp_atomic_commit(struct drm_connector *connector,
			      struct drm_connector_state *new_state)
{
	struct intel_connector *intel_connector = to_intel_connector(connector);
	uint64_t new_cp = new_state->content_protection;

	if (new_state->cp_srm_blob_id &&
		new_state->cp_srm_blob_id != intel_connector->srm_blob_id)
		intel_hdcp_update_srm(connector, new_state->cp_srm_blob_id);

	/* Enable hdcp if it's desired */
	if (new_state->crtc && new_cp == DRM_MODE_CONTENT_PROTECTION_DESIRED)
		intel_hdcp_enable(to_intel_connector(connector));
}

/* Implements Part 3 of the HDCP authorization procedure */
int intel_hdcp_check_link(struct intel_connector *connector)
{
	struct drm_i915_private *dev_priv = connector->base.dev->dev_private;
	struct intel_digital_port *intel_dig_port = conn_to_dig_port(connector);
	enum port port = intel_dig_port->base.port;
	int ret = 0;

	if (!connector->hdcp_shim)
		return -ENOENT;

	mutex_lock(&connector->hdcp_mutex);

	if (connector->hdcp_value == DRM_MODE_CONTENT_PROTECTION_UNDESIRED)
		goto out;

	if (!(I915_READ(PORT_HDCP_STATUS(port)) & HDCP_STATUS_ENC)) {
		DRM_ERROR("%s:%d HDCP check failed: link is not encrypted,%x\n",
			  connector->base.name, connector->base.base.id,
			  I915_READ(PORT_HDCP_STATUS(port)));
		ret = -ENXIO;
		connector->hdcp_value = DRM_MODE_CONTENT_PROTECTION_DESIRED;
		schedule_work(&connector->hdcp_prop_work);
		goto out;
	}

	if (connector->hdcp_shim->check_link(intel_dig_port)) {
		if (connector->hdcp_value !=
		    DRM_MODE_CONTENT_PROTECTION_UNDESIRED) {
			connector->hdcp_value =
				DRM_MODE_CONTENT_PROTECTION_ENABLED;
			schedule_work(&connector->hdcp_prop_work);
		}
		goto out;
	}

	DRM_DEBUG_KMS("[%s:%d] HDCP link failed, retrying authentication\n",
		      connector->base.name, connector->base.base.id);

	ret = _intel_hdcp_disable(connector);
	if (ret) {
		DRM_ERROR("Failed to disable hdcp (%d)\n", ret);
		connector->hdcp_value = DRM_MODE_CONTENT_PROTECTION_DESIRED;
		schedule_work(&connector->hdcp_prop_work);
		goto out;
	}

	ret = _intel_hdcp_enable(connector);
	if (ret) {
		DRM_ERROR("Failed to enable hdcp (%d)\n", ret);
		connector->hdcp_value = DRM_MODE_CONTENT_PROTECTION_DESIRED;
		schedule_work(&connector->hdcp_prop_work);
		goto out;
	}

out:
	mutex_unlock(&connector->hdcp_mutex);
	return ret;
}
