/*
 *  Copyright (C) 2009 LSI Corporation
 *
 * See file CREDITS for list of people who contributed to this
 * project.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of
 * the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.	 See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston,
 * MA 02111-1307 USA
 */

#include <linux/module.h>
#include <linux/jiffies.h>

#include <linux/io.h>

#include "lsi-ncr.h"

#ifdef CONFIG_ARCH_AXXIA
#define NCA_PHYS_ADDRESS 0x002020100000ULL
#define APB2SER_PHY_PHYS_ADDRESS 0x002010000000ULL
#else
#define NCA_PHYS_ADDRESS 0x002000520000ULL
#endif

static void __iomem *nca_address;
#ifdef APB2SER_PHY_PHYS_ADDRESS
static void __iomem *apb2ser0_address;
#endif /* APB2SER_PHY_PHYS_ADDRESS */

#define WFC_TIMEOUT (400000)

static DEFINE_RAW_SPINLOCK(ncr_spin_lock);
static unsigned long flags;

#define LOCK_DOMAIN 0

typedef union {
	unsigned long raw;
	struct {
#ifdef __BIG_ENDIAN
		unsigned long start_done:1;
		unsigned long unused:6;
		unsigned long local_bit:1;
		unsigned long status:2;
		unsigned long byte_swap_enable:1;
		unsigned long cfg_cmpl_int_enable:1;
		unsigned long cmd_type:4;
		unsigned long dbs:16;
#else
		unsigned long dbs:16;
		unsigned long cmd_type:4;
		unsigned long cfg_cmpl_int_enable:1;
		unsigned long byte_swap_enable:1;
		unsigned long status:2;
		unsigned long local_bit:1;
		unsigned long unused:6;
		unsigned long start_done:1;
#endif
	} __packed bits;
} __packed command_data_register_0_t;

typedef union {
	unsigned long raw;
	struct {
		unsigned long target_address:32;
	} __packed bits;
} __packed command_data_register_1_t;

typedef union {
	unsigned long raw;
	struct {
#ifdef __BIG_ENDIAN
		unsigned long unused:16;
		unsigned long target_node_id:8;
		unsigned long target_id_address_upper:8;
#else
		unsigned long target_id_address_upper:8;
		unsigned long target_node_id:8;
		unsigned long unused:16;
#endif
	} __packed bits;
} __packed command_data_register_2_t;

#ifdef CONFIG_ARM

/*
 * like iowrite32be but without the barrier.
 * The iowmb barrier in the standard macro includes a outer_cache_sync
 * which we don't want for Axxia register IO.
 */
#define axxia_write32be(v, p) \
	({  __raw_writel((__force __u32)cpu_to_be32(v), p); })

/*
  ----------------------------------------------------------------------
  ncr_register_read
*/

inline unsigned long
ncr_register_read(unsigned *address)
{
	unsigned long value;

	value = ioread32be(address);

	return value;
}

/*
  ----------------------------------------------------------------------
  ncr_register_write
*/

void
inline ncr_register_write(const unsigned value, unsigned *address)
{
	axxia_write32be(value, address);
	asm volatile ("mcr p15,0,%0,c7,c5,4" : : "r" (0));  /* isb */
}

#else

/*
  ----------------------------------------------------------------------
  ncr_register_read
*/

inline unsigned long
ncr_register_read(unsigned *address)
{
	unsigned long value;

	value = in_be32((unsigned *)address);

	return value;
}

/*
  ----------------------------------------------------------------------
  ncr_register_write
*/

inline void
ncr_register_write(const unsigned value, unsigned *address)
{
	out_be32(address, value);
}

#endif

/*
  ------------------------------------------------------------------------------
  ncr_lock
*/

