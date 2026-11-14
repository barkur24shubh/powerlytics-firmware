#include "stm32f407xx.h"
#include "sha256.h"
#include "app_ota_manifest.h"
#include "monocypher-ed25519.h"
#include "ota_signature.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifndef OTA_SIGNING_PUBLIC_KEY_HEX
#define OTA_SIGNING_PUBLIC_KEY_HEX ""
#endif
#ifndef OTA_DEVICE_MODEL
#define OTA_DEVICE_MODEL ""
#endif

#define BOOT_APP_ADDR          APP_OTA_APPLICATION_ADDR
#define BOOT_FLASH_END_ADDR    APP_OTA_FLASH_END_ADDR
#define BOOT_CHUNK_SIZE        512U
#define BOOT_SPI_WAIT_LOOPS    1000000UL
#define BOOT_W25Q_BUSY_LOOPS   8000000UL
#define BOOT_FLASH_WAIT_LOOPS  16000000UL

#define W25Q_CMD_READ_DATA     0x03U
#define W25Q_CMD_JEDEC_ID      0x9FU
#define W25Q_CMD_WRITE_ENABLE  0x06U
#define W25Q_CMD_PAGE_PROGRAM  0x02U
#define W25Q_CMD_SECTOR_ERASE  0x20U
#define W25Q_CMD_READ_SR1      0x05U
#define W25Q_SR1_BUSY          0x01U

#define FLASH_KEY1             0x45670123UL
#define FLASH_KEY2             0xCDEF89ABUL

#define FLASH_CR_SNB_POS       3U
#define FLASH_CR_PSIZE_MASK    (3UL << 8U)
#define FLASH_SR_CLEAR_MASK    (FLASH_SR_EOP | FLASH_SR_OPERR | FLASH_SR_WRPERR | \
                                FLASH_SR_PGAERR | FLASH_SR_PGPERR | FLASH_SR_PGSERR)

static uint8_t s_buf[BOOT_CHUNK_SIZE];

static void led_set(uint8_t on);

static void boot_uart_init(void)
{
    RCC->AHB1ENR |= RCC_AHB1ENR_GPIOBEN;
    RCC->APB2ENR |= RCC_APB2ENR_USART1EN;
    __DSB();

    GPIOB->MODER &= ~((3UL << (6U * 2U)) | (3UL << (7U * 2U)));
    GPIOB->MODER |=  ((2UL << (6U * 2U)) | (2UL << (7U * 2U)));
    GPIOB->OSPEEDR |= ((3UL << (6U * 2U)) | (3UL << (7U * 2U)));
    GPIOB->PUPDR &= ~((3UL << (6U * 2U)) | (3UL << (7U * 2U)));
    GPIOB->AFR[0] &= ~((0xFUL << (6U * 4U)) | (0xFUL << (7U * 4U)));
    GPIOB->AFR[0] |=  ((7UL << (6U * 4U)) | (7UL << (7U * 4U)));

    USART1->CR1 = 0U;
    USART1->CR2 = 0U;
    USART1->CR3 = 0U;
    USART1->BRR = 0x008BUL;
    USART1->CR1 = USART_CR1_TE | USART_CR1_RE | USART_CR1_UE;
}

static void boot_uart_puts(const char *s)
{
    if (!s) {
        return;
    }

    while (*s) {
        uint32_t loops = BOOT_SPI_WAIT_LOOPS;
        while ((USART1->SR & USART_SR_TXE) == 0U) {
            if (loops-- == 0U) {
                return;
            }
        }
        USART1->DR = (uint8_t)*s++;
    }
}

static void iwdg_refresh(void)
{
    IWDG->KR = 0xAAAAU;
}

