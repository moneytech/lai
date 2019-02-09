
/*
 * Lux ACPI Implementation
 * Copyright (C) 2018-2019 by Omar Muhamed
 */

#include "lai.h"
#include "ns_impl.h"

/* ACPI Control Method Execution */
/* Type1Opcode := DefBreak | DefBreakPoint | DefContinue | DefFatal | DefIfElse |
   DefLoad | DefNoop | DefNotify | DefRelease | DefReset | DefReturn |
   DefSignal | DefSleep | DefStall | DefUnload | DefWhile */

// Prepare the interpreter state for a control method call.
// Param: acpi_state_t *state - will store method name and arguments
// Param: acpi_nsnode_t *method - identifies the control method

void acpi_init_call_state(acpi_state_t *state, acpi_nsnode_t *method) {
    acpi_memset(state, 0, sizeof(acpi_state_t));
    state->handle = method;
    state->stack_ptr = -1;
}

// Finalize the interpreter state. Frees all memory owned by the state.

void acpi_finalize_state(acpi_state_t *state) {
    acpi_free_object(&state->retvalue);
    for(int i = 0; i < 7; i++)
    {
        acpi_free_object(&state->arg[i]);
    }

    for(int i = 0; i < 8; i++)
    {
        acpi_free_object(&state->local[i]);
    }
}

// Pushes a new item to the opstack and returns it.
static acpi_object_t *acpi_exec_push_opstack_or_die(acpi_state_t *state) {
    if(state->opstack_ptr == 16)
        acpi_panic("operand stack overflow\n");
    acpi_object_t *object = &state->opstack[state->opstack_ptr];
    acpi_memset(object, 0, sizeof(acpi_object_t));
    state->opstack_ptr++;
    return object;
}

// Returns the n-th item from the opstack.
static acpi_object_t *acpi_exec_get_opstack(acpi_state_t *state, int n) {
    if(n >= state->opstack_ptr)
        acpi_panic("opstack access out of bounds"); // This is an internal execution error.
    return &state->opstack[n];
}

// Removes n items from the opstack.
static void acpi_exec_pop_opstack(acpi_state_t *state, int n) {
    for(int k = 0; k < n; k++)
        acpi_free_object(&state->opstack[state->opstack_ptr - k - 1]);
    state->opstack_ptr -= n;
}

// Pushes a new item to the execution stack and returns it.
static acpi_stackitem_t *acpi_exec_push_stack_or_die(acpi_state_t *state) {
    state->stack_ptr++;
    if(state->stack_ptr == 16)
        acpi_panic("execution engine stack overflow\n");
    return &state->stack[state->stack_ptr];
}

// Returns the n-th item from the top of the stack.
static acpi_stackitem_t *acpi_exec_peek_stack(acpi_state_t *state, int n) {
    if(state->stack_ptr - n < 0)
        return NULL;
    return &state->stack[state->stack_ptr - n];
}

// Returns the last item of the stack.
static acpi_stackitem_t *acpi_exec_peek_stack_back(acpi_state_t *state) {
    return acpi_exec_peek_stack(state, 0);
}

// Removes n items from the stack.
static void acpi_exec_pop_stack(acpi_state_t *state, int n) {
    state->stack_ptr -= n;
}

// Removes the last item from the stack.
static void acpi_exec_pop_stack_back(acpi_state_t *state) {
    acpi_exec_pop_stack(state, 1);
}

