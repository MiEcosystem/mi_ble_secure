/***********************************************************************************************//**
 * \file   main.c
 * \brief  Silicon Labs Empty Example Project
 *
 * This example demonstrates the bare minimum needed for a Blue Gecko C application
 * that allows Over-the-Air Device Firmware Upgrading (OTA DFU). The application
 * starts advertising after boot and restarts advertising after a connection is closed.
 ***************************************************************************************************
 * <b> (C) Copyright 2016 Silicon Labs, http://www.silabs.com</b>
 ***************************************************************************************************
 * This file is licensed under the Silabs License Agreement. See the file
 * "Silabs_License_Agreement.txt" for details. Before using this software for
 * any purpose, you must agree to the terms of that agreement.
 **************************************************************************************************/
#include <time.h>

/* Board headers */
#include "init_mcu.h"
#include "init_board.h"
#include "init_app.h"
#include "ble-configuration.h"
#include "board_features.h"

/* Bluetooth stack headers */
#include "bg_types.h"
#include "native_gecko.h"
#include "gatt_db.h"

/* Libraries containing default Gecko configuration values */
#include "em_emu.h"
#include "em_cmu.h"
#include "em_gpio.h"
#include "em_rtcc.h"
#include "gpiointerrupt.h"

/* Device initialization header */
#include "hal-config.h"

#if defined(HAL_CONFIG)
#include "bsphalconfig.h"
#else
#include "bspconfig.h"
#endif

/* Mijia BLE API and Middleware */
#include "efr32_api.h"
#include "mible_api.h"
#include "mible_trace.h"
#include "common/mible_beacon.h"
#include "secure_auth/mible_secure_auth.h"
#include "mijia_profiles/mi_service_server.h"
#include "mijia_profiles/lock_service_server.h"

#undef  MI_LOG_MODULE_NAME
#define MI_LOG_MODULE_NAME __FILE__
#include "mible_log.h"

#include "mi_config.h"

#ifndef MAX_CONNECTIONS
#define MAX_CONNECTIONS                1
#endif
#ifndef MAX_ADVERTISERS
#define MAX_ADVERTISERS                2
#endif

#define MSEC_TO_UNITS(TIME, RESOLUTION) (((TIME) * 1000) / (RESOLUTION))

#define TIMER_ID_OBJ_PERIOD_ADV        11
#define TIMER_ID_CLEAR_BIND_CFM        12

/// Number of ticks after which press is considered to be long (1s)
#define LONG_PRESS_TIME_TICKS           (32768)
#define EXT_SIGNAL_PB0_SHORT_PRESS      (1<<0)
#define EXT_SIGNAL_PB0_LONG_PRESS       (1<<1)
#define EXT_SIGNAL_PB1_SHORT_PRESS      (1<<2)

static uint8_t bluetooth_stack_heap[DEFAULT_BLUETOOTH_HEAP(MAX_CONNECTIONS)];

/* Bluetooth stack configuration parameters (see "UG136: Silicon Labs Bluetooth C Application Developer's Guide" for details on each parameter) */
static gecko_configuration_t config = {
  .config_flags = 0,                                   /* Check flag options from UG136 */
#if defined(FEATURE_LFXO)
  .sleep.flags = SLEEP_FLAGS_DEEP_SLEEP_ENABLE,        /* Sleep is enabled */
#else
  .sleep.flags = 0,
#endif // LFXO
  .bluetooth.max_connections = MAX_CONNECTIONS,        /* Maximum number of simultaneous connections */
  .bluetooth.max_advertisers = MAX_ADVERTISERS,        /* Maximum number of advertisement sets */
  .bluetooth.heap = bluetooth_stack_heap,              /* Bluetooth stack memory for connection management */
  .bluetooth.heap_size = sizeof(bluetooth_stack_heap), /* Bluetooth stack memory for connection management */
  .bluetooth.sleep_clock_accuracy = 100,               /* Accuracy of the Low Frequency Crystal Oscillator in ppm. *
                                                        * Do not modify if you are using a module                  */
  .gattdb = &bg_gattdb_data,                           /* Pointer to GATT database */
#if (HAL_PA_ENABLE)
  .pa.config_enable = 1, // Enable high power PA
#if defined(FEATURE_PA_INPUT_FROM_VBAT)
  .pa.input = GECKO_RADIO_PA_INPUT_VBAT, // Configure PA input to VBAT
#else
  .pa.input = GECKO_RADIO_PA_INPUT_DCDC,               /* Configure PA input to DCDC */
#endif // (HAL_PA_ENABLE) && defined(FEATURE_PA_HIGH_POWER)
#endif // (HAL_PA_ENABLE)
  .rf.flags = GECKO_RF_CONFIG_ANTENNA,                 /* Enable antenna configuration. */
  .rf.antenna = GECKO_RF_ANTENNA,                      /* Select antenna path! */
  .max_timers = 8,
};

