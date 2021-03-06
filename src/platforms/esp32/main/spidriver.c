/***************************************************************************
 *   Copyright 2019 by Davide Bettio <davide@uninstall.it>                 *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU Lesser General Public License as        *
 *   published by the Free Software Foundation; either version 2 of the    *
 *   License, or (at your option) any later version.                       *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program; if not, write to the                         *
 *   Free Software Foundation, Inc.,                                       *
 *   51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA .        *
 ***************************************************************************/

#include "spidriver.h"

#include <string.h>

#include <driver/spi_master.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "atom.h"
#include "bif.h"
#include "context.h"
#include "debug.h"
#include "defaultatoms.h"
#include "platform_defaultatoms.h"
#include "globalcontext.h"
#include "interop.h"
#include "mailbox.h"
#include "module.h"
#include "utils.h"
#include "term.h"

#include "trace.h"

#include "sys.h"
#include "esp32_sys.h"

static void spidriver_consume_mailbox(Context *ctx);

static uint8_t spidriver_transfer_at(Context *ctx, uint8_t address, uint8_t data);

struct SPIData
{
    spi_device_handle_t handle;
    spi_transaction_t transaction;
};

void spidriver_init(Context *ctx, term opts)
{
    struct SPIData *spi_data = calloc(1, sizeof(struct SPIData));

    ctx->native_handler = spidriver_consume_mailbox;
    ctx->platform_data = spi_data;

    term bus_config = interop_proplist_get_value(opts, BUS_CONFIG_ATOM);
    term miso_io_num_term = interop_proplist_get_value(bus_config, MISO_IO_NUM_ATOM);
    term mosi_io_num_term = interop_proplist_get_value(bus_config, MOSI_IO_NUM_ATOM);
    term sclk_io_num_term = interop_proplist_get_value(bus_config, SCLK_IO_NUM_ATOM);

    spi_bus_config_t buscfg;
    memset(&buscfg, 0, sizeof(spi_bus_config_t));
    buscfg.miso_io_num = term_to_int32(miso_io_num_term);
    buscfg.mosi_io_num = term_to_int32(mosi_io_num_term);
    buscfg.sclk_io_num = term_to_int32(sclk_io_num_term);
    buscfg.quadwp_io_num = -1;
    buscfg.quadhd_io_num = -1;

    term device_config = interop_proplist_get_value(opts, DEVICE_CONFIG_ATOM);
    term clock_speed_hz_term = interop_proplist_get_value(device_config, SPI_CLOCK_HZ_ATOM);
    term mode_term = interop_proplist_get_value(device_config, SPI_MODE_ATOM);
    term spics_io_num_term = interop_proplist_get_value(device_config, SPI_CS_IO_NUM_ATOM);
    term address_bits_term = interop_proplist_get_value(device_config, ADDRESS_LEN_BITS_ATOM);

    spi_device_interface_config_t devcfg;
    memset(&devcfg, 0, sizeof(spi_device_interface_config_t));
    devcfg.clock_speed_hz = term_to_int32(clock_speed_hz_term);
    devcfg.mode = term_to_int32(mode_term);
    devcfg.spics_io_num = term_to_int32(spics_io_num_term);
    devcfg.queue_size = 4;
    devcfg.address_bits = term_to_int32(address_bits_term);

    int ret = spi_bus_initialize(HSPI_HOST, &buscfg, 1);

    if (ret == ESP_OK) {
        TRACE("initialized SPI\n");
    } else {
        TRACE("spi_bus_initialize return code: %i\n", ret);
    }

    ret = spi_bus_add_device(HSPI_HOST, &devcfg, &spi_data->handle);

    if (ret == ESP_OK) {
        TRACE("initialized SPI device\n");
    } else {
        TRACE("spi_bus_add_device return code: %i\n", ret);
    }
}

static uint8_t spidriver_transfer_at(Context *ctx, uint8_t address, uint8_t data)
{
    TRACE("--- SPI transfer ---\n");
    TRACE("spi: address: %x, tx: %x\n", (int) address, (int) data);

    struct SPIData *spi_data = ctx->platform_data;

    memset(&spi_data->transaction, 0, sizeof(spi_transaction_t));

    spi_data->transaction.flags = SPI_TRANS_USE_TXDATA | SPI_TRANS_USE_RXDATA;
    spi_data->transaction.length = 8;
    spi_data->transaction.addr = address;
    spi_data->transaction.tx_data[0] = data;

    //TODO: int ret = spi_device_queue_trans(spi_data->handle, &spi_data->transaction, portMAX_DELAY);
    int ret = spi_device_polling_transmit(spi_data->handle, &spi_data->transaction);

    //TODO check return code

    uint8_t rx_data = spi_data->transaction.rx_data[0];

    TRACE("spi: ret: %x\n", (int) ret);
    TRACE("spi: rx: %x\n", (int) rx_data);
    TRACE("--- end of transfer ---\n");

    return rx_data;
}

static term spidriver_read_at(Context *ctx, term req)
{
    //cmd is at index 0
    term address_term = term_get_tuple_element(req, 1);
    term len_term = term_get_tuple_element(req, 2);

    uint8_t address = term_to_int32(address_term);
    UNUSED(len_term);

    if (UNLIKELY(memory_ensure_free(ctx, 1 + 2) != MEMORY_GC_OK)) {
        return ERROR_ATOM;
    }
    term result_tuple = term_alloc_tuple(2, ctx);

    uint8_t read_value = spidriver_transfer_at(ctx, address, 0);
    term_put_tuple_element(result_tuple, 0, OK_ATOM);
    term_put_tuple_element(result_tuple, 1, term_from_int11(read_value));

    return result_tuple;
}

static term spidriver_write_at(Context *ctx, term req)
{
    //cmd is at index 0
    term address_term = term_get_tuple_element(req, 1);
    term len_term = term_get_tuple_element(req, 2);
    term data_term = term_get_tuple_element(req, 3);

    uint8_t address = term_to_int32(address_term);
    UNUSED(len_term);
    uint8_t data = term_to_int32(data_term);

    if (UNLIKELY(memory_ensure_free(ctx, 1 + 2) != MEMORY_GC_OK)) {
        return ERROR_ATOM;
    }
    term result_tuple = term_alloc_tuple(2, ctx);

    uint8_t read_value = spidriver_transfer_at(ctx, address, data);
    term_put_tuple_element(result_tuple, 0, OK_ATOM);
    term_put_tuple_element(result_tuple, 1, term_from_int11(read_value));

    return result_tuple;
}

static void spidriver_consume_mailbox(Context *ctx)
{
    Message *message = mailbox_dequeue(ctx);
    term msg = message->message;
    term pid = term_get_tuple_element(msg, 0);
    term ref = term_get_tuple_element(msg, 1);
    term req = term_get_tuple_element(msg, 2);

    term cmd = term_get_tuple_element(req, 0);

    int local_process_id = term_to_local_process_id(pid);
    Context *target = globalcontext_get_process(ctx->global, local_process_id);

    term ret;

    switch (cmd) {
        case READ_AT_ATOM:
            TRACE("spi: read at.\n");
            ret = spidriver_read_at(ctx, req);
            break;

        case WRITE_AT_ATOM:
            TRACE("spi: write at.\n");
            ret = spidriver_write_at(ctx, req);
            break;

        default:
            TRACE("spi: error: unrecognized command.\n");
            ret = ERROR_ATOM;
    }

    free(message);

    UNUSED(ref);
    mailbox_send(target, ret);
}
