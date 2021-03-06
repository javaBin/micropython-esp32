/*
 * This file is part of the MicroPython project, http://micropython.org/
 *
 * Development of the code in this file was sponsored by Microbric Pty Ltd
 *
 * The MIT License (MIT)
 *
 * Copyright (c) 2016 Damien P. George
 * Copyright (c) 2017 Anne Jan Brouwer
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_task.h"
#include "soc/cpu.h"
#include "rom/rtc.h"

#include "sha2017_ota.h"
#include "esprtcmem.h"

#include "py/stackctrl.h"
#include "py/nlr.h"
#include "py/compile.h"
#include "py/runtime.h"
#include "py/repl.h"
#include "py/gc.h"
#include "py/mphal.h"
#include "lib/mp-readline/readline.h"
#include "lib/utils/pyexec.h"
#include "uart.h"
#include "modmachine.h"
#include "mpthreadport.h"
#include "bpp_init.h"
#include "badge_pins.h"
#include "badge_base.h"
#include "badge_first_run.h"
#include <badge_input.h>
#include <badge_button.h>
#include <badge.h>

// MicroPython runs as a task under FreeRTOS
#define MP_TASK_PRIORITY        (ESP_TASK_PRIO_MIN + 1)
#define MP_TASK_STACK_SIZE      ( 8 * 1024)
#define MP_TASK_STACK_LEN       (MP_TASK_STACK_SIZE / sizeof(StackType_t))
#define MP_TASK_HEAP_SIZE       (88 * 1024)

//define BUTTON_SAFE_MODE ((1 << BADGE_BUTTON_A) || (1 << BADGE_BUTTON_B))
#define BUTTON_SAFE_MODE ((1 << BADGE_BUTTON_START))

STATIC StaticTask_t mp_task_tcb;
STATIC StackType_t mp_task_stack[MP_TASK_STACK_LEN] __attribute__((aligned (8)));
STATIC uint8_t mp_task_heap[MP_TASK_HEAP_SIZE];

extern uint32_t reset_cause;
extern bool in_safe_mode;

static const char *import_blacklist[] = {
	"/lib/json",
	"/lib/os",
	"/lib/socket",
	"/lib/struct",
	"/lib/time",
};

mp_import_stat_t
mp_import_stat(const char *path) {
	if (in_safe_mode) {
		// be more strict in which modules we would like to load
		if (strncmp(path, "/lib/", 5) != 0
				&& strncmp(path, "/bpp/lib/", 9) != 0
				&& strncmp(path, "/sdcard/lib/", 12) != 0) {
			return MP_IMPORT_STAT_NO_EXIST;
		}

		/* check blacklist */
		int i;
		for (i=0; i<sizeof(import_blacklist)/sizeof(const char *); i++) {
			if (strcmp(path, import_blacklist[i]) == 0) {
				return MP_IMPORT_STAT_NO_EXIST;
			}
		}

		const char *x = index(&path[5], '/');
		if (x == NULL) {
			// only allow directories
			mp_import_stat_t res = mp_vfs_import_stat(path);
			if (res != MP_IMPORT_STAT_DIR) {
				return MP_IMPORT_STAT_NO_EXIST;
			}
			return res;
		}
	}

    return mp_vfs_import_stat(path);
}

