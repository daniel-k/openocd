/***************************************************************************
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program; if not, write to the                         *
 *   Free Software Foundation, Inc.,                                       *
 *   51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.           *
 ***************************************************************************/

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <helper/time_support.h>
#include <jtag/jtag.h>
#include "target/target.h"
#include "target/target_type.h"
#include "rtos.h"
#include "helper/log.h"
#include "helper/types.h"
#include "target/armv7m.h"
#include "rtos_riot_stackings.h"

static int riot_detect_rtos(struct target *target);
static int riot_create(struct target *target);
static int riot_update_threads(struct rtos *rtos);
static int riot_get_thread_reg_list(struct rtos *rtos, int64_t thread_id, char **hex_reg_list);
static int riot_get_symbol_list_to_lookup(symbol_table_elem_t *symbol_list[]);


struct riot_thread_state {
	int value;
	const char *desc;
};

/* refer tcb.h */
static const struct riot_thread_state riot_thread_states[] = {
	{ 0, "Stopped" },
	{ 1, "Sleeping" },
	{ 2, "Mutex blocked" },
	{ 3, "Receive blocked" },
	{ 4, "Send blocked" },
	{ 5, "Replay blocked" },
	{ 6, "Running" },
	{ 7, "Pending" },
};

#define RIOT_NUM_STATES (sizeof(riot_thread_states)/sizeof(struct riot_thread_state))

struct riot_params {
	const char *target_name;
	unsigned char thread_sp_offset;
	unsigned char thread_status_offset;
};

/* Initialize in riot_create() depending on architecture */
static const struct rtos_register_stacking *stacking_info;

static const struct riot_params riot_params_list[] = {
	{
	"cortex_m",             /* target_name */
	0x00,					/* thread_sp_offset; */
	0x04,					/* thread_status_offset; */
	},
    { /* STLink */
    "hla_target",           /* target_name */
    0x00,                   /* thread_sp_offset; */
    0x04,                   /* thread_status_offset; */
    }
};

#define RIOT_NUM_PARAMS ((int)(sizeof(riot_params_list)/sizeof(struct riot_params)))


enum riot_symbol_values {
	RIOT_THREADS_BASE = 0,
	RIOT_NUM_THREADS,
	RIOT_ACTIVE_PID,
	RIOT_MAX_THREADS,
	RIOT_NAME_OFFSET,
};

/* refer sched.c */
static const char * const riot_symbol_list[] = {
	"sched_threads",
	"sched_num_threads",
	"sched_active_pid",
	"max_threads",
    "_tcb_name_offset",
	NULL
};

const struct rtos_type riot_rtos = {
	.name = "riot",

	.detect_rtos = riot_detect_rtos,
	.create = riot_create,
	.update_threads = riot_update_threads,
	.get_thread_reg_list = riot_get_thread_reg_list,
	.get_symbol_list_to_lookup = riot_get_symbol_list_to_lookup,

};



