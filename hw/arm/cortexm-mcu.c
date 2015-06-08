/*
 * Cortex-M System emulation.
 *
 * Copyright (c) 2014 Liviu Ionescu
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, see <http://www.gnu.org/licenses/>.
 */

#include "config.h"
#include "sysemu/sysemu.h"
#include "hw/arm/cortexm.h"
#include "qemu/option.h"
#include "qemu/config-file.h"
#include "hw/arm/arm.h"
#include "exec/address-spaces.h"
#include "hw/sysbus.h"
#include "qemu/error-report.h"
#include "sysemu/qtest.h"
#include "hw/loader.h"
#include "elf.h"
#include "cpu.h"

#if defined(CONFIG_VERBOSE)
#include "verbosity.h"
#endif

/* Redefined from armv7m.c */
#define TYPE_BITBAND "ARM,bitband-memory"

#define DEFAULT_NUM_IRQ		256

static void cortexm_reset(void *opaque);

static void cortexm_mcu_image_load(DeviceState *dev);

/*
 * There are two kind of definitions in this file, cortexm_core_* for
 * ARM Cortex-M core, and cortexm_mcu_*, as common code for vendor
 * MCU implementations.
 */

#define BITBAND_OFFSET (0x02000000)

static void cortexm_bitband_init(uint32_t address)
{
    DeviceState *dev;

    /* Make address a multiple of 32MB */
    address &= ~(BITBAND_OFFSET - 1);
    dev = qdev_create(NULL, TYPE_BITBAND);
    qdev_prop_set_uint32(dev, "base", address);
    qdev_init_nofail(dev);
    sysbus_mmio_map(SYS_BUS_DEVICE(dev), 0, address + BITBAND_OFFSET);
}

/**
 * Properties for the 'cortexm_mcu' object, used as parent for
 * all vendor MCUs.
 */
static Property cortexm_mcu_properties[] = {
        DEFINE_PROP_UINT32("sram-sizeK", CortexMState, sram_size_kb, 0),
        DEFINE_PROP_UINT32("flash-sizeK", CortexMState, flash_size_kb, 0),
    DEFINE_PROP_END_OF_LIST() };

/**
 * Used during qdev_create() as parent before the call
 * to device_mcu_instance_init().
 *
 * Called in vendor_mcu_create(), which calls cortexm_mcu_create().
 *
 * It is a different step than *_realize().
 */
static void cortexm_mcu_instance_init(Object *obj)
{
    qemu_log_function_name();

    CortexMState *cm_state = CORTEXM_MCU_STATE(obj);

    /* Construct the ITM object. */
    object_initialize(&cm_state->itm, sizeof(cm_state->itm), TYPE_ARMV7M_ITM);

    DeviceState *itmdev;
    itmdev = DEVICE(&cm_state->itm);
    qdev_set_parent_bus(itmdev, sysbus_get_default());
}

/**
 * Cortex-M core initialisation routine.
 * The capabilities were already copied into the state object by the
 * *_instance_init() functions.
 *
 * Some MCU properties can be overwritten by command line options
 * (core type, flash/ram sizes).
 */
