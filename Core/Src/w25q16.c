#include "w25q16.h"
#include "cmsis_os2.h"
#include <string.h>

/* One lock for the chip: the config copies, flash logs, the engine sequence
 * sector and OTA all share it from different tasks. Recursive, and skipped
 * before the scheduler runs or from an interrupt. */
static osMutexId_t s_w25q_mutex;

static uint8_t w25q_lock(void)
{
    if (osKernelGetState() != osKernelRunning || __get_IPSR() != 0U) return 0U;
    if (!s_w25q_mutex) {
        static const osMutexAttr_t attr = { .name = "w25q", .attr_bits = osMutexRecursive | osMutexPrioInherit };
        s_w25q_mutex = osMutexNew(&attr);
        if (!s_w25q_mutex) return 0U;
    }
    return (osMutexAcquire(s_w25q_mutex, osWaitForever) == osOK) ? 1U : 0U;
}

static void w25q_unlock(uint8_t locked)
{
    if (locked) (void)osMutexRelease(s_w25q_mutex);
}

/* ===================== COMMANDS ===================== */
#define CMD_JEDEC_ID        0x9F
#define CMD_READ_DATA       0x03
#define CMD_WRITE_ENABLE    0x06
#define CMD_PAGE_PROGRAM    0x02
#define CMD_READ_SR1        0x05
#define CMD_SECTOR_ERASE_4K 0x20

#define SR1_BUSY            (1U << 0)

/* ===================== LOW LEVEL ===================== */
static inline void cs_low(W25Q_Handle *d) {
    HAL_GPIO_WritePin(d->cs_port, d->cs_pin, GPIO_PIN_RESET);
}
static inline void cs_high(W25Q_Handle *d) {
    HAL_GPIO_WritePin(d->cs_port, d->cs_pin, GPIO_PIN_SET);
}

static bool spi_tx(W25Q_Handle *d, const uint8_t *buf, uint16_t len) {
    return (HAL_SPI_Transmit(d->hspi, (uint8_t *)buf, len, 1000) == HAL_OK);
}
static bool spi_rx(W25Q_Handle *d, uint8_t *buf, uint16_t len) {
    return (HAL_SPI_Receive(d->hspi, buf, len, 1000) == HAL_OK);
}

/* ===================== STATUS ===================== */
bool w25q_is_busy(W25Q_Handle *d) {
    uint8_t cmd = CMD_READ_SR1;
    uint8_t sr  = 0;

    cs_low(d);
    spi_tx(d, &cmd, 1);
    spi_rx(d, &sr, 1);
    cs_high(d);

    return (sr & SR1_BUSY);
}

bool w25q_wait_ready(W25Q_Handle *d, uint32_t timeout_ms) {
    uint32_t start = HAL_GetTick();
    while (w25q_is_busy(d)) {
        if ((HAL_GetTick() - start) > timeout_ms)
            return false;
        HAL_Delay(1);
    }
    return true;
}

/* ===================== CORE ===================== */
static bool write_enable(W25Q_Handle *d) {
    uint8_t cmd = CMD_WRITE_ENABLE;
    cs_low(d);
    bool ok = spi_tx(d, &cmd, 1);
    cs_high(d);
    return ok;
}

uint32_t w25q_read_jedec_id(W25Q_Handle *d) {
    uint8_t cmd = CMD_JEDEC_ID;
    uint8_t id[3] = {0};

    cs_low(d);
    spi_tx(d, &cmd, 1);
    spi_rx(d, id, 3);
    cs_high(d);

    return ((uint32_t)id[0] << 16) |
           ((uint32_t)id[1] << 8)  |
           ((uint32_t)id[2]);
}

bool w25q_init(W25Q_Handle *d) {
    cs_high(d);
    HAL_Delay(2);

    uint32_t id = w25q_read_jedec_id(d);

    /* Expected for W25Q16JV: EF 40 15 */
    if (id == 0 || id == 0xFFFFFF)
        return false;

    return true;
}

/* ===================== READ ===================== */
static bool w25q_read_unlocked(W25Q_Handle *d, uint32_t addr, uint8_t *buf, uint32_t len);

bool w25q_read(W25Q_Handle *d, uint32_t addr, uint8_t *buf, uint32_t len) {
    uint8_t locked = w25q_lock();
    bool ok = w25q_read_unlocked(d, addr, buf, len);
    w25q_unlock(locked);
    return ok;
}

static bool w25q_read_unlocked(W25Q_Handle *d, uint32_t addr, uint8_t *buf, uint32_t len) {
    uint8_t hdr[4];

    hdr[0] = CMD_READ_DATA;
    hdr[1] = (addr >> 16) & 0xFF;
    hdr[2] = (addr >> 8)  & 0xFF;
    hdr[3] = addr & 0xFF;

    cs_low(d);
    if (!spi_tx(d, hdr, 4)) { cs_high(d); return false; }
    bool ok = spi_rx(d, buf, len);
    cs_high(d);

    return ok;
}

/* ===================== WRITE ===================== */
static bool page_program(W25Q_Handle *d, uint32_t addr,
                         const uint8_t *buf, uint32_t len)
{
    if (len == 0 || len > W25Q_PAGE_SIZE)
        return false;

    if ((addr % W25Q_PAGE_SIZE) + len > W25Q_PAGE_SIZE)
        return false;

    if (!write_enable(d))
        return false;

    uint8_t hdr[4];
    hdr[0] = CMD_PAGE_PROGRAM;
    hdr[1] = (addr >> 16) & 0xFF;
    hdr[2] = (addr >> 8)  & 0xFF;
    hdr[3] = addr & 0xFF;

    cs_low(d);
    if (!spi_tx(d, hdr, 4)) { cs_high(d); return false; }
    if (!spi_tx(d, buf, len)) { cs_high(d); return false; }
    cs_high(d);

    return w25q_wait_ready(d, 5000);
}

bool w25q_write(W25Q_Handle *d, uint32_t addr,
                const uint8_t *buf, uint32_t len)
{
    uint8_t locked = w25q_lock();
    bool ok = true;
    while (len) {
        uint32_t page_off = addr % W25Q_PAGE_SIZE;
        uint32_t chunk = W25Q_PAGE_SIZE - page_off;
        if (chunk > len)
            chunk = len;

        if (!page_program(d, addr, buf, chunk)) {
            ok = false;
            break;
        }

        addr += chunk;
        buf  += chunk;
        len  -= chunk;
    }
    w25q_unlock(locked);
    return ok;
}

/* ===================== ERASE ===================== */
static bool w25q_erase_unlocked(W25Q_Handle *d, uint32_t addr);

bool w25q_erase_sector_4k(W25Q_Handle *d, uint32_t addr) {
    uint8_t locked = w25q_lock();
    bool ok = w25q_erase_unlocked(d, addr);
    w25q_unlock(locked);
    return ok;
}

static bool w25q_erase_unlocked(W25Q_Handle *d, uint32_t addr) {
    addr &= ~(W25Q_SECTOR_SIZE - 1);

    if (!write_enable(d))
        return false;

    uint8_t cmd[4];
    cmd[0] = CMD_SECTOR_ERASE_4K;
    cmd[1] = (addr >> 16) & 0xFF;
    cmd[2] = (addr >> 8)  & 0xFF;
    cmd[3] = addr & 0xFF;

    cs_low(d);
    bool ok = spi_tx(d, cmd, 4);
    cs_high(d);

    if (!ok)
        return false;

    return w25q_wait_ready(d, 10000);
}