static const iic_config_t msc_iic_config = {
        .scl_pin = BSP_I2C0_SCL_PIN,
        .scl_port = BSP_I2C0_SCL_PORT,
        .scl_extra_conf = BSP_I2C0_SCL_LOC,
        .sda_pin = BSP_I2C0_SDA_PIN,
        .sda_port = BSP_I2C0_SDA_PORT,
        .sda_extra_conf = BSP_I2C0_SDA_LOC,
        .freq = HAVE_MSC==1 ? IIC_100K : IIC_400K,
};

#define PAIRCODE_NUMS 6
bool need_kbd_input;
uint8_t pair_code_num;
uint8_t pair_code[PAIRCODE_NUMS];
/// button press timestamp for very long/long/short Push Button 0 press detection
static uint32 pb0_press, pb1_press;

extern void time_init(struct tm * time_ptr);

static void advertising_init(uint8_t solicite_bind)
{
    MI_LOG_INFO("advertising init...\n");

    if(MI_SUCCESS != mibeacon_adv_data_set(solicite_bind, 0, NULL, 0)){
        MI_LOG_ERROR("mibeacon_data_set failed. \r\n");
    }
}

static void advertising_start(void)
{
    MI_LOG_INFO("advertising start...\n");
    mibeacon_adv_start(300);
}

int scan_keyboard(uint8_t *pdata, uint8_t len)
{
    if (pdata == NULL)
        return 0;

    return SEGGER_RTT_ReadNoLock(0, pdata, len);
}

void flush_keyboard_buffer(void)
{
    uint8_t tmp[16];
    while(SEGGER_RTT_ReadNoLock(0, tmp, 16));
}

void gpio_irq_handler(uint8_t pin)
{
    uint32_t t_diff;

    if (pin == BSP_BUTTON0_PIN) {
        if (GPIO_PinInGet(BSP_BUTTON0_PORT, BSP_BUTTON0_PIN) == 0) {
            // PB0 pressed - record RTCC timestamp
            pb0_press = RTCC_CounterGet();
        } else {
            // PB0 released - check if it was short or long press
            t_diff = RTCC_CounterGet() - pb0_press;
            if (t_diff < LONG_PRESS_TIME_TICKS) {
                gecko_external_signal(EXT_SIGNAL_PB0_SHORT_PRESS);
            } else {
                gecko_external_signal(EXT_SIGNAL_PB0_LONG_PRESS);
            }
        }
    }

    if (pin == BSP_BUTTON1_PIN) {
        if (GPIO_PinInGet(BSP_BUTTON0_PORT, BSP_BUTTON1_PIN) == 0) {
            // PB0 pressed - record RTCC timestamp
            pb1_press = RTCC_CounterGet();
        } else {
            // PB0 released - check if it was short or long press
            t_diff = RTCC_CounterGet() - pb1_press;
            if (t_diff < LONG_PRESS_TIME_TICKS) {
                gecko_external_signal(EXT_SIGNAL_PB1_SHORT_PRESS);
            } else {

            }
        }
    }
}

