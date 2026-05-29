#include "smp/smp.h"

#include "acpi.h"
#include "console.h"
#include "gdt.h"
#include "idt.h"
#include "memory/heap.h"
#include "memory/pmm.h"
#include "memory/vmm.h"
#include "smp/apic.h"
#include "string.h"
#include "util.h"

extern "C" {
extern uint8_t trampoline_start[];
extern uint8_t trampoline_end[];
extern uint8_t ap_data_start[];
extern uint8_t ap_start_32[];
extern uint8_t ap_start_64[];
extern uint8_t gdt32[];
extern uint8_t gdt64[];
extern uint32_t ap_jump32_offset;
extern uint32_t ap_gdt32_base;
extern uint32_t ap_jump64_offset;
extern uint64_t ap_gdt64_base;

extern uint8_t higher_stack_top[];  // BSP stack

extern void enable_cpu_features();
}

namespace smp {
    static uint64_t core_count = 0;
    static cpu_local **cpu_table = nullptr;

    static constexpr uint32_t IPI_DELIVERY_INIT = 5u << 8;
    static constexpr uint32_t IPI_DELIVERY_SIPI = 6u << 8;
    static constexpr uint32_t IPI_LEVEL_ASSERT = 1u << 14;
    static constexpr uint32_t IPI_TRIGGER_LEVEL = 1u << 15;
    static constexpr uint32_t PIT_TICKS_PER_MS = 1193;
    static constexpr uint32_t TRAMPOLINE_FLAG_LA57 = 1u << 0;

    static void startup_delay_ms(uint32_t ms) {
        for (uint32_t n = 0; n < ms; ++n) {
            outb(0x43, 0x30);
            outb(0x40, (uint8_t)(PIT_TICKS_PER_MS & 0xFF));
            outb(0x40, (uint8_t)(PIT_TICKS_PER_MS >> 8));

            uint16_t prev = 0xFFFF;
            for (uint32_t guard = 0; guard < 1000000; ++guard) {
                outb(0x43, 0x00);
                uint8_t low = inb(0x40);
                uint8_t high = inb(0x40);
                uint16_t cur = ((uint16_t)high << 8) | low;
                if (cur == 0 || cur > prev) break;
                prev = cur;
                asm volatile("pause");
            }
        }
    }

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

    static void xsetbv(uint32_t index, uint64_t value) {
        uint32_t eax = (uint32_t)value;
        uint32_t edx = (uint32_t)(value >> 32);
        asm volatile("xsetbv" : : "c"(index), "a"(eax), "d"(edx) : "memory");
    }