static uint32_t crc32_sw(const void *data, uint32_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    uint32_t crc = 0xFFFFFFFFUL;

    for (uint32_t i = 0; i < len; i++) {
        crc ^= p[i];
        for (uint8_t b = 0; b < 8U; b++) {
            uint32_t mask = (uint32_t)(-(int32_t)(crc & 1UL));
            crc = (crc >> 1) ^ (0xEDB88320UL & mask);
        }
    }

    return ~crc;
}

static void delay_cycles(volatile uint32_t cycles)
{
    while (cycles--) {
        __NOP();
    }
}

static void led_init(void)
{
    RCC->AHB1ENR |= RCC_AHB1ENR_GPIODEN;
    __DSB();
    GPIOD->MODER &= ~(3UL << (3U * 2U));
    GPIOD->MODER |=  (1UL << (3U * 2U));
    led_set(0U);
}

static void led_set(uint8_t on)
{
    /*
     * PD3 debug LED is active-low on this board, matching app_debug_led.c.
     */
    if (on) {
        GPIOD->BSRR = (1UL << (3U + 16U));
    } else {
        GPIOD->BSRR = (1UL << 3U);
    }
}

static void led_error_loop(void)
{
    for (;;) {
        iwdg_refresh();
        led_set(1U);
        delay_cycles(600000UL);
        iwdg_refresh();
        led_set(0U);
        delay_cycles(600000UL);
    }
}

static uint8_t wait_reg_set(volatile uint32_t *reg, uint32_t mask, uint32_t loops)
{
    while (((*reg) & mask) == 0U) {
        if (loops-- == 0U) {
            return 0U;
        }
    }

    return 1U;
}

static uint8_t spi1_xfer(uint8_t tx, uint8_t *rx)
{
    if (!wait_reg_set(&SPI1->SR, SPI_SR_TXE, BOOT_SPI_WAIT_LOOPS)) {
        return 0U;
    }

    *(__IO uint8_t *)&SPI1->DR = tx;

    if (!wait_reg_set(&SPI1->SR, SPI_SR_RXNE, BOOT_SPI_WAIT_LOOPS)) {
        return 0U;
    }

    if (rx) {
        *rx = *(__IO uint8_t *)&SPI1->DR;
    } else {
        (void)*(__IO uint8_t *)&SPI1->DR;
    }

    return 1U;
}

static void w25q_cs(uint8_t high)
{
    if (high) {
        GPIOB->BSRR = (1UL << 0U);
    } else {
        GPIOB->BSRR = (1UL << (0U + 16U));
    }
}

static void w25q_init_gpio_spi(void)
{
    RCC->AHB1ENR |= RCC_AHB1ENR_GPIOBEN;
    RCC->APB2ENR |= RCC_APB2ENR_SPI1EN;
    __DSB();

    GPIOB->BSRR = (1UL << 0U);

    GPIOB->MODER &= ~((3UL << (0U * 2U)) |
                      (3UL << (3U * 2U)) |
                      (3UL << (4U * 2U)) |
                      (3UL << (5U * 2U)));
    GPIOB->MODER |=  ((1UL << (0U * 2U)) |
                      (2UL << (3U * 2U)) |
                      (2UL << (4U * 2U)) |
                      (2UL << (5U * 2U)));

    GPIOB->OSPEEDR |= ((3UL << (0U * 2U)) |
                       (3UL << (3U * 2U)) |
                       (3UL << (4U * 2U)) |
                       (3UL << (5U * 2U)));

    GPIOB->PUPDR &= ~((3UL << (3U * 2U)) |
                      (3UL << (4U * 2U)) |
                      (3UL << (5U * 2U)));
    GPIOB->PUPDR |=  (1UL << (4U * 2U));

    GPIOB->AFR[0] &= ~((0xFUL << (3U * 4U)) |
                       (0xFUL << (4U * 4U)) |
                       (0xFUL << (5U * 4U)));
    GPIOB->AFR[0] |=  ((5UL << (3U * 4U)) |
                       (5UL << (4U * 4U)) |
                       (5UL << (5U * 4U)));

    SPI1->CR1 = SPI_CR1_MSTR | SPI_CR1_SSM | SPI_CR1_SSI |
                SPI_CR1_BR_1 | SPI_CR1_BR_0;
    SPI1->CR2 = 0U;
    SPI1->CR1 |= SPI_CR1_SPE;
}

