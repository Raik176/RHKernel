#include "smp/smp.h"

#include "acpi.h"
#include "console.h"
#include "gdt.h"
#include "idt.h"
#include "memory/heap.h"
#include "memory/vmm.h"
#include "smp/apic.h"
#include "string.h"
#include "util.h"

extern "C" {
extern uint8_t trampoline_start[];
extern uint8_t trampoline_end[];
extern uint8_t ap_data_start[];

extern uint8_t higher_stack_top[];  // BSP stack

extern void enable_cpu_features();
}

namespace smp {
    static uint64_t core_count = 0;
    static constexpr uintptr_t TRAM_PHYS = 0x8000;
    static cpu_local** cpu_table = nullptr;

    uint64_t get_core_count() { return core_count; }

    cpu_local* get_cpu_by_index(uint64_t index) {
        if (index >= core_count || cpu_table == nullptr) { return nullptr; }
        return cpu_table[index];
    }

    void enable_optional_cpu_features() {
        uint32_t eax, ebx, ecx, edx;

        eax = 7;
        ecx = 0;
        asm volatile("cpuid" : "=b"(ebx), "=a"(eax), "=c"(ecx), "=d"(edx) : "a"(eax), "c"(ecx));

        cpu_features feat = get_cpu()->cpu_features;

        feat.smep = (ebx & (1 << 7)) != 0;
        feat.smap = (ebx & (1 << 20)) != 0;

        uint64_t cr4;
        asm volatile("mov %%cr4, %0" : "=r"(cr4));
        if (feat.smep) { cr4 |= (1ULL << 20); }
        if (feat.smap) { cr4 |= (1ULL << 21); }
        asm volatile("mov %0, %%cr4" ::"r"(cr4));

        get_cpu()->cpu_features = feat;
    }

    void setup_cpu_local(trampoline_data* data) {
        cpu_local* local = (cpu_local*)heap::kmalloc(sizeof(cpu_local));
        memset(local, 0, sizeof(cpu_local));

        local->self = local;
        local->cpu_index = data->cpu_index;
        local->lapic_id = apic::get_id();
        local->ticks = 0;
        local->kernel_stack = (void*)data->stack_top;

        apic::wrmsr(0xC0000101, (uintptr_t)local); // GS_BASE
        apic::wrmsr(0xC0000102, (uintptr_t)local); // KERNEL_GS_BASE

        cpu_table[local->cpu_index] = local;
    }

    extern "C" void ap_kernel_entry() {
        __asm__ volatile("cli");
        enable_cpu_features();

        idt::init_ap();

        auto* data = (trampoline_data*)(TRAM_PHYS +
                                        ((uintptr_t)ap_data_start - (uintptr_t)trampoline_start));
        setup_cpu_local(data);
        data->status = 1;

        enable_optional_cpu_features();

        gdt::init_core();

        apic::init_ap();
        scheduler::init_core();
        __asm__ volatile("sti");

        for (;;) { asm volatile("pause"); }
    }

    void init_bsp() {
        cpu_local* local = (cpu_local*)heap::kmalloc(sizeof(cpu_local));
        memset(local, 0, sizeof(cpu_local));

        local->self = local;
        local->cpu_index = 0;  // BSP is always 0
        local->lapic_id = apic::get_id();
        local->ticks = 0;

        local->kernel_stack = (void*)higher_stack_top;

        asm volatile("mov %0, %%gs" : : "r"(0));
        apic::wrmsr(0xC0000101, (uintptr_t)local); // GS_BASE
        apic::wrmsr(0xC0000102, (uintptr_t)local); // KERNEL_GS_BASE

        gdt::init_core();
        enable_optional_cpu_features();
    }

    void boot_core(uint8_t lapic_id, uintptr_t trampoline_phys, trampoline_data* data_ptr,
                   uint64_t cpu_index) {
        static constexpr size_t STACK_SIZE = 1024 * 8;
        void* stack_base = heap::kmalloc(STACK_SIZE);
        uintptr_t stack_top = ((uintptr_t)stack_base + STACK_SIZE) & ~0xF;

        data_ptr->cr3 = vmm::get_kernel_pagemap();
        data_ptr->stack_top = stack_top;
        data_ptr->cpu_index = cpu_index;
        data_ptr->entry_point = (uintptr_t)ap_kernel_entry;
        data_ptr->status = 0;

        // INIT
        apic::write_reg(apic::Register::ICRHI, (uint32_t)lapic_id << 24);
        apic::write_reg(apic::Register::ICRLO, 0x00000500);
        busy_sleep(10);  // 10ms

        // SIPI (Targeting the 0x8000 vector)
        uint8_t vector = (uint8_t)(trampoline_phys >> 12);
        for (int i = 0; i < 2; i++) {
            apic::write_reg(apic::Register::ICRHI, (uint32_t)lapic_id << 24);
            apic::write_reg(apic::Register::ICRLO, 0x00000600 | vector);
            busy_sleep(1);
        }
    }

    void init_aps() {
        core_count = 1;  // include BSP

        size_t tram_size = (uintptr_t)trampoline_end - (uintptr_t)trampoline_start;
        memcpy((void*)TRAM_PHYS, (void*)trampoline_start, tram_size);

        auto* data = (trampoline_data*)(TRAM_PHYS +
                                        ((uintptr_t)ap_data_start - (uintptr_t)trampoline_start));
        auto* madt = (acpi::MADT*)acpi::find_table("APIC");
        if (!madt) {
            console::printf("[SMP] MADT not found. Single core mode.\n");
            return;
        }

        uint8_t bsp_id = apic::get_id();

        uintptr_t start = (uintptr_t)madt + sizeof(acpi::MADT);
        uintptr_t current = start;
        uintptr_t end = (uintptr_t)madt + madt->header.length;

        while (current < end) {
            auto* header = (acpi::MADTEntryHeader*)current;

            if (header->type == 0) {
                auto* lapic = (acpi::MADTEntryLAPIC*)current;
                bool ready = (lapic->flags & 1) || (lapic->flags & 2);

                if (ready && lapic->lapic_id != bsp_id) core_count++;
            }

            current += header->length;
        }

        cpu_table = (cpu_local**)heap::kmalloc(sizeof(cpu_local*) * core_count);
        cpu_table[0] = get_cpu();

        current = start;
        core_count = 1;
        while (current < end) {
            auto* header = (acpi::MADTEntryHeader*)current;

            if (header->type == 0) {  // Type 0 = Processor Local APIC
                auto* lapic = (acpi::MADTEntryLAPIC*)current;
                // Flags bit 0 = Enabled, bit 1 = Online Capable
                bool ready = (lapic->flags & 1) || (lapic->flags & 2);

                if (ready && lapic->lapic_id != bsp_id) {
                    console::printf("[SMP] Booting AP (LAPIC %d)... ", lapic->lapic_id);
                    boot_core(lapic->lapic_id, TRAM_PHYS, data, core_count++);

                    uint32_t wait_count = 0;
                    while (data->status == 0 && wait_count < 10) {
                        busy_sleep(5);
                        wait_count++;
                    }

                    if (data->status == 1) {
                        console::printf("Success.\n");
                    } else {
                        console::printf("Failure.\n");
                    }
                }
            }

            current += header->length;
        }
    }
}  // namespace smp