/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef REALTEK_Y4_NET_NAME_H
#define REALTEK_Y4_NET_NAME_H

#include <linux/netdevice.h>
#include <linux/of.h>
#include <linux/pci.h>

/* Dedicated onboard ports are named by their physical PCIe controller. */
static inline void rtl_y4_set_netdev_name(struct pci_dev *pdev,
                                         struct net_device *dev)
{
	struct pci_bus *bus = pdev->bus;
	struct device_node *node;
	const char *name = NULL;
	u32 controller;
	bool y12 = of_machine_is_compatible("myt,y12");

	if ((!y12 && !of_machine_is_compatible("myt,y4")) || pdev->devfn ||
	    !bus->self || !pci_is_pcie(bus->self) ||
	    pci_pcie_type(bus->self) != PCI_EXP_TYPE_ROOT_PORT)
		return;

	while (bus->parent)
		bus = bus->parent;
	node = pci_bus_to_OF_node(bus);
	if (!node || !of_device_is_compatible(node, "cix,sky1-pcie-host") ||
	    of_property_read_u32(node, "sky1,pcie-ctrl-id", &controller))
		return;

	if (pdev->device == 0x8125 && controller == 3)
		name = y12 ? "y12lan" : "y4lan";
	else if (pdev->device == 0x8127 && controller == 4)
		name = y12 ? "y12sfp1" : "y4sfp1";
	else if (pdev->device == 0x8127 && controller == 2)
		name = y12 ? "y12sfp2" : "y4sfp2";

	if (name) {
		strscpy(dev->name, name, sizeof(dev->name));
		dev->name_assign_type = NET_NAME_PREDICTABLE;
	}
}

#endif