static uint32_t w25q_read_jedec(void)
{
    uint8_t id0 = 0U;
    uint8_t id1 = 0U;
    uint8_t id2 = 0U;
    uint8_t ok = 1U;

    w25q_cs(0U);
    ok &= spi1_xfer(W25Q_CMD_JEDEC_ID, NULL);
    ok &= spi1_xfer(0xFFU, &id0);
    ok &= spi1_xfer(0xFFU, &id1);
    ok &= spi1_xfer(0xFFU, &id2);
    w25q_cs(1U);

    if (!ok) {
        return 0U;
    }

    return ((uint32_t)id0 << 16U) | ((uint32_t)id1 << 8U) | id2;
}

static uint8_t w25q_read(uint32_t addr, uint8_t *buf, uint32_t len)
{
    w25q_cs(0U);
    if (!spi1_xfer(W25Q_CMD_READ_DATA, NULL) ||
        !spi1_xfer((uint8_t)(addr >> 16U), NULL) ||
        !spi1_xfer((uint8_t)(addr >> 8U), NULL) ||
        !spi1_xfer((uint8_t)addr, NULL)) {
        w25q_cs(1U);
        return 0U;
    }

    for (uint32_t i = 0U; i < len; i++) {
        if (!spi1_xfer(0xFFU, &buf[i])) {
            w25q_cs(1U);
            return 0U;
        }
    }

    w25q_cs(1U);
    return 1U;
}

static uint8_t app_is_valid(uint32_t app_addr)
{
    uint32_t sp = *(volatile uint32_t *)app_addr;
    uint32_t pc = *(volatile uint32_t *)(app_addr + 4U);

    uint8_t sp_ok = (sp >= 0x20000000UL && sp <= 0x20020000UL) ? 1U : 0U;
    uint8_t pc_ok = (pc >= app_addr && pc < BOOT_FLASH_END_ADDR && (pc & 1UL)) ? 1U : 0U;

    return (sp_ok && pc_ok) ? 1U : 0U;
}

static void jump_to_app(void)
{
    uint32_t sp = *(volatile uint32_t *)BOOT_APP_ADDR;
    uint32_t pc = *(volatile uint32_t *)(BOOT_APP_ADDR + 4U);
    typedef void (*app_entry_t)(void);

    __disable_irq();
    SysTick->CTRL = 0U;
    SysTick->LOAD = 0U;
    SysTick->VAL = 0U;

    for (uint32_t i = 0U; i < 8U; i++) {
        NVIC->ICER[i] = 0xFFFFFFFFUL;
        NVIC->ICPR[i] = 0xFFFFFFFFUL;
    }

    SCB->VTOR = BOOT_APP_ADDR;
    __set_MSP(sp);
    __set_CONTROL(0U);
    __set_BASEPRI(0U);
    __set_FAULTMASK(0U);
    __DSB();
    __ISB();
    __enable_irq();

    ((app_entry_t)pc)();

    for (;;) {
    }
}

