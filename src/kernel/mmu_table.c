#include <kernel/mmu_table.h>
#include <kernel/page_alloc.h>

#define MMU_TABLE_MAX_PAGES 67UL
#define MMU_TABLE_MAX_NAME_LEN 24UL

static unsigned long table_page_addresses[MMU_TABLE_MAX_PAGES];
static char table_page_names[MMU_TABLE_MAX_PAGES][MMU_TABLE_MAX_NAME_LEN];
static unsigned long table_page_count;
static unsigned long table_pages_used;

static void copy_name(char *destination, const char *source)
{
    unsigned long index;

    for (index = 0UL;
         index < (MMU_TABLE_MAX_NAME_LEN - 1UL) && source[index] != '\0';
         index++) {
        destination[index] = source[index];
    }
    destination[index] = '\0';
}

static int name_has_prefix(const char *name, const char *prefix)
{
    unsigned long index;

    for (index = 0UL; prefix[index] != '\0'; index++) {
        if (name[index] != prefix[index]) {
            return 0;
        }
    }
    return 1;
}

static void set_chunk_name(char *destination, const char *prefix,
                           unsigned long chunk_index)
{
    char digits[21];
    unsigned long digit_count = 0UL;
    unsigned long output_index = 0UL;
    unsigned long index;

    do {
        digits[digit_count++] = (char)('0' + (chunk_index % 10UL));
        chunk_index /= 10UL;
    } while (chunk_index != 0UL && digit_count < sizeof(digits));

    for (index = 0UL;
         prefix[index] != '\0' && output_index < (MMU_TABLE_MAX_NAME_LEN - 1UL);
         index++) {
        destination[output_index++] = prefix[index];
    }

    while (digit_count > 0UL && output_index < (MMU_TABLE_MAX_NAME_LEN - 1UL)) {
        destination[output_index++] = digits[--digit_count];
    }
    destination[output_index] = '\0';
}

void mmu_table_registry_reset(void)
{
    table_page_count = 0UL;
    table_pages_used = 0UL;
}

void mmu_table_record_page(unsigned long address, const char *name)
{
    if (table_page_count >= MMU_TABLE_MAX_PAGES) {
        return;
    }

    table_page_addresses[table_page_count] = address;
    copy_name(table_page_names[table_page_count], name);
    table_page_count++;
    table_pages_used++;
}

unsigned long *mmu_table_alloc_named_page(const char *name)
{
    unsigned long *table = (unsigned long *)page_alloc();

    if (table != (unsigned long *)0) {
        mmu_table_record_page((unsigned long)table, name);
    }
    return table;
}

unsigned long *mmu_table_alloc_chunk_page(const char *prefix,
                                          unsigned long chunk_index)
{
    unsigned long *table = (unsigned long *)page_alloc();
    char name[MMU_TABLE_MAX_NAME_LEN];

    if (table == (unsigned long *)0) {
        return (unsigned long *)0;
    }

    set_chunk_name(name, prefix, chunk_index);
    mmu_table_record_page((unsigned long)table, name);
    return table;
}

void mmu_table_release_boot_ttbr0(unsigned long new_root)
{
    unsigned long read_index;
    unsigned long write_index = 0UL;

    for (read_index = 0UL; read_index < table_page_count; read_index++) {
        const char *name = table_page_names[read_index];

        if (name_has_prefix(name, "t1-")) {
            if (write_index != read_index) {
                table_page_addresses[write_index] = table_page_addresses[read_index];
                copy_name(table_page_names[write_index], name);
            }
            write_index++;
            continue;
        }

        page_free((void *)table_page_addresses[read_index]);
        if (table_pages_used > 0UL) {
            table_pages_used--;
        }
    }

    table_page_count = write_index;
    mmu_table_record_page(new_root, "t0-empty-root");
}

unsigned long mmu_table_page_count(void)
{
    return table_page_count;
}

unsigned long mmu_table_page_address(unsigned long index)
{
    return index < table_page_count ? table_page_addresses[index] : 0UL;
}

const char *mmu_table_page_name(unsigned long index)
{
    return index < table_page_count ? table_page_names[index]
                                    : "mmu-table-invalid";
}

unsigned long mmu_table_pages_used(void)
{
    return table_pages_used;
}