static void acpi_exec_reduce(int opcode, acpi_object_t *operands, acpi_object_t *result) {
    //acpi_debug("acpi_exec_reduce: opcode 0x%02X\n", opcode);
    switch(opcode) {
    case STORE_OP:
        acpi_move_object(result, operands);
        break;
    case NOT_OP:
        result->type = ACPI_INTEGER;
        result->integer = ~operands[0].integer;
        break;
    case ADD_OP:
        result->type = ACPI_INTEGER;
        result->integer = operands[0].integer + operands[1].integer;
        break;
    case SUBTRACT_OP:
        result->type = ACPI_INTEGER;
        result->integer = operands[0].integer - operands[1].integer;
        break;
    case MULTIPLY_OP:
        result->type = ACPI_INTEGER;
        result->integer = operands[0].integer * operands[1].integer;
        break;
    case AND_OP:
        result->type = ACPI_INTEGER;
        result->integer = operands[0].integer & operands[1].integer;
        break;
    case OR_OP:
        result->type = ACPI_INTEGER;
        result->integer = operands[0].integer | operands[1].integer;
        break;
    case XOR_OP:
        result->type = ACPI_INTEGER;
        result->integer = operands[0].integer ^ operands[1].integer;
        break;
    case SHL_OP:
        result->type = ACPI_INTEGER;
        result->integer = operands[0].integer << operands[1].integer;
        break;
    case SHR_OP:
        result->type = ACPI_INTEGER;
        result->integer = operands[0].integer >> operands[1].integer;
        break;
    default:
        acpi_panic("undefined opcode in acpi_exec_reduce: %02X\n", opcode);
    }
}

// acpi_exec_run(): Internal function, executes actual AML opcodes
// Param:  uint8_t *method - pointer to method opcodes
// Param:  acpi_state_t *state - machine state
// Return: int - 0 on success

