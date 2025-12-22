#include "smp/smp.h"
#include "acpi.h"
#include "smp/apic.h"
#include "util.h"
#include "console.h"
#include "string.h"
#include "heap.h"
#include "vmm.h"
#include "gdt.h"
#include "idt.h"

extern "C" {
    extern uint8_t trampoline_start[];
    extern uint8_t trampoline_end[];
    extern uint8_t ap_data_start[];
    
    extern uint8_t higher_stack_top[]; // BSP stack

    extern void enable_cpu_features();
}

namespace smp {
    static uint64_t core_count = 0;
    static constexpr uintptr_t TRAM_PHYS = 0x8000;


    void boot_core(uint8_t lapic_id, uintptr_t trampoline_phys, trampoline_data* data_ptr, uint64_t cpu_index) {
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
        busy_sleep(10); // 10ms

        // SIPI (Targeting the 0x8000 vector)
        uint8_t vector = (uint8_t)(trampoline_phys >> 12);
        for (int i = 0; i < 2; i++) {
            apic::write_reg(apic::Register::ICRHI, (uint32_t)lapic_id << 24);
            apic::write_reg(apic::Register::ICRLO, 0x00000600 | vector);
            busy_sleep(1); 
        }
    }

    void init_bsp() {
        cpu_local* local = (cpu_local*)heap::kmalloc(sizeof(cpu_local));
        memset(local, 0, sizeof(cpu_local));

        local->self = local;
        local->cpu_index = 0; // BSP is always 0
        local->lapic_id = apic::get_id();
        local->ticks = 0;
        
        local->kernel_stack = (void*)higher_stack_top;

        asm volatile("mov %0, %%gs" : : "r"(0));
        apic::wrmsr(0xC0000101, (uintptr_t)local);
    }

    void init_aps() {
        core_count = 1; // include BSP

        size_t tram_size = (uintptr_t)trampoline_end - (uintptr_t)trampoline_start;
        memcpy((void*)TRAM_PHYS, (void*)trampoline_start, tram_size);

        auto* data = (trampoline_data*)(TRAM_PHYS + ((uintptr_t)ap_data_start - (uintptr_t)trampoline_start));
        auto* madt = (acpi::MADT*)acpi::find_table("APIC");
        if (!madt) {
            console::printf("[SMP] MADT not found. Single core mode.\n");
            return;
        }

        uint8_t bsp_id = apic::get_id();
        
        uintptr_t current = (uintptr_t)madt + sizeof(acpi::MADT);
        uintptr_t end = (uintptr_t)madt + madt->header.length;

        while (current < end) {
            auto* header = (acpi::MADTEntryHeader*)current;

            if (header->type == 0) { // Type 0 = Processor Local APIC
                auto* lapic = (acpi::MADTEntryLAPIC*)current;
                
                // Flags bit 0 = Enabled, bit 1 = Online Capable
                bool ready = (lapic->flags & 1) || (lapic->flags & 2);

                if (ready && lapic->lapic_id != bsp_id) {
                    console::printf("[SMP] Booting AP (LAPIC %d)... ", lapic->lapic_id);
                    boot_core(lapic->lapic_id, TRAM_PHYS, data, core_count);
                    
                    while(data->status == 0) {
                        busy_sleep(5);
                    }
                    
                    console::printf("Success.\n", lapic->lapic_id);
                    core_count++;
                }
            }
            current += header->length;
        }
    }

    uint64_t get_core_count() {
        return core_count;
    }

    void setup_cpu_local(trampoline_data* data) {
        cpu_local* local = (cpu_local*)heap::kmalloc(sizeof(cpu_local));
        memset(local, 0, sizeof(cpu_local));

        local->self = local;
        local->cpu_index = data->cpu_index;
        local->lapic_id = apic::get_id();
        local->ticks = 0;
        local->kernel_stack = (void*)data->stack_top;

        asm volatile("mov %0, %%gs" : : "r"(0));
        apic::wrmsr(0xC0000101, (uintptr_t)local);
    }

    extern "C" void ap_kernel_entry() {
        __asm__ volatile("cli");
        enable_cpu_features();

        auto* data = (trampoline_data*)(TRAM_PHYS + ((uintptr_t)ap_data_start - (uintptr_t)trampoline_start));
        setup_cpu_local(data);
        data->status = 1;

        gdt::init_ap();
        idt::init_ap();

        apic::init_ap(); //TODO: make ticks per cpu
        __asm__ volatile("sti");

        for(;;);
    }
}