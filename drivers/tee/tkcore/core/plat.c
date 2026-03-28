#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/err.h>
#include <linux/of.h>
#include <linux/topology.h>
#include <linux/clk.h>
#include <linux/tee_clkmgr.h>
#include <linux/version.h>
#include "tee_core_priv.h"

// support 64 cores at the most
static bool large_cpus[64] = { };

bool plat_cpu_is_big(int cpu)
{
	return large_cpus[cpu];
}

#if IS_ENABLED(CONFIG_TRUSTKERNEL_TEE_FP_SUPPORT)

#if IS_ENABLED(CONFIG_ARCH_MEDIATEK) || IS_ENABLED(CONFIG_MEDIATEK_SOLUTION)

#if 0
static struct clkmgr_handle *try_spi_clk(uint32_t token)
{
	struct clk *clk;
	struct spi_controller *master;
	struct clkmgr_handle *h;

	uint32_t busnum = TEE_CLKMGR_TOKEN_ID(token);

	h = kmalloc(sizeof(struct clkmgr_handle), GFP_KERNEL);
	if (h == NULL) {
		return NULL;
	}

	memset(h, 0, sizeof(*h));
	h->token = token;

	master = spi_busnum_to_master(busnum);
	if (master == NULL) {
		pr_warn("tkcoredrv: spi%u not found\n",
			busnum);
		return h;
	}

	clk = devm_clk_get(master->dev.parent, "spi-clk");
	put_device(&master->dev);

	if (IS_ERR(clk)) {
		pr_warn("tkcoredrv: failed to get spi-clk: %d\n",
			PTR_ERR(clk));
		return h;
	}

	h->token = token;
	h->e = (void *) &clk_prepare_enable;
	h->d = (void *) &clk_disable_unprepare;
	h->p0 = (const void *) clk;
	h->argnum = 1;

	return h;
}
#endif

static void mtk_enable_clk(void *p)
{
	clk_prepare_enable((struct clk *) p);
}

static void mtk_disable_clk(void *p)
{
	clk_disable_unprepare((struct clk *) p);
}

int plat_register_spi_clk(void)
{
	int i;
	char spi_devname[10];

	pr_info("register platform spi clk");

	/* support 20 spi device clocks at the most */
	for (i = 0; i < 20; i++) {
		struct device_node *spi_node;
		struct clk *clk;

		sprintf(spi_devname, "spi%d", i);
		spi_node = of_find_node_by_name(NULL, spi_devname);
		if (spi_node == NULL)
			break;

		/* match clock name in platform dts */
		clk = of_clk_get_by_name(spi_node, "spi-clk");
		if (!IS_ERR_OR_NULL(clk)) {
			tee_clkmgr_register1("spi", i,
					mtk_enable_clk, mtk_disable_clk, clk);
		} else {
			pr_warn("of_clk_get_by_name: failed with %ld", PTR_ERR(clk));
		}

		of_node_put(spi_node);
	}

	return 0;
}

#else

int plat_register_spi_clk(void)
{
	return 0;
}

#endif

#endif

int plat_tee_init(void)
{
	int cpu;
	int max_package_id = -1, min_package_id = 64;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 16, 0)
	int max_cluster_id = -1, min_cluster_id = 64;
#endif

	for_each_possible_cpu(cpu) {
		int package_id = topology_physical_package_id(cpu);
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 16, 0)
		int cluster_id = topology_cluster_id(cpu);
#endif

		max_package_id = max(package_id, max_package_id);
		min_package_id = min(package_id, min_package_id);

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 16, 0)
		max_cluster_id = max(cluster_id, max_cluster_id);
		min_cluster_id = min(cluster_id, min_cluster_id);
#endif
	}

	if (max_package_id != -1 && min_package_id != max_package_id) {
		for_each_possible_cpu(cpu) {
			if (topology_physical_package_id(cpu) == max_package_id) {
				large_cpus[cpu] = true;
			}
		}

		// we consider CPUs that reside on the max package to be big CPUs
		pr_info("tkcore: big package_id = %d\n", max_package_id);
		return 0;
	}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 16, 0)
	if (max_cluster_id != -1 && min_cluster_id != max_cluster_id) {
		for_each_possible_cpu(cpu) {
			if (topology_cluster_id(cpu) == max_cluster_id) {
				large_cpus[cpu] = true;
			}
		}

		// we consider CPUs that reside on the max cluster to be big CPUs
		pr_info("tkcore: big cluster_id = %d\n", max_cluster_id);
		return 0;
	}
#endif

	pr_info("tkcore: symmetric arch: no big cpus\n");
	return 0;
}
