/*****************************************************************************
* Copyright 2011-2012 Broadcom Corporation.  All rights reserved.
*
* Unless you and Broadcom execute a separate written software license
* agreement governing use of this software, this software is licensed to you
* under the terms of the GNU General Public License version 2, available at
* http://www.broadcom.com/licenses/GPLv2.php (the "GPL").
*
* Notwithstanding the above, under no circumstances may you combine this
* software in any way with any other Broadcom software provided under a
* license other than the GPL, without Broadcom's express prior written
* consent.
*****************************************************************************/

/* ---- Include Files ----------------------------------------------------- */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/types.h>
#include <linux/errno.h>
#include <linux/cdev.h>
#include <linux/mm.h>
#include <linux/fs.h>
#include <linux/device.h>
#include <linux/pagemap.h>
#include <linux/slab.h>
#include <linux/ioctl.h>
#include <linux/semaphore.h>
#include <linux/proc_fs.h>
#include <linux/dma-mapping.h>
#include <linux/pfn.h>
#include <linux/hugetlb.h>
#include <linux/seq_file.h>
#include <linux/list.h>

#include <asm/cacheflush.h>

#include <vc_mem.h>

#include "vchiq_connected.h"
#include "vc_vchi_sm.h"

#include <vmcs_sm_ioctl.h>
#include "vc_sm_knl.h"

/* ---- Private Constants and Types --------------------------------------- */

