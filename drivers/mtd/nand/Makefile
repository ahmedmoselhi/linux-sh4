# SPDX-License-Identifier: GPL-2.0

nandcore-objs := core.o bbt.o
obj-$(CONFIG_MTD_NAND_CORE) += nandcore.o

obj-$(CONFIG_MTD_NAND_STM_BCH)          += stm_nand_bch.o
obj-$(CONFIG_MTD_NAND_STM_EMI)		+= stm_nand_emi.o
obj-$(CONFIG_MTD_NAND_STM_FLEX)		+= stm_nand_flex.o stm_nand_ecc.o
obj-$(CONFIG_MTD_NAND_STM_AFM)          += stm_nand_afm.o stm_nand_ecc.o
obj-y	+= onenand/
obj-y	+= raw/
obj-y	+= spi/
