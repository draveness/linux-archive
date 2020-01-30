/*******************************************************************************

  
  Copyright(c) 1999 - 2004 Intel Corporation. All rights reserved.
  
  This program is free software; you can redistribute it and/or modify it 
  under the terms of the GNU General Public License as published by the Free 
  Software Foundation; either version 2 of the License, or (at your option) 
  any later version.
  
  This program is distributed in the hope that it will be useful, but WITHOUT 
  ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or 
  FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for 
  more details.
  
  You should have received a copy of the GNU General Public License along with
  this program; if not, write to the Free Software Foundation, Inc., 59 
  Temple Place - Suite 330, Boston, MA  02111-1307, USA.
  
  The full GNU General Public License is included in this distribution in the
  file called LICENSE.
  
  Contact Information:
  Linux NICS <linux.nics@intel.com>
  Intel Corporation, 5200 N.E. Elam Young Parkway, Hillsboro, OR 97124-6497

*******************************************************************************/

/* e1000_hw.c
 * Shared functions for accessing and configuring the MAC
 */

#include "e1000_hw.h"

static int32_t e1000_set_phy_type(struct e1000_hw *hw);
static void e1000_phy_init_script(struct e1000_hw *hw);
static int32_t e1000_setup_copper_link(struct e1000_hw *hw);
static int32_t e1000_setup_fiber_serdes_link(struct e1000_hw *hw);
static int32_t e1000_adjust_serdes_amplitude(struct e1000_hw *hw);
static int32_t e1000_phy_force_speed_duplex(struct e1000_hw *hw);
static int32_t e1000_config_mac_to_phy(struct e1000_hw *hw);
static void e1000_raise_mdi_clk(struct e1000_hw *hw, uint32_t *ctrl);
static void e1000_lower_mdi_clk(struct e1000_hw *hw, uint32_t *ctrl);
static void e1000_shift_out_mdi_bits(struct e1000_hw *hw, uint32_t data,
                                     uint16_t count);
static uint16_t e1000_shift_in_mdi_bits(struct e1000_hw *hw);
static int32_t e1000_phy_reset_dsp(struct e1000_hw *hw);
static int32_t e1000_write_eeprom_spi(struct e1000_hw *hw, uint16_t offset,
                                      uint16_t words, uint16_t *data);
static int32_t e1000_write_eeprom_microwire(struct e1000_hw *hw,
                                            uint16_t offset, uint16_t words,
                                            uint16_t *data);
static int32_t e1000_spi_eeprom_ready(struct e1000_hw *hw);
static void e1000_raise_ee_clk(struct e1000_hw *hw, uint32_t *eecd);
static void e1000_lower_ee_clk(struct e1000_hw *hw, uint32_t *eecd);
static void e1000_shift_out_ee_bits(struct e1000_hw *hw, uint16_t data,
                                    uint16_t count);
static int32_t e1000_write_phy_reg_ex(struct e1000_hw *hw, uint32_t reg_addr,
                                      uint16_t phy_data);
static int32_t e1000_read_phy_reg_ex(struct e1000_hw *hw,uint32_t reg_addr,
                                     uint16_t *phy_data);
static uint16_t e1000_shift_in_ee_bits(struct e1000_hw *hw, uint16_t count);
static int32_t e1000_acquire_eeprom(struct e1000_hw *hw);
static void e1000_release_eeprom(struct e1000_hw *hw);
static void e1000_standby_eeprom(struct e1000_hw *hw);
static int32_t e1000_id_led_init(struct e1000_hw * hw);
static int32_t e1000_set_vco_speed(struct e1000_hw *hw);
static int32_t e1000_set_phy_mode(struct e1000_hw *hw);

/* IGP cable length table */
static const
uint16_t e1000_igp_cable_length_table[IGP01E1000_AGC_LENGTH_TABLE_SIZE] =
    { 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5,
      5, 10, 10, 10, 10, 10, 10, 10, 20, 20, 20, 20, 20, 25, 25, 25,
      25, 25, 25, 25, 30, 30, 30, 30, 40, 40, 40, 40, 40, 40, 40, 40,
      40, 50, 50, 50, 50, 50, 50, 50, 60, 60, 60, 60, 60, 60, 60, 60,
      60, 70, 70, 70, 70, 70, 70, 80, 80, 80, 80, 80, 80, 90, 90, 90,
      90, 90, 90, 90, 90, 90, 100, 100, 100, 100, 100, 100, 100, 100, 100, 100,
      100, 100, 100, 100, 110, 110, 110, 110, 110, 110, 110, 110, 110, 110, 110, 110,
      110, 110, 110, 110, 110, 110, 120, 120, 120, 120, 120, 120, 120, 120, 120, 120};


/******************************************************************************
 * Set the phy type member in the hw struct.
 *
 * hw - Struct containing variables accessed by shared code
 *****************************************************************************/
int32_t
e1000_set_phy_type(struct e1000_hw *hw)
{
    DEBUGFUNC("e1000_set_phy_type");

    switch(hw->phy_id) {
    case M88E1000_E_PHY_ID:
    case M88E1000_I_PHY_ID:
    case M88E1011_I_PHY_ID:
        hw->phy_type = e1000_phy_m88;
        break;
    case IGP01E1000_I_PHY_ID:
        if(hw->mac_type == e1000_82541 ||
           hw->mac_type == e1000_82541_rev_2 ||
           hw->mac_type == e1000_82547 ||
           hw->mac_type == e1000_82547_rev_2) {
            hw->phy_type = e1000_phy_igp;
            break;
        }
        /* Fall Through */
    default:
        /* Should never have loaded on this device */
        hw->phy_type = e1000_phy_undefined;
        return -E1000_ERR_PHY_TYPE;
    }

    return E1000_SUCCESS;
}

/******************************************************************************
 * IGP phy init script - initializes the GbE PHY
 *
 * hw - Struct containing variables accessed by shared code
 *****************************************************************************/
static void
e1000_phy_init_script(struct e1000_hw *hw)
{
    DEBUGFUNC("e1000_phy_init_script");

    if(hw->phy_init_script) {
        msec_delay(20);

        e1000_write_phy_reg(hw,0x0000,0x0140);

        msec_delay(5);

        if(hw->mac_type == e1000_82541 || hw->mac_type == e1000_82547) {
            e1000_write_phy_reg(hw, 0x1F95, 0x0001);

            e1000_write_phy_reg(hw, 0x1F71, 0xBD21);

            e1000_write_phy_reg(hw, 0x1F79, 0x0018);

            e1000_write_phy_reg(hw, 0x1F30, 0x1600);

            e1000_write_phy_reg(hw, 0x1F31, 0x0014);

            e1000_write_phy_reg(hw, 0x1F32, 0x161C);

            e1000_write_phy_reg(hw, 0x1F94, 0x0003);

            e1000_write_phy_reg(hw, 0x1F96, 0x003F);

            e1000_write_phy_reg(hw, 0x2010, 0x0008);
        } else {
            e1000_write_phy_reg(hw, 0x1F73, 0x0099);
        }

        e1000_write_phy_reg(hw, 0x0000, 0x3300);

        if(hw->mac_type == e1000_82547) {
            uint16_t fused, fine, coarse;

            /* Move to analog registers page */
            e1000_read_phy_reg(hw, IGP01E1000_ANALOG_SPARE_FUSE_STATUS, &fused);

            if(!(fused & IGP01E1000_ANALOG_SPARE_FUSE_ENABLED)) {
                e1000_read_phy_reg(hw, IGP01E1000_ANALOG_FUSE_STATUS, &fused);

                fine = fused & IGP01E1000_ANALOG_FUSE_FINE_MASK;
                coarse = fused & IGP01E1000_ANALOG_FUSE_COARSE_MASK;

                if(coarse > IGP01E1000_ANALOG_FUSE_COARSE_THRESH) {
                    coarse -= IGP01E1000_ANALOG_FUSE_COARSE_10;
                    fine -= IGP01E1000_ANALOG_FUSE_FINE_1;
                } else if(coarse == IGP01E1000_ANALOG_FUSE_COARSE_THRESH)
                    fine -= IGP01E1000_ANALOG_FUSE_FINE_10;

                fused = (fused & IGP01E1000_ANALOG_FUSE_POLY_MASK) |
                        (fine & IGP01E1000_ANALOG_FUSE_FINE_MASK) |
                        (coarse & IGP01E1000_ANALOG_FUSE_COARSE_MASK);

                e1000_write_phy_reg(hw, IGP01E1000_ANALOG_FUSE_CONTROL, fused);
                e1000_write_phy_reg(hw, IGP01E1000_ANALOG_FUSE_BYPASS,
                                    IGP01E1000_ANALOG_FUSE_ENABLE_SW_CONTROL);
            }
        }
    }
}

/******************************************************************************
 * Set the mac type member in the hw struct.
 *
 * hw - Struct containing variables accessed by shared code
 *****************************************************************************/
int32_t
e1000_set_mac_type(struct e1000_hw *hw)
{
    DEBUGFUNC("e1000_set_mac_type");

    switch (hw->device_id) {
    case E1000_DEV_ID_82542:
        switch (hw->revision_id) {
        case E1000_82542_2_0_REV_ID:
            hw->mac_type = e1000_82542_rev2_0;
            break;
        case E1000_82542_2_1_REV_ID:
            hw->mac_type = e1000_82542_rev2_1;
            break;
        default:
            /* Invalid 82542 revision ID */
            return -E1000_ERR_MAC_TYPE;
        }
        break;
    case E1000_DEV_ID_82543GC_FIBER:
    case E1000_DEV_ID_82543GC_COPPER:
        hw->mac_type = e1000_82543;
        break;
    case E1000_DEV_ID_82544EI_COPPER:
    case E1000_DEV_ID_82544EI_FIBER:
    case E1000_DEV_ID_82544GC_COPPER:
    case E1000_DEV_ID_82544GC_LOM:
        hw->mac_type = e1000_82544;
        break;
    case E1000_DEV_ID_82540EM:
    case E1000_DEV_ID_82540EM_LOM:
    case E1000_DEV_ID_82540EP:
    case E1000_DEV_ID_82540EP_LOM:
    case E1000_DEV_ID_82540EP_LP:
        hw->mac_type = e1000_82540;
        break;
    case E1000_DEV_ID_82545EM_COPPER:
    case E1000_DEV_ID_82545EM_FIBER:
        hw->mac_type = e1000_82545;
        break;
    case E1000_DEV_ID_82545GM_COPPER:
    case E1000_DEV_ID_82545GM_FIBER:
    case E1000_DEV_ID_82545GM_SERDES:
        hw->mac_type = e1000_82545_rev_3;
        break;
    case E1000_DEV_ID_82546EB_COPPER:
    case E1000_DEV_ID_82546EB_FIBER:
    case E1000_DEV_ID_82546EB_QUAD_COPPER:
        hw->mac_type = e1000_82546;
        break;
    case E1000_DEV_ID_82546GB_COPPER:
    case E1000_DEV_ID_82546GB_FIBER:
    case E1000_DEV_ID_82546GB_SERDES:
        hw->mac_type = e1000_82546_rev_3;
        break;
    case E1000_DEV_ID_82541EI:
    case E1000_DEV_ID_82541EI_MOBILE:
        hw->mac_type = e1000_82541;
        break;
    case E1000_DEV_ID_82541ER:
    case E1000_DEV_ID_82541GI:
    case E1000_DEV_ID_82541GI_MOBILE:
        hw->mac_type = e1000_82541_rev_2;
        break;
    case E1000_DEV_ID_82547EI:
        hw->mac_type = e1000_82547;
        break;
    case E1000_DEV_ID_82547GI:
        hw->mac_type = e1000_82547_rev_2;
        break;
    default:
        /* Should never have loaded on this device */
        return -E1000_ERR_MAC_TYPE;
    }

    switch(hw->mac_type) {
    case e1000_82541:
    case e1000_82547:
    case e1000_82541_rev_2:
    case e1000_82547_rev_2:
        hw->asf_firmware_present = TRUE;
        break;
    default:
        break;
    }

    return E1000_SUCCESS;
}

/*****************************************************************************
 * Set media type and TBI compatibility.
 *
 * hw - Struct containing variables accessed by shared code
 * **************************************************************************/
void
e1000_set_media_type(struct e1000_hw *hw)
{
    uint32_t status;

    DEBUGFUNC("e1000_set_media_type");

    if(hw->mac_type != e1000_82543) {
        /* tbi_compatibility is only valid on 82543 */
        hw->tbi_compatibility_en = FALSE;
    }

    switch (hw->device_id) {
    case E1000_DEV_ID_82545GM_SERDES:
    case E1000_DEV_ID_82546GB_SERDES:
        hw->media_type = e1000_media_type_internal_serdes;
        break;
    default:
        if(hw->mac_type >= e1000_82543) {
            status = E1000_READ_REG(hw, STATUS);
            if(status & E1000_STATUS_TBIMODE) {
                hw->media_type = e1000_media_type_fiber;
                /* tbi_compatibility not valid on fiber */
                hw->tbi_compatibility_en = FALSE;
            } else {
                hw->media_type = e1000_media_type_copper;
            }
        } else {
            /* This is an 82542 (fiber only) */
            hw->media_type = e1000_media_type_fiber;
        }
    }
}

/******************************************************************************
 * Reset the transmit and receive units; mask and clear all interrupts.
 *
 * hw - Struct containing variables accessed by shared code
 *****************************************************************************/
int32_t
e1000_reset_hw(struct e1000_hw *hw)
{
    uint32_t ctrl;
    uint32_t ctrl_ext;
    uint32_t icr;
    uint32_t manc;
    uint32_t led_ctrl;

    DEBUGFUNC("e1000_reset_hw");

    /* For 82542 (rev 2.0), disable MWI before issuing a device reset */
    if(hw->mac_type == e1000_82542_rev2_0) {
        DEBUGOUT("Disabling MWI on 82542 rev 2.0\n");
        e1000_pci_clear_mwi(hw);
    }

    /* Clear interrupt mask to stop board from generating interrupts */
    DEBUGOUT("Masking off all interrupts\n");
    E1000_WRITE_REG(hw, IMC, 0xffffffff);

    /* Disable the Transmit and Receive units.  Then delay to allow
     * any pending transactions to complete before we hit the MAC with
     * the global reset.
     */
    E1000_WRITE_REG(hw, RCTL, 0);
    E1000_WRITE_REG(hw, TCTL, E1000_TCTL_PSP);
    E1000_WRITE_FLUSH(hw);

    /* The tbi_compatibility_on Flag must be cleared when Rctl is cleared. */
    hw->tbi_compatibility_on = FALSE;

    /* Delay to allow any outstanding PCI transactions to complete before
     * resetting the device
     */
    msec_delay(10);

    ctrl = E1000_READ_REG(hw, CTRL);

    /* Must reset the PHY before resetting the MAC */
    if((hw->mac_type == e1000_82541) || (hw->mac_type == e1000_82547)) {
        E1000_WRITE_REG_IO(hw, CTRL, (ctrl | E1000_CTRL_PHY_RST));
        msec_delay(5);
    }

    /* Issue a global reset to the MAC.  This will reset the chip's
     * transmit, receive, DMA, and link units.  It will not effect
     * the current PCI configuration.  The global reset bit is self-
     * clearing, and should clear within a microsecond.
     */
    DEBUGOUT("Issuing a global reset to MAC\n");

    switch(hw->mac_type) {
        case e1000_82544:
        case e1000_82540:
        case e1000_82545:
        case e1000_82546:
        case e1000_82541:
        case e1000_82541_rev_2:
            /* These controllers can't ack the 64-bit write when issuing the
             * reset, so use IO-mapping as a workaround to issue the reset */
            E1000_WRITE_REG_IO(hw, CTRL, (ctrl | E1000_CTRL_RST));
            break;
        case e1000_82545_rev_3:
        case e1000_82546_rev_3:
            /* Reset is performed on a shadow of the control register */
            E1000_WRITE_REG(hw, CTRL_DUP, (ctrl | E1000_CTRL_RST));
            break;
        default:
            E1000_WRITE_REG(hw, CTRL, (ctrl | E1000_CTRL_RST));
            break;
    }

    /* After MAC reset, force reload of EEPROM to restore power-on settings to
     * device.  Later controllers reload the EEPROM automatically, so just wait
     * for reload to complete.
     */
    switch(hw->mac_type) {
        case e1000_82542_rev2_0:
        case e1000_82542_rev2_1:
        case e1000_82543:
        case e1000_82544:
            /* Wait for reset to complete */
            udelay(10);
            ctrl_ext = E1000_READ_REG(hw, CTRL_EXT);
            ctrl_ext |= E1000_CTRL_EXT_EE_RST;
            E1000_WRITE_REG(hw, CTRL_EXT, ctrl_ext);
            E1000_WRITE_FLUSH(hw);
            /* Wait for EEPROM reload */
            msec_delay(2);
            break;
        case e1000_82541:
        case e1000_82541_rev_2:
        case e1000_82547:
        case e1000_82547_rev_2:
            /* Wait for EEPROM reload */
            msec_delay(20);
            break;
        default:
            /* Wait for EEPROM reload (it happens automatically) */
            msec_delay(5);
            break;
    }

    /* Disable HW ARPs on ASF enabled adapters */
    if(hw->mac_type >= e1000_82540) {
        manc = E1000_READ_REG(hw, MANC);
        manc &= ~(E1000_MANC_ARP_EN);
        E1000_WRITE_REG(hw, MANC, manc);
    }

    if((hw->mac_type == e1000_82541) || (hw->mac_type == e1000_82547)) {
        e1000_phy_init_script(hw);

        /* Configure activity LED after PHY reset */
        led_ctrl = E1000_READ_REG(hw, LEDCTL);
        led_ctrl &= IGP_ACTIVITY_LED_MASK;
        led_ctrl |= (IGP_ACTIVITY_LED_ENABLE | IGP_LED3_MODE);
        E1000_WRITE_REG(hw, LEDCTL, led_ctrl);
    }

    /* Clear interrupt mask to stop board from generating interrupts */
    DEBUGOUT("Masking off all interrupts\n");
    E1000_WRITE_REG(hw, IMC, 0xffffffff);

    /* Clear any pending interrupt events. */
    icr = E1000_READ_REG(hw, ICR);

    /* If MWI was previously enabled, reenable it. */
    if(hw->mac_type == e1000_82542_rev2_0) {
        if(hw->pci_cmd_word & CMD_MEM_WRT_INVALIDATE)
            e1000_pci_set_mwi(hw);
    }

    return E1000_SUCCESS;
}

/******************************************************************************
 * Performs basic configuration of the adapter.
 *
 * hw - Struct containing variables accessed by shared code
 *
 * Assumes that the controller has previously been reset and is in a
 * post-reset uninitialized state. Initializes the receive address registers,
 * multicast table, and VLAN filter table. Calls routines to setup link
 * configuration and flow control settings. Clears all on-chip counters. Leaves
 * the transmit and receive units disabled and uninitialized.
 *****************************************************************************/
int32_t
e1000_init_hw(struct e1000_hw *hw)
{
    uint32_t ctrl;
    uint32_t i;
    int32_t ret_val;
    uint16_t pcix_cmd_word;
    uint16_t pcix_stat_hi_word;
    uint16_t cmd_mmrbc;
    uint16_t stat_mmrbc;
    DEBUGFUNC("e1000_init_hw");

    /* Initialize Identification LED */
    ret_val = e1000_id_led_init(hw);
    if(ret_val) {
        DEBUGOUT("Error Initializing Identification LED\n");
        return ret_val;
    }

    /* Set the media type and TBI compatibility */
    e1000_set_media_type(hw);

    /* Disabling VLAN filtering. */
    DEBUGOUT("Initializing the IEEE VLAN\n");
    E1000_WRITE_REG(hw, VET, 0);

    e1000_clear_vfta(hw);

    /* For 82542 (rev 2.0), disable MWI and put the receiver into reset */
    if(hw->mac_type == e1000_82542_rev2_0) {
        DEBUGOUT("Disabling MWI on 82542 rev 2.0\n");
        e1000_pci_clear_mwi(hw);
        E1000_WRITE_REG(hw, RCTL, E1000_RCTL_RST);
        E1000_WRITE_FLUSH(hw);
        msec_delay(5);
    }

    /* Setup the receive address. This involves initializing all of the Receive
     * Address Registers (RARs 0 - 15).
     */
    e1000_init_rx_addrs(hw);

    /* For 82542 (rev 2.0), take the receiver out of reset and enable MWI */
    if(hw->mac_type == e1000_82542_rev2_0) {
        E1000_WRITE_REG(hw, RCTL, 0);
        E1000_WRITE_FLUSH(hw);
        msec_delay(1);
        if(hw->pci_cmd_word & CMD_MEM_WRT_INVALIDATE)
            e1000_pci_set_mwi(hw);
    }

    /* Zero out the Multicast HASH table */
    DEBUGOUT("Zeroing the MTA\n");
    for(i = 0; i < E1000_MC_TBL_SIZE; i++)
        E1000_WRITE_REG_ARRAY(hw, MTA, i, 0);

    /* Set the PCI priority bit correctly in the CTRL register.  This
     * determines if the adapter gives priority to receives, or if it
     * gives equal priority to transmits and receives.
     */
    if(hw->dma_fairness) {
        ctrl = E1000_READ_REG(hw, CTRL);
        E1000_WRITE_REG(hw, CTRL, ctrl | E1000_CTRL_PRIOR);
    }

    switch(hw->mac_type) {
    case e1000_82545_rev_3:
    case e1000_82546_rev_3:
        break;
    default:
        /* Workaround for PCI-X problem when BIOS sets MMRBC incorrectly. */
        if(hw->bus_type == e1000_bus_type_pcix) {
            e1000_read_pci_cfg(hw, PCIX_COMMAND_REGISTER, &pcix_cmd_word);
            e1000_read_pci_cfg(hw, PCIX_STATUS_REGISTER_HI,
                &pcix_stat_hi_word);
            cmd_mmrbc = (pcix_cmd_word & PCIX_COMMAND_MMRBC_MASK) >>
                PCIX_COMMAND_MMRBC_SHIFT;
            stat_mmrbc = (pcix_stat_hi_word & PCIX_STATUS_HI_MMRBC_MASK) >>
                PCIX_STATUS_HI_MMRBC_SHIFT;
            if(stat_mmrbc == PCIX_STATUS_HI_MMRBC_4K)
                stat_mmrbc = PCIX_STATUS_HI_MMRBC_2K;
            if(cmd_mmrbc > stat_mmrbc) {
                pcix_cmd_word &= ~PCIX_COMMAND_MMRBC_MASK;
                pcix_cmd_word |= stat_mmrbc << PCIX_COMMAND_MMRBC_SHIFT;
                e1000_write_pci_cfg(hw, PCIX_COMMAND_REGISTER,
                    &pcix_cmd_word);
            }
        }
        break;
    }

    /* Call a subroutine to configure the link and setup flow control. */
    ret_val = e1000_setup_link(hw);

    /* Set the transmit descriptor write-back policy */
    if(hw->mac_type > e1000_82544) {
        ctrl = E1000_READ_REG(hw, TXDCTL);
        ctrl = (ctrl & ~E1000_TXDCTL_WTHRESH) | E1000_TXDCTL_FULL_TX_DESC_WB;
        E1000_WRITE_REG(hw, TXDCTL, ctrl);
    }

    /* Clear all of the statistics registers (clear on read).  It is
     * important that we do this after we have tried to establish link
     * because the symbol error count will increment wildly if there
     * is no link.
     */
    e1000_clear_hw_cntrs(hw);

    return ret_val;
}

/******************************************************************************
 * Adjust SERDES output amplitude based on EEPROM setting.
 *
 * hw - Struct containing variables accessed by shared code.
 *****************************************************************************/
static int32_t
e1000_adjust_serdes_amplitude(struct e1000_hw *hw)
{
    uint16_t eeprom_data;
    int32_t  ret_val;

    DEBUGFUNC("e1000_adjust_serdes_amplitude");

    if(hw->media_type != e1000_media_type_internal_serdes)
        return E1000_SUCCESS;

    switch(hw->mac_type) {
    case e1000_82545_rev_3:
    case e1000_82546_rev_3:
        break;
    default:
        return E1000_SUCCESS;
    }

    ret_val = e1000_read_eeprom(hw, EEPROM_SERDES_AMPLITUDE, 1, &eeprom_data);
    if (ret_val) {
        return ret_val;
    }

    if(eeprom_data != EEPROM_RESERVED_WORD) {
        /* Adjust SERDES output amplitude only. */
        eeprom_data &= EEPROM_SERDES_AMPLITUDE_MASK; 
        ret_val = e1000_write_phy_reg(hw, M88E1000_PHY_EXT_CTRL, eeprom_data);
        if(ret_val)
            return ret_val;
    }

    return E1000_SUCCESS;
}