void mp_task(void *pvParameter) {
    volatile uint32_t sp = (uint32_t)get_sp();
    #if MICROPY_PY_THREAD
    mp_thread_init(&mp_task_stack[0], MP_TASK_STACK_LEN);
    #endif
    uart_init();
    machine_init();

soft_reset:
    // initialise the stack pointer for the main thread
    mp_stack_set_top((void *)sp);
    mp_stack_set_limit(MP_TASK_STACK_SIZE - 1024);
    gc_init(mp_task_heap, mp_task_heap + sizeof(mp_task_heap));
    mp_init();
    mp_obj_list_init(mp_sys_path, 0);
    // library-path '' is needed for the internal modules.
    mp_obj_list_append(mp_sys_path, MP_OBJ_NEW_QSTR(MP_QSTR_));
    mp_obj_list_append(mp_sys_path, MP_OBJ_NEW_QSTR(MP_QSTR__slash_lib));
    mp_obj_list_append(mp_sys_path, MP_OBJ_NEW_QSTR(MP_QSTR__slash_bpp_slash_lib));
    mp_obj_list_append(mp_sys_path, MP_OBJ_NEW_QSTR(MP_QSTR__slash_sdcard_slash_lib));
    mp_obj_list_init(mp_sys_argv, 0);
    readline_init0();

    // initialise peripherals
    machine_pins_init();

    // run boot-up scripts
    pyexec_frozen_module("_boot.py");
    if (pyexec_mode_kind != PYEXEC_MODE_RAW_REPL) {
        pyexec_frozen_module("boot.py");
    }
    // if (pyexec_mode_kind == PYEXEC_MODE_FRIENDLY_REPL) {
    //     pyexec_file("main.py");
    // }

    for (;;) {
        if (pyexec_mode_kind == PYEXEC_MODE_RAW_REPL) {
            if (pyexec_raw_repl() != 0) {
                break;
            }
        } else {
            if (pyexec_friendly_repl() != 0) {
                break;
            }
        }
    }

    #if MICROPY_PY_THREAD
    mp_thread_deinit();
    #endif

    mp_hal_stdout_tx_str("SHA2017Badge: soft reboot\r\n");

    // deinitialise peripherals
    machine_pins_deinit();

    mp_deinit();
    fflush(stdout);
    reset_cause = MACHINE_SOFT_RESET;
    goto soft_reset;
}

void do_bpp_bgnd() {
    // Kick off bpp
    bpp_init();

    printf("Bpp inited.\n");

    // immediately abort and reboot when touchpad detects something
    while (badge_input_get_event(1000) == 0) { }

    printf("Touch detected. Exiting bpp, rebooting.\n");
    esp_restart();
}

void app_main(void) {
	badge_check_first_run();
	badge_base_init();

	uint8_t magic = esp_rtcmem_read(0);
	uint8_t inv_magic = esp_rtcmem_read(1);

	if (magic == (uint8_t)~inv_magic) {
		printf("Magic checked out!\n");
		switch (magic) {
			case 1:
				printf("Starting OTA\n");
				sha2017_ota_update();
				break;

#ifdef CONFIG_SHA_BPP_ENABLE
			case 2:
				badge_init();
				if (badge_input_button_state == 0) {
					printf("Starting bpp.\n");
					do_bpp_bgnd();
				} else {
					printf("Touch wake after bpp.\n");
					xTaskCreateStaticPinnedToCore(mp_task, "mp_task", MP_TASK_STACK_LEN, NULL, MP_TASK_PRIORITY,
							&mp_task_stack[0], &mp_task_tcb, 0);
				}
				break;
#endif

			case 3:
				badge_first_run();
		}

	} else {
		uint32_t reset_cause = rtc_get_reset_reason(0);
		if (reset_cause != DEEPSLEEP_RESET) {
			badge_init();
			if ((badge_input_button_state & BUTTON_SAFE_MODE) == BUTTON_SAFE_MODE) {
				in_safe_mode = true;
			}
		}
		xTaskCreateStaticPinnedToCore(mp_task, "mp_task", MP_TASK_STACK_LEN, NULL, MP_TASK_PRIORITY,
				&mp_task_stack[0], &mp_task_tcb, 0);
	}
}

void nlr_jump_fail(void *val) {
    printf("NLR jump failed, val=%p\n", val);
    esp_restart();
}

// modussl_mbedtls uses this function but it's not enabled in ESP IDF
void mbedtls_debug_set_threshold(int threshold) {
    (void)threshold;
}
