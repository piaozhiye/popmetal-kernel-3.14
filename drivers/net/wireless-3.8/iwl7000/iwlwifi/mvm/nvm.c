/******************************************************************************
 *
 * This file is provided under a dual BSD/GPLv2 license.  When using or
 * redistributing this file, you may do so under either license.
 *
 * GPL LICENSE SUMMARY
 *
 * Copyright(c) 2012 - 2014 Intel Corporation. All rights reserved.
 * Copyright(c) 2013 - 2014 Intel Mobile Communications GmbH
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of version 2 of the GNU General Public License as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110,
 * USA
 *
 * The full GNU General Public License is included in this distribution
 * in the file called COPYING.
 *
 * Contact Information:
 *  Intel Linux Wireless <ilw@linux.intel.com>
 * Intel Corporation, 5200 N.E. Elam Young Parkway, Hillsboro, OR 97124-6497
 *
 * BSD LICENSE
 *
 * Copyright(c) 2012 - 2014 Intel Corporation. All rights reserved.
 * Copyright(c) 2013 - 2014 Intel Mobile Communications GmbH
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 *  * Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *  * Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 *  * Neither the name Intel Corporation nor the names of its
 *    contributors may be used to endorse or promote products derived
 *    from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 *****************************************************************************/
#include <linux/firmware.h>
#include <linux/rtnetlink.h>
#include "iwl-trans.h"
#include "mvm.h"
#include "iwl-eeprom-parse.h"
#include "iwl-eeprom-read.h"
#include "iwl-nvm-parse.h"

/* Default NVM size to read */
#define IWL_NVM_DEFAULT_CHUNK_SIZE (2*1024)
#define IWL_MAX_NVM_SECTION_SIZE	0x1b58
#define IWL_MAX_NVM_8000A_SECTION_SIZE	0xffc
#define IWL_MAX_NVM_8000B_SECTION_SIZE	0x1ffc

#define NVM_WRITE_OPCODE 1
#define NVM_READ_OPCODE 0

/* load nvm chunk response */
enum {
	READ_NVM_CHUNK_SUCCEED = 0,
	READ_NVM_CHUNK_NOT_VALID_ADDRESS = 1
};

/*
 * prepare the NVM host command w/ the pointers to the nvm buffer
 * and send it to fw
 */
static int iwl_nvm_write_chunk(struct iwl_mvm *mvm, u16 section,
			       u16 offset, u16 length, const u8 *data)
{
	struct iwl_nvm_access_cmd nvm_access_cmd = {
		.offset = cpu_to_le16(offset),
		.length = cpu_to_le16(length),
		.type = cpu_to_le16(section),
		.op_code = NVM_WRITE_OPCODE,
	};
	struct iwl_host_cmd cmd = {
		.id = NVM_ACCESS_CMD,
		.len = { sizeof(struct iwl_nvm_access_cmd), length },
		.flags = CMD_SEND_IN_RFKILL,
		.data = { &nvm_access_cmd, data },
		/* data may come from vmalloc, so use _DUP */
		.dataflags = { 0, IWL_HCMD_DFL_DUP },
	};

	return iwl_mvm_send_cmd(mvm, &cmd);
}