/******************************************************************************
 * Configures flow control and link settings.
 *
 * hw - Struct containing variables accessed by shared code
 *
 * Determines which flow control settings to use. Calls the apropriate media-
 * specific link configuration function. Configures the flow control settings.
 * Assuming the adapter has a valid link partner, a valid link should be
 * established. Assumes the hardware has previously been reset and the
 * transmitter and receiver are not enabled.
 *****************************************************************************/
int32_t
e1000_setup_link(struct e1000_hw *hw)
{
    uint32_t ctrl_ext;
    int32_t ret_val;
    uint16_t eeprom_data;

    DEBUGFUNC("e1000_setup_link");

    /* Read and store word 0x0F of the EEPROM. This word contains bits
     * that determine the hardware's default PAUSE (flow control) mode,
     * a bit that determines whether the HW defaults to enabling or
     * disabling auto-negotiation, and the direction of the
     * SW defined pins. If there is no SW over-ride of the flow
     * control setting, then the variable hw->fc will
     * be initialized based on a value in the EEPROM.
     */
    if(e1000_read_eeprom(hw, EEPROM_INIT_CONTROL2_REG, 1, &eeprom_data) < 0) {
        DEBUGOUT("EEPROM Read Error\n");
        return -E1000_ERR_EEPROM;
    }

    if(hw->fc == e1000_fc_default) {
        if((eeprom_data & EEPROM_WORD0F_PAUSE_MASK) == 0)
            hw->fc = e1000_fc_none;
        else if((eeprom_data & EEPROM_WORD0F_PAUSE_MASK) ==
                EEPROM_WORD0F_ASM_DIR)
            hw->fc = e1000_fc_tx_pause;
        else
            hw->fc = e1000_fc_full;
    }

    /* We want to save off the original Flow Control configuration just
     * in case we get disconnected and then reconnected into a different
     * hub or switch with different Flow Control capabilities.
     */
    if(hw->mac_type == e1000_82542_rev2_0)
        hw->fc &= (~e1000_fc_tx_pause);

    if((hw->mac_type < e1000_82543) && (hw->report_tx_early == 1))
        hw->fc &= (~e1000_fc_rx_pause);

    hw->original_fc = hw->fc;

    DEBUGOUT1("After fix-ups FlowControl is now = %x\n", hw->fc);

    /* Take the 4 bits from EEPROM word 0x0F that determine the initial
     * polarity value for the SW controlled pins, and setup the
     * Extended Device Control reg with that info.
     * This is needed because one of the SW controlled pins is used for
     * signal detection.  So this should be done before e1000_setup_pcs_link()
     * or e1000_phy_setup() is called.
     */
    if(hw->mac_type == e1000_82543) {
        ctrl_ext = ((eeprom_data & EEPROM_WORD0F_SWPDIO_EXT) <<
                    SWDPIO__EXT_SHIFT);
        E1000_WRITE_REG(hw, CTRL_EXT, ctrl_ext);
    }

    /* Call the necessary subroutine to configure the link. */
    ret_val = (hw->media_type == e1000_media_type_copper) ?
              e1000_setup_copper_link(hw) :
              e1000_setup_fiber_serdes_link(hw);

    /* Initialize the flow control address, type, and PAUSE timer
     * registers to their default values.  This is done even if flow
     * control is disabled, because it does not hurt anything to
     * initialize these registers.
     */
    DEBUGOUT("Initializing the Flow Control address, type and timer regs\n");

    E1000_WRITE_REG(hw, FCAL, FLOW_CONTROL_ADDRESS_LOW);
    E1000_WRITE_REG(hw, FCAH, FLOW_CONTROL_ADDRESS_HIGH);
    E1000_WRITE_REG(hw, FCT, FLOW_CONTROL_TYPE);
    E1000_WRITE_REG(hw, FCTTV, hw->fc_pause_time);

    /* Set the flow control receive threshold registers.  Normally,
     * these registers will be set to a default threshold that may be
     * adjusted later by the driver's runtime code.  However, if the
     * ability to transmit pause frames in not enabled, then these
     * registers will be set to 0.
     */
    if(!(hw->fc & e1000_fc_tx_pause)) {
        E1000_WRITE_REG(hw, FCRTL, 0);
        E1000_WRITE_REG(hw, FCRTH, 0);
    } else {
        /* We need to set up the Receive Threshold high and low water marks
         * as well as (optionally) enabling the transmission of XON frames.
         */
        if(hw->fc_send_xon) {
            E1000_WRITE_REG(hw, FCRTL, (hw->fc_low_water | E1000_FCRTL_XONE));
            E1000_WRITE_REG(hw, FCRTH, hw->fc_high_water);
        } else {
            E1000_WRITE_REG(hw, FCRTL, hw->fc_low_water);
            E1000_WRITE_REG(hw, FCRTH, hw->fc_high_water);
        }
    }
    return ret_val;
}

/******************************************************************************
 * Sets up link for a fiber based or serdes based adapter
 *
 * hw - Struct containing variables accessed by shared code
 *
 * Manipulates Physical Coding Sublayer functions in order to configure
 * link. Assumes the hardware has been previously reset and the transmitter
 * and receiver are not enabled.
 *****************************************************************************/
static int32_t
e1000_setup_fiber_serdes_link(struct e1000_hw *hw)
{
    uint32_t ctrl;
    uint32_t status;
    uint32_t txcw = 0;
    uint32_t i;
    uint32_t signal = 0;
    int32_t ret_val;

    DEBUGFUNC("e1000_setup_fiber_serdes_link");

    /* On adapters with a MAC newer than 82544, SW Defineable pin 1 will be
     * set when the optics detect a signal. On older adapters, it will be
     * cleared when there is a signal.  This applies to fiber media only.
     * If we're on serdes media, adjust the output amplitude to value set in
     * the EEPROM.
     */
    ctrl = E1000_READ_REG(hw, CTRL);
    if(hw->media_type == e1000_media_type_fiber)
        signal = (hw->mac_type > e1000_82544) ? E1000_CTRL_SWDPIN1 : 0;

    ret_val = e1000_adjust_serdes_amplitude(hw);
    if(ret_val)
        return ret_val;

    /* Take the link out of reset */
    ctrl &= ~(E1000_CTRL_LRST);

    /* Adjust VCO speed to improve BER performance */
    ret_val = e1000_set_vco_speed(hw);
    if(ret_val)
        return ret_val;

    e1000_config_collision_dist(hw);

    /* Check for a software override of the flow control settings, and setup
     * the device accordingly.  If auto-negotiation is enabled, then software
     * will have to set the "PAUSE" bits to the correct value in the Tranmsit
     * Config Word Register (TXCW) and re-start auto-negotiation.  However, if
     * auto-negotiation is disabled, then software will have to manually
     * configure the two flow control enable bits in the CTRL register.
     *
     * The possible values of the "fc" parameter are:
     *      0:  Flow control is completely disabled
     *      1:  Rx flow control is enabled (we can receive pause frames, but
     *          not send pause frames).
     *      2:  Tx flow control is enabled (we can send pause frames but we do
     *          not support receiving pause frames).
     *      3:  Both Rx and TX flow control (symmetric) are enabled.
     */
    switch (hw->fc) {
    case e1000_fc_none:
        /* Flow control is completely disabled by a software over-ride. */
        txcw = (E1000_TXCW_ANE | E1000_TXCW_FD);
        break;
    case e1000_fc_rx_pause:
        /* RX Flow control is enabled and TX Flow control is disabled by a
         * software over-ride. Since there really isn't a way to advertise
         * that we are capable of RX Pause ONLY, we will advertise that we
         * support both symmetric and asymmetric RX PAUSE. Later, we will
         *  disable the adapter's ability to send PAUSE frames.
         */
        txcw = (E1000_TXCW_ANE | E1000_TXCW_FD | E1000_TXCW_PAUSE_MASK);
        break;
    case e1000_fc_tx_pause:
        /* TX Flow control is enabled, and RX Flow control is disabled, by a
         * software over-ride.
         */
        txcw = (E1000_TXCW_ANE | E1000_TXCW_FD | E1000_TXCW_ASM_DIR);
        break;
    case e1000_fc_full:
        /* Flow control (both RX and TX) is enabled by a software over-ride. */
        txcw = (E1000_TXCW_ANE | E1000_TXCW_FD | E1000_TXCW_PAUSE_MASK);
        break;
    default:
        DEBUGOUT("Flow control param set incorrectly\n");
        return -E1000_ERR_CONFIG;
        break;
    }

    /* Since auto-negotiation is enabled, take the link out of reset (the link
     * will be in reset, because we previously reset the chip). This will
     * restart auto-negotiation.  If auto-neogtiation is successful then the
     * link-up status bit will be set and the flow control enable bits (RFCE
     * and TFCE) will be set according to their negotiated value.
     */
    DEBUGOUT("Auto-negotiation enabled\n");

    E1000_WRITE_REG(hw, TXCW, txcw);
    E1000_WRITE_REG(hw, CTRL, ctrl);
    E1000_WRITE_FLUSH(hw);

    hw->txcw = txcw;
    msec_delay(1);

    /* If we have a signal (the cable is plugged in) then poll for a "Link-Up"
     * indication in the Device Status Register.  Time-out if a link isn't
     * seen in 500 milliseconds seconds (Auto-negotiation should complete in
     * less than 500 milliseconds even if the other end is doing it in SW).
     * For internal serdes, we just assume a signal is present, then poll.
     */
    if(hw->media_type == e1000_media_type_internal_serdes ||
       (E1000_READ_REG(hw, CTRL) & E1000_CTRL_SWDPIN1) == signal) {
        DEBUGOUT("Looking for Link\n");
        for(i = 0; i < (LINK_UP_TIMEOUT / 10); i++) {
            msec_delay(10);
            status = E1000_READ_REG(hw, STATUS);
            if(status & E1000_STATUS_LU) break;
        }
        if(i == (LINK_UP_TIMEOUT / 10)) {
            DEBUGOUT("Never got a valid link from auto-neg!!!\n");
            hw->autoneg_failed = 1;
            /* AutoNeg failed to achieve a link, so we'll call
             * e1000_check_for_link. This routine will force the link up if
             * we detect a signal. This will allow us to communicate with
             * non-autonegotiating link partners.
             */
            ret_val = e1000_check_for_link(hw);
            if(ret_val) {
                DEBUGOUT("Error while checking for link\n");
                return ret_val;
            }
            hw->autoneg_failed = 0;
        } else {
            hw->autoneg_failed = 0;
            DEBUGOUT("Valid Link Found\n");
        }
    } else {
        DEBUGOUT("No Signal Detected\n");
    }
    return E1000_SUCCESS;
}

/******************************************************************************
* Detects which PHY is present and the speed and duplex
*
* hw - Struct containing variables accessed by shared code
******************************************************************************/
static int32_t
e1000_setup_copper_link(struct e1000_hw *hw)
{
    uint32_t ctrl;
    uint32_t led_ctrl;
    int32_t ret_val;
    uint16_t i;
    uint16_t phy_data;

    DEBUGFUNC("e1000_setup_copper_link");

    ctrl = E1000_READ_REG(hw, CTRL);
    /* With 82543, we need to force speed and duplex on the MAC equal to what
     * the PHY speed and duplex configuration is. In addition, we need to
     * perform a hardware reset on the PHY to take it out of reset.
     */
    if(hw->mac_type > e1000_82543) {
        ctrl |= E1000_CTRL_SLU;
        ctrl &= ~(E1000_CTRL_FRCSPD | E1000_CTRL_FRCDPX);
        E1000_WRITE_REG(hw, CTRL, ctrl);
    } else {
        ctrl |= (E1000_CTRL_FRCSPD | E1000_CTRL_FRCDPX | E1000_CTRL_SLU);
        E1000_WRITE_REG(hw, CTRL, ctrl);
        e1000_phy_hw_reset(hw);
    }

    /* Make sure we have a valid PHY */
    ret_val = e1000_detect_gig_phy(hw);
    if(ret_val) {
        DEBUGOUT("Error, did not detect valid phy.\n");
        return ret_val;
    }
    DEBUGOUT1("Phy ID = %x \n", hw->phy_id);

    /* Set PHY to class A mode (if necessary) */
    ret_val = e1000_set_phy_mode(hw);
    if(ret_val)
        return ret_val;

    if(hw->mac_type == e1000_82545_rev_3) {
        ret_val = e1000_read_phy_reg(hw, M88E1000_PHY_SPEC_CTRL, &phy_data);
        phy_data |= 0x00000008;
        ret_val = e1000_write_phy_reg(hw, M88E1000_PHY_SPEC_CTRL, phy_data);
    }

    if(hw->mac_type <= e1000_82543 ||
       hw->mac_type == e1000_82541 || hw->mac_type == e1000_82547 ||
       hw->mac_type == e1000_82541_rev_2 || hw->mac_type == e1000_82547_rev_2)
        hw->phy_reset_disable = FALSE;

    if(!hw->phy_reset_disable) {
        if (hw->phy_type == e1000_phy_igp) {

            ret_val = e1000_phy_reset(hw);
            if(ret_val) {
                DEBUGOUT("Error Resetting the PHY\n");
                return ret_val;
            }

            /* Wait 10ms for MAC to configure PHY from eeprom settings */
            msec_delay(15);

            /* Configure activity LED after PHY reset */
            led_ctrl = E1000_READ_REG(hw, LEDCTL);
            led_ctrl &= IGP_ACTIVITY_LED_MASK;
            led_ctrl |= (IGP_ACTIVITY_LED_ENABLE | IGP_LED3_MODE);
            E1000_WRITE_REG(hw, LEDCTL, led_ctrl);

            /* disable lplu d3 during driver init */
            ret_val = e1000_set_d3_lplu_state(hw, FALSE);
            if(ret_val) {
                DEBUGOUT("Error Disabling LPLU D3\n");
                return ret_val;
            }

            /* Configure mdi-mdix settings */
            ret_val = e1000_read_phy_reg(hw, IGP01E1000_PHY_PORT_CTRL,
                                         &phy_data);
            if(ret_val)
                return ret_val;

            if((hw->mac_type == e1000_82541) || (hw->mac_type == e1000_82547)) {
                hw->dsp_config_state = e1000_dsp_config_disabled;
                /* Force MDI for IGP B-0 PHY */
                phy_data &= ~(IGP01E1000_PSCR_AUTO_MDIX |
                              IGP01E1000_PSCR_FORCE_MDI_MDIX);
                hw->mdix = 1;

            } else {
                hw->dsp_config_state = e1000_dsp_config_enabled;
                phy_data &= ~IGP01E1000_PSCR_AUTO_MDIX;

                switch (hw->mdix) {
                case 1:
                    phy_data &= ~IGP01E1000_PSCR_FORCE_MDI_MDIX;
                    break;
                case 2:
                    phy_data |= IGP01E1000_PSCR_FORCE_MDI_MDIX;
                    break;
                case 0:
                default:
                    phy_data |= IGP01E1000_PSCR_AUTO_MDIX;
                    break;
                }
            }
            ret_val = e1000_write_phy_reg(hw, IGP01E1000_PHY_PORT_CTRL,
                                          phy_data);
            if(ret_val)
                return ret_val;

            /* set auto-master slave resolution settings */
            if(hw->autoneg) {
                e1000_ms_type phy_ms_setting = hw->master_slave;

                if(hw->ffe_config_state == e1000_ffe_config_active)
                    hw->ffe_config_state = e1000_ffe_config_enabled;

                if(hw->dsp_config_state == e1000_dsp_config_activated)
                    hw->dsp_config_state = e1000_dsp_config_enabled;

                /* when autonegotiation advertisment is only 1000Mbps then we
                 * should disable SmartSpeed and enable Auto MasterSlave
                 * resolution as hardware default. */
                if(hw->autoneg_advertised == ADVERTISE_1000_FULL) {
                    /* Disable SmartSpeed */
                    ret_val = e1000_read_phy_reg(hw, IGP01E1000_PHY_PORT_CONFIG,
                                                 &phy_data);
                    if(ret_val)
                        return ret_val;
                    phy_data &= ~IGP01E1000_PSCFR_SMART_SPEED;
                    ret_val = e1000_write_phy_reg(hw,
                                                  IGP01E1000_PHY_PORT_CONFIG,
                                                  phy_data);
                    if(ret_val)
                        return ret_val;
                    /* Set auto Master/Slave resolution process */
                    ret_val = e1000_read_phy_reg(hw, PHY_1000T_CTRL, &phy_data);
                    if(ret_val)
                        return ret_val;
                    phy_data &= ~CR_1000T_MS_ENABLE;
                    ret_val = e1000_write_phy_reg(hw, PHY_1000T_CTRL, phy_data);
                    if(ret_val)
                        return ret_val;
                }

                ret_val = e1000_read_phy_reg(hw, PHY_1000T_CTRL, &phy_data);
                if(ret_val)
                    return ret_val;

                /* load defaults for future use */
                hw->original_master_slave = (phy_data & CR_1000T_MS_ENABLE) ?
                                            ((phy_data & CR_1000T_MS_VALUE) ?
                                             e1000_ms_force_master :
                                             e1000_ms_force_slave) :
                                             e1000_ms_auto;

                switch (phy_ms_setting) {
                case e1000_ms_force_master:
                    phy_data |= (CR_1000T_MS_ENABLE | CR_1000T_MS_VALUE);
                    break;
                case e1000_ms_force_slave:
                    phy_data |= CR_1000T_MS_ENABLE;
                    phy_data &= ~(CR_1000T_MS_VALUE);
                    break;
                case e1000_ms_auto:
                    phy_data &= ~CR_1000T_MS_ENABLE;
                default:
                    break;
                }
                ret_val = e1000_write_phy_reg(hw, PHY_1000T_CTRL, phy_data);
                if(ret_val)
                    return ret_val;
            }
        } else {
            /* Enable CRS on TX. This must be set for half-duplex operation. */
            ret_val = e1000_read_phy_reg(hw, M88E1000_PHY_SPEC_CTRL,
                                         &phy_data);
            if(ret_val)
                return ret_val;

            phy_data |= M88E1000_PSCR_ASSERT_CRS_ON_TX;

            /* Options:
             *   MDI/MDI-X = 0 (default)
             *   0 - Auto for all speeds
             *   1 - MDI mode
             *   2 - MDI-X mode
             *   3 - Auto for 1000Base-T only (MDI-X for 10/100Base-T modes)
             */
            phy_data &= ~M88E1000_PSCR_AUTO_X_MODE;

            switch (hw->mdix) {
            case 1:
                phy_data |= M88E1000_PSCR_MDI_MANUAL_MODE;
                break;
            case 2:
                phy_data |= M88E1000_PSCR_MDIX_MANUAL_MODE;
                break;
            case 3:
                phy_data |= M88E1000_PSCR_AUTO_X_1000T;
                break;
            case 0:
            default:
                phy_data |= M88E1000_PSCR_AUTO_X_MODE;
                break;
            }

            /* Options:
             *   disable_polarity_correction = 0 (default)
             *       Automatic Correction for Reversed Cable Polarity
             *   0 - Disabled
             *   1 - Enabled
             */
            phy_data &= ~M88E1000_PSCR_POLARITY_REVERSAL;
            if(hw->disable_polarity_correction == 1)
                phy_data |= M88E1000_PSCR_POLARITY_REVERSAL;
            ret_val = e1000_write_phy_reg(hw, M88E1000_PHY_SPEC_CTRL,
                                          phy_data);
            if(ret_val)
                return ret_val;

            /* Force TX_CLK in the Extended PHY Specific Control Register
             * to 25MHz clock.
             */
            ret_val = e1000_read_phy_reg(hw, M88E1000_EXT_PHY_SPEC_CTRL,
                                         &phy_data);
            if(ret_val)
                return ret_val;

            phy_data |= M88E1000_EPSCR_TX_CLK_25;

            if (hw->phy_revision < M88E1011_I_REV_4) {
                /* Configure Master and Slave downshift values */
                phy_data &= ~(M88E1000_EPSCR_MASTER_DOWNSHIFT_MASK |
                              M88E1000_EPSCR_SLAVE_DOWNSHIFT_MASK);
                phy_data |= (M88E1000_EPSCR_MASTER_DOWNSHIFT_1X |
                             M88E1000_EPSCR_SLAVE_DOWNSHIFT_1X);
                ret_val = e1000_write_phy_reg(hw, M88E1000_EXT_PHY_SPEC_CTRL,
                                              phy_data);
                if(ret_val)
                    return ret_val;
            }

            /* SW Reset the PHY so all changes take effect */
            ret_val = e1000_phy_reset(hw);
            if(ret_val) {
                DEBUGOUT("Error Resetting the PHY\n");
                return ret_val;
            }
        }

        /* Options:
         *   autoneg = 1 (default)
         *      PHY will advertise value(s) parsed from
         *      autoneg_advertised and fc
         *   autoneg = 0
         *      PHY will be set to 10H, 10F, 100H, or 100F
         *      depending on value parsed from forced_speed_duplex.
         */

        /* Is autoneg enabled?  This is enabled by default or by software
         * override.  If so, call e1000_phy_setup_autoneg routine to parse the
         * autoneg_advertised and fc options. If autoneg is NOT enabled, then
         * the user should have provided a speed/duplex override.  If so, then
         * call e1000_phy_force_speed_duplex to parse and set this up.
         */
        if(hw->autoneg) {
            /* Perform some bounds checking on the hw->autoneg_advertised
             * parameter.  If this variable is zero, then set it to the default.
             */
            hw->autoneg_advertised &= AUTONEG_ADVERTISE_SPEED_DEFAULT;

            /* If autoneg_advertised is zero, we assume it was not defaulted
             * by the calling code so we set to advertise full capability.
             */
            if(hw->autoneg_advertised == 0)
                hw->autoneg_advertised = AUTONEG_ADVERTISE_SPEED_DEFAULT;

            DEBUGOUT("Reconfiguring auto-neg advertisement params\n");
            ret_val = e1000_phy_setup_autoneg(hw);
            if(ret_val) {
                DEBUGOUT("Error Setting up Auto-Negotiation\n");
                return ret_val;
            }
            DEBUGOUT("Restarting Auto-Neg\n");

            /* Restart auto-negotiation by setting the Auto Neg Enable bit and
             * the Auto Neg Restart bit in the PHY control register.
             */
            ret_val = e1000_read_phy_reg(hw, PHY_CTRL, &phy_data);
            if(ret_val)
                return ret_val;

            phy_data |= (MII_CR_AUTO_NEG_EN | MII_CR_RESTART_AUTO_NEG);
            ret_val = e1000_write_phy_reg(hw, PHY_CTRL, phy_data);
            if(ret_val)
                return ret_val;

            /* Does the user want to wait for Auto-Neg to complete here, or
             * check at a later time (for example, callback routine).
             */
            if(hw->wait_autoneg_complete) {
                ret_val = e1000_wait_autoneg(hw);
                if(ret_val) {
                    DEBUGOUT("Error while waiting for autoneg to complete\n");
                    return ret_val;
                }
            }
            hw->get_link_status = TRUE;
        } else {
            DEBUGOUT("Forcing speed and duplex\n");
            ret_val = e1000_phy_force_speed_duplex(hw);
            if(ret_val) {
                DEBUGOUT("Error Forcing Speed and Duplex\n");
                return ret_val;
            }
        }
    } /* !hw->phy_reset_disable */

    /* Check link status. Wait up to 100 microseconds for link to become
     * valid.
     */
    for(i = 0; i < 10; i++) {
        ret_val = e1000_read_phy_reg(hw, PHY_STATUS, &phy_data);
        if(ret_val)
            return ret_val;
        ret_val = e1000_read_phy_reg(hw, PHY_STATUS, &phy_data);
        if(ret_val)
            return ret_val;

        if(phy_data & MII_SR_LINK_STATUS) {
            /* We have link, so we need to finish the config process:
             *   1) Set up the MAC to the current PHY speed/duplex
             *      if we are on 82543.  If we
             *      are on newer silicon, we only need to configure
             *      collision distance in the Transmit Control Register.
             *   2) Set up flow control on the MAC to that established with
             *      the link partner.
             */
            if(hw->mac_type >= e1000_82544) {
                e1000_config_collision_dist(hw);
            } else {
                ret_val = e1000_config_mac_to_phy(hw);
                if(ret_val) {
                    DEBUGOUT("Error configuring MAC to PHY settings\n");
                    return ret_val;
                }
            }
            ret_val = e1000_config_fc_after_link_up(hw);
            if(ret_val) {
                DEBUGOUT("Error Configuring Flow Control\n");
                return ret_val;
            }
            DEBUGOUT("Valid link established!!!\n");

            if(hw->phy_type == e1000_phy_igp) {
                ret_val = e1000_config_dsp_after_link_change(hw, TRUE);
                if(ret_val) {
                    DEBUGOUT("Error Configuring DSP after link up\n");
                    return ret_val;
                }
            }
            DEBUGOUT("Valid link established!!!\n");
            return E1000_SUCCESS;
        }
        udelay(10);
    }

    DEBUGOUT("Unable to establish link!!!\n");
    return E1000_SUCCESS;
}