    void enable_optional_cpu_features() {
        uint32_t eax = 0, ebx = 0, ecx = 0, edx = 0;
        cpu_features feat = get_cpu()->cpu_features;
        bool cpu_xsave = false;
        bool cpu_avx = false;
        bool cpu_fma = false;
        bool cpu_f16c = false;
        bool cpu_avx2 = false;
        bool cpu_avx512 = false;
        bool cpu_pat = false;
        bool cpu_fsgsbase = false;
        bool cpu_pku = false;
        bool cpu_mce = false;
        bool cpu_mca = false;

        cpuid(0, 0, eax, ebx, ecx, edx);
        uint32_t max_basic_leaf = eax;

        if (max_basic_leaf >= 1) {
            cpuid(1, 0, eax, ebx, ecx, edx);
            cpu_mce = (edx & (1u << 7)) != 0;
            feat.pge = (edx & (1u << 13)) != 0;
            cpu_mca = (edx & (1u << 14)) != 0;
            cpu_pat = (edx & (1u << 16)) != 0;
            cpu_fma = (ecx & (1u << 12)) != 0;
            cpu_xsave = (ecx & (1u << 26)) != 0;
            cpu_avx = (ecx & (1u << 28)) != 0;
            cpu_f16c = (ecx & (1u << 29)) != 0;
        }

        if (max_basic_leaf >= 7) {
            cpuid(7, 0, eax, ebx, ecx, edx);
            cpu_fsgsbase = (ebx & (1u << 0)) != 0;
            cpu_avx2 = (ebx & (1u << 5)) != 0;
            feat.smep = (ebx & (1u << 7)) != 0;
            feat.smap = (ebx & (1u << 20)) != 0;
            cpu_avx512 = (ebx & ((1u << 16) | (1u << 17) | (1u << 28) | (1u << 30) | (1u << 31))) != 0;
            feat.umip = (ecx & (1u << 2)) != 0;
            cpu_pku = (ecx & (1u << 3)) != 0;
        }

        uint64_t cr4;
        asm volatile("mov %%cr4, %0" : "=r"(cr4));
        if (cpu_mce) { cr4 |= (1ULL << 6); }
        if (feat.pge) { cr4 |= (1ULL << 7); }
        if (feat.umip) { cr4 |= (1ULL << 11); }
        if (cpu_fsgsbase) { cr4 |= (1ULL << 16); }
        if (cpu_xsave) { cr4 |= (1ULL << 18); }
        if (feat.smep) { cr4 |= (1ULL << 20); }
        if (feat.smap) { cr4 |= (1ULL << 21); }
        if (cpu_pku) { cr4 |= (1ULL << 22); }
        asm volatile("mov %0, %%cr4" ::"r"(cr4));
        asm volatile("mov %%cr4, %0" : "=r"(cr4));

        uint64_t xcr0 = 0;
        if (cpu_xsave && (cr4 & (1ULL << 18))) {
            xcr0 = 0x3;
            if (cpu_avx) xcr0 |= 0x4;
            if (cpu_avx && cpu_avx512) xcr0 |= 0xE0;
            xsetbv(0, xcr0);
            xcr0 = xgetbv(0);
        }

        feat.mce = cpu_mce && ((cr4 & (1ULL << 6)) != 0);
        feat.pge = (cr4 & (1ULL << 7)) != 0;
        feat.umip = (cr4 & (1ULL << 11)) != 0;
        feat.smep = (cr4 & (1ULL << 20)) != 0;
        feat.smap = (cr4 & (1ULL << 21)) != 0;
        feat.pat = cpu_pat;
        feat.wc = cpu_pat;
        feat.xsave = cpu_xsave && ((cr4 & (1ULL << 18)) != 0) && ((xcr0 & 0x3) == 0x3);
        feat.avx = feat.xsave && cpu_avx && ((xcr0 & 0x7) == 0x7);
        feat.avx2 = feat.avx && cpu_avx2;
        feat.fma = feat.avx && cpu_fma;
        feat.f16c = feat.avx && cpu_f16c;
        feat.avx512 = feat.avx && cpu_avx512 && ((xcr0 & 0xE0) == 0xE0);
        feat.fsgsbase = cpu_fsgsbase && ((cr4 & (1ULL << 16)) != 0);
        feat.pku = cpu_pku && ((cr4 & (1ULL << 22)) != 0);
        if (feat.pku) {
            uint32_t zero = 0;
            asm volatile("wrpkru" : : "a"(zero), "c"(zero), "d"(zero) : "memory");
        }
        feat.mca = feat.mce && cpu_mca;

        if (feat.mca) {
            constexpr uint32_t IA32_MCG_CAP = 0x179;
            constexpr uint32_t IA32_MCG_STATUS = 0x17A;
            constexpr uint32_t IA32_MCG_CTL = 0x17B;
            constexpr uint32_t IA32_MC0_CTL = 0x400;
            constexpr uint32_t IA32_MC0_STATUS = 0x401;

            uint64_t cap = apic::rdmsr(IA32_MCG_CAP);
            uint32_t banks = cap & 0xffu;
            if (banks > 32) banks = 32;
            if (cap & (1ULL << 8)) { apic::wrmsr(IA32_MCG_CTL, UINT64_MAX); }
            apic::wrmsr(IA32_MCG_STATUS, 0);
            for (uint32_t i = 0; i < banks; i++) {
                apic::wrmsr(IA32_MC0_STATUS + i * 4, 0);
                apic::wrmsr(IA32_MC0_CTL + i * 4, UINT64_MAX);
            }
        }

        get_cpu()->cpu_features = feat;
    }

    static uintptr_t trampoline_offset(const void *symbol) {
        return (uintptr_t)symbol - (uintptr_t)trampoline_start;
    }

    static void patch_ap_trampoline(uintptr_t trampoline_phys) {
        size_t tram_size = (uintptr_t)trampoline_end - (uintptr_t)trampoline_start;
        size_t reserved_size = pmm::get_ap_trampoline_size();
        if ((trampoline_phys & 0xFFF) != 0) kpanic("SMP: AP trampoline is unaligned");
        if (trampoline_phys >= 0x100000) kpanic("SMP: AP trampoline is above SIPI range");
        if (reserved_size == 0 || tram_size > reserved_size) {
            kpanic("SMP: AP trampoline exceeds reserved span");
        }
        if (trampoline_phys + reserved_size > 0x100000) {
            kpanic("SMP: AP trampoline crosses 1 MiB");
        }

        uint8_t *dst = (uint8_t *)p2v(trampoline_phys);
        memset(dst, 0, reserved_size);
        memcpy(dst, (void *)trampoline_start, tram_size);

        *(uint32_t *)(dst + trampoline_offset(&ap_jump32_offset)) =
            (uint32_t)(trampoline_phys + trampoline_offset(ap_start_32));
        *(uint32_t *)(dst + trampoline_offset(&ap_gdt32_base)) =
            (uint32_t)(trampoline_phys + trampoline_offset(gdt32));
        *(uint32_t *)(dst + trampoline_offset(&ap_jump64_offset)) =
            (uint32_t)(trampoline_phys + trampoline_offset(ap_start_64));
        *(uint64_t *)(dst + trampoline_offset(&ap_gdt64_base)) =
            trampoline_phys + trampoline_offset(gdt64);
    }

