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
    static cpu_local **cpu_table = nullptr;

    static_assert((TRAM_PHYS & 0xF) == 0, "TRAM_PHYS must be 16-byte aligned");

    uint64_t get_core_count() { return core_count; }

    cpu_local *get_cpu_by_index(uint64_t index) {
        if (index >= core_count || cpu_table == nullptr) { return nullptr; }
        return cpu_table[index];
    }

    static void cpuid(uint32_t leaf, uint32_t subleaf, uint32_t &eax, uint32_t &ebx,
                      uint32_t &ecx, uint32_t &edx) {
        asm volatile("cpuid"
                     : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                     : "a"(leaf), "c"(subleaf));
    }

    static uint64_t xgetbv(uint32_t index) {
        uint32_t eax = 0, edx = 0;
        asm volatile("xgetbv" : "=a"(eax), "=d"(edx) : "c"(index));
        return ((uint64_t)edx << 32) | eax;
    }

    void enable_optional_cpu_features() {
        uint32_t eax = 0, ebx = 0, ecx = 0, edx = 0;
        cpu_features feat = get_cpu()->cpu_features;
        bool cpu_xsave = false;
        bool cpu_avx = false;
        bool cpu_pat = false;
        bool cpu_fsgsbase = false;

        cpuid(0, 0, eax, ebx, ecx, edx);
        uint32_t max_basic_leaf = eax;

        if (max_basic_leaf >= 1) {
            cpuid(1, 0, eax, ebx, ecx, edx);
            feat.pge = (edx & (1u << 13)) != 0;
            cpu_pat = (edx & (1u << 16)) != 0;
            cpu_xsave = (ecx & (1u << 26)) != 0;
            cpu_avx = (ecx & (1u << 28)) != 0;
        }

        if (max_basic_leaf >= 7) {
            cpuid(7, 0, eax, ebx, ecx, edx);
            cpu_fsgsbase = (ebx & (1u << 0)) != 0;
            feat.smep = (ebx & (1u << 7)) != 0;
            feat.smap = (ebx & (1u << 20)) != 0;
            feat.umip = (ecx & (1u << 2)) != 0;
        }

        uint64_t cr4;
        asm volatile("mov %%cr4, %0" : "=r"(cr4));
        if (feat.pge) { cr4 |= (1ULL << 7); }
        if (feat.umip) { cr4 |= (1ULL << 11); }
        if (feat.smep) { cr4 |= (1ULL << 20); }
        if (feat.smap) { cr4 |= (1ULL << 21); }
        asm volatile("mov %0, %%cr4" ::"r"(cr4));
        asm volatile("mov %%cr4, %0" : "=r"(cr4));

        uint64_t xcr0 = (cr4 & (1ULL << 18)) ? xgetbv(0) : 0;
        feat.pge = (cr4 & (1ULL << 7)) != 0;
        feat.umip = (cr4 & (1ULL << 11)) != 0;
        feat.smep = (cr4 & (1ULL << 20)) != 0;
        feat.smap = (cr4 & (1ULL << 21)) != 0;
        feat.pat = cpu_pat;
        feat.wc = cpu_pat;
        feat.xsave = cpu_xsave && ((xcr0 & 0x3) == 0x3);
        feat.avx = cpu_xsave && cpu_avx && ((xcr0 & 0x7) == 0x7);
        feat.fsgsbase_supported = cpu_fsgsbase;
        feat.fsgsbase = false;
        get_cpu()->cpu_features = feat;
    }

    void setup_cpu_local(trampoline_data *data) {
        cpu_local *local = (cpu_local *)heap::kmalloc(sizeof(cpu_local));
        memset(local, 0, sizeof(cpu_local));

        local->self = local;
        local->cpu_index = data->cpu_index;
        local->lapic_id = apic::get_id();
        local->ticks = 0;
        local->kernel_stack = (void *)data->stack_top;

        apic::wrmsr(0xC0000101, (uintptr_t)local);  // GS_BASE
        apic::wrmsr(0xC0000102, (uintptr_t)local);  // KERNEL_GS_BASE

        cpu_table[local->cpu_index] = local;
    }

    extern "C" void ap_kernel_entry() {
        __asm__ volatile("cli");
        enable_cpu_features();

        idt::init_ap();

        auto *data = (trampoline_data *)p2v(
            TRAM_PHYS + ((uintptr_t)ap_data_start - (uintptr_t)trampoline_start));
        setup_cpu_local(data);

        enable_optional_cpu_features();

        gdt::init_core();

        apic::init_ap();
        scheduler::init_core();

        data->status = 1;

        __asm__ volatile("sti");

        for (;;) { asm volatile("hlt"); }
    }

    void init_bsp() {
        cpu_local *local = (cpu_local *)heap::kmalloc(sizeof(cpu_local));
        memset(local, 0, sizeof(cpu_local));

        local->self = local;
        local->cpu_index = 0;  // BSP is always 0
        local->lapic_id = apic::get_id();
        local->ticks = 0;

        local->kernel_stack = (void *)higher_stack_top;

        asm volatile("mov %0, %%gs" : : "r"(0));
        apic::wrmsr(0xC0000101, (uintptr_t)local);  // GS_BASE
        apic::wrmsr(0xC0000102, (uintptr_t)local);  // KERNEL_GS_BASE

        core_count = 1;
        cpu_table = (cpu_local **)heap::kmalloc(sizeof(cpu_local *));
        if (!cpu_table) kpanic("SMP: failed to allocate BSP CPU table");
        cpu_table[0] = local;

        gdt::init_core();
        enable_optional_cpu_features();

    }


    void boot_core(uint8_t lapic_id, uintptr_t trampoline_phys, trampoline_data *data_ptr,
                   uint64_t cpu_index) {
        static constexpr size_t STACK_SIZE = 1024 * 8;
        void *stack_base = heap::kmalloc(STACK_SIZE);
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
        size_t tram_size = (uintptr_t)trampoline_end - (uintptr_t)trampoline_start;
        memcpy(p2v(TRAM_PHYS), (void *)trampoline_start, tram_size);

        auto *data = (trampoline_data *)p2v(
            TRAM_PHYS + ((uintptr_t)ap_data_start - (uintptr_t)trampoline_start));
        auto *madt = (MADT *)acpi::find_table("APIC");
        if (!madt) {
            console::printf("[SMP] MADT not found. Single core mode.\n");
            return;
        }

        uint8_t bsp_id = apic::get_id();

        uintptr_t start = (uintptr_t)madt + sizeof(MADT);
        uintptr_t current = start;
        uintptr_t end = (uintptr_t)madt + madt->header.length;

        uint64_t detected_cores = 1;
        while (current < end) {
            auto *header = (MADTEntryHeader *)current;

            if (header->type == 0) {
                auto *lapic = (MADTEntryLAPIC *)current;
                bool ready = (lapic->flags & 1) || (lapic->flags & 2);

                if (ready && lapic->lapic_id != bsp_id) detected_cores++;
            }

            current += header->length;
        }

        cpu_local **old_table = cpu_table;
        cpu_local *bsp_local = get_cpu();
        cpu_local **new_table = (cpu_local **)heap::kmalloc(sizeof(cpu_local *) * detected_cores);
        if (!new_table) kpanic("SMP: failed to allocate CPU table");
        memset(new_table, 0, sizeof(cpu_local *) * detected_cores);
        new_table[0] = bsp_local;
        cpu_table = new_table;
        if (old_table) heap::kfree(old_table);

        current = start;
        core_count = 1;
        while (current < end) {
            auto *header = (MADTEntryHeader *)current;

            if (header->type == 0) {  // Type 0 = Processor Local APIC
                auto *lapic = (MADTEntryLAPIC *)current;
                // Flags bit 0 = Enabled, bit 1 = Online Capable
                bool ready = (lapic->flags & 1) || (lapic->flags & 2);

                if (ready && lapic->lapic_id != bsp_id) {
                    uint64_t cpu_index = core_count;

                    console::printf("[SMP] Booting AP (LAPIC %d)... ", lapic->lapic_id);
                    boot_core(lapic->lapic_id, TRAM_PHYS, data, cpu_index);

                    uint32_t wait_count = 0;
                    while (data->status == 0 && wait_count < 100) {
                        busy_sleep(10);
                        wait_count++;
                    }

                    if (data->status == 1 && cpu_table[cpu_index]) {
                        core_count = cpu_index + 1;
                        console::printf("Success.\n");
                    } else {
                        cpu_table[cpu_index] = nullptr;
                        console::printf("Failure.\n");
                    }
                }
            }

            current += header->length;
        }
    }

    static bool enqueue_mail(cpu_local *target, mail *message) {
        if (!target || !message) return false;

        uint64_t flags;
        target->mail_lock.acquire(flags);

        uint32_t next = (target->mail_tail + 1) % MAILBOX_SIZE;
        if (next == target->mail_head) {
            target->mail_lock.release(flags);
            return false;
        }

        target->mailbox[target->mail_tail] = message;
        target->mail_tail = next;
        target->mail_lock.release(flags);
        return true;
    }

    void send_mail(int64_t target_cpu, mail *message) {
        auto *current_cpu = get_cpu();
        message->sender_core = current_cpu ? current_cpu->cpu_index : 0;

        if (target_cpu < 0) {
            for (uint64_t i = 0; i < get_core_count(); i++) {
                if (current_cpu && i == current_cpu->cpu_index &&
                    target_cpu == MAIL_RECEIVER_OTHERS)
                    continue;
                if (!enqueue_mail(get_cpu_by_index(i), message)) { kpanic("SMP: mailbox full"); }
            }
            return;
        }

        if (!enqueue_mail(get_cpu_by_index(target_cpu), message)) { kpanic("SMP: mailbox full"); }
    }

    bool send_tlb_shootdown_mail(int64_t target_cpu, mail *message, uint64_t cr3, uint64_t addr,
                                  uint32_t pages, volatile bool *handled) {
        if (!message) return false;

        memset(message, 0, sizeof(*message));
        message->type = mail_type::TLB_SHOOTDOWN;
        message->handled = handled;
        message->tlb.cr3 = cr3;
        message->tlb.addr = addr;
        message->tlb.pages = pages;

        send_mail(target_cpu, message);
        return true;
    }

    void send_halt_mail(int64_t target_cpu) {
        mail *message = (mail *)heap::kmalloc(sizeof(mail));
        message->type = mail_type::HALT;
        message->handled = nullptr;

        send_mail(target_cpu, message);
    }

    void flush_mail(int64_t target_cpu) {
        if (target_cpu == MAIL_RECEIVER_ALL) {
            apic::write_reg(apic::Register::ICRLO, 0x00080000 | idt::MAILBOX_VECTOR);
        } else if (target_cpu == MAIL_RECEIVER_OTHERS) {
            apic::write_reg(apic::Register::ICRLO, 0x000C0000 | idt::MAILBOX_VECTOR);
        } else {
            cpu_local *target = get_cpu_by_index(target_cpu);
            if (target == nullptr) return;

            apic::write_reg(apic::Register::ICRHI, (uint32_t)target->lapic_id << 24);
            apic::write_reg(apic::Register::ICRLO, 0x00000000 | idt::MAILBOX_VECTOR);
        }
    }
}  // namespace smp