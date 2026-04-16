/*
Vendor Reset - Vendor Specific Reset
Copyright (C) 2020 Geoffrey McRae <geoff@hostfission.com>

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free Software
Foundation; either version 2 of the License, or (at your option) any later
version.

This program is distributed in the hope that it will be useful, but WITHOUT ANY
WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A
PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with
this program; if not, write to the Free Software Foundation, Inc., 59 Temple
Place, Suite 330, Boston, MA 02111-1307 USA
*/

#include <linux/delay.h>

#include "vendor-reset-dev.h"

#include "amd.h"
#include "common_defs.h"
#include "common.h"
#include "firmware.h"
#include "amdgpu_discovery.h"
#include "smu_v11_0.h"
#include "mp/mp_11_0_offset.h"
#include "mp/mp_11_0_sh_mask.h"
#include "nbio_2_3_offset.h"
#include "nv.h"
#include "psp_gfx_if.h"
#include "smu_v11_0_ppsmc.h"

extern bool amdgpu_get_bios(struct amd_fake_dev *adev);

static int amd_cezanne_reset(struct vendor_reset_dev *dev)
{
  struct amd_vendor_private *priv = amd_private(dev);
  struct amd_fake_dev *adev;
  int ret = 0, timeout;
  u32 sol, smu_resp, mp1_intr, psp_bl_ready, tmp, offset;

  adev = &priv->adev;
  ret = amd_fake_dev_init(adev, dev);
  if (ret)
    return ret;

  ret = amdgpu_discovery_reg_base_init(adev);
  if (ret < 0)
  {
    vr_err(dev, "amdgpu_discovery_reg_base_init failed for APU: [%04x:%04x]\n", dev->pdev->vendor, dev->pdev->device);
    ret = -ENOTSUPP;
    goto free_adev;
  }

  if (!amdgpu_get_bios(adev))
  {
    vr_warn(dev, "amdgpu_get_bios failed, proceeding without AtomBIOS (common on APUs)\n");
  }
  else
  {
    ret = atom_bios_init(adev);
    if (ret)
      vr_warn(dev, "atom_bios_init failed: %d\n", ret);
  }

  /* it's important we wait for the SOC to be ready */
  for (timeout = 100000; timeout; --timeout)
  {
    sol = RREG32_SOC15(MP0, 0, mmMP0_SMN_C2PMSG_81);
    if (sol != 0xFFFFFFFF && sol != 0)
      break;
    udelay(1);
  }

  if (sol == ~1L)
  {
    vr_warn(dev, "Timed out waiting for SOL to be valid\n");
  }

  /* collect some info for logging for now */
  smu_resp = RREG32_SOC15(MP1, 0, mmMP1_SMN_C2PMSG_90);
  mp1_intr = (RREG32_PCIE(MP1_Public |
                          (smnMP1_FIRMWARE_FLAGS & 0xffffffff)) &
              MP1_FIRMWARE_FLAGS__INTERRUPTS_ENABLED_MASK) >>
             MP1_FIRMWARE_FLAGS__INTERRUPTS_ENABLED__SHIFT;
  psp_bl_ready = !!(RREG32_SOC15(MP0, 0, mmMP0_SMN_C2PMSG_35) & 0x80000000L);
  vr_info(dev, "SMU response reg: %x, sol reg: %x, mp1 intr enabled? %s, bl ready? %s\n",
          smu_resp, sol, mp1_intr ? "yes" : "no",
          psp_bl_ready ? "yes" : "no");

  /* okay, if we're in this state, we're probably reset */
  if (sol == 0x0 && !mp1_intr && psp_bl_ready)
    goto free_adev;

  /* 
   * Forcibly find and clear NBIO scratch registers.
   * On Cezanne/Vega 7, these registers (0-7) are crucial.
   * We use the SOC15 common offset for NBIF (NBIO).
   */
  vr_info(dev, "Clearing Vega 7 scratch registers via SOC15 NBIF\n");
  WREG32_SOC15(NBIF, 0, mmBIOS_SCRATCH_0, 0);
  WREG32_SOC15(NBIF, 0, mmBIOS_SCRATCH_1, 0);
  WREG32_SOC15(NBIF, 0, mmBIOS_SCRATCH_2, 0);
  WREG32_SOC15(NBIF, 0, mmBIOS_SCRATCH_3, 0);
  WREG32_SOC15(NBIF, 0, mmBIOS_SCRATCH_4, 0);
  WREG32_SOC15(NBIF, 0, mmBIOS_SCRATCH_5, 0);
  WREG32_SOC15(NBIF, 0, mmBIOS_SCRATCH_6, 0);
  WREG32_SOC15(NBIF, 0, mmBIOS_SCRATCH_7, 0);

  if (mp1_intr)
  {
    /* 
     * For Cezanne/Vega 7 (SMU v12), the message IDs are different from Navi.
     * 0x42 (66) is GfxDeviceDriverReset.
     * 0x10 (16) is DisallowGfxOff.
     */
    vr_info(dev, "Preparing SMU v12 for reset\n");
    smum_send_msg_to_smc(adev, 0x10, NULL); /* DisallowGfxOff */
    
    /* Try Cezanne specific reset message 0x42 */
    if (smum_send_msg_to_smc_with_parameter(adev, 0x42, 2, NULL) == 0x1) {
      vr_info(dev, "SMU v12 GfxDeviceDriverReset (0x42) accepted\n");
    } else {
      /* Fallback to 0x4 if 0x42 is rejected */
      smum_send_msg_to_smc_with_parameter(adev, 0x4, 2, NULL);
    }
    msleep(200);
  }

  vr_info(dev, "triggering PSP Mode1 Reset for Cezanne (Vega 7)\n");
  if (adev->bios_scratch_reg_offset)
    amdgpu_atombios_scratch_regs_engine_hung(adev, true);

  pci_save_state(dev->pdev);

  /* check validity of PSP before reset */
  offset = SOC15_REG_OFFSET(MP0, 0, mmMP0_SMN_C2PMSG_64);
  tmp = psp_wait_for(adev, offset, 0x80000000, 0x8000FFFF, false);
  if (tmp)
    vr_warn(dev, "timed out waiting for PSP to reach valid state, but continuing anyway\n");

  /* reset command */
  WREG32_SOC15(MP0, 0, mmMP0_SMN_C2PMSG_64, GFX_CTRL_CMD_ID_MODE1_RST);
  
  /* Critical: Cezanne needs significant time to recover display paths */
  msleep(1500);

  /* wait for ACK */
  offset = SOC15_REG_OFFSET(MP0, 0, mmMP0_SMN_C2PMSG_33);
  tmp = psp_wait_for(adev, offset, 0x80000000, 0x80000000, false);
  if (tmp)
  {
    vr_warn(dev, "PSP did not acknowledger reset\n");
    ret = -EINVAL;
    goto out;
  }

  vr_info(dev, "mode1 reset succeeded\n");

  pci_restore_state(dev->pdev);

  for (timeout = 100000; timeout; --timeout)
  {
    tmp = RREG32_SOC15(NBIO, 0, mmRCC_DEV0_EPF0_RCC_CONFIG_MEMSIZE);

    if (tmp != 0xffffffff)
      break;
    udelay(1);
  }

  /*
   * this takes a long time :(
   */
  for (timeout = 100; timeout; --timeout)
  {
    /* see if PSP bootloader comes back */
    if (RREG32_SOC15(MP0, 0, mmMP0_SMN_C2PMSG_35) & 0x80000000L)
      break;
    msleep(100);
  }

  if (!timeout && !(RREG32_SOC15(MP0, 0, mmMP0_SMN_C2PMSG_35) & 0x80000000L))
  {
    vr_warn(dev, "timed out waiting for PSP bootloader to respond after reset\n");
    ret = -ETIME;
  }
  else
    vr_info(dev, "PSP mode1 reset successful\n");

out:
  pci_restore_state(dev->pdev);
  amdgpu_atombios_scratch_regs_engine_hung(adev, false);

free_adev:
  amd_fake_dev_fini(adev);

  return ret;
}

const struct vendor_reset_ops amd_renoir_ops =
{
  .version = {1, 1},
  .probe = amd_common_probe,
  .pre_reset = amd_common_pre_reset,
  .reset = amd_cezanne_reset,
  .post_reset = amd_common_post_reset,
};

const struct vendor_reset_ops amd_cezanne_ops =
{
  .version = {1, 1},
  .probe = amd_common_probe,
  .pre_reset = amd_common_pre_reset,
  .reset = amd_cezanne_reset,
  .post_reset = amd_common_post_reset,
};