void button_init(void)
{
    // configure pushbutton PB0 and PB1 as inputs, with pull-up enabled
    GPIO_PinModeSet(BSP_BUTTON0_PORT, BSP_BUTTON0_PIN, gpioModeInputPull, 1);
    GPIO_PinModeSet(BSP_BUTTON1_PORT, BSP_BUTTON1_PIN, gpioModeInputPull, 1);

    GPIOINT_Init();

    /* configure interrupt for PB0 and PB1, both falling and rising edges */
    GPIO_ExtIntConfig(BSP_BUTTON0_PORT, BSP_BUTTON0_PIN, BSP_BUTTON0_PIN, true, true, true);
    GPIO_ExtIntConfig(BSP_BUTTON1_PORT, BSP_BUTTON1_PIN, BSP_BUTTON1_PIN, true, true, true);

    /* register the callback function that is invoked when interrupt occurs */
    GPIOINT_CallbackRegister(BSP_BUTTON0_PIN, gpio_irq_handler);
    GPIOINT_CallbackRegister(BSP_BUTTON1_PIN, gpio_irq_handler);
}

static void enqueue_new_objs()
{
    static int8_t  battery;
    battery = battery < 100 ? battery + 1 : 0;
    mibeacon_obj_enque(MI_STA_BATTERY, sizeof(battery), &battery, 0);
}

void mi_schd_event_handler(schd_evt_t *p_event)
{
    MI_LOG_INFO("USER CUSTOM CALLBACK RECV EVT ID %d\n", p_event->id);
    switch(p_event->id) {
    case SCHD_EVT_OOB_REQUEST:
        need_kbd_input = true;
        flush_keyboard_buffer();
        MI_LOG_INFO(MI_LOG_COLOR_GREEN "Please input your pair code ( MUST be 6 digits ) : \n");
        break;

    case SCHD_EVT_REG_SUCCESS:
        // set register bit.
        advertising_init(0);
        gecko_cmd_hardware_set_soft_timer(SEC_2_TIMERTICK(60), TIMER_ID_OBJ_PERIOD_ADV, 0);
        break;

    case SCHD_EVT_KEY_DEL_SUCC:
        // clear register bit.
        advertising_init(0);
        break;

    default:
        break;
    }
}

static bool m_allow_sleep = true;
static int msc_pwr_manage(bool power_stat)
{
    if (power_stat == 1) {
        GPIO_PinModeSet(gpioPortA, 0, gpioModeWiredAndPullUp, 1);
        GPIO_PinOutSet(gpioPortA, 0);
        m_allow_sleep = false;
    } else {
        GPIO_PinOutClear(gpioPortA, 0);
        GPIO_PinModeSet(gpioPortA, 0, gpioModeDisabled, 0);
        m_allow_sleep = true;
    }
    return 0;
}

void lock_ops_handler (uint8_t opcode)
{
    switch(opcode) {
    case 0:
        MI_LOG_INFO(" unlock \n");
        break;

    case 1:
        MI_LOG_INFO(" lock \n");
        break;

    case 2:
        MI_LOG_INFO(" bolt \n");
        break;

    default:
        MI_LOG_ERROR("lock opcode error %d", opcode);
    }

    lock_event_t obj_lock_event;
    obj_lock_event.action = opcode;
    obj_lock_event.method = 0;
    obj_lock_event.user_id= get_mi_key_id();
    obj_lock_event.time   = time(NULL);

    mibeacon_obj_enque(MI_EVT_LOCK, sizeof(obj_lock_event), &obj_lock_event, 0);

    reply_lock_stat(opcode);
    send_lock_log(MI_EVT_LOCK, sizeof(obj_lock_event), &obj_lock_event);
}

static void process_system_boot(struct gecko_cmd_packet *evt)
{

    struct gecko_msg_system_boot_evt_t boot_info = evt->data.evt_system_boot;
    MI_LOG_INFO("system stack %d.%0d.%0d-%d, heap %d bytes\n", boot_info.major, boot_info.minor, boot_info.patch, boot_info.build,sizeof(bluetooth_stack_heap));

    gecko_cmd_system_set_tx_power(0);
    /* Start general advertising and enable connections. */
    advertising_init(0);
    advertising_start();

    mi_service_init();

    lock_init_t lock_cfg = {
            .opcode_handler = lock_ops_handler
    };
    lock_service_init(&lock_cfg);

    mible_libs_config_t lib_cfg = {
            .msc_onoff = msc_pwr_manage,
            .p_msc_iic_config = (void*)&msc_iic_config,
    };

    mi_scheduler_init(10, mi_schd_event_handler, &lib_cfg);
    mi_scheduler_start(SYS_KEY_RESTORE);
}