/******************************************************************************
* Configures PHY autoneg and flow control advertisement settings
*
* hw - Struct containing variables accessed by shared code
******************************************************************************/
int32_t
e1000_phy_setup_autoneg(struct e1000_hw *hw)
{
    int32_t ret_val;
    uint16_t mii_autoneg_adv_reg;
    uint16_t mii_1000t_ctrl_reg;

    DEBUGFUNC("e1000_phy_setup_autoneg");

    /* Read the MII Auto-Neg Advertisement Register (Address 4). */
    ret_val = e1000_read_phy_reg(hw, PHY_AUTONEG_ADV, &mii_autoneg_adv_reg);
    if(ret_val)
        return ret_val;

    /* Read the MII 1000Base-T Control Register (Address 9). */
    ret_val = e1000_read_phy_reg(hw, PHY_1000T_CTRL, &mii_1000t_ctrl_reg);
    if(ret_val)
        return ret_val;

    /* Need to parse both autoneg_advertised and fc and set up
     * the appropriate PHY registers.  First we will parse for
     * autoneg_advertised software override.  Since we can advertise
     * a plethora of combinations, we need to check each bit
     * individually.
     */

    /* First we clear all the 10/100 mb speed bits in the Auto-Neg
     * Advertisement Register (Address 4) and the 1000 mb speed bits in
     * the  1000Base-T Control Register (Address 9).
     */
    mii_autoneg_adv_reg &= ~REG4_SPEED_MASK;
    mii_1000t_ctrl_reg &= ~REG9_SPEED_MASK;

    DEBUGOUT1("autoneg_advertised %x\n", hw->autoneg_advertised);

    /* Do we want to advertise 10 Mb Half Duplex? */
    if(hw->autoneg_advertised & ADVERTISE_10_HALF) {
        DEBUGOUT("Advertise 10mb Half duplex\n");
        mii_autoneg_adv_reg |= NWAY_AR_10T_HD_CAPS;
    }

    /* Do we want to advertise 10 Mb Full Duplex? */
    if(hw->autoneg_advertised & ADVERTISE_10_FULL) {
        DEBUGOUT("Advertise 10mb Full duplex\n");
        mii_autoneg_adv_reg |= NWAY_AR_10T_FD_CAPS;
    }

    /* Do we want to advertise 100 Mb Half Duplex? */
    if(hw->autoneg_advertised & ADVERTISE_100_HALF) {
        DEBUGOUT("Advertise 100mb Half duplex\n");
        mii_autoneg_adv_reg |= NWAY_AR_100TX_HD_CAPS;
    }

    /* Do we want to advertise 100 Mb Full Duplex? */
    if(hw->autoneg_advertised & ADVERTISE_100_FULL) {
        DEBUGOUT("Advertise 100mb Full duplex\n");
        mii_autoneg_adv_reg |= NWAY_AR_100TX_FD_CAPS;
    }

    /* We do not allow the Phy to advertise 1000 Mb Half Duplex */
    if(hw->autoneg_advertised & ADVERTISE_1000_HALF) {
        DEBUGOUT("Advertise 1000mb Half duplex requested, request denied!\n");
    }

    /* Do we want to advertise 1000 Mb Full Duplex? */
    if(hw->autoneg_advertised & ADVERTISE_1000_FULL) {
        DEBUGOUT("Advertise 1000mb Full duplex\n");
        mii_1000t_ctrl_reg |= CR_1000T_FD_CAPS;
    }

    /* Check for a software override of the flow control settings, and
     * setup the PHY advertisement registers accordingly.  If
     * auto-negotiation is enabled, then software will have to set the
     * "PAUSE" bits to the correct value in the Auto-Negotiation
     * Advertisement Register (PHY_AUTONEG_ADV) and re-start auto-negotiation.
     *
     * The possible values of the "fc" parameter are:
     *      0:  Flow control is completely disabled
     *      1:  Rx flow control is enabled (we can receive pause frames
     *          but not send pause frames).
     *      2:  Tx flow control is enabled (we can send pause frames
     *          but we do not support receiving pause frames).
     *      3:  Both Rx and TX flow control (symmetric) are enabled.
     *  other:  No software override.  The flow control configuration
     *          in the EEPROM is used.
     */
    switch (hw->fc) {
    case e1000_fc_none: /* 0 */
        /* Flow control (RX & TX) is completely disabled by a
         * software over-ride.
         */
        mii_autoneg_adv_reg &= ~(NWAY_AR_ASM_DIR | NWAY_AR_PAUSE);
        break;
    case e1000_fc_rx_pause: /* 1 */
        /* RX Flow control is enabled, and TX Flow control is
         * disabled, by a software over-ride.
         */
        /* Since there really isn't a way to advertise that we are
         * capable of RX Pause ONLY, we will advertise that we
         * support both symmetric and asymmetric RX PAUSE.  Later
         * (in e1000_config_fc_after_link_up) we will disable the
         *hw's ability to send PAUSE frames.
         */
        mii_autoneg_adv_reg |= (NWAY_AR_ASM_DIR | NWAY_AR_PAUSE);
        break;
    case e1000_fc_tx_pause: /* 2 */
        /* TX Flow control is enabled, and RX Flow control is
         * disabled, by a software over-ride.
         */
        mii_autoneg_adv_reg |= NWAY_AR_ASM_DIR;
        mii_autoneg_adv_reg &= ~NWAY_AR_PAUSE;
        break;
    case e1000_fc_full: /* 3 */
        /* Flow control (both RX and TX) is enabled by a software
         * over-ride.
         */
        mii_autoneg_adv_reg |= (NWAY_AR_ASM_DIR | NWAY_AR_PAUSE);
        break;
    default:
        DEBUGOUT("Flow control param set incorrectly\n");
        return -E1000_ERR_CONFIG;
    }

    ret_val = e1000_write_phy_reg(hw, PHY_AUTONEG_ADV, mii_autoneg_adv_reg);
    if(ret_val)
        return ret_val;

    DEBUGOUT1("Auto-Neg Advertising %x\n", mii_autoneg_adv_reg);

    ret_val = e1000_write_phy_reg(hw, PHY_1000T_CTRL, mii_1000t_ctrl_reg);
    if(ret_val)
        return ret_val;

    return E1000_SUCCESS;
}

/******************************************************************************
* Force PHY speed and duplex settings to hw->forced_speed_duplex
*
* hw - Struct containing variables accessed by shared code
******************************************************************************/
static int32_t
e1000_phy_force_speed_duplex(struct e1000_hw *hw)
{
    uint32_t ctrl;
    int32_t ret_val;
    uint16_t mii_ctrl_reg;
    uint16_t mii_status_reg;
    uint16_t phy_data;
    uint16_t i;

    DEBUGFUNC("e1000_phy_force_speed_duplex");

    /* Turn off Flow control if we are forcing speed and duplex. */
    hw->fc = e1000_fc_none;

    DEBUGOUT1("hw->fc = %d\n", hw->fc);

    /* Read the Device Control Register. */
    ctrl = E1000_READ_REG(hw, CTRL);

    /* Set the bits to Force Speed and Duplex in the Device Ctrl Reg. */
    ctrl |= (E1000_CTRL_FRCSPD | E1000_CTRL_FRCDPX);
    ctrl &= ~(DEVICE_SPEED_MASK);

    /* Clear the Auto Speed Detect Enable bit. */
    ctrl &= ~E1000_CTRL_ASDE;

    /* Read the MII Control Register. */
    ret_val = e1000_read_phy_reg(hw, PHY_CTRL, &mii_ctrl_reg);
    if(ret_val)
        return ret_val;

    /* We need to disable autoneg in order to force link and duplex. */

    mii_ctrl_reg &= ~MII_CR_AUTO_NEG_EN;

    /* Are we forcing Full or Half Duplex? */
    if(hw->forced_speed_duplex == e1000_100_full ||
       hw->forced_speed_duplex == e1000_10_full) {
        /* We want to force full duplex so we SET the full duplex bits in the
         * Device and MII Control Registers.
         */
        ctrl |= E1000_CTRL_FD;
        mii_ctrl_reg |= MII_CR_FULL_DUPLEX;
        DEBUGOUT("Full Duplex\n");
    } else {
        /* We want to force half duplex so we CLEAR the full duplex bits in
         * the Device and MII Control Registers.
         */
        ctrl &= ~E1000_CTRL_FD;
        mii_ctrl_reg &= ~MII_CR_FULL_DUPLEX;
        DEBUGOUT("Half Duplex\n");
    }

    /* Are we forcing 100Mbps??? */
    if(hw->forced_speed_duplex == e1000_100_full ||
       hw->forced_speed_duplex == e1000_100_half) {
        /* Set the 100Mb bit and turn off the 1000Mb and 10Mb bits. */
        ctrl |= E1000_CTRL_SPD_100;
        mii_ctrl_reg |= MII_CR_SPEED_100;
        mii_ctrl_reg &= ~(MII_CR_SPEED_1000 | MII_CR_SPEED_10);
        DEBUGOUT("Forcing 100mb ");
    } else {
        /* Set the 10Mb bit and turn off the 1000Mb and 100Mb bits. */
        ctrl &= ~(E1000_CTRL_SPD_1000 | E1000_CTRL_SPD_100);
        mii_ctrl_reg |= MII_CR_SPEED_10;
        mii_ctrl_reg &= ~(MII_CR_SPEED_1000 | MII_CR_SPEED_100);
        DEBUGOUT("Forcing 10mb ");
    }

    e1000_config_collision_dist(hw);

    /* Write the configured values back to the Device Control Reg. */
    E1000_WRITE_REG(hw, CTRL, ctrl);

    if (hw->phy_type == e1000_phy_m88) {
        ret_val = e1000_read_phy_reg(hw, M88E1000_PHY_SPEC_CTRL, &phy_data);
        if(ret_val)
            return ret_val;

        /* Clear Auto-Crossover to force MDI manually. M88E1000 requires MDI
         * forced whenever speed are duplex are forced.
         */
        phy_data &= ~M88E1000_PSCR_AUTO_X_MODE;
        ret_val = e1000_write_phy_reg(hw, M88E1000_PHY_SPEC_CTRL, phy_data);
        if(ret_val)
            return ret_val;

        DEBUGOUT1("M88E1000 PSCR: %x \n", phy_data);

        /* Need to reset the PHY or these changes will be ignored */
        mii_ctrl_reg |= MII_CR_RESET;
    } else {
        /* Clear Auto-Crossover to force MDI manually.  IGP requires MDI
         * forced whenever speed or duplex are forced.
         */
        ret_val = e1000_read_phy_reg(hw, IGP01E1000_PHY_PORT_CTRL, &phy_data);
        if(ret_val)
            return ret_val;

        phy_data &= ~IGP01E1000_PSCR_AUTO_MDIX;
        phy_data &= ~IGP01E1000_PSCR_FORCE_MDI_MDIX;

        ret_val = e1000_write_phy_reg(hw, IGP01E1000_PHY_PORT_CTRL, phy_data);
        if(ret_val)
            return ret_val;
    }

    /* Write back the modified PHY MII control register. */
    ret_val = e1000_write_phy_reg(hw, PHY_CTRL, mii_ctrl_reg);
    if(ret_val)
        return ret_val;

    udelay(1);

    /* The wait_autoneg_complete flag may be a little misleading here.
     * Since we are forcing speed and duplex, Auto-Neg is not enabled.
     * But we do want to delay for a period while forcing only so we
     * don't generate false No Link messages.  So we will wait here
     * only if the user has set wait_autoneg_complete to 1, which is
     * the default.
     */
    if(hw->wait_autoneg_complete) {
        /* We will wait for autoneg to complete. */
        DEBUGOUT("Waiting for forced speed/duplex link.\n");
        mii_status_reg = 0;

        /* We will wait for autoneg to complete or 4.5 seconds to expire. */
        for(i = PHY_FORCE_TIME; i > 0; i--) {
            /* Read the MII Status Register and wait for Auto-Neg Complete bit
             * to be set.
             */
            ret_val = e1000_read_phy_reg(hw, PHY_STATUS, &mii_status_reg);
            if(ret_val)
                return ret_val;

            ret_val = e1000_read_phy_reg(hw, PHY_STATUS, &mii_status_reg);
            if(ret_val)
                return ret_val;

            if(mii_status_reg & MII_SR_LINK_STATUS) break;
            msec_delay(100);
        }
        if((i == 0) && (hw->phy_type == e1000_phy_m88)) {
            /* We didn't get link.  Reset the DSP and wait again for link. */
            ret_val = e1000_phy_reset_dsp(hw);
            if(ret_val) {
                DEBUGOUT("Error Resetting PHY DSP\n");
                return ret_val;
            }
        }
        /* This loop will early-out if the link condition has been met.  */
        for(i = PHY_FORCE_TIME; i > 0; i--) {
            if(mii_status_reg & MII_SR_LINK_STATUS) break;
            msec_delay(100);
            /* Read the MII Status Register and wait for Auto-Neg Complete bit
             * to be set.
             */
            ret_val = e1000_read_phy_reg(hw, PHY_STATUS, &mii_status_reg);
            if(ret_val)
                return ret_val;

            ret_val = e1000_read_phy_reg(hw, PHY_STATUS, &mii_status_reg);
            if(ret_val)
                return ret_val;
        }
    }

    if (hw->phy_type == e1000_phy_m88) {
        /* Because we reset the PHY above, we need to re-force TX_CLK in the
         * Extended PHY Specific Control Register to 25MHz clock.  This value
         * defaults back to a 2.5MHz clock when the PHY is reset.
         */
        ret_val = e1000_read_phy_reg(hw, M88E1000_EXT_PHY_SPEC_CTRL, &phy_data);
        if(ret_val)
            return ret_val;

        phy_data |= M88E1000_EPSCR_TX_CLK_25;
        ret_val = e1000_write_phy_reg(hw, M88E1000_EXT_PHY_SPEC_CTRL, phy_data);
        if(ret_val)
            return ret_val;

        /* In addition, because of the s/w reset above, we need to enable CRS on
         * TX.  This must be set for both full and half duplex operation.
         */
        ret_val = e1000_read_phy_reg(hw, M88E1000_PHY_SPEC_CTRL, &phy_data);
        if(ret_val)
            return ret_val;

        phy_data |= M88E1000_PSCR_ASSERT_CRS_ON_TX;
        ret_val = e1000_write_phy_reg(hw, M88E1000_PHY_SPEC_CTRL, phy_data);
        if(ret_val)
            return ret_val;
    }
    return E1000_SUCCESS;
}

/******************************************************************************
* Sets the collision distance in the Transmit Control register
*
* hw - Struct containing variables accessed by shared code
*
* Link should have been established previously. Reads the speed and duplex
* information from the Device Status register.
******************************************************************************/
void
e1000_config_collision_dist(struct e1000_hw *hw)
{
    uint32_t tctl;

    DEBUGFUNC("e1000_config_collision_dist");

    tctl = E1000_READ_REG(hw, TCTL);

    tctl &= ~E1000_TCTL_COLD;
    tctl |= E1000_COLLISION_DISTANCE << E1000_COLD_SHIFT;

    E1000_WRITE_REG(hw, TCTL, tctl);
    E1000_WRITE_FLUSH(hw);
}

/******************************************************************************
* Sets MAC speed and duplex settings to reflect the those in the PHY
*
* hw - Struct containing variables accessed by shared code
* mii_reg - data to write to the MII control register
*
* The contents of the PHY register containing the needed information need to
* be passed in.
******************************************************************************/
static int32_t
e1000_config_mac_to_phy(struct e1000_hw *hw)
{
    uint32_t ctrl;
    int32_t ret_val;
    uint16_t phy_data;

    DEBUGFUNC("e1000_config_mac_to_phy");

    /* Read the Device Control Register and set the bits to Force Speed
     * and Duplex.
     */
    ctrl = E1000_READ_REG(hw, CTRL);
    ctrl |= (E1000_CTRL_FRCSPD | E1000_CTRL_FRCDPX);
    ctrl &= ~(E1000_CTRL_SPD_SEL | E1000_CTRL_ILOS);

    /* Set up duplex in the Device Control and Transmit Control
     * registers depending on negotiated values.
     */
    if (hw->phy_type == e1000_phy_igp) {
        ret_val = e1000_read_phy_reg(hw, IGP01E1000_PHY_PORT_STATUS,
                                     &phy_data);
        if(ret_val)
            return ret_val;

        if(phy_data & IGP01E1000_PSSR_FULL_DUPLEX) ctrl |= E1000_CTRL_FD;
        else ctrl &= ~E1000_CTRL_FD;

        e1000_config_collision_dist(hw);

        /* Set up speed in the Device Control register depending on
         * negotiated values.
         */
        if((phy_data & IGP01E1000_PSSR_SPEED_MASK) ==
           IGP01E1000_PSSR_SPEED_1000MBPS)
            ctrl |= E1000_CTRL_SPD_1000;
        else if((phy_data & IGP01E1000_PSSR_SPEED_MASK) ==
                IGP01E1000_PSSR_SPEED_100MBPS)
            ctrl |= E1000_CTRL_SPD_100;
    } else {
        ret_val = e1000_read_phy_reg(hw, M88E1000_PHY_SPEC_STATUS,
                                     &phy_data);
        if(ret_val)
            return ret_val;

        if(phy_data & M88E1000_PSSR_DPLX) ctrl |= E1000_CTRL_FD;
        else ctrl &= ~E1000_CTRL_FD;

        e1000_config_collision_dist(hw);

        /* Set up speed in the Device Control register depending on
         * negotiated values.
         */
        if((phy_data & M88E1000_PSSR_SPEED) == M88E1000_PSSR_1000MBS)
            ctrl |= E1000_CTRL_SPD_1000;
        else if((phy_data & M88E1000_PSSR_SPEED) == M88E1000_PSSR_100MBS)
            ctrl |= E1000_CTRL_SPD_100;
    }
    /* Write the configured values back to the Device Control Reg. */
    E1000_WRITE_REG(hw, CTRL, ctrl);
    return E1000_SUCCESS;
}

/******************************************************************************
 * Forces the MAC's flow control settings.
 *
 * hw - Struct containing variables accessed by shared code
 *
 * Sets the TFCE and RFCE bits in the device control register to reflect
 * the adapter settings. TFCE and RFCE need to be explicitly set by
 * software when a Copper PHY is used because autonegotiation is managed
 * by the PHY rather than the MAC. Software must also configure these
 * bits when link is forced on a fiber connection.
 *****************************************************************************/
int32_t
e1000_force_mac_fc(struct e1000_hw *hw)
{
    uint32_t ctrl;

    DEBUGFUNC("e1000_force_mac_fc");

    /* Get the current configuration of the Device Control Register */
    ctrl = E1000_READ_REG(hw, CTRL);

    /* Because we didn't get link via the internal auto-negotiation
     * mechanism (we either forced link or we got link via PHY
     * auto-neg), we have to manually enable/disable transmit an
     * receive flow control.
     *
     * The "Case" statement below enables/disable flow control
     * according to the "hw->fc" parameter.
     *
     * The possible values of the "fc" parameter are:
     *      0:  Flow control is completely disabled
     *      1:  Rx flow control is enabled (we can receive pause
     *          frames but not send pause frames).
     *      2:  Tx flow control is enabled (we can send pause frames
     *          frames but we do not receive pause frames).
     *      3:  Both Rx and TX flow control (symmetric) is enabled.
     *  other:  No other values should be possible at this point.
     */

    switch (hw->fc) {
    case e1000_fc_none:
        ctrl &= (~(E1000_CTRL_TFCE | E1000_CTRL_RFCE));
        break;
    case e1000_fc_rx_pause:
        ctrl &= (~E1000_CTRL_TFCE);
        ctrl |= E1000_CTRL_RFCE;
        break;
    case e1000_fc_tx_pause:
        ctrl &= (~E1000_CTRL_RFCE);
        ctrl |= E1000_CTRL_TFCE;
        break;
    case e1000_fc_full:
        ctrl |= (E1000_CTRL_TFCE | E1000_CTRL_RFCE);
        break;
    default:
        DEBUGOUT("Flow control param set incorrectly\n");
        return -E1000_ERR_CONFIG;
    }

    /* Disable TX Flow Control for 82542 (rev 2.0) */
    if(hw->mac_type == e1000_82542_rev2_0)
        ctrl &= (~E1000_CTRL_TFCE);

    E1000_WRITE_REG(hw, CTRL, ctrl);
    return E1000_SUCCESS;
}

/******************************************************************************
 * Configures flow control settings after link is established
 *
 * hw - Struct containing variables accessed by shared code
 *
 * Should be called immediately after a valid link has been established.
 * Forces MAC flow control settings if link was forced. When in MII/GMII mode
 * and autonegotiation is enabled, the MAC flow control settings will be set
 * based on the flow control negotiated by the PHY. In TBI mode, the TFCE
 * and RFCE bits will be automaticaly set to the negotiated flow control mode.
 *****************************************************************************/