static void cortexm_mcu_realize(DeviceState *dev, Error **errp)
{
    qemu_log_function_name();

    CortexMState *cm_state = CORTEXM_MCU_STATE(dev);
    CortexMClass *cm_class = CORTEXM_MCU_GET_CLASS(cm_state);

    /*
     * Capabilities were set late, in an *_intance_init(), and were not
     * available during our instance_init(), so the whole story happens
     * in the realize() callback.
     */
    CortexMCapabilities *cm_capabilities = cm_state->capabilities;
    assert(cm_capabilities != NULL);

    const char *kernel_filename = NULL;
    const char *cpu_model_arg = NULL;
    int ram_size_arg_kb = 0;
    int flash_size_arg_kb = 0;
    if (cm_state) {
        kernel_filename = cm_state->kernel_filename;
        cpu_model_arg = cm_state->cpu_model;
        ram_size_arg_kb = cm_state->sram_size_kb;
        flash_size_arg_kb = cm_state->flash_size_kb;
    }

    if (cpu_model_arg) {
        /* If explicitly given via the --cpu command line option,
         * overwrite the board MCU definition. */
        if (strcmp(cpu_model_arg, "cortex-m0") == 0) {
            cm_capabilities->cortexm_model = CORTEX_M0;
        } else if (strcmp(cpu_model_arg, "cortex-m0p") == 0) {
            cm_capabilities->cortexm_model = CORTEX_M0PLUS;
        } else if (strcmp(cpu_model_arg, "cortex-m1") == 0) {
            cm_capabilities->cortexm_model = CORTEX_M1;
        } else if (strcmp(cpu_model_arg, "cortex-m3") == 0) {
            cm_capabilities->cortexm_model = CORTEX_M3;
        } else if (strcmp(cpu_model_arg, "cortex-m4") == 0) {
            cm_capabilities->cortexm_model = CORTEX_M4;
            cm_capabilities->has_mpu = false;
        } else if (strcmp(cpu_model_arg, "cortex-m4f") == 0) {
            cm_capabilities->cortexm_model = CORTEX_M4F;
            cm_capabilities->has_mpu = true;
        } else if (strcmp(cpu_model_arg, "cortex-m7") == 0) {
            cm_capabilities->cortexm_model = CORTEX_M7;
            cm_capabilities->has_mpu = false;
        } else if (strcmp(cpu_model_arg, "cortex-m7f") == 0) {
            cm_capabilities->cortexm_model = CORTEX_M7F;
            cm_capabilities->has_mpu = true;
        } else {
            error_report("Illegal --cpu %s, only cortex-m* supported.",
                    cpu_model_arg);
            exit(1);
        }
    }

    const char *cpu_model = "?";
    const char *display_model = "?";

    int max_num_irq = 496;

    /* Some capabilities are hard-wired. */
    switch (cm_capabilities->cortexm_model) {
    case CORTEX_M0:
        display_model = "Cortex-M0";
        cpu_model = "cortex-m0";
        cm_capabilities->has_mpu = false;
        cm_capabilities->has_fpu = false;
        cm_capabilities->fpu_type = CORTEX_M_FPU_TYPE_NONE;
        break;

    case CORTEX_M0PLUS:
        display_model = "Cortex-M0+";
        cpu_model = "cortex-m0p";
        cm_capabilities->has_mpu = false;
        cm_capabilities->has_fpu = false;
        cm_capabilities->fpu_type = CORTEX_M_FPU_TYPE_NONE;
        break;

    case CORTEX_M1:
        display_model = "Cortex-M1";
        cpu_model = "cortex-m1";
        /* TODO: Check if it has no MPU/FPU. */
        cm_capabilities->has_mpu = false;
        cm_capabilities->has_fpu = false;
        cm_capabilities->fpu_type = CORTEX_M_FPU_TYPE_NONE;
        break;

    case CORTEX_M3:
        display_model = "Cortex-M3";
        cpu_model = "cortex-m3";
        max_num_irq = 240;
        cm_capabilities->has_fpu = false;
        cm_capabilities->fpu_type = CORTEX_M_FPU_TYPE_NONE;
        break;

    case CORTEX_M4:
        display_model = "Cortex-M4";
        cpu_model = "cortex-m4";
        cm_capabilities->has_fpu = false;
        cm_capabilities->fpu_type = CORTEX_M_FPU_TYPE_NONE;
        break;

    case CORTEX_M4F:
        display_model = "Cortex-M4F";
        cpu_model = "cortex-mf4";
        cm_capabilities->has_fpu = true;
        cm_capabilities->fpu_type = CORTEX_M_FPU_TYPE_FPV4_SP_D16;
        break;

    case CORTEX_M7:
        display_model = "Cortex-M7";
        cpu_model = "cortex-m7";
        cm_capabilities->has_fpu = false;
        cm_capabilities->fpu_type = CORTEX_M_FPU_TYPE_NONE;
        break;

    case CORTEX_M7F:
        display_model = "Cortex-M7F";
        cpu_model = "cortex-m7f";
        cm_capabilities->has_fpu = true;
        cm_capabilities->fpu_type = CORTEX_M_FPU_TYPE_FPV5_SP_D16;
        break;

    default:
        error_report("Illegal cortexm_model %d.",
                cm_capabilities->cortexm_model);
        exit(1);
    }

    cm_state->cpu_model = cpu_model;
    cm_state->display_model = display_model;

    int sram_size_kb;
    if (ram_size_arg_kb != 0) {
        /* If explicitly given via the -m command line option,
         * or by --global, overwrite the board MCU definition. */
        sram_size_kb = ram_size_arg_kb;
    } else {
        sram_size_kb = cm_capabilities->sram_size_kb;
    }

    /* Max 32 MB ram, to avoid overlapping with the bit-banding area */
    if (sram_size_kb > 32 * 1024) {
        sram_size_kb = 32 * 1024;
    }
    cm_state->sram_size_kb = sram_size_kb;

    int flash_size_kb;
    if (flash_size_arg_kb) {
        /* If explicitly given via the  --global command line option,
         * overwrite the board MCU definition. */
        flash_size_kb = flash_size_arg_kb;
    } else {
        flash_size_kb = cm_capabilities->flash_size_kb;
    }
    cm_state->flash_size_kb = flash_size_kb;

#if defined(CONFIG_VERBOSE)
    if (verbosity_level >= VERBOSITY_COMMON) {
        const char *cmdline;

        printf("Device: '%s' (%s", cm_capabilities->device_name, display_model);
        if (cm_capabilities->has_mpu) {
            printf(", MPU");
        }
        if (cm_capabilities->has_fpu) {
            printf(", FPU");
        }
        printf("), Flash: %d KB, RAM: %d KB.\n", flash_size_kb, sram_size_kb);
        if (kernel_filename) {
            printf("Image: '%s'.\n", kernel_filename);
        }

        cmdline = semihosting.cmdline;
        if (cmdline != NULL) {
            printf("Command line: '%s' (%d bytes).\n", cmdline,
                    (int) strlen(cmdline));
        }
    }
#endif

    /* ----- Create CPU based on model. ----- */
    ARMCPU *cpu;
    cpu = cpu_arm_init(cm_state->cpu_model);
    if (cpu == NULL) {
        error_report("Unable to find CPU definition %s", cm_state->cpu_model);
        exit(1);
    }
    cm_state->cpu = cpu;

    CPUARMState *env = &cpu->env;

    /* ----- Create memory regions. ----- */
    (*cm_class->memory_regions_create)(dev);

    /* ----- Create the NVIC device. ----- */

    DeviceState *nvic;
    nvic = qdev_create(NULL, "armv7m_nvic");
    env->nvic = nvic;

    int num_irq;
    if (cm_capabilities->num_irq) {
        num_irq = cm_capabilities->num_irq;
    } else {
        num_irq = DEFAULT_NUM_IRQ;
    }

    if (num_irq > max_num_irq) {
        num_irq = max_num_irq;
    }
    /* Must be a multiple of 32 */
    num_irq = (num_irq + 31) & (~31);
    cm_state->num_irq = num_irq;

    qdev_prop_set_uint32(nvic, "num-irq", num_irq);

    qdev_init_nofail(nvic);
    sysbus_connect_irq(SYS_BUS_DEVICE(nvic), 0,
            qdev_get_gpio_in(DEVICE(cpu), ARM_CPU_IRQ));

    qemu_irq *pic = g_new(qemu_irq, num_irq);
    for (int i = 0; i < num_irq; i++) {
        pic[i] = qdev_get_gpio_in(nvic, i);
    }
    cm_state->pic = pic;

    /* ----- Create the ITM device. ----- */
    Error *err = NULL;
    if (cm_capabilities->has_itm) {
        object_property_set_bool(OBJECT(&cm_state->itm), true, "realized",
                &err);
        if (err != NULL) {
            error_propagate(errp, err);
            return;
        }
    }

    /* ----- Load image. ----- */
    if (!kernel_filename && !qtest_enabled() && !with_gdb) {
        error_report("Guest image must be specified (using -kernel)");
        exit(1);
    }

    /* The image must be loaded later, after all memory regions are mapped */
    cm_class->image_load(dev);

    /* Assume 8000000 Hz */
    /* TODO: compute according to board clock & pll settings */
    system_clock_scale = 80;

#if defined(CONFIG_VERBOSE)
    if (verbosity_level >= VERBOSITY_COMMON) {
        printf("%s core initialised.\n", cm_state->display_model);
    }
#endif

    if (kernel_filename) {
        /* Schedule a CPU core reset. */
        qemu_register_reset(cortexm_reset, cm_state);
    }
}