static int iwl_nvm_read_chunk(struct iwl_mvm *mvm, u16 section,
			      u16 offset, u16 length, u8 *data)
{
	struct iwl_nvm_access_cmd nvm_access_cmd = {
		.offset = cpu_to_le16(offset),
		.length = cpu_to_le16(length),
		.type = cpu_to_le16(section),
		.op_code = NVM_READ_OPCODE,
	};
	struct iwl_nvm_access_resp *nvm_resp;
	struct iwl_rx_packet *pkt;
	struct iwl_host_cmd cmd = {
		.id = NVM_ACCESS_CMD,
		.flags = CMD_WANT_SKB | CMD_SEND_IN_RFKILL,
		.data = { &nvm_access_cmd, },
	};
	int ret, bytes_read, offset_read;
	u8 *resp_data;

	cmd.len[0] = sizeof(struct iwl_nvm_access_cmd);

	ret = iwl_mvm_send_cmd(mvm, &cmd);
	if (ret)
		return ret;

	pkt = cmd.resp_pkt;
	if (pkt->hdr.flags & IWL_CMD_FAILED_MSK) {
		IWL_ERR(mvm, "Bad return from NVM_ACCES_COMMAND (0x%08X)\n",
			pkt->hdr.flags);
		ret = -EIO;
		goto exit;
	}

	/* Extract NVM response */
	nvm_resp = (void *)pkt->data;
	ret = le16_to_cpu(nvm_resp->status);
	bytes_read = le16_to_cpu(nvm_resp->length);
	offset_read = le16_to_cpu(nvm_resp->offset);
	resp_data = nvm_resp->data;
	if (ret) {
		if ((offset != 0) &&
		    (ret == READ_NVM_CHUNK_NOT_VALID_ADDRESS)) {
			/*
			 * meaning of NOT_VALID_ADDRESS:
			 * driver try to read chunk from address that is
			 * multiple of 2K and got an error since addr is empty.
			 * meaning of (offset != 0): driver already
			 * read valid data from another chunk so this case
			 * is not an error.
			 */
			IWL_DEBUG_EEPROM(mvm->trans->dev,
					 "NVM access command failed on offset 0x%x since that section size is multiple 2K\n",
					 offset);
			ret = 0;
		} else {
			IWL_DEBUG_EEPROM(mvm->trans->dev,
					 "NVM access command failed with status %d (device: %s)\n",
					 ret, mvm->cfg->name);
			ret = -EIO;
		}
		goto exit;
	}

	if (offset_read != offset) {
		IWL_ERR(mvm, "NVM ACCESS response with invalid offset %d\n",
			offset_read);
		ret = -EINVAL;
		goto exit;
	}

	/* Write data to NVM */
	memcpy(data + offset, resp_data, bytes_read);
	ret = bytes_read;

exit:
	iwl_free_resp(&cmd);
	return ret;
}

static int iwl_nvm_write_section(struct iwl_mvm *mvm, u16 section,
				 const u8 *data, u16 length)
{
	int offset = 0;

	/* copy data in chunks of 2k (and remainder if any) */

	while (offset < length) {
		int chunk_size, ret;

		chunk_size = min(IWL_NVM_DEFAULT_CHUNK_SIZE,
				 length - offset);

		ret = iwl_nvm_write_chunk(mvm, section, offset,
					  chunk_size, data + offset);
		if (ret < 0)
			return ret;

		offset += chunk_size;
	}

	return 0;
}

/*
 * Reads an NVM section completely.
 * NICs prior to 7000 family doesn't have a real NVM, but just read
 * section 0 which is the EEPROM. Because the EEPROM reading is unlimited
 * by uCode, we need to manually check in this case that we don't
 * overflow and try to read more than the EEPROM size.
 * For 7000 family NICs, we supply the maximal size we can read, and
 * the uCode fills the response with as much data as we can,
 * without overflowing, so no check is needed.
 */
static int iwl_nvm_read_section(struct iwl_mvm *mvm, u16 section,
				u8 *data, u32 size_read)
{
	u16 length, offset = 0;
	int ret;

	/* Set nvm section read length */
	length = IWL_NVM_DEFAULT_CHUNK_SIZE;

	ret = length;

	/* Read the NVM until exhausted (reading less than requested) */
	while (ret == length) {
		/* Check no memory assumptions fail and cause an overflow */
		if ((size_read + offset + length) >
		    mvm->cfg->base_params->eeprom_size) {
			IWL_ERR(mvm, "EEPROM size is too small for NVM\n");
			return -ENOBUFS;
		}

		ret = iwl_nvm_read_chunk(mvm, section, offset, length, data);
		if (ret < 0) {
			IWL_DEBUG_EEPROM(mvm->trans->dev,
					 "Cannot read NVM from section %d offset %d, length %d\n",
					 section, offset, length);
			return ret;
		}
		offset += ret;
	}

	IWL_DEBUG_EEPROM(mvm->trans->dev,
			 "NVM section %d read completed\n", section);
	return offset;
}