static int riot_update_threads(struct rtos *rtos)
{
	int retval;
	int tasks_found = 0;
	const struct riot_params *param;

	if (rtos == NULL)
		return -1;

	if (rtos->rtos_specific_params == NULL)
		return -3;

	param = (const struct riot_params *) rtos->rtos_specific_params;

	if (rtos->symbols == NULL) {
		LOG_ERROR("No symbols for riot");
		return -4;
	}

	if (rtos->symbols[RIOT_THREADS_BASE].address == 0) {
		LOG_ERROR("Don't have the thread base");
		return -2;
	}

	/* wipe out previous thread details if any */
	rtos_free_threadlist(rtos);

	/* Reset values */
	rtos->current_thread = 0;   /* undefined PID in RIOT is 0 */
	rtos->thread_count = 0;

	/* read the current thread id */
	int16_t active_pid = 0;
	retval = target_read_buffer(rtos->target,
	                            rtos->symbols[RIOT_ACTIVE_PID].address,
                                sizeof(active_pid),
                                (uint8_t *)&active_pid);
	if (retval != ERROR_OK) {
	    LOG_ERROR("Couldn't read `sched_active_pid`");
	    return retval;
	}
	rtos->current_thread = active_pid;

    /* read the current thread count */
    int32_t thread_count = 0; /* `int` in RIOT, but this is Cortex M* only anyway */
    retval = target_read_buffer(rtos->target,
                                rtos->symbols[RIOT_NUM_THREADS].address,
                                sizeof(thread_count),
                                (uint8_t *)&thread_count);
    if (retval != ERROR_OK) {
        LOG_ERROR("Couldn't read `sched_num_threads`");
        return retval;
    }
    rtos->thread_count = thread_count;

    /* read the maximum number of threads */
    uint8_t max_threads = 0;
    retval = target_read_buffer(rtos->target,
                                rtos->symbols[RIOT_MAX_THREADS].address,
                                sizeof(max_threads),
                                (uint8_t *)&max_threads);
    if (retval != ERROR_OK) {
        LOG_ERROR("Couldn't read `_max_threads`");
        return retval;
    }

    /* Base address of thread array */
    uint32_t threads_base = rtos->symbols[RIOT_THREADS_BASE].address;
    printf("threads_base = 0x%08x\n", threads_base);

    uint8_t name_offset = 0;
    if(rtos->symbols[RIOT_NAME_OFFSET].address != 0) {
        retval = target_read_buffer(rtos->target,
                                    rtos->symbols[RIOT_NAME_OFFSET].address,
                                    sizeof(name_offset),
                                    (uint8_t *)&name_offset);
        if (retval != ERROR_OK) {
            LOG_ERROR("Couldn't read `_tcb_name_offset`");
            return retval;
        }
    }

    printf("Name offset: %d\n", name_offset);

    /* Allocate memory for thread description */
    rtos->thread_details = malloc(sizeof(struct thread_detail) * thread_count);

    /* Buffer for thread names */
    char buffer[32];

    uint32_t tcb_pointer = 0;

    for(int i = 0; i < max_threads; i++) {

        /* get pointer to tcb_t */
        retval = target_read_buffer(rtos->target,
                                    threads_base + (i * 4),
                                    sizeof(tcb_pointer),
                                    (uint8_t *)&tcb_pointer);
        if (retval != ERROR_OK) {
            LOG_ERROR("Couldn't read `sched_threads[i]`");
            return retval;
        }

        if(tcb_pointer == 0) {
            /* PID unused */
            continue;
        }

        /* Index is PID */
        rtos->thread_details[tasks_found].threadid = i;

        /* read thread state */
        uint16_t status = 0;
        retval = target_read_buffer(rtos->target,
                                    tcb_pointer + param->thread_status_offset,
                                    sizeof(status),
                                    (uint8_t *)&status);
        if (retval != ERROR_OK) {
            LOG_ERROR("Couldn't read `sched_threads[i].status`");
            return retval;
        }

        /* Search for state */
        unsigned int k;
        for(k = 0; k < RIOT_NUM_STATES; k++) {
            if(riot_thread_states[k].value == status) {
                break;
            }
        }
        const char* state_str = (k == RIOT_NUM_STATES) ?
                                    "unknown state" :
                                    riot_thread_states[k].desc;
        /* Copy state string */
        rtos->thread_details[tasks_found].extra_info_str = malloc(
                                                        strlen(state_str) + 1);
        strcpy(rtos->thread_details[tasks_found].extra_info_str, state_str);

        /* Thread names are only available if */
        if(name_offset != 0) {

            uint32_t name_pointer = 0;
            retval = target_read_buffer(rtos->target,
                                        tcb_pointer + name_offset,
                                        sizeof(name_pointer),
                                        (uint8_t *)&name_pointer);
            if (retval != ERROR_OK) {
                LOG_ERROR("Couldn't read name pointer");
                return retval;
            }

            /* read thread name */
            retval = target_read_buffer(rtos->target,
                                        name_pointer,
                                        sizeof(buffer),
                                        (uint8_t *)&buffer);
            if (retval != ERROR_OK) {
                LOG_ERROR("Couldn't read name");
                return retval;
            }

            if(buffer[sizeof(buffer)-1] != 0) {
                buffer[sizeof(buffer)-1] = 0;
            }

            rtos->thread_details[tasks_found].thread_name_str = malloc(
                                                            strlen(buffer) + 1);
            strcpy(rtos->thread_details[tasks_found].thread_name_str, buffer);

        } else {
            const char no_name[] = "Need DEVELHELP";
            rtos->thread_details[tasks_found].thread_name_str = malloc(
                                                         strlen(no_name) + 1);
            strcpy(rtos->thread_details[tasks_found].thread_name_str, no_name);
        }




        rtos->thread_details[tasks_found].exists = true;
        rtos->thread_details[tasks_found].display_str = NULL;

        printf("thread id:      %d\n", i);
        printf("thread name  at %p\n", rtos->thread_details[tasks_found].thread_name_str);
        printf("thread state at %p\n", rtos->thread_details[tasks_found].extra_info_str);

        printf("thread name  = '%s'\n", rtos->thread_details[tasks_found].thread_name_str);
        printf("thread state = '%s'\n", rtos->thread_details[tasks_found].extra_info_str);
        tasks_found++;
    }

	return 0;
}

