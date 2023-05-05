/* Linux boot Example

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/
#include <stdio.h>
#include "sdkconfig.h"
#include "esp_system.h"
#include "spi_flash_mmap.h"
#include "esp_partition.h"

static void IRAM_ATTR dump_data(unsigned long base, unsigned long size)
{
	unsigned long i, j;

	for (i = 0; i < size; i += 65536) {
		unsigned long sum = 0;
		for (j = 0; j < 16384; ++j) {
			sum += j[(unsigned long*)(base + i)];
		}
		printf("%08lx..%08lx: %08lx\n", base + i, base + i + 65535, sum);
	}
}

static const void * IRAM_ATTR map_partition(const char *name, unsigned long off)
{
	const void *ptr;
	spi_flash_mmap_handle_t handle;
	esp_partition_iterator_t it;
	const esp_partition_t *part;
	esp_err_t rv;

	it = esp_partition_find(ESP_PARTITION_TYPE_ANY, ESP_PARTITION_SUBTYPE_ANY, name);
	part = esp_partition_get(it);
	rv = esp_partition_mmap(part, off, part->size - off, SPI_FLASH_MMAP_INST, &ptr, &handle);
	if (rv != ESP_OK) {
		printf("esp_partition_mmap: %d\n", rv);
		abort();
	}
	dump_data((unsigned long)ptr, part->size);
	return ptr;
}

static void IRAM_ATTR map_flash_and_go(void)
{
	const void *ptr0, *ptr;

	ptr0 = map_partition("linux", 0);
	printf("ptr = %p\n", ptr0);

	ptr = map_partition("rootfs", 0);
	printf("ptr = %p\n", ptr);

	spi_flash_mmap_dump();

	asm volatile ("jx %0" :: "r"(ptr0));
}

void app_main(void)
{
    map_flash_and_go();
    esp_restart();
}