int32_t
e1000_config_fc_after_link_up(struct e1000_hw *hw)
{
    int32_t ret_val;
    uint16_t mii_status_reg;
    uint16_t mii_nway_adv_reg;
    uint16_t mii_nway_lp_ability_reg;
    uint16_t speed;
    uint16_t duplex;

    DEBUGFUNC("e1000_config_fc_after_link_up");

    /* Check for the case where we have fiber media and auto-neg failed
     * so we had to force link.  In this case, we need to force the
     * configuration of the MAC to match the "fc" parameter.
     */
    if(((hw->media_type == e1000_media_type_fiber) && (hw->autoneg_failed)) ||
       ((hw->media_type == e1000_media_type_internal_serdes) && (hw->autoneg_failed)) ||
       ((hw->media_type == e1000_media_type_copper) && (!hw->autoneg))) {
        ret_val = e1000_force_mac_fc(hw);
        if(ret_val) {
            DEBUGOUT("Error forcing flow control settings\n");
            return ret_val;
        }
    }

    /* Check for the case where we have copper media and auto-neg is
     * enabled.  In this case, we need to check and see if Auto-Neg
     * has completed, and if so, how the PHY and link partner has
     * flow control configured.
     */
    if((hw->media_type == e1000_media_type_copper) && hw->autoneg) {
        /* Read the MII Status Register and check to see if AutoNeg
         * has completed.  We read this twice because this reg has
         * some "sticky" (latched) bits.
         */
        ret_val = e1000_read_phy_reg(hw, PHY_STATUS, &mii_status_reg);
        if(ret_val)
            return ret_val;
        ret_val = e1000_read_phy_reg(hw, PHY_STATUS, &mii_status_reg);
        if(ret_val)
            return ret_val;

        if(mii_status_reg & MII_SR_AUTONEG_COMPLETE) {
            /* The AutoNeg process has completed, so we now need to
             * read both the Auto Negotiation Advertisement Register
             * (Address 4) and the Auto_Negotiation Base Page Ability
             * Register (Address 5) to determine how flow control was
             * negotiated.
             */
            ret_val = e1000_read_phy_reg(hw, PHY_AUTONEG_ADV,
                                         &mii_nway_adv_reg);
            if(ret_val)
                return ret_val;
            ret_val = e1000_read_phy_reg(hw, PHY_LP_ABILITY,
                                         &mii_nway_lp_ability_reg);
            if(ret_val)
                return ret_val;

            /* Two bits in the Auto Negotiation Advertisement Register
             * (Address 4) and two bits in the Auto Negotiation Base
             * Page Ability Register (Address 5) determine flow control
             * for both the PHY and the link partner.  The following
             * table, taken out of the IEEE 802.3ab/D6.0 dated March 25,
             * 1999, describes these PAUSE resolution bits and how flow
             * control is determined based upon these settings.
             * NOTE:  DC = Don't Care
             *
             *   LOCAL DEVICE  |   LINK PARTNER
             * PAUSE | ASM_DIR | PAUSE | ASM_DIR | NIC Resolution
             *-------|---------|-------|---------|--------------------
             *   0   |    0    |  DC   |   DC    | e1000_fc_none
             *   0   |    1    |   0   |   DC    | e1000_fc_none
             *   0   |    1    |   1   |    0    | e1000_fc_none
             *   0   |    1    |   1   |    1    | e1000_fc_tx_pause
             *   1   |    0    |   0   |   DC    | e1000_fc_none
             *   1   |   DC    |   1   |   DC    | e1000_fc_full
             *   1   |    1    |   0   |    0    | e1000_fc_none
             *   1   |    1    |   0   |    1    | e1000_fc_rx_pause
             *
             */
            /* Are both PAUSE bits set to 1?  If so, this implies
             * Symmetric Flow Control is enabled at both ends.  The
             * ASM_DIR bits are irrelevant per the spec.
             *
             * For Symmetric Flow Control:
             *
             *   LOCAL DEVICE  |   LINK PARTNER
             * PAUSE | ASM_DIR | PAUSE | ASM_DIR | Result
             *-------|---------|-------|---------|--------------------
             *   1   |   DC    |   1   |   DC    | e1000_fc_full
             *
             */
            if((mii_nway_adv_reg & NWAY_AR_PAUSE) &&
               (mii_nway_lp_ability_reg & NWAY_LPAR_PAUSE)) {
                /* Now we need to check if the user selected RX ONLY
                 * of pause frames.  In this case, we had to advertise
                 * FULL flow control because we could not advertise RX
                 * ONLY. Hence, we must now check to see if we need to
                 * turn OFF  the TRANSMISSION of PAUSE frames.
                 */
                if(hw->original_fc == e1000_fc_full) {
                    hw->fc = e1000_fc_full;
                    DEBUGOUT("Flow Control = FULL.\r\n");
                } else {
                    hw->fc = e1000_fc_rx_pause;
                    DEBUGOUT("Flow Control = RX PAUSE frames only.\r\n");
                }
            }
            /* For receiving PAUSE frames ONLY.
             *
             *   LOCAL DEVICE  |   LINK PARTNER
             * PAUSE | ASM_DIR | PAUSE | ASM_DIR | Result
             *-------|---------|-------|---------|--------------------
             *   0   |    1    |   1   |    1    | e1000_fc_tx_pause
             *
             */
            else if(!(mii_nway_adv_reg & NWAY_AR_PAUSE) &&
                    (mii_nway_adv_reg & NWAY_AR_ASM_DIR) &&
                    (mii_nway_lp_ability_reg & NWAY_LPAR_PAUSE) &&
                    (mii_nway_lp_ability_reg & NWAY_LPAR_ASM_DIR)) {
                hw->fc = e1000_fc_tx_pause;
                DEBUGOUT("Flow Control = TX PAUSE frames only.\r\n");
            }
            /* For transmitting PAUSE frames ONLY.
             *
             *   LOCAL DEVICE  |   LINK PARTNER
             * PAUSE | ASM_DIR | PAUSE | ASM_DIR | Result
             *-------|---------|-------|---------|--------------------
             *   1   |    1    |   0   |    1    | e1000_fc_rx_pause
             *
             */
            else if((mii_nway_adv_reg & NWAY_AR_PAUSE) &&
                    (mii_nway_adv_reg & NWAY_AR_ASM_DIR) &&
                    !(mii_nway_lp_ability_reg & NWAY_LPAR_PAUSE) &&
                    (mii_nway_lp_ability_reg & NWAY_LPAR_ASM_DIR)) {
                hw->fc = e1000_fc_rx_pause;
                DEBUGOUT("Flow Control = RX PAUSE frames only.\r\n");
            }
            /* Per the IEEE spec, at this point flow control should be
             * disabled.  However, we want to consider that we could
             * be connected to a legacy switch that doesn't advertise
             * desired flow control, but can be forced on the link
             * partner.  So if we advertised no flow control, that is
             * what we will resolve to.  If we advertised some kind of
             * receive capability (Rx Pause Only or Full Flow Control)
             * and the link partner advertised none, we will configure
             * ourselves to enable Rx Flow Control only.  We can do
             * this safely for two reasons:  If the link partner really
             * didn't want flow control enabled, and we enable Rx, no
             * harm done since we won't be receiving any PAUSE frames
             * anyway.  If the intent on the link partner was to have
             * flow control enabled, then by us enabling RX only, we
             * can at least receive pause frames and process them.
             * This is a good idea because in most cases, since we are
             * predominantly a server NIC, more times than not we will
             * be asked to delay transmission of packets than asking
             * our link partner to pause transmission of frames.
             */
            else if((hw->original_fc == e1000_fc_none ||
                     hw->original_fc == e1000_fc_tx_pause) ||
                    hw->fc_strict_ieee) {
                hw->fc = e1000_fc_none;
                DEBUGOUT("Flow Control = NONE.\r\n");
            } else {
                hw->fc = e1000_fc_rx_pause;
                DEBUGOUT("Flow Control = RX PAUSE frames only.\r\n");
            }

            /* Now we need to do one last check...  If we auto-
             * negotiated to HALF DUPLEX, flow control should not be
             * enabled per IEEE 802.3 spec.
             */
            ret_val = e1000_get_speed_and_duplex(hw, &speed, &duplex);
            if(ret_val) {
                DEBUGOUT("Error getting link speed and duplex\n");
                return ret_val;
            }

            if(duplex == HALF_DUPLEX)
                hw->fc = e1000_fc_none;

            /* Now we call a subroutine to actually force the MAC
             * controller to use the correct flow control settings.
             */
            ret_val = e1000_force_mac_fc(hw);
            if(ret_val) {
                DEBUGOUT("Error forcing flow control settings\n");
                return ret_val;
            }
        } else {
            DEBUGOUT("Copper PHY and Auto Neg has not completed.\r\n");
        }
    }
    return E1000_SUCCESS;
}

/******************************************************************************
 * Checks to see if the link status of the hardware has changed.
 *
 * hw - Struct containing variables accessed by shared code
 *
 * Called by any function that needs to check the link status of the adapter.
 *****************************************************************************/
int32_t
e1000_check_for_link(struct e1000_hw *hw)
{
    uint32_t rxcw = 0;
    uint32_t ctrl;
    uint32_t status;
    uint32_t rctl;
    uint32_t signal = 0;
    int32_t ret_val;
    uint16_t phy_data;

    DEBUGFUNC("e1000_check_for_link");

    ctrl = E1000_READ_REG(hw, CTRL);
    status = E1000_READ_REG(hw, STATUS);

    /* On adapters with a MAC newer than 82544, SW Defineable pin 1 will be
     * set when the optics detect a signal. On older adapters, it will be
     * cleared when there is a signal.  This applies to fiber media only.
     */
    if((hw->media_type == e1000_media_type_fiber) ||
       (hw->media_type == e1000_media_type_internal_serdes)) {
        rxcw = E1000_READ_REG(hw, RXCW);

        if(hw->media_type == e1000_media_type_fiber) {
            signal = (hw->mac_type > e1000_82544) ? E1000_CTRL_SWDPIN1 : 0;
            if(status & E1000_STATUS_LU)
                hw->get_link_status = FALSE;
        }
    }

    /* If we have a copper PHY then we only want to go out to the PHY
     * registers to see if Auto-Neg has completed and/or if our link
     * status has changed.  The get_link_status flag will be set if we
     * receive a Link Status Change interrupt or we have Rx Sequence
     * Errors.
     */
    if((hw->media_type == e1000_media_type_copper) && hw->get_link_status) {
        /* First we want to see if the MII Status Register reports
         * link.  If so, then we want to get the current speed/duplex
         * of the PHY.
         * Read the register twice since the link bit is sticky.
         */
        ret_val = e1000_read_phy_reg(hw, PHY_STATUS, &phy_data);
        if(ret_val)
            return ret_val;
        ret_val = e1000_read_phy_reg(hw, PHY_STATUS, &phy_data);
        if(ret_val)
            return ret_val;

        if(phy_data & MII_SR_LINK_STATUS) {
            hw->get_link_status = FALSE;
            /* Check if there was DownShift, must be checked immediately after
             * link-up */
            e1000_check_downshift(hw);

        } else {
            /* No link detected */
            e1000_config_dsp_after_link_change(hw, FALSE);
            return 0;
        }

        /* If we are forcing speed/duplex, then we simply return since
         * we have already determined whether we have link or not.
         */
        if(!hw->autoneg) return -E1000_ERR_CONFIG;

        /* optimize the dsp settings for the igp phy */
        e1000_config_dsp_after_link_change(hw, TRUE);

        /* We have a M88E1000 PHY and Auto-Neg is enabled.  If we
         * have Si on board that is 82544 or newer, Auto
         * Speed Detection takes care of MAC speed/duplex
         * configuration.  So we only need to configure Collision
         * Distance in the MAC.  Otherwise, we need to force
         * speed/duplex on the MAC to the current PHY speed/duplex
         * settings.
         */
        if(hw->mac_type >= e1000_82544)
            e1000_config_collision_dist(hw);
        else {
            ret_val = e1000_config_mac_to_phy(hw);
            if(ret_val) {
                DEBUGOUT("Error configuring MAC to PHY settings\n");
                return ret_val;
            }
        }

        /* Configure Flow Control now that Auto-Neg has completed. First, we
         * need to restore the desired flow control settings because we may
         * have had to re-autoneg with a different link partner.
         */
        ret_val = e1000_config_fc_after_link_up(hw);
        if(ret_val) {
            DEBUGOUT("Error configuring flow control\n");
            return ret_val;
        }

        /* At this point we know that we are on copper and we have
         * auto-negotiated link.  These are conditions for checking the link
         * partner capability register.  We use the link speed to determine if
         * TBI compatibility needs to be turned on or off.  If the link is not
         * at gigabit speed, then TBI compatibility is not needed.  If we are
         * at gigabit speed, we turn on TBI compatibility.
         */
	if(hw->tbi_compatibility_en) {
            uint16_t speed, duplex;
            e1000_get_speed_and_duplex(hw, &speed, &duplex);
            if(speed != SPEED_1000) {
                /* If link speed is not set to gigabit speed, we do not need
                 * to enable TBI compatibility.
                 */
                if(hw->tbi_compatibility_on) {
                    /* If we previously were in the mode, turn it off. */
                    rctl = E1000_READ_REG(hw, RCTL);
                    rctl &= ~E1000_RCTL_SBP;
                    E1000_WRITE_REG(hw, RCTL, rctl);
                    hw->tbi_compatibility_on = FALSE;
                }
            } else {
                /* If TBI compatibility is was previously off, turn it on. For
                 * compatibility with a TBI link partner, we will store bad
                 * packets. Some frames have an additional byte on the end and
                 * will look like CRC errors to to the hardware.
                 */
                if(!hw->tbi_compatibility_on) {
                    hw->tbi_compatibility_on = TRUE;
                    rctl = E1000_READ_REG(hw, RCTL);
                    rctl |= E1000_RCTL_SBP;
                    E1000_WRITE_REG(hw, RCTL, rctl);
                }
            }
        }
    }
    /* If we don't have link (auto-negotiation failed or link partner cannot
     * auto-negotiate), the cable is plugged in (we have signal), and our
     * link partner is not trying to auto-negotiate with us (we are receiving
     * idles or data), we need to force link up. We also need to give
     * auto-negotiation time to complete, in case the cable was just plugged
     * in. The autoneg_failed flag does this.
     */
    else if((((hw->media_type == e1000_media_type_fiber) &&
              ((ctrl & E1000_CTRL_SWDPIN1) == signal)) ||
             (hw->media_type == e1000_media_type_internal_serdes)) &&
            (!(status & E1000_STATUS_LU)) &&
            (!(rxcw & E1000_RXCW_C))) {
        if(hw->autoneg_failed == 0) {
            hw->autoneg_failed = 1;
            return 0;
        }
        DEBUGOUT("NOT RXing /C/, disable AutoNeg and force link.\r\n");

        /* Disable auto-negotiation in the TXCW register */
        E1000_WRITE_REG(hw, TXCW, (hw->txcw & ~E1000_TXCW_ANE));

        /* Force link-up and also force full-duplex. */
        ctrl = E1000_READ_REG(hw, CTRL);
        ctrl |= (E1000_CTRL_SLU | E1000_CTRL_FD);
        E1000_WRITE_REG(hw, CTRL, ctrl);

        /* Configure Flow Control after forcing link up. */
        ret_val = e1000_config_fc_after_link_up(hw);
        if(ret_val) {
            DEBUGOUT("Error configuring flow control\n");
            return ret_val;
        }
    }
    /* If we are forcing link and we are receiving /C/ ordered sets, re-enable
     * auto-negotiation in the TXCW register and disable forced link in the
     * Device Control register in an attempt to auto-negotiate with our link
     * partner.
     */
    else if(((hw->media_type == e1000_media_type_fiber) ||
             (hw->media_type == e1000_media_type_internal_serdes)) &&
            (ctrl & E1000_CTRL_SLU) && (rxcw & E1000_RXCW_C)) {
        DEBUGOUT("RXing /C/, enable AutoNeg and stop forcing link.\r\n");
        E1000_WRITE_REG(hw, TXCW, hw->txcw);
        E1000_WRITE_REG(hw, CTRL, (ctrl & ~E1000_CTRL_SLU));

        hw->serdes_link_down = FALSE;
    }
    /* If we force link for non-auto-negotiation switch, check link status
     * based on MAC synchronization for internal serdes media type.
     */
    else if((hw->media_type == e1000_media_type_internal_serdes) &&
            !(E1000_TXCW_ANE & E1000_READ_REG(hw, TXCW))) {
        /* SYNCH bit and IV bit are sticky. */
        udelay(10);
        if(E1000_RXCW_SYNCH & E1000_READ_REG(hw, RXCW)) {
            if(!(rxcw & E1000_RXCW_IV)) {
                hw->serdes_link_down = FALSE;
                DEBUGOUT("SERDES: Link is up.\n");
            }
        } else {
            hw->serdes_link_down = TRUE;
            DEBUGOUT("SERDES: Link is down.\n");
        }
    }
    if((hw->media_type == e1000_media_type_internal_serdes) &&
       (E1000_TXCW_ANE & E1000_READ_REG(hw, TXCW))) {
        hw->serdes_link_down = !(E1000_STATUS_LU & E1000_READ_REG(hw, STATUS));
    }
    return E1000_SUCCESS;
}

/******************************************************************************
 * Detects the current speed and duplex settings of the hardware.
 *
 * hw - Struct containing variables accessed by shared code
 * speed - Speed of the connection
 * duplex - Duplex setting of the connection
 *****************************************************************************/
int32_t
e1000_get_speed_and_duplex(struct e1000_hw *hw,
                           uint16_t *speed,
                           uint16_t *duplex)
{
    uint32_t status;
    int32_t ret_val;
    uint16_t phy_data;

    DEBUGFUNC("e1000_get_speed_and_duplex");

    if(hw->mac_type >= e1000_82543) {
        status = E1000_READ_REG(hw, STATUS);
        if(status & E1000_STATUS_SPEED_1000) {
            *speed = SPEED_1000;
            DEBUGOUT("1000 Mbs, ");
        } else if(status & E1000_STATUS_SPEED_100) {
            *speed = SPEED_100;
            DEBUGOUT("100 Mbs, ");
        } else {
            *speed = SPEED_10;
            DEBUGOUT("10 Mbs, ");
        }

        if(status & E1000_STATUS_FD) {
            *duplex = FULL_DUPLEX;
            DEBUGOUT("Full Duplex\r\n");
        } else {
            *duplex = HALF_DUPLEX;
            DEBUGOUT(" Half Duplex\r\n");
        }
    } else {
        DEBUGOUT("1000 Mbs, Full Duplex\r\n");
        *speed = SPEED_1000;
        *duplex = FULL_DUPLEX;
    }

    /* IGP01 PHY may advertise full duplex operation after speed downgrade even
     * if it is operating at half duplex.  Here we set the duplex settings to
     * match the duplex in the link partner's capabilities.
     */
    if(hw->phy_type == e1000_phy_igp && hw->speed_downgraded) {
        ret_val = e1000_read_phy_reg(hw, PHY_AUTONEG_EXP, &phy_data);
        if(ret_val)
            return ret_val;

        if(!(phy_data & NWAY_ER_LP_NWAY_CAPS))
            *duplex = HALF_DUPLEX;
        else {
            ret_val = e1000_read_phy_reg(hw, PHY_LP_ABILITY, &phy_data);
            if(ret_val)
                return ret_val;
            if((*speed == SPEED_100 && !(phy_data & NWAY_LPAR_100TX_FD_CAPS)) ||
               (*speed == SPEED_10 && !(phy_data & NWAY_LPAR_10T_FD_CAPS)))
                *duplex = HALF_DUPLEX;
        }
    }

    return E1000_SUCCESS;
}

/******************************************************************************
* Blocks until autoneg completes or times out (~4.5 seconds)
*
* hw - Struct containing variables accessed by shared code
******************************************************************************/
int32_t
e1000_wait_autoneg(struct e1000_hw *hw)
{
    int32_t ret_val;
    uint16_t i;
    uint16_t phy_data;

    DEBUGFUNC("e1000_wait_autoneg");
    DEBUGOUT("Waiting for Auto-Neg to complete.\n");

    /* We will wait for autoneg to complete or 4.5 seconds to expire. */
    for(i = PHY_AUTO_NEG_TIME; i > 0; i--) {
        /* Read the MII Status Register and wait for Auto-Neg
         * Complete bit to be set.
         */
        ret_val = e1000_read_phy_reg(hw, PHY_STATUS, &phy_data);
        if(ret_val)
            return ret_val;
        ret_val = e1000_read_phy_reg(hw, PHY_STATUS, &phy_data);
        if(ret_val)
            return ret_val;
        if(phy_data & MII_SR_AUTONEG_COMPLETE) {
            return E1000_SUCCESS;
        }
        msec_delay(100);
    }
    return E1000_SUCCESS;
}

/******************************************************************************
* Raises the Management Data Clock
*
* hw - Struct containing variables accessed by shared code
* ctrl - Device control register's current value
******************************************************************************/
static void
e1000_raise_mdi_clk(struct e1000_hw *hw,
                    uint32_t *ctrl)
{
    /* Raise the clock input to the Management Data Clock (by setting the MDC
     * bit), and then delay 10 microseconds.
     */
    E1000_WRITE_REG(hw, CTRL, (*ctrl | E1000_CTRL_MDC));
    E1000_WRITE_FLUSH(hw);
    udelay(10);
}

/******************************************************************************
* Lowers the Management Data Clock
*
* hw - Struct containing variables accessed by shared code
* ctrl - Device control register's current value
******************************************************************************/
static void
e1000_lower_mdi_clk(struct e1000_hw *hw,
                    uint32_t *ctrl)
{
    /* Lower the clock input to the Management Data Clock (by clearing the MDC
     * bit), and then delay 10 microseconds.
     */
    E1000_WRITE_REG(hw, CTRL, (*ctrl & ~E1000_CTRL_MDC));
    E1000_WRITE_FLUSH(hw);
    udelay(10);
}

/******************************************************************************
* Shifts data bits out to the PHY
*
* hw - Struct containing variables accessed by shared code
* data - Data to send out to the PHY
* count - Number of bits to shift out
*
* Bits are shifted out in MSB to LSB order.
******************************************************************************/
static void
e1000_shift_out_mdi_bits(struct e1000_hw *hw,
                         uint32_t data,
                         uint16_t count)
{
    uint32_t ctrl;
    uint32_t mask;

    /* We need to shift "count" number of bits out to the PHY. So, the value
     * in the "data" parameter will be shifted out to the PHY one bit at a
     * time. In order to do this, "data" must be broken down into bits.
     */
    mask = 0x01;
    mask <<= (count - 1);

    ctrl = E1000_READ_REG(hw, CTRL);

    /* Set MDIO_DIR and MDC_DIR direction bits to be used as output pins. */
    ctrl |= (E1000_CTRL_MDIO_DIR | E1000_CTRL_MDC_DIR);

    while(mask) {
        /* A "1" is shifted out to the PHY by setting the MDIO bit to "1" and
         * then raising and lowering the Management Data Clock. A "0" is
         * shifted out to the PHY by setting the MDIO bit to "0" and then
         * raising and lowering the clock.
         */
        if(data & mask) ctrl |= E1000_CTRL_MDIO;
        else ctrl &= ~E1000_CTRL_MDIO;

        E1000_WRITE_REG(hw, CTRL, ctrl);
        E1000_WRITE_FLUSH(hw);

        udelay(10);

        e1000_raise_mdi_clk(hw, &ctrl);
        e1000_lower_mdi_clk(hw, &ctrl);

        mask = mask >> 1;
    }
}

/******************************************************************************
* Shifts data bits in from the PHY
*
* hw - Struct containing variables accessed by shared code
*
* Bits are shifted in in MSB to LSB order.
******************************************************************************/
static uint16_t
e1000_shift_in_mdi_bits(struct e1000_hw *hw)
{
    uint32_t ctrl;
    uint16_t data = 0;
    uint8_t i;

    /* In order to read a register from the PHY, we need to shift in a total
     * of 18 bits from the PHY. The first two bit (turnaround) times are used
     * to avoid contention on the MDIO pin when a read operation is performed.
     * These two bits are ignored by us and thrown away. Bits are "shifted in"
     * by raising the input to the Management Data Clock (setting the MDC bit),
     * and then reading the value of the MDIO bit.
     */
    ctrl = E1000_READ_REG(hw, CTRL);

    /* Clear MDIO_DIR (SWDPIO1) to indicate this bit is to be used as input. */
    ctrl &= ~E1000_CTRL_MDIO_DIR;
    ctrl &= ~E1000_CTRL_MDIO;

    E1000_WRITE_REG(hw, CTRL, ctrl);
    E1000_WRITE_FLUSH(hw);

    /* Raise and Lower the clock before reading in the data. This accounts for
     * the turnaround bits. The first clock occurred when we clocked out the
     * last bit of the Register Address.
     */
    e1000_raise_mdi_clk(hw, &ctrl);
    e1000_lower_mdi_clk(hw, &ctrl);

    for(data = 0, i = 0; i < 16; i++) {
        data = data << 1;
        e1000_raise_mdi_clk(hw, &ctrl);
        ctrl = E1000_READ_REG(hw, CTRL);
        /* Check to see if we shifted in a "1". */
        if(ctrl & E1000_CTRL_MDIO) data |= 1;
        e1000_lower_mdi_clk(hw, &ctrl);
    }

    e1000_raise_mdi_clk(hw, &ctrl);
    e1000_lower_mdi_clk(hw, &ctrl);

    return data;
}

/*****************************************************************************
* Reads the value from a PHY register, if the value is on a specific non zero
* page, sets the page first.
* hw - Struct containing variables accessed by shared code
* reg_addr - address of the PHY register to read
******************************************************************************/
int32_t
e1000_read_phy_reg(struct e1000_hw *hw,
                   uint32_t reg_addr,
                   uint16_t *phy_data)
{
    uint32_t ret_val;

    DEBUGFUNC("e1000_read_phy_reg");

    if(hw->phy_type == e1000_phy_igp &&
       (reg_addr > MAX_PHY_MULTI_PAGE_REG)) {
        ret_val = e1000_write_phy_reg_ex(hw, IGP01E1000_PHY_PAGE_SELECT,
                                         (uint16_t)reg_addr);
        if(ret_val)
            return ret_val;
    }

    ret_val = e1000_read_phy_reg_ex(hw, IGP01E1000_PHY_PAGE_SELECT & reg_addr,
                                    phy_data);

    return ret_val;
}

int32_t
e1000_read_phy_reg_ex(struct e1000_hw *hw,
                      uint32_t reg_addr,
                      uint16_t *phy_data)
{
    uint32_t i;
    uint32_t mdic = 0;
    const uint32_t phy_addr = 1;

    DEBUGFUNC("e1000_read_phy_reg_ex");

    if(reg_addr > MAX_PHY_REG_ADDRESS) {
        DEBUGOUT1("PHY Address %d is out of range\n", reg_addr);
        return -E1000_ERR_PARAM;
    }

    if(hw->mac_type > e1000_82543) {
        /* Set up Op-code, Phy Address, and register address in the MDI
         * Control register.  The MAC will take care of interfacing with the
         * PHY to retrieve the desired data.
         */
        mdic = ((reg_addr << E1000_MDIC_REG_SHIFT) |
                (phy_addr << E1000_MDIC_PHY_SHIFT) |
                (E1000_MDIC_OP_READ));

        E1000_WRITE_REG(hw, MDIC, mdic);

        /* Poll the ready bit to see if the MDI read completed */
        for(i = 0; i < 64; i++) {
            udelay(50);
            mdic = E1000_READ_REG(hw, MDIC);
            if(mdic & E1000_MDIC_READY) break;
        }
        if(!(mdic & E1000_MDIC_READY)) {
            DEBUGOUT("MDI Read did not complete\n");
            return -E1000_ERR_PHY;
        }
        if(mdic & E1000_MDIC_ERROR) {
            DEBUGOUT("MDI Error\n");
            return -E1000_ERR_PHY;
        }
        *phy_data = (uint16_t) mdic;
    } else {
        /* We must first send a preamble through the MDIO pin to signal the
         * beginning of an MII instruction.  This is done by sending 32
         * consecutive "1" bits.
         */
        e1000_shift_out_mdi_bits(hw, PHY_PREAMBLE, PHY_PREAMBLE_SIZE);

        /* Now combine the next few fields that are required for a read
         * operation.  We use this method instead of calling the
         * e1000_shift_out_mdi_bits routine five different times. The format of
         * a MII read instruction consists of a shift out of 14 bits and is
         * defined as follows:
         *    <Preamble><SOF><Op Code><Phy Addr><Reg Addr>
         * followed by a shift in of 18 bits.  This first two bits shifted in
         * are TurnAround bits used to avoid contention on the MDIO pin when a
         * READ operation is performed.  These two bits are thrown away
         * followed by a shift in of 16 bits which contains the desired data.
         */
        mdic = ((reg_addr) | (phy_addr << 5) |
                (PHY_OP_READ << 10) | (PHY_SOF << 12));

        e1000_shift_out_mdi_bits(hw, mdic, 14);

        /* Now that we've shifted out the read command to the MII, we need to
         * "shift in" the 16-bit value (18 total bits) of the requested PHY
         * register address.
         */
        *phy_data = e1000_shift_in_mdi_bits(hw);
    }
    return E1000_SUCCESS;
}

