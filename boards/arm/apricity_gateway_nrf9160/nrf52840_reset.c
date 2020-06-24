/*
 * Copyright (c) 2020 Nordic Semiconductor ASA.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <drivers/gpio.h>
#include <drivers/uart.h>
#include <device.h>

#define RESET_PIN CONFIG_BOARD_NRF52840_GPIO_RESET_PIN


/* PETE: need to resolve header issue -- drivers/uart.h is found
   in modules, not in zephyr as it should be */
static int uart_fifo_read(struct device *dev, u8_t *rx_data,
				 const int size)
{
	const struct uart_driver_api *api =
		(const struct uart_driver_api *)dev->driver_api;

	if (api->fifo_read) {
		return api->fifo_read(dev, rx_data, size);
	}

	return 0;
}

int bt_hci_transport_setup(struct device *dev)
{
	int err;
	char c;
	struct device *port;

	port = device_get_binding(DT_LABEL(DT_NODELABEL(gpio0)));
	if (!port) {
		return -EIO;
	}

	/* Configure pin as output and initialize it to low. */
	err = gpio_pin_configure(port, RESET_PIN, GPIO_OUTPUT_LOW);
	if (err) {
		return err;
	}

	/* Reset the nRF52840 and let it wait until the pin is
	 * pulled low again before running to main to ensure
	 * that it won't send any data until the DEV device
	 * is setup and ready to receive.
	 */
	err = gpio_pin_set(port, RESET_PIN, 1);
	if (err) {
		return err;
	}

	/* Wait for the nRF52840 peripheral to stop sending data.
	 *
	 * It is critical (!) to wait here, so that all bytes
	 * on the lines are received and drained correctly.
	 */
	k_sleep(K_MSEC(10));

	/* Drain bytes */
	while (uart_fifo_read(dev, &c, 1)) {
		continue;
	}

	/* We are ready, let the nRF52840 run to main */
	err = gpio_pin_set(port, RESET_PIN, 0);
	if (err) {
		return err;
	}

	return 0;
}