static uint8_t manifest_load(app_ota_manifest_t *m)
{
    if (!m) {
        return 0U;
    }

    if (!w25q_read(APP_OTA_MANIFEST_W25Q_ADDR, (uint8_t *)m, sizeof(*m))) {
        return 0U;
    }

    if (m->magic != APP_OTA_MANIFEST_MAGIC ||
        m->version != APP_OTA_MANIFEST_VERSION ||
        m->header_size != sizeof(*m) ||
        m->state != APP_OTA_STATE_PENDING ||
        m->image_addr != APP_OTA_STAGING_W25Q_ADDR ||
        m->app_addr != APP_OTA_APPLICATION_ADDR ||
        m->app_max_size != APP_OTA_APPLICATION_SIZE ||
        m->image_size == 0U ||
        m->image_size > APP_OTA_APPLICATION_SIZE) {
        return 0U;
    }

    if (m->install_result != APP_OTA_RESULT_NONE &&
        m->install_result != APP_OTA_RESULT_INSTALLED &&
        m->install_result != APP_OTA_RESULT_FAILED &&
        m->install_result != APP_OTA_RESULT_ROLLED_BACK) {
        return 0U;
    }

    return (crc32_sw(m, APP_OTA_MANIFEST_CRC_LEN) == m->crc32) ? 1U : 0U;
}

static void w25q_write_enable(void)
{
    w25q_cs(0U);
    (void)spi1_xfer(W25Q_CMD_WRITE_ENABLE, NULL);
    w25q_cs(1U);
}

static uint8_t w25q_busy(void)
{
    uint8_t sr = 0U;

    w25q_cs(0U);
    if (!spi1_xfer(W25Q_CMD_READ_SR1, NULL) ||
        !spi1_xfer(0xFFU, &sr)) {
        w25q_cs(1U);
        return 1U;
    }
    w25q_cs(1U);

    return (sr & W25Q_SR1_BUSY) ? 1U : 0U;
}

static uint8_t w25q_wait_ready(void)
{
    uint32_t loops = BOOT_W25Q_BUSY_LOOPS;

    while (w25q_busy()) {
        iwdg_refresh();
        if (loops-- == 0U) {
            return 0U;
        }
    }

    return 1U;
}

static uint8_t w25q_program_bytes(uint32_t addr, const uint8_t *data, uint32_t len)
{
    if (!data || len == 0U || len > 256U || ((addr & 0xFFU) + len) > 256U) {
        return 0U;
    }

    w25q_write_enable();
    w25q_cs(0U);
    if (!spi1_xfer(W25Q_CMD_PAGE_PROGRAM, NULL) ||
        !spi1_xfer((uint8_t)(addr >> 16U), NULL) ||
        !spi1_xfer((uint8_t)(addr >> 8U), NULL) ||
        !spi1_xfer((uint8_t)addr, NULL)) {
        w25q_cs(1U);
        return 0U;
    }

    for (uint32_t i = 0U; i < len; i++) {
        if (!spi1_xfer(data[i], NULL)) {
            w25q_cs(1U);
            return 0U;
        }
    }

    w25q_cs(1U);
    return w25q_wait_ready();
}

static uint8_t manifest_write_result(uint32_t result, uint32_t error)
{
    uint32_t words[2];
    uint32_t addr = APP_OTA_MANIFEST_W25Q_ADDR +
                    (uint32_t)offsetof(app_ota_manifest_t, install_result);

    words[0] = result;
    words[1] = error;

    return w25q_program_bytes(addr, (const uint8_t *)words, sizeof(words));
}

static uint8_t manifest_write_word(size_t field_offset, uint32_t value)
{
    uint32_t addr = APP_OTA_MANIFEST_W25Q_ADDR + (uint32_t)field_offset;
    return w25q_program_bytes(addr, (const uint8_t *)&value, sizeof(value));
}

static uint8_t sha256_w25q(uint32_t addr, uint32_t len, uint8_t hash[32]);

static uint8_t w25q_erase_4k(uint32_t addr)
{
    if ((addr & 0xFFFU) != 0U || addr >= APP_OTA_W25Q_TOTAL_SIZE) return 0U;
    w25q_write_enable();
    w25q_cs(0U);
    if (!spi1_xfer(W25Q_CMD_SECTOR_ERASE, NULL) ||
        !spi1_xfer((uint8_t)(addr >> 16U), NULL) ||
        !spi1_xfer((uint8_t)(addr >> 8U), NULL) ||
        !spi1_xfer((uint8_t)addr, NULL)) {
        w25q_cs(1U);
        return 0U;
    }
    w25q_cs(1U);
    return w25q_wait_ready();
}

