/*
 * This file is part of the MicroPython project, http://micropython.org/
 *
 * The MIT License (MIT)
 *
 * Copyright (c) 2020 Damien P. George
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

#include "py/runtime.h"
#include "py/mphal.h"
#include "drv_softtimer.h"
#include "modmicrobit.h"

typedef struct _microbit_button_obj_t {
    mp_obj_base_t base;
    const microbit_pin_obj_t *pin;
    uint8_t button_id;
} microbit_button_obj_t;

mp_obj_t microbit_button_is_pressed(mp_obj_t self_in) {
    microbit_button_obj_t *self = (microbit_button_obj_t *)self_in;
    return mp_obj_new_bool(microbit_hal_button_state(self->button_id, NULL, NULL));
}
MP_DEFINE_CONST_FUN_OBJ_1(microbit_button_is_pressed_obj, microbit_button_is_pressed);

mp_obj_t microbit_button_get_presses(mp_obj_t self_in) {
    microbit_button_obj_t *self = (microbit_button_obj_t *)self_in;
    int n;
    microbit_hal_button_state(self->button_id, NULL, &n);
    return MP_OBJ_NEW_SMALL_INT(n);
}
MP_DEFINE_CONST_FUN_OBJ_1(microbit_button_get_presses_obj, microbit_button_get_presses);

mp_obj_t microbit_button_was_pressed(mp_obj_t self_in) {
    microbit_button_obj_t *self = (microbit_button_obj_t *)self_in;
    int n;
    microbit_hal_button_state(self->button_id, &n, NULL);
    return mp_obj_new_bool(n);
}
MP_DEFINE_CONST_FUN_OBJ_1(microbit_button_was_pressed_obj, microbit_button_was_pressed);

static const mp_map_elem_t microbit_button_locals_dict_table[] = {
    { MP_OBJ_NEW_QSTR(MP_QSTR_is_pressed), (mp_obj_t)&microbit_button_is_pressed_obj },
    { MP_OBJ_NEW_QSTR(MP_QSTR_was_pressed), (mp_obj_t)&microbit_button_was_pressed_obj },
    { MP_OBJ_NEW_QSTR(MP_QSTR_get_presses), (mp_obj_t)&microbit_button_get_presses_obj },
};

static MP_DEFINE_CONST_DICT(microbit_button_locals_dict, microbit_button_locals_dict_table);

MP_DEFINE_CONST_OBJ_TYPE(
    microbit_button_type,
    MP_QSTR_MicroBitButton,
    MP_TYPE_FLAG_NONE,
    locals_dict, &microbit_button_locals_dict
    );

const microbit_button_obj_t microbit_button_a_obj = {
    .base = { &microbit_button_type },
    .pin = &microbit_p5_obj,
    .button_id = MICROBIT_HAL_BUTTON_A,
};

const microbit_button_obj_t microbit_button_b_obj = {
    .base = { &microbit_button_type },
    .pin = &microbit_p11_obj,
    .button_id = MICROBIT_HAL_BUTTON_B,
};

// Virtual button that is pressed when A and B are pressed together.
const microbit_button_obj_t microbit_button_ab_obj = {
    .base = { &microbit_button_type },
    .pin = NULL,
    .button_id = MICROBIT_HAL_BUTTON_AB,
};

// This function assumes "button" is of type microbit_button_type.
uint8_t microbit_obj_get_button_id(mp_obj_t button) {
    return ((microbit_button_obj_t *)MP_OBJ_TO_PTR(button))->button_id;
}

/******************************************************************************/
// press_button decorator

// Object returned by press_button(button).  Calling it with a function registers
// that function to be run whenever the button is pressed.  "Pressed" means pressed
// and then released (a click), like MakeCode, so that button_ab can be distinguished
// from button_a and button_b.
typedef struct _microbit_press_button_obj_t {
    mp_obj_base_t base;
    uint8_t button_id;
    mp_obj_t user_callback;
} microbit_press_button_obj_t;