/******************************************************************************
* Writes a value to a PHY register
*
* hw - Struct containing variables accessed by shared code
* reg_addr - address of the PHY register to write
* data - data to write to the PHY
******************************************************************************/
int32_t
e1000_write_phy_reg(struct e1000_hw *hw,
                    uint32_t reg_addr,
                    uint16_t phy_data)
{
    uint32_t ret_val;

    DEBUGFUNC("e1000_write_phy_reg");

    if(hw->phy_type == e1000_phy_igp &&
       (reg_addr > MAX_PHY_MULTI_PAGE_REG)) {
        ret_val = e1000_write_phy_reg_ex(hw, IGP01E1000_PHY_PAGE_SELECT,
                                         (uint16_t)reg_addr);
        if(ret_val)
            return ret_val;
    }

    ret_val = e1000_write_phy_reg_ex(hw, IGP01E1000_PHY_PAGE_SELECT & reg_addr,
                                     phy_data);

    return ret_val;
}

int32_t
e1000_write_phy_reg_ex(struct e1000_hw *hw,
                    uint32_t reg_addr,
                    uint16_t phy_data)
{
    uint32_t i;
    uint32_t mdic = 0;
    const uint32_t phy_addr = 1;

    DEBUGFUNC("e1000_write_phy_reg_ex");

    if(reg_addr > MAX_PHY_REG_ADDRESS) {
        DEBUGOUT1("PHY Address %d is out of range\n", reg_addr);
        return -E1000_ERR_PARAM;
    }

    if(hw->mac_type > e1000_82543) {
        /* Set up Op-code, Phy Address, register address, and data intended
         * for the PHY register in the MDI Control register.  The MAC will take
         * care of interfacing with the PHY to send the desired data.
         */
        mdic = (((uint32_t) phy_data) |
                (reg_addr << E1000_MDIC_REG_SHIFT) |
                (phy_addr << E1000_MDIC_PHY_SHIFT) |
                (E1000_MDIC_OP_WRITE));

        E1000_WRITE_REG(hw, MDIC, mdic);

        /* Poll the ready bit to see if the MDI read completed */
        for(i = 0; i < 640; i++) {
            udelay(5);
            mdic = E1000_READ_REG(hw, MDIC);
            if(mdic & E1000_MDIC_READY) break;
        }
        if(!(mdic & E1000_MDIC_READY)) {
            DEBUGOUT("MDI Write did not complete\n");
            return -E1000_ERR_PHY;
        }
    } else {
        /* We'll need to use the SW defined pins to shift the write command
         * out to the PHY. We first send a preamble to the PHY to signal the
         * beginning of the MII instruction.  This is done by sending 32
         * consecutive "1" bits.
         */
        e1000_shift_out_mdi_bits(hw, PHY_PREAMBLE, PHY_PREAMBLE_SIZE);

        /* Now combine the remaining required fields that will indicate a
         * write operation. We use this method instead of calling the
         * e1000_shift_out_mdi_bits routine for each field in the command. The
         * format of a MII write instruction is as follows:
         * <Preamble><SOF><Op Code><Phy Addr><Reg Addr><Turnaround><Data>.
         */
        mdic = ((PHY_TURNAROUND) | (reg_addr << 2) | (phy_addr << 7) |
                (PHY_OP_WRITE << 12) | (PHY_SOF << 14));
        mdic <<= 16;
        mdic |= (uint32_t) phy_data;

        e1000_shift_out_mdi_bits(hw, mdic, 32);
    }

    return E1000_SUCCESS;
}

/******************************************************************************
* Returns the PHY to the power-on reset state
*
* hw - Struct containing variables accessed by shared code
******************************************************************************/
void
e1000_phy_hw_reset(struct e1000_hw *hw)
{
    uint32_t ctrl, ctrl_ext;
    uint32_t led_ctrl;

    DEBUGFUNC("e1000_phy_hw_reset");

    DEBUGOUT("Resetting Phy...\n");

    if(hw->mac_type > e1000_82543) {
        /* Read the device control register and assert the E1000_CTRL_PHY_RST
         * bit. Then, take it out of reset.
         */
        ctrl = E1000_READ_REG(hw, CTRL);
        E1000_WRITE_REG(hw, CTRL, ctrl | E1000_CTRL_PHY_RST);
        E1000_WRITE_FLUSH(hw);
        msec_delay(10);
        E1000_WRITE_REG(hw, CTRL, ctrl);
        E1000_WRITE_FLUSH(hw);
    } else {
        /* Read the Extended Device Control Register, assert the PHY_RESET_DIR
         * bit to put the PHY into reset. Then, take it out of reset.
         */
        ctrl_ext = E1000_READ_REG(hw, CTRL_EXT);
        ctrl_ext |= E1000_CTRL_EXT_SDP4_DIR;
        ctrl_ext &= ~E1000_CTRL_EXT_SDP4_DATA;
        E1000_WRITE_REG(hw, CTRL_EXT, ctrl_ext);
        E1000_WRITE_FLUSH(hw);
        msec_delay(10);
        ctrl_ext |= E1000_CTRL_EXT_SDP4_DATA;
        E1000_WRITE_REG(hw, CTRL_EXT, ctrl_ext);
        E1000_WRITE_FLUSH(hw);
    }
    udelay(150);

    if((hw->mac_type == e1000_82541) || (hw->mac_type == e1000_82547)) {
        /* Configure activity LED after PHY reset */
        led_ctrl = E1000_READ_REG(hw, LEDCTL);
        led_ctrl &= IGP_ACTIVITY_LED_MASK;
        led_ctrl |= (IGP_ACTIVITY_LED_ENABLE | IGP_LED3_MODE);
        E1000_WRITE_REG(hw, LEDCTL, led_ctrl);
    }
}

/******************************************************************************
* Resets the PHY
*
* hw - Struct containing variables accessed by shared code
*
* Sets bit 15 of the MII Control regiser
******************************************************************************/
int32_t
e1000_phy_reset(struct e1000_hw *hw)
{
    int32_t ret_val;
    uint16_t phy_data;

    DEBUGFUNC("e1000_phy_reset");

    if(hw->mac_type != e1000_82541_rev_2) {
        ret_val = e1000_read_phy_reg(hw, PHY_CTRL, &phy_data);
        if(ret_val)
            return ret_val;

        phy_data |= MII_CR_RESET;
        ret_val = e1000_write_phy_reg(hw, PHY_CTRL, phy_data);
        if(ret_val)
            return ret_val;

        udelay(1);
    } else e1000_phy_hw_reset(hw);

    if(hw->phy_type == e1000_phy_igp)
        e1000_phy_init_script(hw);

    return E1000_SUCCESS;
}

/******************************************************************************
* Probes the expected PHY address for known PHY IDs
*
* hw - Struct containing variables accessed by shared code
******************************************************************************/
int32_t
e1000_detect_gig_phy(struct e1000_hw *hw)
{
    int32_t phy_init_status, ret_val;
    uint16_t phy_id_high, phy_id_low;
    boolean_t match = FALSE;

    DEBUGFUNC("e1000_detect_gig_phy");

    /* Read the PHY ID Registers to identify which PHY is onboard. */
    ret_val = e1000_read_phy_reg(hw, PHY_ID1, &phy_id_high);
    if(ret_val)
        return ret_val;

    hw->phy_id = (uint32_t) (phy_id_high << 16);
    udelay(20);
    ret_val = e1000_read_phy_reg(hw, PHY_ID2, &phy_id_low);
    if(ret_val)
        return ret_val;

    hw->phy_id |= (uint32_t) (phy_id_low & PHY_REVISION_MASK);
    hw->phy_revision = (uint32_t) phy_id_low & ~PHY_REVISION_MASK;

    switch(hw->mac_type) {
    case e1000_82543:
        if(hw->phy_id == M88E1000_E_PHY_ID) match = TRUE;
        break;
    case e1000_82544:
        if(hw->phy_id == M88E1000_I_PHY_ID) match = TRUE;
        break;
    case e1000_82540:
    case e1000_82545:
    case e1000_82545_rev_3:
    case e1000_82546:
    case e1000_82546_rev_3:
        if(hw->phy_id == M88E1011_I_PHY_ID) match = TRUE;
        break;
    case e1000_82541:
    case e1000_82541_rev_2:
    case e1000_82547:
    case e1000_82547_rev_2:
        if(hw->phy_id == IGP01E1000_I_PHY_ID) match = TRUE;
        break;
    default:
        DEBUGOUT1("Invalid MAC type %d\n", hw->mac_type);
        return -E1000_ERR_CONFIG;
    }
    phy_init_status = e1000_set_phy_type(hw);

    if ((match) && (phy_init_status == E1000_SUCCESS)) {
        DEBUGOUT1("PHY ID 0x%X detected\n", hw->phy_id);
        return E1000_SUCCESS;
    }
    DEBUGOUT1("Invalid PHY ID 0x%X\n", hw->phy_id);
    return -E1000_ERR_PHY;
}

/******************************************************************************
* Resets the PHY's DSP
*
* hw - Struct containing variables accessed by shared code
******************************************************************************/
static int32_t
e1000_phy_reset_dsp(struct e1000_hw *hw)
{
    int32_t ret_val;
    DEBUGFUNC("e1000_phy_reset_dsp");

    do {
        ret_val = e1000_write_phy_reg(hw, 29, 0x001d);
        if(ret_val) break;
        ret_val = e1000_write_phy_reg(hw, 30, 0x00c1);
        if(ret_val) break;
        ret_val = e1000_write_phy_reg(hw, 30, 0x0000);
        if(ret_val) break;
        ret_val = E1000_SUCCESS;
    } while(0);

    return ret_val;
}

/******************************************************************************
* Get PHY information from various PHY registers for igp PHY only.
*
* hw - Struct containing variables accessed by shared code
* phy_info - PHY information structure
******************************************************************************/
int32_t
e1000_phy_igp_get_info(struct e1000_hw *hw,
                       struct e1000_phy_info *phy_info)
{
    int32_t ret_val;
    uint16_t phy_data, polarity, min_length, max_length, average;

    DEBUGFUNC("e1000_phy_igp_get_info");

    /* The downshift status is checked only once, after link is established,
     * and it stored in the hw->speed_downgraded parameter. */
    phy_info->downshift = hw->speed_downgraded;

    /* IGP01E1000 does not need to support it. */
    phy_info->extended_10bt_distance = e1000_10bt_ext_dist_enable_normal;

    /* IGP01E1000 always correct polarity reversal */
    phy_info->polarity_correction = e1000_polarity_reversal_enabled;

    /* Check polarity status */
    ret_val = e1000_check_polarity(hw, &polarity);
    if(ret_val)
        return ret_val;

    phy_info->cable_polarity = polarity;

    ret_val = e1000_read_phy_reg(hw, IGP01E1000_PHY_PORT_STATUS, &phy_data);
    if(ret_val)
        return ret_val;

    phy_info->mdix_mode = (phy_data & IGP01E1000_PSSR_MDIX) >>
                          IGP01E1000_PSSR_MDIX_SHIFT;

    if((phy_data & IGP01E1000_PSSR_SPEED_MASK) ==
       IGP01E1000_PSSR_SPEED_1000MBPS) {
        /* Local/Remote Receiver Information are only valid at 1000 Mbps */
        ret_val = e1000_read_phy_reg(hw, PHY_1000T_STATUS, &phy_data);
        if(ret_val)
            return ret_val;

        phy_info->local_rx = (phy_data & SR_1000T_LOCAL_RX_STATUS) >>
                             SR_1000T_LOCAL_RX_STATUS_SHIFT;
        phy_info->remote_rx = (phy_data & SR_1000T_REMOTE_RX_STATUS) >>
                              SR_1000T_REMOTE_RX_STATUS_SHIFT;

        /* Get cable length */
        ret_val = e1000_get_cable_length(hw, &min_length, &max_length);
        if(ret_val)
            return ret_val;

        /* transalte to old method */
        average = (max_length + min_length) / 2;

        if(average <= e1000_igp_cable_length_50)
            phy_info->cable_length = e1000_cable_length_50;
        else if(average <= e1000_igp_cable_length_80)
            phy_info->cable_length = e1000_cable_length_50_80;
        else if(average <= e1000_igp_cable_length_110)
            phy_info->cable_length = e1000_cable_length_80_110;
        else if(average <= e1000_igp_cable_length_140)
            phy_info->cable_length = e1000_cable_length_110_140;
        else
            phy_info->cable_length = e1000_cable_length_140;
    }

    return E1000_SUCCESS;
}

/******************************************************************************
* Get PHY information from various PHY registers fot m88 PHY only.
*
* hw - Struct containing variables accessed by shared code
* phy_info - PHY information structure
******************************************************************************/
int32_t
e1000_phy_m88_get_info(struct e1000_hw *hw,
                       struct e1000_phy_info *phy_info)
{
    int32_t ret_val;
    uint16_t phy_data, polarity;

    DEBUGFUNC("e1000_phy_m88_get_info");

    /* The downshift status is checked only once, after link is established,
     * and it stored in the hw->speed_downgraded parameter. */
    phy_info->downshift = hw->speed_downgraded;

    ret_val = e1000_read_phy_reg(hw, M88E1000_PHY_SPEC_CTRL, &phy_data);
    if(ret_val)
        return ret_val;

    phy_info->extended_10bt_distance =
        (phy_data & M88E1000_PSCR_10BT_EXT_DIST_ENABLE) >>
        M88E1000_PSCR_10BT_EXT_DIST_ENABLE_SHIFT;
    phy_info->polarity_correction =
        (phy_data & M88E1000_PSCR_POLARITY_REVERSAL) >>
        M88E1000_PSCR_POLARITY_REVERSAL_SHIFT;

    /* Check polarity status */
    ret_val = e1000_check_polarity(hw, &polarity);
    if(ret_val)
        return ret_val;

    phy_info->cable_polarity = polarity;

    ret_val = e1000_read_phy_reg(hw, M88E1000_PHY_SPEC_STATUS, &phy_data);
    if(ret_val)
        return ret_val;

    phy_info->mdix_mode = (phy_data & M88E1000_PSSR_MDIX) >>
                          M88E1000_PSSR_MDIX_SHIFT;

    if(phy_data & M88E1000_PSSR_1000MBS) {
        /* Cable Length Estimation and Local/Remote Receiver Informatoion
         * are only valid at 1000 Mbps
         */
        phy_info->cable_length = ((phy_data & M88E1000_PSSR_CABLE_LENGTH) >>
                                  M88E1000_PSSR_CABLE_LENGTH_SHIFT);

        ret_val = e1000_read_phy_reg(hw, PHY_1000T_STATUS, &phy_data);
        if(ret_val)
            return ret_val;

        phy_info->local_rx = (phy_data & SR_1000T_LOCAL_RX_STATUS) >>
                             SR_1000T_LOCAL_RX_STATUS_SHIFT;

        phy_info->remote_rx = (phy_data & SR_1000T_REMOTE_RX_STATUS) >>
                              SR_1000T_REMOTE_RX_STATUS_SHIFT;
    }

    return E1000_SUCCESS;
}

/******************************************************************************
* Get PHY information from various PHY registers
*
* hw - Struct containing variables accessed by shared code
* phy_info - PHY information structure
******************************************************************************/
int32_t
e1000_phy_get_info(struct e1000_hw *hw,
                   struct e1000_phy_info *phy_info)
{
    int32_t ret_val;
    uint16_t phy_data;

    DEBUGFUNC("e1000_phy_get_info");

    phy_info->cable_length = e1000_cable_length_undefined;
    phy_info->extended_10bt_distance = e1000_10bt_ext_dist_enable_undefined;
    phy_info->cable_polarity = e1000_rev_polarity_undefined;
    phy_info->downshift = e1000_downshift_undefined;
    phy_info->polarity_correction = e1000_polarity_reversal_undefined;
    phy_info->mdix_mode = e1000_auto_x_mode_undefined;
    phy_info->local_rx = e1000_1000t_rx_status_undefined;
    phy_info->remote_rx = e1000_1000t_rx_status_undefined;

    if(hw->media_type != e1000_media_type_copper) {
        DEBUGOUT("PHY info is only valid for copper media\n");
        return -E1000_ERR_CONFIG;
    }

    ret_val = e1000_read_phy_reg(hw, PHY_STATUS, &phy_data);
    if(ret_val)
        return ret_val;

    ret_val = e1000_read_phy_reg(hw, PHY_STATUS, &phy_data);
    if(ret_val)
        return ret_val;

    if((phy_data & MII_SR_LINK_STATUS) != MII_SR_LINK_STATUS) {
        DEBUGOUT("PHY info is only valid if link is up\n");
        return -E1000_ERR_CONFIG;
    }

    if(hw->phy_type == e1000_phy_igp)
        return e1000_phy_igp_get_info(hw, phy_info);
    else
        return e1000_phy_m88_get_info(hw, phy_info);
}

int32_t
e1000_validate_mdi_setting(struct e1000_hw *hw)
{
    DEBUGFUNC("e1000_validate_mdi_settings");

    if(!hw->autoneg && (hw->mdix == 0 || hw->mdix == 3)) {
        DEBUGOUT("Invalid MDI setting detected\n");
        hw->mdix = 1;
        return -E1000_ERR_CONFIG;
    }
    return E1000_SUCCESS;
}


/******************************************************************************
 * Sets up eeprom variables in the hw struct.  Must be called after mac_type
 * is configured.
 *
 * hw - Struct containing variables accessed by shared code
 *****************************************************************************/
void
e1000_init_eeprom_params(struct e1000_hw *hw)
{
    struct e1000_eeprom_info *eeprom = &hw->eeprom;
    uint32_t eecd = E1000_READ_REG(hw, EECD);
    uint16_t eeprom_size;

    DEBUGFUNC("e1000_init_eeprom_params");

    switch (hw->mac_type) {
    case e1000_82542_rev2_0:
    case e1000_82542_rev2_1:
    case e1000_82543:
    case e1000_82544:
        eeprom->type = e1000_eeprom_microwire;
        eeprom->word_size = 64;
        eeprom->opcode_bits = 3;
        eeprom->address_bits = 6;
        eeprom->delay_usec = 50;
        break;
    case e1000_82540:
    case e1000_82545:
    case e1000_82545_rev_3:
    case e1000_82546:
    case e1000_82546_rev_3:
        eeprom->type = e1000_eeprom_microwire;
        eeprom->opcode_bits = 3;
        eeprom->delay_usec = 50;
        if(eecd & E1000_EECD_SIZE) {
            eeprom->word_size = 256;
            eeprom->address_bits = 8;
        } else {
            eeprom->word_size = 64;
            eeprom->address_bits = 6;
        }
        break;
    case e1000_82541:
    case e1000_82541_rev_2:
    case e1000_82547:
    case e1000_82547_rev_2:
        if (eecd & E1000_EECD_TYPE) {
            eeprom->type = e1000_eeprom_spi;
            eeprom->opcode_bits = 8;
            eeprom->delay_usec = 1;
            if (eecd & E1000_EECD_ADDR_BITS) {
                eeprom->page_size = 32;
                eeprom->address_bits = 16;
            } else {
                eeprom->page_size = 8;
                eeprom->address_bits = 8;
            }
        } else {
            eeprom->type = e1000_eeprom_microwire;
            eeprom->opcode_bits = 3;
            eeprom->delay_usec = 50;
            if (eecd & E1000_EECD_ADDR_BITS) {
                eeprom->word_size = 256;
                eeprom->address_bits = 8;
            } else {
                eeprom->word_size = 64;
                eeprom->address_bits = 6;
            }
        }
        break;
    default:
        eeprom->type = e1000_eeprom_spi;
        eeprom->opcode_bits = 8;
        eeprom->delay_usec = 1;
        if (eecd & E1000_EECD_ADDR_BITS) {
            eeprom->page_size = 32;
            eeprom->address_bits = 16;
        } else {
            eeprom->page_size = 8;
            eeprom->address_bits = 8;
        }
        break;
    }

    if (eeprom->type == e1000_eeprom_spi) {
        eeprom->word_size = 64;
        if (e1000_read_eeprom(hw, EEPROM_CFG, 1, &eeprom_size) == 0) {
            eeprom_size &= EEPROM_SIZE_MASK;

            switch (eeprom_size) {
            case EEPROM_SIZE_16KB:
                eeprom->word_size = 8192;
                break;
            case EEPROM_SIZE_8KB:
                eeprom->word_size = 4096;
                break;
            case EEPROM_SIZE_4KB:
                eeprom->word_size = 2048;
                break;
            case EEPROM_SIZE_2KB:
                eeprom->word_size = 1024;
                break;
            case EEPROM_SIZE_1KB:
                eeprom->word_size = 512;
                break;
            case EEPROM_SIZE_512B:
                eeprom->word_size = 256;
                break;
            case EEPROM_SIZE_128B:
            default:
                eeprom->word_size = 64;
                break;
            }
        }
    }
}

/******************************************************************************
 * Raises the EEPROM's clock input.
 *
 * hw - Struct containing variables accessed by shared code
 * eecd - EECD's current value
 *****************************************************************************/
static void
e1000_raise_ee_clk(struct e1000_hw *hw,
                   uint32_t *eecd)
{
    /* Raise the clock input to the EEPROM (by setting the SK bit), and then
     * wait <delay> microseconds.
     */
    *eecd = *eecd | E1000_EECD_SK;
    E1000_WRITE_REG(hw, EECD, *eecd);
    E1000_WRITE_FLUSH(hw);
    udelay(hw->eeprom.delay_usec);
}

/******************************************************************************
 * Lowers the EEPROM's clock input.
 *
 * hw - Struct containing variables accessed by shared code
 * eecd - EECD's current value
 *****************************************************************************/
static void
e1000_lower_ee_clk(struct e1000_hw *hw,
                   uint32_t *eecd)
{
    /* Lower the clock input to the EEPROM (by clearing the SK bit), and then
     * wait 50 microseconds.
     */
    *eecd = *eecd & ~E1000_EECD_SK;
    E1000_WRITE_REG(hw, EECD, *eecd);
    E1000_WRITE_FLUSH(hw);
    udelay(hw->eeprom.delay_usec);
}

/******************************************************************************
 * Shift data bits out to the EEPROM.
 *
 * hw - Struct containing variables accessed by shared code
 * data - data to send to the EEPROM
 * count - number of bits to shift out
 *****************************************************************************/
static void
e1000_shift_out_ee_bits(struct e1000_hw *hw,
                        uint16_t data,
                        uint16_t count)
{
    struct e1000_eeprom_info *eeprom = &hw->eeprom;
    uint32_t eecd;
    uint32_t mask;

    /* We need to shift "count" bits out to the EEPROM. So, value in the
     * "data" parameter will be shifted out to the EEPROM one bit at a time.
     * In order to do this, "data" must be broken down into bits.
     */
    mask = 0x01 << (count - 1);
    eecd = E1000_READ_REG(hw, EECD);
    if (eeprom->type == e1000_eeprom_microwire) {
        eecd &= ~E1000_EECD_DO;
    } else if (eeprom->type == e1000_eeprom_spi) {
        eecd |= E1000_EECD_DO;
    }
    do {
        /* A "1" is shifted out to the EEPROM by setting bit "DI" to a "1",
         * and then raising and then lowering the clock (the SK bit controls
         * the clock input to the EEPROM).  A "0" is shifted out to the EEPROM
         * by setting "DI" to "0" and then raising and then lowering the clock.
         */
        eecd &= ~E1000_EECD_DI;

        if(data & mask)
            eecd |= E1000_EECD_DI;

        E1000_WRITE_REG(hw, EECD, eecd);
        E1000_WRITE_FLUSH(hw);

        udelay(eeprom->delay_usec);

        e1000_raise_ee_clk(hw, &eecd);
        e1000_lower_ee_clk(hw, &eecd);

        mask = mask >> 1;

    } while(mask);

    /* We leave the "DI" bit set to "0" when we leave this routine. */
    eecd &= ~E1000_EECD_DI;
    E1000_WRITE_REG(hw, EECD, eecd);
}

/******************************************************************************
 * Shift data bits in from the EEPROM
 *
 * hw - Struct containing variables accessed by shared code
 *****************************************************************************/
