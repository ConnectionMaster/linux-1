# SPDX-License-Identifier: GPL-2.0
# define_trace.h needs to know how to find our header
CFLAGS_trace.o			:= -I$(src)

udc-core-y			:= core.o trace.o

#
# USB peripheral controller drivers
#
obj-$(CONFIG_USB_GADGET)	+= udc-core.o
obj-$(CONFIG_USB_DUMMY_HCD)	+= dummy_hcd.o
obj-$(CONFIG_USB_NET2280)	+= net2280.o
obj-$(CONFIG_USB_SNP_CORE)	+= snps_udc_core.o
obj-$(CONFIG_USB_AMD5536UDC)	+= amd5536udc_pci.o
obj-$(CONFIG_USB_PXA25X)	+= pxa25x_udc.o
obj-$(CONFIG_USB_PXA27X)	+= pxa27x_udc.o
obj-$(CONFIG_USB_GOKU)		+= goku_udc.o
obj-$(CONFIG_USB_OMAP)		+= omap_udc.o
obj-$(CONFIG_USB_AT91)		+= at91_udc.o
obj-$(CONFIG_USB_ATMEL_USBA)	+= atmel_usba_udc.o
obj-$(CONFIG_USB_BCM63XX_UDC)	+= bcm63xx_udc.o
obj-$(CONFIG_USB_FSL_USB2)	+= fsl_usb2_udc.o
fsl_usb2_udc-y			:= fsl_udc_core.o
obj-$(CONFIG_USB_TEGRA_XUDC)	+= tegra-xudc.o
obj-$(CONFIG_USB_M66592)	+= m66592-udc.o
obj-$(CONFIG_USB_R8A66597)	+= r8a66597-udc.o
obj-$(CONFIG_USB_RENESAS_USB3)	+= renesas_usb3.o
obj-$(CONFIG_USB_RZV2M_USB3DRD)	+= rzv2m_usb3drd.o
obj-$(CONFIG_USB_RENESAS_USBF)	+= renesas_usbf.o
obj-$(CONFIG_USB_FSL_QE)	+= fsl_qe_udc.o
obj-$(CONFIG_USB_LPC32XX)	+= lpc32xx_udc.o
obj-$(CONFIG_USB_EG20T)		+= pch_udc.o
obj-$(CONFIG_USB_GR_UDC)	+= gr_udc.o
obj-$(CONFIG_USB_GADGET_XILINX)	+= udc-xilinx.o
obj-$(CONFIG_USB_SNP_UDC_PLAT) += snps_udc_plat.o
obj-$(CONFIG_USB_ASPEED_VHUB)	+= aspeed-vhub/
obj-$(CONFIG_USB_ASPEED_UDC)	+= aspeed_udc.o
obj-$(CONFIG_USB_BDC_UDC)	+= bdc/
obj-$(CONFIG_USB_MAX3420_UDC)	+= max3420_udc.o
obj-$(CONFIG_USB_CDNS2_UDC)	+= cdns2/