// Runs in the main thread via the scheduler, after the button was pressed.
static mp_obj_t microbit_press_button_callback(mp_obj_t self_in) {
    microbit_press_button_obj_t *self = MP_OBJ_TO_PTR(self_in);

    if (MP_STATE_PORT(button_press_handler)[self->button_id] != self_in) {
        // Handler was replaced or removed after this event was scheduled.
        return mp_const_none;
    }

    nlr_buf_t nlr;
    if (nlr_push(&nlr) == 0) {
        mp_call_function_0(self->user_callback);
        nlr_pop();
    } else {
        // Exception raised, so stop this handler from being called again.
        MP_STATE_PORT(button_press_handler)[self->button_id] = MP_OBJ_NULL;

        mp_obj_t exc = MP_OBJ_FROM_PTR(nlr.ret_val);
        if (microbit_outer_nlr_will_handle_soft_timer_exceptions) {
            // The outer NLR handler will handle this exception so raise it via a SystemExit.
            mp_obj_t args[2] = {mp_const_none, exc};
            mp_sched_exception(mp_obj_exception_make_new(&mp_type_SystemExit, 2, 0, args));
        } else {
            // Print exception to stdout right now.
            mp_obj_print_exception(&mp_plat_print, exc);
        }
    }

    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(microbit_press_button_callback_obj, microbit_press_button_callback);

static mp_obj_t microbit_press_button_obj_call(mp_obj_t self_in, size_t n_args, size_t n_kw, const mp_obj_t *args) {
    microbit_press_button_obj_t *self = MP_OBJ_TO_PTR(self_in);
    mp_arg_check_num(n_args, n_kw, 1, 1, false);
    self->user_callback = args[0];
    MP_STATE_PORT(button_press_handler)[self->button_id] = self_in;
    // Return the function unchanged so it can still be called directly.
    return args[0];
}

static MP_DEFINE_CONST_OBJ_TYPE(
    microbit_press_button_type,
    MP_QSTR_press_button,
    MP_TYPE_FLAG_NONE,
    call, microbit_press_button_obj_call
    );

// press_button(button, callback=None)
//
// Used as a decorator:
//
//     @press_button(button_a)
//     def on_a():
//         display.show(Image.HEART)
//
// or as a function: press_button(button_a, on_a).  Use button_ab for A and B
// pressed together.  Only one handler per button; registering again replaces
// the previous one.
static mp_obj_t microbit_press_button(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {
    enum { ARG_button, ARG_callback };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_button, MP_ARG_REQUIRED | MP_ARG_OBJ },
        { MP_QSTR_callback, MP_ARG_OBJ, {.u_rom_obj = MP_ROM_NONE} },
    };

    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all(n_args, pos_args, kw_args, MP_ARRAY_SIZE(allowed_args), allowed_args, args);

    if (!mp_obj_is_type(args[ARG_button].u_obj, &microbit_button_type)) {
        mp_raise_TypeError(MP_ERROR_TEXT("expecting a button"));
    }

    microbit_press_button_obj_t *self = mp_obj_malloc(microbit_press_button_obj_t, &microbit_press_button_type);
    self->button_id = microbit_obj_get_button_id(args[ARG_button].u_obj);
    self->user_callback = MP_OBJ_NULL;

    if (args[ARG_callback].u_obj == mp_const_none) {
        // Return decorator-compatible object.
        return MP_OBJ_FROM_PTR(self);
    } else {
        // Register the callback now.
        return microbit_press_button_obj_call(MP_OBJ_FROM_PTR(self), 1, 0, &args[ARG_callback].u_obj);
    }
}
MP_DEFINE_CONST_FUN_OBJ_KW(microbit_press_button_obj, 1, microbit_press_button);

// Called from the HAL, at interrupt priority, when a button event occurs.
void microbit_hal_button_callback(int button, int event) {
    // A long press is also a press, so the handler runs whenever the button is released.
    if (event == MICROBIT_HAL_BUTTON_EVT_CLICK || event == MICROBIT_HAL_BUTTON_EVT_LONG_CLICK) {
        mp_obj_t handler = MP_STATE_PORT(button_press_handler)[button];
        if (handler != MP_OBJ_NULL) {
            // If the schedule queue is full then this press is dropped.
            mp_sched_schedule(MP_OBJ_FROM_PTR(&microbit_press_button_callback_obj), handler);
        }
    }
}

// Return true if any press_button handler is registered.
bool microbit_button_has_handlers(void) {
    for (size_t i = 0; i < MP_ARRAY_SIZE(MP_STATE_PORT(button_press_handler)); ++i) {
        if (MP_STATE_PORT(button_press_handler)[i] != MP_OBJ_NULL) {
            return true;
        }
    }
    return false;
}

// Remove all press_button handlers, eg at the end of a script or on soft reset.
void microbit_button_deinit(void) {
    for (size_t i = 0; i < MP_ARRAY_SIZE(MP_STATE_PORT(button_press_handler)); ++i) {
        MP_STATE_PORT(button_press_handler)[i] = MP_OBJ_NULL;
    }
}

MP_REGISTER_ROOT_POINTER(mp_obj_t button_press_handler[3]);