static void cortexm_mcu_memory_regions_create(DeviceState *dev)
{
    qemu_log_function_name();

    CortexMState *cm_state = CORTEXM_MCU_STATE(dev);

    /* Get the system memory region, it must start at 0. */
    MemoryRegion *system_memory = get_system_memory();

    int flash_size = cm_state->flash_size_kb * 1024;
    int sram_size = cm_state->sram_size_kb * 1024;

    MemoryRegion *flash_mem = &cm_state->flash_mem;
    /* Flash programming is done via the SCU, so pretend it is ROM.  */
    memory_region_init_ram(flash_mem, NULL, "cortexm-mem-flash", flash_size,
            &error_abort);
    vmstate_register_ram_global(flash_mem);
    memory_region_set_readonly(flash_mem, true);
    memory_region_add_subregion(system_memory, 0x00000000, flash_mem);

    MemoryRegion *sram_mem = &cm_state->sram_mem;
    memory_region_init_ram(sram_mem, NULL, "cortexm-mem-sram", sram_size,
            &error_abort);
    vmstate_register_ram_global(sram_mem);
    memory_region_add_subregion(system_memory, 0x20000000, sram_mem);
    cortexm_bitband_init(0x20000000);

    MemoryRegion *hack_mem = &cm_state->hack_mem;
    /* Hack to map an additional page of ram at the top of the address
     * space.  This stops qemu complaining about executing code outside RAM
     * when returning from an exception.  */
    memory_region_init_ram(hack_mem, NULL, "cortexm-mem-hack", 0x1000,
            &error_abort);
    vmstate_register_ram_global(hack_mem);
    memory_region_add_subregion(system_memory, 0xFFFFF000, hack_mem);
}

