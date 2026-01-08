#include "acpi.h"

#include "console.h"
#include "multiboot2.h"
#include "string.h"
#include "util.h"

namespace acpi {

    static SDTHeader *root_table = nullptr;
    static bool is_xsdt = false;

    struct multiboot_tag_acpi {
        uint32_t type;
        uint32_t size;
        uint8_t rsdp[0];
    } __attribute__((packed));

    void init(uint64_t mb_phys_addr) {
        uint8_t *mb_info = (uint8_t *)p2v(mb_phys_addr);
        uint32_t total_mb_size = *(uint32_t *)mb_info;

        multiboot_tag_acpi *acpi_old = nullptr;
        multiboot_tag_acpi *acpi_new = nullptr;

        for (uint8_t *tag = mb_info + 8; tag < mb_info + total_mb_size;
             tag += ((*(uint32_t *)(tag + 4) + 7) & ~7)) {
            uint32_t type = *(uint32_t *)tag;
            if (type == 0) break;

            if (type == 15) acpi_new = (multiboot_tag_acpi *)tag;
            if (type == 14) acpi_old = (multiboot_tag_acpi *)tag;
        }

        if (acpi_new) {
            XSDP *xsdp = (XSDP *)acpi_new->rsdp;
            root_table = (SDTHeader *)p2v(xsdp->xsdt_address);
            is_xsdt = true;
            console::printf("[ACPI] Using XSDT at %p", root_table);
        } else if (acpi_old) {
            RSDP *rsdp = (RSDP *)acpi_old->rsdp;
            root_table = (SDTHeader *)p2v(rsdp->rsdt_address);
            is_xsdt = false;
            console::printf("[ACPI] Using RSDT at %p", root_table);
        } else {
            console::printf("[FAIL] No ACPI RSDP found in Multiboot2 info!\n");
            return;
        }

        size_t pointer_size = is_xsdt ? 8 : 4;
        size_t entries = (root_table->length - sizeof(SDTHeader)) / pointer_size;
        uint8_t *ptr_array = (uint8_t *)root_table + sizeof(SDTHeader);

        console::printf(". Found %d tables:\n", (int)entries);

        for (size_t i = 0; i < entries; i++) {
            uint64_t phys_addr = is_xsdt ? ((uint64_t *)ptr_array)[i] : ((uint32_t *)ptr_array)[i];
            SDTHeader *header = (SDTHeader *)p2v(phys_addr);

            char sig[5] = {0};
            memcpy(sig, header->signature, 4);

            console::printf("  [%d] Signature: %s, Address: %p, Length: %d\n", (int)i, sig, header,
                            header->length);
        }
    }

    SDTHeader *find_table(const char *signature) {
        if (!root_table) return nullptr;

        size_t pointer_size = is_xsdt ? 8 : 4;
        size_t entries = (root_table->length - sizeof(SDTHeader)) / pointer_size;
        uint8_t *ptr_array = (uint8_t *)root_table + sizeof(SDTHeader);

        for (size_t i = 0; i < entries; i++) {
            uint64_t phys_addr = 0;

            if (is_xsdt) {
                phys_addr = ((uint64_t *)ptr_array)[i];
            } else {
                phys_addr = ((uint32_t *)ptr_array)[i];
            }

            SDTHeader *header = (SDTHeader *)p2v(phys_addr);
            if (header->length < sizeof(MADTEntryHeader)) break;

            if (memcmp(header->signature, signature, 4) == 0) return header;
        }

        return nullptr;
    }

}  // namespace acpi