// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * Apple T6041 (J616s) external display enable gate
 *
 * The J616s device tree describes the external display processors (dcpext0
 * behind the HDMI port, dcpext1 behind the Type-C ports), the DISPEXT power
 * domains with their PMP requests, ATC3, the display crossbars, the DARTs and
 * the Type-C display routes with every node disabled. Like the T6030 gate
 * (t6030-display-gate.c) this one enables them together, before platform
 * devices are created, so that
 *  - the genpd parents in the device tree order the power-on: the PMP
 *    acknowledgement (pmp-dispextN) and FAB7_SOC before DISPEXTN_SYS, which
 *    hangs the machine when touched without them;
 *  - the display DARTs are in the display subsystem's iommus from the start
 *    (its IOMMU group is built when the device is created);
 *  - the Type-C routes are available together with dcpext1, so the port
 *    controllers wait for their mode switches instead of missing them.
 *
 * Built in, it does nothing unless apple_t6041_display.enable=1 is on the
 * kernel command line (m4.safe also keeps it off). Built as a module (the
 * boot-safe way to try it: m4-boot-mark's boot-pending mark is set by then, so a
 * reset sends the next boot to the rescue kernel), loading it with enable=1 does
 * the same at run time: the display subsystem is taken down first and brought
 * back with the display DARTs in its iommus (appledrm must not be loaded yet).
 * Either way the machine must be a J616s whose nodes are all there and still
 * disabled; otherwise the tree is left as it is and the userspace steps
 * (m4-extdisp-steps) remain the way to enable them.
 */

#define pr_fmt(fmt) "apple-t6041-display: " fmt

#include <linux/init.h>
#include <linux/kstrtox.h>
#include <linux/delay.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/printk.h>
#include <linux/slab.h>
#include <linux/string.h>

static unsigned int gate_stage_delay_ms __initdata = 3000;

#ifdef MODULE
static bool gate_requested;
module_param_named(enable, gate_requested, bool, 0444);
MODULE_PARM_DESC(enable, "enable the J616s external display chain now");
#else
static bool gate_requested __initdata;

static int __init gate_setup(char *arg)
{
	bool v;

	if (!kstrtobool(arg, &v))
		gate_requested = v;
	return 1;
}
__setup("apple_t6041_display.enable=", gate_setup);
#endif

/*
 * Two stages, each with its own display DART (a DART still disabled in the
 * display subsystem's iommus would make of_iommu drop its whole fwspec): the
 * HDMI chain, then the Type-C chain. Run at the same time the two processors
 * crashed (dcpext0: axi_rd_err), as they never did one after the other.
 * Parents first; the genpd links in the tree do the real ordering.
 */
struct gate_stage {
	const char *dart;		/* display DART of this stage */
	const char *const *nodes;
	unsigned int nr;
};

static const char *const gate_hdmi_nodes[] __initconst = {
	/* ATC3 power, PMP DISPEXT0 request, DISPEXT0, AOP pins, PHY, crossbar, DARTs, mailbox, dcpext0 */
	"/power-management@502800000/power-controller@408",
	"/power-management@508300000/power-controller@e0",
	"/power-management@508300000/power-controller@168",
	"/power-management@508300000/power-controller@b8",
	"/power-management@508300000/power-controller@148",
	"/pmp-report@5003c0000/report@11",
	"/power-management@502800000/power-controller@340",
	"/power-management@502800000/power-controller@348",
	"/pinctrl@510824000",
	"/mailbox@39b148000",
	"/phy@39aa90000",
	"/mux@39b24c000",
	"/iommu@3a2340000",
	"/iommu@3a2300000",
	"/mailbox@3a2e08000",
	"/dcp@3a2e00000",
};

static const char *const gate_usbc_nodes[] __initconst = {
	/* PMP DISPEXT1 request, FAB7_SOC, DISPEXT1, crossbars, DARTs, mailbox, routes, dcpext1 */
	"/pmp-report@5003c0000/report@12",
	"/power-management@502800000/power-controller@488",
	"/power-management@502800000/power-controller@490",
	"/power-management@502800000/power-controller@498",
	"/mux@38324c000",
	"/mux@38b24c000",
	"/mux@39324c000",
	"/iommu@602340000",
	"/iommu@602300000",
	"/mailbox@602e08000",
	"/dcp@602e00000/typec-routes/route@0",
	"/dcp@602e00000/typec-routes/route@1",
	"/dcp@602e00000/typec-routes/route@2",
	"/dcp@602e00000",
};

static const struct gate_stage gate_stages[] __initconst = {
	{ "/iommu@3a2300000", gate_hdmi_nodes, ARRAY_SIZE(gate_hdmi_nodes) },
	{ "/iommu@602300000", gate_usbc_nodes, ARRAY_SIZE(gate_usbc_nodes) },
};

static int __init gate_display_add_dart(struct of_changeset *cs, struct device_node *display,
					struct device_node *dart)
{
	struct property *old, *prop;
	__be32 *value;