static void cortexm_mcu_image_load(DeviceState *dev)
{
    qemu_log_function_name();

    CortexMState *cm_state = CORTEXM_MCU_STATE(dev);
    ARMCPU *cpu = cm_state->cpu;
    CPUARMState *env = &cpu->env;

    const char *kernel_filename = cm_state->kernel_filename;

    /* Fill-in a minimal boot info, required for semihosting.  */
    static struct arm_boot_info cortexm_binfo;
    cortexm_binfo.kernel_cmdline = "";
    cortexm_binfo.kernel_filename = kernel_filename;

    env->boot_info = &cortexm_binfo;

    if (kernel_filename) {
        int big_endian;
#ifdef TARGET_WORDS_BIGENDIAN
        big_endian = 1;
#else
        big_endian = 0;
#endif
        int image_size;
        uint64_t entry;
        uint64_t lowaddr;
        image_size = load_elf(kernel_filename, NULL, NULL, &entry, &lowaddr,
        NULL, big_endian, ELF_MACHINE, 1);
        if (image_size < 0) {
            image_size = load_image_targphys(kernel_filename, 0,
                    cm_state->flash_size_kb * 1024);
            lowaddr = 0;
        }
        if (image_size < 0) {
            error_report("Could not load image '%s'", kernel_filename);
            exit(1);
        }
    }
}