static uint8_t copy_internal_to_backup(const app_ota_manifest_t *m)
{
    uint32_t done = 0U;
    uint8_t hash[32];
    if (!m) return 0U;
    for (uint32_t addr = APP_OTA_BACKUP_W25Q_ADDR;
         addr < APP_OTA_BACKUP_W25Q_ADDR + APP_OTA_SLOT_SIZE;
         addr += 4096U) {
        iwdg_refresh();
        if (!w25q_erase_4k(addr)) return 0U;
    }
    while (done < APP_OTA_SLOT_SIZE) {
        uint32_t chunk = APP_OTA_SLOT_SIZE - done;
        if (chunk > sizeof(s_buf)) chunk = sizeof(s_buf);
        memcpy(s_buf, (const void *)(APP_OTA_APPLICATION_ADDR + done), chunk);
        for (uint32_t offset = 0U; offset < chunk; offset += 256U) {
            uint32_t page = chunk - offset;
            if (page > 256U) page = 256U;
            if (!w25q_program_bytes(APP_OTA_BACKUP_W25Q_ADDR + done + offset,
                                    &s_buf[offset], page)) return 0U;
        }
        done += chunk;
        iwdg_refresh();
    }
    if (!sha256_w25q(APP_OTA_BACKUP_W25Q_ADDR, APP_OTA_SLOT_SIZE, hash) ||
        memcmp(hash, m->rollback_sha256, sizeof(hash)) != 0) return 0U;
    return 1U;
}

static uint8_t sha256_w25q(uint32_t addr,
                           uint32_t len,
                           uint8_t hash[32])
{
    sha256_ctx_t sha;
    uint32_t done = 0U;

    sha256_init(&sha);

    while (done < len) {
        uint32_t chunk = len - done;
        if (chunk > sizeof(s_buf)) {
            chunk = sizeof(s_buf);
        }

        iwdg_refresh();
        if (!w25q_read(addr + done, s_buf, chunk)) {
            return 0U;
        }
        sha256_update(&sha, s_buf, chunk);
        done += chunk;
    }

    sha256_final(&sha, hash);
    return 1U;
}

static uint8_t sha256_internal_flash(uint32_t addr,
                                     uint32_t len,
                                     uint8_t hash[32])
{
    sha256_ctx_t sha;
    uint32_t done = 0U;

    sha256_init(&sha);

    while (done < len) {
        uint32_t chunk = len - done;
        if (chunk > sizeof(s_buf)) {
            chunk = sizeof(s_buf);
        }

        iwdg_refresh();
        memcpy(s_buf, (const void *)(addr + done), chunk);
        sha256_update(&sha, s_buf, chunk);
        done += chunk;
    }

    sha256_final(&sha, hash);
    return 1U;
}

static int8_t hex_nibble(char c)
{
    if (c >= '0' && c <= '9') return (int8_t)(c - '0');
    if (c >= 'a' && c <= 'f') return (int8_t)(c - 'a' + 10);
    if (c >= 'A' && c <= 'F') return (int8_t)(c - 'A' + 10);
    return -1;
}

static uint8_t hex_to_bytes(const char *hex, uint8_t *out, uint32_t bytes)
{
    if (!hex || !out || strlen(hex) != bytes * 2U) return 0U;
    for (uint32_t i = 0U; i < bytes; i++) {
        int8_t hi = hex_nibble(hex[i * 2U]);
        int8_t lo = hex_nibble(hex[i * 2U + 1U]);
        if (hi < 0 || lo < 0) return 0U;
        out[i] = (uint8_t)(((uint8_t)hi << 4U) | (uint8_t)lo);
    }
    return 1U;
}