    static void setup_ap_cpu_local(trampoline_data *data) {
        cpu_local *local = (cpu_local *)data->cpu_local_ptr;
        if (!local || local->self != local || local->cpu_index != data->cpu_index) {
            for (;;) { asm volatile("cli; hlt"); }
        }

        apic::wrmsr(0xC0000101, (uintptr_t)local);  // GS_BASE
        apic::wrmsr(0xC0000102, (uintptr_t)local);  // KERNEL_GS_BASE
    }

    extern "C" __attribute__((no_stack_protector)) void ap_kernel_entry() {
        __asm__ volatile("cli");

        auto *data = (trampoline_data *)p2v(
            pmm::get_ap_trampoline_page() +
            ((uintptr_t)ap_data_start - (uintptr_t)trampoline_start));
        setup_ap_cpu_local(data);

        gdt::init_core();
        idt::init_ap();
        enable_cpu_features();
        enable_optional_cpu_features();

        apic::init_ap();
        scheduler::init_core();

        data->status = 1;

        __asm__ volatile("sti");

        for (;;) { asm volatile("hlt"); }
    }

    void init_bsp() {
        cpu_local *local = (cpu_local *)heap::kzalloc(sizeof(cpu_local));
        if (!local) kpanic("SMP: BSP CPU-local allocation failed");

        local->self = local;
        local->cpu_index = 0;  // BSP is always 0
        local->lapic_id = apic::get_id();
        local->ticks = 0;

        local->kernel_stack = (void *)higher_stack_top;
        local->panic_stack = vmm::alloc_kernel_stack(PANIC_STACK_SIZE);
        if (!local->panic_stack) kpanic("SMP: BSP panic stack allocation failed");
        local->panic_stack = (void *)((uintptr_t)local->panic_stack + PANIC_STACK_SIZE);

        local->tlb_shootdown_mail.type = mail_type::TLB_SHOOTDOWN;
        local->tlb_shootdown_mail.handled = true;
        local->reschedule_mail.type = mail_type::RESCHEDULE;
        local->reschedule_mail.handled = true;
        local->halt_mail.type = mail_type::HALT;
        local->halt_mail.handled = true;

        asm volatile("mov %0, %%gs" : : "r"(0));
        apic::wrmsr(0xC0000101, (uintptr_t)local);  // GS_BASE
        apic::wrmsr(0xC0000102, (uintptr_t)local);  // KERNEL_GS_BASE

        core_count = 1;
        cpu_table = (cpu_local **)heap::kmalloc_array(1, sizeof(cpu_local *));
        if (!cpu_table) kpanic("SMP: failed to allocate BSP CPU table");
        cpu_table[0] = local;

        gdt::init_core();
        idt::enable_panic_ist();
        enable_optional_cpu_features();

    }