	old = of_find_property(display, "iommus", NULL);
	if (!old || !dart->phandle || old->length % (2 * sizeof(*value)))
		return -EINVAL;
	prop = kzalloc(sizeof(*prop), GFP_KERNEL);
	value = kmalloc(old->length + 2 * sizeof(*value), GFP_KERNEL);
	if (!prop || !value) {
		kfree(prop);
		kfree(value);
		return -ENOMEM;
	}
	memcpy(value, old->value, old->length);
	value[old->length / sizeof(*value)] = cpu_to_be32(dart->phandle);
	value[old->length / sizeof(*value) + 1] = cpu_to_be32(0);
	prop->name = kstrdup("iommus", GFP_KERNEL);
	prop->length = old->length + 2 * sizeof(*value);
	prop->value = value;
	if (!prop->name) {
		kfree(value);
		kfree(prop);
		return -ENOMEM;
	}
	return of_changeset_update_property(cs, display, prop);
}

/* Every node of @st is there and still disabled. */
static bool __init gate_stage_ready(const struct gate_stage *st)
{
	struct device_node *np;
	unsigned int i;

	for (i = 0; i < st->nr; i++) {
		np = of_find_node_by_path(st->nodes[i]);
		if (!np) {
			pr_err("%s: no such node, nothing enabled\n", st->nodes[i]);
			return false;
		}
		if (of_device_is_available(np)) {
			pr_err("%s: already enabled, nothing changed\n", st->nodes[i]);
			of_node_put(np);
			return false;
		}
		of_node_put(np);
	}
	return true;
}

/* Enable @st's nodes and put its display DART into the display subsystem's iommus. */
static int __init gate_stage_apply(const struct gate_stage *st, struct device_node *display)
{
	struct device_node *np;
	struct of_changeset *cs;
	unsigned int i;
	int ret;

#ifdef MODULE
	/* Its IOMMU group is built when the device is created: re-create it. */
	if (of_device_is_available(display)) {
		struct of_changeset down;

		of_changeset_init(&down);
		ret = of_changeset_update_prop_string(&down, display, "status", "disabled");
		if (!ret)
			ret = of_changeset_apply(&down);
		of_changeset_destroy(&down);
		if (ret)
			return ret;
	}
#endif
	cs = kzalloc(sizeof(*cs), GFP_KERNEL);
	if (!cs)
		return -ENOMEM;
	of_changeset_init(cs);

	np = of_find_node_by_path(st->dart);
	ret = np ? gate_display_add_dart(cs, display, np) : -ENODEV;
	of_node_put(np);
	for (i = 0; !ret && i < st->nr; i++) {
		np = of_find_node_by_path(st->nodes[i]);
		ret = np ? of_changeset_update_prop_string(cs, np, "status", "okay") : -ENODEV;
		of_node_put(np);
	}
#ifdef MODULE
	/* Last, so the display subsystem is created with the DART in its iommus. */
	if (!ret)
		ret = of_changeset_update_prop_string(cs, display, "status", "okay");
#endif
	if (!ret)
		ret = of_changeset_apply(cs);
	if (ret) {
		of_changeset_destroy(cs);
		kfree(cs);
	}
	/* Applied, it stays: the live tree refers to it. */
	return ret;
}

static int __init t6041_display_gate_init(void)
{
	struct device_node *display;
	unsigned int i;
	int ret = 0;

	if (!gate_requested)
		return 0;
#ifndef MODULE
	if (strstr(saved_command_line, "m4.safe")) {
		pr_info("safe mode, external displays stay disabled\n");
		return 0;
	}
#endif
	if (!of_machine_is_compatible("apple,j616s"))
		return 0;

	/* Everything must be there and disabled before anything is changed. */
	for (i = 0; i < ARRAY_SIZE(gate_stages); i++)
		if (!gate_stage_ready(&gate_stages[i]))
			return 0;
	display = of_find_node_by_path("/display-subsystem");
	if (!display) {
		pr_err("no display subsystem, nothing enabled\n");
		return 0;
	}

	for (i = 0; !ret && i < ARRAY_SIZE(gate_stages); i++) {
		ret = gate_stage_apply(&gate_stages[i], display);
		if (!ret)
			pr_info("stage %u enabled (%u nodes)\n", i, gate_stages[i].nr);
		/* The processors came up one after the other in the steps, too. */
		if (!ret && i + 1 < ARRAY_SIZE(gate_stages))
			msleep(gate_stage_delay_ms);
	}
	if (ret)
		pr_err("could not enable the external displays: %d\n", ret);
	of_node_put(display);
	return 0;
}
#ifdef MODULE
module_init(t6041_display_gate_init);
MODULE_DESCRIPTION("Apple T6041 (J616s) external display enable gate");
MODULE_LICENSE("Dual MIT/GPL");
#else
arch_initcall(t6041_display_gate_init);
#endif