static struct iwl_nvm_data *
iwl_parse_nvm_sections(struct iwl_mvm *mvm)
{
	struct iwl_nvm_section *sections = mvm->nvm_sections;
	const __le16 *hw, *sw, *calib, *regulatory, *mac_override;

	/* Checking for required sections */
	if (mvm->trans->cfg->device_family != IWL_DEVICE_FAMILY_8000) {
		if (!mvm->nvm_sections[NVM_SECTION_TYPE_SW].data ||
		    !mvm->nvm_sections[mvm->cfg->nvm_hw_section_num].data) {
			IWL_ERR(mvm, "Can't parse empty OTP/NVM sections\n");
			return NULL;
		}
	} else {
		/* SW and REGULATORY sections are mandatory */
		if (!mvm->nvm_sections[NVM_SECTION_TYPE_SW].data ||
		    !mvm->nvm_sections[NVM_SECTION_TYPE_REGULATORY].data) {
			IWL_ERR(mvm,
				"Can't parse empty family 8000 OTP/NVM sections\n");
			return NULL;
		}
		/* MAC_OVERRIDE or at least HW section must exist */
		if (!mvm->nvm_sections[mvm->cfg->nvm_hw_section_num].data &&
		    !mvm->nvm_sections[NVM_SECTION_TYPE_MAC_OVERRIDE].data) {
			IWL_ERR(mvm,
				"Can't parse mac_address, empty sections\n");
			return NULL;
		}
	}

	if (WARN_ON(!mvm->cfg))
		return NULL;

	hw = (const __le16 *)sections[mvm->cfg->nvm_hw_section_num].data;
	sw = (const __le16 *)sections[NVM_SECTION_TYPE_SW].data;
	calib = (const __le16 *)sections[NVM_SECTION_TYPE_CALIBRATION].data;
	regulatory = (const __le16 *)sections[NVM_SECTION_TYPE_REGULATORY].data;
	mac_override =
		(const __le16 *)sections[NVM_SECTION_TYPE_MAC_OVERRIDE].data;

	return iwl_parse_nvm_data(mvm->trans->dev, mvm->cfg, hw, sw, calib,
				  regulatory, mac_override,
				  mvm->fw->valid_tx_ant,
				  mvm->fw->valid_rx_ant,
				  mvm->fw->ucode_capa.capa[0] &
				  IWL_UCODE_TLV_CAPA_LAR_SUPPORT);
}

#define MAX_NVM_FILE_LEN	16384

/*
 * Reads external NVM from a file into mvm->nvm_sections
 *
 * HOW TO CREATE THE NVM FILE FORMAT:
 * ------------------------------
 * 1. create hex file, format:
 *      3800 -> header
 *      0000 -> header
 *      5a40 -> data
 *
 *   rev - 6 bit (word1)
 *   len - 10 bit (word1)
 *   id - 4 bit (word2)
 *   rsv - 12 bit (word2)
 *
 * 2. flip 8bits with 8 bits per line to get the right NVM file format
 *
 * 3. create binary file from the hex file
 *
 * 4. save as "iNVM_xxx.bin" under /lib/firmware
 */