    void boot_core(uint32_t lapic_id, uintptr_t trampoline_phys, trampoline_data *data_ptr,
                   uint64_t cpu_index) {
        static constexpr size_t STACK_SIZE = scheduler::KERNEL_STACK_SIZE;
        void *stack_base = vmm::alloc_kernel_stack(STACK_SIZE);
        if (!stack_base) kpanic("SMP: AP stack allocation failed");
        uintptr_t stack_top = ((uintptr_t)stack_base + STACK_SIZE) & ~0xF;

        cpu_local *local = (cpu_local *)heap::kzalloc(sizeof(cpu_local));
        if (!local) kpanic("SMP: AP CPU-local allocation failed");

        local->self = local;
        local->cpu_index = cpu_index;
        local->lapic_id = lapic_id;
        local->kernel_stack = (void *)stack_top;
        local->panic_stack = vmm::alloc_kernel_stack(PANIC_STACK_SIZE);
        if (!local->panic_stack) kpanic("SMP: AP panic stack allocation failed");
        local->panic_stack = (void *)((uintptr_t)local->panic_stack + PANIC_STACK_SIZE);

        local->tlb_shootdown_mail.type = mail_type::TLB_SHOOTDOWN;
        local->tlb_shootdown_mail.handled = true;
        local->reschedule_mail.type = mail_type::RESCHEDULE;
        local->reschedule_mail.handled = true;
        local->halt_mail.type = mail_type::HALT;
        local->halt_mail.handled = true;

        uint64_t kernel_pagemap = vmm::get_kernel_pagemap();
        if (kernel_pagemap > UINT32_MAX) kpanic("SMP: AP CR3 is above 4 GiB");

        data_ptr->cr3 = (uint32_t)kernel_pagemap;
        data_ptr->flags = vmm::five_level_paging_enabled() ? TRAMPOLINE_FLAG_LA57 : 0;
        data_ptr->stack_top = stack_top;
        data_ptr->cpu_index = cpu_index;
        data_ptr->entry_point = (uintptr_t)ap_kernel_entry;
        data_ptr->status = 0;
        data_ptr->cpu_local_ptr = (uintptr_t)local;
        data_ptr->lapic_id = lapic_id;
        cpu_table[cpu_index] = local;

        apic::send_ipi(lapic_id, IPI_DELIVERY_INIT | IPI_LEVEL_ASSERT | IPI_TRIGGER_LEVEL);
        startup_delay_ms(10);

        uint8_t vector = (uint8_t)(trampoline_phys >> 12);
        for (int i = 0; i < 2; i++) {
            apic::send_ipi(lapic_id, IPI_DELIVERY_SIPI | vector);
            startup_delay_ms(1);
        }
    }