static uint8_t manifest_signature_valid(const app_ota_manifest_t *m)
{
    uint8_t public_key[32];
    char message[192];
    size_t length = 0U;
    if (!m || !hex_to_bytes(OTA_SIGNING_PUBLIC_KEY_HEX, public_key, sizeof(public_key))) return 0U;
    if (!ota_signature_build_message(message, sizeof(message), &length,
                                     OTA_DEVICE_MODEL, m->target_version,
                                     m->image_size, m->sha256)) return 0U;
    return (crypto_ed25519_check(m->signature_ed25519, public_key,
                                 (const uint8_t *)message, length) == 0) ? 1U : 0U;
}

static uint8_t flash_wait_ready(void)
{
    uint32_t loops = BOOT_FLASH_WAIT_LOOPS;

    while (FLASH->SR & FLASH_SR_BSY) {
        iwdg_refresh();
        if (loops-- == 0U) {
            return 0U;
        }
    }

    return (FLASH->SR & (FLASH_SR_OPERR |
                         FLASH_SR_WRPERR |
                         FLASH_SR_PGAERR |
                         FLASH_SR_PGPERR |
                         FLASH_SR_PGSERR)) ? 0U : 1U;
}

static void flash_unlock(void)
{
    if (FLASH->CR & FLASH_CR_LOCK) {
        FLASH->KEYR = FLASH_KEY1;
        FLASH->KEYR = FLASH_KEY2;
    }
}

static void flash_lock(void)
{
    FLASH->CR |= FLASH_CR_LOCK;
}

static uint8_t flash_sector_from_addr(uint32_t addr, uint32_t *sector)
{
    if (!sector || addr < BOOT_APP_ADDR || addr >= BOOT_FLASH_END_ADDR) {
        return 0U;
    }

    if (addr < 0x0800C000UL) {
        *sector = 2U;
    } else if (addr < 0x08010000UL) {
        *sector = 3U;
    } else if (addr < 0x08020000UL) {
        *sector = 4U;
    } else if (addr < 0x08040000UL) {
        *sector = 5U;
    } else if (addr < 0x08060000UL) {
        *sector = 6U;
    } else {
        *sector = 7U;
    }

    return 1U;
}

static uint8_t flash_erase_sector(uint32_t sector)
{
    if (sector < 2U || sector > 7U) {
        return 0U;
    }

    FLASH->SR = FLASH_SR_CLEAR_MASK;
    FLASH->CR &= ~(FLASH_CR_SNB | FLASH_CR_PSIZE_MASK);
    FLASH->CR |= FLASH_CR_SER | (sector << FLASH_CR_SNB_POS);
    FLASH->CR |= FLASH_CR_STRT;

    if (!flash_wait_ready()) {
        FLASH->CR &= ~FLASH_CR_SER;
        return 0U;
    }

    FLASH->CR &= ~FLASH_CR_SER;
    return 1U;
}

static uint8_t flash_program_byte(uint32_t addr, uint8_t value)
{
    FLASH->SR = FLASH_SR_CLEAR_MASK;
    FLASH->CR &= ~FLASH_CR_PSIZE_MASK;
    FLASH->CR |= FLASH_CR_PG;

    *(volatile uint8_t *)addr = value;

    if (!flash_wait_ready()) {
        FLASH->CR &= ~FLASH_CR_PG;
        return 0U;
    }

    FLASH->CR &= ~FLASH_CR_PG;
    return (*(volatile uint8_t *)addr == value) ? 1U : 0U;
}

