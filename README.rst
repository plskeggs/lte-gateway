.. _nrfcloud_ble_gateway:

nRF9160: nRF Cloud BLE Gateway
##############################

The nRF Cloud BLE Gateway uses the :ref:`lib_nrf_cloud` to connect an nRF9160-based board to `nRF Cloud`_ via LTE, connnect to multiple Bluetooth LE peripherals, and transmit their data to the cloud.
Therefore, this application acts as a gateway between Bluetooth LE and the LTE connection to nRF Cloud.

Overview
********

The application uses the LTE link control driver to establish a network connection.
It is then able to connect to multiple Bluetooth LE peripherals, and transmits the peripheral data to Nordic Semiconductor's cloud solution, `nRF Cloud`_.
The data is visualized in nRF Cloud's web interface.

The `LTE Link Monitor`_ application, implemented as part of `nRF Connect for Desktop`_  can be used to interact with the included shell.
You can also send AT commands from the **Terminal** card on nRF Cloud when the device is connected.

By default, the gateway supports firmware updates through :ref:`lib_nrf_cloud_fota`.

.. _nrfcloud_ble_gateway_requirements:

Requirements
************

* One of the following boards:

  * | Apricity Gateway |
  * | :ref:`nRF9160 DK <ug_nrf9160>` |

* For the Apricity Gateway nRF9160, :ref:`lte-gateway-ble` must be programmed in the nRF52 board controller.
* For the nRF9160 DK, :ref:`hci_lpuart` must instead be programmed in the nRF52 board controller.
* The sample is configured to compile and run as a non-secure application on nRF91's Cortex-M33. Therefore, it automatically includes the :ref:`secure_partition_manager` that prepares the required peripherals to be available for the application.


.. _nrfcloud_ble_gateway_user_interface:

User interface
**************

The Apricity Gateway button has the following functions:

Button:
    * Reset device when held for 7 seconds.
    * Power off device when held for more than one second and released before reset.

The application state is indicated by the LEDs.

.. _apricty_gateway_operating_states:


.. list-table::
   :header-rows: 1
   :align: center

   * - LTE LED 1 color
     - State
   * - Off
     - Not connected to LTE carrier
   * - Slow White Pulse
     - Connecting to LTE carrier
   * - Slow Yellow Pulse
     - Associating with nRF Cloud
   * - Slow Cyan Pulse
     - Connecting to nRF Cloud
   * - Solid Blue
     - Connected, ready for BLE connections
   * - Red Pulse
     - Error

   * - BLE LED 2 color
     - State
   * - Slow Purple Pulse
     - Button being held; continue to hold to enter nRF52840 USB MCUboot update mode
   * - Rapid Purple Pulse
     - in nRF52840 USB MCUboot update mode
   * - Slow Yellow Pulse
     - Waiting for Bluetooth LE device to connect
   * - Solid White
     - Bluetooth LE connection established

Building and running
********************

In order to Flash the first firmware image to the Apricity Gateway, you will need either an nRF9160 DK with VDDIO set to 3V, a 10 pin ribbon connected to Debug out, and an adapter to a 6 pin Tag Connect connector; or a Segger J-Link with an adapter to the same Tag Connect.  Connect the Tag Connect to NRF91:J1 on the PCB.  For programming to the nRF9160 DK, set PROG/DEBUG to nRF91.

:file:`lte-gateway`

1. Checkout this repository.
#. Execute the following to pull down all other required repositories:

      west update 
 
#. Execute the following to build for the Apricity Gateway hardware:

      west build -d build -b apricity_gateway_nrf9160ns

#. Or execute this, to build for the nRF9160 DK:

      west build -d build_dk -b nrf9160dk_nrf9160_ns

#. Flash to either board:

      west flash -d <build dir> --erase --force


:ref:`lte-gateway-ble`

For the Apricity Gateway hardware, follow the same instructions as above in the folder for its repository, except use apricity_gateway_nrf52840 instead of apricity_gateway_nrf9160ns, and connect the Tag Connect to NRF52:J1.

Testing
=======

After programming the application and all prerequisites to your board, test the Apricity Gateway application by performing the following steps:

1. Connect the board to the computer using a USB cable.
   The board is assigned a COM port (Windows) or ttyACM or ttyS device (Linux).
#. Connect to the board with a terminal emulator, for example, PuTTY, Tera Term, or LTE Link Monitor.  Turn off local echo.  The shell uses VT100-compatible escape sequences for coloration.
#. Reset the board.
#. Observe in the terminal window that the board starts up in the Secure Partition Manager and that the application starts.
   This is indicated by output similar to the following lines::

      *** Booting Zephyr OS build v2.6.99-ncs1-rc2-5-ga64e96d17cc7  ***
      
      SPM: prepare to jump to Non-Secure image.

      login:

#. For PuTTY or LTE Link Monitor, reconnect terminal. (Bluetooth LE HCI control resets the terminal output and needs to be reconnected).  Tera Term automatically reconnects.
#. Login with the default password:

      nordic

#. If you wish to see logging messages other than ERROR, such as INFO, execute:

      log enable inf

#. Open a web browser and navigate to https://nrfcloud.com/.  Click on Device Management then Gateways.  Click on your device's Device ID (UUID), which takes you to the detailed view of your gateway.
#. The first time you start the application, the device will be added to your account automatically.

   a. Observe that the LED(s) indicate that the device is connected.
   #. If the LED(s) indicate an error, check the details of the error in the terminal window.

#. Add BLE devices by clicking on the + sign.  Read, write, and enable notifications on connected peripheral and observe data being received on the nRF Cloud. 
#. Optionally send AT commands from the terminal, and observe that the response is received.


Dependencies
************

This application uses the following |NCS| libraries and drivers:

* :ref:`lib_nrf_cloud`
* :ref:`modem_info_readme`
* :ref:`at_cmd_parser_readme`
* ``lib/modem_lib``
* :ref:`dk_buttons_and_leds_readme`
* ``drivers/lte_link_control``
* ``drivers/flash``
* ``bluetooth/gatt_dm``
* ``bluetooth/scan``

From Zephyr:
  * :ref:`zephyr:bluetooth_api`

In addition, it uses the Secure Partition Manager sample:

* :ref:`secure_partition_manager`

For nrf52840
* :ref:`lte-gateway-ble`
* :ref:`hci_lpuart`

History
************

The Apricity Gateway application was created using the following |NCS| sample applications:

  * :ref:`lte_ble_gateway`
  * :ref:`asset_tracker`

From Zephyr:
  * :ref:`zephyr:bluetooth-hci-uart-sample`