static int acpi_exec_run(uint8_t *method, acpi_state_t *state)
{
    acpi_stackitem_t *item;
    while((item = acpi_exec_peek_stack_back(state)))
    {
        // Whether we use the result of an expression or not.
        // If yes, it will be pushed onto the opstack after the expression is computed.
        int want_exec_result = 0;

        if(item->kind == LAI_METHOD_CONTEXT_STACKITEM)
        {
			// ACPI does an implicit Return(0) at the end of a control method.
			if(state->pc == state->limit)
			{
				if(state->opstack_ptr) // This is an internal error.
					acpi_panic("opstack is not empty before return\n");
				acpi_object_t *result = acpi_exec_push_opstack_or_die(state);
				result->type = ACPI_INTEGER;
				result->integer = 0;

				acpi_exec_pop_stack_back(state);
				continue;
			}
        }else if(item->kind == LAI_OP_STACKITEM)
        {
            if(state->opstack_ptr == item->op_opstack + item->op_num_operands) {
                acpi_object_t result = {0};
                acpi_object_t *operands = acpi_exec_get_opstack(state, item->op_opstack);
                acpi_exec_reduce(item->op_opcode, operands, &result);
                acpi_exec_pop_opstack(state, item->op_num_operands);

                if(item->op_want_result)
                {
                    acpi_object_t *opstack_res = acpi_exec_push_opstack_or_die(state);
                    acpi_copy_object(opstack_res, &result);
                }
                state->pc += acpi_write_object(method + state->pc, &result, state);

                acpi_exec_pop_stack_back(state);
                continue;
            }

            want_exec_result = 1;
        }else if(item->kind == LAI_LOOP_STACKITEM)
        {
            if(state->pc == item->loop_pred)
            {
                // We are at the beginning of a loop. We check the predicate; if it is false,
                // we jump to the end of the loop and remove the stack item.
                acpi_object_t predicate = {0};
                state->pc += acpi_eval_object(&predicate, state, method + state->pc);
                if(!predicate.integer)
                {
                    state->pc = item->loop_end;
                    acpi_exec_pop_stack_back(state);
                }
                continue;
            }else if(state->pc == item->loop_end)
            {
                // Unconditionally jump to the loop's predicate.
                state->pc = item->loop_pred;
                continue;
            }

            if (state->pc > item->loop_end) // This would be an interpreter bug.
                acpi_panic("execution escaped out of While() body\n");
        }else if(item->kind == LAI_COND_STACKITEM)
        {
            // If the condition wasn't taken, execute the Else() block if it exists
            if(!item->cond_taken)
            {
                if(method[state->pc] == ELSE_OP)
                {
                    size_t else_size;
                    state->pc++;
                    state->pc += acpi_parse_pkgsize(method + state->pc, &else_size);
                }

                acpi_exec_pop_stack_back(state);
                continue;
            }

            // Clean up the execution stack at the end of If().
            if(state->pc == item->cond_end)
            {
                // Consume a follow-up Else() opcode.
                if(state->pc < state->limit && method[state->pc] == ELSE_OP) {
                    size_t else_size;
                    state->pc++;
                    size_t j = state->pc;
                    state->pc += acpi_parse_pkgsize(method + state->pc, &else_size);

                    state->pc = j + else_size;
                }

                acpi_exec_pop_stack_back(state);
                continue;
            }
        }else
            acpi_panic("unexpected acpi_stackitem_t\n");

        if(state->pc > state->limit) // This would be an interpreter bug.
            acpi_panic("execution escaped out of code range\n");

        // Process names.
        if(acpi_is_name(method[state->pc])) {
            char name[ACPI_MAX_NAME];
            size_t name_size = acpins_resolve_path(state->handle, name, method + state->pc);
            acpi_nsnode_t *handle = acpi_exec_resolve(name);
            if(!handle)
                acpi_panic("undefined reference %s\n", name);

            acpi_object_t result = {0};
            if(handle->type == ACPI_NAMESPACE_NAME)
            {
                acpi_copy_object(&result, &handle->object);
                state->pc += name_size;
            }else if(handle->type == ACPI_NAMESPACE_METHOD)
            {
                state->pc += acpi_methodinvoke(method + state->pc, state, &result);
            }else if(handle->type == ACPI_NAMESPACE_FIELD
                    || handle->type == ACPI_NAMESPACE_INDEXFIELD)
            {
                // It's an Operation Region field; perform IO in that region.
                acpi_read_opregion(&result, handle);
                state->pc += name_size;
            }else
                acpi_panic("unexpected type of named object\n");

            if(want_exec_result)
            {
                acpi_object_t *opstack_res = acpi_exec_push_opstack_or_die(state);
                acpi_move_object(opstack_res, &result);
            }
            acpi_free_object(&result);
            continue;
        }

        /* General opcodes */
        int opcode;
        if(method[state->pc] == EXTOP_PREFIX)
        {
            if(state->pc + 1 == state->limit)
                acpi_panic("two-byte opcode on method boundary\n");
            opcode = (EXTOP_PREFIX << 8) | method[state->pc + 1];
        }else
            opcode = method[state->pc];

        // This switch handles the majority of all opcodes.
        switch(opcode)
        {
        case NOP_OP:
            state->pc++;

        case ZERO_OP:
            if(want_exec_result)
            {
                acpi_object_t *result = acpi_exec_push_opstack_or_die(state);
                result->type = ACPI_INTEGER;
                result->integer = 0;
            }
            state->pc++;
            break;
        case ONE_OP:
            if(want_exec_result)
            {
                acpi_object_t *result = acpi_exec_push_opstack_or_die(state);
                result->type = ACPI_INTEGER;
                result->integer = 1;
            }
            state->pc++;
            break;
        case ONES_OP:
            if(want_exec_result)
            {
                acpi_object_t *result = acpi_exec_push_opstack_or_die(state);
                result->type = ACPI_INTEGER;
                result->integer = ~((uint64_t)0);
            }
            state->pc++;
            break;

        case BYTEPREFIX:
        case WORDPREFIX:
        case DWORDPREFIX:
        case QWORDPREFIX:
        {
            uint64_t integer;
            size_t integer_size = acpi_eval_integer(method + state->pc, &integer);
            if(!integer_size)
                acpi_panic("failed to parse integer opcode\n");
            if(want_exec_result)
            {
                acpi_object_t *result = acpi_exec_push_opstack_or_die(state);
                result->type = ACPI_INTEGER;
                result->integer = integer;
            }
            state->pc += integer_size;
            continue;
        }
        case PACKAGE_OP:
        {
            size_t encoded_size;
            acpi_parse_pkgsize(method + state->pc + 1, &encoded_size);

            if(want_exec_result)
            {
                acpi_object_t *result = acpi_exec_push_opstack_or_die(state);
                result->type = ACPI_PACKAGE;
                result->package = acpi_calloc(sizeof(acpi_object_t), ACPI_MAX_PACKAGE_ENTRIES);
                result->package_size = acpins_create_package(state->handle,
                        result->package, method + state->pc);
            }
            state->pc += encoded_size + 1;
            break;
        }

        case (EXTOP_PREFIX << 8) | SLEEP_OP:
            state->pc += acpi_exec_sleep(&method[state->pc], state);
            break;

        /* A control method can return literally any object */
        /* So we need to take this into consideration */
        case RETURN_OP:
        {
            state->pc++;
            acpi_object_t result = {0};
            state->pc += acpi_eval_object(&result, state, method + state->pc);

            // Find the last LAI_METHOD_CONTEXT_STACKITEM on the stack.
            int j = 0;
            acpi_stackitem_t *method_item;
            while(1) {
                method_item = acpi_exec_peek_stack(state, j);
                if(!method_item)
                    acpi_panic("Return() outside of control method()\n");
                if(method_item->kind == LAI_METHOD_CONTEXT_STACKITEM)
                    break;
                // TODO: Verify that we only cross conditions/loops.
                j++;
            }

            // Remove the method stack item and push the return value.
            if(state->opstack_ptr) // This is an internal error.
                acpi_panic("opstack is not empty before return\n");
            acpi_object_t *opstack_res = acpi_exec_push_opstack_or_die(state);
            acpi_move_object(opstack_res, &result);

            acpi_exec_pop_stack(state, j + 1);
            break;
        }
        /* While Loops */
        case WHILE_OP:
        {
            size_t loop_size;
            state->pc++;
            size_t j = state->pc;
            state->pc += acpi_parse_pkgsize(&method[state->pc], &loop_size);

            acpi_stackitem_t *loop_item = acpi_exec_push_stack_or_die(state);
            loop_item->kind = LAI_LOOP_STACKITEM;
            loop_item->loop_pred = state->pc;
            loop_item->loop_end = j + loop_size;
            break;
        }
        /* Continue Looping */
        case CONTINUE_OP:
        {
            // Find the last LAI_LOOP_STACKITEM on the stack.
            int j = 0;
            acpi_stackitem_t *loop_item;
            while(1) {
                loop_item = acpi_exec_peek_stack(state, j);
                if(!loop_item)
                    acpi_panic("Continue() outside of While()\n");
                if(loop_item->kind == LAI_LOOP_STACKITEM)
                    break;
                // TODO: Verify that we only cross conditions/loops.
                j++;
            }

            // Keep the loop item but remove nested items from the exeuction stack.
            state->pc = loop_item->loop_pred;
            acpi_exec_pop_stack(state, j);
            break;
        }
        /* Break Loop */
        case BREAK_OP:
        {
            // Find the last LAI_LOOP_STACKITEM on the stack.
            int j = 0;
            acpi_stackitem_t *loop_item;
            while(1) {
                loop_item = acpi_exec_peek_stack(state, j);
                if(!loop_item)
                    acpi_panic("Break() outside of While()\n");
                if(loop_item->kind == LAI_LOOP_STACKITEM)
                    break;
                // TODO: Verify that we only cross conditions/loops.
                j++;
            }

            // Remove the loop item from the execution stack.
            state->pc = loop_item->loop_end;
            acpi_exec_pop_stack(state, j + 1);
            break;
        }
        /* If/Else Conditional */
        case IF_OP:
        {
            size_t if_size;
            state->pc++;
            size_t j = state->pc;
            state->pc += acpi_parse_pkgsize(method + state->pc, &if_size);

            // Evaluate the predicate
            acpi_object_t predicate = {0};
            state->pc += acpi_eval_object(&predicate, state, method + state->pc);

            acpi_stackitem_t *cond_item = acpi_exec_push_stack_or_die(state);
            cond_item->kind = LAI_COND_STACKITEM;
            cond_item->cond_taken = predicate.integer;
            cond_item->cond_end = j + if_size;

            if(!cond_item->cond_taken)
                state->pc = cond_item->cond_end;

            break;
        }
        case ELSE_OP:
            acpi_panic("Else() outside of If()\n");
            break;

        /* Most of the type 2 opcodes are implemented in exec2.c */
        case NAME_OP:
            state->pc += acpi_exec_name(&method[state->pc], state);
            break;
        case BYTEFIELD_OP:
            state->pc += acpi_exec_bytefield(&method[state->pc], state);
            break;
        case WORDFIELD_OP:
            state->pc += acpi_exec_wordfield(&method[state->pc], state);
            break;
        case DWORDFIELD_OP:
            state->pc += acpi_exec_dwordfield(&method[state->pc], state);
            break;

        case ARG0_OP:
        case ARG1_OP:
        case ARG2_OP:
        case ARG3_OP:
        case ARG4_OP:
        case ARG5_OP:
        case ARG6_OP:
        {
            if(want_exec_result)
            {
                acpi_object_t *result = acpi_exec_push_opstack_or_die(state);
                acpi_copy_object(result, &state->arg[opcode - ARG0_OP]);
            }
            state->pc++;
            break;
        }

        case LOCAL0_OP:
        case LOCAL1_OP:
        case LOCAL2_OP:
        case LOCAL3_OP:
        case LOCAL4_OP:
        case LOCAL5_OP:
        case LOCAL6_OP:
        case LOCAL7_OP:
        {
            if(want_exec_result)
            {
                acpi_object_t *result = acpi_exec_push_opstack_or_die(state);
                acpi_copy_object(result, &state->local[opcode - LOCAL0_OP]);
            }
            state->pc++;
            break;
        }

        case STORE_OP:
        case NOT_OP:
        {
            acpi_stackitem_t *op_item = acpi_exec_push_stack_or_die(state);
            op_item->kind = LAI_OP_STACKITEM;
            op_item->op_opcode = method[state->pc];
            op_item->op_opstack = state->opstack_ptr;
            op_item->op_num_operands = 1;
            op_item->op_want_result = want_exec_result;
            state->pc++;
            break;
        }
        case ADD_OP:
        case SUBTRACT_OP:
        case MULTIPLY_OP:
        case AND_OP:
        case OR_OP:
        case XOR_OP:
        case SHR_OP:
        case SHL_OP:
        {
            acpi_stackitem_t *op_item = acpi_exec_push_stack_or_die(state);
            op_item->kind = LAI_OP_STACKITEM;
            op_item->op_opcode = method[state->pc];
            op_item->op_opstack = state->opstack_ptr;
            op_item->op_num_operands = 2;
            op_item->op_want_result = want_exec_result;
            state->pc++;
            break;
        }
        case INCREMENT_OP:
            state->pc += acpi_exec_increment(&method[state->pc], state);
            break;
        case DECREMENT_OP:
            state->pc += acpi_exec_decrement(&method[state->pc], state);
            break;
        case DIVIDE_OP:
            state->pc += acpi_exec_divide(&method[state->pc], state);
            break;
        default:
            // Opcodes that we do not natively handle here still need to be passed
            // to acpi_eval_object(). TODO: Get rid of this call.
            acpi_debug("opcode 0x%02X is handled by acpi_eval_object()\n", opcode);
            if(want_exec_result)
            {
                acpi_object_t *operand = acpi_exec_push_opstack_or_die(state);
                state->pc += acpi_eval_object(operand, state, method + state->pc);
            }else{
                acpi_object_t discarded = {0};
                state->pc += acpi_eval_object(&discarded, state, method + state->pc);
                acpi_free_object(&discarded);
            }
        }
    }

    return 0;
}