static uint8_t flash_install_from_w25q(const app_ota_manifest_t *m)
{
    uint32_t first_sector;
    uint32_t last_sector;
    uint32_t end_addr;
    uint32_t written = 0U;

    if (!m ||
        m->app_addr != BOOT_APP_ADDR ||
        m->image_size == 0U ||
        m->image_size > APP_OTA_APPLICATION_SIZE ||
        (m->app_addr + m->image_size) > BOOT_FLASH_END_ADDR) {
        return 0U;
    }

    end_addr = m->app_addr + m->image_size - 1U;
    if (!flash_sector_from_addr(m->app_addr, &first_sector) ||
        !flash_sector_from_addr(end_addr, &last_sector)) {
        return 0U;
    }

    flash_unlock();

    for (uint32_t sector = first_sector; sector <= last_sector; sector++) {
        iwdg_refresh();
        if (!flash_erase_sector(sector)) {
            flash_lock();
            return 0U;
        }
    }

    while (written < m->image_size) {
        uint32_t chunk = m->image_size - written;
        if (chunk > sizeof(s_buf)) {
            chunk = sizeof(s_buf);
        }

        iwdg_refresh();
        if (!w25q_read(m->image_addr + written, s_buf, chunk)) {
            flash_lock();
            return 0U;
        }

        for (uint32_t i = 0U; i < chunk; i++) {
            if (!flash_program_byte(m->app_addr + written + i, s_buf[i])) {
                flash_lock();
                return 0U;
            }
        }

        written += chunk;
    }

    flash_lock();
    return 1U;
}

static uint8_t flash_restore_backup(const app_ota_manifest_t *m)
{
    uint8_t hash[32];
    uint32_t done = 0U;
    if (!m || !sha256_w25q(APP_OTA_BACKUP_W25Q_ADDR, APP_OTA_SLOT_SIZE, hash) ||
        memcmp(hash, m->rollback_sha256, sizeof(hash)) != 0) return 0U;

    flash_unlock();
    for (uint32_t sector = 2U; sector <= 7U; sector++) {
        iwdg_refresh();
        if (!flash_erase_sector(sector)) { flash_lock(); return 0U; }
    }
    while (done < APP_OTA_SLOT_SIZE) {
        uint32_t chunk = APP_OTA_SLOT_SIZE - done;
        if (chunk > sizeof(s_buf)) chunk = sizeof(s_buf);
        iwdg_refresh();
        if (!w25q_read(APP_OTA_BACKUP_W25Q_ADDR + done, s_buf, chunk)) {
            flash_lock(); return 0U;
        }
        for (uint32_t i = 0U; i < chunk; i++) {
            if (!flash_program_byte(APP_OTA_APPLICATION_ADDR + done + i, s_buf[i])) {
                flash_lock(); return 0U;
            }
        }
        done += chunk;
    }
    flash_lock();
    return sha256_internal_flash(APP_OTA_APPLICATION_ADDR, APP_OTA_SLOT_SIZE, hash) &&
           memcmp(hash, m->rollback_sha256, sizeof(hash)) == 0 ? 1U : 0U;
}

static uint32_t trial_attempt_count(uint32_t word)
{
    return (uint32_t)__builtin_popcount(~word);
}

