/* main.c - Application main entry point */

/*
 * Copyright (c) 2016 Intel Corporation
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/types.h>
#include <stddef.h>
#include <string.h>
#include <errno.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/kernel.h>

#include <zephyr/types.h>
#include <zephyr/settings/settings.h>

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/pm/device.h>

#include "hog.h"
#include "zephyr/sys/poweroff.h"

#include <zephyr/drivers/display.h>

const struct device *disp = DEVICE_DT_GET(DT_NODELABEL(lcd0));
static uint16_t line[128];   /* 一行 256B，避免 128*150*2=38KB 大缓冲 */

#define LED0_NODE DT_NODELABEL(led0)
#define ADV_IDLE_TIMEOUT_S  10
bool adv_flag = false;
bool power = true;
const struct gpio_dt_spec led0_spec = GPIO_DT_SPEC_GET(LED0_NODE, gpios);

//广播数据
static const struct bt_data ad[] =
{
	BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
	BT_DATA_BYTES(BT_DATA_UUID16_ALL,
		      BT_UUID_16_ENCODE(BT_UUID_HIDS_VAL),
		      BT_UUID_16_ENCODE(BT_UUID_BAS_VAL)),
};

void lcd_test(void)
{
	static const struct display_buffer_descriptor desc = {
		.width = 128,			/* 每行 128 像素 */
		.height = 1,			/* 一次写 1 行 */
		.pitch = 128,			/* 缓冲区行间距（像素） */
		.buf_size = sizeof(line),	/* 缓冲区大小 256 字节 */
	};

	display_blanking_off(disp);
	for (uint16_t y = 0; y < 150; y++) 
	{
		for (uint16_t x = 0; x < 128; x++) 
		{
			line[x] = (x < 64) ? 0xF800 : 0x07E0;   /* 左红右绿 */
		}

		display_write(disp, 0, y, &desc, line);
	}
}

//扫描回应数据
static const struct bt_data sd[] = 
{
	BT_DATA(BT_DATA_NAME_COMPLETE, CONFIG_BT_DEVICE_NAME, sizeof(CONFIG_BT_DEVICE_NAME) - 1),
};

void sys_off(struct k_work *work)
{
	printk("No connection for %d s, powering off\n", ADV_IDLE_TIMEOUT_S);

	gpio_pin_set_dt(&led0_spec, 1);

	power = false;
}

//定义延时功工作项
K_WORK_DELAYABLE_DEFINE(sleep_work,sys_off);

static void connected(struct bt_conn *conn, uint8_t err)
{
	char addr[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

	if (err) 
	{
		printk("Failed to connect to %s, err 0x%02x %s\n", addr,
		       err, bt_hci_err_to_str(err));
		return;
	}

	printk("Connected %s\n", addr);

	/* 请求加密
	 * HID over GATT 规范要求链路加密后主机才会启用输入报告通知 */
	if (bt_conn_set_security(conn, BT_SECURITY_L2))
	{
		printk("Failed to set security\n");
	}

	adv_flag = true;

	err = gpio_pin_set_dt(&led0_spec,0);
	if (err)
	{
		printk("设置LED常亮失败\r\n");
	}

	// k_work_cancel_delayable(&sleep_work);
	
}

static void disconnected(struct bt_conn *conn, uint8_t reason)
{
	char addr[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

	printk("Disconnected from %s, reason 0x%02x %s\n", addr,
	       reason, bt_hci_err_to_str(reason));

	gpio_pin_set_dt(&led0_spec,1);
	adv_flag = false;
}

//安全等级发生改变
static void security_changed(struct bt_conn *conn, bt_security_t level,
			     enum bt_security_err err)
{
	char addr[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

	if (!err) 
	{
		printk("Security changed: %s level %u\n", addr, level);//打印当前安全等级
	} 
	else 
	{
		printk("Security failed: %s level %u err %s(%d)\n", addr, level,
		       bt_security_err_to_str(err), err);
	}
}

void adv_start(void)
{
	int err;
	//开始广播
	err = bt_le_adv_start(BT_LE_ADV_CONN_FAST_1, ad, ARRAY_SIZE(ad), sd, ARRAY_SIZE(sd));
	if (err) 
	{
		printk("Advertising failed to start (err %d)\n", err);
		return;
	}

	// k_work_cancel_delayable(&sleep_work);
	// k_work_schedule(&sleep_work,K_SECONDS(ADV_IDLE_TIMEOUT_S));
}

//注册连接回调函数
BT_CONN_CB_DEFINE(conn_callbacks) = 
{
	.connected = connected,
	.disconnected = disconnected,
	.recycled = adv_start,
	.security_changed = security_changed,
};

static void bt_ready(int err)
{
	if (err) 
	{
		printk("Bluetooth init failed (err %d)\n", err);
		return;
	}

	printk("Bluetooth initialized\n");

	if (IS_ENABLED(CONFIG_SETTINGS)) 
	{
		settings_load();
	}

	//开始广播
	adv_start();

	printk("Advertising successfully started\n");
}

static void auth_passkey_display(struct bt_conn *conn, unsigned int passkey)
{
	char addr[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

	printk("Passkey for %s: %06u\n", addr, passkey);
}

static void auth_cancel(struct bt_conn *conn)
{
	char addr[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

	printk("Pairing cancelled: %s\n", addr);
}

static struct bt_conn_auth_cb auth_cb_display = 
{
	.passkey_display = auth_passkey_display,
	.passkey_entry = NULL,
	.cancel = auth_cancel,
};

void led_init(void)
{
	int err;
	err = gpio_pin_configure_dt(&led0_spec, GPIO_OUTPUT);
	if (err)
	{
		printk("LED初始化失败\r\n");
	}
	
}

int main(void)
{
	int err;
	bt_addr_le_t addr ;
	uint8_t blink_status = 1;

	led_init();

	//设置蓝牙地址存到addr中
	err = bt_addr_le_from_str("FF:EE:DD:CC:BB:AA","random",&addr);
	if(err)
	{
		printk("addr error\r\n");
	}

	//以addr，创建一个新的MAC地址
	err = bt_id_create(&addr,NULL);
	if(err)
	{
		printk("id creat error\r\n");
	}


	err = bt_enable(bt_ready);
	if (err) 
	{
		printk("Bluetooth init failed (err %d)\n", err);
		return 0;
	}

	if (IS_ENABLED(CONFIG_SAMPLE_BT_USE_AUTHENTICATION)) 
	{
		bt_conn_auth_cb_register(&auth_cb_display);
		printk("Bluetooth authentication callbacks registered.\n");
	}

	lcd_test();

	while (1)
	{
		if (adv_flag == false)
		{
			gpio_pin_set_dt(&led0_spec,(++blink_status) % 2);
		}

		k_sleep(K_MSEC(500));
		
	}

	return 0;
}
