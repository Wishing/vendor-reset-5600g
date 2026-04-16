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

  /* collect some info for logging */
  smu_resp = RREG32_SOC15(MP1, 0, mmMP1_SMN_C2PMSG_90);
  mp1_intr = (RREG32_PCIE(MP1_Public |
                          (smnMP1_FIRMWARE_FLAGS & 0xffffffff)) &
              MP1_FIRMWARE_FLAGS__INTERRUPTS_ENABLED_MASK) >>
             MP1_FIRMWARE_FLAGS__INTERRUPTS_ENABLED__SHIFT;
  psp_bl_ready = !!(RREG32_SOC15(MP0, 0, mmMP0_SMN_C2PMSG_35) & 0x80000000L);
  vr_info(dev, "SMU resp: %x, sol: %x, mp1: %s, bl: %s\n",
          smu_resp, sol, mp1_intr ? "on" : "off",
          psp_bl_ready ? "ready" : "not ready");

  /* Forcefully clear all scratch registers via SOC15 macros */
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
    /* Cezanne Final Attempt: Mode2 + Mode1 */
    vr_info(dev, "Attempting SMU Mode2 Reset (0x11)\n");
    smum_send_msg_to_smc(adev, 0x11, NULL);
    msleep(1000);
  }

  vr_info(dev, "Triggering PSP Mode1 Reset\n");
  pci_save_state(dev->pdev);
  WREG32_SOC15(MP0, 0, mmMP0_SMN_C2PMSG_64, GFX_CTRL_CMD_ID_MODE1_RST);
  msleep(3000);
  pci_restore_state(dev->pdev);

  /* Force PCIe Command register back to active state */
  pci_write_config_word(dev->pdev, PCI_COMMAND, PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER | PCI_COMMAND_IO);

  vr_info(dev, "Hardware reset cycle complete, forcing D0 state\n");
  
  /* Prevent D3hot in post_reset */
  dev->reset_ret = 1; 

free_adev:
  amd_fake_dev_fini(adev);
  return 0;
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