static int iwl_mvm_read_external_nvm(struct iwl_mvm *mvm)
{
	int ret, section_size;
	u16 section_id;
	const struct firmware *fw_entry;
	const struct {
		__le16 word1;
		__le16 word2;
		u8 data[];
	} *file_sec;
	const u8 *eof, *temp;
	int max_section_size;

#define NVM_WORD1_LEN(x) (8 * (x & 0x03FF))
#define NVM_WORD2_ID(x) (x >> 12)
#define NVM_WORD2_LEN_FAMILY_8000(x) (2 * ((x & 0xFF) << 8 | x >> 8))
#define NVM_WORD1_ID_FAMILY_8000(x) (x >> 4)

	IWL_DEBUG_EEPROM(mvm->trans->dev, "Read from external NVM\n");

	/* Maximal size depends on HW family and step */
	if (mvm->trans->cfg->device_family != IWL_DEVICE_FAMILY_8000)
		max_section_size = IWL_MAX_NVM_SECTION_SIZE;
	else if ((mvm->trans->hw_rev & 0xc) == 0) /* Family 8000 A-step */
		max_section_size = IWL_MAX_NVM_8000A_SECTION_SIZE;
	else /* Family 8000 B-step */
		max_section_size = IWL_MAX_NVM_8000B_SECTION_SIZE;

	/*
	 * Obtain NVM image via request_firmware. Since we already used
	 * request_firmware_nowait() for the firmware binary load and only
	 * get here after that we assume the NVM request can be satisfied
	 * synchronously.
	 */
	ret = request_firmware(&fw_entry, mvm->nvm_file_name,
			       mvm->trans->dev);
	if (ret) {
		IWL_ERR(mvm, "ERROR: %s isn't available %d\n",
			mvm->nvm_file_name, ret);
		return ret;
	}

	IWL_INFO(mvm, "Loaded NVM file %s (%zu bytes)\n",
		 mvm->nvm_file_name, fw_entry->size);

	if (fw_entry->size < sizeof(*file_sec)) {
		IWL_ERR(mvm, "NVM file too small\n");
		ret = -EINVAL;
		goto out;
	}

	if (fw_entry->size > MAX_NVM_FILE_LEN) {
		IWL_ERR(mvm, "NVM file too large\n");
		ret = -EINVAL;
		goto out;
	}

	eof = fw_entry->data + fw_entry->size;

	file_sec = (void *)fw_entry->data;

	while (true) {
		if (file_sec->data > eof) {
			IWL_ERR(mvm,
				"ERROR - NVM file too short for section header\n");
			ret = -EINVAL;
			break;
		}

		/* check for EOF marker */
		if (!file_sec->word1 && !file_sec->word2) {
			ret = 0;
			break;
		}

		if (mvm->trans->cfg->device_family != IWL_DEVICE_FAMILY_8000) {
			section_size =
				2 * NVM_WORD1_LEN(le16_to_cpu(file_sec->word1));
			section_id = NVM_WORD2_ID(le16_to_cpu(file_sec->word2));
		} else {
			section_size = 2 * NVM_WORD2_LEN_FAMILY_8000(
						le16_to_cpu(file_sec->word2));
			section_id = NVM_WORD1_ID_FAMILY_8000(
						le16_to_cpu(file_sec->word1));
		}

		if (section_size > max_section_size) {
			IWL_ERR(mvm, "ERROR - section too large (%d)\n",
				section_size);
			ret = -EINVAL;
			break;
		}

		if (!section_size) {
			IWL_ERR(mvm, "ERROR - section empty\n");
			ret = -EINVAL;
			break;
		}

		if (file_sec->data + section_size > eof) {
			IWL_ERR(mvm,
				"ERROR - NVM file too short for section (%d bytes)\n",
				section_size);
			ret = -EINVAL;
			break;
		}

		if (WARN(section_id >= NVM_MAX_NUM_SECTIONS,
			 "Invalid NVM section ID %d\n", section_id)) {
			ret = -EINVAL;
			break;
		}

		temp = kmemdup(file_sec->data, section_size, GFP_KERNEL);
		if (!temp) {
			ret = -ENOMEM;
			break;
		}
		mvm->nvm_sections[section_id].data = temp;
		mvm->nvm_sections[section_id].length = section_size;

		/* advance to the next section */
		file_sec = (void *)(file_sec->data + section_size);
	}
out:
	release_firmware(fw_entry);
	return ret;
}

/* Loads the NVM data stored in mvm->nvm_sections into the NIC */
int iwl_mvm_load_nvm_to_nic(struct iwl_mvm *mvm)
{
	int i, ret = 0;
	struct iwl_nvm_section *sections = mvm->nvm_sections;

	IWL_DEBUG_EEPROM(mvm->trans->dev, "'Write to NVM\n");

	for (i = 0; i < ARRAY_SIZE(mvm->nvm_sections); i++) {
		if (!mvm->nvm_sections[i].data || !mvm->nvm_sections[i].length)
			continue;
		ret = iwl_nvm_write_section(mvm, i, sections[i].data,
					    sections[i].length);
		if (ret < 0) {
			IWL_ERR(mvm, "iwl_mvm_send_cmd failed: %d\n", ret);
			break;
		}
	}
	return ret;
}