/**
 * Initialise the "cortexm-mcu" object. Currently there is no input data.
 * Called during module_call_init() in main().
 */
static void cortexm_mcu_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->props = cortexm_mcu_properties;
    dc->realize = cortexm_mcu_realize;

    CortexMClass *cm_class = CORTEXM_MCU_CLASS(klass);
    cm_class->memory_regions_create = cortexm_mcu_memory_regions_create;
    cm_class->image_load = cortexm_mcu_image_load;
}

static const TypeInfo cortexm_mcu_type_init = {
    .name = TYPE_CORTEXM_MCU,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(CortexMState),
    .instance_init = cortexm_mcu_instance_init,
    .class_init = cortexm_mcu_class_init,
    .class_size = sizeof(CortexMClass) };

/* ----- Type inits. ----- */

static void cortexm_types_init()
{
    type_register_static(&cortexm_mcu_type_init);
}

#if defined(CONFIG_GNU_ARM_ECLIPSE)
type_init(cortexm_types_init);
#endif

/* ----- */

/**
 * When verbose, display a line to identify the board (name, description).
 *
 * Does not really depend on Cortex-M, but I could not find a better place.
 */
void cortexm_board_greeting(MachineState *machine)
{
#if defined(CONFIG_VERBOSE)
    if (verbosity_level >= VERBOSITY_COMMON) {
        MachineClass *mc = MACHINE_GET_CLASS(machine);
        printf("Board: '%s' (%s).\n", mc->name, mc->desc);
    }
#endif
}

/**
 * Create the device, initialise members and complete initialisations.
 */
DeviceState *cortexm_mcu_init(MachineState *machine, const char *mcu_type)
{
    DeviceState *dev;
    dev = qdev_create(NULL, mcu_type);
    CortexMState *cm_state = CORTEXM_MCU_STATE(dev);

    if (machine->kernel_filename) {
        cm_state->kernel_filename = machine->kernel_filename;
    }

    if (machine->cpu_model) {
        cm_state->cpu_model = machine->cpu_model;
    }

    Error *err = NULL;
    object_property_set_bool(OBJECT(dev), true, "realized", &err);
    if (err != NULL) {
        error_report("%s", error_get_pretty(err));
        exit(1);
    }

    return dev;
}

/* ----- */

/**
 * Used solely by cortexm_mcu_realize() above.
 */
static void cortexm_reset(void *opaque)
{
    qemu_log_function_name();

    CortexMState *cm_state = opaque;
    ARMCPU *cpu = cm_state->cpu;

#if defined(CONFIG_VERBOSE)
    if (verbosity_level >= VERBOSITY_COMMON) {
        printf("Cortex-M core reset.\n");
    }
#endif

    /* Ensure the image is copied into memory before reset fetches msp & pc */
    rom_reset(NULL);

    cpu_reset(CPU(cpu));
}

/* TODO: remove all following functions */

/* Cortex-M0 initialisation routine.  */
qemu_irq *
cortex_m0_core_init(CortexMCapabilities *cm_info, MachineState *machine)
{
    return NULL;
}

/* Cortex-M0+ initialisation routine.  */
qemu_irq *
cortex_m0p_core_init(CortexMCapabilities *cm_info, MachineState *machine)
{
    return NULL;
}

/* Cortex-M3 initialisation routine.  */
qemu_irq *
cortex_m3_core_init(CortexMCapabilities *cm_info, MachineState *machine)
{
    return NULL;
}

/* Cortex-M4 initialisation routine.  */
qemu_irq *
cortex_m4_core_init(CortexMCapabilities *cm_info, MachineState *machine)
{
    return NULL;
}

/* Cortex-M7 initialisation routine.  */
qemu_irq *
cortex_m7_core_init(CortexMCapabilities *cm_info, MachineState *machine)
{
    return NULL;
}

/* -------------------------------------------------------------------------- */