/* Logging macros */
#define LOG_DBG(exp, fmt, ...) \
	do { if (exp) \
		printk(KERN_DEBUG fmt "\n", ##__VA_ARGS__); } while (0)
#define LOG_INFO(fmt, ...)      printk(KERN_INFO fmt "\n", ##__VA_ARGS__)
#define LOG_ERR(fmt, ...)       printk(KERN_ERR  fmt "\n", ##__VA_ARGS__)

#define LOG_DBG_LEVEL_MIN          1
#define LOG_DBG_LEVEL_INTER_1      2
#define LOG_DBG_LEVEL_MAX          3

#define DEVICE_NAME              "vcsm"
#define DEVICE_MINOR             0

#define PROC_DIR_ROOT_NAME       "vc-smem"
#define PROC_DIR_ALLOC_NAME      "alloc"
#define PROC_STATE               "state"
#define PROC_STATS               "statistics"
#define PROC_RESOURCES           "resources"
#define PROC_DEBUG               "debug"
#define PROC_WRITE_BUF_SIZE      128

/* Statistics tracked per resource and globally.
*/
enum SM_STATS_T {
	/* Attempt. */
	ALLOC,
	FREE,
	LOCK,
	UNLOCK,
	MAP,
	FLUSH,
	INVALID,

	END_ATTEMPT,

	/* Failure. */
	ALLOC_FAIL,
	FREE_FAIL,
	LOCK_FAIL,
	UNLOCK_FAIL,
	MAP_FAIL,
	FLUSH_FAIL,
	INVALID_FAIL,

	END_ALL,

};

static const char *const sm_stats_human_read[] = {
	"Alloc",
	"Free",
	"Lock",
	"Unlock",
	"Map",
	"Cache Flush",
	"Cache Invalidate",
};

typedef int (*PROC_ENTRY_READ) (struct seq_file * s);
struct SM_PDE_T {
	PROC_ENTRY_READ proc_read;	/* Proc read function hookup. */
	struct proc_dir_entry *dir_entry;	/* Proc directory entry. */
	void *priv_data;	/* Private data associated with PDE. */

};

/* Single resource allocation tracked for all devices.
*/
struct sm_mmap {
	struct list_head map_list;	/* Linked list of maps. */

	struct SM_RESOURCE_T *resource;	/* Pointer to the resource. */

	pid_t res_pid;		/* PID owning that resource. */
	unsigned int res_vc_hdl;	/* Resource handle (videocore). */
	unsigned int res_usr_hdl;	/* Resource handle (user). */

	long unsigned int res_addr;	/* Mapped virtual address. */
	struct vm_area_struct *vma;	/* VM area for this mapping. */
	unsigned int ref_count;	/* Reference count to this vma. */

	/* Used to link maps associated with a resource. */
	struct list_head resource_map_list;
};

/* Single resource allocation tracked for each opened device.
*/
struct SM_RESOURCE_T {
	struct list_head resource_list;	/* List of resources. */
	struct list_head global_resource_list;	/* Global list of resources. */

	pid_t pid;		/* PID owning that resource. */
	uint32_t res_guid;	/* Unique identifier. */
	uint32_t lock_count;	/* Lock count for this resource. */
	uint32_t ref_count;	/* Ref count for this resource. */

	uint32_t res_handle;	/* Resource allocation handle. */
	void *res_base_mem;	/* Resource base memory address. */
	uint32_t res_size;	/* Resource size allocated. */
	enum vmcs_sm_cache_e res_cached;	/* Resource cache type. */
	struct SM_RESOURCE_T *res_shared;	/* Shared resource */

	enum SM_STATS_T res_stats[END_ALL];	/* Resource statistics. */

	uint8_t map_count;	/* Counter of mappings for this resource. */
	struct list_head map_list;	/* Maps associated with a resource. */

	struct SM_PRIV_DATA_T *private;
};

/* Private file data associated with each opened device.
*/
struct SM_PRIV_DATA_T {
	struct list_head resource_list;	/* List of resources. */

	pid_t pid;		/* PID of creator. */

	struct proc_dir_entry *dir_pid;	/* Proc entries root. */
	struct SM_PDE_T dir_stats;	/* Proc entries statistics sub-tree. */
	struct SM_PDE_T dir_res;	/* Proc entries resource sub-tree. */

	int restart_sys;	/* Tracks restart on interrupt. */
	VC_SM_MSG_TYPE int_action;	/* Interrupted action. */
	uint32_t int_trans_id;	/* Interrupted transaction. */

};

/* Global state information.
*/
struct SM_STATE_T {
	VC_VCHI_SM_HANDLE_T sm_handle;	/* Handle for videocore service. */
	struct proc_dir_entry *dir_root;	/* Proc entries root. */
	struct proc_dir_entry *dir_alloc;	/* Proc entries allocations. */
	struct SM_PDE_T dir_stats;	/* Proc entries statistics sub-tree. */
	struct SM_PDE_T dir_state;	/* Proc entries state sub-tree. */
	struct proc_dir_entry *debug;	/* Proc entries debug. */

	struct mutex map_lock;	/* Global map lock. */
	struct list_head map_list;	/* List of maps. */
	struct list_head resource_list;	/* List of resources. */

	enum SM_STATS_T deceased[END_ALL];	/* Natural termination stats. */
	enum SM_STATS_T terminated[END_ALL];	/* Forced termination stats. */
	uint32_t res_deceased_cnt;	/* Natural termination counter. */
	uint32_t res_terminated_cnt;	/* Forced termination counter. */

	struct cdev sm_cdev;	/* Device. */
	dev_t sm_devid;		/* Device identifier. */
	struct class *sm_class;	/* Class. */
	struct device *sm_dev;	/* Device. */

	struct SM_PRIV_DATA_T *data_knl;	/* Kernel internal data tracking. */

	struct mutex lock;	/* Global lock. */
	uint32_t guid;		/* GUID (next) tracker. */

};

/* ---- Private Variables ----------------------------------------------- */

static struct SM_STATE_T *sm_state;
static unsigned int sm_debug_log;
static int sm_inited;

static const char *const sm_cache_map_vector[] = {
	"(null)",
	"host",
	"videocore",
	"host+videocore",
};

/* ---- Private Function Prototypes -------------------------------------- */

/* ---- Private Functions ------------------------------------------------ */

static inline unsigned vcaddr_to_pfn(unsigned long vc_addr)
{
	unsigned long pfn = vc_addr & 0x3FFFFFFF;
	pfn += mm_vc_mem_phys_addr;
	pfn >>= PAGE_SHIFT;
	return pfn;
}

/* Carries over to the state statistics the statistics once owned by a deceased
** resource.
*/
static void vc_sm_resource_deceased(struct SM_RESOURCE_T *p_res, int terminated)
{
	if (sm_state != NULL) {
		if (p_res != NULL) {
			int ix;

			if (terminated)
				sm_state->res_terminated_cnt++;
			else
				sm_state->res_deceased_cnt++;

			for (ix = 0; ix < END_ALL; ix++) {
				if (terminated)
					sm_state->terminated[ix] +=
					    p_res->res_stats[ix];
				else
					sm_state->deceased[ix] +=
					    p_res->res_stats[ix];
			}
		}
	}
}

/* Fetch a videocore handle corresponding to a mapping of the pid+address
** returns 0 (ie NULL) if no such handle exists in the global map.
*/
static unsigned int vmcs_sm_vc_handle_from_pid_and_address(unsigned int pid,
							   unsigned int addr)
{
	struct sm_mmap *map = NULL;
	unsigned int handle = 0;

	if (sm_state == NULL || addr == 0)
		goto out;

	mutex_lock(&(sm_state->map_lock));

	/* Lookup the resource.
	 */
	if (!list_empty(&sm_state->map_list)) {
		list_for_each_entry(map, &sm_state->map_list, map_list) {
			if (map->res_pid != pid || map->res_addr != addr)
				continue;

			LOG_DBG((sm_debug_log >= LOG_DBG_LEVEL_MAX),
				"[%s]: global map %p (pid %u, addr %lx) -> vc-hdl %x (usr-hdl %x)",
				__func__, map, map->res_pid, map->res_addr,
				map->res_vc_hdl, map->res_usr_hdl);

			handle = map->res_vc_hdl;
			break;
		}
	}

	mutex_unlock(&(sm_state->map_lock));

out:
	/* Use a debug log here as it may be a valid situation that we query
	 ** for something that is not mapped, we do not want a kernel log each
	 ** time around.
	 **
	 ** There are other error log that would pop up accordingly if someone
	 ** subsequently tries to use something invalid after being told not to
	 ** use it...
	 */
	if (handle == 0) {
		LOG_DBG((sm_debug_log >= LOG_DBG_LEVEL_MAX),
			"[%s]: not a valid map (pid %u, addr %x)",
			__func__, pid, addr);
	}

	return handle;
}

/* Fetch a user handle corresponding to a mapping of the pid+address
** returns 0 (ie NULL) if no such handle exists in the global map.
*/
static unsigned int vmcs_sm_usr_handle_from_pid_and_address(unsigned int pid,
							    unsigned int addr)
{
	struct sm_mmap *map = NULL;
	unsigned int handle = 0;

	if (sm_state == NULL || addr == 0)
		goto out;

	mutex_lock(&(sm_state->map_lock));

	/* Lookup the resource.
	 */
	if (!list_empty(&sm_state->map_list)) {
		list_for_each_entry(map, &sm_state->map_list, map_list) {
			if (map->res_pid != pid || map->res_addr != addr)
				continue;

			LOG_DBG((sm_debug_log >= LOG_DBG_LEVEL_MAX),
				"[%s]: global map %p (pid %u, addr %lx) -> usr-hdl %x (vc-hdl %x)",
				__func__, map, map->res_pid, map->res_addr,
				map->res_usr_hdl, map->res_vc_hdl);

			handle = map->res_usr_hdl;
			break;
		}
	}

	mutex_unlock(&(sm_state->map_lock));

out:
	/* Use a debug log here as it may be a valid situation that we query
	 * for something that is not mapped yet.
	 *
	 * There are other error log that would pop up accordingly if someone
	 * subsequently tries to use something invalid after being told not to
	 * use it...
	 */
	if (handle == 0)
		LOG_DBG((sm_debug_log >= LOG_DBG_LEVEL_MAX),
			"[%s]: not a valid map (pid %u, addr %x)",
			__func__, pid, addr);

	return handle;
}

#if defined(DO_NOT_USE)
/* Fetch an address corresponding to a mapping of the pid+handle
** returns 0 (ie NULL) if no such address exists in the global map.
*/
static unsigned int vmcs_sm_usr_address_from_pid_and_vc_handle(unsigned int pid,
							       unsigned int hdl)
{
	struct sm_mmap *map = NULL;
	unsigned int addr = 0;

	if (sm_state == NULL || hdl == 0)
		goto out;

	mutex_lock(&(sm_state->map_lock));

	/* Lookup the resource.
	 */
	if (!list_empty(&sm_state->map_list)) {
		list_for_each_entry(map, &sm_state->map_list, map_list) {
			if (map->res_pid != pid || map->res_vc_hdl != hdl)
				continue;

			LOG_DBG((sm_debug_log >= LOG_DBG_LEVEL_MAX),
				"[%s]: global map %p (pid %u, vc-hdl %x, usr-hdl %x) -> addr %lx",
				__func__, map, map->res_pid, map->res_vc_hdl,
				map->res_usr_hdl, map->res_addr);

			addr = map->res_addr;
			break;
		}
	}

	mutex_unlock(&(sm_state->map_lock));

out:
	/* Use a debug log here as it may be a valid situation that we query
	 ** for something that is not mapped, we do not want a kernel log each
	 ** time around.
	 **
	 ** There are other error log that would pop up accordingly if someone
	 ** subsequently tries to use something invalid after being told not to
	 ** use it...
	 */
	if (addr == 0)
		LOG_DBG((sm_debug_log >= LOG_DBG_LEVEL_MAX),
			"[%s]: not a valid map (pid %u, hdl %x)",
			__func__, pid, hdl);

	return addr;
}
#endif

/* Fetch an address corresponding to a mapping of the pid+handle
** returns 0 (ie NULL) if no such address exists in the global map.
*/
static unsigned int vmcs_sm_usr_address_from_pid_and_usr_handle(unsigned int
								pid,
								unsigned int
								hdl)
{
	struct sm_mmap *map = NULL;
	unsigned int addr = 0;

	if (sm_state == NULL || hdl == 0)
		goto out;

	mutex_lock(&(sm_state->map_lock));

	/* Lookup the resource.
	 */
	if (!list_empty(&sm_state->map_list)) {
		list_for_each_entry(map, &sm_state->map_list, map_list) {
			if (map->res_pid != pid || map->res_usr_hdl != hdl)
				continue;

			LOG_DBG((sm_debug_log >= LOG_DBG_LEVEL_MAX),
				"[%s]: global map %p (pid %u, vc-hdl %x, usr-hdl %x) -> addr %lx",
				__func__, map, map->res_pid, map->res_vc_hdl,
				map->res_usr_hdl, map->res_addr);

			addr = map->res_addr;
			break;
		}
	}

	mutex_unlock(&(sm_state->map_lock));

out:
	/* Use a debug log here as it may be a valid situation that we query
	 * for something that is not mapped, we do not want a kernel log each
	 * time around.
	 *
	 * There are other error log that would pop up accordingly if someone
	 * subsequently tries to use something invalid after being told not to
	 * use it...
	 */
	if (addr == 0)
		LOG_DBG((sm_debug_log >= LOG_DBG_LEVEL_MAX),
			"[%s]: not a valid map (pid %u, hdl %x)",
			__func__, pid, hdl);

	return addr;
}

/* Adds a resource mapping to the global data list.
*/
static void vmcs_sm_add_map(struct SM_STATE_T *state,
			    struct SM_RESOURCE_T *resource, struct sm_mmap *map)
{
	mutex_lock(&(state->map_lock));

	/* Add to the global list of mappings
	 */
	list_add(&map->map_list, &state->map_list);

	/* Add to the list of mappings for this resource
	 */
	list_add(&map->resource_map_list, &resource->map_list);
	resource->map_count++;

	mutex_unlock(&(state->map_lock));

	LOG_DBG((sm_debug_log >= LOG_DBG_LEVEL_INTER_1),
		"[%s]: added map %p (pid %u, vc-hdl %x, usr-hdl %x, addr %lx)",
		__func__, map, map->res_pid, map->res_vc_hdl,
		map->res_usr_hdl, map->res_addr);
}

/* Removes a resource mapping from the global data list.
*/
static void vmcs_sm_remove_map(struct SM_STATE_T *state,
			       struct SM_RESOURCE_T *resource,
			       struct sm_mmap *map)
{
	mutex_lock(&(state->map_lock));

	/* Remove from the global list of mappings
	 */
	list_del(&map->map_list);

	/* Remove from the list of mapping for this resource
	 */
	list_del(&map->resource_map_list);
	if (resource->map_count > 0)
		resource->map_count--;

	mutex_unlock(&(state->map_lock));

	LOG_DBG((sm_debug_log >= LOG_DBG_LEVEL_INTER_1),
		"[%s]: removed map %p (pid %d, vc-hdl %x, usr-hdl %x, addr %lx)",
		__func__, map, map->res_pid, map->res_vc_hdl,
		map->res_usr_hdl, map->res_addr);

	kfree(map);
}

/* Read callback for the debug proc entry.
*/
static int vc_sm_debug_proc_read(char *buffer,
				 char **start,
				 off_t off, int count, int *eof, void *data)
{
	int len = 0;

	len += sprintf(buffer + len,
		       "debug log level set to %u\n",
		       (unsigned int)sm_debug_log);
	len += sprintf(buffer + len,
		       "level is one increment in [0 (disabled), %u (highest)]\n",
		       LOG_DBG_LEVEL_MAX);

	return len;
}

/* Read callback for the global state proc entry.
*/
static int vc_sm_global_state_proc_read(struct seq_file *s)
{
	struct sm_mmap *map = NULL;
	int map_count = 0;

	if (sm_state == NULL)
		return 0;

	seq_printf(s, "\nVC-ServiceHandle     0x%x\n",
		   (unsigned int)sm_state->sm_handle);

	/* Log all applicable mapping(s).
	 */

	mutex_lock(&(sm_state->map_lock));

	if (!list_empty(&sm_state->map_list)) {
		list_for_each_entry(map, &sm_state->map_list, map_list) {
			map_count++;

			seq_printf(s, "\nMapping                0x%x\n",
				   (unsigned int)map);
			seq_printf(s, "           TGID        %u\n",
				   map->res_pid);
			seq_printf(s, "           VC-HDL      0x%x\n",
				   map->res_vc_hdl);
			seq_printf(s, "           USR-HDL     0x%x\n",
				   map->res_usr_hdl);
			seq_printf(s, "           USR-ADDR    0x%lx\n",
				   map->res_addr);
		}
	}

	mutex_unlock(&(sm_state->map_lock));

	seq_printf(s, "\n\nTotal map count:   %d\n\n", map_count);

	return 0;
}

static int vc_sm_global_statistics_proc_read(struct seq_file *s)
{
	int ix;

	/* Global state tracked statistics.
	 */
	if (sm_state != NULL) {
		seq_printf(s, "\nDeceased Resources Statistics\n");

		seq_printf(s, "\nNatural Cause (%u occurences)\n",
			   sm_state->res_deceased_cnt);
		for (ix = 0; ix < END_ATTEMPT; ix++) {
			if (sm_state->deceased[ix] > 0) {
				seq_printf(s, "                %u\t%s\n",
					   sm_state->deceased[ix],
					   sm_stats_human_read[ix]);
			}
		}
		seq_printf(s, "\n");
		for (ix = 0; ix < END_ATTEMPT; ix++) {
			if (sm_state->deceased[ix + END_ATTEMPT] > 0) {
				seq_printf(s, "                %u\tFAILED %s\n",
					   sm_state->deceased[ix + END_ATTEMPT],
					   sm_stats_human_read[ix]);
			}
		}

		seq_printf(s, "\nForcefull (%u occurences)\n",
			   sm_state->res_terminated_cnt);
		for (ix = 0; ix < END_ATTEMPT; ix++) {
			if (sm_state->terminated[ix] > 0) {
				seq_printf(s, "                %u\t%s\n",
					   sm_state->terminated[ix],
					   sm_stats_human_read[ix]);
			}
		}
		seq_printf(s, "\n");
		for (ix = 0; ix < END_ATTEMPT; ix++) {
			if (sm_state->terminated[ix + END_ATTEMPT] > 0) {
				seq_printf(s, "                %u\tFAILED %s\n",
					   sm_state->terminated[ix +
								END_ATTEMPT],
					   sm_stats_human_read[ix]);
			}
		}
	}

	return 0;
}

/* Read callback for the statistics proc entry.
*/
static int vc_sm_statistics_proc_read(struct seq_file *s)
{
	int ix;
	struct SM_PRIV_DATA_T *file_data;
	struct SM_RESOURCE_T *resource;
	int res_count = 0;
	struct SM_PDE_T *p_pde;

	p_pde = (struct SM_PDE_T *)(s->private);
	file_data = (struct SM_PRIV_DATA_T *)(p_pde->priv_data);

	if (file_data == NULL)
		return 0;

	/* Per process statistics.
	 */

	seq_printf(s, "\nStatistics for TGID %d\n", file_data->pid);

	mutex_lock(&(sm_state->map_lock));

	if (!list_empty(&file_data->resource_list)) {
		list_for_each_entry(resource, &file_data->resource_list,
				    resource_list) {
			res_count++;

			seq_printf(s, "\nGUID:         0x%x\n\n",
				   resource->res_guid);
			for (ix = 0; ix < END_ATTEMPT; ix++) {
				if (resource->res_stats[ix] > 0) {
					seq_printf(s,
						   "                %u\t%s\n",
						   resource->res_stats[ix],
						   sm_stats_human_read[ix]);
				}
			}
			seq_printf(s, "\n");
			for (ix = 0; ix < END_ATTEMPT; ix++) {
				if (resource->res_stats[ix + END_ATTEMPT] > 0) {
					seq_printf(s,
						   "                %u\tFAILED %s\n",
						   resource->res_stats[ix +
								       END_ATTEMPT],
						   sm_stats_human_read[ix]);
				}
			}
		}
	}

	mutex_unlock(&(sm_state->map_lock));

	seq_printf(s, "\nResources Count %d\n", res_count);

	return 0;
}

/* Read callback for the allocation proc entry.
*/
static int vc_sm_alloc_proc_read(struct seq_file *s)
{
	struct SM_PRIV_DATA_T *file_data;
	struct SM_RESOURCE_T *resource;
	int alloc_count = 0;
	struct SM_PDE_T *p_pde;

	p_pde = (struct SM_PDE_T *)(s->private);
	file_data = (struct SM_PRIV_DATA_T *)(p_pde->priv_data);

	if (file_data == NULL)
		return 0;

	/* Per process statistics.
	 */

	seq_printf(s, "\nAllocation for TGID %d\n", file_data->pid);

	mutex_lock(&(sm_state->map_lock));

	if (!list_empty(&file_data->resource_list)) {
		list_for_each_entry(resource, &file_data->resource_list,
				    resource_list) {
			alloc_count++;

			seq_printf(s, "\nGUID:              0x%x\n",
				   resource->res_guid);
			seq_printf(s, "Lock Count:        %u\n",
				   resource->lock_count);
			seq_printf(s, "Mapped:            %s\n",
				   (resource->map_count ? "yes" : "no"));
			seq_printf(s, "VC-handle:         0x%x\n",
				   resource->res_handle);
			seq_printf(s, "VC-address:        0x%p\n",
				   resource->res_base_mem);
			seq_printf(s, "VC-size (bytes):   %u\n",
				   resource->res_size);
			seq_printf(s, "Cache:             %s\n",
				   sm_cache_map_vector[resource->res_cached]);
		}
	}

	mutex_unlock(&(sm_state->map_lock));

	seq_printf(s, "\n\nTotal allocation count: %d\n\n", alloc_count);

	return 0;
}

/* Write callback for the debug proc entry.
*/
static int vc_sm_debug_proc_write(struct file *file,
				  const char __user *buffer,
				  unsigned long count, void *data)
{
	int ret;
	uint32_t debug_value;
	unsigned char kbuf[PROC_WRITE_BUF_SIZE + 1];

	memset(kbuf, 0, PROC_WRITE_BUF_SIZE + 1);
	if (count >= PROC_WRITE_BUF_SIZE)
		count = PROC_WRITE_BUF_SIZE;

	if (copy_from_user(kbuf, buffer, count) != 0) {
		LOG_ERR("[%s]: failed to copy-from-user", __func__);

		ret = -EFAULT;
		goto out;
	}
	kbuf[count - 1] = 0;

	/* Return read value no matter what from there on.
	 */
	ret = count;

	/* coverity[secure_coding] - scanning integer, can't overflow. */
	if (sscanf(kbuf, "%u", &debug_value) != 1) {
		LOG_ERR("[%s]: echo <value> > /proc/%s/%s",
			__func__, PROC_DIR_ROOT_NAME, PROC_DEBUG);

		/* Failed to assign the proper value.
		 */
		goto out;
	}

	if (debug_value > LOG_DBG_LEVEL_MAX) {
		LOG_ERR("[%s]: echo [0,%u] > /proc/%s/%s",
			__func__,
			LOG_DBG_LEVEL_MAX, PROC_DIR_ROOT_NAME, PROC_DEBUG);

		/* Failed to assign the proper value.
		 */
		goto out;
	}

	LOG_INFO("[%s]: debug log change from level %u to level %u",
		 __func__, sm_debug_log, debug_value);
	sm_debug_log = debug_value;

	/* Done.
	 */
	goto out;

out:
	return ret;
}

static int vc_sm_seq_file_proc_read(struct seq_file *s, void *unused)
{
	struct SM_PDE_T *sm_pde;

	sm_pde = (struct SM_PDE_T *)(s->private);

	if (sm_pde && sm_pde->proc_read)
		sm_pde->proc_read(s);

	return 0;
}

static int vc_sm_single_proc_open(struct inode *inode, struct file *file)
{
	return single_open(file, vc_sm_seq_file_proc_read, PDE(inode)->data);
}

static const struct file_operations vc_sm_proc_fops = {
	.open = vc_sm_single_proc_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};

/* Adds a resource to the private data list which tracks all the allocated
** data.
*/
static void vmcs_sm_add_resource(struct SM_PRIV_DATA_T *privdata,
				 struct SM_RESOURCE_T *resource)
{
	mutex_lock(&(sm_state->map_lock));
	list_add(&resource->resource_list, &privdata->resource_list);
	list_add(&resource->global_resource_list, &sm_state->resource_list);
	mutex_unlock(&(sm_state->map_lock));

	LOG_DBG((sm_debug_log >= LOG_DBG_LEVEL_INTER_1),
		"[%s]: added resource %p (base addr %p, hdl %x, size %u, cache %u)",
		__func__, resource, resource->res_base_mem,
		resource->res_handle, resource->res_size, resource->res_cached);
}

/* Locates a resource and acquire a reference on it.
** The resource won't be deleted while there is a reference on it.
*/
static struct SM_RESOURCE_T *vmcs_sm_acquire_resource(struct SM_PRIV_DATA_T
						      *private,
						      unsigned int res_guid)
{
	struct SM_RESOURCE_T *resource, *ret = NULL;

	mutex_lock(&(sm_state->map_lock));

	list_for_each_entry(resource, &private->resource_list, resource_list) {
		if (resource->res_guid != res_guid)
			continue;

		LOG_DBG((sm_debug_log >= LOG_DBG_LEVEL_MAX),
			"[%s]: located resource %p (guid: %x, base addr %p, hdl %x, size %u, cache %u)",
			__func__, resource, resource->res_guid,
			resource->res_base_mem, resource->res_handle,
			resource->res_size, resource->res_cached);
		resource->ref_count++;
		ret = resource;
		break;
	}

	mutex_unlock(&(sm_state->map_lock));

	return ret;
}

/* Locates a resource and acquire a reference on it.
** The resource won't be deleted while there is a reference on it.
*/
static struct SM_RESOURCE_T *vmcs_sm_acquire_first_resource(struct
							    SM_PRIV_DATA_T
							    *private)
{
	struct SM_RESOURCE_T *resource, *ret = NULL;

	mutex_lock(&(sm_state->map_lock));

	list_for_each_entry(resource, &private->resource_list, resource_list) {
		LOG_DBG((sm_debug_log >= LOG_DBG_LEVEL_MAX),
			"[%s]: located resource %p (guid: %x, base addr %p, hdl %x, size %u, cache %u)",
			__func__, resource, resource->res_guid,
			resource->res_base_mem, resource->res_handle,
			resource->res_size, resource->res_cached);
		resource->ref_count++;
		ret = resource;
		break;
	}

	mutex_unlock(&(sm_state->map_lock));

	return ret;
}

/* Locates a resource and acquire a reference on it.
** The resource won't be deleted while there is a reference on it.
*/
static struct SM_RESOURCE_T *vmcs_sm_acquire_global_resource(unsigned int
							     res_guid)
{
	struct SM_RESOURCE_T *resource, *ret = NULL;

	mutex_lock(&(sm_state->map_lock));

	list_for_each_entry(resource, &sm_state->resource_list,
			    global_resource_list) {
		if (resource->res_guid != res_guid)
			continue;

		LOG_DBG((sm_debug_log >= LOG_DBG_LEVEL_MAX),
			"[%s]: located resource %p"
			" (guid: %x, base addr %p, hdl %x, size %u, cache %u)",
			__func__, resource, resource->res_guid,
			resource->res_base_mem, resource->res_handle,
			resource->res_size, resource->res_cached);
		resource->ref_count++;
		ret = resource;
		break;
	}

	mutex_unlock(&(sm_state->map_lock));

	return ret;
}

/* Release a previously acquired resource.
** The resource will be deleted when its refcount reaches 0.
*/
static void vmcs_sm_release_resource(struct SM_RESOURCE_T *resource, int force)
{
	struct SM_PRIV_DATA_T *private = resource->private;
	struct sm_mmap *map, *map_tmp;
	struct SM_RESOURCE_T *res_tmp;
	int ret;

	mutex_lock(&(sm_state->map_lock));

	if (--resource->ref_count) {
		if (force)
			LOG_ERR("[%s]: resource %p in use", __func__, resource);

		mutex_unlock(&(sm_state->map_lock));
		return;
	}

	/* Time to free the resource. Start by removing it from the list */
	list_del(&resource->resource_list);
	list_del(&resource->global_resource_list);

	/* Walk the global resource list, find out if the resource is used
	 * somewhere else. In which case we don't want to delete it.
	 */
	list_for_each_entry(res_tmp, &sm_state->resource_list,
			    global_resource_list) {
		if (res_tmp->res_handle == resource->res_handle) {
			resource->res_handle = 0;
			break;
		}
	}

	mutex_unlock(&(sm_state->map_lock));

	LOG_DBG((sm_debug_log >= LOG_DBG_LEVEL_MIN),
		"[%s]: freeing data - guid %x, hdl %x, base address %p",
		__func__, resource->res_guid, resource->res_handle,
		resource->res_base_mem);
	resource->res_stats[FREE]++;

	/* Make sure the resource we're removing is unmapped first */
	if (resource->map_count && !list_empty(&resource->map_list)) {
		down_write(&current->mm->mmap_sem);
		list_for_each_entry_safe(map, map_tmp, &resource->map_list,
					 resource_map_list) {
			ret =
			    do_munmap(current->mm, map->res_addr,
				      resource->res_size);
			if (ret) {
				LOG_ERR("[%s]: could not unmap resource %p",
					__func__, resource);
			}
		}
		up_write(&current->mm->mmap_sem);
	}

	/* Free up the videocore allocated resource.
	 */
	if (resource->res_handle) {
		VC_SM_FREE_T free = {
			resource->res_handle, resource->res_base_mem
		};
		int status = vc_vchi_sm_free(sm_state->sm_handle, &free,
					     &private->int_trans_id);
		if (status != 0 && status != -EINTR) {
			LOG_ERR
			    ("[%s]: failed to free memory on videocore"
			     " (status: %u, trans_id: %u)",
			     __func__, status, private->int_trans_id);
			resource->res_stats[FREE_FAIL]++;
			ret = -EPERM;
		}
	}

	/* Free up the shared resource.
	 */
	if (resource->res_shared)
		vmcs_sm_release_resource(resource->res_shared, 0);

	/* Free up the local resource tracking this allocation.
	 */
	vc_sm_resource_deceased(resource, force);
	kfree(resource);
}

/* Dump the map table for the driver.  If process is -1, dumps the whole table,
** if process is a valid pid (non -1) dump only the entries associated with the
** pid of interest.
*/
static void vmcs_sm_host_walk_map_per_pid(int pid)
{
	struct sm_mmap *map = NULL;

	/* Make sure the device was started properly.
	 */
	if (sm_state == NULL) {
		LOG_ERR("[%s]: invalid device", __func__);
		return;
	}

	mutex_lock(&(sm_state->map_lock));

	/* Log all applicable mapping(s).
	 */
	if (!list_empty(&sm_state->map_list)) {
		list_for_each_entry(map, &sm_state->map_list, map_list) {
			if (pid == -1 || map->res_pid == pid) {
				LOG_INFO
				    ("[%s]: tgid: %u"
				     " - vc-hdl: %x, usr-hdl: %x, usr-addr: %lx",
				     __func__, map->res_pid, map->res_vc_hdl,
				     map->res_usr_hdl, map->res_addr);
			}
		}
	}

	mutex_unlock(&(sm_state->map_lock));

	return;
}

/* Dump the allocation table from host side point of view.  This only dumps the
** data allocated for this process/device referenced by the file_data.
*/
static void vmcs_sm_host_walk_alloc(struct SM_PRIV_DATA_T *file_data)
{
	struct SM_RESOURCE_T *resource = NULL;

	/* Make sure the device was started properly.
	 */
	if ((sm_state == NULL) || (file_data == NULL)) {
		LOG_ERR("[%s]: invalid device", __func__);
		return;
	}

	mutex_lock(&(sm_state->map_lock));

	if (!list_empty(&file_data->resource_list)) {
		list_for_each_entry(resource, &file_data->resource_list,
				    resource_list) {
			LOG_INFO
			    ("[%s]: guid: %x -"
			     " hdl: %x, vc-mem: %p, size: %u, cache: %u",
			     __func__, resource->res_guid, resource->res_handle,
			     resource->res_base_mem, resource->res_size,
			     resource->res_cached);
		}
	}

	mutex_unlock(&(sm_state->map_lock));

	return;
}

/* Create support for private data tracking.
*/
static struct SM_PRIV_DATA_T *vc_sm_create_priv_data(pid_t id)
{
	char alloc_name[32];
	struct SM_PRIV_DATA_T *file_data = NULL;

	/* Allocate private structure.
	 */
	file_data = kzalloc(sizeof(*file_data), GFP_KERNEL);

	if (file_data == NULL) {
		LOG_ERR("[%s]: cannot allocate file data", __func__);
		goto out;
	}

	snprintf(alloc_name, sizeof(alloc_name), "%d", id);

	INIT_LIST_HEAD(&file_data->resource_list);
	file_data->pid = id;
	file_data->dir_pid = proc_mkdir(alloc_name, sm_state->dir_alloc);

	if (file_data->dir_pid != NULL) {
		file_data->dir_res.dir_entry = create_proc_entry(PROC_RESOURCES,
								 0,
								 file_data->dir_pid);
		if (file_data->dir_res.dir_entry == NULL) {
			LOG_ERR("[%s]: failed to create \'%s\' entry",
				__func__, alloc_name);
		} else {
			file_data->dir_res.priv_data = (void *)file_data;
			file_data->dir_res.proc_read = &vc_sm_alloc_proc_read;

			file_data->dir_res.dir_entry->proc_fops =
			    &vc_sm_proc_fops;
			file_data->dir_res.dir_entry->data =
			    &(file_data->dir_res);
		}

		file_data->dir_stats.dir_entry = create_proc_entry(PROC_STATS,
								   0,
								   file_data->dir_pid);
		if (file_data->dir_stats.dir_entry == NULL) {
			LOG_ERR("[%s]: failed to create \'%s\' entry",
				__func__, alloc_name);
		} else {
			file_data->dir_stats.priv_data = (void *)file_data;
			file_data->dir_stats.proc_read =
			    &vc_sm_statistics_proc_read;

			file_data->dir_stats.dir_entry->proc_fops =
			    &vc_sm_proc_fops;
			file_data->dir_stats.dir_entry->data =
			    &(file_data->dir_stats);
		}
	}

	LOG_DBG((sm_debug_log >= LOG_DBG_LEVEL_INTER_1),
		"[%s]: private data allocated %p", __func__, file_data);

out:
	return file_data;
}

/* Open the device.  Creates a private state to help track all allocation
** associated with this device.
*/
static int vc_sm_open(struct inode *inode, struct file *file)
{
	int ret = 0;

	/* Make sure the device was started properly.
	 */
	if (sm_state == NULL) {
		LOG_ERR("[%s]: invalid device", __func__);

		ret = -EPERM;
		goto out;
	}

	file->private_data = vc_sm_create_priv_data(current->tgid);
	if (file->private_data == NULL) {
		LOG_ERR("[%s]: failed to create data tracker", __func__);

		ret = -ENOMEM;
		goto out;
	}

out:
	return ret;
}

/* Close the device.  Free up all resources still associated with this device
** at the time.
*/
static int vc_sm_release(struct inode *inode, struct file *file)
{
	struct SM_PRIV_DATA_T *file_data =
	    (struct SM_PRIV_DATA_T *)file->private_data;
	struct SM_RESOURCE_T *resource;
	char alloc_name[32];
	int ret = 0;

	/* Make sure the device was started properly.
	 */
	if (sm_state == NULL || file_data == NULL) {
		LOG_ERR("[%s]: invalid device", __func__);
		ret = -EPERM;
		goto out;
	}

	LOG_DBG((sm_debug_log >= LOG_DBG_LEVEL_MIN),
		"[%s]: using private data %p", __func__, file_data);

	if (file_data->restart_sys == -EINTR) {
		VC_SM_ACTION_CLEAN_T action_clean;

		LOG_DBG((sm_debug_log >= LOG_DBG_LEVEL_MIN),
			"[%s]: releasing following EINTR on %u (trans_id: %u) (likely due to signal)...",
			__func__,
			file_data->int_action, file_data->int_trans_id);

		action_clean.res_action = file_data->int_action;
		action_clean.action_trans_id = file_data->int_trans_id;

		vc_vchi_sm_clean_up(sm_state->sm_handle, &action_clean);
	}

	while ((resource = vmcs_sm_acquire_first_resource(file_data)) != NULL) {
		vmcs_sm_release_resource(resource, 0);
		vmcs_sm_release_resource(resource, 1);
	}

	/* Remove the corresponding proc entry.
	 */
	snprintf(alloc_name, sizeof(alloc_name), "%d", file_data->pid);
	if (file_data->dir_pid != NULL) {
		remove_proc_entry(PROC_RESOURCES, file_data->dir_pid);
		remove_proc_entry(PROC_STATS, file_data->dir_pid);
		remove_proc_entry(alloc_name, sm_state->dir_alloc);
	}

	/* Terminate the private data.
	 */
	kfree(file_data);

out:
	return ret;
}

static void vcsm_vma_open(struct vm_area_struct *vma)
{
	struct sm_mmap *map = (struct sm_mmap *)vma->vm_private_data;

	LOG_DBG((sm_debug_log >= LOG_DBG_LEVEL_MAX),
		"[%s]: virt %lx-%lx, pid %i, pfn %i",
		__func__, vma->vm_start, vma->vm_end, (int)current->tgid,
		(int)vma->vm_pgoff);

	map->ref_count++;
}

static void vcsm_vma_close(struct vm_area_struct *vma)
{
	struct sm_mmap *map = (struct sm_mmap *)vma->vm_private_data;

	LOG_DBG((sm_debug_log >= LOG_DBG_LEVEL_MAX),
		"[%s]: virt %lx-%lx, pid %i, pfn %i",
		__func__, vma->vm_start, vma->vm_end, (int)current->tgid,
		(int)vma->vm_pgoff);

	map->ref_count--;

	/* Remove from the map table.
	 */
	if (map->ref_count == 0)
		vmcs_sm_remove_map(sm_state, map->resource, map);
}

static int vcsm_vma_fault(struct vm_area_struct *vma, struct vm_fault *vmf)
{
	struct sm_mmap *map = (struct sm_mmap *)vma->vm_private_data;
	struct SM_RESOURCE_T *resource = map->resource;
	pgoff_t page_offset;
	unsigned long pfn;
	int ret = 0;

	/* Lock the resource if necessary.
	 */
	if (!resource->lock_count) {
		VC_SM_LOCK_UNLOCK_T lock_unlock;
		VC_SM_LOCK_RESULT_T lock_result;
		int status;

		lock_unlock.res_handle = resource->res_handle;
		lock_unlock.res_mem = resource->res_base_mem;

		LOG_DBG((sm_debug_log >= LOG_DBG_LEVEL_MAX),
			"[%s]: attempt to lock data - hdl %x, base address %p",
			__func__, lock_unlock.res_handle, lock_unlock.res_mem);

		/* Lock the videocore allocated resource.
		 */
		status = vc_vchi_sm_lock(sm_state->sm_handle,
					 &lock_unlock, &lock_result, 0);
		if ((status != 0) ||
		    ((status == 0) && (lock_result.res_mem == NULL))) {
			LOG_ERR
			    ("[%s]: failed to lock memory on videocore"
			     " (status: %u)", __func__, status);
			resource->res_stats[LOCK_FAIL]++;
			return VM_FAULT_SIGBUS;
		}

		pfn = vcaddr_to_pfn((unsigned long)resource->res_base_mem);
		outer_inv_range(__pfn_to_phys(pfn),
				__pfn_to_phys(pfn) + resource->res_size);

		resource->res_stats[LOCK]++;
		resource->lock_count++;

		/* Keep track of the new base memory.
		 */
		if ((lock_result.res_mem != NULL) &&
		    (lock_result.res_old_mem != NULL) &&
		    (lock_result.res_mem != lock_result.res_old_mem)) {
			resource->res_base_mem = lock_result.res_mem;
		}
	}

	/* We don't use vmf->pgoff since that has the fake offset */
	page_offset = ((unsigned long)vmf->virtual_address - vma->vm_start);
	pfn = (uint32_t)resource->res_base_mem & 0x3FFFFFFF;
	pfn += mm_vc_mem_phys_addr;
	pfn += page_offset;
	pfn >>= PAGE_SHIFT;

	/* Finally, remap it */
	ret = vm_insert_pfn(vma, (unsigned long)vmf->virtual_address, pfn);

	switch (ret) {
	case 0:
	case -ERESTARTSYS:
		return VM_FAULT_NOPAGE;
	case -ENOMEM:
	case -EAGAIN:
		return VM_FAULT_OOM;
	default:
		return VM_FAULT_SIGBUS;
	}
}

static struct vm_operations_struct vcsm_vm_ops = {
	.open = vcsm_vma_open,
	.close = vcsm_vma_close,
	.fault = vcsm_vma_fault,
};

/* Walks a VMA and clean each valid page from the cache */
static void vcsm_vma_cache_clean_page_range(unsigned long addr,
					    unsigned long end)
{
	pgd_t *pgd;
	pud_t *pud;
	pmd_t *pmd;
	pte_t *pte;
	unsigned long pgd_next, pud_next, pmd_next;

	if (addr >= end)
		return;

	/* Walk PGD */
	pgd = pgd_offset(current->mm, addr);
	do {
		pgd_next = pgd_addr_end(addr, end);

		if (pgd_none(*pgd) || pgd_bad(*pgd))
			continue;

		/* Walk PUD */
		pud = pud_offset(pgd, addr);
		do {
			pud_next = pud_addr_end(addr, pgd_next);
			if (pud_none(*pud) || pud_bad(*pud))
				continue;

			/* Walk PMD */
			pmd = pmd_offset(pud, addr);
			do {
				pmd_next = pmd_addr_end(addr, pud_next);
				if (pmd_none(*pmd) || pmd_bad(*pmd))
					continue;

				/* Walk PTE */
				pte = pte_offset_map(pmd, addr);
				do {
					if (pte_none(*pte)
					    || !pte_present(*pte))
						continue;

					/* Clean + invalidate */
					dmac_flush_range((const void *)addr,
							 (const void *)(addr +
									PAGE_SIZE));

				} while (pte++, addr +=
					 PAGE_SIZE, addr != pmd_next);
				pte_unmap(pte);

			} while (pmd++, addr = pmd_next, addr != pud_next);

		} while (pud++, addr = pud_next, addr != pgd_next);
	} while (pgd++, addr = pgd_next, addr != end);
}

/* Map an allocated data into something that the user space.
*/
static int vc_sm_mmap(struct file *file, struct vm_area_struct *vma)
{
	int ret = 0;
	struct SM_PRIV_DATA_T *file_data =
	    (struct SM_PRIV_DATA_T *)file->private_data;
	struct SM_RESOURCE_T *resource = NULL;
	struct sm_mmap *map = NULL;

	/* Make sure the device was started properly.
	 */
	if ((sm_state == NULL) || (file_data == NULL)) {
		LOG_ERR("[%s]: invalid device", __func__);
		return -EPERM;
	}

	LOG_DBG((sm_debug_log >= LOG_DBG_LEVEL_MAX),
		"[%s]: private data %p, guid %x", __func__, file_data,
		((unsigned int)vma->vm_pgoff << PAGE_SHIFT));

	/* We lookup to make sure that the data we are being asked to mmap is
	 ** something that we allocated.
	 **
	 ** We use the offset information as the key to tell us which resource
	 ** we are mapping.
	 */
	resource = vmcs_sm_acquire_resource(file_data,
					    ((unsigned int)vma->vm_pgoff <<
					     PAGE_SHIFT));
	if (resource == NULL) {
		LOG_ERR("[%s]: failed to locate resource for guid %x", __func__,
			((unsigned int)vma->vm_pgoff << PAGE_SHIFT));
		return -ENOMEM;
	}

	LOG_DBG((sm_debug_log >= LOG_DBG_LEVEL_INTER_1),
		"[%s]: guid %x, tgid %u, %u, %u",
		__func__, resource->res_guid, current->tgid, resource->pid,
		file_data->pid);

	/* Check permissions.
	 */
	if (resource->pid && (resource->pid != current->tgid)) {
		LOG_ERR("[%s]: current tgid %u != %u owner",
			__func__, current->tgid, resource->pid);
		ret = -EPERM;
		goto error;
	}

	/* Verify that what we are asked to mmap is proper.
	 */
	if (resource->res_size != (unsigned int)(vma->vm_end - vma->vm_start)) {
		LOG_ERR("[%s]: size inconsistency (resource: %u - mmap: %u)",
			__func__,
			resource->res_size,
			(unsigned int)(vma->vm_end - vma->vm_start));

		ret = -EINVAL;
		goto error;
	}

	/* Keep track of the tuple in the global resource list such that one
	 * can do a mapping lookup for address/memory handle.
	 */
	map = kzalloc(sizeof(*map), GFP_KERNEL);
	if (map == NULL) {
		LOG_ERR("[%s]: failed to allocate global tracking resource",
			__func__);
		ret = -ENOMEM;
		goto error;
	}

	map->res_pid = current->tgid;
	map->res_vc_hdl = resource->res_handle;
	map->res_usr_hdl = resource->res_guid;
	map->res_addr = (long unsigned int)vma->vm_start;
	map->resource = resource;
	map->vma = vma;
	vmcs_sm_add_map(sm_state, resource, map);

	/* We are not actually mapping the pages, we just provide a fault
	 ** handler to allow pages to be mapped when accessed
	 */
	vma->vm_flags |=
	    VM_IO | VM_RESERVED | VM_PFNMAP | VM_DONTCOPY | VM_DONTEXPAND;
	vma->vm_ops = &vcsm_vm_ops;
	vma->vm_private_data = map;

	/* vm_pgoff is the first PFN of the mapped memory */
	vma->vm_pgoff = (unsigned long)resource->res_base_mem & 0x3FFFFFFF;
	vma->vm_pgoff += mm_vc_mem_phys_addr;
	vma->vm_pgoff >>= PAGE_SHIFT;

	if ((resource->res_cached == VMCS_SM_CACHE_NONE) ||
	    (resource->res_cached == VMCS_SM_CACHE_VC)) {
		/* Allocated non host cached memory, honour it.
		 */
		vma->vm_page_prot = pgprot_noncached(vma->vm_page_prot);
	}

	LOG_DBG((sm_debug_log >= LOG_DBG_LEVEL_MAX),
		"[%s]: resource %p (guid %x)"
		" - cnt %u, base address %p, handle %x, size %u (%u), cache %u",
		__func__,
		resource,
		resource->res_guid,
		resource->lock_count,
		resource->res_base_mem,
		resource->res_handle,
		resource->res_size,
		(unsigned int)(vma->vm_end - vma->vm_start),
		resource->res_cached);

	LOG_DBG((sm_debug_log >= LOG_DBG_LEVEL_MAX),
		"[%s]: resource %p (base address %p, handle %x)"
		" - map-count %d, usr-addr %x",
		__func__,
		resource,
		resource->res_base_mem,
		resource->res_handle,
		resource->map_count, (unsigned int)vma->vm_start);

	vcsm_vma_open(vma);
	resource->res_stats[MAP]++;
	vmcs_sm_release_resource(resource, 0);
	return 0;

error:
	vmcs_sm_release_resource(resource, 0);
	resource->res_stats[MAP_FAIL]++;
	return ret;
}

/* Allocate a shared memory handle and block.
*/
int vc_sm_ioctl_alloc(struct SM_PRIV_DATA_T *private,
		      struct vmcs_sm_ioctl_alloc *ioparam)
{
	int ret = 0;
	int status;
	struct SM_RESOURCE_T *resource;
	VC_SM_ALLOC_T alloc = { 0 };
	VC_SM_ALLOC_RESULT_T result = { 0 };

	/* Setup our allocation parameters */
	alloc.type = ((ioparam->cached == VMCS_SM_CACHE_VC)
		      || (ioparam->cached ==
			  VMCS_SM_CACHE_BOTH)) ? VC_SM_ALLOC_CACHED :
	    VC_SM_ALLOC_NON_CACHED;
	alloc.base_unit = ioparam->size;
	alloc.num_unit = ioparam->num;
	alloc.allocator = current->tgid;
	/* Align to kernel page size */
	alloc.alignement = 4096;
	/* Align the size to the kernel page size */
	alloc.base_unit =
	    (alloc.base_unit + alloc.alignement - 1) & ~(alloc.alignement - 1);
	if (*ioparam->name) {
		memcpy(alloc.name, ioparam->name, sizeof(alloc.name) - 1);
	} else {
		memcpy(alloc.name, VMCS_SM_RESOURCE_NAME_DEFAULT,
		       sizeof(VMCS_SM_RESOURCE_NAME_DEFAULT));
	}

	LOG_DBG((sm_debug_log >= LOG_DBG_LEVEL_MIN),
		"[%s]: attempt to allocate \"%s\" data"
		" - type %u, base %u (%u), num %u, alignement %u",
		__func__, alloc.name, alloc.type, ioparam->size,
		alloc.base_unit, alloc.num_unit, alloc.alignement);

	/* Allocate local resource to track this allocation.
	 */
	resource = kzalloc(sizeof(*resource), GFP_KERNEL);
	if (resource == NULL) {
		LOG_ERR("[%s]: failed to allocate local tracking resource",
			__func__);
		ret = -ENOMEM;
		goto error;
	}
	INIT_LIST_HEAD(&resource->map_list);
	resource->ref_count++;
	resource->pid = current->tgid;

	/* Allocate the videocore resource.
	 */
	status = vc_vchi_sm_alloc(sm_state->sm_handle, &alloc, &result,
				  &private->int_trans_id);
	if (status == -EINTR) {
		LOG_DBG((sm_debug_log >= LOG_DBG_LEVEL_INTER_1),
			"[%s]: requesting allocate memory action restart (trans_id: %u)",
			__func__, private->int_trans_id);
		ret = -ERESTARTSYS;
		private->restart_sys = -EINTR;
		private->int_action = VC_SM_MSG_TYPE_ALLOC;
		goto error;
	} else if (status != 0 || (status == 0 && result.res_mem == NULL)) {
		LOG_ERR
		    ("[%s]: failed to allocate memory on videocore"
		     " (status: %u, trans_id: %u)",
		     __func__, status, private->int_trans_id);
		ret = -ENOMEM;
		resource->res_stats[ALLOC_FAIL]++;
		goto error;
	}

	/* Keep track of the resource we created.
	 */
	resource->private = private;
	resource->res_handle = result.res_handle;
	resource->res_base_mem = result.res_mem;
	resource->res_size = alloc.base_unit * alloc.num_unit;
	resource->res_cached = ioparam->cached;

	/* Kernel/user GUID.  This global identifier is used for mmap'ing the
	 * allocated region from user space, it is passed as the mmap'ing
	 * offset, we use it to 'hide' the videocore handle/address.
	 */
	mutex_lock(&sm_state->lock);
	resource->res_guid = ++sm_state->guid;
	mutex_unlock(&sm_state->lock);
	resource->res_guid <<= PAGE_SHIFT;

	vmcs_sm_add_resource(private, resource);

	LOG_DBG((sm_debug_log >= LOG_DBG_LEVEL_MIN),
		"[%s]: allocated data"
		" - guid %x, hdl %x, base address %p, size %d, cache %d",
		__func__, resource->res_guid, resource->res_handle,
		resource->res_base_mem, resource->res_size,
		resource->res_cached);

	/* We're done */
	resource->res_stats[ALLOC]++;
	ioparam->handle = resource->res_guid;
	return 0;

error:
	LOG_ERR
	    ("[%s]: failed to allocate \"%s\" data (%i)"
	     " - type %u, base %u (%u), num %u, alignement %u",
	     __func__, alloc.name, ret, alloc.type, ioparam->size,
	     alloc.base_unit, alloc.num_unit, alloc.alignement);
	if (resource != NULL) {
		vc_sm_resource_deceased(resource, 1);
		kfree(resource);
	}
	return ret;
}

/* Share an allocate memory handle and block.
*/
int vc_sm_ioctl_alloc_share(struct SM_PRIV_DATA_T *private,
			    struct vmcs_sm_ioctl_alloc_share *ioparam)
{
	struct SM_RESOURCE_T *resource, *shared_resource;
	int ret = 0;

	LOG_DBG((sm_debug_log >= LOG_DBG_LEVEL_MIN),
		"[%s]: attempt to share resource %u", __func__,
		ioparam->handle);

	shared_resource = vmcs_sm_acquire_global_resource(ioparam->handle);
	if (shared_resource == NULL) {
		ret = -ENOMEM;
		goto error;
	}

	/* Allocate local resource to track this allocation.
	 */
	resource = kzalloc(sizeof(*resource), GFP_KERNEL);
	if (resource == NULL) {
		LOG_ERR("[%s]: failed to allocate local tracking resource",
			__func__);
		ret = -ENOMEM;
		goto error;
	}
	INIT_LIST_HEAD(&resource->map_list);
	resource->ref_count++;
	resource->pid = current->tgid;

	/* Keep track of the resource we created.
	 */
	resource->private = private;
	resource->res_handle = shared_resource->res_handle;
	resource->res_base_mem = shared_resource->res_base_mem;
	resource->res_size = shared_resource->res_size;
	resource->res_cached = shared_resource->res_cached;
	resource->res_shared = shared_resource;

	mutex_lock(&sm_state->lock);
	resource->res_guid = ++sm_state->guid;
	mutex_unlock(&sm_state->lock);
	resource->res_guid <<= PAGE_SHIFT;

	vmcs_sm_add_resource(private, resource);

	LOG_DBG((sm_debug_log >= LOG_DBG_LEVEL_MIN),
		"[%s]: allocated data - guid %x, hdl %x, base address %p, size %d, cache %d",
		__func__, resource->res_guid, resource->res_handle,
		resource->res_base_mem, resource->res_size,
		resource->res_cached);

	/* We're done */
	resource->res_stats[ALLOC]++;
	ioparam->handle = resource->res_guid;
	ioparam->size = resource->res_size;
	return 0;

error:
	LOG_ERR("[%s]: failed to share %u", __func__, ioparam->handle);
	if (shared_resource != NULL)
		vmcs_sm_release_resource(shared_resource, 0);

	return ret;
}

/* Free a previously allocated shared memory handle and block.
*/
static int vc_sm_ioctl_free(struct SM_PRIV_DATA_T *private,
			    struct vmcs_sm_ioctl_free *ioparam)
{
	struct SM_RESOURCE_T *resource =
	    vmcs_sm_acquire_resource(private, ioparam->handle);

	if (resource == NULL) {
		LOG_ERR("[%s]: resource for guid %u does not exist", __func__,
			ioparam->handle);
		return -EINVAL;
	}

	/* Check permissions.
	 */
	if (resource->pid && (resource->pid != current->tgid)) {
		LOG_ERR("[%s]: current tgid %u != %u owner",
			__func__, current->tgid, resource->pid);
		vmcs_sm_release_resource(resource, 0);
		return -EPERM;
	}

	vmcs_sm_release_resource(resource, 0);
	vmcs_sm_release_resource(resource, 0);
	return 0;
}

/* Resize a previously allocated shared memory handle and block.
*/
static int vc_sm_ioctl_resize(struct SM_PRIV_DATA_T *private,
			      struct vmcs_sm_ioctl_resize *ioparam)
{
	int ret = 0;
	int status;
	VC_SM_RESIZE_T resize;
	struct SM_RESOURCE_T *resource;

	/* Locate resource from GUID.
	 */
	resource = vmcs_sm_acquire_resource(private, ioparam->handle);
	if (resource == NULL) {
		LOG_ERR
		    ("[%s]: failed resource"
		     " - guid %x", __func__, ioparam->handle);
		ret = -EFAULT;
		goto error;
	}

	/* If the resource is locked, its reference count will be not NULL,
	 ** in which case we will not be allowed to resize it anyways, so
	 ** reject the attempt here.
	 */
	if (resource->lock_count != 0) {
		LOG_ERR
		    ("[%s]: cannot resize"
		     " - guid %x, ref-cnt %d",
		     __func__, ioparam->handle, resource->lock_count);
		ret = -EFAULT;
		goto error;
	}

	/* Check permissions.
	 */
	if (resource->pid && (resource->pid != current->tgid)) {
		LOG_ERR("[%s]: current tgid %u != %u owner",
			__func__, current->tgid, resource->pid);
		ret = -EPERM;
		goto error;
	}

	if (resource->map_count != 0) {
		LOG_ERR
		    ("[%s]: cannot resize"
		     " - guid %x, ref-cnt %d",
		     __func__, ioparam->handle, resource->map_count);
		ret = -EFAULT;
		goto error;
	}

	resize.res_handle = (resource != NULL) ? resource->res_handle : 0;
	resize.res_mem = (resource != NULL) ? resource->res_base_mem : NULL;
	resize.res_new_size = ioparam->new_size;

	LOG_DBG((sm_debug_log >= LOG_DBG_LEVEL_MAX),
		"[%s]: attempt to resize data - guid %x, hdl %x, base address %p",
		__func__, ioparam->handle, resize.res_handle, resize.res_mem);

	/* Resize the videocore allocated resource.
	 */
	status = vc_vchi_sm_resize(sm_state->sm_handle, &resize,
				   &private->int_trans_id);
	if (status == -EINTR) {
		LOG_DBG((sm_debug_log >= LOG_DBG_LEVEL_MAX),
			"[%s]: requesting resize memory action restart (trans_id: %u)",
			__func__, private->int_trans_id);
		ret = -ERESTARTSYS;
		private->restart_sys = -EINTR;
		private->int_action = VC_SM_MSG_TYPE_RESIZE;
		goto error;
	} else if (status != 0) {
		LOG_ERR
		    ("[%s]: failed to resize memory on videocore"
		     " (status: %u, trans_id: %u)",
		     __func__, status, private->int_trans_id);
		ret = -EPERM;
		goto error;
	}

	LOG_DBG((sm_debug_log >= LOG_DBG_LEVEL_MAX),
		"[%s]: success to resize data - hdl %x, size %d -> %d",
		__func__, resize.res_handle, resource->res_size,
		resize.res_new_size);

	/* Successfully resized, save the information and inform the user.
	 */
	ioparam->old_size = resource->res_size;
	resource->res_size = resize.res_new_size;

error:
	if (resource)
		vmcs_sm_release_resource(resource, 0);

	return ret;
}

/* Lock a previously allocated shared memory handle and block.
*/
static int vc_sm_ioctl_lock(struct SM_PRIV_DATA_T *private,
			    struct vmcs_sm_ioctl_lock_unlock *ioparam,
			    int change_cache, enum vmcs_sm_cache_e cache_type,
			    unsigned int vc_addr)
{
	int status;
	VC_SM_LOCK_UNLOCK_T lock;
	VC_SM_LOCK_RESULT_T result;
	struct SM_RESOURCE_T *resource;
	int ret = 0;
	struct sm_mmap *map, *map_tmp;
	long unsigned int phys_addr;

	map = NULL;

	/* Locate resource from GUID.
	 */
	resource = vmcs_sm_acquire_resource(private, ioparam->handle);
	if (resource == NULL) {
		ret = -EINVAL;
		goto error;
	}

	/* Check permissions.
	 */
	if (resource->pid && (resource->pid != current->tgid)) {
		LOG_ERR("[%s]: current tgid %u != %u owner",
			__func__, current->tgid, resource->pid);
		ret = -EPERM;
		goto error;
	}

	lock.res_handle = resource->res_handle;
	lock.res_mem = resource->res_base_mem;

	/* Take the lock and get the address to be mapped.
	 */
	if (vc_addr == 0) {
		LOG_DBG((sm_debug_log >= LOG_DBG_LEVEL_INTER_1),
			"[%s]: attempt to lock data - guid %x, hdl %x, base address %p",
			__func__, ioparam->handle, lock.res_handle,
			lock.res_mem);

		/* Lock the videocore allocated resource.
		 */
		status = vc_vchi_sm_lock(sm_state->sm_handle, &lock, &result,
					 &private->int_trans_id);
		if (status == -EINTR) {
			LOG_DBG((sm_debug_log >= LOG_DBG_LEVEL_INTER_1),
				"[%s]: requesting lock memory action restart (trans_id: %u)",
				__func__, private->int_trans_id);
			ret = -ERESTARTSYS;
			private->restart_sys = -EINTR;
			private->int_action = VC_SM_MSG_TYPE_LOCK;
			goto error;
		} else if (status != 0 ||
			   (status == 0 && result.res_mem == NULL)) {
			LOG_ERR
			    ("[%s]: failed to lock memory on videocore"
			     " (status: %u, trans_id: %u)",
			     __func__, status, private->int_trans_id);
			ret = -EPERM;
			resource->res_stats[LOCK_FAIL]++;
			goto error;
		}

		LOG_DBG((sm_debug_log >= LOG_DBG_LEVEL_INTER_1),
			"[%s]: succeed to lock data"
			" - hdl %x, base address %p (%p), ref-cnt %d",
			__func__, lock.res_handle, result.res_mem, lock.res_mem,
			resource->lock_count);
	}
	/* Lock assumed taken already, address to be mapped is known.
	 */
	else
		resource->res_base_mem = (void *)vc_addr;

	resource->res_stats[LOCK]++;
	resource->lock_count++;

	/* Keep track of the new base memory allocation if it has changed.
	 */
	if ((vc_addr == 0) &&
	    (result.res_mem != NULL) &&
	    (result.res_old_mem != NULL) &&
	    (result.res_mem != result.res_old_mem)) {
		resource->res_base_mem = result.res_mem;

		/* Kernel allocated resources.
		 */
		if (resource->pid == 0) {
			if (!list_empty(&resource->map_list)) {
				list_for_each_entry_safe(map, map_tmp,
							 &resource->map_list,
							 resource_map_list) {
					if (map->res_addr) {
						iounmap((void *)map->res_addr);
						map->res_addr = 0;

						vmcs_sm_remove_map(sm_state,
								   map->resource,
								   map);
						break;
					}
				}
			}
		}
	}

	if (change_cache)
		resource->res_cached = cache_type;

	if (resource->map_count) {
		ioparam->addr =
		    vmcs_sm_usr_address_from_pid_and_usr_handle(current->tgid,
								ioparam->handle);

		LOG_DBG((sm_debug_log >= LOG_DBG_LEVEL_INTER_1),
			"[%s] map_count %d private->pid %d current->tgid %d hnd %x addr %u",
			__func__, resource->map_count, private->pid,
			current->tgid, ioparam->handle, ioparam->addr);
	} else {
		/* Kernel allocated resources.
		 */
		if (resource->pid == 0) {
			LOG_DBG((sm_debug_log >= LOG_DBG_LEVEL_INTER_1),
				"[%s]: attempt mapping kernel resource - guid %x, hdl %x",
				__func__, ioparam->handle, lock.res_handle);

			ioparam->addr = 0;

			map = kzalloc(sizeof(*map), GFP_KERNEL);
			if (map == NULL) {
				LOG_ERR
				    ("[%s]: failed allocating tracker",
				     __func__);
				ret = -ENOMEM;
				goto error;
			} else {
				phys_addr =
				    (uint32_t)resource->res_base_mem &
				    0x3FFFFFFF;
				phys_addr += mm_vc_mem_phys_addr;
				if (resource->res_cached == VMCS_SM_CACHE_HOST) {
					ioparam->addr = (long unsigned int)
					    ioremap_cached(phys_addr,
							   resource->res_size);

					LOG_DBG((sm_debug_log >=
						 LOG_DBG_LEVEL_INTER_1),
						"[%s]: mapping kernel"
						" - guid %x, hdl %x - cached mapping %u",
						__func__, ioparam->handle,
						lock.res_handle, ioparam->addr);
				} else {
					ioparam->addr = (long unsigned int)
					    ioremap_nocache(phys_addr,
							    resource->res_size);

					LOG_DBG((sm_debug_log >=
						 LOG_DBG_LEVEL_INTER_1),
						"[%s]: mapping kernel"
						" - guid %x, hdl %x - non cached mapping %u",
						__func__, ioparam->handle,
						lock.res_handle, ioparam->addr);
				}

				map->res_pid = 0;
				map->res_vc_hdl = resource->res_handle;
				map->res_usr_hdl = resource->res_guid;
				map->res_addr = ioparam->addr;
				map->resource = resource;
				map->vma = NULL;

				vmcs_sm_add_map(sm_state, resource, map);
			}
		} else
			ioparam->addr = 0;
	}

error:
	if (resource)
		vmcs_sm_release_resource(resource, 0);

	return ret;
}

/* Unlock a previously allocated shared memory handle and block.
*/
static int vc_sm_ioctl_unlock(struct SM_PRIV_DATA_T *private,
			      struct vmcs_sm_ioctl_lock_unlock *ioparam,
			      int flush, int wait_reply, int no_vc_unlock)
{
	int status;
	VC_SM_LOCK_UNLOCK_T unlock;
	struct sm_mmap *map, *map_tmp;
	struct SM_RESOURCE_T *resource;
	int ret = 0;

	map = NULL;

	/* Locate resource from GUID.
	 */
	resource = vmcs_sm_acquire_resource(private, ioparam->handle);
	if (resource == NULL) {
		ret = -EINVAL;
		goto error;
	}

	/* Check permissions.
	 */
	if (resource->pid && (resource->pid != current->tgid)) {
		LOG_ERR("[%s]: current tgid %u != %u owner",
			__func__, current->tgid, resource->pid);
		ret = -EPERM;
		goto error;
	}

	unlock.res_handle = resource->res_handle;
	unlock.res_mem = resource->res_base_mem;

	LOG_DBG((sm_debug_log >= LOG_DBG_LEVEL_INTER_1),
		"[%s]: attempt to unlock data - guid %x, hdl %x, base address %p",
		__func__, ioparam->handle, unlock.res_handle, unlock.res_mem);

	/* User space allocated resources.
	 */
	if (resource->pid) {
		/* Flush if requested */
		if (resource->res_cached && flush) {
			dma_addr_t phys_addr = 0;
			resource->res_stats[FLUSH]++;

			phys_addr =
			    (dma_addr_t)((uint32_t)resource->res_base_mem &
					 0x3FFFFFFF);
			phys_addr += (dma_addr_t)mm_vc_mem_phys_addr;

			/* L1 cache flush */
			down_read(&current->mm->mmap_sem);
			list_for_each_entry(map, &resource->map_list,
					    resource_map_list) {
				if (map->vma)
					vcsm_vma_cache_clean_page_range((unsigned long)
									map->
									vma->
									vm_start,
									(unsigned
									 long)
									map->
									vma->
									vm_end);
			}
			up_read(&current->mm->mmap_sem);

			/* L2 cache flush */
			outer_clean_range(phys_addr,
					  phys_addr +
					  (size_t) resource->res_size);
		}

		/* We need to zap all the vmas associated with this resource */
		if (resource->lock_count == 1) {
			down_read(&current->mm->mmap_sem);
			list_for_each_entry(map, &resource->map_list,
					    resource_map_list) {
				if (map->vma) {
					zap_vma_ptes(map->vma,
						     map->vma->vm_start,
						     map->vma->vm_end -
						     map->vma->vm_start);
				}
			}
			up_read(&current->mm->mmap_sem);
		}
	}
	/* Kernel allocated resources. */
	else {
		if (resource->ref_count ==
		    2 /* Global + Taken in this context */ ) {
			if (!list_empty(&resource->map_list)) {
				list_for_each_entry_safe(map, map_tmp,
							 &resource->map_list,
							 resource_map_list) {
					if (map->res_addr) {
						if (flush
						    && (resource->res_cached ==
							VMCS_SM_CACHE_HOST)) {
							long unsigned int
							    phys_addr;
							phys_addr = (uint32_t)
							    resource->res_base_mem & 0x3FFFFFFF;
							phys_addr +=
							    mm_vc_mem_phys_addr;

							/* L1 cache flush */
							dmac_flush_range((const
									  void
									  *)
									 map->res_addr, (const void *)
									 (map->res_addr + resource->res_size));

							/* L2 cache flush */
							outer_clean_range
							    (phys_addr,
							     phys_addr +
							     (size_t)
							     resource->res_size);
						}

						iounmap((void *)map->res_addr);
						map->res_addr = 0;

						vmcs_sm_remove_map(sm_state,
								   map->resource,
								   map);
						break;
					}
				}
			}
		}
	}

	if (resource->lock_count) {
		/* Bypass the videocore unlock.
		 */
		if (no_vc_unlock)
			status = 0;
		/* Unlock the videocore allocated resource.
		 */
		else {
			status =
			    vc_vchi_sm_unlock(sm_state->sm_handle, &unlock,
					      &private->int_trans_id,
					      wait_reply);
			if (status == -EINTR) {
				LOG_DBG((sm_debug_log >= LOG_DBG_LEVEL_INTER_1),
					"[%s]: requesting unlock memory action restart (trans_id: %u)",
					__func__, private->int_trans_id);

				ret = -ERESTARTSYS;
				resource->res_stats[UNLOCK]--;
				private->restart_sys = -EINTR;
				private->int_action = VC_SM_MSG_TYPE_UNLOCK;
				goto error;
			} else if (status != 0) {
				LOG_ERR
				    ("[%s]: failed to unlock vc mem"
				     " (status: %u, trans_id: %u)",
				     __func__, status, private->int_trans_id);

				ret = -EPERM;
				resource->res_stats[UNLOCK_FAIL]++;
				goto error;
			}
		}

		resource->res_stats[UNLOCK]++;
		resource->lock_count--;
	}

	LOG_DBG((sm_debug_log >= LOG_DBG_LEVEL_INTER_1),
		"[%s]: success to unlock data - hdl %x, base address %p, ref-cnt %d",
		__func__, unlock.res_handle, unlock.res_mem,
		resource->lock_count);

error:
	if (resource)
		vmcs_sm_release_resource(resource, 0);

	return ret;
}

/* Handle control from host.
*/
static long vc_sm_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	int ret = 0;
	unsigned int cmdnr = _IOC_NR(cmd);
	struct SM_PRIV_DATA_T *file_data =
	    (struct SM_PRIV_DATA_T *)file->private_data;
	struct SM_RESOURCE_T *resource = NULL;

	/* Validate we can work with this device.
	 */
	if ((sm_state == NULL) || (file_data == NULL)) {
		LOG_ERR("[%s]: invalid device", __func__);
		ret = -EPERM;
		goto out;
	}

	LOG_DBG((sm_debug_log >= LOG_DBG_LEVEL_INTER_1),
		"[%s]: cmd %x tgid %u, owner %u",
		__func__, cmdnr, current->tgid, file_data->pid);

	/* Action is a re-post of a previously interrupted action?
	 */
	if (file_data->restart_sys == -EINTR) {
		VC_SM_ACTION_CLEAN_T action_clean;

		LOG_DBG((sm_debug_log >= LOG_DBG_LEVEL_MIN),
			"[%s]: clean up of action %u (trans_id: %u) following EINTR",
			__func__,
			file_data->int_action, file_data->int_trans_id);

		action_clean.res_action = file_data->int_action;
		action_clean.action_trans_id = file_data->int_trans_id;

		vc_vchi_sm_clean_up(sm_state->sm_handle, &action_clean);

		file_data->restart_sys = 0;
	}

	/* Now process the command.
	 */
	switch (cmdnr) {
		/* New memory allocation.
		 */
	case VMCS_SM_CMD_ALLOC:
		{
			struct vmcs_sm_ioctl_alloc ioparam;

			/* Get the parameter data.
			 */
			if (copy_from_user
			    (&ioparam, (void *)arg, sizeof(ioparam)) != 0) {
				LOG_ERR
				    ("[%s]: failed to copy-from-user"
				     " for cmd %x", __func__, cmdnr);
				ret = -EFAULT;
				goto out;
			}

			ret = vc_sm_ioctl_alloc(file_data, &ioparam);
			if (!ret &&
			    (copy_to_user((void *)arg,
					  &ioparam, sizeof(ioparam)) != 0)) {
				struct vmcs_sm_ioctl_free freeparam = {
					ioparam.handle
				};
				LOG_ERR
				    ("[%s]: failed to copy-to-user"
				     " for cmd %x", __func__, cmdnr);
				vc_sm_ioctl_free(file_data, &freeparam);
				ret = -EFAULT;
			}

			/* Done.
			 */
			goto out;
		}
		break;

		/* Share existing memory allocation.
		 */
	case VMCS_SM_CMD_ALLOC_SHARE:
		{
			struct vmcs_sm_ioctl_alloc_share ioparam;

			/* Get the parameter data.
			 */
			if (copy_from_user
			    (&ioparam, (void *)arg, sizeof(ioparam)) != 0) {
				LOG_ERR
				    ("[%s]: failed to copy-from-user"
				     " for cmd %x", __func__, cmdnr);
				ret = -EFAULT;
				goto out;
			}

			ret = vc_sm_ioctl_alloc_share(file_data, &ioparam);

			/* Copy result back to user.
			 */
			if (!ret
			    && copy_to_user((void *)arg, &ioparam,
					    sizeof(ioparam)) != 0) {
				struct vmcs_sm_ioctl_free freeparam = {
					ioparam.handle
				};
				LOG_ERR
				    ("[%s]: failed to copy-to-user"
				     " for cmd %x", __func__, cmdnr);
				vc_sm_ioctl_free(file_data, &freeparam);
				ret = -EFAULT;
			}

			/* Done.
			 */
			goto out;
		}
		break;

		/* Lock (attempt to) *and* register a cache behavior change.
		 */
	case VMCS_SM_CMD_LOCK_CACHE:
		{
			struct vmcs_sm_ioctl_lock_cache ioparam;
			struct vmcs_sm_ioctl_lock_unlock lock;

			/* Get parameter data.
			 */
			if (copy_from_user
			    (&ioparam, (void *)arg, sizeof(ioparam)) != 0) {
				LOG_ERR
				    ("[%s]: failed to copy-from-user"
				     " for cmd %x", __func__, cmdnr);
				ret = -EFAULT;
				goto out;
			}

			lock.handle = ioparam.handle;
			ret =
			    vc_sm_ioctl_lock(file_data, &lock, 1,
					     ioparam.cached, 0);

			/* Done.
			 */
			goto out;
		}
		break;

		/* Lock (attempt to) existing memory allocation.
		 */
	case VMCS_SM_CMD_LOCK:
		{
			struct vmcs_sm_ioctl_lock_unlock ioparam;

			/* Get parameter data.
			 */
			if (copy_from_user
			    (&ioparam, (void *)arg, sizeof(ioparam)) != 0) {
				LOG_ERR
				    ("[%s]: failed to copy-from-user"
				     " for cmd %x", __func__, cmdnr);
				ret = -EFAULT;
				goto out;
			}

			ret = vc_sm_ioctl_lock(file_data, &ioparam, 0, 0, 0);

			/* Copy result back to user.
			 */
			if (copy_to_user((void *)arg, &ioparam, sizeof(ioparam))
			    != 0) {
				LOG_ERR
				    ("[%s]: failed to copy-to-user for cmd %x",
				     __func__, cmdnr);
				ret = -EFAULT;
			}

			/* Done.
			 */
			goto out;
		}
		break;

		/* Unlock (attempt to) existing memory allocation.
		 */
	case VMCS_SM_CMD_UNLOCK:
		{
			struct vmcs_sm_ioctl_lock_unlock ioparam;

			/* Get parameter data.
			 */
			if (copy_from_user
			    (&ioparam, (void *)arg, sizeof(ioparam)) != 0) {
				LOG_ERR
				    ("[%s]: failed to copy-from-user"
				     " for cmd %x", __func__, cmdnr);
				ret = -EFAULT;
				goto out;
			}

			ret = vc_sm_ioctl_unlock(file_data, &ioparam, 0, 1, 0);

			/* Done.
			 */
			goto out;
		}
		break;

		/* Resize (attempt to) existing memory allocation.
		 */
	case VMCS_SM_CMD_RESIZE:
		{
			struct vmcs_sm_ioctl_resize ioparam;

			/* Get parameter data.
			 */
			if (copy_from_user
			    (&ioparam, (void *)arg, sizeof(ioparam)) != 0) {
				LOG_ERR
				    ("[%s]: failed to copy-from-user"
				     " for cmd %x", __func__, cmdnr);
				ret = -EFAULT;
				goto out;
			}

			ret = vc_sm_ioctl_resize(file_data, &ioparam);

			/* Copy result back to user.
			 */
			if (copy_to_user((void *)arg, &ioparam, sizeof(ioparam))
			    != 0) {
				LOG_ERR
				    ("[%s]: failed to copy-to-user for cmd %x",
				     __func__, cmdnr);
				ret = -EFAULT;
			}

			/* Done.
			 */
			goto out;
		}
		break;

		/* Terminate existing memory allocation.
		 */
	case VMCS_SM_CMD_FREE:
		{
			struct vmcs_sm_ioctl_free ioparam;

			/* Get parameter data.
			 */
			if (copy_from_user
			    (&ioparam, (void *)arg, sizeof(ioparam)) != 0) {
				LOG_ERR
				    ("[%s]: failed to copy-from-user"
				     " for cmd %x", __func__, cmdnr);
				ret = -EFAULT;
				goto out;
			}

			ret = vc_sm_ioctl_free(file_data, &ioparam);

			/* Done.
			 */
			goto out;
		}
		break;

		/* Walk allocation on videocore, information shows up in the
		 ** videocore log.
		 */
	case VMCS_SM_CMD_VC_WALK_ALLOC:
		{
			LOG_DBG((sm_debug_log >= LOG_DBG_LEVEL_MAX),
				"[%s]: invoking walk alloc", __func__);

			if (vc_vchi_sm_walk_alloc(sm_state->sm_handle) != 0)
				LOG_ERR
				    ("[%s]: failed to walk-alloc on videocore",
				     __func__);

			/* Done.
			 */
			goto out;
		}
		break;

		/* Walk mapping table on host, information shows up in the
		 ** kernel log.
		 */
	case VMCS_SM_CMD_HOST_WALK_MAP:
		{
			/* Use pid of -1 to tell to walk the whole map.
			 */
			vmcs_sm_host_walk_map_per_pid(-1);

			/* Done.
			 */
			goto out;
		}
		break;

		/* Walk mapping table per process on host.
		 */
	case VMCS_SM_CMD_HOST_WALK_PID_ALLOC:
		{
			struct vmcs_sm_ioctl_walk ioparam;

			/* Get parameter data.
			 */
			if (copy_from_user(&ioparam,
					   (void *)arg, sizeof(ioparam)) != 0) {
				LOG_ERR
				    ("[%s]: failed to copy-from-user"
				     " for cmd %x", __func__, cmdnr);

				ret = -EFAULT;
				goto out;
			}

			vmcs_sm_host_walk_alloc(file_data);

			/* Done.
			 */
			goto out;
		}
		break;

		/* Walk allocation per process on host.
		 */
	case VMCS_SM_CMD_HOST_WALK_PID_MAP:
		{
			struct vmcs_sm_ioctl_walk ioparam;

			/* Get parameter data.
			 */
			if (copy_from_user(&ioparam,
					   (void *)arg, sizeof(ioparam)) != 0) {
				LOG_ERR
				    ("[%s]: failed to copy-from-user"
				     " for cmd %x", __func__, cmdnr);

				ret = -EFAULT;
				goto out;
			}

			vmcs_sm_host_walk_map_per_pid(ioparam.pid);

			/* Done.
			 */
			goto out;
		}
		break;

		/* Gets the size of the memory associated with a user handle.
		 */
	case VMCS_SM_CMD_SIZE_USR_HANDLE:
		{
			struct vmcs_sm_ioctl_size ioparam;

			/* Get parameter data.
			 */
			if (copy_from_user(&ioparam,
					   (void *)arg, sizeof(ioparam)) != 0) {
				LOG_ERR
				    ("[%s]: failed to copy-from-user"
				     " for cmd %x", __func__, cmdnr);

				ret = -EFAULT;
				goto out;
			}

			/* Locate resource from GUID.
			 */
			resource =
			    vmcs_sm_acquire_resource(file_data, ioparam.handle);
			if (resource != NULL) {
				ioparam.size = resource->res_size;
				vmcs_sm_release_resource(resource, 0);
			} else {
				ioparam.size = 0;
			}

			if (copy_to_user((void *)arg,
					 &ioparam, sizeof(ioparam)) != 0) {
				LOG_ERR
				    ("[%s]: failed to copy-to-user for cmd %x",
				     __func__, cmdnr);

				ret = -EFAULT;
			}

			/* Done.
			 */
			goto out;
		}
		break;

		/* Verify we are dealing with a valid resource.
		 */
	case VMCS_SM_CMD_CHK_USR_HANDLE:
		{
			struct vmcs_sm_ioctl_chk ioparam;

			/* Get parameter data.
			 */
			if (copy_from_user(&ioparam,
					   (void *)arg, sizeof(ioparam)) != 0) {
				LOG_ERR
				    ("[%s]: failed to copy-from-user"
				     " for cmd %x", __func__, cmdnr);

				ret = -EFAULT;
				goto out;
			}

			/* Locate resource from GUID.
			 */
			resource =
			    vmcs_sm_acquire_resource(file_data, ioparam.handle);
			if (resource == NULL)
				ret = -EINVAL;
			/* If the resource is cacheable, return additional
			 * information that may be needed to flush the cache.
			 */
			else if ((resource->res_cached == VMCS_SM_CACHE_HOST) ||
				 (resource->res_cached == VMCS_SM_CACHE_BOTH)) {
				ioparam.addr =
				    vmcs_sm_usr_address_from_pid_and_usr_handle
				    (current->tgid, ioparam.handle);
				ioparam.size = resource->res_size;
				ioparam.cache = resource->res_cached;
			} else {
				ioparam.addr = 0;
				ioparam.size = 0;
				ioparam.cache = resource->res_cached;
			}

			if (resource)
				vmcs_sm_release_resource(resource, 0);

			if (copy_to_user((void *)arg,
					 &ioparam, sizeof(ioparam)) != 0) {
				LOG_ERR
				    ("[%s]: failed to copy-to-user"
				     " for cmd %x", __func__, cmdnr);

				ret = -EFAULT;
			}

			/* Done.
			 */
			goto out;
		}
		break;

		/* Maps a user handle given the process and the virtual address.
		 */
	case VMCS_SM_CMD_MAPPED_USR_HANDLE:
		{
			struct vmcs_sm_ioctl_map ioparam;

			/* Get parameter data.
			 */
			if (copy_from_user(&ioparam,
					   (void *)arg, sizeof(ioparam)) != 0) {
				LOG_ERR
				    ("[%s]: failed to copy-from-user"
				     " for cmd %x", __func__, cmdnr);

				ret = -EFAULT;
				goto out;
			}

			ioparam.handle =
			    vmcs_sm_usr_handle_from_pid_and_address(ioparam.pid,
								    ioparam.addr);

			resource =
			    vmcs_sm_acquire_resource(file_data, ioparam.handle);
			if ((resource != NULL)
			    && ((resource->res_cached == VMCS_SM_CACHE_HOST)
				|| (resource->res_cached ==
				    VMCS_SM_CACHE_BOTH))) {
				ioparam.size = resource->res_size;
			} else {
				ioparam.size = 0;
			}

			if (resource)
				vmcs_sm_release_resource(resource, 0);

			if (copy_to_user((void *)arg,
					 &ioparam, sizeof(ioparam)) != 0) {
				LOG_ERR
				    ("[%s]: failed to copy-to-user for cmd %x",
				     __func__, cmdnr);

				ret = -EFAULT;
			}

			/* Done.
			 */
			goto out;
		}
		break;

		/* Maps a videocore handle given process and virtual address.
		 */
	case VMCS_SM_CMD_MAPPED_VC_HDL_FROM_ADDR:
		{
			struct vmcs_sm_ioctl_map ioparam;

			/* Get parameter data.
			 */
			if (copy_from_user(&ioparam,
					   (void *)arg, sizeof(ioparam)) != 0) {
				LOG_ERR
				    ("[%s]: failed to copy-from-user"
				     " for cmd %x", __func__, cmdnr);

				ret = -EFAULT;
				goto out;
			}

			ioparam.handle =
			    vmcs_sm_vc_handle_from_pid_and_address(ioparam.pid,
								   ioparam.addr);

			if (copy_to_user((void *)arg,
					 &ioparam, sizeof(ioparam)) != 0) {
				LOG_ERR
				    ("[%s]: failed to copy-to-user for cmd %x",
				     __func__, cmdnr);

				ret = -EFAULT;
			}

			/* Done.
			 */
			goto out;
		}
		break;

		/* Maps a videocore handle given process and user handle.
		 */
	case VMCS_SM_CMD_MAPPED_VC_HDL_FROM_HDL:
		{
			struct vmcs_sm_ioctl_map ioparam;

			/* Get parameter data.
			 */
			if (copy_from_user(&ioparam,
					   (void *)arg, sizeof(ioparam)) != 0) {
				LOG_ERR
				    ("[%s]: failed to copy-from-user"
				     " for cmd %x", __func__, cmdnr);

				ret = -EFAULT;
				goto out;
			}

			/* Locate resource from GUID.
			 */
			resource =
			    vmcs_sm_acquire_resource(file_data, ioparam.handle);
			if (resource != NULL) {
				ioparam.handle = resource->res_handle;
				vmcs_sm_release_resource(resource, 0);
			} else {
				ioparam.handle = 0;
			}

			if (copy_to_user((void *)arg,
					 &ioparam, sizeof(ioparam)) != 0) {
				LOG_ERR
				    ("[%s]: failed to copy-to-user for cmd %x",
				     __func__, cmdnr);

				ret = -EFAULT;
			}

			/* Done.
			 */
			goto out;
		}
		break;

		/* Maps a user address given process and vc handle.
		 */
	case VMCS_SM_CMD_MAPPED_USR_ADDRESS:
		{
			struct vmcs_sm_ioctl_map ioparam;

			/* Get parameter data.
			 */
			if (copy_from_user(&ioparam,
					   (void *)arg, sizeof(ioparam)) != 0) {
				LOG_ERR
				    ("[%s]: failed to copy-from-user"
				     " for cmd %x", __func__, cmdnr);

				ret = -EFAULT;
				goto out;
			}

			/* Return the address information from the mapping,
			 * 0 (ie NULL) if it cannot locate the actual mapping.
			 */
			ioparam.addr =
			    vmcs_sm_usr_address_from_pid_and_usr_handle
			    (ioparam.pid, ioparam.handle);

			if (copy_to_user((void *)arg,
					 &ioparam, sizeof(ioparam)) != 0) {
				LOG_ERR
				    ("[%s]: failed to copy-to-user for cmd %x",
				     __func__, cmdnr);

				ret = -EFAULT;
			}

			/* Done.
			 */
			goto out;
		}
		break;

		/* Flush the cache for a given mapping.
		 */
	case VMCS_SM_CMD_FLUSH:
		{
			struct vmcs_sm_ioctl_cache ioparam;

			/* Get parameter data.
			 */
			if (copy_from_user(&ioparam,
					   (void *)arg, sizeof(ioparam)) != 0) {
				LOG_ERR
				    ("[%s]: failed to copy-from-user"
				     " for cmd %x", __func__, cmdnr);

				ret = -EFAULT;
				goto out;
			}

			/* Locate resource from GUID.
			 */
			resource =
			    vmcs_sm_acquire_resource(file_data, ioparam.handle);

			if ((resource != NULL) && resource->res_cached) {
				dma_addr_t phys_addr = 0;

				resource->res_stats[FLUSH]++;

				phys_addr =
				    (dma_addr_t)((uint32_t)
						 resource->res_base_mem &
						 0x3FFFFFFF);
				phys_addr += (dma_addr_t)mm_vc_mem_phys_addr;

				/* L1 cache flush */
				down_read(&current->mm->mmap_sem);
				vcsm_vma_cache_clean_page_range((unsigned long)
								ioparam.addr,
								(unsigned long)
								ioparam.addr +
								ioparam.size);
				up_read(&current->mm->mmap_sem);

				/* L2 cache flush */
				outer_clean_range(phys_addr,
						  phys_addr +
						  (size_t) ioparam.size);
			} else if (resource == NULL) {
				ret = -EINVAL;
				goto out;
			}

			if (resource)
				vmcs_sm_release_resource(resource, 0);

			/* Done.
			 */
			goto out;
		}
		break;

		/* Invalidate the cache for a given mapping.
		 */
	case VMCS_SM_CMD_INVALID:
		{
			struct vmcs_sm_ioctl_cache ioparam;

			/* Get parameter data.
			 */
			if (copy_from_user(&ioparam,
					   (void *)arg, sizeof(ioparam)) != 0) {
				LOG_ERR
				    ("[%s]: failed to copy-from-user"
				     " for cmd %x", __func__, cmdnr);

				ret = -EFAULT;
				goto out;
			}

			/* Locate resource from GUID.
			 */
			resource =
			    vmcs_sm_acquire_resource(file_data, ioparam.handle);

			if ((resource != NULL) && resource->res_cached) {
				dma_addr_t phys_addr = 0;

				resource->res_stats[INVALID]++;

				phys_addr =
				    (dma_addr_t)((uint32_t)
						 resource->res_base_mem &
						 0x3FFFFFFF);
				phys_addr += (dma_addr_t)mm_vc_mem_phys_addr;

				/* L2 cache invalidate */
				outer_inv_range(phys_addr,
						phys_addr +
						(size_t) ioparam.size);

				/* L1 cache invalidate */
				down_read(&current->mm->mmap_sem);
				vcsm_vma_cache_clean_page_range((unsigned long)
								ioparam.addr,
								(unsigned long)
								ioparam.addr +
								ioparam.size);
				up_read(&current->mm->mmap_sem);
			} else if (resource == NULL) {
				ret = -EINVAL;
				goto out;
			}

			if (resource)
				vmcs_sm_release_resource(resource, 0);

			/* Done.
			 */
			goto out;
		}
		break;

	default:
		{
			ret = -EINVAL;
			goto out;
		}
		break;
	}

out:
	return ret;
}

/* Device operations that we managed in this driver.
*/
static const struct file_operations vmcs_sm_ops = {
	.owner = THIS_MODULE,
	.unlocked_ioctl = vc_sm_ioctl,
	.open = vc_sm_open,
	.release = vc_sm_release,
	.mmap = vc_sm_mmap,
};

/* Creation of device.
*/
static int vc_sm_create_sharedmemory(void)
{
	int ret;

	if (sm_state == NULL) {
		ret = -ENOMEM;
		goto out;
	}

	/* Create a device class for creating dev nodes.
	 */
	sm_state->sm_class = class_create(THIS_MODULE, "vc-sm");
	if (IS_ERR(sm_state->sm_class)) {
		LOG_ERR("[%s]: unable to create device class", __func__);

		ret = PTR_ERR(sm_state->sm_class);
		goto out;
	}

	/* Create a character driver.
	 */
	ret = alloc_chrdev_region(&sm_state->sm_devid,
				  DEVICE_MINOR, 1, DEVICE_NAME);
	if (ret != 0) {
		LOG_ERR("[%s]: unable to allocate device number", __func__);
		goto out_dev_class_destroy;
	}

	cdev_init(&sm_state->sm_cdev, &vmcs_sm_ops);
	ret = cdev_add(&sm_state->sm_cdev, sm_state->sm_devid, 1);
	if (ret != 0) {
		LOG_ERR("[%s]: unable to register device", __func__);
		goto out_chrdev_unreg;
	}

	/* Create a device node.
	 */
	sm_state->sm_dev = device_create(sm_state->sm_class,
					 NULL,
					 MKDEV(MAJOR(sm_state->sm_devid),
					       DEVICE_MINOR), NULL,
					 DEVICE_NAME);
	if (IS_ERR(sm_state->sm_dev)) {
		LOG_ERR("[%s]: unable to create device node", __func__);
		ret = PTR_ERR(sm_state->sm_dev);
		goto out_chrdev_del;
	}

	goto out;

out_chrdev_del:
	cdev_del(&sm_state->sm_cdev);
out_chrdev_unreg:
	unregister_chrdev_region(sm_state->sm_devid, 1);
out_dev_class_destroy:
	class_destroy(sm_state->sm_class);
	sm_state->sm_class = NULL;
out:
	return ret;
}

/* Termination of the device.
*/
static int vc_sm_remove_sharedmemory(void)
{
	int ret;

	if (sm_state == NULL) {
		/* Nothing to do.
		 */
		ret = 0;
		goto out;
	}

	/* Remove the sharedmemory character driver.
	 */
	cdev_del(&sm_state->sm_cdev);

	/* Unregister region.
	 */
	unregister_chrdev_region(sm_state->sm_devid, 1);

	ret = 0;
	goto out;

out:
	return ret;
}

/* Videocore connected.
*/
static void vc_sm_connected_init(void)
{
	int ret;
	VCHI_INSTANCE_T vchi_instance;
	VCHI_CONNECTION_T *vchi_connection = NULL;

	LOG_INFO("[%s]: start", __func__);

	/* Allocate memory for the state structure.
	 */
	sm_state = kzalloc(sizeof(struct SM_STATE_T), GFP_KERNEL);
	if (sm_state == NULL) {
		LOG_ERR("[%s]: failed to allocate memory", __func__);

		ret = -ENOMEM;
		goto out;
	}

	mutex_init(&sm_state->lock);
	mutex_init(&sm_state->map_lock);

	/* Initialize and create a VCHI connection for the shared memory service
	 ** running on videocore.
	 */
	ret = vchi_initialise(&vchi_instance);
	if (ret != 0) {
		LOG_ERR("[%s]: failed to initialise VCHI instance (ret=%d)",
			__func__, ret);

		ret = -EIO;
		goto err_free_mem;
	}

	ret = vchi_connect(NULL, 0, vchi_instance);
	if (ret != 0) {
		LOG_ERR("[%s]: failed to connect VCHI instance (ret=%d)",
			__func__, ret);

		ret = -EIO;
		goto err_free_mem;
	}

	/* Initialize an instance of the shared memory service.
	 */
	sm_state->sm_handle =
	    vc_vchi_sm_init(vchi_instance, &vchi_connection, 1);
	if (sm_state->sm_handle == NULL) {
		LOG_ERR("[%s]: failed to initialize shared memory service",
			__func__);

		ret = -EPERM;
		goto err_free_mem;
	}

	/* Create a proc directory entry (root).
	 */
	sm_state->dir_root = proc_mkdir(PROC_DIR_ROOT_NAME, NULL);
	if (sm_state->dir_root == NULL) {
		LOG_ERR("[%s]: failed to create \'%s\' directory entry",
			__func__, PROC_DIR_ROOT_NAME);

		ret = -EPERM;
		goto err_stop_sm_service;
	}

	sm_state->debug = create_proc_entry(PROC_DEBUG, 0, sm_state->dir_root);
	if (sm_state->debug == NULL) {
		LOG_ERR("[%s]: failed to create \'%s\' entry",
			__func__, PROC_DEBUG);

		ret = -EPERM;
		goto err_remove_proc_dir;
	} else {
		sm_state->debug->read_proc = &vc_sm_debug_proc_read;
		sm_state->debug->write_proc = &vc_sm_debug_proc_write;
	}

	sm_state->dir_state.dir_entry = create_proc_entry(PROC_STATE,
							  0,
							  sm_state->dir_root);
	if (sm_state->dir_state.dir_entry == NULL) {
		LOG_ERR("[%s]: failed to create \'%s\' entry",
			__func__, PROC_STATE);

		ret = -EPERM;
		goto err_remove_proc_debug;
	} else {
		sm_state->dir_state.priv_data = NULL;
		sm_state->dir_state.proc_read = &vc_sm_global_state_proc_read;

		sm_state->dir_state.dir_entry->proc_fops = &vc_sm_proc_fops;
		sm_state->dir_state.dir_entry->data = &(sm_state->dir_state);
	}

	sm_state->dir_stats.dir_entry = create_proc_entry(PROC_STATS,
							  0,
							  sm_state->dir_root);
	if (sm_state->dir_stats.dir_entry == NULL) {
		LOG_ERR("[%s]: failed to create \'%s\' entry",
			__func__, PROC_STATS);

		ret = -EPERM;
		goto err_remove_proc_state;
	} else {
		sm_state->dir_stats.priv_data = NULL;
		sm_state->dir_stats.proc_read =
		    &vc_sm_global_statistics_proc_read;

		sm_state->dir_stats.dir_entry->proc_fops = &vc_sm_proc_fops;
		sm_state->dir_stats.dir_entry->data = &(sm_state->dir_stats);
	}

	/* Create the proc entry children.
	 */
	sm_state->dir_alloc =
	    proc_mkdir(PROC_DIR_ALLOC_NAME, sm_state->dir_root);
	if (sm_state->dir_alloc == NULL) {
		LOG_ERR("[%s]: failed to create \'%s\' directory entry",
			__func__, PROC_DIR_ALLOC_NAME);

		ret = -EPERM;
		goto err_remove_proc_statistics;
	}

	/* Create a shared memory device.
	 */
	ret = vc_sm_create_sharedmemory();
	if (ret != 0) {
		LOG_ERR("[%s]: failed to create shared memory device",
			__func__);
		goto err_remove_alloc_dir;
	}

	INIT_LIST_HEAD(&sm_state->map_list);
	INIT_LIST_HEAD(&sm_state->resource_list);

	sm_state->data_knl = vc_sm_create_priv_data(0);
	if (sm_state->data_knl == NULL) {
		LOG_ERR("[%s]: failed to create kernel private data tracker",
			__func__);
		goto err_remove_shared_memory;
	}

	/* Done!
	 */
	sm_inited = 1;
	goto out;

err_remove_shared_memory:
	vc_sm_remove_sharedmemory();
err_remove_alloc_dir:
	remove_proc_entry(PROC_DIR_ALLOC_NAME, sm_state->dir_root);
err_remove_proc_statistics:
	remove_proc_entry(PROC_STATS, sm_state->dir_root);
err_remove_proc_state:
	remove_proc_entry(PROC_STATE, sm_state->dir_root);
err_remove_proc_debug:
	remove_proc_entry(PROC_DEBUG, sm_state->dir_root);
err_remove_proc_dir:
	remove_proc_entry(PROC_DIR_ROOT_NAME, NULL);
err_stop_sm_service:
	vc_vchi_sm_stop(&sm_state->sm_handle);
err_free_mem:
	kfree(sm_state);
out:
	LOG_INFO("[%s]: end - returning %d", __func__, ret);
}

/* Driver loading.
*/
static int __init vc_sm_init(void)
{
	printk(KERN_INFO "vc-sm: Videocore shared memory driver\n");

	vchiq_add_connected_callback(vc_sm_connected_init);
	return 0;
}

/* Driver unloading.
*/
static void __exit vc_sm_exit(void)
{
	LOG_INFO("[%s]: start", __func__);

	if (sm_inited) {
		/* Remove shared memory device.
		 */
		vc_sm_remove_sharedmemory();

		/* Remove all proc entries.
		 */
		remove_proc_entry(PROC_DIR_ALLOC_NAME, sm_state->dir_root);
		remove_proc_entry(PROC_DEBUG, sm_state->dir_root);
		remove_proc_entry(PROC_STATE, sm_state->dir_root);
		remove_proc_entry(PROC_STATS, sm_state->dir_root);
		remove_proc_entry(PROC_DIR_ROOT_NAME, NULL);

		/* Stop the videocore shared memory service.
		 */
		vc_vchi_sm_stop(&sm_state->sm_handle);

		/* Free the memory for the state structure.
		 */
		mutex_destroy(&(sm_state->map_lock));
		kfree(sm_state);
	}

	LOG_INFO("[%s]: end", __func__);
}

#if defined(__KERNEL__)
/* Allocate a shared memory handle and block.
*/
int vc_sm_alloc(VC_SM_ALLOC_T * alloc, int *handle)
{
	struct vmcs_sm_ioctl_alloc ioparam = { 0 };
	int ret;
	struct SM_RESOURCE_T *resource;

	/* Validate we can work with this device.
	 */
	if (sm_state == NULL || alloc == NULL || handle == NULL) {
		LOG_ERR("[%s]: invalid input", __func__);
		return -EPERM;
	}

	ioparam.size = alloc->base_unit;
	ioparam.num = alloc->num_unit;
	ioparam.cached =
	    alloc->type == VC_SM_ALLOC_CACHED ? VMCS_SM_CACHE_VC : 0;

	ret = vc_sm_ioctl_alloc(sm_state->data_knl, &ioparam);

	if (ret == 0) {
		resource =
		    vmcs_sm_acquire_resource(sm_state->data_knl,
					     ioparam.handle);
		if (resource) {
			resource->pid = 0;
			vmcs_sm_release_resource(resource, 0);

			/* Assign valid handle at this time.
			 */
			*handle = ioparam.handle;
		} else {
			ret = -ENOMEM;
		}
	}

	return ret;
}

EXPORT_SYMBOL_GPL(vc_sm_alloc);

/* Get an internal resource handle mapped from the external one.
*/
int vc_sm_int_handle(int handle)
{
	struct SM_RESOURCE_T *resource;
	int ret = 0;

	/* Validate we can work with this device.
	 */
	if (sm_state == NULL || handle == 0) {
		LOG_ERR("[%s]: invalid input", __func__);
		return 0;
	}

	/* Locate resource from GUID.
	 */
	resource = vmcs_sm_acquire_resource(sm_state->data_knl, handle);
	if (resource) {
		ret = resource->res_handle;
		vmcs_sm_release_resource(resource, 0);
	}

	return ret;
}

EXPORT_SYMBOL_GPL(vc_sm_int_handle);

/* Free a previously allocated shared memory handle and block.
*/
int vc_sm_free(int handle)
{
	struct vmcs_sm_ioctl_free ioparam = { handle };

	/* Validate we can work with this device.
	 */
	if (sm_state == NULL || handle == 0) {
		LOG_ERR("[%s]: invalid input", __func__);
		return -EPERM;
	}

	return vc_sm_ioctl_free(sm_state->data_knl, &ioparam);
}

EXPORT_SYMBOL_GPL(vc_sm_free);

/* Lock a memory handle for use by kernel.
*/
int vc_sm_lock(int handle, VC_SM_LOCK_CACHE_MODE_T mode,
	       long unsigned int *data)
{
	struct vmcs_sm_ioctl_lock_unlock ioparam;
	int ret;

	/* Validate we can work with this device.
	 */
	if (sm_state == NULL || handle == 0 || data == NULL) {
		LOG_ERR("[%s]: invalid input", __func__);
		return -EPERM;
	}

	*data = 0;

	ioparam.handle = handle;
	ret = vc_sm_ioctl_lock(sm_state->data_knl,
			       &ioparam,
			       1,
			       ((mode ==
				 VC_SM_LOCK_CACHED) ? VMCS_SM_CACHE_HOST :
				VMCS_SM_CACHE_NONE), 0);

	*data = ioparam.addr;
	return ret;
}

EXPORT_SYMBOL_GPL(vc_sm_lock);

/* Unlock a memory handle in use by kernel.
*/
int vc_sm_unlock(int handle, int flush, int no_vc_unlock)
{
	struct vmcs_sm_ioctl_lock_unlock ioparam;

	/* Validate we can work with this device.
	 */
	if (sm_state == NULL || handle == 0) {
		LOG_ERR("[%s]: invalid input", __func__);
		return -EPERM;
	}

	ioparam.handle = handle;
	return vc_sm_ioctl_unlock(sm_state->data_knl,
				  &ioparam, flush, 0, no_vc_unlock);
}

EXPORT_SYMBOL_GPL(vc_sm_unlock);

/* Map a shared memory region for use by kernel.
*/
int vc_sm_map(int handle, unsigned int sm_addr, VC_SM_LOCK_CACHE_MODE_T mode,
	      long unsigned int *data)
{
	struct vmcs_sm_ioctl_lock_unlock ioparam;
	int ret;

	/* Validate we can work with this device.
	 */
	if (sm_state == NULL || handle == 0 || data == NULL || sm_addr == 0) {
		LOG_ERR("[%s]: invalid input", __func__);
		return -EPERM;
	}

	*data = 0;

	ioparam.handle = handle;
	ret = vc_sm_ioctl_lock(sm_state->data_knl,
			       &ioparam,
			       1,
			       ((mode ==
				 VC_SM_LOCK_CACHED) ? VMCS_SM_CACHE_HOST :
				VMCS_SM_CACHE_NONE), sm_addr);

	*data = ioparam.addr;
	return ret;
}

EXPORT_SYMBOL_GPL(vc_sm_map);
#endif

late_initcall(vc_sm_init);
module_exit(vc_sm_exit);

MODULE_AUTHOR("Broadcom");
MODULE_DESCRIPTION("VideoCore SharedMemory Driver");
MODULE_LICENSE("GPL v2");