static uint16_t
e1000_shift_in_ee_bits(struct e1000_hw *hw,
                       uint16_t count)
{
    uint32_t eecd;
    uint32_t i;
    uint16_t data;

    /* In order to read a register from the EEPROM, we need to shift 'count'
     * bits in from the EEPROM. Bits are "shifted in" by raising the clock
     * input to the EEPROM (setting the SK bit), and then reading the value of
     * the "DO" bit.  During this "shifting in" process the "DI" bit should
     * always be clear.
     */

    eecd = E1000_READ_REG(hw, EECD);

    eecd &= ~(E1000_EECD_DO | E1000_EECD_DI);
    data = 0;

    for(i = 0; i < count; i++) {
        data = data << 1;
        e1000_raise_ee_clk(hw, &eecd);

        eecd = E1000_READ_REG(hw, EECD);

        eecd &= ~(E1000_EECD_DI);
        if(eecd & E1000_EECD_DO)
            data |= 1;

        e1000_lower_ee_clk(hw, &eecd);
    }

    return data;
}

/******************************************************************************
 * Prepares EEPROM for access
 *
 * hw - Struct containing variables accessed by shared code
 *
 * Lowers EEPROM clock. Clears input pin. Sets the chip select pin. This
 * function should be called before issuing a command to the EEPROM.
 *****************************************************************************/
static int32_t
e1000_acquire_eeprom(struct e1000_hw *hw)
{
    struct e1000_eeprom_info *eeprom = &hw->eeprom;
    uint32_t eecd, i=0;

    DEBUGFUNC("e1000_acquire_eeprom");

    eecd = E1000_READ_REG(hw, EECD);

    /* Request EEPROM Access */
    if(hw->mac_type > e1000_82544) {
        eecd |= E1000_EECD_REQ;
        E1000_WRITE_REG(hw, EECD, eecd);
        eecd = E1000_READ_REG(hw, EECD);
        while((!(eecd & E1000_EECD_GNT)) &&
              (i < E1000_EEPROM_GRANT_ATTEMPTS)) {
            i++;
            udelay(5);
            eecd = E1000_READ_REG(hw, EECD);
        }
        if(!(eecd & E1000_EECD_GNT)) {
            eecd &= ~E1000_EECD_REQ;
            E1000_WRITE_REG(hw, EECD, eecd);
            DEBUGOUT("Could not acquire EEPROM grant\n");
            return -E1000_ERR_EEPROM;
        }
    }

    /* Setup EEPROM for Read/Write */

    if (eeprom->type == e1000_eeprom_microwire) {
        /* Clear SK and DI */
        eecd &= ~(E1000_EECD_DI | E1000_EECD_SK);
        E1000_WRITE_REG(hw, EECD, eecd);

        /* Set CS */
        eecd |= E1000_EECD_CS;
        E1000_WRITE_REG(hw, EECD, eecd);
    } else if (eeprom->type == e1000_eeprom_spi) {
        /* Clear SK and CS */
        eecd &= ~(E1000_EECD_CS | E1000_EECD_SK);
        E1000_WRITE_REG(hw, EECD, eecd);
        udelay(1);
    }

    return E1000_SUCCESS;
}

/******************************************************************************
 * Returns EEPROM to a "standby" state
 *
 * hw - Struct containing variables accessed by shared code
 *****************************************************************************/
static void
e1000_standby_eeprom(struct e1000_hw *hw)
{
    struct e1000_eeprom_info *eeprom = &hw->eeprom;
    uint32_t eecd;

    eecd = E1000_READ_REG(hw, EECD);

    if(eeprom->type == e1000_eeprom_microwire) {
        eecd &= ~(E1000_EECD_CS | E1000_EECD_SK);
        E1000_WRITE_REG(hw, EECD, eecd);
        E1000_WRITE_FLUSH(hw);
        udelay(eeprom->delay_usec);

        /* Clock high */
        eecd |= E1000_EECD_SK;
        E1000_WRITE_REG(hw, EECD, eecd);
        E1000_WRITE_FLUSH(hw);
        udelay(eeprom->delay_usec);

        /* Select EEPROM */
        eecd |= E1000_EECD_CS;
        E1000_WRITE_REG(hw, EECD, eecd);
        E1000_WRITE_FLUSH(hw);
        udelay(eeprom->delay_usec);

        /* Clock low */
        eecd &= ~E1000_EECD_SK;
        E1000_WRITE_REG(hw, EECD, eecd);
        E1000_WRITE_FLUSH(hw);
        udelay(eeprom->delay_usec);
    } else if(eeprom->type == e1000_eeprom_spi) {
        /* Toggle CS to flush commands */
        eecd |= E1000_EECD_CS;
        E1000_WRITE_REG(hw, EECD, eecd);
        E1000_WRITE_FLUSH(hw);
        udelay(eeprom->delay_usec);
        eecd &= ~E1000_EECD_CS;
        E1000_WRITE_REG(hw, EECD, eecd);
        E1000_WRITE_FLUSH(hw);
        udelay(eeprom->delay_usec);
    }
}

/******************************************************************************
 * Terminates a command by inverting the EEPROM's chip select pin
 *
 * hw - Struct containing variables accessed by shared code
 *****************************************************************************/
static void
e1000_release_eeprom(struct e1000_hw *hw)
{
    uint32_t eecd;

    DEBUGFUNC("e1000_release_eeprom");

    eecd = E1000_READ_REG(hw, EECD);

    if (hw->eeprom.type == e1000_eeprom_spi) {
        eecd |= E1000_EECD_CS;  /* Pull CS high */
        eecd &= ~E1000_EECD_SK; /* Lower SCK */

        E1000_WRITE_REG(hw, EECD, eecd);

        udelay(hw->eeprom.delay_usec);
    } else if(hw->eeprom.type == e1000_eeprom_microwire) {
        /* cleanup eeprom */

        /* CS on Microwire is active-high */
        eecd &= ~(E1000_EECD_CS | E1000_EECD_DI);

        E1000_WRITE_REG(hw, EECD, eecd);

        /* Rising edge of clock */
        eecd |= E1000_EECD_SK;
        E1000_WRITE_REG(hw, EECD, eecd);
        E1000_WRITE_FLUSH(hw);
        udelay(hw->eeprom.delay_usec);

        /* Falling edge of clock */
        eecd &= ~E1000_EECD_SK;
        E1000_WRITE_REG(hw, EECD, eecd);
        E1000_WRITE_FLUSH(hw);
        udelay(hw->eeprom.delay_usec);
    }

    /* Stop requesting EEPROM access */
    if(hw->mac_type > e1000_82544) {
        eecd &= ~E1000_EECD_REQ;
        E1000_WRITE_REG(hw, EECD, eecd);
    }
}

/******************************************************************************
 * Reads a 16 bit word from the EEPROM.
 *
 * hw - Struct containing variables accessed by shared code
 *****************************************************************************/
int32_t
e1000_spi_eeprom_ready(struct e1000_hw *hw)
{
    uint16_t retry_count = 0;
    uint8_t spi_stat_reg;

    DEBUGFUNC("e1000_spi_eeprom_ready");

    /* Read "Status Register" repeatedly until the LSB is cleared.  The
     * EEPROM will signal that the command has been completed by clearing
     * bit 0 of the internal status register.  If it's not cleared within
     * 5 milliseconds, then error out.
     */
    retry_count = 0;
    do {
        e1000_shift_out_ee_bits(hw, EEPROM_RDSR_OPCODE_SPI,
                                hw->eeprom.opcode_bits);
        spi_stat_reg = (uint8_t)e1000_shift_in_ee_bits(hw, 8);
        if (!(spi_stat_reg & EEPROM_STATUS_RDY_SPI))
            break;

        udelay(5);
        retry_count += 5;

        e1000_standby_eeprom(hw);
    } while(retry_count < EEPROM_MAX_RETRY_SPI);

    /* ATMEL SPI write time could vary from 0-20mSec on 3.3V devices (and
     * only 0-5mSec on 5V devices)
     */
    if(retry_count >= EEPROM_MAX_RETRY_SPI) {
        DEBUGOUT("SPI EEPROM Status error\n");
        return -E1000_ERR_EEPROM;
    }

    return E1000_SUCCESS;
}

/******************************************************************************
 * Reads a 16 bit word from the EEPROM.
 *
 * hw - Struct containing variables accessed by shared code
 * offset - offset of  word in the EEPROM to read
 * data - word read from the EEPROM
 * words - number of words to read
 *****************************************************************************/
int32_t
e1000_read_eeprom(struct e1000_hw *hw,
                  uint16_t offset,
                  uint16_t words,
                  uint16_t *data)
{
    struct e1000_eeprom_info *eeprom = &hw->eeprom;
    uint32_t i = 0;

    DEBUGFUNC("e1000_read_eeprom");

    /* A check for invalid values:  offset too large, too many words, and not
     * enough words.
     */
    if((offset > eeprom->word_size) || (words > eeprom->word_size - offset) ||
       (words == 0)) {
        DEBUGOUT("\"words\" parameter out of bounds\n");
        return -E1000_ERR_EEPROM;
    }

    /* Prepare the EEPROM for reading  */
    if(e1000_acquire_eeprom(hw) != E1000_SUCCESS)
        return -E1000_ERR_EEPROM;

    if(eeprom->type == e1000_eeprom_spi) {
        uint16_t word_in;
        uint8_t read_opcode = EEPROM_READ_OPCODE_SPI;

        if(e1000_spi_eeprom_ready(hw)) {
            e1000_release_eeprom(hw);
            return -E1000_ERR_EEPROM;
        }

        e1000_standby_eeprom(hw);

        /* Some SPI eeproms use the 8th address bit embedded in the opcode */
        if((eeprom->address_bits == 8) && (offset >= 128))
            read_opcode |= EEPROM_A8_OPCODE_SPI;

        /* Send the READ command (opcode + addr)  */
        e1000_shift_out_ee_bits(hw, read_opcode, eeprom->opcode_bits);
        e1000_shift_out_ee_bits(hw, (uint16_t)(offset*2), eeprom->address_bits);

        /* Read the data.  The address of the eeprom internally increments with
         * each byte (spi) being read, saving on the overhead of eeprom setup
         * and tear-down.  The address counter will roll over if reading beyond
         * the size of the eeprom, thus allowing the entire memory to be read
         * starting from any offset. */
        for (i = 0; i < words; i++) {
            word_in = e1000_shift_in_ee_bits(hw, 16);
            data[i] = (word_in >> 8) | (word_in << 8);
        }
    } else if(eeprom->type == e1000_eeprom_microwire) {
        for (i = 0; i < words; i++) {
            /* Send the READ command (opcode + addr)  */
            e1000_shift_out_ee_bits(hw, EEPROM_READ_OPCODE_MICROWIRE,
                                    eeprom->opcode_bits);
            e1000_shift_out_ee_bits(hw, (uint16_t)(offset + i),
                                    eeprom->address_bits);

            /* Read the data.  For microwire, each word requires the overhead
             * of eeprom setup and tear-down. */
            data[i] = e1000_shift_in_ee_bits(hw, 16);
            e1000_standby_eeprom(hw);
        }
    }

    /* End this read operation */
    e1000_release_eeprom(hw);

    return E1000_SUCCESS;
}

/******************************************************************************
 * Verifies that the EEPROM has a valid checksum
 *
 * hw - Struct containing variables accessed by shared code
 *
 * Reads the first 64 16 bit words of the EEPROM and sums the values read.
 * If the the sum of the 64 16 bit words is 0xBABA, the EEPROM's checksum is
 * valid.
 *****************************************************************************/
int32_t
e1000_validate_eeprom_checksum(struct e1000_hw *hw)
{
    uint16_t checksum = 0;
    uint16_t i, eeprom_data;

    DEBUGFUNC("e1000_validate_eeprom_checksum");

    for(i = 0; i < (EEPROM_CHECKSUM_REG + 1); i++) {
        if(e1000_read_eeprom(hw, i, 1, &eeprom_data) < 0) {
            DEBUGOUT("EEPROM Read Error\n");
            return -E1000_ERR_EEPROM;
        }
        checksum += eeprom_data;
    }

    if(checksum == (uint16_t) EEPROM_SUM)
        return E1000_SUCCESS;
    else {
        DEBUGOUT("EEPROM Checksum Invalid\n");
        return -E1000_ERR_EEPROM;
    }
}

/******************************************************************************
 * Calculates the EEPROM checksum and writes it to the EEPROM
 *
 * hw - Struct containing variables accessed by shared code
 *
 * Sums the first 63 16 bit words of the EEPROM. Subtracts the sum from 0xBABA.
 * Writes the difference to word offset 63 of the EEPROM.
 *****************************************************************************/
int32_t
e1000_update_eeprom_checksum(struct e1000_hw *hw)
{
    uint16_t checksum = 0;
    uint16_t i, eeprom_data;

    DEBUGFUNC("e1000_update_eeprom_checksum");

    for(i = 0; i < EEPROM_CHECKSUM_REG; i++) {
        if(e1000_read_eeprom(hw, i, 1, &eeprom_data) < 0) {
            DEBUGOUT("EEPROM Read Error\n");
            return -E1000_ERR_EEPROM;
        }
        checksum += eeprom_data;
    }
    checksum = (uint16_t) EEPROM_SUM - checksum;
    if(e1000_write_eeprom(hw, EEPROM_CHECKSUM_REG, 1, &checksum) < 0) {
        DEBUGOUT("EEPROM Write Error\n");
        return -E1000_ERR_EEPROM;
    }
    return E1000_SUCCESS;
}

/******************************************************************************
 * Parent function for writing words to the different EEPROM types.
 *
 * hw - Struct containing variables accessed by shared code
 * offset - offset within the EEPROM to be written to
 * words - number of words to write
 * data - 16 bit word to be written to the EEPROM
 *
 * If e1000_update_eeprom_checksum is not called after this function, the
 * EEPROM will most likely contain an invalid checksum.
 *****************************************************************************/
int32_t
e1000_write_eeprom(struct e1000_hw *hw,
                   uint16_t offset,
                   uint16_t words,
                   uint16_t *data)
{
    struct e1000_eeprom_info *eeprom = &hw->eeprom;
    int32_t status = 0;

    DEBUGFUNC("e1000_write_eeprom");

    /* A check for invalid values:  offset too large, too many words, and not
     * enough words.
     */
    if((offset > eeprom->word_size) || (words > eeprom->word_size - offset) ||
       (words == 0)) {
        DEBUGOUT("\"words\" parameter out of bounds\n");
        return -E1000_ERR_EEPROM;
    }

    /* Prepare the EEPROM for writing  */
    if (e1000_acquire_eeprom(hw) != E1000_SUCCESS)
        return -E1000_ERR_EEPROM;

    if(eeprom->type == e1000_eeprom_microwire) {
        status = e1000_write_eeprom_microwire(hw, offset, words, data);
    } else {
        status = e1000_write_eeprom_spi(hw, offset, words, data);
        msec_delay(10);
    }

    /* Done with writing */
    e1000_release_eeprom(hw);

    return status;
}

/******************************************************************************
 * Writes a 16 bit word to a given offset in an SPI EEPROM.
 *
 * hw - Struct containing variables accessed by shared code
 * offset - offset within the EEPROM to be written to
 * words - number of words to write
 * data - pointer to array of 8 bit words to be written to the EEPROM
 *
 *****************************************************************************/
int32_t
e1000_write_eeprom_spi(struct e1000_hw *hw,
                       uint16_t offset,
                       uint16_t words,
                       uint16_t *data)
{
    struct e1000_eeprom_info *eeprom = &hw->eeprom;
    uint16_t widx = 0;

    DEBUGFUNC("e1000_write_eeprom_spi");

    while (widx < words) {
        uint8_t write_opcode = EEPROM_WRITE_OPCODE_SPI;

        if(e1000_spi_eeprom_ready(hw)) return -E1000_ERR_EEPROM;

        e1000_standby_eeprom(hw);

        /*  Send the WRITE ENABLE command (8 bit opcode )  */
        e1000_shift_out_ee_bits(hw, EEPROM_WREN_OPCODE_SPI,
                                    eeprom->opcode_bits);

        e1000_standby_eeprom(hw);

        /* Some SPI eeproms use the 8th address bit embedded in the opcode */
        if((eeprom->address_bits == 8) && (offset >= 128))
            write_opcode |= EEPROM_A8_OPCODE_SPI;

        /* Send the Write command (8-bit opcode + addr) */
        e1000_shift_out_ee_bits(hw, write_opcode, eeprom->opcode_bits);

        e1000_shift_out_ee_bits(hw, (uint16_t)((offset + widx)*2),
                                eeprom->address_bits);

        /* Send the data */

        /* Loop to allow for up to whole page write (32 bytes) of eeprom */
        while (widx < words) {
            uint16_t word_out = data[widx];
            word_out = (word_out >> 8) | (word_out << 8);
            e1000_shift_out_ee_bits(hw, word_out, 16);
            widx++;

            /* Some larger eeprom sizes are capable of a 32-byte PAGE WRITE
             * operation, while the smaller eeproms are capable of an 8-byte
             * PAGE WRITE operation.  Break the inner loop to pass new address
             */
            if((((offset + widx)*2) % eeprom->page_size) == 0) {
                e1000_standby_eeprom(hw);
                break;
            }
        }
    }

    return E1000_SUCCESS;
}

/******************************************************************************
 * Writes a 16 bit word to a given offset in a Microwire EEPROM.
 *
 * hw - Struct containing variables accessed by shared code
 * offset - offset within the EEPROM to be written to
 * words - number of words to write
 * data - pointer to array of 16 bit words to be written to the EEPROM
 *
 *****************************************************************************/
int32_t
e1000_write_eeprom_microwire(struct e1000_hw *hw,
                             uint16_t offset,
                             uint16_t words,
                             uint16_t *data)
{
    struct e1000_eeprom_info *eeprom = &hw->eeprom;
    uint32_t eecd;
    uint16_t words_written = 0;
    uint16_t i = 0;

    DEBUGFUNC("e1000_write_eeprom_microwire");

    /* Send the write enable command to the EEPROM (3-bit opcode plus
     * 6/8-bit dummy address beginning with 11).  It's less work to include
     * the 11 of the dummy address as part of the opcode than it is to shift
     * it over the correct number of bits for the address.  This puts the
     * EEPROM into write/erase mode.
     */
    e1000_shift_out_ee_bits(hw, EEPROM_EWEN_OPCODE_MICROWIRE,
                            (uint16_t)(eeprom->opcode_bits + 2));

    e1000_shift_out_ee_bits(hw, 0, (uint16_t)(eeprom->address_bits - 2));

    /* Prepare the EEPROM */
    e1000_standby_eeprom(hw);

    while (words_written < words) {
        /* Send the Write command (3-bit opcode + addr) */
        e1000_shift_out_ee_bits(hw, EEPROM_WRITE_OPCODE_MICROWIRE,
                                eeprom->opcode_bits);

        e1000_shift_out_ee_bits(hw, (uint16_t)(offset + words_written),
                                eeprom->address_bits);

        /* Send the data */
        e1000_shift_out_ee_bits(hw, data[words_written], 16);

        /* Toggle the CS line.  This in effect tells the EEPROM to execute
         * the previous command.
         */
        e1000_standby_eeprom(hw);

        /* Read DO repeatedly until it is high (equal to '1').  The EEPROM will
         * signal that the command has been completed by raising the DO signal.
         * If DO does not go high in 10 milliseconds, then error out.
         */
        for(i = 0; i < 200; i++) {
            eecd = E1000_READ_REG(hw, EECD);
            if(eecd & E1000_EECD_DO) break;
            udelay(50);
        }
        if(i == 200) {
            DEBUGOUT("EEPROM Write did not complete\n");
            return -E1000_ERR_EEPROM;
        }

        /* Recover from write */
        e1000_standby_eeprom(hw);

        words_written++;
    }

    /* Send the write disable command to the EEPROM (3-bit opcode plus
     * 6/8-bit dummy address beginning with 10).  It's less work to include
     * the 10 of the dummy address as part of the opcode than it is to shift
     * it over the correct number of bits for the address.  This takes the
     * EEPROM out of write/erase mode.
     */
    e1000_shift_out_ee_bits(hw, EEPROM_EWDS_OPCODE_MICROWIRE,
                            (uint16_t)(eeprom->opcode_bits + 2));

    e1000_shift_out_ee_bits(hw, 0, (uint16_t)(eeprom->address_bits - 2));

    return E1000_SUCCESS;
}

/******************************************************************************
 * Reads the adapter's part number from the EEPROM
 *
 * hw - Struct containing variables accessed by shared code
 * part_num - Adapter's part number
 *****************************************************************************/
int32_t
e1000_read_part_num(struct e1000_hw *hw,
                    uint32_t *part_num)
{
    uint16_t offset = EEPROM_PBA_BYTE_1;
    uint16_t eeprom_data;

    DEBUGFUNC("e1000_read_part_num");

    /* Get word 0 from EEPROM */
    if(e1000_read_eeprom(hw, offset, 1, &eeprom_data) < 0) {
        DEBUGOUT("EEPROM Read Error\n");
        return -E1000_ERR_EEPROM;
    }
    /* Save word 0 in upper half of part_num */
    *part_num = (uint32_t) (eeprom_data << 16);

    /* Get word 1 from EEPROM */
    if(e1000_read_eeprom(hw, ++offset, 1, &eeprom_data) < 0) {
        DEBUGOUT("EEPROM Read Error\n");
        return -E1000_ERR_EEPROM;
    }
    /* Save word 1 in lower half of part_num */
    *part_num |= eeprom_data;

    return E1000_SUCCESS;
}

/******************************************************************************
 * Reads the adapter's MAC address from the EEPROM and inverts the LSB for the
 * second function of dual function devices
 *
 * hw - Struct containing variables accessed by shared code
 *****************************************************************************/
int32_t
e1000_read_mac_addr(struct e1000_hw * hw)
{
    uint16_t offset;
    uint16_t eeprom_data, i;

    DEBUGFUNC("e1000_read_mac_addr");

    for(i = 0; i < NODE_ADDRESS_SIZE; i += 2) {
        offset = i >> 1;
        if(e1000_read_eeprom(hw, offset, 1, &eeprom_data) < 0) {
            DEBUGOUT("EEPROM Read Error\n");
            return -E1000_ERR_EEPROM;
        }
        hw->perm_mac_addr[i] = (uint8_t) (eeprom_data & 0x00FF);
        hw->perm_mac_addr[i+1] = (uint8_t) (eeprom_data >> 8);
    }
    if(((hw->mac_type == e1000_82546) || (hw->mac_type == e1000_82546_rev_3)) &&
       (E1000_READ_REG(hw, STATUS) & E1000_STATUS_FUNC_1))
            hw->perm_mac_addr[5] ^= 0x01;

    for(i = 0; i < NODE_ADDRESS_SIZE; i++)
        hw->mac_addr[i] = hw->perm_mac_addr[i];
    return E1000_SUCCESS;
}

/******************************************************************************
 * Initializes receive address filters.
 *
 * hw - Struct containing variables accessed by shared code
 *
 * Places the MAC address in receive address register 0 and clears the rest
 * of the receive addresss registers. Clears the multicast table. Assumes
 * the receiver is in reset when the routine is called.
 *****************************************************************************/
void
e1000_init_rx_addrs(struct e1000_hw *hw)
{
    uint32_t i;

    DEBUGFUNC("e1000_init_rx_addrs");

    /* Setup the receive address. */
    DEBUGOUT("Programming MAC Address into RAR[0]\n");

    e1000_rar_set(hw, hw->mac_addr, 0);

    /* Zero out the other 15 receive addresses. */
    DEBUGOUT("Clearing RAR[1-15]\n");
    for(i = 1; i < E1000_RAR_ENTRIES; i++) {
        E1000_WRITE_REG_ARRAY(hw, RA, (i << 1), 0);
        E1000_WRITE_REG_ARRAY(hw, RA, ((i << 1) + 1), 0);
    }
}

/******************************************************************************
 * Updates the MAC's list of multicast addresses.
 *
 * hw - Struct containing variables accessed by shared code
 * mc_addr_list - the list of new multicast addresses
 * mc_addr_count - number of addresses
 * pad - number of bytes between addresses in the list
 * rar_used_count - offset where to start adding mc addresses into the RAR's
 *
 * The given list replaces any existing list. Clears the last 15 receive
 * address registers and the multicast table. Uses receive address registers
 * for the first 15 multicast addresses, and hashes the rest into the
 * multicast table.
 *****************************************************************************/