static int
ncr_lock(int domain)
{
	unsigned long offset;
	unsigned long value;
	unsigned long timeout = jiffies + msecs_to_jiffies(1000);
	command_data_register_0_t cdr0;

	raw_spin_lock_irqsave(&ncr_spin_lock, flags);
	offset = (0xff80 + (domain * 4));

	do {
		value = ncr_register_read((unsigned *)(nca_address + offset));
	} while ((0 != value) && (time_before(jiffies, timeout)));

	if (!(time_before(jiffies, timeout))) {
		raw_spin_unlock_irqrestore(&ncr_spin_lock, flags);
		pr_err("ncr_lock() Timeout!\n");
		BUG();

		return -1;
	}
	/*
	  Make sure any previous commands completed, and check for errors.
	*/
	timeout = jiffies + msecs_to_jiffies(1000);

	do {
		cdr0.raw =
			ncr_register_read((unsigned *)(nca_address + 0xf0));
	} while ((0x1 == cdr0.bits.status) &&
		(time_before(jiffies, timeout)));

	if (!(time_before(jiffies, timeout))) {
		raw_spin_unlock_irqrestore(&ncr_spin_lock, flags);
		pr_err("ncr_lock() Previous command didn't complete!\n");
		BUG();

		return -1;
	}
	if (0x2 == cdr0.bits.status)
		pr_err("Previous ncr access failed!\n");

	return 0;
}
EXPORT_SYMBOL(ncr_lock);

/*
  ------------------------------------------------------------------------------
  ncr_unlock
*/

static void
ncr_unlock(int domain)
{
	unsigned long offset;

	offset = (0xff80 + (domain * 4));
	ncr_register_write(0, (unsigned *)(nca_address + offset));
	raw_spin_unlock_irqrestore(&ncr_spin_lock, flags);
}
EXPORT_SYMBOL(ncr_unlock);

/*
  ======================================================================
  ======================================================================
  Public Interface
  ======================================================================
  ======================================================================
*/

/*
  ----------------------------------------------------------------------
  ncr_read
*/

int
ncr_read(unsigned long region, unsigned long address, int number,
	void *buffer)
{
	command_data_register_0_t cdr0;
	command_data_register_1_t cdr1;
	command_data_register_2_t cdr2;
	int wfc_timeout = WFC_TIMEOUT;

	if (NULL == nca_address)
		return -1;

#ifdef APB2SER_PHY_PHYS_ADDRESS
	if (NULL == apb2ser0_address)
		return -1;
#endif /* APB2SER_PHY_PHYS_ADDRESS */

	if (0 != ncr_lock(LOCK_DOMAIN))
		return -1;

	if ((NCP_NODE_ID(region) != 0x0153) && (NCP_NODE_ID(region) != 0x115)) {
		/*
		Set up the read command.
		*/

		cdr2.raw = 0;
		cdr2.bits.target_node_id = NCP_NODE_ID(region);
		cdr2.bits.target_id_address_upper = NCP_TARGET_ID(region);
		ncr_register_write(cdr2.raw, (unsigned *) (nca_address + 0xf8));

		cdr1.raw = 0;
		cdr1.bits.target_address = (address >> 2);
		ncr_register_write(cdr1.raw, (unsigned *) (nca_address + 0xf4));

		cdr0.raw = 0;
		cdr0.bits.start_done = 1;

		if (0xff == cdr2.bits.target_id_address_upper)
			cdr0.bits.local_bit = 1;

		cdr0.bits.cmd_type = 4;
		/* TODO: Verify number... */
		cdr0.bits.dbs = (number - 1);
		ncr_register_write(cdr0.raw, (unsigned *) (nca_address + 0xf0));
		/* Memory barrier */
		mb();

		/*
		Wait for completion.
		*/

		do {
			--wfc_timeout;
			cdr0.raw =
				ncr_register_read((unsigned *)
						  (nca_address + 0xf0));
		} while (1 == cdr0.bits.start_done && 0 < wfc_timeout);

		if (0 == wfc_timeout) {
			ncr_unlock(LOCK_DOMAIN);
			pr_err("ncr_read() Timeout!\n");
			BUG();

			return -1;
		}

		if (0x3 != cdr0.bits.status) {
			ncr_unlock(LOCK_DOMAIN);
			pr_err("ncr_write() failed: 0x%x\n",
			       cdr0.bits.status);

			return -1;
		}

		/*
		Copy data words to the buffer.
		*/

		address = (unsigned long)(nca_address + 0x1000);
		while (4 <= number) {
			*((unsigned long *) buffer) =
				ncr_register_read((unsigned *) address);
			address += 4;
			buffer += 4;
			number -= 4;
		}

		if (0 < number) {
			unsigned long temp =
				ncr_register_read((unsigned *) address);
			memcpy((void *) buffer, &temp, number);
		}
	} else {
#ifdef APB2SER_PHY_PHYS_ADDRESS
		if (NCP_NODE_ID(region) != 0x115) {
			void __iomem *targ_address = apb2ser0_address +
				(address & (~0x3));
			/*
			* Copy data words to the buffer.
			*/

			while (4 <= number) {
				*((unsigned long *) buffer) =
					*((unsigned long *) targ_address);
				targ_address += 4;
				number -= 4;
			}
		} else {
			void __iomem *base;

			if (0xffff < address) {
				ncr_unlock(LOCK_DOMAIN);
				return -1;
			}

			switch (NCP_TARGET_ID(region)) {
			case 0:
				base = (apb2ser0_address + 0x1e0);
				break;
			case 1:
				base = (apb2ser0_address + 0x1f0);
				break;
			case 2:
				base = (apb2ser0_address + 0x200);
				break;
			case 3:
				base = (apb2ser0_address + 0x210);
				break;
			case 4:
				base = (apb2ser0_address + 0x220);
				break;
			case 5:
				base = (apb2ser0_address + 0x230);
				break;
			default:
				ncr_unlock(LOCK_DOMAIN);
				return -1;
			}
			if ((NCP_TARGET_ID(region) == 0x1) ||
				(NCP_TARGET_ID(region) == 0x4)) {
				writel((0x84c00000 + address), (base + 4));
			} else {
				writel((0x85400000 + address), (base + 4));
			}
			do {
				--wfc_timeout;
				*((unsigned long *) buffer) =
					readl(base + 4);
			} while (0 != (*((unsigned long *) buffer) & 0x80000000)
					&& 0 < wfc_timeout);

			if (0 == wfc_timeout) {
				ncr_unlock(LOCK_DOMAIN);
				return -1;
			}

			if ((NCP_TARGET_ID(region) == 0x1) ||
				(NCP_TARGET_ID(region) == 0x4)) {
				*((unsigned short *) buffer) =
					readl(base + 8);
			} else {
				*((unsigned long *) buffer) =
					readl(base + 8);
			}

		}
#else
		ncr_unlock(LOCK_DOMAIN);
		return -1;
#endif /* APB2SER_PHY_PHYS_ADDRESS */
	}

	ncr_unlock(LOCK_DOMAIN);

	return 0;
}
EXPORT_SYMBOL(ncr_read);

