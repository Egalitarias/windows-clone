#ifndef KERNEL_CONTEXT_SWITCH_H
#define KERNEL_CONTEXT_SWITCH_H

#include "tcb.h"

void context_switch(TCB *from, TCB *to);

#endif