    void init_aps() {
        uintptr_t trampoline_phys = pmm::get_ap_trampoline_page();
        if (!trampoline_phys) kpanic("SMP: AP trampoline span was not reserved");
        patch_ap_trampoline(trampoline_phys);

        auto *data = (trampoline_data *)p2v(
            trampoline_phys + ((uintptr_t)ap_data_start - (uintptr_t)trampoline_start));
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
        cpu_local **new_table = (cpu_local **)heap::kcalloc(detected_cores, sizeof(cpu_local *));
        if (!new_table) kpanic("SMP: failed to allocate CPU table");
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
                    boot_core(lapic->lapic_id, trampoline_phys, data, cpu_index);

                    uint32_t wait_count = 0;
                    while (data->status == 0 && wait_count < 100) {
                        startup_delay_ms(10);
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

    static bool valid_mail_type(mail_type type) {
        switch (type) {
            case mail_type::HALT:
            case mail_type::TLB_SHOOTDOWN:
            case mail_type::RESCHEDULE:
                return true;
        }
        return false;
    }

    static void stamp_mail(mail *message) {
        cpu_local *current_cpu = get_cpu();
        message->sender_core = current_cpu && current_cpu->self == current_cpu ? current_cpu->cpu_index : 0;
    }

    static mail_status enqueue_mail_locked(cpu_local *target, mail *message) {
        if (!target || target->self != target || !message) return mail_status::INVALID_MESSAGE;
        if (!valid_mail_type(message->type)) return mail_status::INVALID_MESSAGE;

        if (message->queued || !message->handled) {
            target->mail_busy++;
            return message->type == mail_type::RESCHEDULE ? mail_status::ALREADY_PENDING
                                                          : mail_status::BUSY;
        }

        if (message->type == mail_type::RESCHEDULE) {
            target->reschedule_requests++;
            if (target->reschedule_pending) {
                target->mail_coalesced++;
                return mail_status::ALREADY_PENDING;
            }
            target->reschedule_pending = true;
        }

        message->next = nullptr;
        message->queued = true;
        message->handled = false;

        if (target->mail_tail) {
            target->mail_tail->next = message;
        } else {
            target->mail_head = message;
        }
        target->mail_tail = message;
        target->mail_depth++;
        target->mail_enqueued++;
        return mail_status::QUEUED;
    }

    static mail_status enqueue_mail(cpu_local *target, mail *message) {
        if (!target || target->self != target) return mail_status::INVALID_TARGET;
        if (!message || !valid_mail_type(message->type)) {
            target->mail_invalid++;
            return mail_status::INVALID_MESSAGE;
        }

        stamp_mail(message);

        uint64_t flags;
        target->mail_lock.acquire(flags);
        mail_status status = enqueue_mail_locked(target, message);
        target->mail_lock.release(flags);
        return status;
    }

    static void send_one_or_panic(cpu_local *target, mail *message, bool flush) {
        mail_status status = enqueue_mail(target, message);
        if (status == mail_status::INVALID_TARGET) kpanic("SMP: invalid mail target");
        if (status == mail_status::INVALID_MESSAGE) kpanic("SMP: invalid mail message");
        if (status == mail_status::BUSY) kpanic("SMP: mail object busy");
        if (flush && status == mail_status::QUEUED) {
            target->reschedule_ipis += message->type == mail_type::RESCHEDULE ? 1 : 0;
            apic::send_ipi(target->lapic_id, idt::MAILBOX_VECTOR);
        }
    }

    bool send_tlb_shootdown_mail(int64_t target_cpu, mail *message, uint64_t cr3, uint64_t addr,
                                  uint32_t pages) {
        if (target_cpu < 0 || !message) return false;
        cpu_local *target = get_cpu_by_index((uint64_t)target_cpu);
        if (!target || target->self != target) return false;

        uint64_t flags;
        target->mail_lock.acquire(flags);
        if (message->queued || !message->handled) {
            target->mail_busy++;
            target->mail_lock.release(flags);
            return false;
        }

        message->type = mail_type::TLB_SHOOTDOWN;
        message->next = nullptr;
        message->tlb.cr3 = cr3;
        message->tlb.addr = addr;
        message->tlb.pages = pages;
        stamp_mail(message);
        mail_status status = enqueue_mail_locked(target, message);
        target->mail_lock.release(flags);
        return status == mail_status::QUEUED;
    }

    bool send_reschedule_mail(uint64_t target_cpu) {
        cpu_local *target = get_cpu_by_index(target_cpu);
        if (!target || target->self != target) return false;

        target->reschedule_mail.type = mail_type::RESCHEDULE;

        mail_status status = enqueue_mail(target, &target->reschedule_mail);
        if (status == mail_status::QUEUED) {
            target->reschedule_ipis++;
            apic::send_ipi(target->lapic_id, idt::MAILBOX_VECTOR);
            return true;
        }
        return status == mail_status::ALREADY_PENDING;
    }

    void send_halt_mail(int64_t target_cpu) {
        cpu_local *current_cpu = get_cpu();

        auto send_halt_to = [&](cpu_local *target) {
            if (!target || target->self != target) return;
            target->halt_mail.type = mail_type::HALT;
            send_one_or_panic(target, &target->halt_mail, false);
        };

        if (target_cpu < 0) {
            for (uint64_t i = 0; i < get_core_count(); i++) {
                cpu_local *target = get_cpu_by_index(i);
                if (target_cpu == MAIL_RECEIVER_OTHERS && current_cpu && target == current_cpu) continue;
                send_halt_to(target);
            }
            flush_mail(target_cpu);
            return;
        }

        send_halt_to(get_cpu_by_index((uint64_t)target_cpu));
        flush_mail(target_cpu);
    }

    void flush_mail(int64_t target_cpu) {
        if (target_cpu == MAIL_RECEIVER_ALL) {
            apic::send_ipi(0, 0x00080000 | idt::MAILBOX_VECTOR);
        } else if (target_cpu == MAIL_RECEIVER_OTHERS) {
            apic::send_ipi(0, 0x000C0000 | idt::MAILBOX_VECTOR);
        } else if (target_cpu >= 0) {
            cpu_local *target = get_cpu_by_index((uint64_t)target_cpu);
            if (target == nullptr || target->self != target) return;

            apic::send_ipi(target->lapic_id, idt::MAILBOX_VECTOR);
        }
    }

    void panic_stop_others() {
        cpu_local *current = get_cpu();
        if (!current || current->self != current || core_count <= 1) return;

        for (uint64_t i = 0; i < core_count; i++) {
            cpu_local *target = get_cpu_by_index(i);
            if (!target || target == current || target->self != target) continue;

            target->halt_mail.type = mail_type::HALT;
            target->halt_mail.sender_core = current->cpu_index;

            uint64_t flags;
            if (!target->mail_lock.try_acquire(flags)) continue;
            if (!target->halt_mail.queued && target->halt_mail.handled) {
                target->halt_mail.next = nullptr;
                target->halt_mail.queued = true;
                target->halt_mail.handled = false;
                if (target->mail_tail) {
                    target->mail_tail->next = &target->halt_mail;
                } else {
                    target->mail_head = &target->halt_mail;
                }
                target->mail_tail = &target->halt_mail;
                target->mail_depth++;
                target->mail_enqueued++;
            } else {
                target->mail_busy++;
            }
            target->mail_lock.release(flags);
        }

        apic::send_ipi(0, 0x000C0000 | idt::MAILBOX_VECTOR);
    }
}  // namespace smp