void
e1000_mc_addr_list_update(struct e1000_hw *hw,
                          uint8_t *mc_addr_list,
                          uint32_t mc_addr_count,
                          uint32_t pad,
                          uint32_t rar_used_count)
{
    uint32_t hash_value;
    uint32_t i;

    DEBUGFUNC("e1000_mc_addr_list_update");

    /* Set the new number of MC addresses that we are being requested to use. */
    hw->num_mc_addrs = mc_addr_count;

    /* Clear RAR[1-15] */
    DEBUGOUT(" Clearing RAR[1-15]\n");
    for(i = rar_used_count; i < E1000_RAR_ENTRIES; i++) {
        E1000_WRITE_REG_ARRAY(hw, RA, (i << 1), 0);
        E1000_WRITE_REG_ARRAY(hw, RA, ((i << 1) + 1), 0);
    }

    /* Clear the MTA */
    DEBUGOUT(" Clearing MTA\n");
    for(i = 0; i < E1000_NUM_MTA_REGISTERS; i++) {
        E1000_WRITE_REG_ARRAY(hw, MTA, i, 0);
    }

    /* Add the new addresses */
    for(i = 0; i < mc_addr_count; i++) {
        DEBUGOUT(" Adding the multicast addresses:\n");
        DEBUGOUT7(" MC Addr #%d =%.2X %.2X %.2X %.2X %.2X %.2X\n", i,
                  mc_addr_list[i * (ETH_LENGTH_OF_ADDRESS + pad)],
                  mc_addr_list[i * (ETH_LENGTH_OF_ADDRESS + pad) + 1],
                  mc_addr_list[i * (ETH_LENGTH_OF_ADDRESS + pad) + 2],
                  mc_addr_list[i * (ETH_LENGTH_OF_ADDRESS + pad) + 3],
                  mc_addr_list[i * (ETH_LENGTH_OF_ADDRESS + pad) + 4],
                  mc_addr_list[i * (ETH_LENGTH_OF_ADDRESS + pad) + 5]);

        hash_value = e1000_hash_mc_addr(hw,
                                        mc_addr_list +
                                        (i * (ETH_LENGTH_OF_ADDRESS + pad)));

        DEBUGOUT1(" Hash value = 0x%03X\n", hash_value);

        /* Place this multicast address in the RAR if there is room, *
         * else put it in the MTA
         */
        if(rar_used_count < E1000_RAR_ENTRIES) {
            e1000_rar_set(hw,
                          mc_addr_list + (i * (ETH_LENGTH_OF_ADDRESS + pad)),
                          rar_used_count);
            rar_used_count++;
        } else {
            e1000_mta_set(hw, hash_value);
        }
    }
    DEBUGOUT("MC Update Complete\n");
}

/******************************************************************************
 * Hashes an address to determine its location in the multicast table
 *
 * hw - Struct containing variables accessed by shared code
 * mc_addr - the multicast address to hash
 *****************************************************************************/
uint32_t
e1000_hash_mc_addr(struct e1000_hw *hw,
                   uint8_t *mc_addr)
{
    uint32_t hash_value = 0;

    /* The portion of the address that is used for the hash table is
     * determined by the mc_filter_type setting.
     */
    switch (hw->mc_filter_type) {
    /* [0] [1] [2] [3] [4] [5]
     * 01  AA  00  12  34  56
     * LSB                 MSB
     */
    case 0:
        /* [47:36] i.e. 0x563 for above example address */
        hash_value = ((mc_addr[4] >> 4) | (((uint16_t) mc_addr[5]) << 4));
        break;
    case 1:
        /* [46:35] i.e. 0xAC6 for above example address */
        hash_value = ((mc_addr[4] >> 3) | (((uint16_t) mc_addr[5]) << 5));
        break;
    case 2:
        /* [45:34] i.e. 0x5D8 for above example address */
        hash_value = ((mc_addr[4] >> 2) | (((uint16_t) mc_addr[5]) << 6));
        break;
    case 3:
        /* [43:32] i.e. 0x634 for above example address */
        hash_value = ((mc_addr[4]) | (((uint16_t) mc_addr[5]) << 8));
        break;
    }

    hash_value &= 0xFFF;
    return hash_value;
}

/******************************************************************************
 * Sets the bit in the multicast table corresponding to the hash value.
 *
 * hw - Struct containing variables accessed by shared code
 * hash_value - Multicast address hash value
 *****************************************************************************/
void
e1000_mta_set(struct e1000_hw *hw,
              uint32_t hash_value)
{
    uint32_t hash_bit, hash_reg;
    uint32_t mta;
    uint32_t temp;

    /* The MTA is a register array of 128 32-bit registers.
     * It is treated like an array of 4096 bits.  We want to set
     * bit BitArray[hash_value]. So we figure out what register
     * the bit is in, read it, OR in the new bit, then write
     * back the new value.  The register is determined by the
     * upper 7 bits of the hash value and the bit within that
     * register are determined by the lower 5 bits of the value.
     */
    hash_reg = (hash_value >> 5) & 0x7F;
    hash_bit = hash_value & 0x1F;

    mta = E1000_READ_REG_ARRAY(hw, MTA, hash_reg);

    mta |= (1 << hash_bit);

    /* If we are on an 82544 and we are trying to write an odd offset
     * in the MTA, save off the previous entry before writing and
     * restore the old value after writing.
     */
    if((hw->mac_type == e1000_82544) && ((hash_reg & 0x1) == 1)) {
        temp = E1000_READ_REG_ARRAY(hw, MTA, (hash_reg - 1));
        E1000_WRITE_REG_ARRAY(hw, MTA, hash_reg, mta);
        E1000_WRITE_REG_ARRAY(hw, MTA, (hash_reg - 1), temp);
    } else {
        E1000_WRITE_REG_ARRAY(hw, MTA, hash_reg, mta);
    }
}

/******************************************************************************
 * Puts an ethernet address into a receive address register.
 *
 * hw - Struct containing variables accessed by shared code
 * addr - Address to put into receive address register
 * index - Receive address register to write
 *****************************************************************************/
void
e1000_rar_set(struct e1000_hw *hw,
              uint8_t *addr,
              uint32_t index)
{
    uint32_t rar_low, rar_high;

    /* HW expects these in little endian so we reverse the byte order
     * from network order (big endian) to little endian
     */
    rar_low = ((uint32_t) addr[0] |
               ((uint32_t) addr[1] << 8) |
               ((uint32_t) addr[2] << 16) | ((uint32_t) addr[3] << 24));

    rar_high = ((uint32_t) addr[4] | ((uint32_t) addr[5] << 8) | E1000_RAH_AV);

    E1000_WRITE_REG_ARRAY(hw, RA, (index << 1), rar_low);
    E1000_WRITE_REG_ARRAY(hw, RA, ((index << 1) + 1), rar_high);
}

/******************************************************************************
 * Writes a value to the specified offset in the VLAN filter table.
 *
 * hw - Struct containing variables accessed by shared code
 * offset - Offset in VLAN filer table to write
 * value - Value to write into VLAN filter table
 *****************************************************************************/
void
e1000_write_vfta(struct e1000_hw *hw,
                 uint32_t offset,
                 uint32_t value)
{
    uint32_t temp;

    if((hw->mac_type == e1000_82544) && ((offset & 0x1) == 1)) {
        temp = E1000_READ_REG_ARRAY(hw, VFTA, (offset - 1));
        E1000_WRITE_REG_ARRAY(hw, VFTA, offset, value);
        E1000_WRITE_REG_ARRAY(hw, VFTA, (offset - 1), temp);
    } else {
        E1000_WRITE_REG_ARRAY(hw, VFTA, offset, value);
    }
}

/******************************************************************************
 * Clears the VLAN filer table
 *
 * hw - Struct containing variables accessed by shared code
 *****************************************************************************/
void
e1000_clear_vfta(struct e1000_hw *hw)
{
    uint32_t offset;

    for(offset = 0; offset < E1000_VLAN_FILTER_TBL_SIZE; offset++)
        E1000_WRITE_REG_ARRAY(hw, VFTA, offset, 0);
}

static int32_t
e1000_id_led_init(struct e1000_hw * hw)
{
    uint32_t ledctl;
    const uint32_t ledctl_mask = 0x000000FF;
    const uint32_t ledctl_on = E1000_LEDCTL_MODE_LED_ON;
    const uint32_t ledctl_off = E1000_LEDCTL_MODE_LED_OFF;
    uint16_t eeprom_data, i, temp;
    const uint16_t led_mask = 0x0F;

    DEBUGFUNC("e1000_id_led_init");

    if(hw->mac_type < e1000_82540) {
        /* Nothing to do */
        return E1000_SUCCESS;
    }

    ledctl = E1000_READ_REG(hw, LEDCTL);
    hw->ledctl_default = ledctl;
    hw->ledctl_mode1 = hw->ledctl_default;
    hw->ledctl_mode2 = hw->ledctl_default;

    if(e1000_read_eeprom(hw, EEPROM_ID_LED_SETTINGS, 1, &eeprom_data) < 0) {
        DEBUGOUT("EEPROM Read Error\n");
        return -E1000_ERR_EEPROM;
    }
    if((eeprom_data== ID_LED_RESERVED_0000) ||
       (eeprom_data == ID_LED_RESERVED_FFFF)) eeprom_data = ID_LED_DEFAULT;
    for(i = 0; i < 4; i++) {
        temp = (eeprom_data >> (i << 2)) & led_mask;
        switch(temp) {
        case ID_LED_ON1_DEF2:
        case ID_LED_ON1_ON2:
        case ID_LED_ON1_OFF2:
            hw->ledctl_mode1 &= ~(ledctl_mask << (i << 3));
            hw->ledctl_mode1 |= ledctl_on << (i << 3);
            break;
        case ID_LED_OFF1_DEF2:
        case ID_LED_OFF1_ON2:
        case ID_LED_OFF1_OFF2:
            hw->ledctl_mode1 &= ~(ledctl_mask << (i << 3));
            hw->ledctl_mode1 |= ledctl_off << (i << 3);
            break;
        default:
            /* Do nothing */
            break;
        }
        switch(temp) {
        case ID_LED_DEF1_ON2:
        case ID_LED_ON1_ON2:
        case ID_LED_OFF1_ON2:
            hw->ledctl_mode2 &= ~(ledctl_mask << (i << 3));
            hw->ledctl_mode2 |= ledctl_on << (i << 3);
            break;
        case ID_LED_DEF1_OFF2:
        case ID_LED_ON1_OFF2:
        case ID_LED_OFF1_OFF2:
            hw->ledctl_mode2 &= ~(ledctl_mask << (i << 3));
            hw->ledctl_mode2 |= ledctl_off << (i << 3);
            break;
        default:
            /* Do nothing */
            break;
        }
    }
    return E1000_SUCCESS;
}

/******************************************************************************
 * Prepares SW controlable LED for use and saves the current state of the LED.
 *
 * hw - Struct containing variables accessed by shared code
 *****************************************************************************/
int32_t
e1000_setup_led(struct e1000_hw *hw)
{
    uint32_t ledctl;
    int32_t ret_val = E1000_SUCCESS;

    DEBUGFUNC("e1000_setup_led");

    switch(hw->mac_type) {
    case e1000_82542_rev2_0:
    case e1000_82542_rev2_1:
    case e1000_82543:
    case e1000_82544:
        /* No setup necessary */
        break;
    case e1000_82541:
    case e1000_82547:
    case e1000_82541_rev_2:
    case e1000_82547_rev_2:
        /* Turn off PHY Smart Power Down (if enabled) */
        ret_val = e1000_read_phy_reg(hw, IGP01E1000_GMII_FIFO,
                                     &hw->phy_spd_default);
        if(ret_val)
            return ret_val;
        ret_val = e1000_write_phy_reg(hw, IGP01E1000_GMII_FIFO,
                                      (uint16_t)(hw->phy_spd_default &
                                      ~IGP01E1000_GMII_SPD));
        if(ret_val)
            return ret_val;
        /* Fall Through */
    default:
        if(hw->media_type == e1000_media_type_fiber) {
            ledctl = E1000_READ_REG(hw, LEDCTL);
            /* Save current LEDCTL settings */
            hw->ledctl_default = ledctl;
            /* Turn off LED0 */
            ledctl &= ~(E1000_LEDCTL_LED0_IVRT |
                        E1000_LEDCTL_LED0_BLINK |
                        E1000_LEDCTL_LED0_MODE_MASK);
            ledctl |= (E1000_LEDCTL_MODE_LED_OFF <<
                       E1000_LEDCTL_LED0_MODE_SHIFT);
            E1000_WRITE_REG(hw, LEDCTL, ledctl);
        } else if(hw->media_type == e1000_media_type_copper)
            E1000_WRITE_REG(hw, LEDCTL, hw->ledctl_mode1);
        break;
    }

    return E1000_SUCCESS;
}

/******************************************************************************
 * Restores the saved state of the SW controlable LED.
 *
 * hw - Struct containing variables accessed by shared code
 *****************************************************************************/
int32_t
e1000_cleanup_led(struct e1000_hw *hw)
{
    int32_t ret_val = E1000_SUCCESS;

    DEBUGFUNC("e1000_cleanup_led");

    switch(hw->mac_type) {
    case e1000_82542_rev2_0:
    case e1000_82542_rev2_1:
    case e1000_82543:
    case e1000_82544:
        /* No cleanup necessary */
        break;
    case e1000_82541:
    case e1000_82547:
    case e1000_82541_rev_2:
    case e1000_82547_rev_2:
        /* Turn on PHY Smart Power Down (if previously enabled) */
        ret_val = e1000_write_phy_reg(hw, IGP01E1000_GMII_FIFO,
                                      hw->phy_spd_default);
        if(ret_val)
            return ret_val;
        /* Fall Through */
    default:
        /* Restore LEDCTL settings */
        E1000_WRITE_REG(hw, LEDCTL, hw->ledctl_default);
        break;
    }

    return E1000_SUCCESS;
}

/******************************************************************************
 * Turns on the software controllable LED
 *
 * hw - Struct containing variables accessed by shared code
 *****************************************************************************/
int32_t
e1000_led_on(struct e1000_hw *hw)
{
    uint32_t ctrl = E1000_READ_REG(hw, CTRL);

    DEBUGFUNC("e1000_led_on");

    switch(hw->mac_type) {
    case e1000_82542_rev2_0:
    case e1000_82542_rev2_1:
    case e1000_82543:
        /* Set SW Defineable Pin 0 to turn on the LED */
        ctrl |= E1000_CTRL_SWDPIN0;
        ctrl |= E1000_CTRL_SWDPIO0;
        break;
    case e1000_82544:
        if(hw->media_type == e1000_media_type_fiber) {
            /* Set SW Defineable Pin 0 to turn on the LED */
            ctrl |= E1000_CTRL_SWDPIN0;
            ctrl |= E1000_CTRL_SWDPIO0;
        } else {
            /* Clear SW Defineable Pin 0 to turn on the LED */
            ctrl &= ~E1000_CTRL_SWDPIN0;
            ctrl |= E1000_CTRL_SWDPIO0;
        }
        break;
    default:
        if(hw->media_type == e1000_media_type_fiber) {
            /* Clear SW Defineable Pin 0 to turn on the LED */
            ctrl &= ~E1000_CTRL_SWDPIN0;
            ctrl |= E1000_CTRL_SWDPIO0;
        } else if(hw->media_type == e1000_media_type_copper) {
            E1000_WRITE_REG(hw, LEDCTL, hw->ledctl_mode2);
            return E1000_SUCCESS;
        }
        break;
    }

    E1000_WRITE_REG(hw, CTRL, ctrl);

    return E1000_SUCCESS;
}

/******************************************************************************
 * Turns off the software controllable LED
 *
 * hw - Struct containing variables accessed by shared code
 *****************************************************************************/
int32_t
e1000_led_off(struct e1000_hw *hw)
{
    uint32_t ctrl = E1000_READ_REG(hw, CTRL);

    DEBUGFUNC("e1000_led_off");

    switch(hw->mac_type) {
    case e1000_82542_rev2_0:
    case e1000_82542_rev2_1:
    case e1000_82543:
        /* Clear SW Defineable Pin 0 to turn off the LED */
        ctrl &= ~E1000_CTRL_SWDPIN0;
        ctrl |= E1000_CTRL_SWDPIO0;
        break;
    case e1000_82544:
        if(hw->media_type == e1000_media_type_fiber) {
            /* Clear SW Defineable Pin 0 to turn off the LED */
            ctrl &= ~E1000_CTRL_SWDPIN0;
            ctrl |= E1000_CTRL_SWDPIO0;
        } else {
            /* Set SW Defineable Pin 0 to turn off the LED */
            ctrl |= E1000_CTRL_SWDPIN0;
            ctrl |= E1000_CTRL_SWDPIO0;
        }
        break;
    default:
        if(hw->media_type == e1000_media_type_fiber) {
            /* Set SW Defineable Pin 0 to turn off the LED */
            ctrl |= E1000_CTRL_SWDPIN0;
            ctrl |= E1000_CTRL_SWDPIO0;
        } else if(hw->media_type == e1000_media_type_copper) {
            E1000_WRITE_REG(hw, LEDCTL, hw->ledctl_mode1);
            return E1000_SUCCESS;
        }
        break;
    }

    E1000_WRITE_REG(hw, CTRL, ctrl);

    return E1000_SUCCESS;
}

/******************************************************************************
 * Clears all hardware statistics counters.
 *
 * hw - Struct containing variables accessed by shared code
 *****************************************************************************/
void
e1000_clear_hw_cntrs(struct e1000_hw *hw)
{
    volatile uint32_t temp;

    temp = E1000_READ_REG(hw, CRCERRS);
    temp = E1000_READ_REG(hw, SYMERRS);
    temp = E1000_READ_REG(hw, MPC);
    temp = E1000_READ_REG(hw, SCC);
    temp = E1000_READ_REG(hw, ECOL);
    temp = E1000_READ_REG(hw, MCC);
    temp = E1000_READ_REG(hw, LATECOL);
    temp = E1000_READ_REG(hw, COLC);
    temp = E1000_READ_REG(hw, DC);
    temp = E1000_READ_REG(hw, SEC);
    temp = E1000_READ_REG(hw, RLEC);
    temp = E1000_READ_REG(hw, XONRXC);
    temp = E1000_READ_REG(hw, XONTXC);
    temp = E1000_READ_REG(hw, XOFFRXC);
    temp = E1000_READ_REG(hw, XOFFTXC);
    temp = E1000_READ_REG(hw, FCRUC);
    temp = E1000_READ_REG(hw, PRC64);
    temp = E1000_READ_REG(hw, PRC127);
    temp = E1000_READ_REG(hw, PRC255);
    temp = E1000_READ_REG(hw, PRC511);
    temp = E1000_READ_REG(hw, PRC1023);
    temp = E1000_READ_REG(hw, PRC1522);
    temp = E1000_READ_REG(hw, GPRC);
    temp = E1000_READ_REG(hw, BPRC);
    temp = E1000_READ_REG(hw, MPRC);
    temp = E1000_READ_REG(hw, GPTC);
    temp = E1000_READ_REG(hw, GORCL);
    temp = E1000_READ_REG(hw, GORCH);
    temp = E1000_READ_REG(hw, GOTCL);
    temp = E1000_READ_REG(hw, GOTCH);
    temp = E1000_READ_REG(hw, RNBC);
    temp = E1000_READ_REG(hw, RUC);
    temp = E1000_READ_REG(hw, RFC);
    temp = E1000_READ_REG(hw, ROC);
    temp = E1000_READ_REG(hw, RJC);
    temp = E1000_READ_REG(hw, TORL);
    temp = E1000_READ_REG(hw, TORH);
    temp = E1000_READ_REG(hw, TOTL);
    temp = E1000_READ_REG(hw, TOTH);
    temp = E1000_READ_REG(hw, TPR);
    temp = E1000_READ_REG(hw, TPT);
    temp = E1000_READ_REG(hw, PTC64);
    temp = E1000_READ_REG(hw, PTC127);
    temp = E1000_READ_REG(hw, PTC255);
    temp = E1000_READ_REG(hw, PTC511);
    temp = E1000_READ_REG(hw, PTC1023);
    temp = E1000_READ_REG(hw, PTC1522);
    temp = E1000_READ_REG(hw, MPTC);
    temp = E1000_READ_REG(hw, BPTC);

    if(hw->mac_type < e1000_82543) return;

    temp = E1000_READ_REG(hw, ALGNERRC);
    temp = E1000_READ_REG(hw, RXERRC);
    temp = E1000_READ_REG(hw, TNCRS);
    temp = E1000_READ_REG(hw, CEXTERR);
    temp = E1000_READ_REG(hw, TSCTC);
    temp = E1000_READ_REG(hw, TSCTFC);

    if(hw->mac_type <= e1000_82544) return;

    temp = E1000_READ_REG(hw, MGTPRC);
    temp = E1000_READ_REG(hw, MGTPDC);
    temp = E1000_READ_REG(hw, MGTPTC);
}

/******************************************************************************
 * Resets Adaptive IFS to its default state.
 *
 * hw - Struct containing variables accessed by shared code
 *
 * Call this after e1000_init_hw. You may override the IFS defaults by setting
 * hw->ifs_params_forced to TRUE. However, you must initialize hw->
 * current_ifs_val, ifs_min_val, ifs_max_val, ifs_step_size, and ifs_ratio
 * before calling this function.
 *****************************************************************************/
void
e1000_reset_adaptive(struct e1000_hw *hw)
{
    DEBUGFUNC("e1000_reset_adaptive");

    if(hw->adaptive_ifs) {
        if(!hw->ifs_params_forced) {
            hw->current_ifs_val = 0;
            hw->ifs_min_val = IFS_MIN;
            hw->ifs_max_val = IFS_MAX;
            hw->ifs_step_size = IFS_STEP;
            hw->ifs_ratio = IFS_RATIO;
        }
        hw->in_ifs_mode = FALSE;
        E1000_WRITE_REG(hw, AIT, 0);
    } else {
        DEBUGOUT("Not in Adaptive IFS mode!\n");
    }
}

/******************************************************************************
 * Called during the callback/watchdog routine to update IFS value based on
 * the ratio of transmits to collisions.
 *
 * hw - Struct containing variables accessed by shared code
 * tx_packets - Number of transmits since last callback
 * total_collisions - Number of collisions since last callback
 *****************************************************************************/
void
e1000_update_adaptive(struct e1000_hw *hw)
{
    DEBUGFUNC("e1000_update_adaptive");

    if(hw->adaptive_ifs) {
        if((hw->collision_delta * hw->ifs_ratio) > hw->tx_packet_delta) {
            if(hw->tx_packet_delta > MIN_NUM_XMITS) {
                hw->in_ifs_mode = TRUE;
                if(hw->current_ifs_val < hw->ifs_max_val) {
                    if(hw->current_ifs_val == 0)
                        hw->current_ifs_val = hw->ifs_min_val;
                    else
                        hw->current_ifs_val += hw->ifs_step_size;
                    E1000_WRITE_REG(hw, AIT, hw->current_ifs_val);
                }
            }
        } else {
            if(hw->in_ifs_mode && (hw->tx_packet_delta <= MIN_NUM_XMITS)) {
                hw->current_ifs_val = 0;
                hw->in_ifs_mode = FALSE;
                E1000_WRITE_REG(hw, AIT, 0);
            }
        }
    } else {
        DEBUGOUT("Not in Adaptive IFS mode!\n");
    }
}

/******************************************************************************
 * Adjusts the statistic counters when a frame is accepted by TBI_ACCEPT
 *
 * hw - Struct containing variables accessed by shared code
 * frame_len - The length of the frame in question
 * mac_addr - The Ethernet destination address of the frame in question
 *****************************************************************************/
void
e1000_tbi_adjust_stats(struct e1000_hw *hw,
                       struct e1000_hw_stats *stats,
                       uint32_t frame_len,
                       uint8_t *mac_addr)
{
    uint64_t carry_bit;

    /* First adjust the frame length. */
    frame_len--;
    /* We need to adjust the statistics counters, since the hardware
     * counters overcount this packet as a CRC error and undercount
     * the packet as a good packet
     */
    /* This packet should not be counted as a CRC error.    */
    stats->crcerrs--;
    /* This packet does count as a Good Packet Received.    */
    stats->gprc++;

    /* Adjust the Good Octets received counters             */
    carry_bit = 0x80000000 & stats->gorcl;
    stats->gorcl += frame_len;
    /* If the high bit of Gorcl (the low 32 bits of the Good Octets
     * Received Count) was one before the addition,
     * AND it is zero after, then we lost the carry out,
     * need to add one to Gorch (Good Octets Received Count High).
     * This could be simplified if all environments supported
     * 64-bit integers.
     */
    if(carry_bit && ((stats->gorcl & 0x80000000) == 0))
        stats->gorch++;
    /* Is this a broadcast or multicast?  Check broadcast first,
     * since the test for a multicast frame will test positive on
     * a broadcast frame.
     */
    if((mac_addr[0] == (uint8_t) 0xff) && (mac_addr[1] == (uint8_t) 0xff))
        /* Broadcast packet */
        stats->bprc++;
    else if(*mac_addr & 0x01)
        /* Multicast packet */
        stats->mprc++;

    if(frame_len == hw->max_frame_size) {
        /* In this case, the hardware has overcounted the number of
         * oversize frames.
         */
        if(stats->roc > 0)
            stats->roc--;
    }

    /* Adjust the bin counters when the extra byte put the frame in the
     * wrong bin. Remember that the frame_len was adjusted above.
     */
    if(frame_len == 64) {
        stats->prc64++;
        stats->prc127--;
    } else if(frame_len == 127) {
        stats->prc127++;
        stats->prc255--;
    } else if(frame_len == 255) {
        stats->prc255++;
        stats->prc511--;
    } else if(frame_len == 511) {
        stats->prc511++;
        stats->prc1023--;
    } else if(frame_len == 1023) {
        stats->prc1023++;
        stats->prc1522--;
    } else if(frame_len == 1522) {
        stats->prc1522++;
    }
}

