/* Linux boot Example

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/
#include <stdio.h>
#include <inttypes.h>
#include "sdkconfig.h"
#include "esp_system.h"
#include "esp_attr.h"
#include "esp_cpu.h"
#include "spi_flash_mmap.h"
#include "esp_partition.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "soc/rtc_cntl_reg.h"
#include "soc/timer_group_reg.h"
#include "soc/uart_reg.h"
#include "soc/soc.h"

#if CONFIG_IDF_TARGET_ESP32S3
#include "hal/mmu_hal.h"
#include "hal/mmu_types.h"
#include "hal/cache_hal.h"
#include "hal/cache_ll.h"
#endif

/*
 * The kernel image is XIP: it executes straight out of memory-mapped flash and
 * every absolute reference in it is baked in at link time. Its built-in
 * devicetree describes the world it expects:
 *
 *   flash@42000000 { compatible = "mtd-rom"; reg = <0x42000000 0x1000000>; }
 *   memory@0       { reg = <0x3d800000 0x800000>; }
 *   chosen { bootargs = "... console=ttyS0,115200n8 root=mtd:rootfs"; }
 *
 * So before jumping we have to hand it exactly that: the whole 16 MB of flash
 * identity-mapped on the instruction bus at 0x42000000 (which also puts the
 * IDF partition table at 0x42008000, where the kernel's
 * "esp,esp32-partition-table" parser looks for it), and 8 MB of PSRAM at
 * 0x3d800000.
 *
 * The kernel's link base therefore has to equal 0x42000000 + <flash offset of
 * the "linux" partition>; partition_table.esp32s3 puts it at 0x120000 to match
 * the 0x42120000 the image was built for.
 */
#define LINUX_FLASH_VADDR   0x42000000
#define LINUX_FLASH_SIZE    (16 * 1024 * 1024)
#define LINUX_RAM_VADDR     0x3d800000
#define LINUX_RAM_SIZE      (8 * 1024 * 1024)

/*
 * NOTE: this gets Linux to run, but not to mount root. The kernel's flash MTD
 * is not usable standalone: drivers/mtd/maps/physmap-core.c picks its probe
 * type from the "controller" phandle on flash@42000000, which selects the
 * "esp32-ipc-flash" chip driver, and that driver is only registered once
 * esp,esp32-ipc has come up against a peer on the other core. Without it
 * do_map_probe() returns NULL ("map_probe failed") and root=mtd:rootfs cannot
 * resolve. Deleting the phandle is not a fallback either -- the patched
 * map_rom_probe() then calls of_find_device_by_node(NULL), which matches the
 * first platform device with no of_node, and dereferences its NULL drvdata.
 *
 * Getting to a shell needs an ESP-side IPC responder, which also means this app
 * has to survive the remap below instead of stalling. Neither exists yet.
 */

/* Magic value that write-enables the WDT config registers. Same key for the
   RTC WDT and both timer-group WDTs. */
#define WDT_WKEY_VALUE      0x50D83AA1

static void IRAM_ATTR disable_all_watchdogs(void)
{
	/* The timer-group WDTs are left write-protected by IDF (the interrupt
	   WDT locks TG1 in wdt_hal_init), so unlock before clearing WDT_EN --
	   otherwise the write is silently dropped and TG1WDT_SYS_RST still
	   fires a few hundred ms after we jump. */
	for (int i = 0; i < 2; i++) {
		REG_WRITE(TIMG_WDTWPROTECT_REG(i), WDT_WKEY_VALUE);
		REG_CLR_BIT(TIMG_WDTCONFIG0_REG(i), TIMG_WDT_EN);
		REG_WRITE(TIMG_WDTFEED_REG(i), 1);
		REG_WRITE(TIMG_WDTWPROTECT_REG(i), 0);
	}

	REG_WRITE(RTC_CNTL_WDTWPROTECT_REG, WDT_WKEY_VALUE);
	REG_CLR_BIT(RTC_CNTL_WDTCONFIG0_REG, RTC_CNTL_WDT_EN);
	REG_WRITE(RTC_CNTL_WDTWPROTECT_REG, 0);
}

/*
 * Console output for the window between "cache disabled" and "kernel running":
 * printf() is in flash and the flash mapping is gone, so poke UART0's FIFO
 * directly. Same port the IDF log and the kernel's earlycon use.
 */
static void IRAM_ATTR dbg_putc(char c)
{
	while (((REG_READ(UART_STATUS_REG(0)) >> UART_TXFIFO_CNT_S) &
		UART_TXFIFO_CNT_V) > 100) {
	}
	REG_WRITE(UART_FIFO_REG(0), c);
}

static void IRAM_ATTR dbg_str(const char *s)
{
	while (*s)
		dbg_putc(*s++);
}

static void IRAM_ATTR dbg_hex(const char *label, uint32_t v)
{
	dbg_str(label);
	for (int i = 28; i >= 0; i -= 4)
		dbg_putc(DRAM_STR("0123456789abcdef")[(v >> i) & 0xf]);
	dbg_putc('\r');
	dbg_putc('\n');
}

static void __attribute__((noreturn)) IRAM_ATTR jump_to_kernel(uint32_t entry,
							     uint32_t bootparam)
{
	/* arch/xtensa/kernel/head.S starts with "wsr a2, excsave1" to stash the
	   boot parameter list pointer, so a2 is where it wants to find it. */
	asm volatile (
		"mov   a2, %1\n"
		"jx    %0\n"
		:: "r"(entry), "r"(bootparam)
		: "a2", "memory"
	);

	__builtin_unreachable();
}

#if CONFIG_IDF_TARGET_ESP32S3