// acpi_exec_method(): Finds and executes a control method
// Param:    acpi_state_t *state - method name and arguments
// Param:    acpi_object_t *method_return - return value of method
// Return:    int - 0 on success

int acpi_exec_method(acpi_state_t *state)
{
    // Check for OS-defined methods.
    if(state->handle->method_override)
        return state->handle->method_override(state->arg, &state->retvalue);

    // Okay, by here it's a real method.
    //acpi_debug("execute control method %s\n", state->handle->path);
    acpi_stackitem_t *item = acpi_exec_push_stack_or_die(state);
    item->kind = LAI_METHOD_CONTEXT_STACKITEM;

    state->pc = 0;
    state->limit = state->handle->size;
    int status = acpi_exec_run(state->handle->pointer, state);
    if(status)
        return status;

    /*acpi_debug("%s finished, ", state->handle->path);

    if(state->retvalue.type == ACPI_INTEGER)
        acpi_debug("return value is integer: %d\n", state->retvalue.integer);
    else if(state->retvalue.type == ACPI_STRING)
        acpi_debug("return value is string: '%s'\n", state->retvalue.string);
    else if(state->retvalue.type == ACPI_PACKAGE)
        acpi_debug("return value is package\n");
    else if(state->retvalue.type == ACPI_BUFFER)
        acpi_debug("return value is buffer\n");*/

    if(state->opstack_ptr != 1) // This would be an internal error.
        acpi_panic("expected exactly one return value after method invocation\n");
    acpi_object_t *result = acpi_exec_get_opstack(state, 0);
    acpi_move_object(&state->retvalue, result);
    acpi_exec_pop_opstack(state, 1);
    return 0;
}