void boot_main(void)
{
    app_ota_manifest_t manifest;
    uint8_t staged_hash[32];
    uint8_t flash_hash[32];

    led_init();
    boot_uart_init();
    boot_uart_puts("\r\n[BL] boot\r\n");
    w25q_init_gpio_spi();

    if (w25q_read_jedec() != 0U && manifest_load(&manifest)) {
        if (manifest.install_result == APP_OTA_RESULT_INSTALLED) {
            if (manifest.rollback_requested == APP_OTA_ROLLBACK_REQUESTED) {
                boot_uart_puts("[BL] manual rollback requested\r\n");
                if (!manifest.backup_valid || !flash_restore_backup(&manifest) ||
                    !app_is_valid(BOOT_APP_ADDR) ||
                    !manifest_write_result(APP_OTA_RESULT_ROLLED_BACK, APP_OTA_ERR_MANUAL_ROLLBACK)) {
                    boot_uart_puts("[BL][ERR] manual rollback failed\r\n");
                    led_error_loop();
                }
            } else if (manifest.trial_confirmed == APP_OTA_TRIAL_CONFIRMED) {
                boot_uart_puts("[BL] confirmed image retained\r\n");
            } else {
                uint32_t attempts = trial_attempt_count(manifest.trial_attempts);
                if (attempts >= APP_OTA_MAX_TRIAL_BOOTS) {
                    boot_uart_puts("[BL] trial failed; restoring backup\r\n");
                    if (!manifest.backup_valid || !flash_restore_backup(&manifest) ||
                        !app_is_valid(BOOT_APP_ADDR) ||
                        !manifest_write_result(APP_OTA_RESULT_ROLLED_BACK, APP_OTA_ERR_TRIAL_EXHAUSTED)) {
                        boot_uart_puts("[BL][ERR] automatic rollback failed\r\n");
                        led_error_loop();
                    }
                } else {
                    uint32_t attempts_word = manifest.trial_attempts & ~(1UL << attempts);
                    if (!manifest_write_word(offsetof(app_ota_manifest_t, trial_attempts), attempts_word)) {
                        boot_uart_puts("[BL][ERR] cannot record trial boot\r\n");
                        led_error_loop();
                    }
                }
            }
        } else if (manifest.install_result == APP_OTA_RESULT_NONE) {
            boot_uart_puts("[BL] OTA install pending\r\n");
            if (!sha256_w25q(manifest.image_addr, manifest.image_size, staged_hash) ||
                memcmp(staged_hash, manifest.sha256, sizeof(staged_hash)) != 0) {
                boot_uart_puts("[BL][ERR] staged hash mismatch\r\n");
                (void)manifest_write_result(APP_OTA_RESULT_FAILED, APP_OTA_ERR_STAGED_HASH);
            } else if (!manifest_signature_valid(&manifest)) {
                boot_uart_puts("[BL][ERR] Ed25519 signature verification failed\r\n");
                (void)manifest_write_result(APP_OTA_RESULT_FAILED, APP_OTA_ERR_SIGNATURE_INVALID);
            } else {
                if (manifest.backup_valid != APP_OTA_BACKUP_VALID) {
                    if (!copy_internal_to_backup(&manifest) ||
                        !manifest_write_word(offsetof(app_ota_manifest_t, backup_valid), APP_OTA_BACKUP_VALID)) {
                        boot_uart_puts("[BL][ERR] original image backup failed\r\n");
                        (void)manifest_write_result(APP_OTA_RESULT_FAILED, APP_OTA_ERR_BACKUP);
                    } else {
                        manifest.backup_valid = APP_OTA_BACKUP_VALID;
                    }
                }
                if (manifest.backup_valid == APP_OTA_BACKUP_VALID) {
                    if (flash_install_from_w25q(&manifest) &&
                        sha256_internal_flash(manifest.app_addr, manifest.image_size, flash_hash) &&
                        memcmp(flash_hash, manifest.sha256, sizeof(flash_hash)) == 0 &&
                        app_is_valid(BOOT_APP_ADDR)) {
                        boot_uart_puts("[BL] OTA installed; starting trial\r\n");
                        if (!manifest_write_result(APP_OTA_RESULT_INSTALLED, APP_OTA_ERR_NONE)) led_error_loop();
                        NVIC_SystemReset();
                    } else {
                        boot_uart_puts("[BL][ERR] install/verify failed; restoring\r\n");
                        if (!flash_restore_backup(&manifest) || !app_is_valid(BOOT_APP_ADDR)) led_error_loop();
                        (void)manifest_write_result(APP_OTA_RESULT_FAILED, APP_OTA_ERR_FLASH_VERIFY);
                    }
                }
            }
        }
    }

    if (app_is_valid(BOOT_APP_ADDR)) {
        boot_uart_puts("[BL] jump app\r\n");
        jump_to_app();
    }

    boot_uart_puts("[BL][ERR] no valid app\r\n");
    led_error_loop();
}