/*
  ----------------------------------------------------------------------
  ncr_write
*/

int
ncr_write(unsigned long region, unsigned long address, int number,
	  void *buffer)
{
	command_data_register_0_t cdr0;
	command_data_register_1_t cdr1;
	command_data_register_2_t cdr2;
	unsigned long data_word_base;
	int dbs = (number - 1);
	int wfc_timeout = WFC_TIMEOUT;

	if (NULL == nca_address)
		return -1;

#ifdef APB2SER_PHY_PHYS_ADDRESS
	if (NULL == apb2ser0_address)
		return -1;
#endif /* APB2SER_PHY_PHYS_ADDRESS */

	if (0 != ncr_lock(LOCK_DOMAIN))
		return -1;

	if ((NCP_NODE_ID(region) != 0x0153) && (NCP_NODE_ID(region) != 0x115)) {
		/*
		  Set up the write.
		*/

		cdr2.raw = 0;
		cdr2.bits.target_node_id = NCP_NODE_ID(region);
		cdr2.bits.target_id_address_upper = NCP_TARGET_ID(region);
		ncr_register_write(cdr2.raw, (unsigned *) (nca_address + 0xf8));

		cdr1.raw = 0;
		cdr1.bits.target_address = (address >> 2);
		ncr_register_write(cdr1.raw, (unsigned *) (nca_address + 0xf4));

		/*
		  Copy from buffer to the data words.
		*/

		data_word_base = (unsigned long)(nca_address + 0x1000);

		while (4 <= number) {
			ncr_register_write(*((unsigned long *) buffer),
					(unsigned *) data_word_base);
			data_word_base += 4;
			buffer += 4;
			number -= 4;
		}

		if (0 < number) {
			unsigned long temp = 0;

			memcpy((void *) &temp, (void *) buffer, number);
			ncr_register_write(temp, (unsigned *) data_word_base);
			data_word_base += number;
			buffer += number;
			number = 0;
		}

		cdr0.raw = 0;
		cdr0.bits.start_done = 1;

		if (0xff == cdr2.bits.target_id_address_upper)
			cdr0.bits.local_bit = 1;

		cdr0.bits.cmd_type = 5;
		/* TODO: Verify number... */
		cdr0.bits.dbs = dbs;
		ncr_register_write(cdr0.raw, (unsigned *) (nca_address + 0xf0));
		/* Memory barrier */
		mb();

		/*
		Wait for completion.
		*/

		do {
			--wfc_timeout;
			cdr0.raw =
				ncr_register_read((unsigned *)
						  (nca_address + 0xf0));
		} while (1 == cdr0.bits.start_done && 0 < wfc_timeout);

		if (0 == wfc_timeout) {
			ncr_unlock(LOCK_DOMAIN);
			pr_err("ncr_write() Timeout!\n");
			BUG();

			return -1;
		}

		/*
		Check status.
		*/

		if ((0x3 != cdr0.bits.status) && (0x00c00000) >> 22) {
			unsigned long status;

			status = ncr_register_read((unsigned *)(nca_address +
								0xe4));
			ncr_unlock(LOCK_DOMAIN);
			pr_err("ncr_write() Error: 0x%x 0x%lx\n",
			       cdr0.bits.status, status);

			return status;
		}
	} else {
#ifdef APB2SER_PHY_PHYS_ADDRESS
	if (NCP_NODE_ID(region) != 0x115) {
		void __iomem *targ_address = apb2ser0_address +
					     (address & (~0x3));
		/*
		  Copy from buffer to the data words.
		*/

		while (4 <= number) {
			*((unsigned long *) targ_address) =
				*((unsigned long *) buffer);
			targ_address += 4;
			number -= 4;
		}
	} else {
		void __iomem *base;

		if (0xffff < address) {
			ncr_unlock(LOCK_DOMAIN);
			return -1;
		}

		switch (NCP_TARGET_ID(region)) {
		case 0:
			base = (apb2ser0_address + 0x1e0);
			break;
		case 1:
			base = (apb2ser0_address + 0x1f0);
			break;
		case 2:
			base = (apb2ser0_address + 0x200);
			break;
		case 3:
			base = (apb2ser0_address + 0x210);
			break;
		case 4:
			base = (apb2ser0_address + 0x220);
			break;
		case 5:
			base = (apb2ser0_address + 0x230);
			break;
		default:
			ncr_unlock(LOCK_DOMAIN);
			return -1;
		}
		if ((NCP_TARGET_ID(region) == 0x1) ||
				(NCP_TARGET_ID(region) == 0x4)) {
			writel(*((unsigned short *) buffer), base);
			writel((0xc4c00000 + address), (base + 4));
		} else {
			writel(*((unsigned long *) buffer), base);
			writel((0xc5400000 + address), (base + 4));
		}
			do {
				--wfc_timeout;
				*((unsigned long *) buffer) =
					readl(base + 4);
			} while (0 != (*((unsigned long *) buffer) & 0x80000000)
				&& 0 < wfc_timeout);

			if (0 == wfc_timeout) {
				ncr_unlock(LOCK_DOMAIN);
				return -1;
			}
		}
#else
		ncr_unlock(LOCK_DOMAIN);
		return -1;
#endif /* APB2SER_PHY_PHYS_ADDRESS */
	}

	ncr_unlock(LOCK_DOMAIN);

	return 0;
}
EXPORT_SYMBOL(ncr_write);

/*
  ----------------------------------------------------------------------
  ncr_init
*/

int
ncr_init(void)
{
	nca_address = ioremap(NCA_PHYS_ADDRESS, 0x20000);

#ifdef APB2SER_PHY_PHYS_ADDRESS
	apb2ser0_address = ioremap(APB2SER_PHY_PHYS_ADDRESS, 0x10000);
#endif /* APB2SER_PHY_PHYS_ADDRESS */

	pr_info("ncr: available\n");

	return 0;
}


module_init(ncr_init);

/*
  ----------------------------------------------------------------------
  ncr_exit
*/

void __exit
ncr_exit(void)
{
	/* Unmap the NCA. */
	if (NULL != nca_address)
		iounmap(nca_address);

#ifdef APB2SER_PHY_PHYS_ADDRESS
	/* Unmap the APB2SER0 PHY. */
	if (NULL != apb2ser0_address)
		iounmap(apb2ser0_address);
#endif /* APB2SER_PHY_PHYS_ADDRESS */
}


module_exit(ncr_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Register Ring access for LSI's ACP board");