// acpi_methodinvoke(): Executes a MethodInvokation
// Param:    void *data - pointer to MethodInvokation
// Param:    acpi_state_t *old_state - state of currently executing method
// Param:    acpi_object_t *method_return - object to store return value
// Return:    size_t - size in bytes for skipping

size_t acpi_methodinvoke(void *data, acpi_state_t *old_state, acpi_object_t *method_return)
{
    uint8_t *methodinvokation = (uint8_t*)data;

    size_t return_size = 0;

    // determine the name of the method
    char path[ACPI_MAX_NAME];
    size_t name_size = acpins_resolve_path(old_state->handle, path, methodinvokation);
    return_size += name_size;
    methodinvokation += name_size;

    acpi_nsnode_t *method = acpi_exec_resolve(path);
    if(!method)
        acpi_panic("undefined MethodInvokation %s\n", path);

    acpi_state_t state;
    acpi_init_call_state(&state, method);
    uint8_t argc = method->method_flags & METHOD_ARGC_MASK;
    uint8_t current_argc = 0;
    size_t arg_size;
    if(argc != 0)
    {
        // parse method arguments here
        while(current_argc < argc)
        {
            arg_size = acpi_eval_object(&state.arg[current_argc], old_state, methodinvokation);
            methodinvokation += arg_size;
            return_size += arg_size;

            current_argc++;
        }
    }

    // execute
    acpi_exec_method(&state);
    acpi_move_object(method_return, &state.retvalue);
    acpi_finalize_state(&state);

    return return_size;
}

// acpi_exec_sleep(): Executes a Sleep() opcode
// Param:    void *data - opcode data
// Param:    acpi_state_t *state - AML VM state
// Return:    size_t - size in bytes for skipping

size_t acpi_exec_sleep(void *data, acpi_state_t *state)
{
    size_t return_size = 2;
    uint8_t *opcode = (uint8_t*)data;
    opcode += 2;        // skip EXTOP_PREFIX and SLEEP_OP

    acpi_object_t time = {0};
    return_size += acpi_eval_object(&time, state, &opcode[0]);

    if(time.integer == 0)
        time.integer = 1;

    acpi_sleep(time.integer);

    return return_size;
}