static int riot_get_thread_reg_list(struct rtos *rtos, int64_t thread_id, char **hex_reg_list)
{
//	int retval;
	const struct riot_params *param;

	*hex_reg_list = NULL;

	if (rtos == NULL)
		return -1;

	if (thread_id == 0)
		return -2;

	if (rtos->rtos_specific_params == NULL)
		return -3;

	param = (const struct riot_params *) rtos->rtos_specific_params;

	/* Find the thread with that thread id */
	uint32_t threads_base = rtos->symbols[RIOT_THREADS_BASE].address;
	uint32_t tcb_pointer = 0;
	target_read_buffer(rtos->target,
	                   threads_base + (thread_id * 4),
	                   sizeof(tcb_pointer),
	                   (uint8_t *)&tcb_pointer);

	uint32_t stackptr = 0;
    target_read_buffer(rtos->target,
                       tcb_pointer + param->thread_sp_offset,
                       sizeof(stackptr),
                       (uint8_t *)&stackptr);

    printf("stack pointer for thread %li at 0x%04x\n", thread_id, stackptr);

    return rtos_generic_stack_read(rtos->target,
        stacking_info,
        stackptr,
        hex_reg_list);
}

static int riot_get_symbol_list_to_lookup(symbol_table_elem_t *symbol_list[])
{
	unsigned int i;
	*symbol_list = calloc(
			ARRAY_SIZE(riot_symbol_list), sizeof(symbol_table_elem_t));

	for (i = 0; i < ARRAY_SIZE(riot_symbol_list); i++) {
		(*symbol_list)[i].symbol_name = riot_symbol_list[i];
		if(i == RIOT_NAME_OFFSET) {
		    (*symbol_list)[i].optional = true;
		}
	}

	return 0;
}

static int riot_detect_rtos(struct target *target)
{
	if ((target->rtos->symbols != NULL) &&
			(target->rtos->symbols[RIOT_THREADS_BASE].address != 0)) {
		/* looks like RIOT */
		return 1;
	}
	return 0;
}

static int riot_create(struct target *target)
{
	int i = 0;
	printf("Target type name is: '%s'\n", target->type->name);
	/* lookup if target is supported by RIOT */
	while ((i < RIOT_NUM_PARAMS) &&
		(0 != strcmp(riot_params_list[i].target_name, target->type->name))) {
		i++;
	}
	if (i >= RIOT_NUM_PARAMS) {
		LOG_ERROR("Could not find target in riot compatibility list");
		return -1;
	}

	target->rtos->rtos_specific_params = (void *) &riot_params_list[i];
	target->rtos->current_thread = 0;
	target->rtos->thread_details = NULL;

	/* Stacking is different depending on architecture */
    struct armv7m_common *armv7m_target = target_to_armv7m(target);

    if(armv7m_target->arm.is_armv6m) {
        stacking_info = &rtos_riot_Cortex_M0_stacking;
    } else if(is_armv7m(armv7m_target)) {
        stacking_info = &rtos_riot_Cortex_M34_stacking;
    } else {
        LOG_ERROR("No stacking info for architecture");
        return -1;
    }
	return 0;
}