/*
 * Reprogram the flash/PSRAM MMU into the layout the kernel was linked for and
 * jump. This tears down the mapping the running app itself is executing from,
 * so everything below the cache_hal_disable() must live in IRAM and must not
 * touch anything in .rodata / .flash.text.
 */
static void __attribute__((noreturn)) IRAM_ATTR remap_and_go(uint32_t entry)
{
	uint32_t mapped;
	cache_bus_mask_t bus_mask;

	esp_cpu_stall(!esp_cpu_get_core_id());
	portDISABLE_INTERRUPTS();
	disable_all_watchdogs();

	cache_hal_disable(CACHE_LL_LEVEL_EXT_MEM, CACHE_TYPE_ALL);

	/* Identity-map all of flash on the instruction bus, and the PSRAM where
	   the kernel's memory@ node says its RAM lives. Both CPUs have their
	   own MMU on this chip, so program both. */
	for (int id = 0; id < 2; id++) {
		mmu_hal_map_region(id, MMU_TARGET_FLASH0, LINUX_FLASH_VADDR,
				   0, LINUX_FLASH_SIZE, &mapped);
		mmu_hal_map_region(id, MMU_TARGET_PSRAM0, LINUX_RAM_VADDR,
				   0, LINUX_RAM_SIZE, &mapped);
	}

	bus_mask = cache_ll_l1_get_bus(0, LINUX_FLASH_VADDR, LINUX_FLASH_SIZE);
	bus_mask |= cache_ll_l1_get_bus(0, LINUX_RAM_VADDR, LINUX_RAM_SIZE);
	cache_ll_l1_enable_bus(0, bus_mask);
	cache_ll_l1_enable_bus(1, bus_mask);

	cache_hal_enable(CACHE_LL_LEVEL_EXT_MEM, CACHE_TYPE_ALL);

	/* The old mapping is still sitting in the cache, and the app's IROM
	   covered 0x42000000..0x42020000 -- which is where the kernel's
	   partition-table parser will look (0x42008000). Drop all of it. */
	cache_ll_invalidate_all(CACHE_LL_LEVEL_EXT_MEM, CACHE_TYPE_ALL,
				CACHE_LL_ID_ALL);


	/* Everything the kernel is about to depend on, read back through the
	   new mapping: flash image magic at the identity base, the first word
	   of the kernel (wsr a2, excsave1), the FDT magic where we staged it,
	   and the tag list itself. */
	dbg_str(DRAM_STR("\r\n[remap]\r\n"));
	dbg_hex(DRAM_STR("flash@42000000 = 0x"), *(volatile uint32_t *)LINUX_FLASH_VADDR);
	dbg_hex(DRAM_STR("kernel entry   = 0x"), *(volatile uint32_t *)entry);
	dbg_hex(DRAM_STR("psram probe    = 0x"), *(volatile uint32_t *)LINUX_RAM_VADDR);
	dbg_str(DRAM_STR("jumping\r\n"));

	jump_to_kernel(entry, 0);
}

static void map_flash_and_go(void)
{
	const esp_partition_t *part;

	part = esp_partition_find_first(ESP_PARTITION_TYPE_ANY,
					ESP_PARTITION_SUBTYPE_ANY, "linux");
	if (part == NULL) {
		printf("no 'linux' partition -- check partition_table.esp32s3\n");
		return;
	}

	uint32_t entry = LINUX_FLASH_VADDR + part->address;

	printf("linux partition at 0x%08" PRIx32 ", size 0x%08" PRIx32 "\n",
	       (uint32_t)part->address, (uint32_t)part->size);
	printf("kernel entry 0x%08" PRIx32 " (must equal the image's XIP link address)\n",
	       entry);

	if (part->address + part->size > LINUX_FLASH_SIZE) {
		printf("partition runs past the %d MB the kernel's DT maps\n",
		       LINUX_FLASH_SIZE >> 20);
		return;
	}
#if !CONFIG_SPIRAM
	printf("CONFIG_SPIRAM is off -- the kernel needs 8 MB of PSRAM at 0x%08x\n",
	       LINUX_RAM_VADDR);
	return;
#else
	printf("jumping to kernel...\n");
	fflush(stdout);
	remap_and_go(entry);
#endif
}

#else /* ESP32: the kernel there is linked for wherever IDF maps the partition */

static const void *map_partition(const char *name)
{
	const void *ptr;
	spi_flash_mmap_handle_t handle;
	const esp_partition_t *part;

	part = esp_partition_find_first(ESP_PARTITION_TYPE_ANY,
					ESP_PARTITION_SUBTYPE_ANY, name);
	if (part == NULL) {
		printf("partition '%s' not found\n", name);
		abort();
	}
	if (esp_partition_mmap(part, 0, part->size, SPI_FLASH_MMAP_INST,
			       &ptr, &handle) != ESP_OK) {
		abort();
	}
	return ptr;
}

static void map_flash_and_go(void)
{
	const void *kernel = map_partition("linux");
	const void *rootfs = map_partition("rootfs");

	printf("kernel = %p, rootfs = %p\n", kernel, rootfs);
	fflush(stdout);

	portDISABLE_INTERRUPTS();
	disable_all_watchdogs();
	jump_to_kernel((uint32_t)kernel, 0);
}

#endif

static void linux_task(void *p)
{
	map_flash_and_go();
	printf("kernel did not start; restarting\n");
	esp_restart();
}

void app_main(void)
{
	printf("Starting kernel bootloader...\n");

	xTaskCreatePinnedToCore(linux_task, "linux_task", 4096, NULL, 5, NULL,
				CONFIG_LINUX_CORE);
	vTaskSuspend(NULL);
}