int iwl_nvm_init(struct iwl_mvm *mvm, bool read_nvm_from_nic)
{
	int ret, section;
	u32 size_read = 0;
	u8 *nvm_buffer, *temp;

	if (WARN_ON_ONCE(mvm->cfg->nvm_hw_section_num >= NVM_MAX_NUM_SECTIONS))
		return -EINVAL;

	/* load NVM values from nic */
	if (read_nvm_from_nic) {
		/* Read From FW NVM */
		IWL_DEBUG_EEPROM(mvm->trans->dev, "Read from NVM\n");

		nvm_buffer = kmalloc(mvm->cfg->base_params->eeprom_size,
				     GFP_KERNEL);
		if (!nvm_buffer)
			return -ENOMEM;
		for (section = 0; section < NVM_MAX_NUM_SECTIONS; section++) {
			/* we override the constness for initial read */
			ret = iwl_nvm_read_section(mvm, section, nvm_buffer,
						   size_read);
			if (ret < 0)
				continue;
			size_read += ret;
			temp = kmemdup(nvm_buffer, ret, GFP_KERNEL);
			if (!temp) {
				ret = -ENOMEM;
				break;
			}
			mvm->nvm_sections[section].data = temp;
			mvm->nvm_sections[section].length = ret;

#ifdef CPTCFG_IWLWIFI_DEBUGFS
			switch (section) {
			case NVM_SECTION_TYPE_SW:
				mvm->nvm_sw_blob.data = temp;
				mvm->nvm_sw_blob.size  = ret;
				break;
			case NVM_SECTION_TYPE_CALIBRATION:
				mvm->nvm_calib_blob.data = temp;
				mvm->nvm_calib_blob.size  = ret;
				break;
			case NVM_SECTION_TYPE_PRODUCTION:
				mvm->nvm_prod_blob.data = temp;
				mvm->nvm_prod_blob.size  = ret;
				break;
			default:
				if (section == mvm->cfg->nvm_hw_section_num) {
					mvm->nvm_hw_blob.data = temp;
					mvm->nvm_hw_blob.size = ret;
					break;
				}
			}
#endif
		}
		if (!size_read)
			IWL_ERR(mvm, "OTP is blank\n");
		kfree(nvm_buffer);
	}

	/* load external NVM if configured */
	if (mvm->nvm_file_name) {
		/* move to External NVM flow */
		ret = iwl_mvm_read_external_nvm(mvm);
		if (ret)
			return ret;
	}

	/* parse the relevant nvm sections */
	mvm->nvm_data = iwl_parse_nvm_sections(mvm);
	if (!mvm->nvm_data)
		return -ENODATA;

	return 0;
}

struct iwl_mcc_update_resp *
iwl_mvm_update_mcc(struct iwl_mvm *mvm, const char *alpha2)
{
	struct iwl_mcc_update_cmd mcc_update_cmd = {
		.mcc = cpu_to_le16(alpha2[0] << 8 | alpha2[1]),
	};
	struct iwl_mcc_update_resp *mcc_resp, *resp_cp = NULL;
	struct iwl_rx_packet *pkt;
	struct iwl_host_cmd cmd = {
		.id = MCC_UPDATE_CMD,
		.flags = CMD_WANT_SKB,
		.data = { &mcc_update_cmd },
	};

	int ret;
	u32 status;
	int resp_len, n_channels;
	u16 mcc;

	if (WARN_ON_ONCE(!iwl_mvm_is_lar_supported(mvm)))
		return ERR_PTR(-EOPNOTSUPP);

	cmd.len[0] = sizeof(struct iwl_mcc_update_cmd);

	IWL_DEBUG_LAR(mvm, "send MCC update to FW with '%c%c'\n",
		      alpha2[0], alpha2[1]);

	ret = iwl_mvm_send_cmd(mvm, &cmd);
	if (ret)
		return ERR_PTR(ret);

	pkt = cmd.resp_pkt;
	if (pkt->hdr.flags & IWL_CMD_FAILED_MSK) {
		IWL_ERR(mvm, "Bad return from MCC_UPDATE_COMMAND (0x%08X)\n",
			pkt->hdr.flags);
		ret = -EIO;
		goto exit;
	}

	/* Extract MCC response */
	mcc_resp = (void *)pkt->data;
	status = le32_to_cpu(mcc_resp->status);

	if (status == MCC_RESP_INVALID) {
		IWL_ERR(mvm,
			"FW ERROR: MCC update with invalid parameter '%c%c'\n",
			alpha2[0], alpha2[1]);
		ret = -EINVAL;
		goto exit;
	} else if (status == MCC_RESP_NVM_DISABLED) {
		ret = 0;
		/* resp_cp will be NULL */
		goto exit;
	}

	mcc = le16_to_cpu(mcc_resp->mcc);

	/* W/A for a FW/NVM issue - returns 0x00 for the world domain */
	if (mcc == 0) {
		mcc = 0x3030;  /* "00" - world */
		mcc_resp->mcc = cpu_to_le16(mcc);
	}

	n_channels =  __le32_to_cpu(mcc_resp->n_channels);
	IWL_DEBUG_LAR(mvm,
		"MCC response status: 0x%x. new MCC: 0x%x ('%c%c') change: %d n_chans: %d\n",
		 status, mcc, mcc >> 8, mcc & 0xff,
		 !!(status == MCC_RESP_SAME_CHAN_PROFILE), n_channels);

	resp_len = sizeof(*mcc_resp) + n_channels * sizeof(__le32);
	resp_cp = kmemdup(mcc_resp, resp_len, GFP_KERNEL);
	if (!resp_cp) {
		ret = -ENOMEM;
		goto exit;
	}

	ret = 0;
exit:
	iwl_free_resp(&cmd);
	if (ret)
		return ERR_PTR(ret);
	return resp_cp;
}

