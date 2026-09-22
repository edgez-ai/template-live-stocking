#if defined(EDGEZ_MORSE_HEALTH_STACK_WRAPPER)

#include <string.h>

#include <mmosal.h>

#define HEALTH_STACK_MIN_WORDS 768U

struct mmosal_task *__real_mmosal_task_create(mmosal_task_fn_t task_fn,
					       void *argument,
					       enum mmosal_task_priority priority,
					       unsigned stack_size_u32,
					       const char *name);

struct mmosal_task *__wrap_mmosal_task_create(mmosal_task_fn_t task_fn,
					       void *argument,
					       enum mmosal_task_priority priority,
					       unsigned stack_size_u32,
					       const char *name)
{
	if (name != NULL && strcmp(name, "health") == 0 &&
	    stack_size_u32 < HEALTH_STACK_MIN_WORDS) {
		stack_size_u32 = HEALTH_STACK_MIN_WORDS;
	}
	return __real_mmosal_task_create(task_fn, argument, priority,
					 stack_size_u32, name);
}

#endif