/******************************************************************************
 * Gets the current PCI bus type, speed, and width of the hardware
 *
 * hw - Struct containing variables accessed by shared code
 *****************************************************************************/
void
e1000_get_bus_info(struct e1000_hw *hw)
{
    uint32_t status;

    if(hw->mac_type < e1000_82543) {
        hw->bus_type = e1000_bus_type_unknown;
        hw->bus_speed = e1000_bus_speed_unknown;
        hw->bus_width = e1000_bus_width_unknown;
        return;
    }

    status = E1000_READ_REG(hw, STATUS);
    hw->bus_type = (status & E1000_STATUS_PCIX_MODE) ?
                   e1000_bus_type_pcix : e1000_bus_type_pci;

    if(hw->device_id == E1000_DEV_ID_82546EB_QUAD_COPPER) {
        hw->bus_speed = (hw->bus_type == e1000_bus_type_pci) ?
                        e1000_bus_speed_66 : e1000_bus_speed_120;
    } else if(hw->bus_type == e1000_bus_type_pci) {
        hw->bus_speed = (status & E1000_STATUS_PCI66) ?
                        e1000_bus_speed_66 : e1000_bus_speed_33;
    } else {
        switch (status & E1000_STATUS_PCIX_SPEED) {
        case E1000_STATUS_PCIX_SPEED_66:
            hw->bus_speed = e1000_bus_speed_66;
            break;
        case E1000_STATUS_PCIX_SPEED_100:
            hw->bus_speed = e1000_bus_speed_100;
            break;
        case E1000_STATUS_PCIX_SPEED_133:
            hw->bus_speed = e1000_bus_speed_133;
            break;
        default:
            hw->bus_speed = e1000_bus_speed_reserved;
            break;
        }
    }
    hw->bus_width = (status & E1000_STATUS_BUS64) ?
                    e1000_bus_width_64 : e1000_bus_width_32;
}
/******************************************************************************
 * Reads a value from one of the devices registers using port I/O (as opposed
 * memory mapped I/O). Only 82544 and newer devices support port I/O.
 *
 * hw - Struct containing variables accessed by shared code
 * offset - offset to read from
 *****************************************************************************/
uint32_t
e1000_read_reg_io(struct e1000_hw *hw,
                  uint32_t offset)
{
    unsigned long io_addr = hw->io_base;
    unsigned long io_data = hw->io_base + 4;

    e1000_io_write(hw, io_addr, offset);
    return e1000_io_read(hw, io_data);
}

/******************************************************************************
 * Writes a value to one of the devices registers using port I/O (as opposed to
 * memory mapped I/O). Only 82544 and newer devices support port I/O.
 *
 * hw - Struct containing variables accessed by shared code
 * offset - offset to write to
 * value - value to write
 *****************************************************************************/
void
e1000_write_reg_io(struct e1000_hw *hw,
                   uint32_t offset,
                   uint32_t value)
{
    unsigned long io_addr = hw->io_base;
    unsigned long io_data = hw->io_base + 4;

    e1000_io_write(hw, io_addr, offset);
    e1000_io_write(hw, io_data, value);
}


/******************************************************************************
 * Estimates the cable length.
 *
 * hw - Struct containing variables accessed by shared code
 * min_length - The estimated minimum length
 * max_length - The estimated maximum length
 *
 * returns: - E1000_ERR_XXX
 *            E1000_SUCCESS
 *
 * This function always returns a ranged length (minimum & maximum).
 * So for M88 phy's, this function interprets the one value returned from the
 * register to the minimum and maximum range.
 * For IGP phy's, the function calculates the range by the AGC registers.
 *****************************************************************************/
int32_t
e1000_get_cable_length(struct e1000_hw *hw,
                       uint16_t *min_length,
                       uint16_t *max_length)
{
    int32_t ret_val;
    uint16_t agc_value = 0;
    uint16_t cur_agc, min_agc = IGP01E1000_AGC_LENGTH_TABLE_SIZE;
    uint16_t i, phy_data;

    DEBUGFUNC("e1000_get_cable_length");

    *min_length = *max_length = 0;

    /* Use old method for Phy older than IGP */
    if(hw->phy_type == e1000_phy_m88) {
        ret_val = e1000_read_phy_reg(hw, M88E1000_PHY_SPEC_STATUS,
                                     &phy_data);
        if(ret_val)
            return ret_val;

        /* Convert the enum value to ranged values */
        switch((phy_data & M88E1000_PSSR_CABLE_LENGTH) >>
               M88E1000_PSSR_CABLE_LENGTH_SHIFT) {
        case e1000_cable_length_50:
            *min_length = 0;
            *max_length = e1000_igp_cable_length_50;
            break;
        case e1000_cable_length_50_80:
            *min_length = e1000_igp_cable_length_50;
            *max_length = e1000_igp_cable_length_80;
            break;
        case e1000_cable_length_80_110:
            *min_length = e1000_igp_cable_length_80;
            *max_length = e1000_igp_cable_length_110;
            break;
        case e1000_cable_length_110_140:
            *min_length = e1000_igp_cable_length_110;
            *max_length = e1000_igp_cable_length_140;
            break;
        case e1000_cable_length_140:
            *min_length = e1000_igp_cable_length_140;
            *max_length = e1000_igp_cable_length_170;
            break;
        default:
            return -E1000_ERR_PHY;
            break;
        }
    } else if(hw->phy_type == e1000_phy_igp) { /* For IGP PHY */
        uint16_t agc_reg_array[IGP01E1000_PHY_CHANNEL_NUM] =
                                                         {IGP01E1000_PHY_AGC_A,
                                                          IGP01E1000_PHY_AGC_B,
                                                          IGP01E1000_PHY_AGC_C,
                                                          IGP01E1000_PHY_AGC_D};
        /* Read the AGC registers for all channels */
        for(i = 0; i < IGP01E1000_PHY_CHANNEL_NUM; i++) {

            ret_val = e1000_read_phy_reg(hw, agc_reg_array[i], &phy_data);
            if(ret_val)
                return ret_val;

            cur_agc = phy_data >> IGP01E1000_AGC_LENGTH_SHIFT;

            /* Array bound check. */
            if((cur_agc >= IGP01E1000_AGC_LENGTH_TABLE_SIZE - 1) ||
               (cur_agc == 0))
                return -E1000_ERR_PHY;

            agc_value += cur_agc;

            /* Update minimal AGC value. */
            if(min_agc > cur_agc)
                min_agc = cur_agc;
        }

        /* Remove the minimal AGC result for length < 50m */
        if(agc_value < IGP01E1000_PHY_CHANNEL_NUM * e1000_igp_cable_length_50) {
            agc_value -= min_agc;

            /* Get the average length of the remaining 3 channels */
            agc_value /= (IGP01E1000_PHY_CHANNEL_NUM - 1);
        } else {
            /* Get the average length of all the 4 channels. */
            agc_value /= IGP01E1000_PHY_CHANNEL_NUM;
        }

        /* Set the range of the calculated length. */
        *min_length = ((e1000_igp_cable_length_table[agc_value] -
                       IGP01E1000_AGC_RANGE) > 0) ?
                       (e1000_igp_cable_length_table[agc_value] -
                       IGP01E1000_AGC_RANGE) : 0;
        *max_length = e1000_igp_cable_length_table[agc_value] +
                      IGP01E1000_AGC_RANGE;
    }

    return E1000_SUCCESS;
}

/******************************************************************************
 * Check the cable polarity
 *
 * hw - Struct containing variables accessed by shared code
 * polarity - output parameter : 0 - Polarity is not reversed
 *                               1 - Polarity is reversed.
 *
 * returns: - E1000_ERR_XXX
 *            E1000_SUCCESS
 *
 * For phy's older then IGP, this function simply reads the polarity bit in the
 * Phy Status register.  For IGP phy's, this bit is valid only if link speed is
 * 10 Mbps.  If the link speed is 100 Mbps there is no polarity so this bit will
 * return 0.  If the link speed is 1000 Mbps the polarity status is in the
 * IGP01E1000_PHY_PCS_INIT_REG.
 *****************************************************************************/
int32_t
e1000_check_polarity(struct e1000_hw *hw,
                     uint16_t *polarity)
{
    int32_t ret_val;
    uint16_t phy_data;

    DEBUGFUNC("e1000_check_polarity");

    if(hw->phy_type == e1000_phy_m88) {
        /* return the Polarity bit in the Status register. */
        ret_val = e1000_read_phy_reg(hw, M88E1000_PHY_SPEC_STATUS,
                                     &phy_data);
        if(ret_val)
            return ret_val;
        *polarity = (phy_data & M88E1000_PSSR_REV_POLARITY) >>
                    M88E1000_PSSR_REV_POLARITY_SHIFT;
    } else if(hw->phy_type == e1000_phy_igp) {
        /* Read the Status register to check the speed */
        ret_val = e1000_read_phy_reg(hw, IGP01E1000_PHY_PORT_STATUS,
                                     &phy_data);
        if(ret_val)
            return ret_val;

        /* If speed is 1000 Mbps, must read the IGP01E1000_PHY_PCS_INIT_REG to
         * find the polarity status */
        if((phy_data & IGP01E1000_PSSR_SPEED_MASK) ==
           IGP01E1000_PSSR_SPEED_1000MBPS) {

            /* Read the GIG initialization PCS register (0x00B4) */
            ret_val = e1000_read_phy_reg(hw, IGP01E1000_PHY_PCS_INIT_REG,
                                         &phy_data);
            if(ret_val)
                return ret_val;

            /* Check the polarity bits */
            *polarity = (phy_data & IGP01E1000_PHY_POLARITY_MASK) ? 1 : 0;
        } else {
            /* For 10 Mbps, read the polarity bit in the status register. (for
             * 100 Mbps this bit is always 0) */
            *polarity = phy_data & IGP01E1000_PSSR_POLARITY_REVERSED;
        }
    }
    return E1000_SUCCESS;
}

/******************************************************************************
 * Check if Downshift occured
 *
 * hw - Struct containing variables accessed by shared code
 * downshift - output parameter : 0 - No Downshift ocured.
 *                                1 - Downshift ocured.
 *
 * returns: - E1000_ERR_XXX
 *            E1000_SUCCESS 
 *
 * For phy's older then IGP, this function reads the Downshift bit in the Phy
 * Specific Status register.  For IGP phy's, it reads the Downgrade bit in the
 * Link Health register.  In IGP this bit is latched high, so the driver must
 * read it immediately after link is established.
 *****************************************************************************/
int32_t
e1000_check_downshift(struct e1000_hw *hw)
{
    int32_t ret_val;
    uint16_t phy_data;

    DEBUGFUNC("e1000_check_downshift");

    if(hw->phy_type == e1000_phy_igp) {
        ret_val = e1000_read_phy_reg(hw, IGP01E1000_PHY_LINK_HEALTH,
                                     &phy_data);
        if(ret_val)
            return ret_val;

        hw->speed_downgraded = (phy_data & IGP01E1000_PLHR_SS_DOWNGRADE) ? 1 : 0;
    }
    else if(hw->phy_type == e1000_phy_m88) {
        ret_val = e1000_read_phy_reg(hw, M88E1000_PHY_SPEC_STATUS,
                                     &phy_data);
        if(ret_val)
            return ret_val;

        hw->speed_downgraded = (phy_data & M88E1000_PSSR_DOWNSHIFT) >>
                               M88E1000_PSSR_DOWNSHIFT_SHIFT;
    }
    return E1000_SUCCESS;
}

/*****************************************************************************
 *
 * 82541_rev_2 & 82547_rev_2 have the capability to configure the DSP when a
 * gigabit link is achieved to improve link quality.
 *
 * hw: Struct containing variables accessed by shared code
 *
 * returns: - E1000_ERR_PHY if fail to read/write the PHY
 *            E1000_SUCCESS at any other case.
 *
 ****************************************************************************/

int32_t
e1000_config_dsp_after_link_change(struct e1000_hw *hw,
                                   boolean_t link_up)
{
    int32_t ret_val;
    uint16_t phy_data, speed, duplex, i;
    uint16_t dsp_reg_array[IGP01E1000_PHY_CHANNEL_NUM] =
                                        {IGP01E1000_PHY_AGC_PARAM_A,
                                        IGP01E1000_PHY_AGC_PARAM_B,
                                        IGP01E1000_PHY_AGC_PARAM_C,
                                        IGP01E1000_PHY_AGC_PARAM_D};
    uint16_t min_length, max_length;

    DEBUGFUNC("e1000_config_dsp_after_link_change");

    if(hw->phy_type != e1000_phy_igp)
        return E1000_SUCCESS;

    if(link_up) {
        ret_val = e1000_get_speed_and_duplex(hw, &speed, &duplex);
        if(ret_val) {
            DEBUGOUT("Error getting link speed and duplex\n");
            return ret_val;
        }

        if(speed == SPEED_1000) {

            e1000_get_cable_length(hw, &min_length, &max_length);

            if((hw->dsp_config_state == e1000_dsp_config_enabled) &&
                min_length >= e1000_igp_cable_length_50) {

                for(i = 0; i < IGP01E1000_PHY_CHANNEL_NUM; i++) {
                    ret_val = e1000_read_phy_reg(hw, dsp_reg_array[i],
                                                 &phy_data);
                    if(ret_val)
                        return ret_val;

                    phy_data &= ~IGP01E1000_PHY_EDAC_MU_INDEX;

                    ret_val = e1000_write_phy_reg(hw, dsp_reg_array[i],
                                                  phy_data);
                    if(ret_val)
                        return ret_val;
                }
                hw->dsp_config_state = e1000_dsp_config_activated;
            }

            if((hw->ffe_config_state == e1000_ffe_config_enabled) &&
               (min_length < e1000_igp_cable_length_50)) {

                uint16_t ffe_idle_err_timeout = FFE_IDLE_ERR_COUNT_TIMEOUT_20;
                uint32_t idle_errs = 0;

                /* clear previous idle error counts */
                ret_val = e1000_read_phy_reg(hw, PHY_1000T_STATUS,
                                             &phy_data);
                if(ret_val)
                    return ret_val;

                for(i = 0; i < ffe_idle_err_timeout; i++) {
                    udelay(1000);
                    ret_val = e1000_read_phy_reg(hw, PHY_1000T_STATUS,
                                                 &phy_data);
                    if(ret_val)
                        return ret_val;

                    idle_errs += (phy_data & SR_1000T_IDLE_ERROR_CNT);
                    if(idle_errs > SR_1000T_PHY_EXCESSIVE_IDLE_ERR_COUNT) {
                        hw->ffe_config_state = e1000_ffe_config_active;

                        ret_val = e1000_write_phy_reg(hw,
                                    IGP01E1000_PHY_DSP_FFE,
                                    IGP01E1000_PHY_DSP_FFE_CM_CP);
                        if(ret_val)
                            return ret_val;
                        break;
                    }

                    if(idle_errs)
                        ffe_idle_err_timeout = FFE_IDLE_ERR_COUNT_TIMEOUT_100;
                }
            }
        }
    } else {
        if(hw->dsp_config_state == e1000_dsp_config_activated) {
            ret_val = e1000_write_phy_reg(hw, 0x0000,
                                          IGP01E1000_IEEE_FORCE_GIGA);
            if(ret_val)
                return ret_val;
            for(i = 0; i < IGP01E1000_PHY_CHANNEL_NUM; i++) {
                ret_val = e1000_read_phy_reg(hw, dsp_reg_array[i], &phy_data);
                if(ret_val)
                    return ret_val;

                phy_data &= ~IGP01E1000_PHY_EDAC_MU_INDEX;
                phy_data |=  IGP01E1000_PHY_EDAC_SIGN_EXT_9_BITS;

                ret_val = e1000_write_phy_reg(hw,dsp_reg_array[i], phy_data);
                if(ret_val)
                    return ret_val;
            }

            ret_val = e1000_write_phy_reg(hw, 0x0000,
                                          IGP01E1000_IEEE_RESTART_AUTONEG);
            if(ret_val)
                return ret_val;

            hw->dsp_config_state = e1000_dsp_config_enabled;
        }

        if(hw->ffe_config_state == e1000_ffe_config_active) {
            ret_val = e1000_write_phy_reg(hw, 0x0000,
                                          IGP01E1000_IEEE_FORCE_GIGA);
            if(ret_val)
                return ret_val;
            ret_val = e1000_write_phy_reg(hw, IGP01E1000_PHY_DSP_FFE,
                                          IGP01E1000_PHY_DSP_FFE_DEFAULT);
            if(ret_val)
                return ret_val;

            ret_val = e1000_write_phy_reg(hw, 0x0000,
                                          IGP01E1000_IEEE_RESTART_AUTONEG);
            if(ret_val)
                return ret_val;
            hw->ffe_config_state = e1000_ffe_config_enabled;
        }
    }
    return E1000_SUCCESS;
}

/*****************************************************************************
 * Set PHY to class A mode
 * Assumes the following operations will follow to enable the new class mode.
 *  1. Do a PHY soft reset
 *  2. Restart auto-negotiation or force link.
 *
 * hw - Struct containing variables accessed by shared code
 ****************************************************************************/
static int32_t
e1000_set_phy_mode(struct e1000_hw *hw)
{
    int32_t ret_val;
    uint16_t eeprom_data;

    DEBUGFUNC("e1000_set_phy_mode");

    if((hw->mac_type == e1000_82545_rev_3) &&
       (hw->media_type == e1000_media_type_copper)) {
        ret_val = e1000_read_eeprom(hw, EEPROM_PHY_CLASS_WORD, 1, &eeprom_data);
        if(ret_val) {
            return ret_val;
        }

        if((eeprom_data != EEPROM_RESERVED_WORD) &&
           (eeprom_data & EEPROM_PHY_CLASS_A)) {
            ret_val = e1000_write_phy_reg(hw, M88E1000_PHY_PAGE_SELECT, 0x000B);
            if(ret_val)
                return ret_val;
            ret_val = e1000_write_phy_reg(hw, M88E1000_PHY_GEN_CONTROL, 0x8104);
            if(ret_val)
                return ret_val;

            hw->phy_reset_disable = FALSE;
        }
    }

    return E1000_SUCCESS;
}

/*****************************************************************************
 *
 * This function sets the lplu state according to the active flag.  When
 * activating lplu this function also disables smart speed and vise versa.
 * lplu will not be activated unless the device autonegotiation advertisment
 * meets standards of either 10 or 10/100 or 10/100/1000 at all duplexes.
 * hw: Struct containing variables accessed by shared code
 * active - true to enable lplu false to disable lplu.
 *
 * returns: - E1000_ERR_PHY if fail to read/write the PHY
 *            E1000_SUCCESS at any other case.
 *
 ****************************************************************************/

int32_t
e1000_set_d3_lplu_state(struct e1000_hw *hw,
                        boolean_t active)
{
    int32_t ret_val;
    uint16_t phy_data;
    DEBUGFUNC("e1000_set_d3_lplu_state");

    if(!((hw->mac_type == e1000_82541_rev_2) ||
         (hw->mac_type == e1000_82547_rev_2)))
        return E1000_SUCCESS;

    /* During driver activity LPLU should not be used or it will attain link
     * from the lowest speeds starting from 10Mbps. The capability is used for
     * Dx transitions and states */
    ret_val = e1000_read_phy_reg(hw, IGP01E1000_GMII_FIFO, &phy_data);
    if(ret_val)
        return ret_val;

    if(!active) {
        phy_data &= ~IGP01E1000_GMII_FLEX_SPD;
        ret_val = e1000_write_phy_reg(hw, IGP01E1000_GMII_FIFO, phy_data);
        if(ret_val)
            return ret_val;

        /* LPLU and SmartSpeed are mutually exclusive.  LPLU is used during
         * Dx states where the power conservation is most important.  During
         * driver activity we should enable SmartSpeed, so performance is
         * maintained. */
        ret_val = e1000_read_phy_reg(hw, IGP01E1000_PHY_PORT_CONFIG, &phy_data);
        if(ret_val)
            return ret_val;

        phy_data |= IGP01E1000_PSCFR_SMART_SPEED;
        ret_val = e1000_write_phy_reg(hw, IGP01E1000_PHY_PORT_CONFIG, phy_data);
        if(ret_val)
            return ret_val;

    } else if((hw->autoneg_advertised == AUTONEG_ADVERTISE_SPEED_DEFAULT) ||
              (hw->autoneg_advertised == AUTONEG_ADVERTISE_10_ALL ) ||
              (hw->autoneg_advertised == AUTONEG_ADVERTISE_10_100_ALL)) {

        phy_data |= IGP01E1000_GMII_FLEX_SPD;
        ret_val = e1000_write_phy_reg(hw, IGP01E1000_GMII_FIFO, phy_data);
        if(ret_val)
            return ret_val;

        /* When LPLU is enabled we should disable SmartSpeed */
        ret_val = e1000_read_phy_reg(hw, IGP01E1000_PHY_PORT_CONFIG, &phy_data);
        if(ret_val)
            return ret_val;

        phy_data &= ~IGP01E1000_PSCFR_SMART_SPEED;
        ret_val = e1000_write_phy_reg(hw, IGP01E1000_PHY_PORT_CONFIG, phy_data);
        if(ret_val)
            return ret_val;

    }
    return E1000_SUCCESS;
}

/******************************************************************************
 * Change VCO speed register to improve Bit Error Rate performance of SERDES.
 *
 * hw - Struct containing variables accessed by shared code
 *****************************************************************************/
static int32_t
e1000_set_vco_speed(struct e1000_hw *hw)
{
    int32_t  ret_val;
    uint16_t default_page = 0;
    uint16_t phy_data;

    DEBUGFUNC("e1000_set_vco_speed");

    switch(hw->mac_type) {
    case e1000_82545_rev_3:
    case e1000_82546_rev_3:
       break;
    default:
        return E1000_SUCCESS;
    }

    /* Set PHY register 30, page 5, bit 8 to 0 */

    ret_val = e1000_read_phy_reg(hw, M88E1000_PHY_PAGE_SELECT, &default_page);
    if(ret_val)
        return ret_val;

    ret_val = e1000_write_phy_reg(hw, M88E1000_PHY_PAGE_SELECT, 0x0005);
    if(ret_val)
        return ret_val;

    ret_val = e1000_read_phy_reg(hw, M88E1000_PHY_GEN_CONTROL, &phy_data);
    if(ret_val)
        return ret_val;

    phy_data &= ~M88E1000_PHY_VCO_REG_BIT8;
    ret_val = e1000_write_phy_reg(hw, M88E1000_PHY_GEN_CONTROL, phy_data);
    if(ret_val)
        return ret_val;

    /* Set PHY register 30, page 4, bit 11 to 1 */

    ret_val = e1000_write_phy_reg(hw, M88E1000_PHY_PAGE_SELECT, 0x0004);
    if(ret_val)
        return ret_val;

    ret_val = e1000_read_phy_reg(hw, M88E1000_PHY_GEN_CONTROL, &phy_data);
    if(ret_val)
        return ret_val;

    phy_data |= M88E1000_PHY_VCO_REG_BIT11;
    ret_val = e1000_write_phy_reg(hw, M88E1000_PHY_GEN_CONTROL, phy_data);
    if(ret_val)
        return ret_val;

    ret_val = e1000_write_phy_reg(hw, M88E1000_PHY_PAGE_SELECT, default_page);
    if(ret_val)
        return ret_val;

    return E1000_SUCCESS;
}

/******************************************************************************
 * Verifies the hardware needs to allow ARPs to be processed by the host
 *
 * hw - Struct containing variables accessed by shared code
 *
 * returns: - TRUE/FALSE
 *
 *****************************************************************************/
uint32_t
e1000_enable_mng_pass_thru(struct e1000_hw *hw)
{
    uint32_t manc;

    if (hw->asf_firmware_present) {
        manc = E1000_READ_REG(hw, MANC);

        if (!(manc & E1000_MANC_RCV_TCO_EN) ||
            !(manc & E1000_MANC_EN_MAC_ADDR_FILTER))
            return FALSE;
        if ((manc & E1000_MANC_SMBUS_EN) && !(manc & E1000_MANC_ASF_EN))
            return TRUE;
    }
    return FALSE;
}