int iwl_mvm_init_mcc(struct iwl_mvm *mvm)
{
	bool tlv_lar;
	bool nvm_lar;

	if (mvm->cfg->device_family == IWL_DEVICE_FAMILY_8000) {
		tlv_lar = mvm->fw->ucode_capa.capa[0] &
			IWL_UCODE_TLV_CAPA_LAR_SUPPORT;
		nvm_lar = mvm->nvm_data->lar_enabled;
		if (tlv_lar != nvm_lar)
			IWL_INFO(mvm,
				 "Conflict between TLV & NVM regarding enabling LAR (TLV = %s NVM =%s)\n",
				 tlv_lar ? "enabled" : "disabled",
				 nvm_lar ? "enabled" : "disabled");
	}

	if (!iwl_mvm_is_lar_supported(mvm))
		return 0;

	/*
	 * During HW restart, only replay the last set MCC to FW. Otherwise,
	 * queue an update to cfg80211 to retrieve the default alpha2 from FW.
	 */
	if (test_bit(IWL_MVM_STATUS_IN_HW_RESTART, &mvm->status)) {
		/* This should only be called during vif up and hold RTNL */
		const struct ieee80211_regdomain *r =
				rtnl_dereference(mvm->hw->wiphy->regd);

		if (r) {
			struct iwl_mcc_update_resp *resp;

			resp = iwl_mvm_update_mcc(mvm, r->alpha2);
			if (IS_ERR_OR_NULL(resp))
				return -EIO;

			kfree(resp);
		}

		return 0;
	}

	/*
	 * Driver regulatory hint for initial update - use the special
	 * unknown-country "99" code. This will also clear the "custom reg"
	 * flag and allow regdomain changes. It will happen after init since
	 * RTNL is required.
	 * Disallow scans that might crash the FW while the LAR regdomain
	 * is not set.
	 */
	mvm->lar_regdom_set = false;
	return regulatory_hint(mvm->hw->wiphy, "99");
}

int iwl_mvm_rx_chub_update_mcc(struct iwl_mvm *mvm,
			       struct iwl_rx_cmd_buffer *rxb,
			       struct iwl_device_cmd *cmd)
{
	struct iwl_rx_packet *pkt = rxb_addr(rxb);
	struct iwl_mcc_chub_notif *notif = (void *)pkt->data;
	char mcc[3];

	if (WARN_ON_ONCE(!iwl_mvm_is_lar_supported(mvm)))
		return -EOPNOTSUPP;

	mcc[0] = notif->mcc >> 8;
	mcc[1] = notif->mcc & 0xff;
	mcc[2] = '\0';

	IWL_DEBUG_LAR(mvm,
		      "RX: received chub update mcc command (mcc 0x%x '%s')\n",
		      notif->mcc, mcc);

	return 0;
}