static void process_softtimer(struct gecko_cmd_packet *evt)
{
    switch (evt->data.evt_hardware_soft_timer.handle) {
    case TIMER_ID_OBJ_PERIOD_ADV:
        MI_LOG_WARNING("systime %d\n", gecko_cmd_hardware_get_time()->seconds);
        enqueue_new_objs();
        break;

    case TIMER_ID_CLEAR_BIND_CFM:
        advertising_init(0);
        break;
    }
}

static void process_external_signal(struct gecko_cmd_packet *evt)
{
    if (evt->data.evt_system_external_signal.extsignals & EXT_SIGNAL_PB0_SHORT_PRESS) {
        if (get_mi_reg_stat()) {
            enqueue_new_objs();
        } else {
            MI_LOG_DEBUG("Set bind confirm bit in mibeacon.\n");
            advertising_init(1);
            gecko_cmd_hardware_set_soft_timer(SEC_2_TIMERTICK(10), TIMER_ID_CLEAR_BIND_CFM, 1);
        }
    }

    if (evt->data.evt_system_external_signal.extsignals & EXT_SIGNAL_PB0_LONG_PRESS) {
    	MI_LOG_DEBUG("Factory reset.\n");
        mi_scheduler_start(SYS_KEY_DELETE);
        gecko_cmd_hardware_set_soft_timer(0, TIMER_ID_OBJ_PERIOD_ADV, 0);
    }

    if (evt->data.evt_system_external_signal.extsignals & EXT_SIGNAL_PB1_SHORT_PRESS) {
    	MI_LOG_DEBUG("MSC Self test.\n");
        mi_scheduler_start(SYS_MSC_SELF_TEST);

    }
}

void user_stack_event_handler(struct gecko_cmd_packet* evt)
{
    /* Handle events */
    switch (BGLIB_MSG_ID(evt->header)) {
    /* This boot event is generated when the system boots up after reset.
     * Do not call any stack commands before receiving the boot event.
     * Here the system is set to start advertising immediately after boot procedure. */
    case gecko_evt_system_boot_id:
        process_system_boot(evt);
        break;

    case gecko_evt_hardware_soft_timer_id:
        process_softtimer(evt);
        break;

    case gecko_evt_system_external_signal_id:
        process_external_signal(evt);
        break;

    default:
        break;
    }
}

int main()
{
    // Initialize device
    initMcu();
    // Initialize board
    initBoard();
    // Initialize application
    initApp();

    MI_LOG_INFO(RTT_CTRL_CLEAR"\n");
    MI_LOG_INFO("Compiled %s %s\n", __DATE__, __TIME__);
    MI_LOG_INFO("system clock %d Hz\n", SystemCoreClockGet());

    // Initialize stack
    gecko_stack_init(&config);
    gecko_bgapi_class_system_init();
    gecko_bgapi_class_le_gap_init();
    gecko_bgapi_class_le_connection_init();
    gecko_bgapi_class_gatt_server_init();
    gecko_bgapi_class_hardware_init();
    gecko_bgapi_class_flash_init();

    button_init();
    time_init(NULL);

    /* Event pointer for handling events */
    struct gecko_cmd_packet* evt;

    while (1) {
        /* Process stack event */
        do {
            evt = gecko_peek_event();
            if (evt != NULL) {
                mible_stack_event_handler(evt);

                /* customized event handler can be added here. */
                user_stack_event_handler(evt);
            }
        } while (evt != NULL);

        /* Process mi scheduler */
        mi_schd_process();

        /* Scan keyboard */
        if (need_kbd_input) {
            if (pair_code_num < PAIRCODE_NUMS) {
                pair_code_num += scan_keyboard(pair_code + pair_code_num, PAIRCODE_NUMS - pair_code_num);
            }
            if (pair_code_num == PAIRCODE_NUMS) {
                pair_code_num = 0;
                need_kbd_input = false;
                mi_schd_oob_rsp(pair_code, sizeof(pair_code));
            }
        }

        /* Enter low power mode */
        if (m_allow_sleep == true){
            /* Go to sleep. Sleeping will be avoided if there isn't enough time to sleep */
            gecko_sleep_for_ms(gecko_can_sleep_ms());
        } else {
            /* Block EM2/3/4 entry, because of I2C master driver only work in EM0/1. */
        }
    }
}


